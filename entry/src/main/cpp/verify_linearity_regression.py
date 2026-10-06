# -*- coding: utf-8 -*-
"""
线性自检:「分数比 == 性能比」(最高优先级验收标准)的离线回归脚本

为什么要有它
------------------------------------------------------------------------------
用户的要求是定量的: 二代单核 +15% / 多核 +30%, 那么一代 100/1000 分, 二代就必须
115/1300 分。这不是"差不多就行", 是线性尺度的要求。 本脚本把这件事拆成三块,
全部用数字核对(不是写"做到了"):

  [A] 公式层面的线性(可在单台设备上机器核对)
      score = k x (metric x conv), k 与 conv 是编译期常量 => score/metric 恒为常数
      => 任何两个吞吐之比严格等于两个分数之比。
      这一条用"注入式回归"验证: 构造两组吞吐, 检查分数比 == 吞吐比(误差 0)。
  [B] 复合分的口径与线性
      复合分 = 所有计分项的几何平均; GM(c x a_i) = c x GM(a_i) —— 所有项同时乘 c
      时复合分比严格等于 c; 不同项倍数不一致时, 复合分比不代表任何单一"性能比",
      必须看逐项倍数离散度。官方 GB7 的复合分是几何平均(本工程 gb7.cpp 同口径)。
  [C] 公开真值锚点(第三方查证)
      GB6 单核 9000s 1299 -> 9020 1616 -> 9030 Pro 约 1866;
      3DMark SNL 991 / 454 / 303; 3DMark GPU 三代累计约 2.64x。
      脚本把真值比值算出来, 并核对它们自身的自洽性(第三方近似值恰好线性这件事
      必须标注出来, 不能当成"真值线性"的证据)。
  [D] 破坏线性的因素: 可用核集合 / 频率档不同
      真机事实: 手机只能用 cpu0-7(8 逻辑核), 硬件 9 物理核 / 14 线程; 平板 12 逻辑核
      只能用 7 个。用数字示例证明:
        * 直接比总吞吐 => 比值被核数差污染(算出来是 1.2375, 而真实每核性能比是 1.10, 虚高 12.5%);
        * 用每核吞吐相除 => 精确恢复 1.10;
        * 频率档不同时, 直接比被频差污染(1.135), 用同频归一化 => 精确恢复 1.00。
      并给出可执行方案的检查项(报告里必须真的给出这些字段)。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io
import json
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
SN = os.path.join(HERE, "sn")
fails = 0
checks = 0
_OUT = io.open(os.path.join(HERE, "verify_linearity_out.txt"), "w", encoding="utf-8", newline="\n")


def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    _OUT.write(s + "\n")
    _OUT.flush()


def check(name, cond, detail=""):
    global fails, checks
    checks += 1
    if cond:
        emit("  [PASS] " + name + ((" -- " + detail) if detail else ""))
    else:
        emit("  [FAIL] " + name + ((" -- " + detail) if detail else ""))
        fails += 1


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


# ===========================================================================
#  真值锚点(与 reference_compare.cpp 的 kTruthAnchors 保持一致)
# ===========================================================================
GB6_SINGLE = {"9000s": 1299.0, "9020": 1616.0, "9030pro": 1866.0}
SNL = {"9030pro": 991.0, "9020": 454.0, "9000s": 303.0}
GB7_9030PRO = {"single": 1633.0, "multi": 6802.0}
K3DMARK = 135.0
RES_RATIO = (1920.0 * 1080.0) / (2560.0 * 1440.0)
K_SCORE_PER_FPS = K3DMARK * RES_RATIO      # 75.9375

# 与代码里一致的示例参数(仅用于回归演算, 不是设备实测值)
DEMO_K = 7.027686550          # File Compression 的 kSingle
DEMO_CONV = 1.0


def score_of(metric, k=DEMO_K, conv=DEMO_CONV):
    """与 gb7.cpp / sn_renderer.cpp 完全相同的单项分公式"""
    return k * (metric * conv)


def main():
    sn_cpp = os.path.join(SN, "sn_renderer.cpp")
    ref_cpp = os.path.join(HERE, "reference_compare.cpp")
    gb7_cpp = os.path.join(HERE, "gb7.cpp")
    for p in (sn_cpp, ref_cpp, gb7_cpp):
        if not os.path.isfile(p):
            emit("找不到文件: " + p)
            return 1
    sn = read(sn_cpp)
    ref = read(ref_cpp)
    gb7 = read(gb7_cpp)

    emit("=" * 104)
    emit("[A] 公式层面的线性(单台设备即可核对): score = k x (metric x conv)")
    emit("=" * 104)
    check("A1 单项分公式是 k x (metric x conv)(只有一个变量)",
          re.search(r"score\s*=\s*k\s*\*\s*\(value\s*\*\s*e\.conv\)", gb7) is not None,
          "gb7.cpp: score = k * (value * e.conv)")
    check("A2 GPU-SNL 的分数公式是 fps x 常量(只有一个变量)",
          re.search(r"s\.score\s*=\s*s\.fps\s*\*\s*kScorePerFps", sn) is not None,
          "sn_renderer.cpp: s.score = s.fps * kScorePerFps")
    check("A3 换算是编译期常量(不在运行期按设备改)",
          re.search(r"kScorePerFps\s*=\s*k3dmarkNomadScale\s*\*\s*kWorkloadScale\s*\*\s*kResolutionScaleRatio", sn)
          is not None and "kWorkloadScale = 1.0" in sn,
          "kScorePerFps = 135 x 1.0 x 0.5625 = 75.9375(编译期常量)")

    # 注入式回归: 两组吞吐 -> 两个分数, 比值必须完全相等
    emit("")
    emit("  ---- 注入式回归(构造性验证: 分数比 == 吞吐比)----")
    emit("      场景                        吞吐A      吞吐B   吞吐比     分数A       分数B   分数比    误差")
    cases = [
        ("单核 +15%(用户举的例子)", 100.0, 115.0),
        ("单核 +30%", 100.0, 130.0),
        ("多核 +30%", 1000.0, 1300.0),
        ("跨代 x1.2440(GB6 9020/9000s)", 1299.0, 1616.0),
        ("跨代 x1.4365(GB6 9030Pro/9000s)", 1299.0, 1866.0),
        ("降低 20%", 100.0, 80.0),
        ("极大值(未来芯片 x8)", 100.0, 800.0),
    ]
    worst = 0.0
    for (label, a, b) in cases:
        sa, sb = score_of(a), score_of(b)
        ratio_t, ratio_s = b / a, sb / sa
        err = abs(ratio_s - ratio_t)
        worst = max(worst, err)
        emit("      %-34s %8.1f %8.1f %8.4f %9.1f %11.1f %8.4f %.2e"
             % (label, a, b, ratio_t, sa, sb, ratio_s, err))
    check("A4 所有场景下 分数比 == 吞吐比(误差 < 1e-12)", worst < 1e-12,
          "最大误差 %.2e" % worst)
    check("A5 分数与吞吐是严格正比(100 -> 115 分就是 100 -> 115 的那条线)",
          abs(score_of(115.0) / score_of(100.0) - 1.15) < 1e-12,
          "score(115)/score(100) = %.15f" % (score_of(115.0) / score_of(100.0)))

    emit("")
    emit("=" * 104)
    emit("[B] 复合分: 几何平均的口径与线性")
    emit("=" * 104)
    check("B1 复合分是几何平均(只统计计分项)",
          "几何平均" in gb7 and re.search(r"prod\s*\*=\s*s;", gb7) is not None and
          re.search(r"std::pow\(prod,\s*1\.0\s*/\s*\(double\)n\)", gb7) is not None,
          "gb7.cpp compositeJson(): GM = (Π score_i)^(1/n)")
    # 2026-10-08 更新: 排除逻辑重写成"逐条写进 excluded[] 再 continue"(口径没变,
    #   仍然是 scored + (多核时)inMulti + 分数 > 0 三条), 因此断言改成核对这三条规则本身,
    #   并要求排除的项逐条列出来(报告里可以两台机器逐条对照)。
    check("B2 未计分项被排除在复合分之外(且三条排除规则逐条可核对)",
          re.search(r"if\s*\(!ENTRIES\[i\]\.scored\)", gb7) is not None and
          "skipWhy = \"scored=false" in gb7 and
          "inMulti=false" in gb7 and
          "本次没有拿到正的分数" in gb7 and
          'out += "],\\"excluded\\":[\";' in gb7,
          "compositeJson: !scored / !inMulti / 分数 <= 0 三条 -> 记入 excluded[] 并 continue")
    check("B3 未计分项的口径写明(宁可未计分也不混进复合分)",
          "宁可未计分" in ref or "宁可未计分" in gb7,
          "basis/注释里写了这条口径")

    # 几何平均的线性: 所有项 x c
    base = [100.0, 250.0, 40.0, 900.0, 12.0]
    gm = lambda xs: (lambda pr: pr ** (1.0 / len(xs)))(__import__("functools").reduce(lambda x, y: x * y, xs, 1.0))
    gm_a = gm(base)
    for c in (1.15, 1.30, 2.0, 0.8):
        gm_b = gm([v * c for v in base])
        if abs(gm_b / gm_a - c) > 1e-12:
            check("B4 几何平均线性(c=%.2f)" % c, False, "%.15f != %.15f" % (gm_b / gm_a, c))
            break
    else:
        check("B4 几何平均对所有项同时乘 c 时严格线性(GM(c*a)/GM(a) == c)",
              True, "c ∈ {1.15, 1.30, 2.00, 0.80} 全部精确")
    # 不同项倍数不一致时, 复合分比不代表单一性能比 -> 必须报离散度
    mixed = [v * m for v, m in zip(base, [1.4, 0.9, 1.6, 1.0, 0.7])]
    gm_mixed = gm(mixed)
    mults = [1.4, 0.9, 1.6, 1.0, 0.7]
    spread = (max(mults) - min(mults)) / (sum(mults) / len(mults)) * 100.0
    check("B5 逐项倍数不一致时, 复合分比偏离任何单一性能比(必须报离散度)",
          abs(gm_mixed / gm_a - 1.0) > 0.02 and spread > 50.0,
          "GM 比 = %.4f, 逐项倍数离散度 = %.1f%%" % (gm_mixed / gm_a, spread))
    check("B6 报告里真的会给出逐项倍数离散度字段",
          "itemScalingSpreadPercent" in ref,
          "linearityAudit 里预留了 itemScalingSpreadPercent(两台设备时可填)")

    emit("")
    emit("=" * 104)
    emit("[C] 公开真值锚点与它们的自洽性")
    emit("=" * 104)
    r_90_20 = GB6_SINGLE["9020"] / GB6_SINGLE["9000s"]
    r_20_30 = GB6_SINGLE["9030pro"] / GB6_SINGLE["9020"]
    r_90_30 = GB6_SINGLE["9030pro"] / GB6_SINGLE["9000s"]
    emit("      GB6 单核:  9000s %.0f -> 9020 %.0f -> 9030 Pro %.0f"
         % (GB6_SINGLE["9000s"], GB6_SINGLE["9020"], GB6_SINGLE["9030pro"]))
    emit("      步进比值: 9020/9000s = %.4f, 9030Pro/9020 = %.4f, 累计 = %.4f"
         % (r_90_20, r_20_30, r_90_30))
    emit("      自洽: %.4f x %.4f = %.4f (与累计 %.4f 的差 %.2e)"
         % (r_90_20, r_20_30, r_90_20 * r_20_30, r_90_30, abs(r_90_20 * r_20_30 - r_90_30)))
    check("C1 GB6 三档真值彼此自洽(步进相乘 = 累计, 误差 < 1e-3)",
          abs(r_90_20 * r_20_30 - r_90_30) < 1e-3,
          "%.4f x %.4f = %.4f" % (r_90_20, r_20_30, r_90_20 * r_20_30))
    check("C2 这三档恰好严格线性 —— 但必须标注是第三方近似值, 不是真值线性的证据",
          "第三方给的近似值" in ref and "truthAnchorsAreApproximate" in ref,
          "reference_compare.cpp 里 truthAnchorsAreApproximate=true + caveat")
    snl_ratio = SNL["9030pro"] / SNL["9000s"]
    check("C3 3DMark SNL 累计比 = 3.27x 与已知值一致",
          abs(snl_ratio - 3.27) < 0.01, "991/303 = %.4f" % snl_ratio)
    #  2026-08-31 变更 : 991 的来源已从"未证实"改成"用户拍屏的一手证据"
    #   (USER_PROVIDED_PRIMARY_OBSERVATION); 303 仍然只有第三方。这条断言随之更新 ——
    #   判据从"两端都未证实"改成"两端强度分开标注"。
    check("C4 SNL 比值的两端强度标注(991 = 一手证据 / 303 仍未证实; 不把未证实值当标尺)",
          "UNVERIFIED" in ref and "USER_PROVIDED_PRIMARY_OBSERVATION" in ref and
          "只有一端有一手证据" in ref,
          "kTruthAnchors 的 sourceKind = MIXED(...) + caveat")
    check("C5 3DMark GPU 三代累计 2.64x 也作为锚点收录",
          "2.64" in ref, "kTruthAnchors: 3DMark GPU 三代累计 2.64x")
    check("C6 官方 135 与分辨率归一化方向正确(与真值闭环)",
          abs((SNL["9030pro"] / K3DMARK / RES_RATIO) * K_SCORE_PER_FPS - SNL["9030pro"]) < 1e-6,
          "991 -> 7.34fps(1440p) -> 13.05fps(1080p) -> 991 分")

    emit("")
    emit("=" * 104)
    emit("[D] 破坏线性的因素: 可用核集合 / 频率档不同(用数字示例证明)")
    emit("=" * 104)
    # 场景: 芯片 B 的每核性能比 A 高 10%; 但 A 只能用 8 核, B 能用 9 核
    per_a, per_b = 100.0, 110.0          # 每核吞吐(代表每核性能)
    cores_a, cores_b = 8, 9              # 内核允许使用的核数
    total_a, total_b = per_a * cores_a, per_b * cores_b
    naive = total_b / total_a
    honest = per_b / per_a
    emit("      芯片 A: 每核 %.0f x %d 核 = 总吞吐 %.0f   (硬件 9 核, 只能用 8)"
         % (per_a, cores_a, total_a))
    emit("      芯片 B: 每核 %.0f x %d 核 = 总吞吐 %.0f   (硬件 9 核, 能用 9)"
         % (per_b, cores_b, total_b))
    emit("      真实每核性能比 = %.4f;  直接比总吞吐 = %.4f  <-- 被核数差污染"
         % (honest, naive))
    emit("      每核归一化后 = %.4f  <-- 精确恢复真实性能比" % (total_b / cores_b / (total_a / cores_a)))
    # 990/800 = 1.2375; 真实每核性能比 1.10; 核数差把比值虚高 12.5%
    check("D1 直接比总吞吐会被核数差污染(算出来 1.2375, 真实性能比 1.10, 虚高 12.5%)",
          abs(naive - 1.2375) < 1e-9 and abs(honest - 1.10) < 1e-9 and abs(naive - honest) > 0.10,
          "总吞吐比 %.4f vs 每核性能比 %.4f(偏差 %.1f%%)"
          % (naive, honest, (naive / honest - 1) * 100))
    check("D2 用'每核吞吐'相除精确恢复芯片性能比(误差 0)",
          abs((total_b / cores_b) / (total_a / cores_a) - honest) < 1e-12,
          "(%.4f/%.4f) = %.4f" % (total_b / cores_b, total_a / cores_a, (total_b / cores_b) / (total_a / cores_a)))
    # 频率档场景: 每频性能相同, 但 A 跑 2.0GHz, B 跑 2.27GHz
    freq_a, freq_b = 2000000.0, 2270000.0
    ipc_a, ipc_b = 50.0, 50.0            # 每 MHz 每核吞吐(相同 => 同架构同代)
    ta, tb = ipc_a * freq_a / 1000.0, ipc_b * freq_b / 1000.0
    naive_f = tb / ta
    norm_a = ta / freq_a * freq_b        # 折算到 B 的频率
    norm_b = tb / freq_b * freq_b
    emit("")
    emit("      同代同架构, 只是频率档不同: A 跑 %.0f MHz, B 跑 %.0f MHz" % (freq_a, freq_b))
    emit("      直接比吞吐 = %.4f(看起来'性能提升' %.1f%%, 其实只是频率高)"
         % (naive_f, (naive_f - 1) * 100))
    emit("      同频归一化(A 折算到 %.0f MHz) = %.4f/%.4f = %.4f  <-- 真实性能比 1.0"
         % (freq_b, norm_a, norm_b, norm_b / norm_a))
    check("D3 直接比吞吐会把'频率档更高'误当成'性能更高'(1.135 vs 真实 1.0)",
          abs(naive_f - 1.135) < 1e-3 and abs(naive_f - 1.0) > 0.1,
          "直接比 %.4f vs 同频归一化 %.4f" % (naive_f, norm_b / norm_a))
    check("D4 同频归一化(吞吐 / 频率中位 x 标称最高频)精确恢复 1.0",
          abs(norm_b / norm_a - 1.0) < 1e-9,
          "归一化后比值 = %.6f" % (norm_b / norm_a))
    check("D5 报告里真的给出这两个归一化量与条件字段",
          "perCoreThroughput" in ref and "throughputAtTopKhz" in ref and
          "allowedCores" in ref and "coresActuallyUsed" in ref and
          "runtimeKhzMedian" in ref,
          "comparabilityAudit: perCoreThroughput / throughputAtTopKhz / conditions{...}")
    check("D6 报告里给出条件化可比性判断(与谁可比 / 与谁不可比)",
          "comparableWith" in ref and "notComparableWith" in ref and "conditionText" in ref,
          "comparabilityAudit.verdict.{comparableWith,notComparableWith,conditionText}")
    check("D7 口径写明'不写 sysfs 改频率', 同频对标只用只读归一化",
          "不会**去写 sysfs" in ref or "doesNotDo" in ref,
          "comparabilityAudit.doesNotDo")
    check("D8 全部读数拿不到时给 null 而不是编数",
          "null" in ref and "不编一个数" in ref,
          "ourRatio:null + ourRatioWhy")

    emit("")
    emit("=" * 104)
    emit("共 %d 项断言, 失败 %d 项 -> %s" % (checks, fails, "PASS" if fails == 0 else "FAIL"))
    emit("=" * 104)
    emit("")
    emit("结论(数字, 不是口号):")
    emit("  * 单项分与吞吐严格成正比: 上面 7 个场景的比值误差 < 1e-12(构造性保证)。")
    emit("  * 复合分(几何平均)在所有计分项同时乘 c 时严格线性; 倍数不一致时必须看离散度。")
    emit("  * 核数限制会把总吞吐比污染成 1.2375(真实每核性能比 1.10, 虚高 12.5%); 每核归一化后精确恢复 1.10。")
    emit("  * 频率档差异会把吞吐比污染成 1.135(真实 1.0); 同频归一化后精确恢复。")
    emit("  => 跨设备要拿到'精准的提升比例', 必须用 perCoreThroughput x throughputAtTopKhz 的组合。")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
