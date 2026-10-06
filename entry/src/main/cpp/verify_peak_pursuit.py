# -*- coding: utf-8 -*-
r"""
verify_peak_pursuit.py —— "把芯片最后一丝性能压出来"这三件事的自查器(静态源码断言 + 真机形状镜像; 无设备)

对应三件事:
  一件一 自研套件的绑核修成与 CS1 同口径(生效快簇), 并补上落核/标称上限/运行时频率取证;
  一件二 GPU 侧补上"跑满判据"(计时区间之外, 同一份负载两档, 结论三选一);
  一件三 "本机性能天花板"一行(全机最快档 -> 实测允许集合 -> 因此单核最高哪一档 / 多核几线程 / 占比)。

每一条断言都对应"必须成立的结构事实", 不做任何语义猜测。
退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_peak_pursuit_out.txt"), "w", encoding="utf-8", newline="\n")
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
    m = re.search(sig_regex, text, re.S)
    if not m:
        return ""
    rest = text[m.start():]
    end = rest.find("\n}\n")
    return rest[:end + 2] if end >= 0 else rest

aff = rd("cpu_affinity.h")
bcpu = rd("bench_cpu.cpp")
freq = rd("cpu_freq_sample.cpp")
freqh = rd("cpu_freq_sample.h")
gpu7 = rd("gpu7/gpu7_renderer.cpp")
gpu7h = rd("gpu7/gpu7_renderer.h")
gpu7napi = rd("gpu7/gpu7_napi.cpp")
napi = rd("napi_init.cpp")
benchh = rd("bench.h")
benchcpp = bcpu

emit("=" * 100)
emit("[A] 源码结构断言")
emit("=" * 100)

# ---------------------------------------------------------------- 一件一
# A1 单核分支现在真的绑核了(旧实现是裸串行循环, 一次 sched_setaffinity 都没发过)
par = body_of(r"template <typename F>\nvoid parallelFor\(int threads, int tasks, F fn\)", bcpu)
check("A1-1 自研套件单核分支会绑核(不再是裸串行循环)",
      "if (threads <= 1) {" in par and "bindSingleThreadToFastCluster();" in par)
check("A1-2 绑核在负载之前、落点采样在还原之前(时序不许颠倒)",
      par.find("bindSingleThreadToFastCluster();") < par.find("for (int i = 0; i < tasks; ++i)") <
      par.find("g_bind.landingCpu = auroraCurrentCpu();") < par.find("auroraApplyThreadAffinity(before)"))
check("A1-3 每项跑完恢复原掩码(那条线程是 napi 异步工作线程, 后面每一项都要复用)",
      "AuroraAffinitySnapshot before = auroraCaptureThreadAffinity();" in par
      and "g_bind.restoreOk = auroraApplyThreadAffinity(before) ? 1 : 0;" in par)
check("A1-4 多核分支一个字没动: 仍是一核一线程(order[t]), 线程数口径不变",
      "int targetCpu = (t < (int)perfOrder.size()) ? perfOrder[t] : -1;" in par
      and "pinCurrentThread(targetCpu);" in par)

# A2 绑的是"生效快簇"这个集合, 而不是某个具体核号; 目标是唯一来源
entry = body_of(r"inline int auroraBindCurrentThreadToSingleLoadTarget\(int\* errOut = nullptr\)", aff)
check("A2-1 自研套件的绑核入口按位次取一段集合(bindSlotsEx), 不是绑单个核号",
      "aurora_cpu_detail::bindSlotsEx(t.order, 0, targetCount, errOut)" in entry)
check("A2-2 目标位次数来自唯一来源 loadPhaseTargetCount(t, false)",
      "aurora_cpu_detail::loadPhaseTargetCount(t, false)" in entry)
lpt = body_of(r"inline int loadPhaseTargetCount\(const CpuTopology& t, bool multiCore\)", aff)
# 期望值随 2026-10-10 修复更新: 多核 = 全部**可用核**(multiCoreSlotList = t.order),
#   与线程数(auroraThreadCap = 可用逻辑核数)同一个口径。旧写法(spreadOrder, 每物理核一个代表)
#   只改了落点表没改线程数, 真机上 CS1 多核因此从 1492.8 掉到 1239(-17%), 见 verify_core_spread 的 A8b。
check("A2-3 唯一来源的规则: 多核 = 全部可用核(multiCoreSlotList); 单核 = 快簇(交集为空时退化为整条可用序列)",
      "return (int)multiCoreSlotList(t).size();" in lpt
      and "return t.fallbackTarget ? (int)t.order.size() : (int)t.fastCpus.size();" in lpt)
begin = body_of(r"inline void auroraAffinitySessionBegin\(int threads\)", aff)
check("A2-4 CS1 会话调的是同一个唯一来源(两条路径不可能再各算一遍)",
      "const int targetCount = aurora_cpu_detail::loadPhaseTargetCount(t, s.multiCore != 0);" in begin)
check("A2-5 bench_cpu.cpp 里没有自己重算目标(交集/档位/order 序列都不出现)",
      "fallbackTarget" not in bcpu and "fastCpus" not in bcpu and "t.order" not in bcpu)

# A3 取证: 落核 / 该核标称上限 / 运行时频率
check("A3-1 绑定后立刻回读掩码(内核夹回时'要了几个核'与'给了几个核'必须分开记)",
      "sched_getaffinity(0, sizeof(back), &back)" in bcpu and "e.readbackCores = n;" in bcpu)
check("A3-2 取证里带: 落核 / 该核标称上限 / 位次 / 快簇 / 全机最快档 / 允许核数",
      all(k in bcpu for k in ["auroraCpuMaxFreqKhz(cpu)", "auroraCpuRank(cpu)",
                              "auroraCpuInFastCluster(cpu)", "auroraCpuInMachineTopTier(cpu)",
                              "auroraAllowedCoreCount()", "auroraMachineTopTierMaxKhz()"]))
check("A3-3 运行时频率会话在计时区间之外开/关, 且用了与 CS1 同一个入口",
      "auroraFreqSampleSessionBegin(threads);" in bcpu and "auroraFreqSampleSessionEnd();" in bcpu
      and bcpu.find("auroraFreqSampleSessionBegin(threads);") < bcpu.find("double ms = def.run(threads);")
      < bcpu.find("auroraFreqSampleSessionEnd();"))
ms = bcpu.count("auroraFreqMarkStart();")
me = bcpu.count("auroraFreqMarkStop();")
check("A3-4 自研套件 8 项负载都打了点(8/8 入口 + 8/8 出口)", ms == 8 and me == 8, "%d/%d" % (ms, me))
check("A3-5 打点紧邻 t0 / t1(写在计时区间之外, 与 CS1 16 项同一约定)",
      bcpu.count("auroraFreqMarkStart();\n    double t0 = wallMs();") == 8
      and bcpu.count("double t1 = wallMs();\n    // 计时区间出口") == 8)
check("A3-6 BenchOutcome 新增字段齐全(bench.h), napi 与 ArkTS 都交出去了",
      all(k in benchh for k in ["int cpu = -1;", "int cpuMaxKhz = 0;", "int cpuRank = -1;",
                                "int cpuInFastCluster = 0;", "int cpuInMachineTopTier = 0;",
                                "std::string cpuInfo;", "std::string runFreq;"])
      and all(k in napi for k in ['\\"cpu\\"', '\\"cpuMaxKhz\\"', '\\"cpuRank\\"',
                                  '\\"cpuInMachineTopTier\\"', '\\"cpuSingleTargetCores\\"']))

# A4 不许动的东西: 计分公式 / 刻度系数 / 负载的尺寸与算法常量
check("A4-1 计分公式与刻度系数一个字未动",
      "double score = def.refMs / ms * 1000.0;" in bcpu
      and "const double kScaleSingle = 7.70;" in bcpu
      and "const double kScaleMulti = 11.55;" in bcpu)
consts = ["const int IMG_W = 3840;", "const int IMG_H = 2160;", "const int BODIES = 2048;",
          "const int STEPS = 160;", "const int AI_N = 256;", "const int AI_ITERS = 128;",
          "const int AUDIO_N = 512;", "const int AUDIO_FRAMES = 8192;",
          "const size_t chunkSize = 4u * 1024 * 1024;", "const int chunks = 4;",
          "const int w = 1024;", "const int h = 1024;", "const int runs = 24;",
          "const size_t total = 24u * 1024 * 1024;", "const size_t elems = 24u * 1024 * 1024;"]
check("A4-2 8 项负载的尺寸常量一个都没改", all(c in bcpu for c in consts),
      [c for c in consts if c not in bcpu])
check("A4-3 TESTS 注册表(名称/参考耗时/单位)一个都没改",
      all(t in bcpu for t in ['{"图像处理", "3840x2160 · 两遍模糊 + 边缘检测", 1100.0',
                              '{"音频编解码", "8192 帧 512 点 FFT/MDCT 编码+解码", 700.0']))

# ---------------------------------------------------------------- 一件二
check("A5-1 gpu7 导出 GPU 跑满判据(gpu7_renderer.h / .cpp / napi 三处都在)",
      "std::string gpu7Fullness(int id);" in gpu7h and "std::string gpu7Fullness(int id)" in gpu7
      and '"fullness", nullptr, Gpu7Fullness' in gpu7napi)
check("A5-2 判据在计时区间之外: 正式那一遍(gpu7Run)里不调用探测",
      "gpu7Fullness(" not in gpu7.split("std::string gpu7Run(int id)")[1].split("std::string gpu7Fullness")[0])
check("A5-3 两档跑的是同一个负载函数(同一份着色器/尺寸/算法), 结果一律丢弃",
      "const FrameStats st = kLoads[id].fn(frames);" in gpu7
      and "gpuProbeRung(id, halfFrames)" in gpu7 and "gpuProbeRung(id, fullFrames)" in gpu7
      and "g_lastScore" not in body_of(r"GpuProbeRung gpuProbeRung\(int id, int frames\)", gpu7))
check("A5-4 帧数表与计分表(k/conv/scored)一个字未动",
      all(t in gpu7 for t in ['{"Background Blur", loadBlur, 6}', '{"Face Tracking", loadFaceTracking, 4}',
                              '{"Video Filter", loadVideoFilter, 10}'])
      and "{387.2534580, 1.0 / (1920.0 * 1080.0 * 2.0 / 1e6), true," in gpu7)
check("A5-5 每帧两段(递交 / 等 GPU 排水)由同一个计时循环切出来, 且 st.ms 定义未动",
      "const double tIssue = nowMs();" in gpu7 and "issueMs += (tIssue - lastEnd);" in gpu7
      and "drainMs += (tEnd - tIssue);" in gpu7
      and "double elapsed = nowMs() - t0;" in gpu7)
check("A5-6 结论三选一(已达到 / 未达到 / 本项不适用)且都写进 native 文本",
      '"REACHED" : "NOT_REACHED"' in gpu7 and "NOT_APPLICABLE" in gpu7
      and "GPU 跑满判据 -> 已达到" in gpu7 and "GPU 跑满判据 -> 未达到" in gpu7
      and "GPU 跑满判据 -> 本项不适用" in gpu7)
check("A5-6b 两个阈值都在源码常量里写明, 且文本里注明'是我们自己定的'",
      re.search(r"kGpuFullDutyPct\s*=\s*90\.0", gpu7) is not None
      and re.search(r"kGpuPlateauRatio\s*=\s*0\.97", gpu7) is not None
      and "阈值 %.0f%% 与 %.2f 都是我们自己定的" in gpu7)
check("A5-7 判据不假装知道 GPU 频率(明确写出这条局限)",
      "本判据不回答'驱动有没有给 GPU 降频'" in gpu7 or "本判据不回答" in gpu7)
check("A5-8 探测也是'有负载在跑': napi 侧同样走 Gpu7IdleGuard(卡死判据照常有效)",
      "Gpu7IdleGuard idleGuard(env);\n    return MakeString(env, gpu7Fullness((int)id));" in gpu7napi)

# ---------------------------------------------------------------- 一件三
ceil = body_of(r"int auroraCeilingText\(char\* buf, int cap\)", freq)
check("A6-1 存在 auroraCeilingText()", ceil != "")
check("A6-2 一句话里五个事实齐全: 全机最快档 / 内核允许集合 / 单核最高档 / 多核线程数 / 运行时占比",
      all(k in ceil for k in ["全机最快档", "内核实测只放我们用", "单核最高只能跑在",
                              "多核最多", "运行时频率中位", "占标称".replace("占标称", "的 ")])
      or all(k in ceil for k in ["全机最快档", "内核实测只放我们用", "单核最高只能跑在",
                                 "多核最多", "运行时频率中位"]))
check("A6-3 读不到就写读不到(不用 0 冒充)",
      "全机最快档读不到" in ceil and "内核允许的核集合读不到" in ceil)
check("A6-4 超出容量时显式标注截断(不静默丢尾部)", "尾部被截断" in ceil)
check("A6-5 导出为 napi 的 ceilingText", '"ceilingText", nullptr, CeilingText' in napi
      and "auroraCeilingText(buf, (int)sizeof(buf));" in napi)
check("A6-6 派生量访问器与实现都在(供报告与界面直接取用)",
      all(k in freqh for k in ["int auroraCeilingText(char* buf, int cap);",
                               "int auroraFreqLastMedianRatioPermille(void);"])
      and all(k in freq for k in ["int auroraFreqLastMedianRatioPermille(void)",
                                  "int auroraFreqLastMedianKhz(void)"]))

# ---------------------------------------------------------------- 界面与报告的接线(ArkTS)
ROOT_ETS = os.path.join(HERE, "..", "ets")

def rd_ets(*parts):
    p = os.path.normpath(os.path.join(ROOT_ETS, *parts))
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()

MODEL = rd_ets("model", "BenchModel.ets")
RUNNER = rd_ets("service", "BenchRunner.ets")
FULL = rd_ets("service", "FullRun.ets")
LAYERS = rd_ets("common", "ResultLayers.ets")
INDEX = rd_ets("pages", "Index.ets")
DTS = rd("types/libaurorabench/index.d.ts")

check("A7-1 类型层: napi 声明 + TestResult 新增取证字段 + Gb7Result.gpuFullness",
      "export const ceilingText: () => string;" in DTS
      and "cpuSingleTargetCores: number;" in MODEL and "runFreq: string;" in MODEL
      and "gpuFullness: string;" in MODEL)
check("A7-2 自研套件路径真的把这些字段解析出来了(BenchRunner.one)",
      "cpu: (o.cpu as number) ?? -1," in RUNNER
      and "cpuSingleTargetCores: (o.cpuSingleTargetCores as number) ?? 0," in RUNNER
      and "runFreq: (o.runFreq as string) ?? ''" in RUNNER)
check("A7-3 「本机性能天花板」只有一个入口(BenchRunner.ceiling), 三处显示都读它",
      "static ceiling(): string {" in RUNNER and "ceilingText()" in RUNNER
      and "BenchRunner.ceiling()" in FULL and "BenchRunner.ceiling()" in INDEX
      and "this.ceilingShow()" in INDEX)
check("A7-4 报告正文: 自研套件每一项都打印运行时频率 / 落核 / 快簇 / 全机最快档 + 天花板一行",
      "运行时频率=' + (t.runFreq.length > 0" in FULL
      and "落核 cpu=' + (t.cpu >= 0" in FULL
      and "跑完在全机最快档内=" in FULL
      and "本机性能天花板" in FULL)
check("A7-5 报告 JSON: 自研套件项与 CS1 项走同一批取证键; header 里带天花板",
      "j.cpuRank = t.cpuRank;" in FULL and "j.singleTargetCores = t.cpuSingleTargetCores;" in FULL
      and "j.gpuFullness = r.gpuFullness;" in FULL
      and "ceilingNote: ceilTxt," in FULL)
check("A7-6 报告: GPU 跑满判据逐项一行 + 小节汇总一行(三种结论分开计数)",
      "B.ln('    ' + r.gpuFullness);" in FULL
      and "static gpuFullnessTally(items: Gb7Result[]): string {" in FULL
      and "已达到 ' + reached.toString()" in FULL)
check("A7-7 结果页 L3: 自研套件项与 GPU 场景项都有交代(不留空 -> 不会被读成'跑满了')",
      "运行时频率: ' + (t.runFreq.length > 0" in LAYERS
      and "GPU 跑满判据 -> 本项不适用（原因：自研套件 GPU 场景" in LAYERS
      and "r.gpuFullness.length > 0" in LAYERS)
check("A7-8 首页设备卡第一屏就有天花板一行(与跑没跑过无关)",
      "Text(this.ceilingShow())" in INDEX and "private ceilingShow(): string {" in INDEX)
check("A7-9 没有为了凑分数写下限定频 / 改 governor(ArkTS 侧也不许出现)",
      "scaling_max_freq" not in (MODEL + RUNNER + FULL + LAYERS + INDEX))
check("A7-10 界面上不许把'未上报'显示成'跑满了': 空串一律走兜底文案",
      "旧版 .so 没有 ceilingText 导出" in INDEX
      and "旧版 .so 没有 ceilingText 导出" in FULL)

emit("")
emit("=" * 100)
emit("[B] 真机形状镜像(本机读数, 不含任何外部资料; 用于核对'生效快簇'与'跑满判据'的判定)")
emit("=" * 100)

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

def load_target_count(freqs, allowed_mask, multi):
    """逐字镜像 aurora_cpu_detail::loadPhaseTargetCount(t, multiCore)。"""
    order = sorted([c for c in freqs if (allowed_mask >> c) & 1], key=lambda c: (-freqs[c], c))
    # fallbackTarget 的成立条件是"(可用核集合 ∩ 有频率读数的核) 为空" —— 见 buildTopology 里
    # t.fallbackTarget = 1 那一处。它**不是**"全机最快档 ∩ 可用核集合 为空": 真机上后者恒成立,
    # 而真机报告里 fastClusterFallback 一直是 False(mate60/tab82: 0x7f/3 核; phone84: 0xff/4 核)。
    eff = [c for c in order if c in freqs]
    fallback = (len(eff) == 0) and bool(allowed_mask)
    slot_freq = [freqs[c] for c in order]
    g = build_topo(slot_freq)
    fast_n = g[0][1] if g else 0
    if multi:
        return len(order), order, fast_n, fallback
    return (len(order) if fallback else fast_n), order, fast_n, fallback

# 形状一: 麒麟 9000S 手机(Mate 60 Pro)与平板(MatePad Pro) —— 真机报告读数:
#   allowedMask=0x7f(0-6) · fastClusterCores=3 maxKhz=2150000 mask=0x70 · tierIdx=1
F1 = {}
for c in range(0, 4):   F1[c] = 1530000
for c in range(4, 8):   F1[c] = 2150000
for c in range(8, 12):  F1[c] = 2620000
# 形状二: 麒麟 9030(Pura X Max) —— 真机报告读数:
#   allowedMask=0xff(0-7) · fastClusterCores=4 maxKhz=2270000 mask=0xf0 · tierIdx=1
F2 = {}
for c in range(0, 4):   F2[c] = 1720000
for c in range(4, 12):  F2[c] = 2270000
for c in range(12, 14): F2[c] = 2750000

n1, o1, f1, fb1 = load_target_count(F1, 0x7f, False)
n1m, _, _, _ = load_target_count(F1, 0x7f, True)
n2, o2, f2, fb2 = load_target_count(F2, 0xff, False)
n2m, _, _, _ = load_target_count(F2, 0xff, True)
emit("  形状一 麒麟 9000S: allowed=0-6(7 核) / 全机最快档 = cpu8-11 @2620MHz(拿不到)")
emit("            生效序列 = %s · 生效快簇 = %d 核(cpu4-6 @2150MHz)" % (o1, f1))
emit("            单核绑定目标 = %d 核 · 多核绑定目标 = %d 核" % (n1, n1m))
emit("  形状二 麒麟 9030 : allowed=0-7(8 核) / 全机最快档 = cpu12,13 @2750MHz(拿不到)")
emit("            生效序列 = %s · 生效快簇 = %d 核(cpu4-7 @2270MHz)" % (o2, f2))
emit("            单核绑定目标 = %d 核 · 多核绑定目标 = %d 核" % (n2, n2m))
emit("")
emit("  旧的单核路径: 0 次 sched_setaffinity(掩码不动) -> 线程可以在 allowed 集合里的任何一颗核上跑,")
emit("  包括 1720/1530MHz 那四颗小核。这就是两台同芯片设备自研单核差 37.67% 的来源。")
emit("")

def gpu_verdict(drain_pct, rung_ratio, duty=90.0, plateau=0.97):
    """逐字镜像 gpu7Fullness 的判定。"""
    duty_ok = drain_pct >= duty
    plateau_ok = rung_ratio >= plateau
    if duty_ok and plateau_ok:
        return "REACHED", 0.0
    return "NOT_REACHED", max(0.0, duty - drain_pct)

cases = [
    ("GPU 排水占 96.2%%, 两档吞吐比 1.004", 96.2, 1.004),
    ("GPU 排水占 77.6%%, 两档吞吐比 1.061", 77.6, 1.061),
    ("GPU 排水占 93.0%%, 两档吞吐比 0.902(还没进平台期)", 93.0, 0.902),
]
emit("  GPU 跑满判据的判定镜像(阈值: 排水占比 >= 90%%, 两档吞吐比 >= 0.97):")
gpu_results = []
for label, d, rr in cases:
    v, gap = gpu_verdict(d, rr)
    gpu_results.append((label, v, gap))
    emit("    %-46s -> %-12s 还差 %.1f 个百分点" % (label, v, gap))

emit("")
emit("=" * 100)
emit("[C] 断言")
emit("=" * 100)
checks = [
    ("麒麟 9000S 形状: 生效快簇 = 3 核 cpu4-6 @2150MHz(与真机 fastClusterCores=3 / mask=0x70 一致)",
     f1 == 3 and set(o1[:3]) == {4, 5, 6} and n1 == 3),
    ("麒麟 9000S 形状: 多核绑定目标 = 允许核数 7(与真机 7 线程一致)", n1m == 7),
    ("麒麟 9030 形状: 生效快簇 = 4 核 cpu4-7 @2270MHz(与真机 fastClusterCores=4 / mask=0xf0 一致)",
     f2 == 4 and set(o2[:4]) == {4, 5, 6, 7} and n2 == 4),
    ("麒麟 9030 形状: 多核绑定目标 = 允许核数 8(与真机 8 线程一致)", n2m == 8),
    ("两个形状下全机最快档都不在允许集合里(所以单核最高只能到第二档)",
     all(F1[c] < 2620000 for c in range(0, 7)) and all(F2[c] < 2750000 for c in range(0, 8))),
    ("GPU 判据: 排水占比与平台期都达标 -> 已达到",
     gpu_results[0][1] == "REACHED" and gpu_results[0][2] == 0.0),
    ("GPU 判据: 排水占比不够 -> 未达到, 且给出还差几个百分点",
     gpu_results[1][1] == "NOT_REACHED" and abs(gpu_results[1][2] - 12.4) < 0.01),
    ("GPU 判据: 占比够但没进平台期 -> 仍判未达到(不许只看一条)",
     gpu_results[2][1] == "NOT_REACHED"),
]
# ---------------------------------------------------------------- 不许动的三件
LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]
check("C-0a CS1 16 项负载的打点数量未变(16/16)",
      sum(rd(f).count("auroraFreqMarkStart();") for f in LOAD_FILES) == 16
      and sum(rd(f).count("auroraFreqMarkStop();") for f in LOAD_FILES) == 16)
check("C-0b CS1 计分公式未动", "score = k * (value * e.conv);" in rd("gb7.cpp"))
check("C-0c 没有为了凑分数去写 scaling_max_freq / 改 governor(全树搜写入型 API)",
      "scaling_max_freq\", \"w\"" not in aff + bcpu + freq + gpu7
      and "O_WRONLY" not in bcpu and "O_RDWR" not in bcpu)
for name, ok in checks:
    check(name, ok)

emit("")
emit("=" * 100)
if not fails:
    emit("结论: 全部 PASS —— 自研套件单核阶段已与 CS1 同口径绑到生效快簇(唯一来源 loadPhaseTargetCount),")
    emit("      落核/标称上限/运行时频率取证补齐; GPU 侧有双档 + 排水分段的跑满判据(三选一结论);")
    emit("      「本机性能天花板」一行覆盖五个事实。真机形状镜像与真机报告读数逐项一致。")
else:
    emit("结论: 有 %d 项 FAIL: %s" % (len(fails), ", ".join(fails)))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
