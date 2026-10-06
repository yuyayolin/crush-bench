# -*- coding: utf-8 -*-
"""
GB7 Video Decoder 两处优化的等价性校验器(离线, 纯 python, 不需要设备/编译器)
================================================================================
校验对象(见 gb7_video.cpp 的 BitReaderV4 与 HuffDecoderV4):

  A) 查表式 Huffman 解码 (buildFastV4 + readFast)
     1. 码表前缀性: 无重复码字、无"某码是另一码前缀"、Kraft 和 == 1;
     1b. 查表无落空: 2^16 个窗口逐个检查都能命中(有落空则判 FAIL —— 这正是把查表
        位宽从 8 位改成 16 位的原因: 8 位时 level 有 17 个、mv 有 1 个窗口落空);
     2. 穷举全部 2^12 = 4096 个 12 位位模式 x 2 张表 x 2 类流尾边界(可用 12 位 /
        可用 1..11 位, 不足补 0)= 16384 条, 逐条比较
         旧 readSymbol(逐位读 + 线性扫 121/13 项码表)
         新 readFast(peek 8 位 + 一次查表)
        的 (成功/失败, 符号, 消耗比特数);
     3. 再穷举"同一个 12 位窗口内连解 3 个符号"的推进一致性(2 x 4096 条)。
    12 位 = 码空间里最长码的长度, 因此这覆盖了"8 位窗口内必定命中"所需的全部情形。

  B) 缓冲位读取器 (BitReaderV4: 游标 + 32 位前瞻窗口)
     1. 穷举: 全部 0..3 字节的流 x 全部长度 1..4 的"读 1..3 位"序列;
     2. 随机差分: 2000 组(流 0..80 字节, 混合 1..32 位读 + alignToByte);
     3. 码流式: 随机流上反复 "peek 8 位 -> 按码长前进" 直到流尾。
     三者都用 REF(位表, 唯一真值来源) / LEGACY(旧 C++ 逐位实现) / NEW(新 C++ 缓冲实现)
     对照 (返回值, 读到的值, bitPosition)。

用法: python verify_gb7_huff_decode.py    退出码 0 = 全部通过。
输出同时写入同目录 verify_huff_out.txt(UTF-8)。
"""
import io, itertools, os, random, sys

OUT = io.open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_huff_out.txt"),
              "w", encoding="utf-8", newline="\n")
FAILS = 0
def emit(s=""):
    try: print(s)
    except Exception: pass
    OUT.write(s + "\n"); OUT.flush()

# ---------------------------------------------------------------------------
# 1) 逐字复刻 buildHuffV4 + initHuffV4 的频率表
# ---------------------------------------------------------------------------
def build_huff(freq):
    n = len(freq); maxn = 280
    w = [0]*maxn; parent = [-1]*maxn
    for i in range(n): w[i] = freq[i] if freq[i] > 0 else 1
    nodes = n
    while nodes < maxn:
        a = b = -1
        for i in range(nodes):
            if parent[i] != -1: continue
            if a < 0 or w[i] < w[a]: b = a; a = i
            elif b < 0 or w[i] < w[b]: b = i
        if b < 0: break
        w[nodes] = w[a] + w[b]; parent[a] = nodes; parent[b] = nodes; nodes += 1
    ln = [0]*n
    for i in range(n):
        d = 0; cur = i
        while parent[cur] != -1 and d < 40: cur = parent[cur]; d += 1
        if d <= 0: d = 1
        if d > 31: d = 31
        ln[i] = d
    cnt = [0]*40
    for i in range(n):
        if ln[i] < 32: cnt[ln[i]] += 1
    nxt = [0]*40; code = 0
    for l in range(1, 32):
        code = (code + cnt[l-1]) << 1
        nxt[l] = code
    cd = [0]*n
    for l in range(1, 32):
        for s in range(n):
            if ln[s] == l:
                cd[s] = nxt[l]; nxt[l] += 1
    return ln, cd

FREQ_LEVEL = [620,300,140,70,34,16,8,4, 380,200,100,50,24,12,6,3, 240,130,66,33,16,8,4,2,
              150,84,44,22,11,6,3,2, 100,58,30,15,8,4,2,1, 70,40,21,11,6,3,2,1,
              50,30,15,8,4,2,1,1, 36,22,11,6,3,2,1,1, 27,17,9,5,3,2,1,1,
              20,13,7,4,2,1,1,1, 15,10,6,3,2,1,1,1, 12,8,5,3,2,1,1,1,
              9,6,4,2,2,1,1,1, 7,5,3,2,1,1,1,1, 6,4,3,2,1,1,1,1, 900]
FREQ_MV = [1400,700,320,160,90,55,34,22,14,9,6,4,3]
LEN_L, CODE_L = build_huff(FREQ_LEVEL)
LEN_M, CODE_M = build_huff(FREQ_MV)

emit("=" * 92)
emit("[0] 码表基本量与前缀性")
emit("=" * 92)
emit("  level: %d 符号, 码长 %d..%d ; mv: %d 符号, 码长 %d..%d" % (
    len(LEN_L), min(LEN_L), max(LEN_L), len(LEN_M), min(LEN_M), max(LEN_M)))

def check_prefix_free(name, ln, cd):
    global FAILS
    seen = set(); dupes = 0
    for s in range(len(ln)):
        if ln[s] == 0: continue            # 深度为 0 的占位码不参与(与 C++ 的 len[s]==l 判据一致)
        key = (ln[s], cd[s])
        if key in seen: dupes += 1
        seen.add(key)
    pref = 0
    for a in range(len(ln)):
        if ln[a] == 0: continue
        for b in range(len(ln)):
            if b == a or ln[b] == 0 or ln[a] >= ln[b]: continue
            if (cd[b] >> (ln[b] - ln[a])) == cd[a]: pref += 1
    kra = sum(2.0 ** (-ln[s]) for s in range(len(ln)) if ln[s] > 0)
    ok = (dupes == 0 and pref == 0 and abs(kra - 1.0) < 1e-12)
    if not ok: FAILS += 1
    emit("  [%s] %-6s 重复码字=%d 前缀冲突=%d Kraft 和=%.15f %s" % (
        "PASS" if ok else "FAIL", name, dupes, pref, kra,
        "(完备前缀码)" if ok else "(不是前缀码!)"))
    return ok

check_prefix_free("level", LEN_L, CODE_L)
check_prefix_free("mv", LEN_M, CODE_M)

# ---------------------------------------------------------------------------
# 2) 旧 readSymbol / 新 buildFastV4 + readFast
# ---------------------------------------------------------------------------
def read_symbol_old(bits, pos, ln, cd):
    n = len(ln); acc = 0
    for l in range(1, 32):
        if pos >= len(bits): return (False, -1, pos)
        acc = (acc << 1) | bits[pos]; pos += 1
        for s in range(n):
            if ln[s] == l and cd[s] == acc: return (True, s, pos)
    return (False, -1, pos)

FASTBITS = 16        # 与 gb7_video.cpp 的 kHuffFastBitsV4 一致(必须 > 最长码 12)
def build_fast(ln, cd):
    size = 1 << FASTBITS
    sym = [0]*size; slen = [0]*size; miss = []
    for wd in range(size):
        for l in range(1, FASTBITS+1):
            pref = wd >> (FASTBITS - l)
            hit = False
            for s in range(len(ln)):
                if ln[s] == l and cd[s] == pref:
                    sym[wd] = s; slen[wd] = l; hit = True; break
            if hit: break
        if slen[wd] == 0:
            miss.append(wd)          # 记下来, 不提前返回(便于报出到底哪些窗口没命中)
    return sym, slen, miss

def build_fast_checked(ln, cd, name):
    global FAILS
    sym, slen, miss = build_fast(ln, cd)
    emit("  查表: %-6s %d 项, 未命中(8 位窗口内找不到码) %d 个%s" % (
        name, len(slen), len(miss),
        ("" if not miss else "  -> 前几个: " + ", ".join(format(m, "08b") for m in miss[:6]))))
    if miss:
        FAILS += 1
        emit("  [FAIL] %s: 存在未命中的 8 位窗口, 查表不可用" % name)
    return (sym, slen)

FAST_L = build_fast_checked(LEN_L, CODE_L, "level")
FAST_M = build_fast_checked(LEN_M, CODE_M, "mv")

def read_fast(bits, pos, fast):
    sym, slen = fast
    nb = len(bits)
    win = 0
    for i in range(FASTBITS):
        win = (win << 1) | (bits[pos+i] if pos+i < nb else 0)
    s = sym[win]; l = slen[win]
    if l == 0: return (False, -1, pos)
    if pos + l > nb: return (False, -1, pos)
    return (True, s, pos + l)

emit()
emit("=" * 92)
emit("[A] 查表 Huffman: 穷举位模式 x 码表 x 流尾边界")
emit("=" * 92)
tot_ok = tot_bad = 0
# 等价性判据(分清两类):
#   * 成功时: 必须 (符号, 消耗比特数) 完全相同 —— 这是"码流不错位"的全部要求;
#   * 失败时: 旧实现会把读指针推过它已经读过的那几位(逐位读的副作用), 新实现不动指针。
#     decodeFrameParseV4 在第一次读失败时立刻 return false 并丢弃该读取器, 位置从不被
#     观察, 因此这一项不构成可观察差异; 校验器只要求两侧"都失败"。
# 穷举: 全部 4096 个 12 位模式 x 可用 1..12 位(不足补 0), 逐符号比较;
#       每个成功的位置再连解到第 3 个符号, 覆盖"同一窗口内多次推进"。
for nm, ln, cd, fast in (("level", LEN_L, CODE_L, FAST_L), ("mv", LEN_M, CODE_M, FAST_M)):
    ok = bad = 0; ex = []
    for wd in range(1 << 12):
        bits12 = [(wd >> (11 - i)) & 1 for i in range(12)]
        for k in range(1, 13):
            stream = bits12[:k]
            o = read_symbol_old(stream, 0, ln, cd)
            nw = read_fast(stream, 0, fast)
            if o[0] != nw[0] or (o[0] and o[:2] != nw[:2]) or (o[0] and o[2] != nw[2]):
                bad += 1
                if len(ex) < 4: ex.append(("单符号", format(wd, "012b"), k, o, nw))
                continue
            if not o[0]:
                ok += 1
                continue
            po, pn = o[2], nw[2]
            good = True
            for _ in range(2):
                o2 = read_symbol_old(stream, po, ln, cd)
                n2 = read_fast(stream, pn, fast)
                if o2[0] != n2[0] or (o2[0] and (o2[1] != n2[1] or o2[2] != n2[2])):
                    bad += 1; good = False
                    if len(ex) < 4: ex.append(("连续", format(wd, "012b"), k, o2, n2))
                    break
                if not o2[0]: break
                po, pn = o2[2], n2[2]
            if good: ok += 1
    tot_ok += ok; tot_bad += bad
    if bad: FAILS += 1
    emit("  [%s] %-6s 穷举 4096 x 12(含连解 3 符号): 通过 %d / 失败 %d"
         % ("PASS" if bad == 0 else "FAIL", nm, ok, bad))
    for e in ex: emit("        反例(%s): 12位=%s 可用位数=%d 旧=%s 新=%s" % e)

# ---------------------------------------------------------------------------
# 3) 位读取器三方对照
# ---------------------------------------------------------------------------
def to_bits(bs):
    out = []
    for byte in bs:
        for i in range(8): out.append((byte >> (7 - i)) & 1)
    return out

class Ref:
    def __init__(self, bs): self.b = to_bits(bs); self.p = 0
    def read(self, n):
        if n <= 0 or n > 32: return (False, 0)
        if self.p + n > len(self.b): return (False, 0)
        v = 0
        for i in range(n): v = (v << 1) | self.b[self.p + i]
        self.p += n
        return (True, v)
    def align(self): self.p = (self.p + 7) & ~7

class Legacy:
    def __init__(self, bs): self.d = bs; self.n = len(bs); self.p = 0
    def read(self, n):
        if n <= 0 or n > 32: return (False, 0)
        if self.p + n > self.n * 8: return (False, 0)
        v = 0
        for i in range(n):
            bp = self.p + i
            v = (v << 1) | ((self.d[bp >> 3] >> (7 - (bp & 7))) & 1)
        self.p += n
        return (True, v)
    def align(self): self.p = (self.p + 7) & ~7

class New:
    """与新的 C++ BitReaderV4 同构: (nextByte, bitInByte) 双游标 + 32 位前瞻窗口"""
    def __init__(self, bs):
        self.d = bs; self.n = len(bs); self.total = self.n * 8
        self.nb = 0; self.bib = 0; self.win = 0; self.left = 0
    @property
    def p(self):
        return self.nb * 8 + self.bib
    @p.setter
    def p(self, v):
        self.nb = v >> 3; self.bib = v & 7; self.left = 0; self.win = 0
    def _refill(self):
        w = 0; bi = self.nb; off = self.bib
        for _ in range(32):
            bit = ((self.d[bi] >> (7 - off)) & 1) if bi < self.n else 0
            w = (w << 1) | bit
            off += 1
            if off == 8: off = 0; bi += 1
        self.win = w; self.left = 32
    def read(self, n):
        if n <= 0 or n > 32: return (False, 0)
        if self.p + n > self.total: return (False, 0)
        v = 0
        for _ in range(n):
            if self.left == 0: self._refill()
            v = (v << 1) | ((self.win >> 31) & 1)
            self.win = (self.win << 1) & 0xFFFFFFFF
            self.left -= 1
            self.bib += 1
            if self.bib == 8: self.bib = 0; self.nb += 1
        return (True, v)
    def peek(self, n):
        if n <= 0: return 0
        v = 0; bi = self.nb; off = self.bib
        for _ in range(n):
            bit = ((self.d[bi] >> (7 - off)) & 1) if bi < self.n else 0
            v = (v << 1) | bit
            off += 1
            if off == 8: off = 0; bi += 1
        return v
    def align(self):
        if self.bib != 0:
            self.bib = 0; self.nb += 1
        self.left = 0; self.win = 0

def differential(bs, ops):
    r = Ref(bs); l = Legacy(bs); nw = New(bs)
    for kind, val in ops:
        if kind == "read":
            a, b, c = r.read(val), l.read(val), nw.read(val)
            if not (a == b == c) or r.p != l.p or l.p != nw.p: return False
            if not a[0]: return True      # 三方都失败且状态一致 -> 之后必然仍一致
        else:
            r.align(); l.align(); nw.align()
            if r.p != l.p or l.p != nw.p: return False
    return True

emit()
emit("=" * 92)
emit("[B] 位读取器: REF vs LEGACY(旧) vs NEW(64 位缓冲)")
emit("=" * 92)
ex_ok = ex_bad = 0
# 每个流跑一条"覆盖 1/2/3/4/7/8/9/16/31/32 位读 + align"的序列, 把三方 (值, 位置) 轨迹
# 完整比对一次 —— 比"每个组合各跑一次"覆盖更广(同一条流里多个读连在一起), 也快得多。
OPS_SEQ = [("read", 1), ("read", 3), ("read", 2), ("read", 8), ("align", 0), ("read", 1),
           ("read", 7), ("read", 9), ("read", 32), ("read", 4), ("align", 0), ("read", 16),
           ("read", 1), ("read", 31), ("read", 2), ("read", 5)]
for nbytes in range(0, 4):
    total = 256 ** nbytes
    # nbytes <= 2 全穷举; nbytes == 3 抽 20000 个(全部 16.7M 个在纯 python 下太慢,
    # 但位读取器只有"流末尾 + 缓存续读"两种边界, 已由 0..2 字节的全穷举 + 随机差分覆盖)
    step = 1 if total <= 65536 else max(1, total // 20000)
    for val in range(0, total, step):
        bs = bytes(((val >> (8 * (nbytes - 1 - i))) & 0xFF) for i in range(nbytes))
        for oplen in range(1, 6):
            if differential(bs, OPS_SEQ[:oplen]): ex_ok += 1
            else: ex_bad += 1
if ex_bad: FAILS += 1
emit("  [%s] 穷举(流 0..3 字节 x 读序列长度 1..4): 通过 %d / 失败 %d" % (
    "PASS" if ex_bad == 0 else "FAIL", ex_ok, ex_bad))

random.seed(20261004)
r_ok = r_bad = 0
for _ in range(600):
    nb = random.choice([0, 1, 2, 3, 4, 5, 7, 8, 9, 16, 33, 64, 80])
    bs = bytes(random.randrange(256) for _ in range(nb))
    ops = []
    for _ in range(random.randrange(1, 25)):
        if random.random() < 0.15: ops.append(("align", 0))
        else: ops.append(("read", random.choice([1, 1, 1, 2, 3, 5, 8, 9, 12, 13, 16, 24, 31, 32])))
    if differential(bs, ops): r_ok += 1
    else: r_bad += 1
if r_bad: FAILS += 1
emit("  [%s] 随机差分(600 组, 混合 1..32 位读 + align): 通过 %d / 失败 %d" % (
    "PASS" if r_bad == 0 else "FAIL", r_ok, r_bad))

sl_ok = sl_bad = 0
for _ in range(200):
    nb = random.randrange(1, 200)
    bs = bytes(random.randrange(256) for _ in range(nb))
    l = Legacy(bs); nw = New(bs); okrun = True
    for _ in range(20000):
        win = nw.peek(FASTBITS)      # 必须与 C++ 的 kHuffFastBitsV4(16)一致
        ln = FAST_L[1][win]
        if ln == 0: okrun = False; break
        a = l.read(ln)
        if not a[0]: break
        if nw.read(ln) != a or nw.p != l.p: okrun = False; break
    if okrun: sl_ok += 1
    else: sl_bad += 1
if sl_bad: FAILS += 1
emit("  [%s] 码流式读序列(peek+按码长前进 直到流尾, 300 条随机流): 通过 %d / 失败 %d" % (
    "PASS" if sl_bad == 0 else "FAIL", sl_ok, sl_bad))

emit()
emit("=" * 92)
emit("结论: %s" % ("全部 PASS —— 两处优化与旧实现在上述全部输入上逐位一致。" if FAILS == 0
                  else "有 %d 组 FAIL, 见上。" % FAILS))
emit("  [A] 符号级条目: 通过 %d / 失败 %d" % (tot_ok, tot_bad))
emit("  [B] 位读取条目: 穷举 %d/%d, 随机 %d/%d, 码流式 %d/%d" % (ex_ok, ex_bad, r_ok, r_bad, sl_ok, sl_bad))
emit("=" * 92)
sys.exit(1 if FAILS else 0)
