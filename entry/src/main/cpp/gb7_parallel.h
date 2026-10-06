#ifndef AURORA_GB7_PARALLEL_H
#define AURORA_GB7_PARALLEL_H

// 负载内部的并行 executor(仅用于 CS1的"多核"阶段)。
// 设计约束:
//   1) threads <= 1 时退化为串行执行, 结果与单核路径逐位一致(便于单核/多核对照);
//   2) 只做数据分解, 不改变工作量与数据布局, 因此不增加内存占用;
//   3) 动态分块(每块 64 个索引), 避免核心数多于任务数时负载倾斜。
//
//  扩展(2026-10-04, 只增不改): 增加一个可选的"池线程入口回调", 用来给工作线程做
//  CPU 亲和性绑定(大核优先, 见 cpu_affinity.h)。默认值 gb7WorkerStartDefault 只在
//  GB7 CPU 负载的亲和性会话进行中才真正绑核, 会话之外(GPU7 / 旧套件 / 任何未开会话的
//  调用方)它在第一次原子读之后立即返回 —— 也就是说本 header 的**默认语义与历史版本
//  完全一致**: 不绑核、不改分块、不改线程数、不改 join 顺序。调用方若想完全绕开,
//  显式传 nullptr 即可(例如 gb7ParallelFor(t, n, body, nullptr))。
//  计时口径: 回调在线程执行任何负载工作之前调用一次, 只做一次 sched_setaffinity
//  (≈1~3 µs)。本套件的 16 项负载每个最多建几十个池线程(最重的是 Photo Editor:
//  4 张照片 x 3 趟 = 12 个并行池 x 14 线程 ≈ 168 次), 因此新增的 syscall 总量在
//  亚毫秒量级、且绝大部分与父线程的 clone 重叠 —— 对 ≥700 ms 的负载 <0.1%,
//  换来的是"线程不再随机落到小核"。单核路径(这次 1.8 倍抖动的来源)完全不涉及池线程:
//  它的绑核发生在负载 t0 之前, 一个字节都不进 o.ms(见 cpu_affinity.h 的计时口径说明)。
#include "cpu_affinity.h"
#include "cpu_freq_sample.h"   // 池线程 tid 登记/注销(多核频率口径A 的唯一数据源)
#include "qos_priority.h"      // QoS 运行条件(负载线程设高档; 设备不支持时静默降级)

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

// 池线程入口回调类型: 参数 = 线程在本次并行池里的序号(0 起)
using Gb7WorkerStartFn = void (*)(int);

// 默认回调: 绑核(仅在该线程属于一次 GB7 CPU 亲和性会话时生效, 否则空转返回)
inline void gb7WorkerStartDefault(int index)
{
    // QoS(2026-10 新增, 运行条件): 池线程同样是负载线程, 开工前给自己设最高档
    // QOS_USER_INTERACTIVE, 让调度器把 CPU 时间份额给够(官方实测同一段计算低/高档差 2.2 倍)。
    // 位置与绑核完全一致: 在这条线程碰任何负载数据之前; 设备不支持 / canIUse 为假 /
    // 符号拿不到时它返回负值并记账(静默降级), 负载照常跑, 不会抛异常、不影响线程数/分块。
    // QoS 是 per-thread 属性, 线程退出即失效, 因此这里不需要复位。
    // 它不改任何算法/尺寸/metric/k/conv/计分公式, 也不计分。
    (void)auroraQosApplyLoadThread();
    auroraAffinityWorkerStart(index);
    // 多核频率口径A("只统计当刻有负载线程落上的核")靠这一行知道"当期哪些核上有线程":
    // 一次 CAS + 一次 store, 无锁无分配, 在这条线程碰任何负载数据之前完成。
    // 它不改工作量、不改分块、不改线程数, 也不计分。
    (void)auroraFreqRegisterWorkerTid();
}

inline void gb7ParallelFor(int threads, long long n, const std::function<void(long long, long long)>& body,
                           Gb7WorkerStartFn onWorkerStart = gb7WorkerStartDefault)
{
    if (n <= 0) {
        return;
    }
    // 超线程(SMT)开关的线程数上限: 只在"开关关掉且拓扑已知"时生效(= 物理核数),
    // 默认(开关开)时 auroraCapThreads 原样返回, 线程数一个都不改。
    // 为什么池线程数必须 <= 实际使用集合的大小: 池线程的落点是"一个线程一个均衡组",
    // 线程数超过组的个数就会回卷复用, 从而把两个池线程放进同一个物理核 ——
    // 那正是 SMT 开关要避免的事(见 cpu_affinity.h 的 bindWorkerGroup)。
    threads = auroraCapThreads(threads);
    if (threads <= 1) {
        body(0, n);
        return;
    }
    if ((long long)threads > n) {
        threads = (int)n;
    }
    std::atomic<long long> next(0);
    // 块大小固定为 64(GB7_BLOCK): 这是本 header 与所有调用方之间的硬约定 ——
    // "任务级并行"必须用下面的 gb7ParallelTasks(它把索引空间放大 64 倍, 使块边界
    // 与任务边界严格对齐)。若把块大小改成自适应值, 这个对齐关系会被破坏,
    // 导致任务被漏算或重复执行, 因此这里刻意不做自适应。
    const long long chunk = 64;
    std::vector<std::thread> pool;
    pool.reserve((size_t)threads);
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&next, n, &body, t, onWorkerStart]() {
            // 绑核必须在本线程碰任何负载数据之前完成(它不改工作量, 只改"跑在哪个核")
            if (onWorkerStart != nullptr) {
                onWorkerStart(t);
            }
            for (;;) {
                long long start = next.fetch_add(chunk);
                if (start >= n) {
                    break;
                }
                long long end = start + chunk;
                if (end > n) {
                    end = n;
                }
                body(start, end);
            }
            // 干完活之后记一次"结束落点": 线程可能在干活期间被内核挪走, 只报起始落点不足以
            // 证明"每颗核都有人在跑"。这一步只是读一次 sched_getcpu(约 20ns, vDSO)+ 两次原子或,
            // 不改分块、不改任务分配、不改 join 顺序 —— 无会话在跑时它立即返回(默认行为不变)。
            auroraAffinityWorkerEnd(t);
            // 注销自己: 让"当期有线程落上的核"这份表里只留活着的负载线程(见 cpu_freq_sample.h)。
            auroraFreqUnregisterWorkerTid();
        });
    }
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i].join();
    }
}

// 任务级并行: 适用于"任务数可能远小于线程数"的场景(例如 5 个 PDF 页面、17 个视角对)。
// 直接调 gb7ParallelFor(threads, tasks, ...) 在 tasks <= 64 时会退化成单线程,
// 因此这里把索引空间放大 64 倍, 保证每个线程分到的区间恰好对齐到整数个任务。
// body(start, end) 的语义与 gb7ParallelFor 一致(处理任务区间 [start, end))。
// threads <= 1 时直接 body(0, tasks), 与串行循环逐位一致。
inline void gb7ParallelTasks(int threads, long long tasks, const std::function<void(long long, long long)>& body)
{
    if (tasks <= 0) {
        return;
    }
    if (threads <= 1) {
        body(0, tasks);
        return;
    }
    gb7ParallelFor(threads, tasks * 64, [&body](long long start, long long end) {
        body(start / 64, end / 64);
    });
}

// 并行度估计: 线程 CPU 时间 / 墙钟时间。用于 CS1 多核规则(只纳入真正多线程的负载)。
inline double gb7Parallelism(double cpuMs, double wallMs)
{
    if (wallMs <= 0.0) {
        return 0.0;
    }
    return cpuMs / wallMs;
}

#endif
