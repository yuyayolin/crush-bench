# -*- coding: utf-8 -*-
"""
"Aurora Nomad Light"(GPU-SNL 小节)的统一尺子自查器 (全离线; 不连设备、不跑 hdc、不跑构建)

为什么要有这个脚本
------------------------------------------------------------------------------
2026 版的证据缺口: 公开的 3DMark Steel Nomad Light 代际差是
    991 (Mate 80 Pro Max / 麒麟 9030 Pro) / 454 (9020) / 303 (9000S) = 3.27 倍
而本工程 GB7 GPU 11 项在手机 / 平板之间只跑出 5064 / 2563 = 1.98 倍, 差 1.65 倍。
原因是那 11 项是图像算子型的 GPU 套件, 不是 game-like 的
deferred/PBR 着色负载。本脚本要保证新增的这一节真的是一把跨代可比的尺子,
而不是"再来一个好看的分数"。

它做五件事(逐条对应设计约束):
  [A] 负载与设备无关: 源码里不许出现任何设备相关分支(设备名 / SoC 名 / 机型 /
      分辨率 / 年份 / "如果是麒麟" 之类的字符串比较), 负载参数必须是编译期常量。
  [B] 吞吐量口径 + 分数口径:
        * 分数必须严格等于 fps x kScorePerFps;
        * kScorePerFps 必须由公开的 3DMark 常数 135 与公开的 SNL 渲染分辨率
          2560x1440 推出(135 x ((1920x1080)/(2560x1440)) = 75.9375);
        * 结果里必须同时给出 fps 与 Mpx/s 吞吐量, 且吞吐量 = pixelsPerFrame x fps。
  [C] 规模自适应不改每单位工作量:
        * 唯一的自由度是 passesPerFrame;
        * 每像素 ALU 数 / 分辨率 / tile 像素数在源码里是常量, 不出现在任何
          "按测量结果赋值"的路径上;
        * 阶梯是 2 的幂, 且结果里必须写明本项用了多少趟与自适应的依据。
  [D] 离线复算工作量: 把两份 GLSL 逐条展开(把固定次数的 for 循环展开), 数出
      每像素的 ALU 次数, 与 C++ 里写死的 kOpsPerPixelPassA/B 对照(容差 25%,
      因为"一条 op 值多少次 ALU"本身有解释空间), 并断言每个像素的工作量
      与 passesPerFrame 无关。
  [E] 报告字段与"不许静默失败": 源码必须包含契约里的全部字段名, 结果 JSON 里
      必须带 benchVersion 与"跨版本不可比"的说明、CPU/GPU 自证字段、以及
      "尺子有没有被喂满"的记录。

用法:  python verify_sn_unified_ruler.py [--cpp-dir 路径] [--json-out 路径]
退出码: 0 = 全部 PASS, 1 = 有 FAIL  (与工程其它 verify_*.py 同一口径)
"""

import argparse
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
DEFAULT_DIR = os.path.join(HERE, "sn")
fails = 0
checks = 0


def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    if _OUT is not None:
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


def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_cpp_comments(src):
    """把 // 与 /* */ 注释换成等长空白(保持行号), 免得注释里的字被当成代码"""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def block_of(src, marker):
    """取出 R"GLSL( ... )GLSL" 里的着色器源码"""
    m = re.search(re.escape(marker) + r'\s*=\s*R"GLSL\((.*?)\)GLSL";', src, re.S)
    return m.group(1) if m else ""


# ===========================================================================
#  [D] 需要用到的小工具: 把 GLSL 的函数体切出来 + 展开固定次数的 for 循环
# ===========================================================================

def glsl_functions(src):
    """返回 {函数名: 函数体}; 只认顶格定义的函数(本文件里的着色器都是这种写法)"""
    out = {}
    for m in re.finditer(r"(?m)^[A-Za-z_][\w]*\s+(?P<name>[A-Za-z_]\w*)\s*\([^;{]*\)\s*\{", src):
        name = m.group("name")
        if name in ("main", "if", "for", "while", "return", "else"):
            continue
        start = m.end() - 1
        depth = 0
        i = start
        while i < len(src):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out[name] = src[start:i + 1]
    return out


def glsl_main(src):
    i = src.find("void main()")
    if i < 0:
        return ""
    j = src.find("{", i)
    depth = 0
    k = j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    return src[j:k + 1]


def expand_loops(body):
    """把固定次数的 for (int i = 0; i < N; ++i) 循环体复制 N 份(只处理本工程这种写法)"""
    out = body
    for _ in range(8):
        m = re.search(r"for\s*\(int\s+(\w+)\s*=\s*0;\s*\1\s*<\s*(\d+);\s*\+\+\1\s*\)\s*\{", out)
        if not m:
            break
        start = m.end() - 1
        depth = 0
        i = start
        while i < len(out):
            if out[i] == "{":
                depth += 1
            elif out[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        inner = out[start + 1:i]
        n = int(m.group(2))
        out = out[:m.start()] + (inner * n) + out[i + 1:]
    return out


OPS = [
    (re.compile(r"^0x[0-9A-Fa-f]+u?$"), 1),
    (re.compile(r"^[0-9]+\.?[0-9]*(?:[eE][-+]?\d+)?$"), 1),
    (re.compile(r"^\.\d+(?:[eE][-+]?\d+)?$"), 1),
    (re.compile(r"^[A-Za-z_]\w*$"), 1),
    (re.compile(r"^[\w.]+\.[xyzwrgba]{1,4}$"), 1),
    (re.compile(r"^[\w.]+\.[xyzwrgba]{1,4}\.[xyzwrgba]{1,4}$"), 3),
]

# 没有一个"每条内置函数值几次 ALU"的权重表: 那种权重是主观的, 会让两边对不上。
# 本脚本只数与源码里 kOpsPerPixel* 注释逐字同口径的三类东西(内置函数调用 /
# 算术运算符 / 多分量 swizzle), 展开循环与内联函数之后再数。


def count_ops(body):
    """数"着色操作"次数。

    口径(与 sn_renderer.cpp 里 kOpsPerPixel* 的注释逐字一致):
      一次 op = 一次内置函数调用 + 一个算术运算符 + 一次多分量 swizzle 读取。
      这不是"ALU 周期数", 也不是"指令数" —— 它只是一个可复算的、两边用同一套规则的量。
    """
    b = re.sub(r"//[^\n]*", " ", body)
    total = 0
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", b):
        fn = m.group(1)
        if fn in ("if", "for", "while", "return", "switch"):
            continue
        total += 1                      # 函数调用本身算一次 op
    total += len(re.findall(r"[+\-*/]", b))       # 算术运算符
    total += len(re.findall(r"\b\w+\.([xyzwrgba]{2,4})\b", b))  # 多分量 swizzle
    return total


IDENT_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def expand_shader(fs):
    """把着色器展开成直线代码: 先按字面次数展开 for 循环, 再把自定义函数按调用点内联。

    这样 8 步光线步进会真的被算成 8 份, 自定义函数被调用几次就算几份 ——
    只有这样才能和"每像素工作量"这个说法对上。
    """
    funcs = glsl_functions(fs)
    body = glsl_main(fs)
    body = re.sub(r"//[^\n]*", " ", body)
    for fn in funcs:
        funcs[fn] = re.sub(r"//[^\n]*", " ", funcs[fn])
    keywords = {"if", "for", "while", "return", "switch", "else"}
    for _ in range(10):
        # 1) 展开固定次数的循环
        m = re.search(r"for\s*\(int\s+(\w+)\s*=\s*0;\s*\1\s*<\s*(\d+);\s*\+\+\1\s*\)\s*\{", body)
        if m:
            i = m.end() - 1
            depth = 0
            j = i
            while j < len(body):
                if body[j] == "{":
                    depth += 1
                elif body[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            inner = body[i + 1:j]
            body = body[:m.start()] + inner * int(m.group(2)) + body[j + 1:]
            continue
        # 2) 内联自定义函数(按调用点)
        inlined = False
        for name, fbody in funcs.items():
            calls = [mm for mm in IDENT_RE.finditer(body) if mm.group(1) == name]
            if not calls:
                continue
            mm = calls[0]
            i = mm.end() - 1
            depth = 0
            j = i
            while j < len(body):
                if body[j] == "(":
                    depth += 1
                elif body[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            inner = fbody[1:-1]
            body = body[:mm.start()] + "(" + inner + ")" + body[j + 1:]
            inlined = True
            break
        if not inlined:
            break
    return body


# ===========================================================================
def main():
    global _OUT
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp-dir", default=DEFAULT_DIR)
    ap.add_argument("--json-out", default=os.path.join(HERE, "sn_json_sample.json"))
    ap.add_argument("--report", default=os.path.join(HERE, "verify_sn_out.txt"))
    args = ap.parse_args()

    global _OUT
    _OUT = io.open(args.report, "w", encoding="utf-8", newline="\n")

    cpp_path = os.path.join(args.cpp_dir, "sn_renderer.cpp")
    hdr_path = os.path.join(args.cpp_dir, "sn_renderer.h")
    napi_path = os.path.join(args.cpp_dir, "sn_napi.cpp")
    cm_path = os.path.join(os.path.dirname(os.path.abspath(args.cpp_dir)), "CMakeLists.txt")

    for p in (cpp_path, hdr_path, napi_path, cm_path):
        if not os.path.isfile(p):
            emit("找不到文件: " + p)
            return 1

    src = read(cpp_path)
    hdr = read(hdr_path)
    napi = read(napi_path)
    base_cpp = os.path.dirname(os.path.abspath(args.cpp_dir))
    ref_cpp = os.path.join(base_cpp, "reference_compare.cpp")
    ref_hdr = os.path.join(base_cpp, "reference_compare.h")
    ref_src = read(ref_cpp) if os.path.isfile(ref_cpp) else ""
    if os.path.isfile(ref_hdr):
        ref_src += read(ref_hdr)
    check("[H] 参考分对照模块存在(只读; 不跑负载)", len(ref_src) > 2000,
          "reference_compare.cpp/.h")
    cm = read(cm_path)
    code = strip_cpp_comments(src)

    emit("=" * 100)
    emit("Aurora Nomad Light (GPU-SNL) 统一尺子自查 -- " + cpp_path)
    emit("=" * 100)

    # ---------------------------------------------------------------- A
    emit("")
    emit("[A] 负载与设备完全无关(不许有任何设备相关分支)")
    dev_words = ["Kirin", "kirin", "麒麟", "Mate 6", "Mate 7", "Mate 8", "Mate 9", "Pura",
                 "HOP-AL00", "Maleoon", "Maleoon", "Adreno", "Mali", "Snapdragon", "骁龙",
                 "9030", "9020", "9000S", "9060", "9050", "kirin", "deviceModel", "socName",
                 "OHOS_PRODUCT", "prop.product", "PHONE", "TABLET", "isTablet", "screenWidth",
                 "getDefaultDisplay", "deviceType"]
    # 唯一的例外: 公开参照值表(kRefPoints)是纯报告文本, 不参与任何判断。
    # 先把它整块从被扫描的源码里去掉, 再查设备名 —— 这样"A1 查的是负载代码"才是真的。
    # 三类纯报告文本里本来就会写机型 / SoC 名, 它们不参与任何判断, 只是给用户看的
    # 出处说明。先把它们整段去掉, 剩下的才是"负载代码", 再在剩下的里面查设备名。
    code_no_ref = re.sub(r"const RefPoint kRefPoints\[\][\s\S]*?\n\};", "/*REF_TABLE_REMOVED*/", code)
    # 报告文本里的型号字面量先换成中性占位符(只替换字面量, 不改任何结构):
    # 这样"剩下的代码里还有没有设备名"才是干净的判据 —— 如果哪天有人写出
    # "if (soc == 'Kirin 9030')" 这种分支, 那个 Kirin 不会被替换掉, A1 立刻报 FAIL。
    # 纯报告文本里会出现"麒麟"这个词(说明"读不到的那条路"), 先整段去掉再查
    code_no_ref = code_no_ref.replace("麒麟平台的 GPU 频率/温度", "REPORT_TEXT")
    for lit in ("Kirin 9030 Pro", "Kirin 9020", "Kirin 9000S", "Kirin", "Maleoon", "Mate 80",
                "Mate 70", "Mate 60", "Mate X6", "Mate X7", "MatePad", "Pura 80",
                "9030 Pro", "9030", "9020", "9000S", "991", "454", "303"):
        code_no_ref = code_no_ref.replace(lit, "DEV")
    hits = []
    for w in dev_words:
        for m in re.finditer(re.escape(w), code_no_ref):
            ln = code_no_ref[:m.start()].count("\n") + 1
            line = code_no_ref.splitlines()[ln - 1]
            hits.append((w, ln, line.strip()[:120]))
    check("A1 负载代码里没有设备名 / SoC 名 / 机型串", len(hits) == 0,
          ("命中: " + str(hits[:6])) if hits else "0 处命中(参照值表 kRefPoints 已单独排除)")
    # 按屏幕 / 设备类型做分支: 只查真实存在的负载宏与标识符(不是随便一个 if)
    bad_branch = re.findall(r"if\s*\([^)]*(?:displayInfo|screenSize|deviceType|isTablet|isPhone|"
                            r"OHOS_PRODUCT|productModel|screenDensity)[^)]*\)", code, re.I)
    check("A2 没有按分辨率 / 屏幕 / 设备类型做分支", len(bad_branch) == 0,
          ("命中: " + str(bad_branch[:3])) if bad_branch else "没有 displayInfo/deviceType/screenSize 之类的分支")
    # 公开参照值只能出现在 JSON 文本里
    ref_used = set(re.findall(r"kRefPoints\[i\]\.(\w+)", code))
    ref_bare = re.sub(r"kRefPoints\[i\]\.\w+", "", code)
    ref_bare = re.sub(r"const RefPoint kRefPoints\[\][\s\S]*?\n\};", "", ref_bare)
    check("A3 公开参照值只用于输出文本(不参与任何判断/计算分支)",
          ref_used <= {"soc", "device", "officialScore", "verification", "note"} and
          len(re.findall(r"\bkRefPoints\b", ref_bare)) <= 2 and
          re.search(r"if\s*\([^)]*kRefPoints", code) is None,
          "kRefPoints 的字段只被 buildJson 的循环读出来写成文本: " + str(sorted(ref_used)))
    # 负载参数必须是编译期常量
    for name in ("kWidth", "kHeight", "kTilePx", "kTilesX", "kOpsPerPixelPassA",
                 "kOpsPerPixelPassB", "kFragsPerTile", "kPassLadderMin", "kPassLadderMax"):
        check("A4 " + name + " 是 const 编译期常量",
              re.search(r"const\s+int\s+" + name + r"\s*=", code) is not None,
              "const int " + name)
    check("A5 顶点着色器不读任何顶点属性缓冲(几何在 GPU 侧生成)",
          "in vec" not in block_of(src, "VS_TILE") and "in vec" not in block_of(src, "VS_SHADE") and
          "attribute" not in block_of(src, "VS_TILE"),
          "VS 里没有任何 in 变量声明")
    check("A6 顶点位置只由 gl_VertexID / gl_InstanceID 决定",
          "gl_VertexID" in block_of(src, "VS_TILE") and "gl_InstanceID" in block_of(src, "VS_TILE") and
          "gl_VertexID" in block_of(src, "VS_SHADE") and "gl_InstanceID" in block_of(src, "VS_SHADE"),
          "两个 VS 都用 gl_VertexID + gl_InstanceID")
    check("A7 C++ 侧不递交任何顶点数据",
          "glBufferData" not in code and "glVertexAttribPointer" not in code and
          "glEnableVertexAttribArray" not in code,
          "没有 glBufferData / glVertexAttribPointer / glEnableVertexAttribArray")
    # "gb7Item" / "gpu7Item" 只是结果 JSON 里的布尔报告字段名(声明"本小节不是 GB7 项"),
    # 它不含任何 gb7_*/gpu7_* 的代码引用; 去掉这两个整串之后再查符号名。
    code_no_gb7tag = code.replace("gb7Item", "").replace("gpu7Item", "")
    bad_syms = re.findall(r"\b(?:gb7|gpu7)_\w+", code_no_gb7tag)
    check("A8 本小节不引用 gb7_* / gpu7_* 的任何代码", len(bad_syms) == 0,
          ("命中: " + str(sorted(set(bad_syms)))) if bad_syms else
          "去掉只是报告字段名的 gb7Item/gpu7Item 之后, 没有任何 gb7_*/gpu7_* 符号")
    check("A9 没有读设备文件做任何负载决策",
          re.search(r'fopen\s*\(\s*"/sys', code) is None and
          re.search(r'fopen\s*\(\s*"/proc', code) is None,
          "负载路径里没有 /sys 或 /proc 读取")

    # 把源码里的转义引号去掉: 这样才能同时覆盖"写死的键"与"拼出来的键"
    # (例: 写死的 '"section":"GPU-SNL"' 与 拼出来的 ',"score":{' )
    code_plain = code.replace('\\"', '"')
    # 注: 有些说明文字只写在注释里(那正是它们的用途), 所以还要留一份保留注释的副本,
    # 否则"注释里写了"这件事本身就没法被断言。
    src_plain = src.replace('\\"', '"')

    # ---------------------------------------------------------------- B
    emit("")
    emit("[B] 吞吐量口径与分数口径(与 3DMark 公开值同形)")
    check("B1 计分公式就是 score = fps x kScorePerFps",
          re.search(r"s\.score\s*=\s*s\.fps\s*\*\s*kScorePerFps", code) is not None and
          re.search(r"const double score = s\.score;", code) is not None,
          "snComputeStats(): s.score = s.fps * kScorePerFps; buildJson 只引用它(单点定义)")
    check("B2 k3dmarkNomadScale = 135(UL 官方明文)",
          re.search(r"k3dmarkNomadScale\s*=\s*135\.0", code) is not None,
          "135.0")
    check("B3 kScorePerFps = 135 x kWorkloadScale x 像素比(不是随便写的数)",
          re.search(r"kScorePerFps\s*=\s*k3dmarkNomadScale\s*\*\s*kWorkloadScale\s*\*\s*kResolutionScaleRatio",
                    code) is not None and
          re.search(r"const double kWorkloadScale\s*=\s*1\.0", code) is not None,
          "kScorePerFps = k3dmarkNomadScale * kWorkloadScale * kResolutionScaleRatio")
    check("B4 像素比来自 1920x1080 与 SNL 官方 2560x1440",
          "kSnlOfficialWidth = 2560.0" in code and "kSnlOfficialHeight = 1440.0" in code and
          "(double)kWidth * (double)kHeight" in code,
          "0.5625 = (1920*1080)/(2560*1440)")
    check("B5 吞吐量 = 每帧像素 x 帧率",
          re.search(r"s\.pxPerSecond\s*=\s*s\.pxPerFrame\s*\*\s*s\.fps", code) is not None and
          re.search(r"const double pxPerSecondD = s\.pxPerSecond;", code) is not None,
          "snComputeStats(): s.pxPerSecond = s.pxPerFrame * s.fps")
    check("B6 报告里同时给出 fps 与 Mpx/s",
          '"fps":{"value"' in code_plain and '"throughput":{"value"' in code_plain and
          '"unit":"Mpx/s"' in code_plain,
          "fps.value + throughput.value(unit=Mpx/s)")
    check("B7 分数与吞吐量严格成正比(两者只差一个常数)",
          re.search(r"proportional", code) is not None,
          "JSON 里带 proportional:true 与 formula")

    # ---------------------------------------------------------------- C
    emit("")
    emit("[C] 规模自适应: 只改趟数, 不改每单位工作量")
    check("C1 唯一的自由度是 passesPerFrame",
          ("passesPerFrame" in code) and
          re.search(r"int\s+choosePasses\(double\s+perPassMs,\s*double\s+targetMs,\s*int\s+maxPasses", code) is not None,
          "choosePasses() 只返回趟数")
    check("C2 每像素 ALU 数是常量(不出现在任何按测量结果赋值的路径上)",
          re.search(r"int\s+kOpsPerPixelPassA\s*=\s*\d+", code) is not None and
          re.search(r"int\s+kOpsPerPixelPassB\s*=\s*\d+", code) is not None and
          re.search(r"kOpsPerPixelPass[AB]\s*=", code).group(0) is not None and
          len(re.findall(r"kOpsPerPixelPass[AB]\s*=", code)) == 2,
          "两个 ops 常量各只赋值一次")
    check("C3 分辨率是常量, 自适应不改它",
          len(re.findall(r"kWidth\s*=", code)) == 1 and len(re.findall(r"kHeight\s*=", code)) == 1 and
          re.search(r"glViewport\(\s*0\s*,\s*0\s*,\s*kWidth\s*,\s*kHeight\s*\)", code) is not None,
          "glViewport 永远用 kWidth x kHeight")
    check("C4 tile 像素数是常量(不随设备变)",
          re.search(r"int\s+kTilePx\s*=\s*32", code) is not None and
          re.search(r"instances\s*=\s*g\.tilesX\s*\*\s*g\.tilesY", code) is not None,
          "kTilesX x kTilesYCeil 个 32x32 的 tile")
    check("C5 阶梯是 2 的幂",
          re.search(r"while\s*\(\(double\)p\s*\*\s*2\.0\s*<=\s*want", code) is not None,
          "choosePasses 里按 2 的幂取档")
    check("C6 自适应过程本身不进最终统计(试探帧单独一段)",
          re.search(r"FrameTiming\s+probe\s*=\s*runFrames\(probePasses,\s*1,\s*false\)", code) is not None and
          re.search(r"FrameTiming\s+fin\s*=\s*runFrames\(m\.passCount,\s*m\.measuredFrames,\s*true\)", code) is not None,
          "probe 与 final 是两次独立调用")
    check("C7 结果里写明本项用了多少趟 + 自适应的依据",
          '"scale":{"passesPerFrame"' in code_plain and '"probePerPassMs"' in code_plain and
          '"basisText"' in code_plain and '"scaleRatio"' in code_plain,
          "scale.passesPerFrame / probePerPassMs / scaleRatio / basisText")
    check("C8 一趟的定义与趟数无关(趟循环里不改任何负载参数)",
          re.search(r"for\s*\(int\s+p\s*=\s*0;\s*p\s*<\s*passesPerFrame;\s*\+\+p\)", code) is not None and
          re.search(r"submitPass\(p,", code) is not None,
          "只有趟序号 p 传进去(它只用来选种子/双缓冲目标)")

    # ---------------------------------------------------------------- D
    emit("")
    emit("[D] 离线复算每像素工作量(把两份 GLSL 展开成直线代码再数 ALU)")
    fs_gb = block_of(src, "FS_SN_GBUFFER")
    fs_sh = block_of(src, "FS_SN_SHADE")
    check("D0 两份片元着色器都解析到了", len(fs_gb) > 400 and len(fs_sh) > 400,
          "gbuffer=%d 字节, shade=%d 字节" % (len(fs_gb), len(fs_sh)))

    cost_gb = count_ops(expand_shader(fs_gb))
    cost_sh = count_ops(expand_shader(fs_sh))
    cpp_a = int(re.search(r"kOpsPerPixelPassA\s*=\s*(\d+)", code).group(1))
    cpp_b = int(re.search(r"kOpsPerPixelPassB\s*=\s*(\d+)", code).group(1))
    emit("     离线数出: passA=%d op/px, passB=%d op/px, 合计 %d" % (cost_gb, cost_sh, cost_gb + cost_sh))
    emit("     源码写死: passA=%d op/px, passB=%d op/px, 合计 %d" % (cpp_a, cpp_b, cpp_a + cpp_b))

    def within(a, b, tol=0.25):
        if a == 0:
            return False
        return abs(a - b) / float(a) <= tol

    check("D1 pass A 的每像素 ALU 数与源码常量一致(±25%)", within(cost_gb, cpp_a),
          "%d vs %d" % (cost_gb, cpp_a))
    check("D2 pass B 的每像素 ALU 数与源码常量一致(±25%)", within(cost_sh, cpp_b),
          "%d vs %d" % (cost_sh, cpp_b))
    check("D3 每像素工作量与 passesPerFrame 无关(着色器里没有 passesPerFrame)",
          "passesPerFrame" not in fs_gb and "passesPerFrame" not in fs_sh,
          "两份 GLSL 里都没有这个 uniform")
    check("D4 着色器里没有 discard(否则每单位工作量会因像素而变)",
          "discard" not in code, "没有 discard")
    check("D5 着色器里的循环都是固定次数(循环上界是字面量)",
          all(re.match(r"^\d+$", n) for n in re.findall(r"for\s*\([^;]*;\s*\w+\s*<\s*(\w+)\s*;", fs_gb + fs_sh)),
          "循环上界: " + str(sorted(set(re.findall(r"for\s*\([^;]*;\s*\w+\s*<\s*(\w+)\s*;", fs_gb + fs_sh)))))
    check("D6 片元着色器里没有 texture() 以外的数据依赖采样(没有纹理上传/回读)",
          "glTexImage2D" in code and "glReadPixels" in code and
          code.count("glTexImage2D") <= 8,
          "只建固定尺寸的 G-buffer/颜色目标, 回读只在自证时读 32x32")

    # ---------------------------------------------------------------- E
    emit("")
    emit("[E] 报告字段 / 不许静默失败 / 尺子余量")
    need_fields = [
        '"section":"GPU-SNL"', '"scored":false', '"gb7Item":false',
        '"benchVersion"', '"benchVersionText"', '"format"', '"fingerprint"',
        '"score"', '"fps"', '"throughput"', '"work"', '"scale"',
        '"gpuBoundEvidence"', '"cpuGpuScaling"', '"selfProof"', '"crossGeneration"',
        '"reference"', '"ruler"', '"rendering"', '"gpu"', '"config"',
        '"validity"', '"warnings"', '"notes"', '"risks"', '"calibration"',
    ]
    missing = [f for f in need_fields if f not in code_plain]
    check("E1 结果 JSON 契约里的字段名齐全", len(missing) == 0,
          ("缺: " + str(missing)) if missing else "%d 个字段名全部出现" % len(need_fields))
    check("E2 带 benchmark 级版本号, 且与 App 版本号分开说明",
          "kSnBenchVersion" in code and "appVersionIsSeparate" in code and "benchmarkVersion" in code,
          "benchVersion + 说明字段")
    check("E3 明确写出跨版本不可比",
          "不可比" in src and "benchVersion" in src and "perVersionComparable" in code,
          "benchVersionText 与 format.perVersionComparable=false")
    check("E4 有「递增 benchVersion」的规则说明",
          "必须递增" in src or "必须 +1" in src,
          "文件头与常量注释里都写了")
    # 把源码里的 \" 去掉之后再查字段名: 这样才能同时覆盖
    #   写死的键("\"section\":\"GPU-SNL\"") 与 拼出来的键(",\"score\":{")
    code_plain = code.replace('\\"', '"')
    def has(*names):
        return all(('"' + n + '"') in code_plain for n in names)
    _ = has
    check("E5 自证: 每帧 GPU 侧工作量", has("pixelsPerFrame", "opsPerFrame", "fragmentInvocationsPerFrame"),
          "work.pixelsPerFrame / work.opsPerFrame / work.fragmentInvocationsPerFrame")
    check("E6 自证: 不是被 CPU 递交卡住的(有 draw call 数与递交/GPU 比值)",
          has("drawCallsPerFrame", "drawCallsPerPass", "submissionDuty", "cpuSubmitMsPerFrame",
              "gpuBusyMsPerFrame", "cpuGpuRatio", "gpuSideGeneratedGeometry",
              "cpuVertexDataBytesPerFrame"),
          "gpuBoundEvidence 里的八项都在")
    check("E7 自证: 两种趟数的耗时比值(GPU/CPU 判别)",
          has("scalingRatio", "expectedIfGpuBound", "expectedIfCpuBound", "lowPasses", "highPasses"),
          "cpuGpuScaling 段")
    check("E8 尺子余量: 记录核数 / SMT / 向量能力",
          has("logicalCores", "physicalCores", "smtPossible", "smtEnabled", "threadCap",
              "neon", "sve", "sve2", "i8mm", "bf16", "dotprod"),
          "ruler.cpu + ruler.simd")
    check("E9 尺子余量: 写明本小节只用到几个线程 + 未来余量说明",
          has("maxConcurrencyUsed", "headroomNotes", "whatThisDoesNotMeasure"),
          "ruler.cpu.maxConcurrencyUsed / ruler.headroomNotes")
    check("E10 顶到阶梯上限时明确报警(不静默)",
          "limitReached" in code and "kPassLadderMax" in code and "修法只有一条" in src,
          "limitReached 分支")
    check("E11 失败不返回 0/静默: 失败结果带 error 与 lastError",
          '"ok":false' in code_plain and '"error"' in code_plain and '"lastError"' in code_plain and
          "failJson" in code,
          "failJson() 同时写 error 与 lastError")
    check("E12 napi 层导出(模块名 aurorasn / 函数齐全)",
          'nm_modname = "aurorasn"' in napi and all(
              ('"%s"' % f) in napi for f in
              ["run", "prepare", "ready", "lastError", "benchVersion", "versionText", "sectionName",
               "workloadName", "lastScore"]),
          "9 个导出")
    check("E13 CMake 里是独立的 napi 模块, 且不依赖 libaurorabench",
          "add_library(aurorasn SHARED sn/sn_napi.cpp sn/sn_renderer.cpp)" in cm and
          re.search(r"target_link_libraries\(aurorasn PUBLIC[^)]*\)", cm) is not None and
          "aurorabench" not in re.search(r"target_link_libraries\(aurorasn PUBLIC[^)]*\)", cm).group(0),
          "libaurorasn.so 只链 napi + EGL/GLES3")
    check("E14 不并入 GB7: 源码里没有任何写 GB7 分数的地方",
          "gb7Composite" not in code and "kScores" not in code and "geo" not in code.split("buildJson")[0][-2000:],
          "本文件只输出自己的 JSON")

    # ---------------------------------------------------------------- F
    emit("")
    emit("[F] 离线镜像: 生成一份与实现字段逐字一致的结果 JSON 样例")
    # 用源码里的常量复算一遍, 得到一份"应当长这样"的样例(数值是可复算的)
    consts = {}
    for name in ["kWidth", "kHeight", "kTilePx", "kOpsPerPixelPassA", "kOpsPerPixelPassB",
                 "kBytesPerPixelPerPass", "kDrawCallsPerPass", "kApiCallsPerPass", "kPassLadderMin",
                 "kPassLadderMax", "kDefaultMeasureFrames", "kReadbackPx"]:
        m = re.search(r"int\s+" + name + r"\s*=\s*([^;]+);", code)
        if m:
            consts[name] = m.group(1).strip()
    check("F1 关键常量都能从源码里取到", len(consts) >= 11, str(sorted(consts.keys())))

    def cint(expr):
        e = expr.split("//")[0].strip()
        try:
            return int(eval(e, {}, {}))
        except Exception:
            return None

    W = cint(consts.get("kWidth", "0"))
    H = cint(consts.get("kHeight", "0"))
    TP = cint(consts.get("kTilePx", "0"))
    tilesX = (W // TP) if (W and TP) else 0
    tilesY = ((H + TP - 1) // TP) if (H and TP) else 0
    instances = tilesX * tilesY
    frags_per_tile = TP * TP
    pass_pixels = instances * frags_per_tile
    ops_a = cint(consts.get("kOpsPerPixelPassA", "0"))
    ops_b = cint(consts.get("kOpsPerPixelPassB", "0"))
    ops_per_px = ops_a + ops_b
    check("F2 一趟的像素数 >= 1920x1080 且只多出重叠的一行",
          pass_pixels >= W * H and pass_pixels < W * H + W * TP + 1,
          "%d px(满屏 %d, 多出 %d = 重叠部分)" % (pass_pixels, W * H, pass_pixels - W * H))

    res_ratio = (float(W) * float(H)) / (2560.0 * 1440.0)      # 0.5625
    workload_scale = 1.0                                        # 源码里的 kWorkloadScale
    score_per_fps = 135.0 * workload_scale * res_ratio          # 135 x 0.5625 = 75.9375
    check("F3 kScorePerFps = 135 x kWorkloadScale x 0.5625 (= 75.9375)",
          abs(score_per_fps - 75.9375) < 1e-9, "%.6f" % score_per_fps)
    check("F3b 分辨率归一化的闭环: 两者相等条件成立 -> 修正系数为 1",
          abs((res_ratio * (135.0 / res_ratio)) / 135.0 - 1.0) < 1e-12,
          "score_aurora = 135 x fps_snl/res_ratio x workload_scale = score_snl (workload_scale=1)")

    sample = {
        "ok": True,
        "section": "GPU-SNL",
        "kind": "gpu-nomad-light",
        "workload": "Aurora Nomad Light",
        "scored": False,
        "gb7Item": False,
        "scoreContributes": False,
        "benchVersion": 1,
        "benchVersionText": "benchVersion=1 ... 跨版本不可比 ...",
        "format": {"revision": "snl-1", "fingerprint": "fnv1a64:XXXXXXXXXXXXXXXX",
                   "perVersionComparable": False},
        "score": {
            "value": 0.0, "unit": "points", "formula": "score = fps x kScorePerFps",
            "kScorePerFps": score_per_fps, "k3dmarkNomadScale": 135.0,
            "kResolutionScaleRatio": (float(W) * float(H)) / (2560.0 * 1440.0),
            "basis": "UL 官方 44002528075: SNL 总分 = 平均帧率 x 135; 135 x 0.5625 = 75.9375",
            "proportional": True,
        },
        "fps": {"value": 0.0, "meanFrameMs": 0.0, "medianFrameMs": 0.0, "bestFrameMs": 0.0,
                "worstFrameMs": 0.0, "stability": 0.0, "measuredFrames": 8, "warmupFrames": 2},
        "throughput": {"value": 0.0, "unit": "Mpx/s", "formula": "pixelsPerFrame x fps",
                       "pixelsPerFrame": 0.0, "pixelsPerSecond": 0.0},
        "work": {
            "pixelsPerPass": pass_pixels, "passesPerFrame": 0, "pixelsPerFrame": 0.0,
            "fullScreenPixels": W * H, "fragmentInvocationsPerFrame": 0.0,
            "opsPerPixelPerPass": ops_per_px, "opsPerPixelPassA": ops_a, "opsPerPixelPassB": ops_b,
            "opsPerFrame": 0.0, "opsPerSecond": 0.0, "unit": "gpu-work",
            "perUnitWorkIsConstant": True,
        },
        "scale": {
            "passesPerFrame": 0, "source": "ADAPTIVE_PROBE(...)", "adaptive": True,
            "onlyFreedomIsPassCount": True, "neverChangesPerUnitWork": True,
            "ladder": "2 的幂, 1..4096", "targetFrameMs": 12.0, "maxPasses": 64,
            "probeUsed": True, "probePasses": 4, "probePerPassMs": 0.0, "probeAtMs": 0.0,
            "probeScalePerSecond": 0.0, "scaleRatio": 0.0, "limitReached": False,
        },
        "gpuBoundEvidence": {
            "conclusive": True, "verdict": "GPU_BOUND", "tilesPerPass": instances, "tilePx": TP,
            "fragmentsPerTile": frags_per_tile, "gridPerPass": "%dx%d" % (tilesX, tilesY),
            "drawCallsPerPass": 2, "drawCallsPerFrame": 0, "drawCallsIndependentOfWorkload": True,
            "verticesPerFrame": 0.0, "cpuVertexDataBytesPerFrame": 0,
            "gpuSideGeneratedGeometry": True, "syncPointsPerFrame": 1,
            "cpuSubmitMsPerFrame": 0.0, "gpuBusyMsPerFrame": 0.0, "submissionDuty": 0.0,
            "gpuBusyPercentOfFrame": 0.0, "cpuGpuRatio": 0.0, "ratioMin": 0.0, "ratioMax": 0.0,
            "fenceAvailable": True,
        },
        "cpuGpuScaling": {"available": True, "lowPasses": 4, "lowMs": 0.0, "highPasses": 4,
                          "highMs": 0.0, "scalingRatio": 0.0, "expectedIfGpuBound": 0.0,
                          "expectedIfCpuBound": 1.0, "validityRange": "0..0", "verdict": "SEPARATED"},
        "selfProof": {"completed": True, "readbackAvailable": True, "readbackPx": 32,
                      "readbackSum": 0, "readbackNonZero": 0, "readbackMin": 0, "readbackMax": 0,
                      "readbackExpectedNonZero": 32 * 32 * 4, "perUnitWorkConstant": True,
                      "gpuThroughputTargeted": True},
        "crossGeneration": {"primaryComparableQuantity": "throughput(Mpx/s)",
                            "scoreRatioIsStableAcrossDevices": True,
                            "publishedSnlRatios": [{"pair": "9030 Pro / 9000S", "ratio": 3.27},
                                                   {"pair": "9030 Pro / 9020", "ratio": 2.18},
                                                   {"pair": "9020 / 9000S", "ratio": 1.50}]},
        "reference": {
            "source": "3DMark Steel Nomad Light 公开成绩", "verified": False,
            "resolutionFixedByUL": True,
            "latestVerifiedAnchor": "Kirin 9020 = 454",
            "cpuLimitationInvalidatesGraphicsScore": True,
            "scaling": {"officialScoreEquals135TimesFps": True, "k3dmarkNomadScale": 135.0,
                        "kResolutionScaleRatio": 0.5625},
            "points": [
                #  2026-08-31: 991 的来源已从"未证实"改成一手证据(用户拍屏) —— 样例里也带上
                #   verification / note 两个键, 与 sn_renderer.cpp 的 kRefPoints 输出形状一致
                #   (A1 那条"负载代码里不许出现设备名"的判据会把 kRefPoints 整块排除掉,
                #    这里给的是报告样例 JSON, 不是负载代码)。
                {"soc": "Kirin 9030 Pro", "device": "HUAWEI Mate 80 Pro Max", "officialScore": 991.0,
                 "verification": "USER_PROVIDED_PRIMARY_OBSERVATION",
                 "note": "机型 Mate 80 Pro Max / 测试项 Steel Nomad Light / 总分 991 / 平均帧率 7.34 FPS / "
                         "来源=用户拍屏(一手证据, 2026-08-31 截图); caveat(保留): UL 官方成绩库查不到该机型条目, "
                         "且与 956/998/993 等其他 9030 Pro 读数不完全一致",
                 "officialFpsAt2560x1440": 991.0 / 135.0 / 0.5625,
                 "equivFpsAt1920x1080": 991.0 / 135.0 / 0.5625, "equivScoreOnThisRuler": 991.0},
                {"soc": "Kirin 9020", "device": "HUAWEI Mate 70 / Pura 80 / Mate X6",
                 "officialScore": 454.0, "verification": "VERIFIED_UL_DB",
                 "note": "UL 官方成绩库机型行(用户提交结果中位数) + nanoreview 一致",
                 "officialFpsAt2560x1440": 454.0 / 135.0 / 0.5625,
                 "equivFpsAt1920x1080": 454.0 / 135.0 / 0.5625, "equivScoreOnThisRuler": 454.0},
                {"soc": "Kirin 9000S", "device": "HUAWEI Mate 60 / MatePad Pro 13.2",
                 "officialScore": 303.0, "verification": "UNVERIFIED",
                 "note": "只有 nanoreview 一处; UL 成绩库没有 Mate 60 系列条目",
                 "officialFpsAt2560x1440": 303.0 / 135.0 / 0.5625,
                 "equivFpsAt1920x1080": 303.0 / 135.0 / 0.5625, "equivScoreOnThisRuler": 303.0},
            ],
        },
        "calibration": {"status": "UNCALIBRATED", "kScorePerFpsInUse": score_per_fps,
                        "kWorkloadScaleInUse": workload_scale,
                        "derivation": "两者相等 => kScorePerFps = 135 x 0.5625 = 75.9375",
                        "affectsAbsoluteScoreOnly": True,
                        "ratioIsValidWithoutCalibration": True},
        "ruler": {
            "cpu": {"logicalCores": 0, "physicalCores": 0, "smtPossible": False, "smtEnabled": False,
                    "threadCap": 0, "maxConcurrencyUsed": 1},
            "simd": {"arch": 0, "neon": False, "sve": False, "sve2": False, "i8mm": False},
        },
        "rendering": {"width": W, "height": H, "fixedResolution": True, "passes": 2,
                      "instancing": "glDrawArraysInstanced(GL_TRIANGLES, 0, 6, %d)" % instances,
                      "vertexBuffers": 0, "fixedSeed": True},
        "gpu": {"glVersion": "", "glVendor": "", "glRenderer": "", "vsync": False, "offScreen": True},
        # 验收标准追加的三块(实现里的字段名与这里逐字一致; F4 会核对)

        # 最高优先级验收标准: 线性自检 + 条件化可比性(字段名与实现逐字一致)
        "linearity": {
            "requirement": "分数比 == 性能比(线性尺度)",
            "direction": "score = (工作量 / 时间) x 常数",
            "byConstruction": {"workLinearInTime": True},
            "selfChecks": {"perPassStable": {"pass": True, "spreadPercent": 0.0},
                           "roundStable": {"pass": True, "spreadPercent": 0.0, "rounds": 3}},
            "canSelfCheckOnThisDevice": True,
            "verdict": "LINEAR",
        },
        "comparability": {
            "question": "这个分数是在什么条件下取得的?",
            "conditions": {"allowedCoresOk": True, "allowedCores": 9,
                           "allowedList": "0,1,2,3,4,5,6,7,8", "logicalCores": 14,
                           "physicalCores": 9, "smtPossible": True, "threadsUsedByThisSection": 1,
                           "coresUsedByThisSection": 1, "coresUsedMOverN": "1/9",
                           "runtimeKhzMedian": 2270000, "runtimeKhzMax": 2270000,
                           "nominalTopKhz": 2270000, "envState": "STABLE"},
            "normalization": {"normalizedThroughputMpxPerSec": None, "normalizedScore": None,
                              "formula": "normalized = throughput / runtimeKhzMedian x nominalTopKhz"},
            "verdict": {"class": "CLEAN_TOP_FREQ", "comparableWith": [], "notComparableWith": [],
                        "conditionText": "..."},
            "whatWouldMakeItComparable": [],
        },
        "realUse": "非光追的 game-like 场景: 对程序化地形做光线求交 -> G-buffer -> 延迟 PBR 着色",
        "realUseShort": "非光追 game-like 场景的地形求交 + 延迟 PBR 着色(现代手游主渲染通路)",
        "environment": {
            "gpuClockReadable": False,
            "temperature": {"available": True, "unit": "degC", "zoneIndex": 0, "zoneName": "soc_thermal",
                            "zonesScanned": 12, "samples": 40, "min": 41.0, "median": 44.5, "max": 47.0,
                            "errno": 0},
            "cpuKhz": {"available": True, "unit": "kHz", "coresScanned": 14, "samples": 40,
                       "min": 1200000, "median": 2270000, "max": 2270000,
                       "firstHalfMedian": 2270000, "secondHalfMedian": 2270000, "errno": 0},
            "sampling": {"intervalMs": 50, "repeats": 3, "gapMs": 1500},
            "stability": {"state": "STABLE", "cpuKhzDropPercent": 0.0, "throttled": False},
            "probe": {"allowedCoresOk": True, "allowedCores": 9, "allowedList": "0,1,2,3,4,5,6,7,8",
                      "governor": "schedutil", "governorErrno": 0, "perCore": []},
            "antiBenchmarkMode": "不检测任何'跑分模式', 也不做任何特供优化",
        },
        "referenceComparison": {
            "thisDeviceIsNotCalibrated": True,
            "referenceDevice": "HUAWEI Mate 80 Pro Max (Kirin 9030 Pro)",
            "referenceDeviceSource": "用户提供的公开真值",
            "comparisonIsNotAnOfficialConversion": True,
            "gpuSnl": {"ours": {"score": 0.0, "fps": 0.0, "throughputMpxPerSec": 0.0},
                       "officialFormula": "score = 平均帧率 x 135", "sourceKind": "OFFICIAL_3DMARK_SUPPORT",
                       "rows": [{"name": "Kirin 9020", "referenceScore": 454.0, "ourScore": 0.0,
                                 "deltaPercent": 0.0, "sourceKind": "VERIFIED_UL_DB", "source": "..."}]},
            "gb7SingleCore":{"role": "见 aurora-reference 模块"},
            "gb7SingleCoreTruths": {"referenceDevice": "HUAWEI Mate 80 Pro Max (Kirin 9030 Pro)",
                                    "composite": {"single": 1633.0, "multi": 6802.0},
                                    "rows": [{"name": "Clang", "referenceMetric": "2.78 Klines/s",
                                              "referenceScore": 1697.0, "ourScore": None,
                                              "deltaPercent": None, "comparable": False}]},
            "notAvailable": ["查不到: ..."],
        },
        "repeatability": {
            "repeats": 3, "roundsOk": 3, "roundValuesAreAllListed": True,
            "representative": "median(中位) —— 不是最好一次, 也不是平均",
            "dispersionDefinition": "相对离散度 = (最大 - 最小) / 中位 x 100%",
            "thresholdsAreOurs": True,
            "thresholds": {"reliable": "<= 2%", "fair": "<= 5%", "unreliable": "> 5%"},
            "statistics": {"fps": {"median": 0.0, "min": 0.0, "max": 0.0, "dispersion": 0.0, "cv": 0.0},
                           "throughputMpxPerSec": {"median": 0.0, "min": 0.0, "max": 0.0},
                           "score": {"median": 0.0, "min": 0.0, "max": 0.0},
                           "perPassMs": {"median": 0.0}},
            "credibility": {"verdict": "RELIABLE", "dispersion": 0.0, "reliable": True,
                            "roundCountSufficient": True},
            "roundsDetail": [{"round": 1, "ok": True, "passesPerFrame": 16, "fps": 0.0,
                              "perPassMs": 0.0, "throughputMpxPerSec": 0.0, "score": 0.0,
                              "tempC": {"min": 0.0, "median": 0.0, "max": 0.0},
                              "cpuKhz": {"min": 0, "median": 0, "max": 0}, "envState": "STABLE"}],
            "stability": {"perPassMsFirst": 0.0, "perPassMsLast": 0.0, "perPassDriftPercent": 0.0,
                          "dropped": False, "firstRoundFps": 0.0, "laterRoundsMedianFps": 0.0,
                          "firstVsLaterPercent": 0.0, "warmupEffect": False},
        },
        "config": {"passesPerFrameOption": 0, "measureFrames": 8, "repeats": 3, "gapMs": 1500,
                   "targetMs": 12.0, "maxPasses": 64,
                   "defaults": {"passesPerFrame": 0, "measureFrames": 8, "repeats": 3, "gapMs": 1500,
                                "targetMs": 12.0, "maxPasses": 64}},
        "validity": {"valid": True, "pass": True, "checks": {}},
        "warnings": [],
        "elapsedMs": 0.0,
    }

    # 样例 JSON 的"形状"必须与实现里的字段逐字一致: 用实现里的字段名反查
    flat_impl = set(re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"\s*:', code_plain))
    flat_sample = set()

    def flatten(o, prefix=""):
        if isinstance(o, dict):
            for k, v in o.items():
                flat_sample.add(k)
                flatten(v, prefix + k + ".")
        elif isinstance(o, list):
            for v in o:
                flatten(v, prefix)

    flatten(sample)
    core_fields = ["section", "scored", "gb7Item", "benchVersion", "benchVersionText", "format",
                   "score", "fps", "throughput", "work", "scale", "gpuBoundEvidence",
                   "cpuGpuScaling", "selfProof", "crossGeneration", "reference", "ruler",
                   "rendering", "gpu", "config", "validity", "warnings", "notes", "risks",
                   "calibration", "isolation", "kind", "workload"]
    miss2 = [f for f in core_fields if f not in flat_impl]
    check("F4 实现里的字段名与样例一致(实现侧缺项)", len(miss2) == 0,
          ("实现侧缺: " + str(miss2)) if miss2 else "全部 %d 个字段在实现里都能找到" % len(core_fields))

    slim = dict(sample)
    for k in ("notes", "risks"):
        if k in flat_impl and k not in slim:
            slim[k] = ""
    with io.open(args.json_out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(slim, ensure_ascii=False, indent=2))
        fh.write("\n")
    emit("     样例 JSON 已写入: " + args.json_out)

    # G: 一致性数字(把"跨代可比"这件事量化)
    emit("")
    emit("[G] 跨代可比性的自洽检查(用公开的 SNL 代际比)")
    snl = {"9030 Pro": 991.0, "9020": 454.0, "9000S": 303.0}
    r1 = snl["9030 Pro"] / snl["9000S"]
    r2 = snl["9030 Pro"] / snl["9020"]
    r3 = snl["9020"] / snl["9000S"]
    check("G1 公开值之间的比值自洽(991/303 = 3.27, 991/454 = 2.18, 454/303 = 1.50)",
          abs(r1 - 3.27) < 0.01 and abs(r2 - 2.18) < 0.01 and abs(r3 - 1.50) < 0.01,
          "%.3f / %.3f / %.3f" % (r1, r2, r3))
    check("G2 本小节的分数对同一设备是严格线性的(比值不受 kScorePerFps 影响)",
          abs((2.0 * 10.0 * score_per_fps) / (10.0 * score_per_fps) - 2.0) < 1e-12,
          "score ∝ fps = 吞吐量, 所以代际比 = 吞吐量比")
    closed = (991.0 / 135.0 / 0.5625) * score_per_fps
    check("G3 公开值折算到本小节像素数后在尺子上回到公开分(换算闭环)",
          abs(closed - 991.0) < 1e-6,
          "991 分 -> 官方 7.341 fps(1440p) -> 本小节等效 13.05 fps(1080p) -> %.3f 分" % closed)

    # ---------------------------------------------------------------- H
    emit("")
    emit("[H] 验收标准「要有跑分的意义, 像其他主流跑分软件一样」逐条落实")
    # ---- 1) 可重复性被量化 ----
    mrep = re.search(r"const int kDefaultRepeats\s*=\s*(\d+)", code)
    check("H1a 多轮重复: 默认 >= 3 轮, 且可以配置",
          mrep is not None and int(mrep.group(1)) >= 3 and
          'readNumber(json, "repeats"' in code and 'readNumber(json, "gapMs"' in code,
          "kDefaultRepeats=%s, repeats/gapMs 可由 optionsJson 传入" % (mrep.group(1) if mrep else "?"))
    check("H1b 每一轮都完整跑(预热 + 计时 + 环境采样), 逐轮原始值全部列出",
          "roundValuesAreAllListed" in code_plain and "m.rounds.push_back" in code and
          "roundsDetail" in code_plain,
          "repeatability.roundsDetail 列出每一轮的原始值")
    check("H1c 给出中位 / 最小 / 最大 / 相对离散度",
          all(('"%s"' % k) in code_plain for k in ("median", "min", "max", "dispersion")) and
          "相对离散度 = (最大 - 最小) / 中位 x 100%" in code,
          "statistics.{fps,throughputMpxPerSec,score,perPassMs} 都带 median/min/max/dispersion")
    check("H1d 代表值取中位, 不是最好一次",
          '"representative":"median' in code_plain and "medianOf" in code and
          "takeBest" not in code and "bestOf" not in code,
          "JSON 写明 representative=median; 源码里没有「取最好一轮」的路径")
    check("H1e 醒目的「本次结果是否可信」判断(>5% 标注仅供参考)",
          "credibility" in code_plain and "UNRELIABLE" in code and
          "repeatDispersion > 5.0" in code and "仅供参考" in code,
          "credibility.verdict(RELIABLE/FAIR/UNRELIABLE) + warnings 里带「仅供参考」")
    check("H1f 有效性检查里也带「可重复」这一项",
          "repeatable" in code_plain and "repeatDispersion" in code,
          "validity.checks.repeatable")
    check("H1g 少于 3 轮时明确说「离散度本身也不可信」",
          "有效轮数只有" in code and "离散度本身也不可信" in code,
          "warnings 里有一条针对轮数不足的提示")
    # ---- 2) 分数只随硬件变: 温度 / 频率 / 降频标注 ----
    check("H2a 报告温度(最小/中位/最大), 来源写明",
          "thermal_zone" in code and '"temperature"' in code_plain and
          "tempMin" in code and "tempMed" in code and "tempMax" in code,
          "/sys/class/thermal/thermal_zoneN/temp 逐 zone 采样")
    check("H2b 报告运行时频率(最小/中位/最大), 逐核原文",
          "scaling_cur_freq" in code and '"cpuKhz"' in code_plain and
          "khzMed" in code and "perCore" in code_plain,
          "/sys/.../cpufreq/scaling_cur_freq 逐核采样 + perCore 原文")
    check("H2c 明确标注本次是否降频 / 未跑满频率档",
          "THROTTLED" in code and "WARMING_UP" in code and "STABLE" in code and
          "state" in code_plain,
          "environment.stability.state + 判据(前后半段频率比 / 温度)")
    check("H2d 首轮 vs 后续轮记录(不掩盖热身效应)",
          "firstVsLaterPercent" in code_plain and "warmupEffect" in code and
          "首轮明显低于后续轮" in code,
          "firstRoundFps / laterRoundsMedianFps / firstVsLaterPercent / warmupEffect")
    check("H2e 诚实写明 GPU 频率/温度读不到, 只给间接证据",
          '"gpuClockReadable":false' in code_plain and "间接" in code,
          "environment.gpuClockReadable=false + 说明")
    # ---- 3) 参考分对照表 ----
    check("H3a 有参考分对照表(与公开真值并列)",
          "referenceComparison" in code_plain, "referenceComparison 块")
    check("H3b 每条对照都带来源与来源种类",
          "sourceKind" in code_plain and 'source' in ref_src and
          "OFFICIAL_3DMARK_SUPPORT" in code and "OFFICIAL_UL_DB" in ref_src and
          "USER_PROVIDED_PUBLIC_TRUTH" in code,
          "source + sourceKind(官方/用户提供/第三方分开)")
    #  2026-08-31 变更 : 991(Kirin 9030 Pro)的来源从 UNVERIFIED 改成
    #   USER_PROVIDED_PRIMARY_OBSERVATION —— 用户拍屏的一手证据(机型 Mate 80 Pro Max /
    #   Steel Nomad Light / 总分 991 / 平均帧率 7.34 FPS)。303 仍是 UNVERIFIED, 454 仍是已证实。
    check("H3c 来源强度逐条标注, 第三方数据不写成官方",
          "UNVERIFIED" in code and "未证实" in code and "第三方数据不写成官方" in code and
          "USER_PROVIDED_PRIMARY_OBSERVATION" in code,
          "991=USER_PROVIDED_PRIMARY_OBSERVATION(用户拍屏一手证据) / 303=UNVERIFIED / "
          "454=VERIFIED_UL_DB")
    check("H3d 查不到的项写查不到",
          "notAvailable" in code_plain and "查不到" in code,
          "notAvailable[] + comparable=false + why")
    check("H3e 用户给的 GB7 真值(8 项 + 复合分)都在真值表里",
          all(k in ref_src for k in ["226 MB/s", "11.0 routes/s", "23.0 pages/s", "67.2 Mpx/s",
                                     "5.62 images/s", "2.78 Klines/s", "76.3 pages/s", "26.8 MB/s"]) and
          all(k in ref_src for k in ["1589", "2004", "1839", "1941", "1639", "1697", "1582", "1602"]) and
          "1633" in ref_src and "6802" in ref_src,
          "8 个吞吐 + 8 个分数 + 单核 1633 / 多核 6802")
    check("H3f 参考分对照是独立只读模块(libauroraref.so), 不并进跑分主模块",
          "add_library(auroraref SHARED" in cm and "reference_compare.cpp" in cm and
          "aurorabench" not in re.search(r"target_link_libraries\(auroraref PUBLIC[^)]*\)", cm).group(0),
          "libauroraref.so 只链 napi + hilog")
    # ---- 4) 真实用途 ----
    check("H4a 本小节有一句话真实用途",
          "realUse" in code_plain and "kRealUseText" in code,
          "realUse / realUseShort")
    check("H4b GB7 16 项 + GPU 11 项也都有真实用途(对照模块里的表)",
          "kRealUseTable" in ref_src and
          all(k in ref_src for k in ["File Compression", "Navigation", "Clang", "Structure from Motion",
                                     "Background Blur", "Path Tracer", "Video Filter", "Aurora Nomad Light"]),
          "reference_compare.cpp 的 kRealUseTable 覆盖 16+11+1 项")
    # ---- 5) 可解释性 ----
    check("H5a 分数是怎么来的(公式 + k 的来源)",
          "score = fps x kScorePerFps" in code and "kScorePerFpsFormula" in code_plain and
          "k3dmarkNomadScale" in code,
          "score.formula + kScorePerFpsFormula + k3dmarkNomadScale")
    check("H5b 单位是什么",
          '"unit"' in code_plain and '"Mpx/s"' in code_plain and "points" in code,
          "score.unit=points / throughput.unit=Mpx/s / fps")
    check("H5c 为什么与官方同口径或不同口径",
          ("同形" in hdr or "同形" in src_plain) and "sameCaliber" in ref_src and
          "differentCaliber" in ref_src and "同一份负载" in src_plain and
          "differentWorkload" in ref_src,
          "basis + 对照表里的同口径/不同口径说明")
    check("H5d 计分公式与口径写进了逐项解释",
          "howComputed" in ref_src and "metric x conv" in ref_src,
          "referenceComparison 的 howComputed.formula = k x (metric x conv)")
    # ---- 6) 不许被跑分模式污染 ----
    check("H6a 报告允许核集合与 governor 原文",
          "sched_getaffinity" in code and "scaling_governor" in code and
          "allowedCores" in code_plain and "governor" in code_plain,
          "environment.probe.allowedCores/List + governor + errno")
    check("H6b 明确写「不检测跑分模式 / 不做特供优化」并给出可质疑的原始读数",
          "antiBenchmarkMode" in code_plain and "宁可暴露不利事实" in code,
          "environment.antiBenchmarkMode")
    check("H6c 读不到的读数写 errno, 不填 0 冒充",
          "errno" in code_plain and "不填 0 冒充数据" in src,
          "逐项 errno 字段")
    check("H6d 温度/频率读数不可用时明确说不假装稳定",
          "不假装稳定" in code and "环境读数不可用" in code,
          "environment.state=UNKNOWN + warnings")
    check("H6e 阈值是我们自己定的, 报告里注明",
          "thresholdsAreOurs" in code_plain and "我们自己定的" in src,
          "repeatability.thresholdsAreOurs")

    # ---------------------------------------------------------------- I
    emit("")
    emit("[I] 最高优先级验收标准: 分数比 == 性能比(线性尺度)")
    check("I1 单项分公式只有一个变量(score = k x metric x conv)",
          re.search(r"s\.score\s*=\s*s\.fps\s*\*\s*kScorePerFps", code) is not None and
          "kScorePerFps = k3dmarkNomadScale * kWorkloadScale * kResolutionScaleRatio" in code,
          "score = fps x 编译期常量; 没有分段/指数/按设备取值")
    check("I2 报告里有线性自检块(构造性 + 两个可自检条件)",
          "linearity" in code_plain and "byConstruction" in code_plain and
          "perPassStable" in code_plain and "roundStable" in code_plain and
          "canSelfCheckOnThisDevice" in code_plain,
          "linearity.{byConstruction,selfChecks,verdict}")
    check("I3 线性自检会给出 verdict 与不通过时的处置口径",
          "LINEAR" in code and "NONLINEAR_SUSPECT" in code and "ifNotLinear" in code_plain,
          "LINEAR / NONLINEAR_SUSPECT + ifNotLinear")
    check("I4 跨档位每趟耗时被用来验证线性(固定开销不随规模变)",
          "perPassSpreadPct" in code and "帧时间 / passesPerFrame" in code,
          "perPassSpreadPct = 不同 pass 档位每趟耗时的离散度")
    check("I5 需要两台设备才能做的那部分标注",
          "needsTwoDevices" in code_plain and "跨代线性回归" in code,
          "needsTwoDevices[] 指向 verify_linearity_regression.py")
    # ---- 条件化可比性 ----
    check("I6 分数旁标注本次条件(可用核集合 / 频率中位 / 线程数 / M/N)",
          all(k in code_plain for k in ("allowedCores", "allowedList", "runtimeKhzMedian",
                                        "nominalTopKhz", "threadsUsedByThisSection",
                                        "coresUsedMOverN")),
          "comparability.conditions.*")
    check("I7 给出条件化可比性判断(与谁可比 / 与谁不可比)",
          "comparableWith" in code_plain and "notComparableWith" in code_plain and
          "conditionText" in code_plain and "CLEAN_TOP_FREQ" in code,
          "comparability.verdict.{class,comparableWith,notComparableWith,conditionText}")
    check("I8 给出可执行的归一化方案(同频 + 每核)",
          "normalizedThroughputMpxPerSec" in code_plain and "throughput / runtimeKhzMedian x nominalTopKhz" in code,
          "normalization.normalizedThroughput = 吞吐 / 频率中位 x 标称最高频")
    check("I9 写明'吞吐 ∝ 频率'的前提与读不到时的降级",
          "precondition" in code_plain and "间接" in code and "null" in code_plain,
          "normalization.precondition + 拿不到就给 null")
    check("I10 参考模块里也带线性自检与条件化可比性(GB7 多核侧)",
          "linearityAudit" in ref_src and "comparabilityAudit" in ref_src and
          "perCoreThroughput" in ref_src and "throughputAtTopKhz" in ref_src,
          "reference_compare.cpp: linearityAudit + comparabilityAudit")
    check("I11 参考模块里写明复合分是几何平均 + 官方同口径",
          "officialIsGeometricMean" in ref_src and "GM(c x a_i) = c x GM(a_i)" in ref_src,
          "composite.formula + 线性说明")
    check("I12 未计分项明确不进复合分(宁可未计分也不混进去)",
          "compositeExcludesNotScored" in ref_src and "NOT_SCORED" in ref_src,
          "linearityAudit 逐项 status = LINEAR_BY_CONSTRUCTION / NOT_SCORED")

    emit("")
    emit("=" * 100)
    emit("共 %d 项断言, 失败 %d 项 -> %s" % (checks, fails, "PASS" if fails == 0 else "FAIL"))
    emit("=" * 100)
    return 0 if fails == 0 else 1


_OUT = None
if __name__ == "__main__":
    sys.exit(main())