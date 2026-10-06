#ifndef AURORA_CPU_AFFINITY_H
#define AURORA_CPU_AFFINITY_H

// ===========================================================================
//  统一的 CPU 亲和性(大核簇优先)实现 —— 旧 7 项自研套件与 CS1 16 项负载共用同一份
// ===========================================================================
//
//  为什么需要它
//  ---------------------------------------------------------------------------
//  带大小核的手机 SoC 上"线程落在哪个核"完全由系统调度器决定: 同一个负载这次可能跑在
//  2.4 GHz 的超大核、下次跑在 1.6 GHz 的小核, 同一份构建下同一个负载实测耗时能差到
//  1.8 倍(真机观察到 Photo Editor 单核 1761 ms 与 3212 ms 两种结果)。更糟的是
//  "多核加速比 = 多核吞吐 / 单核吞吐": 分母(单核)随机落到小核时, 比值会被凭空放大
//  数倍 —— 真机截图里出现过物理上不可能解释的 58.4x(14 核机器的上限是 14x)。
//  把"跑负载的线程"限制在频率最高的那批核上, 消掉的是调度随机性, 不是工作量。
//
//  为什么是"大核簇"而不是"最快的单个核"(2026-10 真机证据驱动的改动)
//  ---------------------------------------------------------------------------
//  第一版把线程钉在唯一一个核上(sched_setaffinity 的掩码里只有一位)。
//  真机 14 核机的 runlog 显示: sched_setaffinity 返回成功、cpuBound=true, 但 16 项里
//  有 5 项在负载跑完时线程已经不在开跑的那个核上 —— 每一项的 note 都是
//  "cpu=8(位次4/最高2270MHz) 已绑定 · 注意: 跑在非最快核(位次4)", 汇总行还写着
//  "有 5 项绑核后线程被系统挪走(起于 cpu4 -> 结束在 cpu7)"。
//  即: 内核/厂商调度器推翻了单核掩码。典型机制是 EAS 的 misfit 判定 —— 一个满
//  负载线程被限制在单核上时, 该核的 capacity 装不下它的 utilization, 调度器就会把它
//  迁走(另一个常见机制是系统在跑分/前台场景下主动做核间均衡)。注意被挪动的两端
//  (cpu4 -> cpu7)频率档位相同, 说明调度器实际上是在同一个频率档内自由均衡。
//  因此本文件把"绑核"的粒度从"单个核"改成"快簇: 频率最高的一批核组成的集合":
//    * 小核仍然进不来 -> 仍然消掉"随机落到小核"这个方差来源(原始目的不变);
//    * 快簇内所有核同频, 内核在簇内怎么挪都不改变性能 -> 不再触发 misfit 强行迁移;
//    * 簇的大小由机器自己的频率表决定, 没有任何设备相关系数进入计分公式。
//
//  快簇的定义(纯离线规则, 只读 cpuinfo_max_freq)
//  ---------------------------------------------------------------------------
//    1) 频率表按 cpuinfo_max_freq 降序(同频保持原核号顺序); 频率相同的核并成一个"频率档";
//    2) 从最高档开始贪心地往一个组里吸收频率档, 直到该组至少含 2 个核 —— 这个组就是
//       "快簇": 最高档本身 >= 2 个核时, 快簇 = 最高档的全部核(例: 4 个大核同频 2400 MHz
//       -> 快簇 = 这 4 个); 最高档只有 1 个核时, 继续吸收次高档(于是快簇 = 最高档 +
//       次高档, >= 2 个核), 避免又退化成"单核钉死";
//    3) 同一条规则继续往下扫描, 就得到整机的"均衡组"划分(每组 >= 2 个核; 只有
//       单核机器才会出现 1 个核的组)。多核阶段的池线程按均衡组轮流分配, 而不是按
//       单个核分配: 位次 i 的核属于哪个组, 线程 i 就拿哪个组(频率降序 -> 大核优先),
//       内核在该组内自由均衡。这样既保住了"大核优先 + 线程分散到全机", 又消除了单核
//       掩码被系统推翻的问题。
//       为什么是"位次优先"而不是"按组容量比例分配"(2026-10 定案, 不要改): 本 App 的
//       多核阶段 threads = cpuCoreCount()(真机 14), 池线程数 >= 核数, 于是每个均衡组都会
//       被占满、所有核都被用上, "线程数 < 组容量"这条分支在实践中根本不触发。改成按容量
//       比例分配只会多出一条没人验证过的路径(以及随之而来的 8 线程/14 核之类的边角行为),
//       收益为零, 因此明确不做;
//    4) 频率表读不到(打不开 sysfs)或最高频率解析结果 <= 0 -> 快簇未定义, 全程不绑,
//       静默降级(见下)。
//  这套划分是纯函数: 只依赖 sysfs 里每个核的最高频率, 与设备型号、核数、当前负载、
//  当前温度都无关, 也没有任何人为阈值(不是"取前 4 个", 也不是"频率 > 2000 MHz")。
//
//  跨平台一致性声明(必须保持, 改动前先读这一条)
//  ---------------------------------------------------------------------------
//  绑核只做一件事: 把线程限制在"频率最高的若干个核组成的集合"里。它**不改变任何负载的
//  工作量、数据规模、内存布局、循环次数, 也不引入任何设备相关系数** —— 计分公式里
//  没有任何一项来自这里, 任何设备上跑的都是同一份负载、同一份常量。读不到频率信息时
//  (权限不足 / sysfs 路径不存在 / 容器里没有 cpufreq / 频率解析结果全为 0)本文件
//  静默降级为"不绑、不报错", 负载照常跑, 只是结果里带着"未绑定"这一事实
//  (见 AuroraCpuPlacement::bound), 供事后判读。
//
//  旧套件(bench_cpu.cpp)为什么不受影响
//  ---------------------------------------------------------------------------
//  旧 7 项套件刻意保持改动前的逐核绑定行为: 它只调用 auroraReadCoreMaxFreqKhz /
//  auroraFastCoreList / auroraBindCurrentThreadToCpu 三个老接口, 这三个接口的语义与实现
//  一字未动(逐次读盘、同频保持原序、单核绑定), 因此旧套件的分数与改动前零差异。
//  簇粒度只用在 GB7 会话路径(auroraAffinitySessionBegin / auroraAffinityWorkerStart)。
//
//  预期效果(日后核对用)
//  ---------------------------------------------------------------------------
//   1) 单核结果应当更稳定且更高: 不再随机落到小核。同机重复跑同一项的耗时离散度
//      应显著下降(此前同一构建下同一项能差到 1.8 倍, 绑核后应收敛到几个百分点)。
//   2) "绑核后线程被系统挪走"的项数应当归零: 判据从"跑完的 cpu == 开跑的 cpu"改成
//      "跑完的 cpu 属于快簇"(结构化字段 cpuInFastCluster)。簇内迁移是设计允许的。
//   3) 多核加速比应当回落到物理合理的区间: 本机 14 核, 内存密集型负载预期 3~9x。
//      若仍出现 >14x 或 <1x, 说明问题不在亲和性, 而在分子/分母口径或负载分解。
//
//  计时口径(绑核必须在计时区间之外)
//  ---------------------------------------------------------------------------
//  * 会话在负载函数被调用之前建立(auroraAffinitySessionBegin): 该函数只做
//    "读一次 sysfs + 对当前线程一次 sched_setaffinity(+ 一次 sched_yield)", 完全发生在
//    负载自己的 t0 之前, 其开销一分钱都不会进 o.ms。
//  * 池线程是 gb7ParallelFor 在计时区间内创建的, 每个池线程在执行任何负载工作之前
//    绑定自己一次(一次 syscall ≈1~3 µs, 与父线程的 clone 完全重叠), 详见 gb7_parallel.h。
//  * 会话结束时(auroraAffinitySessionEnd, 同样在 t1 之后)把主线程的原始掩码还原:
//    napi 异步工作线程会被复用(后面还有 GPU7 / 其它异步任务), 不还原等于把整个异步
//    线程永久限制在一个小掩码里。
// ===========================================================================

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// 逐核"启动状态"取证(A/B/C/D 分类 + C 类拉起探测; 只读 sysfs + 逐核实测, 见该文件头)
#include "cpu_core_state.h"

// ===========================================================================
//  超线程(SMT)支持: CPU 拓扑读取(逻辑核 / 物理核)+ 多核阶段"实际使用集合"
// ===========================================================================
//
//  为什么需要它
//  ---------------------------------------------------------------------------
//  本 App 一直用 cpuCoreCount()(= std::thread::hardware_concurrency(), 即逻辑核数)
//  当多核阶段的线程数, 并且绑核逻辑按"频率档"分组 —— 全文没有任何地方区分逻辑核与物理核。
//  在可能带 SMT 的 SoC 上这就产生了歧义: 同一个"核数"既可能是逻辑核数, 也可能是物理核数 ——
//  实测到的是"逻辑核 14"(真机日志里 14 个可用核), 而物理核数只有读拓扑才知道。
//  本文件不引用任何第三方 SoC 资料, 也不写任何无法从设备上核实的型号字符串;
//  本节的职责只有一个: 读出这台机器的逻辑核 / 物理核关系, 并据此算出一个
//  "实际使用集合", 供多核阶段的线程数 / 绑核掩码 / 池线程落点使用。
//
//  三级降级(读不到就说读不到, 不假装)
//  ---------------------------------------------------------------------------
//   ① thread_siblings_list: 逐核读 /sys/devices/system/cpu/cpuN/topology/thread_siblings_list
//      (形如 "0-1" / "0,4" / "0-3,8-11"), 同组的逻辑核 = 同一个物理核上的硬件线程。
//      只要至少两个核读到了非空且各含 >= 1 个核的兄弟表, 就以它为准 ——
//      缺失的那些核各自单独成组(会把物理核数算多, 因此不会凭空造出 SMT);
//   ② core_id + physical_package_id: ①读不到(整片 sysfs 不可访问)时退而求其次, 逐核读
//      /sys/devices/system/cpu/cpuN/topology/{core_id,physical_package_id}, 用
//      "(package_id, core_id) 相同 => 同一个物理核"判断。两个文件都读到了才承认这一级;
//   ③ 未知: ①②都读不到。此时标记为拓扑未知, 逻辑核数与物理核数按相等(1:1)处理,
//      来源字符串写成 "未知(已按 1:1 处理, SMT 开关无效)" —— 界面上会原样显示这句话。
//      不假装"检测到 SMT", 也不假装"没有 SMT"。
//
//  逻辑核数从哪来(顺序固定, 每一步都受 ①②③ 的取值影响)
//  ---------------------------------------------------------------------------
//   (a) 频率表长度(readCoreMaxFreqKhz 从 cpu0 顺序读到第一个打不开的编号为止;
//       它与 /sys/devices/system/cpu/present 在本工程里实测一致, 且是所有既有绑核逻辑
//       (fastCpus / rank / 均衡组)的编号空间, 用它可以保证"拓扑的核号"与"绑核的核号"
//       永远是同一个编号空间);
//   (b) 频率表读不到时, 改用 /proc/cpuinfo 里 processor 行的条数(容器/权限受限时最稳);
//   (c) 都没有则按 0 处理(调用方看到的就是"拓扑未知且一个核都没有", 不会绑错)。
//
//  设计约束(必须保持)
//  ---------------------------------------------------------------------------
//  * 本节只读: 三个 sysfs 文件 + /proc/cpuinfo, 进程内只读一次; 不写任何文件、
//    不改任何负载的工作量 / 数据规模 / 循环次数 / 计分公式, 不引入任何设备相关系数。
//  * "实际使用集合"只决定用哪些核、用几个线程, 不改变任何负载跑的是什么。
//  * SMT 关掉时不会把两个池线程绑到同一个物理核: 落点分配按"物理核"分派(见
//    buildWorkerTopology / bindWorkerGroup), 且线程数被夹到实际使用集合的大小。
// ===========================================================================

// 拓扑来源(哪一级读到的)
enum {
    AURORA_TOPO_SRC_UNKNOWN = 0,   // ③ 未知: 逻辑核数与物理核数按 1:1 处理
    AURORA_TOPO_SRC_SIBLINGS = 1,  // ① thread_siblings_list
    AURORA_TOPO_SRC_CORE_ID = 2    // ② core_id + physical_package_id
};

// 拓扑上限。与 readCoreMaxFreqKhz() 的扫描上限(32)保持一致; 数组字段全用定长数组,
// 没有任何动态分配(池线程 / 计时区间内都允许安全调用)。
constexpr int kMaxTopoCpus = 32;

// 读到的 CPU 拓扑(纯只读数据, 进程内只构造一次)
struct AuroraCpuTopoInfo {
    int logical;                        // 逻辑核数(硬件线程数)
    int physical;                       // 物理核数(>= 1 时才有意义; logical<=0 时为 0)
    int smtPossible;                    // 物理核数 < 逻辑核数 => 1(存在 SMT), 否则 0
    int source;                         // AURORA_TOPO_SRC_*
    int known;                          // 1 = 拓扑已知(①②), 0 = 未知(③, 已按 1:1 处理)
    int coreId[kMaxTopoCpus];           // 每个逻辑核的 core_id(-1 = 读不到)
    int packageId[kMaxTopoCpus];        // 每个逻辑核的 physical_package_id(-1 = 读不到)
    int physicalOfCpu[kMaxTopoCpus];    // 每个逻辑核所属的物理核编号(0 起)
    // ---- 逐路径实测结果(可核对的事实: 哪个文件在 App 的 SELinux 域下读到了, 哪个没读到) ----
    int siblingsRead;                   // 读到 thread_siblings_list 的逻辑核个数
    int siblingsErrno;                  // 一个都没读到时的 errno(0 = 全部读到; 13 = EACCES 权限不足)
    int coreIdRead;                     // 读到 core_id 的逻辑核个数
    int coreIdErrno;                    // 一个都没读到时的 errno
    int pkgIdRead;                      // 读到 physical_package_id 的逻辑核个数
    int pkgIdErrno;                     // 一个都没读到时的 errno
    int freqRead;                       // 读到 cpuinfo_max_freq 的逻辑核个数(= 频率表长度)
    int freqErrno;                      // 一个都没读到时的 errno
    int procCpuinfoOk;                  // /proc/cpuinfo 是否可读(1/0)
    int procCpuinfoErrno;               // 不可读时的 errno
    int possibleRead;                   // /sys/devices/system/cpu/possible 是否可读(1/0)
    int possibleErrno;                  // 不可读时的 errno
    int presentRead;                    // /sys/devices/system/cpu/present 是否可读(1/0)
    int presentErrno;                   // 不可读时的 errno
    char sourceText[96];                // 来源说明(中文, 可直接显示在界面上)
    char probeText[512];                // 逐路径一行文本(成功/失败 + errno; 可直接显示)
    AuroraCpuTopoInfo()
        : logical(0), physical(0), smtPossible(0),
          source(AURORA_TOPO_SRC_UNKNOWN), known(0), coreId(), packageId(), physicalOfCpu(),
          siblingsRead(0), siblingsErrno(0), coreIdRead(0), coreIdErrno(0),
          pkgIdRead(0), pkgIdErrno(0), freqRead(0), freqErrno(0),
          procCpuinfoOk(0), procCpuinfoErrno(0), possibleRead(0), possibleErrno(0),
          presentRead(0), presentErrno(0), sourceText(), probeText()
    {
    }
};

// 实际使用集合(SMT 开关作用在这里)
struct AuroraCpuEffectiveSet {
    int count;                          // 集合大小 = 多核阶段应当使用的线程数
    int cpus[kMaxTopoCpus];             // 集合里的逻辑核编号(频率降序, 同频按核号升序)
    int physical[kMaxTopoCpus];         // 每个成员对应的物理核编号
    unsigned long long mask;            // 成员位图(只覆盖前 64 个核)
    int smtEnabled;                     // 计算这份集合时的开关状态(0/1)
    int topologyKnown;                  // 拓扑是否已知(未知 => 按 1:1, 开关无效)

    AuroraCpuEffectiveSet()
        : count(0), cpus(), physical(), mask(0), smtEnabled(1), topologyKnown(0)
    {
    }
};

// 前置声明(文件作用域): SMT 开关状态 + "实际使用集合"的重算 + 几个只依赖这两者的独立函数。
// 它们的定义在文件末尾的公开接口区, 而 aurora_smt_detail / aurora_cpu_detail 里的缓存都要
// 用到, 所以在这里先声明。注意: 声明在文件作用域, 从命名空间里调用时必须写全局限定
// (::auroraSmtEnabledFlag() 等); 反过来, aurora_cpu_detail 里的代码要拿拓扑 / 集合 /
// 线程数上限时, 一律直接调 ::aurora_smt_detail 下的实现, 不经过公开包装。
// 频率表缓存与"频率降序"位次表缓存: 定义在 aurora_cpu_detail 里, 但拓扑读取(它要用频率表长度
// 确定逻辑核数)在 aurora_smt_detail, 两者互相需要, 所以按 C++ 的单遍规则在这里先声明。
namespace aurora_cpu_detail {
const std::vector<int>& coreMaxFreqKhzCached();
const std::vector<int>& perfOrderCached();
}

bool auroraSmtEnabledFlag();
void auroraRebuildEffectiveSet();
int auroraSmtEnabled();
int auroraSmtThreads();
int auroraSmtThreadsFor(int requested);
const AuroraCpuTopoInfo& auroraSmtTopology();
const AuroraCpuEffectiveSet& auroraEffectiveSet();
int auroraThreadCap();

namespace aurora_smt_detail {

// ---- 极简 sysfs 读取: 读不到就是读不到, 不打印、不抛异常 ----
// errOut(可空): 打开失败时回传 errno(EACCES = 13 权限不足, ENOENT = 2 路径不存在)。
// 记 errno 不是为了重试, 而是为了把"到底是没权限还是没这个文件"写进诊断文本 ——
// 用户用 hdc shell 读这些路径会被拒, 而 App 自己的 SELinux 域可能读得到, 两者的差异必须可见。
inline bool readTextFile(const std::string& path, char* buf, int cap, int* errOut = nullptr)
{
    if (errOut != nullptr) {
        *errOut = 0;
    }
    if (cap <= 1) {
        return false;
    }
    buf[0] = '\0';
    errno = 0;
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) {
        if (errOut != nullptr) {
            *errOut = errno != 0 ? errno : -1;
        }
        return false;
    }
    const size_t n = fread(buf, 1, (size_t)(cap - 1), f);
    fclose(f);
    buf[n] = '\0';
    if (n == 0 && errOut != nullptr) {
        *errOut = -1;   // 打得开但读不到内容(空文件 / 读失败): 也记下来
    }
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') {
            buf[i] = ' ';
        }
    }
    return true;
}

// 解析 cpulist 掩码("0-1" / "0,4" / "0-3,8-11"); 返回写进 out[] 的核数。
// out[] 是"或"进去的, 调用方负责先清零。最多 kMaxTopoCpus 个, 更靠后的位直接忽略。
inline int parseCpuMask(const char* s, int* out, int nOut)
{
    if (s == nullptr) {
        return 0;
    }
    int n = 0;
    int i = 0;
    while (s[i] != '\0' && s[i] != '\n' && s[i] != '\r') {
        if (s[i] < '0' || s[i] > '9') {
            ++i;
            continue;
        }
        int a = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            a = a * 10 + (s[i] - '0');
            if (a > 4096) {
                a = 4096;
            }
            ++i;
        }
        int b = a;
        if (s[i] == '-') {
            ++i;
            b = 0;
            while (s[i] >= '0' && s[i] <= '9') {
                b = b * 10 + (s[i] - '0');
                if (b > 4096) {
                    b = 4096;
                }
                ++i;
            }
        }
        if (b < a || a > 4096) {
            continue;   // 语法异常: 跳过这一段, 不猜
        }
        if (b - a + 1 > nOut + 1) {
            b = a + nOut;   // 超大区间(不该出现)只取前 nOut 个
        }
        for (int c = a; c <= b && n < nOut; ++c) {
            out[n++] = c;
        }
    }
    return n;
}

// ---- ① thread_siblings_list ----
inline bool readSiblingGroup(int cpu, int* out, int nOut, int* errOut)
{
    char buf[512];
    // out[] 必须全部填成 -1(2026-10-07 真机回归修复, 本次事故的根因)
    //   parseCpuMask 只写它解析出来的前 n 个位置, 剩下的一个都不碰; 而本函数的调用方
    //   (readTopology 的 ① 分支)按 kMaxTopoCpus 遍历整张 mask[]。以前这里没有任何初始化
    //   —— 那片栈内存是未定义的, 真机(Pura X Max / HOP-AL00)上它恰好是 0, 而 0 又是合法核号:
    //     每个逻辑核的"兄弟"里因此凭空多出 cpu0 => 组号被压成 1+0 = 1
    //     => 14 个逻辑核全归成同一个物理核 => physical = 1
    //     (而 smtPossible = (physical < logical) 仍为 true, 表面还写着"检测到 SMT")。
    //   后果: 关掉超线程后实际使用集合只剩 1 个核, 多核阶段 parallelism 掉到 1, 所有多核
    //   分数作废(真机: 多核 5714 -> 1448, 总分 2622 -> 1412)。SMT 开着时被"可用核集合"
    //   夹到 9, 与物理核数无关, 所以一直没暴露。
    //   -1 不是合法核号, 会被 readTopology 的 (mask[k] < 0) 过滤掉 —— 这是唯一安全的初值:
    //   填 0 反而会把"没解析到的位置"当成 cpu0, 那正是本 bug 的形态。
    for (int i = 0; i < nOut; ++i) {
        out[i] = -1;
    }
    const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/thread_siblings_list";
    if (readTextFile(path, buf, (int)sizeof(buf), errOut) == false) {
        return false;
    }
    if (parseCpuMask(buf, out, nOut) < 1) {
        if (errOut != nullptr) {
            *errOut = -2;   // 文件读到了但内容解析不出核号(格式异常): 与"读不到"区分开
        }
        return false;
    }
    return true;
}

// ---- ② core_id / physical_package_id ----
inline bool readCorePair(int cpu, int& coreId, int& pkgId, int* coreErr, int* pkgErr)
{
    char buf[64];
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
    coreId = -1;
    pkgId = -1;
    if (readTextFile(base + "core_id", buf, (int)sizeof(buf), coreErr)) {
        coreId = atoi(buf);
    }
    if (readTextFile(base + "physical_package_id", buf, (int)sizeof(buf), pkgErr)) {
        pkgId = atoi(buf);
    }
    return (coreId >= 0 && pkgId >= 0);
}

// ---- 逻辑核数的第二个来源: /proc/cpuinfo 的 processor 行数 ----
inline int logicalCountFromProcCpuinfo(int* errOut = nullptr)
{
    if (errOut != nullptr) {
        *errOut = 0;
    }
    errno = 0;
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f == nullptr) {
        if (errOut != nullptr) {
            *errOut = errno != 0 ? errno : -1;
        }
        return 0;
    }
    int n = 0;
    char line[256];
    while (fgets(line, (int)sizeof(line), f) != nullptr) {
        if (strncmp(line, "processor", 9) == 0) {
            ++n;
        }
    }
    fclose(f);
    return n;
}

// 逻辑核数: 频率表长度优先(它决定绑核用的编号空间), 否则 /proc/cpuinfo
inline int logicalCpuCountFrom(const std::vector<int>& freqs, int* procErr = nullptr)
{
    int n = (int)freqs.size();
    if (n > kMaxTopoCpus) {
        n = kMaxTopoCpus;
    }
    if (n > 0) {
        return n;
    }
    n = logicalCountFromProcCpuinfo(procErr);
    if (n < 0) {
        n = 0;
    }
    if (n > kMaxTopoCpus) {
        n = kMaxTopoCpus;
    }
    return n;
}

// 逻辑核 cpu 所属的物理核编号(0 起, 与 AuroraCpuTopoInfo::physicalOfCpu 同一套编号)
inline int physOf(const AuroraCpuTopoInfo& t, int cpu)
{
    if (cpu >= 0 && cpu < kMaxTopoCpus) {
        return t.physicalOfCpu[cpu];
    }
    return -1;
}

// 读一次 CPU 拓扑(只读 sysfs + /proc, 不做任何写入)
inline AuroraCpuTopoInfo readTopology(const std::vector<int>& freqs)
{
    AuroraCpuTopoInfo t;
    for (int i = 0; i < kMaxTopoCpus; ++i) {
        t.physicalOfCpu[i] = i;   // 默认: 一核一物理核(会在下面被改写)
    }
    t.logical = logicalCpuCountFrom(freqs, &t.procCpuinfoErrno);
    t.freqRead = (int)freqs.size();
    const int n = t.logical;
    if (t.freqRead <= 0) {
        int fe = 0;
        char tmp[16];
        const std::string first = "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq";
        if (readTextFile(first, tmp, (int)sizeof(tmp), &fe)) {
            t.freqErrno = -2;   // 打得开却没进表(不该发生): 标出来
        } else {
            t.freqErrno = fe;
        }
    }
    t.procCpuinfoOk = (t.procCpuinfoErrno == 0) ? 1 : 0;
    {
        // /sys/devices/system/cpu/{possible,present}: 不是本节的判据, 但"读不读得到"是
        // 用户最关心的事实之一(hdc shell 读不到, App 域可能读得到), 一并记录。
        char buf[256];
        int e1 = 0;
        int e2 = 0;
        t.possibleRead = readTextFile("/sys/devices/system/cpu/possible", buf, (int)sizeof(buf), &e1) ? 1 : 0;
        t.possibleErrno = t.possibleRead ? 0 : e1;
        t.presentRead = readTextFile("/sys/devices/system/cpu/present", buf, (int)sizeof(buf), &e2) ? 1 : 0;
        t.presentErrno = t.presentRead ? 0 : e2;
    }

    // ---- ① thread_siblings_list ----
    int group[kMaxTopoCpus];
    int groupRead = 0;
    for (int c = 0; c < n; ++c) {
        group[c] = -1;
        int mask[kMaxTopoCpus];
        int sibErr = 0;
        if (readSiblingGroup(c, mask, kMaxTopoCpus, &sibErr)) {
            group[c] = 0;
            for (int k = 0; k < kMaxTopoCpus; ++k) {
                if (mask[k] < 0 || mask[k] >= n) {
                    continue;
                }
                const int g = 1 + mask[k];   // 1 起编号, 0 留给"没有组"
                if (group[c] == 0 || g < group[c]) {
                    group[c] = g;            // 同一物理核 => 同一个组号
                }
            }
            if (group[c] > 0) {
                ++groupRead;
                ++t.siblingsRead;
            } else if (t.siblingsErrno == 0) {
                t.siblingsErrno = -2;   // 读到了但组号落不到有效核号上(格式异常)
            }
        } else if (t.siblingsErrno == 0) {
            t.siblingsErrno = sibErr;   // 第一个失败核的 errno(通常就是 EACCES=13 / ENOENT=2)
        }
    }
    // 至少两个核读到了兄弟表才承认这一级(单核读到不足以说明整机拓扑)
    if (groupRead >= 2) {
        int maxG = 0;
        for (int c = 0; c < n; ++c) {
            if (group[c] > maxG) {
                maxG = group[c];
            }
        }
        int physOfGroup[kMaxTopoCpus + 2];
        for (int i = 0; i <= maxG + 1 && i < kMaxTopoCpus + 2; ++i) {
            physOfGroup[i] = -1;
        }
        t.physical = 0;
        for (int c = 0; c < n; ++c) {
            if (group[c] <= 0) {
                t.physicalOfCpu[c] = t.physical++;   // 兄弟表缺失的核: 单独成组
                continue;
            }
            if (physOfGroup[group[c]] < 0) {
                physOfGroup[group[c]] = t.physical++;
            }
            t.physicalOfCpu[c] = physOfGroup[group[c]];
        }
        if (t.physical <= 0) {
            t.physical = n;
            for (int c = 0; c < n; ++c) {
                t.physicalOfCpu[c] = c;
            }
        }
        t.source = AURORA_TOPO_SRC_SIBLINGS;
        t.known = 1;
    } else {
        // ---- ② core_id + physical_package_id ----
        int pairRead = 0;
        for (int c = 0; c < n; ++c) {
            t.coreId[c] = -1;
            t.packageId[c] = -1;
            int cid = -1;
            int pid = -1;
            int cErr = 0;
            int pErr = 0;
            if (readCorePair(c, cid, pid, &cErr, &pErr)) {
                t.coreId[c] = cid;
                t.packageId[c] = pid;
                ++pairRead;
            }
            if (cid >= 0) {
                ++t.coreIdRead;
            } else if (t.coreIdErrno == 0) {
                t.coreIdErrno = cErr;
            }
            if (pid >= 0) {
                ++t.pkgIdRead;
            } else if (t.pkgIdErrno == 0) {
                t.pkgIdErrno = pErr;
            }
        }
        if (pairRead >= 1) {
            int seenPkg[kMaxTopoCpus];
            int seenCore[kMaxTopoCpus];
            int seenN = 0;
            t.physical = 0;
            for (int c = 0; c < n; ++c) {
                if (t.coreId[c] < 0 || t.packageId[c] < 0) {
                    t.physicalOfCpu[c] = t.physical++;   // 读不到这个核的 core_id: 单独成组
                    continue;
                }
                int found = -1;
                for (int k = 0; k < seenN; ++k) {
                    if (seenPkg[k] == t.packageId[c] && seenCore[k] == t.coreId[c]) {
                        found = k;
                        break;
                    }
                }
                if (found < 0) {
                    if (seenN < kMaxTopoCpus) {
                        seenPkg[seenN] = t.packageId[c];
                        seenCore[seenN] = t.coreId[c];
                        found = seenN;
                        ++seenN;
                    } else {
                        found = seenN;   // 表满(不该出现): 当作新的物理核
                    }
                }
                t.physicalOfCpu[c] = found;
                if (found + 1 > t.physical) {
                    t.physical = found + 1;
                }
            }
            if (t.physical <= 0) {
                t.physical = n;
                for (int c = 0; c < n; ++c) {
                    t.physicalOfCpu[c] = c;
                }
            }
            t.source = AURORA_TOPO_SRC_CORE_ID;
            t.known = 1;
        }
    }

    // ---- ③ 未知: 标记, 逻辑核数与物理核数按相等(1:1)处理 ----
    if (!t.known) {
        t.source = AURORA_TOPO_SRC_UNKNOWN;
        t.physical = n;
        for (int c = 0; c < kMaxTopoCpus; ++c) {
            t.physicalOfCpu[c] = c;
            t.coreId[c] = -1;
            t.packageId[c] = -1;
        }
    }
    // smtPossible 只由读到的拓扑决定; 未知时 physical == logical, 因而恒为 0(没有 SMT)
    t.smtPossible = (t.physical > 0 && t.physical < t.logical) ? 1 : 0;

    // ---- 来源说明(界面会原样显示, 所以必须写清楚"未知时按 1:1 处理") ----
    if (t.source == AURORA_TOPO_SRC_SIBLINGS) {
        snprintf(t.sourceText, sizeof(t.sourceText),
                 "thread_siblings_list(%d/%d 个逻辑核读到; 逐核读 /sys/devices/system/cpu/cpuN/topology/thread_siblings_list)",
                 t.siblingsRead, n);
    } else if (t.source == AURORA_TOPO_SRC_CORE_ID) {
        snprintf(t.sourceText, sizeof(t.sourceText),
                 "core_id + physical_package_id(sysfs topology; thread_siblings_list 读不到, errno=%d)",
                 t.siblingsErrno);
    } else {
        snprintf(t.sourceText, sizeof(t.sourceText),
                 "未知(thread_siblings_list errno=%d, core_id errno=%d, physical_package_id errno=%d; "
                 "已按 1:1 处理, SMT 开关无效)",
                 t.siblingsErrno, t.coreIdErrno, t.pkgIdErrno);
    }
    // 逐路径实测结果(errno 一并写出): 这是"App 域到底读到了什么"的可核对事实
    snprintf(t.probeText, sizeof(t.probeText),
             "thread_siblings_list=%d/%d 核读到(errno=%d) · core_id=%d/%d(errno=%d) · "
             "physical_package_id=%d/%d(errno=%d) · cpuinfo_max_freq=%d 核(errno=%d) · "
             "cpu/possible=%s(errno=%d) · cpu/present=%s(errno=%d) · /proc/cpuinfo=%s(errno=%d)",
             t.siblingsRead, n, t.siblingsErrno,
             t.coreIdRead, n, t.coreIdErrno,
             t.pkgIdRead, n, t.pkgIdErrno,
             t.freqRead, t.freqErrno,
             t.possibleRead ? "可读" : "读不到", t.possibleErrno,
             t.presentRead ? "可读" : "读不到", t.presentErrno,
             t.procCpuinfoOk ? "可读" : "读不到", t.procCpuinfoErrno);
    return t;
}

// ---- 拓扑单例(进程内只读一次 sysfs) ----
// 频率表只用于确定"逻辑核数"(它是绑核逻辑的编号空间), 直接从缓存取, 不需要调用方传。
inline const AuroraCpuTopoInfo& topologyInfoCached()
{
    static const AuroraCpuTopoInfo t = readTopology(aurora_cpu_detail::coreMaxFreqKhzCached());
    return t;
}

// ---- 实际使用集合 ----
// 开关开(默认): 用全部逻辑核 —— 与历史行为逐位一致;
// 开关关: 每个物理核只保留一个逻辑核(该物理核里频率档最高的那个, 同频取核号最小),
//             其余逻辑核从并行集合中排除。
// 结果按频率降序排列(同频按核号升序), 与绑核逻辑使用的位次顺序一致。
// 参数里刻意不要频率表: 挑选"每个物理核的代表"只看 order(频率降序位次),
// 位次越小 = 频率越高, 同频时 order 里核号升序 —— 于是"同频取核号最小"这条规则自然成立,
// 也就不需要再单独读一遍频率表(少一个参数 = 少一处可以各说各话的地方)。
inline AuroraCpuEffectiveSet buildEffectiveSet(const AuroraCpuTopoInfo& info,
                                               const std::vector<int>& order,
                                               int smtEnabled)
{
    AuroraCpuEffectiveSet s;
    s.smtEnabled = smtEnabled ? 1 : 0;
    s.topologyKnown = info.known;
    int rank[kMaxTopoCpus];
    for (int i = 0; i < kMaxTopoCpus; ++i) {
        rank[i] = 1 << 20;   // 频率未知的核排在最后
    }
    for (size_t i = 0; i < order.size(); ++i) {
        const int cpu = order[i];
        if (cpu >= 0 && cpu < kMaxTopoCpus) {
            rank[cpu] = (int)i;
        }
    }
    if (smtEnabled || !info.known) {
        // 全部逻辑核(= 现有行为; 拓扑未知时开关无效, 同样走这一支)
        for (size_t i = 0; i < order.size() && s.count < kMaxTopoCpus; ++i) {
            const int cpu = order[i];
            if (cpu < 0 || cpu >= kMaxTopoCpus) {
                continue;
            }
            s.cpus[s.count] = cpu;
            s.physical[s.count] = (cpu < info.logical) ? info.physicalOfCpu[cpu] : cpu;
            ++s.count;
        }
    } else {
        int picked[kMaxTopoCpus];
        for (int i = 0; i < kMaxTopoCpus; ++i) {
            picked[i] = -1;
        }
        // 先按频率位次挑出每个物理核的代表(位次越小 = 频率越高)
        for (size_t i = 0; i < order.size(); ++i) {
            const int cpu = order[i];
            if (cpu < 0 || cpu >= info.logical) {
                continue;
            }
            const int p = info.physicalOfCpu[cpu];
            if (p < 0 || p >= kMaxTopoCpus) {
                continue;
            }
            if (picked[p] < 0) {
                picked[p] = cpu;
            }
        }
        // 再按频率降序(同频按核号升序)输出
        for (size_t i = 0; i < order.size() && s.count < kMaxTopoCpus; ++i) {
            const int cpu = order[i];
            if (cpu < 0 || cpu >= info.logical) {
                continue;
            }
            const int p = info.physicalOfCpu[cpu];
            if (p < 0 || p >= kMaxTopoCpus || picked[p] != cpu) {
                continue;
            }
            s.cpus[s.count] = cpu;
            s.physical[s.count] = p;
            ++s.count;
        }
    }
    for (int i = 0; i < s.count; ++i) {
        const int cpu = s.cpus[i];
        if (cpu >= 0 && cpu < 64) {
            s.mask |= (1ull << (unsigned)cpu);
        }
    }
    return s;
}

// ---- 实际使用集合的单例(开关一变就整体重算) ----
//
// 已知的理论竞态(不修, 但必须写在这里)
//   本函数与 topologyCached() 都是函数局部 static 单例(与本文件既有的频率表 / 拓扑缓存
//   同一套设计)。若在负载运行期间调用 auroraSetSmtEnabled() 触发重算, 正在跑的池线程可能
//   读到重算中的集合 —— 严格说不是线程安全的。它的前提是"跑分期间开关可被改动":
//   ArkTS 侧已用 Index.anyBenchRunning() 在 UI 层挡死(Toggle 的 .enabled + 回调护栏),
//   所以真实路径上不会发生。为它加锁会引入一条没人验证过的并发路径, 因此刻意不加。
inline const AuroraCpuEffectiveSet& effectiveSetCached(const AuroraCpuTopoInfo& info,
                                                       const std::vector<int>& order)
{
    static AuroraCpuEffectiveSet s;
    static int builtFor = -1;
    const int now = ::auroraSmtEnabledFlag() ? 1 : 0;
    if (builtFor != now) {
        s = buildEffectiveSet(info, order, now);
        builtFor = now;
    }
    return s;
}

} // namespace aurora_smt_detail

// ---------------------------------------------------------------------------
//  一次负载的"跑在哪个核 / 哪个簇"诊断(旁路信息, 不计分)
//  下面这些字段由 napi 层原样放进 runGb7 的 JSON:
//    cpu / cpuAtStart / maxKhz / rank / cores / bound
//    cpuInFastCluster / cpuAtStartInFastCluster
//    fastClusterCores / fastClusterMaxKhz / fastClusterMask / workers
//  其余(workerMask)只进 text 供 runlog 阅读。
// ---------------------------------------------------------------------------
struct AuroraCpuPlacement {
    int cpu;                            // 负载线程实际所在的 CPU 编号(负载跑完那一刻采样)
    int cpuAtStart;                     // 绑定做完、负载开跑前的采样; 仅供参考:
                                        //   sched_setaffinity 返回时线程未必已经被调度到
                                        //   掩码内, 所以它不在快簇里不能证明绑定失败
                                        //   (绑定是否生效看 cpuInFastCluster)
    int maxKhz;                         // 该 CPU 的 cpuinfo_max_freq(kHz); 0 = 未知
    int rank;                           // 该 CPU 在"频率降序"表里的位次, 0 = 最快; -1 = 未知
                                        //   (附加信息: 位次不再作为"绑核是否生效"的判据)
    int cores;                          // 频率表里读到的核数; 0 = 读不到 -> 全程未绑定
    bool bound;                         // 主线程是否成功应用了"快簇掩码"
    // ---- 新判据(2026-10 起): 跑完时所在的核是否属于快簇 ----
    bool cpuInFastCluster;              // 绑核是否真的生效, 看这个(不再要求 cpu == cpuAtStart)
    bool cpuAtStartInFastCluster;       // 开跑时的采样是否也在快簇内(仅参考)
    int fastClusterCores;               // 生效快簇包含的核数; 0 = 快簇未定义(读不到频率表)
    int fastClusterMaxKhz;              // 生效快簇的最高频率(kHz); 0 = 未知
    unsigned long long fastClusterMask; // 生效快簇的核心位图(每核一位; 只覆盖前 64 个核)
                                        //   = (全机最快档 ∩ 可用核集合); 交集为空时退化成
                                        //     "可用核集合里最快的核"(见 fastClusterFallback)
    int threads;                        // 本次负载实际用到的线程数(已按 SMT 开关夹过上限)
    int workers;                        // 成功绑定自己的池线程个数
    int workersOverflow;                // 其中因"分配给我的位次已被占用"而改到其它位次的个数
    unsigned long long workerMask;      // 池线程实际落点(每核一位)的核心位图
    // ---- 超线程(SMT)诊断: 拓扑 / 实际使用集合 / 开关状态(旁路信息, 不计分) ----
    int logical;                        // 逻辑核数(0 = 未知)
    int physical;                       // 物理核数(拓扑未知时 = 逻辑核数, 按 1:1 处理)
    int smtPossible;                    // 1 = 检测到 SMT(物理核数 < 逻辑核数)
    int smtEnabled;                     // 1 = 本次负载跑的时候 SMT 开关是开的
    int topoKnown;                      // 1 = 拓扑已知(读到了 sysfs); 0 = 未知(已按 1:1 处理)
    int threadsRequested;               // 调用方请求的线程数(未被夹时与 threads 相同)
    int threadsEffective;               // 本项真正开了几个线程(已过 auroraThreadCap 的夹取)
                                        //   2026-10-06 修正: 以前这里是"实际使用集合的大小",
                                        //   于是在被内核限核的机器上, 连单核项都会被写成"14 线程"。
    int threadsBasisFull;               // 不看可用核集合时(全机逻辑核/物理核口径)应有的线程数
                                        //   (= "本应 N"; 与 threadsEffective 之差就是被夹掉的量)
    unsigned long long effMask;         // 实际使用集合的位图(每核一位; 只覆盖前 64 个核)
    char topoSource[96];                // 拓扑来源(哪个文件 / "未知, 已按 1:1 处理")
    // ---- 内核允许的核集合(2026-10-05 追加) ----
    // 回答"为什么这台机器的分数像被锁住了": 内核允许本进程用的核, 与全机频率排名并不是一回事。
    int cpuAllowedOk;                   // 1 = 允许核集合读到了(>= 1 个核)
    int cpuAllowedCount;                // 可用核集合里的核数(0 = 读不到)
    unsigned long long cpuAllowedMask;  // 可用核集合位图(只覆盖前 64 个核)
    unsigned long long cpuAppliedMask;  // 主线程真正应用的那张掩码(sched_setaffinity 实际生效的目标)
    int cpuFastClusterFallback;         // 1 = 可用核集合 ∩ 最快档为空, 已退化成"可用核集合里最快的核"
    int cpuFastClusterTierIndex;        // 生效快簇落在全机的第几个频率档(0 = 全机最快档)
    // ---- "是否落在全机最快档" —— 与"是否属于生效快簇"分开报, 两个概念不许混 ----
    int cpuInFastClusterJudged;         // 1 = 生效快簇已定义, cpuInFastCluster 有意义
    int cpuInMachineTopTier;            // 1 = 跑完那一刻的核属于全机最快频率档
    int cpuMachineTopTierCores;         // 全机最快频率档有几个核
    int cpuMachineTopTierKhz;           // 全机最快频率档的频率(kHz)
    unsigned long long cpuMachineTopTierMask;   // 全机最快档的位图(排障用)
    char cpuFastClusterSource[256];     // 快簇是怎么算出来的(中文一行, 可直接显示)
    // ---- 掩码回读(2026-10 追加): "内核接受了掩码"与"线程真的只能在掩码里跑"是两件事 ----
    //   真机证据: 同一项 note 里 allowed=0-8 而运行时采样写着"本线程所在核 cpu12,cpu13"。
    //   因此这里把"绑定后立刻读回的掩码"原样上报: 若回读掩码里没有 cpu12, 而线程确实跑在
    //   cpu12, 那这一条就是"内核接受掩码但不按它调度"的硬证据(而不是我们算错了集合)。
    unsigned long long cpuAppliedReadbackMask;  // 绑定后立刻 sched_getaffinity 读回的掩码
    int cpuAppliedReadbackOk;                   // 1 = 回读成功
    int cpuOutsideAppliedMask;                  // 1 = 跑完那一刻的 cpu 不在回读掩码内(硬事实)
    int cpuAtStartOutsideAppliedMask;           // 1 = 开跑那一刻的 cpu 也不在回读掩码内
    char cpuReadbackText[384];                  // 回读掩码 + 由它得出的硬事实(中文一行)
    // ---- 绑核决策的数据源 + 实测许可探测结果(2026-10 追加) ----
    int cpuAuthority;                           // 0 实测探测 / 1 sched_getaffinity / 2 proc list / 3 proc hex / 4 读不到
    int cpuProbeAcceptedCount;                  // 实测探测中"内核接受"的核数
    unsigned long long cpuProbeAcceptedMask;    // 它的位图
    char cpuProbeText[1024];                    // 逐核 接受/被拒/errno 的一行(原样)
    // ---- 逐核最高频率 + 频率档划分(2026-10 追加) ----
    //   回答"生效快簇为什么是这几个核、为什么从 5 核变成 3 核": 把当时逐核
    //   cpuinfo_max_freq 与按它划出来的频率档原样摆出来, 不需要任何事后猜测。
    char cpuTierText[1024];
    // ---- 逐核启动状态与 A/B/C/D 分类(2026-10 追加) ----
    //   回答用户的原话问题:"为什么有核没有启动"。这一行的内容是 probeCoreStartupState()
    //   的逐字输出: 全局 present/possible/online 三个文件的原文(含 errno) + 逐核
    //   cpuN/online 原文 / 逐核 cpuinfo_max_freq 与 scaling_cur_freq 原文 / 逐核拓扑
    //   (core_id, physical_package_id, thread_siblings_list) 原文 / 逐核 setaffinity 实测
    //   与 errno / C 类拉起探测(写 '1' -> 读回 -> 按原值恢复, 并记下每个 errno) / 分类汇总。
    //   它只读 sysfs + 一次逐核 setaffinity 探测 + 一次 online 写探测(写前备份、无论成败都恢复),
    //   全部发生在计时区间之外, 不计分。
    int coreStartupRan;                 // 1 = 逐核启动状态探测执行过
    int coreStartupUpper;               // 探测的核号上界(= 0..upper-1 都被逐核探过)
    int coreStartupPresentCount;        // present 原文里的核数
    int coreStartupPossibleCount;       // possible 原文里的核数
    int coreStartupOnlineCount;         // 全局 online 原文里的核数
    int coreClassA;                     // A 在线且被内核允许(可用)
    int coreClassB;                     // B 在线但被本进程的许可集合拒绝
    int coreClassC;                     // C 离线(offline) —— 核没有启动
    int coreClassD;                     // D present 里根本没有
    unsigned long long coreClassAMask;
    unsigned long long coreClassBMask;
    unsigned long long coreClassCMask;
    unsigned long long coreClassDMask;
    int coreOnlineWriteAttempted;       // C 类拉起探测: 真的写过 '1' 的核数
    int coreOnlineWriteOk;              // 写后读回 = 1 的核数(真的拉起来了)
    int coreOnlineWriteFail;            // 写后读回 != 1 的核数
    int coreOnlineLastOpenErrno;        // 最后一次打开 cpuN/online 失败的 errno(13=EACCES, 1=EPERM)
    int coreOnlineLastWriteErrno;       // 最后一次写失败的 errno
    int coreOnlineRestoreOk;            // 恢复成功的核数 / 应恢复的核数(全成功才是"已全部恢复")
    int coreOnlineRestoreTotal;
    char coreStartupText[3584];         // 逐核启动状态与分类的完整一行(中文)
    // ---- 多核阶段"每颗核都跑满"的取证(2026-10 追加) ----
    //   起因: 真机多核项运行时频率中位 558MHz、界面并行度 3.5~4.3 核(可用 9 核) —— 核没被用满。
    //   这一组字段回答四个问题: 开了几个线程 / 每个线程被指派到哪个核 / 实际用到了哪些核 /
    //   有没有核从头到尾没人用(由 /proc/stat 逐核占用率给出)。
    int multiCore;                      // 1 = 本项是多核阶段(铺满模式)
    int availCores;                     // N = 可用核数(权威集合 ∩ SMT 口径)
    int spreadCores;                    // 工作集合大小(多核阶段 = N; 单核阶段 = 快簇核数)
    int spreadBound;                    // 成功绑到"独占核"的池线程数
    int spreadUnbound;                  // 没能绑到独占核的池线程数(必须报)
    int spreadClamped;                  // 绑定后回读掩码 != {目标核} 的次数(内核夹过掩码)
    int usedCores;                      // M = 实际用到的核数(起始落点 ∪ 结束落点, 去重)
    unsigned long long usedMask;        // M 的位图
    unsigned long long targetMask;      // 目标核位图(= 工作集合)
    int emptyCores;                     // 工作集合里"一个线程都没落到"的核数(>0 必须给原因)
    char spreadTable[768];              // 逐核落点表: "cpu0:#0@cpu0->cpu0 cpu1:#1@cpu1->cpu1 ..."
    char spreadVerdict[640];            // "本项用到 M/N 个核, 原因是 ..."(不许静默)
    char spreadText[1024];              // 完整一行(线程数/核清单/每核是否有线程/自检结论)
    // 允许核集合的一行完整文本(含 cpuset / Mems_allowed_list)。
    // 2026-10 容量 512 -> 1024: 这一行是"每项 note 里由 native 唯一生成、ArkTS 原样贴进
    //   note"的字符串(见下面 auroraAffinitySessionEnd 的注释)。512 字节在中文(UTF-8 一汉字
    //   3 字节)下已经装不下"cpuset 全文 + 线程数被夹说明 + 运行时实际频率"三段的合计, 原来的
    //   strncat 会静默截断尾部 —— 那正是"日志里少了半句还没人发现"的来源。只改容量,
    //   内容与措辞一个字未动(短内容的行为逐字节不变)。
    char cpuAllowedText[9216];          // 允许核集合的一行完整文本(含 cpuset / Mems_allowed_list /
                                        //   双源差集 / 实测探测 / 逐核频率档 / 掩码回读 / 运行时频率)
                                        //   容量 1024 -> 2560(2026-10): 本次新增的"双源对照 +
                                        //   逐位差集 + 实测探测逐核结果 + 逐核频率档"四段都是
                                        //   必报内容, 1024 装不下会让尾部被静默截断 ——
                                        //   那正是"日志里少了半句还没人发现"的来源。只改容量。
                                        //   容量 6144 -> 9216(2026-10, 本轮) —— 为什么是这个数:
                                        //   ① 真机 8.1(report-latest.txt)里这一行的**真实内容 =
                                        //      5778 字节**(报告里 "    可用核集合原文=" 那一行 5801
                                        //      字节减掉 23 字节前缀), 相对旧容量 6144 只剩 6% 余量 ——
                                        //      这一行后面 appendSeg() 拼进来的那几段以前是静默截断
                                        //      (放不下就把那一段切在那里, 不留任何标记): 本轮已把那条出口
                                        //      改成显式标记(见 appendSeg 里的 kSegTrunc), 但余量本身也得
                                        //      给够 —— 真机上再多两段内容就会掉尾巴;
                                        //   ② 9216 >= 5778 x 1.5(= 8667) -> 余量 = +59%;
                                        //   ③ 为什么不再大: 一旦 >= (追加运行时频率前已用的 4211 B
                                        //      + 3 + 那一行 7256 B + 1) = 11471 B, gb7.cpp 里那段
                                        //      "装得下就拼进 note, 装不下退到 o.diag"的既定分流就会
                                        //      改道(运行时频率从 diag 挤进 note, 反过来把 QoS 与
                                        //      cpuset 结论挤到 diag) —— 那是报告版面的变化, 不属于
                                        //      "只扩容量"; 9216 与 11471 之间留了 2255 B 的改道余量;
                                        //   ④ 上界: 这一行由固定几个片段拼成(实测 5778 B), 各片段
                                        //      的最大值合计 <= 9216; 真放不下时 gb7.cpp 侧走的是
                                        //      非截断的 o.diag 兜底, 一个字都不丢。
                                        //   只改容量, 内容与措辞一个字未动。
    char cpuBoundReason[256];           // cpuBound = false 时的原因(绑核目标不在可用核集合内 / 读不到频率表 …)
    std::string text;                   // 一行人类可读的诊断文本

    AuroraCpuPlacement()
        : cpu(-1), cpuAtStart(-1), maxKhz(0), rank(-1), cores(0), bound(false),
          cpuInFastCluster(false), cpuAtStartInFastCluster(false),
          fastClusterCores(0), fastClusterMaxKhz(0), fastClusterMask(0),
          threads(1), workers(0), workersOverflow(0), workerMask(0),
          logical(0), physical(0), smtPossible(0), smtEnabled(1), topoKnown(0),
          threadsRequested(1), threadsEffective(0), threadsBasisFull(0), effMask(0),
          topoSource(),
          cpuAllowedOk(0), cpuAllowedCount(0), cpuAllowedMask(0), cpuAppliedMask(0),
          cpuFastClusterFallback(0), cpuFastClusterTierIndex(0),
          cpuInFastClusterJudged(0), cpuInMachineTopTier(0),
          cpuMachineTopTierCores(0), cpuMachineTopTierKhz(0), cpuMachineTopTierMask(0),
          cpuFastClusterSource(),
          cpuAppliedReadbackMask(0), cpuAppliedReadbackOk(0), cpuOutsideAppliedMask(0),
          cpuAtStartOutsideAppliedMask(0), cpuReadbackText(),
          cpuAuthority(4), cpuProbeAcceptedCount(0), cpuProbeAcceptedMask(0), cpuProbeText(),
          cpuTierText(),
          coreStartupRan(0), coreStartupUpper(0), coreStartupPresentCount(0),
          coreStartupPossibleCount(0), coreStartupOnlineCount(0),
          coreClassA(0), coreClassB(0), coreClassC(0), coreClassD(0),
          coreClassAMask(0ull), coreClassBMask(0ull), coreClassCMask(0ull), coreClassDMask(0ull),
          coreOnlineWriteAttempted(0), coreOnlineWriteOk(0), coreOnlineWriteFail(0),
          coreOnlineLastOpenErrno(0), coreOnlineLastWriteErrno(0),
          coreOnlineRestoreOk(0), coreOnlineRestoreTotal(0), coreStartupText(),
          multiCore(0), availCores(0), spreadCores(0), spreadBound(0), spreadUnbound(0),
          spreadClamped(0), usedCores(0), usedMask(0), targetMask(0), emptyCores(0),
          spreadTable(), spreadVerdict(), spreadText(),
          cpuAllowedText(), cpuBoundReason(), text()
    {
    }
};

namespace aurora_cpu_detail {

// ===========================================================================
//  内核到底允许本进程用哪些核 —— Cpus_allowed_list / sched_getaffinity / cpuset
// ===========================================================================
//
//  为什么必须有这一节(2026-10 真机证据驱动的改动)
//  ---------------------------------------------------------------------------
//  sched_setaffinity() 是请求, 不是命令: 内核只会把请求的掩码与"进程当前的
//  可用核集合"(= cpuset cgroup 的 cpuset.cpus ∩ 进程已有的 sched_getaffinity 掩码)
//  求交集。厂商 ROM 常把第三方应用放进一个受限 cpuset(把 Prime 核留给前台/系统线程),
//  这时:
//    * sched_setaffinity 到"可用核集合之外"的核 可以返回 0(成功) —— 掩码被静默夹回
//      可用核集合, 调用方拿到"成功"却根本没绑到想绑的核(极端情况返回 EINVAL, 取决于内核
//      版本与掩码是否有交集);
//    * 于是"全机频率排名算出来的快簇"可能是空的可用集 —— 绑到不允许的核上等于没绑,
//      负载仍然被调度器在可用核集合里随机摆放。
//  这正是"绑核成功(cpuBound=true)却跑在核外"最可能的机制, 也是本节存在的全部理由:
//  把"内核允许我们用哪些核"变成可核对的事实, 并让绑核逻辑永远与它求交集。
//
//  读取的三条路径(逐条记结果与 errno, 读不到就说读不到, 不静默)
//  ---------------------------------------------------------------------------
//   ① /proc/self/status 的 Cpus_allowed_list(内核允许的核集合, 十进制 cpulist)
//      + Cpus_allowed(十六进制掩码, 与前者互相印证) + Mems_allowed_list;
//   ② sched_getaffinity(0, ...) 的实际掩码 —— 与 ① 互相印证(不一致就是硬事实, 要报出来);
//      注意: 它返回的是调用线程当前生效的掩码, 若此前有人绑过核就会偏窄, 所以读取
//      发生在任何绑核动作之前, 且与 ① 的差异会写进 mismatched 字段;
//   ③ /proc/self/cgroup + /proc/self/mountinfo 推出 cgroup 目录, 再读
//      cpuset.cpus.effective / cpuset.cpus(cgroup v2) 或 cpuset.effective_cpus /
//      cpuset.cpus(cgroup v1); 每一级读不到都记自己的 errno(EACCES=13 / ENOENT=2)。
//
//  降级规则(不因为读不到就不绑)
//  ---------------------------------------------------------------------------
//    * 可用核集合读到了(①/② 任一, 核数 >= 1) -> 按它过滤(见 buildTopology);
//    * 可用核集合完全读不到 -> 退回历史行为(全机频率排名, 不做任何过滤), 但把
//      "读不到"这一事实(cpuAllowedOk=false + errno)写进每一项的诊断里 —— 界面会照实显示。
//  本节全部只读: 只 fopen/fread/syscall, 不写任何文件、不改任何全局状态、不计分。
// ===========================================================================

struct AuroraCpuAllowedSet {
    int ok;                          // 1 = 可用核集合有效(核数 >= 1)
    unsigned long long mask;         // 可用核集合位图(只覆盖前 64 个核)
    int count;                       // 可用核集合里的核数(<= kMaxTopoCpus)
    int cpus[kMaxTopoCpus];          // 可用核集合成员(升序)
    int inThreadMask;                // 1 = 掩码来自 sched_getaffinity(本线程); 0 = 来自 /proc
    // ---- 逐路径实测结果(可核对的事实: 哪个文件读到了什么, 读不到是什么 errno) ----
    unsigned long long statusMask;   // /proc/self/status 的 Cpus_allowed(十六进制; 0 = 读不到)
    int statusMaskOk;                // 1 = Cpus_allowed 可读且非零
    int statusErrno;                 // 打开 /proc/self/status 失败时的 errno
    int statusListFound;             // 1 = 找到了 Cpus_allowed_list 行
    int syscallOk;                   // 1 = sched_getaffinity 成功
    int syscallErrno;                // 失败时的 errno
    int mismatched;                  // 1 = 两条主路径给出的掩码不同(硬事实, 必须报出来)
    // ==== 双源原始读数(原样保留, 供"不许只给一个 bool"地逐位对照; 2026-10 追加) ====
    char statusMaskRaw[64];          // /proc/self/status 的 Cpus_allowed 十六进制原文
    int statusMaskErrno;             // 读 Cpus_allowed 失败时的 errno(0 = 读到; -2 = 没有这一行)
    char statusListRaw[192];         // Cpus_allowed_list 原文
    int statusListErrno;             // 读 Cpus_allowed_list 失败时的 errno(0 = 读到; -2 = 没有这一行)
    unsigned long long statusListMask;   // 由 Cpus_allowed_list 原文解析出的位图
    unsigned long long syscallMask;  // sched_getaffinity(0,...) 的掩码(原样)
    char syscallCoreList[160];       // 它的逐核清单(cpu0,cpu1,...)
    char statusHexCoreList[160];     // Cpus_allowed 十六进制的逐核清单
    char statusListCoreList[160];    // Cpus_allowed_list 原文的逐核清单
    char diffText[768];              // 三条差集(逐位列出, 不许只给一个 bool)
    // ==== 实测许可探测(2026-10 追加; 唯一权威来源) ====
    int probeRan;                    // 1 = 探测执行过
    int probeTid;                    // 被探测的线程(gettid)
    int probeUpper;                  // 探测的核号上界(0..probeUpper-1)
    int probeUpperSrc;               // 上界来源(0 频率表 / 1 cpuinfo / 2 possible / 3 sysconf / -1 兜底)
    int probeAcceptedCount;          // 内核接受的核数
    unsigned long long probeAcceptedMask;
    int probeEinval;                 // EINVAL 次数(该核不在 cpuset 内)
    int probeEperm;                  // EPERM 次数
    int probeOtherFail;              // 其它失败(含"返回 0 但读回被夹回")
    int probeClampedCount;           // 其中"返回 0 但读回掩码 != {cpu}"的次数
    int probeSavedOk;                // 1 = 探测前成功保存原掩码
    unsigned long long probeSavedMask;
    int probeRestoreOk;              // 1 = 还原成功且读回一致
    char probeText[1024];            // 探测的一行完整文本(含逐核 接受/被拒/errno)
    // ==== 权威来源(绑核决策用哪一个读数) ====
    int authority;                   // 0 = 实测探测; 1 = sched_getaffinity; 2 = Cpus_allowed_list; 3 = Cpus_allowed(十六进制); 4 = 都读不到
    char authorityText[448];         // "用的是哪个来源、差在哪"的一行
    int memsFound;                   // 1 = 读到 Mems_allowed_list
    char memsList[64];               // Mems_allowed_list 原文
    char cgroupPath[128];            // /proc/self/cgroup 的第一行(原文, 去掉换行)
    char cpusetDir[192];             // 推出/读到的 cpuset 目录(空 = 推不出来)
    char cpusetCpus[64];             // cpuset.cpus(或 v1 cpuset.cpus)原文
    int cpusetCpusErrno;             // 读它失败时的 errno
    char cpusetEffective[64];        // cpuset.cpus.effective(或 v1 cpuset.effective_cpus)原文
    int cpusetEffErrno;              // 读它失败时的 errno
    char listText[192];              // 权威集合的 cpulist 文本, 形如 "0-9" / "0-3,8-11"
    char sourceText[448];            // 来源一行(界面直接显示)
    char line[1024];                 // 面向"设备画像 / runlog"的一行完整文本(下文 auroraCpuAllowedText 会补齐快簇/交集)
    AuroraCpuAllowedSet()
        : ok(0), mask(0), count(0), cpus(), inThreadMask(0),
          statusMask(0), statusMaskOk(0), statusErrno(0), statusListFound(0),
          syscallOk(0), syscallErrno(0), mismatched(0),
          statusMaskRaw(), statusMaskErrno(0), statusListRaw(), statusListErrno(0),
          statusListMask(0), syscallMask(0), syscallCoreList(), statusHexCoreList(),
          statusListCoreList(), diffText(),
          probeRan(0), probeTid(-1), probeUpper(0), probeUpperSrc(-1),
          probeAcceptedCount(0), probeAcceptedMask(0), probeEinval(0), probeEperm(0),
          probeOtherFail(0), probeClampedCount(0), probeSavedOk(0), probeSavedMask(0),
          probeRestoreOk(0), probeText(), authority(4), authorityText(),
          memsFound(0), memsList(),
          cgroupPath(), cpusetDir(), cpusetCpus(), cpusetCpusErrno(0),
          cpusetEffective(), cpusetEffErrno(0), listText(), sourceText(), line()
    {
    }
};

// 掩码 -> cpulist 文本("0-9" / "0-3,8-11"); 只覆盖前 64 个核。
// 写法与内核一致: 连续区间用 a-b, 单个用 a, 区间之间逗号分隔。
inline void maskToCpuList(unsigned long long m, int maxCpu, char* out, int cap)
{
    if (out == nullptr || cap <= 0) {
        return;
    }
    out[0] = '\0';
    if (m == 0) {
        return;
    }
    if (maxCpu > 64) {
        maxCpu = 64;
    }
    int n = 0;
    int c = 0;
    while (c < maxCpu) {
        if (((m >> (unsigned)c) & 1ull) == 0) {
            ++c;
            continue;
        }
        const int start = c;
        int end = c;
        while (end + 1 < maxCpu && ((m >> (unsigned)(end + 1)) & 1ull) != 0) {
            ++end;
        }
        char one[24];
        if (start == end) {
            snprintf(one, sizeof(one), "%s%d", (n == 0 ? "" : ","), start);
        } else {
            snprintf(one, sizeof(one), "%s%d-%d", (n == 0 ? "" : ","), start, end);
        }
        const size_t len = strlen(one);
        if ((int)strlen(out) + (int)len >= cap - 1) {
            break;
        }
        strncat(out, one, (size_t)(cap - 1 - (int)strlen(out)));
        ++n;
        c = end + 1;
    }
}

// 字节级读 /proc/self/status(它有多行; 这里只挑我们关心的几行)
inline bool readProcLine(const char* path, const char* key, char* out, int cap, int* errOut)
{
    if (errOut != nullptr) {
        *errOut = 0;
    }
    if (out != nullptr && cap > 0) {
        out[0] = '\0';
    }
    errno = 0;
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        if (errOut != nullptr) {
            *errOut = errno != 0 ? errno : -1;
        }
        return false;
    }
    const size_t keyLen = strlen(key);
    char line[512];
    bool found = false;
    while (fgets(line, (int)sizeof(line), f) != nullptr) {
        if (strncmp(line, key, keyLen) != 0) {
            continue;
        }
        const char* v = line + keyLen;
        while (*v == ' ' || *v == '\t' || *v == ':') {
            ++v;
        }
        size_t n = 0;
        while (v[n] != '\0' && v[n] != '\n' && v[n] != '\r' && n + 1 < (size_t)cap) {
            out[n] = v[n];
            ++n;
        }
        out[n] = '\0';
        found = true;
        break;
    }
    fclose(f);
    if (!found && errOut != nullptr) {
        *errOut = -2;   // 文件读到了但没有这一行(与"打不开"区分开)
    }
    return found;
}

// 十六进制掩码(不带 0x 前缀, 内核 Cpus_allowed 的格式) -> 位图
inline unsigned long long parseHexMask(const char* s)
{
    if (s == nullptr) {
        return 0;
    }
    unsigned long long v = 0;
    int digits = 0;
    for (int i = 0; s[i] != '\0' && digits < 16; ++i) {
        const char c = s[i];
        int d = -1;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        v = (v << 4) | (unsigned long long)d;
        ++digits;
    }
    return v;
}

inline unsigned long long maskFromCpuList(const int* list, int n)
{
    unsigned long long m = 0;
    for (int i = 0; i < n; ++i) {
        if (list[i] >= 0 && list[i] < 64) {
            m |= (1ull << (unsigned)list[i]);
        }
    }
    return m;
}

// ---- 掩码 -> 显式逐核清单文本("cpu0,cpu1,..."; 空掩码 -> "无") ----
// 与 maskToCpuList 的区别: 后者给的是内核 cpulist 写法("0-8"), 这里给的是逐核列举,
// 任务要求"逐核清单"时必须给得出这一种, 免得"0-8"被误读成"一个叫 0-8 的核"。
inline void maskToCoreListText(unsigned long long m, char* out, int cap)
{
    if (out == nullptr || cap <= 0) {
        return;
    }
    out[0] = '\0';
    int used = 0;
    for (int c = 0; c < 64; ++c) {
        if (((m >> (unsigned)c) & 1ull) == 0) {
            continue;
        }
        char one[24];
        snprintf(one, sizeof(one), "%scpu%d", (used > 0) ? "," : "", c);
        if (used + (int)strlen(one) >= cap - 1) {
            if (used + 3 < cap - 1) {
                strncat(out, "...", (size_t)(cap - 1 - used));
            }
            return;
        }
        strncat(out, one, (size_t)(cap - 1 - used));
        used += (int)strlen(one);
    }
    if (used == 0) {
        snprintf(out, (size_t)cap, "无");
    }
}

// ---- 掩码差集(逐位列出, 不给一个 bool 了事) ----
// 输出形如: "sched_getaffinity 独有 cpu9,cpu10,cpu11 / Cpus_allowed 独有 无"
inline void maskDiffText(unsigned long long a, unsigned long long b,
                         const char* aName, const char* bName, char* out, int cap)
{
    if (out == nullptr || cap <= 0) {
        return;
    }
    char onlyA[224];
    char onlyB[224];
    maskToCoreListText(a & ~b, onlyA, (int)sizeof(onlyA));
    maskToCoreListText(b & ~a, onlyB, (int)sizeof(onlyB));
    snprintf(out, (size_t)cap, "%s 独有 %s / %s 独有 %s", aName, onlyA, bName, onlyB);
}

// ===========================================================================
//  实测许可探测(2026-10 新增; 本节的结论优先于任何单个文件的解释)
// ===========================================================================
//
//  为什么必须实测(真机硬证据逼出来的)
//  ---------------------------------------------------------------------------
//  真机同一项 note 里同时出现:
//    "cpuset: allowed=0-8 (9 核; sched_getaffinity) · Cpus_allowed 与 sched_getaffinity 不一致"
//    而同一份日志的单核项运行时采样写着: "采样核 = 本线程所在核(cpu12, cpu13)"
//  若内核真的只允许 cpu0-8, 线程不可能落在 cpu12/cpu13(线程掩码由内核强制)。两条读数
//  至少有一条不是在描述"真正跑负载的那个线程"。sched_getaffinity 是"内核此刻报给我的掩码",
//  /proc/self/status 的 Cpus_allowed 是线程组组长线程的掩码(读 /proc/self 时读的是
//  leader, 不是调用线程)—— 两者本来就可能各说各话。
//  因此本节的判据改成行为学的: 对每个核号单独发一次 sched_setaffinity({cpu}), 然后
//  立刻读回 sched_getaffinity, 看内核是否真把掩码收成了那一位。
//    * 读回掩码 == {cpu}       -> 内核接受该核(该核在 cgroup cpuset 之内); 记"接受"
//    * 返回 EINVAL / 读回不含  -> 内核拒绝(该核不在 cpuset 内, 或被静默夹回); 记"被拒 + errno"
//  这个结果不依赖任何文件的解释, 也不受"读的是哪个线程"的影响: 它测的就是调用线程
//  的许可集合。因此绑核决策改用实测集合(见 readAllowedSet 的 authority 选择)。
//
//  安全性(为什么它不会污染任何计时)
//  ---------------------------------------------------------------------------
//    * 只在会话开始(计时区间之外)调用; 每个核 2 次 syscall, 整机 14 核约 28 次, 亚毫秒量级;
//    * 结束时无条件还原探测前的掩码(读回校验), 并记录还原是否成功;
//    * 不改任何负载的工作量/尺寸/线程数, 不写任何内核节点, 不计分。
// ===========================================================================

struct AuroraCpuPermitProbe {
    int ran;                        // 1 = 探测执行过
    int tid;                        // 被探测的线程(gettid)
    int upper;                      // 探测的核号范围 0..upper-1
    int upperSrc;                   // 上界来源: 0 = 频率表长度, 1 = /proc/cpuinfo, 2 = cpu/possible, 3 = sysconf, -1 = 兜底
    int acceptedCount;              // 内核接受的核数
    unsigned long long acceptedMask;
    int accepted[kMaxTopoCpus];     // 逐核: 1 = 接受, 0 = 被拒
    int errnoOf[kMaxTopoCpus];      // 逐核: 被拒时的 errno(0 = 接受; -1 = 读回失败; -4 = 返回成功但读回被夹)
    int clamped[kMaxTopoCpus];      // 1 = 返回 0 但读回掩码 != {cpu}(被静默夹回)
    int epermCount;                 // EPERM 次数(进程无权改亲和性)
    int einvalCount;                // EINVAL 次数(该核不在可用核集合内)
    int otherFailCount;             // 其它失败次数
    int savedOk;                    // 1 = 探测前成功读到原掩码
    unsigned long long savedMask;   // 原掩码
    int restoreOk;                  // 1 = 还原成功(且读回一致)
    char text[768];                 // 可直接进 note 的一行
    AuroraCpuPermitProbe()
        : ran(0), tid(-1), upper(0), upperSrc(-1),
          acceptedCount(0), acceptedMask(0), accepted(), errnoOf(), clamped(),
          epermCount(0), einvalCount(0), otherFailCount(0),
          savedOk(0), savedMask(0), restoreOk(0), text()
    {
    }
};

// 探测的核号上界: 取"频率表长度 / cpu/possible / /proc/cpuinfo 的 processor 行数 /
// sysconf(_SC_NPROCESSORS_ONLN)"四者中的最大值。为什么取最大而不是取第一个读到的:
// 本次事故的形态正是"编号空间太窄"(频率表在某个核处 break, 后面的核在绑核逻辑里根本不存在),
// 取最大值保证"真实存在的核"不会因为某一处读数失败而被漏掉。
inline int probeCpuUpperBound(int* srcOut)
{
    int best = 0;
    int src = -1;
    {
        const int nf = (int)coreMaxFreqKhzCached().size();
        if (nf > best) {
            best = nf;
            src = 0;
        }
    }
    {
        char buf[256];
        int e = 0;
        int cpus[kMaxTopoCpus];
        if (::aurora_smt_detail::readTextFile("/sys/devices/system/cpu/possible",
                                              buf, (int)sizeof(buf), &e)) {
            const int n = ::aurora_smt_detail::parseCpuMask(buf, cpus, kMaxTopoCpus);
            for (int i = 0; i < n; ++i) {
                if (cpus[i] + 1 > best) {
                    best = cpus[i] + 1;
                    src = 2;
                }
            }
        }
    }
    {
        int e = 0;
        const int n = ::aurora_smt_detail::logicalCountFromProcCpuinfo(&e);
        if (n > best) {
            best = n;
            src = 1;
        }
    }
    {
        const long on = sysconf(_SC_NPROCESSORS_ONLN);
        if (on > 0 && (int)on > best) {
            best = (int)on;
            src = 3;
        }
    }
    if (best < 1) {
        best = 8;      // 四个来源全部读不到: 仍探测 0..7(有界、便宜), 并标注来源未知
        src = -1;
    }
    if (best > kMaxTopoCpus) {
        best = kMaxTopoCpus;
    }
    if (srcOut != nullptr) {
        *srcOut = src;
    }
    return best;
}

inline const char* probeUpperSrcText(int src)
{
    switch (src) {
        case 0:  return "频率表长度";
        case 1:  return "/proc/cpuinfo 的 processor 行数";
        case 2:  return "/sys/devices/system/cpu/possible";
        case 3:  return "sysconf(_SC_NPROCESSORS_ONLN)";
        default: return "四个来源都读不到(兜底探测 0..7)";
    }
}

// 逐核实测: {cpu} -> 立刻读回 -> 判定内核是否接受。结束时还原原掩码。
inline AuroraCpuPermitProbe probePermittedCpuSet()
{
    AuroraCpuPermitProbe p;
    p.ran = 1;
    p.tid = (int)::syscall(SYS_gettid);
    p.upperSrc = -1;
    p.upper = probeCpuUpperBound(&p.upperSrc);
    // ---- 先保存原掩码(探测结束后无条件还原) ----
    {
        cpu_set_t cur;
        CPU_ZERO(&cur);
        errno = 0;
        if (sched_getaffinity(0, sizeof(cur), &cur) == 0) {
            p.savedOk = 1;
            for (int c = 0; c < 64; ++c) {
                if (CPU_ISSET(c, &cur)) {
                    p.savedMask |= (1ull << (unsigned)c);
                }
            }
        }
    }
    // ---- 逐核探测 ----
    for (int c = 0; c < p.upper && c < kMaxTopoCpus; ++c) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(c, &one);
        errno = 0;
        const int rc = sched_setaffinity(0, sizeof(one), &one);
        const int setErrno = (rc != 0) ? (errno != 0 ? errno : -1) : 0;
        // 立刻读回(这是唯一能证明"内核接受了"的证据)
        cpu_set_t back;
        CPU_ZERO(&back);
        errno = 0;
        const int rc2 = sched_getaffinity(0, sizeof(back), &back);
        unsigned long long backMask = 0;
        if (rc2 == 0) {
            for (int k = 0; k < 64; ++k) {
                if (CPU_ISSET(k, &back)) {
                    backMask |= (1ull << (unsigned)k);
                }
            }
        }
        const unsigned long long want = (1ull << (unsigned)c);
        if (rc == 0 && rc2 == 0 && backMask == want) {
            p.accepted[c] = 1;
            p.errnoOf[c] = 0;
            p.acceptedCount += 1;
            p.acceptedMask |= want;
            continue;
        }
        p.accepted[c] = 0;
        if (rc != 0) {
            p.errnoOf[c] = setErrno;
            if (setErrno == 1) {
                p.epermCount += 1;
            } else if (setErrno == 22) {
                p.einvalCount += 1;
            } else {
                p.otherFailCount += 1;
            }
        } else if (rc2 != 0) {
            p.errnoOf[c] = -1;        // 返回成功但读回失败
            p.otherFailCount += 1;
        } else {
            // 返回 0 但读回掩码不是那一位 = 内核静默夹回(本次事故最想抓到的形态)
            p.errnoOf[c] = -4;
            p.clamped[c] = 1;
            p.otherFailCount += 1;
        }
    }
    // ---- 还原原掩码(必须做: 后面绑核逻辑要在一个干净的起点上工作) ----
    if (p.savedOk) {
        cpu_set_t back2;
        CPU_ZERO(&back2);
        for (int c = 0; c < 64; ++c) {
            if (((p.savedMask >> (unsigned)c) & 1ull) != 0) {
                CPU_SET(c, &back2);
            }
        }
        errno = 0;
        const int rc = sched_setaffinity(0, sizeof(back2), &back2);
        unsigned long long rb = 0;
        cpu_set_t rbSet;
        CPU_ZERO(&rbSet);
        errno = 0;
        if (rc == 0 && sched_getaffinity(0, sizeof(rbSet), &rbSet) == 0) {
            for (int c = 0; c < 64; ++c) {
                if (CPU_ISSET(c, &rbSet)) {
                    rb |= (1ull << (unsigned)c);
                }
            }
            p.restoreOk = (rb == p.savedMask) ? 1 : 0;
        }
    }
    // ---- 一行文本(逐核 接受/被拒 + errno, 原样可核对) ----
    {
        char acc[128];
        char rej[384];
        char saved[128];
        maskToCoreListText(p.acceptedMask, acc, (int)sizeof(acc));
        rej[0] = '\0';
        int used = 0;
        int rejN = 0;
        for (int c = 0; c < p.upper && c < kMaxTopoCpus; ++c) {
            if (p.accepted[c] != 0) {
                continue;
            }
            char one[48];
            if (p.errnoOf[c] == -4) {
                snprintf(one, sizeof(one), "%scpu%d(返回0但读回被夹回)", (rejN > 0) ? "," : "", c);
            } else if (p.errnoOf[c] < 0) {
                snprintf(one, sizeof(one), "%scpu%d(读回失败)", (rejN > 0) ? "," : "", c);
            } else {
                snprintf(one, sizeof(one), "%scpu%d(errno=%d%s)", (rejN > 0) ? "," : "", c,
                         p.errnoOf[c],
                         (p.errnoOf[c] == 22) ? " EINVAL" : ((p.errnoOf[c] == 1) ? " EPERM" : ""));
            }
            if (used + (int)strlen(one) >= (int)sizeof(rej) - 4) {
                break;
            }
            strncat(rej, one, sizeof(rej) - 1 - (size_t)used);
            used += (int)strlen(one);
            ++rejN;
        }
        if (rejN == 0) {
            snprintf(rej, sizeof(rej), "无");
        }
        maskToCoreListText(p.savedMask, saved, (int)sizeof(saved));
        // 逐核表(接受/被拒 + 该核的 errno): 任务要求"逐核记 接受/被拒/errno", 这里逐位列出
        char perCore[320];
        perCore[0] = '\0';
        {
            int u2 = 0;
            for (int c = 0; c < p.upper && c < kMaxTopoCpus; ++c) {
                char one[32];
                if (p.accepted[c] != 0) {
                    snprintf(one, sizeof(one), "%sc%d=\u63a5\u53d7", (c > 0) ? " " : "", c);
                } else if (p.errnoOf[c] == -4) {
                    snprintf(one, sizeof(one), "%sc%d=\u62d2(\u5939\u56de)", (c > 0) ? " " : "", c);
                } else {
                    snprintf(one, sizeof(one), "%sc%d=\u62d2(errno=%d)", (c > 0) ? " " : "", c, p.errnoOf[c]);
                }
                if (u2 + (int)strlen(one) >= (int)sizeof(perCore) - 2) {
                    break;
                }
                strncat(perCore, one, sizeof(perCore) - 1 - (size_t)u2);
                u2 += (int)strlen(one);
            }
        }
        snprintf(p.text, sizeof(p.text),
                 "实测许可探测(唯一权威来源): 对 cpu0..%d 逐个 sched_setaffinity({cpu}) 后立刻"
                 "sched_getaffinity 读回 -> 内核接受 %d 核(%s); 被拒: %s; EINVAL=%d EPERM=%d 其它=%d; "
                 "探测线程 tid=%d; 核号上界=%d(来自 %s); 探测前掩码=%s 还原=%s; 逐核[%s]",
                 p.upper - 1, p.acceptedCount, acc, rej,
                 p.einvalCount, p.epermCount, p.otherFailCount,
                 p.tid, p.upper, probeUpperSrcText(p.upperSrc), saved,
                 p.savedOk ? (p.restoreOk ? "成功(读回一致)" : "失败(读回不一致)") : "未保存(无法还原)",
                 perCore);
    }
    return p;
}

// 读一次"内核允许本进程用哪些核"。只读; 进程内只调一次(调用方缓存)。
inline AuroraCpuAllowedSet readAllowedSet()
{
    AuroraCpuAllowedSet a;
    // ---- ① /proc/self/status(原样保留两行原文; 注意:这是线程组组长线程的掩码,
    //          不等于调用线程的掩码 —— 这正是"两条读数不一致"的常见来源, 必须逐位摆出来) ----
    {
        char hex[64];
        int e = 0;
        if (readProcLine("/proc/self/status", "Cpus_allowed:", hex, (int)sizeof(hex), &e)) {
            snprintf(a.statusMaskRaw, sizeof(a.statusMaskRaw), "%s", hex);
            a.statusMask = parseHexMask(hex);
            a.statusMaskOk = (a.statusMask != 0) ? 1 : 0;
            maskToCoreListText(a.statusMask, a.statusHexCoreList, (int)sizeof(a.statusHexCoreList));
        } else {
            a.statusErrno = e;
            a.statusMaskErrno = e;
        }
        char list[256];
        int e2 = 0;
        if (readProcLine("/proc/self/status", "Cpus_allowed_list:", list, (int)sizeof(list), &e2)) {
            a.statusListFound = 1;
            snprintf(a.statusListRaw, sizeof(a.statusListRaw), "%s", list);
            int cpus[kMaxTopoCpus];
            const int n = ::aurora_smt_detail::parseCpuMask(list, cpus, kMaxTopoCpus);
            if (n > 0) {
                a.statusListMask = maskFromCpuList(cpus, n);
                a.mask = a.statusListMask;
                a.count = n;
                for (int i = 0; i < n; ++i) {
                    a.cpus[i] = cpus[i];
                }
                maskToCoreListText(a.statusListMask, a.statusListCoreList,
                                   (int)sizeof(a.statusListCoreList));
                a.inThreadMask = 0;
                a.ok = 1;
            }
        } else {
            a.statusListErrno = e2;
            if (a.statusErrno == 0) {
                a.statusErrno = e2;
            }
        }
        char mems[64];
        int e3 = 0;
        if (readProcLine("/proc/self/status", "Mems_allowed_list:", mems, (int)sizeof(mems), &e3)) {
            a.memsFound = 1;
            snprintf(a.memsList, sizeof(a.memsList), "%s", mems);
        }
    }
    // ---- ② sched_getaffinity(0, ...) —— 与 ① 互相印证 ----
    {
        cpu_set_t cur;
        CPU_ZERO(&cur);
        errno = 0;
        if (sched_getaffinity(0, sizeof(cur), &cur) == 0) {
            a.syscallOk = 1;
            unsigned long long m = 0;
            int cpus[kMaxTopoCpus];
            int n = 0;
            for (int c = 0; c < 64; ++c) {
                if (CPU_ISSET(c, &cur)) {
                    m |= (1ull << (unsigned)c);
                    if (n < kMaxTopoCpus) {
                        cpus[n] = c;
                    }
                    ++n;
                }
            }
            // 两条主路径不同时把差异记下来(mismatched), 由界面/note 显示, 不做"取交集"的猜测。
            // 2026-10 起: sched_getaffinity 不再是最终权威 —— 它只是三个读数之一,
            //   最终权威由下面的实测探测(④)决定, 见 authority 字段。
            if (n > 0) {
                if (a.ok && m != a.mask) {
                    a.mismatched = 1;
                }
                a.syscallMask = m;
                maskToCoreListText(m, a.syscallCoreList, (int)sizeof(a.syscallCoreList));
                a.mask = m;
                a.count = (n > kMaxTopoCpus) ? kMaxTopoCpus : n;
                for (int i = 0; i < a.count; ++i) {
                    a.cpus[i] = cpus[i];
                }
                a.inThreadMask = 1;
                a.ok = 1;
            }
        } else {
            a.syscallErrno = errno != 0 ? errno : -1;
        }
    }
    // ---- ③ cgroup / cpuset ----
    {
        char cg[160];
        int e = 0;
        if (readProcLine("/proc/self/cgroup", "0::", cg, (int)sizeof(cg), &e)) {
            snprintf(a.cgroupPath, sizeof(a.cgroupPath), "0::%s", cg);
        } else {
            // cgroup v1: 取第一行原文(fgets 口径), 只为了"属于哪个 cgroup"这一事实
            errno = 0;
            FILE* f = fopen("/proc/self/cgroup", "r");
            if (f == nullptr) {
                snprintf(a.cgroupPath, sizeof(a.cgroupPath), "读不到(errno=%d)", errno != 0 ? errno : -1);
            } else {
                char line[192];
                if (fgets(line, (int)sizeof(line), f) != nullptr) {
                    for (size_t i = 0; i < strlen(line); ++i) {
                        if (line[i] == '\n' || line[i] == '\r') {
                            line[i] = '\0';
                            break;
                        }
                    }
                    snprintf(a.cgroupPath, sizeof(a.cgroupPath), "v1: %s", line);
                } else {
                    snprintf(a.cgroupPath, sizeof(a.cgroupPath), "可读但为空");
                }
                fclose(f);
            }
        }
        // 挂载点(cgroup2 优先, 其次 cgroup)
        char mount[192];
        mount[0] = '\0';
        {
            errno = 0;
            FILE* f = fopen("/proc/self/mountinfo", "r");
            if (f != nullptr) {
                char line[1024];
                while (fgets(line, (int)sizeof(line), f) != nullptr) {
                    char* p = strstr(line, " - cgroup2 ");
                    if (p == nullptr) {
                        continue;
                    }
                    // 第 5 个字段是挂载点
                    int field = 1;
                    char* q = line;
                    while (field < 5 && *q != '\0') {
                        while (*q == ' ') {
                            ++q;
                        }
                        while (*q != ' ' && *q != '\0') {
                            ++q;
                        }
                        ++field;
                    }
                    while (*q == ' ') {
                        ++q;
                    }
                    size_t n = 0;
                    while (q[n] != '\0' && q[n] != ' ' && n + 1 < sizeof(mount)) {
                        mount[n] = q[n];
                        ++n;
                    }
                    mount[n] = '\0';
                    break;
                }
                fclose(f);
            }
        }
        // 拼 cpuset 目录: <挂载点><cgroup 相对路径>
        char rel[160];
        rel[0] = '\0';
        if (a.cgroupPath[0] == '0' && a.cgroupPath[1] == ':' && a.cgroupPath[2] == ':') {
            snprintf(rel, sizeof(rel), "%s", a.cgroupPath + 3);
        }
        if (mount[0] != '\0') {
            if (rel[0] == '\0' || rel[0] == '/') {
                snprintf(a.cpusetDir, sizeof(a.cpusetDir), "%s", mount);
            } else {
                snprintf(a.cpusetDir, sizeof(a.cpusetDir), "%s%s", mount, rel);
            }
        } else if (rel[0] == '/') {
            // 挂载点读不到(cgroup 命名空间): 退而用最常见的挂载点拼一次, 错了也不影响其它判据
            snprintf(a.cpusetDir, sizeof(a.cpusetDir), "/sys/fs/cgroup%s", rel);
        }
        if (a.cpusetDir[0] != '\0') {
            const std::string base(a.cpusetDir);
            int e1 = 0;
            int e2 = 0;
            char buf[64];
            if (::aurora_smt_detail::readTextFile(base + "/cpuset.cpus.effective", buf, (int)sizeof(buf), &e1)) {
                snprintf(a.cpusetEffective, sizeof(a.cpusetEffective), "%s", buf);
            } else {
                a.cpusetEffErrno = e1;
                if (::aurora_smt_detail::readTextFile(base + "/cpuset.effective_cpus", buf, (int)sizeof(buf), &e1)) {
                    snprintf(a.cpusetEffective, sizeof(a.cpusetEffective), "%s", buf);
                    a.cpusetEffErrno = 0;
                }
            }
            if (::aurora_smt_detail::readTextFile(base + "/cpuset.cpus", buf, (int)sizeof(buf), &e2)) {
                snprintf(a.cpusetCpus, sizeof(a.cpusetCpus), "%s", buf);
            } else {
                a.cpusetCpusErrno = e2;
            }
        }
    }
    // ---- ④ 实测许可探测(2026-10 追加): 逐核 sched_setaffinity({cpu}) + 立刻读回 ----
    //  为什么放在最后: 它要覆盖前面从文件/系统调用读到的任何结论, 成为绑核决策的唯一权威。
    //  它测的是"调用线程"的许可集合(而不是"组长线程"的, 也不是"某个文件说的"), 因此当它与
    //  sched_getaffinity 不一致时, 以它为准 —— 理由见本节开头与 probePermittedCpuSet 的注释。
    {
        const AuroraCpuPermitProbe pr = probePermittedCpuSet();
        a.probeRan = pr.ran;
        a.probeTid = pr.tid;
        a.probeUpper = pr.upper;
        a.probeUpperSrc = pr.upperSrc;
        a.probeAcceptedCount = pr.acceptedCount;
        a.probeAcceptedMask = pr.acceptedMask;
        a.probeEinval = pr.einvalCount;
        a.probeEperm = pr.epermCount;
        a.probeOtherFail = pr.otherFailCount;
        a.probeClampedCount = 0;
        for (int c = 0; c < kMaxTopoCpus; ++c) {
            if (pr.clamped[c] != 0) {
                a.probeClampedCount += 1;
            }
        }
        a.probeSavedOk = pr.savedOk;
        a.probeSavedMask = pr.savedMask;
        a.probeRestoreOk = pr.restoreOk;
        snprintf(a.probeText, sizeof(a.probeText), "%s", pr.text);
    }

    // ---- ⑤ 权威来源的选择(绑核决策从此只用这一个集合) ----
    //   优先级: 实测探测 > sched_getaffinity > Cpus_allowed_list > Cpus_allowed(十六进制) > 无。
    //   为什么实测优先: 只有它是行为学读数(内核接受了 {cpu} 才算数), 另外三个都是"别人说的";
    //   真机上正是"文件/系统调用各说各话"才导致整条绑核链建在一个错误的集合上。
    {
        unsigned long long auth = 0;
        int authCount = 0;
        a.authority = 4;
        if (a.probeRan && a.probeAcceptedCount > 0) {
            auth = a.probeAcceptedMask;
            a.authority = 0;
        } else if (a.syscallOk && a.syscallMask != 0ull) {
            auth = a.syscallMask;
            a.authority = 1;
        } else if (a.statusListFound && a.statusListMask != 0ull) {
            auth = a.statusListMask;
            a.authority = 2;
        } else if (a.statusMaskOk && a.statusMask != 0ull) {
            auth = a.statusMask;
            a.authority = 3;
        }
        if (a.authority != 4) {
            a.mask = auth;
            a.count = 0;
            for (int c = 0; c < 64; ++c) {
                if (((auth >> (unsigned)c) & 1ull) != 0) {
                    if (a.count < kMaxTopoCpus) {
                        a.cpus[a.count] = c;
                    }
                    a.count += 1;
                }
            }
            if (a.count > kMaxTopoCpus) {
                a.count = kMaxTopoCpus;
            }
            authCount = a.count;
            a.inThreadMask = (a.authority == 1) ? 1 : 0;
            a.ok = 1;
        } else {
            a.ok = 0;
        }
        // ---- 三条差集(逐位列出; 任务明确要求"不许只给一个 bool") ----
        {
            char d1[224];
            char d2[224];
            char d3[224];
            maskDiffText(a.syscallMask, a.statusMask, "sched_getaffinity", "Cpus_allowed(16 进制)",
                         d1, (int)sizeof(d1));
            maskDiffText(a.syscallMask, a.statusListMask, "sched_getaffinity", "Cpus_allowed_list",
                         d2, (int)sizeof(d2));
            maskDiffText(a.probeRan ? a.probeAcceptedMask : 0ull, a.syscallMask,
                         "实测许可", "sched_getaffinity", d3, (int)sizeof(d3));
            snprintf(a.diffText, sizeof(a.diffText),
                     "差集(逐位): [1] sched_getaffinity vs Cpus_allowed(16 进制): %s; "
                     "[2] sched_getaffinity vs Cpus_allowed_list: %s; "
                     "[3] 实测许可 vs sched_getaffinity: %s",
                     d1, d2, d3);
        }
        // ---- "用的是哪个来源、差在哪"(一行, 原样进 note) ----
        if (a.authority == 0) {
            const bool differFromSyscall = (a.probeAcceptedMask != a.syscallMask);
            snprintf(a.authorityText, sizeof(a.authorityText),
                     "绑核决策的数据源 = 实测探测(tid=%d; 内核接受 %d 核: %s)%s",
                     a.probeTid, a.probeAcceptedCount,
                     (a.listText[0] != '\0') ? a.listText : "",
                     differFromSyscall
                         ? " —— 与 sched_getaffinity(0) 给出的集合不同, 按本文件规则以实测为准"
                         : " —— 与 sched_getaffinity(0) 给出的集合一致");
        } else if (a.authority == 1) {
            snprintf(a.authorityText, sizeof(a.authorityText),
                     "绑核决策的数据源 = sched_getaffinity(0)(实测探测没能拿到任何被接受的核: "
                     "被拒 %d 次其中 EINVAL=%d EPERM=%d), 该读数可能与真实许可集合不符",
                     a.probeEinval + a.probeEperm + a.probeOtherFail, a.probeEinval, a.probeEperm);
        } else if (a.authority == 2 || a.authority == 3) {
            snprintf(a.authorityText, sizeof(a.authorityText),
                     "绑核决策的数据源 = /proc/self/status 的 %s(sched_getaffinity 失败 errno=%d; "
                     "实测探测也没拿到任何被接受的核) —— 注意它读的是组长线程的掩码",
                     (a.authority == 2) ? "Cpus_allowed_list" : "Cpus_allowed(16 进制)",
                     a.syscallErrno);
        } else {
            snprintf(a.authorityText, sizeof(a.authorityText),
                     "绑核决策的数据源 = 无(三个读数全部为空: status errno=%d, "
                     "sched_getaffinity errno=%d, 实测探测接受 0 核) —— 已退回全机频率排名, 不做过滤",
                     a.statusErrno, a.syscallErrno);
        }
        (void)authCount;
    }

    // ---- 派生文本 ----
    maskToCpuList(a.mask, 64, a.listText, (int)sizeof(a.listText));
    if (a.ok) {
        snprintf(a.sourceText, sizeof(a.sourceText),
                 "权威集合 %d 核(%s; 来源=%s; 实测探测接受 %d 核; sched_getaffinity=%s; "
                 "Cpus_allowed_list 原文=%s)",
                 a.count, a.listText,
                 (a.authority == 0) ? "实测探测(唯一权威)" :
                 ((a.authority == 1) ? "sched_getaffinity(0)" :
                 ((a.authority == 2) ? "/proc Cpus_allowed_list" : "/proc Cpus_allowed(16 进制)")),
                 a.probeAcceptedCount,
                 a.syscallOk ? (a.syscallCoreList[0] ? a.syscallCoreList : "空") : "读不到",
                 a.statusListFound ? a.statusListRaw : "读不到");
    } else {
        snprintf(a.sourceText, sizeof(a.sourceText),
                 "允许核集合读不到(status errno=%d, sched_getaffinity errno=%d, 实测探测接受 %d 核) —— "
                 "已退回全机频率排名(不做任何过滤), 此项诊断的 allowed* 字段全部为未知",
                 a.statusErrno, a.syscallErrno, a.probeAcceptedCount);
    }
    // 这一行是"双源对照"的原样打印: 每个读数都带上掩码(十六进制/逐核清单)+ errno, 一个不漏。
    snprintf(a.line, sizeof(a.line),
             "cpuset: 权威集合=%s (%d 核; 来源=%s)",
             (a.listText[0] != '\0') ? a.listText : "读不到", a.count,
             (a.authority == 0) ? "实测探测" : ((a.authority == 1) ? "sched_getaffinity" :
             ((a.authority == 2) ? "proc Cpus_allowed_list" : ((a.authority == 3) ? "proc Cpus_allowed" : "读不到"))));
    {
        char one[512];
        snprintf(one, sizeof(one),
                 " · sched_getaffinity(0)=0x%llx [%s] (%s; errno=%d)",
                 a.syscallMask,
                 (a.syscallCoreList[0] != '\0') ? a.syscallCoreList : "空",
                 a.syscallOk ? "成功" : "失败", a.syscallOk ? 0 : a.syscallErrno);
        strncat(a.line, one, sizeof(a.line) - strlen(a.line) - 1);
        snprintf(one, sizeof(one),
                 " · /proc/self/status Cpus_allowed 原文=\"%s\" (0x%llx [%s]; errno=%d)",
                 (a.statusMaskRaw[0] != '\0') ? a.statusMaskRaw : "读不到", a.statusMask,
                 (a.statusHexCoreList[0] != '\0') ? a.statusHexCoreList : "空", a.statusMaskErrno);
        strncat(a.line, one, sizeof(a.line) - strlen(a.line) - 1);
        snprintf(one, sizeof(one),
                 " · Cpus_allowed_list 原文=\"%s\" [%s] (errno=%d)",
                 (a.statusListRaw[0] != '\0') ? a.statusListRaw : "读不到",
                 (a.statusListCoreList[0] != '\0') ? a.statusListCoreList : "空", a.statusListErrno);
        strncat(a.line, one, sizeof(a.line) - strlen(a.line) - 1);
        if (a.mismatched) {
            strncat(a.line, " · 两个读数不一致(硬事实, 差集见下一段)", sizeof(a.line) - strlen(a.line) - 1);
        }
    }
    if (a.cpusetEffective[0] != '\0') {
        char one[96];
        snprintf(one, sizeof(one), " · cpuset.cpus.effective=%s", a.cpusetEffective);
        strncat(a.line, one, sizeof(a.line) - strlen(a.line) - 1);
    } else {
        char one[96];
        snprintf(one, sizeof(one), " · cpuset.cpus.effective 读不到(errno=%d)", a.cpusetEffErrno);
        strncat(a.line, one, sizeof(a.line) - strlen(a.line) - 1);
    }
    if (a.memsFound) {
        char one[96];
        snprintf(one, sizeof(one), " · Mems_allowed_list=%s", a.memsList);
        strncat(a.line, one, sizeof(a.line) - strlen(a.line) - 1);
    } else {
        strncat(a.line, " · Mems_allowed_list 读不到", sizeof(a.line) - strlen(a.line) - 1);
    }
    if (a.cgroupPath[0] != '\0') {
        char one[192];
        snprintf(one, sizeof(one), " · cgroup=%s", a.cgroupPath);
        strncat(a.line, one, sizeof(a.line) - strlen(a.line) - 1);
    }
    return a;
}

// 可用核集合的世代号: 每次重测(见 auroraRefreshPermitSetOnCallingThread)自增一次,
// 由它把所有派生的缓存(可用核集合本身 / 生效序列 / 拓扑)一起作废重算。
// 为什么需要它(2026-10): 同一个进程的不同线程可能活在不同的 cpuset 里(cgroup v1 允许
// 逐线程挂载), 真机上"cpuset: allowed=0-8"与"线程实际跑在 cpu12/cpu13"就是这种形态。
// 因此"可用核集合"不是进程常量, 必须在真正跑负载的那个线程上重测, 并把旧缓存全部作废。
inline int& permittedGenRef()
{
    static int g = 0;
    return g;
}

// 可用核集合缓存(可重算: 重测后由 auroraRefreshPermitSetOnCallingThread 整体替换)
inline AuroraCpuAllowedSet& allowedSetMutable()
{
    static AuroraCpuAllowedSet a = readAllowedSet();
    return a;
}

inline const AuroraCpuAllowedSet& allowedSetCached()
{
    return allowedSetMutable();
}

// 在调用线程上重新做一次实测许可探测, 并(仅在结果与当前权威集合不同时)整体重建:
//   可用核集合本体 + 生效序列(base) + 拓扑(fastCpus/均衡组/allowedMask)。
// 返回 1 = 集合发生了变化(已重建), 0 = 与原来相同(什么都没动)。
// 位置: 由 auroraAffinitySessionBegin 在计时区间之外调用一次(那是真正跑负载的线程)。
inline int auroraRefreshPermitSetOnCallingThread();   // 前置声明(定义在本节末尾)

// 读取每个核心的最高频率(kHz), 用于把工作线程绑到性能核心(大核优先)。
// 本函数与 bench_cpu.cpp 里原来的 readCoreFreqs() 逐行同源(旧套件改为调用它,
// 因此旧套件读到的频率表与改动前完全一致): 从 cpu0 开始顺序尝试, 第一个打不开的
// 编号即视为末尾(break), 解析失败记 0, 全程不打印任何错误、不抛异常。
inline std::vector<int> readCoreMaxFreqKhz()
{
    std::vector<int> freqs;
    for (int cpu = 0; cpu < 32; ++cpu) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq";
        FILE* f = fopen(path.c_str(), "r");
        if (f == nullptr) {
            break;
        }
        int khz = 0;
        if (fscanf(f, "%d", &khz) != 1) {
            khz = 0;
        }
        fclose(f);
        freqs.push_back(khz);
    }
    return freqs;
}

// 按频率降序得到核心序号, 大核在前。选择排序与原来的 coresByPerfDesc() 逐行同源
// (连"频率相同则保持原顺序"这一点都一样), 所以旧套件拿到的核心顺序与改动前零差异。
inline std::vector<int> coresByPerfDesc(const std::vector<int>& freqs)
{
    std::vector<int> idx;
    for (size_t i = 0; i < freqs.size(); ++i) {
        idx.push_back((int)i);
    }
    for (size_t i = 0; i < idx.size(); ++i) {
        for (size_t j = i + 1; j < idx.size(); ++j) {
            if (freqs[(size_t)idx[j]] > freqs[(size_t)idx[i]]) {
                int t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
        }
    }
    return idx;
}

// 进程级缓存(cpuinfo_max_freq 是 SoC 的静态属性, 运行期不会变)。整个进程只读一次
// sysfs, 频率表 / 频率降序表 / 拓扑全部由同一份数据派生 —— 这样 cpuRank(位次)与
// cpuInFastCluster(是否属于快簇)永远不会因为两次读盘结果不同而自相矛盾。
// C++11 起函数局部 static 的初始化是线程安全的(magic static), 池线程可以放心调用。
inline const std::vector<int>& coreMaxFreqKhzCached()
{
    static const std::vector<int> freqs = readCoreMaxFreqKhz();
    return freqs;
}

inline const std::vector<int>& perfOrderCached()
{
    static const std::vector<int> order = coresByPerfDesc(coreMaxFreqKhzCached());
    return order;
}

// ---------------------------------------------------------------------------
//  "快簇 / 均衡组"的划分规则 —— 纯函数, 可在编译期求值(便于用 static_assert 复核)
// ---------------------------------------------------------------------------
//  输入 freqBySlot[]: 已经按频率降序排好的每个"位次"上的最高频率(kHz)。
//  输出: groupOfSlot / groupFirst / groupSize / fastCores。
//  不变式(频率表可读时成立):
//    (a) 每个组的核数 >= 2 —— 即永远不会产出单核掩码。唯一例外: 机器总共只有两个组、
//        且最后剩下的就是那 1 个核时(例如 3 核 = 2 个大核 + 1 个小核), 这个尾部的核会
//        保留成单核组 —— 两害相权取其轻: 把它并进组 0 会污染快簇, 让单核负载有概率再次
//        掉到小核, 那正是本次要修的问题; 而单核组只影响多核阶段编号最大的那一个池线程;
//    (b) 组 0(快簇)恒 >= 2 个核(单核机器除外), 且只包含最高频的那批核;
//    (c) 组的频率范围按组号单调不增;
//    (d) 频率表可读时所有组恰好覆盖 0..slotCount-1, 不重不漏; 频率不可读时组数恒为 0
//        (此时 slotCount > 0, 组划分不存在 —— 调用方必须按"不绑"处理)。
constexpr int kMaxCores = 32;   // 与 readCoreMaxFreqKhz() 的扫描上限一致

struct CpuTopo {
    int freqBySlot[kMaxCores];    // 位次 -> 最高频率(kHz)
    int groupOfSlot[kMaxCores];   // 位次 -> 组号
    int groupFirst[kMaxCores];    // 组号 -> 该组第一个位次
    int groupSize[kMaxCores];     // 组号 -> 该组核数
    int groupCount;               // 组数
    int slotCount;                // 频率表里实际用到的核数(<= kMaxCores)
    int fastMaxKhz;               // 组 0(快簇)的最高频率(kHz); 0 = 频率不可读
    int fastCores;                // 组 0(快簇)的核数; 0 = 频率不可读 -> 不绑
};

constexpr CpuTopo buildTopo(const int* freqBySlot, int slotCount)
{
    CpuTopo t{};
    int n = slotCount;
    if (n < 0) {
        n = 0;
    }
    if (n > kMaxCores) {
        n = kMaxCores;
    }
    t.slotCount = n;
    for (int s = 0; s < n; ++s) {
        t.freqBySlot[s] = freqBySlot[s];
    }
    if (n <= 0) {
        return t;
    }
    t.fastMaxKhz = t.freqBySlot[0];
    if (t.fastMaxKhz <= 0) {
        // 频率解析失败(全是 0)当作"读不到频率表": 不绑, 静默降级
        t.fastMaxKhz = 0;
        return t;
    }
    int g = 0;
    int s = 0;
    while (s < n) {
        const int first = s;
        int size = 0;
        do {
            const int f = t.freqBySlot[s];
            int j = s;
            while (j < n && t.freqBySlot[j] == f) {   // 同一频率档
                ++j;
            }
            size += (j - s);
            s = j;
        } while (size < 2 && s < n);                  // 不足 2 个核就继续吸收下一档
        t.groupFirst[g] = first;
        t.groupSize[g] = size;
        for (int k = first; k < first + size; ++k) {
            t.groupOfSlot[k] = g;
        }
        ++g;
    }
    // 收尾: 核总数为奇数时最后一组可能只剩 1 个核 -> 并入上一组。
    // 但 g == 2 时"上一组"就是快簇(组 0): 把小核并进快簇会让单核负载有概率掉到小核,
    // 正是本次要修的问题, 所以这种情况下宁可留一个单核组(见文件头不变式 (a) 的例外)。
    if (g >= 3 && t.groupSize[g - 1] == 1) {
        t.groupOfSlot[t.groupFirst[g - 1]] = g - 2;
        t.groupSize[g - 2] += 1;
        --g;
    }
    t.groupCount = g;
    t.fastCores = (g > 0) ? t.groupSize[0] : 0;
    return t;
}

// 运行期拓扑(由频率表实例化的 CpuTopo + 具体的核号)
struct CpuTopology {
    std::vector<int> order;        // 位次 -> 核号(频率降序)
    std::vector<int> groupOfSlot;  // 位次 -> 组号
    std::vector<int> groupFirst;   // 组号 -> 该组第一个位次
    std::vector<int> groupSize;    // 组号 -> 该组核数
    std::vector<int> fastCpus;     // 快簇成员(核号, 频率降序)
    int maxKhz;                    // 快簇最高频率; 0 = 快簇未定义
    int cores;                     // 频率表里的核数
    unsigned long long fastMask;   // 快簇位图
    int physOfCpu[kMaxTopoCpus];   // 逻辑核 -> 物理核编号(SMT 关掉时的池线程落点分配只用它)
    int groupCount;                // 均衡组数(= 物理核数上限, SMT 关掉时一个池线程一组)
    // ---- 多核并行池的落点表(物理核口径; 2026-10-07 新增, 见 buildEffectiveTopology 的说明) ----
    //   spreadOrder = order 的"每个物理核只留一个代表"投影(频率位次最靠前的那个逻辑核),
    //   同物理核上的 SMT 兄弟不进并行池。拓扑未知 / 无 SMT 时它与 order 逐位相同。
    std::vector<int> spreadOrder;  // 池线程 i -> spreadOrder[i](一物理核一线程)
    int spreadPhysical;            // spreadOrder 覆盖的物理核数(= 池线程数上限)
    // ---- 内核允许的核集合 ∩ 全机频率排名(2026-10-05 追加; 见本文件"允许核集合"一节) ----
    unsigned long long allowedMask;   // 可用核集合位图(只在 allowedRead=1 时才用于过滤 —— 掩码为 0 不等于读不到)
    int allowedRead;                  // 1 = 可用核集合读到了(此时即使只为 0 也按"交集为空"处理, 不退回历史行为)
    int allowedOk;                    // 1 = 可用核集合有效
    int allowedCount;                 // 可用核集合里的核数(0 = 读不到)
    unsigned long long bootMask;      // 会话建立时真正应用的那张掩码(排障用; 由会话填写)
    int fallbackTarget;               // 1 = 交集为空, 快簇退化成"可用核集合里最快的那些核"
    int effTierIndex;                 // 生效快簇所在的频率档位次(0 = 全机最快档)
    int machineTopTierCores;          // 全机最快频率档的核数(与可用核集合无关)
    int machineTopTierKhz;            // 全机最快频率档的频率(kHz)
    int machineTopTierSlotFirst;      // 全机最快档在 order[] 里的起始位次
    unsigned long long machineTopTierMask;   // 全机最快档的位图("跑没跑在全机最快档"的判据)

    CpuTopology()
        : maxKhz(0), cores(0), fastMask(0), groupCount(0), spreadPhysical(0),
          allowedMask(0), allowedRead(0), allowedOk(0), allowedCount(0), bootMask(0), fallbackTarget(0),
          effTierIndex(0), machineTopTierCores(0), machineTopTierKhz(0),
          machineTopTierSlotFirst(0), machineTopTierMask(0)
    {
        for (int i = 0; i < kMaxTopoCpus; ++i) {
            physOfCpu[i] = i;
        }
    }
};

// 运行期拓扑: 输入"已经按频率降序排好、且只含实际使用集合的核心序列"。
// 均衡组划分规则与历史实现逐字一致(同一份 buildTopo 纯函数, 频率档合并 / 组 >= 2 核 /
// 奇数尾巴并入上一组的收尾规则都没动), 只是喂进去的序列可能是"每个物理核一个逻辑核":
//   * 开关开(默认): 序列 = 全部逻辑核的频率降序 => 与改动前逐位一致;
//   * 开关关      : 序列 = 每个物理核选出的代表(频率降序)=> 每组恰好落在不同的物理核上,
//                   因此"一个池线程拿一个组"天然保证不会有两个池线程共享同一个物理核。
//
//  可用核集合过滤(2026-10-05, 本节最关键的改动)
//   绑核的实际语义是"在这条序列的前若干位上放一个掩码"。若序列里混进了**内核不允许我们
//   用的核**(厂商把它留给前台/系统线程), 那么:
//     * 这些核永远排在最前面(它们频率最高) -> 快簇掩码全部落在不允许的核上 ->
//       sched_setaffinity 成功但被内核夹回可用核集合 -> 等于根本没绑(真机现象:
//       "已绑定到大核簇, 跑完却在核外");
//     * 池线程同样会被分到不允许的核上, 被静默夹回后反而挤在可用核集合里的某几个核上乱跑。
//   因此这里只把可用核集合内的核放进 order; 可用核集合本身就是"可用核集合里最快的那些核"
//   (order 已按频率降序), 于是 out.order[0] 恒为"能在不违反系统策略的前提下拿到的最好位置"。
//   交集为空(可用核集合读到了, 但与任何有频率读数的核都不相交)时不假装绑定成功:
//   退化成"可用核集合里最快的那个核"单核目标, 并把 fallbackTarget 置 1, 由会话层标注
//   cpuBound=false + 原因(见 auroraAffinitySessionBegin)。
//   可用核集合读不到(allowedRead=0)-> 不做任何过滤(与历史行为逐位一致), 只把"读不到"记进诊断。
inline CpuTopology buildTopology(const std::vector<int>& freqs, const std::vector<int>& order)
{
    CpuTopology t;
    for (int i = 0; i < kMaxTopoCpus; ++i) {
        t.physOfCpu[i] = i;
    }
    const AuroraCpuAllowedSet& a = allowedSetCached();
    t.allowedRead = a.ok;
    t.allowedMask = a.ok ? a.mask : 0ull;
    t.allowedCount = a.ok ? a.count : 0;

    // ---- 全机口径(不受可用核集合影响): 频率降序位次 + 整机最快频率档 ----
    // "跑没跑在全机最快档"必须与"绑核是否生效"分开报, 所以这里保留一份过滤前的划分。
    // 频率档的边界用"频率值变化"判定 —— 与 buildTopo 内部"同频并档"的规则逐字一致。
    //
    // 2026-10-06 方向性错误修正(这是本次最要紧的一处)
    //   这段以前遍历的是传进来的 order, 而 buildEffectiveTopology 传进来的 order 已经在
    //   effectiveOrderCached() 里按"可用核集合"过滤过了(只留内核允许本进程用的核)。于是
    //   "全机最快频率档"被算成了"可用核集合里最快的那些核" —— 判据本身没错, 数据源错了。
    //   真机证据(Pura X Max / HOP-AL00, 305 行 runlog):
    //     内核只允许 cpu0-8(9 核), 全机最快档 2750MHz 在 9-13, 生效快簇 = cpu4-8 @2270MHz。
    //     日志里"在全机最快档内"出现 48 次、"不在全机最快档"0 次, 而同一条 note 里还写着
    //     "快簇已生效(cpu=8, 5 核簇/最高2270MHz, 但低于全机最快档 1 档)" —— 同一行自相矛盾。
    //   现在改为从 perfOrderCached()/coreMaxFreqKhzCached() 取全机逐核 cpuinfo_max_freq 的
    //   频率降序表(与可用核集合无关)重新划分, 判据回到"该核的最高频率是否 == 全机最高频率"。
    //   可用核集合不含全机最快档的机器上, cpuInMachineTopTier 因此必然为 false。
    {
        const std::vector<int>& allOrder = perfOrderCached();
        const std::vector<int>& allFreqs = coreMaxFreqKhzCached();
        const int n0 = (int)allOrder.size();
        const int lim = (n0 < kMaxCores) ? n0 : kMaxCores;
        int prevKhz = -1;
        int tier = -1;
        for (int s = 0; s < lim; ++s) {
            const int cpu = allOrder[(size_t)s];
            const int khz = (cpu >= 0 && (size_t)cpu < allFreqs.size()) ? allFreqs[(size_t)cpu] : 0;
            if (khz != prevKhz) {
                ++tier;
                prevKhz = khz;
            }
            if (tier == 0) {
                if (s == 0) {
                    t.machineTopTierKhz = khz;
                    t.machineTopTierSlotFirst = s;
                }
                // khz > 0: 读不到频率的核不算进"全机最快档"(否则整表读不到时会把全部核
                // 都算成最快档, 那是"假装知道"); 此时 machineTopTierCores 保持 0,
                // auroraCpuInMachineTopTier 恒为 false —— 与"频率未知"这一事实一致。
                if (khz > 0 && khz == t.machineTopTierKhz) {
                    ++t.machineTopTierCores;
                    if (cpu >= 0 && cpu < 64) {
                        t.machineTopTierMask |= (1ull << (unsigned)cpu);
                    }
                }
            }
        }
    }

    // ---- 生效口径: 只保留可用核集合内的核(交集为空时退化成"可用核集合里最快的核") ----
    std::vector<int> eff;
    eff.reserve(order.size());
    if (t.allowedRead) {
        for (size_t i = 0; i < order.size(); ++i) {
            const int cpu = order[i];
            if (cpu >= 0 && cpu < 64 && ((t.allowedMask >> (unsigned)cpu) & 1ull) != 0) {
                eff.push_back(cpu);
            }
        }
    }
    if (eff.empty()) {
        if (t.allowedRead) {
            // 可用核集合读到了, 但与"有频率读数的核"完全不相交(或可用核集合不含任何有效核号):
            // 不假装绑定成功 —— 目标是"可用核集合里最快的那个核"(核号最小的一个), 由会话层标注。
            t.fallbackTarget = 1;
            for (int c = 0; c < 64; ++c) {
                if (((t.allowedMask >> (unsigned)c) & 1ull) != 0) {
                    eff.push_back(c);
                    break;
                }
            }
        }
        if (eff.empty()) {
            eff = order;   // 可用核集合没读到(或为空且读不到) -> 不做过滤, 与历史行为逐位一致
        }
    }
    t.order = eff;
    t.cores = (int)t.order.size();
    if (t.cores <= 0) {
        return t;
    }
    const int n = (t.cores < kMaxCores) ? t.cores : kMaxCores;
    int slotFreq[kMaxCores];
    for (int s = 0; s < n; ++s) {
        const int cpu = t.order[(size_t)s];
        slotFreq[s] = (cpu >= 0 && (size_t)cpu < freqs.size()) ? freqs[(size_t)cpu] : 0;
    }
    const CpuTopo m = buildTopo(slotFreq, n);
    if (m.fastCores <= 0) {
        return t;   // 读不到频率表 -> 快簇未定义, 调用方静默降级为"不绑"
    }
    t.maxKhz = m.fastMaxKhz;
    t.groupOfSlot.assign(m.groupOfSlot, m.groupOfSlot + n);
    t.groupCount = m.groupCount;
    t.groupFirst.reserve((size_t)m.groupCount);
    t.groupSize.reserve((size_t)m.groupCount);
    for (int g = 0; g < m.groupCount; ++g) {
        t.groupFirst.push_back(m.groupFirst[g]);
        t.groupSize.push_back(m.groupSize[g]);
    }
    t.fastCpus.reserve((size_t)m.fastCores);
    for (int s = 0; s < m.fastCores; ++s) {
        const int cpu = t.order[(size_t)s];
        t.fastCpus.push_back(cpu);
        if (cpu >= 0 && cpu < 64) {
            t.fastMask |= (1ull << (unsigned)cpu);
        }
    }
    // ---- 生效快簇落在全机的第几个频率档(0 = 全机最快档) ----
    // 两个口径的频率档边界都用同一条规则(频率值变化即新档), 所以这个下标可以直接比较:
    //   effTierIndex == 0  => 生效快簇就在全机最快的那个频率档上(这才是"绑到了真大核");
    //   effTierIndex >  0  => 生效快簇低于全机最快档(native 会在 note 里写明差了几档)。
    //   而"跑完那一刻在不在全机最快档"是另一个独立事实(判据 machineTopTierMask), 由
    //   cpuInFastCluster(生效) 与 cpuInMachineTopTier(全机) 两个字段分开报, 不混成一个。
    //
    // 2026-10-06 修正 以前这里的 tier 数是"生效序列里不同频率值的个数 - 1", 那回答的是
    //   "可用核集合里被切成了几档", 不是"生效快簇落在全机的第几档" —— 两者只在特定数据上碰巧相等
    //   (真机恰好都是 1, 所以 note 里那句"低于全机最快档 1 档"看着是对的)。现在改成正规算法:
    //   在全机频率降序表里数"比生效快簇最高频更高的、互不相同的频率值有几个", 那就是档位。
    {
        const std::vector<int>& allOrder = perfOrderCached();
        const std::vector<int>& allFreqs = coreMaxFreqKhzCached();
        const int n0 = (int)allOrder.size();
        const int lim = (n0 < kMaxCores) ? n0 : kMaxCores;
        int above = 0;
        int prevKhz = -1;
        bool found = false;
        for (int s = 0; s < lim; ++s) {
            const int cpu = allOrder[(size_t)s];
            const int khz = (cpu >= 0 && (size_t)cpu < allFreqs.size()) ? allFreqs[(size_t)cpu] : 0;
            if (khz == prevKhz) {
                continue;   // 同一档, 只看档边界
            }
            prevKhz = khz;
            if (t.maxKhz > 0 && khz == t.maxKhz) {
                found = true;   // 找到生效快簇最高频所属的档: 上面数过的就是档位
                break;
            }
            if (khz > t.maxKhz) {
                ++above;
            }
        }
        if (found) {
            t.effTierIndex = above;
        } else {
            // 兜底(生效快簇最高频不在全机频率表里: 读数为 0 或半张表读不到):
            // 退回"生效序列里不同频率值个数 - 1", 与改动前的行为逐位一致, 不假装更准。
            int tier = -1;
            int prev2 = -1;
            for (int s = 0; s < n; ++s) {
                if (slotFreq[s] != prev2) {
                    ++tier;
                    prev2 = slotFreq[s];
                }
            }
            t.effTierIndex = (tier < 0) ? 0 : tier;
        }
    }
    return t;
}

// 实际使用集合(频率降序的核号序列)。开关一变就整体重算 ——
// 因此绑核拓扑与线程数永远出自同一份数据, 不会各说各话。
//
// 2026-10-07 修正(这是"关掉 SMT 后线程数不对"的第二个成因)
//   "每个物理核只留一个逻辑核"这一步必须在可用核集合之内做, 顺序反过来会丢核:
//   旧写法先用全机频率表挑代表, 再拿可用核集合过滤 —— 代表若落在可用核集合之外的核上
//   (真机: 全机最快档 2750MHz 的那两个核在可用核集合 cpu0-8 之外), 那个物理核的代表就被整条
//   丢掉, 于是"关掉 SMT 后线程数 = 可用核集合里的物理核数"这条断言不成立(会凭空少几个线程)。
//   现在改成: 先把频率降序表与可用核集合求交(base), 再在 base 里挑代表 —— 某个物理核只要还有
//   一个内核允许我们用的逻辑核, 它就不会从实际使用集合里消失。
//   开关开时两种顺序逐位相同(都等于"可用核集合里的全部逻辑核"), 默认行为不变。
inline const std::vector<int>& effectiveOrderCached()
{
    const std::vector<int>& order = perfOrderCached();
    const AuroraCpuAllowedSet& a = allowedSetCached();
    // ① base = 频率降序表 ∩ 可用核集合(可用核集合读不到 / 交集为空 -> 不过滤, 与历史行为一致)
    //    2026-10: 用世代号而不是一次性的 bool 作为有效性判据 —— 可用核集合在
    //    "真正跑负载的那个线程"上重测后可能变化(见 auroraRefreshPermitSetOnCallingThread),
    //    那时这条序列必须整体重建, 否则绑核/线程数/落点会继续按旧集合算。
    static std::vector<int> base;
    static int baseGen = -1;
    if (baseGen != permittedGenRef()) {
        base.clear();
        if (a.ok) {
            for (size_t i = 0; i < order.size(); ++i) {
                const int cpu = order[i];
                // 可用核集合过滤(2026-10-05): 内核不允许我们用的核不进这条序列 ——
                //   否则池线程会被分到不允许的核上, 被静默夹回后全挤在可用核集合的某几个核上。
                if (cpu >= 0 && cpu < 64 && ((a.mask >> (unsigned)cpu) & 1ull) != 0) {
                    base.push_back(cpu);
                }
            }
        }
        if (base.empty()) {
            base = order;
        }
        baseGen = permittedGenRef();
    }
    // ② SMT 口径: 开 -> base 全部; 关 -> base 里每个物理核一个代表。
    //    这里刻意不用 auroraEffectiveSet()/effectiveSetCached(): 那一份是"全机口径"
    //    (它回答的是"不看可用核集合时本应几个线程", 见 auroraSmtThreads / threads.basisFull),
    //    它不认识可用核集合 —— 拿它挑代表正是上面那个会丢核的旧写法。
    static AuroraCpuEffectiveSet s;
    static std::vector<int> out;
    static int builtFor = -1;
    static int builtGen = -1;
    const int now = ::auroraSmtEnabledFlag() ? 1 : 0;
    if (builtFor != now || builtGen != permittedGenRef()) {
        s = ::aurora_smt_detail::buildEffectiveSet(
            ::aurora_smt_detail::topologyInfoCached(), base, now);
        out.clear();
        for (int i = 0; i < s.count; ++i) {
            const int cpu = s.cpus[i];
            if (a.ok && !(cpu >= 0 && cpu < 64 && ((a.mask >> (unsigned)cpu) & 1ull) != 0)) {
                continue;   // 双保险: buildEffectiveSet 的输入已经是 base, 正常不会命中
            }
            out.push_back(cpu);
        }
        if (out.empty() && !a.ok) {
            // 兜底只在"可用核集合读不到"时生效(此时不做任何过滤, 与历史行为逐位一致)。
            // 可用核集合读到了却与任何核都不相交时, 刻意不在这里兜底: 那条路径交给
            // buildTopology 统一处理(它会把快簇退化成"可用核集合里最快的核"并把 fallbackTarget
            // 置 1, 由会话层标注 cpuBound=false + 原因)。这样"交集为空"的语义只有一处实现。
            out = order;
        }
        builtFor = now;
        builtGen = permittedGenRef();
    }
    return out;
}

// 前置声明: 拓扑缓存的构造(定义在下面; 这里只用它的"每次调用都返回同一份"语义)
inline const CpuTopology& topologyCached();

// 进程级拓扑缓存: cpuinfo_max_freq 是 SoC 的静态属性, 运行期不会变。首次调用读一次
// sysfs(与历史实现的读盘次数同一个数量级), 之后只读内存。
//
// SMT: 这里缓存的 order / fastCpus / 均衡组都建立在实际使用集合上 ——
//   开关开(默认): 全部逻辑核 => 与改动前逐位一致;
//   开关关      : 每个物理核一个逻辑核 => 线程数、快簇掩码、池线程落点全部随之收窄。
// 于是"用哪些核"只有一处定义(实际使用集合), 绑核 / 诊断 / 线程数三条路径永远自洽。
inline CpuTopology buildEffectiveTopology()
{
    const std::vector<int>& freqs = coreMaxFreqKhzCached();
    // 传入"频率降序 + SMT 口径"的序列; 可用核集合的交集在 buildTopology 内部做 ——
    // 只留这一处定义, 免得线程数 / 快簇 / 落点三条路径各有一份交集逻辑(那样迟早各说各话)。
    const std::vector<int>& order = effectiveOrderCached();
    CpuTopology t = buildTopology(freqs, order);
    const AuroraCpuTopoInfo& info = ::aurora_smt_detail::topologyInfoCached();
    if (info.known) {
        for (int c = 0; c < kMaxTopoCpus; ++c) {
            t.physOfCpu[c] = info.physicalOfCpu[c];
        }
    }
    // -----------------------------------------------------------------------
    //  多核并行池的落点表 = order 的"物理核口径"投影(2026-10-08 新增)
    // -----------------------------------------------------------------------
    //  规则: 沿 order(频率降序)走一遍, 每个物理核只收下**第一个**遇到的逻辑核,
    //        同一个物理核上后面的逻辑核(SMT 兄弟)不进并行池。
    //
    //  为什么必须这样(真机 8.5 证据, 全部有据可查):
    //    设备 HUAWEI Pura X Max / HOP-AL00: 14 逻辑核 / 9 物理核, 内核允许本进程用 cpu0-7
    //    (8 个逻辑核), 而这 8 个逻辑核只落在 6 个物理核上:
    //        c0-c3 = 4 个小核(无 SMT), {c4,c5} = 大核 P4(SMT), {c6,c7} = 大核 P5(SMT)。
    //    旧口径把并行池按**逻辑核**铺(8 条线程), 于是其中 2 条被钉在 SMT 兄弟 c5 / c6 上。
    //    后果(同一台机器, 两条独立证据):
    //      ① App 自己测的并行度(= 各线程 CPU 时间之和 / 墙钟, bench_cpu.cpp
    //         out.parallelism = cpuSum / ms): 8 项多核负载在 8 线程下的实测值是
    //         5.90 / 3.78 / 6.04 / 5.09 / 6.10 / 6.11 / 6.09 / 5.47 —— 天花板 ≈ 6.1,
    //         一次都没接近 8。6 正是这台机器**可用物理核数**。
    //      ② 华为 SmartPerf 逐秒逐核占用(data3.zip, 378 s): 多核阶段(第 283~357 秒)
    //         全程没有任何一颗核的占用 >= 90%, 逐核占用中位只有 ~50-70%; 8 个可用逻辑核
    //         里同时 >= 50% 的最多 6 个。两个线程挤在同一个物理核上时, 每条硬件线程各拿
    //         ~50%, 于是"没有任何一颗核能到 90%"成为必然 —— 这不是负载不够并行, 而是
    //         并行单元(线程)数超过了物理并行单元(物理核)数。
    //    因此: 池线程数 = **可用物理核数**, 落点 = 每个物理核一个代表(频率位次最靠前的那个),
    //    SMT 兄弟不再进池。工作总量 / 算法 / 计分公式 / k / conv / 单位一个字都不动 ——
    //    变的只是"用几条线程、钉在哪几颗核上"。
    //
    //  退化规则(必须保持): 拓扑未知(读不到 sysfs)、或机器本来就没有 SMT(物理核数 == 逻辑核数)
    //    时, 投影是恒等映射 —— spreadOrder 与 order 逐位相同, 线程数与落点和改动前**完全一致**。
    //    也就是说这次改动对非 SMT 设备是零行为变化。
    {
        const bool haveTopo = (info.known != 0 && info.smtPossible != 0);
        bool physUsed[kMaxTopoCpus];
        for (int i = 0; i < kMaxTopoCpus; ++i) {
            physUsed[i] = false;
        }
        t.spreadOrder.clear();
        t.spreadPhysical = 0;
        for (size_t i = 0; i < t.order.size(); ++i) {
            const int cpu = t.order[i];
            if (!haveTopo || cpu < 0 || cpu >= kMaxTopoCpus) {
                t.spreadOrder.push_back(cpu);   // 无 SMT / 核号异常: 原样保留(与历史行为逐位一致)
                continue;
            }
            const int ph = t.physOfCpu[cpu];
            if (ph < 0 || ph >= kMaxTopoCpus) {
                t.spreadOrder.push_back(cpu);
                continue;
            }
            if (physUsed[ph]) {
                continue;                        // 同一物理核上的 SMT 兄弟: 不进并行池
            }
            physUsed[ph] = true;
            ++t.spreadPhysical;
            t.spreadOrder.push_back(cpu);
        }
        if (t.spreadOrder.empty()) {
            // 兜底: 序列为空(频率表与可用核集合都读不到)时与 order 保持一致
            t.spreadOrder = t.order;
            t.spreadPhysical = 0;
        }
    }
    return t;
}

inline const CpuTopology& topologyCached()
{
    // 2026-10: 从"一次性的 static const"改成"按世代号可重算"
    //   原因: 可用核集合可能在同一个进程内因线程而异(cgroup v1 允许逐线程 cpuset), 因此
    //   会话开始时会在真正跑负载的那个线程上重测一次许可集合; 重测结果一旦与旧集合不同,
    //   这里就必须整体重建(否则快簇/线程数/池线程落点会继续按旧集合算 —— 那正是本次事故)。
    static CpuTopology t;
    static int builtGen = -1;
    if (builtGen != permittedGenRef()) {
        t = buildEffectiveTopology();
        builtGen = permittedGenRef();
    }
    return t;
}

// 在调用线程上重测许可集合(实测探测), 与当前权威集合不同时整体作废派生缓存。
// 返回 1 = 集合变了(已重建), 0 = 没变。只由会话层在计时区间之外调用。
// 为什么不复用 allowedSetCached(): 那份是按进程缓存的, 而本函数要回答的是
// "我这个线程现在到底被允许用哪些核" —— 真机证据表明这两个问题的答案可以不同。
inline int auroraRefreshPermitSetOnCallingThread()
{
    const AuroraCpuAllowedSet fresh = readAllowedSet();   // 含实测探测与全部派生文本
    AuroraCpuAllowedSet& cur = allowedSetMutable();
    const bool changed = ((fresh.ok != cur.ok) || (fresh.mask != cur.mask));
    cur = fresh;                                          // 无论如何都刷新读数与文本
    if (changed) {
        ++permittedGenRef();                              // 作废 base / 生效序列 / 拓扑缓存
        return 1;
    }
    return 0;
}


// 把"当前线程"绑到 order[] 里 [first, first+count) 这一段位次对应的核集合。
// 返回成功写进掩码的核数(>= 1), -1 = 没有可用核或 sched_setaffinity 失败。
// 不分配内存: 池线程在计时区间内会调用它(每个并行池一次/线程), 这里只碰栈上的 cpu_set_t。
//
//  errno 必须能拿到(2026-10-05)
//   此前失败只返回 -1, 调用方无法区分"这段位次里一个合法核都没有"(掩码为空, 根本没调
//   syscall)和"sched_setaffinity 真的失败了"(EINVAL = 掩码与可用核集合无交集 / EPERM)。
//   两者的处置完全不同(前者是"绑核无效", 后者是"内核拒绝"), 所以这里把 errno 原样带出去。
//   注意: order 里只会有可用核集合内的核(见 buildTopology), 所以 EINVAL 在本工程里
//   理论上不该出现 —— 一旦出现, 它本身就是"内核拒绝"这条硬证据。
inline int bindSlotsEx(const std::vector<int>& order, int first, int count, int* errOut)
{
    if (errOut != nullptr) {
        *errOut = 0;
    }
    const int total = (int)order.size();
    if (first < 0 || count <= 0 || first >= total) {
        return -1;
    }
    int end = first + count;
    if (end > total) {
        end = total;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    int n = 0;
    for (int s = first; s < end; ++s) {
        const int cpu = order[(size_t)s];
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            continue;
        }
        CPU_SET(cpu, &set);
        ++n;
    }
    if (n == 0) {
        if (errOut != nullptr) {
            *errOut = -3;   // -3 = 这段位次里一个合法核都没有, 连 syscall 都没发出去
        }
        return -1;
    }
    errno = 0;
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        if (errOut != nullptr) {
            *errOut = errno != 0 ? errno : -1;
        }
        return -1;
    }
    return n;
}

inline int bindSlots(const std::vector<int>& order, int first, int count)
{
    return bindSlotsEx(order, first, count, nullptr);
}

// 一次负载的绑定目标位次数(单核 / 多核两个阶段) —— 全工程唯一来源。
//
//  单核阶段: 生效快簇 = (全机频率最快档) ∩ (可用核集合) = order 的前 fastCpus.size() 位。
//            交集为空(fallbackTarget)时退化为整条可用序列 —— 单核掩码只剩一个核时,
//            内核会以 misfit 为由推翻它(见本文件头), 所以"退化"这一步是必须的。
//  多核阶段: 全部可用核(order 的整条序列), 池线程再各自独占 order[i]。
//
//  为什么必须只有这一处(2026-10 事故的形态): 自研套件(bench_cpu.cpp)此前自己算了一套
//  "绑到某个具体核号"的老路径, 与 CS1 会话这条链各写一遍。结果是同一颗芯片的两台设备上
//  CS1 单核和自研套件单核给出互相矛盾的名次(自研套件那条路径的单核阶段当时甚至完全不绑核)。
//  现在两条路径调的是同一个函数, "两条链各算一遍"从结构上不可能再发生。
// ===========================================================================
//  多核阶段的落点表 —— 全工程唯一来源(2026-10-10 修复)
//
//  ★ 不变式: **池线程数 == 这张表的长度**, 而且这张表就是"可用核序列"本身
//    (t.order = effectiveOrderCached() = 内核允许的那些逻辑核, 真机 8)。
//    线程数那一侧的同一个来源是 auroraThreadCap() —— 它返回的正是这条序列的长度。
//    "要 N 条线程、却只有 M 个格子(N > M)"是缺陷状态, 不许再出现; 由 verify_core_spread.py
//    与 verify_physical_core_pool.py 的断言钉死。
//
//  事故(真机 HOP-AL00, 同一台机器 / 同一个包 / 同一颗芯片):
//    套件        8.5(OpenHarmony SDK)   9.6(HarmonyOS SDK)
//    CS1 单核    418.2                  416
//    CS1 多核    1492.8                 1239    <- 低 17%
//    CS1 GPU     4522.4                 5241
//    自研 单核    2799                   2663
//    自研 多核    6276                   6269    <- 一模一样(满血回来)
//    自研 GPU     1070                   1111
//    ★ 决定性的是自研那一组: 同一个包、同一套 SDK、同一颗芯片, 自研多核只差 0.1% ——
//      所以不是 SDK 的锅, 是 CS1 这条路径自己身上。
//
//  根因: 2026-10-07/08 把多核落点表改成了"每物理核一个代表"的投影(spreadOrder, 真机 6 格),
//    并把线程数一起夹到 6。9.1 按真机 A/B(8 线程 1481.6/1488.4/1540.2 vs 6 线程 1069.8,
//    慢 34%)把**线程数**回退成可用逻辑核数, **但落点表没跟着回退**。
//    于是 8 条线程往 6 个格子上钉: index 6/7 >= 表长, bindWorkerCoreSpread 返回 -5 不绑,
//    auroraAffinityWorkerStart 再把这两条线程夹回 s.spreadMask(= 那 6 个核) ——
//    它们只能与已经独占那 6 个核的线程抢时间片, 实测并行度因此回到 ~6, 多核分掉 17%。
//    (自研套件没有这个"失败就夹回 6 核掩码"的分支: 它钉不上就不钉、保留原有宽掩码,
//     能落到空闲的 SMT 兄弟上, 所以同一版里它满血 —— 这也解释了两个套件为什么表现不同。)
//
//  修法: 把这次没做完的回退做完 —— 落点表与线程数用**同一个口径**(可用逻辑核), 且只有这一处定义。
//    k / conv / 单位 / 公式 / 负载规模 / 线程数 / 阈值一个字都没动:
//    变的只是"线程钉在哪几颗核上", 工作量与算法完全不变。
// ===========================================================================
inline const std::vector<int>& multiCoreSlotList(const CpuTopology& t)
{
    // 就是"能用哪些核"这份序列本身(频率降序, 已做过权威许可集合交集)。
    // 刻意不做"每物理核只留一个代表"的投影 —— 那张表(spreadOrder)现在只留给报告与取证读数用。
    return t.order;
}

inline int loadPhaseTargetCount(const CpuTopology& t, bool multiCore)
{
    if (multiCore) {
        // 多核阶段: 每个可用核一条线程。落点表与线程数同一个来源(见 multiCoreSlotList 的取证)。
        return (int)multiCoreSlotList(t).size();
    }
    return t.fallbackTarget ? (int)t.order.size() : (int)t.fastCpus.size();
}

// 进程级"本会话位次占用"位图 + 由于位次冲突而回退的线程计数。
// 为什么要占用标记: 位次优先分配(线程 i 拿位次 i mod N)在"可用核集合被裁掉若干核"之后,
// 可能出现多个线程抢同一个物理核(N < 线程数时必然回卷; 位次相邻的核也可能同属一个物理核)。
// 被占用的线程改为最短可用位次(仍然是频率降序里的最靠前位置), 并把冲突计数报出去 ——
// 这比"两个线程挤在一个核上"更接近"线程分散到全机"的原始意图。
inline std::atomic<unsigned long long>& claimSlotMask()
{
    static std::atomic<unsigned long long> m(0);
    return m;
}

inline std::atomic<int>& claimFallbacks()
{
    static std::atomic<int> n(0);
    return n;
}

inline void claimSlot(int slot)
{
    if (slot >= 0 && slot < 64) {
        claimSlotMask().fetch_or(1ull << (unsigned)slot, std::memory_order_relaxed);
    }
}

inline void releaseSlot(int slot)
{
    if (slot >= 0 && slot < 64) {
        claimSlotMask().fetch_and(~(1ull << (unsigned)slot), std::memory_order_relaxed);
    }
}

// 最短可用位次(频率降序里最靠前的、尚未被本会话占用的位次); -1 = 全被占了/序列为空。
// 可用核集合过滤已经在 order 里做过了, 所以这里挑出来的位次对应的核一定是内核允许我们用的。
inline int firstUnclaimedSlot(const CpuTopology& t)
{
    const unsigned long long used = claimSlotMask().load(std::memory_order_relaxed);
    const int n = (int)t.order.size();
    for (int s = 0; s < n && s < 64; ++s) {
        if (((used >> (unsigned)s) & 1ull) == 0) {
            return s;
        }
    }
    return -1;
}

// 池线程 index 绑到哪个核: 位次 index 的那一个核, 一核一线程(2026-10 改)
//
//  为什么从"同频组"改成"独占一个核"
//  ---------------------------------------------------------------------------
//  旧规则给池线程分的是"位次 index mod N 所在的同频组(>= 2 个核)", 多个线程共享一组;
//  线程数上限又取的是"全机物理核数"(与"本进程真正能用的核数"无关)。真机后果(用户实测):
//    * 多核项运行时频率中位 558MHz(Ray Tracer)/1380MHz(File Compression), 标称 2270MHz
//      —— 采样口径是"可用核的核 x 时间合并样本", 中位这么低 = 绝大多数核在绝大多数时刻空闲;
//    * 界面"多核阶段明细(并行度)"只有 3.5 / 2.8 / 4.3 核, 而可用核是 9 个;
//    * note 里"池线程 2 个因位次冲突改到其它位次" —— 正是"9 个线程抢 7 个核"的形态。
//  本 App 的多核阶段要的是把所有可用核跑满(多核分数就是"整机吞吐"), 不是"簇内均衡";
//  簇内均衡那套只对单核项有意义(消除单核项的调度随机性), 用到多核阶段就把"只用少数几个核"
//  变成了常态, 与跑分目的相反。因此两个阶段的绑核策略从此分开:
//    * 单核阶段: 仍绑最快档(快簇)—— 见 auroraAffinitySessionBegin 的 multi 分支;
//    * 多核阶段: 主线程绑"全部可用核"这张整掩码, 池线程 i 独占 order[i] 这一个核。
//
//  不变式(多核阶段必须成立, 由 verify_core_spread.py 离线复核)
//  ---------------------------------------------------------------------------
//    (1) 线程 i 与线程 j (i != j) 的目标核一定不同 —— 位次即核, 没有回卷、没有共享;
//    (2) 线程数上限 = 可用核数 N(auroraThreadCap), 因此 index 恒 < N, 不会出现"多余线程";
//    (3) 一旦万一出现 index >= N(调用方绕过 gb7ParallelFor 直接开池), 不绑、不抢别人的核,
//        并返回 -5 让会话层把它计成"没能绑到独占核"上报 —— 宁可少绑一个, 也不挤掉别人。
//    (4) 绑定后立刻读回掩码: 读回 == {目标核} 才算真的独占(内核夹过就记 -2)。
//
//  前置声明: currentCpu() 定义在本节之后(它要用 /proc 兜底), 这里先用它的签名。
inline int currentCpu();

inline int bindWorkerCoreSpread(const CpuTopology& t, int index, int* landingOut, int* errOut)
{
    if (landingOut != nullptr) {
        *landingOut = -1;
    }
    if (errOut != nullptr) {
        *errOut = 0;
    }
    // 落点表 = 可用核序列本身(见 multiCoreSlotList 的取证: 线程数与落点表必须同一个口径)。
    // 线程 i 拿 multiCoreSlotList(t)[i] —— 一个人一个核, 不重不漏。
    const std::vector<int>& spread = multiCoreSlotList(t);
    const int n = (int)spread.size();
    if (n <= 0) {
        if (errOut != nullptr) {
            *errOut = -7;      // 可用序列为空(频率表与可用核集合都读不到)
        }
        return -1;
    }
    if (index < 0) {
        if (errOut != nullptr) {
            *errOut = -8;
        }
        return -1;
    }
    if (index >= n) {
        if (errOut != nullptr) {
            *errOut = -5;      // 线程数 > 可用核数: 不回卷(见不变式 3)
        }
        return -1;
    }
    const int cpu = spread[(size_t)index];
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        if (errOut != nullptr) {
            *errOut = -6;
        }
        return -1;
    }
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);
    errno = 0;
    if (sched_setaffinity(0, sizeof(one), &one) != 0) {
        if (errOut != nullptr) {
            *errOut = (errno != 0) ? errno : -1;
        }
        return -1;
    }
    // 立刻读回: 只有"读回掩码 == 这一个核"才算真独占
    cpu_set_t back;
    CPU_ZERO(&back);
    unsigned long long backMask = 0;
    errno = 0;
    if (sched_getaffinity(0, sizeof(back), &back) == 0) {
        for (int c = 0; c < 64; ++c) {
            if (CPU_ISSET(c, &back)) {
                backMask |= (1ull << (unsigned)c);
            }
        }
    }
    if (backMask != (1ull << (unsigned)cpu)) {
        if (errOut != nullptr) {
            *errOut = -2;      // 内核夹过这张掩码: 不算独占
        }
        if (landingOut != nullptr) {
            *landingOut = -2;
        }
        return -1;
    }
    if (landingOut != nullptr) {
        // 让出一次 CPU 再采样: sched_setaffinity 只是改掩码, 要等下一次被调度才真的迁过去。
        // 这一次 yield 在负载开始之前(池线程的回调在第一个任务之前调用), 不进任何计时。
        (void)sched_yield();
        const int at = currentCpu();
        *landingOut = (at >= 0) ? at : cpu;
    }
    return 1;
}

// 把"当前线程"绑到指定核心。返回 true = sched_setaffinity 成功。
// 失败(核心号非法 / 内核不允许 / cpuset 限制)只返回 false, 不报错、不降级跑别的核。
// 注意: 这是单核绑定, 只保留给 bench_cpu.cpp 的旧套件路径(行为要求零变化);
// GB7 会话路径不再产生单核掩码(理由见文件头: 单核掩码会被系统判 misfit 并推翻)。
inline bool setAffinity(int cpu)
{
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

// 保存当前线程的亲和性掩码(会话结束时还原用)
inline bool getAffinity(cpu_set_t& out)
{
    CPU_ZERO(&out);
    return sched_getaffinity(0, sizeof(out), &out) == 0;
}

inline void restoreAffinity(const cpu_set_t& set)
{
    (void)sched_setaffinity(0, sizeof(set), &set);
}

inline void applyMask(const cpu_set_t& set)
{
    (void)sched_setaffinity(0, sizeof(set), &set);
}

// /proc/self/task/<tid>/stat 的第 39 个字段 = processor(线程最后运行在哪个 CPU)。
// 只作为 sched_getcpu() 不可用时的兜底 —— 两者在这里都只是"读一个数字", 不影响负载。
inline int cpuFromProcStat()
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", (long)syscall(SYS_gettid));
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        return -1;
    }
    char buf[1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    // comm 字段可能含空格与括号, 必须从最后一个 ')' 之后开始数字段
    const char* p = strrchr(buf, ')');
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
    if (sscanf(p, "%d", &cpu) != 1) {
        return -1;
    }
    return cpu;
}

// 当前线程此刻所在的 CPU 编号; -1 = 取不到。
inline int currentCpu()
{
    const int c = sched_getcpu();
    if (c >= 0) {
        return c;
    }
    return cpuFromProcStat();
}

// 亲和性会话状态。整个进程同一时刻只会有一次 GB7 CPU 负载在跑(napi 异步逐项串行),
// 因此这里用一个进程级状态 + 原子标志就够了; 池线程只读 active / 累加计数。
struct SessionState {
    std::atomic<int> active;
    int threads;          // 实际使用的线程数(已按 SMT 开关夹上限)
    int threadsRequested; // 调用方请求的线程数
    int mainCpu;          // 名义目标核(快簇里最低的那个核号), 仅供诊断/展示
    int mainCpuAtStart;
    bool mainBound;
    bool hasSavedMask;
    cpu_set_t savedMask;
    std::atomic<int> workers;
    std::atomic<unsigned long long> workerMask;
    // ---- 可用核集合 / 实际应用掩码 / 位次冲突(2026-10-05 追加, 全部只是诊断) ----
    unsigned long long appliedMask;   // 主线程真正应用的那张掩码(= 生效快簇; 0 = 没绑)
    // ---- 掩码回读(2026-10 追加): 绑定后立刻读回的掩码(证明"内核接受了这张掩码") ----
    unsigned long long appliedReadbackMask;
    int appliedReadbackOk;            // 1 = 回读成功
    int affinityErrno;                // sched_setaffinity 失败时的 errno(0 = 成功或没调)
    int fallbackTarget;               // 1 = 可用核集合 ∩ 最快档为空, 已退化成"可用核集合里最快的核"
    int boundOk;                      // 1 = 绑定确实生效(非回退目标且 sched_setaffinity 成功)
    int workersOverflow;              // 位次冲突(线程数 > 可用位次数)导致回退的池线程个数
    // ==== 多核阶段"每颗核都跑满"的取证(2026-10 追加) ====
    //  为什么要有这一组: 真机上多核项的运行时频率中位只有 558MHz、界面并行度只有 3.5~4.3 核
    //  (可用 9 核), note 里还写着"池线程 2 个因位次冲突改到其它位次" —— 即核没被用满。
    //  旧策略给池线程分的是"同频组"(>= 2 个核), 多个线程共享一组; 线程数上限又用的是
    //  "全机物理核数"(与"本进程真正能用的核数"无关), 于是出现 9 个线程抢 7 个核。
    //  新策略: 多核阶段 = 一核一线程, 工作集合 = 权威集合 ∩ SMT 口径(全部可用核)。
    int multiCore;                    // 1 = 本次会话是多核阶段(铺满模式; 不用快簇限制)
    int spreadCores;                  // 工作集合大小 = 可用核数 N
    unsigned long long spreadMask;    // 工作集合位图(主线程被绑到这一整张; 0 = 没绑)
    int workTarget[kMaxTopoCpus];     // 池线程 i 的目标核(-1 = 未分配)
    int workLanding[kMaxTopoCpus];    // 池线程 i 绑定后立刻采到的核(-1 = 没绑上; -2 = 掩码被夹)
    int workEnd[kMaxTopoCpus];        // 池线程 i 干完活那一刻所在的核(-1 = 没采到)
    int workBound[kMaxTopoCpus];      // 1 = 该池线程确实只被允许用一个核
    std::atomic<int> workersUnbound;  // 没能绑到独占核的池线程数(必须报出)
    std::atomic<unsigned long long> workUsedMask;   // 池线程起始落点并集(每核一位)
    std::atomic<unsigned long long> workEndMask;    // 池线程结束落点并集(每核一位)
    std::atomic<int> workClampedMask;               // 绑定后回读掩码 != {目标核} 的次数(内核夹过掩码)

    SessionState()
        : active(0), threads(1), threadsRequested(1), mainCpu(-1), mainCpuAtStart(-1), mainBound(false),
          hasSavedMask(false), savedMask(), workers(0), workerMask(0),
          appliedMask(0), appliedReadbackMask(0), appliedReadbackOk(0),
          affinityErrno(0), fallbackTarget(0), boundOk(0), workersOverflow(0),
          multiCore(0), spreadCores(0), spreadMask(0), workTarget(), workLanding(), workEnd(),
          workBound(), workersUnbound(0), workUsedMask(0), workEndMask(0), workClampedMask(0)
    {
        CPU_ZERO(&savedMask);
        for (int i = 0; i < kMaxTopoCpus; ++i) {
            workTarget[i] = -1;
            workLanding[i] = -1;
            workEnd[i] = -1;
            workBound[i] = 0;
        }
    }
};

inline SessionState& sessionState()
{
    static SessionState s;
    return s;
}

// 池线程干完活之后调用一次: 记下"结束时它还在不在那个核上"(迁移是可观测的事实, 必须报)。
// 放在 sessionState() 之后: 它要用那个单例, 而单例的定义在上面。
inline void bindWorkerFinish(int index, int cpu)
{
    SessionState& s = sessionState();
    if (index < 0 || index >= kMaxTopoCpus) {
        return;
    }
    s.workEnd[index] = cpu;
    if (cpu >= 0 && cpu < 64) {
        s.workEndMask.fetch_or(1ull << (unsigned)cpu, std::memory_order_relaxed);
    }
}

// 超线程(SMT)开关状态。初值 = 1(开) —— 这是"默认行为不变"的关键:
// 任何调用方(包括老路径 / 自检 / 未来新增的调用点)在没有显式设置之前, 拿到的都是
// "用全部逻辑核", 与改动前的行为逐位一致。
// 用原子量而不是普通 bool: 开跑前 ArkTS 会设置一次, 之后负载线程只读, 池线程也会读它
// (bindWorkerGroup / auroraThreadCap), 原子读保证没有数据竞争、也没有额外开销。
inline std::atomic<int>& smtFlag()
{
    static std::atomic<int> on(1);
    return on;
}

} // namespace aurora_cpu_detail

// ---------------------------------------------------------------------------
//  公开接口(旧套件与 GB7 都从这里取)
// ---------------------------------------------------------------------------

// 每个核心的 cpuinfo_max_freq(kHz)。不缓存 —— 与 bench_cpu.cpp 原实现逐次读盘的行为一致。
inline std::vector<int> auroraReadCoreMaxFreqKhz()
{
    return aurora_cpu_detail::readCoreMaxFreqKhz();
}

// 按频率降序的核心序号(大核在前)。空 = 读不到频率信息 -> 调用方静默降级为"不绑"。
// 不缓存, 与旧套件原 coresByPerfDesc() 的每次读盘行为保持一致。
inline std::vector<int> auroraFastCoreList()
{
    return aurora_cpu_detail::coresByPerfDesc(aurora_cpu_detail::readCoreMaxFreqKhz());
}

// 缓存版(cpuinfo_max_freq 是 SoC 的静态属性, 运行期不会变): GB7 路径用,
// 避免每次绑核都要重新 open/read 32 个 sysfs 文件。
inline const std::vector<int>& auroraCoreMaxFreqKhzCached()
{
    return aurora_cpu_detail::coreMaxFreqKhzCached();
}

inline const std::vector<int>& auroraFastCoreListCached()
{
    return aurora_cpu_detail::perfOrderCached();
}

// 把调用线程绑到指定核心; 返回 true = 成功。
// (单核绑定: 保留给 bench_cpu.cpp 的旧套件路径, 行为与改动前完全一致)
inline bool auroraBindCurrentThreadToCpu(int cpu)
{
    return aurora_cpu_detail::setAffinity(cpu);
}

// 把调用线程绑到频率最高的那个核; 返回绑到的核心号, -1 = 读不到频率表或绑定失败。
// 兼容保留, GB7 路径已不再使用(单核掩码会被系统判 misfit 并推翻, 见文件头);
// 新代码请用 auroraBindCurrentThreadToFastCluster()。
inline int auroraBindToFastestCore()
{
    const std::vector<int>& order = auroraFastCoreListCached();
    if (order.empty()) {
        return -1;
    }
    const int target = order[0];
    return auroraBindCurrentThreadToCpu(target) ? target : -1;
}

// 调用线程此刻所在的 CPU 编号(-1 = 取不到)
inline int auroraCurrentCpu()
{
    return aurora_cpu_detail::currentCpu();
}

// 指定核心的 cpuinfo_max_freq(kHz); 0 = 未知(读不到频率表或该核不在表里)
inline int auroraCpuMaxFreqKhz(int cpu)
{
    const std::vector<int>& freqs = auroraCoreMaxFreqKhzCached();
    if (cpu < 0 || (size_t)cpu >= freqs.size()) {
        return 0;
    }
    return freqs[(size_t)cpu];
}

// 指定核心在"频率降序"表里的位次(0 = 最快); -1 = 未知。
// 仅作附加信息: 位次不再是"绑核是否生效"的判据(那个判据是 auroraCpuInFastCluster)。
inline int auroraCpuRank(int cpu)
{
    const std::vector<int>& order = auroraFastCoreListCached();
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == cpu) {
            return (int)i;
        }
    }
    return -1;
}

// ---- 快簇(频率最高的那批核组成的集合) -------------------------------------

// 快簇成员(核号, 频率降序)。空 = 快簇未定义(读不到频率表) -> 调用方静默降级为"不绑"。
inline const std::vector<int>& auroraFastClusterCpus()
{
    return aurora_cpu_detail::topologyCached().fastCpus;
}

// 生效快簇包含的核数; 0 = 快簇未定义
inline int auroraFastClusterCoreCount()
{
    return (int)aurora_cpu_detail::topologyCached().fastCpus.size();
}

// ---- 内核允许的核集合(公开接口; 界面与 runlog 都从这里取, 两边不许各写一套) ----

// 内核允许本进程使用的核数(0 = 读不到 —— 此时绑核不做任何过滤, 退回全机频率排名)
inline int auroraAllowedCoreCount()
{
    const aurora_cpu_detail::AuroraCpuAllowedSet& a = aurora_cpu_detail::allowedSetCached();
    return a.ok ? a.count : 0;
}

// 可用核集合的位图(0 = 读不到)
inline unsigned long long auroraAllowedCpuMask()
{
    const aurora_cpu_detail::AuroraCpuAllowedSet& a = aurora_cpu_detail::allowedSetCached();
    return a.ok ? a.mask : 0ull;
}

// 可用核集合是否读到(1 = 读到; 0 = 读不到, 此时"可用核集合"这一概念在本进程内不可知)
inline int auroraAllowedKnown()
{
    return aurora_cpu_detail::allowedSetCached().ok ? 1 : 0;
}

// 生效快簇是否已经退化成"可用核集合里最快的核"(交集为空)
inline int auroraFastClusterFallback()
{
    return aurora_cpu_detail::topologyCached().fallbackTarget ? 1 : 0;
}

// 生效快簇落在全机的第几个频率档(0 = 全机最快档)
inline int auroraFastClusterTierIndex()
{
    return aurora_cpu_detail::topologyCached().effTierIndex;
}

// 全机最快频率档的核数与频率(与可用核集合无关; "我们够不够快"这个问题看这两个值)
inline int auroraMachineTopTierCoreCount()
{
    return aurora_cpu_detail::topologyCached().machineTopTierCores;
}

inline int auroraMachineTopTierMaxKhz()
{
    return aurora_cpu_detail::topologyCached().machineTopTierKhz;
}

// 快簇的最高频率(kHz); 0 = 未知
inline int auroraFastClusterMaxKhz()
{
    return aurora_cpu_detail::topologyCached().maxKhz;
}

// 快簇的核心位图(每核一位; 只覆盖前 64 个核)
inline unsigned long long auroraFastClusterMask()
{
    return aurora_cpu_detail::topologyCached().fastMask;
}

// 全机最快频率档的核心位图(每核一位; 0 = 读不到频率表)。
// 与 auroraFastClusterMask() 是两个不同的东西: 前者看可用核集合, 后者不看。
// 「本机性能天花板」那一行要把"最快档在哪几核"印出来, 所以必须有它。
inline unsigned long long auroraMachineTopTierMask()
{
    return aurora_cpu_detail::topologyCached().machineTopTierMask;
}

// 指定核是否属于快簇 —— 结构化诊断字段 cpuInFastCluster 的取值来源。
// 快簇未定义(读不到频率表)时恒为 false: 此时"在不在快簇里"这个问题没有意义, 调用方应
// 先看 bound / fastClusterCores。
inline bool auroraCpuInFastCluster(int cpu)
{
    if (cpu < 0) {
        return false;
    }
    const std::vector<int>& fast = aurora_cpu_detail::topologyCached().fastCpus;
    for (size_t i = 0; i < fast.size(); ++i) {
        if (fast[i] == cpu) {
            return true;
        }
    }
    return false;
}

// 指定核是否属于"全机最快频率档" —— 与 auroraCpuInFastCluster 是两个不同的概念:
//   * auroraCpuInFastCluster(cpu)     : 该核在不在"生效快簇"里(可用核集合 ∩ 最快档),
//                                       回答的是"绑核有没有生效";
//   * auroraCpuInMachineTopTier(cpu)  : 该核在不在全机最快频率档里(不看可用核集合),
//                                       回答的是"我们到底站在全机的什么位置、够不够快"。
//   机器把 Prime 核排除在可用核集合之外时, 会出现 "in fast cluster = true, in machine top tier = false",
//   这正是"绑核生效了但拿不到最快核"的准确表述 —— 两个字段必须同时报, 不许混成一个。
inline bool auroraCpuInMachineTopTier(int cpu)
{
    if (cpu < 0 || cpu >= 64) {
        return false;
    }
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    if (t.machineTopTierCores <= 0) {
        return false;
    }
    return ((t.machineTopTierMask >> (unsigned)cpu) & 1ull) != 0;
}

// ---- "全机最快档"判据的自查(2026-10-06 追加; 纯只读, 不计分) ----
// 为什么要它: 上一版把"全机最快档"算成了"可用核集合里最快的核", 于是 cpuInMachineTopTier 恒为
// true, 而同一行 note 里又写着"低于全机最快档 N 档" —— 没有任何东西能拦住这种自相矛盾。
// 这里把"必须成立"的三条写成可复核的断言, 由 auroraMachineTopTierSelfCheckText() 显示出来:
//   ① 若生效快簇最高频 < 全机最快档频率, 则 生效快簇掩码 ∩ 全机最快档掩码 必须为 0;
//   ② 于是生效快簇里的每一个核, cpuInMachineTopTier 都必须为 false
//      (该字段按全机最快频率档判定, 与生效快簇无关 —— 这正是本次要分开的两个事实);
//   ③ 全机最快档掩码里的每个核, 其 cpuinfo_max_freq 必须 == machineTopTierKhz。
// 返回 0 = 自洽; 1/2/3 = 第几条不满足(直接显示在「设备画像」的那一行里)。
inline int auroraMachineTopTierSelfCheck()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    if (t.machineTopTierKhz <= 0 || t.machineTopTierCores <= 0) {
        return 0;   // 读不到频率表 -> 这条判据不参与判定(cpuInMachineTopTier 同样恒为 false)
    }
    for (int c = 0; c < 64; ++c) {
        if (((t.machineTopTierMask >> (unsigned)c) & 1ull) != 0) {
            if (auroraCpuMaxFreqKhz(c) != t.machineTopTierKhz) {
                return 3;
            }
        }
    }
    if (t.maxKhz > 0 && t.maxKhz < t.machineTopTierKhz) {
        if ((t.fastMask & t.machineTopTierMask) != 0ull) {
            return 1;
        }
        for (size_t i = 0; i < t.fastCpus.size(); ++i) {
            if (auroraCpuInMachineTopTier(t.fastCpus[i])) {
                return 2;
            }
        }
    }
    return 0;
}

// 一行自查结论(进「设备画像」的可复制文本; 两台机器上各看一次就能直接对照)。
// 例(真机 Pura X Max 的形状): "自查: 全机最快档 2750MHz(cpu9-13) vs 生效快簇 2270MHz(cpu4-8)
//   -> 生效快簇低于全机最快档 1 档, cpuInMachineTopTier 必为 false; 判据自洽(码 0)"
inline std::string auroraMachineTopTierSelfCheckText()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    char topList[64];
    char fastList[64];
    aurora_cpu_detail::maskToCpuList(t.machineTopTierMask, 64, topList, (int)sizeof(topList));
    aurora_cpu_detail::maskToCpuList(t.fastMask, 64, fastList, (int)sizeof(fastList));
    const int code = auroraMachineTopTierSelfCheck();
    char buf[384];
    if (t.machineTopTierCores <= 0 || t.machineTopTierKhz <= 0) {
        snprintf(buf, sizeof(buf),
                 "自查: 全机最快档读不到(逐核 cpuinfo_max_freq 全为 0 或读不到) -> "
                 "cpuInMachineTopTier 恒为 false(不假装知道); 判据自洽(码 %d)", code);
        return std::string(buf);
    }
    const bool belowTop = (t.maxKhz > 0 && t.maxKhz < t.machineTopTierKhz);
    snprintf(buf, sizeof(buf),
             "自查: 全机最快档 %d MHz(%s, %d 核) vs 生效快簇 %d MHz(%s, %d 核, 低 %d 档) -> "
             "生效快簇%s全机最快档, 故 cpuInMachineTopTier 必为 %s; 判据%s(码 %d)",
             (t.machineTopTierKhz + 500) / 1000, (topList[0] != 0) ? topList : "读不到",
             t.machineTopTierCores,
             (t.maxKhz + 500) / 1000, (fastList[0] != 0) ? fastList : "读不到",
             (int)t.fastCpus.size(), t.effTierIndex,
             belowTop ? "低于" : "就是", belowTop ? "false" : "true",
             (code == 0) ? "自洽" : "不自洽", code);
    return std::string(buf);
}

// 逐核 cpuinfo_max_freq + 按它划出的频率档 + 生效序列 + 快簇(回答"快簇为什么是这几个核")。
// 为什么要单独给一行(2026-10): 真机出现过"生效快簇从 5 核缩到 3 核", 而只靠"3 核簇/最高2270MHz"
// 这一句无法判断是频率表变了、可用核集合变了、还是 SMT 口径变了。把当时的逐核频率与档位
// 原样摆出来, 这个问题在设备上就是一眼可见的, 不需要事后猜。
// 数据源与绑核逻辑完全同源(coreMaxFreqKhzCached / perfOrderCached / topologyCached),
// 因此不可能出现"这一行说的"和"实际用来绑核的"不一致。
inline std::string auroraCoreTierText()
{
    const std::vector<int>& freqs = aurora_cpu_detail::coreMaxFreqKhzCached();
    const std::vector<int>& order = aurora_cpu_detail::perfOrderCached();
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    char perCore[384];
    perCore[0] = '\0';
    int used = 0;
    for (size_t i = 0; i < freqs.size() && i < (size_t)kMaxTopoCpus; ++i) {
        char one[24];
        snprintf(one, sizeof(one), "%sc%d=%d", (i == 0) ? "" : " ", (int)i, freqs[i]);
        if (used + (int)strlen(one) >= (int)sizeof(perCore) - 2) {
            break;
        }
        strncat(perCore, one, sizeof(perCore) - 1 - (size_t)used);
        used += (int)strlen(one);
    }
    if (perCore[0] == '\0') {
        snprintf(perCore, sizeof(perCore), "读不到");
    }
    // 全机频率档(降序; 同频并档, 与 buildTopo 的判档规则逐字一致): "2750000×2(cpu12,cpu13)"
    char tiers[384];
    tiers[0] = '\0';
    used = 0;
    int tierN = 0;
    for (size_t s = 0; s < order.size() && tierN < 8; ++s) {
        const int cpu = order[s];
        const int khz = (cpu >= 0 && (size_t)cpu < freqs.size()) ? freqs[(size_t)cpu] : 0;
        if (khz <= 0) {
            continue;
        }
        if (s > 0) {
            const int prevCpu = order[s - 1];
            const int prevKhz = (prevCpu >= 0 && (size_t)prevCpu < freqs.size()) ? freqs[(size_t)prevCpu] : 0;
            if (prevKhz == khz) {
                continue;   // 同一档, 只报档首
            }
        }
        unsigned long long m = 0;
        for (size_t k = s; k < order.size(); ++k) {
            const int c2 = order[k];
            const int f2 = (c2 >= 0 && (size_t)c2 < freqs.size()) ? freqs[(size_t)c2] : 0;
            if (f2 != khz) {
                break;
            }
            if (c2 >= 0 && c2 < 64) {
                m |= (1ull << (unsigned)c2);
            }
        }
        char cl[128];
        aurora_cpu_detail::maskToCoreListText(m, cl, (int)sizeof(cl));
        char one[192];
        snprintf(one, sizeof(one), "%s%dMHz(%s)", (tierN > 0) ? " / " : "", khz / 1000, cl);
        if (used + (int)strlen(one) >= (int)sizeof(tiers) - 2) {
            break;
        }
        strncat(tiers, one, sizeof(tiers) - 1 - (size_t)used);
        used += (int)strlen(one);
        ++tierN;
    }
    if (tiers[0] == '\0') {
        snprintf(tiers, sizeof(tiers), "读不到/全部为 0");
    }
    char eff[192];
    aurora_cpu_detail::maskToCoreListText(t.fastMask, eff, (int)sizeof(eff));
    char allowList[64];
    aurora_cpu_detail::maskToCpuList(t.allowedMask, 64, allowList, (int)sizeof(allowList));
    // 逐核 -> 物理核映射(每个逻辑核属于哪个物理核): 回答"关掉 SMT 后每个物理核留了谁"必须有它。
    // 例: 若 cpu4 与 cpu5 同属 P4, 那么"2270 档 5 个逻辑核"在 SMT 关之后就只剩 3 个代表核。
    char physMap[256];
    physMap[0] = '\0';
    {
        const AuroraCpuTopoInfo& ti = ::auroraSmtTopology();
        int u3 = 0;
        if (ti.known) {
            for (int c = 0; c < ti.logical && c < kMaxTopoCpus; ++c) {
                char one[24];
                snprintf(one, sizeof(one), "%sc%d:P%d", (c > 0) ? " " : "", c,
                         (c < kMaxTopoCpus) ? ti.physicalOfCpu[c] : c);
                if (u3 + (int)strlen(one) >= (int)sizeof(physMap) - 2) {
                    break;
                }
                strncat(physMap, one, sizeof(physMap) - 1 - (size_t)u3);
                u3 += (int)strlen(one);
            }
        }
    }
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "逐核 cpuinfo_max_freq(kHz): %s · 全机频率档(降序, 同频并档): %s · "
             "绑核表 %d 核(SMT %s) · 可用核集合 %s(%d 核) · 生效序列 %d 核 · 快簇(组 0) %d 核 %s "
             "(组划分: ",
             perCore, tiers, (int)t.order.size(),
             (::auroraSmtEnabledFlag() != 0) ? "开" : "关",
             (allowList[0] != '\0') ? allowList : "读不到", t.allowedCount,
             (int)t.order.size(), (int)t.fastCpus.size(), eff);
    std::string s(buf);
    for (int g = 0; g < t.groupCount && g < 8; ++g) {
        char one[64];
        snprintf(one, sizeof(one), "%s组%d=%d核", (g > 0) ? "," : "", g,
                 (g < (int)t.groupSize.size()) ? t.groupSize[(size_t)g] : 0);
        s += one;
    }
    s += ")";
    // 物理核 -> 逻辑核 的映射(SMT 关掉时"每个物理核留一个代表"的直接依据)
    if (physMap[0] != '\0') {
        s += " · 逐核所属物理核(";
        s += physMap;
        s += ")";
    } else {
        s += " · 逐核所属物理核: 拓扑未知(已按 1:1 处理)";
    }
    return s;
}

// 生效快簇是怎么算出来的(一行中文; 界面与 runlog 直接显示, 不用两边各写一套措辞)
inline std::string auroraFastClusterSourceText()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    const aurora_cpu_detail::AuroraCpuAllowedSet& a = aurora_cpu_detail::allowedSetCached();
    char buf[384];
    if (t.allowedRead) {
        char allowList[64];
        aurora_cpu_detail::maskToCpuList(t.allowedMask, 64, allowList, (int)sizeof(allowList));
        if (!t.fallbackTarget && t.fastCpus.size() > 0) {
            const int cpu0 = t.fastCpus[0];
            const int khz0 = auroraCpuMaxFreqKhz(cpu0);
            snprintf(buf, sizeof(buf),
                     "快簇 = (全机频率最快档 %d 核 / %s) ∩ (可用核集合 %s) = %d 核(最高 %d MHz) —— "
                     "排序只允许使用可用核集合内的核, 交集为空时退化为可用核集合里最快的核",
                     t.machineTopTierCores, (t.machineTopTierKhz > 0) ? "有频率读数" : "频率未知",
                     (allowList[0] != 0) ? allowList : "空", (int)t.fastCpus.size(),
                     (khz0 + 500) / 1000);
        } else {
            const int cpu0 = t.order.empty() ? -1 : t.order[0];
            snprintf(buf, sizeof(buf),
                     "快簇回退(交集为空): 可用核集合 %s 里没有任何核落在全机最快频率档上 -> "
                     "实际目标是可用核集合里最快的核 cpu%d —— 这是\u201c在不违反系统策略的前提下能拿到的最好位置\u201d, "
                     "但不是最快档, 因此 cpuBound 记 false(不假装绑定成功)",
                     (allowList[0] != 0) ? allowList : "空", cpu0);
        }
    } else {
        snprintf(buf, sizeof(buf),
                 "快簇 = 全机频率最快档 %d 核(可用核集合读不到(status errno=%d), 因此未做任何过滤 —— "
                 "注意: 可用核集合可能比这更窄, 掩码可能被内核静默夹回)",
                 t.machineTopTierCores, a.statusErrno);
    }
    return std::string(buf);
}

// "可用核集合 => 快簇 => 交集" 的一行结论(用户在两台机器上各看一次就能直接对比)。
// 例子:
//   "cpuset: allowed=0-9 (10 核) · fast=10-13(2750MHz) · 交集=空 => 绑核无效, 只能在 0-9 里跑"
//   "cpuset: allowed=0-13 (14 核) · fast=12-13(2750MHz) · 交集=12-13 => 已按可用核集合内最快的核绑核"
// 这一行的每一个数字都来自本进程自己读到的 sysfs / procfs(逐核 cpuinfo_max_freq +
// /proc/self/status + sched_getaffinity), 没有任何第三方资料参与判读。
inline std::string auroraAffinitySummaryText()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    const aurora_cpu_detail::AuroraCpuAllowedSet& a = aurora_cpu_detail::allowedSetCached();
    char allowList[64];
    char fastList[64];
    char interList[64];
    aurora_cpu_detail::maskToCpuList(a.ok ? a.mask : 0ull, 64, allowList, (int)sizeof(allowList));
    aurora_cpu_detail::maskToCpuList(t.machineTopTierMask, 64, fastList, (int)sizeof(fastList));
    aurora_cpu_detail::maskToCpuList(t.allowedRead ? (t.machineTopTierMask & t.allowedMask) : 0ull, 64,
                                     interList, (int)sizeof(interList));
    char buf[512];
    if (!a.ok) {
        snprintf(buf, sizeof(buf),
                 "cpuset: allowed=读不到(errno=%d/%d) · 全机最快档=%s(%d 核, 最高 %d MHz) · "
                 "未做过滤 => 已退回全机频率排名绑核(掩码可能被内核静默夹回)",
                 a.statusErrno, a.syscallErrno,
                 (fastList[0] != 0) ? fastList : "读不到", t.machineTopTierCores,
                 (t.machineTopTierKhz + 500) / 1000);
    } else if (interList[0] == 0) {
        snprintf(buf, sizeof(buf),
                 "cpuset: allowed=%s (%d 核) · 全机最快档=%s(%d 核, 最高 %d MHz) · 交集=空 => "
                 "绑核无效(内核不允许我们用最快档), 只能在 allowed 集合(%s)里跑 —— "
                 "实际目标已退化为该集合里最快的核 cpu%d, 因此 cpuBound 记 false",
                 allowList, a.count,
                 (fastList[0] != 0) ? fastList : "读不到", t.machineTopTierCores,
                 (t.machineTopTierKhz + 500) / 1000, allowList,
                 (!t.order.empty()) ? t.order[0] : -1);
    } else if (t.effTierIndex == 0) {
        snprintf(buf, sizeof(buf),
                 "cpuset: allowed=%s (%d 核) · 全机最快档=%s(%d 核, 最高 %d MHz) · 交集=%s => "
                 "快簇就是全机最快档里内核允许的那部分, 绑核有效",
                 allowList, a.count,
                 (fastList[0] != 0) ? fastList : "读不到", t.machineTopTierCores,
                 (t.machineTopTierKhz + 500) / 1000, interList);
    } else {
        snprintf(buf, sizeof(buf),
                 "cpuset: allowed=%s (%d 核) · 全机最快档=%s(%d 核, 最高 %d MHz) · 交集=%s 但生效快簇"
                 "低于全机最快档 %d 档 => 绑核有效, 但拿不到最快档(该档的核没有频率读数, 或不在本项序列里)",
                 allowList, a.count,
                 (fastList[0] != 0) ? fastList : "读不到", t.machineTopTierCores,
                 (t.machineTopTierKhz + 500) / 1000, interList, t.effTierIndex);
    }
    // 自查一行(见 auroraMachineTopTierSelfCheck): 把"全机最快档判据是否自洽"直接写进这一行,
    // 免得再出现"cpuInMachineTopTier=true 而同一行写着低于全机最快档 N 档"这种自相矛盾。
    std::string out(buf);
    // 2026-10: 把"绑核决策用的是哪个来源"与"三条读数逐位差在哪"一并写进这一行 ——
    //   这一次的事故就是"两条读数不一致却只报了一个 bool", 所以这里必须把来源与差集摆出来。
    out += " · ";
    out += a.authorityText;
    out += " · ";
    out += a.diffText;
    out += " · ";
    out += auroraMachineTopTierSelfCheckText();
    return out;
}

// 把调用线程绑到快簇; 返回快簇的核数(>= 2), -1 = 快簇未定义或绑定失败。
// 这是"绑核"的新语义: 掩码里有多个位, 内核可以在簇内自由均衡, 不会因为"被限制在单核上
// 装不下 utilization"而被判 misfit 强行迁走。
inline int auroraBindCurrentThreadToFastCluster()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    return aurora_cpu_detail::bindSlots(t.order, 0, (int)t.fastCpus.size());
}

// ---- 自研套件(旧 7 项)路径用的绑定入口(2026-10 修) ------------------------------
//
//  历史缺陷(为什么必须有这两个函数): 自研套件那条路径此前既不用会话, 也不按快簇绑 ——
//    * 单核阶段(threads <= 1)在 bench_cpu.cpp 的 parallelFor 里直接串行跑, 一次
//      sched_setaffinity 都没有发过, 于是单核项落在哪颗核上完全由调度器决定;
//    * 多核阶段绑的是"频率降序表里的第 t 个核号", 与 (全机最快档 ∩ 可用核集合) 无关。
//  真机后果(两台同芯片设备, 见 D:\gb7logs\mate60 与 \tab82):
//    CS1 单核复合分 331.80 vs 331.01(差 0.24%), 而自研套件单核 1691 vs 2328(差 37.7%) ——
//    同一颗芯片、同一个 App, 只有"完全没绑核"的那条路径对不上。多核阶段因为绑了核,
//    两台设备只差 0.6%。
//
//  修法: 单核阶段绑到与 CS1 单核阶段逐位相同的目标 —— 用的是同一个
//  aurora_cpu_detail::loadPhaseTargetCount(t, false)(单一来源, 不是又抄一遍规则)。
//  交集为空时它返回整条可用序列(掩码 >= 2 位): 单核掩码会被内核以 misfit 为由推翻,
//  这一点 CS1 会话早就踩过, 所以"退化到整条可用序列"是必须的, 不是可选项。
//
//  返回绑进掩码的核数(>= 1); -1 = 没有可用核或 sched_setaffinity 失败。
//  errOut 原样带出 errno(-3 = 这段位次里一个合法核都没有, 连 syscall 都没发出去)。
inline int auroraBindCurrentThreadToSingleLoadTarget(int* errOut = nullptr)
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    const int targetCount = aurora_cpu_detail::loadPhaseTargetCount(t, false);
    return aurora_cpu_detail::bindSlotsEx(t.order, 0, targetCount, errOut);
}

// 单核负载目标的位次数与核清单(纯只读, 供报告写"绑到几个核"这一句; 不绑核、不改任何状态)
inline int auroraSingleLoadTargetCount()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    return aurora_cpu_detail::loadPhaseTargetCount(t, false);
}

// 调用线程亲和性掩码的"存-改-还": 自研套件每项跑完必须还原。
//   为什么必须还原: 跑负载的那条线程是 napi 的异步工作线程, 会被后面每一项(以及 GPU /
//   NPU / 存储阶段)复用; 不还原 = 把整个异步线程永久钉在一小撮核上, 后面所有项的
//   落点与频率都跟着变 —— 那是"改被测对象", 不只是脏读。
struct AuroraAffinitySnapshot {
    cpu_set_t set;
    bool ok;
    AuroraAffinitySnapshot() : ok(false) { CPU_ZERO(&set); }
};

inline AuroraAffinitySnapshot auroraCaptureThreadAffinity()
{
    AuroraAffinitySnapshot s;
    s.ok = aurora_cpu_detail::getAffinity(s.set);
    return s;
}

// 还原: 只看"掩码是否被真的写回去"(applyMask 本身不返回状态, 所以这里再读回一次核对)。
// 读回不一致 = 还原失败, 由调用方原样报出来(不假装还原成功)。
inline bool auroraApplyThreadAffinity(const AuroraAffinitySnapshot& s)
{
    if (!s.ok) {
        return false;
    }
    aurora_cpu_detail::applyMask(s.set);
    cpu_set_t back;
    if (!aurora_cpu_detail::getAffinity(back)) {
        return false;
    }
    return memcmp(&back, &s.set, sizeof(cpu_set_t)) == 0;
}

// 一行人类可读的诊断文本(进 runlog; 结构化字段见 AuroraCpuPlacement 的各成员)
//
// 文本结构(2026-10-05 起): 「结论 · 快簇怎么算出来的 · 可用核集合 · 落点 · 两个判据 · SMT」
//   结论行区分三种互不相同的状态, 不含糊:
//     ① 已绑定到快簇(有效)                 -> cpuBound=true
//     ② 绑定目标在可用核集合之外(绑核无效)     -> cpuBound=false + 明确原因 + 实际用的是哪个核
//     ③ 未绑定(读不到频率表)               -> cpuBound=false + 读不到的原因
//   "是否属于生效快簇"(cpuInFastCluster) 与 "是否落在全机最快档"(cpuInMachineTopTier)
//     是两个独立事实, 这里分开各写一句 —— 前者问"绑核有没有生效", 后者问"我们到底够不够快"。
inline std::string auroraCpuPlacementText(const AuroraCpuPlacement& p)
{
    char buf[1024];
    const char* mode = (p.threads > 1) ? "多核" : "单核";
    // SMT 口径后缀(旁路文本, 不计分): 关掉开关时必须写明"物理核数 = 线程数",
    // 否则日后看 runlog 无法区分"14 线程"是逻辑核还是物理核。
    char smt[256];
    if (!p.topoKnown) {
        snprintf(smt, sizeof(smt),
                 " · CPU 拓扑未知(已按 1:1 处理, SMT 开关无效)");
    } else if (p.smtPossible) {
        snprintf(smt, sizeof(smt),
                 " · CPU 拓扑 %d 逻辑核 / %d 物理核(检测到 SMT, 开关当前%s); 本项 %d 线程(%s)",
                 p.logical, p.physical, p.smtEnabled ? "已启用" : "已关闭",
                 p.threads, "物理核口径: 每个可用物理核 1 条线程, SMT 兄弟不进并行池");
    } else {
        snprintf(smt, sizeof(smt),
                 " · CPU 拓扑 %d 逻辑核 / %d 物理核(无 SMT); 本项 %d 线程",
                 p.logical, p.physical, p.threads);
    }
    char tier[192];
    if (p.cpuFastClusterTierIndex == 0) {
        snprintf(tier, sizeof(tier), "生效快簇 = 全机最快的那个频率档");
    } else {
        snprintf(tier, sizeof(tier),
                 "生效快簇低于全机最快档 %d 档(可用核集合里没有更快档的核, 或更快档全在全机口径的核号之外)",
                 p.cpuFastClusterTierIndex);
    }
    if (p.cpuInFastClusterJudged && p.cpu >= 0) {
        const bool wholeMachine = (p.fastClusterCores > 0 && p.fastClusterCores >= p.cores);
        snprintf(buf, sizeof(buf),
                 "%s %d 线程 · 已绑定到快簇(%d 个核, 最高 %d MHz, 掩码 0x%llx%s) · "
                 "跑完 cpu=%d(%d MHz, 位次 %d, 本项序列 %d 核) · %s · 开跑 cpu=%d · "
                 "生效快簇判据: %s · 全机最快档判据: %s",
                 mode, p.threads, p.fastClusterCores, (p.fastClusterMaxKhz + 500) / 1000,
                 p.fastClusterMask,
                 wholeMachine ? "; 本机频率表只有一个档, 掩码覆盖全部核" : "",
                 p.cpu, (p.maxKhz + 500) / 1000, p.rank, p.cores, tier,
                 p.cpuAtStart,
                 p.cpuInFastCluster ? "在生效快簇内(绑核生效)" : "不在生效快簇内: 掩码被系统推翻",
                 p.cpuInMachineTopTier
                     ? "在全机最快档内"
                     : "不在全机最快档内(全机最快档另有其核, 见下面的可用核集合行)");
    } else if (p.bound) {
        // 掩码应用成功了, 但连"当前在哪个核"都取不到(sched_getcpu 与 /proc 兜底同时失败)——
        // 这种情况不能写成"未绑定", 否则会误导判读
        snprintf(buf, sizeof(buf),
                 "%s %d 线程 · 已绑定到快簇(%d 个核, 最高 %d MHz, 掩码 0x%llx), 但取不到当前 cpu 编号 · %s",
                 mode, p.threads, p.fastClusterCores, (p.fastClusterMaxKhz + 500) / 1000,
                 p.fastClusterMask, tier);
    } else {
        // 绑定目标不在可用核集合内(cpuBound=false, 但确实绑了可用核集合里最快的核)
        //   这里必须与"读不到频率表"严格区分: 前者是系统策略决定的, 后者是读数失败。
        snprintf(buf, sizeof(buf),
                 "%s %d 线程 · 未绑定%s · 负载线程 cpu=%d(该核最高频 %d MHz)",
                 mode, p.threads,
                 (p.cpuBoundReason[0] != 0) ? p.cpuBoundReason : "(读不到 cpuinfo_max_freq, 或最高频率解析为 0; 已静默降级)",
                 p.cpu, (p.maxKhz + 500) / 1000);
    }
    std::string s(buf);
    s += smt;
    if (p.cpuFastClusterSource[0] != 0) {
        s += " · ";
        s += p.cpuFastClusterSource;
    }
    // 掩码回读 + 由它得出的硬事实(2026-10): "内核接受掩码"与"线程只能在掩码里跑"分开报
    if (p.cpuReadbackText[0] != 0) {
        s += " · ";
        s += p.cpuReadbackText;
    }
    // 多核"跑满"取证(2026-10): 线程数 / 核清单 / 每核是否有线程 / M<N 的原因 —— 必须进 note
    if (p.spreadText[0] != 0) {
        s += " · ";
        s += p.spreadText;
    }
    if (p.cpuAllowedText[0] != 0) {
        s += " · ";
        s += p.cpuAllowedText;
    }
    if (p.workers > 0) {
        char w[192];
        snprintf(w, sizeof(w), " · 池线程 %d 个已绑定, 落点掩码 0x%llx%s",
                 p.workers, p.workerMask,
                 (p.workersOverflow > 0) ? "(有位次冲突后回退的线程)" : "");
        s += w;
    } else if (p.cpuBoundReason[0] != 0 && p.cpuInFastClusterJudged == 0 && p.cores > 0) {
        s += " · 池线程未上报落点";
    }
    return s;
}

// 采样"调用线程现在在哪个核", 并补齐频率/位次/核数/快簇/SMT 信息
inline AuroraCpuPlacement auroraCpuSampleCurrent()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    const AuroraCpuTopoInfo& ti = ::auroraSmtTopology();
    const AuroraCpuEffectiveSet& es = ::auroraEffectiveSet();
    AuroraCpuPlacement p;
    p.cores = (int)t.order.size();
    p.logical = ti.logical;
    p.physical = ti.physical;
    p.smtPossible = ti.smtPossible;
    p.smtEnabled = es.smtEnabled;
    p.topoKnown = ti.known;
    p.threadsEffective = es.count;
    // 全机口径(不看可用核集合)应有的线程数: 只在"可用核集合确实把集合裁小了"时才与
    // threadsEffective 不同 —— 界面上因此能直接看出"这台机器被内核限了几个核"。
    p.threadsBasisFull = (ti.known ? ti.logical : es.count);
    if (p.threadsBasisFull < es.count) {
        p.threadsBasisFull = es.count;
    }
    p.effMask = es.mask;
    if (ti.sourceText[0] != 0) {
        snprintf(p.topoSource, sizeof(p.topoSource), "%s", ti.sourceText);
    }
    // ---- 可用核集合 / 生效快簇 / 全机最快档(2026-10-05 追加) ----
    const aurora_cpu_detail::AuroraCpuAllowedSet& al = aurora_cpu_detail::allowedSetCached();
    p.cpuAllowedOk = al.ok;
    p.cpuAllowedCount = al.ok ? al.count : 0;
    p.cpuAllowedMask = al.ok ? al.mask : 0ull;
    p.cpuFastClusterFallback = t.fallbackTarget;
    p.cpuFastClusterTierIndex = t.effTierIndex;
    // ---- 绑核决策的数据源 + 实测探测结果(2026-10 追加; 全是旁路诊断) ----
    p.cpuAuthority = al.authority;
    p.cpuProbeAcceptedCount = al.probeAcceptedCount;
    p.cpuProbeAcceptedMask = al.probeAcceptedMask;
    snprintf(p.cpuProbeText, sizeof(p.cpuProbeText), "%s", al.probeText);
    // ---- 逐核 cpuinfo_max_freq + 频率档划分(回答"快簇为什么是这几个核") ----
    {
        const std::string tierText = auroraCoreTierText();
        snprintf(p.cpuTierText, sizeof(p.cpuTierText), "%s", tierText.c_str());
    }
    // ---- 逐核启动状态与 A/B/C/D 分类(2026-10 追加) ----
    //  回答用户的原话问题"为什么有核没有启动"。它是行为学判据: 全局 present/possible/online
    //  三个文件的原文 + 逐核 cpuN/online 原文 + 逐核 setaffinity 实测(设一位 -> 立刻读回) +
    //  逐核频率/拓扑原文 + C 类拉起探测(写 '1' 前先备份, 无论成败都按原值恢复)。
    //  只在第一次采样时真正探测(静态缓存): 探测含逐核 syscall 与 9~14 次 sysfs 打开,
    //  虽然全程在计时区间之外, 但没有必要每一项都重做一遍 —— 结果随每一项原样上报。
    //  缓存的是"读一次得到的事实", 不是"上一次的判断"; 任何一个核的状态变了都由下一项
    //  重新探测才会体现, 所以这里同时把探测时刻的核号上界一并上报, 供事后判断覆盖范围。
    {
        static const AuroraCoreStateProbe kCoreStartup = probeCoreStartupState();
        p.coreStartupRan = kCoreStartup.ran;
        p.coreStartupUpper = kCoreStartup.upper;
        p.coreStartupPresentCount = kCoreStartup.presentCount;
        p.coreStartupPossibleCount = kCoreStartup.possibleCount;
        p.coreStartupOnlineCount = kCoreStartup.onlineCount;
        p.coreClassA = kCoreStartup.countA;
        p.coreClassB = kCoreStartup.countB;
        p.coreClassC = kCoreStartup.countC;
        p.coreClassD = kCoreStartup.countD;
        p.coreClassAMask = kCoreStartup.maskA;
        p.coreClassBMask = kCoreStartup.maskB;
        p.coreClassCMask = kCoreStartup.maskC;
        p.coreClassDMask = kCoreStartup.maskD;
        p.coreOnlineWriteAttempted = 0;
        p.coreOnlineWriteOk = kCoreStartup.cWriteOk;
        p.coreOnlineWriteFail = kCoreStartup.cWriteFail;
        p.coreOnlineLastOpenErrno = kCoreStartup.cLastOpenErrno;
        p.coreOnlineLastWriteErrno = kCoreStartup.cLastWriteErrno;
        p.coreOnlineRestoreOk = 0;
        p.coreOnlineRestoreTotal = 0;
        for (int c = 0; c < kCoreStartup.upper && c < kMaxCoreStateCpus; ++c) {
            if (kCoreStartup.writeAttempted[c] != 0) {
                p.coreOnlineWriteAttempted += 1;
                p.coreOnlineRestoreTotal += 1;
                if (kCoreStartup.restoreOk[c] != 0) {
                    p.coreOnlineRestoreOk += 1;
                }
            }
        }
        snprintf(p.coreStartupText, sizeof(p.coreStartupText), "%s", kCoreStartup.text);
    }
    p.cpuMachineTopTierCores = t.machineTopTierCores;
    p.cpuMachineTopTierKhz = t.machineTopTierKhz;
    p.cpuMachineTopTierMask = t.machineTopTierMask;
    {
        const std::string src = auroraFastClusterSourceText();
        snprintf(p.cpuFastClusterSource, sizeof(p.cpuFastClusterSource), "%s", src.c_str());
        // 三个数随每一行 note 上报(2026-10-07): physical / effectiveSet.count / cap。
        //   这一行 cpuAllowedText 是 native 唯一生成、由 ArkTS 原样贴进 note 的字符串,
        //   把它挂在这里 = 每一项的 note 都带着这三个数(不需要动 ArkTS)。
        //   起因: 真机上 physical 被算成 1 却只藏在 native 内部, 没有任何一行文本能看见它,
        //   直到用户关掉 SMT、多核线程数掉成 1 才暴露。
        {
            const int capNow = ::auroraThreadCap();
            char three[192];
            snprintf(three, sizeof(three),
                     " · physical=%d · effectiveSet.count=%d · cap=%d%s",
                     p.physical, es.count, capNow,
                     es.smtEnabled ? "(SMT 开)" : "(SMT 关)");
            snprintf(p.cpuAllowedText, sizeof(p.cpuAllowedText), "%s%s", al.line, three);
        }
        // ---- 双源差集 / 实测探测 / 权威来源 / 逐核频率档(2026-10 追加) ----
        //  这四段都是"必报"内容: 前两段是本次任务要求的"原样打印 + 逐位差集",
        //  第三段是"绑核决策用的到底是哪个来源", 第四段是"快簇为什么是这几个核"
        //  (回答真机上"生效快簇从 5 核缩到 3 核"必须靠它, 不能靠事后猜)。
        //  全部走 strncat + 余量判断: 放不下就停在那里, 不越界, 也不静默改内容。
        {
            const auto appendSeg = [](char* dst, size_t cap, const char* seg) {
                if (seg == nullptr || seg[0] == '\0') {
                    return;
                }
                const size_t used = strlen(dst);
                if (used + 3 >= cap) {
                    return;
                }
                strncat(dst, " · ", cap - 1 - used);
                const size_t used2 = strlen(dst);
                const size_t room = cap - 1 - used2;
                // 这一段原来也是静默截断(2026-10): 旧写法是 strncat(dst, seg, room) —— 放不下
                // 就把这一段无声地切在那里, 连"切过"都不说(与 capText 那条兜底同一类缺陷)。
                // 容量 9216 对真机实测载荷 5778 字节有 +59% 余量, 正常路径永不触发; 一旦触发就必须
                // 显式写出来, 否则报告里会再出现一次"少了半句还没人发现"。
                static const char kSegTrunc[] = "…(本段超出容量上限, 尾部被截断)";
                const size_t tl = sizeof(kSegTrunc) - 1;
                if (strlen(seg) > room) {
                    if (room <= tl) {
                        return;   // 连标记都放不下: 前面的段已是完整的, 这里只少掉本段(不写半个标记)
                    }
                    strncat(dst, seg, room - tl);
                    strncat(dst, kSegTrunc, tl);
                    return;
                }
                strncat(dst, seg, room);
            };
            appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), al.diffText);
            appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), al.probeText);
            appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), al.authorityText);
            appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), p.cpuTierText);
            // 逐核启动状态与 A/B/C/D 分类(2026-10 追加): 必须在这一段里上报 —— 它是
            // "为什么有核没有启动"的唯一判据来源, 而 cpuAllowedText 是 native 唯一生成、
            // 由 ArkTS 原样贴进每项 note 的字符串(不动 ets 就能让每一项的 note 都带着它)。
            appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), p.coreStartupText);
        }
    }
    if (!al.ok && t.fastCpus.empty()) {
        snprintf(p.cpuBoundReason, sizeof(p.cpuBoundReason),
                 "(读不到 cpuinfo_max_freq 或最高频率解析为 0, 且可用核集合也读不到; 已静默降级)");
    } else if (!al.ok) {
        snprintf(p.cpuBoundReason, sizeof(p.cpuBoundReason),
                 "(可用核集合读不到: /proc/self/status errno=%d, sched_getaffinity errno=%d; "
                 "已按全机频率排名绑定, 但掩码可能被内核夹回)",
                 al.statusErrno, al.syscallErrno);
    } else if (t.fallbackTarget) {
        snprintf(p.cpuBoundReason, sizeof(p.cpuBoundReason),
                 "(绑核目标不在可用核集合内: 全机最快频率档 ∩ 可用核集合 = 空, 已退化为"
                 "可用核集合里最快的核; 因此不记为绑定成功)");
    }
    p.cpu = aurora_cpu_detail::currentCpu();
    p.cpuAtStart = p.cpu;
    p.maxKhz = auroraCpuMaxFreqKhz(p.cpu);
    p.rank = auroraCpuRank(p.cpu);
    p.fastClusterCores = (int)t.fastCpus.size();
    p.fastClusterMaxKhz = t.maxKhz;
    p.fastClusterMask = t.fastMask;
    p.cpuInFastClusterJudged = (t.fastCpus.empty() ? 0 : 1);
    p.cpuInFastCluster = auroraCpuInFastCluster(p.cpu);
    p.cpuAtStartInFastCluster = p.cpuInFastCluster;
    p.cpuInMachineTopTier = auroraCpuInMachineTopTier(p.cpu) ? 1 : 0;
    return p;
}

// ---------------------------------------------------------------------------
//  会话: 由负载调度层(gb7RunTest)在计时区间之外开/关
// ---------------------------------------------------------------------------

// 会话开始: 读一次频率表 + 把当前线程(负载线程)绑到快簇 + 记下原掩码。
// 单核与多核走的是同一条路(都绑快簇): 单核阶段负载随后在同一线程上串行跑; 多核阶段
// 主线程负责建池/汇总, 池线程由 gb7ParallelFor 各自绑到"自己的均衡组"(见下)。
// 读不到频率表 / 绑定失败 -> 什么都不做, 静默降级。
inline void auroraAffinitySessionBegin(int threads)
{
    // 2026-10 第一件事: 在本线程(= 真正跑负载的那个线程)上重测许可集合
    //   为什么必须在绑核决策之前: 后面的 auroraThreadCap() / topologyCached() 全部以
    //   "可用核集合"为输入; 若沿用"别的线程读到的"那一份, 整条绑核链就建在一个错误的集合上
    //   (真机硬证据: 同一项 note 里 allowed=0-8 而线程实际跑在 cpu12/cpu13)。
    //   这是一次实测探测(逐核 sched_setaffinity + 立刻读回)+ 一次从 sysfs/procfs 重读,
    //   全部发生在计时区间之外, 不进 o.ms; 集合没变时不会作废任何缓存。
    (void)aurora_cpu_detail::auroraRefreshPermitSetOnCallingThread();
    aurora_cpu_detail::SessionState& s = aurora_cpu_detail::sessionState();
    s.threadsRequested = (threads < 1) ? 1 : threads;
    // 实际线程数: 夹到"实际使用集合"的大小(SMT 关掉时就是物理核数; 可用核集合更窄时还要再窄)。
    // 诊断字段用实际值, 免得 runlog 里出现"14 线程"而实际只开了 5 个线程的假账。
    int eff = (threads < 1) ? 1 : threads;
    const int cap = ::auroraThreadCap();
    if (cap > 0 && eff > cap) {
        eff = cap;
    }
    s.threads = eff;
    s.mainCpu = -1;
    s.mainCpuAtStart = -1;
    s.mainBound = false;
    s.hasSavedMask = false;
    s.appliedMask = 0;
    s.appliedReadbackMask = 0;
    s.appliedReadbackOk = 0;
    s.affinityErrno = 0;
    s.fallbackTarget = 0;
    s.boundOk = 0;
    s.workers.store(0, std::memory_order_relaxed);
    s.workerMask.store(0, std::memory_order_relaxed);
    aurora_cpu_detail::claimSlotMask().store(0, std::memory_order_relaxed);
    aurora_cpu_detail::claimFallbacks().store(0, std::memory_order_relaxed);
    // 多核阶段"每颗核都跑满"的取证状态: 每项清零(不把上一项的落点带进这一项)
    s.multiCore = (s.threads > 1) ? 1 : 0;
    s.spreadCores = 0;
    s.spreadMask = 0ull;
    s.workersUnbound.store(0, std::memory_order_relaxed);
    s.workUsedMask.store(0, std::memory_order_relaxed);
    s.workEndMask.store(0, std::memory_order_relaxed);
    s.workClampedMask.store(0, std::memory_order_relaxed);
    for (int wi = 0; wi < kMaxTopoCpus; ++wi) {
        s.workTarget[wi] = -1;
        s.workLanding[wi] = -1;
        s.workEnd[wi] = -1;
        s.workBound[wi] = 0;
    }
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    // 目标掩码: 两个阶段从此分开(2026-10)——
    //   单核阶段: 生效快簇(t.order 前 fastCpus.size() 位)= 最快档, 保留"消除单核调度随机性"的原意;
    //   多核阶段: 全部可用核(t.order 的整条序列)= 铺满, 不再用快簇限制(把簇用到多核阶段
    //             就等于"只用少数几个核", 与跑分目的相反)。落点表由 multiCoreSlotList() 一处给出 ——
    //             2026-10-10 修复: 它必须与线程数同口径, 否则会出现"要 8 条线程、只有 6 个格子"
    //             (真机 1239 vs 1492.8 那次事故), 取证见 multiCoreSlotList 上方。
    // 两种情况用的核都已经做过"权威许可集合"过滤, 所以这些核一定是内核允许我们用的,
    // 不在这里再猜。
    // 目标位次数的计算只有一个来源(aurora_cpu_detail::loadPhaseTargetCount):
    // 自研套件(bench_cpu.cpp)的单核阶段调的也是它, 两条路径不可能再各算一遍。
    const int targetCount = aurora_cpu_detail::loadPhaseTargetCount(t, s.multiCore != 0);
    if (s.multiCore && targetCount > 0) {
        // 工作集合(= 铺满集合)的位图, 供池线程失败时把掩码还原到"允许范围内"以及上报用
        const std::vector<int>& spreadRef = aurora_cpu_detail::multiCoreSlotList(t);
        for (int i = 0; i < targetCount && i < 64 && i < (int)spreadRef.size(); ++i) {
            const int c = spreadRef[(size_t)i];
            if (c >= 0 && c < 64) {
                s.spreadMask |= (1ull << (unsigned)c);
            }
        }
        s.spreadCores = targetCount;
    }
    if (targetCount > 0) {
        cpu_set_t old;
        if (aurora_cpu_detail::getAffinity(old)) {
            s.savedMask = old;
            s.hasSavedMask = true;
        }
        int be = 0;
        const std::vector<int>& spreadBind = aurora_cpu_detail::multiCoreSlotList(t);
        const int bound = aurora_cpu_detail::bindSlotsEx(spreadBind, 0, targetCount, &be);
        s.affinityErrno = (bound > 0) ? 0 : be;
        if (bound > 0) {
            s.mainCpu = spreadBind.empty() ? -1 : spreadBind[0];
            // "绑定是否成功"与"绑到了什么"是两件事
            //   交集为空(回退到"可用核集合里最快的核")时, syscall 确实成功了, 但目标不是
            //   全机最快档 —— 按要求标注 cpuBound = false + 原因, 不假装绑定成功。
            s.mainBound = true;
            s.boundOk = t.fallbackTarget ? 0 : 1;
            // fallbackTarget 只在"可用核集合里确实还有一个能用的核"时才成立; 可用核集合为空
            // (或位图全在前 64 核之外)时按"没有可用绑定目标"报, 免得把原因写成"目标不在
            // 可用核集合内"而实际上是"可用核集合里一个核都用不了"。
            s.fallbackTarget = (t.fallbackTarget && !t.order.empty()) ? 1 : 0;
            for (int i = 0; i < targetCount && i < 64 && i < (int)spreadBind.size(); ++i) {
                const int c = spreadBind[(size_t)i];
                if (c >= 0 && c < 64) {
                    s.appliedMask |= (1ull << (unsigned)c);
                }
            }
            // 单核阶段: 主线程占住位次 0(历史上池线程不会与它抢同一个组)。
            // 多核阶段不占: 位次 0 对应的那个核要留给池线程 #0 —— 一核一线程, 谁都不能空着。
            if (s.multiCore == 0) {
                aurora_cpu_detail::claimSlot(0);
            }
            // 掩码回读(2026-10): sched_setaffinity 返回 0 只说明"内核接受了这张掩码",
            //   不说明"线程真的只能在掩码里跑"。这里立刻把掩码读回来 —— 它与"跑完那一刻的
            //   cpu"必须相容(跑完的 cpu 必须在回读掩码里); 不相容就是一条硬事实, 由
            //   auroraAffinitySessionEnd 原样写进 note, 供定案"到底是集合算错了还是内核不执行"。
            {
                cpu_set_t back;
                if (aurora_cpu_detail::getAffinity(back)) {
                    s.appliedReadbackOk = 1;
                    s.appliedReadbackMask = 0;
                    for (int c = 0; c < 64; ++c) {
                        if (CPU_ISSET(c, &back)) {
                            s.appliedReadbackMask |= (1ull << (unsigned)c);
                        }
                    }
                }
            }
            // 绑定后让出一次 CPU 再采样: sched_setaffinity 只是改了掩码, 线程要等到下一次
            // 被调度才会真的迁进去; yield 一次能让 mainCpuAtStart 有"绑定已作用于当前线程"
            // 的含义。它发生在负载 t0 之前, 不进 o.ms。
            // 注意: 这只是参考值 —— 迁移最终由内核决定, "绑定是否生效"的判据是跑完时采样的
            // cpuInFastCluster。
            (void)sched_yield();
            s.mainCpuAtStart = aurora_cpu_detail::currentCpu();
        }
    }
    // 掩码与拓扑准备好之后再放行池线程(池线程只会在本会话内被绑)
    s.active.store(1, std::memory_order_release);
}

// 会话结束: 先采样(此时仍处于绑定状态, 采到的就是负载真正跑的那个核), 再还原掩码。
// 重复调用安全(第二次直接返回一份"未绑定"的结果)。
inline AuroraCpuPlacement auroraAffinitySessionEnd()
{
    aurora_cpu_detail::SessionState& s = aurora_cpu_detail::sessionState();
    const bool wasActive = (s.active.exchange(0, std::memory_order_acq_rel) != 0);
    AuroraCpuPlacement p = auroraCpuSampleCurrent();
    p.threads = wasActive ? s.threads : 1;
    p.threadsRequested = wasActive ? s.threadsRequested : 1;
    // 本项线程数上报(2026-10-06)
    //   threads = 本次会话真正开的线程数(auroraAffinitySessionBegin 里已过 auroraThreadCap);
    //   threadsEffective 以前是"实际使用集合大小", 与本次到底开了几个线程无关 —— 真机日志里
    //   同一行 note 因此同时出现"allowed=0-8 (9 核; sched_getaffinity)"与"14 线程（逻辑核,
    //   SMT 开）"(连单核 16 项也被写成 14 线程)。现在两者都指向实跑线程数。
    p.threadsEffective = p.threads;
    // 被夹住时把"夹到几、本应几、谁夹的"写进随 note 一起显示的 cpuAllowedText:
    // 这是 native 侧唯一的措辞处(ArkTS 只是把这一行原样贴进 note), 所以不用改 ArkTS 就能
    // 让 runlog 里出现"被内核夹到 M(本应 N)"。
    if (wasActive && s.threadsRequested > 1 && p.threadsBasisFull > p.threads) {
        const int cap = ::auroraThreadCap();
        const int allowed = ::auroraAllowedCoreCount();
        if (cap > 0) {
            const bool byKernel = (allowed > 0 && allowed <= cap);
            char one[256];
            if (byKernel) {
                snprintf(one, sizeof(one),
                         " · 本项线程数实跑 %d —— 被内核夹住"
                         "(内核只允许 %d 个核; 不看可用核集合时本应 %d 个)",
                         p.threads, allowed, p.threadsBasisFull);
            } else {
                snprintf(one, sizeof(one),
                         " · 本项线程数实跑 %d —— 被 SMT 开关夹住"
                         "(物理核口径; 不看开关时本应 %d 个)",
                         p.threads, p.threadsBasisFull);
            }
            const size_t used = strlen(p.cpuAllowedText);
            const size_t room = sizeof(p.cpuAllowedText) - 1 - used;
            if (room > 16) {
                strncat(p.cpuAllowedText, one, room);
            }
        }
    }
    p.cpuAtStart = (wasActive && s.mainCpuAtStart >= 0) ? s.mainCpuAtStart : p.cpu;
    // cpuBound 的新语义(2026-10-05)
    //   p.bound    = 主线程确实应用了一张掩码(sched_setaffinity 返回成功);
    //   p.boundOk  = 那张掩码就是"生效快簇"(不是"交集为空"时的退化目标);
    //   于是"交集为空"这种情形: 掩码应用成功了(=真的绑了可用核集合里最快的核), 但 cpuBound
    //   仍然记 false 并在 cpuBoundReason 里写明原因 —— 宁可少报一个"成功", 也不让
    //   "绑到了不允许的核(等于没绑)"被算成绑核生效。两者同时上报, 谁都不会被抹掉。
    p.bound = wasActive && s.mainBound;
    if (wasActive && s.fallbackTarget) {
        p.cpuFastClusterFallback = 1;
    }
    p.cpuAppliedMask = wasActive ? s.appliedMask : 0ull;
    // ---- 掩码回读 + 由它得出的硬事实(2026-10 追加) ----
    //   判据只有一条: 跑完那一刻的 cpu 必须落在"绑定后立刻读回的掩码"里。
    //   为什么这条判据不可替代: sched_setaffinity 返回 0 只证明"内核接受了掩码";
    //   若线程随后仍跑在回读掩码之外的核上, 那就是"内核接受了掩码却不按它调度"的硬证据 ——
    //   与"我们把集合算错了"是完全不同的两件事, 必须能分开定案。
    //   真机证据(2026-10-04 15:23 单核 16 项): 运行时采样写着"本线程所在核 cpu12,cpu13",
    //   而同一行的 cpuset 读数只允许 cpu0-8 —— 本段就是为定案这条而加。
    p.cpuAppliedReadbackMask = wasActive ? s.appliedReadbackMask : 0ull;
    p.cpuAppliedReadbackOk = (wasActive && s.appliedReadbackOk) ? 1 : 0;
    p.cpuOutsideAppliedMask = 0;
    p.cpuAtStartOutsideAppliedMask = 0;
    if (wasActive && s.appliedReadbackOk && p.cpu >= 0 && p.cpu < 64) {
        p.cpuOutsideAppliedMask =
            (((s.appliedReadbackMask >> (unsigned)p.cpu) & 1ull) == 0) ? 1 : 0;
    }
    if (wasActive && s.appliedReadbackOk && p.cpuAtStart >= 0 && p.cpuAtStart < 64) {
        p.cpuAtStartOutsideAppliedMask =
            (((s.appliedReadbackMask >> (unsigned)p.cpuAtStart) & 1ull) == 0) ? 1 : 0;
    }
    {
        char rbList[192];
        char apList[192];
        aurora_cpu_detail::maskToCoreListText(p.cpuAppliedReadbackMask, rbList, (int)sizeof(rbList));
        aurora_cpu_detail::maskToCoreListText(p.cpuAppliedMask, apList, (int)sizeof(apList));
        if (p.cpuAppliedReadbackOk) {
            snprintf(p.cpuReadbackText, sizeof(p.cpuReadbackText),
                     "掩码回读: 绑定后立刻 sched_getaffinity 读回=0x%llx [%s](与请求的 0x%llx [%s] %s); "
                     "跑完 cpu=%d 在回读掩码内=%s; 开跑 cpu=%d 在回读掩码内=%s%s",
                     p.cpuAppliedReadbackMask, rbList, p.cpuAppliedMask, apList,
                     (p.cpuAppliedReadbackMask == p.cpuAppliedMask) ? "一致(内核接受了这张掩码)" :
                                                                      "不一致(内核夹过掩码)",
                     p.cpu, p.cpuOutsideAppliedMask ? "否(硬事实)" : "是",
                     p.cpuAtStart, p.cpuAtStartOutsideAppliedMask ? "否(硬事实)" : "是",
                     (p.cpuOutsideAppliedMask || p.cpuAtStartOutsideAppliedMask)
                         ? " · 内核接受了掩码, 但线程出现在掩码之外 —— 这条与\"集合算错了\"是两件事, "
                           "说明本机内核/厂商调度没有按 sched_setaffinity 的掩码执行"
                         : "");
        } else {
            snprintf(p.cpuReadbackText, sizeof(p.cpuReadbackText),
                     "掩码回读: 读不到(绑定后 sched_getaffinity 失败; 请求的掩码=0x%llx [%s])",
                     p.cpuAppliedMask, apList);
        }
    }
    // ================= 多核阶段"跑满"取证 + 自检(2026-10 追加) =================
    //  这一段回答用户的原话问题: "9 个核是不是都有人在跑"。四条证据缺一不可:
    //    ① 线程数与工作集合大小(线程数 = 可用核数 N, 由 auroraThreadCap 保证);
    //    ② 逐核落点表: 每个池线程被指派到哪个核, 绑定后立刻采到哪个核, 结束时又在哪个核;
    //    ③ 实际用到的核清单(去重): 少了哪个核一眼可见;
    //    ④ 自检: M < N 时必须写出"本项只用到 M/N 个核, 原因是 ..." —— 不许静默。
    //  (有没有核"从头到尾没人用"由运行时频率那一段的 /proc/stat 逐核占用率给出, 两处互相印证)
    {
        const aurora_cpu_detail::CpuTopology& topo = aurora_cpu_detail::topologyCached();
        const int avail = ::auroraAllowedCoreCount();
        p.multiCore = wasActive ? s.multiCore : 0;
        p.availCores = (avail > 0) ? avail : (int)topo.order.size();
        p.spreadCores = wasActive ? s.spreadCores : 0;
        p.spreadBound = wasActive ? s.workers.load(std::memory_order_relaxed) : 0;
        p.spreadUnbound = wasActive ? s.workersUnbound.load(std::memory_order_relaxed) : 0;
        p.spreadClamped = wasActive ? s.workClampedMask.load(std::memory_order_relaxed) : 0;
        p.targetMask = wasActive ? s.spreadMask : 0ull;
        unsigned long long used = 0ull;
        if (wasActive) {
            used = s.workUsedMask.load(std::memory_order_relaxed) |
                   s.workEndMask.load(std::memory_order_relaxed);
        }
        p.usedMask = used;
        {
            int m = 0;
            for (int c = 0; c < 64; ++c) {
                if (((used >> (unsigned)c) & 1ull) != 0) {
                    ++m;
                }
            }
            p.usedCores = m;
        }
        // 工作集合里"一个线程都没落到"的核数(阈值用目标集合: 那才是本项应当铺满的核)
        {
            int empty = 0;
            int spread = 0;
            for (int c = 0; c < 64; ++c) {
                if (((p.targetMask >> (unsigned)c) & 1ull) == 0) {
                    continue;
                }
                ++spread;
                if (((used >> (unsigned)c) & 1ull) == 0) {
                    ++empty;
                }
            }
            if (spread > 0) {
                p.emptyCores = empty;
            } else {
                p.emptyCores = (p.availCores > p.usedCores) ? (p.availCores - p.usedCores) : 0;
            }
        }
        // ② 逐核落点表(只在多核阶段有意义; 单核阶段不占篇幅)
        if (p.multiCore != 0) {
            char tb[768];
            tb[0] = '\0';
            int usedLen = 0;
            const int nT = (p.spreadCores < kMaxTopoCpus) ? p.spreadCores : kMaxTopoCpus;
            for (int i = 0; i < nT; ++i) {
                char one[64];
                snprintf(one, sizeof(one), "%s#%d->cpu%d", (i > 0) ? " " : "", i,
                         (wasActive && s.workTarget[i] >= 0) ? s.workTarget[i] : -1);
                if (wasActive && s.workLanding[i] >= 0) {
                    char two[24];
                    snprintf(two, sizeof(two), "@cpu%d", s.workLanding[i]);
                    strncat(one, two, sizeof(one) - 1 - strlen(one));
                } else {
                    strncat(one, "@未绑上", sizeof(one) - 1 - strlen(one));
                }
                if (wasActive && s.workEnd[i] >= 0) {
                    char two[24];
                    snprintf(two, sizeof(two), "→cpu%d", s.workEnd[i]);
                    strncat(one, two, sizeof(one) - 1 - strlen(one));
                }
                if (usedLen + (int)strlen(one) >= (int)sizeof(tb) - 2) {
                    break;
                }
                strncat(tb, one, sizeof(tb) - 1 - (size_t)usedLen);
                usedLen += (int)strlen(one);
            }
            snprintf(p.spreadTable, sizeof(p.spreadTable), "%s", (tb[0] != '\0') ? tb : "(没有池线程)");
        } else {
            snprintf(p.spreadTable, sizeof(p.spreadTable), "(单核阶段: 不铺核)");
        }
        // ④ 自检 + 原因(不许静默)
        {
            char usedList[160];
            char targetList[160];
            aurora_cpu_detail::maskToCoreListText(used, usedList, (int)sizeof(usedList));
            aurora_cpu_detail::maskToCoreListText(p.targetMask, targetList, (int)sizeof(targetList));
            std::string reason;
            if (p.multiCore == 0) {
                reason = "单核阶段(不适用)";
            } else if (p.availCores <= 0) {
                reason = "可用核集合读不到, 可用核数未知(已按不夹处理)";
            } else if (p.usedCores >= p.availCores) {
                reason = "无 —— 已铺满";
            } else {
                // 逐条查原因(每条都带数字; 按"能不能修"排序)
                char one[256];
                if (p.spreadCores < p.availCores) {
                    snprintf(one, sizeof(one),
                             "本项工作集合只有 %d 核(可用 %d): 关掉 SMT 时每个物理核只留一个逻辑核, "
                             "或可用核集合本身比核数窄; ",
                             p.spreadCores, p.availCores);
                    reason += one;
                }
                if (p.threads < p.availCores) {
                    snprintf(one, sizeof(one),
                             "本项线程数被夹到 %d(可用核数 %d; 上限=%d = 可用核数, 请求 %d); ",
                             p.threads, p.availCores, p.spreadCores, p.threadsRequested);
                    reason += one;
                }
                if (p.spreadBound < p.threads) {
                    snprintf(one, sizeof(one),
                             "实际建起来的池线程只有 %d 个(会话线程数 %d; 负载的任务数可能少于核数); ",
                             p.spreadBound, p.threads);
                    reason += one;
                }
                if (p.spreadUnbound > 0) {
                    snprintf(one, sizeof(one),
                             "有 %d 个池线程没能绑到独占核(内核夹过掩码 %d 次; 见上面的逐核落点表); ",
                             p.spreadUnbound, p.spreadClamped);
                    reason += one;
                }
                if (p.emptyCores > 0) {
                    snprintf(one, sizeof(one),
                             "工作集合里有 %d 个核没有任何线程落到(目标 %s; 实际用到 %s); ",
                             p.emptyCores,
                             (targetList[0] != '\0') ? targetList : "?",
                             (usedList[0] != '\0') ? usedList : "无");
                    reason += one;
                }
                // "为什么有核没有启动"进原因链(2026-10 追加)
                //  用户的原话问题必须在这一行就有答案, 而不是要人去翻另一段日志:
                //  M<N 的原因链里, "有几个核根本没上线(C 类) / 有几个核在线却被许可集合拒绝(B 类)"
                //  是最上游的两条原因 —— 工作集合窄、线程数被夹, 全都是它们的结果。
                if (p.coreClassC > 0 || p.coreClassB > 0 || p.coreClassD > 0) {
                    snprintf(one, sizeof(one),
                             "逐核启动状态: C(离线, 没启动) %d 个 / B(在线但被许可集合拒绝) %d 个 / "
                             "D(present 不含) %d 个 / A(可用) %d 个 —— 这四类里只有 A 能承载线程, "
                             "所以『线程数 = 可用核数』里的那个可用核数本身就等于 A 的个数; ",
                             p.coreClassC, p.coreClassB, p.coreClassD, p.coreClassA);
                    reason += one;
                } else if (p.coreStartupRan != 0) {
                    snprintf(one, sizeof(one),
                             "逐核启动状态: A(可用) %d 个, B/C/D 全为 0 —— 可用核数少于标称核数的"
                             "原因不是核没启动(没有核处于 offline, 也没有核被许可集合拒绝); ",
                             p.coreClassA);
                    reason += one;
                }
                if (reason.empty()) {
                    reason = "未定位(见逐核落点表与逐核占用率)";
                }
            }
            snprintf(p.spreadVerdict, sizeof(p.spreadVerdict),
                     "本项用到 %d/%d 个核(工作集合 %d 核, 线程 %d, 成功绑到独占核 %d 个)%s%s",
                     p.usedCores, p.availCores, p.spreadCores, p.threads, p.spreadBound,
                     (p.usedCores >= p.availCores && p.multiCore != 0) ? " —— " : ", 原因: ",
                     reason.c_str());
        }
        // 完整一行(线程数 / 核清单 / 每核是否有线程 / 自检结论): 这就是"能自证跑满了"的那一行
        {
            char usedList2[160];
            char targetList2[160];
            char perCoreThread[512];
            aurora_cpu_detail::maskToCoreListText(used, usedList2, (int)sizeof(usedList2));
            aurora_cpu_detail::maskToCoreListText(p.targetMask, targetList2, (int)sizeof(targetList2));
            perCoreThread[0] = '\0';
            {
                int u4 = 0;
                for (int i = 0; i < p.spreadCores && i < kMaxTopoCpus; ++i) {
                    char one[40];
                    snprintf(one, sizeof(one), "%scpu%d=%s", (i > 0) ? " " : "",
                             (wasActive && s.workTarget[i] >= 0) ? s.workTarget[i] : -1,
                             (wasActive && s.workBound[i] != 0) ? "有" : "无");
                    if (u4 + (int)strlen(one) >= (int)sizeof(perCoreThread) - 2) {
                        break;
                    }
                    strncat(perCoreThread, one, sizeof(perCoreThread) - 1 - (size_t)u4);
                    u4 += (int)strlen(one);
                }
            }
            if (p.multiCore == 0) {
                snprintf(p.spreadText, sizeof(p.spreadText),
                         "多核铺满取证: 本项是单核阶段(不铺核) · 线程 %d / 请求 %d",
                         p.threads, p.threadsRequested);
            } else {
                snprintf(p.spreadText, sizeof(p.spreadText),
                         "多核铺满取证: 可用核数 N=%d · 工作集合 %s(%d 核, 全部可用核; 已放弃快簇限制) · "
                         "本项线程 %d(请求 %d) · 成功绑到独占核的池线程 %d 个 · 未绑上 %d 个 · "
                         "内核夹回掩码 %d 次 · 实际用到的核 M=%d 个: %s · 每核是否有线程: %s · "
                         "逐线程落点(目标@绑定后→结束): %s · %s",
                         p.availCores,
                         (targetList2[0] != '\0') ? targetList2 : "读不到", p.spreadCores,
                         p.threads, p.threadsRequested, p.spreadBound, p.spreadUnbound,
                         p.spreadClamped, p.usedCores,
                         (usedList2[0] != '\0') ? usedList2 : "无",
                         (perCoreThread[0] != '\0') ? perCoreThread : "(无)",
                         (p.spreadTable[0] != '\0') ? p.spreadTable : "(无)",
                         p.spreadVerdict);
            }
        }
        // ---- 多核阶段: 上报的"工作集合"就是全部可用核(不再用快簇限制) ----
        //  为什么必须覆盖: 界面/note 的"快簇已生效(cpu=…, N 核簇…)"与"注意: 跑完落在核外"两个
        //  分支都以 cpuInFastCluster 为判据; 多核阶段如果还拿"最快档"当判据, 一个正常的落点
        //  会被误报成"落在核外"。这里把判据换成本阶段真正的工作集合(全部可用核), 语义是
        //  "这个线程是否在本项应当使用的核集合里", 而 native 的铺满取证一行会写清它是怎么来的。
        if (p.multiCore != 0 && p.targetMask != 0ull) {
            p.fastClusterCores = p.spreadCores;
            p.fastClusterMask = p.targetMask;
            {
                int mx = 0;
                for (int c = 0; c < 64; ++c) {
                    if (((p.targetMask >> (unsigned)c) & 1ull) == 0) {
                        continue;
                    }
                    const int khz = auroraCpuMaxFreqKhz(c);
                    if (khz > mx) {
                        mx = khz;
                    }
                }
                p.fastClusterMaxKhz = mx;
            }
            p.cpuInFastCluster = (p.cpu >= 0 && p.cpu < 64 &&
                                  ((p.targetMask >> (unsigned)p.cpu) & 1ull) != 0) ? 1 : 0;
            p.cpuAtStartInFastCluster = (p.cpuAtStart >= 0 && p.cpuAtStart < 64 &&
                                         ((p.targetMask >> (unsigned)p.cpuAtStart) & 1ull) != 0) ? 1 : 0;
            p.cpuInFastClusterJudged = 1;
            snprintf(p.cpuFastClusterSource, sizeof(p.cpuFastClusterSource),
                     "多核阶段: 不使用快簇限制 —— 工作集合 = 全部可用核 %d 个(权威许可集合 ∩ SMT 口径), "
                     "池线程 i 独占其中第 i 个核(一核一线程); 单核阶段仍按最快档绑核",
                     p.spreadCores);
        }
    }
    // 开跑时的采样是否在生效快簇内: 这里显式重算一次而不是沿用 auroraCpuSampleCurrent
    // 里的那份 —— 那份算的是"跑完那一刻的 cpu", 两者是不同的时间点, 不能互相赋值。
    p.cpuAtStartInFastCluster = auroraCpuInFastCluster(p.cpuAtStart);
    // 两个判据分开赋值(不互相赋值): 生效快簇判据看 cpuInFastCluster, 全机最快档判据看
    // cpuInMachineTopTier —— 开机采样时它们恰好同值只是巧合, 在这里被显式分开。
    p.cpuInFastClusterJudged = aurora_cpu_detail::topologyCached().fastCpus.empty() ? 0 : 1;
    p.cpuInMachineTopTier = auroraCpuInMachineTopTier(p.cpu) ? 1 : 0;
    p.cpuFastClusterTierIndex = aurora_cpu_detail::topologyCached().effTierIndex;
    p.workers = wasActive ? s.workers.load(std::memory_order_relaxed) : 0;
    p.workersOverflow = wasActive ? aurora_cpu_detail::claimFallbacks().load(std::memory_order_relaxed) : 0;
    p.workerMask = wasActive ? s.workerMask.load(std::memory_order_relaxed) : 0;
    if (wasActive && !p.bound && p.cpuBoundReason[0] == 0) {
        // 没有原因文本时补一条, 免得界面出现"未绑定"却不知道为什么
        if (s.affinityErrno != 0) {
            snprintf(p.cpuBoundReason, sizeof(p.cpuBoundReason),
                     "(sched_setaffinity 失败 errno=%d%s)", s.affinityErrno,
                     (s.affinityErrno == 22) ? " EINVAL: 掩码与可用核集合无交集" :
                     ((s.affinityErrno == 1) ? " EPERM: 当前进程无权改亲和性" : ""));
        } else if (s.fallbackTarget) {
            snprintf(p.cpuBoundReason, sizeof(p.cpuBoundReason),
                     "(绑核目标不在可用核集合内: 已退化为可用核集合里最快的核; 不记为绑定成功)");
        } else if (!s.mainBound) {
            snprintf(p.cpuBoundReason, sizeof(p.cpuBoundReason),
                     "(没有可用的绑定目标: 生效快簇为空或读不到频率表; 已静默降级)");
        }
    }
    if (wasActive && s.hasSavedMask) {
        aurora_cpu_detail::restoreAffinity(s.savedMask);
    }
    s.hasSavedMask = false;
    s.mainBound = false;
    s.mainCpu = -1;
    s.mainCpuAtStart = -1;
    s.appliedMask = 0;
    s.affinityErrno = 0;
    s.fallbackTarget = 0;
    s.boundOk = 0;
    s.threads = 1;
    aurora_cpu_detail::claimSlotMask().store(0, std::memory_order_relaxed);
    p.text = auroraCpuPlacementText(p);
    return p;
}

// 池线程入口钩子(由 gb7ParallelFor 在每个工作线程开始干活之前调用一次)。
// index = 线程在本次并行池里的序号。绑定目标 = order[index] 那一个核 —— 一核一线程
// (2026-10 改; 旧规则是"位次 index 所在的同频组", 多线程共享一组, 真机上就变成了核没被用满:
//  多核项运行时频率中位 558MHz、界面并行度只有 3.5~4.3 核, 而可用核是 9 个)。
//
//  为什么用单核掩码而不是"一组"或"整张掩码"
//  ---------------------------------------------------------------------------
//  多核阶段的目的是"每颗核都跑满": 线程数 = 可用核数 N, 线程 i 独占 order[i]。
//  这样做的代价是可能撞上"单核掩码被系统判 misfit 推翻"(那条经验来自单核阶段: 一个线程
//  独占一个核, 别的核空着, 内核自然要把它挪到更闲的核上)。多核阶段不存在这个前提 ——
//  所有核都有人, 内核没有更闲的地方可挪; 万一真被挪走, 会话层会用"/proc/stat 逐核占用率 +
//  落点表 + 同一核上是否出现两个线程"把它报出来(见 auroraAffinitySessionEnd)。
//  绑定成功与否的判据也是硬的: sched_setaffinity 返回 0 且立刻读回的掩码恰好是那一个核。
//
//  index 越界(>= N)时不回卷: 宁可少绑一个线程(并报"本项只用到 M/N 个核"), 也不让
//  两个线程挤在同一个核上 —— 那正是这次要修的现象。
// 没有会话在跑时立即返回 —— 这就是"默认行为不变": GPU7 / 旧套件 / 任何没开会话的
// 调用方都不会发生任何绑核动作。
inline void auroraAffinityWorkerStart(int index)
{
    aurora_cpu_detail::SessionState& s = aurora_cpu_detail::sessionState();
    if (s.active.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (index >= 0 && index < kMaxTopoCpus) {
        s.workTarget[index] = -1;
        s.workLanding[index] = -1;
        s.workEnd[index] = -1;
        s.workBound[index] = 0;
    }
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    // 目标核 = 多核落点表的第 index 位(= 可用核序列, 见 multiCoreSlotList)。
    {
        const std::vector<int>& spreadRef = aurora_cpu_detail::multiCoreSlotList(t);
        if (index >= 0 && index < kMaxTopoCpus && index < (int)spreadRef.size()) {
            s.workTarget[index] = spreadRef[(size_t)index];
        }
    }
    int landing = -1;
    int be = 0;
    const int bound = aurora_cpu_detail::bindWorkerCoreSpread(t, index, &landing, &be);
    if (bound <= 0) {
        // 绑定失败(线程数超过可用核数 / 内核夹过掩码 / syscall 失败): 这个线程继承的是主线程
        // 那张"全部可用核"的掩码。不把它还原成会话之前那张掩码(那可能比可用核集合更宽,
        // 会把线程放到内核不允许的核上); 只把它保持在"工作集合"范围内, 并计数。
        if (s.multiCore != 0 && s.spreadMask != 0ull) {
            cpu_set_t set;
            CPU_ZERO(&set);
            for (int c = 0; c < 64; ++c) {
                if (((s.spreadMask >> (unsigned)c) & 1ull) != 0) {
                    CPU_SET(c, &set);
                }
            }
            aurora_cpu_detail::applyMask(set);
        } else if (s.hasSavedMask) {
            aurora_cpu_detail::applyMask(s.savedMask);
        }
        s.workersUnbound.fetch_add(1, std::memory_order_relaxed);
        if (be == -2) {
            s.workClampedMask.fetch_add(1, std::memory_order_relaxed);
        }
        if (index >= 0 && index < kMaxTopoCpus) {
            s.workLanding[index] = -1;
            s.workBound[index] = 0;
        }
        return;
    }
    // 绑定成功: 记下目标核 + 绑定后立刻采到的核(readback 已证明掩码就是那一个核)
    s.workers.fetch_add(1, std::memory_order_relaxed);
    if (index >= 0 && index < kMaxTopoCpus) {
        s.workBound[index] = 1;
        s.workLanding[index] = landing;
    }
    if (landing >= 0 && landing < 64) {
        s.workerMask.fetch_or(1ull << (unsigned)landing, std::memory_order_relaxed);
        s.workUsedMask.fetch_or(1ull << (unsigned)landing, std::memory_order_relaxed);
    }
}

// 池线程干完活之后调用一次(由 gb7ParallelFor 在任务循环结束后调用)。
// 目的: "每核是否有线程"不能只靠"开始时采到的落点"下结论 —— 线程可能在干活期间被内核挪走。
// 这里记下结束落点, 会话层把它与起始落点一起报出来(并集 = 实际用到的核)。
inline void auroraAffinityWorkerEnd(int index)
{
    aurora_cpu_detail::SessionState& s = aurora_cpu_detail::sessionState();
    if (s.active.load(std::memory_order_acquire) == 0) {
        return;
    }
    aurora_cpu_detail::bindWorkerFinish(index, aurora_cpu_detail::currentCpu());
}

// ---------------------------------------------------------------------------
//  超线程(SMT): 公开接口(ArkTS 侧只经过 napi 的 setSmtEnabled / smtEnabled /
//  smtCapabilities / smtThreadsFor，签名见 types/libaurorabench/index.d.ts)
// ---------------------------------------------------------------------------

// 开关状态: 1 = 开(默认), 0 = 关
inline bool auroraSmtEnabledFlag()
{
    return aurora_cpu_detail::smtFlag().load(std::memory_order_relaxed) != 0;
}

// 打开/关闭超线程。返回写入后的开关状态(1/0)。
// 语义: 只决定多核阶段使用哪些逻辑核(线程数 / 快簇掩码 / 池线程落点),
// 不改变任何负载的算法、工作量、metric 口径与计分公式; 单核阶段完全不受影响。
// 默认值必须是开 —— smtFlag() 的初值就是 1, 调用方不设置任何东西时行为与历史完全一致。
inline int auroraSetSmtEnabled(int on)
{
    aurora_cpu_detail::smtFlag().store(on ? 1 : 0, std::memory_order_relaxed);
    // 开关一变, 派生出来的"实际使用集合"必须立刻重算(下一次负载就会用到)
    auroraRebuildEffectiveSet();
    return auroraSmtEnabledFlag() ? 1 : 0;
}

// 开关的查询接口(供 ArkTS 显示)
inline int auroraSmtEnabled()
{
    return auroraSmtEnabledFlag() ? 1 : 0;
}

// 读到的 CPU 拓扑(进程内只读一次 sysfs)
inline const AuroraCpuTopoInfo& auroraSmtTopology()
{
    return ::aurora_smt_detail::topologyInfoCached();
}

// 实际使用集合的顺序(核号, 频率降序; 开关关掉时每个物理核只留一个逻辑核)。
// 与 auroraFastCoreList() 的关系: 开关开时两者逐位相同(默认行为不变);
// 开关关掉时这里少了被排除的兄弟核, 于是"线程 i 钉第 i 个核"的老规则自动变成
// "每个物理核最多一个线程"。空 = 读不到任何核。
inline const std::vector<int>& auroraEffectiveCoreList()
{
    return aurora_cpu_detail::effectiveOrderCached();
}

// "每物理核一个代表"的**投影**表(核号, 频率降序), 2026-10-07/08 曾用它当多核落点表。
//  ★ 2026-10-10 起它**不再用于多核落点**(那次实验在真机上被证伪并已回退, 取证见
//    multiCoreSlotList 上方: 8 线程 1481.6 vs 6 线程 1069.8, 慢 34%; 而且它正是
//    "线程数与落点表口径不一致 -> CS1 多核 1239 vs 1492.8" 那次事故的那半张表)。
//    现在它只作为**读数/取证**保留: 报告里的 spreadPhysical 之类的量还从这里取,
//    多核落点一律走 multiCoreSlotList()。
inline const std::vector<int>& auroraPhysicalCoreList()
{
    const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();
    return t.spreadOrder.empty() ? t.order : t.spreadOrder;
}

// 多核阶段实际会开几个线程(= 可用物理核数; 与 auroraThreadCap 同源, 只是取正数形式)
inline int auroraPhysicalCoreCount()
{
    const int n = (int)auroraPhysicalCoreList().size();
    return n > 0 ? n : 0;
}

// 实际使用集合(受开关影响)
inline const AuroraCpuEffectiveSet& auroraEffectiveSet()
{
    return ::aurora_smt_detail::effectiveSetCached(auroraSmtTopology(),
        aurora_cpu_detail::perfOrderCached());
}

// 重算实际使用集合(开关变化时由 setter 调用; 也可以由调用方在换设备/换偏好后手动调)
inline void auroraRebuildEffectiveSet()
{
    (void)auroraEffectiveSet();
    (void)aurora_cpu_detail::topologyCached();
}

// 多核阶段应当使用的线程数:
//   开关开(默认) -> 全部逻辑核(= 历史行为)
//   开关关       -> 物理核数(每个物理核只留一个逻辑核)
//   拓扑未知     -> 全部逻辑核(1:1, 开关无效) —— 界面必须注明
inline int auroraSmtThreads()
{
    const AuroraCpuEffectiveSet& s = auroraEffectiveSet();
    if (s.count > 0) {
        return s.count;
    }
    const int n = auroraSmtTopology().logical;
    return n > 0 ? n : 1;
}

// 多核阶段实际会用的线程数(= auroraSmtThreads() 再过后面的 auroraThreadCap 夹子)。
//   与 auroraSmtThreads() 的唯一区别: 可用核集合比"全机口径应有的核数"更窄时, 后者返回那个
//   更大的数(真机: 14), 前者返回真正会开的线程数(真机: 9)。界面/日志里凡是要说"多核阶段用几个
//   线程", 都必须用这个; 要说"本应几个"(不看可用核集合)才用 auroraSmtThreads()。
//   两个数都不含任何设备相关分支: 全部由本进程读到的可用核集合(sched_getaffinity / procfs)决定。
inline int auroraSmtThreadsCapped()
{
    const int full = auroraSmtThreads();
    const int cap = ::auroraThreadCap();
    if (cap > 0 && full > cap) {
        return cap;
    }
    return full;
}

// 某个开关状态下多核阶段实际会开的线程数(只读计算: 不改全局开关, 不碰任何缓存)。
//   只用于"把开关的后果先说清楚"(见 auroraSmtSwitchHint) —— 开 / 关两种口径各算一遍。
//   口径与 auroraSmtThreadsCapped() / auroraSmtThreadsFor() 严格一致, 用同一个夹子:
//     开 -> 全部逻辑核; 关 -> 每个物理核一个逻辑核(拓扑未知时开关无效, 与开相同);
//     两者都用可用核集合的核数夹一次(auroraThreadCap 的那条规则, 不另立一套)。
inline int auroraSmtThreadsForMode(int smtOn)
{
    const int want = smtOn ? 1 : 0;
    if (want == (::auroraSmtEnabledFlag() ? 1 : 0)) {
        return auroraSmtThreadsCapped();   // 当前状态: 用界面/日志同款的那个数, 不另算一份
    }
    const AuroraCpuTopoInfo& info = auroraSmtTopology();
    const AuroraCpuEffectiveSet s = ::aurora_smt_detail::buildEffectiveSet(
        info, aurora_cpu_detail::perfOrderCached(), want);
    int n = (info.known && s.count > 0) ? s.count : info.logical;
    if (n <= 0) {
        n = 1;
    }
    const int cap = auroraThreadCap();
    if (cap > 0 && n > cap) {
        n = cap;
    }
    return n;
}

// 超线程(SMT)开关的真实约束说明(纯文本, 不计分)。
//   要求(用户 2026-10-07): 关掉开关会发生什么、会少几个线程, 必须在打开界面时就能看到,
//   不许靠用户自己撞。文案只在 native 这一处生成, ArkTS 只负责把这一行显示出来 ——
//   这样"两边各说各话"不可能发生。
//   全部数字都由本进程读到的拓扑 / 可用核集合算出, 没有任何按机型 / SoC 的分支。
inline std::string auroraSmtSwitchHint()
{
    const AuroraCpuTopoInfo& t = auroraSmtTopology();
    const AuroraCpuEffectiveSet& s = auroraEffectiveSet();
    const int cap = auroraThreadCap();
    const int on = auroraSmtThreadsForMode(1);
    const int off = auroraSmtThreadsForMode(0);
    const int allowed = auroraAllowedCoreCount();
    char buf[768];
    if (!t.known) {
        snprintf(buf, sizeof(buf),
                 "本机超线程(SMT)开关无效：CPU 拓扑读不到(逻辑核数与物理核数按 1:1 处理), "
                 "开与关都是 %d 线程。【physical=%d · effectiveSet.count=%d · cap=%d】",
                 on, t.physical, s.count, cap);
        return std::string(buf);
    }
    if (!t.smtPossible) {
        snprintf(buf, sizeof(buf),
                 "本机未检测到超线程(逻辑核 %d = 物理核 %d)：开关对线程数没有影响, "
                 "开与关都是 %d 线程。【physical=%d · effectiveSet.count=%d · cap=%d】",
                 t.logical, t.physical, on, t.physical, s.count, cap);
        return std::string(buf);
    }
    // 真实差异: 全部由本机读到的数算出(逻辑核 / 物理核 / 可用核集合)
    //
    // 2026-10-08 口径变更后的措辞(必须与实现一致, 不许再写"开启 = 逻辑核 8 线程"):
    //   并行池的口径从此是"每个可用**物理核**一条线程", 与开关状态无关 —— 因为真机实测
    //   8 条线程(逻辑核口径)里那 2 条落在 SMT 兄弟上的线程拿不到算力: 实测并行度天花板
    //   只有 ~6.1(= 可用物理核数), 且多核阶段没有任何一颗核的占用能到 90%。
    //   所以这一行现在报的是同一个数(开/关都是可用物理核数), 并把"为什么"写清楚。
    const int delta = on - off;
    char d[64];
    if (delta > 0) {
        snprintf(d, sizeof(d), "(%d → %d, 多 %d 个)", on, off, delta);
    } else {
        snprintf(d, sizeof(d), "(开与关都是 %d 个, 线程数不变)", on);
    }
    snprintf(buf, sizeof(buf),
             "本机 %d 逻辑核 / %d 物理核, 内核允许 %d 个核; 多核阶段的并行池按**物理核**铺 —— "
             "每个可用物理核只留频率档最高的那 1 条线程：开启 %d 线程 → 关闭 %d 线程%s。"
             "2026-10-08 口径变更(有真机证据)：以前按逻辑核铺, 8 条线程里有 2 条落在 SMT 兄弟上, "
             "一个物理核上两条线程各拿 ~50%% —— 实测并行度天花板只有 ~6.1(等于可用物理核数), "
             "多核阶段也没有任何一颗核的占用能到 90%%。现在无论开关状态都用 %d 线程、一物理核一线程；"
             "计分公式 / 工作量 / metric 口径 / 单位一个字都不改, 单核阶段完全不受影响。"
             "【physical=%d · effectiveSet.count=%d · cap=%d】",
             t.logical, t.physical, allowed > 0 ? allowed : t.logical,
             on, off, d, cap, t.physical, s.count, cap);
    return std::string(buf);
}

// 把"用户请求的线程数"过一遍 SMT 口径(ArkTS 侧取线程数用这个):
//   开关开(默认) -> 原样返回 requested(与历史行为一致, 一个数都不改);
//   开关关       -> 返回实际使用集合的大小(= 物理核数);
//   拓扑未知     -> 原样返回 requested(1:1, 开关无效)。
// 单核阶段(requested <= 1)永远原样返回, 不受开关影响。
inline int auroraSmtThreadsFor(int requested)
{
    const int want = (requested < 1) ? 1 : requested;
    if (want <= 1) {
        return 1;
    }
    const AuroraCpuEffectiveSet& s = auroraEffectiveSet();
    int out = want;
    if (s.smtEnabled == 0 && s.topologyKnown && s.count > 0) {
        out = s.count;
    }
    // 可用核集合更窄时再夹一次(与 auroraThreadCap 同一口径, 只有一处定义 auroraThreadCap)
    const int cap = auroraThreadCap();
    if (cap > 0 && out > cap) {
        out = cap;
    }
    return out;
}

// 一行中文拓扑说明(ArkTS 的「设备画像」与 runlog 直接用这个字符串, 免得两边各写一套措辞)
inline std::string auroraSmtTopologyText()
{
    const AuroraCpuTopoInfo& t = auroraSmtTopology();
    const AuroraCpuEffectiveSet& s = auroraEffectiveSet();
    // 三个数必须同时出现在这一行里(2026-10-07)
    //   physical(物理核数) / effectiveSet.count(实际使用集合大小) / cap(线程上限)。
    //   物理核数以前只藏在 native 内部: 真机上它被算成 1(14 个逻辑核全归成同一个物理核)
    //   也没有任何人看得见, 一直等到用户把 SMT 开关关掉、多核线程数掉成 1 才发现。
    //   三个数写在同一个括号里, 以后谁再算错一眼就能看出来。
    const int capNow = auroraThreadCap();
    char capTxt[32];
    if (capNow > 0) {
        snprintf(capTxt, sizeof(capTxt), "%d", capNow);
    } else {
        snprintf(capTxt, sizeof(capTxt), "不限制");
    }
    char three[192];
    snprintf(three, sizeof(three),
             "【physical=%d · effectiveSet.count=%d · cap=%s】",
             t.physical, s.count, capTxt);
    if (!t.known) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "拓扑未知（已按 1:1 处理, SMT 开关无效）：逻辑核 %d / 物理核 %d（相等), "
                 "多核阶段仍用 %d 线程 %s", t.logical, t.physical, auroraSmtThreadsCapped(), three);
        return std::string(buf);
    }
    char buf[512];
    if (t.smtPossible) {
        // 2026-10-06 措辞回正(与 auroraThreadCap() 的实现对齐):
        //   9.0 曾经按**物理核**口径铺并行池(每物理核一条线程, 真机 cap=6), 那版文案留在了这里。
        //   真机 A/B 证明物理核口径慢 34%(8 线程 gb8Multi 1481.6/1488.4/1540.2, parallelism 8.00;
        //   6 线程 1069.8, parallelism 6.00), 9.1 已回退成**可用逻辑核数**(真机 8) —— 也就是说
        //   现在 8 条线程里有 2 条正落在 SMT 兄弟上, 而且那正是更快的跑法。
        //   本工程的老规矩: 界面/报告里说的话必须与 native 算的数同源, 不许两边各说各话。
        snprintf(buf, sizeof(buf),
                 "%d 逻辑核 / %d 物理核（检测到 SMT, 开关当前%s）→ "
                 "多核阶段 %d 线程（逻辑核口径: 用满内核允许的逻辑核; "
                 "物理核口径 9.0 试过、实测慢 34%%, 已于 9.1 回退, 见 auroraThreadCap()）%s；来源 %s",
                 t.logical, t.physical, s.smtEnabled ? "已启用" : "已关闭",
                 auroraSmtThreadsCapped(),
                 three,
                 t.sourceText);
    } else {
        // 措辞口径(2026-10-05 修正, 不许写回"无 SMT")
        // 这一支只说明"没测到超线程", 不说明"硬件没有超线程" —— 两者是不同的事实:
        //   真机已证实存在"内核能读到 thread_siblings_list、但每个核的兄弟表只含它自己(自指)"
        //   的平台(用户那台 MatePad Pro 13.2 / PCE-W30 就是这种: 读到 12/12 个核、全是自指,
        //   而该机按第三方资料是麒麟 9000S 8 物理核 / 12 线程, SMT 开在 4 个泰山核上)。
        //   在那种机器上, "物理核 12" 是固件没上报的产物, 不是硬件事实。
        // 本 App 的既定原则是"宁可说不知道, 不许假装"。所以这里只陈述观测结果与它的边界,
        // 并把 siblings 是否读到一并写出来, 让读日志的人自己能分辨是哪种情况。
        const bool siblingsAllRead = (t.siblingsRead >= t.logical && t.logical > 0);
        if (siblingsAllRead) {
            snprintf(buf, sizeof(buf),
                     "%d 逻辑核 / %d 物理核（未检测到超线程: thread_siblings_list %d/%d 核全部读到, "
                     "但每核的兄弟表只含它自己 —— 这是固件上报的样子, 不代表硬件一定没有 SMT）"
                     "→ 多核阶段 %d 线程 %s；来源 %s",
                     t.logical, t.physical, t.siblingsRead, t.logical,
                     auroraSmtThreadsCapped(), three, t.sourceText);
        } else {
            snprintf(buf, sizeof(buf),
                     "%d 逻辑核 / %d 物理核（未检测到超线程: thread_siblings_list 只读到 %d/%d 核"
                     "(errno=%d), 已退到 core_id+physical_package_id 判定, 未发现共享同一物理核的逻辑核 —— "
                     "同样不代表硬件一定没有 SMT）→ 多核阶段 %d 线程 %s；来源 %s",
                     t.logical, t.physical, t.siblingsRead, t.logical, t.siblingsErrno,
                     auroraSmtThreadsCapped(), three, t.sourceText);
        }
    }
    return std::string(buf);
}

// 线程数上限 = 可用**物理核**数(2026-10-08 口径变更; > 0 时把并行池的线程数夹到这个值)。
//
//  可用核数的定义(只有一处): `aurora_cpu_detail::topologyCached().spreadOrder.size()` ——
//  它就是"权威许可集合 ∩ 物理核口径"的落点表长度:
//    * 每个可用物理核恰好一个代表(频率位次最靠前的那个逻辑核), SMT 兄弟不进并行池;
//    * 拓扑未知 / 机器没有 SMT 时, 它与"可用核集合里的逻辑核数"逐位相同(恒等投影)。
//  旧口径(2026-10 之前到 2026-10-08): 取 effectiveOrderCached().size() = 可用**逻辑**核数。
//  为什么必须改成物理核: 真机(HOP-AL00)可用核集合是 cpu0-7, 而这 8 个逻辑核只落在 6 个物理核上
//  (c0-c3 小核无 SMT; {c4,c5} 与 {c6,c7} 各是一个带 SMT 的大核)。按逻辑核铺 8 条线程时,
//  2 条被钉在 SMT 兄弟上 —— 一个物理核两条线程各拿 ~50%, 于是"没有任何一颗核能到 90%"
//  (SmartPerf 逐秒数据: 多核阶段 75 个采样里 58 个采样 >=90% 的核数为 0), App 自己测的
//  并行度天花板也只有 ~6.1(= 可用物理核数)。改成物理核口径后: 线程数 == 可用物理核数,
//  池线程 i 独占 spreadOrder[i] 那个物理核, 于是
//    * 每个物理核恰好一条线程(不空、不挤、不再有 SMT 兄弟抢同一个核);
//    * 位次冲突这条路从此不可能发生。
//  仍然沿用"全机物理核数"的旧旧规则为什么不行: 那个数与"本进程真正能用的核"无关 ——
//  真机上因此出现过"9 个线程抢 7 个可用核"(note 原话: '池线程 2 个因位次冲突改到其它位次')。
//  最后一级兜底: 序列为空(频率表与可用核集合都读不到)时退回旧口径(可用核集合核数 / 不限制),
//  保证"读不到"的机器仍然是"不夹"的历史行为。
inline int auroraThreadCap()
{
    // 第一顺位 = 可用**逻辑**核数(effectiveOrderCached().size(), 真机 8)。
    //   (2026-10-08 曾短暂改成 topologyCached().spreadOrder.size() = 可用物理核数, 真机 6;
    //    2026-10-06 已按真机 A/B 回退, 理由见下面那段 —— 这里别再写回"物理核数"。)
    // 2026-10-06 回退: 真机 A/B 证明「物理核口径」在这台机器上是**错的**。
    //   HOP-AL00 可用核集合 = cpu0-7(8 逻辑核, 落在 6 个物理核上: c0-c3 小核无 SMT,
    //   {c4,c5} 与 {c6,c7} 各是一个带 SMT 的大核)。改成 6 线程后实测:
    //     8 线程(旧口径) gb8Multi = 1481.6 / 1488.4 / 1540.2, parallelism 实测 = 8.00
    //     6 线程(物理核口径) gb8Multi = 1069.8,                  parallelism 实测 = 6.00
    //   逐项吞吐掉 27%~48%(Clang 单线程那一项逐位不变, 说明机器状态相同),
    //   即两条 SMT 兄弟线程**确实在贡献吞吐**(每条约 +17%), 不是"白占核"。
    //   当初"改成物理核"的依据是 SmartPerf 的逐核占用率(两条线程各 ~50% = 没有核到 90%),
    //   那是 SMT 的正常形态, 被误读成"没喂饱"。经验证据优先: 用满内核允许的逻辑核。
    const int usable = (int)aurora_cpu_detail::effectiveOrderCached().size();
    if (usable > 0) {
        return usable;
    }
    const AuroraCpuEffectiveSet& s = auroraEffectiveSet();
    if (s.smtEnabled == 0 && s.topologyKnown && s.count > 0) {
        return s.count;
    }
    const int allowed = auroraAllowedCoreCount();
    return (allowed > 0) ? allowed : -1;
}

// 把线程数夹到上限(供并行池使用): n <= 0 = 不限制。
inline int auroraCapThreads(int threads)
{
    int t = (threads < 1) ? 1 : threads;
    const int cap = auroraThreadCap();
    if (cap > 0 && t > cap) {
        t = cap;
    }
    return t;
}

// 该核是否属于"实际使用集合"(SMT 关掉时被排除的兄弟核会返回 false)。
// 诊断用: 只是读数, 不绑核、不改任何行为。
inline bool auroraCpuInEffectiveSet(int cpu)
{
    if (cpu < 0) {
        return false;
    }
    const AuroraCpuEffectiveSet& s = auroraEffectiveSet();
    for (int i = 0; i < s.count; ++i) {
        if (s.cpus[i] == cpu) {
            return true;
        }
    }
    return false;
}

// 会话是否在跑(诊断/自检用)
inline bool auroraAffinitySessionActive()
{
    return aurora_cpu_detail::sessionState().active.load(std::memory_order_acquire) != 0;
}

// ===========================================================================
//  芯片判读用的只读探测: 逐核原始频率读数 + /proc/cpuinfo 的 MIDR
// ===========================================================================
//
//  为什么另开一节, 而不是改 readCoreMaxFreqKhz()
//  ---------------------------------------------------------------------------
//  绑核路径用的 readCoreMaxFreqKhz() 是"从 cpu0 顺序读, 第一个打不开的编号即视为末尾
//  (break)"。这条 break 定义了既有的绑核行为, 本节的任何代码都不许改它。
//  但它有一个必须被看见的副作用: 只要中间有任何一个核的 cpuinfo_max_freq 打不开
//  (权限 / 瞬时错误), 频率表就在那个核处截断, 它后面的核对绑核逻辑根本不存在 ——
//  包括可能是频率最高的那个 Prime 核。于是"大核簇"会建在一个更低的频率档上,
//  单核结果就变成了"跑在大核, 实际跑在中核"。
//
//  本节只做一件事: 在不碰绑核规则的前提下, 把每个核读到的东西记下来, 并把
//  "绑核用的频率表"与"逐核探测的全集"摆在一起对比, 让"漏核"一眼可见:
//    * 逐核扫全部真实核号(范围取自 cpu/present -> cpu/possible -> 固定 0..31),
//      不 break: 每个核都保留原始字符串 + 该核自己的 errno, 读失败也逐条列出;
//    * probeMaxKhz > tableMaxKhz  => 绑核路径漏掉了更高频的核 => 快簇可能建错档;
//    * tableCores  < probeOkCount => 绑核表被 readCoreMaxFreqKhz 的 break 截断;
//    * /proc/cpuinfo 的 CPU implementer / part(MIDR) 是最硬的 SoC 指纹:
//      part 号与频率档互相独立, 可以用来交叉印证(读不到就报 errno, 不猜)。
//  本节全部只读: 只 fopen/fread/fgets/fclose, 不写任何文件、不改任何全局状态、
//  不计分。进程内各读一次(magic static)。
// ===========================================================================

namespace aurora_chip_probe {

// 与 kMaxTopoCpus / readCoreMaxFreqKhz() 的扫描上限(32)保持一致
constexpr int kMaxProbeCpus = 32;

// ---- 原始读文件: 保留原字符串(只去掉首尾空白与换行), 供"逐核上报原始值" ----
// 与 aurora_smt_detail::readTextFile 的区别: 那个把 \n \r \t 一律换成空格且不区分
// "空文件"与"打不开"; 这里要的是逐字节留证: 读到什么就是什么, 读不到回传 errno。
inline bool readRawToken(const std::string& path, char* raw, int cap, int* errOut)
{
    if (errOut != nullptr) {
        *errOut = 0;
    }
    if (cap <= 1) {
        return false;
    }
    raw[0] = '\0';
    errno = 0;
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) {
        if (errOut != nullptr) {
            *errOut = errno != 0 ? errno : -1;   // 13 = EACCES 权限不足; 2 = ENOENT 路径不存在
        }
        return false;
    }
    const size_t n = fread(raw, 1, (size_t)(cap - 1), f);
    fclose(f);
    raw[n] = '\0';
    if (n == 0) {
        if (errOut != nullptr) {
            *errOut = -1;   // 打得开但没有内容: 与"打不开"分开记
        }
        return false;
    }
    size_t b = 0;
    size_t e = n;
    while (b < e && (raw[b] == ' ' || raw[b] == '\t' || raw[b] == '\r' || raw[b] == '\n')) {
        ++b;
    }
    while (e > b && (raw[e - 1] == ' ' || raw[e - 1] == '\t' || raw[e - 1] == '\r' || raw[e - 1] == '\n')) {
        --e;
    }
    const size_t len = e - b;
    if (b > 0) {
        memmove(raw, raw + b, len);
    }
    raw[len] = '\0';
    if (len == 0) {
        if (errOut != nullptr) {
            *errOut = -1;
        }
        return false;
    }
    return true;
}

// ---- 单个核的频率读数(原始字符串 + 解析值 + 该核自己的 errno) ----
struct AuroraCpuFreqSample {
    int cpu;        // 核号
    int khz;        // 解析出的 kHz; 0 = 这个核没读到
    int err;        // 0 = 读到并解析成功; >0 = fopen 失败的 errno;
                    // -1 = 打得开但内容为空; -2 = 读到内容但不是正的十进制数
    char raw[24];   // 原始字符串(逐字节保留, 只去了首尾空白/换行)
    AuroraCpuFreqSample() : cpu(-1), khz(0), err(0), raw() {}
};

// 逐核频率探测的完整结果(全部是"设备自己说的话")
struct AuroraCpuFreqProbe {
    int scanCount;                       // 实际扫了几个核号
    AuroraCpuFreqSample s[kMaxProbeCpus];
    int okCount;                         // 读成功(解析出正数)的核数
    int firstFailCpu;                    // 第一个读失败的核号(-1 = 全部成功)
    int firstFailErr;                    // 它的 errno
    int rangeSource;                     // 0 = cpu/present; 1 = cpu/possible; 2 = 固定扫 0..31
    int presentErrno;                    // cpu/present 读不到时的 errno
    int possibleErrno;                   // cpu/possible 读不到时的 errno
    // ---- 与"绑核用的频率表"对比 ----
    int tableCores;                      // readCoreMaxFreqKhz() 实际返回的核数
    int tableMaxKhz;                     // 那张表的最高频率
    int tableMaxCpu;                     // 最高频率落在哪个位次(-1 = 空表)
    int probeMaxKhz;                     // 逐核探测的最高频率
    int probeMaxCpu;                     // 它属于哪个核
    // ---- 频率档(逐核探测口径, 降序) ----
    int tierKhz[kMaxProbeCpus];
    int tierCount[kMaxProbeCpus];
    int tierN;
    // ---- 频率档(按物理核归并: 每个物理核取其兄弟线程里的最高频) ----
    int physTierKhz[kMaxProbeCpus];
    int physTierCount[kMaxProbeCpus];
    int physTierN;
    int physCount;                       // 归并出的物理核数
    // ---- 判定 ----
    int truncated;                       // 1 = 绑核表比逐核探测少核(被 break 截断)
    int missedFaster;                    // 1 = 逐核探测的最高频 > 绑核表的最高频(漏了更高频的核)
    char rangeText[96];                  // 扫描范围是怎么定下来的
    char perCoreText[1024];              // 逐核原始读数一行(可直接进 runlog)
    char verdictText[512];               // 漏核/截断的判定(人话)
    AuroraCpuFreqProbe()
        : scanCount(0), s(), okCount(0), firstFailCpu(-1), firstFailErr(0),
          rangeSource(2), presentErrno(0), possibleErrno(0),
          tableCores(0), tableMaxKhz(0), tableMaxCpu(-1),
          probeMaxKhz(0), probeMaxCpu(-1),
          tierKhz(), tierCount(), tierN(0),
          physTierKhz(), physTierCount(), physTierN(0), physCount(0),
          truncated(0), missedFaster(0), rangeText(), perCoreText(), verdictText()
    {
    }
};

// 把一个 (khz) 归入降序档位表; 返回档位下标
inline int addTier(int* khzTab, int* cntTab, int& n, int khz)
{
    for (int t = 0; t < n; ++t) {
        if (khzTab[t] == khz) {
            cntTab[t] += 1;
            return t;
        }
    }
    if (n >= kMaxProbeCpus) {
        return -1;
    }
    khzTab[n] = khz;
    cntTab[n] = 1;
    ++n;
    return n - 1;
}

// 档位表按频率降序(同频保持"首次遇到"的顺序, 与绑核表的排序口径一致)
inline void sortTiersDesc(int* khzTab, int* cntTab, int n)
{
    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            if (khzTab[b] > khzTab[a]) {
                const int tk = khzTab[a];
                khzTab[a] = khzTab[b];
                khzTab[b] = tk;
                const int tc = cntTab[a];
                cntTab[a] = cntTab[b];
                cntTab[b] = tc;
            }
        }
    }
}

// 逐核探测(只读)。范围确定顺序: cpu/present -> cpu/possible -> 固定 0..kMaxProbeCpus-1。
// 不因为某个核读不到就停下 —— 那正是本节要暴露的东西。
inline AuroraCpuFreqProbe readCpuFreqProbe()
{
    AuroraCpuFreqProbe p;
    int list[kMaxProbeCpus];
    int n = 0;
    char buf[256];
    int e1 = 0;
    if (::aurora_smt_detail::readTextFile("/sys/devices/system/cpu/present", buf, (int)sizeof(buf), &e1)) {
        n = ::aurora_smt_detail::parseCpuMask(buf, list, kMaxProbeCpus);
        if (n > 0) {
            p.rangeSource = 0;
            snprintf(p.rangeText, sizeof(p.rangeText), "cpu/present(%d 个核号)", n);
        }
    } else {
        p.presentErrno = e1;
    }
    if (n <= 0) {
        int e2 = 0;
        if (::aurora_smt_detail::readTextFile("/sys/devices/system/cpu/possible", buf, (int)sizeof(buf), &e2)) {
            n = ::aurora_smt_detail::parseCpuMask(buf, list, kMaxProbeCpus);
            if (n > 0) {
                p.rangeSource = 1;
                snprintf(p.rangeText, sizeof(p.rangeText),
                         "cpu/possible(%d 个核号; cpu/present errno=%d)", n, p.presentErrno);
            }
        } else {
            p.possibleErrno = e2;
        }
    }
    if (n <= 0) {
        p.rangeSource = 2;
        for (int c = 0; c < kMaxProbeCpus; ++c) {
            list[c] = c;
        }
        n = kMaxProbeCpus;
        snprintf(p.rangeText, sizeof(p.rangeText),
                 "固定扫描 0..%d(cpu/present errno=%d, cpu/possible errno=%d)",
                 kMaxProbeCpus - 1, p.presentErrno, p.possibleErrno);
    }
    if (n > kMaxProbeCpus) {
        n = kMaxProbeCpus;
    }
    p.scanCount = n;

    for (int i = 0; i < n; ++i) {
        const int cpu = list[i];
        AuroraCpuFreqSample& sm = p.s[i];
        sm.cpu = cpu;
        const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
            "/cpufreq/cpuinfo_max_freq";
        int err = 0;
        if (readRawToken(path, sm.raw, (int)sizeof(sm.raw), &err) == false) {
            sm.khz = 0;
            sm.err = (err != 0) ? err : -1;
        } else {
            char* endp = nullptr;
            const long v = strtol(sm.raw, &endp, 10);
            if (endp == sm.raw || v <= 0) {
                sm.khz = 0;
                sm.err = -2;   // 读到内容了, 但不是正的十进制数
            } else {
                sm.khz = (int)v;
                sm.err = 0;
            }
        }
        if (sm.err == 0) {
            ++p.okCount;
            if (sm.khz > p.probeMaxKhz) {
                p.probeMaxKhz = sm.khz;
                p.probeMaxCpu = cpu;
            }
            addTier(p.tierKhz, p.tierCount, p.tierN, sm.khz);
        } else if (p.firstFailCpu < 0) {
            p.firstFailCpu = cpu;
            p.firstFailErr = sm.err;
        }
    }
    sortTiersDesc(p.tierKhz, p.tierCount, p.tierN);

    // ---- 按物理核归并(每个物理核取兄弟线程里的最高频); 拓扑未知时一个逻辑核算一个 ----
    {
        const AuroraCpuTopoInfo& ti = ::auroraSmtTopology();
        int physMax[kMaxProbeCpus];
        int physSeen[kMaxProbeCpus];
        for (int i = 0; i < kMaxProbeCpus; ++i) {
            physMax[i] = 0;
            physSeen[i] = 0;
        }
        for (int i = 0; i < n; ++i) {
            if (p.s[i].khz <= 0) {
                continue;
            }
            const int cpu = p.s[i].cpu;
            int ph = (cpu >= 0 && cpu < kMaxTopoCpus) ? ti.physicalOfCpu[cpu] : cpu;
            if (ph < 0 || ph >= kMaxProbeCpus) {
                ph = cpu;
            }
            if (ph < 0 || ph >= kMaxProbeCpus) {
                continue;
            }
            if (physSeen[ph] == 0 || p.s[i].khz > physMax[ph]) {
                physMax[ph] = p.s[i].khz;
                physSeen[ph] = 1;
            }
        }
        for (int i = 0; i < kMaxProbeCpus; ++i) {
            if (physSeen[i] == 0) {
                continue;
            }
            ++p.physCount;
            addTier(p.physTierKhz, p.physTierCount, p.physTierN, physMax[i]);
        }
        sortTiersDesc(p.physTierKhz, p.physTierCount, p.physTierN);
    }

    // ---- 与"绑核用的频率表"对比(这是本节存在的全部理由) ----
    {
        const std::vector<int>& table = ::aurora_cpu_detail::coreMaxFreqKhzCached();
        p.tableCores = (int)table.size();
        for (size_t i = 0; i < table.size(); ++i) {
            if (table[i] > p.tableMaxKhz) {
                p.tableMaxKhz = table[i];
                p.tableMaxCpu = (int)i;
            }
        }
        p.truncated = (p.okCount > 0 && p.tableCores < p.okCount) ? 1 : 0;
        p.missedFaster = (p.tableMaxKhz > 0 && p.probeMaxKhz > p.tableMaxKhz) ? 1 : 0;
    }

    // ---- 逐核原始读数一行(读失败的核逐个列出, 带它自己的 errno) ----
    {
        std::string t;
        for (int i = 0; i < n; ++i) {
            char one[64];
            if (p.s[i].err == 0) {
                snprintf(one, sizeof(one), "%scpu%d=%s(%d kHz)",
                         (i == 0 ? "" : " "), p.s[i].cpu, p.s[i].raw, p.s[i].khz);
            } else {
                // 原始字符串也照抄(读失败时通常是空串), 再补上 errno
                snprintf(one, sizeof(one), "%scpu%d=读不到(errno=%d)",
                         (i == 0 ? "" : " "), p.s[i].cpu, p.s[i].err);
            }
            t += one;
        }
        snprintf(p.perCoreText, sizeof(p.perCoreText), "%s", t.c_str());
    }

    // ---- 判定文本(人话) ----
    {
        char head[320];
        snprintf(head, sizeof(head),
                 "绑核用频率表 %d 核(最高 %d kHz%s) vs 逐核探测 %d/%d 核成功(最高 %d kHz%s, 范围 %s)",
                 p.tableCores, p.tableMaxKhz,
                 p.tableMaxCpu >= 0 ? "" : "/空表",
                 p.okCount, p.scanCount,
                 p.probeMaxKhz, p.probeMaxCpu >= 0 ? "" : "/无", p.rangeText);
        std::string t(head);
        if (p.okCount <= 0) {
            t += " · 一个核的频率都没读到: 绑核全程未启用(静默降级)";
        } else if (p.missedFaster) {
            char b[224];
            snprintf(b, sizeof(b),
                     " · 不一致: 逐核探测到的最高频 %d kHz 比绑核表(最高 %d kHz)更高 —— "
                     "绑核路径漏掉了至少一个更高频的核(已知 readCoreMaxFreqKhz 在第一个打不开的核处 break), "
                     "大核簇可能建在错误的频率档上", p.probeMaxKhz, p.tableMaxKhz);
            t += b;
        } else if (p.truncated) {
            char b[160];
            snprintf(b, sizeof(b),
                     " · 不一致: 绑核表比逐核探测少 %d 个核(被 break 截断), 但截断掉的核频率都不高于已有的最高频",
                     p.okCount - p.tableCores);
            t += b;
        } else {
            t += " · 一致: 绑核表覆盖了全部逐核探测成功的核, 且最高频相同";
        }
        if (p.firstFailCpu >= 0) {
            char b[128];
            snprintf(b, sizeof(b), " · 首个读失败的核 cpu%d(errno=%d)",
                     p.firstFailCpu, p.firstFailErr);
            t += b;
        }
        snprintf(p.verdictText, sizeof(p.verdictText), "%s", t.c_str());
    }
    return p;
}

// 进程内只读一次(与拓扑/频率表同一套 magic static 设计; 只读, 无副作用)
inline const AuroraCpuFreqProbe& cpuFreqProbeCached()
{
    static const AuroraCpuFreqProbe p = readCpuFreqProbe();
    return p;
}

// ===========================================================================
//  /proc/cpuinfo: CPU implementer / architecture / variant / part / revision
//  (ARM64 上这套 MIDR 字段是最硬的 SoC 指纹: 它与频率档互相独立,
//   可以用来交叉印证频率档推出的型号; 读不到就报 errno, 不猜)
// ===========================================================================

constexpr int kMaxCpuinfoCombos = 16;

// 一个 MIDR 组合(同一种核在 /proc/cpuinfo 里就是一个组合)
struct AuroraCpuinfoCombo {
    int count;              // 这种组合对应多少个 processor 块
    char implementer[16];
    char architecture[16];
    char variant[16];
    char part[16];
    char revision[16];
    AuroraCpuinfoCombo() : count(0), implementer(), architecture(), variant(), part(), revision() {}
};

struct AuroraCpuinfoProbe {
    int ok;                     // 1 = /proc/cpuinfo 打开成功
    int openErrno;              // 打开失败时的 errno(13 = EACCES, 2 = ENOENT)
    int processorLines;         // "processor" 行的条数(= 逻辑核数)
    int comboCount;             // 不同的 MIDR 组合数
    AuroraCpuinfoCombo combo[kMaxCpuinfoCombos];
    char hardware[80];          // "Hardware" 行(没有则空串)
    char modelName[80];         // "model name" 行(没有则空串)
    char partDecimal[256];      // 每个 part 号的十进制换算(资料里的 3334/3335 就是它)
    char text[768];             // 一行可读文本
    AuroraCpuinfoProbe()
        : ok(0), openErrno(0), processorLines(0), comboCount(0),
          combo(), hardware(), modelName(), partDecimal(), text()
    {
    }
};

// 取 part 号的十进制值: "0xd06" -> 3334; "1234" -> 1234; 取不到 -> -1
inline int partNumberToDecimal(const char* s)
{
    if (s == nullptr || s[0] == '\0') {
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return (int)strtol(s + 2, nullptr, 16);
    }
    return (int)strtol(s, nullptr, 10);
}

inline void trimInPlace(char* s)
{
    if (s == nullptr) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t b = 0;
    while (s[b] == ' ' || s[b] == '\t') {
        ++b;
    }
    if (b > 0) {
        memmove(s, s + b, strlen(s + b) + 1);
    }
}

inline void copyCstr(char* dst, int cap, const char* src)
{
    if (dst == nullptr || cap <= 0) {
        return;
    }
    snprintf(dst, (size_t)cap, "%s", (src != nullptr) ? src : "");
}

inline AuroraCpuinfoProbe readCpuinfoProbe()
{
    AuroraCpuinfoProbe p;
    errno = 0;
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f == nullptr) {
        p.ok = 0;
        p.openErrno = errno != 0 ? errno : -1;
        snprintf(p.text, sizeof(p.text),
                 "/proc/cpuinfo 读不到(errno=%d) —— 取不到 CPU implementer / part(MIDR); "
                 "errno 13 = EACCES 权限不足(用 hdc shell 读同一条路径也会被拒), 2 = ENOENT",
                 p.openErrno);
        return p;
    }
    p.ok = 1;
    char curImpl[16] = "";
    char curArch[16] = "";
    char curVar[16] = "";
    char curPart[16] = "";
    char curRev[16] = "";
    int inBlock = 0;
    char line[512];

    auto flushBlock = [&p, &curImpl, &curArch, &curVar, &curPart, &curRev]() {
        int found = -1;
        for (int i = 0; i < p.comboCount; ++i) {
            if (strcmp(p.combo[i].implementer, curImpl) == 0 &&
                strcmp(p.combo[i].architecture, curArch) == 0 &&
                strcmp(p.combo[i].variant, curVar) == 0 &&
                strcmp(p.combo[i].part, curPart) == 0 &&
                strcmp(p.combo[i].revision, curRev) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            if (p.comboCount < kMaxCpuinfoCombos) {
                found = p.comboCount;
                ++p.comboCount;
                copyCstr(p.combo[found].implementer, 16, curImpl);
                copyCstr(p.combo[found].architecture, 16, curArch);
                copyCstr(p.combo[found].variant, 16, curVar);
                copyCstr(p.combo[found].part, 16, curPart);
                copyCstr(p.combo[found].revision, 16, curRev);
            } else {
                found = kMaxCpuinfoCombos - 1;   // 表满(不该出现): 并到最后一条上
            }
        }
        p.combo[found].count += 1;
    };

    while (fgets(line, (int)sizeof(line), f) != nullptr) {
        char* colon = strchr(line, ':');
        if (colon == nullptr) {
            continue;
        }
        *colon = '\0';
        char* key = line;
        char* val = colon + 1;
        trimInPlace(key);
        trimInPlace(val);
        if (key[0] == '\0') {
            continue;
        }
        if (strcmp(key, "processor") == 0) {
            if (inBlock) {
                flushBlock();
            }
            inBlock = 1;
            curImpl[0] = '\0';
            curArch[0] = '\0';
            curVar[0] = '\0';
            curPart[0] = '\0';
            curRev[0] = '\0';
            ++p.processorLines;
        } else if (strcmp(key, "CPU implementer") == 0) {
            copyCstr(curImpl, 16, val);
        } else if (strcmp(key, "CPU architecture") == 0) {
            copyCstr(curArch, 16, val);
        } else if (strcmp(key, "CPU variant") == 0) {
            copyCstr(curVar, 16, val);
        } else if (strcmp(key, "CPU part") == 0) {
            copyCstr(curPart, 16, val);
        } else if (strcmp(key, "CPU revision") == 0) {
            copyCstr(curRev, 16, val);
        } else if (strcmp(key, "Hardware") == 0) {
            copyCstr(p.hardware, 80, val);
        } else if (strcmp(key, "model name") == 0) {
            copyCstr(p.modelName, 80, val);
        }
    }
    if (inBlock) {
        flushBlock();
    }
    fclose(f);

    // part 号的十进制换算(资料里 9030 家族写作 3334/3335, 就是 0xd06/0xd07)
    {
        std::string t;
        for (int i = 0; i < p.comboCount; ++i) {
            const int dec = partNumberToDecimal(p.combo[i].part);
            char one[64];
            if (dec >= 0) {
                snprintf(one, sizeof(one), "%s%s=%d", (i == 0 ? "" : " "),
                         p.combo[i].part[0] != '\0' ? p.combo[i].part : "(空)", dec);
            } else {
                snprintf(one, sizeof(one), "%s%s",
                         (i == 0 ? "" : " "), p.combo[i].part[0] != '\0' ? p.combo[i].part : "(空)");
            }
            t += one;
        }
        snprintf(p.partDecimal, sizeof(p.partDecimal), "%s", t.c_str());
    }

    // 一行可读文本
    {
        std::string t;
        char head[160];
        snprintf(head, sizeof(head), "/proc/cpuinfo 可读: processor 行 %d 条; MIDR 组合 %d 种",
                 p.processorLines, p.comboCount);
        t += head;
        for (int i = 0; i < p.comboCount; ++i) {
            const int dec = partNumberToDecimal(p.combo[i].part);
            char one[224];
            snprintf(one, sizeof(one),
                     " · [implementer=%s architecture=%s variant=%s part=%s%s revision=%s ×%d]",
                     p.combo[i].implementer[0] != '\0' ? p.combo[i].implementer : "(空)",
                     p.combo[i].architecture[0] != '\0' ? p.combo[i].architecture : "(空)",
                     p.combo[i].variant[0] != '\0' ? p.combo[i].variant : "(空)",
                     p.combo[i].part[0] != '\0' ? p.combo[i].part : "(空)",
                     "",  // 十进制单独写在 partDecimal 里, 这里不重复
                     p.combo[i].revision[0] != '\0' ? p.combo[i].revision : "(空)",
                     p.combo[i].count);
            t += one;
            if (dec >= 0) {
                char two[24];
                snprintf(two, sizeof(two), "(=%d)", dec);
                t += two;
            }
        }
        if (p.hardware[0] != '\0') {
            t += " · Hardware=";
            t += p.hardware;
        }
        if (p.modelName[0] != '\0') {
            t += " · model name=";
            t += p.modelName;
        }
        snprintf(p.text, sizeof(p.text), "%s", t.c_str());
    }
    return p;
}

inline const AuroraCpuinfoProbe& cpuinfoProbeCached()
{
    static const AuroraCpuinfoProbe p = readCpuinfoProbe();
    return p;
}

} // namespace aurora_chip_probe

#endif

