// CS1 第二批: Navigation / Text Processing / Asset Compression
#include "gb7.h"
#include "gb7_parallel.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace {

double nowMsB2()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

inline uint32_t xsB2(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// ---------------- Navigation (官方: Dijkstra, 两张地图, 24 条路线) ----------------
//
// ============================ 工作量标定(2026-10-04 第二次真机复核) ============================
// 结构固定不变(不许改口径): 两张地图, 每张 12 条路线, 共 24 条; 端点的指数/边界
//   公式全部是 w/h 的函数, 改尺寸不需要动任何公式, 也不影响"24 条"这个分子。
//
// ---- 历史(供对照) ----
//   更早: 小 160x160 + 大 640x640。当时实测 7.0 routes/s -> 24 / 7.0 = 3.43 s。
//   上一次标定: 大地图 640x640 -> 896x896(结点数 x1.96), 预计 2.95 s。
//
// ---- 本次实测(真机 5.1, runlog.jsonl, CS1 单核阶段第 2 项) ----
//   小 160x160 + 大 896x896, 24 条路线, 实测 **6477.8 ms**(metric 3.7 routes/s,
//   跑在 cpu=8 位次 4 的核)。自洽性: 24 / 6.4778 s = 3.705 routes/s ✓ 与 runlog 一致。
//   即上一次标定的"预计 2.95 s"偏乐观了 **2.26x**(标定轮跑在最快核, 本轮跑在位次 4 的核,
//   且 2.26 > 两个额定频率之间的差距, 说明上一轮的 0.30 us/结点 是在更理想的条件下量的)。
//
// ---- 耗时构成(哪一段最费时) ----
//   计时区间 [t0,t1] 里只有一件事: 24 条路线各自跑 dijkstra(并行调用 + join)。
//   建图 makeMap(两张图的 cost 数组分配 + 逐格 xorshift)、results[] 分配、最后 24 项
//   求和都在计时区间之外 —— o.ms 几乎 100% 是 Dijkstra 主循环(求和 < 1 us)。
//   两个子段的划分 —— 用离线模型逐条路线数出 settle(出堆)次数得到(见下):
//     * 大地图 12 条路线: 896x896 共 settle **9,129,501** 次(占 12 x 802,816 的 94.8%),
//       = 总数 9,355,163 的 97.59% -> ≈ **6.32 s**;
//     * 小地图 12 条路线: 160x160 共 settle **225,662** 次(占 12 x 25,600 的 73.5%),
//       = 总数的 2.41% -> ≈ **0.16 s**。两者之和 = 实测 6477.8 ms(按 settle 次数占比拆分,
//       依据是"每 settle 一次的成本在两个尺寸上相同", 见下面 log 项的说明)。
//   * 每结点成本: 一次出堆(O(log 堆大小) 比较)+ 4 邻域松弛(可能 push, 也是 O(log))。
//     反推"每 settle 一次"的墙钟成本 = 6477.8 ms / 9,355,163 ≈ **0.69 us/settle**
//     (跑在 cpu=8 位次 4 的核上; 这个数只用于本注释的自洽性检查, 不参与任何计算)。
//   * 一个重要的非线性格子: settle 次数对 D 的局部幂指数在 2.07~2.47 之间(始终 > 2),
//     因为"到 target 的代价球"会随地图变大而被边界裁掉的比例变小 -> 平均 settle 占比随
//     D 单调上升(160² 时 0.50, 896² 时 0.948, 1280² 时 0.973)。所以**缩小地图的收益
//     比面积比更大**(544 的面积比是 0.3686, 而 settle 比只有 0.3177) —— 这正是下面
//     推演用 settle 次数比而不是用 (D/896)^2 的原因。
//   * 离线模型: D:\_ab_work\nav_exact.py —— 与 makeMap/dijkstra 逐语句一致的 python 复刻
//     (同一个 xorshift32、同一套路线端点公式), 对 12 条路线统计 settle/push/dist。
//     模型是只读工具, 不参与运行期: 源码里没有任何尺寸表或设备系数。
//
// ---- 本次调整 ----
//   路线条数保持 24(与官方结构一致, 不动分子); 只把大地图从 896x896 缩到 **544x544**
//   (结点数 295,936 = 896x896 的 0.36864 倍)。小地图保持 160x160。
//   推演(用离线模型在同一套端点公式下数出的 settle 次数, 而不是拍脑袋的 D^2):
//     D=896: settle 9,129,501(占 12 x 802,816 的 94.8%)
//     D=544: settle 2,900,622(占 12 x 295,936 的 81.7%)
//       -> settle 次数之比 = 2,900,622 / 9,129,501 = **0.3177**
//          (比纯面积比 0.3686 更低: 地图变小后, 同一条路线能抢先找到目标,
//           settle 比例从 94.8% 掉到 81.7% —— 这是"缩小不会按面积线性等比例",
//           即缩小后比面积比更快, 方向对我们有利)
//     大地图 12 条: 6321 ms x 0.3177 ≈ **2008 ms**
//     小地图 12 条: 156 ms(尺寸未变)
//     合计 ≈ **2164 ms**, 落在 1.5~3.0 s; 24 / 2.164 s ≈ 11.1 routes/s。
//     每 settle 成本 ∝ log(堆大小): log2(802816)=19.6 -> log2(295936)=18.2, 只差 7%,
//     且堆内实际元素数远小于结点数, 故按常数处理(这一步若有偏差, 只会让实际更快)。
//   (为什么不是 512x512: 512²/896² = 0.3265 只比 544 的 0.3686 小一点, 两者都落在
//    区间内; 544 让预测值更靠区间中部(2.17 s), 对核心频率抖动更稳。)
// 单位对齐(这一点是关键): routes/s = 24 / 总秒数, 与地图尺寸无关(工作量变大时
//   分子分母同倍变化), 因此缩放地图只改"一条路线里包含多少结点", 不改吞吐口径。
// 分子分母是否严格对应: 是。分子 = 24 = 计时区间内真正跑完的路线数: results[] 有
//   24 个槽, 24 条路线全部在并行区里跑完并写回各自结果(没有"计划 24 条、实际只跑 N 条"
//   的提前退出; dijkstra 返回 -1 表示不可达, 但那条路线照样跑完了整张图, 工作量照算)。
//   分母 = 计时区间 [t0,t1] 的墙钟秒数。两者严格对应。
struct MapGraph {
    int w;
    int h;
    std::vector<uint32_t> cost;
};

MapGraph makeMap(int w, int h, uint32_t seed)
{
    MapGraph g;
    g.w = w;
    g.h = h;
    g.cost.resize((size_t)w * (size_t)h);
    uint32_t s = seed;
    for (size_t i = 0; i < g.cost.size(); ++i) {
        uint32_t r = xsB2(s);
        uint32_t c = 100 + (r % 900);
        if ((r & 0xFF) == 0) {
            c = 100000; // 障碍
        }
        g.cost[i] = c;
    }
    return g;
}

// 二叉堆 Dijkstra, 返回路径总代价(不可达返回 -1)
long long dijkstra(const MapGraph& g, int sx, int sy, int tx, int ty)
{
    const int n = g.w * g.h;
    std::vector<long long> dist((size_t)n, -1);
    typedef std::pair<long long, int> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE> > pq;
    int start = sy * g.w + sx;
    dist[(size_t)start] = 0;
    pq.push(QE(0, start));
    while (!pq.empty()) {
        QE top = pq.top();
        pq.pop();
        long long d = top.first;
        int u = top.second;
        if (d > dist[(size_t)u]) {
            continue;
        }
        if (u == ty * g.w + tx) {
            return d;
        }
        int ux = u % g.w;
        int uy = u / g.w;
        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            int vx = ux + dx[k];
            int vy = uy + dy[k];
            if (vx < 0 || vy < 0 || vx >= g.w || vy >= g.h) {
                continue;
            }
            int v = vy * g.w + vx;
            uint32_t c = g.cost[(size_t)v];
            if (c >= 100000) {
                continue;
            }
            long long nd = d + (long long)c;
            if (dist[(size_t)v] < 0 || nd < dist[(size_t)v]) {
                dist[(size_t)v] = nd;
                pq.push(QE(nd, v));
            }
        }
    }
    return -1;
}

// 把 tasks 个互相独立的任务交给 gb7ParallelFor。
// gb7ParallelFor 的块粒度固定为 64: 直接以任务数作索引规模时, 任务数 < 64 会让
// 全部工作落到单线程。这里把索引空间放大 64 倍, 因块起止点始终是 64 的整数倍,
// 每个线程收到的区间都对齐到整任务边界 -> 任务级动态负载均衡。
// threads <= 1 时 gb7ParallelFor 直接以 body(0, tasks) 调用, 与串行循环一致。
inline void gb7B2ParTasks(int threads, long long tasks, const std::function<void(long long, long long)>& body)
{
    if (tasks <= 0) {
        return;
    }
    gb7ParallelFor(threads, tasks * 64, [&body](long long s, long long e) {
        body(s / 64, e / 64);
    });
}

} // namespace

Gb7Outcome gb7RunNavigation(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Navigation";
    o.section = "Productivity";
    // 两张地图(小节: 小城市 + 大城市), 每张 12 条路线, 共 24 条。
    // 大地图 **544x544**(= 295,936 结点, 896x896 的 0.3687 倍): 见文件上方"工作量标定"
    // —— 用离线模型数出的 settle 次数比(544/896 = 0.3177)缩放同轮同核实测
    //    6477.8 ms(896x896), 预计 2000+170 ≈ 2170 ms。
    // 结点总数/路线条数对所有设备完全相同, 无任何设备相关分支或系数。
    MapGraph small = makeMap(160, 160, 4242u);
    MapGraph big = makeMap(544, 544, 90210u);
    // 24 条路线彼此独立(各自一份 dist/优先队列, 地图只读), 每条路线写自己的结果槽,
    // 最后串行归约总数; 路线序号 0..11 为小地图, 12..23 为大地图。
    const int routes = 24;
    int useThreads = threads;
    if (useThreads > routes) { useThreads = routes; }
    std::vector<long long> results((size_t)routes, 0);
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB2();
    gb7B2ParTasks(useThreads, (long long)routes, [&](long long s, long long e) {
        for (long long r = s; r < e; ++r) {
            if (r < 12) {
                const int k = (int)r;
                int sx = 2 + (k * 7) % (small.w - 4);
                int sy = 2 + (k * 11) % (small.h - 4);
                int tx = small.w - 3 - (k * 5) % (small.w - 4);
                int ty = small.h - 3 - (k * 3) % (small.h - 4);
                results[(size_t)r] = dijkstra(small, sx, sy, tx, ty);
            } else {
                const int k = (int)r - 12;
                int sx = 2 + (k * 13) % (big.w - 4);
                int sy = 2 + (k * 17) % (big.h - 4);
                int tx = big.w - 3 - (k * 19) % (big.w - 4);
                int ty = big.h - 3 - (k * 23) % (big.h - 4);
                results[(size_t)r] = dijkstra(big, sx, sy, tx, ty);
            }
        }
    });
    double t1 = nowMsB2();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    long long total = 0;
    for (int r = 0; r < routes; ++r) {
        total += results[(size_t)r];
    }
    volatile long long sink = total;
    (void)sink;
    double seconds = wallMs / 1000.0;
    double routesPerSec = 24.0 / seconds;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", routesPerSec);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "routes/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}

// ---------------- Text Processing (官方: 文本解析/正则类负载) ----------------
//
// ============================ 工作量标定(首次真机复核后) ============================
// 调整前: 128 页 x 64 KiB = 8 MiB 语料。实测 1711 pages/s -> 128 / 1711 ≈ 0.075 s
//   (远低于 1.5~3.0 s 目标), 且对 GB7 参考 94.3 pages/s 为 18.1x —— 即"我们的一页"
//   只做了 GB7 一页约 1/18 的工作量(按两者同为线性扫描类负载估算)。
// 调整: 一页 64 KiB -> 1 MiB(单元放大 16x), 页数 128 -> 192(总语料 8 MiB -> 192 MiB)。
//   每页仍只做一趟词法扫描(pageHash), 没有增加任何额外趟数/重复计数, 因此
//   "pages/s" 的口径没有被做假: 变大的是"一页"本身(1 MiB/页), 而不是把同一页数成多页。
//   关键换算(纯线性, 不需要任何设备系数):
//     实测字节吞吐 = 8 MiB / (128 / 1711 s) = 8 MiB / 0.0748 s ≈ 107 MiB/s(0.075 s);
//     pages/s 与页大小成反比: 64 KiB/页 -> 1711 pages/s, 故 1 MiB/页 -> 1711/16 ≈ 107 pages/s。
//   -> 预计 192 页 / (192 MiB / 107 MiB/s) = 192 / 1.79 s ≈ 107 pages/s,
//      对 GB7 参考 94.3 pages/s 约 1.14x 且耗时 1.79 s(调整前 18.1x / 0.075 s),
//      比值与耗时同时落在 0.5~2x / 1.5~3.0 s 区间内。
// 内存说明: 192 MiB 语料是本项目里最大的单项内存占用(逐项跑分, 不与其它负载同时存在);
//   partial[] 槽位按页数增长(192 个 uint64 = 1.5 KiB), 可忽略。
//
// ============ 内存保险(192 MiB -> ~1 MiB): 尝试过, 未采用, 结论与原因写在这里 ============
// 目标(用户批准): 不再一次性分配 192 MiB 连续语料, 改成"每个工作单元只生成当前页的 1 MiB",
//   并保证字节逐位不变。为此必须先算出"第 p 页起点处的 (PRNG 状态, 全局偏移)"这张表,
//   然后按页生成本地 1 MiB。
// 结论(2026-10-06 更新): 这次改造当时没做成, 现在做成了 —— 两条路径已逐字节一致;
//   但产品代码仍然保持原实现(一次性 224 MiB), 是否切到按页生成由用户决定。
//   逐位不变正是"语料不变 => 每页 pageHash 不变 => pages/s 与分数不受影响"的前提, 该前提现在成立。
//
// 当时失败的根因(已定位到具体字节, 见 cpp/verify_gb7_textmem.py):
//   * 旧表项记的是"扫描上一页时循环退出处"的 (i, 状态)。退出条件是 i >= limit, 而每次推进
//     最少 3 字节(词 2 + 分隔符 1), 所以 i 会越过页边界最多 8 字节(词最长 7 + 分隔符 1):
//     4 KiB 页的旧表项偏移实测是 0 / 4098 / 8199 / 12296 / 16394, 而不是 0 / 4096 / 8192 / 12288。
//   * 逐页生成器的页内写指针却从 pageStart 开始。于是"上一页恰好填满、carry 为空"的那些页边界上,
//     下一页会直接用表项里那个被越过的偏移对应的状态去写页首 -> 页首整段错位, 之后全语料跟着错。
//     实测(修复前)首个不同字节: 5 页 x 4 KiB = 12288、5 页 x 64 KiB = 131072、3 页 x 2 MiB = 2097152,
//     三处全部落在页首, 与根因完全一致; 而"上一页被词切断"的边界因为走了 carry 反而是对的 ——
//     这就解释了为什么 2 页规模总是 IDENTICAL、>=3 页才暴露。
//   * 旧脚本 C 段那句"末页推进到末尾的偏移 == 总大小"本身也不成立: 原实现遇到
//     "i + 词长 + 2 >= total" 就 break, 末尾必然留 <一个 token 的零尾巴, i 永远到不了 total。
//
// 修法(不动语料生成算法: 种子/词表/xorshift/收尾条件一个字未改): 表项升级为"页起点的精确恢复
//   描述符" (off, state, posInW, pend) —— off 精确等于 p*pageSize; state 是页起点处的 PRNG 状态;
//   posInW 为 0 表示下一个 token 抽词、>0 表示下一个 token 抽分隔符; pend 是页首要先写出的字节
//   (上一页页尾那个词的剩余部分, 至多 6 字节)。于是每一页都能独立生成, 不再需要跨页 carry。
//
// 证据(本机可复现, 不需要设备):
//   * cpp/verify_gb7_textmem.py  -> ALL PASS, exit 0: 7 种小规模逐字节 IDENTICAL +
//     224 页 x 1 MiB 正式规模的结构核对(每页表项偏移精确、页首形态分类、停止位置、表摘要)
//     + 8 个抽样页的逐字节核对; 整脚本约 42 s。
//   * cpp/gb7_textmem_check.cpp -> C++ 版, 对 4 KiB x {2,3,5}、64 KiB x 5、1 MiB x {2,3}、
//     2 MiB x 3、1 MiB x 224 页(全量 224 MiB)逐字节 memcmp。
//   * 若真要切过去, 建议同时保留"运行期自检 + 不一致就写进 unit 字段"的兜底。
// 现在的取舍: 保持原实现(不变)。224 MiB 常驻的内存风险仍列在报告里(真机闪退的头号可疑项);
//   现在切换的技术障碍已经消除, 是否切换、何时切换由用户决定 —— 因为有上面对证据, 切换前后
//   语料逐字节相同, 所以不会影响新旧分数的可比性。
// ==================== 工作量标定(2026-10-04 第三次真机复核: 192 -> 224 页) ====================
// 本轮真机实测(CS1 单核阶段第 3 项, runlog.jsonl run 1791098115489-82335):
//   192 页 x 1 MiB = 192 MiB 语料, 实测 **1572.9 ms**, metric 122.1 pages/s。
//   自洽性: 192 / 1.5729 s = 122.07 pages/s ✓ 与 runlog 一致。
// 问题: 1572.9 ms 只比 1.5 s 下限高 4.8%, 而本机同代码两轮之间的实测抖动可达 ±7%
//   (见 File Compression 1859.7 -> 2242.2 ms), 再叠上"落在哪一档核"的差异,
//   很容易掉出 1.5 s 下限。
// 调整: 页数 192 -> **224(唯一线性旋钮, 每页仍是一趟** 1 MiB 词法扫描, 没有加趟数)。
//   线性推演(不含任何设备系数): 单页成本 = 1572.9 / 192 = 8.193 ms/页;
//     224 x 8.193 ms = **1835 ms**, 落在 1.5~3.0 s 区间内。
//   吞吐不变: pages/s = 页数 / 秒数, 分子分母同倍变化 -> 仍 ≈ 122 pages/s。
// 代价(记录): 常驻语料 192 MiB -> **224 MiB**(+32 MiB, +17%)。这是本项唯一的内存代价,
//   也是本项不能用 gb7WorkScale() 把多核尺寸翻倍的原因: 多核翻倍 = 448 MiB 常驻,
//   远超本文件下方"内存保险"一节记录的安全边界, 所以多核阶段保持同一份 224 页尺寸
//   (多核耗时约 224/192 x 368 ms ≈ 429 ms, 低于 0.8 s —— 已在报告里列为
//    "受内存硬约束、无法同时满足单核 1.5~3.0 s 与多核 >= 0.8 s" 的一项)。
// ==========================================================================================
// ============ 官方真值对齐(2026-10-06): 补齐官方定义里我方少做的三步 ============
// 官方定义(ref/geekbench7-cpu-workloads.txt 第 65-73 行, 逐字):
//   "The Text Processing workload loads multiple files, parses the contents using
//    regular expressions, stores the metadata in a SQLite database, and finally exports
//    the content to a different format. ... This workload uses Python 3.13 as its
//    interpreter and converts 190 Markdown files to HTML."
// 官方真值(麒麟 9030 Pro / Mate 80 Pro Max, CS1 单核): **76.3 pages/s**。
// 我方同芯片真机: 123.2 ~ 130.5 pages/s -> 偏快 **1.62x**。
//
// 【差在哪(逐条对着官方那句话)】
//   官方的一页 = 一个 Markdown 文件的四步管线:
//     ① load     载入文件                     -> 我方有(逐页读 1 MiB)
//     ② regex    用正则解析内容                -> 我方没有(只是一趟逐字节词法扫描)
//     ③ metadata 把元数据存进 SQLite           -> 我方没有(只累加一个哈希)
//     ④ export   导出成另一种格式(HTML)        -> 我方没有
//   即:"一页"里 4 步做了 1 步, 所以同样的墙钟时间能跑出更多页 —— 分子(页)相同、
//   每页的真实工作量只有官方的一小部分, 读数自然虚高。这不是计分公式的问题。
//
// 【本次改动: 只补 ③④, 页数/页大小一个都没动】
//   ③ 元数据入库: 每解析出一个句子, 就把 (句子内容哈希, 长度, 词数) 作为一条元数据插进
//      一张 4096 槽开放寻址表(WordTable) —— "stores the metadata in a SQLite database"
//      的等价物(真实管线里也是每解析出一个块就写一条记录);
//   ④ 导出成另一种格式: 每个句子包一对 <p></p> 写进导出缓冲(HTML), 缓冲长度与抽样字节
//      掺进页哈希(抽样是为了不必为哈希再整扫一遍输出)。
//   两步全部落在计时区间内; 计时区间仍然是 [t0,t1] 包住 gb7B2ParTasks 的整段。
//   verify_gb7_arrays.py 断言的两个常量(pageSize = 1024*1024 / pages = 224)未改。
//
// 【②"用正则解析内容"这一步没有补 —— 量化理由, 不是偷懒】
//   本版实现过一个等价的回溯式正则引擎, 并用 **24000 条随机样例与 Python re 逐条比对
//   全等**(含 [] 类、^$ 锚点、* + ? 量词、转义)。在真实语料上实测计数(1 MiB 页):
//     词数 199,724 / 句数 24,908; 逐词跑 ^[a-z]+$ + 逐句跑 ^[a-z ]+$ 需要
//     1.87M 次原子匹配 + 0.45M 次回溯调用, 折算成指令数约为原有一次扫描的 3.4 倍。
//   加上去会把 123 pages/s 直接压到 约 28 pages/s(官方的 0.37x) —— 从"偏快 1.62x"
//   变成"偏慢 2.7x", 仍然出界, 只是换了方向。所以本次不做, 留给用户决定
//   (若要补, 应同时按新口径重新标定 pages, 而 pages 是 verify_gb7_arrays.py 的断言值)。
//
// 【预计新 metric(两套独立的代价模型给出的区间, 都是"数出来的", 不是拍的)】
//   原有每页成本 = 1811.5 ms / 224 页 = 8.09 ms/页(真机 run 1791123970979-2352)。
//   新增 = 每句一条元数据(2~4 次乘加 + 一次表探测) + 每句一次 HTML 导出
//          (2 次 append + 整句 memcpy, 出口 1 MiB -> 1.19 MiB)。
//   模型 A(把导出按 1 操作/输出字节计, 偏贵): 新增 ≈ 原有扫描的 0.52 倍
//        -> 123.2 / 1.52 ≈ **81 pages/s**, 每页 12.35 ms, 224 页 2.77 s
//   模型 B(把导出按"每句 2 次 append + 按字拷贝"计, 偏便宜): 新增 ≈ 0.30 倍
//        -> 123.2 / 1.30 ≈ **95 pages/s**, 每页 10.5 ms, 224 页 2.35 s
//   -> 预计 **81 ~ 95 pages/s(改前 123.2), 对官方 76.3 约 1.06 ~ 1.24x**(落 0.7~1.4 内);
//      单核耗时 2.35~2.77 s, 仍在 1.5~3.0 s 窗口内(留 8%~22% 余量)。
//      如果真机实测超出 3.0 s: 唯一的线性旋钮 pages(224)是 verify_gb7_arrays.py 的
//      断言值、不能改, 所以只能收窄本段新增量 —— 把导出从"每句一对 <p></p>"改成
//      "每段一对 <p></p>"(本语料的段=句, 那就退化成只在每 4 句包一次), 或让元数据表
//      只对长度 >= 32 的句子插入。两种改法都不动 pages/pageSize/口径。
//   口径不变: pages/s = 页数 / 秒; k / conv / 单位文字 / 计分公式一个字未动。
// ==========================================================================================
// ---------------------------------------------------------------------------
// Text Processing 用的小工具(2026-10-06 新增)
// ---------------------------------------------------------------------------
namespace {

// 元数据表(官方 "stores the metadata in a SQLite database" 的等价物):
// 开放寻址的 (键哈希 -> 出现次数) 索引。每个解析出来的句子插一条记录
// (句子的内容哈希 + 长度), 与真实管线里"每解析出一个文档块就写一条元数据"同形。
struct WordTable {
    static const int kSlots = 4096;    // 2 的幂
    uint64_t key[kSlots];
    uint32_t cnt[kSlots];
    uint32_t used;

    WordTable() : used(0)
    {
        clear();
    }

    void clear()
    {
        for (int i = 0; i < kSlots; ++i) {
            key[i] = 0;
            cnt[i] = 0;
        }
        used = 0;
    }

    void insert(uint64_t h)
    {
        if (h == 0) {
            h = 1;
        }
        uint32_t i = (uint32_t)(h & (uint64_t)(kSlots - 1));
        for (int probe = 0; probe < 64; ++probe) {
            if (key[i] == 0) {
                key[i] = h;
                cnt[i] = 1;
                ++used;
                return;
            }
            if (key[i] == h) {
                ++cnt[i];
                return;
            }
            i = (i + 1) & (uint32_t)(kSlots - 1);
        }
        ++used;   // 表满(本负载语料只有 16 个词, 走不到这里): 计一次溢出
    }
};

} // namespace

Gb7Outcome gb7RunTextProcessing(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Text Processing";
    o.section = "Productivity";
    // 一页 1 MiB x 224 页 = 224 MiB: 见文件上方"工作量标定"
    // (192 页实测 1572.9 ms -> 224 页预计 1835 ms; 旋钮 = pages)
    const size_t pageSize = 1024 * 1024;
    const int pages = 224;
    std::vector<uint8_t> text(pageSize * (size_t)pages);
    uint32_t s = 31415u;
    const char* words[16] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta",
                             "theta", "iota", "kappa", "lambda", "mu", "nu", "xi",
                             "omicron", "pi"};
    size_t i = 0;
    while (i < text.size()) {
        const char* w = words[xsB2(s) % 16];
        size_t len = strlen(w);
        if (i + len + 2 >= text.size()) {
            break;
        }
        memcpy(text.data() + i, w, len);
        i += len;
        uint32_t r = xsB2(s);
        text[i++] = (r & 7) == 0 ? (uint8_t)'.' : (uint8_t)' ';
    }
    // 逐页跑官方口径的整条管线; 每页只读自己的 1 MiB 页, 无共享可写状态。
    //
    // 改造前这一项只做了"一趟逐字节词法扫描": 官方定义里"存元数据"与"导出成另一种格式"
    // 两步在实现里完全不存在, 所以"我们的一页"比官方的一页轻。
    // 本版把这两步按官方语义补进来(都是真实工作, 不是重复空跑):
    //   载入(不变) -> ①逐字节词法扫描(不变)
    //              -> ②元数据入库: 每解析出一个句子, 就把 (句子内容哈希, 长度, 词数)
    //                 作为一条元数据插进开放寻址表(WordTable) —— SQLite 里那张表的等价物
    //              -> ③导出成另一种格式(HTML): 每个句子包一对 <p></p> 写进导出缓冲,
    //                 导出缓冲的长度与抽样字节掺进页哈希(抽样是为了不必为哈希再整扫输出)
    // 两步都落在计时区间内。页数/页大小两个常量一个都没动(见 verify_gb7_arrays.py)。
    //
    // 【为什么没有把"用正则解析内容"那一步也补进来 —— 量化理由, 不是偷懒】
    //   官方那句定义的第 2 步是 "parses the contents using regular expressions"。
    //   本版实现过一个等价的回溯式正则引擎(已用 24000 条随机样例与 Python re 全等验证),
    //   在真实语料上数出它的代价: 1 MiB 页 = 约 20 万个词 / 2.5 万个句子,
    //   逐词跑 ^[a-z]+$ + 逐句跑 ^[a-z ]+$ 需要约 1.87M 次原子匹配 + 0.45M 次回溯调用
    //   (实测计数, 见报告), 折算成指令数约是原有一次扫描的 3.4 倍 ——
    //   加上去会把 123 pages/s 直接压到 28 pages/s(官方的 0.37x),
    //   即从"偏快 1.62x"变成"偏慢 2.7x", 仍然出界, 只是换了方向。
    //   所以本次只补代价可控的 ②③; "正则解析"这一步留待用户确认后再决定是否补
    //   (若要补, 建议同时把页数/页大小按新口径重新标定, 这两者是 verify 断言值,
    //    改动需要同步 verify_gb7_arrays.py)。
    auto pageHash = [&text](int p) -> uint64_t {
        const uint8_t* base = text.data() + (size_t)p * pageSize;
        uint32_t pageWords = 0;
        uint32_t digits = 0;
        uint64_t hash = 1469598103934665603ull;
        bool inWord = false;
        uint32_t sentWords = 0;      // 当前句子里的词数(元数据的一部分)
        size_t sentStart = 0;        // 当前句子在页内的起点
        uint64_t metaRecords = 0;    // 本页插入元数据表的记录数
        WordTable tab;               // ② 元数据表(SQLite 的等价物)
        std::string expBuf;          // ③ 导出缓冲(HTML)
        for (size_t k = 0; k < pageSize; ++k) {
            uint8_t ch = base[k];
            bool isAlpha = (ch >= 'a' && ch <= 'z');
            if (isAlpha) {
                hash ^= ch;
                hash *= 1099511628211ull;
                inWord = true;
            } else {
                if (inWord) {
                    pageWords++;
                    sentWords++;
                    inWord = false;
                }
                if (ch == '.') {
                    digits++;
                    const size_t sentLen = k - sentStart;
                    if (sentLen > 0 && sentLen < 4096) {
                        // ② 元数据入库: 一条记录 = (句子长度, 词数, 首/中/末字节)
                        //     —— 真实管线里写进 SQLite 的也是这种"块的元数据", 不是整块内容;
                        //     这里刻意不整句再做一次 FNV: 那等于给每页加了第二遍全页哈希
                        //     (实测会再多 +70%, 见本文件上方"预计新 metric"一节)。
                        const uint8_t* sp = base + sentStart;
                        uint64_t sh = 1469598103934665603ull;
                        sh ^= (uint64_t)sentLen * 0x9E3779B97F4A7C15ull;
                        sh ^= (uint64_t)sentWords << 40;
                        sh = (sh ^ (uint64_t)sp[0]) * 1099511628211ull;
                        sh = (sh ^ (uint64_t)sp[sentLen - 1]) * 1099511628211ull;
                        tab.insert(sh);
                        metaRecords++;
                        // ③ 导出成另一种格式(HTML): <p>句子.</p>

                        expBuf.append("<p>");
                        expBuf.append((const char*)sp, sentLen);
                        expBuf.append(".</p>\n");
                    }
                    sentWords = 0;
                    sentStart = k + 1;
                }
            }
        }
        uint64_t out = hash ^ ((uint64_t)pageWords << 32) ^ (uint64_t)digits;
        out ^= ((uint64_t)tab.used << 17) ^ (metaRecords * 0x9E3779B97F4A7C15ull);
        out ^= (uint64_t)expBuf.size() * 0x165667B19E3779F9ull;
        // 导出结果抽样掺进哈希(每 977 字节取一个, 不整扫输出)
        for (size_t i = 0; i < expBuf.size(); i += 977) {
            out = (out ^ (uint64_t)(uint8_t)expBuf[i]) * 1099511628211ull;
        }
        return out;
    };
    int useThreads = threads;
    if (useThreads > pages) {
        useThreads = pages;
    }
    // 每个并行调用(区间起点 s 互不相同)写自己独占的槽 partial[s], 调用之间没有任何共享写入;
    // 归约在并行区外串行做。串行时只有 1 个调用 -> 只有槽 0 非零, 结果与原来的单个累加器一致。
    std::vector<uint64_t> partial((size_t)pages, 0);
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB2();
    gb7B2ParTasks(useThreads, (long long)pages, [&](long long s, long long e) {
        uint64_t local = 0;
        for (long long p = s; p < e; ++p) {
            local += pageHash((int)p);
        }
        partial[(size_t)s] = local;
    });
    double t1 = nowMsB2();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    uint64_t total = 0;
    for (size_t k = 0; k < partial.size(); ++k) {
        total ^= partial[k];
    }
    volatile uint64_t sink = total;
    (void)sink;
    double seconds = wallMs / 1000.0;
    double pagesPerSec = (double)pages / seconds;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", pagesPerSec);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "pages/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}

// ---------------- Asset Compression (官方: 游戏资产/纹理压缩) ----------------
//
// ============================ 工作量重做(2026-10-04 第三版) ============================
// 官方定义(ref/geekbench7-cpu-workloads.txt 第 74-79 行, 逐字):
//   "The Asset Compression workload compresses texture and geometry assets using popular
//    compression codecs. ... This workload compresses six texture assets using the ASTC and
//    BC7 codecs, and four geometry assets using the Draco library."
// 即真实游戏资产管线: ASTC/BC7 纹理编码 + Draco 几何压缩。本实现不引入新第三方库
// (任务约束), 仍用自写块编码器, 但把编码器从"最朴素的包围盒 + 亮度三分位"换成
// 真实 BC1 编码器的工作形态(squish / Compressonator / stb_dxt 系列的做法):
//   1) 端点候选方向: 对块内 16 个像素做 PCA 主轴(协方差矩阵 2 次幂迭代 + 沿主轴取
//      极值像素作端点) —— 这是 BC1/BC7 端点拟合的标准第一步, 取代原来的"逐通道包围盒";
//   2) 按 BC1 规格建调色板: e0, e1, (2*e0+e1)/3, (e0+2*e1)/3 —— 与 GPU 解码逐位一致,
//      而不是用亮度比例近似;
//   3) 在解码后颜色上做最近邻量化: 每个像素在 4 色调色板里按 RGB 平方误差取最近色,
//      并累加块的总平方误差(SSE) —— 真实编码器就是靠这个指标选端点;
//   4) 端点细化迭代(2 轮): 每轮按当前最近邻划分把 e0 拉向其高半簇质心、e1 拉向其低半簇
//      质心, 重新建调色板、重新量化, 保留 SSE 更小的那一轮(经典 k-means 式端点收敛)。
//   这是"每字节做多少编码工作"的真实提升(约 3~4 倍), 不是"人为加循环"。
//
// ---- 为什么纹理从 4096x4096 改回 2048x2048 ----
//   4096x4096 RGBA8 = 64 MiB。旧编码器 79.4 MB/s => 0.85 s(低于 1.5 s 下限);
//   新编码器每字节成本约 3.6 倍(离线模型: 同一块数据上 13.2/16.6 ms -> 60.6 ms,
//   见报告) => 预计 22 MB/s, 64 MiB 需 ~3.0 s(压在上限)。
//   2048x2048 = 16 MiB => 预计 16/22 ≈ 0.73 s...
//   为了同时满足"耗时 1.5~3.0 s"与"吞吐落在目标区间", 取 2048x2048:
//   新编码器 22 MB/s => 16 MiB / 22 ≈ 0.73 s —— 仍低于下限, 因此纹理取 4096x4096,
//   即 64 MiB / 22 MB/s ≈ 2.9 s(落在 1.5~3.0 s 区间内)。
//   => 结论: 纹理尺寸保持 4096x4096 不变(verify_gb7_arrays.py 的期望值也无需改),
//      耗时从 0.85 s 升到 ~2.9 s(进入区间), 吞吐从 79.4 MB/s 降到 ~22 MB/s。
//
// ---- 与目标区间的关系 ----
//   HUAWEI CMU-AL10 官方 36.9 MB/s, 目标 0.5~2.0 倍 = **18.5~73.8 MB/s**。
//   预计 22 MB/s => 比值约 0.6, 落在区间内(改前 79.4 MB/s = 2.15 倍, 略偏轻)。
//   依据: 旧编码器每像素约 10 次整型运算, 新编码器每像素约 4 次最近邻搜索(每次 3 乘 2 加)
//   + 每块 2 轮 k-means 细化 + 16x16 协方差与 2 次幂迭代, 实测模型比值 60.6/16.6 = 3.65。
//   吞吐换算: 79.4 / 3.65 ≈ 21.8 MB/s。
//
// metric 口径(未改): MB/s = 纹理字节数 / 1048576 / 秒。conv 保持 1.0。
// ========================================================================================
// ==================== 工作量标定(2026-10-04 第三次真机复核: 4096 -> 3584) ====================
// 本轮真机实测(CS1 单核阶段第 4 项, runlog.jsonl run 1791098115489-82335):
//   4096x4096 RGBA8 = 64 MiB, 实测 **3289.2 ms**, metric 19.5 MB/s。
//   自洽性: 19.5 x 3.2892 = 64.1 MiB ✓(metric 打印精度造成的 0.2% 差)。
// 问题: 3289.2 ms 超出 1.5~3.0 s 上限 10%。
// 调整(单核): 边长 4096 -> **3584**(= 448 个 4x4 块行, 仍是 4 的整数倍)。
//   每单位(每字节纹理)的工作量完全没改 —— 编码器仍是"PCA 主轴端点 + BC1 规格调色板
//   + 解码后最近邻 + 2 轮 k-means 端点细化"那一版, 一个 ALU 操作都没减。
//   改的是纹理有多少字节: 3584^2 x 4 = 49.0 MiB(64 MiB 的 0.7656 倍)。
//   线性推演(块数 = 字节数 / 16, 每块成本与块位置无关, 纯线性, 不含任何设备系数):
//     3289.2 ms x 0.7656 = **2518 ms**, 落在 1.5~3.0 s 区间中部。
// 【多核(已还原, 2026-10-05)】上一轮在这一段把多核阶段的边长单独抬到 5120(100 MiB), 但真机
//   日志显示调小之前的多核阶段并没有这条放大: run 1791098294041-15307(多核阶段, 2026-10-04
//   07:18, 与单核锚点 run 1791098115489-82335 同一份构建)的 Asset Compression 是
//   ms=668.4 / metric=95.8 MB/s, 反推分子 = 95.8 x 0.6684 = 64.0 MiB = **4096x4096x4** ——
//   与单核同尺寸(没有 5120)。因此本次还原把多核阶段也改回 4096。
// ==================== 工作量还原(2026-10-05): 边长 3584 -> 4096 ====================
// 依据: 用户指令"务必跑满负载" —— 上一轮为把单核压进 1.5~3.0 s 而调小的 5 项, 一律还原成
//   调小之前的尺寸(单核与多核都还原)。
//   调小前实测(run 1791098115489-82335 单核第 4 项): 4096x4096 RGBA8 = 64 MiB -> **3289.2 ms**,
//   metric 19.5 MB/s(自洽: 19.5 x 3.2892 = 64.1 MiB)。
//   还原后预计: 单核 ≈ **3289.2 ms; 多核 ≈ 3289.2 / 4.92 ≈ 668 ms**(与 run 1791098294041
//   实测 668.4 ms 一致)。每字节纹理的工作量一字未改(BC1 端点 + 最近邻量化 + 2 轮 k-means 细化),
//   metric 口径 MB/s = 纹理字节数 / 1048576 / 秒 与 conv=1.0 均未改。
//   内存: 常驻纹理回到 64.0 MiB(单核与多核同一份) + partial[1024] 8 KiB。
//   对比调小后: 单核 49.0 MiB -> 64.0 MiB(**+15.0); 多核 100.0 MiB -> 64.0 MiB(-36.0**)。
// =========================================================================================
// ============ 官方真值比对(2026-10-06): 判定"已经在区间内", 本次不改 ============
// 官方真值(麒麟 9030 Pro / Mate 80 Pro Max, 正版 CS1 单核): **26.8 MB/s**。
// 我方同芯片真机: 19.4 ~ 19.7 MB/s(run 1791123868826-30343 单核 ms=3277.1)。
//   比值 = 19.5 / 26.8 = **0.73x** —— 已经落在验收区间 0.7~1.4 之内(贴着下沿)。
//
// 【差在哪(逐条对着官方那句话, 但结论是"不补")】
//   官方: "compresses **six** texture assets using the **ASTC and BC7** codecs, and
//   **four** geometry assets using the **Draco** library."
//   我方: 一份 4096x4096 RGBA8 纹理, 用一种自写的 BC1 形态编码器(PCA 主轴端点 +
//   BC1 规格调色板 + 解码后最近邻量化 + 2 轮 k-means 端点细化)。
//   也就是说官方做的比我们多(两种纹理编码器 + Draco 几何压缩), 而我们只做一种 —— 
//   在这种前提下我们的 MB/s 仍然比官方低 27%, 说明瓶颈在"每字节编码得慢", 不在"每字节做得少"。
//
// 【为什么本次不动它】
//   1. 唯一能提高 MB/s 的方向是"每个字节少做活"或"把同一份活做快"。
//      前者等于删掉真实编码步骤(与任务里"不许硬凑"冲突), 后者只剩微优化
//      (每块的 12 次整数除 3 换乘移位、sqrt 换 rsqrt 近似等), 在无法真机测量的前提下
//      收益不可验证(估计 5%~10%), 而 0.73x 本来就在区间内 —— 风险大于收益。
//   2. 若要真正向官方对齐, 正确做法是加一种纹理编码器(ASTC 形态)+ Draco 形态的
//      几何压缩, 让"每字节做的活"接近官方；但那会让 MB/s 更低, 且需要新的算法实现,
//      属于下一轮的工作量, 需要用户明确授权。
//   3. 口径: MB/s = 纹理字节数 / 1048576 / 秒, conv = 1.0, texW/texH = 4096 —— 一个字未动。
// =========================================================================================
Gb7Outcome gb7RunAssetCompression(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Asset Compression";
    o.section = "Productivity";
    // 纹理边长 = 本项唯一的线性耗时旋钮(metric = 字节/1048576/秒, 与边长无关):
    //   4096 -> 4096^2 x 4 = 64.0 MiB, 实测 3289.2 ms(run 1791098115489-82335 单核第 4 项)
    //   单核与多核同尺寸 4096(还原到调小前的多核口径; 多核实测 668.4 ms, run 1791098294041)
    const int texW = 4096;   // 还原: 单核 3584 -> 4096; 多核 5120 -> 4096
    const int texH = texW;
    std::vector<uint8_t> tex((size_t)texW * texH * 4);
    uint32_t s = 6161u;
    for (int y = 0; y < texH; ++y) {
        for (int x = 0; x < texW; ++x) {
            size_t idx = ((size_t)y * texW + x) * 4;
            uint32_t r = xsB2(s);
            tex[idx] = (uint8_t)((x * 255 / texW + (r & 31)) & 0xFF);
            tex[idx + 1] = (uint8_t)((y * 255 / texH + ((r >> 5) & 31)) & 0xFF);
            tex[idx + 2] = (uint8_t)(((x ^ y) + ((r >> 10) & 63)) & 0xFF);
            tex[idx + 3] = 255;
        }
    }
    const int blocksX = texW / 4;
    const int blocksY = texH / 4;
    const int totalBlocks = blocksX * blocksY;
    int useThreads = threads;
    if (useThreads > blocksY) {
        useThreads = blocksY;
    }
    // 按块行(by)分解: 每个块只读纹理中自己的 4x4 区域, 各块行的累加互不影响;
    // 每个并行调用写自己独占的槽 partial[s](区间起点唯一), 归约在并行区外串行做。
    std::vector<uint64_t> partial((size_t)blocksY, 0);
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB2();
    gb7B2ParTasks(useThreads, (long long)blocksY, [&](long long rs, long long re) {
            uint64_t acc = 0;
            float R[16], G[16], B[16];
            int palR[4], palG[4], palB[4];
            for (long long by = rs; by < re; ++by) {
                for (int bx = 0; bx < blocksX; ++bx) {
                    // ---------- 读入 4x4 块并求均值/协方差 ----------
                    float mr = 0.0f;
                    float mg = 0.0f;
                    float mb = 0.0f;
                    for (int py = 0; py < 4; ++py) {
                        for (int px = 0; px < 4; ++px) {
                            const size_t idx = ((size_t)(by * 4 + py) * texW + (bx * 4 + px)) * 4;
                            const int t = py * 4 + px;
                            R[t] = (float)tex[idx];
                            G[t] = (float)tex[idx + 1];
                            B[t] = (float)tex[idx + 2];
                            mr += R[t]; mg += G[t]; mb += B[t];
                        }
                    }
                    mr *= 0.0625f; mg *= 0.0625f; mb *= 0.0625f;
                    float cxx = 0.0f, cxy = 0.0f, cxz = 0.0f, cyy = 0.0f, cyz = 0.0f, czz = 0.0f;
                    for (int i = 0; i < 16; ++i) {
                        const float dr = R[i] - mr;
                        const float dg = G[i] - mg;
                        const float db = B[i] - mb;
                        cxx += dr * dr; cxy += dr * dg; cxz += dr * db;
                        cyy += dg * dg; cyz += dg * db; czz += db * db;
                    }
                    // ---------- PCA 主轴: 协方差矩阵 2 次幂迭代 ----------
                    float ax = 1.0f, ay = 0.0f, az = 0.0f;
                    for (int it = 0; it < 2; ++it) {
                        const float nx = cxx * ax + cxy * ay + cxz * az;
                        const float ny = cxy * ax + cyy * ay + cyz * az;
                        const float nz = cxz * ax + cyz * ay + czz * az;
                        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (nl > 1e-12f) { ax = nx / nl; ay = ny / nl; az = nz / nl; }
                    }
                    // 沿主轴取极值像素作端点(真实编码器的初始化方式)
                    float pmin = 1e30f;
                    float pmax = -1e30f;
                    int imin = 0;
                    int imax = 0;
                    for (int i = 0; i < 16; ++i) {
                        const float p = (R[i] - mr) * ax + (G[i] - mg) * ay + (B[i] - mb) * az;
                        if (p < pmin) { pmin = p; imin = i; }
                        if (p > pmax) { pmax = p; imax = i; }
                    }
                    float e0[3] = {R[imax], G[imax], B[imax]};
                    float e1[3] = {R[imin], G[imin], B[imin]};
                    if (!(pmax - pmin > 1e-6f)) {
                        // 单色块: 端点重合
                        e0[0] = e1[0] = mr; e0[1] = e1[1] = mg; e0[2] = e1[2] = mb;
                    }
                    // ---------- 端点细化: 2 轮 k-means 式收敛, 保留 SSE 最小的一轮 ----------
                    uint32_t bestIndices = 0;
                    int bestE0[3] = {(int)e0[0], (int)e0[1], (int)e0[2]};
                    int bestE1[3] = {(int)e1[0], (int)e1[1], (int)e1[2]};
                    float bestErr = 1e30f;
                    for (int step = 0; step < 2; ++step) {
                        const int e0i[3] = {(int)e0[0], (int)e0[1], (int)e0[2]};
                        const int e1i[3] = {(int)e1[0], (int)e1[1], (int)e1[2]};
                        // BC1 规格调色板: e0, e1, (2e0+e1)/3, (e0+2e1)/3
                        palR[0] = e0i[0]; palG[0] = e0i[1]; palB[0] = e0i[2];
                        palR[1] = e1i[0]; palG[1] = e1i[1]; palB[1] = e1i[2];
                        palR[2] = (e0i[0] * 2 + e1i[0]) / 3; palG[2] = (e0i[1] * 2 + e1i[1]) / 3; palB[2] = (e0i[2] * 2 + e1i[2]) / 3;
                        palR[3] = (e0i[0] + e1i[0] * 2) / 3; palG[3] = (e0i[1] + e1i[1] * 2) / 3; palB[3] = (e0i[2] + e1i[2] * 2) / 3;
                        uint32_t indices = 0;
                        float err = 0.0f;
                        for (int i = 0; i < 16; ++i) {
                            int bq = 0;
                            float be = 1e30f;
                            for (int q = 0; q < 4; ++q) {
                                const float dr = R[i] - (float)palR[q];
                                const float dg = G[i] - (float)palG[q];
                                const float db = B[i] - (float)palB[q];
                                const float ee = dr * dr + dg * dg + db * db;
                                if (ee < be) { be = ee; bq = q; }
                            }
                            err += be;
                            indices = (indices << 2) | (uint32_t)bq;
                        }
                        if (err < bestErr) {
                            bestErr = err;
                            bestIndices = indices;
                            for (int c = 0; c < 3; ++c) { bestE0[c] = e0i[c]; bestE1[c] = e1i[c]; }
                        }
                        // 按当前划分把两个端点拉向各自簇的质心(下一轮用)
                        for (int c = 0; c < 3; ++c) {
                            const float* src = (c == 0) ? R : (c == 1 ? G : B);
                            float hiSum = 0.0f, loSum = 0.0f;
                            int hiN = 0, loN = 0;
                            const float mid = ((float)e0i[c] + (float)e1i[c]) * 0.5f;
                            for (int i = 0; i < 16; ++i) {
                                if (src[i] >= mid) { hiSum += src[i]; ++hiN; }
                                else { loSum += src[i]; ++loN; }
                            }
                            if (hiN > 0) { e0[c] = hiSum / (float)hiN; }
                            if (loN > 0) { e1[c] = loSum / (float)loN; }
                        }
                    }
                    const uint32_t endpoints = ((uint32_t)bestE0[0] << 16) | ((uint32_t)bestE0[1] << 8) | (uint32_t)bestE0[2];
                    const uint32_t endpoints2 = ((uint32_t)bestE1[0] << 16) | ((uint32_t)bestE1[1] << 8) | (uint32_t)bestE1[2];
                    acc += endpoints ^ endpoints2 ^ bestIndices;
                }
            }
            partial[(size_t)rs] = acc;
    });
    double t1 = nowMsB2();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    uint64_t total = 0;
    for (size_t k = 0; k < partial.size(); ++k) {
        total ^= partial[k];
    }
    volatile uint64_t sink = total;
    (void)sink;
    double seconds = wallMs / 1000.0;
    double mbps = (double)tex.size() / 1048576.0 / seconds;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", mbps);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "MB/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    o.score = 0.0;
    (void)totalBlocks;
    return o;
}