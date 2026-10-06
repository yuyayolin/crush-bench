#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gpu7 shader offline checker.

真机跑不了 GL, 这个脚本用静态核对把"无法在真机验证"的损失补回来:
解析 gpu7_renderer.cpp 里全部 GLSL 源码字符串 + 程序登记表 + 各项负载里的
glUseProgram/glDrawArrays/setXXX 调用, 对 34 个程序逐条核对下列规则。

  错误级(不通过):
    R1  VS 的 out 集合 == FS 的 in 集合(名字 + 类型完全一致)
    R3  片元着色器里不许出现 gl_VertexID / gl_InstanceID(ES 3.00 只在 VS 提供)
    R4  gl_PointCoord 只能出现在"点绘制"(GL_POINTS)程序的 FS 里
    R5  gl_PointSize 只能出现在"点绘制"程序的 VS 里(且点程序必须写)
    R6  着色器里"被用到"的 uniform, C++ 侧必须有对应 set 调用(F1/I1/2F/3F)
    R7  C++ 侧 set 的每个 uniform 名字都必须在着色器里声明过(拼写不一致)
    R9  程序登记表条数 == ProgId 枚举条数 == 34, 顺序一致, 每个程序都被负载取用
    R10 一律 #version 300 es; FS 必须有 precision 声明(float 在 FS 无默认精度)
    R11 顶点着色器里不许出现 gl_PointCoord / gl_FragCoord 这类片元专用内建
    R12 vecN(...) 构造函数的实参分量数必须正好等于 N
        (vec4(aPos, 0.0, 0.0, 1.0) = 2+1+1+1 = 5 就是真机上踩过的坑)

  告警级(通过但有备注):
    R2  varying 只声明不使用 / 只在单侧被使用(驱动会做未使用消除, 两端消除结果
        不一致就会在链接期报 Undeclared variable —— 真机 hough_vote_b 的报错形态)
    R8  着色器声明了但既没用、C++ 也没 set 的 uniform(死声明, 不报错但应清理)

用法:  python tools/gpu7_shader_check.py [--cpp 路径] [--md 输出路径]
退出码: 0 = 全部通过(允许有告警), 1 = 有错误级不通过项
"""

import argparse
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BT = chr(96)

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CPP = os.path.join(HERE, "..", "entry", "src", "main", "cpp", "gpu7", "gpu7_renderer.cpp")
DEFAULT_MD = os.path.join(HERE, "gpu7_shader_check_report.md")

SHADER_RE = re.compile(r'const\s+char\s*\*\s*(\w+)\s*=\s*R"GLSL\((.*?)\)GLSL"\s*;', re.S)
TABLE_RE = re.compile(r'g_progTable\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\};', re.S)
ENTRY_RE = re.compile(r'/\*\s*(P_\w+)\s*\*/\s*\{\s*(\w+)\s*,\s*(\w+)\s*,\s*"([^"]*)"\s*\}')
ENUM_RE = re.compile(r'enum\s+ProgId\s*\{(.*?)\};', re.S)
GETPROG_RE = re.compile(r'GLuint\s+(\w+)\s*=\s*getProg\(\s*(P_\w+)\s*\)')
INLINE_GETPROG_RE = re.compile(r'glUseProgram\(\s*getProg\(\s*(P_\w+)\s*\)\s*\)')
USE_RE = re.compile(r'glUseProgram\(\s*(\w+)\s*\)')
POINTS_RE = re.compile(r'glDrawArrays\(\s*GL_POINTS')
FULLSCREEN_RE = re.compile(r'drawFullscreen\(\s*\)')
SET_RE = re.compile(r'set(?:F1|I1|2F|3F)\(\s*(\w+)\s*,\s*"(\w+)"')
UNIFORM_RE = re.compile(r'\buniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(\[\s*\d+\s*\])?\s*;')
DECL_RE = re.compile(r'(?m)^\s*(?:(flat|smooth|noperspective)\s+)?(in|out)\s+'
                     r'(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;')
TYPEDECL_RE = re.compile(r'\b(?:lowp\s+|mediump\s+|highp\s+)?'
                         r'(float|int|vec[234]|ivec[234]|bvec[234]|mat[234])\s+(\w+)\b')
VECCTOR_RE = re.compile(r'\b(vec[234])\s*\(([^()]*)\)')
LITERAL_RE = re.compile(r'^(\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+)$')
SWIZZLE_RE = re.compile(r'^(\w+)\.([xyzwrgba]{1,4})$')


def strip_comments(src):
    """把 GLSL 注释替换成空格(保持长度/行号), 免得注释里的标识符被当成代码"""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                out.append(' ')
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            out.append('  ')
            i += 2
            while i < n and not (src[i] == '*' and i + 1 < n and src[i + 1] == '/'):
                out.append('\n' if src[i] == '\n' else ' ')
                i += 1
            out.append('  ')
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def uses_identifier(body, name):
    return re.search(r'\b' + re.escape(name) + r'\b', body) is not None


def body_after_decls(src):
    """声明区之后的函数体(用于判断 varying/uniform 是否"真正被使用")"""
    idx = src.find('void main')
    return src[idx:] if idx >= 0 else src


def find_functions(src):
    """切出 FrameStats loadXxx(int frames) { ... } 的函数体"""
    funcs = {}
    for m in re.finditer(r'FrameStats\s+(\w+)\s*\(\s*int\s+\w+\s*\)\s*\{', src):
        i = m.end() - 1
        depth = 0
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        funcs[m.group(1)] = src[m.end():i]
    return funcs


def type_components(typ):
    if typ in ("float", "int"):
        return 1
    m = re.fullmatch(r'(?:[ib])?vec([234])', typ)
    if m:
        return int(m.group(1))
    return None


def vec_ctor_check(body):
    """R12: vecN(...) 实参分量数检查(保守: 实参是表达式/未知变量就跳过该条)"""
    syms = {}
    for typ, name in TYPEDECL_RE.findall(body):
        syms[name] = typ
    problems = []
    for m in VECCTOR_RE.finditer(body):
        typ = m.group(1)
        want = int(typ[-1])
        args = [a.strip() for a in m.group(2).split(',') if a.strip() != '']
        if not args:
            continue
        total = 0
        unknown = False
        scalar_only = len(args) == 1
        for a in args:
            if LITERAL_RE.match(a):
                total += 1
                continue
            sw = SWIZZLE_RE.match(a)
            if sw and sw.group(1) in syms:
                comps = type_components(syms[sw.group(1)])
                if comps is None:
                    unknown = True
                else:
                    total += len(sw.group(2))
                continue
            if re.fullmatch(r'\w+', a) and a in syms:
                comps = type_components(syms[a])
                if comps is None:
                    unknown = True
                else:
                    total += comps
                    scalar_only = scalar_only and comps == 1
                continue
            unknown = True  # 表达式或未知标识符: 判不了, 跳过
        if unknown:
            continue
        if scalar_only and total == 1:
            continue  # vecN(单个标量) 是合法的, 所有分量都填这个标量
        if total != want:
            problems.append("%s(%s) -> %d 个分量, 需要 %d" % (typ, m.group(2)[:60], total, want))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp", default=DEFAULT_CPP)
    ap.add_argument("--md", default=DEFAULT_MD)
    args = ap.parse_args()

    with open(os.path.abspath(args.cpp), "r", encoding="utf-8") as f:
        cpp = f.read()

    shaders = dict((name, strip_comments(body)) for name, body in SHADER_RE.findall(cpp))
    table_match = TABLE_RE.search(cpp)
    if not table_match:
        print("FATAL: cannot find g_progTable")
        return 2
    table_body = table_match.group(1)
    entries = ENTRY_RE.findall(table_body)

    enum_match = ENUM_RE.search(cpp)
    prog_ids = []
    if enum_match:
        for chunk in enum_match.group(1).split(','):
            em = re.search(r'(P_\w+)', chunk)
            if em:
                prog_ids.append(em.group(1))
    prog_ids = [p for p in prog_ids if p != 'P_PROG_COUNT']

    prog_draw = {}
    prog_sets = {}
    prog_loaded = set()
    for body in find_functions(cpp).values():
        varmap = {}
        for var, pid in GETPROG_RE.findall(body):
            varmap[var] = pid
            prog_loaded.add(pid)
        for pid in INLINE_GETPROG_RE.findall(body):
            prog_loaded.add(pid)
        cur = None
        for line in body.splitlines():
            m = USE_RE.search(line)
            if m:
                cur = varmap.get(m.group(1))
                continue
            inline = INLINE_GETPROG_RE.search(line)
            if inline:
                cur = inline.group(1)
                continue
            if cur is not None:
                if POINTS_RE.search(line):
                    prog_draw.setdefault(cur, set()).add("POINTS")
                elif FULLSCREEN_RE.search(line):
                    prog_draw.setdefault(cur, set()).add("TRIANGLES")
        for var, uname in SET_RE.findall(body):
            pid = varmap.get(var)
            if pid:
                prog_sets.setdefault(pid, set()).add(uname)

    rows = []
    for pid, vs, fs, label in entries:
        row = {"id": pid, "label": label, "vs": vs, "fs": fs,
               "errors": [], "warnings": [], "notes": [],
               "vs_out": [], "fs_in": [], "uniforms": []}
        vsrc = shaders.get(vs)
        fsrc = shaders.get(fs)
        if vsrc is None:
            row["errors"].append("R1 顶点着色器源码 %s 不存在" % vs)
        if fsrc is None:
            row["errors"].append("R1 片元着色器源码 %s 不存在" % fs)
        if vsrc is None or fsrc is None:
            rows.append(row)
            continue

        v_decl = DECL_RE.findall(vsrc)
        f_decl = DECL_RE.findall(fsrc)
        v_out = dict(((t, n), q) for q, d, t, n in v_decl if d == "out")
        f_in = dict(((t, n), q) for q, d, t, n in f_decl if d == "in")
        row["vs_out"] = ["%s %s" % k for k in sorted(v_out)]
        row["fs_in"] = ["%s %s" % k for k in sorted(f_in)]

        # R1
        for key in sorted(set(v_out) | set(f_in)):
            if key not in v_out:
                row["errors"].append("R1 FS 声明了 in %s %s, 顶点着色器没有同名同类型的 out" % key)
            elif key not in f_in:
                row["errors"].append("R1 VS 声明了 out %s %s, 片元着色器没有同名同类型的 in" % key)

        # R2
        v_main = body_after_decls(vsrc)
        f_main = body_after_decls(fsrc)
        for (t, n) in sorted(v_out):
            if not uses_identifier(v_main, n):
                row["warnings"].append("R2 VS 的 out %s %s 声明后没有使用" % (t, n))
        for (t, n) in sorted(f_in):
            if not uses_identifier(f_main, n):
                row["warnings"].append("R2 FS 的 in %s %s 声明后没有使用" % (t, n))

        # R3
        for builtin in ("gl_VertexID", "gl_InstanceID"):
            if uses_identifier(fsrc, builtin):
                row["errors"].append("R3 片元着色器引用了 %s(ES 3.00 只在顶点着色器提供)" % builtin)
        if uses_identifier(vsrc, "gl_VertexID"):
            row["notes"].append("VS 用 gl_VertexID")

        # R11
        for builtin in ("gl_PointCoord", "gl_FragCoord", "gl_FrontFacing", "gl_FragDepth"):
            if uses_identifier(vsrc, builtin):
                row["errors"].append("R11 顶点着色器引用了片元专用内建 %s" % builtin)

        # R10
        if not vsrc.lstrip().startswith("#version 300 es"):
            row["errors"].append("R10 VS 不是 #version 300 es")
        if not fsrc.lstrip().startswith("#version 300 es"):
            row["errors"].append("R10 FS 不是 #version 300 es")
        if "precision" not in fsrc:
            row["errors"].append("R10 FS 缺 precision 声明")

        # R4 / R5
        draws = prog_draw.get(pid, set())
        is_points = "POINTS" in draws
        row["draw"] = "/".join(sorted(draws)) if draws else "?"
        if is_points:
            row["notes"].append("点绘制")
        if uses_identifier(fsrc, "gl_PointCoord"):
            row["notes"].append("FS 用 gl_PointCoord")
            if not is_points:
                row["errors"].append("R4 非点绘制程序(%s)的 FS 引用了 gl_PointCoord" % row["draw"])
        if uses_identifier(vsrc, "gl_PointSize"):
            row["notes"].append("VS 写 gl_PointSize")
            if not is_points:
                row["errors"].append("R5 非点绘制程序(%s)的 VS 写了 gl_PointSize" % row["draw"])
        if is_points and not uses_identifier(vsrc, "gl_PointSize"):
            row["errors"].append("R5 点绘制程序的 VS 没写 gl_PointSize")

        # R6 / R7 / R8
        declared = {}
        for owner, src in (("VS", vsrc), ("FS", fsrc)):
            for typ, name, arr in UNIFORM_RE.findall(src):
                declared[name] = (owner, typ, arr.replace(" ", "") if arr else "")
                if arr:
                    row["notes"].append("数组 uniform %s%s(%s)" % (name, arr.replace(" ", ""), owner))
        setnames = prog_sets.get(pid, set())
        for name in sorted(declared):
            owner, typ, arr = declared[name]
            used = uses_identifier(body_after_decls(vsrc if owner == "VS" else fsrc), name)
            if used and name not in setnames:
                row["errors"].append("R6 %s 用了 uniform %s(%s), C++ 侧没有对应 set 调用"
                                     % (owner, name, typ))
            elif (not used) and name not in setnames:
                row["warnings"].append("R8 %s 声明了 uniform %s(%s) 但既没用到也没被 set(死声明)"
                                       % (owner, name, typ))
        for name in sorted(setnames):
            if name not in declared:
                row["errors"].append("R7 C++ 侧 set 了 uniform %s, 着色器里没有声明(拼写不一致?)" % name)
        row["uniforms"] = sorted(declared)

        # R12
        for p in vec_ctor_check(vsrc) + vec_ctor_check(fsrc):
            row["errors"].append("R12 构造函数分量数: " + p)

        # R9
        if pid not in prog_loaded:
            row["errors"].append("R9 没有任何负载 getProg(%s)" % pid)
        rows.append(row)

    struct_problems = []
    table_ids = [pid for pid, _, _, _ in entries]
    if len(table_ids) != 34:
        struct_problems.append("R9 程序登记表条数 = %d, 应为 34" % len(table_ids))
    if len(prog_ids) != 34:
        struct_problems.append("R9 ProgId 枚举条数 = %d, 应为 34" % len(prog_ids))
    for a, b in zip(table_ids, prog_ids):
        if a != b:
            struct_problems.append("R9 程序登记表与 ProgId 顺序不一致: %s vs %s" % (a, b))
    for name in sorted(shaders):
        if not re.search(r'\b' + name + r'\b', table_body):
            struct_problems.append("R9 着色器 %s 没有被任何程序登记表项使用" % name)

    ok_rows = [r for r in rows if not r["errors"]]
    bad_rows = [r for r in rows if r["errors"]]
    warn_rows = [r for r in rows if r["warnings"] and not r["errors"]]

    L = []
    L.append("# gpu7 着色器离线核对报告")
    L.append("")
    L.append("由 " + BT + "tools/gpu7_shader_check.py" + BT + " 自动生成; 核对对象 "
             + BT + "entry/src/main/cpp/gpu7/gpu7_renderer.cpp" + BT + "。")
    L.append("")
    L.append("- 程序数: %d (无错误 %d / 有错误 %d / 无错误但有告警 %d)"
             % (len(rows), len(ok_rows), len(bad_rows), len(warn_rows)))
    L.append("- 着色器源码字符串: %d" % len(shaders))
    L.append("- 表结构检查(条数/顺序/取用): %s" % ("通过" if not struct_problems else "不通过"))
    L.append("")
    L.append("## 逐程序核对表")
    L.append("")
    L.append("| # | ProgId | 程序名 | VS | FS | 绘制 | VS out | FS in | uniform(VS+FS) | C++ set | 结论 |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for i, r in enumerate(rows):
        verdict = "不通过" if r["errors"] else ("通过(告警 %d)" % len(r["warnings"]) if r["warnings"] else "通过")
        L.append("| %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            i, r["id"], r["label"], r["vs"], r["fs"], r.get("draw", "?"),
            ", ".join(r["vs_out"]) or "-", ", ".join(r["fs_in"]) or "-",
            ", ".join(r["uniforms"]) or "-",
            ", ".join(sorted(prog_sets.get(r["id"], set()))) or "-", verdict))
    L.append("")
    if bad_rows:
        L.append("## 不通过明细(错误级)")
        L.append("")
        for r in bad_rows:
            for p in r["errors"]:
                L.append("- " + BT + r["label"] + BT + " (" + r["id"] + "): " + p)
        L.append("")
    if warn_rows:
        L.append("## 告警明细(可以通过, 但值得清理)")
        L.append("")
        for r in warn_rows:
            for p in r["warnings"]:
                L.append("- " + BT + r["label"] + BT + " (" + r["id"] + "): " + p)
        L.append("")
    if struct_problems:
        L.append("## 表结构问题")
        L.append("")
        for p in struct_problems:
            L.append("- " + p)
        L.append("")
    L.append("## 规则清单")
    L.append("")
    L.append("错误级: R1 VS out == FS in(名字+类型); R3 片元里禁止 gl_VertexID/gl_InstanceID;")
    L.append("R4 gl_PointCoord 只在点程序 FS; R5 gl_PointSize 只在点程序 VS;")
    L.append("R6 被用到的 uniform 必须有 C++ set; R7 C++ set 的 uniform 必须已声明;")
    L.append("R9 登记表/枚举/取用一致(34 条); R10 #version 300 es + FS precision;")
    L.append("R11 顶点里禁止片元专用内建; R12 vecN 构造函数分量数正好等于 N。")
    L.append("")
    L.append("告警级: R2 varying 只声明不使用(驱动消除后可能两端不一致); R8 死 uniform 声明。")
    L.append("")
    with open(os.path.abspath(args.md), "w", encoding="utf-8") as f:
        f.write("\n".join(L))

    print("shaders: %d   programs: %d   clean: %d   errors: %d   warnings-only: %d"
          % (len(shaders), len(rows), len(ok_rows), len(bad_rows), len(warn_rows)))
    for r in rows:
        status = "FAIL" if r["errors"] else ("warn" if r["warnings"] else "OK  ")
        detail = "; ".join(r["errors"] + r["warnings"])[:120]
        print("%s  %-22s %-18s %-18s %-10s %s" % (
            status, r["label"], r["vs"], r["fs"], r.get("draw", "?"), detail))
    if struct_problems:
        print("struct problems:")
        for p in struct_problems:
            print("  - " + p)
    print("report: " + os.path.abspath(args.md))
    return 1 if (bad_rows or struct_problems) else 0


if __name__ == "__main__":
    sys.exit(main())
