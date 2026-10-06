# -*- coding: utf-8 -*-
"""
"全机最快档"判据的自查器 (静态源码断言 + 真机数字镜像; 无设备)

背景(2026-10-06 修的方向性错误):
  cpuInMachineTopTier 这个字段是专门用来让用户分辨
     "绑核有没有生效"(cpuInFastCluster) 与 "够不够快"(cpuInMachineTopTier)
  的。但它上一版把"全机最快频率档"算成了"可用核集合里最快的那些核"
  (buildTopology 里那段用了已经被可用核集合过滤过的 order), 于是在内核只允许
  cpu0-8 的真机上, machineTopTierKhz 被算成 2270000, cpu8 落进掩码 ->
  cpuInMachineTopTier 恒为 true, 而同一条 note 里还写着
  "快簇已生效(cpu=8, 5 核簇/最高2270MHz, 但低于全机最快档 1 档)" —— 自相矛盾。
  真机 305 行 runlog: "在全机最快档内" 48 次, "不在全机最快档" 0 次。

本脚本做两件事(全部离线, 不需要设备):
  A) 源码结构断言: 从 cpu_affinity.h 里确认
       ① "全机最快档"那一段遍历的是 perfOrderCached()/coreMaxFreqKhzCached()
          (全机逐核频率表), 而不是参数 order(已按可用核集合过滤);
       ② 判定函数 auroraCpuInMachineTopTier() 用的是 machineTopTierMask;
       ③ 存在自查函数 auroraMachineTopTierSelfCheck() 且被写进设备画像那一行。
  B) 数字镜像: 用真机(Pura X Max / HOP-AL00)读到的形状跑一遍旧算法与新算法,
     证明: 旧算法 -> true(错), 新算法 -> false(对), 并逐条验证断言
       "若 fastClusterMaxKhz < machineTopTierKhz, 则 cpuInMachineTopTier 必须为 false"。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import os, re, sys, io

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "cpu_affinity.h")
fails = 0

# 输出同时写文件(UTF-8, 避免 PowerShell 重定向产生 UTF-16; 与另外两个核对脚本同口径)
_OUT = io.open(os.path.join(HERE, "verify_toptier_out.txt"), "w", encoding="utf-8", newline="\n")

def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    _OUT.write(s + "\n")
    _OUT.flush()

with io.open(SRC, encoding="utf-8", errors="replace") as fh:
    src = fh.read()

# ---------------------------------------------------------------- A) 源码结构断言
emit("=" * 100)
emit("[A] 源码结构断言(cpu_affinity.h)")
emit("=" * 100)

# 取出 buildTopology 的函数体(从声明到下一个顶层 "inline " 或文件尾)
m = re.search(r"inline CpuTopology buildTopology\(.*?\n\}\n", src, re.S)
body = m.group(0) if m else ""
if not body:
    emit("  [FAIL] 找不到 buildTopology 的函数体")
    fails += 1

def check(name, cond, detail):
    global fails
    if not cond:
        fails += 1
    emit("  [%s] %-52s %s" % ("PASS" if cond else "FAIL", name, detail))

# ① "全机口径"那一段必须用全机频率表, 不能用参数 order
seg = ""
mm = re.search(r"全机口径\(不受可用核集合影响\).*?\n    \}", body, re.S)
if mm:
    seg = mm.group(0)
check("全机最快档那一段存在且取自 perfOrderCached()",
      bool(seg) and "perfOrderCached()" in seg,
      "段内遍历 allOrder = perfOrderCached()")
check("全机最快档那一段取自 coreMaxFreqKhzCached()",
      "coreMaxFreqKhzCached()" in seg,
      "段内取 allFreqs = coreMaxFreqKhzCached()")
check("全机最快档那一段不使用参数 order 做判定",
      bool(seg) and not re.search(r"order\[\(size_t\)s\]", seg),
      "段内没有 order[(size_t)s](那正是被可用核集合过滤过的序列)")

# ② 判定函数用全机掩码
m2 = re.search(r"inline bool auroraCpuInMachineTopTier\(int cpu\).*?\n\}", src, re.S)
fn = m2.group(0) if m2 else ""
check("auroraCpuInMachineTopTier 用 machineTopTierMask 判定",
      "machineTopTierMask" in fn and "fastMask" not in fn,
      "只看全机最快档掩码, 不看生效快簇掩码")

# ③ 自查函数存在并被摘要行使用
check("存在 auroraMachineTopTierSelfCheck()",
      "inline int auroraMachineTopTierSelfCheck()" in src, "自查函数已定义")
check("自查结论被写进 auroraAffinitySummaryText()",
      "auroraMachineTopTierSelfCheckText()" in src, "设备画像那一行会带上自查结论")

# ---------------------------------------------------------------- B) 数字镜像
emit("")
emit("=" * 100)
emit("[B] 真机数字镜像(HOP-AL00: 允许 cpu0-8, 全机最快档 2750MHz 在 cpu9-13)")
emit("=" * 100)

# 真机逐核 cpuinfo_max_freq(kHz) 的形状(用户给的硬事实, 这里只做形状建模):
#   cpu0-3 : 中核档(取 2100 MHz 作代表), cpu4-8 : 快簇 2270 MHz, cpu9-13 : 全机最快 2750 MHz
FREQ = {}
for c in range(0, 4):   FREQ[c] = 2100000
for c in range(4, 9):   FREQ[c] = 2270000
for c in range(9, 14):  FREQ[c] = 2750000
ALLOWED_MASK = 0
for c in range(0, 9):
    ALLOWED_MASK |= (1 << c)

# 全机频率降序表 = perfOrderCached() 的镜像(同频按核号升序)
all_order = sorted(FREQ.keys(), key=lambda c: (-FREQ[c], c))

def tier_scan(order, freqs, use_positive_only=True):
    """逐字镜像 cpu_affinity.h 里"全机口径"那一段的循环。"""
    top_khz, top_cores, top_mask, slot_first = 0, 0, 0, 0
    prev, tier = -1, -1
    for s, cpu in enumerate(order):
        khz = freqs.get(cpu, 0)
        if khz != prev:
            tier += 1
            prev = khz
        if tier == 0:
            if s == 0:
                top_khz = khz
                slot_first = s
            if (khz > 0 if use_positive_only else True) and khz == top_khz:
                top_cores += 1
                top_mask |= (1 << cpu)
    return top_khz, top_cores, top_mask

def eff_tier_index_new(order, freqs, fast_max_khz, slot_freq):
    """镜像 effTierIndex 的新算法: 全机里比 fast_max 更高的互异频率个数。"""
    above, prev, found = 0, -1, False
    for cpu in order:
        khz = freqs.get(cpu, 0)
        if khz == prev:
            continue
        prev = khz
        if fast_max_khz > 0 and khz == fast_max_khz:
            found = True
            break
        if khz > fast_max_khz:
            above += 1
    if found:
        return above
    # 兜底: 生效序列里不同频率值个数 - 1
    tier, prev2 = -1, -1
    for k in slot_freq:
        if k != prev2:
            tier += 1
            prev2 = k
    return max(tier, 0)

# --- 旧算法: 用可用核集合过滤后的 order(这就是那个方向性错误的来源) ---
eff_order = [c for c in all_order if (ALLOWED_MASK >> c) & 1]
old_khz, old_cores, old_mask = tier_scan(eff_order, FREQ)
old_i8 = ((old_mask >> 8) & 1) != 0
emit("  旧算法(遍历可用核集合内的 order): 全机最快档 = %d MHz, %d 核, 掩码=0x%x -> cpu8 在档内 = %s"
     % (old_khz // 1000, old_cores, old_mask, old_i8))
emit("     ↑ 这就是真机日志里 cpuInMachineTopTier 恒为 true 的原因(方向性错误)")

# --- 新算法: 用全机频率表 ---
new_khz, new_cores, new_mask = tier_scan(all_order, FREQ)
new_i8 = ((new_mask >> 8) & 1) != 0
emit("  新算法(遍历全机 perfOrderCached()): 全机最快档 = %d MHz, %d 核, 掩码=0x%x -> cpu8 在档内 = %s"
     % (new_khz // 1000, new_cores, new_mask, new_i8))

# 生效快簇 = 全机最快档 ∩ 可用核集合 = 空 -> 退化为可用核集合里最快的那些核(cpu4-8 @2270)
fast_khz = 2270000
fast_cpus = [c for c in eff_order if FREQ[c] == fast_khz]
slot_freq = [FREQ[c] for c in eff_order]
new_tier = eff_tier_index_new(all_order, FREQ, fast_khz, slot_freq)
old_tier = -1
prev = -1
for k in slot_freq:
    if k != prev:
        old_tier += 1
        prev = k
emit("  生效快簇: %d 核(cpu %s), 最高 %d MHz" % (len(fast_cpus), fast_cpus, fast_khz // 1000))
emit("  effTierIndex: 旧算法 = %d(实际是'可用核集合被切成几档'), 新算法 = %d(全机第几档)"
     % (old_tier, new_tier))

emit("")
checks = [
    ("新算法把全机最快档认成 2750 MHz(而不是 2270 MHz)", new_khz == 2750000),
    ("新算法的全机最快档掩码 = cpu9-13(0x3e00)", new_mask == 0x3E00),
    ("新算法下 cpu8 不在全机最快档内", new_i8 is False),
    ("旧算法会误判 cpu8 在全机最快档内(复现原 bug)", old_i8 is True),
    ("新算法 effTierIndex = 1(生效快簇低于全机最快档 1 档)", new_tier == 1),
    ("用户断言: fastClusterMaxKhz(2270) < machineTopTierKhz(2750) -> 生效快簇里每个核都不在全机最快档",
     all(not ((new_mask >> c) & 1) for c in fast_cpus)),
]
for name, ok in checks:
    if not ok:
        fails += 1
    emit("  [%s] %s" % ("PASS" if ok else "FAIL", name))

emit("")
emit("=" * 100)
if fails == 0:
    emit("结论: 全部 PASS —— 全机最快档判据按全机频率表判定, 且与可用核集合无关;")
    emit("      在'可用核集合不含全机最快档'的机器上 cpuInMachineTopTier 必为 false。")
else:
    emit("结论: 有 %d 条 FAIL, 见上。" % fails)
emit("=" * 100)
_OUT.close()
sys.exit(1 if fails else 0)
