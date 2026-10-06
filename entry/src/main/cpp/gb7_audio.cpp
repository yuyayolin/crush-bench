// CS1 第四批之二: Media / Audio Encoder
//
// 实现一个符合 FLAC 规范的帧内(intra-frame)无损音频编码器:
//   * 分块 4096 样本/块, 立体声双声道独立(independent)编码;
//   * 每块每声道在 [固定预测器(order 0..4)] 与 [LPC(order 1..12, Levinson-Durbin +
//     系数量化到 QLP 精度)] 之间按"估计残差比特数 + 头部开销"选最优;
//   * 残差用 Rice 分区编码(partition order 0..8, 每分区独立选最优 Rice 参数 k);
//   * 输出真实 FLAC 位流: "fLaC" + STREAMINFO + 逐帧(帧头 + UTF-8 帧号 + CRC-8 帧头
//     校验 + 子帧 + 字节对齐 + CRC-16 全帧校验)。
// 全部为 FLAC 规范定义的标准算法(FLAC format spec: frame header / subframe /
// residual coding / CRC-8 poly 0x07 / CRC-16 poly 0x8005), 未使用任何私有格式。
//
// 输入: 程序内生成的 48kHz / 16bit / 立体声 PCM, 长度 60 秒 = 2,880,000 样本/声道
//       = 11,520,000 字节原始数据。信号为"类音乐"内容: 多个正弦分音 + 打击瞬态 +
//       指数衰减包络 + 噪声纹理 + 缓变 pad, 非静音。
// metric: 输入 PCM 的吞吐量 MB/s(原始 16bit 立体声数据量 / 编码耗时), 1 位小数。
//         压缩率只写入注释与运行日志(stderr), 不进入 metric。
//
// 目标: aarch64-linux-ohos, clang, C++17, musl libc。纯整数/标量, 无 SIMD, 单线程。

#include "gb7.h"
#include "gb7_parallel.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

double nowMsA4()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// CRC-8 (poly x^8+x^2+x^1+x^0 = 0x07) 与 CRC-16 (poly x^16+x^15+x^2+1 = 0x8005)
// ---------------------------------------------------------------------------
uint8_t crc8Flac(const uint8_t* data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (uint8_t)((crc & 0x80u) ? ((uint32_t)(crc << 1) ^ 0x07u) : (uint32_t)(crc << 1));
        }
    }
    return crc;
}

uint16_t crc16Flac(const uint8_t* data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));
        for (int b = 0; b < 8; ++b) {
            crc = (uint16_t)((crc & 0x8000u) ? ((uint32_t)(crc << 1) ^ 0x8005u) : (uint32_t)(crc << 1));
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// 位写入器: 高位优先(FLAC 位序)。容量预分配, 只增不减, 不越界。
// ---------------------------------------------------------------------------
class BitWriterA4 {
public:
    explicit BitWriterA4(size_t reserveBytes) { buf.reserve(reserveBytes); }

    void writeBits(uint64_t value, int bits)
    {
        if (bits <= 0) { return; }
        if (bits < 64) { value &= (((uint64_t)1 << bits) - 1u); }
        int remaining = bits;
        while (remaining > 0) {
            if (accBits == 0) { buf.push_back(0); }
            int room = 8 - accBits;
            int take = (remaining < room) ? remaining : room;
            int shift = remaining - take;
            uint8_t chunk = (uint8_t)((value >> shift) & (((uint64_t)1 << take) - 1u));
            buf.back() = (uint8_t)(buf.back() | (uint8_t)(chunk << (room - take)));
            accBits = (accBits + take) & 7;
            remaining -= take;
        }
    }

    void writeSigned(int64_t value, int bits)
    {
        if (bits <= 0) { return; }
        uint64_t mask = (((uint64_t)1 << bits) - 1u);
        writeBits((uint64_t)value & mask, bits);
    }

    // 一元编码: n 个 0 后跟一个 1
    void writeUnary(uint32_t n)
    {
        uint32_t left = n;
        while (left >= 32u) { writeBits(0u, 32); left -= 32u; }
        writeBits(1u, (int)left + 1);
    }

    void writeUtf8(uint32_t value)
    {
        if (value < 0x80u) {
            writeBits(value, 8);
        } else if (value < 0x800u) {
            writeBits(0xC0u | (value >> 6), 8);
            writeBits(0x80u | (value & 0x3Fu), 8);
        } else if (value < 0x10000u) {
            writeBits(0xE0u | (value >> 12), 8);
            writeBits(0x80u | ((value >> 6) & 0x3Fu), 8);
            writeBits(0x80u | (value & 0x3Fu), 8);
        } else if (value < 0x200000u) {
            writeBits(0xF0u | (value >> 18), 8);
            writeBits(0x80u | ((value >> 12) & 0x3Fu), 8);
            writeBits(0x80u | ((value >> 6) & 0x3Fu), 8);
            writeBits(0x80u | (value & 0x3Fu), 8);
        } else {
            writeBits(0xF8u | (value >> 24), 8);
            writeBits(0x80u | ((value >> 18) & 0x3Fu), 8);
            writeBits(0x80u | ((value >> 12) & 0x3Fu), 8);
            writeBits(0x80u | ((value >> 6) & 0x3Fu), 8);
            writeBits(0x80u | (value & 0x3Fu), 8);
        }
    }

    void writeRawBytes(const uint8_t* data, size_t len)
    {
        for (size_t i = 0; i < len; ++i) { writeBits(data[i], 8); }
    }

    void alignToByte()
    {
        if (accBits != 0) { writeBits(0u, 8 - accBits); }
    }

    size_t size() const { return buf.size(); }
    const std::vector<uint8_t>& bytes() const { return buf; }
    std::vector<uint8_t>& mutBytes() { return buf; }
    // 复位以复用缓冲(只在字节对齐处调用): 并行编码时每个工作单元复用自己的写位器
    void reset()
    {
        buf.clear();
        accBits = 0;
    }
    // 把最后 2 字节改写为 CRC-16(调用前必须已写入 2 字节占位)
    void setLastTwoBytes(uint16_t v)
    {
        size_t n = buf.size();
        if (n >= 2) {
            buf[n - 2] = (uint8_t)(v >> 8);
            buf[n - 1] = (uint8_t)(v & 0xFFu);
        }
    }

private:
    std::vector<uint8_t> buf;
    int accBits = 0;
};

// ---------------------------------------------------------------------------
// 正弦查表(只用于生成输入信号, 不在编码路径上)
// ---------------------------------------------------------------------------
const int kSinBitsA4 = 13;
const int kSinSizeA4 = 1 << kSinBitsA4; // 8192
const uint32_t kSinMaskA4 = (uint32_t)(kSinSizeA4 - 1);

void buildSinTableA4(std::vector<float>& tbl)
{
    tbl.resize((size_t)kSinSizeA4 + 1u);
    for (int i = 0; i <= kSinSizeA4; ++i) {
        double ph = 6.283185307179586476925286766559 * (double)i / (double)kSinSizeA4;
        tbl[(size_t)i] = (float)std::sin(ph);
    }
}

// 相位: 32bit 无符号定点(整圈 = 2^32), 线性插值查表
inline float sinPhaseA4(uint32_t phase, const std::vector<float>& tbl)
{
    uint32_t idx = (phase >> (32 - kSinBitsA4)) & kSinMaskA4;
    uint32_t frac = phase << kSinBitsA4;
    float a = tbl[(size_t)idx];
    float b = tbl[(size_t)idx + 1u];
    float t = (float)((double)frac * (1.0 / 4294967296.0));
    return a + (b - a) * t;
}

// ---------------------------------------------------------------------------
// 生成 60 秒 48kHz 16bit 立体声"类音乐"信号
//   每 4 秒 8 拍(0.5s/拍): 基频 + 3 个分音, 指数衰减包络, 每 4 拍重音;
//   每拍起始 6ms 宽带瞬态(模拟鼓点); 叠加双声道失谐 pad 与低电平噪声纹理。
// ---------------------------------------------------------------------------
void generateMusicA4(std::vector<int16_t>& pcm, int sampleRate, int frames, int channels)
{
    pcm.assign((size_t)frames * (size_t)channels, 0);
    std::vector<float> sinTbl;
    buildSinTableA4(sinTbl);

    const int beatSamples = sampleRate / 2;                 // 0.5 秒
    const int barSamples = beatSamples * 8;                 // 4 秒
    static const int scale[8] = {0, 3, 5, 7, 5, 3, 7, 10};  // 半音
    const double baseHz = 110.0;

    uint32_t rng = 0x9E3779B9u;
    float noiseLp[2] = {0.0f, 0.0f};
    uint32_t padPhase[2][2] = {{0u, 0x20000000u}, {0x0AAAAAAu, 0x35555555u}};
    uint32_t noisePhase[2] = {0x12345678u, 0x9ABCDEF0u};
    const uint32_t padInc[2][2] = {
        {(uint32_t)(4294967296.0 * 220.0 / (double)sampleRate),
         (uint32_t)(4294967296.0 * 330.6 / (double)sampleRate)},
        {(uint32_t)(4294967296.0 * 219.1 / (double)sampleRate),
         (uint32_t)(4294967296.0 * 329.2 / (double)sampleRate)},
    };
    const double harmInc[3] = {1.0, 2.0, 3.0};
    const double harmAmp[3] = {0.62, 0.30, 0.16};
    const int transientLen = sampleRate * 6 / 1000;         // 6ms

    for (int n = 0; n < frames; ++n) {
        int barPos = n % barSamples;
        int beatIdx = barPos / beatSamples;
        int beatOff = barPos % beatSamples;
        int note = scale[beatIdx & 7];
        double f0 = baseHz * std::pow(2.0, (double)note / 12.0);
        double env = std::exp(-3.2 * (double)beatOff / (double)beatSamples)
            * (((beatIdx & 3) == 0) ? 1.0 : 0.68);
        double te = 0.0;
        if (beatOff < transientLen) {
            te = std::exp(-14.0 * (double)beatOff / (double)transientLen);
        }

        for (int c = 0; c < 2; ++c) {
            double voice = 0.0;
            for (int h = 0; h < 3; ++h) {
                double hz = f0 * harmInc[h] * (c == 0 ? 1.0 : 1.0015);
                double cyc = (double)n * hz / (double)sampleRate;
                uint32_t ph = (uint32_t)(int64_t)(cyc * 4294967296.0);
                voice += harmAmp[h] * (double)sinPhaseA4(ph, sinTbl);
            }
            voice *= env * 5200.0;

            double pad = 0.5 * (double)sinPhaseA4(padPhase[c][0], sinTbl)
                + 0.34 * (double)sinPhaseA4(padPhase[c][1], sinTbl);
            padPhase[c][0] = padPhase[c][0] + padInc[c][0];
            padPhase[c][1] = padPhase[c][1] + padInc[c][1];

            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            float white = (float)((double)(int32_t)(rng >> 8) - 8388608.0) * (1.0f / 8388608.0f);
            noiseLp[c] = noiseLp[c] * 0.72f + white * 0.28f;
            double noise = (double)noiseLp[c] * 240.0;

            double tr = 0.0;
            if (te > 0.0) {
                noisePhase[c] += (c == 0 ? 0x51E2A3B4u : 0x2C7F19D3u);
                double burst = (double)white * 0.7 + 0.3 * (double)sinPhaseA4(noisePhase[c], sinTbl);
                tr = burst * te * 5200.0 * (c == 0 ? 1.0 : 0.96);
            }

            double v = voice + pad * 2100.0 + noise + tr;
            if (!(v == v)) { v = 0.0; } // NaN 保护
            if (v > 32767.0) { v = 32767.0; }
            if (v < -32768.0) { v = -32768.0; }
            pcm[(size_t)n * 2u + (size_t)c] = (int16_t)(v >= 0.0 ? (int)(v + 0.5) : -(int)(-v + 0.5));
        }
    }
}

// ---------------------------------------------------------------------------
// 编码参数
// ---------------------------------------------------------------------------
const int kBlockSizeA4 = 4096;
const int kMaxLpcOrderA4 = 12;
const int kMaxPartOrderA4 = 8;
const int kMaxRiceParamA4 = 14;    // Rice 参数 k 上限(规范中 15 是 escape 码)
const int kCoeffPrecA4 = 15;       // QLP 系数精度(位)
const int kMaxResidualBitsA4 = 25; // 残差位宽上限(FLAC 规范约束)

inline int64_t absI64A4(int64_t v) { return v < 0 ? -v : v; }

// order 阶固定预测器的残差(前 order 个样本为 warmup, 不产生残差)
inline int64_t fixedResidualA4(int order, const int16_t* b, int i)
{
    switch (order) {
    case 0: return (int64_t)b[i];
    case 1: return (int64_t)b[i] - (int64_t)b[i - 1];
    case 2: return (int64_t)b[i] - 2 * (int64_t)b[i - 1] + (int64_t)b[i - 2];
    case 3: return (int64_t)b[i] - 3 * (int64_t)b[i - 1] + 3 * (int64_t)b[i - 2] - (int64_t)b[i - 3];
    default:
        return (int64_t)b[i] - 4 * (int64_t)b[i - 1] + 6 * (int64_t)b[i - 2] - 4 * (int64_t)b[i - 3]
            + (int64_t)b[i - 4];
    }
}

// 某个残余分区的最优 Rice 参数与估计比特数(4bit 参数 + 每样本 (k+1)bit 一元 + k bit 余数)
void riceBestA4(const int64_t* res, int count, int* outK, int64_t* outBits)
{
    int bestK = 0;
    int64_t bestBits = (int64_t)1 << 62;
    for (int k = 0; k <= kMaxRiceParamA4; ++k) {
        int64_t sum = 0;
        for (int i = 0; i < count; ++i) {
            int64_t q = absI64A4(res[i]) >> k;
            if (q > 1000000000LL) { q = 1000000000LL; }
            sum += q;
            if (sum > ((int64_t)1 << 60)) { sum = (int64_t)1 << 60; break; }
        }
        int64_t bits = 4 + sum + (int64_t)count * (int64_t)(k + 1);
        if (bits < bestBits) { bestBits = bits; bestK = k; }
    }
    *outK = bestK;
    *outBits = bestBits;
}

// 在分区 order 0..maxOrder 中选最优(分区数 2^order, 每分区样本数 n >> order)
void bestPartitionsA4(const int64_t* res, int n, int maxOrder, int* partOrder)
{
    *partOrder = 0;
    int64_t best = (int64_t)1 << 62;
    for (int po = 0; po <= maxOrder; ++po) {
        int parts = 1 << po;
        if (parts > n || parts > 256) { break; }
        int partLen = n >> po;
        if (partLen < 1) { break; }
        int64_t total = 8; // 4bit partition order + 每分区 4bit Rice 参数
        for (int p = 0; p < parts; ++p) {
            int k = 0;
            int64_t b = 0;
            riceBestA4(res + (size_t)p * (size_t)partLen, partLen, &k, &b);
            total += b;
            if (total > ((int64_t)1 << 60)) { total = (int64_t)1 << 60; break; }
        }
        if (total < best) {
            best = total;
            *partOrder = po;
        }
    }
}

// 写入残余子帧体(固定预测器与 LPC 共用)
void writeResidualA4(BitWriterA4& bw, const int64_t* res, int n, int maxOrder)
{
    int partOrder = 0;
    bestPartitionsA4(res, n, maxOrder, &partOrder);
    int parts = 1 << partOrder;
    int partLen = n >> partOrder;
    int ks[256];
    for (int p = 0; p < parts; ++p) {
        int k = 0;
        int64_t b = 0;
        riceBestA4(res + (size_t)p * (size_t)partLen, partLen, &k, &b);
        if (k < 0) { k = 0; }
        if (k > kMaxRiceParamA4) { k = kMaxRiceParamA4; }
        ks[p] = k;
    }
    bw.writeBits((uint64_t)partOrder, 4);
    for (int p = 0; p < parts; ++p) { bw.writeBits((uint64_t)ks[p], 4); }
    for (int p = 0; p < parts; ++p) {
        const int64_t* src = res + (size_t)p * (size_t)partLen;
        int k = ks[p];
        for (int i = 0; i < partLen; ++i) {
            int64_t u = src[i];
            uint64_t mag = (u < 0) ? (((uint64_t)(-(u + 1)) << 1) | 1u) : ((uint64_t)u << 1);
            uint32_t q = (uint32_t)(mag >> k);
            if (q > 1000000u) { q = 1000000u; } // 仅防御: 正常路径不会触发
            bw.writeUnary(q);
            if (k > 0) { bw.writeBits(mag, k); }
        }
    }
}

// Levinson-Durbin: 由自相关求 order 阶 LPC 系数
bool levinsonDurbinA4(const double* ac, int order, double* outLpc)
{
    if (order < 1) { return false; }
    if (!(ac[0] > 1e-9)) { return false; }
    double lpc[16];
    for (int i = 0; i < 16; ++i) { lpc[i] = 0.0; }
    double err = ac[0];
    for (int i = 0; i < order; ++i) {
        double acc = ac[i + 1];
        for (int j = 0; j < i; ++j) { acc -= lpc[j] * ac[i - j]; }
        double k = acc / err;
        if (!(k == k)) { return false; }
        if (k > 0.999) { k = 0.999; }
        if (k < -0.999) { k = -0.999; }
        lpc[i] = k;
        for (int j = 0; j < i / 2; ++j) {
            double tmp = lpc[j];
            lpc[j] = tmp - k * lpc[i - 1 - j];
            lpc[i - 1 - j] = lpc[i - 1 - j] - k * tmp;
        }
        if (i % 2 == 0) {
            int mid = i / 2;
            lpc[mid] = lpc[mid] * (1.0 - k);
        }
        err = err * (1.0 - k * k);
        if (!(err > 1e-9)) { return false; }
    }
    for (int i = 0; i < order; ++i) {
        if (!(lpc[i] == lpc[i])) { return false; }
        outLpc[i] = lpc[i];
    }
    return true;
}

// 量化 LPC 系数到 QLP(带符号, 精度 <= kCoeffPrecA4), 输出 shift
void quantizeLpcA4(const double* lpc, int order, int32_t* q, int* shift)
{
    double cmax = 0.0;
    for (int i = 0; i < order; ++i) {
        double a = lpc[i] < 0 ? -lpc[i] : lpc[i];
        if (a > cmax) { cmax = a; }
    }
    int msb = 0;
    double t = cmax;
    while (t >= 2.0 && msb < 30) { t *= 0.5; ++msb; }
    int sh = kCoeffPrecA4 - 2 - msb; // 保证量化值不超过 15bit 有符号
    if (sh > 15) { sh = 15; }
    if (sh < -16) { sh = -16; }
    double scale;
    if (sh >= 0) {
        scale = (double)((uint32_t)1 << sh);
    } else {
        scale = 1.0 / (double)((uint32_t)1 << (-sh));
    }
    for (int i = 0; i < order; ++i) {
        double v = lpc[i] * scale;
        if (v > 16383.0) { v = 16383.0; }
        if (v < -16384.0) { v = -16384.0; }
        q[i] = (int32_t)(v >= 0.0 ? (int)(v + 0.5) : -(int)(-v + 0.5));
    }
    *shift = sh;
}

// 量化系数下的预测值(解码端也用同一公式)
inline int64_t lpcPredictA4(const int32_t* q, int order, const int16_t* b, int idx, int shift)
{
    int64_t acc = 0;
    for (int i = 0; i < order; ++i) {
        acc += (int64_t)q[i] * (int64_t)b[idx - 1 - i];
    }
    if (shift > 0) { acc >>= shift; }
    return acc;
}

struct BestSubA4 {
    int type = 1;      // 0=constant, 1=verbatim, 8..12=fixed(order = type-8), 32=LPC
    int lpcOrder = 0;
    int64_t bits = 0;
    int32_t coeff[16];
    int shift = 0;
    int32_t constant = 0;
};

// 单声道子帧最优预测器选择(代价 = 子帧头 + warmup + 系数 + 最优 Rice 残差比特)
void chooseSubframeA4(const int16_t* b, int n, int bps, BestSubA4* best)
{
    bool constant = true;
    for (int i = 1; i < n; ++i) {
        if (b[i] != b[0]) { constant = false; break; }
    }
    if (constant) {
        best->type = 0;
        best->lpcOrder = 0;
        best->bits = 8 + bps;
        best->constant = b[0];
        return;
    }

    std::vector<int64_t> res((size_t)n);

    // 1) verbatim 基准
    int64_t bestBits = 8 + (int64_t)n * (int64_t)bps;
    best->type = 1;
    best->lpcOrder = 0;
    best->bits = bestBits;

    // 2) 固定预测器 order 0..4
    for (int order = 0; order <= 4; ++order) {
        if (order >= n) { break; }
        for (int i = order; i < n; ++i) { res[(size_t)i] = fixedResidualA4(order, b, i); }
        int po = 0;
        bestPartitionsA4(res.data() + order, n - order, kMaxPartOrderA4, &po);
        int partLen = (n - order) >> po;
        int64_t rbits = 8;
        for (int p = 0; p < (1 << po); ++p) {
            int k = 0;
            int64_t bits = 0;
            riceBestA4(res.data() + order + (size_t)p * (size_t)partLen, partLen, &k, &bits);
            rbits += bits;
        }
        int64_t total = 8 + (int64_t)order * (int64_t)bps + rbits;
        if (total < bestBits) {
            bestBits = total;
            best->type = 8 + order;
            best->lpcOrder = 0;
            best->bits = total;
        }
    }

    // 3) LPC: 自相关 + Levinson-Durbin, 代价用"量化后"的真实残差统计
    double ac[16];
    for (int i = 0; i <= kMaxLpcOrderA4; ++i) {
        double s = 0.0;
        for (int j = i; j < n; ++j) { s += (double)b[j] * (double)b[j - i]; }
        ac[i] = s;
    }
    double lpc[16];
    if (levinsonDurbinA4(ac, kMaxLpcOrderA4, lpc)) {
        for (int order = 1; order <= kMaxLpcOrderA4; ++order) {
            int32_t q[16];
            int shift = 0;
            quantizeLpcA4(lpc, order, q, &shift);
            bool fits = true;
            for (int i = order; i < n; ++i) {
                int64_t r = (int64_t)b[i] - lpcPredictA4(q, order, b, i, shift);
                res[(size_t)i] = r;
                if (absI64A4(r) >= ((int64_t)1 << (kMaxResidualBitsA4 - 1))) { fits = false; break; }
            }
            if (!fits) { continue; }
            int po = 0;
            bestPartitionsA4(res.data() + order, n - order, kMaxPartOrderA4, &po);
            int partLen = (n - order) >> po;
            int64_t rbits = 8;
            for (int p = 0; p < (1 << po); ++p) {
                int k = 0;
                int64_t bits = 0;
                riceBestA4(res.data() + order + (size_t)p * (size_t)partLen, partLen, &k, &bits);
                rbits += bits;
            }
            int64_t total = 8 + 4 + 4 + (int64_t)order * kCoeffPrecA4 + rbits;
            if (total < bestBits) {
                bestBits = total;
                best->type = 32;
                best->lpcOrder = order;
                best->bits = total;
                best->shift = shift;
                for (int i = 0; i < order; ++i) { best->coeff[i] = q[i]; }
            }
        }
    }
}

// 写入一个 16bit 单声道子帧
void writeSubframeA4(BitWriterA4& bw, const int16_t* b, int n, int bps, const BestSubA4& best,
                     std::vector<int64_t>& scratch)
{
    bw.writeBits(0u, 1); // zero padding bit
    if (best.type == 0) {
        bw.writeBits(0u, 6);  // 000000 = constant
        bw.writeBits(0u, 1);  // wasted bits = 0
        bw.writeSigned((int64_t)best.constant, bps);
        return;
    }
    if (best.type == 1) {
        bw.writeBits(1u, 6);  // 000001 = verbatim
        bw.writeBits(0u, 1);
        for (int i = 0; i < n; ++i) { bw.writeSigned((int64_t)b[i], bps); }
        return;
    }
    if (best.type == 32) {
        int order = best.lpcOrder;
        bw.writeBits((uint64_t)(31 + order), 6); // 1xxxxx = LPC, order = xxxxx + 1
        bw.writeBits(0u, 1);
        for (int i = 0; i < order; ++i) { bw.writeSigned((int64_t)b[i], bps); }
        int prec = kCoeffPrecA4 - 1;             // QLP 精度字段
        bw.writeBits((uint64_t)prec, 4);
        int sh = best.shift;
        if (sh < 0) { sh = 0; }
        if (sh > 15) { sh = 15; }
        bw.writeSigned((int64_t)sh, 5);
        for (int i = 0; i < order; ++i) { bw.writeSigned((int64_t)best.coeff[i], kCoeffPrecA4); }
        scratch.resize((size_t)n);
        for (int i = order; i < n; ++i) {
            scratch[(size_t)i] = (int64_t)b[i] - lpcPredictA4(best.coeff, order, b, i, sh);
        }
        writeResidualA4(bw, scratch.data() + order, n - order, kMaxPartOrderA4);
        return;
    }
    // 固定预测器
    int order = best.type - 8;
    if (order < 0) { order = 0; }
    if (order > 4) { order = 4; }
    if (order >= n) { order = n - 1; }
    if (order < 0) { order = 0; }
    bw.writeBits((uint64_t)(8 + order), 6); // 001xxx = fixed, order = xxx
    bw.writeBits(0u, 1);
    for (int i = 0; i < order; ++i) { bw.writeSigned((int64_t)b[i], bps); }
    scratch.resize((size_t)n);
    for (int i = order; i < n; ++i) { scratch[(size_t)i] = fixedResidualA4(order, b, i); }
    writeResidualA4(bw, scratch.data() + order, n - order, kMaxPartOrderA4);
}

// ---------------------------------------------------------------------------
// 编码一帧(帧头 + CRC-8 + 两个子帧 + 字节对齐 + CRC-16), 返回该帧占用的比特数。
//
// 帧与帧之间完全独立, 这正是并行分解的依据:
//   * 每帧编码前先 alignToByte(), 因此帧的起始位置永远是字节边界, 上一帧的
//     比特长度不会泄漏到下一帧(帧内所有语法都由"本帧 PCM + 帧号"唯一决定);
//   * CRC-8 只覆盖本帧帧头, CRC-16 只覆盖本帧内容, 不跨帧;
//   * 因此把每个工作单元(一批帧)的位流写进各自的缓冲, 最后按帧序做纯字节拼接,
//     产出的位流与"单个写位器顺序写入"逐位一致(帧边界处写作不跨越字节)。
// 该函数是串行路径与并行路径共用的唯一实现, 保证两条路径结果完全相同。
// ---------------------------------------------------------------------------
size_t encodeFrameA4(BitWriterA4& bw, const int16_t* pcm, int blk, int bps,
                     int16_t blockBuf[2][kBlockSizeA4], BestSubA4* bestCh,
                     std::vector<int64_t>& scratch)
{
    size_t base = (size_t)blk * (size_t)kBlockSizeA4;
    for (int i = 0; i < kBlockSizeA4; ++i) {
        size_t idx = (base + (size_t)i) * 2u;
        blockBuf[0][i] = pcm[idx];
        blockBuf[1][i] = pcm[idx + 1u];
    }
    bw.alignToByte();
    size_t frameStart = bw.size();

    // 帧头: 14bit 同步(11111111111110) + 1bit blocking strategy(0=固定块大小)
    //       + 4bit 块大小码(12 = 4096) + 4bit 采样率码(10 = 48kHz)
    //       + 4bit 声道分配(1 = 独立立体声) + 3bit 位深码(4 = 16bit) + 1bit 保留
    bw.writeBits(0x3FFEu, 14);
    bw.writeBits(0u, 1);
    bw.writeBits(12u, 4);
    bw.writeBits(10u, 4);
    bw.writeBits(1u, 4);
    bw.writeBits(4u, 3);
    bw.writeBits(0u, 1);
    bw.writeUtf8((uint32_t)blk);
    bw.alignToByte();
    {
        size_t headerEnd = bw.size();
        std::vector<uint8_t>& buf = bw.mutBytes();
        uint8_t crc = crc8Flac(buf.data() + frameStart, headerEnd - frameStart);
        bw.writeBits((uint64_t)crc, 8);
    }

    for (int c = 0; c < 2; ++c) {
        chooseSubframeA4(blockBuf[c], kBlockSizeA4, bps, &bestCh[c]);
        writeSubframeA4(bw, blockBuf[c], kBlockSizeA4, bps, bestCh[c], scratch);
    }

    bw.alignToByte();
    bw.writeBits(0u, 16); // CRC-16 占位
    size_t frameEnd = bw.size();
    std::vector<uint8_t>& buf = bw.mutBytes();
    uint16_t crc16 = crc16Flac(buf.data() + frameStart, frameEnd - frameStart - 2);
    bw.setLastTwoBytes(crc16);
    return frameEnd - frameStart;
}

// ---------------------------------------------------------------------------
// 自校验用: 从位流中回读子帧(与编码端成对), 仅在线下自检时执行, 不计入计时
// ---------------------------------------------------------------------------
struct CheckCursorA4 {
    const uint8_t* buf;
    size_t len;      // 字节数
    size_t bitPos;

    bool readBits(int bits, uint64_t* out)
    {
        (void)out;
        if (bits < 0 || bits > 32) { return false; }
        if (bitPos + (size_t)bits > len * 8u) { return false; }
        uint64_t v = 0;
        for (int i = 0; i < bits; ++i) {
            size_t bp = bitPos + (size_t)i;
            v = (v << 1) | (uint64_t)((buf[bp >> 3] >> (7 - (int)(bp & 7u))) & 1u);
        }
        bitPos += (size_t)bits;
        return true;
    }
    bool readUnary(uint32_t* out, uint32_t limit)
    {
        uint32_t zeros = 0;
        for (;;) {
            uint64_t bit = 0;
            if (!readBits(1, &bit)) { return false; }
            if (bit != 0) { break; }
            ++zeros;
            if (zeros > limit) { return false; }
        }
        *out = zeros;
        return true;
    }
};

bool checkResidualA4(CheckCursorA4& cs, int n, int16_t* out)
{
    uint64_t po = 0;
    if (!cs.readBits(4, &po)) { return false; }
    int parts = 1 << (int)po;
    int partLen = n >> (int)po;
    if ((partLen << (int)po) != n || parts > 256) { return false; }
    int ks[256];
    for (int p = 0; p < parts; ++p) {
        uint64_t k = 0;
        if (!cs.readBits(4, &k)) { return false; }
        if (k >= 15u) { return false; } // escape 码在我们的编码器中不会出现
        ks[p] = (int)k;
    }
    int idx = 0;
    for (int p = 0; p < parts; ++p) {
        int k = ks[p];
        for (int i = 0; i < partLen; ++i) {
            uint32_t ms = 0;
            if (!cs.readUnary(&ms, 1u << 24)) { return false; }
            uint64_t rem = 0;
            if (k > 0 && !cs.readBits(k, &rem)) { return false; }
            int64_t mag = ((int64_t)ms << k) | (int64_t)rem;
            int64_t r = (mag & 1) ? -((mag + 1) >> 1) : (mag >> 1);
            if (r > 32767 || r < -32768) { return false; }
            out[idx++] = (int16_t)r;
        }
    }
    return true;
}

bool checkSubframeA4(CheckCursorA4& cs, int n, int bps, int16_t* out)
{
    uint64_t pad = 0;
    uint64_t type = 0;
    if (!cs.readBits(1, &pad)) { return false; }
    if (pad != 0) { return false; }
    if (!cs.readBits(6, &type)) { return false; }
    uint32_t wasted = 0;
    if (!cs.readUnary(&wasted, 64u)) { return false; }
    if (wasted != 0) { return false; } // 编码器不产生 wasted bits

    if (type == 0) {
        uint64_t v = 0;
        if (!cs.readBits(bps, &v)) { return false; }
        int16_t s = (int16_t)((int32_t)(v << (32 - bps)) >> (32 - bps));
        for (int i = 0; i < n; ++i) { out[i] = s; }
        return true;
    }
    if (type == 1) {
        for (int i = 0; i < n; ++i) {
            uint64_t v = 0;
            if (!cs.readBits(bps, &v)) { return false; }
            out[i] = (int16_t)((int32_t)(v << (32 - bps)) >> (32 - bps));
        }
        return true;
    }
    if (type >= 8 && type <= 12) {
        int order = (int)type - 8;
        if (order >= n) { return false; }
        for (int i = 0; i < order; ++i) {
            uint64_t v = 0;
            if (!cs.readBits(bps, &v)) { return false; }
            out[i] = (int16_t)((int32_t)(v << (32 - bps)) >> (32 - bps));
        }
        if (!checkResidualA4(cs, n - order, out + order)) { return false; }
        for (int i = order; i < n; ++i) {
            int64_t p = 0;
            switch (order) {
            case 0: p = 0; break;
            case 1: p = out[i - 1]; break;
            case 2: p = 2 * (int64_t)out[i - 1] - (int64_t)out[i - 2]; break;
            case 3: p = 3 * (int64_t)out[i - 1] - 3 * (int64_t)out[i - 2] + (int64_t)out[i - 3]; break;
            default:
                p = 4 * (int64_t)out[i - 1] - 6 * (int64_t)out[i - 2] + 4 * (int64_t)out[i - 3]
                    - (int64_t)out[i - 4];
                break;
            }
            int64_t s = (int64_t)out[i] + p;
            if (s > 32767 || s < -32768) { return false; }
            out[i] = (int16_t)s;
        }
        return true;
    }
    if (type >= 32) {
        int order = (int)type - 31;
        if (order < 1 || order > 32 || order >= n) { return false; }
        for (int i = 0; i < order; ++i) {
            uint64_t v = 0;
            if (!cs.readBits(bps, &v)) { return false; }
            out[i] = (int16_t)((int32_t)(v << (32 - bps)) >> (32 - bps));
        }
        uint64_t prec = 0;
        uint64_t sh = 0;
        if (!cs.readBits(4, &prec)) { return false; }
        if (!cs.readBits(5, &sh)) { return false; }
        int32_t coef[32];
        for (int i = 0; i < order; ++i) {
            uint64_t v = 0;
            if (!cs.readBits((int)prec + 1, &v)) { return false; }
            int bits = (int)prec + 1;
            coef[i] = (int32_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
        }
        if (!checkResidualA4(cs, n - order, out + order)) { return false; }
        for (int i = order; i < n; ++i) {
            int64_t acc = 0;
            for (int j = 0; j < order; ++j) { acc += (int64_t)coef[j] * (int64_t)out[i - 1 - j]; }
            if (sh > 0) { acc >>= (int)sh; }
            int64_t s = (int64_t)out[i] + acc;
            if (s > 32767 || s < -32768) { return false; }
            out[i] = (int16_t)s;
        }
        return true;
    }
    return false; // reserved(2..7) 不应出现
}

} // namespace

// ---------------------------------------------------------------------------
// Audio Encoder
// ---------------------------------------------------------------------------
Gb7Outcome gb7RunAudioEncoder(int threads)
{
    Gb7Outcome o;
    o.name = "Audio Encoder";
    o.section = "Media";

    // ======================== 工作量标定(首次真机复核后) ========================
    // metric 口径(明确写死): MB/s = (输入 PCM 原始字节数 / 1e6) / 秒数, 其中原始字节数
    //   = 采样率 x 时长 x 声道数 x 2 字节。压缩率只进日志, 不进 metric。
    // 调整前: 48000 Hz / 16bit / 立体声 / 60 s = 11,520,000 B。实测 0.10 MB/s
    //   -> 11.52 MB / 0.10 ≈ 115 s(远超 1.5~3.0 s 上限; 折算约 0.246 s 每 4096 样本块,
    //   因为逐块要在 [固定预测器 0..4] 与 [LPC 1..12] 之间试算并按 Rice 分区估比特)。
    // 关键换算: 每块(4096 样本 x 2 声道)成本固定 —— 每块都要在 [固定预测器 0..4] 与
    //   [LPC 1..12] 之间试算, 并按 partition order 0..8 估 Rice 比特数, 与块数无关。
    //   因此"每块耗时" ≈ 115 s / 703 块 ≈ 0.164 s/块, 总耗时 ∝ 样本数。
    // 调整: 时长 60 s -> 1 s。实际进入编码循环的样本 = 11 个整块 x 4096 = 45,056 样本
    //   (48,000 中的末尾 2,944 样本按原实现的 blocks = totalFrames / kBlockSizeA4 不入循环,
    //    与调整前的截断口径一致)。
    //   -> 预计耗时 = 115 s x (45,056 / 2,880,000) ≈ 1.80 s, 落在 1.5~3.0 s 区间内。
    //   -> 预计吞吐 = (45,056 x 2 x 2 B / 1e6) / 1.80 s ≈ 0.100 MB/s —— 与调整前的
    //      0.10 MB/s 相同(每字节成本没变, 只是总字节少了), 这也说明 MB/s 型指标
    //      本来就与数据量无关, 调数据量只影响耗时、不影响这个比值。
    // 说明: 采样率/位深/声道数都没改(它们只改数据量, 不改每字节成本); 本项未锚定,
    //   只做耗时对齐, 不编造锚点。
    //   位流自检(逐帧重解析 CRC-8/CRC-16)在计时区间外, 不进入 o.ms。
    const int sampleRate = 48000;
    const int channels = 2;
    const int bps = 16;
    // 编码样本数 = 本项唯一的线性耗时旋钮(metric = 输入 PCM 字节/1e6/秒, 与样本数无关)。
    // 本轮真机实测(CS1 单核阶段第 12 项, runlog.jsonl run 1791098115489-82335):
    //   totalFrames = 48,000(1 s) -> blocks = 11, o.ms = **1261.1 ms**,
    //   runlog metric 打印 "0.2"(真值 = 192,000 B / 1e6 / 1.2611 s = 0.1523, %.1f -> "0.2", 自洽)。
    //   单块成本 = 1261.1 / 11 = 114.6 ms/块(每块都要在 [固定预测器 0..4] 与 [LPC 1..12]
    //   之间试算, 并按 partition order 0..8 估 Rice 比特数, 与"第几块"无关)。
    //   21 个整块(88,000 样本 = 1.833 s 音频)x 114.6 ms = **2408 ms**, 落在 1.5~3.0 s 中部。
    //   加的是真实的音频样本: 更多块 = 更多帧的 LPC 定阶 / Rice 分区搜索 / 位流写出,
    //   不是把同一块多编码几遍、也不是加空循环。
    //   吞吐不变: MB/s = 输入字节 / 1e6 / 秒 -> 352,000 B / 2.408 s ≈ 0.146 MB/s(与 1 s 版同量级)。
    const int totalFrames = 88000;                  // 21 个 4096 样本整块(88000/4096 = 21)
    const int blocks = totalFrames / kBlockSizeA4;  // 21 块(4096 样本/块)
    const size_t inputBytes = (size_t)totalFrames * (size_t)channels * sizeof(int16_t); // 352,000

    std::vector<int16_t> pcm;
    generateMusicA4(pcm, sampleRate, totalFrames, channels);
    if (pcm.size() != (size_t)totalFrames * (size_t)channels) {
        pcm.assign((size_t)totalFrames * (size_t)channels, 0);
    }

    BitWriterA4 bw(inputBytes);
    std::vector<int64_t> scratch((size_t)kBlockSizeA4);
    BestSubA4 bestCh[2];
    int16_t blockBuf[2][kBlockSizeA4];

    if (threads < 1) { threads = 1; }

    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsA4();
    std::clock_t cpu0 = std::clock();

    // ---- 流标记 + STREAMINFO ----
    static const uint8_t kMagic[4] = {0x66u, 0x4Cu, 0x61u, 0x43u}; // "fLaC"
    bw.writeRawBytes(kMagic, 4);
    {
        uint8_t si[38];
        memset(si, 0, sizeof(si));
        si[0] = 0x00; // last-metadata-block = 1, type = 0 (STREAMINFO)
        si[3] = 34;   // 元数据长度
        si[4] = (uint8_t)(kBlockSizeA4 >> 8);
        si[5] = (uint8_t)(kBlockSizeA4 & 0xFF);
        si[6] = (uint8_t)(kBlockSizeA4 >> 8);
        si[7] = (uint8_t)(kBlockSizeA4 & 0xFF);
        // min/max frame size 与 MD5 置 0 = unknown(规范允许)
        uint64_t packed = 0;
        packed |= (((uint64_t)sampleRate & 0xFFFFFu) << 44);
        packed |= (((uint64_t)(channels - 1) & 0x7u) << 41);
        packed |= (((uint64_t)(bps - 1) & 0x1Fu) << 36);
        packed |= ((uint64_t)totalFrames & 0xFFFFFFFFFull);
        for (int i = 0; i < 8; ++i) {
            si[8 + i] = (uint8_t)((packed >> (56 - 8 * i)) & 0xFFu);
        }
        bw.writeRawBytes(si, sizeof(si));
    }

    // ---- 逐帧编码 ----
    // 数据分解: 帧是自包含而且字节对齐的独立单元(见 encodeFrameA4 注释), 因此把
    // 帧区间分给 gb7ParallelFor, 每个工作单元把自己的若干帧写进私有位流缓冲,
    // 全部完成后按帧序做字节拼接。工作量不变(每帧仍然只编码一次), 每帧的
    // LPC 系数 / Rice 参数 / CRC 都在自己所属的工作单元内独立计算, 无共享写。
    size_t frameSizeSum = 0;
    if (threads <= 1) {
        // 串行路径: 与并发之前完全一致的执行顺序与结果
        for (int blk = 0; blk < blocks; ++blk) {
            frameSizeSum += encodeFrameA4(bw, pcm.data(), blk, bps, blockBuf, bestCh, scratch);
        }
    } else {
        // 每帧一个独立缓冲: 帧号本身就是拼接顺序, 因此既不需要排序也不需要锁.
        // 一个工作单元只处理 [start,end) 这个帧区间, 而每个帧只属于一个区间 ->
        // 任何两个工作单元写的 frameBytes 下标都不相交, 不存在共享写.
        // (该写法不依赖 executor 的分块粒度: 无论块大小是 1 还是 64 都成立.)
        std::vector<std::vector<uint8_t> > frameBytes((size_t)blocks);
        gb7ParallelFor(threads, (long long)blocks, [&](long long start, long long end) {
            // 工作单元私有的写位器与临时缓冲(不跨线程共享)
            BitWriterA4 fw(1u << 16);
            int16_t cbuf[2][kBlockSizeA4];
            std::vector<int64_t> cscratch((size_t)kBlockSizeA4);
            BestSubA4 cbest[2];
            for (long long blk = start; blk < end; ++blk) {
                encodeFrameA4(fw, pcm.data(), (int)blk, bps, cbuf, cbest, cscratch);
                std::vector<uint8_t>& fb = frameBytes[(size_t)blk];
                fb.assign(fw.bytes().begin(), fw.bytes().end());
                fw.reset();     // 复用写位器(缓冲容量保留, 不重复分配)
            }
        });
        // 按帧序拼接(每帧都从字节边界开始, 因此纯字节拼接与顺序写入逐位等价)
        for (size_t c = 0; c < (size_t)blocks; ++c) {
            std::vector<uint8_t>& fb = frameBytes[c];
            frameSizeSum += fb.size() * 8u;     // 与串行路径口径一致(比特)
            bw.mutBytes().insert(bw.mutBytes().end(), fb.begin(), fb.end());
            std::vector<uint8_t>().swap(fb);    // 及时释放, 降低峰值内存
        }
    }

    double t1 = nowMsA4();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)

    // ---- 位流自检: 逐帧重新解析(CRC-8 / 子帧 / CRC-16), 不计入计时 ----
    int framesChecked = 0;
    int framesBad = 0;
    {
        const std::vector<uint8_t>& buf = bw.bytes();
        size_t pos = 42; // 4 ("fLaC") + 4 (元数据头) + 34 (STREAMINFO)
        while (pos + 2 < buf.size()) {
            if (!(buf[pos] == 0xFFu && (buf[pos + 1] & 0xFEu) == 0xF8u)) { ++framesBad; break; }
            CheckCursorA4 cs;
            cs.buf = buf.data();
            cs.len = buf.size();
            cs.bitPos = pos * 8u + 14u;
            uint64_t v = 0;
            bool ok = cs.readBits(1, &v) && v == 0;          // blocking strategy
            ok = ok && cs.readBits(4, &v) && v == 12;        // 4096
            ok = ok && cs.readBits(4, &v) && v == 10;        // 48kHz
            ok = ok && cs.readBits(4, &v) && v == 1;         // 独立立体声
            ok = ok && cs.readBits(3, &v) && v == 4;         // 16bit
            ok = ok && cs.readBits(1, &v) && v == 0;         // reserved
            uint64_t b0 = 0;
            ok = ok && cs.readBits(8, &b0);
            if (ok) {                                        // UTF-8 帧号
                int extra = 0;
                if ((b0 & 0x80u) == 0) { extra = 0; }
                else if ((b0 & 0xE0u) == 0xC0u) { extra = 1; }
                else if ((b0 & 0xF0u) == 0xE0u) { extra = 2; }
                else if ((b0 & 0xF8u) == 0xF0u) { extra = 3; }
                else if ((b0 & 0xFCu) == 0xF8u) { extra = 4; }
                else { extra = 5; }
                for (int i = 0; i < extra && ok; ++i) { ok = cs.readBits(8, &v); }
            }
            if (!ok || (cs.bitPos & 7u) != 0u) { ++framesBad; break; }
            size_t hdrEnd = cs.bitPos >> 3;
            if (hdrEnd >= buf.size()) { ++framesBad; break; }
            if (crc8Flac(buf.data() + pos, hdrEnd - pos) != buf[hdrEnd]) { ++framesBad; break; }
            cs.bitPos = (hdrEnd + 1) * 8u;

            int16_t scratchSamples[kBlockSizeA4];
            for (int c = 0; c < 2 && ok; ++c) {
                ok = checkSubframeA4(cs, kBlockSizeA4, bps, scratchSamples);
            }
            if (!ok) { ++framesBad; break; }
            cs.bitPos = (cs.bitPos + 7u) & ~(size_t)7u;
            if (cs.bitPos + 16u > buf.size() * 8u) { ++framesBad; break; }
            size_t frameEnd = (cs.bitPos >> 3);
            uint16_t want = crc16Flac(buf.data() + pos, frameEnd - pos);
            uint16_t got = (uint16_t)(((uint16_t)buf[frameEnd] << 8) | (uint16_t)buf[frameEnd + 1]);
            if (want != got) { ++framesBad; break; }
            pos = frameEnd + 2;
            ++framesChecked;
        }
    }

    // 对输出位流做一次 CRC 累加(防止编译器消除编码结果)
    uint64_t crcAcc = 0;
    {
        const std::vector<uint8_t>& buf = bw.bytes();
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < buf.size(); ++i) {
            h ^= (uint32_t)buf[i];
            h *= 16777619u;
        }
        crcAcc = (uint64_t)h;
    }
    volatile uint64_t sink = crcAcc;
    (void)sink;

    // 真实并行度: 参与线程的 CPU 时间之和 / 墙钟时间(std::clock 统计整个进程)
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    o.parallelism = gb7Parallelism(cpuMs, t1 - t0);

    double secondsElapsed = (t1 - t0) / 1000.0;
    if (!(secondsElapsed > 1e-6)) { secondsElapsed = 1e-6; }
    double mbPerSec = ((double)inputBytes / 1000000.0) / secondsElapsed;
    if (!(mbPerSec == mbPerSec) || mbPerSec < 0.0) { mbPerSec = 0.0; }
    if (mbPerSec > 1e7) { mbPerSec = 1e7; }

    // 压缩率只用于日志, 不进入 metric
    double ratio = (double)bw.size() / (double)inputBytes;
    char logbuf[224];
    snprintf(logbuf, sizeof(logbuf),
             "[gb7] Audio Encoder: %d frames ok, %d bad, blocks=%d, in=%u B, out=%u B, ratio=%.4f, %.2f MB/s, crc=0x%08X",
             framesChecked, framesBad, blocks, (unsigned)inputBytes, (unsigned)bw.size(), ratio, mbPerSec,
             (unsigned)(crcAcc & 0xFFFFFFFFu));
    fputs(logbuf, stderr);
    fputc('\n', stderr);
    (void)frameSizeSum;

    char buf32[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf32, sizeof(buf32), "%.4g", mbPerSec);
    o.ms = t1 - t0;
    o.metric = buf32;
    o.unit = "MB/s";
    return o;
}
