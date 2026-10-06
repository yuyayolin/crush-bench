#ifndef AURORA_CPU_CORE_STATE_H
#define AURORA_CPU_CORE_STATE_H

// ===========================================================================
//  逐核"启动状态"取证: 为什么有核没有启动 / 为什么有线程没有跑
// ===========================================================================
//
//  被回答的原话问题
//  ---------------------------------------------------------------------------
//    "它是有 9 核 14 线程, 你应该去看为什么有核没有启动, 为什么有线程没有跑"
//
//  已经确定的矛盾(真机实测; 具体机型与 SoC 型号不写进本文件 —— 本节是纯行为学判据, 与设备无关)
//  ---------------------------------------------------------------------------
//    三个来源互相打架, 而"不可用"的性质完全不同:
//      * /sys/devices/system/cpu/present 报 0-8   (9 个核)
//      * /proc/self/status 的 Cpus_allowed 报 0-13 (14 个核)
//      * 逐核 sched_setaffinity + 立刻读回报 0-7    (8 个核)
//    "有核没启动"可能是四种性质完全不同的原因, 必须分清, 不能用一句"用不了"带过:
//      (a) 核处于 offline 状态(热插拔下线)        -> 本文件 B/C 类判据
//      (b) 被 cpuset / cgroup 拒绝                -> 本文件 B 类判据
//      (c) 被厂商策略拒绝                         -> 本文件 B 类判据(靠 errno 分辨)
//      (d) 被热/功耗策略临时关掉                  -> 本文件 B 类判据(靠 errno 分辨)
//
//  本文件的判据(全部是读数, 没有一条靠推测)
//  ---------------------------------------------------------------------------
//    对每一个核号 c = 0..上界-1 逐项记录(原文 + errno):
//      1) /sys/devices/system/cpu/cpuN/online            (1=在线 0=下线; 打不开=不支持热插拔 + errno)
//      2) /sys/devices/system/cpu/{present,possible,online} 三个全局文件的原文
//      3) cpuN/cpufreq/{cpuinfo_max_freq,scaling_cur_freq}   原文 + errno
//      4) cpuN/topology/{core_id,physical_package_id,thread_siblings_list} 原文 + errno
//      5) 逐核 sched_setaffinity({c}) + 立刻读回 + errno(与 cpu_affinity.h 的实测许可探测同一口径)
//      6) 对 online 可写的核尝试写 '1'(先备份原值, 无论成败都恢复), 记下返回值与 errno
//    然后按下面四条把每个核归入且仅归入一类:
//      A 在线且被内核允许(可用)             判据: 该核出现在 possible/present 的读数里
//                                            且逐核实测 setaffinity 被接受
//      B 在线但被本进程的许可集合拒绝        判据: present 含该核, 但实测 setaffinity 被拒
//                                            (errno=EINVAL/EPERM 或"返回 0 但读回被夹回")
//      C 离线(offline) —— 核没有启动         判据: present 含该核, 而 cpuN/online 原文为 "0"
//                                            (或该文件不存在但 present 含它)
//      D present 里根本没有                  判据: present 的原文不含该核号
//    判据的顺序固定为: D -> C -> B -> A(先按 present 划边界, 再用实测结果修正)。
//
//  为什么"实测"是必要的(而不是只看 sched_getaffinity)
//  ---------------------------------------------------------------------------
//    sched_getaffinity 是"内核此刻报给我的掩码", /proc/self/status 的 Cpus_allowed 是
//    线程组组长线程的掩码 —— 两者本来就可能各说各话(真机日志里已出现过这种不一致)。
//    只有"对每个核号单独发一次 sched_setaffinity({c}) 再立刻读回"才是行为学的判据:
//    它测的就是调用线程的许可集合, 不依赖任何文件的解释。
//
//  拉起下线核(判据 6)的安全规则 —— 三条, 一条都不能省
//  ---------------------------------------------------------------------------
//    * 写之前先备份原值(读一次 cpuN/online 的原文);
//    * 无论写成功还是失败, 探测结束无条件恢复: 原来是什么就写回什么(只有在原值与
//      探测后的读回值确实不同时才回写, 避免多余的写操作);
//    * 写失败要记 errno(EACCES=13 权限不足 / EPERM=1 操作不允许 / EROFS=30 只读 /
//      ENOENT=2 文件不存在 / EBUSY=16 占用中 …), 并明确写下"应用域无权拉起下线核" ——
//      不写成含糊的"拉不起来"。errno 本身就是证据, 不许丢掉。
//
//  跨平台一致性声明(必须保持)
//  ---------------------------------------------------------------------------
//    * 本节只读 sysfs + 一次逐核 setaffinity 探测 + 一次 online 写探测, 全部发生在
//      计时区间之外(会话开始之前); 不改任何负载的工作量 / 数据规模 / 循环次数 / 计分公式,
//      不引入任何设备相关系数, 不按机型/SoC 分任何支。
//    * 不做任何绕过内核策略的事: 被内核拒绝的核(B 类)只上报, 不尝试任何"绕开"
//      手段(不写 cgroup 的 cpuset.cpus, 不改 /proc/self/status, 不伪造掩码)。
//    * 读不到就是读不到: 每个文件、每一次 syscall 都带 errno 一起上报, 不假装成功,
//      也不为了"看起来跑满"而编造数字。
// ===========================================================================

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// 逐核在线状态分类(每个核归入且仅归入一类)
enum {
    AURORA_CORE_OFFLINE_STATE = 0,   // C 类: 离线(offline) —— 核没有启动
    AURORA_CORE_ONLINE_STATE  = 1,   // 在线(online 原文为 1)
    AURORA_CORE_ABSENT_STATE  = 2    // present 里根本没有这个核号
};

// 逐核最终分类(A/B/C/D)
enum {
    AURORA_CORE_UNKNOWN_CLASS = 0,
    AURORA_CORE_CLASS_A = 1,   // 在线且被内核允许(可用)
    AURORA_CORE_CLASS_B = 2,   // 在线但被本进程的许可集合拒绝(被挡住)
    AURORA_CORE_CLASS_C = 3,   // 离线(offline) —— 核没有启动
    AURORA_CORE_CLASS_D = 4    // present 里根本没有
};

struct AuroraCoreState {
    const char* name;          // "A"/"B"/"C"/"D"
    int         usable;        // 1 = 本进程可用
    const char* desc;
};

inline const AuroraCoreState& auroraCoreStateOf(int cls)
{
    static const AuroraCoreState kStates[5] = {
        { "?", 0, "未分类" },
        { "A", 1, "在线且被内核允许(可用)" },
        { "B", 0, "在线但被本进程的许可集合拒绝(被 cpuset/cgroup 或厂商/热功耗策略挡住)" },
        { "C", 0, "离线(offline) —— 核没有启动(热插拔下线)" },
        { "D", 0, "present 里根本没有该核号" }
    };
    if (cls < 0 || cls > 4) {
        cls = 0;
    }
    return kStates[cls];
}

// 核号上界(与 cpu_affinity.h 的 kMaxTopoCpus = 32 保持同一口径, 却不引用它 ——
// 本文件要能被任何翻译单元独立 include)
constexpr int kMaxCoreStateCpus = 32;

// 一行一行的小工具(无分配; 超容量就停在那里, 不越界)
inline void auroraCoreStateAppend(char* dst, size_t cap, const char* seg)
{
    if (dst == nullptr || cap < 2 || seg == nullptr || seg[0] == '\0') {
        return;
    }
    const size_t used = strlen(dst);
    if (used + 1 >= cap) {
        return;
    }
    strncat(dst, seg, cap - 1 - used);
}

// 读一个小文本文件: 成功返回 1 并把原文放进 buf(换行/制表换成空格, 尾空格去掉);
// 打不开/读空返回 0, 其中 *errOut = errno(读空记 -1)。空文件也算"读到了但没内容" -> -1。
inline int auroraCoreStateReadRaw(const char* path, char* buf, int cap, int* errOut)
{
    if (errOut != nullptr) {
        *errOut = 0;
    }
    if (buf == nullptr || cap <= 1) {
        return 0;
    }
    buf[0] = '\0';
    errno = 0;
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        if (errOut != nullptr) {
            *errOut = (errno != 0) ? errno : -1;
        }
        return 0;
    }
    const size_t n = fread(buf, 1, (size_t)(cap - 1), f);
    fclose(f);
    buf[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') {
            buf[i] = ' ';
        }
    }
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }
    if (len == 0) {
        if (errOut != nullptr) {
            *errOut = -1;   // 打得开但读不到内容: 记 -1(不是 errno)
        }
        return 0;
    }
    return 1;
}

// 把 "0-3,8-11" 这样的 cpulist 解析成位图(位 c = 核号 c 在集合里)。
// 语法异常(区间反了 / 数字过大)时跳过那一段而不是猜 —— 与 cpu_affinity.h 的
// parseCpuMask 同一取舍: 宁可少认一个核, 不许把不存在的核当成存在。
inline unsigned long long auroraCoreStateParseCpuList(const char* s, int* countOut)
{
    unsigned long long m = 0ull;
    if (countOut != nullptr) {
        *countOut = 0;
    }
    if (s == nullptr) {
        return 0ull;
    }
    int i = 0;
    while (s[i] != '\0') {
        if (s[i] < '0' || s[i] > '9') {
            ++i;
            continue;
        }
        long a = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            a = a * 10 + (s[i] - '0');
            if (a > 4096) {
                a = 4096;
            }
            ++i;
        }
        long b = a;
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
        if (b < a || a > 4095) {
            continue;
        }
        for (long c = a; c <= b; ++c) {
            m |= (1ull << (unsigned)c);
        }
    }
    if (countOut != nullptr) {
        int n = 0;
        for (int c = 0; c < 64; ++c) {
            if (((m >> (unsigned)c) & 1ull) != 0) {
                ++n;
            }
        }
        *countOut = n;
    }
    return m;
}

// 位图 -> "cpu0,cpu1,..." 文本(只列出前 64 位)
inline void auroraCoreStateMaskText(unsigned long long m, char* out, int cap)
{
    if (out == nullptr || cap < 2) {
        return;
    }
    out[0] = '\0';
    int n = 0;
    for (int c = 0; c < 64; ++c) {
        if (((m >> (unsigned)c) & 1ull) == 0) {
            continue;
        }
        char one[16];
        snprintf(one, sizeof(one), "%scpu%d", (n > 0) ? "," : "", c);
        if ((int)strlen(out) + (int)strlen(one) >= cap - 1) {
            break;
        }
        strncat(out, one, (size_t)(cap - 1 - (int)strlen(out)));
        ++n;
    }
    if (out[0] == '\0') {
        snprintf(out, (size_t)cap, "无");
    }
}

struct AuroraCoreStateProbe {
    int             ran;                     // 1 = 探测执行过
    int             tid;                     // 被探测的线程(gettid)
    int             upper;                   // 探测的核号范围 0..upper-1
    int             upperSrc;                // 上界来源(与 cpu_affinity.h 的 probeUpperSrcText 同一口径)
    char            presentRaw[128];         // /sys/devices/system/cpu/present 原文
    int             presentErrno;
    char            possibleRaw[128];        // /sys/devices/system/cpu/possible 原文
    int             possibleErrno;
    char            onlineRaw[128];          // /sys/devices/system/cpu/online 原文(全局那个)
    int             globalOnlineErrno;       // 它读不到时的 errno(与逐核的 onlineErrno[] 区分开)
    unsigned long long presentMask;
    unsigned long long possibleMask;
    unsigned long long onlineMask;
    int             presentCount;            // present 的核数
    int             possibleCount;
    int             onlineCount;             // 全局 online 文件里的核数
    // ---- 逐核记录(下标 = 核号; 只覆盖 0..kMaxCoreStateCpus-1) ----
    int             hasPresent[kMaxCoreStateCpus];   // 1 = present 原文含该核号
    int             hasPossible[kMaxCoreStateCpus];
    int             hasOnlineGlobal[kMaxCoreStateCpus];  // 1 = 全局 online 原文含该核号
    int             onlineKnown[kMaxCoreStateCpus];  // 1 = cpuN/online 文件读到了
    int             onlineValue[kMaxCoreStateCpus];  // 0/1(onlineKnown==0 时无意义)
    char            onlineRawPer[kMaxCoreStateCpus][8];  // cpuN/online 的原文("0"/"1")
    int             onlineErrno[kMaxCoreStateCpus];
    int             freqKnown[kMaxCoreStateCpus];    // 1 = cpuinfo_max_freq 读到了
    char            freqRaw[kMaxCoreStateCpus][16];  // 原文
    int             freqErrno[kMaxCoreStateCpus];
    int             curFreqKnown[kMaxCoreStateCpus]; // 1 = scaling_cur_freq 读到了
    char            curFreqRaw[kMaxCoreStateCpus][16];
    int             curFreqErrno[kMaxCoreStateCpus];
    int             coreIdKnown[kMaxCoreStateCpus];
    char            coreIdRaw[kMaxCoreStateCpus][8];
    int             coreIdErrno[kMaxCoreStateCpus];
    int             pkgKnown[kMaxCoreStateCpus];
    char            pkgRaw[kMaxCoreStateCpus][8];
    int             pkgErrno[kMaxCoreStateCpus];
    int             sibKnown[kMaxCoreStateCpus];
    char            sibRaw[kMaxCoreStateCpus][40];   // thread_siblings_list 原文
    int             sibErrno[kMaxCoreStateCpus];
    int             accepted[kMaxCoreStateCpus];     // 1 = 实测 setaffinity({c}) 被内核接受
    int             clamped[kMaxCoreStateCpus];      // 1 = 返回 0 但读回不是那一位(被夹回)
    int             errnoOf[kMaxCoreStateCpus];      // 被拒时的 errno(0=接受; -1=读回失败; -4=夹回)
    int             cls[kMaxCoreStateCpus];          // 最终分类 A/B/C/D
    int             clsByProbe[kMaxCoreStateCpus];   // 1 = 分类被"实测"修正过(不是仅按 present 归的)
    // ---- C 类拉起探测(写 cpuN/online = '1', 先备份后恢复) ----
    int             onlineProbeable[kMaxCoreStateCpus];   // 1 = cpuN/online 文件存在(支持热插拔)
    int             offlineWritable[kMaxCoreStateCpus];   // 1 = 该核的 online 文件打开成功(可写判定见下)
    int             onlineOpenErrno[kMaxCoreStateCpus];   // 打不开时的 errno(关键证据: 13=EACCES 1=EPERM 30=EROFS)
    int             onlineBackupOk[kMaxCoreStateCpus];    // 1 = 写之前备份成功
    int             writeAttempted[kMaxCoreStateCpus];    // 1 = 真的尝试过写 '1'
    int             writeRc[kMaxCoreStateCpus];           // fwrite 返回值(0 = 失败)
    int             writeErrno[kMaxCoreStateCpus];
    int             onlineAfterWrite[kMaxCoreStateCpus];  // 写后读回(1=在线 0=仍离线 -1=读不到)
    int             restoreOk[kMaxCoreStateCpus];         // 1 = 恢复原值成功
    // ---- 汇总 ----
    int             countA;
    int             countB;
    int             countC;
    int             countD;
    unsigned long long maskA;
    unsigned long long maskB;
    unsigned long long maskC;
    unsigned long long maskD;
    int             cWriteOk;                // C 类里"写 '1' 之后真的变成 1"的核数
    int             cWriteFail;              // C 类里写失败的核数
    int             cLastWriteErrno;         // 最后一次写失败的 errno(0 = 没有失败)
    int             cLastOpenErrno;          // 最后一次打开 online 失败的 errno(0 = 没有失败)
    char            cVerdict[256];           // "应用域能不能把 C 类拉起来"的结论
    char            perCore[1280];           // 逐核一行(核号: online 原文 / 实测 / 频率 / 分类)
    char            writeTable[768];         // 逐核写探测表(备份 -> 写 -> 读回 -> 恢复)
    char            text[3584];              // 完整一行(可直接进 note)
    AuroraCoreStateProbe()
        : ran(0), tid(-1), upper(0), upperSrc(-1),
          presentRaw(), presentErrno(0), possibleRaw(), possibleErrno(0),
          onlineRaw(), globalOnlineErrno(0),
          presentMask(0ull), possibleMask(0ull), onlineMask(0ull),
          presentCount(0), possibleCount(0), onlineCount(0),
          hasPresent(), hasPossible(), hasOnlineGlobal(),
          onlineKnown(), onlineValue(), onlineRawPer(), onlineErrno(),
          freqKnown(), freqRaw(), freqErrno(),
          curFreqKnown(), curFreqRaw(), curFreqErrno(),
          coreIdKnown(), coreIdRaw(), coreIdErrno(),
          pkgKnown(), pkgRaw(), pkgErrno(),
          sibKnown(), sibRaw(), sibErrno(),
          accepted(), clamped(), errnoOf(), cls(), clsByProbe(),
          onlineProbeable(), offlineWritable(), onlineOpenErrno(),
          onlineBackupOk(), writeAttempted(), writeRc(), writeErrno(),
          onlineAfterWrite(), restoreOk(),
          countA(0), countB(0), countC(0), countD(0),
          maskA(0ull), maskB(0ull), maskC(0ull), maskD(0ull),
          cWriteOk(0), cWriteFail(0), cLastWriteErrno(0), cLastOpenErrno(0),
          cVerdict(), perCore(), writeTable(), text()
    {
    }
};

// ---------------------------------------------------------------------------
//  核号上界(与 cpu_affinity.h 的 probeCpuUpperBound 同一口径): 取"present / possible /
//  频率表长度 / /proc/cpuinfo 行数 / sysconf(_SC_NPROCESSORS_ONLN)"五者的最大值。
//  为什么取最大: 本次事故的形态正是"编号空间太窄"—— 频率表在某个核处 break, 后面的核
//  在整套绑核逻辑里根本不存在。取最大保证"真实存在的核"不会因某一处读数失败而被漏掉。
// ---------------------------------------------------------------------------
inline int auroraCoreStateUpperBound(int* srcOut)
{
    int best = 0;
    int src = -1;
    {
        char buf[128];
        int e = 0;
        if (auroraCoreStateReadRaw("/sys/devices/system/cpu/present", buf, (int)sizeof(buf), &e)) {
            int n = 0;
            (void)auroraCoreStateParseCpuList(buf, &n);
            if (n > best) {
                best = n;
                src = 0;
            }
        }
    }
    {
        char buf[128];
        int e = 0;
        if (auroraCoreStateReadRaw("/sys/devices/system/cpu/possible", buf, (int)sizeof(buf), &e)) {
            int n = 0;
            const unsigned long long m = auroraCoreStateParseCpuList(buf, &n);
            for (int c = 0; c < 64; ++c) {
                if (((m >> (unsigned)c) & 1ull) != 0 && c + 1 > best) {
                    best = c + 1;
                    src = 1;
                }
            }
        }
    }
    {
        int n = 0;
        errno = 0;
        FILE* f = fopen("/proc/cpuinfo", "r");
        if (f != nullptr) {
            char line[256];
            while (fgets(line, (int)sizeof(line), f) != nullptr) {
                if (strncmp(line, "processor", 9) == 0) {
                    ++n;
                }
            }
            fclose(f);
        }
        if (n > best) {
            best = n;
            src = 2;
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
        best = 8;     // 五个来源全部读不到: 仍探测 0..7(有界、便宜), 并标注来源未知
        src = -1;
    }
    if (best > kMaxCoreStateCpus) {
        best = kMaxCoreStateCpus;
    }
    if (srcOut != nullptr) {
        *srcOut = src;
    }
    return best;
}

inline const char* auroraCoreStateUpperSrcText(int src)
{
    switch (src) {
        case 0:  return "/sys/devices/system/cpu/present";
        case 1:  return "/sys/devices/system/cpu/possible";
        case 2:  return "/proc/cpuinfo 的 processor 行数";
        case 3:  return "sysconf(_SC_NPROCESSORS_ONLN)";
        default: return "五个来源都读不到(兜底探测 0..7)";
    }
}

// ---------------------------------------------------------------------------
//  唯一入口 逐核"启动状态"取证 + A/B/C/D 分类 + C 类拉起探测(先备份, 无论成败都恢复)
//
//  调用时机: 会话开始之前(auroraAffinitySessionBegin 内), 完全在计时区间之外。
//  本函数只读 sysfs + 发一次逐核 sched_setaffinity 探测 + 一次 online 写探测;
//  它不改任何负载的工作量/尺寸/线程数, 也不计分。
// ---------------------------------------------------------------------------
inline AuroraCoreStateProbe probeCoreStartupState()
{
    AuroraCoreStateProbe p;
    p.ran = 1;
    p.tid = (int)::syscall(SYS_gettid);
    p.upper = auroraCoreStateUpperBound(&p.upperSrc);

    // ---- (2) 三个全局文件的原文 + errno ----
    {
        int e = 0;
        if (auroraCoreStateReadRaw("/sys/devices/system/cpu/present", p.presentRaw,
                                  (int)sizeof(p.presentRaw), &e)) {
            p.presentErrno = 0;
            p.presentMask = auroraCoreStateParseCpuList(p.presentRaw, &p.presentCount);
        } else {
            p.presentErrno = e;
            snprintf(p.presentRaw, sizeof(p.presentRaw), "(读不到)");
        }
    }
    {
        int e = 0;
        if (auroraCoreStateReadRaw("/sys/devices/system/cpu/possible", p.possibleRaw,
                                  (int)sizeof(p.possibleRaw), &e)) {
            p.possibleErrno = 0;
            p.possibleMask = auroraCoreStateParseCpuList(p.possibleRaw, &p.possibleCount);
        } else {
            p.possibleErrno = e;
            snprintf(p.possibleRaw, sizeof(p.possibleRaw), "(读不到)");
        }
    }
    {
        int e = 0;
        if (auroraCoreStateReadRaw("/sys/devices/system/cpu/online", p.onlineRaw,
                                  (int)sizeof(p.onlineRaw), &e)) {
            p.globalOnlineErrno = 0;
            p.onlineMask = auroraCoreStateParseCpuList(p.onlineRaw, &p.onlineCount);
        } else {
            p.globalOnlineErrno = e;
            snprintf(p.onlineRaw, sizeof(p.onlineRaw), "(读不到)");
        }
    }
    for (int c = 0; c < kMaxCoreStateCpus; ++c) {
        p.hasPresent[c] = (((p.presentMask >> (unsigned)c) & 1ull) != 0) ? 1 : 0;
        p.hasPossible[c] = (((p.possibleMask >> (unsigned)c) & 1ull) != 0) ? 1 : 0;
        p.hasOnlineGlobal[c] = (((p.onlineMask >> (unsigned)c) & 1ull) != 0) ? 1 : 0;
    }

    // ---- 先备份原掩码(探测结束后无条件还原) ----
    unsigned long long savedMask = 0ull;
    int savedOk = 0;
    {
        cpu_set_t cur;
        CPU_ZERO(&cur);
        errno = 0;
        if (sched_getaffinity(0, sizeof(cur), &cur) == 0) {
            savedOk = 1;
            for (int c = 0; c < 64; ++c) {
                if (CPU_ISSET(c, &cur)) {
                    savedMask |= (1ull << (unsigned)c);
                }
            }
        }
    }

    // ---- 逐核读盘 + 逐核实测(1)(3)(4)(5) ----
    for (int c = 0; c < p.upper && c < kMaxCoreStateCpus; ++c) {
        char path[160];
        char buf[128];
        int e = 0;
        const std::string dir = std::string("/sys/devices/system/cpu/cpu") + std::to_string(c) + "/";
        // (1) cpuN/online
        if (auroraCoreStateReadRaw((dir + "online").c_str(), buf, (int)sizeof(buf), &e)) {
            p.onlineKnown[c] = 1;
            p.onlineProbeable[c] = 1;
            p.onlineValue[c] = (buf[0] == '1') ? 1 : 0;
            snprintf(p.onlineRawPer[c], sizeof(p.onlineRawPer[c]), "%s", buf);
            p.onlineErrno[c] = 0;
        } else {
            p.onlineErrno[c] = e;
            snprintf(p.onlineRawPer[c], sizeof(p.onlineRawPer[c]), "%s",
                     (e == 2) ? "(文件不存在: 不支持热插拔)" : "(读不到)");
        }
        // (3) 逐核频率(原文 + errno)
        if (auroraCoreStateReadRaw((dir + "cpufreq/cpuinfo_max_freq").c_str(), buf,
                                   (int)sizeof(buf), &e)) {
            p.freqKnown[c] = 1;
            snprintf(p.freqRaw[c], sizeof(p.freqRaw[c]), "%s", buf);
            p.freqErrno[c] = 0;
        } else {
            p.freqErrno[c] = e;
            snprintf(p.freqRaw[c], sizeof(p.freqRaw[c]), "(读不到)");
        }
        if (auroraCoreStateReadRaw((dir + "cpufreq/scaling_cur_freq").c_str(), buf,
                                   (int)sizeof(buf), &e)) {
            p.curFreqKnown[c] = 1;
            snprintf(p.curFreqRaw[c], sizeof(p.curFreqRaw[c]), "%s", buf);
            p.curFreqErrno[c] = 0;
        } else {
            p.curFreqErrno[c] = e;
            snprintf(p.curFreqRaw[c], sizeof(p.curFreqRaw[c]), "(读不到)");
        }
        // (4) 拓扑三件套(原文 + errno)
        if (auroraCoreStateReadRaw((dir + "topology/core_id").c_str(), buf, (int)sizeof(buf), &e)) {
            p.coreIdKnown[c] = 1;
            snprintf(p.coreIdRaw[c], sizeof(p.coreIdRaw[c]), "%s", buf);
            p.coreIdErrno[c] = 0;
        } else {
            p.coreIdErrno[c] = e;
            snprintf(p.coreIdRaw[c], sizeof(p.coreIdRaw[c]), "(读不到)");
        }
        if (auroraCoreStateReadRaw((dir + "topology/physical_package_id").c_str(), buf,
                                   (int)sizeof(buf), &e)) {
            p.pkgKnown[c] = 1;
            snprintf(p.pkgRaw[c], sizeof(p.pkgRaw[c]), "%s", buf);
            p.pkgErrno[c] = 0;
        } else {
            p.pkgErrno[c] = e;
            snprintf(p.pkgRaw[c], sizeof(p.pkgRaw[c]), "(读不到)");
        }
        if (auroraCoreStateReadRaw((dir + "topology/thread_siblings_list").c_str(), buf,
                                   (int)sizeof(buf), &e)) {
            p.sibKnown[c] = 1;
            snprintf(p.sibRaw[c], sizeof(p.sibRaw[c]), "%s", buf);
            p.sibErrno[c] = 0;
        } else {
            p.sibErrno[c] = e;
            snprintf(p.sibRaw[c], sizeof(p.sibRaw[c]), "(读不到)");
        }
        // (5) 逐核实测: sched_setaffinity({c}) + 立刻读回
        {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(c, &one);
            errno = 0;
            const int rc = sched_setaffinity(0, sizeof(one), &one);
            const int setErrno = (rc != 0) ? ((errno != 0) ? errno : -1) : 0;
            cpu_set_t back;
            CPU_ZERO(&back);
            errno = 0;
            const int rc2 = sched_getaffinity(0, sizeof(back), &back);
            unsigned long long backMask = 0ull;
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
            } else if (rc != 0) {
                p.errnoOf[c] = setErrno;
            } else if (rc2 != 0) {
                p.errnoOf[c] = -1;      // 返回成功但读回失败
            } else {
                p.errnoOf[c] = -4;      // 返回 0 但读回掩码不是那一位 = 被内核静默夹回
                p.clamped[c] = 1;
            }
        }
    }

    // ---- 分类(判据顺序: D -> C -> B -> A; 再用实测修正) ----
    for (int c = 0; c < p.upper && c < kMaxCoreStateCpus; ++c) {
        if (p.hasPresent[c] == 0) {
            p.cls[c] = AURORA_CORE_CLASS_D;      // present 的原文不含这个核号
            continue;
        }
        if (p.onlineKnown[c] == 1 && p.onlineValue[c] == 0) {
            // 核在 present 里, 但它的 online 文件明确写着 0: 核处于 offline(热插拔下线)
            p.cls[c] = AURORA_CORE_CLASS_C;
            // 即便 online=0, 也把实测结果记下来(有些平台 offline 核的掩码仍被接受, 反之亦然),
            // 但只要 online 原文是 0, 就仍然归 C 类 —— "没启动"是比"掩码被接受"更高一级的事实。
            continue;
        }
        if (p.accepted[c] != 0) {
            p.cls[c] = AURORA_CORE_CLASS_A;      // 在线且被内核允许
        } else {
            p.cls[c] = AURORA_CORE_CLASS_B;      // 在线但被本进程的许可集合拒绝
        }
        // 实测修正的可见化: 若某核 online 文件读不到(不支持热插拔), 就完全靠实测/全局读数定案,
        // 这时标 clsByProbe = 1, 让日志里一眼能看出"这一条依据的是行为学判据"。
        p.clsByProbe[c] = (p.onlineKnown[c] == 0) ? 1 : 0;
    }

    // ---- (6) C 类拉起探测: 对 online 文件存在的核尝试写 '1'; 先备份, 无论成败都恢复 ----
    for (int c = 0; c < p.upper && c < kMaxCoreStateCpus; ++c) {
        if (p.onlineProbeable[c] == 0) {
            continue;      // 没有 online 文件 = 不支持热插拔, 写也没有意义
        }
        char path[160];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", c);
        // 备份原值(必须: 恢复时要写回它)
        char backup[16];
        int be = 0;
        if (auroraCoreStateReadRaw(path, backup, (int)sizeof(backup), &be)) {
            p.onlineBackupOk[c] = 1;
        } else {
            snprintf(backup, sizeof(backup), "%d", p.onlineValue[c]);
            p.onlineBackupOk[c] = 0;
        }
        // 打开(只写): 这一步的 errno 就是"应用域到底有没有权限动这个核"的直接证据
        errno = 0;
        FILE* f = fopen(path, "w");
        if (f == nullptr) {
            p.onlineOpenErrno[c] = (errno != 0) ? errno : -1;
            p.cLastOpenErrno = p.onlineOpenErrno[c];
            // 打不开就没有写, 更没有"改过"—— 不需要恢复; 但为了可核对, 仍然读一次读回值
            int re = 0;
            char cur[16];
            if (auroraCoreStateReadRaw(path, cur, (int)sizeof(cur), &re)) {
                p.onlineAfterWrite[c] = (cur[0] == '1') ? 1 : 0;
            } else {
                p.onlineAfterWrite[c] = -1;
            }
            continue;
        }
        p.offlineWritable[c] = 1;
        p.writeAttempted[c] = 1;
        const char* payload = "1";
        errno = 0;
        const size_t wrote = fwrite(payload, 1, 1, f);
        const int werr = (wrote != 1) ? ((errno != 0) ? errno : -1) : 0;
        errno = 0;
        const int frc = fflush(f);
        const int ferr = (frc != 0) ? ((errno != 0) ? errno : -1) : 0;
        errno = 0;
        const int crc = fclose(f);
        const int cerr = (crc != 0) ? ((errno != 0) ? errno : -1) : 0;
        p.writeRc[c] = (int)wrote;
        p.writeErrno[c] = (werr != 0) ? werr : ((ferr != 0) ? ferr : ((cerr != 0) ? cerr : 0));
        if (p.writeErrno[c] != 0) {
            p.cLastWriteErrno = p.writeErrno[c];
        }
        // 写后读回: 这是"到底拉起来没有"的唯一硬证据(返回值 0 不代表核真的上线了)
        {
            char cur[16];
            int re = 0;
            if (auroraCoreStateReadRaw(path, cur, (int)sizeof(cur), &re)) {
                p.onlineAfterWrite[c] = (cur[0] == '1') ? 1 : 0;
            } else {
                p.onlineAfterWrite[c] = -1;
            }
        }
        if (p.onlineAfterWrite[c] == 1) {
            p.cWriteOk += 1;
        } else {
            p.cWriteFail += 1;
        }
        // 无条件恢复: 只有"当前值 != 备份值"时才回写(不多做无谓的写), 回写后读回校验
        {
            char cur[16];
            int re = 0;
            const bool known = (auroraCoreStateReadRaw(path, cur, (int)sizeof(cur), &re) != 0);
            if (!known || strcmp(cur, backup) != 0) {
                errno = 0;
                FILE* fw = fopen(path, "w");
                if (fw != nullptr) {
                    const size_t w2 = fwrite(backup, 1, strlen(backup), fw);
                    (void)w2;
                    fclose(fw);
                    char cur2[16];
                    int re2 = 0;
                    if (auroraCoreStateReadRaw(path, cur2, (int)sizeof(cur2), &re2)) {
                        p.restoreOk[c] = (strcmp(cur2, backup) == 0) ? 1 : 0;
                    } else {
                        p.restoreOk[c] = 0;
                    }
                } else {
                    p.restoreOk[c] = 0;
                }
            } else {
                p.restoreOk[c] = 1;   // 没有被改动过, 视为已恢复(读回就等于备份值)
            }
        }
    }

    // ---- 还原原掩码(必须做: 后面绑核逻辑要在一个干净的起点上工作) ----
    if (savedOk) {
        cpu_set_t back2;
        CPU_ZERO(&back2);
        for (int c = 0; c < 64; ++c) {
            if (((savedMask >> (unsigned)c) & 1ull) != 0) {
                CPU_SET(c, &back2);
            }
        }
        errno = 0;
        (void)sched_setaffinity(0, sizeof(back2), &back2);
    }

    // ---- 汇总计数 + 位图 ----
    for (int c = 0; c < p.upper && c < kMaxCoreStateCpus; ++c) {
        const unsigned long long bit = (1ull << (unsigned)c);
        switch (p.cls[c]) {
            case AURORA_CORE_CLASS_A: p.countA += 1; p.maskA |= bit; break;
            case AURORA_CORE_CLASS_B: p.countB += 1; p.maskB |= bit; break;
            case AURORA_CORE_CLASS_C: p.countC += 1; p.maskC |= bit; break;
            case AURORA_CORE_CLASS_D: p.countD += 1; p.maskD |= bit; break;
            default: break;
        }
    }

    // ---- C 类拉起的结论(措辞必须与证据一致, 不许含糊) ----
    {
        if (p.countC == 0) {
            snprintf(p.cVerdict, sizeof(p.cVerdict),
                     "本机 C 类(离线)核 0 个 —— 没有核处于 offline, 不存在\"应用把核拉起来\"这件事");
        } else if (p.cWriteOk > 0) {
            snprintf(p.cVerdict, sizeof(p.cVerdict),
                     "C 类里有 %d 个核在写入 '1' 之后真的变成在线(写后读回=1): 这些核可以被"
                     "本进程拉起(已按原值恢复)", p.cWriteOk);
        } else {
            char e1[64];
            char e2[64];
            snprintf(e1, sizeof(e1), "%d", p.cLastOpenErrno);
            snprintf(e2, sizeof(e2), "%d", p.cLastWriteErrno);
            const char* openName = "";
            if (p.cLastOpenErrno == 13) {
                openName = "(EACCES 权限不足)";
            } else if (p.cLastOpenErrno == 1) {
                openName = "(EPERM 操作不允许)";
            } else if (p.cLastOpenErrno == 30) {
                openName = "(EROFS 只读文件系统)";
            } else if (p.cLastOpenErrno == 2) {
                openName = "(ENOENT 文件不存在)";
            } else if (p.cLastOpenErrno != 0) {
                openName = "(其它 errno)";
            }
            const char* writeName = "";
            if (p.cLastWriteErrno == 13) {
                writeName = "(EACCES 权限不足)";
            } else if (p.cLastWriteErrno == 1) {
                writeName = "(EPERM 操作不允许)";
            } else if (p.cLastWriteErrno == 30) {
                writeName = "(EROFS 只读文件系统)";
            } else if (p.cLastWriteErrno == 16) {
                writeName = "(EBUSY 占用中)";
            } else if (p.cLastWriteErrno != 0) {
                writeName = "(其它 errno)";
            }
            snprintf(p.cVerdict, sizeof(p.cVerdict),
                     "应用域无权拉起下线核: C 类 %d 个核里, 写 '1' 后变成在线的 0 个, 写失败的 %d 个; "
                     "打开 cpuN/online 失败的 errno=%s%s, 写失败的 errno=%s%s —— 这是权限/策略层面的拒绝, "
                     "不是本 App 能绕过的(按要求: 不尝试绕过内核策略)。所有被写过的核都已按原值恢复(逐核见下表)",
                     p.countC, p.cWriteFail, e1, openName, e2, writeName);
        }
    }

    // ---- 逐核一行(原文 + 实测 + 频率 + 分类) ----
    {
        p.perCore[0] = '\0';
        for (int c = 0; c < p.upper && c < kMaxCoreStateCpus; ++c) {
            char one[160];
            const char* st = auroraCoreStateOf(p.cls[c]).name;
            if (p.cls[c] == AURORA_CORE_CLASS_D) {
                snprintf(one, sizeof(one), "%scpu%d[%s:present 不含它]",
                         (c > 0) ? " " : "", c, st);
            } else if (p.cls[c] == AURORA_CORE_CLASS_C) {
                snprintf(one, sizeof(one), "%scpu%d[%s:online=%s]",
                         (c > 0) ? " " : "", c, st, p.onlineRawPer[c]);
            } else {
                snprintf(one, sizeof(one), "%scpu%d[%s:online=%s,setaffinity=%s%s]",
                         (c > 0) ? " " : "", c, st,
                         p.onlineKnown[c] ? p.onlineRawPer[c] : "无此文件",
                         p.accepted[c] ? "接受" : "被拒",
                         p.accepted[c] ? "" : "");
            }
            if ((int)strlen(p.perCore) + (int)strlen(one) >= (int)sizeof(p.perCore) - 2) {
                auroraCoreStateAppend(p.perCore, sizeof(p.perCore), " …(核号超出本行容量)");
                break;
            }
            auroraCoreStateAppend(p.perCore, sizeof(p.perCore), one);
        }
    }

    // ---- 逐核写探测表(备份 -> 写 -> 读回 -> 恢复; 一行一个核) ----
    {
        p.writeTable[0] = '\0';
        for (int c = 0; c < p.upper && c < kMaxCoreStateCpus; ++c) {
            if (p.onlineProbeable[c] == 0) {
                continue;
            }
            char one[128];
            if (p.writeAttempted[c] == 0) {
                snprintf(one, sizeof(one), "%scpu%d(打开失败 errno=%d, 未写)",
                         (p.writeTable[0] != '\0') ? " " : "", c, p.onlineOpenErrno[c]);
            } else {
                snprintf(one, sizeof(one), "%scpu%d(备份=%s ok=%d -> 写1 rc=%d errno=%d -> 读回=%d -> 恢复=%s)",
                         (p.writeTable[0] != '\0') ? " " : "", c,
                         p.onlineBackupOk[c] ? "有" : "无", p.onlineBackupOk[c],
                         p.writeRc[c], p.writeErrno[c], p.onlineAfterWrite[c],
                         p.restoreOk[c] ? "成功" : "失败");
            }
            if ((int)strlen(p.writeTable) + (int)strlen(one) >= (int)sizeof(p.writeTable) - 2) {
                auroraCoreStateAppend(p.writeTable, sizeof(p.writeTable), " …(核号超出本行容量)");
                break;
            }
            auroraCoreStateAppend(p.writeTable, sizeof(p.writeTable), one);
        }
        if (p.writeTable[0] == '\0') {
            snprintf(p.writeTable, sizeof(p.writeTable),
                     "(没有任何 cpuN/online 文件可打开 —— 本机不支持核热插拔)");
        }
    }

    // ---- 完整一行 ----
    {
        char la[160];
        char lb[160];
        char lc[160];
        char ld[160];
        auroraCoreStateMaskText(p.maskA, la, (int)sizeof(la));
        auroraCoreStateMaskText(p.maskB, lb, (int)sizeof(lb));
        auroraCoreStateMaskText(p.maskC, lc, (int)sizeof(lc));
        auroraCoreStateMaskText(p.maskD, ld, (int)sizeof(ld));
        char head[1024];
        snprintf(head, sizeof(head),
                 "逐核启动状态与分类(唯一判据来源: sysfs 原文 + 逐核实测 setaffinity + online 写探测; "
                 "线程 tid=%d, 核号上界 %d 来自 %s) "
                 "全局文件原文: present=\"%s\"(errno=%d, %d 核) · possible=\"%s\"(errno=%d, %d 核) · "
                 "online=\"%s\"(errno=%d, %d 核) · ",
                 p.tid, p.upper, auroraCoreStateUpperSrcText(p.upperSrc),
                 p.presentRaw, p.presentErrno, p.presentCount,
                 p.possibleRaw, p.possibleErrno, p.possibleCount,
                 p.onlineRaw, p.globalOnlineErrno, p.onlineCount);
        char summary[640];
        snprintf(summary, sizeof(summary),
                 "分类汇总: A 在线且被允许 %d 个[%s] · B 在线但被许可集合拒绝 %d 个[%s] · "
                 "C 离线(没启动) %d 个[%s] · D present 不含 %d 个[%s] · %s · "
                 "逐核写探测(备份->写1->读回->恢复): %s",
                 p.countA, la, p.countB, lb, p.countC, lc, p.countD, ld,
                 p.cVerdict, p.writeTable);
        snprintf(p.text, sizeof(p.text), "%s%s · 逐核[%s]", head, summary, p.perCore);
    }
    return p;
}

// ---------------------------------------------------------------------------
//  "为什么有核没有启动 / 为什么有线程没有跑"的一句话判据(可直接拼进 note 的原因链)
//
//  它不是新的探测, 只是把上面那一行里的四个计数与位图原样翻译成原因链:
//    * 有 C 类(离线)核 -> "N 个核处于 offline, 没有启动(不是被挡住, 是根本没上线)";
//    * 有 B 类(在线但被许可集合拒绝)核 -> "N 个核在线却被内核/策略挡在可用核集合之外";
//    * 有 D 类核 -> "N 个核不在 present 里(硬件根本没报)";
//    * 三者都为空 -> "逐核探测里没有'没启动'的核: 可用核数少不是核没启动造成的"。
//  这一句是纯读的: 只读结构体字段, 不探测、不写盘、不改任何行为。
// ---------------------------------------------------------------------------
inline std::string auroraCoreStartupReasonText(const AuroraCoreStateProbe& p)
{
    std::string s;
    char one[320];
    if (p.ran == 0) {
        return std::string("逐核启动状态探测未执行(读不到任何可用读数)");
    }
    snprintf(one, sizeof(one),
             "逐核启动状态: A(在线可用) %d 个 / B(在线但被许可集合拒绝) %d 个 / "
             "C(离线, 没有启动) %d 个 / D(present 不含) %d 个(核号上界 %d)",
             p.countA, p.countB, p.countC, p.countD, p.upper);
    s += one;
    if (p.countC > 0) {
        snprintf(one, sizeof(one),
                 "; 这 %d 个核没有启动(处于 offline, 通常是热插拔下线): 这就是"
                 "『一核一线程』落不到它们身上的原因 —— 线程数被夹到可用核数 N 时, 它们本来就不在 N 里面",
                 p.countC);
        s += one;
    }
    if (p.countB > 0) {
        snprintf(one, sizeof(one),
                 "; 另有 %d 个核在线但被本进程的许可集合拒绝(cpuset/cgroup 或厂商/热功耗策略): "
                 "本 App 不尝试绕过内核策略, 只上报", p.countB);
        s += one;
    }
    if (p.countD > 0) {
        snprintf(one, sizeof(one), "; 另有 %d 个核号不在 present 里(硬件/固件没有报出来)", p.countD);
        s += one;
    }
    if (p.countC == 0 && p.countB == 0 && p.countD == 0) {
        s += "; 逐核探测里没有任何\"没启动\"的核: 可用核数少于标称核数这件事与核的在线状态无关";
    }
    return s;
}

#endif
