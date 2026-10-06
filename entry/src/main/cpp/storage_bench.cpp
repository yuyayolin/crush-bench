// ============================================================================
//  storage_bench.cpp — 「极光跑分」存储 I/O 小节(顺序读写 + 4K 随机读写 + 元数据)
//
//  本文件是独立小节, 不参与 CS1 单项分/复合分, 不改任何 gb7_*.cpp 负载,
//  也不碰 gpu7/* 、coremark/* 、cpu_affinity.h 、crash_guard.cpp 、Index.ets。
//
//  ---------------------------------------------------------------------------
//  一、OHOS musl 上的可用性核实(逐个读头文件, 不靠假设)
//  ---------------------------------------------------------------------------
//  sysroot = <SDK>/native/sysroot/usr/include
//
//   [可用] O_DIRECT
//       定义在 aarch64-linux-ohos/bits/fcntl.h:15  "#define O_DIRECT 0200000",
//       没有任何特性宏门控; fcntl.h:22 无条件 include 了 bits/fcntl.h,
//       所以 #include <fcntl.h> 就能拿到。→ 直接用。
//
//   [可用] posix_fadvise + POSIX_FADV_DONTNEED
//       fcntl.h:37 "int posix_fadvise(int, off_t, off_t, int);" —— 无条件声明;
//       fcntl.h:71-74 无条件 #define POSIX_FADV_DONTNEED 4(只挡了重复定义)。
//       → 直接用。注意 musl 的返回值符号约定与 POSIX 文字不完全一致, 所以本文件
//         对返回值只判 "== 0", 另外单独记 errno, 不做正负号解释。
//
//   [可用] statvfs / fstatvfs
//       sys/statvfs.h:29-30 —— 无条件声明, struct statvfs 无条件定义。→ 直接用。
//   [可用] statfs(备用) —— sys/statfs.h:18 无条件声明。
//
//   [可用] fsync / fdatasync —— unistd.h:42-43 无条件声明。本小节用 fsync(整文件),
//       不用 fdatasync: 后者允许省略元数据, 对"新建文件 + 改了长度"的文件语义不同。
//
//   [需 _GNU_SOURCE] posix_memalign
//       stdlib.h:103 被 #if defined(_POSIX_SOURCE)||defined(_POSIX_C_SOURCE)||
//       defined(_XOPEN_SOURCE)||defined(_GNU_SOURCE)||defined(_BSD_SOURCE) 包着。
//       本工程 CMake 里 CMAKE_CXX_EXTENSIONS OFF → -std=c++17 会定义 __STRICT_ANSI__,
//       musl 的 features.h 因此不会自动打开上面任何一个 → 不定义 _GNU_SOURCE 时
//       posix_memalign 根本不可见。本文件按 crash_guard.cpp 的既有做法显式
//       #define _GNU_SOURCE 1(必须放在所有 include 之前)。
//       (备选: aligned_alloc, stdlib.h:42 无条件声明; 但 C11 要求 size 是 alignment
//        的整数倍, 语意更容易踩坑, 故选 posix_memalign。)
//
//  注意: 头文件可用 ≠ 运行期一定生效。内核配置 / 文件系统(f2fs 等)/ SELinux 域
//  都可能让 O_DIRECT 打开失败或让 fadvise 变成 no-op, 所以本文件在开跑前会实测
//  一次并把 errno 写进结果(env.oDirectWorks / env.oDirectErrno / fadviseDontNeed*),
//  失败就降级并在 warnings 里说明, 不假装用了绕过缓存的口径。
//
//  ---------------------------------------------------------------------------
//  二、已知陷阱与本文件的对策
//  ---------------------------------------------------------------------------
//   1) 读被页缓存放大: 刚写完的文件整份可能都在页缓存里。对策 = 顺序读给两个口径
//      (seqReadWarm 页缓存命中 / seqReadCold 绕过页缓存), 并标明哪个可比。
//      绕过缓存的优先级: O_DIRECT(缓冲区/偏移/长度全部 4 KiB 对齐) >
//      posix_fadvise(POSIX_FADV_DONTNEED) 丢缓存后重读 > 都没有则按缓存口径跑并标
//      comparable=no。
//   2) 写被 writeback 延迟: 不 fsync 的数字可能根本没落盘。对策 = 顺序写与 4K 随机写
//      都给"不 fsync"与"fsync 落盘"两个口径, 分别报出, 并把 fsync 单独计时(auxMs)。
//      补充事实: 即使不 fsync 也不是纯内存拷贝 —— 脏页上限(通常是内存的几个百分点)
//      会在中途触发回写, 大文件写到一半就被拖慢。
//   3) 随机偏移必须可复现: 固定种子 xorshift64*(只用整数, 不依赖 libm),
//      offset = (rng % (文件大小/4096)) * 4096 → 在整个文件区间内均匀采样。
//      同时统计"命中不同 4K 块数", 把真实覆盖率写进 JSON —— 4096 次操作不是
//      顺序遍历整个文件, 覆盖率必须报, 不许说成"覆盖全盘"。
//   4) 不许写进用户可见目录: 只写 ArkTS 传入目录下的 storage_bench/ 子目录。
//   5) 必须清理(即使中途失败): TempArea 是 RAII, 析构里扫目录逐个 unlink + rmdir,
//      幂等; 异常路径由 auroraStorageRunJson 的 try/catch 兜住后统一收尾;
//      storageSetDir 还会顺手清掉上一次进程被杀留下的残留。
//   6) 空间可控: 开跑前 statvfs 检查 avail >= seqBytes + 小文件总量 + 安全余量,
//      不够就直接跳过返回 skipped=true + skipReason, 一个文件都不写。
//   7) 不许压缩/去重把数字做假: 写入的数据是 PRNG 伪随机字节, 不是全零。
//
//  ---------------------------------------------------------------------------
//  三、时间预算
//  ---------------------------------------------------------------------------
//  默认 budgetMs = 10000(10 秒)。存储项天然比 CPU 慢, CPU 小节 3~8 秒的惯例在这里
//  放宽, 理由是 8 秒内跑不完 10 个子项(尤其慢设备上的 4K 随机)。预算只在子项之间
//  生效: 已经开始的子项一定跑完(中途掐断会毁掉该项的可比性), 没开始的记进
//  stoppedEarly/stopReason。调用方要严格 8 秒可传 {"budgetMs":8000}。
//
//  ---------------------------------------------------------------------------
//  四、线程与队列深度
//  ---------------------------------------------------------------------------
//  单线程、队列深度 1(存储类负载的常见口径), 不测并发聚合带宽。
// ============================================================================

#define _GNU_SOURCE 1  // 必须最先: 见文件头"需 _GNU_SOURCE"一节(posix_memalign)

#include "storage_bench.h"

#include "bench.h"  // auroraSetCurrentItem: 让崩溃/卡死取证能看到"正在跑存储小节"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <exception>
#include <new>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
constexpr int kAlignBytes = 4096;          // O_DIRECT 对齐粒度(取 4096 以同时覆盖 512/4096 逻辑块)
constexpr int kSeqChunkBytes = 1024 * 1024;  // 顺序 I/O 分块 = 1 MiB
constexpr int kRandBlockBytes = 4096;      // 4K 随机块
constexpr long long kOneMiB = 1024LL * 1024LL;
constexpr long long kMaxDistinctTrackBlocks = 64LL * 1024 * 1024;  // 统计覆盖率的位图上限(64M 块 = 256 GiB)

const char* const kSubDirName = "storage_bench";
const char* const kSeqFileName = "seq.dat";
const char* const kManyDirName = "many";

// 编译期事实(直接写进结果 JSON, 让"哪些宏可用"变成可核查的输出而不是注释)
#if defined(O_DIRECT)
const char* const kFactODirect = "O_DIRECT 宏可用(<fcntl.h> → <bits/fcntl.h>, 无特性宏门控)";
#else
const char* const kFactODirect = "O_DIRECT 宏不可用(编译期就没有)";
#endif
#if defined(POSIX_FADV_DONTNEED)
const char* const kFactFadvise = "posix_fadvise/POSIX_FADV_DONTNEED 可用(<fcntl.h>, 无特性宏门控)";
#else
const char* const kFactFadvise = "posix_fadvise/POSIX_FADV_DONTNEED 不可用(编译期就没有)";
#endif

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
double nowMs()
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

std::string esc(const std::string& s)
{
    // 与 napi_init.cpp 的 escapeJson 同规则: JSON 字符串里不允许裸控制字符。
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char e[8];
                    snprintf(e, sizeof(e), "\\u%04x", (unsigned)c);
                    out += e;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

std::string jFmt(const char* fmt, double v)
{
    char b[64];
    snprintf(b, sizeof(b), fmt, v);
    return std::string(b);
}

std::string jNum2(double v) { return jFmt("%.2f", v); }
std::string jNum1(double v) { return jFmt("%.1f", v); }

std::string jI64(long long v)
{
    char b[64];
    snprintf(b, sizeof(b), "%lld", v);
    return std::string(b);
}

std::string jU64(unsigned long long v)
{
    char b[64];
    snprintf(b, sizeof(b), "%llu", v);
    return std::string(b);
}

// ---------------------------------------------------------------------------
// 确定性 PRNG(固定种子 → 可复现)。xorshift64*, 纯整数, 不依赖 libm,
// 所有平台上给出一模一样的序列。
// ---------------------------------------------------------------------------
struct Rng {
    unsigned long long s;
    explicit Rng(unsigned long long seed) : s(seed != 0ULL ? seed : 0x9E3779B97F4A7C15ULL) {}
    unsigned long long next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    unsigned long long below(unsigned long long n) { return n == 0ULL ? 0ULL : (next() % n); }
};

void fillPattern(unsigned char* p, size_t n, unsigned long long seed)
{
    // 伪随机填充而不是全零: 避免文件系统压缩/去重把写入吞吐做假。
    Rng r(seed);
    size_t i = 0;
    while (i + 8 <= n) {
        unsigned long long v = r.next();
        for (int b = 0; b < 8; ++b) {
            p[i + (size_t)b] = (unsigned char)((v >> (8 * b)) & 0xFFULL);
        }
        i += 8;
    }
    while (i < n) {
        p[i] = (unsigned char)(r.next() & 0xFFULL);
        i += 1;
    }
}

void* allocAligned(size_t n)
{
    void* p = nullptr;
    if (posix_memalign(&p, (size_t)kAlignBytes, n) != 0) {
        return nullptr;
    }
    return p;
}

// 对齐缓冲的 RAII 包装: 中途抛异常时也不会漏掉 free。
struct AlignedBuf {
    unsigned char* p = nullptr;
    explicit AlignedBuf(size_t n) : p((unsigned char*)allocAligned(n)) {}
    ~AlignedBuf()
    {
        if (p != nullptr) {
            free(p);
        }
    }
    AlignedBuf(const AlignedBuf&) = delete;
    AlignedBuf& operator=(const AlignedBuf&) = delete;
};

// ---------------------------------------------------------------------------
// RAII 临时区: <base>/storage_bench/ 与 <base>/storage_bench/many/
// 析构一定清理, 且幂等(显式调用 + 析构两次不会重复计数)。
// ---------------------------------------------------------------------------
int countEntries(const std::string& dir)
{
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return 0;
    }
    int n = 0;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        n += 1;
    }
    closedir(d);
    return n;
}

struct TempArea {
    std::string root;  // <base>/storage_bench
    std::string many;  // <base>/storage_bench/many
    int removedFiles = 0;
    int removedDirs = 0;
    int errors = 0;
    bool done = false;

    ~TempArea() { cleanup(); }

    void cleanup()
    {
        if (done) {
            return;  // 幂等: 显式调用过就不再重复
        }
        done = true;
        // 1) many/ 里可能只建了一半(中途失败), 逐个 unlink
        if (!many.empty()) {
            DIR* d = opendir(many.c_str());
            if (d != nullptr) {
                struct dirent* e = nullptr;
                while ((e = readdir(d)) != nullptr) {
                    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                        continue;
                    }
                    const std::string p = many + "/" + e->d_name;
                    if (unlink(p.c_str()) == 0) {
                        removedFiles += 1;
                    } else if (errno != ENOENT) {
                        errors += 1;
                    }
                }
                closedir(d);
            }
            if (rmdir(many.c_str()) == 0) {
                removedDirs += 1;
            } else if (errno != ENOENT) {
                errors += 1;
            }
        }
        // 2) 大文件 + 根目录
        if (!root.empty()) {
            const std::string seq = root + "/" + kSeqFileName;
            if (unlink(seq.c_str()) == 0) {
                removedFiles += 1;
            } else if (errno != ENOENT) {
                errors += 1;
            }
            if (rmdir(root.c_str()) == 0) {
                removedDirs += 1;
            } else if (errno != ENOENT) {
                errors += 1;
            }
        }
    }

    int leftover() const
    {
        int n = countEntries(root) + countEntries(many);
        return n;
    }
};

// ---------------------------------------------------------------------------
// 能力与空间探测结果
// ---------------------------------------------------------------------------
struct Caps {
    bool statvfsOk = false;
    int statvfsErrno = 0;
    unsigned long blockSize = 0;
    unsigned long long totalBytes = 0;
    unsigned long long freeBytes = 0;
    unsigned long long availBytes = 0;  // 非特权用户可用(f_bavail)
    unsigned long long memTotalBytes = 0;  // 0 = 读不到
    bool oDirectWorks = false;
    int oDirectErrno = -1;  // -1 = 没测
    bool fadviseWorks = false;
    int fadviseRet = -2;    // -2 = 没测
    int fadviseErrno = -1;
};

bool probeStatvfs(const std::string& dir, Caps& caps)
{
    struct statvfs vfs;
    memset(&vfs, 0, sizeof(vfs));
    errno = 0;
    if (statvfs(dir.c_str(), &vfs) != 0) {
        caps.statvfsOk = false;
        caps.statvfsErrno = errno;
        return false;
    }
    caps.statvfsOk = true;
    caps.statvfsErrno = 0;
    caps.blockSize = (unsigned long)(vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize);
    if (caps.blockSize == 0) {
        caps.blockSize = 4096;
    }
    caps.totalBytes = (unsigned long long)vfs.f_blocks * (unsigned long long)caps.blockSize;
    caps.freeBytes = (unsigned long long)vfs.f_bfree * (unsigned long long)caps.blockSize;
    caps.availBytes = (unsigned long long)vfs.f_bavail * (unsigned long long)caps.blockSize;
    return true;
}

unsigned long long readMemTotalBytes()
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (f == nullptr) {
        return 0ULL;
    }
    char line[256];
    unsigned long long kb = 0;
    bool found = false;
    while (fgets(line, (int)sizeof(line), f) != nullptr) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            if (sscanf(line + 9, "%llu", &kb) == 1) {
                found = true;
            }
            break;
        }
    }
    fclose(f);
    return found ? (kb * 1024ULL) : 0ULL;
}

// 实测 O_DIRECT: 用 4 KiB 对齐缓冲 + 4 KiB 对齐长度写一个 4 KiB 块。
bool probeODirect(const std::string& root, int& errOut)
{
    errOut = -1;
    const std::string p = root + "/probe_direct.bin";
    unsigned char* buf = (unsigned char*)allocAligned((size_t)kRandBlockBytes);
    if (buf == nullptr) {
        errOut = ENOMEM;
        return false;
    }
    memset(buf, 0x5A, (size_t)kRandBlockBytes);
    const int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0600);
    if (fd < 0) {
        errOut = errno;
        free(buf);
        return false;
    }
    errno = 0;
    const ssize_t n = pwrite(fd, buf, (size_t)kRandBlockBytes, (off_t)0);
    const bool ok = (n == (ssize_t)kRandBlockBytes);
    errOut = ok ? 0 : (errno != 0 ? errno : EIO);
    (void)close(fd);
    (void)unlink(p.c_str());
    free(buf);
    return ok;
}

// 实测 posix_fadvise(POSIX_FADV_DONTNEED)。musl 的返回值符号约定与 POSIX 文字
// 不完全一致, 所以只判 "== 0", 另外单独记 errno, 不做正负号解释。
bool probeFadvise(const std::string& root, int& retOut, int& errnoOut)
{
    const std::string p = root + "/probe_fadvise.bin";
    const int fd = open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        retOut = -1;
        errnoOut = errno;
        return false;
    }
    char b[64];
    memset(b, 0x33, sizeof(b));
    const ssize_t w = pwrite(fd, b, sizeof(b), (off_t)0);
    errno = 0;
    const int r = posix_fadvise(fd, (off_t)0, (off_t)0, POSIX_FADV_DONTNEED);
    const int e = errno;
    retOut = r;
    errnoOut = e;
    (void)close(fd);
    (void)unlink(p.c_str());
    return (w == (ssize_t)sizeof(b)) && (r == 0);
}

// ---------------------------------------------------------------------------
// 配置项(扁平数字 JSON)
// ---------------------------------------------------------------------------
struct Opts {
    long long seqMiB = 256;
    long long randOps = 4096;
    long long smallFiles = 512;
    long long smallFileBytes = 4096;
    unsigned long long seed = 20260101ULL;
    long long budgetMs = 10000;
    long long marginMiB = 128;

    long long seqBytes() const { return seqMiB * kOneMiB; }
    long long marginBytes() const { return marginMiB * kOneMiB; }
    long long needBytes() const { return seqBytes() + smallFiles * smallFileBytes + marginBytes(); }
};

bool findNum(const std::string& js, const char* key, long long& out)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) {
        return false;
    }
    const char* s = js.c_str() + p + 1;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s += 1;
    }
    char* end = nullptr;
    errno = 0;
    const long long v = strtoll(s, &end, 10);
    if (end == s || errno != 0) {
        return false;
    }
    out = v;
    return true;
}

long long clampLL(long long v, long long lo, long long hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

// 返回被夹过的键名(空串 = 没夹)
std::string parseOpts(const std::string& js, Opts& o)
{
    long long v = 0;
    std::string clamped;
    auto take = [&](const char* key, long long lo, long long hi, long long def) -> long long {
        long long val = def;
        if (!js.empty() && findNum(js, key, v)) {
            val = v;
        }
        const long long c = clampLL(val, lo, hi);
        if (c != val) {
            clamped += std::string(clamped.empty() ? "" : ",") + key;
        }
        return c;
    };

    o.seqMiB = take("seqMiB", 16, 4096, 256);
    o.randOps = take("randOps", 64, 2000000, 4096);
    o.smallFiles = take("smallFiles", 0, 65536, 512);
    o.smallFileBytes = take("smallFileBytes", 512, 1024 * 1024, 4096);
    o.budgetMs = take("budgetMs", 2000, 600000, 10000);
    o.marginMiB = take("marginMiB", 16, 4096, 128);
    if (!js.empty() && findNum(js, "seed", v) && v > 0) {
        o.seed = (unsigned long long)v;
    }
    return clamped;
}

// ---------------------------------------------------------------------------
// 单项结果
// ---------------------------------------------------------------------------
struct IoStat {
    bool ok = false;
    int err = 0;
    std::string errText;
    long long bytes = 0;
    long long ops = 0;
    double ms = 0.0;
    double auxMs = -1.0;  // fsync / 丢缓存 / mkdir 之类, <0 = 没测
    int auxErr = 0;
    std::string auxText;
};

struct Item {
    std::string id;
    std::string name;
    std::string mode;
    std::string cmp;  // "yes" | "no" | "partial"
    std::string note;
    bool ok = true;
    std::string error;
    double ms = 0.0;
    long long bytes = -1;
    double mbps = -1.0;
    double iops = -1.0;
    long long ops = -1;
    double auxMs = -1.0;
    std::string auxName;
    long long extra = -1;
    std::string extraName;
};

std::string renderItem(const Item& it)
{
    std::string o = "{\"id\":\"" + esc(it.id) + "\",\"name\":\"" + esc(it.name) + "\"";
    o += ",\"ok\":";
    o += it.ok ? "true" : "false";
    o += ",\"ms\":" + jNum1(it.ms);
    if (it.bytes >= 0) {
        o += ",\"bytes\":" + jI64(it.bytes);
    }
    if (it.mbps >= 0.0) {
        o += ",\"mbps\":" + jNum2(it.mbps);
    }
    if (it.iops >= 0.0) {
        o += ",\"iops\":" + jNum2(it.iops);
    }
    if (it.ops >= 0) {
        o += ",\"ops\":" + jI64(it.ops);
    }
    if (it.auxMs >= 0.0) {
        o += ",\"auxMs\":" + jNum1(it.auxMs);
    }
    if (!it.auxName.empty()) {
        o += ",\"auxName\":\"" + esc(it.auxName) + "\"";
    }
    if (it.extra >= 0 && !it.extraName.empty()) {
        o += ",\"" + esc(it.extraName) + "\":" + jI64(it.extra);
    }
    o += ",\"mode\":\"" + esc(it.mode) + "\"";
    o += ",\"comparable\":\"" + esc(it.cmp) + "\"";
    if (!it.error.empty()) {
        o += ",\"error\":\"" + esc(it.error) + "\"";
    }
    o += ",\"note\":\"" + esc(it.note) + "\"";
    o += "}";
    return o;
}

double mbPerSec(long long bytes, double ms)
{
    if (ms <= 0.0 || bytes <= 0) {
        return -1.0;
    }
    return ((double)bytes / kOneMiB) / (ms / 1000.0);
}

double opsPerSec(long long ops, double ms)
{
    if (ms <= 0.0 || ops <= 0) {
        return -1.0;
    }
    return ((double)ops) / (ms / 1000.0);
}

// ---------------------------------------------------------------------------
// I/O 原语
// ---------------------------------------------------------------------------
int dropPageCache(int fd)
{
    errno = 0;
    return posix_fadvise(fd, (off_t)0, (off_t)0, POSIX_FADV_DONTNEED);
}

void ioSeqWrite(const std::string& path, long long bytes, unsigned char* buf, int chunk, bool direct,
                bool doFsync, IoStat& st)
{
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (direct) {
        flags |= O_DIRECT;
    }
    const int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        st.err = errno;
        st.errText = "open 失败";
        return;
    }
    const double t0 = nowMs();
    long long done = 0;
    while (done < bytes) {
        const long long left = bytes - done;
        const int n = (int)(left < (long long)chunk ? left : (long long)chunk);
        errno = 0;
        const ssize_t w = pwrite(fd, buf, (size_t)n, (off_t)done);
        if (w <= 0) {
            st.err = (errno != 0 ? errno : EIO);
            st.errText = "pwrite 失败";
            break;
        }
        done += (long long)w;
        if (w != (ssize_t)n) {
            st.err = EIO;
            st.errText = "pwrite 短写";
            break;
        }
    }
    st.ms = nowMs() - t0;
    st.bytes = done;
    st.ops = done / (long long)chunk;
    st.ok = (done == bytes);
    if (doFsync) {
        errno = 0;
        const double f0 = nowMs();
        const int r = fsync(fd);
        st.auxMs = nowMs() - f0;
        if (r != 0) {
            st.auxErr = errno;
            st.auxText = "fsync 失败";
            st.ok = false;
        }
    }
    (void)close(fd);
}

void ioSeqRead(const std::string& path, long long bytes, unsigned char* buf, int chunk, bool direct,
               bool dropFirst, IoStat& st)
{
    int flags = O_RDONLY;
    if (direct) {
        flags |= O_DIRECT;
    }
    const int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        st.err = errno;
        st.errText = "open 失败";
        return;
    }
    if (!direct && dropFirst) {
        const double d0 = nowMs();
        const int r = dropPageCache(fd);
        st.auxMs = nowMs() - d0;
        if (r != 0) {
            st.auxErr = (errno != 0 ? errno : r);
            st.auxText = "posix_fadvise(DONTNEED) 返回非 0";
        }
    }
    const double t0 = nowMs();
    long long done = 0;
    while (done < bytes) {
        const long long left = bytes - done;
        const int n = (int)(left < (long long)chunk ? left : (long long)chunk);
        errno = 0;
        const ssize_t r = pread(fd, buf, (size_t)n, (off_t)done);
        if (r <= 0) {
            st.err = (errno != 0 ? errno : EIO);
            st.errText = (r == 0 ? "读到文件尾(短文件?)" : "pread 失败");
            break;
        }
        done += (long long)r;
    }
    st.ms = nowMs() - t0;
    st.bytes = done;
    st.ops = done / (long long)chunk;
    st.ok = (done == bytes);
    (void)close(fd);
}

void ioRandRead(const std::string& path, long long fileBytes, long long ops, unsigned long long seed,
                unsigned char* buf, bool direct, bool dropFirst, IoStat& st, long long& distinctOut)
{
    distinctOut = 0;
    const long long blocks = fileBytes / (long long)kRandBlockBytes;
    if (blocks <= 0) {
        st.errText = "文件太小, 没有完整的 4K 块";
        return;
    }
    int flags = O_RDONLY;
    if (direct) {
        flags |= O_DIRECT;
    }
    const int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        st.err = errno;
        st.errText = "open 失败";
        return;
    }
    if (!direct && dropFirst) {
        const double d0 = nowMs();
        const int r = dropPageCache(fd);
        st.auxMs = nowMs() - d0;
        if (r != 0) {
            st.auxErr = (errno != 0 ? errno : r);
            st.auxText = "posix_fadvise(DONTNEED) 返回非 0";
        }
    }

    std::vector<unsigned char> seen;
    const bool track = (blocks <= kMaxDistinctTrackBlocks);
    if (track) {
        seen.assign((size_t)blocks, 0);
    }

    Rng rng(seed);
    const double t0 = nowMs();
    long long done = 0;
    for (long long i = 0; i < ops; ++i) {
        const long long b = (long long)rng.below((unsigned long long)blocks);
        const off_t off = (off_t)(b * (long long)kRandBlockBytes);
        errno = 0;
        const ssize_t r = pread(fd, buf, (size_t)kRandBlockBytes, off);
        if (r != (ssize_t)kRandBlockBytes) {
            st.err = (errno != 0 ? errno : EIO);
            st.errText = "pread 4K 失败";
            break;
        }
        done += 1;
        if (track) {
            seen[(size_t)b] = 1;
        }
    }
    st.ms = nowMs() - t0;
    st.ops = done;
    st.bytes = done * (long long)kRandBlockBytes;
    st.ok = (done == ops);
    if (track) {
        long long n = 0;
        for (size_t i = 0; i < seen.size(); ++i) {
            n += (seen[i] != 0) ? 1 : 0;
        }
        distinctOut = n;
    } else {
        distinctOut = -1;
    }
    (void)close(fd);
}

void ioRandWrite(const std::string& path, long long fileBytes, long long ops, unsigned long long seed,
                 unsigned char* buf, bool direct, bool doFsync, IoStat& st)
{
    const long long blocks = fileBytes / (long long)kRandBlockBytes;
    if (blocks <= 0) {
        st.errText = "文件太小, 没有完整的 4K 块";
        return;
    }
    int flags = direct ? (O_WRONLY | O_DIRECT) : O_RDWR;
    const int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        st.err = errno;
        st.errText = "open 失败";
        return;
    }
    Rng rng(seed);
    const double t0 = nowMs();
    long long done = 0;
    for (long long i = 0; i < ops; ++i) {
        const long long b = (long long)rng.below((unsigned long long)blocks);
        const off_t off = (off_t)(b * (long long)kRandBlockBytes);
        errno = 0;
        const ssize_t r = pwrite(fd, buf, (size_t)kRandBlockBytes, off);
        if (r != (ssize_t)kRandBlockBytes) {
            st.err = (errno != 0 ? errno : EIO);
            st.errText = "pwrite 4K 失败";
            break;
        }
        done += 1;
    }
    st.ms = nowMs() - t0;
    st.ops = done;
    st.bytes = done * (long long)kRandBlockBytes;
    st.ok = (done == ops);
    if (doFsync) {
        errno = 0;
        const double f0 = nowMs();
        const int r = fsync(fd);
        st.auxMs = nowMs() - f0;
        if (r != 0) {
            st.auxErr = errno;
            st.auxText = "fsync 失败";
            st.ok = false;
        }
    }
    (void)close(fd);
}

void ioManyCreate(const std::string& manyDir, long long n, long long fileBytes, unsigned char* buf,
                  IoStat& create, IoStat& del, IoStat& dirSync)
{
    std::vector<std::string> names;
    names.reserve((size_t)(n > 0 ? n : 0));

    dirSync.ms = -1.0;  // -1 = 没测到(打不开目录)
    const double t0 = nowMs();
    long long made = 0;
    for (long long i = 0; i < n; ++i) {
        char nm[64];
        snprintf(nm, sizeof(nm), "/f%05lld.bin", i);
        const std::string p = manyDir + nm;
        const int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            create.err = errno;
            create.errText = "open(O_CREAT|O_EXCL) 失败";
            break;
        }
        errno = 0;
        const ssize_t w = pwrite(fd, buf, (size_t)fileBytes, (off_t)0);
        (void)close(fd);
        if (w != (ssize_t)fileBytes) {
            create.err = (errno != 0 ? errno : EIO);
            create.errText = "小文件 pwrite 失败";
            break;
        }
        names.push_back(p);
        made += 1;
    }
    create.ms = nowMs() - t0;
    create.ops = made;
    create.bytes = made * fileBytes;
    create.ok = (made == n);

    // 目录 fsync: 把"创建"的元数据真正推下去(单独计时, 不混进 create.ms)
    const int dfd = open(manyDir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        errno = 0;
        const double d0 = nowMs();
        const int r = fsync(dfd);
        dirSync.ms = nowMs() - d0;
        dirSync.ok = (r == 0);
        if (r != 0) {
            dirSync.err = errno;
            dirSync.errText = "目录 fsync 失败(部分文件系统不支持)";
            dirSync.auxText = "文件系统不支持目录 fsync 时为 EINVAL, 属正常";
        }
        (void)close(dfd);
    } else {
        dirSync.err = errno;
        dirSync.errText = "打不开目录(无法做元数据落盘)";
    }

    const double t1 = nowMs();
    long long removed = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (unlink(names[i].c_str()) == 0) {
            removed += 1;
        }
    }
    del.ms = nowMs() - t1;
    del.ops = removed;
    del.bytes = removed * fileBytes;
    del.ok = (removed == made);
}

// ---------------------------------------------------------------------------
// 一次运行的上下文
// ---------------------------------------------------------------------------
struct RunCtx {
    Opts opt;
    Caps caps;
    TempArea* area = nullptr;
    std::string bypassMethod;  // "O_DIRECT" | "posix_fadvise(DONTNEED)" | "none"
    std::string bypassNote;
    bool fileFitsInPageCache = false;
    bool stoppedEarly = false;
    std::string stopReason;
    std::vector<std::string> items;
    std::vector<std::string> warnings;

    // headline(给 UI 直接取用)
    double seqWriteDurableMbps = -1.0;
    double seqReadColdMbps = -1.0;
    double randRead4kIops = -1.0;
    double randWrite4kIops = -1.0;
    double filesPerSecCreate = -1.0;
    double filesPerSecDelete = -1.0;

    double tStart = 0.0;
    bool budgetExceeded() const { return (nowMs() - tStart) > (double)opt.budgetMs; }
    void stop(const std::string& why)
    {
        if (!stoppedEarly) {
            stoppedEarly = true;
            stopReason = why;
        }
    }
    void push(const Item& it) { items.push_back(renderItem(it)); }
};

void addSkipItem(RunCtx& c, const char* id, const char* name, const std::string& why)
{
    Item it;
    it.id = id;
    it.name = name;
    it.ok = false;
    it.error = why;
    it.mode = "skipped";
    it.cmp = "no";
    it.note = "未运行(预算/前置失败), 数字留空而不是填 0 —— 0 会被误读成“极慢”。";
    c.push(it);
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
void runPhases(RunCtx& c)
{
    const Opts& o = c.opt;
    const std::string seqPath = c.area->root + "/" + kSeqFileName;
    const long long seqBytes = o.seqBytes();

    AlignedBuf ab((size_t)kSeqChunkBytes);
    if (ab.p == nullptr) {
        throw std::bad_alloc();
    }
    unsigned char* buf = ab.p;
    fillPattern(buf, (size_t)kSeqChunkBytes, o.seed);
    // 4K 随机项复用同一块缓冲的前 4 KiB(同样 4096 对齐)
    unsigned char* blk = buf;

    const bool direct = c.caps.oDirectWorks;
    const bool fadvise = (c.caps.fadviseRet == 0);

    // ---- ① 顺序写 (不 fsync) ----
    IoStat w1;
    ioSeqWrite(seqPath, seqBytes, buf, kSeqChunkBytes, false, false, w1);
    {
        Item it;
        it.id = "seqWrite";
        it.name = "顺序写 (不 fsync, 只到 write 返回)";
        it.mode = "buffered-no-fsync";
        it.cmp = "no";
        it.ok = w1.ok;
        it.error = w1.errText;
        it.ms = w1.ms;
        it.bytes = w1.bytes;
        it.mbps = mbPerSec(w1.bytes, w1.ms);
        it.note = "计时只到 write() 返回, 数据可能还在页缓存里 —— 这是上界, 不保证落盘, 也"
                  "不可跨设备比较。即使不 fsync 也不是纯内存拷贝: 脏页上限(通常内存的几个百分点)"
                  "会中途触发回写, 大文件写到一半就会被拖慢。";
        if (!w1.ok) {
            it.note = "顺序写失败, 后续依赖该文件的项(顺序读/4K 随机)会一并跳过。";
        }
        c.push(it);
    }

    // ---- ② 顺序写 (fsync 落盘) ----
    if (w1.ok) {
        IoStat w2;
        ioSeqWrite(seqPath, seqBytes, buf, kSeqChunkBytes, false, true, w2);
        const double totalMs = w2.ms + (w2.auxMs > 0.0 ? w2.auxMs : 0.0);
        Item it;
        it.id = "seqWriteDurable";
        it.name = "顺序写 (fsync 落盘)";
        it.mode = "buffered+fsync";
        it.cmp = "yes";
        it.ok = w2.ok;
        it.error = w2.errText;
        it.ms = totalMs;
        it.bytes = w2.bytes;
        it.mbps = mbPerSec(w2.bytes, totalMs);
        it.auxMs = w2.auxMs;
        it.auxName = "fsyncMs";
        it.note = "重写同一文件并在末尾 fsync(fsync 会把数据+元数据都推到存储), 报的是 "
                  "write+fsync 的总时间口径 —— 这一项才接近“真实落盘速度”。auxMs 是其中 fsync "
                  "单独占的时间: seqBytes/fsyncMs 就是设备的稳态写入速率。";
        c.push(it);
        c.seqWriteDurableMbps = it.mbps;
    }

    // ---- ③ 顺序读 (页缓存命中, 对照值) ----
    bool fileUsable = w1.ok && w1.bytes == seqBytes;
    if (fileUsable && !c.budgetExceeded()) {
        IoStat r1;
        ioSeqRead(seqPath, seqBytes, buf, kSeqChunkBytes, false, false, r1);
        Item it;
        it.id = "seqReadWarm";
        it.name = "顺序读 (页缓存命中, 仅对照)";
        it.mode = "buffered-warm";
        it.cmp = "no";
        it.ok = r1.ok;
        it.error = r1.errText;
        it.ms = r1.ms;
        it.bytes = r1.bytes;
        it.mbps = mbPerSec(r1.bytes, r1.ms);
        it.note = "刚写完就立刻读回, 整份文件大概率还在页缓存里 —— 这个数字是被缓存放大的上界, "
                  "只用来和 seqReadCold 对照, 不要单独拿去做横向比较。";
        c.push(it);
    } else if (!fileUsable) {
        addSkipItem(c, "seqReadWarm", "顺序读 (页缓存命中, 仅对照)", "顺序写没有把文件写完整");
    }

    // ---- ④ 顺序读 (绕过页缓存) ----
    if (fileUsable && !c.budgetExceeded()) {
        IoStat r2;
        ioSeqRead(seqPath, seqBytes, buf, kSeqChunkBytes, direct, true, r2);
        Item it;
        it.id = "seqReadCold";
        it.name = "顺序读 (绕过页缓存)";
        it.mode = direct ? "O_DIRECT" : (fadvise ? "posix_fadvise(DONTNEED)" : "buffered-no-bypass");
        it.cmp = direct ? "yes" : (fadvise ? "partial" : "no");
        it.ok = r2.ok;
        it.error = r2.errText;
        it.ms = r2.ms;
        it.bytes = r2.bytes;
        it.mbps = mbPerSec(r2.bytes, r2.ms);
        it.auxMs = r2.auxMs;
        it.auxName = "dropCacheMs";
        it.note = direct
            ? "用 O_DIRECT 读回同一文件: 缓冲区/偏移/长度全部 4 KiB 对齐, 绕过页缓存。"
              "局限: O_DIRECT 绕不过存储设备自身的 DRAM 缓存, 也绕不过文件系统的内部缓存。"
            : (fadvise
                  ? "先 posix_fadvise(POSIX_FADV_DONTNEED) 尽力丢弃页缓存再按普通读计时。"
                    "局限: DONTNEED 只保证“尽力”, 脏页要先回写才会被丢; 对已 mmap 的页面无效; "
                    "内核/文件系统可以部分实现 —— 所以这是冷读的近似值, 不是硬保证。"
                  : "本设备上 O_DIRECT 与 posix_fadvise 都不可用, 只能按普通读计时, 数字会被页缓存"
                    "放大, 不可跨设备比较(见 warnings)。");
        if (!direct && !fadvise) {
            c.warnings.push_back("顺序冷读/4K 随机项都拿不到绕过页缓存的手段(O_DIRECT 与 "
                                 "posix_fadvise 均不可用), 这几项是缓存口径的上界。");
        }
        c.push(it);
        c.seqReadColdMbps = it.mbps;
    } else if (!fileUsable) {
        addSkipItem(c, "seqReadCold", "顺序读 (绕过页缓存)", "顺序写没有把文件写完整");
    }

    // ---- ⑤ 4K 随机读 ----
    if (fileUsable && !c.budgetExceeded()) {
        IoStat rr;
        long long distinct = 0;
        ioRandRead(seqPath, seqBytes, o.randOps, o.seed, blk, direct, true, rr, distinct);
        const long long blocks = seqBytes / (long long)kRandBlockBytes;
        Item it;
        it.id = "randRead4k";
        it.name = "4K 随机读";
        it.mode = direct ? "O_DIRECT" : (fadvise ? "posix_fadvise(DONTNEED)" : "buffered-no-bypass");
        it.cmp = direct ? "yes" : (fadvise ? "partial" : "no");
        it.ok = rr.ok;
        it.error = rr.errText;
        it.ms = rr.ms;
        it.bytes = rr.bytes;
        it.ops = rr.ops;
        it.iops = opsPerSec(rr.ops, rr.ms);
        it.mbps = mbPerSec(rr.bytes, rr.ms);
        it.auxMs = rr.auxMs;
        it.auxName = "dropCacheMs";
        it.extra = distinct;
        it.extraName = "distinct4kBlocks";
        it.note = "单线程、队列深度 1。偏移由固定种子的 xorshift64*(config.seed)生成: "
                  "offset = (rng % (文件大小/4096)) * 4096, 同一种子在所有设备上给出同一串偏移, 可复现。"
                  "覆盖范围 = 整个文件区间内的均匀采样; 本次命中不同 4K 块 " + jI64(distinct) +
                  " / 共 " + jI64(blocks) + " 块(覆盖率 = distinct/总块数), 也就是说它不是顺序遍历"
                  "整个文件, 实际覆盖率必须按这两个数解读。";
        c.push(it);
        c.randRead4kIops = it.iops;
    } else if (!fileUsable) {
        addSkipItem(c, "randRead4k", "4K 随机读", "顺序写没有把文件写完整");
    }

    // ---- ⑥ 4K 随机写(页缓存口径) ----
    if (fileUsable && !c.budgetExceeded()) {
        IoStat rw1;
        ioRandWrite(seqPath, seqBytes, o.randOps, o.seed, blk, false, true, rw1);
        Item it;
        it.id = "randWrite4kBuffered";
        it.name = "4K 随机写 (页缓存, 不 fsync)";
        it.mode = "buffered-no-fsync";
        it.cmp = "no";
        it.ok = rw1.ok;
        it.error = rw1.errText;
        it.ms = rw1.ms;
        it.bytes = rw1.bytes;
        it.ops = rw1.ops;
        it.iops = opsPerSec(rw1.ops, rw1.ms);
        it.mbps = mbPerSec(rw1.bytes, rw1.ms);
        it.auxMs = rw1.auxMs;
        it.auxName = "fsyncMs";
        it.note = "随机写落在已缓存的页面上, 计时只到 write 返回 —— 这是被页缓存/写回延迟放大的上界。"
                  "auxMs 是把这些脏页真正刷下去的 fsync 耗时, 用 (ops/auxMs) 可以粗看设备的随机写落盘能力。"
                  "本项跑完会做一次 fsync, 也是为了让后面的 O_DIRECT 阶段不与未刷的脏页冲突。";
        c.push(it);
    } else if (!fileUsable) {
        addSkipItem(c, "randWrite4kBuffered", "4K 随机写 (页缓存, 不 fsync)", "顺序写没有把文件写完整");
    }

    // ---- ⑦ 4K 随机写(O_DIRECT / 绕过缓存) ----
    if (fileUsable && !c.budgetExceeded()) {
        IoStat rw2;
        ioRandWrite(seqPath, seqBytes, o.randOps, o.seed, blk, direct, true, rw2);
        Item it;
        it.id = "randWrite4k";
        it.name = "4K 随机写 (绕过页缓存)";
        it.mode = direct ? "O_DIRECT" : (fadvise ? "buffered-no-fsync" : "buffered-no-bypass");
        it.cmp = direct ? "partial" : "no";
        it.ok = rw2.ok;
        it.error = rw2.errText;
        it.ms = rw2.ms;
        it.bytes = rw2.bytes;
        it.ops = rw2.ops;
        it.iops = opsPerSec(rw2.ops, rw2.ms);
        it.mbps = mbPerSec(rw2.bytes, rw2.ms);
        it.auxMs = rw2.auxMs;
        it.auxName = "fsyncMs";
        it.note = direct
            ? "O_DIRECT 随机写: 4 KiB 对齐, 绕过页缓存, 写放大与 GC 都真实发生, 比上一项更接近设备"
              "行为。局限: 仍受文件系统(f2fs 的 GC/段分配)与写回策略影响, 且不同文件系统之间"
              "不可直接横向比较, 故 comparable 标 partial; auxMs 是收尾 fsync 的耗时。"
            : "本设备没有可用的 O_DIRECT, 退回普通写 + 收尾 fsync 的口径, 数字会被页缓存放大, "
              "不可跨设备比较。";
        c.push(it);
        c.randWrite4kIops = it.iops;
    } else if (!fileUsable) {
        addSkipItem(c, "randWrite4k", "4K 随机写 (绕过页缓存)", "顺序写没有把文件写完整");
    }

    // ---- ⑧ 摘要行: fsync 后文件的真实落盘(给 headline 用, 不重复计时) ----
    // (不单独成一个 item, 它的耗时已经并进 seqWriteDurable)

    // ---- ⑨ 小文件创建/删除 ----
    if (o.smallFiles > 0 && !c.budgetExceeded()) {
        const double t0 = nowMs();
        const int mk = mkdir(c.area->many.c_str(), 0700);
        const double mkMs = nowMs() - t0;
        if (mk != 0 && errno != EEXIST) {
            addSkipItem(c, "fileCreate", "创建小文件", std::string("mkdir 失败 errno=") + jI64((long long)errno));
            addSkipItem(c, "fileDelete", "删除小文件", "mkdir 失败");
        } else {
            IoStat cr;
            IoStat dl;
            IoStat dsync;
            ioManyCreate(c.area->many, o.smallFiles, o.smallFileBytes, blk, cr, dl, dsync);

            Item it;
            it.id = "fileCreate";
            it.name = "创建 " + jI64(o.smallFiles) + " 个 4KB 小文件";
            it.mode = "O_CREAT|O_EXCL + write + close";
            it.cmp = "no";
            it.ok = cr.ok;
            it.error = cr.errText;
            it.ms = cr.ms;
            it.bytes = cr.bytes;
            it.ops = cr.ops;
            it.iops = opsPerSec(cr.ops, cr.ms);
            it.mbps = mbPerSec(cr.bytes, cr.ms);
            it.auxMs = dsync.ms >= 0.0 ? dsync.ms : mkMs;
            it.auxName = dsync.ms >= 0.0 ? "dirFsyncMs" : "mkdirMs";
            it.extra = cr.ops;
            it.extraName = "files";
            it.note = "报 文件/s(= iops 字段)与 MiB/s。元数据项, 受文件系统与内核缓存影响极大: "
                      "新建的 dentry/inode 会先进内存缓存, 批量创建往往不触发真正的元数据落盘, "
                      "不同文件系统(f2fs / ext4 / erofs)之间差几倍很正常。auxName=dirFsyncMs 是把"
                      "目录 fsync 强制落盘单独计时的结果(部分文件系统不支持目录 fsync, 会返回 EINVAL, "
                      "此时该项仍有效但 auxName 变成 mkdirMs)。本项只做同一口径下的横向参考。";
            c.push(it);
            c.filesPerSecCreate = it.iops;

            Item it2;
            it2.id = "fileDelete";
            it2.name = "删除 " + jI64(o.smallFiles) + " 个 4KB 小文件";
            it2.mode = "unlink 循环";
            it2.cmp = "no";
            it2.ok = dl.ok;
            it2.error = "";
            it2.ms = dl.ms;
            it2.bytes = dl.bytes;
            it2.ops = dl.ops;
            it2.iops = opsPerSec(dl.ops, dl.ms);
            it2.mbps = mbPerSec(dl.bytes, dl.ms);
            it2.extra = dl.ops;
            it2.extraName = "files";
            it2.note = "报 文件/s(= iops 字段)。同样受缓存影响: unlink 只改内存里的 dentry, 真正"
                       "回收 inode/数据块是延迟的, 所以这个数字是“元数据删除”的速度而不是“空间回收”的速度。";
            c.push(it2);
            c.filesPerSecDelete = it2.iops;
        }
    } else if (o.smallFiles <= 0) {
        addSkipItem(c, "fileCreate", "创建小文件", "options 里 smallFiles=0, 主动跳过");
        addSkipItem(c, "fileDelete", "删除小文件", "options 里 smallFiles=0, 主动跳过");
    } else {
        addSkipItem(c, "fileCreate", "创建小文件", "时间预算用尽, 未开始");
        addSkipItem(c, "fileDelete", "删除小文件", "时间预算用尽, 未开始");
    }
}

// ---------------------------------------------------------------------------
// JSON 组装
// ---------------------------------------------------------------------------
std::string composeResultJson(const RunCtx& c, const TempArea& area, const std::string& baseDir,
                              const std::string& fatal, const std::string& clamped,
                              bool skipped, const std::string& skipReason, long long skipNeed)
{
    const Opts& o = c.opt;
    const double totalMs = nowMs() - c.tStart;

    std::string out;
    out.reserve(8192);
    out += "{\"ok\":";
    out += (fatal.empty() && !skipped) ? "true" : "false";
    out += ",\"section\":\"storage\"";
    out += ",\"sectionTitle\":\"存储 I/O(独立小节, 不进 CS1 分数)\"";
    out += ",\"jsonVersion\":1";
    out += ",\"skipped\":";
    out += skipped ? "true" : "false";
    if (skipped) {
        out += ",\"skipReason\":\"" + esc(skipReason) + "\"";
    }

    out += ",\"dir\":\"" + esc(area.root) + "\"";
    out += ",\"baseDir\":\"" + esc(baseDir) + "\"";
    out += ",\"totalMs\":" + jNum1(totalMs);
    out += ",\"budgetMs\":" + jI64(o.budgetMs);
    out += ",\"stoppedEarly\":";
    out += c.stoppedEarly ? "true" : "false";
    if (c.stoppedEarly) {
        out += ",\"stopReason\":\"" + esc(c.stopReason) + "\"";
    }
    if (!clamped.empty()) {
        out += ",\"clampedOptions\":\"" + esc(clamped) + "\"";
    }
    if (!fatal.empty()) {
        out += ",\"fatal\":\"" + esc(fatal) + "\"";
    }

    // ---- env ----
    out += ",\"env\":{";
    out += "\"statvfsOk\":";
    out += c.caps.statvfsOk ? "true" : "false";
    out += ",\"statvfsErrno\":" + jI64((long long)c.caps.statvfsErrno);
    out += ",\"blockSize\":" + jU64((unsigned long long)c.caps.blockSize);
    out += ",\"totalBytes\":" + jU64(c.caps.totalBytes);
    out += ",\"freeBytes\":" + jU64(c.caps.freeBytes);
    out += ",\"availBytes\":" + jU64(c.caps.availBytes);
    out += ",\"memTotalBytes\":" + jU64(c.caps.memTotalBytes);
    out += ",\"fileFitsInPageCache\":";
    out += c.fileFitsInPageCache ? "true" : "false";
    out += ",\"oDirectMacro\":\"" + esc(kFactODirect) + "\"";
    out += ",\"oDirectWorks\":";
    out += c.caps.oDirectWorks ? "true" : "false";
    out += ",\"oDirectErrno\":" + jI64((long long)c.caps.oDirectErrno);
    out += ",\"fadviseMacro\":\"" + esc(kFactFadvise) + "\"";
    out += ",\"fadviseRet\":" + jI64((long long)c.caps.fadviseRet);
    out += ",\"fadviseErrno\":" + jI64((long long)c.caps.fadviseErrno);
    out += ",\"fadviseDontNeedWorks\":";
    out += (c.caps.fadviseRet == 0) ? "true" : "false";
    out += ",\"bypassMethod\":\"" + esc(c.bypassMethod) + "\"";
    out += "}";

    // ---- config ----
    out += ",\"config\":{";
    out += "\"seqMiB\":" + jI64(o.seqMiB);
    out += ",\"seqBytes\":" + jI64(o.seqBytes());
    out += ",\"randOps\":" + jI64(o.randOps);
    out += ",\"blockBytes\":" + jI64((long long)kRandBlockBytes);
    out += ",\"smallFiles\":" + jI64(o.smallFiles);
    out += ",\"smallFileBytes\":" + jI64(o.smallFileBytes);
    out += ",\"seed\":" + jU64(o.seed);
    out += ",\"prng\":\"xorshift64* (纯整数, 跨设备同种子同序列)\"";
    out += ",\"threads\":1,\"queueDepth\":1";
    out += ",\"mbpsUnit\":\"MiB/s (1 MiB = 1048576 B); 换算成 MB/s(10^6) 请乘以 1.048576\"";
    if (skipped) {
        out += ",\"needBytes\":" + jI64(skipNeed);
    } else {
        out += ",\"needBytes\":" + jI64(o.needBytes());
        out += ",\"marginMiB\":" + jI64(o.marginMiB);
    }
    out += "}";

    // ---- headline ----
    out += ",\"headline\":{";
    out += "\"seqWriteDurableMbps\":" + jNum2(c.seqWriteDurableMbps);
    out += ",\"seqReadColdMbps\":" + jNum2(c.seqReadColdMbps);
    out += ",\"randRead4kIops\":" + jNum2(c.randRead4kIops);
    out += ",\"randWrite4kIops\":" + jNum2(c.randWrite4kIops);
    out += ",\"filesPerSecCreate\":" + jNum2(c.filesPerSecCreate);
    out += ",\"filesPerSecDelete\":" + jNum2(c.filesPerSecDelete);
    out += "}";

    // ---- cleanup ----
    out += ",\"cleanup\":{";
    out += "\"ok\":";
    out += (area.leftover() == 0 && area.errors == 0) ? "true" : "false";
    out += ",\"removedFiles\":" + jI64((long long)area.removedFiles);
    out += ",\"removedDirs\":" + jI64((long long)area.removedDirs);
    out += ",\"errors\":" + jI64((long long)area.errors);
    out += ",\"leftoverEntries\":" + jI64((long long)area.leftover());
    out += "}";

    // ---- items ----
    out += ",\"items\":[";
    for (size_t i = 0; i < c.items.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += c.items[i];
    }
    out += "]";

    // ---- warnings ----
    out += ",\"warnings\":[";
    for (size_t i = 0; i < c.warnings.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "\"" + esc(c.warnings[i]) + "\"";
    }
    out += "]";

    // ---- notes: 口径与已知陷阱(可直接显示给用户) ----
    static const char* const kNotes[] = {
        "本小节是独立小节, 不参与 CS1 单项分/复合分(GB7 没有存储项, 混进去会毁掉可比性)。",
        "单位: 吞吐一律 MiB/s(1 MiB = 1048576 B); 与用 MB/s(10^6)报数的工具对比请 ×1.048576。"
        "IOPS = 操作次数/秒。单线程、队列深度 1, 不测并发聚合带宽。",
        "顺序写给两个口径: seqWrite 不 fsync(只到 write 返回, 上界, 不可跨设备比较); "
        "seqWriteDurable 是 write+fsync 的总时间(更接近真实落盘)。两者都报, 不要混用。",
        "读会给两个口径: seqReadWarm(页缓存命中, 被放大, 仅对照)与 seqReadCold(绕过页缓存)。"
        "实际用哪种绕过手段写在每个 item 的 mode 与 env.bypassMethod 里。",
        "绕过页缓存的优先级: O_DIRECT(缓冲区/偏移/长度全部 4 KiB 对齐) > "
        "posix_fadvise(POSIX_FADV_DONTNEED) 丢缓存后重读 > 都没有则按缓存口径跑并把 comparable 标 no。"
        "开跑前会各实测一次, 结果在 env.oDirectWorks / env.fadviseDontNeedWorks。",
        "O_DIRECT 的局限: 需要内核与文件系统支持, 不支持时 open 直接 EINVAL; 它绕过页缓存但**不绕过"
        "存储设备自身的 DRAM 缓存**。posix_fadvise(DONTNEED) 的局限: 只保证“尽力”丢弃干净页, "
        "脏页要先回写; 对 mmap 的页面无效; 实现可以部分生效 —— 所以它是冷读的近似值。",
        "4K 随机偏移用固定种子的 xorshift64*(config.seed), 同一种子在所有设备上给出同一串偏移, "
        "可复现; offset = (rng % (文件大小/4096)) * 4096, 在整个文件区间内均匀采样。"
        "每个随机读项都报 distinct4kBlocks 与实际总块数, 覆盖率按这两个数解读 —— "
        "4096 次操作远不足以顺序遍历整个文件。",
        "随机写同样给“页缓存(不 fsync)”与“绕过缓存”两个口径, 因为写回延迟这个陷阱对随机写一样成立。",
        "文件创建/删除(元数据)项受文件系统与内核缓存影响极大: 新建 dentry/inode 先进内存缓存, "
        "unlink 只改内存里的目录项, inode/数据块的真正回收是延迟的。不同文件系统之间差几倍很正常, "
        "本项只做同一口径下的横向参考。",
        "所有临时文件只写在 ArkTS 传入目录下的 storage_bench/ 子目录, 不碰用户可见目录; "
        "无论成功/失败/异常/提前中止都会删除, 结果里的 cleanup 字段报告删了多少、有没有残留。",
        "开跑前用 statvfs 检查可用空间, 不足 needBytes 时直接跳过并且一个文件都不写; "
        "空间读不出来也按跳过处理(宁可跳过也不赌)。",
        "写入的数据是 PRNG 伪随机字节而不是全零, 避免文件系统压缩/去重把写入吞吐做假。",
    };
    out += ",\"notes\":[";
    for (size_t i = 0; i < sizeof(kNotes) / sizeof(kNotes[0]); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "\"" + esc(kNotes[i]) + "\"";
    }
    out += "]";

    out += "}";
    return out;
}

std::string skipJson(const std::string& reason, const Opts& o, const Caps& caps,
                     const std::string& baseDir, long long need, const std::string& clamped)
{
    RunCtx c;
    c.opt = o;
    c.caps = caps;
    c.tStart = nowMs();
    // 只用于展示路径与统计残留: done=true 保证不删任何东西。
    // leftover() 会在目录不存在时返回 0, 所以正常情况下 cleanup.ok 为 true;
    // 若上一次进程被杀留下残骸, 这里会报成 cleanup.ok=false。
    TempArea dummy;
    dummy.root = baseDir.empty() ? std::string() : (baseDir + "/" + kSubDirName);
    dummy.many = dummy.root.empty() ? std::string() : (dummy.root + "/" + kManyDirName);
    dummy.done = true;  // 不碰磁盘
    return composeResultJson(c, dummy, baseDir, std::string(), clamped, true, reason, need);
}

}  // namespace

// ===========================================================================
// 对外入口
// ===========================================================================

namespace {
// 沙箱根目录(napi 线程与主线程都可能读写, 用一把互斥锁保护)
pthread_mutex_t g_dirLock = PTHREAD_MUTEX_INITIALIZER;
std::string g_baseDir;
}  // namespace

int auroraStorageSetDir(const char* dir)
{
    if (dir == nullptr || dir[0] == '\0') {
        return 0;
    }
    const size_t n = strlen(dir);
    if (n > 3500) {  // 路径长度上限保护(远小于 PATH_MAX)
        return 0;
    }
    pthread_mutex_lock(&g_dirLock);
    g_baseDir.assign(dir, n);
    pthread_mutex_unlock(&g_dirLock);

    // 兜底清理: 上一次进程被杀/断电留下的临时文件在这里被扫掉。
    TempArea stale;
    stale.root = std::string(dir, n) + "/" + kSubDirName;
    stale.many = stale.root + "/" + kManyDirName;
    stale.cleanup();
    return 1;
}

std::string auroraStorageInfoJson()
{
    std::string base;
    pthread_mutex_lock(&g_dirLock);
    base = g_baseDir;
    pthread_mutex_unlock(&g_dirLock);

    Caps caps;
    const bool haveDir = !base.empty();
    if (haveDir) {
        (void)probeStatvfs(base, caps);
        caps.memTotalBytes = readMemTotalBytes();
    }

    Opts o;
    std::string out;
    out.reserve(1024);
    out += "{\"ok\":";
    out += (haveDir && caps.statvfsOk) ? "true" : "false";
    out += ",\"section\":\"storage\"";
    out += ",\"dirSet\":";
    out += haveDir ? "true" : "false";
    out += ",\"baseDir\":\"" + esc(base) + "\"";
    out += ",\"statvfsOk\":";
    out += caps.statvfsOk ? "true" : "false";
    out += ",\"statvfsErrno\":" + jI64((long long)caps.statvfsErrno);
    out += ",\"availBytes\":" + jU64(caps.availBytes);
    out += ",\"totalBytes\":" + jU64(caps.totalBytes);
    out += ",\"memTotalBytes\":" + jU64(caps.memTotalBytes);
    out += ",\"needBytes\":" + jI64(o.needBytes());
    out += ",\"wouldRun\":";
    out += (caps.statvfsOk && caps.availBytes >= (unsigned long long)o.needBytes()) ? "true" : "false";
    out += ",\"oDirectMacro\":\"" + esc(kFactODirect) + "\"";
    out += ",\"fadviseMacro\":\"" + esc(kFactFadvise) + "\"";
    out += ",\"note\":\"这是只读探测, 不写任何文件; 运行期能力(O_DIRECT/fadvise 是否真的生效)"
           "只有在 storageRun 里实测后才会有结论。\"";
    out += "}";
    return out;
}

int auroraStorageCleanupNow()
{
    std::string base;
    pthread_mutex_lock(&g_dirLock);
    base = g_baseDir;
    pthread_mutex_unlock(&g_dirLock);
    if (base.empty()) {
        return 0;
    }
    TempArea area;
    area.root = base + "/" + kSubDirName;
    area.many = area.root + "/" + kManyDirName;
    area.cleanup();
    return (area.leftover() == 0) ? 1 : 0;
}

std::string auroraStorageRunJson(const std::string& optionsJson)
{
    const double tStart = nowMs();

    Opts o;
    const std::string clamped = parseOpts(optionsJson, o);

    std::string base;
    pthread_mutex_lock(&g_dirLock);
    base = g_baseDir;
    pthread_mutex_unlock(&g_dirLock);

    if (base.empty()) {
        return skipJson("未设置沙箱目录: ArkTS 侧必须先调用 storageSetDir(context.filesDir)",
                        o, Caps(), std::string(), o.needBytes(), clamped);
    }

    // ---- 空间检查: 不够就跳过, 一个文件都不写 ----
    Caps caps;
    caps.memTotalBytes = readMemTotalBytes();
    if (!probeStatvfs(base, caps)) {
        return skipJson(std::string("statvfs 失败(errno=") + jI64((long long)caps.statvfsErrno) +
                            "), 读不到可用空间 —— 宁可跳过也不赌",
                        o, caps, base, o.needBytes(), clamped);
    }
    const long long need = o.needBytes();
    if (caps.availBytes < (unsigned long long)need) {
        char b[512];  // 中文是 3 字节/字, 留足余量避免截断出半个 UTF-8 序列
        snprintf(b, sizeof(b),
                 "可用空间不足: 需要 %lld MiB(测试文件 %lld MiB + 小文件 %lld B + 安全余量 %lld MiB), "
                 "可用 %llu MiB —— 已跳过, 未创建任何文件",
                 need / kOneMiB, o.seqBytes() / kOneMiB, o.smallFiles * o.smallFileBytes,
                 o.marginBytes() / kOneMiB, caps.availBytes / kOneMiB);
        return skipJson(std::string(b), o, caps, base, need, clamped);
    }

    RunCtx c;
    c.opt = o;
    c.caps = caps;
    c.tStart = tStart;

    TempArea area;
    area.root = base + "/" + kSubDirName;
    area.many = area.root + "/" + kManyDirName;
    c.area = &area;

    std::string fatal;
    const int mk = mkdir(area.root.c_str(), 0700);
    if (mk != 0 && errno != EEXIST) {
        const int e = errno;
        return skipJson(std::string("创建临时目录失败 errno=") + jI64((long long)e), o, caps, base,
                        need, clamped);
    }

    auroraSetCurrentItem("存储 I/O", "存储 I/O 小节", 1, 1);
    try {
        // 运行期能力实测(会写两个极小的探针文件并立刻删掉)
        (void)probeODirect(area.root, caps.oDirectErrno);
        caps.oDirectWorks = (caps.oDirectErrno == 0);
        (void)probeFadvise(area.root, caps.fadviseRet, caps.fadviseErrno);
        c.caps = caps;

        c.bypassMethod = caps.oDirectWorks
            ? "O_DIRECT"
            : ((caps.fadviseRet == 0) ? "posix_fadvise(POSIX_FADV_DONTNEED)" : "none");
        if (!caps.oDirectWorks) {
            c.warnings.push_back(std::string("O_DIRECT 实测不可用(errno=") +
                                 jI64((long long)caps.oDirectErrno) +
                                 "), 冷读/4K 随机项改用 posix_fadvise(DONTNEED) 或普通读口径。");
        }
        if (caps.fadviseRet != 0) {
            c.warnings.push_back(std::string("posix_fadvise(POSIX_FADV_DONTNEED) 实测返回非 0(ret=") +
                                 jI64((long long)caps.fadviseRet) + ", errno=" +
                                 jI64((long long)caps.fadviseErrno) + "), 丢缓存可能不生效。");
        }

        const long long seqBytes = o.seqBytes();
        if (caps.memTotalBytes == 0) {
            c.warnings.push_back("读不到 /proc/meminfo 的 MemTotal, 无法判断测试文件是否装得进页缓存; "
                                 "缓存口径的项请按“上界”理解。");
            c.fileFitsInPageCache = false;
        } else {
            // 粗略判据: 文件小于物理内存的一半时, 页缓存完全装得下(还有其它进程在用内存)
            c.fileFitsInPageCache = ((unsigned long long)seqBytes * 2ULL) < caps.memTotalBytes;
            if (c.fileFitsInPageCache && !caps.oDirectWorks) {
                c.warnings.push_back("测试文件(" + jI64(seqBytes / kOneMiB) +
                                     " MiB)相对物理内存很小且 O_DIRECT 不可用: 不绕过缓存的读数会被"
                                     "放大, 只能作上界参考。");
            }
        }

        runPhases(c);
    } catch (const std::exception& e) {
        fatal = std::string("C++ 异常: ") + e.what();
    } catch (...) {
        fatal = "未知 C++ 异常(可能是内存不足)";
    }

    auroraSetIdleMarker();   // 同 napi_init.cpp markIdle: 空闲标记必须走这个入口

    // 显式清理(析构里还会再调一次, 但 cleanup() 幂等)
    area.cleanup();
    if (area.leftover() != 0 || area.errors != 0) {
        c.warnings.push_back("临时文件没有清干净: 残留 " + jI64((long long)area.leftover()) +
                             " 个条目, 删除失败 " + jI64((long long)area.errors) + " 次。");
    }

    return composeResultJson(c, area, base, fatal, clamped, false, std::string(), 0);
}

// ===========================================================================
// napi 接线
// ===========================================================================
namespace {

struct StorageWork {
    napi_env env = nullptr;
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string options;
    std::string result;
};

void ExecuteStorage(napi_env env, void* data)
{
    (void)env;
    auto* w = static_cast<StorageWork*>(data);
    try {
        w->result = auroraStorageRunJson(w->options);
    } catch (const std::exception& e) {
        w->result = std::string("{\"ok\":false,\"section\":\"storage\",\"skipped\":true,"
                                "\"skipReason\":\"native 异常: ") + esc(e.what()) + "\"}";
    } catch (...) {
        w->result = "{\"ok\":false,\"section\":\"storage\",\"skipped\":true,"
                    "\"skipReason\":\"native 未知异常\"}";
    }
}

void CompleteStorage(napi_env env, napi_status status, void* data)
{
    auto* w = static_cast<StorageWork*>(data);
    napi_value value = nullptr;
    if (status == napi_ok && napi_create_string_utf8(env, w->result.c_str(), NAPI_AUTO_LENGTH, &value) == napi_ok) {
        napi_resolve_deferred(env, w->deferred, value);
    } else {
        napi_value message = nullptr;
        napi_value error = nullptr;
        napi_create_string_utf8(env, "storage bench failed", NAPI_AUTO_LENGTH, &message);
        napi_create_error(env, nullptr, message, &error);
        napi_reject_deferred(env, w->deferred, error);
    }
    napi_delete_async_work(env, w->work);
    delete w;
}

std::string jsStr(napi_env env, napi_value v)
{
    std::string out;
    if (v == nullptr) {
        return out;
    }
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok || len == 0) {
        return out;
    }
    out.resize(len + 1);
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, v, out.data(), len + 1, &copied) != napi_ok) {
        return std::string();
    }
    out.resize(copied);
    return out;
}

// storageSetDir(dir: string): boolean
napi_value StorageSetDir(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string dir = jsStr(env, argc > 0 ? args[0] : nullptr);
    napi_value out = nullptr;
    napi_get_boolean(env, auroraStorageSetDir(dir.empty() ? nullptr : dir.c_str()) != 0, &out);
    return out;
}

// storageInfo(): string
napi_value StorageInfo(napi_env env, napi_callback_info info)
{
    (void)info;
    const std::string json = auroraStorageInfoJson();
    napi_value out = nullptr;
    napi_create_string_utf8(env, json.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// storageCleanup(): boolean
napi_value StorageCleanup(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_get_boolean(env, auroraStorageCleanupNow() != 0, &out);
    return out;
}

// storageRun(optionsJson?: string): Promise<string>
napi_value StorageRun(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto* w = new StorageWork();
    w->env = env;
    w->options = jsStr(env, argc > 0 ? args[0] : nullptr);

    napi_value promise = nullptr;
    napi_create_promise(env, &w->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "aurorastorage", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, ExecuteStorage, CompleteStorage, w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

}  // namespace

void auroraStorageRegisterNapi(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"storageSetDir", nullptr, StorageSetDir, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"storageInfo", nullptr, StorageInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"storageRun", nullptr, StorageRun, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"storageCleanup", nullptr, StorageCleanup, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
}
