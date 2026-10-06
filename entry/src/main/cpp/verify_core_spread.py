# -*- coding: utf-8 -*-
r"""
verify_core_spread.py —— 多核阶段"每颗核都跑满"的自查器(静态源码断言 + 真机形状镜像; 无设备)

用户原话: 「跑满？每颗核都跑满！！！」
真机证据(用户实测 + D:\gb7logs\now2.jsonl):
  * 多核项运行时频率中位只有 558MHz(Ray Tracer)/1380MHz(File Compression), 标称 2270MHz ——
    采样口径是"可用核集合的 核 x 时间 合并样本", 中位这么低 = 绝大多数核在绝大多数时刻空闲;
  * 界面"多核阶段明细(并行度)"只有 3.5 / 2.8 / 4.3 核, 而可用核是 9 个;
  * note 里写着"3 核簇/最高2270MHz"与"池线程 2 个因位次冲突改到其它位次"
    (= 9 个线程抢 7 个可用核: 线程数上限用的是"全机物理核数", 与"本进程能用的核"无关)。

本脚本做三件事(全部离线, 不需要设备):
  A) 源码结构断言: 确认
       ① 池线程绑定改成"一核一线程"(单核掩码 + 立刻读回校验), index 越界不回卷;
       ② 单核/多核两阶段的绑核策略分开: 单核 = 快簇(最快档), 多核 = 全部可用核;
       ③ 线程数上限 = 可用核数(可用核集合 ∩ SMT 口径), 不再是"全机物理核数";
       ④ 每项 note 里给出: 线程数 / 实际用到的核清单 / 每核是否有线程 / 逐核落点 /
          空闲比例与"完全没干活的核"/ 以及 M<N 时的原因(不许静默);
       ⑤ 逐核占用用 /proc/stat 差值(两次读盘都在计时区间之外), 读不到就写读不到;
       ⑥ 不许动的东西没动(工作量/尺寸/metric/unit/k/conv/计分公式/打点数量), 也没有按机型分支。
  B) 数字镜像:
       * "9 个线程抢 7 个核"的旧形态可复现(2 个线程无处可去);
       * 新规则 "线程数 = 可用核数" 下, 穷举多个核数与多组频率排列: 目标核两两不同,
         并集恰好等于工作集合(不空、不挤) —— 一核一线程;
       * /proc/stat 解析镜像: busy/idle 字段口径正确, 能算出"完全没干活的核"与空闲比例;
       * 自检文本在 M<N 时必须给出原因(线程数被夹 / 池线程没绑上 / 工作集合比可用核窄)。
  C) 断言汇总。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys, itertools

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_core_spread_out.txt"), "w", encoding="utf-8", newline="\n")
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

src  = rd("cpu_affinity.h")
par  = rd("gb7_parallel.h")
freq = rd("cpu_freq_sample.cpp")
gb7  = rd("gb7.cpp")

def body_of(sig_regex, text):
    m = re.search(sig_regex, text, re.S)
    if not m:
        return ""
    rest = text[m.start():]
    end = rest.find("\n}\n")
    return rest[:end + 2] if end >= 0 else rest

emit("=" * 100)
emit("[A] 源码结构断言")
emit("=" * 100)

# ---- ① 一核一线程 ----
spread = body_of(r"inline int bindWorkerCoreSpread\(const CpuTopology& t, int index, int\* landingOut, int\* errOut\)", src)
check("A1 存在池线程绑定函数 bindWorkerCoreSpread()", spread != "")
check("A2 绑定目标是一个核(单核掩码), 不是同频组", "CPU_SET(cpu, &one);" in spread and "CPU_SET(cpu, &one)" in spread)
# 期望值随 2026-10-10 修复更新(见下面的 A3b): 位次表的来源回到"可用核序列", 由
#   multiCoreSlotList() 一处给出。2026-10-07/08 曾把它换成 spreadOrder(每物理核一个代表),
#   那次只改了落点表、没同步改线程数, 于是 8 条线程往 6 个格子上钉 —— 真机 CS1 多核
#   8.5 = 1492.8 -> 9.6 = 1239(低 17%), 而同包同芯片的自研多核 6276 -> 6269(只差 0.1%)。
check("A3 目标核 = multiCoreSlotList(t)[index](一个人一个核, 没有 mod 回卷)",
      "const int cpu = spread[(size_t)index];" in spread and re.search(r"index\s*%\s*n", spread) is None
      and "const std::vector<int>& spread = multiCoreSlotList(t);" in spread)
check("A4 index 越界 -> 不绑、不回卷(返回 -5), 不与别人挤同一个核",
      "index >= n" in spread and "*errOut = -5;" in spread)
check("A5 绑定后立刻读回且要求掩码恰好是那一个核(独占自证)",
      "sched_getaffinity(0, sizeof(back), &back)" in spread and "backMask != (1ull << (unsigned)cpu)" in spread)
check("A6 读回被夹回 -> 不算成功(errno=-2), 由会话层计数",
      "*errOut = -2;" in spread and "workClampedMask" in src)

# ---- ② 两阶段策略分开 ----
begin = body_of(r"inline void auroraAffinitySessionBegin\(int threads\)", src)
check("A7 会话里区分单核/多核阶段(multiCore)", "s.multiCore = (s.threads > 1) ? 1 : 0;" in begin)
# A8/A9(2026-10 改): 目标位次数的计算被提取成唯一来源 loadPhaseTargetCount(t, multiCore),
#   会话与自研套件(bench_cpu.cpp)都调它 —— 这正是"两条路径各算一遍"这个缺陷的根治办法。
#   因此断言拆成两条: ① 唯一来源里的规则没变; ② 会话确实调的是它。
lpt = body_of(r"inline int loadPhaseTargetCount\(const CpuTopology& t, bool multiCore\)", src)
check("A8 多核阶段目标 = 全部可用核(multiCoreSlotList 整条序列, 与线程数同口径)",
      "if (multiCore) {" in lpt
      and "return (int)multiCoreSlotList(t).size();" in lpt
      and "t.spreadOrder" not in lpt)

# ★ 2026-10-10 新增: 把这次事故的核心不变式钉死 ——
#   **池线程数 == 多核落点表长度**, 而且两者的最终来源是同一个(可用核序列)。
#   事故回顾(真机 HOP-AL00, 同一台机器 / 同一个包 / 同一颗芯片):
#     套件      8.5(OpenHarmony SDK)   9.6(HarmonyOS SDK)
#     CS1 单核  418.2                  416
#     CS1 多核  1492.8                 1239    <- 低 17%
#     CS1 GPU   4522.4                 5241
#     自研 单核  2799                   2663
#     自研 多核  6276                   6269    <- 一模一样(满血回来)
#     自研 GPU   1070                   1111
#   自研那一组证明 SDK 是干净的; 掉分只在 CS1 这条路径上。根因 = 9.1 把线程数从 6 回退到 8 时
#   只改了"要几个线程", 没改"往哪张表上钉", 于是 index 6/7 越界、拿到 -5 不绑, 又被夹回
#   那 6 个核的掩码里抢时间片。
check("A9 单核阶段目标仍是快簇(最快档), 交集为空时退化为整条可用序列",
      "return t.fallbackTarget ? (int)t.order.size() : (int)t.fastCpus.size();" in lpt)
check("A9b 会话调的是这个唯一来源(不再自己算一遍)",
      "const int targetCount = aurora_cpu_detail::loadPhaseTargetCount(t, s.multiCore != 0);" in begin)
# 自研套件那一侧: bench_cpu.cpp 只调公开入口, 公开入口再调同一个 loadPhaseTargetCount。
# 两条路径因此共用一个来源 —— 这是"两台同芯片设备自研单核差 37.7%"那个缺陷的结构性根治。
single_entry = body_of(r"inline int auroraBindCurrentThreadToSingleLoadTarget\(int\* errOut = nullptr\)", src)
check("A9c 自研套件单核阶段与 CS1 单核阶段共用同一个目标函数",
      "aurora_cpu_detail::loadPhaseTargetCount(t, false)" in single_entry
      and "auroraBindCurrentThreadToSingleLoadTarget" in rd("bench_cpu.cpp")
      and "loadPhaseTargetCount" not in rd("bench_cpu.cpp"))
check("A10 多核阶段不再占用位次 0(那个核要留给池线程 #0 —— 不空不挤)",
      "if (s.multiCore == 0) {" in begin and "claimSlot(0);" in begin)
check("A11 多核阶段记录工作集合位图(供失败线程还原到允许范围内)",
      "s.spreadMask |= (1ull << (unsigned)c);" in begin and "s.spreadCores = targetCount;" in begin)

# ---- ③ 线程数 = 可用核数 ----
cap = body_of(r"inline int auroraThreadCap\(\)", src)
check("A12 线程数上限的第一顺位 = 可用**逻辑**核数(2026-10-06 按真机 A/B 回退: 物理核口径实测慢 34%)",
      "const int usable = (int)aurora_cpu_detail::effectiveOrderCached().size();" in cap
      and "const int usable = (int)aurora_cpu_detail::topologyCached().spreadOrder.size();" not in cap)
check("A13 序列为空时才退回旧口径(不夹)", "if (usable > 0)" in cap and "? allowed : -1;" in cap)

# ---- ④ 每线程落点 + 每核是否有线程 + 自检 ----
wstart = body_of(r"inline void auroraAffinityWorkerStart\(int index\)", src)
check("A14 池线程记录: 目标核(物理核口径) / 绑定后落点 / 是否独占",
      "s.workTarget[index] = spreadRef[(size_t)index];" in wstart
      and "s.workLanding[index] = landing;" in wstart and "s.workBound[index] = 1;" in wstart)
check("A15 绑定失败要计数(不许静默)", "s.workersUnbound.fetch_add(1, std::memory_order_relaxed);" in wstart)
#   这一条断言把"两边必须同源"逐条钉住: 线程数上限 / 落点表 / 位次数 / 逐线程绑定 四处都指向
#   同一个 multiCoreSlotList(t), 而它返回的就是 t.order(= effectiveOrderCached() 的过滤结果)。
check("A8b ★不变式: 多核池线程数与落点表同口径(线程数 = t.order 长度, 落点表 = t.order; 真机 1239 vs 1492.8 的根因)",
      "inline const std::vector<int>& multiCoreSlotList(const CpuTopology& t)" in src
      and "return t.order;" in body_of(r"inline const std::vector<int>& multiCoreSlotList\(const CpuTopology& t\)", src)
      and "return (int)multiCoreSlotList(t).size();" in lpt
      and "const std::vector<int>& spread = multiCoreSlotList(t);" in spread
      # 线程数上限那一侧: 必须仍然取"可用逻辑核数"(与 t.order 同源), 不许回到物理核投影
      and "const int usable = (int)aurora_cpu_detail::effectiveOrderCached().size();" in cap
      # 逐线程记录的目标核也必须来自同一个表
      and "s.workTarget[index] = spreadRef[(size_t)index];" in wstart
      and "const std::vector<int>& spreadRef = aurora_cpu_detail::multiCoreSlotList(t);" in wstart
      # 会话级铺满掩码同源
      and "const std::vector<int>& spreadRef = aurora_cpu_detail::multiCoreSlotList(t);" in begin
      # 而且全文件里不许再有"落点表可以换成物理核投影"的第二种写法
      and "? t.spreadOrder : t.order" not in src)

check("A16 绑定失败时把线程保持在工作集合内(不比可用核集合更宽)",
      "if (s.multiCore != 0 && s.spreadMask != 0ull) {" in wstart)
check("A17 存在【干完活】钩子(结束落点: 迁移是可观测事实)",
      "inline void auroraAffinityWorkerEnd(int index)" in src
      and "auroraAffinityWorkerEnd(t);" in par
      and par.find("auroraAffinityWorkerEnd(t);") > par.find("body(start, end);"))
check("A18 executor 的任务分解一个字没动(chunk=64 / join 顺序 / threads<=1 串行)",
      "const long long chunk = 64;" in par and "threads <= 1" in par
      and par.find("pool[i].join();") > 0)

send = body_of(r"inline AuroraCpuPlacement auroraAffinitySessionEnd\(\)", src)
check("A19 会话结束统计: 实际用到的核数 M(起始 ∪ 结束落点)",
      "s.workUsedMask.load(std::memory_order_relaxed) |" in send and "p.usedCores = m;" in send)
check("A20 会话结束统计: 工作集合里没有线程落到的空核数", "p.emptyCores = empty;" in send)
check("A21 自检结论按原因链给出(工作集合窄 / 线程数被夹 / 池线程没绑上 / 有空核)",
      "本项工作集合只有 %d 核(可用 %d)" in send and "本项线程数被夹到 %d" in send
      and "实际建起来的池线程只有 %d 个" in send and "没能绑到独占核" in send
      and "没有任何线程落到" in send)
check("A22 note 里出现【本项用到 M/N 个核】(不许静默)",
      "本项用到 %d/%d 个核" in send and "p.usedCores, p.availCores" in send)
check("A23 铺满取证一行含: 线程数 / 核清单 / 每核是否有线程 / 逐线程落点",
      "多核铺满取证" in send and "每核是否有线程" in send and "逐线程落点(目标@绑定后→结束)" in send)

ptxt = body_of(r"inline std::string auroraCpuPlacementText\(const AuroraCpuPlacement& p\)", src)
check("A24 铺满取证被拼进每项 note(cpuAllowedText 之外的那一行)", "s += p.spreadText;" in ptxt)
check("A25 多核阶段上报的【工作集合】就是全部可用核(不再拿最快档当判据)",
      "p.fastClusterMask = p.targetMask;" in send and "不使用快簇限制" in send)

# ---- ⑤ /proc/stat 逐核占用 ----
check("A26 采样器读 /proc/stat 逐核 jiffies(整机口径)",
      "struct ProcStatSnapshot" in freq and "readProcStatSnapshot" in freq
      and "out->busy[cpu] = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];" in freq
      and "out->idle[cpu] = v[3] + v[4];" in freq)
check("A27 两次读盘分别在计时区间之外(markStart 之前 / markStop 之后)",
      freq.find("(void)readProcStatSnapshot(&g_s.statStart);") < freq.find("g_s.armed = 1;")
      and freq.find("(void)readProcStatSnapshot(&g_s.statStop);") < freq.find("g_s.stopped = 1;"))
check("A28 note 里给出逐核占用率 + 空闲比例 + 完全没干活的核",
      "逐核占用 = " in freq and "空闲比例" in freq and "完全没干活的核" in freq)
check("A29 读不到就写读不到(不填 0 冒充)", "无法判断是否有核空着" in freq)

# ---- ⑥ 不许动的东西 ----
check("A30 计分公式一个字未动", "score = k * (value * e.conv);" in gb7)
check("A31 k/conv 未触碰(抽查 3 项)",
      "7.027686550" in gb7 and "181.6940430" in gb7 and "184.8221414" in gb7)
LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]
ms = sum(rd(f).count("auroraFreqMarkStart();") for f in LOAD_FILES)
me = sum(rd(f).count("auroraFreqMarkStop();") for f in LOAD_FILES)
check("A32 16 项负载的打点数量未变(16/16)", ms == 16 and me == 16, "%d/%d" % (ms, me))
check("A33 多核阶段的新代码里没有按机型/SoC 的分支",
      not re.search(r"Pura|HOP-AL00|Kirin|kirin|Snapdragon|MediaTek|Exynos", spread + begin + send))
check("A34 负载文件里没有出现新的绑核/铺核调用(只动 executor 与亲和性层)",
      all(("bindWorkerCoreSpread" not in rd(f)) and ("spreadMask" not in rd(f)) for f in LOAD_FILES))
check("A35 每项的线程数仍由 auroraCapThreads 唯一夹取(没有第二处夹法)",
      par.count("auroraCapThreads(threads)") == 1)

# ---------------------------------------------------------------- B) 数字镜像
emit("")
emit("=" * 100)
emit("[B] 数字镜像(真机形状: 可用核集合 0-8, SMT 关时 2270 档只剩部分物理核)")
emit("=" * 100)

KMAX = 32

def build_topo(slot_freq):
    """buildTopo() 的逐字镜像: 同频并档; 不足 2 核继续吸收下一档; 尾组为 1 且组数>=3 并入上一组。"""
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

def new_thread_cap(usable, smt_on, es_count, allowed, known=True):
    """auroraThreadCap() 的逐字镜像(2026-10 新规则)。"""
    if usable > 0:
        return usable
    cap = -1
    if (not smt_on) and known and es_count > 0:
        cap = es_count
    if allowed > 0 and (cap < 0 or allowed < cap):
        cap = allowed
    return cap

def old_thread_cap(smt_on, es_count, allowed, known=True):
    """旧规则(全机物理核数): 复现"9 线程抢 7 核"用的。"""
    cap = -1
    if (not smt_on) and known and es_count > 0:
        cap = es_count
    if allowed > 0 and (cap < 0 or allowed < cap):
        cap = allowed
    return cap

def spread_targets(order, threads):
    """bindWorkerCoreSpread() 的目标核镜像: 线程 i -> order[i]; i >= N 不绑(不回卷)。"""
    out = []
    for i in range(threads):
        out.append(order[i] if i < len(order) else None)
    return out

# ---- B1: 真机形状(允许 0-8; 2270 档的 5 个逻辑核落在 3 个物理核上) ----
SIB = {4: 4, 5: 4, 6: 6, 7: 6, 8: 8}      # SMT 关时的物理核代表关系
ALLOWED = list(range(0, 9))
FREQ = {}
for c in range(0, 4):  FREQ[c] = 2100000
for c in range(4, 9):  FREQ[c] = 2270000
order_all = sorted(ALLOWED, key=lambda c: (-FREQ[c], c))
reps = []
seen = set()
for c in order_all:
    p = SIB.get(c, c)
    if p in seen:
        continue
    seen.add(p)
    reps.append(c)
usable_off = len(reps)                      # 可用核数(SMT 关)
es_count = 9                                # 真机 note: physical=9
old_cap = old_thread_cap(False, es_count, len(ALLOWED))
new_cap = new_thread_cap(usable_off, False, es_count, len(ALLOWED))
emit("  真机形状: 可用核集合 0-8(9 逻辑核) · 2270 档 = cpu4-8(其中 2 对共享物理核)")
emit("     SMT 关时可用核数 N = %d(核: %s)" % (usable_off, ",".join("cpu%d" % c for c in reps)))
emit("     旧规则 cap = %d(全机物理核数) -> 线程 %d 个抢 %d 个核" % (old_cap, old_cap, usable_off))
emit("     新规则 cap = %d(可用核数)   -> 线程 %d 个配 %d 个核" % (new_cap, new_cap, usable_off))

old_t = spread_targets(reps, old_cap)
old_unbound = len([x for x in old_t if x is None])
new_t = spread_targets(reps, new_cap)
new_unbound = len([x for x in new_t if x is None])
emit("     旧: 目标核 %s -> 无处可去的线程 %d 个(= 真机 note 的'位次冲突')" % (old_t, old_unbound))
emit("     新: 目标核 %s -> 无处可去的线程 %d 个" % (new_t, new_unbound))

# ---- B2: 穷举(目标核两两不同, 并集 == 工作集合) ----
bad = None
cases = 0
for n in range(2, 17):
    order = list(range(n))
    for perm in [order, list(reversed(order)), order[1:] + order[:1]]:
        cases += 1
        t = spread_targets(perm, len(perm))
        s = set(t)
        if len(s) != len(perm) or any(x is None for x in t) or s != set(perm):
            bad = (n, perm, t)
check("B2a 穷举 %d 组: 线程 i 的目标核两两不同, 且并集恰好 == 工作集合(不空、不挤)" % cases, bad is None, bad)

# ---- B3: M < N 时必须给原因 ----
def verdict(usable, threads, built, bound, used, empty, spread):
    """会话层自检文本的镜像(只看"原因链是否命中")。"""
    if used >= usable:
        return "无 —— 已铺满"
    reasons = []
    if spread < usable:
        reasons.append("工作集合窄")
    if threads < usable:
        reasons.append("线程数被夹")
    if built < threads:
        reasons.append("池线程建得少")
    if bound < built:
        reasons.append("有线程没绑上")
    if empty > 0:
        reasons.append("有空核")
    return ",".join(reasons) if reasons else "未定位"

r1 = verdict(9, 9, 7, 7, 7, 2, 9)
r2 = verdict(9, 9, 9, 9, 9, 0, 9)
r3 = verdict(9, 4, 4, 4, 4, 5, 9)
emit("")
emit("  自检镜像: 9 可用/9 线程/建起 7/绑上 7/用到 7 -> 原因 = %s" % r1)
emit("            9 可用/9 线程/建起 9/绑上 9/用到 9 -> 原因 = %s" % r2)
emit("            9 可用/夹到 4 线程/用到 4      -> 原因 = %s" % r3)

# ---- B4: /proc/stat 解析镜像 ----
def parse_proc_stat(text):
    """readProcStatSnapshot() 的逐字镜像(只算 busy/idle)。"""
    busy, idle = {}, {}
    for line in text.split("\n"):
        m = re.match(r"cpu(\d+)\s+(.*)$", line)
        if not m:
            continue
        c = int(m.group(1))
        v = [int(x) for x in m.group(2).split()]
        while len(v) < 10:
            v.append(0)
        busy[c] = v[0] + v[1] + v[2] + v[5] + v[6] + v[7]
        idle[c] = v[3] + v[4]
    return busy, idle

S1 = "\n".join([
    "cpu  1000 0 500 8000 0 0 0 0 0 0",
    "cpu0 100 0 50 850 0 0 0 0 0 0",     # 忙 150 / 闲 850  -> 15% 忙
    "cpu1 200 0 0 800 0 0 0 0 0 0",      # 忙 200 / 闲 800  -> 20% 忙
    "cpu2 0 0 0 1000 0 0 0 0 0 0",       # 全程空闲
    "intr 1 2 3",
])
S2 = "\n".join([
    "cpu  1010 0 505 8100 0 0 0 0 0 0",
    "cpu0 110 0 55 895 0 0 0 0 0 0",     # Δ忙 15 / Δ闲 45
    "cpu1 220 0 0 880 0 0 0 0 0 0",      # Δ忙 20 / Δ闲 80
    "cpu2 0 0 0 1100 0 0 0 0 0 0",       # Δ忙 0  / Δ闲 100 -> 完全没干活
    "intr 9 9",
])
b1, i1 = parse_proc_stat(S1)
b2, i2 = parse_proc_stat(S2)
stats = {}
for c in (0, 1, 2):
    db = b2[c] - b1[c]
    di = i2[c] - i1[c]
    tot = db + di
    stats[c] = (db, di, tot, 100.0 * db / tot if tot > 0 else 0.0)
idle_cores = [c for c in stats if stats[c][0] == 0]
avg_idle = sum(100.0 - stats[c][3] for c in stats) / len(stats)
emit("")
emit("  /proc/stat 镜像: %s" % ", ".join("cpu%d 忙%.0f%%(%d/%d)" % (c, stats[c][3], stats[c][0], stats[c][2]) for c in sorted(stats)))
emit("     完全没干活的核 %d/%d: %s; 空闲比例(各核均值) %.1f%%"
     % (len(idle_cores), len(stats), ",".join("cpu%d" % c for c in idle_cores), avg_idle))

emit("")
emit("=" * 100)
emit("[C] 断言")
emit("=" * 100)
checks = [
    ("旧规则复现真机现象: 线程数(9) > 可用核数(%d) -> 有线程无处可去(= 位次冲突)" % usable_off,
     old_cap > usable_off and old_unbound == old_cap - usable_off),
    ("新规则: 线程数 == 可用核数 -> 每个线程都有自己的核", new_cap == usable_off and new_unbound == 0),
    ("新规则下目标核两两不同且并集 == 工作集合(不空、不挤)",
     len(set(new_t)) == len(new_t) and set(new_t) == set(reps)),
    ("M<N 且池线程建得少/有空核 -> 原因链必须命中", r1 == "池线程建得少,有空核"),
    ("M==N -> 结论是'已铺满'(不报假原因)", r2 == "无 —— 已铺满"),
    ("线程数被夹 -> 原因里必须出现'线程数被夹'", "线程数被夹" in r3),
    ("/proc/stat 口径正确: busy=user+nice+system+irq+softirq+steal, idle=idle+iowait",
     b1[0] == 150 and i1[0] == 850 and b1[1] == 200 and i1[2] == 1000),
    ("逐核差值 -> 空闲比例可算(cpu0: Δ忙 15 / Δ闲 45 = 25%% 忙)", abs(stats[0][3] - 25.0) < 0.01),
    ("'完全没干活的核'能被识别(cpu2: Δ忙 = 0)", idle_cores == [2]),
]
for name, ok in checks:
    check(name, ok)

emit("")
emit("=" * 100)
if not fails:
    emit("结论: 全部 PASS —— 多核阶段 = 一核一线程, 线程数 = 可用核数(可用核集合 ∩ SMT 口径),")
    emit("      工作集合 = 全部可用核(不再用快簇限制), 每项 note 自带线程数/核清单/每核是否有线程/")
    emit("      逐核占用率/空闲比例与 M<N 的原因; 单核阶段仍按最快档绑核。")
else:
    emit("结论: 有 %d 项 FAIL: %s" % (len(fails), ", ".join(fails)))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
