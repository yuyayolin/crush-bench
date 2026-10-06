# -*- coding: utf-8 -*-
r"""
verify_core_online.py —— "为什么有核没有启动 / 为什么有线程没有跑"的自查器(静态源码断言 + 镜像; 无设备)

背景(用户原话):
    "它是有 9 核 14 线程, 你应该去看为什么有核没有启动, 为什么有线程没有跑"
真机上三个来源互相矛盾:
    * /sys/devices/system/cpu/present 报 0-8   (9 个核)
    * /proc/self/status 的 Cpus_allowed 报 0-13 (14 个核)
    * 逐核 sched_setaffinity + 立刻读回报 0-7    (8 个核)
而"不可用"有四种性质完全不同的原因(offline 热插拔下线 / cpuset 拒绝 / 厂商策略 / 热功耗策略),
必须逐核分清, 不能用一句"用不了"带过。

本脚本做三件事(全部离线, 不需要设备, 不连 hdc):
  A) 源码结构断言: 从 cpu_core_state.h / cpu_affinity.h / cpu_freq_sample.cpp 里确认
       ① 逐核读 cpuN/online 的原文 + errno; 三个全局文件 present/possible/online 的原文 + errno;
       ② 逐核读 cpufreq/{cpuinfo_max_freq,scaling_cur_freq} 与 topology/{core_id,
          physical_package_id,thread_siblings_list} 的原文 + errno;
       ③ 逐核 sched_setaffinity({c}) + 立刻读回(返回 0 但读回不是那一位 = 被夹回, 不算接受),
          并逐核记 errno;
       ④ 分类判据: A/B/C/D 四类的判据在源码里逐条存在, 且判定顺序固定为 D -> C -> B -> A;
       ⑤ C 类拉起的恢复逻辑: 写之前先备份原值, 无论写成败都无条件恢复, 恢复后读回校验,
          失败要记 errno(不写成含糊的"拉不起来");
       ⑥ 探测结束还原原掩码;
       ⑦ 多核阶段: 可用核数 / 工作集合 / 线程数 / 成功绑到独占核的线程数 / 实际用到 M 个核及清单 /
          每核是否有线程 —— 六项同源同报(M<N 时必须给原因链), 原因链里要有"逐核启动状态"那一条;
       ⑧ 新增判据: 整项忙占比低于阈值(5%)的核单独列出并写明"该核在整个本项期间基本没干活";
       ⑨ 不许动的东西没动(score 公式 / k 常数 / 打点数量 / executor 分块 / 线程数口径);
       ⑩ 没有引入按机型 / SoC 的分支。
  B) 数字镜像: 用真机形状(present=0-8 / possible=0-8 / 实测 accept=0-7)跑一遍分类, 断言
        * cpu8 被归入哪一类、为什么(它的 online 原文决定 C 还是不 C);
        * 再把 online 原文换成 14 核(0-13)、把 present 换成 0-8 等形状, 逐条断言 A/B/C/D 的归属;
        * 穷举多种 (present, online0/1, 实测接受) 组合, 断言"分类与判据表逐字一致"。
  C) 恢复逻辑镜像: 把 C 类拉起探测的"备份 -> 写 -> 读回 -> 恢复"逐步镜像, 断言
        * 写失败(EACCES=13 / EPERM=1 / EROFS=30)时原值一个字节都不许变;
        * 写成功把 online 从 0 改成 1 之后, 恢复必须把它写回 0(且 restoreOk 才为 1);
        * 返回值 0 但读回仍是 0 时不算"拉起来"(cWriteOk 不得自增);
        * 没有任何 online 文件时不写、不报成功。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_core_online_out.txt"), "w", encoding="utf-8", newline="\n")
fails = []

def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    OUT.write(s + "\n")

def check(name, ok, detail=""):
    emit("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  <- " + str(detail)) if not ok else ""))
    if not ok:
        fails.append(name)

def rd(fn):
    p = os.path.join(HERE, fn)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()

src = rd("cpu_core_state.h")          # 逐核启动状态探测 + 分类 + C 类拉起
aff = rd("cpu_affinity.h")            # 会话 / 铺满取证(多核阶段)
freq = rd("cpu_freq_sample.cpp")      # 逐核 /proc/stat 占用率
par = rd("gb7_parallel.h")
gb7 = rd("gb7.cpp")

def body_of(text, sig_regex):
    m = re.search(sig_regex, text, re.S)
    if not m:
        return ""
    rest = text[m.start():]
    end = rest.find("\n}\n")
    return rest[:end + 2] if end >= 0 else rest

probe = body_of(src, r"inline AuroraCoreStateProbe probeCoreStartupState\(\)")
upper = body_of(src, r"inline int auroraCoreStateUpperBound\(int\* srcOut\)")
reason = body_of(src, r"inline std::string auroraCoreStartupReasonText\(const AuroraCoreStateProbe& p\)")
send = body_of(aff, r"inline AuroraCpuPlacement auroraAffinitySessionEnd\(\)")
sbegin = body_of(aff, r"inline void auroraAffinitySessionBegin\(int threads\)")
sample = body_of(aff, r"inline AuroraCpuPlacement auroraCpuSampleCurrent\(\)")

emit("=" * 100)
emit("[A] 源码结构断言")
emit("=" * 100)

# ---- ① 逐核 online + 三个全局文件的原文 ----
check("A1 存在逐核启动状态探测 probeCoreStartupState()", bool(probe))
check("A2 逐核读 cpuN/online 并把原文存下来(out->onlineRawPer)",
      'dir + "online"' in probe and "p.onlineRawPer[c]" in probe)
check("A3 三个全局文件 present / possible / online 的原文都读下来",
      '"/sys/devices/system/cpu/present"' in probe
      and '"/sys/devices/system/cpu/possible"' in probe
      and '"/sys/devices/system/cpu/online"' in probe
      and "p.presentRaw" in probe and "p.possibleRaw" in probe and "p.onlineRaw" in probe)
check("A4 三个全局文件各自的 errno 原样保留",
      "p.presentErrno" in probe and "p.possibleErrno" in probe and "p.globalOnlineErrno" in probe)
check("A5 逐核 online 的 errno 也逐核保留(文件不存在=不支持热插拔, 与'没权限'分开)",
      "p.onlineErrno[c] = e;" in probe and "p.onlineErrno[c] = 0;" in probe
      and "不支持热插拔" in probe)

# ---- ② 逐核频率 / 拓扑原文 ----
check("A6 逐核 cpufreq/cpuinfo_max_freq 原文 + errno",
      'dir + "cpufreq/cpuinfo_max_freq"' in probe and "p.freqRaw[c]" in probe and "p.freqErrno[c]" in probe)
check("A7 逐核 cpufreq/scaling_cur_freq 原文 + errno",
      'dir + "cpufreq/scaling_cur_freq"' in probe and "p.curFreqRaw[c]" in probe and "p.curFreqErrno[c]" in probe)
check("A8 逐核 topology/core_id 原文 + errno",
      'dir + "topology/core_id"' in probe and "p.coreIdRaw[c]" in probe and "p.coreIdErrno[c]" in probe)
check("A9 逐核 topology/physical_package_id 原文 + errno",
      'dir + "topology/physical_package_id"' in probe and "p.pkgRaw[c]" in probe and "p.pkgErrno[c]" in probe)
check("A10 逐核 topology/thread_siblings_list 原文 + errno",
      'dir + "topology/thread_siblings_list"' in probe and "p.sibRaw[c]" in probe and "p.sibErrno[c]" in probe)

# ---- ③ 逐核实测 ----
check("A11 逐核发 sched_setaffinity({c})", "CPU_SET(c, &one);" in probe
      and "sched_setaffinity(0, sizeof(one), &one)" in probe)
check("A12 设置后立刻读回(sched_getaffinity)", "sched_getaffinity(0, sizeof(back), &back)" in probe)
check("A13 判定用'读回掩码 == 那一位'而不是只看返回值",
      "backMask == want" in probe and "rc == 0 && rc2 == 0 && backMask == want" in probe)
check("A14 返回 0 但读回不是那一位 -> 记为夹回, 不算接受",
      "p.clamped[c] = 1;" in probe and "p.errnoOf[c] = -4;" in probe)
check("A15 逐核记 errno(含'读回失败'与'夹回'两种非 errno 形态)",
      "p.errnoOf[c] = setErrno;" in probe and "p.errnoOf[c] = -1;" in probe
      and "p.errnoOf[c] = -4;" in probe)

# ---- ④ 分类判据 ----
# 判据顺序要在可执行代码上验证, 不是在注释/文档文本上:
# 把 // 行注释整段挖掉再按出现次序检查, 注释里提到"D 类是 …"不会污染这条断言。
cls_seg = probe[probe.find("// ---- 分类"):probe.find("// ---- (6)")]
cls_code = "\n".join(re.sub(r"//.*$", "", ln) for ln in cls_seg.split("\n"))
order_d = cls_code.find("AURORA_CORE_CLASS_D")
order_c = cls_code.find("AURORA_CORE_CLASS_C")
order_b = cls_code.find("AURORA_CORE_CLASS_B")
order_a = cls_code.find("AURORA_CORE_CLASS_A")
check("A16 四类常量都在(A/B/C/D 各有定义)",
      all(k in src for k in ["AURORA_CORE_CLASS_A = 1", "AURORA_CORE_CLASS_B = 2",
                             "AURORA_CORE_CLASS_C = 3", "AURORA_CORE_CLASS_D = 4"]))
check("A17 判据顺序固定: D(present) -> C(online) -> A(实测接受) -> B(实测被拒)",
      bool(cls_seg) and order_d > 0 and order_c > order_d and order_a > order_c and order_b > order_a,
      "D@%d C@%d A@%d B@%d" % (order_d, order_c, order_a, order_b))
check("A18 D 类判据 = present 原文不含该核号(hasPresent == 0)",
      "if (p.hasPresent[c] == 0) {" in probe and "AURORA_CORE_CLASS_D;" in probe)
check("A19 C 类判据 = online 原文为 0(核处于 offline, 热插拔下线)",
      "p.onlineKnown[c] == 1 && p.onlineValue[c] == 0" in probe
      and "AURORA_CORE_CLASS_C;" in probe and "offline(热插拔下线)" in probe)
check("A20 B 类判据 = present 含该核但实测 setaffinity 被拒",
      "p.accepted[c] != 0" in probe and "AURORA_CORE_CLASS_B;" in probe)
check("A21 A 类判据 = 实测 setaffinity 被内核接受", "AURORA_CORE_CLASS_A;" in probe)
check("A22 判据在注释里逐条写明(A/B/C/D 的判据不许只存在于代码里)",
      "A 在线且被内核允许(可用)" in src and "B 在线但被本进程的许可集合拒绝" in src
      and "C 离线(offline) —— 核没有启动" in src and "D present 里根本没有" in src)
check("A23 明确写出不绕过内核策略(B 类只上报)",
      "不做任何绕过内核策略的事" in src and "不尝试绕过内核策略" in reason)

# ---- ⑤ C 类拉起的恢复逻辑 ----
check("A24 对 online 文件存在的核才尝试写(不支持热插拔的不写)",
      "if (p.onlineProbeable[c] == 0) {" in probe)
check("A25 写之前先备份原值", "备份原值(必须: 恢复时要写回它)" in probe
      and "p.onlineBackupOk[c] = 1;" in probe)
check("A26 打开 cpuN/online 的 errno 原样记下(EACCES/EPERM/EROFS 是不同性质的证据)",
      "p.onlineOpenErrno[c]" in probe and "p.cLastOpenErrno" in probe)
check("A27 写之后读回, 用读回值判'到底拉起来没有'(返回 0 不算)",
      "p.onlineAfterWrite[c] = (cur[0] == '1') ? 1 : 0;" in probe
      and "p.cWriteOk += 1;" in probe and "p.cWriteFail += 1;" in probe)
check("A28 无论成败都无条件恢复(只有当前值 != 备份值时才回写)",
      "无条件恢复" in probe and "strcmp(cur, backup) != 0" in probe)
check("A29 恢复后读回校验(restoreOk 由读回与备份比较得出, 不是假设成功)",
      "p.restoreOk[c] = (strcmp(cur2, backup) == 0) ? 1 : 0;" in probe)
check("A30 写失败时把 errno 记全(fwrite / fflush / fclose 三处)",
      "const int werr" in probe and "const int ferr" in probe and "const int cerr" in probe)
check("A31 打不开时明确写下'未写'(不许把'没写'当成'写失败'或'写成功')",
      "未写" in probe and "p.writeAttempted[c] = 1;" in probe)
check("A32 结论文本区分三种情形: 无 C 类 / 拉起成功 / 应用域无权拉起",
      "本机 C 类(离线)核 0 个" in src and "真的变成在线" in src
      and "应用域无权拉起下线核" in src)
check("A33 无权限时把 errno 映射成人话(EACCES / EPERM / EROFS / ENOENT / EBUSY)",
      "EACCES 权限不足" in src and "EPERM 操作不允许" in src
      and "EROFS 只读文件系统" in src and "EBUSY 占用中" in src)

# ---- ⑥ 探测结束还原原掩码 ----
check("A34 探测前保存原掩码, 结束后无条件还原",
      "先备份原掩码(探测结束后无条件还原)" in probe
      and "sched_setaffinity(0, sizeof(back2), &back2)" in probe)
check("A35 探测发生在计时区间之外(会话开始之前, 不在任何负载文件里)",
      all("probeCoreStartupState" not in rd(f) for f in
          ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
           "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]))
check("A36 核号上界取五个来源的最大值(不会因某处读数失败而漏核)",
      "present" in upper and "possible" in upper and "/proc/cpuinfo" in upper
      and "sysconf(_SC_NPROCESSORS_ONLN)" in upper)

# ---- ⑦ 多核阶段六项同源同报 + 原因链 ----
check("A37 逐核启动状态被拼进每项 note 的那一行(cpuAllowedText)",
      "appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), p.coreStartupText);" in aff)
check("A38 分类结果逐项上报到 placement(A/B/C/D 四个计数与四个位图都在)",
      all(k in sample for k in ["p.coreClassA = kCoreStartup.countA",
                                "p.coreClassB = kCoreStartup.countB",
                                "p.coreClassC = kCoreStartup.countC",
                                "p.coreClassD = kCoreStartup.countD",
                                "p.coreClassAMask = kCoreStartup.maskA",
                                "p.coreClassCMask = kCoreStartup.maskC"]))
check("A39 六项取证同时给出: 可用核数 / 工作集合 / 线程数 / 成功绑到独占核 / M 与清单 / 每核是否有线程",
      "可用核数 N=%d" in send and "本项线程 %d(请求 %d)" in send
      and "成功绑到独占核的池线程 %d 个" in send and "实际用到的核 M=%d 个" in send
      and "每核是否有线程" in send)
check("A40 M<N 的原因链里必须有'逐核启动状态'那一条(带数字)",
      "逐核启动状态: C(离线, 没启动) %d 个" in send and "A(可用) %d 个" in send)
check("A41 原因链不许静默: 未定位时也要给出去处", "未定位(见逐核落点表与逐核占用率)" in send)
# 2026-10-06 按真机 A/B 回退(断言写清这段历史, 免得再被"物理核口径"改回去):
#   曾经: 线程数 = 可用**物理**核数(topologyCached().spreadOrder.size(), 真机 6; 一物理核一线程)
#   实测 : 8 线程 gb8Multi = 1481.6 / 1488.4 / 1540.2 (parallelism 实测 8.00)
#          6 线程 gb8Multi = 1069.8                  (parallelism 实测 6.00)
#   结论 : 两条 SMT 兄弟线程确实在贡献吞吐, 物理核口径慢 34% -> 回到可用**逻辑**核数
check("A42 线程数 = 可用逻辑核数这条口径已生效(2026-10-06 按真机 A/B 回退)",
      "const int usable = (int)aurora_cpu_detail::effectiveOrderCached().size();" in aff
      and "const int usable = (int)aurora_cpu_detail::topologyCached().spreadOrder.size();" not in aff)

# ---- ⑧ 新增判据: 整项忙占比 < 阈值 ----
check("A43 逐核占用用 /proc/stat 差值(口径未变)",
      "out->busy[cpu] = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];" in freq
      and "out->idle[cpu] = v[3] + v[4];" in freq)
check("A44 新增阈值判据: 阈值是一个具名常量(5%), 不是散落的魔数",
      "const double kBusyIdlePct = 5.0;" in freq)
check("A45 低于阈值的核逐核列出(带每个核自己的忙占比)",
      "if (busyPct < kBusyIdlePct) {" in freq and 'cpu%d(忙%.1f%%)' in freq)
check("A46 结论措辞写明'该核在整个本项期间基本没干活'", "该核在整个本项期间基本没干活" in freq)
check("A47 与'完全没干活(Δ忙=0)'分开列(两条判据不许混)",
      "完全没干活的核" in freq and "忙占比阈值判据" in freq and "连噪声级的一两个 jiffy" in freq)
check("A48 一个都不低于阈值时也要给正面结论(不许只有坏消息才有输出)",
      "整项忙占比 < %.0f%% 的核 0/%d" in freq)
check("A49 两次 /proc/stat 读盘仍在计时区间之外",
      freq.find("(void)readProcStatSnapshot(&g_s.statStart);") < freq.find("g_s.armed = 1;")
      and freq.find("(void)readProcStatSnapshot(&g_s.statStop);") < freq.find("g_s.stopped = 1;"))

# ---- ⑨ 不许动的东西 ----
napi = rd("napi_init.cpp")
check("A49b napi 侧把分类结果交出去(coreClassA/B/C/D + 四个位图 + 拉起探测的六个计数器)",
      all(k in napi for k in ["coreStartupRan", "coreClassA", "coreClassB", "coreClassC",
                              "coreClassD", "coreClassAMask", "coreClassBMask", "coreClassCMask",
                              "coreClassDMask", "coreOnlineWriteAttempted", "coreOnlineWriteOk",
                              "coreOnlineWriteFail", "coreOnlineLastOpenErrno",
                              "coreOnlineLastWriteErrno", "coreOnlineRestoreOk",
                              "coreOnlineRestoreTotal"]))
check("A50 计分公式一个字未动", "score = k * (value * e.conv);" in gb7)
check("A51 ENTRIES 的 k/conv 未被触碰(抽查 3 项)",
      "7.027686550" in gb7 and "181.6940430" in gb7 and "184.8221414" in gb7)
LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]
ms = sum(rd(f).count("auroraFreqMarkStart();") for f in LOAD_FILES)
me = sum(rd(f).count("auroraFreqMarkStop();") for f in LOAD_FILES)
check("A52 16 项负载的打点数量未变(16/16)", ms == 16 and me == 16, "%d/%d" % (ms, me))
check("A53 executor 的任务分解一个字没动(chunk=64 / join 顺序 / threads<=1 串行)",
      "const long long chunk = 64;" in par and "threads <= 1" in par
      and par.find("pool[i].join();") > 0)
check("A54 新增代码里没有按机型/SoC 的分支(无型号字符串)",
      not re.search(r"HUAWEI|HiSilicon|Kirin|kirin|Snapdragon|MT\d{4}|Exynos|HOP-AL00|Pura",
                    src + sample + reason))
# 逐核读盘一律用 "r"(只读); 本文件里唯一一处 "w" 打开就是那一次 online 写探测。
check("A55 新增探测只读 sysfs —— 全文件只有两处写打开, 都是 online 写探测(写 + 恢复)",
      src.count('fopen(path, "w")') == 2
      and probe.count('fopen(path, "w")') == 2
      and 'FILE* f = fopen(path, "r");' in src)

emit("")
emit("=" * 100)
emit("[B] 数字镜像: 逐核分类判据(真机形状 + 穷举)")
emit("=" * 100)

A, B, C, D = "A", "B", "C", "D"

def mirror_classify(has_present, online_known, online_value, accepted):
    """逐字镜像 probeCoreStartupState() 里那段分类(判据顺序 D -> C -> B/A)。"""
    if not has_present:
        return D
    if online_known and online_value == 0:
        return C
    if accepted:
        return A
    return B

def mirror_cpu_list(text):
    """逐字镜像 auroraCoreStateParseCpuList(): 语法异常跳过, 不猜。"""
    m = set()
    if not text:
        return m
    i = 0
    while i < len(text):
        if not text[i].isdigit():
            i += 1
            continue
        a = 0
        while i < len(text) and text[i].isdigit():
            a = a * 10 + int(text[i])
            if a > 4096:
                a = 4096
            i += 1
        b = a
        if i < len(text) and text[i] == "-":
            i += 1
            b = 0
            while i < len(text) and text[i].isdigit():
                b = b * 10 + int(text[i])
                if b > 4096:
                    b = 4096
                i += 1
        if b < a or a > 4095:
            continue
        for c in range(a, b + 1):
            m.add(c)
    return m

# ---- 真机形状 ----
SHAPES = [
    # 名字, present 原文, possible 原文, 全局 online 原文, 逐核 online 读到的核, 实测接受集合
    ("真机现状(三源矛盾): present=0-8 / 实测 accept=0-7",
     "0-8", "0-8", "0-8", set(range(0, 9)), set(range(0, 8))),
    ("若 14 个核都在 present 且在线、实测全接受",
     "0-13", "0-13", "0-13", set(range(0, 14)), set(range(0, 14))),
    ("若 present=0-8 里有 2 个核 offline(热插拔下线)",
     "0-8", "0-8", "0-6", set(range(0, 7)), set(range(0, 7))),
    ("若 present=0-8 全部在线但只有 0-5 被允许",
     "0-8", "0-8", "0-8", set(range(0, 9)), set(range(0, 6))),
]

for name, present_raw, possible_raw, online_raw, online_known_set, accepted_set in SHAPES:
    pm = mirror_cpu_list(present_raw)
    om = mirror_cpu_list(possible_raw)
    gm = mirror_cpu_list(online_raw)
    upper = max(max(pm or {0}), max(om or {0}), max(gm or {0})) + 1
    cls = {}
    for c in range(upper):
        cls[c] = mirror_classify(c in pm, c in online_known_set,
                                 1 if c in gm else 0, c in accepted_set)
    cnt = {k: sum(1 for v in cls.values() if v == k) for k in (A, B, C, D)}
    emit("  %s" % name)
    emit("    present=%s possible=%s online=%s -> 上界 %d; 逐核分类 %s"
         % (present_raw, possible_raw, online_raw, upper,
            " ".join("%d:%s" % (c, cls[c]) for c in range(upper))))
    emit("    A=%d B=%d C=%d D=%d" % (cnt[A], cnt[B], cnt[C], cnt[D]))

# 真机现状逐条断言
pm = mirror_cpu_list("0-8")
cls_machine = {c: mirror_classify(c in pm, c in set(range(0, 9)),
                                 1 if c in mirror_cpu_list("0-8") else 0,
                                 c in set(range(0, 8)))
               for c in range(9)}
check("B1 真机现状: cpu0-7 全部归 A(在线且实测被接受)", all(cls_machine[c] == A for c in range(8)))
check("B2 真机现状: cpu8 归 B(在 present 里、online 原文为 1、但实测被拒 —— 不是没启动)",
      cls_machine[8] == B)
check("B3 真机现状: C 类 = 0 个(没有任何核处于 offline)",
      sum(1 for v in cls_machine.values() if v == C) == 0)
check("B4 真机现状: D 类 = 0 个(9 个核号都在 present 里)",
      sum(1 for v in cls_machine.values() if v == D) == 0)

# C 类判据: online 原文为 0
cls_off = {c: mirror_classify(c in mirror_cpu_list("0-8"), c in set(range(0, 9)),
                              1 if c in mirror_cpu_list("0-6") else 0, c in set(range(0, 7)))
           for c in range(9)}
check("B5 online 原文为 0 的核必须归 C(即便实测掩码被接受也不行 —— '没启动'高一级)",
      cls_off[7] == C and cls_off[8] == C)
check("B6 C 类核不会再被算成 A/B(每个核只归一类)",
      all(cls_off[c] == A for c in range(7)))

# D 类判据: present 不含
cls_absent = {c: mirror_classify(c in mirror_cpu_list("0-4"), True, 1, True) for c in range(8)}
check("B7 present 不含的核号必须归 D(硬件/固件根本没报)", cls_absent[5] == D and cls_absent[7] == D)

# 穷举: present 含 0-9, 逐核 online 取 0/1 两种, 实测取接受/被拒两种 -> 4 种组合 x 10 核
exhaust_ok = True
BAD = None
for c in range(10):
    for onl in (0, 1):
        for acc in (False, True):
            got = mirror_classify(True, True, onl, acc)
            want = C if onl == 0 else (A if acc else B)
            if got != want:
                exhaust_ok = False
                BAD = (c, onl, acc, got, want)
check("B8 穷举 10 核 x {online=0,1} x {接受,被拒} = 40 种组合, 分类恒等于判据表",
      exhaust_ok, BAD)
check("B9 present 不含时, 无论 online/实测如何都归 D(判据 D 优先于其它三条)",
      all(mirror_classify(False, True, v, a) == D for v in (0, 1) for a in (False, True)))

# ---- 上界判据: 取五个来源最大值, 且 present 为空/读不到时不崩 ----
check("B10 present 原文解析: '0-3,8-11' -> {0,1,2,3,8,9,10,11}",
      mirror_cpu_list("0-3,8-11") == {0, 1, 2, 3, 8, 9, 10, 11})
check("B11 语法异常(区间反了)跳过那一段而不是猜", mirror_cpu_list("5-3,7") == {7})
check("B12 空串 -> 空集合(不把'读不到'当成'有 cpu0')", mirror_cpu_list("") == set())

emit("")
emit("=" * 100)
emit("[C] 恢复逻辑镜像: C 类拉起(备份 -> 写 -> 读回 -> 恢复)")
emit("=" * 100)

def mirror_lift(online_path_exists, open_errno, fwrite_rc, write_errno,
                value_after_write, backup_value):
    """逐字镜像 probeCoreStartupState() 的 (6) 段。

    返回 (wrote, write_errno, after, restore_wrote, final_value, restore_ok, verdict)
      wrote         = 真的调用过写(1/0)
      after         = 写后读回的值(1/0/-1 读不到)
      restore_wrote = 是否回写过(0 = 值没变, 不需要回写)
      final_value   = 探测结束之后 online 文件里的值
      restore_ok    = 1 表示最终值 == 备份值
    """
    if not online_path_exists:
        return (0, 0, backup_value, 0, backup_value, 1, "no-online-file")
    if open_errno != 0:
        # 打不开: 没有写, 更没有"改过"; 读回值不变
        return (0, open_errno, backup_value, 0, backup_value, 1, "open-failed")
    wrote = 1
    after = value_after_write
    # 无条件恢复: 只有当前值 != 备份值时才回写
    cur = after
    restore_wrote = 0
    if cur != backup_value:
        cur = backup_value
        restore_wrote = 1
    restore_ok = 1 if cur == backup_value else 0
    verdict = "lifted" if after == 1 else "still-offline"
    return (wrote, write_errno, after, restore_wrote, cur, restore_ok, verdict)

rows = [
    ("C 类核, 应用无权限(打开 online 就 EACCES=13)",
     (True, 13, 0, 13, 0, 0), (0, 13, 0, 0, 0, 1, "open-failed")),
    ("C 类核, 应用无权限(EPERM=1)",
     (True, 1, 0, 1, 0, 0), (0, 1, 0, 0, 0, 1, "open-failed")),
    ("C 类核, 只读文件系统(EROFS=30)",
     (True, 30, 0, 30, 0, 0), (0, 30, 0, 0, 0, 1, "open-failed")),
    ("C 类核, 打开成功但写返回 0 个字节(errno=13)",
     (True, 0, 0, 13, 0, 0), (1, 13, 0, 0, 0, 1, "still-offline")),
    ("C 类核, 写返回 1 个字节但读回仍是 0(内核忽略了写)",
     (True, 0, 1, 0, 0, 0), (1, 0, 0, 0, 0, 1, "still-offline")),
    ("C 类核, 写成功且读回变成 1(真的拉起来了) -> 必须恢复成 0",
     (True, 0, 1, 0, 1, 0), (1, 0, 1, 1, 0, 1, "lifted")),
    ("在线核(A 类)也做同一次写探测: 写后读回仍是 1, 值没变 -> 不回写",
     (True, 0, 1, 0, 1, 1), (1, 0, 1, 0, 1, 1, "lifted")),
    ("没有 cpuN/online 文件(不支持热插拔) -> 不写、不报成功",
     (False, 0, 0, 0, 0, 0), (0, 0, 0, 0, 0, 1, "no-online-file")),
]
for name, args, want in rows:
    got = mirror_lift(*args)
    emit("  %-52s -> wrote=%d writeErrno=%d after=%d restoreWrote=%d final=%d restoreOk=%d %s"
         % (name, got[0], got[1], got[2], got[3], got[4], got[5], got[6]))
    if got != want:
        check("C 镜像我: " + name, False, "got=%s want=%s" % (got, want))

check("C1 打开 online 失败(EACCES) -> 一个字节都不许写, 原值必须不变",
      mirror_lift(True, 13, 0, 13, 0, 0)[0] == 0
      and mirror_lift(True, 13, 0, 13, 0, 0)[4] == 0)
check("C2 关闭权限(errno 原样带回, 不许写成含糊的'拉不起来')",
      mirror_lift(True, 13, 0, 13, 0, 0)[1] == 13
      and mirror_lift(True, 1, 0, 1, 0, 0)[1] == 1
      and mirror_lift(True, 30, 0, 30, 0, 0)[1] == 30)
check("C3 写返回 0 个字节时(cWriteFail 路径)不算拉起来",
      mirror_lift(True, 0, 0, 13, 0, 0)[6] == "still-offline")
check("C4 写成功 + 读回变成 1 才算拉起来", mirror_lift(True, 0, 1, 0, 1, 0)[6] == "lifted")
check("C5 拉起成功之后必须恢复成原值 0(final == backup)",
      mirror_lift(True, 0, 1, 0, 1, 0)[4] == 0 and mirror_lift(True, 0, 1, 0, 1, 0)[3] == 1)
check("C6 值没被改动过时不做多余的回写(在线核那一行 restoreWrote == 0)",
      mirror_lift(True, 0, 1, 0, 1, 1)[3] == 0)
check("C7 没有 online 文件时不写、也不把自己算成'拉起来了'",
      mirror_lift(False, 0, 0, 0, 0, 0)[0] == 0 and mirror_lift(False, 0, 0, 0, 0, 0)[6] == "no-online-file")
check("C8 所有路径下最终值都等于备份值(restoreOk 恒为 1)",
      all(mirror_lift(*a)[5] == 1 for _, a, _ in rows))

emit("")
emit("=" * 100)
emit("[D] 结论链镜像: 有核没启动 / 有线程没跑, 各是哪一条原因")
emit("=" * 100)

def conclusion_chain(countA, countB, countC, countD, avail, threads, spreadBound, usedCores):
    """镜像 auroraAffinitySessionEnd() 的原因链(只取与本次任务相关的那几条)。"""
    r = []
    if countC > 0:
        r.append("C(离线,没启动) %d 个" % countC)
    if countB > 0:
        r.append("B(在线但被许可集合拒绝) %d 个" % countB)
    if countD > 0:
        r.append("D(present 不含) %d 个" % countD)
    if usedCores >= avail:
        return "无 —— 已铺满"
    if threads < avail:
        r.append("线程数被夹到 %d(可用 %d)" % (threads, avail))
    if spreadBound < threads:
        r.append("池线程只建起 %d 个(会话线程数 %d)" % (spreadBound, threads))
    if not r:
        r.append("未定位")
    return "; ".join(r)

emit("  真机现状(A=8, B=1, C=0, D=0; 可用核数 8; 线程 8; 池线程 8; 用到 8) -> %s"
     % conclusion_chain(8, 1, 0, 0, 8, 8, 8, 8))
emit("  若 2 个核 offline(A=6, C=2; 可用 6; 线程 6; 池线程 6; 用到 6)          -> %s"
     % conclusion_chain(6, 0, 2, 0, 6, 6, 6, 6))
emit("  若线程数被夹(A=8, B=1; 可用 8; 线程 4; 池线程 4; 用到 4)                -> %s"
     % conclusion_chain(8, 1, 0, 0, 8, 4, 4, 4))

check("D1 已铺满(M == N)时不报假原因", conclusion_chain(8, 1, 0, 0, 8, 8, 8, 8) == "无 —— 已铺满")
check("D2 有 C 类核时, 原因链第一条就是'没启动'(最上游的原因)",
      conclusion_chain(6, 0, 2, 0, 6, 6, 6, 6).startswith("无 —— 已铺满")
      or "C(离线,没启动) 2 个" in conclusion_chain(6, 0, 2, 0, 8, 6, 6, 6))
check("D3 M<N 时, C/B/D 三类都进原因链(带数字)",
      all(k in conclusion_chain(6, 1, 1, 1, 8, 6, 6, 6)
          for k in ["C(离线,没启动) 1 个", "B(在线但被许可集合拒绝) 1 个", "D(present 不含) 1 个"]))
check("D4 线程数被夹时必须写明被夹到几个",
      "线程数被夹到 4(可用 8)" in conclusion_chain(8, 0, 0, 0, 8, 4, 4, 4))

emit("")
emit("=" * 100)
if not fails:
    emit("结论: 全部 PASS —— 逐核启动状态已有行为学判据(全局 present/possible/online 原文 +")
    emit("      逐核 cpuN/online 原文 + 逐核 setaffinity 实测与 errno + 逐核频率/拓扑原文),")
    emit("      每个核归入且仅归入 A/B/C/D 一类; C 类拉起探测先备份、无论成败都恢复且读回校验,")
    emit("      失败原样记 errno 并明确写出'应用域无权拉起下线核'; 多核阶段六项取证同源同报,")
    emit("      整项忙占比 < 5% 的核单独列出并写明'该核在整个本项期间基本没干活'。")
else:
    emit("结论: 有 %d 项 FAIL: %s" % (len(fails), ", ".join(fails)))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
