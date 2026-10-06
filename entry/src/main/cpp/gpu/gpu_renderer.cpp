#include "gpu_renderer.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <hilog/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kTag = "AuroraGpu";

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLConfig g_config = nullptr;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
void* g_window = nullptr;
int g_width = 0;
int g_height = 0;

// 固定渲染分辨率: 所有设备执行完全相同的像素工作量(与 3DMark 固定预设同理)
const int GPU_W = 1920;
const int GPU_H = 1080;

GLuint g_fbo = 0;
GLuint g_colorTex = 0;
GLuint g_depthRb = 0;

std::atomic<bool> g_surfaceReady{false};
std::atomic<bool> g_running{false};
std::atomic<bool> g_stopRequested{false};
std::atomic<int> g_sceneId{0};
std::atomic<int> g_framesDone{0};
std::atomic<int> g_framesTarget{0};
std::atomic<double> g_elapsedMs{0.0};
std::atomic<double> g_frameMs{0.0};

std::mutex g_resultMutex;
std::string g_finalJson;
std::thread g_thread;

void ensureFbo()
{
    if (g_fbo != 0) {
        return;
    }
    glGenTextures(1, &g_colorTex);
    glBindTexture(GL_TEXTURE_2D, g_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GPU_W, GPU_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &g_depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, GPU_W, GPU_H);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depthRb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "fbo incomplete %{public}u", (unsigned)st);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void presentToScreen()
{
    if (g_fbo == 0) {
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, GPU_W, GPU_H, 0, 0, g_width, g_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string num(double v, int digits)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return std::string(buf);
}

GLuint compileShader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "shader compile failed: %{public}s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint linkProgram(const char* vs, const char* fs)
{
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (v == 0 || f == 0) {
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "link failed: %{public}s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ---------------- 几何风暴 ----------------
const int GEO_INSTANCES = 24000;
const int GEO_VERTS = 36;

const char* GEO_VS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uVP;
uniform float uTime;
out vec3 vNormal;
out vec3 vColor;
void main() {
    float fi = float(gl_InstanceID);
    float a = fi * 0.00061 + uTime * 0.4;
    float ca = cos(a);
    float sa = sin(a);
    float ring = 2.5 + mod(fi, 113.0) * 0.055;
    float lift = mod(fi, 71.0) * 0.05 - 1.7;
    vec3 p = vec3(aPos.x * ca - aPos.z * sa, aPos.y, aPos.x * sa + aPos.z * ca);
    vec3 world = vec3(p.x + ring * ca, p.y + lift, p.z + ring * sa);
    vNormal = normalize(vec3(p.x, p.y, p.z));
    vColor = vec3(0.35 + 0.65 * abs(sin(fi * 0.11)), 0.45 + 0.4 * abs(cos(fi * 0.07)), 0.95);
    gl_Position = uVP * vec4(world, 1.0);
}
)GLSL";

const char* GEO_FS = R"GLSL(#version 300 es
precision highp float;
in vec3 vNormal;
in vec3 vColor;
out vec4 fragColor;
uniform vec3 uLight;
uniform float uTime;
void main() {
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, normalize(uLight)), 0.0);
    float rim = pow(1.0 - abs(n.z), 2.0);
    vec3 c = vColor * (0.22 + 0.78 * diff) + vec3(0.25, 0.45, 0.9) * rim;
    c = mix(c, c * 0.85 + vec3(0.05), 0.3 * abs(sin(uTime * 0.6)));
    fragColor = vec4(c, 1.0);
}
)GLSL";

// ---------------- 粒子填充 ----------------
const int PAR_COUNT = 180000;

const char* PAR_VS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aSeed;
uniform float uTime;
out vec4 vColor;
void main() {
    float t = uTime * 0.6 + aSeed * 6.283;
    vec3 p = aPos;
    p.x += sin(t * 1.7) * 1.2;
    p.y += cos(t * 1.3) * 1.0;
    p.z += sin(t * 0.9 + aSeed) * 1.0;
    gl_Position = vec4(p.x * 0.42, p.y * 0.42, p.z * 0.2, 1.0);
    gl_PointSize = 24.0;
    vColor = vec4(0.4 + 0.6 * abs(sin(aSeed * 9.0)), 0.6 + 0.4 * abs(cos(aSeed * 5.0)), 1.0, 1.0);
}
)GLSL";

const char* PAR_FS = R"GLSL(#version 300 es
precision highp float;
in vec4 vColor;
out vec4 fragColor;
void main() {
    vec2 d = gl_PointCoord - vec2(0.5);
    float r2 = dot(d, d);
    if (r2 > 0.25) {
        discard;
    }
    float fall = 1.0 - r2 * 4.0;
    fragColor = vec4(vColor.rgb * fall, fall * 0.55);
}
)GLSL";

// ---------------- 着色计算 ----------------
const char* ALU_VS = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* ALU_FS = R"GLSL(#version 300 es
precision highp float;
uniform float uTime;
uniform vec2 uRes;
out vec4 fragColor;
void main() {
    vec2 uv = gl_FragCoord.xy / uRes;
    vec3 acc = vec3(uv, 0.5 + 0.5 * sin(uTime));
    for (int i = 0; i < 32; i++) {
        float fi = float(i) + 1.0;
        vec3 s = vec3(sin(acc.x * fi + uTime), cos(acc.y * fi - uTime), sin((acc.x + acc.y) * fi * 0.5));
        acc = acc + s * 0.017;
        acc = mix(acc, acc.yzx, 0.27);
        acc = acc * 0.995 + vec3(0.002);
        float len = length(acc) + 0.001;
        acc = acc / len * 0.7;
    }
    fragColor = vec4(acc, 1.0);
}
)GLSL";

// ---------------- 并行计算 ----------------
const char* COMP_SRC = R"GLSL(#version 310 es
layout(local_size_x = 256) in;
layout(std430, binding = 0) buffer Data {
    float v[];
} data;
uniform uint uCount;
uniform float uTime;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uCount) {
        return;
    }
    float x = data.v[i] + uTime * 0.0001;
    for (int k = 0; k < 96; k++) {
        x = sin(x) * 1.0002 + cos(x * 0.5) * 0.0003;
    }
    data.v[i] = x;
}
)GLSL";

const int COMP_COUNT = 1048576;
const int COMP_ITERS_PER_FRAME = 2;

struct SceneName {
    const char* name;
};

const SceneName SCENES[] = {
    {"几何风暴"},
    {"粒子填充"},
    {"着色计算"},
    {"并行计算"},
};

const int SCENE_COUNT = 4;

// 参考基线(用于相对分数; 绝对物理量同时输出以保证跨平台可比)
const double REF_GEO_MTRI = 120.0;
const double REF_FILL_GPX = 8.0;
const double REF_ALU_GFLOPS = 60.0;
const double REF_COMPUTE_GOPS = 40.0;

GLuint buildCubeVbo()
{
    std::vector<float> data;
    data.reserve(GEO_VERTS * 6);
    const float faces[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };
    for (int f = 0; f < 6; ++f) {
        float nx = faces[f][0];
        float ny = faces[f][1];
        float nz = faces[f][2];
        float ux = (nx != 0.0f) ? 0.0f : 1.0f;
        float uy = (nx != 0.0f) ? 1.0f : ((ny != 0.0f) ? 0.0f : 1.0f);
        float uz = (nz != 0.0f) ? 0.0f : 1.0f;
        float vx = ny * uz - nz * uy;
        float vy = nz * ux - nx * uz;
        float vz = nx * uy - ny * ux;
        const float s = 0.5f;
        float corners[4][2] = {{-s, -s}, {s, -s}, {s, s}, {-s, s}};
        int tri[6] = {0, 1, 2, 0, 2, 3};
        for (int t = 0; t < 6; ++t) {
            int c = tri[t];
            float a = corners[c][0];
            float b = corners[c][1];
            float px = nx * s + ux * a + vx * b;
            float py = ny * s + uy * a + vy * b;
            float pz = nz * s + uz * a + vz * b;
            data.push_back(px);
            data.push_back(py);
            data.push_back(pz);
            data.push_back(nx);
            data.push_back(ny);
            data.push_back(nz);
        }
    }
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
    return vbo;
}

void setupCubeAttribs()
{
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
}

GLuint buildParticleVbo()
{
    std::vector<float> data;
    data.reserve((size_t)PAR_COUNT * 4);
    unsigned int s = 20260828u;
    for (int i = 0; i < PAR_COUNT; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        float x = (float)((int)(s % 2000) - 1000) * 0.001f;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        float y = (float)((int)(s % 2000) - 1000) * 0.001f;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        float z = (float)((int)(s % 2000) - 1000) * 0.001f;
        float seed = (float)(s % 1000) * 0.001f;
        data.push_back(x);
        data.push_back(y);
        data.push_back(z);
        data.push_back(seed);
    }
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
    return vbo;
}

void setupParticleAttribs()
{
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));
}

EGLSurface g_pbuffer = EGL_NO_SURFACE;
std::string g_errorText;

// 无窗口(离屏)初始化: GPU 基准本来就把场景渲染到 1920x1080 FBO,
// 不依赖任何屏幕画布, 因此不依赖 XComponent。
std::string initHeadlessInternal()
{
    if (g_display == EGL_NO_DISPLAY) {
        g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_display == EGL_NO_DISPLAY) {
            return "{\"ok\":false,\"error\":\"no egl display\"}";
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(g_display, &major, &minor) != EGL_TRUE) {
            g_display = EGL_NO_DISPLAY;
            return "{\"ok\":false,\"error\":\"eglInitialize failed\"}";
        }
        const EGLint cfgAttribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                     EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                     EGL_NONE};
        EGLint numConfig = 0;
        if (eglChooseConfig(g_display, cfgAttribs, &g_config, 1, &numConfig) != EGL_TRUE || numConfig < 1) {
            return "{\"ok\":false,\"error\":\"eglChooseConfig failed\"}";
        }
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctxAttribs);
        if (g_context == EGL_NO_CONTEXT) {
            return "{\"ok\":false,\"error\":\"eglCreateContext failed\"}";
        }
        const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        g_pbuffer = eglCreatePbufferSurface(g_display, g_config, pbAttribs);
        if (g_pbuffer == EGL_NO_SURFACE) {
            return "{\"ok\":false,\"error\":\"eglCreatePbufferSurface failed\"}";
        }
    }
    if (g_width <= 0) {
        g_width = GPU_W;
    }
    if (g_height <= 0) {
        g_height = GPU_H;
    }
    g_surfaceReady.store(true);
    const char* ver = (const char*)glGetString(GL_VERSION);
    (void)ver;
    return "{\"ok\":true}";
}

void setError(const std::string& msg)
{
    g_errorText = msg;
    g_finalJson = "{\"ok\":false,\"error\":\"" + msg + "\"}";
}

bool versionAtLeast31()
{
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (ver == nullptr) {
        return false;
    }
    return std::strstr(ver, "3.1") != nullptr || std::strstr(ver, "3.2") != nullptr;
}

struct SceneStats {
    double fps = 0.0;
    double frameMs = 0.0;
    double metricValue = 0.0;
    std::string metricUnit;
    double score = 0.0;
    std::string extra;
};

SceneStats runGeometry(int frames)
{
    SceneStats st;
    GLuint prog = linkProgram(GEO_VS, GEO_FS);
    if (prog == 0) {
        return st;
    }
    GLuint vbo = buildCubeVbo();
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    setupCubeAttribs();
    GLint uVP = glGetUniformLocation(prog, "uVP");
    GLint uTime = glGetUniformLocation(prog, "uTime");
    GLint uLight = glGetUniformLocation(prog, "uLight");
    glUseProgram(prog);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    ensureFbo();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, GPU_W, GPU_H);
    float aspect = (float)GPU_W / (float)GPU_H;
    float proj[16] = {1.0f / aspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float fov = 60.0f * 3.14159265f / 180.0f;
    float f = 1.0f / std::tan(fov * 0.5f);
    float nearZ = 0.1f;
    float farZ = 100.0f;
    float persp[16] = {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (farZ + nearZ) / (nearZ - farZ), -1, 0, 0,
                       (2.0f * farZ * nearZ) / (nearZ - farZ), 0};
    glUniform3f(uLight, 0.4f, 0.7f, 0.6f);
    glUniformMatrix4fv(uVP, 1, GL_FALSE, persp);
    glClearColor(0.02f, 0.03f, 0.06f, 1.0f);
    int warmup = 15;
    double t0 = 0.0;
    int measured = 0;
    double elapsed = 0.0;
    for (int fr = 0; fr < frames + warmup; ++fr) {
        if (g_stopRequested.load()) {
            break;
        }
        double t = (double)fr * 0.016;
        glUniform1f(uTime, (float)t);
        float ang = (float)t * 0.35f;
        float ca = std::cos(ang);
        float sa = std::sin(ang);
        float mv[16] = {ca, 0, -sa, 0, 0, 1, 0, 0, sa, 0, ca, 0, 0, -0.2f, -8.5f, 1};
        glUniformMatrix4fv(uVP, 1, GL_FALSE, persp);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindVertexArray(vao);
        glUseProgram(prog);
        glDrawArraysInstanced(GL_TRIANGLES, 0, GEO_VERTS, GEO_INSTANCES);
        double f0 = nowMs();
        if (g_surface != EGL_NO_SURFACE) {
            presentToScreen();
            eglSwapBuffers(g_display, g_surface);
        } else {
            glFinish();
        }
        double f1 = nowMs();
        (void)mv;
        if (fr == warmup) {
            t0 = f0;
        }
        if (fr >= warmup) {
            measured++;
            elapsed = f1 - t0;
        }
        g_framesDone.store(fr + 1);
        g_elapsedMs.store(elapsed);
        if (measured > 0) {
            g_frameMs.store(elapsed / (double)measured);
        }
    }
    if (measured > 0 && elapsed > 0.0) {
        st.frameMs = elapsed / (double)measured;
        st.fps = 1000.0 / st.frameMs;
        double mtri = (double)GEO_INSTANCES * 12.0 * st.fps / 1000000.0;
        st.metricValue = mtri;
        st.metricUnit = "Mtri/s";
        st.score = mtri / REF_GEO_MTRI * 1000.0;
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    return st;
}

SceneStats runParticles(int frames)
{
    SceneStats st;
    GLuint prog = linkProgram(PAR_VS, PAR_FS);
    if (prog == 0) {
        return st;
    }
    GLuint vbo = buildParticleVbo();
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    setupParticleAttribs();
    GLint uTime = glGetUniformLocation(prog, "uTime");
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    ensureFbo();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, GPU_W, GPU_H);
    int warmup = 15;
    double t0 = 0.0;
    int measured = 0;
    double elapsed = 0.0;
    for (int fr = 0; fr < frames + warmup; ++fr) {
        if (g_stopRequested.load()) {
            break;
        }
        glUseProgram(prog);
        glUniform1f(uTime, (float)((double)fr * 0.016));
        glClearColor(0.01f, 0.02f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindVertexArray(vao);
        glDrawArrays(GL_POINTS, 0, PAR_COUNT);
        double f0 = nowMs();
        if (g_surface != EGL_NO_SURFACE) {
            presentToScreen();
            eglSwapBuffers(g_display, g_surface);
        } else {
            glFinish();
        }
        double f1 = nowMs();
        if (fr == warmup) {
            t0 = f0;
        }
        if (fr >= warmup) {
            measured++;
            elapsed = f1 - t0;
        }
        g_framesDone.store(fr + 1);
        g_elapsedMs.store(elapsed);
        if (measured > 0) {
            g_frameMs.store(elapsed / (double)measured);
        }
    }
    if (measured > 0 && elapsed > 0.0) {
        st.frameMs = elapsed / (double)measured;
        st.fps = 1000.0 / st.frameMs;
        double px = (double)PAR_COUNT * 24.0 * 24.0;
        double gpx = px * st.fps / 1000000000.0;
        st.metricValue = gpx;
        st.metricUnit = "Gpx/s";
        st.score = gpx / REF_FILL_GPX * 1000.0;
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    return st;
}

SceneStats runAlu(int frames)
{
    SceneStats st;
    GLuint prog = linkProgram(ALU_VS, ALU_FS);
    if (prog == 0) {
        return st;
    }
    float verts[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    GLint uTime = glGetUniformLocation(prog, "uTime");
    GLint uRes = glGetUniformLocation(prog, "uRes");
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    ensureFbo();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, GPU_W, GPU_H);
    glUseProgram(prog);
    glUniform2f(uRes, (float)GPU_W, (float)GPU_H);
    int warmup = 15;
    double t0 = 0.0;
    int measured = 0;
    double elapsed = 0.0;
    for (int fr = 0; fr < frames + warmup; ++fr) {
        if (g_stopRequested.load()) {
            break;
        }
        glUseProgram(prog);
        glUniform1f(uTime, (float)((double)fr * 0.016));
        glClear(GL_COLOR_BUFFER_BIT);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        double f0 = nowMs();
        if (g_surface != EGL_NO_SURFACE) {
            presentToScreen();
            eglSwapBuffers(g_display, g_surface);
        } else {
            glFinish();
        }
        double f1 = nowMs();
        if (fr == warmup) {
            t0 = f0;
        }
        if (fr >= warmup) {
            measured++;
            elapsed = f1 - t0;
        }
        g_framesDone.store(fr + 1);
        g_elapsedMs.store(elapsed);
        if (measured > 0) {
            g_frameMs.store(elapsed / (double)measured);
        }
    }
    if (measured > 0 && elapsed > 0.0) {
        st.frameMs = elapsed / (double)measured;
        st.fps = 1000.0 / st.frameMs;
        double pixels = (double)GPU_W * (double)GPU_H;
        double opsPerPixel = 640.0;
        double gflops = pixels * opsPerPixel * st.fps / 1000000000.0;
        st.metricValue = gflops;
        st.metricUnit = "GFLOPS";
        st.score = gflops / REF_ALU_GFLOPS * 1000.0;
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    return st;
}

SceneStats runCompute(int frames)
{
    SceneStats st;
    if (!versionAtLeast31()) {
        st.extra = "unsupported";
        return st;
    }
    GLuint cs = compileShader(GL_COMPUTE_SHADER, COMP_SRC);
    if (cs == 0) {
        st.extra = "unsupported";
        return st;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(cs);
    if (!ok) {
        glDeleteProgram(prog);
        st.extra = "unsupported";
        return st;
    }
    std::vector<float> init((size_t)COMP_COUNT);
    for (int i = 0; i < COMP_COUNT; ++i) {
        init[(size_t)i] = (float)((i % 1000) * 0.001);
    }
    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(init.size() * sizeof(float)), init.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    GLint uCount = glGetUniformLocation(prog, "uCount");
    GLint uTime = glGetUniformLocation(prog, "uTime");
    GLuint groups = (GLuint)((COMP_COUNT + 255) / 256);
    int warmup = 8;
    double t0 = 0.0;
    int measured = 0;
    double elapsed = 0.0;
    for (int fr = 0; fr < frames + warmup; ++fr) {
        if (g_stopRequested.load()) {
            break;
        }
        double f0 = nowMs();
        glUseProgram(prog);
        glUniform1ui(uCount, (GLuint)COMP_COUNT);
        glUniform1f(uTime, (float)((double)fr * 0.016));
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
        for (int it = 0; it < COMP_ITERS_PER_FRAME; ++it) {
            glDispatchCompute(groups, 1, 1);
        }
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        glFinish();
        double f1 = nowMs();
        if (fr == warmup) {
            t0 = f0;
        }
        if (fr >= warmup) {
            measured++;
            elapsed = f1 - t0;
        }
        g_framesDone.store(fr + 1);
        g_elapsedMs.store(elapsed);
        if (measured > 0) {
            g_frameMs.store(elapsed / (double)measured);
        }
    }
    if (measured > 0 && elapsed > 0.0) {
        st.frameMs = elapsed / (double)measured;
        st.fps = 1000.0 / st.frameMs;
        double ops = (double)COMP_COUNT * 96.0 * 4.0 * (double)COMP_ITERS_PER_FRAME;
        double gops = ops * st.fps / 1000000000.0;
        st.metricValue = gops;
        st.metricUnit = "GOPS";
        st.score = gops / REF_COMPUTE_GOPS * 1000.0;
    }
    glDeleteBuffers(1, &ssbo);
    glDeleteProgram(prog);
    return st;
}

void renderThreadMain(int sceneId, int frames)
{
    if (!g_surfaceReady.load() || g_context == EGL_NO_CONTEXT) {
        setError("surface not ready");
        g_running.store(false);
        return;
    }
    EGLSurface target = (g_surface != EGL_NO_SURFACE) ? g_surface : g_pbuffer;
    if (target == EGL_NO_SURFACE || eglMakeCurrent(g_display, target, target, g_context) != EGL_TRUE) {
        setError("eglMakeCurrent failed");
        g_running.store(false);
        return;
    }
    SceneStats st;
    if (sceneId == 0) {
        st = runGeometry(frames);
    } else if (sceneId == 1) {
        st = runParticles(frames);
    } else if (sceneId == 2) {
        st = runAlu(frames);
    } else {
        st = runCompute(frames);
    }
    if (st.extra == "unsupported") {
        g_finalJson = "{\"ok\":true,\"scene\":" + std::to_string(sceneId) + ",\"unsupported\":true}";
    } else if (st.fps <= 0.0) {
        g_finalJson = "{\"ok\":false,\"error\":\"scene produced no frames\"}";
    } else {
        std::string name = SCENES[sceneId].name;
        g_finalJson = "{\"ok\":true,\"scene\":" + std::to_string(sceneId) +
            ",\"name\":\"" + name + "\",\"fps\":" + num(st.fps, 2) +
            ",\"frameMs\":" + num(st.frameMs, 3) +
            ",\"metric\":" + num(st.metricValue, 2) +
            ",\"unit\":\"" + st.metricUnit + "\",\"score\":" + num(st.score, 1) + "}";
    }
    g_running.store(false);
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

} // namespace

int gpuSceneCount()
{
    return SCENE_COUNT;
}

std::string gpuSceneName(int id)
{
    if (id < 0 || id >= SCENE_COUNT) {
        return "";
    }
    return SCENES[id].name;
}

bool gpuSurfaceReady()
{
    return g_surfaceReady.load();
}

void gpuSetWindow(void* nativeWindow, int width, int height)
{
    g_window = nativeWindow;
    g_width = width;
    g_height = height;
    if (g_display == EGL_NO_DISPLAY) {
        g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_display == EGL_NO_DISPLAY) {
            setError("no egl display");
            return;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(g_display, &major, &minor) != EGL_TRUE) {
            setError("eglInitialize failed");
            g_display = EGL_NO_DISPLAY;
            return;
        }
        const EGLint cfgAttribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                     EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                     EGL_NONE};
        EGLint numConfig = 0;
        if (eglChooseConfig(g_display, cfgAttribs, &g_config, 1, &numConfig) != EGL_TRUE || numConfig < 1) {
            setError("eglChooseConfig failed");
            return;
        }
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctxAttribs);
        if (g_context == EGL_NO_CONTEXT) {
            setError("eglCreateContext failed");
            return;
        }
    }
    if (g_window == nullptr) {
        return;
    }
    if (g_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_display, g_surface);
        g_surface = EGL_NO_SURFACE;
    }
    const EGLint surfAttribs[] = {EGL_NONE};
    g_surface = eglCreateWindowSurface(g_display, g_config, (EGLNativeWindowType)g_window, surfAttribs);
    if (g_surface == EGL_NO_SURFACE) {
        setError("eglCreateWindowSurface failed");
        return;
    }
    eglMakeCurrent(g_display, g_surface, g_surface, g_context);
    eglSwapInterval(g_display, 0);
    g_surfaceReady.store(true);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "surface ready %{public}dx%{public}d", width, height);
}

void gpuSetSize(int width, int height)
{
    g_width = width;
    g_height = height;
}

void gpuDestroySurface()
{
    g_stopRequested.store(true);
    if (g_thread.joinable()) {
        g_thread.join();
    }
    if (g_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_surface != EGL_NO_SURFACE) {
            eglDestroySurface(g_display, g_surface);
            g_surface = EGL_NO_SURFACE;
        }
    }
    g_surfaceReady.store(false);
}

std::string gpuStartScene(int sceneId, int frames)
{
    if (!g_surfaceReady.load()) {
        return "{\"ok\":false,\"error\":\"surface not ready\"}";
    }
    if (g_running.load()) {
        return "{\"ok\":false,\"error\":\"already running\"}";
    }
    if (sceneId < 0 || sceneId >= SCENE_COUNT) {
        return "{\"ok\":false,\"error\":\"bad scene\"}";
    }
    if (frames <= 0) {
        frames = 240;
    }
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_stopRequested.store(false);
    g_running.store(true);
    g_framesDone.store(0);
    g_framesTarget.store(frames);
    g_elapsedMs.store(0.0);
    g_frameMs.store(0.0);
    g_sceneId.store(sceneId);
    {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        g_finalJson.clear();
    }
    g_thread = std::thread(renderThreadMain, sceneId, frames);
    return "{\"ok\":true}";
}

std::string gpuPoll()
{
    bool running = g_running.load();
    int done = g_framesDone.load();
    int target = g_framesTarget.load();
    double elapsed = g_elapsedMs.load();
    double frameMs = g_frameMs.load();
    double fps = frameMs > 0.0 ? 1000.0 / frameMs : 0.0;
    std::string out = "{\"running\":" + std::string(running ? "true" : "false") +
        ",\"scene\":" + std::to_string(g_sceneId.load()) +
        ",\"framesDone\":" + std::to_string(done) +
        ",\"framesTarget\":" + std::to_string(target) +
        ",\"fps\":" + num(fps, 1) +
        ",\"frameMs\":" + num(frameMs, 3) +
        ",\"elapsedMs\":" + num(elapsed, 0);
    if (!running) {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        if (!g_finalJson.empty()) {
            std::string body = g_finalJson;
            if (body.size() > 1 && body[0] == '{') {
                body = body.substr(1, body.size() - 2);
            }
            out += "," + body;
        }
    }
    out += "}";
    return out;
}

void gpuStop()
{
    g_stopRequested.store(true);
}

std::string gpuInitHeadless()
{
    return initHeadlessInternal();
}

void gpuShutdown()
{
    g_stopRequested.store(true);
    if (g_thread.joinable()) {
        g_thread.join();
    }
    if (g_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_surface != EGL_NO_SURFACE) {
            eglDestroySurface(g_display, g_surface);
            g_surface = EGL_NO_SURFACE;
        }
        if (g_context != EGL_NO_CONTEXT) {
            eglDestroyContext(g_display, g_context);
            g_context = EGL_NO_CONTEXT;
        }
        eglTerminate(g_display);
        g_display = EGL_NO_DISPLAY;
    }
    g_surfaceReady.store(false);
}
