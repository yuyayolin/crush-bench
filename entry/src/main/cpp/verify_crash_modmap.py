# -*- coding: utf-8 -*-
"""
crash_guard「模块表 / 逐项地址空间归因」离线断言(本机没有真机, 所以全部在主机上跑真代码)

为什么这么写:
  真机不在手上, "pc 到底落在哪个库"这条逻辑没法靠真机崩溃来验证。于是把这段纯逻辑
  (crash_modmap.h)做成完全不依赖任何头文件/全局状态的代码, 然后用 NDK 自带的 clang
  编成一个没有 CRT 的 x86_64 DLL, 再从 Python(ctypes) 里对合成的 maps 文本与
  合成的栈字节直接调用它, 逐字节断言输出。
  关键点: 这里跑的就是 libaurorabench.so 里跑的那一份源码(同一个 crash_modmap.h),
  不是"照着抄一遍"。

覆盖:
  A. 合成 maps 文本 -> 模块表: 可执行段入表 / 非可执行段被忽略 / 空路径 / [vdso] /
     文件偏移与装载基址 / 超长路径截断 / 表满(还有 N 段没记) / 脏行不崩;
  B. 地址归属: pc 命中 / 未命中(落在段间空隙) / 未命中(落在非可执行段) / 空表的措辞;
  C. 穷人的调用栈: 合成栈字 -> 候选个数 / 槽位偏移 / 上限截断 / 每行文本;
  D. 源码级断言: 处理器里没有 dladdr/dl_iterate_phdr/重新读 maps/分配;
     逐项归因接在既有的 item 标记点上, 判据写死 512MB/256MB, 落盘文件是 native_memory.txt。
"""
import os, io, re, sys, ctypes, subprocess, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = io.open(os.path.join(HERE, "verify_modmap_out.txt"), "w", encoding="utf-8", newline="\n")
fails = 0
checks = 0

def emit(s=""):
    try:
        print(s)
    except Exception:
        pass
    OUT.write(s + "\n")
    OUT.flush()

def check(label, ok, detail=""):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
    emit("  [%s] %s%s" % ("PASS" if ok else "FAIL", label, ("   " + detail) if detail else ""))

def read(f):
    with io.open(os.path.join(HERE, f), encoding="utf-8", errors="replace") as fh:
        return fh.read()

def find_clang():
    cands = []
    env = os.environ.get("AURORA_OHOS_LLVM_BIN")
    if env:
        cands.append(env)
    try:
        lp = read("../../../../local.properties")
        m = re.search(r"(?:sdk|sdk\.dir|native)\s*=\s*(.+)", lp)
        if m:
            p = m.group(1).strip().replace("\\\\", "\\")
            cands.append(os.path.join(p, "native", "llvm", "bin"))
            cands.append(os.path.join(p, "llvm", "bin"))
    except Exception:
        pass
    cands.append(r"D:\ohos-tools\sdk\native\llvm\bin")
    for c in cands:
        cc = os.path.join(c, "clang++.exe")
        ld = os.path.join(c, "lld-link.exe")
        if os.path.isfile(cc) and os.path.isfile(ld):
            return c
    return None

# ===========================================================================
emit("=" * 108)
emit("Aurora crash_guard 取证升级 · 离线断言(crash_modmap.h 真代码 + 合成输入)")
emit("=" * 108)

# ---------------------------------------------------------------- D) 源码级断言
emit("")
emit("D) 源码级断言(处理器里不许碰动态链接器 / 逐项归因接在既有标记点上 / 判据写死)")
cg = read("crash_guard.cpp")
hdr = read("crash_modmap.h")

check("crash_guard.cpp 使用 crash_modmap.h", '#include "crash_modmap.h"' in cg)
check("模块表刷新调用纯逻辑解析函数",
      "auroracg::mmFillLine(&fill, line)" in cg and "auroracg::mmParseExecLine(line, &tmp)" in cg)

# 处理器区间: cgHandleFatal 的各个分段(主记录 + 栈扫描)
i_main = cg.index("void cgHandleFatal(")
i_inst = cg.index("void cgInstallOne(")
handler = cg[i_main:i_inst]
i_stack = cg.index("void cgWriteStackCandidates(")
stackfn = cg[i_stack:i_main]
check("处理器内 pc/lr 归属走纯内存查表", "mmFormatAddrHit" in handler)
check("处理器内栈扫描走纯内存扫描", "mmScanStack" in stackfn and "cgWriteStackCandidates(fd, spv, nMod)" in handler)
for bad, why in [("dladdr(", "dladdr 会碰动态链接器的锁"),
                 ("dl_iterate_phdr", "dl_iterate_phdr 会碰动态链接器的锁"),
                 ('open("/proc/self/maps"', "处理器里重新读 maps"),
                 ("malloc(", "处理器里分配"),
                 ("snprintf(", "处理器里 snprintf")]:
    offenders = [name for name, text in (("cgHandleFatal", handler), ("cgWriteStackCandidates", stackfn)) if bad in text]
    check("处理器内没有 %-22s (%s)" % (bad, why), not offenders, ("出现在 " + ",".join(offenders)) if offenders else "")

check("先落主记录再扫栈(栈扫描踩空也不丢现场)",
      handler.index("cgWriteAll(fd, rec, (int)(p - rec));") < handler.index("cgWriteStackCandidates(fd, spv, nMod)"))
i_ctorA = cg.index("__attribute__((constructor)) void cgInstall()")
i_ctorB = cg.index("g_ready = 1;", i_ctorA)
check("模块表在安装时读一次 maps(普通上下文)",
      "cgRefreshExecRanges();" in cg[i_ctorA:i_ctorB])
check("每个标记点刷新模块表(1 秒节流)", "cgMaybeRefreshModTable();" in cg and "(now - g_modRefreshMs) < 1000" in cg)

i_set = cg.index("extern \"C\" void auroraSetCurrentItem(")
i_idle = cg.index("extern \"C\" void auroraSetIdleMarker(")
check("逐项归因接在 auroraSetCurrentItem(既有 item 标记点)上",
      "cgItemMemOnMarker(phase, item, index, total);" in cg[i_set:i_idle])
i_after_idle = cg.index("extern \"C\" int auroraSetLogDir(")
check("auroraSetIdleMarker 会给最后一项结算", "cgItemMemOnIdle();" in cg[i_idle:i_after_idle])
check("逐项采样发生在 g_seq 那段临界区之外(标记仍然原子)",
      cg[i_set:i_idle].index("cgItemMemOnMarker") < cg[i_set:i_idle].index("int s = (int)g_seq;"))
check("判据写死 ΔVmSize > 512 MB", "kAbnormalSizeKb = 512LL * 1024LL" in hdr)
check("判据写死 ΔVmRSS > 256 MB", "kAbnormalRssKb  = 256LL * 1024LL" in hdr)
check("异常判定只有一处实现(状态机在头文件里, crash_guard 不再自己算一遍)",
      "r->abnormal = mmItemRowAbnormal(r->dSizeKb, r->dRssKb);" in hdr
      and hdr.count("= mmItemRowAbnormal(") == 1
      and "mmItemRowAbnormal" not in cg
      and cg.count("> kAbnormalSizeKb") == 0)
check("阶段名/负载名上限与头文件一致(static_assert 把漂移变成编译错误)",
      "static_assert(kPhaseCap == auroracg::kItemPhaseCap" in cg
      and "static_assert(kItemCap == auroracg::kItemNameCap" in cg)
check("异常行文案 = 本项占用异常, 见该项明细", "[本项占用异常, 见该项明细]" in cg)
check("逐项账目落盘到 native_memory.txt", '"/native_memory.txt"' in cg)
check("崩溃记录里整表打一遍(逐项 + 增量最大的一项)",
      "cgItemMemPutReport(p, end, kItemMemRecordRows)" in handler and "增量最大的一项" in cg)
check("旁路接口已声明(bench.h)", "auroraItemMemoryReport" in read("bench.h"))
check("头文件自足(不 include 任何头)", "#include" not in hdr.split("// ============================================================================")[1])

# ---------------------------------------------------------------- A/B/C) 真代码
emit("")
emit("A/B/C) 用 NDK clang 把 crash_modmap.h 编成无 CRT 的 x86_64 DLL, 再对合成输入断言")
bindir = find_clang()
if bindir is None:
    check("找到 NDK clang/lld-link", False, "没找到: 设 AURORA_OHOS_LLVM_BIN 或检查 D:\\ohos-tools\\sdk\\native\\llvm\\bin")
    emit("")
    emit("=" * 108)
    emit("结论: %d 项检查, %d 项失败(缺少主机编译器 -> 行为断言没能执行)" % (checks, fails))
    emit("=" * 108)
    OUT.close()
    sys.exit(1)

work = os.path.join(tempfile.gettempdir(), "aurora_cg_modmap_hosttest")
if os.path.isdir(work):
    shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
hpath = os.path.join(HERE, "crash_modmap.h").replace("\\", "/")

harness = r'''
#include "%s"
using namespace auroracg;

extern "C" void* memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s;
    for (unsigned long long i = 0; i < n; ++i) dd[i] = ss[i];
    return d;
}
extern "C" void* memset(void* d, int c, unsigned long long n) {
    unsigned char* dd = (unsigned char*)d;
    for (unsigned long long i = 0; i < n; ++i) dd[i] = (unsigned char)c;
    return d;
}

extern "C" int mmt_entry_size() { return (int)sizeof(ModRange); }
extern "C" int mmt_max_mods() { return kMaxMods; }
extern "C" int mmt_path_cap() { return kModPathCap; }

// 合成 maps 文本 -> 定长表
extern "C" int mmt_fill(const char* text, int len, unsigned char* tab, int cap, int* outCount, int* outDropped) {
    ModFill f;
    mmFillBegin(&f, (ModRange*)tab, cap);
    mmFillText(&f, text, len);
    *outCount = f.count;
    *outDropped = f.dropped;
    return f.lines;
}

extern "C" unsigned long long mmt_start(const unsigned char* tab, int i) { return ((const ModRange*)tab)[i].start; }
extern "C" unsigned long long mmt_end(const unsigned char* tab, int i) { return ((const ModRange*)tab)[i].end; }
extern "C" unsigned long long mmt_off(const unsigned char* tab, int i) { return ((const ModRange*)tab)[i].fileOffset; }
extern "C" unsigned long long mmt_base(const unsigned char* tab, int i) { return ((const ModRange*)tab)[i].base; }
extern "C" const char* mmt_path(const unsigned char* tab, int i) { return ((const ModRange*)tab)[i].path; }

// pc/lr 归属整行
extern "C" int mmt_fmt_hit(char* out, int cap, const char* tag, unsigned long long addr,
                           const unsigned char* tab, int n) {
    return mmFormatAddrHit(out, cap, tag, addr, (const ModRange*)tab, n);
}
// 模块表说明一行
extern "C" int mmt_note(char* out, int cap, int recorded, int capLimit, int dropped) {
    return mmFormatTableNote(out, cap, recorded, capLimit, dropped);
}
// 合成栈窗口 -> 候选返回地址
extern "C" int mmt_scan(const unsigned char* bytes, unsigned int len, const unsigned char* tab, int n,
                        unsigned long long* addrs, unsigned int* slots, int outCap, int* stored) {
    return mmScanStack(bytes, len, (const ModRange*)tab, n, addrs, slots, outCap, stored);
}
extern "C" int mmt_fmt_cand(char* out, int cap, unsigned long long addr, unsigned int off,
                            const unsigned char* tab, int n) {
    return mmFormatStackCandidate(out, cap, addr, off, (const ModRange*)tab, n);
}

// ---- 逐项地址空间归因(纯逻辑部分) ----
extern "C" int mmt_item_size() { return (int)sizeof(ItemMemRow); }
// abnormalFlag: -1 = 用写死的判据算; 0/1 = 强制(用来验证判据边界)
extern "C" void mmt_item_make(unsigned char* rowBytes, const char* phase, const char* item, int index, int total,
                              long long sSize, long long sRss, long long eSize, long long eRss,
                              long long eHwm, long long eThreads, long long dSize, long long dRss,
                              int closed, int startOk, int endOk, int abnormalFlag, long long endMs) {
    ItemMemRow* r = (ItemMemRow*)rowBytes;
    mmCopyPath(r->phase, kItemPhaseCap, phase);
    mmCopyPath(r->item, kItemNameCap, item);
    r->index = index; r->total = total;
    r->startSizeKb = sSize; r->startRssKb = sRss;
    r->endSizeKb = eSize; r->endRssKb = eRss; r->endHwmKb = eHwm; r->endThreads = eThreads;
    r->dSizeKb = dSize; r->dRssKb = dRss; r->endMs = endMs;
    r->closed = (closed != 0); r->startOk = (startOk != 0); r->endOk = (endOk != 0);
    r->abnormal = (abnormalFlag < 0) ? mmItemRowAbnormal(dSize, dRss) : (abnormalFlag != 0);
}
extern "C" int mmt_item_row(char* out, int cap, int ordinal, const unsigned char* rowBytes, int indent) {
    char* p = mmItemRowLine(out, out + cap - 1, ordinal, (const ItemMemRow*)rowBytes, indent != 0);
    *p = 0;
    return (int)(p - out);
}
extern "C" int mmt_item_worst(const unsigned char* rowsBytes, int n, long long* ds, long long* dr) {
    return mmItemMemWorst((const ItemMemRow*)rowsBytes, n, ds, dr);
}
extern "C" int mmt_item_report(char* out, int cap, const unsigned char* rowsBytes, int n, int dropped,
                               int maxRows, const char* memPath, int tableCap, int updating) {
    char* p = mmItemMemReport(out, out + cap - 1, (const ItemMemRow*)rowsBytes, n, dropped, maxRows,
                              memPath, tableCap, updating != 0);
    *p = 0;
    return (int)(p - out);
}
extern "C" long long mmt_limit_size() { return kAbnormalSizeKb; }
extern "C" long long mmt_limit_rss() { return kAbnormalRssKb; }

// 逐项账目的状态机(开一项 / 结算一项): 崩溃记录里的"增量"到底怎么算, 由这里断言
static ItemMemSample mk_sample(long long sz, long long rss, long long hwm, long long th, int ok) {
    ItemMemSample s;
    s.sizeKb = sz; s.rssKb = rss; s.hwmKb = hwm; s.threads = th; s.ok = (ok != 0);
    return s;
}
extern "C" int mmt_open(const unsigned char* rowsBytes, int* count, int cap, int* dropped,
                        const char* phase, const char* item, int index, int total,
                        long long sz, long long rss, long long hwm, long long th, int ok) {
    return mmItemMemOpen((ItemMemRow*)rowsBytes, count, cap, dropped, phase, item, index, total,
                         mk_sample(sz, rss, hwm, th, ok));
}
extern "C" int mmt_close(const unsigned char* rowsBytes, int count, long long sz, long long rss,
                         long long hwm, long long th, int ok, long long now) {
    return mmItemMemCloseOpen((ItemMemRow*)rowsBytes, count, mk_sample(sz, rss, hwm, th, ok), now);
}
''' % hpath

src = os.path.join(work, "modmap_hosttest.cpp")
io.open(src, "w", encoding="utf-8", newline="\n").write(harness)

clang = os.path.join(bindir, "clang++.exe")
lld = os.path.join(bindir, "lld-link.exe")
obj = os.path.join(work, "modmap_hosttest.obj")
dll = os.path.join(work, "modmap_hosttest.dll")
exports = ["mmt_entry_size", "mmt_max_mods", "mmt_path_cap", "mmt_fill", "mmt_start", "mmt_end",
           "mmt_off", "mmt_base", "mmt_path", "mmt_fmt_hit", "mmt_note", "mmt_scan", "mmt_fmt_cand",
           "mmt_item_size", "mmt_item_make", "mmt_item_row", "mmt_item_worst", "mmt_item_report",
           "mmt_limit_size", "mmt_limit_rss", "mmt_open", "mmt_close"]
cc = [clang, "--target=x86_64-pc-windows-msvc", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
      "-fno-builtin-memcpy", "-fno-builtin-memset", "-O1", "-c", src, "-o", obj]
lk = [lld, "/dll", "/noentry", "/nodefaultlib", "/machine:x64", "/out:" + dll] + ["/export:" + e for e in exports] + [obj]

emit("     编译: %s" % " ".join(cc[:6]) + " ...")
r1 = subprocess.run(cc, capture_output=True, text=True)
check("crash_modmap.h 能用 NDK clang 独立编译(无头文件依赖)", r1.returncode == 0,
      (r1.stderr or r1.stdout).strip().replace("\n", " | ")[:300])
if r1.returncode != 0:
    OUT.close()
    sys.exit(1)
r2 = subprocess.run(lk, capture_output=True, text=True)
check("无 CRT 链接通过(证明纯逻辑不引用任何运行库)", r2.returncode == 0,
      (r2.stderr or r2.stdout).strip().replace("\n", " | ")[:300])
if r2.returncode != 0:
    OUT.close()
    sys.exit(1)

lib = ctypes.CDLL(dll)
lib.mmt_fill.restype = ctypes.c_int
lib.mmt_fill.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                         ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
lib.mmt_entry_size.restype = ctypes.c_int
lib.mmt_path.restype = ctypes.c_char_p
lib.mmt_path.argtypes = [ctypes.c_void_p, ctypes.c_int]
for nm in ("mmt_start", "mmt_end", "mmt_off", "mmt_base"):
    getattr(lib, nm).restype = ctypes.c_ulonglong
    getattr(lib, nm).argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.mmt_fmt_hit.restype = ctypes.c_int
lib.mmt_fmt_hit.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_ulonglong,
                            ctypes.c_void_p, ctypes.c_int]
lib.mmt_note.restype = ctypes.c_int
lib.mmt_note.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
lib.mmt_scan.restype = ctypes.c_int
lib.mmt_scan.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_int,
                         ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(ctypes.c_uint),
                         ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
lib.mmt_fmt_cand.restype = ctypes.c_int
lib.mmt_fmt_cand.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_ulonglong, ctypes.c_uint,
                             ctypes.c_void_p, ctypes.c_int]

ENTRY = lib.mmt_entry_size()
check("ModRange 是定长 POD(4 个 8 字节字段 + 定长路径)", ENTRY == 4 * 8 + lib.mmt_path_cap(),
      "sizeof=%d, 路径上限=%d" % (ENTRY, lib.mmt_path_cap()))

def newtab(n):
    return (ctypes.c_ulonglong * ((ENTRY // 8 + 1) * n))()

def tabptr(t):
    return ctypes.cast(t, ctypes.c_void_p)

def fill(text, cap):
    t = newtab(max(cap, 1))
    c = ctypes.c_int(0)
    d = ctypes.c_int(0)
    lines = lib.mmt_fill(text.encode("utf-8"), len(text.encode("utf-8")), tabptr(t), cap,
                         ctypes.byref(c), ctypes.byref(d))
    return t, c.value, d.value, lines

def path_of(t, i):
    return lib.mmt_path(tabptr(t), i).decode("utf-8")

LONGNAME = "/data/storage/el1/bundle/" + "d" * 40 + "/libs/arm64/" + "e" * 40 + "/libverysuperlongname.so"
MAPS = "\n".join([
    "7f0000000000-7f0000100000 r-xp 00000000 fd:00 1234 /system/lib64/libc.so",
    "7f0000100000-7f0000300000 r--p 00100000 fd:00 1234 /system/lib64/libc.so",
    "7f1000000000-7f1000020000 r-xp 00000000 fd:00 1 /system/lib64/libc++_shared.so",
    "7f2000000000-7f2000010000 r-xp 00002000 fd:00 2 /system/lib64/libaurorabench.so",
    "7f3000000000-7f3000004000 r-xp 00000000 00:00 0 ",
    "7f4000000000-7f4000001000 r-xp 00000000 00:00 0 [vdso]",
    "7f5000000000-7f5000001000 rw-p 00000000 00:00 0 [anon:stack]",
    "7f6000000000-7f6000010000 r-xp 00000000 fd:00 9 " + LONGNAME,
    "ffffffffffffffff-ffffffffffffff r-xp",
    "not a maps line at all",
]) + "\n"

t, cnt, drop, lines = fill(MAPS, 64)
check("A1 只把可执行段入表(6 段: libc/libc++/bench/匿名/vdso/长路径; r--p 与 rw-p 被忽略)",
      cnt == 6, "入表=%d 丢弃=%d 行数=%d" % (cnt, drop, lines))
check("A2 脏行/malformed 行不影响解析", lines >= 10)
check("A3 段起止/文件偏移解析正确",
      lib.mmt_start(tabptr(t), 0) == 0x7f0000000000 and lib.mmt_end(tabptr(t), 0) == 0x7f0000100000
      and lib.mmt_off(tabptr(t), 0) == 0)
# 入表顺序: 0=libc(r-xp), 1=libc++_shared, 2=libaurorabench(文件偏移 0x2000), 3=匿名, 4=[vdso], 5=超长路径
check("A4 非可执行段(r--p)确实没占位(索引 1 就是 libc++_shared)",
      lib.mmt_start(tabptr(t), 1) == 0x7f1000000000, "idx1 start=%#x" % lib.mmt_start(tabptr(t), 1))
check("A5 装载基址 = start - file_offset(非零偏移)",
      lib.mmt_base(tabptr(t), 2) == 0x7f2000000000 - 0x2000,
      "base=%#x off=%#x" % (lib.mmt_base(tabptr(t), 2), lib.mmt_off(tabptr(t), 2)))
check("A6 空路径(匿名可执行映射)有明确名字", path_of(t, 3) == "(无路径/匿名映射)", path_of(t, 3))
check("A7 [vdso] 这类方括号名字原样保留", path_of(t, 4) == "[vdso]", path_of(t, 4))
check("A8 超长路径截断但保留尾部(文件名还在)",
      path_of(t, 5).startswith("...") and path_of(t, 5).endswith("libverysuperlongname.so")
      and len(path_of(t, 5)) == lib.mmt_path_cap() - 1,
      path_of(t, 5)[:32] + " ... " + path_of(t, 5)[-28:])

# 表满: 上限 4, 有 6 个可执行段 -> 记 4 段 + "还有 2 段没记"
t4, c4, d4, _ = fill(MAPS, 4)
check("A9 数组满就停止并记下'还有 N 段没记'", c4 == 4 and d4 == 2, "入表=%d 没记=%d" % (c4, d4))

def note(*a):
    buf = ctypes.create_string_buffer(512)
    n = lib.mmt_note(buf, 512, *a)
    return buf.value.decode("utf-8")

check("A10 表满时的说明文案", note(4, 4, 2).endswith("还有 2 段没记(表满即停: 没记的段不参与下面的归属判定)"), note(4, 4, 2))
check("A11 没丢段时的说明文案", "没有段被丢弃" in note(6, 128, 0), note(6, 128, 0))
check("A12 空表时的说明文案", note(0, 128, 0).startswith("(模块表为空"), note(0, 128, 0))

def hit(tag, addr, tab=None, n=None):
    if tab is None:
        tab, n = t, cnt
    buf = ctypes.create_string_buffer(512)
    got = lib.mmt_fmt_hit(buf, 512, tag.encode("utf-8"), addr, tabptr(tab), n)
    return buf.value.decode("utf-8"), got

# B) pc 命中
got, n = hit("pc", 0x7f0000000500)
want = ("pc     : pc=0x7f0000000500 落在 /system/lib64/libc.so +0x500"
        "（该模块映射段 0x7f0000000000-0x7f0000100000, 装载基址 0x7f0000000000, 文件偏移 0x0;"
        " +0x… 就是 nm/addr2line 直接可用的 vaddr）")
check("B1 pc 命中: 路径 + 偏移 + 该段范围, 逐字节一致", got == want, "\n        得到: %s" % got)
check("B2 返回值 = 实际写入长度", n == len(want.encode("utf-8")), "n=%d len=%d" % (n, len(want.encode("utf-8"))))

got, _ = hit("lr", 0x7f1000001000)
check("B3 lr 命中 libc++_shared.so(非零基址也正确)",
      got == ("lr     : lr=0x7f1000001000 落在 /system/lib64/libc++_shared.so +0x1000"
              "（该模块映射段 0x7f1000000000-0x7f1000020000, 装载基址 0x7f1000000000, 文件偏移 0x0;"
              " +0x… 就是 nm/addr2line 直接可用的 vaddr）"), got)

got, _ = hit("pc", 0x7f2000000500)
check("B4 文件偏移非零的库: 偏移按装载基址算(nm 可直接反查)",
      "+0x2500（该模块映射段 0x7f2000000000-0x7f2000010000, 装载基址 0x7f1fffffe000, 文件偏移 0x2000" in got, got)

# B) 未命中: 段间空隙(跨段)
got, _ = hit("pc", 0x7f1000020000)
check("B5 跨段(段间空隙, end 是开区间) -> 未命中",
      got == "pc     : pc=0x7f1000020000 不在已记录的任何可执行段内(可能是匿名映射/被 dlopen 后 dlclose 的库)", got)
check("B6 未命中措辞与任务要求逐字一致", "不在已记录的任何可执行段内(可能是匿名映射/被 dlopen 后 dlclose 的库)" in got)

# B) 未命中: 落在非可执行段(libc 的 r--p 段)
got, _ = hit("pc", 0x7f0000150000)
check("B7 落在非可执行段(r--p) -> 未命中(证明只认可执行段)", "不在已记录的任何可执行段内" in got, got)

# B) 空表
empty = newtab(1)
got, _ = hit("pc", 0x7f0000000500, empty, 0)
check("B8 空表时的措辞(自己说明读 maps 失败)", got.endswith("[模块表为空: 安装时读 /proc/self/maps 失败]"), got)

# B) 256 字节小缓冲也不越界
buf = ctypes.create_string_buffer(64)
nn = lib.mmt_fmt_hit(buf, 64, b"pc", 0x7f0000000500, tabptr(t), cnt)
check("B9 小缓冲下不越界且仍以 NUL 结尾", nn <= 63 and buf.raw[nn] == 0, "n=%d" % nn)

# C) 穷人的调用栈
words = [0x7f0000000500,            # 命中 libc
         0x4141414141414141,        # 不命中(垃圾)
         0x7f1000001000,            # 命中 libc++_shared
         0x7f0000150000,            # 落在 r--p(不命中)
         0x0, 0x7f2000000000,       # 命中 libaurorabench(段首)
         0xdeadbeef, 0x7f3000000010]  # 命中匿名段
stack = (ctypes.c_ulonglong * len(words))(*words)
addrs = (ctypes.c_ulonglong * 12)()
slots = (ctypes.c_uint * 12)()
stored = ctypes.c_int(0)
found = lib.mmt_scan(ctypes.cast(stack, ctypes.c_void_p), len(words) * 8, tabptr(t), cnt,
                     addrs, slots, 12, ctypes.byref(stored))
check("C1 合成栈: 命中数 = 落在可执行段里的字数", found == 4 and stored.value == 4,
      "found=%d stored=%d" % (found, stored.value))
check("C2 槽位偏移按字节给出(第 0/2/5/7 个字)",
      [slots[i] for i in range(stored.value)] == [0, 16, 40, 56],
      str([slots[i] for i in range(stored.value)]))
check("C3 候选值原样保留(段首地址也算命中)",
      [addrs[i] for i in range(stored.value)] == [0x7f0000000500, 0x7f1000001000, 0x7f2000000000, 0x7f3000000010])

buf = ctypes.create_string_buffer(400)
lib.mmt_fmt_cand(buf, 400, addrs[0], slots[0], tabptr(t), cnt)
wantc = ("         栈上候选返回地址: /system/lib64/libc.so +0x500"
         "（该模块映射段 0x7f0000000000-0x7f0000100000; 槽位 sp+0x0, 值 0x7f0000000500）")
check("C4 候选行文本逐字节一致(前缀=栈上候选返回地址:) ", buf.value.decode("utf-8") == wantc,
      "\n        得到: %s" % buf.value.decode("utf-8"))
buf2 = ctypes.create_string_buffer(400)
lib.mmt_fmt_cand(buf2, 400, 0x7f1000001000, 16, tabptr(t), cnt)
check("C5 第二个候选指向 libc++_shared.so", "libc++_shared.so +0x1000" in buf2.value.decode("utf-8"),
      buf2.value.decode("utf-8"))
buf3 = ctypes.create_string_buffer(400)
lib.mmt_fmt_cand(buf3, 400, 0x4141414141414141, 8, tabptr(t), cnt)
check("C6 非命中值(不该出现, 兜底措辞)", "不在已记录的任何可执行段内" in buf3.value.decode("utf-8"))

# C) 只列前 N 个
stored2 = ctypes.c_int(0)
found2 = lib.mmt_scan(ctypes.cast(stack, ctypes.c_void_p), len(words) * 8, tabptr(t), cnt,
                      addrs, slots, 2, ctypes.byref(stored2))
check("C7 上限 2 -> 只列 2 个但命中总数照实报", found2 == 4 and stored2.value == 2,
      "found=%d stored=%d" % (found2, stored2.value))

# C) 空表 / 短窗口
stored3 = ctypes.c_int(0)
f3 = lib.mmt_scan(ctypes.cast(stack, ctypes.c_void_p), 7, tabptr(t), cnt, addrs, slots, 12, ctypes.byref(stored3))
check("C8 窗口不足 8 字节时不扫(不越界)", f3 == 0 and stored3.value == 0)
f4 = lib.mmt_scan(ctypes.cast(stack, ctypes.c_void_p), len(words) * 8, tabptr(empty), 0, addrs, slots, 12,
                  ctypes.byref(stored3))
check("C9 空模块表 -> 0 个候选(不死循环)", f4 == 0 and stored3.value == 0)

# ---------------------------------------------------------------- E) 逐项归因
ITEM = lib.mmt_item_size()
lib.mmt_item_make.restype = None
lib.mmt_item_make.argtypes = ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
                              + [ctypes.c_longlong] * 8 + [ctypes.c_int] * 4 + [ctypes.c_longlong])
lib.mmt_item_row.restype = ctypes.c_int
lib.mmt_item_row.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
lib.mmt_item_worst.restype = ctypes.c_int
lib.mmt_item_worst.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_longlong),
                               ctypes.POINTER(ctypes.c_longlong)]
lib.mmt_item_report.restype = ctypes.c_int
lib.mmt_item_report.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
lib.mmt_limit_size.restype = ctypes.c_longlong
lib.mmt_limit_rss.restype = ctypes.c_longlong

def mkrow(phase, item, index, total, sSize, sRss, eSize, eRss, eHwm, eThreads, dSize, dRss,
          closed=1, startOk=1, endOk=1, abnormal=-1, endMs=0):
    buf = (ctypes.c_ulonglong * ((ITEM // 8) + 1))()
    lib.mmt_item_make(ctypes.cast(buf, ctypes.c_void_p), phase.encode("utf-8"), item.encode("utf-8"),
                      index, total, sSize, sRss, eSize, eRss, eHwm, eThreads, dSize, dRss,
                      closed, startOk, endOk, abnormal, endMs)
    return buf

def rowline(ordinal, row, indent=1):
    buf = ctypes.create_string_buffer(512)
    lib.mmt_item_row(buf, 512, ordinal, ctypes.cast(row, ctypes.c_void_p), indent)
    return buf.value.decode("utf-8")

def rowsof(rows):
    big = (ctypes.c_ulonglong * (((ITEM // 8) + 1) * max(len(rows), 1)))()
    for i, r in enumerate(rows):
        ctypes.memmove(ctypes.byref(big, i * ITEM), ctypes.cast(r, ctypes.c_void_p), ITEM)
    return big

def itemreport(rows, dropped=0, maxRows=0, path="/data/app/el2/100/base/com.aurora.bench/haps/entry/files/native_memory.txt",
               tableCap=160, updating=0):
    big = rowsof(rows)
    buf = ctypes.create_string_buffer(32768)
    lib.mmt_item_report(buf, 32768, ctypes.cast(big, ctypes.c_void_p), len(rows), dropped, maxRows,
                        path.encode("utf-8"), tableCap, updating)
    return buf.value.decode("utf-8")

MB = 1024
SIM_ROWS = [
    mkrow("GB7 单核", "File Compression", 1, 16, 1200000, 150000, 1390152, 152432, 168400, 12, 190152, 2432),
    mkrow("GB7 单核", "Text Processing", 4, 16, 1390152, 152432, 1621000, 452900, 512000, 13, 230848, 300468),
    mkrow("GB7 单核", "Asset Compression", 6, 16, 1621000, 452900, 14034764, 202808, 451640, 14, 12413764, -250092),
    mkrow("GB7 单核", "HDR", 9, 16, 14034764, 202808, 14034764, 202808, 451640, 14, 0, 0),
]
r0 = rowline(1, SIM_ROWS[0])
want0 = ('         #1 phase="GB7 单核" item="File Compression" 1/16 VmSize=1390152 kB VmRSS=152432 kB'
         ' VmHWM=168400 kB Threads=12 ΔVmSize=+190152 kB ΔVmRSS=+2432 kB')
check("E1 逐项行文本逐字节一致", r0 == want0, "\n        得到: %s" % r0)

ds = ctypes.c_longlong(0)
dr = ctypes.c_longlong(0)
worst = lib.mmt_item_worst(ctypes.cast(rowsof(SIM_ROWS), ctypes.c_void_p), len(SIM_ROWS),
                           ctypes.byref(ds), ctypes.byref(dr))
check("E2 增量最大的一项 = VmSize 增量最大那一项(#3 Asset Compression)",
      worst == 2 and ds.value == 12413764 and dr.value == -250092,
      "worst=#%d ΔVmSize=%d ΔVmRSS=%d" % (worst + 1, ds.value, dr.value))

rep = itemreport(SIM_ROWS)
check("E3 报告里给出逐项 VmSize/VmRSS + 相对上一项的增量", rep.count("ΔVmSize=") == len(SIM_ROWS) + 1 and "VmRSS=" in rep,
      "行数=%d" % rep.count("\n         #"))
check("E4 报告里点名增量最大的一项", ("增量最大的一项: #3 phase=\"GB7 单核\" item=\"Asset Compression\""
      " ΔVmSize=+12413764 kB ΔVmRSS=-250092 kB => 本项占用异常, 见该项明细") in rep)
check("E5 异常行自带'本项占用异常, 见该项明细'", "#3 phase=\"GB7 单核\" item=\"Asset Compression\"" in rep
      and "[本项占用异常, 见该项明细]" in rep)
check("E6 判据写死 512 MB / 256 MB", lib.mmt_limit_size() == 512 * MB and lib.mmt_limit_rss() == 256 * MB
      and "单项 ΔVmSize > 524288 kB(512 MB) 或 ΔVmRSS > 262144 kB(256 MB)" in rep)
check("E7 也点出 RSS 超标的那一项(Text Processing ΔVmRSS>256MB)",
      '#2 phase="GB7 单核" item="Text Processing" 4/16' in rep and rep.count("[本项占用异常, 见该项明细]") >= 2)

# 判据边界: 写死的是严格大于
lim_s = lib.mmt_limit_size()
lim_r = lib.mmt_limit_rss()
b_at = mkrow("P", "I", 1, 1, 0, 0, 0, 0, 0, 0, lim_s, lim_r)
b_over = mkrow("P", "I", 1, 1, 0, 0, 0, 0, 0, 0, lim_s + 1, 0)
b_over2 = mkrow("P", "I", 1, 1, 0, 0, 0, 0, 0, 0, 0, lim_r + 1)
check("E8 判据是严格大于: 恰好 512MB/256MB 不标, 多 1 kB 才标",
      "[本项占用异常" not in rowline(1, b_at) and "[本项占用异常" in rowline(1, b_over)
      and "[本项占用异常" in rowline(1, b_over2))

# 读不到 / 未结算
b_bad = mkrow("P", "I", 1, 1, -1, -1, -1, -1, -1, -1, 0, 0, closed=1, startOk=0, endOk=0)
check("E9 读不到就写读不到", "VmSize=(读不到)" in rowline(1, b_bad)
      and "ΔVmSize=(未结算/读不到)" in rowline(1, b_bad), rowline(1, b_bad))
b_open = mkrow("P", "I", 1, 1, 123, 45, -1, -1, -1, -1, 0, 0, closed=0)
check("E10 崩溃时正在跑的那一项有明确标注", "崩溃时正在跑该项" in rowline(1, b_open), rowline(1, b_open))

rep_trunc = itemreport(SIM_ROWS, maxRows=2)
check("E11 记录里只列最近 N 项并指路完整文件",
      "(只列最近 2 项, 前面的 2 项见 native_memory.txt)" in rep_trunc and "#1 phase=" not in rep_trunc)
rep_drop = itemreport(SIM_ROWS, dropped=5)
check("E12 表满时写还有 N 项没记", "还有 5 项没记 —— 表满即停" in rep_drop)
rep_upd = itemreport(SIM_ROWS, updating=1)
check("E13 正在更新表时说明最后一行可能不完整", "[逐项表正在更新: 下面最后一行可能不完整]" in rep_upd)
rep_empty = itemreport([])
check("E14 一项都没采到时说明", "(还没有任何一项的采样: 标记点没到过, 或 /proc/self/status 读不到)" in rep_empty)
check("E15 报告末尾给出逐项账目文件路径", "native_memory.txt (进程被 SIGKILL 时, 这份逐项账目是盘上唯一的地址空间归因)" in rep)

# 状态机: 一行一项, 增量 = 结束 - 开始; 结算幂等(同一行不会被算两遍/写两遍)
lib.mmt_open.restype = ctypes.c_int
lib.mmt_open.argtypes = ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.c_int,
                         ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_char_p,
                         ctypes.c_int, ctypes.c_int] + [ctypes.c_longlong] * 4 + [ctypes.c_int])
lib.mmt_close.restype = ctypes.c_int
lib.mmt_close.argtypes = ([ctypes.c_void_p, ctypes.c_int] + [ctypes.c_longlong] * 4
                          + [ctypes.c_int, ctypes.c_longlong])

def mkseq(cap):
    rows = (ctypes.c_ulonglong * (((ITEM // 8) + 1) * cap))()
    return rows, ctypes.c_int(0), ctypes.c_int(0)

def op(rows, cnt, drop, cap, phase, item, index, total, sz, rss, hwm=0, th=12, ok=1):
    return lib.mmt_open(ctypes.cast(rows, ctypes.c_void_p), ctypes.byref(cnt), cap, ctypes.byref(drop),
                        phase.encode("utf-8"), item.encode("utf-8"), index, total, sz, rss, hwm, th, ok)

def cl(rows, cnt, sz, rss, hwm=0, th=12, ok=1, now=0):
    return lib.mmt_close(ctypes.cast(rows, ctypes.c_void_p), cnt.value, sz, rss, hwm, th, ok, now)

rows, cnt, drop = mkseq(4)
r_a = op(rows, cnt, drop, 4, "GB7 单核", "File Compression", 1, 16, 1000000, 100000, 110000, 12, 1)
c_a = cl(rows, cnt, 1010000, 102000, 112000, 13, 1, 111)
r_b = op(rows, cnt, drop, 4, "GB7 单核", "Asset Compression", 3, 16, 1010000, 102000, 112000, 13, 1)
c_b = cl(rows, cnt, 13000000, 300000, 451640, 14, 1, 222)
check("E16 开/结算返回正确的行号", r_a == 0 and c_a == 0 and r_b == 1 and c_b == 1,
      "open=%d close=%d open=%d close=%d" % (r_a, c_a, r_b, c_b))
def rowat(rows, i):
    return ctypes.cast(ctypes.byref(rows, i * ITEM), ctypes.c_void_p)

big = ctypes.cast(rows, ctypes.c_void_p)
line_b = rowline(2, rowat(rows, 1), 1)
check("E17 增量 = 结束值 - 开始值(= 本项造成的增长)",
      "ΔVmSize=+11990000 kB ΔVmRSS=+198000 kB" in line_b and "VmSize=13000000 kB" in line_b, line_b)
check("E18 超过 512 MB 的那一项自动标异常", "[本项占用异常, 见该项明细]" in line_b)
check("E19 结算是幂等的(同一行不会被算两遍 / 写两遍)", cl(rows, cnt, 99999999, 99999999, 0, 14, 1, 333) == -1)
worst2 = lib.mmt_item_worst(big, cnt.value, ctypes.byref(ds), ctypes.byref(dr))
check("E20 增量最大的一项就是 Asset Compression 那一行", worst2 == 1, "worst=#%d" % (worst2 + 1))

rows2, cnt2, drop2 = mkseq(2)
op(rows2, cnt2, drop2, 2, "P", "A", 1, 3, 10, 10)
op(rows2, cnt2, drop2, 2, "P", "B", 2, 3, 20, 20)
r_over = op(rows2, cnt2, drop2, 2, "P", "C", 3, 3, 30, 30)
check("E21 表满后不再入表, 只把'还有几项没记'加一", r_over == -1 and cnt2.value == 2 and drop2.value == 1,
      "count=%d dropped=%d" % (cnt2.value, drop2.value))
rows_bad, cnt_bad, drop_bad = mkseq(2)
op(rows_bad, cnt_bad, drop_bad, 2, "P", "X", 1, 1, -1, -1, -1, -1, 0)
c_bad = cl(rows_bad, cnt_bad, -1, -1, -1, -1, 0, 0)
check("E22 采样读不到时: 行照样结算, 但增量写'未结算/读不到'",
      c_bad == 0 and "ΔVmSize=(未结算/读不到)" in rowline(1, rowat(rows_bad, 0)), rowline(1, rowat(rows_bad, 0)))

# ---------------------------------------------------------------- F) 仿真记录
emit("")
emit("F) 仿真输出: 用同一份真代码(crash_modmap.h)把一条真实形状的崩溃记录拼出来")
SIM_MAPS = "\n".join([
    "5a76000000-5a76200000 r-xp 00000000 fd:00 100 /system/lib64/libc.so",
    "5a76200000-5a76300000 r--p 00200000 fd:00 100 /system/lib64/libc.so",
    "5a77000000-5a77b00000 r-xp 00000000 fd:00 101 /system/lib64/libc++_shared.so",
    "5b27800000-5b27900000 r--p 00000000 fd:00 102 /system/lib64/libaurorabench.so",
    "5b279a4000-5b27d15000 r-xp 001a4000 fd:00 102 /system/lib64/libaurorabench.so",
    "7fdc000000-7fdc021000 rw-p 00000000 00:00 0 [anon:stack]",
]) + "\n"
simtab, simcnt, simdrop, _ = fill(SIM_MAPS, 128)
SIM_PC = 0x5a77adadd0          # 真机那一次的 pc(比 self_base 低约 2.9 GB)
SIM_LR = 0x5a7610f2c4
sim_pc, _ = hit("pc", SIM_PC, simtab, simcnt)
sim_lr, _ = hit("lr", SIM_LR, simtab, simcnt)
sim_words = [SIM_PC, 0x5b279b2000, SIM_LR, 0xdeadbeefdeadbeef, 0x5a77500123, 0x0, 0x5b279a4000, 0x7fdc000010]
sim_stack = (ctypes.c_ulonglong * len(sim_words))(*sim_words)
saddrs = (ctypes.c_ulonglong * 12)()
sslots = (ctypes.c_uint * 12)()
sstored = ctypes.c_int(0)
sim_found = lib.mmt_scan(ctypes.cast(sim_stack, ctypes.c_void_p), len(sim_words) * 8, tabptr(simtab), simcnt,
                         saddrs, sslots, 12, ctypes.byref(sstored))
sim_cands = []
for i in range(sstored.value):
    cb = ctypes.create_string_buffer(400)
    lib.mmt_fmt_cand(cb, 400, saddrs[i], sslots[i], tabptr(simtab), simcnt)
    sim_cands.append(cb.value.decode("utf-8"))

rec = []
rec.append("----- AURORA NATIVE CRASH -----")
rec.append("when   : 2026-10-12T09:41:07.318Z  epoch_ms=1791798067318")
rec.append("signal : 11 SIGSEGV  si_code=1 SEGV_MAPERR(地址未映射=野指针/越界)  si_addr=0x18")
rec.append("thread : pid=24817 tid=24853 (工作线程: 负载线程崩的)")
rec.append("pc/lr  : pc=%s lr=%s sp=0x7fdc01a3b0" % (hex(SIM_PC), hex(SIM_LR)))
rec.append("        (用未 strip 的 libaurorabench.so 按 pc/lr 反查符号)")
rec.append("module : pc 不在 libaurorabench.so 的代码段内 => 崩在系统库/运行时/其它 .so"
           " [self_base=0x5b278c1000 text=0x5b279a4000-0x5b27d15000]")
rec.append("modmap : " + note(simcnt, lib.mmt_max_mods(), simdrop)
           + "  刷新次数=37 最近刷新=2026-10-12T09:41:07.201Z")
rec.append(sim_pc)
rec.append(sim_lr)
rec.append("        (pc 不在我们的 .so 里 ≠ 不是我们的 bug: 我们的 std::string / operator new 住在"
           " libc++_shared.so, 堆被写坏之后最先炸的通常是 libc.so 里的 malloc/free。上面这两行 +"
           " 下一条的栈上候选才够定罪或洗清)")
rec.extend(itemreport(SIM_ROWS, dropped=0, maxRows=64).rstrip("\n").split("\n"))
rec.append('item   : phase="GB7 单核" item="HDR" index=11/16 marked=2026-10-12T09:41:06.902Z age_ms=416')
rec.append("memory : VmPeak=14568144 kB VmHWM(RSS峰值)=451640 kB VmRSS=202808 kB VmSize=14034764 kB Threads=14")
rec.append("at_item: rss=202804 kB hwm=451604 kB size=14034700 kB threads=14"
           " sampled=2026-10-12T09:41:06.880Z age_ms=438  (ArkTS 调 sampleMemory() 采的, 用于判断是否逼近内存上限)")
rec.append("source : crash_guard@libaurorabench.so (仅记录, 不改负载)")
rec.append("stack  : 穷人的调用栈(粗扫崩溃线程栈上的 8 字节槽位, 落在已记录可执行段里的值当成候选返回地址;"
           " 只是候选, 不是精确回溯, 不依赖 .eh_frame/展开器)")
rec.extend(sim_cands)
rec.append("         共 %d 个候选(只列前 12 个, 本次列出 %d 个; 窗口 = sp 起 2048 字节)."
           " 对照看看: 命中 libaurorabench.so 的候选 = 我们的调用链; 命中 libc++_shared/libc 的候选 ="
           " 标准库内部(常见于堆被写坏后的 free)" % (sim_found, sstored.value))
rec.append("----- END AURORA NATIVE CRASH -----")
sample = "\n".join(rec) + "\n"
simfile = os.path.join(HERE, "verify_modmap_simulated_crash.txt")
io.open(simfile, "w", encoding="utf-8", newline="\n").write(sample)
for ln in rec:
    emit("     " + ln)
check("F1 仿真记录里 pc 落在 libc++_shared.so(真机那一次的 pc 形状)", "libc++_shared.so +0xadadd0" in sim_pc, sim_pc)
check("F2 仿真记录里 lr 落在 libc.so", "/system/lib64/libc.so +0x10f2c4" in sim_lr, sim_lr)
check("F3 仿真记录里同时给出我们自己的调用链候选(段首地址也算)",
      any("libaurorabench.so" in c for c in sim_cands), str(len(sim_cands)))
emit("     仿真记录已写入: %s" % simfile)

emit("     主机 DLL: %s" % dll)
emit("     (脚本生成的测试宿主: %s)" % src)

emit("")
emit("=" * 108)
emit("结论: %d 项检查, %d 项失败" % (checks, fails))
emit("  * A 段证明模块表解析(命中/非可执行段/空路径/方括号名/超长截断/表满计数)正确;")
emit("  * B 段证明 pc/lr 归属三种结果(命中 / 段间空隙未命中 / 非可执行段未命中)与措辞逐字节正确;")
emit("  * C 段证明穷人的调用栈(候选计数/槽位/上限截断/每行文本)正确;")
emit("  * D 段证明处理器里没有任何动态链接器/分配调用, 且逐项归因接在既有标记点上、判据写死。")
emit("  仍未验证的: 真机上的实际 pc/lr/栈内容(没有设备), 以及 native_memory.txt 的真机落盘效果。")
emit("=" * 108)
OUT.close()
sys.exit(1 if fails else 0)
