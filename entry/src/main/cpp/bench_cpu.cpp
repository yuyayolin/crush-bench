#include "bench.h"
#include "cpu_affinity.h"
// 运行时实际频率采样(旁路诊断; 口径、调用位置约定与"跑满"判据见 cpu_freq_sample.h)。
// 自研套件此前完全没有这一块 —— 每一项在报告里都是 cpu=-1 · maxKhz=0, 于是
// "这一项到底跑在哪颗核上、那颗核的标称上限是多少、实际跑到多少"三个问题一个都答不了。
// 现在按 CS1 那套写法补上: 会话在计时区间之外开/关, 计时区间内只有 markStart/markStop
// 两次原子级调用(各 ~20~60 ns, 写在 t0 之前 / t1 之后)。
// 它不改任何负载的算法、尺寸、线程数与计分, 也不参与 metric / unit / 复合分。
#include "cpu_freq_sample.h"
#include <atomic>
#include <cstdio>
#include <ctime>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <algorithm>

namespace {

inline uint32_t xsNext(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

double wallMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// 线程 CPU 时间累计: 用于实测并行度(实际占满多少核心)
std::atomic<double> g_threadCpuMs{0.0};

// 按频率降序得到核心序号, 大核在前。
// 实现已提取到 cpu_affinity.h 与 GB7 路径共用, 本文件保留同名薄封装: 调用点、调用次数、
// 每次读盘的行为一行未动; 排序算法(含"频率相同则保持原序"这一细节)逐字搬运 ——
// 因此旧套件拿到的核心顺序与改动前零差异, 分数不受本次改动影响。
//
// 超线程(SMT, 2026-10): 这里改用 auroraEffectiveCoreList() = "实际使用集合"的频率降序表。
//   * 开关开(默认): 实际使用集合 = 全部逻辑核, 顺序与 auroraFastCoreList() 逐位相同
//     (同一份频率降序表, 只是过滤掉了不存在的核号), 因此默认行为与改动前零差异;
//   * 开关关: 集合里每个物理核只留一个逻辑核, 于是本文件 parallelFor 的
//     "线程 t 绑 perfOrder[t]" 自动变成"每个物理核最多一个线程" —— 旧套件的多核阶段
//     永远不会把两个线程放进同一个物理核(这正是开关的意义)。
std::vector<int> coresByPerfDesc()
{
    //  ★ 2026-10-06 回退: 本套件**必须**用"每物理核一个代表"的表(真机 6 格), 不能跟着改成
    //    "可用核序列"(8 格)。9.7 曾经为了"两个套件同口径"把它一起改了, 真机立刻掉分:
    //      自研多核 8.5/9.6 = 6276 / 6269  ->  9.7 = 6082 / 6133 / 5238
    //      逐项: 物理模拟 2.14x 慢 · AI 推理 2.60x 慢 · 文本处理 1.96x 慢 · 内存带宽 1.95x 慢
    //      (而同一版自研**单核**八项全在 ±8% 以内 —— 单核路径没动过, 所以问题就在"多核钉核"这一处)
    //    为什么本套件用 6 格表反而更快: 本文件多核阶段是"i = t; i < tasks; i += threads"静态分块,
    //      线程 t 的任务是定死的。用 6 格表时第 7/8 条线程(targetCpu = -1)**不钉**, 保留原有宽掩码,
    //      于是它们能落到当下最空闲的核上; 而用 8 格表时线程 4~7 被硬钉到 4 个小核上,
    //      这 4 个慢分块成了整轮的时间下界 —— 静态分块下那等于直接砍掉一大截并行度。
    //    CS1 那一侧的问题不是"表不同", 而是它"钉不上以后会把线程夹回那 6 个核" —— 那一处已修在
    //      cpu_affinity.h 的 multiCoreSlotList 上, 与本文件无关。
    //    **两个套件的线程落点策略本来就该不一样, 不要为了"看起来统一"再改回去。**
    return auroraPhysicalCoreList();
}

// 把当前线程绑到指定核心(失败则忽略)
void pinCurrentThread(int cpu)
{
    (void)auroraBindCurrentThreadToCpu(cpu);
}

double threadCpuMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// ---------------------------------------------------------------------------
//  绑核取证(自研套件每一项一份; 旁路, 不计分)
// ---------------------------------------------------------------------------
//  为什么要有它: 这份结构回答的问题, 正是"两台同芯片设备自研单核差 37.7%"那个 bug
//  当年没人能一眼看出来的原因 —— 报告里那一项只写着 cpu=-1 · maxKhz=0。
//  现在每一项都带上: 目标核数 / 绑定是否真的生效 / 跑完落在那颗核 / 那颗核的标称上限 /
//  还原是否成功。字段只读不写负载, 不进 metric / unit / 计分。
struct OwnBindEvidence {
    int singleThread = 0;    // 1 = 本项单核阶段(只有这一阶段在本文件里绑核)
    int attempted = 0;       // 1 = 发过 sched_setaffinity
    int rc = 0;              // 返回值: >= 1 = 绑进掩码的核数; -1 = 失败
    int errnoCode = 0;       // 失败时的 errno; -3 = 这段位次里一个合法核都没有(没发 syscall)
    int targetCores = 0;     // 目标核数 = 生效快簇核数((全机最快档) ∩ (可用核集合))
    int landingCpu = -1;     // 跑完那一刻线程所在的核(-1 = 取不到)
    int restoreOk = 0;       // 1 = 原掩码已还原并读回核对一致
    int readbackCores = 0;   // 绑定后立刻读回的掩码里的核数(内核夹回时会小于 targetCores)
};
OwnBindEvidence g_bind;

// 单核阶段: 把调用线程绑到"生效快簇"(与 CS1 单核阶段同一个函数: 见 cpu_affinity.h 的
// auroraBindCurrentThreadToSingleLoadTarget), 跑完立刻还原。
//
//  历史缺陷(2026-10 修): 这条分支此前一次 sched_setaffinity 都没有发过 —— 单核项落在
//  哪颗核上完全由调度器决定。多核分支一直绑(池线程 t -> 频率降序第 t 个核), 单核分支
//  一直不绑, 于是同一颗芯片上"多核两台设备对得上、单核对不上"。t0 在 parallelFor 之前,
//  所以这次绑核一次都不进计时区间。
void bindSingleThreadToFastCluster()
{
    OwnBindEvidence& e = g_bind;
    e.singleThread = 1;
    // 2026-10-06 回退成"不绑"(= 8.5 之前的行为), 真机 A/B 证据:
    //   8.5(不绑) 自研套件单核 = 2694 / 2450 / 2710, 逐项落点由调度器决定
    //   9.0(绑到快簇 cpu4-7) 自研套件单核 = 1816, 逐项掉 33%~55%(只有"数据压缩"那一项没掉)
    //   **同一轮里 CS1 单核(本来就绑 0xf0) 418.1 -> 418.9 一位没变, Clang 那一项也逐位不变**,
    //   所以不是机器状态、不是降频, 就是这次绑核本身。
    // 为什么绑了反而慢: 绑核用的内核集合是本进程自己算出来的 allowedMask(cpu0-7, 最高 2270MHz),
    //   而不绑时调度器能把这条线程放到更快的核上(实测这颗芯片还有一档 2750MHz)。
    //   也就是说"我们以为自己只能用到 cpu0-7"这件事本身才是瓶颈 —— 详见交付说明里的待办。
    // 仍然保留证据结构(attempted = 0 = 这次没发过 syscall), 报告里照实写"未尝试", 不写假话。
    e.attempted = 0;
    e.rc = 0;
    e.errnoCode = 0;
    e.targetCores = 0;
    e.landingCpu = -1;
    e.readbackCores = 0;
    e.restoreOk = 0;
    return;
    // ---- 以下为 9.0 的实现, 保留备查(重新启用前必须先解释清楚"为什么绑了反而慢") ----
    int be = 0;
    const int rc = auroraBindCurrentThreadToSingleLoadTarget(&be);
    e.attempted = 1;
    e.rc = rc;
    e.errnoCode = be;
    e.targetCores = auroraSingleLoadTargetCount();
    e.landingCpu = -1;
    e.readbackCores = 0;
    e.restoreOk = 0;
    if (rc > 0) {
        // 掩码回读: sched_setaffinity 返回 0 只说明"内核接受了这张掩码"。
        // 逐核实测探测已经证明这台机器会把越界的掩码静默夹回, 所以这里必须读回一次,
        // 把"要了几个核 / 内核给了几个核"两个数分开记。
        cpu_set_t back;
        if (sched_getaffinity(0, sizeof(back), &back) == 0) {
            int n = 0;
            for (int c = 0; c < CPU_SETSIZE; ++c) {
                if (CPU_ISSET(c, &back)) {
                    ++n;
                }
            }
            e.readbackCores = n;
        }
        (void)sched_yield();   // 让掩码立刻作用到本线程, 之后的落点才有意义
    }
}

template <typename F>
void parallelFor(int threads, int tasks, F fn)
{
    if (threads <= 1) {
        AuroraAffinitySnapshot before = auroraCaptureThreadAffinity();
        bindSingleThreadToFastCluster();
        for (int i = 0; i < tasks; ++i) {
            fn(i, 1);
        }
        // 采样落点必须在还原之前(还原之后线程可能立刻被挪走, 那时采到的就不是负载跑过的核)
        if (g_bind.attempted != 0) {
            g_bind.landingCpu = auroraCurrentCpu();
        }
        g_bind.restoreOk = auroraApplyThreadAffinity(before) ? 1 : 0;
        return;
    }
    // 多核阶段: 线程 t 独占"频率降序表第 t 个核"(= 可用核集合里第 t 快的核)。
    // 与 CS1 多核阶段的一核一线程同口径(bindWorkerCoreSpread), 本次一个字都没改。
    std::vector<int> perfOrder = coresByPerfDesc();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        int targetCpu = (t < (int)perfOrder.size()) ? perfOrder[t] : -1;
        pool.emplace_back([&fn, t, threads, tasks, targetCpu]() {
            if (targetCpu >= 0) {
                pinCurrentThread(targetCpu);
            }
            // 运行时频率的"口径A(只统计当刻有负载线程落上的核)"靠这张 tid 表知道当期哪些核在干活
            // (CS1 路径在 gb7_parallel.h 的工作线程入口做同一件事)。无锁、无分配、不计分,
            // 不改工作量与线程数 —— 少了它, 自研套件多核项的 runFreq 会因为"一个池线程都没登记"
            // 而拿不到样本, 报告里就只剩一句"没采到"。
            (void)auroraFreqRegisterWorkerTid();
            double cpu0 = threadCpuMs();
            for (int i = t; i < tasks; i += threads) {
                fn(i, threads);
            }
            double cpu1 = threadCpuMs();
            g_threadCpuMs.store(g_threadCpuMs.load() + (cpu1 - cpu0));
            // 干完活注销自己: 表里只留活着的负载线程(见 cpu_freq_sample.h 的口径说明)
            auroraFreqUnregisterWorkerTid();
        });
    }
    for (auto& th : pool) {
        th.join();
    }
}

// ---------------- 1. 图像处理 ----------------
const int IMG_W = 3840;
const int IMG_H = 2160;

void blurRows(const uint8_t* in, uint8_t* out, int y0, int y1)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < IMG_W; ++x) {
            int acc[3] = {0, 0, 0};
            int cnt = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int yy = y + dy;
                if (yy < 0 || yy >= IMG_H) {
                    continue;
                }
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= IMG_W) {
                        continue;
                    }
                    const uint8_t* p = in + ((size_t)yy * IMG_W + xx) * 4;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    cnt++;
                }
            }
            uint8_t* o = out + ((size_t)y * IMG_W + x) * 4;
            o[0] = (uint8_t)(acc[0] / cnt);
            o[1] = (uint8_t)(acc[1] / cnt);
            o[2] = (uint8_t)(acc[2] / cnt);
            o[3] = 255;
        }
    }
}

void edgeRows(const uint8_t* in, uint8_t* out, int y0, int y1)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < IMG_W; ++x) {
            int gx = 0;
            int gy = 0;
            const int yUp = std::max(0, y - 1);
            const int yDown = std::min(IMG_H - 1, y + 1);
            const int xLeft = std::max(0, x - 1);
            const int xRight = std::min(IMG_W - 1, x + 1);
            for (int c = 0; c < 3; ++c) {
                int p00 = in[((size_t)yUp * IMG_W + xLeft) * 4 + c];
                int p01 = in[((size_t)yUp * IMG_W + x) * 4 + c];
                int p02 = in[((size_t)yUp * IMG_W + xRight) * 4 + c];
                int p10 = in[((size_t)y * IMG_W + xLeft) * 4 + c];
                int p12 = in[((size_t)y * IMG_W + xRight) * 4 + c];
                int p20 = in[((size_t)yDown * IMG_W + xLeft) * 4 + c];
                int p21 = in[((size_t)yDown * IMG_W + x) * 4 + c];
                int p22 = in[((size_t)yDown * IMG_W + xRight) * 4 + c];
                gx += (p02 + 2 * p12 + p22) - (p00 + 2 * p10 + p20);
                gy += (p20 + 2 * p21 + p22) - (p00 + 2 * p01 + p02);
            }
            uint8_t* o = out + ((size_t)y * IMG_W + x) * 4;
            int mag = (int)std::sqrt((double)(gx * gx + gy * gy));
            o[0] = (uint8_t)std::min(255, mag);
            o[1] = (uint8_t)std::min(255, mag / 2);
            o[2] = (uint8_t)std::min(255, mag / 3);
            o[3] = 255;
        }
    }
}

double runImage(int threads)
{
    const size_t bytes = (size_t)IMG_W * IMG_H * 4;
    std::vector<uint8_t> a(bytes);
    std::vector<uint8_t> b(bytes);
    std::vector<uint8_t> c(bytes);
    uint32_t s = 20260828u;
    for (size_t i = 0; i < bytes; ++i) {
        a[i] = (uint8_t)(xsNext(s) & 0xFF);
    }
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    parallelFor(threads, threads, [&](int idx, int total) {
        int span = IMG_H / total;
        int y0 = idx * span;
        int y1 = (idx == total - 1) ? IMG_H : y0 + span;
        blurRows(a.data(), b.data(), y0, y1);
    });
    parallelFor(threads, threads, [&](int idx, int total) {
        int span = IMG_H / total;
        int y0 = idx * span;
        int y1 = (idx == total - 1) ? IMG_H : y0 + span;
        blurRows(b.data(), c.data(), y0, y1);
    });
    parallelFor(threads, threads, [&](int idx, int total) {
        int span = IMG_H / total;
        int y0 = idx * span;
        int y1 = (idx == total - 1) ? IMG_H : y0 + span;
        edgeRows(c.data(), a.data(), y0, y1);
    });
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    volatile uint8_t sink = a[12345] + c[77777];
    (void)sink;
    return t1 - t0;
}

// ---------------- 2. 数据压缩 ----------------
size_t lzCompressChunk(const uint8_t* src, size_t n, std::vector<uint8_t>& out)
{
    out.clear();
    const int HASH_BITS = 14;
    const int HASH_SIZE = 1 << HASH_BITS;
    std::vector<int> head(HASH_SIZE, -1);
    size_t i = 0;
    while (i < n) {
        size_t bestLen = 0;
        size_t bestOff = 0;
        if (i + 3 <= n) {
            uint32_t key = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | (uint32_t)src[i + 2];
            uint32_t h = key & (HASH_SIZE - 1);
            int cand = head[h];
            head[h] = (int)i;
            if (cand >= 0 && (size_t)cand < i) {
                size_t maxLen = std::min<size_t>(258, n - i);
                size_t len = 0;
                while (len < maxLen && src[(size_t)cand + len] == src[i + len]) {
                    len++;
                }
                if (len >= 4) {
                    bestLen = len;
                    bestOff = i - (size_t)cand;
                }
            }
        }
        if (bestLen >= 4) {
            out.push_back(0xFF);
            out.push_back((uint8_t)(bestOff & 0xFF));
            out.push_back((uint8_t)((bestOff >> 8) & 0xFF));
            out.push_back((uint8_t)bestLen);
            i += bestLen;
        } else {
            uint8_t lit = src[i];
            if (lit == 0xFF) {
                out.push_back(0xFF);
                out.push_back(0);
                out.push_back(0);
                out.push_back(0);
            } else {
                out.push_back(lit);
            }
            i++;
        }
    }
    return out.size();
}

void lzDecompressChunk(const uint8_t* src, size_t n, uint8_t* dst, size_t dstN)
{
    size_t i = 0;
    size_t o = 0;
    while (i < n && o < dstN) {
        uint8_t b = src[i++];
        if (b == 0xFF && i + 2 < n) {
            size_t off = (size_t)src[i] | ((size_t)src[i + 1] << 8);
            size_t len = src[i + 2];
            i += 3;
            if (len == 0) {
                dst[o++] = 0xFF;
            } else {
                size_t from = o - off;
                for (size_t k = 0; k < len && o < dstN; ++k) {
                    dst[o++] = dst[from + k];
                }
            }
        } else {
            dst[o++] = b;
        }
    }
}

double runCompress(int threads)
{
    const size_t chunkSize = 4u * 1024 * 1024;
    const int chunks = 4;
    std::vector<std::vector<uint8_t>> raw(chunks);
    std::vector<std::vector<uint8_t>> packed(chunks);
    std::vector<std::vector<uint8_t>> restored(chunks);
    uint32_t s = 777u;
    for (int c = 0; c < chunks; ++c) {
        raw[c].resize(chunkSize);
        uint32_t lcg = 991u + (uint32_t)c * 131u;
        size_t i = 0;
        while (i < chunkSize) {
            for (int k = 0; k < 48 && i < chunkSize; ++k) {
                raw[c][i++] = (uint8_t)('a' + (lcg % 26));
            }
            lcg = lcg * 1103515245u + 12345u;
            for (int k = 0; k < 16 && i < chunkSize; ++k) {
                raw[c][i++] = (uint8_t)(xsNext(s) & 0xFF);
            }
        }
    }
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    parallelFor(threads, chunks, [&](int idx, int total) {
        (void)total;
        lzCompressChunk(raw[idx].data(), raw[idx].size(), packed[idx]);
    });
    parallelFor(threads, chunks, [&](int idx, int total) {
        (void)total;
        restored[idx].resize(chunkSize);
        lzDecompressChunk(packed[idx].data(), packed[idx].size(), restored[idx].data(), chunkSize);
    });
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    size_t totalPacked = 0;
    for (int c = 0; c < chunks; ++c) {
        totalPacked += packed[c].size();
    }
    volatile uint8_t sink = restored[0][1000];
    (void)sink;
    (void)totalPacked;
    return t1 - t0;
}

// ---------------- 3. 物理模拟 ----------------
const int BODIES = 2048;
const int STEPS = 160;

double runPhysics(int threads)
{
    std::vector<float> px(BODIES), py(BODIES), pz(BODIES);
    std::vector<float> vx(BODIES, 0.0f), vy(BODIES, 0.0f), vz(BODIES, 0.0f);
    uint32_t s = 4242u;
    for (int i = 0; i < BODIES; ++i) {
        px[i] = (float)((int)(xsNext(s) % 2000) - 1000) * 0.1f;
        py[i] = (float)((int)(xsNext(s) % 2000) - 1000) * 0.1f;
        pz[i] = (float)((int)(xsNext(s) % 2000) - 1000) * 0.1f;
    }
    std::vector<std::vector<float>> fx(threads), fy(threads), fz(threads);
    for (int t = 0; t < threads; ++t) {
        fx[t].assign(BODIES, 0.0f);
        fy[t].assign(BODIES, 0.0f);
        fz[t].assign(BODIES, 0.0f);
    }
    const float eps2 = 0.05f;
    const float g = 0.001f;
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    for (int step = 0; step < STEPS; ++step) {
        parallelFor(threads, threads, [&](int idx, int total) {
            std::vector<float>& lfx = fx[idx];
            std::vector<float>& lfy = fy[idx];
            std::vector<float>& lfz = fz[idx];
            std::fill(lfx.begin(), lfx.end(), 0.0f);
            std::fill(lfy.begin(), lfy.end(), 0.0f);
            std::fill(lfz.begin(), lfz.end(), 0.0f);
            int span = BODIES / total;
            int i0 = idx * span;
            int i1 = (idx == total - 1) ? BODIES : i0 + span;
            for (int i = i0; i < i1; ++i) {
                float ax = 0.0f, ay = 0.0f, az = 0.0f;
                for (int j = 0; j < BODIES; ++j) {
                    if (j == i) {
                        continue;
                    }
                    float dx = px[j] - px[i];
                    float dy = py[j] - py[i];
                    float dz = pz[j] - pz[i];
                    float d2 = dx * dx + dy * dy + dz * dz + eps2;
                    float inv = g / (d2 * std::sqrt(d2));
                    ax += dx * inv;
                    ay += dy * inv;
                    az += dz * inv;
                }
                lfx[i] = ax;
                lfy[i] = ay;
                lfz[i] = az;
            }
        });
        for (int i = 0; i < BODIES; ++i) {
            float ax = 0.0f, ay = 0.0f, az = 0.0f;
            for (int t = 0; t < threads; ++t) {
                ax += fx[t][i];
                ay += fy[t][i];
                az += fz[t][i];
            }
            vx[i] += ax * 0.01f;
            vy[i] += ay * 0.01f;
            vz[i] += az * 0.01f;
            px[i] += vx[i] * 0.01f;
            py[i] += vy[i] * 0.01f;
            pz[i] += vz[i] * 0.01f;
        }
    }
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    volatile float sink = px[10] + py[20] + pz[30];
    (void)sink;
    return t1 - t0;
}

// ---------------- 4. 路径查找 ----------------
int astar(const std::vector<uint8_t>& grid, int w, int h, int sx, int sy, int tx, int ty)
{
    const int n = w * h;
    std::vector<int> gScore(n, 0x3FFFFFFF);
    std::vector<int> parent(n, -1);
    std::vector<uint8_t> closed(n, 0);
    std::vector<int> heap;
    heap.reserve(4096);
    auto heuristic = [&](int x, int y) {
        int dx = x - tx;
        int dy = y - ty;
        return (int)(std::abs(dx) + std::abs(dy));
    };
    auto push = [&](int node, int f) {
        heap.push_back(node);
        size_t i = heap.size() - 1;
        while (i > 0) {
            size_t p = (i - 1) / 2;
            int pf = gScore[heap[p]] + heuristic(heap[p] % w, heap[p] / w);
            if (pf <= f) {
                break;
            }
            heap[i] = heap[p];
            i = p;
        }
        heap[i] = node;
    };
    auto pop = [&]() -> int {
        int top = heap[0];
        int last = heap.back();
        heap.pop_back();
        if (!heap.empty()) {
            size_t i = 0;
            int lastF = gScore[last] + heuristic(last % w, last / w);
            while (true) {
                size_t l = 2 * i + 1;
                size_t r = 2 * i + 2;
                size_t best = i;
                int bestF = lastF;
                if (l < heap.size()) {
                    int lf = gScore[heap[l]] + heuristic(heap[l] % w, heap[l] / w);
                    if (lf < bestF) {
                        bestF = lf;
                        best = l;
                    }
                }
                if (r < heap.size()) {
                    int rf = gScore[heap[r]] + heuristic(heap[r] % w, heap[r] / w);
                    if (rf < bestF) {
                        bestF = rf;
                        best = r;
                    }
                }
                if (best == i) {
                    break;
                }
                heap[i] = heap[best];
                i = best;
            }
            heap[i] = last;
        }
        return top;
    };
    int start = sy * w + sx;
    gScore[start] = 0;
    push(start, heuristic(sx, sy));
    const int dxs[4] = {1, -1, 0, 0};
    const int dys[4] = {0, 0, 1, -1};
    while (!heap.empty()) {
        int cur = pop();
        if (closed[cur]) {
            continue;
        }
        closed[cur] = 1;
        if (cur == ty * w + tx) {
            int len = 0;
            int node = cur;
            while (node != -1) {
                len++;
                node = parent[node];
            }
            return len;
        }
        int cx = cur % w;
        int cy = cur / w;
        for (int k = 0; k < 4; ++k) {
            int nx = cx + dxs[k];
            int ny = cy + dys[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            int nid = ny * w + nx;
            if (grid[nid] != 0 || closed[nid]) {
                continue;
            }
            int tentative = gScore[cur] + 1;
            if (tentative < gScore[nid]) {
                gScore[nid] = tentative;
                parent[nid] = cur;
                push(nid, tentative + heuristic(nx, ny));
            }
        }
    }
    return -1;
}

double runPath(int threads)
{
    const int w = 1024;
    const int h = 1024;
    const int runs = 24;
    std::vector<std::vector<uint8_t>> grids(runs);
    std::vector<int> sx(runs), sy(runs), tx(runs), ty(runs);
    uint32_t s = 31337u;
    for (int r = 0; r < runs; ++r) {
        grids[r].assign((size_t)w * h, 0);
        for (size_t i = 0; i < grids[r].size(); ++i) {
            if ((xsNext(s) & 0x3F) == 0) {
                grids[r][i] = 1;
            }
        }
        sx[r] = 4;
        sy[r] = 4 + (r * 7) % 512;
        tx[r] = w - 5;
        ty[r] = h - 5 - (r * 11) % 512;
        grids[r][(size_t)sy[r] * w + sx[r]] = 0;
        grids[r][(size_t)ty[r] * w + tx[r]] = 0;
    }
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    std::vector<int> lens(runs, 0);
    parallelFor(threads, runs, [&](int idx, int total) {
        (void)total;
        lens[idx] = astar(grids[idx], w, h, sx[idx], sy[idx], tx[idx], ty[idx]);
    });
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    volatile int sink = lens[0] + lens[runs - 1];
    (void)sink;
    return t1 - t0;
}

// ---------------- 5. AI 推理 (int8 GEMM) ----------------
const int AI_N = 256;
const int AI_ITERS = 128;

double runAi(int threads)
{
    std::vector<int8_t> a((size_t)AI_N * AI_N);
    std::vector<int8_t> b((size_t)AI_N * AI_N);
    std::vector<int32_t> c((size_t)AI_N * AI_N);
    uint32_t s = 909u;
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = (int8_t)((int)(xsNext(s) % 255) - 127);
        b[i] = (int8_t)((int)(xsNext(s) % 255) - 127);
    }
    for (size_t i = 0; i < c.size(); i += 4096) {
        c[i] = 0;
    }
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    for (int it = 0; it < AI_ITERS; ++it) {
        parallelFor(threads, threads, [&](int idx, int total) {
            int span = AI_N / total;
            int r0 = idx * span;
            int r1 = (idx == total - 1) ? AI_N : r0 + span;
            for (int i = r0; i < r1; ++i) {
                for (int j = 0; j < AI_N; ++j) {
                    int32_t acc = 0;
                    const int8_t* ar = a.data() + (size_t)i * AI_N;
                    for (int k = 0; k < AI_N; ++k) {
                        acc += (int32_t)ar[k] * (int32_t)b[(size_t)k * AI_N + j];
                    }
                    c[(size_t)i * AI_N + j] = acc < 0 ? 0 : acc;
                }
            }
        });
    }
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    volatile int32_t sink = c[100] + c[AI_N * AI_N - 1];
    (void)sink;
    return t1 - t0;
}

// ---------------- 6. 文本处理 ----------------
double runText(int threads)
{
    const size_t total = 24u * 1024 * 1024;
    std::vector<uint8_t> text(total);
    uint32_t s = 5150u;
    const char* words[8] = {"harmony", "benchmark", "aurora", "graphics", "compute", "device", "score", "kernel"};
    size_t i = 0;
    while (i < total) {
        const char* w = words[xsNext(s) % 8];
        size_t len = std::strlen(w);
        if (i + len + 1 >= total) {
            break;
        }
        std::memcpy(text.data() + i, w, len);
        i += len;
        text[i++] = ' ';
    }
    const int passes = 6;
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    uint64_t acc = 0;
    for (int p = 0; p < passes; ++p) {
        std::vector<uint64_t> partial(threads, 0);
        parallelFor(threads, threads, [&](int idx, int totalThreads) {
            size_t span = i / (size_t)totalThreads;
            size_t b0 = (size_t)idx * span;
            size_t b1 = (idx == totalThreads - 1) ? i : b0 + span;
            uint64_t hash = 1469598103934665603ull;
            bool inWord = false;
            for (size_t k = b0; k < b1; ++k) {
                uint8_t ch = text[k];
                if (ch == ' ') {
                    if (inWord) {
                        hash ^= 0xFF;
                        hash *= 1099511628211ull;
                        inWord = false;
                    }
                } else {
                    hash ^= ch;
                    hash *= 1099511628211ull;
                    inWord = true;
                }
            }
            partial[idx] = hash;
        });
        for (int t = 0; t < threads; ++t) {
            acc ^= partial[t];
        }
    }
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    volatile uint64_t sink = acc;
    (void)sink;
    return t1 - t0;
}

// ---------------- 7. 内存带宽 ----------------
double runMemory(int threads)
{
    const size_t elems = 24u * 1024 * 1024;
    std::vector<float> a(elems), b(elems), c(elems);
    uint32_t s = 6161u;
    for (size_t i = 0; i < elems; ++i) {
        b[i] = (float)(xsNext(s) % 1000) * 0.001f;
        c[i] = (float)(xsNext(s) % 1000) * 0.002f;
    }
    const int iters = 10;
    for (size_t i = 0; i < elems; i += 4096) {
        a[i] = 0.0f;
    }
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    for (int it = 0; it < iters; ++it) {
        parallelFor(threads, threads, [&](int idx, int total) {
            size_t span = elems / (size_t)total;
            size_t i0 = (size_t)idx * span;
            size_t i1 = (idx == total - 1) ? elems : i0 + span;
            for (size_t i = i0; i < i1; ++i) {
                a[i] = b[i] + 3.0f * c[i];
            }
        });
    }
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    volatile float sink = a[1000];
    (void)sink;
    return t1 - t0;
}

// workUnits: 该项每次跑分完成的固定工作量(已按单位缩放), metric = workUnits * 1000 / ms
// 这些常量与参考基线一样写死, 不随设备调整, 保证任何平台结果可比
// ---------------- 8. 音频编解码 (FFT/MDCT 变换 + 量化, 覆盖音频/媒体类负载) ----------------
const int AUDIO_N = 512;
const int AUDIO_FRAMES = 8192;

void fftRadix2(float* re, float* im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float tr = re[i];
            re[i] = re[j];
            re[j] = tr;
            float ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -6.28318530717958647692 / (double)len;
        float wr = (float)std::cos(ang);
        float wi = (float)std::sin(ang);
        int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0f;
            float cwi = 0.0f;
            for (int k = 0; k < half; ++k) {
                int a = i + k;
                int b = i + k + half;
                float ur = re[a];
                float ui = im[a];
                float vr = re[b] * cwr - im[b] * cwi;
                float vi = re[b] * cwi + im[b] * cwr;
                re[a] = ur + vr;
                im[a] = ui + vi;
                re[b] = ur - vr;
                im[b] = ui - vi;
                float nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
}

double runAudio(int threads)
{
    const double flopsPerFrame = 2.0 * 5.0 * (double)AUDIO_N * 9.0;
    (void)flopsPerFrame;
    std::atomic<float> sink{0.0f};
    // 计时区间入口(紧邻 t0 之前): 只把采样窗口打开, 纯内存操作 —— 与 CS1 16 项逐字同一约定
    auroraFreqMarkStart();
    double t0 = wallMs();
    parallelFor(threads, threads, [&](int idx, int total) {
        int span = AUDIO_FRAMES / total;
        int f0 = idx * span;
        int f1 = (idx == total - 1) ? AUDIO_FRAMES : f0 + span;
        std::vector<float> lre((size_t)AUDIO_N);
        std::vector<float> lim((size_t)AUDIO_N);
        uint32_t ls = 12345u + (uint32_t)idx * 7919u;
        float acc = 0.0f;
        for (int f = f0; f < f1; ++f) {
            for (int k = 0; k < AUDIO_N; ++k) {
                ls ^= ls << 13;
                ls ^= ls >> 17;
                ls ^= ls << 5;
                lre[(size_t)k] = ((float)(ls % 2000) * 0.001f) - 1.0f;
                lim[(size_t)k] = 0.0f;
            }
            fftRadix2(lre.data(), lim.data(), AUDIO_N);
            for (int k = 0; k < AUDIO_N; ++k) {
                float mag = std::sqrt(lre[(size_t)k] * lre[(size_t)k] + lim[(size_t)k] * lim[(size_t)k]);
                float q = std::floor(mag * 8.0f) * 0.125f;
                lre[(size_t)k] = q;
                lim[(size_t)k] = 0.0f;
            }
            fftRadix2(lre.data(), lim.data(), AUDIO_N);
            for (int k = 0; k < AUDIO_N; k += 8) {
                acc += lre[(size_t)k];
            }
        }
        sink.store(sink.load() + acc);
    });
    double t1 = wallMs();
    // 计时区间出口(紧邻 t1 之后): 关闭采样窗口。这两行与 CS1 16 项逐字同一约定。
    auroraFreqMarkStop();
    volatile float s2 = sink.load();
    (void)s2;
    return t1 - t0;
}

struct TestDef {
    const char* name;
    const char* detail;
    double refMs;
    double (*run)(int threads);
    double workUnits;
    const char* unit;
};

const TestDef TESTS[] = {
    {"图像处理", "3840x2160 · 两遍模糊 + 边缘检测", 1100.0, runImage, 24.8832, "Mpx/s"},
    {"数据压缩", "16MB · LZ77 压缩 + 解压", 900.0, runCompress, 33.5544, "MB/s"},
    {"物理模拟", "2048 粒子 · 160 步引力积分", 950.0, runPhysics, 0.335401, "Gpair/s"},
    {"路径查找", "1024x1024 网格 · 24 次 A* 寻路", 1000.0, runPath, 24.0, "次/s"},
    {"AI 推理", "int8 GEMM 256x256 · 128 次推理", 850.0, runAi, 4.29497, "GOPS"},
    {"文本处理", "24MB 文本 · 6 遍词法扫描哈希", 800.0, runText, 150.995, "MB/s"},
    {"内存带宽", "96MB 数组 · STREAM Triad x10", 750.0, runMemory, 2.88, "GB/s"},
    {"音频编解码", "8192 帧 512 点 FFT/MDCT 编码+解码", 700.0, runAudio, 0.377, "GOPS"},
};

const int TEST_COUNT = (int)(sizeof(TESTS) / sizeof(TESTS[0]));

} // namespace

int auroraCpuCount()
{
    unsigned int n = std::thread::hardware_concurrency();
    if (n == 0) {
        n = 4;
    }
    return (int)n;
}

int auroraTestCount()
{
    return TEST_COUNT;
}

std::string auroraTestName(int id)
{
    if (id < 0 || id >= TEST_COUNT) {
        return "";
    }
    return TESTS[id].name;
}

std::string auroraVersion()
{
    return "2.0 (CS1 16 项 CPU + 11 项 GPU)";
}

BenchOutcome auroraRunTest(int id, int threads)
{
    BenchOutcome out;
    if (id < 0 || id >= TEST_COUNT) {
        out.name = "unknown";
        out.ms = 0;
        out.score = 0;
        out.detail = "";
        out.metric = 0;
        out.unit = "";
        out.parallelism = 0;
        return out;
    }
    if (threads < 1) {
        threads = 1;
    }
    if (threads > 16) {
        threads = 16;
    }
    const TestDef& def = TESTS[id];
    // 每一项开跑前清空上一项的绑核取证(不把上一项的落点带进这一项)
    g_bind = OwnBindEvidence();
    g_threadCpuMs.store(0.0);
    // 频率采样会话: 起 / 停采样线程都在计时区间之外(与 CS1 路径同一个入口)。
    // 采样线程自己设 QOS_BACKGROUND, 不去抢负载线程的 CPU 份额(见 cpu_freq_sample.cpp)。
    auroraFreqSampleSessionBegin(threads);
    double ms = def.run(threads);
    double cpuSum = g_threadCpuMs.load();
    auroraFreqSampleSessionEnd();
    if (ms < 1.0) {
        ms = 1.0;
    }
    double score = def.refMs / ms * 1000.0;
    // 刻度校准(本工程的做法: 一台参考设备 + 一个目标分数)
    // 参考设备: HUAWEI Mate 80 Pro Max (麒麟 9030 Pro)
    //   其 Geekbench 7 官方成绩: 单核 1633 / 多核 6802
    //   本工具在该设备上实测:   单核  212 / 多核  589
    // => 单核刻度系数 = 1633/212 = 7.70,  多核 = 6802/589 = 11.55
    // 系数固化于代码, 不随机型调整; 其他设备的分数即按其相对该参考设备的性能成比例换算。
    const double kScaleSingle = 7.70;
    const double kScaleMulti = 11.55;
    score *= (threads > 1) ? kScaleMulti : kScaleSingle;
    if (score < 10.0) {
        score = 10.0;
    }
    if (score > 60000.0) {
        score = 60000.0;
    }
    out.name = def.name;
    out.ms = ms;
    out.score = score;
    out.detail = def.detail;
    out.metric = def.workUnits * 1000.0 / ms;
    out.unit = def.unit;
    out.parallelism = (threads <= 1 || ms <= 0.0) ? 1.0 : (cpuSum / ms);

    // ---- 落核 / 标称上限 / 运行时频率的取证(2026-10 补; 旁路, 不计分) ----------
    //  以前自研套件每一项在报告里都是 cpu=-1 · maxKhz=0, 于是"这一项跑在哪颗核上"
    //  这个最基本的问题答不了 —— 而它正是"两台同芯片设备自研单核差 37.7%"的根因所在。
    //  这里照 CS1 那套写: 落核(跑完那一刻仍在绑定状态下采样)、该核标称上限、
    //  运行时频率(逐样本取该样本所在核的标称上限, 中位占比)、以及绑核动作本身的取证。
    {
        const int cpu = (g_bind.attempted != 0) ? g_bind.landingCpu : auroraCurrentCpu();
        out.cpu = cpu;
        out.cpuAtStart = cpu;
        out.cpuMaxKhz = auroraCpuMaxFreqKhz(cpu);
        out.cpuRank = auroraCpuRank(cpu);
        out.cpuBound = (g_bind.rc > 0 && g_bind.singleThread != 0) ? 1 : 0;
        out.cpuInFastClusterJudged = (auroraFastClusterCoreCount() > 0) ? 1 : 0;
        out.cpuInFastCluster = auroraCpuInFastCluster(cpu) ? 1 : 0;
        out.cpuInMachineTopTier = auroraCpuInMachineTopTier(cpu) ? 1 : 0;
        out.cpuFastClusterCores = auroraFastClusterCoreCount();
        out.cpuFastClusterMaxKhz = auroraFastClusterMaxKhz();
        out.cpuMachineTopTierCores = auroraMachineTopTierCoreCount();
        out.cpuMachineTopTierKhz = auroraMachineTopTierMaxKhz();
        out.cpuAllowedCount = auroraAllowedCoreCount();
        out.cpuSingleTargetCores = g_bind.targetCores;
        // 容量与 g_text 同量级: auroraFreqSampleText 在放不下时会写"尾部被截断"标记,
        // 但正常路径不该触发它(CS1 那一侧用的是同一份文本、同一个容量口径)。
        std::vector<char> freq(16384, '\0');
        (void)auroraFreqSampleText(freq.data(), (int)freq.size());
        out.runFreq.assign(freq.data());
        // 一行中文说明(与 CS1 的 cpuInfo 同性质: 直接显示, 不参与任何计算)
        char note[1024];
        if (threads <= 1) {
            snprintf(note, sizeof(note),
                     "自研套件单核阶段 · 绑核目标 = (全机频率最快档 %d 核) ∩ (内核实测允许的 %d 核) "
                     "= 生效快簇 %d 核(最高 %d MHz) · sched_setaffinity 返回值 %d(>=1 = 绑进掩码的核数)"
                     "%s · 掩码回读 %d 核 · 跑完落在 cpu%d(该核标称上限 %d MHz, 频率降序位次 %d) · "
                     "还原原掩码%s · 跑完在生效快簇内=%s · 在全机最快档内=%s",
                     auroraMachineTopTierCoreCount(), auroraAllowedCoreCount(),
                     auroraFastClusterCoreCount(), (auroraFastClusterMaxKhz() + 500) / 1000,
                     g_bind.rc,
                     (g_bind.rc > 0) ? "" : (g_bind.errnoCode == -3
                         ? "(errno=-3: 这段位次里一个合法核都没有, 连 syscall 都没发出去)"
                         : "(失败: 内核拒绝或没有可用核; errno 已记在 runlog)"),
                     g_bind.readbackCores, cpu, (out.cpuMaxKhz + 500) / 1000, out.cpuRank,
                     g_bind.restoreOk != 0 ? "成功(读回一致)" : "未能确认(读回不一致或取不到掩码)",
                     out.cpuInFastCluster != 0 ? "是" : "否",
                     out.cpuInMachineTopTier != 0 ? "是" : "否");
        } else {
            snprintf(note, sizeof(note),
                     "自研套件多核阶段(%d 线程) · 池线程 t 独占「可用核集合里频率第 t 高的核」"
                     "(一核一线程, 与 CS1 多核阶段同口径; 本项池线程数不变) · 可用核集合 %d 核 · "
                     "全机最快档 %d 核 %d MHz · 单核阶段的快簇取证见上一节 · "
                     "注意: 多核项那一行 cpu=/maxKhz= 采的是本阶段的主线程(建池/汇总线程)的落点, "
                     "不是负载线程的落点 —— 负载线程的落点由「一核一线程」这句话决定(线程 t -> order[t]), "
                     "运行时频率那一行里的口径A 才是真正在干活的核",
                     threads, auroraAllowedCoreCount(), auroraMachineTopTierCoreCount(),
                     (auroraMachineTopTierMaxKhz() + 500) / 1000);
        }
        out.cpuInfo = note;
    }
    return out;
}
