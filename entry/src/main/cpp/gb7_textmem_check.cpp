// ============================================================================
// Text Processing 语料的"字节逐位不变"离线验证器(独立小工具, 不参与 App 构建)
//
// 背景: gb7RunTextProcessing 现在实际使用的是"一次性连续生成"224 MiB 连续语料
//   (pageSize = 1 MiB, pages = 224; 见 gb7_batch2.cpp 的"内存保险"一节: 那次"按页生成"的
//   改造因为做不到逐字节一致而回退)。
//
// 本文件用来证明:修好状态交接之后, "按页生成"与"一次性连续生成"逐字节相同 ——
//   也就是那次省内存改造的前置条件已经满足(是否真的切过去, 由用户决定; 本文件不改产品代码)。
//
// 【2026-10-06 修复: 旧版为什么不一致】
//   旧表项记的是"扫描上一页时循环退出处"的 (i, state)。退出条件是 i >= limit, 而每次推进
//   最少 3 字节(词 2 + 分隔符 1), 所以 i 会越过页边界最多 8 字节(词最长 7 + 分隔符 1)。
//   逐页生成器的页内写指针却从 pageStart 开始, 于是在"上一页恰好填满、carry 为空"的页边界上,
//   它会用表项里那个被越过的偏移对应的状态去写页首 -> 页首整段错位。
//   本版把表项升级成"页起点的精确恢复描述符" (off, st, posInW, pend):
//     off    = p*pageSize(精确)
//     st     = 页起点处的 PRNG 状态(posInW==0 时是"抽词前", >0 时是"抽词后")
//     posInW = 0 表示下一个 token 抽词; >0 表示下一个 token 抽分隔符
//     pend   = 页首要先写出的字节(上一页页尾那个词的剩余部分, 至多 6 字节)
//   于是每一页都能独立生成, 不需要任何跨页 carry。
//
// 做法(两条路径都实现一遍, 按页生成逐字节与一次性生成比对):
//   (A) refCorpus()  —— 与原实现逐字相同的"一次性连续生成"
//   (B) buildTable() + genPage() —— 新实现: 先走一遍 token 序列建表(不留语料字节),
//       再按页生成并拼接
//   规模覆盖: 4 KiB x {2,3,5} 页、64 KiB x 5 页、1 MiB x {2,3} 页、2 MiB x 3 页、
//             1 MiB x 224 页(= 正式规模 224 MiB)逐字节比对。
//   另外打印起始状态表摘要与若干表项常量, 与 Python 版 verify_gb7_textmem.py 的输出逐字比对。
//
// 编译运行(本机没有 host C++ 工具链时, 用有 g++/clang++ 的机器直接编即可; 纯 CPU, 无设备依赖):
//   g++ -O2 -std=c++11 gb7_textmem_check.cpp -o textmem_check && ./textmem_check
//   注意: 1 MiB x 224 页那一档会同时持有两份 224 MiB 语料(约 450 MiB 内存), 内存小的机器
//         可以把 kScales 里最后一项注释掉 —— 前面 7 档已经覆盖了各种页边界形态。
// 期望输出: 每一档都 IDENTICAL, 最后 ALL PASS, 退出码 0。
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

inline uint32_t xsB2(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static const uint32_t kSeed = 31415u;
static const char* const kWords[16] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta",
                                       "theta", "iota", "kappa", "lambda", "mu", "nu", "xi",
                                       "omicron", "pi"};
static const int kWordLen[16] = {5, 4, 5, 5, 7, 4, 3, 5, 4, 5, 6, 2, 2, 2, 7, 2};

// ---------------- (A) 原实现: 一次性连续生成 ----------------
std::vector<uint8_t> refCorpus(size_t pageSize, size_t pages)
{
    const size_t total = pageSize * pages;
    std::vector<uint8_t> text(total);
    uint32_t s = kSeed;
    size_t i = 0;
    while (i < total) {
        xsB2(s);
        const int wi = (int)(s & 15u);
        const size_t len = (size_t)kWordLen[wi];
        if (i + len + 2 >= total) {
            break;                                  // 原实现的收尾条件
        }
        memcpy(text.data() + i, kWords[wi], len);
        i += len;
        xsB2(s);
        text[i++] = ((s & 7u) == 0) ? (uint8_t)'.' : (uint8_t)' ';
    }
    return text;
}

// ---------------- (B) 新实现: 起始状态表 + 按页生成 ----------------
struct PageEnt {
    uint64_t off;
    uint32_t st;
    uint8_t posInW;
    uint8_t pendLen;
    uint8_t pend[8];
};

// 建表: 只走一遍 token 序列, 不保存任何语料字节
std::vector<PageEnt> buildTable(size_t pageSize, size_t pages, size_t& stopOut)
{
    const size_t total = pageSize * pages;
    std::vector<PageEnt> tab(pages);
    uint32_t s = kSeed;
    size_t g = 0;
    uint8_t posInW = 0;
    int lnCur = 0;
    uint32_t sWord = s;
    const char* word = kWords[0];
    size_t stop = total;
    for (size_t p = 0; p < pages; ++p) {
        const size_t b = p * pageSize;
        PageEnt& e = tab[p];
        e.pendLen = 0;
        if (g >= total) {                           // 收尾之后的空页: 全 0
            e.off = total;
            e.st = s;
            e.posInW = 0;
            continue;
        }
        if (g == b) {                               // 页起点正好落在 token 边界上
            e.off = b;
            e.st = s;
            e.posInW = posInW;
        } else {                                    // 页起点落在一个词的中间
            e.off = b;
            e.st = sWord;
            e.posInW = (uint8_t)lnCur;
            e.pendLen = (uint8_t)(g + (size_t)lnCur - b);
            for (size_t k = b; k < g + (size_t)lnCur; ++k) {
                e.pend[k - b] = (uint8_t)word[k - g];
            }
            g += (size_t)lnCur;
            xsB2(s);
            g += 1;
            posInW = 0;
        }
        const size_t nb = b + pageSize;
        if (posInW != 0) {                          // 页首正好落在分隔符上: 先走掉它
            xsB2(s);
            g += 1;
            posInW = 0;
        }
        while (g < nb) {
            xsB2(s);
            const int wi = (int)(s & 15u);
            lnCur = kWordLen[wi];
            word = kWords[wi];
            if (g + (size_t)lnCur + 2 >= total) {   // 收尾: 流到此结束
                stop = g;
                g = total;
                break;
            }
            if (g + (size_t)lnCur > nb) {           // 这个词跨过页边界
                sWord = s;
                break;
            }
            g += (size_t)lnCur;
            if (g >= nb) {                          // 词正好收在页尾: 分隔符属于下一页
                posInW = (uint8_t)lnCur;
                break;
            }
            xsB2(s);                                // 抽分隔符
            g += 1;
        }
    }
    stopOut = stop;
    return tab;
}

// 单页生成: 只依赖表项, 不做任何跨页 carry
void genPage(const PageEnt& e, uint8_t* dst, size_t pageSize, size_t total)
{
    memset(dst, 0, pageSize);
    const size_t pageStart = (size_t)e.off;
    if (pageStart >= total) {
        return;
    }
    size_t right = pageStart + pageSize;
    if (right > total) {
        right = total;
    }
    const size_t lastByte = right - 1;
    uint32_t s = e.st;
    uint8_t posInW = e.posInW;
    size_t po = 0;
    size_t g = pageStart;
    for (uint8_t k = 0; k < e.pendLen; ++k) {
        dst[po++] = e.pend[k];
        ++g;
    }
    while (po < pageSize && g <= lastByte) {
        if (posInW == 0) {
            xsB2(s);
            const int wi = (int)(s & 15u);
            const size_t ln = (size_t)kWordLen[wi];
            if (g + ln + 2 >= total) {
                break;
            }
            size_t n = lastByte - g + 1;
            if (ln < n) {
                n = ln;
            }
            memcpy(dst + po, kWords[wi], n);
            po += n;
            if (n < ln) {                           // 词尾越出本页 -> 下一页的表项接着写
                break;
            }
            g += ln;
            posInW = (uint8_t)ln;
        }
        xsB2(s);
        if (g > lastByte || po >= pageSize) {
            break;                                  // 分隔符属于下一页
        }
        dst[po] = ((s & 7u) == 0) ? (uint8_t)'.' : (uint8_t)' ';
        ++po;
        ++g;
        posInW = 0;
    }
}

std::vector<uint8_t> fastCorpus(size_t pageSize, size_t pages, const std::vector<PageEnt>& tab)
{
    const size_t total = pageSize * pages;
    std::vector<uint8_t> text(total);
    std::vector<uint8_t> page(pageSize);
    for (size_t p = 0; p < pages; ++p) {
        genPage(tab[p], page.data(), pageSize, total);
        memcpy(text.data() + p * pageSize, page.data(), pageSize);
    }
    return text;
}

uint64_t fnv(const std::vector<uint8_t>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < v.size(); ++i) {
        h ^= v[i];
        h *= 1099511628211ull;
    }
    return h;
}

// 表摘要: 与 Python 版 verify_gb7_textmem.py 的 digest_table() 逐字相同的折法
uint64_t digestTable(const std::vector<PageEnt>& tab)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < tab.size(); ++i) {
        h = (h ^ (uint64_t)tab[i].off) * 1099511628211ull;
        h = (h ^ (uint64_t)tab[i].st) * 1099511628211ull;
        h = (h ^ (uint64_t)(((uint32_t)tab[i].posInW << 8) | (uint32_t)tab[i].pendLen)) * 1099511628211ull;
        for (uint8_t k = 0; k < tab[i].pendLen; ++k) {
            h = (h ^ (uint64_t)tab[i].pend[k]) * 1099511628211ull;
        }
    }
    return h;
}

bool run(size_t pageSize, size_t pages)
{
    const size_t total = pageSize * pages;
    const std::vector<uint8_t> ref = refCorpus(pageSize, pages);
    size_t stop = 0;
    const std::vector<PageEnt> tab = buildTable(pageSize, pages, stop);
    const std::vector<uint8_t> fast = fastCorpus(pageSize, pages, tab);
    bool ok = (ref.size() == fast.size());
    size_t firstDiff = (size_t)-1;
    size_t diffCount = 0;
    if (ok) {
        for (size_t i = 0; i < ref.size(); ++i) {
            if (ref[i] != fast[i]) {
                if (firstDiff == (size_t)-1) {
                    firstDiff = i;
                }
                ++diffCount;
            }
        }
        ok = (diffCount == 0);
    }
    bool offOk = true;
    for (size_t p = 0; p < pages; ++p) {
        if (tab[p].off != p * pageSize && tab[p].off != total) {
            offOk = false;
        }
    }
    const bool tailOk = (stop <= total) && (total - stop <= 8);
    printf("  %4zu 页 x %9zu B (=%7.2f MiB): ref=%016llx fast=%016llx %s | 表偏移精确=%s 停止=%zu(尾 %zu)\n",
           pages, pageSize, (double)total / 1048576.0,
           (unsigned long long)fnv(ref), (unsigned long long)fnv(fast),
           (ok ? "IDENTICAL" : "DIFFERENT"), (offOk ? "是" : "否"), stop, total - stop);
    if (!ok) {
        printf("     !! 首个不同字节 offset=%zu (= 页 %zu + %zu), 不同字节数=%zu\n",
               firstDiff, firstDiff / pageSize, firstDiff % pageSize, diffCount);
    }
    return ok && offOk && tailOk;
}

} // namespace

int main()
{
    printf("gb7 Text Processing 语料字节不变性验证(一次性连续生成 vs 逐页生成)\n");
    bool ok = true;
    const size_t kScales[][2] = {{4096, 2}, {4096, 3}, {4096, 5}, {65536, 5},
                                 {1024 * 1024, 2}, {1024 * 1024, 3}, {2 * 1024 * 1024, 3},
                                 {1024 * 1024, 224}};
    for (size_t i = 0; i < sizeof(kScales) / sizeof(kScales[0]); ++i) {
        ok = run(kScales[i][0], kScales[i][1]) && ok;
    }
    // 正式规模的表常量(与 Python 版打印的"交叉核对常量"逐字比对)
    const size_t PS = 1024 * 1024;
    const size_t PG = 224;
    size_t stop = 0;
    const std::vector<PageEnt> tab = buildTable(PS, PG, stop);
    printf("\n交叉核对常量(应与 Python 版 verify_gb7_textmem.py 一致):\n");
    const size_t idx[6] = {0, 1, 95, 96, 191, PG - 1};
    for (size_t i = 0; i < 6; ++i) {
        const PageEnt& e = tab[idx[i]];
        printf("    表项[%3zu] = {off=%9llu, state=0x%08X, posInW=%u, pend=", idx[i],
               (unsigned long long)e.off, e.st, (unsigned)e.posInW);
        if (e.pendLen == 0) {
            printf("-}\n");
        } else {
            for (uint8_t k = 0; k < e.pendLen; ++k) {
                printf("%02x", e.pend[k]);
            }
            printf("}\n");
        }
    }
    printf("    起始状态表摘要 = 0x%016llX\n", (unsigned long long)digestTable(tab));
    printf("    停止位置 = %zu\n", stop);
    printf("%s\n", ok ? "ALL PASS: 按页生成与一次性连续生成逐字节相同" : "FAILED");
    return ok ? 0 : 1;
}
