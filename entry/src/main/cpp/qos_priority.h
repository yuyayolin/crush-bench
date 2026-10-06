#ifndef AURORA_QOS_PRIORITY_H
#define AURORA_QOS_PRIORITY_H

// ===========================================================================
//  QoS(任务服务质量) —— 官方唯一有实测数据支撑的"提高 CPU 时间份额"手段
// ===========================================================================
//
//  为什么加这个文件
//  ---------------------------------------------------------------------------
//  官方最佳实践实测(同一段计算): 低 QoS 726.8 ms vs 高 QoS 323.9 ms = 2.2 倍。
//  它提高的不是频率、也不是核数, 而是调度器分给这个线程的 CPU 时间份额 ——
//  正好对应本机"单核项运行时频率中位只有 1580 MHz / 标称 2270 MHz(约 70%)"这个症状:
//  份额没拿满时, 调频器也不会把频率顶上去。本工程此前从未用过这条路径。
//
//  官方接口在 API 18 SDK 里确实存在(已逐字核对, 不是凭印象写的)
//  ---------------------------------------------------------------------------
//    * 头文件: <qos/qos.h>   @since 12
//        D:\ohos-tools\sdk\18\native\sysroot\usr\include\qos\qos.h
//    * 库:     libqos.so     @kit KernelEnhanceKit
//        sysroot/usr/lib/aarch64-linux-ohos/libqos.so
//    * C API(三个, 一个不多一个不少):
//        int OH_QoS_SetThreadQoS(QoS_Level level);   // 0 成功 / -1 越界或内部失败
//        int OH_QoS_ResetThreadQoS();                // 0 成功 / -1 没设过或内部失败
//        int OH_QoS_GetThreadQoS(QoS_Level *level);  // 0 成功 / -1 level 为空或没设过
//      等级枚举(取值 0..5, 本文件用同名常量逐一对齐, 不 include SDK 头):
//        QOS_BACKGROUND=0 · QOS_UTILITY=1 · QOS_DEFAULT=2 ·
//        QOS_USER_INITIATED=3 · QOS_DEADLINE_REQUEST=4 · QOS_USER_INTERACTIVE=5
//    * 能力名(SysCap): SystemCapability.Resourceschedule.QoS.Core
//
//  官方要求"先用 CanIUse 判断设备能力" —— 在 native 侧怎么落地
//  ---------------------------------------------------------------------------
//    不需要绕回 ArkTS: SDK 的 NDK 里本来就有同名的 C 版本
//        D:\ohos-tools\sdk\18\native\sysroot\usr\include\syscap_ndk.h
//        bool canIUse(const char *cap);          @since 10, @kit BasicServicesKit
//    已用 llvm-readelf 核实: 它在 sysroot 里由 **libdeviceinfo_ndk.z.so** 导出
//    (不是 libc.so —— libc.so 的 dynsym 里没有这个符号)。本文件用 dlopen 拿它,
//    因此不新增任何硬依赖, 拿不到就记原因并在"能力未知"下继续按 dlopen 结果尝试。
//
//  为什么用 dlopen 而不是硬链 libqos.so(与工程对 NNRt 的同一条结论)
//  ---------------------------------------------------------------------------
//    sysroot 里的 libqos.so 是链接桩: 已用 llvm-readelf 核实 ——
//      5: 000000000000173c  4 FUNC GLOBAL DEFAULT 13 OH_QoS_SetThreadQoS
//      6: 000000000000173c  4 FUNC GLOBAL DEFAULT 13 OH_QoS_ResetThreadQoS
//      7: 000000000000173c  4 FUNC GLOBAL DEFAULT 13 OH_QoS_GetThreadQoS
//    三个导出函数指向同一个地址、size 都是 4(一条 ret), 且没有任何 DT_NEEDED。
//    一旦硬链, libaurorabench.so 会带上一条硬 DT_NEEDED libqos.so; 设备上若没有这个
//    .so / soname 对不上, 失效方式是整个 requireNapi("aurorabench") 加载失败 ——
//    CS1 / CoreMark / 旧套件全部一起被拖死。可选功能不该有让主功能加载失败的能力
//    (与 CMakeLists.txt 里 NNRt 那一段的长注释同一条理由)。
//    => dlopen("libqos.so") + dlsym; 拿不到就静默降级 + 记原因。
//
//  本文件做什么 / 不做什么(硬约束)
//  ---------------------------------------------------------------------------
//    * 只改运行条件: 给"跑负载的线程"设高等级(拿更多时间份额)、给"旁路/采样线程"设
//      低等级(不让诊断线程去抢负载的份额)。不改任何负载的算法 / 尺寸 / metric /
//      unit / k / conv / 计分公式 / 线程数口径 / 绑核策略;
//    * 不参与计分: 没有任何一个分数用到这里的数据。报告里只标注
//      "本次跑分是否启用了 QoS 及其等级"(见 auroraQosStatusLine);
//    * 设备不支持 / canIUse 为假 / 符号拿不到 -> 静默降级: 返回负值 + 记原因,
//      负载照常跑, 不抛异常、不中断整轮;
//    * 每个调用的返回值与 errno 都被逐项记下来, 供报告自证;
//    * "设 / 不设" 的性能对照由 qosAbProbeOnce() 给出: 同一段固定整数运算、固定墙钟
//      预算, 比较两种条件下做完的迭代数(迭代数之比就是时间份额之比)。它不是跑分项:
//      不进任何 metric / 分数 / 复合分, 只进报告的一行诊断文本。
//
//  线程口径
//  ---------------------------------------------------------------------------
//    * 负载线程(主线程 + 池线程)  -> QOS_USER_INTERACTIVE(最高档)
//    * 旁路/采样线程(频率采样等) -> QOS_BACKGROUND
//    * QoS 是 **per-thread** 属性: 只有"自己调"才对得自己。池线程在每次干活前自己设一次,
//      线程退出即失效(无需 reset); 主线程是 napi 复用的异步线程, 所以用 RAII 在负载返回后
//      还原(与 cpu_affinity 还原掩码同一个理由: 不复原等于把后面的 GPU7 阶段也改了)。
// ===========================================================================

#include <dlfcn.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace aurora_qos_detail {

// ---- 等级常量: 与 SDK <qos/qos.h> 的 QoS_Level 逐字对齐(0..5) ----
enum AuroraQosLevel {
    kQosBackground      = 0,
    kQosUtility         = 1,
    kQosDefault         = 2,
    kQosUserInitiated   = 3,
    kQosDeadlineRequest = 4,
    kQosUserInteractive = 5
};

// 负载线程 / 旁路线程各用哪一档(改这里就是改运行条件, 不改任何计分)
constexpr int kAuroraQosLoadLevel   = kQosUserInteractive;
constexpr int kAuroraQosBypassLevel = kQosBackground;

// 能力名(与 qos.h 的 @syscap 一致)
constexpr const char* kAuroraQosSyscap = "SystemCapability.Resourceschedule.QoS.Core";

inline const char* qosLevelName(int lv)
{
    switch (lv) {
        case kQosBackground:      return "QOS_BACKGROUND(0)";
        case kQosUtility:         return "QOS_UTILITY(1)";
        case kQosDefault:         return "QOS_DEFAULT(2)";
        case kQosUserInitiated:   return "QOS_USER_INITIATED(3)";
        case kQosDeadlineRequest: return "QOS_DEADLINE_REQUEST(4)";
        case kQosUserInteractive: return "QOS_USER_INTERACTIVE(5)";
        default:                  return "未设(等级未知)";
    }
}

// ---------------------------------------------------------------------------
//  逐"角色"的调用记账(负载线程 / 旁路线程 / Reset)
//  第一次与最后一次的返回值 + errno 都留下 —— 报告里能逐条核对, 不用猜。
// ---------------------------------------------------------------------------
struct AuroraQosChannel {
    std::atomic<int> sets;
    std::atomic<int> ok;
    std::atomic<int> fail;
    int firstRc;
    int firstErrno;
    int lastRc;
    int lastErrno;
    AuroraQosChannel() : sets(0), ok(0), fail(0), firstRc(999), firstErrno(0), lastRc(999), lastErrno(0) {}
};

// ---------------------------------------------------------------------------
//  状态(进程内一份; 全是只读诊断数据 + 计数器)
// ---------------------------------------------------------------------------
struct AuroraQosState {
    // ---- 能力探测 ----
    std::atomic<int> probed;        // 1 = 已探测过
    std::atomic<int> libOk;         // 1 = libqos.so 打开且 3 个符号全部拿到
    int dlopenErrno;                // dlopen 失败时的 errno(0 = 成功)
    char dlopenErr[256];            // dlerror() 原文(, 不加工)
    int haveSet, haveReset, haveGet;
    char missing[192];              // 缺哪些符号(空 = 一个都不缺)
    void* setFn;                    // 解析到的函数指针(dlopen 句柄刻意不关闭, 与调用同生命周期)
    void* resetFn;
    void* getFn;
    int canIUseProbed;              // 1 = 已调用过 CanIUse 等价物
    int canIUseOk;                  // 1 = canIUse() 符号拿到了
    int canIUseErrno;               // 拿不到时的 errno
    char canIUseErr[256];           // dlerror() 原文
    std::atomic<int> capability;    // -1 未知 / 0 不支持 / 1 支持
    // ---- 逐次调用的返回值 ----
    AuroraQosChannel loadCh;        // 负载线程(主线程 + 池线程)
    AuroraQosChannel bypassCh;      // 旁路/采样线程
    AuroraQosChannel resetCh;       // Reset 调用
    std::atomic<int> appliedThreads;// 成功设上 QoS 的线程次数
    std::atomic<int> getCalls;
    std::atomic<int> getOk;
    int getFirstRc, getFirstErrno, getFirstLevel;
    // ---- "设 / 不设" 的性能对照(固定墙钟预算下的迭代数) ----
    int abRan;
    int abBudgetMs;
    long long abLowIter[2];         // 不设 QoS(Reset 后)
    long long abHighIter[2];        // 设 QOS_USER_INTERACTIVE
    int abLowRc[2];
    int abHighRc[2];
    int abLowGot[2];                // 两次对照里 GetThreadQoS 读回的等级(-1 表示读不到)
    int abHighGot[2];
    char abText[640];
    // ---- 给人看的一行 ----
    char text[2560];

    AuroraQosState()
        : probed(0), libOk(0), dlopenErrno(0), dlopenErr(), haveSet(0), haveReset(0), haveGet(0),
          missing(), setFn(nullptr), resetFn(nullptr), getFn(nullptr),
          canIUseProbed(0), canIUseOk(0), canIUseErrno(0), canIUseErr(), capability(-1),
          loadCh(), bypassCh(), resetCh(), appliedThreads(0),
          getCalls(0), getOk(0), getFirstRc(999), getFirstErrno(0), getFirstLevel(-1),
          abRan(0), abBudgetMs(0), abLowIter(), abHighIter(), abLowRc(), abHighRc(),
          abLowGot(), abHighGot(), abText(), text()
    {
        abLowGot[0] = abLowGot[1] = -1;
        abHighGot[0] = abHighGot[1] = -1;
    }
};

// inline 函数里的函数局部 static: 全进程一份(跨 TU 共享, C++ 保证)
inline AuroraQosState& qosState()
{
    static AuroraQosState s;
    return s;
}

// 本线程"我设成功过"的标记(只有设成功过才复位, 避免把别人的设置抹掉)
inline bool& qosThreadApplied()
{
    static thread_local bool v = false;
    return v;
}

typedef int (*QosSetFn)(int level);
typedef int (*QosResetFn)(void);
typedef int (*QosGetFn)(int* level);
typedef bool (*CanIUseFn)(const char* cap);

// ---- 符号表补齐(用一个空字符分隔的列表记"缺了哪些") ----
inline void noteMissing(AuroraQosState& s, const char* name)
{
    const size_t used = strlen(s.missing);
    if (used + 1 >= sizeof(s.missing)) {
        return;
    }
    snprintf(s.missing + used, sizeof(s.missing) - used, "%s%s", (used > 0) ? " " : "", name);
}

// ---- dlopen 探测(只做一次; 失败也把原因记满) ----
inline bool qosEnsureProbed()
{
    AuroraQosState& s = qosState();
    if (s.probed.load(std::memory_order_acquire) != 0) {
        return s.libOk.load(std::memory_order_acquire) != 0;
    }
    s.probed.store(1, std::memory_order_release);

    // ---- ① 能力判定(官方要求): canIUse(SystemCapability.Resourceschedule.QoS.Core) ----
    s.canIUseProbed = 1;
    errno = 0;
    void* dHandle = dlopen("libdeviceinfo_ndk.z.so", RTLD_NOW | RTLD_LOCAL);
    if (dHandle == nullptr) {
        s.canIUseOk = 0;
        s.canIUseErrno = errno != 0 ? errno : -1;
        const char* e = dlerror();
        snprintf(s.canIUseErr, sizeof(s.canIUseErr), "dlopen(libdeviceinfo_ndk.z.so) 失败: %s",
                 e != nullptr ? e : "(dlerror 无输出)");
        s.capability.store(-1, std::memory_order_release);
    } else {
        CanIUseFn f = reinterpret_cast<CanIUseFn>(dlsym(dHandle, "canIUse"));
        if (f == nullptr) {
            s.canIUseOk = 0;
            s.canIUseErrno = errno != 0 ? errno : -1;
            const char* e = dlerror();
            snprintf(s.canIUseErr, sizeof(s.canIUseErr), "dlsym(canIUse) 失败: %s",
                     e != nullptr ? e : "(dlerror 无输出)");
            s.capability.store(-1, std::memory_order_release);
        } else {
            s.canIUseOk = 1;
            errno = 0;
            const bool got = f(kAuroraQosSyscap);
            s.capability.store(got ? 1 : 0, std::memory_order_release);
            s.canIUseErrno = errno;
        }
    }

    // ---- ② 接口探测: dlopen("libqos.so") + 三个符号 ----
    errno = 0;
    void* h = dlopen("libqos.so", RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        s.libOk.store(0, std::memory_order_release);
        s.dlopenErrno = errno != 0 ? errno : -1;
        const char* e = dlerror();
        snprintf(s.dlopenErr, sizeof(s.dlopenErr), "dlopen(libqos.so) 失败: %s",
                 e != nullptr ? e : "(dlerror 无输出)");
        return false;
    }
    // 句柄刻意不 dlclose: 函数指针的生命周期与进程同级(与 npu_bench.cpp 同做法)。
    // 关掉句柄会让函数指针悬空 —— 那时失效方式就从"返回错误码"变成"跳进野地址"。
    s.setFn = reinterpret_cast<void*>(dlsym(h, "OH_QoS_SetThreadQoS"));
    s.resetFn = reinterpret_cast<void*>(dlsym(h, "OH_QoS_ResetThreadQoS"));
    s.getFn = reinterpret_cast<void*>(dlsym(h, "OH_QoS_GetThreadQoS"));
    s.haveSet = (s.setFn != nullptr) ? 1 : 0;
    s.haveReset = (s.resetFn != nullptr) ? 1 : 0;
    s.haveGet = (s.getFn != nullptr) ? 1 : 0;
    if (s.haveSet == 0) { noteMissing(s, "OH_QoS_SetThreadQoS"); }
    if (s.haveReset == 0) { noteMissing(s, "OH_QoS_ResetThreadQoS"); }
    if (s.haveGet == 0) { noteMissing(s, "OH_QoS_GetThreadQoS"); }
    const bool ok = (s.haveSet && s.haveReset && s.haveGet);
    s.libOk.store(ok ? 1 : 0, std::memory_order_release);
    return ok;
}

// ---- 原始调用(不记账; 供内部与对照探测使用) ----
//  返回: 0 = 接口返回 0; 其它 = 接口返回的原值; -2 = 设备不支持(库/符号拿不到)
inline int qosRawSet(int level)
{
    AuroraQosState& s = qosState();
    if (s.libOk.load(std::memory_order_acquire) == 0 || s.setFn == nullptr) {
        return -2;
    }
    errno = 0;
    return reinterpret_cast<QosSetFn>(s.setFn)(level);
}

inline int qosRawReset()
{
    AuroraQosState& s = qosState();
    if (s.libOk.load(std::memory_order_acquire) == 0 || s.resetFn == nullptr) {
        return -2;
    }
    errno = 0;
    return reinterpret_cast<QosResetFn>(s.resetFn)();
}

// 读回当前线程的等级: >= 0 = 等级; -1 = 没设过/读失败; -2 = 设备不支持
inline int qosRawGet()
{
    AuroraQosState& s = qosState();
    if (s.libOk.load(std::memory_order_acquire) == 0 || s.getFn == nullptr) {
        return -2;
    }
    int lv = -1;
    errno = 0;
    if (reinterpret_cast<QosGetFn>(s.getFn)(&lv) != 0) {
        return -1;
    }
    return lv;
}

inline void channelRecord(AuroraQosChannel& c, int rc, int e)
{
    c.sets.fetch_add(1, std::memory_order_relaxed);
    if (rc == 0) {
        c.ok.fetch_add(1, std::memory_order_relaxed);
    } else {
        c.fail.fetch_add(1, std::memory_order_relaxed);
    }
    if (c.firstRc == 999) {
        c.firstRc = rc;
        c.firstErrno = e;
    }
    c.lastRc = rc;
    c.lastErrno = e;
}

// ---- 对"当前线程"设等级(带完整记账) ----
//  返回: 0 = 成功; -1 = 接口返回 -1; -2 = 设备不支持(库/符号拿不到);
//        -3 = canIUse 明确为假(按官方要求不调用该能力); -4 = 等级越界(调用方 bug)
inline int qosApplyToCurrentThread(int level, AuroraQosChannel& ch)
{
    AuroraQosState& s = qosState();
    if (level < kQosBackground || level > kQosUserInteractive) {
        channelRecord(ch, -4, 0);
        return -4;
    }
    if (!qosEnsureProbed()) {
        channelRecord(ch, -2, s.dlopenErrno);
        return -2;
    }
    if (s.capability.load(std::memory_order_acquire) == 0) {
        // 官方要求: canIUse 为假 -> 不调用该能力(调用也只会失败, 还会在日志里刷错误)
        channelRecord(ch, -3, 0);
        return -3;
    }
    errno = 0;
    const int rc = qosRawSet(level);
    const int e = errno;
    channelRecord(ch, rc, e);
    if (rc == 0) {
        qosThreadApplied() = true;
        s.appliedThreads.fetch_add(1, std::memory_order_relaxed);
    }
    return rc;
}

// ---------------------------------------------------------------------------
//  固定工作量 / 固定墙钟预算的自证探测(不是跑分项)
// ---------------------------------------------------------------------------
//  为什么要有它: 光看"设置返回 0"只证明调用成功, 不证明真的拿到了更多时间份额。
//  这里用同一段纯整数运算 + 固定墙钟预算, 比较两种条件下做完的迭代数; 迭代数之比
//  就是调度器给这个线程的时间份额之比(官方那条 2.2 倍实测用的也是这个思路)。
//  它没有 metric、没有 k、没有 conv、不进任何分数与复合分, 只进报告的一行诊断文本。
inline volatile unsigned long long& qosSpinSink()
{
    static volatile unsigned long long v = 0;
    return v;
}

inline long long qosSpinForNs(long long budgetNs)
{
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    unsigned long long x = 0x9e3779b97f4a7c15ull;
    long long iters = 0;
    for (;;) {
        // 固定的一小段运算(64 步): 每次循环的工作量恒定, 且是数据依赖链, 不会被优化掉
        for (int k = 0; k < 64; ++k) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            x += 0x9e3779b97f4a7c15ull;
        }
        ++iters;
        if ((iters & 0x3f) == 0) {
            const long long el = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t0).count();
            if (el >= budgetNs) {
                break;
            }
        }
    }
    qosSpinSink() = x;
    return iters;
}

// 对照探测: 只跑一次(第一次 GB7 会话开始时), 约 4 x 150 ms, 全部在计时区间之外。
inline void qosAbProbeOnce()
{
    AuroraQosState& s = qosState();
    if (s.abRan != 0) {
        return;
    }
    s.abRan = 1;
    s.abBudgetMs = 150;
    if (!qosEnsureProbed()) {
        snprintf(s.abText, sizeof(s.abText), "设/不设对照: 没做(libqos.so 不可用: %s)",
                 s.dlopenErr[0] ? s.dlopenErr : "原因未记录");
        return;
    }
    if (s.capability.load(std::memory_order_acquire) == 0) {
        snprintf(s.abText, sizeof(s.abText),
                 "设/不设对照: 没做(canIUse(\"%s\") = false, 按官方要求不调用该能力)",
                 kAuroraQosSyscap);
        return;
    }
    const long long budgetNs = (long long)s.abBudgetMs * 1000000LL;
    for (int r = 0; r < 2; ++r) {
        // ① 不设(先复位, 回到系统默认档): 这就是"不设 QoS"那一侧
        s.abLowRc[r] = qosRawReset();
        s.abLowGot[r] = qosRawGet();
        s.abLowIter[r] = qosSpinForNs(budgetNs);
        // ② 设最高档: 这就是"设 QoS"那一侧
        s.abHighRc[r] = qosRawSet(kAuroraQosLoadLevel);
        s.abHighGot[r] = qosRawGet();
        s.abHighIter[r] = qosSpinForNs(budgetNs);
    }
    (void)qosRawReset();   // 探测结束后还原, 不把这次设置留给后面的负载
    qosThreadApplied() = false;
    const long long lo = (s.abLowIter[0] > s.abLowIter[1]) ? s.abLowIter[0] : s.abLowIter[1];
    const long long hi = (s.abHighIter[0] > s.abHighIter[1]) ? s.abHighIter[0] : s.abHighIter[1];
    if (lo <= 0) {
        snprintf(s.abText, sizeof(s.abText),
                 "设/不设对照: 不设那一侧的迭代数为 0(探测无效), 不给出比值");
        return;
    }
    snprintf(s.abText, sizeof(s.abText),
             "设/不设对照(同一段固定整数运算, 每次 %d ms 墙钟预算, 比做完的迭代数, "
             "取两轮里各自较高的一次): 不设=%lld/%lld 迭代(reset rc=%d/%d, 读回等级=%d/%d) · "
             "设 %s=%lld/%lld 迭代(set rc=%d/%d, 读回等级=%d/%d) -> 份额比(设/不设) = %.4f",
             s.abBudgetMs,
             s.abLowIter[0], s.abLowIter[1], s.abLowRc[0], s.abLowRc[1], s.abLowGot[0], s.abLowGot[1],
             qosLevelName(kAuroraQosLoadLevel),
             s.abHighIter[0], s.abHighIter[1], s.abHighRc[0], s.abHighRc[1], s.abHighGot[0], s.abHighGot[1],
             (double)hi / (double)lo);
}

// ---- 重算那一行给人看的文本(每次取之前都刷新, 数字永远是最新的) ----
inline void qosRefreshText()
{
    AuroraQosState& s = qosState();
    const int loadSets = s.loadCh.sets.load(std::memory_order_relaxed);
    const int loadOk = s.loadCh.ok.load(std::memory_order_relaxed);
    const int loadFail = s.loadCh.fail.load(std::memory_order_relaxed);
    const int bySets = s.bypassCh.sets.load(std::memory_order_relaxed);
    const int byOk = s.bypassCh.ok.load(std::memory_order_relaxed);
    const int byFail = s.bypassCh.fail.load(std::memory_order_relaxed);
    const int rsSets = s.resetCh.sets.load(std::memory_order_relaxed);
    const int rsOk = s.resetCh.ok.load(std::memory_order_relaxed);
    const int rsFail = s.resetCh.fail.load(std::memory_order_relaxed);

    char libPart[512];
    if (s.probed.load(std::memory_order_acquire) == 0) {
        snprintf(libPart, sizeof(libPart), "libqos.so: 尚未探测");
    } else if (s.libOk.load(std::memory_order_acquire) != 0) {
        snprintf(libPart, sizeof(libPart),
                 "libqos.so: dlopen 成功, 3 个符号全部拿到"
                 "(OH_QoS_SetThreadQoS / OH_QoS_ResetThreadQoS / OH_QoS_GetThreadQoS)");
    } else if (s.missing[0] != 0) {
        snprintf(libPart, sizeof(libPart), "libqos.so: 不可用 —— dlopen 成功但缺符号: %s", s.missing);
    } else {
        snprintf(libPart, sizeof(libPart), "libqos.so: 不可用 —— %s(errno=%d)",
                 s.dlopenErr[0] ? s.dlopenErr : "原因未记录", s.dlopenErrno);
    }

    char capPart[512];
    if (s.canIUseProbed == 0) {
        snprintf(capPart, sizeof(capPart), "canIUse: 尚未调用");
    } else if (s.canIUseOk == 0) {
        snprintf(capPart, sizeof(capPart),
                 "canIUse: 拿不到(%s, errno=%d) -> 能力按未知处理, 仍按 dlopen 结果尝试",
                 s.canIUseErr[0] ? s.canIUseErr : "原因未记录", s.canIUseErrno);
    } else {
        const int cap = s.capability.load(std::memory_order_acquire);
        snprintf(capPart, sizeof(capPart), "canIUse(\"%s\") = %s",
                 kAuroraQosSyscap,
                 (cap == 1) ? "true(设备支持)" : (cap == 0 ? "false(设备不支持 -> 不调用该能力)" : "未知"));
    }

    char getPart[256];
    const int gc = s.getCalls.load(std::memory_order_relaxed);
    if (gc == 0) {
        snprintf(getPart, sizeof(getPart), "GetThreadQoS: 未调用");
    } else {
        snprintf(getPart, sizeof(getPart),
                 "GetThreadQoS: 调用 %d 次 / 成功 %d 次(首次 rc=%d errno=%d 读回等级=%d)",
                 gc, s.getOk.load(std::memory_order_relaxed), s.getFirstRc, s.getFirstErrno,
                 s.getFirstLevel);
    }

    const bool enabled = (loadOk > 0);
    snprintf(s.text, sizeof(s.text),
             "QoS(运行条件, 不计分): 本次%s · 负载线程等级=%s(调用 %d 次: 成功 %d / 失败 %d; "
             "首次 rc=%d errno=%d, 末次 rc=%d errno=%d) · 旁路/采样线程等级=%s(调用 %d 次: 成功 %d / 失败 %d; "
             "首次 rc=%d errno=%d) · Reset 调用 %d 次(成功 %d / 失败 %d; 首次 rc=%d errno=%d) · "
             "成功设上 QoS 的线程次数=%d · %s · %s · %s",
             enabled ? "已启用 QoS" : "未启用 QoS(原因见下)",
             qosLevelName(kAuroraQosLoadLevel), loadSets, loadOk, loadFail,
             s.loadCh.firstRc, s.loadCh.firstErrno, s.loadCh.lastRc, s.loadCh.lastErrno,
             qosLevelName(kAuroraQosBypassLevel), bySets, byOk, byFail,
             s.bypassCh.firstRc, s.bypassCh.firstErrno,
             rsSets, rsOk, rsFail, s.resetCh.firstRc, s.resetCh.firstErrno,
             s.appliedThreads.load(std::memory_order_relaxed),
             libPart, capPart, getPart);
    if (s.abRan != 0 && s.abText[0] != 0) {
        const size_t used = strlen(s.text);
        const size_t need = strlen(s.abText);
        if (used + 3 + need + 1 <= sizeof(s.text)) {
            snprintf(s.text + used, sizeof(s.text) - used, " · %s", s.abText);
        }
    }
}

} // namespace aurora_qos_detail

// ===========================================================================
//  对外接口(全是 inline; 名字统一 auroraQos*)
// ===========================================================================

// 负载线程(主线程 / 池线程)调: 设最高档。返回 0 = 成功, 负数 = 降级原因(见 qosApplyToCurrentThread)。
inline int auroraQosApplyLoadThread()
{
    return aurora_qos_detail::qosApplyToCurrentThread(aurora_qos_detail::kAuroraQosLoadLevel,
                                                      aurora_qos_detail::qosState().loadCh);
}

// 旁路/采样线程调: 设最低档(别去抢负载的时间份额)。
inline int auroraQosApplyBypassThread()
{
    return aurora_qos_detail::qosApplyToCurrentThread(aurora_qos_detail::kAuroraQosBypassLevel,
                                                      aurora_qos_detail::qosState().bypassCh);
}

// 复位当前线程的 QoS(只有"我设成功过"才复位; 否则会把别人的设置抹掉)。
inline void auroraQosResetCurrentThread()
{
    if (!aurora_qos_detail::qosThreadApplied()) {
        return;
    }
    errno = 0;
    const int rc = aurora_qos_detail::qosRawReset();
    const int e = errno;
    aurora_qos_detail::channelRecord(aurora_qos_detail::qosState().resetCh, rc, e);
    if (rc == 0) {
        aurora_qos_detail::qosThreadApplied() = false;
    }
}

// 读回当前线程的等级(顺带记账)。返回等级 0..5; -1 = 没设过/读不到; -2 = 接口不可用。
inline int auroraQosGetCurrentThread()
{
    aurora_qos_detail::AuroraQosState& s = aurora_qos_detail::qosState();
    (void)aurora_qos_detail::qosEnsureProbed();
    const int lv = aurora_qos_detail::qosRawGet();
    s.getCalls.fetch_add(1, std::memory_order_relaxed);
    if (lv >= 0) {
        s.getOk.fetch_add(1, std::memory_order_relaxed);
    }
    if (s.getFirstRc == 999) {
        s.getFirstRc = (lv >= 0) ? 0 : -1;
        s.getFirstErrno = errno;
        s.getFirstLevel = (lv >= 0) ? lv : -1;
    }
    return lv;
}

// GB7 会话(每一项)开始: ① 首次做一次"设/不设"对照; ② 给当前(负载)线程设最高档。
// 时序: 严格在负载函数的 t0 之前, 与绑核会话同一位置 —— 它的开销一分钱都不会进 o.ms。
inline void auroraQosLoadSessionBegin()
{
    aurora_qos_detail::qosAbProbeOnce();
    (void)auroraQosApplyLoadThread();
}

// GB7 会话(每一项)结束: 还原当前线程的 QoS(napi 异步工作线程会被复用)。
inline void auroraQosLoadSessionEnd()
{
    auroraQosResetCurrentThread();
}

// 一行文本(报告/日志用; 每次调用都刷新, 数字是最新的)
inline std::string auroraQosStatusLine()
{
    aurora_qos_detail::qosRefreshText();
    return std::string(aurora_qos_detail::qosState().text);
}

#endif  // AURORA_QOS_PRIORITY_H
