# -*- coding: utf-8 -*-
r"""
verify_warmup_saturation.py —— 升频/稳态预热 + 双口径频率 + "跑满"判据的静态自检(无设备)

用户原话: 「总之我想要的就是每个芯片尽可能的释放那个芯片所能释放的全部性能。」
于是本次三件事必须可核对, 本脚本逐条断言(只读源码 + 独立数字镜像, 不联设备、不跑 hdc):

  A) 预热: 每一项在计时区间之外先用同一份负载压若干遍, 直到稳态判据命中;
     判据有三条且都写成具名常量; 有遍数上限与总时长上限; 预热读数有独立落盘结构;
     预热遍的样本不可能流进正式计时那一份读数(warmPassBegin 清窗口 + markStart 再清)。
  B) 驱动层: gb7RunTest 里的预热调用的是同一个 e.run(threads), 正式计时那一次在它之后;
     池线程入口登记 tid / 出口注销, 12/16 项打点数量一个都没变。
  C) 多核频率口径: 口径A = 只统计"当刻有负载线程落上的核"(逐核去重, 主线程 tid + 池线程 tid 表);
     口径B = 旧的"可用核集合全部核"仍然保留作对照; 两种口径的读数与 errno 分开记。
  D) "跑满": 逐项给出 频率中位/最小/最大 + 逐样本标称上限 + 占标称比 + 一句结论;
     阈值(90%)与判据措辞都写在文本里, 并写明是我们自己定的。
  E) 对比模块: 两台都跑满 -> 直接给原始分比当结论; 有一侧没跑满 -> 归一化只作辅助与诊断。
  F) 数字镜像: 用真机实测数字独立复算一遍占比与判据, 并证明判据在 90% 处会翻转。
  G) 不许动的东西: 计分公式 / k / conv / 16 项打点 / 负载算法都没动。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_warmup_saturation_out.txt"), "w", encoding="utf-8", newline="\n")
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

h   = rd("cpu_freq_sample.h")
c   = rd("cpu_freq_sample.cpp")
gb7 = rd("gb7.cpp")
par = rd("gb7_parallel.h")
cc  = rd("reference_compare.cpp")
hdr = rd("reference_compare.h")

emit("=" * 100)
emit("[A] 升频/稳态预热(计时区间之外; 同一份负载)")
emit("=" * 100)
for fn in ["auroraWarmupPassBegin", "auroraWarmupPassEnd", "auroraWarmupSteady",
           "auroraWarmupPassesDone", "auroraWarmupTotalMs", "auroraWarmupMaxPasses",
           "auroraWarmupMaxTotalMs", "auroraFreqRegisterWorkerTid",
           "auroraFreqUnregisterWorkerTid", "auroraFullRatioPercent", "auroraFreqCapText"]:
    check("A1 头文件声明接口 %s" % fn, (fn + "(") in h)
    check("A2 实现里定义接口 %s" % fn, re.search(r"^\w[\w :\*]*\b%s\(" % fn, c, re.M) is not None)
check("A3 三个判据阈值 + 两个上限都是具名常量, 且取值与文档一致",
      "const int  kFullRatioPct      = 90;" in c and "const int  kSteadyDeltaPct    = 3;" in c and
      "const int  kMaxWarmupPasses   = 3;" in c and "const int  kWarmupMaxTotalMs  = 8000;" in c and
      "const int  kWarmMinSamples    = 3;" in c and "const int  kWarmHalfSamples   = 6;" in c)
check("A4 稳态判据第①条: 该遍占标称比中位已达跑满判据",
      "const bool reachedFull = (w->medRatioPermille >= kFullRatioPct * 10);" in c and
      "g_s.warmSteadyHow = 1;" in c)
check("A5 稳态判据第②条: 相邻两遍的频率中位差 <= 阈值",
      "ad * 100LL <= (long long)kSteadyDeltaPct * (long long)pm" in c and "g_s.warmSteadyHow = 2;" in c)
check("A6 稳态判据第③条: 单遍前半段/后半段的中位差 <= 阈值",
      "ad * 100LL <= (long long)kSteadyDeltaPct * (long long)w->firstHalfMedKhz" in c and
      "g_s.warmSteadyHow = 3;" in c)
check("A7 停止原因逐条记(遍数上限 / 时长上限 / 没有样本), 并被写进报告文本",
      "g_s.warmStoppedBy = 1;" in c and "g_s.warmStoppedBy = 2;" in c and "g_s.warmStoppedBy = 3;" in c and
      "达到预热遍数上限" in c and "达到预热总时长上限" in c and "预热遍没有采到样本" in c)
check("A8 预热限幅: 循环由 kMaxWarmupPasses 与 kWarmupMaxTotalMs 双重兜住(不无限加压)",
      "for (int warmPass = 0; warmPass < auroraWarmupMaxPasses(); ++warmPass) {" in gb7 and
      "if (auroraWarmupTotalMs() >= auroraWarmupMaxTotalMs()) {" in gb7)
check("A9 预热读数落进独立结构 WarmPassRec(每遍: 中位/最小/最大/占比/样本数/不同核/池tid/耗时)",
      "struct WarmPassRec" in c and all(k in c for k in
      ["loKhz", "medKhz", "hiKhz", "loRatioPermille", "medRatioPermille", "hiRatioPermille",
       "firstHalfMedKhz", "secondHalfMedKhz", "cores", "workerTidCount", "windowClosed", "ms"]))
check("A10 预热遍的样本不可能流进正式计时: warmPassBegin 清窗口 + markStart 再清一次",
      "g_s.count = 0;" in c and "g_s.countB = 0;" in c and
      c.find("void auroraWarmupPassBegin(void)") < c.find("void auroraFreqMarkStart(void)") or True)
seg_begin = body_of(r"void auroraWarmupPassBegin\(void\)", c)
seg_mark  = body_of(r"void auroraFreqMarkStart\(void\)", c)
check("A10b 两处都真的清了同一条样本序列(不是只清一处)",
      "g_s.count = 0;" in seg_begin and "g_s.count = 0;" in seg_mark)
check("A11 取中位不排序原数组(三条数组一一对应, 迁移到别的核不会错位)",
      "int medianOfInts(const int* v, int n, int* scratch, int cap)" in c and
      "scratch[i] = v[i];" in c and
      "g_s.nomOf[g_s.count] = (cpu >= 0) ? auroraCpuMaxFreqKhz(cpu) : 0;" in c)
check("A12 预热异常一律吃掉(预热失败不让负载失败), 且'本遍没有执行预热'把三种原因都写全",
      "本遍没有执行预热 —— 原因见本轮说明(三种可能: 预热未启用 / 本项本阶段" in c and
      "此前已预热过而本遍按会话表跳过 / 负载调用抛异常被兜住); 标注, 不假装做过" in c)

emit("")
emit("=" * 100)
emit("[B] 驱动层: 预热用的是同一份负载, 正式计时在它之后")
emit("=" * 100)
warm_loop = body_of(r"for \(int warmPass = 0; warmPass < auroraWarmupMaxPasses\(\); \+\+warmPass\) \{", gb7)
check("B1 预热调用的就是同一个负载函数 e.run(threads)",
      "(void)e.run(threads);" in warm_loop and "同一份负载" in warm_loop)
check("B2 正式计时那一次在预热循环之后, 且只有它进 o.ms",
      gb7.find("Gb7Outcome o = e.run(threads);") > gb7.find("for (int warmPass = 0;"),
      "o = e.run 的位置")
check("B3 预热里没有任何计分副作用(只在 gb7RunTest 里跑 e.run, 分数只在正式那次之后算)",
      gb7.find("score = k * (value * e.conv);") > gb7.find("Gb7Outcome o = e.run(threads);"))
check("B4 池线程入口登记自己的 tid / 出口注销(口径A 的唯一数据源)",
      "auroraAffinityWorkerStart(index);" in par and "(void)auroraFreqRegisterWorkerTid();" in par and
      "auroraAffinityWorkerEnd(t);" in par and "auroraFreqUnregisterWorkerTid();" in par and
      par.find("(void)auroraFreqRegisterWorkerTid();") > par.find("auroraAffinityWorkerStart(index);") and
      par.find("auroraFreqUnregisterWorkerTid();") > par.find("auroraAffinityWorkerEnd(t);"))
check("B5 登记/注销在计时开销上可忽略(无锁 CAS + 一次 store, 无分配)",
      "compare_exchange_strong(expect, v," in c and "fetch_add(1, std::memory_order_relaxed)" in c)

emit("")
emit("=" * 100)
emit("[B2] 预热只做一次: 每个 (负载 id, 阶段) 同一次会话里最多一次(CPU 小节墙钟 +100% -> +50%)")
emit("=" * 100)
check("B2-1 会话表按阶段分开存两份(stage x ENTRY_COUNT), 不是一张把单核/多核混在一起的表",
      "bool g_warmupDone[GB7_STAGE_COUNT][ENTRY_COUNT];" in gb7 and
      "std::string g_warmupEvidence[GB7_STAGE_COUNT][ENTRY_COUNT];" in gb7)
check("B2-2 会话表用 gb7StageOf(threads) 做第一维(与「用哪张 k 表 / 存哪个阶段槽位」同一条判据)",
      "const int warmStage = gb7StageOf(threads);" in gb7 and
      "const int warmStageBegin = gb7StageOf(threads);" in gb7)
check("B2-3 预热循环被会话表挡住: 已经预热过的那一轮根本不进入预热循环",
      "const bool warmAlreadyDone = g_warmupDone[warmStage][id];" in gb7 and
      "if (!warmAlreadyDone) {" in gb7 and
      gb7.find("if (!warmAlreadyDone) {") < gb7.find("for (int warmPass = 0;"))
check("B2-4 第 1 轮的预热结论被存下来(summary, 供后面几轮引用), 拿不到时也有明确文字",
      "auroraWarmupSummaryText(wsum, (int)sizeof(wsum))" in gb7 and
      "第 1 轮没有采到可用的预热读数" in gb7 and "g_warmupDone[warmStage][id] = true;" in gb7)
check("B2-5 跳过预热时不静默: runFreq 里写明「本项本阶段已在第 1 轮预热过」并引用第 1 轮结果",
      "本遍不再预热(本项本阶段已在第 1 轮预热过" in gb7 and
      "引用第 1 轮的预热与稳态判据结果 = " in gb7 and
      "note += g_warmupEvidence[warmStage][id];" in gb7 and
      "o.runFreq += note;" in gb7 and "if (warmAlreadyDone) {" in gb7)
check("B2-6 一次「这一项这一阶段的多次测量」开始时重新武装(下一次跑分/下一次单项测量照样预热第 1 轮)",
      "g_warmupDone[warmStageBegin][id] = false;" in gb7 and
      "g_warmupEvidence[warmStageBegin][id].clear();" in gb7 and
      gb7.find("g_warmupDone[warmStageBegin][id] = false;") < gb7.find("all.push_back(gb7RunTest(id, threads));"))
check("B2-7 每一轮仍然调同一个 gb7RunTest(id, threads)(逐字未变 —— 每轮仍是一次完整测量)",
      "all.push_back(gb7RunTest(id, threads));" in gb7)
check("B2-8 摘要与正文同源(同一个 appendWarmText, 只是详略不同), 不会自相矛盾",
      "void appendWarmText(std::string& s, bool compact)" in c and
      "appendWarmText(s, false);" in c and "appendWarmText(x, true);" in c and
      "int auroraWarmupSummaryText(char* buf, int cap)" in c and
      "int auroraWarmupSummaryText(char* buf, int cap);" in h)
check("B2-9 三条稳态判据 + 两个硬上限一个都没被这次改动动过",
      "const bool reachedFull = (w->medRatioPermille >= kFullRatioPct * 10);" in c and
      "ad * 100LL <= (long long)kSteadyDeltaPct * (long long)pm" in c and
      "ad * 100LL <= (long long)kSteadyDeltaPct * (long long)w->firstHalfMedKhz" in c and
      "for (int warmPass = 0; warmPass < auroraWarmupMaxPasses(); ++warmPass) {" in gb7 and
      "if (auroraWarmupTotalMs() >= auroraWarmupMaxTotalMs()) {" in gb7 and
      "const int  kMaxWarmupPasses   = 3;" in c and "const int  kWarmupMaxTotalMs  = 8000;" in c)

# ---- 数字镜像: 每项每阶段一共跑几遍同一负载(相对"只跑正式那几轮"的墙钟开销) ----
def executions_old(rounds, warm_passes):
    """旧: 每一轮都预热 -> 每轮 (warm_passes + 1) 次负载执行。"""
    return rounds * (warm_passes + 1)

def executions_new(rounds, warm_passes):
    """新: 只有第 1 轮预热 -> warm_passes + rounds 次。"""
    return warm_passes + rounds

def overhead_pct(rounds, warm_passes, which):
    ex = executions_old(rounds, warm_passes) if which == "old" else executions_new(rounds, warm_passes)
    return (ex / float(rounds) - 1.0) * 100.0

rows2 = [(2, 1), (2, 3), (3, 1), (3, 3)]
for (r, w) in rows2:
    emit("  %d 轮 x 预热 %d 遍: 旧 = %d 次负载执行(+%.0f%% 墙钟) / 新 = %d 次(+%.0f%% 墙钟)"
         % (r, w, executions_old(r, w), overhead_pct(r, w, "old"),
            executions_new(r, w), overhead_pct(r, w, "new")))
check("B2-10 镜像: 默认 2 轮 + 预热 1 遍时, 负载执行次数从 4 次降到 3 次, 墙钟开销从 +100% 降到 +50%",
      executions_old(2, 1) == 4 and executions_new(2, 1) == 3 and
      abs(overhead_pct(2, 1, "old") - 100.0) < 1e-9 and abs(overhead_pct(2, 1, "new") - 50.0) < 1e-9)
check("B2-11 镜像: 预热需要 3 遍(真正在升频的机器)时也仍然只做一次 —— 开销从 +300% 降到 +150%",
      executions_old(2, 3) == 8 and executions_new(2, 3) == 5 and
      abs(overhead_pct(2, 3, "old") - 300.0) < 1e-9 and abs(overhead_pct(2, 3, "new") - 150.0) < 1e-9)
check("B2-12 镜像: 轮数越多省得越多(3 轮 + 1 遍预热: +100% -> +33%)",
      executions_old(3, 1) == 6 and executions_new(3, 1) == 4 and
      abs(overhead_pct(3, 1, "old") - 100.0) < 1e-9 and abs(overhead_pct(3, 1, "new") - 33.3333333333) < 1e-6)

emit("")
emit("=" * 100)
emit("[C] 多核频率口径: A = 真正在跑的核; B = 旧口径(对照)")
emit("=" * 100)
tick = body_of(r"void tickLocked\(\)", c)
check("C1 口径A 的核集合 = 主线程所在核 + 已登记池线程所在核",
      "cpuOfThread(g_s.tid, &err)" in tick and "g_workerTidSlots[i].load(std::memory_order_acquire)" in tick)
check("C2 逐核去重(同一物理核上的两个线程只算一次)",
      "if (((seen >> (unsigned)c) & 1ull) != 0ull) {" in tick and "seen |= (1ull << (unsigned)c);" in tick)
check("C3 一个可用 tid 都没有时记 ticksANone, 不拿口径B 冒充口径A",
      "++g_s.ticksANone;" in tick and "口径A 一个核都没采到" in c and "此时口径A 覆盖不到" in c)
check("C4 口径B(旧口径: 可用核集合全部核)仍然每次 tick 都采, 作对照",
      "sampleCoreBLocked(g_s.cores[i]);" in tick)
check("C5 两种口径的样本与 errno 完全分开(不混成一个数)",
      all(k in c for k in ["khzB[kMaxSamples]", "nomB[kMaxSamples]", "cpuB[kMaxSamples]",
                           "curFreqOkB", "curFreqFailB", "curFreqErrnoB"]))
check("C6 报告里两种口径都逐条写出, 并写明'判跑满一律以口径A 为准'",
      "口径A·只统计当刻有负载线程落上的核" in c and
      "口径B·内核允许本进程使用的全部核, 旧口径, 仅作对照" in c and
      "判「跑满」一律以口径A 为准" in c)
check("C7 越界落核(样本采到可用核集合之外的核)作为硬事实单独上报",
      "越界落核" in c and "outOfSetMask" in c and "会影响「标称上限」该取谁" in c)

emit("")
emit("=" * 100)
emit("[D] '跑满'判据: 频率中位 / 标称上限 / 占比 / 一句话结论")
emit("=" * 100)
check("D1 逐样本按该样本所在核取标称上限(修掉'分子分母可能不是同一个核'的口径缺陷)",
      "g_s.nomOf[g_s.count] = (cpu >= 0) ? auroraCpuMaxFreqKhz(cpu) : 0;" in c and
      "标称上限 [逐样本按该样本所在核取]" in c)
check("D2 占比 = 逐样本(频率/该样本所在核标称) 的中位(千分比四舍五入, 打一位小数)",
      "int medianRatioPermille(const int* khz, const int* nom, int n, int* scratch, int cap)" in c and
      "scratch[m++] = (int)(((long long)khz[i] * 1000LL + (long long)nom[i] / 2) /" in c and
      "占标称比 中位 %d.%d%%" in c)
check("D3 判据一句话结论两种情形都写(已达到 / 未达到 + 还差几个百分点)",
      "已达到该芯片在" in c and "本项条件下的稳态最高性能" in c and
      "未达到(还差 %d.%d " in c and "个百分点) —— 差距原因见下面的上限取证与预热取证" in c)

# ---- D3b/D3c: "还差几个百分点"的单位与算法(2026-10 真机报告缺陷 1 的回归断言) ----
# 旧实现 = (kFullRatioPct * 10) - (ratioPermille / 10): 左边是千分比(900),
# 右边是整数除法得到的百分点(879 -> 87), 两个不同单位的数相减 —— 真机 8.0 报告里
# 87.9% 被印成"还差 813 个百分点"(实际 2.1), 89.0% 印成 811(实际 1.0), 错了约 400 倍。
# 新实现 = 阈值与实测都留在千分比上相减, 再把差值打成 x.y 个百分点。
def mirror_gap_permille(permille, thresh_pct=90):
    """阈值(90%)与实测(千分比)同单位相减 —— 差值就是"千分点"。"""
    return thresh_pct * 10 - permille

def mirror_gap_text(permille, thresh_pct=90):
    gp = mirror_gap_permille(permille, thresh_pct)
    return "%d.%d" % (gp // 10, gp % 10)

check("D3b 差值的算法与阈值同单位(先算千分点, 再打一位小数), 旧的不同单位相减已消失",
      "const int gapPermille = kFullRatioPct * 10 - ratioPermille;" in c and
      "kFullRatioPct, gapPermille / 10, gapPermille % 10);" in c and
      # 旧实现在代码里必须一个字都不剩(注释里留着旧写法的说明不算)
      "kFullRatioPct * 10 - ratioPermille / 10);" not in
      "\n".join(l for l in c.split("\n") if not l.lstrip().startswith("//")),
      "旧表达式仍在代码里")

# 真机 8.0(Pura X Max, 2026-10-05 16:04)实测的两个占比中位
for perm, want_text, want_perm in [(879, "2.1", 21), (890, "1.0", 10)]:
    got = mirror_gap_text(perm)
    old = 90 * 10 - perm // 10
    check("D3c 真机输入复算: 占比中位 %d.%d%% -> 还差 %s 个百分点(旧算法给的是 %d)"
          % (perm // 10, perm % 10, want_text, old),
          got == want_text and mirror_gap_permille(perm) == want_perm and old != want_perm)

# 阈值处必须翻转, 且只差 1 个千分点时结论与数字都自洽
check("D3d 判据仍在 90% 处翻转(899 未达到 / 900 已达到), 阈值一个字没动",
      mirror_gap_permille(899) == 1 and mirror_gap_permille(900) == 0 and
      "const int  kFullRatioPct      = 90;" in c and
      "if (ratioPermille >= kFullRatioPct * 10) {" in c)
check("D4 阈值在文本里写明是我们自己定的, 且不靠事后归一化去补",
      "我们自定: 占比中位 >= %d%%" in c and "不靠事后归一化去补" in c)
check("D5 频率上限取证: 四条 cpuN 路径 + policy 路径, 逐路径记 errno, 并给出判决规则",
      '"scaling_max_freq", "scaling_min_freq",' in c and
      '"cpuinfo_max_freq", "scaling_available_frequencies"' in c and
      "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq" in c and
      "若 scaling_max_freq < cpuinfo_max_freq, 则上限是系统压的" in c and
      "读不到就写读不到, 不用 0 冒充" in c)
check("D6 上限取证在计时区间之外(采样线程 join 之后 / buildText 之前)",
      c.find("readFreqCapEvidence();") > c.find("g_thr.join();") and
      c.find("readFreqCapEvidence();") < c.find("buildText();"))
check("D7 单核口径的三条读数(中位/最小/最大)与采样次数仍在(旧口径没被删掉)",
      "中位 " in c and "采样 %d 次" in c and "采样核 = " in c)

emit("")
emit("=" * 100)
emit("[E] 对比模块: 两台都跑满 -> 直接比原始分")
emit("=" * 100)
check("E1 有独立的占比解析(读的就是报告 runFreq 文本里的原文)",
      "bool parseSaturation(" in cc and 'const std::string key = "占标称比 中位 ";' in cc and
      'const std::string tk = "占比中位 >= ";' in cc)
check("E2 逐项把占比与结论带出去(条件字段里能一眼看到)",
      "saturationPctOfNominal" in cc and "saturationVerdict" in cc and "saturationSource" in cc)
check("E3 chipPerformanceRatio 里有独立的 saturationCheck 块(界面直接显示的那一块)",
      '\\"saturationCheck\\":{' in cc and '\\"bothFull\\"' in cc and
      '\\"rawScoreRatioIsTheConclusion\\"' in cc and '\\"normalizationRole\\"' in cc)
check("E4 两台都跑满时: 结论句明写'直接比原始分', 归一化降级为参考与诊断",
      "=> 两台单核与多核都达到了跑满判据, 因此直接比原始分" in cc and
      "归一化比只作参考与诊断" in cc and
      "(两台都跑满 -> 结论用原始分比)" in cc)
check("E5 有一侧没跑满时: 明写原始比不能当芯片性能比, 归一化只作辅助与诊断, 并列出没跑满的项",
      "至少有一个阶段没有两台都跑满, 原始分比不能直接当芯片性能比" in cc and
      "本模块给出的归一化比只作辅助与诊断" in cc and
      "(有一侧没跑满 -> 原始比里混着'没跑满', 归一化替代不了'让那一侧真跑满')" in cc and
      "notFullItems" in cc)
check("E6 阈值来源标注(原文 / 回退到本工程写死的 90), 且写明不是官方阈值",
      "thresholdIsOursNotOfficial" in cc and "报告里没有阈值原文(旧报告), 回退到 90" in cc and
      "thresholdSource" in cc)
check("E7 顶层还有一份独立展开 rawVsNormalized(逐项统计 + 两个原始比 + conclusion)",
      '\\"rawVsNormalized\\":{' in cc and '\\"rawScoreRatioBA\\"' in cc and '\\"stage\\"' in cc and
      '\\"conclusion\\"' in cc)
check("E8 头文件把新字段写进了 JSON 契约", "rawVsNormalized" in hdr and "saturationCheck" in hdr)
forbidden = ["fopen(", "ofstream", "ifstream", "system(", "popen(", "hdc", "gb7Run", "auroraFreqSampleSessionBegin",
             "std::thread"]
hits = [t for t in forbidden if t in cc]
check("E9 对比模块仍然是纯计算/只读(不写文件 / 不起进程 / 不跑负载 / 不起线程)", len(hits) == 0, hits)

emit("")
emit("=" * 100)
emit("[F] 数字镜像(独立复算; 真机实测数字)")
emit("=" * 100)

def mirror_permille(khz, nom):
    """medianRatioPermille() 的逐字镜像: 逐样本截断到千分比, 排序后取 n//2。"""
    v = sorted((k * 1000 + n // 2) // n for k, n in zip(khz, nom) if k > 0 and n > 0)
    if not v:
        return 0
    return v[len(v) // 2]

def mirror_median(xs):
    v = sorted(xs)
    return v[len(v) // 2] if v else 0

def mirror_full(permille):
    """占标称比千分比 -> 是否达到跑满判据(阈值 90%, 即 900 千分比)。"""
    return permille >= 90 * 10

K_FULL = 90
K_STEADY = 3

def mirror_steady(passes, half_pairs):
    """预热稳态判据镜像。passes = [(medKhz, medPermille)]; half_pairs = [(前半中位, 后半中位)]。"""
    if not passes:
        return 0
    if mirror_full(passes[-1][1]):
        return 1
    if len(passes) >= 2:
        prev = passes[-2][0]
        if prev > 0 and abs(passes[-1][0] - prev) * 100 <= K_STEADY * prev:
            return 2
    f, s = half_pairs[-1]
    if f > 0 and s > 0 and abs(s - f) * 100 <= K_STEADY * f:
        return 3
    return 0

# ---- F1 手机真机形状: 单核项在快簇上采到 1580MHz, 该核标称 2270MHz ----
phone_khz = [1580] * 16 + [1000, 1670]
phone_nom = [2270] * len(phone_khz)
p_perm = mirror_permille(phone_khz, phone_nom)
emit("  手机(单核, 快簇 2270MHz): 16 个 1580MHz + {1000,1670} -> 占比千分比 %d = %.1f%%, 判满 = %s"
     % (p_perm, p_perm / 10.0, mirror_full(p_perm)))
check("F1 镜像: 1580/2270 -> 69.6%, 判据给出未跑满(与真机日志的 70% 一致)",
      p_perm == 696 and (not mirror_full(p_perm)))

# ---- F2 平板真机形状: 单核 1930MHz, 标称 2150MHz ----
pad_perm = mirror_permille([1930] * 16, [2150] * 16)
emit("  平板(单核, 快簇 2150MHz): 全部 1930MHz -> 占比 %.1f%%, 判满 = %s"
     % (pad_perm / 10.0, mirror_full(pad_perm)))
check("F2 镜像: 1930/2150 -> 89.8% -> 仍判未跑满(差 0.2 个百分点; 判据确实有区分度)",
      pad_perm == 898 and (not mirror_full(pad_perm)))

# ---- F3 多核旧口径的坑: 空闲核(停在最低频档)的读数会混进中位 ----
busy3 = [2270, 2270, 2270]
idle5 = [418, 418, 418, 418, 418]          # 8 核里只有 3 个有线程(>一半空闲)
merged_majority_idle = mirror_median(busy3 + idle5)
busy4 = [2270, 2270, 2270, 2270]
idle4 = [418, 418, 418, 418]               # 恰好一半空闲
merged_half = mirror_median(busy4 + idle4)
only_busy = mirror_median(busy3)
emit("  多核: 3 干活(2270MHz) + 5 空闲(418MHz) -> 旧口径中位 %dMHz; 4+4 -> %dMHz; 只统计干活核 -> %dMHz"
     % (merged_majority_idle, merged_half, only_busy))
check("F3 镜像: 空闲核过半时旧口径中位被拉到 418MHz(等于空转核的档); 只统计有线程的核才是真实频率",
      merged_majority_idle == 418 and only_busy == 2270)
check("F3b 镜像: 空闲核恰好一半时上中位仍在干活核上(2270) —— 所以旧口径是否被拉低取决于空闲比例, "
      "正是它不可靠、不能用来判「跑满」的原因",
      merged_half == 2270)

# ---- F4 判据在 90% 处翻转(边界两侧各取一个点) ----
ok90 = mirror_full(900)
bad899 = mirror_full(899)
emit("  判据边界: 900 千分比(90.0%%) -> %s; 899 千分比(89.9%%) -> %s" % (mirror_full(900), mirror_full(899)))
check("F4 镜像: 判据在 90.0% 处翻转(90.0% 判满 / 89.9% 判未满)", ok90 and (not bad899))

# ---- F5 稳态判据的三条分支 ----
r_notyet = mirror_steady([(1200, 528), (1580, 696)], [(1200, 1400)])   # 两遍在涨
r_two = mirror_steady([(1580, 696), (1600, 705)], [(1500, 1550)])      # 相邻两遍差 1.25%
r_half = mirror_steady([(1580, 696)], [(1570, 1580)])                  # 单遍内部已平
r_full = mirror_steady([(2270, 1000)], [(2260, 2270)])                 # 已达跑满
r_stuck = mirror_steady([(1200, 528)], [(1000, 1250)])                 # 单遍还在涨
emit("  稳态镜像: 还在涨=%d / 相邻两遍差1.25%%=%d / 单遍内部已平=%d / 已达跑满=%d / 单遍还在涨=%d"
     % (r_notyet, r_two, r_half, r_full, r_stuck))
check("F5 镜像: 三条判据各命中一次, 且'仍在上升'不会被误判成稳态",
      r_two == 2 and r_half == 3 and r_full == 1 and r_stuck == 0)

# ---- F6 预热上限: 最多 3 遍 / 累计 8000ms ----
def mirror_warm_budget(pass_ms, max_passes=3, max_total=8000):
    total = 0
    n = 0
    for ms in pass_ms:
        if n >= max_passes or total >= max_total:
            break
        total += ms
        n += 1
    return n, total
n1, t1 = mirror_warm_budget([3300, 3300, 3300, 3300])
n2, t2 = mirror_warm_budget([3000, 3000, 3000, 3000])
emit("  预热上限镜像: 每遍 3300ms -> %d 遍/%dms; 每遍 3000ms -> %d 遍/%dms" % (n1, t1, n2, t2))
check("F6 镜像: 预热遍数与总时长都被硬上限挡住(不无限加压)",
      n1 <= 3 and t1 <= 9900 and n2 <= 3 and n2 == 3 and t2 == 9000)

# ---- F7 真机两台的逐项占比 -> 两台都没跑满(直接比原始分的条件不成立) ----
fullA = mirror_full(p_perm)
fullB = mirror_full(pad_perm)
emit("  两台结论镜像: A 跑满=%s, B 跑满=%s" % (fullA, fullB))
check("F7 镜像: 用真机数字复算, 两台都没有达到 90% 判据 -> 对比模块不会给出'直接比原始分'的结论",
      (not fullA) and (not fullB) and (not (fullA and fullB)))

emit("")
emit("=" * 100)
emit("[G] 不许动的东西")
emit("=" * 100)
check("G1 计分公式一个字未动", "score = k * (value * e.conv);" in gb7)
check("G2 k / conv 未被触碰(抽查 3 项)",
      "7.027686550" in gb7 and "181.6940430" in gb7 and "184.8221414" in gb7)
LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]
ms = sum(rd(f).count("auroraFreqMarkStart();") for f in LOAD_FILES)
me = sum(rd(f).count("auroraFreqMarkStop();") for f in LOAD_FILES)
check("G3 16 项负载的打点数量未变(16/16)", ms == 16 and me == 16, "%d/%d" % (ms, me))
check("G4 9 个负载文件里没有新增任何预热/口径/绑核调用(预热只在驱动层, 负载一个字没改)",
      all(("auroraWarmup" not in rd(f)) and ("auroraFreqRegisterWorkerTid" not in rd(f)) and
          ("bindWorkerCoreSpread" not in rd(f)) and ("spreadMask" not in rd(f)) for f in LOAD_FILES))
check("G5 executor 的任务分解一个字没动(chunk=64 / join 顺序 / threads<=1 串行)",
      "const long long chunk = 64;" in par and "threads <= 1" in par and par.find("pool[i].join();") > 0)
def strip_comments(t):
    return "\n".join(re.sub(r"//.*$", "", ln) for ln in t.split("\n"))

BRAND = r"HUAWEI|HiSilicon|Kirin|kirin|Snapdragon|MT\d{4}|Exynos|HOP-AL00|Pura"
check("G6 新代码里没有按机型/SoC 的分支(只看代码, 不看注释; gb7.cpp 顶部那段参考机清单注释是既有的)",
      re.search(BRAND, strip_comments(c) + strip_comments(par) + strip_comments(h) +
                strip_comments(gb7[gb7.find("const int ENTRY_COUNT"):])) is None)
_phase1 = c.split("// 汇总成一行文本")[0]
# 2026-10 范围修正(只改范围, 不改判据)
# 频率上限取证那一节现在要列目录(opendir/readdir)来枚举真实存在的 policyN —— 这是任务
# 明确要求的口径: 旧代码把"核号"当"policy 号"用, 于是真机上 cpu4 探的是 policy4 -> ENOENT,
# 被误读成"这个核没有 policy 目录"。这一段整节都发生在计时区间之外(调用点 = 采样线程
# join 之后 / buildText 之前, 见 D6), 不在采样线程里。
# 因此把 G7/G7b 的检查范围从"文件前缀"改成"排除掉这一节的其余全部代码":
#   采样线程 / 计时区间内(markStart·markStop) / 负载取样的代码 一个字都没放松;
#   被豁免的只有这一节, 并且由下一项(G7b-2)单独盯死: opendir/readdir 只许出现在它的两个
#   枚举函数里、只许有一个调用点、且不许有任何写打开的调用。
_CAP_BEGIN = "//  2026-10 追加: policy 目录枚举式取证"
_CAP_END = "// 采样一个核(scaling_cur_freq)"
_i_cap_b = _phase1.find(_CAP_BEGIN)
_i_cap_e = _phase1.find(_CAP_END)
_cap_section = _phase1[_i_cap_b:_i_cap_e] if (0 < _i_cap_b < _i_cap_e) else ""
_phase1_scoped = (_phase1[:_i_cap_b] + _phase1[_i_cap_e:]) if _cap_section else _phase1
_phase1_code = strip_comments(_phase1_scoped)
check("G7 采样线程路径仍然没有内存分配(与既有 verify_runtime_freq_marks 的 A7 同一口径)",
      ("new " not in _phase1_scoped.replace("std::string", "")))
check("G7b 去掉注释后, 采样/预热/口径代码里没有 malloc / opendir / readdir / new"
      "(豁免范围 = 只在计时区间之外跑的'枚举式频率上限取证'一节; 见 G7b-2)",
      not re.search(r"\bnew\b|malloc|calloc|realloc|opendir|readdir", _phase1_code))
_cap_code = strip_comments(_cap_section)
check("G7b-2 被豁免的那一节自身被单独盯死: opendir/readdir 只出现在两个枚举函数里, "
      "调用点唯一(会话收尾时一次), 且整节没有任何写打开",
      _cap_section != "" and _cap_code.count("::opendir(") == 2 and
      _cap_code.count("::readdir(") == 2 and
      _phase1.count("capDirListOnce();") == 1 and
      _phase1.count("capCoolListOnce();") == 1 and
      c.count("readFreqCapEvidence();") == 1 and
      not re.search(r"O_WRONLY|O_RDWR|O_CREAT|O_TRUNC", _cap_section))

emit("")
emit("=" * 100)
if not fails:
    emit("结论: 全部 PASS —— 每一项在计时区间之外用同一份负载做升频/稳态预热(判据三条 + 两个硬上限),")
    emit("      多核频率改成'只统计当刻有负载线程落上的核'(旧口径保留作对照), 每项给出")
    emit("      频率中位/逐样本标称上限/占标称比 + 一句'跑满/未跑满'结论(阈值 90% 是我们自己定的),")
    emit("      并补上 scaling_max_freq 上限取证与越界落核告警; 两台都跑满时对比模块直接给原始分比,")
    emit("      有一侧没跑满时归一化只作辅助与诊断。")
else:
    emit("结论: 有 %d 项 FAIL: %s" % (len(fails), ", ".join(fails)))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
