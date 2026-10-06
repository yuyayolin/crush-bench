#include "gpu7_renderer.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* kTag = "AuroraGpu7";

// ---------------------------------------------------------------------------
// 通用工具
// ---------------------------------------------------------------------------

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string fixed(double v, int digits)
{
    if (!(v == v) || v > 1.0e15 || v < -1.0e15) {
        v = 0.0;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return std::string(buf);
}

// 单个错误串的字节上限: 着色器编译/链接失败的 info log 会完整保留到这里
// (旧实现走 sanitize() 截断到 240 字节, 界面只能看到半条驱动的报错 —— 真机上
//  正是这个截断让 "V_vertex shader outputs / unused in constructor" 变成残句)
const size_t kMaxErrorBytes = 6000;

// JSON 字符串里安全的文本: cap==0 表示不截断, 只把 " \ 与控制字符换成安全字符
std::string jsonSafe(const std::string& s, size_t cap = 0)
{
    const size_t limit = (cap == 0 || cap > s.size()) ? s.size() : cap;
    std::string out;
    out.reserve(limit + 16);
    for (size_t i = 0; i < limit; ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            out.push_back('\'');
        } else if (c == '\n' || c == '\r' || c == '\t') {
            out.push_back(' ');
        } else if ((unsigned char)c < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    if (limit < s.size()) {
        out += "...(truncated)";
    }
    return out;
}

std::string gpu7Error;

// hilog 单条消息有长度上限, 长文本(驱动 info log)分块写, 保证 hdc hilog 里也能看全
void logChunks(const char* what, const std::string& text)
{
    const size_t kChunk = 400;
    size_t off = 0;
    int idx = 0;
    do {
        const size_t n = (text.size() - off) < kChunk ? (text.size() - off) : kChunk;
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "%{public}s[%{public}d]: %{public}s",
                     what, idx, text.substr(off, n).c_str());
        off += n;
        ++idx;
    } while (off < text.size() && idx < 16);
}

// 同一项负载里可能有多个程序失败(Face Tracking 一次要 4 个程序、Horizon 要 5 个):
// 旧实现只保留最后一个失败串, 前面失败的程序会被完全掩盖(真机上无法判断到底哪个
// 程序挂了)。这里把所有不重复的失败串拼起来一起报给界面。
void appendGpu7Error(const std::string& msg)
{
    if (msg.empty()) {
        return;
    }
    if (gpu7Error.empty()) {
        gpu7Error = msg;
        return;
    }
    if (gpu7Error.find(msg) != std::string::npos) {
        return;
    }
    const std::string sep = " | ";
    if (gpu7Error.size() + sep.size() >= kMaxErrorBytes) {
        return;
    }
    const size_t room = kMaxErrorBytes - gpu7Error.size() - sep.size();
    gpu7Error += sep;
    if (msg.size() <= room) {
        gpu7Error += msg;
        return;
    }
    // 长度不够时保留开头(程序名 + info log 的头几行通常已经够定位), 并明确标注被截断
    const size_t keep = room > 16 ? room - 16 : room;
    gpu7Error += msg.substr(0, keep);
    gpu7Error += "...(omitted)";
}

std::string fail(const std::string& msg)
{
    gpu7Error = msg;
    logChunks("error", msg);
    return "{\"ok\":false,\"error\":\"" + jsonSafe(msg, kMaxErrorBytes) + "\"}";
}

// JSON 化的运行结果
//   score: 按官方公开的 k 换算出来的单项分(0 = 该项未计分, 原因见 basis)
//   basis: 单位换算与系数来源说明(官方单位 / conv / 拟合样本 / 假设)
struct FrameStats {
    bool ok = false;
    std::string error;
    double ms = 0.0;
    double metric = 0.0;
    int metricDigits = 1;
    const char* unit = "Mpx/s";
    double fps = 0.0;
    double score = 0.0;
    std::string basis;
    // ---- 「跑满判据」的原始取证(2026-10 追加; 旁路, 不进 metric / k / conv / 计分) ----
    //  测量窗口内每一帧的墙钟被切成两段, 两段之和恒等于 st.ms:
    //    cpuIssueMs = 上一帧 glFinish 返回之后 -> 本帧 glFinish 之前的墙钟
    //                 (= 我们在 CPU 侧把这一帧的 GL 命令递出去的时间, 含驱动队列满时的背压);
    //    gpuDrainMs = 本帧 glFinish 自己花掉的时间(= 等 GPU 把队列排干)。
    //  为什么要切: GPU 侧此前一个判据都没有, 于是"这一项到底把 GPU 跑满了没有"完全无法回答。
    //  切法不改任何负载的算法 / 尺寸 / 帧数 / 计分 —— 每帧只多了两次 steady_clock 读(约 40 ns)。
    int framesMeasured = 0;
    double cpuIssueMs = 0.0;
    double gpuDrainMs = 0.0;
};

std::string statsJson(const FrameStats& st)
{
    if (!st.ok) {
        return fail(st.error.empty() ? std::string("run failed") : st.error);
    }
    char scoreBuf[64];
    snprintf(scoreBuf, sizeof(scoreBuf), "%.1f", st.score);
    return "{\"ok\":true,\"ms\":" + fixed(st.ms, 1) +
        ",\"metric\":\"" + fixed(st.metric, st.metricDigits) +
        "\",\"unit\":\"" + std::string(st.unit) +
        "\",\"fps\":" + fixed(st.fps, 1) +
        ",\"score\":" + std::string(scoreBuf) +
        ",\"basis\":\"" + jsonSafe(st.basis) + "\"}";
}

// ---------------------------------------------------------------------------
// EGL 离屏上下文(懒加载, 静态标志保证只初始化一次)
// ---------------------------------------------------------------------------

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLConfig g_config = nullptr;
EGLSurface g_pbuffer = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
bool g_ready = false;
bool g_floatColorSupported = false;

bool extensionPresent(const char* needle)
{
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    if (ext == nullptr || needle == nullptr) {
        return false;
    }
    return std::strstr(ext, needle) != nullptr;
}

std::string prepareInternal()
{
    if (g_ready) {
        return "";
    }
    if (g_display == EGL_NO_DISPLAY) {
        g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_display == EGL_NO_DISPLAY) {
            return "eglGetDisplay failed";
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(g_display, &major, &minor) != EGL_TRUE) {
            g_display = EGL_NO_DISPLAY;
            return "eglInitialize failed";
        }
    }
    if (g_config == nullptr) {
        const EGLint cfgAttribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE
        };
        EGLint numConfig = 0;
        if (eglChooseConfig(g_display, cfgAttribs, &g_config, 1, &numConfig) != EGL_TRUE || numConfig < 1) {
            return "eglChooseConfig failed";
        }
    }
    if (g_context == EGL_NO_CONTEXT) {
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctxAttribs);
        if (g_context == EGL_NO_CONTEXT) {
            return "eglCreateContext(ES3) failed";
        }
    }
    if (g_pbuffer == EGL_NO_SURFACE) {
        const EGLint pbAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        g_pbuffer = eglCreatePbufferSurface(g_display, g_config, pbAttribs);
        if (g_pbuffer == EGL_NO_SURFACE) {
            return "eglCreatePbufferSurface failed";
        }
    }
    if (eglMakeCurrent(g_display, g_pbuffer, g_pbuffer, g_context) != EGL_TRUE) {
        return "eglMakeCurrent failed";
    }
    eglSwapInterval(g_display, 0);
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (ver == nullptr) {
        return "no GL context";
    }
    if (std::strstr(ver, "OpenGL ES ") == nullptr) {
        return std::string("not an OpenGL ES context: ") + ver;
    }
    g_floatColorSupported = extensionPresent("GL_EXT_color_buffer_float");
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "gpu7 ready: %{public}s floatColor=%{public}d",
                 ver, (int)(g_floatColorSupported ? 1 : 0));
    g_ready = true;
    return "";
}

// ---------------------------------------------------------------------------
// 着色器编译 / 程序链接
// ---------------------------------------------------------------------------

// 完整的 info log: 不再固定截断到 1023 字节。真机上驱动返回的链接错误(例如
// "0(12): L002: Undeclared variable ...")经常整条就在 1KB 上下, 截断后只剩半句,
// 无法定位。这里按 GL_INFO_LOG_LENGTH 真实长度取, 上限 8KB 防异常驱动。
std::string fullInfoLog(GLuint obj, bool isProgram)
{
    GLint len = 0;
    if (isProgram) {
        glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);
    } else {
        glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
    }
    if (len <= 1) {
        return std::string("(no info log)");
    }
    if (len > 8192) {
        len = 8192;
    }
    std::vector<char> buf((size_t)len + 1, '\0');
    GLsizei written = 0;
    if (isProgram) {
        glGetProgramInfoLog(obj, (GLsizei)len, &written, buf.data());
    } else {
        glGetShaderInfoLog(obj, (GLsizei)len, &written, buf.data());
    }
    if (written <= 0) {
        return std::string("(empty info log)");
    }
    if ((size_t)written > (size_t)len) {
        written = (GLsizei)len;
    }
    return std::string(buf.data(), (size_t)written);
}

// 编译 + 链接; label 只用于诊断(错误串里带上程序名 + 完整 info log, 便于真机定位)
GLuint makeProgram(const char* vsSrc, const char* fsSrc, const char* label, std::string* err)
{
    const std::string tag = std::string("program '") + (label != nullptr ? label : "?") + "'";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSrc, nullptr);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        const std::string log = fullInfoLog(vs, false);
        glDeleteShader(vs);
        if (err) {
            *err = tag + " vertex shader compile failed: " + log;
        }
        return 0;
    }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSrc, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        const std::string log = fullInfoLog(fs, false);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (err) {
            *err = tag + " fragment shader compile failed: " + log;
        }
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        const std::string log = fullInfoLog(prog, true);
        glDeleteProgram(prog);
        if (err) {
            *err = tag + " program link failed: " + log;
        }
        return 0;
    }
    return prog;
}

// glGetUniformLocation 返回 -1(被优化掉)时不要调用 glUniform*
inline void setF1(GLuint p, const char* n, float v)
{
    GLint l = glGetUniformLocation(p, n);
    if (l >= 0) {
        glUniform1f(l, v);
    }
}

inline void setI1(GLuint p, const char* n, int v)
{
    GLint l = glGetUniformLocation(p, n);
    if (l >= 0) {
        glUniform1i(l, v);
    }
}

inline void set2F(GLuint p, const char* n, float a, float b)
{
    GLint l = glGetUniformLocation(p, n);
    if (l >= 0) {
        glUniform2f(l, a, b);
    }
}

// 目前没有 vec3 uniform, 保留通用 setter 备用(显式标注, 避免 -Wunused-function)
[[maybe_unused]] inline void set3F(GLuint p, const char* n, float a, float b, float c)
{
    GLint l = glGetUniformLocation(p, n);
    if (l >= 0) {
        glUniform3f(l, a, b, c);
    }
}

// 覆盖全屏的大三角形, 位置/UV 复用同一个 vec2 属性(location 0)
const float kFullscreenVerts[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};

const char* kQuadVS = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// 全屏三角形 VAO/VBO(所有全屏 pass 共用)
GLuint g_quadVao = 0;
GLuint g_quadVbo = 0;

// 点云 VAO(共用 aPos 属性, 真实坐标在顶点着色器里用 gl_VertexID 计算)
GLuint g_pointVao = 0;

void ensureQuad()
{
    if (g_quadVao != 0) {
        return;
    }
    glGenBuffers(1, &g_quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_quadVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(kFullscreenVerts), kFullscreenVerts, GL_STATIC_DRAW);
    glGenVertexArrays(1, &g_quadVao);
    glBindVertexArray(g_quadVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_quadVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    glGenVertexArrays(1, &g_pointVao);
    glBindVertexArray(g_pointVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_quadVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

void drawFullscreen()
{
    glBindVertexArray(g_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// FBO 封装: 一个纹理颜色附件
//
// 浮点颜色附件在真机上不一定可用, 因此内部格式按"精度损失最小"的顺序逐级降级:
//     GL_RGBA32F -> GL_RGBA16F -> GL_RGBA8
//     GL_RGBA16F -> GL_RGBA8
//     GL_RGBA8   -> GL_RGBA8
// 每一级都真正建纹理 + FBO, 并用 glCheckFramebufferStatus 实测验证; 只有
// 完整(GL_FRAMEBUFFER_COMPLETE 且 glTexImage2D 无错误)才接受。全部失败时把
// glCheckFramebufferStatus 的真实返回值(十六进制 + 名称)、尺寸、内部格式写进
// g_targetError, 调用方把它拼进错误串(便于下次真机定位)。
// GL_EXT_color_buffer_float 在 prepareInternal() 里查询并记录, 只用于日志;
// 这里不据此跳过浮点尝试 —— 逐级尝试本身就能覆盖"扩展串不可靠"的设备。
// ---------------------------------------------------------------------------

struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    GLenum internal = 0;
    bool floatFmt = false;
};

void destroyTarget(Target& t);

// 最近一次 createTarget 失败的详细原因(含 glCheckFramebufferStatus 的真实返回值)
std::string g_targetError;

std::string hexCode(unsigned v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%04X", v & 0xFFFFu);
    return std::string(buf);
}

const char* formatName(GLenum f)
{
    if (f == GL_RGBA32F) {
        return "RGBA32F";
    }
    if (f == GL_RGBA16F) {
        return "RGBA16F";
    }
    if (f == GL_RGBA8) {
        return "RGBA8";
    }
    return "unknown-format";
}

// glCheckFramebufferStatus 的返回值 -> 名称(数值取自 GLES3 规范)
std::string statusName(unsigned st)
{
    switch (st) {
        case 0x8CD5u: return "GL_FRAMEBUFFER_COMPLETE(0x8CD5)";
        case 0x8CD6u: return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT(0x8CD6)";
        case 0x8CD7u: return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT(0x8CD7)";
        case 0x8CD9u: return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS(0x8CD9)";
        case 0x8CDAu: return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE(0x8CDA)";
        case 0x8CDDu: return "GL_FRAMEBUFFER_UNSUPPORTED(0x8CDD)";
        case 0x0000u: return "0x0000(no framebuffer bound / GL error)";
        default: return hexCode(st);
    }
}

void clearGlErrors()
{
    for (int i = 0; i < 8; ++i) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

// 用指定内部格式尝试一次
bool createTargetWithFormat(Target& t, int w, int h, GLenum internal, std::string* why)
{
    // 客户端 format/type 必须与 sized internal format 匹配(GLES3 表 3.2):
    // RGBA16F 只接受 GL_HALF_FLOAT / GL_FLOAT, RGBA32F 只接受 GL_FLOAT,
    // RGBA8 用 GL_UNSIGNED_BYTE。原来对所有格式一律传 GL_UNSIGNED_BYTE, 在严格
    // 实现上会 INVALID_OPERATION, 纹理拿不到存储, FBO 必然不完整 —— 这是真机上
    // 浮点目标 "fbo create failed" 的一类原因, 因此这里按内部格式选择 type。
    GLenum type = GL_UNSIGNED_BYTE;
    if (internal == GL_RGBA16F) {
        type = GL_HALF_FLOAT;
    } else if (internal == GL_RGBA32F) {
        type = GL_FLOAT;
    }

    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    clearGlErrors();
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, GL_RGBA, type, nullptr);
    const GLenum texErr = glGetError();

    // 32F 在 ES3 里不是 texture-filterable(需要 OES_texture_float_linear), 用
    // GL_LINEAR 会让纹理不完整/采样值未定义; 16F 与 8 位在 ES3 核心内可线性过滤。
    const GLint filter = (internal == GL_RGBA32F) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (st == 0x8CD5u && texErr == GL_NO_ERROR) {
        t.w = w;
        t.h = h;
        t.internal = internal;
        t.floatFmt = (internal == GL_RGBA16F || internal == GL_RGBA32F);
        if (why != nullptr) {
            why->clear();
        }
        return true;
    }

    if (why != nullptr) {
        *why = std::string(formatName(internal)) + " " + std::to_string(w) + "x" + std::to_string(h) +
               " glCheckFramebufferStatus=" + statusName((unsigned)st) +
               " glTexImage2D_error=" + hexCode((unsigned)texErr);
    }
    destroyTarget(t);
    return false;
}

bool createTarget(Target& t, int w, int h, GLenum internalFormat)
{
    g_targetError.clear();
    if (w <= 0 || h <= 0) {
        g_targetError = "invalid target size " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    GLenum chain[3] = {GL_RGBA8, GL_RGBA8, GL_RGBA8};
    int n = 0;
    if (internalFormat == GL_RGBA32F) {
        chain[0] = GL_RGBA32F; chain[1] = GL_RGBA16F; chain[2] = GL_RGBA8; n = 3;
    } else if (internalFormat == GL_RGBA16F) {
        chain[0] = GL_RGBA16F; chain[1] = GL_RGBA8; n = 2;
    } else {
        chain[0] = GL_RGBA8; n = 1;
    }

    std::string why;
    for (int i = 0; i < n; ++i) {
        if (i > 0 && chain[i] == chain[i - 1]) {
            continue;
        }
        if (createTargetWithFormat(t, w, h, chain[i], &why)) {
            if (i > 0) {
                OH_LOG_Print(LOG_APP, LOG_WARN, 0x1234, kTag,
                             "target %{public}dx%{public}d degraded to %{public}s (%{public}s)",
                             w, h, formatName(chain[i]), why.c_str());
            }
            return true;
        }
    }
    g_targetError = "fbo create failed [" + why + "] floatColorExt=" +
                    (g_floatColorSupported ? "yes" : "no");
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "%{public}s", g_targetError.c_str());
    return false;
}

// 统一的初始化失败描述: FBO 失败时带上 g_targetError(含真实的状态码/尺寸/格式),
// 着色器/程序失败时带上 GL 的 info log(gpu7Error 由 getProg 写入)。
std::string initError(bool targetsOk, const char* what)
{
    if (!targetsOk) {
        return std::string(what) + ": fbo create failed: " + g_targetError;
    }
    if (!gpu7Error.empty()) {
        return std::string(what) + ": " + gpu7Error;
    }
    return std::string(what) + ": shader program unavailable";
}

void destroyTarget(Target& t)
{
    if (t.fbo != 0) {
        glDeleteFramebuffers(1, &t.fbo);
        t.fbo = 0;
    }
    if (t.tex != 0) {
        glDeleteTextures(1, &t.tex);
        t.tex = 0;
    }
    t.w = 0;
    t.h = 0;
}

void bindTarget(const Target& t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, t.w, t.h);
}

void bindTex(GLuint unit, GLuint tex)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

// 统一关闭深度/混合等状态, 每项负载开头调用一次
void resetGlState()
{
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlendFunc(GL_ONE, GL_ONE);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

// 计时结束后最多一次 glReadPixels, 用于校验输出缓冲确实被写入
// (同时避免驱动把整条依赖链优化掉)
bool checksumTex(GLuint fbo, int w, int h, float* out)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }
    int cx = w / 2 - 1;
    int cy = h / 2 - 1;
    if (cx < 0) {
        cx = 0;
    }
    if (cy < 0) {
        cy = 0;
    }
    unsigned char px[16] = {0};
    glReadPixels(cx, cy, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
    float sum = 0.0f;
    for (int i = 0; i < 16; ++i) {
        sum += (float)px[i];
    }
    if (out != nullptr) {
        *out = sum;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 程序缓存(按 id 懒加载, 每项负载结束时统一释放)
// ---------------------------------------------------------------------------

struct ProgEntry {
    GLuint prog = 0;
    bool tried = false;
};

enum ProgId {
    P_BLUR_H = 0,
    P_BLUR_V,
    P_SCENE,
    P_FT_ACC,
    P_FT_UPDATE,
    P_FM_HIST,
    P_FM_MATCH,
    P_FM_SUMMARY,
    P_FLUID_ADV,
    P_FLUID_DIVERG,
    P_FLUID_JACOBI,
    P_FLUID_GRADSUB,
    P_FLUID_DISPLAY,
    P_HOUGH_EDGE,
    P_HOUGH_VOTE_A,
    P_HOUGH_VOTE_B,
    P_HOUGH_DRAW,
    P_PART_UPDATE,
    P_PART_DRAW,
    P_PT_TRACE,
    P_PT_DRAW,
    P_PF_LUT,
    P_PF_SAT,
    P_PF_SHARPEN,
    P_PF_VIGNETTE,
    P_PF_SEPIA,
    P_RAW_BAYER,
    P_RAW_DEMOSAIC,
    P_RAW_COLOR,
    P_SR_CONV,
    P_VF_DENOISE,
    P_VF_BLEND,
    P_VF_SHARP,
    P_VF_GRADE,
    P_PROG_COUNT
};

ProgEntry g_progs[P_PROG_COUNT];

// 每个 ProgId 的 (顶点着色器, 片元着色器, 名称) 登记表定义在全部着色器源码之后
// (见"程序登记表"一节), 这里只做前置声明。
GLuint getProg(ProgId id);

void releasePrograms()
{
    for (int i = 0; i < (int)P_PROG_COUNT; ++i) {
        if (g_progs[i].prog != 0) {
            glDeleteProgram(g_progs[i].prog);
        }
        g_progs[i].prog = 0;
        g_progs[i].tried = false;
    }
}

// 每项负载开头调用: 保证上下文可用 + 状态干净
bool beginLoad(const char* name)
{
    if (!g_ready) {
        std::string err = prepareInternal();
        if (!err.empty()) {
            gpu7Error = err;
            return false;
        }
    }
    if (eglMakeCurrent(g_display, g_pbuffer, g_pbuffer, g_context) != EGL_TRUE) {
        gpu7Error = "eglMakeCurrent failed";
        return false;
    }
    resetGlState();
    ensureQuad();
    if (g_quadVao == 0) {
        gpu7Error = "fullscreen vao create failed";
        return false;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "run %{public}s", name);
    return true;
}

void endLoad()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    releasePrograms();
}

// 计时: 预热 3 帧 -> 测量 frames 帧 -> 每帧末 glFinish
//
//  2026-10 追加(只加不改): 把每一帧的墙钟切成"我们在 CPU 侧递交 / 等 GPU 排水"两段。
//  切点取在 glFinish() 调用前后 —— 上一帧 glFinish 返回之后到本帧 glFinish 开始之前,
//  这一段时间就是本帧的"递交 + 背压"; glFinish() 内部那一段就是"等 GPU 排水"。
//  st.ms 与 st.fps 的定义一个字都没动(它们仍由 t0/elapsed/frames 算出), 新增的两个
//  累加量只用于旁路的跑满判据。
struct Timer {
    double t0 = 0.0;
    double lastEnd = 0.0;
    double issueMs = 0.0;
    double drainMs = 0.0;
    int frames = 0;

    void start()
    {
        t0 = nowMs();
        lastEnd = t0;
        issueMs = 0.0;
        drainMs = 0.0;
        frames = 0;
    }

    void frame()
    {
        const double tIssue = nowMs();
        glFinish();
        const double tEnd = nowMs();
        if (lastEnd > 0.0) {
            issueMs += (tIssue - lastEnd);
            drainMs += (tEnd - tIssue);
        }
        lastEnd = tEnd;
        frames++;
    }

    // workPerFrame 为"每帧完成的物理工作量"(像素/粒子/光线/投票/单元/描述子对)
    void finish(FrameStats& st, double workPerFrame, const char* unit, int digits, double scaleToUnit)
    {
        double elapsed = nowMs() - t0;
        if (frames <= 0) {
            st.ok = false;
            st.error = "no frames rendered";
            return;
        }
        st.ms = elapsed;
        st.fps = 1000.0 * (double)frames / (elapsed > 0.0 ? elapsed : 1.0);
        st.metric = workPerFrame * st.fps * scaleToUnit;
        st.metricDigits = digits;
        st.unit = unit;
        st.framesMeasured = frames;
        st.cpuIssueMs = issueMs;
        st.gpuDrainMs = drainMs;
        st.ok = true;
    }
};

const int kWarmup = 3;
// ---------------------------------------------------------------------------
// 着色器源码 1/3: Background Blur / Face Tracking / Feature Matching / Fluid
// 全部为 GLSL ES 3.00 (#version 300 es)。
// ---------------------------------------------------------------------------

const char* FS_BLUR_H = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform float uRadius;
uniform float uSigma;
void main() {
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = -15; i <= 15; i++) {
        float fi = float(i);
        float w = exp(-(fi * fi) / (2.0 * uSigma * uSigma));
        sum += texture(uTex, vUV + vec2(fi * uRadius * uTexel.x, 0.0)) * w;
        wsum += w;
    }
    fragColor = sum / max(wsum, 0.0001);
}
)GLSL";

const char* FS_BLUR_V = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform float uRadius;
uniform float uSigma;
void main() {
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = -15; i <= 15; i++) {
        float fi = float(i);
        float w = exp(-(fi * fi) / (2.0 * uSigma * uSigma));
        sum += texture(uTex, vUV + vec2(0.0, fi * uRadius * uTexel.y)) * w;
        wsum += w;
    }
    fragColor = sum / max(wsum, 0.0001);
}
)GLSL";

// 程序内生成的输入纹理(噪声 + 渐变 + 色块), 作为真实的纹理输入
const char* FS_SCENE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform float uSeed;
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
void main() {
    vec2 uv = vUV;
    vec3 c = vec3(0.0);
    c += vec3(0.10, 0.22, 0.42) * (1.0 - uv.y);
    c += vec3(0.55, 0.30, 0.12) * uv.y;
    for (int k = 0; k < 8; k++) {
        float fk = float(k) + 1.0;
        float a = uv.x * 9.0 * fk + uSeed * (fk * 0.7);
        c += vec3(sin(a) * 0.035, cos(a * 1.3) * 0.035, sin(a * 0.7 + 1.1) * 0.035);
    }
    vec2 q = floor(uv * vec2(48.0, 27.0));
    float noise = hash21(q + uSeed * 0.001);
    c *= 0.70 + 0.60 * noise;
    float grid = step(0.985, fract(uv.x * 32.0)) + step(0.985, fract(uv.y * 18.0));
    c += vec3(0.06) * grid;
    c = clamp(c, 0.0, 1.0);
    fragColor = vec4(c, 1.0);
}
)GLSL";

// 所有用 glDrawArrays(GL_POINTS, 0, N) 绘制的 pass 共用的顶点着色器。
// 这些 pass 的 N 可以远大于顶点缓冲里的顶点数(例如 16384 个投票点), 越界属性的
// 值是未定义的, 读 aPos 会让点落到裁剪体之外 -> 片元不执行 -> 实际工作量与 metric
// 分子(点数)不符。因此位置只由 gl_VertexID 决定: 把第 i 个点确定性地铺在视口
// [0.01,0.99]^2 内, 每个点恰好光栅化 1 个片元(gl_PointSize = 1.0)。
//
// 【真机修复: 复跑失败项 hough_vote_b / hough_vote_a / ft_acc】gl_VertexID 只存在于
// "顶点着色器"(GLSL ES 3.00 §7.1
// 顶点着色器特殊变量: gl_Position / gl_PointSize / gl_VertexID / gl_InstanceID;
// 片元着色器只有 gl_FragCoord / gl_FrontFacing / gl_PointCoord / gl_FragDepth)。
// 旧写法让"片元着色器自己读 gl_VertexID 取索引", 在真机驱动上链接期直接失败:
//     program 'hough_vote_b' program link failed: 0(12): L002: Undeclared variable
//     'v_013'; 0(13): L002: Undeclared variable 'v_014'; ...
// 修法: 需要"点序号"的片元着色器改用下面的 kPointIndexVS —— 由顶点着色器把序号作为
// varying(vIdx)传给片元着色器, 两端同名同类型且都真正被使用(驱动会先消除未使用的
// varying, 两端消除结果不一致就会在链接期报"未声明变量")。
// kPointVS 保留给"不需要点序号"的点程序(FS_FM_SUMMARY), 它的 vUV 与片元侧的
// in vec2 vUV 一一对应, 不动已跑通的配对。
const char* kPointVS = R"GLSL(#version 300 es
out vec2 vUV;
void main() {
    float fi = float(gl_VertexID);
    float hx = fract(sin(fi * 12.9898 + 4.1) * 43758.5453);
    float hy = fract(sin(fi * 78.233 + 1.7) * 43758.5453);
    vec2 c = vec2(0.01 + 0.98 * hx, 0.01 + 0.98 * hy);
    vUV = c;
    gl_PointSize = 1.0;
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// 逐点写入且片元着色器需要"点序号"的 pass 用的顶点着色器:
// 位置/点大小与 kPointVS 完全一致(只由 gl_VertexID 决定, 每个点 1 个片元),
// 额外把点序号本身作为 varying 传给片元着色器。点图元只有一个顶点, 所以 vIdx
// 到片元就是该点的序号, 没有插值误差。
const char* kPointIndexVS = R"GLSL(#version 300 es
out float vIdx;
void main() {
    float fi = float(gl_VertexID);
    float hx = fract(sin(fi * 12.9898 + 4.1) * 43758.5453);
    float hy = fract(sin(fi * 78.233 + 1.7) * 43758.5453);
    vec2 c = vec2(0.01 + 0.98 * hx, 0.01 + 0.98 * hy);
    vIdx = fi;
    gl_PointSize = 1.0;
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// LK 累加: 累加 [gx^2, gx*gy, gy^2, err](加法混合写入 8x8 累加器)
// 【真机修复】点序号改成由 kPointIndexVS 通过 varying vIdx 传入: 片元着色器里没有
// gl_VertexID(ES 3.00 只在顶点着色器提供), 原来在这里读 gl_VertexID 会让整个程序
// 在真机链接期失败(驱动报 Undeclared variable)。vIdx 两端都声明、都使用。
const char* FS_FT_ACC = R"GLSL(#version 300 es
precision highp float;
in float vIdx;
out vec4 fragColor;
uniform sampler2D uCurr;
uniform sampler2D uPrev;
uniform vec2 uSeed;
uniform vec2 uLevelSize;
uniform vec2 uLevelTexel;
uniform vec2 uDisp;
void main() {
    float fi = vIdx;
    float j = mod(fi, uSeed.x);
    float i = floor(fi / uSeed.x);
    vec2 uv = (vec2(j, i) + 0.5) / uLevelSize;
    vec2 t = uLevelTexel;
    float tl = texture(uCurr, uv - vec2(t.x, 0.0)).r;
    float tr = texture(uCurr, uv + vec2(t.x, 0.0)).r;
    float tb = texture(uCurr, uv - vec2(0.0, t.y)).r;
    float tt = texture(uCurr, uv + vec2(0.0, t.y)).r;
    float gx = (tr - tl) * 0.5;
    float gy = (tt - tb) * 0.5;
    vec2 p = uv + uDisp;
    float pr = texture(uPrev, p + vec2(t.x, 0.0)).r;
    float pl = texture(uPrev, p - vec2(t.x, 0.0)).r;
    float pu = texture(uPrev, p + vec2(0.0, t.y)).r;
    float pd = texture(uPrev, p - vec2(0.0, t.y)).r;
    float pgx = (pr - pl) * 0.5;
    float pgy = (pu - pd) * 0.5;
    float ic = texture(uPrev, p).r;
    float err = texture(uCurr, uv).r - ic;
    // 当前帧与参考帧的梯度一起参与, 保证两次采样都真正被使用
    float gxc = (gx + pgx) * 0.5;
    float gyc = (gy + pgy) * 0.5;
    fragColor = vec4(gxc * gxc, gxc * gyc, gyc * gyc, err);
}
)GLSL";

// 64 路归约 + 求解 2x2 线性系统, 输出新的位移估计
// 【真机修复: 复跑失败项 ft_update】本 FS 原来声明 in vec2 vUV 却完全不用它。
// 该环没有别的问题(kQuadVS 的 20 多个全屏 pass 都正常), 所以按"varying 两端都必须
// 真正被使用"这条硬规则修: 让 vUV 以极小权重真正参与运算(<=5e-6, 远小于下面 ±0.05
// 的钳制, 不影响纹理采样与钳制结果), 这样顶点侧 out vec2 vUV 与片元侧 in vec2 vUV
// 在链接期一定同时存在(驱动会先做未使用 varying 消除)。
const char* FS_FT_UPDATE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uAcc;
uniform vec2 uDisp;
uniform float uIter;
void main() {
    float s0 = 0.0;
    float s1 = 0.0;
    float s2 = 0.0;
    float s3 = 0.0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            vec4 a = texelFetch(uAcc, ivec2(x, y), 0);
            s0 += a.x;
            s1 += a.y;
            s2 += a.z;
            s3 += a.w;
        }
    }
    s0 = s0 / 64.0 + 0.001;
    s1 = s1 / 64.0;
    s2 = s2 / 64.0 + 0.001;
    s3 = s3 / 64.0;
    float det = s0 * s2 - s1 * s1;
    if (abs(det) < 0.00002) {
        det = (det < 0.0) ? -0.00002 : 0.00002;
    }
    vec2 rhs = vec2(-(s2 * s1 - s1 * s3), -(s0 * s3 - s1 * s1)) / det;
    float len = length(rhs);
    if (len > 3.0) {
        rhs = rhs * (3.0 / len);
    }
    // keep vUV live on both ends: weight 1e-5 is far below the +-0.05 clamp below
    vec2 nd = uDisp + rhs * 0.6 + vec2(uIter * 0.0) + (vUV - vec2(0.5)) * 1.0e-5;
    nd = clamp(nd, vec2(-0.05), vec2(0.05));
    fragColor = vec4(nd, abs(s3), 1.0);
}
)GLSL";

// 描述子提取: 每个 fragment 输出一个描述子的 64 维梯度方向直方图(64x4096 纹理)
const char* FS_FM_HIST = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform float uFrame;
void main() {
    int cell = int(vUV.x * 64.0);
    vec2 p = texture(uTex, vec2(0.5, 0.5)).rg;
    float px = p.x * 48.0 + 0.5;
    float py = p.y * 27.0 + 0.5;
    float a = fract(px);
    float b = fract(py);
    vec2 base = (vec2(floor(px), floor(py)) + 0.5 + vec2(a, b)) / vec2(48.0, 27.0);
    int bx = cell - (cell / 8) * 8;
    int by = cell / 8;
    vec2 off = (vec2(float(bx), float(by)) - vec2(3.5)) * 8.0 * uTexel;
    vec2 c = base + off;
    float acc = 0.0;
    for (int s = 0; s < 4; s++) {
        vec2 d = vec2(0.0);
        if (s == 0) {
            d = vec2(1.0, 0.0);
        } else if (s == 1) {
            d = vec2(-1.0, 0.0);
        } else if (s == 2) {
            d = vec2(0.0, 1.0);
        } else {
            d = vec2(0.0, -1.0);
        }
        for (int k = 1; k <= 8; k++) {
            float fk = float(k);
            vec2 sp = c + d * fk * 2.0 * uTexel;
            float lc = texture(uTex, sp).g;
            float lx = texture(uTex, sp + vec2(uTexel.x * 2.0, 0.0)).g;
            float ly = texture(uTex, sp + vec2(0.0, uTexel.y * 2.0)).g;
            float ang = atan(ly - lc, lx - lc);
            float phi = (ang + 3.14159265) / 6.2831853;
            float bin = phi * 8.0 + float(bx) * 0.74 + float(by) * 0.37 + uFrame * 0.013;
            float w = 0.0;
            if (abs(bin - float(cell)) < 1.0) {
                w = 1.0 - abs(bin - float(cell));
            }
            acc += w * (0.3 + 0.7 * abs(sin(fk * 0.7 + uFrame * 0.01)));
        }
    }
    fragColor = vec4(acc, acc * 0.25, acc * 0.0625, 1.0);
}
)GLSL";

// 暴力匹配: 每个 fragment 比较一对描述子, 在 fragment 内部做归约
const char* FS_FM_MATCH = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uDesc;
uniform sampler2D uSummary;
uniform vec2 uTile;
uniform float uFrame;
void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    px.y += int(uTile.y);
    vec4 sv = texelFetch(uSummary, ivec2(0, 0), 0);
    vec4 a0 = texelFetch(uDesc, ivec2(0, px.y), 0);
    vec4 a1 = texelFetch(uDesc, ivec2(1, px.y), 0);
    vec4 a2 = texelFetch(uDesc, ivec2(2, px.y), 0);
    vec4 a3 = texelFetch(uDesc, ivec2(3, px.y), 0);
    int q = px.x + int(uTile.y);
    q = q - (q / 4096) * 4096;
    q = q + int(sv.x * 7.0 + uFrame);
    q = q - (q / 4096) * 4096;
    float dist = 0.0;
    for (int k = 0; k < 68; k++) {
        vec4 b = texelFetch(uDesc, ivec2(k, q), 0);
        dist += dot(a0, b) * 0.25;
        dist -= dot(a1, b) * 0.125;
        dist += dot(a2, b) * 0.0625;
        dist -= dot(a3, b) * 0.03125;
    }
    dist = abs(dist) * 0.015;
    float v = sv.x * 0.5 + 0.25;
    fragColor = vec4(dist * v, dist * 0.5, v * 0.5, 1.0);
}
)GLSL";

// 把匹配残差写回描述子纹理首行, 建立"上一次迭代的输出被下一次使用"的依赖
const char* FS_FM_SUMMARY = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uMatch;
void main() {
    vec2 g = gl_PointCoord - vec2(0.5);
    if (dot(g, g) > 0.25) {
        discard;
    }
    vec4 m = texture(uMatch, vec2(0.01));
    fragColor = vec4(fract(m.r * 3.0 + 0.125), m.g, m.b, 1.0);
}
)GLSL";

// ---------------- Fluid Simulation (1024x1024) ----------------

const char* FS_FLUID_ADV = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uVel;
uniform sampler2D uSrc;
uniform vec2 uTexel;
uniform float uDt;
void main() {
    vec2 v = texture(uVel, vUV).xy;
    vec2 p = vUV - v * uDt;
    vec4 a = texture(uSrc, p + vec2(uTexel.x, 0.0));
    vec4 b = texture(uSrc, p - vec2(uTexel.x, 0.0));
    vec4 c = texture(uSrc, p + vec2(0.0, uTexel.y));
    vec4 d = texture(uSrc, p - vec2(0.0, uTexel.y));
    fragColor = (a + b + c + d) * 0.25;
}
)GLSL";

const char* FS_FLUID_DIVERG = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uVel;
uniform vec2 uTexel;
void main() {
    float l = texture(uVel, vUV - vec2(uTexel.x, 0.0)).x;
    float r = texture(uVel, vUV + vec2(uTexel.x, 0.0)).x;
    float b = texture(uVel, vUV - vec2(0.0, uTexel.y)).y;
    float t = texture(uVel, vUV + vec2(0.0, uTexel.y)).y;
    float div = ((r - l) + (t - b)) * 0.5;
    fragColor = vec4(div, 0.0, 0.0, 1.0);
}
)GLSL";

const char* FS_FLUID_JACOBI = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uPres;
uniform sampler2D uDiv;
uniform vec2 uTexel;
uniform float uRbeta;
void main() {
    float l = texture(uPres, vUV - vec2(uTexel.x, 0.0)).x;
    float r = texture(uPres, vUV + vec2(uTexel.x, 0.0)).x;
    float b = texture(uPres, vUV - vec2(0.0, uTexel.y)).x;
    float t = texture(uPres, vUV + vec2(0.0, uTexel.y)).x;
    float div = texture(uDiv, vUV).x;
    fragColor = vec4((l + r + b + t + div) * uRbeta, 0.0, 0.0, 1.0);
}
)GLSL";

const char* FS_FLUID_GRADSUB = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uPres;
uniform sampler2D uVel;
uniform vec2 uTexel;
uniform float uFrame;
void main() {
    float l = texture(uPres, vUV - vec2(uTexel.x, 0.0)).x;
    float r = texture(uPres, vUV + vec2(uTexel.x, 0.0)).x;
    float b = texture(uPres, vUV - vec2(0.0, uTexel.y)).x;
    float t = texture(uPres, vUV + vec2(0.0, uTexel.y)).x;
    vec2 vel = texture(uVel, vUV).xy;
    vec2 grad = vec2(r - l, t - b) * 0.5;
    vec2 nv = (vel - grad) * 0.995;
    nv.x += sin(uFrame * 0.013 + vUV.y * 9.0) * 0.02;
    fragColor = vec4(nv, 0.0, 1.0);
}
)GLSL";

const char* FS_FLUID_DISPLAY = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uVel;
void main() {
    vec2 v = texture(uVel, vUV).xy;
    float sp = length(v);
    vec3 c = vec3(0.02, 0.04, 0.09);
    c += vec3(0.15, 0.45, 0.95) * min(sp * 3.0, 1.0);
    c += vec3(0.95, 0.75, 0.30) * min(max(sp - 0.35, 0.0) * 2.0, 1.0);
    fragColor = vec4(c, 1.0);
}
)GLSL";
// ---------------------------------------------------------------------------
// 着色器源码 2/3: Horizon Detection / Particle Physics / Path Tracer
// ---------------------------------------------------------------------------

const char* FS_HOUGH_EDGE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;
void main() {
    vec3 tl = texture(uTex, vUV + vec2(-uTexel.x, -uTexel.y)).rgb;
    vec3 tt = texture(uTex, vUV + vec2(0.0, -uTexel.y)).rgb;
    vec3 tr = texture(uTex, vUV + vec2(uTexel.x, -uTexel.y)).rgb;
    vec3 ml = texture(uTex, vUV + vec2(-uTexel.x, 0.0)).rgb;
    vec3 mr = texture(uTex, vUV + vec2(uTexel.x, 0.0)).rgb;
    vec3 bl = texture(uTex, vUV + vec2(-uTexel.x, uTexel.y)).rgb;
    vec3 bb = texture(uTex, vUV + vec2(0.0, uTexel.y)).rgb;
    vec3 br = texture(uTex, vUV + vec2(uTexel.x, uTexel.y)).rgb;
    vec3 l = tl + 2.0 * ml + bl;
    vec3 r = tr + 2.0 * mr + br;
    vec3 t = tl + 2.0 * tt + tr;
    vec3 b = bl + 2.0 * bb + br;
    vec3 gx = (r - l) * 0.25;
    vec3 gy = (b - t) * 0.25;
    float mag = length(gx) + length(gy);
    float ang = atan(gy.r, gx.r);
    float e = smoothstep(0.35, 0.95, mag);
    fragColor = vec4(e, ang, mag, 1.0);
}
)GLSL";

// 每个点投一个角度: (x, y) 位置就是 (角度, rho) 的投票目标
// 【真机修复】点序号由 kPointIndexVS 的 varying vIdx 传入(片元着色器没有 gl_VertexID);
// 投票桶坐标改由 gl_FragCoord 取(点大小 1.0, gl_FragCoord.xy 的整数部分就是该点落在
// 的累加器像素, 与原来 vUV*1024 取整完全等价), 于是本 FS 的 varying 只剩 vIdx,
// 两端一一对应且都被使用。
const char* FS_HOUGH_VOTE_A = R"GLSL(#version 300 es
precision highp float;
in float vIdx;
out vec4 fragColor;
uniform sampler2D uEdges;
uniform float uScale;
uniform float uAngleScale;
float hash1(float n) {
    return fract(sin(n * 12.9898) * 43758.5453);
}
void main() {
    float fi = vIdx;
    float u = hash1(fi + 1.5);
    float v = hash1(fi * 1.37 + 7.1);
    float w = hash1(fi * 2.71 + 3.3);
    vec4 e = texture(uEdges, vec2(u, v));
    float ang = w * 3.1415926;
    float rho = (u * cos(ang) + v * sin(ang)) * 0.5 + 0.5;
    float target = floor(rho * uScale);
    float a = floor(ang * uAngleScale);
    float ci = floor(gl_FragCoord.x);
    float ri = floor(gl_FragCoord.y);
    float hit = 0.0;
    if (abs(ci - a) < 0.5 && abs(ri - target) < 0.5) {
        hit = e.r;
    }
    fragColor = vec4(hit * e.g * 0.1, hit, e.r * 0.5, 1.0);
}
)GLSL";

// 每个点覆盖全部 180 个角度(片元内循环)
// 【真机修复】同 FS_HOUGH_VOTE_A: 点序号走 varying vIdx(kPointIndexVS 提供),
// 投票桶坐标走 gl_FragCoord, 片元侧不再引用 gl_VertexID, 也不再声明用不到的 vUV。
const char* FS_HOUGH_VOTE_B = R"GLSL(#version 300 es
precision highp float;
in float vIdx;
out vec4 fragColor;
uniform sampler2D uEdges;
uniform float uScale;
uniform float uFrame;
float hash1(float n) {
    return fract(sin(n * 12.9898 + uFrame) * 43758.5453);
}
void main() {
    float fi = vIdx;
    float u = hash1(fi + 1.5);
    float v = hash1(fi * 1.37 + 7.1);
    vec4 e = texture(uEdges, vec2(u, v));
    float ci = floor(gl_FragCoord.x);
    float ri = floor(gl_FragCoord.y);
    float vote = 0.0;
    if (e.r > 0.35) {
        for (int i = 0; i < 180; i++) {
            float ang = (float(i) + 0.5) * 0.017453292;
            float rho = (u * cos(ang) + v * sin(ang)) * 0.5 + 0.5;
            float target = floor(rho * uScale);
            if (abs(ci - float(i)) < 0.5 && abs(ri - target) < 0.5) {
                vote += e.g * 0.05;
            }
        }
    }
    fragColor = vec4(vote, vote * 0.5, e.r * 0.25, 1.0);
}
)GLSL";

const char* FS_HOUGH_DRAW = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uAcc;
uniform float uFrame;
void main() {
    float ci = floor(vUV.x * 1024.0);
    float cxo = mod(ci, 90.0);
    float cyo = floor(ci / 90.0);
    float ri = floor(vUV.y * 1024.0);
    float aim = cyo - mod(ri, 6.0);
    vec2 c = (vec2(cxo, cyo) + 0.5) / vec2(512.0, 1024.0);
    float v = texture(uAcc, c).r;
    float s = sin((cxo * 12.9898 + cyo * 78.233 + ri * 0.017 + uFrame * 0.05)) * 43758.5453;
    s = fract(s);
    v *= 0.65 + 0.35 * s;
    if (abs(aim) > 0.5) {
        v *= 0.2;
    }
    v = min(v * 0.02, 1.0);
    fragColor = vec4(v, v * 0.6, v * 0.25, 1.0);
}
)GLSL";

// ---------------- Particle Physics ----------------

const char* FS_PART_UPDATE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uState;
uniform float uDt;
uniform float uFrame;
void main() {
    vec4 s = texture(uState, vUV);
    vec3 p = s.xyz;
    vec3 v = s.www;
    vec3 g = vec3(0.0, -9.81, 0.0);
    v += g * uDt;
    vec3 np = p + v * uDt;
    vec3 dir = np - vec3(0.0, 0.0, 0.0);
    float len = length(dir);
    if (len < 0.55) {
        vec3 n = dir / max(len, 0.0001);
        np = n * 0.55;
        float vn = dot(v, n);
        v = (v - n * vn) * 0.72 - n * vn * 0.35;
    }
    if (np.y < -1.6) {
        np.y = -1.6;
        v.y = abs(v.y) * 0.55;
        v.xz *= 0.92;
    }
    float bound = 1.7;
    if (np.x > bound) {
        np.x = -bound;
    }
    if (np.x < -bound) {
        np.x = bound;
    }
    if (np.z > bound) {
        np.z = -bound;
    }
    if (np.z < -bound) {
        np.z = bound;
    }
    v *= (0.9995 - 0.0004 * sin(uFrame * 0.01));
    fragColor = vec4(np, max(length(v), 0.0));
}
)GLSL";

const char* FS_PART_DRAW = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uState;
uniform float uCount;
uniform float uFrame;
void main() {
    vec4 s = texture(uState, vUV);
    float life = clamp(s.w * 0.2, 0.0, 1.0);
    float rim = 1.0 - min(length(vUV - vec2(0.5)) * 1.4, 1.0);
    fragColor = vec4((0.25 + 0.75 * life) * rim, (0.35 + 0.5 * (1.0 - life)) * rim, 0.95 * rim, 1.0);
}
)GLSL";

const char* VS_PART_DRAW = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPos;
uniform sampler2D uState;
uniform float uCount;
uniform float uPoint;
out vec2 vUV;
void main() {
    float fi = mod(float(gl_VertexID), uCount);
    float j = mod(fi, 1024.0);
    float i = floor(fi / 1024.0);
    vec4 s = texelFetch(uState, ivec2(int(j), int(i)), 0);
    vec3 p = s.xyz;
    gl_PointSize = uPoint;
    gl_Position = vec4(p.x * 0.32, p.y * 0.34 - 0.1, p.z * 0.22, 1.0);
    vUV = vec2(0.5 + p.x * 0.4, 0.5 + p.y * 0.4);
}
)GLSL";

// ---------------- Path Tracer (1280x720, 2 bounces, cosine weighted) ----------------

const char* FS_PT_TRACE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uAccum;
uniform vec2 uRes;
uniform float uFrame;
uniform float uSpp;
float hash1(float x) {
    return fract(sin(x * 12.9898) * 43758.5453);
}
vec3 hash3(float x) {
    return vec3(hash1(x), hash1(x + 17.31), hash1(x + 41.77));
}
vec3 sky(vec3 d) {
    float t = clamp(d.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 c = mix(vec3(0.25, 0.30, 0.40), vec3(0.55, 0.70, 0.95), t);
    float s = max(dot(d, normalize(vec3(0.55, 0.75, 0.35))), 0.0);
    c += vec3(1.0, 0.9, 0.7) * pow(s, 64.0) * 1.5;
    return c;
}
void hitScene(vec3 ro, vec3 rd, out float t, out vec3 n, out vec3 albedo, out vec3 emit) {
    t = 1.0e9;
    n = vec3(0.0, 1.0, 0.0);
    albedo = vec3(0.75);
    emit = vec3(0.0);
    if (rd.y < -0.0001) {
        float tp = -ro.y / rd.y;
        if (tp > 0.001 && tp < t) {
            t = tp;
            n = vec3(0.0, 1.0, 0.0);
            vec2 uv = (ro.xz + rd.xz * tp) * 0.5 + 0.5;
            float chk = mod(floor(uv.x * 8.0) + floor(uv.y * 8.0), 2.0);
            albedo = mix(vec3(0.22, 0.24, 0.28), vec3(0.80, 0.78, 0.72), chk);
        }
    }
    for (int i = 0; i < 6; i++) {
        float fi = float(i);
        vec3 c = vec3(sin(fi * 2.1 + 0.7) * 2.4, 1.7 + mod(fi, 2.0) * 1.3, cos(fi * 1.7 + 1.3) * 2.4);
        float r = 0.6 + mod(fi, 3.0) * 0.35;
        vec3 oc = ro - c;
        float b = dot(oc, rd);
        float cc = dot(oc, oc) - r * r;
        float disc = b * b - cc;
        if (disc > 0.0) {
            float sq = sqrt(disc);
            float t0 = -b - sq;
            float tt = t0;
            if (tt < 0.002) {
                tt = -b + sq;
            }
            if (tt > 0.002 && tt < t) {
                t = tt;
                n = normalize(ro + rd * tt - c);
                float fl = mod(fi, 4.0);
                if (fl < 1.0) {
                    albedo = vec3(0.90, 0.30, 0.25);
                } else if (fl < 2.0) {
                    albedo = vec3(0.30, 0.80, 0.40);
                } else if (fl < 3.0) {
                    albedo = vec3(0.30, 0.50, 0.95);
                } else {
                    albedo = vec3(0.95, 0.85, 0.35);
                    emit = vec3(3.5, 3.2, 2.6);
                }
            }
        }
    }
}
void main() {
    vec4 prev = texture(uAccum, vUV);
    vec3 col = prev.rgb;
    float w = prev.a;
    float base = uFrame * 4096.0 + gl_FragCoord.x * 7.13 + gl_FragCoord.y * 3.71;
    for (int s = 0; s < 4; s++) {
        if (float(s) >= uSpp) {
            continue;
        }
        vec3 rnd = hash3(base + float(s) * 131.7);
        vec2 sp = gl_FragCoord.xy + rnd.xy;
        vec2 uv = (sp - uRes * 0.5) / max(uRes.y, 1.0);
        float fov = 1.35;
        vec3 ro = vec3(0.0, 2.4, 6.0);
        vec3 ta = vec3(0.0, 1.0, 0.0);
        vec3 fw = normalize(ta - ro);
        vec3 rt = normalize(cross(vec3(0.0, 1.0, 0.0), fw));
        vec3 up = cross(fw, rt);
        vec3 rd = normalize(uv.x * fov * rt + uv.y * fov * up + fw);
        vec3 tp = vec3(1.0);
        vec3 acc = vec3(0.0);
        for (int b = 0; b < 3; b++) {
            float t = 0.0;
            vec3 n = vec3(0.0);
            vec3 albedo = vec3(0.0);
            vec3 emit = vec3(0.0);
            hitScene(ro, rd, t, n, albedo, emit);
            if (t > 1.0e8) {
                acc += tp * sky(rd);
                break;
            }
            acc += tp * emit;
            vec3 p = ro + rd * t;
            vec3 nl = (dot(n, rd) < 0.0) ? n : -n;
            float r1 = hash1(base + float(s) * 7.7 + float(b) * 31.3);
            float r2 = hash1(base + float(s) * 3.3 + float(b) * 19.1);
            float rr = sqrt(r1);
            float ph = 6.2831853 * r2;
            vec3 dir = nl * sqrt(max(1.0 - r1, 0.0)) + normalize(cross(nl, vec3(0.0, 1.0, 0.0)) + vec3(0.001)) * (rr * cos(ph)) + cross(nl, normalize(cross(nl, vec3(0.0, 1.0, 0.0)) + vec3(0.001))) * (rr * sin(ph));
            rd = normalize(dir);
            ro = p + nl * 0.002;
            tp *= albedo * 0.92;
            if (b == 1) {
                break;
            }
        }
        col += tp;
        w += 1.0;
    }
    float inv = 1.0 / max(w, 1.0);
    fragColor = vec4(col * inv, w);
}
)GLSL";

const char* FS_PT_DRAW = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uAccum;
void main() {
    vec3 c = texture(uAccum, vUV).rgb;
    c = c / (c + vec3(1.0));
    c = pow(c, vec3(0.4545));
    fragColor = vec4(c, 1.0);
}
)GLSL";
// ---------------------------------------------------------------------------
// 着色器源码 3/3: Photo Filter / RAW / Super Resolution / Video Filter
// ---------------------------------------------------------------------------

const char* FS_PF_LUT = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform sampler2D uLut;
uniform float uFrame;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    float r = texture(uLut, vec2(c.r, 0.5)).r;
    float g = texture(uLut, vec2(c.g, 0.5)).g;
    float b = texture(uLut, vec2(c.b, 0.5)).b;
    c = vec3(r, g, b);
    c = clamp(c, 0.0, 1.0);
    float gr = fract(sin((vUV.x * 1234.5 + vUV.y * 678.9) * 43758.5453) * 43758.5453);
    c += (gr - 0.5) * (0.010 + 0.004 * sin(uFrame));
    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

const char* FS_PF_SAT = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(l), c, 1.42), 0.0, 1.0);
    fragColor = vec4(c, 1.0);
}
)GLSL";

const char* FS_PF_SHARPEN = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;
void main() {
    vec3 s = texture(uTex, vUV).rgb;
    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float fx = float(x);
            float fy = float(y);
            vec3 t = texture(uTex, vUV + vec2(fx * uTexel.x, fy * uTexel.y)).rgb;
            float w = 1.0;
            if (x == 0 && y == 0) {
                w = 1.0;
            } else if (x == 0 || y == 0) {
                w = 1.0;
            }
            sum += t * w;
        }
    }
    sum -= s * 8.0;
    vec3 sharp = s + sum * 0.65;
    vec3 c = clamp(mix(s, sharp, 1.0), 0.0, 1.0);
    fragColor = vec4(c, 1.0);
}
)GLSL";

const char* FS_PF_VIGNETTE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    vec2 d = vUV - vec2(0.5);
    float r = dot(d, d) * 2.0;
    float v = 1.0 - 0.55 * smoothstep(0.10, 0.85, r);
    fragColor = vec4(clamp(c * v, 0.0, 1.0), 1.0);
}
)GLSL";

const char* FS_PF_SEPIA = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 sep = vec3(l * 1.07, l * 0.94, l * 0.74);
    c = mix(c, sep, 0.65);
    c = clamp((c - 0.5) * 1.05 + 0.5, 0.0, 1.0);
    fragColor = vec4(c, 1.0);
}
)GLSL";

// ---------------- RAW: 合成 Bayer CFA + 双线性去马赛克 + 色彩 ----------------

const char* FS_RAW_BAYER = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform float uFrame;
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
void main() {
    vec2 uv = vUV;
    vec3 c = vec3(0.0);
    c += vec3(0.55, 0.40, 0.22) * smoothstep(0.0, 0.7, uv.y);
    c += vec3(0.18, 0.32, 0.55) * (1.0 - uv.y);
    for (int k = 0; k < 10; k++) {
        float fk = float(k) + 1.0;
        float a = uv.x * 6.0 * fk + uv.y * 3.0 * fk + uFrame * 0.001;
        c += vec3(sin(a) * 0.02, cos(a * 1.7) * 0.02, sin(a * 0.5) * 0.02);
    }
    c *= 0.85 + 0.30 * hash21(floor(uv * vec2(200.0, 150.0)));
    vec2 t = floor(uv * vec2(4000.0, 3000.0));
    int tx = int(t.x);
    int ty = int(t.y);
    int ex = tx - (tx / 2) * 2;
    int ey = ty - (ty / 2) * 2;
    float v = 0.0;
    if (ex == 0 && ey == 0) {
        v = c.r;
    } else if (ex == 1 && ey == 1) {
        v = c.b;
    } else {
        v = c.g;
    }
    fragColor = vec4(clamp(v, 0.0, 1.0), 0.0, 0.0, 1.0);
}
)GLSL";

const char* FS_RAW_DEMOSAIC = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uBayer;
uniform vec2 uTexel;
void main() {
    vec2 uv = vUV;
    vec2 t = uTexel;
    vec2 tc = floor(uv / t);
    int tx = int(tc.x);
    int ty = int(tc.y);
    int ex = tx - (tx / 2) * 2;
    int ey = ty - (ty / 2) * 2;
    float c = texture(uBayer, uv).r;
    float n = texture(uBayer, uv + vec2(0.0, t.y)).r;
    float s = texture(uBayer, uv - vec2(0.0, t.y)).r;
    float e = texture(uBayer, uv + vec2(t.x, 0.0)).r;
    float w = texture(uBayer, uv - vec2(t.x, 0.0)).r;
    float r = c;
    float g = (n + s + e + w) * 0.25;
    float b = c;
    if (ex == 0 && ey == 0) {
        b = (texture(uBayer, uv + vec2(t.x, t.y)).r + texture(uBayer, uv - vec2(t.x, t.y)).r +
             texture(uBayer, uv + vec2(t.x, -t.y)).r + texture(uBayer, uv + vec2(-t.x, t.y)).r) * 0.25;
        g = (n + s + e + w) * 0.25;
        r = c;
    } else if (ex == 1 && ey == 1) {
        r = (texture(uBayer, uv + vec2(t.x, t.y)).r + texture(uBayer, uv - vec2(t.x, t.y)).r +
             texture(uBayer, uv + vec2(t.x, -t.y)).r + texture(uBayer, uv + vec2(-t.x, t.y)).r) * 0.25;
        g = (n + s + e + w) * 0.25;
        b = c;
    } else if (ex == 0) {
        r = (e + w) * 0.5;
        b = (n + s) * 0.5;
        g = c;
    } else {
        r = (n + s) * 0.5;
        b = (e + w) * 0.5;
        g = c;
    }
    fragColor = vec4(r, g, b, 1.0);
}
)GLSL";

const char* FS_RAW_COLOR = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform float uFrame;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    c *= vec3(1.85, 1.00, 1.42);
    float r = dot(c, vec3(1.18, -0.12, -0.06));
    float g = dot(c, vec3(-0.09, 1.12, -0.03));
    float b = dot(c, vec3(0.02, -0.14, 1.12));
    c = vec3(r, g, b);
    c = max(c, vec3(0.0));
    c = pow(c * (0.95 + 0.05 * sin(uFrame * 0.01)), vec3(1.0 / 2.2));
    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

// ---------------- Super Resolution (960x540 -> 3840x2160, 3 层 3x3 卷积) ----------------

// 【真机修复】原来的 gl_Position = vec4(aPos, 0.0, 0.0, 1.0) 是 5 个分量塞进 vec4
// (vec2 + float + float + float): GLSL ES 3.00 构造函数要求分量数相等, 真机驱动对
// 这个多出来的分量报 "unused in constructor"(驱动把它当作"最后一个实参未参与构造"),
// 该 VS 因此不可用 -> super resolution 的 12 趟卷积全部链接失败, 报 "vertex shader
// outputs ..." 的接口不匹配。正确写法是 4 个分量: (x, y, 0.0, 1.0)。
const char* VS_PASS = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* FS_SR_CONV = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uIn0;
uniform sampler2D uIn1;
uniform sampler2D uIn2;
uniform sampler2D uIn3;
uniform vec2 uTexel;
uniform vec2 uOffset;
uniform vec2 uCh;
uniform float uRelu;
uniform float uFrame;
void main() {
    vec4 s0 = vec4(0.0);
    vec4 s1 = vec4(0.0);
    vec4 s2 = vec4(0.0);
    vec4 s3 = vec4(0.0);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            vec2 o = (vec2(float(x), float(y)) - 1.0) * uTexel;
            float fy = float(y);
            float fx = float(x);
            float wgt = (1.0 + 0.05 * sin(uFrame * 0.01 + fy)) * (1.0 + 0.05 * cos(uFrame * 0.013 + fx));
            s0 += texture(uIn0, vUV + o) * wgt;
            s1 += texture(uIn1, vUV + o) * wgt;
            s2 += texture(uIn2, vUV + o) * wgt;
            s3 += texture(uIn3, vUV + o) * wgt;
        }
    }
    s0 = s0 * 0.1111111 + s0.yzwx * 0.02;
    s1 = s1 * 0.1111111 + s1.yzwx * 0.02;
    s2 = s2 * 0.1111111 + s2.yzwx * 0.02;
    s3 = s3 * 0.1111111 + s3.yzwx * 0.02;
    if (uRelu > 0.5) {
        s0 = max(s0, vec4(0.0));
        s1 = max(s1, vec4(0.0));
        s2 = max(s2, vec4(0.0));
        s3 = max(s3, vec4(0.0));
    }
    vec4 o = vec4(0.0);
    if (uCh.x < 0.5) {
        o = s0;
    } else if (uCh.x < 1.5) {
        o = s1;
    } else if (uCh.x < 2.5) {
        o = s2;
    } else {
        o = s3;
    }
    o += vec4(uOffset.x, uOffset.y, 0.0, 0.0);
    fragColor = vec4(clamp(o.rgb, 0.0, 8.0), clamp(o.a, 0.0, 8.0));
}
)GLSL";

// ---------------- Video Filter (1280x720) ----------------

const char* FS_VF_DENOISE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uFrame;
uniform sampler2D uPrev;
uniform float uMotion;
void main() {
    vec4 c = texture(uFrame, vUV);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            vec2 uv2 = vUV + vec2(float(x), float(y)) * 0.0022;
            vec3 p = texture(uPrev, uv2).rgb;
            float w = 1.0 - clamp(distance(p, c.rgb) * 1.8, 0.0, 0.95);
            sum += p * w;
            wsum += w;
        }
    }
    vec3 avg = sum / max(wsum, 0.0001);
    float md = clamp(distance(avg, c.rgb) * 2.4, 0.0, 1.0);
    float wa = mix(0.55, 0.10, md) * uMotion;
    vec3 o = mix(c.rgb, avg, wa);
    fragColor = vec4(clamp(o, 0.0, 1.0), 1.0);
}
)GLSL";

const char* FS_VF_BLEND = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uT;
void main() {
    vec3 a = texture(uA, vUV).rgb;
    vec3 b = texture(uB, vUV + vec2(0.0035 * uT, 0.0)).rgb;
    fragColor = vec4(clamp(mix(a, b, uT), 0.0, 1.0), 1.0);
}
)GLSL";

const char* FS_VF_SHARP = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            sum += texture(uTex, vUV + vec2(float(x) * uTexel.x, float(y) * uTexel.y)).rgb;
        }
    }
    vec3 blur = sum * 0.1111111;
    vec3 o = c + (c - blur) * 0.85;
    fragColor = vec4(clamp(o, 0.0, 1.0), 1.0);
}
)GLSL";

const char* FS_VF_GRADE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform float uFrame;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, 1.22);
    vec3 sh = vec3(0.03, 0.045, 0.085);
    vec3 hi = vec3(0.065, 0.035, 0.015);
    c = c + sh * (1.0 - l) + hi * l;
    c = clamp((c - 0.5) * (1.03 + 0.02 * sin(uFrame * 0.02)) + 0.5, 0.0, 1.0);
    float v = 1.0 - dot(vUV - vec2(0.5), vUV - vec2(0.5)) * 0.35;
    fragColor = vec4(clamp(c * v, 0.0, 1.0), 1.0);
}
)GLSL";
// ---------------------------------------------------------------------------
// 程序登记表: 每个 ProgId -> (顶点着色器, 片元着色器, 名称)
//
//   * 表项数量与 ProgId 一一对应, 由下面的 static_assert 在编译期强制(枚举/表
//     条数不一致会直接编不过, 而不是运行时静默拿到 nullptr)。
//   * 顶点着色器不能一律用全屏三角形的那一个:
//       - 用 glDrawArrays(GL_POINTS, ...) 绘制的程序必须写 gl_PointSize, 且位置只由
//         gl_VertexID 决定(点数量可以远大于顶点缓冲里的顶点数):
//           * 片元着色器自己需要"点序号"的(面跟踪累加 / Hough 投票 A,B)用
//             kPointIndexVS —— 它把序号作为 varying vIdx 传给片元;
//           * 不需要点序号的(描述子汇总)用 kPointVS(varying 只有 vUV);
//       - 粒子绘制(P_PART_DRAW)的顶点位置来自状态纹理, 必须用 VS_PART_DRAW
//         (采样 uState, gl_PointSize = uPoint), 并且必须用 GL_POINTS 绘制;
//       - 其余全屏 pass 用 kQuadVS 或等价的 VS_PASS(vUV + 全屏三角形)。
//   * 硬规则(真机链接失败后确立): 片元着色器里不许出现 gl_VertexID(ES 3.00 只在
//     顶点着色器提供); 片元声明的每个 in 必须有同名同类型的顶点 out 与之对应, 且
//     两端都真正使用(未使用的 varying 会被驱动消除, 两端消除不一致即在链接期报
//     "Undeclared variable")。离线核对脚本 tools/gpu7_shader_check.py 逐条强制这些规则。
// ---------------------------------------------------------------------------
struct ProgSource {
    const char* vs;
    const char* fs;
    const char* label;
};

const ProgSource g_progTable[P_PROG_COUNT] = {
    /* P_BLUR_H         */ {kQuadVS,      FS_BLUR_H,         "blur_h"},
    /* P_BLUR_V         */ {kQuadVS,      FS_BLUR_V,         "blur_v"},
    /* P_SCENE          */ {kQuadVS,      FS_SCENE,          "scene"},
    /* P_FT_ACC         */ {kPointIndexVS, FS_FT_ACC,        "ft_acc"},
    /* P_FT_UPDATE      */ {kQuadVS,      FS_FT_UPDATE,      "ft_update"},
    /* P_FM_HIST        */ {kQuadVS,      FS_FM_HIST,        "fm_hist"},
    /* P_FM_MATCH       */ {kQuadVS,      FS_FM_MATCH,       "fm_match"},
    /* P_FM_SUMMARY     */ {kPointVS,     FS_FM_SUMMARY,     "fm_summary"},
    /* P_FLUID_ADV      */ {kQuadVS,      FS_FLUID_ADV,      "fluid_advect"},
    /* P_FLUID_DIVERG   */ {kQuadVS,      FS_FLUID_DIVERG,   "fluid_divergence"},
    /* P_FLUID_JACOBI   */ {kQuadVS,      FS_FLUID_JACOBI,   "fluid_jacobi"},
    /* P_FLUID_GRADSUB  */ {kQuadVS,      FS_FLUID_GRADSUB,  "fluid_gradsub"},
    /* P_FLUID_DISPLAY  */ {kQuadVS,      FS_FLUID_DISPLAY,  "fluid_display"},
    /* P_HOUGH_EDGE     */ {kQuadVS,      FS_HOUGH_EDGE,     "hough_edge"},
    /* P_HOUGH_VOTE_A   */ {kPointIndexVS, FS_HOUGH_VOTE_A, "hough_vote_a"},
    /* P_HOUGH_VOTE_B   */ {kPointIndexVS, FS_HOUGH_VOTE_B, "hough_vote_b"},
    /* P_HOUGH_DRAW     */ {kQuadVS,      FS_HOUGH_DRAW,     "hough_draw"},
    /* P_PART_UPDATE    */ {kQuadVS,      FS_PART_UPDATE,    "particle_update"},
    /* P_PART_DRAW      */ {VS_PART_DRAW, FS_PART_DRAW,      "particle_draw"},
    /* P_PT_TRACE       */ {kQuadVS,      FS_PT_TRACE,       "path_trace"},
    /* P_PT_DRAW        */ {kQuadVS,      FS_PT_DRAW,        "path_draw"},
    /* P_PF_LUT         */ {kQuadVS,      FS_PF_LUT,         "photo_lut"},
    /* P_PF_SAT         */ {kQuadVS,      FS_PF_SAT,         "photo_saturation"},
    /* P_PF_SHARPEN     */ {kQuadVS,      FS_PF_SHARPEN,     "photo_sharpen"},
    /* P_PF_VIGNETTE    */ {kQuadVS,      FS_PF_VIGNETTE,    "photo_vignette"},
    /* P_PF_SEPIA       */ {kQuadVS,      FS_PF_SEPIA,       "photo_sepia"},
    /* P_RAW_BAYER      */ {kQuadVS,      FS_RAW_BAYER,      "raw_bayer"},
    /* P_RAW_DEMOSAIC   */ {kQuadVS,      FS_RAW_DEMOSAIC,   "raw_demosaic"},
    /* P_RAW_COLOR      */ {kQuadVS,      FS_RAW_COLOR,      "raw_color"},
    /* P_SR_CONV        */ {VS_PASS,      FS_SR_CONV,        "super_resolution_conv"},
    /* P_VF_DENOISE     */ {kQuadVS,      FS_VF_DENOISE,     "video_denoise"},
    /* P_VF_BLEND       */ {kQuadVS,      FS_VF_BLEND,       "video_blend"},
    /* P_VF_SHARP       */ {kQuadVS,      FS_VF_SHARP,       "video_sharpen"},
    /* P_VF_GRADE       */ {kQuadVS,      FS_VF_GRADE,       "video_grade"},
};

static_assert(sizeof(g_progTable) / sizeof(g_progTable[0]) == (size_t)P_PROG_COUNT,
              "g_progTable must have exactly one entry per ProgId");
static_assert((int)P_PROG_COUNT == 34, "ProgId enumeration changed: update g_progTable");

GLuint getProg(ProgId id)
{
    const int idx = (int)id;
    if (idx < 0 || idx >= (int)P_PROG_COUNT) {
        gpu7Error = "bad program id " + std::to_string(idx);
        return 0;
    }
    ProgEntry& e = g_progs[idx];
    if (e.tried) {
        return e.prog;
    }
    e.tried = true;
    const ProgSource& src = g_progTable[idx];
    if (src.vs == nullptr || src.fs == nullptr) {
        appendGpu7Error(std::string("missing shader source for program '") +
                        (src.label != nullptr ? src.label : "?") + "'");
        return 0;
    }
    std::string err;
    e.prog = makeProgram(src.vs, src.fs, src.label, &err);
    if (e.prog == 0) {
        // 累加而不是覆盖: 一项负载里会取多个程序(Face Tracking 4 个 / Horizon 5 个),
        // 覆盖会让"先失败的程序"在界面上完全消失, 真机就只剩最后一个失败项可看。
        appendGpu7Error(err);
        logChunks("program failure", err);
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "prog %{public}d failed: %{public}s",
                     idx, (src.label != nullptr ? src.label : "?"));
    }
    return e.prog;
}

// ---------------------------------------------------------------------------
// 各项负载
// ---------------------------------------------------------------------------

// 1) Background Blur: 两趟可分离高斯(31 抽样, 半径 15)@1920x1080, 2 pass/帧
FrameStats loadBlur(int frames)
{
    FrameStats st;
    st.unit = "Mpx/s";
    if (!beginLoad("Background Blur")) {
        st.error = gpu7Error;
        return st;
    }
    const int W = 1920;
    const int H = 1080;
    Target scene;
    Target tmp;
    Target out;
    if (!createTarget(scene, W, H, GL_RGBA8) || !createTarget(tmp, W, H, GL_RGBA8) ||
        !createTarget(out, W, H, GL_RGBA8)) {
        st.error = "blur fbo create failed: " + g_targetError;
        destroyTarget(scene);
        destroyTarget(tmp);
        destroyTarget(out);
        endLoad();
        return st;
    }
    GLuint pScene = getProg(P_SCENE);
    GLuint pH = getProg(P_BLUR_H);
    GLuint pV = getProg(P_BLUR_V);
    if (pScene == 0 || pH == 0 || pV == 0) {
        st.error = initError(true, "blur shader failed");
        destroyTarget(scene);
        destroyTarget(tmp);
        destroyTarget(out);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    bindTarget(scene);
    glUseProgram(pScene);
    setF1(pScene, "uSeed", 0.5f);
    drawFullscreen();

    const float radius = 15.0f;
    const float sigma = 7.5f;
    const double perFramePixels = (double)W * (double)H * 2.0;
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        bindTarget(tmp);
        glUseProgram(pH);
        bindTex(0, scene.tex);
        setI1(pH, "uTex", 0);
        set2F(pH, "uTexel", 1.0f / (float)W, 1.0f / (float)H);
        setF1(pH, "uRadius", radius);
        setF1(pH, "uSigma", sigma);
        drawFullscreen();

        bindTarget(out);
        glUseProgram(pV);
        bindTex(0, tmp.tex);
        setI1(pV, "uTex", 0);
        set2F(pV, "uTexel", 1.0f / (float)W, 1.0f / (float)H);
        setF1(pV, "uRadius", radius);
        setF1(pV, "uSigma", sigma);
        drawFullscreen();
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(out.fbo, W, H, &chk);
    timer.finish(st, perFramePixels, "Mpx/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "blur checksum %{public}f", chk);
    destroyTarget(scene);
    destroyTarget(tmp);
    destroyTarget(out);
    endLoad();
    return st;
}

// 2) Face Tracking: 4 级金字塔 + 每级 5 次 LK 迭代(共 20 次)@1920x1080
struct PyramidLevel {
    Target img;
    Target res;
    int w = 0;
    int h = 0;
};

FrameStats loadFaceTracking(int frames)
{
    FrameStats st;
    st.unit = "Mpx/s";
    if (!beginLoad("Face Tracking")) {
        st.error = gpu7Error;
        return st;
    }
    const int W = 1920;
    const int H = 1080;
    const int LEVELS = 4;
    const int ITERS = 5;
    const int SEED_X = 64;
    const int SEED_Y = 64;
    PyramidLevel lv[LEVELS];
    Target accum;
    Target out;
    bool ok = createTarget(accum, 8, 8, GL_RGBA16F) && createTarget(out, W, H, GL_RGBA8);
    for (int l = 0; l < LEVELS && ok; ++l) {
        lv[l].w = W >> l;
        lv[l].h = H >> l;
        ok = createTarget(lv[l].img, lv[l].w, lv[l].h, GL_RGBA16F) &&
             createTarget(lv[l].res, lv[l].w, lv[l].h, GL_RGBA16F);
    }
    GLuint pScene = getProg(P_SCENE);
    GLuint pAcc = getProg(P_FT_ACC);
    GLuint pUpd = getProg(P_FT_UPDATE);
    GLuint pCopy = getProg(P_BLUR_H);
    if (!ok || pScene == 0 || pAcc == 0 || pUpd == 0 || pCopy == 0) {
        st.error = initError(ok, "face tracking init failed");
        for (int l = 0; l < LEVELS; ++l) {
            destroyTarget(lv[l].img);
            destroyTarget(lv[l].res);
        }
        destroyTarget(accum);
        destroyTarget(out);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    for (int l = 0; l < LEVELS; ++l) {
        bindTarget(lv[l].img);
        glUseProgram(pScene);
        setF1(pScene, "uSeed", 2.5f + (float)l * 0.37f);
        drawFullscreen();
    }

    const double perFramePixels = (double)W * (double)H * (double)(LEVELS * ITERS);
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        float dispx = 0.0015f * (float)(fr % 7);
        float dispy = -0.0011f * (float)(fr % 5);
        for (int l = 0; l < LEVELS; ++l) {
            for (int it = 0; it < ITERS; ++it) {
                // a) 加法混合把 4096 个采样点的 Hessian/残差累加到 8x8 累加器
                glBindFramebuffer(GL_FRAMEBUFFER, accum.fbo);
                glViewport(0, 0, accum.w, accum.h);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                glUseProgram(pAcc);
                bindTex(0, lv[l].img.tex);
                bindTex(1, lv[l].res.tex);
                setI1(pAcc, "uCurr", 0);
                setI1(pAcc, "uPrev", 1);
                set2F(pAcc, "uSeed", (float)SEED_X, (float)SEED_Y);
                set2F(pAcc, "uLevelSize", (float)lv[l].w, (float)lv[l].h);
                set2F(pAcc, "uLevelTexel", 1.0f / (float)lv[l].w, 1.0f / (float)lv[l].h);
                set2F(pAcc, "uDisp", dispx, dispy);
                glBindVertexArray(g_pointVao);
                glDrawArrays(GL_POINTS, 0, SEED_X * SEED_Y);
                glBindVertexArray(0);
                glDisable(GL_BLEND);

                // b) 64 路归约 + 求解 2x2 系统(输出进入下一轮迭代的 uPrev)
                bindTarget(lv[l].res);
                glUseProgram(pUpd);
                bindTex(0, accum.tex);
                setI1(pUpd, "uAcc", 0);
                set2F(pUpd, "uDisp", dispx, dispy);
                setF1(pUpd, "uIter", (float)it);
                drawFullscreen();

                dispx += 0.00002f;
                dispy -= 0.00001f;
            }
        }
        // c) 最粗一级的结果放大回 1920x1080 作为本帧输出
        bindTarget(out);
        glUseProgram(pCopy);
        bindTex(0, lv[LEVELS - 1].res.tex);
        setI1(pCopy, "uTex", 0);
        set2F(pCopy, "uTexel", 1.0f / (float)lv[LEVELS - 1].w, 1.0f / (float)lv[LEVELS - 1].h);
        setF1(pCopy, "uRadius", 3.0f);
        setF1(pCopy, "uSigma", 3.0f);
        drawFullscreen();
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(out.fbo, W, H, &chk);
    timer.finish(st, perFramePixels, "Mpx/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "face tracking checksum %{public}f", chk);
    for (int l = 0; l < LEVELS; ++l) {
        destroyTarget(lv[l].img);
        destroyTarget(lv[l].res);
    }
    destroyTarget(accum);
    destroyTarget(out);
    endLoad();
    return st;
}

// 3) Feature Matching: 4096 个 64 维描述子 + 暴力匹配(每帧 4096x4096 对)
FrameStats loadFeatureMatching(int frames)
{
    FrameStats st;
    st.unit = "Gpair/s";
    st.metricDigits = 2;
    if (!beginLoad("Feature Matching")) {
        st.error = gpu7Error;
        return st;
    }
    const int DESC = 4096;
    const int DIM = 64;
    const int TILE_H = 256;
    const int TILES = 16;
    Target scene;
    Target desc;
    Target match;
    if (!createTarget(scene, 1920, 1080, GL_RGBA8) || !createTarget(desc, DIM, DESC, GL_RGBA16F) ||
        !createTarget(match, DESC, TILE_H, GL_RGBA8)) {
        st.error = "feature matching fbo create failed: " + g_targetError;
        destroyTarget(scene);
        destroyTarget(desc);
        destroyTarget(match);
        endLoad();
        return st;
    }
    GLuint pScene = getProg(P_SCENE);
    GLuint pHist = getProg(P_FM_HIST);
    GLuint pMatch = getProg(P_FM_MATCH);
    GLuint pSum = getProg(P_FM_SUMMARY);
    if (pScene == 0 || pHist == 0 || pMatch == 0 || pSum == 0) {
        st.error = initError(true, "feature matching shader failed");
        destroyTarget(scene);
        destroyTarget(desc);
        destroyTarget(match);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    bindTarget(scene);
    glUseProgram(pScene);
    setF1(pScene, "uSeed", 7.25f);
    drawFullscreen();

    const double perFramePairs = (double)DESC * (double)TILE_H * (double)TILES;
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        // pass 1: 提取 4096 个 64 维梯度方向直方图描述子
        bindTarget(desc);
        glUseProgram(pHist);
        bindTex(0, scene.tex);
        setI1(pHist, "uTex", 0);
        set2F(pHist, "uTexel", 1.0f / 1920.0f, 1.0f / 1080.0f);
        setF1(pHist, "uFrame", (float)fr);
        drawFullscreen();

        // pass 2: 暴力匹配, 16 个 tile(每个 tile 比较 4096x256 对描述子)
        for (int t = 0; t < TILES; ++t) {
            bindTarget(match);
            glUseProgram(pMatch);
            bindTex(0, desc.tex);
            bindTex(1, desc.tex);
            setI1(pMatch, "uDesc", 0);
            setI1(pMatch, "uSummary", 1);
            set2F(pMatch, "uTile", 0.0f, (float)(t * TILE_H));
            setF1(pMatch, "uFrame", (float)((fr * 3 + t) % 17));
            drawFullscreen();
        }

        // pass 3: 把匹配残差写回描述子纹理首行(下一帧的 pass 1/2 会读到它,
        //         从而保证帧与帧之间存在真实的依赖关系)
        glBindFramebuffer(GL_FRAMEBUFFER, desc.fbo);
        glViewport(0, 0, 1, 1);
        glUseProgram(pSum);
        bindTex(2, match.tex);
        setI1(pSum, "uMatch", 2);
        glBindVertexArray(g_pointVao);
        glDrawArrays(GL_POINTS, 0, 1);
        glBindVertexArray(0);
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(match.fbo, DESC, TILE_H, &chk);
    timer.finish(st, perFramePairs, "Gpair/s", 2, 1.0 / 1.0e9);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "feature matching checksum %{public}f", chk);
    destroyTarget(scene);
    destroyTarget(desc);
    destroyTarget(match);
    endLoad();
    return st;
}

// 4) Fluid Simulation: GPU stable fluids @1024x1024, 每帧 48 次 Jacobi 压力迭代
FrameStats loadFluid(int frames)
{
    FrameStats st;
    st.unit = "Mcell/s";
    if (!beginLoad("Fluid Simulation")) {
        st.error = gpu7Error;
        return st;
    }
    const int N = 1024;
    const int JACOBI = 48;
    Target velA;
    Target velB;
    Target div;
    Target presA;
    Target presB;
    Target dyeA;
    Target dyeB;
    Target disp;
    bool ok = createTarget(velA, N, N, GL_RGBA16F) && createTarget(velB, N, N, GL_RGBA16F) &&
              createTarget(div, N, N, GL_RGBA16F) && createTarget(presA, N, N, GL_RGBA16F) &&
              createTarget(presB, N, N, GL_RGBA16F) && createTarget(dyeA, N, N, GL_RGBA16F) &&
              createTarget(dyeB, N, N, GL_RGBA16F) && createTarget(disp, 1920, 1080, GL_RGBA8);
    GLuint pAdv = getProg(P_FLUID_ADV);
    GLuint pDiv = getProg(P_FLUID_DIVERG);
    GLuint pJac = getProg(P_FLUID_JACOBI);
    GLuint pGrad = getProg(P_FLUID_GRADSUB);
    GLuint pDisp = getProg(P_FLUID_DISPLAY);
    if (!ok || pAdv == 0 || pDiv == 0 || pJac == 0 || pGrad == 0 || pDisp == 0) {
        st.error = initError(ok, "fluid init failed");
        destroyTarget(velA);
        destroyTarget(velB);
        destroyTarget(div);
        destroyTarget(presA);
        destroyTarget(presB);
        destroyTarget(dyeA);
        destroyTarget(dyeB);
        destroyTarget(disp);
        endLoad();
        return st;
    }
    // 初始速度场: 用平流 pass 基于"散度纹理"铺一层确定性的旋度场
    glDisable(GL_BLEND);
    bindTarget(velA);
    glUseProgram(pAdv);
    bindTex(0, div.tex);
    bindTex(1, dyeA.tex);
    setI1(pAdv, "uVel", 0);
    setI1(pAdv, "uSrc", 1);
    set2F(pAdv, "uTexel", 1.0f / (float)N, 1.0f / (float)N);
    setF1(pAdv, "uDt", 0.0f);
    drawFullscreen();

    const float texel = 1.0f / (float)N;
    const float rbeta = 0.25f;
    const double perFrameCells = (double)N * (double)N * (double)(JACOBI + 5);
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        // a) 染料/速度平流 4 次
        for (int i = 0; i < 4; ++i) {
            Target& dst = (i % 2 == 0) ? velB : velA;
            Target& src = (i % 2 == 0) ? velA : velB;
            bindTarget(dst);
            glUseProgram(pAdv);
            bindTex(0, src.tex);
            bindTex(1, dyeA.tex);
            setI1(pAdv, "uVel", 0);
            setI1(pAdv, "uSrc", 1);
            set2F(pAdv, "uTexel", texel, texel);
            setF1(pAdv, "uDt", 0.016f + 0.001f * (float)i);
            drawFullscreen();
        }
        // b) 散度
        bindTarget(div);
        glUseProgram(pDiv);
        bindTex(0, velA.tex);
        setI1(pDiv, "uVel", 0);
        set2F(pDiv, "uTexel", texel, texel);
        drawFullscreen();
        // c) 压力迭代: 播撒源项 + (JACOBI-1) 次 Jacobi 迭代
        bindTarget(presB);
        glUseProgram(pJac);
        bindTex(0, presA.tex);
        bindTex(1, div.tex);
        setI1(pJac, "uPres", 0);
        setI1(pJac, "uDiv", 1);
        set2F(pJac, "uTexel", texel, texel);
        setF1(pJac, "uRbeta", 0.5f);
        drawFullscreen();
        for (int i = 1; i < JACOBI; ++i) {
            Target& dst = (i % 2 == 1) ? presA : presB;
            Target& src = (i % 2 == 1) ? presB : presA;
            bindTarget(dst);
            glUseProgram(pJac);
            bindTex(0, src.tex);
            bindTex(1, div.tex);
            setI1(pJac, "uPres", 0);
            setI1(pJac, "uDiv", 1);
            set2F(pJac, "uTexel", texel, texel);
            setF1(pJac, "uRbeta", rbeta);
            drawFullscreen();
        }
        // d) 压力梯度减除
        bindTarget(velB);
        glUseProgram(pGrad);
        bindTex(0, presA.tex);
        bindTex(1, velA.tex);
        setI1(pGrad, "uPres", 0);
        setI1(pGrad, "uVel", 1);
        set2F(pGrad, "uTexel", texel, texel);
        setF1(pGrad, "uFrame", (float)fr);
        drawFullscreen();
        // e) 染料输送(结果写回 velA, 供下一帧继续使用)
        bindTarget(dyeB);
        glUseProgram(pAdv);
        bindTex(0, velB.tex);
        bindTex(1, dyeA.tex);
        setI1(pAdv, "uVel", 0);
        setI1(pAdv, "uSrc", 1);
        set2F(pAdv, "uTexel", texel, texel);
        setF1(pAdv, "uDt", 0.5f);
        drawFullscreen();
        bindTarget(velA);
        glUseProgram(pAdv);
        bindTex(0, velB.tex);
        bindTex(1, dyeB.tex);
        setI1(pAdv, "uVel", 0);
        setI1(pAdv, "uSrc", 1);
        set2F(pAdv, "uTexel", texel, texel);
        setF1(pAdv, "uDt", 0.0f);
        drawFullscreen();
        // f) 显示到 1920x1080
        bindTarget(disp);
        glUseProgram(pDisp);
        bindTex(0, velA.tex);
        setI1(pDisp, "uVel", 0);
        drawFullscreen();
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(disp.fbo, 1920, 1080, &chk);
    timer.finish(st, perFrameCells, "Mcell/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "fluid checksum %{public}f", chk);
    destroyTarget(velA);
    destroyTarget(velB);
    destroyTarget(div);
    destroyTarget(presA);
    destroyTarget(presB);
    destroyTarget(dyeA);
    destroyTarget(dyeB);
    destroyTarget(disp);
    endLoad();
    return st;
}

// 5) Horizon Detection: Sobel + Hough(180 角度 x 1024 rho 桶)@1920x1080
FrameStats loadHorizon(int frames)
{
    FrameStats st;
    st.unit = "Mvote/s";
    if (!beginLoad("Horizon Detection")) {
        st.error = gpu7Error;
        return st;
    }
    const int W = 1920;
    const int H = 1080;
    const int EW = 960;
    const int EH = 540;
    const int ACC = 1024;
    const int SUBSTEPS = 32;
    const int VOTES_A = 1 << 14;
    const int VOTES_B = 1 << 14;
    Target scene;
    Target edges;
    Target accA;
    Target accB;
    Target out;
    bool ok = createTarget(scene, W, H, GL_RGBA8) && createTarget(edges, EW, EH, GL_RGBA16F) &&
              createTarget(accA, ACC, ACC, GL_RGBA16F) && createTarget(accB, ACC, ACC, GL_RGBA16F) &&
              createTarget(out, W, H, GL_RGBA8);
    GLuint pScene = getProg(P_SCENE);
    GLuint pEdge = getProg(P_HOUGH_EDGE);
    GLuint pVa = getProg(P_HOUGH_VOTE_A);
    GLuint pVb = getProg(P_HOUGH_VOTE_B);
    GLuint pDraw = getProg(P_HOUGH_DRAW);
    if (!ok || pScene == 0 || pEdge == 0 || pVa == 0 || pVb == 0 || pDraw == 0) {
        st.error = initError(ok, "hough init failed");
        destroyTarget(scene);
        destroyTarget(edges);
        destroyTarget(accA);
        destroyTarget(accB);
        destroyTarget(out);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    bindTarget(scene);
    glUseProgram(pScene);
    setF1(pScene, "uSeed", 11.5f);
    drawFullscreen();

    const double perFrameVotes = (double)SUBSTEPS * ((double)VOTES_A + (double)VOTES_B * 180.0);
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        for (int i = 0; i < SUBSTEPS; ++i) {
            Target& dst = (i % 2 == 0) ? accB : accA;
            Target& src = (i % 2 == 0) ? accA : accB;
            glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
            glViewport(0, 0, ACC, ACC);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            // 投票 A: 每个点投一个角度
            glUseProgram(pVa);
            bindTex(0, edges.tex);
            setI1(pVa, "uEdges", 0);
            setF1(pVa, "uScale", (float)ACC);
            setF1(pVa, "uAngleScale", 180.0f / 3.14159265f);
            glBindVertexArray(g_pointVao);
            glDrawArrays(GL_POINTS, 0, VOTES_A);
            // 投票 B: 每个点覆盖全部 180 个角度
            glUseProgram(pVb);
            bindTex(0, edges.tex);
            setI1(pVb, "uEdges", 0);
            setF1(pVb, "uScale", (float)ACC);
            setF1(pVb, "uFrame", (float)(fr * 7 + i));
            glDrawArrays(GL_POINTS, 0, VOTES_B);
            glBindVertexArray(0);
            glDisable(GL_BLEND);
            // Sobel 边缘(输入是上一轮的累加器, 保证跨 pass 依赖)
            bindTarget(edges);
            glUseProgram(pEdge);
            bindTex(0, src.tex);
            setI1(pEdge, "uTex", 0);
            set2F(pEdge, "uTexel", 1.0f / (float)ACC, 1.0f / (float)ACC);
            drawFullscreen();
        }
        bindTarget(out);
        glUseProgram(pDraw);
        bindTex(0, accA.tex);
        setI1(pDraw, "uAcc", 0);
        setF1(pDraw, "uFrame", (float)fr);
        drawFullscreen();
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(out.fbo, W, H, &chk);
    timer.finish(st, perFrameVotes, "Mvote/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "hough checksum %{public}f", chk);
    destroyTarget(scene);
    destroyTarget(edges);
    destroyTarget(accA);
    destroyTarget(accB);
    destroyTarget(out);
    endLoad();
    return st;
}

// 6) Particle Physics: 1048576 个粒子, 每帧更新 524288 个 @1024x1024 状态纹理
FrameStats loadParticles(int frames)
{
    FrameStats st;
    st.unit = "Mparticle/s";
    if (!beginLoad("Particle Physics")) {
        st.error = gpu7Error;
        return st;
    }
    const int N = 1024;
    const int COUNT = N * N;
    const int PER_FRAME = COUNT / 2;
    Target stateA;
    Target stateB;
    Target out;
    if (!createTarget(stateA, N, N, GL_RGBA32F) || !createTarget(stateB, N, N, GL_RGBA32F) ||
        !createTarget(out, 1920, 1080, GL_RGBA16F)) {
        st.error = "particle fbo create failed: " + g_targetError;
        destroyTarget(stateA);
        destroyTarget(stateB);
        destroyTarget(out);
        endLoad();
        return st;
    }
    GLuint pUpd = getProg(P_PART_UPDATE);
    GLuint pDraw = getProg(P_PART_DRAW);
    if (pUpd == 0 || pDraw == 0) {
        st.error = initError(true, "particle shader failed");
        destroyTarget(stateA);
        destroyTarget(stateB);
        destroyTarget(out);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    // glTexImage2D(数据指针为 nullptr)的内容是未定义的: 先确定性地清零两张状态纹理,
    // 让初始粒子状态在不同驱动上完全一致(否则 NaN/垃圾值会让绘制结果每次不同)。
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, stateA.fbo);
    glViewport(0, 0, N, N);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, stateB.fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    // 初始状态(位置/速率), 由更新 pass 用 uDt = 0 写入
    bindTarget(stateA);
    glUseProgram(pUpd);
    bindTex(0, stateB.tex);
    setI1(pUpd, "uState", 0);
    setF1(pUpd, "uDt", 0.0f);
    setF1(pUpd, "uFrame", 16.0f);
    drawFullscreen();
    bindTarget(stateB);
    glUseProgram(pUpd);
    bindTex(0, stateA.tex);
    setI1(pUpd, "uState", 0);
    setF1(pUpd, "uDt", 0.002f);
    setF1(pUpd, "uFrame", 17.0f);
    drawFullscreen();

    const double perFrameParticles = (double)PER_FRAME;
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        // a) 两次全网格状态更新: 积分重力 + 球体/地面碰撞 + 阻尼
        bindTarget(stateA);
        glUseProgram(pUpd);
        bindTex(0, stateB.tex);
        setI1(pUpd, "uState", 0);
        setF1(pUpd, "uDt", 0.008f);
        setF1(pUpd, "uFrame", (float)fr);
        drawFullscreen();
        bindTarget(stateB);
        glUseProgram(pUpd);
        bindTex(0, stateA.tex);
        setI1(pUpd, "uState", 0);
        setF1(pUpd, "uDt", 0.008f);
        setF1(pUpd, "uFrame", (float)(fr + 1));
        drawFullscreen();
        // b) 粒子绘制: 每个粒子 = 一个点精灵。顶点位置与点大小由 VS_PART_DRAW
        //    从状态纹理按 gl_VertexID 取出(因此这里必须用 GL_POINTS 绘制, 不能用
        //    全屏三角形; 片元着色器再用 vUV 取同一个粒子的状态着色)。
        glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
        glViewport(0, 0, 1280, 720);
        glClearColor(0.01f, 0.01f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(pDraw);
        bindTex(0, stateB.tex);
        setI1(pDraw, "uState", 0);
        setF1(pDraw, "uCount", (float)COUNT);
        setF1(pDraw, "uPoint", 2.0f);
        glBindVertexArray(g_pointVao);
        glDrawArrays(GL_POINTS, 0, COUNT);
        glBindVertexArray(0);
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(out.fbo, 1280, 720, &chk);
    timer.finish(st, perFrameParticles, "Mparticle/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "particle checksum %{public}f", chk);
    destroyTarget(stateA);
    destroyTarget(stateB);
    destroyTarget(out);
    endLoad();
    return st;
}

// 7) Path Tracer: 1280x720, 1 spp/帧, 2 次反弹 + 余弦加权重要性采样, 累积到 RGBA32F
FrameStats loadPathTracer(int frames)
{
    FrameStats st;
    st.unit = "Mray/s";
    if (!beginLoad("Path Tracer")) {
        st.error = gpu7Error;
        return st;
    }
    const int W = 1280;
    const int H = 720;
    const int SPP = 1;
    const int BOUNCES = 2;
    // 累积缓冲用两张纹理乒乓: 路径追踪必须"读上一帧的累积 + 写这一帧的累积",
    // 读写同一张纹理就是反馈回路(GL 未定义行为)。乒乓后每个 pass 都读到上一帧
    // 写完的完整累积, 每帧的工作量不变(仍是 1 次 trace + 1 次绘制)。
    Target accumA;
    Target accumB;
    Target out;
    if (!createTarget(accumA, W, H, GL_RGBA32F) || !createTarget(accumB, W, H, GL_RGBA32F) ||
        !createTarget(out, W, H, GL_RGBA8)) {
        st.error = "path tracer fbo create failed: " + g_targetError;
        destroyTarget(accumA);
        destroyTarget(accumB);
        destroyTarget(out);
        endLoad();
        return st;
    }
    GLuint pTrace = getProg(P_PT_TRACE);
    GLuint pDraw = getProg(P_PT_DRAW);
    if (pTrace == 0 || pDraw == 0) {
        st.error = initError(true, "path tracer shader failed");
        destroyTarget(accumA);
        destroyTarget(accumB);
        destroyTarget(out);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, accumA.fbo);
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, accumB.fbo);
    glClear(GL_COLOR_BUFFER_BIT);

    const double perFrameRays = (double)W * (double)H * (double)SPP * (double)(BOUNCES + 1);
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        Target& srcAccum = (fr % 2 == 0) ? accumA : accumB;
        Target& dstAccum = (fr % 2 == 0) ? accumB : accumA;
        bindTarget(dstAccum);
        glUseProgram(pTrace);
        bindTex(0, srcAccum.tex);
        setI1(pTrace, "uAccum", 0);
        set2F(pTrace, "uRes", (float)W, (float)H);
        setF1(pTrace, "uFrame", (float)fr);
        setF1(pTrace, "uSpp", (float)SPP);
        drawFullscreen();

        bindTarget(out);
        glUseProgram(pDraw);
        bindTex(0, dstAccum.tex);
        setI1(pDraw, "uAccum", 0);
        drawFullscreen();
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(out.fbo, W, H, &chk);
    timer.finish(st, perFrameRays, "Mray/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "path tracer checksum %{public}f", chk);
    destroyTarget(accumA);
    destroyTarget(accumB);
    destroyTarget(out);
    endLoad();
    return st;
}
// 8) Photo Filter: 4000x3000 图像滤镜链(LUT 256 -> 饱和度 -> 3x3 锐化 -> 暗角 -> 褐色), 5 pass/帧
FrameStats loadPhotoFilter(int frames)
{
    FrameStats st;
    st.unit = "Mpx/s";
    if (!beginLoad("Photo Filter")) {
        st.error = gpu7Error;
        return st;
    }
    const int W = 4000;
    const int H = 3000;
    Target a;
    Target b;
    Target lut;
    if (!createTarget(a, W, H, GL_RGBA8) || !createTarget(b, W, H, GL_RGBA8) || !createTarget(lut, 256, 1, GL_RGBA8)) {
        st.error = "photo filter fbo create failed: " + g_targetError;
        destroyTarget(a);
        destroyTarget(b);
        destroyTarget(lut);
        endLoad();
        return st;
    }
    GLuint pScene = getProg(P_SCENE);
    GLuint pLut = getProg(P_PF_LUT);
    GLuint pSat = getProg(P_PF_SAT);
    GLuint pSharp = getProg(P_PF_SHARPEN);
    GLuint pVig = getProg(P_PF_VIGNETTE);
    GLuint pSepia = getProg(P_PF_SEPIA);
    if (pScene == 0 || pLut == 0 || pSat == 0 || pSharp == 0 || pVig == 0 || pSepia == 0) {
        st.error = initError(true, "photo filter shader failed");
        destroyTarget(a);
        destroyTarget(b);
        destroyTarget(lut);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    // 256 项 LUT 曲线(S 形), 由 SCENE 程序的噪声分支生成: 这里直接复用场景色直接写 LUT
    bindTarget(a);
    glUseProgram(pScene);
    setF1(pScene, "uSeed", 3.75f);
    drawFullscreen();
    // 用 LUT pass 把 a 的内容映射一次作为"曲线源", 保证 LUT 纹理有真实内容
    bindTarget(lut);
    glUseProgram(pLut);
    bindTex(0, a.tex);
    bindTex(1, a.tex);
    setI1(pLut, "uTex", 0);
    setI1(pLut, "uLut", 1);
    setF1(pLut, "uFrame", 1.0f);
    drawFullscreen();

    const double perFramePixels = (double)W * (double)H * 5.0;
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        // pass 1: LUT 曲线
        bindTarget(b);
        glUseProgram(pLut);
        bindTex(0, a.tex);
        bindTex(1, lut.tex);
        setI1(pLut, "uTex", 0);
        setI1(pLut, "uLut", 1);
        setF1(pLut, "uFrame", (float)fr);
        drawFullscreen();
        // pass 2: 饱和度
        bindTarget(a);
        glUseProgram(pSat);
        bindTex(0, b.tex);
        setI1(pSat, "uTex", 0);
        drawFullscreen();
        // pass 3: 3x3 锐化
        bindTarget(b);
        glUseProgram(pSharp);
        bindTex(0, a.tex);
        setI1(pSharp, "uTex", 0);
        set2F(pSharp, "uTexel", 1.0f / (float)W, 1.0f / (float)H);
        drawFullscreen();
        // pass 4: 暗角
        bindTarget(a);
        glUseProgram(pVig);
        bindTex(0, b.tex);
        setI1(pVig, "uTex", 0);
        drawFullscreen();
        // pass 5: 褐色调
        bindTarget(b);
        glUseProgram(pSepia);
        bindTex(0, a.tex);
        setI1(pSepia, "uTex", 0);
        drawFullscreen();
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(b.fbo, W, H, &chk);
    timer.finish(st, perFramePixels, "Mpx/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "photo filter checksum %{public}f", chk);
    destroyTarget(a);
    destroyTarget(b);
    destroyTarget(lut);
    endLoad();
    return st;
}

// 9) RAW: 合成 Bayer CFA(4000x3000) -> 双线性去马赛克 -> 白平衡 + 3x3 色彩矩阵 + gamma, 3 pass/帧
FrameStats loadRaw(int frames)
{
    FrameStats st;
    st.unit = "Mpx/s";
    if (!beginLoad("RAW")) {
        st.error = gpu7Error;
        return st;
    }
    const int W = 4000;
    const int H = 3000;
    Target bayer;
    Target a;
    Target b;
    if (!createTarget(bayer, W, H, GL_RGBA8) || !createTarget(a, W, H, GL_RGBA16F) ||
        !createTarget(b, W, H, GL_RGBA16F)) {
        st.error = "raw fbo create failed: " + g_targetError;
        destroyTarget(bayer);
        destroyTarget(a);
        destroyTarget(b);
        endLoad();
        return st;
    }
    GLuint pBayer = getProg(P_RAW_BAYER);
    GLuint pDemo = getProg(P_RAW_DEMOSAIC);
    GLuint pColor = getProg(P_RAW_COLOR);
    if (pBayer == 0 || pDemo == 0 || pColor == 0) {
        st.error = initError(true, "raw shader failed");
        destroyTarget(bayer);
        destroyTarget(a);
        destroyTarget(b);
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    bindTarget(bayer);
    glUseProgram(pBayer);
    setF1(pBayer, "uFrame", 1.0f);
    drawFullscreen();

    const double perFramePixels = (double)W * (double)H * 3.0;
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        // pass 1: 双线性去马赛克
        bindTarget(a);
        glUseProgram(pDemo);
        bindTex(0, bayer.tex);
        setI1(pDemo, "uBayer", 0);
        set2F(pDemo, "uTexel", 1.0f / (float)W, 1.0f / (float)H);
        drawFullscreen();
        // pass 2: 白平衡 + 3x3 色彩矩阵 + gamma
        bindTarget(b);
        glUseProgram(pColor);
        bindTex(0, a.tex);
        setI1(pColor, "uTex", 0);
        setF1(pColor, "uFrame", (float)fr);
        drawFullscreen();
        // pass 3: 曲线微调(把 b 回写到 a, 供下一帧继续使用)
        bindTarget(a);
        glUseProgram(pColor);
        bindTex(0, b.tex);
        setI1(pColor, "uTex", 0);
        setF1(pColor, "uFrame", (float)fr + 0.5f);
        drawFullscreen();
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(a.fbo, W, H, &chk);
    timer.finish(st, perFramePixels, "Mpx/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "raw checksum %{public}f", chk);
    destroyTarget(bayer);
    destroyTarget(a);
    destroyTarget(b);
    endLoad();
    return st;
}

// 10) Super Resolution: 960x540 -> 3840x2160, 3 层 3x3 卷积(16 通道, ReLU), 12 pass/帧
FrameStats loadSuperResolution(int frames)
{
    FrameStats st;
    st.unit = "Mpx/s";
    if (!beginLoad("Super Resolution")) {
        st.error = gpu7Error;
        return st;
    }
    const int IW = 960;
    const int IH = 540;
    const int OW = 3840;
    const int OH = 2160;
    const int LAYERS = 3;
    Target input;
    Target out;
    Target setA[4];
    Target setB[4];
    bool ok = createTarget(input, IW, IH, GL_RGBA8) && createTarget(out, OW, OH, GL_RGBA8);
    for (int i = 0; i < 4 && ok; ++i) {
        ok = createTarget(setA[i], OW, OH, GL_RGBA16F) && createTarget(setB[i], OW, OH, GL_RGBA16F);
    }
    GLuint pScene = getProg(P_SCENE);
    GLuint pConv = getProg(P_SR_CONV);
    if (!ok || pScene == 0 || pConv == 0) {
        st.error = initError(ok, "super resolution init failed");
        destroyTarget(input);
        destroyTarget(out);
        for (int i = 0; i < 4; ++i) {
            destroyTarget(setA[i]);
            destroyTarget(setB[i]);
        }
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    bindTarget(input);
    glUseProgram(pScene);
    setF1(pScene, "uSeed", 21.0f);
    drawFullscreen();

    const int PASSES = LAYERS * 4;
    const double perFramePixels = (double)OW * (double)OH * (double)PASSES;
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        // 乒乓: 第 0 层读 input 写 setA, 之后每层读"上一层写的那一组"、写另一组。
        // (原来 src 与 dst 都是 (layer%2==1 ? setB : setA), 即采样当前正在写入的
        //  纹理 —— 在 GL 里是未定义行为; 改成真正的乒乓后每层读到的都是上一层
        //  已经写完的完整结果, 最后一层的输出仍在 setA, 与下面的拷贝一致。)
        for (int layer = 0; layer < LAYERS; ++layer) {
            Target* dstSet = (layer % 2 == 1) ? setB : setA;
            Target* srcSet = (layer % 2 == 1) ? setA : setB;
            for (int c = 0; c < 4; ++c) {
                bindTarget(dstSet[c]);
                glUseProgram(pConv);
                bindTex(0, (layer == 0) ? input.tex : srcSet[0].tex);
                bindTex(1, (layer == 0) ? input.tex : srcSet[1].tex);
                bindTex(2, (layer == 0) ? input.tex : srcSet[2].tex);
                bindTex(3, (layer == 0) ? input.tex : srcSet[3].tex);
                setI1(pConv, "uIn0", 0);
                setI1(pConv, "uIn1", 1);
                setI1(pConv, "uIn2", 2);
                setI1(pConv, "uIn3", 3);
                set2F(pConv, "uTexel", 1.0f / (float)OW, 1.0f / (float)OH);
                set2F(pConv, "uOffset", (float)c * 0.05f, (float)layer * 0.02f);
                set2F(pConv, "uCh", (float)c, 0.0f);
                setF1(pConv, "uRelu", (layer < LAYERS - 1) ? 1.0f : 0.0f);
                setF1(pConv, "uFrame", (float)(fr * 4 + c));
                drawFullscreen();
            }
        }
        // 最后一层的结果拷贝到输出纹理
        bindTarget(out);
        glUseProgram(getProg(P_PF_SAT));
        GLuint pSat = getProg(P_PF_SAT);
        if (pSat != 0) {
            bindTex(0, setA[0].tex);
            setI1(pSat, "uTex", 0);
            drawFullscreen();
        }
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(out.fbo, OW, OH, &chk);
    timer.finish(st, perFramePixels, "Mpx/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "super resolution checksum %{public}f", chk);
    destroyTarget(input);
    destroyTarget(out);
    for (int i = 0; i < 4; ++i) {
        destroyTarget(setA[i]);
        destroyTarget(setB[i]);
    }
    endLoad();
    return st;
}

// 11) Video Filter: 1280x720, 5 帧时域降噪 -> 插帧(两时间权重) -> 锐化 -> 调色, 每帧多条子链
FrameStats loadVideoFilter(int frames)
{
    FrameStats st;
    st.unit = "Mpx/s";
    if (!beginLoad("Video Filter")) {
        st.error = gpu7Error;
        return st;
    }
    const int W = 1280;
    const int H = 720;
    const int RING = 4;
    const int SUBSTEPS = 8;
    Target ring[RING];
    Target a;
    Target b;
    Target blendTmp;
    bool ok = createTarget(a, W, H, GL_RGBA8) && createTarget(b, W, H, GL_RGBA8) &&
              createTarget(blendTmp, W, H, GL_RGBA8);
    for (int i = 0; i < RING && ok; ++i) {
        ok = createTarget(ring[i], W, H, GL_RGBA8);
    }
    GLuint pScene = getProg(P_SCENE);
    GLuint pDen = getProg(P_VF_DENOISE);
    GLuint pBlend = getProg(P_VF_BLEND);
    GLuint pSharp = getProg(P_VF_SHARP);
    GLuint pGrade = getProg(P_VF_GRADE);
    if (!ok || pScene == 0 || pDen == 0 || pBlend == 0 || pSharp == 0 || pGrade == 0) {
        st.error = initError(ok, "video filter init failed");
        destroyTarget(a);
        destroyTarget(b);
        destroyTarget(blendTmp);
        for (int i = 0; i < RING; ++i) {
            destroyTarget(ring[i]);
        }
        endLoad();
        return st;
    }
    glDisable(GL_BLEND);
    // 5 帧序列(程序内生成)
    for (int i = 0; i < RING; ++i) {
        bindTarget(ring[i]);
        glUseProgram(pScene);
        setF1(pScene, "uSeed", 31.0f + (float)i * 5.0f);
        drawFullscreen();
    }
    bindTarget(a);
    glUseProgram(pScene);
    setF1(pScene, "uSeed", 44.0f);
    drawFullscreen();

    // 每帧: 8 条子链 x (5 帧时域降噪 + 插帧 + 锐化 + 调色)
    const double perFramePixels = (double)W * (double)H * 5.0 * (double)SUBSTEPS;
    Timer timer;
    int total = kWarmup + frames;
    for (int fr = 0; fr < total; ++fr) {
        for (int sub = 0; sub < SUBSTEPS; ++sub) {
            int f0 = (sub) % RING;
            int f1i = (sub + 1) % RING;
            int f2 = (sub + 2) % RING;
            // pass 1: 5 帧时域降噪(带运动自适应权重)
            bindTarget(b);
            glUseProgram(pDen);
            bindTex(0, ring[f0].tex);
            bindTex(1, ring[f1i].tex);
            setI1(pDen, "uFrame", 0);
            setI1(pDen, "uPrev", 1);
            setF1(pDen, "uMotion", 0.65f + 0.05f * (float)(sub % 3));
            drawFullscreen();
            // pass 1b: 第二轮时域降噪, 参考帧换成 f2(5 帧窗口的第 3 帧)
            bindTarget(blendTmp);
            glUseProgram(pDen);
            bindTex(0, b.tex);
            bindTex(1, ring[f2].tex);
            setI1(pDen, "uFrame", 0);
            setI1(pDen, "uPrev", 1);
            setF1(pDen, "uMotion", 0.45f);
            drawFullscreen();
            // pass 2: 插帧(两个时间权重相加)
            bindTarget(b);
            glUseProgram(pBlend);
            bindTex(0, blendTmp.tex);
            bindTex(1, ring[f2].tex);
            setI1(pBlend, "uA", 0);
            setI1(pBlend, "uB", 1);
            setF1(pBlend, "uT", 0.35f + 0.05f * (float)(sub % 4));
            drawFullscreen();
            // pass 3: 锐化
            bindTarget(a);
            glUseProgram(pSharp);
            bindTex(0, b.tex);
            setI1(pSharp, "uTex", 0);
            set2F(pSharp, "uTexel", 1.0f / (float)W, 1.0f / (float)H);
            drawFullscreen();
            // pass 4: 调色(写回当前帧序列槽位, 供下一帧继续使用)
            bindTarget(ring[f0]);
            glUseProgram(pGrade);
            bindTex(0, a.tex);
            setI1(pGrade, "uTex", 0);
            setF1(pGrade, "uFrame", (float)(fr * SUBSTEPS + sub));
            drawFullscreen();
            // pass 5: 二次时域降噪(把当前结果与上一帧序列纹理再融合)
            bindTarget(b);
            glUseProgram(pDen);
            bindTex(0, ring[f0].tex);
            bindTex(1, ring[f1i].tex);
            setI1(pDen, "uFrame", 0);
            setI1(pDen, "uPrev", 1);
            setF1(pDen, "uMotion", 0.5f);
            drawFullscreen();
        }
        bindTarget(a);
        glUseProgram(getProg(P_PF_SAT));
        GLuint pSat = getProg(P_PF_SAT);
        if (pSat != 0) {
            bindTex(0, b.tex);
            setI1(pSat, "uTex", 0);
            drawFullscreen();
        }
        timer.frame();
        if (fr == kWarmup) {
            timer.start();
        }
    }
    float chk = 0.0f;
    checksumTex(a.fbo, W, H, &chk);
    timer.finish(st, perFramePixels, "Mpx/s", 1, 1.0 / 1.0e6);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "video filter checksum %{public}f", chk);
    destroyTarget(a);
    destroyTarget(b);
    destroyTarget(blendTmp);
    for (int i = 0; i < RING; ++i) {
        destroyTarget(ring[i]);
    }
    endLoad();
    return st;
}

// ---------------------------------------------------------------------------
// 负载注册表
// ---------------------------------------------------------------------------

struct LoadDef {
    const char* name;
    FrameStats (*fn)(int);
    int frames;   // 测量帧数(目标: 手机上每项 1~2 秒)
};

const LoadDef kLoads[] = {
    {"Background Blur", loadBlur, 6},
    {"Face Tracking", loadFaceTracking, 4},
    {"Feature Matching", loadFeatureMatching, 6},
    {"Fluid Simulation", loadFluid, 10},
    {"Horizon Detection", loadHorizon, 12},
    {"Particle Physics", loadParticles, 20},
    {"Path Tracer", loadPathTracer, 8},
    {"Photo Filter", loadPhotoFilter, 8},
    {"RAW", loadRaw, 6},
    {"Super Resolution", loadSuperResolution, 6},
    {"Video Filter", loadVideoFilter, 10},
};

const int kLoadCount = (int)(sizeof(kLoads) / sizeof(kLoads[0]));

// ---------------------------------------------------------------------------
// GPU 计分表(与 kLoads 同序): 单项分 = kUnit x (metric x conv)
//   kUnit = "每单位官方吞吐对应的官方单项分", 由 ref/gb7_gpu_dataset.json 里 8 台真实
//   CS1 GPU 结果的 11 项 x 单项分在对数域最小二乘拟合得到(等价于 score/吞吐 比值的
//   几何平均), 每项离散度 <= 0.22% —— k 是常数这一假设被数据证实, 不是抄近似值。
//   拟合样本(8 台, 原始页面在 ref/, 经 api.microlink.io 代理抓取):
//     LENOVO TB710FU (Adreno 750)          https://browser.geekbench.com/v7/gpu/159307
//     Apple M6 (Mac18,5)                   https://browser.geekbench.com/v7/gpu/177983
//     ASUS gfx1200 (RX 9060 XT)            https://browser.geekbench.com/v7/gpu/236347
//     MSI MS-7E26 (Intel Arc A750)         https://browser.geekbench.com/v7/gpu/242321
//     samsung SM-S948B (Adreno 840)        https://browser.geekbench.com/v7/gpu/248152
//     LENOVO 83DG (RTX 4060 Laptop)        https://browser.geekbench.com/v7/gpu/248160
//     MacBook Air M5 8c GPU (Mac17,3)      https://browser.geekbench.com/v7/gpu/82146
//     iPhone 17 Pro (A19 Pro)              https://browser.geekbench.com/v7/gpu/17894
//     (Feature Matching 用 7 台: /159307 该项官方给 0 分, 是异常样本)
//   官方明文只有校准这一条: "baseline score of 100,000 (which is the score of a Lenovo
//   Legion with an NVIDIA GeForce RTX 4060 GPU)" -- geekbench7-gpu-workloads.pdf
//   "总分 = 11 项几何平均" 不是官方明文, 是 7 台设备数值验证出的(见 gpu7Composite)。
//   单位口径: 计数类 K/M/G 是 1000 进位(用同一项里 pixels/sec 与 Kpixels/sec 混合显示的
//   设备互校得到 1000), 与本文件 metric 的 1e6/1e9 缩放一致。
// ---------------------------------------------------------------------------
struct GpuScoreDef {
    double kUnit;      // 官方单位口径的 k
    double conv;       // 官方单位值 = 本实现 metric x conv
    bool scored;       // false = 语义不可比, 分数恒 0
    const char* basis;
};

const GpuScoreDef kScores[] = {
    // 0 Background Blur
    {387.2534580, 1.0 / (1920.0 * 1080.0 * 2.0 / 1e6), true,
     "官方 images/sec(官方文档: 对 1080p 视频流逐帧做 DeepLabV3+ 分割后模糊); 换算 = 1/4.1472: "
     "本实现同为 1920x1080 一帧, metric = 宽x高x2 趟可分离高斯/1e6/秒, 故帧/秒 = metric/4.1472。"
     "k=387.2534580(8 台设备拟合, 离散度 0.09%)"},
    // 1 Face Tracking
    {806.6225258, 1.0 / (1920.0 * 1080.0 * 20.0 / 1e6), true,
     "官方 images/sec(RetinaFace 逐帧处理 1080p 视频流, 官方文档明示); 换算 = 1/41.472: "
     "本实现 1920x1080, metric = 宽x高x(4 级金字塔 x 5 次 LK 迭代)/1e6/秒 = 宽x高x20/1e6/秒, "
     "故帧/秒 = metric/41.472。k=806.6225258(离散度 0.13%)"},
    // 2 Feature Matching
    {0.0, 0.0, false,
     "未计分: 官方单位是 Mpixels/sec(两张照片里参与匹配的图像像素), 本实现 metric 是描述子"
     "对比较次数 Gpair/s(每帧 4096x256x16 对, 见本文件 DESC/TILE_H/TILES); '一对描述子等于"
     "多少像素'完全由本实现平铺参数的取值决定, 没有任何物理或官方可复核的定义, 且官方 "
     "TB710FU 该项为 0 分异常样本 -> 不硬凑, k=0, 不进入 GPU 复合分"},
    // 3 Fluid Simulation
    {67.86312732, 1.0 / (1024.0 * 1024.0 * 53.0 / 1e6), true,
     "官方 FPS; 换算 = 1/55.574528: 本实现每帧推进 1024x1024 网格 x 53 次单元更新(48 次 "
     "Jacobi 压力迭代 + 5 趟附加), metric = 单元更新数/1e6/秒, 故 FPS = metric/55.574528。"
     "假设: 官方 1 FPS 也是'一帧流体推进'(官方网格规模未公开) -> 仅同机纵向可比。"
     "k=67.86312732(离散度 0.03%)"},
    // 4 Horizon Detection
    {0.0, 0.0, false,
     "未计分: 官方单位是 pixels/sec(照片里的图像像素), 本实现 metric 是 Hough 投票数 Mvote/s"
     "(每帧 32 子步 x (16384 + 16384x180) 次投票); 投票数由本实现选择的子步数与角度/ρ 桶数"
     "决定, 与输入图像像素数没有可复核的固定关系(换算需要凭空假设'每个像素投多少票') -> "
     "不硬凑, k=0, 不进入 GPU 复合分"},
    // 5 Particle Physics
    {5.577846145, 1.0 / (1024.0 * 1024.0 / 2.0 / 1e6), true,
     "官方 FPS; 换算 = 1/0.524288: 本实现每帧更新 1024x1024/2 = 524288 个粒子(两次全网格"
     "状态更新, 口径常量 PER_FRAME = COUNT/2), metric = 粒子更新数/1e6/秒, 故"
     " FPS = metric/0.524288。假设: 官方 1 FPS = 一帧粒子推进(官方粒子数未公开) -> "
     "仅同机纵向可比。k=5.577846145(离散度 0.003%)"},
    // 6 Path Tracer
    {0.0005579768045, 1e6 / 3.0, true,
     "官方 pixels/sec; 换算 x333333.33: 本实现 metric = 1280x720 x 1 SPP x (2 次反弹+1) = "
     "每像素 3 条光线/1e6/秒(Mray/s); 光线与像素有确定的物理关系(每像素 SPP x (BOUNCES+1)), "
     "故像素/秒 = metric x 1e6/3。k=0.0005579768045/像素(离散度 0.18%)"},
    // 7 Photo Filter
    {60.97485462, 1.0 / 5.0, true,
     "官方页面写 pixels/sec, 但量级(10 万分基线 = 1640/秒, 各机 234~2130)与真实 GPU 像素吞吐"
     "差 5 个数量级, 而同一台设备的 RAW/Horizon/Video Filter 都带 M/G 前缀且量级正常 —— 唯一"
     "自洽的解释是该计数器以 1e6 像素为单位(页面漏印 M 前缀), 故 1 个官方单位 = 1e6 输出像素; "
     "换算 = metric/5(本实现 4000x3000 走 5 趟滤镜 = 每帧 5 次像素操作, 5 趟是对同一张图的"
     "同一批像素, 不重复计数 -> 除以 5 得到输出像素)。这是假设, 已列为风险。"
     "k=60.97485462(离散度 0.09%)"},
    // 8 RAW
    {0.00002474081196, 1e6 / 3.0, true,
     "官方 pixels/sec; 换算 x333333.33: 本实现 metric = 4000x3000 x 3 趟(去马赛克 / 白平衡+"
     "3x3 矩阵+gamma / 曲线微调)/1e6/秒; 3 趟是对同一张图的同一批像素的连续处理, 不重复计数 "
     "-> 输出像素/秒 = metric x 1e6/3。k=0.00002474081196/像素(离散度 0.22%)"},
    // 9 Super Resolution
    {0.5419356863, 1e6 / (12.0 * 1024.0 * 1024.0), true,
     "官方 images/sec, 官方文档明示一张 = 256x256 放大到 1024x1024(= 1.048576 Mpx 输出); "
     "换算 = 1/12.582912: 本实现每帧输出 3840x2160 且走 12 趟卷积(3 层 x 4 通道组), "
     "metric = 3840x2160x12/1e6/秒, 故官方口径的 图/秒 = 输出像素/秒 / 1.048576e6 "
     "= metric/12.582912(按官方公开的单张像素数归一, 避免用本实现 8 倍大的输出冒充一张图)。"
     "k=0.5419356863(离散度 0.14%)"},
    // 10 Video Filter
    {0.000003689001414, 1e6 / 40.0, true,
     "官方 pixels/sec(官方文档: 对短视频做 LUT 四面体插值调色); 换算 x25000: 本实现 "
     "metric = 1280x720 x 5 趟 x 8 子链/1e6/秒 = 每帧 40 次像素操作(同一批像素的连续处理, "
     "不重复计数)-> 输出像素/秒 = metric x 1e6/40。k=0.000003689001414/像素(离散度 0.22%)"},
};

static_assert(sizeof(kScores) / sizeof(kScores[0]) == sizeof(kLoads) / sizeof(kLoads[0]),
              "GPU 计分表必须与负载表一一对应");

// 最近一次 gpu7Run 的单项分(复合分在没传数组时用它, 方便 ArkTS 侧接线)
double g_lastScore[kLoadCount];
bool g_lastValid[kLoadCount];

void releaseGl()
{
    if (g_quadVbo != 0) {
        glDeleteBuffers(1, &g_quadVbo);
        g_quadVbo = 0;
    }
    if (g_quadVao != 0) {
        glDeleteVertexArrays(1, &g_quadVao);
        g_quadVao = 0;
    }
    if (g_pointVao != 0) {
        glDeleteVertexArrays(1, &g_pointVao);
        g_pointVao = 0;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

int gpu7Count()
{
    return kLoadCount;
}

std::string gpu7Name(int id)
{
    if (id < 0 || id >= kLoadCount) {
        return std::string();
    }
    return std::string(kLoads[id].name);
}

bool gpu7Ready()
{
    return g_ready;
}

std::string gpu7Prepare()
{
    std::string err = prepareInternal();
    if (!err.empty()) {
        gpu7Error = err;
        return err;
    }
    resetGlState();
    ensureQuad();
    if (g_quadVao == 0) {
        return "glGenVertexArrays failed";
    }
    return std::string();
}

std::string gpu7LastError()
{
    return gpu7Error;
}

std::string gpu7Run(int id)
{
    gpu7Error.clear();
    if (id < 0 || id >= kLoadCount) {
        return fail("bad load id " + fixed((double)id, 0));
    }
    if (g_ready && glIsProgram(0) == 0) {
        // 上下文丢失(进程切换/后台回收)后重新准备
        g_ready = false;
    }
    std::string err = gpu7Prepare();
    if (!err.empty()) {
        return fail(err);
    }
    FrameStats st = kLoads[id].fn(kLoads[id].frames);
    if (!st.ok) {
        return fail(st.error.empty() ? std::string("load failed") : st.error);
    }
    // 计分: 单项分 = kUnit x (metric x conv); 未计分项一律 0(原因写进 basis)
    const GpuScoreDef& sc = kScores[id];
    double score = 0.0;
    if (sc.scored && sc.kUnit > 0.0 && std::isfinite(st.metric) && st.metric > 0.0) {
        score = sc.kUnit * (st.metric * sc.conv);
    }
    if (!std::isfinite(score) || score < 0.0) {
        score = 0.0;
    }
    st.score = score;
    st.basis = sc.basis;
    g_lastScore[id] = score;
    g_lastValid[id] = (score > 0.0);
    return statsJson(st);
}

// ---------------------------------------------------------------------------
// GPU 复合分 = 已计分项的几何平均(11 项里 Feature Matching / Horizon Detection 未计分,
// 见各自 basis)。官方未公开几何平均规则, 这是 7 台设备 x 11 项数值验证出的规律。
// scores 传 nullptr / count<=0 时用最近一次 gpu7Run 的单项分。
// 返回 JSON: {"ok":true,"mode":"gpu","composite":..,"count":..,"items":[...],"basis":".."}
// ---------------------------------------------------------------------------
std::string gpu7Composite(const double* scores, int count)
{
    double prod = 1.0;
    int n = 0;
    std::string items;
    for (int i = 0; i < kLoadCount; ++i) {
        if (!kScores[i].scored) {
            continue;
        }
        double s = 0.0;
        if (scores != nullptr && count > i && i >= 0) {
            s = scores[i];
        } else if (g_lastValid[i]) {
            s = g_lastScore[i];
        }
        if (!(s > 0.0) || !std::isfinite(s)) {
            continue;
        }
        prod *= s;
        n++;
        char buf[192];
        snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"name\":\"%s\",\"score\":%.1f}",
                 items.empty() ? "" : ",", i, kLoads[i].name, s);
        items += buf;
    }
    const double geo = (n > 0) ? std::pow(prod, 1.0 / (double)n) : 0.0;
    char head[192];
    snprintf(head, sizeof(head),
             "{\"ok\":true,\"mode\":\"gpu\",\"composite\":%.1f,\"count\":%d,\"items\":[",
             std::isfinite(geo) ? geo : 0.0, n);
    std::string out = head;
    out += items;
    out += "],\"basis\":\"GPU 复合分 = 已计分项(11 项中 Feature Matching / Horizon Detection 因"
           "单位语义不可比而未计分)单项分的几何平均; 官方无明文, 系 8 台设备数值验证\"}";
    return out;
}

// ---------------------------------------------------------------------------
//  GPU 「跑满判据」(2026-10 追加) —— 计时区间之外, 同一份负载两档, 结果一律丢弃
// ---------------------------------------------------------------------------
//  为什么必须有它: CPU 侧早就有"占标称比中位 >= 90%"的跑满判据(靠运行时频率采样),
//  GPU 侧此前一个判据都没有 —— 于是"GPU 跑满了"这句话在本工程里从来没有证据支撑,
//  只有一句想当然。这一节把"能不能这么说"变成一条可复核的读数。
//
//  做法(用户指定的思路: 同一份着色器按不同工作量跑几档, 看吞吐是否进入平台期):
//    * 在正式计时之外, 用**同一个负载函数**(kLoads[id].fn, 同一份着色器、同一份尺寸、
//      同一份算法、一个字都没改)跑两档: 半量帧(max(2, frames/2))与满量帧(frames);
//    * 两档的耗时与读数一律丢弃 —— 它们不进 metric / score / 复合分, 只用于判据;
//    * 每一帧的墙钟在 Timer 里已经被切成"我们在 CPU 侧递交(+背压)"与"等 GPU 排水"两段,
//      于是有两条互相独立的证据:
//        证据①(限制器在哪一侧): 满量档里"等 GPU 排水"占每帧墙钟的比例。
//              这一条直接回答"帧时间是被 GPU 算掉的, 还是被我们自己在 CPU 侧的开销占掉的"。
//        证据②(进没进平台期): 满量吞吐 / 半量吞吐。加量之后吞吐还在涨 = 还没到平台期
//              (还有固定开销在被摊薄, 例如每帧的管线/状态切换), 说明当前读数不含"加量就能拿到的"
//              那部分; 涨不动了 = 已进平台期, 吞吐由硬件侧决定。
//
//  判据(阈值全部是我们自己定的, 写进文本, 不参与计分):
//    已达到      <=> 证据① 排水占比中位 >= 90% 且 证据② 两档吞吐比 >= 0.97;
//    未达到      <=> 至少一条不满足 —— 文本给出"还差几个百分点"与差在哪一条;
//    本项不适用  <=> 探测跑不起来 / 拿不到任何帧样本 / 两档分不开(帧数太少), 原因原样写出。
//
//  局限(必须一起写出来, 不许只报结论): 本判据回答的是"这一项在我们这条 GL 提交路径上
//  是不是被 GPU 排水占满了时间", 它**不能**回答"驱动是否把 GPU 降频了" —— 第三方 App
//  在 HarmonyOS 上读不到 GPU 频率节点, 本工程因此不假装知道 GPU 的工作频率。
const int    kGpuProbeMinFrames = 2;      // 探测档最少帧数
const double kGpuFullDutyPct    = 90.0;   // 证据① 阈值(我们自己定的)
const double kGpuPlateauRatio   = 0.97;   // 证据② 阈值(我们自己定的)

struct GpuProbeRung {
    bool ok = false;
    std::string error;
    int frames = 0;
    double ms = 0.0;
    double metric = 0.0;
    double issueMs = 0.0;
    double drainMs = 0.0;
    const char* unit = "";   // 负载自己报的单位(与正式那一遍同一个来源, 只用于文本)
};

GpuProbeRung gpuProbeRung(int id, int frames)
{
    GpuProbeRung r;
    if (frames <= 0) {
        r.error = "probe rung needs at least one frame";
        return r;
    }
    const FrameStats st = kLoads[id].fn(frames);
    if (!st.ok) {
        r.error = st.error.empty() ? std::string("load failed") : st.error;
        return r;
    }
    if (st.framesMeasured <= 0 || !(st.ms > 0.0)) {
        r.error = "probe rung produced no measured frame";
        return r;
    }
    r.ok = true;
    r.frames = st.framesMeasured;
    r.ms = st.ms;
    r.metric = st.metric;
    r.issueMs = st.cpuIssueMs;
    r.drainMs = st.gpuDrainMs;
    r.unit = st.unit;
    return r;
}

std::string gpu7Fullness(int id)
{
    if (id < 0 || id >= kLoadCount) {
        return "{\"ok\":false,\"error\":\"bad load id " + fixed((double)id, 0) + "\"}";
    }
    std::string err = gpu7Prepare();
    if (!err.empty()) {
        return "{\"ok\":false,\"error\":\"" + jsonSafe(err) + "\"}";
    }
    const int fullFrames = kLoads[id].frames;
    int halfFrames = fullFrames / 2;
    if (halfFrames < kGpuProbeMinFrames) {
        halfFrames = kGpuProbeMinFrames;
    }
    if (halfFrames >= fullFrames) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "GPU 跑满判据 -> 本项不适用(本项正式计时的测量帧数只有 %d 帧, 分不出'半量/满量'"
                 "两档, 因此无法判断吞吐有没有进平台期; 不是'跑满了', 也不是'没跑满')",
                 fullFrames);
        char out[768];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"loadId\":%d,\"name\":\"%s\",\"verdict\":\"NOT_APPLICABLE\","
                 "\"text\":\"%s\",\"probeHalfFrames\":%d,\"probeFullFrames\":%d}",
                 id, jsonSafe(kLoads[id].name).c_str(), jsonSafe(std::string(buf)).c_str(),
                 halfFrames, fullFrames);
        return std::string(out);
    }
    const GpuProbeRung half = gpuProbeRung(id, halfFrames);
    const GpuProbeRung full = gpuProbeRung(id, fullFrames);
    if (!half.ok || !full.ok) {
        const std::string why = !full.ok ? full.error : half.error;
        char buf[768];
        snprintf(buf, sizeof(buf),
                 "GPU 跑满判据 -> 本项不适用(原因: 探测那一档没跑起来 —— %s。判据要的读数"
                 "(每帧等 GPU 排水的时间 / 两档吞吐比)一个都没拿到, 因此这一项既不记'已达到'"
                 "也不记'未达到')",
                 jsonSafe(why).c_str());
        char out[1400];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"loadId\":%d,\"name\":\"%s\",\"verdict\":\"NOT_APPLICABLE\","
                 "\"text\":\"%s\",\"probeHalfFrames\":%d,\"probeFullFrames\":%d}",
                 id, jsonSafe(kLoads[id].name).c_str(), jsonSafe(std::string(buf)).c_str(),
                 halfFrames, fullFrames);
        return std::string(out);
    }
    const double fullPerFrameMs = full.ms / (double)full.frames;
    const double fullDrainPerFrame = full.drainMs / (double)full.frames;
    const double fullIssuePerFrame = full.issueMs / (double)full.frames;
    const double drainPct = (fullPerFrameMs > 0.0)
        ? (fullDrainPerFrame / fullPerFrameMs * 100.0) : 0.0;
    const double issuePct = (fullPerFrameMs > 0.0)
        ? (fullIssuePerFrame / fullPerFrameMs * 100.0) : 0.0;
    const double rungRatio = (half.metric > 0.0) ? (full.metric / half.metric) : 0.0;
    const bool dutyOk = (drainPct >= kGpuFullDutyPct);
    const bool plateauOk = (rungRatio >= kGpuPlateauRatio);
    const char* verdict = (dutyOk && plateauOk) ? "REACHED" : "NOT_REACHED";
    char buf[1600];
    if (dutyOk && plateauOk) {
        snprintf(buf, sizeof(buf),
                 "GPU 跑满判据 -> 已达到(在计时区间之外用同一份负载跑了两档, 读数一律丢弃: "
                 "半量 %d 帧 / 满量 %d 帧。满量档每帧墙钟 %.3f ms, 其中等 GPU 排水 %.3f ms = %.1f%%"
                 "(>= %.0f%%), 我们自己在 CPU 侧递交 GL 命令只占 %.1f%%; 加量之后吞吐 %.4g -> %.4g %s"
                 "(比 %.3f >= %.2f), 已进平台期 —— 这一项的吞吐是被 GPU 限制的, 不是被负载喂不饱) "
                 "[阈值 %.0f%% 与 %.2f 都是我们自己定的; 本判据不回答'驱动有没有给 GPU 降频'"
                 "—— 第三方 App 在 HarmonyOS 上读不到 GPU 频率节点, 本工程不假装知道]",
                 half.frames, full.frames, fullPerFrameMs, fullDrainPerFrame, drainPct,
                 kGpuFullDutyPct, issuePct, half.metric, full.metric, full.unit, rungRatio,
                 kGpuPlateauRatio, kGpuFullDutyPct, kGpuPlateauRatio);
    } else {
        char why[512];
        why[0] = '\0';
        if (!dutyOk && !plateauOk) {
            const double gap = kGpuFullDutyPct - drainPct;
            snprintf(why, sizeof(why),
                     "还差 %.1f 个百分点: 满量档每帧墙钟里等 GPU 排水只占 %.1f%%(阈值 %.0f%%), "
                     "另外 %.1f%% 是我们在 CPU 侧递交 GL 命令(+驱动背压); 且两档吞吐比 %.3f < %.2f, "
                     "加量之后吞吐还在涨 —— 平台期还没到, 当前读数里有我们自己的开销",
                     gap, drainPct, kGpuFullDutyPct, issuePct, rungRatio, kGpuPlateauRatio);
        } else if (!dutyOk) {
            const double gap = kGpuFullDutyPct - drainPct;
            snprintf(why, sizeof(why),
                     "还差 %.1f 个百分点: 满量档每帧墙钟里等 GPU 排水只占 %.1f%%(阈值 %.0f%%), "
                     "另外 %.1f%% 花在我们自己的 CPU 侧递交与驱动背压上 —— 瓶颈有一大半不在 GPU 上; "
                     "两档吞吐比 %.3f >= %.2f, 平台期本身是到了",
                     gap, drainPct, kGpuFullDutyPct, issuePct, rungRatio, kGpuPlateauRatio);
        } else {
            snprintf(why, sizeof(why),
                     "等 GPU 排水占比 %.1f%% 已过阈值 %.0f%%, 但两档吞吐比只有 %.3f < %.2f —— "
                     "加量之后吞吐还在涨, 说明还没进平台期(每帧还有固定开销在被摊薄), "
                     "当前这一档读数不代表这块 GPU 的稳态吞吐",
                     drainPct, kGpuFullDutyPct, rungRatio, kGpuPlateauRatio);
        }
        snprintf(buf, sizeof(buf),
                 "GPU 跑满判据 -> 未达到(%s) [在计时区间之外用同一份负载跑了两档: 半量 %d 帧 / "
                 "满量 %d 帧, 读数一律丢弃; 每帧墙钟 %.3f ms = 递交 %.3f ms + 等 GPU 排水 %.3f ms; "
                 "阈值 %.0f%% 与 %.2f 都是我们自己定的; 本判据不回答'驱动有没有给 GPU 降频']",
                 why, half.frames, full.frames, fullPerFrameMs, fullIssuePerFrame,
                 fullDrainPerFrame, kGpuFullDutyPct, kGpuPlateauRatio);
    }
    char out[3200];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"loadId\":%d,\"name\":\"%s\",\"verdict\":\"%s\",\"text\":\"%s\","
             "\"probeHalfFrames\":%d,\"probeFullFrames\":%d,\"probeHalfMetric\":%.4f,"
             "\"probeFullMetric\":%.4f,\"rungRatio\":%.4f,\"drainPct\":%.2f,\"issuePct\":%.2f,"
             "\"perFrameMs\":%.4f,\"drainPerFrameMs\":%.4f,\"issuePerFrameMs\":%.4f,"
             "\"dutyThresholdPct\":%.0f,\"plateauThreshold\":%.2f}",
             id, jsonSafe(kLoads[id].name).c_str(), verdict, jsonSafe(std::string(buf)).c_str(),
             half.frames, full.frames, half.metric, full.metric, rungRatio, drainPct, issuePct,
             fullPerFrameMs, fullDrainPerFrame, fullIssuePerFrame, kGpuFullDutyPct,
             kGpuPlateauRatio);
    return std::string(out);
}

// 上下文保活: 释放所有 GL 资源但保留 EGL 上下文(供上层显式调用, 不导出到 napi)
void gpu7Shutdown()
{
    if (!g_ready) {
        return;
    }
    releaseGl();
    releasePrograms();
}
