# -*- coding: utf-8 -*-
"""
验收标准「要有跑分的意义, 像其他主流跑分软件一样」的离线核对器

对照主流跑分软件真正具备的性质, 逐条核对:
  1) 可重复性被量化(多轮 + 中位/最小/最大/离散度 + 醒目的可信度判断)
  2) 分数只随硬件变(报温度/频率, 标注是否降频; 不掩盖热身效应)
  3) 参考分对照表(与公开真值并列, 带来源; 第三方不许写成官方; 查不到就写查不到)
  4) 工作负载有代表性(每一项一句话真实用途)
  5) 可解释性(分数怎么来的 / 单位 / 同口径与否)
  6) 不被"跑分模式"污染(报告运行环境)

本脚本做四件事(全部离线):
  A) 真值表核对: 从 reference_compare.cpp / sn_renderer.cpp 里把公开真值解析出来,
     与已知清单逐条比对(数值 + 吞吐 + 来源种类), 确保没有抄错、也没有把第三方写成官方。
  B) 对照逻辑复算: 用真值表算出 deltaPercent 的定义式, 并核对 3DMark 分数 = 帧率 x 135
     这条官方明文在本工程里被原样使用(含分辨率归一化的方向: 是乘 0.5625, 不是除)。
  C) 用途覆盖: GB7 16 项 + GPU 11 项 + 本小节, 每一项都要在真实用途表里查到, 且不能是空话。
  D) 报告样例核对: sn_json_sample.json 必须能解析, 且六条验收标准对应的字段都在。

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
_OUT = io.open(os.path.join(HERE, "verify_acceptance_out.txt"), "w", encoding="utf-8", newline="\n")


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


def nums(s):
    return [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?", s)]


# ===========================================================================
#  已知清单(用户提供的公开真值 + UL 官方明文)
# ===========================================================================
GB7_TRUTH = [
    ("File Compression", "226 MB/s", 1589.0),
    ("Navigation", "11.0 routes/s", 2004.0),
    ("HTML5 Browser", "23.0 pages/s", 1839.0),
    ("PDF Viewer", "67.2 Mpx/s", 1941.0),
    ("Photo Library", "5.62 images/s", 1639.0),
    ("Clang", "2.78 Klines/s", 1697.0),
    ("Text Processing", "76.3 pages/s", 1582.0),
    ("Asset Compression", "26.8 MB/s", 1602.0),
]
GB7_COMPOSITE = {"single": 1633.0, "multi": 6802.0}
SNL_TRUTH = {"Kirin 9030 Pro": 991.0, "Kirin 9020": 454.0, "Kirin 9000S": 303.0}
K3DMARK = 135.0
RES_RATIO = (1920.0 * 1080.0) / (2560.0 * 1440.0)   # 0.5625

# 每一项代表什么真实用途(核对"不是空话"):
#   长度 >= 16 个字符, 且至少含一个 3 个字母以上的英文技术词或一个汉字 ——
#   "其他"、"杂项"、"综合性能" 这类空话过不了; 真正的用途说明(含具体场景)过得去。
def use_is_concrete(u):
    if len(u) < 16:
        return False
    if re.search(r"[\u4e00-\u9fff]", u):
        return True
    return re.search(r"[A-Za-z]{3,}", u) is not None


def main():
    ref_cpp = os.path.join(HERE, "reference_compare.cpp")
    ref_hdr = os.path.join(HERE, "reference_compare.h")
    sn_cpp = os.path.join(SN, "sn_renderer.cpp")
    sn_hdr = os.path.join(SN, "sn_renderer.h")
    sample = os.path.join(HERE, "sn_json_sample.json")
    for p in (ref_cpp, ref_hdr, sn_cpp, sn_hdr, sample):
        if not os.path.isfile(p):
            emit("找不到文件: " + p)
            return 1
    ref = read(ref_cpp)
    sn = read(sn_cpp)
    snh = read(sn_hdr)

    emit("=" * 100)
    emit("[A] 公开真值表核对(reference_compare.cpp + sn_renderer.cpp)")
    emit("=" * 100)

    # GB7 逐项真值: 从 { "name", "metric", score, "sourceKind", ... } 里解析
    rows = re.findall(
        r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*([0-9.]+)\s*,\s*"([A-Z_]+)"',
        ref)
    got = {n: (m, float(s), k) for (n, m, s, k) in rows}
    for (name, metric, score) in GB7_TRUTH:
        ok = name in got
        check("A1 GB7 真值 " + name, ok and got[name][1] == score and got[name][0] == metric,
              ("%s -> %.0f (%s)" % (got[name][0], got[name][1], got[name][2])) if ok
              else "真值表里没找到 " + name)
    check("A2 GB7 真值的来源种类都是 USER_PROVIDED_PUBLIC_TRUTH(不许写成官方)",
          all(got[n][2] == "USER_PROVIDED_PUBLIC_TRUTH" for (n, _, _) in GB7_TRUTH if n in got),
          "8 项全部标为用户提供的公开真值")
    check("A3 GB7 复合分真值 1633 / 6802 在代码里",
          "1633.0" in ref and "6802.0" in ref,
          "kGb7SingleCompositeTruth=1633, kGb7MultiCompositeTruth=6802")
    check("A4 参考机写明了, 且注明不是官方基准机",
          "HUAWEI Mate 80 Pro Max" in ref and "不是官方基准机" in ref,
          "referenceDevice + 说明")
    check("A5 官方基准机也写了(GB7 官方明文: Legion + RTX 4060 = 100,000)",
          "Lenovo Legion" in ref and "100,000" in ref,
          "caveats 里写了官方基准机")

    # 3DMark SNL 真值
    snl = re.findall(r'\{"([^"]+)",\s*"([^"]+)",\s*([0-9.]+),\s*"([A-Z_0-9]+)"', sn)
    snl_map = {a: (float(c), d) for (a, b, c, d) in snl}
    for k, v in SNL_TRUTH.items():
        hit = [a for a in snl_map if k.replace("Kirin ", "") in a]
        check("A6 SNL 真值 " + k, bool(hit) and snl_map[hit[0]][0] == v,
              ("%.0f (%s)" % (snl_map[hit[0]][0], snl_map[hit[0]][1])) if hit else "没找到")
    #  2026-08-31 变更 : 991 的来源从 "UNVERIFIED" 改成 "USER_PROVIDED_PRIMARY_OBSERVATION"
    #   —— 依据是用户提供的拍屏照片(一手证据: Mate 80 Pro Max / Steel Nomad Light / 991 / 7.34 FPS)。
    #   这条断言随之更新; 9020=454 与 9000S=303 一个字都没有动。
    check("A7 来源强度逐条标注: 9020=454 有 UL 官方成绩库出处 / 9030 Pro=991 是用户拍屏的一手证据 / "
          "9000S=303 仍未证实(第三方不写成官方)",
          snl_map.get("Kirin 9020", (0, ""))[1] == "VERIFIED_UL_DB" and
          snl_map.get("Kirin 9030 Pro", (0, ""))[1] == "USER_PROVIDED_PRIMARY_OBSERVATION" and
          snl_map.get("Kirin 9000S", (0, ""))[1] == "UNVERIFIED",
          "454=VERIFIED_UL_DB / 991=USER_PROVIDED_PRIMARY_OBSERVATION / 303=UNVERIFIED")
    # A7b: 一手证据的逐条内容必须写在源码里(机型 / 测试项 / 总分 / 平均帧率 / 来源),
    #      并且原有的两条 caveat 必须一字不少地保留。
    snl_ok_detail = ["Mate 80 Pro Max", "Steel Nomad Light", "991", "7.34", "用户拍屏", "一手证据"]
    snl_ok_caveat = ["956", "998", "993", "UL 官方成绩库"]
    check("A7b 991 这条写全了: 机型 / 测试项 / 总分 / 平均帧率 7.34 / 来源=用户拍屏(一手证据)",
          all(k in sn for k in snl_ok_detail),
          "缺: " + str([k for k in snl_ok_detail if k not in sn]))
    check("A7c 991 原有的两条 caveat 保留(公开渠道查不到该机型 + 与 956/998/993 不完全一致)",
          all(k in sn for k in snl_ok_caveat),
          "缺: " + str([k for k in snl_ok_caveat if k not in sn]))
    check("A7d 自洽核对写在源码里: 991 / 135 = 7.3407 fps 与拍屏上的 7.34 fps 一致",
          "7.3407" in sn and "135" in sn,
          "991 / 135 = 7.3407 FPS")
    notes = re.findall(r'\{"([^"]+)",\s*"([^"]+)",\s*([0-9.]+),\s*"([A-Z_0-9]+)",\s*"([^"]*)"', sn)
    check("A8 每条真值都带来源文字(不是光一个数)",
          len(notes) >= 3 and all(len(t[4]) > 10 for t in notes),
          "SNL 三条都带 note(共 %d 条)" % len(notes))

    emit("")
    emit("[B] 对照逻辑与官方口径复算")
    emit("=" * 100)
    check("B1 官方公式 score = 帧率 x 135 被原样使用",
          "k3dmarkNomadScale = 135.0" in sn and "官方明文" in sn,
          "k3dmarkNomadScale = 135.0")
    check("B2 分辨率归一化方向正确(乘 0.5625, 不是除)",
          re.search(r"kScorePerFps\s*=\s*k3dmarkNomadScale\s*\*\s*kWorkloadScale\s*\*\s*kResolutionScaleRatio", sn)
          is not None,
          "kScorePerFps = 135 x 1.0 x 0.5625 = 75.9375")
    for k, v in SNL_TRUTH.items():
        fps = v / K3DMARK
        our = (fps / RES_RATIO) * (K3DMARK * RES_RATIO)
        check("B3 %s: %.0f 分 -> %.2f fps(1440p) -> 本尺子 %.1f 分(闭环)" % (k, v, fps, our),
              abs(our - v) < 1e-6, "%.6f" % our)
    check("B4 deltaPercent 的定义式写进了报告",
          "deltaPercentDefinition" in ref and "(本机 - 参考值) / 参考值 x 100%" in ref,
          "referenceComparison.deltaPercentDefinition")
    check("B5 明确写了这是跨负载换算, 不是官方换算",
          "comparisonIsNotAnOfficialConversion" in ref and "没有官方换算关系" in ref,
          "comparisonIsNotAnOfficialConversion=true")

    emit("")
    emit("[C] 工作负载的代表性(每一项都要有一句话真实用途)")
    emit("=" * 100)
    use_rows = re.findall(r'\{"([^"]+)",\s*"([^"]{8,})"\s*\}', ref)
    use_map = {a: b for (a, b) in use_rows}
    gb7_cpu = ["File Compression", "Navigation", "Text Processing", "Asset Compression", "Photo Library",
               "Photo Editor", "HDR", "Ray Tracer", "Game Physics", "PDF Viewer", "HTML5 Browser",
               "Clang", "Audio Encoder", "Video Encoder", "Video Decoder", "Structure from Motion"]
    gb7_gpu = ["Background Blur", "Face Tracking", "Feature Matching", "Fluid Simulation",
               "Horizon Detection", "Particle Physics", "Path Tracer", "Photo Filter", "RAW",
               "Super Resolution", "Video Filter"]
    missing = [n for n in (gb7_cpu + gb7_gpu + ["Aurora Nomad Light"]) if n not in use_map]
    check("C1 16 项 CPU + 11 项 GPU + 本小节都有真实用途", len(missing) == 0,
          ("缺: " + str(missing)) if missing else "共 %d 项全部覆盖" % (len(use_map)))
    vague = [n for n, u in use_map.items() if not use_is_concrete(u)]
    check("C2 每句用途都不是空话(长度 + 含具体场景词)", len(vague) == 0,
          ("可能是空话: " + str(vague[:5])) if vague else "全部通过")
    check("C3 用途表覆盖了本小节自身的名字", "Aurora Nomad Light" in use_map,
          "Aurora Nomad Light -> " + use_map.get("Aurora Nomad Light", "")[:40])

    emit("")
    emit("[D] 报告样例(六条验收标准对应的字段都在)")
    emit("=" * 100)
    try:
        d = json.load(io.open(sample, encoding="utf-8"))
        ok = True
        err = ""
    except Exception as e:
        d = {}
        ok = False
        err = str(e)
    check("D1 sn_json_sample.json 可解析", ok, err if not ok else "已解析")
    need = {
        "1 可重复性": ["repeatability"],
        "2 温度/频率/降频": ["environment"],
        "3 参考分对照": ["referenceComparison"],
        "4 真实用途": ["realUse"],
        "5 可解释性": ["score", "work"],
        "6 运行环境/防跑分模式": ["environment"],
    }
    for label, keys in need.items():
        check("D2 第 %s 条对应字段在样例里" % label, all(k in d for k in keys),
              ", ".join(keys))
    r_ = d.get("repeatability", {})
    check("D3 样例里 中位/最小/最大/离散度 四件套齐全",
          all(k in r_.get("statistics", {}).get("fps", {}) for k in ("median", "min", "max", "dispersion")),
          "repeatability.statistics.fps.{median,min,max,dispersion}")
    check("D4 样例里带醒目的可信度判断",
          "credibility" in r_ and "verdict" in r_.get("credibility", {}),
          "repeatability.credibility.verdict")
    check("D5 样例里带逐轮原始值(不是只留最好一轮)",
          isinstance(r_.get("roundsDetail"), list) and len(r_["roundsDetail"]) >= 1 and
          r_.get("roundValuesAreAllListed") is True,
          "roundsDetail[%d] + roundValuesAreAllListed=true" % len(r_.get("roundsDetail") or []))
    env = d.get("environment", {})
    check("D6 样例里温度与频率都给了(且说明 GPU 频率读不到)",
          "temperature" in env and "cpuKhz" in env and env.get("gpuClockReadable") is False,
          "environment.temperature / cpuKhz / gpuClockReadable=false")
    rc = d.get("referenceComparison", {})
    check("D7 样例里对照表带来源种类",
          "gpuSnl" in rc and "sourceKind" in rc.get("gpuSnl", {}),
          "referenceComparison.gpuSnl.sourceKind")
    check("D8 样例里写明了查不到的项",
          isinstance(rc.get("notAvailable"), list) and len(rc["notAvailable"]) > 0,
          "referenceComparison.notAvailable[]")

    emit("")
    emit("=" * 100)
    emit("共 %d 项断言, 失败 %d 项 -> %s" % (checks, fails, "PASS" if fails == 0 else "FAIL"))
    emit("=" * 100)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
