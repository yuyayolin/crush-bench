# -*- coding: utf-8 -*-
"""
CPU 物理核归并 + SMT 开关线程数的自查器 (静态源码断言 + 真机形状镜像; 无设备, 不连 hdc)

背景(2026-10-07 真机回归, 用户已复现):
  设备 HUAWEI Pura X Max / HOP-AL00: 14 逻辑核 / 9 物理核, 可用核集合 = cpu0-8(9 核),
  thread_siblings_list 14/14 核读到(errno=0)。但设备画像那一行写着
      "14 逻辑核 / 1 物理核（检测到 SMT, 当前已启用）→ 多核阶段 9 线程"
  —— physical 应该是 9, 却算成了 1。

  根因(读代码 + 真机数字共同确认):
    readTopology() 逐核读 thread_siblings_list 到栈上的 int mask[kMaxTopoCpus],
    而 readSiblingGroup() 只经 parseCpuMask 写"解析出来的前 n 个位置", 剩下的一个都不碰;
    readTopology 却按 kMaxTopoCpus 遍历整张数组。那片未初始化的栈内存真机上恰好是 0,
    而 0 是合法核号 => 每个逻辑核的兄弟表里凭空多出 cpu0 => 组号被压成 1+0 = 1
    => 14 个逻辑核全归成同一个物理核 => physical = 1
    (smtPossible = (1 < 14) 仍为 true, 所以表面还写着"检测到 SMT")。

  后果: 关掉 SMT 后实际使用集合只剩 1 个核 -> auroraThreadCap() 返回 1 ->
  多核阶段每一项 parallelism = 1(HDR 3297.7ms / Ray Tracer 6211.0ms / Clang 145.5ms),
  多核得分 5714 -> 1448, 总分 2622 -> 1412。
  SMT 开着时线程数由"可用核集合(9 核)"夹出 9, 与物理核数无关, 所以一直没暴露。

  第二个成因(同一处真机症状的另一半): "每个物理核留一个逻辑核"这一步必须在**可用核集合
  之内**做。旧写法先用全机频率表挑代表再过滤可用核集合 —— 代表落在可用核集合之外时(真机全机最快
  档在可用核集合之外), 那个物理核被整条丢掉, 关掉 SMT 后的线程数会少于"可用核集合里的物理核数"。

本脚本做两件事(全部离线):
  A) 源码结构断言: 从 cpu_affinity.h 里确认
       ① readSiblingGroup() 把 out[] 全部填 -1, 且发生在 parseCpuMask 之前;
       ② readTopology() 的 ① 分支仍然按 (mask[k] < 0) 过滤(所以 -1 不会被当成核号);
       ③ effectiveOrderCached() 先在"频率降序表 ∩ 可用核集合"里挑物理核代表(buildEffectiveSet
          的输入是 base), 而不是先挑代表再过滤;
       ④ auroraThreadCap() 的 SMT 关分支仍然 = 实际使用集合大小;
       ⑤ 物理核数 / effectiveSet.count / cap 三个数确实出现在拓扑行、每项 note 与开关说明里;
       ⑥ 新增的这几处没有任何按机型 / SoC 的分支(不许出现 Pura / HOP-AL00 / Kirin 等字样)。
  B) 数字镜像: 用真机形状(14 逻辑核 / 9 物理核 / 可用核集合 0-8)跑一遍旧初始化(mask 未初始化
     -> 真机上读到 0)与新初始化(mask 全 -1), 断言:
       * 旧初始化 => physical == 1(复现回归), SMT 关 -> 多核线程数 == 1;
       * 新初始化 => physical == 9, SMT 开 -> 9, SMT 关 -> 9, 两种情况都不许是 1;
       * 穷举 0-8 与 9-13 之间所有 C(9,5)=126 种配对方式 x 多组频率排列, 断言
         physical 恒为 9 且"关掉 SMT 后线程数 = 可用核集合里的物理核数", 一次都不许退化成 1。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys, itertools

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "cpu_affinity.h")
fails = 0

_OUT = io.open(os.path.join(HERE, "verify_smt_out.txt"), "w", encoding="utf-8", newline="\n")

def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    _OUT.write(s + "\n")
    _OUT.flush()

def check(name, cond, detail=""):
    global fails
    if cond:
        emit("  [PASS] " + name + ((" — " + detail) if detail else ""))
    else:
        emit("  [FAIL] " + name + ((" — " + detail) if detail else ""))
        fails += 1

with io.open(SRC, encoding="utf-8", errors="replace") as fh:
    src = fh.read()

def body_of(sig_regex):
    m = re.search(sig_regex, src, re.S)
    if not m:
        return ""
    # 从声明处起, 取到第一个顶格 "}" 为止(本文件的函数都是顶格定义)
    rest = src[m.start():]
    end = rest.find("\n}\n")
    return rest[:end + 2] if end >= 0 else rest

# =============================================================== A) 源码结构断言
emit("=" * 100)
emit("[A] 源码结构断言(cpu_affinity.h)")
emit("=" * 100)

sib = body_of(r"inline bool readSiblingGroup\(int cpu, int\* out, int nOut, int\* errOut\)")
check("A1 找到 readSiblingGroup() 函数体", sib != "")
if sib:
    init_at = sib.find("out[i] = -1;")
    parse_at = sib.find("parseCpuMask(buf, out, nOut)")
    check("A2 readSiblingGroup() 把 out[] 全部填成 -1(唯一安全的初值)",
          init_at > 0, "在第 %d 个字符处" % init_at if init_at > 0 else "没找到 out[i] = -1;")
    check("A3 填 -1 发生在 parseCpuMask 之前",
          init_at > 0 and parse_at > init_at,
          "init@%d parse@%d" % (init_at, parse_at))
    check("A4 填 -1 覆盖整个 nOut(不是只填解析出来的那几个)",
          re.search(r"for \(int i = 0; i < nOut; \+\+i\)", sib) is not None)

topo_read = body_of(r"inline AuroraCpuTopoInfo readTopology\(const std::vector<int>& freqs\)")
check("A5 找到 readTopology() 函数体", topo_read != "")
if topo_read:
    check("A6 readTopology() 的 ① 分支仍按 (mask[k] < 0) 过滤(所以 -1 不会被当成核号)",
          re.search(r"mask\[k\] < 0 \|\| mask\[k\] >= n", topo_read) is not None)
    check("A7 readTopology() 仍然按整张兄弟表遍历(这正是必须填满初值的原因)",
          re.search(r"for \(int k = 0; k < kMaxTopoCpus; \+\+k\)", topo_read) is not None)

eff = body_of(r"inline const std::vector<int>& effectiveOrderCached\(\)")
check("A8 找到 effectiveOrderCached() 函数体", eff != "")
if eff:
    base_at = eff.find("base.push_back(cpu)")
    build_at = eff.find("buildEffectiveSet(")
    check("A9 先与可用核集合求交(base), 再挑物理核代表",
          base_at > 0 and build_at > base_at, "base@%d build@%d" % (base_at, build_at))
    check("A10 buildEffectiveSet() 的输入序列是 base(不是全机 order)",
          re.search(r"buildEffectiveSet\(\s*::aurora_smt_detail::topologyInfoCached\(\), base, now\)", eff) is not None)

cap = body_of(r"inline int auroraThreadCap\(\)")
check("A11 找到 auroraThreadCap() 函数体", cap != "")
if cap:
    check("A12 SMT 关时上限 = 实际使用集合大小(拓扑已知)",
          re.search(r"s\.smtEnabled == 0 && s\.topologyKnown && s\.count > 0", cap) is not None)
    check("A13 仍然被「可用核集合」再夹一次(更窄时取更小的那个)",
          re.search(r"auroraAllowedCoreCount\(\)", cap) is not None)

txt = body_of(r"inline std::string auroraSmtTopologyText\(\)")
check("A14 拓扑行里同时给出 physical / effectiveSet.count / cap",
      txt != "" and "physical=%d" in txt and "effectiveSet.count=%d" in txt and "cap=" in txt)

sample = body_of(r"inline AuroraCpuPlacement auroraCpuSampleCurrent\(\)")
check("A15 每项 note 走的那一行(cpuAllowedText)里也带着这三个数",
      sample != "" and "physical=%d" in sample and "effectiveSet.count=%d" in sample and "cap=%d" in sample)

hint = body_of(r"inline std::string auroraSmtSwitchHint\(\)")
check("A16 存在 SMT 开关真实约束说明 auroraSmtSwitchHint()", hint != "")
if hint:
    check("A17 开关说明里写明了「开几个线程 / 关几个线程」与「为什么开与关一样」(不是让用户自己撞)",
          ("开启 %d 线程" in hint) and ("关闭 %d 线程" in hint)
          and ("线程数不变" in hint) and ("物理核" in hint))
    # 2026-10-08 口径变更(断言写清新旧差异):
    #   旧: 上限第一顺位 = effectiveOrderCached().size()(可用逻辑核数, 真机 8),
    #       并行池按逻辑核铺 -> 2 条线程落在 SMT 兄弟上, 一个物理核两条线程各拿 ~50%。
    #   新: 上限第一顺位 = topologyCached().spreadOrder.size()(**可用物理核数**, 真机 6),
    #       并行池按物理核铺 -> 一物理核一线程, SMT 兄弟不进池。
    #   2026-10-06 回退: 真机 A/B(8 线程 1540 vs 6 线程 1070)证明并行池按逻辑核铺是对的,
    #   SMT 兄弟确实在贡献吞吐。这里的断言跟着改回"可用逻辑核数"。
    check("A17b 线程数上限的第一顺位 = 可用逻辑核数(2026-10-06 按真机 A/B 回退)",
          "const int usable = (int)aurora_cpu_detail::effectiveOrderCached().size();" in cap
          and "const int usable = (int)aurora_cpu_detail::topologyCached().spreadOrder.size();" not in cap)
    bet_body = body_of(r"inline CpuTopology buildEffectiveTopology()")
    check("A17c 落点表 spreadOrder 由 buildEffectiveTopology() 按物理核投影构造",
          "t.spreadOrder" in bet_body and "physUsed[ph]" in bet_body)
    check("A18 开关说明里带着 physical / effectiveSet.count / cap 三个数",
          "physical=%d" in hint and "effectiveSet.count=%d" in hint and "cap=%d" in hint)

napi = ""
_napi_path = os.path.join(HERE, "napi_init.cpp")
if os.path.exists(_napi_path):
    with io.open(_napi_path, encoding="utf-8", errors="replace") as fh:
        napi = fh.read()
check("A19 napi 导出 smtSwitchHint(ArkTS 只需透传显示)",
      re.search(r"\{\"smtSwitchHint\", nullptr, SmtSwitchHint", napi) is not None)
check("A20 smtCapabilities 里同时给出 effectiveCount 与 switchHint",
      ("effectiveCount" in napi) and ("switchHint" in napi))

# 不许引入按机型 / SoC 的分支: 本次新增的几处, 去掉注释之后的代码行里不许出现任何机型 /
# SoC 字样(注释里提到真机型号是允许的 —— 那是取证记录, 不是分支)。
def strip_cpp_comments(code):
    code = re.sub(r"/\*.*?\*/", " ", code, flags=re.S)
    out_lines = []
    for ln in code.split("\n"):
        cut = ln.find("//")
        out_lines.append(ln[:cut] if cut >= 0 else ln)
    return "\n".join(out_lines)

_new_code = strip_cpp_comments(sib + eff + txt + sample + hint)
bad_tokens = [t for t in ["Pura", "HOP-AL00", "Kirin", "kirin", "hardwareModel", "Snapdragon", "MediaTek"]
              if t in _new_code]
check("A21 新增代码里没有任何按机型 / SoC 的分支", len(bad_tokens) == 0, "命中: " + ",".join(bad_tokens))

# ================================================== B) 数字镜像(真机形状, 离线)
emit("")
emit("=" * 100)
emit("[B] 真机形状镜像: 14 逻辑核 / 9 物理核 / 可用核集合 cpu0-8")
emit("=" * 100)

KMAX = 32

def parse_cpu_mask(s):
    """parseCpuMask 的逐字镜像: 只写解析出来的核号, 返回写进去的个数。"""
    out = []
    i = 0
    while i < len(s) and s[i] not in "\n\r":
        if not s[i].isdigit():
            i += 1
            continue
        a = 0
        while i < len(s) and s[i].isdigit():
            a = a * 10 + int(s[i])
            if a > 4096:
                a = 4096
            i += 1
        b = a
        if i < len(s) and s[i] == "-":
            i += 1
            b = 0
            while i < len(s) and s[i].isdigit():
                b = b * 10 + int(s[i])
                if b > 4096:
                    b = 4096
                i += 1
        if b < a or a > 4096:
            continue
        if b - a + 1 > len(out) + 1:
            b = a + len(out)
        for c in range(a, b + 1):
            if len(out) >= KMAX:
                break
            out.append(c)
    return out

def group_of_cpu(sibling_text, n, fill):
    """
    readTopology() ① 分支的逐字镜像(含真机那处未初始化的行为):
      int mask[kMaxTopoCpus];                 <- fill=None 时不初始化(真机栈上是 0)
      readSiblingGroup(c, mask, kMaxTopoCpus) <- 修复后: 先全填 -1, 再写解析出来的核号
      for (k = 0; k < kMaxTopoCpus; ++k)      <- 注意: 遍历整张数组
    """
    if fill is None:
        mask = [0] * KMAX          # 真机实测: 那片未初始化栈内存是 0
    else:
        mask = [fill] * KMAX
    parsed = parse_cpu_mask(sibling_text)
    for idx, v in enumerate(parsed):
        if idx < KMAX:
            mask[idx] = v
    g = 0
    for k in range(KMAX):
        if mask[k] < 0 or mask[k] >= n:
            continue
        gg = 1 + mask[k]
        if g == 0 or gg < g:
            g = gg
    return g

def build_topology(sib_texts, n, fill):
    """返回 (physical, physicalOfCpu[], smtPossible, siblingsRead)"""
    group = []
    group_read = 0
    for c in range(n):
        g = group_of_cpu(sib_texts[c], n, fill)
        group.append(g)
        if g > 0:
            group_read += 1
    phys_of = [i for i in range(KMAX)]
    physical = 0
    if group_read >= 2:
        max_g = max(group) if group else 0
        phys_of_group = [-1] * (max_g + 2)
        physical = 0
        for c in range(n):
            if group[c] <= 0:
                phys_of[c] = physical
                physical += 1
                continue
            if phys_of_group[group[c]] < 0:
                phys_of_group[group[c]] = physical
                physical += 1
            phys_of[c] = phys_of_group[group[c]]
    smt_possible = 1 if (physical > 0 and physical < n) else 0
    return physical, phys_of, smt_possible, group_read

def build_effective_set(n, phys_of, known, order, smt_on):
    """buildEffectiveSet() 的逐字镜像(只保留与线程数有关的部分)。"""
    cpus = []
    if smt_on or not known:
        cpus = [c for c in order if 0 <= c < n]
    else:
        picked = {}
        for c in order:
            if c < 0 or c >= n:
                continue
            p = phys_of[c]
            if p not in picked:
                picked[p] = c
        for c in order:
            if c < 0 or c >= n:
                continue
            p = phys_of[c]
            if picked.get(p) == c:
                cpus.append(c)
    return cpus

def spread_list(order, phys_of, known, smt_possible):
    """buildEffectiveTopology() 里 spreadOrder 那一段的逐字镜像(2026-10-08 口径变更)。

    规则: 沿频率降序序列走一遍, 每个**物理核**只收下第一个遇到的逻辑核(同物理核上的 SMT
    兄弟不进并行池)。拓扑未知 / 没有 SMT 时是恒等映射 —— 与改动前逐位相同。
    旧口径没有这一步: 并行池直接用 order 逐位(可用逻辑核数 = 线程数)。
    """
    if not (known and smt_possible):
        return list(order)
    used = set()
    out = []
    for c in order:
        p = phys_of.get(c, c) if isinstance(phys_of, dict) else phys_of[c]
        if p in used:
            continue
        used.add(p)
        out.append(c)
    return out

def thread_cap(smt_on, known, es_count, allowed, usable=0):
    """auroraThreadCap() 的逐字镜像。

    2026-10 改(用户要求"每颗核都跑满"): 上限的第一顺位是 可用核数 usable
    (= 可用核集合 ∩ SMT 口径之后的核序列大小, 也就是 C++ 里的 effectiveOrderCached().size())。
    旧口径用"全机物理核数"(es_count), 那个数与"本进程真正能用的核"无关 —— 真机上因此出现
    "9 个线程抢 7 个可用核"(note 原话: 池线程 2 个因位次冲突改到其它位次), 与铺满正相反。
    usable <= 0(序列为空: 频率表与可用核集合都读不到)时才退回旧口径。

    2026-10-08 口径变更: 第一顺位从 effectiveOrderCached().size()(可用**逻辑**核数)改成
    topologyCached().spreadOrder.size()(**可用物理核数**, 每物理核一个代表)。调用方传进来的
    usable 一律是 spread_list(...) 的长度 —— 无 SMT / 拓扑未知的机器上它与旧值逐位相同。
    """
    if usable > 0:
        return usable
    cap = -1
    if (not smt_on) and known and es_count > 0:
        cap = es_count
    if allowed > 0 and (cap < 0 or allowed < cap):
        cap = allowed
    return cap

def threads_for(requested, smt_on, known, es_count, cap):
    """auroraSmtThreadsFor() 的逐字镜像。"""
    want = requested if requested >= 1 else 1
    if want <= 1:
        return 1
    out = want
    if (not smt_on) and known and es_count > 0:
        out = es_count
    if cap > 0 and out > cap:
        out = cap
    return out

def device_shape(pairs, n=14):
    """真机形状: cpu 0-8 = 9 个物理核各一个线程, cpu 9-13 是其中 5 个物理核的兄弟线程。
    pairs = [(i, j) ...], i 在 0-8, j 在 9-13, 表示 cpu i 与 cpu j 共享同一个物理核。"""
    sib = {}
    for i in range(9):
        sib[i] = "%d" % i
    for j in range(9, n):
        sib[j] = "%d" % j
    for (i, j) in pairs:
        sib[i] = "%d,%d" % (i, j)
        sib[j] = "%d,%d" % (i, j)
    return [sib[c] for c in range(n)]

ALLOWED = [0, 1, 2, 3, 4, 5, 6, 7, 8]     # 真机: allowed=0-8 (9 核; sched_getaffinity)
ALLOWED_N = 9

# ---- B0: 旧的"未初始化 mask"必须复现回归(否则说明镜像没抓住真机行为) ----
pairs0 = [(0, 9), (1, 10), (2, 11), (3, 12), (4, 13)]
sib0 = device_shape(pairs0)
phys_bug, phys_of_bug, smt_bug, read_bug = build_topology(sib0, 14, fill=None)
emit("  B0 旧行为(兄弟表数组未初始化, 真机栈上是 0):")
emit("     thread_siblings_list 读到 %d/14 核 · physical = %d · smtPossible = %d" % (read_bug, phys_bug, smt_bug))
check("B0a 复现回归: physical == 1", phys_bug == 1, "physical=%d" % phys_bug)
check("B0b 复现回归: 表面上仍写着「检测到 SMT」(smtPossible = 1)", smt_bug == 1)
es_bug = len(build_effective_set(14, phys_of_bug, True, list(range(14)), False))
usable_bug = len(spread_list(build_effective_set(14, phys_of_bug, True, ALLOWED, False),
                               phys_of_bug, True, True))   # 可用核集合 ∩ 物理核口径(每个物理核一个代表)
cap_bug = thread_cap(False, True, es_bug, ALLOWED_N, usable_bug)
thr_bug = threads_for(14, False, True, es_bug, cap_bug)
emit("     SMT 关: 实际使用集合 %d 核 · cap %d · 多核阶段线程数 %d" % (es_bug, cap_bug, thr_bug))
check("B0c 复现回归: 关掉 SMT 后多核线程数 == 1(所有多核分数作废)",
      thr_bug == 1, "threads=%d" % thr_bug)

# ---- B1: 修复后的行为(真机形状) ----
phys, phys_of, smt_possible, sib_read = build_topology(sib0, 14, fill=-1)
emit("")
emit("  B1 修复后(兄弟表数组全部填 -1):")
emit("     thread_siblings_list 读到 %d/14 核 · physical = %d · smtPossible = %d" % (sib_read, phys, smt_possible))
check("B1a 14 逻辑核归成 9 个物理核", phys == 9, "physical=%d" % phys)
check("B1b 可用核集合 cpu0-8 里 9 个核分属 9 个不同的物理核",
      len(set(phys_of[c] for c in ALLOWED)) == 9,
      "distinct=%d" % len(set(phys_of[c] for c in ALLOWED)))
# SMT 开: 线程数由"可用核集合"夹出 9
es_on = len(build_effective_set(14, phys_of, True, list(range(14)), True))
usable_on = len(spread_list(build_effective_set(14, phys_of, True, ALLOWED, True),
                              phys_of, True, True))    # 可用核集合 -> 每个物理核一个代表
cap_on = thread_cap(True, True, es_on, ALLOWED_N, usable_on)
thr_on = threads_for(14, True, True, es_on, cap_on)
# SMT 关: 每个物理核一个逻辑核(只在可用核集合之内挑代表)
es_off = len(build_effective_set(14, phys_of, True, list(range(14)), False))
usable_off = len(spread_list(build_effective_set(14, phys_of, True, ALLOWED, False),
                               phys_of, True, True))
cap_off = thread_cap(False, True, es_off, ALLOWED_N, usable_off)
thr_off = threads_for(14, False, True, es_off, cap_off)
emit("     SMT 开: 实际使用集合 %d 核 · cap %d · 多核阶段线程数 %d" % (es_on, cap_on, thr_on))
emit("     SMT 关: 实际使用集合 %d 核 · cap %d · 多核阶段线程数 %d" % (es_off, cap_off, thr_off))
check("B1c SMT 开 -> 多核线程数 == 9(被可用核集合夹住, 不许退化)", thr_on == 9, "threads=%d" % thr_on)
check("B1d SMT 关 -> 多核线程数 == 物理核数(可用核集合内 9 个)", thr_off == 9, "threads=%d" % thr_off)
check("B1e 两种情况都不许是 1", thr_on != 1 and thr_off != 1)

# ---- B2: 第二个成因(先挑代表再过滤 vs 先过滤再挑代表) ----
# 真机证据: 全机最快档 2750MHz 有 2 个核, 而在可用核集合 cpu0-8 里最高只有 2270MHz(5 核簇)
# => 那 2 个最快的核位于可用核集合之外(9-13)。这里把这一事实镜像进频率表, 看两种顺序差多少。
def order_by_freq(khz):
    return sorted(range(len(khz)), key=lambda c: (-khz[c], c))

khz = [2270] * 14
for c in [4, 5, 6, 7, 8]:
    khz[c] = 2270
for c in [0, 1, 2, 3, 11, 12, 13]:
    khz[c] = 1900
khz[9] = 2750
khz[10] = 2750
order = order_by_freq(khz)
old_way = [c for c in build_effective_set(14, phys_of, True, order, False) if c in ALLOWED]
new_way = build_effective_set(14, phys_of, True, [c for c in order if c in ALLOWED], False)
emit("")
emit("  B2 频率降序表(前几位): %s ...(2750MHz 的核在可用核集合之外)" % order[:6])
emit("     旧顺序(先挑代表再过滤可用核集合) -> %d 个核: %s" % (len(old_way), old_way))
emit("     新顺序(先过滤可用核集合再挑代表) -> %d 个核: %s" % (len(new_way), new_way))
check("B2a 旧顺序会丢掉物理核(线程数少于可用核集合里的物理核数)",
      len(old_way) < 9, "old=%d" % len(old_way))
check("B2b 新顺序一个都不丢(每个物理核都有内核允许的线程)", len(new_way) == 9, "new=%d" % len(new_way))

# ---- B3: 穷举所有配对方式 x 多组频率排列 ----
emit("")
emit("  B3 穷举: 0-8 里挑 5 个与 9-13 配对(C(9,5)=126) x 4 组频率排列")
worst = None
cases = 0
for chosen in itertools.combinations(range(9), 5):
    pairs = list(zip(chosen, range(9, 14)))
    sibs = device_shape(pairs)
    p, pof, smtp, rd = build_topology(sibs, 14, fill=-1)
    for khz_variant in range(4):
        k = [2000] * 14
        for idx, c in enumerate(range(9, 14)):
            k[c] = 2750 if (idx % 2 == khz_variant % 2) else 2400
        for idx, c in enumerate(range(9)):
            k[c] = 2270 if (idx % 2 == (khz_variant + 1) % 2) else 1900
        if khz_variant == 3:
            k = [2000] * 14        # 频率全未知也不能退化成 1
        o = order_by_freq(k)
        es_o = len(build_effective_set(14, pof, True, o, False))
        u_o = len(spread_list(build_effective_set(14, pof, True, [c for c in o if c in ALLOWED], False),
                              pof, True, True))
        c_o = thread_cap(False, True, es_o, ALLOWED_N, u_o)
        t_o = threads_for(14, False, True, es_o, c_o)
        es_n = len(build_effective_set(14, pof, True, o, True))
        u_n = len(spread_list(build_effective_set(14, pof, True, [c for c in o if c in ALLOWED], True),
                              pof, True, True))
        c_n = thread_cap(True, True, es_n, ALLOWED_N, u_n)
        t_n = threads_for(14, True, True, es_n, c_n)
        cases += 1
        ok = (p == 9) and (t_o == 9) and (t_n == 9) and (t_o != 1) and (t_n != 1)
        if not ok and worst is None:
            worst = (pairs, khz_variant, p, t_o, t_n)
check("B3a 全部 %d 个用例: physical == 9 且 SMT 开/关线程数都是 9(没有一次退化成 1)" % cases,
      worst is None, "" if worst is None else ("反例: %s" % (worst,)))

emit("")
emit("=" * 100)
if fails == 0:
    emit("全部 PASS(源码断言 A + 真机形状镜像 B)")
else:
    emit("有 %d 项 FAIL" % fails)
emit("=" * 100)
_OUT.close()
sys.exit(0 if fails == 0 else 1)
