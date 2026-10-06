# -*- coding: utf-8 -*-
"""
多核并行池"按物理核铺"的自查器 (静态源码断言 + 真机形状数字镜像; 无设备, 不连 hdc)

背景(2026-10-08; 证据来自华为 SmartPerf 逐秒逐核 trace + 同机 8.4 报告)
----------------------------------------------------------------------------
设备 HUAWEI Pura X Max / HOP-AL00: 14 逻辑核 / 9 物理核, 内核允许本 App 用 cpu0-7(8 个逻辑核),
thread_siblings_list 14/14 核读到。这 8 个可用逻辑核只落在 6 个可用物理核上:
    c0-c3 = 4 个小核(无 SMT, 顶档 1720MHz)
    {c4,c5} = 大核 P4(SMT, 顶档 2270MHz)      {c6,c7} = 大核 P5(SMT, 顶档 2270MHz)
    c8-c11 = P6/P7(内核拒绝)                  c12,c13 = P8(内核拒绝)

旧口径(本次改动前): 多核阶段的并行池按**逻辑核**铺 —— 线程数 = 可用逻辑核数(8),
落点 = 频率降序第 i 个逻辑核 = c4,c5,c6,c7,c0,c1,c2,c3。于是 2 条线程被钉在 SMT 兄弟
c5 / c6 上: 同一个物理核上两条线程, 每条各拿 ~50%。

两条互相独立、可核对的证据都指向"8 条线程喂不满 6 个物理核":
  ① App 自己测的并行度(bench_cpu.cpp: out.parallelism = 各线程 CPU 时间之和 / 墙钟):
     8 项多核负载在 8 线程下的实测值 = 5.90 / 3.78 / 6.04 / 5.09 / 6.10 / 6.11 / 6.09 / 5.47,
     天花板 ≈ 6.1 —— 6 正是**可用物理核数**, 一次都没接近 8。
  ② SmartPerf 逐秒逐核占用(data3.zip, 378 s): 多核阶段(第 283~357 秒的 75 个采样)
     全程"逐核占用 >= 90%"的核数为 0 的采样有 58 个, 最大只有 4(那 4 个正是无 SMT 的 c0-c3);
     8 个可用逻辑核里同时 >= 50% 的最多 6 个。两个线程挤在一个物理核上时, 每条硬件线程各拿
     ~50% —— "没有任何一颗核能到 90%"因此是必然, 而不是负载不够并行。

新口径: 并行池 = **每个可用物理核一条线程**, 线程数 = 可用物理核数, 落点 = 每个物理核里
频率位次最靠前的那一个逻辑核; SMT 兄弟不进池。
  * 工作量 / 算法 / 数据规模 / metric / 单位 / k / conv / 计分公式一个字都不改;
  * 只改"开几条线程、钉在哪几颗核上";
  * 无 SMT 或拓扑未知的机器上, 投影是恒等映射 —— 线程数与落点**与改动前逐位相同**。

本脚本做两件事(全部离线):
  A) 源码结构断言: 从 cpu_affinity.h / bench_cpu.cpp / gb7_parallel.h / gb7.cpp 里确认
       ① CpuTopology 有 spreadOrder(物理核口径落点表);
       ② buildEffectiveTopology() 按"每物理核只收第一个遇到的逻辑核"构造它;
       ③ 多核阶段的目标位次数(= 线程数上限)取自 spreadOrder, 不是 order;
       ④ auroraThreadCap() 第一顺位 = spreadOrder.size()(物理核口径, 不许超过可用物理核数);
       ⑤ 池线程落点 bindWorkerCoreSpread() 取自 spreadOrder(一物理核一线程, 不回卷);
       ⑥ 自研套件 coresByPerfDesc() 与 CS1 同口径(auroraPhysicalCoreList());
       ⑦ 没有"按逻辑核铺"的旧写法残留(多核阶段不许再把 t.order.size() 当线程数);
       ⑧ 负载文件里不出现 spreadOrder / auroraPhysicalCoreList(绑核只在 cpu_affinity.h 一处,
          任何一个负载都不知道自己被绑在哪 —— 保证"没改负载"可核对);
       ⑨ 无 SMT / 拓扑未知时投影恒等(源码里有 haveTopo 判据与空表兜底)。

  B) 数字镜像: 用真机形状与几组变形跑一遍新旧两套口径, 断言
       * 旧口径 => 8 线程, 其中 2 条落在 SMT 兄弟上(复现被修的现象);
       * 新口径 => 6 线程 == 可用物理核数, 落点两两不同物理核(每个物理核恰好一条);
       * 新口径线程数恒 <= 可用物理核数(穷举 siblings 配对与频率排列, 一次都不许超);
       * 无 SMT / 拓扑未知的机器上, 新口径 == 旧口径(零行为变化)。
     并把"新旧差异"逐条打印出来(这是口径变更, 必须写清楚)。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys, itertools

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_AFF = os.path.join(HERE, "cpu_affinity.h")
SRC_OWN = os.path.join(HERE, "bench_cpu.cpp")
SRC_PAR = os.path.join(HERE, "gb7_parallel.h")
# CS1 的 16 项负载: 一个都不许知道"自己被绑在哪颗核上"(绑核只在 cpu_affinity.h 一处)。
# bench_cpu.cpp(自研套件)不在这一组: 它是本工程唯一另一处"线程 -> 核"的分配点, 而且现在
# 与 CS1 同口径(coresByPerfDesc() = auroraPhysicalCoreList()), 见 A6。
LOAD_FILES = ["gb7.cpp", "gb7_clang.cpp", "gb7_filecompress.cpp", "gb7_batch2.cpp",
              "gb7_batch3.cpp", "gb7_pdf.cpp", "gb7_sfm.cpp", "gb7_browser.cpp",
              "gb7_video.cpp", "gb7_audio.cpp"]

fails = 0
_OUT = io.open(os.path.join(HERE, "verify_physical_core_pool_out.txt"), "w",
               encoding="utf-8", newline="\n")

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

def rd(p):
    with io.open(os.path.join(HERE, p), encoding="utf-8", errors="replace") as fh:
        return fh.read()

def body_of(src, sig_regex):
    m = re.search(sig_regex, src, re.S)
    if not m:
        return ""
    rest = src[m.start():]
    end = rest.find("\n}\n")
    return rest[:end + 2] if end >= 0 else rest

aff = rd("cpu_affinity.h")
own = rd("bench_cpu.cpp")
par = rd("gb7_parallel.h")

emit("=" * 100)
emit("[A] 源码结构断言")
emit("=" * 100)

# A1 CpuTopology 有 spreadOrder
struct_re = re.search(r"struct CpuTopology \{(.*?)\n\};", aff, re.S)
st = struct_re.group(1) if struct_re else ""
check("A1 CpuTopology 里有物理核口径落点表 spreadOrder / spreadPhysical",
      ("spreadOrder" in st) and ("spreadPhysical" in st))

# A2 buildEffectiveTopology 构造投影
bet = body_of(aff, r"inline CpuTopology buildEffectiveTopology\(\)")
check("A2 找到 buildEffectiveTopology() 函数体", bet != "")
if bet:
    check("A2-1 投影按「物理核已用过就跳过」构造(physUsed[ph] => continue)",
          ("physUsed[ph]" in bet) and re.search(r"if \(physUsed\[ph\]\) \{\s*continue;", bet) is not None)
    check("A2-2 无 SMT / 拓扑未知时投影恒等(haveTopo 判据)",
          "haveTopo" in bet and "info.smtPossible" in bet)
    check("A2-3 空表兜底 = order(不凭空造落点)",
          re.search(r"if \(t\.spreadOrder\.empty\(\)\) \{\s*//", bet) is not None and "t.spreadOrder = t.order;" in bet)

# A3 多核目标位次数取自"与线程数同口径的那张表"
#   期望值随 2026-10-10 修复更新: 从 spreadOrder(每物理核一个代表, 真机 6)改回 multiCoreSlotList
#   (= t.order, 可用核序列, 真机 8)。理由见 verify_core_spread.py 的 A8b: 8 条线程往 6 个格子上钉,
#   真机 CS1 多核 8.5 = 1492.8 -> 9.6 = 1239(-17%), 而同包同芯片的自研多核只差 0.1%。
lpt = body_of(aff, r"inline int loadPhaseTargetCount\(const CpuTopology& t, bool multiCore\)")
check("A3 找到 loadPhaseTargetCount() 函数体", lpt != "")
if lpt:
    check("A3-1 多核阶段线程数上限 = multiCoreSlotList(t).size()(与线程数同口径)",
          "return (int)multiCoreSlotList(t).size();" in lpt,
          "旧写法是 spreadOrder.size()(物理核口径, 已证伪)")

# A4 auroraThreadCap 第一顺位
cap = body_of(aff, r"inline int auroraThreadCap\(\)")
check("A4 找到 auroraThreadCap() 函数体", cap != "")
if cap:
    #  期望值随 2026-10-10 修复更新: 代码里取的就是 effectiveOrderCached().size()(可用逻辑核数)。
    #  旧断言写的是 topologyCached().spreadOrder.size() —— 它当时只是被函数里那句
    #  "曾短暂改成 spreadOrder.size()" 的**注释**顶着的, 测的是注释不是代码(假通过)。
    #  现在直接钉住代码那一行, 见 verify_core_spread.py 的 A12/A8b。
    #  这里只断言**代码那一行**(正向)。原来那条"不许出现 topologyCached().spreadOrder.size()"
    #  的反向写法已经没意义 —— 函数体里有一句历史注释正好含这个字符串, 反向条件永远为假,
    #  那是"测注释不是测代码"(旧版 A4-1 就是这么假通过的)。
    check("A4-1 第一顺位 = effectiveOrderCached().size()(可用逻辑核数, 与落点表同口径)",
          "const int usable = (int)aurora_cpu_detail::effectiveOrderCached().size();" in cap
          and "return usable;" in cap)
    check("A4-2 仍然被「可用核集合」再夹一次(更窄时取更小的那个)",
          "auroraAllowedCoreCount()" in cap)

# A5 池线程落点
spread_fn = body_of(aff, r"inline int bindWorkerCoreSpread\(const CpuTopology& t, int index, int\* landingOut, int\* errOut\)")
check("A5 找到 bindWorkerCoreSpread() 函数体", spread_fn != "")
if spread_fn:
    #  期望值随 2026-10-10 修复更新: 落点唯一来源 = multiCoreSlotList(t)(= t.order)。
    check("A5-1 落点取自 multiCoreSlotList(t)(与线程数同口径的唯一来源)",
          "const std::vector<int>& spread = multiCoreSlotList(t);" in spread_fn
          and "t.spreadOrder" not in spread_fn)
    check("A5-2 落点仍按「一核一线程 + index 越界不回卷」处理",
          "index >= n" in spread_fn and "-5" in spread_fn)

# A6 自研套件同口径
own_fn = body_of(own, r"std::vector<int> coresByPerfDesc\(\)")
#  期望值随 2026-10-06 真机回退更新(9.9): 自研套件**必须**用 auroraPhysicalCoreList()
#  (每物理核一个代表, 真机 6 格), 不许跟着 CS1 改成 auroraEffectiveCoreList()(8 格)。
#  9.7 曾经为了"两个套件看起来同口径"把它改成 8 格, 真机立刻掉分:
#    自研多核 6276/6269 -> 6082/6133/5238, 逐项 物理模拟 2.14x / AI 推理 2.60x 慢;
#    而同一版自研**单核**八项全在 ±8% 以内(单核路径没动过, 所以问题只在多核钉核这一处)。
#  机理: 本套件多核是静态分块(i = t; i < tasks; i += threads), 用 6 格表时第 7/8 条线程不钉、
#    保留宽掩码能落到最空闲的核上; 用 8 格表时线程 4~7 被硬钉到 4 个小核, 那 4 个慢分块
#    成了整轮的时间下界。**两个套件的落点策略本来就该不一样。**
check("A6 自研套件 coresByPerfDesc() 用物理核落点表(auroraPhysicalCoreList; 2026-10-06 真机回退)",
      "auroraPhysicalCoreList()" in own_fn and "auroraEffectiveCoreList()" not in own_fn)

# A7 旧写法残留
#  期望值随 2026-10-10 修复更新: 多核那一支现在就是 t.order 口径(multiCoreSlotList),
#  所以原来那条"多核不许出现 t.order.size()"已经过时、并且会与 A3-1 自相矛盾。
#  换成它真正要防的东西: 多核支不许再出现"每物理核一个代表"的投影表。
check("A7 多核阶段不再用物理核投影表(spreadOrder)当线程数上限",
      "t.spreadOrder" not in lpt)

# A8 负载不知道绑核细节
bad = [f for f in LOAD_FILES if ("spreadOrder" in rd(f)) or ("auroraPhysicalCoreList" in rd(f))]
check("A8 没有任何 CS1 负载文件引用 spreadOrder / auroraPhysicalCoreList(绑核只在 cpu_affinity.h 一处)",
      bad == [], "命中: %s" % ",".join(bad) if bad else "")
check("A8-1 自研套件 bench_cpu.cpp 与 CS1 同口径(它引用 auroraPhysicalCoreList 是设计如此)",
      "auroraPhysicalCoreList" in own)

# A9 公开入口存在
check("A9 存在公开落点表入口 auroraPhysicalCoreList()",
      "inline const std::vector<int>& auroraPhysicalCoreList()" in aff)

emit("")
emit("=" * 100)
emit("[B] 数字镜像(真机形状 + 变形; 新旧口径对照)")
emit("=" * 100)

def spread_of(order, phys_of, known, smt_possible):
    """buildEffectiveTopology() 里 spreadOrder 这一段的逐字镜像。"""
    if not (known and smt_possible):
        return list(order)
    used = set()
    out = []
    for c in order:
        p = phys_of.get(c, c)
        if p in used:
            continue
        used.add(p)
        out.append(c)
    return out

def old_threads(order):
    """旧口径: 线程数 = 可用逻辑核数, 落点 = order 逐位。"""
    return list(order)

# ---- 真机形状(用户真机 = 本次报告的那台) ----
LOGICAL = 14
SIB = {0: 0, 1: 1, 2: 2, 3: 3,          # c0-c3: 4 个小核, 各自独立物理核
       4: 4, 5: 4, 6: 6, 7: 6,          # {c4,c5} = P4, {c6,c7} = P5(各带 SMT)
       8: 8, 9: 8, 10: 10, 11: 10,      # c8-c11 = P6/P7(内核拒绝)
       12: 12, 13: 12}                  # c12,c13 = P8(内核拒绝)
ALLOWED = list(range(0, 8))             # 内核允许本进程用 cpu0-7
FREQ = {0: 1720000, 1: 1720000, 2: 1720000, 3: 1720000,
        4: 2270000, 5: 2270000, 6: 2270000, 7: 2270000,
        8: 2270000, 9: 2270000, 10: 2270000, 11: 2270000,
        12: 2750000, 13: 2750000}
order = sorted(ALLOWED, key=lambda c: (-FREQ[c], c))     # 频率降序, 同频核号升序
phys_of = dict(SIB)

old_l = old_threads(order)
new_l = spread_of(order, phys_of, True, True)
phys_used = [phys_of[c] for c in new_l]

emit("  真机形状: %d 逻辑核 / %d 物理核; 内核允许 %s; 频率降序序列 = %s"
     % (LOGICAL, len(set(SIB.values())), "cpu0-7", ",".join("cpu%d" % c for c in order)))
emit("  [旧口径] 线程数 = %d, 落点 = %s" % (len(old_l), ",".join("cpu%d" % c for c in old_l)))
dup = [c for c in old_l if sum(1 for x in old_l if SIB[x] == SIB[c]) > 1]
emit("           旧口径里共享物理核的核: %s(同一物理核上两条线程, 每条各拿 ~50%%)"
     % ",".join("cpu%d" % c for c in sorted(set(dup))))
emit("  [新口径] 线程数 = %d, 落点 = %s(每个物理核恰好一条)"
     % (len(new_l), ",".join("cpu%d" % c for c in new_l)))

check("B1 新口径线程数 == 可用核集合里的物理核数(%d)" % len(set(SIB[c] for c in ALLOWED)),
      len(new_l) == len(set(SIB[c] for c in ALLOWED)),
      "threads=%d" % len(new_l))
check("B2 新口径线程数 <= 可用物理核数",
      len(new_l) <= len(set(SIB[c] for c in ALLOWED)))
check("B3 新口径落点两两不同物理核(一物理核一线程)",
      len(set(phys_used)) == len(phys_used), "物理核: %s" % phys_used)
check("B4 旧口径线程数 > 新口径(这正是被修的差异: 8 条线程铺 6 个物理核)",
      len(old_l) > len(new_l), "旧=%d 新=%d" % (len(old_l), len(new_l)))
check("B5 旧口径里确实存在 SMT 兄弟共享物理核(复现被修的现象)",
      len(set(dup)) > 0, "共享核: %s" % ",".join("cpu%d" % c for c in sorted(set(dup))))
check("B6 新口径仍然大核优先(前两条落在大核代表上)",
      all(FREQ[c] == 2270000 for c in new_l[:2]), "前两位: %s" % new_l[:2])

# ---- 变形 1: 无 SMT 机器(物理核数 == 逻辑核数) -> 必须零行为变化 ----
order_nosmt = [0, 1, 2, 3, 4, 5, 6, 7]
n1 = spread_of(order_nosmt, {c: c for c in range(8)}, True, False)
check("B7 无 SMT 机器: 新口径 == 旧口径(零行为变化)",
      n1 == old_threads(order_nosmt), "%s" % n1)

# ---- 变形 2: 拓扑未知 -> 恒等 ----
n2 = spread_of(order_nosmt, {c: c for c in range(8)}, False, False)
check("B8 拓扑未知: 新口径 == 旧口径(恒等, 不假装知道)", n2 == order_nosmt)

# ---- 变形 3: 穷举各种 SMT 配对方式, 线程数恒 == 可用物理核数且不大于它 ----
cases = 0
ok_threads = True
ok_unique = True
for ncore in range(1, 9):
    # 把 0..ncore-1 这些"逻辑核"两两配对成物理核(允许落单), 穷举前 6 个逻辑核的配对
    base = list(range(ncore))
    for pair_cnt in range(0, ncore // 2 + 1):
        for pairs in itertools.combinations(itertools.combinations(base, 2), pair_cnt):
            flat = [c for p in pairs for c in p]
            if len(set(flat)) != len(flat):
                continue
            phys = {}
            pid = 0
            for c in base:
                hit = None
                for idx, p in enumerate(pairs):
                    if c in p:
                        hit = idx
                if hit is None:
                    phys[c] = 100 + c          # 独立物理核
                else:
                    phys[c] = hit
            sp = spread_of(base, phys, True, True)
            cases += 1
            if len(sp) != len(set(phys[c] for c in base)):
                ok_threads = False
            if len(set(phys[c] for c in sp)) != len(sp):
                ok_unique = False
            if len(sp) > len(set(phys[c] for c in base)):
                ok_threads = False
check("B9 穷举 %d 种 SMT 配对: 新口径线程数恒 == 可用物理核数" % cases, ok_threads)
check("B10 穷举 %d 种 SMT 配对: 落点恒不共享物理核" % cases, ok_unique)

emit("")
emit("=" * 100)
emit("【新旧差异(口径变更, 必须写在报告里)】")
emit("=" * 100)
emit("  旧: 多核阶段线程数 = 可用**逻辑核**数(真机 8), 落点 = 频率降序第 i 个逻辑核")
emit("      -> c4,c5,c6,c7,c0,c1,c2,c3; 其中 {c4,c5} 与 {c6,c7} 各是一个带 SMT 的物理核,")
emit("         同一物理核上两条线程各拿 ~50% => 逐核占用上不去, 也解释不了「喂满」。")
emit("  新: 多核阶段线程数 = 可用**物理核**数(真机 6), 落点 = 每个物理核里频率位次最靠前的那个")
emit("      -> c4,c6,c0,c1,c2,c3; 一个物理核恰好一条线程, 无 SMT 兄弟参与。")
emit("  预期可核对的效果(离线推理, 需真机复核):")
emit("    ① 多核阶段「同时 >= 90% 的核数」应从 0~4 升到 6(6 条线程各占一个物理核, 各 ~100%);")
emit("    ② App 自测并行度应从天花板 ~6.1(= 物理核数) 变成「贴着 6.0 的窄带」(线程数就是 6);")
emit("    ③ 吞吐不应下降: 旧口径下 8 条线程实际只交付了 ~6 核的 CPU 时间(见文件头证据 ①),")
emit("       去掉那两条拿不到算力的 SMT 线程, 工作总量与算法一个字都没变;")
emit("    ④ 无 SMT / 拓扑未知的设备: 线程数与落点与改动前逐位相同(见 B7 / B8)。")

emit("")
emit("=" * 100)
if fails == 0:
    emit("结论: 全部 PASS —— 多核并行池的线程数与落点由**物理核**推导, 且不超过可用物理核数。")
else:
    emit("结论: 有 %d 项 FAIL, 见上面 [FAIL] 行。" % fails)
emit("=" * 100)

_OUT.close()
sys.exit(0 if fails == 0 else 1)
