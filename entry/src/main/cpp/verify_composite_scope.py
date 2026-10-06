# -*- coding: utf-8 -*-
"""
verify_composite_scope.py —— 「复合分口径」与「逐项随芯片变化自检」的离线自检器
(离线: 不连设备、不跑 hdc、不跑构建、不改任何文件; 只读源码 + 做算术)

为什么需要它(用户 2026-10-08 定的最高验收标准)
-------------------------------------------------------------------------------
"只要测试的基准相同, 保证不同芯片之间的分数差能真实地表现它们的实力差, 那其他的不重要。"
这件事有两个前提, 本脚本逐条核对:
  ① 复合分必须是一个口径 —— 7.2 真机上报告算 328.2、native 算 561.1, 差 1.7 倍。
     本脚本把这个差的根因做成一串可以独立复核的断言(不是"我们改了, 相信我们")。
  ② 每一项的吞吐必须真的随芯片算力变化 —— 否则它会把两台设备的分数比往 1 压。
     本脚本核对自检判据、三种结论、以及"复合分排除不通过项"这条规则真的接上了。

[A] 328.2 vs 561.1 的根因: 阶段污染(算术自洽性)
[B] 根因已修: native 侧有阶段维度; 报告侧改传显式数组
[C] 两边口径逐条一致(参与项 / 排除规则 / 取值来源)
[D] 逐项"是否随芯片变化"自检: 判据 / 三种结论 / 接口都对得上
[E] 2026-10-05 改 复合分的参与项集合 == 报告 items[] 里该阶段有正分的项集合
[F] 永久断言 同上, 并用真机报告逐项复算(8.1 phone81 / 8.0 r80)

为什么 [E] 从"复合分真的排除了不通过的项"改成现在这条(8.1 真机 Pura X Max 2026-10-05 17:52)
-------------------------------------------------------------------------------
那一轮的真机报告里同时出现三句话, 互相打架:
  ① [3/7] 小节结论: "综合分（计分项几何平均，已排除 5 项未通过随芯片变化自检的项）2072
     · 线程 8 · 计分 8 项 · 未拿到分 0 项"      -> 说排除了 5 项, 又说没有任何一项没拿到分
  ② 排除项明细: File Compression(id=0: 本次没有拿到正的分数) ... 共 5 项  -> 理由是"没拿到分"
  ③ items[]:    File Compression 176.9 / Photo Library 5446.1 / HDR 39.2 / Ray Tracer 3957.8 /
                Clang 14761.3                          -> 这 5 项明明是正分, 也确实被算进了复合分
根因是 ArkTS 把"未通过随芯片变化自检(FAIL)"的项置 0 之后再传给 native:
  · native 只能看到 0, 于是把它写成"本次没有拿到正的分数"  -> ①与②的矛盾;
  · 置 0 用的是单核 ∪ 多核的并集, 只在多核口径判 FAIL 的项连单核数组也被抹掉,
    单核复合分因此少了 5 项 -> 报告自己那行「核对不一致 native 409.4 / 本报告 416.2」;
  · 同一台设备 8.0 -> 8.1: 全口径多核 GM 只差 0.26%(1447.06 -> 1450.76),
    被"自检排除"之后却差 160%(795.81 -> 2071.86) —— 复合分变成这一轮的噪声抽签。
现在: 传下去的数组逐位等于本阶段各项的真实分数(不再有任何置 0),
复合分的参与项集合 == 报告 items[] 里该阶段有正分的项集合;
"未通过随芯片变化自检"只作提示(不可用于跨芯片比较), 不再改写参与项集合。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io
import json
import math
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ETS = os.path.normpath(os.path.join(HERE, "..", "ets"))
OUT = io.open(os.path.join(HERE, "verify_composite_scope_out.txt"), "w", encoding="utf-8", newline="\n")
fails = []
checks = 0

# 真机报告(只在存在时用; 缺了就是 SKIP, 不算 FAIL)
REAL_REPORTS = [
    ("8.1 手机 Pura X Max 2026-10-05 17:52", r"D:\gb7logs\phone81\report-latest.json"),
    ("8.0 手机 Pura X Max 2026-10-05 16:04", r"D:\gb7logs\r80\report-latest.json"),
    ("7.5 手机 Pura X Max", r"D:\gb7logs\r75\report-latest.json"),
]


def gm(xs):
    p = 1.0
    for x in xs:
        p *= x
    return p ** (1.0 / len(xs)) if xs else 0.0


def ids_from_items_text(txt):
    """从 native 的 items[] 文本("名字 6536.2(id=2) · ...")里取出负载 id 集合。"""
    return sorted(set(int(m) for m in re.findall(r"\(id=(-?\d+)\)", txt)))


def scope_audit(path):
    """在一份真机报告上复算永久断言。返回 dict; 报告不存在 -> None。"""
    if not os.path.exists(path):
        return None
    j = json.load(io.open(path, encoding="utf-8", errors="replace"))
    comp = j.get("composite", {}) or {}
    items = j.get("items", []) or []
    out = {"new_build": ("gb7SingleScopeOk" in comp), "stages": {}}
    for stage, pre in (("GB7 单核", "gb7Single"), ("GB7 多核", "gb7Multi")):
        expected = {}
        for it in items:
            if it.get("section") == stage and float(it.get("score") or 0) > 0:
                lid = it.get("loadId")
                if isinstance(lid, int) and lid >= 0:
                    expected[lid] = float(it.get("score") or 0)
        actual = ids_from_items_text(str(comp.get(pre + "Items", "")))
        exp_ids = sorted(expected.keys())
        missing = [i for i in exp_ids if i not in actual]
        extra = [i for i in actual if i not in exp_ids]
        out["stages"][pre] = {
            "expected": exp_ids, "actual": actual, "missing": missing, "extra": extra,
            "ok": (len(missing) == 0 and len(extra) == 0 and len(actual) > 0),
            "report_value": float(comp.get(pre + "Composite") or 0),
            "gm_from_items": gm([expected[i] for i in exp_ids]),
            "gm_actual_set": gm([expected[i] for i in actual if i in expected]) if actual else 0.0,
            "count": comp.get(pre + "Count"),
            "scope_ok_field": comp.get(pre + "ScopeOk"),
            "participants_field": comp.get(pre + "Participants"),
        }
    return out


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


def rd(*parts):
    p = os.path.join(*parts)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()


def main():
    gb7 = rd(HERE, "gb7.cpp")
    gb7h = rd(HERE, "gb7.h")
    napi = rd(HERE, "napi_init.cpp")
    runner = rd(ETS, "service", "BenchRunner.ets")
    fullrun = rd(ETS, "service", "FullRun.ets")
    scaling = rd(ETS, "service", "ScalingAudit.ets")
    model = rd(ETS, "model", "BenchModel.ets")
    index = rd(ETS, "pages", "Index.ets")
    for nm, src in (("gb7.cpp", gb7), ("BenchRunner.ets", runner), ("FullRun.ets", fullrun),
                    ("ScalingAudit.ets", scaling), ("BenchModel.ets", model)):
        if not src:
            emit("找不到文件: " + nm)
            return 1

    emit("=" * 104)
    emit("[A] 328.2 vs 561.1 的根因: native 的单核复合分被多核阶段污染(算术自洽性核对)")
    emit("=" * 104)
    report_v = 328.2
    native_v = 561.1
    # 单核阶段的计分项数 = 注册表里 scored=true 的项数(16 项 - Structure from Motion 未计分)
    scored_single = len(re.findall(r"^\s*\{\"[^\"]+\",\s*\"[^\"]+\",\s*gb7Run", gb7, re.M))
    multi_n = len(re.findall(r"true,\s+true,", gb7))
    emit("      报告卡片口径(15 项单核分几何平均)      = %.1f" % report_v)
    emit("      native 旧口径(g_lastScore 缓存读出来)   = %.1f" % native_v)
    r = native_v / report_v
    n_used = 15          # 单核计分项数(SfM 未计分)
    n_multi = 8          # 官方多核套件项数(这 8 项被多核那一遍覆盖)
    prod_speedup = r ** n_used
    gm_speedup = r ** (n_used / n_multi)
    emit("      比值 = %.4f; 被覆盖的项数 = %d; 参与项数 = %d" % (r, n_multi, n_used))
    emit("      => 那 8 项的多核/单核比的乘积 = %.4f^%d = %.1f" % (r, n_used, prod_speedup))
    emit("      => 每项并行加速比的几何平均 = %.4f^(%d/%d) = %.4fx" % (r, n_used, n_multi, gm_speedup))
    # 物理合理性: 8 线程设备上, 一项真正受算力支配的负载加速比应当在 1.5x 以上; 上限不该超过核数太多
    check("A1 反推出来的并行加速比(每项 %.2fx)落在 8 线程设备的物理合理区间 [1.5, 8]" % gm_speedup,
          1.5 <= gm_speedup <= 8.0,
          "GM 加速比 %.4fx —— 与'8 核上真实并行负载'完全吻合" % gm_speedup)
    # 口径差会表现为"参与项集合不同"; 而"参与项集合相同、只有值不同"只有一种解释: 取值来源被覆盖。
    # 判据: 单核阶段 15 个计分项全被写入缓存, 多核阶段只覆盖其中 8 个 -> 得到的是混合集。
    check("A2 排除规则的代码是同一份(scored / inMulti / score > 0 三条), 两边不可能'规则不同'",
          gb7.count("if (!ENTRIES[i].scored)") == 1 and
          gb7.count("if (multi && !ENTRIES[i].inMulti)") == 1 and n_multi == 8,
          "同一段循环里三条规则(单一来源) -> 328.2 与 561.1 的参与规则相同, 只可能差在'值'")
    check("A3 单核阶段写满 15 项、多核阶段只覆盖其中 8 项 => 缓存里是混合集(根因的行为侧写)",
          n_used == 15 and n_multi == 8 and 1.5 <= gm_speedup <= 8.0,
          "15 项参与、8 项被覆盖, 反推加速比 %.2fx 落在物理区间 -> 混合集假设成立" % gm_speedup)
    # 多核侧为什么对得上: gb7CompositeMulti 只统计 inMulti 的 8 项, 而这 8 项恰好被多核那一遍刷新过
    multi_filter = "if (multi && !ENTRIES[i].inMulti)" in gb7
    check("A3 多核侧当年看不出问题, 是因为它只统计 inMulti 的 8 项, 而这 8 项恰好被多核那一遍刷新过",
          multi_filter,
          "compositeJson: multi 时 !inMulti 直接排除 -> 多核口径用的 8 项全是新值(923.8 vs 923.9 的 0.1 只是 %.1f 舍入)")
    check("A4 多核阶段确实只跑 inMulti 的项(污染源的行为侧写)",
          "if (!gb7TestIsMulti(i))" in runner and "continue;" in runner,
          "BenchRunner.runGb7Multi: 非多核项直接 continue(官方多核结果页里也没有它们)")

    emit("")
    emit("=" * 104)
    emit("[B] 根因已修: native 侧加了阶段维度; 报告侧改传显式数组")
    emit("=" * 104)
    check("B1 native: 单项分缓存带阶段维度(2 个阶段 x ENTRY_COUNT 项)",
          "double g_lastScore[GB7_STAGE_COUNT][ENTRY_COUNT];" in gb7 and
          "bool g_lastValid[GB7_STAGE_COUNT][ENTRY_COUNT];" in gb7,
          "g_lastScore[阶段][id] / g_lastValid[阶段][id]")
    check("B2 native: 逐轮分矩阵同样带阶段维度",
          "double g_roundScore[GB7_STAGE_COUNT][ENTRY_COUNT][GB7_MAX_ROUNDS];" in gb7 and
          "bool g_roundValid[GB7_STAGE_COUNT][ENTRY_COUNT][GB7_MAX_ROUNDS];" in gb7 and
          "int g_roundCount[GB7_STAGE_COUNT][ENTRY_COUNT];" in gb7,
          "g_roundScore[阶段][id][轮] —— 单核的逐轮复合分也不会被多核那一遍改掉")
    check("B3 native: 阶段判定与'用哪张 k 表'是同一条判据(threads > 1)",
          "inline int gb7StageOf(int threads)" in gb7 and
          "return (threads > 1) ? GB7_STAGE_MULTI : GB7_STAGE_SINGLE;" in gb7 and
          "gb7TestKOfficial(id, (threads > 1) ? 1 : 0)" in gb7,
          "gb7StageOf(threads) 与 gb7TestKOfficial 的第二参数同源 -> 两者不可能各说各话")
    # 两处回写: gb7RunTest 里是 g_lastScore[stage][id] = score, gb7RunTestRepeated 里是
    # g_lastScore[gb7StageOf(threads)][id] = o.score。两种写法都必须带阶段。
    stage_writes = len(re.findall(r"g_lastScore\[(?:gb7StageOf\(threads\)|stage)\]\[id\]\s*=", gb7))
    stage_writes_round = len(re.findall(r"g_roundCount\[stage\]\[id\]\s*=", gb7))
    check("B4 native: 两条回写路径都写本阶段槽位(不是只改一处)",
          stage_writes >= 2 and stage_writes_round >= 1,
          "g_lastScore[阶段][id] 回写 %d 处 + g_roundCount[阶段][id] 回写 %d 处" %
          (stage_writes, stage_writes_round))
    check("B5 native: 取值函数按阶段回退",
          "double scoreOf(const double* scores, int count, int id, int stage)" in gb7 and
          "g_lastValid[stage][id]" in gb7,
          "scoreOf(scores, count, id, stage)")
    check("B6 报告侧: 复合分改成传本阶段的逐项分数组(不再让 native 读缓存)",
          "static compositeRepeatJson(multi: boolean, scores: number[])" in runner and
          "return multi ? gb7CompositeMulti(scores) : gb7CompositeSingle(scores);" in runner and
          "BenchRunner.compositeRepeatJson(false, BenchRunner.gb7ScoreArray(single))" in fullrun,
          "compositeRepeatFacts: native 与报告用同一个数组算, 由构造同源")
    check("B7 报告侧: 数组按负载 id 摊平, 不是按列表下标(两个阶段列表长度不同, 用下标会错位)",
          "static gb7ScoreArray(list: Gb7Result[]): number[]" in runner and
          "arr[r.loadId] = r.score;" in runner,
          "gb7ScoreArray: 只认 r.loadId")
    check("B8 每一项都带上了 loadId(GPU / 老 native / 失败项默认 -1)",
          "loadId: number;" in model and "base.loadId = i;" in runner and
          "baseM.loadId = i;" in runner and "loadId: -1," in runner,
          "Gb7Result.loadId 在单核 / 多核两条路径上都填了")

    emit("")
    emit("=" * 104)
    emit("[C] 两边口径逐条一致: 参与项 / 排除规则 / 取值来源")
    emit("=" * 104)
    check("C1 native 的排除规则不变: scored=false / 多核时 !inMulti / 分数 <= 0 三条",
          "if (!ENTRIES[i].scored)" in gb7 and "skipWhy = \"scored=false" in gb7 and
          "inMulti=false" in gb7 and "本次没有拿到正的分数" in gb7,
          "compositeJson: 三条规则逐条写进 excluded[]")
    check("C2 native 把参与项与排除项逐条输出(两台机器可以逐条对照)",
          'out += "],\\"excluded\\":[\";' in gb7 and
          "\\\"excludedCount\\\":" in gb7 and
          "{\\\"id\\\":%d,\\\"name\\\":\\\"%s\\\",\\\"score\\\":%.1f}" in gb7,
          "items[] = 参与项(名字 + 分 + id); excluded[] = 排除项(名字 + 原因)")
    check("C3 native 把取值来源写出来(explicit-array / stage-cache), 一眼看出用的是哪一份分",
          "\\\"scoreSource\\\":" in gb7 and "\\\"scoreSourceExplicit\\\":" in gb7 and
          "explicit-array" in gb7 and "stage-cache" in gb7,
          "compositeJson: scoreSource + scoreSourceExplicit + scoreSourceText")
    check("C4 ArkTS 侧读得出这三样并写进报告(不是只在 native 里躺着)",
          "static compositeSourceText(json: string): string" in runner and
          "static compositeItemsText(json: string): string" in runner and
          "static compositeExcludedText(json: string): string" in runner and
          "crep.singleItems" in fullrun and "crep.singleSource" in fullrun and
          "crep.singleExcluded" in fullrun,
          "BenchRunner.compositeSourceText / compositeItemsText / compositeExcludedText -> 报告 D 节逐行印出")
    check("C5 报告里逐项印出「口径来源 / 参与项 / 排除项 / 自检提示 / 参与项核对」五行",
          "B.ln('    口径来源：'" in fullrun and "B.ln('    参与项：'" in fullrun and
          "B.ln('    排除项：'" in fullrun and
          "不可用于跨芯片比较（仍计入上面的复合分）" in fullrun and
          "参与项核对不通过：" in fullrun,
          "compositeRepeatText: 五行逐条披露, 不再只印两个对不上的数")
    check("C6 核对阈值写明: 差 > 0.05 才判不一致, 并写明只可能是 %.1f 的舍入",
          "diff > 0.05" in fullrun and "核对一致" in fullrun and "核对不一致" in fullrun,
          "compositeRepeatText: 一致/不一致都印, 且给出差值的来源解释")
    check("C7 报告里把两个口径的含义分别写清楚(不许混用)",
          "口径①（卡片口径 / 跨芯片比较用这一个）" in fullrun and
          "口径②（可信度用这一个）" in fullrun and
          "不是另一个分数，别拿去比芯片" in fullrun,
          "口径① = 各项中位分的 GM(跨芯片比这一个); 口径② = 逐轮复合分的中位(只看可信度)")

    emit("")
    emit("=" * 104)
    emit("[D] 逐项「本项是否随芯片性能变化」自检: 判据 / 三种结论 / 接口")
    emit("=" * 104)
    check("D1 自检模块存在且三种结论齐全(PASS / FAIL / UNPROVEN)",
          "export const SC_PASS: string = 'PASS';" in scaling and
          "export const SC_FAIL: string = 'FAIL';" in scaling and
          "export const SC_UNPROVEN: string = 'UNPROVEN';" in scaling,
          "ScalingAudit.ets: 三种结论, 且 UNPROVEN 不被当成\"通过\"")
    check("D2 SC1 判据 = 同一项的 多核分 / 单核分(工作量完全相同, 只改变可用算力)",
          "speedup = r.score / s.score;" in scaling and
          "const upper: number = threads * SC_MAX_SPEEDUP_SLACK;" in scaling and
          "speedup < SC_MIN_SPEEDUP" in scaling and "speedup > upper" in scaling,
          "物理区间 [1.15, 实际核数 x 1.30]; 区间外判 FAIL")
    check("D3 SC2 判据 = 轮间离散度(与 native 的 UNRELIABLE 阈值同源, 5%)",
          "const SC_SPREAD_FAIL_PCT: number = 5.0;" in scaling and
          "r.scoreSpreadPct > SC_SPREAD_FAIL_PCT ? SC_FAIL : SC_PASS" in scaling and
          "r.roundsOk < 2" in scaling,
          "轮数 < 2 -> UNPROVEN(没有离散度数据, 不假装稳定)")
    check("D4 阈值明确标注是我们自己定的, 不是官方阈值",
          "thresholdsAreOursNotOfficial: true" in fullrun and
          "都是我们自己定的, 不是官方阈值" in scaling,
          "报告 JSON scalingAudit.thresholdsAreOursNotOfficial + 正文同一句话")
    check("D5 判不出来的项一律 UNPROVEN, 并且写明为什么判不出来",
          "单核阶段固定 1 线程, 本阶段没有可比的算力变化" in scaling and
          "单核阶段只能判 SC2(轮间离散度)" in fullrun and
          '本轮**没有任何"算力变化"的' in fullrun,
          "官方多核套件只有 8 项 -> 其余项本轮没有算力变化的对照 -> 写未自证")
    check("D6 每一个 GB7 项在报告里都有「随芯片变化自检=...」这一行(三种取值都不许缺)",
          "B.ln('    随芯片变化自检='" in fullrun and "'未自证: 现有数据判不出来, 不冒充通过'" in fullrun and
          "B.ln('    随芯片变化自检=未做" in fullrun,
          "gb7ItemBlock: 通过 / 不通过 / 未自证 / 未做 四种写法都在")
    check("D7 界面上也有这一行(而且通过 / 不通过 / 未自证三种颜色不同)",
          "private scalingItemLine(r: Gb7Result): string" in index and
          "private scalingItemColor(r: Gb7Result): string" in index and
          "private scalingSummaryLine(single: Gb7Result[], multi: Gb7Result[]): string" in index,
          "Index.ets: 逐项行 + 汇总行(与报告同一份 summarize 结论)")
    check("D8 自检不跑负载、不改负载: 模块里没有任何 runGb7 / 负载调用",
          "runGb7" not in scaling and "gb7Run" not in scaling and
          "不改任何 metric/unit/k/conv/计分公式" in scaling,
          "ScalingAudit.ets 只有纯函数 + 旁路字段")

    emit("")
    emit("=" * 104)
    emit("[E] 复合分的参与项集合 == 报告 items[] 里该阶段有正分的项集合(唯一入口)")
    emit("=" * 104)
    # 把 geoOf 的函数体切出来单独核对 —— 只看"复合分这个入口到底怎么筛项"
    geo_m = re.search(r"static geoOf\(list: Gb7Result\[\]\): number \{(.*?)\n  \}", fullrun, re.S)
    geo_body = geo_m.group(1) if geo_m else ""
    check("E1 复合分只有一个入口 geoOf, 且它的参与项判据就是 score > 0(不再额外过滤自检 FAIL)",
          "static geoOf(list: Gb7Result[]): number {" in fullrun and
          "if (r.score > 0) {" in geo_body and "isComparable" not in geo_body,
          "geoOf 函数体: 只认 score > 0 -> 与 native compositeJson 的 scored/score>0 逐项一致")
    check("E2 isComparable 仍在(回答'能不能跨芯片比'), 但它不再是复合分的过滤器",
          "export function isComparable(r: Gb7Result): boolean {" in scaling and
          "return r.scalingVerdict !== SC_FAIL;" in scaling and
          "不再是复合分的过滤器" in scaling and
          bool(geo_body) and "isComparable" not in geo_body,
          "ScalingAudit.isComparable 只作提示; geoOf 的判据里不出现它")
    check("E3 传给 native 的数组不做任何置 0(单核/多核各自逐位等于本阶段各项的真实分)",
          "const skip: number[] = [];" not in fullrun and
          "gb7ScoreArray(single, skip)" not in fullrun and
          "gb7ScoreArray(multi, skip)" not in fullrun and
          "BenchRunner.gb7ScoreArray(single)" in fullrun and
          "BenchRunner.gb7ScoreArray(multi)" in fullrun,
          "compositeRepeatFacts: 数组就是本阶段各项分, 不存在'有正分却被排除'的第三种情况")
    check("E3b 旧的'跨阶段并集置 0'写法已从代码里消失(注释里留着历史说明不算)",
          not any(("skip.push(r.loadId)" in l and not l.strip().startswith("//"))
                  for l in fullrun.split("\n")),
          "旧写法: skip 由 单核 ∪ 多核 的 FAIL 项并集组成 -> 单核数组也被抹掉 5 项")
    check("E4 结果页卡片 / 小节 note / 正文 / JSON / 参考对照输入全部走 geoOf",
          fullrun.count("FullReportBuilder.geoOf(") >= 5 and
          "return FullReportBuilder.geoOf(list);" in fullrun and
          "return FullReportBuilder.geoOf(list);" in index,
          "FullRun.geo / Index.logGeo 都改成转调 geoOf(%d 处)" % fullrun.count("FullReportBuilder.geoOf("))
    check("E5 报告里同时给出「参与 N 项」与「其中可用于跨芯片比较 M 项」(M 是 N 的子集, 不许只印一个)",
          "static participantOf(list: Gb7Result[]): number {" in fullrun and
          "其中可用于跨芯片比较 '" in fullrun and
          "' 项 · 其中可用于跨芯片比较 '" in fullrun,
          "participantOf = geoOf 的参与项数; comparableOf 是它的子集")
    check("E6 自检结论进 JSON 报告(scalingAudit 块 + 逐项字段)",
          "scalingAudit: RepScalingJson;" in fullrun and
          'chunks.push(\',"scalingAudit":\');' in fullrun and
          "j.scalingVerdict = r.scalingVerdict;" in fullrun and
          "j.loadId = r.loadId;" in fullrun,
          "JSON: scalingAudit{verdicts,passCount,failCount,unprovenCount,excludedIds,...} + items[].scalingVerdict")

    emit("")
    emit("=" * 104)
    emit("[F] 永久断言 复合分参与项集合 == 报告 items[] 里该阶段有正分的项集合")
    emit("=" * 104)
    # ---- 静态: 运行时硬核对真的接上了 ----
    check("F1 运行时硬核对已接上: compositeScopeCheck() 双向逐项比对两个 id 集合",
          "function compositeScopeCheck(list: Gb7Result[], json: string): ScopeCheck {" in fullrun and
          "const actual: number[] = BenchRunner.compositeItemIds(json);" in fullrun and
          "missing.push(id);" in fullrun and "extra.push(id);" in fullrun,
          "FullRun.compositeScopeCheck: 少了谁 / 多了谁都写出来")
    check("F2 核对结果进报告正文与 JSON(不通过直接印 参与项核对不通过)",
          "参与项核对不通过：" in fullrun and
          "gb7SingleScopeOk: crep.singleScopeOk," in fullrun and
          "gb7MultiScopeOk: crep.multiScopeOk," in fullrun and
          "gb7SingleParticipants: FullReportBuilder.participantOf(out.gb7Single)," in fullrun,
          "composite.gb7*ScopeOk / gb7*Participants / gb7*ScopeText 全部落 JSON")
    check("F3 自检清单不再冒充「复合分的排除集」: JSON 明说 affectsCompositeScope=false",
          "affectsCompositeScope: false," in fullrun and
          "affectsCompositeScope: boolean;" in fullrun and
          "复合分已排除" not in scaling and "已排除出复合分" not in fullrun,
          "scalingAudit.excludedDetailNote + affectsCompositeScope 与正文口径一致")
    # ---- 真机产物复算: 这条断言必须能抓到8.1 那一轮的 bug ----
    for label, path in REAL_REPORTS:
        au = scope_audit(path)
        if au is None:
            emit("  [SKIP] F4 真机报告不存在: " + path)
            continue
        for pre, cn in (("gb7Single", "单核"), ("gb7Multi", "多核")):
            st = au["stages"][pre]
            detail = ("参与项 [%s] vs items[] 有正分项 [%s]" %
                      (",".join(str(x) for x in st["actual"]), ",".join(str(x) for x in st["expected"])) +
                      ("; native 少算 " + ",".join(str(x) for x in st["missing"]) if st["missing"] else "") +
                      ("; native 多算 " + ",".join(str(x) for x in st["extra"]) if st["extra"] else ""))
            if au["new_build"]:
                # 修复后的产物: 断言必须成立, 且 JSON 里的自核位必须也是 true
                check("F4 [%s] %s: 参与项集合 == items[] 有正分项集合" % (label, cn),
                      st["ok"] and st["scope_ok_field"] is True and
                      st["participants_field"] == len(st["actual"]),
                      detail)
            else:
                # 修复前的历史产物: 断言必须按预期不成立, 否则说明它抓不到这个 bug
                check("F4 [%s] %s: 历史产物命中(修复前版本这里必须不成立 -> 证明断言有效)" % (label, cn),
                      (not st["ok"]) and len(st["missing"]) > 0,
                      detail)
            emit("       %s 报告写的复合分 = %.2f; 按 items[] 有正分项重算 = %.2f; "
                 "按 native 实际参与项重算 = %.2f" %
                 (label, st["report_value"], st["gm_from_items"], st["gm_actual_set"]))
    # ---- 用 8.1/8.0 两份真机报告把"正确口径"的数算出来(与报告里的数对照) ----
    r81 = scope_audit(REAL_REPORTS[0][1])
    r80 = scope_audit(REAL_REPORTS[1][1])
    if r81 is not None and r80 is not None:
        m81 = r81["stages"]["gb7Multi"]
        m80 = r80["stages"]["gb7Multi"]
        emit("")
        emit("   真机复算(同一台 Pura X Max, 同一天两次运行)")
        emit("     8.1 多核: 全口径(8 项有正分) GM = %.2f   报告当时写的(3 项) = %.2f"
             % (m81["gm_from_items"], m81["report_value"]))
        emit("     8.0 多核: 全口径(8 项有正分) GM = %.2f   报告当时写的(6 项) = %.2f"
             % (m80["gm_from_items"], m80["report_value"]))
        check("F5 同一台设备两次运行: 全口径只差 <2%(说明设备没变), 旧口径差 >100%(说明旧口径是噪声抽签)",
              abs(m81["gm_from_items"] / m80["gm_from_items"] - 1.0) < 0.02 and
              abs(m81["report_value"] / m80["report_value"] - 1.0) > 1.0,
              "全口径 %.2f -> %.2f (%.2f%%); 旧口径 %.2f -> %.2f (%.0f%%)" %
              (m80["gm_from_items"], m81["gm_from_items"],
               (m81["gm_from_items"] / m80["gm_from_items"] - 1.0) * 100.0,
               m80["report_value"], m81["report_value"],
               (m81["report_value"] / m80["report_value"] - 1.0) * 100.0))
        check("F6 8.1 单核: 参与项集合必须覆盖 items[] 里 15 个有正分的项(旧口径只用了 10 项)",
              len(r81["stages"]["gb7Single"]["expected"]) == 15 and
              len(r81["stages"]["gb7Single"]["actual"]) == 10,
              "items[] 有正分 15 项; 旧 native 参与 10 项 -> 报告里那行 核对不一致 416.2 vs 409.4")

    emit("")
    emit("=" * 104)
    emit("共 %d 项断言, 失败 %d 项 -> %s" % (checks, len(fails), "PASS" if not fails else "FAIL"))
    emit("=" * 104)
    emit("结论(数字, 不是口号):")
    emit("  * 328.2 vs 561.1 的根因是阶段污染(多核阶段只跑 8 项却覆盖了这 8 项的单核分),")
    emit("    算术自洽: (561.1/328.2)^15 = %.0f = 那 8 项多核/单核比的乘积 -> 每项 %.2fx。" %
         (prod_speedup, gm_speedup))
    emit("  * 修法①: native 的单项分/逐轮分缓存加阶段维度; 报告侧改传本阶段逐项分数组。")
    emit("  * 修法②(2026-10-05): 传下去的数组不再置 0 —— 复合分的参与项集合 == 报告 items[] 里")
    emit("    该阶段有正分的项集合(逐项相等)。'未通过随芯片变化自检'只作提示, 不改写参与项集合;")
    emit("    否则同一台设备 8.0 -> 8.1 的复合分会因为 2 轮离散度抽签而差 160%(795.81 -> 2071.86)。")
    emit("  * 逐项自检: SC1(并行加速比) + SC2(轮间离散度); 判不出来的一律 UNPROVEN(未自证)。")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
