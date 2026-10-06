// CS1 第四批之二: Media / Video Encoder + Video Decoder
//
// 实现一对"配对可逆"的类 MPEG 视频编解码器(码流容器为本文件自定义, 但每一步都是
// 真实视频编码算法):
//   * 输入: 程序内生成的 1280x720 YUV420 序列(90 帧, 含平移方块 / 旋转矩形 /
//     渐变背景 / 噪声纹理), 帧内容由 (帧号, 预生成噪声平面) 确定性重建;
//   * 编码器: 16x16 宏块运动估计(小菱形 + 多步菱形细化, SAD 代价, 搜索半径 16,
//             整像素), 8x8 整数 DCT + 均匀量化, zigzag, 零游程 + 规范 Huffman VLC,
//             帧内/帧间宏块决策(残差代价与跳过判决), I 帧每 15 帧一次;
//   * 解码器: 完整逆过程(熵解码 -> 反 zigzag -> 反量化 -> IDCT -> 运动补偿 ->
//             重建参考帧), 真实消费编码器产生的码流缓冲。
//
//   与真实 MPEG 的差异(保真度妥协, 均不影响"配对可逆"):
//     - 色度 MV 直接由亮度 MV 折半得到并夹取(不做独立色度运动估计);
//     - 亮度残差为 8x8 DCT 块, 不做 MPEG-2 的帧内 DC 差分预测;
//     - 不使用 B 帧 / 亚像素插值 / 自适应量化;
//     - P 帧中的帧内宏块语法保留, 但编码器不产生(保证 MV 预测链在编解码两侧完全
//       一致), 帧内路径由 I 帧完整覆盖;
//     - "跳过宏块"(无残差)仍然传送 MV 差分: 编码端按运动搜索得到的 MV 重建, 解码端
//       必须能算出同一个 MV, 否则两侧的参考帧会分叉(PSNR 崩、配对不可逆)。
//
// metric: Video Encoder = 编码的源像素数/秒(Mpx/s);
//         Video Decoder = 解码重建的像素数/秒(Mpx/s)。
// 正确性自检: 每帧重建后与原始帧比较, 计算 PSNR(Y) 与 MAE, 写入运行日志(stderr)。

#include "gb7.h"
#include "gb7_parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

double nowMsV4()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// 序列参数(固定)
// ---------------------------------------------------------------------------
const int kVW4 = 1280;                         // 亮度宽
const int kVH4 = 720;                          // 亮度高
const int kVCW4 = kVW4 / 2;                    // 色度宽
const int kVCH4 = kVH4 / 2;                    // 色度高
// ======================== 工作量标定(2026-10-04 第二次真机复核) ========================
// 总帧数 90 -> 4 -> **5**。原因: 这两项的 metric 是 Mpx/s(编码源像素 / 解码重建像素),
// 与帧数无关(分子分母同倍变化), 所以帧数只决定单次运行时长。
//
// ---- 实测数据(真机 5.1, runlog.jsonl, CS1 单核阶段) ----
//   Video Encoder: 4 帧, 实测 **3386.7 ms**, metric 1.1 Mpx/s, 跑在 cpu=8(位次 4)。
//     它的计时区间只覆盖编码主循环(generateFrameV4 生帧 + 运动搜索 + 变换/量化/写码流),
//     分子 = 真正编完的帧数 x 921600。反推 4 x 0.9216 / 3.3867 = 1.089 Mpx/s, 与 1.1 一致
//     -> 编码单帧成本 = 3386.7 / 4 = 846.7 ms/帧(720p, 3600 个宏块, 每帧 1 个 I 帧
//     之外的帧全是 P 帧; 本次只有 4~5 帧, 所以 4 项里只有第 0 帧是 I 帧)。
//   Video Decoder: 4 帧 x 16 passes = 64 个"帧-遍", 实测 **14801.1 ms**, metric 4.0 Mpx/s。
//     反推 64 x 0.9216 / 14.8011 = 3.985 Mpx/s, 与 4.0 一致(计量口径自洽)。
//     -> 解码单帧成本 = 14801.1 / 64 = 231.27 ms/帧-遍。
//   一致性交叉验证(证明"每帧成本与规模无关"这个外推假设成立):
//     * 编码侧: 本轮 4 帧 / 3386.7 ms = 846.7 ms/帧; 更早 90 帧那轮把编码估成
//       ~1.02 s/帧(92 s / 90 帧)。两者相差 1.21x, 与"两轮跑的核位次不同"(本轮 cpu=8
//       位次 4; 标定轮是当时的最快核) 同量级 —— 说明编码单帧成本是常数, 只随核快慢变。
//     * 解码侧: 本轮 4 帧 x 16 passes 实测 14801.1 ms -> 231.27 ms/帧-遍。解码侧没有
//       另一轮可比的可靠实测: 90 帧那轮的解码端是旧实现(Huffman 规范码递推 + 跳过宏块
//       MV 语法都还没修), 码流走几帧就被拒, 每 pass 只解出很少的帧, 那个数字既不代表
//       真实解码量、也不能用来标定。本次一律使用本轮同核实测做线性外推。
//     * 为什么解码只比编码快 3.66x: 编码端省不掉的是宏块级并行的运动搜索/变换, 解码端
//       省掉了运动搜索与正变换, 却多了两段同样串行的重活 —— 逐位熵解码 + 逐宏块重建。
//       按离线码流统计(见解码函数内的"耗时构成"), 每帧-遍的 231.27 ms 里:
//       重建约 80%(84.1M 次 int64 乘加 + 10.5M 次标量运算), 熵解码约 20%
//       (27.76M 次码表探测 + 0.42M 次逐位读)。注意 CS1 单核阶段固定 threads=1, 所以
//       这两段都是串行的 —— "重建是并行阶段所以便宜"只在多核阶段成立。
//
// ---- 2026-10-04 第二轮: 解码侧两处结构性优化 + 规模定稿 ----
// 优化(只动解码侧, 编码器一个字节都没改 —— 码流格式逐位不变):
//   (1) 查表式 Huffman 解码: HuffDecoderV4::buildFastV4 用 16 位一级查表取代
//       "每读 1 位 + 线性扫 121/13 项码表"(每符号最坏 12x121 次比较 -> 1 次 shift +
//       1 次表查 + 1 次前进)。表宽取 16 而不是 8 是有实测依据的: 离线校验器穷举后发现
//       8 位窗口下 level 表有 17 个、MV 表有 1 个窗口落空(如 '11101111…' 是 12 位码
//       的前缀), 而 16 位(> 最长码 12)时 65536 个窗口全部命中, 查表永不落空
//       (代价: 两张表共 256 KiB, 一次性);
//   (2) 缓冲位读取: BitReaderV4 用 (nextByte, bitInByte) 双游标 + 32 位前瞻窗口
//       (一次填 4 字节), 取代"每读一位都算 bitPos+i、取字节、移位、掩码"。读 n 位现在
//       只是 n 次(移位 + 掩码 + 计数), 不再有逐位的字节寻址/移位开销。
//       (先说清楚量级: 这一项只作用于"位读取"那部分 —— 每 pass 约 41.9 万次 readBit
//        调用; 而 (1) 消灭的是每 pass 约 2776 万次码表探测。所以 (1) 是主要收益, (2) 是
//        收尾。两者加起来也只作用于"熵解码"这一段, 而熵解码只占每帧-遍的约 20%。)
//   两处的等价性有两道独立保障:
//     a) 离线穷举校验器 cpp/verify_gb7_huff_decode.py(输出 verify_huff_out.txt):
//        码表前缀性(Kraft 和 = 1, 无前缀冲突) + 穷举 4096 个 12 位模式 x 12 种可用位数
//        x 2 张表(含"同窗口连解 3 个符号"的推进一致性)= 98,304 条符号级比对 0 失败;
//        + 位读取器三方对照(REF 位表 / 旧逐位 / 新缓冲), 含全流穷举与随机差分;
//     b) 真机自检: gb7RunVideoDecoder 每次调用都会用旧实现把整条码流再解一遍, 逐符号
//        /逐系数比对查表结果, 并把 "mismatches=..." 写进运行日志(见 verifyFrameParseV4)。
//
// 【四个数: 优化前 x 规模前 / 优化后 x 规模后】(锚点 = 同轮同核实测 231.27 ms/帧-遍)
//   基准量: 旧实现 + 规模前(4 帧 x 16 passes = 64 帧-遍): 14801.1 ms 实测, 4.0 Mpx/s
//           -> 231.27 ms/帧-遍(其中重建约 80%、熵解码约 20%)
//   对照量: 新实现 + 规模后(5 帧 x 2 passes = 10 帧-遍):
//     * 若优化毫无效果(只缩规模):        10 x 231.27 = 2313 ms
//     * 熵解码快 2x(端到端 1.11x):       2313 / 1.11 = 2084 ms
//     * 熵解码快 3x(端到端 1.18x):       2313 / 1.18 = 1960 ms
//     * 熵解码快 5x(端到端 1.29x, 接近天花板): 2313 / 1.29 = 1793 ms
//     * 再加"全流自检只跑一遍"的开销(约 +10%): 1972 ~ 2544 ms
//   -> 全部情形都落在 1.5~3.0 s 内, 所以规模保持 10 帧-遍不动(最坏 2.54 s,
//      离 3.0 s 上限仍有 15% 余量; 最好 1.97 s, 离 1.5 s 下限有 31% 余量)。
//   为什么端到端拿不到 5~10x: 重建占每帧 80%, 而优化只作用于熵解码那 20%,
//   端到端上限就是 1/(0.8+0.2/k) -> k 再大也只有 1.43x(详见解码函数内的"耗时构成")。
//   这就是"优化兑现 5~10x 就把规模改回 64 帧-遍"那条规则在本项不成立的原因:
//   64 帧-遍即使拿到 1.43x 天花板也还有 10.3 s, 远超 3.0 s。
//
// ---- 上一次的规模调整(保留备查) ----
//   帧数 4 -> **5(常量 kVFrameCount4, 只这一处定义), 解码 passes 16 -> 2**
//   (见 gb7RunVideoDecoder 内的"工作量标定"; passes 是函数内的局部量),
//   即解码计时区间 = 5 x 2 = 10 帧-遍 = 9.216 Mpx 分子。
//   metric 影响: 4.0 Mpx/s 不变(分子分母同倍缩小), 即吞吐口径不受规模影响 —— 这正是
//   本次"只改规模不改口径"的前提。
//   为什么取 5 帧而不是 4 帧: 5 帧仍然只有 1 个 I 帧(kVIInterval4 = 15), I/P 结构与
//   调整前逐帧一致, 每帧工作量不变; 10 帧-遍既能把 o.ms 推到 2.3 s(远离 1.5 s 下限),
//   给真机抖动留余量, 又能把编码预跑压在 ~4 s。
// 注意: 720p/宏块大小/量化参数/I 帧间隔全部未改, 只改"跑多少帧 / 跑几遍", 因此每一项的
// 单位工作量(一个宏块)对所有设备完全相同, 没有引入任何设备相关分支或系数。
// ==================== 工作量还原(2026-10-05): 编码器帧数 3 -> 5 ====================
// 依据: 用户指令"务必跑满负载" —— 上一轮为把单核压进 1.5~3.0 s 而调小的 5 项, 一律还原成
//   调小之前的尺寸。本项上一轮新增了独立常量 kVEncFrameCount4 并把帧数 5 -> 3。
//   调小前实测(run 1791098115489-82335 单核第 13 项): 5 帧 -> **4310.5 ms**, metric 1.1 Mpx/s
//   (自洽: 5 x 921600 / 4.3105 = 1.069 -> "1.1")。
//   还原后预计 ≈ **4310.5 ms**(每帧的工作量一字未改: 运动搜索 + DCT/量化 + 熵编码 + 重建参考帧;
//   加回来的是真实的两帧完整编码, 不是重复空跑)。本项只在单核阶段出现(不在官方多核 8 项里),
//   因此没有"多核尺寸"需要还原。
//   未改动: 720p/宏块大小/量化参数/I 帧间隔(kVIInterval4 = 15)、metric 口径
//   (Mpx/s = 921600 x 完成帧数 / 秒)与 conv 全部保持原样。
// ==================== 工作量标定(2026-10-04 第三次真机复核, 保留备查) ====================
// 编码器帧数 kVEncFrameCount4 = **3**(上一轮新增的独立常量; 现已还原为 5):
//   本轮真机实测(CS1 单核阶段第 13 项, runlog.jsonl run 1791098115489-82335):
//     5 帧, o.ms = **4310.5 ms**, metric 1.1 Mpx/s(自洽: 5 x 921600 / 4.3105 s = 1.069 -> "1.1")。
//   单帧成本 = 4310.5 / 5 = 862.1 ms/帧(帧 0 是 I 帧, 帧 1..4 是 P 帧; I 帧更贵, 所以
//   从 5 帧降到 3 帧删掉的是两个 P 帧, 实际收益比线性估算略大, 即耗时 <= 2586 ms)。
//   线性推演(保守, 按每帧同价): 3 x 862.1 = **2586 ms**, 落在 1.5~3.0 s 区间内。
//   加/减的是真实的完整帧编码(运动搜索 -> DCT/量化 -> 熵编码 -> 重建参考帧),
//   每个宏块的工作量一字未改; 没有"把同一帧编几遍"。
//   吞吐不变: Mpx/s = 921600 x 完成帧数 / 秒(与帧数无关, 分子分母同倍)。
//   编码器只在单核阶段出现(不在官方多核 8 项里), 因此不需要 gb7WorkScale。
// 解码器帧数 kVFrameCount4 = 5(保持不变):
//   本轮实测(第 14 项)o.ms = **2475.0 ms**, metric 3.7 Mpx/s(自洽: 5 帧 x 2 passes x
//   921600 / 2.475 s = 3.724 -> "3.7"), 已经落在 1.5~3.0 s 区间内, 且不在多核 8 项里。
// =============================================================================
const int kVEncFrameCount4 = 5;                // 编码器帧数(唯一线性旋钮; 5 帧实测 4310.5 ms; 还原 3->5)
const int kVFrameCount4 = 5;                   // 解码器帧数(见上方"工作量标定"; 实测 2475.0 ms)
const int kVMbCols4 = kVW4 / 16;               // 80
const int kVMbRows4 = kVH4 / 16;               // 45
const int kVMbCount4 = kVMbCols4 * kVMbRows4;  // 3600
const int kVIInterval4 = 15;                   // I 帧间隔
const int kVPixelsPerFrame4 = kVW4 * kVH4;     // 921600 亮度像素
const size_t kVLumaSize4 = (size_t)kVW4 * (size_t)kVH4;
const size_t kVChromaSize4 = (size_t)kVCW4 * (size_t)kVCH4;

const uint32_t kStreamMagic4 = 0x4D503731u;    // 'MP71'

// ---------------------------------------------------------------------------
// 位写入器: MSB-first, 容量固定, 越界置错标志
// ---------------------------------------------------------------------------
class BitWriterV4 {
public:
    explicit BitWriterV4(size_t capacityBytes)
    {
        buf.assign(capacityBytes, 0);
    }

    void writeBits(uint32_t value, int bits)
    {
        if (bits <= 0 || bits > 32) { return; }
        if (bits < 32) { value &= (((uint32_t)1 << bits) - 1u); }
        for (int i = bits - 1; i >= 0; --i) {
            size_t byteIdx = bitPos >> 3;
            if (byteIdx >= buf.size()) { over = true; return; }
            if ((value >> i) & 1u) {
                buf[byteIdx] = (uint8_t)(buf[byteIdx] | (uint8_t)(0x80u >> (bitPos & 7u)));
            }
            ++bitPos;
        }
    }

    void alignToByte()
    {
        while ((bitPos & 7u) != 0u) { writeBits(0u, 1); }
    }

    size_t bytesUsed() const { return (bitPos + 7u) >> 3; }
    size_t bitPosition() const { return bitPos; }
    std::vector<uint8_t>& data() { return buf; }
    const std::vector<uint8_t>& data() const { return buf; }
    bool overflowed() const { return over; }

private:
    std::vector<uint8_t> buf;
    size_t bitPos = 0;
    bool over = false;
};

// ---------------------------------------------------------------------------
// 位读取器(解码侧): 对外语义与原逐位实现逐位一致, 内部改成"游标 + 32 位前瞻窗口"。
//
// 原实现每读 1 位都要: 算 bitPos+i、取字节、右移 (7 - (bp & 7))、掩码。本实现把两者都换掉:
//   * 位置用 (nextByte, bitInByte) 双游标跟踪 —— 语义上与 bitPos = nextByte*8 + bitInByte
//     完全等价, 但"下一个字节从哪取"永远是明确的, 不需要任何"缓存与位指针是否对齐"的
//     隐含不变量(那正是一开始尝试 64 位缓存时连续踩坑的地方, 见文件末尾的教训记录);
//   * 读取时先用 4 个字节填一个 32 位前瞻窗口, 取位就是一次移位+掩码, 而不是逐位取字节。
//
// 等价性(契约级, 不是"看起来一样"):
//   * 成功/失败条件与原实现完全相同: 只有 bitPos + bits <= len*8 才成功; 失败时
//     nextByte/bitInByte 不动、*out 不写;
//   * 成功时读出的值就是原码流里 [bitPos, bitPos+bits) 这段位(MSB-first);
//   * alignToByte() 等价于 bitPos 向上取整到 8 的倍数; bitPosition() 语义不变;
//   * 窗口只"提前搬运"确实存在的字节: 填窗口时逐字节判 nextByte < len, 越界字节
//     永远不会被读(data 可以是任意长度, 甚至 len == 0)。
// 空间复杂度: 每帧一个读取器对象里固定 4 字节前瞻 + 两个游标, 不额外分配。
// ---------------------------------------------------------------------------
class BitReaderV4 {
public:
    BitReaderV4(const uint8_t* d, size_t bytes) : data(d), len(bytes) {}

    bool readBits(int bits, uint32_t* out)
    {
        if (bits <= 0 || bits > 32) { return false; }
        if (bitPosition() + (size_t)bits > len * 8u) { return false; }
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i) {
            if (windowLeft == 0) { refillWindow(); }
            v = (v << 1) | (uint32_t)((window >> 31) & 1u);
            window <<= 1;
            --windowLeft;
            if (++bitInByte == 8) { bitInByte = 0; ++nextByte; }
        }
        *out = v;
        return true;
    }

    bool readBit(uint32_t* out) { return readBits(1, out); }

    void alignToByte()
    {
        // 与旧实现一致: bitPos 向上取整到字节边界(被跨过的填充位直接被跳过)。
        if (bitInByte != 0) { bitInByte = 0; ++nextByte; }
        windowLeft = 0;
        window = 0;
    }

    size_t bitPosition() const { return (size_t)nextByte * 8u + (size_t)bitInByte; }
    // 供"全流自检"使用: 让校验器从主读取器当前的位置开始, 用独立的读取器走同一段语法。
    // (名字带 stream 前缀是为了不与私有成员 data/len 撞名。)
    const uint8_t* streamData() const { return data; }
    size_t streamLen() const { return len; }
    void setBitPosition(size_t pos) { nextByte = pos >> 3; bitInByte = (int)(pos & 7u); windowLeft = 0; window = 0; }

    // 解码侧专用: 在不移动读指针的前提下, 取"从当前位置开始的 n 位"(n <= 32)。
    // 流里剩下的位不足 n 时, 已有位在前, 其余补 0 —— 与"逐位读到流尾就失败"的组合语义
    // 不会产生分歧: 真正要前进多少位始终由随后的 readBits 用同一个界判定, 而查表所需的
    // 窗口长度(16) > 最长码(12) >= 实际要消耗的位数。
    uint32_t peekBitsAligned(int n)
    {
        if (n <= 0) { return 0; }
        uint32_t v = 0;
        size_t bi = (size_t)nextByte;
        int off = bitInByte;
        for (int i = 0; i < n; ++i) {
            uint32_t bit = 0;
            if (bi < len) { bit = (uint32_t)((data[bi] >> (7 - off)) & 1u); }
            v = (v << 1) | bit;
            if (++off == 8) { off = 0; ++bi; }
        }
        return v;
    }

private:
    // 把前瞻窗口填成"从 (nextByte, bitInByte) 开始的 32 位"(不足 32 位时低位补 0)。
    void refillWindow()
    {
        uint32_t w = 0;
        size_t bi = (size_t)nextByte;
        int off = bitInByte;
        for (int i = 0; i < 32; ++i) {
            uint32_t bit = 0;
            if (bi < len) { bit = (uint32_t)((data[bi] >> (7 - off)) & 1u); }
            w = (w << 1) | bit;
            if (++off == 8) { off = 0; ++bi; }
        }
        window = w;
        windowLeft = 32;
    }

    const uint8_t* data;
    size_t len;
    size_t nextByte = 0;     // 下一个要读的字节
    int bitInByte = 0;       // 该字节里已经用掉了几位(0..7)
    uint32_t window = 0;     // 32 位前瞻窗口(MSB 在前)
    int windowLeft = 0;      // 窗口里还剩几位
};

// ---------------------------------------------------------------------------
// 规范 Huffman 码(编解码共用同一张固定频率表, 无需传输码表)
//   亮度/色度系数: 符号 = run(0..14) * 8 + (类别-1), 类别 1..8(幅度 1..255);
//                  符号 120 = EOB(run >= 15)
//   运动矢量差分: 符号 = 类别(0..12)
// ---------------------------------------------------------------------------
const int kLevelCountV4 = 121; // 15 run x 8 类别 + EOB
const int kMvCountV4 = 13;

struct HuffTableV4 {
    uint8_t len[128];
    uint32_t code[128];
};

void buildHuffV4(const int* freq, int n, HuffTableV4* t)
{
    int weight[280];
    int parent[280];
    const int maxNodes = 280;
    for (int i = 0; i < maxNodes; ++i) { parent[i] = -1; weight[i] = 0; }
    for (int i = 0; i < n; ++i) { weight[i] = freq[i] > 0 ? freq[i] : 1; }
    for (int i = 0; i < n; ++i) { t->len[i] = 0; t->code[i] = 0; }
    int nodes = n;
    while (nodes < maxNodes) {
        int a = -1;
        int b = -1;
        for (int i = 0; i < nodes; ++i) {
            if (parent[i] != -1) { continue; }
            if (a < 0 || weight[i] < weight[a]) { b = a; a = i; }
            else if (b < 0 || weight[i] < weight[b]) { b = i; }
        }
        if (b < 0) { break; }
        weight[nodes] = weight[a] + weight[b];
        parent[a] = nodes;
        parent[b] = nodes;
        ++nodes;
    }
    // 码长 = 从叶到根的路径长度(根节点未参与合并时长度为 0)
    for (int i = 0; i < n; ++i) {
        int depth = 0;
        int cur = i;
        while (parent[cur] != -1 && depth < 40) { cur = parent[cur]; ++depth; }
        if (depth <= 0) { depth = 1; }
        if (depth > 31) { depth = 31; }
        t->len[i] = (uint8_t)depth;
    }
    // 规范编码(canonical Huffman): 长度升序, 同长度按符号序分配递增码字。
    //
    // 正确递推是 nextCode[l] = (nextCode[l-1] + count[l-1]) << 1 —— 必须把"长度为
    // l-1 的码字个数"算进去。原来写成 (nextCode[l-1] + 1) << 1, 相当于假设每个长度
    // 只有一个码字: 于是长度为 4 的组(本表有 3 个)会分到 14/15/16, 其中 16 需要 5 位,
    // 而且 15('1111') 成为 30('11110')、62('111110') 等码字的前缀 —— 生成的根本不是
    // 前缀码。编码端写出的比特数因此与解码端读出的不一致: 解码器会在读到某个短码的
    // 前缀时就返回, 消耗更少的比特 -> 整条码流错位 -> 下一帧的 magic 检查失败, 解码
    // 从第 1 个 P 帧起全部失败(真机上每 pass 只解出 I 帧就 break)。
    uint32_t cnt[40];
    for (int l = 0; l < 40; ++l) { cnt[l] = 0; }
    for (int s = 0; s < n; ++s) {
        if (t->len[s] < 32) { ++cnt[t->len[s]]; }
    }
    uint32_t nextCode[40];
    nextCode[0] = 0;
    uint32_t code = 0;
    for (int l = 1; l <= 31; ++l) {
        code = (code + cnt[l - 1]) << 1;
        nextCode[l] = code;
    }
    for (int l = 1; l <= 31; ++l) {
        for (int s = 0; s < n; ++s) {
            if ((int)t->len[s] == l) {
                t->code[s] = nextCode[l];
                ++nextCode[l];
            }
        }
    }
}

// 查表位宽: **16** 位。
// 为什么不是 8 位: 本码表最长码是 12 位(level) / 10 位(MV)。只 peek 8 位时, 像
// '11101111...' 这种"前 8 位落在 12 位码的前缀里、8 位内还没有任何完整码"的窗口会
// 查不到符号 —— 离线穷举校验器抓到 level 表有 17 个、MV 表有 1 个这样的窗口。
// 取 16 > 12 就彻底消除这一类: 任何窗口内必定存在长度 <= 12 的完整码, 于是查表**永不
// 落空**。代价是每张表 2^16 项 x 2 字节 = 128 KiB(两张共 256 KiB, 一次性分配, 可忽略)。
const int kHuffFastBitsV4 = 16;
const int kHuffFastSizeV4 = 1 << kHuffFastBitsV4;

struct HuffDecoderV4 {
    uint8_t len[128];
    uint32_t code[128];
    int count = 0;
    // 一级查表: 用"从当前位置开始的 8 位"直接得到 (符号, 码长)。
    //   fastSym[i] = 在 8 位窗口 i 内被命中的符号; fastLen[i] = 该符号的码长。
    //   取代原来的 "每读 1 位 + 线性扫 121/13 项码表" (最坏 12x121 次比较/symbol)。
    uint8_t fastSym[kHuffFastSizeV4];
    uint8_t fastLen[kHuffFastSizeV4];   // uint8 够用(码长最多 12)
    int fastReady = 0;

    // 建立查表。做法是枚举而不是猜: 对 0..2^8-1 的每个位模式 w, 用"旧实现的判据"
    // 逐长度 l = 1..8 扫一遍码表, 第一个满足 len[s]==l && code[s]==(w 的高 l 位) 的就是
    // 旧实现会返回的符号 —— 因此这里的表按定义与旧实现一致(离线校验器再对它做
    // 全空间穷举复核, 见 cpp/verify_gb7_huff_decode.py)。
    void buildFastV4()
    {
        fastReady = 0;
        for (int w = 0; w < kHuffFastSizeV4; ++w) {
            fastSym[w] = 0;
            fastLen[w] = 0;
            for (int l = 1; l <= kHuffFastBitsV4; ++l) {
                const uint32_t pref = (uint32_t)w >> (kHuffFastBitsV4 - l);
                for (int s = 0; s < count; ++s) {
                    if ((int)len[s] == l && code[s] == pref) {
                        fastSym[w] = (uint8_t)s;
                        fastLen[w] = (uint8_t)l;
                        l = kHuffFastBitsV4 + 1;   // 命中即停(与旧实现"第一个匹配就返回"一致)
                        break;
                    }
                }
            }
            if (fastLen[w] == 0) { return; }        // 码表不完整 -> 不启用查表(回退到底层读位)
        }
        fastReady = 1;
    }

    // 生产路径: 一次 peek(16 位) + 一次表查 + 一次前进(码长 <= 12, 所以 peek 16 位
    // 在任何流位置都够用; 流尾不足 16 位时 peek 补 0, 与旧实现"读到流尾就失败"的判据
    // 由随后的 readBits(l) 负责, 那一步的位置/成功语义与旧实现完全一致)。
    bool readFast(BitReaderV4& br, int* out)
    {
        const uint32_t win = br.peekBitsAligned(kHuffFastBitsV4);
        const int sym = (int)fastSym[win];
        const int l = (int)fastLen[win];
        if (l == 0) { return false; }               // 只可能在 fastReady == 0 时发生
        uint32_t drop = 0;
        if (!br.readBits(l, &drop)) { return false; }   // 该长度确实存在才前进(与旧实现同判据)
        *out = sym;
        return true;
    }

    // 参考实现: 原来那套"逐位读 + 线性扫全表"。运行期的解码路径只用 readFast, 但每个
    // 码流在 parse 之前会用它跑一遍全流校验(见 decodeFrameParseV4 里的 verifyStream)——
    // 于是"新表 == 旧扫描"这件事在真机上也被逐符号验证, 并把结果写进运行日志。
    bool readSymbol(BitReaderV4& br, int* out)
    {
        uint32_t acc = 0;
        for (int l = 1; l <= 31; ++l) {
            uint32_t bit = 0;
            if (!br.readBit(&bit)) { return false; }
            acc = (acc << 1) | bit;
            for (int s = 0; s < count; ++s) {
                if ((int)len[s] == l && code[s] == acc) { *out = s; return true; }
            }
        }
        return false;
    }
};

HuffTableV4 gHuffLevelV4;
HuffTableV4 gHuffMvV4;
HuffDecoderV4 gDecLevelV4;
HuffDecoderV4 gDecMvV4;
bool gHuffsReadyV4 = false;

inline int levelSymbolV4(int run, int cat)
{
    if (run >= 15 || cat <= 0) { return 120; } // EOB
    if (run < 0) { run = 0; }
    if (cat < 1) { cat = 1; }
    if (cat > 8) { cat = 8; }
    return run * 8 + (cat - 1);
}

void initHuffV4()
{
    if (gHuffsReadyV4) { return; }
    // 固定频率(贴近真实分布: 小 run / 小幅度最常见), 不传输码表
    static const int freqLevel[kLevelCountV4] = {
        620, 300, 140, 70, 34, 16, 8, 4,      // run 0
        380, 200, 100, 50, 24, 12, 6, 3,      // run 1
        240, 130, 66, 33, 16, 8, 4, 2,        // run 2
        150, 84, 44, 22, 11, 6, 3, 2,         // run 3
        100, 58, 30, 15, 8, 4, 2, 1,          // run 4
        70, 40, 21, 11, 6, 3, 2, 1,           // run 5
        50, 30, 15, 8, 4, 2, 1, 1,            // run 6
        36, 22, 11, 6, 3, 2, 1, 1,            // run 7
        27, 17, 9, 5, 3, 2, 1, 1,             // run 8
        20, 13, 7, 4, 2, 1, 1, 1,             // run 9
        15, 10, 6, 3, 2, 1, 1, 1,             // run 10
        12, 8, 5, 3, 2, 1, 1, 1,              // run 11
        9, 6, 4, 2, 2, 1, 1, 1,               // run 12
        7, 5, 3, 2, 1, 1, 1, 1,               // run 13
        6, 4, 3, 2, 1, 1, 1, 1,               // run 14
        900                                    // EOB(符号 120)
    };
    static const int freqMv[kMvCountV4] = {1400, 700, 320, 160, 90, 55, 34, 22, 14, 9, 6, 4, 3};
    buildHuffV4(freqLevel, kLevelCountV4, &gHuffLevelV4);
    buildHuffV4(freqMv, kMvCountV4, &gHuffMvV4);
    for (int i = 0; i < kLevelCountV4; ++i) {
        gDecLevelV4.len[i] = gHuffLevelV4.len[i];
        gDecLevelV4.code[i] = gHuffLevelV4.code[i];
    }
    for (int i = 0; i < kMvCountV4; ++i) {
        gDecMvV4.len[i] = gHuffMvV4.len[i];
        gDecMvV4.code[i] = gHuffMvV4.code[i];
    }
    gDecLevelV4.count = kLevelCountV4;
    gDecMvV4.count = kMvCountV4;
    // 建查表(见 HuffDecoderV4::buildFastV4): 256 项 x 2 张表, 一次性开销可忽略。
    gDecLevelV4.buildFastV4();
    gDecMvV4.buildFastV4();
    gHuffsReadyV4 = true;
}

// ---------------------------------------------------------------------------
// 8x8 整数 DCT / IDCT
//   正变换: D[u][v] = ( sum_x b[u][x] * sum_y b[v][y] * s[y][x] ) >> 24
//           (基函数 b 为 Q12 定点; 因此 DC = 8 * 块均值)
//   逆变换: s[y][x] = ( sum_u b[u][x] * sum_v b[v][y] * c[u][v] ) >> 27
//           (两次 >>12 与一次 >>3 合为 >>27, 与正变换严格互逆)
// ---------------------------------------------------------------------------
const int32_t kDctBasis4[8][8] = {
    {1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024},
    {1420, 1203, 803, 284, -284, -803, -1203, -1420},
    {1352, 553, -553, -1352, -1352, -553, 553, 1352},
    {1203, -284, -1420, -803, 803, 1420, 284, -1203},
    {1024, -1024, -1024, 1024, 1024, -1024, -1024, 1024},
    {803, -1420, 284, 1203, -1203, -284, 1420, -803},
    {553, -1352, 1352, -553, -553, 1352, -1352, 553},
    {284, -803, 1203, -1420, 1420, -1203, 803, -284},
};

void fdct8x8V4(const int16_t* src, int stride, int32_t* out)
{
    int64_t tmp[64];
    for (int u = 0; u < 8; ++u) {
        const int32_t* b = kDctBasis4[u];
        for (int y = 0; y < 8; ++y) {
            const int16_t* s = src + (size_t)y * (size_t)stride;
            int64_t acc = 0;
            for (int x = 0; x < 8; ++x) { acc += (int64_t)b[x] * (int64_t)s[x]; }
            tmp[(size_t)u * 8u + (size_t)y] = acc;
        }
    }
    for (int v = 0; v < 8; ++v) {
        const int32_t* b = kDctBasis4[v];
        for (int u = 0; u < 8; ++u) {
            int64_t acc = 0;
            for (int y = 0; y < 8; ++y) { acc += (int64_t)b[y] * tmp[(size_t)u * 8u + (size_t)y]; }
            acc >>= 24;
            if (acc > 40000) { acc = 40000; }
            if (acc < -40000) { acc = -40000; }
            out[(size_t)u * 8u + (size_t)v] = (int32_t)acc;
        }
    }
}

void idct8x8V4(const int32_t* coef, int16_t* dst, int stride)
{
    int64_t tmp[64];
    for (int y = 0; y < 8; ++y) {
        const int32_t* b = kDctBasis4[y];
        for (int v = 0; v < 8; ++v) {
            int64_t acc = 0;
            for (int u = 0; u < 8; ++u) { acc += (int64_t)b[u] * (int64_t)coef[(size_t)u * 8u + (size_t)v]; }
            tmp[(size_t)y * 8u + (size_t)v] = acc;
        }
    }
    for (int x = 0; x < 8; ++x) {
        const int32_t* b = kDctBasis4[x];
        for (int y = 0; y < 8; ++y) {
            int64_t acc = 0;
            for (int v = 0; v < 8; ++v) { acc += (int64_t)b[v] * tmp[(size_t)y * 8u + (size_t)v]; }
            acc >>= 27;
            if (acc > 32767) { acc = 32767; }
            if (acc < -32768) { acc = -32768; }
            dst[(size_t)y * (size_t)stride + (size_t)x] = (int16_t)acc;
        }
    }
}

// 量化步长表: 实际步长 = q/8(与 DCT 的 x8 尺度配套), 等效步长 0.5 ~ 4.0
const int32_t kQuantTable4[32] = {
    4, 6, 8, 10, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72,
    80, 96, 112, 128, 144, 168, 192, 224, 256, 288, 336, 384, 448, 512, 640, 768
};

inline int32_t quantScaleV4(int qp)
{
    if (qp < 0) { qp = 0; }
    if (qp > 31) { qp = 31; }
    return kQuantTable4[qp];
}

static const int kZigzag4[64] = {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// DCT + 量化; 输出量化系数与"解码端完全一致"的重建残差
void analyzeBlockV4(const int16_t* resid, int qp, int16_t* recon, int32_t* coefOut)
{
    int32_t dct[64];
    fdct8x8V4(resid, 8, dct);
    const int32_t qs = quantScaleV4(qp);
    int32_t deq[64];
    for (int i = 0; i < 64; ++i) {
        int32_t c = dct[i];
        int32_t s = c >= 0 ? (c + qs / 2) : -((-c) + qs / 2);
        int32_t qc = s / qs;
        if (qc > 255) { qc = 255; }    // 幅度上限 255(类别 8), 保证可被 VLC 表示
        if (qc < -255) { qc = -255; }
        coefOut[i] = qc;
        deq[i] = qc * qs;
    }
    idct8x8V4(deq, recon, 8);
}

// ---------------------------------------------------------------------------
// 系数块熵编码 / 熵解码(zigzag + 零游程 + 规范 Huffman)
// ---------------------------------------------------------------------------
void writeCoefBlockV4(BitWriterV4& bw, const int32_t* coef)
{
    int32_t zz[64];
    for (int i = 0; i < 64; ++i) { zz[i] = coef[kZigzag4[i]]; }
    int idx = 0;
    int run = 0;
    while (idx < 64) {
        if (zz[idx] == 0) {
            ++idx;
            ++run;
            if (run >= 15) { break; }
            continue;
        }
        int32_t v = zz[idx];
        const int32_t mag = v < 0 ? -v : v; // 反量化前幅度 <= 255
        int cat = 1;
        int32_t t = mag >> 1;
        while (t > 0) { t >>= 1; ++cat; }
        if (cat > 8) { cat = 8; }
        const int sym = levelSymbolV4(run, cat);
        bw.writeBits(gHuffLevelV4.code[sym], gHuffLevelV4.len[sym]);
        // 符号位: 原来这里恒写 1, 解码端 (sign ? -v : v) 会把每个系数都变成负数,
        // 重建残差整体反号 -> PSNR 直接崩掉(编码/解码"配对可逆"就不成立了)。
        bw.writeBits(v < 0 ? 1u : 0u, 1);
        bw.writeBits((uint32_t)mag, cat); // 幅度
        run = 0;
        ++idx;
    }
    const int eob = levelSymbolV4(15, 1);
    bw.writeBits(gHuffLevelV4.code[eob], gHuffLevelV4.len[eob]);
}

bool readCoefBlockV4(BitReaderV4& br, int32_t* coefOut)
{
    for (int i = 0; i < 64; ++i) { coefOut[i] = 0; }
    int pos = 0;
    int guard = 0;
    while (guard++ < 4096) {
        int sym = 0;
        if (!gDecLevelV4.readFast(br, &sym)) { return false; }
        if (sym == 120) { return true; } // EOB
        const int run = sym >> 3;
        const int cat = (sym & 7) + 1;
        pos += run;
        if (pos > 63) { return false; }
        uint32_t sign = 0;
        uint32_t mag = 0;
        if (!br.readBit(&sign)) { return false; }
        if (!br.readBits(cat, &mag)) { return false; }
        int32_t v = (int32_t)mag;
        if (sign) { v = -v; }
        coefOut[kZigzag4[pos]] = v;
        ++pos;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 运动矢量差分(MVD)语法: 分量 -> (类目码, 符号位, 幅度), 编解码两侧逐位对称。
//   跳过宏块(无残差)与帧间宏块(有残差)都传 MVD。原来跳过宏块只写 1 个 '0' 比特,
//   但编码端随后把 prevMv 更新成"运动搜索得到的 MV", 解码端却只能继续用旧的预测值
//   —— 两侧的 MV 预测链从此分叉, 之后每个帧间宏块的 MV 都会解错, 参考帧漂移, PSNR
//   崩掉("配对可逆"不成立)。让跳过宏块也传 MVD 后, 两侧的链严格一致, 且运动搜索
//   的结果(以及基于它做的跳过判决)都被真实使用。
// ---------------------------------------------------------------------------
void writeMvComponentV4(BitWriterV4& bw, int delta)
{
    const int mag = delta < 0 ? -delta : delta;
    int cat = 0;
    {
        int m = mag;
        while (m > 0) { m >>= 1; ++cat; }
        if (cat > 12) { cat = 12; }
    }
    bw.writeBits(gHuffMvV4.code[cat], gHuffMvV4.len[cat]);
    if (cat > 0) {
        bw.writeBits(delta < 0 ? 1u : 0u, 1);
        bw.writeBits((uint32_t)mag, cat);
    }
}

void writeMvDeltaV4(BitWriterV4& bw, int dx, int dy)
{
    writeMvComponentV4(bw, dx);
    writeMvComponentV4(bw, dy);
}

bool readMvComponentV4(BitReaderV4& br, int* out)
{
    int sym = 0;
    if (!gDecMvV4.readFast(br, &sym)) { return false; }
    const int cat = sym;
    int mag = 0;
    if (cat > 0) {
        uint32_t sign = 0;
        uint32_t m = 0;
        if (!br.readBit(&sign)) { return false; }
        if (!br.readBits(cat, &m)) { return false; }
        mag = sign ? -(int)m : (int)m;
    }
    *out = mag;
    return true;
}

bool readMvDeltaV4(BitReaderV4& br, int* dx, int* dy)
{
    return readMvComponentV4(br, dx) && readMvComponentV4(br, dy);
}

// ---------------------------------------------------------------------------
// 测试序列: 渐变背景 + 噪声纹理 + 旋转矩形 + 平移方块(带深色描边)
// ---------------------------------------------------------------------------
void buildNoiseV4(std::vector<uint8_t>& lumaNoise, std::vector<uint8_t>& chromaNoise)
{
    lumaNoise.resize(kVLumaSize4);
    uint32_t s = 0x2545F491u;
    for (size_t i = 0; i < kVLumaSize4; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        lumaNoise[i] = (uint8_t)(s & 0x3Fu); // 0..63 的纹理
    }
    chromaNoise.resize(kVChromaSize4);
    s = 0x9E3779B9u;
    for (size_t i = 0; i < kVChromaSize4; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        chromaNoise[i] = (uint8_t)(s & 0x0Fu);
    }
}

// 方块中心: 三角波往复(周期 64 帧, 幅度 48 / 36 像素) -> 有真实运动但不越界
inline float triWaveV4(int frameIdx, int period, float amp)
{
    int phase = frameIdx % period;
    int tri = (phase < period / 2) ? phase : (period - 1 - phase);
    return amp * (float)tri;
}

void generateFrameV4(int frameIdx, const std::vector<uint8_t>& lumaNoise,
                     const std::vector<uint8_t>& chromaNoise, std::vector<uint8_t>& y,
                     std::vector<uint8_t>& u, std::vector<uint8_t>& v)
{
    y.resize(kVLumaSize4);
    u.resize(kVChromaSize4);
    v.resize(kVChromaSize4);

    const float sqCx = 300.0f + triWaveV4(frameIdx, 64, 48.0f);
    const float sqCy = 260.0f + triWaveV4(frameIdx + 21, 64, 36.0f);

    const float barCx = 880.0f;
    const float barCy = 400.0f;
    const float angle = 0.03f * (float)frameIdx;
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);

    for (int py = 0; py < kVH4; ++py) {
        float fy = (float)py / (float)kVH4;
        uint8_t* row = y.data() + (size_t)py * (size_t)kVW4;
        const uint8_t* nrow = lumaNoise.data() + (size_t)py * (size_t)kVW4;
        for (int px = 0; px < kVW4; ++px) {
            float fx = (float)px / (float)kVW4;
            float base = 96.0f + 64.0f * std::sin(fx * 6.2831853f) * std::cos(fy * 4.7123890f)
                + 40.0f * fx;
            float val = base + (float)nrow[px];
            {
                // 旋转矩形(用逆旋转判定, 保持像素中心一致)
                float dx = (float)px - barCx;
                float dy = (float)py - barCy;
                float lx = ca * dx + sa * dy;
                float ly = -sa * dx + ca * dy;
                if (lx > -150.0f && lx < 150.0f && ly > -40.0f && ly < 40.0f) {
                    if (lx > -144.0f && lx < 144.0f && ly > -34.0f && ly < 34.0f) {
                        val = 200.0f;
                    } else {
                        val = 40.0f;
                    }
                }
            }
            {
                // 平移方块
                float dx = (float)px - sqCx;
                float dy = (float)py - sqCy;
                if (dx > -80.0f && dx < 80.0f && dy > -80.0f && dy < 80.0f) {
                    if (dx > -74.0f && dx < 74.0f && dy > -74.0f && dy < 74.0f) {
                        val = 175.0f;
                    } else {
                        val = 32.0f;
                    }
                }
            }
            if (!(val == val)) { val = 0.0f; }
            if (val < 0.0f) { val = 0.0f; }
            if (val > 255.0f) { val = 255.0f; }
            row[px] = (uint8_t)(val + 0.5f);
        }
    }

    for (int py = 0; py < kVCH4; ++py) {
        float fy = (float)py / (float)kVCH4;
        uint8_t* ur = u.data() + (size_t)py * (size_t)kVCW4;
        uint8_t* vr = v.data() + (size_t)py * (size_t)kVCW4;
        const uint8_t* nrow = chromaNoise.data() + (size_t)py * (size_t)kVCW4;
        for (int px = 0; px < kVCW4; ++px) {
            float fx = (float)px / (float)kVCW4;
            float cu = 128.0f + 18.0f * std::sin(fx * 3.1415927f) * std::cos(fy * 2.0f);
            float cv = 128.0f + 16.0f * std::cos(fx * 2.0f + fy * 1.5f)
                + 0.5f * ((float)nrow[px] - 8.0f);
            if (cu < 0.0f) { cu = 0.0f; }
            if (cu > 255.0f) { cu = 255.0f; }
            if (cv < 0.0f) { cv = 0.0f; }
            if (cv > 255.0f) { cv = 255.0f; }
            ur[px] = (uint8_t)(cu + 0.5f);
            vr[px] = (uint8_t)(cv + 0.5f);
        }
    }
}

// ---------------------------------------------------------------------------
// 运动估计: 小菱形 + 多步菱形细化(step 8/4/2/1), SAD 代价, 半径受限并夹取
// ---------------------------------------------------------------------------
inline void clampVecV4(int* mx, int* my, int cx, int cy)
{
    if (*mx < -cx) { *mx = -cx; }
    if (*mx > kVW4 - 16 - cx) { *mx = kVW4 - 16 - cx; }
    if (*my < -cy) { *my = -cy; }
    if (*my > kVH4 - 16 - cy) { *my = kVH4 - 16 - cy; }
}

int sadBlockV4(const uint8_t* cur, const uint8_t* ref)
{
    int sad = 0;
    for (int yy = 0; yy < 16; ++yy) {
        const uint8_t* a = cur + (size_t)yy * (size_t)kVW4;
        const uint8_t* b = ref + (size_t)yy * (size_t)kVW4;
        for (int xx = 0; xx < 16; ++xx) {
            int d = (int)a[xx] - (int)b[xx];
            sad += d < 0 ? -d : d;
        }
    }
    return sad;
}

void motionSearchV4(const uint8_t* cur, int cx, int cy, const uint8_t* ref, int* bestMx, int* bestMy)
{
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    const uint8_t* curBlk = cur + (size_t)cy * (size_t)kVW4 + (size_t)cx;
    int bx = 0;
    int by = 0;
    int best = sadBlockV4(curBlk, ref + (size_t)cy * (size_t)kVW4 + (size_t)cx);
    for (int i = 0; i < 4; ++i) {
        int mx = kDx[i];
        int my = kDy[i];
        clampVecV4(&mx, &my, cx, cy);
        int s = sadBlockV4(curBlk, ref + (size_t)(cy + my) * (size_t)kVW4 + (size_t)(cx + mx));
        if (s < best) { best = s; bx = mx; by = my; }
    }
    for (int step = 8; step >= 1; step >>= 1) {
        bool moved = true;
        while (moved) {
            moved = false;
            for (int i = 0; i < 4; ++i) {
                int mx = bx + kDx[i] * step;
                int my = by + kDy[i] * step;
                clampVecV4(&mx, &my, cx, cy);
                if (mx == bx && my == by) { continue; }
                int s = sadBlockV4(curBlk, ref + (size_t)(cy + my) * (size_t)kVW4 + (size_t)(cx + mx));
                if (s < best) { best = s; bx = mx; by = my; moved = true; }
            }
        }
    }
    *bestMx = bx;
    *bestMy = by;
}

// ---------------------------------------------------------------------------
// 共用工具
// ---------------------------------------------------------------------------
void fillResidualBlockV4(const uint8_t* cur, const uint8_t* pred, int stride, int16_t* out)
{
    for (int yy = 0; yy < 8; ++yy) {
        const uint8_t* a = cur + (size_t)yy * (size_t)stride;
        const uint8_t* b = pred + (size_t)yy * (size_t)stride;
        int16_t* o = out + (size_t)yy * 8u;
        for (int xx = 0; xx < 8; ++xx) { o[xx] = (int16_t)((int)a[xx] - (int)b[xx]); }
    }
}

void fillFlatBlockV4(uint8_t value, int16_t* out)
{
    for (int i = 0; i < 64; ++i) { out[i] = (int16_t)value; }
}

// 解码端重建: 系数 -> 反量化 -> IDCT -> 加预测 -> 夹取
void reconstructBlockV4(const int32_t* coef, int qp, int base, uint8_t* dst, int stride)
{
    const int32_t qs = quantScaleV4(qp);
    int32_t deq[64];
    for (int i = 0; i < 64; ++i) { deq[i] = coef[i] * qs; }
    int16_t tmp[64];
    idct8x8V4(deq, tmp, 8);
    for (int yy = 0; yy < 8; ++yy) {
        for (int xx = 0; xx < 8; ++xx) {
            int val = (int)tmp[(size_t)yy * 8u + (size_t)xx] + base;
            if (val < 0) { val = 0; }
            if (val > 255) { val = 255; }
            dst[(size_t)yy * (size_t)stride + (size_t)xx] = (uint8_t)val;
        }
    }
}

// 编码端重建: 与解码端同一路径(用 IDCT 结果 + 预测值)
void reconstructFromReconV4(const int16_t* recon, const uint8_t* pred, int stride, uint8_t* dst)
{
    for (int yy = 0; yy < 8; ++yy) {
        for (int xx = 0; xx < 8; ++xx) {
            int val = (int)recon[(size_t)yy * 8u + (size_t)xx]
                + (int)pred[(size_t)yy * (size_t)stride + (size_t)xx];
            if (val < 0) { val = 0; }
            if (val > 255) { val = 255; }
            dst[(size_t)yy * (size_t)stride + (size_t)xx] = (uint8_t)val;
        }
    }
}

void copyBlockV4(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride, int w, int h)
{
    for (int yy = 0; yy < h; ++yy) {
        const uint8_t* s = src + (size_t)yy * (size_t)srcStride;
        uint8_t* d = dst + (size_t)yy * (size_t)dstStride;
        for (int xx = 0; xx < w; ++xx) { d[xx] = s[xx]; }
    }
}

// ---------------------------------------------------------------------------
// 宏块级中间结果: 两阶段之间的数据交换
// (编码端: "分析阶段" -> "写码流阶段"; 解码端: "熵解码阶段" -> "重建阶段")
//   kind: 0 = 帧间编码宏块, 1 = 跳过宏块(纯运动补偿), 2 = 帧内宏块
//   coef: 每宏块 6 个 8x8 块(槽 0..3 = 亮度 by*2+bx, 4 = U, 5 = V). 量化系数幅度
//         <= 255, 用 int16 精确保存(3600*6*64*2B = 2.6MB, 整个负载只分配一次并复用)
// ---------------------------------------------------------------------------
struct MbRecV4 {
    int mvx = 0;
    int mvy = 0;
    uint8_t kind = 0;
    uint8_t chromaCoded = 0;
};

struct MbStoreV4 {
    std::vector<MbRecV4> mb;        // kVMbCount4 条
    std::vector<int16_t> coef;      // kVMbCount4 * 6 * 64
    bool inited = false;

    void init()
    {
        if (inited) { return; }
        mb.assign((size_t)kVMbCount4, MbRecV4());
        coef.assign((size_t)kVMbCount4 * 6u * 64u, (int16_t)0);
        inited = true;
    }
    int16_t* block(int mbIdx, int slot)
    {
        return &coef[((size_t)mbIdx * 6u + (size_t)slot) * 64u];
    }
    const int16_t* block(int mbIdx, int slot) const
    {
        return &coef[((size_t)mbIdx * 6u + (size_t)slot) * 64u];
    }
};

// ---------------------------------------------------------------------------
// 阶段 A: 单个宏块的分析(运动估计 + 变换量化 + 重建).
//   只读 y/u/v 与本帧的参考帧, 只写"本宏块自己的"重建区域(recY/recU/recV 中属于该
//   宏块的像素)与自己的 coef 槽 -> 宏块之间没有任何共享写, 可以安全地按宏块区间并行;
//   又因为编码端 ref 与 rec 是两个不同的缓冲、帧内不存在跨宏块依赖(运动补偿只读参考
//   帧), 所以分析结果与宏块处理顺序无关 -> 并行结果与串行逐位一致.
//   (解码器里"先跑一遍编码产生测试码流"的那一步刻意让 ref == rec 复用同一缓冲,
//    那种情况下顺序会影响重建值, 因此那一步固定 threads = 1 走串行路径.)
// ---------------------------------------------------------------------------
void analyzeMbV4(int mbIdx, bool isIFrame, int qp,
                 const uint8_t* y, const uint8_t* u, const uint8_t* v,
                 const uint8_t* refY, const uint8_t* refU, const uint8_t* refV,
                 uint8_t* recY, uint8_t* recU, uint8_t* recV,
                 MbRecV4& info, int16_t* coefOut)
{
    const int mbx = (mbIdx % kVMbCols4) * 16;
    const int mby = (mbIdx / kVMbCols4) * 16;
    int16_t resid[64];
    int16_t recon[64];
    int32_t coef[64];

    if (isIFrame) {
        info.kind = 2;              // 帧内宏块(与解码端的 intra 语法对应)
        info.chromaCoded = 1;
        info.mvx = 0;
        info.mvy = 0;
        for (int by = 0; by < 2; ++by) {
            for (int bx = 0; bx < 2; ++bx) {
                const uint8_t* cur = y + (size_t)(mby + by * 8) * (size_t)kVW4 + (size_t)(mbx + bx * 8);
                uint8_t* rec = recY + (size_t)(mby + by * 8) * (size_t)kVW4 + (size_t)(mbx + bx * 8);
                fillFlatBlockV4(128, resid);
                for (int yy = 0; yy < 8; ++yy) {
                    for (int xx = 0; xx < 8; ++xx) {
                        resid[(size_t)yy * 8u + (size_t)xx] =
                            (int16_t)((int)cur[(size_t)yy * (size_t)kVW4 + (size_t)xx] - 128);
                    }
                }
                analyzeBlockV4(resid, qp, recon, coef);
                int16_t* dst = coefOut + (size_t)(by * 2 + bx) * 64u;
                for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
                for (int yy = 0; yy < 8; ++yy) {
                    for (int xx = 0; xx < 8; ++xx) {
                        int val = (int)recon[(size_t)yy * 8u + (size_t)xx] + 128;
                        if (val < 0) { val = 0; }
                        if (val > 255) { val = 255; }
                        rec[(size_t)yy * (size_t)kVW4 + (size_t)xx] = (uint8_t)val;
                    }
                }
            }
        }
        for (int plane = 0; plane < 2; ++plane) {
            const uint8_t* src = (plane == 0) ? u : v;
            uint8_t* dstp = (plane == 0) ? recU : recV;
            const uint8_t* cur = src + (size_t)(mby / 2) * (size_t)kVCW4 + (size_t)(mbx / 2);
            uint8_t* rec = dstp + (size_t)(mby / 2) * (size_t)kVCW4 + (size_t)(mbx / 2);
            for (int yy = 0; yy < 8; ++yy) {
                for (int xx = 0; xx < 8; ++xx) {
                    resid[(size_t)yy * 8u + (size_t)xx] =
                        (int16_t)((int)cur[(size_t)yy * (size_t)kVCW4 + (size_t)xx] - 128);
                }
            }
            analyzeBlockV4(resid, qp, recon, coef);
            int16_t* dst = coefOut + (size_t)(4 + plane) * 64u;
            for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
            for (int yy = 0; yy < 8; ++yy) {
                for (int xx = 0; xx < 8; ++xx) {
                    int val = (int)recon[(size_t)yy * 8u + (size_t)xx] + 128;
                    if (val < 0) { val = 0; }
                    if (val > 255) { val = 255; }
                    rec[(size_t)yy * (size_t)kVCW4 + (size_t)xx] = (uint8_t)val;
                }
            }
        }
        return;
    }

    // ---- P 帧: 运动估计(结果先夹取, 再算差分, 保证与解码端一致) ----
    int mvx = 0;
    int mvy = 0;
    motionSearchV4(y, mbx, mby, refY, &mvx, &mvy);
    clampVecV4(&mvx, &mvy, mbx, mby);
    const uint8_t* predY = refY + (size_t)(mby + mvy) * (size_t)kVW4 + (size_t)(mbx + mvx);
    const int sad = sadBlockV4(y + (size_t)mby * (size_t)kVW4 + (size_t)mbx, predY);

    // 编码代价估算: 亮度 4 个块的非零系数个数 + 色度标志 + MV 语法
    int nzCount = 0;
    for (int by = 0; by < 2; ++by) {
        for (int bx = 0; bx < 2; ++bx) {
            const uint8_t* cur = y + (size_t)(mby + by * 8) * (size_t)kVW4 + (size_t)(mbx + bx * 8);
            const uint8_t* pd = predY + (size_t)(by * 8) * (size_t)kVW4 + (size_t)(bx * 8);
            fillResidualBlockV4(cur, pd, kVW4, resid);
            analyzeBlockV4(resid, qp, recon, coef);
            for (int i = 0; i < 64; ++i) { if (coef[i] != 0) { ++nzCount; } }
        }
    }
    const int codedCost = 4 + 2 * 8 + nzCount * 3 + 16; // 类型位 + MV 码 + 系数码 + 色度
    const bool skip = (sad < 1024) || (sad * 3 < codedCost * 8);

    info.mvx = mvx;
    info.mvy = mvy;
    info.kind = skip ? 1 : 0;
    info.chromaCoded = 0;           // 跳过宏块没有色度残差

    if (!skip) {
        for (int by = 0; by < 2; ++by) {
            for (int bx = 0; bx < 2; ++bx) {
                const uint8_t* cur = y + (size_t)(mby + by * 8) * (size_t)kVW4 + (size_t)(mbx + bx * 8);
                const uint8_t* pd = predY + (size_t)(by * 8) * (size_t)kVW4 + (size_t)(bx * 8);
                uint8_t* rec = recY + (size_t)(mby + by * 8) * (size_t)kVW4 + (size_t)(mbx + bx * 8);
                fillResidualBlockV4(cur, pd, kVW4, resid);
                analyzeBlockV4(resid, qp, recon, coef);
                int16_t* dst = coefOut + (size_t)(by * 2 + bx) * 64u;
                for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
                reconstructFromReconV4(recon, pd, kVW4, rec);
            }
        }

        // 色度: MV 折半并夹取, 残差只在"运动补偿不足"时编码
        int cmvx = mvx / 2;
        int cmvy = mvy / 2;
        const int ccx = mbx / 2;
        const int ccy = mby / 2;
        if (cmvx < -ccx) { cmvx = -ccx; }
        if (cmvx > kVCW4 - 8 - ccx) { cmvx = kVCW4 - 8 - ccx; }
        if (cmvy < -ccy) { cmvy = -ccy; }
        if (cmvy > kVCH4 - 8 - ccy) { cmvy = kVCH4 - 8 - ccy; }

        int chromaSad = 0;
        for (int plane = 0; plane < 2; ++plane) {
            const uint8_t* src = (plane == 0) ? u : v;
            const uint8_t* refp = (plane == 0) ? refU : refV;
            const uint8_t* cur = src + (size_t)ccy * (size_t)kVCW4 + (size_t)ccx;
            const uint8_t* pd = refp + (size_t)(ccy + cmvy) * (size_t)kVCW4 + (size_t)(ccx + cmvx);
            for (int yy = 0; yy < 8; ++yy) {
                for (int xx = 0; xx < 8; ++xx) {
                    int d = (int)cur[(size_t)yy * (size_t)kVCW4 + (size_t)xx]
                        - (int)pd[(size_t)yy * (size_t)kVCW4 + (size_t)xx];
                    chromaSad += d < 0 ? -d : d;
                }
            }
        }
        const bool chromaCoded = (chromaSad > 640);
        info.chromaCoded = chromaCoded ? 1 : 0;
        for (int plane = 0; plane < 2; ++plane) {
            const uint8_t* src = (plane == 0) ? u : v;
            const uint8_t* refp = (plane == 0) ? refU : refV;
            uint8_t* dstp = (plane == 0) ? recU : recV;
            const uint8_t* cur = src + (size_t)ccy * (size_t)kVCW4 + (size_t)ccx;
            const uint8_t* pd = refp + (size_t)(ccy + cmvy) * (size_t)kVCW4 + (size_t)(ccx + cmvx);
            uint8_t* rec = dstp + (size_t)ccy * (size_t)kVCW4 + (size_t)ccx;
            if (chromaCoded) {
                fillResidualBlockV4(cur, pd, kVCW4, resid);
                analyzeBlockV4(resid, qp, recon, coef);
                int16_t* dst = coefOut + (size_t)(4 + plane) * 64u;
                for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
                reconstructFromReconV4(recon, pd, kVCW4, rec);
            } else {
                copyBlockV4(pd, kVCW4, rec, kVCW4, 8, 8);
            }
        }
    } else {
        // 跳过宏块: 纯运动补偿(与解码端用同一 MV 预测链)
        copyBlockV4(predY, kVW4, recY + (size_t)mby * (size_t)kVW4 + (size_t)mbx, kVW4, 16, 16);
        int cmvx = mvx / 2;
        int cmvy = mvy / 2;
        const int ccx = mbx / 2;
        const int ccy = mby / 2;
        if (cmvx < -ccx) { cmvx = -ccx; }
        if (cmvx > kVCW4 - 8 - ccx) { cmvx = kVCW4 - 8 - ccx; }
        if (cmvy < -ccy) { cmvy = -ccy; }
        if (cmvy > kVCH4 - 8 - ccy) { cmvy = kVCH4 - 8 - ccy; }
        for (int plane = 0; plane < 2; ++plane) {
            const uint8_t* refp = (plane == 0) ? refU : refV;
            uint8_t* dstp = (plane == 0) ? recU : recV;
            const uint8_t* pd = refp + (size_t)(ccy + cmvy) * (size_t)kVCW4 + (size_t)(ccx + cmvx);
            uint8_t* rec = dstp + (size_t)ccy * (size_t)kVCW4 + (size_t)ccx;
            copyBlockV4(pd, kVCW4, rec, kVCW4, 8, 8);
        }
    }
}

// ---------------------------------------------------------------------------
// 阶段 B: 按宏块顺序把语法写进比特流(串行).
//   熵编码顺序与 MV 差分预测链(prevMv)是顺序相关的, 所以这一段不能并行; 它只是把
//   阶段 A 已经算好的结果序列化, 不做任何搜索/变换. 比特写入的顺序与并发之前逐位
//   一致(类型位 -> MV 语法 -> 4 个亮度系数块 -> 色度标志 -> 色度系数块).
// ---------------------------------------------------------------------------
void writeMbV4(BitWriterV4& bw, bool isIFrame, const MbRecV4& info,
               const MbStoreV4& store, int mbIdx, int& prevMvx, int& prevMvy)
{
    int32_t coef[64];
    if (isIFrame) {
        for (int slot = 0; slot < 6; ++slot) {
            const int16_t* src = store.block(mbIdx, slot);
            for (int i = 0; i < 64; ++i) { coef[i] = (int32_t)src[i]; }
            writeCoefBlockV4(bw, coef);
        }
        return;
    }
    if (info.kind == 0) {
        bw.writeBits(1u, 1); // coded
        bw.writeBits(0u, 1); // inter
        writeMvDeltaV4(bw, info.mvx - prevMvx, info.mvy - prevMvy);
        prevMvx = info.mvx;
        prevMvy = info.mvy;

        for (int by = 0; by < 2; ++by) {
            for (int bx = 0; bx < 2; ++bx) {
                const int16_t* src = store.block(mbIdx, by * 2 + bx);
                for (int i = 0; i < 64; ++i) { coef[i] = (int32_t)src[i]; }
                writeCoefBlockV4(bw, coef);
            }
        }
        bw.writeBits(info.chromaCoded ? 1u : 0u, 1);
        if (info.chromaCoded) {
            for (int plane = 0; plane < 2; ++plane) {
                const int16_t* src = store.block(mbIdx, 4 + plane);
                for (int i = 0; i < 64; ++i) { coef[i] = (int32_t)src[i]; }
                writeCoefBlockV4(bw, coef);
            }
        }
    } else {
        // 跳过宏块: 无残差, 但仍然传 MV 差分 —— 解码端据此恢复出与编码端完全相同的
        // MV(见 writeMvDeltaV4 的说明), 之后两侧的预测链一致。
        bw.writeBits(0u, 1);
        writeMvDeltaV4(bw, info.mvx - prevMvx, info.mvy - prevMvy);
        prevMvx = info.mvx;
        prevMvy = info.mvy;
    }
}

// ---------------------------------------------------------------------------
// 编码一帧: 阶段 A(宏块级并行) + 阶段 B(串行写语法)
//   threads <= 1 时阶段 A 走原样的宏块顺序循环, 结果与并发之前完全一致.
// ---------------------------------------------------------------------------
void encodeFrameV4(BitWriterV4& bw, int frameIdx, int qp, bool isIFrame,
                   const std::vector<uint8_t>& y, const std::vector<uint8_t>& u,
                   const std::vector<uint8_t>& v, const std::vector<uint8_t>& refY,
                   const std::vector<uint8_t>& refU, const std::vector<uint8_t>& refV,
                   std::vector<uint8_t>& recY, std::vector<uint8_t>& recU, std::vector<uint8_t>& recV,
                   int threads, MbStoreV4& store)
{
    bw.alignToByte();
    bw.writeBits(kStreamMagic4, 32);
    bw.writeBits((uint32_t)frameIdx, 16);
    bw.writeBits(isIFrame ? 1u : 0u, 1);
    bw.writeBits((uint32_t)qp, 5);

    store.init();
    const uint8_t* yp = y.data();
    const uint8_t* up = u.data();
    const uint8_t* vp = v.data();
    const uint8_t* ryp = refY.data();
    const uint8_t* rup = refU.data();
    const uint8_t* rvp = refV.data();
    uint8_t* cyp = recY.data();
    uint8_t* cup = recU.data();
    uint8_t* cvp = recV.data();

    // ---- 阶段 A: 宏块级并行的重活(运动估计 / 变换量化 / 重建) ----
    if (threads <= 1) {
        for (int mbIdx = 0; mbIdx < kVMbCount4; ++mbIdx) {
            analyzeMbV4(mbIdx, isIFrame, qp, yp, up, vp, ryp, rup, rvp, cyp, cup, cvp,
                        store.mb[(size_t)mbIdx], store.block(mbIdx, 0));
        }
    } else {
        gb7ParallelFor(threads, kVMbCount4, [&](long long start, long long end) {
            for (long long mbIdx = start; mbIdx < end; ++mbIdx) {
                analyzeMbV4((int)mbIdx, isIFrame, qp, yp, up, vp, ryp, rup, rvp, cyp, cup, cvp,
                            store.mb[(size_t)mbIdx], store.block((int)mbIdx, 0));
            }
        });
    }

    // ---- 阶段 B: 串行写语法(码流顺序与 MV 预测链保持原样) ----
    int prevMvx = 0;
    int prevMvy = 0;
    for (int mbIdx = 0; mbIdx < kVMbCount4; ++mbIdx) {
        writeMbV4(bw, isIFrame, store.mb[(size_t)mbIdx], store, mbIdx, prevMvx, prevMvy);
    }

    bw.alignToByte();
}

// ---------------------------------------------------------------------------
// 解码一帧(与编码端逐像素一致)
//
//   阶段 A(串行, 熵解码): 比特流是顺序语法(宏块类型位 / MV 差分预测链 / 系数),
//       只能由单线程推进; 解析出来的每个宏块的 MV 与 6 个系数块全部存进 MbStoreV4.
//   阶段 B(宏块级并行, 重建): 反量化 + IDCT + 运动补偿 + 夹取.
//       每个宏块只写自己的像素区域(recY/recU/recV 中属于它的 16x16 / 8x8),
//       预测只读"上一帧"的参考缓冲(refY/refU/refV), 本帧的重建不参与本帧预测,
//       所以宏块之间既没有依赖也没有共享写 -> 可安全并行, 结果与串行逐位一致.
// ---------------------------------------------------------------------------
// 全流等价性自检: 用参考实现(逐位读 + 线性扫全表)在同一份码流上再走一遍语法,
// 逐符号比较 (符号值, 消耗比特数, 最终比特位置)。它只读码流、不分配、不修改任何状态,
// 因此不影响解码结果; 结果写进 stderr 的运行日志, 让"查表 == 旧扫描"在真机上也可核。
// 开销: 参考实现只跑一遍, 而每个 pass 里 4~5 个帧会被解码一次 —— 即约 1/passes 的额外
// 解析开销(passes = 4 时约 25%), 为"真机自证等价"付的代价, 已计入规模标定的余量。
struct HuffSelfCheckV4 {
    int frames = 0;
    long long symbols = 0;
    long long mismatches = 0;
    long long firstBadFrame = -1;
};

// 解一帧(并把主读取器推进到本帧末尾), 全程用"新查表"与"旧逐位扫描"两份读取器对照。
// 返回 false = 本帧两侧都读到流尾(与 decodeFrameParseV4 的失败语义一致)。
// 自检辅助: 任何 [两侧不一致] 的退出点都必须 (1) 记下不一致, (2) 把主读取器推进到与新
// 读取器相同的位置 —— 校验器只读码流, 不允许影响真正的解码路径。
#define SYNC_MAIN() main.setBitPosition(br.bitPosition())

bool verifyFrameParseV4(BitReaderV4& main, int expectFrame, int qp, bool isIFrame, HuffSelfCheckV4& sc)
{
    BitReaderV4 br(main.streamData(), main.streamLen());
    br.setBitPosition(main.bitPosition());
    BitReaderV4 ref(br.streamData(), br.streamLen());
    ref.setBitPosition(br.bitPosition());
    ++sc.frames;
    uint32_t magic = 0, magicR = 0, fIdx = 0, fIdxR = 0, iflag = 0, iflagR = 0, qpRead = 0, qpReadR = 0;
    const bool hA = br.readBits(32, &magic) && br.readBits(16, &fIdx) && br.readBits(1, &iflag) && br.readBits(5, &qpRead);
    const bool hB = ref.readBits(32, &magicR) && ref.readBits(16, &fIdxR) && ref.readBits(1, &iflagR) && ref.readBits(5, &qpReadR);
    if (hA != hB || magic != magicR || fIdx != fIdxR || iflag != iflagR || qpRead != qpReadR) {
        ++sc.mismatches; SYNC_MAIN(); return false;
    }
    if (!hA) { SYNC_MAIN(); return false; }   // 两侧都到流尾: 本帧失败

    int prevMvx = 0, prevMvy = 0;
    int32_t coefA[64], coefB[64];
    for (int mbIdx = 0; mbIdx < kVMbCount4; ++mbIdx) {
        const int mbx = (mbIdx % kVMbCols4) * 16;
        const int mby = (mbIdx / kVMbCols4) * 16;
        bool okA = true, okB = true;
        if (isIFrame) {
            for (int k = 0; k < 6 && okA && okB; ++k) {
                okA = readCoefBlockV4(br, coefA);
                okB = readCoefBlockV4(ref, coefB);
                if (okA != okB) { ++sc.mismatches; SYNC_MAIN(); return false; }
                for (int i = 0; i < 64; ++i) { if (coefA[i] != coefB[i]) { ++sc.mismatches; SYNC_MAIN(); return false; } }
                sc.symbols += 1;
            }
        } else {
            uint32_t modeA = 0, modeB = 0;
            if (!br.readBit(&modeA) || !ref.readBit(&modeB)) { ++sc.mismatches; SYNC_MAIN(); return false; }
            if (modeA != modeB) { ++sc.mismatches; SYNC_MAIN(); return false; }
            if (modeA == 0) {
                int dxA = 0, dyA = 0, dxB = 0, dyB = 0;
                if (!readMvDeltaV4(br, &dxA, &dyA) || !readMvDeltaV4(ref, &dxB, &dyB)) { ++sc.mismatches; SYNC_MAIN(); return false; }
                if (dxA != dxB || dyA != dyB) { ++sc.mismatches; SYNC_MAIN(); return false; }
                int mvx = prevMvx + dxA;
                int mvy = prevMvy + dyA;
                clampVecV4(&mvx, &mvy, mbx, mby);
                prevMvx = mvx; prevMvy = mvy;
                sc.symbols += 2;
                continue;
            }
            uint32_t intraA = 0, intraB = 0;
            if (!br.readBit(&intraA) || !ref.readBit(&intraB)) { ++sc.mismatches; SYNC_MAIN(); return false; }
            if (intraA != intraB) { ++sc.mismatches; SYNC_MAIN(); return false; }
            if (intraA) {
                for (int k = 0; k < 4 && okA && okB; ++k) {
                    okA = readCoefBlockV4(br, coefA);
                    okB = readCoefBlockV4(ref, coefB);
                    if (okA != okB) { ++sc.mismatches; SYNC_MAIN(); return false; }
                    for (int i = 0; i < 64; ++i) { if (coefA[i] != coefB[i]) { ++sc.mismatches; SYNC_MAIN(); return false; } }
                    sc.symbols += 1;
                }
                uint32_t cA = 0, cB = 0;
                if (!br.readBit(&cA) || !ref.readBit(&cB)) { ++sc.mismatches; SYNC_MAIN(); return false; }
                if (cA != cB) { ++sc.mismatches; SYNC_MAIN(); return false; }
                if (cA) {
                    for (int k = 0; k < 2; ++k) {
                        okA = readCoefBlockV4(br, coefA);
                        okB = readCoefBlockV4(ref, coefB);
                        if (okA != okB) { ++sc.mismatches; SYNC_MAIN(); return false; }
                        sc.symbols += 1;
                    }
                }
                continue;
            }
            int dxA = 0, dyA = 0, dxB = 0, dyB = 0;
            if (!readMvDeltaV4(br, &dxA, &dyA) || !readMvDeltaV4(ref, &dxB, &dyB)) { ++sc.mismatches; SYNC_MAIN(); return false; }
            if (dxA != dxB || dyA != dyB) { ++sc.mismatches; SYNC_MAIN(); return false; }
            int mvx = prevMvx + dxA;
            int mvy = prevMvy + dyA;
            clampVecV4(&mvx, &mvy, mbx, mby);
            prevMvx = mvx; prevMvy = mvy;
            sc.symbols += 2;
            for (int k = 0; k < 4; ++k) {
                okA = readCoefBlockV4(br, coefA);
                okB = readCoefBlockV4(ref, coefB);
                if (okA != okB) { ++sc.mismatches; SYNC_MAIN(); return false; }
                for (int i = 0; i < 64; ++i) { if (coefA[i] != coefB[i]) { ++sc.mismatches; SYNC_MAIN(); return false; } }
                sc.symbols += 1;
            }
            uint32_t cA2 = 0, cB2 = 0;
            if (!br.readBit(&cA2) || !ref.readBit(&cB2)) { ++sc.mismatches; SYNC_MAIN(); return false; }
            if (cA2 != cB2) { ++sc.mismatches; SYNC_MAIN(); return false; }
            if (cA2) {
                for (int k = 0; k < 2; ++k) {
                    okA = readCoefBlockV4(br, coefA);
                    okB = readCoefBlockV4(ref, coefB);
                    if (okA != okB) { ++sc.mismatches; SYNC_MAIN(); return false; }
                    sc.symbols += 1;
                }
            }
        }
        if (!okA && !okB) { SYNC_MAIN(); return false; }   // 两侧都读到流尾: 一致(该帧本来就会被判失败)
        if (okA != okB) { ++sc.mismatches; SYNC_MAIN(); return false; }
    }
    br.alignToByte();
    ref.alignToByte();
    if (br.bitPosition() != ref.bitPosition()) { ++sc.mismatches; }
    (void)expectFrame; (void)qp;
    SYNC_MAIN();
    return (sc.mismatches == 0);
}
#undef SYNC_MAIN


bool decodeFrameParseV4(BitReaderV4& br, int expectFrame, int qp, bool isIFrame, MbStoreV4& store)
{
    uint32_t magic = 0;
    uint32_t fIdx = 0;
    uint32_t iflag = 0;
    uint32_t qpRead = 0;
    if (!br.readBits(32, &magic) || magic != kStreamMagic4) { return false; }
    if (!br.readBits(16, &fIdx)) { return false; }
    if (!br.readBits(1, &iflag)) { return false; }
    if (!br.readBits(5, &qpRead)) { return false; }
    if ((int)fIdx != expectFrame || ((iflag != 0) != isIFrame) || (int)qpRead != qp) { return false; }

    int prevMvx = 0;
    int prevMvy = 0;
    int32_t coef[64];

    for (int mbIdx = 0; mbIdx < kVMbCount4; ++mbIdx) {
        const int mbx = (mbIdx % kVMbCols4) * 16;
        const int mby = (mbIdx / kVMbCols4) * 16;
        MbRecV4& info = store.mb[(size_t)mbIdx];
        info.mvx = 0;
        info.mvy = 0;
        info.kind = 2;
        info.chromaCoded = 1;

        if (isIFrame) {
            for (int by = 0; by < 2; ++by) {
                for (int bx = 0; bx < 2; ++bx) {
                    if (!readCoefBlockV4(br, coef)) { return false; }
                    int16_t* dst = store.block(mbIdx, by * 2 + bx);
                    for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
                }
            }
            for (int plane = 0; plane < 2; ++plane) {
                if (!readCoefBlockV4(br, coef)) { return false; }
                int16_t* dst = store.block(mbIdx, 4 + plane);
                for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
            }
            continue;
        }

        uint32_t mode = 0;
        if (!br.readBit(&mode)) { return false; }
        if (mode == 0) {
            // 跳过宏块: 无残差, 但 MV 差分照读, 预测链与编码端保持同一状态
            int dx = 0;
            int dy = 0;
            if (!readMvDeltaV4(br, &dx, &dy)) { return false; }
            int mvx = prevMvx + dx;
            int mvy = prevMvy + dy;
            clampVecV4(&mvx, &mvy, mbx, mby);
            prevMvx = mvx;
            prevMvy = mvy;
            info.kind = 1;              // 跳过宏块: 纯运动补偿
            info.mvx = mvx;
            info.mvy = mvy;
            continue;
        }

        uint32_t intra = 0;
        if (!br.readBit(&intra)) { return false; }
        if (intra) {
            // 语法保留; 当前编码器不在 P 帧内产生帧内宏块
            info.kind = 2;
            for (int by = 0; by < 2; ++by) {
                for (int bx = 0; bx < 2; ++bx) {
                    if (!readCoefBlockV4(br, coef)) { return false; }
                    int16_t* dst = store.block(mbIdx, by * 2 + bx);
                    for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
                }
            }
            uint32_t chromaFlag = 0;
            if (!br.readBit(&chromaFlag)) { return false; }
            info.chromaCoded = (uint8_t)chromaFlag;
            for (int plane = 0; plane < 2; ++plane) {
                if (chromaFlag) {
                    if (!readCoefBlockV4(br, coef)) { return false; }
                    int16_t* dst = store.block(mbIdx, 4 + plane);
                    for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
                }
            }
            continue;
        }

        // inter: MV 差分(与跳过宏块同一段语法)
        info.kind = 0;
        int dx2 = 0;
        int dy2 = 0;
        if (!readMvDeltaV4(br, &dx2, &dy2)) { return false; }
        int mvx = prevMvx + dx2;
        int mvy = prevMvy + dy2;
        clampVecV4(&mvx, &mvy, mbx, mby);
        prevMvx = mvx;
        prevMvy = mvy;
        info.mvx = mvx;
        info.mvy = mvy;

        for (int by = 0; by < 2; ++by) {
            for (int bx = 0; bx < 2; ++bx) {
                if (!readCoefBlockV4(br, coef)) { return false; }
                int16_t* dst = store.block(mbIdx, by * 2 + bx);
                for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
            }
        }
        uint32_t chromaFlag = 0;
        if (!br.readBit(&chromaFlag)) { return false; }
        info.chromaCoded = (uint8_t)chromaFlag;
        for (int plane = 0; plane < 2; ++plane) {
            if (chromaFlag) {
                if (!readCoefBlockV4(br, coef)) { return false; }
                int16_t* dst = store.block(mbIdx, 4 + plane);
                for (int i = 0; i < 64; ++i) { dst[i] = (int16_t)coef[i]; }
            }
        }
    }
    br.alignToByte();
    return true;
}

// 阶段 B: 单个宏块的重建(只读 store 与参考帧, 只写本宏块的像素)
void reconstructMbV4(int mbIdx, int qp, const MbStoreV4& store,
                     const uint8_t* refY, const uint8_t* refU, const uint8_t* refV,
                     uint8_t* recY, uint8_t* recU, uint8_t* recV)
{
    const int mbx = (mbIdx % kVMbCols4) * 16;
    const int mby = (mbIdx / kVMbCols4) * 16;
    const MbRecV4& info = store.mb[(size_t)mbIdx];
    const int32_t qs = quantScaleV4(qp);
    int32_t coef[64];
    int32_t deq[64];
    int16_t tmp[64];

    if (info.kind == 2) {
        // 帧内宏块: 预测值 = 128
        for (int by = 0; by < 2; ++by) {
            for (int bx = 0; bx < 2; ++bx) {
                uint8_t* rec = recY + (size_t)(mby + by * 8) * (size_t)kVW4 + (size_t)(mbx + bx * 8);
                const int16_t* src = store.block(mbIdx, by * 2 + bx);
                for (int i = 0; i < 64; ++i) { coef[i] = (int32_t)src[i]; }
                reconstructBlockV4(coef, qp, 128, rec, kVW4);
            }
        }
        for (int plane = 0; plane < 2; ++plane) {
            uint8_t* dstp = (plane == 0) ? recU : recV;
            uint8_t* rec = dstp + (size_t)(mby / 2) * (size_t)kVCW4 + (size_t)(mbx / 2);
            if (info.chromaCoded) {
                const int16_t* src = store.block(mbIdx, 4 + plane);
                for (int i = 0; i < 64; ++i) { coef[i] = (int32_t)src[i]; }
                reconstructBlockV4(coef, qp, 128, rec, kVCW4);
            } else {
                for (int yy = 0; yy < 8; ++yy) {
                    for (int xx = 0; xx < 8; ++xx) { rec[(size_t)yy * (size_t)kVCW4 + (size_t)xx] = 128u; }
                }
            }
        }
        return;
    }

    const int mvx = info.mvx;
    const int mvy = info.mvy;
    const uint8_t* predY = refY + (size_t)(mby + mvy) * (size_t)kVW4 + (size_t)(mbx + mvx);
    int cmvx = mvx / 2;
    int cmvy = mvy / 2;
    const int ccx = mbx / 2;
    const int ccy = mby / 2;
    if (cmvx < -ccx) { cmvx = -ccx; }
    if (cmvx > kVCW4 - 8 - ccx) { cmvx = kVCW4 - 8 - ccx; }
    if (cmvy < -ccy) { cmvy = -ccy; }
    if (cmvy > kVCH4 - 8 - ccy) { cmvy = kVCH4 - 8 - ccy; }

    if (info.kind == 1) {
        // 跳过宏块: 纯运动补偿
        copyBlockV4(predY, kVW4, recY + (size_t)mby * (size_t)kVW4 + (size_t)mbx, kVW4, 16, 16);
        for (int plane = 0; plane < 2; ++plane) {
            const uint8_t* refp = (plane == 0) ? refU : refV;
            uint8_t* dstp = (plane == 0) ? recU : recV;
            const uint8_t* pd = refp + (size_t)(ccy + cmvy) * (size_t)kVCW4 + (size_t)(ccx + cmvx);
            uint8_t* rec = dstp + (size_t)ccy * (size_t)kVCW4 + (size_t)ccx;
            copyBlockV4(pd, kVCW4, rec, kVCW4, 8, 8);
        }
        return;
    }

    // 帧间编码宏块: 残差 + 运动补偿
    for (int by = 0; by < 2; ++by) {
        for (int bx = 0; bx < 2; ++bx) {
            uint8_t* rec = recY + (size_t)(mby + by * 8) * (size_t)kVW4 + (size_t)(mbx + bx * 8);
            const uint8_t* pd = predY + (size_t)(by * 8) * (size_t)kVW4 + (size_t)(bx * 8);
            const int16_t* src = store.block(mbIdx, by * 2 + bx);
            for (int i = 0; i < 64; ++i) { deq[i] = (int32_t)src[i] * qs; }
            idct8x8V4(deq, tmp, 8);
            reconstructFromReconV4(tmp, pd, kVW4, rec);
        }
    }
    for (int plane = 0; plane < 2; ++plane) {
        const uint8_t* refp = (plane == 0) ? refU : refV;
        uint8_t* dstp = (plane == 0) ? recU : recV;
        const uint8_t* pd = refp + (size_t)(ccy + cmvy) * (size_t)kVCW4 + (size_t)(ccx + cmvx);
        uint8_t* rec = dstp + (size_t)ccy * (size_t)kVCW4 + (size_t)ccx;
        if (info.chromaCoded) {
            const int16_t* src = store.block(mbIdx, 4 + plane);
            for (int i = 0; i < 64; ++i) { deq[i] = (int32_t)src[i] * qs; }
            idct8x8V4(deq, tmp, 8);
            reconstructFromReconV4(tmp, pd, kVCW4, rec);
        } else {
            copyBlockV4(pd, kVCW4, rec, kVCW4, 8, 8);
        }
    }
}

// 阶段 B 的宏块区间调度(threads <= 1 时按原样顺序循环)
void reconstructFrameV4(int threads, int qp, const MbStoreV4& store,
                        const std::vector<uint8_t>& refY, const std::vector<uint8_t>& refU,
                        const std::vector<uint8_t>& refV, std::vector<uint8_t>& recY,
                        std::vector<uint8_t>& recU, std::vector<uint8_t>& recV)
{
    const uint8_t* ryp = refY.data();
    const uint8_t* rup = refU.data();
    const uint8_t* rvp = refV.data();
    uint8_t* cyp = recY.data();
    uint8_t* cup = recU.data();
    uint8_t* cvp = recV.data();
    if (threads <= 1) {
        for (int mbIdx = 0; mbIdx < kVMbCount4; ++mbIdx) {
            reconstructMbV4(mbIdx, qp, store, ryp, rup, rvp, cyp, cup, cvp);
        }
    } else {
        gb7ParallelFor(threads, kVMbCount4, [&](long long start, long long end) {
            for (long long mbIdx = start; mbIdx < end; ++mbIdx) {
                reconstructMbV4((int)mbIdx, qp, store, ryp, rup, rvp, cyp, cup, cvp);
            }
        });
    }
}

bool decodeFrameV4(BitReaderV4& br, int expectFrame, int qp, bool isIFrame,
                   std::vector<uint8_t>& recY, std::vector<uint8_t>& recU, std::vector<uint8_t>& recV,
                   const std::vector<uint8_t>& refY, const std::vector<uint8_t>& refU,
                   const std::vector<uint8_t>& refV, int threads, MbStoreV4& store)
{
    store.init();
    if (!decodeFrameParseV4(br, expectFrame, qp, isIFrame, store)) {
        return false;
    }
    reconstructFrameV4(threads, qp, store, refY, refU, refV, recY, recU, recV);
    return true;
}

double psnrLumaV4(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double* maeOut)
{
    uint64_t sse = 0;
    uint64_t sae = 0;
    for (size_t i = 0; i < kVLumaSize4; ++i) {
        int d = (int)a[i] - (int)b[i];
        sse += (uint64_t)(d * d);
        sae += (uint64_t)(d < 0 ? -d : d);
    }
    double mse = (double)sse / (double)kVLumaSize4;
    *maeOut = (double)sae / (double)kVLumaSize4;
    if (mse <= 1e-12) { return 99.0; }
    double v = 10.0 * std::log10((255.0 * 255.0) / mse);
    if (!(v == v)) { return 0.0; }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// Video Encoder
// ---------------------------------------------------------------------------
Gb7Outcome gb7RunVideoEncoder(int threads)
{
    Gb7Outcome o;
    o.name = "Video Encoder";
    o.section = "Media";

    initHuffV4();

    const int qp = 18; // 固定量化参数(等效量化步长 = q/8 = 2.0)

    std::vector<uint8_t> lumaNoise;
    std::vector<uint8_t> chromaNoise;
    buildNoiseV4(lumaNoise, chromaNoise);

    // 码流缓冲: 实际输出约 1~3MB, 4MB 留足余量
    BitWriterV4 bw(4u * 1024u * 1024u);

    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    std::vector<uint8_t> refY(kVLumaSize4, 128);
    std::vector<uint8_t> refU(kVChromaSize4, 128);
    std::vector<uint8_t> refV(kVChromaSize4, 128);
    std::vector<uint8_t> recY(kVLumaSize4, 0);
    std::vector<uint8_t> recU(kVChromaSize4, 0);
    std::vector<uint8_t> recV(kVChromaSize4, 0);

    if (threads < 1) { threads = 1; }
    // 宏块级中间结果(每帧复用同一份, 只分配一次; 见 MbStoreV4 注释)
    // 在计时区间之前分配好, 串行/并行两条路径都不把分配成本算进 o.ms
    MbStoreV4 store;
    store.init();

    double psnrSum = 0.0;
    double maeSum = 0.0;
    int framesDone = 0;

    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsV4();
    std::clock_t cpu0 = std::clock();
    for (int f = 0; f < kVEncFrameCount4; ++f) {
        const bool isI = (f % kVIInterval4) == 0;
        generateFrameV4(f, lumaNoise, chromaNoise, y, u, v);
        encodeFrameV4(bw, f, qp, isI, y, u, v, refY, refU, refV, recY, recU, recV, threads, store);
        if (bw.overflowed()) { break; }
        double mae = 0.0;
        psnrSum += psnrLumaV4(y, recY, &mae);
        maeSum += mae;
        ++framesDone;
        refY = recY;
        refU = recU;
        refV = recV;
    }
    double t1 = nowMsV4();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)

    // 码流 CRC 累加, 防止编码结果被优化掉
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bw.bytesUsed(); ++i) {
        h ^= (uint32_t)bw.data()[i];
        h *= 16777619u;
    }
    volatile uint32_t sinkH = h;
    (void)sinkH;

    // 真实并行度: 参与线程的 CPU 时间之和 / 墙钟时间
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    o.parallelism = gb7Parallelism(cpuMs, t1 - t0);

    double secondsElapsed = (t1 - t0) / 1000.0;
    if (!(secondsElapsed > 1e-6)) { secondsElapsed = 1e-6; }
    double mpx = (double)kVPixelsPerFrame4 * (double)framesDone / 1000000.0;
    double mpxPerSec = mpx / secondsElapsed;
    if (!(mpxPerSec == mpxPerSec) || mpxPerSec < 0.0) { mpxPerSec = 0.0; }
    if (mpxPerSec > 1e7) { mpxPerSec = 1e7; }

    char logbuf[288];
    double denom = (framesDone > 0) ? (double)framesDone : 1.0;
    snprintf(logbuf, sizeof(logbuf),
             "[gb7] Video Encoder: %d/%d frames (I every %d), stream=%u B (%.3f bpp), avgPSNR(Y)=%.2f dB, MAE=%.3f, %.1f Mpx/s, overflow=%d, crc=0x%08X",
             framesDone, kVEncFrameCount4, kVIInterval4, (unsigned)bw.bytesUsed(),
             8.0 * (double)bw.bytesUsed() / ((double)kVPixelsPerFrame4 * denom),
             psnrSum / denom, maeSum / denom, mpxPerSec, bw.overflowed() ? 1 : 0, (unsigned)h);
    fputs(logbuf, stderr);
    fputc('\n', stderr);

    char buf32[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf32, sizeof(buf32), "%.4g", mpxPerSec);
    o.ms = t1 - t0;
    o.metric = buf32;
    if (framesDone == kVEncFrameCount4) {
        o.unit = "Mpx/s";
    } else {
        // 码流缓冲溢出等异常情况下不要把"少编了几帧"藏起来
        char why[160];
        snprintf(why, sizeof(why), "Mpx/s (partial: %d/%d frames encoded, stream overflow=%d)",
                 framesDone, kVEncFrameCount4, bw.overflowed() ? 1 : 0);
        o.unit = why;
    }
    return o;
}

// ---------------------------------------------------------------------------
// Video Decoder
// ---------------------------------------------------------------------------
Gb7Outcome gb7RunVideoDecoder(int threads)
{
    Gb7Outcome o;
    o.name = "Video Decoder";
    o.section = "Media";

    initHuffV4();

    const int qp = 18;
    std::vector<uint8_t> lumaNoise;
    std::vector<uint8_t> chromaNoise;
    buildNoiseV4(lumaNoise, chromaNoise);

    if (threads < 1) { threads = 1; }

    BitWriterV4 bw(4u * 1024u * 1024u);
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    std::vector<uint8_t> refY(kVLumaSize4, 128);
    std::vector<uint8_t> refU(kVChromaSize4, 128);
    std::vector<uint8_t> refV(kVChromaSize4, 128);

    // 宏块级中间结果: 编解码两个阶段共用同一份(先给产生码流的预跑用, 再给解码用)
    MbStoreV4 store;
    store.init();

    // ---- 先完整跑一遍编码产生真实码流(该耗时不计入解码计时) ----
    // 注意: 这一步 ref 与 rec 复用同一组缓冲(ref == rec), 重建会覆盖马上要读的参考
    // 像素, 结果与宏块处理顺序有关; 因此这里固定 threads = 1, 保证产生的码流与并发
    // 之前逐位一致(该步骤本来就不计入计时, 串行不影响多核成绩).
    // 2026-10-06 追加: 给这段预跑也计个时 它不计入 o.ms, 但它是一段确定的编码工作量,
    //   紧挨着计时区间发生。它的耗时是"这一项开跑时这台机器有多快"的现场参照物, 也是
    //   "码流本身有没有变"的间接证据(码流长度由工作量和机器共同决定):
    //   下一轮若 preRunMs 与上一轮一致、而 o.ms 又掉了, 就说明掉掉的那部分只可能来自
    //   计时区间内部(自检 / 帧-遍), 排除了"整机当时就是快/慢"这种解释。
    double preRunT0 = nowMsV4();
    for (int f = 0; f < kVFrameCount4; ++f) {
        const bool isI = (f % kVIInterval4) == 0;
        generateFrameV4(f, lumaNoise, chromaNoise, y, u, v);
        encodeFrameV4(bw, f, qp, isI, y, u, v, refY, refU, refV, refY, refU, refV, 1, store);
        if (bw.overflowed()) { break; }
    }
    const double preRunMs = nowMsV4() - preRunT0;
    const size_t streamBytes = bw.bytesUsed();

    double psnrSum = 0.0;
    double maeSum = 0.0;
    int decodedFrames = 0;
    int failedFrames = 0;
    int passes = 0;

    std::vector<uint8_t> decY(kVLumaSize4, 0);
    std::vector<uint8_t> decU(kVChromaSize4, 0);
    std::vector<uint8_t> decV(kVChromaSize4, 0);

    if (streamBytes > 0) {
        // ==================== 解码计时区间的工作量标定(2026-10-04 第二次真机复核) ====================
        // 【调整前的估算错在哪】原注释按"解码器没有运动搜索, 所以比编码快 8~40 倍"来外推
        //   (7~36 Mpx/s), 实测完全不成立: 解码 4.0 Mpx/s 只有编码 1.1 Mpx/s 的 3.66 倍。
        //   根因不是"规模没调够", 而是解码的熵解码阶段是串行逐位的(见下方"耗时构成"),
        //   它的单位成本远高于编码端那套"宏块级并行 + 整字节输出"的路径。旧估算把这个
        //   常数 factor 猜错了, 于是"预计 1.6~2.4 s"变成了实测 14.8 s。
        // 【实测(本轮, 同一核)】帧数 4 x passes 16 = 64 个"帧-遍", o.ms = 14801.1 ms,
        //   metric 4.0 Mpx/s。自洽性: 64 x 921600 / 14.8011 s = 3.985 Mpx/s ✓。
        //   -> 解码单帧成本 = 14801.1 / 64 = 231.27 ms/帧-遍(跑在 cpu=5, 位次 9 的核)。
        // 【每帧成本与规模无关的理由(外推的全部依据)】
        //   * 每 pass 重置一次参考帧缓冲(3 x fill: 921600 + 2 x 230400 B ≈ 1.4 MB),
        //     与"第几遍"无关;
        //   * 每帧的解析量只由该帧的码流决定: 帧 0 是 I 帧(kVIInterval4 = 15, 本规模下
        //     帧 0..4 里只有帧 0 是 I), 帧 1..4 都是 P 帧, 结构不随帧数变化;
        //   * 重建阶段按宏块区间并行, 每帧 3600 个宏块的固定工作量;
        //   * 因此"帧-遍"这个单位的工作量是常数, 总耗时 = 帧-遍数 x 231.27 ms。
        // 【本次调整】帧数 4 -> 5(见文件顶部标定), passes 16 -> **2**:
        //   计时区间 = 5 x 2 = 10 帧-遍 = 9.216 Mpx
        //   -> 预计 o.ms = 10 x 231.27 ms = **2313 ms**(对照调整前 64 x 231.27 = 14801 ms,
        //      与实测 14801.1 ms 完全对上)。metric 不变(4.0 Mpx/s), 因为分子分母同倍缩小。
        // ==================== 耗时构成(2026-10-04 按码流统计重新核算, 修正了上一版结论) ====================
        // 上一版注释写"串行的熵解码是主项"——这条结论是错的, 现按离线码流统计
        // (D:\_ab_work\gb7_video_sim.py + gb7v_stats.json, 与源码逐语句复刻, 用真实的
        //  DCT/量化算出每帧的符号数与系数)重新核算。每 pass(4 帧)的真实工作量:
        //   * 熵解码(串行, decodeFrameParseV4):
        //       - Huffman 符号 127,971 个(level 106,371 + MV 21,600);
        //       - 旧实现的"码表探测"次数 = sum(码长 x 表长) = 27,760,082 次
        //         (其中 71.6% 花在 EOB 上: 82,136 个 EOB x 242 次探测);
        //       - 位读取 readBit 调用 = 327,064(符号)+ 91,970(符号位/幅度位) ≈ 419k 次;
        //   * 重建(串行, threads=1 时 reconstructFrameV4 走串行分支):
        //       - IDCT 块 82,136 个(16x16 纯拷贝 0 个: P 帧 3600 个宏块全部判为 inter);
        //       - 每块 1024 次 int64 乘加 + 64 次反量化 + 64 次加预测/夹取/存
        //         -> 84,107,264 次乘加 + 5.26M + 5.26M 次标量运算;
        //   * 量级对比(不需要任何成本模型): 84.1M 次 int64 乘加 vs 27.8M 次码表探测,
        //     即重建的工作量是熵解码的约 3 倍。因此每帧-遍 231.27 ms 里:
        //       重建 ≈ 80%, 熵解码 ≈ 20%(离线微基准: 熵 20.3% : 重建 79.7%)。
        //   上一版注释把这两段的主次搞反了 —— 那是"多核阶段把重建并行掉之后"的图景,
        //   而 CS1 单核阶段固定 threads = 1, 两段都是串行的。
        // 【因此查表 + 缓冲位读取这两项优化的天花板】(它们只作用于熵解码那 ~20%):
        //   若熵解码本身快 k 倍, 端到端只快 1 / (0.8 + 0.2/k):
        //       k=2 -> 1.11x ; k=3 -> 1.18x ; k=5 -> 1.32x ; k=10 -> 1.43x
        //   即端到端不可能拿到 5~10x(用户期望的那个倍数只属于熵解码那一段)。
        //   本次实测的熵解码单位成本(27.76M 次探测 + 0.42M 次逐位读 = 19.8 s 里的一部分)
        //   与本机 x64 上的 V8 微基准差异很大, 无法在这里推出 k 的真值 -> 规模按"最保守"
        //   选: 假定 k = 2~3(端到端 1.11~1.18x), 让调整后的耗时仍然落在区间内。
        // 【规模为什么不改回 4 x 16 = 64 帧-遍】(用户要求: 优化兑现 5~10x 就改回去)
        //   64 帧-遍在本轮实测核上 = 14.8 s; 就算端到端拿到 1.43x(熵解码快 10 倍的天花板)
        //   也还有 10.3 s, 远超 3.0 s。所以"改回 64 帧-遍"只有在优化真能带来 ~5x 端到端
        //   加速时才成立, 而上面的核算证明那不可能(重建占 80%)。按用户给的备选规则
        //   ("若只拿到 2~3x, 按实测比例选一个使耗时落在 1.5~3 s 的规模"), 这里**维持
        //   5 帧 x 2 passes = 10 帧-遍**, 并把四个数写清楚(见文件顶部"优化前后"表)。
        // 【端到端墙钟(不计入 o.ms, 但用户能感知)】解码前的编码预跑固定 threads=1:
        //   5 帧 x 846.7 ms ≈ 4.2 s。
        passes = 2;
        // metric 分子只认"真正解码完成的帧": 原来固定按 帧数 x passes 计, 一旦某一帧
        // 解码失败就 break, 实际工作量远小于分子, 吞吐会被放大十几倍(真机上每 pass
        // 只解出第 0 帧就 break, 于是报出 1976 Mpx/s —— 编码器只有 0.9 Mpx/s)。
        uint64_t framesDecodedTotal = 0;
        int passesDone = 0;
        int failPass = -1;
        int failFrame = -1;
        bool streamRejected = false;
        auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
        double t0 = nowMsV4();
        std::clock_t cpu0 = std::clock();
        // ---- 全流自检(每次调用只做一遍) ----
        // 用参考实现(逐位读 + 线性扫全表)把整条码流再解一遍, 逐符号/逐系数与查表结果比对。
        // 它只读码流、不分配、不改任何状态, 唯一的代价是时间。
        // 计时口径: 自检计入 o.ms, 但只跑一次 —— 相当于给计时区间加了"一遍解析"的
        // 1/passes 倍(5 帧 x 2 passes 时约 20% 的解析量, 即总时间的约 10%)。之所以不放到
        // 计时区间之外, 是因为那样会把真实开销藏起来; 之所以只跑一遍, 是为了让
        // "每帧-遍成本"这个标定锚点仍然可比较。
        // 2026-10-06 修复(6.2 掉档 31% 的排查结论之一)
        //   这里原来是 `if (!verifyFrameParseV4(chk, f, qp, isI, selfCheck)) { break; }`。
        //   verifyFrameParseV4 在两种情况下返回 false:
        //     (a) 新查表与旧线性扫描出现不一致 —— 注意 selfCheck.mismatches 是累计量,
        //         一旦非 0, 后续每一帧都会在开头立刻返回 false;
        //     (b) 本帧两侧都读到流尾。
        //   原写法一命中就 break —— 于是"把整条码流再解一遍"会静默缩水成"只解了前 k 帧",
        //   计时区间(t0..t1)跟着变小; 而这个缩水对 runlog 完全不可见:
        //     * o.metric 的分子只认"真正解码完成的帧"(framesDecodedTotal), 自检不进去;
        //     * o.unit 只看 failedFrames / framesDecodedTotal, 自检也不进去;
        //     * selfCheck.frames 原来只 fputs 到 stderr(进不了 runlog)。
        //   这正是 6.2 那一轮(1681.2 ms)无法从日志判断"跑满还是提前退"的原因。
        //   改成每帧都跑完(不 break), 只记录覆盖帧数与不一致数 —— 计时区间的工作量因此恒定,
        //   且 o.diag 会把覆盖情况写进结果 JSON。
        //   工作量口径: 正常情况下(无不一致)本改动不增加也不减少任何工作量 ——
        //   标定注释写的就是"把整条码流再解一遍", 5 帧本来就该跑满; 只有"本来会提前退"的
        //   异常情况下, 才把少掉的帧补回来。
        const bool runSelfCheck = true;
        const int wantFrames = kVFrameCount4 * passes;
        const int wantCheckFrames = kVFrameCount4;
        HuffSelfCheckV4 selfCheck;
        double tChk0 = nowMsV4();
        if (runSelfCheck) {
            BitReaderV4 chk(bw.data().data(), streamBytes);
            for (int f = 0; f < wantCheckFrames; ++f) {
                const bool isI = (f % kVIInterval4) == 0;
                (void)verifyFrameParseV4(chk, f, qp, isI, selfCheck);
            }
        }
        double tChk1 = nowMsV4();
        const double selfCheckMs = tChk1 - tChk0;
        char chkbuf[320];
        snprintf(chkbuf, sizeof(chkbuf),
                 "[gb7] Video Decoder self-check: fast-table == linear-scan, frames=%d/%d symbols=%lld mismatches=%lld (%.1f ms, 计入 o.ms)",
                 selfCheck.frames, wantCheckFrames, selfCheck.symbols, selfCheck.mismatches, selfCheckMs);
        fputs(chkbuf, stderr);
        fputc('\n', stderr);
        (void)runSelfCheck;
        for (int pass = 0; pass < passes; ++pass) {
            BitReaderV4 br(bw.data().data(), streamBytes);
            std::fill(refY.begin(), refY.end(), (uint8_t)128);
            std::fill(refU.begin(), refU.end(), (uint8_t)128);
            std::fill(refV.begin(), refV.end(), (uint8_t)128);
            passesDone = pass + 1;
            for (int f = 0; f < kVFrameCount4; ++f) {
                const bool isI = (f % kVIInterval4) == 0;
                if (!decodeFrameV4(br, f, qp, isI, decY, decU, decV, refY, refU, refV, threads, store)) {
                    ++failedFrames;
                    streamRejected = true;
                    failPass = pass;
                    failFrame = f;
                    break;
                }
                ++framesDecodedTotal;
                if (pass == 0) {
                    generateFrameV4(f, lumaNoise, chromaNoise, y, u, v);
                    double mae = 0.0;
                    psnrSum += psnrLumaV4(y, decY, &mae);
                    maeSum += mae;
                    ++decodedFrames;
                }
                refY = decY;
                refU = decU;
                refV = decV;
            }
            if (streamRejected) {
                break;      // 码流被拒后重复跑同样的失败没有意义
            }
        }
        double t1 = nowMsV4();
        auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)

        uint32_t h = 2166136261u;
        for (size_t i = 0; i < kVLumaSize4; i += 7) {
            h ^= (uint32_t)decY[i];
            h *= 16777619u;
        }
        volatile uint32_t sinkH = h;
        (void)sinkH;

        // 真实并行度: 参与线程的 CPU 时间之和 / 墙钟时间
        double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
        o.parallelism = gb7Parallelism(cpuMs, t1 - t0);

        double secondsElapsed = (t1 - t0) / 1000.0;
        if (!(secondsElapsed > 1e-6)) { secondsElapsed = 1e-6; }
        // 分子 = 真正解码完成的像素数(帧数 x 每帧亮度像素), 与计时区间严格对应
        double mpx = (double)kVPixelsPerFrame4 * (double)framesDecodedTotal / 1000000.0;
        double mpxPerSec = mpx / secondsElapsed;
        if (!(mpxPerSec == mpxPerSec) || mpxPerSec < 0.0) { mpxPerSec = 0.0; }
        if (mpxPerSec > 1e7) { mpxPerSec = 1e7; }

        char logbuf[320];
        double denom = (decodedFrames > 0) ? (double)decodedFrames : 1.0;
        snprintf(logbuf, sizeof(logbuf),
                 "[gb7] Video Decoder: passes=%d/%d, decoded=%llu frames (pass0=%d), failed=%d, stream=%u B, "
                 "avgPSNR(Y)=%.2f dB, MAE=%.3f, %.1f Mpx/s, crc=0x%08X",
                 passesDone, passes, (unsigned long long)framesDecodedTotal, decodedFrames, failedFrames,
                 (unsigned)streamBytes, psnrSum / denom, maeSum / denom, mpxPerSec, (unsigned)h);
        fputs(logbuf, stderr);
        fputc('\n', stderr);

        char buf32[64];
        // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
        snprintf(buf32, sizeof(buf32), "%.4g", mpxPerSec);
        o.ms = t1 - t0;
        o.metric = buf32;
        // wantFrames 已在计时区间之前定义(= kVFrameCount4 x passes), 这里直接用它判完整性.
        if (failedFrames == 0 && framesDecodedTotal == (uint64_t)wantFrames) {
            o.unit = "Mpx/s";
        } else if (framesDecodedTotal == 0) {
            // 失败不静默: 分子已经是 0, 把原因放进 unit(界面可见), 不再只显示 0.0
            char why[192];
            snprintf(why, sizeof(why),
                     "Mpx/s (failed: stream rejected at frame %d of pass %d, 0/%d frames decoded)",
                     failFrame, failPass, wantFrames);
            o.unit = why;
        } else {
            char why[192];
            snprintf(why, sizeof(why),
                     "Mpx/s (partial: %llu/%d frames decoded, rejected at frame %d of pass %d)",
                     (unsigned long long)framesDecodedTotal, wantFrames, failFrame, failPass);
            o.unit = why;
        }

        // ==================== 自证字段(o.diag): 让下一轮的 runlog 自己说明"跑满还是提前退" ====================
        // 为什么必须进结果 JSON 而不是只 print 到 stderr: 本项的计时区间(t0..t1)里有两段工作,
        //   一段是 10 个"帧-遍"(其完成度进 metric 分子与 unit), 另一段是"整条码流再解一遍"的
        //   全流自检(不进 metric 分子, 也不进 unit)。自检一旦提前退出, o.ms 会掉, 而
        //   metric / unit / score 三个字段一个字都不变 —— 6.2 那轮(1681.2 ms)就是这么变成
        //   悬案的: 从 runlog 只能看到"变快了", 看不到"哪一段少了"。
        // 键名(用户点名要求的五个 + 自检的三个):
        //   passes              : 本轮计划的解码遍数(常量, 见上方"工作量标定")
        //   passesDone          : 真正跑完的遍数(提前 break 时 < passes)
        //   decodedFrames       : 真正解码完成的"帧-遍"总数(metric 分子的那一项)
        //   wantFrames          : 计划等于 passes x kVFrameCount4(跑满就是 10)
        //   failedFrames        : 解码失败的帧数(0 = 没有走过失败路径)
        //   selfCheckFrames     : 全流自检实际覆盖的帧数 / 应覆盖帧数(必须 5/5)
        //   selfCheckMismatches : 新查表 vs 旧线性扫描的不一致数(应当恒为 0)
        //   selfCheckMs         : 自检本身占掉的毫秒(计入 o.ms)
        //   preRunMs            : 计时区间之前那段"编码预跑"的毫秒(不计入 o.ms) —— 机器速度参照物
        // 判读法则(下一轮直接照这条读):
        //   decodedFrames == wantFrames && failedFrames == 0 && selfCheckFrames == wantCheckFrames
        //     -> 10 个帧-遍 + 整条码流自检全部跑满, o.ms 就是"跑满"的耗时;
        //   任何一项对不上 -> 提前退了, 少的正是对不上的那一项。
        // 纯诊断: 不参与 metric / unit / 计分, 不改变任何工作量。
        {
            char dbuf[384];
            snprintf(dbuf, sizeof(dbuf),
                     "passes=%d passesDone=%d decodedFrames=%llu wantFrames=%d failedFrames=%d "
                     "selfCheckFrames=%d/%d selfCheckMismatches=%lld selfCheckMs=%.1f "
                     "preRunMs=%.1f streamBytes=%u",
                     passes, passesDone, (unsigned long long)framesDecodedTotal, wantFrames, failedFrames,
                     selfCheck.frames, wantCheckFrames, selfCheck.mismatches, selfCheckMs,
                     preRunMs, (unsigned)streamBytes);
            o.diag = dbuf;
        }
        return o;
    }

    // 码流为空(不应发生): 返回安全的退化结果
    o.ms = 0.0;
    o.metric = "0.0";
    o.unit = "Mpx/s";
    o.parallelism = 1.0;
    // 退化路径同样给满形状的 diag(全 0): ArkTS 侧 / 日志侧不需要判 undefined
    o.diag = "passes=0 passesDone=0 decodedFrames=0 wantFrames=0 failedFrames=0 "
             "selfCheckFrames=0/0 selfCheckMismatches=0 selfCheckMs=0.0 streamBytes=0 (空码流, 未进计时区间)";
    return o;
}
