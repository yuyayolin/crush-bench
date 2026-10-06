#ifndef AURORA_BENCH_H
#define AURORA_BENCH_H

#include <string>

struct BenchOutcome {
    std::string name;
    double ms;
    double score;
    std::string detail;
    double metric;
    std::string unit;
    double parallelism;

    // ---- 落核 / 标称上限 / 运行时频率取证(2026-10 追加; 自研套件路径) ----
    //  与 CS1 路径同口径、同数据源(全部来自 cpu_affinity.h 与 cpu_freq_sample.h)。
    //  为什么必须补: 自研套件此前每一项在报告里都只有 cpu=-1 · maxKhz=0, 于是
    //  "这一项跑在哪颗核上、那颗核的标称上限是多少、实际跑到多少"三个问题一个都答不了 ——
    //  而"两台同芯片设备自研单核差 37.7%、CS1 单核只差 0.24%"这件事的根因正藏在这里。
    //  全部是旁路诊断: 不参与 metric / unit / k / conv / 计分公式, 也不进任何复合分。
    int cpu = -1;                  // 跑完时所在的核(-1 = 取不到)
    int cpuAtStart = -1;           // 开跑时的核(-1 = 未知; 与 cpu 同源, 只作参考)
    int cpuMaxKhz = 0;             // cpu 那颗核的 cpuinfo_max_freq(kHz; 0 = 未知)
    int cpuRank = -1;              // 该核在全机频率降序表里的位次(-1 = 未知)
    int cpuBound = 0;              // 1 = 已把单核阶段的线程绑到生效快簇
    int cpuInFastClusterJudged = 0;   // 1 = 生效快簇已定义(cpuInFastCluster 才有意义)
    int cpuInFastCluster = 0;      // 1 = 跑完时所在核属于生效快簇
    int cpuInMachineTopTier = 0;   // 1 = 跑完时所在核属于全机最快频率档
    int cpuFastClusterCores = 0;   // 生效快簇核数
    int cpuFastClusterMaxKhz = 0;  // 生效快簇最高频率(kHz)
    int cpuMachineTopTierCores = 0;   // 全机最快档核数
    int cpuMachineTopTierKhz = 0;     // 全机最快档频率(kHz)
    int cpuAllowedCount = 0;       // 内核实测允许本进程用的核数(0 = 读不到)
    int cpuSingleTargetCores = 0;  // 单核阶段绑核目标核数(= 生效快簇核数)
    std::string cpuInfo;           // 一行中文说明(可直接显示; '' = 未上报)
    std::string runFreq;           // 运行时频率一行(native runFreq 文本; '' = 未采到样本)
};


int auroraCpuCount();
int auroraTestCount();
std::string auroraTestName(int id);
BenchOutcome auroraRunTest(int id, int threads);
std::string auroraVersion();

// ---- native 崩溃取证装置(实现在 crash_guard.cpp, 只记录, 不改任何负载行为) ----
// 处理器安装: crash_guard.cpp 的 __attribute__((constructor)), 模块 dlopen 时自动完成。
#ifdef __cplusplus
extern "C" {
#endif

// 每次开始跑一项之前调用(napi 层已接)。标记本身仍然只做一次 bounded copy + 3 次标量写;
// 2026-10-12 追加两件旁路事(都在负载开跑之前, 不在任何计时窗口里, 不计分):
//   ① 读一次 /proc/self/status(VmSize/VmRSS/VmHWM/Threads) 做逐项地址空间归因;
//   ② 至少隔 1 秒才真的读一次 /proc/self/maps 刷新"模块表"(库是逐步 dlopen 的)。
//   任何失败都只是让记录写"读不到", 不改任何负载行为与分数。
// phase 例如 "CS1 单核", item 例如 "HDR"。
void auroraSetCurrentItem(const char* phase, const char* item, int index, int total);

// 一项跑完 / 异常退出前调用: 把标记清成"无负载运行中(空闲)"。
// 必须用这个而不是自己调 auroraSetCurrentItem 写那两句空闲文案 —— 只有它会把内部的
//   "当前没有负载在跑"标志置 1, 看门狗才不会把两项之间的空档当成卡死(真机踩过:
//   空档 32 秒 -> native_hang.txt 写出 112 KB 的假现场)。
void auroraSetIdleMarker(void);

// 由 ArkTS 传 context.filesDir 进来; 在此把 <dir>/native_crash.txt open 好并常驻 fd
// (处理器里不做 open)。返回 1 = 成功。
int auroraSetLogDir(const char* dir);

// 证据文件的绝对路径(未设置时为空串)。
const char* auroraCrashLogPath(void);

// 信号处理器是否已武装(1/0)。
int auroraCrashGuardReady(void);

// 证据文件是否存在(启动自检)。
int auroraCrashReportExists(void);

// 读证据文件内容到调用者缓冲, 返回字节数; -1 = 无文件/无记录。
// 文件过大时只返回尾部(最近一次崩溃)并丢弃首行残片。
int auroraTakeCrashReport(char* buf, int cap);

// 清空证据(读完成功横幅后调用)。
void auroraClearCrashReport(void);

// 采样一次内存足迹(VmRSS/VmHWM/VmSize/Threads), 结果留在内部供 auroraLastMemory 取走。
// 建议 ArkTS 每项开跑前调一次(一次 lseek+read, 只在普通上下文)。
void auroraSampleMemory(void);

// 取最近一次采样的一行文本, 返回长度; -1 = 还没采样过。
int auroraLastMemory(char* buf, int cap);

// ---- 逐项地址空间归因(旁路, 不计分; 2026-10-12 追加) ----
// 每个 item 标记点采一次 VmSize/VmRSS/VmHWM/Threads, 结算出"相对上一项的增量";
// 逐行落到 <filesDir>/native_memory.txt(进程被 SIGKILL 时盘上唯一的地址空间归因),
// 崩溃记录里也会整表打一遍(含"增量最大的一项")。
// 判据写死: 单项 ΔVmSize > 512 MB 或 ΔVmRSS > 256 MB => "本项占用异常, 见该项明细"。
// 返回整表文本长度; -1 = 参数不对或还没有任何采样。
int auroraItemMemoryReport(char* buf, int cap);
// 逐项账目文件路径(一般是 <filesDir>/native_memory.txt); 空串 = 还没调 setLogDir。
const char* auroraItemMemoryLogPath(void);

// 真机自检钩子(排障期用, 发布版可删):
// mode 0 = 只往证据文件写一条链路自检记录(不崩); 1 = 故意空指针写触发 SIGSEGV; 2 = raise(SIGABRT)。
void auroraCrashSelfTest(int mode);

// ---- 卡死取证: 挂起看门狗 + 栈采样(实现在 crash_guard.cpp) ----
// 证据文件 <filesDir>/native_hang.txt, 由 auroraSetLogDir 一并打开并启动看门狗。
// 看门狗: 每 5 秒纯内存检查“当前项 + 已运行时长”, 超过 30 秒 -> 向跑该项的线程
// tgkill(SIGUSR1) 抓 64 层栈, 采 3 次(间隔 1 秒), 采样后不杀进程。
const char* auroraHangLogPath(void);
int auroraHangWatchdogAlive(void);
// 一行状态(看门狗开关/阈值/已采样次数/展开器来源/当前项与已运行秒数/证据路径)。
int auroraHangStatus(char* buf, int cap);
// 读证据文件尾部, 返回字节数; -1 = 还没有卡死记录。
int auroraTakeHangReport(char* buf, int cap);
int auroraHangReportExists(void);
void auroraClearHangReport(void);
// 自检: 起一个空转线程(默认 45 秒)并标成当前项, 30 秒后看门狗会自动抓它。返回 1 = 已启动。
int auroraHangSelfTest(int seconds);

#ifdef __cplusplus
}
#endif

#endif
