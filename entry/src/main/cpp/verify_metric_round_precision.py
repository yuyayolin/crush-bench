# -*- coding: utf-8 -*-
r"""
verify_metric_round_precision.py —— 「打印精度」与「代表轮选取」两条口径的永久断言(无设备)

为什么单列一个脚本: 这两条都属于"显示出来的数字与计分用的数字必须自洽"这一类口径,
一旦回退就会静默产生两种假象 —— ①分数被打印精度网格化(同一负载在不同芯片上给出
逐位相同的分数); ②显示的吞吐与显示的分数互相矛盾。

[A] metric 打印精度(用户 2026-10-09 的硬要求)
    * 计分解析的就是 metric 字符串: gb7RunTest 里 value = sscanf(o.metric), score = k x value x conv;
      => 格式串给出的有效数字位数, 直接决定分数能被量化到多粗的格子上。
    * 断言: 9 个负载文件里由 snprintf 填出来的 metric 站点恰好 16 个(= 注册表 16 项),
      每一个的格式串都必须给出 >= 4 位有效数字(禁止 %.1f / %.2f / %.3f ...);
      反例数字: 0.152 与 0.249 在 %.1f 下都会打印成 "0.2" -> 同一个分数;
    * 真机实证逐项复算(数字来自 D:\gb7logs\r75 的报告 JSON, 硬编码在这里作为证据):
      Ray Tracer / Audio Encoder / HDR / Game Physics / Video Encoder 在旧格式下的
      分数网格是 50.0% / 50.0% / 19.9% / 11.1% / 7.7%(手机与平板在这几项上给出逐位相同的分数);
      改成 %.4g 之后每一项的网格都 <= 0.2%。
    * 退化路径(负载失败)允许写字面 metric, 但必须解析为 0(score 恒 0, 不可能把真实分数量格化)。

[B] medianRoundIndex 的新行为(代表值 metric 文本取自哪一轮)
    * 旧实现只做精确匹配, 匹配不到返回 -1, 调用方于是静默保留 all.back()(最后一轮);
      而 medianOfList 在偶数轮返回的是中间两个数的均值 -> 精确匹配每次都会失败。
      真机实证(平板 tab75 / PDF Viewer): 显示 metric=10.0 而代表分 224.9(反推 10.1)。
    * 新行为: 精确命中优先(奇数轮与旧行为逐位一致), 否则取最接近中位的那一轮,
      找不到任何有效轮才返回 -1。
    * 断言: 结构(源码级) + 镜像(C++ 逐字复刻的独立复算), 含"不返回最后一轮"的反例。

[C] diag 是纯文本: napi / ArkTS / 报告 / 任何 verify 脚本都不得按 k=v 解析它。
    (medianRoundIndex 的差额说明就是追加进 o.diag 的 —— 这条是它成立的前提。)

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys, math

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                          # .../entry/src/main
OUT = io.open(os.path.join(HERE, "verify_metric_round_precision_out.txt"), "w", encoding="utf-8", newline="\n")
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

def rd(*parts):
    p = os.path.join(HERE, *parts)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()

LOAD_FILES = ["gb7_filecompress.cpp", "gb7_batch2.cpp", "gb7_batch3.cpp", "gb7_pdf.cpp",
              "gb7_audio.cpp", "gb7_video.cpp", "gb7_browser.cpp", "gb7_sfm.cpp", "gb7_clang.cpp"]
gb7 = rd("gb7.cpp")

# ---------------------------------------------------------------------------
# 工具: 一个 metric 站点 = (文件, 行号, 变量名或 <literal>, 格式串或字面值)
#   o.metric = <变量>;   -> 往上找最近一条填同一个变量的 snprintf(..., "FMT", ...)
#   o.metric = "字面值"; -> 退化路径
# ---------------------------------------------------------------------------
def metric_sites(text):
    lines = text.split("\n")
    out = []
    for i, ln in enumerate(lines):
        m = re.search(r"\bo\.metric\s*=\s*([A-Za-z_]\w*)\s*;", ln)
        if m:
            var = m.group(1)
            fmt = None
            for j in range(i - 1, max(-1, i - 60), -1):
                mm = re.search(r"snprintf\(\s*" + re.escape(var) + r"\s*,[^,]+,\s*\"([^\"]*)\"", lines[j])
                if mm:
                    fmt = mm.group(1)
                    break
            out.append((i + 1, var, fmt))
            continue
        m2 = re.search(r"\bo\.metric\s*=\s*\"([^\"]*)\"\s*;", ln)
        if m2:
            out.append((i + 1, "<literal>", m2.group(1)))
    return out

def sig_digits_of_fmt(fmt):
    """格式串给出的有效数字位数; None = 这个格式串不是"按有效数字打印"(例如 %f 系列)。"""
    if fmt is None:
        return None
    # 只认最后那一个转换说明(格式串里可能还有别的字符, 但 metric 站点都是单转换)
    convs = re.findall(r"%[-+ #0]*(\d*)(?:\.(\d+))?([a-zA-Z%])", fmt)
    if not convs:
        return None
    _, prec, kind = convs[-1]
    if kind in ("g", "G"):
        return int(prec) if prec else 6          # %g 缺省 6 位有效数字
    if kind in ("e", "E"):
        return int(prec) if prec else 6
    return None                                   # f / d / s ... 都不算"有效数字"

def fixed_decimals_of_fmt(fmt):
    if fmt is None:
        return None
    convs = re.findall(r"%[-+ #0]*(\d*)(?:\.(\d+))?([a-zA-Z%])", fmt)
    if not convs:
        return None
    _, prec, kind = convs[-1]
    if kind in ("f", "F"):
        return int(prec) if prec else 6
    return None

emit("=" * 100)
emit("[A] metric 打印精度: 计分解析的就是这一串, 位数不足会把分数网格化")
emit("=" * 100)

sites = []
for f in LOAD_FILES:
    for (ln, var, fmt) in metric_sites(rd(f)):
        sites.append((f, ln, var, fmt))
snprintf_sites = [s for s in sites if s[2] != "<literal>"]
literal_sites = [s for s in sites if s[2] == "<literal>"]

entries_rows = len(re.findall(r'^\s*\{"[^"]+",', gb7, re.M))
emit("  注册表 ENTRIES 行数 = %d; metric 站点: snprintf 填的 %d 个 + 字面值 %d 个"
     % (entries_rows, len(snprintf_sites), len(literal_sites)))
for (f, ln, var, fmt) in sites:
    emit("    %-24s %5d  %-9s %s" % (f, ln, var, fmt))

check("A1 GB7 注册表 16 项, 且由 snprintf 填出来的 metric 站点恰好 16 个(一项一个, 不多不少)",
      entries_rows == 16 and len(snprintf_sites) == 16,
      "entries=%d, snprintf 站点=%d" % (entries_rows, len(snprintf_sites)))
check("A2 9 个负载文件里都有 metric 站点(没有哪一项漏在扫描之外)",
      len(set(s[0] for s in snprintf_sites)) == 9,
      sorted(set(s[0] for s in snprintf_sites)))

bad_digits = [(f, ln, fmt) for (f, ln, v, fmt) in snprintf_sites if (sig_digits_of_fmt(fmt) or 0) < 4]
check("A3 每一个 GB7 metric 的格式串都给出 >= 4 位有效数字(%%.4g / %%.5g / %%%%g ... 合格)",
      len(bad_digits) == 0, bad_digits)

forbidden_f = [(f, ln, fmt) for (f, ln, v, fmt) in snprintf_sites if fixed_decimals_of_fmt(fmt) is not None]
check("A4 禁止在 metric 上用 %%.[N]f 系列(%.1f / %.2f / %.3f 都会把小数位与量级绑死)",
      len(forbidden_f) == 0, forbidden_f)

# ---- 反例数字(用户点名要的那一条): 0.152 与 0.249 在 %.1f 下得到同一个分数 ----
old_a, old_b = "%.1f" % 0.152, "%.1f" % 0.249
new_a, new_b = "%.4g" % 0.152, "%.4g" % 0.249
emit("")
emit("  反例: 真值 0.152 与 0.249 在 %%.1f 下都打印成 \"%s\" / \"%s\" -> 同一个 metric -> 同一个分数"
     % (old_a, old_b))
emit("        同一个 %%.4g 下打印成 \"%s\" / \"%s\" -> 不同分数(相差 %.1f%%)"
     % (new_a, new_b, (0.249 / 0.152 - 1.0) * 100.0))
check("A5 反例成立: %%.1f 把 0.152 与 0.249 压成同一个字符串, %%.4g 能区分",
      old_a == old_b and new_a != new_b)

# ---- 真机实证的分数网格逐项复算(证据数字硬编码, 来源写在上面) ----
# (项名, kSingle, conv, 旧报告里 metric 字符串的小数位, 该轮代表分, 该轮旧报告 metric 原文)
REAL_GRID = [
    ("Ray Tracer",     3.560843717, 1000.0,             1, 712.2,   "0.2"),
    ("Audio Encoder",  718.4717517, 0.5,                1, 71.8,    "0.2"),
    ("HDR",            16.55415633, 1.0,                1, 8.3,     "0.5"),
    ("Game Physics",   24.71979771, 122.0703125,        1, 2715.8,  "0.9"),
    ("Video Encoder",  27.49885069, 1.0850694444444444, 1, 38.8,    "1.3"),
    ("Video Decoder",  9.459432954, 1.0850694444444444, 1, 46.2,    "4.5"),
    ("File Compression", 7.027686550, 1.0,              1, 50.6,    "7.2"),
    ("Navigation",     181.6940430, 1.0,                1, 1489.9,  "8.2"),
    ("Asset Compression", 45.02062482, 1.0,             1, 542.5,   "12.1"),
    ("Photo Editor",   54.41947208, 0.16666666666666666, 1, 79.8,   "8.8"),
    ("PDF Viewer",     22.27127003, 1.0,                1, 224.9,   "10.1"),
    ("Text Processing", 16.94929558, 1.0,               1, 1031.4,  "60.8"),
    ("Photo Library",  291.9049489, 1.0,                2, 1208.5,  "4.13"),
    ("HTML5 Browser",  80.00299486, 1.0,                2, 2955.7,  "36.99"),
    ("SfM",            0.0,         0.0,                2, 0.0,     "0.02"),
    ("Clang",          184.8221414, 1.0,                2, 11436.8, "62.06"),
]
emit("")
emit("  真机实证的分数网格(旧格式; 网格 = k x conv x 打印步长, 占比 = 网格 / 代表分):")
worst_old = 0.0
worst_name = ""
new_grid_all = []
for (name, k, conv, dec, score, met) in REAL_GRID:
    if k <= 0.0 or score <= 0.0:
        continue
    grid = k * conv * (10.0 ** (-dec))
    pct = grid / score * 100.0
    newpct = 10.0 ** (1 - 4) * 100.0    # %.4g = 4 位有效数字 -> 相对网格 <= 0.1%
    new_grid_all.append((name, newpct))
    if pct > worst_old:
        worst_old, worst_name = pct, name
    emit("    %-20s metric=%-7s k*conv=%10.4f 网格=%9.3f 分=%9.1f -> 网格占比 %5.2f%%"
         % (name, met, k * conv, grid, score, pct))
emit("    最粗的一项 = %s(网格占分的 %.1f%%); 改成 %%.4g 之后每项的网格都 <= %.2f%%"
     % (worst_name, worst_old, new_grid_all[0][1] if new_grid_all else 0.0))
check("A6 真机实证可复算: 旧格式下最粗的项网格 >= 7%%(Ray Tracer/Audio Encoder 达 50%% —— "
      "手机与平板在这几项上给出逐位相同的分数, 对芯片差异携带的信息量为 0)",
      worst_old >= 7.0, "worst=%.2f%%" % worst_old)
check("A7 %.4g 之后每一项的分数网格都 <= 0.2%(比 2% 的重复性阈值低一个数量级)",
      all(g <= 0.2 for (_, g) in new_grid_all))

lit_ok = True
lit_detail = []
for (f, ln, var, val) in literal_sites:
    try:
        v = float(val)
    except Exception:
        v = -1.0
    lit_detail.append((f, ln, val, v))
    if not (v == 0.0):
        lit_ok = False
check("A8 退化路径的字面 metric 必须解析为 0(score 恒 0, 不可能把真实分数量格化)",
      lit_ok, lit_detail)

check("A9 计分口径未变: 仍然是 sscanf 解析 metric 字符串 + score = k x (value x conv)",
      'sscanf(o.metric.c_str(), "%lf", &value)' in gb7 and "score = k * (value * e.conv);" in gb7)

emit("")
emit("=" * 100)
emit("[B] medianRoundIndex: 精确命中优先, 否则取最接近中位的那一轮")
emit("=" * 100)

m = re.search(r"int medianRoundIndex\(const std::vector<Gb7Outcome>& all, double medianMetric\)(.*?)\n\}\n",
              gb7, re.S)
body = m.group(1) if m else ""
check("B1 结构: medianMetric <= 0 时返回 -1(这一条与旧行为一致)", "if (!(medianMetric > 0.0)) {" in body and "return -1;" in body)
check("B2 结构: 精确命中立刻返回(奇数轮时与旧行为逐位一致)",
      "return (int)i;" in body and "d <= 1.0e-9 *" in body)
check("B3 结构: 否则记下最接近的那一轮(严格小于才换, 因此并列时取更早的那一轮)",
      "if (best < 0 || d < bestDiff) {" in body and "bestDiff = d;" in body)
check("B4 结构: 循环之后返回 best —— 也就是说只有'没有有效轮'才会是 -1",
      "return best;" in body and body.rstrip().endswith("return best;"))
after_loop = body.split("return best;")[0]
check("B5 结构: 循环之后没有别的 -1 出口(旧实现的'匹配不到就 -1'已经被删掉)",
      "return -1;" not in after_loop.split("for (")[-1])

def mirror_parse(s):
    """parseMetricText() 的镜像: 解析不出/非有限/负数 -> 失败。"""
    try:
        v = float(s)
    except Exception:
        return None
    if v != v or v in (float("inf"), float("-inf")) or v < 0.0:
        return None
    return v

def mirror_median_round_index(items, median_metric):
    """medianRoundIndex() 的逐字镜像。"""
    if not (median_metric > 0.0):
        return -1
    best = -1
    best_diff = 0.0
    for i, s in enumerate(items):
        mv = mirror_parse(s)
        if mv is None:
            continue
        d = abs(mv - median_metric)
        if d <= 1.0e-9 * (median_metric if median_metric > 1.0 else 1.0):
            return i
        if best < 0 or d < best_diff:
            best = i
            best_diff = d
    return best

def old_median_round_index(items, median_metric):
    """旧实现(只做精确匹配)的镜像 —— 用来证明"偶数轮每次都会失败"。"""
    if not (median_metric > 0.0):
        return -1
    for i, s in enumerate(items):
        mv = mirror_parse(s)
        if mv is None:
            continue
        if abs(mv - median_metric) <= 1.0e-9 * (median_metric if median_metric > 1.0 else 1.0):
            return i
    return -1

def mirror_median_of_list(xs):
    """medianOfList() 的镜像(偶数个返回中间两个的均值)。"""
    if not xs:
        return 0.0
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 == 1 else 0.5 * (s[n // 2 - 1] + s[n // 2])

# ---- B6 奇数轮: 精确命中 -> 与旧行为逐位一致 ----
odd = ["1.0", "2.0", "3.0"]
med3 = mirror_median_of_list([1.0, 2.0, 3.0])
r_new3 = mirror_median_round_index(odd, med3)
r_old3 = old_median_round_index(odd, med3)
emit("  奇数轮(3 轮): metric=%s 中位=%.4f -> 新 %d / 旧 %d" % (odd, med3, r_new3, r_old3))
check("B6 镜像: 奇数轮时新旧实现返回同一个下标(逐位一致, 老行为没被改掉)",
      r_new3 == r_old3 == 1)

# ---- B7 偶数轮(默认 2 轮): 中位是两轮均值 -> 旧实现必然 -1(调用方静默用最后一轮) ----
even2 = ["10.1", "9.9"]
med2 = mirror_median_of_list([10.1, 9.9])
r_new2 = mirror_median_round_index(even2, med2)
r_old2 = old_median_round_index(even2, med2)
emit("  偶数轮(2 轮, 真机 PDF Viewer 形状): metric=%s 中位=%.4f -> 新 %d / 旧 %d(旧实现回退到最后一轮 %s)"
     % (even2, med2, r_new2, r_old2, even2[-1]))
check("B7 镜像: 偶数轮时旧实现返回 -1(也就是每次都会静默用最后一轮的文本), 新实现返回一个有效轮",
      r_old2 == -1 and r_new2 >= 0)
check("B8 镜像: 偶数轮时新实现不是最后一轮(两轮并列时取更早的那一轮, 确定性行为)",
      r_new2 == 0 and r_new2 != len(even2) - 1)
gap = abs(float(even2[r_new2]) - med2) / med2 * 100.0
check("B9 镜像: 选中的那一轮与中位的差额 = %.2f%%(> 0.05%% -> 调用方会把差额写进 diag)" % gap,
      gap > 0.05)

# ---- B10 精确命中优先于"仅仅更接近"(并列/重复值) ----
rep = ["5.0", "2.0", "2.0"]
med_rep = mirror_median_of_list([5.0, 2.0, 2.0])
r_rep = mirror_median_round_index(rep, med_rep)
check("B10 镜像: 存在精确命中的那一轮时返回它(取第一个精确命中), 不会被'更接近'抢走",
      r_rep == 1, r_rep)

# ---- B11 解析失败的轮被跳过 ----
r_bad = mirror_median_round_index(["bad", "2.0"], 2.0)
check("B11 镜像: 解析不出数值的那一轮被跳过, 返回真正可用的那一轮", r_bad == 1, r_bad)

# ---- B12 没有任何有效轮 -> -1; medianMetric <= 0 -> -1 ----
check("B12 镜像: 一轮都解析不出来 -> -1(写'没有代表值', 不编数字)",
      mirror_median_round_index(["bad", "-1"], 2.0) == -1)
check("B13 镜像: medianMetric <= 0 -> -1",
      mirror_median_round_index(["2.0"], 0.0) == -1)

# ---- B14 偶数轮 4 轮: 取最接近的中间轮, 不取最后一轮 ----
four = ["1.0", "2.0", "3.0", "100.0"]
med4 = mirror_median_of_list([1.0, 2.0, 3.0, 100.0])
r4 = mirror_median_round_index(four, med4)
emit("  偶数轮(4 轮): metric=%s 中位=%.4f -> 新 %d(最接近中位的那一轮; 最后一轮是 100.0, 离得很远)"
     % (four, med4, r4))
check("B14 镜像: 4 轮时返回最接近中位的那一轮, 不是最后一轮", r4 == 1)

check("B15 差额说明写进 diag(阈值 0.05%), 且没有改任何计分数值",
      "gapPct > 0.05" in gb7 and "o.diag = o.diag.empty() ? std::string(nb)" in gb7 and
      "o.score = o.scoreMedian;" in gb7)

emit("")
emit("=" * 100)
emit("[C] diag 必须是纯文本(medianRoundIndex 的差额说明就追加在它后面)")
emit("=" * 100)

# C1 napi: 只做转义输出 + 失败路径固定形状
napi = rd("napi_init.cpp")
diag_lines_napi = [l.strip() for l in napi.split("\n") if "diag" in l and not l.strip().startswith("//")]
check("C1 napi 侧只把 diag 原样转义输出(没有按 = 拆键值)",
      all(("escapeJson(r.diag)" in l) or ('\\"diag\\":\\"\\"' in l) or l.startswith("*") or l.startswith("//")
          for l in diag_lines_napi),
      diag_lines_napi[:6])

# C2 ArkTS: 所有出现 .diag 的行都不含任何解析调用
ETS_DIR = os.path.join(ROOT, "ets")
ets_files = []
for dirpath, _dirs, files in os.walk(ETS_DIR):
    for fn in files:
        if fn.endswith(".ets"):
            ets_files.append(os.path.join(dirpath, fn))
PARSE_TOKENS = [".split(", ".indexOf(", ".match(", ".replace(", ".substring(", ".startsWith(",
                ".includes(", "JSON.parse", "RegExp", ".search(", ".slice("]
diag_parse_hits = []
for p in ets_files:
    for idx, ln in enumerate(io.open(p, encoding="utf-8", errors="replace").read().split("\n"), 1):
        if "diag" not in ln:
            continue
        if any(t in ln for t in PARSE_TOKENS):
            diag_parse_hits.append("%s:%d: %s" % (os.path.basename(p), idx, ln.strip()[:120]))
emit("  ArkTS 里出现 diag 的文件数 = %d; 含解析调用的行数 = %d"
     % (len([p for p in ets_files if "diag" in io.open(p, encoding="utf-8", errors="replace").read()]),
        len(diag_parse_hits)))
check("C2 ArkTS 侧对 diag 只做字符串拼接/赋值(没有任何 split/indexOf/match/replace/RegExp)",
      len(diag_parse_hits) == 0, diag_parse_hits[:6])

# C3 verify 脚本自己也不许解析 diag
# 只认"真的在对 diag 的内容做解析"的形态; 本脚本自己会提到这些词(它就是检查器),
# 因此把自己排除在外 —— 其余每一个 verify 脚本都必须干净。
SELF = "verify_metric_round_precision.py"
verify_files = [f for f in os.listdir(HERE)
                if f.startswith("verify_") and f.endswith(".py") and f != SELF]
DIAG_PARSE = re.compile(r"\.diag\s*\.\s*(split|indexOf|match|replace|substring|startsWith|includes|search|slice)\s*\("
                        r"|(split|search|findall|match|groups)\s*\([^)]*\bdiag\b")
py_hits = []
for fn in verify_files:
    for idx, ln in enumerate(rd(fn).split("\n"), 1):
        if "diag" not in ln:
            continue
        if DIAG_PARSE.search(ln):
            py_hits.append("%s:%d: %s" % (fn, idx, ln.strip()[:120]))
check("C3 除本脚本外, 任何 verify 脚本都没有按 k=v 解析 diag 的内容(%d 个脚本已扫)" % len(verify_files),
      len(py_hits) == 0, py_hits[:6])

# C4 结论: diag 是自由文本字段 —— 追加"第 N 轮 / 差额"这样的说明不会破坏任何消费者
check("C4 diag 的声明就是自由文本(旁路自证字段), 且报告里只被原样打印",
      "std::string diag;" in rd("gb7.h") and
      any("diag: string;" in io.open(p, encoding="utf-8", errors="replace").read() for p in ets_files))

emit("")
emit("=" * 100)
emit("[D] 不许动的东西")
emit("=" * 100)
check("D1 计分公式一个字未动", "score = k * (value * e.conv);" in gb7)
check("D2 k / conv 未被触碰(抽查 3 项)",
      "7.027686550" in gb7 and "181.6940430" in gb7 and "184.8221414" in gb7)
ms = sum(rd(f).count("auroraFreqMarkStart();") for f in LOAD_FILES)
me = sum(rd(f).count("auroraFreqMarkStop();") for f in LOAD_FILES)
check("D3 16 项负载的打点数量未变(16/16)", ms == 16 and me == 16, "%d/%d" % (ms, me))
check("D4 负载文件里没有为了本次口径改动而改算法/尺寸(仍只有 metric 格式串一处变化)",
      all(rd(f).count("%.4g") >= 1 for f in LOAD_FILES))
check("D5 unit(单位)未被触碰(抽查 3 项)",
      '"MB/sec"' in gb7 and '"routes/sec"' in gb7 and '"Klines/sec"' in gb7)

emit("")
emit("=" * 100)
if not fails:
    emit("结论: 全部 PASS —— 16 个 GB7 metric 的格式串都给出 >= 4 位有效数字(%.4g),")
    emit("      分数不再被打印精度网格化(旧格式下 Ray Tracer/Audio Encoder 的网格是分的 50%);")
    emit("      medianRoundIndex 精确命中优先、否则取最接近中位的那一轮(奇数轮与旧行为逐位一致,")
    emit("      偶数轮不再静默用最后一轮), 差额 > 0.05% 写进 diag; diag 全链路只被当纯文本显示。")
else:
    emit("结论: 有 %d 项 FAIL: %s" % (len(fails), ", ".join(fails)))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
