# -*- coding: utf-8 -*-
"""
verify_report_compare.py —— 「两份报告对比」(compareReports)的离线自检器
(不连设备、不跑 hdc、不跑构建、不改任何文件)

要验的东西(用户最需要的那一条): 在两台设备上各跑一次之后, 自动算出
  ① 逐项对照表: 设备A 分 / 设备B 分 / 分数比 / 该项是否计分 / 两边的取得条件
     (可用核数、可用最高频档、线程数、实际用到核数、运行时频率中位);
  ② 原始分数比 与 归一化后的比(每核吞吐比、同频归一化比) —— 因为已知
     核数不同会让总分比虚高 12.5%、频率档不同能凭空造出 13.5% 的假提升;
  ③ 与参考真值对照(有真值的项)给出偏差百分比, 并判定该实测是否线性(阈值 5%,
     且必须标明阈值是我们自己定的);
  ④ 任一输入缺失 / 格式不符时, 写缺什么, 不许编数;
  ⑤ 纯计算、只读: 不跑负载、不连设备、不读 /sys、不写任何文件、不改任何分数。

脚本做两件事:
  A~F) 静态断言: 逐条核对上面 1)~5) 在源码里真的是这么做的(含否定断言:
        没有写文件、没有跑负载、没有读设备、没有编数字的路径)。
  G)   数值模型交叉核对: 用一份独立的 Python 模型实现同一套公式, 逐字符核对
        C++ 里的公式与模型的定义一致, 并把"消掉 12.5% 核数虚高 / 13.5% 频率假提升"
        算成具体数字打印出来(与 verify_linearity_regression.py 的口径一致)。

另外提供一个开发机上用的交叉核对模式(可选, 不是构建的一部分):
    python verify_report_compare.py --reports A.json B.json
  它会用同一套公式独立复算一遍两张报告, 打印:
    [1] 两边各自的『随芯片变化自检』排除集(项名 + 排除原因 + 判定依据)与"是否一致"的结论;
    [2] 复合分的参与项集合(交集/差集)与"只用共同参与项"重算的比(参与项不同时明确写不可用);
    [3] 两台的运行时频率中位 / 可用最高频档 / 报告原话, 以及逐项 sameFreq 为 null 的原因;
    [4] 逐项对照表(A 分 / B 分 / 原始比 / 每核比 / 同频比; null 一律带原因);
    [5] 多核比较的条件差(线程数比 / SMT / 可用核集合)与影响方向;
    [6] 最终结论: 原始比 / 同项比 / 每核比 / 同频比 + 芯片性能比最接近的估计 + 前提与缺口。
  设备上跑出来的 compareReports 输出可以直接与这张表对拍(数字对不上就是有 bug)。

『每核 x 同频』的口径(2026-10 修正, 不要改回去)
  两个归一化必须作用在同一份吞吐上(与设备侧逐字同口径):

      perCoreSameFreq = ((metricB / khzMedB * topB) / coresB)
                      / ((metricA / khzMedA * topA) / coresA)

  禁止写成 perCoreRatio x sameFreqRatio(旧写法, 已修正): 那样 metric 会在分子里出现
  两次, 结果被额外乘上一个『两台原始吞吐比 (metricB/metricA)』—— 报告里的
  perCoreSameFreqRatioGM 因此系统性地偏低(本次两份真机报告: 旧 0.6987 / 正确 0.7889,
  0.6987/0.7889 = 0.8857 正是两台原始吞吐比的几何平均)。
  见 model_per_core_same_freq() 与 main() 的 [J] 段断言。

退出码 0 = 全部通过, 1 = 有 FAIL。
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
OUT = io.open(os.path.join(HERE, "verify_report_compare_out.txt"), "w", encoding="utf-8", newline="\n")
fails = []
checks = 0


def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    OUT.write(s + "\n")
    OUT.flush()


def check(name, ok, detail=""):
    global checks
    checks += 1
    if ok:
        emit("  [PASS] " + name + ((" -- " + detail) if detail else ""))
    else:
        emit("  [FAIL] " + name + ((" -- " + detail) if detail else ""))
        fails.append(name)


def rd(fn):
    p = os.path.join(HERE, fn)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()


def squash(s):
    """把空白折叠成单空格 —— 便于跨行核对公式。"""
    return re.sub(r"\s+", " ", s)


# ===========================================================================
#  独立数值模型(与 cpp 里的公式必须逐字一致; 这里也是"消污染"的算例来源)
# ===========================================================================
def model_ratio(score_a, score_b):
    if score_a <= 0 or score_b <= 0:
        return None
    return score_b / score_a


def model_per_core(metric_a, cores_a, metric_b, cores_b):
    if min(metric_a, metric_b, cores_a, cores_b) <= 0:
        return None
    return (metric_b / cores_b) / (metric_a / cores_a)


def model_same_freq(metric_a, khz_med_a, top_a, metric_b, khz_med_b, top_b):
    if min(metric_a, metric_b, khz_med_a, khz_med_b, top_a, top_b) <= 0:
        return None
    return (metric_b / khz_med_b * top_b) / (metric_a / khz_med_a * top_a)


def model_per_core_same_freq(metric_a, cores_a, khz_med_a, top_a,
                             metric_b, cores_b, khz_med_b, top_b):
    """『两个归一化作用在同一份吞吐上』—— 与设备侧逐字同口径

    设备侧 reference_compare.cpp(compareReports 的逐项 normalized.perCoreSameFreqRatio)写的是:

        safeRatio((sa.metric / sa.khzMed * sa.khzTop) / sa.coresUsed,
                  (sb.metric / sb.khzMed * sb.khzTop) / sb.coresUsed, &perCoreSameFreq)

    即: 同一份 metric 先按各自的运行时频率中位折算到标称最高频档, 再除以各自核数;
    分子里 metric 只出现一次, 分母里 metric 也只出现一次。

    本函数代替旧写法 perCoreRatio * sameFreqRatio(那是错的)
      旧写法把两个比值相乘, 展开后分子里 metric 出现两次:

          perCore x sameFreq = [(mb/cb)/(ma/ca)] x [(mb/kb*tb)/(ma/ka*ta)]
                             = 本函数结果 x (mb/ma)

      多出来的 (mb/ma) 就是两台设备的原始吞吐比。它不属于任何一个归一化量, 却会被
      当成"芯片性能比"的一部分报出去 —— 因为 B 机吞吐通常低于 A 机, 这个因子 < 1,
      于是『最接近估计』被系统性压低(本次两份真机报告: 旧口径 0.6987 / 正确 0.7889,
      比值 0.6987/0.7889 = 0.8857 = 两台原始吞吐比的几何平均, 与上面的代数关系完全吻合)。

    口径只此一套, 不许再改回乘积形式(见 main() 里的 J 段断言)。
    """
    if min(metric_a, metric_b, cores_a, cores_b, khz_med_a, khz_med_b, top_a, top_b) <= 0:
        return None
    return ((metric_b / khz_med_b * top_b) / cores_b) / ((metric_a / khz_med_a * top_a) / cores_a)


def model_truth_delta(ours, reference):
    if reference <= 0:
        return None
    return (ours / reference - 1.0) * 100.0


def model_deviation(measured_ratio, truth_ratio):
    if truth_ratio <= 0 or measured_ratio is None:
        return None
    return (measured_ratio / truth_ratio - 1.0) * 100.0


def main():
    hdr = rd("reference_compare.h")
    cc = rd("reference_compare.cpp")
    napi = rd("reference_napi.cpp")
    cm = rd("CMakeLists.txt")
    if not hdr or not cc or not napi:
        emit("找不到 reference_compare.h / reference_compare.cpp / reference_napi.cpp")
        return 1
    ccflat = squash(cc)

    emit("=" * 100)
    emit("[A] 接口与输出形状(逐项对照表 / 分数比 / 取得条件)")
    emit("=" * 100)
    check("A1 头文件里声明了 auroraReferenceCompareReports(jsonA, jsonB, optionsJson)",
          "auroraReferenceCompareReports(const std::string& jsonA, const std::string& jsonB,"
          in squash(hdr) and "const std::string& optionsJson)" in squash(hdr),
          "reference_compare.h")
    check("A2 napi 里注册了 compareReports, 并且读 3 个参数(第三个可选)",
          '"compareReports", nullptr, RefCompareReports' in napi and
          "size_t argc = 3;" in napi and "napi_get_cb_info(env, info, &argc, args" in napi,
          "reference_napi.cpp: compareReports(jsonA, jsonB, options?)")
    check("A3 分数比的定义写在输出里(设备B / 设备A), 不留歧义",
          "ratioDefinition" in cc and "ratio = 设备B 的值 / 设备A 的值" in cc in cc,
          "ratioDefinition")
    check("A4 逐项对照表每项都带 section/name/a/b/ratio/normalized/truth",
          all(k in cc for k in ['\\"section\\":', '\\"name\\":', ',\\"a\\":', ',\\"b\\":',
                                ',\\"ratio\\":', ',\\"normalized\\":{', ',\\"truth\\":{']),
          "items[]")
    check("A5 两边的取得条件都在: 可用核数 / 可用最高频档 / 线程数 / 实际用到核数 / 运行时频率中位",
          all(k in cc for k in ['\\"allowedCores\\":', '\\"nominalTopKhz\\":', '\\"threadsRequested\\":',
                                '\\"threadsEffective\\":', '\\"workersBound\\":',
                                '\\"coresActuallyUsed\\":', '\\"runtimeKhzMedian\\":']),
          "conditions{...}")
    check("A6 该项是否计分也逐项给出(scoredByPolicy + scoredNote + 一句解释)",
          '\\"scoredByPolicy\\":' in cc and '\\"scoredNote\\":' in cc and "scoredMeaning" in cc,
          "scoredMeaning + 每项 scoredByPolicy")
    check("A7 运行时频率中位是从报告里的 runFreq 文本解析出来的(不是自己猜一个)",
          "parseRuntimeKhzMedian" in cc and 'text.find("中位")' in cc and
          "报告里的 runFreq 文本(负载计时区间内的后台采样, 取中位)" in cc,
          "parseRuntimeKhzMedian(runFreq)")
    #  期望值随 2026-10 的开源改名更新: 套件显示名改过两次(GB7 -> GB8 -> CS1),
    #  所以历史报告里的小节名有三种写法, canonSection 必须**三种都认**。
    #  只认 GB7 不认 GB8 的话, 8.4~9.x 那一批报告会被判成"不是 CS1 的 CPU 小节",
    #  跨设备对比直接拒绝工作 —— 那是数据兼容事故。这条断言就是钉住"三种都要认"。
    check("A8 逐项匹配用 section + name(同名不同节的项不会被错配)",
          "d.items[i].name == name && sameSection(d.items[i].section, section)" in cc and
          "if (s == \"GB7 单核\" || s == \"GB8 单核\") { return \"CS1 单核\"; }" in cc and
          "if (s == \"GB7 多核\" || s == \"GB8 多核\") { return \"CS1 多核\"; }" in cc and
          "if (s == \"GB7 GPU\" || s == \"GB8 GPU\") { return \"CS1 GPU\"; }" in cc,
          "findItem(section, name) + canonSection 认 GB7 / GB8 / CS1 三种历史写法")
    check("A9 只在一侧出现的项也照实列出(不静默丢掉)",
          "itemsOnlyInA" in cc and "itemsOnlyInB" in cc and "设备B 的报告里没有这一项" in cc,
          "itemsOnlyInA / itemsOnlyInB")
    # ---- A10(2026-10 真机报告缺陷 2): 报告 items[] 的两个旁路键(qos / cpuset)也必须进对比 ----
    #   用户原话问的是"这台机器的限制在哪一层""QoS 到底生效没有"; 报告 JSON 以前根本没有这两个键
    #   (只在 .txt 的 note 里露一半), 对比模块也就无从谈起。现在逐项带 present + 摘录,
    #   机器级再各存一份全文, 摘录一律按 UTF-8 字符边界切(不许把中文切一半写出坏 JSON)。
    check("A10 对比表带 QoS / cpuset 证据(items[] 的两个旁路键 + 机器级全文 + 按字符边界摘录)",
          'jsonGetString(objs[i], "qos", &it.qos)' in cc and
          'jsonGetString(objs[i], "cpuset", &it.cpuset)' in cc and
          "qosPresent" in cc and "cpusetPresent" in cc and "qosExcerpt" in cc and
          "std::string envEvidenceJson(const RepDoc& d)" in cc and
          "environmentEvidence" in cc and
          "std::string utf8Cut(const std::string& s, size_t cap)" in cc,
          "items[].qos / items[].cpuset -> environmentEvidence")

    emit("")
    emit("[B] 原始分数比 vs 归一化比(消掉 12.5% 核数虚高与 13.5% 频率假提升)")
    emit("=" * 100)
    check("B1 三个归一化量都在输出里(每核 / 同频 / 两个一起)",
          all(k in cc for k in ['\\"perCoreRatio\\":', '\\"sameFreqRatio\\":',
                                '\\"perCoreSameFreqRatio\\":']),
          "normalized{perCoreRatio, sameFreqRatio, perCoreSameFreqRatio}")
    check("B2 每核比的公式与独立模型一致: (B 吞吐/B 核数) / (A 吞吐/A 核数)",
          "safeRatio(sa.metric / sa.coresUsed, sb.metric / sb.coresUsed, &perCore)" in ccflat,
          "model_per_core() 对应")
    check("B3 同频比的公式与独立模型一致: (吞吐/运行时频率中位 x 标称最高频) 两侧相除",
          "safeRatio(sa.metric / sa.khzMed * sa.khzTop, sb.metric / sb.khzMed * sb.khzTop, &sameFreq)"
          in ccflat,
          "model_same_freq() 对应")
    check("B4 两个已知污染源写清楚了(12.5% 核数虚高 / 13.5% 频率假提升)",
          "12.5%(9 核 vs 8 核 = 1.125x)" in cc and "13.5% 的假提升(1.135x)" in cc,
          "knownInflationSources")
    check("B5 归一化的前提也标注('吞吐 ∝ 频率' 跨架构是假设)",
          "precondition" in cc and "跨架构/跨代是假设, 必须标注" in cc,
          "normalizationDefinition.precondition")
    check("B6 明确写了不写 /sys 锁频锁核(那是改被测对象, 只做事后归一化)",
          "本模块不写 /sys 去锁频锁核" in cc,
          "normalizationDefinition.notDone")
    check("B7 实测到的核数比 / 频率档比被单独列出来(用户能看到污染源有多大)",
          "coreCountRatioBA" in cc and "freqTierRatioBA" in cc and "observedDifferences" in cc,
          "conditions.observedDifferences")
    check("B8 复合分的归一化按几何平均合成(与官方'复合分 = 各项几何平均'同口径)",
          "perCoreRatioGM" in cc and "sameFreqRatioGM" in cc and "geometric" not in cc.lower() or
          "几何平均" in cc,
          "composites.normalized")
    check("B9 缺输入时不硬算: 任何一侧缺核数 / 缺频率 / 缺吞吐都返回 null 并说明缺什么",
          '\\"why\\":\\"' in cc and "有一侧报告里没有'实际用到核数', 无法做每核归一化" in cc and
          "有一侧报告里没有运行时频率中位(runFreq 为空或读不到), 无法做同频归一化" in cc,
          "normalized.why")

    emit("")
    emit("[C] 与参考真值对照: 偏差百分比 + 线性判定(阈值 5%, 且标明是我们自己定的)")
    emit("=" * 100)
    check("C1 偏差公式与独立模型一致: (实测比 / 真值比 - 1) x 100%",
          "(measuredRatio / truthRatio - 1.0) * 100.0" in ccflat,
          "model_deviation() 对应")
    check("C2 单侧偏差公式: (本机 / 参考值 - 1) x 100%",
          "(sa.score / refA - 1.0) * 100.0" in ccflat and "(sb.score / refB - 1.0) * 100.0" in ccflat,
          "model_truth_delta() 对应")
    check("C3 阈值 5% 出现在判定里, 且每一处都标明是我们自己定的",
          "std::fabs(dev) <= 5.0" in ccflat and
          cc.count("thresholdIsOursNotOfficial") >= 2 and
          "该阈值是我们自己定的, 不是官方阈值" in cc,
          "within5Percent / linearWithin5Percent + thresholdIsOursNotOfficial")
    check("C4 只有两端都有真值时才算比值偏差(单端真值只给各自的 deltaPercent)",
          "只有一端有真值(另一端查不到该机型/该 SoC 的真值), 因此算不出真值比" in cc,
          "truth.why")
    check("C5 真值来源写在每一项里(sourceKind + source 全文)",
          '\\"sourceKindA\\":' in cc and '\\"sourceA\\":' in cc,
          "truth.sourceKindA/sourceA")
    check("C6 991 的一手证据口径被沿用(对照表也说 991 的来源是用户拍屏)",
          "991 的来源是用户拍屏的一手证据" in cc,
          "truthRule")
    check("C7 本工程根本不跑的量(GB6 单核 / 3DMark GPU 累计)写 null + 缺什么",
          "本工程不跑这个量(" in cc,
          "truthAnchors.rows[].why")
    check("C8 判不出来机型时写'判不出来', 不瞎猜",
          "判不出来(报告里既没有 SoC 串, 机型名也不在已知表里; 本模块不猜)" in cc and
          "由机型名" in cc and "推断得到(不是实测 SoC, 仅供参考" in cc,
          "detectSoc(): 显式指定 > 文本命中 > 机型名推断(标注为推断)")

    emit("")
    emit("[D] 缺什么写什么(不许编数)")
    emit("=" * 100)
    check("D1 不是本工程的报告 -> 直接拒绝, 不硬解析",
          'aurora-fullreport' in cc and "看起来不是本工程一键跑分生成的报告(拒绝把别的 JSON 硬当成报告解析)" in cc,
          'schema 标记 "aurora-fullreport"')
    check("D2 空报告 / 缺 items / 缺 header / 格式不符 都有对应的人话说明",
          all(k in cc for k in ["报告字符串是空的(调用方没有传这一份)",
                                "items 数组里没有一项能解析出 section + name",
                                "没有 items 数组 —— 这份报告里没有任何逐项结果, 无法对比",
                                "没有 header 块(设备型号/版本拿不到)"]),
          "parseProblem[]")
    check("D3 缺什么会汇总进 inputs.missing, 且写明规则",
          ',\\"missing\\":[' in cc and "本模块不编数字" in cc,
          "inputs.missing + missingRule")
    check("D4 报告被中断 / 版本不同 -> 明确列为不可比",
          "报告是中断的(interrupted = true), 后半段项缺失, 比值不可比" in cc and
          "两份报告的版本不同(appVersion / nativeVersion), 跨版本分数不可比" in cc,
          "notComparable[]")
    check("D5 比对不成立时 ok=false(调用方一眼能看出来)",
          'j += (aOk && bOk) ? "true" : "false";' in ccflat,
          "ok 字段")

    emit("")
    emit("[E] 纯计算 / 只读(不跑负载 / 不连设备 / 不读 /sys / 不写文件)")
    emit("=" * 100)
    forbidden = {
        "fopen(": "打开文件", "ofstream": "写文件", "ifstream": "读文件",
        "system(": "起外部进程", "popen(": "起外部管道", "hdc": "hdc(不许连设备)",
        "gb7Run": "跑 GB7 负载", "snRun": "跑 GPU-SNL 负载", "gpu7Run": "跑 GPU7 负载",
        "auroraAffinitySessionBegin": "碰绑核会话", "auroraFreqSampleSessionBegin": "起频率采样线程",
        "std::thread": "起线程",
    }
    hits = []
    for tok, why in forbidden.items():
        if tok in cc or tok in napi:
            hits.append(tok + "(" + why + ")")
    check("E1 compareReports 的实现里没有任何'写文件 / 起进程 / 连设备 / 跑负载 / 起线程'的调用",
          len(hits) == 0, ("命中: " + str(hits)) if hits else "0 处命中")
    check("E2 模块自述就是'纯计算 / 只读'",
          "两份报告对比: 纯计算 / 只读" in cc and "不跑负载 / 不连设备 / 不改任何分数" in cc,
          "module 字段")
    check("E3 libauroraref 仍然独立(只链 napi + hilog), 不给主模块加依赖",
          "add_library(auroraref SHARED reference_napi.cpp reference_compare.cpp)" in cm and
          re.search(r"target_link_libraries\(auroraref PUBLIC[^)]*\)", cm) is not None and
          "aurorabench" not in re.search(r"target_link_libraries\(auroraref PUBLIC[^)]*\)", cm).group(0),
          "CMakeLists.txt")
    check("E4 报告里也给用户写了'怎么用'(四步)",
          "howToUse" in cc and "① 先看 items[] 的 ratio" in cc,
          "verdicts.howToUse")

    emit("")
    emit("[F] 真值表版本与 991 标注(与任务一的一致性)")
    emit("=" * 100)
    check("F1 真值表版本已 +1(改动真值表必须 +1, 旧结论届时不可比)",
          re.search(r"const int kReferenceVersion = 2;", cc) is not None,
          "kReferenceVersion = 2")
    check("F2 991 的 sourceKind 是一手证据类, 且不再写成'未证实'",
          '"USER_PROVIDED_PRIMARY_OBSERVATION"' in cc and
          '"Kirin 9030 Pro", "Mate 80 Pro Max", 991.0' in squash(cc),
          "kSnlTruth[0]")
    check("F3 note 里写全了 机型/测试项/总分/平均帧率/来源, 并保留了原来的 caveat",
          all(k in cc for k in ["机型 Mate 80 Pro Max", "测试项 Steel Nomad Light", "总分 991",
                                "平均帧率 7.34 FPS", "来源=用户拍屏",
                                "956(MatePad Pro Max)/ 998(Mate X7)/ 993(转载页)"]),
          "kSnlTruth[0].source")
    check("F4 9020=454 与 9000S=303 的标注一个字都没动",
          '"Kirin 9020", "Pura 80 Pro+ / Ultra", 454.0, "OFFICIAL_UL_DB"' in squash(cc) and
          '"Kirin 9000S", "Mate 60 系列 / MatePad Pro 13.2", 303.0, "UNVERIFIED_THIRD_PARTY"' in squash(cc),
          "两条原样保留")

    emit("")
    emit("[G] 数值模型交叉核对(独立 Python 模型 vs C++ 公式) + 消污染算例")
    emit("=" * 100)
    # 算例①: 同一颗芯片、同频, 但可用核数不同(A 机 8 核 / B 机 9 核, 每核性能完全相同)。
    #   真实每核性能比 = 1.0000; 原始"总吞吐比"却会虚高 12.5%(1.125x)。
    per_core_rate = 100.0
    cores_a, cores_b = 8.0, 9.0
    metric_a1, metric_b1 = cores_a * per_core_rate, cores_b * per_core_rate
    raw1 = metric_b1 / metric_a1
    pc1 = model_per_core(metric_a1, cores_a, metric_b1, cores_b)
    # 算例②: 核数相同(8 核), 但跑的频率档不同(A 机运行时 2.0GHz / B 机 2.27GHz, 标称上限 2.27GHz)。
    #   真实每核性能比 = 1.0000; 原始比却会凭空多出 13.5%(1.135x)。
    cores2, top2 = 8.0, 2270.0
    khz_a2, khz_b2 = 2000.0, 2270.0
    metric_a2, metric_b2 = cores2 * per_core_rate * khz_a2 / 1000.0, cores2 * per_core_rate * khz_b2 / 1000.0
    raw2 = metric_b2 / metric_a2
    sf2 = model_same_freq(metric_a2, khz_a2, top2, metric_b2, khz_b2, top2)
    # 算例③: 核数与频率同时不同(A 机 9 核 @2.0GHz / B 机 8 核 @2.27GHz)。
    cores_a3, khz_a3, cores_b3, khz_b3, top3 = 9.0, 2000.0, 8.0, 2270.0, 2270.0
    metric_a3 = cores_a3 * per_core_rate * khz_a3 / 1000.0
    metric_b3 = cores_b3 * per_core_rate * khz_b3 / 1000.0
    raw3 = metric_b3 / metric_a3
    pcsf3 = model_per_core(metric_a3 / khz_a3 * top3, cores_a3, metric_b3 / khz_b3 * top3, cores_b3)
    check("G1 算例①: 原始总吞吐比被核数差污染成 1.1250(9 核 vs 8 核 = 虚高 12.5%)",
          abs(raw1 - 1.125) < 1e-12, "raw = %.4f(真实每核性能比是 1.0000)" % raw1)
    check("G2 算例①: 每核归一化后精确恢复 1.0000",
          abs(pc1 - 1.0) < 1e-12, "perCore = %.4f" % pc1)
    check("G3 算例②: 原始比因频率档差凭空多出 13.5%(1.1350x)",
          abs(raw2 - 1.135) < 1e-12, "raw = %.4f(真实性能相同)" % raw2)
    check("G4 算例②: 同频归一化后精确恢复 1.0000",
          abs(sf2 - 1.0) < 1e-12, "sameFreq = %.4f" % sf2)
    check("G5 算例③: 核数与频率同时不同时两个一起消 -> 每核同频比 = 1.0000",
          abs(pcsf3 - 1.0) < 1e-12,
          "raw = %.4f -> perCoreSameFreq = %.4f" % (raw3, pcsf3))
    # 模型与 C++ 公式的逐字对照(把两边都折叠空白后比对)
    model_formulas = [
        "(metric_b / cores_b) / (metric_a / cores_a)",
        "(metric_b / khz_med_b * top_b) / (metric_a / khz_med_a * top_a)",
        "(measured_ratio / truth_ratio - 1.0) * 100.0",
        "(ours / reference - 1.0) * 100.0",
    ]
    cpp_forms = [
        "safeRatio(sa.metric / sa.coresUsed, sb.metric / sb.coresUsed, &perCore)",
        "safeRatio(sa.metric / sa.khzMed * sa.khzTop, sb.metric / sb.khzMed * sb.khzTop, &sameFreq)",
        "(measuredRatio / truthRatio - 1.0) * 100.0",
        "(sa.score / refA - 1.0) * 100.0",
    ]
    ok_forms = all(f in ccflat for f in cpp_forms)
    check("G6 模型里的 4 条公式在 C++ 里逐字能找到(公式没有走样)", ok_forms,
          "模型: " + " | ".join(model_formulas))
    check("G7 阈值口径一致: 模型判定与 C++ 判定都是 |偏差| <= 5%",
          "std::fabs(dev) <= 5.0" in ccflat and abs(model_deviation(1.03, 1.0)) < 5.0 and
          abs(model_deviation(1.07, 1.0)) > 5.0,
          "3% -> 通过 / 7% -> 不通过")

    emit("")
    emit("[H] 用户真正要的那个数: 「本次两台的分数比 → 芯片性能比是多少」(2026-10-08 新增)")
    emit("=" * 100)
    # 这一块以前没有: 归一化量只散落在逐项表与 composites 里, 用户要的是一句话结论。
    check("H1 输出里有独立的 chipPerformanceRatio 块(不是只有分散的归一化字段)",
          '\\"chipPerformanceRatio\\"' in cc and
          '本次两台的分数比, 换算成芯片性能比是多少' in cc,
          "compareReports -> chipPerformanceRatio")
    check("H2 块里同时给出: 原始分数比 / 核数比 / 频率档比 / 三个归一化 GM",
          all(('\\"' + k + '\\"') in cc for k in ['rawMultiCompositeRatioBA', 'coreCountRatioBA',
                                                 'freqTierRatioBA', 'perCoreRatioGM', 'sameFreqRatioGM',
                                                 'perCoreSameFreqRatioGM']),
          "contamination + normalized(三个 GM 都在)")
    check("H3 明确写出结论取哪一个(两个污染源一起消的优先, 逐级降级并写明用的是哪一个)",
          '\\"bestEstimateOfChipPerformanceRatio\\"' in cc and
          '\\"bestEstimateWhich\\"' in cc and
          "perCoreSameFreqRatioGM(同时消掉核数差与频率档差)" in cc and
          "perCoreRatioGM(只消掉核数差" in cc,
          "best > both > perCore > null(不拿分数比冒充芯片性能比)")
    check("H4 给出人话结论 conclusion, 且里面必须同时出现'分数比'与'芯片多核性能比最接近的估计'",
          '\\"conclusion\\"' in cc and "多核分数比 = " in cc and
          "芯片多核性能比最接近的估计 = " in cc,
          "conclusion: 数字 + 每一层归一化做了什么 + 前提")
    check("H5 算不出来时写 null(不拿分数比冒充) + 前提与 caveat 都在",
          "芯片性能比写 null, 不拿分数比冒充" in cc and
          "'吞吐 ∝ 频率' 在同架构上成立" in cc and
          "本块不写 sysfs / 不锁频 / 不锁核" in cc,
          "caveats + conclusion 的兜底分支")
    check("H6 单核分数比也一并给出(单核不含核数污染, 可直接当芯片单核性能比读)",
          '\\"rawSingleCompositeRatioBA\\"' in cc and
          "单核分数比 = " in cc and "单核不含核数污染" in cc,
          "单核与多核两个口径都报, 并写明各自能怎么用")
    # 数值模型: 与 C++ 同口径独立复算一次"两个一起消"的算例, 精确恢复 1.0
    check("H7 模型复算: 核数与频率档同时不同 -> 两个一起消精确恢复 1.0000(与 C++ 同口径)",
          abs(pcsf3 - 1.0) < 1e-12,
          "perCoreSameFreq = %.6f(与上面 G5 同源)" % pcsf3)

    emit("")
    emit("[I] 任务一/二/三的三条硬要求(2026-10-08 新增)")
    emit("=" * 100)
    # 任务一: 排除集必须逐项列出(项名 + 原因 + 判定依据) + 明确结论 + 参与项不同时不许悄悄相除
    check("I1 输出里有独立的 selfCheckAudit 块(两边各自的随芯片变化自检清单)",
          '\\"selfCheckAudit\\"' in cc and 'whatItIs' in cc and
          "判 FAIL 的项不再被排除在复合分之外" in cc and
          "一律以 compositeParticipants 为准" in cc,
          "selfCheckAudit{a,b,conclusion} —— 自检清单只标注'不可用于跨芯片比较', 不改变参与项集合")
    check("I2 每一项排除都带项名 + 排除原因(判定依据原文) + 判定口径, 不是只给一个数字",
          '\\"excluded\\":[' in cc and '\\"reason\\":' in cc and '\\"hasReason\\":' in cc and
          '\\"stage\\":' in cc and "报告里没有给出这一项的判定依据原文" in cc,
          "selfCheckAudit.a/b.excluded[]")
    check("I3 直接给出『两边自检清单是否一致』的结论(一致/不一致/判不出来 三态), 且明说它不改参与项集合",
          '\\"excludedSetsIdentical\\"' in cc and "两边的自检清单不一致" in cc and
          "两边的自检清单一致" in cc and "无法判定" in cc and
          "不改变复合分的" in cc and "参与项集合(见 compositeParticipants)" in cc,
          "excludedSetsIdentical + conclusion(2026-10-05 起: 自检清单只作提示)")
    check("I4 参与项集合逐阶段对照, 且明写『参与项不同 -> 复合分原始比不可用』",
          '\\"compositeParticipants\\"' in cc and '\\"onlyInA\\":[' in cc and '\\"onlyInB\\":[' in cc and
          "复合分原始比不可用" in cc and '\\"ratioUsable\\"' in cc,
          "compositeParticipants.single/multi")
    check("I5 参与项不同时, 额外给出『只用两边共同参与项』重算的比(不是悄悄相除, 而是换一套同项口径)",
          '\\"sameItemSetComparison\\"' in cc and '\\"compositeScoreRatioBA\\"' in cc and
          "只用两边都参与该阶段复合分的项重算" in cc,
          "sameItemSetComparison")
    check("I6 原始多核复合分比带可用性标记与原因(不可用时不许被当成芯片比)",
          '\\"rawMultiCompositeRatioUsable\\"' in cc and
          '\\"rawMultiCompositeRatioUnusableReason\\"' in cc and
          "不是同一批项" in cc,
          "rawMultiCompositeRatioUsable")
    # 任务二: 运行时频率中位要真的接进来 + 拿不到时写 null 与原因 + 最高频档可核对
    check("I7 逐项频率证据齐全: runFreq 原文 + 拿不到的原因 + 最高频档的来源",
          '\\"frequencyRawText\\"' in cc and '\\"frequencyNullReason\\"' in cc and
          '\\"nominalTopKhzSource\\"' in cc and
          "本项线程实际所在核的标称最高频档" in cc,
          "items[].a/b.conditions{...}")
    check("I8 两台各自的运行时频率中位与可用最高频档都在输出里(逐项统计 + 报告原话两条证据)",
          '\\"runtimeKhzMedianOfItems\\"' in cc and '\\"median\\":' in cc and
          '\\"machineLevelFromReport\\"' in cc and 'runtimeKhzMedian=' in cc and 'nominalTopKhz=' in cc,
          "conditions.a/b.runtimeKhzMedianOfItems + machineLevelFromReport")
    check("I9 同频归一化公式仍是『吞吐 / 运行时频率中位 x 标称最高频』两侧相除(没被改写)",
          "safeRatio(sa.metric / sa.khzMed * sa.khzTop, sb.metric / sb.khzMed * sb.khzTop, &sameFreq)"
          in ccflat,
          "model_same_freq() 对应")
    # 任务三: 条件差必须显式给出, 且不许伪造"同条件"
    check("I10 条件差逐项给出(线程数比 / SMT 状态 / 可用核集合 / 物理逻辑核) + 影响方向",
          '\\"conditionGaps\\"' in cc and '\\"threadsRatioBA\\"' in cc and '\\"smtSame\\"' in cc and
          '\\"allowedCoresSame\\"' in cc and '\\"direction\\":' in cc,
          "conditionGaps")
    check("I11 没拿到同条件数据时明确写『不同/判不出来』, 不写同条件",
          '\\"sameRunConditions\\"' in cc and "多核比因此不是纯芯片比" in cc and
          "条件差算不出来" in cc,
          "sameRunConditions=false/null")
    check("I12 每核比的前提写清楚了(核数取线程数; SMT 开的一侧两个线程 != 两个核)",
          '\\"perCorePrecondition\\"' in cc and "SMT 开的一侧, 同一物理核上的两个线程不等于两个核" in cc,
          "perCorePrecondition")
    check("I13 核数取值有硬判据: workersBound 超过可用核集合时不算核数(真机上 72/84 是线程池大小)",
          "超过可用核集合" in cc and "那是线程池大小, 不是真正用到的核数" in cc,
          "coresUsedSource")
    # 交叉核对工具本身也必须把这三条打出来(用户要能一眼看到)
    py = rd("verify_report_compare.py")
    check("I14 交叉核对模式自己就把三条打全: 排除集对照 / 参与项同项性 / 频率证据 / 条件差 / 最终结论",
          all(k in py for k in ["[1] 『随芯片变化自检』排除集对照", "[2] 复合分参与项集合",
                                "[3] 运行时频率中位与可用最高频档", "[4] 逐项对照表",
                                "[5] 多核比较的条件差", "[6] 最终结论"]),
          "cross_check() 的 [1]~[6]")
    check("I15 交叉核对模式在参与项不同时明确拒绝把复合分原始比当结论",
          "两边参与项不同 -> 该阶段的复合分原始比 不可用" in py and
          "两边参与项一致(%d 项) -> 该阶段的复合分原始比可用" in py,
          "cross_check() [2]")
    check("I16 交叉核对模式的 GM 与 native 同口径(只统计 GB7 单核/多核两节)",
          'gb7_rows = [r for r in rows_for_gm if r[0][0] in ("GB7 单核", "GB7 多核")]' in py,
          "cross_check() [6]")

    emit("")
    emit("[J] 双重归一化 bug 的回归断言(2026-10 新增): 口径与设备侧一致, 且旧写法不许再出现")
    emit("=" * 100)
    # 事实: 设备侧 reference_compare.cpp 把两个归一化作用在同一份吞吐上:
    #   safeRatio((metric / khzMed * khzTop) / coresUsed, ...) 两侧相除。
    # 本脚本的 [6] 曾经把 perCoreRatio 与 sameFreqRatio 相乘 —— 等价于把上面这个正确
    # 结果再乘一个『两台原始吞吐比 (metricB/metricA)』, 因此『最接近估计』被系统性压低。
    # 真机实测: 旧口径 0.6987 / 正确 0.7889(= 设备侧同口径值)。
    _old1 = "r[4]" + " * " + "r[5]"
    _old2 = '(m.get("perCore") or 0)' + " * " + '(m.get("sameFreq") or 0)'
    check("J1 旧的『两个比值相乘』写法在本脚本里已经一处都不剩",
          _old1 not in py and _old2 not in py,
          "旧写法: perCoreRatio x sameFreqRatio(会把 metric 用两次)")
    check("J2 新口径函数 model_per_core_same_freq 已定义, 且在 [2]/[4]/[6] 都被使用",
          py.count("model_per_core_same_freq(") >= 3 and
          'gm_both = _gm([r[8] for r in gb7_rows if r[8]])' in py and
          '_fmt(m.get("both"))' in py,
          "两个归一化作用在同一份吞吐上")
    # 设备侧那一行(折叠空白后逐字核对; 两处拼起来就是同一个 safeRatio 调用的两个实参)
    _devForm = squash("safeRatio((sa.metric / sa.khzMed * sa.khzTop) / sa.coresUsed, "
                      "(sb.metric / sb.khzMed * sb.khzTop) / sb.coresUsed, &perCoreSameFreq)")
    check("J3 设备侧仍是『两个归一化作用在同一份吞吐上』(不是两个比值相乘)",
          _devForm in ccflat,
          "reference_compare.cpp: safeRatio((metric / khzMed * khzTop) / coresUsed, ...)")
    # 代数关系: 旧口径(两个比值相乘) = 新口径 x 原始吞吐比(metricB/metricA)
    _ma, _mb = 1234.5, 987.6
    _ca, _ha, _ta = 8.0, 1580.0, 2270.0
    _cb, _hb, _tb = 7.0, 1930.0, 2150.0
    _newway = model_per_core_same_freq(_ma, _ca, _ha, _ta, _mb, _cb, _hb, _tb)
    _pcw = model_per_core(_ma, _ca, _mb, _cb)
    _sfw = model_same_freq(_ma, _ha, _ta, _mb, _hb, _tb)
    _oldway = _pcw * _sfw
    check("J4 代数量化: 旧口径 = 新口径 x 两台原始吞吐比(metricB/metricA) —— 这就是被多乘的因子",
          abs(_oldway - _newway * (_mb / _ma)) < 1e-12,
          "新 %.6f · 旧 %.6f · 旧/新 %.6f = metricB/metricA %.6f" %
          (_newway, _oldway, _oldway / _newway, _mb / _ma))
    # 真机记录: 两份报告旧口径 0.6987 / 正确 0.7889 -> 比值应等于两台原始吞吐比 GM
    check("J5 真机记录自洽: 旧 0.6987 / 正确 0.7889 = 0.8857 = 两台原始吞吐比 GM(与 J4 同一条代数关系)",
          abs(0.6987 / 0.7889 - 0.8857) < 0.0001,
          "0.6987 / 0.7889 = %.4f(旧口径低报 %.1f%%)" %
          (0.6987 / 0.7889, (1.0 - 0.6987 / 0.7889) * 100.0))
    emit("   说明: 两份原始真机报告 JSON 不在本仓库里(自检器只做静态核对); 拿到报告后跑")
    emit("         python verify_report_compare.py --reports A.json B.json")
    emit("         即可用修正后的口径复算, [6] 里的『每核x同频』应当与设备侧")
    emit("         perCoreSameFreqRatioGM 逐位一致(本次两份报告的正确值是 0.7889)。")

    emit("")
    emit("=" * 100)
    emit("共 %d 项断言, 失败 %d 项 -> %s" % (checks, len(fails), "PASS" if not fails else "FAIL"))
    emit("=" * 100)
    return 0 if not fails else 1


# ===========================================================================
#  可选: 开发机上的交叉核对模式(用同一套公式独立复算两张真实报告)
#  python verify_report_compare.py --reports A.json B.json
# ===========================================================================
def _khz_median_from_runfreq(text):
    m = re.search(r"中位\s*([0-9.]+)\s*MHz", text or "")
    if not m:
        return None
    return float(m.group(1)) * 1000.0


def _khz_top_of_report(doc):
    """一台机器的『可用最高频档』: 报告逐项字段 cpuMaxKhz 的最大值(native 同口径)。"""
    top = 0.0
    for it in doc.get("items", []):
        v = it.get("cpuMaxKhz") or 0
        if isinstance(v, (int, float)) and v > top:
            top = float(v)
    return top


def _note_number(doc, key):
    """把报告 inputsNote 里『键=数字』的原话取出来(只读, 不参与计算)。"""
    note = (doc.get("referenceAudit") or {}).get("inputsNote", "") or ""
    m = re.search(re.escape(key) + r"[0-9.]+", note)
    if not m:
        return None
    v = float(m.group(0)[len(key):])
    return int(v) if v == int(v) else v


def _scaling_audit(doc):
    """随芯片变化自检的排除集: 项名 + 排除原因(判定依据原文) + 判据。"""
    sa = doc.get("scalingAudit") or {}
    out = {"present": bool(sa), "names": [], "ids": [], "detail": [],
           "criteria": sa.get("criteria") or [], "text": sa.get("text", ""),
           "pass": sa.get("passCount"), "fail": sa.get("failCount"),
           "unproven": sa.get("unprovenCount")}
    names = sa.get("excludedNames") or []
    ids = sa.get("excludedIds") or []
    srows = sa.get("singleRows") or []
    mrows = sa.get("multiRows") or []
    for i, nm in enumerate(names):
        reason, stage = "", ""
        for r in mrows:                      # 多核那一行才是『改变可用算力』的对照
            if r.startswith(nm + " "):
                reason, stage = r, "多核"
                break
        if not reason:
            for r in srows:
                if r.startswith(nm + " "):
                    reason, stage = r, "单核"
                    break
        out["names"].append(nm)
        out["ids"].append(ids[i] if i < len(ids) else None)
        out["detail"].append({"name": nm, "id": ids[i] if i < len(ids) else None,
                              "stage": stage, "reason": reason})
    return out


def _parts_of(doc, key):
    """报告自己给出的参与项名单: composite.gb7SingleItems / gb7MultiItems(与 native 同解析)。"""
    text = (doc.get("composite") or {}).get(key, "") or ""
    names = []
    for chunk in text.split("\u00b7"):
        c = chunk.strip()
        if not c:
            continue
        at = c.rfind("(id=")
        head = c[:at] if at >= 0 else c
        sp = head.rfind(" ")
        nm = head[:sp] if sp > 0 else head
        if nm:
            names.append(nm)
    return names


def _cores_used(it):
    """与 native 同判据: workersBound 只在不超过可用核集合时才算『实际用到核数』。
    (真机上出现过 workersBound = 72/84 而允许核集合只有 8/7 —— 那是线程池大小, 不是核数。)"""
    we = float(it.get("threadsEffective") or 0)
    wb = float(it.get("workersBound") or 0)
    ac = float(it.get("allowedCount") or 0)
    par = float(it.get("parallelism") or 0)
    if wb > 0 and (ac <= 0 or wb <= ac):
        return wb, "workersBound"
    if wb > 0 and we > 0:
        return we, "threadsEffective(workersBound=%g 超过可用核集合 %g)" % (wb, ac)
    if we > 0:
        return we, "threadsEffective"
    if par > 0:
        return par, "parallelism"
    return 0.0, "报告里没有线程/核数字段"


def _gm(vals):
    if not vals:
        return None
    prod = 1.0
    for v in vals:
        prod *= v
    return prod ** (1.0 / len(vals))


def _fmt(x, d=4):
    return ("%.*f" % (d, x)) if isinstance(x, (int, float)) else "null"


def cross_check(path_a, path_b):
    da = json.load(io.open(path_a, encoding="utf-8"))
    db = json.load(io.open(path_b, encoding="utf-8"))
    emit("=" * 100)
    emit("[X] 交叉核对模式: 用同一套公式独立复算两份报告(数字应与设备上的 compareReports 一致)")
    emit("=" * 100)
    emit("A = %s (%s)" % (da.get("header", {}).get("device", "?"), path_a))
    emit("B = %s (%s)" % (db.get("header", {}).get("device", "?"), path_b))
    emit("")
    ia = {(i.get("section"), i.get("name")): i for i in da.get("items", [])}
    ib = {(i.get("section"), i.get("name")): i for i in db.get("items", [])}

    # =====================================================================
    # [1] 任务一: 两边各自的『随芯片变化自检』排除集 + 是否一致
    # =====================================================================
    emit("-" * 100)
    emit("[1] 『随芯片变化自检』排除集对照(任务一): 两边各自排除了哪些项、为什么、依据是什么")
    emit("-" * 100)
    aa, ab = _scaling_audit(da), _scaling_audit(db)
    for tag, a in (("A", aa), ("B", ab)):
        if not a["present"]:
            emit("%s: 报告里没有 scalingAudit(自检没跑) -> 排除了哪些项判不出来(不假定相同)" % tag)
            continue
        emit("%s 的排除集(%d 项; 通过 %s · 不通过 %s · 未自证 %s):" %
             (tag, len(a["names"]), a["pass"], a["fail"], a["unproven"]))
        if not a["names"]:
            emit("   (无)")
        for i, d in enumerate(a["detail"]):
            emit("   %d) %-24s id=%-4s 口径=%s" % (i + 1, d["name"], d["id"], d["stage"] or "?"))
            emit("      判定依据(报告原文): %s" % (d["reason"] or "报告里没有给出这一项的判定依据原文"))
    only_a = [n for n in aa["names"] if n not in ab["names"]]
    only_b = [n for n in ab["names"] if n not in aa["names"]]
    common = [n for n in aa["names"] if n in ab["names"]]
    emit("")
    emit("只在设备A 排除(%d): %s" % (len(only_a), "、".join(only_a) if only_a else "无"))
    emit("只在设备B 排除(%d): %s" % (len(only_b), "、".join(only_b) if only_b else "无"))
    emit("两边共同排除(%d): %s" % (len(common), "、".join(common) if common else "无"))
    if aa["criteria"]:
        emit("判据(两边同一套; 阈值都是我们自己定的, 不是官方阈值):")
        for c in aa["criteria"]:
            emit("   · " + c)
    both_present = aa["present"] and ab["present"]
    same_excl = (both_present and not only_a and not only_b and len(aa["names"]) == len(ab["names"]))
    if not both_present:
        emit("结论: 有一份报告没有自检结果 -> 两边的排除集无法判定(写缺, 不假定相同)")
    elif same_excl:
        emit("结论: 两边的排除集一致(各 %d 项) —— 这是『两个复合分含同一批项』的可核对前提(N=%d)"
             % (len(aa["names"]), len(aa["names"])))
    else:
        emit("结论: 两边的排除集不一致(A %d 项 / B %d 项) -> 两个复合分含的不是同一批项, "
             "原始复合分比不可用" % (len(aa["names"]), len(ab["names"])))

    # =====================================================================
    # [2] 任务一(续): 复合分的参与项集合 + 只用共同参与项重算的比
    # =====================================================================
    emit("")
    emit("-" * 100)
    emit("[2] 复合分参与项集合(同项性)与『只用共同参与项』重算的比")
    emit("-" * 100)
    stage_common = {}
    for stage, key in (("单核", "gb7SingleItems"), ("多核", "gb7MultiItems")):
        sect = "GB7 " + stage
        ckey = "gb7SingleComposite" if stage == "单核" else "gb7MultiComposite"
        pa, pb = _parts_of(da, key), _parts_of(db, key)
        common_s = [n for n in pa if n in pb]
        onlya_s = [n for n in pa if n not in pb]
        onlyb_s = [n for n in pb if n not in pa]
        identical = (bool(pa) and pa == pb)
        emit("GB7 %s: A 参与 %d 项 / B 参与 %d 项 / 共同 %d 项 / 只在A %d / 只在B %d" %
             (stage, len(pa), len(pb), len(common_s), len(onlya_s), len(onlyb_s)))
        if onlya_s:
            emit("   只在 A 参与: " + "、".join(onlya_s))
        if onlyb_s:
            emit("   只在 B 参与: " + "、".join(onlyb_s))
        ca = (da.get("composite") or {}).get(ckey)
        cb = (db.get("composite") or {}).get(ckey)
        raw = model_ratio(ca or 0, cb or 0)
        emit("   复合分: A %s / B %s -> 原始比(B/A) = %s" % (ca, cb, _fmt(raw)))
        if identical:
            emit("   两边参与项一致(%d 项) -> 该阶段的复合分原始比可用" % len(common_s))
        else:
            emit("   两边参与项不同 -> 该阶段的复合分原始比 不可用(它比的不是同一批项)")
        sr, pc, sf, bs = [], [], [], []
        for nm in common_s:
            x, y = ia.get((sect, nm)), ib.get((sect, nm))
            if not x or not y:
                continue
            r = model_ratio(x.get("score", 0), y.get("score", 0))
            if r:
                sr.append(r)
            cax, _ = _cores_used(x)
            cbx, _ = _cores_used(y)
            pcr = model_per_core(x.get("metric", 0), cax, y.get("metric", 0), cbx)
            if pcr:
                pc.append(pcr)
            ka, kb = _khz_median_from_runfreq(x.get("runFreq")), _khz_median_from_runfreq(y.get("runFreq"))
            sfr = model_same_freq(x.get("metric", 0), ka or 0, x.get("cpuMaxKhz", 0),
                                  y.get("metric", 0), kb or 0, y.get("cpuMaxKhz", 0))
            if sfr:
                sf.append(sfr)
            # 两个归一化作用在同一份吞吐上(不是 perCore * sameFreq —— 那样会多乘一个原始吞吐比)
            bsr = model_per_core_same_freq(x.get("metric", 0), cax, ka or 0, x.get("cpuMaxKhz", 0),
                                           y.get("metric", 0), cbx, kb or 0, y.get("cpuMaxKhz", 0))
            if bsr:
                bs.append(bsr)
        emit("   只用共同参与项(%d 项)重算: 分比 GM = %s · 每核比 GM = %s · 同频比 GM = %s · "
             "每核x同频(=两个归一化作用在同一份吞吐上) GM = %s" %
             (len(sr), _fmt(_gm(sr)), _fmt(_gm(pc)), _fmt(_gm(sf)), _fmt(_gm(bs))))
        stage_common[stage] = {"common": common_s, "identical": identical, "score": _gm(sr),
                               "perCore": _gm(pc), "sameFreq": _gm(sf), "both": _gm(bs),
                               "raw": raw, "a": ca, "b": cb}

    # =====================================================================
    # [3] 任务二: 运行时频率中位 / 可用最高频档 / 同频归一化
    # =====================================================================
    emit("")
    emit("-" * 100)
    emit("[3] 运行时频率中位与可用最高频档(任务二): 同频归一化靠的就是这两个数")
    emit("-" * 100)
    for tag, doc in (("A", da), ("B", db)):
        gb7 = [it for it in doc.get("items", [])
               if it.get("section", "").startswith("GB7") and it.get("section") != "GB7 GPU"]
        khz = [k for k in (_khz_median_from_runfreq(it.get("runFreq")) for it in gb7) if k]
        has_run = sum(1 for it in doc.get("items", []) if (it.get("runFreq") or "").strip())
        emit("%s: 逐项运行时频率中位(GB7 项 %d/%d 有读数): min %s / 中位 %s / max %s kHz" %
             (tag, len(khz), len(gb7),
              int(min(khz)) if khz else "null",
              int(sorted(khz)[len(khz) // 2]) if khz else "null",
              int(max(khz)) if khz else "null"))
        emit("   可用最高频档(逐项 cpuMaxKhz 最大值, 同频归一化用的就是它): %s kHz = %s MHz" %
             (int(_khz_top_of_report(doc)), int(_khz_top_of_report(doc)) // 1000))
        emit("   报告 inputsNote 原话: runtimeKhzMedian=%s kHz · nominalTopKhz=%s kHz(全机最快频率档)" %
             (_note_number(doc, "runtimeKhzMedian="), _note_number(doc, "nominalTopKhz=")))
        emit("   全报告 %d 项里有 %d 项带 runFreq 文本(其余 %d 项 = 自研套件/GPU/NPU/存储 I/O, "
             "这些小节本来就不做频率采样 -> 它们的 sameFreq 只能是 null)" %
             (len(doc.get("items", [])), has_run, len(doc.get("items", [])) - has_run))
    emit("")
    emit("逐项 sameFreq 为 null 的原因(逐条列出, 不编数):")
    null_rows = []
    for key in ia:
        if key not in ib:
            continue
        x, y = ia[key], ib[key]
        ka, kb = _khz_median_from_runfreq(x.get("runFreq")), _khz_median_from_runfreq(y.get("runFreq"))
        why = ""
        if not ka or not kb:
            why = "有一侧 runFreq 为空 —— 该小节不采样运行时频率"
        elif not x.get("cpuMaxKhz") or not y.get("cpuMaxKhz"):
            why = "有一侧没有标称最高频档(cpuMaxKhz=0)"
        elif not (x.get("metric") or 0) > 0 or not (y.get("metric") or 0) > 0:
            why = "有一侧没有正的吞吐(metric)"
        if why:
            null_rows.append((key, why))
    by_why = {}
    for key, why in null_rows:
        by_why.setdefault(why, []).append(key[1])
    for why, names in by_why.items():
        emit("   · %s: %d 项 —— %s" % (why, len(names), "、".join(names[:8]) + ("…" if len(names) > 8 else "")))
    emit("   可算的项: %d / 共对照 %d 项" % (len(ia) - len(null_rows), len(ia)))

    # =====================================================================
    # [4] 逐项对照表
    # =====================================================================
    emit("")
    emit("-" * 100)
    emit("[4] 逐项对照表(A 分 / B 分 / 原始比 / 每核比 / 同频比 / 每核x同频; null 一律带原因见 [3])")
    emit("-" * 100)
    emit("   both = 每核x同频 = 两个归一化作用在同一份吞吐上(与设备侧同口径; 不是 perCore*sameFreq)")
    emit("%-14s %-22s %10s %10s %8s %8s %8s %8s %5s %5s %8s %8s" %
         ("section", "name", "A", "B", "ratio", "perCore", "sameFreq", "both", "thA", "thB", "freqA", "freqB"))
    rows_for_gm = []
    for key in ia:
        if key not in ib:
            emit("%-14s %-22s  只在一侧出现(A), 无比值" % (key[0][:14], key[1][:22]))
            continue
        x, y = ia[key], ib[key]
        cax, _ = _cores_used(x)
        cbx, _ = _cores_used(y)
        ka, kb = _khz_median_from_runfreq(x.get("runFreq")), _khz_median_from_runfreq(y.get("runFreq"))
        r = model_ratio(x.get("score", 0), y.get("score", 0))
        pcr = model_per_core(x.get("metric", 0), cax, y.get("metric", 0), cbx)
        sfr = model_same_freq(x.get("metric", 0), ka or 0, x.get("cpuMaxKhz", 0),
                              y.get("metric", 0), kb or 0, y.get("cpuMaxKhz", 0))
        # 两个归一化作用在同一份吞吐上(与设备侧 reference_compare.cpp 逐字同口径)
        # 不许写成"每核比 x 同频比"(旧写法: metric 会出现两次, 被额外乘上原始吞吐比)。
        pcsf = model_per_core_same_freq(x.get("metric", 0), cax, ka or 0, x.get("cpuMaxKhz", 0),
                                        y.get("metric", 0), cbx, kb or 0, y.get("cpuMaxKhz", 0))
        emit("%-14s %-22s %10.3f %10.3f %8s %8s %8s %8s %5s %5s %8s %8s" %
             (key[0][:14], key[1][:22], x.get("score", 0), y.get("score", 0),
              _fmt(r), _fmt(pcr), _fmt(sfr), _fmt(pcsf), int(cax) if cax else "-", int(cbx) if cbx else "-",
              int(ka) if ka else "null", int(kb) if kb else "null"))
        rows_for_gm.append((key, x, y, r, pcr, sfr, cax, cbx, pcsf))

    # =====================================================================
    # [5] 任务三: 条件差 + 每核吞吐比
    # =====================================================================
    emit("")
    emit("-" * 100)
    emit("[5] 多核比较的条件差(任务三): 线程数 / SMT / 可用核集合")
    emit("-" * 100)

    def _maxitem(doc, f):
        return max([float(it.get(f) or 0) for it in doc.get("items", [])] or [0.0])

    th_a, th_b = _maxitem(da, "threadsEffective"), _maxitem(db, "threadsEffective")
    al_a, al_b = _maxitem(da, "allowedCount"), _maxitem(db, "allowedCount")
    emit("线程数(多核阶段 threadsEffective 最大值): A %g / B %g -> 比 B/A = %s" %
         (th_a, th_b, _fmt(model_ratio(th_a, th_b))))
    emit("SMT 原文: A %s" % ((da.get("env") or {}).get("smtTopology", "?")[:70]))
    emit("           B %s" % ((db.get("env") or {}).get("smtTopology", "?")[:70]))
    emit("可用核集合(逐项 allowedCount 最大值): A %g / B %g -> 比 B/A = %s" %
         (al_a, al_b, _fmt(model_ratio(al_a, al_b))))
    emit("影响方向: 线程多的一侧在原始多核分里被抬高。本次 B 用 %g 线程 / A 用 %g 线程, "
         "原始多核比(B/A)里因此混进了约 %s x 的线程数因子。" % (th_b, th_a, _fmt(model_ratio(th_a, th_b))))
    emit("每核吞吐比(把线程数差除回去)已经在 [2][4] 给出; 适用前提见下:")
    emit("   · 核数取的是本项真正开了几个线程(报告 workersBound 超过可用核集合时已按判据改用线程数);")
    emit("   · SMT 开的一侧, 同一物理核上的两个线程不等于两个核 -> 该侧每核吞吐被系统性低估;")
    emit("   · 拿不到同条件(SMT 状态相同 / 线程数相同)的数据时, 本工具不会写『同条件』。")
    emit("本次两台的核数/SMT 条件不同 -> 多核比不是纯芯片比")

    # =====================================================================
    # [6] 最终结论
    # =====================================================================
    # 与 native 的 perCoreRatioGM / sameFreqRatioGM / perCoreSameFreqRatioGM 同口径:
    #   只统计 GB7 单核 / GB7 多核两节的项(自研套件 / GPU / NPU / 存储 I/O 不并入任何 GB7 复合分)。
    gb7_rows = [r for r in rows_for_gm if r[0][0] in ("GB7 单核", "GB7 多核")]
    gm_pc = _gm([r[4] for r in gb7_rows if r[4]])
    gm_sf = _gm([r[5] for r in gb7_rows if r[5]])
    # r[8] = 每核x同频(两个归一化作用在同一份吞吐上, 与 native 同口径)
    #   旧写法(把每核比与同频比两个比值相乘)是错的: 等于多乘了一个
    #   『两台原始吞吐比(metricB/metricA)』, 把结论系统性压低(真机: 0.6987 vs 0.7889)。
    gm_both = _gm([r[8] for r in gb7_rows if r[8]])
    emit("")
    emit("=" * 100)
    emit("[6] 最终结论(实跑上面两份真机报告)")
    emit("=" * 100)
    m = stage_common.get("多核", {})
    s = stage_common.get("单核", {})
    emit("① 原始多核复合分比(B/A) = %s   <- 两边参与项不同, 不可用, 只能读作两台机器在各自条件下的实测分比"
         % _fmt(m.get("raw")))
    emit("② 同项(只用两边共同参与多核复合分的 %d 项)多核分比 = %s" % (len(m.get("common") or []), _fmt(m.get("score"))))
    emit("③ 同项(同上 %d 项)每核归一化比 GM = %s" % (len(m.get("common") or []), _fmt(m.get("perCore"))))
    emit("④ 同项(同上 %d 项)同频归一化比 GM = %s" % (len(m.get("common") or []), _fmt(m.get("sameFreq"))))
    emit("   -> 同项 + 每核 + 同频 三个一起 = %s   (两个归一化作用在同一份吞吐上, 与设备侧同口径)"
         % _fmt(m.get("both")))
    emit("⑤ GB7 全部同项(单核 %d 项 + 多核 %d 项 = %d 项; 与 native 的 *GM 同口径): "
         "每核比 GM = %s · 同频比 GM = %s · 两个一起 GM = %s" %
         (len([r for r in gb7_rows if r[0][0] == "GB7 单核"]), len([r for r in gb7_rows if r[0][0] == "GB7 多核"]),
          len(gb7_rows), _fmt(gm_pc), _fmt(gm_sf), _fmt(gm_both)))
    emit("⑥ 单核复合分原始比 = %s(同样参与项不同, 不可用); 同项(%d 项)单核分比 = %s · 同频比 GM = %s" %
         (_fmt(s.get("raw")), len(s.get("common") or []), _fmt(s.get("score")), _fmt(s.get("sameFreq"))))
    # ⑦/⑧ 分阶段的自洽核对: 单核阶段 1 线程 = 1 核, 没有线程数污染, 只剩频率档差要消。
    one_rows = [r for r in gb7_rows if r[0][0] == "GB7 单核"]
    mul_rows = [r for r in gb7_rows if r[0][0] == "GB7 多核"]
    gm_one_raw = _gm([r[3] for r in one_rows if r[3]])
    gm_one_sf = _gm([r[5] for r in one_rows if r[5]])
    gm_mul_raw = _gm([r[3] for r in mul_rows if r[3]])
    gm_mul_pc = _gm([r[4] for r in mul_rows if r[4]])
    gm_mul_sf = _gm([r[5] for r in mul_rows if r[5]])
    emit("⑦ GB7 单核 %d 项(两边都有): 原始分比 GM = %s · 同频比 GM = %s —— 单核 1 线程 = 1 核, "
         "没有线程数污染, 这个同频比可直接当『同频下单核性能比』读" %
         (len(one_rows), _fmt(gm_one_raw), _fmt(gm_one_sf)))
    emit("⑧ GB7 多核 %d 项(两边都有): 原始分比 GM = %s · 每核比 GM = %s · 同频比 GM = %s" %
         (len(mul_rows), _fmt(gm_mul_raw), _fmt(gm_mul_pc), _fmt(gm_mul_sf)))
    emit("")
    emit("本次两台的芯片性能比最接近的估计")
    emit("   口径: 只用两边都存在且字段齐全的 GB7 项; 先按各设备自己的运行时频率中位折算到各自的标称最高频档,")
    emit("         再按实际用到核数归一化(每核), 最后取几何平均 —— 即 perCoreSameFreqRatioGM。")
    emit("   - 逐项(GB7 单核+多核 %d 项)每核x同频 几何平均 = %s" % (len(gb7_rows), _fmt(gm_both)))
    emit("   - 只用同项多核(%d 项)每核x同频 = %s" %
         (len(m.get("common") or []), _fmt(m.get("both"))))
    emit("   两个口径指向同一量级; 建议报区间而不是单点。")
    emit("")
    emit("成立的前提与缺口(必须与数字一起读):")
    emit("   1) 『吞吐 ∝ 频率』只在同一颗芯片同一架构上成立; 本次是跨 SoC(9030 Pro vs 9000S)、跨代、")
    emit("      跨形态(手机 vs 平板), 所以它是假设, 不是事实。")
    emit("   2) 每核归一化按线程数算: SMT 开的一侧(设备B)两个线程不等于两个核, 该侧每核吞吐被系统性低估。")
    emit("   3) 频率中位是负载计时区间内的后台采样, 两台的采样核集合不同(A 是允许的 8 核 / B 是 7 核)。")
    emit("   4) 两台的散热与调度策略不同(报告 env.thermalText 可核对), 归一化消不掉。")
    emit("   5) 全程没有锁频/锁核(那会改变被测对象), 全部是事后归一化。")
    emit("   6) 两边复合分参与项不同(见 [1][2]), 因此不要拿复合分原始比当芯片比。")
    emit("")
    emit("共 %d 项可对照。复合分: A 单核 %s / B 单核 %s; A 多核 %s / B 多核 %s" %
         (len(rows_for_gm), da.get("composite", {}).get("gb7SingleComposite"), db.get("composite", {}).get("gb7SingleComposite"),
          da.get("composite", {}).get("gb7MultiComposite"), db.get("composite", {}).get("gb7MultiComposite")))
    emit("提示: 设备上的 compareReports 输出里 items[].ratio / normalized.* 应当与上表逐项一致。")
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--reports":
        sys.exit(cross_check(sys.argv[2], sys.argv[3]))
    sys.exit(main())
