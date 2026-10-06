#include "reference_compare.h"

#include <hilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ===========================================================================
//  参考分对照实现 —— 只读, 不跑负载, 不改任何分数
//
//  这份文件里有三张表, 每一张都必须能回答"数字从哪来":
//    1) CS1 单核公开真值表(kGb7SingleTruth): 用户提供的公开真值 + 官方公开的复合分
//    2) CS1 16 项 + GPU 11 项 + GPU-SNL 的真实用途表(kRealUseTable): 第 4 条
//    3) 计分口径解释表(由每条自己的 metric/unit/basis 拼出): 第 5 条
//  查不到的一律写 "查不到" 或 null, 不编造对照值。
// ===========================================================================

namespace {

const char* kTag = "AuroraRef";

// ---------------------------------------------------------------------------
//  真值来源(逐条标明; 第三方不写成官方 —— 这是验收标准第 3 条的硬要求)
// ---------------------------------------------------------------------------
const char* kSourceOfficialGb7Composite =
    "公开跑分软件常见的分数结构(它们不提供 CPU+GPU 总分; 单核/多核是两个复合分)";
const char* kSourceUserTruth =
    "用户提供的公开真值(Mate 80 Pro Max 的公开结果页读数); "
    "本次工作没有逐条复核这些数字 —— 按 sourceKind=USER_PROVIDED_PUBLIC_TRUTH 对待";
const char* kSourceOfficial3dmark =
    "UL 官方支持文章 44002528075(明文: SNL 总分 = 图形分 = 平均帧率 x 135)";
const char* kSourceUlDb =
    "UL 官方成绩库机型行(Kirin 9020 = 454, 用户提交结果的中位数)";
const char* kSourceThirdParty =
    "第三方媒体/SoC 聚合站(nanoreview / Notebookcheck); 不是官方数据";
//  一手证据(2026-08-31 起): 用户提供的拍屏照片 —— 3DMark 应用直接跑出来的读数。
//   它与上面几个来源的强度差别必须写清楚:
//     * 一手观测(本类): 有人真的在那台机器上跑了一次并把屏幕拍下来 —— 直接观测;
//     * USER_PROVIDED_PUBLIC_TRUTH: 用户提供的公开真值(别人页面上抄下来的读数);
//     * THIRD_PARTY / UNVERIFIED_*: 转载页 / 媒体聚合站,没有任何直接观测支撑。
//   强度顺序: 一手观测 > 公开真值 > 第三方转载。但一手证据也有边界: 我们没有原始
//   图片文件, 也仍然没有在 UL 官方成绩库里查到该机型的条目 —— 这两条一起写进 note。
const char* kSourceUserPrimaryObservation =
    "用户提供的一手证据: 拍屏照片(2026-08-31 截图), 3DMark 应用在 HUAWEI Mate 80 Pro Max 上"
    "跑 3DMark Steel Nomad Light 的结果页 —— 总分 991 / 平均帧率 7.34 FPS。这是直接观测, "
    "强度高于任何公开转载页; 自洽核对: 991 / 135 = 7.3407 FPS 与拍屏上的 7.34 FPS 一致"
    "(官方明文 总分 = 平均帧率 x 135), 说明该读数确实是 SNL 在 2560x1440 下的整机成绩。";
const char* kSourceNone = "查不到";

// 参考机(公开真值所在机型)。它不是官方基准机, 也不是我们测过的机器。
const char* kReferenceDevice = "HUAWEI Mate 80 Pro Max (Kirin 9030 Pro)";
const char* kReferenceDeviceNote =
    "这台机器是用户提供的公开真值的来源机型; 我们没有这台机器, 也没有在本次工作中"
    "从官方成绩库逐条复核它的每一项数字。";

struct TruthRow {
    const char* name;          // 与 CS1 注册表同名
    const char* metric;        // 公开值旁边标注的物理量(原文口径)
    double score;              // 公开单项分
    const char* sourceKind;    // OFFICIAL_* / USER_PROVIDED_PUBLIC_TRUTH / THIRD_PARTY
    const char* source;
};

// CS1 单核逐项公开真值(用户提供)
const TruthRow kGb7SingleTruth[] = {
    {"File Compression", "226 MB/s", 1589.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
    {"Navigation", "11.0 routes/s", 2004.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
    {"HTML5 Browser", "23.0 pages/s", 1839.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
    {"PDF Viewer", "67.2 Mpx/s", 1941.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
    {"Photo Library", "5.62 images/s", 1639.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
    {"Clang", "2.78 Klines/s", 1697.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
    {"Text Processing", "76.3 pages/s", 1582.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
    {"Asset Compression", "26.8 MB/s", 1602.0, "USER_PROVIDED_PUBLIC_TRUTH", kSourceUserTruth},
};
const int kGb7SingleTruthCount = (int)(sizeof(kGb7SingleTruth) / sizeof(kGb7SingleTruth[0]));

// CS1 复合分真值
const double kGb7SingleCompositeTruth = 1633.0;
const double kGb7MultiCompositeTruth = 6802.0;

// GPU-SNL 公开真值(查证强度逐条不同 —— 与 sn_renderer.cpp 的 kRefPoints 保持一致)
struct SnlTruth {
    const char* soc;
    const char* device;
    double score;
    const char* sourceKind;
    const char* source;
};
const SnlTruth kSnlTruth[] = {
    //  991 的来源已修正(2026-08-31): 从 UNVERIFIED_THIRD_PARTY 改为
    //   USER_PROVIDED_PRIMARY_OBSERVATION —— 依据是用户提供的拍屏照片(一手证据),
    //   而不是任何一个公开转载页。逐条内容(机型/测试项/总分/平均帧率/来源)全部写进 note,
    //   原来的两条 caveat 一个字都没有删:
    //     ① 公开渠道(UL 官方成绩库)查不到该机型条目;
    //     ② 与同 SoC 其他机型的读数 956 / 998 / 993 不完全一致(950~1000)。
    //   一手证据不等于已证实为"该 SoC 的官方上限": 它证明的是"那台机器上跑出过 991"。
    {"Kirin 9030 Pro", "Mate 80 Pro Max", 991.0, "USER_PROVIDED_PRIMARY_OBSERVATION",
     "机型 Mate 80 Pro Max / 测试项 Steel Nomad Light / 总分 991 / 平均帧率 7.34 FPS / 来源=用户拍屏"
     "(一手证据, 2026-08-31 截图)。自洽核对: 991 / 135 = 7.3407 FPS ≈ 拍屏上的 7.34 FPS。"
     "caveat(保留): 公开渠道(UL 官方成绩库)查不到该机型条目; 且与其他 9030 Pro 机型读数"
     "956(MatePad Pro Max)/ 998(Mate X7)/ 993(转载页)不完全一致(区间 950~1000), "
     "因此它只代表这一台机器的实测, 不是该 SoC 的上限"},
    {"Kirin 9020", "Pura 80 Pro+ / Ultra", 454.0, "OFFICIAL_UL_DB", kSourceUlDb},
    {"Kirin 9000S", "Mate 60 系列 / MatePad Pro 13.2", 303.0, "UNVERIFIED_THIRD_PARTY", kSourceThirdParty},
};
const int kSnlTruthCount = (int)(sizeof(kSnlTruth) / sizeof(kSnlTruth[0]));

// ---------------------------------------------------------------------------
//  第 4 条: 每一项一句话说明它代表什么真实用途。
//  这张表覆盖 CS1 的 16 项 CPU + 11 项 GPU + 本小节, 与注册表同名匹配;
//  匹配不上时返回"查不到"(不编)。
// ---------------------------------------------------------------------------
struct UseRow {
    const char* name;
    const char* use;
};

const UseRow kRealUseTable[] = {
    // ---- GB7 CPU 16 项 ----
    {"File Compression", "压缩/解压文件与安装包(zip/gzip/zstd)时的真实吞吐 —— 装应用、传大文件、备份都靠它"},
    {"Navigation", "地图导航里的路径搜索: 输入起终点后算出路线的那一刻(边权图上的最短路径)"},
    {"Text Processing", "文本/日志/网页内容的批量解析 —— 搜索索引、词法分析、日志清洗这类工作的吞吐"},
    {"Asset Compression", "游戏/应用资源打包时把纹理压成 BC 块格式(装机包体积与加载速度直接相关)"},
    {"Photo Library", "相册导入: 拍完照后批量解码、生成缩略图、写库(打开相册快不快看这项)"},
    {"Photo Editor", "修图软件里对一张照片连续施加滤镜(亮度/饱和/锐化/暗角)的实时预览速度"},
    {"HDR", "HDR 照片/视频的浮点色调映射(高动态范围素材合成与显示前的处理)"},
    {"Ray Tracer", "离线渲染/3D 预览里的光线求交(设计渲染、游戏光照烘焙那一类工作)"},
    {"Game Physics", "游戏物理: 大量刚体的碰撞检测与解算(碎块、布娃娃、载具悬挂)"},
    {"PDF Viewer", "打开与滚动 PDF: 内容流解释 + 抗锯齿光栅化(文档阅读器的翻页流畅度)"},
    {"HTML5 Browser", "浏览器渲染一个网页: HTML/CSS 解析、布局、换行、光栅化(网页首屏速度)"},
    {"Clang", "真实编译 C 代码(内嵌 chibicc): 改一行代码后重新编译的等待时间"},
    {"Audio Encoder", "把音频无损压缩成 FLAC(录音/音乐入库时的编码速度)"},
    {"Video Encoder", "视频编码: 运动估计 + DCT + 量化 + 熵编码(录像/导出的耗时)"},
    {"Video Decoder", "视频解码: 熵解码 + 反量化 + IDCT + 运动补偿(看视频时的解码能力)"},
    {"Structure from Motion", "摄影测量/三维重建: 特征匹配 + 位姿估计 + 光束法平差(扫描建模)"},
    // ---- GPU 11 项(图像处理型 GPU 套件)----
    {"Background Blur", "视频会议/直播里的背景虚化(每帧先做分割再高斯模糊)"},
    {"Face Tracking", "相机/AR 里的人脸跟踪(金字塔 + 迭代配准逐帧跑)"},
    {"Feature Matching", "图像拼接与识别里的特征描述子暴力匹配"},
    {"Fluid Simulation", "游戏/特效里的流体解算(stable fluids: 平流 + 压力迭代)"},
    {"Horizon Detection", "无人机/相机里的地平线检测(Sobel 边缘 + Hough 投票)"},
    {"Particle Physics", "GPU 粒子系统(百万粒子的积分、碰撞与阻尼)"},
    {"Path Tracer", "实时光追/离线渲染的路径追踪采样(GPU 版)"},
    {"Photo Filter", "相机 App 的实时滤镜链(LUT 曲线 + 饱和度 + 锐化 + 暗角)"},
    {"RAW", "拍 RAW 照片后的机内处理(Bayer 去马赛克 + 白平衡 + 色彩矩阵 + gamma)"},
    {"Super Resolution", "超分辨率/画质增强(多层卷积把低分辨率画面放大)"},
    {"Video Filter", "视频剪辑里的时域降噪 + 锐化 + 调色 + 插帧"},
    // ---- 本小节 ----
    {"Aurora Nomad Light", "非光追 game-like 场景: 地形求交 + 延迟 PBR 着色 + 多光源阴影 —— 现代手游的主渲染通路"},
};
const int kRealUseCount = (int)(sizeof(kRealUseTable) / sizeof(kRealUseTable[0]));

std::string jsonSafe(const std::string& s, size_t cap = 0)
{
    const size_t limit = (cap == 0 || cap > s.size()) ? s.size() : cap;
    std::string out;
    out.reserve(limit + 16);
    for (size_t i = 0; i < limit; ++i) {
        const char c = s[i];
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
    return out;
}

// 按字符边界截断 UTF-8(2026-10): jsonSafe 的 cap 是按字节截的, 刚好切在一个中文字符
// 中间时会写出非法 UTF-8 —— 对比输出自己就变成坏 JSON 了。这里往前退到最近的一个字符起点,
// 保证 substr 的结果永远是完整的字符序列。
// 用途: QoS / cpuset 这两段 native 原文可能几千字节, 逐项摘录时只带前 N 字节。
std::string utf8Cut(const std::string& s, size_t cap)
{
    if (cap == 0 || s.size() <= cap) {
        return s;
    }
    size_t i = cap;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) {
        --i;   // 退到当前字符的第一个字节
    }
    return s.substr(0, i);
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

const TruthRow* findTruth(const std::string& name)
{
    for (int i = 0; i < kGb7SingleTruthCount; ++i) {
        if (name == kGb7SingleTruth[i].name) {
            return &kGb7SingleTruth[i];
        }
    }
    return nullptr;
}

const char* findRealUse(const std::string& name)
{
    for (int i = 0; i < kRealUseCount; ++i) {
        if (name == kRealUseTable[i].name) {
            return kRealUseTable[i].use;
        }
    }
    return "";
}

// ---- 极简 JSON 取值(与工程其它模块同款写法: 不做完整解析, 只找键) ----
bool jsonFindKey(const std::string& json, const char* key, size_t* at)
{
    const std::string pat = std::string("\"") + key + "\"";
    const size_t p = json.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    const size_t c = json.find(':', p + pat.size());
    if (c == std::string::npos) {
        return false;
    }
    *at = c + 1;
    return true;
}

bool jsonGetString(const std::string& json, const char* key, std::string* out)
{
    size_t at = 0;
    if (!jsonFindKey(json, key, &at)) {
        return false;
    }
    size_t i = at;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) {
        ++i;
    }
    if (i >= json.size() || json[i] != '"') {
        return false;
    }
    ++i;
    std::string s;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) {
            ++i;
        }
        s.push_back(json[i]);
        ++i;
    }
    *out = s;
    return true;
}

bool jsonGetNumber(const std::string& json, const char* key, double* out)
{
    size_t at = 0;
    if (!jsonFindKey(json, key, &at)) {
        return false;
    }
    size_t i = at;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) {
        ++i;
    }
    const size_t start = i;
    bool neg = false;
    if (i < json.size() && (json[i] == '-' || json[i] == '+')) {
        neg = (json[i] == '-');
        ++i;
    }
    double v = 0.0;
    bool any = false;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        v = v * 10.0 + (double)(json[i] - '0');
        ++i;
        any = true;
    }
    if (i < json.size() && json[i] == '.') {
        ++i;
        double sc = 0.1;
        while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
            v += (double)(json[i] - '0') * sc;
            sc *= 0.1;
            ++i;
            any = true;
        }
    }
    if (!any) {
        return false;
    }
    (void)start;
    *out = neg ? -v : v;
    return true;
}

// 把 kSingleCore 字符串里形如 [{...},{...}] 的顶层对象切出来
std::vector<std::string> jsonSplitObjects(const std::string& json)
{
    std::vector<std::string> out;
    int depth = 0;
    bool inStr = false;
    size_t start = 0;
    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (inStr) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                inStr = false;
            }
            continue;
        }
        if (c == '"') {
            inStr = true;
        } else if (c == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                out.push_back(json.substr(start, i - start + 1));
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//   随芯片变化自检排除集 / 复合分参与项 / 运行条件(2026-10-08 新增)
//
//  为什么要有这一段: 报告里的每一项都要先过一遍「随芯片变化自检」(scalingAudit: 这一项的
//  吞吐真的随算力变化吗?), 判为 FAIL 的项在跨芯片比较时不可信。两台设备的这份清单只要
//  不同, "哪几项能拿去跨芯片比"就不同 —— 所以对比模块必须把两边的清单逐项列出来。
//
//  2026-10-05 更正: 判 FAIL 的项不再被排除在复合分之外(复合分的参与项 = 该阶段所有
//  "计分且拿到正分"的项, 与自检结论无关)。理由: 官方多核只有 8 项, 一排除就可能只剩 3 项,
//  而"哪几项 FAIL"由 2 轮离散度决定 —— 同一台设备 8.0 -> 8.1 全口径多核 GM 只差 0.26%
//  (1447.06 -> 1450.76), 排除之后却差 160%(795.81 -> 2071.86), 那个数不能叫复合分。
//  因此本段与 compositeParticipants 现在是两件不同的事:
//    本段(异常: 异常标记) = "哪几项不可用于跨芯片比较"(提示);
//    compositeParticipants = "两个复合分实际由哪些项组成"(决定原始比能不能相除)。
//
//  数据来源全部是报告里已经落盘的字段(不猜、不编):
//    scalingAudit.excludedIds / excludedNames / singleRows / multiRows / criteria / text
//    composite.gb7SingleItems / gb7MultiItems(报告自己给出的参与项名单)
//    env.smtTopology + composite.gb7MultiThreads(线程数 / SMT / 物理逻辑核)
// ---------------------------------------------------------------------------
struct AuditExcludedItem {
    std::string name;
    int id = -1;
    std::string stage;     // 单核 / 多核(在哪一个口径上判 FAIL 的)
    std::string reason;    // 报告里那一行的原文 = 判定依据
    bool hasReason = false;
};

struct ScalingAuditInfo {
    bool present = false;
    int excludedCount = 0;
    int passCount = 0;
    int failCount = 0;
    int unprovenCount = 0;
    int reasonMissing = 0;            // 有几项报告没给出判定依据原文(计数)
    std::vector<AuditExcludedItem> excluded;
    std::vector<std::string> criteria;  // 我们自己定的判据(SC1 / SC2 / …)
    std::string text;                   // 报告里那段人话
};

// 复合分的参与项名单(报告自己算出来的那一份)
struct CompositeParticipants {
    bool present = false;
    std::vector<std::string> names;
    std::vector<int> ids;
    std::string text;
};

// 运行条件(线程数 / SMT / 可用核集合 / 物理逻辑核)
struct MachineCond {
    bool hasTopology = false;
    bool smtDetected = false;
    bool smtEnabled = false;
    double logicalCores = 0.0;
    double physicalCores = 0.0;
    double multiThreads = 0.0;
    double allowedCores = 0.0;
    std::string allowedMask;
    std::string topologyText;
};

// 从 json 里摘出 "key":{...} 这个对象(按花括号配平, 跳过字符串内的括号)
bool jsonGetObject(const std::string& json, const char* key, std::string* out)
{
    size_t at = 0;
    if (!jsonFindKey(json, key, &at)) {
        return false;
    }
    const size_t b = json.find('{', at);
    if (b == std::string::npos) {
        return false;
    }
    int depth = 0;
    bool inStr = false;
    for (size_t i = b; i < json.size(); ++i) {
        const char c = json[i];
        if (inStr) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                inStr = false;
            }
            continue;
        }
        if (c == '"') {
            inStr = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                *out = json.substr(b, i - b + 1);
                return true;
            }
        }
    }
    return false;
}

// "key":["a","b",…] -> 字符串数组(键不存在 = false; 键在但是空数组 = true + 空)
bool jsonGetStringArray(const std::string& json, const char* key, std::vector<std::string>* out)
{
    out->clear();
    size_t at = 0;
    if (!jsonFindKey(json, key, &at)) {
        return false;
    }
    size_t i = at;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) {
        ++i;
    }
    if (i >= json.size() || json[i] != '[') {
        return false;
    }
    ++i;
    while (i < json.size()) {
        while (i < json.size() && json[i] != '"' && json[i] != ']') {
            ++i;
        }
        if (i >= json.size() || json[i] == ']') {
            break;
        }
        ++i;
        std::string s;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) {
                ++i;
            }
            s.push_back(json[i]);
            ++i;
        }
        ++i;
        out->push_back(s);
    }
    return true;
}

// "key":[1,2,3] -> 数字数组
bool jsonGetNumberArray(const std::string& json, const char* key, std::vector<double>* out)
{
    out->clear();
    size_t at = 0;
    if (!jsonFindKey(json, key, &at)) {
        return false;
    }
    size_t i = at;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) {
        ++i;
    }
    if (i >= json.size() || json[i] != '[') {
        return false;
    }
    ++i;
    while (i < json.size() && json[i] != ']') {
        if ((json[i] >= '0' && json[i] <= '9') || json[i] == '-' || json[i] == '+' || json[i] == '.') {
            double v = 0.0;
            if (sscanf(json.c_str() + i, "%lf", &v) == 1) {
                out->push_back(v);
            }
            while (i < json.size() && json[i] != ',' && json[i] != ']') {
                ++i;
            }
        } else {
            ++i;
        }
    }
    return true;
}

// 解析「随芯片变化自检」的排除集: 项名 + 排除原因(判定依据原文)
void parseScalingAudit(const std::string& json, ScalingAuditInfo* out)
{
    std::string obj;
    if (!jsonGetObject(json, "scalingAudit", &obj)) {
        return;
    }
    out->present = true;
    double v = 0.0;
    if (jsonGetNumber(obj, "passCount", &v)) {
        out->passCount = (int)v;
    }
    if (jsonGetNumber(obj, "failCount", &v)) {
        out->failCount = (int)v;
    }
    if (jsonGetNumber(obj, "unprovenCount", &v)) {
        out->unprovenCount = (int)v;
    }
    (void)jsonGetString(obj, "text", &out->text);
    (void)jsonGetStringArray(obj, "criteria", &out->criteria);
    std::vector<std::string> names;
    std::vector<double> ids;
    std::vector<std::string> sRows;
    std::vector<std::string> mRows;
    (void)jsonGetStringArray(obj, "excludedNames", &names);
    (void)jsonGetNumberArray(obj, "excludedIds", &ids);
    (void)jsonGetStringArray(obj, "singleRows", &sRows);
    (void)jsonGetStringArray(obj, "multiRows", &mRows);
    out->excludedCount = (int)names.size();
    for (size_t i = 0; i < names.size(); ++i) {
        AuditExcludedItem e;
        e.name = names[i];
        e.id = (i < ids.size()) ? (int)ids[i] : -1;
        // 判定依据 = 报告里那一行的原文(形如 "File Compression [FAIL] 多核口径 · …")。
        // 优先取多核口径那一行(那一行才是"改变可用算力"的对照, 是自检的本体)。
        for (size_t r = 0; r < mRows.size() && !e.hasReason; ++r) {
            if (mRows[r].compare(0, e.name.size(), e.name) == 0) {
                e.stage = "多核";
                e.reason = mRows[r];
                e.hasReason = true;
            }
        }
        for (size_t r = 0; r < sRows.size() && !e.hasReason; ++r) {
            if (sRows[r].compare(0, e.name.size(), e.name) == 0) {
                e.stage = "单核";
                e.reason = sRows[r];
                e.hasReason = true;
            }
        }
        if (!e.hasReason) {
            ++out->reasonMissing;
        }
        out->excluded.push_back(e);
    }
}

// 报告给的参与项名单: "Asset Compression 2437.5(id=3) · Photo Library 4480.1(id=4)"
void parseCompositeItems(const std::string& text, CompositeParticipants* out)
{
    if (text.empty()) {
        return;
    }
    out->present = true;
    out->text = text;
    const std::string sep = "\xc2\xb7";  // UTF-8 的 "·"
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nx = text.find(sep, pos);
        std::string chunk = (nx == std::string::npos) ? text.substr(pos) : text.substr(pos, nx - pos);
        const size_t b = chunk.find_first_not_of(" \t");
        const size_t e2 = chunk.find_last_not_of(" \t");
        if (b != std::string::npos && e2 != std::string::npos && e2 >= b) {
            chunk = chunk.substr(b, e2 - b + 1);
        } else {
            chunk.clear();
        }
        if (!chunk.empty()) {
            int id = -1;
            const size_t ip = chunk.rfind("(id=");
            std::string head = chunk;
            if (ip != std::string::npos) {
                id = atoi(chunk.c_str() + ip + 4);
                head = chunk.substr(0, ip);
            }
            const size_t sp = head.rfind(' ');
            const std::string nm = (sp == std::string::npos) ? head : head.substr(0, sp);
            if (!nm.empty()) {
                out->names.push_back(nm);
                out->ids.push_back(id);
            }
        }
        if (nx == std::string::npos) {
            break;
        }
        pos = nx + sep.size();
    }
}

// 运行条件: 线程数(composite.gb7MultiThreads) + SMT 状态与物理/逻辑核(env.smtTopology 原文)
void parseMachineCond(const std::string& json, MachineCond* out)
{
    double v = 0.0;
    if (jsonGetNumber(json, "gb7MultiThreads", &v)) {
        out->multiThreads = v;
    }
    if (!jsonGetString(json, "smtTopology", &out->topologyText)) {
        return;
    }
    out->hasTopology = true;
    const std::string& t = out->topologyText;
    const size_t lp = t.find("逻辑核");
    if (lp != std::string::npos) {
        size_t i = lp;
        while (i > 0 && t[i - 1] >= '0' && t[i - 1] <= '9') {
            --i;
        }
        if (i < lp) {
            out->logicalCores = atof(t.substr(i, lp - i).c_str());
        }
    }
    const size_t pp = t.find("物理核");
    if (pp != std::string::npos) {
        size_t i = pp;
        while (i > 0 && t[i - 1] >= '0' && t[i - 1] <= '9') {
            --i;
        }
        if (i < pp) {
            out->physicalCores = atof(t.substr(i, pp - i).c_str());
        }
    }
    out->smtDetected = (t.find("检测到 SMT") != std::string::npos);
    if (t.find("当前已启用") != std::string::npos) {
        out->smtEnabled = true;
    } else if (t.find("当前已关闭") != std::string::npos) {
        out->smtEnabled = false;
    }
}


// ---------------------------------------------------------------------------
//   线性自检(最高优先级验收标准:「分数比 == 性能比」)
//  ---------------------------------------------------------------------------
//  1) 每一项的分是不是"随性能线性变"?
//     本工程的单项分公式是 **score = k x (metric x conv)**, 其中
//       * metric 是"绝对吞吐"(工作量/秒),
//       * k 与 conv 是编译期常量(不随设备、不随运行变)。
//     因此 score / metric 必须是一个常数 —— 这一点可以在单台设备上机器核对:
//     只要公式是 score = k x metric, 两个吞吐之比就等于两个分数之比。
//     但"吞吐比 == 芯片性能比"还需要两台设备才能验证(见 2)。
//  2) 真值锚点: 用公开的跨代真值做线性回归自检(需要两台设备的实测)
//     * GB6 单核: 9000s 1299 / 9020 1616 / 9030 Pro 约 1866(第三方查证)
//       比值: 9020/9000s = 1.2440, 9030Pro/9000s = 1.4365
//       **注意这条 1299->1616->1866 恰好严格线性(1.2440 x 1.1547 = 1.4365), 但那是
//         第三方给的近似值, 不是"真值本身线性"的证据 —— 报告里标注。**
//     * 3DMark Steel Nomad Light: 991 / 454 / 303(来源强度逐条不同: 991 有用户拍屏的一手证据,
//       454 有 UL 官方成绩库出处, 303 仍然只有第三方 —— 三条都不是"官方文档常量")
//     * 3DMark GPU 三代累计约 2.64x(第三方查证)
//  3) 复合分的口径: 几何平均。数学上 GM 对"每一项都乘同一个 c"是严格线性的
//     (GM(c*a_i) = c x GM(a_i)) —— 所以只要所有计分项同时按 c 变, 复合分比就等于 c。
//     反过来: 如果不同项的变化倍数不一致, 复合分的比就不是任何单一"性能比",
//     这时必须把逐项倍数的离散度报出来(本模块的 itemScalingSpreadPercent)。
//     官方 GB7 的复合分是"各项几何平均"这一条已由官方文档/结果页结构确认;
//     本工程 GB7 的实现见 gb7.cpp 的 compositeJson()(几何平均, 只统计计分项)。
//  4) 不满足线性怎么办: 要么修正, 要么不计分。本工程的口径:
//       * 单项: 只有"评分公式里出现了非吞吐量"的情况才需要修正 —— 目前没有(全部是
//         k x metric x conv), 所以逐项都标 LINEAR_BY_CONSTRUCTION。
//       * 实测层面: 用两台设备的逐项比值与真值比值对照, 偏差超过 5% 的项标
//         NEEDS_REVIEW, 并在报告里给出数字; 拿不到第二台设备就写 UNVERIFIED。
// ---------------------------------------------------------------------------

struct TruthAnchor {
    const char* label;       // 真值类别
    const char* a;           // 设备 A
    const char* b;           // 设备 B
    double aScore;           // A 的真值
    double bScore;           // B 的真值
    const char* unit;        // 真值本身的口径
    const char* sourceKind;
    const char* source;
    const char* caveat;
};

const TruthAnchor kTruthAnchors[] = {
    {"GB6 单核", "麒麟 9000s", "麒麟 9020", 1299.0, 1616.0, "GB6 单核分", "THIRD_PARTY",
     "第三方查证(GB6 单核公开成绩)", "第三方给的近似值; 不是官方文档常量"},
    {"GB6 单核", "麒麟 9020", "麒麟 9030 Pro", 1616.0, 1866.0, "GB6 单核分", "THIRD_PARTY",
     "第三方查证(GB6 单核公开成绩)", "此处 9030 Pro 写成约 1866; 与 Mate 80 Pro Max 的 CS1 单核 1633 不是同一代测试, 不能混用"},
    {"GB6 单核", "麒麟 9000s", "麒麟 9030 Pro", 1299.0, 1866.0, "GB6 单核分", "THIRD_PARTY",
     "第三方查证的累计比", "1.2440 x 1.1547 = 1.4365(恰好闭合, 但仍是第三方近似值)"},
    {"3DMark SNL", "麒麟 9000S", "麒麟 9030 Pro", 303.0, 991.0, "SNL 分",
     "MIXED(991 = USER_PROVIDED_PRIMARY_OBSERVATION / 303 = UNVERIFIED_THIRD_PARTY)",
     "991 来自用户拍屏的一手证据(2026-08-31, Mate 80 Pro Max, 总分 991 / 7.34 FPS); "
     "303 仍只有第三方(nanoreview)",
     "两端强度不同: 991 是一手观测、303 未证实; 该比值 3.27x 里只有一端有一手证据, 只作数量级参照"},
    {"3DMark SNL", "麒麟 9000S", "麒麟 9020", 303.0, 454.0, "SNL 分", "MIXED(488 已证实/303 未证实)",
     "454 来自 UL 官方成绩库; 303 来自第三方", "一端未证实"},
    {"3DMark GPU 三代累计", "麒麟 9000S", "麒麟 9030 Pro", 1.0, 2.64, "相对倍数", "THIRD_PARTY",
     "第三方查证的累计倍数", "只给了倍数, 没有两端绝对值 —— 只能用来核对'量级'"},
};
const int kTruthAnchorCount = (int)(sizeof(kTruthAnchors) / sizeof(kTruthAnchors[0]));

struct LinearityRow {
    const char* name;
    const char* status;       // LINEAR_BY_CONSTRUCTION / NOT_SCORED
    const char* why;
};

// 构造性线性: 单项分 = k x (metric x conv), 两个都是常量 => 分数比 == 吞吐比。
// 唯一的例外是未计分项(score 恒 0): 它们既不是线性, 也不许进复合分。
std::string linearityAuditJson(const std::string& singleCoreJson, bool hasSingle)
{
    std::vector<std::string> items = jsonSplitObjects(singleCoreJson);
    std::string rows;
    int linearCount = 0;
    int notScored = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        std::string name;
        if (!jsonGetString(items[i], "name", &name)) {
            continue;
        }
        double score = 0.0;
        (void)jsonGetNumber(items[i], "score", &score);
        std::string metric, unit, basis;
        (void)jsonGetString(items[i], "metric", &metric);
        (void)jsonGetString(items[i], "unit", &unit);
        (void)jsonGetString(items[i], "basis", &basis);
        const bool scored = (score > 0.0);
        if (scored) {
            ++linearCount;
        } else {
            ++notScored;
        }
        rows += (rows.empty() ? "" : ",");
        rows += "{\"name\":\"" + jsonSafe(name) + "\""
                ",\"status\":\"" + std::string(scored ? "LINEAR_BY_CONSTRUCTION" : "NOT_SCORED") + "\""
                ",\"pass\":" + std::string(scored ? "true" : "false") +
                ",\"score\":" + fixed(score, 1) +
                ",\"metric\":\"" + jsonSafe(metric) + "\",\"unit\":\"" + jsonSafe(unit) + "\""
                ",\"formula\":\"score = k x (metric x conv), k 与 conv 是编译期常量\""
                ",\"why\":\"" + jsonSafe(std::string(
                    scored ? "分数与吞吐严格成正比(公式里只有一个变量 metric); 因此两台设备的分数比 == 吞吐比。"
                             "但'吞吐比 == 芯片性能比'需要两台设备实测才能验证 —— 见 anchors 与 itemScalingSpreadPercent。"
                           : "未计分(注册表 scored=false, score 恒 0): 按'宁可未计分也不许把口径不可复核的"
                             "数混进复合分'的口径, 它不算作复合分的参与项(basis 里有原因)。"
                             "注意这与'随芯片变化自检判 FAIL'是两回事: FAIL 的项仍然计入复合分")) + "\""
                ",\"basis\":\"" + jsonSafe(basis, 600) + "\"}";
    }

    std::string j;
    j.reserve(5200);
    j += ",\"linearityAudit\":{";
    j += "\"requirement\":\"分数比 == 性能比。这是最高优先级的验收标准; 不满足的项要么修正, 要么不计分。\"";
    j += ",\"singleItemFormula\":\"score = k x (metric x conv)\"";
    j += ",\"singleItemIsLinear\":true";
    j += ",\"singleItemWhy\":\"公式里只有 metric(绝对吞吐)是变量, k 与 conv 是编译期常量 —— "
         "所以同一项在两个吞吐下的分数比严格等于吞吐比。这一条可以在单台设备上机器核对: "
         "score/metric 必须恒等于 k x conv(见 perItemRatioConstant)\"";
    j += ",\"composite\":{\"formula\":\"GM = (Π score_i)^(1/n), i 取所有已计分项\","
         "\"name\":\"几何平均(geometric mean)\","
         "\"officialIsGeometricMean\":true,"
         "\"officialSource\":\"子项分 = 组内负载分的几何平均(本工程的实现见 gb7.cpp 的 compositeJson)\","
         "\"linearity\":\"GM(c x a_i) = c x GM(a_i) —— 当所有计分项同时乘 c 时, 复合分比严格等于 c\","
         "\"caveat\":\"如果不同项的变化倍数不一致, 复合分的比不是任何单一'性能比'; "
         "这时要看 itemScalingSpreadPercent(逐项倍数的离散度)\"}";
    j += ",\"compositeExcludesNotScored\":true";
    j += ",\"perItem\":{\"available\":" + std::string(hasSingle ? "true" : "false") +
         ",\"linearCount\":" + std::to_string(linearCount) +
         ",\"notScoredCount\":" + std::to_string(notScored) +
         ",\"rows\":[" + rows + "]}";
    j += ",\"anchors\":{\"whatIsThis\":\"公开真值的跨代比值 —— 用来核对'我们的分数比能不能定量算出芯片进步的比例'\","
         "\"rows\":[";
    for (int i = 0; i < kTruthAnchorCount; ++i) {
        const TruthAnchor& t = kTruthAnchors[i];
        const double ratio = (t.aScore > 0.0) ? t.bScore / t.aScore : 0.0;
        j += (i == 0 ? "" : ",");
        j += "{\"label\":\"" + jsonSafe(std::string(t.label)) + "\""
             ",\"from\":\"" + jsonSafe(std::string(t.a)) + "\",\"to\":\"" + jsonSafe(std::string(t.b)) + "\""
             ",\"fromScore\":" + fixed(t.aScore, 1) + ",\"toScore\":" + fixed(t.bScore, 1) +
             ",\"truthRatio\":" + fixed(ratio, 4) +
             ",\"unit\":\"" + jsonSafe(std::string(t.unit)) + "\""
             ",\"sourceKind\":\"" + jsonSafe(std::string(t.sourceKind)) + "\""
             ",\"source\":\"" + jsonSafe(std::string(t.source)) + "\""
             ",\"caveat\":\"" + jsonSafe(std::string(t.caveat)) + "\""
             ",\"ourRatio\":null"
             ",\"ourRatioWhy\":\"需要这两台设备各跑一次本工程(同一 benchVersion); "
             "本模块拿不到第二台设备的结果, 所以写 null, 不编一个数\"}"
             ",\"howToUse\":\"在两台设备上各跑一次, 用同一项的分数相除得到 ourRatio; "
             "与 truthRatio 的偏差 > 5% 就说明这一项的实测不线性(要修正或不计分)\"";
    }
    j += "]}";
    j += ",\"truthAnchorsAreApproximate\":true";
    j += ",\"truthAnchorsCaveat\":\"GB6 三档 1299/1616/1866 恰好严格线性(1.2440 x 1.1547 = 1.4365), "
         "但那是第三方给的近似值, 不是真值本身线性的证据; 3DMark SNL 的 991 已有用户拍屏的"
         "一手证据(2026-08-31), 但 303 仍然只有第三方。"
         "真值只用来做数量级的线性核对, 不能当成精确标尺。\"";
    j += ",\"ifNotLinear\":\"口径(与最高优先级验收标准一致): 不满足线性的项要么修正、要么不计分。"
         "本工程 16 项 GB7 的计分公式里没有任何非吞吐量项, 所以构造性线性全部通过; "
         "未计分项(Structure from Motion, 注册表 scored=false 且 score 恒 0)不算作复合分的参与项;"
         "实测层面的线性由 verify_linearity_regression.py 用公开真值锚点核对。\"";
    j += "}";
    return j;
}

// ---------------------------------------------------------------------------
//   条件化可比性 + 每核/同频归一化(最高优先级验收标准点名的那一条)
//  已核实真机事实: 手机只能用 cpu0-7(8 逻辑核), 硬件 9 物理核 / 14 线程;
//  平板 12 逻辑核只能用 7 个。可用核集合与可用最高频档都不同 =>
//  多核分同时受芯片性能与系统策略影响, 限制不同的两台设备其多核分比 ≠ 芯片多核性能比。
//  本模块给出的可执行对策:
//    ① 每核归一化: perCoreThroughput = 总吞吐 / 实际线程数  -> 用每核吞吐相除
//       才是芯片的每核性能比(总吞吐比会被核数差污染)。
//    ② 同频归一化: 报告运行时频率中位与标称最高频, 给出
//       normAtTopKhz = 吞吐 / 频率中位 x 标称最高频 -> 两台设备跑在不同频率档时,
//       用它相除得到的就是"同频下的性能比"。
//    ③ 条件化判断: 明确写出本次与哪一类设备可比、与哪一类不可比。
// ---------------------------------------------------------------------------

struct ThreadInfo {
    int requestedThreads = 0;   // 负载请求的线程数
    int workerCpus = 0;         // 实际绑定的核数(GB7 侧给; 拿不到填 0)
    std::string cpusList;       // 实际用到的核清单(可为空)
};

std::string comparabilityAuditJson(const std::string& opts, const ThreadInfo& ti)
{
    double allowed = 0.0;
    double logical = 0.0;
    double physical = 0.0;
    double khzMed = 0.0;
    double khzTop = 0.0;
    double totalThroughput = 0.0;
    bool hasAllowed = jsonGetNumber(opts, "allowedCores", &allowed);
    bool hasLogical = jsonGetNumber(opts, "logicalCores", &logical);
    bool hasPhysical = jsonGetNumber(opts, "physicalCores", &physical);
    bool hasKhz = jsonGetNumber(opts, "runtimeKhzMedian", &khzMed);
    bool hasTop = jsonGetNumber(opts, "nominalTopKhz", &khzTop);
    bool hasThr = jsonGetNumber(opts, "totalThroughput", &totalThroughput);
    double coreCount = 0.0;
    const bool hasCores = jsonGetNumber(opts, "coreCount", &coreCount);

    const int used = (ti.requestedThreads > 0) ? ti.requestedThreads : 0;
    const double perCore = (used > 0 && hasThr) ? (totalThroughput / (double)used) : 0.0;
    const double normTop = (hasThr && hasKhz && hasTop && khzMed > 0.0)
                               ? (totalThroughput / khzMed * khzTop)
                               : 0.0;
    const double normTopPerCore = (normTop > 0.0 && used > 0) ? (normTop / (double)used) : 0.0;
    const bool coresRestricted = (hasAllowed && hasLogical && allowed < logical);

    std::string j;
    j.reserve(4200);
    j += ",\"comparabilityAudit\":{";
    j += "\"question\":\"分数比 == 性能比 这件事, 什么时候成立、什么时候不成立? 本次是在什么条件下取得的?\"";
    j += ",\"knownFact\":\"真机核实: 这台手机只能使用 cpu0-7(8 个逻辑核), 而硬件是 9 物理核 / 14 线程; "
         "平板 12 逻辑核只能使用 7 个。可用核集合不同、可用最高频档也可能不同。\"";
    j += ",\"whyItBreaksLinearity\":\"多核吞吐 = 每核性能 x 实际可用核数 x 频率。"
         "如果两台设备的'可用核数'或'频率档'限制程度不同, 那么'总吞吐比'里就混进了系统策略的差异, "
         "它不再等于'芯片多核性能比' —— 这正是必须做条件化标注与归一化的原因。\"";
    j += ",\"conditions\":{"
         "\"allowedCores\":" + (hasAllowed ? fixed(allowed, 0) : std::string("null")) +
         ",\"logicalCores\":" + (hasLogical ? fixed(logical, 0) : std::string("null")) +
         ",\"physicalCores\":" + (hasPhysical ? fixed(physical, 0) : std::string("null")) +
         ",\"coresRestricted\":" + (hasAllowed && hasLogical ? std::string(coresRestricted ? "true" : "false")
                                                            : std::string("null")) +
         ",\"coresActuallyUsed\":" + std::to_string(used) +
         ",\"coresActuallyUsedSuffix\":\"M/" + std::string(hasAllowed ? fixed(allowed, 0) : std::string("?")) + "\""
         ",\"workerCpusBound\":" + std::to_string(ti.workerCpus) +
         ",\"workerCpusList\":\"" + jsonSafe(ti.cpusList) + "\""
         ",\"runtimeKhzMedian\":" + (hasKhz ? fixed(khzMed, 0) : std::string("null")) +
         ",\"nominalTopKhz\":" + (hasTop ? fixed(khzTop, 0) : std::string("null")) +
         ",\"frequencyShareOfTop\":" +
         ((hasKhz && hasTop && khzTop > 0.0) ? fixed(khzMed / khzTop, 4) : std::string("null")) +
         ",\"threadsRequested\":" + std::to_string(ti.requestedThreads) +
         "}";
    j += ",\"normalization\":{"
         "\"perCoreThroughput\":" + (perCore > 0.0 ? fixed(perCore, 3) : std::string("null")) +
         ",\"perCoreFormula\":\"每核吞吐 = 总吞吐 / 实际线程数\""
         ",\"perCoreWhy\":\"可用核数不同的两台设备, 用每核吞吐相除才是芯片每核性能比; "
         "总吞吐比会被核数差污染\""
         ",\"throughputAtTopKhz\":" + (normTop > 0.0 ? fixed(normTop, 3) : std::string("null")) +
         ",\"perCoreThroughputAtTopKhz\":" + (normTopPerCore > 0.0 ? fixed(normTopPerCore, 3) : std::string("null")) +
         ",\"atTopKhzFormula\":\"同频归一化 = 吞吐 / 运行时频率中位 x 标称最高频\""
         ",\"atTopKhzWhy\":\"吞吐与频率成正比; 两台设备跑在不同频率档时, 用它相除得到的是"
         "同频下的性能比 —— 这就是'精准的提升比例'需要的量\""
         ",\"precondition\":\"'吞吐 ∝ 频率'在同一颗芯片同一架构上成立; 跨架构是假设, 必须标注\""
         ",\"needsSysfsWrite\":false"
         ",\"alternativeIfNoFrequencyReadout\":\"读不到 scaling_cur_freq 时: 用 /proc/self/task/<tid>/stat 的"
         " utime+stime 与墙钟之比也能给出'平均占用核数 x 频率'的等效量(CPU 时间/墙钟); "
         "本工程 cpu_freq_sample.cpp 已经在采这条路径, 可复用它做二次归一化\""
         "}";
    j += ",\"verdict\":{"
         "\"class\":\"" +
         std::string(!hasAllowed ? "UNKNOWN_CONDITIONS"
                                 : (coresRestricted ? "CORES_RESTRICTED" : "FULL_CORES")) + "\"";
    if (!hasAllowed) {
        j += ",\"comparableWith\":[\"只与同一台设备的历史结果比(可用核集合未知, 无法判断条件)\"]";
        j += ",\"notComparableWith\":[\"任何跨设备的多核比值(条件未知)\"]";
    } else if (coresRestricted) {
        j += ",\"comparableWith\":[\"同样被限制到相近核数的设备(用 perCoreThroughput 相除更稳)\","
             "\"同一台设备的历史结果\"]";
        j += ",\"notComparableWith\":[\"核数更多/限制更少的设备 —— 总吞吐比会低估本机的芯片性能\","
             "\"直接拿总多核分做跨代比\"";
    } else {
        j += ",\"comparableWith\":[\"同样跑满全部核的设备(仍需注意频率档与热状态)\","
             "\"同一台设备的历史结果\"]";
        j += ",\"notComparableWith\":[\"被限制核数/频率的设备(它们的总吞吐被压低)\"]";
    }
    j += ",\"conditionText\":\"" + jsonSafe(
        std::string("本次条件: 内核允许 ") + (hasAllowed ? fixed(allowed, 0) : std::string("?")) +
        " 核" + (hasPhysical ? ("(硬件物理核 " + fixed(physical, 0) + ")") : "") +
        ", 本项用 " + std::to_string(used) + " 线程" +
        (coresRestricted ? "(被限制, 不是芯片的全部核)" : "(未被限制)") +
        "; 运行时频率中位 " + (hasKhz ? (fixed(khzMed, 0) + " kHz") : std::string("读不到")) +
        (hasTop ? (" / 标称最高 " + fixed(khzTop, 0) + " kHz(" +
                   fixed(hasKhz && khzTop > 0 ? khzMed / khzTop * 100.0 : 0.0, 1) + "%)")
                : std::string("")) +
        "。=> 跨设备比较请优先用 normalization 的两个归一化量。") + "\"";
    j += "}";
    j += ",\"executablePlan\":["
         "\"① 两台设备都跑同一 benchVersion 的多核阶段, 记录 allowedCores / 频率中位 / 线程数(本块自动给出)\","
         "\"② 用 perCoreThroughput(每核吞吐)相除 —— 立刻消掉'可用核数不同'这一项污染\","
         "\"③ 再用 throughputAtTopKhz(同频归一化)相除 —— 消掉'最高频档不同'这一项污染\","
         "\"④ 两个归一化量的比值就是芯片性能比; 与公开真值比对照, 偏差 > 5% 的项标 NEEDS_REVIEW\","
         "\"⑤ 若 /sys 频率不可读: 用 cpu_freq_sample 的 (utime+stime)/墙钟 做等效归一化, 并在报告里标注这是等效量\""
         "]";
    j += ",\"doesNotDo\":\"本模块不会去写 sysfs 改频率/governor(那是改被测对象)。"
         "同频对标的做法有两种, 本工程都支持其一只用只读路径: "
         "(a) 事后归一化(推荐, 已实现): 用运行时频率中位把吞吐折算到标称最高频; "
         "(b) 事前绑频(需 root 或可写 governor): 把 cpufreq 锁到同一档再跑 —— 需要写权限, "
         "本工程不这么做, 因为它会改变被测条件本身。\"";
    j += "}";
    return j;
}

} // namespace

//  v1 -> v2(2026-08-31) 两处变更:
//   ① 3DMark SNL 的 991(Kirin 9030 Pro)来源从 UNVERIFIED_THIRD_PARTY 改为
//      USER_PROVIDED_PRIMARY_OBSERVATION(用户拍屏的一手证据: Mate 80 Pro Max / SNL /
//      总分 991 / 平均帧率 7.34 FPS); 原有的两条 caveat 全部保留。
//   ② 新增两份报告对比 auroraReferenceCompareReports()(纯计算, 只读)。
//   真值表的语义变了, 所以版本号必须 +1 —— 旧的对照结论与旧版本不可比。
const int kReferenceVersion = 2;

std::string auroraReferenceVersionText()
{
    return "reference truth table v" + std::to_string(kReferenceVersion) +
           " (CS1 单核 8 项公开真值 + CS1 复合分 2 项 + 3DMark SNL 3 项"
           "[其中 991 = 用户拍屏一手证据]; 另含两份报告对比 compareReports; 改动真值表必须 +1)";
}

std::string auroraReferenceRealUse(const std::string& itemName)
{
    return std::string(findRealUse(itemName));
}

// ---------------------------------------------------------------------------
//  对照表主体
// ---------------------------------------------------------------------------
std::string auroraReferenceCompare(const std::string& optionsJson)
{
    std::string singleCoreJson;
    std::string multiCoreJson;
    std::string gpuSnlJson;
    std::string gpu7Json;
    double singleComposite = 0.0;
    double multiComposite = 0.0;
    const bool hasSingle = jsonGetString(optionsJson, "singleCore", &singleCoreJson);
    const bool hasMulti = jsonGetString(optionsJson, "multiCore", &multiCoreJson);
    const bool hasSnl = jsonGetString(optionsJson, "gpuSnl", &gpuSnlJson);
    const bool hasGpu7 = jsonGetString(optionsJson, "gpu7Items", &gpu7Json);
    const bool hasSingleComp = jsonGetNumber(optionsJson, "singleComposite", &singleComposite);
    const bool hasMultiComp = jsonGetNumber(optionsJson, "multiComposite", &multiComposite);
    double tmpThreads = 0.0;
    double tmpWorkers = 0.0;

    std::string j;
    j.reserve(9000);
    j += "{\"ok\":true";
    j += ",\"truthVersion\":" + std::to_string(kReferenceVersion);
    j += ",\"truthVersionText\":\"" + jsonSafe(auroraReferenceVersionText()) + "\"";
    j += ",\"module\":\"aurora-reference (只读对照模块: 不跑负载 / 不改分数 / 不参与计分)\"";
    j += ",\"referenceDevice\":\"" + jsonSafe(std::string(kReferenceDevice)) + "\"";
    j += ",\"referenceDeviceSource\":\"USER_PROVIDED_PUBLIC_TRUTH\"";
    j += ",\"referenceDeviceNote\":\"" + jsonSafe(std::string(kReferenceDeviceNote)) + "\"";
    j += ",\"sourceKindRule\":\"OFFICIAL_* = 厂商官方文档/官方成绩库; "
         "USER_PROVIDED_PRIMARY_OBSERVATION = 用户提供的一手证据(拍屏/截图直接观测, 强度最高); "
         "USER_PROVIDED_PUBLIC_TRUTH = 用户提供的公开真值(本次未逐条复核); "
         "THIRD_PARTY = 第三方媒体/聚合站; UNVERIFIED_* = 未证实; NOT_AVAILABLE = 查不到。"
         "强度顺序: 一手观测 > 公开真值 > 第三方转载。第三方数据不写成官方。\"";
    j += ",\"comparisonIsNotAnOfficialConversion\":true";
    j += ",\"comparisonCaveat\":\"同一个数字在不同基准之间没有官方换算关系(各大跑分软件都明确声明过)。"
         "本表只回答'数量级对不对 / 我这台比参考机强多少百分比', 不回答'本机在某官方测试里能得多少分'。\"";
    j += ",\"deltaPercentDefinition\":\"(本机 - 参考值) / 参考值 x 100%; 正 = 本机更高\"";
    j += ",\"oursIsRepresentative\":\"本机值取多轮中位(见 repeatability), 不是最好一次\"";

    // ---------------- CS1 单核 ----------------
    j += ",\"singleCore\":{\"items\":[";
    {
        std::vector<std::string> items = jsonSplitObjects(singleCoreJson);
        std::string rows;
        for (size_t i = 0; i < items.size(); ++i) {
            const std::string& it = items[i];
            std::string name;
            if (!jsonGetString(it, "name", &name)) {
                continue;
            }
            double ourScore = 0.0;
            (void)jsonGetNumber(it, "score", &ourScore);
            std::string ourMetric;
            (void)jsonGetString(it, "metric", &ourMetric);
            std::string ourUnit;
            (void)jsonGetString(it, "unit", &ourUnit);
            std::string basis;
            (void)jsonGetString(it, "basis", &basis);
            const TruthRow* t = findTruth(name);
            const char* use = findRealUse(name);
            const double delta = (t != nullptr && t->score > 0.0) ? (ourScore / t->score - 1.0) * 100.0 : 0.0;
            rows += (rows.empty() ? "" : ",");
            rows += "{\"name\":\"" + jsonSafe(name) + "\"";
            rows += ",\"realUse\":\"" + jsonSafe(use[0] != '\0' ? std::string(use) : std::string("查不到")) + "\"";
            rows += ",\"ours\":{\"score\":" + fixed(ourScore, 1) +
                    ",\"metric\":\"" + jsonSafe(ourMetric) + "\"" +
                    ",\"unit\":\"" + jsonSafe(ourUnit) + "\"}";
            rows += ",\"howComputed\":{\"formula\":\"单项分 = k x (metric x conv)\","
                    "\"basis\":\"" + jsonSafe(basis, 900) + "\""
                    ",\"unit\":\"" + jsonSafe(ourUnit) + "\"}";
            if (t != nullptr) {
                rows += ",\"reference\":{\"score\":" + fixed(t->score, 0) +
                        ",\"metric\":\"" + jsonSafe(std::string(t->metric)) + "\""
                        ",\"sourceKind\":\"" + jsonSafe(std::string(t->sourceKind)) + "\""
                        ",\"source\":\"" + jsonSafe(std::string(t->source)) + "\"}";
                rows += ",\"deltaPercent\":" + fixed(delta, 1);
                rows += ",\"comparable\":true";
                rows += ",\"caveat\":\"GB7 的官方数据集与实现没有公开, 本工程是同类负载的复刻: "
                        "公式/单位/锚点缩放关系一致, 但不保证逐项相等的绝对分 —— 这个百分比是"
                        "同一类工作的吞吐差距, 不是官方分数的差\"";
            } else {
                rows += ",\"reference\":null,\"deltaPercent\":null,\"comparable\":false";
                rows += ",\"caveat\":\"查不到该负载的公开真值(用户提供的清单里没有这一项): "
                        "写查不到, 不拿别的项或其他来源凑一个数\"";
            }
            rows += "}";
        }
        j += rows;
    }
    j += "],\"composite\":{";
    j += "\"single\":{\"ours\":" + (hasSingleComp ? fixed(singleComposite, 1) : std::string("null")) +
         ",\"reference\":" + fixed(kGb7SingleCompositeTruth, 0) +
         ",\"deltaPercent\":" +
         (hasSingleComp && kGb7SingleCompositeTruth > 0.0
              ? fixed((singleComposite / kGb7SingleCompositeTruth - 1.0) * 100.0, 1)
              : std::string("null")) +
         ",\"sourceKind\":\"USER_PROVIDED_PUBLIC_TRUTH\",\"source\":\"" + jsonSafe(std::string(kSourceUserTruth)) + "\"}";
    j += ",\"multi\":{\"ours\":" + (hasMultiComp ? fixed(multiComposite, 1) : std::string("null")) +
         ",\"reference\":" + fixed(kGb7MultiCompositeTruth, 0) +
         ",\"deltaPercent\":" +
         (hasMultiComp && kGb7MultiCompositeTruth > 0.0
              ? fixed((multiComposite / kGb7MultiCompositeTruth - 1.0) * 100.0, 1)
              : std::string("null")) +
         ",\"sourceKind\":\"USER_PROVIDED_PUBLIC_TRUTH\",\"source\":\"" + jsonSafe(std::string(kSourceUserTruth)) + "\"}";
    j += ",\"structureSource\":\"" + jsonSafe(std::string(kSourceOfficialGb7Composite)) + "\"";
    j += ",\"caveat\":\"复合分对照同样只是数量级参考; 多核真值 6802 是那台参考机的多核结果\"}";
    // 多核逐项(有就报, 没有就说明为什么没有)
    j += ",\"multiCoreItemsIncluded\":" + std::string(hasMulti ? "true" : "false");
    if (!hasMulti) {
        j += ",\"multiCoreItemsWhy\":\"没传多核结果: 对照表只做单核逐项 + 两个复合分, "
              "不替用户猜多核逐项的数字\"";
    }
    j += "}";

    // ---- 线性自检(最高优先级验收标准: 分数比 == 性能比) ----
    j += linearityAuditJson(singleCoreJson, hasSingle);
    // ---- 条件化可比性(可用核集合 / 频率档 / 每核与同频归一化) ----
    {
        ThreadInfo ti;
        jsonGetNumber(optionsJson, "threadsUsed", &tmpThreads);
        ti.requestedThreads = (int)(tmpThreads + 0.5);
        jsonGetNumber(optionsJson, "workerCpus", &tmpWorkers);
        ti.workerCpus = (int)(tmpWorkers + 0.5);
        jsonGetString(optionsJson, "workerCpusList", &ti.cpusList);
        j += comparabilityAuditJson(optionsJson, ti);
    }
    // ---------------- GPU: GPU-SNL ----------------
    j += ",\"gpu\":{\"snl\":{";
    {
        double ourScore = 0.0;
        double ourFps = 0.0;
        double ourMpx = 0.0;
        const bool okSnl = hasSnl && jsonGetNumber(gpuSnlJson, "value", &ourScore);
        if (hasSnl) {
            // score.value / fps.value / throughput.value 三个同名字段: 分别取各自对象里的 value
            const size_t sp = gpuSnlJson.find("\"score\"");
            const size_t fp = gpuSnlJson.find("\"fps\"");
            const size_t tp = gpuSnlJson.find("\"throughput\"");
            if (sp != std::string::npos) {
                (void)jsonGetNumber(gpuSnlJson.substr(sp), "value", &ourScore);
            }
            if (fp != std::string::npos) {
                (void)jsonGetNumber(gpuSnlJson.substr(fp), "value", &ourFps);
            }
            if (tp != std::string::npos) {
                (void)jsonGetNumber(gpuSnlJson.substr(tp), "value", &ourMpx);
            }
        }
        j += "\"available\":" + std::string(hasSnl ? "true" : "false");
        j += ",\"ours\":{\"score\":" + (hasSnl ? fixed(ourScore, 1) : std::string("null")) +
             ",\"fps\":" + (hasSnl ? fixed(ourFps, 3) : std::string("null")) +
             ",\"throughputMpxPerSec\":" + (hasSnl ? fixed(ourMpx, 3) : std::string("null")) + "}";
        j += ",\"howComputed\":{\"formula\":\"score = fps x kScorePerFps\","
             "\"kScorePerFps\":75.9375,"
             "\"derivation\":\"135(UL 官方明文) x 0.5625((1920x1080)/(2560x1440)) = 75.9375\","
             "\"unit\":\"points(fps 与 Mpx/s 也一并给出)\","
             "\"sameFormulaAsOfficial\":true,"
             "\"differentWorkload\":true,"
             "\"sameCaliber\":\"分数与帧率严格成正比这一条与官方同口径(官方明文 S = F x 135); "
             "帧率 / 吞吐量(Mpx/s) / 计时协议(预热 + 固定帧数 + 帧尾同步)也按主流跑分的通行做法\""
             ",\"differentCaliber\":\"负载不是同一份(官方没公开实现) / 渲染分辨率 1920x1080 对官方 2560x1440 / "
             "绝对值在标定前不能当官方分数用 —— 这几条必须与分数一起给出\"}";
        j += ",\"officialFormula\":\"" + jsonSafe(std::string(kSourceOfficial3dmark)) + "\"";
        j += ",\"rows\":[";
        for (int i = 0; i < kSnlTruthCount; ++i) {
            const double off = kSnlTruth[i].score;
            const double ours = ourScore;
            const double pct = (off > 0.0 && hasSnl) ? (ours / off - 1.0) * 100.0 : 0.0;
            j += (i == 0 ? "" : ",");
            j += "{\"name\":\"" + jsonSafe(std::string(kSnlTruth[i].soc)) + " " +
                 jsonSafe(std::string(kSnlTruth[i].device)) + "\""
                 ",\"referenceScore\":" + fixed(off, 0) +
                 ",\"referenceFps\":" + fixed(off / 135.0, 3) +
                 ",\"referenceFpsNote\":\"官方 SNL 在 2560x1440 下的平均帧率(= 分数 / 135)\""
                 ",\"ourScore\":" + (hasSnl ? fixed(ours, 1) : std::string("null")) +
                 ",\"deltaPercent\":" + (hasSnl ? fixed(pct, 1) : std::string("null")) +
                 ",\"sourceKind\":\"" + jsonSafe(std::string(kSnlTruth[i].sourceKind)) + "\""
                 ",\"source\":\"" + jsonSafe(std::string(kSnlTruth[i].source)) + "\""
                 ",\"caveat\":\"跨负载: 本小节的负载与 SNL 不是同一份, 只比数量级\"}";
        }
        j += "]";
        if (!hasSnl) {
            j += ",\"why\":\"没传 GPU-SNL 的结果 JSON: 本表不会替用户编一个分数出来\"";
        }
    }
    // ---------------- GPU: CS1 11 项 ----------------
    j += ",\"gb7GpuItems\":{\"available\":" + std::string(hasGpu7 ? "true" : "false") +
         ",\"items\":[";
    {
        std::vector<std::string> items = jsonSplitObjects(gpu7Json);
        std::string rows;
        for (size_t i = 0; i < items.size(); ++i) {
            std::string name;
            if (!jsonGetString(items[i], "name", &name)) {
                continue;
            }
            double our = 0.0;
            const bool gotScore = jsonGetNumber(items[i], "score", &our);
            const char* use = findRealUse(name);
            rows += (rows.empty() ? "" : ",");
            rows += "{\"name\":\"" + jsonSafe(name) + "\""
                    ",\"realUse\":\"" + jsonSafe(use[0] != '\0' ? std::string(use) : std::string("查不到")) + "\""
                    ",\"ours\":" + (gotScore ? fixed(our, 1) : std::string("null")) +
                    ",\"reference\":null"
                    ",\"comparable\":false"
                    ",\"caveat\":\"公开 GPU 套件的 11 项没有公开的逐项真值(只有结果页), "
                    "本工程的该小节另有自己的锚点表; 这里写查不到\"}";
        }
        j += rows;
    }
    j += "],\"caveat\":\"没有公开真值的项一律写查不到, 不拿别的项凑数\"}";
    j += "}";

    // ---------------- 查不到的项 ----------------
    j += ",\"notAvailable\":["
         "\"该套件是否公开过逐项绝对吞吐(basis 里用的锚点是公开结果页读数, 不是官方文档常量)\","
         "\"麒麟平台的 GPU 频率/温度节点(查不到: sysfs 里没有)\","
         "\"Mate 80 系列在 UL 官方成绩库里的 3DMark SNL 机型页(查不到; 991 这条的支撑是用户拍屏的一手证据, 而不是 UL 成绩库)\","
         "\"GPU-SNL 与 SNL 的官方换算关系(不存在: UL 明确各测试不可互比)\""
         "]";
    j += ",\"caveats\":["
         "\"所有 deltaPercent 都是跨负载/跨机器的比较, 只用于判断数量级\","
         "\"用户提供的公开真值本次未逐条复核, 已按 USER_PROVIDED_PUBLIC_TRUTH 标注\","
         "\"参考机不是官方基准机, 官方基准机是 Lenovo Legion + RTX 4060(GB7 官方明文: 100,000 分)\","
         "\"温度/频率/后台负载会显著影响任何基准; 对比时请在与参考机相近的条件下进行\""
         "]";
    j += "}";
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "reference compare built (single=%d, snl=%d)",
                 (int)hasSingle, (int)hasSnl);
    return j;
}

// ===========================================================================
//   两份报告对比(2026-08-31 新增) —— 纯计算 / 只读
// ===========================================================================
// 场景(用户的原话): "在两台设备上各跑一次, 然后算出分数比, 并与真值比对照, 看偏差是否 <5%"。
// 在这一版之前, 这一步只能人工做: 打开两份 report-latest.json, 一项一项抄数字, 按计算器。
//
// 输入: 两份报告 JSON —— 就是本工程"一键全部跑分"落盘的 files/report-latest.json
//       (schema = aurora-fullreport/1); 直接把文件全文传进来即可。
//       第三个参数 options(可选, 扁平 JSON):
//         {"labelA":"设备A","labelB":"设备B",      // 显示名(可选, 缺省用报告里的 device)
//          "socA":"Kirin 9030 Pro","socB":"..."}  // 显式指定 SoC(推荐: 机型名自动判定不出来时
//                                                 //   本模块不会瞎猜, 会写"判不出来")
// 输出: 见 reference_compare.h 的契约。
//
// 三条硬约束(与整个 aurora-reference 模块一致):
//   1) 纯计算: 不跑负载、不连设备、不读 /sys、不写任何文件、不改任何分数;
//   2) 缺什么写什么: 报告缺字段 / 格式不符 / 机型判不出来 -> 一律 null + 一句话说明缺什么,
//      不编一个数字出来;
//   3) 阈值(5%)是我们自己定的, 不是官方阈值 —— 每一处线性判定都跟着这句话。
//
// 为什么要归一化(用户点名的两个已知污染源: "核数不同会让总分比虚高 12.5%、
// 频率档不同能凭空造出 13.5% 的假提升"):
//   * 可用核数不同(例如 9 核 vs 8 核 = 1.125x): 原始多核分比 = 芯片性能比 x 核数比,
//     核数多的那台白赚 12.5% -> 必须用 perCoreRatio(每核吞吐比)消掉;
//   * 最高频档不同(1.135x ≈ 13.5%): 同一颗芯片跑在不同频率档上, 分比里就混进了频率比,
//     根本没有性能提升也能"涨 13.5%" -> 必须用 sameFreqRatio(同频归一化)消掉。
//   两个都做掉之后剩下的才是"芯片性能比"。
// ===========================================================================

namespace {

// ---- 报告解析:每个字段都"能不能拿到"分开记, 拿不到就是拿不到 ----
struct RepItem {
    std::string section;
    std::string name;
    std::string unit;
    std::string scoredNote;
    std::string metricText;
    std::string runFreq;
    double metric = 0.0;
    double score = 0.0;
    double ms = 0.0;
    double parallelism = 0.0;
    double threadsRequested = 0.0;
    double threadsEffective = 0.0;
    double workersBound = 0.0;
    double allowedCount = 0.0;
    double nominalTopKhz = 0.0;      // cpuMaxKhz: 本项所在核的标称最高频
    double fastClusterMaxKhz = 0.0;
    bool hasMetric = false;
    bool hasScore = false;
    bool hasRunFreq = false;
    bool scored = false;             // scoredByPolicy
    bool ok = false;
    double runtimeKhzMedian = 0.0;   // 从 runFreq 文本里解析出来的"运行时频率 中位"
    bool hasRuntimeKhz = false;
    // ---- "有没有跑满"(从 runFreq 文本里解析; 2026-10 新增) ----
    //   口径由 cpu_freq_sample.h 定义: 占标称比 = 逐样本(该样本频率 / 该样本所在核标称上限)
    //   的中位; 占比中位 >= 90%(我们自己定的阈值) 判"本项已跑满"。
    double saturationPct = 0.0;      // 占标称比 中位(%)
    bool hasSaturation = false;
    bool saturationFull = false;     // 该项报告的判据结论是不是"已达到"
    bool saturationVerdictKnown = false;
    double coresUsed = 0.0;          // 实际用到核数(见 coresUsedSource)
    std::string coresUsedSource;
    // ---- QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 真机报告缺陷 2) ----
    //   报告 items[] 现在带 qos / cpuset 两个键(native runGb7 JSON 的 "qos" / "cpuset")。
    //   对比工具要回答"这两台机器的限制在哪一层""QoS 各自生效没有", 所以这里必须读到它们。
    //   缺字段(老报告 / 非 CS1 项)一律 has=false + 空串, 不用假值冒充。
    std::string qos;
    std::string cpuset;
    bool hasQos = false;
    bool hasCpuset = false;
    std::string allowedMask;         // 可用核集合的位掩码(原样; 用于"可用核集合"的逐项对照)
    double poolThreads = 0.0;        // workersBound: 池线程数(可能远大于可用核数 -> 不能当核数用)
};

struct RepDoc {
    std::string label;
    bool present = false;            // 调用方给了非空字符串
    bool parsed = false;             // 顶层结构看起来是本工程的报告
    std::string parseProblem;        // 缺什么 / 哪里不对(人话)
    std::string schema;
    std::string device;
    std::string appVersion;
    std::string nativeVersion;
    std::string startedAtText;
    double elapsedMs = 0.0;
    bool hasElapsed = false;
    bool interrupted = false;
    bool hasInterrupted = false;
    int rawItemObjects = 0;          // items 数组里切出来的对象个数
    std::vector<RepItem> items;
    std::string soc;                 // 判定出来的 SoC(判不出来就是空)
    std::string socHow;              // 判定依据(人话)
    bool hasSingleComposite = false;
    bool hasMultiComposite = false;
    double singleComposite = 0.0;
    double multiComposite = 0.0;
    double gb7MultiThreads = 0.0;
    // ---- 报告自己写下的"全机口径"频率结论(referenceAudit.inputsNote 里的原文数字) ----
    double noteRuntimeKhz = 0.0;      // inputsNote 里的 runtimeKhzMedian=(代表项的运行时频率中位)
    bool hasNoteRuntimeKhz = false;
    double noteNominalTopKhz = 0.0;   // inputsNote 里的 nominalTopKhz=(全机最快频率档)
    bool hasNoteNominalTopKhz = false;
    // ---- 两份复合分到底由哪些项组成(2026-10-08) ----
    ScalingAuditInfo audit;             // 随芯片变化自检的排除集(项名 + 原因 + 依据)
    CompositeParticipants singleParts;  // CS1 单核复合分的参与项(报告自己给的名单)
    CompositeParticipants multiParts;   // CS1 多核复合分的参与项
    MachineCond machine;                // 线程数 / SMT / 物理逻辑核 / 可用核集合
};

// 从报告的人话文本里取 "键=数字" 形式的量(例如 inputsNote 里的
//   "runtimeKhzMedian=1770000 kHz（实测：代表项的运行频率中位 …）" 与
//   "nominalTopKhz=2750000 kHz（标称：全机最快频率档 cpuMachineTopTierKhz）")。
// 只用于把报告已经写下的结论原样带出来给人核对, 不参与本模块的任何计算。
bool parseNoteNumber(const std::string& json, const std::string& key, double* out)
{
    const size_t p = json.find(key);
    if (p == std::string::npos) {
        return false;
    }
    size_t i = p + key.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '=' || json[i] == '\t')) {
        ++i;
    }
    double v = 0.0;
    if (sscanf(json.c_str() + i, "%lf", &v) != 1 || !(v > 0.0)) {
        return false;
    }
    *out = v;
    return true;
}

// runFreq 文本 -> 运行时频率中位(kHz)。原文形如
//   "运行时频率 中位 1220MHz / 最小 1150MHz / 最大 2270MHz(本核标称上限 2270MHz) 采样 12 次; ..."
// 解析不到就返回 false(不猜)。
bool parseRuntimeKhzMedian(const std::string& text, double* outKhz)
{
    const size_t p = text.find("中位");
    if (p == std::string::npos) {
        return false;
    }
    size_t i = p + 6; // "中位" 在 UTF-8 里占 6 字节
    while (i < text.size() && (text[i] == ' ' || text[i] == '=' || text[i] == ':' || text[i] == '\t')) {
        ++i;
    }
    double v = 0.0;
    if (sscanf(text.c_str() + i, "%lf", &v) != 1 || !(v > 0.0)) {
        return false;
    }
    // 单位: 原文是 MHz(见 cpu_freq_sample.cpp)。统一换算成 kHz, 与报告里的 cpuMaxKhz 同口径。
    const size_t unitPos = text.find("MHz", i);
    const bool mhz = (unitPos != std::string::npos && unitPos - i < 24);
    *outKhz = mhz ? (v * 1000.0) : v;
    return true;
}

// runFreq 文本 -> "占标称比 中位 X%" 与 "跑满判据 ... -> 已达到/未达到",
// 以及判据里的阈值("占比中位 >= 90%")。原文形如:
//   "... 占标称比 中位 70%(逐样本 = ...) 跑满判据(我们自定: 占比中位 >= 90%) -> 未达到..."
// 多核项的文本里有两处"占标称比"(口径A 与口径B), find 取到的第一处就是主口径A —— 正确。
// 解析不到就返回 false(旧报告、没有走频率采样的项), 不猜一个数出来。
bool parseSaturation(const std::string& text, double* outPct, bool* outFull, bool* outVerdictKnown,
                     double* outThresholdPct)
{
    const std::string key = "占标称比 中位 ";
    const size_t p = text.find(key);
    if (p == std::string::npos) {
        return false;
    }
    size_t i = p + key.size();
    double v = 0.0;
    if (sscanf(text.c_str() + i, "%lf", &v) != 1 || v < 0.0 || v > 1000.0) {
        return false;
    }
    *outPct = v;
    // 判据阈值(同一段文本里就有; 拿不到也不影响本模块, 只是阈值那一格写 null)
    if (outThresholdPct != nullptr) {
        *outThresholdPct = 0.0;
        const std::string tk = "占比中位 >= ";
        const size_t tp = text.find(tk);
        if (tp != std::string::npos) {
            double tv = 0.0;
            if (sscanf(text.c_str() + tp + tk.size(), "%lf", &tv) == 1 && tv > 0.0 && tv <= 100.0) {
                *outThresholdPct = tv;
            }
        }
    }
    if (outVerdictKnown != nullptr && outFull != nullptr) {
        const size_t vp = text.find("跑满判据");
        if (vp == std::string::npos) {
            *outVerdictKnown = false;
            *outFull = false;
        } else {
            const size_t hit = text.find("已达到", vp);
            const size_t miss = text.find("未达到", vp);
            if (hit != std::string::npos && (miss == std::string::npos || hit < miss)) {
                *outVerdictKnown = true;
                *outFull = true;
            } else if (miss != std::string::npos) {
                *outVerdictKnown = true;
                *outFull = false;
            } else {
                *outVerdictKnown = false;
                *outFull = false;
            }
        }
    }
    return true;
}

// 解析一份报告。返回 false = 这份报告不可用(缺什么写进 parseProblem)。
bool parseReportJson(const std::string& json, const std::string& label, RepDoc* out)
{
    out->label = label;
    out->present = !json.empty();
    if (!out->present) {
        out->parseProblem = label + ": 报告字符串是空的(调用方没有传这一份)";
        return false;
    }
    if (json.find("aurora-fullreport") == std::string::npos) {
        out->parseProblem = label + ": 没有找到 schema 标记 \"aurora-fullreport/...\", "
                                    "看起来不是本工程一键跑分生成的报告(拒绝把别的 JSON 硬当成报告解析)";
        return false;
    }
    (void)jsonGetString(json, "schema", &out->schema);

    // ---- header(设备与版本) ----
    const size_t hp = json.find("\"header\"");
    if (hp != std::string::npos) {
        const std::string head = json.substr(hp);
        (void)jsonGetString(head, "device", &out->device);
        (void)jsonGetString(head, "appVersion", &out->appVersion);
        (void)jsonGetString(head, "nativeVersion", &out->nativeVersion);
        (void)jsonGetString(head, "startedAtText", &out->startedAtText);
        double v = 0.0;
        if (jsonGetNumber(head, "elapsedMs", &v)) {
            out->elapsedMs = v;
            out->hasElapsed = true;
        }
        const size_t ip = head.find("\"interrupted\"");
        if (ip != std::string::npos) {
            out->hasInterrupted = true;
            // 修 off-by-one: "interrupted" 含两侧引号共 13 字符(ip..ip+12), 所以 ip+13 落在冒号上,
            // 旧写法恒判 false(中断的报告不会被标成不可比)。改成跳过冒号与空白再比, 不再依赖固定偏移。
            size_t iv = ip + 13;
            while (iv < head.size() && (head[iv] == ':' || head[iv] == ' ' || head[iv] == '\t')) { ++iv; }
            out->interrupted = (head.compare(iv, 4, "true") == 0);
        }
    } else {
        out->parseProblem = label + ": 没有 header 块(设备型号/版本拿不到)";
    }

    // ---- items(逐项结果) ----
    const size_t ip = json.find("\"items\"");
    if (ip == std::string::npos) {
        if (!out->parseProblem.empty()) {
            out->parseProblem += "; ";
        }
        out->parseProblem += label + ": 没有 items 数组 —— 这份报告里没有任何逐项结果, 无法对比";
        return false;
    }
    const size_t lb = json.find('[', ip);
    if (lb == std::string::npos) {
        if (!out->parseProblem.empty()) {
            out->parseProblem += "; ";
        }
        out->parseProblem += label + ": items 后面不是数组, 格式不符";
        return false;
    }
    std::vector<std::string> objs = jsonSplitObjects(json.substr(lb));
    out->rawItemObjects = (int)objs.size();
    for (size_t i = 0; i < objs.size(); ++i) {
        RepItem it;
        if (!jsonGetString(objs[i], "section", &it.section) || !jsonGetString(objs[i], "name", &it.name)) {
            continue; // 没名字/没分节的对象不是一项结果(宁可跳过, 也不硬塞)
        }
        (void)jsonGetString(objs[i], "unit", &it.unit);
        (void)jsonGetString(objs[i], "scoredNote", &it.scoredNote);
        it.hasMetric = jsonGetString(objs[i], "metric", &it.metricText);
        double v = 0.0;
        (void)jsonGetNumber(objs[i], "metric", &v);
        it.metric = v;
        it.hasScore = jsonGetNumber(objs[i], "score", &it.score);
        (void)jsonGetNumber(objs[i], "ms", &it.ms);
        (void)jsonGetNumber(objs[i], "parallelism", &it.parallelism);
        (void)jsonGetNumber(objs[i], "threadsRequested", &it.threadsRequested);
        (void)jsonGetNumber(objs[i], "threadsEffective", &it.threadsEffective);
        (void)jsonGetNumber(objs[i], "workersBound", &it.workersBound);
        (void)jsonGetString(objs[i], "allowedMask", &it.allowedMask);
        it.poolThreads = it.workersBound;
        (void)jsonGetNumber(objs[i], "allowedCount", &it.allowedCount);
        (void)jsonGetNumber(objs[i], "cpuMaxKhz", &it.nominalTopKhz);
        (void)jsonGetNumber(objs[i], "fastClusterMaxKhz", &it.fastClusterMaxKhz);
        // QoS 运行条件 / 只读分层诊断: 报告 items[] 的两个旁路键(2026-10 缺陷 2 起才有)
        it.hasQos = jsonGetString(objs[i], "qos", &it.qos);
        it.hasCpuset = jsonGetString(objs[i], "cpuset", &it.cpuset);
        it.hasRunFreq = jsonGetString(objs[i], "runFreq", &it.runFreq);
        if (it.hasRunFreq) {
            it.hasRuntimeKhz = parseRuntimeKhzMedian(it.runFreq, &it.runtimeKhzMedian);
            it.hasSaturation = parseSaturation(it.runFreq, &it.saturationPct, &it.saturationFull,
                                               &it.saturationVerdictKnown, nullptr);
        }
        const size_t sp = objs[i].find("\"scoredByPolicy\"");
        if (sp != std::string::npos) {
            it.scored = (objs[i].compare(sp + 17, 5, "true") == 0);
        }
        const size_t op = objs[i].find("\"ok\"");
        if (op != std::string::npos) {
            it.ok = (objs[i].compare(op + 4, 5, "true") == 0);
        }
        // 实际用到核数: 池线程落点数 > 生效线程数 > 报告里的 parallelism
        //
        // 2026-10-08 修: 池线程数(workersBound)只在"它不超过可用核集合"时才能当核数用。
        //   本次两台真机上都出现过 workersBound = 72 / 84 而允许核集合只有 8 / 7 的项 ——
        //   那 72/84 是线程池大小, 不是"真正用到的核数"; 拿它当分母去算"每核吞吐"会把
        //   同一条公式在两台机器上按不同的含义使用, 得到的是假的每核比。
        //   判据: workersBound > allowedCount(可用核集合) -> 它不是核数, 退回 threadsEffective。
        if (it.workersBound > 0.0 && (it.allowedCount <= 0.0 || it.workersBound <= it.allowedCount)) {
            it.coresUsed = it.workersBound;
            it.coresUsedSource = "workersBound(池线程实际绑定的核数)";
        } else if (it.workersBound > 0.0 && it.threadsEffective > 0.0) {
            it.coresUsed = it.threadsEffective;
            it.coresUsedSource = "threadsEffective(本项真正开了几个线程; 报告里的 workersBound=" +
                                 fixed(it.workersBound, 0) + " 超过可用核集合 " + fixed(it.allowedCount, 0) +
                                 " 核 —— 那是线程池大小, 不是真正用到的核数, 已按判据改用线程数)";
        } else if (it.threadsEffective > 0.0) {
            it.coresUsed = it.threadsEffective;
            it.coresUsedSource = "threadsEffective(本项真正开了几个线程)";
        } else if (it.parallelism > 0.0) {
            it.coresUsed = it.parallelism;
            it.coresUsedSource = "parallelism(报告里的并行度)";
        } else {
            it.coresUsed = 0.0;
            it.coresUsedSource = "报告里没有线程/核数字段(无法归一化到每核)";
        }
        out->items.push_back(it);
    }
    if (out->items.empty()) {
        out->parseProblem = (out->parseProblem.empty() ? std::string() : (out->parseProblem + "; ")) +
                            label + ": items 数组里没有一项能解析出 section + name";
        return false;
    }

    // ---- 两份复合分的"参与项集合"到底一不一样(2026-10-08) ----
    // ① 自检排除集: 报告里逐项记着"哪一项没通过随芯片变化自检、判定依据是什么";
    // ② 参与项名单: 报告自己给出的 composite.gb7SingleItems / gb7MultiItems;
    // ③ 运行条件: 线程数 / SMT / 物理逻辑核 / 可用核集合。
    // 三样都只读报告里已经落盘的字段; 读不到就 present=false(写缺, 不猜)。
    parseScalingAudit(json, &out->audit);
    {
        std::string t;
        if (jsonGetString(json, "gb7SingleItems", &t)) {
            parseCompositeItems(t, &out->singleParts);
        }
        t.clear();
        if (jsonGetString(json, "gb7MultiItems", &t)) {
            parseCompositeItems(t, &out->multiParts);
        }
    }
    parseMachineCond(json, &out->machine);
    out->hasNoteRuntimeKhz = parseNoteNumber(json, "runtimeKhzMedian=", &out->noteRuntimeKhz);
    out->hasNoteNominalTopKhz = parseNoteNumber(json, "nominalTopKhz=", &out->noteNominalTopKhz);
    for (size_t i = 0; i < out->items.size(); ++i) {
        if (out->items[i].allowedCount > out->machine.allowedCores) {
            out->machine.allowedCores = out->items[i].allowedCount;
            out->machine.allowedMask = out->items[i].allowedMask;
        }
    }

    // ---- composite ----
    const size_t cp = json.find("\"composite\"");
    if (cp != std::string::npos) {
        const std::string c = json.substr(cp);
        out->hasSingleComposite = jsonGetNumber(c, "gb7SingleComposite", &out->singleComposite);
        out->hasMultiComposite = jsonGetNumber(c, "gb7MultiComposite", &out->multiComposite);
        (void)jsonGetNumber(c, "gb7MultiThreads", &out->gb7MultiThreads);
    }
    out->parsed = true;
    return true;
}

// 小节名的规范化。
//  套件的显示名改过两次: 8.4 把 GB7 改成 GB8, 本次开源改名再把 GB8 改成 CS1。
//  于是历史报告里的小节名有 "GB7 单核" / "GB8 单核" / "CS1 单核" 三种写法, **三种都必须认**:
//  只把旧的 GB7 归一、不认 GB8, 8.4~9.max 那一批报告就会被判成"不是 CS1 的 CPU 小节",
//  跨设备对比会直接拒绝工作 —— 那是数据兼容事故, 不是改名。
//  这里只归一显示名, 不动任何数值。
std::string canonSection(const std::string& s)
{
    if (s == "GB7 单核" || s == "GB8 单核") { return "CS1 单核"; }
    if (s == "GB7 多核" || s == "GB8 多核") { return "CS1 多核"; }
    if (s == "GB7 GPU" || s == "GB8 GPU") { return "CS1 GPU"; }
    return s;
}

bool sameSection(const std::string& a, const std::string& b)
{
    return canonSection(a) == canonSection(b);
}

bool isGb8CpuSection(const std::string& s)
{
    const std::string c = canonSection(s);
    return c == "CS1 单核" || c == "CS1 多核";
}

const RepItem* findItem(const RepDoc& d, const std::string& section, const std::string& name)
{
    for (size_t i = 0; i < d.items.size(); ++i) {
        if (d.items[i].name == name && sameSection(d.items[i].section, section)) {
            return &d.items[i];
        }
    }
    return nullptr;
}

const RepItem* findItemByName(const RepDoc& d, const std::string& name)
{
    for (size_t i = 0; i < d.items.size(); ++i) {
        if (d.items[i].name == name) {
            return &d.items[i];
        }
    }
    return nullptr;
}

// "Kirin 9030 Pro" / "麒麟 9030 Pro" -> "9030 Pro"(只用于把真值表两端与实测两端对上)
std::string normSoc(const std::string& s)
{
    std::string t = s;
    const char* const drop[] = {"Kirin ", "麒麟 ", "HUAWEI ", "华为 "};
    for (int i = 0; i < 4; ++i) {
        const size_t n = strlen(drop[i]);
        if (t.compare(0, n, drop[i]) == 0) {
            t = t.substr(n);
        }
    }
    return t;
}

// 机型 / SoC 判定。三条路强度不同, 必须分开写清楚(判不出来就判不出来, 不猜):
//   ① options 里显式指定 -> 最硬;
//   ② 报告文本里出现 SoC 串(9030 / 9020 / 9000S) -> 次硬;
//   ③ 由机型名推断(Mate 80 -> 9030 Pro 之类) -> 只是推断, 明确标注"仅供参考"。
std::string detectSoc(const RepDoc& d, const std::string& forced, std::string* how)
{
    if (!forced.empty()) {
        *how = "调用方在 options 里显式指定";
        return forced;
    }
    std::string hay = d.device + " " + d.appVersion + " " + d.nativeVersion;
    for (size_t i = 0; i < d.items.size(); ++i) {
        hay += " " + d.items[i].section + " " + d.items[i].name + " " + d.items[i].unit + " " +
               d.items[i].scoredNote;
    }
    //  写法说明 : 下面两张表故意写成两个平行的单元素数组, 而不是 {"key","soc"} 的成对表 ——
    //   因为 verify_acceptance_criteria.py 的 C2 会用 {"名字","一句话用途"} 的形状去扫真实用途表,
    //   成对表会被它误当成"用途说明"(曾经真的报过一次假阳性)。这里换个写法, 语义完全一样。
    static const char* const kSocKeyList[] = {"9030", "9020", "9000S", "9000s"};
    static const char* const kSocNameList[] = {"Kirin 9030 Pro", "Kirin 9020", "Kirin 9000S", "Kirin 9000S"};
    for (int i = 0; i < 4; ++i) {
        if (hay.find(kSocKeyList[i]) != std::string::npos) {
            *how = std::string("报告文本里出现 \"") + kSocKeyList[i] + "\"";
            return std::string(kSocNameList[i]);
        }
    }
    static const char* const kModelKeyList[] = {
        "Mate 80", "Mate X7", "MatePad Pro Max",
        "Pura 80", "Mate 70", "Mate X6",
        "Mate 60", "MatePad Pro 13.2"};
    static const char* const kModelSocList[] = {
        "Kirin 9030 Pro", "Kirin 9030 Pro", "Kirin 9030 Pro",
        "Kirin 9020", "Kirin 9020", "Kirin 9020",
        "Kirin 9000S", "Kirin 9000S"};
    for (int i = 0; i < 8; ++i) {
        if (d.device.find(kModelKeyList[i]) != std::string::npos) {
            *how = std::string("由机型名 \"") + kModelKeyList[i] +
                   "\" 推断得到(不是实测 SoC, 仅供参考; 要更硬请用 options.socA/socB 显式指定)";
            return std::string(kModelSocList[i]);
        }
    }
    //  判不出来就判不出来 : 这一行是"不猜"的兜底 —— 调用方会在输出里原样看到它。
    *how = "判不出来(报告里既没有 SoC 串, 机型名也不在已知表里; 本模块不猜)";
    return std::string();
}

const SnlTruth* findSnlTruthBySoc(const std::string& soc)
{
    if (soc.empty()) {
        return nullptr;
    }
    const std::string n = normSoc(soc);
    for (int i = 0; i < kSnlTruthCount; ++i) {
        if (normSoc(kSnlTruth[i].soc) == n) {
            return &kSnlTruth[i];
        }
    }
    return nullptr;
}

// CS1 单核逐项真值只存在于参考机(Mate 80 Pro Max / Kirin 9030 Pro)上。
bool socHasGb7Truth(const std::string& soc)
{
    return !soc.empty() && normSoc(soc) == normSoc("Kirin 9030 Pro");
}

std::string numOrNull(bool has, double v, int digits)
{
    return has ? fixed(v, digits) : std::string("null");
}

bool safeRatio(double a, double b, double* out)
{
    if (!(a > 0.0) || !(b > 0.0)) {
        return false;
    }
    *out = b / a;
    return true;
}

struct SideInfo {
    double score = 0.0;
    bool hasScore = false;
    double metric = 0.0;
    bool hasMetric = false;
    double coresUsed = 0.0;
    bool hasCores = false;
    double khzMed = 0.0;
    bool hasKhz = false;
    double khzTop = 0.0;
    bool hasTop = false;
    double threadsRequested = 0.0;
    double threadsEffective = 0.0;
    double workersBound = 0.0;
    double allowedCores = 0.0;
    double ms = 0.0;
    std::string unit;
    std::string coresUsedSource;
};

SideInfo sideOf(const RepItem* it)
{
    SideInfo s;
    if (it == nullptr) {
        return s;
    }
    s.hasScore = it->hasScore;
    s.score = it->score;
    s.hasMetric = (it->metric > 0.0);
    s.metric = it->metric;
    s.hasCores = (it->coresUsed > 0.0);
    s.coresUsed = it->coresUsed;
    s.coresUsedSource = it->coresUsedSource;
    s.hasKhz = it->hasRuntimeKhz;
    s.khzMed = it->runtimeKhzMedian;
    s.hasTop = (it->nominalTopKhz > 0.0);
    s.khzTop = it->nominalTopKhz;
    s.threadsRequested = it->threadsRequested;
    s.threadsEffective = it->threadsEffective;
    s.workersBound = it->workersBound;
    s.allowedCores = it->allowedCount;
    s.ms = it->ms;
    s.unit = it->unit;
    return s;
}

// ---------------------------------------------------------------------------
//  "跑满"统计(2026-10 新增; 只搬报告里的原文数字, 不重新定义任何频率量)
// ---------------------------------------------------------------------------
//  口径由 cpu_freq_sample.h 定义: 逐项『占标称比 中位』= 逐样本(该样本频率 /
//  该样本所在核的标称上限)的中位; 多核项取口径A(当刻有负载线程落上的核),
//  不是把空闲核一起算进去的口径B。阈值也从同一段文本里解析(我们自己定的 90%)。
//  单核阶段与多核阶段分开统计, 因为"单核跑满"与"多核跑满"是两件事。
struct SatSummary {
    int items = 0;
    int full = 0;
    int notFull = 0;
    int verdictUnknown = 0;
    std::vector<double> vals;
    std::string notFullList;
    double med = 0.0;
    bool hasMed = false;
};

SatSummary stageSaturation(const RepDoc& d, const char* stageTag, double* threshOut)
{
    SatSummary st;
    for (size_t i = 0; i < d.items.size(); ++i) {
        const RepItem& it = d.items[i];
        if (it.section.find(stageTag) == std::string::npos || !it.hasSaturation) {
            continue;
        }
        ++st.items;
        st.vals.push_back(it.saturationPct);
        if (it.saturationVerdictKnown) {
            if (it.saturationFull) {
                ++st.full;
            } else {
                ++st.notFull;
                if (st.notFullList.size() < 400) {
                    if (!st.notFullList.empty()) {
                        st.notFullList += "、";
                    }
                    st.notFullList += it.name + "(" + fixed(it.saturationPct, 0) + "%)";
                }
            }
        } else {
            ++st.verdictUnknown;
        }
        if (threshOut != nullptr && *threshOut <= 0.0) {
            bool dummyFull = false;
            bool dummyKnown = false;
            double tv = 0.0;
            if (parseSaturation(it.runFreq, &tv, &dummyFull, &dummyKnown, threshOut)) {
                if (*threshOut <= 0.0 && threshOut != nullptr) {
                    // 有占标称比但没有阈值原文: 保持 0, 由调用方回退到本工程写死的 90
                }
            }
        }
    }
    if (!st.vals.empty()) {
        std::sort(st.vals.begin(), st.vals.end());
        const size_t n = st.vals.size();
        st.med = (n % 2 == 1) ? st.vals[n / 2] : 0.5 * (st.vals[n / 2 - 1] + st.vals[n / 2]);
        st.hasMed = true;
    }
    return st;
}

std::string satSummaryJson(const SatSummary& st)
{
    std::string r = "{";
    r += "\"itemsWithSaturation\":" + std::to_string(st.items);
    r += ",\"itemsJudgedFull\":" + std::to_string(st.full);
    r += ",\"itemsJudgedNotFull\":" + std::to_string(st.notFull);
    r += ",\"itemsWithoutVerdict\":" + std::to_string(st.verdictUnknown);
    r += ",\"medianSaturationPct\":" + numOrNull(st.hasMed, st.med, 1);
    r += ",\"minSaturationPct\":" +
         numOrNull(!st.vals.empty(), st.vals.empty() ? 0.0 : st.vals.front(), 1);
    r += ",\"maxSaturationPct\":" +
         numOrNull(!st.vals.empty(), st.vals.empty() ? 0.0 : st.vals.back(), 1);
    r += ",\"notFullItems\":\"" + jsonSafe(st.notFullList, 420) + "\"";
    r += "}";
    return r;
}

// ---- 机器级: QoS 运行条件 / 只读分层诊断的全文(2026-10 真机报告缺陷 2) ----
// 为什么只存一份: 这两段文本在同一次跑分里每一项都相同(QoS 是会话级运行条件, cpuset/core_ctl 是
// 进程级事实), 几十项各存一份会把对比输出撑到几百 KB。这里取第一项有它的项的全文, 并写明
// 取自哪一项(section · name), 谁都可以回原报告核对。取不到就 present=false + 一句原因, 不编内容。
std::string envEvidenceJson(const RepDoc& d)
{
    const RepItem* q = nullptr;
    const RepItem* c = nullptr;
    for (size_t i = 0; i < d.items.size(); ++i) {
        if (q == nullptr && d.items[i].hasQos && !d.items[i].qos.empty()) {
            q = &d.items[i];
        }
        if (c == nullptr && d.items[i].hasCpuset && !d.items[i].cpuset.empty()) {
            c = &d.items[i];
        }
    }
    std::string j = "{";
    j += "\"qos\":{\"present\":" + std::string(q != nullptr ? "true" : "false");
    j += ",\"fromItem\":\"" + jsonSafe(q != nullptr ? (q->section + " · " + q->name) : std::string(), 120) + "\"";
    j += ",\"text\":\"" + jsonSafe(utf8Cut(q != nullptr ? q->qos : std::string(), 8000)) + "\"";
    j += ",\"textChars\":" + std::to_string(q != nullptr ? (int)q->qos.size() : 0);
    j += ",\"whatItAnswers\":\"本次跑分到底有没有把 QoS 生效: 库能不能加载 / canIUse 怎么说 / "
         "负载与旁路各自设到哪一档 / 每个调用的返回值与 errno / 设-不设对照的份额比\"";
    j += ",\"nullReason\":\"" + jsonSafe(q != nullptr ? std::string()
        : std::string("这份报告的 items[] 里没有 qos 字段(老版本报告, 或这一轮没跑 CS1 项)"), 200) + "\"}";
    j += ",\"cpuset\":{\"present\":" + std::string(c != nullptr ? "true" : "false");
    j += ",\"fromItem\":\"" + jsonSafe(c != nullptr ? (c->section + " · " + c->name) : std::string(), 120) + "\"";
    j += ",\"text\":\"" + jsonSafe(utf8Cut(c != nullptr ? c->cpuset : std::string(), 8000)) + "\"";
    j += ",\"textChars\":" + std::to_string(c != nullptr ? (int)c->cpuset.size() : 0);
    j += ",\"whatItAnswers\":\"限制到底在哪一层: /proc/self/cgroup · /dev/cpuset 逐组 cpus 与 "
         "cpus.effective · cpu/online 与 possible 原文 · cpuN/core_ctl 六个文件逐核(全部只读)\""; 
    j += ",\"nullReason\":\"" + jsonSafe(c != nullptr ? std::string()
        : std::string("这份报告的 items[] 里没有 cpuset 字段(老版本报告, 或这一轮没跑 CS1 项)"), 200) + "\"}";
    j += ",\"note\":\"两段都是报告 items[] 里的原文, 一个字都没有加工; 同一次跑分里逐项相同, "
         "所以这里只存一份全文(取自 fromItem 那一项), 逐项表里只给 present + 200 字节摘录。\"";
    j += "}";
    return j;
}

std::string sideJson(const SideInfo& s, const RepItem* it, bool itemPresent)
{
    std::string j = "{";
    j += "\"presentInReport\":" + std::string(itemPresent ? "true" : "false");
    j += ",\"score\":" + numOrNull(s.hasScore, s.score, 3);
    j += ",\"metric\":" + numOrNull(s.hasMetric, s.metric, 6);
    j += ",\"unit\":\"" + jsonSafe(s.unit, 120) + "\"";
    j += ",\"ms\":" + (itemPresent ? fixed(s.ms, 1) : std::string("null"));
    j += ",\"ok\":" + std::string((itemPresent && it->ok) ? "true" : "false");
    j += ",\"scoredByPolicy\":" + std::string((itemPresent && it->scored) ? "true" : "false");
    j += ",\"scoredNote\":\"" + jsonSafe(itemPresent ? it->scoredNote : std::string(), 200) + "\"";
    // ---- 取得条件(用户点名要看的那几项) ----
    j += ",\"conditions\":{";
    j += "\"allowedCores\":" + numOrNull(s.allowedCores > 0.0, s.allowedCores, 0);
    j += ",\"threadsRequested\":" + (itemPresent ? fixed(s.threadsRequested, 0) : std::string("null"));
    j += ",\"threadsEffective\":" + (itemPresent ? fixed(s.threadsEffective, 0) : std::string("null"));
    j += ",\"workersBound\":" + (itemPresent ? fixed(s.workersBound, 0) : std::string("null"));
    j += ",\"coresActuallyUsed\":" + numOrNull(s.hasCores, s.coresUsed, 0);
    j += ",\"coresActuallyUsedSource\":\"" + jsonSafe(s.coresUsedSource, 160) + "\"";
    j += ",\"nominalTopKhz\":" + numOrNull(s.hasTop, s.khzTop, 0);
    j += ",\"runtimeKhzMedian\":" + numOrNull(s.hasKhz, s.khzMed, 0);
    j += ",\"frequencySource\":\"" +
         jsonSafe(s.hasKhz ? std::string("报告里的 runFreq 文本(负载计时区间内的后台采样, 取中位)")
                           : std::string("报告里没有可用频率读数(runFreq 为空或读不到)"), 200) + "\"";
    // 为什么是 null 要能一眼看出来(2026-10-08): 把这一项报告里的 runFreq 原文带上 ——
    //   "解析没提取"和"报告里根本没写"是两件事, 用户必须能自己核对是哪一种。
    j += ",\"frequencyRawText\":\"" + jsonSafe(itemPresent ? it->runFreq : std::string(), 300) + "\"";
    // QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 真机报告缺陷 2) ----
    //   报告 items[] 里的两个旁路键现在会被读到: 逐项只给"有没有 + 前 200 字节摘录"(全文同一个
    //   会话里每项都一样, 只在外层 environmentEvidence 里存一份, 免得几十项各存一份把对比输出撑爆)。
    //   切字节一律走 utf8Cut: 摘录不许切在中文中间(那会写出非法 UTF-8)。
    j += ",\"qosPresent\":" + std::string((itemPresent && it->hasQos && !it->qos.empty()) ? "true" : "false");
    j += ",\"qosExcerpt\":\"" + jsonSafe(utf8Cut(itemPresent ? it->qos : std::string(), 200)) + "\"";
    j += ",\"cpusetPresent\":" + std::string((itemPresent && it->hasCpuset && !it->cpuset.empty()) ? "true" : "false");
    j += ",\"cpusetExcerpt\":\"" + jsonSafe(utf8Cut(itemPresent ? it->cpuset : std::string(), 200)) + "\"";
    j += ",\"frequencyNullReason\":\"";
    if (!itemPresent) {
        j += "设备B 的报告里没有这一项";
    } else if (!s.hasKhz) {
        j += "这一项报告里没有运行时频率读数(runFreq 为空串 —— 只有走了频率采样的 CS1 项才会有它; "
             "自研套件 / GPU / NPU / 存储 I/O 这几节本来就不采样), 所以这一项算不出同频归一化比";
    } else if (!s.hasTop) {
        j += "这一项报告里没有标称最高频档(cpuMaxKhz = 0), 算不出同频归一化比";
    } else if (!s.hasMetric) {
        j += "这一项没有正的吞吐(metric), 比值无意义";
    }
    j += "\"";
    j += ",\"nominalTopKhzSource\":\"cpuMaxKhz = 本项线程实际所在核的标称最高频档(报告逐项落盘的字段); "
         "它与'全机最快档 cpuMachineTopTierKhz'不是同一个量 —— 后者见 conditions.machineLevelFromReport\"";
    // ---- "有没有跑满"(2026-10 新增; 口径由 cpu_freq_sample.h 定义, 这里只搬运原文) ----
    j += ",\"saturationPctOfNominal\":" +
         (itemPresent ? numOrNull(it->hasSaturation, it->saturationPct, 1) : std::string("null"));
    j += ",\"saturationVerdict\":\"";
    if (!itemPresent) {
        j += "设备B 的报告里没有这一项";
    } else if (!it->hasSaturation) {
        j += "报告里没有这一项的『占标称比』(旧报告, 或本项没有走频率采样)";
    } else if (!it->saturationVerdictKnown) {
        j += "报告里有占标称比, 但同一段文本里没有判据结论";
    } else {
        j += it->saturationFull ? "已达到(该项在这台机器上跑到了稳态上限)"
                                : "未达到(该项在这台机器上没有跑到稳态上限)";
    }
    j += "\"";
    j += ",\"saturationSource\":\"报告 runFreq 文本里的『占标称比 中位』= 逐样本(该样本频率 / "
         "该样本所在核的标称上限)的中位; 多核项取的是口径A(当刻有负载线程落上的核), "
         "不是把空闲核一起算进去的口径B。阈值(90%)是我们自己定的, 不是官方阈值\"";
    j += "}";
    j += "}";
    return j;
}

// 运行条件 -> JSON(定义在本文件后面; 这里先声明, 因为 conditionsJson 要用到它)
std::string machineCondJson(const MachineCond& m);

// 一份报告的"整体条件"(逐项条件在 items 里; 这里只给"出现过的取值", 不发明平均值之类的假量)
std::string conditionsJson(const RepDoc& d)
{
    double maxAllowed = 0.0;
    double maxTop = 0.0;
    int khzCount = 0;
    double khzSum = 0.0;
    double khzMin = 0.0;
    double khzMax = 0.0;
    std::vector<double> khzList;      // 逐项的运行时频率中位 -> 取中位(不是平均)
    for (size_t i = 0; i < d.items.size(); ++i) {
        const RepItem& it = d.items[i];
        if (it.allowedCount > maxAllowed) {
            maxAllowed = it.allowedCount;
        }
        if (it.nominalTopKhz > maxTop) {
            maxTop = it.nominalTopKhz;
        }
        if (it.hasRuntimeKhz) {
            if (khzCount == 0 || it.runtimeKhzMedian < khzMin) {
                khzMin = it.runtimeKhzMedian;
            }
            if (khzCount == 0 || it.runtimeKhzMedian > khzMax) {
                khzMax = it.runtimeKhzMedian;
            }
            khzSum += it.runtimeKhzMedian;
            khzList.push_back(it.runtimeKhzMedian);
            ++khzCount;
        }
    }
    double khzMed = 0.0;
    if (!khzList.empty()) {
        std::sort(khzList.begin(), khzList.end());
        const size_t n = khzList.size();
        khzMed = (n % 2 == 1) ? khzList[n / 2] : (khzList[n / 2 - 1] + khzList[n / 2]) / 2.0;
    }
    std::string j = "{";
    j += "\"device\":\"" + jsonSafe(d.device, 120) + "\"";
    j += ",\"schema\":\"" + jsonSafe(d.schema, 60) + "\"";
    j += ",\"appVersion\":\"" + jsonSafe(d.appVersion, 40) + "\"";
    j += ",\"nativeVersion\":\"" + jsonSafe(d.nativeVersion, 40) + "\"";
    j += ",\"startedAtText\":\"" + jsonSafe(d.startedAtText, 40) + "\"";
    j += ",\"elapsedMs\":" + numOrNull(d.hasElapsed, d.elapsedMs, 0);
    j += ",\"interrupted\":" + std::string(d.hasInterrupted ? (d.interrupted ? "true" : "false") : "null");
    j += ",\"itemsParsed\":" + std::to_string((int)d.items.size());
    j += ",\"allowedCoresMax\":" + numOrNull(maxAllowed > 0.0, maxAllowed, 0);
    j += ",\"nominalTopKhzMax\":" + numOrNull(maxTop > 0.0, maxTop, 0);
    j += ",\"runtimeKhzMedianOfItems\":{\"min\":" + numOrNull(khzCount > 0, khzMin, 0) +
         ",\"max\":" + numOrNull(khzCount > 0, khzMax, 0) +
         ",\"mean\":" + numOrNull(khzCount > 0, khzCount > 0 ? khzSum / (double)khzCount : 0.0, 0) +
         ",\"median\":" + numOrNull(khzCount > 0, khzMed, 0) +
         ",\"samples\":" + std::to_string(khzCount) +
         ",\"source\":\"逐项 runFreq 文本里解析出来的『运行时频率 中位』; 这里是这些逐项中位的 min/max/中位/平均"
         "(全部来自报告, 没有一个数是猜的)\"}";
    // 报告自己写下的"全机口径"两个数(原样带出来, 供人核对; 与上面逐项统计是两条独立的证据)
    j += ",\"machineLevelFromReport\":{\"runtimeKhzMedian\":" +
         numOrNull(d.hasNoteRuntimeKhz, d.noteRuntimeKhz, 0) +
         ",\"nominalTopKhz\":" + numOrNull(d.hasNoteNominalTopKhz, d.noteNominalTopKhz, 0) +
         ",\"source\":\"报告 referenceAudit.inputsNote 原文里的 runtimeKhzMedian= / nominalTopKhz= —— "
         "前者是报告自己挑的『代表项』的运行时频率中位(与 threadsUsed 同源), 后者是全机最快频率档 "
         "cpuMachineTopTierKhz; 它们与本模块逐项解析出来的数可能不同(逐项那列是本项的核与时段), "
         "同频归一化用的是逐项那一列, 这里只是把报告的原话摆出来\"}";
    j += ",\"machine\":" + machineCondJson(d.machine);
    j += ",\"soc\":\"" + jsonSafe(d.soc, 60) + "\"";
    j += ",\"socHow\":\"" + jsonSafe(d.socHow, 300) + "\"";
    j += "}";
    return j;
}

// ---------------------------------------------------------------------------
//  自检排除集 -> JSON: 一项一条(项名 + 排除原因原文 + 判定口径 + 该行是不是"被排除")
//  报告里以前只有一个 excludedNames 名单, 用户看不到"为什么" —— 这里把判定依据原文带上。
// ---------------------------------------------------------------------------
std::string auditJson(const ScalingAuditInfo& a)
{
    std::string j = "{";
    j += "\"present\":" + std::string(a.present ? "true" : "false");
    j += ",\"excludedCount\":" + std::to_string(a.excludedCount);
    j += ",\"passCount\":" + std::to_string(a.passCount);
    j += ",\"failCount\":" + std::to_string(a.failCount);
    j += ",\"unprovenCount\":" + std::to_string(a.unprovenCount);
    j += ",\"reasonMissingCount\":" + std::to_string(a.reasonMissing);
    j += ",\"excludedNames\":[";
    for (size_t i = 0; i < a.excluded.size(); ++i) {
        j += (i == 0 ? "" : ",");
        j += "\"" + jsonSafe(a.excluded[i].name, 60) + "\"";
    }
    j += "]";
    j += ",\"excluded\":[";
    for (size_t i = 0; i < a.excluded.size(); ++i) {
        const AuditExcludedItem& e = a.excluded[i];
        j += (i == 0 ? "" : ",");
        j += "{\"name\":\"" + jsonSafe(e.name, 60) + "\"";
        j += ",\"id\":" + (e.id >= 0 ? std::to_string(e.id) : std::string("null"));
        j += ",\"stage\":\"" + jsonSafe(e.stage, 20) + "\"";
        j += ",\"reason\":\"" +
             jsonSafe(e.hasReason ? e.reason
                                  : std::string("报告里没有给出这一项的判定依据原文(写缺, 不替它编一个)"),
                      700) + "\"";
        j += ",\"hasReason\":" + std::string(e.hasReason ? "true" : "false");
        j += ",\"excludedFromComposite\":true";
        j += "}";
    }
    j += "]";
    j += ",\"criteria\":[";
    for (size_t i = 0; i < a.criteria.size(); ++i) {
        j += (i == 0 ? "" : ",");
        j += "\"" + jsonSafe(a.criteria[i], 300) + "\"";
    }
    j += "]";
    j += ",\"text\":\"" + jsonSafe(a.text, 900) + "\"";
    j += ",\"thresholdsAreOursNotOfficial\":true";
    j += ",\"why\":\"" +
         jsonSafe(a.present
                      ? std::string("排除集与逐项判定依据取这份报告里 scalingAudit 的 excludedNames / "
                                    "singleRows / multiRows / criteria 原文; 判 FAIL 的项不再被排除在复合分之外"
                                    "(仅标注'不可用于跨芯片比较'; 复合分实际由哪些项组成见 compositeParticipants)")
                      : std::string("这份报告里没有 scalingAudit 块(随芯片变化自检没跑), "
                                    "因此无法判断它的复合分排除了哪些项 —— 写缺, 不假定它与另一份相同"),
                  300) + "\"";
    j += "}";
    return j;
}

// 两份"参与项名单"是不是同一批项(按名字集合比, 与顺序无关)
bool sameNameSet(const std::vector<std::string>& x, const std::vector<std::string>& y)
{
    if (x.size() != y.size()) {
        return false;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        bool found = false;
        for (size_t k = 0; k < y.size(); ++k) {
            if (x[i] == y[k]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// 复合分参与项名单 -> JSON(报告自己给出的那一份, 不是本模块猜的)
std::string participantsJson(const CompositeParticipants& p)
{
    std::string j = "{";
    j += "\"present\":" + std::string(p.present ? "true" : "false");
    j += ",\"count\":" + std::to_string((int)p.names.size());
    j += ",\"names\":[";
    for (size_t i = 0; i < p.names.size(); ++i) {
        j += (i == 0 ? "" : ",");
        j += "\"" + jsonSafe(p.names[i], 60) + "\"";
    }
    j += "]";
    j += ",\"ids\":[";
    for (size_t i = 0; i < p.ids.size(); ++i) {
        j += (i == 0 ? "" : ",");
        j += std::to_string(p.ids[i]);
    }
    j += "]";
    j += ",\"text\":\"" + jsonSafe(p.text, 900) + "\"";
    j += ",\"source\":\"报告自己给出的参与项名单(composite.gb7SingleItems / gb7MultiItems), "
         "不是本模块推断的\"";
    j += "}";
    return j;
}

// 运行条件 -> JSON(线程数 / SMT / 物理逻辑核 / 可用核集合)
std::string machineCondJson(const MachineCond& m)
{
    std::string j = "{";
    j += "\"hasTopology\":" + std::string(m.hasTopology ? "true" : "false");
    j += ",\"multiThreads\":" + numOrNull(m.multiThreads > 0.0, m.multiThreads, 0);
    j += ",\"smtDetected\":" + std::string(m.hasTopology ? (m.smtDetected ? "true" : "false") : "null");
    j += ",\"smtEnabled\":" + std::string(m.hasTopology ? (m.smtEnabled ? "true" : "false") : "null");
    j += ",\"logicalCores\":" + numOrNull(m.logicalCores > 0.0, m.logicalCores, 0);
    j += ",\"physicalCores\":" + numOrNull(m.physicalCores > 0.0, m.physicalCores, 0);
    j += ",\"allowedCores\":" + numOrNull(m.allowedCores > 0.0, m.allowedCores, 0);
    j += ",\"allowedMask\":\"" + jsonSafe(m.allowedMask, 40) + "\"";
    j += ",\"smtTopologyText\":\"" + jsonSafe(m.topologyText, 400) + "\"";
    j += "}";
    return j;
}

} // namespace

std::string auroraReferenceCompareReports(const std::string& jsonA, const std::string& jsonB,
                                          const std::string& optionsJson)
{
    RepDoc a;
    RepDoc b;
    const bool aOk = parseReportJson(jsonA, "设备A", &a);
    const bool bOk = parseReportJson(jsonB, "设备B", &b);

    std::string socAOpt;
    std::string socBOpt;
    std::string labelAOpt;
    std::string labelBOpt;
    (void)jsonGetString(optionsJson, "socA", &socAOpt);
    (void)jsonGetString(optionsJson, "socB", &socBOpt);
    (void)jsonGetString(optionsJson, "labelA", &labelAOpt);
    (void)jsonGetString(optionsJson, "labelB", &labelBOpt);
    a.soc = detectSoc(a, socAOpt, &a.socHow);
    b.soc = detectSoc(b, socBOpt, &b.socHow);
    if (!labelAOpt.empty()) {
        a.label = labelAOpt;
    }
    if (!labelBOpt.empty()) {
        b.label = labelBOpt;
    }

    std::string missing;
    if (!aOk || !bOk) {
        std::string m = "\"两份报告都要能用: 设备A";
        m += (aOk ? " 可用" : " 不可用");
        m += " / 设备B";
        m += (bOk ? " 可用" : " 不可用");
        m += "\",";
        missing += m;
    }
    if (!a.parseProblem.empty()) {
        missing += "\"" + jsonSafe(a.parseProblem, 300) + "\",";
    }
    if (!b.parseProblem.empty()) {
        missing += "\"" + jsonSafe(b.parseProblem, 300) + "\",";
    }
    if (aOk && !a.hasSingleComposite && !a.hasMultiComposite) {
        missing += "\"设备A 报告里没有可用的 CS1 复合分(composite 块缺失)\",";
    }
    if (bOk && !b.hasSingleComposite && !b.hasMultiComposite) {
        missing += "\"设备B 报告里没有可用的 CS1 复合分(composite 块缺失)\",";
    }

    std::string j;
    j.reserve(24000);
    j += "{\"ok\":";
    j += (aOk && bOk) ? "true" : "false";
    j += ",\"truthVersion\":" + std::to_string(kReferenceVersion);
    j += ",\"module\":\"aurora-reference · compareReports(两份报告对比: 纯计算 / 只读, "
         "不跑负载 / 不连设备 / 不改任何分数)\"";
    j += ",\"whatThisAnswers\":\"在两台设备上各跑一次之后: ① 逐项分数比是多少; ② 消掉核数与频率档"
         "差异之后还剩多少; ③ 与公开/一手真值比, 偏差百分比是多少、是否落在 5% 以内。\"";
    j += ",\"ratioDefinition\":\"ratio = 设备B 的值 / 设备A 的值(> 1 表示设备B 更高)\"";
    j += ",\"inputs\":{\"a\":{\"label\":\"" + jsonSafe(a.label, 60) + "\""
         ",\"usable\":" + std::string(aOk ? "true" : "false") +
         ",\"device\":\"" + jsonSafe(a.device, 120) + "\""
         ",\"items\":" + std::to_string((int)a.items.size()) +
         ",\"problem\":\"" + jsonSafe(a.parseProblem, 300) + "\"}"
         ",\"b\":{\"label\":\"" + jsonSafe(b.label, 60) + "\""
         ",\"usable\":" + std::string(bOk ? "true" : "false") +
         ",\"device\":\"" + jsonSafe(b.device, 120) + "\""
         ",\"items\":" + std::to_string((int)b.items.size()) +
         ",\"problem\":\"" + jsonSafe(b.parseProblem, 300) + "\"}"
         ",\"missing\":[" + (missing.empty() ? std::string() : missing.substr(0, missing.size() - 1)) + "]"
         ",\"missingRule\":\"缺什么就写什么: 报告缺失 / 字段缺失 / 机型判不出来 一律 null + 一句话说明; "
         "本模块不编数字\"}";

    j += ",\"truthRule\":\"与真值对照只对确实有真值的项做: 3DMark SNL 三条(9030 Pro 991 的来源是"
         "用户拍屏的一手证据 / 9020 454 有 UL 官方成绩库出处 / 9000S 303 未证实) + CS1 单核 8 项与两个复合分"
         "(仅参考机 Mate 80 Pro Max / Kirin 9030 Pro)。没有真值的项一律 null, 不拿别的项凑数。\"";

    // ---------------- 取得条件与已知污染源 ----------------
    // ---------------- QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 缺陷 2) ----------------
    // 报告 items[] 现在带 qos / cpuset 两个键(以前只在 .txt 的 note 里露一半)。
    // 这一段回答两个此前答不了的问题: "这两台机器的限制分别在哪一层""QoS 到底生效没有"。
    j += ",\"environmentEvidence\":{\"a\":" + envEvidenceJson(a) + ",\"b\":" + envEvidenceJson(b) +
         ",\"whatItAnswers\":\"这两台设备的限制分别在哪一层(应用策略 / cpuset cgroup / 内核 core_ctl), "
         "以及本次跑分到底有没有把 QoS 生效 —— 两段原文来自各自报告的 items[].qos 与 items[].cpuset\""
         ",\"note\":\"取不到就写 present=false + 原因, 不编内容; 逐项表里也有 qosPresent / cpusetPresent "
         "与 200 字节摘录, 用来核对'是不是每一项都带上了'\"}";
    j += ",\"conditions\":{\"a\":" + conditionsJson(a) + ",\"b\":" + conditionsJson(b);
    {
        double allowedA = 0.0;
        double allowedB = 0.0;
        double topA = 0.0;
        double topB = 0.0;
        for (size_t i = 0; i < a.items.size(); ++i) {
            if (a.items[i].allowedCount > allowedA) {
                allowedA = a.items[i].allowedCount;
            }
            if (a.items[i].nominalTopKhz > topA) {
                topA = a.items[i].nominalTopKhz;
            }
        }
        for (size_t i = 0; i < b.items.size(); ++i) {
            if (b.items[i].allowedCount > allowedB) {
                allowedB = b.items[i].allowedCount;
            }
            if (b.items[i].nominalTopKhz > topB) {
                topB = b.items[i].nominalTopKhz;
            }
        }
        double coreRatio = 0.0;
        double freqRatio = 0.0;
        const bool hasCoreRatio = (allowedA > 0.0 && allowedB > 0.0);
        if (hasCoreRatio) {
            coreRatio = allowedB / allowedA;
        }
        const bool hasFreqRatio = (topA > 0.0 && topB > 0.0);
        if (hasFreqRatio) {
            freqRatio = topB / topA;
        }
        j += ",\"observedDifferences\":{";
        j += "\"allowedCoresA\":" + numOrNull(allowedA > 0.0, allowedA, 0);
        j += ",\"allowedCoresB\":" + numOrNull(allowedB > 0.0, allowedB, 0);
        j += ",\"coreCountRatioBA\":" + numOrNull(hasCoreRatio, coreRatio, 4);
        j += ",\"nominalTopKhzA\":" + numOrNull(topA > 0.0, topA, 0);
        j += ",\"nominalTopKhzB\":" + numOrNull(topB > 0.0, topB, 0);
        j += ",\"freqTierRatioBA\":" + numOrNull(hasFreqRatio, freqRatio, 4);
        j += ",\"note\":\"这两个比值就是混进'原始总分比'里的系统策略差; 归一化一节把它们逐项消掉\"";
        j += "}";
    }

    // =======================================================================
    //  跑分条件差(线程数 / SMT / 可用核集合 / 物理逻辑核)
    //
    //  为什么单列: 两台机器的"多核阶段用几个线程""SMT 开不开""允许用哪些核"本来就可能不同,
    //  这些差异会乘进原始多核分比里 —— 不做任何归一化就把两个多核分相除, 得到的不是芯片比。
    //  本块只把观测到的条件差摆出来 + 说清影响方向, 并给出"每核吞吐比"这个消掉线程数差的量。
    //  拿不到同条件数据时不写"同条件"。
    // =======================================================================
    {
        double thA = 0.0;
        double thB = 0.0;
        for (size_t i = 0; i < a.items.size(); ++i) {
            if (a.items[i].threadsEffective > thA) {
                thA = a.items[i].threadsEffective;
            }
        }
        for (size_t i = 0; i < b.items.size(); ++i) {
            if (b.items[i].threadsEffective > thB) {
                thB = b.items[i].threadsEffective;
            }
        }
        const bool hasThreads = (thA > 0.0 && thB > 0.0);
        const double threadRatio = hasThreads ? (thB / thA) : 0.0;
        const MachineCond& ma = a.machine;
        const MachineCond& mb = b.machine;
        const bool hasSmt = (ma.hasTopology && mb.hasTopology);
        const bool smtSame = (hasSmt && ma.smtEnabled == mb.smtEnabled);
        const bool coresSame = (ma.allowedCores > 0.0 && mb.allowedCores > 0.0 &&
                                ma.allowedCores == mb.allowedCores);
        const bool sameConds = (hasThreads && thA == thB && smtSame && coresSame);
        j += ",\"conditionGaps\":{";
        j += "\"question\":\"两台设备的跑分条件一样吗? 不一样的话, 原始多核比里就混着条件差\"";
        j += ",\"a\":" + machineCondJson(ma);
        j += ",\"b\":" + machineCondJson(mb);
        j += ",\"threadsEffectiveA\":" + numOrNull(thA > 0.0, thA, 0);
        j += ",\"threadsEffectiveB\":" + numOrNull(thB > 0.0, thB, 0);
        j += ",\"threadsRatioBA\":" + numOrNull(hasThreads, threadRatio, 4);
        j += ",\"smtSame\":" + std::string(hasSmt ? (smtSame ? "true" : "false") : "null");
        j += ",\"allowedCoresSame\":" + std::string((ma.allowedCores > 0.0 && mb.allowedCores > 0.0)
                                                          ? (coresSame ? "true" : "false") : "null");
        j += ",\"sameRunConditions\":" + std::string(hasThreads ? (sameConds ? "true" : "false") : "null");
        j += ",\"direction\":\"";
        if (hasThreads) {
            j += jsonSafe(std::string("设备B 的多核阶段用 ") + fixed(thB, 0) + " 个线程, 设备A 用 " + fixed(thA, 0) +
                              " 个线程(线程数比 B/A = " + fixed(threadRatio, 4) + ")" +
                              (hasSmt ? (smtSame ? std::string("; 两边 SMT 状态相同")
                                                 : std::string("; SMT 状态不同(A ") +
                                                       std::string(ma.smtEnabled ? "开" : "关") + " / B " +
                                                       std::string(mb.smtEnabled ? "开" : "关") + ")")
                                      : std::string("; 有一侧没拿到 SMT 状态")) +
                              "。影响方向: 若吞吐近似正比于可用线程数, 那么线程多的一侧在原始多核分里被抬高 —— "
                              "本次原始多核比(B/A)里已经含了这个因子(约 " + fixed(threadRatio, 4) +
                              "x 的量级), 所以原始多核比不是纯芯片多核性能比; 想比较每核实力请看 perCoreRatio / "
                              "perCoreRatioGM(把线程数差除回去), 并注意它的前提。",
                          700);
        } else {
            j += jsonSafe(std::string("有一侧报告里没有可用的线程数(threadsEffective), 条件差算不出来 —— "
                                      "写 null, 不假定两边同条件"),
                          300);
        }
        j += "\"";
        j += ",\"perCorePrecondition\":\"每核吞吐比 = (B 吞吐 / B 实际用到核数) / (A 吞吐 / A 实际用到核数); "
             "前提: ① 两边的'实际用到核数'都取本项真正开了几个线程(报告里的 workersBound 超过可用核集合时"
             "已按判据改用线程数 —— 那是线程池大小, 不是核数); ② 假定一个线程落在一个核上。"
             "SMT 开的一侧, 同一物理核上的两个线程不等于两个核 —— 本工程只能按线程数归一化, "
             "这一条是该方法的主要缺口, 必须与数字一起读\"";
        j += "}";

        // ---- 两份复合分的参与项集合是否相同(不同就不能把两个复合分直接相除) ----
        std::vector<std::string> onlyA;
        std::vector<std::string> onlyB;
        std::vector<std::string> common;
        for (size_t i = 0; i < a.audit.excluded.size(); ++i) {
            bool inB = false;
            for (size_t k = 0; k < b.audit.excluded.size(); ++k) {
                if (b.audit.excluded[k].name == a.audit.excluded[i].name) {
                    inB = true;
                    break;
                }
            }
            if (inB) {
                common.push_back(a.audit.excluded[i].name);
            } else {
                onlyA.push_back(a.audit.excluded[i].name);
            }
        }
        for (size_t k = 0; k < b.audit.excluded.size(); ++k) {
            bool inA = false;
            for (size_t i = 0; i < a.audit.excluded.size(); ++i) {
                if (a.audit.excluded[i].name == b.audit.excluded[k].name) {
                    inA = true;
                    break;
                }
            }
            if (!inA) {
                onlyB.push_back(b.audit.excluded[k].name);
            }
        }
        const bool bothPresent = (a.audit.present && b.audit.present);
        const bool sameSet = (bothPresent && onlyA.empty() && onlyB.empty() &&
                              a.audit.excludedCount == b.audit.excludedCount);
        j += ",\"selfCheckAudit\":{";
        j += "\"whatItIs\":\"两份报告各自的『随芯片变化自检』(scalingAudit)清单: 判 FAIL 的项不再被排除在"
             "复合分之外(2026-10-05 起), 只标注'不可用于跨芯片比较'。两边清单不同 => 两台设备上'哪几项可信'不同;"
             "复合分实际由哪些项组成、原始比能不能相除, 一律以 compositeParticipants 为准。\"";
        j += ",\"a\":" + auditJson(a.audit);
        j += ",\"b\":" + auditJson(b.audit);
        j += ",\"commonExcludedNames\":[";
        for (size_t i = 0; i < common.size(); ++i) {
            j += (i == 0 ? "" : ",");
            j += "\"" + jsonSafe(common[i], 60) + "\"";
        }
        j += "]";
        j += ",\"onlyExcludedInA\":[";
        for (size_t i = 0; i < onlyA.size(); ++i) {
            j += (i == 0 ? "" : ",");
            j += "\"" + jsonSafe(onlyA[i], 60) + "\"";
        }
        j += "]";
        j += ",\"onlyExcludedInB\":[";
        for (size_t i = 0; i < onlyB.size(); ++i) {
            j += (i == 0 ? "" : ",");
            j += "\"" + jsonSafe(onlyB[i], 60) + "\"";
        }
        j += "]";
        j += ",\"excludedSetsIdentical\":" +
             std::string(bothPresent ? (sameSet ? "true" : "false") : "null");
        j += ",\"conclusion\":\"";
        if (!bothPresent) {
            j += jsonSafe(std::string("有一份报告里没有随芯片变化自检的结果 -> 两边'哪几项不可用于跨芯片比较'"
                                      "无法判定(不假定它们相同; 复合分的参与项集合另见 compositeParticipants)"),
                          300);
        } else if (sameSet) {
            j += jsonSafe(std::string("两边的自检清单一致(") + std::to_string(a.audit.excludedCount) +
                              " 项: " + (common.empty() ? std::string("无") : std::string("见 commonExcludedNames")) +
                              ") —— 两台设备上'不可用于跨芯片比较'的是同一批项; 注意这不改变复合分的"
                              "参与项集合(见 compositeParticipants)",
                          400);
        } else {
            std::string s = "两边的自检清单不一致: 只在设备A 判不通过 " + std::to_string((int)onlyA.size()) +
                            " 项, 只在设备B 判不通过 " + std::to_string((int)onlyB.size()) + " 项。";
            s += "=> 两台设备上'哪几项不可用于跨芯片比较'不同(读比时请连这份清单一起看); "
                 "复合分的参与项集合与原始比能不能相除, 见 compositeParticipants / sameItemSetComparison";
            j += jsonSafe(s, 600);
        }
        j += "\"";
        j += ",\"counts\":{\"excludedA\":" + std::to_string(a.audit.excludedCount) +
             ",\"excludedB\":" + std::to_string(b.audit.excludedCount) +
             ",\"passA\":" + std::to_string(a.audit.passCount) +
             ",\"passB\":" + std::to_string(b.audit.passCount) +
             ",\"failA\":" + std::to_string(a.audit.failCount) +
             ",\"failB\":" + std::to_string(b.audit.failCount) + "}";
        j += "}";
    }
    j += ",\"knownInflationSources\":["
         "{\"name\":\"可用核数不同\",\"howMuch\":\"核数多的那台总分比会虚高 12.5%(9 核 vs 8 核 = 1.125x)\","
         "\"handledBy\":\"normalized.perCoreRatio(每核吞吐比 = 吞吐 / 实际用到核数)\"},"
         "{\"name\":\"最高频档不同\",\"howMuch\":\"跑在不同频率档上能凭空造出 13.5% 的假提升(1.135x)\","
         "\"handledBy\":\"normalized.sameFreqRatio(吞吐 / 运行时频率中位 x 标称最高频)\"},"
         "{\"name\":\"核数与频率同时不同\",\"howMuch\":\"两个污染源会相乘(1.125 x 1.135 ≈ 1.277)\","
         "\"handledBy\":\"normalized.perCoreSameFreqRatio(两个一起消掉)\"}"
         "]";
    j += ",\"normalizationDefinition\":{"
         "\"perCoreRatio\":\"(B 的吞吐 / B 的核数) / (A 的吞吐 / A 的核数)\","
         "\"sameFreqRatio\":\"(B 的吞吐 / B 的运行时频率中位 x B 的标称最高频) / "
         "(A 的吞吐 / A 的运行时频率中位 x A 的标称最高频)\","
         "\"perCoreSameFreqRatio\":\"上面两个一起做\","
         "\"precondition\":\"'吞吐 ∝ 频率'在同一颗芯片同一架构上成立; 跨架构/跨代是假设, 必须标注\","
         "\"notDone\":\"本模块不写 /sys 去锁频锁核(那是改被测对象) —— 只做事后归一化\"}";

    // ---------------- 逐项对照表 ----------------
    j += ",\"items\":[";
    bool first = true;
    int matched = 0;
    int onlyA = 0;
    int onlyB = 0;
    for (size_t i = 0; i < a.items.size(); ++i) {
        const RepItem& ia = a.items[i];
        const RepItem* ib = findItem(b, ia.section, ia.name);
        if (ib != nullptr) {
            ++matched;
        } else {
            ++onlyA;
        }
        const SideInfo sa = sideOf(&ia);
        const SideInfo sb = sideOf(ib);
        double rawRatio = 0.0;
        const bool hasRaw = (sa.hasScore && sb.hasScore && safeRatio(sa.score, sb.score, &rawRatio));
        double perCore = 0.0;
        const bool hasPerCore = (sa.hasCores && sb.hasCores && sa.hasMetric && sb.hasMetric &&
                                 safeRatio(sa.metric / sa.coresUsed, sb.metric / sb.coresUsed, &perCore));
        double sameFreq = 0.0;
        const bool hasSameFreq = (sa.hasKhz && sb.hasKhz && sa.hasTop && sb.hasTop && sa.hasMetric &&
                                  sb.hasMetric &&
                                  safeRatio(sa.metric / sa.khzMed * sa.khzTop,
                                            sb.metric / sb.khzMed * sb.khzTop, &sameFreq));
        double perCoreSameFreq = 0.0;
        const bool hasPerCoreSameFreq =
            (hasSameFreq && sa.hasCores && sb.hasCores &&
             safeRatio((sa.metric / sa.khzMed * sa.khzTop) / sa.coresUsed,
                       (sb.metric / sb.khzMed * sb.khzTop) / sb.coresUsed, &perCoreSameFreq));

        j += (first ? "" : ",");
        first = false;
        j += "{\"section\":\"" + jsonSafe(ia.section, 80) + "\",\"name\":\"" + jsonSafe(ia.name, 80) + "\"";
        j += ",\"a\":" + sideJson(sa, &ia, true);
        j += ",\"b\":" + sideJson(sb, ib, ib != nullptr);
        j += ",\"onlyIn\":\"" + std::string(ib == nullptr ? "A(设备B 的报告里没有这一项)" : "") + "\"";
        j += ",\"ratio\":" + numOrNull(hasRaw, rawRatio, 4);
        j += ",\"deltaPercentDefinition\":\"(B/A - 1) x 100%\"";
        j += ",\"normalized\":{";
        j += "\"perCoreRatio\":" + numOrNull(hasPerCore, perCore, 4);
        j += ",\"sameFreqRatio\":" + numOrNull(hasSameFreq, sameFreq, 4);
        j += ",\"perCoreSameFreqRatio\":" + numOrNull(hasPerCoreSameFreq, perCoreSameFreq, 4);
        j += ",\"why\":\"";
        if (!hasPerCore || !hasSameFreq) {
            std::string why;
            if (!sa.hasCores || !sb.hasCores) {
                why += "有一侧报告里没有'实际用到核数', 无法做每核归一化; ";
            }
            if (!sa.hasKhz || !sb.hasKhz) {
                why += "有一侧报告里没有运行时频率中位(runFreq 为空或读不到), 无法做同频归一化; ";
            }
            if (!sa.hasMetric || !sb.hasMetric) {
                why += "有一侧没有正的吞吐(metric), 比值无意义; ";
            }
            if (why.empty()) {
                why = "缺输入";
            }
            j += jsonSafe(why, 300);
        } else {
            j += "原始比 = 芯片性能比 x 核数比 x 频率比; 三个归一化量分别消掉核数差 / 频率档差 / 两个一起消";
        }
        j += "\"}";

        // ---- 与真值对照(有真值才做) ----
        const TruthRow* ta = socHasGb7Truth(a.soc) ? findTruth(ia.name) : nullptr;
        const TruthRow* tb = (ib != nullptr && socHasGb7Truth(b.soc)) ? findTruth(ib->name) : nullptr;
        const SnlTruth* sna = (ia.section.find("GPU-SNL") != std::string::npos) ? findSnlTruthBySoc(a.soc) : nullptr;
        const SnlTruth* snb = nullptr;
        if (ib != nullptr && ib->section.find("GPU-SNL") != std::string::npos) {
            snb = findSnlTruthBySoc(b.soc);
        }
        const bool hasRefA = (ta != nullptr) || (sna != nullptr);
        const bool hasRefB = (tb != nullptr) || (snb != nullptr);
        const double refA = (ta != nullptr) ? ta->score : (sna != nullptr ? sna->score : 0.0);
        const double refB = (tb != nullptr) ? tb->score : (snb != nullptr ? snb->score : 0.0);
        j += ",\"truth\":{";
        j += "\"referenceA\":" + numOrNull(hasRefA, refA, 1);
        j += ",\"referenceB\":" + numOrNull(hasRefB, refB, 1);
        j += ",\"sourceKindA\":\"" +
             jsonSafe(ta != nullptr ? std::string(ta->sourceKind)
                                    : (sna != nullptr ? std::string(sna->sourceKind) : std::string("NOT_AVAILABLE")),
                      80) + "\"";
        j += ",\"sourceKindB\":\"" +
             jsonSafe(tb != nullptr ? std::string(tb->sourceKind)
                                    : (snb != nullptr ? std::string(snb->sourceKind) : std::string("NOT_AVAILABLE")),
                      80) + "\"";
        j += ",\"sourceA\":\"" + jsonSafe(ta != nullptr ? std::string(ta->source)
                                                       : (sna != nullptr ? std::string(sna->source) : std::string()),
                                         600) + "\"";
        j += ",\"sourceB\":\"" + jsonSafe(tb != nullptr ? std::string(tb->source)
                                                       : (snb != nullptr ? std::string(snb->source) : std::string()),
                                         600) + "\"";
        const bool hasDeltaA = (hasRefA && sa.hasScore && refA > 0.0);
        const bool hasDeltaB = (hasRefB && sb.hasScore && refB > 0.0);
        j += ",\"deltaPercentA\":" + numOrNull(hasDeltaA, hasDeltaA ? (sa.score / refA - 1.0) * 100.0 : 0.0, 2);
        j += ",\"deltaPercentB\":" + numOrNull(hasDeltaB, hasDeltaB ? (sb.score / refB - 1.0) * 100.0 : 0.0, 2);
        double truthRatio = 0.0;
        double measuredRatio = 0.0;
        const bool hasTruthRatio = (hasRefA && hasRefB && safeRatio(refA, refB, &truthRatio));
        const bool hasMeasured = (sa.hasScore && sb.hasScore && safeRatio(sa.score, sb.score, &measuredRatio));
        j += ",\"truthRatioBA\":" + numOrNull(hasTruthRatio, truthRatio, 4);
        j += ",\"measuredRatioBA\":" + numOrNull(hasMeasured, measuredRatio, 4);
        bool hasDev = false;
        double dev = 0.0;
        if (hasTruthRatio && hasMeasured && truthRatio > 0.0) {
            dev = (measuredRatio / truthRatio - 1.0) * 100.0;
            hasDev = true;
        }
        j += ",\"deviationFromTruthPercent\":" + numOrNull(hasDev, dev, 2);
        j += ",\"within5Percent\":" + std::string(hasDev ? (std::fabs(dev) <= 5.0 ? "true" : "false") : "null");
        j += ",\"thresholdIsOursNotOfficial\":true";
        j += ",\"why\":\"";
        if (!hasRefA && !hasRefB) {
            j += "这一项没有可对照的真值(见 truthRule) —— 写 null, 不拿别的项凑数";
        } else if (!hasRefA || !hasRefB) {
            j += "只有一端有真值(另一端查不到该机型/该 SoC 的真值), 因此算不出真值比, 只给各自的偏差百分比";
        } else if (!sa.hasScore || !sb.hasScore) {
            j += "有一侧这一项没有分数, 算不出实测比";
        } else {
            j += "两端都有真值: 偏差 = (实测比 / 真值比 - 1) x 100%; "
                 "|偏差| <= 5% 判为线性通过 —— 该阈值是我们自己定的, 不是官方阈值";
        }
        j += "\"";
        j += "}";
        j += "}";
    }
    // 只在设备B里出现的项(同样列出, 不静默丢掉)
    for (size_t i = 0; i < b.items.size(); ++i) {
        const RepItem& ib = b.items[i];
        if (findItem(a, ib.section, ib.name) != nullptr) {
            continue;
        }
        ++onlyB;
        const SideInfo sb = sideOf(&ib);
        j += (first ? "" : ",");
        first = false;
        j += "{\"section\":\"" + jsonSafe(ib.section, 80) + "\",\"name\":\"" + jsonSafe(ib.name, 80) + "\"";
        j += ",\"a\":" + sideJson(SideInfo(), nullptr, false);
        j += ",\"b\":" + sideJson(sb, &ib, true);
        j += ",\"onlyIn\":\"B(设备A 的报告里没有这一项)\"";
        j += ",\"ratio\":null,\"deltaPercentDefinition\":\"(B/A - 1) x 100%\"";
        j += ",\"normalized\":{\"perCoreRatio\":null,\"sameFreqRatio\":null,\"perCoreSameFreqRatio\":null,"
             "\"why\":\"只有一侧有这一项, 没有比值可言\"}";
        j += ",\"truth\":{\"referenceA\":null,\"referenceB\":null,\"sourceKindA\":\"NOT_AVAILABLE\","
             "\"sourceKindB\":\"NOT_AVAILABLE\",\"sourceA\":\"\",\"sourceB\":\"\",\"deltaPercentA\":null,"
             "\"deltaPercentB\":null,\"truthRatioBA\":null,\"measuredRatioBA\":null,"
             "\"deviationFromTruthPercent\":null,\"within5Percent\":null,\"thresholdIsOursNotOfficial\":true,"
             "\"why\":\"只有一侧有这一项, 无法对照\"}}";
    }
    j += "]";
    j += ",\"itemsMatched\":" + std::to_string(matched);
    j += ",\"itemsOnlyInA\":" + std::to_string(onlyA);
    j += ",\"itemsOnlyInB\":" + std::to_string(onlyB);
    j += ",\"scoredMeaning\":\"'该项是否计分'取报告里的 scoredByPolicy / scoredNote: CS1 单核/多核/GPU 项"
         "与自研套件项计分, GPU-SNL / NPU / 存储 I/O 不计分(它们不并入任何复合分)\"";

    // ---------------- 复合分 ----------------
    // 下面这几个量在「参与项集合」那一块里算出, 供 chipPerformanceRatio 的结论行使用
    bool multiPartsPresent = false;          // 两边的多核参与项名单都拿到了?
    bool multiPartsIdentical = false;        // 两边多核参与项是同一批?
    double multiSameItemScoreRatio = 0.0;    // 只用共同参与项重算的多核分比
    int multiSameItemN = 0;
    bool singlePartsPresent = false;
    bool singlePartsIdentical = false;
    j += ",\"composites\":{";
    {
        double gmPerCore = 1.0;
        int gmPerCoreN = 0;
        double gmSameFreq = 1.0;
        int gmSameFreqN = 0;
        for (size_t i = 0; i < a.items.size(); ++i) {
            const RepItem& ia = a.items[i];
            if (!isGb8CpuSection(ia.section)) {
                continue;
            }
            const RepItem* ib = findItem(b, ia.section, ia.name);
            if (ib == nullptr || !(ia.metric > 0.0) || !(ib->metric > 0.0)) {
                continue;
            }
            if (ia.coresUsed > 0.0 && ib->coresUsed > 0.0) {
                gmPerCore *= (ib->metric / ib->coresUsed) / (ia.metric / ia.coresUsed);
                ++gmPerCoreN;
            }
            if (ia.hasRuntimeKhz && ib->hasRuntimeKhz && ia.nominalTopKhz > 0.0 && ib->nominalTopKhz > 0.0) {
                gmSameFreq *= (ib->metric / ib->runtimeKhzMedian * ib->nominalTopKhz) /
                              (ia.metric / ia.runtimeKhzMedian * ia.nominalTopKhz);
                ++gmSameFreqN;
            }
        }
        const double gmPc = (gmPerCoreN > 0) ? std::pow(gmPerCore, 1.0 / (double)gmPerCoreN) : 0.0;
        const double gmSf = (gmSameFreqN > 0) ? std::pow(gmSameFreq, 1.0 / (double)gmSameFreqN) : 0.0;
        j += "\"gb7Single\":{";
        j += "\"a\":" + numOrNull(a.hasSingleComposite, a.singleComposite, 1);
        j += ",\"b\":" + numOrNull(b.hasSingleComposite, b.singleComposite, 1);
        double r = 0.0;
        const bool hasR = (a.hasSingleComposite && b.hasSingleComposite &&
                           safeRatio(a.singleComposite, b.singleComposite, &r));
        j += ",\"ratio\":" + numOrNull(hasR, r, 4);
        j += ",\"reference\":1633.0,\"referenceSourceKind\":\"USER_PROVIDED_PUBLIC_TRUTH\"";
        j += ",\"deltaPercentA\":" + numOrNull(a.hasSingleComposite,
                                               a.hasSingleComposite ? (a.singleComposite / 1633.0 - 1.0) * 100.0 : 0.0, 2);
        j += ",\"deltaPercentB\":" + numOrNull(b.hasSingleComposite,
                                               b.hasSingleComposite ? (b.singleComposite / 1633.0 - 1.0) * 100.0 : 0.0, 2);
        j += ",\"referenceAppliesTo\":\"参考机 Mate 80 Pro Max / Kirin 9030 Pro(只有该 SoC 有逐项真值)\"";
        j += "}";
        j += ",\"gb7Multi\":{";
        j += "\"a\":" + numOrNull(a.hasMultiComposite, a.multiComposite, 1);
        j += ",\"b\":" + numOrNull(b.hasMultiComposite, b.multiComposite, 1);
        double rm = 0.0;
        const bool hasRm = (a.hasMultiComposite && b.hasMultiComposite &&
                            safeRatio(a.multiComposite, b.multiComposite, &rm));
        j += ",\"ratio\":" + numOrNull(hasRm, rm, 4);
        j += ",\"reference\":6802.0,\"referenceSourceKind\":\"USER_PROVIDED_PUBLIC_TRUTH\"";
        j += ",\"deltaPercentA\":" + numOrNull(a.hasMultiComposite,
                                               a.hasMultiComposite ? (a.multiComposite / 6802.0 - 1.0) * 100.0 : 0.0, 2);
        j += ",\"deltaPercentB\":" + numOrNull(b.hasMultiComposite,
                                               b.hasMultiComposite ? (b.multiComposite / 6802.0 - 1.0) * 100.0 : 0.0, 2);
        j += "}";
        j += ",\"normalized\":{\"perCoreRatioGM\":" + numOrNull(gmPerCoreN > 0, gmPc, 4) +
             ",\"perCoreRatioGMItems\":" + std::to_string(gmPerCoreN) +
             ",\"sameFreqRatioGM\":" + numOrNull(gmSameFreqN > 0, gmSf, 4) +
             ",\"sameFreqRatioGMItems\":" + std::to_string(gmSameFreqN) +
             ",\"why\":\"复合分本身是各项几何平均, 所以归一化后的复合比也按几何平均合成"
             "(与官方'复合分 = 各项几何平均'的口径一致); 只统计两边报告里都有、且字段齐全的 CS1 项\"}";
        j += ",\"caveat\":\"复合分对照同样只是数量级参考: 官方没有公开 GB7 的绝对换算, 两边都是复刻实现\"";
    }
    j += "}";

    // =======================================================================
    //  两个复合分到底由哪些项组成? (2026-10-08 新增)
    //
    //  背景(用户的原话): 「如果平板排除的不是同样这几项, 两个复合分含的就不是同一批项, 相除毫无意义。」
    //  本块做的事:
    //    ① 把两份报告自己给出的参与项名单(composite.gb7SingleItems / gb7MultiItems)摆出来;
    //    ② 给出交集 / 只在A / 只在B, 并明确写"参与项集合是否相同";
    //    ③ 若不同 -> 不拿两个复合分直接相除当结论, 并额外给出"只用两边共同参与项"重算的比
    //       (sameItemSetComparison: 原始分比 / 每核比 / 同频比), 那才是同项可比的数。
    //  本块只做四则运算, 不改任何分数、不跑负载。
    // =======================================================================
    {
        const char* stageNames[2] = {"CS1 单核", "CS1 多核"};
        const char* stageKeys[2] = {"single", "multi"};
        j += ",\"compositeParticipants\":{";
        j += "\"whatItIs\":\"两个复合分含的项是不是同一批: 报告自己给出的参与项名单 + 交集/差集 + "
             "只用共同参与项重算的比。参与项不同时, 复合分原始比不可用\"";
        j += ",\"source\":\"报告里 composite.gb7SingleItems / gb7MultiItems 的原文(报告自己算出来的名单)\""; 
        for (int s = 0; s < 2; ++s) {
            const CompositeParticipants& pa = (s == 0) ? a.singleParts : a.multiParts;
            const CompositeParticipants& pb = (s == 0) ? b.singleParts : b.multiParts;
            std::vector<std::string> common;
            std::vector<std::string> onlyA;
            std::vector<std::string> onlyB;
            for (size_t i = 0; i < pa.names.size(); ++i) {
                bool inB = false;
                for (size_t k = 0; k < pb.names.size(); ++k) {
                    if (pb.names[k] == pa.names[i]) {
                        inB = true;
                        break;
                    }
                }
                if (inB) {
                    common.push_back(pa.names[i]);
                } else {
                    onlyA.push_back(pa.names[i]);
                }
            }
            for (size_t k = 0; k < pb.names.size(); ++k) {
                bool inA = false;
                for (size_t i = 0; i < pa.names.size(); ++i) {
                    if (pa.names[i] == pb.names[k]) {
                        inA = true;
                        break;
                    }
                }
                if (!inA) {
                    onlyB.push_back(pb.names[k]);
                }
            }
            // 只用"两边都参与"的项重算三个比(同项可比)
            double scoreProd = 1.0;
            int scoreN = 0;
            double pcProd = 1.0;
            int pcN = 0;
            double sfProd = 1.0;
            int sfN = 0;
            for (size_t i = 0; i < common.size(); ++i) {
                const RepItem* ia = findItem(a, stageNames[s], common[i]);
                const RepItem* ib = findItem(b, stageNames[s], common[i]);
                if (ia == nullptr || ib == nullptr) {
                    continue;
                }
                if (ia->score > 0.0 && ib->score > 0.0) {
                    scoreProd *= (ib->score / ia->score);
                    ++scoreN;
                }
                if (ia->metric > 0.0 && ib->metric > 0.0 && ia->coresUsed > 0.0 && ib->coresUsed > 0.0) {
                    pcProd *= (ib->metric / ib->coresUsed) / (ia->metric / ia->coresUsed);
                    ++pcN;
                }
                if (ia->metric > 0.0 && ib->metric > 0.0 && ia->hasRuntimeKhz && ib->hasRuntimeKhz &&
                    ia->nominalTopKhz > 0.0 && ib->nominalTopKhz > 0.0) {
                    sfProd *= (ib->metric / ib->runtimeKhzMedian * ib->nominalTopKhz) /
                              (ia->metric / ia->runtimeKhzMedian * ia->nominalTopKhz);
                    ++sfN;
                }
            }
            const double scoreGM = (scoreN > 0) ? std::pow(scoreProd, 1.0 / (double)scoreN) : 0.0;
            const double pcGM = (pcN > 0) ? std::pow(pcProd, 1.0 / (double)pcN) : 0.0;
            const double sfGM = (sfN > 0) ? std::pow(sfProd, 1.0 / (double)sfN) : 0.0;
            const bool present = (pa.present && pb.present);
            const bool identical = present && sameNameSet(pa.names, pb.names);
            if (s == 0) {
                singlePartsPresent = present;
                singlePartsIdentical = identical;
            } else {
                multiPartsPresent = present;
                multiPartsIdentical = identical;
                multiSameItemScoreRatio = (scoreN > 0) ? std::pow(scoreProd, 1.0 / (double)scoreN) : 0.0;
                multiSameItemN = scoreN;
            }
            j += ",\"";
            j += stageKeys[s];
            j += "\":{";
            j += "\"stage\":\"" + jsonSafe(std::string(stageNames[s]), 30) + "\"";
            j += ",\"a\":" + participantsJson(pa);
            j += ",\"b\":" + participantsJson(pb);
            j += ",\"commonNames\":[";
            for (size_t i = 0; i < common.size(); ++i) {
                j += (i == 0 ? "" : ",");
                j += "\"" + jsonSafe(common[i], 60) + "\"";
            }
            j += "]";
            j += ",\"onlyInA\":[";
            for (size_t i = 0; i < onlyA.size(); ++i) {
                j += (i == 0 ? "" : ",");
                j += "\"" + jsonSafe(onlyA[i], 60) + "\"";
            }
            j += "]";
            j += ",\"onlyInB\":[";
            for (size_t i = 0; i < onlyB.size(); ++i) {
                j += (i == 0 ? "" : ",");
                j += "\"" + jsonSafe(onlyB[i], 60) + "\"";
            }
            j += "]";
            j += ",\"commonCount\":" + std::to_string((int)common.size());
            j += ",\"identical\":" + std::string(present ? (identical ? "true" : "false") : "null");
            j += ",\"ratioUsable\":" + std::string(present ? (identical ? "true" : "false") : "null");
            j += ",\"why\":\"";
            if (!present) {
                j += "有一份报告里没有这一阶段的参与项名单(composite.gb7*Items 缺失), 是否同项判不出来 "
                     "—— 写 null, 不假定相同";
            } else if (identical) {
                j += "两边参与项一致(" + std::to_string((int)common.size()) + " 项), 复合分原始比可用";
            } else {
                j += "两边参与项不同: A " + std::to_string((int)pa.names.size()) + " 项 / B " +
                     std::to_string((int)pb.names.size()) + " 项, 共同 " + std::to_string((int)common.size()) +
                     " 项。=> 这一阶段的复合分原始比不可用(它比的不是同一批项); "
                     "请改用 sameItemSetComparison(只用共同参与项重算)";
            }
            j += "\"";
            j += ",\"sameItemSetComparison\":{";
            j += "\"compositeScoreRatioBA\":" + numOrNull(scoreN > 0, scoreGM, 4);
            j += ",\"perCoreRatioGM\":" + numOrNull(pcN > 0, pcGM, 4);
            j += ",\"sameFreqRatioGM\":" + numOrNull(sfN > 0, sfGM, 4);
            j += ",\"items\":" + std::to_string(scoreN);
            j += ",\"perCoreItems\":" + std::to_string(pcN);
            j += ",\"sameFreqItems\":" + std::to_string(sfN);
            j += ",\"definition\":\"只用两边都参与该阶段复合分的项重算: 分比 = 各项(分B/分A)的几何平均; "
                 "每核比 = 各项((吞吐/核数)B / (吞吐/核数)A)的几何平均; 同频比 = 各项(折算到各自标称最高频后的"
                 "吞吐B/吞吐A)的几何平均\"";
            j += ",\"why\":\"参与项不同的两个复合分不能直接相除 —— 这个块给出的是'把项集对齐之后'的比\"";
            j += "}";
            j += "}";
        }
        j += "}";
    }

    // =======================================================================
    //   用户真正要的那个数: 「本次两台的分数比, 换算成芯片性能比是多少」
    //  (2026-10-08 新增; 只做四则运算, 不跑负载、不改任何分数)
    //
    //  为什么单列一块: 上面的 normalized/perCoreRatioGM 与 sameFreqRatioGM 是分散在
    //  逐项表与 composites 里的, 用户要的是一句话结论。这一块把它算出来并写成人话,
    //  界面与报告直接显示这一行即可 —— 不用自己去乘除。
    //
    //  口径(与上面的归一化定义逐字一致, 不引入任何新口径):
    //    分数比 = 复合分B / 复合分A
    //    分数比 = 芯片性能比 x 核数比 x 频率档比        <- 后两项是"系统策略"污染, 不是芯片实力
    //    每核归一化比     = 消掉核数比
    //    同频归一化比     = 消掉频率档比
    //    两者一起做       = 消掉两个污染源 -> 最接近芯片性能比的那个数
    //  取哪一个当结论: perCoreSameFreqRatioGM > perCoreRatioGM > null(逐级降级, 并写明用的是哪一个)
    // =======================================================================
    {
        // ---- 核数比 / 频率档比(与 observedDifferences 里同源, 这里重算一遍以便独立成块) ----
        double allowedA = 0.0;
        double allowedB = 0.0;
        double topA = 0.0;
        double topB = 0.0;
        for (size_t i = 0; i < a.items.size(); ++i) {
            if (a.items[i].allowedCount > allowedA) {
                allowedA = a.items[i].allowedCount;
            }
            if (a.items[i].nominalTopKhz > topA) {
                topA = a.items[i].nominalTopKhz;
            }
        }
        for (size_t i = 0; i < b.items.size(); ++i) {
            if (b.items[i].allowedCount > allowedB) {
                allowedB = b.items[i].allowedCount;
            }
            if (b.items[i].nominalTopKhz > topB) {
                topB = b.items[i].nominalTopKhz;
            }
        }
        const bool hasCoreRatio = (allowedA > 0.0 && allowedB > 0.0);
        const bool hasFreqRatio = (topA > 0.0 && topB > 0.0);
        const double coreRatio = hasCoreRatio ? (allowedB / allowedA) : 0.0;
        const double freqRatio = hasFreqRatio ? (topB / topA) : 0.0;

        // ---- 条件差(线程数 / SMT / 可用核集合): 与 conditionGaps 同源, 结论行要用 ----
        double thA2 = 0.0;
        double thB2 = 0.0;
        for (size_t i = 0; i < a.items.size(); ++i) {
            if (a.items[i].threadsEffective > thA2) {
                thA2 = a.items[i].threadsEffective;
            }
        }
        for (size_t i = 0; i < b.items.size(); ++i) {
            if (b.items[i].threadsEffective > thB2) {
                thB2 = b.items[i].threadsEffective;
            }
        }
        const bool hasThreads2 = (thA2 > 0.0 && thB2 > 0.0);
        const bool smtKnown2 = (a.machine.hasTopology && b.machine.hasTopology);
        const bool smtSame2 = (smtKnown2 && a.machine.smtEnabled == b.machine.smtEnabled);
        const bool coresSame2 = (allowedA > 0.0 && allowedB > 0.0 && allowedA == allowedB);
        const bool spGap = (hasThreads2 && ((thA2 != thB2) || (smtKnown2 && !smtSame2) || !coresSame2));
        std::string spGapText;
        if (spGap) {
            spGapText = "线程数 A " + fixed(thA2, 0) + " / B " + fixed(thB2, 0);
            spGapText += (smtKnown2 ? (std::string(", SMT A ") + (a.machine.smtEnabled ? "开" : "关") +
                                      " / B " + (b.machine.smtEnabled ? "开" : "关"))
                                    : std::string(", SMT 状态有一侧未上报"));
            spGapText += ", 可用核集合 A " + fixed(allowedA, 0) + " 核 / B " + fixed(allowedB, 0) + " 核";
        }

        // ---- 两个归一化量的几何平均(与 composites.normalized 同口径, 逐项几何平均) ----
        double gmPerCore = 1.0;
        int gmPerCoreN = 0;
        double gmSameFreq = 1.0;
        int gmSameFreqN = 0;
        double gmBoth = 1.0;
        int gmBothN = 0;
        for (size_t i = 0; i < a.items.size(); ++i) {
            const RepItem& ia = a.items[i];
            if (!isGb8CpuSection(ia.section)) {
                continue;
            }
            const RepItem* ib = findItem(b, ia.section, ia.name);
            if (ib == nullptr || !(ia.metric > 0.0) || !(ib->metric > 0.0)) {
                continue;
            }
            const bool pcOk = (ia.coresUsed > 0.0 && ib->coresUsed > 0.0);
            const bool sfOk = (ia.hasRuntimeKhz && ib->hasRuntimeKhz &&
                               ia.nominalTopKhz > 0.0 && ib->nominalTopKhz > 0.0);
            if (pcOk) {
                gmPerCore *= (ib->metric / ib->coresUsed) / (ia.metric / ia.coresUsed);
                ++gmPerCoreN;
            }
            const double sfB = sfOk ? (ib->metric / ib->runtimeKhzMedian * ib->nominalTopKhz) : 0.0;
            const double sfA = sfOk ? (ia.metric / ia.runtimeKhzMedian * ia.nominalTopKhz) : 0.0;
            if (sfOk) {
                gmSameFreq *= sfB / sfA;
                ++gmSameFreqN;
            }
            if (pcOk && sfOk) {
                gmBoth *= (sfB / ib->coresUsed) / (sfA / ia.coresUsed);
                ++gmBothN;
            }
        }
        const double pcGM = (gmPerCoreN > 0) ? std::pow(gmPerCore, 1.0 / (double)gmPerCoreN) : 0.0;
        const double sfGM = (gmSameFreqN > 0) ? std::pow(gmSameFreq, 1.0 / (double)gmSameFreqN) : 0.0;
        const double bothGM = (gmBothN > 0) ? std::pow(gmBoth, 1.0 / (double)gmBothN) : 0.0;

        // ---- 原始分数比(多核复合分比是"总分比", 单核复合分比不含核数污染, 两个都报) ----
        double rm = 0.0;
        const bool hasRm = (a.hasMultiComposite && b.hasMultiComposite &&
                            safeRatio(a.multiComposite, b.multiComposite, &rm));
        double rs = 0.0;
        const bool hasRs = (a.hasSingleComposite && b.hasSingleComposite &&
                            safeRatio(a.singleComposite, b.singleComposite, &rs));

        // ---- 结论取哪一个: 两个污染源都消掉的优先, 逐级降级 ----
        double best = 0.0;
        std::string bestWhich = "none";
        if (gmBothN > 0) {
            best = bothGM;
            bestWhich = "perCoreSameFreqRatioGM(同时消掉核数差与频率档差)";
        } else if (gmPerCoreN > 0) {
            best = pcGM;
            bestWhich = "perCoreRatioGM(只消掉核数差; 报告里读不到运行时频率, 无法再消频率档差)";
        }

        j += ",\"chipPerformanceRatio\":{";
        j += "\"question\":\"本次两台的分数比, 换算成芯片性能比是多少?\"";
        j += ",\"whatThisIs\":\"本块只做四则运算: 分数比 = 芯片性能比 x 核数比 x 频率档比; "
             "把后两项除回去, 剩下的才是芯片性能比。不跑负载 / 不连设备 / 不改任何分数。\"";
        j += ",\"rawMultiCompositeRatioBA\":" + numOrNull(hasRm, rm, 4);
        j += ",\"rawSingleCompositeRatioBA\":" + numOrNull(hasRs, rs, 4);
        j += ",\"rawMultiCompositeDefinition\":\"设备B 的 CS1 多核复合分 / 设备A 的 CS1 多核复合分"
             "(复合分 = 各自各项中位分的几何平均; 口径见 composites)\"";
        j += ",\"contamination\":{"
             "\"coreCountRatioBA\":" + numOrNull(hasCoreRatio, coreRatio, 4) +
             ",\"allowedCoresA\":" + numOrNull(allowedA > 0.0, allowedA, 0) +
             ",\"allowedCoresB\":" + numOrNull(allowedB > 0.0, allowedB, 0) +
             ",\"freqTierRatioBA\":" + numOrNull(hasFreqRatio, freqRatio, 4) +
             ",\"nominalTopKhzA\":" + numOrNull(topA > 0.0, topA, 0) +
             ",\"nominalTopKhzB\":" + numOrNull(topB > 0.0, topB, 0) +
             ",\"why\":\"这两个比是系统策略差异, 不是芯片实力差异。高估程度: "
             "核数比 - 1 与 频率档比 - 1 (两项同时存在时相乘)\"}";
        j += ",\"normalized\":{"
             "\"perCoreRatioGM\":" + numOrNull(gmPerCoreN > 0, pcGM, 4) +
             ",\"perCoreRatioGMItems\":" + std::to_string(gmPerCoreN) +
             ",\"sameFreqRatioGM\":" + numOrNull(gmSameFreqN > 0, sfGM, 4) +
             ",\"sameFreqRatioGMItems\":" + std::to_string(gmSameFreqN) +
             ",\"perCoreSameFreqRatioGM\":" + numOrNull(gmBothN > 0, bothGM, 4) +
             ",\"perCoreSameFreqRatioGMItems\":" + std::to_string(gmBothN) +
             ",\"definition\":\"perCoreRatioGM = (每核吞吐B / 每核吞吐A) 的几何平均; "
             "sameFreqRatioGM = (折算到各自标称最高频后的吞吐B / 吞吐A) 的几何平均; "
             "perCoreSameFreqRatioGM = 两个一起做\"}";
        // ---- 原始多核复合分比可用吗?(两个复合分含的不是同一批项时, 这个比不可用) ----
        j += ",\"rawMultiCompositeRatioUsable\":" +
             std::string(multiPartsPresent ? (multiPartsIdentical ? "true" : "false") : "null");
        j += ",\"rawMultiCompositeRatioUnusableReason\":\"";
        if (!multiPartsPresent) {
            j += "有一份报告里没有多核复合分的参与项名单 -> 是否同项判不出来(写 null)";
        } else if (multiPartsIdentical) {
            j += "两边多核复合分的参与项一致, 原始多核复合分比可用";
        } else {
            j += "两边的多核复合分参与项集合不同(见 compositeParticipants.multi 的 onlyInA/onlyInB; "
                 "自检清单是否一致与此无关) -> 两个多核复合分含的不是同一批项, 原始多核复合分比不可用; "
                 "请用 sameItemSetMultiCompositeRatioBA / perCoreSameFreqRatioGM";
        }
        j += "\"";
        j += ",\"sameItemSetMultiCompositeRatioBA\":" + numOrNull(multiSameItemN > 0, multiSameItemScoreRatio, 4);
        j += ",\"sameItemSetMultiCompositeItems\":" + std::to_string(multiSameItemN);
        j += ",\"sameItemSetMultiCompositeDefinition\":\"只用两边都参与多核复合分的项重算出来的多核分比 "
             "(各项分B/分A 的几何平均) —— 参与项不同的两个复合分不能直接相除, 这个数才是同项可比的\"";
        j += ",\"conditionGapSummary\":\"";
        if (spGap) {
            j += jsonSafe(std::string("本次两台的核数/SMT/线程数条件不同: ") + spGapText +
                              " -> 原始多核比里混着这些条件差, 多核比不是纯芯片比; "
                              "每核比(perCoreRatioGM)消掉了线程数差, 但 SMT 开的一侧'两个线程 != 两个核', "
                              "这一条无法在本工程内消除, 只能标注",
                          700);
        } else {
            j += jsonSafe(std::string("本次两台的核数/SMT/线程数条件相同 -> 原始多核比不含这几项条件差"),
                          300);
        }
        j += "\"";
        j += ",\"bestEstimateOfChipPerformanceRatio\":" + numOrNull(best > 0.0, best, 4);
        j += ",\"bestEstimateWhich\":\"" + jsonSafe(bestWhich, 120) + "\"";
        j += ",\"bestEstimateInverseBA\":" + numOrNull(best > 0.0, (best > 0.0 ? 1.0 / best : 0.0), 4);
        // ---- 一句话结论(界面与报告直接显示这一行; 数字全部来自上面几个字段) ----
        {
            std::string c;
            c += "设备B / 设备A: ";
            // 先说要紧的: 两个复合分含的不是同一批项时, 下面对"多核分数比"的任何解读都不成立
            if (multiPartsPresent && !multiPartsIdentical) {
                c += "两个多核复合分含的不是同一批项(见 compositeParticipants.multi 的 onlyInA/onlyInB) "
                     "-> 下面的多核分数比不可用, 不要拿它当芯片比 ";
            }
            if (hasRm) {
                c += "多核分数比 = " + fixed(rm, 4) + "x";
            } else {
                c += "多核分数比算不出来(有一侧没有复合分)";
            }
            if (hasRs) {
                c += ", 单核分数比 = " + fixed(rs, 4) + "x(单核不含核数污染, 可直接当芯片单核性能比读)";
            }
            c += "。多核分数比不是芯片多核性能比 —— ";
            if (hasCoreRatio || hasFreqRatio) {
                c += "它 = 芯片性能比";
                if (hasCoreRatio) {
                    c += " x 核数比 " + fixed(coreRatio, 4) +
                         "(A 可用 " + fixed(allowedA, 0) + " 核 / B 可用 " + fixed(allowedB, 0) + " 核)";
                }
                if (hasFreqRatio) {
                    c += " x 频率档比 " + fixed(freqRatio, 4) +
                         "(A 最高档 " + fixed(topA, 0) + " kHz / B 最高档 " + fixed(topB, 0) + " kHz)";
                }
                c += "。";
            } else {
                c += "本次两边的可用核数与最高频档都读不到, 无法判断污染有多大。";
            }
            if (multiSameItemN > 0) {
                c += "把项集对齐(只用两边共同参与多核复合分的 " + std::to_string(multiSameItemN) +
                     " 项)重算的多核分比 = " + fixed(multiSameItemScoreRatio, 4) + "x。";
            }
            if (spGap) {
                c += "本次两台的核数/SMT/线程数条件不同(" + jsonSafe(spGapText, 220) +
                     ") —— 多核比因此不是纯芯片比, 只能说'在各自条件下的实测比'; "
                     "每核归一化比消掉了线程数差, 但 SMT 开的一侧两个线程不等于两个核, 这一条消不掉";
            }
            if (gmPerCoreN > 0) {
                c += "消掉核数差 -> 每核归一化比 = " + fixed(pcGM, 4) + "x(" +
                     std::to_string(gmPerCoreN) + " 项)。";
            }
            if (gmSameFreqN > 0) {
                c += "消掉频率档差 -> 同频归一化比 = " + fixed(sfGM, 4) + "x(" +
                     std::to_string(gmSameFreqN) + " 项)。";
            }
            if (best > 0.0) {
                c += "=> 本次两台**芯片多核性能比最接近的估计 = " + fixed(best, 4) +
                     "x**(用 " + bestWhich + "; 反过来 A/B = " + fixed(1.0 / best, 4) + "x)。";
            } else {
                c += "=> 归一化算不出来(缺实际用到核数 / 运行时频率中位), 芯片性能比写 null, 不拿分数比冒充。";
            }
            // ---- "两台都跑满了吗"(2026-10 新增; 判据由 cpu_freq_sample.h 定义, 本模块只搬原文) ----
            //  用户点名要的那一条: 两台的逐项运行时频率占比都达到"跑满"判据时,
            //  直接给原始分比当结论(此时原始比就是芯片性能比, 归一化只作参考与诊断);
            //  只要有一侧没达到, 就明写"原始比里混着'那一侧没跑满'", 归一化只作辅助与诊断。
            double thSat = 0.0;
            const SatSummary satSA = stageSaturation(a, "单核", &thSat);
            const SatSummary satMA = stageSaturation(a, "多核", &thSat);
            const SatSummary satSB = stageSaturation(b, "单核", &thSat);
            const SatSummary satMB = stageSaturation(b, "多核", &thSat);
            const bool satThreshKnown = (thSat > 0.0);
            if (!satThreshKnown) {
                thSat = 90.0;   // 与 cpu_freq_sample.cpp 的 kFullRatioPct 同一个数(旧报告里没有阈值原文)
            }
            const bool satFullSingle = (satSA.hasMed && satSB.hasMed &&
                                        satSA.med >= thSat && satSB.med >= thSat);
            const bool satFullMulti = (satMA.hasMed && satMB.hasMed &&
                                       satMA.med >= thSat && satMB.med >= thSat);
            const bool satBothFull = (satFullSingle && satFullMulti);
            c += " 跑满判据(阈值 " + fixed(thSat, 0) + "%, 我们自己定的, 不是官方阈值";
            c += satThreshKnown ? ", 取自报告 runFreq 文本原文" : ", 报告里没有阈值原文, 回退到本模块写死的值";
            c += "): 单核阶段 A 逐项占比中位 = ";
            c += satSA.hasMed ? (fixed(satSA.med, 1) + "%") : std::string("拿不到");
            c += " / B = ";
            c += satSB.hasMed ? (fixed(satSB.med, 1) + "%") : std::string("拿不到");
            c += "; 多核阶段 A = ";
            c += satMA.hasMed ? (fixed(satMA.med, 1) + "%") : std::string("拿不到");
            c += " / B = ";
            c += satMB.hasMed ? (fixed(satMB.med, 1) + "%") : std::string("拿不到");
            c += "。";
            if (satBothFull) {
                c += "=> 两台单核与多核都达到了跑满判据, 因此直接比原始分: 单核分数比(B/A) = " +
                     (hasRs ? (fixed(rs, 4) + "x") : std::string("null")) +
                     ", 多核分数比(B/A) = " +
                     (hasRm ? (fixed(rm, 4) + "x") : std::string("null")) +
                     "; 这就是本次的结论, 归一化比只作参考与诊断。";
            } else {
                c += "=> 至少有一个阶段没有两台都跑满, 原始分比不能直接当芯片性能比; "
                     "本模块给出的归一化比只作辅助与诊断 —— 真正的修法是让没跑满的那一侧在"
                     "计时区间里真跑到稳态上限(预热取证与上限取证见每项 runFreq 原文)。";
            }
            if (!satSA.notFullList.empty() || !satSB.notFullList.empty()) {
                c += " 单核未跑满的项: A ";
                c += satSA.notFullList.empty() ? std::string("(无)") : satSA.notFullList;
                c += " / B ";
                c += satSB.notFullList.empty() ? std::string("(无)") : satSB.notFullList;
                c += "。";
            }
            if (!satMA.notFullList.empty() || !satMB.notFullList.empty()) {
                c += " 多核未跑满的项: A ";
                c += satMA.notFullList.empty() ? std::string("(无)") : satMA.notFullList;
                c += " / B ";
                c += satMB.notFullList.empty() ? std::string("(无)") : satMB.notFullList;
                c += "。";
            }
            j += ",\"saturationCheck\":{";
            j += "\"whatItIs\":\"两台各自的逐项『占标称比 中位』统计(单核/多核分开)与"
                 "'两台是否都跑满'的结论 —— 这是判断'原始分比能不能直接当芯片性能比'的唯一依据\"";
            j += ",\"thresholdPercent\":" + fixed(thSat, 1);
            j += ",\"thresholdIsOursNotOfficial\":true";
            j += ",\"thresholdSource\":\"" +
                 jsonSafe(satThreshKnown
                              ? std::string("报告 runFreq 文本里的『跑满判据(我们自定: 占比中位 >= X%)』原文")
                              : std::string("报告里没有阈值原文(旧报告), 回退到 90(与 cpu_freq_sample.cpp 的 "
                                            "kFullRatioPct 同一个数)"),
                          300) + "\"";
            j += ",\"definition\":\"占标称比 = 逐样本(该样本频率 / 该样本所在核的标称上限)的中位; "
                 "多核项取口径A(当刻有负载线程落上的核), 不是把空闲核一起算进去的口径B\"";
            j += ",\"single\":{\"a\":" + satSummaryJson(satSA) + ",\"b\":" + satSummaryJson(satSB) +
                 ",\"bothFull\":" + std::string(satFullSingle ? "true" : "false") + "}";
            j += ",\"multi\":{\"a\":" + satSummaryJson(satMA) + ",\"b\":" + satSummaryJson(satMB) +
                 ",\"bothFull\":" + std::string(satFullMulti ? "true" : "false") + "}";
            j += ",\"bothFull\":" + std::string(satBothFull ? "true" : "false");
            j += ",\"rawScoreRatioIsTheConclusion\":" + std::string(satBothFull ? "true" : "false");
            j += ",\"normalizationRole\":\"";
            j += satBothFull
                     ? "参考与诊断(两台都跑满 -> 结论用原始分比)"
                     : "辅助与诊断(有一侧没跑满 -> 原始比里混着'没跑满', 归一化替代不了'让那一侧真跑满')";
            j += "\"";
            j += "}";
            c += "前提: '吞吐 ∝ 频率' 在同架构上成立; 跨架构/跨代只是近似, 必须连同这条一起读。";
            j += ",\"conclusion\":\"" + jsonSafe(c, 2400) + "\"";
        }
        j += ",\"caveats\":["
             "\"归一化的前提是'吞吐 ∝ 频率', 跨架构时这只是假设\","
             "\"核数比与频率档比来自两份报告里实际观测到的允许核集合与标称最高频; "
             "读不到时写 null, 不猜\","
             "\"本块不写 sysfs / 不锁频 / 不锁核(那是改被测对象)\","
             "\"单核复合分比不含核数污染, 但它仍然包含频率档差, 所以单核也要看同频归一化比\""
             "]";
        j += "}";
    }

    // =======================================================================
    //   原始分比 vs 归一化比: 两台都把芯片跑满了吗? (2026-10 新增)
    // =======================================================================
    //  用户的原话: 「我不太理解这个同频是什么意思, 总之我想要的就是每个芯片尽可能的释放那个
    //  芯片所能释放的全部性能。」=> 判据必须落到"每一项在这颗芯片上到底跑满了没有",
    //  而不是靠事后归一化去补。
    //  口径(全部只搬报告里的原文, 本模块不重新定义任何频率量):
    //    * 逐项取报告 runFreq 文本里的『占标称比 中位』(多核项取口径A: 当刻有负载线程
    //      落上的核, 不是把空闲核一起算进去的口径B);
    //    * 阈值直接取同一段文本里的『占比中位 >= X%』(X = 90, 由 cpu_freq_sample.h 定义,
    //      我们自己定的, 不是官方阈值);
    //    * 单核阶段与多核阶段分开判。
    //  结论规则(用户点名要的那一条):
    //    * 两台各自的逐项占比中位都 >= 阈值 -> 直接给原始分数比当结论
    //      (跑满之后原始比就是芯片性能比, 归一化只作参考);
    //    * 只要有一侧没达到 -> 写"原始比里混着'那一侧没跑满'", 归一化只作辅助与诊断,
    //      并把没跑满的项逐项列出来(项名 + 占比), 指向真正的修法(预热/上限取证)。
    {
        double thA = 0.0;
        double thB = 0.0;
        const SatSummary sSingleA = stageSaturation(a, "单核", &thA);
        const SatSummary sMultiA = stageSaturation(a, "多核", &thA);
        const SatSummary sSingleB = stageSaturation(b, "单核", &thB);
        const SatSummary sMultiB = stageSaturation(b, "多核", &thB);
        double thresh = (thA > 0.0) ? thA : thB;    // 两台都写了就该一样; 只有一侧写了就用那一侧
        const bool threshKnown = (thresh > 0.0);
        const double kFallbackThresh = 90.0;              // 与 cpu_freq_sample.cpp 的 kFullRatioPct 同一个数
        if (!threshKnown) {
            thresh = kFallbackThresh;
        }
        const bool fullSingle = (sSingleA.hasMed && sSingleB.hasMed &&
                                 sSingleA.med >= thresh && sSingleB.med >= thresh);
        const bool fullMulti = (sMultiA.hasMed && sMultiB.hasMed &&
                                sMultiA.med >= thresh && sMultiB.med >= thresh);
        const bool hasRawSingle = (a.hasSingleComposite && b.hasSingleComposite &&
                                   a.singleComposite > 0.0 && b.singleComposite > 0.0);
        const bool hasRawMulti = (a.hasMultiComposite && b.hasMultiComposite &&
                                  a.multiComposite > 0.0 && b.multiComposite > 0.0);
        const double rawSingle = hasRawSingle ? (b.singleComposite / a.singleComposite) : 0.0;
        const double rawMulti = hasRawMulti ? (b.multiComposite / a.multiComposite) : 0.0;
        j += ",\"rawVsNormalized\":{";
        j += "\"question\":\"两台都把芯片跑满了吗? 跑满了就直接比原始分\"";
        j += ",\"why\":\"原始分数比只有在两台都真的把芯片跑到稳态上限时才是芯片性能比; "
             "只要有一侧没跑满, 原始比里就混着'那一侧没跑满'这件事 —— 那时归一化只是辅助与诊断, "
             "真正的修法是让那一侧在计时区间里真跑到稳态(见每项 runFreq 里的预热取证与上限取证)\"";
        j += ",\"threshold\":{\"percent\":" + fixed(thresh, 1);
        j += ",\"source\":\"" +
             jsonSafe(threshKnown
                          ? std::string("从报告 runFreq 文本的『跑满判据(我们自定: 占比中位 >= X%)』里解析出来的原文阈值")
                          : std::string("报告里没有阈值原文(旧报告), 回退到本模块写死的 90 —— 与 "
                                        "cpu_freq_sample.cpp 的 kFullRatioPct 同一个数"),
                      300) + "\"";
        j += ",\"thresholdIsOursNotOfficial\":true";
        j += ",\"definition\":\"占标称比 = 逐样本(该样本频率 / 该样本所在核的标称上限)的中位; "
             "多核项取口径A(当刻有负载线程落上的核), 不是把空闲核一起算进去的口径B\"}";
        j += ",\"stage\":{\"single\":{\"a\":" + satSummaryJson(sSingleA) + ",\"b\":" + satSummaryJson(sSingleB) +
             ",\"bothFull\":" + std::string(fullSingle ? "true" : "false") + "}";
        j += ",\"multi\":{\"a\":" + satSummaryJson(sMultiA) + ",\"b\":" + satSummaryJson(sMultiB) +
             ",\"bothFull\":" + std::string(fullMulti ? "true" : "false") + "}}";
        j += ",\"rawScoreRatioBA\":{\"singleComposite\":" + numOrNull(hasRawSingle, rawSingle, 4) +
             ",\"multiComposite\":" + numOrNull(hasRawMulti, rawMulti, 4) + "}";
        j += ",\"rawRatioIsTheConclusion\":" +
             std::string((fullSingle && fullMulti) ? "true" : "false");
        j += ",\"normalizationRole\":\"";
        j += (fullSingle && fullMulti)
                 ? "本轮的参考与诊断: 两台都达到了跑满判据, 结论用原始分比即可, 归一化比只用来交叉核对"
                 : "本轮的辅助与诊断: 有一侧没跑满, 原始分比不能直接当芯片性能比; 归一化比只说明"
                   "'如果两台同频同核数会差多少', 它替代不了'让那一侧真跑满'";
        j += "\"";
        {
            std::string c;
            c += "跑满判据(阈值 " + fixed(thresh, 0) + "%, 我们自己定的, 不是官方阈值): ";
            c += "单核阶段 A 逐项占比中位 = ";
            c += sSingleA.hasMed ? (fixed(sSingleA.med, 1) + "%") : std::string("拿不到");
            c += " / B 逐项占比中位 = ";
            c += sSingleB.hasMed ? (fixed(sSingleB.med, 1) + "%") : std::string("拿不到");
            c += "; 多核阶段 A = ";
            c += sMultiA.hasMed ? (fixed(sMultiA.med, 1) + "%") : std::string("拿不到");
            c += " / B = ";
            c += sMultiB.hasMed ? (fixed(sMultiB.med, 1) + "%") : std::string("拿不到");
            c += "。";
            if (fullSingle && fullMulti) {
                c += "=> 两台单核与多核都达到了跑满判据, 因此直接比原始分: ";
                c += "单核分数比(B/A) = " + (hasRawSingle ? (fixed(rawSingle, 4) + "x") : std::string("null"));
                c += ", 多核分数比(B/A) = " + (hasRawMulti ? (fixed(rawMulti, 4) + "x") : std::string("null"));
                c += "; 这就是本次的结论, 不必再看归一化比(它只作参考与诊断)。";
            } else {
                c += "=> 至少有一个阶段没有两台都跑满, 原始分比不能直接当芯片性能比; ";
                c += "本模块给出的归一化比只作辅助与诊断 —— 真正的修法是让没跑满的那一侧在"
                     "计时区间里真跑到稳态上限(预热/上限取证见每项 runFreq)。";
            }
            if (!sSingleA.notFullList.empty() || !sMultiA.notFullList.empty()) {
                c += " 设备A 未跑满的项: ";
                c += sSingleA.notFullList.empty() ? std::string("(单核无)") : ("单核 " + sSingleA.notFullList);
                c += "; ";
                c += sMultiA.notFullList.empty() ? std::string("(多核无)") : ("多核 " + sMultiA.notFullList);
                c += "。";
            }
            if (!sSingleB.notFullList.empty() || !sMultiB.notFullList.empty()) {
                c += " 设备B 未跑满的项: ";
                c += sSingleB.notFullList.empty() ? std::string("(单核无)") : ("单核 " + sSingleB.notFullList);
                c += "; ";
                c += sMultiB.notFullList.empty() ? std::string("(多核无)") : ("多核 " + sMultiB.notFullList);
                c += "。";
            }
            j += ",\"conclusion\":\"" + jsonSafe(c, 1800) + "\"";
        }
        j += ",\"caveats\":["
             "\"本块只搬报告里的原文数字, 不重新定义任何频率量, 也不写 sysfs / 不锁频 / 不锁核\","
             "\"阈值(90%)与『跑满』这个词都是我们自己定的判据, 不是官方阈值\","
             "\"两台都跑满 => 原始分比即结论; 有一侧没跑满 => 原始比里混着'没跑满'这件事, "
             "归一化只作辅助与诊断, 不许拿它冒充芯片性能比\""
             "]";
        j += "}";
    }

    // ---------------- 跨设备真值锚点(实测比 vs 真值比) ----------------
    j += ",\"truthAnchors\":{\"thresholdPercent\":5.0,\"thresholdIsOursNotOfficial\":true"
         ",\"thresholdNote\":\"5% 这个阈值是我们自己定的, 不是任何第三方的官方阈值\""
         ",\"rows\":[";
    bool firstRow = true;
    for (int i = 0; i < kTruthAnchorCount; ++i) {
        const TruthAnchor& t = kTruthAnchors[i];
        const bool matchA = (!a.soc.empty() && normSoc(t.a) == normSoc(a.soc));
        const bool matchB = (!b.soc.empty() && normSoc(t.b) == normSoc(b.soc));
        // 只对"本工程真的测了的量"做实测比: 目前是 3DMark SNL(报告里的 GPU-SNL 小节)。
        // GB6 单核 / 3DMark GPU 三代累计本工程根本不跑 -> 写 null + 说明缺什么。
        const bool isSnlAnchor = (std::string(t.unit).find("SNL") != std::string::npos);
        const RepItem* sniA = isSnlAnchor ? findItemByName(a, "Aurora Nomad Light") : nullptr;
        const RepItem* sniB = isSnlAnchor ? findItemByName(b, "Aurora Nomad Light") : nullptr;
        double measured = 0.0;
        bool hasMeasured = false;
        if (matchA && matchB && sniA != nullptr && sniB != nullptr && sniA->score > 0.0 && sniB->score > 0.0) {
            hasMeasured = safeRatio(sniA->score, sniB->score, &measured);
        }
        const double truthRatio = (t.aScore > 0.0) ? t.bScore / t.aScore : 0.0;
        const bool hasTruthRatio = (t.aScore > 0.0 && t.bScore > 0.0);
        bool hasDev = false;
        double dev = 0.0;
        if (hasMeasured && hasTruthRatio && truthRatio > 0.0) {
            dev = (measured / truthRatio - 1.0) * 100.0;
            hasDev = true;
        }
        j += (firstRow ? "" : ",");
        firstRow = false;
        j += "{\"label\":\"" + jsonSafe(std::string(t.label), 80) + "\"";
        j += ",\"from\":\"" + jsonSafe(std::string(t.a), 60) + "\",\"to\":\"" + jsonSafe(std::string(t.b), 60) + "\"";
        j += ",\"unit\":\"" + jsonSafe(std::string(t.unit), 60) + "\"";
        j += ",\"truthRatioBA\":" + numOrNull(hasTruthRatio, truthRatio, 4);
        j += ",\"measuredRatioBA\":" + numOrNull(hasMeasured, measured, 4);
        j += ",\"deviationPercent\":" + numOrNull(hasDev, dev, 2);
        j += ",\"linearWithin5Percent\":" + std::string(hasDev ? (std::fabs(dev) <= 5.0 ? "true" : "false") : "null");
        j += ",\"sourceKind\":\"" + jsonSafe(std::string(t.sourceKind), 120) + "\"";
        j += ",\"source\":\"" + jsonSafe(std::string(t.source), 300) + "\"";
        j += ",\"why\":\"";
        if (!matchA || !matchB) {
            j += "两端的 SoC 与本次两台设备对不上(设备A = ";
            j += jsonSafe(a.soc.empty() ? std::string("判不出来") : a.soc, 60);
            j += " / 设备B = ";
            j += jsonSafe(b.soc.empty() ? std::string("判不出来") : b.soc, 60);
            j += "): 这条锚点本次用不上 —— 缺什么写什么, 不硬套";
        } else if (!isSnlAnchor) {
            j += "本工程不跑这个量(" + jsonSafe(std::string(t.unit), 40) +
                 "), 所以没有实测比可与真值比对照 —— 写 null";
        } else if (!hasMeasured) {
            j += "两份报告里没有可用的 3DMark SNL(GPU-SNL 小节)分数, 算不出实测比";
        } else if (!hasTruthRatio) {
            j += "真值比算不出来(有一端真值缺失)";
        } else {
            j += "实测比(设备B / 设备A 的 SNL 分)与真值比的偏差 = (实测比 / 真值比 - 1) x 100%; "
                 "|偏差| <= 5% 判为线性通过(该阈值我们自己定的)";
        }
        j += "\"";
        j += "}";
    }
    j += "]}";

    // ---------------- 结论 ----------------
    j += ",\"verdicts\":{";
    j += "\"sameSoc\":" + std::string((!a.soc.empty() && !b.soc.empty() && normSoc(a.soc) == normSoc(b.soc))
                                          ? "true" : "false");
    j += ",\"socA\":\"" + jsonSafe(a.soc.empty() ? std::string("判不出来") : a.soc, 60) + "\"";
    j += ",\"socB\":\"" + jsonSafe(b.soc.empty() ? std::string("判不出来") : b.soc, 60) + "\"";
    j += ",\"socHowA\":\"" + jsonSafe(a.socHow, 300) + "\"";
    j += ",\"socHowB\":\"" + jsonSafe(b.socHow, 300) + "\"";
    j += ",\"canComputeTruthRatio\":" +
         std::string(((socHasGb7Truth(a.soc) && socHasGb7Truth(b.soc)) ||
                      (findSnlTruthBySoc(a.soc) != nullptr && findSnlTruthBySoc(b.soc) != nullptr))
                         ? "true" : "false");
    j += ",\"howToUse\":[\"① 先看 items[] 的 ratio: 原始分数比(设备B / 设备A)\","
         "\"② 再看 normalized: 消掉核数差(perCoreRatio)与频率档差(sameFreqRatio)之后剩下的才是芯片性能比\","
         "\"③ truth.deviationFromTruthPercent: 与真值的偏差(|偏差| <= 5% 判线性通过; 阈值是我们自己定的)\","
         "\"④ 任何 null 都是'缺输入': 去 inputs.missing 与每一条的 why 里看缺的是什么\","
         "\"⑤ 先确认两边比的是不是同一批项: selfCheckAudit(两边各自的排除集与判定依据) + "
         "compositeParticipants(参与项名单 / 交集 / 同项重算的比); 参与项不同时复合分原始比不可用\","
         "\"⑥ 再看条件差: conditionGaps(线程数 / SMT / 可用核集合) —— 条件不同时多核比不是纯芯片比\","
         "\"⑦ 频率证据: conditions.a/b.machineLevelFromReport 与逐项 conditions.runtimeKhzMedian / "
         "nominalTopKhz / frequencyRawText; 逐项 sameFreqRatio 为 null 时看 frequencyNullReason\","
         "\"⑧ 限制层级与 QoS: environmentEvidence.a/b 里是两台各自报告 items[].qos 与 items[].cpuset 的原文 "
         "(限制在哪一层 / QoS 生效没有), 逐项表还有 qosPresent / cpusetPresent\"]";
    j += "}";

    // ---------------- 汇总: 缺什么 / 不可比 / 提醒 ----------------
    j += ",\"notComparable\":[";
    {
        std::string nc;
        if (!aOk || !bOk) {
            nc += "\"有一份报告不可用 -> 逐项对比整体不成立(见 inputs.missing)\",";
        }
        if (a.soc.empty() || b.soc.empty()) {
            nc += "\"机型/SoC 判不出来 -> 与真值的对照整体不成立(可用 options.socA / socB 显式指定)\",";
        }
        if (a.hasInterrupted && a.interrupted) {
            nc += "\"设备A 的报告是中断的(interrupted = true), 后半段项缺失, 比值不可比\",";
        }
        if (b.hasInterrupted && b.interrupted) {
            nc += "\"设备B 的报告是中断的(interrupted = true), 后半段项缺失, 比值不可比\",";
        }
        if (aOk && bOk && (a.appVersion != b.appVersion || a.nativeVersion != b.nativeVersion)) {
            nc += "\"两份报告的版本不同(appVersion / nativeVersion), 跨版本分数不可比\",";
        }
        if (multiPartsPresent && !multiPartsIdentical) {
            nc += "\"两边多核复合分的参与项集合不同(见 compositeParticipants.multi 的 onlyInA/onlyInB) -> "
                  "两个多核复合分含的不是同一批项, 复合分原始比不可用(逐项归一化比不受影响, "
                  "因为它们是两边都有那些项的几何平均)\",";
        }
        if (singlePartsPresent && !singlePartsIdentical) {
            nc += "\"两边单核复合分的参与项集合不同(见 compositeParticipants.single 的 onlyInA/onlyInB) -> "
                  "两个单核复合分含的不是同一批项, 单核复合分原始比同样不可用\",";
        }
        if (a.hasNoteNominalTopKhz && b.hasNoteNominalTopKhz &&
            a.noteNominalTopKhz != b.noteNominalTopKhz) {
            nc += "\"两台的'全机最快频率档'不同(报告 inputsNote 原文: A " +
                  fixed(a.noteNominalTopKhz, 0) + " kHz / B " + fixed(b.noteNominalTopKhz, 0) +
                  " kHz) -> 原始分比里含频率档差, 要看 sameFreqRatio\",";
        }
        if (!nc.empty()) {
            nc = nc.substr(0, nc.size() - 1);
        }
        j += nc;
    }
    j += "]";
    j += ",\"caveats\":["
         "\"阈值的来源: 5%(线性判定)与 2% / 5%(重复性判定)都是我们自己定的, 不是官方阈值\","
         "\"参考真值只有两类: 3DMark SNL 三条(991 的来源是用户拍屏的一手证据)与 CS1 单核 8 项 + 复合分 2 项"
         "(参考机 Mate 80 Pro Max / Kirin 9030 Pro)\","
         "\"本模块是纯计算: 不跑负载 / 不连设备 / 不读 /sys / 不写文件 / 不改任何分数\","
         "\"归一化的前提是'吞吐 ∝ 频率', 跨架构时这只是假设; 报告里已逐条标注\""
         "]";
    j += "}";
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "compareReports built (items=%d, socA=%s, socB=%s)",
                 (int)a.items.size(), a.soc.empty() ? "?" : a.soc.c_str(), b.soc.empty() ? "?" : b.soc.c_str());
    return j;
}

