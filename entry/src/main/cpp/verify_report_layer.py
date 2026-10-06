# -*- coding: utf-8 -*-
r"""
verify_report_layer.py —— 8.0 真机报告暴露的 4 个汇报层缺陷的静态自检 + 真机产物复算(无设备)

对应 D:\gb7logs\r80\(手机 Pura X Max, 8.0, 2026-10-05 16:04) 与 D:\gb7logs\tab80\(平板)。
本脚本只读源码 + 独立数字/编码镜像 + 已落盘的真机报告, 不联设备、不跑 hdc、不改任何文件。

  1) 跑满判据的"还差几个百分点": 阈值与实测必须同单位相减(千分比), 再打一位小数。
     旧实现 (kFullRatioPct*10) - (ratioPermille/10) 把千分比与百分点混着减: 真机 87.9% 被印成
     "还差 813 个百分点"(实际 2.1), 89.0% 印成 811(实际 1.0)。
  2) QoS / cpuset: native 算了、napi 也输出, 但 ArkTS 的键一路缺到 items[] —— 逐层核对到底断在哪。
  3) GPU-SNL: 小节状态("失败 · （native 未给文本）")与它自己的原始 JSON("ok":true)矛盾 ——
     真实原因是原始 JSON 不是合法 JSON(解析失败被当成了 native 失败, 原因被丢掉)。
  4) 报告 .txt 的非法 UTF-8: 真机现场 12 个非法字节 = 4 个完整 CESU-8 序列(孤立代理项),
     不是"分块写入把多字节字符切在块边界上"; 修法 = 写盘前把孤立代理项换成 U+FFFD + 硬断言。

退出码: 0 = 全部 PASS(真机产物不存在时对应几条会明确标 SKIP, 不算 FAIL), 1 = 有 FAIL。
"""
import io, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_report_layer_out.txt"), "w", encoding="utf-8", newline=chr(10))
fails = []
skips = []

def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    OUT.write(s + "\n")

passes = []

def check(name, ok, detail=""):
    emit("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  <- " + str(detail)) if not ok else ""))
    if ok:
        passes.append(name)
    else:
        fails.append(name)

def skip(name, why):
    emit("  [SKIP] %s  <- %s" % (name, why))
    skips.append(name)

def rd(fn):
    p = os.path.join(HERE, fn)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()

def rd_ets(rel):
    p = os.path.join(HERE, "..", "ets", rel)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()

# ---- 源码(本层全部是"汇报层": native 只作为证据链的一环被读) ----
C_FREQ = rd("cpu_freq_sample.cpp")
ETS_RUNNER = rd_ets(os.path.join("service", "BenchRunner.ets"))
ETS_MODEL = rd_ets(os.path.join("model", "BenchModel.ets"))
ETS_FULL = rd_ets(os.path.join("service", "FullRun.ets"))
ETS_SINK = rd_ets(os.path.join("service", "ReportSink.ets"))
ETS_GUARD = rd_ets(os.path.join("common", "Utf8Guard.ets"))
ETS_INDEX = rd_ets(os.path.join("pages", "Index.ets"))
ETS_LAYERS = rd_ets(os.path.join("common", "ResultLayers.ets"))
NAPI = rd("napi_init.cpp")
GB7H = rd("gb7.h")
CMP = rd("reference_compare.cpp")            # 对比模块(native): 回答"限制在哪一层 / QoS 生效没有"
ETS_CMP = rd_ets(os.path.join("service", "ReportCompare.ets"))   # 对比页的数据层

# ---- 真机报告(只在存在时用; 缺了就是 SKIP) ----
R80_TXT = r"D:\gb7logs\r80\report-latest.txt"
R80_JSON = r"D:\gb7logs\r80\report-latest.json"

def read_bytes(p):
    if not os.path.exists(p):
        return None
    return open(p, "rb").read()

def is_v8_report(j):
    try:
        return "8.0" in str(j.get("header", {}).get("appVersion", ""))
    except Exception:
        return False

emit("=" * 100)
emit("[0] 证据文件与源码是否就位")
emit("=" * 100)
check("0.1 汇报层源码齐全(BenchRunner / BenchModel / FullRun / ReportSink / Utf8Guard)",
      len(ETS_RUNNER) > 1000 and len(ETS_MODEL) > 1000 and len(ETS_FULL) > 1000 and
      len(ETS_SINK) > 1000 and len(ETS_GUARD) > 1000)
check("0.2 native 侧源码可读(napi_init.cpp / gb7.h / cpu_freq_sample.cpp)",
      len(NAPI) > 1000 and len(GB7H) > 1000 and len(C_FREQ) > 1000)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[1] 缺陷 1: 跑满判据的'还差几个百分点'(阈值与实测必须同单位)")
emit("=" * 100)
K_FULL = 90
def old_gap(permille):      # 旧实现(逐字镜像): 千分比 - 整数除法得到的百分点
    return K_FULL * 10 - permille // 10
def new_gap_permille(permille):
    return K_FULL * 10 - permille
def new_gap_text(permille):
    gp = new_gap_permille(permille)
    return "%d.%d" % (gp // 10, gp % 10)

check("1.1 新算式: 阈值与实测都在千分比上相减, 再打成 x.y 个百分点",
      "const int gapPermille = kFullRatioPct * 10 - ratioPermille;" in C_FREQ and
      "kFullRatioPct, gapPermille / 10, gapPermille % 10);" in C_FREQ and
      '"(还差 %d.%d "' in C_FREQ.replace("\n", " ") or "还差 %d.%d " in C_FREQ)
# 旧算式必须从代码里消失(注释里留着说明不算)
code_lines = [l for l in C_FREQ.split("\n") if not l.lstrip().startswith("//")]
check("1.2 旧算式(不同单位相减)已从代码里消失, 只剩注释里的历史说明",
      not any("kFullRatioPct * 10 - ratioPermille / 10);" in l for l in code_lines),
      "旧表达式仍在代码里")
check("1.3 判据与阈值一个字都没改(仍是 >= 90%, 仍写作 kFullRatioPct * 10)",
      "const int  kFullRatioPct      = 90;" in C_FREQ and
      "if (ratioPermille >= kFullRatioPct * 10) {" in C_FREQ)

# ---- 真机两个输入复算(手机 8.0 报告里出现过的占比中位) ----
cases = [(879, "2.1"), (890, "1.0")]
for perm, want in cases:
    got = new_gap_text(perm)
    check("1.4 真机输入复算 %d.%d%% -> 还差 %s 个百分点(旧算法给 %d)"
          % (perm // 10, perm % 10, want, old_gap(perm)),
          got == want and old_gap(perm) != new_gap_permille(perm))
check("1.5 真机报告里印过的旧值就是旧算法算出来的(813/811/831 = 900 - 千分比//10)",
      old_gap(879) == 813 and old_gap(890) == 811 and old_gap(690) == 831 and
      old_gap(690) == K_FULL * 10 - 69)
check("1.6 阈值处翻转正确, 且只差 1 千分点时仍判'未达到'(不把 89.9% 当跑满)",
      new_gap_permille(899) == 1 and new_gap_permille(900) == 0 and
      new_gap_text(899) == "0.1" and not (899 >= K_FULL * 10))

# 真机产物: r80 报告里到底印的是什么(旧值 813 是这条缺陷的直接现场)
rb = read_bytes(R80_TXT)
if rb is None:
    skip("1.7 真机 r80 报告里 813 / 811 / 831 的现场", "找不到 " + R80_TXT)
else:
    txt = rb.decode("utf-8", errors="replace")
    n813 = txt.count("还差 813 个百分点")
    n811 = txt.count("还差 811 个百分点")
    n831 = txt.count("还差 831 个百分点")
    check("1.7 真机 r80 报告里确实印着旧值(813 x%d / 811 x%d / 831 x%d), 共 %d 处判据结论"
          % (n813, n811, n831, txt.count("跑满判据")),
          (n813 + n811 + n831) > 0 and txt.count("跑满判据") > 0)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[2] 缺陷 2: QoS / cpuset 从 native 一路接到 items[]")
emit("=" * 100)
# 2.1 native: 字段与 napi 输出都在(说明断点不在 native)
check("2.1 native 侧: gb7.h 里有 o.qos / o.cpuset 两个字段",
      "std::string qos;" in GB7H and "std::string cpuset;" in GB7H)
BS = chr(92)   # 反斜杠: 用 chr() 拼, 免得脚本自身的转义把判定写歪
check("2.2 native 侧: napi_init.cpp 确实输出了 qos 与 cpuset 两个 JSON 键",
      ('out += ",' + BS + '"qos' + BS + '":' + BS + '"' in NAPI) and
      ('out += ",' + BS + '"cpuset' + BS + '":' + BS + '"' in NAPI))
# 2.3 ArkTS 逐层
check("2.3 ArkTS 第①层: Gb7Partial(native JSON 的中间类型)有 qos / cpuset",
      re.search(r"interface Gb7Partial \{[^}]*qos: string;", ETS_RUNNER, re.S) is not None and
      re.search(r"interface Gb7Partial \{[^}]*cpuset: string;", ETS_RUNNER, re.S) is not None)
check("2.4 ArkTS 第②层: cpuFieldsFrom() 真的从 native JSON 里读这两个键",
      "qos: (o.qos as string) ?? ''," in ETS_RUNNER and
      "cpuset: (o.cpuset as string) ?? ''," in ETS_RUNNER)
check("2.5 ArkTS 第③层: 中性默认值里有 qos/cpuset(空串 = 未上报, 不用假值冒充)",
      re.search(r"runFreq: '',\s*\n\s*//[^\n]*\n\s*qos: '',\s*\n\s*cpuset: '',", ETS_RUNNER) is not None or
      ("qos: ''," in ETS_RUNNER and "cpuset: ''," in ETS_RUNNER))
check("2.6 ArkTS 第④层: withCpuFields() 把它们拷进 Gb7Result",
      "r.qos = c.qos;" in ETS_RUNNER and "r.cpuset = c.cpuset;" in ETS_RUNNER)
check("2.7 ArkTS 第⑤层: model/BenchModel.ets 的 Gb7Result 有这两个字段(以前就是断在这里)",
      re.search(r"export interface Gb7Result \{[^}]*\n\s*qos: string;", ETS_MODEL, re.S) is not None and
      re.search(r"export interface Gb7Result \{[^}]*\n\s*cpuset: string;", ETS_MODEL, re.S) is not None)
check("2.8 ArkTS 第⑥层: 报告 JSON 的 RepItemJson 有这两个键",
      re.search(r"interface RepItemJson \{[^}]*\n\s*qos: string;", ETS_FULL, re.S) is not None and
      re.search(r"interface RepItemJson \{[^}]*\n\s*cpuset: string;", ETS_FULL, re.S) is not None)
check("2.9 ArkTS 第⑦层: emptyItemJson() 给每一项都填了默认值(=> items[] 的每一项都有这两个键)",
      "runFreq: '', qos: '', cpuset: '', cpu: -1," in ETS_FULL)
check("2.10 ArkTS 第⑧层: gb7ItemJson() 把值写进 item",
      "j.qos = r.qos;" in ETS_FULL and "j.cpuset = r.cpuset;" in ETS_FULL)
check("2.11 .txt 侧: 逐项块里也打出这两段全文(=> .json 与 .txt 都有)",
      "B.ln('    QoS=' + (r.qos.length > 0" in ETS_FULL and
      "分层诊断(cgroup/cpuset/core_ctl)=" in ETS_FULL)

# 2.12 键集合镜像: 从 emptyItemJson 的源码里抽出它写的所有键, 断言 qos/cpuset 在里面
m = re.search(r"private static emptyItemJson\([^)]*\): RepItemJson \{\s*const j: RepItemJson = \{(.*?)\};", ETS_FULL, re.S)
keys = []
if m:
    body = m.group(1)
    keys = re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*:", body)
check("2.12 items[] 键集合镜像是从 emptyItemJson 源码里抽出来的, 且含 qos 与 cpuset",
      m is not None and "qos" in keys and "cpuset" in keys, keys[:8])

# 2.13 真机产物: 旧 .json 里 0 次 + 旧 .txt 里有 => 断点在 ArkTS 解析层(不是 native/napi)
if rb is None or read_bytes(R80_JSON) is None:
    skip("2.13 真机 8.0 报告里'QoS 只在 .txt、不在 .json'的现场", "找不到 r80 报告")
else:
    jb = read_bytes(R80_JSON)
    try:
        jd = json.loads(jb.decode("utf-8"))
    except Exception as e:
        jd = None
    if jd is None or not is_v8_report(jd):
        skip("2.13 真机现场", "这份 report-latest.json 不是 8.0 那一轮(已被新版本覆盖)")
    else:
        jtxt = jb.decode("utf-8")
        item_keys = set()
        for it in jd.get("items", []):
            item_keys |= set(it.keys())
        n_txt_qos = txt.count("QoS")
        n_json_qos = jtxt.count("QoS") + jtxt.count("qos")
        check("2.13 真机现场: 8.0 的 .txt 里有 QoS(%d 次)而 .json 里一次都没有, items[] 键里也没有 qos/cpuset "
              "=> 断点确实在 ArkTS(解析层丢键), 不在 native/napi" % n_txt_qos,
              n_txt_qos > 0 and n_json_qos == 0 and "qos" not in item_keys and "cpuset" not in item_keys)

# --- 2.14~2.19: 接完 items[] 之后, 对比工具/对比页也必须能看到这些证据(缺陷 2 的后果) ---
check("2.14 对比模块读得到这两列: RepItem 增加 qos/cpuset 字段, 解析时逐个读",
      "bool hasQos = false;" in CMP and "bool hasCpuset = false;" in CMP and
      'jsonGetString(objs[i], "qos", &it.qos)' in CMP and
      'jsonGetString(objs[i], "cpuset", &it.cpuset)' in CMP)
check("2.15 逐项对照表带 qosPresent / cpusetPresent + 200 字节摘录(摘录用 utf8Cut 按字符边界切)",
      "qosPresent" in CMP and "cpusetPresent" in CMP and "qosExcerpt" in CMP and
      "cpusetExcerpt" in CMP and "std::string utf8Cut(" in CMP and
      "while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)" in CMP)
check("2.16 顶层有 environmentEvidence.a/b(两台各自的 qos 与 cpuset 全文 + 取自哪一项 + 取不到时的原因)",
      "std::string envEvidenceJson(const RepDoc& d)" in CMP and
      'environmentEvidence' in CMP and
      "whatItAnswers" in CMP and "nullReason" in CMP and "fromItem" in CMP)
check("2.17 对比页的数据层(ReportCompare.ets)解析了这些字段",
      "envQosPresentA" in ETS_CMP and "envCpusetTextB" in ETS_CMP and
      "rec(o, 'environmentEvidence')" in ETS_CMP)
check("2.18 对比页有专门的一段显示它, 并且真的被调用了",
      "cmpEnvEvidenceBlock()" in ETS_INDEX and
      ETS_INDEX.count("cmpEnvEvidenceBlock()") >= 2)
check("2.19 「怎么用」里加了第 ⑧ 条(用户知道去哪一节看限制层级与 QoS)",
      "⑧ 限制层级与 QoS" in CMP)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[3] 缺陷 3: GPU-SNL 小节的状态与它自己的原始 JSON 必须自洽")
emit("=" * 100)
check("3.1 解析失败被显式标记(不再靠一个 ok:false 的替身对象悄悄顶替)",
      "export const PARSE_FAILED_KEY: string = '__auroraParseFailed';" in ETS_RUNNER and
      "export const PARSE_ERROR_KEY: string = '__auroraParseError';" in ETS_RUNNER and
      "bad[PARSE_FAILED_KEY] = true;" in ETS_RUNNER and "bad[PARSE_ERROR_KEY] = why;" in ETS_RUNNER)
check("3.2 parseSn() 记下两个事实位: 解析成功没有 / 原文自称什么",
      "r.rawParseOk = !BenchRunner.jBool(o, PARSE_FAILED_KEY, false);" in ETS_RUNNER and
      'r.rawSelfClaimOk = raw.startsWith(\'{"ok":true\');' in ETS_RUNNER)
check("3.3 SnRunResult 暴露这两个事实位",
      "rawParseOk: boolean;" in ETS_RUNNER and "rawSelfClaimOk: boolean;" in ETS_RUNNER)
check("3.4 解析失败时 error 一定非空(带 JSON.parse 的异常与原文前 120 字符)",
      "r.error = BenchRunner.jStr(o, PARSE_ERROR_KEY," in ETS_RUNNER and
      "'native 返回的 JSON 无法解析（JSON.parse 抛异常：'" in ETS_RUNNER)
check("3.5 native 说 ok=false 却没给原因时, 写明'它没给原因'(不留空、不印'（native 未给文本）')",
      "但没有给出任何原因文本（error 与 lastError 都是空）" in ETS_RUNNER)
check("3.6 统一入口 snFailReason(): 三种失败都有非空原因, 且把矛盾写清楚",
      "static snFailReason(sn: SnRunResult): string {" in ETS_FULL and
      "初始化失败（驱动原文）：" in ETS_FULL and
      "与原始 JSON 的自称相矛盾, 已核实并记录" in ETS_FULL and
      "但 native 没有给出任何原因文本（error 与 lastError 都是空）" in ETS_FULL)
check("3.7 小节状态与结论都走这个入口(报告 / 界面 / 三层呈现三处一致)",
      "line = FullReportBuilder.snFailReason(r) +" in ETS_FULL and
      "return '失败：' + FullReportBuilder.snFailReason(sn);" in ETS_FULL and
      "FullReportBuilder.snFailReason(r);" in ETS_INDEX and
      "FullReportBuilder.snFailReason(sn)" in ETS_LAYERS)
def code_only(src):
    """去掉整行注释后的源码(注释里留着"旧写法"的说明是本工程的惯例, 不影响判定)。"""
    return [l for l in src.splitlines() if not l.lstrip().startswith("//")]
idx_code = "".join(code_only(ETS_INDEX))
lay_code = "".join(code_only(ETS_LAYERS))
check("3.8 SNL 路径的代码里不再有'（native 未给文本）'这种空原因(界面两处已删; NPU 那两处不属本节)",
      "（native 未给文本）" not in idx_code and
      "native 未给文本" not in idx_code and
      "native 未给文本" not in lay_code)
check("3.9 报告 JSON 新增 sn 块(状态 / 非空失败原因 / 两个事实位 / 原始 JSON 在哪)",
      "interface RepSnJson {" in ETS_FULL and "parseOk: boolean;" in ETS_FULL and
      "selfClaimedOk: boolean;" in ETS_FULL and "failReason: string;" in ETS_FULL and
      """chunks.push(',"sn":');""" in ETS_FULL and
      "private static snJson(out: FullRunOutcome): RepSnJson {" in ETS_FULL)
check("3.10 .txt 的 F 段把两个事实位也印出来(解析=失败 时说明'下面各项是未取到, 不是读到 0')",
      "原始 JSON 解析=" in ETS_FULL and "原始 JSON 自称=" in ETS_FULL and
      "不是\"读到了 0\"" in ETS_FULL)

# ---- 真机复算: 这一轮的原始 JSON 到底是不是合法 JSON, 它自称什么, 是不是本轮的 ----
if rb is None:
    skip("3.11 真机 SNL 原始 JSON 的复算(合法性 / 自称 / 是不是本轮)", "找不到 " + R80_TXT)
else:
    txt = rb.decode("utf-8", errors="replace")
    mraw = re.search(r"原始 JSON=(\{.*?\})\n", txt, re.S)
    # SNL 段那一行是"原始 JSON={"ok":true,"section":"GPU-SNL"…}"
    i = txt.find('原始 JSON={"ok":true,"section":"GPU-SNL"')
    raw = ""
    if i >= 0:
        j = txt.find("\n", i)
        raw = txt[i + len("原始 JSON="):j if j > 0 else len(txt)]
    ok_parse = False
    if raw:
        try:
            json.loads(raw)
            ok_parse = True
        except Exception:
            ok_parse = False
    check("3.11 真机 SNL 的原始 JSON 文本自称 ok:true, 但它不是合法 JSON(所以解析得 0 个字段)",
          raw.startswith('{"ok":true') and not ok_parse and len(raw) > 1000)
    # 是不是本轮: 原始 JSON 末尾的 elapsedMs 应该略小于小节耗时 3419 ms
    mel = re.search(r'"elapsedMs":([0-9.]+)\}', raw)
    # 小节行与"状态=/耗时="分两行(见真机报告 [5/7] 那一段), 所以从 [5/7] 往后 200 字符里找
    s5 = txt.find("[5/7] GPU-SNL")
    sect = re.search(r"耗时=([0-9]+) ms", txt[s5:s5 + 400]) if s5 >= 0 else None
    check("3.12 这份原始 JSON 就是本轮的 SNL 结果(elapsedMs %.1f ms <= 小节耗时 %s ms, 同量级)"
          % (float(mel.group(1)) if mel else -1, sect.group(1) if sect else "?"),
          mel is not None and sect is not None and
          float(mel.group(1)) <= float(sect.group(1)) and float(mel.group(1)) > float(sect.group(1)) * 0.5)
    check("3.13 旧版小节状态=失败 + 结论里没有真实原因(现场: '（native 未给文本）') —— 这就是'矛盾'的出处",
          "状态=失败" in txt and "native 返回失败：（native 未给文本）" in txt)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[4] 缺陷 4: 报告 .txt 必须是合法 UTF-8(孤立代理项 -> U+FFFD, 硬断言)")
emit("=" * 100)
check("4.1 Utf8Guard 模块存在, 并导出三个入口(检查 / 替换 / 断言)",
      "export function checkReportText(s: string): Utf8Check {" in ETS_GUARD and
      "export function sanitizeReportText(s: string): string {" in ETS_GUARD and
      "export function assertTxtUtf8Safe(s: string): string {" in ETS_GUARD)
check("4.2 只动孤立代理项: 合法代理对(astral 字符/emoji)原样保留",
      "if (isLow(n)) {" in ETS_GUARD and "out += s.substring(i, i + 2);" in ETS_GUARD and
      "return s;" in ETS_GUARD)
check("4.3 正文拼装阶段(唯一能看到全文的位置)扫一遍并替换, 且把个数/位置写进报告",
      "文本编码自检（硬断言：报告 .txt 必须是合法 UTF-8）" in ETS_FULL and
      "const chk: Utf8Check = checkReportText(piece);" in ETS_FULL and
      "B.parts[ci] = sanitizeReportText(piece);" in ETS_FULL and
      "firstLoneAt = scannedChars + chk.firstIndex;" in ETS_FULL)
check("4.4 写盘处再兜一道: .txt 的每一块写之前过断言(触发次数会写进落盘回执)",
      "const isText: boolean = path.endsWith('.txt');" in ETS_FULL and
      "written += fs.writeSync(f.fd, isText ? txtGuardPiece(c) : c);" in ETS_FULL and
      "const why: string = assertTxtUtf8Safe(c);" in ETS_FULL and
      "文本编码兜底断言触发 " in ETS_FULL)
check("4.5 「下载」目录那一份 .txt 也过同一道断言(ReportSink)",
      "assertTxtUtf8Safe(body)" in ETS_SINK and "sanitizeReportText(body)" in ETS_SINK)
check("4.6 报告里解释了'为什么不是分块写入切断的'(现场证据写进了源码注释)",
      "分块写入把多字节字符切在块边界上" in ETS_FULL and
      "CESU-8" in ETS_GUARD and "546380" in ETS_GUARD)

# ---- Python 镜像: 与 ArkTS 逐字同口径 ----
def is_high(c): return 0xD800 <= c <= 0xDBFF
def is_low(c):  return 0xDC00 <= c <= 0xDFFF

def mirror_check(s):
    lone, first, i = 0, -1, 0
    while i < len(s):
        c = ord(s[i])
        if is_high(c):
            n = ord(s[i + 1]) if i + 1 < len(s) else 0
            if is_low(n):
                i += 2
                continue
            lone += 1
            if first < 0:
                first = i
            i += 1
            continue
        if is_low(c):
            lone += 1
            if first < 0:
                first = i
            i += 1
            continue
        i += 1
    return lone, first

def mirror_sanitize(s):
    out, i = [], 0
    while i < len(s):
        c = ord(s[i])
        if is_high(c):
            n = ord(s[i + 1]) if i + 1 < len(s) else 0
            if is_low(n):
                out.append(s[i:i + 2])
                i += 2
                continue
            out.append("\uFFFD")
            i += 1
            continue
        if is_low(c):
            out.append("\uFFFD")
            i += 1
            continue
        out.append(s[i])
        i += 1
    return "".join(out)

def encodable(s):
    try:
        s.encode("utf-8")
        return True
    except UnicodeEncodeError:
        return False

probe = "正常文本" + "\uddac\udc69\udece\udcbd" + "尾巴" + "\U0001F600" + "结尾" + "\ud83d"
lone, first = mirror_check(probe)
fixed = mirror_sanitize(probe)
check("4.7 镜像单测: 4 个孤立代理项 + 1 个合法代理对 + 1 个孤立高位 -> 只数出 5 个孤立, 且 emoji 保留",
      lone == 5 and first == 4 and "\U0001F600" in fixed and fixed.count("\uFFFD") == 5)
check("4.8 镜像单测: 替换后一定可编码成合法 UTF-8; 原文则不行(证明这就是非法字节的来源)",
      encodable(fixed) and not encodable(probe.replace("\U0001F600", ""))
      and not encodable("\uddac"))
check("4.9 镜像单测: 没有孤立代理项的文本一个字节都不动(幂等)",
      mirror_sanitize("普通文本 123 ABC") == "普通文本 123 ABC" and mirror_check("普通文本")[0] == 0)

# ---- 真机产物: 12 个非法字节到底是什么 ----
if rb is None:
    skip("4.10 真机 .txt 非法字节的复算", "找不到 " + R80_TXT)
else:
    # 第一步: 找出第一个非法字节的位置
    p = -1
    for off in range(len(rb)):
        try:
            rb[off:].decode("utf-8")
            p = -1
            break
        except UnicodeDecodeError as e:
            p = off + e.start
            break
    # 第二步: 从 p 往后找到第一个"从这里开始能干净解码"的位置 q —— [p, q) 就是那段非法字节
    bad = None
    if p >= 0:
        q = p
        while q < len(rb):
            try:
                rb[q:].decode("utf-8")
                break
            except UnicodeDecodeError:
                q += 1
        bad = (p, q)
    check("4.10 真机 .txt 确实是非法 UTF-8, 且非法字节是连续 12 个",
          bad is not None and (bad[1] - bad[0]) == 12, bad)
    if bad:
        seg = rb[bad[0]:bad[1]]
        # 逐个 3 字节序列解码(CESU-8: 每个序列解出一个代理码元)
        units = []
        ok3 = True
        for k in range(0, len(seg), 3):
            b3 = seg[k:k + 3]
            if len(b3) != 3 or (b3[0] & 0xF0) != 0xE0:
                ok3 = False
                break
            cp = ((b3[0] & 0x0F) << 12) | ((b3[1] & 0x3F) << 6) | (b3[2] & 0x3F)
            if not (0xD800 <= cp <= 0xDFFF):
                ok3 = False
                break
            units.append(cp)
        check("4.11 这 12 个字节 = 4 个完整的 3 字节 CESU-8 序列(每个解出一个代理码元 %s) "
              "=> 不是'分块写入把 1 个多字节字符切在块边界上'(那最多断 1 个字符 = <=3 字节)"
              % " ".join("U+%04X" % u for u in units),
              ok3 and len(units) == 4)
        # 这 4 个码元全是低位代理(DC00-DFFF): 连续两个低位不可能配成合法代理对,
        # 也不可能与前面的字符配成对 —— 所以它们全是孤立代理项(这一点由 4.11 的解码结果直接给出)。
        check("4.12 这 4 个码元全都落在代理区间且都是低位(%s) => 没有一个是合法代理对的一半"
              % " ".join("U+%04X" % u for u in units),
              len(units) == 4 and all(is_low(u) for u in units) and
              all(0xDC00 <= u <= 0xDFFF for u in units))
        check("4.13 镜像 sanitize 作用到真机现场文本后, 一定可编码成合法 UTF-8",
              encodable(mirror_sanitize(rb.decode("utf-8", errors="replace"))))

jb2 = read_bytes(R80_JSON)
if jb2 is None:
    skip("4.14 同一轮的 .json 是干净的(对照)", "找不到 " + R80_JSON)
else:
    clean = True
    try:
        jb2.decode("utf-8")
    except UnicodeDecodeError:
        clean = False
    check("4.14 同一轮的 .json 是合法 UTF-8(对照: 坏的不是数据, 是 .txt 的编码路径)", clean)
    check("4.15 .json 把同一处的 4 个孤立代理项写成了 \\uXXXX 转义(证明源字符串里本来就有孤立代理项, "
          "不是写盘时才坏的)",
          b"uddac" in jb2 and b"udc69" in jb2 and b"udece" in jb2 and b"udcbd" in jb2)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[5] 2026-10-05 缺陷 5: 同一份报告里「小节结论 / 排除项理由 / 参与项集合」必须互相对得上")
emit("=" * 100)
# 现场(8.1 真机 Pura X Max 2026-10-05 17:52, D:\gb7logs\phone81):
#   ① [3/7] 小节结论: "...已排除 5 项未通过随芯片变化自检的项) 2072 · 线程 8 · 计分 8 项 · 未拿到分 0 项"
#   ② 排除项明细:   File Compression(id=0: 本次没有拿到正的分数(计分项但这一遍没跑出吞吐)) ... 共 5 项
#   ③ items[]:     File Compression 176.9 / Photo Library 5446.1 / HDR 39.2 / Ray Tracer 3957.8 / Clang 14761.3
# 三句话互相打架。根因: ArkTS 把"未通过随芯片变化自检"的项置 0 之后才传给 native,
# native 只能把它写成"没有拿到正的分数"; 而置 0 用的是单核 ∪ 多核的并集 => 单核复合分也丢项。
# 本段把它做成能自己喊出来的断言(而不是靠人逐字读三句话):
#   N1 任一阶段: native 的「排除项」里不得把"items[] 该阶段有正分的项"写成"没有拿到正的分数";
#   N2 任一阶段: 复合分的参与项集合 == items[] 里该阶段有正分的项集合;
#   N3 代码侧: 传给 native 的数组不许再置 0(见 verify_composite_scope.py 的 [E]/[F])。
# 修复前的真机产物上 N1/N2 必须不成立(证明断言有效); 修复后的产物上必须成立。

def _ids_of(text):
    return sorted(set(int(x) for x in re.findall(r"\(id=(-?\d+)\)", text)))


def _excluded_rows(text):
    """把 native 的 excluded[] 文本拆成 [(名字, id, 原因)]。"""
    out = []
    for part in text.split(" · "):
        m = re.match(r"^(.+?)\(id=(-?\d+): (.*)\)$", part.strip())
        if m:
            out.append((m.group(1), int(m.group(2)), m.group(3)))
    return out


def report_scope_probe(path):
    if not os.path.exists(path):
        return None
    j = json.load(io.open(path, encoding="utf-8", errors="replace"))
    comp = j.get("composite", {}) or {}
    items = j.get("items", []) or []
    res = {"new": ("gb7SingleScopeOk" in comp), "stages": {}}
    for stage, pre in (("GB7 单核", "gb7Single"), ("GB7 多核", "gb7Multi")):
        positive = {}
        for it in items:
            if it.get("section") == stage and float(it.get("score") or 0) > 0:
                lid = it.get("loadId")
                if isinstance(lid, int) and lid >= 0:
                    positive[lid] = it.get("name")
        names = set(positive.values())
        lies = [n for (n, _i, why) in _excluded_rows(str(comp.get(pre + "Excluded", "")))
                if n in names and "没有拿到正的分数" in why]
        actual = _ids_of(str(comp.get(pre + "Items", "")))
        res["stages"][pre] = {
            "positive": sorted(positive.keys()), "actual": actual,
            "lies": lies,
            "set_ok": (len(actual) > 0 and set(actual) == set(positive.keys())),
        }
    return res


for tag, pth in (("8.1 phone81", r"D:\gb7logs\phone81\report-latest.json"),
                 ("8.0 r80", r"D:\gb7logs\r80\report-latest.json")):
    pr = report_scope_probe(pth)
    if pr is None:
        skip("5.x [%s] 真机报告不存在" % tag, pth)
        continue
    for pre, cn in (("gb7Single", "单核"), ("gb7Multi", "多核")):
        st = pr["stages"][pre]
        if pr["new"]:
            check("5.x [%s] %s: 排除项里不再把'有正分的项'写成'没有拿到正的分数'" % (tag, cn),
                  len(st["lies"]) == 0, st["lies"])
            check("5.x [%s] %s: 复合分参与项集合 == items[] 有正分项集合" % (tag, cn),
                  st["set_ok"], "参与项 %s vs 有正分 %s" % (st["actual"], st["positive"]))
        else:
            check("5.x [%s] %s: 历史产物命中(修复前这里必须不成立 -> 证明断言有效)" % (tag, cn),
                  (not st["set_ok"]) and len(st["lies"]) > 0,
                  "被谎报成'没有拿到正的分数'的有正分项: %s; 参与项 %s vs 有正分 %s" %
                  (st["lies"], st["actual"], st["positive"]))

# N3: 代码侧(与 verify_composite_scope.py 的 [E]/[F] 同一条不变量, 这里只核最要紧的一行)
def _runner_code():
    """去掉整行注释(// 与块注释续行 *; 注释里留着"旧写法"的说明是本工程惯例, 不参与代码判定)。"""
    keep = []
    for l in ETS_RUNNER.splitlines():
        t = l.lstrip()
        if t.startswith("//") or t.startswith("*") or t.startswith("/*"):
            continue
        keep.append(l)
    return "\n".join(keep)


check("5.3 报告侧不再有'按自检结论把分数置 0'的写法(GB7 复合分口径)",
      "skipIds" not in _runner_code() and
      "static gb7ScoreArray(list: Gb7Result[]): number[]" in ETS_RUNNER and
      "arr[r.loadId] = r.score;" in ETS_RUNNER)
check("5.4 报告侧有运行时硬核对: 参与项集合 vs items[] 有正分的项集合",
      "function compositeScopeCheck(list: Gb7Result[], json: string): ScopeCheck {" in ETS_FULL and
      "参与项核对不通过" in ETS_FULL and
      "gb7SingleScopeOk: crep.singleScopeOk," in ETS_FULL)
check("5.5 小节结论不再出现'已排除 N 项'这种与'计分 N 项 / 未拿到分 0 项'打架的写法",
      "已排除 ' + n.toString() + ' 项未通过随芯片变化自检的项" not in ETS_FULL and
      "仍按同一口径计入上面的复合分" in ETS_FULL)

# ===========================================================================
emit("")
emit("=" * 100)
emit("结果: %d 项 PASS, %d 项 FAIL, %d 项 SKIP" % (len(passes), len(fails), len(skips)))
if fails:
    emit("FAIL 清单:")
    for f in fails:
        emit("  - " + f)
else:
    emit("全部 PASS" + ("（%d 项因真机产物缺失而 SKIP）" % len(skips) if skips else ""))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
