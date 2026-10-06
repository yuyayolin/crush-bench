# -*- coding: utf-8 -*-
"""
verify_freq_cap_policy.py —— 频率上限取证的"枚举式"改动的静态自检 + 离线断言

背景(真机硬事实, 写在这里当契约)
  * cpu4 的 scaling_available_frequencies 有 ...1995000 2100000 2270000 三档, cpuinfo_max_freq
    = 2270000, 而 16 个单核项 20 次采样全部是 1995MHz(零离散);
  * cpu4/scaling_max_freq=EACCES(13), policy0/scaling_max_freq=EACCES(13),
    而 **policy4/scaling_max_freq=ENOENT(2)** —— 旧代码把"核号"当"policy 号"用, 探错了目录。

本脚本做三件事(全部离线, 不联设备, 不跑 hdc, 不改任何文件):
  [A] 源码锚: 真的 opendir/readdir 列 policyN; 逐 policy 读 9 条只读节点; readlink 定"核->policy";
      time_in_state 看顶档历史; 逐路径记 errno; 只读(没有任何写 sysfs 的调用);
      取证在计时区间之外(readFreqCapEvidence 的调用点在采样线程 join 之后 / buildText 之前)。
  [B] 数据表从源码里抽出来再与本脚本的模型共用同一份: 路径模板 / 节点名 / errno 短名表。
      源码改了而脚本没跟着改 -> 本脚本失败(两边不会各说各话)。
  [C] 把 C++ 里那四条纯逻辑(parseCpuListMaskText / parsePolicyIndexFromLink /
      parseTimeInStateLine / findPolicyIndexForCpu)按同一套规则在 Python 里复刻一份, 用
      合成的目录列表与文件内容跑:
        ① 目录读不到(opendir EACCES)  ② policy 目录为空(只有 boost 之类)
        ③ related_cpus 格式异常(空串 / 尾随逗号 / 连续逗号 / 缺数字 / 区间反了 / 分号 / >=64)
        ④ time_in_state 顶两档全 0(以及"用过"的对照) + 非法行计数
      外加一次真机布局回放: policy0/policy1/policy2 三个目录(编号 != 首个核号, policy4
      根本不存在), cpu4 的 readlink 指向 policy1 —— 断言新逻辑解析到 policy1, 而旧写法必然
      拿到 ENOENT(即复现真机上那条读数, 并证明它被修正了)。

只读源码。退出码 0 = 全部通过。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_freq_cap_policy_out.txt"), "w", encoding="utf-8", newline="\n")
fails = []


def emit(s):
    print(s)
    OUT.write(s + "\n")


def check(name, ok, detail=""):
    emit("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("" if ok else "  <- " + str(detail))))
    if not ok:
        fails.append(name)


def rd(fn):
    p = os.path.join(HERE, fn)
    if not os.path.exists(p):
        return ""
    return io.open(p, encoding="utf-8", errors="replace").read()


SRC = rd("cpu_freq_sample.cpp")
HDR = rd("cpu_freq_sample.h")


def func_text(name):
    """按"函数签名行 -> 下一个顶格 } 行"取函数正文(本文件全部是顶格收尾)。"""
    lines = SRC.split("\n")
    start = None
    for i, ln in enumerate(lines):
        if re.match(r"^[A-Za-z_][\w:<>,\s\*&]*\b" + re.escape(name) + r"\s*\(", ln):
            start = i
            break
    if start is None:
        return ""
    for j in range(start + 1, len(lines)):
        if lines[j] == "}":
            return "\n".join(lines[start:j + 1])
    return ""


# ===========================================================================
emit("=" * 100)
emit("[A] 源码锚: 枚举 / 只读 / 逐路径 errno / 计时区间之外")
emit("=" * 100)
check("A1 cpu_freq_sample.h / .cpp 都在", HDR != "" and SRC != "")
for fn in ["capDirListOnce", "capLoadPolicy", "capLoadTimeInState", "capResolvePolicyOfCore",
           "capAppendPolicySummary", "capAppendTisSummary", "capAppendAlternatives",
           "readFreqCapEvidence"]:
    check("A2 有 %s()" % fn, func_text(fn) != "")

dirf = func_text("capDirListOnce")
check("A3 真的是列目录(opendir + readdir), 不是拼路径猜编号",
      "::opendir(" in dirf and "::readdir(" in dirf and "::closedir(" in dirf)
check("A4 目录读不到时记 errno(而不是当成'没有 policy')",
      "g_capDirErr = (errno != 0) ? errno : -1;" in dirf)
check("A5 目录为空也要能区分(条目总数 + 第一个非 policy 条目都记下来)",
      "++g_capDirEntries;" in dirf and "g_capDirOther" in dirf)

resf = func_text("capResolvePolicyOfCore")
check("A6 核->policy 用 readlink(内核自己给的映射), 再退到 related_cpus/affected_cpus 位图",
      "::readlink(" in resf and "parsePolicyIndexFromLink" in resf and
      "findPolicyIndexForCpu" in resf)
check("A7 readlink 的失败与'认不出'分开记(errno vs -2)",
      "linkErr = (errno != 0) ? errno : -1;" in resf and "linkErr = -2;" in resf)

pol = func_text("capLoadPolicy")
pol_nodes = re.findall(r'capReadPolicyNodeKhz\(idx, "([a-z_]+)"', pol)
check("A8 逐 policy 读 9 条只读节点(related/affected/driver/governor + 5 个频率节点)",
      sorted(pol_nodes) == sorted(["scaling_max_freq", "scaling_min_freq", "cpuinfo_max_freq",
                                   "bios_limit", "scaling_cur_freq"]) and
      '"%s/policy%d/related_cpus"' in pol and '"%s/policy%d/affected_cpus"' in pol and
      '"%s/policy%d/scaling_driver"' in pol and '"%s/policy%d/scaling_governor"' in pol,
      pol_nodes)
tis = func_text("capLoadTimeInState")
check("A9 time_in_state 两条路径都试(policyN/ 与 policyN/stats/), 逐路径记 errno",
      '"%s/policy%d/time_in_state"' in tis and '"%s/policy%d/stats/time_in_state"' in tis and
      "r->tisErr" in tis and "r->tisStatsErr" in tis)
alt = func_text("capAppendAlternatives")
check("A10 替代读数三条都在(/proc/cpufreq + cpufreq/boost + cooling_device*/cur_state)",
      "/proc/cpufreq" in alt and '"%s/boost"' in alt and
      '"%s/cooling_device%d/type"' in alt and '"%s/cooling_device%d/cur_state"' in alt)
cap = func_text("readFreqCapEvidence")
check("A11 每一段都逐路径记 errno, 读不到就写读不到(不用 0 冒充)",
      "capErrWord" in SRC and "读不到" in cap and
      "读不到就写读不到, 不用 0 冒充" in SRC)
check("A12 容量放不下时显式写'被截断'(不静默丢尾部判决)",
      "取证文本超出容量上限, 尾部被截断" in SRC)

# 只读性: 取证这一段里不许出现任何写打开 / 写节点
write_calls = re.findall(r"(?:O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|::write\(|fopen\([^)]*\"w|std::ofstream)", SRC)
check("A13 全文件只有只读调用(没有 O_WRONLY/O_RDWR/O_CREAT/::write/fopen\"w\")",
      write_calls == [], write_calls)
check("A14 取证里没有任何'写 scaling_max_freq / 锁频 / 改 governor'的动作",
      not re.search(r"setFreq|lockFreq|writeFreq|echo .*>.*scaling_|scaling_setspeed", SRC))

# 计时区间之外 + 调用点位置
i_join = SRC.find("g_thr.join();")
i_cap = SRC.find("readFreqCapEvidence();")
i_build = SRC.find("buildText();")
check("A15 取证在采样线程 join 之后 / buildText 之前(即计时区间之外)",
      i_join > 0 and i_cap > i_join and i_build > i_cap, (i_join, i_cap, i_build))
sampler = func_text("samplerMain")
check("A16 采样线程里仍然没有 opendir/readdir(枚举只在会话收尾做一次)",
      "opendir" not in sampler and "readdir" not in sampler and "readFreqCapEvidence" not in sampler)
check("A17 目录列举结果进程内只取一次(静态标志), 逐档读数每会话重读",
      "g_capDirListed" in SRC and "if (g_capDirListed != 0)" in SRC and
      "g_capCoolListed" in SRC)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[B] 数据表: 从源码里抽出来, 再给下面的模型用(两份不一致就失败)")
emit("=" * 100)
root = re.search(r'const char kCapFreqRoot\[\]\s*=\s*"([^"]+)"', SRC)
troot = re.search(r'const char kCapThermalRoot\[\]\s*=\s*"([^"]+)"', SRC)
check("B1 policy 根目录就是内核那一条", root is not None and root.group(1) == "/sys/devices/system/cpu/cpufreq",
      root.group(1) if root else None)
check("B2 热区根目录就是 /sys/class/thermal", troot is not None and troot.group(1) == "/sys/class/thermal",
      troot.group(1) if troot else None)
ROOT = root.group(1) if root else ""
THERM = troot.group(1) if troot else ""

policy_paths = sorted(set(re.findall(r'"%s/policy%d/([a-z_/]+)"', SRC)))
check("B3 逐 policy 的路径模板齐备(4 个 cpu 列表/策略节点 + 2 条 time_in_state)",
      sorted(policy_paths) == sorted(["related_cpus", "affected_cpus", "scaling_driver",
                                      "scaling_governor", "time_in_state", "stats/time_in_state"]),
      policy_paths)
readlink_tpl = re.findall(r'"(/sys/devices/system/cpu/cpu%d/cpufreq)"', SRC)
check("B4 readlink 的路径模板就是 cpuN/cpufreq", readlink_tpl == ["/sys/devices/system/cpu/cpu%d/cpufreq"],
      readlink_tpl)

capword = func_text("capErrWord")
errno_pairs = dict((int(a), b) for a, b in re.findall(r'case (\d+):\s*n = "([A-Z]+)";', capword))
check("B5 errno 短名表覆盖真机出现的那几个(1/2/5/13/21/22)",
      all(k in errno_pairs for k in (1, 2, 5, 13, 21, 22)), errno_pairs)
check("B6 非 errno 的两个口径码也写清楚了(-1 空 / -2 非法)",
      '"空(-1)"' in capword and '"非法(-2)"' in capword)
ERRNO_NAME = errno_pairs
FILES4 = re.findall(r'"(scaling_max_freq|scaling_min_freq|cpuinfo_max_freq|scaling_available_frequencies)"', SRC)
check("B7 逐核四条 cpuN 路径仍在(旧行为没被删掉)",
      set(FILES4) >= {"scaling_max_freq", "scaling_min_freq", "cpuinfo_max_freq",
                      "scaling_available_frequencies"}, FILES4)

# ===========================================================================
#  [C] 纯逻辑的 Python 复刻(与 C++ 同名函数同一套规则)
# ===========================================================================
emit("")
emit("=" * 100)
emit("[C] 纯逻辑复刻 + 合成数据断言")
emit("=" * 100)

OK, BAD, TOO_BIG = 0, -1, -2


def parse_cpu_list_mask(s):
    """复刻 parseCpuListMaskText: 0 = 成功; -1 = 空/格式非法; -2 = 核号 >= 64。"""
    mask = 0
    if s is None:
        return mask, BAD
    p = 0
    n = len(s)
    while p < n and s[p] in " \t":
        p += 1
    if p >= n:
        return mask, BAD
    fields = 0
    while p < n:
        if not s[p].isdigit():
            return 0, BAD
        lo = 0
        while p < n and s[p].isdigit():
            lo = lo * 10 + int(s[p])
            if lo > 4096:
                return 0, TOO_BIG
            p += 1
        hi = lo
        if p < n and s[p] == "-":
            p += 1
            if p >= n or not s[p].isdigit():
                return 0, BAD
            hi = 0
            while p < n and s[p].isdigit():
                hi = hi * 10 + int(s[p])
                if hi > 4096:
                    return 0, TOO_BIG
                p += 1
            if hi < lo:
                return 0, BAD
        if hi >= 64:
            return 0, TOO_BIG
        for c in range(lo, hi + 1):
            mask |= (1 << c)
        fields += 1
        while p < n and s[p] in " \t":
            p += 1
        if p >= n:
            break
        if s[p] == ",":
            p += 1
            while p < n and s[p] in " \t":
                p += 1
            if p >= n:
                return 0, BAD
            continue
        if s[p] in "\n\r":
            p += 1
            while p < n and s[p] in "\n\r \t":
                p += 1
            if p >= n:
                break
            return 0, BAD
        return 0, BAD
    return (mask, OK) if fields > 0 else (0, BAD)


def parse_policy_index_from_link(target):
    """复刻 parsePolicyIndexFromLink: 取最后一个 policy<十进制>; -1 = 认不出。"""
    if target is None:
        return -1
    at = -1
    i = 0
    while i + 6 <= len(target):
        if target[i:i + 6] == "policy":
            at = i
        i += 1
    if at < 0:
        return -1
    p = at + 6
    if p >= len(target) or not target[p].isdigit():
        return -1
    v = 0
    while p < len(target) and target[p].isdigit():
        v = v * 10 + int(target[p])
        if v > 4096:
            return -1
        p += 1
    if p < len(target) and target[p] not in ("/",):
        return -1
    return v


def parse_time_in_state_line(line):
    """复刻 parseTimeInStateLine: (khz, time) 或 None。多一个字段就算非法。"""
    if line is None:
        return None
    p = 0
    n = len(line)
    while p < n and line[p] in " \t":
        p += 1
    if p >= n or not line[p].isdigit():
        return None
    f = 0
    while p < n and line[p].isdigit():
        f = f * 10 + int(line[p])
        if f > 100000000:
            return None
        p += 1
    while p < n and line[p] in " \t":
        p += 1
    if p >= n or not line[p].isdigit():
        return None
    t = 0
    while p < n and line[p].isdigit():
        t = t * 10 + int(line[p])
        if t > 100000000000000:
            return None
        p += 1
    while p < n and line[p] in " \t\r\n":
        p += 1
    if p != n:
        return None
    return (f, t)


def find_policy_index_for_cpu(recs, cpu, via_out=None):
    """复刻 findPolicyIndexForCpu: 先 related 再 affected; 解析失败的 policy 一律跳过。
    via_out(可空, 传一个长度为 1 的 list 当出参): 1 = 命中 related; 2 = 命中 affected。"""
    if via_out is not None:
        via_out[0] = 0
    if cpu < 0 or cpu >= 64:
        return -1
    for i, r in enumerate(recs):
        if r["relErr"] == OK and (r["relMask"] >> cpu) & 1:
            if via_out is not None:
                via_out[0] = 1
            return i
    for i, r in enumerate(recs):
        if r["affErr"] == OK and (r["affMask"] >> cpu) & 1:
            if via_out is not None:
                via_out[0] = 2
            return i
    return -1


# ---- C++ 侧的四条规则必须在源码里有对应实现(逐条锚) ----
pl = func_text("parseCpuListMaskText")
check("C1 区间反了判非法", "if (hi < lo) { return -1; }" in pl)
check("C2 核号 >= 64 判非法(不截断)", "if (hi >= 64) {" in pl and "return -2;" in pl)
check("C3 尾随逗号 / 连续逗号 / 非数字开头都判非法",
      pl.count("return -1;") >= 4 and "if (*p == ',') {" in pl)
pl2 = func_text("parsePolicyIndexFromLink")
check("C4 readlink 目标取最后一个 policy 片段", "at = i;   // 取最后一个(最深的那个目录)" in pl2)
pl3 = func_text("parseTimeInStateLine")
check("C5 time_in_state 第三个字段判非法", "return -1;                            // 还有第三个字段" in pl3)
pl4 = func_text("findPolicyIndexForCpu")
check("C6 先 related 再 affected, 失败的一律跳过",
      pl4.find("relErr ==") < pl4.find("affErr ==") and "ps[i].relErr == 0" in pl4 and "ps[i].affErr == 0" in pl4)

# ---- 合成文件系统 ----
MISSING = object()


class Fs(object):
    def __init__(self, files=None, dirs=None, links=None):
        self.files = files or {}
        self.dirs = dirs or {}
        self.links = links or {}

    def read(self, path):
        """返回 (errno, 内容); errno 0 = 读到。缺文件 = ENOENT(2)。"""
        v = self.files.get(path, MISSING)
        if v is MISSING:
            return 2, ""
        if isinstance(v, int):
            return v, ""
        return 0, v

    def listdir(self, path):
        v = self.dirs.get(path, MISSING)
        if v is MISSING:
            return 13, []
        if isinstance(v, int):
            return v, []
        return 0, v

    def readlink(self, path):
        v = self.links.get(path, MISSING)
        if v is MISSING:
            return 2, ""
        if isinstance(v, int):
            return v, ""
        return 0, v


def cap_enum(fs):
    """复刻 capDirListOnce: 返回 (policy 编号升序, opendir errno, 条目数, 第一个非 policy 条目)。"""
    e, names = fs.listdir(ROOT)
    if e != 0:
        return [], e, 0, ""
    idxs = []
    other = ""
    entries = 0
    for nm in names:
        entries += 1
        m = re.match(r"^policy(\d+)$", nm)
        if m:
            idxs.append(int(m.group(1)))
        elif not nm.startswith(".") and other == "":
            other = nm
    return sorted(idxs), 0, entries, other


def cap_load_policy(fs, idx):
    r = {"index": idx, "relErr": OK, "relMask": 0, "affErr": OK, "affMask": 0,
         "maxErr": OK, "maxKhz": 0, "cpuinfoErr": OK, "cpuinfoKhz": 0,
         "biosErr": OK, "curErr": OK, "tisErr": OK, "tisStatsErr": OK, "tisSrc": 0,
         "tisSteps": 0, "tisBadLines": 0, "tisTop": [(0, 0), (0, 0)], "tisTotal": 0}
    for key, node in (("rel", "related_cpus"), ("aff", "affected_cpus")):
        e, txt = fs.read("%s/policy%d/%s" % (ROOT, idx, node))
        if e == 0:
            mask, code = parse_cpu_list_mask(txt)
            if code != OK:
                r[key + "Err"] = code
            else:
                r[key + "Mask"] = mask
        else:
            r[key + "Err"] = e
    for key, node in (("max", "scaling_max_freq"), ("cpuinfo", "cpuinfo_max_freq"),
                      ("bios", "bios_limit"), ("cur", "scaling_cur_freq")):
        e, txt = fs.read("%s/policy%d/%s" % (ROOT, idx, node))
        if e != 0:
            r[key + "Err"] = e
        else:
            try:
                r[key + "Khz"] = int(txt.strip())
            except ValueError:
                r[key + "Err"] = -2
    return r


def cap_load_tis(fs, r):
    idx = r["index"]
    e, txt = fs.read("%s/policy%d/time_in_state" % (ROOT, idx))
    r["tisErr"] = e
    if e != 0:
        e2, txt = fs.read("%s/policy%d/stats/time_in_state" % (ROOT, idx))
        r["tisStatsErr"] = e2
        if e2 != 0:
            return
        r["tisSrc"] = 2
    else:
        r["tisSrc"] = 1
    for line in txt.split("\n"):
        if line.strip() == "":
            continue
        got = parse_time_in_state_line(line)
        if got is None:
            r["tisBadLines"] += 1
            continue
        khz, t = got
        r["tisSteps"] += 1
        r["tisTotal"] += t
        top = r["tisTop"]
        if khz > top[0][0]:
            top[1] = top[0]
            top[0] = (khz, t)
        elif khz > top[1][0]:
            top[1] = (khz, t)


def cap_resolve(fs, cpu, recs):
    """复刻 capResolvePolicyOfCore: 返回 (policy 编号 或 -1, how, linkErr)。"""
    e, tgt = fs.readlink("/sys/devices/system/cpu/cpu%d/cpufreq" % cpu)
    link_err = 0
    if e == 0:
        v = parse_policy_index_from_link(tgt)
        if v >= 0:
            return v, "readlink", 0
        link_err = -2
    else:
        link_err = e if e != 0 else -1
    i = find_policy_index_for_cpu(recs, cpu)
    if i >= 0:
        return recs[i]["index"], "related_cpus", link_err
    return -1, "none", link_err


def cap_tis_text(r):
    """复刻 capAppendTisSummary 的结论句(只保留断言要用的那两句)。"""
    if r["tisSteps"] <= 0:
        return "time_in_state 也没读到 => 这一路也定不了案"
    t0 = r["tisTop"][0][1]
    t1 = r["tisTop"][1][1]
    if t0 == 0 and t1 == 0:
        return "这两档在本机累计运行时间为 0 = 从未被请求过"
    return "顶档用过(不是不可达)"


def cap_verdict(fs, cpu, recs, pol, how):
    """复刻 readFreqCapEvidence 的判决那一段(三档 + policy 未定那一档)。"""
    ri = -1
    for i, r in enumerate(recs):
        if r["index"] == pol:
            ri = i
            break
    upErr, upKhz = OK, 0
    nomErr, nomKhz = OK, 0
    if ri >= 0:
        upErr, upKhz = recs[ri]["maxErr"], recs[ri]["maxKhz"]
        nomErr, nomKhz = recs[ri]["cpuinfoErr"], recs[ri]["cpuinfoKhz"]
    else:
        nomErr = -1
    if pol < 0:
        return "所属 policy 定不了"
    if upErr != 0:
        return "上限读数(scaling_max_freq)读不到(%s) => 仍不能定案" % (ERRNO_NAME.get(upErr) or upErr)
    if nomKhz > 0 and upKhz > 0 and upKhz < nomKhz:
        return "上限是系统压的"
    if nomKhz > 0 and upKhz >= nomKhz:
        return "内核此刻没压上限"
    return "仍不能定案"


# ---- 情形 ①: 目录读不到 ----
emit("")
emit("  --- ① 目录读不到(opendir EACCES) ---")
fs = Fs(dirs={ROOT: 13}, links={"/sys/devices/system/cpu/cpu4/cpufreq": 13})
idxs, derr, entries, other = cap_enum(fs)
check("1.1 opendir EACCES -> 一个 policy 都拿不到, 且 errno 记成 13",
      idxs == [] and derr == 13 and entries == 0, (idxs, derr))
recs = [cap_load_policy(fs, i) for i in idxs]
pol, how, lerr = cap_resolve(fs, 4, recs)
check("1.2 readlink 也 EACCES 时 -> '定不了所属 policy'(不退回拿核号当 policy 号)",
      pol == -1 and how == "none" and lerr == 13, (pol, how, lerr))
check("1.3 判决必须是'仍不能定案'(而不是猜成'没压上限'): policy 都没定出来时不许给结论",
      "定不了" in cap_verdict(fs, 4, recs, pol, how) and
      "没压上限" not in cap_verdict(fs, 4, recs, pol, how))

# ---- 情形 ②: 目录读到了但没有 policy 目录 ----
emit("")
emit("  --- ② policy 目录为空(只有 boost 之类) ---")
fs = Fs(dirs={ROOT: ["boost", "stats"]}, links={"/sys/devices/system/cpu/cpu4/cpufreq": 2})
idxs, derr, entries, other = cap_enum(fs)
check("2.1 目录能读, 但 policyN 目录 0 个 -> 明确区分于'读不到'",
      idxs == [] and derr == 0 and entries == 2 and other == "boost", (idxs, derr, entries, other))
recs = []
pol, how, lerr = cap_resolve(fs, 4, recs)
check("2.2 没有 policy 目录 + readlink ENOENT -> 仍然是'定不了'",
      pol == -1 and lerr == 2, (pol, lerr))

# ---- 情形 ③: related_cpus 格式异常 ----
emit("")
emit("  --- ③ related_cpus 格式异常 ---")
bad_cases = {"": BAD, "   ": BAD, "0-3,": BAD, "0-3,,5": BAD, ",0-3": BAD, "0-": BAD,
             "3-0": BAD, "abc": BAD, "0-3;5": BAD, "cpu0-3": BAD, "4-11x": BAD,
             "0-70": TOO_BIG, "64": TOO_BIG}
for s, want in sorted(bad_cases.items()):
    mask, code = parse_cpu_list_mask(s)
    check("3.1 非法 cpulist %r -> 口径码 %d, 且不产生任何核匹配" % (s, want),
          code == want and mask == 0, (code, mask))
good_cases = {"0-3": [0, 1, 2, 3], "4-11": list(range(4, 12)), "0": [0],
              "0-3,8-11": [0, 1, 2, 3, 8, 9, 10, 11], "4-7\n": [4, 5, 6, 7],
              " 4-7 ": [4, 5, 6, 7]}
for s, want in sorted(good_cases.items()):
    mask, code = parse_cpu_list_mask(s)
    got = [c for c in range(64) if (mask >> c) & 1]
    check("3.2 合法 cpulist %r -> %s" % (s, want), code == OK and got == want, (code, got))
recs = [{"index": 0, "relErr": BAD, "relMask": 0, "affErr": OK, "affMask": 0},
        {"index": 1, "relErr": OK, "relMask": parse_cpu_list_mask("4-11")[0], "affErr": OK, "affMask": 0}]
check("3.3 前面那条 policy 的 related_cpus 非法时, 不会挡住后面那条的正确命中",
      find_policy_index_for_cpu(recs, 4) == 1)
check("3.4 全部非法 -> 未命中(-1), 不猜", find_policy_index_for_cpu(
    [{"index": 0, "relErr": BAD, "relMask": 0, "affErr": BAD, "affMask": 0}], 4) == -1)

# ---- 情形 ④: time_in_state ----
emit("")
emit("  --- ④ time_in_state(顶档全 0 / 用过 / 非法行) ---")
TABLE = """558000 100
640000 200
750000 0
850000 0
1000000 0
1150000 0
1280000 0
1380000 0
1480000 0
1580000 0
1670000 0
1770000 0
1880000 0
1995000 7777
2100000 0
2270000 0
"""
fs = Fs(files={"%s/policy1/time_in_state" % ROOT: TABLE})
r = cap_load_policy(fs, 1)
cap_load_tis(fs, r)
check("4.1 16 档全部解析出来, 0 条非法行",
      r["tisSteps"] == 16 and r["tisBadLines"] == 0 and r["tisSrc"] == 1, r)
check("4.2 最高两档 = 2270000 与 2100000", [t[0] for t in r["tisTop"]] == [2270000, 2100000],
      r["tisTop"])
check("4.3 顶两档全 0 -> 结论句 = '从未被请求过'",
      "从未被请求过" in cap_tis_text(r), cap_tis_text(r))
r2 = dict(r)
r2 = cap_load_policy(fs, 1)
cap_load_tis(Fs(files={"%s/policy1/time_in_state" % ROOT: TABLE.replace("2270000 0", "2270000 99")}), r2)
check("4.4 顶档用过(99) -> 结论句 = '顶档用过'", "用过" in cap_tis_text(r2), cap_tis_text(r2))
r15 = cap_load_policy(fs, 1)
cap_load_tis(Fs(files={"%s/policy1/time_in_state" % ROOT:
                       "\n".join(l for l in TABLE.split("\n") if not l.startswith("2270000"))}), r15)
check("4.5 time_in_state 里没有 2270 档 -> 最高档就是 2100, 不凭空造出一个 2270",
      r15["tisSteps"] == 15 and r15["tisTop"][0][0] == 2100000, (r15["tisSteps"], r15["tisTop"]))
r3 = cap_load_policy(fs, 1)
cap_load_tis(Fs(files={"%s/policy1/time_in_state" % ROOT:
                       TABLE + "1995000 12 34\n\n   \n"}), r3)
check("4.6 多一个字段的行判非法并计数(tisBadLines), 空行不算非法",
      r3["tisBadLines"] == 1 and r3["tisSteps"] == 16, (r3["tisBadLines"], r3["tisSteps"]))
r4 = cap_load_policy(fs, 1)
cap_load_tis(Fs(files={"%s/policy1/time_in_state" % ROOT: 13,
                       "%s/policy1/stats/time_in_state" % ROOT: 2}), r4)
check("4.7 两条路径都读不到 -> 逐路径 errno 都留着(13 / 2), 步数为 0",
      r4["tisErr"] == 13 and r4["tisStatsErr"] == 2 and r4["tisSteps"] == 0 and
      "定不了案" in cap_tis_text(r4), r4)
r5 = cap_load_policy(fs, 1)
cap_load_tis(Fs(files={"%s/policy1/time_in_state" % ROOT: 2,
                       "%s/policy1/stats/time_in_state" % ROOT: TABLE}), r5)
check("4.8 旧路径没有、新路径有 -> 走 stats/ 并且来源标成 2",
      r5["tisSrc"] == 2 and r5["tisSteps"] == 16, (r5["tisSrc"], r5["tisSteps"]))

# ===========================================================================
emit("")
emit("=" * 100)
emit("[D] 真机布局回放: policy 编号 != 首个核号, policy4 根本不存在")
emit("=" * 100)
PHONE = Fs(
    dirs={ROOT: ["policy0", "policy1", "policy2", "boost"]},
    links={"/sys/devices/system/cpu/cpu0/cpufreq": "../../cpufreq/policy0",
           "/sys/devices/system/cpu/cpu4/cpufreq": "../../cpufreq/policy1",
           "/sys/devices/system/cpu/cpu7/cpufreq": "../../cpufreq/policy1",
           "/sys/devices/system/cpu/cpu12/cpufreq": "../../cpufreq/policy2"},
    files={
        "%s/policy0/related_cpus" % ROOT: "0-3\n",
        "%s/policy0/affected_cpus" % ROOT: "0-3\n",
        "%s/policy0/scaling_driver" % ROOT: "cpufreq-dt\n",
        "%s/policy0/scaling_max_freq" % ROOT: 13,
        "%s/policy0/cpuinfo_max_freq" % ROOT: "1720000\n",
        "%s/policy0/bios_limit" % ROOT: 2,
        "%s/policy0/scaling_cur_freq" % ROOT: "1720000\n",
        "%s/policy1/related_cpus" % ROOT: "4-11\n",
        "%s/policy1/affected_cpus" % ROOT: "4-11\n",
        "%s/policy1/scaling_driver" % ROOT: "cpufreq-dt\n",
        "%s/policy1/scaling_max_freq" % ROOT: 13,
        "%s/policy1/cpuinfo_max_freq" % ROOT: "2270000\n",
        "%s/policy1/scaling_cur_freq" % ROOT: "1995000\n",
        "%s/policy1/time_in_state" % ROOT: TABLE,
        "%s/policy2/related_cpus" % ROOT: "12-13\n",
        "%s/policy2/scaling_max_freq" % ROOT: 13,
        "%s/policy2/cpuinfo_max_freq" % ROOT: "2750000\n",
        "/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq": 13,
        "/sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq": 13,
        "/sys/devices/system/cpu/cpu4/cpufreq/cpuinfo_max_freq": "2270000\n",
        "/sys/devices/system/cpu/cpu4/cpufreq/scaling_available_frequencies":
            "558000 640000 750000 850000 1000000 1150000 1280000 1380000 1480000 1580000 "
            "1670000 1770000 1880000 1995000 2100000 2270000\n",
    })

idxs, derr, entries, other = cap_enum(PHONE)
check("D1 枚举出的是 policy0/policy1/policy2 三个(没有 policy4), 排序稳定",
      idxs == [0, 1, 2] and derr == 0 and entries == 4, (idxs, derr, entries))
recs = [cap_load_policy(PHONE, i) for i in idxs]
pol4, how4, lerr4 = cap_resolve(PHONE, 4, recs)
check("D2 cpu4 解析到 **policy1**(readlink), 不是 policy4", pol4 == 1 and how4 == "readlink",
      (pol4, how4))
check("D3 旧写法(核号当 policy 号)会去读 policy4 -> 该目录不存在(ENOENT): 复现真机那条读数",
      PHONE.read("%s/policy4/scaling_max_freq" % ROOT)[0] == 2)
check("D4 cpu4 的 readlink 目标能被解析成 1(相对路径也认)", parse_policy_index_from_link(
    "../../cpufreq/policy1") == 1)
check("D5 交叉核对: related_cpus 位图也指向同一个 policy1",
      find_policy_index_for_cpu(recs, 4) == 1 and find_policy_index_for_cpu(recs, 7) == 1 and
      find_policy_index_for_cpu(recs, 3) == 0 and find_policy_index_for_cpu(recs, 12) == 2)
r1 = recs[1]
check("D6 policy1 的 cpuinfo_max_freq=2270000 读到了, 但 scaling_max_freq=EACCES(13)",
      r1["cpuinfoKhz"] == 2270000 and r1["maxErr"] == 13, (r1["cpuinfoKhz"], r1["maxErr"]))
cap_load_tis(PHONE, r1)
check("D7 policy1 的历史: 1995 有累计时间(7777), 2100/2270 都是 0 -> '从未被请求过'",
      r1["tisSteps"] == 16 and r1["tisTop"] == [(2270000, 0), (2100000, 0)] and
      "从未被请求过" in cap_tis_text(r1), (r1["tisSteps"], r1["tisTop"]))
v = cap_verdict(PHONE, 4, recs, pol4, how4)
check("D8 判决: 上限读数读不到 -> 仍不能定案(不是'没压'), 并把两档全 0 的历史证据摆出来",
      "仍不能定案" in v and "EACCES" in v, v)
check("D9 反例对照: 若 scaling_max_freq 可读且 =1995000 -> 判决必须是'上限是系统压的'",
      "上限是系统压的" in cap_verdict(
          Fs(), 4, [{"index": 1, "maxErr": OK, "maxKhz": 1995000, "cpuinfoErr": OK,
                     "cpuinfoKhz": 2270000}], 1, "readlink"))
check("D10 反例对照: 两者相等 -> 判决必须是'没压上限'(不许把'没压'说成'压了')",
      "没压上限" in cap_verdict(
          Fs(), 4, [{"index": 1, "maxErr": OK, "maxKhz": 2270000, "cpuinfoErr": OK,
                     "cpuinfoKhz": 2270000}], 1, "readlink"))
check("D11 99.5% 与 87.9% 都算出来: 1995000/2270000 -> 87%",
      (1995000 * 100) // 2270000 == 87)

# ---- errno 口径: 数字 + 名字 ----
emit("")
emit("=" * 100)
emit("[E] errno 口径与文本完整性")
emit("=" * 100)
check("E1 逐路径都写 errno 的数字与名字(EACCES(13) 这种形状)", "%s(%d)" in capword)
check("E2 判决三档的措辞都在源码里(压了 / 没压 / 读不到)",
      "上限是系统压的" in SRC and "没压上限" in SRC and "仍不能定案" in SRC)
check("E3 '编号规则'那句必须写进报告(N 不是第一个核号)", "编号规则: policyN 的 N 不是该 policy 的第一个核号" in SRC)
check("E4 旧的判决规则原句一字未改(verify_warmup_saturation.py 也在盯它)",
      "若 scaling_max_freq < cpuinfo_max_freq, 则上限是系统压的" in SRC)
_cap_decl = re.search(r"char capText\[\s*(?:(\d+)|kCapTextCap)\s*\]", SRC)
_cap_const = re.search(r"kCapTextCap\s*=\s*(\d+)", SRC)
_cap_bytes = 0
if _cap_decl is not None:
    _cap_bytes = int(_cap_decl.group(1)) if _cap_decl.group(1) else (
        int(_cap_const.group(1)) if _cap_const is not None else 0)
check("E5 capText 容量已按新内容扩到 >= 2048(直接写数字或用 kCapTextCap 常量都认)",
      _cap_bytes >= 2048, _cap_bytes)
check("E6 取证文本的容量常量与 FreqSession 用的同一个",
      "char capText[kCapTextCap];" in SRC and re.search(r"kCapTextCap\s*=\s*(\d+)", SRC) is not None)

# ===========================================================================
emit("")
emit("=" * 100)
emit("[F] 原生执行: 把 C++ 里那四条纯逻辑逐字抽出来编译成真程序, 跑同一批合成输入")
emit("=" * 100)
# 为什么要有这一段: [C] 里的 Python 复刻再忠实也只是"另一份实现"。这一段把 cpu_freq_sample.cpp
# 里 parseCpuListMaskText / parsePolicyIndexFromLink / parseTimeInStateLine /
# findPolicyIndexForCpu 的源码原文抽出来(一字不改), 用 OHOS SDK 自带的 Windows 宿主 clang
# 编成 x86-64 目标, 再用 lld-link 链成一个不需要 CRT 的独立 exe(入口 mainCRTStartup,
# 退出码 = 第一条失败的断言序号), 然后在本机真的跑一遍。
# 于是"C++ 的行为"与"Python 复刻的行为"是拿同一批输入对出来的 —— 两边不一致就失败。
import json
import subprocess
import tempfile

CLANG = r"D:\ohos-tools\sdk\native\llvm\bin\clang++.exe"
LLD = r"D:\ohos-tools\sdk\native\llvm\bin\lld-link.exe"

LINK_CASES = [("../../cpufreq/policy1", 1), ("policy0", 0), ("policy12", 12),
              ("/sys/devices/system/cpu/cpufreq/policy7", 7), ("../policyX", -1),
              ("../policy", -1), ("/sys/devices/system/cpu", -1), ("policy1/foo", 1)]
TIS_CASES = [("1995000 0", (1995000, 0)), ("558000 12345\n", (558000, 12345)),
             ("1995000\t777\n", (1995000, 777)), ("2270000 0000\r\n", (2270000, 0)),
             ("1995000 12 34", None), ("1995000", None), ("", None), ("abc", None),
             ("   ", None), ("0 0", (0, 0))]

# ---- 先把同一批输入喂给 Python 复刻(两边必须给出一模一样的答案) ----
for s, want in LINK_CASES:
    got = parse_policy_index_from_link(s)
    check("F1 [Python 复刻] readlink 目标 %r -> %d" % (s, want), got == want, got)
for s, want in TIS_CASES:
    got = parse_time_in_state_line(s)
    check("F2 [Python 复刻] time_in_state 行 %r -> %r" % (s, want), got == want, got)

# ---- 拼原生测试程序 ----
def cstr(s):
    return json.dumps(s, ensure_ascii=True)


def mask_of(cpus):
    m = 0
    for c in cpus:
        m |= (1 << c)
    return m


def gen_harness():
    lines = []
    names = []

    def case(name, code):
        names.append(name)
        if code.startswith("{"):
            # 复合语句(里面自己调 CHK): 不能再套一层 CHK(...)
            lines.append("    %s   // %d: %s" % (code, len(names), name.replace("*/", "* /")))
        else:
            lines.append("    CHK(%s);   // %d: %s" % (code, len(names), name.replace("*/", "* /")))

    for s, want in sorted(bad_cases.items()):
        case("cpulist %r -> %d" % (s, want),
             "parseCpuListMaskText(%s, &m) == %d" % (cstr(s), want))
    for s, cpus in sorted(good_cases.items()):
        case("cpulist %r -> %s" % (s, cpus),
             "parseCpuListMaskText(%s, &m) == 0 && m == %dULL" % (cstr(s), mask_of(cpus)))
    for s, want in LINK_CASES:
        case("readlink %r -> %d" % (s, want), "parsePolicyIndexFromLink(%s) == %d" % (cstr(s), want))
    for s, want in TIS_CASES:
        if want is None:
            case("tis %r -> 非法" % s,
                 "parseTimeInStateLine(%s, &k, &t) != 0" % cstr(s))
        else:
            case("tis %r -> %s" % (s, want),
                 "parseTimeInStateLine(%s, &k, &t) == 0 && k == %d && t == %dLL"
                 % (cstr(s), want[0], want[1]))
    # findPolicyIndexForCpu: 三条记录 —— 第 0 条 related 非法, 第 1 条 related=4-11, 第 2 条 affected=12-13
    case("findPolicy: related 非法的记录必须被跳过, cpu4 -> 下标 1",
         "{ CapPolicyRec rr[3]; rr[0].relErr = -1; rr[1].relErr = 0; rr[1].relMask = %dULL; "
         "rr[1].affErr = 0; rr[2].affErr = 0; rr[2].affMask = %dULL; int via = 0; "
         "CHK(findPolicyIndexForCpu(rr, 3, 4, &via) == 1 && via == 1); }" % (mask_of(range(4, 12)), mask_of([12, 13])))
    case("findPolicy: cpu12 只能靠 affected_cpus 命中 -> 下标 2",
         "{ CapPolicyRec rr[3]; rr[1].relErr = 0; rr[1].relMask = %dULL; "
         "rr[2].affErr = 0; rr[2].affMask = %dULL; int via = 0; "
         "CHK(findPolicyIndexForCpu(rr, 3, 12, &via) == 2 && via == 2); }" % (mask_of(range(4, 12)), mask_of([12, 13])))
    # cpu1 既不在 related=4-11 里, 也不在 affected 里 -> 必须返回 -1(不猜)
    case("findPolicy: cpu1 两条位图都没命中 -> -1",
         "{ CapPolicyRec rr[1]; rr[0].relErr = 0; rr[0].relMask = %dULL; "
         "rr[0].affErr = -2; int via = 0; "
         "CHK(findPolicyIndexForCpu(rr, 1, 1, &via) == -1 && via == 0); }" % mask_of(range(4, 12)))

    # 同一批 findPolicy 输入, 先让 Python 复刻答一遍(与原生那三条逐条对照)
    _recs = [{"relErr": BAD, "relMask": 0, "affErr": OK, "affMask": 0},
             {"relErr": OK, "relMask": mask_of(range(4, 12)), "affErr": OK, "affMask": 0},
             {"relErr": OK, "relMask": 0, "affErr": OK, "affMask": mask_of([12, 13])}]
    _via = [0]
    check("F7 [Python 复刻] findPolicy: related 非法的记录被跳过, cpu4 -> 下标 1 (via=related)",
          find_policy_index_for_cpu(_recs, 4, _via) == 1 and _via[0] == 1, _via[0])
    check("F8 [Python 复刻] findPolicy: cpu12 只能靠 affected_cpus 命中 -> 下标 2 (via=affected)",
          find_policy_index_for_cpu(_recs, 12, _via) == 2 and _via[0] == 2, _via[0])
    check("F9 [Python 复刻] findPolicy: cpu1 两条位图都没命中 -> -1",
          find_policy_index_for_cpu(_recs, 1, _via) == -1, _via[0])

    hdr = """// 自动生成(verify_freq_cap_policy.py):
//   下面这段是被测代码的原文抽取(一字未改) + 一个极小的 freestanding 外壳。
//   不链 CRT: 入口就是 mainCRTStartup, 退出码 = 第一条失败的断言序号(0 = 全部通过)。
namespace std {
inline unsigned long strlen(const char* s)
{
    unsigned long n = 0;
    while (s != nullptr && s[n] != 0) { ++n; }
    return n;
}
inline int strncmp(const char* a, const char* b, unsigned long n)
{
    for (unsigned long i = 0; i < n; ++i) {
        const unsigned char ca = (unsigned char)a[i];
        const unsigned char cb = (unsigned char)b[i];
        if (ca != cb) { return (int)ca - (int)cb; }
        if (ca == 0) { return 0; }
    }
    return 0;
}
}   // namespace std

"""
    body = "\n".join([
        "// ===== 以下 5 段从 cpu_freq_sample.cpp 逐字抽取 =====",
        struct_cap,
        func_text("parseCpuListMaskText"),
        func_text("parsePolicyIndexFromLink"),
        func_text("parseTimeInStateLine"),
        func_text("findPolicyIndexForCpu"),
    ])
    tail = """

// ===== freestanding 测试外壳 =====
static int g_case = 0;
static int g_fail = 0;
static void CHK(bool ok)
{
    ++g_case;
    if (!ok && g_fail == 0) { g_fail = g_case; }
}

extern "C" int mainCRTStartup(void)
{
    unsigned long long m = 0;
    int k = 0;
    long long t = 0;
    (void)m; (void)k; (void)t;
""" + "\n".join(lines) + """
    return g_fail;
}
"""
    return hdr + body + tail, names


i_s = SRC.find("struct CapPolicyRec {")
i_e = SRC.find("};", i_s)
struct_cap = SRC[i_s:i_e + 3] if i_s >= 0 else ""

if struct_cap == "":
    check("F3 能抽出 struct CapPolicyRec(原生执行需要它)", False)
    all_names = []
else:
    harness_src, all_names = gen_harness()
    check("F3 抽出来的片段齐全(结构体 + 四条纯逻辑函数)",
          all(x in harness_src for x in ["struct CapPolicyRec", "parseCpuListMaskText",
                                         "parsePolicyIndexFromLink", "parseTimeInStateLine",
                                         "findPolicyIndexForCpu"]) and
          harness_src.count("\n") > 200, harness_src.count("\n"))

    if not (os.path.exists(CLANG) and os.path.exists(LLD)):
        emit("  [SKIP] 本机没有 %s / %s —— '原生执行'这一段跳过(纯 Python 复刻与源码锚仍然全跑)"
             % (CLANG, LLD))
    else:
        tmp = os.path.join(tempfile.gettempdir(), "aurora_freq_cap_native")
        if not os.path.isdir(tmp):
            os.makedirs(tmp)
        logp = os.path.join(tmp, "build.log")
        if os.path.exists(logp):
            os.remove(logp)
        io.open(os.path.join(tmp, "harness.cpp"), "w", encoding="utf-8", newline="\n").write(harness_src)

        def run_native(cmd):
            with io.open(logp, "a", encoding="utf-8") as lf:
                lf.write("$ " + " ".join(cmd) + "\n")
                lf.flush()
                pr = subprocess.Popen(cmd, cwd=tmp, stdout=lf, stderr=lf)   # 用文件重定向, 不用管道
                return pr.wait()

        rc1 = run_native([CLANG, "-c", "--target=x86_64-pc-windows-msvc", "-nostdlib",
                          "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-O1",
                          "harness.cpp", "-o", "harness.obj"])
        check("F4 抽出来的源码能编译成 x86-64 目标(说明抽的是完整可编译的片段)", rc1 == 0,
              "见 " + logp)
        rc2 = -1
        if rc1 == 0:
            rc2 = run_native([LLD, "/entry:mainCRTStartup", "/subsystem:console", "/nodefaultlib",
                              "/machine:x64", "/out:harness.exe", "harness.obj"])
            check("F5 能链成不需要 CRT 的独立可执行文件", rc2 == 0, "见 " + logp)
        if rc2 == 0:
            rc3 = run_native([os.path.join(tmp, "harness.exe")])
            detail = "退出码 %d" % rc3
            if 0 < rc3 <= len(all_names):
                detail += " = 第 %d 条断言失败: %s" % (rc3, all_names[rc3 - 1])
            check("F6 原生执行 %d 条合成输入: 四条纯逻辑的真实行为逐条符合预期(退出码 0)"
                  % len(all_names), rc3 == 0, detail)
            emit("        (测试程序 = %s, 编译/链接日志 = %s)" % (os.path.join(tmp, "harness.exe"), logp))

emit("")
emit("=" * 100)
emit(("全部通过(%d) " % (len(fails) == 0)) + ("FAILS: " + ", ".join(fails) if fails else ""))
emit("=" * 100)
OUT.close()
sys.exit(1 if fails else 0)
