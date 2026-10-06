#ifndef AURORA_GB7_H
#define AURORA_GB7_H

// ===========================================================================
//  显示名 = CS1; 内部键仍用 gb7 前缀
// ---------------------------------------------------------------------------
//  套件显示名改过两次: 8.4 把 GB7 改成 GB8, 本次开源改名再把 GB8 改成 CS1。
//  代码里的内部标识符与 JSON 键名一律保留 gb7 前缀
//  (gb7RunTest / gb7SingleComposite / gb7Items / 'gb7s' 等等)。
//  为什么不改: 报告对比工具、历史记录与 runhistory 都按这些键名读,
//  改名会直接断掉跨版本对比。显示名与内部键指的是同一套东西。
//  另: 两次改名都只动显示名, 不改任何负载 / 单位 / k / conv / 计分公式, 所以
//  benchVersion 不递增, 旧 GB7 / GB8 分数与新 CS1 分数在数值上完全可比。
// ===========================================================================

#include <string>
#include <vector>

#include "cpu_affinity.h"      // AuroraCpuPlacement(亲和性诊断结构, 见 cpu_affinity.h)
#include "cpu_freq_sample.h"      // 运行时实际频率采样(旁路; 见该文件的文件头)

// ---------------------------------------------------------------------------
// 多核阶段的工作量倍数(2026-10-04 第三次真机复核新增)
// ---------------------------------------------------------------------------
// 为什么需要它: 单核阶段每一项的尺寸必须落在 1.5~3.0 s。同一份尺寸在多核阶段只花
//   (单核耗时 / 并行加速比) —— 本机 HOP-AL00(允许 9 核)实测加速比 2.3~6.4x,
//   于是多核读数会落到 0.37~1.0 s, 并行开销占比过大、读数不稳。
// 它是什么: 一个编译期常量(所有设备、所有机型同一个值, 没有任何按机型/SoC 的分支)。
//   上一轮(2026-10-04 第三次真机复核)曾把它作用在三个负载的线性尺寸旋钮上:
//     * Ray Tracer          : samples 3 -> 6   (单位数 x2, 单位 = 一条完整光路)
//     * HDR                 : w/h 各 x2       (1296x704 -> 2592x1408, 像素数 x4)
//     * Asset Compression   : 边长 3584 -> 5120(面积 x2.04 = 49 -> 100 MiB)
//   2026-10-05 全部撤销: 用户要求把上一轮为压进 1.5~3.0 s 而调小的 5 项还原成"调小之前"
//   的尺寸, 而真机日志(run 1791098294041-15307 多核阶段, 与单核锚点 run 1791098115489-82335
//   同一份构建)证明调小之前的多核阶段并没有这条缩放: 该轮多核的 Asset = 64.0 MiB(4096)、
//   HDR = 1.263 Mpx(1536x832)、Ray Tracer = 1.176 Msamples(384x384x8), 全部与单核同尺寸。
//   因此上面三条缩放已随还原一并去掉, 三个负载现在单核/多核跑同一份尺寸。
//   本宏与 gb7WorkScale() 保留但当前没有任何负载使用(不留死代码语义: 保留是因为它描述了
//   "多核读数偏低"这个仍然存在的现象, 将来若要重新启用, 只需在有实测锚点的负载上重新引用)。
//   加的是真实工作量(更多像素/更多样本), 不是空转、不是 sleep、不是重复空跑。
// 口径不变: metric 是"吞吐"(单位数/秒), 与尺寸无关 —— k / conv / 单位 / 计分公式
//   一个都没有动。
// 适用范围(历史): 只用在内存代价可接受的多核项上(Asset Compression / HDR / Ray Tracer)。
//   文本处理(Text Processing)无法使用: 它的单位是"1 MiB 的页", 页数翻倍 = 常驻语料
//   192 MiB -> 384 MiB(见 gb7_batch2.cpp 文件头的内存说明), 因此保持单核尺寸。
#define GB7_MULTI_WORK_SCALE 2
inline int gb7WorkScale(int threads)
{
    return (threads > 1) ? GB7_MULTI_WORK_SCALE : 1;
}

// ---------------------------------------------------------------------------
//   可重复性 / 离散度(2026-08-31 新增): CS1 的 16 项单核与 8 项多核也要跑多轮 
// ---------------------------------------------------------------------------
// 为什么: 主流跑分软件建立可信度的方式都是
//   重复测量: 同一项跑多轮 -> 报中位与离散度 -> 离散度大就明说这个数字不可信。
//   本工程此前只有 GPU-SNL 小节跑多轮(libaurorasn), GB7 的单核 16 项 / 多核 8 项只跑一轮,
//   于是 GB7 的分数既没有离散度、也没有可信度判定 —— 用户的要求是"要有跑分的意义,
//   像其他主流跑分软件一样", 这一块就是补它。
//
// 口径(硬约束, 一个都不许改):
//   * 轮数只能通过调用方选项改(runGb7 的第三个参数: {"rounds":N} 或 {"gb7Rounds":N});
//     单轮的负载工作量一个字都没动 —— 每一轮都是同一次完整测量, 我们只是把它做了 N 次。
//     算法 / 尺寸 / metric / unit / k / conv / 计分公式 / 线程数 / 绑核策略全部保持原样。
//   * 代表值一律取中位(median)。代码里不存在"取最好一轮"的路径 ——
//     逐轮原始值全部列出来(repeatability.roundsDetail), 想核对随时可以核对。
//   * 每一轮各自按老公式算分(score = k x metric x conv); 代表分取逐轮分数的中位。
//     score 是 metric 的严格增函数, 所以"中位分 == k x conv x 中位吞吐"必然同时成立。
//   * 轮数只有 1 时: 写"本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度",
//     不假装稳定(见 gb7RepeatabilityJson 的 verdict = SINGLE_ROUND_NO_DATA)。
//   * 复合分同样有离散度: 每一项都跑完多轮之后, gb7CompositeSingle/Multi 的返回里
//     多出一块 repeatability —— 逐轮复合分(把每一项那一轮的分按同一套几何平均重算)
//     的中位/最小/最大/相对离散度 + 可信度判定。报告里的 composite 字段仍然是"由各项
//     中位分算出的几何平均"; 两个数都报出来, 不替用户挑一个好看的。
//
// 阈值(是我们自己定的, 不是官方阈值; 与 libaurorasn 的 GPU-SNL 阈值保持一致):
//     RELIABLE   相对离散度 <= 2%
//     FAIR       2% < 相对离散度 <= 5%
//     UNRELIABLE 相对离散度 > 5%  -> 报告里明确写"本项重复性差, 该数字仅供参考"
//   相对离散度 = (最大 - 最小) / 中位 x 100%(与 GPU-SNL 同一定义)。
#define GB7_DEFAULT_ROUNDS 2      // 默认轮数(一键全部跑分走的也是这个默认值)
#define GB7_MAX_ROUNDS 9          // 上限: 再多就是浪费用户时间, 且收益极小
#define GB7_RELIABLE_SPREAD_PCT 2.0
#define GB7_FAIR_SPREAD_PCT 5.0

// CS1: 负载注册与计分
struct Gb7Outcome {
    std::string name;
    std::string section;
    double ms;
    double score;
    std::string metric;
    std::string unit;
    double parallelism;
    std::string basis;   // 单位换算与系数说明(官方单位, conv, 来源, 假设)
    // 本次负载实际跑在哪个核 / 哪个簇(诊断旁路): 由 gb7RunTest 在负载跑完、计时区间之外
    // 采样填入。注意: 它不是 metric/unit 的一部分, 不计分, 也不改变任何负载的
    // 工作量。napi 层把它展开成 runGb7 JSON 的
    //   cpu / cpuMaxKhz / cpuRank / cpuBound / cpuAtStart / cpuWorkers / cpuInfo
    //   cpuInFastCluster / cpuAtStartInFastCluster                     <- "绑核是否生效"的判据
    //   cpuFastClusterCores / cpuFastClusterMaxKhz / cpuFastClusterMask <- 生效快簇的形状
    // 判据变化(2026-10): 旧判据是 cpu == cpuAtStart, 新判据是 cpuInFastCluster
    // (结束时的核属于大核簇) —— 簇内迁移是设计允许的, 掩码被推翻才要报警。
    // 判据变化(2026-10-05): 快簇的定义改成 "(全机最快频率档) ∩ (可用核集合)"; 交集为空时
    // 不假装绑定成功(cpuBound = false + cpuBoundReason), 实际目标退化为可用核集合里最快的核。
    // 同时把两个概念分开报(不混成一个):
    //   cpuInFastCluster      = 在不在"生效快簇"里(绑核有没有生效)
    //   cpuInMachineTopTier   = 在不在"全机最快频率档"里(我们到底够不够快)
    // napi 层展开的完整键见 napi_init.cpp 的 ExecuteGb7Body; 结构见 cpu_affinity.h 的
    // AuroraCpuPlacement(其中 cpuAllowed* / cpuFastClusterSource / cpuAllowedText /
    // cpuBoundReason 就是"为什么这台机器被按住了"的取证字段)。
    AuroraCpuPlacement cpuInfo;
    // 诊断自证串(旁路字段, 2026-10-06 新增): 由负载自己填一行 "k=v k=v ..." 的文本, 说明
    //   "这一次到底跑了多少工作量 / 有没有提前退"。为什么需要它: 有些负载的计时区间里含
    //   不影响 metric 分子、也不影响 unit 的工作(典型是 Video Decoder 的"全流自检"),
    //   一旦那段工作提前退出, o.ms 会掉而 o.metric / o.unit 一个字都不变 —— runlog 里看不出
    //   差别, 只能靠人去猜(6.2 的 Video Decoder 掉 31% 就是这么变成悬案的)。
    // 约束: 纯诊断, 不参与 metric / unit / 计分 / 任何负载的算法与工作量尺寸;
    //   为空字符串时 napi 层照样输出 "diag":""(形状恒定, ArkTS 侧不用判 undefined)。
    // napi 层展开成 runGb7 JSON 的 "diag" 键。
    std::string diag;
    // 运行时实际频率的一行文本(旁路字段, 2026-10 新增): 由 gb7RunTest 在负载跑完、
    //   采样线程收尾之后填, 内容形如
    //   "运行时频率 中位 1220MHz / 最小 1150MHz / 最大 2270MHz(本核标称上限 2270MHz) 采样 12 次;
    //    采样核 = 本线程所在核(cpu8); 口径 = 跑本项负载的那个线程在每次采样时刻所在的核, 不是
    //    全机所有核; 源 scaling_cur_freq 全部读到(errno=0; 本项 12/12 次)…"
    // 为什么必须有(这是当前最大的测量盲点): 本工程此前只报 cpuinfo_max_freq(标称上限),
    //   于是一台"标称 2270MHz 但被限到 1200MHz"的机器与一台"标称 2150MHz 但跑满"的机器在日志里
    //   长得一模一样。真实频率由负载自己的计时区间内的后台采样线程给出(每 200ms 一次),
    //   口径与采样实现见 cpu_freq_sample.h, 那里也写了开销论证。
    // 约束: 旁路 —— 不参与 metric / unit / k / conv / 单位口径 / 任何计分公式, 也不改变
    //   任何负载的算法、尺寸与线程数; 为空字符串时 napi 层照样输出 "runFreq":""(形状恒定)。
    // 同一行文本还会被追加到 cpuInfo.cpuAllowedText(即每项 note 里 "cpuset: …" 那一段之后),
    //   因此不需要动 ArkTS 就能在 runlog 的 note 里看到它。
    std::string runFreq;
    // QoS 运行条件的一行文本(旁路字段, 2026-10 新增): 由 gb7RunTest 在负载跑完之后
    //   填, 内容形如
    //   "QoS(运行条件, 不计分): 本次已启用 QoS · 负载线程等级=QOS_USER_INTERACTIVE(5)
    //    (调用 17 次: 成功 17 / 失败 0; 首次 rc=0 errno=0…) · 旁路/采样线程等级=QOS_BACKGROUND(0)…
    //    · libqos.so: dlopen 成功, 3 个符号全部拿到 · canIUse(…QoS.Core) = true · 设/不设对照(…)
    //    -> 份额比(设/不设) = 1.8342"
    // 为什么必须有: 用户明确要求"必须在报告里标注『本次跑分是否启用了 QoS 及其等级』"。
    //   口径、官方接口出处、dlopen 探测与降级策略全部写在 qos_priority.h 的文件头。
    // 约束: 纯运行条件, 不参与 metric / unit / k / conv / 计分公式 / 任何负载的算法与
    //   线程数; 为空字符串时 napi 层照样输出 "qos":""(形状恒定, 不判 undefined)。
    // 同一行文本还会被追加到 cpuInfo.cpuAllowedText(即每项 note 里 "cpuset: …" 那一段之后),
    //   因此不需要动 ArkTS 就能在 runlog 的 note 里看到它。
    std::string qos;
    // 只读分层诊断的完整文本(cgroup / cpuset 分组 / core_ctl; 旁路字段, 2026-10 新增):
    //   由 gb7RunTest 在负载跑完之后填, 内容形如
    //   "[1] /proc/self/cgroup: 读到(errno=0) 原文=[0::/top-app] · [2] cgroup v2 目录=… ·
    //    [3] /dev/cpuset: 打开成功(errno=0), 记到 6 个分组 · [3.1] top-app: cpus=[0-7](errno=0)
    //    effective_cpus=[0-7](errno=0) · [4] cpu/online=[0-13](errno=0) · cpu/possible=[0-13](errno=0) ·
    //    [5] core_ctl 逐核(…): cpu0{enable=0,…}(errno=…) · [6] 与逐核实测的可用核集合对照: 8 个核 [0-7] ·
    //    [7] 限制在哪一层(只读结论): …"
    // 目的(用户原话): "定位限制到底在哪一层" —— 逐项记 errno, 读不到就写读不到, 只读,
    //   一个字节都不写这些节点; 不计分。口径见 cpuset_probe.h 的文件头。
    // 为空字符串时 napi 层照样输出 "cpuset":""(形状恒定)。
    std::string cpuset;

    // ---- 可重复性 / 离散度(2026-08-31 新增; 纯统计旁路, 不改任何负载) ----
    // 逐轮原始值全部保留(不只留最好一轮): 分数 / 吞吐 / 计时 / 那一轮跑在哪个核 /
    // 那一轮的运行时频率文本。rounds = 1 时它们只有一个元素, 代表"没有离散度数据"。
    int rounds = 1;              // 本次实际跑了几轮
    int roundsRequested = 1;     // 选项/默认值要求的轮数(已夹到 1..GB7_MAX_ROUNDS)
    bool roundsFromOption = false; // true = 轮数来自调用方选项; false = 用的默认值
    std::vector<double> roundScores;
    std::vector<double> roundMetrics;
    std::vector<double> roundMs;
    std::vector<int> roundCpu;             // 每一轮负载跑完时所在的核(-1 = 取不到)
    std::vector<std::string> roundRunFreq; // 每一轮的运行时频率文本(旁路诊断)
    double scoreMedian = 0.0;    //  代表分(中位) —— 与 o.score 同一个值
    double scoreMin = 0.0;
    double scoreMax = 0.0;
    double scoreSpreadPct = 0.0;
    double metricMedian = 0.0;   // 代表吞吐(中位)
    double metricMin = 0.0;
    double metricMax = 0.0;
    double metricSpreadPct = 0.0;
    double msMedian = 0.0;       // 代表计时(中位)
    double msMin = 0.0;
    double msMax = 0.0;
    // 自检: 代表分 == k x conv x 代表吞吐(score 是 metric 的严格增函数 -> 必然成立;
    // 对不上说明计分口径被动过, 报 false)
    double metricImpliedScore = 0.0;
    bool metricImpliedScoreOk = true;
    // RELIABLE / FAIR / UNRELIABLE / SINGLE_ROUND_NO_DATA(轮数不足时不许判可信度)
    std::string repeatVerdict;
    std::string repeatText;      // 一句话人话结论(含"阈值是我们自己定的")
};

Gb7Outcome gb7RunFileCompression(int threads);
Gb7Outcome gb7RunNavigation(int threads);
Gb7Outcome gb7RunTextProcessing(int threads);
Gb7Outcome gb7RunAssetCompression(int threads);
Gb7Outcome gb7RunPhotoLibrary(int threads);
Gb7Outcome gb7RunPhotoEditor(int threads);
Gb7Outcome gb7RunHdr(int threads);
Gb7Outcome gb7RunRayTracer(int threads);
Gb7Outcome gb7RunGamePhysics(int threads);
Gb7Outcome gb7RunPdfViewer(int threads);
Gb7Outcome gb7RunHtml5Browser(int threads);
Gb7Outcome gb7RunAudioEncoder(int threads);
Gb7Outcome gb7RunVideoEncoder(int threads);
Gb7Outcome gb7RunVideoDecoder(int threads);
Gb7Outcome gb7RunStructureFromMotion(int threads);
Gb7Outcome gb7RunClang(int threads);

int gb7TestCount();
std::string gb7TestName(int id);
std::string gb7TestSection(int id);

// 单轮: 跑一次负载, 按 score = k x (metric x conv) 算一次分。语义与历史版本完全一致
// (多轮路径 gb7RunTestRepeated 就是把它调用 N 次, 每一轮都是同一次完整测量)。
Gb7Outcome gb7RunTest(int id, int threads);

// ---- 可重复性(2026-08-31 新增): 轮数只来自选项, 单轮工作量一个字都没动 ----
// 选项 JSON(扁平, 全部可选): { "rounds": N } 或 { "gb7Rounds": N }(1..GB7_MAX_ROUNDS)。
// 解析不到 / 越界 -> 用默认值 GB7_DEFAULT_ROUNDS(=2), 并把"这是默认值"记进 *outFromOption。
int gb7RoundsFromOptions(const std::string& optionsJson, bool* outFromOption);

// 多轮: 跑 rounds 轮, 代表值一律取中位(不是最好一轮, 也不是平均),
// 并把中位/最小/最大/相对离散度/可信度判定填进 Gb7Outcome 的 repeat* 字段。
// rounds <= 1 时等价于 gb7RunTest(id, threads), 且 verdict = SINGLE_ROUND_NO_DATA。
Gb7Outcome gb7RunTestRepeated(int id, int threads, int rounds, bool roundsFromOption);

// 重复性 JSON 片段(以 "," 开头, 供 napi 层直接拼进 runGb7 的返回值):
//   ,"repeatability":{ rounds / roundsOk / roundsDetail[] / score{median,min,max,dispersion}
//                      metric{...} / ms{...} / representative / dispersionDefinition
//                      credibility{verdict,thresholdsAreOursNotOfficial,text} / singleRoundWarning }
// 字段名与 libaurorasn 的 repeatability 块保持同一风格(便于 UI / 报告统一渲染)。
std::string gb7RepeatabilityJson(const Gb7Outcome& o);

// ---- 官方口径元数据(供 UI / 复核使用) ----
// 是否属于官方多核套件(官方多核表里只有 8 项, 其余 8 项根本不出现在多核页)
bool gb7TestIsMulti(int id);
// 该项是否计分(false = 语义不可比, 分数恒为 0, 不进入复合分)
bool gb7TestScored(int id);
// 单位换算 + 系数来源说明(可直接显示)
std::string gb7TestBasis(int id);
// 该项在 GB7 结果页上的单位
std::string gb7TestGb7Unit(int id);
// 官方单位值 = 本实现 metric x conv
double gb7TestConversion(int id);
// 拟合出的 k(官方单位口径), multi!=0 取多核表的 k
double gb7TestKOfficial(int id, int multi);
// 实际乘在本实现 metric 上的系数 = kOfficial x conv(未计分项为 0)
double gb7TestK(int id, int multi);

// ---- 复合分(几何平均) ----
// scores 传 nullptr / count<=0 时使用最近一次 gb7RunTest 的单项分。
// 单核 = 已计分项几何平均(SfM 未计分被剔除); 多核 = 官方多核 8 项几何平均。
// 返回 JSON 里除 composite 之外, 自 2026-08-31 起还带一块 repeatability(见上),
// 内容是"逐轮复合分"的中位/最小/最大/相对离散度 + 可信度判定(阈值同样是我们自己定的);
// 每一项只跑 1 轮时写"没有离散度数据, 不能据此判断可信度"。
std::string gb7CompositeSingle(const double* scores, int count);
std::string gb7CompositeMulti(const double* scores, int count);

// 旧接口(按传入数组做几何平均), 保留兼容
std::string gb7Composite(const double* scores, const char* const* sections, int count);

#endif
