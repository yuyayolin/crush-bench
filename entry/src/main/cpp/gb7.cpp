// CS1: 负载注册表与计分
// ============================================================================
//  计分方式(用户明确要求: 不再用参考机锚定)
//      单项分 = k_项 x (本机吞吐换算到 GB7 官方单位后的值)
//             = k_项 x (o.metric x conv_项)
//  其中 k_项 是"每单位官方吞吐对应的官方单项分", 用 ref/gb7_cpu_dataset.json 里 14 台
//  真实 GB7 结果页数据在对数域做最小二乘(等价于各项 score/吞吐 比值的几何平均)拟合
//  得到, 每项离散度(最大相对偏差)都 <= 0.36% —— 即"k 是常数"这个假设在 CPU 全部 16 项
//  上都被数据证实, 而不是抄一个整数近似值。
//
//  拟合样本(14 台, 全部来自 browser.geekbench.com, 原始 HTML 在 ref/ 下):
//      AMD Ryzen AI Max+ 395     https://browser.geekbench.com/v7/cpu/1
//      Apple M6 Pro              https://browser.geekbench.com/v7/cpu/389219
//      Apple M6 (x2)             https://browser.geekbench.com/v7/cpu/436337 , /526356
//      Intel Core Ultra 7 255H   https://browser.geekbench.com/v7/cpu/500109 (官方标 Invalid)
//      samsung SM-S948U1 (SD8E5) https://browser.geekbench.com/v7/cpu/523625
//      iPhone 16 Pro Max (A18P)  https://browser.geekbench.com/v7/cpu/532751
//      iPhone 15 Pro (A17P)      https://browser.geekbench.com/v7/cpu/535872
//      HUAWEI CMU-AL10 (Kirin)   https://browser.geekbench.com/v7/cpu/536410
//      HUAWEI PAL-AL00           https://browser.geekbench.com/v7/cpu/536374
//      HUAWEI SGT-AL00 (Kirin)   https://browser.geekbench.com/v7/cpu/492596
//      HUAWEI ELS-AN00 (Kirin)   https://browser.geekbench.com/v7/cpu/536160
//      HUAWEI BLK-LX9 (x2)       https://browser.geekbench.com/v7/cpu/536222 , /535742
//  多核 k 只用多核表的样本(7 项 14 台, Text Processing 13 台: /500109 该项官方为 0)。
//
//  官方明文(可以逐字核对的两条, 引用时不要扩写):
//    1) 校准: "scores are calibrated against a baseline score of 2,500 (which is the
//       score of a Lenovo Legion with an AMD Ryzen 7 7700 processor). Higher scores are
//       better, with double the score indicating double the performance."
//       -- https://www.geekbench.com/doc/geekbench7-cpu-workloads.pdf
//    2) 多核套件只包含"真实场景里确实多线程"的负载(原文举例: HTML5 Browser 不进多核套件,
//       因为浏览器是单线程/弱线程) -- https://www.geekbench.com/blog/2026/07/geekbench-7/
//       -> 本表 inMulti == true 的 8 项, 就是官方多核表里出现的 8 项。
//  注: "总分 = 各单项分的几何平均"不是官方明文, 而是在 14 台设备的 20 个总分上数值
//      验证出的规律(19/20 吻合到 <0.1%, 唯一例外是官方标 Invalid 的 /500109), 见
//      gb7CompositeSingle / gb7CompositeMulti。
//
//  单位换算总原则: conv 的定义是"官方单位值 = 本实现 o.metric x conv"; 逐项依据写在
//  下表 basis 字段与 ref/selfcheck_gb7_k.py 里。语义对不上的项不计分(scored=false,
//  分数返回 0 并在 basis 写明原因), 不硬凑。
// ============================================================================
#include "gb7.h"
// QoS 运行条件(实现在 qos_priority.h 里, header-only):
// 本文件的 AuroraAffinityScope 直接调用 auroraQosLoadSessionBegin/End, 必须在这里包含它。
// (并发的 QoS 改动只把 include 加在 gb7_parallel.h 上, 而 gb7.cpp 不在那条包含链上 ——
//  gb7.cpp 会 "error: use of undeclared identifier"; 这一行是把那处遗漏补齐, 不改 QoS 的任何逻辑。)
#include "qos_priority.h"

// 只读分层诊断(cgroup / cpuset 分组 / core_ctl): 回答"限制到底在哪一层"。只读 ——
// 只用 fopen(path,"r") 与 opendir/readdir, 一个字节都不写这些节点; 不计分。
// 口径、errno 记法与结论规则见 cpuset_probe.h 的文件头。
#include "cpuset_probe.h"
#include <algorithm>
#include <cmath>

// 本文件后面定义的两个小工具(静态函数), 这里先声明: compositeJson() 也要用它们。
static std::string gb7JsonSafe(const std::string& s);
static std::string fixed(double v, int digits);
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// CPU 亲和性会话的 RAII 守卫(实现在 cpu_affinity.h)
// ---------------------------------------------------------------------------
// 位置与时机: 构造 -> 负载函数被调用之前(严格早于负载内部的 t0); 析构 -> 负载返回之后
// (晚于 t1)。因此绑核开销一分钱都不会进 o.ms; 采样也在析构前完成, 拿到的是负载真正
// 跑过的那个核。用 RAII 而不是裸调用, 是因为负载可能抛异常(典型是内存不足的
// std::bad_alloc): 异常路径同样必须关会话 + 还原主线程掩码, 否则 napi 的异步工作
// 线程会被永久钉在一个核上, 连后面的 GPU7 阶段都受影响。
class AuroraAffinityScope {
public:
    explicit AuroraAffinityScope(int threads)
    {
        // QoS 运行条件(与绑核同一位置、同样严格早于负载 t0):
        //   ① 进程内第一次进入本项时做一次"设 QoS / 不设 QoS"的对照探测(约 4 x 150 ms,
        //      固定整数运算 + 固定墙钟预算, 不是跑分项, 不进任何 metric/分数/复合分);
        //   ② 给当前线程(= 真正跑负载的那条线程)设 QOS_USER_INTERACTIVE。
        // 设备不支持 / canIUse 为假 / 符号拿不到 -> 静默降级并记账, 负载照常跑, 不抛异常。
        auroraQosLoadSessionBegin();
        auroraAffinitySessionBegin(threads);
    }
    ~AuroraAffinityScope()
    {
        (void)auroraAffinitySessionEnd();
        // 还原当前线程的 QoS: napi 异步工作线程会被复用(后面还有 GPU7 / 其它异步任务),
        // 不复原等于把整个异步线程永久提到最高档 —— 与"不还原掩码"是同一类错误。
        auroraQosLoadSessionEnd();
    }
    AuroraAffinityScope(const AuroraAffinityScope&) = delete;
    AuroraAffinityScope& operator=(const AuroraAffinityScope&) = delete;
};

// ---------------------------------------------------------------------------
// 运行时实际频率采样的 RAII 守卫(实现在 cpu_freq_sample.cpp / .h)
// ---------------------------------------------------------------------------
// 为什么用 RAII: 负载可能抛异常(典型是内存不足的 std::bad_alloc)。异常路径同样必须停掉
// 采样线程并汇总, 否则会留下一个还在每 200ms 读一次 sysfs 的孤儿线程, 影响后面的所有项。
// 位置与时机: 构造 -> 负载函数被调用之前(严格早于负载内部的 markStart / t0);
//             析构 -> 负载返回之后(晚于负载内部的 markStop / t1)。
// 因此"起采样线程 / 停采样线程 / 汇总成文本"三件事一分钱都不进 o.ms;
// 计时区间内只剩两次互斥量加解锁(markStart / markStop, 各约 20~60 ns), 而且它们写在
// t0 之前 / t1 之后 —— 连这点开销也在计时区间之外。口径见 cpu_freq_sample.h。
class AuroraFreqScope {
public:
    explicit AuroraFreqScope(int threads) { auroraFreqSampleSessionBegin(threads); }
    ~AuroraFreqScope() { auroraFreqSampleSessionEnd(); }
    AuroraFreqScope(const AuroraFreqScope&) = delete;
    AuroraFreqScope& operator=(const AuroraFreqScope&) = delete;
};

// ---------------------------------------------------------------------------
// 单位口径的两个实测结论(从 ref 的 14 台设备原始数字里拟合出来的, 不是查文档得到的)
//  A) 字节类单位(File Compression / Asset Compression)的 MB->GB 是 1024 进位:
//     把 GB/sec 的设备按 GB = 1024 x MB 归一后, File Compression 的 k 离散度从 2.34%
//     降到 0.07%(Asset Compression 从 2.79% 降到 0.36%)。本实现的 metric 恰好也是
//     2^20 口径(这两个负载的文件里除以 1048576), 所以这两项取 1:1。
//     风险: 若官方 MB 实为 10^6 字节, 这两项的 k 应再乘 1.048576(<=4.9%, 见报告)。
//  B) 计数类单位(像素/行/页/样本/图/路线)的 K/M/G 是 1000 进位: 用 GPU 页里同一项
//     同时出现 pixels/sec 与 Kpixels/sec 的设备互校, 系数正是 1000; CPU 页的
//     Klines/Ksamples/Kpages 用 1000 归一后离散度都 <0.4%。
// ---------------------------------------------------------------------------
static const char* const BASIS_FC =
    "官方 MB/sec; 换算 1:1 (本实现 metric = 压缩前原始字节/1048576/秒, 与官方页面 MB->GB 1024"
    " 进位的口径一致) [依据: 14 台设备按 1024 归一后 k 离散度 0.07%; 来源 v7/cpu/1, /523625,"
    " /536410]。工作量按官方定义重做(2026-10-04): 三个压缩包 x 三种格式(LZ4 / zlib level 1 /"
    " Zstandard level 3, 后者为 vendored 官方 zstd v1.5.6) x (压缩 + 解压 + SHA1 校验);"
    " 口径(conv)未变。若真机吞吐仍低于官方量级, 原因是本机 zlib 为 sysroot 的普通 libz,"
    " 而官方量级需要 zlib-ng 类 SIMD/硬件 CRC32 实现 —— 已在报告里量化说明, 不用调档位去凑";
static const char* const BASIS_NAV =
    "官方 routes/sec; 换算 1:1 (同为每秒完成的路线数; 官方是 24 条 OSM 路线 Dijkstra,"
    " 本实现也是 24 条 Dijkstra, 地图数据不同) [来源 v7/cpu/1, /523625, /536410]";
static const char* const BASIS_HTML =
    "官方 pages/sec; 换算 1:1 (同为每秒渲染完成的页面数; 官方用无头浏览器打开 8 个真实网站,"
    " 本实现是自写 HTML/CSS/布局/绘制, 页面内容与官方不同 -> 仅同机纵向可比)";
static const char* const BASIS_PDF =
    "官方 Mpixels/sec; 换算 1:1 (口径已修正, 2026-10-04): 本实现 metric = 页宽 x 页高 x 实际"
    " 栅格化页数/1e6/秒, 即输出位图像素每秒, 与官方同义。修正前分子还乘了本实现内部的 8x"
    " 子扫描线因子, 那是实现细节而非官方计数器(详见 gb7_pdf.cpp 文件头的四条依据); 同时把"
    " 内部采样密度由 8 降到 1, 与 PDFium 在 150 DPI 下 1 采样/像素对齐。conv 仍为 1.0"
    " (分子定义改了, 换算系数不用改)。残余差距(自写光栅化内核每像素成本高于 Skia)记录";
static const char* const BASIS_PHOTOLIB =
    "官方 images/sec; 换算 1:1 (同一张照片 = 一次处理单元; 官方一张是 JPEG/JPEG-XL/DNG 原图 +"
    " MobileNetV1 SSDLite 打标 + SQLite, 本实现一张是 1024x768 的编解码+色调流水线,"
    " 单元工作量不同 -> 仅同机纵向可比)";
static const char* const BASIS_CLANG =
    "官方 Klines/sec; 换算 1:1, 但源文件与官方不同: 官方编译 Lua 解释器, 本实现用 chibicc"
    " 编译自带源文件, 只有单位相同 -> 仅同机纵向可比";
static const char* const BASIS_TEXT =
    "官方 pages/sec; 换算 1:1 (本实现一页 = 1 MiB 词法扫描; 官方是 Python 3.13 把 190 个"
    " Markdown 转 HTML, 一页的字节数官方未公开, 倍数关系未知 -> 仅同机纵向可比)";
static const char* const BASIS_ASSET =
    "官方 MB/sec; 换算 1:1 (本实现 metric = 纹理字节/1048576/秒; 官方是 ASTC/BC7 纹理 + Draco"
    " 几何, 本实现是自写 BC1 编码器【PCA 主轴端点 + BC1 规格调色板 + 解码后颜色最近邻 + 2 轮"
    " 端点细化】, 编解码器不同 -> 仅同机纵向可比)";
static const char* const BASIS_HDR =
    "官方 Mpixels/sec; 换算 1:1 (口径已修正, 2026-10-04): 本实现 metric = 输出图像宽 x 高"
    "/1e6/秒, 分子只有输出像素, 与官方同义(官方原文: one 16 MP HDR image from six 16 MP SDR"
    " photos, 被计量的就是那张成品图)。修正前分子乘了 3 个颜色分量, 那是本实现的实现细节;"
    " 同时把负载由『单图 tone map x3 趟』改成真实的 6 曝光合成(曝光权重 + 去鬼影 + 加权融合 +"
    " 色调映射 + 显示 gamma)。conv 仍为 1.0(分子定义改了, 换算系数不用改)。残余差距"
    "(自写标量内核 vs 官方向量化内核)记录";
static const char* const BASIS_PHOTOEDIT =
    "官方 images/sec; 换算 = 1/6: 本实现 metric 是 Mpx/s, 一张照片 = 3000x2000 = 6.0 Mpx"
    " (gb7_batch3.cpp 的 w/h 常量), 故 images/s = metric/6。官方对 10 张照片施加多类效果,"
    " 单张像素数未公开, 这里以本实现的一张为单元 -> 仅同机纵向可比";
static const char* const BASIS_RAY =
    "官方 Ksamples/sec; 换算 x1000: 本实现 metric = 宽 x 高 x 每像素采样数/1e6/秒 = Msamples/s"
    " (384x384, 单核与多核 samples=8, 见 gb7_batch3.cpp), 每个 metric 单位就是"
    "一条完整的相机样本光路"
    " (含平均 4 次反弹 + 面光源立体角采样的阴影线), 与 Cycles 的 samples 同义, 故"
    " Ksamples/s = metric x 1000。官方用 Blender Cycles + Embree(场景与材质不同, 单样本成本"
    " 仍有差异) -> 仅同机纵向可比";
static const char* const BASIS_SFM =
    "未计分: 官方单位是 Kpixels/sec(图像像素每秒), 本实现 metric = 三维点数 x 相机数 x BA 迭代"
    "/1e6/秒(gb7_sfm.cpp), 是导出量而非图像像素数; 一个三维点等于多少像素没有任何可复核的定义"
    " (官方 56.7 Kpixels/sec = 0.057 Mpx/s 也远低于任何真实特征检测的像素吞吐, 说明其计数基准"
    " 未公开), 因此不硬凑, k 记 0, 并在单核复合分里按未计分剔除";
static const char* const BASIS_GAMEPHYS =
    "官方 FPS; 换算 = 1000000/8192 = 122.0703125: 本实现 metric = 刚体数 x 步数/1e6/秒"
    " (n=8192, 见 gb7_batch3.cpp), 每步推进全部 8192 个刚体 = 一帧物理更新(dt=0.016),"
    " 故 FPS = metric x 1e6/8192。假设: 官方 1 FPS 也是'一帧物理推进', 但官方 Jolt 场景的"
    " 刚体数未公开 -> 仅同机纵向可比";
static const char* const BASIS_VENC =
    "官方 FPS; 换算 = 1000000/921600 = 1.0850694444: 本实现帧尺寸取 kVW4 x kVH4 = 1280x720"
    " (gb7_video.cpp), metric = 帧亮度像素/1e6/秒, 故 FPS = metric/0.9216。"
    " 官方是 AOM/AV1 编码屏幕共享画面, 帧分辨率未公开 -> 仅同机纵向可比";
static const char* const BASIS_VDEC =
    "官方 FPS(官方结果页把它显示成 Video Player, 是同一个负载: AV1 解码 + Opus 解码 +"
    " Whisper 语音识别); 换算 = 1000000/921600 = 1.0850694444, 帧尺寸同 Video Encoder"
    " (gb7_video.cpp 1280x720)。官方视频分辨率未公开 -> 仅同机纵向可比";
static const char* const BASIS_AUDIO =
    "官方 Msamples/sec; 换算 = 0.5: 本实现 metric = 输入 PCM 字节数/1e6/秒(gb7_audio.cpp,"
    " 48000Hz x 2 声道 x 16bit), 1 个样本 = 1 个 int16 = 2 字节(两个声道的样本都计入), 故"
    " Msamples/s = metric/2。若官方按每声道样本计数, 系数应为 1/4, 风险已记录。"
    " 官方是 Opus 编码 2 分钟音频";

// ---------------------------------------------------------------------------
// 注册表。kSingle/kMulti = 每单位官方吞吐对应的官方单项分(拟合值, 见文件头);
// conv 把本实现 metric 换算成官方单位(官方值 = metric x conv)。
// 单项分 = kSingle(多核阶段用 kMulti) x conv x metric。
// scored == false 的项语义不可比: 分数恒为 0, 不进入复合分。
// ---------------------------------------------------------------------------
struct Entry {
    const char* name;
    const char* section;
    Gb7Outcome (*run)(int);
    const char* gb7Unit;   // GB7 结果页单位
    double kSingle;        // 单核 k(官方单位口径)
    double kMulti;         // 多核 k(官方多核表口径; 非多核项与 kSingle 相同)
    double conv;           // 官方单位值 = 本实现 metric x conv
    bool inMulti;          // 是否属于官方多核 8 项
    bool scored;           // false = 未计分(原因写在 basis)
    const char* basis;
};

Entry ENTRIES[] = {
    //                        name                      section            run                        gb7Unit        kSingle         kMulti          conv                 inMulti scored basis
    {"File Compression",       "Productivity",     gb7RunFileCompression,      "MB/sec",       7.027686550,    7.030028980,    1.0,                 true,  true,  BASIS_FC},
    {"Navigation",             "Productivity",     gb7RunNavigation,           "routes/sec",   181.6940430,    181.6940430,    1.0,                 false, true,  BASIS_NAV},
    {"Text Processing",        "Productivity",     gb7RunTextProcessing,       "pages/sec",    16.94929558,    16.95280516,    1.0,                 true,  true,  BASIS_TEXT},
    {"Asset Compression",      "Productivity",     gb7RunAssetCompression,     "MB/sec",       45.02062482,    45.01440921,    1.0,                 true,  true,  BASIS_ASSET},
    {"Photo Library",          "Image Synthesis",  gb7RunPhotoLibrary,         "images/sec",   291.9049489,    291.8613852,    1.0,                 true,  true,  BASIS_PHOTOLIB},
    {"Photo Editor",           "Image Synthesis",  gb7RunPhotoEditor,          "images/sec",   54.41947208,    54.40966834,    0.16666666666666666, true,  true,  BASIS_PHOTOEDIT},
    {"HDR",                    "Image Synthesis",  gb7RunHdr,                  "Mpixels/sec",  16.55415633,    16.55561091,    1.0,                 true,  true,  BASIS_HDR},
    {"Ray Tracer",             "Image Synthesis",  gb7RunRayTracer,            "Ksamples/sec", 3.560843717,    3.559198565,    1000.0,              true,  true,  BASIS_RAY},
    {"Game Physics",           "Image Synthesis",  gb7RunGamePhysics,          "FPS",          24.71979771,    24.71979771,    122.0703125,         false, true,  BASIS_GAMEPHYS},
    {"PDF Viewer",             "Productivity",     gb7RunPdfViewer,            "Mpixels/sec",  22.27127003,    22.27127003,    1.0,                 false, true,  BASIS_PDF},
    {"HTML5 Browser",          "Productivity",     gb7RunHtml5Browser,         "pages/sec",    80.00299486,    80.00299486,    1.0,                 false, true,  BASIS_HTML},
    {"Audio Encoder",          "Media",            gb7RunAudioEncoder,         "Msamples/sec", 718.4717517,    718.4717517,    0.5,                 false, true,  BASIS_AUDIO},
    {"Video Encoder",          "Media",            gb7RunVideoEncoder,         "FPS",          27.49885069,    27.49885069,    1.0850694444444444,  false, true,  BASIS_VENC},
    {"Video Decoder",          "Media",            gb7RunVideoDecoder,         "FPS",          9.459432954,    9.459432954,    1.0850694444444444,  false, true,  BASIS_VDEC},
    {"Structure from Motion",  "Image Synthesis",  gb7RunStructureFromMotion,  "Kpixels/sec",  0.0,            0.0,            0.0,                 false, false, BASIS_SFM},
    {"Clang",                  "Productivity",     gb7RunClang,                "Klines/sec",   184.8221414,    185.0482201,    1.0,                 true,  true,  BASIS_CLANG},
};

const int ENTRY_COUNT = (int)(sizeof(ENTRIES) / sizeof(ENTRIES[0]));

// gb7RunTest 最近一次的单项分(复合分在没传数组时用它, 方便 ArkTS 侧接线)
//
// 阶段维度 [2][ENTRY_COUNT](2026-10-08 修): 第一维 0 = 单核阶段, 1 = 多核阶段。
// 为什么必须分阶段 —— 这是真机上"报告里的单核复合分 328.2 / native 算 561.1, 差 1.7 倍"
// 这个核对不一致的唯一根因:
//   * 单核阶段跑全部 16 项(threads = 1), 每一遍都写 g_lastScore[?][id] = 该项的单核分;
//   * 多核阶段只跑 8 项(ENTRIES[i].inMulti), 但它同样写同一个 id 槽位 —— 于是这 8 项的
//     "单核分"被覆盖成了多核分;
//   * 报告生成时调 gb7CompositeSingle()(旧写法不传数组)读的就是这份缓存, 算出来的
//     "单核复合分"其实是 {8 项多核分} ∪ {7 项单核分} 的几何平均 => 561.1,
//     而卡片口径 geoOf(out.gb7Single)(15 项单核分)是 328.2。
//   数值自洽: (561.1 / 328.2)^15 = 3121 = 那 8 项多核/单核比值的乘积
//   => 每项并行加速比几何平均 = 2.73x(8 核设备上的合理值), 与"多核分污染"完全吻合。
//   多核侧之所以看不出问题, 是因为 gb7CompositeMulti() 只统计 inMulti 的 8 项,
//   而这 8 项恰好都被多核阶段刷新过 —— 923.8 vs 923.9 的 0.1 只是 JSON 里 %.1f 的舍入。
// 加了阶段维度之后, "单核那一遍的分"再也不会被多核那一遍改掉。
const int GB7_STAGE_SINGLE = 0;
const int GB7_STAGE_MULTI = 1;
const int GB7_STAGE_COUNT = 2;

// 阶段判定: 与 gb7TestKOfficial(id, multi) 用的是同一条判据(threads > 1),
// 因此"用哪张 k 表"与"存到哪个阶段槽位"永远不会各说各话。
inline int gb7StageOf(int threads)
{
    return (threads > 1) ? GB7_STAGE_MULTI : GB7_STAGE_SINGLE;
}

double g_lastScore[GB7_STAGE_COUNT][ENTRY_COUNT];
bool g_lastValid[GB7_STAGE_COUNT][ENTRY_COUNT];

Gb7Outcome emptyOutcome()
{
    Gb7Outcome o;
    o.name = "";
    o.section = "";
    o.ms = 0.0;
    o.score = 0.0;
    o.metric = "";
    o.unit = "";
    o.parallelism = 0.0;
    o.basis = "";
    o.diag = "";
    o.runFreq = "";
    return o;
}

// 按 id 取分: 传入数组优先, 否则用本阶段最近一次 gb7RunTest 的结果
//
// stage 只影响"回退路径": ArkTS 传了显式数组时, 数组就是唯一口径(见 compositeJson 的
// scoreSource 字段) —— 这一点是"报告里的复合分与 native 算的复合分必须逐位一致"的保证。
double scoreOf(const double* scores, int count, int id, int stage)
{
    if (scores != nullptr && count > id && id >= 0) {
        return scores[id];
    }
    if (id >= 0 && id < ENTRY_COUNT && stage >= 0 && stage < GB7_STAGE_COUNT && g_lastValid[stage][id]) {
        return g_lastScore[stage][id];
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
//   复合分的可重复性(2026-08-31 新增): 每一项都跑多轮之后, 复合分也能给出
//  逐轮复合分 -> 中位 / 最小 / 最大 / 相对离散度 + 可信度判定。
// ---------------------------------------------------------------------------
// 为什么要单独存一份"逐轮单项分矩阵": 复合分是所有项的几何平均, 只有在每一项都跑完
// 多轮之后才能按轮次重算。矩阵只用于统计, 不计分:
//   * 复合分的代表值仍然是"由各项中位分算出的几何平均"(与历史行为一致);
//   * 这里额外给出"逐轮复合分"这一列, 它的中位/最小/最大/离散度回答的是
//     "如果每次只跑一轮, 复合分会飘多少" —— 这正是主流跑分软件用重复性建立可信度的方式。
//   * GM 对每一项都是单调的, 所以"各项中位分算出的复合分"与"逐轮复合分的中位"很接近,
//     但不保证逐位相等 —— 两个数都报出来, 不替用户挑一个好看的。
// 同样带阶段维度: 第一维 0 = 单核 / 1 = 多核(理由同上, 与 g_lastScore 完全一致)。
// 不加这一维的话, 报告里"逐轮复合分的中位"这一列在单核口径下同样是混了 8 项多核分的假数。
double g_roundScore[GB7_STAGE_COUNT][ENTRY_COUNT][GB7_MAX_ROUNDS];
bool g_roundValid[GB7_STAGE_COUNT][ENTRY_COUNT][GB7_MAX_ROUNDS];
int g_roundCount[GB7_STAGE_COUNT][ENTRY_COUNT];

// ---------------------------------------------------------------------------
//  预热会话表(2026-10 新增): 每一个 (负载 id, 阶段) 在同一次会话里最多只预热一次
// ---------------------------------------------------------------------------
//  为什么需要它(用户点名的性能代价问题): 预热写在 gb7RunTest 里, 而 gb7RunTestRepeated 每一轮
//  都调一次 gb7RunTest —— 默认 2 轮时每一项的负载被跑了 4 遍(2 遍预热 + 2 遍正式),
//  CPU 小节墙钟接近翻倍。要求: 同一个 (负载 id, 阶段) 最多只预热一次, 也就是只在第 1 轮做,
//  第 2..N 轮直接进正式计时区间 —— 墙钟从 ~+100% 降到 ~+50%。
//  为什么按阶段分开存两份: 单核阶段与多核阶段是两次独立的测量(threads 不同), 单核预热过
//  不等于多核预热过。用一张表把两个阶段混在一起, 多核那一次就会被静默跳过预热。
//  什么时候重新武装: gb7RunTestRepeated 在"这一项这一阶段的多次测量"开始时清掉对应槽位
//  (那是一次新会话的开始), 于是下一次跑分 / 下一次单项测量照样会在它的第 1 轮预热。
//  跳过时不静默: 那一轮的 runFreq 里会写明"本项本阶段已在第 1 轮预热过, 本遍不再预热",
//  并引用第 1 轮的稳态判据结果(g_warmupEvidence, 与第 1 轮正文同源, 不是另算一遍)。
//  前提(与 cpu_affinity.h 的 SessionState / cpu_freq_sample 的采样会话同一条): napi 异步
//  逐项串行跑 GB7 CPU 负载, 同一时刻只有一项在跑 —— 因此这两张表不需要加锁。
//  另有: 不经过 gb7RunTestRepeated 而直接调 gb7RunTest(id, threads) 的路径(本工程当前没有),
//  语义同样是"同一次会话里最多预热一次"(第一次真预热, 之后跳过并引用那一次)。
bool g_warmupDone[GB7_STAGE_COUNT][ENTRY_COUNT];
std::string g_warmupEvidence[GB7_STAGE_COUNT][ENTRY_COUNT];

// 下面这些统计工具定义在本文件后面(匿名命名空间内), 这里先声明以便 compositeJson 使用
double medianOfList(const std::vector<double>& v);
double minOfList(const std::vector<double>& v);
double maxOfList(const std::vector<double>& v);
double spreadPctOf(const std::vector<double>& v, double med);
const char* verdictOfSpread(double spreadPct, int roundsOk);
std::string verdictText(const std::string& verdict, int roundsOk, double spreadPct);

// 几何平均复合分: 只统计"该模式下参与计分"且分数 > 0 的项
std::string compositeJson(const char* label, bool multi, const double* scores, int count)
{
    // 阶段(2026-10-08): 用哪一阶段的单项分, 与"用哪张 k 表"是同一条判据(见 gb7StageOf)。
    const int stage = multi ? GB7_STAGE_MULTI : GB7_STAGE_SINGLE;
    // 口径来源: 传了显式数组 = 以数组为唯一口径(报告就是这么调的, 于是 native 算出来的
    // 复合分与报告里卡片上那个数逐位一致); 没传 = 回退到 native 自己那份本阶段缓存。
    // 这个字段存在的意义: 让人一眼看出"这个复合分到底是由哪一份单项分算出来的", 不会再出现
    // "报告算 328.2 / native 算 561.1 却看不出差别在哪"这种情况。
    const bool explicitScores = (scores != nullptr && count > 0);
    double prod = 1.0;
    int n = 0;
    std::string items;
    std::string excluded;
    int excludedN = 0;
    bool usedItem[ENTRY_COUNT];
    for (int i = 0; i < ENTRY_COUNT; ++i) {
        usedItem[i] = false;
    }
    for (int i = 0; i < ENTRY_COUNT; ++i) {
        // ---- 排除规则(逐条写进 excluded[], 便于两台机器逐条对照) ----
        // 与历史口径完全一致, 一个字都没改: 只统计 scored 且(multi 时)inMulti 且分数 > 0 的项。
        const char* skipWhy = nullptr;
        if (!ENTRIES[i].scored) {
            skipWhy = "scored=false(按政策不计分, 原因见该项 basis)";
        } else if (multi && !ENTRIES[i].inMulti) {
            skipWhy = "inMulti=false(不属于官方多核 8 项, 官方多核结果页里也没有它)";
        } else {
            const double t = scoreOf(scores, count, i, stage);
            if (!(t > 0.0) || !std::isfinite(t)) {
                skipWhy = "本次没有拿到正的分数(计分项但这一遍没跑出吞吐)";
            }
        }
        if (skipWhy != nullptr) {
            char eb[224];
            snprintf(eb, sizeof(eb), "%s{\"id\":%d,\"name\":\"%s\",\"why\":\"%s\"}",
                     excluded.empty() ? "" : ",", i, ENTRIES[i].name, skipWhy);
            excluded += eb;
            ++excludedN;
            continue;
        }
        const double s = scoreOf(scores, count, i, stage);
        prod *= s;
        n++;
        usedItem[i] = true;
        char buf[192];
        snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"name\":\"%s\",\"score\":%.1f}", items.empty() ? "" : ",",
                 i, ENTRIES[i].name, s);
        items += buf;
    }
    const double geo = (n > 0) ? std::pow(prod, 1.0 / (double)n) : 0.0;
    char head[1024];
    snprintf(head, sizeof(head),
             "{\"ok\":true,\"mode\":\"%s\",\"stage\":%d,\"stageName\":\"%s\","
             "\"composite\":%.1f,\"count\":%d,"
             "\"scoreSource\":\"%s\",\"scoreSourceExplicit\":%s,"
             "\"scoreSourceText\":\"%s\","
             "\"excludedCount\":%d,\"items\":[",
             label, stage, multi ? "multi(多核阶段: 8 项套装, threads > 1)" : "single(单核阶段: 16 项全跑, threads = 1)",
             std::isfinite(geo) ? geo : 0.0, n,
             explicitScores ? "explicit-array" : "stage-cache",
             explicitScores ? "true" : "false",
             explicitScores
                 ? "口径 = ArkTS 传进来的本阶段各项分(按负载 id 索引); 与报告卡片上那个数同源, 必须逐位一致"
                 : "口径 = native 自己那份本阶段缓存(按负载 id, 带阶段维度)。"
                   "该缓存只写本阶段的分数, 不会被另一阶段覆盖; 但它的逐项数值可能与报告里的 %.1f 有舍入差",
             excludedN);
    std::string out = head;
    out += items;
    out += "],\"excluded\":[";
    out += excluded;
    out += "],\"excludedRule\":\"排除规则(与历史口径一致, 未改): 只统计 scored=true 且(多核时)inMulti=true "
           "且分数 > 0 的项; 被排除的项逐条列在 excluded[] 里\"";
    out += ",\"basis\":\"";
    out += (multi
                ? "多核复合分 = 官方多核 8 项(File Compression / Photo Library / Clang / Text Processing / Asset Compression / HDR / Photo Editor / Ray Tracer)单项分的几何平均; 官方无明文, 系 14 台设备多核表数值验证"
                : "单核复合分 = 单核 16 项中已计分项(Structure from Motion 未计分被剔除)单项分的几何平均; 官方无明文, 系 14 台设备单核表数值验证");
    out += "\"";
    // ----  复合分的可重复性(2026-08-31): 逐轮复合分 -> 中位/最小/最大/离散度 + 可信度 ----
    // 统计口径: 只用"按轮次重算得出的复合分"; 要算第 r 轮的复合分, 参与该项的每一项
    // 都必须有第 r 轮的有效读数, 否则那一轮不参与统计(并且把参与轮数报出来)。
    {
        std::vector<double> perRound;
        int usableRounds = -1;
        int itemsWithRounds = 0;
        for (int i = 0; i < ENTRY_COUNT; ++i) {
            if (!usedItem[i]) {
                continue;
            }
            ++itemsWithRounds;
            if (g_roundCount[stage][i] <= 0) {
                usableRounds = 0;
                break;
            }
            if (usableRounds < 0 || g_roundCount[stage][i] < usableRounds) {
                usableRounds = g_roundCount[stage][i];
            }
        }
        if (usableRounds > 0) {
            for (int r = 0; r < usableRounds; ++r) {
                double rp = 1.0;
                int rn = 0;
                for (int i = 0; i < ENTRY_COUNT; ++i) {
                    if (!usedItem[i] || !g_roundValid[stage][i][r]) {
                        continue;
                    }
                    rp *= g_roundScore[stage][i][r];
                    ++rn;
                }
                if (rn == n && n > 0) {
                    perRound.push_back(std::pow(rp, 1.0 / (double)rn));
                }
            }
        }
        const double med = medianOfList(perRound);
        const double mn = perRound.empty() ? 0.0 : minOfList(perRound);
        const double mx = perRound.empty() ? 0.0 : maxOfList(perRound);
        const double spread = spreadPctOf(perRound, med);
        const std::string verdict = perRound.empty()
                                        ? std::string("SINGLE_ROUND_NO_DATA")
                                        : std::string(verdictOfSpread(spread, (int)perRound.size()));
        out += ",\"repeatability\":{";
        out += "\"itemsUsed\":" + std::to_string(n);
        out += ",\"itemsWithRoundData\":" + std::to_string(itemsWithRounds);
        out += ",\"roundsPerItem\":" + std::to_string(usableRounds > 0 ? usableRounds : 0);
        out += ",\"compositePerRound\":[";
        for (size_t k = 0; k < perRound.size(); ++k) {
            out += (k == 0 ? "" : ",");
            out += fixed(perRound[k], 2);
        }
        out += "]";
        out += ",\"median\":" + (perRound.empty() ? std::string("null") : fixed(med, 2));
        out += ",\"min\":" + (perRound.empty() ? std::string("null") : fixed(mn, 2));
        out += ",\"max\":" + (perRound.empty() ? std::string("null") : fixed(mx, 2));
        out += ",\"dispersion\":" + (perRound.empty() ? std::string("null") : fixed(spread, 4));
        out += ",\"representative\":\"报告里的 composite 字段 = 由各项中位分算出的几何平均; "
               "这一块的 median = 逐轮复合分的中位。两个数很接近但不保证相等(GM 单调但非可加), "
               "都报出来, 不挑一个好看的\"";
        out += ",\"dispersionDefinition\":\"相对离散度 = (最大 - 最小) / 中位 x 100%\"";
        out += ",\"credibility\":{\"verdict\":\"" + verdict + "\""
               ",\"thresholdsAreOursNotOfficial\":true"
               ",\"thresholdsSource\":\"我们自己定的阈值(与单项、与 GPU-SNL 小节同一套), 不是官方阈值\""
               ",\"text\":\"" + gb7JsonSafe(verdictText(verdict, (int)perRound.size(), spread)) + "\"}";
        if (perRound.size() < 2) {
            out += ",\"singleRoundWarning\":\"本次每一项只跑 1 轮, 没有离散度数据, 不能据此判断复合分的可信度\"";
        }
        out += ",\"note\":\"逐轮复合分 = 把每一项那一轮的分按同一套几何平均重算一遍; "
               "某一轮只要有一项缺读数, 那一轮就不参与统计(不拿别的轮凑数)\"";
        out += "}";
    }
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------
//  重复性统计的小工具(2026-08-31)。全部是纯函数, 不碰任何负载。
//   代表值 = 中位(median) —— 这里没有、也不许加任何"取最大值/取最好一轮"的路径。
// ---------------------------------------------------------------------------
double medianOfList(const std::vector<double>& v)
{
    if (v.empty()) {
        return 0.0;
    }
    std::vector<double> s = v;
    std::sort(s.begin(), s.end());
    const size_t n = s.size();
    if (n % 2 == 1) {
        return s[n / 2];
    }
    return 0.5 * (s[n / 2 - 1] + s[n / 2]);
}

double minOfList(const std::vector<double>& v)
{
    double m = 0.0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i == 0 || v[i] < m) {
            m = v[i];
        }
    }
    return m;
}

double maxOfList(const std::vector<double>& v)
{
    double m = 0.0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i == 0 || v[i] > m) {
            m = v[i];
        }
    }
    return m;
}

// 相对离散度 = (最大 - 最小) / 中位 x 100%(与 GPU-SNL 同一定义; 中位 <= 0 时返回 0)
double spreadPctOf(const std::vector<double>& v, double med)
{
    if (v.size() < 2 || !(med > 0.0)) {
        return 0.0;
    }
    return (maxOfList(v) - minOfList(v)) / med * 100.0;
}

// 把负载给的 metric 字符串解析成数(与 gb7RunTest 里的计分解析同一套写法,
// 保证"统计用的吞吐"与"计分用的吞吐"是同一个数)
bool parseMetricText(const std::string& s, double* out)
{
    double v = 0.0;
    if (sscanf(s.c_str(), "%lf", &v) != 1 || !std::isfinite(v) || v < 0.0) {
        return false;
    }
    *out = v;
    return true;
}

// 取"中位那一轮"在 all 里的下标(用于代表值的原始文本: metric 字符串按负载自己的格式原样带出,
// 我们不重新格式化 metric —— 口径与展示格式都保持负载原来的样子)。
// 返回值: 精确命中中位的那一轮 > 最接近中位的那一轮 > -1(一个有效轮都没有)。
int medianRoundIndex(const std::vector<Gb7Outcome>& all, double medianMetric)
{
    if (!(medianMetric > 0.0)) {
        return -1;
    }
    //  2026-10 修  以前这里只做精确匹配, 匹配不到就返回 -1, 调用方于是静默保留了
    //   all.back() 的文本(最后一轮)。而 medianOfList 在偶数轮时返回的是中间两个数的
    //   均值 —— 默认就是 2 轮, 于是「中位那一轮」根本不存在, 精确匹配每次都会失败:
    //   真机实证(平板 tab75 / PDF Viewer): 报告显示 metric=10.0, 同一条目的代表分却是 224.9,
    //   而 224.9 / (k x conv) = 10.1 —— 两者差 1.0%, 恰好是该项轮间离散度 1.98% 的一半。
    //   显示出来的吞吐与显示出来的分数互相矛盾, 读的人只会以为计分被动过。
    // 修法: 精确命中优先(奇数轮时行为与以前逐位一致), 否则取最接近中位的那一轮。
    //   注意: 计分用的始终是 metricMedian(数值), 不是这一串文本 —— 差额由调用方写进 diag。
    int best = -1;
    double bestDiff = 0.0;
    for (size_t i = 0; i < all.size(); ++i) {
        double m = 0.0;
        if (!parseMetricText(all[i].metric, &m)) {
            continue;
        }
        const double d = std::fabs(m - medianMetric);
        if (d <= 1.0e-9 * (medianMetric > 1.0 ? medianMetric : 1.0)) {
            return (int)i;
        }
        if (best < 0 || d < bestDiff) {
            best = (int)i;
            bestDiff = d;
        }
    }
    return best;
}

// 可信度判定(阈值是我们自己定的, 报告里必须原样带上这句话)
const char* verdictOfSpread(double spreadPct, int roundsOk)
{
    if (roundsOk < 2) {
        return "SINGLE_ROUND_NO_DATA";
    }
    if (spreadPct <= GB7_RELIABLE_SPREAD_PCT) {
        return "RELIABLE";
    }
    if (spreadPct <= GB7_FAIR_SPREAD_PCT) {
        return "FAIR";
    }
    return "UNRELIABLE";
}

std::string verdictText(const std::string& verdict, int roundsOk, double spreadPct)
{
    char buf[512];
    if (verdict == "SINGLE_ROUND_NO_DATA") {
        return "本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度(不假装稳定)。"
               "想判断可信度请把轮数调到 >= 2(runGb7 的选项 {\"rounds\":3}); "
               "阈值(2% / 5%)是我们自己定的, 不是官方阈值。";
    }
    if (verdict == "NO_VALID_ROUNDS") {
        snprintf(buf, sizeof(buf),
                 "跑满 %d 轮但没有任何一轮给出有效吞吐读数, 因此没有代表值(写空, 不编数字)。",
                 roundsOk);
        return std::string(buf);
    }
    snprintf(buf, sizeof(buf), "%d 轮相对离散度 %.2f%%", roundsOk, spreadPct);
    const std::string head = buf;
    if (verdict == "RELIABLE") {
        return "重复性好: " + head + "(<= 2%), 代表值取中位。"
               "阈值(2% / 5%)是我们自己定的, 不是官方阈值。";
    }
    if (verdict == "FAIR") {
        return "重复性一般: " + head + "(2% ~ 5%), 代表值仍取中位, 但对比时请留意这个区间。"
               "阈值(2% / 5%)是我们自己定的, 不是官方阈值。";
    }
    return "本项重复性差, 该数字仅供参考: " + head +
           "(> 5%), 全部轮次的原始值见 repeatability.roundsDetail。"
           "阈值(2% / 5%)是我们自己定的, 不是官方阈值。";
}

} // namespace

int gb7TestCount()
{
    return ENTRY_COUNT;
}

std::string gb7TestName(int id)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return "";
    }
    return ENTRIES[id].name;
}

std::string gb7TestSection(int id)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return "";
    }
    return ENTRIES[id].section;
}

// 该负载是否属于官方多核 8 项(供多核阶段过滤显示)
bool gb7TestIsMulti(int id)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return false;
    }
    return ENTRIES[id].inMulti;
}

// 该项是否计分(false = 语义不可比, 分数恒 0)
bool gb7TestScored(int id)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return false;
    }
    return ENTRIES[id].scored;
}

// 单位换算与系数说明(UI 展示)
std::string gb7TestBasis(int id)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return "";
    }
    return ENTRIES[id].basis;
}

// 官方结果页上的单位
std::string gb7TestGb7Unit(int id)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return "";
    }
    return ENTRIES[id].gb7Unit;
}

// 官方单位值 = 本实现 metric x conv
double gb7TestConversion(int id)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return 0.0;
    }
    return ENTRIES[id].conv;
}

// 拟合出来的 k(官方单位口径: 官方分 = kOfficial x 官方单位吞吐)
double gb7TestKOfficial(int id, int multi)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return 0.0;
    }
    return multi ? ENTRIES[id].kMulti : ENTRIES[id].kSingle;
}

// 实际乘在本实现 metric 上的系数 = kOfficial x conv
double gb7TestK(int id, int multi)
{
    return gb7TestKOfficial(id, multi) * gb7TestConversion(id);
}

Gb7Outcome gb7RunTest(int id, int threads)
{
    if (id < 0 || id >= ENTRY_COUNT) {
        return emptyOutcome();
    }
    const Entry& e = ENTRIES[id];
    // ---- CPU 亲和性(计时区间之外) ----
    // 单核(threads<=1): 把"当前线程"= napi 异步工作线程 = 负载线程绑到大核簇(= 频率
    //   最高的那批核组成的集合, 至少 2 个核; 定义见 cpu_affinity.h), 负载随后就在簇内某个
    //   大核上串行跑完, 不会再随机掉到小核。
    // 多核(threads>1): 主线程同样绑大核簇(它负责建池/汇总), 池线程由 gb7ParallelFor 各自
    //   绑到"自己的均衡组"(频率降序轮流占位, 大核优先; 每组 >= 2 个核) —— 见 cpu_affinity.h
    //   与 gb7_parallel.h。
    // 为什么是"簇"而不是"最快的单个核": 单核掩码会被内核/厂商调度判成 misfit 并强行迁走
    //   (真机 16 项里有 5 项出现 cpuBound=true 却"起于 cpu4 -> 结束在 cpu7"), 详见
    //   cpu_affinity.h 文件头。簇内每个核同频, 内核在簇内怎么挪都不改变性能。
    // 读不到频率表时全部静默降级为"不绑", 只把"未绑定"这一事实记进 o.cpuInfo。
    const AuroraAffinityScope affinityScope(threads);
    // 运行时频率采样会话: 只在这里起采样线程(计时区间之外), 具体采样窗口由负载自己
    // 在计时区间内用 auroraFreqMarkStart/Stop 圈定(16 项每一处都已打点)。
    const AuroraFreqScope freqScope(threads);
    // -----------------------------------------------------------------------
    //  升频/稳态预热(2026-10 新增; 全部发生在计时区间之外)
    // -----------------------------------------------------------------------
    //  为什么要它: 真机单核项只有 1.0~5.3 s, 冷态起步时调频器可能还没把频率拉到稳态,
    //  计时区间就结束了 —— 那样测到的不是"这颗芯片在这项负载下的稳态性能"。
    //  口径(必须一字不改地守住):
    //    * 预热调用的就是同一个负载函数 e.run(threads): 同一份计算、同一份数据规模、
    //      同一个线程数口径; 它的返回值与它自己的耗时一律丢弃, 不进 o.ms。
    //    * 每遍预热的读数由 cpu_freq_sample 的后台采样线程按正式口径采下来; 稳态判据
    //      (占比中位达标 / 相邻两遍中位差 <= 3% / 单遍前后半段差 <= 3%)写在
    //      cpu_freq_sample.h, 命中即停, 最多 3 遍或累计 8 s —— 不无限加压。
    //    * 预热不改任何负载的算法/尺寸/metric/unit/k/conv/计分公式/线程数口径:
    //      这里一个字节的负载参数都没有动, 只是"多跑一遍同样的负载"。
    //  说明代价: 每项每轮多跑一遍同一负载(正常形态下每轮 1 遍预热), CPU 小节墙钟
    //  大约翻一倍; 换来的是"计时区间确实在稳态下开始"这一点可核对。
    //  异常路径: 预热里任何异常都被吃掉(预热失败最多是"没有预热证据"), 正式计时照跑。
    //  每个 (负载 id, 阶段) 只预热一次(见 g_warmupDone 的长注释): 第 1 轮真预热, 第 2..N 轮跳过
    const int warmStage = gb7StageOf(threads);
    const bool warmAlreadyDone = g_warmupDone[warmStage][id];
    if (!warmAlreadyDone) {
        for (int warmPass = 0; warmPass < auroraWarmupMaxPasses(); ++warmPass) {
            const int passesBefore = auroraWarmupPassesDone();
            auroraWarmupPassBegin();
            bool warmOk = true;
            try {
                (void)e.run(threads);   // 同一份负载 —— 输出与耗时丢弃
            } catch (...) {
                warmOk = false;
            }
            auroraWarmupPassEnd();
            if (!warmOk) {
                break;                  // 预热抛异常 -> 不再加压, 正式计时照跑(标注)
            }
            if (auroraWarmupPassesDone() <= passesBefore) {
                break;                  // 预热会话不可用(起不来采样线程等) -> 不空跑三遍
            }
            if (auroraWarmupSteady() != 0) {
                break;                  // 稳态判据命中
            }
            if (auroraWarmupTotalMs() >= auroraWarmupMaxTotalMs()) {
                break;                  // 预热总时长到点
            }
        }
        // 把这一轮的预热结论存下来, 供第 2..N 轮引用(摘要与正文同源, 不会自相矛盾)
        {
            char wsum[1024];
            const int wl = auroraWarmupSummaryText(wsum, (int)sizeof(wsum));
            g_warmupEvidence[warmStage][id] = (wl > 0)
                ? std::string(wsum)
                : std::string("第 1 轮没有采到可用的预热读数");
        }
        g_warmupDone[warmStage][id] = true;
    }
    // -----------------------------------------------------------------------
    //  正式计时: 从下面这一行开始才算 o.ms(预热的每一次调用都在它之前)
    // -----------------------------------------------------------------------
    Gb7Outcome o = e.run(threads);
    // 运行时频率采样: 在这里显式收尾(停后台采样线程 + 汇总出那一行文本), 因为紧接着就要
    // 读它。freqScope 的析构里还会再调一次 —— 那是给异常路径兜底的保险;
    // auroraFreqSampleSessionEnd() 是幂等的(第二次直接返回, 不会清空已汇总的文本)。
    auroraFreqSampleSessionEnd();
    // 采样时机: 负载已跑完(晚于 t1)、主线程掩码尚未还原 —— 采到的就是负载实际所在的核。
    o.cpuInfo = auroraAffinitySessionEnd();
    // ---- 运行时实际频率(旁路; 2026-10 新增) ----
    // 口径与采样实现见 cpu_freq_sample.h 文件头。这一段不改任何数值:
    //   * o.runFreq -> napi 层展开成 runGb7 JSON 的 "runFreq"(完整文本, 不受任何截断);
    //   * 同时把它接到 o.cpuInfo.cpuAllowedText 末尾 —— 即每项 note 里 "cpuset: …" 那一段的
    //     后面(ArkTS 的 cpuNote() 已经把 cpuAllowedText 拼进 note, 所以不动 ets)。
    // cpuAllowedText 前面已经装了 cpuset 全文与"线程数被夹"说明(容量 1024), 万一还是放不下,
    // 就退到 o.diag(diag 同样是旁路字段、同样已被 ArkTS 原样拼进 note) —— 两条路都不丢信息。
    try {
        // 容量 1024 -> 2048 -> 3072(2026-10): 多核那一行现在还要带"逐核占用率 + 空闲核比例"
        //   + "整项忙占比 < 5% 的核"(14 核 × ~24 字符 ≈ 340 字节, 新增判据再 ≈ 200 字节),
        //   2048 会把尾部的结论截掉 —— 那正是"日志里少了半句还没人发现"的来源。只加容量。
        // 容量 3072 -> 8192(2026-10): 多核那一行现在要多带 口径A/口径B 两套频率读数 +
        //   预热取证 + 上限取证, 3072 会把尾部结论截掉。只加容量, 内容与措辞不变。
        // 容量 8192 -> 16384(2026-10, 本轮) —— 为什么是这个数、上界怎么估的:
        //   ① 承接的那一行(g_text)的容量已从 8192 提到 16384, 因为"上限取证"那一段的容量
        //      从 2048 提到 8192 后, 它在真机 8.1 上实际要用 3919 字节(推导见
        //      cpu_freq_sample.cpp 里 kCapTextCap 处的 ①~③), 整行实测将从 5384 涨到 7256 字节;
        //   ② 这一环必须 >= 上游那一环的容量, 否则 auroraFreqSampleText() 的拷贝会在
        //      这一层把尾部截掉(这正是"取证文本被容量截断"这条链上最容易漏看的一环);
        //      两边同取 16384 -> 拷贝永远不触发截断分支;
        //   ③ 16384 >= 7256 x 1.5(= 10884) -> 相对真机需要 7256 字节的余量 = +126%;
        //   ④ 上界: 上游 g_text 的极端值 <= 16383(它自己放不下时会写"尾部被截断"标记),
        //      这一环同为 16384 -> 结构上不可能比上游小。只加容量, 内容与措辞不变。
        char freq[16384];
        const int freqLen = auroraFreqSampleText(freq, (int)sizeof(freq));
        if (freqLen > 0) {
            o.runFreq = freq;
            const size_t used = strlen(o.cpuInfo.cpuAllowedText);
            const size_t cap = sizeof(o.cpuInfo.cpuAllowedText);
            if (used + 3 + (size_t)freqLen + 1 <= cap) {
                snprintf(o.cpuInfo.cpuAllowedText + used, cap - used, "%s%s",
                         (used > 0) ? " · " : "", freq);
            } else {
                if (!o.diag.empty()) {
                    o.diag += " · ";
                }
                o.diag += freq;
            }
        }
        // 本遍跳过了预热 -> 写进报告, 不静默跳过
        //  口径: 预热每个 (负载 id, 阶段) 只做一次(第 1 轮), 第 2..N 轮不再预热(墙钟 +100% -> +50%);
        //  这里写明"为什么没有预热"并引用第 1 轮的预热与稳态判据结果。
        if (warmAlreadyDone) {
            std::string note = "本遍不再预热(本项本阶段已在第 1 轮预热过: 预热每个 "
                               "(负载 id, 阶段) 只做一次, CPU 小节墙钟从 +100% 降到 +50%); "
                               "引用第 1 轮的预热与稳态判据结果 = ";
            note += g_warmupEvidence[warmStage][id];
            if (!o.runFreq.empty()) {
                o.runFreq += " · ";
            }
            o.runFreq += note;
        }
    } catch (...) {
        // 旁路诊断不能让本项失败: 这里只有 std::string 拼接可能抛(bad_alloc),
        // 真抛了就丢掉这一行频率文本, 分数与单位一个字都不受影响。
    }
    // ---- QoS 运行条件上报(旁路; 不计分) ----
    // 用户要求:"必须在报告里标注『本次跑分是否启用了 QoS 及其等级』"。
    // 这里把 qos_priority.h 记下的全部事实(是否启用 / 负载与旁路各自用了哪一档 / 每个调用的
    // 返回值与 errno / libqos.so 与 canIUse 的探测结果 / "设-不设"对照的份额比)接进本项诊断。
    // 路径与上面那行运行时频率完全一样:
    //   * 优先接进 o.cpuInfo.cpuAllowedText(ArkTS 的 cpuNote() 已把它拼进 note, 不动 ets);
    //   * 装不下就退到 o.diag(同样是已被 ArkTS 原样拼进 note 的旁路字段);
    //   * 同时放进结构化字段 o.qos(napi 展开成 runGb7 JSON 的 "qos" 键)。
    // 万一这里抛异常(bad_alloc), 也只是少一行诊断, 分数与单位一个字都不受影响。
    try {
        const std::string qosLine = auroraQosStatusLine();
        if (!qosLine.empty()) {
            o.qos = qosLine;
            const size_t used = strlen(o.cpuInfo.cpuAllowedText);
            const size_t cap = sizeof(o.cpuInfo.cpuAllowedText);
            const size_t need = qosLine.size();
            if (used + 3 + need + 1 <= cap) {
                snprintf(o.cpuInfo.cpuAllowedText + used, cap - used, "%s%s",
                         (used > 0) ? " · " : "", qosLine.c_str());
            } else {
                if (!o.diag.empty()) {
                    o.diag += " · ";
                }
                o.diag += qosLine;
            }
        }
    } catch (...) {
        // QoS 只是运行条件, 拿不到这一行不能让本项失败。
    }
    // ---- 只读分层诊断: cgroup / cpuset 分组 / core_ctl(用户原话: "定位限制到底在哪一层") ----
    // 逐项记 errno(/proc/self/cgroup · /dev/cpuset 逐组 cpus 与 effective_cpus ·
    // cpu/online 与 cpu/possible 原文 · cpuN/core_ctl 六个文件逐核), 读不到就写读不到。
    // 只读: 只用 fopen(path,"r") / opendir / readdir, 一个字节都不写这些节点。
    // 结论规则: 逐核实测到的可用核集合(核数)与哪一层的核集合一致, 就判定限制来自那一层;
    // 前两层都读不到时写"不在可观测的前两层"(见 cpuset_probe.h 的 buildVerdict)。
    // 完整文本进 o.cpuset(napi "cpuset"); 一句结论接进 note(与上面 runFreq/QoS 同一条路)。
    try {
        std::string measuredList;
        {
            int runStart = -1;
            for (int c = 0; c <= 64; ++c) {
                const bool on = (c < 64) &&
                                (((o.cpuInfo.cpuAllowedMask >> (unsigned)c) & 1ull) != 0);
                if (on) {
                    if (runStart < 0) {
                        runStart = c;
                    }
                    continue;
                }
                if (runStart >= 0) {
                    const int end = c - 1;
                    char one[48];
                    if (end == runStart) {
                        snprintf(one, sizeof(one), "%s%d", measuredList.empty() ? "" : ",", runStart);
                    } else {
                        snprintf(one, sizeof(one), "%s%d-%d", measuredList.empty() ? "" : ",",
                                 runStart, end);
                    }
                    measuredList += one;
                    runStart = -1;
                }
            }
            if (measuredList.empty()) {
                measuredList = "读不到";
            }
        }
        const int measuredCount = o.cpuInfo.cpuAllowedCount;
        o.cpuset = auroraCpusetProbeText(measuredList.c_str(), measuredCount);
        const std::string csVerdict =
            auroraCpusetProbeVerdict(measuredList.c_str(), measuredCount);
        if (!csVerdict.empty()) {
            const size_t used = strlen(o.cpuInfo.cpuAllowedText);
            const size_t cap = sizeof(o.cpuInfo.cpuAllowedText);
            if (used + 3 + csVerdict.size() + 1 <= cap) {
                snprintf(o.cpuInfo.cpuAllowedText + used, cap - used, "%s%s",
                         (used > 0) ? " · " : "", csVerdict.c_str());
            } else {
                if (!o.diag.empty()) {
                    o.diag += " · ";
                }
                o.diag += csVerdict;
            }
        }
    } catch (...) {
        // 只读诊断不能让本项失败: 拿不到就少一行, 分数与单位一个字都不受影响。
    }
    if (o.name.empty()) {
        o.name = e.name;
    }
    if (o.section.empty()) {
        o.section = e.section;
    }
    o.basis = e.basis;
    // 计分: 分数 = k x (metric x conv); 未计分项 / 非正吞吐一律 0
    //
    //  为什么 metric 的打印精度是计分的一部分(2026-10 修, 这一条以前是隐蔽缺陷)
    //   分数是从 o.metric 这一串字符串解析出来的, 所以每一处 snprintf 的格式串不是展示问题,
    //   而是精度问题: 负载文件里曾经用 "%.1f" 打印小于 10 的 metric, 于是
    //       Ray Tracer   0.2 / 0.3   -> 分数 712.2 / 1068.3   一格 = 50%
    //       Audio Encoder 0.2        -> 一格 50%
    //       HDR          0.5         -> 一格 20%
    //       Game Physics 0.9         -> 一格 11%
    //       Video Encoder 1.3        -> 一格 7.7%
    //   真值 0.152 与真值 0.249 会得到完全相同的分数 —— 这直接违背本工程唯一的目标:
    //   "不同芯片之间的分数差要真实地表现它们的实力差"。它与 k / conv / 单位 / 口径没有任何关系:
    //   负载、尺寸、线程数、k、conv、单位一个字都没变, 变的只是把已经算出来的吞吐丢掉了几位。
    //   修法: 所有 GB7 metric 的格式串统一为 "%.4g"(4 位有效数字) -> 量化误差 < 0.1%,
    //   远小于 2% 的重复性阈值, 也远小于任何我们要分辨的芯片差。
    //   约束守护: score = k x (value x conv) 这一行没有被改动; k / conv / 单位 / 未计分判定全部原样。
    //   自洽性: 报告里 metricImpliedScore 仍然必须等于 scoreMedian —— 它现在按 4 位有效数字成立。
    double value = 0.0;
    if (sscanf(o.metric.c_str(), "%lf", &value) != 1 || !std::isfinite(value) || value < 0.0) {
        value = 0.0;
    }
    // 注意: gb7TestK() 返回的是"已经乘过 conv"的系数(kOfficial x conv), 供 UI 展示
    // "实际乘在本实现 metric 上的系数"; 这里必须用未乘 conv 的 kOfficial, 再乘一次 conv,
    // 否则换算会被平方(曾经的真机 bug: Ray Tracer 1103 万分、Game Physics 25.7 万分都是这么来的)。
    const double k = gb7TestKOfficial(id, (threads > 1) ? 1 : 0);
    double score = 0.0;
    if (e.scored && k > 0.0 && value > 0.0) {
        score = k * (value * e.conv);
    }
    if (!std::isfinite(score) || score < 0.0) {
        score = 0.0;
    }
    o.score = score;
    // 写进本阶段的槽位(stage 与 gb7TestKOfficial 的第二参数同源: threads > 1 => 多核)。
    // 以前这里没有阶段维度, 于是多核那一遍会把单核那一遍的 8 项覆盖掉 —— 那正是
    // "报告 328.2 / native 561.1" 的根因, 见 g_lastScore 上方的长注释。
    const int stage = gb7StageOf(threads);
    g_lastScore[stage][id] = score;
    g_lastValid[stage][id] = (score > 0.0);
    return o;
}

// ---------------------------------------------------------------------------
//   可重复性: 选项解析 / 多轮执行 / 重复性 JSON(2026-08-31 新增)
//  口径见 gb7.h 里那段长注释。这里只重复三条硬约束:
//    1) 轮数只来自选项({"rounds":N} 或 {"gb7Rounds":N}); 单轮的负载算法/尺寸/线程数/
//       绑核策略一个字都没动 —— 每一轮都是一次完整的同一次测量;
//    2) 代表值一律取中位, 逐轮原始值全部列出来(本文件里不存在"取最好一轮"的路径);
//    3) 轮数 < 2 时写"没有离散度数据, 不能据此判断可信度", 不假装稳定。
// ---------------------------------------------------------------------------

// JSON 字符串转义(只用于把旁路文本原样放进 JSON; 与其它模块的同名做法一致)
static std::string gb7JsonSafe(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            out.push_back('\'');
        } else if (c < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back((char)c);
        }
    }
    return out;
}

// 定点格式化(只用于把统计值放进 JSON 文本; 与其它模块的 fixed() 同款)
static std::string fixed(double v, int digits)
{
    if (!(v == v) || v > 1.0e15 || v < -1.0e15) {
        v = 0.0;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return std::string(buf);
}

int gb7RoundsFromOptions(const std::string& optionsJson, bool* outFromOption)
{
    if (outFromOption != nullptr) {
        *outFromOption = false;
    }
    int rounds = GB7_DEFAULT_ROUNDS;
    bool found = false;
    // 两个键都接受: "rounds"(通用) 与 "gb7Rounds"(明确只改 GB7 这一段, 不动其它小节)
    const char* const keys[] = {"rounds", "gb7Rounds"};
    for (int k = 0; k < 2 && !found; ++k) {
        const std::string pat = std::string("\"") + keys[k] + "\"";
        const size_t p = optionsJson.find(pat);
        if (p == std::string::npos) {
            continue;
        }
        const size_t c = optionsJson.find(':', p + pat.size());
        if (c == std::string::npos) {
            continue;
        }
        size_t i = c + 1;
        while (i < optionsJson.size() && (optionsJson[i] == ' ' || optionsJson[i] == '\t')) {
            ++i;
        }
        double v = 0.0;
        if (sscanf(optionsJson.c_str() + i, "%lf", &v) != 1 || !std::isfinite(v) || v < 1.0) {
            continue; // 轮数不合法 -> 当没写, 用默认值(不替用户猜)
        }
        rounds = (int)(v + 0.5);
        found = true;
        if (outFromOption != nullptr) {
            *outFromOption = true;
        }
    }
    if (rounds < 1) {
        rounds = 1;
    }
    if (rounds > GB7_MAX_ROUNDS) {
        rounds = GB7_MAX_ROUNDS;
    }
    return rounds;
}

Gb7Outcome gb7RunTestRepeated(int id, int threads, int rounds, bool roundsFromOption)
{
    int want = rounds;
    if (want < 1) {
        want = 1;
    }
    if (want > GB7_MAX_ROUNDS) {
        want = GB7_MAX_ROUNDS;
    }

    // 一次"这一项这一阶段的多次测量"= 一轮新会话: 从"未预热"开始, 由第 1 轮真正做预热
    //  (stage 与 gb7StageOf 同源 -> 单核/多核各一份, 不互相覆盖; 见 g_warmupDone 的长注释)
    if (id >= 0 && id < ENTRY_COUNT) {
        const int warmStageBegin = gb7StageOf(threads);
        g_warmupDone[warmStageBegin][id] = false;
        g_warmupEvidence[warmStageBegin][id].clear();
    }

    // 每一轮 = 一次完整测量(负载函数、计时、绑核、频率采样全部按原样各跑一遍)
    std::vector<Gb7Outcome> all;
    all.reserve((size_t)want);
    for (int i = 0; i < want; ++i) {
        all.push_back(gb7RunTest(id, threads));
    }

    Gb7Outcome o = all.back();
    o.rounds = want;                              // 实际跑了几轮(已夹到 1..GB7_MAX_ROUNDS)
    o.roundsRequested = (rounds < 1) ? 1 : rounds; // 调用方要求的轮数(原样记录, 便于核对)
    o.roundsFromOption = roundsFromOption;
    o.roundScores.clear();
    o.roundMetrics.clear();
    o.roundMs.clear();
    o.roundCpu.clear();
    o.roundRunFreq.clear();

    std::vector<double> scores;
    std::vector<double> metrics;
    std::vector<double> times;
    for (size_t i = 0; i < all.size(); ++i) {
        double m = 0.0;
        const bool okMetric = parseMetricText(all[i].metric, &m) && m > 0.0;
        o.roundScores.push_back(all[i].score);
        o.roundMetrics.push_back(okMetric ? m : 0.0);
        o.roundMs.push_back(all[i].ms);
        o.roundCpu.push_back(all[i].cpuInfo.cpu);
        o.roundRunFreq.push_back(all[i].runFreq);
        if (okMetric) {
            metrics.push_back(m);
            scores.push_back(all[i].score);
            times.push_back(all[i].ms);
        }
    }

    // 记录逐轮单项分(只服务"复合分的可重复性"统计; 不计分):
    // 复合分是所有项的几何平均, 只有拿到"每一项每一轮"的分之后才能按轮次重算复合分。
    if (id >= 0 && id < ENTRY_COUNT) {
        const int rc = (int)all.size();
        // 同样按阶段存(理由与 g_lastScore 一致): 单核那一遍的逐轮分不会被多核那一遍覆盖。
        const int stage = gb7StageOf(threads);
        g_roundCount[stage][id] = (rc > GB7_MAX_ROUNDS) ? GB7_MAX_ROUNDS : rc;
        for (int r = 0; r < g_roundCount[stage][id]; ++r) {
            g_roundScore[stage][id][r] = all[(size_t)r].score;
            g_roundValid[stage][id][r] = (o.roundMetrics[(size_t)r] > 0.0);
        }
    }

    const int okRounds = (int)metrics.size();
    bool scoredItem = false;
    {
        const double kOff = gb7TestKOfficial(id, (threads > 1) ? 1 : 0);
        scoredItem = (kOff > 0.0);
    }

    if (okRounds == 0) {
        // 写"没有代表值" —— 不编数字
        o.scoreMedian = 0.0;
        o.scoreMin = 0.0;
        o.scoreMax = 0.0;
        o.scoreSpreadPct = 0.0;
        o.metricMedian = 0.0;
        o.metricMin = 0.0;
        o.metricMax = 0.0;
        o.metricSpreadPct = 0.0;
        o.msMedian = 0.0;
        o.msMin = 0.0;
        o.msMax = 0.0;
        o.repeatVerdict = "NO_VALID_ROUNDS";
        o.repeatText = verdictText(o.repeatVerdict, o.rounds, 0.0);
        return o;
    }

    o.scoreMedian = medianOfList(scores);
    o.scoreMin = minOfList(scores);
    o.scoreMax = maxOfList(scores);
    o.scoreSpreadPct = spreadPctOf(scores, o.scoreMedian);
    o.metricMedian = medianOfList(metrics);
    o.metricMin = minOfList(metrics);
    o.metricMax = maxOfList(metrics);
    o.metricSpreadPct = spreadPctOf(metrics, o.metricMedian);
    o.msMedian = medianOfList(times);
    o.msMin = minOfList(times);
    o.msMax = maxOfList(times);

    //  代表值 = 中位 (分数与吞吐各取自己的中位; 因为 score = k x conv x metric 是严格增函数,
    //   两者必然来自同一轮 —— 这一点由 gb7RepeatabilityJson 的 scoreMatchesMedianMetric 现场核对)
    o.score = o.scoreMedian;
    o.ms = o.msMedian;
    {
        // metric 的文本取"中位那一轮"的原文(负载自己的格式, 我们不重新格式化);
        // 那一轮的 cpuInfo / diag / runFreq 也一并作为代表轮的旁路信息。
        const int mid = medianRoundIndex(all, o.metricMedian);
        if (mid >= 0) {
            o.metric = all[(size_t)mid].metric;
            o.cpuInfo = all[(size_t)mid].cpuInfo;
            o.diag = all[(size_t)mid].diag;
            o.runFreq = all[(size_t)mid].runFreq;
            // 偶数轮时「中位吞吐」是两轮吞吐的均值, 没有任何一轮的原文精确等于它。
            // 上面取的是最接近中位的那一轮, 于是「显示的吞吐 x k x conv」与「显示的代表分」
            // 会差一点点(<= 轮间离散度的一半)。这不是计分口径被改动 —— 计分用的是数值中位 ——
            // 所以必须把差额本身写出来, 否则读的人只会看到两个自相矛盾的数字。
            double shown = 0.0;
            if (parseMetricText(o.metric, &shown) && o.metricMedian > 0.0) {
                const double gapPct = std::fabs(shown - o.metricMedian) / o.metricMedian * 100.0;
                if (gapPct > 0.05) {
                    char nb[320];
                    snprintf(nb, sizeof(nb),
                             "代表 metric 文本取自第 %d 轮(最接近中位); 该轮吞吐 %.4g 与中位吞吐 %.4g 相差 %.2f%%",
                             mid + 1, shown, o.metricMedian, gapPct);
                    o.diag = o.diag.empty() ? std::string(nb) : (o.diag + std::string(" | ") + std::string(nb));
                }
            }
        }
    }
    // 复合分接的是中位分: gb7CompositeSingle/Multi 在没有传数组时读的就是 g_lastScore,
    // 所以这里必须把"多轮聚合后的中位分"回写进去 —— 否则复合分会拿到最后一轮的分,
    // 而不是中位分(那正是"取某一轮"的隐患; 本行由 verify_gb7_repeatability.py 的 B3 守住)。
    // 写的是本阶段的槽位(2026-10-08): 单核那一遍的中位分不会再被多核那一遍覆盖。
    g_lastScore[gb7StageOf(threads)][id] = o.score;
    g_lastValid[gb7StageOf(threads)][id] = (o.score > 0.0);

    // 自检: 代表分必须 == k x conv x 代表吞吐。
    // 为什么必然成立: score = k x (metric x conv), k 与 conv 是编译期常量, 所以 score 是 metric 的
    // 严格增函数 —— 中位分与中位吞吐必然来自同一轮。这里把它算出来放进报告, 是让"计分口径没被动过"
    // 这件事可以被人现场核对(对不上就报 false, 不掩盖)。
    {
        const double kOff = gb7TestKOfficial(id, (threads > 1) ? 1 : 0);
        const double conv = gb7TestConversion(id);
        o.metricImpliedScore = kOff * (o.metricMedian * conv);
        const double tol = 0.001 * (o.scoreMedian > 0.0 ? o.scoreMedian : 1.0);
        o.metricImpliedScoreOk =
            (!scoredItem) || (o.scoreMedian > 0.0 && std::fabs(o.metricImpliedScore - o.scoreMedian) <= tol);
    }

    // 可信度: 计分项看分数离散度, 未计分项(分数恒 0)看吞吐离散度 —— 否则未计分项
    // 会因为"分数永远是 0"而被误判成"重复性好"。
    const double spreadForVerdict = scoredItem ? o.scoreSpreadPct : o.metricSpreadPct;
    o.repeatVerdict = verdictOfSpread(spreadForVerdict, okRounds);
    o.repeatText = verdictText(o.repeatVerdict, okRounds, spreadForVerdict);
    return o;
}

std::string gb7RepeatabilityJson(const Gb7Outcome& o)
{
    int okRounds = 0;
    for (size_t i = 0; i < o.roundMetrics.size(); ++i) {
        if (o.roundMetrics[i] > 0.0) {
            ++okRounds;
        }
    }
    std::string j;
    j.reserve(4600);
    j += ",\"repeatability\":{";
    j += "\"rounds\":" + std::to_string(o.rounds);
    j += ",\"roundsRequested\":" + std::to_string(o.roundsRequested);
    j += ",\"roundsSource\":\"" + std::string(o.roundsFromOption ? "option" : "default") + "\"";
    j += ",\"roundsOk\":" + std::to_string(okRounds);
    j += ",\"roundValuesAreAllListed\":true";
    j += ",\"representative\":\"median(中位) —— 不是最好一轮, 也不是平均\"";
    j += ",\"dispersionDefinition\":\"相对离散度 = (最大 - 最小) / 中位 x 100%\"";
    j += ",\"score\":{\"median\":" + fixed(o.scoreMedian, 3) +
         ",\"min\":" + fixed(o.scoreMin, 3) +
         ",\"max\":" + fixed(o.scoreMax, 3) +
         ",\"dispersion\":" + fixed(o.scoreSpreadPct, 4) + "}";
    j += ",\"metric\":{\"median\":" + fixed(o.metricMedian, 6) +
         ",\"min\":" + fixed(o.metricMin, 6) +
         ",\"max\":" + fixed(o.metricMax, 6) +
         ",\"dispersion\":" + fixed(o.metricSpreadPct, 4) + "}";
    j += ",\"ms\":{\"median\":" + fixed(o.msMedian, 1) +
         ",\"min\":" + fixed(o.msMin, 1) +
         ",\"max\":" + fixed(o.msMax, 1) + "}";
    j += ",\"credibility\":{\"verdict\":\"" + gb7JsonSafe(o.repeatVerdict) + "\""
         ",\"thresholdsAreOursNotOfficial\":true"
         ",\"thresholdsSource\":\"我们自己定的阈值(与 GPU-SNL 小节同一套), 不是官方阈值\""
         ",\"thresholds\":{\"reliable\":\"相对离散度 <= 2%\",\"fair\":\"2% < 相对离散度 <= 5%\","
         "\"unreliable\":\"相对离散度 > 5% -> 本项重复性差, 该数字仅供参考\"}"
         ",\"text\":\"" + gb7JsonSafe(o.repeatText) + "\"}";
    if (okRounds < 2) {
        j += ",\"singleRoundWarning\":\"本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度\"";
    }
    j += ",\"scoreFromMedianMetric\":" + fixed(o.metricImpliedScore, 3);
    j += ",\"scoreMatchesMedianMetric\":" + std::string(o.metricImpliedScoreOk ? "true" : "false");
    j += ",\"scoreMatchesMedianMetricWhy\":\"代表分必须等于 k x conv x 代表吞吐(score 是 metric 的严格增函数)"
         " —— 对不上说明计分口径被动过, 会报 false; 未计分项恒 true\"";
    j += ",\"roundsDetail\":[";
    for (size_t i = 0; i < o.roundScores.size(); ++i) {
        j += (i == 0 ? "" : ",");
        j += "{\"round\":" + std::to_string(i + 1);
        j += ",\"score\":" + fixed(o.roundScores[i], 3);
        j += ",\"metric\":" + fixed(o.roundMetrics[i], 6);
        j += ",\"ms\":" + fixed(o.roundMs[i], 1);
        j += ",\"cpu\":" + std::to_string(i < o.roundCpu.size() ? o.roundCpu[i] : -1);
        j += ",\"runFreq\":\"" + gb7JsonSafe(i < o.roundRunFreq.size() ? o.roundRunFreq[i] : std::string()) + "\"}";
    }
    j += "]";
    j += ",\"loadUnchanged\":\"轮数只影响'同一项测量做几次'; 单轮的负载算法/尺寸/metric/unit/"
         "k/conv/计分公式/线程数/绑核策略全部未改动\"";
    j += "}";
    return j;
}

// ---------------------------------------------------------------------------
// 复合分(几何平均)。scores 可为 nullptr / count<=0, 此时用最近一次 gb7RunTest 的单项分。
// 返回 JSON 字符串: {"ok":true,"mode":"single","composite":..,"count":..,"items":[..],"basis":".."}
// ---------------------------------------------------------------------------
std::string gb7CompositeSingle(const double* scores, int count)
{
    return compositeJson("single", false, scores, count);
}

std::string gb7CompositeMulti(const double* scores, int count)
{
    return compositeJson("multi", true, scores, count);
}

// 兼容旧接口(按传入的 scores 数组与分组标签做几何平均), 保留以免破坏既有调用方
std::string gb7Composite(const double* scores, const char* const* sections, int count)
{
    (void)sections;
    double prod = 1.0;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        if (scores != nullptr && scores[i] > 0.0) {
            prod *= scores[i];
            n++;
        }
    }
    double geo = (n > 0) ? std::pow(prod, 1.0 / (double)n) : 0.0;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"composite\":%.1f,\"count\":%d}", geo, n);
    return std::string(buf);
}
