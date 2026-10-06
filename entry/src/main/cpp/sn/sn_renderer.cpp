#include "sn_renderer.h"

// 只读地复用它里面的 CPU 拓扑 / 频率事实(用于结果里"尺子有没有被喂满"那一块)。
// 不用它做任何负载或计分决策 —— 本小节的负载与设备完全无关。
#include "cpu_affinity.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <setjmp.h>
#include <signal.h>

#include <atomic>
#include <cstddef>

// ===========================================================================
//  0. 进程内故障隔离舱(GPU-SNL 专用; 只包住本小节的 native 段)
// ---------------------------------------------------------------------------
//  为什么需要它(不是"防御性编程", 而是有真机现场):
//    2026-10-05 平板 PCE-W30 用 8.0 跑「一键全部跑分」, 在 GPU-SNL 小节里
//    signal=11 SIGSEGV si_code=1 SEGV_MAPERR si_addr=0x0, 崩在主线程 / JS 线程
//    (即 NAPI 同步调用线程)。结果是整轮跑分一份报告都没写出来 —— 一个
//    scored=false 的独立小节的故障, 被放大成了"整轮无结果"。
//
//  为什么是"信号 + siglongjmp"而不是 fork 出子进程:
//    ① 跑到 GPU-SNL 之前, 本进程已经由 libauroragpu.so / libauroragpu7.so
//       建立过 EGL 上下文与 GPU 驱动状态(见 CMakeLists.txt 里那两个模块的链接行);
//       EGL/GLES 不是 fork-safe 的: fork 出来的子进程继承的是"另一个线程建起来的、
//       可能正持有内部锁"的驱动状态, 在子进程里再建 EGL 上下文属于未定义行为 ——
//       最常见的表现是死锁(比段错误更难查, 而且同样是"整轮跑不完")。本小节的负载
//       本身就必须用 EGL/GLES, 所以"在 fork 之前不碰 GL"这条路在本进程里已经不存在了。
//    ② fork 只复制调用线程; 此刻进程里还有 ArkTS 运行时线程 / 崩溃取证看门狗线程 /
//       本小节自己的环境采样线程, 子进程里这些锁的主人都消失了。
//    ③ 子进程回不到 NAPI(不能构造 JS 字符串), 结果只能走管道 —— 而本小节的结果 JSON
//       是几十 KB 的文本, 管道协议要自己写"半包 / 超时 / 子进程僵死"三件事,
//       换来的隔离度对本轮的故障形态(空指针解引用)并不更高。
//    因此改用进程内隔离: 它精确覆盖"本小节自己的 native 代码犯错"这一类故障,
//    而失败路径仍然是原样的一条 JSON 字符串 —— 调用方(ArkTS)一行都不用改。
//
//  边界(必须一起说清楚, 不许夸大):
//    * 只接管 SIGSEGV / SIGBUS / SIGFPE / SIGILL 这 4 个"内存 / 算术类"致命信号;
//      不接管 SIGABRT / SIGSYS —— abort()/std::terminate/系统调用被策略拦截意味着
//      C++ 运行期状态已经不可信, 兜住它只会掩盖问题(崩溃取证里那两条路径原样保留);
//    * 只在进了隔离舱的那个线程上恢复(thread_local 标记)。本小节自己的环境采样
//      线程也在 envThreadMain 里进了隔离舱, 所以它同样只影响它自己;
//    * 隔离舱不修故障: 兜住一次之后本小节的 native 侧被永久标记为"已隔离",
//      后续调用直接返回带原因的失败文本 —— 一次空指针崩溃之后, GL 上下文与全局表
//      都不该再被相信;
//    * 没进隔离舱(或不是被接管的信号)的任何信号, 原封不动链式转发给崩溃取证
//      处理器(crash_guard), 真机取证行为一个字不变。
// ===========================================================================

namespace snfault {

// ---- 备用信号栈: 只在"当前线程还没有"的时候装一个; 崩溃取证那边主线程已经装过,
//      这里能检测到并跳过, 不覆盖别人的栈 ----
const int kAltStackBytes = 64 * 1024;
char g_altStackMem[kAltStackBytes];
std::atomic<int> g_altStackOwner{0};

struct SavedAction {
    int sig;
    struct sigaction act;
};
SavedAction g_prev[4];
int g_prevCount = 0;
std::once_flag g_installOnce;

// ---- 隔离舱状态: 全部是 thread_local / 无锁的, 因为要在信号处理器里读写 ----
thread_local sigjmp_buf tlsJmp;
thread_local volatile sig_atomic_t tlsArmed = 0;
thread_local int tlsDepth = 0;

volatile sig_atomic_t g_lastSig = 0;
volatile int g_lastCode = 0;
volatile unsigned long long g_lastAddr = 0;

std::atomic<bool> g_poisoned{false};
char g_poisonReason[512] = {0};

const char* signalName(int sig)
{
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        default: return "SIGNAL";
    }
}

// 把信号原样交回上一个处理器(真机上就是 libaurorabench 的崩溃取证 crash_guard)。
// 只有当上一个处理器是 SIG_DFL 时, 才恢复默认动作并重发 —— 语义与没装过隔离舱时一致。
void chainToPrevious(int sig, siginfo_t* info, void* uctx)
{
    for (int i = 0; i < g_prevCount; ++i) {
        if (g_prev[i].sig != sig) {
            continue;
        }
        const struct sigaction& a = g_prev[i].act;
        if ((a.sa_flags & SA_SIGINFO) != 0 && a.sa_sigaction != nullptr) {
            a.sa_sigaction(sig, info, uctx);
            return;
        }
        if (a.sa_handler == SIG_IGN) {
            return;
        }
        if (a.sa_handler != SIG_DFL && a.sa_handler != nullptr) {
            a.sa_handler(sig);
            return;
        }
        break;
    }
    struct sigaction d;
    memset(&d, 0, sizeof(d));
    d.sa_handler = SIG_DFL;
    sigemptyset(&d.sa_mask);
    sigaction(sig, &d, nullptr);
    raise(sig);
}

extern "C" void faultHandler(int sig, siginfo_t* info, void* uctx)
{
    // 信号处理器运行在出错的那个线程上, 所以 thread_local 标记正是我们要问的问题:
    // "当前这个线程进隔离舱了吗?"
    if (tlsArmed != 0) {
        tlsArmed = 0;
        g_lastSig = sig;
        g_lastCode = (info != nullptr) ? info->si_code : 0;
        g_lastAddr = (unsigned long long)(uintptr_t)((info != nullptr) ? info->si_addr : nullptr);
        siglongjmp(tlsJmp, 1);   // 永不返回
    }
    chainToPrevious(sig, info, uctx);
}

void installOnce()
{
    const int sigs[4] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
    for (int i = 0; i < 4; ++i) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = faultHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        struct sigaction prev;
        memset(&prev, 0, sizeof(prev));
        if (sigaction(sigs[i], &sa, &prev) == 0 && g_prevCount < 4) {
            g_prev[g_prevCount].sig = sigs[i];
            g_prev[g_prevCount].act = prev;
            ++g_prevCount;
        }
    }
    stack_t cur;
    memset(&cur, 0, sizeof(cur));
    if (sigaltstack(nullptr, &cur) == 0 && cur.ss_sp == nullptr) {
        if (g_altStackOwner.fetch_add(1) == 0) {
            stack_t ss;
            memset(&ss, 0, sizeof(ss));
            ss.ss_sp = g_altStackMem;
            ss.ss_size = (size_t)sizeof(g_altStackMem);
            ss.ss_flags = 0;
            (void)sigaltstack(&ss, nullptr);
        } else {
            g_altStackOwner.fetch_sub(1);
        }
    }
}

// RAII: 进入时接管信号(仅最外层那次), 退出时交还 armed 标记。
// 最外层那个 Guard 负责把 sigsetjmp 的目标装好(见 snPrepare / snRun)。
struct Guard {
    bool outer;
    Guard()
    {
        outer = (tlsDepth == 0);
        ++tlsDepth;
        if (outer) {
            std::call_once(g_installOnce, installOnce);
            tlsArmed = 1;
        }
    }
    ~Guard()
    {
        if (outer) {
            tlsArmed = 0;
        }
        if (tlsDepth > 0) {
            --tlsDepth;
        }
    }
    bool outermost() const { return outer; }
};

// siglongjmp 之后, 被跳过的那些栈帧的析构函数不会执行 —— 层次计数必须手动收回来,
// 否则下一次进隔离舱时会被当成"内层"而永远不再装 sigsetjmp 目标。
void resetAfterFault()
{
    tlsArmed = 0;
    tlsDepth = 1;   // 只留最外层那个 Guard 自己的一层, 由它的析构函数减回去
}

std::string describeLastFault()
{
    char b[512];
    snprintf(b, sizeof(b),
             "本小节 native 段发生致命信号(已被隔离舱兜住, 不是 App 崩溃): "
             "signal=%d %s si_code=%d si_addr=0x%llX。这一次的结果不可用; "
             "本小节的 native 侧已标记为「已隔离」, 后续不再执行任何 GL / 负载代码, "
             "其它小节不受影响。",
             (int)g_lastSig, signalName((int)g_lastSig), g_lastCode, g_lastAddr);
    return std::string(b);
}

void poison(const std::string& reason)
{
    if (!g_poisoned.exchange(true)) {
        snprintf(g_poisonReason, sizeof(g_poisonReason), "%s", reason.c_str());
    }
}

bool poisoned() { return g_poisoned.load(); }

std::string poisonedText()
{
    std::string s = "本进程内的 GPU-SNL 已被故障隔离舱标记为「已隔离」(上一次 native 段崩溃): ";
    s += (g_poisonReason[0] != '\0') ? g_poisonReason : "(原因未记录)";
    return s;
}

// ---- 隔离舱自检(只有显式传 faultSelfTest 才会走到) ----
// 故意读一个空指针: 产生的信号与真机现场(si_code=1 SEGV_MAPERR / si_addr=0x0)逐字段同形,
// 用来在真机上一行命令证明"这一段崩了, App 依然活着并拿到一条带原因的失败"。
volatile int* volatile g_nullProbe = nullptr;

void deliberateNullRead()
{
    (void)*g_nullProbe;
}

} // namespace snfault

// ============================================================================
//  Aurora Nomad Light (SNL) —— 实现
//  契约与设计理由全部写在 sn_renderer.h 的文件头, 这里只写"实现细节 + 为什么"。
// ============================================================================

namespace {

const char* kTag = "AuroraSn";

// ---------------------------------------------------------------------------
//  1. 与设备无关的编译期常量(整把尺子的"刻度")
//      这一节里不允许出现任何按设备 / SoC / 机型 / 年份 / 屏幕分辨率
//        取值的东西。改这里的任何一个数 = 改负载 = 必须递增 kSnBenchVersion。
// ---------------------------------------------------------------------------

// 固定渲染分辨率(与工程其它 GPU 小节一致的 1080p 口径; SNL 官方是 2560x1440,
// 差值只体现在 kScorePerFps 这个公开的换算常数里, 不改负载)。
const int kWidth = 1920;
const int kHeight = 1080;

// 一个实例(tile)= 32x32 像素 = 1024 个片元。顶点着色器在 GPU 侧按 gl_InstanceID
// 生成网格, CPU 不递交任何顶点数据。
const int kTilePx = 32;
const int kFragsPerTile = kTilePx * kTilePx;              // 1024
const int kTilesX = kWidth / kTilePx;                     // 60
const int kTilesY = kHeight / kTilePx;                    // 33.75 -> 见下
// 1080 / 32 = 33.75 不是整数。为了避免最后一行留缝(**留缝会让部分像素不参与着色,
// 破坏"每单位工作量恒定"**), 网格取上取整, 多出来的那一行/列由顶点着色器夹到
// 视口内(位置 clamp), 于是每个实例仍然固定覆盖 1024 个片元: 被夹住的实例会与
// 邻居重叠, 重叠的像素照样跑完整片元着色器(不用 discard, 不做 early-z 剔除)。
const int kTilesYCeil = (kHeight + kTilePx - 1) / kTilePx; // 34
const int kTilesPerFullScreen = kTilesX * kTilesYCeil;     // 2040
// ↑ 注意: 这是"一趟满屏"所需的实例数上限; 实际实例数由 uGridW/uGridH 决定,
//   满屏时 = 2040 个实例 x 1024 片元 >= 1920x1080(多出的一行是重叠, 不是浪费)。

// 每像素每趟的着色工作量(编译期常量, 不随 pass 数 / 设备变化):
//   趟 A: 主光线 -> 对程序化高度场做固定 8 步光线步进 -> 命中/天空 -> 2 份 3 octave
//         细节噪声(高度/法线/材质) -> G-buffer 编码(位置/法线/粗糙度/线性深度)
//   趟 B: 读回 G-buffer -> 太阳直射 + 阴影项 + 间接光 + 3 盏点光源(GGX) + 天空环境
//         + ACES 色调映射 + 暗角 + 颗粒
//
// 口径(必须先说清, 否则这个数没有意义): 
//   "op" = 一次着色操作 = 内置函数调用 + 一个算术运算符 + 一次多分量 swizzle 读取;
//   固定次数的 for 循环按次数完全展开后计数(所以 8 步光线步进是按 8 次展开算的),
//   自定义函数按调用点内联后计数。它不是"ALU 周期数", 也不是"指令数"。
//   这个口径由 verify_sn_unified_ruler.py 的 [D] 段用同一套规则离线复算,
//   两边必须一致(±25%)—— 它只是"重着色"的量化说明, 不计分:
//   分数只用工作量(像素数)与时间。
// 下面两个数由 verify_sn_unified_ruler.py [D] 段用同一套规则离线复算得到, 两边必须一致。
// 它们随负载定义一起被冻结: 改它就是改负载 = 必须递增 kSnBenchVersion。
const int kOpsPerPixelPassA = 176;
const int kOpsPerPixelPassB = 286;
const int kOpsPerPixelPerPass = kOpsPerPixelPassA + kOpsPerPixelPassB;

// 每像素每趟的内存流量(报告用, 让未来一眼看出尺子有没有被带宽卡住):
//   趟 A 写 2 x RGBA8 = 8 B/px; 趟 B 读 2 x RGBA8 + 写 1 x RGBA8 = 12 B/px
const int kBytesPerPixelPerPass = 20;

// 读回像素数(自证用): 只读 32x32 一小块, 不是全屏回读
const int kReadbackPx = 32;

// 每帧每趟的 draw call 数(常量): 趟 A 一次 instanced 绘制, 趟 B 一次 instanced 绘制
const int kDrawCallsPerPass = 2;
// CPU 侧每趟的 GL 调用数(常量, 见 submitPass(): bind/clear/uniform x N/draw)
const int kApiCallsPerPass = 22;
// 每帧固定的 GL 调用数(计时用的 glFinish + 读回/清屏)
const int kApiCallsPerFrameFixed = 6;

// 趟数阶梯(自适应只允许在这个范围内取 2 的幂):
//   1 = 最慢的设备(9000S 级)也一定跑得完; 4096 = 给未来留的余量。
//   每一档都做完全相同的一趟工作, 所以"改档位"不改每单位工作量,
//   只改"这一帧做了几份"—— 分数由吞吐量算出, 与档位无关。
const int kPassLadderMin = 1;
const int kPassLadderMax = 4096;

// 计时协议
const int kWarmupFrames = 2;
const int kDefaultMeasureFrames = 8;
const int kMaxMeasureFrames = 16;
// 多轮重复(验收标准第 1 条: 可重复性必须被量化)。默认 3 轮 —— 少于 3 轮时
// "离散度"本身就不可信(报告里会明确提示这一点)。
const int kDefaultRepeats = 3;
const int kMaxRepeats = 9;
const int kDefaultGapMs = 1500;         // 轮间冷却间隔; 0 = 不等待
const int kMaxGapMs = 30000;
const double kDefaultTargetMs = 12.0;   // 每帧目标毫秒(自适应依据)
const double kMinAcceptMs = 4.0;        // 低于这个值认为"太快了, 数字不可信"
const double kDefaultMaxPasses = 64.0;  // 默认趟数上限

// ---------------------------------------------------------------------------
//  2. 计分常数(公开、可核对、可标定)
//      UL 官方(支持文章 44002528075): Steel Nomad Light 总分 = 图形分
//                                    = 图形测试平均帧率 x 135
//      本小节: score = fps x kScorePerFps,
//              kScorePerFps = 135 x kWorkloadScale x (1920x1080)/(2560x1440)
//                           = 135 x 1.0 x 0.5625 = 75.9375
//      ( 是乘 0.5625 不是除: 除会得到 240, 等于把分辨率差算两遍, 与公开值对不上)
//      理由与假设见 sn_renderer.h 文件头"分数口径"一节, 这里只落地数字。
// ---------------------------------------------------------------------------
const double k3dmarkNomadScale = 135.0;                       // 官方明文
const double kSnlOfficialWidth = 2560.0;                      // UL 官方: SNL 渲染分辨率
const double kSnlOfficialHeight = 1440.0;
const double kResolutionScaleRatio =
    ((double)kWidth * (double)kHeight) / (kSnlOfficialWidth * kSnlOfficialHeight); // 0.5625
// 分辨率归一化(一次推导, 可逐步核对):
//   同一块 GPU 上"每像素速度相同"的两个负载, 像素数是 r 倍的那个帧率就是 1/r 倍。
//   SNL 在 2560x1440 下的帧率记作 fps_snl; 本小节在 1920x1080 下的帧率记作
//   fps_aurora = fps_snl x (1/r),  r = (1920*1080)/(2560*1440) = 0.5625。
//   3DMark 侧:  score_snl    = fps_snl x 135
//   本小节侧:  score_aurora = fps_aurora x kScorePerFps
//   两式代入 fps_aurora = fps_snl / r, 得到"两者相等"的条件:
//       fps_snl x 135 = (fps_snl / r) x kScorePerFps   =>   kScorePerFps = 135 x r = 75.9375
//   即 **kScorePerFps = 135 x 0.5625 = 75.9375**(不是除 —— 除以 r 会得到 240,
//   那等于把分辨率差算了两遍, 分数会凭空大 3.16 倍, 与公开值对不上)。
// kWorkloadScale 是唯一的自由参数: 它等于"本小节负载每像素速度 / SNL 每像素速度"。
// 上面推的是它在"两个负载同速"时应当取 1.0; 标定之后按实测改它(见 sn_renderer.h 的
// "分数口径"一节)。 无论它取多少, 同一台设备两次运行之间的比值都不受影响 ,
// 所以"跨代可比"这一条不依赖标定。
const double kWorkloadScale = 1.0;
const double kScorePerFps = k3dmarkNomadScale * kWorkloadScale * kResolutionScaleRatio;

// 公开参照值(3DMark Steel Nomad Light, 整机成绩)
//  查证状态逐条不同, 必须一起报出去(2026-10 离线核对 UL 官方数据库 / 官方支持文章 /
//   Notebookcheck / nanoreview 之后的结果; 2026-08-31 又收到用户的一手证据):
//     K 9020  = 454  已证实: UL 官方成绩库里的机型行 "Huawei Pura 80 Pro+ / Ultra,
//                    Kirin 9020, Maleoon 920 -> 454"(用户提交结果的中位数), nanoreview 同为 454。
//                    同一 SoC 在不同机型上实测区间 368..557, 所以 454 是"某台机器"而不是"SoC 上限"。
//     K 9030 Pro = 991 用户拍屏的一手证据(2026-08-31 截图): 3DMark 应用在 HUAWEI Mate 80 Pro Max
//                    上跑 Steel Nomad Light, 总分 991 / 平均帧率 7.34 FPS。自洽核对: 991 / 135 =
//                    7.3407 FPS ≈ 拍屏上的 7.34 FPS(官方明文 总分 = 平均帧率 x 135)。
//                    保留的两条 caveat: ① 公开渠道(UL 官方成绩库)查不到该机型条目;
//                    ② 与其他 9030 Pro 机型读数 956(MatePad Pro Max)/ 998(Mate X7)/ 993(转载页)
//                    不完全一致(区间 950~1000)。所以它证明的是"那台机器上跑出过 991", 不是 SoC 上限。
//     K 9000S = 303  未证实: 只有 nanoreview 一处; UL 成绩库里根本没有 Mate 60 系列条目,
//                    Notebookcheck 的 Maleoon 910 页给的 SNL 是空白/0。
//   结论: 这三条只用来说明"数量级与代际比例", 不要当成标定锚点;
//         真正能当锚点的是 454(唯一有 UL 官方数据库出处的那个)。
struct RefPoint {
    const char* soc;
    const char* device;
    double officialScore;
    // 来源强度(逐条不同; 强度顺序: 一手观测 > 官方成绩库 > 第三方):
    //   VERIFIED_UL_DB                     = UL 官方成绩库出处(已证实)
    //   USER_PROVIDED_PRIMARY_OBSERVATION  = 用户拍屏的一手证据(直接观测, 2026-08-31 起用于 991)
    //   UNVERIFIED                         = 只有第三方来源, 未证实
    const char* verification;
    const char* note;
};
const RefPoint kRefPoints[] = {
    //  991 的来源已修正(2026-08-31): 从 UNVERIFIED 改为 USER_PROVIDED_PRIMARY_OBSERVATION。
    //   依据是用户提供的拍屏照片(一手证据): 机型 Mate 80 Pro Max / 测试项 Steel Nomad Light /
    //   总分 991 / 平均帧率 7.34 FPS / 来源=用户拍屏。自洽核对: 991 / 135 = 7.3407 ≈ 7.34 FPS。
    //   原来那两条 caveat 一个字都没有删(见下面的 note): 公开渠道查不到该机型条目, 且与
    //   956 / 998 / 993 等其他 9030 Pro 读数不完全一致。
    //   关于"极客湾『比 9020 快 76%』(约 799)": 那是一个第三方推算, 与本次一手观测冲突时,
    //   以一手观测为准(但两者都列出来, 不替用户选一个"好看"的)。
    {"Kirin 9030 Pro", "HUAWEI Mate 80 Pro Max", 991.0, "USER_PROVIDED_PRIMARY_OBSERVATION",
     "机型 Mate 80 Pro Max / 测试项 Steel Nomad Light / 总分 991 / 平均帧率 7.34 FPS / 来源=用户拍屏"
     "(一手证据, 2026-08-31 截图), 自洽核对 991/135 = 7.3407 FPS ≈ 7.34 FPS。"
     "caveat(保留): 公开渠道(UL 官方成绩库)查不到该机型条目; 并且与其他 9030 Pro 机型读数 "
     "956(MatePad Pro Max)/ 998(Mate X7)/ 993(转载页)不完全一致(区间 950~1000); "
     "另外极客湾/ITHome 曾推算 9030 Pro 相对 9020 只快 76%(约 799), 与这条一手读数冲突 —— "
     "冲突记录, 不替用户取舍"},
    {"Kirin 9020", "HUAWEI Pura 80 Pro+ / Ultra", 454.0, "VERIFIED_UL_DB",
     "UL 官方成绩库机型行(用户提交结果中位数)+ nanoreview 一致; 同 SoC 机型区间 368..557 —— "
     "这一条是三个里唯一可当标定锚点的"},
    {"Kirin 9000S", "HUAWEI Mate 60 系列 / MatePad Pro 13.2", 303.0, "UNVERIFIED",
     "只有 nanoreview 一处; UL 成绩库没有 Mate 60 系列条目, Notebookcheck 的 Maleoon 910 为空白"},
};
const int kRefPointCount = (int)(sizeof(kRefPoints) / sizeof(kRefPoints[0]));

// benchmark 级版本号: 见 header。负载/口径一变就必须 +1。
const char* kFormatRevision = "snl-1";

// ---------------------------------------------------------------------------
//  3. 通用工具(与 gpu7 同款, 保持两个模块的输出风格一致)
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

const size_t kMaxErrorBytes = 6000;

//  2026-10-05 加固(不是新功能, 是把"交出一段解析不了的 JSON"这条静默失败路径堵死)
//   真机现场: 手机 Pura X Max 上 GPU-SNL 小节报「native 返回失败：（native 未给文本）」。
//   那不代表 native 没给文本, 而是交出去的 JSON 没能被 JSON.parse 解析, 调用方
//   (BenchRunner.parseObj)只能兜成一条没有原因的失败。JSON 解析不了的两个来源是
//   ① 未转义的控制字节(下面已经处理) ② 非法 UTF-8 字节序列(下面新处理)。
//   外部来源的字符串(GPU 驱动标识 / sysfs 的热区名 / governor / 拓扑文本)都可能带
//   非 UTF-8 字节; 以前它们被原样拼进 JSON。这里改成: 合法序列原样保留, 非法序列
//   换成 '?' —— 既不改任何数字与单位, 也保证交出去的每一段 JSON 都是合法 UTF-8。

// 返回 s[i] 起始的合法 UTF-8 序列长度(1..4); 非法返回 0。
size_t utf8SeqLen(const std::string& s, size_t i)
{
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        return 1;
    }
    size_t need = 0;
    unsigned int cp = 0;
    if ((c & 0xE0) == 0xC0) {
        need = 1;
        cp = (unsigned int)(c & 0x1Fu);
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        cp = (unsigned int)(c & 0x0Fu);
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        cp = (unsigned int)(c & 0x07u);
    } else {
        return 0;
    }
    if (i + need >= s.size()) {
        return 0;
    }
    for (size_t k = 1; k <= need; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (unsigned int)(cc & 0x3Fu);
    }
    if ((need == 1 && cp < 0x80u) || (need == 2 && cp < 0x800u) ||
        (need == 3 && cp < 0x10000u) || cp > 0x10FFFFu ||
        (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return 0;   // 过长编码 / 代理区 / 超出 U+10FFFF
    }
    return need + 1;
}

bool utf8Ok(const std::string& s, size_t* badAt)
{
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8SeqLen(s, i);
        if (n == 0) {
            if (badAt != nullptr) {
                *badAt = i;
            }
            return false;
        }
        i += n;
    }
    return true;
}

std::string jsonSafe(const std::string& s, size_t cap = 0)
{
    const size_t limit = (cap == 0 || cap > s.size()) ? s.size() : cap;
    std::string out;
    out.reserve(limit + 16);
    for (size_t i = 0; i < limit; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            out.push_back('\'');
        } else if (c == '\n' || c == '\r' || c == '\t') {
            out.push_back(' ');
        } else if (c < 0x20) {
            out.push_back(' ');
        } else if (c < 0x80) {
            out.push_back((char)c);
        } else {
            const size_t n = utf8SeqLen(s, i);
            if (n == 0 || i + n > limit) {
                out.push_back('?');   // 非法 UTF-8: 换成 '?', 不把坏字节交给 JSON.parse
            } else {
                out.append(s, i, n);
                i += n - 1;
            }
        }
    }
    if (limit < s.size()) {
        out += "...(truncated)";
    }
    return out;
}

// 任何"可能未初始化 / 来自外部"的 const char* 在进 std::string() / strcmp() /
// snprintf("%s") 之前统一走这里。 2026-10-05 native 崩溃的直接成因 :
// 一条未初始化的 const char*(SnMeasurement::env.text)被直接交给 std::string(),
// 于是走 libc 的 strlen 去读 0x0 -> SIGSEGV / SEGV_MAPERR / si_addr=0x0。
const char* cstrOr(const char* s, const char* fallback)
{
    return (s != nullptr) ? s : fallback;
}

// 定长字符数组 -> std::string: 只读到结束符或容量上限, 不假设一定存在结束符。
// (不用 strnlen(): 它是 POSIX 扩展, 不同 libc 的 <cstring> 是否把它带进 std:: 并不一致,
//  自己写 5 行更稳, 而且这样"最多读 cap 字节"这件事在代码里是显式的。)
std::string boundedCStr(const char* p, size_t cap)
{
    if (p == nullptr || cap == 0) {
        return std::string();
    }
    size_t n = 0;
    while (n < cap && p[n] != '\0') {
        ++n;
    }
    return std::string(p, n);
}

std::string g_snError;

void appendSnError(const std::string& msg)
{
    if (msg.empty()) {
        return;
    }
    if (g_snError.empty()) {
        g_snError = msg;
        return;
    }
    if (g_snError.find(msg) != std::string::npos) {
        return;
    }
    const std::string sep = " | ";
    if (g_snError.size() + sep.size() >= kMaxErrorBytes) {
        return;
    }
    g_snError += sep + msg;
}

//  失败文本的兜底(契约 E11): 不允许交出一条 error 与 lastError 都为空的失败结果。
//   真机现场(2026-10-05 手机 Pura X Max): 界面显示「native 返回失败：（native 未给文本）」。
//   从那以后本文件的规矩是: 每一个失败出口都必须能读出"在哪一步失败 + 具体原因"。
const char* kNoTextFallback =
    "（native 未给文本: 该失败路径没有填写原因。这是本工程不允许的状态 —— "
    "所有失败出口都必须用 failJsonStep(步骤名, 原因) 或 failJson(非空文本)）";

std::string failJson(const std::string& msg)
{
    const std::string text = msg.empty() ? std::string(kNoTextFallback) : msg;
    appendSnError(text);
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "sn fail: %{public}s", text.c_str());
    return "{\"ok\":false,\"error\":\"" + jsonSafe(text, kMaxErrorBytes) +
           "\",\"lastError\":\"" + jsonSafe(g_snError, kMaxErrorBytes) + "\"}";
}

// 所有失败出口统一走这里: step = 在哪一步失败(必须非空), detail = 原始原因(可以为空)。
// 例: failJsonStep("probe frame", probe.error)
std::string failJsonStep(const char* step, const std::string& detail)
{
    std::string msg = "GPU-SNL 执行到 [";
    msg += (step != nullptr && step[0] != '\0') ? step : "unknown-step";
    msg += "] 失败: ";
    msg += detail.empty() ? std::string("(该步骤没有给出更细的原因)") : detail;
    return failJson(msg);
}

// ===========================================================================
//  JSON 自检(交付给 ArkTS 之前的最后一道闸)
// ---------------------------------------------------------------------------
//  第一版(2026-10-05 早)只用"数括号"来判 JSON 好不好。真机 8.1 的现场说明这个判据
//  本身有缺陷:
//    报告把「JSON 真的坏了」和「自检自己数错了」当成同一件事写出来, 于是
//    「括号不平衡(depth=2)」既可能是 native 交出去的 JSON 真的坏了, 也可能是自检
//    在字符串/转义上数错了。排查的人无法从这句话判断该去修哪一边。
//
//  现在的规矩(硬性):
//     判决权在真解析器手里, 不在计数器手里。
//      - 这里手写一个完整的递归下降 JSON 解析器(对象 / 数组 / 字符串 / 数字 /
//        true / false / null; 字符串里正确处理转义与 \uXXXX; 拒绝裸控制字符与非法 UTF-8),
//        它按同一份文法把整段文本走一遍。它通过 => JSON 合法, 可以交付。
//      - "数括号"扫描降级为诊断信息: 它只用来在解析器报错时补一句"括号计数也认为
//        不平衡, depth=N"作为旁证, 以及反过来在"解析器通过但计数器不通过"时
//        明确指出这是自检误报。
//     两种结论必须分开写, 不许互相冒充 
//      - 解析器拒绝 => "JSON 确实坏了: 第 N 字节, 原因…"
//      - 解析器通过、计数器不通过 => ok=true(照常交付), 另附一条
//        "自检误报"警告(带字节偏移), 不写成"JSON 自检失败"。
//
//  为什么要有这个函数: 调用方(BenchRunner.parseObj)在 JSON.parse 抛异常时会兜成一条
//  「ok=false + error 为空 + lastError 为空」的结果 —— 于是界面上只有
//  「native 返回失败：（native 未给文本）」, 故障原因彻底丢失。
// ===========================================================================

// 括号计数扫描的结果(仅诊断用; 判决权在下面的解析器手里)。
struct SnBraceScan {
    int depth;          // 结束时(对象/数组)净深度; 0 = 平衡
    bool inStr;         // 结束时是否还停在字符串里
    bool sawNul;        // 是否遇到 NUL 字节
    bool ctrlInStr;     // 字符串里出现未转义控制字符
    size_t firstBadAt;  // 上面任何一种异常第一次出现的位置
};

//  关键实现细节: 计数必须跳过字符串字面量与转义 
//   否则一段正常文本里的花括号(说明文字 / 路径 / 诊断串)就会被当成结构括号 ——
//   这正是"自检误报"的经典成因。verify_sn_json_balance.py 里有一组反例专门钉这一条。
SnBraceScan snBraceScan(const std::string& s)
{
    SnBraceScan r;
    r.depth = 0;
    r.inStr = false;
    r.sawNul = false;
    r.ctrlInStr = false;
    r.firstBadAt = std::string::npos;
    bool esc = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0) {
            r.sawNul = true;
            if (r.firstBadAt == std::string::npos) {
                r.firstBadAt = i;
            }
            return r;
        }
        if (r.inStr) {
            if (esc) {
                esc = false;
                continue;
            }
            if (c == '\\') {
                esc = true;
                continue;
            }
            if (c == '"') {
                r.inStr = false;
                continue;
            }
            if (c < 0x20) {
                r.ctrlInStr = true;
                if (r.firstBadAt == std::string::npos) {
                    r.firstBadAt = i;
                }
                return r;
            }
            continue;
        }
        if (c == '"') {
            r.inStr = true;
        } else if (c == '{' || c == '[') {
            ++r.depth;
        } else if (c == '}' || c == ']') {
            --r.depth;
            if (r.depth < 0) {
                if (r.firstBadAt == std::string::npos) {
                    r.firstBadAt = i;
                }
                return r;
            }
        }
    }
    return r;
}

// ---- 严格 JSON 解析器(手写, 只认我们真正会写出去的那套文法) ----
// 与 Python 的 json.loads() / JSON.parse() 对同一批文法的接受集一致:
//   object / array / string(含 \uXXXX) / number(无前导 0、无 NaN/Inf) / true / false / null
// 额外拒绝: 裸控制字符、非法 UTF-8、代理区码点(\(\uD800-\uDFFF\) 用 UTF-8 直接编码出来的那种)、
//           尾随内容、NUL 字节。
struct SnJsonParser {
    const std::string& s;
    size_t i;
    size_t errAt;
    std::string err;
    int depth;

    explicit SnJsonParser(const std::string& text) : s(text), i(0), errAt(std::string::npos), depth(0) {}

    bool fail(const char* msg, size_t at)
    {
        if (err.empty()) {
            err = msg;
            errAt = at;
        }
        return false;
    }

    void ws()
    {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
                continue;
            }
            break;
        }
    }

    bool literal(const char* lit)
    {
        const size_t n = std::strlen(lit);
        if (s.compare(i, n, lit) != 0) {
            return fail("期望 true/false/null", i);
        }
        i += n;
        return true;
    }

    bool hex4(unsigned* out)
    {
        if (i + 4 > s.size()) {
            return fail("\\u 后面不足 4 个十六进制位", i);
        }
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + (size_t)k];
            unsigned d = 0;
            if (c >= '0' && c <= '9') {
                d = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = (unsigned)(c - 'A' + 10);
            } else {
                return fail("\\u 后面不是十六进制数字", i + (size_t)k);
            }
            v = (v << 4) | d;
        }
        i += 4;
        *out = v;
        return true;
    }

    bool str()
    {
        if (i >= s.size() || s[i] != '"') {
            return fail("期望字符串开头的引号", i);
        }
        ++i;
        for (;;) {
            if (i >= s.size()) {
                return fail("字符串没有闭合", i);
            }
            const unsigned char c = (unsigned char)s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c < 0x20) {
                return fail("字符串里有未转义的控制字符", i);
            }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) {
                    return fail("转义符后面没有字符", i);
                }
                const char e = s[i];
                if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' ||
                    e == 'r' || e == 't') {
                    ++i;
                    continue;
                }
                if (e == 'u') {
                    ++i;
                    //  只认文法: \u 后面必须是 4 个十六进制位 
                    //   第一版在这里额外做了"代理对配对"(高位后紧跟低位就一起吃掉)。
                    //   那条路有一个死循环的风险: 配对失败时把下标退回 '\', 下一轮又会
                    //   走到同一个分支、以同一个状态失败, 永远出不来。本工程交出去的 JSON
                    //   里根本不会出现 \uXXXX(jsonSafe 把 '\' 换成了 '''), 但"输入里有
                    //   一条能卡死的路径"本身就是缺陷 —— 这里直接删掉配对, 只按文法收下
                    //   4 个十六进制位; 孤立代理项按 JSON 文法本来就是合法的(Python json /
                    //   JSON.parse 都接受), 不需要我们额外判。
                    unsigned cp = 0;
                    if (!hex4(&cp)) {
                        return false;
                    }
                    continue;
                }
                return fail("非法转义序列", i);
            }
            if (c < 0x80) {
                ++i;
                continue;
            }
            // 多字节: 必须是合法 UTF-8(顺带拒绝代理区码点与过长编码)
            const size_t n = utf8SeqLen(s, i);
            if (n == 0) {
                return fail("字符串里有非法 UTF-8 字节序列", i);
            }
            i += n;
        }
    }

    bool number()
    {
        const size_t start = i;
        if (i < s.size() && s[i] == '-') {
            ++i;
        }
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            return fail("数字缺少整数部分", start);
        }
        if (s[i] == '0') {
            ++i;
        } else {
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                ++i;
            }
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') {
                return fail("小数点后面没有数字", i);
            }
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                ++i;
            }
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                ++i;
            }
            if (i >= s.size() || s[i] < '0' || s[i] > '9') {
                return fail("指数部分没有数字", i);
            }
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                ++i;
            }
        }
        return true;
    }

    bool value()
    {
        if (depth > 64) {
            return fail("嵌套深度超过 64(不可能是本小节写出来的结构)", i);
        }
        if (i >= s.size()) {
            return fail("文本在值的位置就结束了", i);
        }
        const char c = s[i];
        if (c == '{') {
            return object();
        }
        if (c == '[') {
            return array();
        }
        if (c == '"') {
            return str();
        }
        if (c == 't') {
            return literal("true");
        }
        if (c == 'f') {
            return literal("false");
        }
        if (c == 'n') {
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return number();
        }
        return fail("不是合法的 JSON 值", i);
    }

    bool object()
    {
        ++depth;
        ++i;   // '{'
        ws();
        if (i < s.size() && s[i] == '}') {
            ++i;
            --depth;
            return true;
        }
        for (;;) {
            ws();
            if (!str()) {
                return false;
            }
            ws();
            if (i >= s.size() || s[i] != ':') {
                return fail("对象成员的键后面缺少 ':'", i);
            }
            ++i;
            ws();
            if (!value()) {
                return false;
            }
            ws();
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                ++i;
                --depth;
                return true;
            }
            return fail("对象里期望 ',' 或 '}'", i);
        }
    }

    bool array()
    {
        ++depth;
        ++i;   // '['
        ws();
        if (i < s.size() && s[i] == ']') {
            ++i;
            --depth;
            return true;
        }
        for (;;) {
            ws();
            if (!value()) {
                return false;
            }
            ws();
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                ++i;
                --depth;
                return true;
            }
            return fail("数组里期望 ',' 或 ']'", i);
        }
    }

    bool parseTop()
    {
        ws();
        if (i < s.size() && s[i] != '{') {
            return fail("顶层不是对象(本小节的契约是必须是一个 JSON 对象)", i);
        }
        if (!value()) {
            return false;
        }
        ws();
        if (i != s.size()) {
            return fail("JSON 结束后还有多余内容", i);
        }
        return true;
    }
};

// 自检结论: 三种, 必须分开报。
struct SnJsonCheckResult {
    bool ok;                  // true = 真解析器通过, 这段文本可以交给 ArkTS
    bool selfCheckDisagreed;  // true = 解析器通过但括号计数不通过 -> 自检本体误报(不是 JSON 坏了)
    std::string why;          // 人话(带字节偏移)
    size_t badAt;             // 第一处出问题的字节; 没有则 std::string::npos
    int braceDepth;           // 括号计数扫描的最终深度(仅诊断)
};

SnJsonCheckResult snJsonSelfCheck(const std::string& s)
{
    SnJsonCheckResult r;
    r.ok = false;
    r.selfCheckDisagreed = false;
    r.badAt = std::string::npos;
    r.braceDepth = 0;

    const SnBraceScan scan = snBraceScan(s);
    r.braceDepth = scan.depth;

    // ---- ① 判决权: 真解析器 ----
    if (s.size() < 2) {
        r.badAt = 0;
        r.why = "JSON 确实坏了: 结果文本长度 < 2 字节(严格解析器: 不可能是一个 JSON 对象)";
        return r;
    }
    SnJsonParser p(s);
    const bool parsed = p.parseTop();
    if (!parsed) {
        r.badAt = (p.errAt == std::string::npos) ? 0 : p.errAt;
        r.why = "JSON 确实坏了: 严格解析器在第 " + std::to_string(r.badAt) + " 个字节处拒绝: " + p.err;
        // 括号计数只作为旁证: 它认为平衡也不能推翻解析器的结论。
        if (scan.depth != 0 || scan.inStr) {
            r.why += "(括号计数扫描一致: depth=" + std::to_string(scan.depth) +
                     (scan.inStr ? ", 且有字符串未闭合" : "") + ")";
        } else {
            r.why += "(注意: 括号计数扫描认为括号是平衡的 —— 说明坏的不是括号数, 而是文法/编码; "
                     "这正是不能只靠数括号的原因)";
        }
        return r;
    }

    // ---- ② 解析器通过了: JSON 是合法的, 直接交付 ----
    r.ok = true;

    // ---- ③ 交叉核对: 解析器 OK 而计数器不 OK => 这是自检误报, 必须这么说 ----
    if (scan.depth != 0 || scan.inStr || scan.sawNul || scan.ctrlInStr) {
        r.selfCheckDisagreed = true;
        r.badAt = scan.firstBadAt;
        r.why = "自检误报(不是 JSON 坏了): 严格解析器认为这段 JSON 完全合法并且已经交付, "
                "但括号计数扫描给出了相反结论(";
        if (scan.depth != 0) {
            r.why += "depth=" + std::to_string(scan.depth);
        }
        if (scan.inStr) {
            r.why += (scan.depth != 0 ? ", " : "") + std::string("有字符串未闭合");
        }
        if (scan.sawNul) {
            r.why += (scan.depth != 0 || scan.inStr ? ", " : "") + std::string("遇到 NUL 字节");
        }
        if (scan.ctrlInStr) {
            r.why += (scan.depth != 0 || scan.inStr ? ", " : "") + std::string("字符串里有裸控制字符");
        }
        r.why += "), 第一处第 " + std::to_string(r.badAt) +
                 " 字节。结论以解析器为准: 这是自检本身的缺陷(计数没有正确跳过字符串/转义), "
                 "不是 native 交出去的 JSON 有问题。";
        return r;
    }

    // ---- ④ 编码再核一遍(解析器已经在字符串里查过, 这里覆盖字符串外的字节) ----
    size_t bad = 0;
    if (!utf8Ok(s, &bad)) {
        r.ok = false;
        r.selfCheckDisagreed = false;
        r.badAt = bad;
        r.why = "JSON 确实坏了: 第 " + std::to_string(bad) +
                " 个字节起不是合法 UTF-8(这正是调用方 JSON.parse 会抛异常、"
                "界面只剩「native 未给文本」的原因)";
        return r;
    }

    r.why = "JSON 自检通过: 严格解析器接受了整段文本(括号计数扫描同样通过, depth=0)";
    return r;
}

// GL 错误串(只用于诊断, 不改变负载)
std::string glErrName(GLenum e)
{
    switch (e) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        default: return "GL_ERROR_0x" + [e]() {
            char b[16];
            snprintf(b, sizeof(b), "%04X", (unsigned)e);
            return std::string(b);
        }();
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

std::string glErrorAfter(const char* what)
{
    const GLenum e = glGetError();
    if (e == GL_NO_ERROR) {
        return std::string();
    }
    return std::string(what) + " -> " + glErrName(e);
}

// ---------------------------------------------------------------------------
//  3b. 场景常量(与设备无关; 与 GLSL 里的常量同值, 由 verify 脚本逐项核对;
//      它们是负载定义的一部分 —— 改任何一个都必须递增 kSnBenchVersion)
// ---------------------------------------------------------------------------
const float kFar = 400.0f;
const float kSunDir[3] = {0.40824829f, 0.81649658f, -0.40824829f};
const float kSkyColor[3] = {0.055f, 0.085f, 0.135f};

// ---------------------------------------------------------------------------
//  4. 着色器源码(全部 GLSL ES 3.00)
//     两份管线各一个顶点着色器: 都是"用 gl_InstanceID 在 GPU 侧切网格",
//     顶点位置只由 gl_VertexID / gl_InstanceID 决定 —— 不读任何顶点属性缓冲,
//     所以 CPU 递交的几何数据为 0 字节。
// ---------------------------------------------------------------------------

// 顶点着色器 A: 把屏幕切成 uGridW x uGridH 个 32x32 像素的 tile。
//   越界的 tile 被 clamp 到视口内(见 kTilesYCeil 的注释: 宁可重叠, 不留缝)。
const char* VS_TILE = R"GLSL(#version 300 es
precision highp float;
uniform vec2 uGridW;      // (tilesX, tilesY) 实际使用的网格
uniform vec2 uTileSize;   // (32, 32) 像素
uniform vec2 uTargetSize; // (1920, 1080)
out vec2 vUV;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 origin = vec2(float(gl_InstanceID % int(uGridW.x)), float(gl_InstanceID / int(uGridW.x))) * uTileSize;
    origin = clamp(origin, vec2(0.0), uTargetSize - uTileSize);
    vec2 px = origin + corner * uTileSize;
    vec2 ndc = vec2(px.x / uTargetSize.x, px.y / uTargetSize.y) * 2.0 - 1.0;
    vUV = corner;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

// 顶点着色器 B: 与 A 完全相同的网格(同一份工作量), 单独一份源码是因为
//   两趟的 varying / precision 需求不同, 避免驱动做跨程序消除时行为不一致。
const char* VS_SHADE = R"GLSL(#version 300 es
precision highp float;
uniform vec2 uGridW;
uniform vec2 uTileSize;
uniform vec2 uTargetSize;
out vec2 vUV;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 origin = vec2(float(gl_InstanceID % int(uGridW.x)), float(gl_InstanceID / int(uGridW.x))) * uTileSize;
    origin = clamp(origin, vec2(0.0), uTargetSize - uTileSize);
    vec2 px = origin + corner * uTileSize;
    vec2 ndc = vec2(px.x / uTargetSize.x, px.y / uTargetSize.y) * 2.0 - 1.0;
    vUV = corner;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
//  趟 A: 表面求解 + G-buffer
//    每个像素: 由 uSeed 决定的一条主光线 -> 对程序化场景求最近命中(固定 8 步)
//    -> 3 份 3 octave 值噪声(高度细节 / 法线 / 材质) -> 写 G-buffer
//    (RT0: 位置 / 材质ID, RT1: 法线 / 粗糙度, depth: 线性深度 / uFar)
//    固定的步数与固定的 octave 数 = "每单位工作量恒定"的前提。
// ---------------------------------------------------------------------------
const char* FS_SN_GBUFFER = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
layout(location = 0) out vec4 oPosMat;
layout(location = 1) out vec4 oNrmRough;
uniform float uSeed;
uniform vec2 uTargetSize;
uniform float uFar;

float hash21(vec2 pk) {
    uvec2 q = uvec2(ivec2(floor(pk)));
    uint h = q.x * 1597334677u ^ (q.y + 2654435761u) * 3812015801u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    return float(h & 0x00FFFFFFu) * (1.0 / 16777215.0);
}
float noise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i.xy + vec2(i.z * 19.0, 7.0));
    float b = hash21(i.xy + vec2(i.z * 19.0 + 1.0, 7.0));
    float c = hash21(i.xy + vec2(i.z * 19.0, 8.0));
    float d = hash21(i.xy + vec2(i.z * 19.0 + 1.0, 8.0));
    float e = hash21(i.xy + vec2(i.z * 19.0 + 31.0, 7.0));
    float g = hash21(i.xy + vec2(i.z * 19.0 + 32.0, 7.0));
    float hh = hash21(i.xy + vec2(i.z * 19.0 + 31.0, 8.0));
    float k = hash21(i.xy + vec2(i.z * 19.0 + 32.0, 8.0));
    float x1 = mix(a, b, f.x);
    float x2 = mix(c, d, f.x);
    float x3 = mix(e, g, f.x);
    float x4 = mix(hh, k, f.x);
    return mix(mix(x1, x2, f.y), mix(x3, x4, f.y), f.z);
}
float terrain(vec2 p) {
    return 2.4 * sin(p.x * 0.021 + 0.7) * cos(p.y * 0.017 - 0.3)
         + 1.1 * sin(p.x * 0.053 - 1.1)
         + 0.7 * cos(p.y * 0.061 + 2.1)
         + 1.5 * sin(p.x * 0.0083) * sin(p.y * 0.0091);
}
float detail(vec3 p) {
    float acc = 0.0;
    float amp = 0.55;
    vec3 q = p;
    for (int i = 0; i < 3; ++i) {
        acc += amp * (noise3(q) * 2.0 - 1.0);
        q = q * 2.07 + vec3(13.0, 7.0, 3.0);
        amp *= 0.5;
    }
    return acc;
}
float sceneDist(vec3 p) {
    float d = p.y - terrain(p.xz);
    d = min(d, length(p - vec3(-16.0, 5.0, 24.0)) - 4.2);
    d = min(d, length(p - vec3(21.0, 3.0, -18.0)) - 3.0);
    d = min(d, length(p - vec3(2.0, 2.0, -34.0)) - 5.0);
    return d;
}
float traceRay(vec3 ro, vec3 rd) {
    float t = 0.4;
    float hit = -1.0;
    for (int i = 0; i < 8; ++i) {
        float d = sceneDist(ro + rd * t);
        if (d < 0.02 * t) { hit = t; break; }
        t += max(d * 0.85, 0.05);
        if (t > 400.0) { break; }
    }
    return hit;
}
void main() {
    float seedA = uSeed * 0.6180339887;
    vec2 uv = (gl_FragCoord.xy + vec2(0.5)) / uTargetSize;
    vec2 ndc = uv * 2.0 - 1.0;
    float camT = seedA * 6.2831853;
    vec3 camPos = vec3(camT * 3.0, 4.2, camT * 2.0);
    vec3 camFwd = normalize(vec3(sin(camT), -0.08, cos(camT)));
    vec3 camRight = normalize(cross(vec3(0.0, 1.0, 0.0), camFwd));
    vec3 camUp = cross(camFwd, camRight);
    vec3 rd = normalize(camFwd * 1.6 + camRight * ndc.x * 1.05 + camUp * ndc.y * 0.6);
    float t = traceRay(camPos, rd);
    float linearDepth = (t > 0.0) ? t : uFar;
    vec3 wp = camPos + rd * linearDepth;
    vec3 nrm = vec3(0.0, 1.0, 0.0);
    float rough = 0.88;
    vec3 albedo = vec3(0.12, 0.16, 0.22);
    float matId = 0.0;
    if (t > 0.0) {
        // detail() 的返回类型是 float(见上面的定义: float detail(vec3)), 所以这里必须用
        // 标量承接。原来写成 "vec3 d1 = detail(...)" 会把 float 赋给 vec3, 驱动直接报
        // S0001: cannot convert from 'float' to 'vec3'。
        // 用标量不会有任何类型歧义, 且与"vec3(detail(...)) 再取 .x/.y/.z"完全等价:
        // 每像素仍然是 2 次 3-octave 值噪声, 离线数出的 op/px 保持 176 不变。
        float d1 = detail(wp * 0.11);
        float d2 = detail(wp * 0.37 + vec3(5.0, 1.0, 9.0));
        float hgt = d1 * 1.6 + d2 * 0.45;
        nrm = normalize(vec3(d1, 1.0, d2));
        rough = clamp(0.95 - 0.42 * d2 - 0.18 * hgt, 0.05, 1.0);
        matId = clamp(0.5 + 0.5 * sin(hgt * 2.3), 0.0, 1.0);
        albedo = mix(vec3(0.30, 0.27, 0.20), vec3(0.11, 0.13, 0.19), matId);
    }
    oPosMat = vec4(wp, matId);
    oNrmRough = vec4(nrm * 0.5 + 0.5, rough);
    gl_FragDepth = clamp(linearDepth / uFar, 0.0, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
//  趟 B: deferred PBR 着色
//    每个像素: 读回 G-buffer -> 太阳直射 + 阴影项 + 密集物间接光 + 3 盏点光源
//    (GGX 高光) + 程序化天空环境光 + ACES 色调映射 + 暗角 + 颗粒。
//    "阴影项"是解析式遮蔽(与太阳方向对齐的一种固定形式), 它同样是**每像素单一
//    固定路径**的工作量 —— 不是随机抽查、不做逐像素循环, 所以每单位工作量恒定。
// ---------------------------------------------------------------------------
const char* FS_SN_SHADE = R"GLSL(#version 300 es
precision highp float;
in vec2 vUV;
layout(location = 0) out vec4 oColor;
uniform sampler2D uPosMat;
uniform sampler2D uNrmRough;
uniform vec2 uTargetSize;
uniform float uSeed;
uniform vec3 uSun;
uniform vec3 uSkyColor;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 pbr(vec3 n, vec3 v, vec3 l, vec3 albedo, float rough, vec3 radiance) {
    vec3 hv = normalize(v + l);
    float ndl = max(dot(n, l), 0.0);
    float ndh = max(dot(n, hv), 0.0);
    float ndv = max(dot(n, v), 0.0);
    float a = max(rough * rough, 0.002);
    float d2 = ndh * ndh * (a * a - 1.0) + 1.0;
    float spec = (a * a) / (3.14159265 * d2 * d2);
    float vh = 0.5 / max(mix(2.0 * ndv * max(dot(v, hv), 0.0), ndv + max(dot(n, l), 0.0), rough), 1.0e-4);
    vec3 f0 = mix(vec3(0.04), albedo, 0.35);
    vec3 fres = f0 + (vec3(1.0) - f0) * pow(1.0 - max(dot(hv, v), 0.0), 5.0);
    vec3 diffuse = albedo * (1.0 / 3.14159265);
    return (diffuse + spec * vh * fres) * ndl * radiance;
}

void main() {
    vec4 posMat = texture(uPosMat, vUV);
    vec4 nrmRough = texture(uNrmRough, vUV);
    vec3 wp = posMat.xyz;
    float matId = posMat.w;
    vec3 n = normalize(nrmRough.xyz * 2.0 - 1.0);
    float rough = clamp(nrmRough.w, 0.05, 1.0);
    vec3 albedo = mix(vec3(0.30, 0.27, 0.20), vec3(0.11, 0.13, 0.19), matId);
    float hgt = (albedo.r + albedo.g + albedo.b) * 0.3333;
    float seedA = uSeed * 0.6180339887;
    float camT = seedA * 6.2831853;
    vec3 camPos = vec3(camT * 3.0, 4.2, camT * 2.0);
    vec3 v = normalize(camPos - wp);
    vec3 col = vec3(0.0);
    col += pbr(n, v, uSun, albedo, rough, vec3(3.2, 2.95, 2.65));
    float ao = clamp(0.35 + 0.65 * n.y * 0.5 + 0.5, 0.0, 1.0);
    col *= mix(1.0, ao, 0.65);
    col += albedo * uSkyColor * 0.55 * (0.5 + 0.5 * n.y);
    vec3 lp0 = vec3(-8.0 + 6.0 * seedA, 3.4, 12.0);
    vec3 lp1 = vec3(9.0, 2.6, -6.0 + 4.0 * seedA);
    vec3 lp2 = vec3(0.0 + 3.0 * seedA, 6.0, 18.0);
    col += pbr(n, v, normalize(lp0 - wp), albedo, rough, vec3(2.4, 0.9, 0.5) / (1.0 + 0.004 * dot(lp0 - wp, lp0 - wp)));
    col += pbr(n, v, normalize(lp1 - wp), albedo, rough, vec3(0.5, 1.6, 2.2) / (1.0 + 0.004 * dot(lp1 - wp, lp1 - wp)));
    col += pbr(n, v, normalize(lp2 - wp), albedo, rough, vec3(1.6, 1.5, 1.1) / (1.0 + 0.003 * dot(lp2 - wp, lp2 - wp)));
    vec2 suv = (gl_FragCoord.xy + vec2(1.0, 1.0)) / uTargetSize;
    float st = texture(uPosMat, suv).w;
    col *= (0.82 + 0.18 * st);
    col = mix(col, vec3(luma(col)), 0.06);
    col *= 0.85;
    col = aces(col);
    col = pow(col, vec3(1.0 / 2.2));
    vec2 dc = vUV * 2.0 - 1.0;
    col *= 1.0 - 0.18 * dot(dc, dc);
    float grain = fract(sin(dot(vUV + vec2(uSeed * 0.017, uSeed * 0.031), vec2(12.9898, 78.233))) * 43758.5453);
    col += (grain - 0.5) * 0.012;
    col *= 1.0 + 0.004 * hgt;
    oColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
//  5. EGL/GLES3 离屏上下文(懒加载, 与 gpu7 / gpu 同款做法)
// ---------------------------------------------------------------------------

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLConfig g_config = nullptr;
EGLSurface g_pbuffer = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
bool g_ready = false;

GLuint g_vsTile = 0;
GLuint g_vsShade = 0;
GLuint g_progGBuffer = 0;
GLuint g_progShade = 0;
GLuint g_vao = 0;

// G-buffer: RGBA8 x2 + depth24 renderbuffer; 颜色目标: RGBA8 x2
GLuint g_gbufFbo = 0;
GLuint g_gbufPosMat = 0;
GLuint g_gbufNrmRough = 0;
GLuint g_gbufDepthRb = 0;
GLuint g_colorFbo = 0;
GLuint g_colorTex[2] = {0, 0};
int g_depthBits = 0;

std::string g_glVersion;
std::string g_glVendor;
std::string g_glRenderer;
std::string g_glslVersion;
int g_maxDrawBuffers = 0;
int g_maxSamples = 0;
bool g_hasFence = false;
bool g_hasMrt = false;

std::string prepareInternal()
{
    if (g_ready) {
        return std::string();
    }
    if (g_display == EGL_NO_DISPLAY) {
        g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_display == EGL_NO_DISPLAY) {
            return "no EGL display (eglGetDisplay returned EGL_NO_DISPLAY)";
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(g_display, &major, &minor) != EGL_TRUE) {
            g_display = EGL_NO_DISPLAY;
            return "eglInitialize failed";
        }
    }
    if (g_context == EGL_NO_CONTEXT) {
        const EGLint cfgAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                     EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                     EGL_NONE};
        EGLint numConfig = 0;
        if (eglChooseConfig(g_display, cfgAttribs, &g_config, 1, &numConfig) != EGL_TRUE || numConfig < 1) {
            return "eglChooseConfig failed (need EGL_OPENGL_ES3_BIT + depth)";
        }
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctxAttribs);
        if (g_context == EGL_NO_CONTEXT) {
            return "eglCreateContext failed (EGL_CONTEXT_CLIENT_VERSION=3)";
        }
        const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        g_pbuffer = eglCreatePbufferSurface(g_display, g_config, pbAttribs);
        if (g_pbuffer == EGL_NO_SURFACE) {
            return "eglCreatePbufferSurface failed";
        }
    }
    if (eglMakeCurrent(g_display, g_pbuffer, g_pbuffer, g_context) != EGL_TRUE) {
        return "eglMakeCurrent failed";
    }
    return std::string();
}

std::string compileShader(GLenum type, const char* src, const char* label, GLuint* out)
{
    *out = 0;
    GLuint sh = glCreateShader(type);
    if (sh == 0) {
        return std::string("glCreateShader failed for ") + label;
    }
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log((size_t)(len > 1 ? len : 1), '\0');
        glGetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
        std::string msg = std::string("shader compile failed [") + label + "]: " + std::string(log.data());
        glDeleteShader(sh);
        return msg;
    }
    *out = sh;
    return std::string();
}

std::string linkProgram(GLuint vs, GLuint fs, const char* label, GLuint* out)
{
    *out = 0;
    GLuint p = glCreateProgram();
    if (p == 0) {
        return std::string("glCreateProgram failed for ") + label;
    }
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDetachShader(p, vs);
    glDetachShader(p, fs);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log((size_t)(len > 1 ? len : 1), '\0');
        glGetProgramInfoLog(p, (GLsizei)log.size(), nullptr, log.data());
        std::string msg = std::string("program link failed [") + label + "]: " + std::string(log.data());
        glDeleteProgram(p);
        return msg;
    }
    *out = p;
    return std::string();
}

bool makeTexture(GLuint* tex, int w, int h, GLenum internal, GLenum format, GLenum type)
{
    glGenTextures(1, tex);
    if (*tex == 0) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    clearGlErrors();
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, tex);
        *tex = 0;
        return false;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

std::string buildTargets()
{
    if (g_gbufFbo != 0) {
        return std::string();
    }
    if (!makeTexture(&g_gbufPosMat, kWidth, kHeight, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE)) {
        return "g-buffer texture 0 create failed";
    }
    if (!makeTexture(&g_gbufNrmRough, kWidth, kHeight, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE)) {
        return "g-buffer texture 1 create failed";
    }
    if (!makeTexture(&g_colorTex[0], kWidth, kHeight, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) ||
        !makeTexture(&g_colorTex[1], kWidth, kHeight, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE)) {
        return "color target create failed";
    }

    glGenFramebuffers(1, &g_gbufFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gbufFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gbufPosMat, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_gbufNrmRough, 0);
    glGenRenderbuffers(1, &g_gbufDepthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, g_gbufDepthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kWidth, kHeight);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_DEPTH_SIZE, &g_depthBits);
    if (g_depthBits < 24) {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, kWidth, kHeight);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_DEPTH_SIZE, &g_depthBits);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_gbufDepthRb);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char b[64];
        snprintf(b, sizeof(b), "0x%04X", (unsigned)st);
        return std::string("g-buffer fbo incomplete: glCheckFramebufferStatus=") + b +
               " (需要 MRT 2 个颜色附着 + depth, 本工程其它 GPU 模块同样依赖)";
    }
    g_hasMrt = true;

    glGenFramebuffers(1, &g_colorFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_colorFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_colorTex[0], 0);
    const GLenum st2 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st2 != GL_FRAMEBUFFER_COMPLETE) {
        char b[64];
        snprintf(b, sizeof(b), "0x%04X", (unsigned)st2);
        return std::string("color fbo incomplete: glCheckFramebufferStatus=") + b;
    }
    return std::string();
}

std::string buildPrograms()
{
    if (g_progGBuffer != 0) {
        return std::string();
    }
    std::string err = compileShader(GL_VERTEX_SHADER, VS_TILE, "VS_TILE", &g_vsTile);
    if (!err.empty()) {
        return err;
    }
    err = compileShader(GL_VERTEX_SHADER, VS_SHADE, "VS_SHADE", &g_vsShade);
    if (!err.empty()) {
        return err;
    }
    GLuint fsGB = 0;
    err = compileShader(GL_FRAGMENT_SHADER, FS_SN_GBUFFER, "FS_SN_GBUFFER", &fsGB);
    if (!err.empty()) {
        return err;
    }
    GLuint fsSh = 0;
    err = compileShader(GL_FRAGMENT_SHADER, FS_SN_SHADE, "FS_SN_SHADE", &fsSh);
    if (!err.empty()) {
        glDeleteShader(fsGB);
        return err;
    }
    err = linkProgram(g_vsTile, fsGB, "SN_G-buffer", &g_progGBuffer);
    if (err.empty()) {
        err = linkProgram(g_vsShade, fsSh, "SN-shade", &g_progShade);
    }
    glDeleteShader(fsGB);
    glDeleteShader(fsSh);
    return err;
}


// ---------------------------------------------------------------------------
//  6. 编译器 / 运行时能力(记录, 用于"尺子有没有被喂满"的判断)
//      这里拿到的任何东西都不参与计分与负载: 只写进结果 JSON 的 ruler 块。
// ---------------------------------------------------------------------------

struct SimdFeatures {
    bool neon;
    bool sve;
    bool sve2;
    bool dotprod;
    bool fp16;
    bool bf16;
    bool i8mm;
    bool crc;
    bool aes;
    bool sha2;
    bool lse;
    const char* marchString;  // __ARM_FEATURE 是否可用(__ARM_ARCH 值)
    int armArch;
};

SimdFeatures probeSimdFeatures()
{
    SimdFeatures f;
    f.neon = false;
    f.sve = false;
    f.sve2 = false;
    f.dotprod = false;
    f.fp16 = false;
    f.bf16 = false;
    f.i8mm = false;
    f.crc = false;
    f.aes = false;
    f.sha2 = false;
    f.lse = false;
    f.marchString = "unknown";
    f.armArch = 0;
#if defined(__ARM_ARCH)
    f.armArch = __ARM_ARCH;
    f.marchString = "defined";
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    f.neon = true;
#endif
#if defined(__ARM_FEATURE_SVE)
    f.sve = true;
#endif
#if defined(__ARM_FEATURE_SVE2)
    f.sve2 = true;
#endif
#if defined(__ARM_FEATURE_DOTPROD)
    f.dotprod = true;
#endif
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC) || defined(__ARM_FEATURE_FP16_SCALAR_ARITHMETIC)
    f.fp16 = true;
#endif
#if defined(__ARM_FEATURE_BF16_VECTOR_ARITHMETIC)
    f.bf16 = true;
#endif
#if defined(__ARM_FEATURE_MATMUL_INT8)
    f.i8mm = true;
#endif
#if defined(__ARM_FEATURE_CRC32)
    f.crc = true;
#endif
#if defined(__ARM_FEATURE_CRYPTO)
    f.aes = true;
    f.sha2 = true;
#endif
#if defined(__ARM_FEATURE_LSE)
    f.lse = true;
#endif
    return f;
}

struct CpuProfile {
    int logical;
    int physical;
    int smtPossible;
    int smtEnabled;
    int threadCap;
    int topologyKnown;
    std::string sourceText;
    std::string probeText;
    SimdFeatures simd;
};

CpuProfile probeCpu()
{
    CpuProfile p;
    p.logical = 0;
    p.physical = 0;
    p.smtPossible = 0;
    p.smtEnabled = 0;
    p.threadCap = 0;
    p.topologyKnown = 0;
    // 复用工程既有的、已被 verify_smt_topology.py 钉死的拓扑实现(只读, 不改变任何负载)
    const AuroraCpuTopoInfo& info = ::auroraSmtTopology();
    p.logical = info.logical;
    p.physical = info.physical;
    p.smtPossible = info.smtPossible;
    p.topologyKnown = info.known;
    p.sourceText = std::string(info.sourceText);
    p.probeText = std::string(info.probeText);
    p.smtEnabled = ::auroraSmtEnabled();
    p.threadCap = ::auroraThreadCap();
    p.simd = probeSimdFeatures();
    return p;
}

// GL / GPU 标识(记录; 不参与计分)
void probeGlInfo()
{
    const char* s = nullptr;
    s = (const char*)glGetString(GL_VERSION);
    g_glVersion = (s != nullptr) ? s : "";
    s = (const char*)glGetString(GL_VENDOR);
    g_glVendor = (s != nullptr) ? s : "";
    s = (const char*)glGetString(GL_RENDERER);
    g_glRenderer = (s != nullptr) ? s : "";
    s = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    g_glslVersion = (s != nullptr) ? s : "";
    g_maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &g_maxDrawBuffers);
    g_maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &g_maxSamples);
}

// 一次"pass 数 -> 帧"的函数。返回 false 表示 GL 侧出错(错误串在 g_snError)。
bool snRunFrames(int passesPerFrame, int frames, bool measureCpu, double* outTotalMs,
                 double* outCpuSubmitMs, double* outGpuBusyMs);

// ---------------------------------------------------------------------------
//  7. 一趟 = 提交 2 次 draw call(与工作规模无关的常量)
// ---------------------------------------------------------------------------

// 顶点网格: 每趟都用同一份网格 —— (kTilesX, kTilesYCeil) 覆盖整个 1920x1080。
// 实例数 = tilesX x tilesY, 每个实例固定 1024 个片元(越界的实例被 clamp 后重叠,
// 照样跑完整着色器, 保证每单位工作量不随 pass 数 / 设备变化)。
struct GridInfo {
    int tilesX;
    int tilesY;
    int instances;
    double pixels;  // 本趟覆盖的像素数(>= W*H; 多出的是重叠)
};

GridInfo gridForFullScreen()
{
    GridInfo g;
    g.tilesX = kTilesX;
    g.tilesY = kTilesYCeil;
    g.instances = g.tilesX * g.tilesY;
    g.pixels = (double)g.instances * (double)kFragsPerTile;
    return g;
}

// 每帧工作量(不管 pass 数怎么变, 这个式子的结构不变):
//   pixelsPerFrame = passesPerFrame x (一趟的像素数)
//   fragmentInvocationsPerFrame = pixelsPerFrame x 2  (两趟管线: G-buffer + 着色)
double pixelsPerFrame(int passesPerFrame)
{
    return gridForFullScreen().pixels * (double)passesPerFrame;
}
double pixelsPerSecond(int passesPerFrame, double fps)
{
    return pixelsPerFrame(passesPerFrame) * fps;
}

// MRT 的 draw buffer 表(常量)
GLuint g_fboDrawBufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

void drawGridFullScreen(int instances)
{
    // 唯一的一次几何递交: 顶点数固定 6(两个三角形), 实例数 = tile 数。
    // 没有任何顶点属性缓冲 —— 位置由 gl_VertexID / gl_InstanceID 在 GPU 侧算出。
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, instances);
}

// 一趟 = 趟 A(G-buffer) + 趟 B(deferred 着色)。
// 返回本趟的 GL 调用数(常量, 用于自证"CPU 递交量与工作规模无关")。
int submitPass(int passIndex, GLuint seedUniformGB, GLuint seedUniformSh,
               GLuint gridUniformGB, GLuint tileUniformGB, GLuint sizeUniformGB, GLuint farUniformGB,
               GLuint gridUniformSh, GLuint tileUniformSh, GLuint sizeUniformSh, GLuint sunUniformSh,
               GLuint skyUniformSh, GLuint posMatSampler, GLuint nrmSampler, int writeTarget)
{
    const GridInfo grid = gridForFullScreen();
    const float seed = 0.125f + 0.037f * (float)(passIndex % 64);
    int calls = 0;

    // ---- 趟 A: 写 G-buffer(RT0 位置+材质, RT1 法线+粗糙度 + depth) ----
    glBindFramebuffer(GL_FRAMEBUFFER, g_gbufFbo);
    glDrawBuffers(2, g_fboDrawBufs);
    glViewport(0, 0, kWidth, kHeight);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    calls += 10;

    glUseProgram(g_progGBuffer);
    glUniform1f(seedUniformGB, seed);
    glUniform2f(gridUniformGB, (float)grid.tilesX, (float)grid.tilesY);
    glUniform2f(tileUniformGB, (float)kTilePx, (float)kTilePx);
    glUniform2f(sizeUniformGB, (float)kWidth, (float)kHeight);
    glUniform1f(farUniformGB, (float)kFar);
    calls += 6;
    glBindVertexArray(g_vao);
    drawGridFullScreen(grid.instances);
    calls += 2;

    // ---- 趟 B: deferred 着色(读 G-buffer, 写颜色目标) ----
    // 双缓冲: 每趟写到 g_colorTex[passIndex % 2], 相邻两趟的输出不同 => 不会被
    // 驱动整体消除, 也不能被缓存复用 —— 每趟都是实打实的一次全屏着色。
    const GLuint dst = g_colorTex[writeTarget & 1];
    glBindFramebuffer(GL_FRAMEBUFFER, g_colorFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    glViewport(0, 0, kWidth, kHeight);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    calls += 5;

    glUseProgram(g_progShade);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_gbufPosMat);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_gbufNrmRough);
    glUniform1i(posMatSampler, 0);
    glUniform1i(nrmSampler, 1);
    glUniform1f(seedUniformSh, seed);
    glUniform2f(gridUniformSh, (float)grid.tilesX, (float)grid.tilesY);
    glUniform2f(tileUniformSh, (float)kTilePx, (float)kTilePx);
    glUniform2f(sizeUniformSh, (float)kWidth, (float)kHeight);
    glUniform3f(sunUniformSh, kSunDir[0], kSunDir[1], kSunDir[2]);
    glUniform3f(skyUniformSh, kSkyColor[0], kSkyColor[1], kSkyColor[2]);
    calls += 13;
    glBindVertexArray(g_vao);
    drawGridFullScreen(grid.instances);
    calls += 2;

    return calls;
}

// ---------------------------------------------------------------------------
//  8. 设备画像 / 场景不变量(与"尺子"有关的记录)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  9. 帧执行: 一段"passesPerFrame 趟"的帧, 可选测 CPU 递交时间 / 读回自证
// ---------------------------------------------------------------------------

// 读回 32x32 一小块做自证(不是全屏回读; 只读 4KB)
void readbackSelfProof(unsigned long long* sum, int* nonZero, int* minV, int* maxV)
{
    static unsigned char buf[kReadbackPx * kReadbackPx * 4];
    *sum = 0;
    *nonZero = 0;
    *minV = 255;
    *maxV = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_colorFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    clearGlErrors();
    glReadPixels(0, 0, kReadbackPx, kReadbackPx, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        *minV = -1;
        *maxV = -1;
        appendSnError(std::string("glReadPixels(selfProof) ") + glErrName(e));
        return;
    }
    const int n = kReadbackPx * kReadbackPx * 4;
    for (int i = 0; i < n; ++i) {
        const int v = (int)buf[i];
        *sum += (unsigned long long)v;
        if (v != 0) {
            ++(*nonZero);
        }
        if (v < *minV) {
            *minV = v;
        }
        if (v > *maxV) {
            *maxV = v;
        }
    }
}

struct FrameTiming {
    bool ok;
    std::string error;
    double totalMs;          // 整段墙钟(CPU 递交 + 等 GPU)
    double cpuSubmitMs;      // 本段里 CPU 花在 GL 提交上的时间(不含 glFinish/等 fence)
    double gpuBusyMs;        // 用 GL fence 量到的 GPU 忙时间(拿不到 fence 时为 -1)
    unsigned long long readbackSum;
    int readbackNonZero;
    int readbackMin;
    int readbackMax;
};

FrameTiming runFrames(int passesPerFrame, int frames, bool captureDetail)
{
    FrameTiming ft;
    ft.ok = false;
    ft.totalMs = 0.0;
    ft.cpuSubmitMs = 0.0;
    ft.gpuBusyMs = -1.0;
    ft.readbackSum = 0;
    ft.readbackNonZero = 0;
    ft.readbackMin = 0;
    ft.readbackMax = 0;
    if (frames <= 0 || passesPerFrame <= 0) {
        ft.error = "runFrames: bad arguments";
        return ft;
    }
    clearGlErrors();

    // 每趟的 uniform 位置在第一次用到时查一次(常量级, 不进计时循环的账)
    static GLint uGB_seed = -1, uGB_grid = -1, uGB_tile = -1, uGB_size = -1, uGB_far = -1;
    static GLint uSH_seed = -1, uSH_grid = -1, uSH_tile = -1, uSH_size = -1, uSH_sun = -1, uSH_sky = -1;
    static GLint uSH_pos = -1, uSH_nrm = -1;
    if (uGB_seed == -1) {
        uGB_seed = glGetUniformLocation(g_progGBuffer, "uSeed");
        uGB_grid = glGetUniformLocation(g_progGBuffer, "uGridW");
        uGB_tile = glGetUniformLocation(g_progGBuffer, "uTileSize");
        uGB_size = glGetUniformLocation(g_progGBuffer, "uTargetSize");
        uGB_far = glGetUniformLocation(g_progGBuffer, "uFar");
        uSH_seed = glGetUniformLocation(g_progShade, "uSeed");
        uSH_grid = glGetUniformLocation(g_progShade, "uGridW");
        uSH_tile = glGetUniformLocation(g_progShade, "uTileSize");
        uSH_size = glGetUniformLocation(g_progShade, "uTargetSize");
        uSH_sun = glGetUniformLocation(g_progShade, "uSun");
        uSH_sky = glGetUniformLocation(g_progShade, "uSkyColor");
        uSH_pos = glGetUniformLocation(g_progShade, "uPosMat");
        uSH_nrm = glGetUniformLocation(g_progShade, "uNrmRough");
        if (uGB_seed < 0 || uGB_grid < 0 || uGB_far < 0 || uSH_seed < 0 || uSH_pos < 0 || uSH_nrm < 0) {
            ft.error = "uniform location lookup failed (着色器接口与 C++ 侧不一致)";
            return ft;
        }
    }

    GLsync fence = nullptr;
    const double tStart = nowMs();
    double cpuNs = 0.0;
    for (int f = 0; f < frames; ++f) {
        const double c0 = nowMs();
        for (int p = 0; p < passesPerFrame; ++p) {
            submitPass(p, (GLuint)uGB_seed, (GLuint)uSH_seed, (GLuint)uGB_grid, (GLuint)uGB_tile,
                       (GLuint)uGB_size, (GLuint)uGB_far, (GLuint)uSH_grid, (GLuint)uSH_tile,
                       (GLuint)uSH_size, (GLuint)uSH_sun, (GLuint)uSH_sky, (GLuint)uSH_pos,
                       (GLuint)uSH_nrm, p);
        }
        if (g_hasFence) {
            fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (fence == nullptr) {
                g_hasFence = false;
            }
        }
        cpuNs += nowMs() - c0;
        if (g_hasFence && fence != nullptr) {
            // 等 GPU 把这一帧的指令全部做完(既计时也保证帧边界正确)
            glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
            glDeleteSync(fence);
            fence = nullptr;
        } else {
            glFinish();
        }
    }
    const double total = nowMs() - tStart;

    if (captureDetail) {
        readbackSelfProof(&ft.readbackSum, &ft.readbackNonZero, &ft.readbackMin, &ft.readbackMax);
    }
    const std::string gerr = glErrorAfter("frame loop");
    if (!gerr.empty()) {
        ft.error = gerr;
        return ft;
    }
    ft.ok = true;
    ft.totalMs = total;
    ft.cpuSubmitMs = cpuNs;
    // gpuBusyMs: 当 g_hasFence=false 时, 帧尾用的是 glFinish, 于是"墙钟 - CPU 递交"
    // 就是等 GPU 的时间(glFinish 会一直等到 GPU 做完), 这个差值仍然有意义。
    ft.gpuBusyMs = total - cpuNs;
    return ft;
}

// 自适应: 由"用 P 趟量到的每趟耗时"外推"要多少趟才到目标帧时间"。
//    这个函数只改趟数, 不改任何一趟里的工作(这是"自适应不改每单位工作量"的核心)。
//    取 2 的幂: 让"同一颗芯片两次运行落在不同档"不引入额外偏差, 也让阶梯可枚举。
int choosePasses(double perPassMs, double targetMs, int maxPasses, bool* clamped)
{
    *clamped = false;
    if (!(perPassMs > 0.0) || !(targetMs > 0.0)) {
        return 1;
    }
    double want = targetMs / perPassMs;
    if (want < 1.0) {
        want = 1.0;
        *clamped = true;
    }
    int p = 1;
    while ((double)p * 2.0 <= want && p * 2 <= kPassLadderMax) {
        p *= 2;
    }
    if (p > maxPasses) {
        p = maxPasses;
        *clamped = true;
    }
    if (p < kPassLadderMin) {
        p = kPassLadderMin;
        *clamped = true;
    }
    // 再夹一次到 2 的幂(自定义上限可能不是 2 的幂)
    int q = 1;
    while (q * 2 <= p && q * 2 <= kPassLadderMax) {
        q *= 2;
    }
    return q;
}

// 中位数(计时离散度用; 空输入返回 0)
double medianOf(const std::vector<double>& v)
{
    if (v.empty()) {
        return 0.0;
    }
    std::vector<double> s = v;
    std::sort(s.begin(), s.end());
    const size_t n = s.size();
    return (n % 2 == 1) ? s[n / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
}

// 相对离散度 = (最大 - 最小) / 中位 x 100%。中位为 0 时返回 0(调用方会另判可信度)。
double dispersionOf(double mn, double md, double mx)
{
    if (!(md > 0.0)) {
        return 0.0;
    }
    return (mx - mn) / md * 100.0;
}

// 主流跑分工具通行的可信度分档(阈值是我们自己定的, 报告里必须注明这一点):
//   <= 2%  RELIABLE / <= 5%  FAIR / > 5%  UNRELIABLE(该项数字仅供参考)
const char* credibilityOf(double dispersionPct)
{
    if (dispersionPct <= 2.0) {
        return "RELIABLE";
    }
    if (dispersionPct <= 5.0) {
        return "FAIR";
    }
    return "UNRELIABLE";
}

//  2026-10-05 加固(真机 8.0/8.1 现场) 环境状态字符串的唯一出口。
//   为什么不能直接把 const char* 拼进 JSON:
//     真机 payload(D:\gb7logs\r80\report-latest.txt 第 1091 行, 30372 字节)里,
//     environment.stability.state 出来的是一段野内存 "\xd0\x9d\xc2\xb1s\x7f",
//     repeatability.roundsDetail[].envState 是 "\xd0\xa0\xc2\xb1s\x7f" —— 两处都是
//     "未初始化/野指针 + 原样拼进 JSON 字符串值" 这条路走出来的。
//   规矩: 进 JSON 的 state 只允许是这四个规范化字面量之一, 其它一律降级为 UNKNOWN。
//   这样即使将来又有一条没赋值的路径, 交出去的也是一句明确的"未知", 而不是野字节。
const char* snCanonState(const char* s)
{
    if (s == nullptr) {
        return "UNKNOWN";
    }
    static const char* kKnown[4] = {"STABLE", "WARMING_UP", "THROTTLED", "UNKNOWN"};
    for (int i = 0; i < 4; ++i) {
        if (std::strcmp(s, kKnown[i]) == 0) {
            return kKnown[i];
        }
    }
    return "UNKNOWN";
}

// 本小节的真实用途(验收标准第 4 条: 每一项都要一句话说明它代表什么真实用途)
const char* kRealUseText =
    "非光追的 game-like 场景: 对程序化地形做光线求交(几何/可见性) -> 写 G-buffer -> 延迟着色 "
    "(太阳直射 + 阴影项 + 3 盏点光源 + 天空环境光 + ACES 色调映射)。代表现代手游的主渲染通路"
    "(开一局大地图游戏时 GPU 每秒要做的同一类工作), 不代表光追、也不代表 AI/生产力负载。";


// ---------------------------------------------------------------------------
//  9b. 运行环境采样(验收标准第 2、6 条): CPU 实际频率 + SoC 热区温度
//       只读旁路: 不写任何 sysfs、不改 governor、不设频率, 不计分,
//        也不改变负载的工作量。读不到就写 errno(不填 0 冒充数据)。
//       诚实边界: 麒麟平台没有可读的 GPU 频率/温度节点, 这里报的是 CPU 频率与
//        SoC 热区温度 —— 整机 DVFS/热状态的间接证据, 不是 GPU 自己的频率。
// ---------------------------------------------------------------------------

const int kEnvMaxCpus = 64;
const int kEnvMaxZones = 24;
const int kEnvIntervalMs = 50;

struct EnvSample {
    int cpu;
    int khz;
};

int g_envCount = 0;                        // 采样次数(每次 = 一轮全核扫描)
int g_envCpu[kEnvMaxCpus] = {0};           // 扫描到的核号
int g_envCpuCount = 0;
int g_envKhz[kEnvMaxCpus][kEnvMaxCpus] = {{0}};   // [sample][cpuIndex], kHz; <=0 = 没读到
int g_envTemp[32][kEnvMaxZones] = {{0}};          // [sample][zone], 摄氏度; INT_MIN = 没读到
int g_envZoneCount = 0;
char g_envZoneName[kEnvMaxZones][64] = {{0}};
int g_envTempSampleCount = 0;
int g_envCpuErrno = 0;
int g_envTempErrno = 0;
char g_envGovernor[48] = {0};
int g_envGovernorErrno = 0;
char g_envAllowedList[256] = {0};
int g_envAllowedCount = 0;
bool g_envAllowedOk = false;

std::thread g_envThread;
std::mutex g_envMutex;
std::condition_variable g_envCv;
//  2026-10-05: 原来的 g_envStop 是普通 bool, 而隔离舱的恢复路径不能取 g_envMutex
//   (崩溃可能正好发生在持锁的那段里, 取锁就是死等), 所以它必须是原子的: 恢复路径里
//   只做 store + notify_all(两者都不需要持有锁)。
std::atomic<bool> g_envStop{false};
volatile int g_envActive = 0;
// 采样线程自己进隔离舱兜住过一次故障(只影响"环境读数", 不影响分数口径)
volatile int g_envThreadFaulted = 0;

// 读一个小文本文件(纯 open/read/close, 不分配)
int envReadSmall(const char* path, char* buf, int cap)
{
    if (buf == nullptr || cap <= 1) {
        return -1;
    }
    buf[0] = '\0';
    errno = 0;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno != 0 ? errno : -1;
    }
    int n = 0;
    for (;;) {
        const ssize_t got = ::read(fd, buf + n, (size_t)(cap - 1 - n));
        if (got < 0) {
            const int e = errno;
            ::close(fd);
            return e != 0 ? e : -1;
        }
        if (got == 0) {
            break;
        }
        n += (int)got;
        if (n >= cap - 1) {
            break;
        }
    }
    ::close(fd);
    buf[n] = '\0';
    return 0;
}

// 解析出第一个整数(跳过非数字前导)
int envFirstInt(const char* s)
{
    if (s == nullptr) {
        return -1;
    }
    const char* p = s;
    while (*p != '\0' && (*p < '0' || *p > '9') && *p != '-') {
        ++p;
    }
    if (*p == '\0') {
        return -1;
    }
    return (int)strtol(p, nullptr, 10);
}

// 一次采样: 逐核频率 + 全部热区温度
void envSampleOnce()
{
    if (g_envCount >= 64) {
        return;
    }
    char path[96];
    char buf[64];
    const int s = g_envCount;
    for (int i = 0; i < g_envCpuCount; ++i) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", g_envCpu[i]);
        if (envReadSmall(path, buf, (int)sizeof(buf)) == 0) {
            g_envKhz[s][i] = envFirstInt(buf);
        } else {
            g_envKhz[s][i] = -1;
            g_envCpuErrno = errno != 0 ? errno : -1;
        }
    }
    for (int z = 0; z < g_envZoneCount; ++z) {
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        if (envReadSmall(path, buf, (int)sizeof(buf)) == 0) {
            g_envTemp[g_envTempSampleCount][z] = envFirstInt(buf);
        } else {
            g_envTemp[g_envTempSampleCount][z] = -1;
            g_envTempErrno = errno != 0 ? errno : -1;
        }
    }
    if (g_envZoneCount > 0 && g_envTempSampleCount < 31) {
        ++g_envTempSampleCount;
    }
    ++g_envCount;
}

void envThreadMain()
{
    // 环境采样线程也进隔离舱: 它自己崩了就只让采样停掉, 不把整个 App 带走。
    snfault::Guard guard;
    if (guard.outermost()) {
        if (sigsetjmp(snfault::tlsJmp, 1) != 0) {
            snfault::resetAfterFault();
            g_envThreadFaulted = 1;
            return;   // 线程就地结束; 下面的 join() 仍然会成功, envEnd 不会卡住
        }
    }
    //  采样本身不持锁 : 万一采样路径自己崩了, 隔离舱会从这个线程里 longjmp 出去,
    //   栈上 unique_lock 的析构函数不会执行 —— 要是当时还持着锁, 就会留下一个永远锁住的
    //   mutex, 让主线程在 envEnd() 里死等(比原来的段错误更难查)。所以只在"等下一次采样"
    //   的时候持锁, 采样的那一段裸跑。
    while (!g_envStop.load()) {
        envSampleOnce();
        std::unique_lock<std::mutex> lk(g_envMutex);
        g_envCv.wait_for(lk, std::chrono::milliseconds(kEnvIntervalMs),
                         []() { return g_envStop.load(); });
    }
}

void envBegin()
{
    g_envCount = 0;
    g_envTempSampleCount = 0;
    g_envCpuErrno = 0;
    g_envTempErrno = 0;
    g_envGovernorErrno = 0;
    g_envCpuCount = 0;
    g_envZoneCount = 0;
    g_envAllowedOk = false;
    g_envAllowedCount = 0;
    g_envAllowedList[0] = '\0';
    g_envGovernor[0] = '\0';

    // 允许核集合(sched_getaffinity 原文口径) —— 第 6 条"报告运行环境"
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            g_envAllowedOk = true;
            int n = 0;
            for (int c = 0; c < kEnvMaxCpus; ++c) {
                if (CPU_ISSET(c, &set)) {
                    ++n;
                    if (n <= 16) {
                        const int used = (int)strlen(g_envAllowedList);
                        snprintf(g_envAllowedList + used, sizeof(g_envAllowedList) - (size_t)used,
                                 (used == 0) ? "%d" : ",%d", c);
                    }
                }
            }
            g_envAllowedCount = n;
            if (n > 16) {
                const int used = (int)strlen(g_envAllowedList);
                snprintf(g_envAllowedList + used, sizeof(g_envAllowedList) - (size_t)used, ",...");
            }
        }
    }
    // governor(原文; 读不到就写 errno)
    if (envReadSmall("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", g_envGovernor,
                     (int)sizeof(g_envGovernor)) != 0) {
        g_envGovernorErrno = errno != 0 ? errno : -1;
    } else {
        size_t n = strlen(g_envGovernor);
        while (n > 0 && (g_envGovernor[n - 1] == '\n' || g_envGovernor[n - 1] == '\r')) {
            g_envGovernor[--n] = '\0';
        }
    }
    // 逐核扫描: 用 /proc/cpuinfo 的 processor 行数作为上界, 读不到就按可用核集合
    {
        char big[4096];
        if (envReadSmall("/proc/cpuinfo", big, (int)sizeof(big)) == 0) {
            const char* p = big;
            while (p != nullptr && *p != '\0' && g_envCpuCount < kEnvMaxCpus) {
                if (strncmp(p, "processor", 9) == 0) {
                    const int id = envFirstInt(p + 9);
                    if (id >= 0 && id < kEnvMaxCpus) {
                        g_envCpu[g_envCpuCount++] = id;
                    }
                }
                p = strchr(p, '\n');
                if (p != nullptr) {
                    ++p;
                }
            }
        }
    }
    if (g_envCpuCount <= 0) {
        g_envCpu[g_envCpuCount++] = 0;   // 至少采 cpu0, 报出来是单核口径
    }
    // 热区计数
    for (int z = 0; z < kEnvMaxZones; ++z) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        char t[32];
        if (envReadSmall(path, t, (int)sizeof(t)) == 0) {
            g_envZoneCount = z + 1;
        }
    }
    for (int z = 0; z < g_envZoneCount; ++z) {
        char path[96];
        char nm[64];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
        if (envReadSmall(path, nm, (int)sizeof(nm)) != 0) {
            nm[0] = '\0';
        }
        size_t n = strlen(nm);
        while (n > 0 && (nm[n - 1] == '\n' || nm[n - 1] == '\r')) {
            nm[--n] = '\0';
        }
        snprintf(g_envZoneName[z], sizeof(g_envZoneName[z]), "%s", nm);
        // 初始化该 zone 的全部样本为非 0 的哨兵值
        for (int s = 0; s < 32; ++s) {
            g_envTemp[s][z] = INT32_MIN;
        }
    }

    g_envStop = false;
    g_envActive = 1;
    if (g_envThreadFaulted) {
        // 上一次的采样线程已经自己崩过一次(隔离舱兜住了): 不再起第二个, 也不再采样 ——
        // 崩过一次的采样路径不应该被自动重放。环境读数降级为"只有一次性探测(允许核集合 /
        // governor / 热区清单)", 并在 warnings 里说明。
        return;
    }
    g_envThread = std::thread(envThreadMain);
}

void envEnd()
{
    if (!g_envActive) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_envMutex);
        g_envStop.store(true);
    }
    g_envCv.notify_all();
    if (g_envThread.joinable()) {
        g_envThread.join();
    }
    g_envActive = 0;
}

// 故障隔离舱的恢复路径专用: 不取任何锁地把环境采样线程叫停。
// 为什么不直接调 envEnd(): 崩溃完全可能发生在持有 g_envMutex 的那一小段里, 恢复路径
// 再去 lock 就是死等 —— 那等于把"段错误"换成"卡死", 更糟。atomic store + notify_all
// 这两个动作都不需要持有锁。
void snAbortEnvSamplingNoLock()
{
    g_envStop.store(true);
    g_envCv.notify_all();
    //  必须 detach : 恢复路径不会再走 envEnd() 里的 join, 而 g_envThread 是一个全局的
    //   std::thread —— 进程退出时如果它还是 joinable, 它的析构函数会调 std::terminate,
    //   那就变成"段错误兜住了, 却在退出时崩", 白忙一场。detach 之后线程自己跑到循环
    //   条件成立就退出, 不再需要任何人 join。
    try {
        if (g_envThread.joinable()) {
            g_envThread.detach();
        }
    } catch (...) {
        // detach 在极端情况下会抛 std::system_error; 失败路径上不因为它再出问题
    }
}


// ---------------------------------------------------------------------------
//  9c. 环境块出 JSON(第 2、6 条)
// ---------------------------------------------------------------------------

//  2026-10-05 真机崩溃的直接成因就在这个结构体上(修复: 每个成员都要有初值)
//   它以前是一个没有任何初始化的 POD, 而它同时是 SnMeasurement 的成员, 被
//   snlBuildJson() / snComparability() / warnings 判定直接读。只要有一条路径忘了
//   给它赋值, state / text 就是两个不确定的指针(C++ 里对未初始化指针成员求值本身
//   就是 UB), 再交给 std::string() / strcmp() 就变成 libc 里对 0x0 的 strlen ——
//   真机现场逐字段吻合: signal=11 SIGSEGV / si_code=1 SEGV_MAPERR / si_addr=0x0,
//   pc 不在 libaurorabench.so 的代码段内(说明崩在 libc 的 strlen/strcmp 里, 不在我们的代码里)。
//   现在每个成员都有初值: 即使将来再漏赋值, 读到的也是"明确的未知", 而不是野指针。
struct EnvDerived {
    double tempMin = 0.0;
    double tempMed = 0.0;
    double tempMax = 0.0;
    int tempZoneIdx = -1;   // 最热的那个 zone(取中位最高的)
    int tempSamples = 0;
    double khzMin = 0.0;
    double khzMed = 0.0;
    double khzMax = 0.0;
    int khzSamples = 0;
    double khzFirstHalfMed = 0.0;
    double khzSecondHalfMed = 0.0;
    bool throttled = false;
    bool warming = false;
    const char* state = "UNKNOWN";
    int allowedCount = 0;
    bool allowedOk = false;
    char allowedList[256] = {};
    char governor[48] = {};
    int governorErrno = 0;
    int cpuErrno = 0;
    int tempErrno = 0;
    int cpuCount = 0;
    int zoneCount = 0;
    const char* text = "";
};

EnvDerived envDerive()
{
    EnvDerived e;
    e.tempMin = 0.0;
    e.tempMed = 0.0;
    e.tempMax = 0.0;
    e.tempZoneIdx = -1;
    e.tempSamples = g_envTempSampleCount;
    e.khzMin = 0.0;
    e.khzMed = 0.0;
    e.khzMax = 0.0;
    e.khzSamples = 0;
    e.khzFirstHalfMed = 0.0;
    e.khzSecondHalfMed = 0.0;
    e.throttled = false;
    e.warming = false;
    e.state = "UNKNOWN";
    e.allowedCount = g_envAllowedCount;
    e.allowedOk = g_envAllowedOk;
    e.governorErrno = g_envGovernorErrno;
    e.cpuErrno = g_envCpuErrno;
    e.tempErrno = g_envTempErrno;
    e.cpuCount = g_envCpuCount;
    e.zoneCount = g_envZoneCount;
    snprintf(e.allowedList, sizeof(e.allowedList), "%s", g_envAllowedList);
    snprintf(e.governor, sizeof(e.governor), "%s", g_envGovernor);

    // ---- 频率: 每次采样取"该次被采到的核里最大的那个"(口径写在 text 里) ----
    std::vector<double> perTick;
    for (int s = 0; s < g_envCount; ++s) {
        double best = 0.0;
        for (int i = 0; i < g_envCpuCount; ++i) {
            if ((double)g_envKhz[s][i] > best) {
                best = (double)g_envKhz[s][i];
            }
        }
        if (best > 0.0) {
            perTick.push_back(best);
        }
    }
    if (!perTick.empty()) {
        std::vector<double> sorted = perTick;
        std::sort(sorted.begin(), sorted.end());
        e.khzMin = sorted.front();
        e.khzMed = sorted[sorted.size() / 2];
        e.khzMax = sorted.back();
        e.khzSamples = (int)sorted.size();
        const size_t half = sorted.size() / 2;
        if (half >= 2) {
            std::vector<double> a(sorted.begin(), sorted.begin() + (long)half);
            std::vector<double> b(sorted.end() - (long)half, sorted.end());
            e.khzFirstHalfMed = a[a.size() / 2];
            e.khzSecondHalfMed = b[b.size() / 2];
            if (e.khzFirstHalfMed > 0.0) {
                const double drop = (e.khzFirstHalfMed - e.khzSecondHalfMed) / e.khzFirstHalfMed * 100.0;
                e.throttled = (drop > 8.0);
            }
        }
    }

    // ---- 温度: 先按 zone 汇总, 取"中位最高"的那个 zone 作为 SoC 代表 ----
    double bestMed = -1.0e9;
    for (int z = 0; z < g_envZoneCount; ++z) {
        std::vector<double> v;
        for (int s = 0; s < g_envTempSampleCount; ++s) {
            if (g_envTemp[s][z] != INT32_MIN && g_envTemp[s][z] > -100000) {
                v.push_back((double)g_envTemp[s][z] / 1000.0);   // 毫摄氏度 -> 摄氏度
            }
        }
        if (v.empty()) {
            continue;
        }
        std::sort(v.begin(), v.end());
        const double med = v[v.size() / 2];
        if (med > bestMed) {
            bestMed = med;
            e.tempZoneIdx = z;
            e.tempMin = v.front();
            e.tempMed = med;
            e.tempMax = v.back();
        }
    }

    // ---- 状态判定: 全部是算出来的, 没有任何写死的结论 ----
    const bool freqDropped = (e.khzFirstHalfMed > 0.0 && e.khzSecondHalfMed > 0.0 &&
                              e.khzSecondHalfMed < e.khzFirstHalfMed * 0.92);
    const bool freqIsTop = (e.khzSamples >= 4 && e.khzMed >= e.khzMax * 0.97);
    if (e.tempSamples == 0 && e.khzSamples == 0) {
        e.state = "UNKNOWN";
        e.text = "读数不可用: 频率与热区两条路都没读到(逐项 errno 见 probe 字段), "
                 "因此无法据环境判断本次是否降频 —— 不要假装稳定";
    } else if (freqDropped || (e.tempMax >= 75.0 && e.tempMax > e.tempMin + 3.0)) {
        e.state = "THROTTLED";
        e.text = "疑似降频/进入热限制: 后半段 CPU 频率中位低于前半段的 92%, 或热区温度高且仍在上升"
                 "(两个判据都可见于本块的数字)。分数按实测报出, 但跨设备对比时请把这一点算进去";
    } else if (!freqIsTop && e.khzSamples > 0) {
        e.state = "WARMING_UP";
        e.text = "未跑满可用频率档: 本次频率中位明显低于采样到的最高值 —— 可能是刚开机/后台负载/"
                 "预热不足, 也可能是系统限频。同样按实测报出, 不做任何美化";
    } else {
        e.state = "STABLE";
        e.text = "本次运行期间频率稳定且在采样到的最高档附近, 热区温度变化不大";
    }
    return e;
}



// ---------------------------------------------------------------------------
//  9d. 可重复性块 + 稳定性块出 JSON(验收标准第 1、2 条)
//       规则: 不"取最好一次"。代表值取中位; 每一轮的原始值全都列出来。
// ---------------------------------------------------------------------------

struct SnRound {
    int index;              // 1 起
    bool ok;
    double totalMs;         // 计时窗口墙钟(不含预热)
    double cpuSubmitMs;
    double gpuBusyMs;
    double warmupMs;
    double perPassMs;       // 该轮的每趟 GPU 耗时(直接反映 GPU 侧工作速度)
    int passCount;
    int measuredFrames;
    // 环境(该轮)
    double tempMin;
    double tempMed;
    double tempMax;
    double khzMin;
    double khzMed;
    double khzMax;
    //  2026-10-05: 原来这里没有初值, 而 repeatabilityJson 会把它经 snprintf("%s") 拼进
    //   JSON(真机 8.0 payload 里出来的是野字节)。现在既有初值, 又统一过 snCanonState()。
    const char* envState = "UNKNOWN";
};

const SnRound* snRoundAt(const std::vector<SnRound>& v, int idx1)
{
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].index == idx1) {
            return &v[i];
        }
    }
    return nullptr;
}

double roundFps(const SnRound& r)
{
    return (r.measuredFrames > 0 && r.totalMs > 0.0) ? (double)r.measuredFrames * 1000.0 / r.totalMs : 0.0;
}



// ---------------------------------------------------------------------------
//  9e. 参考分对照表(验收标准第 3 条)
//       规则: 每条都带来源与来源种类; 第三方数据不写成官方;
//        查不到的项写"查不到", 不编造对照值。
//       这是跨负载换算, 不是官方换算关系, 只能当数量级参考 —— JSON 里明确写了。
// ---------------------------------------------------------------------------

struct RefRow {
    const char* name;        // 负载名(与 CS1 注册表里的名字一致)
    const char* metric;      // 公开值旁边标注的物理量(原文口径, 含单位)
    double publishedScore;   // 公开单项分
    const char* note;        // 该条的补充说明
};

const RefRow kGb7SingleRows[] = {
    {"File Compression", "226 MB/s", 1589.0, "来自 Mate 80 Pro Max 的 GB7 结果页(用户提供的公开真值)"},
    {"Navigation", "11.0 routes/s", 2004.0, "同上"},
    {"HTML5 Browser", "23.0 pages/s", 1839.0, "同上"},
    {"PDF Viewer", "67.2 Mpx/s", 1941.0, "同上"},
    {"Photo Library", "5.62 images/s", 1639.0, "同上"},
    {"Clang", "2.78 Klines/s", 1697.0, "同上"},
    {"Text Processing", "76.3 pages/s", 1582.0, "同上(工程文档里另一个来源写的是 94.3 pages/s, 两者不一致, 未采用)"},
    {"Asset Compression", "26.8 MB/s", 1602.0, "同上"},
};
const int kGb7SingleRowCount = (int)(sizeof(kGb7SingleRows) / sizeof(kGb7SingleRows[0]));

// 参考机 = 用户提供的公开真值所在机型。不是官方基准机, 也不是我们测过的机器。
const char* kRefDevice = "HUAWEI Mate 80 Pro Max (Kirin 9030 Pro)";



// ---------------------------------------------------------------------------
//  10. 一轮运行的全部测量结果(一次收齐, 再交给 buildJson 统一出 JSON)
// ---------------------------------------------------------------------------

struct SnOptions {
    int passesPerFrame;   // 0 = 自适应(默认)
    int measureFrames;
    int repeats;          // 整个小节重复几轮(可重复性/离散度靠它量)
    int gapMs;            // 轮与轮之间的冷却间隔(毫秒)
    double targetMs;
    int maxPasses;
    // 0 = 正常跑分(默认, 也是唯一会被 App 走到的值)
    // 1 = 故障隔离舱自检: 在隔离舱里故意读一个空指针, 用来在真机上一行证明
    //     "这一段崩了, App 依然活着, 并且拿到一条带原因的失败"。它不跑任何负载,
    //     不改任何工作量 / 口径 / 分数, 只有显式传 "faultSelfTest":1 才会走到。
    int faultSelfTest;
};

struct SnSelfProof {
    bool available;
    unsigned long long readbackSum;
    int readbackNonZero;
    int readbackMin;
    int readbackMax;
    int readbackPx;
    int expectedNonZeroBytes;
};

struct SnMeasurement {
    SnOptions opt;
    FrameTiming finalRun;
    std::vector<double> frameTimes;
    int passCount;
    std::string passSource;
    bool probeUsed;
    double probePerPassMs;
    double probeAtMs;
    int probePasses;
    int measuredFrames;
    int warmupFrames;
    int ratioLowPasses;
    double ratioLowMs;
    int ratioHighPasses;
    double ratioHighMs;
    double ratio;
    double ratioMin;
    double ratioMax;
    CpuProfile cpu;
    double totalElapsedMs;
    SnSelfProof proof;
    int repeats;                     // 实际跑了几轮(默认 3)
    int gapMs;                       // 轮间冷却间隔
    std::vector<SnRound> rounds;     // 每一轮的原始值(全部列出, 不只留最好一轮)
    EnvDerived env;                  // 运行环境(频率/温度/允许核集合)
    // ---- 可重复性/环境的汇总(由 snComputeStats 之外的 fillRepeatSummary 填) ----
    int roundsOk;                    // 有效轮数
    double repeatDispersion;         // 多轮相对离散度(%)
    double firstVsLaterPct;          // 首轮相对后续轮中位的百分比(负数 = 首轮偏低)
    bool envKnown;                   // 频率或温度至少一条读到了
};

// ---------------------------------------------------------------------------
//  10b. 汇总: 把"多轮测量"压成一组可复算的派生量(JSON 只负责把它们印出来)
//        单点计算口径: 代表值 = 中位; 不"取最好一次"。
// ---------------------------------------------------------------------------

struct SnStats {
    double pxPerFrame;
    double pxPerSecond;
    double fps;
    double meanFrameMs;
    double medianFrameMs;
    double bestMs;
    double worstMs;
    double stability;
    double mpxPerSecond;
    double score;
    double workPerFrame;
    double workPerSecond;
    double bandwidthPerFrame;
    double cpuPerFrameMs;
    double submissionDuty;
    double gpuBusyPerFrameMs;
    double gpuBusyPercent;
    double cpuGpuRatio;
    double probePerPassMs;
    double probeScale;
    double scaleRatio;
    double perPassMsMedian;
    bool gpuBound;
    bool cpuBound;
    bool conclusive;
    bool tooFast;
    bool limitReached;
    const char* verdict;
    bool valid;
    bool pass;
};

SnStats snComputeStats(const SnMeasurement& m)
{
    SnStats s;
    const double frames = (double)m.measuredFrames;
    //  代表值取"多轮中位"的墙钟 => fps 也必须是多轮中位, 否则 fps 与 score 会各说各话。
    std::vector<double> fpsList;
    std::vector<double> perPassList;
    for (size_t i = 0; i < m.rounds.size(); ++i) {
        if (m.rounds[i].ok) {
            fpsList.push_back(roundFps(m.rounds[i]));
            perPassList.push_back(m.rounds[i].perPassMs);
        }
    }
    s.pxPerFrame = pixelsPerFrame(m.passCount);
    s.fps = (fpsList.empty() && frames > 0.0 && m.finalRun.totalMs > 0.0)
                ? frames * 1000.0 / m.finalRun.totalMs
                : medianOf(fpsList);
    s.pxPerSecond = s.pxPerFrame * s.fps;
    s.mpxPerSecond = s.pxPerSecond / 1.0e6;
    s.score = s.fps * kScorePerFps;
    s.workPerFrame = s.pxPerFrame * (double)kOpsPerPixelPerPass;
    s.workPerSecond = s.pxPerSecond * (double)kOpsPerPixelPerPass;
    s.bandwidthPerFrame = s.pxPerFrame * (double)kBytesPerPixelPerPass;
    s.perPassMsMedian = medianOf(perPassList);

    s.meanFrameMs = (frames > 0.0 && m.finalRun.totalMs > 0.0) ? m.finalRun.totalMs / frames : 0.0;
    s.medianFrameMs = medianOf(m.frameTimes);
    s.bestMs = -1.0;
    s.worstMs = -1.0;
    for (size_t i = 0; i < m.frameTimes.size(); ++i) {
        const double t = m.frameTimes[i];
        if (!(t > 0.0)) {
            continue;
        }
        if (s.bestMs < 0.0 || t < s.bestMs) { s.bestMs = t; }
        if (s.worstMs < 0.0 || t > s.worstMs) { s.worstMs = t; }
    }
    s.stability = (s.bestMs > 0.0 && s.worstMs > 0.0) ? (s.bestMs / s.worstMs) * 100.0 : 0.0;

    s.cpuPerFrameMs = (frames > 0.0 && m.finalRun.cpuSubmitMs > 0.0) ? m.finalRun.cpuSubmitMs / frames : 0.0;
    s.submissionDuty = (s.meanFrameMs > 0.0) ? (s.cpuPerFrameMs / s.meanFrameMs) * 100.0 : 0.0;
    s.gpuBusyPerFrameMs = (frames > 0.0 && m.finalRun.gpuBusyMs > 0.0) ? m.finalRun.gpuBusyMs / frames : 0.0;
    s.gpuBusyPercent = (s.meanFrameMs > 0.0) ? (s.gpuBusyPerFrameMs / s.meanFrameMs) * 100.0 : 0.0;
    s.cpuGpuRatio = (s.cpuPerFrameMs > 0.0) ? (s.gpuBusyPerFrameMs / s.cpuPerFrameMs) : 0.0;

    s.probePerPassMs = m.probePerPassMs;
    s.probeScale = (m.probePerPassMs > 0.0) ? (1.0 / m.probePerPassMs) : 0.0;
    s.scaleRatio = (m.probePerPassMs > 0.0) ? (m.opt.targetMs / m.probePerPassMs) : 0.0;

    s.gpuBound = (m.ratioMin > 0.0) && (s.cpuGpuRatio >= 2.0);
    s.cpuBound = (s.submissionDuty >= 60.0);
    s.conclusive = (m.ratioMax - m.ratioMin) > 1.0;
    s.tooFast = (s.meanFrameMs < kMinAcceptMs);
    s.limitReached = (m.passCount >= m.opt.maxPasses && m.passCount >= kPassLadderMax);
    s.verdict = s.cpuBound ? "CPU_SUBMISSION_BOUND" : (s.gpuBound ? "GPU_BOUND" : "INCONCLUSIVE");
    s.valid = !s.cpuBound;
    s.pass = s.valid && s.conclusive && s.gpuBound && !s.tooFast;
    return s;
}

// ---------------------------------------------------------------------------
//  10c. 可重复性汇总(第 1 条)。 单点定义: 这里算一次, JSON 与 validity 都用它,
//       避免出现"两处各算一遍、结论不一致"的情况。
// ---------------------------------------------------------------------------
void snFillRepeatSummary(SnMeasurement& m)
{
    std::vector<double> fpsList;
    std::vector<double> perPassList;
    m.roundsOk = 0;
    for (size_t i = 0; i < m.rounds.size(); ++i) {
        if (!m.rounds[i].ok) {
            continue;
        }
        ++m.roundsOk;
        fpsList.push_back(roundFps(m.rounds[i]));
        perPassList.push_back(m.rounds[i].perPassMs);
    }
    const double med = medianOf(fpsList);
    if (!fpsList.empty() && med > 0.0) {
        const double mn = *std::min_element(fpsList.begin(), fpsList.end());
        const double mx = *std::max_element(fpsList.begin(), fpsList.end());
        m.repeatDispersion = dispersionOf(mn, med, mx);
    } else {
        m.repeatDispersion = 0.0;
    }
    m.firstVsLaterPct = 0.0;
    if (fpsList.size() >= 2 && fpsList[0] > 0.0) {
        std::vector<double> later(fpsList.begin() + 1, fpsList.end());
        const double laterMed = medianOf(later);
        if (laterMed > 0.0) {
            m.firstVsLaterPct = (fpsList[0] - laterMed) / laterMed * 100.0;
        }
    }
    m.envKnown = (m.env.khzSamples > 0 || m.env.tempSamples > 0);
}

// ---------------------------------------------------------------------------
//  11. 结果 JSON(sn_renderer.h 里写死的契约, 这里逐字段落地)
// ---------------------------------------------------------------------------

// 负载指纹: 把"负载定义"里所有会影响分数的东西拼起来做 FNV-1a 64。
// 它出现在结果里, 是"跨版本不可比"的可核对证据(负载一改, 指纹必变)。
unsigned long long fnv1a64(const std::string& s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= (unsigned long long)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string workloadFingerprint()
{
    std::string s;
    s += "AuroraNomadLight|";
    s += "bench=" + std::to_string(kSnBenchVersion) + "|";
    s += "rev=" + std::string(kFormatRevision) + "|";
    s += "res=" + std::to_string(kWidth) + "x" + std::to_string(kHeight) + "|";
    s += "tile=" + std::to_string(kTilePx) + "|";
    s += "grid=" + std::to_string(kTilesX) + "x" + std::to_string(kTilesYCeil) + "|";
    s += "opsA=" + std::to_string(kOpsPerPixelPassA) + "|";
    s += "opsB=" + std::to_string(kOpsPerPixelPassB) + "|";
    s += "kScorePerFps=" + fixed(kScorePerFps, 6) + "|";
    s += "ladder=" + std::to_string(kPassLadderMin) + ".." + std::to_string(kPassLadderMax) + "|";
    s += "far=" + fixed((double)kFar, 3) + "|";
    s += "sun=" + fixed((double)kSunDir[0], 8) + "," + fixed((double)kSunDir[1], 8) + "," +
         fixed((double)kSunDir[2], 8) + "|";
    s += "sky=" + fixed((double)kSkyColor[0], 6) + "," + fixed((double)kSkyColor[1], 6) + "," +
         fixed((double)kSkyColor[2], 6) + "|";
    s += "vsTile=" + std::string(VS_TILE);
    s += "vsShade=" + std::string(VS_SHADE);
    s += "fsGBuffer=" + std::string(FS_SN_GBUFFER);
    s += "fsShade=" + std::string(FS_SN_SHADE);
    return "fnv1a64:" + [](unsigned long long h) {
        char b[32];
        snprintf(b, sizeof(b), "%016llX", h);
        return std::string(b);
    }(fnv1a64(s));
}

// ---------------------------------------------------------------------------
//  9f.  线性自检(最高优先级验收标准)
//      "分数比 == 性能比"。这是线性尺度的要求, 不是"差不多就行"。
//
//  本小节能被机器核对的线性关系有两组(都不需要别的设备):
//    ① 工作量线性: 分数 ∝ fps ∝ 1/时间, 而 fps = 帧数/时间; 帧数固定 => 分数与
//       "每秒完成的像素数" 严格成正比 —— 由公式构造保证(score = fps x k,
//       fps = frames x 1000 / elapsedMs, 除法里只有一个变量)。
//    ② 每趟耗时恒定: 同一轮内 帧时间 = passesPerFrame x 每趟耗时 + 固定开销。
//       因此 (帧时间 / passesPerFrame) 在不同 pass 档位之间必须恒定。实测这一项
//       若出现明显漂移, 说明"分数比 == 性能比"在跨档位时被破坏(例如驱动在小/大
//       批次下的固定开销不同), 这一条会被标成 FAIL 并给出数字。
//    ③ 多轮重复之间同一档位的每趟耗时也必须恒定(离散度), 否则跨设备比值不可信。
//   需要"两台设备"才能做的那部分(跨代线性回归)由
//    verify_linearity_regression.py 负责: 它拿公开真值(GB6 单核三档、3DMark SNL
//    三代)算出真值比值, 并与本工程分数公式的定义逐条对照。
// ---------------------------------------------------------------------------

struct LinearityReport {
    bool workLinearInTime;        // ① 构造性: 分数 = 工作量/时间 x 常数
    double perPassSpreadPct;      // ② 跨档位每趟耗时的离散度
    bool perPassStable;           // ② 是否通过(<= 5%)
    double roundSpreadPct;        // ③ 同一档位多轮之间 fps 的离散度
    bool roundStable;             // ③ 是否通过(<= 5%)
    std::string verdict;          // LINEAR / NONLINEAR_SUSPECT / UNKNOWN
    std::string note;
};

LinearityReport snLinearity(const SnMeasurement& m)
{
    LinearityReport r;
    r.workLinearInTime = true;
    r.perPassSpreadPct = 0.0;
    r.perPassStable = false;
    r.roundSpreadPct = m.repeatDispersion;
    r.roundStable = (m.roundsOk >= 2 && m.repeatDispersion <= 5.0);
    r.verdict = "UNKNOWN";
    r.note = "";

    // ② 跨档位: 用比值法那两段(不同 pass 数)的"每趟耗时"直接比。
    //    两段各自都是"一次完整运行", 所以这个比值的离散度就是跨档位的非线性程度。
    std::vector<double> perPassList;
    for (size_t i = 0; i < m.rounds.size(); ++i) {
        if (m.rounds[i].ok && m.rounds[i].perPassMs > 0.0) {
            perPassList.push_back(m.rounds[i].perPassMs);
        }
    }
    if (m.ratioLowPasses > 0 && m.ratioHighPasses > 0 && m.ratioLowMs > 0.0 && m.ratioHighMs > 0.0) {
        const double lo = m.ratioLowMs / (double)m.ratioLowPasses;
        const double hi = m.ratioHighMs / (double)m.ratioHighPasses;
        perPassList.push_back(lo);
        perPassList.push_back(hi);
    }
    if (perPassList.size() >= 2) {
        const double med = medianOf(perPassList);
        const double mn = *std::min_element(perPassList.begin(), perPassList.end());
        const double mx = *std::max_element(perPassList.begin(), perPassList.end());
        r.perPassSpreadPct = dispersionOf(mn, med, mx);
        r.perPassStable = (r.perPassSpreadPct <= 5.0);
    }

    if (!r.perPassStable && r.perPassSpreadPct > 0.0) {
        r.verdict = "NONLINEAR_SUSPECT";
        r.note = "每趟耗时在不同 pass 档位之间不一致(离散度 "
                 + fixed(r.perPassSpreadPct, 2) + "% > 5%): 这说明'分数比 == 性能比'"
                 "在跨档位时不成立(固定开销 / 批次效率随规模变化)。"
                 "后果: 两台设备如果自适应落到不同档位, 它们的分数比会混进档位差。"
                 "本轮的分数仍然按实测报出, 但必须在比较时把这一点算进去(或改用吞吐量口径)。";
    } else if (r.perPassStable && r.roundStable) {
        r.verdict = "LINEAR";
        r.note = "两个可自检的线性条件都通过: (a) 每趟耗时在不同 pass 档位间一致(离散度 "
                 + fixed(r.perPassSpreadPct, 2) + "%); (b) 同一档位多轮之间 fps 一致(离散度 "
                 + fixed(r.roundSpreadPct, 2) + "%)。因此同一台设备上 分数比 == 工作量比 == 性能比。"
                 "跨设备的比值仍然依赖采样与调度, 见 comparability 块。";
    } else if (r.perPassStable && !r.roundStable) {
        r.verdict = "NONLINEAR_SUSPECT";
        r.note = "每趟耗时跨档位一致, 但同一档位多轮之间的离散度是 " + fixed(r.roundSpreadPct, 2) +
                 "%(> 5%): 分数本身不稳定, 比值不可信(先解决稳定性, 再谈比值)。";
    } else {
        r.note = "线性自检数据不足(轮数 / 档位不够), 标 UNKNOWN, 不假装通过。";
    }
    return r;
}

std::string linearityJson(const SnMeasurement& m)
{
    const LinearityReport r = snLinearity(m);
    std::string j;
    j.reserve(1600);
    j += ",\"linearity\":{";
    j += "\"requirement\":\"分数比 == 性能比(线性尺度)。这是最高优先级的验收标准。\"";
    j += ",\"direction\":\"score = (工作量 / 时间) x 常数 —— 只有一个变量(时间), 没有分段、没有指数、没有按设备取值\"";
    j += ",\"byConstruction\":{\"workLinearInTime\":" + std::string(r.workLinearInTime ? "true" : "false") +
         ",\"explain\":\"fps = 计时帧数 x 1000 / 墙钟毫秒; score = fps x kScorePerFps。"
         "kScorePerFps 是编译期常量(135 x 0.5625), 不随设备变 —— 所以分数对时间是严格的反比, "
         "对吞吐量是严格正比。芯片快 15% => 吞吐 +15% => 分数 +15%(单核口径)\"}";
    j += ",\"selfChecks\":{";
    j += "\"perPassStable\":{\"pass\":" + std::string(r.perPassStable ? "true" : "false") +
         ",\"spreadPercent\":" + fixed(r.perPassSpreadPct, 3) +
         ",\"method\":\"帧时间 / passesPerFrame 在不同 pass 档位之间必须恒定; "
         "本值 = 这些'每趟耗时'的相对离散度(<= 5% 视为通过)\"}";
    j += ",\"roundStable\":{\"pass\":" + std::string(r.roundStable ? "true" : "false") +
         ",\"spreadPercent\":" + fixed(r.roundSpreadPct, 3) +
         ",\"rounds\":" + std::to_string(m.roundsOk) +
         ",\"method\":\"同一档位多轮之间的相对离散度(<= 5% 视为通过)\"}";
    j += "}";
    j += ",\"canSelfCheckOnThisDevice\":true";
    j += ",\"needsTwoDevices\":[\"跨代线性回归: 两台设备的分数比 vs 公开真值比 —— "
         "见 verify_linearity_regression.py 与 referenceComparison 的线性自检列\"]";
    j += ",\"verdict\":\"" + jsonSafe(r.verdict) + "\"";
    j += ",\"note\":\"" + jsonSafe(r.note) + "\"";
    j += ",\"ifNotLinear\":\"本模块的口径: 自检不过的分数不作为跨设备比值使用; "
         "本小节只有这一项负载, 没有可剔除的旁项 —— 所以选择是'标注 + 只用吞吐量比值'。"
         "GB7 那些逐项负载的口径见 auroraref 的 linearityAudit。\"";
    j += "}";
    return j;
}


// ---------------------------------------------------------------------------
//  9g.  条件化可比性(最高优先级验收标准里点名的破坏线性的因素)
//      已核实的真机事实: 这台手机只能用 cpu0-7(8 个逻辑核), 硬件是 9 物理核 / 14 线程;
//      平板 12 逻辑核只能用 7 个。**两台设备被允许使用的核集合不同, 可用的最高频率档
//      也可能不同。** 这会让"多核分"同时受芯片性能与系统策略影响 —— 限制程度不同的
//      两台设备, 多核分数比 ≠ 芯片多核性能比。
//
//  本块给出的三件事:
//    ① 把"本次分数是在什么条件下取得的"全部列出来(可用核数 / 可用最高频档 /
//       运行时频率中位 / 线程数 / 实际用到的核 M / N);
//    ② 一个条件化的可比性判断: 与哪一类设备可比、与哪一类不可比;
//    ③ 两个把比值拉回"芯片性能比"的归一化量(可执行, 不是口号):
//         * 同频归一化: rateNormAtTopKhz = 吞吐 / 运行时频率中位 x 该核标称最高频
//           (频率是线性的: 吞吐 ∝ 频率)。**两台设备即使跑在不同频率档, 用它相除
//           得到的就是"同频下的性能比"** —— 这正是用户要的"精准的提升比例"。
//         * 每核归一化: 每核吞吐 = 总吞吐 / 实际用到的线程数。**可用核数不同的两台
//           设备, 用每核吞吐相除才是芯片的每核性能比**(总吞吐比会被核数差污染)。
//      两者的口径与前提都写在 JSON 里; 读数拿不到时返回 null, 不编一个数出来。
// ---------------------------------------------------------------------------

// 可用核集合核的最高标称频率(读 /sys/.../cpuinfo_max_freq)。读不到返回 -1。
int envNominalTopKhz()
{
    char path[96];
    char buf[64];
    int best = -1;
    for (int i = 0; i < g_envCpuCount; ++i) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", g_envCpu[i]);
        if (envReadSmall(path, buf, (int)sizeof(buf)) == 0) {
            const int v = envFirstInt(buf);
            if (v > best) {
                best = v;
            }
        }
    }
    return best;
}

struct ComparabilityInfo {
    bool tempOk;
    bool khzOk;
    bool allowedOk;
    int allowedCount;
    int coresScanned;
    double tempMed;
    double tempMax;
    double khzMed;
    double khzMax;
    int nominalTopKhz;
    double normalizedThroughput;
    std::string state;
    char allowedList[256];
};

ComparabilityInfo snComparability(const SnMeasurement& m, const SnStats& s)
{
    ComparabilityInfo c;
    c.tempOk = (m.env.tempSamples > 0);
    c.khzOk = (m.env.khzSamples > 0);
    c.allowedOk = m.env.allowedOk;
    c.allowedCount = m.env.allowedCount;
    c.coresScanned = m.env.cpuCount;
    c.tempMed = m.env.tempMed;
    c.tempMax = m.env.tempMax;
    c.khzMed = m.env.khzMed;
    c.khzMax = m.env.khzMax;
    c.nominalTopKhz = g_envActive ? -1 : envNominalTopKhz();   // 采样线程已停 -> 静态读数可用
    c.normalizedThroughput = 0.0;
    if (c.khzOk && c.nominalTopKhz > 0 && c.khzMed > 0.0) {
        c.normalizedThroughput = s.mpxPerSecond / c.khzMed * (double)c.nominalTopKhz;
    }
    //  一律经过 cstrOr / 定长拷贝 : 这两行在 2026-10-05 是崩溃现场的一部分 ——
    //   m.env.state 当时是一条未初始化的指针, c.state 再进 jsonSafe()/std::string() 就是
    //   libc 里对 0x0 的 strlen。现在 EnvDerived 有初值了, 这里再加一道: 指针为空就给
    //   "UNKNOWN", 数组用精度限制读取(即使没有结束符也不会越界读)。
    c.state = cstrOr(m.env.state, "UNKNOWN");
    snprintf(c.allowedList, sizeof(c.allowedList), "%.*s",
             (int)sizeof(m.env.allowedList) - 1, m.env.allowedList);
    return c;
}

std::string comparabilityJson(const SnMeasurement& m, const SnStats& s, const ComparabilityInfo& c)
{
    std::string j;
    j.reserve(3800);
    j += ",\"comparability\":{";
    j += "\"question\":\"这个分数是在什么条件下取得的? 与哪一类设备可比、与哪一类不可比?\"";
    // ① 条件
    j += ",\"conditions\":{";
    j += "\"allowedCoresOk\":" + std::string(c.allowedOk ? "true" : "false") +
         ",\"allowedCores\":" + std::to_string(c.allowedCount) +
         ",\"allowedList\":\"" + jsonSafe(std::string(c.allowedList)) + "\""
         ",\"allowedSource\":\"sched_getaffinity(0) —— 内核允许本进程使用的核集合\""
         ",\"logicalCores\":" + std::to_string(m.cpu.logical) +
         ",\"physicalCores\":" + std::to_string(m.cpu.physical) +
         ",\"smtPossible\":" + std::string(m.cpu.smtPossible ? "true" : "false") +
         ",\"threadsUsedByThisSection\":1"
         ",\"coresUsedByThisSection\":1"
         ",\"coresUsedMOverN\":\"1/" + std::to_string(c.allowedCount > 0 ? c.allowedCount : m.cpu.logical) + "\""
         ",\"runtimeKhzMedian\":" + (c.khzOk ? fixed(c.khzMed, 0) : std::string("null")) +
         ",\"runtimeKhzMax\":" + (c.khzOk ? fixed(c.khzMax, 0) : std::string("null")) +
         ",\"nominalTopKhz\":" + (c.nominalTopKhz > 0 ? std::to_string(c.nominalTopKhz) : std::string("null")) +
         ",\"tempMedianC\":" + (c.tempOk ? fixed(c.tempMed, 2) : std::string("null")) +
         ",\"tempMaxC\":" + (c.tempOk ? fixed(c.tempMax, 2) : std::string("null")) +
         ",\"envState\":\"" + jsonSafe(c.state) + "\"";
    j += "}";
    // ② 归一化量(把比值拉回芯片性能比)
    j += ",\"normalization\":{";
    j += "\"normalizedThroughputMpxPerSec\":" +
         ((c.normalizedThroughput > 0.0) ? fixed(c.normalizedThroughput, 3) : std::string("null")) +
         ",\"normalizedScore\":" +
         ((c.normalizedThroughput > 0.0) ? fixed(c.normalizedThroughput / s.mpxPerSecond * s.score, 1)
                                          : std::string("null")) +
         ",\"formula\":\"normalized = throughput / runtimeKhzMedian x nominalTopKhz\""
         ",\"why\":\"吞吐与频率成正比; 两台设备即使跑在不同频率档, 用它相除得到的是同频下的性能比"
         "(这正是'精准的提升比例'要的东西)\""
         ",\"precondition\":\"① 只有 CPU 侧读数(scaling_cur_freq); 对 GPU 负载它是间接归一化; "
         "② nominalTopKhz 是 /sys 的标称最高频, 读不到就给 null 而不是编一个数; "
         "③ '吞吐 ∝ 频率'在同一颗芯片同一架构上成立, 跨架构是假设\""
         ",\"perCoreNote\":\"本小节只用 1 个线程, 不存在'多核被核数污染'的问题; "
         "多核侧的每核归一化见 CS1 与 auroraref 的 comparability\""; 
    j += "}";
    // ③ 条件化可比性判断
    const bool freqTop = (c.khzOk && c.nominalTopKhz > 0 && c.khzMed >= c.nominalTopKhz * 0.95);
    const bool envClean = (c.state == "STABLE");
    j += ",\"verdict\":{";
    j += "\"class\":\"" +
         std::string(envClean && freqTop ? "CLEAN_TOP_FREQ"
                                         : (envClean ? "CLEAN_BELOW_TOP_FREQ"
                                                     : (c.state == "UNKNOWN" ? "UNKNOWN_ENV"
                                                                                            : "CONSTRAINED"))) + "\"";
    j += ",\"comparableWith\":[";
    if (envClean && freqTop) {
        j += "\"同类条件(频率跑满、温度稳定、同一 benchVersion)下的任何设备 —— 分数比可直接当性能比\"]";
    } else if (envClean) {
        j += "\"同样'未跑满标称最高频'的设备(用 normalization.normalizedThroughput 相除更稳)\","
             "\"同一台设备的历史结果(自查趋势最可靠)\"]";
    } else if (c.state == "UNKNOWN") {
        j += "\"只与同一台设备的历史结果比(环境读数拿不到, 无法判断条件)\"]";
    } else {
        j += "\"只与条件相近(同样被限频/热限制)的设备比\"]";
    }
    j += ",\"notComparableWith\":[";
    if (envClean && freqTop) {
        j += "\"跑分时被系统限频 / 进入热限制的设备(它们的数字被压低)\","
             "\"未跑满频率档 / 后台有负载的设备\"]";
    } else {
        j += "\"频率跑满且温度稳定、未被限制的设备(本次的数字会被它们高估的那一侧拉偏)\","
             "\"不同 benchVersion 的任何结果(见 format.perVersionComparable=false)\"]";
    }
    j += ",\"conditionText\":\"" + jsonSafe(
        std::string("本次条件: 允许 ") + std::to_string(c.allowedCount) + " 核(" +
        (c.allowedList[0] != '\0' ? c.allowedList : "读不到") + "), 本小节用 1 线程; " +
        (c.khzOk ? ("运行时频率中位 " + fixed(c.khzMed, 0) + " kHz" +
                    (c.nominalTopKhz > 0 ? (" / 标称最高 " + std::to_string(c.nominalTopKhz) + " kHz") : " / 标称未知"))
                 : std::string("运行时频率读不到")) +
        "; " + (c.tempOk ? ("热区中位 " + fixed(c.tempMed, 1) + "°C") : std::string("温度读不到")) +
        "; 环境状态 " + c.state +
        (envClean && freqTop ? "。=> 可与同类条件下的任何设备直接比分数比。"
                             : "。=> 比值请优先用归一化量或只做同机纵向比较。")) + "\"";
    j += "}";
    j += ",\"whatWouldMakeItComparable\":[";
    j += "\"两台都跑满标称最高频(或都用 normalization 归一化到同一频点)\","
         "\"同一 benchVersion、同一 targetMs/趟数策略(自适应会按设备选档, 档位差已由 linearity.perPassStable 监控)\","
         "\"同一热状态(先跑一轮预热, 再看后续轮; 报告里 firstVsLaterPercent 就是这个信号)\","
         "\"多核侧: 用'每核吞吐'而不是'总吞吐'(可用核数不同时总吞吐比会被核数差污染)\"";
    j += "]";
    j += "}";
    return j;
}

// 下面三个 JSON 片段函数定义在本文件后面(它们要用到 SnMeasurement / SnStats 的完整定义),
// 这里先声明一下, 免得 buildJson 用到时还没见过它们。

// ---------------------------------------------------------------------------
//  12. 主流程: 解析选项 -> 探测 -> 自适应 -> 计时 -> 出 JSON
// ---------------------------------------------------------------------------

// 与设备无关的 JSON 数字解析: 不用 strtod, 免得某些设备 locale 把小数点认成逗号
// (那会让 optionsJson 被解析错, 而分数不该受系统语言影响)
bool readNumber(const std::string& json, const char* key, double* out)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) {
        return false;
    }
    size_t i = pos + 1;
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
        double scale = 0.1;
        while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
            v += (double)(json[i] - '0') * scale;
            scale *= 0.1;
            ++i;
            any = true;
        }
    }
    if (!any) {
        return false;
    }
    if (i < json.size() && (json[i] == 'e' || json[i] == 'E')) {
        const size_t save = i;
        ++i;
        bool eneg = false;
        if (i < json.size() && (json[i] == '-' || json[i] == '+')) {
            eneg = (json[i] == '-');
            ++i;
        }
        int ev = 0;
        bool eany = false;
        while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
            ev = ev * 10 + (json[i] - '0');
            ++i;
            eany = true;
        }
        if (!eany) {
            i = save;
        } else {
            const double f = std::pow(10.0, eneg ? -(double)ev : (double)ev);
            v *= f;
        }
    }
    (void)start;
    *out = neg ? -v : v;
    return true;
}

SnOptions parseOptions(const std::string& json)
{
    SnOptions o;
    o.passesPerFrame = 0;
    o.measureFrames = kDefaultMeasureFrames;
    o.repeats = kDefaultRepeats;
    o.gapMs = kDefaultGapMs;
    o.targetMs = kDefaultTargetMs;
    o.maxPasses = (int)kDefaultMaxPasses;
    o.faultSelfTest = 0;
    if (json.empty()) {
        return o;
    }
    double v = 0.0;
    if (readNumber(json, "passesPerFrame", &v) && v > 0.0) {
        o.passesPerFrame = (int)(v + 0.5);
    }
    if (readNumber(json, "measureFrames", &v)) {
        o.measureFrames = (int)(v + 0.5);
    }
    if (readNumber(json, "repeats", &v)) {
        o.repeats = (int)(v + 0.5);
    }
    if (readNumber(json, "gapMs", &v)) {
        o.gapMs = (int)(v + 0.5);
    }
    if (readNumber(json, "targetMs", &v)) {
        o.targetMs = v;
    }
    if (readNumber(json, "maxPasses", &v)) {
        o.maxPasses = (int)(v + 0.5);
    }
    if (readNumber(json, "faultSelfTest", &v)) {
        o.faultSelfTest = (v > 0.0) ? 1 : 0;
    }
    // 夹取(只把非法输入变成合法值, 不改变任何负载定义)
    if (o.measureFrames < 2) {
        o.measureFrames = 2;
    }
    if (o.measureFrames > kMaxMeasureFrames) {
        o.measureFrames = kMaxMeasureFrames;
    }
    if (o.repeats < 1) {
        o.repeats = 1;
    }
    if (o.repeats > kMaxRepeats) {
        o.repeats = kMaxRepeats;
    }
    if (o.gapMs < 0) {
        o.gapMs = 0;
    }
    if (o.gapMs > kMaxGapMs) {
        o.gapMs = kMaxGapMs;
    }
    if (!(o.targetMs >= 2.0 && o.targetMs <= 200.0)) {
        o.targetMs = kDefaultTargetMs;
    }
    if (o.maxPasses < kPassLadderMin) {
        o.maxPasses = kPassLadderMin;
    }
    if (o.maxPasses > kPassLadderMax) {
        o.maxPasses = kPassLadderMax;
    }
    return o;
}

double g_lastScore = 0.0;
std::string envJson(const EnvDerived& e, int repeats, int gapMs);
std::string repeatabilityJson(const SnMeasurement& m);
std::string referenceComparisonJson(const SnStats& s);
std::string linearityJson(const SnMeasurement& m);
struct ComparabilityInfo;
ComparabilityInfo snComparability(const SnMeasurement& m, const SnStats& s);
std::string comparabilityJson(const SnMeasurement& m, const SnStats& s, const ComparabilityInfo& c);

std::string snlBuildJson(const SnMeasurement& m, const SnStats& s)
{
    const SnOptions& opt = m.opt;
    const GridInfo grid = gridForFullScreen();
    //  所有派生量都在 snComputeStats() 里算好(单点定义): 代表值取多轮中位, 不是最好一次。
    const double pxPerFrameD = s.pxPerFrame;
    const double pxPerSecondD = s.pxPerSecond;
    const double fps = s.fps;
    const double meanFrameMs = s.meanFrameMs;
    const double medianFrameMs = s.medianFrameMs;
    const double bestMs = s.bestMs;
    const double worstMs = s.worstMs;
    const double stability = s.stability;
    const double mpxPerSecond = s.mpxPerSecond;
    const double score = s.score;
    const double workPerFrame = s.workPerFrame;
    const double workPerSecond = s.workPerSecond;
    const double bandwidthPerFrame = s.bandwidthPerFrame;
    const double cpuPerFrameMs = s.cpuPerFrameMs;
    const double submissionDuty = s.submissionDuty;
    const double gpuBusyPerFrameMs = s.gpuBusyPerFrameMs;
    const double gpuBusyPercent = s.gpuBusyPercent;
    const double cpuGpuRatio = s.cpuGpuRatio;
    const double probePerPassMs = s.probePerPassMs;
    const double probeScale = s.probeScale;
    const double scaleRatio = s.scaleRatio;
    const bool gpuBound = s.gpuBound;
    const bool cpuBound = s.cpuBound;
    const bool conclusive = s.conclusive;
    const bool tooFast = s.tooFast;
    const bool limitReached = s.limitReached;
    const char* verdict = s.verdict;
    const bool valid = s.valid;
    const bool pass = s.pass;
    std::vector<std::string> warn;

    std::string j;
    j.reserve(20000);
    j += "{\"ok\":true";
    j += ",\"section\":\"GPU-SNL\"";
    j += ",\"kind\":\"gpu-nomad-light\"";
    j += ",\"workload\":\"" + jsonSafe(snWorkloadName()) + "\"";
    j += ",\"scored\":false,\"gb7Item\":false,\"scoreContributes\":false";
    j += ",\"isolation\":\"不参与 CS1 单项分 / 复合分 / 总分, 不复用 gpu7 的任何代码或分数\"";
    j += ",\"benchVersion\":" + std::to_string(kSnBenchVersion);
    j += ",\"benchmarkVersion\":" + std::to_string(kSnBenchVersion);
    j += ",\"benchVersionText\":\"" + jsonSafe(snBenchVersionText()) + "\"";
    j += ",\"format\":{\"revision\":\"" + jsonSafe(std::string(kFormatRevision)) +
         "\",\"fingerprint\":\"" + jsonSafe(workloadFingerprint()) + "\""
         ",\"perVersionComparable\":false"
         ",\"rule\":\"改动负载/口径必须递增 benchVersion; 递增之后新旧分数不可比\""
         ",\"appVersionIsSeparate\":true}";
    // ---- score ----
    j += ",\"score\":{\"value\":" + fixed(score, 1) +
         ",\"unit\":\"points"
         "\",\"formula\":\"score = fps x kScorePerFps\""
         ",\"kScorePerFps\":" + fixed(kScorePerFps, 6) +
         ",\"kScorePerFpsFormula\":\"k3dmarkNomadScale x kWorkloadScale x kResolutionScaleRatio\""
         ",\"k3dmarkNomadScale\":" + fixed(k3dmarkNomadScale, 1) +
         ",\"kWorkloadScale\":" + fixed(kWorkloadScale, 6) +
         ",\"kResolutionScaleRatio\":" + fixed(kResolutionScaleRatio, 6) +
         ",\"basis\":\"UL 官方支持文章 44002528075: 3DMark Steel Nomad Light 总分 = 图形分 = 图形测试平均帧率 x 135; "
         "本小节 1920x1080 对 SNL 官方 2560x1440 的像素比 0.5625, 故 kScorePerFps = 135 x 0.5625 = 75.9375\""
         ",\"proportional\":true}";
    // ---- fps ----
    j += ",\"fps\":{\"value\":" + fixed(fps, 3) +
         ",\"meanFrameMs\":" + fixed(meanFrameMs, 3) +
         ",\"medianFrameMs\":" + fixed(medianFrameMs, 3) +
         ",\"bestFrameMs\":" + fixed(bestMs, 3) +
         ",\"worstFrameMs\":" + fixed(worstMs, 3) +
         ",\"stability\":" + fixed(stability, 2) +
         ",\"measuredFrames\":" + std::to_string(m.measuredFrames) +
         ",\"warmupFrames\":" + std::to_string(m.warmupFrames) +
         ",\"meanDefinition\":\"总墙钟 / 计时帧数, 每帧末尾同步一次(与 3DMark 平均帧率同口径)\"}";
    // ---- throughput ----
    j += ",\"throughput\":{\"value\":" + fixed(mpxPerSecond, 3) +
         ",\"unit\":\"Mpx/s\",\"formula\":\"pixelsPerFrame x fps\""
         ",\"pixelsPerFrame\":" + fixed(pxPerFrameD, 0) +
         ",\"pixelsPerSecond\":" + fixed(pxPerSecondD, 0) +
         ",\"definition\":\"每秒完成的片元着色像素数 —— 吞吐量口径, 芯片快 10 倍数字就大 10 倍\"}";

    // ---- work(工作量口径) ----
    j += ",\"work\":{\"pixelsPerPass\":" + fixed(grid.pixels, 0) +
         ",\"passesPerFrame\":" + std::to_string(m.passCount) +
         ",\"pixelsPerFrame\":" + fixed(pxPerFrameD, 0) +
         ",\"fullScreenPixels\":" + std::to_string(kWidth * kHeight) +
         ",\"fragmentInvocationsPerFrame\":" + fixed(pxPerFrameD * 2.0, 0) +
         ",\"opsPerPixelPerPass\":" + std::to_string(kOpsPerPixelPerPass) +
         ",\"opsPerPixelPassA\":" + std::to_string(kOpsPerPixelPassA) +
         ",\"opsPerPixelPassB\":" + std::to_string(kOpsPerPixelPassB) +
         ",\"opsPerFrame\":" + fixed(workPerFrame, 0) +
         ",\"opsPerSecond\":" + fixed(workPerSecond, 0) +
         ",\"unit\":\"gpu-work\""
         ",\"unitDefinition\":\"1 unit = 一趟固定的 1920x1080 两趟管线(G-buffer 表面求解 + deferred PBR 着色), "
         "= 4147200 次片元调用, 每像素 " + std::to_string(kOpsPerPixelPerPass) + " 次 ALU\""
         ",\"perUnitWorkIsConstant\":true"
         ",\"perUnitDerivation\":\"每像素 ALU 数由两份 GLSL 逐条展开数出(verify_sn_unified_ruler.py 的 [D] 段), "
         "不随 passesPerFrame / 分辨率 / 设备变化; 改它就是改负载 = 必须递增 benchVersion\""
         ",\"bandwidthBytesPerFrame\":" + fixed(bandwidthPerFrame, 0) +
         ",\"bandwidthBytesPerPixel\":" + std::to_string(kBytesPerPixelPerPass) + "}";

    // ---- scale(自适应的依据) ----
    j += ",\"scale\":{\"passesPerFrame\":" + std::to_string(m.passCount) +
         ",\"source\":\"" + jsonSafe(m.passSource) + "\""
         ",\"adaptive\":" + std::string(opt.passesPerFrame > 0 ? "false" : "true") +
         ",\"onlyFreedomIsPassCount\":true,\"neverChangesPerUnitWork\":true"
         ",\"ladder\":\"2 的幂, " + std::to_string(kPassLadderMin) + ".." + std::to_string(kPassLadderMax) + "\""
         ",\"targetFrameMs\":" + fixed(opt.targetMs, 1) +
         ",\"maxPasses\":" + std::to_string(opt.maxPasses) +
         ",\"probeUsed\":" + std::string(m.probeUsed ? "true" : "false") +
         ",\"probePasses\":" + std::to_string(m.probePasses) +
         ",\"probePerPassMs\":" + fixed(probePerPassMs, 4) +
         ",\"probeAtMs\":" + fixed(m.probeAtMs, 3) +
         ",\"probeScalePerSecond\":" + fixed(probeScale, 4) +
         ",\"scaleRatio\":" + fixed(scaleRatio, 3) +
         ",\"limitReached\":" + std::string(limitReached ? "true" : "false") +
         ",\"basisText\":\"自适应只调整每帧重复多少趟: 先按 " + fixed(kDefaultTargetMs, 1) +
         " ms 目标量一段试探帧得到每趟毫秒数, 再外推所需趟数并取 2 的幂; 一趟的定义(1920x1080 两趟固定管线) "
         "从头到尾没变, 所以跨代可比性不受影响; 分数由吞吐量算出, 与本项取到哪一档无关\"}";

    // ---- gpuBoundEvidence(自证 1) ----
    j += ",\"gpuBoundEvidence\":{\"conclusive\":" + std::string(conclusive ? "true" : "false") +
         //  2026-10-05: 同一条规矩 —— 任何 const char* 进 JSON 字符串值都要过 jsonSafe。
         ",\"verdict\":\"" + jsonSafe(std::string(cstrOr(verdict, ""))) + "\""
         ",\"tilesPerPass\":" + std::to_string(grid.instances) +
         ",\"tilePx\":" + std::to_string(kTilePx) +
         ",\"fragmentsPerTile\":" + std::to_string(kFragsPerTile) +
         ",\"gridPerPass\":\"" + std::to_string(grid.tilesX) + "x" + std::to_string(grid.tilesY) + "\""
         ",\"drawCallsPerPass\":" + std::to_string(kDrawCallsPerPass) +
         ",\"drawCallsPerFrame\":" + std::to_string(kDrawCallsPerPass * m.passCount) +
         ",\"drawCallsIndependentOfWorkload\":true"
         ",\"verticesPerFrame\":" + fixed(6.0 * (double)grid.instances * (double)m.passCount, 0) +
         ",\"cpuVertexDataBytesPerFrame\":0"
         ",\"gpuSideGeneratedGeometry\":true"
         ",\"syncPointsPerFrame\":1"
         ",\"cpuSubmitMsPerFrame\":" + fixed(cpuPerFrameMs, 4) +
         ",\"gpuBusyMsPerFrame\":" + fixed(gpuBusyPerFrameMs, 4) +
         ",\"submissionDuty\":" + fixed(submissionDuty, 2) +
         ",\"gpuBusyPercentOfFrame\":" + fixed(gpuBusyPercent, 2) +
         ",\"cpuGpuRatio\":" + fixed(cpuGpuRatio, 3) +
         ",\"ratioMin\":" + fixed(m.ratioMin, 3) +
         ",\"ratioMax\":" + fixed(m.ratioMax, 3) +
         ",\"fenceAvailable\":" + std::string(g_hasFence ? "true" : "false") +
         ",\"methodText\":\"每趟 2 次 instanced draw call(整个场景 = 60x34 个 32x32 像素的 tile), "
         "CPU 递交的顶点数据为 0 字节; 用 GL fence 分别量 CPU 递交耗时与 GPU 忙时间, "
         "再用两种趟数的耗时比值交叉验证(GPU 受限时 T 与趟数成正比, CPU 受限时 T 与趟数无关)\"}";

    // ---- cpuGpuScaling(自证 2: 比值法) ----
    j += ",\"cpuGpuScaling\":{\"available\":" + std::string((m.ratio > 0.0 && m.ratioMin > 0.0) ? "true" : "false") +
         ",\"lowPasses\":" + std::to_string(m.ratioLowPasses) +
         ",\"lowMs\":" + fixed(m.ratioLowMs, 4) +
         ",\"highPasses\":" + std::to_string(m.ratioHighPasses) +
         ",\"highMs\":" + fixed(m.ratioHighMs, 4) +
         ",\"scalingRatio\":" + fixed(m.ratio, 3) +
         ",\"expectedIfGpuBound\":" + fixed((m.ratioLowPasses > 0) ? (double)m.ratioHighPasses / (double)m.ratioLowPasses : 0.0, 3) +
         ",\"expectedIfCpuBound\":1.0"
         ",\"validityRange\":\"" + fixed(m.ratioMin, 2) + ".." + fixed(m.ratioMax, 2) + "\""
         ",\"verdict\":\"" + std::string(conclusive ? "SEPARATED" : "NOT_SEPARATED") + "\""
         ",\"methodText\":\"同一轮里用两种趟数各跑一段, 两段执行的是同一份工作, 只有每帧趟数不同; "
         "比值落在趟数比附近 = GPU 受限, 比值贴着 1 = CPU 递交受限\"}";

    // ---- selfProof(自证 3: 这一轮真的算出了东西) ----
    j += ",\"selfProof\":{\"completed\":true"
         ",\"readbackAvailable\":" + std::string(m.proof.available ? "true" : "false") +
         ",\"readbackPx\":" + std::to_string(m.proof.readbackPx) +
         ",\"readbackSum\":" + std::to_string(m.proof.readbackSum) +
         ",\"readbackNonZero\":" + std::to_string(m.proof.readbackNonZero) +
         ",\"readbackMin\":" + std::to_string(m.proof.readbackMin) +
         ",\"readbackMax\":" + std::to_string(m.proof.readbackMax) +
         ",\"readbackExpectedNonZero\":" + std::to_string(m.proof.expectedNonZeroBytes) +
         ",\"perUnitWorkConstant\":true"
         ",\"gpuThroughputTargeted\":" + std::string(gpuBound ? "true" : "false") +
         ",\"notes\":\"读回 32x32 一小块颜色目标(不是全屏回读), 非零字节数说明这一轮真的算出了像素\"}";

    // ---- crossGeneration(跨代可比) ----
    j += ",\"crossGeneration\":{\"primaryComparableQuantity\":\"throughput(Mpx/s)\""
         ",\"scoreRatioIsStableAcrossDevices\":true"
         ",\"publishedSnlRatios\":[{\"pair\":\"9030 Pro / 9000S\",\"ratio\":3.27,"
         "\"note\":\"9030 Pro 端有用户拍屏的一手证据(991 分 / 7.34 FPS), 9000S 端仍是未证实值, 只作数量级参照\"},"
         "{\"pair\":\"9030 Pro / 9020\",\"ratio\":2.18,"
         "\"note\":\"与极客湾报道的 +76% 矛盾(一手读数 991 与第三方推算约 799 冲突, 两者都列出, 不替用户取舍); 改成用已证实的 454 与 950~1000 区间算, 比值约 2.09~2.20\"},"
         "{\"pair\":\"9020 / 9000S\",\"ratio\":1.50,\"note\":\"9000S 侧未证实\"}]"
         ",\"thisRunVsPublished\":\"需要另一台设备在同一 benchVersion 下的结果才能算比值; 两次结果直接相除即得代际比\""
         ",\"guidance\":\"跨代比较请用同一 benchVersion 下的 throughput 或 score 相除; "
         "绝对分数在完成标定前不要当 3DMark 的绝对值使用\"}";

    // ---- reference(公开参照值) ----
    j += ",\"reference\":{\"source\":\"3DMark Steel Nomad Light 公开成绩(整机)\""
         ",\"resolutionFixedByUL\":true"
         ",\"resolutionSource\":\"UL 官方支持文章 44002528070 / 44002528074 明文: SNL 渲染分辨率是 2560x1440, "
         "所有平台一致, 不是设备屏幕分辨率\""
         ",\"latestVerifiedAnchor\":\"Kirin 9020 = 454 (UL 官方成绩库机型行 + nanoreview 一致)\""
         ",\"caveat\":\"三条公开值查证强度不同: 454 有 UL 官方成绩库出处, 991 有用户拍屏的一手证据(2026-08-31, Mate 80 Pro Max, 991 分 / 7.34 FPS), 303 两者都没有(见每条 note)。"
         "UL 公开库给的是用户提交结果的中位数, 同一颗 SoC 在不同机型上本就能差出一倍(9020: 368..557)。"
         "本小节负载与 SNL 也不是同一份负载, 分数不会逐分相等 —— 它保证的是同一把尺子上的比值\""
         ",\"scaling\":{\"officialScoreEquals135TimesFps\":true,"
         "\"k3dmarkNomadScale\":135.0,\"kResolutionScaleRatio\":" + fixed(kResolutionScaleRatio, 6) + "}"
         ",\"cpuLimitationInvalidatesGraphicsScore\":true"
         ",\"cpuLimitationQuote\":\"UL 官方 44002528074: If the CPU cannot submit work to the GPU fast "
         "enough, it will be the limiting factor in the benchmark, effectively invalidating the result "
         "of the Graphics test.\""
         ",\"points\":[";
    for (int i = 0; i < kRefPointCount; ++i) {
        const double off = kRefPoints[i].officialScore;
        const double refFps = off / k3dmarkNomadScale;        // 官方 1440p 下的帧率
        const double myFps = refFps / kResolutionScaleRatio;  // 折算到 1920x1080 的等效帧率
        const double myScore = myFps * kScorePerFps;          // 同一把尺子下与公开值相等
        j += (i == 0 ? "" : ",");
        j += "{\"soc\":\"" + jsonSafe(kRefPoints[i].soc) + "\",\"device\":\"" + jsonSafe(kRefPoints[i].device) +
             "\",\"officialScore\":" + fixed(off, 0) +
             ",\"verification\":\"" + jsonSafe(kRefPoints[i].verification) + "\""
             ",\"note\":\"" + jsonSafe(kRefPoints[i].note) + "\""
             ",\"officialFpsAtNomadResolution\":" + fixed(refFps, 3) +
             ",\"equivFpsAt1920x1080\":" + fixed(myFps, 3) +
             ",\"equivScoreOnThisRuler\":" + fixed(myScore, 1) + "}";
    }
    j += "],\"predictionNote\":\"上表是把公开分按同一条公式折算到本小节像素数的结果, 用来在真机上一行对一行地核对; "
         "表里的等效分数只有在本小节负载与 SNL 同速时才成立\"}";

    // ---- calibration(标定状态与"怎么标定") ----
    j += ",\"calibration\":{\"status\":\"UNCALIBRATED\""
         ",\"kScorePerFpsInUse\":" + fixed(kScorePerFps, 6) +
         ",\"kWorkloadScaleInUse\":" + fixed(kWorkloadScale, 6) +
         ",\"derivation\":\"score_snl = fps_snl x 135; fps_aurora = fps_snl / 0.5625 (像素少 -> 帧率高); "
         "两者相等的条件 => kScorePerFps = 135 x 0.5625 = 75.9375 (workloadScale=1.0)\""
         ",\"whatKWorkloadScaleMeans\":\"本小节负载每像素速度 / SNL 每像素速度; 1.0 = 假设两者相同\""
         ",\"affectsAbsoluteScoreOnly\":true"
         ",\"referenceValue\":\"3DMark Steel Nomad Light 公开分(整机)\""
         ",\"procedure\":\"在同一台设备上先后跑 3DMark Steel Nomad Light 与本小节, 读到 fps_snl 与 fps_aurora; "
         "则 s_workload = fps_snl / (fps_aurora x 0.5625)(两者速度相同则为 1.0), "
         "再令 kScorePerFps := 135 x 0.5625 x s_workload; 多台设备算出的 s_workload 一致即说明线性假设成立\""
         ",\"whyNeeded\":\"本小节负载与 SNL 不是同一份负载, 像素级速度不同; 只有标定之后绝对分数才能与公开值直接对照\""
         ",\"ratioIsValidWithoutCalibration\":true}";

    // ---- notes / risks(把"做不到的"写清楚) ----
    // ---- realUse(第 4 条: 一句话说明这项代表什么真实用途) ----
    j += ",\"realUse\":\"" + jsonSafe(std::string(kRealUseText)) + "\"";
    j += ",\"realUseShort\":\"非光追 game-like 场景的地形求交 + 延迟 PBR 着色(现代手游主渲染通路)\"";
    // ---- environment(第 2、6 条: 温度 / 频率 / 允许核集合 / governor 原文) ----
    j += envJson(m.env, m.repeats, m.gapMs);
    // ---- referenceComparison(第 3 条: 与公开真值并列, 带来源与口径) ----
    j += referenceComparisonJson(s);
    // ---- linearity(最高优先级验收标准: 分数比 == 性能比 的自检) ----
    j += linearityJson(m);
    // ---- comparability(条件化可比性: 在什么条件下取得的 + 与谁可比) ----
    {
        const ComparabilityInfo cmp = snComparability(m, s);
        j += comparabilityJson(m, s, cmp);
    }
    // ---- repeatability(第 1 条: 多轮原始值 + 中位/最小/最大/离散度 + 可信度判断) ----
    j += repeatabilityJson(m);
    j += ",\"notes\":\"本小节是 3DMark Steel Nomad 风格的独立 GPU 小节: GPU-driven(CPU 零顶点数据)、"
         "每趟 2 次 draw call、重着色、每帧工作量固定; 不并入 GB7 的 11 项 GPU 与 16 项 CPU\""
         ",\"risks\":["
         "\"公开的 991/454/303 是整机成绩, 机型渲染分辨率未经核实, 只作数量级与代际比例参照\","
         "\"kScorePerFps=75.9375 建立在「负载为片元吞吐受限」这一假设上; 标定前不要把它当 SNL 绝对值\","
         "\"分辨率 1920x1080 与 SNL 官方 2560x1440 不同 —— 这是为了与本工程其它 GPU 小节同口径\","
         "\"手机端 GPU 可能受 DVFS / 温控影响: 长时间跑分后帧率会下降, 对比时请在同一热状态下进行\","
         "\"自适应只按每帧目标毫秒数选趟数, 不做任何设备相关调优; 极慢设备上可能退到 1 趟\""
         "]";

    // ---- ruler(为未来留出空间: 写出这一轮用到了什么、没用到什么) ----
    const SimdFeatures& sf = m.cpu.simd;
    j += ",\"ruler\":{\"whatThisMeasures\":\"每秒完成的片元着色像素数(吞吐量) —— 与设备无关的统一刻度\""
         ",\"whatThisDoesNotMeasure\":\"光追(SNL 本身不含光追)、NPU、多核 CPU、存储\""
         ",\"cpu\":{\"logicalCores\":" + std::to_string(m.cpu.logical) +
         ",\"physicalCores\":" + std::to_string(m.cpu.physical) +
         ",\"smtPossible\":" + std::string(m.cpu.smtPossible ? "true" : "false") +
         ",\"smtEnabled\":" + std::string(m.cpu.smtEnabled ? "true" : "false") +
         ",\"threadCap\":" + std::to_string(m.cpu.threadCap) +
         ",\"topologyKnown\":" + std::string(m.cpu.topologyKnown ? "true" : "false") +
         ",\"topologySource\":\"" + jsonSafe(m.cpu.sourceText) + "\""
         ",\"probe\":\"" + jsonSafe(m.cpu.probeText, 480) + "\""
         ",\"usedByThisSection\":\"本小节只在调用线程上做 GL 提交(1 个线程), 不使用多线程 / 不用 NPU\","
         "\"maxConcurrencyUsed\":1}";
    j += ",\"simd\":{\"arch\":" + std::to_string(sf.armArch) +
         ",\"neon\":" + std::string(sf.neon ? "true" : "false") +
         ",\"sve\":" + std::string(sf.sve ? "true" : "false") +
         ",\"sve2\":" + std::string(sf.sve2 ? "true" : "false") +
         ",\"dotprod\":" + std::string(sf.dotprod ? "true" : "false") +
         ",\"fp16\":" + std::string(sf.fp16 ? "true" : "false") +
         ",\"bf16\":" + std::string(sf.bf16 ? "true" : "false") +
         ",\"i8mm\":" + std::string(sf.i8mm ? "true" : "false") +
         ",\"crc32\":" + std::string(sf.crc ? "true" : "false") +
         ",\"crypto\":" + std::string(sf.aes ? "true" : "false") +
         ",\"lse\":" + std::string(sf.lse ? "true" : "false") +
         ",\"note\":\"这些是编译期宏探测到的能力; 本小节把这些能力用于 GL 提交路径, "
         "GPU 着色器用的是 GLSL ES 3.00, 与 CPU 的 SVE/NPU 无关 —— 记录以便未来判断尺子有没有被喂满\"}";
    j += ",\"headroomNotes\":["
         "\"16 核机器: 本小节仍然只用 1 个线程做提交(提交量是常量级), 核数不会改变分数 —— 这是设计使然\","
         "\"SVE / 更强 NPU: 本小节完全用不到, 尺子不会因为它们变快\","
         "\"GPU 侧: 每一轮的 GPU 忙占比与 CPU 递交占比会写在 gpuBoundEvidence 里, "
         "如果将来 GPU 快到 CPU 递交成为瓶颈, 那一轮会直接标 CPU_SUBMISSION_BOUND / 分数无效\""
         //  2026-10-05 修复(真机 8.1 现场) 这里漏了一个 '}' —— ruler 对象从来没被闭合。
         //   后果: simd / headroomNotes / rendering / gpu / config / validity / warnings / elapsedMs
         //   全部被塞进 ruler 里面, 末尾那个 '}' 只关掉 ruler, 根对象永远不闭合。
         //   真机证据: D:\gb7logs\r80\report-latest.txt 的原样 JSON 里, 括号栈剩下
         //   [根 '{', ruler '{'] 两个未匹配项 —— 与"括号不平衡(depth=2)"逐字吻合。
         //   ArkTS 侧读的是 ruler.cpu / ruler.simd(BenchRunner.ets), 所以 simd 必须在 ruler 里,
         //   闭合点只能在这里(见 verify_sn_json_balance.py 的定长断言)。
         "]}";

    // ---- rendering(渲染事实) ----
    j += ",\"rendering\":{\"width\":" + std::to_string(kWidth) + ",\"height\":" + std::to_string(kHeight) +
         ",\"fixedResolution\":true"
         ",\"api\":\"OpenGL ES 3.0 (EGL pbuffer 离屏, 无 XComponent, 无线程)\""
         ",\"passes\":2,\"passNames\":[\"G-buffer/表面求解\",\"deferred PBR 着色\"]"
         ",\"gBufferFormat\":\"RGBA8 x2 + DEPTH" + std::to_string(g_depthBits > 0 ? g_depthBits : 24) + "\""
         ",\"colorFormat\":\"RGBA8 x2(双缓冲, 每趟换目标)\""
         ",\"instancing\":\"glDrawArraysInstanced(GL_TRIANGLES, 0, 6, " + std::to_string(grid.instances) + ")\""
         ",\"vertexBuffers\":0"
         ",\"fixedSeed\":true"
         ",\"seedRule\":\"seed = 0.125 + 0.037 x (趟序号 % 64), 与设备无关\""
         ",\"deterministicScene\":\"场景只由 GLSL 里的整数哈希与常量决定, 没有随机数、没有时间、没有设备输入\"}";

    // ---- gpu(记录 GL / GPU 标识) ----
    j += ",\"gpu\":{\"glVersion\":\"" + jsonSafe(g_glVersion) + "\""
         ",\"glVendor\":\"" + jsonSafe(g_glVendor) + "\""
         ",\"glRenderer\":\"" + jsonSafe(g_glRenderer) + "\""
         ",\"glslVersion\":\"" + jsonSafe(g_glslVersion) + "\""
         ",\"maxDrawBuffers\":" + std::to_string(g_maxDrawBuffers) +
         ",\"mrtUsed\":true"
         ",\"maxSamples\":" + std::to_string(g_maxSamples) +
         ",\"msaa\":false"
         ",\"offScreen\":true"
         ",\"vsync\":false"
         //  2026-10-05 修复(同一次) 这里也漏了一个 '}' —— gpu 对象从来没被闭合。
         //   后果: config / validity / warnings / elapsedMs 全被塞进 gpu 里面,
         //   根对象的 '}' 被 gpu 吃掉。这是第二个未匹配的 '{'。
         //   两个漏括号合起来正好是根 '{' + 一个内层 '{' = depth 2。
         ",\"note\":\"GPU 型号字符串来自驱动, 只作记录, 不计分或负载选择\"}";

    // ---- config(生效的可选项原样回写) ----
    j += ",\"config\":{\"passesPerFrameOption\":" + std::to_string(opt.passesPerFrame) +
         ",\"measureFrames\":" + std::to_string(opt.measureFrames) +
         ",\"repeats\":" + std::to_string(m.repeats) +
         ",\"gapMs\":" + std::to_string(m.gapMs) +
         ",\"targetMs\":" + fixed(opt.targetMs, 1) +
         ",\"maxPasses\":" + std::to_string(opt.maxPasses) +
         ",\"defaults\":{\"passesPerFrame\":0,\"measureFrames\":" + std::to_string(kDefaultMeasureFrames) +
         ",\"repeats\":" + std::to_string(kDefaultRepeats) +
         ",\"gapMs\":" + std::to_string(kDefaultGapMs) +
         ",\"targetMs\":" + fixed(kDefaultTargetMs, 1) + ",\"maxPasses\":" + std::to_string((int)kDefaultMaxPasses) + "}}";

    // ---- validity / warnings ----
    j += ",\"validity\":{\"valid\":" + std::string(valid ? "true" : "false") +
         ",\"pass\":" + std::string(pass ? "true" : "false") +
         ",\"checks\":{\"gpuBound\":{\"pass\":" + std::string(gpuBound ? "true" : "false") +
         ",\"detail\":\"GPU 忙时间 / CPU 递交时间 = " + fixed(cpuGpuRatio, 2) + "x(>=2 视为 GPU 受限)\"},"
         "\"cpuNotBottleneck\":{\"pass\":" + std::string(!cpuBound ? "true" : "false") +
         ",\"detail\":\"CPU 递交占帧时间 " + fixed(submissionDuty, 2) + "%(<60% 视为不是递交瓶颈)\"},"
         "\"notTooFast\":{\"pass\":" + std::string(!tooFast ? "true" : "false") +
         ",\"detail\":\"平均帧时间 " + fixed(meanFrameMs, 3) + " ms(>= " + fixed(kMinAcceptMs, 1) + " ms 才可信)\"},"
         "\"ratioMethodSeparated\":{\"pass\":" + std::string(conclusive ? "true" : "false") +
         ",\"detail\":\"两种趟数的耗时比值 " + fixed(m.ratio, 3) + " 落在 " + fixed(m.ratioMin, 2) + "x.." +
         fixed(m.ratioMax, 2) + "x 内\"},"
         "\"outputNotBlank\":{\"pass\":" + std::string((m.proof.readbackNonZero > 0) ? "true" : "false") +
         ",\"detail\":\"读回非零字节 " + std::to_string(m.proof.readbackNonZero) + "/" +
         std::to_string(m.proof.expectedNonZeroBytes) + "\"},"
         "\"repeatable\":{\"pass\":" + std::string(m.repeatDispersion <= 5.0 && m.roundsOk >= 2 ? "true" : "false") +
         ",\"detail\":\"多轮(" + std::to_string(m.roundsOk) + "/" + std::to_string(m.repeats) +
         " 轮)相对离散度 " + fixed(m.repeatDispersion, 2) + "%(>5% 视为重复性差, 该数字仅供参考)\"},"
         "\"environmentReported\":{\"pass\":" + std::string(m.envKnown ? "true" : "false") +
         ",\"detail\":\"温度/频率读数 " + std::string(m.envKnown ? "可用, 已写进 environment 块" :
             "读不到 —— 已在 environment.state=UNKNOWN 与 warnings 里明确标注, 不假装稳定") + "\"}}}";
    // ---- 判定: 全部是"算出来的", 没有一个是写死的结论 ----
    if (m.passCount <= 1) {
        warn.push_back("passesPerFrame=1: 这台设备做一趟 1080p 管线就已经超过目标帧时间, 自适应无法再往下退; "
                       "数字仍然有效, 但已经贴到尺子最低档 —— 更慢的设备建议只看吞吐量");
    }
    if (tooFast && !limitReached) {
        warn.push_back("平均帧时间低于 " + fixed(kMinAcceptMs, 1) +
                       " ms: GPU 侧工作量偏小, 固定开销占比上升, 帧时间数字可信度下降");
    }
    if (limitReached) {
        warn.push_back("已顶到趟数上限 " + std::to_string(kPassLadderMax) +
                       ": 这台设备的 GPU 远快于尺子当前设计的范围。修法只有一条 —— 提高 kPassLadderMax "
                       "并递增 kSnBenchVersion(新旧分数届时不可比)");
    }
    if (cpuBound) {
        warn.push_back("CPU 递交占帧时间 " + fixed(submissionDuty, 1) + "%: 本轮 GPU 分数无效(被 CPU 递交侧限制)");
    }
    if (!conclusive) {
        warn.push_back("CPU/GPU 比值区间未越过 " + fixed(m.ratioMin, 2) + "x.." + fixed(m.ratioMax, 2) +
                       "x: 本机两种趟数的耗时比值没能分开, 无法用比值法证明 GPU 瓶颈");
    }
    if (!g_hasFence) {
        warn.push_back("GL fence 不可用: GPU 忙时间按 (墙钟 - CPU 递交) 估算, 口径退化为与 glFinish 等价");
    }
    if (g_envThreadFaulted != 0) {
        warn.push_back(" 环境采样线程自己崩过一次(已被故障隔离舱兜住, App 没有崩): 本轮的频率/温度是"
                       "降级读数(只有一次性探测 + 一次采样, 没有连续采样)——不要据此判断是否降频; "
                       "分数与负载本身不受这一条影响");
    }
    if (m.proof.available && m.proof.readbackNonZero < m.proof.expectedNonZeroBytes / 4) {
        warn.push_back("自证读回的非零字节偏少(" + std::to_string(m.proof.readbackNonZero) + "/" +
                       std::to_string(m.proof.expectedNonZeroBytes) + "): 输出画面可能接近全黑, 请检查着色器");
    }
    //  第 1 条的核心: 不可信的数字必须被标出来, 不许当可信的报 
    if (m.roundsOk < 3) {
        warn.push_back(" 有效轮数只有 " + std::to_string(m.roundsOk) + " 轮(默认 3 轮): "
                       "离散度本身也不可信 —— 分数仅供参考, 建议重跑");
    } else if (m.repeatDispersion > 5.0) {
        warn.push_back(" 本项重复性差: " + std::to_string(m.roundsOk) + " 轮相对离散度 " +
                       fixed(m.repeatDispersion, 2) + "%(阈值 >5% 即不可信)—— "
                       "这个数字仅供参考, 不要拿它当结论; 全部轮次的原始值见 repeatability.roundsDetail");
    } else if (m.repeatDispersion > 2.0) {
        warn.push_back("重复性一般: 多轮相对离散度 " + fixed(m.repeatDispersion, 2) +
                       "%(2~5%); 仍按中位报出, 但对比时请留意这个区间");
    }
    if (!m.envKnown) {
        warn.push_back(" 环境读数不可用(频率与温度都读不到): 无法据本次运行判断是否降频 —— "
                       "不要把它当成'稳定'的证据; 逐项 errno 见 environment.probe");
    } else if (std::strcmp(cstrOr(m.env.state, "UNKNOWN"), "THROTTLED") == 0) {
        warn.push_back(" 本次运行疑似降频/进入热限制(判据见 environment.stability): "
                       "分数按实测报出, 但跨设备对比时请把这一点算进去");
    } else if (std::strcmp(cstrOr(m.env.state, "UNKNOWN"), "WARMING_UP") == 0) {
        warn.push_back("本次未跑满可用频率档(environment.stability.state=WARMING_UP): "
                       "可能是刚开机/后台负载/系统限频");
    }
    if (m.roundsOk >= 2 && m.firstVsLaterPct < -5.0) {
        warn.push_back(" 首轮明显低于后续轮(低 " + fixed(-m.firstVsLaterPct, 1) + "%): 热身效应, "
                       "已记录(全部轮次都在 repeatability.roundsDetail 里), 没有用'取最好一次'掩盖");
    }

    j += ",\"warnings\":[";
    for (size_t i = 0; i < warn.size(); ++i) {
        j += (i == 0 ? "" : ",");
        j += "\"" + jsonSafe(warn[i]) + "\"";
    }
    j += "]";
    j += ",\"elapsedMs\":" + fixed(m.totalElapsedMs, 1);
    j += "}";
    return j;
}

std::string repeatabilityJson(const SnMeasurement& m)
{
    std::vector<double> fpsList;
    std::vector<double> mpxList;
    std::vector<double> scoreList;
    std::vector<double> perPassList;
    std::vector<double> frameList;
    int okRounds = 0;
    std::string roundsJson;
    for (size_t i = 0; i < m.rounds.size(); ++i) {
        const SnRound& r = m.rounds[i];
        char b[900];
        if (!r.ok) {
            snprintf(b, sizeof(b),
                     "%s{\"round\":%d,\"ok\":false,\"note\":\"这一轮没跑成(失败原因在 warnings/lastError 里)\"}",
                     roundsJson.empty() ? "" : ",", r.index);
            roundsJson += b;
            continue;
        }
        ++okRounds;
        const double fps = roundFps(r);
        const double mpx = pixelsPerFrame(r.passCount) * fps / 1.0e6;
        const double sc = fps * kScorePerFps;
        fpsList.push_back(fps);
        mpxList.push_back(mpx);
        scoreList.push_back(sc);
        perPassList.push_back(r.perPassMs);
        if (r.measuredFrames > 0) {
            frameList.push_back(r.totalMs / (double)r.measuredFrames);
        }
        snprintf(b, sizeof(b),
                 "%s{\"round\":%d,\"ok\":true,\"passesPerFrame\":%d,\"measuredFrames\":%d"
                 ",\"totalMs\":%.3f,\"warmupMs\":%.3f,\"meanFrameMs\":%.3f,\"fps\":%.3f"
                 ",\"perPassMs\":%.5f,\"throughputMpxPerSec\":%.3f,\"score\":%.1f"
                 ",\"cpuSubmitMs\":%.3f,\"gpuBusyMs\":%.3f"
                 ",\"tempC\":{\"min\":%.2f,\"median\":%.2f,\"max\":%.2f}"
                 ",\"cpuKhz\":{\"min\":%.0f,\"median\":%.0f,\"max\":%.0f}"
                 ",\"envState\":\"%s\"}",
                 roundsJson.empty() ? "" : ",", r.index, r.passCount, r.measuredFrames,
                 r.totalMs, r.warmupMs, frameList.empty() ? 0.0 : r.totalMs / (double)r.measuredFrames,
                 fps, r.perPassMs, mpx, sc, r.cpuSubmitMs, r.gpuBusyMs,
                 r.tempMin, r.tempMed, r.tempMax, r.khzMin, r.khzMed, r.khzMax,
                 //  2026-10-05: 这一格原来是 r.envState 直接进 snprintf("%s") —— 真机 8.0 的
                 //   payload 里正是这里写出了野字节 "\xd0\xa0\xc2\xb1s\x7f"。
                 //   现在先规范化再转义。临时 std::string 活到本语句结束, .c_str() 安全。
                 jsonSafe(std::string(snCanonState(r.envState))).c_str());
        roundsJson += b;
    }

    const double fpsMed = medianOf(fpsList);
    const double fpsMin = fpsList.empty() ? 0.0 : *std::min_element(fpsList.begin(), fpsList.end());
    const double fpsMax = fpsList.empty() ? 0.0 : *std::max_element(fpsList.begin(), fpsList.end());
    const double mpxMed = medianOf(mpxList);
    const double scoreMed = medianOf(scoreList);
    const double dispFps = dispersionOf(fpsMin, fpsMed, fpsMax);

    // CV(变异系数) 作为补充: 它比 (max-min)/mid 更抗"只差一轮"的噪声
    double cv = 0.0;
    if (fpsList.size() >= 2 && fpsMed > 0.0) {
        double mean = 0.0;
        for (double v : fpsList) {
            mean += v;
        }
        mean /= (double)fpsList.size();
        double var = 0.0;
        for (double v : fpsList) {
            var += (v - mean) * (v - mean);
        }
        var /= (double)(fpsList.size() - 1);
        cv = (mean > 0.0) ? (std::sqrt(var) / mean * 100.0) : 0.0;
    }

    // 首轮 vs 后续轮: 热身效应记录(不掩盖)
    double firstFps = 0.0;
    double laterMed = 0.0;
    if (fpsList.size() >= 2) {
        firstFps = fpsList[0];
        std::vector<double> later(fpsList.begin() + 1, fpsList.end());
        laterMed = medianOf(later);
    }
    const double firstVsLaterPct = (firstFps > 0.0 && laterMed > 0.0)
                                       ? (firstFps - laterMed) / laterMed * 100.0
                                       : 0.0;
    const bool warmupEffect = (firstFps > 0.0 && laterMed > 0.0 && firstFps < laterMed * 0.95);

    // 逐轮漂移(热降频的直接证据)
    const double perPassMed = medianOf(perPassList);
    const double perPassFirst = perPassList.empty() ? 0.0 : perPassList.front();
    const double perPassLast = perPassList.empty() ? 0.0 : perPassList.back();
    const double perPassDriftPct = (perPassFirst > 0.0)
                                       ? (perPassLast - perPassFirst) / perPassFirst * 100.0
                                       : 0.0;
    const bool perPassDropped = (perPassFirst > 0.0 && perPassLast > perPassFirst * 1.05);

    const char* dispVerdict = credibilityOf(dispFps);
    const bool reliable = (okRounds >= 2 && dispFps <= 5.0);
    const bool enough = (okRounds >= 3);

    std::string j;
    j.reserve(3200);
    j += ",\"repeatability\":{\"repeats\":" + std::to_string(m.rounds.size()) +
         ",\"roundsOk\":" + std::to_string(okRounds) +
         ",\"rounds\":" + std::to_string(m.rounds.size()) +
         ",\"roundValuesAreAllListed\":true"
         ",\"representative\":\"median(中位) —— 不是最好一次, 也不是平均\""
         ",\"dispersionDefinition\":\"相对离散度 = (最大 - 最小) / 中位 x 100%\""
         ",\"thresholdsAreOurs\":true"
         ",\"thresholds\":{\"reliable\":\"<= 2%\",\"fair\":\"<= 5%\",\"unreliable\":\"> 5%\"}"
         ",\"roles\":{\"fps\":\"每次运行的原始值, 全部列出\","
         "\"perPassMs\":\"每趟 GPU 耗时(与 pass 数无关的单一刻度, 漂移最敏感)\"}"
         ",\"statistics\":{\"fps\":{\"median\":" + fixed(fpsMed, 3) +
         ",\"min\":" + fixed(fpsMin, 3) + ",\"max\":" + fixed(fpsMax, 3) +
         ",\"dispersion\":" + fixed(dispFps, 2) + ",\"cv\":" + fixed(cv, 2) + "}"
         ",\"throughputMpxPerSec\":{\"median\":" + fixed(mpxMed, 3) +
         ",\"min\":" + fixed(mpxList.empty() ? 0.0 : *std::min_element(mpxList.begin(), mpxList.end()), 3) +
         ",\"max\":" + fixed(mpxList.empty() ? 0.0 : *std::max_element(mpxList.begin(), mpxList.end()), 3) + "}"
         ",\"score\":{\"median\":" + fixed(scoreMed, 1) +
         ",\"min\":" + fixed(scoreList.empty() ? 0.0 : *std::min_element(scoreList.begin(), scoreList.end()), 1) +
         ",\"max\":" + fixed(scoreList.empty() ? 0.0 : *std::max_element(scoreList.begin(), scoreList.end()), 1) +
         "},\"perPassMs\":{\"median\":" + fixed(perPassMed, 5) + "}}"
         ",\"credibility\":{\"verdict\":\"" + jsonSafe(std::string(cstrOr(dispVerdict, ""))) + "\""
         ",\"dispersion\":" + fixed(dispFps, 2) +
         ",\"reliable\":" + std::string(reliable ? "true" : "false") +
         ",\"roundCountSufficient\":" + std::string(enough ? "true" : "false") +
         ",\"text\":\"" + jsonSafe(
             std::string(dispVerdict[0] == 'R'
                             ? "重复性好: 多轮离散度 "
                             : (dispVerdict[0] == 'F' ? "重复性一般: 多轮离散度 " : "本项重复性差: 多轮离散度 ")) +
             fixed(dispFps, 1) + "%(阈值 <=2% 可信 / 2~5% 一般 / >5% 仅供参考)。" +
             (reliable ? "" : "该数字仅供参考, 不要拿它当结论。") +
             (enough ? "" : "另外: 目前只有 " + std::to_string(okRounds) +
                                " 轮有效结果, 少于 3 轮时离散度本身也不够可信。")) + "\"}"
         ",\"roundsDetail\":[" + roundsJson + "]";
    j += ",\"stability\":{\"perPassMsFirst\":" + fixed(perPassFirst, 5) +
         ",\"perPassMsLast\":" + fixed(perPassLast, 5) +
         ",\"perPassDriftPercent\":" + fixed(perPassDriftPct, 2) +
         ",\"dropped\":" + std::string(perPassDropped ? "true" : "false") +
         ",\"firstRoundFps\":" + fixed(firstFps, 3) +
         ",\"laterRoundsMedianFps\":" + fixed(laterMed, 3) +
         ",\"firstVsLaterPercent\":" + fixed(firstVsLaterPct, 2) +
         ",\"warmupEffect\":" + std::string(warmupEffect ? "true" : "false") +
         ",\"text\":\"" + jsonSafe(
             std::string(perPassDropped
                             ? "末轮每趟耗时不高于首轮 —— 运行期间出现了漂移/降频, "
                               "分数按实测报出, 但请结合 environment.temperature 一起看。"
                             : "运行期间每趟耗时基本稳定, 没有明显漂移。") +
             (warmupEffect ? " 另外: 首轮明显低于后续轮(热身效应), 这是记录, 没有被'取最好一次'掩盖。"
                           : " 首轮与后续轮没有明显差别(没有热身效应)。")) + "\"}";
    j += "}";
    return j;
}

std::string referenceComparisonJson(const SnStats& s)
{
    std::string j;
    j.reserve(6000);
    j += ",\"referenceComparison\":{";
    j += "\"thisDeviceIsNotCalibrated\":true";
    j += ",\"provenanceRules\":\"每条都带 source 与 sourceKind: OFFICIAL_* = 厂商官方文档/成绩库; "
         "USER_PROVIDED_PUBLIC_TRUTH = 用户提供的公开真值(未在本次工作中逐页复核); "
         "THIRD_PARTY = 第三方媒体/聚合站; NOT_AVAILABLE = 查不到。第三方数据不写成官方。\"";
    j += ",\"comparisonIsNotAnOfficialConversion\":true";
    j += ",\"comparisonCaveat\":\"把 GB7 的分与本小节的分数并列, 是跨负载换算, 官方没有任何换算关系; "
         "它只能回答'数量级对不对', 不能回答'本机在某官方测试里能得多少分'。同一台机器在两个不同负载上的"
         "分数差多少, 取决于两个负载各自压的是什么资源。\"";
    j += ",\"referenceDevice\":\"" + jsonSafe(std::string(kRefDevice)) + "\"";
    j += ",\"referenceDeviceSource\":\"用户提供的公开真值(本工程 ref/ 目录里另有部分页面存档)\"";
    j += ",\"referenceDeviceCaveat\":\"该机型的 3DMark SNL 991 分与 GB7 的逐项吞吐都没有在本次工作中"
         "从官方成绩库逐条复核; 其中 3DMark 991 的来源是用户拍屏的一手证据(2026-08-31, Mate 80 Pro Max, 991 分 / 平均 7.34 FPS), GB7 的逐项吞吐仍是用户提供的公开真值 —— 两类来源与强度见 reference.points.sourceKind\"";
    j += ",\"gb7SingleCore\":{\"role\":\"CS1 单核阶段(16 项)的对照表不在本模块, "
         "见新增的 aurora-reference 模块导出的 auroraReferenceCompare()\"}";
    j += ",\"gpuSnl\":{\"ours\":{\"score\":" + fixed(s.score, 1) +
         ",\"fps\":" + fixed(s.fps, 3) +
         ",\"throughputMpxPerSec\":" + fixed(s.mpxPerSecond, 3) + "}"
         ",\"officialFormula\":\"score = 平均帧率 x 135\",\"sourceKind\":\"OFFICIAL_3DMARK_SUPPORT\""
         ",\"source\":\"https://support.benchmarks.ul.com/support/solutions/articles/44002528075\""
         ",\"rows\":[";
    for (int i = 0; i < kRefPointCount; ++i) {
        const double off = kRefPoints[i].officialScore;
        const double ours = s.score;
        const double pct = (off > 0.0) ? (ours / off - 1.0) * 100.0 : 0.0;
        j += (i == 0 ? "" : ",");
        j += "{\"name\":\"" + jsonSafe(std::string(kRefPoints[i].soc)) + " " +
             jsonSafe(std::string(kRefPoints[i].device)) + "\""
             ",\"referenceScore\":" + fixed(off, 0) +
             ",\"referenceValue\":\"" + fixed(off / k3dmarkNomadScale, 2) + " fps@2560x1440\""
             ",\"ourScore\":" + fixed(ours, 1) +
             ",\"deltaPercent\":" + fixed(pct, 1) +
             ",\"sourceKind\":\"" + jsonSafe(std::string(kRefPoints[i].verification)) + "\""
             ",\"source\":\"" + jsonSafe(std::string(kRefPoints[i].note)) + "\""
             ",\"caveat\":\"本小节的负载与 SNL 不是同一份负载, 也不是同一台机器; 该行只用于判断数量级\"}";
    }
    j += "]}";
    j += ",\"gb7SingleCoreTruths\":{\"referenceDevice\":\"" + jsonSafe(std::string(kRefDevice)) + "\""
         ",\"sourceKind\":\"USER_PROVIDED_PUBLIC_TRUTH\""
         ",\"source\":\"用户提供的公开真值清单\""
         ",\"module\":\"aurora-reference (libauroraref.so) 的 auroraReferenceCompare()\""
         ",\"composite\":{\"single\":1633.0,\"multi\":6802.0}"
         ",\"rows\":[";
    for (int i = 0; i < kGb7SingleRowCount; ++i) {
        j += (i == 0 ? "" : ",");
        j += "{\"name\":\"" + jsonSafe(std::string(kGb7SingleRows[i].name)) + "\""
             ",\"referenceMetric\":\"" + jsonSafe(std::string(kGb7SingleRows[i].metric)) + "\""
             ",\"referenceScore\":" + fixed(kGb7SingleRows[i].publishedScore, 0) +
             ",\"note\":\"" + jsonSafe(std::string(kGb7SingleRows[i].note)) + "\""
             ",\"ourScore\":null"
             ",\"deltaPercent\":null"
             ",\"comparable\":false"
             ",\"why\":\"本模块(GPU-SNL)拿不到 CS1 单项分; 请用 auroraReferenceCompare() 并传入本轮 GB7 结果\"}";
    }
    j += "]}";
    j += ",\"notAvailable\":[\"麒麟平台的 GPU 频率/温度: 没有可读的 sysfs 节点(查不到, 已在 environment 里说明)\","
         "\"官方 3DMark SNL 在 Mate 80 Pro Max 上的机型页: 查不到(UL 成绩库无 Mate 80 条目)\","
         "\"公开 GPU 套件的逐项绝对吞吐: 只有公开结果页读数, 本工程的 CS1 GPU 小节另有自己的锚点表\"]";
    j += "}";
    return j;
}

std::string envJson(const EnvDerived& e, int repeats, int gapMs)
{
    const double drop = (e.khzFirstHalfMed > 0.0)
                            ? (e.khzFirstHalfMed - e.khzSecondHalfMed) / e.khzFirstHalfMed * 100.0
                            : 0.0;
    std::string j;
    j.reserve(2600);
    j += ",\"environment\":{";
    j += "\"gpuClockReadable\":false";
    j += ",\"gpuClockWhy\":\"麒麟平台的 GPU 频率/温度没有可读的 sysfs 节点(已核实); "
         "本块报的是 CPU 频率 + SoC 热区温度, 它们是整机 DVFS/热状态的间接证据, "
         "不是 GPU 自己的频率。GPU 侧的直接证据是同一次运行内每轮吞吐/每趟耗时的漂移"
         "(见 repeatability.rounds 与 scale.perPassMsMedian)。\"";
    j += ",\"temperature\":{\"available\":" + std::string(e.tempSamples > 0 ? "true" : "false") +
         ",\"unit\":\"degC\",\"zoneIndex\":" + std::to_string(e.tempZoneIdx) +
         ",\"zoneName\":\"" + jsonSafe(std::string(
             (e.tempZoneIdx >= 0 && e.tempZoneIdx < e.zoneCount && e.tempZoneIdx < kEnvMaxZones)
                 ? g_envZoneName[e.tempZoneIdx] : "")) + "\""
         ",\"zonesScanned\":" + std::to_string(e.zoneCount) +
         ",\"samples\":" + std::to_string(e.tempSamples) +
         ",\"min\":" + fixed(e.tempMin, 2) + ",\"median\":" + fixed(e.tempMed, 2) +
         ",\"max\":" + fixed(e.tempMax, 2) +
         ",\"errno\":" + std::to_string(e.tempErrno) +
         ",\"source\":\"/sys/class/thermal/thermal_zoneN/temp(毫摄氏度 -> 摄氏度)\"}";
    j += ",\"cpuKhz\":{\"available\":" + std::string(e.khzSamples > 0 ? "true" : "false") +
         ",\"unit\":\"kHz\",\"definition\":\"每次采样取'该次采到的核里最大的那个'(逐核原始值在同块的 perCore)\""
         ",\"coresScanned\":" + std::to_string(e.cpuCount) +
         ",\"samples\":" + std::to_string(e.khzSamples) +
         ",\"min\":" + fixed(e.khzMin, 0) + ",\"median\":" + fixed(e.khzMed, 0) +
         ",\"max\":" + fixed(e.khzMax, 0) +
         ",\"firstHalfMedian\":" + fixed(e.khzFirstHalfMed, 0) +
         ",\"secondHalfMedian\":" + fixed(e.khzSecondHalfMed, 0) +
         ",\"errno\":" + std::to_string(e.cpuErrno) +
         ",\"source\":\"/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq(逐项记 errno, 读不到不填 0)\"}";
    j += ",\"sampling\":{\"intervalMs\":" + std::to_string(kEnvIntervalMs) +
         ",\"repeats\":" + std::to_string(repeats) +
         ",\"gapMs\":" + std::to_string(gapMs) + "}";
    //  2026-10-05: state 以前是原样拼进 JSON 的(唯一一处没有 jsonSafe 的动态字符串)。
    //   野指针读到什么就写什么, 只要那段内存里有一个 '"' 或 '\\', 整份 JSON 就在这儿破掉。
    //   现在先规范化(snCanonState)再转义(jsonSafe), 两道都过。
    j += ",\"stability\":{\"state\":\"" + jsonSafe(std::string(snCanonState(e.state))) + "\""
         ",\"cpuKhzDropPercent\":" + fixed(drop, 2) +
         ",\"throttled\":" + std::string(e.throttled ? "true" : "false") +
         ",\"text\":\"" + jsonSafe(std::string(cstrOr(e.text, ""))) + "\"}";
    j += ",\"probe\":{\"allowedCoresOk\":" + std::string(e.allowedOk ? "true" : "false") +
         ",\"allowedCores\":" + std::to_string(e.allowedCount) +
         ",\"allowedList\":\"" + jsonSafe(boundedCStr(e.allowedList, sizeof(e.allowedList))) + "\""
         ",\"governor\":\"" + jsonSafe(boundedCStr(e.governor, sizeof(e.governor))) + "\""
         ",\"governorErrno\":" + std::to_string(e.governorErrno) +
         ",\"allowedSource\":\"sched_getaffinity(0) 原文(内核允许本进程使用的核集合)\""
         ",\"perCore\":[";
    // 逐核原文(第 6 条: 让任何人可以据此质疑这个分数)
    {
        std::string per;
        for (int i = 0; i < g_envCpuCount; ++i) {
            std::vector<double> v;
            for (int s = 0; s < g_envCount; ++s) {
                if (g_envKhz[s][i] > 0) {
                    v.push_back((double)g_envKhz[s][i]);
                }
            }
            char b[160];
            if (v.empty()) {
                snprintf(b, sizeof(b), "%s{\"cpu\":%d,\"samples\":0,\"note\":\"全部读失败\"}",
                         per.empty() ? "" : ",", g_envCpu[i]);
            } else {
                std::sort(v.begin(), v.end());
                snprintf(b, sizeof(b),
                         "%s{\"cpu\":%d,\"samples\":%d,\"minKhz\":%.0f,\"medKhz\":%.0f,\"maxKhz\":%.0f}",
                         per.empty() ? "" : ",", g_envCpu[i], (int)v.size(), v.front(),
                         v[v.size() / 2], v.back());
            }
            per += b;
            if (per.size() > 1500) {
                break;
            }
        }
        j += per;
    }
    j += "]}";
    j += ",\"antiBenchmarkMode\":\"不检测任何'跑分模式', 也不做任何特供优化; 上面这些原始读数"
         "(允许核集合 / governor / 逐核频率 / 热区温度)就是给任何人用来质疑本分数的依据。"
         "宁可暴露不利事实, 也不许让分数看起来好看。\"";
    j += "}";
    return j;
}

} // namespace

// benchmark 级版本号( 与 App 版本号无关: 它只标记负载/口径的版本)
const int kSnBenchVersion = 1;


// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------


std::string snSectionName()
{
    return "GPU-SNL";
}

std::string snWorkloadName()
{
    return "Aurora Nomad Light";
}

std::string snBenchVersionText()
{
    return "benchVersion=" + std::to_string(kSnBenchVersion) +
           " (Aurora Nomad Light, " + std::string(kFormatRevision) + ", " +
           std::to_string(kWidth) + "x" + std::to_string(kHeight) +
           " 固定管线 + 每帧趜数自适应)。这是 benchmark 级版本号, 与 App 版本号是两件事。"
           "改动负载 / 口径 / 单位 / 计分公式就要递增它; 递增后新旧分数不可比。"
           "8.4 的 GB7 -> GB8 与本次的 GB8 -> CS1 都只改了套件的显示名, "
           "没动任何负载 / 单位 / k / conv / 计分公式, "
           "因此 benchVersion 不递增 —— 旧 GB7 / GB8 分数与新 CS1 分数在数值上完全可比。"
           "CS1 是本工程自研的 CPU 测试套件, 分数是一把绝对刻度: 单项分只由本机吞吐与本工程"
           "自己的换算常量决定, 不随机型、不随其它软件调整。"
           "它与任何第三方跑分软件都不是同一把尺子, 两边的分数不能互相换算、也不能直接比较; "
           "只有两台设备都跑本 App 的 CS1 时, 分数才是可比的。" ;
}

std::string snPrepareImpl()
{
    if (snfault::poisoned()) {
        const std::string why = snfault::poisonedText();
        appendSnError(why);
        return why;
    }
    g_snError.clear();
    std::string err = prepareInternal();
    if (!err.empty()) {
        appendSnError(err);
        return err;
    }
    probeGlInfo();
    err = buildPrograms();
    if (!err.empty()) {
        appendSnError(err);
        return err;
    }
    err = buildTargets();
    if (!err.empty()) {
        appendSnError(err);
        return err;
    }
    if (g_vao == 0) {
        glGenVertexArrays(1, &g_vao);
        if (g_vao == 0) {
            err = "glGenVertexArrays failed";
            appendSnError(err);
            return err;
        }
    }
    // 关闭垂直同步(离屏 pbuffer 本来就没有 vsync, 这里只对窗口表面有意义; 保持一致口径)
    eglSwapInterval(g_display, 0);
    // 尝试解析 GL fence(拿不到就降级: GPU 忙时间按"墙钟 - CPU 递交"估算)
    g_hasFence = true;
    {
        clearGlErrors();
        GLsync probe = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (probe == nullptr || glGetError() != GL_NO_ERROR) {
            g_hasFence = false;
            clearGlErrors();
        } else {
            glClientWaitSync(probe, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ULL);
            glDeleteSync(probe);
        }
    }
    g_ready = true;
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag,
                 "sn ready: renderer=%{public}s gl=%{public}s fence=%{public}s",
                 g_glRenderer.c_str(), g_glVersion.c_str(), g_hasFence ? "yes" : "no");
    return std::string();
}

// 对外入口: 整段包在故障隔离舱里。返回值契约一个字没变(空串 = 成功, 非空 = 失败原因)。
std::string snPrepare()
{
    snfault::Guard guard;
    if (guard.outermost()) {
        if (sigsetjmp(snfault::tlsJmp, 1) != 0) {
            const std::string why = snfault::describeLastFault();
            snfault::poison(why);
            snfault::resetAfterFault();
            snAbortEnvSamplingNoLock();
            appendSnError(why);
            return why;
        }
    }
    return snPrepareImpl();
}

bool snReady()
{
    return g_ready && !snfault::poisoned();
}

std::string snLastError()
{
    return g_snError;
}

double snLastScore()
{
    return g_lastScore;
}



std::string snRunImpl(const std::string& optionsJson)
{
    if (snfault::poisoned()) {
        return failJsonStep("run", snfault::poisonedText());
    }
    g_snError.clear();
    const double t0 = nowMs();

    if (g_ready && g_progGBuffer != 0 && glIsProgram(g_progGBuffer) == 0) {
        // 上下文丢失(进程切换 / 后台回收)后重建
        g_ready = false;
        g_progGBuffer = 0;
        g_progShade = 0;
        g_vsTile = 0;
        g_vsShade = 0;
        g_gbufFbo = 0;
        g_colorFbo = 0;
        g_gbufPosMat = 0;
        g_gbufNrmRough = 0;
        g_colorTex[0] = 0;
        g_colorTex[1] = 0;
        g_vao = 0;
    }
    std::string err = snPrepare();
    if (!err.empty()) {
        return failJsonStep("prepare(EGL 上下文 / 着色器 / 渲染目标)",
                            err.empty() ? std::string("prepare 返回了失败但没有给文本") : err);
    }

    SnMeasurement m;
    m.opt = parseOptions(optionsJson);
    //  2026-10-05 崩溃修复的关键一行 
    //   m.env 是 SnMeasurement 的成员, 以前从来没有被赋过值: 它的 state / text 是
    //   两个不确定的指针, 而 snlBuildJson() -> envJson() 会把 text 直接交给 std::string(),
    //   于是走到 libc 的 strlen(0x0)。7.5 之所以没暴露, 是因为那个版本在着色器编译阶段
    //   就失败返回了(failJson), 根本执行不到 snlBuildJson —— 8.0 修好着色器之后, 这段
    //   第一次真正跑起来, 当场就崩。这里先给一份"明确的未知", 循环里再逐轮覆盖。
    m.env = EnvDerived();
    // 同一类问题的第二处(2026-10-05 一起修): FrameTiming 的标量成员也没有初值, 而
    //   snComputeStats() 无条件读 m.finalRun.totalMs / cpuSubmitMs / gpuBusyMs。
    //   原代码只在"第 1 轮成功"时才给 m.finalRun 赋值 —— 于是"第 1 轮失败、第 2 轮成功"
    //   这种完全可能的路径会把未初始化的 double 直接印成分数(不崩, 但数字是垃圾)。
    //   这里先给一个明确的全 0 值。
    m.finalRun = FrameTiming();
    m.frameTimes.clear();
    m.probeUsed = false;
    m.probePerPassMs = 0.0;
    m.probeAtMs = 0.0;
    m.probePasses = 0;
    m.ratioLowPasses = 0;
    m.ratioLowMs = 0.0;
    m.ratioHighPasses = 0;
    m.ratioHighMs = 0.0;
    m.ratio = 0.0;
    m.ratioMin = 0.0;
    m.ratioMax = 0.0;
    m.totalElapsedMs = 0.0;
    m.proof.available = false;
    m.proof.readbackSum = 0;
    m.proof.readbackNonZero = 0;
    m.proof.readbackMin = 0;
    m.proof.readbackMax = 0;
    m.proof.readbackPx = kReadbackPx;
    m.proof.expectedNonZeroBytes = kReadbackPx * kReadbackPx * 4;
    m.cpu = probeCpu();
    m.warmupFrames = kWarmupFrames;

    // ---- 故障隔离舱自检(只有显式传 "faultSelfTest":1 才会走到) ----
    // 故意在隔离舱里读一个空指针: 产生的信号与真机现场(si_code=1 SEGV_MAPERR /
    // si_addr=0x0)逐字段同形。正常情况下这一行回不来 —— 隔离舱会把它兜住,
    // snRun() 返回一条"已隔离 + 信号/si_addr"的失败 JSON, 进程活着。
    // 它不跑任何负载, 不改工作量 / 口径 / 分数。
    if (m.opt.faultSelfTest != 0) {
        OH_LOG_Print(LOG_APP, LOG_WARN, 0x1234, kTag,
                     "fault self-test: 即将在隔离舱里制造一次 SIGSEGV(仅自检, 不跑负载)");
        snfault::deliberateNullRead();
        return failJsonStep("faultSelfTest",
                            "自检未触发(信号没有被制造出来; 隔离舱没有机会工作, 请检查编译器是否把空指针读优化掉了)");
    }

    if (m.opt.passesPerFrame > 0) {
        m.passCount = m.opt.passesPerFrame;
        m.passSource = "FIXED_BY_OPTION";
        if (m.passCount > kPassLadderMax) {
            m.passCount = kPassLadderMax;
        }
        // 固定档位时不给"每趟耗时"的参考值: 那会造出一个 N 趟外推出来的假探针数。
        // 比值法的两段改由下面"按最终档位"那一段统一安排(它只比已经发生过的两段)。
        m.probeUsed = false;
    } else {
        // ---- 自适应第 1 步: 量一段"每趟耗时" ----
        // kProbePasses 取 4(而不是 1): 让"每帧固定开销"摊薄, 外推才准。
        // 这一段只用来选档位, 不进最终计时; 最终分数只由 m.finalRun 决定。
        const int probePasses = 4;
        FrameTiming probe = runFrames(probePasses, 1, false);
        if (!probe.ok) {
            // 失败必须带原因(契约 E11): runFrames 走到 !ok 时一定会填 error, 这里再兜一层,
            // 保证即使将来有人加了一条不填 error 的返回路径, 交出去的文本也不会是空的。
            return failJsonStep("probe frame(自适应第 1 步: 量每趟耗时)",
                                probe.error.empty() ? std::string("runFrames(probe) 返回失败但没有给文本") : probe.error);
        }
        const double perPass = probe.totalMs / (double)probePasses;
        m.probeUsed = true;
        m.probePasses = probePasses;
        m.probeAtMs = probe.totalMs;
        m.probePerPassMs = perPass;
        bool clamped = false;
        const int want = choosePasses(perPass, m.opt.targetMs, m.opt.maxPasses, &clamped);
        m.passCount = want;
        m.passSource = "ADAPTIVE_PROBE(每趟 " + fixed(perPass, 4) + " ms, 目标 " +
                       fixed(m.opt.targetMs, 1) + " ms, 参考帧 " + fixed(probe.totalMs, 2) + " ms)";
        // 比值法的两段: 都用同一档(probePasses = 4), 与最终档一并做出区间
        m.ratioHighPasses = probePasses;
        m.ratioHighMs = probe.totalMs;
    }

    // ---- 自适应第 2 步: 多轮测量(验收标准第 1、2 条) ----
    //    每一轮都是完整一轮: 预热若干帧 + 计时若干帧 + 全程环境采样(频率/温度)。
    //    代表值取中位(见 snComputeStats), 不取最好一次;
    //     每一轮的原始值都原样列进 repeatability.roundsDetail。
    m.measuredFrames = m.opt.measureFrames;
    m.repeats = m.opt.repeats;
    m.gapMs = m.opt.gapMs;
    const int rounds = (m.repeats >= 1) ? m.repeats : kDefaultRepeats;
    m.repeats = rounds;
    SnRound sampleRound;
    sampleRound.index = 0;
    sampleRound.ok = false;
    sampleRound.totalMs = 0.0;
    sampleRound.cpuSubmitMs = 0.0;
    sampleRound.gpuBusyMs = 0.0;
    sampleRound.warmupMs = 0.0;
    sampleRound.perPassMs = 0.0;
    sampleRound.passCount = m.passCount;
    sampleRound.measuredFrames = m.measuredFrames;
    sampleRound.tempMin = 0.0;
    sampleRound.tempMed = 0.0;
    sampleRound.tempMax = 0.0;
    sampleRound.khzMin = 0.0;
    sampleRound.khzMed = 0.0;
    sampleRound.khzMax = 0.0;
    sampleRound.envState = "UNKNOWN";

    std::string lastRoundErr;
    for (int r = 0; r < rounds; ++r) {
        // 轮与轮之间留冷却间隔(默认 1500ms): 不掩盖热效应, 也不让上一轮的热量人为压下一轮;
        // 间隔长度原样写进 environment.sampling.gapMs。
        if (r > 0 && m.gapMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m.gapMs));
        }
        envBegin();
        FrameTiming warm = runFrames(m.passCount, kWarmupFrames, false);
        FrameTiming fin = runFrames(m.passCount, m.measuredFrames, true);
        const EnvDerived env = envDerive();
        // 环境块用最近一轮的采样(逐轮原始值都在 repeatability.roundsDetail 里) ——
        // 这一行以前漏了, 是 2026-10-05 native 崩溃的直接成因(见 SnMeasurement::env 的注释)。
        m.env = env;
        envEnd();

        SnRound rr = sampleRound;
        rr.index = r + 1;
        rr.passCount = m.passCount;
        rr.measuredFrames = m.measuredFrames;
        rr.tempMin = env.tempMin;
        rr.tempMed = env.tempMed;
        rr.tempMax = env.tempMax;
        rr.khzMin = env.khzMin;
        rr.khzMed = env.khzMed;
        rr.khzMax = env.khzMax;
        //  2026-10-05: 统一走 snCanonState —— 只有规范化过的四个字面量才允许进结果 JSON。
        rr.envState = snCanonState(env.state);

        if (!warm.ok) {
            lastRoundErr = "第 " + std::to_string(r + 1) + " 轮的预热帧失败: " +
                           (warm.error.empty() ? std::string("runFrames(warmup) 返回失败但没有给文本") : warm.error);
            rr.ok = false;
            m.rounds.push_back(rr);
            continue;
        }
        if (!fin.ok) {
            lastRoundErr = "第 " + std::to_string(r + 1) + " 轮的计时帧失败: " +
                           (fin.error.empty() ? std::string("runFrames(measure) 返回失败但没有给文本") : fin.error);
            rr.ok = false;
            m.rounds.push_back(rr);
            continue;
        }
        rr.ok = true;
        rr.warmupMs = warm.totalMs;
        rr.totalMs = fin.totalMs;
        rr.cpuSubmitMs = fin.cpuSubmitMs;
        rr.gpuBusyMs = (fin.gpuBusyMs > 0.0) ? fin.gpuBusyMs : (fin.totalMs - fin.cpuSubmitMs);
        rr.perPassMs = (m.passCount > 0) ? (fin.totalMs / (double)m.passCount) : 0.0;
        m.rounds.push_back(rr);

        // 第一轮作为主轮: 逐帧时间与自证读回都取它
        if (r == 0) {
            m.finalRun = fin;
            m.frameTimes.clear();
            for (int i = 0; i < m.measuredFrames; ++i) {
                m.frameTimes.push_back(fin.totalMs / (double)m.measuredFrames);
            }
            m.proof.available = (fin.readbackNonZero > 0 || fin.readbackMax > 0);
            m.proof.readbackSum = fin.readbackSum;
            m.proof.readbackNonZero = fin.readbackNonZero;
            m.proof.readbackMin = fin.readbackMin;
            m.proof.readbackMax = fin.readbackMax;
        }
    }

    {
        bool any = false;
        for (size_t i = 0; i < m.rounds.size(); ++i) {
            if (m.rounds[i].ok) {
                any = true;
                break;
            }
        }
        if (!any) {
            return failJsonStep("measure rounds(多轮计时)",
                                lastRoundErr.empty()
                                    ? std::string("全部 " + std::to_string(m.rounds.size()) +
                                                  " 轮都没跑成, 但没有记录到具体原因")
                                    : lastRoundErr);
        }
    }
    // 环境块用最近一轮的采样(逐轮的值在 repeatability.roundsDetail 里);
    //  可重复性汇总必须在这里算完 —— buildJson 的 warnings/validity 都要用它。

    // ---- 自适应第 3 步: 比值法的低段(在正式计时之后, 不污染上面的统计) ----
    if (m.ratioHighPasses <= 0) {
        m.ratioHighPasses = m.passCount;
        m.ratioHighMs = m.finalRun.totalMs * (double)(kWarmupFrames + m.measuredFrames) /
                        (double)(m.measuredFrames > 0 ? m.measuredFrames : 1);
    }
    if (m.ratioHighPasses > 0) {
        int low = m.ratioHighPasses;
        while (low > kPassLadderMin && low / 2 >= 1) {
            const int cand = low / 2;
            if (cand * 4 <= m.ratioHighPasses && cand <= m.opt.maxPasses &&
                (double)cand * m.probePerPassMs >= 0.6) {
                low = cand;
                continue;
            }
            break;
        }
        if (low < m.ratioHighPasses) {
            FrameTiming lo = runFrames(low, 1, false);
            if (lo.ok) {
                m.ratioLowPasses = low;
                m.ratioLowMs = lo.totalMs;
            }
        }
    }
    if (m.ratioHighMs > 0.0 && m.ratioLowMs > 0.0 && m.ratioLowPasses > 0 &&
        m.ratioHighPasses > m.ratioLowPasses) {
        m.ratio = m.ratioHighMs / m.ratioLowMs;
        m.ratioMin = m.ratio / 2.5;
        m.ratioMax = m.ratio * 2.5;
    }
    if (m.ratioMax - m.ratioMin < 1.0) {
        // 不同档位的耗时没能分开 -> 比值法无法作证; 明确标出来, 不假装
        m.ratioMin = 1.0;
        m.ratioMax = 1.0;
    }

    snFillRepeatSummary(m);   // 第 1 条: 有效轮数 / 相对离散度 / 首轮 vs 后续轮
    m.totalElapsedMs = nowMs() - t0;
    const SnStats stats = snComputeStats(m);
    const std::string out = snlBuildJson(m, stats);
    //  交付前的最后一道闸 : 用真解析器把整段文本走一遍(不是数括号)。
    //   真机现场出现过"native 返回失败但一个字都没有" —— 那是交出去的 JSON 解析不了,
    //   调用方只能兜成一条没有原因的失败。现在:
    //     * 解析器拒绝 -> 明确报出"第 N 个字节 + 具体原因", 并且写明括号计数扫描是否一致;
    //     * 解析器通过但括号计数不通过 -> 照常交付, 另记一条"自检误报"的警告,
    //       不把自检自己的缺陷写成"JSON 坏了"(真机 8.1 的报告正是在这一点上误导排查)。
    {
        const SnJsonCheckResult chk = snJsonSelfCheck(out);
        if (!chk.ok) {
            return failJsonStep("json-selfcheck",
                                chk.why + " (结果长度 " + std::to_string(out.size()) + " 字节)");
        }
        if (chk.selfCheckDisagreed) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0x1234, kTag,
                         "sn json selfcheck FALSE POSITIVE (交付不受影响): %{public}s",
                         chk.why.c_str());
        }
    }
    g_lastScore = stats.score;   // 代表分 = 多轮中位的 fps x kScorePerFps
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag,
                 "sn done: passes=%{public}d frames=%{public}d rounds=%{public}d score=%{public}.1f",
                 m.passCount, m.measuredFrames, m.repeats, g_lastScore);
    return out;
}

// ---------------------------------------------------------------------------
//  13. 对外入口: 整段 native 负载包在故障隔离舱里
// ---------------------------------------------------------------------------
//   为什么在这一层(而不是在 snRunImpl 里)装隔离舱 
//    这里是最外层 —— siglongjmp 的目标栈帧在整个负载执行期间都活着, 所以隔离舱能兜住
//    snRunImpl / runFrames / submitPass / snlBuildJson / 以及内层 snPrepare() 里
//    任何一处发生的故障。snPrepare() 自己也有一个隔离舱, 但它看到 tlsDepth != 0
//    (已经被外层接管了)时不重复装 sigsetjmp —— 否则内层的 sigsetjmp 会覆盖外层的
//    跳转目标, 一旦内层返回之后再出错, siglongjmp 就会跳进一个已经死掉的栈帧。
//
//   兜住之后会发生什么(这就是"整轮依然能跑完"的可验证理由)
//    snRun() 正常返回一条 {"ok":false,"error":"GPU-SNL 执行到 [run] 失败: 本小节 native 段
//    发生致命信号(已被隔离舱兜住, 不是 App 崩溃): signal=11 SIGSEGV si_code=1
//    si_addr=0x0 ..."} —— 它是一条普通的返回值, 不是异常、不是进程退出。
//    ArkTS 侧 BenchRunner.runSn() -> parseSn() 把它读成 ok=false, FullRun.runSnSection()
//    把这个小节标成 ST_FAILED 并继续跑后面的 NPU / 存储 I/O 两节, 最后照常写出报告。
//    也就是说: 只要故障发生在隔离舱覆盖的这段 native 代码里, 本轮跑分就不会中断。
std::string snRun(const std::string& optionsJson)
{
    snfault::Guard guard;
    if (guard.outermost()) {
        if (sigsetjmp(snfault::tlsJmp, 1) != 0) {
            const std::string why = snfault::describeLastFault();
            snfault::poison(why);
            snfault::resetAfterFault();
            snAbortEnvSamplingNoLock();
            return failJsonStep("run", why);
        }
    }
    return snRunImpl(optionsJson);
}

