// ============================================================================
//  crash_modmap.h — 「模块表」纯逻辑: /proc/self/maps 解析 + 地址归属 + 穷人的栈扫描
//
//  为什么单独一个头文件(而不是直接写在 crash_guard.cpp 里):
//    这几段逻辑必须能在离线主机上被真刀真枪地断言 ——
//    真机不在手上(见任务背景), 所以 entry/src/main/cpp/verify_crash_modmap.py 会把
//    本头文件的同一份源码用 NDK 自带的 clang 编成一个无 CRT 的 x86_64 DLL, 再从
//    Python(ctypes) 里对合成的 maps 文本 / 合成的栈字节调用它并断言输出。
//    因此 crash_guard.cpp 里跑的, 和断言脚本里跑的是同一份代码, 不是"照着抄一遍"。
//
//  为此本头文件遵守两条铁律:
//    1) 不 include 任何头文件(连 <stdint.h> 都不要): 只用语言内建类型;
//    2) 不碰任何全局状态、不做任何系统调用、不做任何分配 —— 纯字节进、纯字节出。
//       (信号处理器里也要求这一点: 只有整数比较和内存读写。)
//
//  数据布局: 每个可执行段一条 ModRange(定长, 路径截断到固定长度)。
//    start/end   : 该可执行映射段的 [start,end)(maps 第 1 列)
//    fileOffset  : maps 第 3 列(该段在文件里的偏移)
//    base        : start - fileOffset = 模块装载基址(nm/addr2line 的 0 基准)
//    path        : 该段的路径(太长时保留"文件名所在的尾部", 头部用 "..." 标记截断)
//  于是任意地址 addr 的归属偏移 = addr - base, 这个数就是 nm 里的 vaddr,
//  可以直接 `llvm-addr2line -e <库> 0x<偏移>` / `llvm-nm` 反查符号 —— 这正是
//  「崩在哪个库、哪条调用链」这一步的关键(真机现场: pc 比 self_base 低 2.9 GB,
//  只写"不在我们的 .so 里"区分不了 libc++_shared / libc / ArkTS 运行时)。
// ============================================================================
#ifndef AURORA_CRASH_MODMAP_H
#define AURORA_CRASH_MODMAP_H

namespace auroracg {

// 定长模块表: 数组满了就停止并记下"还有 N 段没记"(见 ModFill::dropped)。
// 128 段 × 128 字节 ≈ 16 KB 静态空间 —— 真机上 ArkTS 应用的 .so 数量在几十到一百多,
// 留一倍余量, 同时保证表是一个固定大小的 POD(处理器里可以随便扫, 无锁无分配)。
constexpr int kMaxMods    = 128;
constexpr int kModPathCap = 96;

struct ModRange {
    unsigned long long start;
    unsigned long long end;
    unsigned long long fileOffset;
    unsigned long long base;
    char path[kModPathCap];
};

// "填表"的增量状态(只读一次 maps 的流式解析用)
struct ModFill {
    ModRange* tab;
    int cap;
    int count;
    int dropped;
    int lines;
};

inline int mmHexVal(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

// 定长路径拷贝: 放得下就原样; 放不下则保留尾部(文件名在尾部)并前置 "..." 标记截断。
inline void mmCopyPath(char* dst, int cap, const char* src)
{
    if (dst == nullptr || cap <= 0) {
        return;
    }
    dst[0] = '\0';
    if (src == nullptr) {
        return;
    }
    int n = 0;
    while (src[n] != '\0') {
        ++n;
    }
    if (n <= cap - 1) {
        int i = 0;
        for (; i < n; ++i) {
            dst[i] = src[i];
        }
        dst[i] = '\0';
        return;
    }
    if (cap < 5) {
        return;   // 连 "..." 都放不下: 只能是空串
    }
    const int keep = cap - 1 - 3;
    dst[0] = '.';
    dst[1] = '.';
    dst[2] = '.';
    const int from = n - keep;
    for (int k = 0; k < keep; ++k) {
        dst[3 + k] = src[from + k];
    }
    dst[3 + keep] = '\0';
}

// 解析一行 /proc/self/maps: "start-end perms offset dev inode path"
// 只接受可执行段: perms 第 3 个字符必须是 'x', 且 end > start。
// 空路径(匿名映射)统一记成 "(无路径/匿名映射)"; "[vdso]" 这类方括号名字原样保留。
inline bool mmParseExecLine(const char* line, ModRange* out)
{
    if (line == nullptr || out == nullptr) {
        return false;
    }
    int i = 0;
    unsigned long long start = 0;
    int digits = 0;
    for (; line[i] != '\0'; ++i) {
        const int v = mmHexVal(line[i]);
        if (v < 0) {
            break;
        }
        start = (start << 4) | (unsigned long long)v;
        ++digits;
    }
    if (digits == 0 || line[i] != '-') {
        return false;
    }
    ++i;
    unsigned long long end = 0;
    digits = 0;
    for (; line[i] != '\0'; ++i) {
        const int v = mmHexVal(line[i]);
        if (v < 0) {
            break;
        }
        end = (end << 4) | (unsigned long long)v;
        ++digits;
    }
    if (digits == 0 || end <= start) {
        return false;
    }
    // perms: 跳过空白后紧跟 "rwxp"
    while (line[i] == ' ' || line[i] == '\t') {
        ++i;
    }
    if (line[i] == '\0' || line[i + 1] == '\0' || line[i + 2] == '\0' || line[i + 2] != 'x') {
        return false;
    }
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') {
        ++i;   // 跳过整个 perms 字段(不做 i+=4 的越界假设)
    }
    // 文件偏移(maps 第 3 列, hex)
    while (line[i] == ' ' || line[i] == '\t') {
        ++i;
    }
    unsigned long long off = 0;
    for (; line[i] != '\0'; ++i) {
        const int v = mmHexVal(line[i]);
        if (v < 0) {
            break;
        }
        off = (off << 4) | (unsigned long long)v;
    }
    // 跳过 dev 与 inode 两列
    for (int skip = 0; skip < 2; ++skip) {
        while (line[i] == ' ' || line[i] == '\t') {
            ++i;
        }
        while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
    }
    while (line[i] == ' ' || line[i] == '\t') {
        ++i;
    }
    const char* path = line + i;   // 可能是空串: 匿名/无路径映射
    if (path[0] == '\0') {
        path = "(无路径/匿名映射)";
    }
    out->start = start;
    out->end = end;
    out->fileOffset = off;
    out->base = (off <= start) ? (start - off) : start;   // 畸形行不让它下溢
    mmCopyPath(out->path, kModPathCap, path);
    return true;
}

inline void mmFillBegin(ModFill* f, ModRange* tab, int cap)
{
    if (f == nullptr) {
        return;
    }
    f->tab = tab;
    f->cap = (cap >= 0) ? cap : 0;
    f->count = 0;
    f->dropped = 0;
    f->lines = 0;
}

// 逐行喂给填表器(流式读 maps 用; 处理器的兄弟函数不调用它)。
// 表满之后继续数还有多少可执行段没记 —— 这就是"还有 N 段没记"。
inline void mmFillLine(ModFill* f, const char* line)
{
    if (f == nullptr || f->tab == nullptr) {
        return;
    }
    ++f->lines;
    ModRange tmp;
    if (!mmParseExecLine(line, &tmp)) {
        return;
    }
    if (f->count >= f->cap) {
        ++f->dropped;
        return;
    }
    ModRange* d = &f->tab[f->count];
    d->start = tmp.start;
    d->end = tmp.end;
    d->fileOffset = tmp.fileOffset;
    d->base = tmp.base;
    mmCopyPath(d->path, kModPathCap, tmp.path);
    ++f->count;
}

// 一次性喂一整块文本(离线断言脚本用; 真机构建里同样可用, 只是走的是流式分支)
inline void mmFillText(ModFill* f, const char* text, int len)
{
    char line[512];
    int n = 0;
    for (int i = 0; i < len; ++i) {
        const char c = text[i];
        if (c == '\n') {
            line[n] = '\0';
            mmFillLine(f, line);
            n = 0;
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = c;
        }
    }
    if (n > 0) {
        line[n] = '\0';
        mmFillLine(f, line);
    }
}

// 处理器里的归属判定: 纯内存线性扫描, 零系统调用
// (不能用 dladdr / dl_iterate_phdr / 重新读 maps: 它们会碰动态链接器的锁,
//  而崩溃现场很可能正是某个线程死在那个锁上。)
inline int mmLookup(const ModRange* tab, int n, unsigned long long addr)
{
    if (tab == nullptr || addr == 0) {
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        if (addr >= tab[i].start && addr < tab[i].end) {
            return i;
        }
    }
    return -1;
}

// ------------------------------ 无 libc 的格式化 -----------------------------
// (与 crash_guard.cpp 里的 cgPut* 同源同风格; 这里自己再放一份, 是为了让本头文件
//  完全自足 —— 离线断言脚本能单独编译它。)
inline char* mmPutCh(char* p, char* end, char c)
{
    if (p < end) {
        *p++ = c;
    }
    return p;
}

inline char* mmPutStr(char* p, char* end, const char* s)
{
    if (s == nullptr) {
        return p;
    }
    while (*s != '\0' && p < end) {
        *p++ = *s++;
    }
    return p;
}

inline char* mmPutHex(char* p, char* end, unsigned long long v)
{
    static const char kHex[] = "0123456789abcdef";
    bool lead = true;
    p = mmPutStr(p, end, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        const unsigned int d = (unsigned int)((v >> (unsigned)i) & 0xFULL);
        if (lead && d == 0U && i != 0) {
            continue;
        }
        lead = false;
        p = mmPutCh(p, end, kHex[d]);
    }
    return p;
}

inline char* mmPutDec(char* p, char* end, long long v)
{
    char tmp[24];
    int n = 0;
    unsigned long long u;
    if (v < 0) {
        p = mmPutCh(p, end, '-');
        u = (unsigned long long)(-(v + 1)) + 1ULL;
    } else {
        u = (unsigned long long)v;
    }
    do {
        tmp[n++] = (char)('0' + (int)(u % 10ULL));
        u /= 10ULL;
    } while (u != 0ULL && n < (int)sizeof(tmp));
    while (n > 0) {
        p = mmPutCh(p, end, tmp[--n]);
    }
    return p;
}

inline char* mmPutKey(char* p, char* end, const char* key)
{
    // 与记录里其它字段一致: 关键字左对齐到 7 列 + " : "
    int n = 0;
    p = mmPutStr(p, end, key);
    if (key != nullptr) {
        while (key[n] != '\0') {
            ++n;
        }
    }
    for (int i = n; i < 7; ++i) {
        p = mmPutCh(p, end, ' ');
    }
    p = mmPutStr(p, end, ": ");   // 与记录里既有字段同款: 关键字左对齐 7 列 + ": "
    return p;
}

// "modmap : <note>" 里的那段说明(定长表 + 溢出计数)
inline int mmFormatTableNote(char* out, int cap, int recorded, int capLimit, int dropped)
{
    if (out == nullptr || cap <= 1) {
        return 0;
    }
    char* p = out;
    char* end = out + cap - 1;
    if (recorded <= 0) {
        p = mmPutStr(p, end, "(模块表为空: 安装时读 /proc/self/maps 失败, 或进程没有任何可执行映射)");
    } else if (dropped <= 0) {
        p = mmPutStr(p, end, "已记录 ");
        p = mmPutDec(p, end, (long long)recorded);
        p = mmPutStr(p, end, " 个可执行段(定长表上限 ");
        p = mmPutDec(p, end, (long long)capLimit);
        p = mmPutStr(p, end, "), 没有段被丢弃(全部可执行段都已入表)");
    } else {
        p = mmPutStr(p, end, "已记录 ");
        p = mmPutDec(p, end, (long long)recorded);
        p = mmPutStr(p, end, " 个可执行段(定长表上限 ");
        p = mmPutDec(p, end, (long long)capLimit);
        p = mmPutStr(p, end, "), 还有 ");
        p = mmPutDec(p, end, (long long)dropped);
        p = mmPutStr(p, end, " 段没记(表满即停: 没记的段不参与下面的归属判定)");
    }
    *p = '\0';
    return (int)(p - out);
}

// 一行 "pc/lr 落在哪个模块": 命中 / 未命中 两种措辞都在这里定死, 离线脚本逐字节断言它。
inline int mmFormatAddrHit(char* out, int cap, const char* tag, unsigned long long addr,
                           const ModRange* tab, int n)
{
    if (out == nullptr || cap <= 1) {
        return 0;
    }
    char* p = out;
    char* end = out + cap - 1;
    p = mmPutKey(p, end, tag);
    p = mmPutStr(p, end, tag);
    p = mmPutCh(p, end, '=');
    p = mmPutHex(p, end, addr);
    const int idx = mmLookup(tab, n, addr);
    if (idx >= 0) {
        p = mmPutStr(p, end, " 落在 ");
        p = mmPutStr(p, end, tab[idx].path);
        p = mmPutCh(p, end, ' ');
        p = mmPutCh(p, end, '+');
        p = mmPutHex(p, end, addr - tab[idx].base);
        p = mmPutStr(p, end, "\xEF\xBC\x88");   // 全角左括号(项目里其它中文行同风格)
        p = mmPutStr(p, end, "该模块映射段 ");
        p = mmPutHex(p, end, tab[idx].start);
        p = mmPutCh(p, end, '-');
        p = mmPutHex(p, end, tab[idx].end);
        p = mmPutStr(p, end, ", 装载基址 ");
        p = mmPutHex(p, end, tab[idx].base);
        p = mmPutStr(p, end, ", 文件偏移 ");
        p = mmPutHex(p, end, tab[idx].fileOffset);
        p = mmPutStr(p, end, "; +0x… 就是 nm/addr2line 直接可用的 vaddr\xEF\xBC\x89");
    } else {
        p = mmPutStr(p, end, " 不在已记录的任何可执行段内(可能是匿名映射/被 dlopen 后 dlclose 的库)");
        if (n <= 0) {
            p = mmPutStr(p, end, " [模块表为空: 安装时读 /proc/self/maps 失败]");
        }
    }
    *p = '\0';
    return (int)(p - out);
}

// 穷人的调用栈: 扫一遍栈上的 8 字节槽位, 凡是落在可执行段里的值都算候选返回地址。
// 不依赖 .eh_frame / 不依赖展开器 —— 纯内存扫描, async-signal-safe。
// 返回命中总数(可能大于 outCap), *outStored 回填实际写进数组的个数。
inline int mmScanStack(const unsigned char* bytes, unsigned int len, const ModRange* tab, int n,
                       unsigned long long* outAddrs, unsigned int* outSlots, int outCap, int* outStored)
{
    int found = 0;
    int stored = 0;
    if (outStored != nullptr) {
        *outStored = 0;
    }
    if (bytes == nullptr || tab == nullptr || n <= 0 || outCap <= 0) {
        return 0;
    }
    for (unsigned int off = 0; off + 8 <= len; off += 8) {
        unsigned long long v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | (unsigned long long)bytes[off + i];   // aarch64 小端
        }
        if (mmLookup(tab, n, v) < 0) {
            continue;
        }
        ++found;
        if (stored < outCap && outAddrs != nullptr) {
            outAddrs[stored] = v;
            if (outSlots != nullptr) {
                outSlots[stored] = off;
            }
            ++stored;
        }
    }
    if (outStored != nullptr) {
        *outStored = stored;
    }
    return found;
}

// 一条候选返回地址的整行文本(前缀按任务要求写死成"栈上候选返回地址: ")
inline int mmFormatStackCandidate(char* out, int cap, unsigned long long addr, unsigned int slotOff,
                                  const ModRange* tab, int n)
{
    if (out == nullptr || cap <= 1) {
        return 0;
    }
    char* p = out;
    char* end = out + cap - 1;
    p = mmPutStr(p, end, "         栈上候选返回地址: ");
    const int idx = mmLookup(tab, n, addr);
    if (idx >= 0) {
        p = mmPutStr(p, end, tab[idx].path);
        p = mmPutCh(p, end, ' ');
        p = mmPutCh(p, end, '+');
        p = mmPutHex(p, end, addr - tab[idx].base);
        p = mmPutStr(p, end, "\xEF\xBC\x88");
        p = mmPutStr(p, end, "该模块映射段 ");
        p = mmPutHex(p, end, tab[idx].start);
        p = mmPutCh(p, end, '-');
        p = mmPutHex(p, end, tab[idx].end);
        p = mmPutStr(p, end, "; 槽位 sp+");
        p = mmPutHex(p, end, (unsigned long long)slotOff);
        p = mmPutStr(p, end, ", 值 ");
        p = mmPutHex(p, end, addr);
        p = mmPutStr(p, end, "\xEF\xBC\x89");
    } else {
        p = mmPutStr(p, end, "? (值 ");
        p = mmPutHex(p, end, addr);
        p = mmPutStr(p, end, " 不在已记录的任何可执行段内)");
    }
    *p = '\0';
    return (int)(p - out);
}

// ===========================================================================
//  逐项地址空间归因的行格式(纯逻辑, 离线可断言)
//
//  真机现场: memory 行 VmSize=14034764 kB(14 GB)、VmPeak=14568144 kB, 而 VmRSS 只有
//  202808 kB(RSS 峰值 451640 kB) —— 14 GB 虚拟地址空间远超平板 11.5 GB 物理内存,
//  但记录里没有任何东西能说明是哪一项撑起来的。这一节把"逐项账目"的渲染与判据
//  也做成纯函数, 于是离线脚本能拿合成数据断言"增量最大的一项/异常标记"到底怎么算出来。
//  采样与状态机在 crash_guard.cpp(那里要读 /proc/self/status), 这里只管"怎么算、怎么写"。
// ===========================================================================
constexpr int kItemPhaseCap = 48;   // 必须与 crash_guard.cpp 的 kPhaseCap 一致(那边有 static_assert)
constexpr int kItemNameCap  = 96;   // 必须与 crash_guard.cpp 的 kItemCap  一致
constexpr long long kAbnormalSizeKb = 512LL * 1024LL;   // 512 MB
constexpr long long kAbnormalRssKb  = 256LL * 1024LL;   // 256 MB

struct ItemMemRow {
    char phase[kItemPhaseCap];
    char item[kItemNameCap];
    int index;
    int total;
    long long startSizeKb;   // 本项开始(= 上一个标记点)时
    long long startRssKb;
    long long endSizeKb;     // 本项结束(= 下一个标记点)时
    long long endRssKb;
    long long endHwmKb;
    long long endThreads;
    long long dSizeKb;       // 相对上一项的增量(= 本项造成的增长)
    long long dRssKb;
    long long endMs;
    bool startOk;
    bool endOk;
    bool closed;
    bool abnormal;
};

// 一次 /proc/self/status 采样(普通上下文读出来的四个数)
struct ItemMemSample {
    long long sizeKb;      // VmSize
    long long rssKb;       // VmRSS
    long long hwmKb;       // VmHWM(RSS 峰值)
    long long threads;     // Threads(线程数 —— 每根线程栈都直接占地址空间, 是 14 GB 的常见元凶)
    bool ok;               // 读到了没有
};

// 判据写死(不许调): 单项 ΔVmSize > 512 MB 或 ΔVmRSS > 256 MB => "本项占用异常, 见该项明细"
inline bool mmItemRowAbnormal(long long dSizeKb, long long dRssKb)
{
    return (dSizeKb > kAbnormalSizeKb) || (dRssKb > kAbnormalRssKb);
}

// 开一项(标记点到来时调): 开始值 = 本次采样。表满则不动表, 只把 *dropped 加一。
// 返回新行下标; -1 = 表满。
inline int mmItemMemOpen(ItemMemRow* rows, int* count, int cap, int* dropped,
                         const char* phase, const char* item, int index, int total,
                         const ItemMemSample& s)
{
    if (rows == nullptr || count == nullptr) {
        return -1;
    }
    if (*count >= cap) {
        if (dropped != nullptr) {
            ++(*dropped);
        }
        return -1;
    }
    ItemMemRow* r = &rows[*count];
    mmCopyPath(r->phase, kItemPhaseCap, phase);
    mmCopyPath(r->item, kItemNameCap, item);
    r->index = index;
    r->total = total;
    r->startSizeKb = s.sizeKb;
    r->startRssKb = s.rssKb;
    r->endSizeKb = -1;
    r->endRssKb = -1;
    r->endHwmKb = -1;
    r->endThreads = -1;
    r->dSizeKb = 0;
    r->dRssKb = 0;
    r->endMs = 0;
    r->startOk = s.ok;
    r->endOk = false;
    r->closed = false;
    r->abnormal = false;
    ++(*count);
    return *count - 1;
}

// 结算"正在跑的那一项"(下一个标记点 / 空闲标记到来时调): 结束值 = 本次采样,
// 增量 = 结束值 - 开始值(= 本项造成的增长)。幂等: 已经结算过的行再调返回 -1,
// 于是"同一行被写两遍"这种事在结构上就不可能发生。
// 返回被结算的行下标; -1 = 没有正在跑的行(表空 / 上一行已结算 / 表满之后)。
inline int mmItemMemCloseOpen(ItemMemRow* rows, int count, const ItemMemSample& s, long long now)
{
    if (rows == nullptr || count <= 0) {
        return -1;
    }
    ItemMemRow* r = &rows[count - 1];
    if (r->closed) {
        return -1;
    }
    r->endOk = s.ok;
    r->endSizeKb = s.sizeKb;
    r->endRssKb = s.rssKb;
    r->endHwmKb = s.hwmKb;
    r->endThreads = s.threads;
    if (r->startOk && s.ok) {
        r->dSizeKb = s.sizeKb - r->startSizeKb;
        r->dRssKb = s.rssKb - r->startRssKb;
        r->abnormal = mmItemRowAbnormal(r->dSizeKb, r->dRssKb);
    } else {
        r->dSizeKb = 0;
        r->dRssKb = 0;
        r->abnormal = false;
    }
    r->endMs = now;
    r->closed = true;
    return count - 1;
}

// 一行逐项账目(indent=true 时缩进 9 格, 用于崩溃记录; 文件里不缩进)
inline char* mmItemRowLine(char* p, char* end, int ordinal, const ItemMemRow* r, bool indent)
{
    if (r == nullptr) {
        return p;
    }
    if (indent) {
        p = mmPutStr(p, end, "         ");
    }
    p = mmPutCh(p, end, '#');
    p = mmPutDec(p, end, (long long)ordinal);
    p = mmPutStr(p, end, " phase=\"");
    p = mmPutStr(p, end, r->phase);
    p = mmPutStr(p, end, "\" item=\"");
    p = mmPutStr(p, end, r->item);
    p = mmPutStr(p, end, "\" ");
    p = mmPutDec(p, end, (long long)r->index);
    p = mmPutCh(p, end, '/');
    p = mmPutDec(p, end, (long long)r->total);
    p = mmPutStr(p, end, " VmSize=");
    if (r->closed) {
        if (r->endOk) {
            p = mmPutDec(p, end, r->endSizeKb);
            p = mmPutStr(p, end, " kB");
        } else {
            p = mmPutStr(p, end, "(读不到)");
        }
    } else if (r->startOk) {
        p = mmPutDec(p, end, r->startSizeKb);
        p = mmPutStr(p, end, " kB");
    } else {
        p = mmPutStr(p, end, "(读不到)");
    }
    p = mmPutStr(p, end, " VmRSS=");
    if (r->closed) {
        if (r->endOk) {
            p = mmPutDec(p, end, r->endRssKb);
            p = mmPutStr(p, end, " kB");
        } else {
            p = mmPutStr(p, end, "(读不到)");
        }
    } else if (r->startOk) {
        p = mmPutDec(p, end, r->startRssKb);
        p = mmPutStr(p, end, " kB");
    } else {
        p = mmPutStr(p, end, "(读不到)");
    }
    p = mmPutStr(p, end, " VmHWM=");
    if (r->endHwmKb >= 0) {
        p = mmPutDec(p, end, r->endHwmKb);
        p = mmPutStr(p, end, " kB");
    } else {
        p = mmPutStr(p, end, "(读不到)");
    }
    p = mmPutStr(p, end, " Threads=");
    if (r->endThreads >= 0) {
        p = mmPutDec(p, end, r->endThreads);
    } else {
        p = mmPutStr(p, end, "(读不到)");
    }
    p = mmPutStr(p, end, " ΔVmSize=");
    if (r->closed && r->startOk && r->endOk) {
        if (r->dSizeKb > 0) {
            p = mmPutCh(p, end, '+');
        }
        p = mmPutDec(p, end, r->dSizeKb);
        p = mmPutStr(p, end, " kB");
    } else {
        p = mmPutStr(p, end, "(未结算/读不到)");
    }
    p = mmPutStr(p, end, " ΔVmRSS=");
    if (r->closed && r->startOk && r->endOk) {
        if (r->dRssKb > 0) {
            p = mmPutCh(p, end, '+');
        }
        p = mmPutDec(p, end, r->dRssKb);
        p = mmPutStr(p, end, " kB");
    } else {
        p = mmPutStr(p, end, "(未结算/读不到)");
    }
    if (r->abnormal) {
        p = mmPutStr(p, end, " [本项占用异常, 见该项明细]");
    }
    if (!r->closed) {
        p = mmPutStr(p, end, " [崩溃时正在跑该项: 结束时的采样还没拿到]");
    }
    return p;
}

// 增量最大的一项(只在"两个采样都有效且已结算"的行里挑; 先比 ΔVmSize, 再比 ΔVmRSS)
inline int mmItemMemWorst(const ItemMemRow* rows, int n, long long* outDSize, long long* outDRss)
{
    int best = -1;
    long long bs = 0;
    long long br = 0;
    for (int i = 0; i < (rows != nullptr ? n : 0); ++i) {
        const ItemMemRow& r = rows[i];
        if (!r.closed || !r.startOk || !r.endOk) {
            continue;
        }
        if (best < 0 || r.dSizeKb > bs || (r.dSizeKb == bs && r.dRssKb > br)) {
            best = i;
            bs = r.dSizeKb;
            br = r.dRssKb;
        }
    }
    if (outDSize != nullptr) { *outDSize = bs; }
    if (outDRss != nullptr) { *outDRss = br; }
    return best;
}

// 整张逐项账目表(崩溃记录与 C 接口共用; maxRows<=0 = 全列)
inline char* mmItemMemReport(char* p, char* end, const ItemMemRow* rows, int n, int dropped,
                             int maxRows, const char* memPath, int tableCap, bool updating)
{
    p = mmPutStr(p, end, "itemmem: 逐项地址空间归因(旁路: 每个 item 标记点读一次 /proc/self/status,"
                         " 不计分)\n");
    if (n <= 0) {
        p = mmPutStr(p, end, "         (还没有任何一项的采样: 标记点没到过, 或 /proc/self/status 读不到)\n");
        return p;
    }
    if (updating) {
        p = mmPutStr(p, end, "         [逐项表正在更新: 下面最后一行可能不完整]\n");
    }
    int from = 0;
    if (maxRows > 0 && n > maxRows) {
        from = n - maxRows;
        p = mmPutStr(p, end, "         (只列最近 ");
        p = mmPutDec(p, end, (long long)maxRows);
        p = mmPutStr(p, end, " 项, 前面的 ");
        p = mmPutDec(p, end, (long long)from);
        p = mmPutStr(p, end, " 项见 native_memory.txt)\n");
    }
    for (int i = from; i < n; ++i) {
        p = mmItemRowLine(p, end, i + 1, &rows[i], true);
        p = mmPutCh(p, end, '\n');
    }
    if (dropped > 0) {
        p = mmPutStr(p, end, "         (逐项表已满 ");
        p = mmPutDec(p, end, (long long)tableCap);
        p = mmPutStr(p, end, " 项, 还有 ");
        p = mmPutDec(p, end, (long long)dropped);
        p = mmPutStr(p, end, " 项没记 —— 表满即停)\n");
    }
    long long ds = 0;
    long long dr = 0;
    const int worst = mmItemMemWorst(rows, n, &ds, &dr);
    if (worst >= 0) {
        p = mmPutStr(p, end, "itemmem: 增量最大的一项: #");
        p = mmPutDec(p, end, (long long)(worst + 1));
        p = mmPutStr(p, end, " phase=\"");
        p = mmPutStr(p, end, rows[worst].phase);
        p = mmPutStr(p, end, "\" item=\"");
        p = mmPutStr(p, end, rows[worst].item);
        p = mmPutStr(p, end, "\" ΔVmSize=");
        if (ds > 0) { p = mmPutCh(p, end, '+'); }
        p = mmPutDec(p, end, ds);
        p = mmPutStr(p, end, " kB ΔVmRSS=");
        if (dr > 0) { p = mmPutCh(p, end, '+'); }
        p = mmPutDec(p, end, dr);
        p = mmPutStr(p, end, " kB");
        p = mmPutStr(p, end, rows[worst].abnormal ? " => 本项占用异常, 见该项明细\n"
                                                  : " (没有超过判据的项)\n");
    } else {
        p = mmPutStr(p, end, "itemmem: 增量最大的一项: (还没有已结算的项 —— 每项要等它的下一个标记点)\n");
    }
    p = mmPutStr(p, end, "itemmem: 判据(写死): 单项 ΔVmSize > ");
    p = mmPutDec(p, end, kAbnormalSizeKb);
    p = mmPutStr(p, end, " kB(512 MB) 或 ΔVmRSS > ");
    p = mmPutDec(p, end, kAbnormalRssKb);
    p = mmPutStr(p, end, " kB(256 MB) => 该行标 \"本项占用异常, 见该项明细\"\n");
    p = mmPutStr(p, end, "itemmem: 逐行明细文件: ");
    p = mmPutStr(p, end, (memPath != nullptr && memPath[0] != '\0') ? memPath : "(未设置: 先调 setLogDir)");
    p = mmPutStr(p, end, " (进程被 SIGKILL 时, 这份逐项账目是盘上唯一的地址空间归因)\n");
    return p;
}

}   // namespace auroracg

#endif   // AURORA_CRASH_MODMAP_H
