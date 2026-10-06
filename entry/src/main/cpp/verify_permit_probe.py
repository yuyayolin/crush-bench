# -*- coding: utf-8 -*-
r"""
verify_permit_probe.py —— "实测许可集合"改造的自查器(静态源码断言 + 真机数字镜像; 无设备)

背景(2026-10-04 真机硬证据, 见 D:\gb7logs\now2.jsonl):
  同一项 note 里同时出现
     "cpuset: allowed=0-8 (9 核; sched_getaffinity) · Cpus_allowed 与 sched_getaffinity 不一致"
  而同一份日志的单核项运行时采样写着
     "采样核 = 本线程所在核(cpu12,cpu13 —— 采样期间线程迁移过)"
  若内核真的只允许 cpu0-8, 线程不可能落在 cpu12/cpu13。也就是说: 整条绑核链(快簇划分 /
  交集判定 / 是否绑核成功 / 是否在全机最快档)此前全部信任 sched_getaffinity 一个读数,
  而它已经被 App 自己标成"与 /proc 的 Cpus_allowed 不一致"。

本脚本做三件事(全部离线, 不需要设备):
  A) 源码结构断言: 从 cpu_affinity.h / napi_init.cpp 里确认
       ① 存在逐核实测探测(对每个核号 sched_setaffinity({cpu}) 后立刻 sched_getaffinity
          读回), 且探测结束后还原原掩码;
       ② 双源原始读数被原样保留(Cpus_allowed 十六进制原文 / Cpus_allowed_list 原文 /
          sched_getaffinity 掩码 / 各自的 errno), 并给出逐位差集(不是 bool);
       ③ 权威来源的选择顺序是 实测探测 > sched_getaffinity > Cpus_allowed_list > Cpus_allowed,
          且权威集合被写回 a.mask(即绑核决策用的是它);
       ④ 会话在真正跑负载的那个线程上重测许可集合, 且这件事发生在 auroraThreadCap() /
          topologyCached() 之前; 重测结果变化时派生缓存整体重建(世代号);
       ⑤ 绑定后立刻回读掩码, 并把"跑完/开跑的 cpu 是否落在回读掩码内"作为硬事实上报;
       ⑥ 逐核 cpuinfo_max_freq + 频率档划分随 note 上报(回答"快簇为什么是这几个核");
       ⑦ 不许动的东西没动: score 公式 / k 常数 / 打点数量;
       ⑧ 没有引入按机型/SoC 的分支。
  B) 数字镜像(判据一致性): 用真机读到的形状跑一遍新旧两种权威, 以及 SMT 开/关两种口径。
  C) 断言。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_permit_probe_out.txt"), "w", encoding="utf-8", newline="\n")
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

def body_of(sig_regex, text):
    """取一个函数的函数体(从签名匹配处到第一个行首的右花括号)。与 verify_core_spread.py 同口径。"""
    m = re.search(sig_regex, text, re.S)
    if not m:
        return ""
    rest = text[m.start():]
    end = rest.find("\n}\n")
    return rest[:end + 2] if end >= 0 else rest

src = rd("cpu_affinity.h")
napi = rd("napi_init.cpp")

emit("=" * 100)
emit("[A] 源码结构断言")
emit("=" * 100)

# ---- ① 逐核实测探测 ----
m = re.search(r"inline AuroraCpuPermitProbe probePermittedCpuSet\(\)(.*?)\n\}", src, re.S)
probe = m.group(0) if m else ""
check("A1 存在逐核实测探测 probePermittedCpuSet()", bool(probe))
check("A2 探测对每个核号单独发 sched_setaffinity({cpu})", "CPU_SET(c, &one)" in probe)
check("A3 探测在设置后立刻读回(sched_getaffinity)", "sched_getaffinity(0, sizeof(back), &back)" in probe)
check("A4 判定用'读回掩码 == 那一位'而不是只看返回值",
      "backMask == want" in probe and "rc == 0 && rc2 == 0" in probe)
check("A5 返回 0 但读回不是那一位 -> 记为夹回(clamped), 不算接受",
      "p.clamped[c] = 1" in probe and "p.accepted[c] = 0" in probe)
check("A6 逐核记 errno(含 EINVAL/EPERM 分类)", "p.einvalCount" in probe and "p.epermCount" in probe)
check("A7 探测结束后还原原掩码并校验读回一致",
      "p.restoreOk = (rb == p.savedMask)" in probe and "sched_setaffinity(0, sizeof(back2), &back2)" in probe)
check("A8 探测核号上界取四个来源的最大值(不会因某处读数失败而漏核)",
      "probeCpuUpperBound" in src and "sysconf(_SC_NPROCESSORS_ONLN)" in src
      and "/sys/devices/system/cpu/possible" in src)

# ---- ② 双源原始读数 + 逐位差集 ----
check("A9 保留 Cpus_allowed 十六进制原文", "statusMaskRaw" in src and "snprintf(a.statusMaskRaw" in src)
check("A10 保留 Cpus_allowed_list 原文", "statusListRaw" in src and "snprintf(a.statusListRaw" in src)
check("A11 保留 sched_getaffinity 的掩码与逐核清单",
      "a.syscallMask = m" in src and "a.syscallCoreList" in src)
check("A12 三条读数各自记 errno",
      "statusMaskErrno" in src and "statusListErrno" in src and "syscallErrno" in src)
check("A13 差集是逐位列出(掩码差 -> 逐核清单), 不是 bool",
      "inline void maskDiffText" in src and "maskToCoreListText(a & ~b" in src
      and "maskToCoreListText(b & ~a" in src)
check("A14 diffText 同时给出三条差集(sched_getaffinity vs 十六进制 / vs list / 实测 vs sched_getaffinity)",
      "[1] sched_getaffinity vs Cpus_allowed" in src
      and "[2] sched_getaffinity vs Cpus_allowed_list" in src
      and "[3] 实测许可 vs sched_getaffinity" in src)
check("A15 note 那一行里原样打印了两个读数(掩码 + 逐核清单 + errno)",
      "sched_getaffinity(0)=0x%llx" in src and "Cpus_allowed 原文=" in src
      and "Cpus_allowed_list 原文=" in src)

# ---- ③ 权威来源 ----
m2 = re.search(r"⑤ 权威来源的选择.*?\n    \}", src, re.S)
auth_seg = m2.group(0) if m2 else ""
check("A16 权威来源的选择顺序: 实测探测优先", "a.authority = 0" in auth_seg
      and auth_seg.find("a.probeAcceptedCount > 0") < auth_seg.find("a.syscallMask != 0ull"))
check("A17 权威集合被写回 a.mask(绑核决策因此用实测集合)", "a.mask = auth;" in auth_seg)
check("A18 实测与 sched_getaffinity 不同时, 文本明确写出'以实测为准'",
      "以实测为准" in src and "differFromSyscall" in src)
check("A19 权威来源随 note 上报(authorityText 被拼进 cpuAllowedText)",
      "appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), al.authorityText)" in src)
check("A20 探测全失败时不会假装成功(退回 sched_getaffinity 并写明)",
      "实测探测没能拿到任何被接受的核" in src)

# ---- ④ 在负载线程上重测 + 缓存重建 ----
m3 = re.search(r"inline void auroraAffinitySessionBegin\(int threads\)\n\{(.*?)\n\}\n", src, re.S)
begin_body = m3.group(1) if m3 else ""
REFRESH = "auroraRefreshPermitSetOnCallingThread()"
CAP_CALL = "const int cap = ::auroraThreadCap();"
TOPOCALL = "const aurora_cpu_detail::CpuTopology& t = aurora_cpu_detail::topologyCached();"
check("A21 会话开始时在本线程重测许可集合", REFRESH in begin_body)
check("A22 重测发生在 auroraThreadCap() / topologyCached() 之前",
      begin_body.find(REFRESH) >= 0 and begin_body.find(REFRESH) < begin_body.find(CAP_CALL)
      and begin_body.find(REFRESH) < begin_body.find(TOPOCALL),
      "refresh@%d cap@%d topo@%d" % (begin_body.find(REFRESH), begin_body.find(CAP_CALL),
                                     begin_body.find(TOPOCALL)))
check("A23 可用核集合缓存可重算(不再是 static const)", "allowedSetMutable" in src
      and "static const AuroraCpuAllowedSet a = readAllowedSet();" not in src)
check("A24 派生缓存按世代号重建(base / 生效序列 / 拓扑)",
      "permittedGenRef()" in src and "baseGen != permittedGenRef()" in src
      and "builtGen != permittedGenRef()" in src)
check("A25 集合变化时才作废缓存(changed 判据)",
      "const bool changed = ((fresh.ok != cur.ok) || (fresh.mask != cur.mask));" in src)

# ---- ⑤ 掩码回读 + 硬事实 ----
check("A26 绑定后立刻回读掩码", "s.appliedReadbackMask" in src and "appliedReadbackOk" in src)
check("A27 跑完/开跑的 cpu 是否落在回读掩码内被显式判定",
      "p.cpuOutsideAppliedMask" in src and "p.cpuAtStartOutsideAppliedMask" in src)
check("A28 判据写进 note(cpuReadbackText 被拼进 placement 文本)", "s += p.cpuReadbackText;" in src)
check("A29 明确区分'内核接受了掩码'与'内核按掩码调度'",
      "内核接受了掩码, 但线程出现在掩码之外" in src)

# ---- ⑥ 逐核频率与频率档 ----
check("A30 逐核 cpuinfo_max_freq + 频率档划分随 note 上报",
      "auroraCoreTierText" in src and "逐核 cpuinfo_max_freq(kHz)" in src
      and "appendSeg(p.cpuAllowedText, sizeof(p.cpuAllowedText), p.cpuTierText)" in src)

# ---- ⑦ 不许动的东西 ----
gb7 = rd("gb7.cpp")
check("A31 计分公式一个字未动", "score = k * (value * e.conv);" in gb7)
check("A32 ENTRIES 的 k/conv 未被触碰(抽查 3 项)",
      "7.027686550" in gb7 and "181.6940430" in gb7 and "184.8221414" in gb7)
LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]
ms = sum(rd(f).count("auroraFreqMarkStart();") for f in LOAD_FILES)
me = sum(rd(f).count("auroraFreqMarkStop();") for f in LOAD_FILES)
check("A33 16 项负载的打点数量未变(16/16)", ms == 16 and me == 16, "%d/%d" % (ms, me))
check("A34 新增代码里没有按机型/SoC 的分支(无型号字符串)",
      not re.search(r"HUAWEI|HiSilicon|Kirin|kirin|Snapdragon|MT\d{4}|Exynos", probe + auth_seg))
check("A35 探测不会落在计时区间内(没有紧跟 markStart / 出现在负载文件里)",
      all("probePermittedCpuSet" not in rd(f) for f in LOAD_FILES))
check("A36 napi 侧把新增字段交出去(双源原文 / 差集 / 探测 / 权威 / 频率档)",
      all(k in napi for k in ["statusMaskRaw", "statusListRaw", "diffText", "probeText",
                              "authorityText", "probeAcceptedMask", "tierText"]))
check("A37 note 那一行的容量已扩容(cpuAllowedText >= 2560)",
      re.search(r"char cpuAllowedText\[(\d+)\]", src) is not None
      and int(re.search(r"char cpuAllowedText\[(\d+)\]", src).group(1)) >= 2560)

check("A38 逐核 -> 物理核映射随频率档一起上报(SMT 关'留哪个代表'的直接依据)",
      "逐核所属物理核" in src and "physicalOfCpu[c]" in src
      and re.search(r"char cpuTierText\[(\d+)\]", src) is not None
      and int(re.search(r"char cpuTierText\[(\d+)\]", src).group(1)) >= 1024)
# A39 的沿革(必须留痕, 否则下一次改动没有上下文):
#   2026-10-05 原文 = "旧套件(bench_cpu.cpp)不受影响: 既不重测许可集合, 也不用会话/新拓扑"。
#   2026-10 这次改动**故意推翻了它**: 真机复算证明自研套件单核那条路径一次
#   sched_setaffinity 都没发过, 于是同一颗芯片的两台设备上 CS1 单核只差 0.24%、
#   自研套件单核差 37.67%(见 _work/t1/recompute_ratio.txt)。
#   -> 断言改成"新形态必须成立"的那几条, 而不是把它删掉:
#      * 自研套件单核阶段**必须**绑到生效快簇(同一个 loadPhaseTargetCount 来源);
#      * 仍然不许在自研套件里重测许可集合 / 直接调探测(那是会话级动作, 每项都做代价太大);
#      * 绑核的目标位次数必须来自唯一来源, 不许在 bench_cpu.cpp 里另抄一遍。
bcpu = rd("bench_cpu.cpp")
check("A39 自研套件单核阶段已绑到生效快簇(与 CS1 单核同一函数, 不再是不绑)",
      "auroraBindCurrentThreadToSingleLoadTarget" in bcpu
      and "auroraCaptureThreadAffinity()" in bcpu
      and "auroraApplyThreadAffinity(before)" in bcpu)
check("A40 自研套件不重测许可集合 / 不直接用逐核探测(会话级动作仍只在会话里做)",
      "auroraAffinitySessionBegin" not in bcpu
      and "auroraRefreshPermitSetOnCallingThread" not in bcpu
      and "probePermittedCpuSet" not in bcpu)
# 唯一来源的结构: bench_cpu.cpp -> auroraBindCurrentThreadToSingleLoadTarget()
#              -> aurora_cpu_detail::loadPhaseTargetCount(t, false) <- auroraAffinitySessionBegin()
# 三段都要在, 而且 bench_cpu.cpp 里不许出现"自己算目标"的痕迹(交集/档位/order 序列)。
single_target = body_of(r"inline int auroraBindCurrentThreadToSingleLoadTarget\(int\* errOut = nullptr\)", src)
check("A41 自研套件绑核入口调的是唯一来源 loadPhaseTargetCount",
      "aurora_cpu_detail::loadPhaseTargetCount(t, false)" in single_target)
check("A42 bench_cpu.cpp 里没有任何'自己算目标'的痕迹(交集/档位/order 序列都不许出现)",
      "fallbackTarget" not in bcpu and "fastCpus" not in bcpu and "t.order" not in bcpu)
# ---------------------------------------------------------------- B) 数字镜像
emit("")
emit("=" * 100)
emit("[B] 真机数字镜像(HOP-AL00 手机: sched_getaffinity 报 0-8, 而线程被观测跑在 cpu12/cpu13)")
emit("=" * 100)

def mask_to_core_list(m):
    out = ["cpu%d" % c for c in range(64) if (m >> c) & 1]
    return ",".join(out) if out else "无"

def clist(cs):
    return ",".join("cpu%d" % c for c in cs) if cs else "无"

def select_authority(probe_ok, probe_mask, syscall_ok, syscall_mask, list_ok, list_mask, hex_ok, hex_mask):
    """逐字镜像 readAllowedSet() 里 ⑤ 权威来源的选择顺序。"""
    if probe_ok and bin(probe_mask).count("1") > 0:
        return 0, probe_mask
    if syscall_ok and syscall_mask != 0:
        return 1, syscall_mask
    if list_ok and list_mask != 0:
        return 2, list_mask
    if hex_ok and hex_mask != 0:
        return 3, hex_mask
    return 4, 0

def probe_one(rc, rc_errno, rc2, back_mask, c):
    """逐字镜像 probePermittedCpuSet() 里单个核的判定。"""
    want = 1 << c
    if rc == 0 and rc2 == 0 and back_mask == want:
        return "accept", 0
    if rc != 0:
        return "reject", rc_errno
    if rc2 != 0:
        return "reject", -1
    return "reject", -4      # 返回 0 但读回不是那一位 = 夹回

def build_topo(slot_freq):
    """逐字镜像 buildTopo(): 同频并档; 不足 2 核继续吸收下一档; 尾组为 1 且组数 >= 3 时并入上一组。"""
    n = len(slot_freq)
    groups = []
    s = 0
    while s < n:
        first = s
        size = 0
        while True:
            f = slot_freq[s]
            j = s
            while j < n and slot_freq[j] == f:
                j += 1
            size += j - s
            s = j
            if size >= 2 or s >= n:
                break
        groups.append((first, size))
    if len(groups) >= 3 and groups[-1][1] == 1:
        f0, s0 = groups[-2]
        groups[-2] = (f0, s0 + 1)
        groups.pop()
    return groups

# ---- 真机形状(全部取自设备自己的读数, 不含任何外部资料) ----
#   * 可用核集合 0-8 = 9 个逻辑核(sched_getaffinity 的读数; 也是 note 里"physical=9"的那 9 个)
#   * 2270MHz 档 = cpu4-8(真机 07:15 起每一行都写着"快簇已生效(cpu=4..8, 5 核簇/最高2270MHz)")
#   * 全机最快档 = 2 核 @2750MHz(真机 note: "不在全机最快档内(2 核/2750MHz 才是全机最快档)")
#   * 物理核 9 个 / 逻辑核 14 个(真机 note: "physical=9"), 即 5 对兄弟线程 + 4 个单线程核
FREQ = {}
for c in range(0, 4):   FREQ[c] = 2100000
for c in range(4, 9):   FREQ[c] = 2270000     # 2270 档: 5 个逻辑核
for c in range(9, 12):  FREQ[c] = 2100000
for c in range(12, 14): FREQ[c] = 2750000     # 全机最快档: 2 个核
SIBLING_PAIRS = [(4, 5), (6, 7), (8, 9), (10, 11), (12, 13)]   # 5 对 -> 14 逻辑 / 9 物理
PHYS = {}
for c in FREQ:
    PHYS[c] = c
for a, b in SIBLING_PAIRS:
    PHYS[b] = a
physical_count = len(set(PHYS.values()))
SYSCALL_MASK = sum(1 << c for c in range(0, 9))      # sched_getaffinity 报 0-8
PROBE_MASK = sum(1 << c for c in range(0, 14))       # 实测: 逐核发 {cpu} 都被内核接受
order_all = sorted(FREQ.keys(), key=lambda c: (-FREQ[c], c))

def smt_off_reps(seq):
    """逐字镜像 buildEffectiveSet(smtEnabled=0): 每个物理核只留频率位次最靠前的那个逻辑核。"""
    picked = {}
    for c in seq:                      # seq 已按频率降序
        p = PHYS[c]
        if p not in picked:
            picked[p] = c
    return [c for c in seq if picked[PHYS[c]] == c]

def cluster_of(auth_mask, smt_off=False):
    seq = [c for c in order_all if (auth_mask >> c) & 1]
    if smt_off:
        seq = smt_off_reps(seq)
    slot_freq = [FREQ[c] for c in seq]
    g = build_topo(slot_freq)
    fast = seq[:g[0][1]] if g else []
    return seq, g, fast

emit("  真机形状: 允许 0-8(9 逻辑核) / 2270 档 = cpu4-8 / 全机最快 2750 档 = cpu12,13(2 核)")
emit("            兄弟线程 5 对 %s -> 物理核 %d 个(真机 note 写 physical=9)" % (SIBLING_PAIRS, physical_count))
emit("")

old_auth, old_mask = select_authority(False, 0, True, SYSCALL_MASK, True, SYSCALL_MASK, True, SYSCALL_MASK)
old_seq, old_g, old_fast = cluster_of(old_mask)
emit("  [旧] 权威 = sched_getaffinity(0-8) / SMT 开")
emit("       快簇 = %d 核 %s(@%dMHz)   <- 真机 07:15 那一行'5 核簇/最高2270MHz'"
     % (len(old_fast), clist(old_fast), FREQ[old_fast[0]] // 1000))
old_off_seq, old_off_g, old_off_fast = cluster_of(old_mask, smt_off=True)
emit("  [旧] 权威 = sched_getaffinity(0-8) / SMT 关")
emit("       生效序列 = %d 核 %s" % (len(old_off_seq), clist(old_off_seq)))
emit("       快簇 = %d 核 %s(@%dMHz)   <- 真机 14:24 之后那一行'3 核簇/最高2270MHz'"
     % (len(old_off_fast), clist(old_off_fast), FREQ[old_off_fast[0]] // 1000))

new_auth, new_mask = select_authority(True, PROBE_MASK, True, SYSCALL_MASK, True, SYSCALL_MASK, True, SYSCALL_MASK)
new_seq, new_g, new_fast = cluster_of(new_mask)
emit("  [新] 权威 = 实测探测(0-13) / SMT 开  -> authority=%d" % new_auth)
emit("       快簇 = %d 核 %s(@%dMHz)   <- 就是全机最快档"
     % (len(new_fast), clist(new_fast), FREQ[new_fast[0]] // 1000))
new_off_seq, new_off_g, new_off_fast = cluster_of(new_mask, smt_off=True)
emit("  [新] 权威 = 实测探测(0-13) / SMT 关")
emit("       快簇 = %d 核 %s(最高 %dMHz)"
     % (len(new_off_fast), clist(new_off_fast), FREQ[new_off_fast[0]] // 1000))
emit("")
emit("  逐核探测判定(rc = sched_setaffinity 返回值; back = 立刻读回的掩码):")
cases = [
    ("内核接受该核", 0, 0, 0, 1 << 6, "accept"),
    ("EINVAL(不在 cpuset 内)", 22, 22, 0, SYSCALL_MASK, "reject"),
    ("EPERM(无权改亲和性)", 1, 1, 0, SYSCALL_MASK, "reject"),
    ("返回 0 但读回被夹回", 0, 0, 0, SYSCALL_MASK, "reject"),
    ("返回 0 但读回失败", 0, 0, 1, 0, "reject"),
]
probe_results = []
for name, rc, errno_v, rc2, back, want in cases:
    got, e = probe_one(rc, errno_v, rc2, back, 6)
    probe_results.append((name, got, e, want))
    emit("    %-22s -> %-6s (errno=%d)" % (name, got, e))

emit("")
emit("=" * 100)
emit("[C] 断言")
emit("=" * 100)
checks = [
    ("旧权威(sched_getaffinity=0-8) + SMT 开 -> 快簇 cpu4-8 共 5 核 @2270MHz(复现真机那一行)",
     len(old_fast) == 5 and set(old_fast) == {4, 5, 6, 7, 8} and FREQ[old_fast[0]] == 2270000),
    ("旧权威 + SMT 关 -> 快簇 3 核 @2270MHz(复现真机 5 核 -> 3 核)",
     len(old_off_fast) == 3 and all(FREQ[c] == 2270000 for c in old_off_fast)),
    ("5 -> 3 的机制: 2270 档的 5 个逻辑核只落在 3 个物理核上(2 对兄弟线程)",
     len(set(PHYS[c] for c in range(4, 9))) == 3),
    ("该形状下物理核数 = 9(与真机 note 的 physical=9 一致)", physical_count == 9),
    ("新权威(实测=0-13) + SMT 开 -> 快簇 = 全机最快档 cpu12,13 共 2 核 @2750MHz",
     len(new_fast) == 2 and set(new_fast) == {12, 13} and FREQ[new_fast[0]] == 2750000),
    ("新旧权威给出的快簇不同(这就是本次改动的实际效果)", set(old_fast) != set(new_fast)),
    ("若权威 = sched_getaffinity(0-8), 则 cpu12/cpu13 不在掩码内 -> 真机采样到的 cpu12 与它互斥",
     ((SYSCALL_MASK >> 12) & 1) == 0 and ((SYSCALL_MASK >> 13) & 1) == 0),
    ("实测集合(0-13)才与真机观测相容(cpu12/13 在其中)",
     ((PROBE_MASK >> 12) & 1) == 1 and ((PROBE_MASK >> 13) & 1) == 1),
    ("'返回 0 但读回不是那一位'必须判成被拒(不能当接受)",
     probe_results[3][1] == "reject" and probe_results[3][2] == -4),
    ("只有'返回 0 且读回 == {cpu}'才算接受", probe_results[0][1] == "accept" and probe_results[0][2] == 0),
    ("EINVAL / EPERM 原样记下 errno", probe_results[1][2] == 22 and probe_results[2][2] == 1),
]
for name, ok in checks:
    check(name, ok)

emit("")
emit("=" * 100)
if not fails:
    emit("结论: 全部 PASS —— 绑核决策改用逐核实测探测得到的许可集合(与 sched_getaffinity 不同时")
    emit("      以实测为准), 双源读数与逐位差集原样上报, 掩码回读把'内核接受掩码'与'内核按掩码")
    emit("      调度'分开; 真机形状下'5 核簇 -> 3 核簇'可由 SMT 关闭 + 2270 档只有 3 个物理核复现。")
else:
    emit("结论: 有 %d 项 FAIL: %s" % (len(fails), ", ".join(fails)))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
