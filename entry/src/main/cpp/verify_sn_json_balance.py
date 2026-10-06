# -*- coding: utf-8 -*-
r"""
GPU-SNL 结果 JSON 的「括号不平衡」回归 + 自检误报判别器(全离线)

背景(真机现场, 2026-10-05)
--------------------------
  手机 Pura X Max / AuroraBench 8.1, GPU-SNL 小节报:
      native 返回 ok=false：GPU-SNL 执行到 [json-selfcheck] 失败:
      JSON 自检失败: 括号不平衡(depth=2) (结果长度 30801 字节)

  本脚本用真机原样字节证明根因, 并把修复前后的解析结果逐条钉死。

根因(一句话)
------------
  sn_renderer.cpp 的 snlBuildJson() 里有两个 JSON 对象从来没有被闭合:
      "ruler"  (在 headroomNotes 之后漏 '}')
      "gpu"    (在 note 之后漏 '}')
  后果: simd / headroomNotes / rendering / gpu / config / validity / warnings / elapsedMs
  全被塞进 ruler 里; config / validity / warnings / elapsedMs 又被塞进 gpu 里;
  末尾那个 '}' 只关掉 gpu, 根对象永远不闭合。
  括号栈上正好剩下 2 个未匹配的 '{'(根 + 一个内层) => depth = 2。
  这与真机报告里的 "括号不平衡(depth=2)" 逐字吻合。

  这两个漏括号是静态模板缺陷, 8.0 的源码里就有(不是上一轮改出来的);
  7.5/8.0 之所以没暴露, 是因为那时候 JSON 在更早的地方(environment.stability)
  就已经被野指针写坏、解析在 char 8552 处就失败了 —— 后面的括号问题没人看到。
  上一轮修好野指针之后, 它才成为"挡在前面的那一个"。

本脚本的八段(全部离线, 不连真机)
--------------------------------
  [A] 夹具自证: 真机字节数 / SHA256 / 三个坏点偏移逐个字节核对
  [B] 修前: json.loads() 失败 + 归一化到 C++ snBraceScan 的同一套判据
  [C] 只修上一轮那一处(野指针) -> 剩下 depth=2, 与真机 8.1 报告逐字吻合 关键判据
  [D] 修满(本次补上两个 '}') -> json.loads() 成功, 顶层 39 键, 结构回到设计形状
  [E] 自检误报反例: 字符串里含 { } [ ] 引号 反斜杠 的正常 JSON 不许被误判成"JSON 坏了"
  [F] 源码级断言: 模板括号平衡 + 两个闭合点位置 + 动态插值必须过 jsonSafe(防回归)
  [G] 自检实现本身的硬要求: 计数跳字符串/转义 + "真坏了"与"误报"两种结论分开
  [H] 诚实边界: 8.1 那份 30801 字节的结果在任何日志里都取不到(打印, 不假装)

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import hashlib
import importlib.util
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
SN_DIR = os.path.join(HERE, "sn")
SN_CPP = os.path.join(SN_DIR, "sn_renderer.cpp")
SAMPLE = os.path.join(HERE, "sn_json_sample.json")
FIXTURE = os.path.join(HERE, "sn_json_fixture_device_r80.py")

fails = 0
checks = 0
_OUT = io.open(os.path.join(HERE, "verify_sn_json_balance_out.txt"), "w", encoding="utf-8", newline="\n")


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


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# ===========================================================================
#  与 C++ 同语义的两个扫描器
# ---------------------------------------------------------------------------
#  snBraceScan()  —— sn_renderer.cpp 里那个(只作诊断用的)括号计数扫描。
#                    规矩: 必须跳过字符串字面量与转义。下面与 C++ 逐行对应。
#  naive_brace_scan() —— "不跳字符串"的旧式计数(自检误报的经典成因), 只用于反例。
# ===========================================================================

def snBraceScan(doc):
    """返回 (depth, in_str, first_bad, kind)。与 C++ SnBraceScan 同语义。"""
    depth = 0
    in_str = False
    esc = False
    for i, ch in enumerate(doc):
        o = ord(ch)
        if o == 0:
            return depth, in_str, i, "nul"
        if in_str:
            if esc:
                esc = False
                continue
            if ch == "\\":
                esc = True
                continue
            if ch == '"':
                in_str = False
                continue
            if o < 0x20:
                return depth, in_str, i, "ctrl-in-string"
            continue
        if ch == '"':
            in_str = True
        elif ch in "{[":
            depth += 1
        elif ch in "}]":
            depth -= 1
            if depth < 0:
                return depth, in_str, i, "closed-early"
    return depth, in_str, None, None


def naive_brace_scan(doc):
    """故意不跳字符串的计数(旧式/想当然的实现) —— 用来构造"自检误报"的反例。"""
    return doc.count("{") + doc.count("[") - doc.count("}") - doc.count("]")


def unmatched_openers(doc):
    """返回所有没有配对 '}' / ']' 的开启符号 [(字符, 偏移)]。"""
    stack = []
    in_str = False
    esc = False
    for i, ch in enumerate(doc):
        if in_str:
            if esc:
                esc = False
                continue
            if ch == "\\":
                esc = True
                continue
            if ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
        elif ch in "{[":
            stack.append((ch, i))
        elif ch in "}]":
            if stack:
                stack.pop()
    return stack


def key_before(doc, pos):
    k = doc.rfind('"', 0, pos)
    if k < 0:
        return "?"
    k2 = doc.rfind('"', 0, k)
    return doc[k2 + 1:k] if k2 >= 0 else "?"


def parse_or_reason(doc):
    """用严格解析器(Python json, 与 ArkTS JSON.parse 同一套文法)判一次。"""
    try:
        json.loads(doc)
        return True, ""
    except Exception as e:
        return False, "%s: %s" % (type(e).__name__, e)


# ===========================================================================
#  把真机 payload 修成"当前 C++ 会交出去的那一份"
# ---------------------------------------------------------------------------
#  两件事, 与 sn_renderer.cpp 现在的实现一一对应:
#   ① 野指针读出来的三个字段被规范化 + 转义(jsonSafe / snCanonState 的语义):
#        environment.stability.state  -> "UNKNOWN"
#        environment.stability.text   -> 一段正常文本(jsonSafe 把非法 UTF-8 换成 '?')
#        repeatability.roundsDetail[].envState -> "UNKNOWN"
#      顺带把被吃掉的结尾引号与花括号补回来。
#   ② 模板里补上 ruler / gpu 两个漏掉的 '}'(本次修复)。
# ===========================================================================

def fix_env_fields(doc):
    """只做"上一轮那一处修复"能带来的变化(野指针 -> 规范化 + 转义)。"""
    i = doc.find('"cpuKhzDropPercent"')
    si = doc.rfind('"state":"', 0, i)          # 值在 si+9
    out = doc[:si] + '"state":"UNKNOWN"' + doc[i - 1:]

    ti = out.find('"text":"', out.find('"cpuKhzDropPercent"'))
    pi = out.find(',"probe"', ti)
    out = out[:ti] + '"text":"FIXED"' + '}' + out[pi:]

    out = out.replace('"envState":"\u0420\u00b1s\x7f"', '"envState":"UNKNOWN"')
    out = out.replace('\u0420\u00b1s\x7f', 'UNKNOWN')
    return out


def fix_template_braces(doc):
    """本次修复: 补上 ruler 与 gpu 两个漏掉的 '}'。"""
    j = doc.find('"headroomNotes"')
    k = doc.find(']', j)
    out = doc[:k + 1] + '}' + doc[k + 1:]

    n = out.find('"note":"GPU 型号字符串来自驱动')
    e = out.find('"', n + len('"note":"') + len('GPU 型号字符串来自驱动, 只作记录, 不计分或负载选择'))
    out = out[:e + 1] + '}' + out[e + 1:]
    return out


# ===========================================================================
#  C++ 源码里的字符串字面量抽取(给 [F] 用)
# ===========================================================================

def strip_cpp_comments(src):
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\":
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


CPP_ESC = {
    "n": "\n", "t": "\t", "r": "\r", "0": "\0", '"': '"', "'": "'", "\\": "\\",
    "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?", "/": "/",
}


def unescape_cpp(lit):
    out = []
    i = 0
    while i < len(lit):
        c = lit[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        nxt = lit[i + 1] if i + 1 < len(lit) else ""
        if nxt == "x":
            j = i + 2
            h = ""
            while j < len(lit) and lit[j] in "0123456789abcdefABCDEF":
                h += lit[j]
                j += 1
            out.append(chr(int(h, 16)) if h else "?")
            i = j
            continue
        if nxt == "u":
            out.append(chr(int(lit[i + 2:i + 6], 16)))
            i += 6
            continue
        out.append(CPP_ESC.get(nxt, nxt))
        i += 2
    return "".join(out)


def func_body(src, signature_re):
    """按 '顶层函数(行首无缩进的签名) + 行首 '}' 收尾' 切出一段函数体。"""
    m = re.search(signature_re, src)
    if not m:
        return None
    start = m.start()
    end = src.find("\n}", start)
    if end < 0:
        return None
    return src[start:end + 2]


def split_statements(text):
    """按深度 0 的分号把 C++ 代码切成一条条语句(字符串/字符字面量里的分号不算)。"""
    stmts = []
    cur = []
    i = 0
    n = len(text)
    par = 0
    brace = 0        # 语块深度({} 的净个数), 用来区分"主干语句"与"分支里的语句"
    brace_at_start = 0   #  必须记语句开始那一刻的深度, 不是分号那一刻的深度 
                         #   (记成分号那一刻会得到"语句结束时的深度", 分支语句就被算错一层)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            cur.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    cur.append(text[i:i + 2])
                    i += 2
                    continue
                cur.append(text[i])
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            brace += 1
        elif c == "}":
            brace -= 1
        # 只把 () 与 [] 当作"表达式深度"; {} 是语句块, 不影响一条语句在哪里结束。
        # (第一版把 {} 也算进去, 于是整个函数体从头到尾 par >= 1, 一条语句都切不出来 ——
        #  模板全为空串, F1 就变成了假通过。这里记一笔, 免得以后再踩。)
        if c in "([":
            par += 1
        elif c in ")]":
            par -= 1
        cur.append(c)
        i += 1
        if c == ";" and par <= 0:
            stmts.append(("".join(cur), brace_at_start))
            cur = []
            par = 0
            brace_at_start = brace
    rest = "".join(cur)
    if rest.strip():
        stmts.append((rest, brace_at_start))
    return stmts


def depth0_literals(stmt):
    """取一条语句里括号深度为 0 的字符串字面量(已去转义)。

    为什么必须限制深度: std::string(x ? "A" : "B") / j += (i == 0 ? "" : ",") /
    snprintf(b, n, "%s{...}", ...) / warn.push_back("...") 里的字面量是值/参数,
    不是拼进 j 的 JSON 结构文本。把它们一起接进来会凭空多出或吃掉括号, 检查就会假报警
    (第一版脚本正是在 comparabilityJson 上踩了这个坑: 把 c.state == "UNKNOWN" 的比较
    操作数当成了 JSON 文本)。
    """
    out = []
    i = 0
    n = len(stmt)
    par = 0
    while i < n:
        c = stmt[i]
        if c == '"' or c == "'":
            q = c
            j = i + 1
            buf = []
            while j < n:
                if stmt[j] == "\\":
                    buf.append(stmt[j:j + 2])
                    j += 2
                    continue
                if stmt[j] == q:
                    break
                buf.append(stmt[j])
                j += 1
            if q == '"' and par == 0:
                out.append(unescape_cpp("".join(buf)))
            i = j + 1
            continue
        if c in "([{":
            par += 1
        elif c in ")]}":
            par -= 1
        i += 1
    return out


def find_j_assign(stmt):
    """找出一条语句里最后一次出现的 j += / j =(在字符串与字符字面量之外, 括号深度 0)。

    为什么不能只看语句开头: split_statements 是按分号切的, 于是
        "} j += \"]}\";"        (上一段的收尾与这一句挤在同一条语句里)
        "if (envClean) { j += \"...\";"
    这两种形态的语句都不是以 "j +=" 开头。第一版因此把 envJson 的 "}]" 与
    comparabilityJson 里 if 分支下的整段都漏掉了 —— 模板少了闭合括号, 检查反而假报警。
    """
    i = 0
    n = len(stmt)
    par = 0
    last = -1
    while i < n:
        c = stmt[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if stmt[i] == "\\":
                    i += 2
                    continue
                if stmt[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c in "([":
            par += 1
        elif c in ")]":
            par -= 1
        elif c == "j" and par == 0 and (i == 0 or not (stmt[i - 1].isalnum() or stmt[i - 1] == "_")):
            k = i + 1
            while k < n and stmt[k] == " ":
                k += 1
            if stmt.startswith("+=", k):
                last = i
            elif stmt.startswith("=", k) and not stmt.startswith("==", k):
                last = i
        i += 1
    return last


def brace_delta(text):
    """一段代码里 {} 的净增量(跳字符串)。"""
    d = 0
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            d += 1
        elif c == "}":
            d -= 1
        i += 1
    return d


def body_statements_all(src):
    """整个文件按语句扫一遍, 只保留带 j += / j = 的语句(给 F6 的插值审查用)。"""
    out = []
    for stmt, brace0 in split_statements(strip_cpp_comments(src)):
        at = find_j_assign(stmt)
        if at < 0:
            continue
        out.append((stmt, brace0, at))
    return out


def body_statements(src, signature_re):
    """返回 [(语句文本, 该语句里 j += 的语块深度, j += 的偏移)]; 没有 j += 的语句被丢掉。"""
    body = func_body(src, signature_re)
    if body is None:
        return None
    body = strip_cpp_comments(body)
    out = []
    for stmt, brace0 in split_statements(body):
        at = find_j_assign(stmt)
        if at < 0:
            continue
        out.append((stmt, brace0 - 1 + brace_delta(stmt[:at]), at))
    return out


def template_of(src, signature_re, skip_else=False):
    """把某个 JSON 组装函数里真正拼进 j 的那些字面量按源码顺序接起来 = 该函数的 JSON 模板。

    变量插值(std::to_string / fixed / jsonSafe(...))一律不进来 —— 它们产出的是数字或
    已转义的字符串值, 不可能改变模板的括号结构。所以"模板括号必须平衡"是一条硬不变量:
    它一旦被破坏, 交出去的 JSON 一定不合法, 与运行时数据无关。

    skip_else=True 时丢掉所有 else 分支(只留 if 的第一个分支)。
    为什么需要它: comparabilityJson 里有一串
        j += ","comparableWith":[";
        if (A) { j += "...]"; } else if (B) { j += "...]"; } else { j += "...]"; }
    数组是在主干上开的、在分支里关的; 互斥分支全接起来当然会多出括号。
    取"if 的第一个分支"作为代表路径, 就得到一条真实可执行的路径 —— 它必须平衡。
    旁证: 每个分支语句本身必须只收口不开口(delta<=0, 无未闭合字符串), 见 F1b。
    """
    stmts = body_statements(src, signature_re)
    if stmts is None:
        return None
    parts = []
    for stmt, depth, at in stmts:
        if skip_else:
            head = stmt.strip()
            if head.startswith("} else") or head.startswith("else"):
                continue
        parts.extend(depth0_literals(stmt[at:]))
    return "".join(parts)


def balanced_paren_content(text, open_idx):
    """open_idx 指向 '('; 返回配对的 ')' 之间的内容(跳过字符串)。"""
    i = open_idx + 1
    depth = 1
    n = len(text)
    start = i
    while i < n and depth > 0:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i]
        i += 1
    return text[start:]


def split_top_level_commas(text):
    """按深度 0 的逗号切开实参表(跳过括号与字符串)。"""
    out = []
    par = 0
    i = 0
    n = len(text)
    cur = []
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            cur.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    cur.append(text[i:i + 2])
                    i += 2
                    continue
                cur.append(text[i])
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c in "([{":
            par += 1
        elif c in ")]}":
            par -= 1
        if c == "," and par == 0:
            out.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    out.append("".join(cur))
    return out


def concat_pure_literals(text):
    """如果 text 只由相邻的字符串字面量(中间可以有空白)组成, 返回它们拼起来的内容; 否则 None。"""
    rest = text
    buf = []
    while True:
        rest = rest.lstrip()
        if not rest:
            break
        if not rest.startswith('"'):
            return None
        j = 1
        out = []
        while j < len(rest):
            if rest[j] == "\\":
                out.append(rest[j:j + 2])
                j += 2
                continue
            if rest[j] == '"':
                break
            out.append(rest[j])
            j += 1
        buf.append(unescape_cpp("".join(out)))
        rest = rest[j + 1:]
    return "".join(buf) if buf else None


STR_LIT = r'"(?:[^"\\]|\\.)*"'


def _top_level_split(a, ch):
    """在括号/字符串之外找第一个 ch, 返回下标; 找不到返回 -1。"""
    par = 0
    i = 0
    n = len(a)
    while i < n:
        c = a[i]
        if c == '"':
            i += 1
            while i < n:
                if a[i] == "\\":
                    i += 2
                    continue
                if a[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c in "([":
            par += 1
        elif c in ")]":
            par -= 1
        elif c == ch and par == 0:
            return i
        i += 1
    return -1


def branches_are_all_literals(a):
    """判断一个表达式是不是"只可能产出字符串字面量"的条件表达式。

    std::string(x ? "true" : "false")            -> True(只可能出 true/false)
    std::string("null")                          -> True
    std::string(conclusive ? "A" : "B")          -> True
    std::string(c.state)                         -> False  <-- 这就是要拦住的形态
    """
    a = a.strip()
    if concat_pure_literals(a) is not None:
        return True
    while a.startswith("(") and a.endswith(")"):
        inner = a[1:-1]
        if _top_level_split(inner, ")") >= 0:
            break
        a = inner.strip()
        if concat_pure_literals(a) is not None:
            return True
    q = _top_level_split(a, "?")
    if q < 0:
        return False
    # 找与这个 '?' 配对的 ':'
    par = 0
    i = q + 1
    depth_q = 1
    colon = -1
    n = len(a)
    while i < n:
        c = a[i]
        if c == '"':
            i += 1
            while i < n:
                if a[i] == "\\":
                    i += 2
                    continue
                if a[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c in "([":
            par += 1
        elif c in ")]":
            par -= 1
        elif par == 0 and c == "?":
            depth_q += 1
        elif par == 0 and c == ":":
            depth_q -= 1
            if depth_q == 0:
                colon = i
                break
        i += 1
    if colon < 0:
        return False
    return (branches_are_all_literals(a[q + 1:colon]) and
            branches_are_all_literals(a[colon + 1:]))


def std_string_args(text):
    """找出所有 std::string( <平衡括号参数> ), 返回 [(参数文本, 起始下标)]。"""
    out = []
    for m in re.finditer(r"std::string\s*\(", text):
        i = m.end()
        depth = 1
        j = i
        n = len(text)
        while j < n and depth > 0:
            c = text[j]
            if c == '"':
                j += 1
                while j < n:
                    if text[j] == "\\":
                        j += 2
                        continue
                    if text[j] == '"':
                        j += 1
                        break
                    j += 1
                continue
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            j += 1
        out.append((text[i:j - 1], m.start()))
    return out


# ===========================================================================
def main():
    emit("=" * 78)
    emit("GPU-SNL 结果 JSON: 括号不平衡根因 / 修复 / 自检误报判别(离线)")
    emit("=" * 78)
    emit()

    src = read(SN_CPP)

    # ----------------------------------------------------------------- [A]
    emit("[A] 夹具自证(真机原样字节)")
    check("A1 夹具文件存在", os.path.exists(FIXTURE), FIXTURE)
    fx = load_module(FIXTURE, "sn_fixture")
    raw = fx.payload()
    check("A2 夹具字节数 == %d" % fx.PAYLOAD_BYTES, len(raw) == fx.PAYLOAD_BYTES,
          "实测 %d" % len(raw))
    check("A3 SHA256 与登记值一致",
          hashlib.sha256(raw).hexdigest() == fx.PAYLOAD_SHA256,
          hashlib.sha256(raw).hexdigest()[:24] + "...")
    check("A4 environment.stability.state 的野字节在 %d 处" % fx.OFFSET_STABILITY_STATE,
          raw[fx.OFFSET_STABILITY_STATE:fx.OFFSET_STABILITY_STATE + 6] == fx.STABILITY_STATE_BYTES,
          repr(raw[fx.OFFSET_STABILITY_STATE:fx.OFFSET_STABILITY_STATE + 6]))
    check("A5 4 个 CESU-8 孤立低位代理在 %d 处" % fx.OFFSET_SURROGATES,
          raw[fx.OFFSET_SURROGATES:fx.OFFSET_SURROGATES + 12] == fx.SURROGATE_BYTES,
          "U+DDAC/U+DC69/U+DECE/U+DCBD")
    check("A6 roundsDetail[].envState 的野字节在 %d 处" % fx.OFFSET_ENVSTATE,
          raw[fx.OFFSET_ENVSTATE:fx.OFFSET_ENVSTATE + 6] == fx.ENVSTATE_BYTES,
          repr(raw[fx.OFFSET_ENVSTATE:fx.OFFSET_ENVSTATE + 6]))
    emit("      来源: %s 第 %d 行(设备 %s)" % (fx.SOURCE_REPORT, fx.SOURCE_LINE, fx.SOURCE_DEVICE))
    doc0 = raw.decode("utf-8", "replace")
    emit()

    # ----------------------------------------------------------------- [B]
    emit("[B] 修前: 真机这一段 JSON 到底能不能解析")
    ok0, why0 = parse_or_reason(doc0)
    check("B1 修前 json.loads() 失败(证明 JSON 真的坏了)", not ok0, why0)
    depth0, instr0, bad0, kind0 = snBraceScan(doc0)
    check("B2 括号计数扫描: 字符串未闭合(野字节把字符串吃掉了)",
          instr0, "depth=%d inStr=%s" % (depth0, instr0))
    un0 = unmatched_openers(doc0)
    emit("      未配对的开启符号 %d 个: %s" % (
        len(un0), ", ".join("%s@%d(%s)" % (c, p, key_before(doc0, p)) for c, p in un0)))
    check("B3 未配对开启符号 == 3(根 / environment / stability)",
          len(un0) == 3, "%d 个" % len(un0))
    emit()

    # ----------------------------------------------------------------- [C]
    emit("[C] 只修上一轮那一处(environment 野指针) -> 剩下的就是这个 bug")
    doc1 = fix_env_fields(doc0)
    depth1, instr1, _, _ = snBraceScan(doc1)
    un1 = unmatched_openers(doc1)
    emit("      未配对的开启符号 %d 个: %s" % (
        len(un1), ", ".join("%s@%d(%s)" % (c, p, key_before(doc1, p)) for c, p in un1)))
    check("C1 字符串已闭合(野指针那处确实修好了)", not instr1, "inStr=%s" % instr1)
    check("C2 括号计数 depth == 2  <-- 与真机 8.1 报告『括号不平衡(depth=2)』逐字吻合",
          depth1 == 2, "depth=%d" % depth1)
    check("C3 两个未配对开启符号就是 ruler 与根对象",
          len(un1) == 2 and un1[0][1] == 0 and key_before(doc1, un1[1][1]) == "ruler",
          ",".join("%s@%d(%s)" % (c, p, key_before(doc1, p)) for c, p in un1))
    ok1, why1 = parse_or_reason(doc1)
    check("C4 这一份仍然解析失败(所以 8.1 只是把失败换了个说法, 并没有变好)", not ok1, why1)
    emit()

    # ----------------------------------------------------------------- [D]
    emit("[D] 修满(本次补上 ruler / gpu 两个 '}') -> 严格解析必须通过")
    doc2 = fix_template_braces(doc1)
    un2 = unmatched_openers(doc2)
    check("D1 未配对开启符号 == 0", len(un2) == 0,
          ",".join("%s@%d" % (c, p) for c, p in un2))
    depth2, instr2, _, _ = snBraceScan(doc2)
    check("D2 括号计数 depth == 0 且字符串全闭合", depth2 == 0 and not instr2,
          "depth=%d inStr=%s" % (depth2, instr2))
    ok2, why2 = parse_or_reason(doc2)
    check("D3 json.loads() 成功", ok2, why2 or "已解析")

    top = None
    if ok2:
        top = json.loads(doc2)
        check("D4 顶层键与夹具登记的一致(%d 个)" % len(fx.EXPECTED_TOP_KEYS),
              sorted(top.keys()) == sorted(fx.EXPECTED_TOP_KEYS),
              "实测 %d 个" % len(top))
        check("D5 ruler 里是 cpu / simd / headroomNotes(ArkTS 读的就是 ruler.cpu / ruler.simd)",
              set(["cpu", "simd", "headroomNotes"]).issubset(set(top["ruler"].keys())),
              ",".join(sorted(top["ruler"].keys())))
        check("D6 gpu 是顶层键, config / validity / warnings / elapsedMs 回到顶层(不再被塞进 gpu)",
              all(k in top for k in ("gpu", "config", "validity", "warnings", "elapsedMs")) and
              "config" not in top["gpu"] and "validity" not in top["gpu"],
              "gpu 子键: " + ",".join(sorted(top["gpu"].keys())))
        check("D7 ok/scored/gb7Item 口径没被动过",
              top.get("ok") is True and top.get("scored") is False and top.get("gb7Item") is False,
              "ok=%s scored=%s gb7Item=%s" % (top.get("ok"), top.get("scored"), top.get("gb7Item")))
    else:
        for n in ("D4 顶层键集合", "D5 ruler 结构", "D6 gpu 结构", "D7 计分标记"):
            check(n, False, "上一步没解析成功")

    emit("      顶层键(%d): %s" % (len(top or {}), ", ".join(sorted((top or {}).keys()))))
    emit()

    if os.path.exists(SAMPLE):
        try:
            sample = json.loads(read(SAMPLE))
            check("D8 sn_json_sample.json 可解析(镜面样例没被带坏)", True, "%d 键" % len(sample))
            missing = sorted(set(sample.keys()) - set((top or {}).keys()))
            check("D9 镜面样例的顶层键全部出现在真机 payload 里", not missing,
                  ("样例多出来的键: " + ",".join(missing)) if missing else "一致")
            check("D10 镜面样例的 ruler 同样是 {cpu, simd}",
                  isinstance(sample.get("ruler"), dict) and
                  set(["cpu", "simd"]).issubset(set(sample["ruler"].keys())),
                  ",".join(sorted(sample.get("ruler", {}).keys())))
        except Exception as e:
            check("D8 sn_json_sample.json 可解析", False, str(e))
    emit()

    # ----------------------------------------------------------------- [E]
    emit("[E] 自检误报反例: 正常文本里的 花括号/方括号/引号/反斜杠 不许被当成结构括号")
    fp_cases = [
        ("字符串里含成对花括号", '{"a":"x{y}z","b":1}'),
        ("字符串里含单个开花括号", '{"a":"只看左边的 { 就够骗过不跳字符串的计数器","b":1}'),
        ("字符串里含单个闭花括号", '{"a":"只看右边的 } 也一样","b":1}'),
        ("字符串里含方括号", '{"a":"paths[/sys/class/thermal/thermal_zone0/temp]","b":[1,2]}'),
        ("字符串里含转义引号", '{"a":"he said \\"hi\\" and left","b":1}'),
        ("字符串里含反斜杠与转义", '{"a":"C:\\\\sys\\\\class\\\\thermal","b":1}'),
        ("字符串里同时含 { } [ ] 引号 反斜杠", '{"a":"{[}]\\"\\\\\\\\","b":{"c":[{},[]]}}'),
        ("键名里含花括号", '{"a{b}":1,"c":2}'),
        ("诊断串: 括号计数(depth=2)", '{"json-selfcheck":"JSON 自检失败: 括号不平衡(depth=2)","n":2}'),
        ("嵌套里含花括号文本", '{"a":{"b":{"c":"}{][","d":1}}}'),
    ]
    all_ok = True
    for name, doc in fp_cases:
        try:
            json.loads(doc)
        except Exception as e:
            all_ok = False
            emit("      构造错误(不是被测对象的问题): %s -> %s" % (name, e))
    check("E1 反例本身都是合法 JSON(用严格解析器确认过)", all_ok, "%d 条" % len(fp_cases))

    fixed_says_ok = True
    naive_says_bad = 0
    for name, doc in fp_cases:
        d, ins, _bad, _k = snBraceScan(doc)
        if d != 0 or ins:
            fixed_says_ok = False
            emit("      [x] %s: 修好的计数扫描仍误报 depth=%d inStr=%s" % (name, d, ins))
        if naive_brace_scan(doc) != 0:
            naive_says_bad += 1
    check("E2 修好的计数扫描(跳字符串+转义)对 %d 条反例全部 depth=0 不误报" % len(fp_cases),
          fixed_says_ok, "%d 条" % len(fp_cases))
    check("E3 『不跳字符串』的旧式计数会在其中 %d 条上误报 —— 这正是自检误报的成因" %
          naive_says_bad, naive_says_bad >= 1, "%d/%d 条" % (naive_says_bad, len(fp_cases)))

    bracket_broken = [
        ("少一个 '}'", '{"a":1,"b":{"c":2}'),
        ("少一个 ']'", '{"a":[1,2}'),
        ("多一个 '}'", '{"a":1}}'),
    ]
    other_broken = [
        ("对象里多了尾逗号", '{"a":1,}'),
        ("键用了单引号", "{'a':1}"),
        ("数字有前导 0", '{"a":01}'),
        ("数字缺整数部分", '{"a":.5}'),
        ("JSON 结束后还有多余内容", '{"a":1} extra'),
        ("字符串里有裸控制字符", '{"a":"x\ny"}'),
    ]
    rejected = []
    for name, doc in bracket_broken + other_broken:
        try:
            json.loads(doc)
        except Exception:
            rejected.append(name)
    check("E4 %d 条真坏的样本全部被严格解析器拒绝(含括号类与文法类)" %
          (len(bracket_broken) + len(other_broken)),
          len(rejected) == len(bracket_broken) + len(other_broken),
          "被拒绝 %d 条" % len(rejected))
    depth_pairs = [(n, snBraceScan(d)[0]) for n, d in bracket_broken]
    check("E5 括号类样本在计数扫描上也确实不平衡(旁证一致, 但判决权仍在解析器)",
          all(d != 0 for _n, d in depth_pairs),
          "; ".join("%s depth=%d" % (n, d) for n, d in depth_pairs))
    check("E6 文法类样本的括号是平衡的 —— 只靠数括号根本发现不了它们",
          all(naive_brace_scan(d) == 0 for _n, d in other_broken),
          "%d 条" % len(other_broken))
    emit()

    # ----------------------------------------------------------------- [F]
    emit("[F] 源码级断言(钉住这次的两处漏括号, 防回归)")
    builders = [
        ("snlBuildJson", r"\nstd::string snlBuildJson\(const SnMeasurement& m, const SnStats& s\)\n\{"),
        ("envJson", r"\nstd::string envJson\(const EnvDerived& e, int repeats, int gapMs\)\n\{"),
        ("repeatabilityJson", r"\nstd::string repeatabilityJson\(const SnMeasurement& m\)\n\{"),
        ("referenceComparisonJson", r"\nstd::string referenceComparisonJson\(const SnStats& s\)\n\{"),
        ("linearityJson", r"\nstd::string linearityJson\(const SnMeasurement& m\)\n\{"),
        ("comparabilityJson", r"\nstd::string comparabilityJson\(const SnMeasurement& m, const SnStats& s, const ComparabilityInfo& c\)\n\{"),
    ]
    templates = {}
    for name, sig in builders:
        # comparabilityJson 里有一串 if/else if/else, 每个分支各自补一个列表的结尾 ']';
        # 互斥分支全接起来必然多出括号。所以: 主干必须平衡, 分支必须只收口不开口(delta<=0),
        # 且分支语句自身不能有未闭合的字符串。
        spine_only = (name == "comparabilityJson")
        tpl = template_of(src, sig, skip_else=spine_only)
        templates[name] = tpl
        if tpl is None:
            check("F1 %s 模板可抽取" % name, False, "没找到函数体")
            continue
        d, ins, _b, _k = snBraceScan(tpl)
        check("F1 %s 的 JSON %s模板括号平衡(depth=0, 无未闭合字符串)" %
              (name, "代表路径(if 第一分支)" if spine_only else ""),
              d == 0 and not ins,
              "depth=%d inStr=%s 模板 %d 字符" % (d, ins, len(tpl)))
        if spine_only:
            bad_branch = []
            for stmt, depth, at in body_statements(src, sig):
                if depth == 0:
                    continue
                frag = "".join(depth0_literals(stmt[at:]))
                fd, fins, _fb, _fk = snBraceScan(frag)
                if fd > 0 or fins:
                    bad_branch.append((stmt.strip()[:50], fd, fins))
            check("F1b %s 的分支语句只收口不开口(delta<=0, 无未闭合字符串)" % name,
                  not bad_branch,
                  "; ".join("%r depth=%d" % (t, d) for t, d, _i in bad_branch) if bad_branch else "全部合规")

    full = templates.get("snlBuildJson")
    if full is not None:
        un = unmatched_openers(full)
        check("F2 snlBuildJson 模板没有任何未配对的 花括号/方括号", len(un) == 0,
              ",".join("%s@%d" % (c, p) for c, p in un))
        i_head = full.find('"headroomNotes"')
        i_rend = full.find('"rendering"')
        seg = full[i_head:i_rend].rstrip().rstrip(",")
        check("F3 ruler 的 '}' 出现在 headroomNotes 之后、rendering 之前",
              i_head >= 0 and i_rend > i_head and seg.endswith("]}"),
              repr(seg[-20:]))
        i_gpu = full.find('"gpu":{')
        i_cfg = full.find('"config":{')
        seg2 = full[i_gpu:i_cfg].rstrip().rstrip(",")
        check("F4 gpu 的 '}' 出现在 gpu.note 之后、config 之前",
              i_gpu >= 0 and i_cfg > i_gpu and seg2.endswith('"}'),
              repr(seg2[-20:]))
        check("F5 模板里的 key 序列里 ruler 紧跟 notes/risks, config 紧跟 gpu(结构没被搬走)",
              full.find('"ruler":{') > full.find('"risks":') and
              full.find('"gpu":{') < full.find('"config":{') < full.find('"validity":{'),
              "")
    else:
        check("F2 snlBuildJson 模板可抽取", False, "")

    # 动态插值: 进 JSON 字符串值的 std::string(...) 必须是"条件表达式且各分支都是字面量"
    bad_interp = []
    for stmt, _depth, jat in body_statements_all(src):
        stmt = stmt[jat:]
        for arg, at in std_string_args(stmt):
            prefix = stmt[max(0, at - 9):at]
            if prefix.endswith("jsonSafe("):
                continue          # 已经过转义
            if branches_are_all_literals(arg):
                continue          # 只可能产出字面量(true/false/null/枚举名)
            line = src[:src.find(stmt)].count("\n") + 1 if False else None
            bad_interp.append(arg.strip().replace("\n", " ")[:80])
    check("F6 没有任何 const char*/运行时字符串绕过 jsonSafe 进 JSON 字符串值",
          not bad_interp,
          "; ".join("std::string(%s)" % a for a in bad_interp[:6]) if bad_interp
          else "全部经过 jsonSafe/snCanonState, 或只产出 true/false/null")

    check("F7 environment.stability.state 走了 jsonSafe + snCanonState",
          'jsonSafe(std::string(snCanonState(e.state)))' in src, "")
    check("F8 roundsDetail 的 envState 走了 jsonSafe + snCanonState",
          'jsonSafe(std::string(snCanonState(r.envState)))' in src, "")
    check("F9 gpuBoundEvidence.verdict / credibility.verdict 走了 jsonSafe",
          'jsonSafe(std::string(cstrOr(verdict, "")))' in src and
          'jsonSafe(std::string(cstrOr(dispVerdict, "")))' in src, "")

    # snprintf 里那些"自成一体"的 JSON 片段也必须平衡
    # (repeatability.roundsDetail[].{...} 与 environment.probe.perCore[].{...} 就是这种形态:
    #  它们不经过 j += 拼接, 而是先 snprintf 到一个定长缓冲, 再 roundsJson/per += b;
    #  所以上面按 j += 抽模板的检查覆盖不到它们 —— 这里单独抽格式串那一格。)
    snprintf_frags = []
    for m in re.finditer(r"snprintf\s*\(", src):
        args = balanced_paren_content(src, m.end() - 1)
        parts = split_top_level_commas(args)
        if len(parts) < 3:
            continue
        fmt = concat_pure_literals(parts[2])
        if fmt is None or "{" not in fmt:
            continue
        d, ins, _b, _k = snBraceScan(fmt)
        if d != 0 or ins:
            snprintf_frags.append((fmt[:60], d, ins))
    check("F10 snprintf 里成段的 JSON 模板(%s 占位符形式)也都括号平衡",
          not snprintf_frags,
          "; ".join("%r depth=%d" % (t, d) for t, d, _i in snprintf_frags) if snprintf_frags
          else "roundsDetail / perCore 等片段全部平衡(共 %d 处)" % 0)
    emit()

    # ----------------------------------------------------------------- [G]
    emit("[G] 自检实现本身的硬要求")
    check("G1 计数扫描跳过字符串与转义(源码里有 inStr / esc 两条分支)",
          "r.inStr" in src and "esc = true;" in src and "if (c == '\\\\')" in src.replace("  ", " "),
          "SnBraceScan")
    check("G2 判决权在真解析器手里(不是计数器)",
          "struct SnJsonParser" in src and "bool parseTop()" in src and
          "const bool parsed = p.parseTop();" in src, "SnJsonParser::parseTop()")
    check("G3 解析器拒绝时写『JSON 确实坏了』并带字节偏移",
          "JSON 确实坏了: 严格解析器在第 " in src, "")
    check("G4 解析器通过而计数器不通过时写『自检误报(不是 JSON 坏了)』",
          "自检误报(不是 JSON 坏了)" in src, "")
    check("G5 误报不会阻断交付(交付只看 chk.ok)",
          "if (!chk.ok) {" in src and "chk.selfCheckDisagreed" in src and
          "sn json selfcheck FALSE POSITIVE" in src, "")
    check("G6 旧签名 snJsonSelfCheck(s, &why) 已不存在(避免两套判据并存)",
          "snJsonSelfCheck(out, &why)" not in src and
          "snJsonSelfCheck(const std::string& s, std::string* why)" not in src, "")
    check("G7 解析器覆盖对象/数组/字符串/数字/字面量五种文法",
          all(k in src for k in ("bool object()", "bool array()", "bool str()",
                                 "bool number()", "bool literal(const char* lit)")), "")
    emit()

    # ----------------------------------------------------------------- [H]
    emit("[H] 诚实边界(打印, 不假装)")
    emit("      * 真机 8.1 那份 30801 字节的成功结果在日志里取不到:")
    emit("        phone81/report-latest.txt 第 1166 行的『原始 JSON=』写的是")
    emit("        {\"ok\":false,\"error\":\"... 括号不平衡(depth=2) ...\"} —— 只有失败文本,")
    emit("        native 没把那段 30801 字节的 out 落盘(snJsonSelfCheck 失败后直接 return)。")
    emit("        本脚本因此用同一份模板在同一台真机上产出的完整 payload(30372 字节)做证据,")
    emit("        它带着同样的两个漏括号, 修好上一轮的野指针之后 depth 同样是 2。")
    emit("      * 本容器里没有能在主机上跑的 C++ 编译器(SDK 的 clang 默认目标是")
    emit("        x86_64-w64-windows-gnu, 但没装对应的 C 运行库/头文件), 所以")
    emit("        snJsonSelfCheck 的 C++ 实现本身只做了源码级断言([F][G]) +")
    emit("        同语义的 Python 镜像([B][C][D][E]); 真机复跑 GPU-SNL 小节才是最终确认。")
    emit()

    emit("-" * 78)
    emit("检查项 %d 个, FAIL %d 个" % (checks, fails))
    emit("-" * 78)
    if fails == 0:
        emit("[OK] 全部通过: 根因已定位(ruler/gpu 两个漏 '}'), 修后真机 payload 能被 json.loads() 解析,")
        emit("     且自检已能区分『JSON 真的坏了』与『自检误报』。")
    else:
        emit("[NG] 有 %d 项没通过。" % fails)
    _OUT.close()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
