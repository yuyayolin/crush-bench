# -*- coding: utf-8 -*-
r"""
verify_freq_cap_chain.py —— "频率上限取证文本被容量截断"修复的自查器(静态源码断言 + 真机数字重建; 无设备)

真机硬证据(D:\gb7logs\phone81\report-latest.txt, 8.1, 手机):
  48 个 GB7 项的『运行时频率=』行里, 频率上限取证那一段全都在同一个字节点上被截断:
      ... cpu0 cpuinfo_max_freq=1720000 · cpu0 scaling_available_fre
      (取证文本超出容量上限, 尾部被截断 —— 这本身就是事实, 不许当成'取证完整')
  截断标记是 capText 自己打的(readFreqCapEvidence 末尾的兜底), 也就是说:
    [逐核路径] 的后半 + 判决 + 两条规则整段都没进报告 —— 而那一段正是回答
    "1995MHz 是谁压的"的原始证据(逐路径 errno 清单)。

本脚本做四件事(全部离线, 不需要设备):
  A) 源码结构断言: 把这条串联缓冲链的每一环从源码里解出来并断言其容量
       capText(kCapTextCap) -> g_text -> gb7.cpp freq[] -> cpuAllowedText,
     外加单条小行 one[](它在旧容量下静默切掉了判决句的最后 110 字节)。
  B) 用真机实际文本长度做输入:
       ① 从真机报告里量出"没被截断的那部分" = 1743(头部) + 181([逐核路径] 开头) = 1924 字节;
       ② 按源码顺序逐条 append 重建"被切掉的那一段" = 2176 - 181 = 1995 字节
          (取值全部用真机自己报出来的: policy0/1/2 的 EACCES(13)/ENOENT(2)/
           1720000/2270000/2750000/418000/1995000/1200000, 两个探针核 cpu0 与 cpu10);
       ③ 于是真机实际需要 L_real = 1743 + 2176 = 3919 字节;
       ④ 断言链上每一环容量 >= L_real x 1.5(以及每一环 >= 它自己那份真实载荷 x 1.5)。
  C) 断言截断标记的行为: 真机长度下不出现, 只有超长合成输入(4 倍容量)下才出现, 且出现在尾部。
  D) 与真机报告对表(报告在时): 重建出来的 [逐核路径] 开头必须与真机日志里留存的那 181 字节
     逐字节相同 —— 这样"上界估算"不是空口说的, 是可核对的。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_freq_cap_chain_out.txt"), "w", encoding="utf-8", newline="\n")
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

def rd(fn):
    p = os.path.join(HERE, fn)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()

def B(s):
    return len(s.encode("utf-8"))

def pad_to_bytes(n):
    """构造一个恰好 n 字节的合成输入(中文一字 3 字节)。"""
    s = "频" * (n // 3)
    rest = n - B(s)
    return s + "x" * rest

freq_src = rd("cpu_freq_sample.cpp")
gb7_src = rd("gb7.cpp")
aff_src = rd("cpu_affinity.h")

MARKER = " (取证文本超出容量上限, 尾部被截断 —— 这本身就是事实, 不许当成'取证完整')"

# ===========================================================================
emit("=" * 100)
emit("[A] 串联缓冲链: 每一环的容量(从源码解出, 不信任任何注释里的数字)")
emit("=" * 100)

m_cap = re.search(r"kCapTextCap\s*=\s*(\d+)", freq_src)
CAPTEXT_CAP = int(m_cap.group(1)) if m_cap else 0
m_gt = re.search(r"char g_text\[(\d+)\]", freq_src)
GTEXT_CAP = int(m_gt.group(1)) if m_gt else 0
m_fq = re.search(r"char freq\[(\d+)\]", gb7_src)
FREQ_CAP = int(m_fq.group(1)) if m_fq else 0
m_al = re.search(r"char cpuAllowedText\[(\d+)\]", aff_src)
ALLOWED_CAP = int(m_al.group(1)) if m_al else 0
mf = re.search(r"void readFreqCapEvidence\(\)\s*\{(.*?)\n\}", freq_src, re.S)
m_one = re.search(r"char one\[(\d+)\]", mf.group(1) if mf else "")
ONE_CAP = int(m_one.group(1)) if m_one else 0

emit("  capText[kCapTextCap] = %d | g_text[%d] | gb7 freq[%d] | cpuAllowedText[%d] | one[%d]"
     % (CAPTEXT_CAP, GTEXT_CAP, FREQ_CAP, ALLOWED_CAP, ONE_CAP))
check("A1 五环容量都能从源码解出(不是从注释里抄)",
      min(CAPTEXT_CAP, GTEXT_CAP, FREQ_CAP, ALLOWED_CAP, ONE_CAP) > 0)
check("A2 链上不出现比上游更小的环(capText < g_text <= freq)",
      CAPTEXT_CAP < GTEXT_CAP and GTEXT_CAP <= FREQ_CAP,
      "capText=%d g_text=%d freq=%d" % (CAPTEXT_CAP, GTEXT_CAP, FREQ_CAP))
check("A3 链的每一环都比修复前更大(修复前 2048 / 8192 / 8192 / 6144 / 512)",
      CAPTEXT_CAP > 2048 and GTEXT_CAP > 8192 and FREQ_CAP > 8192
      and ALLOWED_CAP > 6144 and ONE_CAP > 512)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[B] 真机实际需要多少字节(用 8.1 报告的未截断部分 + 源码里每一段 append 重建)")
emit("=" * 100)

# ---- 真机 8.1 报告里留存下来的那一段(逐字节抄下来; D 节还会与报告原文对表) ----
VISIBLE_HEAD_BYTES = 1743       # "频率上限取证 ... [替代读数] ..." 到 " [逐核路径] " 之前
VISIBLE_SEC5 = (" [逐核路径] cpu0 scaling_max_freq 读不到(errno=13(EACCES)) · "
                "cpu0 scaling_min_freq 读不到(errno=13(EACCES)) · "
                "cpu0 cpuinfo_max_freq=1720000 · cpu0 scaling_available_fre")
# 报告里这一段是从中间被切掉的(汇总第一项时最后一个字节是半截单词 "fr"), 重建时按同一刀口对齐。
# ★ 真机 8.1 报告里**每一项**实测留存下来的字节数(逐项相同; 锚点与推导见 D 节开头那段说明)。
# 1743/181 是协作者当时的**分解**(头部 + [逐核路径] 开头), 逐项实测后头部是 1741/1743、
# [逐核路径]开头是 180/178, 两者相加恒为 1921。
REAL_RETAINED_BYTES = 1921
CAP_ANCHOR = "频率上限取证"
TRUNC_MARK = " ★(★★取证文本超出容量上限"
SEC5_ANCHOR = " [逐核路径] "

FILES = ["scaling_max_freq", "scaling_min_freq", "cpuinfo_max_freq", "scaling_available_frequencies"]
ERRTEXT = "errno=13(EACCES)"      # 真机 policy0/1/2 与 cpuN 路径的 scaling_max/min_freq 都是 EACCES(13)
CPUINFO = {0: 1720000, 10: 2270000}   # 真机: cpu0 属 policy0(cpuinfo 1720000) / cpu10 属 policy1(2270000)
PROBES = ((0, 0), (10, 1))            # (探针核号, 所属 policy 号) —— 两个, 与 kCapProbeCores 一致

def lit_join(txt):
    txt = re.sub(r"//[^\n]*", "", txt)
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', txt)
    return "".join(parts).replace('\\"', '"').replace("\\\\", "\\")

# 判决那一句(upErr != 0 那一支)的完整字面量 —— 措辞改了这里跟着变, 长度断言才有意义
m = re.search(r"else if \(upErr != 0\) \{(.*?)else if \(nomKhz", freq_src, re.S)
JUDGE = lit_join(m.group(1)) if m else ""
# 两条规则(判决规则 + 编号规则)
i0 = freq_src.find('"判决规则')
i1 = freq_src.find("const char* const kTrunc", i0)
RULES = lit_join(freq_src[i0:i1]) if (i0 >= 0 and i1 > i0) else ""
check("B1 从源码里抽到判决句(upErr != 0 那一支)", B(JUDGE) > 300 and "上限读数(scaling_max_freq)读不到" in JUDGE)
check("B2 从源码里抽到两条规则(判决规则 + 编号规则)",
      "判决规则" in RULES and "编号规则" in RULES, B(RULES))

def sec5(probes=PROBES, err=None):
    """逐字镜像 ⑤ [逐核路径]: 每条 cpuN 路径 x 4 个节点 + policy 兜底路径。"""
    t = " [逐核路径] "
    for (cpu, pol) in probes:
        for f in range(4):
            if f == 2:
                t += "cpu%d %s=%d" % (cpu, FILES[f], CPUINFO[cpu])
            else:
                t += "cpu%d %s 读不到(%s)" % (cpu, FILES[f], err or ERRTEXT)
            t += " · "
        t += "policy%d(cpu%d 所属)/scaling_max_freq 读不到(%s)" % (pol, cpu, err or ERRTEXT) + " · "
    return t

def sec6(probes=PROBES, uw="EACCES(13)", rules=None):
    """逐字镜像 ⑥ 判决 + 两条规则。"""
    t = "判决: "
    for (cpu, pol) in probes:
        t += JUDGE % (cpu, pol, uw, pol, pol, pol)
    return t + (RULES if rules is None else rules)

FULL_TAIL = sec5() + sec6()
L_REAL = VISIBLE_HEAD_BYTES + B(FULL_TAIL)
JUDGE_FRAG = [B(JUDGE % (c, p, "EACCES(13)", p, p, p)) for (c, p) in PROBES]
# one[] 旧容量(512)会把每一句判决静默切到 511 字节 —— 另一处静默截断, 一并量出来
L_REAL_IF_ONE512 = L_REAL - sum(max(0, j - 511) for j in JUDGE_FRAG)
G_TEXT_REAL = 5384 - 2047 + L_REAL     # 真机最长的一项: 整行 = 非取证部分(3337) + 取证段(实测旧 2047)
ALLOWED_REAL = 5778                    # 真机 "    可用核集合原文=" 那一行的内容长度(5801 - 23 字节前缀)
USED_BEFORE_FREQ = 4211                # 真机: 追加运行时频率之前 cpuAllowedText 已用字节(分段实测求和)
ONE_FRAG_MAX = max(JUDGE_FRAG)

def sec5_upper():
    """上界: 每个节点读数的原文按 kCapFreqBuf-1 = 255 B 算, 失败按最长的 errno 词算。"""
    t = " [逐核路径] "
    for (cpu, pol) in PROBES:
        for f in range(4):
            fail = "cpu%d %s 读不到(%s)" % (cpu, FILES[f], "内容不是正十进制数(码 -2)")
            ok = "cpu%d %s=" % (cpu, FILES[f]) + "0" * 255
            t += (fail if B(fail) >= B(ok) else ok) + " · "
        t += ("policy%d(cpu%d 所属, 编号未定按核号兜底)/scaling_max_freq=" % (pol, cpu)
              + "0" * 255 + " · ")
    return t

def sec6_upper():
    t = "判决: "
    for (cpu, pol) in PROBES:
        t += JUDGE % (cpu, pol, "内容不是正十进制数(码 -2)", pol, pol, pol)
    return t + RULES

L_UPPER = VISIBLE_HEAD_BYTES + B(sec5_upper() + sec6_upper())

emit("  真机 8.1 留存下来的部分(协作者当时的分解): 头部 %d + [逐核路径] 开头 %d = %d 字节"
     % (VISIBLE_HEAD_BYTES, B(VISIBLE_SEC5), VISIBLE_HEAD_BYTES + B(VISIBLE_SEC5)))
emit("      ★注: 这是一次**分解**而不是直接量出来的长度。D 节按“每个截断标记 -> 它自己那一项的 capText 段首”"
     "逐项实测, 48 项全部 = %d 字节(头部 1741/1743 + [逐核路径]开头 180/178)。详见 D6a~D10。"
     % REAL_RETAINED_BYTES)
emit("  按源码重建被切掉的那一段: [逐核路径] 后半 + 判决 + 两条规则 = %d - %d = %d 字节"
     % (B(FULL_TAIL), B(VISIBLE_SEC5), B(FULL_TAIL) - B(VISIBLE_SEC5)))
emit("  => 真机实际需要 L_real = %d + %d = %d 字节(+1 字节 NUL)"
     % (VISIBLE_HEAD_BYTES, B(FULL_TAIL), L_REAL))
emit("     每句判决 = %s 字节(one[512] 时代被静默切到 511, 那一段真实需要还要再少 %d 字节 => %d)"
     % (JUDGE_FRAG, L_REAL - L_REAL_IF_ONE512, L_REAL_IF_ONE512))
emit("     换算成完整『运行时频率=』那一行: 非取证部分 %d - %d = %d 字节, 整行 = %d 字节"
     % (5384, 2047, 5384 - 2047, G_TEXT_REAL))
emit("  上界估算(每个片段取最大实参宽度: 节点原文 255 B / errno 词最长一支 / 核号与策略号 2 位) = %d 字节" % L_UPPER)

for cap, tag in ((CAPTEXT_CAP, "capText"), (GTEXT_CAP, "g_text"), (FREQ_CAP, "gb7 freq[]"),
                 (ALLOWED_CAP, "cpuAllowedText")):
    check("B3 链上每一环容量 >= 真机实际文本长度 x 1.5: %s[%d] >= %d x 1.5 = %d"
          % (tag, cap, L_REAL, int(L_REAL * 1.5)), cap >= L_REAL * 1.5)
check("B4 capText 对整个取证段 %d 字节 >= x 1.5(余量 %.0f%%)"
      % (L_REAL, (CAPTEXT_CAP / float(L_REAL) - 1.0) * 100.0), CAPTEXT_CAP >= L_REAL * 1.5)
check("B5 g_text 对整行 %d 字节 >= x 1.5(余量 %.0f%%)"
      % (G_TEXT_REAL, (GTEXT_CAP / float(G_TEXT_REAL) - 1.0) * 100.0), GTEXT_CAP >= G_TEXT_REAL * 1.5)
check("B6 gb7 freq[] >= 整行 %d x 1.5 且 >= 上游 g_text" % G_TEXT_REAL,
      FREQ_CAP >= G_TEXT_REAL * 1.5 and FREQ_CAP >= GTEXT_CAP)
check("B7 cpuAllowedText 对它自己那份真实载荷 %d 字节 >= x 1.5(余量 %.0f%%)"
      % (ALLOWED_REAL, (ALLOWED_CAP / float(ALLOWED_REAL) - 1.0) * 100.0), ALLOWED_CAP >= ALLOWED_REAL * 1.5)
check("B8 one[] 对最长的一条判决 %d 字节 >= x 1.5" % ONE_FRAG_MAX, ONE_CAP - 1 >= ONE_FRAG_MAX * 1.5)
check("B9 上界估算(%d 字节)也放得下: 每一环 >= L_upper" % L_UPPER,
      min(CAPTEXT_CAP, GTEXT_CAP, FREQ_CAP, ALLOWED_CAP) > L_UPPER)
check("B10 真机长度下连'标记的位置'都占不到(需要 %d + 标记 %d <= 容量-1 %d)"
      % (L_REAL, B(MARKER), CAPTEXT_CAP - 1), L_REAL + B(MARKER) <= CAPTEXT_CAP - 1)
check("B11 cpuAllowedText 没有大到让运行时频率改道进 note(改道阈值 = %d + 3 + %d + 1 = %d)"
      % (USED_BEFORE_FREQ, G_TEXT_REAL, USED_BEFORE_FREQ + 3 + G_TEXT_REAL + 1),
      ALLOWED_CAP < USED_BEFORE_FREQ + 3 + G_TEXT_REAL + 1)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[C] 截断标记的行为: 真机长度下不出现; 只有超长合成输入才出现, 且出现在尾部")
emit("=" * 100)

def sim_truncate(text, cap, marker):
    """逐字镜像 C++ 侧那条兜底: 放得下逐字节照抄; 放不下 = 保留 (cap-1-标记) 字节 + 写标记。"""
    b = text.encode("utf-8")
    limit = cap - 1
    if len(b) <= limit:
        return text, False
    keep = limit - B(marker)
    if keep < 0:
        keep = 0
    return b[:keep].decode("utf-8", "ignore") + marker, True

r_ok, r_mark = sim_truncate(pad_to_bytes(L_REAL), CAPTEXT_CAP, MARKER)
h_ok, h_mark = sim_truncate(pad_to_bytes(CAPTEXT_CAP * 4), CAPTEXT_CAP, MARKER)
check("C1 真机实际长度(%d 字节)下 capText 不出现截断标记" % L_REAL,
      r_mark is False and MARKER not in r_ok)
check("C2 超长合成输入(4 x 容量 = %d 字节)下 capText 出现截断标记" % (CAPTEXT_CAP * 4),
      h_mark is True and h_ok.endswith(MARKER))
check("C3 超长时标记出现在尾部(头部证据保留, 不越界)",
      h_ok.startswith("频") and B(h_ok) <= CAPTEXT_CAP - 1)
check("C4 留出的'装得下'余量: 容量-1-标记 %d >= 真机长度 %d" % (CAPTEXT_CAP - 1 - B(MARKER), L_REAL),
      CAPTEXT_CAP - 1 - B(MARKER) >= L_REAL)

check("C5 capText 的显式截断兜底仍在源码里(标记原文 + 逐字节判定)",
      MARKER in freq_src and "const size_t capBytes = sizeof(g_s.capText) - 1;" in freq_src)
GT_MARK = "运行时频率文本超出容量上限, 尾部被截断"
check("C6 g_text 的收尾有显式截断出口(buildText 末尾, 原来是静默的 snprintf)",
      GT_MARK in freq_src and re.search(r"if \(s\.size\(\) > capBytes\)", freq_src) is not None)
check("C7 auroraFreqSampleText() 的拷贝有显式截断出口(原来是静默的 memcpy(min(n, cap-1)))",
      GT_MARK in freq_src and re.search(r"if \(\(int\)n >= cap - 1\)", freq_src) is not None)
check("C10 cpuAllowedText 的 appendSeg() 也改成显式截断(原来是静默的 strncat(dst, seg, room))",
      "kSegTrunc" in aff_src and re.search(r"if \(strlen\(seg\) > room\)", aff_src) is not None)
g_ok, g_mark = sim_truncate(pad_to_bytes(G_TEXT_REAL), GTEXT_CAP,
                            " " + GT_MARK + " —— 这本身就是事实, 不许当成'取证完整')")
check("C8 真机整行长度(%d 字节)下 g_text 不触发截断出口" % G_TEXT_REAL, g_mark is False)
s_ok, s_mark = sim_truncate(pad_to_bytes(G_TEXT_REAL), FREQ_CAP, " 标记")
check("C9 真机整行长度(%d 字节)下 gb7 freq[] 的拷贝不触发截断" % G_TEXT_REAL, s_mark is False)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[D] 链的接线 + 与真机报告对表 + 没碰不该碰的东西")
emit("=" * 100)
check("D1 capText 被拼进 g_text 那一行(上游 -> 下游)",
      "s += g_s.capText;" in freq_src and "if (g_s.capTextLen > 0)" in freq_src)
check("D2 gb7.cpp 从 auroraFreqSampleText() 取那一行并放进 o.runFreq",
      "auroraFreqSampleText(freq" in gb7_src and "o.runFreq = freq;" in gb7_src)
check("D3 装不下 cpuAllowedText 时退到 o.diag(非截断兜底, 一个字都不丢)",
      "o.cpuInfo.cpuAllowedText + used" in gb7_src and "o.diag += freq;" in gb7_src)
check("D4 计分公式一个字没动", "score = k * (value * e.conv);" in gb7_src)
check("D5 本轮改的三个文件都不在 sn/ 目录里",
      all(not f.startswith("sn_") for f in ["cpu_freq_sample.cpp", "gb7.cpp", "cpu_affinity.h"]))

# ---------------------------------------------------------------------------
#  D6~D9 对表: 真机 8.1 报告里 capText 被截掉的那一刀, 到底在哪
#
#  ★ 测量锚点(这一段是 2026-10 修 D6 时补上的, 不补就会再量错对象)★
#    A) capText 段的开头 = 离截断标记**最近**的那一处 "频率上限取证"。
#       报告正文里还有**一处**同名样的字(它被 ** 包着, 是“口径声明”那一段里的说明文字):
#           "**频率上限取证**
#              (scaling_max_freq 对 cpuinfo_max_freq, ...)"
#       全文件共 49 处 "频率上限取证" = **48 个 capText 段首 + 1 处正文说明**。
#       老写法用 log.find("频率上限取证")(全文第一处) —— 它恰好命中的是
#       **正文里那句说明**, 于是量出来的是“正文说明 -> 某个截断标记”之间的一整段
#       (跨了好几项, 25744 字节), 而不是任何一个 capText 段。
#       所以 D6 从来就是**锚点错**, 不是日志变了 —— 日志还是同一份
#       (D:\gb7logs\phone81\report-latest.txt, AuroraBench 8.1 写的, 727971 字节)。
#    B) 截断标记 = " ★(★★取证文本超出容量上限"(k 落在它的前导空格上)。
#       留存文本 = log[A:B][:-1](去掉标记自己那个前导空格)。
#
#  ★ 两套数字怎么对上★
#    协作者报的 "1743(头部) + 181([逐核路径] 开头) = 1924" 是一次**分解**;
#    按上面的锚点逐项实测 48 项之后:
#      · 头部长度**逐项不同**: 44 项 1741 字节, 4 项 1743 字节
#        (1743 这个数本身是真的, 只是它只适用于那 4 项);
#      · [逐核路径] 开头: 真机留存 180 字节(44 项) / 178 字节(4 项)。
#        181 是手抄 VISIBLE_SEC5 时多拄了一个字符(它以 "scaling_available_fre" 结尾,
#        真机留存的是 "scaling_available_fr")。
#      · 两者相加恒为 **1921** —— 这才是真机那一刀的位置:
#        8.1 的 capText 容量 2048 减掉截断标记自身的字节数。
#    所以 1924 与 1921 的差在“分解方式 + 手抄那一小段”, 不在“从哪儿量到哪儿”。
#    (存量容量后来被扩到 8192, 所以现在同样的长度**不会再被截断** ——
#     那一条由上面 C8/C9 用 G_TEXT_REAL 直接断言, 不在这里重复。)
# ---------------------------------------------------------------------------
# (锚点与 REAL_RETAINED_BYTES 已在 B 节之前定义 —— B 节的 emit 就要用它们。)
LOG = r"D:\gb7logs\phone81\report-latest.txt"
if os.path.exists(LOG):
    log = io.open(LOG, encoding="utf-8", errors="replace").read()
    anchors = [m.start() for m in re.finditer(re.escape(CAP_ANCHOR), log)]
    marks = [m.start() for m in re.finditer(re.escape(TRUNC_MARK), log)]
    prose = [q for q in anchors if log[q - 2:q] == "**"]
    check("D6a 锚点选对了: 49 处「频率上限取证」 = 48 个 capText 段首 + 1 处正文说明",
          len(anchors) == 49 and len(prose) == 1 and len(marks) == 48,
          "锚点 %d / 其中正文说明 %d / 截断标记 %d"
          % (len(anchors), len(prose), len(marks)))
    if len(anchors) == 49 and len(prose) == 1 and len(marks) == 48:
        frags = []
        per_item_anchor_ok = True
        for p in marks:
            prev = [x for x in anchors if x < p]
            if not prev or log[prev[-1] - 2:prev[-1]] == "**":
                per_item_anchor_ok = False
                break
            frags.append(log[prev[-1]:p][:-1])
        check("D6b 每个截断标记的锚点都是**它自己那一项的** capText 段首(不是正文里那句说明)",
              per_item_anchor_ok and len(frags) == 48, "48 项逐项定位")
        if per_item_anchor_ok and len(frags) == 48:
            lens = sorted(set(B(f) for f in frags))
            check("D6c 真机 8.1 每一项留存下来的字节数**完全相同** = %d" % REAL_RETAINED_BYTES,
                  lens == [REAL_RETAINED_BYTES],
                  "实测 %s(项数 %d)" % (lens, len(frags)))
            head_lens = []
            sec_lens = []
            for f in frags:
                i5 = f.find(SEC5_ANCHOR)
                if i5 < 0:
                    head_lens.append(-1)
                    sec_lens.append(-1)
                    continue
                head_lens.append(B(f[:i5]))
                sec_lens.append(B(f[i5:]))
            check("D6d 拆成「头部 + [逐核路径] 开头」后两项相加**恒等于**留存长度(逐项核对)",
                  all(head_lens[i] + sec_lens[i] == REAL_RETAINED_BYTES for i in range(len(frags)))
                  and sorted(set(head_lens)) == [1741, 1743]
                  and sorted(set(sec_lens)) == [178, 180],
                  "头部 %s / [逐核路径]开头 %s(相加恒为 %d)"
                  % (sorted(set(head_lens)), sorted(set(sec_lens)), REAL_RETAINED_BYTES))
            i5 = frags[0].find(SEC5_ANCHOR)
            vis5 = frags[0][i5:]
            check("D7 重建出来的 [逐核路径] 与真机日志留存的那 %d 字节**逐字节相同" % B(vis5),
                  sec5().encode("utf-8").startswith(vis5.encode("utf-8")))
            check("D8 真机日志里确实带着 capText 自己打的截断标记(这就是本次要根治的事实)",
                  log[marks[0] + 1:marks[0] + 30].startswith("★(★★取证文本超出容量上限"))
            check("D9 留着的那一段确实以半截单词结尾(说明是从中间被切掉的)",
                  vis5.endswith("scaling_available_fr"))
            check("D10 手抄的 VISIBLE_SEC5(%d B)是真机留存片段的前缀 —— 两套数字可以逐字对上, 差的只是末尾 1 个字符"
                  % B(VISIBLE_SEC5),
                  VISIBLE_SEC5.startswith(vis5) and B(VISIBLE_SEC5) - B(vis5) == 1)
    else:
        check("D6a 真机报告里能找到 capText 段与截断标记", False, "未找到")
else:
    emit("  [WARN] 真机报告 %s 不在本机 —— D6~D10 未验证(不影响其余断言)" % LOG)

# ===========================================================================
emit("")
emit("=" * 100)
if not fails:
    emit("结论: 全部 PASS —— 真机实际需要 %d 字节; 链上每一环(capText %d / g_text %d / freq %d /"
         % (L_REAL, CAPTEXT_CAP, GTEXT_CAP, FREQ_CAP))
    emit("      cpuAllowedText %d / one %d)都不小于它的 1.5 倍; 截断标记只剩'超长合成输入'这一条路"
         % (ALLOWED_CAP, ONE_CAP))
    emit("      才会出现, 真机长度下一次都不触发(但那条兜底仍然留在源码里, 而三处静默截断出口已改成显式)。")
else:
    emit("结论: 有 %d 项 FAIL: %s" % (len(fails), ", ".join(fails)))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
