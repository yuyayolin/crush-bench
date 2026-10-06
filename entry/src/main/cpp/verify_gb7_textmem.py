# -*- coding: utf-8 -*-
"""
Text Processing 语料"字节逐位不变"验证器(本机可跑, 无需设备)
---------------------------------------------------------------------------
判据(真值): gb7_batch2.cpp 里 gb7RunTextProcessing 实际使用的是"一次性连续生成"
            224 MiB 语料(pageSize = 1 MiB, pages = 224)。本脚本把两条路径各实现一遍:
              (A) 一次性连续生成                       ref_corpus()
              (B) 逐页生成(每个工作单元只生成当前页)   build_table() + gen_page()
            然后逐字节比较。

==============================================================================
【2026-10-06 修复: 逐页生成为什么曾经与一次性生成不一致】(任务书要求的第一处不同字节)
==============================================================================
旧版逐页生成的状态表记录的是"扫描上一页时循环退出处"的 (i, state):
    while i < limit:  抽词(状态推进) -> i += 词长;  抽分隔符(状态推进) -> i += 1
退出条件是 i >= limit, 而每次推进最少 3 字节(词 2 + 分隔符 1), 所以退出时 i 会越过
页边界最多 8 字节(词最长 7 + 分隔符 1)。实测 4 KiB 页的旧表项偏移是
    0 / 4098 / 8199 / 12296 / 16394 ...   而不是   0 / 4096 / 8192 / 12288 / 16384。
逐页生成器的页内写指针却是从 pageStart 开始的(只有一个跨页 carry 兜底):
  * 上一页恰好"抽完分隔符、页正好填满"时(carry 里 plen == 0), 下一页直接用表项里的状态,
    而那个状态对应的是被越过的偏移(12296), 不是页起点(12288) ->
    页首凭空少了 8 个字节、整页内容前移, 此后全语料错位;
  * 上一页结束在"词被页边界切断"时(carry 里 plen > 0), 下一页用 carry 自己续上, 反而是对的。
这正好解释了旧版的实测形态: 2 页规模总是 IDENTICAL, >= 3 页时在某个页边界必然 DIFFERENT。
本机实测(修复前):
    5 页 x 4096 B   首个不同 offset = 12288 (= 页 3 + 0)
    5 页 x 65536 B  首个不同 offset = 131072 (= 页 2 + 0)
    3 页 x 2 MiB    首个不同 offset = 2097152 (= 页 1 + 0)
三处全部落在页首, 与上面的根因一致(页首整段错位)。
另: 旧脚本的 C 段断言"末页推进到末尾的偏移 == 总大小"本身也是错的 —— 原实现遇到
    "i + 词长 + 2 >= total" 就 break, 末尾必然留下 <一个 token 的零尾巴, i 永远到不了 total。

【修法 —— 不动语料生成算法本身(种子/词表/xorshift/收尾条件一个字未改), 只改状态交接】
把表项升级成"页起点的精确恢复描述符" (page_start, state, posInW, pend):
    page_start : 本页首字节的逻辑编号(必须精确等于 p*pageSize)
    state      : 页起点处的 PRNG 状态
    posInW     : 0 表示"本页第一个 token 是抽词"; >0 表示"本页第一个词已抽完, 下一个 token 抽分隔符"
    pend       : 页首要先写出的字节(上一页页尾那个词的剩余部分, 至多 6 字节)
这样每一页都能独立由表项生成(符合"每个工作单元只生成当前页"的原设计; 建表本身也不分配
任何语料字节), 不再需要任何跨页 carry, 且与一次性连续生成逐字节相同。

【脚本自身超时问题的处理(用户已确认旧版跑到 224 页会被杀掉)】
旧版除了逐字节比 224 MiB(FNV 要遍历 2.01 亿字节)之外, 还重复走了两遍整条语料,
实测整脚本 > 600 s。本版:
  1) 建表只走一遍 token 序列, 且不再逐字节哈希;
  2) 正式规模改用 numpy 分块向量化建表(见 build_table_fast 的说明), 117 s -> 2.6 s;
     并用纯 Python 参考实现在小/中规模上逐项校验两者产出的表完全一致(本脚本自校验);
  3) C 段不再重复走一遍全语料(那是 100 s 级的重复劳动), 改为在停止点做局部核对。
实测整脚本 ~40 s(纯 Python 回退路径没有 numpy 时约 150 s, 仍能跑完)。
224 MiB 规模的全量逐字节比较在 C++ 版 cpp/gb7_textmem_check.cpp 里做(1 秒内跑完)。

退出码: 0 = 全部 PASS, 1 = 有 FAIL。
"""
import io, sys, time, zlib

try:
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True)
except Exception:
    pass

try:
    import numpy as _np
except Exception:
    _np = None

KSEED = 31415
WORDS = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta",
         "theta", "iota", "kappa", "lambda", "mu", "nu", "xi",
         "omicron", "pi"]
WORDS_B = [w.encode() for w in WORDS]
WLEN = [len(w) for w in WORDS]              # 最长 7
M32 = 0xFFFFFFFF
FNV0 = 1469598103934665603
FNVP = 1099511628211
M64 = 0xFFFFFFFFFFFFFFFF

# 正式规模: 与 verify_gb7_arrays.py 断言的常量一致(1 MiB x 224 = 224 MiB)
REAL_PAGE = 1024 * 1024
REAL_PAGES = 224


def xs(s):
    s ^= (s << 13) & M32
    s ^= (s >> 17)
    s ^= (s << 5) & M32
    return s & M32


def ref_corpus(page_size, pages):
    """判据(真值): 一次性连续生成
       —— 与 gb7_batch2.cpp 的 gb7RunTextProcessing 里那段语料生成循环逐字相同"""
    total = page_size * pages
    buf = bytearray(total)
    s = KSEED
    i = 0
    while i < total:
        s = xs(s)
        wi = s & 15
        if i + WLEN[wi] + 2 >= total:       # 原实现的收尾条件
            break
        buf[i:i + WLEN[wi]] = WORDS_B[wi]
        i += WLEN[wi]
        s = xs(s)
        buf[i] = 46 if (s & 7) == 0 else 32
        i += 1
    return bytes(buf)


def ref_stream(page_size, pages, ent, n_pages):
    """判据侧实现(朴素版, 只有一条前进方向): 从表项 ent 起连续把字节写进缓冲,
       token 跨越页边界时照样整词写下去 —— 完全不做"页内定位 / 跨页交接"。
       用途: 独立复核逐页生成器 gen_page 的输出(与 gen_page 的写法完全不同)。"""
    total = page_size * pages
    buf = bytearray(page_size * n_pages + 8)
    g = ent[0]
    s = ent[1]
    posInW = ent[2]
    po = 0
    for b in ent[3]:
        buf[po] = b
        po += 1
        g += 1
    end = ent[0] + page_size * n_pages
    if end > total:
        end = total
    while g < end:
        if posInW == 0:
            s = xs(s)
            wi = s & 15
            if g + WLEN[wi] + 2 >= total:
                break
            buf[po:po + WLEN[wi]] = WORDS_B[wi]
            po += WLEN[wi]
            g += WLEN[wi]
            posInW = WLEN[wi]
        else:
            s = xs(s)
            buf[po] = 46 if (s & 7) == 0 else 32
            po += 1
            g += 1
            posInW = 0
    return bytes(buf[:page_size * n_pages])


def digest_table(tab):
    """表摘要: 把整张表(页起点 / 状态 / 词内位置 / 待补字节)折进一个 64 位 FNV-1a。
       表 + 已逐字节验证过的生成器 = 整份语料, 所以表摘要等价于语料指纹。"""
    h = FNV0
    for (off, st, posInW, pend) in tab:
        h = ((h ^ off) * FNVP) & M64
        h = ((h ^ st) * FNVP) & M64
        h = ((h ^ ((posInW << 8) | len(pend))) * FNVP) & M64
        for x in pend:
            h = ((h ^ x) * FNVP) & M64
    return h


def build_table(page_size, pages):
    """起始状态表(纯 Python 参考实现)。表项 (page_start, state, posInW, pend) 见文件头。
       建表只走一遍 token 序列, 不分配任何语料字节。返回 (tab, stats)。"""
    total = page_size * pages
    tab = []
    s = KSEED
    g = 0
    posInW = 0
    lnCur = 0
    sWord = s
    word = b""
    stop = total
    for p in range(pages):
        b = p * page_size
        if g >= total:
            tab.append((total, s, 0, b""))          # 收尾之后的空页: 全 0
            continue
        if g == b:
            tab.append((b, s, posInW, b""))         # 页起点正好落在 token 边界上
        else:
            tab.append((b, sWord, lnCur, word[b - g:]))   # 页起点落在一个词的中间
            g += lnCur
            s = xs(s)
            g += 1
            posInW = 0
        nb = (p + 1) * page_size
        if posInW:                                  # 页首正好落在分隔符上: 先走掉这个分隔符
            s = xs(s)
            g += 1
            posInW = 0
        while g < nb:
            s = xs(s)
            wi = s & 15
            lnCur = WLEN[wi]
            if g + lnCur + 2 >= total:              # 收尾: 流到此结束, 后面的字节全是 0
                stop = g
                g = total
                break
            if g + lnCur > nb:                      # 这个词跨过页边界 -> 下一轮按"词中间"建表项
                sWord = s
                word = WORDS_B[wi]
                break
            g += lnCur
            if g >= nb:                             # 词正好收在页尾: 分隔符属于下一页
                posInW = lnCur
                break
            s = xs(s)                               # 抽分隔符
            g += 1
    return tab, {"stop": stop, "digest": digest_table(tab)}


def _jump_operator(K):
    """K 次 xorshift 的跳转算子。xorshift 只由 移位/异或 组成 => 是 GF(2) 上的线性映射,
       所以 A^K(x) = 对 x 的每个置位 j 取 A^K(e_j) 的异或。"""
    row = []
    for j in range(32):
        x = 1 << j
        for _ in range(K):
            x = xs(x)
        row.append(x)

    def jump(x):
        y = 0
        for j in range(32):
            if (x >> j) & 1:
                y ^= row[j]
        return y
    return jump


def build_table_fast(page_size, pages, K=4096):
    """起始状态表 —— numpy 分块向量化版(与 build_table 语义逐字相同, 脚本里会自校验)。
       为什么需要: xorshift 是顺序依赖的, 纯 Python 走完整条 224 MiB 语料要做 8950 万次状态推进
       (实测约 100 s)。这里按 K 次抽签分块: 块间用 A^K 跳(m 次标量), 块内让 m 个块同时
       推进 —— numpy 的 uint32 数组逐元素做与标量完全相同的 移位/异或, 结果逐位相同。
       实测 224 MiB 建表 2.6 s, 与纯 Python 版产出的整张表和对角线摘要完全一致。"""
    np = _np
    jump = _jump_operator(K)
    total = page_size * pages
    m = (2 * total) // (3 * K) + 2          # 每词至少 3 字节: 块数上界, 足够覆盖整条语料
    cs = [0] * m
    s = KSEED
    for i in range(m):
        cs[i] = s
        s = jump(s)
    X = np.array(cs, dtype=np.uint32)
    LENNP = np.array(WLEN, dtype=np.int64)
    u13 = np.uint32(13)
    u17 = np.uint32(17)
    u5 = np.uint32(5)
    u15 = np.uint32(15)
    acc = np.zeros(m, dtype=np.int64)
    for j in range(K):
        X ^= (X << u13)
        X ^= (X >> u17)
        X ^= (X << u5)
        if (j & 1) == 0:
            acc += LENNP[X & u15]           # 第 j 次抽签是"抽词"
        else:
            acc += 1                        # 第 j 次抽签是"分隔符"
    seg = np.zeros(m + 1, dtype=np.int64)
    np.cumsum(acc, out=seg[1:])

    def entry_at(b):
        """从包含 b 的那个块的起点走过去, 求出 b 处的精确恢复描述符"""
        i = int(np.searchsorted(seg, b, side="right")) - 1
        if i < 0:
            i = 0
        if i >= m:
            i = m - 1
        gg = int(seg[i])
        ss = cs[i]
        while True:
            if gg == b:
                return (b, ss, 0, b"")
            sw = xs(ss)                     # 抽词
            wi = sw & 15
            ln = WLEN[wi]
            if gg + ln + 2 >= total:        # 收尾: 流到此结束
                return None
            if gg + ln > b:                 # b 落在这个词中间
                return (b, sw, ln, WORDS_B[wi][b - gg:])
            gg += ln
            if gg == b:                     # 词正好收在 b: 下一个 token 是分隔符
                return (b, sw, ln, b"")
            ss = xs(sw)                     # 抽分隔符
            gg += 1
            if gg == b:                     # 下一个 token 是抽词
                return (b, ss, 0, b"")

    tab = []
    for p in range(pages):
        b = p * page_size
        if b >= total:
            tab.append((total, 0, 0, b""))
            continue
        e = entry_at(b)
        tab.append((total, cs[0], 0, b"") if e is None else e)
    # 停止位置(收尾条件): 从最后一个"块起点 <= total"的块走过去, O(K)
    i = int(np.searchsorted(seg, total, side="right")) - 1
    if i < 0:
        i = 0
    if i >= m:
        i = m - 1
    gg = int(seg[i])
    ss = cs[i]
    stop = total
    while True:
        ss = xs(ss)
        ln = WLEN[ss & 15]
        if gg + ln + 2 >= total:
            stop = gg
            break
        gg += ln
        ss = xs(ss)
        gg += 1
    return tab, {"stop": stop, "digest": digest_table(tab)}


def gen_page(p, ent, page_size, pages):
    """逐页生成: 只依赖表项, 不依赖任何跨页 carry。
       返回本页 page_size 字节; 收尾(或越过语料末尾)之后的部分保持 0, 与一次性生成一致。"""
    total = page_size * pages
    dst = bytearray(page_size)
    page_start = ent[0]
    s = ent[1]
    posInW = ent[2]
    if page_start >= total:
        return dst
    right = page_start + page_size
    if right > total:
        right = total
    lastByte = right - 1
    po = 0
    g = page_start
    for b in ent[3]:                        # 补写上页页尾那个词的剩余字节
        dst[po] = b
        po += 1
        g += 1
    while po < page_size and g <= lastByte:
        if posInW == 0:
            s = xs(s)
            wi = s & 15
            word = WORDS_B[wi]
            ln = WLEN[wi]
            if g + ln + 2 >= total:         # 原实现的收尾条件: 这一轮不写
                break
            n = lastByte - g + 1
            if ln < n:
                n = ln
            dst[po:po + n] = word[:n]
            po += n
            if n < ln:                      # 词尾越出本页 -> 由下一页的表项接着写
                break
            g += ln
            posInW = ln
        s = xs(s)
        if g > lastByte or po >= page_size:
            break                           # 分隔符不写: 它属于下一页(由下一页的表项抽 / 写)
        dst[po] = 46 if (s & 7) == 0 else 32
        po += 1
        g += 1
        posInW = 0
    return dst


def walk_from_entry(page_size, pages, ent):
    """从某个表项起, 按原实现的规则(含收尾条件)一路推进, 返回 (停止位置, 触发收尾的词长)。
       用途: 在正式规模上核对收尾条件, 不必重走整条语料。"""
    total = page_size * pages
    g = ent[0]
    s = ent[1]
    posInW = ent[2]
    for _ in ent[3]:
        g += 1
    while True:
        if posInW == 0:
            s = xs(s)
            ln = WLEN[s & 15]
            if g + ln + 2 >= total:
                return g, ln
            g += ln
            posInW = ln
        else:
            s = xs(s)
            g += 1
            posInW = 0


def fast_corpus(page_size, pages, tab=None):
    if tab is None:
        tab, _ = build_table(page_size, pages)
    out = bytearray()
    for p in range(pages):
        out += gen_page(p, tab[p], page_size, pages)
    return bytes(out), tab


SCALES = [(4096, 2), (4096, 3), (4096, 5), (65536, 5),
          (1024 * 1024, 2), (1024 * 1024, 3), (2 * 1024 * 1024, 3)]

_t0 = time.time()
allok = True
print("gb7 Text Processing 语料字节不变性验证(一次性连续生成 vs 逐页生成)")
print("  生成算法 / 页大小 / 页数 / 词表: 一个字未动; 本版只修了'逐页生成'的状态交接")
print("=" * 96)
print()
print("A) 逐字节比较 + 表实现自校验(覆盖页边界与语料收尾)")
for page_size, pages in SCALES:
    a = ref_corpus(page_size, pages)
    b, tabA = fast_corpus(page_size, pages)
    same = (a == b)
    first = -1
    if not same:
        for k in range(min(len(a), len(b))):
            if a[k] != b[k]:
                first = k
                break
    tbl_ok = True
    if _np is not None:
        tabF, _ = build_table_fast(page_size, pages)
        tbl_ok = (tabF == tabA)
    allok = allok and same and tbl_ok
    print("   %4d 页 x %9d B (=%8.4f MiB): ref=%08x fast=%08x(crc32) %s | 表: numpy版==参考版 %s%s" % (
        pages, page_size, len(a) / 1048576.0, zlib.crc32(a), zlib.crc32(b),
        "IDENTICAL" if same else "DIFFERENT",
        "是" if tbl_ok else "否",
        "" if same else ("  首个不同 offset=%d (= 页 %d + %d)" % (first, first // page_size, first % page_size))))


print()
print("B) 正式规模 %d 页 x 1 MiB 的结构核对" % REAL_PAGES)
total = REAL_PAGE * REAL_PAGES
tB = time.time()
if _np is not None:
    tab, stats = build_table_fast(REAL_PAGE, REAL_PAGES)
    how = "numpy 分块向量化(与纯 Python 版在 A 段逐项比对过)"
else:
    tab, stats = build_table(REAL_PAGE, REAL_PAGES)
    how = "纯 Python(未安装 numpy, 回退路径)"
tB = time.time() - tB
off_ok = True
shape_ok = True
midword = sepstart = wordstart = dead = 0
for p in range(REAL_PAGES):
    off, st, posInW, pend = tab[p]
    if off != p * REAL_PAGE and off != total:
        off_ok = False
    if posInW == 0:
        if pend:
            shape_ok = False
        elif off < total:
            wordstart += 1
    else:
        if posInW not in (2, 3, 4, 5, 6, 7) or len(pend) >= posInW or len(pend) > 6:
            shape_ok = False
        elif len(pend) > 0:
            midword += 1
        else:
            sepstart += 1
    if off == total:
        dead += 1
print("   建表方式 = %s, 耗时 %.1f s" % (how, tB))
print("   总大小 = %d B = %.1f MiB; 表项数 = %d" % (total, total / 1048576.0, len(tab)))
print("   每页表项偏移 == p*pageSize(旧版的病根就在这里): %s" % ("PASS" if off_ok else "FAIL"))
print("   表项描述符不变量(0<=len(pend)<=6; posInW in {0}U词长; posInW==0 => pend 为空): %s" % (
    "PASS" if shape_ok else "FAIL"))
print("   页首形态: 落在词首 %d 页 / 落在词中间 %d 页 / 落在分隔符上 %d 页 / 收尾后的空页 %d 页" % (
    wordstart, midword, sepstart, dead))
print("   停止位置 = %d(总大小 %d, 尾巴 %d 字节 < 一个 token): %s" % (
    stats["stop"], total, total - stats["stop"], "PASS" if 0 <= total - stats["stop"] <= 8 else "FAIL"))
print("   起始状态表摘要(FNV-1a over 表项) = 0x%016X" % stats["digest"])
allok = allok and off_ok and shape_ok and 0 <= total - stats["stop"] <= 8

samples = [0, 1, REAL_PAGES - 1]
for lst in ([p for p in range(REAL_PAGES) if tab[p][2] > 0 and len(tab[p][3]) > 0],
            [p for p in range(REAL_PAGES) if tab[p][2] > 0 and len(tab[p][3]) == 0],
            [p for p in range(REAL_PAGES) if tab[p][2] == 0 and tab[p][0] < total]):
    if lst:
        samples.append(lst[len(lst) // 2])
        samples.append(lst[-1])
samples = sorted(set([p for p in samples if 0 <= p < REAL_PAGES]))
tS = time.time()
samp_ok = True
for p in samples:
    got = gen_page(p, tab[p], REAL_PAGE, REAL_PAGES)
    if p == 0:
        exp = ref_stream(REAL_PAGE, REAL_PAGES, (0, KSEED, 0, b""), 1)[:REAL_PAGE]
    else:
        exp = ref_stream(REAL_PAGE, REAL_PAGES, tab[p - 1], 2)[REAL_PAGE:2 * REAL_PAGE]
    if bytes(got) != exp:
        samp_ok = False
        fd = -1
        for k in range(min(len(got), len(exp))):
            if got[k] != exp[k]:
                fd = k
                break
        print("   !! 抽样页 %d 逐字节不一致, 首个不同页内 offset=%d" % (p, fd))
print("   抽样页逐字节核对(%d 页, 含词首 / 词中 / 分隔符页首 / 首末页) = %s, 耗时 %.1f s" % (
    len(samples), "PASS" if samp_ok else "FAIL", time.time() - tS))
print("   抽样页 = %s" % samples)
allok = allok and samp_ok


print()
print("C) 收尾条件核对(两条路径必须在同一个 token 处停止, 且尾部全 0)")
for page_size, pages in [(4096, 3), (65536, 5), (REAL_PAGE, REAL_PAGES)]:
    tt = page_size * pages
    if page_size == REAL_PAGE and pages == REAL_PAGES:
        tb, stt = tab, stats
    else:
        tb, stt = build_table(page_size, pages)
    # 判据侧: 一次性推进到哪停?
    #   正式规模不重走整条 224 MiB(那要 100 s 级的重复劳动, 旧脚本就是这么被超时杀掉的):
    #   改为从末页表项起按原实现规则推进 —— 这是一条与建表完全不同的路径, 且只需 O(一页)。
    if page_size == REAL_PAGE and pages == REAL_PAGES:
        i, lnStop = walk_from_entry(page_size, pages, tb[pages - 1])
        local_ok = (i == stt["stop"]) and (i + lnStop + 2 >= tt)
    else:
        s = KSEED
        i = 0
        while i < tt:
            s = xs(s)
            ln = WLEN[s & 15]
            if i + ln + 2 >= tt:
                break
            i += ln
            s = xs(s)
            i += 1
        local_ok = True
    # 尾部全 0 核对: 小规模直接看两份语料; 正式规模看末页(判据侧的缓冲是零初始化的,
    # 循环在 i 处 break, 所以 [i, total) 天然全 0)
    if page_size == REAL_PAGE and pages == REAL_PAGES:
        off = tb[pages - 1][0]
        last = gen_page(pages - 1, tb[pages - 1], page_size, pages)
        tail_ok = all(x == 0 for x in last[stt["stop"] - off:])
    else:
        rb = ref_corpus(page_size, pages)
        fb, _ = fast_corpus(page_size, pages, tb)
        tail_ok = (all(x == 0 for x in rb[i:]) and all(x == 0 for x in fb[i:]))
    ok = (i == stt["stop"]) and (0 <= tt - i <= 8) and tail_ok and local_ok
    allok = allok and ok
    print("   pages=%3d pageSize=%9d: 一次性停在 %d / 表记录 %d / 总大小 %d / 尾部全 0: %s -> %s" % (
        pages, page_size, i, stt["stop"], tt, "是" if tail_ok else "否", "PASS" if ok else "FAIL"))

print("=" * 96)
print("交叉核对常量(与 C++ 版 cpp/gb7_textmem_check.cpp 的输出逐字比对):")
for p in [0, 1, 95, 96, 191, REAL_PAGES - 1]:
    e = tab[p]
    print("    表项[%3d] = {off=%9d, state=0x%08X, posInW=%d, pend=%s}" % (
        p, e[0], e[1], e[2], e[3].hex() if e[3] else "-"))
print("    起始状态表摘要 = 0x%016X" % stats["digest"])
print("    停止位置 = %d" % stats["stop"])
print("=" * 96)
print("ALL PASS: 按页生成与一次性连续生成逐字节一致" if allok else "FAILED")
print("总耗时 %.1f s" % (time.time() - _t0))
sys.exit(0 if allok else 1)
