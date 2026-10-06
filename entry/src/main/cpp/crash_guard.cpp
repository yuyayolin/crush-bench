// ============================================================================
//  crash_guard.cpp — 「极光跑分」native 崩溃取证装置(只记录, 不改任何负载行为)
//
//  目的: 真机跑分中途进程消失时, 区分
//    (A) 进程收到致命信号(SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT/SIGSYS)
//        —— 我们的代码越界 / UB / 被 seccomp 拦截;
//    (B) 进程被系统 SIGKILL(内存压力 / 后台管制)
//        —— SIGKILL 不可捕获, 所以“盘上没有记录”本身就是 (B) 的证据。
//
//  设计约束(全部满足):
//    * 处理器内只做异步信号安全的事: 只用 write()/_exit()/kill()-级原语 + 纯内存读写;
//      不在处理器里 open/malloc/printf/snprintf/std::string/hilog/syslog。
//    * 证据文件 fd 与 /proc/self/status 的 fd 都在“安装处理器时”预先 open 好。
//    * 记录是写死的静态缓冲区 + 手写整数格式化(无 snprintf, 无堆)。
//    * 当前项标记用固定大小静态缓冲区, 只做一次 bounded copy(无 std::string)。
//    * 处理器最后转发给原处理器(链式), 再 _exit(128+sig), 不会 return 回去重复崩溃。
//
//  只有在“当前项标记”和“写记录”里碰内存, 跑分热路径上一次 setCurrentItem
//  只有 1 次 bounded copy + 3 次 volatile 标量写, 无系统调用、无分配。
//
//  2026-10-12 追加(真机实证驱动的升级)
//  真机现场(平板 PCE-W30, 8.0, 两次崩溃记录逐字段相同)最关键的一栏是空的:
//      module : pc 不在 libaurorabench.so 的代码段内 => 崩在系统库/运行时/其它 .so
//               [self_base=0x5b278c1000 text=0x5b279a4000-0x5b27d15000]
//    pc=0x5a77adadd0 比 self_base 低约 2.9 GB, 落在别的模块里 —— 可能是
//    libc++_shared.so(我们的 std::string / operator new 都住在那儿) / libc.so(malloc/free,
//    堆被写坏之后最先炸的通常是它) / ArkTS 运行时 / libaurorasn.so。这几种情况指向完全不同的
//    结论, 而老记录区分不了。判据上的坑也要写清: pc 不在我们的 .so 里 ≠ 不是我们的 bug
//    (堆写坏的典型表现正是在别人的 free 里炸)。
//  于是补了两块东西:
//    (A)「模块表」: 在普通上下文(安装时 + 每个标记点, 1 秒节流)读一次 /proc/self/maps,
//        把所有可执行段(perms 含 x)解析成定长数组(start/end/file_offset/路径; 表满即停
//        并记下"还有 N 段没记"); 处理器里只做纯内存线性扫描 —— pc 与 lr 各查一次表,
//        再把崩溃线程栈上的字过一遍给出"穷人的调用栈"。处理器里不 dladdr /
//        dl_iterate_phdr / 重新读 maps(会碰动态链接器的锁, 而现场很可能正是死在那把锁上)。
//    (B)「逐项地址空间归因」: 每个 item 标记点读一次 /proc/self/status 的
//        VmSize/VmRSS/VmHWM/Threads, 结算出"相对上一项的增量", 逐行落到 native_memory.txt,
//        并在崩溃记录里整表打一遍。真机现场 VmSize=14034764 kB(14 GB) 远超平板 11.5 GB 物理内存,
//        而 VmRSS 只有 202808 kB —— 到底哪一项撑起了这 14 GB, 现在记录里能直接看到。
//        判据写死: 单项 ΔVmSize > 512 MB 或 ΔVmRSS > 256 MB => "本项占用异常, 见该项明细"。
//    (A)(B) 都是旁路: 不计分、不改任何负载的算法/尺寸/线程数; 读不到就写
//        "读不到", 任何一步失败都只影响记录本身, 不影响跑分结果。
// ============================================================================

// 必须在任何 include 之前: -std=c++17 会定义 __STRICT_ANSI__, musl 的 features.h
// 因此不会自动打开 _BSD_SOURCE/_XOPEN_SOURCE, sigaction/siginfo_t/SA_SIGINFO/
// sigaltstack/gettid/mcontext_t 都会被头文件隐藏。定义 _GNU_SOURCE 才能用。
#define _GNU_SOURCE 1

#include "bench.h"
#include "crash_modmap.h"   // 预先解析的「模块表」+ 纯内存归属判定 + 穷人的栈扫描(离线可断言, 见文件头)

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>   // fchmod: 把取证文件权限精确纠正成 0660(见 kEvidenceMode)
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// 编译期开关: 崩溃时是否顺带抓一次内存快照(读预先 open 好的 /proc/self/status)
// 关掉它则处理器内除 write() 外零系统调用; 打开它多两次(已在跑分之外的)调用。
// 默认打开 —— 内存快照是区分 (A)/(B) 的关键证据。
// ---------------------------------------------------------------------------
#ifndef AURORA_CRASH_MEM_SNAPSHOT
#define AURORA_CRASH_MEM_SNAPSHOT 1
#endif

// 编译期开关: 标记“当前项”时是否顺带取一次墙钟(记录里就能有 age_ms = 崩前跑了多久)。
// 置 0 则 auroraSetCurrentItem 一次 libc 调用都不做(bounded copy + 标量写)。
#ifndef AURORA_CRASH_MARK_TS
#define AURORA_CRASH_MARK_TS 1
#endif

namespace {

constexpr int kPhaseCap = 48;      // 阶段名(如 "CS1 单核")
constexpr int kItemCap  = 96;      // 当前负载名(如 "HDR")
constexpr int kPathCap  = 384;     // 证据文件绝对路径
constexpr int kMaxSigs  = 8;       // 最多拦截的信号数
// 崩溃记录本体: 静态缓冲(不再放处理器栈上)。逐项归因表可能有上万字节, 而备用信号栈只有
// 64 KB —— 它存在的理由恰恰是"栈溢出型 SIGSEGV 也要能落笔", 所以记录缓冲改用 BSS。
// g_inHandler 保证同一时刻只有一个写者, 静态缓冲不会互相踩。
constexpr int kCrashRecCap = 20480;
constexpr int kProcCap  = 8192;    // /proc/self/status 的读缓冲(status 在真机上可超 3KB)
constexpr int kAltSize  = 64 * 1024; // 备用信号栈(栈溢出也能执笔)

// ---------------------- 证据文件的创建权限: 0600 -> 0660 --------------------
// native_crash.txt / native_hang.txt 是故障诊断证据, 不是隐私数据: 内容只有
// 信号号/si_code/故障地址/寄存器/负载名/内存快照/调用栈, 不含任何用户数据、文件内容
// 或账号信息。
// 真机教训: 第一版用 0600 创建, 属主是 App 自己 -> 开发者用 hdc file recv 拉不下来
//   (runlog.jsonl 是 0660 所以能拉), 31 KB 的卡死记录因此留在机器上读不到, 排障直接
//   断在"拿不到现场"这一步。
// 0660 = rw-rw----: App 自己可写、与 App 同组的开发者/hdc 侧可读 —— 既够取证, 又不像
//   0644 那样对"其它用户"开放。
// 注意 open(..., mode) 的 mode 会被进程 umask 削掉, 所以下面还会无条件 fchmod 一次
//   (见 cgOpenEvidence): 这样无论 umask 是什么、无论文件是不是老版本用 0600 建的,
//   最终权限都精确落在 0660 上。
constexpr mode_t kEvidenceMode = 0660;

// 打开一个取证文件(O_CREAT), 并把权限精确设成 kEvidenceMode。返回 fd, -1 = 失败。
// 只在普通上下文调用(构造期 / setLogDir), 不在信号处理器里调用。
int cgOpenEvidence(const char* path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, kEvidenceMode);
    if (fd < 0) {
        return -1;
    }
    // 兜底纠正: 对已经存在的文件, O_CREAT 不会改动它的权限位, 老版本留下的 0600
    // 文件必须在这里显式修一次, 否则它永远只能被 App 自己读, hdc 依旧拉不下来。
    (void)fchmod(fd, kEvidenceMode);
    return fd;
}

// ------------------------------ 全局状态(处理器只读) -----------------------
struct SigSlot {
    int signo;
    struct sigaction prev;   // 链式转发用: 我们安装时替换掉的原处理器
};

SigSlot g_slots[kMaxSigs];
int g_slotCount = 0;

volatile sig_atomic_t g_crashFd  = -1;   // 证据文件 fd(安装时 open 好)
volatile sig_atomic_t g_procFd   = -1;   // /proc/self/status fd(安装时 open 好, 崩溃处理器用)
volatile sig_atomic_t g_procFd2  = -1;   // /proc/self/status 的第二个 fd: 逐项归因专用
                                         // (读取会推进文件偏移, 所以每个采样者各用一个 fd,
                                         //  免得负载线程与 JS 线程同时读时互相踩)
volatile sig_atomic_t g_inHandler = 0;   // 防重入: 多线程同时崩时只记第一条
volatile sig_atomic_t g_ready     = 0;   // 处理器是否已装好

// 当前项标记: 写者(普通上下文)与读者(处理器)之间的无锁握手
volatile sig_atomic_t g_seq   = 0;       // 奇=正在写, 偶=稳定
volatile sig_atomic_t g_index = 0;
volatile sig_atomic_t g_total = 0;
volatile long long    g_markMs = 0;      // 标记时的墙钟毫秒
char g_phase[kPhaseCap];                 // NUL 结尾
char g_item[kItemCap];
volatile sig_atomic_t g_markTid = 0;     // 打标记的线程 tid(= 正在跑该项的线程), 供看门狗定向信号
// 2026-10-06 标记的语义: 1 = 当前没有负载在跑(两项之间的空档 / 空闲), 0 = 有负载在跑。
//   为什么必须有它: 看门狗只会看"g_markMs 到现在有多久", 而空档标记同样会写 g_markMs ——
//   真机上于是出现"疑似卡死: phase=(无负载运行中: 上一项已结束) item=(空闲) 已运行 32 秒"的
//   假现场(native_hang.txt 112 KB), 目标线程早就结束了(投递失败 errno=3 = ESRCH)。
//   空闲不是卡死: 看门狗在空闲状态下不判卡死(见 cgWatchdogMain)。
volatile sig_atomic_t g_idle = 1;        // 开机到第一项开始之前也算空闲

// ---- 挂起看门狗(卡死取证)共用状态 ----
volatile sig_atomic_t g_hangFd = -1;              // <filesDir>/native_hang.txt 的常驻 fd
char g_hangPath[kPathCap + 24];
volatile sig_atomic_t g_hangPending = 0;          // 看门狗已投递、尚未被处理器消费的采样请求数
volatile sig_atomic_t g_hangDone = 0;             // 处理器完成采样的次数
volatile sig_atomic_t g_hangInHandler = 0;
volatile sig_atomic_t g_hangSampleNo = 0;
volatile sig_atomic_t g_hangThreadStarted = 0;
// 卡死证据文件的硬上限(2026-10-06 追加): 真机上出现过一次"空档误报"就写掉 112 KB
// (1853 行)的假现场。误报本身已由 g_idle + 线程存在性检查堵住, 这里再压一道总量闸门:
// 超过上限只写一行说明, 不再追加, 保证卡死记录永远是可读的几十 KB 量级。
constexpr long long kMaxHangFileBytes = 96 * 1024;
long long g_hangBytes = 0;                  // 该文件已经写了多少字节(含历史内容)
bool g_hangSizeCapped = false;              // 已写过"已达上限"说明
const char* g_hangWhy = nullptr;                  // 静态字面量: 为什么采这一枪
volatile unsigned long long g_hangStackTop = 0;   // 目标线程栈顶(发信号前从 /proc 读, 给原始栈窗口封顶)
unsigned long long g_hangPcs[64];                 // 每次采样的原始 PC(处理器的静态缓冲, 无分配)
volatile sig_atomic_t g_hangFrameCount = 0;

// 挂起看门狗相关函数(定义在本文件后面重新打开的匿名命名空间里, 同一命名空间)
void cgInstallHang();
int cgStartWatchdog();
void cgHangStatusLine(char* buf, int cap);
int cgStartHangSelfTest(int seconds);

char g_logPath[kPathCap + 24];           // "<filesDir>/native_crash.txt"
char g_crashRec[kCrashRecCap];           // 崩溃记录本体(静态, 见 kCrashRecCap 的理由)
char g_procBuf[kProcCap];                // 崩溃处理器专用(/proc/self/status 快照)
#if AURORA_CRASH_MEM_SNAPSHOT
char g_procBuf2[kProcCap];               // 普通上下文采样专用(与处理器缓冲分开, 免得互相踩)
#endif

// ArkTS 侧“最后一口内存”: 每项开跑前调一次, 结果可以由 ArkTS 写进自己的面包屑,
// 这样即使进程被 SIGKILL(拿不到任何崩溃记录)也能看到临死前的内存足迹。
volatile long long g_sampleRssKb = -1;
volatile long long g_sampleHwmKb = -1;
volatile long long g_sampleSizeKb = -1;
volatile long long g_sampleThreads = -1;
volatile long long g_sampleMs = 0;
char g_sampleText[192];


// 备用信号栈: 主线程装好, 栈溢出型 SIGSEGV 仍能进处理器
unsigned char g_altStack[kAltSize] __attribute__((aligned(16)));
stack_t g_altStackDesc;   // 文件作用域静态 POD: 确保在构造函数里只是赋值, 不触发 guard 变量

// ------------------------------ 无 libc 的字符串/数字工具 -------------------
// 说明: 下面这些函数全部是纯内存循环, 不用 strlen/strcpy/memcpy/snprintf ——
// 它们在处理器里被调用, 必须只做寄存器+内存操作。

void cgCopyBounded(char* dst, int cap, const char* src)
{
    int i = 0;
    if (cap <= 0) {
        return;
    }
    if (src != nullptr) {
        for (; i < cap - 1 && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

char* cgPutStr(char* p, char* end, const char* s)
{
    if (s == nullptr) {
        return p;
    }
    while (*s != '\0' && p < end) {
        *p++ = *s++;
    }
    return p;
}

char* cgPutCh(char* p, char* end, char c)
{
    if (p < end) {
        *p++ = c;
    }
    return p;
}

char* cgPutDec(char* p, char* end, long long v)
{
    char tmp[24];
    int n = 0;
    unsigned long long u;
    if (v < 0) {
        p = cgPutCh(p, end, '-');
        u = (unsigned long long)(-(v + 1)) + 1ULL;   // 避免 -LLONG_MIN 溢出
    } else {
        u = (unsigned long long)v;
    }
    do {
        tmp[n++] = (char)('0' + (int)(u % 10ULL));
        u /= 10ULL;
    } while (u != 0ULL && n < (int)sizeof(tmp));
    while (n > 0) {
        p = cgPutCh(p, end, tmp[--n]);
    }
    return p;
}

char* cgPut2(char* p, char* end, int v)
{
    p = cgPutCh(p, end, (char)('0' + (v / 10) % 10));
    return cgPutCh(p, end, (char)('0' + v % 10));
}

char* cgPut3(char* p, char* end, int v)
{
    p = cgPutCh(p, end, (char)('0' + (v / 100) % 10));
    p = cgPutCh(p, end, (char)('0' + (v / 10) % 10));
    return cgPutCh(p, end, (char)('0' + v % 10));
}

char* cgPutHex(char* p, char* end, unsigned long long v)
{
    static const char kHex[] = "0123456789abcdef";
    bool lead = true;
    p = cgPutStr(p, end, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        unsigned int d = (unsigned int)((v >> (unsigned)i) & 0xFULL);
        if (lead && d == 0U && i != 0) {
            continue;
        }
        lead = false;
        p = cgPutCh(p, end, kHex[d]);
    }
    return p;
}

// Howard Hinnant 的 civil_from_days(全整数, 无 libc 时间函数)
void cgCivilFromDays(long long z, long long* year, int* month, int* day)
{
    z += 719468LL;
    long long era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    unsigned long long doe = (unsigned long long)(z - era * 146097LL);
    unsigned long long yoe = (doe - doe / 1460ULL + doe / 36524ULL - doe / 146096ULL) / 365ULL;
    long long y = (long long)yoe + era * 400LL;
    unsigned long long doy = doe - (365ULL * yoe + yoe / 4ULL - yoe / 100ULL);
    unsigned long long mp = (5ULL * doy + 2ULL) / 153ULL;
    unsigned long long d = doy - (153ULL * mp + 2ULL) / 5ULL + 1ULL;
    unsigned long long m = mp + (mp < 10ULL ? 3ULL : (unsigned long long)-9);
    *year = y + (m <= 2ULL ? 1LL : 0LL);
    *month = (int)m;
    *day = (int)d;
}

// 统一按 UTC 输出(处理器里不能碰 tzset/localtime: 它们会加锁读环境)
char* cgPutIso(char* p, char* end, long long ms)
{
    if (ms < 0) {
        return cgPutStr(p, end, "(time-unavailable)");
    }
    long long sec = ms / 1000LL;
    int milli = (int)(ms % 1000LL);
    long long days = sec / 86400LL;
    long long rem = sec % 86400LL;
    long long y = 1970;
    int mo = 1;
    int d = 1;
    cgCivilFromDays(days, &y, &mo, &d);
    p = cgPutDec(p, end, y);
    p = cgPutCh(p, end, '-');
    p = cgPut2(p, end, mo);
    p = cgPutCh(p, end, '-');
    p = cgPut2(p, end, d);
    p = cgPutCh(p, end, 'T');
    p = cgPut2(p, end, (int)(rem / 3600LL));
    p = cgPutCh(p, end, ':');
    p = cgPut2(p, end, (int)((rem / 60LL) % 60LL));
    p = cgPutCh(p, end, ':');
    p = cgPut2(p, end, (int)(rem % 60LL));
    p = cgPutCh(p, end, '.');
    p = cgPut3(p, end, milli);
    return cgPutCh(p, end, 'Z');
}

long long cgNowMs()
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    // clock_gettime 在 POSIX 异步信号安全列表里, musl 实现无锁(走 vDSO 或裸 syscall)
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
    }
    time_t t = time(nullptr);   // time() 也在安全列表里
    return (long long)t * 1000LL;
}

// ------------------------------ 本模块地址范围(模块归属判定) ---------------
// 崩溃时最要紧的一句话是“故障地址是不是落在我们自己的 .so 里”。
// 这件事在信号处理器里不能用 dl_iterate_phdr / dladdr(可能碰动态链接器的锁),
// 也不能现场读 /proc/self/maps(多次 read + 解析), 所以:
//   安装/初始化阶段(普通上下文)读一次 /proc/self/maps, 把“包含本模块某个已知符号地址”
//   的那一段映射 [base,end) 记下来; 处理器里只做两次整数比较, 零系统调用。
unsigned long long g_selfBase = 0;       // 装载基址 = 该映射 start - 文件偏移(可用 nm 的 vaddr 直接相加)
unsigned long long g_selfTextStart = 0;  // 本模块“可执行段”起点(pc 一定落在这里面)
unsigned long long g_selfTextEnd  = 0;   // 本模块“可执行段”终点
char g_selfName[64];

int cgHexVal(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

// 解析一行 /proc/self/maps: "start-end perms offset dev inode path"
// 只关心“addr 是否落在这一行里”; 命中就把 [start,end) 与模块名(取 basename)记下。
bool cgParseMapLine(const char* line, unsigned long long addr)
{
    unsigned long long start = 0;
    unsigned long long end = 0;
    int i = 0;
    int digits = 0;
    for (; line[i] != '\0'; ++i) {
        int v = cgHexVal(line[i]);
        if (v < 0) {
            break;
        }
        start = (start << 4) | (unsigned long long)v;
        ++digits;
    }
    if (digits == 0 || line[i] != '-') {
        return false;
    }
    ++i;
    digits = 0;
    for (; line[i] != '\0'; ++i) {
        int v = cgHexVal(line[i]);
        if (v < 0) {
            break;
        }
        end = (end << 4) | (unsigned long long)v;
        ++digits;
    }
    if (digits == 0 || addr < start || addr >= end) {
        return false;
    }
    // 第三个字段是文件偏移(hex): 装载基址 = start - offset, nm 的 vaddr 加它就得到运行时地址
    while (line[i] == ' ' || line[i] == '\t') { ++i; }
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') { ++i; }  // 跳过 perms
    while (line[i] == ' ' || line[i] == '\t') { ++i; }
    unsigned long long off = 0;
    int odig = 0;
    for (; line[i] != '\0'; ++i) {
        int v = cgHexVal(line[i]);
        if (v < 0) { break; }
        off = (off << 4) | (unsigned long long)v;
        ++odig;
    }
    (void)odig;
    // 路径: 跳过 "dev inode" 后剩下的部分
    const char* path = nullptr;
    for (int k = i; line[k] != '\0'; ++k) {
        if (line[k] == '/') {
            path = line + k;
            break;
        }
    }
    g_selfBase = start - off;
    g_selfTextStart = start;
    g_selfTextEnd = end;
    if (path != nullptr) {
        const char* base = path;
        for (const char* q = path; *q != '\0'; ++q) {
            if (*q == '/') {
                base = q + 1;
            }
        }
        cgCopyBounded(g_selfName, (int)sizeof(g_selfName), base);
    } else {
        cgCopyBounded(g_selfName, (int)sizeof(g_selfName), "(无路径/匿名映射)");
    }
    return true;
}

// ---- 所有“可执行映射”的区间表 + 名称(给原始栈扫描用) ------------------------
// 纯 C 编译单元在这个 target 上默认没有 .eh_frame(_Unwind_Backtrace 走到 C 帧就断),
// 所以除了展开器, 还需要一把“不依赖任何展开信息”的兜底: 把栈上每个 8 字节槽位
// 与这张表比对, 落在某个模块的可执行段里就说明它很可能是一个返回地址。
// 表在普通上下文(构造函数 / setLogDir)刷新, 处理器只做整数比较。
constexpr int kMaxExec = 48;
struct ExecRange {
    unsigned long long start;
    unsigned long long end;
    char name[40];
};
ExecRange g_execRanges[kMaxExec];
volatile sig_atomic_t g_execCount = 0;
volatile sig_atomic_t g_execSeq = 0;   // 奇=正在刷新

// ---- 预先解析好的「模块表」(2026-10-12 追加, 见文件头  段) -----------------
// 真机现场那一栏空着的根因: 旧代码只记「自己」的代码段, pc 落在别的模块里就只剩一句话
// "崩在系统库/运行时/其它 .so"。这张表把每一个可执行段连同路径/文件偏移/装载基址都
// 预先解析好(普通上下文), 处理器里只做纯内存线性扫描 —— 零系统调用, 不碰动态链接器的锁。
//   * g_mods[i].base = start - file_offset = 模块装载基址, 于是 addr - base 就是 nm 里的 vaddr,
//     可以直接 llvm-addr2line/llvm-nm 反查符号(记录里写出来的 +0x… 就是这个数);
//   * g_modDropped 是"表满了之后还有多少个可执行段没记"(真机上一眼能看出表够不够用);
//   * g_modSeq 奇偶 = 处理器判断这一轮表是不是正在被刷新(奇数就跳过, 用空表并写未命中)。
auroracg::ModRange g_mods[auroracg::kMaxMods];
volatile sig_atomic_t g_modCount = 0;
volatile sig_atomic_t g_modDropped = 0;
volatile sig_atomic_t g_modSeq = 0;
volatile sig_atomic_t g_modRefreshes = 0;
volatile long long    g_modRefreshMs = 0;

// 崩溃时"穷人的调用栈"的扫描窗口: 从崩溃线程的 sp 往上 2048 字节(与卡死取证的原始栈窗口同宽)。
// 栈向下增长, sp 以上必定还在同一个已映射的栈区间里(除非 sp 本身不可信 —— 那一种我们不扫)。
constexpr unsigned int kCrashStackWindow = 2048;

// 普通上下文调用(构造函数 / setLogDir), 处理器里不调用。
void cgResolveSelfRange()
{
    if (g_selfBase != 0) {
        return;
    }
    const unsigned long long self = (unsigned long long)(uintptr_t)(void*)&auroraSetCurrentItem;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    char line[512];
    int len = 0;
    char buf[8192];
    bool found = false;
    while (!found) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        for (ssize_t i = 0; i < n && !found; ++i) {
            if (buf[i] == '\n') {
                line[len] = '\0';
                found = cgParseMapLine(line, self);
                len = 0;
            } else if (len < (int)sizeof(line) - 1) {
                line[len++] = buf[i];
            }
        }
    }
    close(fd);
}

// 刷新两张表(普通上下文: 构造函数 / setLogDir / 每个标记点节流调用; 不在处理器里调用)。
//   ① 旧的可执行区间表(g_execRanges, 裸文件名): 挂起取证的原始栈扫描(cgHangWriteRawStack)
//      仍然用它 —— 那边一行输出都不动, 免得历史 native_hang.txt 的读法失效;
//   ② 新的「模块表」(g_mods, 全路径 + 文件偏移 + 装载基址): 崩溃取证用它把 pc/lr/栈上候选
//      定位到具体哪个 .so。
// 两张表在同一次 read 循环里填, 只读一遍 /proc/self/maps; 解析用的是
// auroracg::mmParseExecLine —— 也就是离线断言脚本(verify_crash_modmap.py)逐字节断言过的那一个。
void cgRefreshExecRanges()
{
    const int s = (int)g_execSeq;
    const int m = (int)g_modSeq;
    g_execSeq = (sig_atomic_t)(s + 1);   // 变奇数: 处理器会跳过这一轮比对
    g_modSeq = (sig_atomic_t)(m + 1);    // 模块表同理
    g_execCount = 0;
    // 读失败时不要留下半张表: 先把旧表收起来, 失败就原样恢复(处理器在刷新窗口内一律按空表处理,
    // 见 g_modSeq 奇偶)。这样一次失败的刷新不会把上一张好表抹掉。
    const int oldModCount = (int)g_modCount;
    const int oldModDropped = (int)g_modDropped;
    g_modCount = 0;
    g_modDropped = 0;

    auroracg::ModFill fill;
    auroracg::mmFillBegin(&fill, g_mods, auroracg::kMaxMods);

    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char line[512];
        int len = 0;
        char buf[8192];
        int count = 0;
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    line[len] = '\0';
                    auroracg::mmFillLine(&fill, line);   // ① 模块表(满了就只计数: "还有 N 段没记")
                    auroracg::ModRange tmp;
                    if (count < kMaxExec && auroracg::mmParseExecLine(line, &tmp)) {
                        g_execRanges[count].start = tmp.start;   // ② 旧区间表(裸文件名)
                        g_execRanges[count].end = tmp.end;
                        const char* base = tmp.path;
                        for (const char* q = tmp.path; *q != '\0'; ++q) {
                            if (*q == '/') {
                                base = q + 1;
                            }
                        }
                        cgCopyBounded(g_execRanges[count].name, 40, base);
                        ++count;
                    }
                    len = 0;
                } else if (len < (int)sizeof(line) - 1) {
                    line[len++] = buf[i];
                }
            }
        }
        close(fd);
        g_execCount = (sig_atomic_t)count;
        g_modCount = (sig_atomic_t)fill.count;
        g_modDropped = (sig_atomic_t)fill.dropped;
    } else {
        g_modCount = (sig_atomic_t)oldModCount;       // 读不到 maps: 保留上一张表
        g_modDropped = (sig_atomic_t)oldModDropped;
    }
    g_modRefreshMs = cgNowMs();
    ++g_modRefreshes;
    g_modSeq = (sig_atomic_t)(m + 2);    // 变偶数: 表稳定
    g_execSeq = (sig_atomic_t)(s + 2);   // 变偶数: 表稳定
}

// 旁路节流: 库是逐步 dlopen 的(requireNapi 各模块、NNRt、GPU 渲染库…), 装处理器那一刻
// 表里只有当时已加载的库。于是每个标记点顺手刷一次, 但至少隔 1 秒 —— 读一次 maps 是几十到
// 一百多 KB 的纯内存读, 而且发生在负载开跑之前, 不在任何计时窗口里, 不影响分数。
void cgMaybeRefreshModTable()
{
    const long long now = cgNowMs();
    if (g_modRefreshMs > 0 && (now - g_modRefreshMs) < 1000) {
        return;
    }
    cgRefreshExecRanges();
}

// ------------------------------ 信号号/ si_code 文本 ------------------------
const char* cgSigName(int sig)
{
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGSYS:  return "SIGSYS";
        default:      return "SIG?";
    }
}

const char* cgCodeText(int sig, int code)
{
    switch (sig) {
        case SIGSEGV:
            if (code == SEGV_MAPERR) { return "SEGV_MAPERR(地址未映射=野指针/越界)"; }
            if (code == SEGV_ACCERR) { return "SEGV_ACCERR(页无权限=写只读/栈溢出)"; }
            break;
        case SIGBUS:
            if (code == BUS_ADRALN) { return "BUS_ADRALN(未对齐访问)"; }
            if (code == BUS_ADRERR) { return "BUS_ADRERR(物理地址不存在)"; }
            if (code == BUS_OBJERR) { return "BUS_OBJERR(对象错误)"; }
            break;
        case SIGFPE:
            if (code == FPE_INTDIV) { return "FPE_INTDIV(整数除零)"; }
            if (code == FPE_INTOVF) { return "FPE_INTOVF(整数溢出)"; }
            if (code == FPE_FLTDIV) { return "FPE_FLTDIV(浮点除零)"; }
            if (code == FPE_FLTOVF) { return "FPE_FLTOVF(浮点上溢)"; }
            if (code == FPE_FLTUND) { return "FPE_FLTUND(浮点下溢)"; }
            if (code == FPE_FLTRES) { return "FPE_FLTRES(结果不精确)"; }
            if (code == FPE_FLTINV) { return "FPE_FLTINV(非法浮点操作)"; }
            if (code == FPE_FLTSUB) { return "FPE_FLTSUB(下标越界)"; }
            break;
        case SIGILL:
            if (code == ILL_ILLOPC) { return "ILL_ILLOPC(非法指令)"; }
            if (code == ILL_ILLOPN) { return "ILL_ILLOPN(非法操作数)"; }
            if (code == ILL_ILLADR) { return "ILL_ILLADR(非法寻址模式)"; }
            if (code == ILL_ILLTRP) { return "ILL_ILLTRP(非法陷阱)"; }
            if (code == ILL_PRVOPC) { return "ILL_PRVOPC(特权指令)"; }
            if (code == ILL_PRVREG) { return "ILL_PRVREG(特权寄存器)"; }
            if (code == ILL_COPROC) { return "ILL_COPROC(协处理器不可用)"; }
            if (code == ILL_BADSTK) { return "ILL_BADSTK(栈损坏)"; }
            break;
        case SIGABRT:
            return "SIGABRT(abort()/assert/std::terminate, 见下 libc 足迹)";
        case SIGSYS:
            return "SIGSYS(被 seccomp/系统策略拦截的系统调用)";
        default:
            break;
    }
    if (code == SI_USER)  { return "SI_USER(kill 发出)"; }
    if (code == SI_TKILL) { return "SI_TKILL(tgkill/raise 发出)"; }
    if (code == SI_QUEUE) { return "SI_QUEUE(sigqueue 发出)"; }
    return "(未知 si_code)";
}

// ------------------------------ /proc/self/status 快照 ----------------------
bool cgChrEq(const char* a, const char* b, int n)
{
    for (int i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == '\0') {
            return false;
        }
    }
    return true;
}

// 在给定缓冲里找行首的 "Key:" 之后的值, 返回数值(失败置 *ok=0)。
// 纯内存操作, 处理器里可安全调用。
long long cgProcFieldEx(const char* buf, const char* key, int keyLen, char* unit, int unitCap, int* ok)
{
    if (ok != nullptr) {
        *ok = 0;
    }
    for (int i = 0; i + keyLen < kProcCap && buf[i] != '\0'; ++i) {
        // 只在行首匹配, 避免 "VmPeak" 被 "VmRSS" 之类的子串误配
        if (i != 0 && buf[i - 1] != '\n') {
            continue;
        }
        if (!cgChrEq(buf + i, key, keyLen)) {
            continue;
        }
        int j = i + keyLen;
        while (j < kProcCap && (buf[j] == ' ' || buf[j] == '\t')) {
            ++j;
        }
        long long v = 0;
        bool any = false;
        while (j < kProcCap && buf[j] >= '0' && buf[j] <= '9') {
            v = v * 10LL + (long long)(buf[j] - '0');
            any = true;
            ++j;
        }
        if (!any) {
            continue;
        }
        while (j < kProcCap && (buf[j] == ' ' || buf[j] == '\t')) {
            ++j;
        }
        int u = 0;
        while (unit != nullptr && u < unitCap - 1 && j < kProcCap && buf[j] != '\n' &&
               buf[j] != '\0' && buf[j] != ' ') {
            unit[u++] = buf[j++];
        }
        if (unit != nullptr) {
            unit[u] = '\0';
        }
        if (ok != nullptr) {
            *ok = 1;
        }
        return v;
    }
    return 0;
}

// 崩溃处理器用: 读 g_procBuf(/proc/self/status 快照)
long long cgProcField(const char* key, int keyLen, char* unit, int unitCap, int* ok)
{
    return cgProcFieldEx(g_procBuf, key, keyLen, unit, unitCap, ok);
}

#if AURORA_CRASH_MEM_SNAPSHOT
// 普通上下文用: 从 /proc/self/status 采样一次内存足迹, 结果留着给 ArkTS 取走
int cgReadProcInto(char* dst)
{
    int fd = (int)g_procFd;
    if (fd < 0) {
        return 0;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return 0;
    }
    int total = 0;
    while (total < kProcCap - 1) {
        ssize_t n = read(fd, dst + total, (size_t)(kProcCap - 1 - total));
        if (n > 0) {
            total += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    dst[total] = '\0';
    return total;
}
#endif

// 读预先 open 好的 /proc/self/status。lseek+read 都在 POSIX 安全列表里, 且 fd 已就绪。
void cgSnapshotMemory()
{
#if AURORA_CRASH_MEM_SNAPSHOT
    int fd = (int)g_procFd;
    if (fd < 0) {
        g_procBuf[0] = '\0';
        return;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        g_procBuf[0] = '\0';
        return;
    }
    int total = 0;
    while (total < kProcCap - 1) {
        ssize_t n = read(fd, g_procBuf + total, (size_t)(kProcCap - 1 - total));
        if (n > 0) {
            total += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    g_procBuf[total] = '\0';
#endif
}

// ------------------------------ 记录与转发 ----------------------------------
void cgWriteAll(int fd, const char* buf, int len)
{
    int off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, (size_t)(len - off));
        if (n > 0) {
            off += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

// ===========================================================================
//  逐项地址空间归因(旁路, 不计分; 见文件头 (B))
//
//  真机现场: memory 行 VmSize=14034764 kB(14 GB)、VmPeak=14568144 kB, 而 VmRSS 只有
//  202808 kB(RSS 峰值 451640 kB)。14 GB 虚拟地址空间远大于平板 11.5 GB 的物理内存, 但记录里
//  没有任何东西能说明是哪一项撑起来的。这里按项记一笔账:
//    * 采样点 = 既有的 item 标记点(auroraSetCurrentItem / auroraSetIdleMarker), 每项一次;
//    * 每一项的增量 = 它结束时的采样 - 它开始时的采样(= 相邻两个标记点之差)。注意标记点是
//      "开跑前"打的, 所以下一项开始的那一枪就是上一项结束时的那一枪, 中间只隔着两项之间的
//      胶水代码(几十毫秒), 归因时把它算进上一项;
//    * 判据写死(不许调): 单项 ΔVmSize > 512 MB 或 ΔVmRSS > 256 MB => 该行标
//      "本项占用异常, 见该项明细";
//    * 逐行 O_APPEND 到 <filesDir>/native_memory.txt —— 进程被 SIGKILL(本工程真机上最可能的
//      消失方式: 没有任何崩溃记录)时, 这份逐项账目仍然留在盘上;
//    * 崩溃记录里再整表打一遍(带"增量最大的一项"), 这样一条 native_crash.txt 就能自洽。
//  旁路: 读不到就写"(读不到)"; 任何一步失败都不改变负载行为与分数。
// ===========================================================================
constexpr int kMaxItemMem   = 160;                               // 逐项表上限(一次完整跑分几十项)
constexpr long long kMaxMemFileBytes = 96 * 1024;                // native_memory.txt 总量闸门
constexpr int kItemMemRecordRows = 64;                           // 崩溃记录里最多打这么多行

// 行结构 / 判据(512MB / 256MB) / 逐行与整表渲染全部在 crash_modmap.h 里 —— 那是纯逻辑,
// 由 verify_crash_modmap.py 用合成数据逐字节断言过; 这里只负责采样与状态机(要读 /proc)。
using ItemMemRow = auroracg::ItemMemRow;
static_assert(kPhaseCap == auroracg::kItemPhaseCap, "阶段名上限必须与 crash_modmap.h 一致");
static_assert(kItemCap == auroracg::kItemNameCap, "负载名上限必须与 crash_modmap.h 一致");

ItemMemRow g_itemRows[kMaxItemMem];
volatile sig_atomic_t g_itemRowCount = 0;   // 已开出的行数(含正在跑的那一行)
volatile sig_atomic_t g_itemDropped = 0;    // 表满之后没记的项数
volatile sig_atomic_t g_itemSeq = 0;        // 奇=正在改表(处理器遇到奇数会说明这一行可能不完整)
char g_memPath[kPathCap + 24];              // "<filesDir>/native_memory.txt"
volatile sig_atomic_t g_memFd = -1;
long long g_memBytes = 0;
bool g_memCapped = false;

using ItemMemSample = auroracg::ItemMemSample;   // 采样值的结构在头文件里(与判据/渲染放一起)

// 逐项归因专用的 status 缓冲与读函数: 用独立的 fd(g_procFd2)和独立缓冲, 因为
// /proc/self/status 的读取会推进文件偏移 —— 负载线程(标记点)与 JS 线程(ArkTS 的
// sampleMemory())可能同时读, 共用 fd 会互相把偏移挪走。
char g_itemProcBuf[kProcCap];

int cgReadItemProcInto()
{
    const int fd = (int)g_procFd2;
    if (fd < 0) {
        return 0;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return 0;
    }
    int total = 0;
    while (total < kProcCap - 1) {
        const ssize_t n = read(fd, g_itemProcBuf + total, (size_t)(kProcCap - 1 - total));
        if (n > 0) {
            total += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    g_itemProcBuf[total] = '\0';
    return total;
}

// 普通上下文: 读一次 /proc/self/status 做逐项归因(专用 fd + 专用缓冲)
ItemMemSample cgSampleStatusNow()
{
    ItemMemSample s;
    s.sizeKb = -1;
    s.rssKb = -1;
    s.hwmKb = -1;
    s.threads = -1;
    s.ok = false;
#if AURORA_CRASH_MEM_SNAPSHOT
    if (cgReadItemProcInto() <= 0) {
        return s;
    }
    char u[8];
    int ok = 0;
    s.sizeKb = cgProcFieldEx(g_itemProcBuf, "VmSize:", 7, u, (int)sizeof(u), &ok);
    if (ok == 0) { s.sizeKb = -1; }
    s.rssKb = cgProcFieldEx(g_itemProcBuf, "VmRSS:", 6, u, (int)sizeof(u), &ok);
    if (ok == 0) { s.rssKb = -1; }
    s.hwmKb = cgProcFieldEx(g_itemProcBuf, "VmHWM:", 6, u, (int)sizeof(u), &ok);
    if (ok == 0) { s.hwmKb = -1; }
    s.threads = cgProcFieldEx(g_itemProcBuf, "Threads:", 8, u, (int)sizeof(u), &ok);
    if (ok == 0) { s.threads = -1; }
    s.ok = (s.sizeKb >= 0 || s.rssKb >= 0);
#endif
    return s;
}

// (逐行渲染已上移到 crash_modmap.h 的 auroracg::mmItemRowLine —— 同一份代码由离线脚本
//  verify_crash_modmap.py 逐字节断言, 见那里的 E 段。)

// 追加一行到 <filesDir>/native_memory.txt(旁路; 总量闸门与卡死文件同思路)
void cgItemMemWriteFile(const ItemMemRow* r, int ordinal)
{
    const int fd = (int)g_memFd;
    if (fd < 0) {
        return;
    }
    char line[512];
    char* p = line;
    char* end = line + sizeof(line) - 2;
    p = auroracg::mmItemRowLine(p, end, ordinal, r, false);
    p = cgPutStr(p, end, " ended=");
    p = cgPutIso(p, end, r->endMs);
    p = cgPutCh(p, end, '\n');
    const int len = (int)(p - line);
    if (g_memBytes >= kMaxMemFileBytes) {
        if (!g_memCapped) {
            g_memCapped = true;
            static const char kNote[] =
                "# [已达 native_memory.txt 上限 96 KB: 后续逐项账目不再写入(崩溃记录里仍会整表打一遍)]\n";
            cgWriteAll(fd, kNote, (int)sizeof(kNote) - 1);
        }
        return;
    }
    g_memBytes += len;
    cgWriteAll(fd, line, len);
}

// 结算"正在跑的那一项": 结束值 = 现在这一枪, 增量 = 现在 - 它开始时。
// 返回被结算的行下标(-1 = 没有正在跑的行, 例如刚做完空闲结算又来一个标记点) ——
// 只有真的结算了一行才允许往 native_memory.txt 追加, 免得同一行写两遍。
int cgItemMemCloseOpen(const ItemMemSample& s, long long now)
{
    // 真正干活的纯函数在 crash_modmap.h(mmItemMemCloseOpen): 幂等 + 判据写死, 离线脚本断言过
    return auroracg::mmItemMemCloseOpen(g_itemRows, (int)g_itemRowCount, s, now);
}

// 一个标记点 = 一次采样: 先给上一项结算(它的结束值就是现在这一枪), 再开一项新的
void cgItemMemOnMarker(const char* phase, const char* item, int index, int total)
{
    const ItemMemSample s = cgSampleStatusNow();
    const long long now = cgNowMs();

    int rowCount = (int)g_itemRowCount;
    int dropped = (int)g_itemDropped;
    const int seq = (int)g_itemSeq;
    g_itemSeq = (sig_atomic_t)(seq + 1);        // 奇数: 表正在改(处理器会据此说明"最后一行可能不完整")
    const int closedIdx = cgItemMemCloseOpen(s, now);
    (void)auroracg::mmItemMemOpen(g_itemRows, &rowCount, kMaxItemMem, &dropped,
                                  phase, item, index, total, s);
    g_itemRowCount = (sig_atomic_t)rowCount;
    g_itemDropped = (sig_atomic_t)dropped;
    g_itemSeq = (sig_atomic_t)(seq + 2);        // 偶数: 表稳定

    if (closedIdx >= 0) {                       // 只有真的结算了一行才落盘(不会写两遍)
        cgItemMemWriteFile(&g_itemRows[closedIdx], closedIdx + 1);
    }
}

// 空闲标记(上一项已结束): 只结算, 不新开项
void cgItemMemOnIdle(void)
{
    const ItemMemSample s = cgSampleStatusNow();
    const long long now = cgNowMs();
    int seq = (int)g_itemSeq;
    g_itemSeq = (sig_atomic_t)(seq + 1);
    const int closedIdx = cgItemMemCloseOpen(s, now);
    g_itemSeq = (sig_atomic_t)(seq + 2);
    if (closedIdx >= 0) {
        cgItemMemWriteFile(&g_itemRows[closedIdx], closedIdx + 1);
    }
}

// (增量最大的一项的选择已上移到 crash_modmap.h 的 auroracg::mmItemMemWorst —— 同一份代码由
//  离线脚本断言, 见 verify_crash_modmap.py 的 E 段。)

// 整张逐项账目表(崩溃记录与 C 接口共用同一份渲染; 渲染本身在 crash_modmap.h, 离线可断言)
char* cgItemMemPutReport(char* p, char* end, int maxRows)
{
    return auroracg::mmItemMemReport(p, end, g_itemRows, (int)g_itemRowCount, (int)g_itemDropped,
                                     maxRows, g_memPath, kMaxItemMem, ((int)g_itemSeq & 1) != 0);
}

// 转发给“安装我们之前”的原处理器(链式), 然后干净退出。
// 若原处理器是 SIG_DFL/SIG_IGN 或为空, 直接 _exit(128+sig)。
__attribute__((noreturn)) void cgForwardAndExit(int sig, siginfo_t* info, void* uctx)
{
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].signo != sig) {
            continue;
        }
        const struct sigaction& prev = g_slots[i].prev;
        // 注意: musl 用宏把 sa_handler/sa_sigaction 展开成 union 成员, 直接写成员名会被宏二次展开,
        // 所以这里只用 prev.sa_handler / prev.sa_sigaction 两个宏名。
        // 0 = SIG_DFL, 1 = SIG_IGN(见 musl signal.h), 这两者都不用转发。
        if ((prev.sa_flags & SA_SIGINFO) != 0) {
            void (*sa)(int, siginfo_t*, void*) = prev.sa_sigaction;
            if (sa != nullptr && (uintptr_t)sa > 1U) {
                sa(sig, info, uctx);
            }
        } else {
            void (*sh)(int) = prev.sa_handler;
            if (sh != nullptr && (uintptr_t)sh > 1U) {
                sh(sig);
            }
        }
        break;
    }
    // 原处理器返回了(或本来就是默认动作): 自己干净退出, 不 return 回故障点。
    _exit(128 + sig);
}

// ---- 穷人的调用栈(第二段写盘; 见文件头 (A)) --------------------------------
// 扫一遍崩溃线程栈上的 8 字节字, 凡是落在模块表任何可执行段里的值, 都当作候选返回地址
// 打出来(前 12 个)。不需要 .eh_frame、不需要展开器、不碰动态链接器 —— 纯内存扫描,
// async-signal-safe; 这是"没有 unwind 信息也能指出调用链方向"的兜底。
// 它是第二段写盘: 主记录(含 pc/lr 归属 + 逐项归因)先落盘, 万一读栈踩到没映射的页,
// 已经落盘的现场也还在 —— 盘上"没有 END 横幅"本身就是这一段的证据。
// 说明写清楚: 这些是候选, 不是精确回溯(栈上的字里混着指针/整数, 只是方向性证据)。
void cgWriteStackCandidates(int fd, unsigned long long sp, int nMod)
{
    char buf[4096];
    char* p = buf;
    char* end = buf + sizeof(buf) - 2;
    p = cgPutStr(p, end, "stack  : 穷人的调用栈(粗扫崩溃线程栈上的 8 字节槽位, 落在已记录可执行段里的值"
                         "当成候选返回地址; 只是候选, 不是精确回溯, 不依赖 .eh_frame/展开器)\n");
    if (sp == 0 || (sp & 7ULL) != 0 || sp < 0x1000ULL) {
        p = cgPutStr(p, end, "         sp 不可信(");
        p = cgPutHex(p, end, sp);
        p = cgPutStr(p, end, "), 跳过栈扫描(其余字段不受影响)\n");
        cgWriteAll(fd, buf, (int)(p - buf));
        return;
    }
    const unsigned int len = kCrashStackWindow;
    const unsigned char* src = (const unsigned char*)(uintptr_t)sp;
    unsigned long long addrs[12];
    unsigned int slots[12];
    int stored = 0;
    const int found = auroracg::mmScanStack(src, len, g_mods, nMod, addrs, slots, 12, &stored);
    if (stored <= 0) {
        p = cgPutStr(p, end, "         栈上候选返回地址: (无 —— 这次栈窗口里没有任何值落在已记录的可执行段内)");
        if (nMod <= 0) {
            p = cgPutStr(p, end, " [模块表为空]");
        }
        p = cgPutCh(p, end, '\n');
    } else {
        for (int i = 0; i < stored; ++i) {
            if ((end - p) < 420) {          // 一行最长约 400 字节: 放不下就先落盘
                cgWriteAll(fd, buf, (int)(p - buf));
                p = buf;
            }
            char line[384];
            auroracg::mmFormatStackCandidate(line, (int)sizeof(line), addrs[i], slots[i], g_mods, nMod);
            p = cgPutStr(p, end, line);
            p = cgPutCh(p, end, '\n');
        }
    }
    p = cgPutStr(p, end, "         共 ");
    p = cgPutDec(p, end, (long long)found);
    p = cgPutStr(p, end, " 个候选(只列前 12 个, 本次列出 ");
    p = cgPutDec(p, end, (long long)stored);
    p = cgPutStr(p, end, " 个; 窗口 = sp 起 ");
    p = cgPutDec(p, end, (long long)len);
    p = cgPutStr(p, end, " 字节). 对照看看: 命中 libaurorabench.so 的候选 = 我们的调用链;"
                         " 命中 libc++_shared/libc 的候选 = 标准库内部(常见于堆被写坏后的 free)\n");
    cgWriteAll(fd, buf, (int)(p - buf));
}

void cgHandleFatal(int sig, siginfo_t* info, void* uctx)
{
    if (g_inHandler != 0) {
        // 已经有线程在写证据(或原处理器正在跑): 不再写第二遍, 直接转发。
        cgForwardAndExit(sig, info, uctx);
    }
    g_inHandler = 1;

    // 记录缓冲是静态的(见 kCrashRecCap 的理由): 逐项归因表可能上万字节, 而备用信号栈只有
    // 64 KB —— 处理器本身必须保持"小栈足迹", 否则栈溢出型 SIGSEGV 连第一行都写不出来。
    char* rec = g_crashRec;
    char* p = rec;
    char* end = rec + (int)sizeof(g_crashRec) - 2;   // 预留结尾换行与余量

    // 故障现场的寄存器先取出来(后面 pc/lr 归属、栈扫描都要用)
    unsigned long long pcv = 0;
    unsigned long long lrv = 0;
    unsigned long long spv = 0;
#if defined(__aarch64__)
    if (uctx != nullptr) {
        const ucontext_t* ucv = (const ucontext_t*)uctx;
        pcv = (unsigned long long)ucv->uc_mcontext.pc;
        lrv = (unsigned long long)ucv->uc_mcontext.regs[30];
        spv = (unsigned long long)ucv->uc_mcontext.sp;
    }
#endif

    int code = (info != nullptr) ? info->si_code : -1;
    unsigned long long addr = 0;
    bool addrValid = false;
    if (info != nullptr && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE)) {
        addr = (unsigned long long)(uintptr_t)info->si_addr;
        addrValid = true;
    }

    long long now = cgNowMs();

    p = cgPutStr(p, end, "----- AURORA NATIVE CRASH -----\n");
    p = cgPutStr(p, end, "when   : ");
    p = cgPutIso(p, end, now);
    p = cgPutStr(p, end, "  epoch_ms=");
    p = cgPutDec(p, end, now);
    p = cgPutCh(p, end, '\n');

    p = cgPutStr(p, end, "signal : ");
    p = cgPutDec(p, end, sig);
    p = cgPutCh(p, end, ' ');
    p = cgPutStr(p, end, cgSigName(sig));
    p = cgPutStr(p, end, "  si_code=");
    p = cgPutDec(p, end, code);
    p = cgPutCh(p, end, ' ');
    p = cgPutStr(p, end, cgCodeText(sig, code));
    if (addrValid) {
        p = cgPutStr(p, end, "  si_addr=");
        p = cgPutHex(p, end, addr);
    } else {
        p = cgPutStr(p, end, "  si_addr=(该信号无故障地址)");
    }
    p = cgPutCh(p, end, '\n');

    {
        long long pid = (long long)getpid();
        long long tid = (long long)gettid();
        p = cgPutStr(p, end, "thread : pid=");
        p = cgPutDec(p, end, pid);
        p = cgPutStr(p, end, " tid=");
        p = cgPutDec(p, end, tid);
        p = cgPutStr(p, end, (tid == pid) ? " (主线程/JS 线程)" : " (工作线程: 负载线程崩的)");
        p = cgPutCh(p, end, '\n');
    }

#if defined(__aarch64__)
    if (uctx != nullptr) {
        const ucontext_t* uc = (const ucontext_t*)uctx;
        p = cgPutStr(p, end, "pc/lr  : pc=");
        p = cgPutHex(p, end, pcv);
        p = cgPutStr(p, end, " lr=");
        p = cgPutHex(p, end, lrv);
        p = cgPutStr(p, end, " sp=");
        p = cgPutHex(p, end, spv);
        if (uc->uc_mcontext.fault_address != 0) {
            p = cgPutStr(p, end, " fault_address=");
            p = cgPutHex(p, end, (unsigned long long)uc->uc_mcontext.fault_address);
        }
        p = cgPutStr(p, end, "\n        (用未 strip 的 libaurorabench.so 按 pc/lr 反查符号)");
        p = cgPutCh(p, end, '\n');
    }
#endif

    // 模块归属: 用初始化阶段解析出来的本模块代码段 [start,end) 做两次整数比较
    {
        p = cgPutStr(p, end, "module : ");
        if (g_selfTextEnd > g_selfTextStart && pcv != 0) {
            if (pcv >= g_selfTextStart && pcv < g_selfTextEnd) {
                p = cgPutStr(p, end, "pc 落在 ");
                p = cgPutStr(p, end, g_selfName);
                p = cgPutStr(p, end, " 的代码段内 => 崩在我们自己的 native 代码里");
            } else {
                p = cgPutStr(p, end, "pc 不在 ");
                p = cgPutStr(p, end, g_selfName);
                p = cgPutStr(p, end, " 的代码段内 => 崩在系统库/运行时/其它 .so");
            }
            if (pcv >= g_selfBase) {
                p = cgPutStr(p, end, " [pc-base=");
                p = cgPutHex(p, end, pcv - g_selfBase);
                p = cgPutStr(p, end, " 就是 nm 里的 vaddr, 可直接反查符号]");
            }
            p = cgPutStr(p, end, " [self_base=");
            p = cgPutHex(p, end, g_selfBase);
            p = cgPutStr(p, end, " text=");
            p = cgPutHex(p, end, g_selfTextStart);
            p = cgPutCh(p, end, '-');
            p = cgPutHex(p, end, g_selfTextEnd);
            p = cgPutCh(p, end, ']');
        } else {
            p = cgPutStr(p, end, "(未解析到本模块地址范围, 只能拿 pc/lr 事后反查)");
        }
        p = cgPutCh(p, end, '\n');
    }

    // ---- 预先解析的模块表: pc / lr 到底落在哪个库(纯内存线性扫描, 零系统调用) ----
    // 真机现场那一栏就是缺在这里: 旧记录只写到"崩在系统库/运行时/其它 .so", 而
    // libc++_shared.so(std::string/operator new) / libc.so(malloc/free) / ArkTS 运行时 /
    // libaurorasn.so 指向完全不同的结论。这里把 pc 与 lr 各自查一次表并写出路径 + 偏移,
    // 偏移 = addr - 模块装载基址 = nm/addr2line 里的 vaddr, 可以直接反查符号。
    {
        const int nMod = (((int)g_modSeq & 1) != 0) ? 0 : (int)g_modCount;
        {
            char note[256];
            auroracg::mmFormatTableNote(note, (int)sizeof(note), nMod, auroracg::kMaxMods,
                                        (int)g_modDropped);
            p = cgPutStr(p, end, "modmap : ");
            p = cgPutStr(p, end, note);
            p = cgPutStr(p, end, "  刷新次数=");
            p = cgPutDec(p, end, (long long)g_modRefreshes);
            if (g_modRefreshMs > 0) {
                p = cgPutStr(p, end, " 最近刷新=");
                p = cgPutIso(p, end, g_modRefreshMs);
            }
            if (((int)g_modSeq & 1) != 0) {
                p = cgPutStr(p, end, "  [模块表正在刷新: 本轮按空表处理]");
            }
            p = cgPutCh(p, end, '\n');
        }
        char hit[512];
        auroracg::mmFormatAddrHit(hit, (int)sizeof(hit), "pc", pcv, g_mods, nMod);
        p = cgPutStr(p, end, hit);
        p = cgPutCh(p, end, '\n');
        auroracg::mmFormatAddrHit(hit, (int)sizeof(hit), "lr", lrv, g_mods, nMod);
        p = cgPutStr(p, end, hit);
        p = cgPutCh(p, end, '\n');
        p = cgPutStr(p, end, "        (pc 不在我们的 .so 里 ≠ 不是我们的 bug: 我们的 std::string /"
                             " operator new 住在 libc++_shared.so, 堆被写坏之后最先炸的通常是"
                             " libc.so 里的 malloc/free。上面这两行 + 下一条的栈上候选才够定罪或洗清)\n");
    }

    // ---- 逐项地址空间归因(旁路; 见文件头 (B)) ----
    p = cgItemMemPutReport(p, end, kItemMemRecordRows);

    // 当前项标记(可能正在被更新, 用 seq 前后比对判断是否撕裂)
    {
        int seq0 = (int)g_seq;
        char phase[kPhaseCap];
        char item[kItemCap];
        int idx = 0;
        int tot = 0;
        long long markMs = 0;
        bool torn = (seq0 & 1) != 0;
        cgCopyBounded(phase, kPhaseCap, g_phase);
        cgCopyBounded(item, kItemCap, g_item);
        idx = (int)g_index;
        tot = (int)g_total;
        markMs = g_markMs;
        if ((int)g_seq != seq0) {
            torn = true;
        }

        p = cgPutStr(p, end, "item   : phase=\"");
        p = cgPutStr(p, end, phase);
        p = cgPutStr(p, end, "\" item=\"");
        p = cgPutStr(p, end, item);
        p = cgPutStr(p, end, "\" index=");
        p = cgPutDec(p, end, idx);
        p = cgPutCh(p, end, '/');
        p = cgPutDec(p, end, tot);
        p = cgPutStr(p, end, " marked=");
        if (markMs > 0) {
            p = cgPutIso(p, end, markMs);
        } else {
            p = cgPutStr(p, end, "(未记时间)");
        }
        if (markMs > 0 && now >= markMs) {
            p = cgPutStr(p, end, " age_ms=");
            p = cgPutDec(p, end, now - markMs);
        }
        if (idx == 0 && tot == 0) {
            p = cgPutStr(p, end, "  [标记为“无负载运行中”: 崩在两项之间/胶水代码里]");
        }
        if (torn) {
            p = cgPutStr(p, end, "  [标记正在更新, 名字可能不完整]");
        }
        p = cgPutCh(p, end, '\n');
    }

    // 内存快照
    cgSnapshotMemory();
    {
        char u1[8] = {0};
        char u2[8] = {0};
        char u3[8] = {0};
        char u5[8] = {0};
        int ok1 = 0;
        int ok2 = 0;
        int ok3 = 0;
        int ok5 = 0;
        long long peak = cgProcField("VmPeak:", 7, u1, (int)sizeof(u1), &ok1);
        long long hwm  = cgProcField("VmHWM:", 6, u5, (int)sizeof(u5), &ok5);
        long long rss  = cgProcField("VmRSS:", 6, u2, (int)sizeof(u2), &ok2);
        long long vsz  = cgProcField("VmSize:", 7, u3, (int)sizeof(u3), &ok3);
        if (ok1 != 0 || ok2 != 0 || ok3 != 0 || ok5 != 0) {
            p = cgPutStr(p, end, "memory : VmPeak=");
            p = cgPutDec(p, end, peak);
            p = cgPutCh(p, end, ' ');
            p = cgPutStr(p, end, u1);
            p = cgPutStr(p, end, " VmHWM(RSS峰值)=");
            p = cgPutDec(p, end, hwm);
            p = cgPutCh(p, end, ' ');
            p = cgPutStr(p, end, u5);
            p = cgPutStr(p, end, " VmRSS=");
            p = cgPutDec(p, end, rss);
            p = cgPutCh(p, end, ' ');
            p = cgPutStr(p, end, u2);
            p = cgPutStr(p, end, " VmSize=");
            p = cgPutDec(p, end, vsz);
            p = cgPutCh(p, end, ' ');
            p = cgPutStr(p, end, u3);
        } else {
            p = cgPutStr(p, end, "memory : (无 /proc/self/status 快照)");
        }
        {
            char u4[8] = {0};
            int ok4 = 0;
            long long th = cgProcField("Threads:", 8, u4, (int)sizeof(u4), &ok4);
            if (ok4 != 0) {
                p = cgPutStr(p, end, " Threads=");
                p = cgPutDec(p, end, th);
            }
        }
        p = cgPutCh(p, end, '\n');
    }

    // 项开始前那一次采样(能被 SIGKILL 情形“外带出去”的那口内存足迹)
    if (g_sampleMs > 0) {
        p = cgPutStr(p, end, "at_item: rss=");
        p = cgPutDec(p, end, g_sampleRssKb);
        p = cgPutStr(p, end, " kB hwm=");
        p = cgPutDec(p, end, g_sampleHwmKb);
        p = cgPutStr(p, end, " kB size=");
        p = cgPutDec(p, end, g_sampleSizeKb);
        p = cgPutStr(p, end, " kB threads=");
        p = cgPutDec(p, end, g_sampleThreads);
        p = cgPutStr(p, end, " sampled=");
        p = cgPutIso(p, end, g_sampleMs);
        if (now >= g_sampleMs) {
            p = cgPutStr(p, end, " age_ms=");
            p = cgPutDec(p, end, now - g_sampleMs);
        }
        p = cgPutStr(p, end, "  (ArkTS 调 sampleMemory() 采的, 用于判断是否逼近内存上限)\n");
    }

    p = cgPutStr(p, end, "source : crash_guard@libaurorabench.so (仅记录, 不改负载)\n");

    int fd = (int)g_crashFd;
    if (fd < 0) {
        fd = STDERR_FILENO;   // 还没拿到 filesDir: 至少落在 hilog/stderr 上
    }
    // ① 主记录先落盘: 现场寄存器/信号/标记/内存/模块归属/逐项归因 —— 这一段不碰任何未验证的内存
    cgWriteAll(fd, rec, (int)(p - rec));
    // ② 穷人的调用栈(要读崩溃线程的栈内存: 万一 sp 不在映射里, 上面那段也已经保住了)
    {
        const int nMod = (((int)g_modSeq & 1) != 0) ? 0 : (int)g_modCount;
        cgWriteStackCandidates(fd, spv, nMod);
    }
    // ③ 收尾横幅(记录里没有它就是"取证时自己又崩了一次"的证据)
    {
        static const char kEnd[] = "----- END AURORA NATIVE CRASH -----\n";
        cgWriteAll(fd, kEnd, (int)sizeof(kEnd) - 1);
    }

    cgForwardAndExit(sig, info, uctx);
}

// ------------------------------ 安装 ----------------------------------------
void cgInstallOne(int sig)
{
    if (g_slotCount >= kMaxSigs) {
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = cgHandleFatal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    struct sigaction prev;
    memset(&prev, 0, sizeof(prev));
    if (sigaction(sig, &sa, &prev) == 0) {
        g_slots[g_slotCount].signo = sig;
        g_slots[g_slotCount].prev = prev;
        ++g_slotCount;
    }
}

__attribute__((constructor)) void cgInstall()
{
    g_phase[0] = '\0';
    g_item[0] = '\0';
    g_logPath[0] = '\0';

    // 备用信号栈(主线程): 栈溢出型 SIGSEGV 时处理器才有地方落脚
    g_altStackDesc.ss_sp = g_altStack;
    g_altStackDesc.ss_size = sizeof(g_altStack);
    g_altStackDesc.ss_flags = 0;
    (void)sigaltstack(&g_altStackDesc, nullptr);

    // /proc/self/status 的 fd 必须在这里 open —— 处理器里不做 open
    g_procFd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    // 逐项地址空间归因用的第二个 fd(同样在安装时 open; 普通上下文里读)
    g_procFd2 = open("/proc/self/status", O_RDONLY | O_CLOEXEC);

    // 本模块的装载基址与代码段范围(读 /proc/self/maps; 只在普通上下文做一次)
    cgResolveSelfRange();
    cgRefreshExecRanges();

    cgInstallOne(SIGSEGV);
    cgInstallOne(SIGBUS);
    cgInstallOne(SIGFPE);
    cgInstallOne(SIGILL);
    cgInstallOne(SIGABRT);
    cgInstallOne(SIGSYS);   // OHOS seccomp 策略拦截系统调用时会发这个(默认动作也是杀进程)

    // 卡死取证用的采样信号(默认 SIGUSR1; 不是致命信号, 处理器抓完栈就返回, 不动进程)
    cgInstallHang();

    // 故意不装的: SIGKILL/SIGSTOP(不可捕获), 以及 SIGTERM/SIGXCPU/SIGXFSZ
    // —— SIGTERM 可能被 ArkTS 运行时用作“优雅退出”的钩子, 我们转发后会紧接着 _exit,
    //    会改变它的语义, 所以默认不碰。若你想连 CPU 时间 rlimit(SIGXCPU)一起抓,
    //    在这里加一行 cgInstallOne(SIGXCPU); 即可。

    g_ready = 1;
}

} // namespace

// ===========================================================================
//  C 接口(供 napi 层与后续负载代码调用)
// ===========================================================================

// 每次开跑一项之前调用。标记本身仍然只做一次 bounded copy + 3 次标量写。
// 2026-10-12 追加的两件旁路事(都在负载开跑之前, 不在任何计时窗口里, 不计分)
//   ① cgItemMemOnMarker: 读一次 /proc/self/status 做逐项地址空间归因(顺便给上一项结算);
//   ② cgMaybeRefreshModTable: 至少隔 1 秒才真的读一次 /proc/self/maps 刷新模块表
//      (库是逐步 dlopen 的, 只有持续刷新, 崩溃现场的那张表里才会有当时的库)。
//   两件事都发生在 g_seq 的那对奇数/偶数写之外 —— 处理器看到的标记仍然是原子的。
//   任何失败都只是让记录里写"读不到", 不改负载行为、不改分数。
extern "C" void auroraSetCurrentItem(const char* phase, const char* item, int index, int total)
{
    cgItemMemOnMarker(phase, item, index, total);
    cgMaybeRefreshModTable();

    int s = (int)g_seq;
    g_seq = (sig_atomic_t)(s + 1);        // 变奇数: 处理器会知道正在写
    cgCopyBounded(g_phase, kPhaseCap, phase);
    cgCopyBounded(g_item, kItemCap, item);
    g_index = (sig_atomic_t)index;
    g_total = (sig_atomic_t)total;
    g_markTid = (sig_atomic_t)gettid();   // 跑该项的线程 -> 卡死时向它要栈
    g_idle = 0;                           // 有负载在跑: 看门狗可以判卡死
#if AURORA_CRASH_MARK_TS
    g_markMs = cgNowMs();   // musl 走 vDSO, 不是陷入内核的系统调用
#else
    g_markMs = 0;
#endif
    g_seq = (sig_atomic_t)(s + 2);        // 变偶数: 标记稳定
}

// 一项跑完(或异常退出前)把标记清成"无负载运行中"。必须用这个函数, 不要自己调
// auroraSetCurrentItem 写那两条空闲文案 —— 只有这里会把 g_idle 置 1, 看门狗才不会把
// 两项之间的空档当成卡死(真机踩过: 空档 32 秒 -> 112KB 假现场)。
// 语义与 auroraSetCurrentItem 逐字一致(同样的 phase/item 文案, index/total = 0/0),
// 因此"面包屑长什么样"没有变化, 崩溃取证那一段(见 cgBuildReport 的空闲分支)也不受影响。
extern "C" void auroraSetIdleMarker(void)
{
    // 旁路: 空闲 = 上一项已经结束 -> 给正在跑的那一项结算(它的 Δ 到这一刻才完整)。
    // g_idle / g_markMs 的语义一个字都没动。
    cgItemMemOnIdle();

    int s = (int)g_seq;
    g_seq = (sig_atomic_t)(s + 1);
    cgCopyBounded(g_phase, kPhaseCap, "(无负载运行中: 上一项已结束)");
    cgCopyBounded(g_item, kItemCap, "(空闲)");
    g_index = 0;
    g_total = 0;
    g_markTid = (sig_atomic_t)gettid();
    g_idle = 1;                           // 空闲: 看门狗不得据此判卡死
#if AURORA_CRASH_MARK_TS
    g_markMs = cgNowMs();
#else
    g_markMs = 0;
#endif
    g_seq = (sig_atomic_t)(s + 2);
}

// 由 ArkTS 传 context.filesDir 进来; 在这里把证据文件 open 好并常驻 fd。
extern "C" int auroraSetLogDir(const char* dir)
{
    if (dir == nullptr || dir[0] == '\0') {
        return 0;
    }
    char path[kPathCap + 24];
    int n = 0;
    for (; dir[n] != '\0' && n < kPathCap - 1; ++n) {
        path[n] = dir[n];
    }
    const char* suffix = "/native_crash.txt";
    for (int i = 0; suffix[i] != '\0' && n < (int)sizeof(path) - 1; ++i) {
        path[n++] = suffix[i];
    }
    path[n] = '\0';

    int fd = cgOpenEvidence(path);   // 0660 + fchmod 兜底(理由见 kEvidenceMode)
    if (fd < 0) {
        return 0;
    }
    int old = (int)g_crashFd;
    g_crashFd = (sig_atomic_t)fd;   // 单次原子替换; 旧 fd 故意不 close(避免与处理器竞态)
    (void)old;
    cgResolveSelfRange();           // 构造期读不到 maps 时在这里补一次(已解析则空操作)
    cgRefreshExecRanges();          // 此时运行时库基本都加载完了, 刷新可执行映射表

    // 同一目录下的卡死证据文件 + 启动挂起看门狗(只启动一次)
    char hpath[kPathCap + 24];
    int hn = 0;
    for (; dir[hn] != '\0' && hn < kPathCap - 1; ++hn) {
        hpath[hn] = dir[hn];
    }
    const char* hsuffix = "/native_hang.txt";
    for (int i = 0; hsuffix[i] != '\0' && hn < (int)sizeof(hpath) - 1; ++i) {
        hpath[hn++] = hsuffix[i];
    }
    hpath[hn] = '\0';
    const int hfd = cgOpenEvidence(hpath);   // 同上: 0600 会让卡死记录拉不下来
    if (hfd >= 0) {
        g_hangFd = (sig_atomic_t)hfd;
        cgCopyBounded(g_hangPath, (int)sizeof(g_hangPath), hpath);
        // 文件是 O_APPEND 打开的(跨启动累积), 所以总量闸门的起点 = 现有大小。fstat 只在
        // 这里(普通上下文)做一次, 看门狗线程只做标量比较。
        struct stat st;
        if (fstat(hfd, &st) == 0 && st.st_size > 0) {
            g_hangBytes = (long long)st.st_size;
        }
        if (g_hangBytes >= kMaxHangFileBytes) {
            g_hangSizeCapped = true;   // 老文件已经超限: 本轮不再追加
        }
    }
    (void)cgStartWatchdog();

    // 同一目录下的逐项地址空间归因文件(旁路, 见文件头 (B))。
    // 为什么要单独落一个文件: 真机上进程更可能是被 SIGKILL 掉的(内存压力/后台管制),
    // 那种情况不会有任何崩溃记录 —— 这时候盘上唯一能回答"是哪一项撑起了 14 GB 虚拟地址
    // 空间"的就是这份逐项账目(开发者用 hdc file recv 拉下来即可)。
    char mpath[kPathCap + 24];
    int mn = 0;
    for (; dir[mn] != '\0' && mn < kPathCap - 1; ++mn) {
        mpath[mn] = dir[mn];
    }
    const char* msuffix = "/native_memory.txt";
    for (int i = 0; msuffix[i] != '\0' && mn < (int)sizeof(mpath) - 1; ++i) {
        mpath[mn++] = msuffix[i];
    }
    mpath[mn] = '\0';
    const int mfd = cgOpenEvidence(mpath);   // 同 kEvidenceMode: 0660 才拉得下来
    if (mfd >= 0) {
        g_memFd = (sig_atomic_t)mfd;
        cgCopyBounded(g_memPath, (int)sizeof(g_memPath), mpath);
        struct stat mst;
        if (fstat(mfd, &mst) == 0 && mst.st_size > 0) {
            g_memBytes = (long long)mst.st_size;
        }
        if (g_memBytes >= kMaxMemFileBytes) {
            g_memCapped = true;   // 老文件已经超限: 本轮不再追加
        } else if (g_memBytes == 0) {
            // 空文件才写表头: 人拿 hdc 拉下来要一眼看懂这是什么、怎么读
            static const char kHead[] =
                "# Aurora 逐项地址空间归因(旁路, 不计分) —— 每个 item 标记点读一次 /proc/self/status\n"
                "# 每行 = 一项结束时的快照 + 相对该项开始时的增量(Δ); 标记点是'开跑前'打的, 所以\n"
                "# '下一项开始那一枪'就是'上一项结束那一枪', 中间只隔两项之间的胶水代码。\n"
                "# 判据(写死): ΔVmSize > 524288 kB(512 MB) 或 ΔVmRSS > 262144 kB(256 MB) => [本项占用异常, 见该项明细]\n"
                "# 列: #序号 phase=\"...\" item=\"...\" index/total VmSize VmRSS VmHWM Threads ΔVmSize ΔVmRSS ended=<UTC>\n";
            cgWriteAll(mfd, kHead, (int)sizeof(kHead) - 1);
            g_memBytes += (long long)sizeof(kHead) - 1;
        }
    }

    cgCopyBounded(g_logPath, (int)sizeof(g_logPath), path);
    return 1;
}

extern "C" const char* auroraCrashLogPath()
{
    return g_logPath;
}

extern "C" int auroraCrashGuardReady()
{
    return (int)g_ready;
}

// 采样一次内存足迹(普通上下文; 复用安装时 open 好的 /proc/self/status fd)。
// 建议 ArkTS 在每项开始前调一次, 并把 lastMemorySample() 写进自己的面包屑——
// 进程被 SIGKILL 时不会留下任何崩溃记录, 这一行就是唯一的“临死内存足迹”。
extern "C" void auroraSampleMemory()
{
#if AURORA_CRASH_MEM_SNAPSHOT
    if (cgReadProcInto(g_procBuf2) <= 0) {
        return;
    }
    char u[8];
    int ok = 0;
    g_sampleRssKb = cgProcFieldEx(g_procBuf2, "VmRSS:", 6, u, (int)sizeof(u), &ok) ;
    if (ok == 0) { g_sampleRssKb = -1; }
    g_sampleHwmKb = cgProcFieldEx(g_procBuf2, "VmHWM:", 6, u, (int)sizeof(u), &ok);
    if (ok == 0) { g_sampleHwmKb = -1; }
    g_sampleSizeKb = cgProcFieldEx(g_procBuf2, "VmSize:", 7, u, (int)sizeof(u), &ok);
    if (ok == 0) { g_sampleSizeKb = -1; }
    g_sampleThreads = cgProcFieldEx(g_procBuf2, "Threads:", 8, u, (int)sizeof(u), &ok);
    if (ok == 0) { g_sampleThreads = -1; }
    g_sampleMs = cgNowMs();

    char* p = g_sampleText;
    char* end = g_sampleText + sizeof(g_sampleText) - 2;
    p = cgPutStr(p, end, "rss=");
    p = cgPutDec(p, end, g_sampleRssKb);
    p = cgPutStr(p, end, "kB hwm=");
    p = cgPutDec(p, end, g_sampleHwmKb);
    p = cgPutStr(p, end, "kB size=");
    p = cgPutDec(p, end, g_sampleSizeKb);
    p = cgPutStr(p, end, "kB threads=");
    p = cgPutDec(p, end, g_sampleThreads);
    p = cgPutStr(p, end, " sampled=");
    p = cgPutIso(p, end, g_sampleMs);
    *p = '\0';
#endif
}

// 逐项地址空间归因(旁路, 不计分): 把"每一项结束时"的 VmSize/VmRSS/VmHWM 以及
// 相对上一项的增量整表渲染成文本。崩溃记录里用的就是同一份渲染函数(见 cgItemMemPutReport)。
// 返回写入长度; 无内容/参数不对返回 -1。逐行落盘的副本在 auroraItemMemoryLogPath()。
extern "C" int auroraItemMemoryReport(char* buf, int cap)
{
    if (buf == nullptr || cap <= 1) {
        return -1;
    }
    char* p = buf;
    char* end = buf + cap - 1;
    p = cgItemMemPutReport(p, end, 0);   // 0 = 全部列出(不截断)
    *p = '\0';
    return (int)(p - buf);
}

// 逐项账目文件(一般是 <filesDir>/native_memory.txt); 空串 = 还没调 setLogDir。
extern "C" const char* auroraItemMemoryLogPath()
{
    return g_memPath;
}

// 取最近一次采样, 返回长度; -1 = 还没采样过。
extern "C" int auroraLastMemory(char* buf, int cap)
{
    if (buf == nullptr || cap <= 1) {
        return -1;
    }
    if (g_sampleText[0] == '\0') {
        buf[0] = '\0';
        return -1;
    }
    cgCopyBounded(buf, cap, g_sampleText);
    int n = 0;
    while (n < cap - 1 && buf[n] != '\0') {
        ++n;
    }
    return n;
}

extern "C" int auroraCrashReportExists()
{
    if (g_logPath[0] == '\0') {
        return 0;
    }
    return access(g_logPath, F_OK) == 0 ? 1 : 0;
}

// 读证据文件内容到调用者缓冲(非信号上下文, 可以用 open/read)。
// 返回写入字节数; -1 = 没有文件/没有记录; 文件过大时只回最后 cap-1 字节(尾部=最近一次)。
extern "C" int auroraTakeCrashReport(char* buf, int cap)
{
    if (buf == nullptr || cap <= 1 || g_logPath[0] == '\0') {
        return -1;
    }
    int fd = open(g_logPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    long long size = (long long)lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        close(fd);
        return -1;
    }
    int want = cap - 1;
    long long off = 0;
    bool truncated = false;
    if (size > (long long)want) {
        off = size - (long long)want;
        truncated = true;
    }
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    int total = 0;
    while (total < want) {
        ssize_t r = read(fd, buf + total, (size_t)(want - total));
        if (r > 0) {
            total += (int)r;
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fd);
    buf[total] = '\0';
    if (truncated) {
        // 从半个记录开始会误导, 丢掉第一行残片
        int k = 0;
        while (k < total && buf[k] != '\n') {
            ++k;
        }
        if (k < total) {
            int rest = total - (k + 1);
            for (int i = 0; i < rest; ++i) {
                buf[i] = buf[k + 1 + i];
            }
            total = rest;
            buf[total] = '\0';
        }
    }
    return total;
}

// ---- 真机自检钩子(只在排障时用, 发布版可以删掉这三行 + 这段实现) ----
// mode 0: 不崩, 只往证据文件写一条链路自检记录(验证路径/fd/格式化/读回)
// mode 1: 故意空指针写 -> 真 SIGSEGV(si_code=SEGV_MAPERR, 走完整处理器路径, 进程会退出)
// mode 2: raise(SIGABRT)(走完整处理器路径, 进程会退出)
__attribute__((noinline)) void cgCrashNowSegv()
{
    volatile unsigned long* bad = (volatile unsigned long*)(uintptr_t)0x18UL;
    *bad = 0xDEADBEEFUL;
}

extern "C" void auroraCrashSelfTest(int mode)
{
    if (mode == 0) {
        char rec[320];
        char* p = rec;
        char* end = rec + sizeof(rec) - 2;
        p = cgPutStr(p, end, "----- AURORA CRASH GUARD SELF TEST (非崩溃) -----\n");
        p = cgPutStr(p, end, "when   : ");
        p = cgPutIso(p, end, cgNowMs());
        p = cgPutStr(p, end, "  fd=");
        p = cgPutDec(p, end, (long long)g_crashFd);
        p = cgPutStr(p, end, "\npath   : ");
        p = cgPutStr(p, end, (g_logPath[0] != '\0') ? g_logPath : "(未设置: 先调 setLogDir)");
        p = cgPutStr(p, end, "\nguard  : ready=");
        p = cgPutDec(p, end, (long long)g_ready);
        p = cgPutStr(p, end, " self_base=");
        p = cgPutHex(p, end, g_selfBase);
        p = cgPutStr(p, end, " text=");
        p = cgPutHex(p, end, g_selfTextStart);
        p = cgPutCh(p, end, '-');
        p = cgPutHex(p, end, g_selfTextEnd);
        p = cgPutStr(p, end, "\n----- END AURORA CRASH GUARD SELF TEST -----\n");
        int fd = (int)g_crashFd;
        if (fd < 0) {
            fd = STDERR_FILENO;
        }
        cgWriteAll(fd, rec, (int)(p - rec));
        return;
    }
    if (mode == 1) {
        cgCrashNowSegv();
        return;
    }
    if (mode == 2) {
        (void)raise(SIGABRT);
    }
}

// 清空证据(读完横幅后调用)。用 ftruncate 而不是 unlink: 保持已 open 的 fd 继续可用。
extern "C" void auroraClearCrashReport()
{
    int fd = (int)g_crashFd;
    if (fd >= 0) {
        (void)ftruncate(fd, 0);
        return;
    }
    if (g_logPath[0] != '\0') {
        (void)unlink(g_logPath);
    }
}

// ===========================================================================
//  挂起看门狗 + 栈采样(卡死取证)  —— 第一部分: 信号处理器侧
//
//  真机现场: 面包屑停住 + 进程活着 + 持续烧 CPU(空转死循环)。这种卡死不会产生
//  任何信号, 崩溃处理器抓不到, 必须由 App 自己主动打栈。
//
//  设计要点:
//   * 看门狗线程每 5 秒纯内存读一次“当前项标记 + 已运行时长”, 平时零 IO;
//     某项超过 30 秒 -> 向“跑该项的线程”tgkill(SIGUSR1), 在该线程上抓 64 层栈。
//   * _Unwind_Backtrace / _Unwind_GetIP 用 dlsym 解析(RTLD_DEFAULT 或已加载的
//     libc++_shared.so / libunwind.so), 不做硬链接依赖: 解析不到就降级成
//     “寄存器现场 + 原始栈窗口”, 不会因为缺符号导致模块加载失败。
//   * 写盘顺序刻意分成三段, 保证任意一段卡住都有东西可用:
//       ① 先写“不依赖展开器”的现场(中断点寄存器 + 当前项标记 + 时间);
//       ② 再调展开器写 64 层原始 PC;
//       ③ 最后才逐帧 dladdr 取符号名(dladdr 要碰动态链接器)。
//   * 采样信号是 SIGUSR1: 只有看门狗自己投递的才被认领(g_hangPending 计数),
//     外来 SIGUSR1 一律按原语义链式转发(原来是什么就还它什么), 处理器不退出进程。
// ===========================================================================

namespace {

constexpr int kHangThresholdMs = 30 * 1000;   // 超过 30 秒判定疑似卡死
constexpr int kMaxHangRounds   = 6;           // 整个进程最多采这么多轮(防止刷盘)
constexpr int kHangSamples     = 3;           // 每轮采样次数(看“卡在同一函数”)
constexpr int kHangSampleGapMs = 1000;        // 采样间隔
constexpr int kMaxFrames       = 64;          // 每次抓多少层栈
constexpr int kRawStackBytes   = 2048;        // 原始栈窗口(给离线兜底分析)
constexpr int kMaxTasks        = 160;         // 最多枚举多少线程
constexpr int kHangWaitMs      = 500;         // 等处理器返回的上限

#ifndef AURORA_HANG_SIGNAL
#define AURORA_HANG_SIGNAL SIGUSR1
#endif
// 注意: musl 里 SIGRTMIN 是函数调用(__libc_current_sigrtmin()), 不能当 constexpr,
// 所以这里用运行时取值, 于是 -DAURORA_HANG_SIGNAL='SIGRTMIN+7' 这种覆盖也能用。
int cgHangSignalNo()
{
    return AURORA_HANG_SIGNAL;
}

#ifndef AURORA_HANG_DLADDR
#define AURORA_HANG_DLADDR 1
#endif

// ---- 展开器: dlsym 解析, 零硬链接依赖 ---------------------------------------
struct _Unwind_Context;   // 不透明类型, 与 libunwind 头文件里的声明一致
typedef int (*CgUnwindTraceFn)(struct _Unwind_Context*, void*);
typedef int (*CgUnwindBacktraceFn)(CgUnwindTraceFn, void*);
typedef unsigned long (*CgUnwindGetIpFn)(struct _Unwind_Context*);

CgUnwindBacktraceFn g_unwindBacktrace = nullptr;
CgUnwindGetIpFn g_unwindGetIp = nullptr;
char g_unwindFrom[40];

void cgResolveUnwinder()
{
    if (g_unwindBacktrace != nullptr && g_unwindGetIp != nullptr) {
        return;
    }
    void* f1 = dlsym(RTLD_DEFAULT, "_Unwind_Backtrace");
    void* f2 = dlsym(RTLD_DEFAULT, "_Unwind_GetIP");
    if (f1 != nullptr && f2 != nullptr) {
        cgCopyBounded(g_unwindFrom, (int)sizeof(g_unwindFrom), "RTLD_DEFAULT");
    } else {
        // 只查“已经加载”的库(RTLD_NOLOAD): 不主动把 libc++_shared 拉进内存
        static const char* const kLibs[] = {"libc++_shared.so", "libunwind.so", "libc++.so"};
        for (int i = 0; i < 3 && f1 == nullptr; ++i) {
            void* h = dlopen(kLibs[i], RTLD_NOW | RTLD_NOLOAD);
            if (h == nullptr) {
                continue;
            }
            void* g1 = dlsym(h, "_Unwind_Backtrace");
            void* g2 = dlsym(h, "_Unwind_GetIP");
            if (g1 != nullptr && g2 != nullptr) {
                f1 = g1;
                f2 = g2;
                cgCopyBounded(g_unwindFrom, (int)sizeof(g_unwindFrom), kLibs[i]);
            }
        }
    }
    if (f1 != nullptr && f2 != nullptr) {
        g_unwindBacktrace = (CgUnwindBacktraceFn)f1;
        g_unwindGetIp = (CgUnwindGetIpFn)f2;
    }
}

// ---- 拉栈: 全静态缓冲, 处理器里只需要“回调存指针” ----------------------------
struct CgTrace {
    int count;
};

int cgTraceCb(struct _Unwind_Context* ctx, void* arg)
{
    CgTrace* t = (CgTrace*)arg;
    if (t->count >= kMaxFrames) {
        return 5;   // _URC_END_OF_STACK
    }
    unsigned long ip = 0UL;
    if (g_unwindGetIp != nullptr) {
        ip = g_unwindGetIp(ctx);
    }
    g_hangPcs[t->count++] = (unsigned long long)ip;
    return 0;       // _URC_NO_REASON
}

// ---- 外来 SIGUSR1 的链式处理(保持原语义, 不 _exit) ------------------------
void cgReRaiseDefault(int sig)
{
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    (void)sigaction(sig, &dfl, nullptr);
    (void)kill(getpid(), sig);
}

void cgChainForeignSignal(int sig, siginfo_t* info, void* uctx)
{
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].signo != sig) {
            continue;
        }
        const struct sigaction& prev = g_slots[i].prev;
        if ((prev.sa_flags & SA_SIGINFO) != 0) {
            void (*sa)(int, siginfo_t*, void*) = prev.sa_sigaction;
            if (sa != nullptr && (uintptr_t)sa > 1U) {
                sa(sig, info, uctx);
                return;
            }
            if ((uintptr_t)sa == 1U) {
                return;   // SIG_IGN
            }
        } else {
            void (*sh)(int) = prev.sa_handler;
            if (sh != nullptr && (uintptr_t)sh > 1U) {
                sh(sig);
                return;
            }
            if ((uintptr_t)sh == 1U) {
                return;   // SIG_IGN
            }
        }
        break;
    }
    cgReRaiseDefault(sig);   // 原来是 SIG_DFL: 还它默认动作(SIGUSR1 默认是终止)
}

// ---- 采样处理器(运行在被卡住的那个线程上) ----------------------------------
void cgHangWriteFrames();
void cgHangWriteSymbols();
void cgHangWriteRawStack(void* uctx, bool full);

void cgHangHandler(int sig, siginfo_t* info, void* uctx)
{
    if (g_hangPending <= 0) {
        cgChainForeignSignal(sig, info, uctx);   // 不是我们投的: 原样转发, 不动进程
        return;
    }
    --g_hangPending;
    if (g_hangInHandler != 0) {
        return;   // 防重入(同线程信号默认被屏蔽, 这里只是保险)
    }
    g_hangInHandler = 1;

    const long long now = cgNowMs();
    const int sampleNo = (int)g_hangSampleNo;

    // ① 先落“不依赖展开器”的现场: 中断点寄存器 + 当前项标记 + 时间
    {
        char rec[1024];
        char* p = rec;
        char* end = rec + sizeof(rec) - 2;
        p = cgPutStr(p, end, "----- AURORA HANG STACK SAMPLE #");
        p = cgPutDec(p, end, sampleNo);
        p = cgPutStr(p, end, " (tid=");
        p = cgPutDec(p, end, (long long)gettid());
        p = cgPutStr(p, end, " why=");
        p = cgPutStr(p, end, (g_hangWhy != nullptr) ? g_hangWhy : "-");
        p = cgPutStr(p, end, ") -----\n");
        p = cgPutStr(p, end, "when      : ");
        p = cgPutIso(p, end, now);
        p = cgPutStr(p, end, "  epoch_ms=");
        p = cgPutDec(p, end, now);
        p = cgPutCh(p, end, '\n');

        {
            unsigned long long pc = 0;
            unsigned long long lr = 0;
            unsigned long long sp = 0;
            unsigned long long fp = 0;
#if defined(__aarch64__)
            if (uctx != nullptr) {
                const ucontext_t* uc = (const ucontext_t*)uctx;
                pc = (unsigned long long)uc->uc_mcontext.pc;
                lr = (unsigned long long)uc->uc_mcontext.regs[30];
                sp = (unsigned long long)uc->uc_mcontext.sp;
                fp = (unsigned long long)uc->uc_mcontext.regs[29];
            }
#endif
            p = cgPutStr(p, end, "interrupt : pc=");
            p = cgPutHex(p, end, pc);
            p = cgPutStr(p, end, " lr=");
            p = cgPutHex(p, end, lr);
            p = cgPutStr(p, end, " sp=");
            p = cgPutHex(p, end, sp);
            p = cgPutStr(p, end, " fp=");
            p = cgPutHex(p, end, fp);
            p = cgPutStr(p, end, "  <- 线程被信号打断时正在执行的地址\n");
        }

        {
            int seq0 = (int)g_seq;
            char phase[kPhaseCap];
            char item[kItemCap];
            bool torn = (seq0 & 1) != 0;
            cgCopyBounded(phase, kPhaseCap, g_phase);
            cgCopyBounded(item, kItemCap, g_item);
            const int idx = (int)g_index;
            const int tot = (int)g_total;
            const long long markMs = g_markMs;
            if ((int)g_seq != seq0) {
                torn = true;
            }
            p = cgPutStr(p, end, "marker    : phase=\"");
            p = cgPutStr(p, end, phase);
            p = cgPutStr(p, end, "\" item=\"");
            p = cgPutStr(p, end, item);
            p = cgPutStr(p, end, "\" index=");
            p = cgPutDec(p, end, idx);
            p = cgPutCh(p, end, '/');
            p = cgPutDec(p, end, tot);
            p = cgPutStr(p, end, " marked=");
            if (markMs > 0) {
                p = cgPutIso(p, end, markMs);
                if (now >= markMs) {
                    p = cgPutStr(p, end, " elapsed_s=");
                    p = cgPutDec(p, end, (now - markMs) / 1000);
                }
            } else {
                p = cgPutStr(p, end, "(未记时间)");
            }
            if (torn) {
                p = cgPutStr(p, end, " [标记正在更新]");
            }
            p = cgPutCh(p, end, '\n');
        }
        int fd = (int)g_hangFd;
        if (fd < 0) {
            fd = STDERR_FILENO;
        }
        cgWriteAll(fd, rec, (int)(p - rec));
    }

    // ② 展开器拉 64 层栈(可能碰动态链接器锁, 所以放在现场之后)
    {
        g_hangFrameCount = 0;
        if (g_unwindBacktrace != nullptr) {
            CgTrace t;
            t.count = 0;
            (void)g_unwindBacktrace(cgTraceCb, &t);
            g_hangFrameCount = t.count;
        }
        cgHangWriteFrames();
    }

    // ③ 逐帧 dladdr 取符号名(最可能碰 loader 的一步, 放最后)
    cgHangWriteSymbols();

    // ④ 原始栈窗口: 展开器不可用时的兜底, 也给离线栈扫描用。
    //    只在每轮第 1 次采样时整窗 dump, 后两次只列“像返回地址的槽位”, 控制文件大小。
    cgHangWriteRawStack(uctx, (g_unwindBacktrace == nullptr) || ((sampleNo % 100) <= 1));

    ++g_hangDone;
    g_hangInHandler = 0;
}

void cgHangWriteFrames()
{
    char rec[4096];
    char* p = rec;
    char* end = rec + sizeof(rec) - 2;
    p = cgPutStr(p, end, "unwind    : ");
    if (g_unwindBacktrace == nullptr) {
        p = cgPutStr(p, end, "不可用(进程里没找到 _Unwind_Backtrace; 只能看上面的中断点 + 原始栈窗口)");
        p = cgPutCh(p, end, '\n');
        cgWriteAll((int)g_hangFd >= 0 ? (int)g_hangFd : STDERR_FILENO, rec, (int)(p - rec));
        return;
    }
    p = cgPutStr(p, end, "frames=");
    p = cgPutDec(p, end, (long long)g_hangFrameCount);
    p = cgPutStr(p, end, "  via=");
    p = cgPutStr(p, end, (g_unwindFrom[0] != '\0') ? g_unwindFrom : "?");
    p = cgPutCh(p, end, '\n');
    for (int i = 0; i < g_hangFrameCount; ++i) {
        p = cgPutStr(p, end, "frame  #");
        p = cgPutDec(p, end, i);
        p = cgPutCh(p, end, ' ');
        p = cgPutHex(p, end, g_hangPcs[i]);
        p = cgPutCh(p, end, '\n');
    }
    int fd = (int)g_hangFd;
    if (fd < 0) {
        fd = STDERR_FILENO;
    }
    cgWriteAll(fd, rec, (int)(p - rec));
}

void cgHangWriteSymbols()
{
#if AURORA_HANG_DLADDR
    if (g_hangFrameCount <= 0) {
        return;
    }
    char line[224];
    for (int i = 0; i < g_hangFrameCount; ++i) {
        Dl_info di;
        memset(&di, 0, sizeof(di));
        const void* addr = (const void*)(uintptr_t)g_hangPcs[i];
        if (dladdr(addr, &di) == 0) {
            continue;   // 取不到就算了(原始 PC 已经写在上一段)
        }
        char* p = line;
        char* end = line + sizeof(line) - 2;
        p = cgPutStr(p, end, "sym    #");
        p = cgPutDec(p, end, i);
        p = cgPutCh(p, end, ' ');
        p = cgPutHex(p, end, g_hangPcs[i]);
        p = cgPutStr(p, end, " ");
        if (di.dli_fname != nullptr) {
            const char* base = di.dli_fname;
            for (const char* q = di.dli_fname; *q != '\0'; ++q) {
                if (*q == '/') {
                    base = q + 1;
                }
            }
            p = cgPutStr(p, end, base);
        } else {
            p = cgPutStr(p, end, "?");
        }
        if (di.dli_fbase != nullptr) {
            p = cgPutStr(p, end, "+");
            p = cgPutHex(p, end, (unsigned long long)(uintptr_t)addr - (unsigned long long)(uintptr_t)di.dli_fbase);
        }
        p = cgPutStr(p, end, " ");
        p = cgPutStr(p, end, (di.dli_sname != nullptr) ? di.dli_sname : "(无符号)");
        p = cgPutCh(p, end, '\n');
        int fd = (int)g_hangFd;
        if (fd < 0) {
            fd = STDERR_FILENO;
        }
        cgWriteAll(fd, line, (int)(p - line));
    }
#endif
}

// 原始栈窗口: 从“被打断时的 sp”往上拷一段栈字节, 全部是纯内存读 + write,
// 不依赖任何展开器。sp 以上的地址一定落在同一个已映射的栈区间里(栈向下增长),
// 再用栈顶(看门狗发信号前从 /proc/self/task/<tid>/stat 读到的 startstack)封顶, 不越界。
void cgHangWriteRawStack(void* uctx, bool full)
{
    unsigned long long sp = 0;
#if defined(__aarch64__)
    if (uctx != nullptr) {
        sp = (unsigned long long)((const ucontext_t*)uctx)->uc_mcontext.sp;
    }
#endif
    if (sp == 0) {
        return;
    }
    unsigned long long end = sp + (unsigned long long)kRawStackBytes;
    const unsigned long long top = g_hangStackTop;
    if (top > sp && end > top) {
        end = top;
    }
    if (end <= sp) {
        return;
    }
    int fd = (int)g_hangFd;
    if (fd < 0) {
        fd = STDERR_FILENO;
    }
    const unsigned int len = (unsigned int)(end - sp);
    {
        char line[160];
        char* p = line;
        char* endp = line + sizeof(line) - 2;
        p = cgPutStr(p, endp, "rawstack  : sp=");
        p = cgPutHex(p, endp, sp);
        p = cgPutStr(p, endp, " bytes=");
        p = cgPutDec(p, endp, (long long)len);
        p = cgPutStr(p, endp, full ? " (完整窗口)\n" : " (只列可疑返回地址)\n");
        cgWriteAll(fd, line, (int)(p - line));
    }
    const unsigned char* src = (const unsigned char*)(uintptr_t)sp;
    if (full && len > 0) {
        char buf[1024];
        char* p = buf;
        char* endp = buf + sizeof(buf) - 128;
        for (unsigned int off = 0; off < len; off += 16) {
            p = cgPutStr(p, endp, "raw     +");
            p = cgPutHex(p, endp, (unsigned long long)off);
            p = cgPutStr(p, endp, " :");
            for (unsigned int i = 0; i < 16 && off + i < len; ++i) {
                const unsigned int b = (unsigned int)src[off + i];
                p = cgPutCh(p, endp, ' ');
                p = cgPutCh(p, endp, "0123456789abcdef"[(b >> 4) & 0xF]);
                p = cgPutCh(p, endp, "0123456789abcdef"[b & 0xF]);
            }
            p = cgPutCh(p, endp, '\n');
            if (p >= endp) {
                cgWriteAll(fd, buf, (int)(p - buf));
                p = buf;
            }
        }
        if (p > buf) {
            cgWriteAll(fd, buf, (int)(p - buf));
        }
    }
    // 栈上“落在某个模块可执行段内”的 8 字节槽位: 没有展开信息时, 这些就是候选返回地址。
    // 比对用的表在普通上下文里刷新好, 处理器只做整数比较(零系统调用)。
    if (len >= 8) {
        const int nExec = ((int)g_execSeq & 1) ? 0 : (int)g_execCount;
        int hits = 0;
        char line[192];
        for (unsigned int off = 0; off + 8 <= len && hits < 48; off += 8) {
            unsigned long long v = 0;
            for (int i = 7; i >= 0; --i) {
                v = (v << 8) | (unsigned long long)src[off + i];
            }
            const char* mod = nullptr;
            unsigned long long modBase = 0;
            for (int r = 0; r < nExec; ++r) {
                if (v >= g_execRanges[r].start && v < g_execRanges[r].end) {
                    mod = g_execRanges[r].name;
                    modBase = g_execRanges[r].start;
                    break;
                }
            }
            if (mod == nullptr) {
                if (nExec == 0 && g_selfTextEnd > g_selfTextStart &&
                    v >= g_selfTextStart && v < g_selfTextEnd) {
                    mod = g_selfName;
                    modBase = g_selfBase;
                } else {
                    continue;
                }
            }
            char* p = line;
            char* endp = line + sizeof(line) - 2;
            p = cgPutStr(p, endp, "codeptr  : sp+");
            p = cgPutHex(p, endp, (unsigned long long)off);
            p = cgPutStr(p, endp, " -> ");
            p = cgPutHex(p, endp, v);
            p = cgPutStr(p, endp, " in ");
            p = cgPutStr(p, endp, mod);
            p = cgPutStr(p, endp, " +");
            p = cgPutHex(p, endp, v - modBase);
            p = cgPutStr(p, endp, "\n");
            cgWriteAll(fd, line, (int)(p - line));
            ++hits;
        }
    }
}

// ---- 安装采样信号 ----------------------------------------------------------
void cgInstallHang()
{
    if (g_slotCount < kMaxSigs) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = cgHangHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        struct sigaction prev;
        memset(&prev, 0, sizeof(prev));
        if (sigaction(cgHangSignalNo(), &sa, &prev) == 0) {
            g_slots[g_slotCount].signo = cgHangSignalNo();
            g_slots[g_slotCount].prev = prev;
            ++g_slotCount;
        }
    }
    cgResolveUnwinder();
}

} // namespace

// ===========================================================================
//  挂起看门狗 + 栈采样  —— 第二部分: 看门狗线程(普通上下文, 允许 IO/分配)
//
//  纯内存巡检(每 5 秒): 读 g_seq/g_markMs/g_markTid/g_phase/g_item, 不碰任何文件。
//  只有“某项超过 30 秒还没结束”时才会: 枚举线程 -> 读 /proc -> 投采样信号 -> 写文件。
// ===========================================================================

namespace {

long long g_clkTck = 100;          // sysconf(_SC_CLK_TCK), 用于把 ticks 换成毫秒
char g_procBufW[kProcCap];         // 看门狗自己的 /proc/self/status 缓冲
int g_watchdogTid = 0;
volatile int g_selfTestSecs = 45;

struct CgTask {
    int tid;
    char state;
    long long ticks;                  // utime + stime
    unsigned long long startStack;    // 该线程栈顶(给原始栈窗口封顶)
    char wchan[24];
};

CgTask g_tasksA[kMaxTasks];
CgTask g_tasksB[kMaxTasks];

void cgSleepMs(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

// 普通上下文: 读一个小文件到缓冲(带 NUL 结尾并去尾部换行)
int cgReadSmallFile(const char* path, char* buf, int cap)
{
    if (cap <= 1) {
        return 0;
    }
    buf[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    int total = 0;
    while (total < cap - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(cap - 1 - total));
        if (n > 0) {
            total += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fd);
    buf[total] = '\0';
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r')) {
        buf[--total] = '\0';
    }
    return total;
}

long long cgAtoLL(const char* s)
{
    long long v = 0;
    bool neg = false;
    if (*s == '-') {
        neg = true;
        ++s;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (long long)(*s - '0');
        ++s;
    }
    return neg ? -v : v;
}

// /proc/<pid>/stat: 字段 2 是 comm(可能含空格/括号), 所以先找最后一个 ')', 之后从字段 3 数起。
// 需要: 3=state, 14=utime, 15=stime, 28=startstack
bool cgParseStat(const char* buf, CgTask* out)
{
    const char* rp = nullptr;
    for (const char* q = buf; *q != '\0'; ++q) {
        if (*q == ')') {
            rp = q;
        }
    }
    if (rp == nullptr) {
        return false;
    }
    const char* p = rp + 1;
    int idx = 2;   // 已消费 pid 与 comm
    while (*p != '\0') {
        while (*p == ' ') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char* s = p;
        while (*p != '\0' && *p != ' ') {
            ++p;
        }
        ++idx;
        if (idx == 3) {
            out->state = s[0];
        } else if (idx == 14) {
            out->ticks = cgAtoLL(s);
        } else if (idx == 15) {
            out->ticks += cgAtoLL(s);
        } else if (idx == 28) {
            out->startStack = (unsigned long long)cgAtoLL(s);
            break;
        }
    }
    return true;
}

void cgTaskPath(char* buf, int cap, int tid, const char* leaf)
{
    // snprintf 只在看门狗线程(普通上下文)里用, 处理器里不使用
    snprintf(buf, (size_t)cap, "/proc/self/task/%d/%s", tid, leaf);
}

int cgSnapTasks(CgTask* out, int cap)
{
    DIR* d = opendir("/proc/self/task");
    if (d == nullptr) {
        return 0;
    }
    int n = 0;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr && n < cap) {
        const char* nm = e->d_name;
        if (nm[0] < '0' || nm[0] > '9') {
            continue;
        }
        const int tid = (int)cgAtoLL(nm);
        if (tid <= 0) {
            continue;
        }
        char path[64];
        char buf[512];
        cgTaskPath(path, (int)sizeof(path), tid, "stat");
        if (cgReadSmallFile(path, buf, (int)sizeof(buf)) <= 0) {
            continue;
        }
        CgTask t;
        t.tid = tid;
        t.state = '?';
        t.ticks = 0;
        t.startStack = 0;
        t.wchan[0] = '\0';
        if (!cgParseStat(buf, &t)) {
            continue;
        }
        cgTaskPath(path, (int)sizeof(path), tid, "wchan");
        (void)cgReadSmallFile(path, t.wchan, (int)sizeof(t.wchan));
        out[n++] = t;
    }
    closedir(d);
    return n;
}

const CgTask* cgTaskOf(const CgTask* t, int n, int tid)
{
    for (int i = 0; i < n; ++i) {
        if (t[i].tid == tid) {
            return &t[i];
        }
    }
    return nullptr;
}

// CPU 增量最大的线程(排除看门狗自己)
int cgFindBusiest(const CgTask* a, int na, const CgTask* b, int nb, long long* deltaOut)
{
    int best = -1;
    long long bestD = 0;
    for (int i = 0; i < nb; ++i) {
        if (b[i].tid == g_watchdogTid) {
            continue;
        }
        const CgTask* pa = cgTaskOf(a, na, b[i].tid);
        if (pa == nullptr) {
            continue;
        }
        const long long d = b[i].ticks - pa->ticks;
        if (d > bestD) {
            bestD = d;
            best = b[i].tid;
        }
    }
    if (deltaOut != nullptr) {
        *deltaOut = bestD;
    }
    return best;
}

struct CgMemInfo {
    long long rssKb;
    long long hwmKb;
    long long peakKb;
    long long sizeKb;
    long long threads;
    bool ok;
};

CgMemInfo cgReadMemInfo()
{
    CgMemInfo m;
    m.rssKb = -1;
    m.hwmKb = -1;
    m.peakKb = -1;
    m.sizeKb = -1;
    m.threads = -1;
    m.ok = false;
    if (cgReadSmallFile("/proc/self/status", g_procBufW, kProcCap) <= 0) {
        return m;
    }
    char u[8];
    int ok = 0;
    m.peakKb = cgProcFieldEx(g_procBufW, "VmPeak:", 7, u, (int)sizeof(u), &ok);
    if (ok == 0) {
        m.peakKb = -1;
    }
    m.hwmKb = cgProcFieldEx(g_procBufW, "VmHWM:", 6, u, (int)sizeof(u), &ok);
    if (ok == 0) {
        m.hwmKb = -1;
    }
    m.rssKb = cgProcFieldEx(g_procBufW, "VmRSS:", 6, u, (int)sizeof(u), &ok);
    if (ok == 0) {
        m.rssKb = -1;
    }
    m.sizeKb = cgProcFieldEx(g_procBufW, "VmSize:", 7, u, (int)sizeof(u), &ok);
    if (ok == 0) {
        m.sizeKb = -1;
    }
    m.threads = cgProcFieldEx(g_procBufW, "Threads:", 8, u, (int)sizeof(u), &ok);
    if (ok == 0) {
        m.threads = -1;
    }
    m.ok = (m.rssKb >= 0 || m.peakKb >= 0 || m.sizeKb >= 0 || m.threads >= 0);
    return m;
}

void cgHangWriteBuf(const char* buf, int len)
{
    int fd = (int)g_hangFd;
    if (fd < 0) {
        fd = STDERR_FILENO;
    }
    if (fd != STDERR_FILENO && len > 0) {
        if (g_hangBytes >= kMaxHangFileBytes) {
            if (!g_hangSizeCapped) {
                g_hangSizeCapped = true;
                const char* note =
                    "\n[已达卡死证据文件上限 96 KB: 后续现场不再写入。"
                    "看门狗已按'空闲不判卡死 + 目标线程必须仍存在'两道检查过滤误报]\n";
                cgWriteAll(fd, note, (int)strlen(note));
            }
            return;
        }
        g_hangBytes += len;
    }
    cgWriteAll(fd, buf, len);
}

// 目标线程此刻是否还存在。tgkill 的第 3 个参数为 0 = 只做"存在性 + 权限"检查, 不投递任何信号。
// 用途: 标记里那个 tid 可能早就结束了(真机现场就是"投递失败 errno=3(ESRCH)" —— 8 项之间
// 的空档里, 上一项所在的线程已退出)。线程不存在就不可能是它卡死, 看门狗据此直接放弃这一轮,
// 既不投信号也不写现场(112 KB 假现场的第二个来源)。
bool cgThreadAlive(int tid)
{
    if (tid <= 0) {
        return false;
    }
    errno = 0;
    const long rc = syscall(SYS_tgkill, (pid_t)getpid(), (pid_t)tid, 0);
    if (rc == 0) {
        return true;
    }
    return errno != ESRCH;
}

// 投一枪采样信号, 并等处理器返回(最多 kHangWaitMs)。
// 注意: 超时也不清 g_hangPending —— 讯号可能只是来晚了, 清掉会让处理器
// 把它当成外来 SIGUSR1 而走默认动作(可能终止进程)。
void cgSignalThread(int tid, const char* why, int sampleNo)
{
    const int doneBefore = (int)g_hangDone;
    g_hangSampleNo = (sig_atomic_t)sampleNo;
    g_hangWhy = why;
    ++g_hangPending;
    if (syscall(SYS_tgkill, (pid_t)getpid(), (pid_t)tid, cgHangSignalNo()) != 0) {
        --g_hangPending;
        char line[160];
        char* p = line;
        char* end = line + sizeof(line) - 2;
        p = cgPutStr(p, end, "signal    : tid=");
        p = cgPutDec(p, end, (long long)tid);
        p = cgPutStr(p, end, " 投递失败 errno=");
        p = cgPutDec(p, end, (long long)errno);
        p = cgPutStr(p, end, " (线程可能已经结束)\n");
        cgHangWriteBuf(line, (int)(p - line));
        return;
    }
    for (int i = 0; i < kHangWaitMs / 10; ++i) {
        if ((int)g_hangDone != doneBefore) {
            return;
        }
        cgSleepMs(10);
    }
    char line[200];
    char* p = line;
    char* end = line + sizeof(line) - 2;
    p = cgPutStr(p, end, "signal    : tid=");
    p = cgPutDec(p, end, (long long)tid);
    p = cgPutStr(p, end, " 处理器 ");
    p = cgPutDec(p, end, (long long)kHangWaitMs);
    p = cgPutStr(p, end, "ms 内没返回 —— 它可能卡在 _Unwind_Backtrace/dladdr 的内部锁上");
    p = cgPutStr(p, end, "(此时盘上仍有中断点寄存器与标记那一段)\n");
    cgHangWriteBuf(line, (int)(p - line));
}

const char* cgVerdict(long long deltaTicks, const char* state, const char* wchan)
{
    if (deltaTicks < 0) {
        return "该线程已消失(项可能刚好跑完了)";
    }
    const long long halfTick = g_clkTck / 2;
    if (deltaTicks >= halfTick) {
        return "空转(死循环/忙等): 这一秒实打实在烧 CPU";
    }
    if (deltaTicks == 0) {
        const bool futex = (wchan != nullptr) && wchan[0] == 'f' && wchan[1] == 'u' &&
                           wchan[2] == 't' && wchan[3] == 'e' && wchan[4] == 'x';
        if (futex) {
            return "阻塞在 futex(锁/条件变量/等唤醒): 这一秒没烧 CPU => 偏死锁, 不是死循环";
        }
        if (state != nullptr && *state == 'D') {
            return "阻塞在不可中断等待(D 状态, 常见于 IO/页错误): 这一秒没烧 CPU";
        }
        if (state != nullptr && *state == 'S') {
            return "阻塞在可中断睡眠(S 状态): 这一秒没烧 CPU => 偏死锁/等事件";
        }
        return "阻塞或刚被唤醒: 这一秒没烧 CPU";
    }
    return "半忙: 部分时间在跑(带等待/重试的循环, 或刚进入卡死)";
}

void cgHangRound(int round, int targetTid, long long elapsedMs)
{
    char phase[kPhaseCap];
    char item[kItemCap];
    cgCopyBounded(phase, kPhaseCap, g_phase);
    cgCopyBounded(item, kItemCap, g_item);
    const int idx = (int)g_index;
    const int tot = (int)g_total;

    int nA = cgSnapTasks(g_tasksA, kMaxTasks);
    int tid = targetTid;
    long long totalFirst = -1;
    long long totalLast = -1;

    {
        char line[768];
        char* p = line;
        char* end = line + sizeof(line) - 2;
        p = cgPutStr(p, end, "\n===== AURORA HANG WATCHDOG round ");
        p = cgPutDec(p, end, round);
        p = cgPutStr(p, end, " =====\n");
        p = cgPutStr(p, end, "when      : ");
        p = cgPutIso(p, end, cgNowMs());
        p = cgPutCh(p, end, '\n');
        p = cgPutStr(p, end, "marker    : phase=\"");
        p = cgPutStr(p, end, phase);
        p = cgPutStr(p, end, "\" item=\"");
        p = cgPutStr(p, end, item);
        p = cgPutStr(p, end, "\" index=");
        p = cgPutDec(p, end, idx);
        p = cgPutCh(p, end, '/');
        p = cgPutDec(p, end, tot);
        p = cgPutStr(p, end, "  已运行 ");
        p = cgPutDec(p, end, elapsedMs / 1000);
        p = cgPutStr(p, end, " 秒(阈值 ");
        p = cgPutDec(p, end, (long long)(kHangThresholdMs / 1000));
        p = cgPutStr(p, end, " 秒) => 疑似卡死\n");
        p = cgPutStr(p, end, "target    : tid=");
        p = cgPutDec(p, end, (long long)tid);
        p = cgPutStr(p, end, "(当前项标记里记下的线程)  本次枚举到 ");
        p = cgPutDec(p, end, (long long)nA);
        p = cgPutStr(p, end, " 个线程\n");
        const CgMemInfo m = cgReadMemInfo();
        if (m.ok) {
            p = cgPutStr(p, end, "memory    : VmRSS=");
            p = cgPutDec(p, end, m.rssKb);
            p = cgPutStr(p, end, " kB VmHWM(RSS峰值)=");
            p = cgPutDec(p, end, m.hwmKb);
            p = cgPutStr(p, end, " kB VmPeak=");
            p = cgPutDec(p, end, m.peakKb);
            p = cgPutStr(p, end, " kB VmSize=");
            p = cgPutDec(p, end, m.sizeKb);
            p = cgPutStr(p, end, " kB Threads=");
            p = cgPutDec(p, end, m.threads);
            p = cgPutCh(p, end, '\n');
        } else {
            p = cgPutStr(p, end, "memory    : (读不到 /proc/self/status)\n");
        }
        p = cgPutStr(p, end, "unwinder  : ");
        if (g_unwindBacktrace != nullptr) {
            p = cgPutStr(p, end, "_Unwind_Backtrace 可用(来源 ");
            p = cgPutStr(p, end, (g_unwindFrom[0] != '\0') ? g_unwindFrom : "?");
            p = cgPutStr(p, end, "), 每次最多抓 ");
            p = cgPutDec(p, end, (long long)kMaxFrames);
            p = cgPutStr(p, end, " 层\n");
        } else {
            p = cgPutStr(p, end, "不可用(没找到 _Unwind_Backtrace): 只记录中断点寄存器与标记\n");
        }
        p = cgPutStr(p, end, "clk_tck   : ");
        p = cgPutDec(p, end, g_clkTck);
        p = cgPutStr(p, end, " ticks/秒(下面 Δcpu 用 ticks 表示, 1 秒 = ");
        p = cgPutDec(p, end, g_clkTck);
        p = cgPutStr(p, end, " ticks = 1 核)\n");
        cgHangWriteBuf(line, (int)(p - line));
    }

    for (int s = 0; s < kHangSamples; ++s) {
        const CgTask* ta = cgTaskOf(g_tasksA, nA, tid);
        const long long a = (ta != nullptr) ? ta->ticks : -1;
        g_hangStackTop = (ta != nullptr) ? ta->startStack : 0ULL;
        const int sampleNo = round * 100 + s + 1;

        cgSignalThread(tid, (s == 0) ? "target" : "target(第2/3次: 看是否卡在同一函数)", sampleNo);
        cgSleepMs(kHangSampleGapMs);

        const int nB = cgSnapTasks(g_tasksB, kMaxTasks);
        const CgTask* tb = cgTaskOf(g_tasksB, nB, tid);
        const long long b = (tb != nullptr) ? tb->ticks : -1;
        const long long d = (a >= 0 && b >= 0) ? (b - a) : -1;

        long long procDelta = 0;
        for (int i = 0; i < nB; ++i) {
            const CgTask* pa = cgTaskOf(g_tasksA, nA, g_tasksB[i].tid);
            if (pa != nullptr && g_tasksB[i].ticks > pa->ticks) {
                procDelta += g_tasksB[i].ticks - pa->ticks;
            }
        }
        if (totalFirst < 0) {
            totalFirst = procDelta;
        }
        totalLast = procDelta;

        {
            char line[512];
            char* p = line;
            char* end = line + sizeof(line) - 2;
            p = cgPutStr(p, end, "sample #");
            p = cgPutDec(p, end, sampleNo);
            p = cgPutStr(p, end, " : tid=");
            p = cgPutDec(p, end, (long long)tid);
            p = cgPutStr(p, end, " Δcpu=");
            p = cgPutDec(p, end, d);
            p = cgPutStr(p, end, " ticks(≈");
            p = cgPutDec(p, end, (d > 0) ? (d * 1000 / (g_clkTck > 0 ? g_clkTck : 100)) : 0);
            p = cgPutStr(p, end, "ms/1000ms) state=");
            p = cgPutStr(p, end, (tb != nullptr) ? &tb->state : "?");
            p = cgPutStr(p, end, " wchan=");
            p = cgPutStr(p, end, (tb != nullptr && tb->wchan[0] != '\0') ? tb->wchan : "-");
            p = cgPutStr(p, end, "  进程总计 Δcpu=");
            p = cgPutDec(p, end, procDelta);
            p = cgPutStr(p, end, " ticks\n         => ");
            p = cgPutStr(p, end, cgVerdict(d, (tb != nullptr) ? &tb->state : nullptr,
                                           (tb != nullptr) ? tb->wchan : nullptr));
            p = cgPutCh(p, end, '\n');
            cgHangWriteBuf(line, (int)(p - line));
        }

        long long busyDelta = 0;
        const int busy = cgFindBusiest(g_tasksA, nA, g_tasksB, nB, &busyDelta);
        if (busy > 0 && busy != tid && busyDelta > 0) {
            const CgTask* tbusy = cgTaskOf(g_tasksB, nB, busy);
            g_hangStackTop = (tbusy != nullptr) ? tbusy->startStack : 0ULL;
            char line[256];
            char* p = line;
            char* end = line + sizeof(line) - 2;
            p = cgPutStr(p, end, "busiest   : tid=");
            p = cgPutDec(p, end, (long long)busy);
            p = cgPutStr(p, end, " Δcpu=");
            p = cgPutDec(p, end, busyDelta);
            p = cgPutStr(p, end, " ticks(比标记线程更忙) -> 也采一枪\n");
            cgHangWriteBuf(line, (int)(p - line));
            cgSignalThread(busy, "busiest(cpu 增量最大)", sampleNo);
        }

        for (int i = 0; i < nB; ++i) {
            g_tasksA[i] = g_tasksB[i];
        }
        nA = nB;
        if (busy > 0 && busyDelta > 0) {
            tid = busy;   // 下一轮盯住真正在烧 CPU 的那个线程
        }
    }

    {
        char line[384];
        char* p = line;
        char* end = line + sizeof(line) - 2;
        p = cgPutStr(p, end, "round ");
        p = cgPutDec(p, end, round);
        p = cgPutStr(p, end, " 结束: 已采 ");
        p = cgPutDec(p, end, (long long)kHangSamples);
        p = cgPutStr(p, end, " 次(每次间隔 1 秒)。看 frame/sym 段里 3 次是否停在同一函数:");
        p = cgPutStr(p, end, "相同 => 稳定的死循环/自旋点; 每次都不同 => 正在推进或抖动。\n");
        p = cgPutStr(p, end, "注: 进程整体 Δcpu 首轮=");
        p = cgPutDec(p, end, totalFirst);
        p = cgPutStr(p, end, " 末轮=");
        p = cgPutDec(p, end, totalLast);
        p = cgPutStr(p, end, " ticks/秒\n");
        cgHangWriteBuf(line, (int)(p - line));
    }
}

void* cgWatchdogMain(void* arg)
{
    (void)arg;
    g_watchdogTid = (int)gettid();
    const long ck = sysconf(_SC_CLK_TCK);
    if (ck > 0) {
        g_clkTck = (long long)ck;
    }
    long long sampledMark = -1;
    int rounds = 0;
    for (;;) {
        cgSleepMs(5000);   // 平时: 只是睡 5 秒 + 读几个 volatile 变量, 零 IO
        const int seq = (int)g_seq;
        if ((seq & 1) != 0) {
            continue;
        }
        // 检查 1: 空闲不判卡死(2026-10-06)
        //   marker = "(无负载运行中: 上一项已结束) / (空闲)" 时, 那是两项之间的空档, 不是
        //   负载在跑。以前这里不看 idle, 于是空档超过 30 秒就被判"疑似卡死"并写出上百 KB 的
        //   假现场(真机: 已运行 32 秒 + tid 早已结束 errno=3)。
        if (g_idle != 0) {
            continue;
        }
        const long long markMs = g_markMs;
        const int tid = (int)g_markTid;
        if (markMs <= 0 || tid <= 0) {
            continue;
        }
        const long long elapsed = cgNowMs() - markMs;
        if (elapsed < (long long)kHangThresholdMs) {
            continue;
        }
        if (markMs == sampledMark) {
            continue;   // 同一项已经采过, 不重复刷盘
        }
        if (rounds >= kMaxHangRounds) {
            continue;   // 全进程上限, 防止一直卡死时把盘写爆
        }
        // 检查 2: 目标线程必须仍然存在(2026-10-06)
        //   标记里的 tid 可能早已退出(项跑完 + 线程被回收)。这时"抓不到栈"不是卡死: 只记
        //   一行"跳过原因"(几十字节), 不写现场、不投信号 —— 真机上原来的 112 KB 就来自这里:
        //   目标 tid 已经消失, 看门狗却仍然枚举全进程、采样 3 轮、并把 busiest 线程的栈全写下来。
        if (!cgThreadAlive(tid)) {
            sampledMark = markMs;
            ++rounds;
            char line[224];
            char* p = line;
            char* end = line + sizeof(line) - 2;
            p = cgPutStr(p, end, "skip round ");
            p = cgPutDec(p, end, (long long)rounds);
            p = cgPutStr(p, end, ": marker tid=");
            p = cgPutDec(p, end, (long long)tid);
            p = cgPutStr(p, end, " 已不存在(项目结束/线程已回收), 标记已 ");
            p = cgPutDec(p, end, elapsed / 1000);
            p = cgPutStr(p, end, " 秒 => 不判定卡死、不写现场(native_hang.txt 只留这一行)\n");
            cgHangWriteBuf(line, (int)(p - line));
            continue;
        }
        sampledMark = markMs;
        ++rounds;
        cgHangRound(rounds, tid, elapsed);
    }
    return nullptr;
}

int cgStartWatchdog()
{
    if (g_hangThreadStarted != 0) {
        return 1;
    }
    const long ck = sysconf(_SC_CLK_TCK);
    if (ck > 0) {
        g_clkTck = (long long)ck;
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return 0;
    }
    (void)pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t th;
    const int rc = pthread_create(&th, &attr, cgWatchdogMain, nullptr);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        return 0;
    }
    (void)pthread_detach(th);
    g_hangThreadStarted = 1;
    return 1;
}

// 一行状态(给 ArkTS 自检显示用)
void cgHangStatusLine(char* buf, int cap)
{
    if (buf == nullptr || cap <= 1) {
        return;
    }
    char phase[kPhaseCap];
    char item[kItemCap];
    cgCopyBounded(phase, kPhaseCap, g_phase);
    cgCopyBounded(item, kItemCap, g_item);
    const long long markMs = g_markMs;
    const long long now = cgNowMs();
    char* p = buf;
    char* end = buf + cap - 2;
    p = cgPutStr(p, end, "watchdog=");
    p = cgPutStr(p, end, (g_hangThreadStarted != 0) ? "on" : "off");
    p = cgPutStr(p, end, " threshold_s=");
    p = cgPutDec(p, end, (long long)(kHangThresholdMs / 1000));
    p = cgPutStr(p, end, " samples_done=");
    p = cgPutDec(p, end, (long long)g_hangDone);
    p = cgPutStr(p, end, " unwinder=");
    p = cgPutStr(p, end, (g_unwindBacktrace != nullptr) ? ((g_unwindFrom[0] != '\0') ? g_unwindFrom : "ok") : "none");
    p = cgPutStr(p, end, " marker_tid=");
    p = cgPutDec(p, end, (long long)g_markTid);
    // idle=1 表示当前标记是"无负载运行中"(空档): 看门狗据此不判卡死(见 cgWatchdogMain)。
    p = cgPutStr(p, end, " idle=");
    p = cgPutDec(p, end, (long long)((g_idle != 0) ? 1 : 0));
    p = cgPutStr(p, end, " bytes=");
    p = cgPutDec(p, end, g_hangBytes);
    p = cgPutStr(p, end, " running_s=");
    p = cgPutDec(p, end, (markMs > 0) ? ((now - markMs) / 1000) : 0);
    p = cgPutStr(p, end, " item=\"");
    p = cgPutStr(p, end, item);
    p = cgPutStr(p, end, "\" phase=\"");
    p = cgPutStr(p, end, phase);
    p = cgPutCh(p, end, '"');
    if (g_hangPath[0] != '\0') {
        p = cgPutStr(p, end, " hang_log=");
        p = cgPutStr(p, end, g_hangPath);
    }
    *p = '\0';
}

// 自检: 起一个“空转 45 秒”的线程并把它标成当前项, 让看门狗在 30 秒后自动抓它。
void* cgSelfTestThread(void* arg)
{
    (void)arg;
    const int secs = (g_selfTestSecs > 0) ? g_selfTestSecs : 45;
    auroraSetCurrentItem("(看门狗自检)", "busy spin", 1, 1);
    volatile unsigned long x = 0;
    const long long stop = cgNowMs() + (long long)secs * 1000;
    while (cgNowMs() < stop) {
        for (int i = 0; i < 200000; ++i) {
            x = x + (unsigned long)i;
        }
    }
    auroraSetIdleMarker();   // 空闲标记: 必须走这个入口(它会把 g_idle 置 1)
    return nullptr;
}

int cgStartHangSelfTest(int seconds)
{
    if (seconds <= 0 || seconds > 600) {
        seconds = 45;
    }
    g_selfTestSecs = seconds;
    pthread_t th;
    if (pthread_create(&th, nullptr, cgSelfTestThread, nullptr) != 0) {
        return 0;
    }
    (void)pthread_detach(th);
    return 1;
}

// 读证据文件尾部(和崩溃报告同一套规则: 太大只回最后 cap-1 字节并丢掉首行残片)
int cgReadReportTail(const char* path, char* buf, int cap)
{
    if (buf == nullptr || cap <= 1 || path == nullptr || path[0] == '\0') {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    const long long size = (long long)lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        close(fd);
        return -1;
    }
    const int want = cap - 1;
    long long off = 0;
    bool truncated = false;
    if (size > (long long)want) {
        off = size - (long long)want;
        truncated = true;
    }
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    int total = 0;
    while (total < want) {
        ssize_t r = read(fd, buf + total, (size_t)(want - total));
        if (r > 0) {
            total += (int)r;
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fd);
    buf[total] = '\0';
    if (truncated) {
        int k = 0;
        while (k < total && buf[k] != '\n') {
            ++k;
        }
        if (k < total) {
            const int rest = total - (k + 1);
            for (int i = 0; i < rest; ++i) {
                buf[i] = buf[k + 1 + i];
            }
            total = rest;
            buf[total] = '\0';
        }
    }
    return total;
}

} // namespace

// ===========================================================================
//  卡死取证的 C 接口
// ===========================================================================

extern "C" const char* auroraHangLogPath()
{
    return g_hangPath;
}

extern "C" int auroraHangWatchdogAlive()
{
    return (g_hangThreadStarted != 0) ? 1 : 0;
}

extern "C" int auroraHangStatus(char* buf, int cap)
{
    if (buf == nullptr || cap <= 1) {
        return -1;
    }
    cgHangStatusLine(buf, cap);
    int n = 0;
    while (n < cap - 1 && buf[n] != '\0') {
        ++n;
    }
    return n;
}

// 读 native_hang.txt 尾部; -1 = 还没有任何卡死记录。
extern "C" int auroraTakeHangReport(char* buf, int cap)
{
    return cgReadReportTail(g_hangPath, buf, cap);
}

extern "C" int auroraHangReportExists()
{
    if (g_hangPath[0] == '\0') {
        return 0;
    }
    return access(g_hangPath, F_OK) == 0 ? 1 : 0;
}

// 用 ftruncate 清空(保持常驻 fd 可用)
extern "C" void auroraClearHangReport()
{
    const int fd = (int)g_hangFd;
    if (fd >= 0) {
        (void)ftruncate(fd, 0);
        return;
    }
    if (g_hangPath[0] != '\0') {
        (void)unlink(g_hangPath);
    }
}

// 自检: 起一个空转线程(默认 45 秒)并标成当前项 -> 30 秒后看门狗会自动抓它的栈
extern "C" int auroraHangSelfTest(int seconds)
{
    return cgStartHangSelfTest(seconds);
}


