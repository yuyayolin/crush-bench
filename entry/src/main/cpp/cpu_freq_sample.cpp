// ===========================================================================
//  运行时实际频率采样(旁路诊断)—— 实现在这里; 口径与调用约定见 cpu_freq_sample.h
// ===========================================================================
//  本文件只做三件事:
//    ① 后台一个采样线程, 每 200 ms 读一次 sysfs 的 scaling_cur_freq(当前频率, kHz);
//    ② 在负载自己的计时区间内累积样本(计时区间由负载调用 markStart/markStop 圈定);
//    ③ 计时区间结束后汇总成一行中文文本(最小/中位/最大/样本数/采样核/口径/errno)。
//
//  设计约束(逐条对应任务要求)
//  ---------------------------------------------------------------------------
//    * 采样线程不做任何内存分配: 没有 std::string / std::vector / opendir / readdir,
//      没有 new/malloc, 全部用固定大小的静态数组与栈上的小缓冲(32~1024 B);
//      唯一的 std::string 出现在汇总里, 那已经在线程 join 之后、在计时区间之外;
//    * 采样线程不写任何内核节点: 只有 open(O_RDONLY)/read/close 与 nanosleep 语义的
//      condition_variable 等待。读 cpufreq 属性不会触发调频、不会改 governor;
//    * 计时区间内只有两次互斥量加解锁(markStart/markStop), 各 ~20~60 ns, 且都在
//      t0 之前 / t1 之后 —— 连这点开销也不进 o.ms;
//    * 失败一律上报(errno + 核号 + 失败次数), 不用 0 冒充"读到了"。
// ===========================================================================
#include "cpu_freq_sample.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <dirent.h>   // 只读枚举 policyN / cooling_deviceN(上限取证一节; 不在采样线程里)
#include <fcntl.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "cpu_affinity.h"   // 只读用: 隐藏的核集合 / 标称上限 / cpulist 格式化(都是 inline 纯读)

// 旁路线程的 QoS(运行条件): 采样线程设 **QOS_BACKGROUND**, 别去抢负载线程的 CPU 时间份额。
// 口径与降级策略见 qos_priority.h 文件头; 它不改任何采集口径, 不计分。
#include "qos_priority.h"

namespace {

// ---------------------------------------------------------------------------
//  常量(全部是编译期常量; 没有任何按机型/SoC 的分支)
// ---------------------------------------------------------------------------
const int  kIntervalMs      = 200;    // 采样间隔(任务要求 200~500 ms, 取最密的一端)
const int  kMaxSamples      = 2048;   // 单次会话的样本硬上限(超出只记 overflow, 不分配)
const int  kMaxSampleCpus   = 64;     // 采样核集合上限(与 cpu_affinity.h 的位图口径一致)
const int  kMaxThermalZones = 16;     // 参考项: 最多探 16 个热区
const int  kThermalEveryN   = 5;      // 每 5 个 tick 读一次热区(约 1 s 一次)

// ---- "跑满"判据与升频/稳态预热(2026-10 新增; 阈值全部是我们自己定的, 不参与计分) ----
const int  kFullRatioPct      = 90;   // 占标称比中位 >= 90% 记为"本项已跑满"
const int  kSteadyDeltaPct    = 3;    // 相邻两遍(或单遍前后半段)频率中位差 <= 3% 记为稳态
const int  kWarmMinSamples    = 3;    // 一遍至少这么多样本才敢拿它判稳态
const int  kWarmHalfSamples   = 6;    // 单遍"前后半段"判据要求的最小样本数
const int  kMaxWarmupPasses   = 3;    // 预热最多这么多遍(硬上限, 不无限加压)
const int  kWarmupMaxTotalMs  = 8000; // 预热总墙钟上限(ms); 到点就停, 原因上报
const int  kMaxTickCores      = 32;   // 单个 tick 里"有线程落上的核"的枚举上限
const int  kMaxWorkerTids     = 64;   // 池线程 tid 登记表大小(超出记为溢出, 不影响负载)
const int  kCapProbeCores     = 2;    // 频率上限取证最多探几个核(读数总量要小)
const int  kCapFreqBuf        = 256;  // scaling_available_frequencies 可能很长(逐档列出)
// ---- policy 目录枚举式取证的上限(2026-10 追加; 全部是编译期常量) ----
const int  kCapMaxPolicies    = 16;   // 最多收这么多 policyN 目录(一台机器的频率域不会这么多)
const int  kCapMaxCooling     = 24;   // 最多收这么多 cooling_device 编号(只用于"替代读数"一节)
const int  kCapTisBuf         = 1024; // 读 time_in_state 的栈上缓冲(16 档 x ~20 字节足够)
// 上限取证那一行文本的容量。不足时仍然显式写"尾部被截断"(见 readFreqCapEvidence 末尾的
// 兜底块) —— 但正常路径下一次都不许触发。
// 容量 2048 -> 8192(2026-10) —— 为什么是这个数、上界怎么估的(逐条可复核):
//   ① 真机 8.1(report-latest.txt)里这一段只留下来了前 1924 字节: 48 个 CS1 项的
//      『运行时频率=』行全都在同一个字节点上被截断(截断前最后几个字节是
//      "cpu0 scaling_available_f"), 紧随其后就是 capText 自己打的截断标记 —— 也就是
//      说 [逐核路径] 的后半 + 判决 + 两条规则整段都没进报告, 而那一段正是回答
//      "1995MHz 是谁压的"的原始证据(哪里读不到、errno 是多少, 逐条清单);
//   ② 被切掉的那一段按源码顺序逐条 append 重建(见 verify_freq_cap_chain.py 里那份
//      可执行的重建/估算, 输入用真机自己报出来的取值: policy0/1/2 的
//      EACCES(13)/ENOENT(2)/cpuinfo=1720000/2270000/2750000/cur=418000/1995000/1200000、
//      两个探针核 cpu0 与 cpu10、cpuinfo_max_freq=1720000 与 2270000、判决走"上限读数读不到"
//      那一支) = 1995 字节;
//   ③ 于是这一段的真实需要 = 1924 + 1995 = 3919 字节(再加 1 字节 NUL = 3920);
//   ④ 8192 >= 3919 x 1.5(= 5879), 相对 3920 的余量 = +109%;
//   ⑤ 上界估算(把每个片段按最大实参宽度渲染: 核号/策略号 <= 2 位、kHz <= 7 位、
//      errno 词 <= 15 B、只读节点原文 <= kCapFreqBuf-1 = 255 B、单个 snprintf 片段受
//      one[] 约束) = 5652 字节, 仍 <= 8192(余量 +45%);
//   ⑥ 只加容量, 内容与措辞一个字未动(短内容的行为逐字节不变)。
const int  kCapTextCap        = 8192;
// 只读取证的两个根目录(policyN 目录 / 冷却设备目录)。取值来自内核的固定路径, 不随机型变。
const char kCapFreqRoot[]    = "/sys/devices/system/cpu/cpufreq";
const char kCapThermalRoot[] = "/sys/class/thermal";

// 采样核的频率读数: 每个 tick 的文件与字节量(用于开销自证, 注释里算清楚)
//   scaling_cur_freq 文本 ≈ 8 字节(如 "1220000\n")
//   /proc/self/task/<tid>/stat ≈ 300 字节(一行, 其中 processor 是第 39 个字段)
//   热区 temp ≈ 7 字节(最多 16 个)
//   -> 单核口径每个 tick ≈ 320 B; 多核口径(14 核)≈ 14*8 + 300 + 16*7 = 524 B
// 注: /proc/self/task/<tid>/stat 的读缓冲。**名字必须与下面那段 /proc/stat 快照用的
// kProcStatBuf(8192, 整机逐核 jiffies)区分开** —— 两者曾经同名, 于是下面那份新的
// 定义把这里的 1024 覆盖成了 8192, 直接编译失败(2026-10 修复)。
const int  kThreadStatBuf   = 1024;   // /proc/self/task/<tid>/stat 的固定读缓冲

// ---------------------------------------------------------------------------
//  会话状态(整个进程同一时刻只有一项 GB7 CPU 负载在跑 —— 与 cpu_affinity.h 的
//  SessionState 同一个前提: napi 异步逐项串行)
// ---------------------------------------------------------------------------
// ---- 逐核占用快照(2026-10 追加) ----
//  口径: 整机, 逐核累计 jiffies; 两次读数之差 = 计时区间里每个核的 idle/busy 比例。
//  定义放在这里(而不是它原来所在的"读取实现"旁边): FreqSession 里有两个这样的成员,
//  结构体必须先完整可见才能定义成员(原来的顺序直接编译失败, 2026-10 修复)。
const int kMaxStatCpus = 64;
const int kProcStatBuf  = 8192;   // /proc/stat 整机快照的静态读缓冲(与 kThreadStatBuf 不同)

struct ProcStatSnapshot {
    int ok;                          // 1 = 读到并解析出 >= 1 个核
    int err;                         // 失败原因(errno; -1 = 读空; -2 = 没有 cpuN 行)
    int n;                           // 解析出的核数
    long long idle[kMaxStatCpus];    // idle + iowait
    long long busy[kMaxStatCpus];    // user + nice + system + irq + softirq + steal
    ProcStatSnapshot() : ok(0), err(0), n(0), idle(), busy() {}
};

// 文本工具的前置声明(定义在本文件后面的"文本工具"一节; 上限取证要用到 errno 的人话名字)
void errnoText(char* dst, int cap, int e);

// ---------------------------------------------------------------------------
//  一遍预热的读数(升频/稳态预热用; 见 cpu_freq_sample.h 的判据说明)
// ---------------------------------------------------------------------------
struct WarmPassRec {
    int used;         // 这一遍有没有执行过
    int count;        // 这一遍收到的样本数
    int loKhz;        // 最小
    int medKhz;       // 中位
    int hiKhz;        // 最大
    // 占标称比(逐样本 = 该样本频率 / 该样本所在核的标称上限)的最小/中位/最大,
    // 单位是千分比(整数, 打印时除 10 取一位小数), 这样 69.6% 不会被截成 69%。
    int loRatioPermille;
    int medRatioPermille;
    int hiRatioPermille;
    int firstHalfMedKhz;   // 前半段样本的中位(判"这一遍内部还在不在往上走")
    int secondHalfMedKhz;  // 后半段样本的中位
    int cores;             // 这一遍采样到的不同核数
    int workerTidCount;    // 这一遍登记到的池线程 tid 数(多核项; 单核项恒为 0)
    int windowClosed;      // 1 = 这一遍的计时区间正常收尾(markStop 被调用过)
    int ms;                // 这一遍的墙钟(ms; 只用于报告, 不进 o.ms)
    WarmPassRec()
        : used(0), count(0), loKhz(0), medKhz(0), hiKhz(0), loRatioPermille(0),
          medRatioPermille(0), hiRatioPermille(0), firstHalfMedKhz(0), secondHalfMedKhz(0),
          cores(0), workerTidCount(0), windowClosed(0), ms(0) {}
};

struct FreqSession {
    // ---- 口径 ----
    int multi;                    // 0 = 单核口径(本线程所在核); 1 = 多核口径(可用核集合全部核)
    int cores[kMaxSampleCpus];    // 多核口径的采样核集合(升序)
    int coreCount;                // 集合大小
    unsigned long long coreMask;  // 集合位图(打印 cpulist 用)
    unsigned long long allowedMask;  // 可用核集合的位图(0 = 读不到); "越界落核"判据用
    int nominalMaxKhz;            // 单核 = 该核标称上限; 多核 = 采样核里最高的标称上限
    int fallbackCpu;              // 单核口径取不到"线程所在核"时的兜底核(会话开始那一刻的核)
    long tid;                     // 负载线程的 tid(单核口径要读它的 stat)

    // ---- 样本(口径A = 主口径: 单核项 = 负载线程所在核; 多核项 = 当刻有负载线程落上的核) ----
    int khz[kMaxSamples];         // 每次采样的频率(kHz)
    int cpuOf[kMaxSamples];       // 它采的是哪个核
    int nomOf[kMaxSamples];       // 它所在那个核的标称上限(kHz; 逐样本取, 见文件头口径)
    int count;                    // 口径A 的有效样本数
    int overflowSamples;          // 口径A 超过 kMaxSamples 被丢弃的样本数
    // ---- 样本(口径B = 对照口径: 内核允许本进程使用的全部核; 只有多核项才有) ----
    int khzB[kMaxSamples];
    int cpuB[kMaxSamples];
    int nomB[kMaxSamples];
    int countB;
    int overflowB;
    // ---- 口径A 到底覆盖了多少核(tick 级证据) ----
    int ticksA;                   // 有样本的 tick 数
    int ticksANone;               // 一个可用池线程 tid 都没有 -> 本 tick 没有口径A样本
    int workerCoreSum;            // 每 tick"有线程落上的不同核数"之和(算平均用)
    int workerCoreMax;            // 单个 tick 里最多的不同核数
    int workerTidFail;            // 读池线程 tid 的 stat 失败次数(线程已退出等)
    int outOfSetSamples;          // 样本落在可用核集合之外的核上的次数(越界落核, 硬事实)
    unsigned long long outOfSetMask;  // 这些越界核的位图
    // ---- 池线程 tid 登记表(由 gb7_parallel.h 的工作线程写入; 见 auroraFreqRegisterWorkerTid) ----
    int workerTidCount;           // 本项计时区间里登记到的 tid 数
    int workerTidOverflow;        // 表满被丢弃的登记数
    // ---- 升频/稳态预热(全部发生在计时区间之外) ----
    int warmPasses;               // 已完成的预热遍数
    int warmSteady;               // 稳态判据是否命中
    int warmSteadyHow;            // 1=已达跑满判据 2=相邻两遍中位差<=阈值 3=单遍前后半段差<=阈值
    int warmStoppedBy;            // 1=遍数上限 2=时长上限 3=本遍没有任何样本 0=未停(还在判)
    int warmTotalMs;              // 预热总墙钟(ms; 不进 o.ms)
    int warmNoSamplePasses;       // 没有任何样本的预热遍数
    WarmPassRec warm[kMaxWarmupPasses];
    // ---- 频率上限取证(scaling_max_freq 等; 见文件头说明) ----
    // 这一段要带整份 policy 枚举(逐 policy 9 条只读读数 + 口径码) + 核->policy 映射 +
    // time_in_state 历史 + 替代读数 + 逐核路径 + 判决 + 两条规则。
    // 容量沿革: 1024 -> 2048(2026-10) -> kCapTextCap(8192, 2026-10, 见上面常量处的估算过程)。
    // 2048 在真机 8.1 上实测被截断(48/48 项都截在同一个字节点, 被切掉的正是 [逐核路径]
    // 后半 + 判决): 真机真实需要 3919 字节, 8192 留 +109% 余量。
    // 它仍然小于 buildText 那一行(g_text = kCapTextCap 的 2 倍, 见 g_text 的容量说明)。
    char capText[kCapTextCap];
    int  capTextLen;

    // ---- errno 取证(逐项记; 0 = 成功) ----
    int curFreqErrno;             // 最后一次读 scaling_cur_freq 失败的 errno(口径A)
    int curFreqErrCpu;            // 它发生在哪个核(-1 = 没有失败)
    int curFreqOk;                // 读成功次数(口径A)
    int curFreqFail;              // 读失败次数(口径A)
    int curFreqErrnoB;            // 口径B 的同名字段(两种口径分开记, 不混成一个数)
    int curFreqErrCpuB;
    int curFreqOkB;
    int curFreqFailB;
    int threadStatErrno;          // 读 /proc/self/task/<tid>/stat 失败的 errno
    int threadStatOk;             // 读成功次数
    int threadStatFail;           // 读失败次数
    int govErrno;                 // 读 scaling_governor 的 errno
    char governor[32];            // 它的内容(读到时)
    int thermalErrno;             // 最后一次读热区失败的 errno
    int thermalOk;                // 读成功的热区次数
    int thermalFail;              // 读失败的热区次数
    int thermalMaxMilli;          // 采样期间的热区最高温(毫摄氏度; 0 = 没读到)
    int thermalMinMilli;

    int ticks;                    // 采样线程在本次计时区间里实际跑了多少个 tick
    int armed;                    // 计时区间是否已打标
    int stopped;                  // 计时区间是否正常收尾(markStop 被调用过)

    // ---- 逐核占用(2026-10 追加): 计时区间前后的 /proc/stat 快照(整机口径) ----
    ProcStatSnapshot statStart;
    ProcStatSnapshot statStop;
};

FreqSession g_s;

// ---------------------------------------------------------------------------
//  池线程 tid 登记表(多核口径A 的数据源: "当期哪些核上有负载线程")
// ---------------------------------------------------------------------------
//  为什么用原子而无锁: 登记发生在计时区间之内(池线程在执行任何负载工作之前登记
//  一次自己), 那里本来就有一次 sched_setaffinity(1~3 µs), 这里只多一次 fetch_add 与
//  一次 store; 而且不做任何内存分配 —— 与采样线程的"无分配"是同一条约束。
//  读的一方(采样线程)只做 acquire 读 + 逐个 relaxed 读, 不持锁。
//  为什么是"槽位 + 注销"而不是"只增的表": 负载会在一次计时区间里建很多个并行池
//  (最重的项 12 个池 x 8 线程), 只增的表会瞬间被已经退出的 tid 塞满, 于是口径A
//  读到一堆死 tid —— 那正好把"当期真正在跑的核"这件事测废。因此槽位在池线程干完活时
//  立刻注销(见 gb7_parallel.h), 表里留下的永远是活着的负载线程。
std::atomic<int> g_workerTidSlots[kMaxWorkerTids];
std::atomic<int> g_workerTidOverflow(0);   // 表满被丢弃的登记数(计数)
std::atomic<int> g_workerTidSeen(0);       // 本窗口累计登记次数(含已注销的)
std::atomic<int> g_workerTidPeak(0);       // 本窗口同时在册的最大个数(覆盖范围证据)

// 排序/取中位用的固定暂存(无分配)。只在"采样线程已停止"或"持 g_mtx"时使用。
int g_sortScratch[kMaxSamples];
int g_ratioScratch[kMaxSamples];

// 当前预热遍的墙钟起点(ms; 只在调用 gb7RunTest 的那条线程上读写)
double g_warmPassStartMs = 0.0;

// 保护 g_s / g_armed / g_stop: 采样线程与负载线程(markStart/markStop)共用。
// 裸 std::mutex: 构造不分配; 无竞争时加解锁走 futex 快路径(约 20 ns)。
std::mutex              g_mtx;
std::condition_variable g_cv;
bool                    g_stop        = false;
bool                    g_sessionOpen = false;
std::thread             g_thr;
// 每次 markStart 自增一次: 让采样线程立刻从"上一个计时区间结束后的等待"里醒来,
// 从而保证每一项的第一个样本都落在 t0 之后的一瞬间(而不是最多晚 200 ms)。
// 只在持锁时读写。
unsigned long long      g_markGen = 0;

// 汇总出来的一行文本(在 SessionEnd 里写; auroraFreqSampleText 只读它)。
// 容量说明: 这一行是中文, UTF-8 下一个汉字 3 字节 —— 完整的一句(标称/实际/样本数/采样核/
// 口径/两个数据源的 errno/governor/热区)约 400~520 字节, 因此取 1024 留足余量。
// 它是静态数组, 不在采样线程里分配任何内存, 也不进计时区间。
// 容量 2048 -> 3072(2026-10): 多核那一行新增了"逐核忙占比 < 5% 的核"与"逐核 jiffies 明细",
//   真机 9 核形状下逐核那一串约 1150 字节, 旧容量会把尾部的结论截掉 —— 见"读不到/被截断"
//   一律上报的既定约束, 这里只加容量, 内容与措辞一个字未动(短内容行为逐字节不变)。
// 容量 3072 -> 8192(2026-10): 多核那一行现在要多带 口径A/口径B 两套读数 + 预热取证 +
//   上限取证, 3072 会把尾部的结论截掉 —— 那正是"日志里少了半句还没人发现"的来源。只加容量。
// 容量 8192 -> 16384(2026-10) —— 为什么是这个数、上界怎么估的:
//   ① 这一行里"上限取证"那一段的容量刚从 2048 提到 kCapTextCap(8192), 而它在真机 8.1 上
//      实际要用 3919 字节(推导见 kCapTextCap 处的 ①~③): 也就是说这一行的长度会从
//      实测的 5384 字节涨到 5384 - 2047 + 3919 = 7256 字节; 旧的 8192 只剩 11% 余量,
//      而这一行一旦放不下就是静默截断(尾部结论消失) —— 正是要根除的那种缺陷;
//   ② 16384 >= 7256 x 1.5(= 10884) -> 相对 7256 的余量 = +126%;
//   ③ 上界估算(这一行的各部分都取各自容量的极端值): 非取证部分(口径/两套读数/预热/errno/
//      governor/热区)实测 3337 字节, 取证那一段 <= kCapTextCap = 8192, 跳过预热时的引用
//      说明 <= 90 字节 -> 合计 <= 11619 字节, 仍 <= 16384; 真放不下时显式写"尾部被截断"
//      (见 buildText 末尾的兜底块), 不静默丢尾部;
//   ④ 它是静态数组, 不在采样线程里分配任何内存, 也不进计时区间 —— 只改容量, 措辞未动。
char g_text[16384];

// ---------------------------------------------------------------------------
//  极小的文本读盘工具(无分配、无 stdio 缓冲)
// ---------------------------------------------------------------------------
// 返回 0 = 成功; >0 = 真实 errno; -1 = 打得开但内容为空; -2 = 内容不是正十进制数
int readSmallText(const char* path, char* out, int cap)
{
    if (out == nullptr || cap <= 1) {
        return -1;
    }
    out[0] = '\0';
    errno = 0;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno != 0 ? errno : -1;
    }
    errno = 0;
    const ssize_t n = ::read(fd, out, (size_t)(cap - 1));
    const int readErrno = errno;
    ::close(fd);
    if (n < 0) {
        return readErrno != 0 ? readErrno : -1;
    }
    out[n] = '\0';
    if (n == 0) {
        return -1;
    }
    return 0;
}

// 解析一个十进制整数(strtol 不分配); 返回 0 = 成功。minV 是最小可接受值。
int parseIntField(const char* s, int minV, int* out)
{
    if (s == nullptr || out == nullptr) {
        return -1;
    }
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || v < (long)minV || v > 2147483647L) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

// 读一个核的当前频率(kHz)。返回 0 = 成功, 否则是 errno / -1 / -2。
// 路径就是任务指定的那一个: /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq
int readCurFreqKhz(int cpu, int* khzOut)
{
    char path[96];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    char buf[32];
    const int e = readSmallText(path, buf, (int)sizeof(buf));
    if (e != 0) {
        return e;
    }
    int v = 0;
    if (parseIntField(buf, 1, &v) != 0) {   // 频率必须是正数
        return -2;
    }
    *khzOut = v;
    return 0;
}

// 读 /proc/self/task/<tid>/stat 的第 39 个字段 = processor(该线程此刻/最近在哪个核)。
// 这是"跑负载那个线程所在核"的唯一来源(sched_getcpu() 只能问调用线程自己, 采样
// 线程拿不到负载线程的核)。返回 >= 0 = 核号; -1 = 读不到(*errOut = errno 或 -1/-2)。
int cpuOfThread(long tid, int* errOut)
{
    if (errOut != nullptr) {
        *errOut = -1;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
    char buf[kThreadStatBuf];
    const int e = readSmallText(path, buf, (int)sizeof(buf));
    if (e != 0) {
        if (errOut != nullptr) {
            *errOut = e;
        }
        return -1;
    }
    // comm 字段可能含空格与括号 -> 必须从最后一个 ')' 之后开始数字段
    const char* p = std::strrchr(buf, ')');
    if (p == nullptr) {
        return -1;
    }
    ++p;
    int field = 3;   // ')' 之后的第一个字段是 state = 第 3 个字段
    while (field < 39) {
        while (*p == ' ') {
            ++p;
        }
        if (*p == '\0') {
            return -1;
        }
        while (*p != ' ' && *p != '\0') {
            ++p;
        }
        ++field;
    }
    while (*p == ' ') {
        ++p;
    }
    int cpu = -1;
    if (parseIntField(p, 0, &cpu) != 0 || cpu > 4096) {   // 核号可以是 0
        return -1;
    }
    if (errOut != nullptr) {
        *errOut = 0;
    }
    return cpu;
}

// ---------------------------------------------------------------------------
//  /proc/stat 逐核占用(整机口径): 回答"这段时间里每个核到底有没有活干"
// ---------------------------------------------------------------------------
//  为什么必须有它(2026-10 用户实测逼出来的): 多核项运行时频率中位只有 558MHz(Ray Tracer)/
//  1380MHz(File Compression), 标称上限 2270MHz —— 采样口径是"可用核集合的 核 x 时间 合并样本",
//  中位这么低说明绝大多数核在绝大多数采样时刻是空闲的; 界面并行度也只有 3.5~4.3 核(可用 9 核)。
//  只看频率不足以定案: 空载核停在最低频档, 但一个满载的核也可能因为同簇功耗被压到中间频档。
//  /proc/stat 的 cpuN 行给出该核累计的各类 jiffies, 用"计时区间前后两次读数之差"可以直接
//  算出这段时间里这个核有多少比例的时间在干活 —— 这是"每颗核都跑满"最直接、最可核对的证据。
//
//  口径声明(必须原样写进文本): 整机口径, 含后台线程与其他进程; 不是"本进程在这个核上的时间"。
//  两次读盘分别发生在 auroraFreqMarkStart / auroraFreqMarkStop(都在计时区间之外),
//  因此连这点开销也不进 o.ms; 固定静态缓冲, 不做任何内存分配。
int readProcStatSnapshot(ProcStatSnapshot* out)
{
    if (out == nullptr) {
        return -1;
    }
    *out = ProcStatSnapshot();
    static char buf[kProcStatBuf];   // 文件级静态缓冲: 打点路径上不分配内存
    errno = 0;
    const int fd = ::open("/proc/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        out->err = errno != 0 ? errno : -1;
        return out->err;
    }
    errno = 0;
    const ssize_t got = ::read(fd, buf, sizeof(buf) - 1);
    const int readErrno = errno;
    ::close(fd);
    if (got <= 0) {
        out->err = (got < 0) ? (readErrno != 0 ? readErrno : -1) : -1;
        return out->err;
    }
    buf[got] = '\0';
    const char* p = buf;
    while (*p != '\0') {
        if (p[0] == 'c' && p[1] == 'p' && p[2] == 'u' && p[3] >= '0' && p[3] <= '9') {
            int cpu = 0;
            const char* q = p + 3;
            while (*q >= '0' && *q <= '9') {
                cpu = cpu * 10 + (*q - '0');
                ++q;
            }
            if (cpu >= 0 && cpu < kMaxStatCpus) {
                long long v[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                for (int f = 0; f < 10; ++f) {
                    while (*q == ' ' || *q == '\t') {
                        ++q;
                    }
                    if (*q < '0' || *q > '9') {
                        break;
                    }
                    long long x = 0;
                    while (*q >= '0' && *q <= '9') {
                        x = x * 10 + (*q - '0');
                        ++q;
                    }
                    v[f] = x;
                }
                // 字段序: 1 user, 2 nice, 3 system, 4 idle, 5 iowait, 6 irq, 7 softirq, 8 steal
                out->busy[cpu] = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
                out->idle[cpu] = v[3] + v[4];
                if (cpu + 1 > out->n) {
                    out->n = cpu + 1;
                }
                out->ok = 1;
            }
        }
        while (*p != '\0' && *p != '\n') {
            ++p;
        }
        if (*p == '\n') {
            ++p;
        }
    }
    if (out->ok == 0) {
        out->err = -2;   // 打开读到了, 但没有一行 cpuN(格式异常)
        return out->err;
    }
    return 0;
}

// 参考项: 热区最高温(毫摄氏度)。只探固定的 thermal_zone0..15, 不用目录枚举(会分配)。
void readThermalLocked()
{
    for (int z = 0; z < kMaxThermalZones; ++z) {
        char path[80];
        std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        char buf[24];
        const int e = readSmallText(path, buf, (int)sizeof(buf));
        if (e != 0) {
            g_s.thermalErrno = e;
            ++g_s.thermalFail;
            continue;
        }
        int v = 0;
        bool ok = false;
        {
            errno = 0;
            char* end = nullptr;
            const long lv = std::strtol(buf, &end, 10);
            if (end != buf && lv > -100000L && lv < 200000L) {
                v = (int)lv;
                ok = true;
            }
        }
        if (!ok) {
            g_s.thermalErrno = -2;
            ++g_s.thermalFail;
            continue;
        }
        ++g_s.thermalOk;
        if (g_s.thermalOk == 1 || v > g_s.thermalMaxMilli) {
            g_s.thermalMaxMilli = v;
        }
        if (g_s.thermalOk == 1 || v < g_s.thermalMinMilli) {
            g_s.thermalMinMilli = v;
        }
    }
}

// 单调墙钟(ms; 只用于预热的时长上限, 不计分/计时)
double steadyNowMs()
{
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 取中位(不排序原数组: 拷进调用方给的固定暂存再排, 因此调用方的样本时序不受影响)
int medianOfInts(const int* v, int n, int* scratch, int cap)
{
    if (v == nullptr || scratch == nullptr || n <= 0 || cap < n) {
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        scratch[i] = v[i];
    }
    std::sort(scratch, scratch + n);
    return scratch[n / 2];
}

// 逐样本"占标称比"(千分比)的中位: 每一样本先算 1000 x 该样本频率 / 该样本所在核标称上限
// (四舍五入到千分比, 于是 1930/2150 = 897.67 -> 898 = 89.8%, 不会被截成 89.7%),
// 再取中位。为什么要逐样本算而不是"中位频率 / 某一个标称": 线程在采样期间会迁移,
// 迁移到不同频率档的核上时, 分子与分母必须是同一个核的量(见文件头口径)。
int medianRatioPermille(const int* khz, const int* nom, int n, int* scratch, int cap)
{
    if (khz == nullptr || nom == nullptr || scratch == nullptr || n <= 0 || cap < n) {
        return 0;
    }
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (nom[i] > 0 && khz[i] > 0) {
            scratch[m++] = (int)(((long long)khz[i] * 1000LL + (long long)nom[i] / 2) /
                                  (long long)nom[i]);
        }
    }
    if (m <= 0) {
        return 0;
    }
    std::sort(scratch, scratch + m);
    return scratch[m / 2];
}

// ---------------------------------------------------------------------------
//  频率上限取证(scaling_max_freq / scaling_available_frequencies ...; 只读)
// ---------------------------------------------------------------------------
//  为什么必须读它: 真机上 governor / /proc/stat / /sys/class/thermal 三处全是 EACCES, 于是
//  "实际频率只有标称的 70%"到底是系统把当前上限压住了还是cpuinfo_max_freq 虚高,
//  现有证据定不了案。scaling_max_freq 就是"内核此刻生效的频率上限" —— 它是唯一能一次定案的读数。
//  判决规则(原样写进报告): scaling_max_freq < cpuinfo_max_freq => 上限是系统压的;
//  两者相等而实际频率仍低 => 才需要继续查 governor / 热 / 功耗墙。
//  口径: 只探固定的一两个核(读数总量要小), 逐路径记 errno; 读不到就写读不到, 不用 0 冒充。
//  时机: 采样线程 join 之后 / buildText 之前, 即计时区间之外, 每个会话一次。
//
//  2026-10 追加: policy 目录枚举式取证(为什么旧的 policy%d 写法是错的)
//  ---------------------------------------------------------------------------
//  真机硬事实(手机): cpu4 的 scaling_available_frequencies =
//  558000 640000 750000 850000 1000000 1150000 1280000 1380000 1480000 1580000 1670000
//  1770000 1880000 1995000 2100000 2270000(1995 是第 14 档, 上面还有 2100 与 2270 两档),
//  cpuinfo_max_freq=2270000, 而 16 个单核项、20 次采样全部落在 1995MHz(零离散);
//  同时 cpu4/scaling_max_freq=EACCES(13)、policy0/scaling_max_freq=EACCES(13), 而
//  **policy4/scaling_max_freq=ENOENT(2)** —— 旧代码把"核号"直接当成"该核所属 policy 的编号"
//  (policy%d 传的就是 c), 于是 policy4 的 ENOENT 会被读成"这个核没有 policy 目录";
//  可它真实的含义只是"这台机器的 policy 编号不是按第一个核来的"。这两条是完全不同的结论,
//  靠猜分不开, 只能列目录。
//  所以本节改成只读的枚举式取证:
//    ① opendir/readdir 列出 /sys/devices/system/cpu/cpufreq/ 下实际存在的 policyN;
//    ② 逐 policy 读 related_cpus / affected_cpus / scaling_driver / scaling_governor /
//       scaling_max_freq / scaling_min_freq / cpuinfo_max_freq / bios_limit / scaling_cur_freq,
//       逐路径记 errno(数字与名字都写出来), 读不到就写读不到, 不用 0 冒充;
//    ③ readlink(/sys/devices/system/cpu/cpuN/cpufreq) 把"核号 -> policy 号"钉死(这是内核
//       自己给的映射), 再与 related_cpus / affected_cpus 的位图交叉核对;
//    ④ time_in_state(逐档历史累计时间)佐证"顶档到底有没有生效过": 若 2100000/2270000 两档
//       累计时间为 0, 那就是"这两档在本机从未被请求过"的硬证据。它不能替代
//       scaling_max_freq(累计 0 也可能只是刚刚才被压住), 但足以把"cpuinfo_max_freq 是标称值、
//       当前生效上限更低"这一条定下来。
//  本节只读: 只有 opendir / readdir / open(O_RDONLY) / read / close / readlink 六种调用。
//  不写 scaling_max_freq、不锁频、不改 governor —— 那会改变被测对象。
//  代价控制: 目录布局是静态事实, 所以目录列举结果进程内只取一次(见 g_capDirListed 那组
//  静态变量, 仍然全部发生在计时区间之外); 逐档读数每个会话重读一次(热/功耗墙是会变的,
//  缓存读数会把变化藏起来)。采样线程一个字都没动 —— 它仍然不碰 opendir/readdir。
//  ===========================================================================

// errno 的短名(数字): 数字与名字都给, 人眼与脚本两边都能读。
// 口径与 readSmallText 一致: 0 = 读到; >0 = 真实 errno; -1 = 内容为空; -2 = 内容格式非法。
void capErrWord(char* dst, int cap, int e)
{
    if (dst == nullptr || cap <= 0) {
        return;
    }
    const char* n = nullptr;
    switch (e) {
        case 1:  n = "EPERM";  break;
        case 2:  n = "ENOENT"; break;
        case 5:  n = "EIO";    break;
        case 13: n = "EACCES"; break;
        case 21: n = "EISDIR"; break;
        case 22: n = "EINVAL"; break;
        default: n = nullptr;  break;
    }
    if (e == 0) {
        std::snprintf(dst, (size_t)cap, "读到");
    } else if (e == -1) {
        std::snprintf(dst, (size_t)cap, "空(-1)");
    } else if (e == -2) {
        std::snprintf(dst, (size_t)cap, "非法(-2)");
    } else if (n != nullptr) {
        std::snprintf(dst, (size_t)cap, "%s(%d)", n, e);
    } else {
        std::snprintf(dst, (size_t)cap, "errno=%d", e);
    }
}

// 一个 policy 目录的只读读数(逐节点一个 errno 口径码; 0 = 读到)。
struct CapPolicyRec {
    int index;                        // policy 编号(取自目录名)
    int relErr;                       // related_cpus
    unsigned long long relMask;       // 它解析出的核位图
    int affErr;                       // affected_cpus
    unsigned long long affMask;
    int drvErr;  char driver[24];     // scaling_driver(文本)
    int govErr;  char governor[24];   // scaling_governor(文本)
    int maxErr;      int maxKhz;      // scaling_max_freq(kHz)
    int minErr;      int minKhz;      // scaling_min_freq
    int cpuinfoErr;  int cpuinfoKhz;  // cpuinfo_max_freq
    int biosErr;     int biosKhz;     // bios_limit(有的内核用它表示固件限频)
    int curErr;      int curKhz;      // scaling_cur_freq(旁证; 不是上限)
    int tisQuery;                     // 本会话有没有查过 time_in_state
    int tisErr;                       // policyN/time_in_state
    int tisStatsErr;                  // policyN/stats/time_in_state
    int tisSrc;                       // 1 = policyN/time_in_state; 2 = policyN/stats/time_in_state
    int tisSteps;                     // 解析出的档位数
    int tisBadLines;                  // 格式非法被丢掉的档位数(计数)
    int tisTopKhz[2];                 // 最高的两档(kHz)
    long long tisTopTime[2];          // 它们各自的历史累计时间
    long long tisTotal;               // 全部档位累计时间之和
    CapPolicyRec()
        : index(-1), relErr(0), relMask(0ull), affErr(0), affMask(0ull),
          drvErr(0), driver(), govErr(0), governor(),
          maxErr(0), maxKhz(0), minErr(0), minKhz(0), cpuinfoErr(0), cpuinfoKhz(0),
          biosErr(0), biosKhz(0), curErr(0), curKhz(0),
          tisQuery(0), tisErr(0), tisStatsErr(0), tisSrc(0), tisSteps(0), tisBadLines(0),
          tisTopKhz(), tisTopTime(), tisTotal(0)
    {
    }
};

// ---------------------------------------------------------------------------
//  纯逻辑(没有任何 I/O): 离线断言脚本按同名函数逐条核对下面这几条规则
// ---------------------------------------------------------------------------
// 解析内核 cpulist("0-3,8-11" / "4" / "4,5,7") -> 核位图(只覆盖前 64 个核)。
// 返回 0 = 成功; -1 = 空 / 格式非法(缺数字 / 连续逗号 / 尾随逗号 / 区间反了 / 混入别的字符);
// -2 = 出现 >= 64 的核号(超出本工程的位图口径 —— 判非法, 不静默截断成别的集合)。
int parseCpuListMaskText(const char* s, unsigned long long* maskOut)
{
    if (maskOut == nullptr) {
        return -1;
    }
    *maskOut = 0ull;
    if (s == nullptr) {
        return -1;
    }
    const char* p = s;
    for (;;) {
        if (*p == ' ' || *p == '\t') { ++p; continue; }
        break;
    }
    if (*p == '\0') {
        return -1;   // 空串 / 全是空白
    }
    int fields = 0;
    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return -1;   // 非数字开头(前导逗号 / 连续逗号 / "cpu0-3" / "abc" 都走这里)
        }
        long lo = 0;
        while (*p >= '0' && *p <= '9') {
            lo = lo * 10 + (*p - '0');
            if (lo > 4096) { return -2; }
            ++p;
        }
        long hi = lo;
        if (*p == '-') {
            ++p;
            if (*p < '0' || *p > '9') { return -1; }   // "0-" 这种半截区间
            hi = 0;
            while (*p >= '0' && *p <= '9') {
                hi = hi * 10 + (*p - '0');
                if (hi > 4096) { return -2; }
                ++p;
            }
            if (hi < lo) { return -1; }                // 区间反了(如 "3-0")
        }
        if (hi >= 64) {
            return -2;   // 核号超出 64: 不猜、不截断
        }
        for (long c = lo; c <= hi; ++c) {
            *maskOut |= (1ull << (unsigned)c);
        }
        ++fields;
        for (;;) {
            if (*p == ' ' || *p == '\t') { ++p; continue; }
            break;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == ',') {
            ++p;
            for (;;) {
                if (*p == ' ' || *p == '\t') { ++p; continue; }
                break;
            }
            if (*p == '\0') { return -1; }            // 尾随逗号
            continue;
        }
        if (*p == '\n' || *p == '\r') {              // sysfs 的行尾
            ++p;
            for (;;) {
                if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { ++p; continue; }
                break;
            }
            if (*p == '\0') { break; }
            return -1;   // 换行之后还有内容 -> 这不是一行 cpulist
        }
        return -1;       // 其它字符(如 "0-3;5")
    }
    return (fields > 0) ? 0 : -1;
}

// 从 readlink 的目标("policy2" / "../cpufreq/policy2" / "/sys/.../cpufreq/policy12")里取
// 最后一个 "policy<十进制>" 的编号。返回 >= 0 的编号; -1 = 认不出(它不是一个 policy 链接)。
int parsePolicyIndexFromLink(const char* target)
{
    if (target == nullptr) {
        return -1;
    }
    const size_t n = std::strlen(target);
    size_t at = (size_t)-1;
    for (size_t i = 0; i + 6 <= n; ++i) {
        if (std::strncmp(target + i, "policy", 6) == 0) {
            at = i;   // 取最后一个(最深的那个目录)
        }
    }
    if (at == (size_t)-1) {
        return -1;
    }
    const char* p = target + at + 6;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 4096) { return -1; }
        ++p;
    }
    if (*p != '\0' && *p != '/') {
        return -1;
    }
    return (int)v;
}

// 解析 time_in_state 的一行 "<频率kHz> <累计时间>"。
// 返回 0 = 成功; -1 = 空行 / 缺字段 / 多出第三个字段(本工程不认识的行一律判非法)。
int parseTimeInStateLine(const char* line, int* khzOut, long long* timeOut)
{
    if (line == nullptr) {
        return -1;
    }
    const char* p = line;
    for (;;) {
        if (*p == ' ' || *p == '\t') { ++p; continue; }
        break;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    long long f = 0;
    while (*p >= '0' && *p <= '9') {
        f = f * 10 + (*p - '0');
        if (f > 100000000LL) { return -1; }   // 频率不可能这么大 -> 这一行不是 time_in_state
        ++p;
    }
    for (;;) {
        if (*p == ' ' || *p == '\t') { ++p; continue; }
        break;
    }
    if (*p < '0' || *p > '9') {
        return -1;                            // 缺第二个字段
    }
    long long t = 0;
    while (*p >= '0' && *p <= '9') {
        t = t * 10 + (*p - '0');
        if (t > 100000000000000LL) { return -1; }
        ++p;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { ++p; }
    if (*p != '\0') {
        return -1;                            // 还有第三个字段
    }
    if (khzOut != nullptr) { *khzOut = (int)f; }
    if (timeOut != nullptr) { *timeOut = t; }
    return 0;
}

// 在已读到的 policy 记录里找 cpu 落在哪一条: 先命中 related_cpus, 再命中 affected_cpus
// (内核把"这个 policy 管哪些核"写在这两个节点里)。返回下标; -1 = 未命中。
// viaOut(可空): 1 = 命中 related_cpus; 2 = 命中 affected_cpus。
int findPolicyIndexForCpu(const CapPolicyRec* ps, int n, int cpu, int* viaOut)
{
    if (viaOut != nullptr) {
        *viaOut = 0;
    }
    if (ps == nullptr || cpu < 0 || cpu >= 64) {
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        if (ps[i].relErr == 0 && ((ps[i].relMask >> (unsigned)cpu) & 1ull) != 0ull) {
            if (viaOut != nullptr) { *viaOut = 1; }
            return i;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (ps[i].affErr == 0 && ((ps[i].affMask >> (unsigned)cpu) & 1ull) != 0ull) {
            if (viaOut != nullptr) { *viaOut = 2; }
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
//  目录枚举(只读; 目录布局是静态事实 -> 进程内只列一次; 逐档读数仍然每会话重读)
// ---------------------------------------------------------------------------
int  g_capPolIdx[kCapMaxPolicies];   // 实际存在的 policy 编号(升序)
int  g_capPolCount  = 0;
int  g_capDirErr    = 0;             // opendir 的 errno(0 = 成功)
int  g_capDirEntries = 0;            // readdir 到的条目总数
char g_capDirOther[48] = {0};        // 第一个"不是 policyN"的条目名(便于判它是什么组织方式)
int  g_capDirListed = 0;

int  g_capCoolIdx[kCapMaxCooling];   // 实际存在的 cooling_device 编号(升序)
int  g_capCoolCount = 0;
int  g_capCoolDirErr = 0;
int  g_capCoolListed = 0;

// 把 "<前缀><十进制>" 的目录名解析成编号(名字后面必须什么都没有)。返回 >= 0 的编号, -1 = 不是。
int parseIndexedDirName(const char* name, const char* prefix)
{
    if (name == nullptr || prefix == nullptr) {
        return -1;
    }
    const size_t pl = std::strlen(prefix);
    if (std::strncmp(name, prefix, pl) != 0) {
        return -1;
    }
    const char* p = name + pl;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 4096) { return -1; }
        ++p;
    }
    if (*p != '\0') {
        return -1;
    }
    return (int)v;
}

void capInsertionSortIndexes(int* v, int n)
{
    for (int i = 1; i < n; ++i) {
        const int x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; --j; }
        v[j + 1] = x;
    }
}

// 列一次 /sys/devices/system/cpu/cpufreq/ 下真实存在的 policyN。
void capDirListOnce(void)
{
    if (g_capDirListed != 0) {
        return;
    }
    g_capDirListed = 1;
    g_capPolCount = 0;
    g_capDirErr = 0;
    g_capDirEntries = 0;
    g_capDirOther[0] = '\0';
    errno = 0;
    DIR* d = ::opendir(kCapFreqRoot);
    if (d == nullptr) {
        g_capDirErr = (errno != 0) ? errno : -1;
        return;
    }
    for (;;) {
        struct dirent* de = ::readdir(d);
        if (de == nullptr) {
            break;
        }
        const char* nm = de->d_name;
        ++g_capDirEntries;
        const int v = parseIndexedDirName(nm, "policy");
        if (v >= 0) {
            if (g_capPolCount < kCapMaxPolicies) {
                g_capPolIdx[g_capPolCount++] = v;
            }
        } else if (nm[0] != '.' && g_capDirOther[0] == '\0') {
            std::snprintf(g_capDirOther, sizeof(g_capDirOther), "%s", nm);
        }
    }
    ::closedir(d);
    capInsertionSortIndexes(g_capPolIdx, g_capPolCount);
}

// 列一次 /sys/class/thermal 下真实存在的 cooling_deviceN(用于"替代读数": 热限频的当前档位)。
void capCoolListOnce(void)
{
    if (g_capCoolListed != 0) {
        return;
    }
    g_capCoolListed = 1;
    g_capCoolCount = 0;
    g_capCoolDirErr = 0;
    errno = 0;
    DIR* d = ::opendir(kCapThermalRoot);
    if (d == nullptr) {
        g_capCoolDirErr = (errno != 0) ? errno : -1;
        return;
    }
    for (;;) {
        struct dirent* de = ::readdir(d);
        if (de == nullptr) {
            break;
        }
        const int v = parseIndexedDirName(de->d_name, "cooling_device");
        if (v >= 0 && g_capCoolCount < kCapMaxCooling) {
            g_capCoolIdx[g_capCoolCount++] = v;
        }
    }
    ::closedir(d);
    capInsertionSortIndexes(g_capCoolIdx, g_capCoolCount);
}

// 把 src 的第一行(去掉 \n / \r)拷进 dst。
void capStripEol(const char* src, char* dst, int cap)
{
    if (dst == nullptr || cap <= 0) {
        return;
    }
    int i = 0;
    if (src != nullptr) {
        while (src[i] != '\0' && src[i] != '\n' && src[i] != '\r' && i < cap - 1) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

// 读 "<root>/policyN/<node>" 并解析成十进制(kHz)。
// 返回 0 = 读到并解析成功; >0 = errno; -1 = 内容为空; -2 = 内容不是十进制数。
int capReadPolicyNodeKhz(int idx, const char* node, int* khzOut)
{
    char path[128];
    char buf[kCapFreqBuf];
    std::snprintf(path, sizeof(path), "%s/policy%d/%s", kCapFreqRoot, idx, node);
    const int e = readSmallText(path, buf, (int)sizeof(buf));
    if (e != 0) {
        return e;
    }
    int v = 0;
    if (parseIntField(buf, 1, &v) != 0) {
        return -2;
    }
    *khzOut = v;
    return 0;
}

// 读一个 policy 目录的全部只读节点(逐节点记 errno)。
void capLoadPolicy(int idx, CapPolicyRec* r)
{
    *r = CapPolicyRec();
    r->index = idx;
    char path[128];
    char buf[kCapFreqBuf];
    std::snprintf(path, sizeof(path), "%s/policy%d/related_cpus", kCapFreqRoot, idx);
    r->relErr = readSmallText(path, buf, (int)sizeof(buf));
    if (r->relErr == 0) {
        const int pe = parseCpuListMaskText(buf, &r->relMask);
        if (pe != 0) {
            r->relErr = pe;
            r->relMask = 0ull;
        }
    }
    std::snprintf(path, sizeof(path), "%s/policy%d/affected_cpus", kCapFreqRoot, idx);
    r->affErr = readSmallText(path, buf, (int)sizeof(buf));
    if (r->affErr == 0) {
        const int pe = parseCpuListMaskText(buf, &r->affMask);
        if (pe != 0) {
            r->affErr = pe;
            r->affMask = 0ull;
        }
    }
    std::snprintf(path, sizeof(path), "%s/policy%d/scaling_driver", kCapFreqRoot, idx);
    r->drvErr = readSmallText(path, buf, (int)sizeof(buf));
    if (r->drvErr == 0) {
        capStripEol(buf, r->driver, (int)sizeof(r->driver));
    }
    std::snprintf(path, sizeof(path), "%s/policy%d/scaling_governor", kCapFreqRoot, idx);
    r->govErr = readSmallText(path, buf, (int)sizeof(buf));
    if (r->govErr == 0) {
        capStripEol(buf, r->governor, (int)sizeof(r->governor));
    }
    r->maxErr     = capReadPolicyNodeKhz(idx, "scaling_max_freq", &r->maxKhz);
    r->minErr     = capReadPolicyNodeKhz(idx, "scaling_min_freq", &r->minKhz);
    r->cpuinfoErr = capReadPolicyNodeKhz(idx, "cpuinfo_max_freq", &r->cpuinfoKhz);
    r->biosErr    = capReadPolicyNodeKhz(idx, "bios_limit", &r->biosKhz);
    r->curErr     = capReadPolicyNodeKhz(idx, "scaling_cur_freq", &r->curKhz);
}

// 读一个 policy 的 time_in_state(逐档历史累计时间)。旧内核在 policyN/time_in_state,
// 新内核在 policyN/stats/time_in_state —— 两条路径都试, 逐路径记 errno。
void capLoadTimeInState(int idx, CapPolicyRec* r)
{
    r->tisQuery = 1;
    char path[160];
    char buf[kCapTisBuf];
    std::snprintf(path, sizeof(path), "%s/policy%d/time_in_state", kCapFreqRoot, idx);
    r->tisErr = readSmallText(path, buf, (int)sizeof(buf));
    if (r->tisErr != 0) {
        std::snprintf(path, sizeof(path), "%s/policy%d/stats/time_in_state", kCapFreqRoot, idx);
        r->tisStatsErr = readSmallText(path, buf, (int)sizeof(buf));
        if (r->tisStatsErr != 0) {
            return;   // 两处都读不到: 留着 errno, 上层写"这一路也定不了案"
        }
        r->tisSrc = 2;
    } else {
        r->tisSrc = 1;
    }
    char* p = buf;
    while (*p != '\0') {
        char* eol = p;
        while (*eol != '\0' && *eol != '\n') {
            ++eol;
        }
        const char saved = *eol;
        *eol = '\0';
        bool blank = true;
        for (const char* q = p; *q != '\0'; ++q) {
            if (*q != ' ' && *q != '\t' && *q != '\r') { blank = false; break; }
        }
        if (!blank) {
            int khz = 0;
            long long t = 0;
            if (parseTimeInStateLine(p, &khz, &t) == 0) {
                ++r->tisSteps;
                r->tisTotal += t;
                if (khz > r->tisTopKhz[0]) {
                    r->tisTopKhz[1] = r->tisTopKhz[0];
                    r->tisTopTime[1] = r->tisTopTime[0];
                    r->tisTopKhz[0] = khz;
                    r->tisTopTime[0] = t;
                } else if (khz > r->tisTopKhz[1]) {
                    r->tisTopKhz[1] = khz;
                    r->tisTopTime[1] = t;
                }
            } else {
                ++r->tisBadLines;   // 计数, 不丢
            }
        }
        if (saved == '\0') {
            break;
        }
        p = eol + 1;
    }
}

int capFindPolicyRec(const CapPolicyRec* ps, int n, int idx)
{
    for (int i = 0; i < n; ++i) {
        if (ps[i].index == idx) {
            return i;
        }
    }
    return -1;
}

// 把"核号 -> policy 号"钉死: ① readlink(cpuN/cpufreq)(内核自己给的映射);
// ② 退到枚举到的 policy 的 related_cpus / affected_cpus 位图命中。
// 返回 policy 编号(>= 0)或 -1(定不了)。
// howOut: 1 = readlink; 2 = related_cpus; 3 = affected_cpus; 0 = 未定。
// linkErrOut: readlink 的结果(0 = 读到; >0 = errno; -1 = 空; -2 = 读到了但认不出是 policyN)。
int capResolvePolicyOfCore(int cpu, const CapPolicyRec* ps, int n, int* howOut,
                           int* linkErrOut, char* linkOut, int linkCap)
{
    if (howOut != nullptr) { *howOut = 0; }
    if (linkErrOut != nullptr) { *linkErrOut = 0; }
    if (linkOut != nullptr && linkCap > 0) { linkOut[0] = '\0'; }
    char path[96];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq", cpu);
    char tgt[160];
    errno = 0;
    const ssize_t got = ::readlink(path, tgt, sizeof(tgt) - 1);
    int linkErr = 0;
    if (got > 0) {
        tgt[got] = '\0';
        if (linkOut != nullptr && linkCap > 0) {
            std::snprintf(linkOut, (size_t)linkCap, "%s", tgt);
        }
        const int idx = parsePolicyIndexFromLink(tgt);
        if (idx >= 0) {
            if (howOut != nullptr) { *howOut = 1; }
            return idx;
        }
        linkErr = -2;   // 读到了, 但目标不是一个 policyN 链接(例如它是个真目录)
    } else {
        linkErr = (errno != 0) ? errno : -1;
    }
    if (linkErrOut != nullptr) { *linkErrOut = linkErr; }
    int via = 0;
    const int ri = findPolicyIndexForCpu(ps, n, cpu, &via);
    if (ri >= 0) {
        if (howOut != nullptr) { *howOut = (via == 1) ? 2 : 3; }
        return ps[ri].index;
    }
    return -1;
}

// ---------------------------------------------------------------------------
//  文本拼装(把上面读到的每一个数都写出来; 读不到的写原因, 不用 0 冒充)
// ---------------------------------------------------------------------------
void capAppendKhz(std::string& out, int err, int khz)
{
    if (err == 0 && khz > 0) {
        char one[24];
        std::snprintf(one, sizeof(one), "%d", khz);
        out += one;
        return;
    }
    char w[24];
    capErrWord(w, (int)sizeof(w), (err == 0) ? -2 : err);
    out += w;
}

void capAppendText(std::string& out, int err, const char* s)
{
    if (err == 0 && s != nullptr && s[0] != '\0') {
        out += s;
        return;
    }
    char w[24];
    capErrWord(w, (int)sizeof(w), (err == 0) ? -1 : err);
    out += w;
}

void capAppendCpuList(std::string& out, int err, unsigned long long mask)
{
    if (err != 0) {
        char w[24];
        capErrWord(w, (int)sizeof(w), err);
        out += w;
        return;
    }
    char l[64];
    l[0] = '\0';
    aurora_cpu_detail::maskToCpuList(mask, 64, l, (int)sizeof(l));
    out += (l[0] != '\0') ? l : "空集";
}

// 一条 policy 的紧凑读数(哪个 policy 管哪些核 + 9 个只读节点的值与 errno)。
void capAppendPolicySummary(std::string& out, const CapPolicyRec& r)
{
    char one[64];
    std::snprintf(one, sizeof(one), "policy%d{related=", r.index);
    out += one;
    capAppendCpuList(out, r.relErr, r.relMask);
    out += " affected=";
    capAppendCpuList(out, r.affErr, r.affMask);
    out += " max=";        capAppendKhz(out, r.maxErr, r.maxKhz);
    out += " min=";        capAppendKhz(out, r.minErr, r.minKhz);
    out += " cpuinfo=";    capAppendKhz(out, r.cpuinfoErr, r.cpuinfoKhz);
    out += " bios_limit="; capAppendKhz(out, r.biosErr, r.biosKhz);
    out += " cur=";        capAppendKhz(out, r.curErr, r.curKhz);
    out += " drv=";        capAppendText(out, r.drvErr, r.driver);
    out += " gov=";        capAppendText(out, r.govErr, r.governor);
    out += "} ";
}

// time_in_state 那一段: 顶档到底有没有生效过。
void capAppendTisSummary(std::string& out, const CapPolicyRec& r, int cpu)
{
    char one[384];
    std::snprintf(one, sizeof(one), "cpu%d(属 policy%d): ", cpu, r.index);
    out += one;
    if (r.tisSteps <= 0) {
        char w1[24];
        char w2[24];
        capErrWord(w1, (int)sizeof(w1), (r.tisErr == 0) ? -1 : r.tisErr);
        capErrWord(w2, (int)sizeof(w2), (r.tisStatsErr == 0) ? -1 : r.tisStatsErr);
        std::snprintf(one, sizeof(one),
                      "time_in_state 也没读到(policy%d/time_in_state=%s; "
                      "policy%d/stats/time_in_state=%s) => 这一路也定不了案; ",
                      r.index, w1, r.index, w2);
        out += one;
        return;
    }
    std::snprintf(one, sizeof(one),
                  "time_in_state 读到 %d 档(来源 = policy%d/%s; 累计总时间 %lld; 其中格式非法 "
                  "被丢掉的档位 %d 个 —— 计数): ",
                  r.tisSteps, r.index,
                  (r.tisSrc == 2) ? "stats/time_in_state" : "time_in_state",
                  r.tisTotal, r.tisBadLines);
    out += one;
    if (r.tisTopKhz[1] > 0) {
        std::snprintf(one, sizeof(one), "最高两档 %d=%lld / %d=%lld",
                      r.tisTopKhz[0], r.tisTopTime[0], r.tisTopKhz[1], r.tisTopTime[1]);
    } else {
        std::snprintf(one, sizeof(one), "最高档 %d=%lld", r.tisTopKhz[0], r.tisTopTime[0]);
    }
    out += one;
    if (r.tisTopTime[0] == 0 && r.tisTopTime[1] == 0) {
        out += " -> 这两档在本机累计运行时间为 0 = 从未被请求过(硬证据: 与"
               "「cpuinfo_max_freq 是标称值、当前生效上限更低」一致); ";
    } else {
        out += " -> 顶档用过(不是不可达): 本项看到的低频率更可能是当时的"
               "负载/调度/热或功耗策略, 不是硬上限; ";
    }
}

// 世界可读的替代读数(用户点名要查的那几条; 逐路径记 errno)。
void capAppendAlternatives(std::string& out)
{
    char one[320];
    char w[24];
    // ---- /proc/cpufreq(极少数内核提供的一行只读快照) ----
    {
        char buf[kCapFreqBuf];
        const int e = readSmallText("/proc/cpufreq", buf, (int)sizeof(buf));
        out += "/proc/cpufreq=";
        if (e == 0) {
            char v[128];
            capStripEol(buf, v, (int)sizeof(v));
            out += (v[0] != '\0') ? v : "空行";
        } else {
            capErrWord(w, (int)sizeof(w), e);
            out += w;
        }
        out += " · ";
    }
    // ---- cpufreq/boost(有的内核用它表示"允许不允许 boost") ----
    {
        char buf[kCapFreqBuf];
        char boostPath[128];
        std::snprintf(boostPath, sizeof(boostPath), "%s/boost", kCapFreqRoot);
        const int e = readSmallText(boostPath, buf, (int)sizeof(buf));
        out += "cpufreq/boost=";
        if (e == 0) {
            char v[64];
            capStripEol(buf, v, (int)sizeof(v));
            out += (v[0] != '\0') ? v : "空行";
        } else {
            capErrWord(w, (int)sizeof(w), e);
            out += w;
        }
        out += " · ";
    }
    // ---- 冷却设备(thermal cooling_device*): cpu 域的 cur_state != 0 = 热限频正在起作用 ----
    capCoolListOnce();
    if (g_capCoolDirErr != 0) {
        capErrWord(w, (int)sizeof(w), g_capCoolDirErr);
        std::snprintf(one, sizeof(one), "冷却设备: opendir(%s) 读不到(%s)", kCapThermalRoot, w);
        out += one;
        return;
    }
    std::snprintf(one, sizeof(one), "冷却设备 %d 个, 其中名字里带 cpu 的(逐档 cur_state): ",
                  g_capCoolCount);
    out += one;
    int cpuRelated = 0;
    int shown = 0;
    for (int i = 0; i < g_capCoolCount; ++i) {
        char path[128];
        char tbuf[64];
        std::snprintf(path, sizeof(path), "%s/cooling_device%d/type",
                      kCapThermalRoot, g_capCoolIdx[i]);
        const int te = readSmallText(path, tbuf, (int)sizeof(tbuf));
        if (te != 0) {
            continue;   // 读不到 type 的单独一个冷却设备: 不猜它是不是 cpu 相关, 跳过
        }
        char t[64];
        capStripEol(tbuf, t, (int)sizeof(t));
        if (std::strstr(t, "cpu") == nullptr) {
            continue;
        }
        ++cpuRelated;
        if (shown >= 6) {
            continue;
        }
        ++shown;
        char cbuf[32];
        std::snprintf(path, sizeof(path), "%s/cooling_device%d/cur_state",
                      kCapThermalRoot, g_capCoolIdx[i]);
        const int ce = readSmallText(path, cbuf, (int)sizeof(cbuf));
        char c[32];
        if (ce == 0) {
            capStripEol(cbuf, c, (int)sizeof(c));
        } else {
            capErrWord(c, (int)sizeof(c), ce);
        }
        std::snprintf(one, sizeof(one), "%scooling_device%d(%s)/cur_state=%s",
                      (shown > 1) ? ", " : "", g_capCoolIdx[i], t, c);
        out += one;
    }
    if (cpuRelated == 0) {
        out += "0 个(thermal 里没有名字带 cpu 的冷却设备)";
    } else if (shown < cpuRelated) {
        std::snprintf(one, sizeof(one), " …(共 %d 个, 只列前 %d 个)", cpuRelated, shown);
        out += one;
    }
}

void readFreqCapEvidence()
{
    g_s.capText[0] = '\0';
    g_s.capTextLen = 0;
    int probe[kCapProbeCores];
    int nProbe = 0;
    probe[nProbe++] = 0;   // cpu0 永远探(两台机器都有它, 便于对照)
    for (int i = 0; i < g_s.count && nProbe < kCapProbeCores; ++i) {
        const int c = g_s.cpuOf[i];
        if (c > 0 && c < 64 && c != probe[0]) {
            probe[nProbe++] = c;
            break;
        }
    }
    const char* const files[4] = {"scaling_max_freq", "scaling_min_freq",
                                  "cpuinfo_max_freq", "scaling_available_frequencies"};
    std::string out = "频率上限取证(scaling_max_freq 等; 计时区间之外读一次, 只读不写) = ";
    // 容量 512 -> 2048(2026-10) 单条小行的缓冲 —— 这一环原来在静默截断:
    //   中文一字 3 字节, 而"上限读数读不到"那一支判决的完整字面量就是 618 字节, 代入真机取值
    //   (cpu0/cpu10 + policy0/policy1 + EACCES(13))后是 621 / 622 字节 —— 都超过 511。
    //   于是 snprintf 把每一句判决的尾部(最后 110 字节: "……③ 在更宽的权限域下读同一个
    //   scaling_max_freq(例如以 shell 用户读这条只读路径 —— 仍然不写、不锁频、不改 governor);")
    //   悄悄切掉, 而且没有任何标记 —— 这是与 capText 被截断同类的缺陷, 只是它被 capText 的
    //   截断挡住了、在真机日志里从来没露过面。
    //   2048 >= 622 x 1.5(= 933) -> 相对最长片段 622 的余量 = +229%;
    //   同函数里其它片段的字面量都 <= 300 字节, 因此这一处就是全函数的紧约束。
    char one[2048];

    // =======================================================================
    // ① policy 目录枚举(只读) —— "哪个 policy 管哪些核"的唯一权威来源
    // =======================================================================
    capDirListOnce();
    CapPolicyRec recs[kCapMaxPolicies];
    int nRec = 0;
    for (int i = 0; i < g_capPolCount && i < kCapMaxPolicies; ++i) {
        capLoadPolicy(g_capPolIdx[i], &recs[nRec]);
        ++nRec;
    }
    // 探针核的判决素材: 所属 policy 编号 + 它是怎么定出来的 + 核路径自己的两条读数
    int polOfProbe[kCapProbeCores];
    int howOfProbe[kCapProbeCores];
    int linkErrOfProbe[kCapProbeCores];
    int cpuMaxErr[kCapProbeCores];
    int cpuMaxKhz[kCapProbeCores];
    int cpuInfoErr[kCapProbeCores];
    int cpuInfoKhz[kCapProbeCores];
    char linkTgt[kCapProbeCores][160];
    for (int p = 0; p < nProbe; ++p) {
        cpuMaxErr[p] = 0;
        cpuMaxKhz[p] = 0;
        cpuInfoErr[p] = 0;
        cpuInfoKhz[p] = 0;
        linkTgt[p][0] = '\0';
        polOfProbe[p] = capResolvePolicyOfCore(probe[p], recs, nRec, &howOfProbe[p],
                                               &linkErrOfProbe[p], linkTgt[p],
                                               (int)sizeof(linkTgt[p]));
    }

    // ---- ①-a 枚举结果本身(目录读不到 / 目录里一个 policyN 都没有 / 有几个 policyN) ----
    if (g_capDirErr != 0) {
        char et[48];
        errnoText(et, (int)sizeof(et), g_capDirErr);
        std::snprintf(one, sizeof(one),
                      "[policy 枚举] opendir(%s) 读不到(%s) —— 列不出 policy 目录, policy 编号"
                      "只能靠 readlink 兜底; ", kCapFreqRoot, et);
        out += one;
    } else if (nRec == 0) {
        std::snprintf(one, sizeof(one),
                      "[policy 枚举] opendir(%s) 成功, readdir 到 %d 个条目, 其中**一个 policyN "
                      "目录都没有**(第一个非 policy 条目 = %s) —— 本机 cpufreq 不是按 policyN "
                      "组织的; ",
                      kCapFreqRoot, g_capDirEntries,
                      (g_capDirOther[0] != '\0') ? g_capDirOther : "无");
        out += one;
    } else {
        std::snprintf(one, sizeof(one),
                      "[policy 枚举] opendir(%s) 成功, readdir 到 %d 个条目, 实际存在的 policy 目录 "
                      "%d 个: ", kCapFreqRoot, g_capDirEntries, nRec);
        out += one;
        for (int i = 0; i < nRec; ++i) {
            std::snprintf(one, sizeof(one), "policy%d ", recs[i].index);
            out += one;
        }
    }
    // ---- ①-b 逐 policy 的 9 条只读读数(每个数都写出来; 读不到的写 errno) ----
    for (int i = 0; i < nRec; ++i) {
        capAppendPolicySummary(out, recs[i]);
    }

    // ---- ② 核 -> policy 映射(readlink 定编号, 再与 related_cpus 位图交叉核对) ----
    out += "[核->policy 映射] ";
    for (int p = 0; p < nProbe; ++p) {
        if (polOfProbe[p] >= 0 && howOfProbe[p] == 1) {
            std::snprintf(one, sizeof(one), "cpu%d/cpufreq --readlink--> %s (= policy%d); ",
                          probe[p], linkTgt[p], polOfProbe[p]);
        } else if (polOfProbe[p] >= 0) {
            char et[48];
            errnoText(et, (int)sizeof(et), linkErrOfProbe[p]);
            std::snprintf(one, sizeof(one),
                          "cpu%d 属 policy%d(按 %s 位图命中; readlink 没给出编号: %s); ",
                          probe[p], polOfProbe[p],
                          (howOfProbe[p] == 2) ? "related_cpus" : "affected_cpus", et);
        } else {
            char et[48];
            errnoText(et, (int)sizeof(et), linkErrOfProbe[p]);
            std::snprintf(one, sizeof(one),
                          "cpu%d 定不了所属 policy(readlink %s; 枚举到的 %d 个 policy 的 "
                          "related_cpus/affected_cpus 位图里都没有它) —— 这时不能退回去拿核号"
                          "当 policy 号用; ", probe[p], et, nRec);
        }
        out += one;
    }

    // ---- ③ 顶档到底有没有生效过: time_in_state(逐档历史累计时间; 只读) ----
    out += "[顶档历史] ";
    for (int p = 0; p < nProbe; ++p) {
        const int ri = capFindPolicyRec(recs, nRec, polOfProbe[p]);
        if (ri < 0) {
            std::snprintf(one, sizeof(one), "cpu%d 的 policy 未定, 这一条跳过; ", probe[p]);
            out += one;
            continue;
        }
        if (recs[ri].tisQuery == 0) {
            capLoadTimeInState(recs[ri].index, &recs[ri]);
        }
        capAppendTisSummary(out, recs[ri], probe[p]);
    }

    // ---- ④ 世界可读的替代读数(/proc/cpufreq / cpufreq/boost / 冷却设备 cur_state) ----
    out += "[替代读数] ";
    capAppendAlternatives(out);

    // ---- ⑤ 每条 cpuN 路径(原有行为一字不改: 四条路径逐条记 errno) ----
    out += " [逐核路径] ";
    for (int p = 0; p < nProbe; ++p) {
        const int c = probe[p];
        for (int f = 0; f < 4; ++f) {
            char path[128];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/%s", c, files[f]);
            char buf[kCapFreqBuf];
            const int e = readSmallText(path, buf, (int)sizeof(buf));
            if (e != 0) {
                char et[48];
                errnoText(et, (int)sizeof(et), e);
                std::snprintf(one, sizeof(one), "cpu%d %s 读不到(%s)", c, files[f], et);
                out += one;
                // 判决要用的两条读数: 单独记下来(读不到 = 这条路径给不出上限, 由 policy 路径补)
                if (f == 0) { cpuMaxErr[p] = e; }
                if (f == 2) { cpuInfoErr[p] = e; }
            } else {
                for (size_t k = 0; k < std::strlen(buf); ++k) {
                    if (buf[k] == '\n' || buf[k] == '\r') {
                        buf[k] = '\0';
                        break;
                    }
                }
                std::snprintf(one, sizeof(one), "cpu%d %s=", c, files[f]);
                out += one;
                out += buf;
                if (f == 0 || f == 2) {
                    int v = 0;
                    if (parseIntField(buf, 1, &v) == 0) {
                        if (f == 0) { cpuMaxKhz[p] = v; cpuMaxErr[p] = 0; }
                        if (f == 2) { cpuInfoKhz[p] = v; cpuInfoErr[p] = 0; }
                    } else {
                        if (f == 0) { cpuMaxErr[p] = -2; }
                        if (f == 2) { cpuInfoErr[p] = -2; }
                    }
                }
            }
            out += " · ";
        }
        // policy 路径(有些机型只有它可读, 与 cpuN/ 路径互为补充)
        {
            // 2026-10 修正: 这里以前把核号直接当 policy 号用(传的就是 c), 于是真机上
            // cpu4 探的是 policy4 -> ENOENT(2), 被读成"这个核没有 policy 目录"; 真实含义只是
            // "这台机器的 policy 编号不是按第一个核来的"。编号必须由上面的 readlink /
            // related_cpus 定; 只有实在定不出来时才退回核号, 并在文本里标注是兜底。
            const int usePol = (polOfProbe[p] >= 0) ? polOfProbe[p] : c;
            char path[128];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", usePol);
            char buf[kCapFreqBuf];
            const int e = readSmallText(path, buf, (int)sizeof(buf));
            if (e != 0) {
                char et[48];
                errnoText(et, (int)sizeof(et), e);
                std::snprintf(one, sizeof(one),
                              "policy%d(cpu%d 所属%s)/scaling_max_freq 读不到(%s)",
                              usePol, c, (polOfProbe[p] >= 0) ? "" : ", 编号未定按核号兜底", et);
                out += one;
            } else {
                for (size_t k = 0; k < std::strlen(buf); ++k) {
                    if (buf[k] == '\n' || buf[k] == '\r') {
                        buf[k] = '\0';
                        break;
                    }
                }
                std::snprintf(one, sizeof(one), "policy%d(cpu%d 所属%s)/scaling_max_freq=",
                              usePol, c, (polOfProbe[p] >= 0) ? "" : ", 编号未定按核号兜底");
                out += one;
                out += buf;
            }
            out += " · ";
        }
    }
    // =======================================================================
    // ⑥ 判决: 逐个探针核给出"上限读数 vs 标称"的一次性结论(三档, 全部写进文本, 不猜)
    // =======================================================================
    //    ① 上限读数 < 标称 -> 上限是系统压的(差多少、只到标称的百分之几都写出来);
    //    ② 两者相等     -> 内核此刻没压上限, 实际频率仍低只能往 governor/热/功耗墙 查;
    //    ③ 上限读不到   -> 仍不能定案(这不是"没压", 是"读不到"), 并明写还缺哪条读数。
    out += "判决: ";
    for (int p = 0; p < nProbe; ++p) {
        const int ri = capFindPolicyRec(recs, nRec, polOfProbe[p]);
        int upErr = 0;
        int upKhz = 0;
        int nomErr = 0;
        int nomKhz = 0;
        const char* upSrc = "该 policy 的 scaling_max_freq";
        const char* nomSrc = "该 policy 的 cpuinfo_max_freq";
        if (ri >= 0) {
            upErr = recs[ri].maxErr;
            upKhz = recs[ri].maxKhz;
            nomErr = recs[ri].cpuinfoErr;
            nomKhz = recs[ri].cpuinfoKhz;
        } else {
            nomErr = -1;   // policy 未定: 下面退到 cpuN 的两条读数
        }
        if (upErr != 0 && cpuMaxErr[p] == 0) {
            upErr = 0;
            upKhz = cpuMaxKhz[p];
            upSrc = "cpuN/scaling_max_freq";
        }
        if (nomErr != 0 && cpuInfoErr[p] == 0) {
            nomErr = 0;
            nomKhz = cpuInfoKhz[p];
            nomSrc = "cpuN/cpuinfo_max_freq";
        }
        if (nomKhz <= 0) {
            const int v = auroraCpuMaxFreqKhz(probe[p]);
            if (v > 0) {
                nomErr = 0;
                nomKhz = v;
                nomSrc = "本工程读到的该核标称上限";
            }
        }
        if (polOfProbe[p] < 0) {
            std::snprintf(one, sizeof(one),
                          "cpu%d: 所属 policy 定不了(见上面的枚举与映射) => 仍不能定案; 还需要: "
                          "一次能列出的 policyN 目录, 或可读的 related_cpus / readlink 目标; ",
                          probe[p]);
        } else if (upErr != 0) {
            char uw[24];
            capErrWord(uw, (int)sizeof(uw), upErr);
            std::snprintf(one, sizeof(one),
                          "cpu%d 属 policy%d: 上限读数(scaling_max_freq)读不到(%s) => 仍不能定案"
                          "(这不是'没压', 是'读不到'; 注意 cpuN/cpufreq 是 policy%d 的软链接, "
                          "两边是同一个文件, 所以换到 policy 路径读出来还是这个 errno); 已改用"
                          "上面的 time_in_state 历史证据; 若那一处也读不到, 还需要的读数(按优先级): "
                          "① policy%d/bios_limit(固件施加的上限) ② policy%d/stats/trans_table"
                          "(逐档迁移次数) ③ 在更宽的权限域下读同一个 scaling_max_freq"
                          "(例如以 shell 用户读这条只读路径 —— 仍然不写、不锁频、不改 governor); ",
                          probe[p], polOfProbe[p], uw, polOfProbe[p],
                          polOfProbe[p], polOfProbe[p]);
        } else if (nomKhz > 0 && upKhz > 0 && upKhz < nomKhz) {
            std::snprintf(one, sizeof(one),
                          "cpu%d 属 policy%d: %s=%d < %s=%d(差 %d kHz, 只到标称的 %d%%) => "
                          "上限是系统压的(1995MHz 这类'上不去'就是这么来的); ",
                          probe[p], polOfProbe[p], upSrc, upKhz, nomSrc, nomKhz,
                          nomKhz - upKhz, (int)((long long)upKhz * 100LL / (long long)nomKhz));
        } else if (nomKhz > 0 && upKhz >= nomKhz) {
            std::snprintf(one, sizeof(one),
                          "cpu%d 属 policy%d: %s=%d >= %s=%d => 内核此刻没压上限; 实际频率仍低就"
                          "只能往 governor/热/功耗墙 查(见上面的冷却设备与 time_in_state); ",
                          probe[p], polOfProbe[p], upSrc, upKhz, nomSrc, nomKhz);
        } else {
            std::snprintf(one, sizeof(one),
                          "cpu%d 属 policy%d: 上限读数读到了(%d kHz), 标称没读到 => 两者无法比较, "
                          "仍不能定案; ",
                          probe[p], polOfProbe[p], upKhz);
        }
        out += one;
    }
    out += "判决规则: 若 scaling_max_freq < cpuinfo_max_freq, 则上限是系统压的; "
           "若两者相等而实际频率仍低, 才继续查 governor / 热 / 功耗墙";
    // 编号规则(与判决规则同等重要): policyN 的 N 是内核注册/频率域序号, 不等于该
    // policy 里最小的那个核号。必须按上面的枚举 + readlink 结果解读 —— 旧代码正是把核号当
    // policy 号用, 于是 policy4 的 ENOENT 被误读成"该核没有 policy"。
    out += "; 编号规则: policyN 的 N 不是该 policy 的第一个核号(真机上 policy4 可以不存在, "
           "而 cpu4 其实属别的 policy) —— 一律以上面的枚举/readlink 为准";
    // 最后一道诚实防线: 放不下时显式写"尾部被截断"(不静默地把尾部 —— 也就是判决 ——
    // 丢掉)。这条规则必须一直留着: 容量再大也只是把"正常路径永远不会触发它"变成事实, 而
    // 不是把这条诚实规则删掉。扩到 kCapTextCap(8192) 之后, 真机 8.1 这一段需要 3919 字节
    // (见上面常量处的估算), 标记不会再出现。
    {
        const char* const kTrunc =
            " (取证文本超出容量上限, 尾部被截断 —— 这本身就是事实, 不许当成'取证完整')";
        const size_t capBytes = sizeof(g_s.capText) - 1;
        const size_t truncLen = std::strlen(kTrunc);
        size_t copy = out.size();
        if (copy > capBytes) {
            copy = (capBytes > truncLen) ? (capBytes - truncLen) : 0;
            out.resize(copy);
            out += kTrunc;
            copy = out.size();
            if (copy > capBytes) {
                copy = capBytes;
            }
        }
        std::memcpy(g_s.capText, out.data(), copy);
        g_s.capText[copy] = '\0';
        g_s.capTextLen = (int)copy;
    }
}

// 采样一个核(scaling_cur_freq), 落进口径A(主口径)的样本序列。
// 同时做"越界落核"取证: 样本所在核不在可用核集合里 -> 计数 + 记位图(硬事实, 必须上报)。
// 调用者必须持锁。
void sampleCoreLocked(int cpu)
{
    int khz = 0;
    const int e = readCurFreqKhz(cpu, &khz);
    if (e != 0) {
        ++g_s.curFreqFail;
        g_s.curFreqErrno = e;
        g_s.curFreqErrCpu = cpu;
        return;
    }
    ++g_s.curFreqOk;
    if (cpu >= 0 && cpu < 64 && g_s.allowedMask != 0ull && ((g_s.allowedMask >> (unsigned)cpu) & 1ull) == 0) {
        ++g_s.outOfSetSamples;
        g_s.outOfSetMask |= (1ull << (unsigned)cpu);
    }
    if (g_s.count < kMaxSamples) {
        g_s.khz[g_s.count] = khz;
        g_s.cpuOf[g_s.count] = cpu;
        g_s.nomOf[g_s.count] = (cpu >= 0) ? auroraCpuMaxFreqKhz(cpu) : 0;
        ++g_s.count;
    } else {
        ++g_s.overflowSamples;
    }
}

// 采样一个核, 落进口径B(对照口径: 可用核集合全部核)。调用者必须持锁。
void sampleCoreBLocked(int cpu)
{
    int khz = 0;
    const int e = readCurFreqKhz(cpu, &khz);
    if (e != 0) {
        ++g_s.curFreqFailB;
        g_s.curFreqErrnoB = e;
        g_s.curFreqErrCpuB = cpu;
        return;
    }
    ++g_s.curFreqOkB;
    if (g_s.countB < kMaxSamples) {
        g_s.khzB[g_s.countB] = khz;
        g_s.cpuB[g_s.countB] = cpu;
        g_s.nomB[g_s.countB] = (cpu >= 0) ? auroraCpuMaxFreqKhz(cpu) : 0;
        ++g_s.countB;
    } else {
        ++g_s.overflowB;
    }
}

// 一个 tick。调用者必须持锁。
//   单核项: 口径A = 负载线程(本线程)在当刻所在的那个核; 口径B 不采(countB 恒为 0)。
//   多核项: 口径A = 当刻有负载线程落上的核(主线程所在核 ∪ 已登记池线程所在核, 逐核去重);
//           口径B = 内核允许本进程使用的全部核(旧口径, 保留作对照)。
void tickLocked()
{
    ++g_s.ticks;
    if (g_s.multi) {
        // ---- 口径A: 先问"这一瞬间哪些核上有我们的负载线程" ----
        int coresA[kMaxTickCores];
        int nA = 0;
        unsigned long long seen = 0ull;
        int err = 0;
        const int mainCpu = cpuOfThread(g_s.tid, &err);
        if (mainCpu >= 0) {
            ++g_s.threadStatOk;
            if (mainCpu < 64) {
                seen |= (1ull << (unsigned)mainCpu);
                coresA[nA++] = mainCpu;
            }
        } else {
            ++g_s.threadStatFail;
            g_s.threadStatErrno = err;
        }
        for (int i = 0; i < kMaxWorkerTids && nA < kMaxTickCores; ++i) {
            const long t = (long)g_workerTidSlots[i].load(std::memory_order_acquire);
            if (t <= 0) {
                continue;
            }
            const int c = cpuOfThread(t, nullptr);
            if (c < 0) {
                ++g_s.workerTidFail;   // 线程已退出等(计数, 不猜)
                continue;
            }
            if (c >= 64) {
                continue;
            }
            if (((seen >> (unsigned)c) & 1ull) != 0ull) {
                continue;              // 同一个核上有多个线程(SMT) -> 只算一次
            }
            seen |= (1ull << (unsigned)c);
            coresA[nA++] = c;
        }
        if (nA > 0) {
            ++g_s.ticksA;
            g_s.workerCoreSum += nA;
            if (nA > g_s.workerCoreMax) {
                g_s.workerCoreMax = nA;
            }
            for (int i = 0; i < nA; ++i) {
                sampleCoreLocked(coresA[i]);
            }
        } else {
            // 一个可用 tid 都没有(例如本项没有走 gb7ParallelFor 建池): 口径A 这一 tick 无样本。
            // 计数, 文本里会写明"本项口径A 覆盖不到", 不拿口径B 冒充口径A。
            ++g_s.ticksANone;
        }
        // ---- 口径B: 可用核集合全部核(旧口径, 对照) ----
        for (int i = 0; i < g_s.coreCount; ++i) {
            sampleCoreBLocked(g_s.cores[i]);
        }
    } else {
        int err = 0;
        int cpu = cpuOfThread(g_s.tid, &err);
        if (cpu >= 0) {
            ++g_s.threadStatOk;
        } else {
            ++g_s.threadStatFail;
            g_s.threadStatErrno = err;
            cpu = g_s.fallbackCpu;   // 读不到线程所在核 -> 用会话开始那一刻的核兜底(标注)
        }
        if (cpu >= 0) {
            ++g_s.ticksA;
            sampleCoreLocked(cpu);
        }
    }
    if (kThermalEveryN > 0 && (g_s.ticks % kThermalEveryN) == 1) {
        readThermalLocked();
    }
}

// 采样线程主体: 没打标时挂起等待; 打标后立刻采一次, 之后每 kIntervalMs 一次。
void samplerMain()
{
    // QoS 运行条件(旁路线程): 采样线程只需要"能按时读到 sysfs", 不需要 CPU 时间份额 ——
    // 设最低档 QOS_BACKGROUND, 让调度器优先把份额给负载线程。这一步在拿锁之前做完,
    // 不改变任何采样时刻/间隔/口径; 设备不支持 / canIUse 为假 / 符号拿不到时它只记账并
    // 返回负值(静默降级), 采样照常。线程退出时 QoS 自然失效, 无需复位。
    (void)auroraQosApplyBypassThread();
    std::unique_lock<std::mutex> lk(g_mtx);
    for (;;) {
        if (g_stop) {
            return;
        }
        if (!g_s.armed) {
            g_cv.wait(lk, [] { return g_stop || g_s.armed != 0; });
            continue;
        }
        tickLocked();
        const unsigned long long gen = g_markGen;
        // 三个唤醒条件: 会话结束 / 本项计时区间结束 / 下一项已经打标(立刻去采它的第一个样本)
        g_cv.wait_for(lk, std::chrono::milliseconds(kIntervalMs),
                      [gen] { return g_stop || g_s.armed == 0 || g_markGen != gen; });
    }
}

// ---------------------------------------------------------------------------
//  文本工具
// ---------------------------------------------------------------------------
void errnoText(char* dst, int cap, int e)
{
    // 口径与工程其它部分一致(见 types/libaurorabench/index.d.ts):
    //   >0 = 真实 errno; -1 = 打得开但内容为空; -2 = 内容不是正十进制数
    const char* name = nullptr;
    switch (e) {
        case 1:  name = "EPERM";  break;
        case 2:  name = "ENOENT"; break;
        case 5:  name = "EIO";    break;
        case 13: name = "EACCES"; break;
        case 21: name = "EISDIR"; break;
        default: name = nullptr;  break;
    }
    if (e == -1) {
        std::snprintf(dst, (size_t)cap, "内容为空(码 -1)");
    } else if (e == -2) {
        std::snprintf(dst, (size_t)cap, "内容不是正十进制数(码 -2)");
    } else if (name != nullptr) {
        std::snprintf(dst, (size_t)cap, "errno=%d(%s)", e, name);
    } else {
        std::snprintf(dst, (size_t)cap, "errno=%d", e);
    }
}

std::string mhzText(int khz)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%dMHz", (khz + 500) / 1000);   // kHz -> MHz(四舍五入)
    return std::string(b);
}

// ---------------------------------------------------------------------------
//  预热取证的文本(③b)。compact=false = 逐遍全量(每项正式写进 runFreq 的那一段);
//  compact=true  = 一行摘要 —— 供"本遍跳过预热"的那一轮引用第 1 轮的结果。
//  两处用的是同一批结构体字段, 只是详略不同: 不会出现"引用的是一个数、正文里是另一个数"。
//  为什么必须两种详略: 预热每个 (负载 id, 阶段) 只做一次(gb7.cpp 的会话表), 第 2..N 轮
//  不再预热 —— 那几轮的报告里必须写明"为什么没有预热"并引用第 1 轮的判据结论, 不静默跳过。
// ---------------------------------------------------------------------------
void appendWarmText(std::string& s, bool compact)
{
    char one[1024];
    s += "; 预热取证(升频/稳态, 全部在计时区间之外, 不计入 o.ms) = 预热 ";
    if (compact) {
        std::snprintf(one, sizeof(one), "%d 遍(上限 %d 遍 / %d ms), 累计 %d ms",
                      g_s.warmPasses, kMaxWarmupPasses, kWarmupMaxTotalMs, g_s.warmTotalMs);
        s += one;
        for (int i = 0; i < g_s.warmPasses && i < kMaxWarmupPasses; ++i) {
            const WarmPassRec& w = g_s.warm[i];
            if (w.used == 0 || w.count <= 0) {
                std::snprintf(one, sizeof(one), "; 第%d遍: 没有采到样本", i + 1);
                s += one;
                continue;
            }
            std::snprintf(one, sizeof(one),
                          "; 第%d遍: 中位 %s, 占标称比 %d.%d%%, 样本 %d, 耗时 %d ms",
                          i + 1, mhzText(w.medKhz).c_str(), w.medRatioPermille / 10,
                          w.medRatioPermille % 10, w.count, w.ms);
            s += one;
        }
    } else {
        std::snprintf(one, sizeof(one), "%d 遍(本工程上限 %d 遍 / %d ms), 累计 %d ms",
                      g_s.warmPasses, kMaxWarmupPasses, kWarmupMaxTotalMs, g_s.warmTotalMs);
        s += one;
        for (int i = 0; i < g_s.warmPasses && i < kMaxWarmupPasses; ++i) {
            const WarmPassRec& w = g_s.warm[i];
            if (w.used == 0) {
                std::snprintf(one, sizeof(one), "; 第%d遍: 未执行", i + 1);
                s += one;
                continue;
            }
            if (w.count <= 0) {
                std::snprintf(one, sizeof(one),
                              "; 第%d遍: 没有采到任何样本(耗时 %d ms%s) —— 这一遍不参与稳态判定",
                              i + 1, w.ms,
                              (w.windowClosed == 0) ? "; 且这一遍的计时区间没有正常收尾" : "");
                s += one;
                continue;
            }
            std::snprintf(one, sizeof(one),
                          "; 第%d遍: 中位 %s / 最小 %s / 最大 %s, 占标称比中位 %d.%d%%, 样本 %d 个, "
                          "不同核 %d 个, 池线程 tid %d 个, 前半段中位 %s / 后半段中位 %s, 耗时 %d ms",
                          i + 1, mhzText(w.medKhz).c_str(), mhzText(w.loKhz).c_str(),
                          mhzText(w.hiKhz).c_str(), w.medRatioPermille / 10,
                          w.medRatioPermille % 10, w.count, w.cores,
                          w.workerTidCount, mhzText(w.firstHalfMedKhz).c_str(),
                          mhzText(w.secondHalfMedKhz).c_str(), w.ms);
            s += one;
        }
    }
    if (g_s.warmPasses <= 0) {
        s += "; 本遍没有执行预热 —— 原因见本轮说明(三种可能: 预热未启用 / 本项本阶段" 
               "此前已预热过而本遍按会话表跳过 / 负载调用抛异常被兜住); 标注, 不假装做过";
    } else if (g_s.warmSteady != 0) {
        if (compact) {
            std::snprintf(one, sizeof(one), "; 稳态判据 -> 命中第 %d 条", g_s.warmSteadyHow);
            s += one;
        } else {
            std::snprintf(one, sizeof(one),
                          "; 稳态判据 -> 命中第 %d 条(%s): 正式计时是在预热之后的稳态下开始的",
                          g_s.warmSteadyHow,
                          (g_s.warmSteadyHow == 1) ? "该遍占标称比中位已达跑满判据(90%)"
                          : ((g_s.warmSteadyHow == 2) ? "相邻两遍的频率中位差 <= 3%(再加压也不涨了)"
                                                      : "单遍样本足够多且前后半段中位差 <= 3%"));
            s += one;
        }
    } else {
        const char* why = (g_s.warmStoppedBy == 1) ? "达到预热遍数上限"
                        : ((g_s.warmStoppedBy == 2) ? "达到预热总时长上限"
                        : ((g_s.warmStoppedBy == 3) ? "预热遍没有采到样本" : "尚未判定"));
        if (compact) {
            std::snprintf(one, sizeof(one), "; 稳态判据 -> 未命中(停止原因: %s)", why);
            s += one;
        } else {
            std::snprintf(one, sizeof(one),
                          "; 稳态判据 -> 未命中(停止原因: %s) —— 也就是说: 再加压频率也**没有"
                          "继续上升**, 或者根本采不到样本; 此时正式计时区间的频率若仍低于标称上限, "
                          "那就不是「负载喂不饱」, 而是上限本身被压住了(见下面的上限取证)",
                          why);
            s += one;
        }
    }
}
// 汇总成一行文本(在采样线程 join 之后调用, 因此不需要持锁)
void buildText()
{
    g_text[0] = '\0';
    std::string s;
    // 单条小行的缓冲: 256 -> 2048(2026-10)。为什么必须加: 中文在 UTF-8 下一个字 3 字节,
    // 而这一版里"越界落核""稳态判据未命中""口径B 的差说明"等单条消息都有 250~400 字节,
    // 用 256 会在句子中间被 snprintf 截断 —— 那正是"日志里少了半句还没人发现"的来源。
    char one[2048];

    // ---- ① 标称 vs 实际 + 占标称比 + 跑满判据(一行里最要紧的几个数) ----
    //  统计工具全部用"拷进暂存再排"的方式(medianOfInts / medianRatioPermille), 因此
    //  khz[] / cpuOf[] / nomOf[] 三条数组始终一一对应(不会因为排序而错位)。
    if (g_s.count > 0) {
        const int mid = medianOfInts(g_s.khz, g_s.count, g_sortScratch, kMaxSamples);
        int lo = g_s.khz[0];
        int hi = g_s.khz[0];
        for (int i = 1; i < g_s.count; ++i) {
            if (g_s.khz[i] < lo) { lo = g_s.khz[i]; }
            if (g_s.khz[i] > hi) { hi = g_s.khz[i]; }
        }
        int nomLo = 0, nomHi = 0, nomMid = 0;
        int nomN = 0;
        for (int i = 0; i < g_s.count; ++i) {
            if (g_s.nomOf[i] > 0) {
                if (nomN == 0 || g_s.nomOf[i] < nomLo) { nomLo = g_s.nomOf[i]; }
                if (nomN == 0 || g_s.nomOf[i] > nomHi) { nomHi = g_s.nomOf[i]; }
                ++nomN;
            }
        }
        if (nomN > 0) {
            nomMid = medianOfInts(g_s.nomOf, g_s.count, g_sortScratch, kMaxSamples);
        }
        const int ratioPermille = medianRatioPermille(g_s.khz, g_s.nomOf, g_s.count,
                                                     g_ratioScratch, kMaxSamples);
        s += (g_s.multi ? "运行时频率(口径A·只统计当刻有负载线程落上的核, 逐核去重) "
                        : "运行时频率 ");
        s += "中位 " + mhzText(mid) + " / 最小 " + mhzText(lo) + " / 最大 " + mhzText(hi);
        if (nomN > 0) {
            s += "(标称上限 [逐样本按该样本所在核取] 中位 " + mhzText(nomMid) + " / 最小 " +
                 mhzText(nomLo) + " / 最大 " + mhzText(nomHi);
            if (g_s.nominalMaxKhz > 0) {
                s += "; 会话开始那一刻的核 " + mhzText(g_s.nominalMaxKhz);
            }
            s += ")";
        } else {
            s += "(标称上限读不到)";
        }
        std::snprintf(one, sizeof(one), " 采样 %d 次", g_s.count);
        s += one;
        if (g_s.multi) {
            const double perTick = (g_s.ticksA > 0)
                ? ((double)g_s.workerCoreSum / (double)g_s.ticksA) : 0.0;
            std::snprintf(one, sizeof(one), "(= 有样本 tick %d 个 x 每 tick 平均 %.1f 个核, 单 tick 最多 %d 个核)",
                          g_s.ticksA, perTick, g_s.workerCoreMax);
            s += one;
        }
        if (g_s.overflowSamples > 0) {
            std::snprintf(one, sizeof(one), "(另有 %d 个样本超出上限被丢弃)", g_s.overflowSamples);
            s += one;
        }
        // ---- 占标称比 + 跑满判据(任务要求: 一眼看到 中位/上限/占比 + 一句结论) ----
        if (nomN > 0) {
            std::snprintf(one, sizeof(one),
                          " 占标称比 中位 %d.%d%%(逐样本 = 该样本频率 / 该样本所在核的标称上限, "
                          "再取中位; 样本数 %d)", ratioPermille / 10, ratioPermille % 10, g_s.count);
            s += one;
            if (ratioPermille >= kFullRatioPct * 10) {
                std::snprintf(one, sizeof(one),
                              " 跑满判据(我们自定: 占比中位 >= %d%%) -> 已达到该芯片在"
                              "本项条件下的稳态最高性能", kFullRatioPct);
                s += one;
            } else {
                // ---- "还差几个百分点"的单位算法(2026-10 修) ----
                // 旧写法 = (kFullRatioPct * 10) - (ratioPermille / 10): 两个操作数单位不一致 ——
                //   kFullRatioPct * 10 是"千分比"(90 -> 900), 而 ratioPermille / 10 是**整数除法
                //   得到的百分点**(879 -> 87, 小数被丢掉), 相减得到的是一个既不是千分比也不是
                //   百分点的数。真机 8.0 报告里 87.9% 被印成"还差 813 个百分点"(实际 2.1),
                //   89.0% 印成 811(实际 1.0) —— 量级错了约 400 倍, 判据本身没错但结论句是错的。
                // 新写法: 阈值与实测都保持千分比, 先在同一单位上相减, 再换算成百分点打一位
                //   小数(与上面"占标称比 中位 x.y%%"同一精度):
                //     还差千分比 = kFullRatioPct * 10 - ratioPermille;
                //     还差百分点 = 还差千分比 / 10 (整数部分) . 还差千分比 % 10 (小数位)
                //   复算: 879 -> 900-879 = 21 千分比 -> 2.1 个百分点;
                //         890 -> 900-890 = 10 千分比 -> 1.0 个百分点;
                //         696 -> 204 千分比 -> 20.4 个百分点。
                // 判据与阈值一个字都没改: 下面这条 if 的判据仍是 ratioPermille >= 900。
                const int gapPermille = kFullRatioPct * 10 - ratioPermille;
                std::snprintf(one, sizeof(one),
                              " 跑满判据(我们自定: 占比中位 >= %d%%) -> 未达到(还差 %d.%d "
                              "个百分点) —— 差距原因见下面的上限取证与预热取证, 不靠事后归一化去补",
                              kFullRatioPct, gapPermille / 10, gapPermille % 10);
                s += one;
            }
        }
        // ---- 口径B(对照: 可用核集合全部核; 旧口径) ----
        if (g_s.multi && g_s.countB > 0) {
            const int midB = medianOfInts(g_s.khzB, g_s.countB, g_sortScratch, kMaxSamples);
            int loB = g_s.khzB[0], hiB = g_s.khzB[0];
            for (int i = 1; i < g_s.countB; ++i) {
                if (g_s.khzB[i] < loB) { loB = g_s.khzB[i]; }
                if (g_s.khzB[i] > hiB) { hiB = g_s.khzB[i]; }
            }
            const int ratioBPermille = medianRatioPermille(g_s.khzB, g_s.nomB, g_s.countB,
                                                          g_ratioScratch, kMaxSamples);
            s += "; 运行时频率(口径B·内核允许本进程使用的全部核, 旧口径, 仅作对照) 中位 " +
                 mhzText(midB) + " / 最小 " + mhzText(loB) + " / 最大 " + mhzText(hiB);
            std::snprintf(one, sizeof(one), " 采样 %d 次(= %d 核 x %d 轮); 占标称比 中位 %d.%d%%",
                          g_s.countB, g_s.coreCount, g_s.ticks > 0 ? g_s.ticks : 0,
                          ratioBPermille / 10, ratioBPermille % 10);
            s += one;
            std::snprintf(one, sizeof(one),
                          "(口径B 与口径A 的差 = 有多少核在空转: 空闲核停在最低频档会把中位拉低, "
                          "所以口径B 的中位不能代表干活核的频率, 判「跑满」一律以口径A 为准; "
                          "口径B 的最小值 %s 就是空转核的读数)",
                          mhzText(loB).c_str());
            s += one;
        }
    } else if (g_s.curFreqFail > 0) {
        char e[64];
        errnoText(e, (int)sizeof(e), g_s.curFreqErrno);
        std::snprintf(one, sizeof(one), "运行时频率 读不到(scaling_cur_freq 打不开: %s, cpu=%d; 本项 %d/%d 次读到)",
                      e, g_s.curFreqErrCpu, g_s.curFreqOk, g_s.curFreqOk + g_s.curFreqFail);
        s += one;
    } else if (g_s.ticks == 0) {
        s += "运行时频率 没有样本(本项计时区间没有打点, 或区间短于 1 个采样周期)";
    } else {
        s += "运行时频率 没有样本(采样线程跑了 tick 但一个读数都没拿到)";
    }

    // ---- ② 采样核(口径的核心: 到底采的是哪个核) ----
    s += "; 采样核 = ";
    if (g_s.multi) {
        // ---- 口径A: 当期真的有负载线程落上去的核(逐核列出) ----
        {
            unsigned long long usedA = 0ull;
            int distinctA = 0;
            char listA[220];
            int usedLen = 0;
            listA[0] = '\0';
            for (int i = 0; i < g_s.count; ++i) {
                const int c = g_s.cpuOf[i];
                if (c < 0 || c >= 64) {
                    continue;
                }
                if (((usedA >> (unsigned)c) & 1ull) != 0ull) {
                    continue;
                }
                usedA |= (1ull << (unsigned)c);
                ++distinctA;
                if (usedLen < (int)sizeof(listA) - 16) {
                    char two[16];
                    std::snprintf(two, sizeof(two), "%scpu%d", (usedLen > 0) ? "," : "", c);
                    std::strncat(listA, two, sizeof(listA) - 1 - (size_t)usedLen);
                    usedLen = (int)std::strlen(listA);
                }
            }
            if (distinctA > 0) {
                std::snprintf(one, sizeof(one),
                              "口径A(当期有负载线程落上的核) %d 个: %s(登记到的池线程 tid %d 个%s)",
                              distinctA, listA, g_s.workerTidCount,
                              (g_s.workerTidOverflow > 0) ? "; 另有登记被表满丢弃" : "");
                s += one;
            } else {
                std::snprintf(one, sizeof(one),
                              "口径A 一个核都没采到(有样本 tick %d 个, 其中 %d 个 tick 没有任何"
                              "可用池线程 tid —— 本项可能没有走 gb7ParallelFor 建池; 此时口径A 覆盖不到, "
                              "只有口径B 可用, 已标注)", g_s.ticksA, g_s.ticksANone);
                s += one;
            }
            s += "; 采样核 = ";
        }
        char list[200];
        aurora_cpu_detail::maskToCpuList(g_s.coreMask, 64, list, (int)sizeof(list));
        // 措辞用"内核允许本进程使用的 N 个核": N 就是下面列出的核数(位图 ∩ 频率表),
        // 不写"全部"—— 万一集合被频率表长度截断, "全部"就成了过度声明。
        std::snprintf(one, sizeof(one), "口径B(对照/旧口径) = 内核允许本进程使用的 %d 个核(%s)",
                      g_s.coreCount, list);
        s += one;
        // ---- 越界落核(硬事实: 样本落在了可用核集合之外的核上) ----
        if (g_s.outOfSetSamples > 0) {
            char olist[160];
            aurora_cpu_detail::maskToCpuList(g_s.outOfSetMask, 64, olist, (int)sizeof(olist));
            std::snprintf(one, sizeof(one),
                          " · 越界落核: 有 %d 个样本采到可用核集合之外的核(%s) —— 说明本项"
                          "实际跑的核不全是可用核集合里的核, 这是硬事实, 会影响「标称上限」该取谁",
                          g_s.outOfSetSamples, olist);
            s += one;
        }
    } else {
        // 单核口径: 把实际采到过的核列出来(正常只有 1 个; 出现多个 = 线程迁移过)
        char list[96];
        int used = 0;
        list[0] = '\0';
        int distinct[8];
        int distinctN = 0;
        for (int i = 0; i < g_s.count && distinctN < 8; ++i) {
            bool seen = false;
            for (int k = 0; k < distinctN; ++k) {
                if (distinct[k] == g_s.cpuOf[i]) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            distinct[distinctN++] = g_s.cpuOf[i];
        }
        if (distinctN == 0) {
            std::snprintf(one, sizeof(one), "本线程所在核(没有样本, 会话开始那一刻的核 cpu=%d)",
                          g_s.fallbackCpu);
            s += one;
        } else {
            for (int k = 0; k < distinctN; ++k) {
                std::snprintf(list + used, sizeof(list) - (size_t)used, "%scpu%d",
                              used > 0 ? "," : "", distinct[k]);
                used = (int)std::strlen(list);
            }
            std::snprintf(one, sizeof(one), "本线程所在核(%s%s)", list,
                          distinctN > 1 ? " —— 采样期间线程迁移过, 已逐一列出" : "");
            s += one;
        }
    }

    // ---- ②b 逐核占用(2026-10 追加; 只有多核阶段才有意义) ----
    //  这一段回答的唯一问题是: "本项计时区间里, 那些应当被用满的核到底有没有在干活"。
    //  数据源 = /proc/stat 的逐核 jiffies 差值(整机口径, 含后台线程); 读不到就写读不到。
    if (g_s.multi) {
        s += "; 逐核占用 = ";
        if (g_s.statStart.ok == 0 || g_s.statStop.ok == 0) {
            const int e = (g_s.statStart.ok == 0) ? g_s.statStart.err : g_s.statStop.err;
            char ebuf[64];
            errnoText(ebuf, (int)sizeof(ebuf), e);
            std::snprintf(one, sizeof(one),
                          "读不到(/proc/stat %s; 起始 ok=%d 终止 ok=%d) —— 无法判断是否有核空着",
                          ebuf, g_s.statStart.ok, g_s.statStop.ok);
            s += one;
        } else {
            int idleCores = 0;
            int measured = 0;
            double idleSum = 0.0;
            int busiestLow = -1;
            double busiestLowPct = 101.0;
            char idleList[128];
            idleList[0] = '\0';
            int idleUsed = 0;
            // ---- 新增判据(2026-10): "在整个项的计时区间内基本没干活"的核  ----
            //  与"完全没干活的核(busy 差值 == 0)"的区别: 那一条要求忙碌 jiffies 一点都没涨,
            //  而一个核只要被调度器跑过哪怕 1 个 jiffy(10ms)噪声, 就会从那一类里漏出去。
            //  这一条按比例判: 整个计时区间里 busy 占比低于 kBusyIdlePct(5%)的核, 单独列出,
            //  并写明"该核在整个本项期间基本没干活"。
            //  为什么是 5%: 一个真正被负载占满的核, 忙占比接近 100%; 一个只是被后台噪声碰过的核,
            //  在 700ms~数秒的计时区间里忙占比通常只有零点几个百分点。5% 是"噪声"与"真的在干活"
            //  之间的分界, 且只用于诊断文本 —— 它不计分、不改变任何负载行为。
            const double kBusyIdlePct = 5.0;   // 阈值(仅诊断)
            int lowBusyCores = 0;
            char lowBusyList[256];
            lowBusyList[0] = '\0';
            int lowBusyUsed = 0;
            double lowBusySum = 0.0;
            for (int i = 0; i < g_s.coreCount && i < kMaxStatCpus; ++i) {
                const int c = g_s.cores[i];
                if (c < 0 || c >= kMaxStatCpus) {
                    continue;
                }
                const long long busy = g_s.statStop.busy[c] - g_s.statStart.busy[c];
                const long long idle = g_s.statStop.idle[c] - g_s.statStart.idle[c];
                const long long tot = (busy > 0 ? busy : 0) + (idle > 0 ? idle : 0);
                if (tot <= 0) {
                    continue;   // 这个核在本区间没有可用的 jiffies 差值(读数异常): 不计入
                }
                ++measured;
                const double busyPct = 100.0 * (double)(busy > 0 ? busy : 0) / (double)tot;
                const double idlePct = 100.0 - busyPct;
                idleSum += idlePct;
                if (busy <= 0) {
                    ++idleCores;
                    char two[16];
                    std::snprintf(two, sizeof(two), "%scpu%d", (idleUsed > 0) ? "," : "", c);
                    if (idleUsed + (int)std::strlen(two) < (int)sizeof(idleList) - 2) {
                        std::strncat(idleList, two, sizeof(idleList) - 1 - (size_t)idleUsed);
                        idleUsed += (int)std::strlen(two);
                    }
                }
                if (busyPct < busiestLowPct) {
                    busiestLowPct = busyPct;
                    busiestLow = c;
                }
                // "整个项期间基本没干活"的核(忙占比 < 5%): 单独累积, 逐核写出忙占比 
                if (busyPct < kBusyIdlePct) {
                    ++lowBusyCores;
                    lowBusySum += busyPct;
                    char two[40];
                    std::snprintf(two, sizeof(two), "%scpu%d(忙%.1f%%)",
                                  (lowBusyUsed > 0) ? "," : "", c, busyPct);
                    if (lowBusyUsed + (int)std::strlen(two) < (int)sizeof(lowBusyList) - 2) {
                        std::strncat(lowBusyList, two, sizeof(lowBusyList) - 1 - (size_t)lowBusyUsed);
                        lowBusyUsed += (int)std::strlen(two);
                    }
                }
                std::snprintf(one, sizeof(one), "%scpu%d %.0f%%忙(%lld/%lld jiffies)",
                              (i > 0) ? " " : "", c, busyPct, (busy > 0 ? busy : 0), tot);
                s += one;
            }
            if (measured == 0) {
                s += "(本区间没有可用的逐核 jiffies 差值)";
            } else {
                std::snprintf(one, sizeof(one),
                              " | 空闲比例(各核 idle 占比均值) %.1f%%; 完全没干活的核 %d/%d%s%s; "
                              "最闲的核 cpu%d 忙 %.0f%%;[整机口径 /proc/stat 差值, 含后台线程]",
                              idleSum / (double)measured, idleCores, measured,
                              (idleCores > 0) ? ": " : "",
                              (idleList[0] != '\0') ? idleList : "无",
                              busiestLow, busiestLowPct);
                s += one;
                // 新增判据: 整个本项计时区间内忙占比 < 5% 的核(逐核 + 均值 + 一句结论) 
                if (lowBusyCores == 0) {
                    std::snprintf(one, sizeof(one),
                                  "; 整项忙占比 < %.0f%% 的核 0/%d —— 工作集合里每一个核在本项"
                                  "整个计时区间内都干了活(不是'有几个线程在跑', 是每个核都在跑)",
                                  kBusyIdlePct, measured);
                } else {
                    std::snprintf(one, sizeof(one),
                                  "; 整项忙占比 < %.0f%% 的核 %d/%d: %s(这些核忙占比均值 %.1f%%) —— "
                                  "该核在整个本项期间基本没干活(忙占比阈值判据; 与'完全没干活(Δ忙=0)'"
                                  "分开列: 这里连噪声级的一两个 jiffy 也算进去了)",
                                  kBusyIdlePct, lowBusyCores, measured,
                                  (lowBusyList[0] != '\0') ? lowBusyList : "无",
                                  lowBusySum / (double)lowBusyCores);
                }
                s += one;
            }
        }
    }

    // ---- ③ 口径声明(单核项与多核项含义完全不同, 不许含糊) ----
    s += "; 口径 = ";
    if (g_s.multi) {
        s += "口径A = 每个采样时刻有负载线程落上去的核(主线程所在核 ∪ 已登记池线程所在核, "
             "逐核去重)的当前频率, 统计量是「干活核 x 时间」的合并样本; "
             "口径B = 内核允许本进程使用的全部核的当前频率, 统计量是「可用核集合全部核 x 时间」的"
             "合并样本(旧口径, 只作对照 —— 空闲核停在最低频档, 会把中位拉低), "
             "两种口径都逐条写出、不混成一个, 不是单线程口径";
    } else {
        s += "跑本项负载的那个线程在每次采样时刻所在的核, 不是全机所有核";
    }
    if (!g_s.stopped) {
        s += "; 本项计时区间未正常收尾(markStop 未调用), 样本可能含计时区间之外的时间";
    }

    // ---- ③b 升频/稳态预热取证(计时区间之外) ----
    //  口径见 cpu_freq_sample.h: 预热遍用的是同一份负载, 其耗时与读数一律不进 o.ms;
    //  正文由 appendWarmText() 生成 —— 与"本遍跳过预热"时引用的那段摘要同源, 只是详略不同。
    appendWarmText(s, false);


    // ---- ③c 频率上限取证(scaling_max_freq 等; 计时区间之外读一次) ----
    if (g_s.capTextLen > 0) {
        s += "; ";
        s += g_s.capText;
    }

    // ---- ④ 数据源与 errno(逐项记; 读不到就写读不到) ----
    if (g_s.curFreqFail == 0) {
        std::snprintf(one, sizeof(one), "; 源 scaling_cur_freq 全部读到(errno=0; 本项 %d/%d 次)",
                      g_s.curFreqOk, g_s.curFreqOk);
        s += one;
    } else {
        char e[64];
        errnoText(e, (int)sizeof(e), g_s.curFreqErrno);
        std::snprintf(one, sizeof(one),
                      "; 源 scaling_cur_freq 有读失败(最后一次 %s, cpu=%d; 本项 %d/%d 次读到)",
                      e, g_s.curFreqErrCpu, g_s.curFreqOk, g_s.curFreqOk + g_s.curFreqFail);
        s += one;
    }
    // 口径B 的读数单独记一份(两种口径的成功/失败次数不混成一个数)
    if (g_s.multi) {
        if (g_s.curFreqFailB == 0) {
            std::snprintf(one, sizeof(one),
                          "; 口径B 源 scaling_cur_freq 全部读到(errno=0; 本项 %d/%d 次)",
                          g_s.curFreqOkB, g_s.curFreqOkB);
            s += one;
        } else {
            char e[64];
            errnoText(e, (int)sizeof(e), g_s.curFreqErrnoB);
            std::snprintf(one, sizeof(one),
                          "; 口径B 源 scaling_cur_freq 有读失败(最后一次 %s, cpu=%d; 本项 %d/%d 次读到)",
                          e, g_s.curFreqErrCpuB, g_s.curFreqOkB,
                          g_s.curFreqOkB + g_s.curFreqFailB);
            s += one;
        }
    }
    if (!g_s.multi) {
        if (g_s.threadStatFail == 0) {
            std::snprintf(one, sizeof(one),
                          "; 源 /proc/self/task/%ld/stat 全部读到(errno=0; 本项 %d/%d 次)",
                          g_s.tid, g_s.threadStatOk, g_s.threadStatOk);
            s += one;
        } else {
            char e[64];
            errnoText(e, (int)sizeof(e), g_s.threadStatErrno);
            std::snprintf(one, sizeof(one),
                          "; 源 /proc/self/task/%ld/stat 有读失败(最后一次 %s; 本项 %d/%d 次读到, "
                          "失败的 tick 用会话开始那一刻的核 cpu=%d 兜底)",
                          g_s.tid, e, g_s.threadStatOk, g_s.threadStatOk + g_s.threadStatFail,
                          g_s.fallbackCpu);
            s += one;
        }
    }
    if (g_s.governor[0] != '\0') {
        std::snprintf(one, sizeof(one), "; governor=%s", g_s.governor);
        s += one;
    } else if (g_s.govErrno != 0) {
        char e[64];
        errnoText(e, (int)sizeof(e), g_s.govErrno);
        std::snprintf(one, sizeof(one), "; governor 读不到(%s)", e);
        s += one;
    }
    if (g_s.thermalOk > 0) {
        std::snprintf(one, sizeof(one), "; 热区最高 %.1f°C(采样期间, %d 次读到, 参考)",
                      (double)g_s.thermalMaxMilli / 1000.0, g_s.thermalOk);
        s += one;
    } else if (g_s.thermalFail > 0) {
        char e[64];
        errnoText(e, (int)sizeof(e), g_s.thermalErrno);
        std::snprintf(one, sizeof(one), "; 热区温度读不到(%s/%d 次失败, 参考项)", e, g_s.thermalFail);
        s += one;
    }

    // 这一行的收尾也是一条"不许静默截断"的出口: 原来这里是一句 std::snprintf(g_text, ...) ——
    // 汇总文本一旦超过容量, snprintf 会悄悄把尾部(结论/errno 清单)切掉且不留任何痕迹。
    // 现在改成显式判断: 放得下逐字节照抄(正常路径与原来完全一致), 放不下就在尾部写"被截断"
    // 标记 —— 与 readFreqCapEvidence 末尾那条兜底同一个口径。容量 16384 下真机需 7256
    // 字节(见 g_text 容量说明), 因此这条出口正常路径永不触发。
    {
        const char* const kTrunc =
            " (运行时频率文本超出容量上限, 尾部被截断 —— 这本身就是事实, 不许当成'取证完整')";
        const size_t capBytes = sizeof(g_text) - 1;
        const size_t truncLen = std::strlen(kTrunc);
        if (s.size() > capBytes) {
            const size_t keep = (capBytes > truncLen) ? (capBytes - truncLen) : 0;
            std::memcpy(g_text, s.data(), keep);
            std::memcpy(g_text + keep, kTrunc, truncLen);
            g_text[keep + truncLen] = '\0';
        } else {
            std::memcpy(g_text, s.data(), s.size());
            g_text[s.size()] = '\0';
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
namespace {

// SessionBegin 的真正实现(与旧版逐行一致, 只是搬进一个内部函数, 由外层统一兜异常)
void sessionBeginImpl(int threads)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    // 重置整份会话状态(纯内存; 结构体是 POD, 无指针成员)
    std::memset(&g_s, 0, sizeof(g_s));
    g_s.curFreqErrCpu = -1;
    g_s.curFreqErrCpuB = -1;
    // 预热记录带构造(不是裸 POD), memset 之后显式重建一次
    for (int wi = 0; wi < kMaxWarmupPasses; ++wi) {
        g_s.warm[wi] = WarmPassRec();
    }
    // 可用核集合(0 = 读不到): 单核口径的"越界落核"判据与多核口径共用同一份读数
    g_s.allowedMask = auroraAllowedCpuMask();
    g_s.multi = (threads > 1) ? 1 : 0;
    g_s.tid = (long)::syscall(SYS_gettid);
    g_s.fallbackCpu = sched_getcpu();
    g_text[0] = '\0';
    // 预热标称频率表一次(在计时区间之外): 这样采样线程里对 auroraCpuMaxFreqKhz 的
    // 调用只是读一个已经建好的静态 vector, 不会在采样线程里触发首次初始化(那是会分配的)。
    (void)auroraCoreMaxFreqKhzCached();
    if (g_s.multi) {
        // 多核口径: 采样核 = 内核允许本进程使用的核集合(读不到则退到"实际使用集合",
        // 再读不到才退到"频率表里的全部核")—— 三级降级, 每一级都写进文本。
        unsigned long long m = auroraAllowedCpuMask();
        if (m == 0ull) {
            m = (unsigned long long)::auroraEffectiveSet().mask;
        }
        // 采样核集合的上界: 优先用频率表的长度(= 本机的核数); 频率表读不到时退到
        // hardware_concurrency(); 两者都拿不到才用 4 兜底。这样即使"可用核集合"与"频率表"
        // 双双读不到, 也不会去扫 64 个根本不存在的核(那会灌一堆 ENOENT 假读数)。
        const int tableN = (int)auroraCoreMaxFreqKhzCached().size();
        int hw = (int)std::thread::hardware_concurrency();
        if (hw <= 0) {
            hw = 4;
        }
        int limit = (tableN > 0) ? tableN : hw;
        if (limit > kMaxSampleCpus) {
            limit = kMaxSampleCpus;
        }
        if (m != 0ull) {
            for (int c = 0; c < limit; ++c) {
                if (((m >> (unsigned)c) & 1ull) == 0) {
                    continue;
                }
                if (g_s.coreCount >= kMaxSampleCpus) {
                    break;
                }
                g_s.cores[g_s.coreCount++] = c;
                g_s.coreMask |= (1ull << (unsigned)c);
                const int nm = auroraCpuMaxFreqKhz(c);
                if (nm > g_s.nominalMaxKhz) {
                    g_s.nominalMaxKhz = nm;
                }
            }
        }
        if (g_s.coreCount == 0) {
            for (int c = 0; c < limit; ++c) {
                g_s.cores[g_s.coreCount++] = c;
                g_s.coreMask |= (1ull << (unsigned)c);
                const int nm = auroraCpuMaxFreqKhz(c);
                if (nm > g_s.nominalMaxKhz) {
                    g_s.nominalMaxKhz = nm;
                }
            }
        }
    } else {
        g_s.nominalMaxKhz = auroraCpuMaxFreqKhz(g_s.fallbackCpu);
    }
    g_stop = false;
    g_s.armed = 0;
    g_sessionOpen = true;
    if (!g_thr.joinable()) {
        // 唯一的分配点(起线程), 且发生在计时区间之外。
        // 防御: 若上一次会话没有正常收尾(异常路径漏掉了 SessionEnd), 这里复用那个还活着的
        // 采样线程而不是再起一个 —— 它读的全是全局状态, 复用是安全的; 若它已经退出, 本项就会
        // 采到 0 个样本, 文本会写"没有样本"(不假装采到了)。
        // 这一句是最后一句: 它若抛异常(std::system_error/EAGAIN 或 bad_alloc), 线程对象
        // 根本没被构造出来, 全局 g_thr 仍是"未启动"状态, 不会留下 joinable 的线程。
        g_thr = std::thread(samplerMain);
    }
}

} // namespace

void auroraFreqSampleSessionBegin(int threads)
{
    // 这一层 try/catch 是"诊断不让负载失败"的兜底
    // 采样是旁路: 就算设备线程/内存资源紧张到起不了线程、或读盘路径抛了异常, 本项负载
    // 也必须照跑不误、分数一分不变。异常在这里被吃掉, 只把"起不来"写进文本(上报)。
    try {
        sessionBeginImpl(threads);
    } catch (...) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_sessionOpen = false;
        g_s.armed = 0;
        g_stop = true;   // 若线程其实已经起来, 让它自己退出
        std::snprintf(g_text, sizeof(g_text),
                      "运行时频率 未采到(采样会话初始化失败: std::thread 构造抛异常, "
                      "设备可能线程/内存资源紧张; 本项负载与分数不受影响)");
    }
}

void auroraFreqMarkStart(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sessionOpen) {
        return;
    }
    // 每一项单独计数: 上一项的样本与 errno 不带到下一项
    g_s.count = 0;
    g_s.overflowSamples = 0;
    g_s.countB = 0;
    g_s.overflowB = 0;
    g_s.ticks = 0;
    g_s.ticksA = 0;
    g_s.ticksANone = 0;
    g_s.workerCoreSum = 0;
    g_s.workerCoreMax = 0;
    g_s.workerTidFail = 0;
    g_s.workerTidOverflow = 0;
    g_s.outOfSetSamples = 0;
    g_s.outOfSetMask = 0ull;
    g_s.curFreqErrno = 0;
    g_s.curFreqErrCpu = -1;
    g_s.curFreqOk = 0;
    g_s.curFreqFail = 0;
    g_s.curFreqErrnoB = 0;
    g_s.curFreqErrCpuB = -1;
    g_s.curFreqOkB = 0;
    g_s.curFreqFailB = 0;
    g_s.threadStatErrno = 0;
    g_s.threadStatOk = 0;
    g_s.threadStatFail = 0;
    g_s.thermalErrno = 0;
    g_s.thermalOk = 0;
    g_s.thermalFail = 0;
    g_s.thermalMaxMilli = 0;
    g_s.thermalMinMilli = 0;
    g_s.stopped = 0;
    // 池线程 tid 登记表: 本项计时区间从空表开始(池线程在 t0 之后才会登记自己)。
    // 只在没有任何池线程的时刻做这件事(池是负载在 t0 之后建的), 因此不存在与登记的竞争。
    (void)g_workerTidOverflow.exchange(0, std::memory_order_relaxed);
    (void)g_workerTidSeen.exchange(0, std::memory_order_relaxed);
    (void)g_workerTidPeak.exchange(0, std::memory_order_relaxed);
    for (int wi = 0; wi < kMaxWorkerTids; ++wi) {
        g_workerTidSlots[wi].store(0, std::memory_order_release);
    }
    g_s.workerTidCount = 0;
    // 逐核占用的"区间起点"(在 t0 之前执行, 不进 o.ms): 与 markStop 那次读盘一减,
    // 就得到"本项计时区间内每个核有多少时间在干活" —— 这是"每颗核都跑满"最直接的证据。
    (void)readProcStatSnapshot(&g_s.statStart);
    g_s.armed = 1;
    ++g_markGen;          // 见 g_markGen 的注释: 让采样线程立刻醒来采第一个样本
    g_cv.notify_all();
}

void auroraFreqMarkStop(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_sessionOpen) {
        return;
    }
    // 逐核占用的"区间终点"(在 t1 之后执行, 不进 o.ms)
    (void)readProcStatSnapshot(&g_s.statStop);
    // 本项计时区间里登记到的池线程 tid 数(口径A 的覆盖范围证据)
    {
        // 覆盖范围证据取峰值而不是收尾那一刻: 池线程在 join 时就注销了, 收尾那一刻
        // 表里通常已经空了 —— 拿收尾值会得出"一个池线程都没有"这种假结论。
        g_s.workerTidCount = g_workerTidPeak.load(std::memory_order_relaxed);
        g_s.workerTidOverflow = g_workerTidOverflow.load(std::memory_order_relaxed);
    }
    g_s.armed = 0;
    g_s.stopped = 1;
}

namespace {

void sessionEndImpl(void)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_sessionOpen) {
            // 幂等: 正常路径已经在负载返回后显式收尾过一次, RAII 析构里的第二次调用
            // (以及异常路径的调用)在这里直接返回 —— 不清空已经汇总好的文本。
            return;
        }
        g_s.armed = 0;
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_thr.joinable()) {
        g_thr.join();   // 采样线程已退出 -> 下面读 g_s 不再需要锁
    }
    // ---- 参考项: governor(在计时区间之后读一次; 它是"这台机器用什么调频策略"的静态事实) ----
    {
        int probeCpu = g_s.fallbackCpu;
        if (g_s.multi && g_s.coreCount > 0) {
            probeCpu = g_s.cores[0];
        } else if (!g_s.multi && g_s.count > 0) {
            probeCpu = g_s.cpuOf[g_s.count - 1];
        }
        if (probeCpu >= 0) {
            char path[96];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", probeCpu);
            char buf[32];
            const int e = readSmallText(path, buf, (int)sizeof(buf));
            if (e == 0) {
                size_t k = 0;
                while (buf[k] != '\0' && buf[k] != '\n' && buf[k] != '\r' && k + 1 < sizeof(g_s.governor)) {
                    g_s.governor[k] = buf[k];
                    ++k;
                }
                g_s.governor[k] = '\0';
                g_s.govErrno = 0;
            } else {
                g_s.govErrno = e;
            }
        }
    }
    // 频率上限取证(计时区间之外, 每个会话一次; 失败只会让这一段文本变短, 不影响分数)
    readFreqCapEvidence();
    buildText();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_sessionOpen = false;
    }
}

} // namespace

void auroraFreqSampleSessionEnd(void)
{
    // 同样兜异常: 本函数会在 ~AuroraFreqScope 里被调用, 而那个析构可能正处在
    // "负载抛了 bad_alloc" 的栈展开过程中 —— 那时再往外抛就是 std::terminate。
    // 汇总失败最多是"这一项没有频率文本", 不能升级成进程终止。
    try {
        sessionEndImpl();
    } catch (...) {
        // 什么都不做: 文本保持上一次的内容(或为空), 负载与分数不受影响
    }
}

int auroraFreqSampleText(char* buf, int cap)
{
    if (buf == nullptr || cap <= 0) {
        return 0;
    }
    buf[0] = '\0';
    const size_t n = std::strlen(g_text);
    if (n == 0) {
        return 0;
    }
    // 拷贝这一环也不许静默截断(2026-10): 调用方(gb7.cpp)给的缓冲若比 g_text 小, 原来的
    // memcpy(min(n, cap-1)) 会把尾部无声切掉。现在改成同一口径: 放得下逐字节照抄(正常
    // 路径与原来完全一致), 放不下就在尾部写"被截断"标记。gb7.cpp 侧的缓冲与 g_text 同容量,
    // 因此这条出口正常路径永不触发。
    if ((int)n >= cap - 1) {
        const char* const kTrunc =
            " (运行时频率文本超出容量上限, 尾部被截断 —— 这本身就是事实, 不许当成'取证完整')";
        const size_t truncLen = std::strlen(kTrunc);
        const size_t keepMax = (size_t)(cap - 1);
        const size_t keep = (keepMax > truncLen) ? (keepMax - truncLen) : 0;
        std::memcpy(buf, g_text, keep);
        std::memcpy(buf + keep, kTrunc, truncLen);
        buf[keep + truncLen] = '\0';
        return (int)(keep + truncLen);
    }
    const size_t copy = n;
    std::memcpy(buf, g_text, copy);
    buf[copy] = '\0';
    return (int)copy;
}

// ---------------------------------------------------------------------------
//  最近一次会话的派生量 + 「本机性能天花板」一行(实现; 口径见 cpu_freq_sample.h)
// ---------------------------------------------------------------------------
//  全部是只读快照, 不重算、不缓存第二份: 取的就是 buildText() 用的那三条数组
//  (khz / nomOf / count 一一对应), 所以这一行与 runFreq 那一行不可能互相矛盾。
int auroraFreqLastSampleCount(void)
{
    return (g_s.count > 0) ? g_s.count : 0;
}

int auroraFreqLastMedianKhz(void)
{
    if (g_s.count <= 0) {
        return 0;
    }
    return medianOfInts(g_s.khz, g_s.count, g_sortScratch, kMaxSamples);
}

int auroraFreqLastNominalMedianKhz(void)
{
    if (g_s.count <= 0) {
        return 0;
    }
    return medianOfInts(g_s.nomOf, g_s.count, g_sortScratch, kMaxSamples);
}

int auroraFreqLastMedianRatioPermille(void)
{
    if (g_s.count <= 0) {
        return 0;
    }
    return medianRatioPermille(g_s.khz, g_s.nomOf, g_s.count, g_ratioScratch, kMaxSamples);
}

int auroraFreqLastWasMulti(void)
{
    return g_s.multi ? 1 : 0;
}

// 「本机性能天花板」一行: 把"这颗芯片在本 App 里到底能被压到哪里"写成一句可核对的话。
//  三个事实必须同时出现, 缺一个这句话就会被误读:
//    ① 全机最快档在哪几核多少 MHz  —— "芯片有更快的地方";
//    ② 内核实测允许我们用哪几核  —— "系统不放我们进去";
//    ③ 因此生效快簇是哪几核多少 MHz + 多核最多几线程 + 运行时频率占标称多少
//                                   —— "所以我们实际上只能到这里"。
//  措辞一律"是几就是几": 读不到就写读不到, 不用 0 冒充, 也不写"大概/应该"。
int auroraCeilingText(char* buf, int cap)
{
    if (buf == nullptr || cap <= 0) {
        return 0;
    }
    buf[0] = '\0';
    const int topCores = ::auroraMachineTopTierCoreCount();
    const int topKhz = ::auroraMachineTopTierMaxKhz();
    const int allowedCores = ::auroraAllowedCoreCount();
    const int allowedKnown = ::auroraAllowedKnown();
    const int fastCores = ::auroraFastClusterCoreCount();
    const int fastKhz = ::auroraFastClusterMaxKhz();
    const int tier = ::auroraFastClusterTierIndex();
    const int threads = ::auroraSmtThreadsCapped();
    char topList[64];
    char allowList[64];
    char fastList[32];
    aurora_cpu_detail::maskToCpuList(::auroraMachineTopTierMask(), 64, topList, (int)sizeof(topList));
    aurora_cpu_detail::maskToCpuList(::auroraAllowedCpuMask(), 64, allowList, (int)sizeof(allowList));
    aurora_cpu_detail::maskToCpuList(::auroraFastClusterMask(), 64, fastList, (int)sizeof(fastList));
    std::string s = "本机性能天花板：";
    if (topCores > 0 && topKhz > 0) {
        s += "全机最快档 " + mhzText(topKhz) + " 在 " +
             std::string((topList[0] != '\0') ? topList : "读不到") + "(" +
             std::to_string(topCores) + " 核)";
    } else {
        s += "全机最快档读不到(逐核 cpuinfo_max_freq 全是 0 或读不到)";
    }
    s += "；";
    if (allowedKnown && allowedCores > 0) {
        s += "内核实测只放我们用 " +
             std::string((allowList[0] != '\0') ? allowList : "读不到") + "(" +
             std::to_string(allowedCores) + " 核)";
    } else {
        s += "内核允许的核集合读不到(绑核因此不做过滤)";
    }
    s += "；";
    if (fastCores > 0 && fastKhz > 0) {
        s += "所以单核最高只能跑在 " + mhzText(fastKhz) + " 这一档(" +
             std::string((fastList[0] != '\0') ? fastList : "读不到") + "，" +
             std::to_string(fastCores) + " 核";
        if (tier > 0) {
            s += "，比全机最快档低 " + std::to_string(tier) + " 档";
        } else {
            s += "，就是全机最快档里我们能用的那部分";
        }
        s += ")";
    } else {
        s += "生效快簇未定义(频率表与可用核集合至少一个读不到)，单核阶段不绑核";
    }
    s += "；多核最多 " + std::to_string(threads) + " 个线程";
    if (g_s.count > 0) {
        const int med = medianOfInts(g_s.khz, g_s.count, g_sortScratch, kMaxSamples);
        const int nomMed = medianOfInts(g_s.nomOf, g_s.count, g_sortScratch, kMaxSamples);
        const int ratio = medianRatioPermille(g_s.khz, g_s.nomOf, g_s.count, g_ratioScratch,
                                              kMaxSamples);
        if (nomMed > 0 && ratio > 0) {
            s += "；负载跑起来时运行时频率中位 " + mhzText(med) + " = 标称 " + mhzText(nomMed) +
                 " 的 " + std::to_string(ratio / 10) + "." + std::to_string(ratio % 10) + "%";
            if (ratio >= kFullRatioPct * 10) {
                s += "(已达我们自定的跑满判据 " + std::to_string(kFullRatioPct) + "%)";
            } else {
                const int gap = kFullRatioPct * 10 - ratio;
                s += "(未达我们自定的跑满判据 " + std::to_string(kFullRatioPct) + "%，还差 " +
                     std::to_string(gap / 10) + "." + std::to_string(gap % 10) + " 个百分点)";
            }
        } else {
            s += "；运行时频率采到了但标称上限读不到，占比算不出来";
        }
        s += "。";
    } else {
        s += "；本轮还没有采到运行时频率样本(跑过任意一项之后这一句才会有数)。";
    }
    const int n = (int)s.size();
    if (n >= cap) {
        const char* const kTrunc = " (本行超出容量上限, 尾部被截断)";
        const int keep = cap - 1 - (int)std::strlen(kTrunc);
        if (keep <= 0) {
            buf[0] = '\0';
            return 0;
        }
        std::memcpy(buf, s.c_str(), (size_t)keep);
        std::memcpy(buf + keep, kTrunc, std::strlen(kTrunc));
        buf[cap - 1] = '\0';
        return (int)std::strlen(buf);
    }
    std::memcpy(buf, s.c_str(), (size_t)n);
    buf[n] = '\0';
    return n;
}

// ---------------------------------------------------------------------------
//  升频/稳态预热 + 池线程 tid 登记(实现; 口径与判据见 cpu_freq_sample.h)
// ---------------------------------------------------------------------------
void auroraWarmupPassBegin(void)
{
    // 与其它对外接口一样兜异常: 预热是旁路能力, 它出问题也不能让负载失败
    try {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_sessionOpen) {
            return;
        }
        g_warmPassStartMs = steadyNowMs();
        // 清空本遍的采样窗口(纯内存)。正式计时那一次还会被 markStart 再清一次, 因此
        // 预热遍的样本不可能流进 o.ms 所对应的那一份读数里。
        g_s.count = 0;
        g_s.overflowSamples = 0;
        g_s.countB = 0;
        g_s.overflowB = 0;
        g_s.ticks = 0;
        g_s.ticksA = 0;
        g_s.ticksANone = 0;
        g_s.workerCoreSum = 0;
        g_s.workerCoreMax = 0;
        g_s.workerTidFail = 0;
        g_s.outOfSetSamples = 0;
        g_s.outOfSetMask = 0ull;
        g_s.stopped = 0;
        g_s.workerTidCount = 0;
        g_s.workerTidOverflow = 0;
        (void)g_workerTidOverflow.exchange(0, std::memory_order_relaxed);
        (void)g_workerTidSeen.exchange(0, std::memory_order_relaxed);
        (void)g_workerTidPeak.exchange(0, std::memory_order_relaxed);
        for (int wi = 0; wi < kMaxWorkerTids; ++wi) {
            g_workerTidSlots[wi].store(0, std::memory_order_release);
        }
    } catch (...) {
        // 什么都不做: 本遍预热照跑, 读数按"没有样本"上报
    }
}

void auroraWarmupPassEnd(void)
{
    try {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_sessionOpen) {
            return;
        }
        int ms = (int)(steadyNowMs() - g_warmPassStartMs);
        if (ms > 0) {
            g_s.warmTotalMs += ms;
        }
        const int idx = g_s.warmPasses;
        WarmPassRec* w = (idx >= 0 && idx < kMaxWarmupPasses) ? &g_s.warm[idx] : nullptr;
        const int n = g_s.count;
        if (w != nullptr) {
            w->used = 1;
            w->count = n;
            w->ms = ms;
            w->windowClosed = g_s.stopped;
            w->workerTidCount = g_s.workerTidCount;
            unsigned long long seen = 0ull;
            int cores = 0;
            for (int i = 0; i < n; ++i) {
                const int c = g_s.cpuOf[i];
                if (c >= 0 && c < 64 && ((seen >> (unsigned)c) & 1ull) == 0ull) {
                    seen |= (1ull << (unsigned)c);
                    ++cores;
                }
            }
            w->cores = cores;
            if (n > 0) {
                w->medKhz = medianOfInts(g_s.khz, n, g_sortScratch, kMaxSamples);
                w->loKhz = g_s.khz[0];
                w->hiKhz = g_s.khz[0];
                int rLo = 0, rHi = 0, rN = 0;
                for (int i = 0; i < n; ++i) {
                    if (g_s.khz[i] < w->loKhz) { w->loKhz = g_s.khz[i]; }
                    if (g_s.khz[i] > w->hiKhz) { w->hiKhz = g_s.khz[i]; }
                    if (g_s.nomOf[i] > 0 && g_s.khz[i] > 0) {
                        const int r = (int)(((long long)g_s.khz[i] * 1000LL +
                                             (long long)g_s.nomOf[i] / 2) / (long long)g_s.nomOf[i]);
                        if (rN == 0 || r < rLo) { rLo = r; }
                        if (rN == 0 || r > rHi) { rHi = r; }
                        ++rN;
                    }
                }
                w->loRatioPermille = rLo;
                w->hiRatioPermille = rHi;
                w->medRatioPermille = medianRatioPermille(g_s.khz, g_s.nomOf, n,
                                                          g_ratioScratch, kMaxSamples);
                const int half = n / 2;
                if (half > 0) {
                    w->firstHalfMedKhz = medianOfInts(g_s.khz, half, g_sortScratch, kMaxSamples);
                    w->secondHalfMedKhz = medianOfInts(g_s.khz + (n - half), half,
                                                       g_sortScratch, kMaxSamples);
                }
            }
        }
        ++g_s.warmPasses;
        g_s.warmSteady = 0;
        g_s.warmSteadyHow = 0;
        if (n < kWarmMinSamples || w == nullptr) {
            // 这一遍根本没有样本 -> 不参与稳态判定, 计数
            ++g_s.warmNoSamplePasses;
            if (g_s.warmPasses >= kMaxWarmupPasses) {
                g_s.warmStoppedBy = 3;
            } else if (g_s.warmTotalMs >= kWarmupMaxTotalMs) {
                g_s.warmStoppedBy = 2;
            }
            return;
        }
        // ---- 稳态判据(三条, 任一条命中即判稳态; 阈值全是我们自己定的) ----
        const bool reachedFull = (w->medRatioPermille >= kFullRatioPct * 10);
        bool byTwoPass = false;
        if (idx >= 1 && g_s.warm[idx - 1].used != 0 && g_s.warm[idx - 1].count >= kWarmMinSamples) {
            const int pm = g_s.warm[idx - 1].medKhz;
            if (pm > 0) {
                const long long d = (long long)w->medKhz - (long long)pm;
                const long long ad = (d < 0) ? -d : d;
                byTwoPass = (ad * 100LL <= (long long)kSteadyDeltaPct * (long long)pm);
            }
        }
        bool byTwoHalf = false;
        if (n >= kWarmHalfSamples && w->firstHalfMedKhz > 0 && w->secondHalfMedKhz > 0) {
            const long long d = (long long)w->secondHalfMedKhz - (long long)w->firstHalfMedKhz;
            const long long ad = (d < 0) ? -d : d;
            byTwoHalf = (ad * 100LL <= (long long)kSteadyDeltaPct * (long long)w->firstHalfMedKhz);
        }
        if (reachedFull) {
            g_s.warmSteady = 1;
            g_s.warmSteadyHow = 1;
        } else if (byTwoPass) {
            g_s.warmSteady = 1;
            g_s.warmSteadyHow = 2;
        } else if (byTwoHalf) {
            g_s.warmSteady = 1;
            g_s.warmSteadyHow = 3;
        }
        if (g_s.warmSteady == 0) {
            if (g_s.warmPasses >= kMaxWarmupPasses) {
                g_s.warmStoppedBy = 1;
            } else if (g_s.warmTotalMs >= kWarmupMaxTotalMs) {
                g_s.warmStoppedBy = 2;
            }
        }
    } catch (...) {
        // 同上: 预热取证失败不影响负载与分数
    }
}

int auroraWarmupSteady(void)
{
    return g_s.warmSteady;
}

int auroraWarmupPassesDone(void)
{
    return g_s.warmPasses;
}

int auroraWarmupTotalMs(void)
{
    return g_s.warmTotalMs;
}

int auroraWarmupMaxPasses(void)
{
    return kMaxWarmupPasses;
}

int auroraWarmupMaxTotalMs(void)
{
    return kWarmupMaxTotalMs;
}

// 池线程登记自己的 tid。无锁、无分配、可在计时区间内安全调用(见文件头说明)。
int auroraFreqRegisterWorkerTid(void)
{
    const long t = (long)::syscall(SYS_gettid);
    if (t <= 0 || t > 2147483647L) {
        return -1;
    }
    const int v = (int)t;
    for (int i = 0; i < kMaxWorkerTids; ++i) {
        int expect = 0;
        if (g_workerTidSlots[i].compare_exchange_strong(expect, v,
                                                       std::memory_order_acq_rel)) {
            g_workerTidSeen.fetch_add(1, std::memory_order_relaxed);
            // 峰值更新(无锁 CAS 循环; 竞争很轻, 且不影响正确性)
            int peak = g_workerTidPeak.load(std::memory_order_relaxed);
            for (;;) {
                int live = 0;
                for (int k = 0; k < kMaxWorkerTids; ++k) {
                    if (g_workerTidSlots[k].load(std::memory_order_relaxed) != 0) {
                        ++live;
                    }
                }
                if (live <= peak) {
                    break;
                }
                if (g_workerTidPeak.compare_exchange_weak(peak, live,
                                                         std::memory_order_relaxed)) {
                    break;
                }
            }
            return v;
        }
    }
    g_workerTidOverflow.fetch_add(1, std::memory_order_relaxed);
    return -1;
}

// 池线程干完活(即将退出)时注销: 让表里留下的永远是活着的负载线程。
void auroraFreqUnregisterWorkerTid(void)
{
    const long t = (long)::syscall(SYS_gettid);
    if (t <= 0 || t > 2147483647L) {
        return;
    }
    const int v = (int)t;
    for (int i = 0; i < kMaxWorkerTids; ++i) {
        int expect = v;
        if (g_workerTidSlots[i].compare_exchange_strong(expect, 0,
                                                       std::memory_order_acq_rel)) {
            return;
        }
    }
}

int auroraFullRatioPercent(void)
{
    return kFullRatioPct;
}

// 预热的一行摘要(与 runFreq 里那段 ③b 同源, 只是不带逐遍明细)。
// 用途: 预热每个 (负载 id, 阶段) 只做一次, 第 2..N 轮的报告里要引用第 1 轮的结果 —— 引用的是
// 同一份读数的摘录, 不是另算一遍, 所以两边永远不会自相矛盾。
int auroraWarmupSummaryText(char* buf, int cap)
{
    if (buf == nullptr || cap <= 0) {
        return 0;
    }
    buf[0] = '\0';
    std::string x;
    appendWarmText(x, true);
    if (x.empty()) {
        return 0;
    }
    const size_t n = x.size();
    const size_t copy = ((int)n < cap - 1) ? n : (size_t)(cap - 1);
    std::memcpy(buf, x.data(), copy);
    buf[copy] = '\0';
    return (int)copy;
}

int auroraFreqCapText(char* buf, int cap)
{
    if (buf == nullptr || cap <= 0) {
        return 0;
    }
    buf[0] = '\0';
    const size_t n = std::strlen(g_s.capText);
    if (n == 0) {
        return 0;
    }
    const size_t copy = ((int)n < cap - 1) ? n : (size_t)(cap - 1);
    std::memcpy(buf, g_s.capText, copy);
    buf[copy] = '\0';
    return (int)copy;
}

