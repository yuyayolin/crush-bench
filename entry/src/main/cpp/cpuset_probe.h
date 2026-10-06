#ifndef AURORA_CPUSET_PROBE_H
#define AURORA_CPUSET_PROBE_H

// ===========================================================================
//  只读诊断: cgroup / cpuset 分组 / core_ctl —— "限制到底在哪一层"
// ===========================================================================
//
//  要回答的问题(用户原话)
//  ---------------------------------------------------------------------------
//  "第三方应用只能用 cpu0-7(8 个逻辑核)"这条实测事实, 到底是被哪一层限住的?
//  可能的三层, 一层一层读出来:
//    ① cgroup/cpuset 分组: 进程落在哪个 cgroup、该分组的 cpus / effective_cpus 是哪些核;
//    ② 内核轻量级隔离 core_ctl: 逐核 enable / min_cpus / max_cpus / active_cpus / need_cpus /
//       global_state —— 厂商内核常见的"动态核隔离"实现;
//    ③ 别的层(厂商调度器 / QoS 时间份额): 前两层都读不出限制时, 写"不在前两层可观测范围"。
//
//  读什么(逐项记 errno; 读不到就写读不到, 不编)
//  ---------------------------------------------------------------------------
//    * /proc/self/cgroup                      —— 原文, 看我们落在哪个 cgroup
//    * <cgroup 目录>/cpuset.cpus              —— cgroup v2 的 cpus
//    * <cgroup 目录>/cpuset.cpus.effective    —— cgroup v2 的 effective_cpus
//    * /dev/cpuset/                           —— 逐个子目录(cgroup v1 挂载点),
//                                                每组的 cpus 与 effective_cpus
//    * /sys/devices/system/cpu/online         —— 原文
//    * /sys/devices/system/cpu/possible       —— 原文
//    * /sys/devices/system/cpu/cpuN/core_ctl/{enable,min_cpus,max_cpus,active_cpus,
//      need_cpus,global_state}                —— 逐核逐文件 + errno
//
//  硬约束
//  ---------------------------------------------------------------------------
//    * 只读: 只用 fopen(path,"r") 与 opendir/readdir; 一个字节都不写这些节点;
//    * 不改任何负载的算法/尺寸/metric/unit/k/conv/计分公式/线程数口径/绑核策略;
//    * 不计分: 结果只进诊断文本;
//    * 任何一步失败(权限 / 路径不存在 / 内容为空)都记 errno; 进程内只探一次并缓存;
//      不抛异常、不影响跑分。
//
//  errno 口径(与工程其它部分一致)
//  ---------------------------------------------------------------------------
//    >0 = 真实 errno(13=EACCES 权限, 2=ENOENT 不存在, 21=EISDIR 是目录, 1=EPERM)
//    -1 = 打得开但读不到内容(空文件 / 读失败)
//    -9 = 没试过 / 不适用
// ===========================================================================

#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace aurora_cpuset_detail {

constexpr int kMaxCpusetGroups = 16;   // /dev/cpuset 下最多记这么多组
constexpr int kMaxProbeCpus = 32;      // 逐核探测的核号上界(与工程其它表的 32 保持一致)

// ---- cpulist("0-3,8-11") 里的核个数(纯字符串计数, 不含任何猜测) ----
inline int countCpusInList(const char* s)
{
    if (s == nullptr) {
        return 0;
    }
    int n = 0;
    const char* q = s;
    while (*q != '\0') {
        if (*q >= '0' && *q <= '9') {
            int a = 0;
            while (*q >= '0' && *q <= '9') {
                a = a * 10 + (*q - '0');
                ++q;
            }
            if (*q == '-') {
                ++q;
                int b = 0;
                int digits = 0;
                while (*q >= '0' && *q <= '9') {
                    b = b * 10 + (*q - '0');
                    ++q;
                    ++digits;
                }
                if (digits > 0 && b >= a) {
                    n += (b - a + 1);
                }
            } else {
                n += 1;
            }
        } else {
            ++q;
        }
    }
    return n;
}

// ---- cpulist 里最大的核号(-1 = 一个都没有) ----
inline int maxCpuInList(const char* s)
{
    if (s == nullptr) {
        return -1;
    }
    int mx = -1;
    const char* q = s;
    while (*q != '\0') {
        if (*q >= '0' && *q <= '9') {
            int a = 0;
            while (*q >= '0' && *q <= '9') {
                a = a * 10 + (*q - '0');
                ++q;
            }
            if (a > mx) {
                mx = a;
            }
            if (*q == '-') {
                ++q;
                int b = 0;
                int digits = 0;
                while (*q >= '0' && *q <= '9') {
                    b = b * 10 + (*q - '0');
                    ++q;
                    ++digits;
                }
                if (digits > 0 && b > mx) {
                    mx = b;
                }
            }
        } else {
            ++q;
        }
    }
    return mx;
}

struct AuroraCpusetGroup {
    char name[40];        // 组名(top-app / foreground / …)
    char cpus[160];       // <组>/cpus 原文
    int cpusErrno;
    char eff[160];        // <组>/effective_cpus 原文
    int effErrno;
};

struct AuroraCpusetProbe {
    // ---- /proc/self/cgroup ----
    int cgroupOk;                 // 1 = 读到
    int cgroupErrno;
    char cgroupRaw[640];          // 原文(换行折成空格)
    // ---- cgroup v2 目录(由 "0::<path>" 推出)----
    char cgroupDir[192];
    int cgroupDirOk;
    int cgCpusOk, cgCpusErrno;    char cgCpus[160];
    int cgEffOk, cgEffErrno;      char cgEff[160];
    // ---- /dev/cpuset(cgroup v1 挂载点)----
    int devOk;                    // 1 = 目录打开成功
    int devErrno;
    char devErr[160];
    int groupCount;
    AuroraCpusetGroup groups[kMaxCpusetGroups];
    // ---- 全局 online / possible ----
    int onlineOk, onlineErrno;    char onlineRaw[160];
    int possibleOk, possibleErrno; char possibleRaw[160];
    // ---- core_ctl(逐核 6 个文件)----
    int coreUpper;
    int coreAnyOk;
    int coreEnableOk;
    int coreFilesOk;
    int coreFilesTotal;
    char coreCtlText[2496];
    // ---- 结论 ----
    char verdict[1024];
    char text[4800];

    AuroraCpusetProbe()
        : cgroupOk(0), cgroupErrno(-9), cgroupRaw(), cgroupDir(), cgroupDirOk(0),
          cgCpusOk(0), cgCpusErrno(-9), cgCpus(), cgEffOk(0), cgEffErrno(-9), cgEff(),
          devOk(0), devErrno(-9), devErr(), groupCount(0), groups(),
          onlineOk(0), onlineErrno(-9), onlineRaw(), possibleOk(0), possibleErrno(-9), possibleRaw(),
          coreUpper(0), coreAnyOk(0), coreEnableOk(0), coreFilesOk(0), coreFilesTotal(0),
          coreCtlText(), verdict(), text()
    {
    }
};

// ---- 极简只读文本读取: 读不到就写读不到, 不打印不抛异常 ----
inline bool readOneLine(const std::string& path, char* buf, int cap, int* errOut)
{
    if (errOut != nullptr) {
        *errOut = -9;
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
    if (n == 0) {
        if (errOut != nullptr) {
            *errOut = -1;   // 打得开但读不到内容: 也记
        }
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') {
            buf[i] = ' ';
        }
    }
    size_t e = strlen(buf);
    while (e > 0 && buf[e - 1] == ' ') {
        buf[--e] = '\0';
    }
    if (errOut != nullptr) {
        *errOut = 0;
    }
    return true;
}

// 从 /proc/self/cgroup 取 cgroup v2 路径("0::<path>"); 取不到返回空串
inline std::string parseCgroupV2Path(const char* raw)
{
    if (raw == nullptr) {
        return std::string();
    }
    const char* p = strstr(raw, "0::");
    if (p == nullptr) {
        return std::string();
    }
    p += 3;
    std::string s;
    while (*p != '\0' && *p != ' ') {
        s.push_back(*p);
        ++p;
    }
    return s;
}

inline void appendSeg(char* dst, size_t cap, const char* seg)
{
    if (seg == nullptr || seg[0] == '\0') {
        return;
    }
    const size_t used = strlen(dst);
    if (used + 3 + strlen(seg) + 1 > cap) {
        return;
    }
    snprintf(dst + used, cap - used, "%s%s", (used > 0) ? " · " : "", seg);
}

// 逐核读 core_ctl 的 6 个文件(只读)
inline void probeCoreCtl(AuroraCpusetProbe& p)
{
    int upper = -1;
    if (p.possibleOk) {
        upper = maxCpuInList(p.possibleRaw) + 1;
    }
    if (upper <= 0 && p.onlineOk) {
        upper = maxCpuInList(p.onlineRaw) + 1;
    }
    if (upper <= 0 || upper > kMaxProbeCpus) {
        upper = kMaxProbeCpus;
    }
    p.coreUpper = upper;

    static const char* kNames[6] = {"enable", "min_cpus", "max_cpus", "active_cpus", "need_cpus",
                                    "global_state"};
    p.coreCtlText[0] = '\0';
    for (int c = 0; c < upper; ++c) {
        char vals[6][28];
        for (int k = 0; k < 6; ++k) {
            char buf[40];
            int e = -9;
            const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(c) +
                                     "/core_ctl/" + kNames[k];
            ++p.coreFilesTotal;
            if (readOneLine(path, buf, (int)sizeof(buf), &e)) {
                snprintf(vals[k], sizeof(vals[k]), "%s", buf);
                ++p.coreFilesOk;
                p.coreAnyOk = 1;
                if (k == 0) {
                    ++p.coreEnableOk;
                }
            } else {
                snprintf(vals[k], sizeof(vals[k]), "(errno=%d)", e);
            }
        }
        char one[240];
        snprintf(one, sizeof(one),
                 "cpu%d{enable=%s,min_cpus=%s,max_cpus=%s,active_cpus=%s,need_cpus=%s,global_state=%s}",
                 c, vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
        appendSeg(p.coreCtlText, sizeof(p.coreCtlText), one);
    }
}

// 读 /dev/cpuset 下每一组的 cpus / effective_cpus(只读)
inline void probeDevCpuset(AuroraCpusetProbe& p)
{
    errno = 0;
    DIR* d = opendir("/dev/cpuset");
    if (d == nullptr) {
        p.devOk = 0;
        p.devErrno = errno != 0 ? errno : -1;
        snprintf(p.devErr, sizeof(p.devErr), "opendir(/dev/cpuset) 失败");
        return;
    }
    p.devOk = 1;
    p.devErrno = 0;
    for (;;) {
        struct dirent* e = readdir(d);
        if (e == nullptr) {
            break;
        }
        const char* nm = e->d_name;
        if (nm == nullptr || nm[0] == '\0' || strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
            continue;
        }
        if (p.groupCount >= kMaxCpusetGroups) {
            break;
        }
        char buf[160];
        int e1 = -9;
        const std::string base = std::string("/dev/cpuset/") + nm;
        const bool cpusOk = readOneLine(base + "/cpus", buf, (int)sizeof(buf), &e1);
        if (!cpusOk && (e1 == 21 /*EISDIR*/ || e1 == 2 /*ENOENT*/ || e1 == 20 /*ENOTDIR*/)) {
            continue;   // 不是 cpuset 分组目录(没有核集合文件): 不记, 免得噪声
        }
        AuroraCpusetGroup& g = p.groups[p.groupCount];
        snprintf(g.name, sizeof(g.name), "%s", nm);
        if (cpusOk) {
            snprintf(g.cpus, sizeof(g.cpus), "%s", buf);
            g.cpusErrno = 0;
        } else {
            g.cpus[0] = '\0';
            g.cpusErrno = e1;
        }
        int e2 = -9;
        if (readOneLine(base + "/effective_cpus", buf, (int)sizeof(buf), &e2)) {
            snprintf(g.eff, sizeof(g.eff), "%s", buf);
            g.effErrno = 0;
        } else {
            g.eff[0] = '\0';
            g.effErrno = e2;
        }
        ++p.groupCount;
    }
    closedir(d);
}

// ---- 结论: 限制在哪一层(只用读到的事实说话) ----
inline void buildVerdict(AuroraCpusetProbe& p, const char* measuredSetText, int measuredCount)
{
    const int onlineN = p.onlineOk ? countCpusInList(p.onlineRaw) : -1;
    const int possibleN = p.possibleOk ? countCpusInList(p.possibleRaw) : -1;

    // 找与我们逐核实测集合核数一致的组(优先 top-app / foreground)
    int hitIdx = -1;
    int hitCnt = -1;
    for (int pass = 0; pass < 2 && hitIdx < 0; ++pass) {
        for (int i = 0; i < p.groupCount; ++i) {
            const bool preferred = (strcmp(p.groups[i].name, "top-app") == 0 ||
                                    strcmp(p.groups[i].name, "foreground") == 0);
            if (pass == 0 && !preferred) {
                continue;
            }
            const char* s = (p.groups[i].effErrno == 0) ? p.groups[i].eff : p.groups[i].cpus;
            if (s[0] == '\0') {
                continue;
            }
            const int n = countCpusInList(s);
            if (measuredCount > 0 && n == measuredCount) {
                hitIdx = i;
                hitCnt = n;
                break;
            }
        }
    }

    char cpusetPart[420];
    bool cpusetHit = false;
    if (hitIdx >= 0) {
        cpusetHit = true;
        snprintf(cpusetPart, sizeof(cpusetPart),
                 "① cpuset 分组层: 命中 —— /dev/cpuset/%s 的核集合 = %d 个核 [%s], "
                 "与本工程逐核实测到的可用核集合(%d 个: %s)核数一致",
                 p.groups[hitIdx].name, hitCnt,
                 (p.groups[hitIdx].effErrno == 0) ? p.groups[hitIdx].eff : p.groups[hitIdx].cpus,
                 measuredCount, measuredSetText != nullptr ? measuredSetText : "?");
    } else if (p.groupCount > 0) {
        snprintf(cpusetPart, sizeof(cpusetPart),
                 "① cpuset 分组层: 读到 %d 个组, 但没有任何一组的核数与实测可用核集合(%d 个)一致 "
                 "-> cgroup 层不是限制来源(或本进程的可读路径覆盖不到那一组)",
                 p.groupCount, measuredCount);
    } else {
        snprintf(cpusetPart, sizeof(cpusetPart),
                 "① cpuset 分组层: 读不到任何组(/dev/cpuset 打开=%s, errno=%d)%s -> 这一层判不出来",
                 p.devOk ? "成功但没有任何 cpus 文件" : "失败", p.devErrno,
                 (p.devOk == 0 && p.devErr[0] != 0) ? p.devErr : "");
    }

    char corePart[360];
    if (p.coreFilesTotal == 0) {
        snprintf(corePart, sizeof(corePart), "② core_ctl 层: 没探(核号上界为 0)");
    } else if (p.coreAnyOk == 0) {
        snprintf(corePart, sizeof(corePart),
                 "② core_ctl 层: 一个文件都读不到(%d 个文件全部失败) -> 本机内核没有这个节点, "
                 "或本进程域下不可读; 这一层不构成可观测的限制",
                 p.coreFilesTotal);
    } else {
        snprintf(corePart, sizeof(corePart),
                 "② core_ctl 层: 读到 %d/%d 个文件(enable 在 %d 个核上读到) -> 逐核一行见 [5], "
                 "active_cpus/need_cpus 与 online 的差就是这一层的隔离量",
                 p.coreFilesOk, p.coreFilesTotal, p.coreEnableOk);
    }

    char otherPart[420];
    if (cpusetHit) {
        snprintf(otherPart, sizeof(otherPart),
                 "③ 别的层: cpuset 分组的核集合已经等于实测可用核集合, 因此『只有 8 个核』"
                 "就是 cpuset 分组限的(cgroup v1 的 cpuset 只能收窄、不能扩大); "
                 "core_ctl 就算在动, 也只会在这批核内部再收窄");
    } else {
        snprintf(otherPart, sizeof(otherPart),
                 "③ 别的层: 前两层若都没有给出可核对的解释, 剩下的候选是厂商调度器 / QoS 时间份额"
                 "(限的不是核集合, 而是给多少时间) —— 这与『单核项运行时频率中位只有标称的 70 个百分点』"
                 "属于同一类现象");
    }

    snprintf(p.verdict, sizeof(p.verdict),
             "限制在哪一层(只读结论): 在线核 online=[%s](%d 个) · possible=[%s](%d 个); %s; %s; %s",
             p.onlineOk ? p.onlineRaw : "读不到", onlineN,
             p.possibleOk ? p.possibleRaw : "读不到", possibleN,
             cpusetPart, corePart, otherPart);
}

// ---- 组装完整文本 ----
inline void buildText(AuroraCpusetProbe& p, const char* measuredSetText, int measuredCount)
{
    char seg[1024];
    p.text[0] = '\0';

    snprintf(seg, sizeof(seg), "[1] /proc/self/cgroup: %s(errno=%d) 原文=[%s]",
             p.cgroupOk ? "读到" : "读不到", p.cgroupErrno, p.cgroupOk ? p.cgroupRaw : "");
    appendSeg(p.text, sizeof(p.text), seg);

    if (p.cgroupDirOk) {
        snprintf(seg, sizeof(seg),
                 "[2] cgroup v2 目录=%s: cpuset.cpus=%s(errno=%d), cpuset.cpus.effective=%s(errno=%d)",
                 p.cgroupDir,
                 p.cgCpusOk ? p.cgCpus : "(读不到)", p.cgCpusErrno,
                 p.cgEffOk ? p.cgEff : "(读不到)", p.cgEffErrno);
    } else {
        snprintf(seg, sizeof(seg),
                 "[2] cgroup v2 目录: /proc/self/cgroup 里没有 \"0::\" 行 -> 本机不是 cgroup v2, "
                 "或该行不可读(这一层判不出来)");
    }
    appendSeg(p.text, sizeof(p.text), seg);

    snprintf(seg, sizeof(seg), "[3] /dev/cpuset: %s(errno=%d%s), 记到 %d 个分组",
             p.devOk ? "打开成功" : "打开失败", p.devErrno,
             (p.devOk == 0 && p.devErr[0] != 0) ? p.devErr : "", p.groupCount);
    appendSeg(p.text, sizeof(p.text), seg);
    for (int i = 0; i < p.groupCount && i < kMaxCpusetGroups; ++i) {
        const AuroraCpusetGroup& g = p.groups[i];
        snprintf(seg, sizeof(seg), "[3.%d] %s: cpus=[%s](errno=%d) effective_cpus=[%s](errno=%d)",
                 i + 1, g.name,
                 g.cpusErrno == 0 ? g.cpus : "(读不到)", g.cpusErrno,
                 g.effErrno == 0 ? g.eff : "(读不到)", g.effErrno);
        appendSeg(p.text, sizeof(p.text), seg);
    }

    snprintf(seg, sizeof(seg),
             "[4] /sys/devices/system/cpu/online=[%s](errno=%d) · "
             "/sys/devices/system/cpu/possible=[%s](errno=%d)",
             p.onlineOk ? p.onlineRaw : "(读不到)", p.onlineErrno,
             p.possibleOk ? p.possibleRaw : "(读不到)", p.possibleErrno);
    appendSeg(p.text, sizeof(p.text), seg);

    snprintf(seg, sizeof(seg), "[5] core_ctl 逐核(核号 0..%d; 读到 %d/%d 个文件): %s",
             p.coreUpper - 1, p.coreFilesOk, p.coreFilesTotal, p.coreCtlText);
    appendSeg(p.text, sizeof(p.text), seg);

    snprintf(seg, sizeof(seg), "[6] 与逐核实测的可用核集合对照: %d 个核 [%s]",
             measuredCount, measuredSetText != nullptr ? measuredSetText : "?");
    appendSeg(p.text, sizeof(p.text), seg);

    snprintf(seg, sizeof(seg), "[7] %s", p.verdict);
    appendSeg(p.text, sizeof(p.text), seg);
}

// ---- 进程内只探一次(纯只读; 缓存起来供每一项复用) ----
inline const AuroraCpusetProbe& probeCached(const char* measuredSetText, int measuredCount)
{
    static AuroraCpusetProbe p = [&]() {
        AuroraCpusetProbe q;
        readOneLine("/proc/self/cgroup", q.cgroupRaw, (int)sizeof(q.cgroupRaw), &q.cgroupErrno);
        q.cgroupOk = (q.cgroupErrno == 0) ? 1 : 0;

        const std::string v2 = parseCgroupV2Path(q.cgroupRaw);
        if (!v2.empty()) {
            snprintf(q.cgroupDir, sizeof(q.cgroupDir), "/sys/fs/cgroup%s", v2.c_str());
            char tmp[64];
            int e = -9;
            if (readOneLine(std::string(q.cgroupDir) + "/cgroup.controllers", tmp,
                            (int)sizeof(tmp), &e)) {
                q.cgroupDirOk = 1;
            } else if (e != 2 /*ENOENT*/) {
                q.cgroupDirOk = 1;   // 目录在, 只是没有这个文件
            }
            if (q.cgroupDirOk) {
                readOneLine(std::string(q.cgroupDir) + "/cpuset.cpus", q.cgCpus,
                            (int)sizeof(q.cgCpus), &q.cgCpusErrno);
                q.cgCpusOk = (q.cgCpusErrno == 0) ? 1 : 0;
                readOneLine(std::string(q.cgroupDir) + "/cpuset.cpus.effective", q.cgEff,
                            (int)sizeof(q.cgEff), &q.cgEffErrno);
                q.cgEffOk = (q.cgEffErrno == 0) ? 1 : 0;
            }
        }

        readOneLine("/sys/devices/system/cpu/online", q.onlineRaw, (int)sizeof(q.onlineRaw),
                    &q.onlineErrno);
        q.onlineOk = (q.onlineErrno == 0) ? 1 : 0;
        readOneLine("/sys/devices/system/cpu/possible", q.possibleRaw, (int)sizeof(q.possibleRaw),
                    &q.possibleErrno);
        q.possibleOk = (q.possibleErrno == 0) ? 1 : 0;

        probeDevCpuset(q);
        probeCoreCtl(q);
        buildVerdict(q, measuredSetText, measuredCount);
        buildText(q, measuredSetText, measuredCount);
        return q;
    }();
    return p;
}

} // namespace aurora_cpuset_detail

// 完整文本(含逐项 errno); 进报告的 cpuset 字段与 note
inline std::string auroraCpusetProbeText(const char* measuredSetText, int measuredCount)
{
    return std::string(aurora_cpuset_detail::probeCached(measuredSetText, measuredCount).text);
}

// 一句人话结论; 追加进每项 note(短)
inline std::string auroraCpusetProbeVerdict(const char* measuredSetText, int measuredCount)
{
    return std::string(aurora_cpuset_detail::probeCached(measuredSetText, measuredCount).verdict);
}

#endif  // AURORA_CPUSET_PROBE_H
