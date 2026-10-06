# -*- coding: utf-8 -*-
"""
verify_gb7_repeatability.py —— GB7 单核/多核阶段"可重复性 / 离散度"的静态自检器
(离线: 不连设备、不跑 hdc、不跑构建、不改任何文件)

为什么需要它(用户的验收标准): "要有跑分的意义, 像其他主流跑分软件一样"。
主流跑分软件建立可信度的方式是重复测量:
同一项跑多轮 -> 报中位与离散度 -> 离散度大就明说这个数字不可信。
在此之前本工程只有 GPU-SNL 小节(libaurorasn)跑多轮, GB7 的 16 项单核 / 8 项多核只跑一轮。

本脚本核对的是"代码里有没有这样做", 逐条:
  A) 轮数是选项(默认 2), 且只影响"同一项测量做几次" —— 单轮的工作量一个字都没动;
  B) 代表值一律取中位: 代码里不存在"取最好一轮 / 取最大"的路径;
  C) 每一项与复合分都给出 中位 / 最小 / 最大 / 相对离散度;
  D) 有可信度判定, 且明确写出阈值是我们自己定的、不是官方的;
  E) 轮数只有 1 时写"没有离散度数据, 不能据此判断可信度", 不假装稳定;
  F) 计分口径(k / conv / 公式 / metric / unit)与单轮负载代码一个字都没动(逐条比对);
  G) 逐轮原始值全部列出(不是只留最好一轮)。

退出码 0 = 全部通过, 1 = 有 FAIL。
"""
import io
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_gb7_repeatability_out.txt"), "w", encoding="utf-8", newline="\n")
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


LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]


def main():
    hdr = rd("gb7.h")
    cpp = rd("gb7.cpp")
    napi = rd("napi_init.cpp")
    dts = rd(os.path.join("types", "libaurorabench", "index.d.ts"))
    if not hdr or not cpp or not napi:
        emit("找不到 gb7.h / gb7.cpp / napi_init.cpp")
        return 1

    emit("=" * 100)
    emit("[A] 轮数是选项, 默认 2 轮, 且只改'做几次'")
    emit("=" * 100)
    check("A1 默认轮数与上限是显式常量(默认 2 轮, 上限 9 轮)",
          re.search(r"#define\s+GB7_DEFAULT_ROUNDS\s+2\b", hdr) is not None and
          re.search(r"#define\s+GB7_MAX_ROUNDS\s+9\b", hdr) is not None,
          "GB7_DEFAULT_ROUNDS=2 / GB7_MAX_ROUNDS=9")
    check("A2 选项解析同时接受 rounds 与 gb7Rounds, 且越界会被夹住",
          '"rounds", "gb7Rounds"' in cpp and "GB7_DEFAULT_ROUNDS" in cpp and
          "GB7_MAX_ROUNDS" in cpp and cpp.count("if (rounds > GB7_MAX_ROUNDS)") >= 1,
          "gb7RoundsFromOptions: 两个键 -> 1..9, 缺省 = 默认值")
    check("A3 轮数不合法时不猜: 当没写, 退回默认值",
          "轮数不合法 -> 当没写, 用默认值" in cpp,
          "options 写了非法值 -> 用默认轮数(不替用户猜)")
    check("A4 napi 侧: runGb7 的第三个参数就是选项(老调用方只传两个参数照样工作)",
          "size_t argc = 3;" in napi and "w->options = options;" in napi and
          "gb7RoundsFromOptions(w->options" in napi,
          "runGb7(id, threads, options?)")
    # 负载函数每轮都用同一份参数调用 -> "做几次"而不是"改工作量"
    check("A5 每一轮都是同一个负载函数 + 同一份参数(做 N 次, 不是改大小)",
          "all.push_back(gb7RunTest(id, threads));" in cpp and
          "std::vector<Gb7Outcome> all;" in cpp,
          "gb7RunTestRepeated: for(rounds) { all.push_back(gb7RunTest(id, threads)); }")
    check("A6 报告里写明'轮数只影响同一项测量做几次, 单轮工作量未改动'",
          "loadUnchanged" in cpp and "轮数只影响'同一项测量做几次'" in cpp,
          "repeatability.loadUnchanged")

    emit("")
    emit("[B] 代表值一律取中位 —— 代码里不存在'取最好一轮'的路径")
    emit("=" * 100)
    check("B1 中位/最小/最大是三个独立的纯函数(medianOfList / minOfList / maxOfList)",
          "double medianOfList(" in cpp and "double minOfList(" in cpp and "double maxOfList(" in cpp,
          "gb7.cpp 匿名命名空间里的三个纯函数")
    check("B2 代表分 = 中位分(scoreMedian), 代表吞吐 = 中位吞吐",
          "o.score = o.scoreMedian;" in cpp and "o.scoreMedian = medianOfList(scores);" in cpp and
          "o.metricMedian = medianOfList(metrics);" in cpp,
          "o.score = o.scoreMedian")
    # 2026-10-08 加强: 这一条以前只要求"回写中位分"。真机上因此漏掉了一个更严重的 bug ——
    #   g_lastScore 当时只有负载 id 一个维度, 而多核阶段只跑 8 项、却会把这 8 项覆盖成多核分,
    #   于是报告里的"单核复合分"被污染(328.2 vs 561.1, 差 1.7 倍)。
    #   现在连阶段维度一起断言: 回写必须落在本阶段的槽位上。
    check("B3 复合分拿到的是中位分, 且按阶段分开存(单核那一遍不会被多核那一遍覆盖)",
          "g_lastScore[gb7StageOf(threads)][id] = o.score;" in cpp and
          "g_lastValid[gb7StageOf(threads)][id] = (o.score > 0.0);" in cpp and
          "double g_lastScore[GB7_STAGE_COUNT][ENTRY_COUNT];" in cpp and
          "inline int gb7StageOf(int threads)" in cpp,
          "回写 g_lastScore[阶段][id](阶段与 gb7TestKOfficial 的第二参数同源: threads > 1 => 多核)")
    # 关键否定断言: 没有任何一条把 max / 最后轮 当代表值的赋值路径
    bad_assign = re.findall(r"o\.score\s*=\s*[^;]*(?:Max|Last)", cpp) + \
                 re.findall(r"o\.metric\s*=\s*[^;]*Max", cpp) + \
                 re.findall(r"g_lastScore\[[^\]]*\]\s*=\s*[^;]*(?:Max|Last)", cpp) + \
                 re.findall(r"bestScore|bestRound|scoreBest", cpp)
    check("B4 没有任何一条'取最大 / 取最后一轮'的赋值路径", len(bad_assign) == 0,
          ("命中: " + str(bad_assign[:3])) if bad_assign else "0 处命中(max 只用于报区间)")
    check("B5 每个统计量的定义都写在报告里(代表值 = median, 离散度 = (max-min)/中位)",
          "representative" in cpp and "不是最好一轮" in cpp and
          "dispersionDefinition" in cpp and "(最大 - 最小) / 中位 x 100%" in cpp,
          "repeatability.representative / dispersionDefinition")

    emit("")
    emit("[C] 中位 / 最小 / 最大 / 相对离散度: 分数与吞吐各有四件套, 计时给三件套")
    emit("=" * 100)
    check("C1 分数统计四件套齐全(median / min / max / dispersion 都算出来并写进报告)",
          all(k in cpp for k in ["o.scoreMedian", "o.scoreMin", "o.scoreMax", "o.scoreSpreadPct"]) and
          cpp.count("o.scoreMedian = medianOfList(scores);") >= 1,
          "repeatability.score.{median,min,max,dispersion}")
    check("C2 吞吐(metric)同样有中位/最小/最大/离散度",
          all(k in cpp for k in ["o.metricMedian", "o.metricMin", "o.metricMax", "o.metricSpreadPct"]),
          "repeatability.metric.{median,min,max,dispersion}")
    check("C3 计时(ms)也给了中位/最小/最大(与 SNL 小节同风格)",
          all(k in cpp for k in ["o.msMedian", "o.msMin", "o.msMax"]) and
          "o.msMedian = medianOfList(times);" in cpp,
          "repeatability.ms.{median,min,max}")
    check("C4 相对离散度的定义与 GPU-SNL 小节一致((max-min)/中位 x 100%)",
          "return (maxOfList(v) - minOfList(v)) / med * 100.0;" in cpp,
          "spreadPctOf()")
    check("C5 每一项都会带上这一块(不是只给复合分)",
          "out += gb7RepeatabilityJson(r);" in napi,
          "ExecuteGb7Body 每项都拼 repeatability")

    emit("")
    emit("[D] 可信度判定 + 阈值是我们自己定的(不是官方阈值)")
    emit("=" * 100)
    check("D1 三档阈值显式写在头上(2% / 5%)且与 GPU-SNL 一致",
          re.search(r"#define\s+GB7_RELIABLE_SPREAD_PCT\s+2\.0", hdr) is not None and
          re.search(r"#define\s+GB7_FAIR_SPREAD_PCT\s+5\.0", hdr) is not None,
          "RELIABLE <= 2% / FAIR 2%~5% / UNRELIABLE > 5%")
    check("D2 verdict 四态齐全",
          all(k in cpp for k in ['"RELIABLE"', '"FAIR"', '"UNRELIABLE"', '"SINGLE_ROUND_NO_DATA"']),
          "verdictOfSpread()")
    check("D3 报告里明确写'阈值是我们自己定的, 不是官方阈值'",
          "thresholdsAreOursNotOfficial" in cpp and
          "阈值(2% / 5%)是我们自己定的, 不是官方阈值" in cpp,
          "credibility.thresholdsAreOursNotOfficial = true + 同一句话")
    check("D4 离散度超阈值时明确写'本项重复性差, 该数字仅供参考'",
          "本项重复性差, 该数字仅供参考" in cpp,
          "UNRELIABLE 的 text")
    check("D5 未计分项不会被'分数恒 0'误判成重复性好(按吞吐离散度判)",
          "spreadForVerdict" in cpp and "scoredItem ? o.scoreSpreadPct : o.metricSpreadPct" in cpp,
          "计分项看分数离散度 / 未计分项看吞吐离散度")

    emit("")
    emit("[E] 轮数只有 1 时: 写'没有离散度数据, 不能据此判断可信度'")
    emit("=" * 100)
    check("E1 单轮结论的原文就在代码里(且是 verdict, 不是一句含糊的话)",
          "本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度" in cpp and
          "SINGLE_ROUND_NO_DATA" in cpp,
          "verdictText(SINGLE_ROUND_NO_DATA)")
    check("E2 roundsOk < 2 时不许判可信度(verdictOfSpread 第一句就是这个判断)",
          re.search(r"if\s*\(roundsOk\s*<\s*2\)\s*\{\s*return\s+\"SINGLE_ROUND_NO_DATA\"", cpp) is not None,
          "verdictOfSpread: roundsOk < 2 -> SINGLE_ROUND_NO_DATA")
    check("E3 报告里另有一条单轮提醒字段(便于 UI 醒目显示)",
          "singleRoundWarning" in cpp and "本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度" in cpp,
          "repeatability.singleRoundWarning(okRounds < 2 时出现)")
    check("E4 所有轮次都没有有效读数时写'没有代表值'(不编数字)",
          "NO_VALID_ROUNDS" in cpp and "没有任何一轮给出有效吞吐读数" in cpp,
          "verdict = NO_VALID_ROUNDS")
    check("E5 运行失败时也有恒定的形状(verdict = RUN_FAILED, 不编离散度)",
          "RUN_FAILED" in napi and "没有任何轮次数据 —— 写空, 不编离散度" in napi,
          "gb7FailJson 里的 repeatability 形状")

    emit("")
    emit("[F] 计分口径与单轮负载代码一个字都没动(硬约束)")
    emit("=" * 100)
    check("F1 计分公式仍是 score = k x (value x conv)(逐字未变)",
          "score = k * (value * e.conv);" in cpp and
          "// 计分: 分数 = k x (metric x conv); 未计分项 / 非正吞吐一律 0" in cpp,
          "gb7RunTest 里的计分行原样保留")
    check("F2 k 的取法未变: 仍然用未乘 conv 的 kOfficial 再乘一次 conv",
          "const double k = gb7TestKOfficial(id, (threads > 1) ? 1 : 0);" in cpp,
          "避免换算被平方(历史真机 bug)")
    check("F3 metric 的解析口径未变(仍然是 sscanf 同一个数)",
          'sscanf(o.metric.c_str(), "%lf", &value)' in cpp and
          'sscanf(s.c_str(), "%lf", &v)' in cpp,
          "计分与统计用的是同一个吞吐")
    check("F4 代表值的 metric 文本取中位那一轮的原文(不重新格式化 metric)",
          "medianRoundIndex(all, o.metricMedian)" in cpp and "o.metric = all[(size_t)mid].metric;" in cpp,
          "不改变 metric 的展示格式")
    check("F5 存在自检: 代表分必须 == k x conv x 代表吞吐(对不上报 false)",
          "metricImpliedScore" in cpp and "scoreMatchesMedianMetric" in cpp and
          "metricImpliedScoreOk" in hdr,
          "repeatability.scoreMatchesMedianMetric")
    # 负载文件里不能出现重复性逻辑(轮数不允许改负载)
    leaked = []
    for fn in LOAD_FILES:
        src = rd(fn)
        for tok in ("gb7RunTestRepeated", "repeatability", "scoreMedian", "GB7_DEFAULT_ROUNDS",
                    "roundsDetail"):
            if tok in src:
                leaked.append(fn + ":" + tok)
    check("F6 9 个负载实现文件里没有任何重复性逻辑(轮数改不了负载)",
          len(leaked) == 0, ("泄漏: " + str(leaked[:5])) if leaked else "0 处泄漏")
    # 线程数与绑核策略未动
    check("F7 线程数/绑核策略未动: 每一轮仍走同一个 AuroraAffinityScope(threads)",
          "const AuroraAffinityScope affinityScope(threads);" in cpp and
          "const AuroraFreqScope freqScope(threads);" in cpp,
          "gb7RunTest 内部未改(多轮只是把它多调用几次)")

    emit("")
    emit("[G] 逐轮原始值全部列出 + 一键跑分耗时说明")
    emit("=" * 100)
    check("G1 逐轮明细数组含 round / score / metric / ms / cpu / runFreq",
          all(k in cpp for k in ['\\"round\\":', '\\"score\\":', '\\"metric\\":', '\\"ms\\":',
                                 '\\"cpu\\":', '\\"runFreq\\":']),
          "repeatability.roundsDetail[]")
    check("G2 明确声明'逐轮原始值全都列出来了'",
          "roundValuesAreAllListed" in cpp and "roundValuesAreAllListed" in cpp,
          "roundValuesAreAllListed = true")
    check("G3 逐轮明细条数 == 轮数(不截断, 不丢轮)",
          "for (size_t i = 0; i < o.roundScores.size(); ++i)" in cpp and
          "o.roundScores.push_back(all[i].score);" in cpp,
          "每一轮都 push 进明细")
    check("G4 默认 2 轮 = 一键跑分的 GB7 阶段耗时约为原来的 2 倍(注释里写清楚)",
          "默认轮数" in hdr and "GB7_DEFAULT_ROUNDS" in hdr,
          "默认 2 轮 -> GB7 单核 16 项 + 多核 8 项的耗时 x2(负载工作量不变)")
    check("G5 d.ts 里写明了选项与返回结构(接线不用猜)",
          "rounds?: string" in dts or "options?: string" in dts,
          "types/libaurorabench/index.d.ts")
    # ---- 静态 JSON 形状自检: repeatability 这一块的字符串字面量拼出来必须是配平的 ----
    # 口径: gb7RepeatabilityJson() 里的 JSON 全部由字符串字面量拼成(没有条件分支的
    #       花括号), 所以把字面量按 C++ 转义解开后, { } 与 [ ] 必须各自配平 ——
    #       少了任何一个括号, ArkTS 侧 JSON.parse 就会抛异常(工程里真实踩过这个坑)。
    m = re.search(r"std::string gb7RepeatabilityJson\(const Gb7Outcome& o\)[\s\S]*?\n\}", cpp)
    body = m.group(0) if m else ""
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    joined = "".join(lits).replace('\"', '"')   # C++ 的 \" -> "
    check("G6 repeatability 的 JSON 字面量是配平的({} 与 [] 各自成对, 不会让 JSON.parse 抛异常)",
          body != "" and joined.count("{") == joined.count("}") and
          joined.count("[") == joined.count("]") and joined.count("{") >= 2,
          "花括号 %d/%d, 方括号 %d/%d" % (joined.count("{"), joined.count("}"),
                                          joined.count("["), joined.count("]")))
    check("G7 轮数原样记录(要求几轮 / 实际几轮 / 来自选项还是默认值)",
          "o.rounds = want;" in cpp and "o.roundsRequested = (rounds < 1) ? 1 : rounds;" in cpp and
          "roundsSource" in cpp,
          "rounds / roundsRequested / roundsSource")

    emit("")
    emit("[H] 复合分也要有 中位 / 最小 / 最大 / 相对离散度 + 可信度(不是只有单项有)")
    emit("=" * 100)
    check("H1 复合分(单核/多核)也输出 repeatability 块(逐轮复合分 + 中位/最小/最大/离散度)",
          all(k in cpp for k in ['\\"compositePerRound\\":[', '\\"median\\":', '\\"min\\":', '\\"max\\":',
                                 '\\"dispersion\\":']),
          "compositeJson() -> repeatability{compositePerRound, median, min, max, dispersion}")
    check("H2 逐轮复合分是按轮次重算的几何平均: 某一轮只要缺一项读数就整轮不参与统计",
          "if (rn == n && n > 0)" in cpp and
          "某一轮只要有一项缺读数, 那一轮就不参与统计(不拿别的轮凑数)" in cpp,
          "逐轮重算 + 参与轮数报")
    check("H3 两个数都报出来, 不挑一个好看的(composite = 各项中位分的 GM; median = 逐轮复合分的中位)",
          "representative" in cpp and "由各项中位分算出的几何平均" in cpp and
          "逐轮复合分的中位" in cpp,
          "repeatability.representative")
    check("H4 复合分的可信度阈值同样标明'我们自己定的'",
          '\\"thresholdsAreOursNotOfficial\\":true' in cpp and
          "我们自己定的阈值(与单项、与 GPU-SNL 小节同一套), 不是官方阈值" in cpp,
          "复合分 credibility.thresholdsAreOursNotOfficial")
    check("H5 每一项只跑 1 轮时, 复合分也写'没有离散度数据, 不能据此判断可信度'",
          "本次每一项只跑 1 轮, 没有离散度数据, 不能据此判断复合分的可信度" in cpp,
          "复合分 singleRoundWarning")
    check("H6 逐轮单项分矩阵只用于统计, 不计分(且同样按阶段分开存)",
          "double g_roundScore[GB7_STAGE_COUNT][ENTRY_COUNT][GB7_MAX_ROUNDS];" in cpp and
          "只服务\"复合分的可重复性\"统计; 不计分" in cpp and
          "g_roundScore[stage][id][r] = all[(size_t)r].score;" in cpp,
          "g_roundScore[阶段][id][轮] 由 gb7RunTestRepeated 写入、只在 compositeJson 读出")

    emit("")
    emit("=" * 100)
    emit("共 %d 项断言, 失败 %d 项 -> %s" % (checks, len(fails), "PASS" if not fails else "FAIL"))
    emit("=" * 100)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
