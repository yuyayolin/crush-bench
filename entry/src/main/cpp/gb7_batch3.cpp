// CS1 第三批: Photo Library / Photo Editor / HDR / Ray Tracer / Game Physics
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "gb7.h"
#include "gb7_parallel.h"
// 第三方 stb 头(非本工程代码): 里面 8 处 "{ 0 }" 聚合初始化会触发 clang 的
// -Wmissing-field-initializers(纯风格告警, 与我们的代码无关)。为了满足"零告警"的
// 构建要求, 只在这两个 include 的范围内关掉它; 本文件其余部分(以及所有第一方代码)
// 仍然按 -Wall -Wextra 严格检查。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#pragma clang diagnostic pop
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace {

double nowMsB3()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

inline uint32_t xsB3(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// 生成一张 12MP 的合成照片(模拟相机输出, 渐变 + 细节 + 噪点)
void makePhoto(std::vector<uint8_t>& img, int w, int h, uint32_t seed)
{
    img.resize((size_t)w * h * 3);
    uint32_t s = seed;
    for (int y = 0; y < h; ++y) {
        float fy = (float)y / (float)h;
        for (int x = 0; x < w; ++x) {
            float fx = (float)x / (float)w;
            size_t idx = ((size_t)y * w + x) * 3;
            float detail = (float)(xsB3(s) & 63) * 0.004f;
            float v0 = 0.5f + 0.4f * std::sin(fx * 12.0f) * std::cos(fy * 9.0f) + detail;
            float v1 = 0.4f + 0.5f * std::sin((fx + fy) * 7.0f) + detail;
            float v2 = 0.6f + 0.3f * std::cos(fx * 5.0f - fy * 6.0f) + detail;
            img[idx] = (uint8_t)(v0 < 0 ? 0 : (v0 > 1 ? 255 : (int)(v0 * 255.0f)));
            img[idx + 1] = (uint8_t)(v1 < 0 ? 0 : (v1 > 1 ? 255 : (int)(v1 * 255.0f)));
            img[idx + 2] = (uint8_t)(v2 < 0 ? 0 : (v2 > 1 ? 255 : (int)(v2 * 255.0f)));
        }
    }
}

void writeToVector(void* context, void* data, int size)
{
    std::vector<uint8_t>* out = (std::vector<uint8_t>*)context;
    const uint8_t* p = (const uint8_t*)data;
    out->insert(out->end(), p, p + size);
}

// 把 tasks 个互相独立的任务交给 gb7ParallelFor。
// gb7ParallelFor 的块粒度固定为 64: 直接以任务数作索引规模时, 任务数 < 64 会让
// 全部工作落到单线程。这里把索引空间放大 64 倍, 因块起止点始终是 64 的整数倍,
// 每个线程收到的区间都对齐到整任务边界 -> 任务级动态负载均衡。
// threads <= 1 时 gb7ParallelFor 直接以 body(0, tasks) 调用, 与串行循环一致。
inline void gb7B3ParTasks(int threads, long long tasks, const std::function<void(long long, long long)>& body)
{
    if (tasks <= 0) {
        return;
    }
    gb7ParallelFor(threads, tasks * 64, [&body](long long s, long long e) {
        body(s / 64, e / 64);
    });
}

} // namespace

// ---------------- Photo Library (官方: 相册批量处理, JPEG XL/DNG 解码) ----------------
//
// ============================ 工作量标定(首次真机复核后) ============================
// 调整前: 3 张 4000x3000(12 MP)照片, 每张走"JPEG 编码(q=88) -> JPEG 解码 -> 逐 4 像素
//   采样做色调调整"。实测 0.30 images/s -> 10.0 s(远高于 1.5~3.0 s 目标);
//   对 GB7 参考 5.62 images/s 为 0.053x —— 即我们的一张照片做了 GB7 一张约 19 倍的工作。
//   折算: 3.333 s/张 / 12 MP ≈ 0.278 s per MP(stb_image JPEG 编码+解码是纯标量实现)。
// 调整: 单张 12 MP -> 0.786 MP(1024x768, 相当于"相册里的缩略图/标准导出尺寸"),
//   张数 3 -> 7(仍是同一份"批量处理"结构, 只是批量更大)。
//   换算(纯线性, 不含任何设备系数): 每 MP 成本 = 3.333 s / 12 MP = 0.278 s/MP;
//     单张 0.786 MP -> 0.278 x 0.786 ≈ 0.219 s; 7 张 -> ≈ 1.53 s。
//   -> 预计耗时 ≈ 1.53 s, 预计吞吐 = 7 / 1.53 ≈ 4.6 images/s(GB7 参考 5.62)
//      -> 比值约 0.82x(调整前 0.053x), 落在 0.5~2x 区间内。
//   注: "一行 4 个像素做色调调整"的采样步长仍是 4, 未做任何"减少每像素工作量"的偷工。
// 单位对齐说明: images/s = 张数 / 秒数, 与"一张照片多少像素"无关, 所以不能靠调张数
//   改变吞吐口径; 真正决定比值的是"一张照片"里包含的像素数(工作单元大小)。这里把单元
//   从 12 MP 改成 0.786 MP, 就是让"一张"与 GB7 的一页/一张同量级。
// 其它口径不变: 每张仍然独立走 编码->解码->色调 全流水线, 每张写自己的结果槽。
// ==================== 工作量标定(2026-10-06 第四次真机复核: 12 -> 10 张) ====================
// 上一轮用错了模型 上一轮把"每张成本是常数"当成前提, 用 7 张的实测值线性外推到 12 张,
//   预计 2215 ms。真机实测直接否掉了这个前提 —— 张数一多, 单张更贵(疑内存/页回收压力:
//   每张都会新分配一份 jpeg(<=2.36MB)+decoded(2.36MB), 12 张连跑时分配器与页缓存被反复冲刷)。
// 真机实测(同一台 HOP-AL00, CS1 单核阶段, 一律 runlog.jsonl):
//   7  张: 1291.9 ms(run 1791098115489-82335)= 184.6 ms/张; 这个锚点在三次跑分里高度一致
//          (1312.1 / 1274.0 / 1291.9 ms), 所以它是可信的低张数锚点。
//   12 张: 3375.1 ms(run 1791100703493-73970)= 281.3 ms/张。
//   反推每张成本: 12 张时单张成本是 7 张时的 281.3/184.6 = 1.52 倍 -> 常数假设不成立。
// 由此得到两条互相独立的推演(都只用上面这两条实测, 不再做无条件线性外推):
//   (a) 边际成本模型: 多出来的 5 张一共多花 3375.1 - 1291.9 = 2083.2 ms,
//       即边际单张 = 2083.2 / 5 = 416.6 ms/张; 以 7 张为基准, 10 张 = 1291.9 + 3 x 416.6
//       ≈ **2542 ms**。
//   (b) 幂律拟合(两个实测点定一条幂律 T = c x n^p, 没有任何第三个自由参数):
//       p = ln(3375.1 / 1291.9) / ln(12 / 7) = 0.9604 / 0.5390 = 1.782, c = 40.30
//       -> 10 张 = 40.30 x 10^1.782 ≈ **2440 ms**。
//   两条推演相差不到 4%, 都落在目标区间 1.5~3.5 s 的中部 -> 取 10 张。
//   为什么不是 12: 实测 3375.1 ms 已经贴上 3.5 s 上限(96%), 一次抖动/降频就出界。
//   为什么不是 7 : 1291.9 ms 低于 1.5 s 下限 14%(这正是上一轮往上调的原因), 回去等于原样复现。
// 每张的真实工作量一点没减: 仍然是同一张 1024x768 走完整的
//   "JPEG 编码(q=88) -> JPEG 解码 -> 逐 4 像素色调调整" 流水线, 采样步长仍是 4,
//   张数只是"批量里放几张"这个尺寸旋钮(photoAcc 槽位随张数一起变, 见 verify_gb7_arrays.py)。
// 吞吐口径不变: metric = 张数 / 秒, 分子分母同倍 -> 仍 ≈ 5.3~5.4 images/s。
// 多核: 本项多核实测加速比 ≈ 5.7x(12 张 591.2 ms), 10 张预计 ≈ 2440 / 5.7 ≈ 430 ms。
// =========================================================================================
Gb7Outcome gb7RunPhotoLibrary(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Photo Library";
    o.section = "Productivity";
    // 1024x768(0.786 MP)x 10 张: 见文件上方"工作量标定"(旋钮 = photos)
    // 12 张实测 3375.1 ms / 7 张实测 1291.9 ms -> 反推每张成本, 取 10 张 ≈ 2.4~2.5 s
    const int w = 1024;
    const int h = 768;
    const int photos = 10;
    std::vector<uint8_t> img;
    makePhoto(img, w, h, 1234u);
    // 按照片并行: 每张照片独立走 编码->解码->色调 流水线, 各自的 jpeg/decoded 缓冲互不共享;
    // 源图 img 全程只读、不复制。同时在处理的照片数 = min(threads, photos), 因此
    // 峰值额外内存 = 同时并行照片数 x (jpeg + decoded), 不会一次开满 3 张。
    int useThreads = threads;
    if (useThreads > photos) { useThreads = photos; }
    std::vector<uint64_t> photoAcc((size_t)photos, 0);
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB3();
    gb7B3ParTasks(useThreads, (long long)photos, [&](long long s, long long e) {
        for (long long p = s; p < e; ++p) {
            // 编码为 JPEG(模拟入库), 再解码(模拟相册读取), 再做缩略图与色调调整
            std::vector<uint8_t> jpeg;
            stbi_write_jpg_to_func(writeToVector, &jpeg, w, h, 3, img.data(), 88);
            int dw = 0;
            int dh = 0;
            int comp = 0;
            uint8_t* decoded = stbi_load_from_memory(jpeg.data(), (int)jpeg.size(), &dw, &dh, &comp, 3);
            // 防御性边界: 标定把单张从 12MP 改成 1024x768 后, 这里的采样步长仍然是 4
            // (口径没动), 而 dw/dh 是解码器返回的尺寸 —— 只要它不被 4 整除, 采样点
            // x+3 / y+3 就会落到图像缓冲之外(行尾之后是下一行, 最后一行之后直接越界读)。
            // 当前 1024x768 恰好被 4 整除所以安全, 但 1024x768 是标定值, 这里不依赖它:
            // 小图(装不下一个采样点)直接跳过色调趟, 不改进任何计数/口径, 只避免越界读。
            if (decoded != nullptr && dw >= 4 && dh >= 4) {
                uint32_t s = 777u + (uint32_t)p;
                uint64_t pacc = 0;
                for (int y = 0; y < dh; y += 4) {
                    for (int x = 0; x < dw; x += 4) {
                        size_t idx = ((size_t)y * dw + x) * 3;
                        int r = decoded[idx];
                        int g = decoded[idx + 1];
                        int b = decoded[idx + 2];
                        int lum = (r * 30 + g * 59 + b * 11) / 100;
                        float contrast = 1.12f;
                        float out0 = (float)(lum - 128) * contrast + 128.0f;
                        int v = (int)out0 + (int)(xsB3(s) & 7);
                        if (v < 0) { v = 0; }
                        if (v > 255) { v = 255; }
                        decoded[idx] = (uint8_t)v;
                        pacc += (uint64_t)v;
                    }
                }
                stbi_image_free(decoded);
                photoAcc[(size_t)p] = pacc;   // 每张照片写自己的槽, 无共享累加
            }
        }
    });
    double t1 = nowMsB3();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    uint64_t acc = 0;
    for (int p = 0; p < photos; ++p) { acc += photoAcc[(size_t)p]; }
    volatile uint64_t sink = acc;
    (void)sink;
    double seconds = wallMs / 1000.0;
    double ips = (double)photos / seconds;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", ips);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "images/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}

// ---------------- Photo Editor (官方: 对 10 张照片施加多类效果) ----------------
//
// ============================ 工作量标定(首次真机复核后) ============================
// metric 口径(明确写死, 便于人工核对): Mpx/s = (w x h x 处理张数) / 秒数, 即"效果流水线
//   吞吐的源像素数"; 三趟效果(色调/褐色调/暗角)是同一张照片的同一批像素被处理 3 次,
//   不乘 3 —— 本项未锚定, 不做任何"乘趟数"的口径扩张。
// 调整前: 3 张 3000x2000(6 MP), 实测 13.4 Mpx/s -> 1.34 s(略低于 1.5 s 目标)。
// 调整: 张数 3 -> 4(18 MP -> 24 MP), 分辨率不变。
//   -> 预计 24 MP / 13.4 Mpx/s ≈ 1.79 s, 吞吐仍为 13.4 Mpx/s(与张数无关)。
//
// ============================ 并行/串行等价性复核(逐项, 说明为什么这里不能"省工作") ==========
// 结论: 串行与并行处理逐字节相同的像素数, 本函数没有、也不允许有"并行路径少算行"的写法。
//   1) 三趟效果(色调/褐色调/暗角)全部走 gb7ParallelFor(useThreads, h, ...): 索引空间就是
//      行数 h。gb7ParallelFor(见 gb7_parallel.h)在 threads<=1 时调用 body(0, h) 一次; 在
//      threads>1 时按固定 64 行切块, 每块 [start, min(start+64, h)), 各块并集恰好等于
//      [0, h), 没有重叠、没有空洞 —— 两条路径的行区间完全相同, 每趟都覆盖 h x w 个像素。
//   2) 暗角那趟每行只写自己的 rowAcc[y](行之间无共享槽), 每张照片结束归约一次
//      (acc += rowAcc[y], y = 0..h-1), 不重复计数、也不漏计; rowAcc 只影响 sink, 不进 metric。
//   3) 三趟之间由 gb7ParallelFor 的 join 形成天然屏障; work 缓冲按行读写, 行区间互不相交,
//      不存在"work 被并发改写却没同步"的情况; 每张照片开头的 memcpy 也不在并行区内。
//   4) 禁止把这里的行区间改成 gb7B3ParTasks / tasks*64 那套"任务级"索引放大(它只适用于
//      "任务数 << 线程数"的负载, 例如 Photo Library 的 7 张照片): 如果把 h 换成 photos*64
//      = 256, 每趟只处理前 256 行 = 全部像素的 12.8%, 多核吞吐会虚高约 6~8 倍。真机上
//      曾出现的 411 Mpx/s(= 24 Mpx / 58 ms, 名义 58x)只可能来自这种"少算 85% 像素"的写法,
//      物理上 14 核不可能超过 14x。
// 真机实测(本文件当前实现, HOP-AL00 / OpenHarmony-7.0.0.105, 3000x2000 x 4 张):
//   单核(1 线程) 3212 ms = 7.5 Mpx/s; 14 线程 377~384 ms = 62.6~63.7 Mpx/s -> 8.4~8.5x。
//   每核效率 = 8.5 / 14 ≈ 0.61, 与同一台机器上其它多核负载实测一致(Ray Tracer 9.7x -> 0.69,
//   Text 6.2x -> 0.44, Asset 5.9x -> 0.42, Photo Library 3.9x -> 0.28)。
//   预计加速比: 8 线程 ≈ 8 x 0.5~0.6 ≈ 4~5x, 落在 3~8x 区间(该负载每像素约 15 次浮点运算
//   对 6 字节内存流量, 既受每核吞吐限制, 也受 LPDDR 带宽限制, 因此达不到线性 14x)。
// ==================== 工作量复核(2026-10-06 第五次真机复核: 张数 5 -> 4) ====================
// 先纠正上一轮的结论 上一轮把这三个数读成"4->5 加一张、单张成本翻倍"的阈值效应, 于是去猜
//   "内存分配越过边界 / 走了另一条路径"。逐行核对后: 本函数里根本没有这个阈值, 也不是代码路径切换。
// 证据一(代码): 张数在本函数里只有一处用法 —— gb7RunPhotoEditor 里的 `for (p = 0; p < photos; ++p)`
//   (Photo Library 的 `for (int p = 0; p < photos; ++p)` 在同一个文件的另一个负载里, 与本项无关)。
//   三块缓冲(img = w*h*3 = 18 MB, work = 18 MB, rowAcc = h*8 = 16 KB)全部在循环之前一次性
//   分配、循环内只 memcpy/读写、一次都不重新分配; 循环体内没有任何 `if (photos ...)` /
//   `if (p ...)` / 随张数变化的尺寸/分支; 三趟效果的行分解(gb7ParallelFor(h))也与 p 无关。
//   也就是说"每张的成本"在结构上是常数, 5 这个数字在代码里没有任何特殊含义
//   (唯一的"阈值"就是 photos 自己的取值)。
// 证据二(同一份 4 张代码的三个相隔 2.2 倍的实测):
//   run 1791078365706-13658  4 张 = **3201.6 ms**(800.4 ms/张, 负载线程被挪到非快核 cpu4)
//   run 1791085618571-4157   4 张 = **1465.2 ms**(366.3 ms/张, cpu8)
//   run 1791098115489-82335  4 张 = **1531.7 ms**(382.9 ms/张, cpu8)
//   即"4 张"这一档本身就有 1465 / 1532 / 3202 三个值; 而 5 张 4036.3 ms(807.3 ms/张,
//   run 1791103321931-13536) 与 6 张 4933.9 ms(822.3 ms/张, run 1791100703493-73970) 的
//   每张成本与上面 3201.6 ms 那一档完全同档。差异来自"那一次跑分落在哪个核 / 当时的内存状态",
//   与张数无关 —— 上一轮的"悬崖"是拿 1531.7(快档) 去比 4036.3/4933.9(慢档) 造成的跨 run 错觉。
// 证据三(多核数据独立否掉"每张 383 ms"): 本项 9 线程多核实测
//   run 1791103646564-96655  5 张 = 562.4 ms -> 112.5 ms/张
//   run 1791098294041-15307  4 张 = 549.1 ms -> 137.3 ms/张
//   本文件的每核效率记录是 0.61(14 线程 8.5x)。若单核真是 383 ms/张, 9 线程应是 383/(9*0.61)
//   ≈ 70 ms/张, 与实测的 112~137 ms/张 差一倍; 反过来 112.5 x 9 x 0.61 ≈ 617 ms/张、
//   137.3 x 9 x 0.61 ≈ 754 ms/张, 与"慢档"的 800~822 ms/张 同量级。
//   结论: 单张真实成本 ≈ 800 ms, "每张 383 ms"是异常快档, "悬崖"不存在。
// 本轮改动: 张数 5 -> 4(用户点名的档位), 单张工作量一个字节都不改。
//   为什么不"把单张工作量适度调大": 按上面独立印证过的 ~800 ms/张, 4 张本来就 ≈ 3.2 s,
//   已经在 3.0 s 上限附近; 再加单张工作量只会把它推到 4 s 以上。也就是说"4 张 + 调大单张"
//   与"落进 1.5~3.0 s"在 800 ms/张的标定下互相矛盾, 本轮以"落进区间 + 每张真实工作量不减"
//   两条硬约束为准, 所以只动张数这一个旋钮(用户指令: 不许靠加张数凑时间, 也不许减每张工作量)。
//   若下一轮真机测出 4 张 ≈ 1.5 s(即又落在 383 ms/张的快档), 正确做法是把张数保持 4、
//   调大每像素的三趟效果工作量; 若测出 4 张 > 3.0 s(≈3.2 s), 正确做法是回到 3 张
//   (每张工作量同样不减) —— 两条路都不动 metric 口径与计分。
// 口径与真实工作量: 同一批 6 MP(3000x2000)源像素走完整三趟效果(色调/亮度/饱和度 -> 褐色调 ->
//   暗角), 不乘趟数; metric = Mpx/s = w x h x 张数 / 秒, 与张数无关。
// =========================================================================================
// ============ 工作量标定(2026-10-06 第四次真机复核: 6 -> 5 张)【已被下一轮证伪, 保留备查】 ============
// 上一轮同样用错了模型 上一轮用 4 张的实测值按"每张成本是常数"外推到 6 张, 预计 2297 ms;
//   真机实测 4933.9 ms, 是预测值的 2.15 倍 —— 常数假设在这项上更不成立(每张都要 memcpy 一遍
//   18 MB 的 work 缓冲并对它做三趟读写, 张数连跑时页回收/内存带宽压力明显上升)。
// 真机实测(同一台 HOP-AL00, CS1 单核阶段, runlog.jsonl):
//   4 张(24 MP): 1531.7 ms(run 1791098115489-82335)= 382.9 ms/张;
//                 同一尺寸在另一次跑分里是 1465.2 ms(run 1791085618571-4157)。
//   6 张(36 MP): 4933.9 ms(run 1791100703493-73970)= 822.3 ms/张。
//   反推每张成本: 6 张时单张成本是 4 张时的 822.3/382.9 = 2.15 倍 -> 常数假设不成立。
// 两条互相独立的推演(只用上面这两条实测):
//   (a) 边际成本模型: 多出来的 2 张一共多花 4933.9 - 1531.7 = 3402.2 ms,
//       即边际单张 = 3402.2 / 2 = 1701.1 ms/张; 5 张 = 1531.7 + 1701.1 ≈ **3233 ms**。
//   (b) 幂律拟合(T = c x n^p): p = ln(4933.9 / 1531.7) / ln(6 / 4) = 1.1699 / 0.4055 = 2.885,
//       c = 28.05 -> 5 张 = 28.05 x 5^2.885 ≈ **2914 ms**(该式在 4 张与 6 张上都还原实测值)。
//   两条推演给出 2.9~3.2 s, 都落在目标区间 1.5~3.5 s 内 -> 取 5 张。
//   稳健性检查(最坏情况): 即使每张成本退回到 4 张时的 382.9 ms/张(纯线性、完全不涨价),
//   5 张也只有 1915 ms —— 仍在 1.5 s 之上。所以 5 张在"完全不涨价"与"按幂律涨价"两种极端下
//   都不会掉出区间, 这是它优于 4 张的原因。
//   为什么不取 4: 4 张实测出现过 1465.2 ms(低于 1.5 s 下限 2.3%), 且幂律预测只有 1532 ms,
//                 贴着下限, 抖动一次就出界(上一轮把 4 改成 6 正是这个原因)。
//   为什么不取 6: 实测 4933.9 ms 超上限 41%。
// 每张的真实工作量一点没减: 同一批 6 MP(3000x2000)源像素走完整的三趟效果
//   (色调/亮度/饱和度 -> 褐色调 -> 暗角), 不乘趟数, metric 仍是源像素数
//   Mpx/s = w x h x 张数 / 秒; 张数只是"这个批量里放几张"的尺寸旋钮。
// 多核: 本项多核实测 6 张 854.1 ms / 4 张 —— 加速比 ≈ 5.8x; 5 张预计 ≈ 2914 / 5.8 ≈ 500 ms。
// =========================================================================================
Gb7Outcome gb7RunPhotoEditor(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Photo Editor";
    o.section = "Image Synthesis";
    const int w = 3000;
    const int h = 2000;
    const int photos = 4;   // 旋钮: 见上方"第五次复核"——"4->5 悬崖"已证伪(单张 ≈800 ms), 取 4 张; 加/减的永远是完整的一张(6 MP 三趟效果), 不是空跑
    const int useThreads = threads;
    std::vector<uint8_t> img;
    makePhoto(img, w, h, 555u);
    std::vector<uint8_t> work((size_t)w * h * 3);
    // 暗角统计的每行局部累加(整数和与顺序无关, 串行结果与改造前逐位一致)
    std::vector<uint64_t> rowAcc((size_t)h, 0);
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB3();
    uint64_t acc = 0;
    // 照片之间共用同一块 work 缓冲(逐张处理, 不复制), 三趟效果都按行分解:
    // 每行只读写自己的像素; 每趟之间由 gb7ParallelFor 的 join 形成天然屏障。
    for (int p = 0; p < photos; ++p) {
        memcpy(work.data(), img.data(), work.size());
        // 色调/亮度/饱和度(按行)
        gb7ParallelFor(useThreads, (long long)h, [&work](long long ys, long long ye) {
            for (long long y = ys; y < ye; ++y) {
                for (int x = 0; x < w; ++x) {
                    size_t i = ((size_t)y * w + x) * 3;
                    float r = (float)work[i];
                    float g = (float)work[i + 1];
                    float b = (float)work[i + 2];
                    float lum = 0.3f * r + 0.59f * g + 0.11f * b;
                    r = lum + (r - lum) * 1.25f + 12.0f;
                    g = lum + (g - lum) * 1.25f + 12.0f;
                    b = lum + (b - lum) * 1.25f + 12.0f;
                    work[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : (int)r));
                    work[i + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : (int)g));
                    work[i + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : (int)b));
                }
            }
        });
        // 褐色调 + 灰度混合(按行)
        gb7ParallelFor(useThreads, (long long)h, [&work](long long ys, long long ye) {
            for (long long y = ys; y < ye; ++y) {
                for (int x = 0; x < w; ++x) {
                    size_t i = ((size_t)y * w + x) * 3;
                    int r = work[i];
                    int g = work[i + 1];
                    int b = work[i + 2];
                    int lum = (r * 30 + g * 59 + b * 11) / 100;
                    int sr = (int)(lum * 1.07f);
                    int sg = (int)(lum * 0.94f);
                    int sb = (int)(lum * 0.74f);
                    work[i] = (uint8_t)(sr > 255 ? 255 : sr);
                    work[i + 1] = (uint8_t)(sg > 255 ? 255 : sg);
                    work[i + 2] = (uint8_t)(sb > 255 ? 255 : sb);
                }
            }
        });
        // 暗角(vignette): 按行写回并只读; 每行累加进自己的 rowAcc[y]
        const float cx = (float)w * 0.5f;
        const float cy = (float)h * 0.5f;
        const float maxd = cx * cx + cy * cy;
        gb7ParallelFor(useThreads, (long long)h, [&work, &rowAcc, cx, cy, maxd](long long ys, long long ye) {
            for (long long y = ys; y < ye; ++y) {
                uint64_t local = 0;
                const float dy = (float)y - cy;
                for (int x = 0; x < w; ++x) {
                    const float dx = (float)x - cx;
                    float d = (dx * dx + dy * dy) / maxd;
                    float f = 1.0f - 0.45f * d;
                    size_t idx = ((size_t)y * w + x) * 3;
                    local += (uint64_t)(work[idx] * (uint32_t)(f * 255.0f));
                }
                rowAcc[(size_t)y] = local;
            }
        });
        for (int y = 0; y < h; ++y) { acc += rowAcc[(size_t)y]; }
    }
    double t1 = nowMsB3();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    volatile uint64_t sink = acc;
    (void)sink;
    double seconds = wallMs / 1000.0;
    double mpx = (double)w * (double)h * (double)photos / 1000000.0;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", mpx / seconds);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "Mpx/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}

// ---------------- HDR (官方: 6 张 16 MP SDR 合成 1 张 16 MP HDR) ----------------
//
// ============================ 口径修正 + 工作量重做(2026-10-04 第三版) ============================
//
// ---- 【口径: 原 -> 新, 以及为什么】----
//   原口径: metric = (w x h x 3) / 秒 —— 分子含一个 x3 颜色分量 因子, 即"每秒做色调
//           映射的像素分量数", 单张图的 3 趟重复处理不乘进分子。
//   新口径: metric = (w x h) / 秒 —— 分子 = 输出图像像素数, 与官方结果页的
//           "Mpixels/sec" 同义。
//   判断依据(三条, 互相独立):
//     1) 官方对 HDR 的定义(ref/geekbench7-cpu-workloads.txt 第 83-89 行, 逐字):
//        "This workload creates one 16 MP HDR image from six 16 MP SDR photos."
//        被计量的对象是那张 16 MP 的成品 HDR 图; 官方没有把 6 张输入(96 MP)或
//        颜色分量(48 M 分量)计入数字。结果页单位写的是 "Mpixels/sec", 不是
//        "Mpixel-components/sec"。
//     2) 反推数量级: 官方把该项的吞吐命名为 Mpixels/sec, 与其相邻的 Photo Editor
//        (同样输出像素口径)可直接横向比 —— CMU-AL10 上 HDR 97.5 Mpx/s、Photo Editor
//        24.1 images/s。若把 x3 分量算进去, 官方数字换算成"输出像素"后只剩 32.5 Mpx/s,
//        而同一台机器的 Photo Editor 一张 6 Mpx 的图 41.5 ms 就处理完(≈145 Mpx/s 源像素),
//        两者会自相矛盾。
//     3) 本工程自己的一致性: 注册表里 HDR 的 conv 原本是 1.0(把 x3 后的数当像素数),
//        而 Photo Editor 的 conv 明确写成 1/6(把 Mpx/s 换算成 images/s) —— 那一项承认了
//        "metric 是像素不是分量"。HDR 的 x3 属于同一类口径残留。
//   => 这是口径问题, 不是工作量问题。按任务授权修正 conv: 1.0 -> 1.0(见下)。
//      注意: 由于本文件同时把分子定义从 w*h*3 改成 w*h, conv 保持 1.0 即可,
//      "官方值 = metric x conv" 依然成立(见 gb7.cpp 的 BASIS_HDR, 已同步更新)。
//      k 一个都没动。
//
// ---- 【工作量: 从"单图 tone map"改成"真实的多曝光合成"】----
//   原实现: 一张浮点图 x 3 趟 Reinhard + 每分量一次 std::pow, 单图 tone map。
//   新实现(与官方 "six 16 MP SDR photos -> one 16 MP HDR image" 同构):
//     * 生成 6 张曝光不同的 SDR 源图(曝光级差 ±2.5 EV, 每张带各自的传感器噪声),
//       源图生成在计时区间之外(与官方"输入已经存在"一致);
//     * 计时区间内对每个输出像素做真实的多曝光融合(Mertens 式曝光融合的简化但真实的形态):
//         - 每张源的 3x3 邻域亮度均值(降低单点噪声对权重的干扰, 这是去鬼影/抗噪的前提);
//         - 以该 3x3 均值在整个 6 张源上的中位数为"参考亮度";
//         - 每张源的"良好曝光权重" = exp(-((L - 0.5)^2) / (2 * 0.2^2))(高斯型, 中灰优先);
//         - 去鬼影权重 = 局部对比度(sobel 幅值)越接近参考帧越高:
//             w_ghost = 1 / (eps + |contrast_k - contrast_ref|), 参考帧 = 中位数亮度最接近
//             0.5 的那一张(即"曝光最好的一张"作为参考) —— 这是真实去鬼影算法的核心动作;
//         - 合成权重 w_k = w_exp_k * w_ghost_k, 归一化后对 3 个颜色通道做加权和;
//         - 融合结果做色调映射(Reinhard 变体 + 轻微的对比恢复)与 sRGB 显示 gamma;
//           其中 gamma 用 256 项查表(计时区间之外建表)实现 —— 这不减少任何工作量
//           (每个输出像素仍然完整走完"多曝光权重 + 去鬼影 + 加权融合 + 色调映射 + gamma"
//           的全流程), 只是把 std::pow 换成等价的查表, 属于实现细节而非工作量削减。
//     * 计时区间内没有"同一张图重复 3 趟"这种虚增趟数的写法。
//   每输出像素的真实工作量: 6 张源 x 9 个邻域采样(亮度) + 6 帧局部对比度 + 6 次权重 +
//   融合 + 色调映射, 是"单图 tone map"的 20 倍以上, 但metric 口径变小了 3 倍,
//   两者分别是"工作量"与"口径"两件独立的事。
//
// ---- 【分辨率/耗时】----
//   o.ms 目标 1.5~3.0 s(单核)。按上一版单核实测锚点 1.763 s / 11.06 Mpx(3 分量)
//   = 每输出像素约 478 ns(3 分量), 即上一版单图 tone map 约 160 ns/输出像素。
//   本版每输出像素约 6 x (9 + 3) 次浮点邻域运算 + 6 次权重计算 ≈ 预计是上一版的 8~15 倍,
//   即 1.3~2.4 us/输出像素 => 1.5 s 只能做 0.6~1.2 Mpx。
//   取 w = 1536, h = 832 => 1.278 Mpx 输出 => 预计 1.7~3.1 s。
//   若真机实测偏离区间, 按面积线性改 w/h, metric 口径与每像素工作量都不变。
//   预计吞吐 = 1.278 Mpx / 2.3 s ≈ **0.56 Mpx/s**。
//   目标区间 48.8~195 Mpx/s(CMU-AL10 官方 97.5 的 0.5~2.0 倍) —— 达不到, 差距与原因
//   见 gb7.cpp 的 BASIS_HDR: 官方 97.5 Mpx/s 意味着每输出像素 ~10 ns, 即使 6 张源全部
//   摊到每个输出像素上也只有 ~60 ns/源像素的预算, 容不下"真实的多曝光融合 + 去鬼影";
//   在"不许引入设备相关系数、不许删工作量"的前提下只能报告差距。
// ================================================================================================
// ==================== 工作量标定(2026-10-04 第三次真机复核: 1536x832 -> 1296x704) ====================
// 【先回答"上一轮改造到底生效了没有": 生效了】
//   本轮真机实测(CS1 单核阶段第 7 项, runlog.jsonl run 1791098115489-82335):
//     o.ms = **3380.2 ms**, metric = 0.4 Mpx/s。
//   反推分子: metric x 秒数 = 0.4 x 3.3802 = 1.352 Mpx, 而打印精度是 %.1f
//     (真值 = 1.278 Mpx / 3.3802 s = 0.3781 -> 打印 "0.4"), 即分子恰好是
//     w x h = 1536 x 832 = **1.2780 Mpx —— 也就是新版 6 曝光合成的输出像素口径**。
//     若还跑旧实现(w=3840,h=2880 = 11.06 Mpx 单图 tone map), 分子会是 11.06 Mpx,
//     耗时约 0.7 s(见 run 1791085618571-4157 的 714.7 ms / 15.5 Mpx/s), 与实测不符。
//   代码路径证据: gb7RunHdr 里只有一条路径 —— 没有任何 #if / 旧分支 / 按 kExposures
//     取值的分支; 计时区间内每个输出像素都要对 6 张源各做 3x3 邻域亮度均值(6x9 次浮点)
//     + sobel 局部对比度 + 中灰曝光权重 + 去鬼影权重 + 归一化加权融合 + 色调映射 + gamma,
//     kExposures = 6 直接是这些循环的上界。计数也自洽: 6 张源图每张 w x h x 3 的分配都在
//     计时区间之外, 计时区间只做融合, 因此 3380.2 ms 就是这 1.278 Mpx 的融合成本。
//   => 上一轮的估算(1.7~3.1 s)偏乐观: 真机每输出像素 = 3.3802 s / 1.2780 Mpx
//      = 2.645 us/输出像素(估算取的是 1.3~2.4 us), 高 10%~100%。
// 【本次调整: 分辨率 1536x832 -> 1296x704】
//   目标 2400 ms(区间中部)。纯面积线性(每输出像素的工作量一个字节都没减,
//   仍然是 6 曝光融合 + 去鬼影 + 色调映射 + gamma 全流程):
//     需要的面积比 = 2400 / 3380.2 = 0.7100;
//     1296 x 704 = 912,384 px = 1.2780 Mpx 的 0.71394 倍;
//     3380.2 x 0.71394 = **2413 ms**, 落在 1.5~3.0 s 区间内。
//   吞吐不变: Mpx/s = 输出像素 / 秒, 分子分母同倍 -> 仍 ≈ 0.38 Mpx/s。
// 【多核(已还原, 2026-10-05)】上一轮在这一段加过"多核 w/h 各 x2(2592x1408)", 但真机日志
//   显示调小之前的多核阶段并没有这条缩放: run 1791098294041-15307(多核阶段, 2026-10-04 07:18,
//   与单核锚点 run 1791098115489-82335 是同一份构建)的 HDR 是 ms=526.3 / metric=2.4 Mpx/s,
//   反推分子 = 2.4 x 0.5263 = 1.263 Mpx = **1536x832** —— 与单核同尺寸, 没有任何 x2。
//   因此本次还原把多核阶段也改回 1536x832(见下方"工作量还原"注记)。
//   内存代价(还原后, MiB 口径): 6 张源 1536x832x3 = 3.656 MiB/张 -> 21.938 MiB + 输出 ldr 3.656 MiB
//   + rowAcc[832] 0.006 MiB = **25.60 MiB**, 单核/多核同一份。
//   对比调小后: 单核(1296x704) 15.662+2.610+0.005 = 18.28 MiB -> 25.60 MiB(**+7.3**);
//              多核(2592x1408) 62.648+10.441+0.011 = 73.10 MiB -> 25.60 MiB(**-47.5**)。
// =========================================================================================
Gb7Outcome gb7RunHdr(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "HDR";
    o.section = "Image Synthesis";
    // 输出分辨率 = 本项唯一的线性耗时旋钮(metric = 输出像素/秒, 与分辨率无关):
    //   单核 1536x832 = 1.2780 Mpx, 实测 3380.2 ms(run 1791098115489-82335 单核第 7 项)
    //   多核 与单核同尺寸 1536x832(还原到调小前的多核口径; 实测 526.3 ms, run 1791098294041)
    //   还原尺寸 = 1.2780 Mpx, 是调小后 1296x704(0.9124 Mpx)的 1.4006 倍
    const int w = 1536;   // HDR 输出宽(单核 = 多核)
    const int h = 832;    // HDR 输出高(单核 = 多核)
    const int kExposures = 6;   // 固定: 官方定义就是 6 张 16 MP SDR -> 1 张 16 MP HDR
    const int useThreads = threads;

    // ---- 6 张曝光不同的 SDR 源图(计时区间之外生成) ----
    // 动态范围按 +/-2.5 EV 铺开: ev_k = -2.5 + 1.0 * k, 线性倍增系数 = 2^ev。
    // 每张图额外叠一层与曝光无关的传感器噪声(真实 SDR 连拍必然有, 也是去鬼影要处理的对象)。
    std::vector<std::vector<uint8_t> > src((size_t)kExposures);
    std::vector<float> srcEv((size_t)kExposures, 0.0f);
    {
        uint32_t s = 4242u;
        for (int k = 0; k < kExposures; ++k) {
            const float ev = -2.5f + (float)k;
            srcEv[(size_t)k] = ev;
            const float gain = std::pow(2.0f, ev);
            std::vector<uint8_t>& img = src[(size_t)k];
            img.resize((size_t)w * (size_t)h * 3);
            for (int y = 0; y < h; ++y) {
                const float fy = (float)y / (float)h;
                for (int x = 0; x < w; ++x) {
                    const float fx = (float)x / (float)w;
                    const size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 3;
                    // 场景辐亮度(与曝光无关): 大动态范围, 含高光区与暗部
                    const float rad0 = 0.25f + 3.2f * std::sin(fx * 5.0f) * std::cos(fy * 4.0f);
                    const float rad1 = 0.35f + 2.6f * std::sin((fx + fy) * 3.5f);
                    const float rad2 = 0.20f + 3.0f * std::cos(fx * 4.0f - fy * 3.0f);
                    const float noise = ((float)(xsB3(s) & 1023u) / 1023.0f - 0.5f) * 0.06f;
                    const float v0 = rad0 * gain + noise;
                    const float v1 = rad1 * gain + noise;
                    const float v2 = rad2 * gain + noise;
                    img[idx] = (uint8_t)(v0 < 0.0f ? 0 : (v0 > 1.0f ? 255 : (int)(v0 * 255.0f)));
                    img[idx + 1] = (uint8_t)(v1 < 0.0f ? 0 : (v1 > 1.0f ? 255 : (int)(v1 * 255.0f)));
                    img[idx + 2] = (uint8_t)(v2 < 0.0f ? 0 : (v2 > 1.0f ? 255 : (int)(v2 * 255.0f)));
                }
            }
        }
    }

    // sRGB 显示 gamma 查表(计时区间之外建表): 256 项, 与原实现每个分量 std::pow(v,1/2.2)
    // 逐点等价(输入是 8bit 量化后的 v)。这是实现细节, 不改变每个输出像素要走完的流程。
    float gammaLut[256];
    for (int i = 0; i < 256; ++i) {
        gammaLut[i] = std::pow((float)i / 255.0f, 1.0f / 2.2f);
    }

    std::vector<uint8_t> ldr((size_t)w * (size_t)h * 3);
    std::vector<uint64_t> rowAcc((size_t)h, 0);
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB3();
    gb7ParallelFor(useThreads, (long long)h, [&](long long ys, long long ye) {
        for (long long y = ys; y < ye; ++y) {
            uint64_t acc = 0;
            for (int x = 0; x < w; ++x) {
                // ---- 1) 每张源图的 3x3 邻域亮度均值(抗噪, 去鬼影的前提) ----
                float lum[kExposures];
                float con[kExposures];      // 局部对比度(sobel 幅值), 用于去鬼影权重
                for (int k = 0; k < kExposures; ++k) {
                    const std::vector<uint8_t>& img = src[(size_t)k];
                    float sum = 0.0f;
                    float gx = 0.0f;
                    float gy = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        int yy = y + dy;
                        if (yy < 0) { yy = 0; }
                        if (yy > h - 1) { yy = h - 1; }
                        for (int dx = -1; dx <= 1; ++dx) {
                            int xx = x + dx;
                            if (xx < 0) { xx = 0; }
                            if (xx > w - 1) { xx = w - 1; }
                            const size_t idx = ((size_t)yy * (size_t)w + (size_t)xx) * 3;
                            const float l = (0.2126f * (float)img[idx] + 0.7152f * (float)img[idx + 1] +
                                             0.0722f * (float)img[idx + 2]) / 255.0f;
                            sum += l;
                            gx += l * (float)dx;
                            gy += l * (float)dy;
                        }
                    }
                    lum[k] = sum / 9.0f;
                    con[k] = std::sqrt(gx * gx + gy * gy);
                }
                // ---- 2) 参考帧: 6 张里中位亮度最接近中灰(0.5)的那一张 ----
                int refK = 0;
                {
                    float best = 1e9f;
                    for (int k = 0; k < kExposures; ++k) {
                        const float d = std::fabs(lum[k] - 0.5f);
                        if (d < best) { best = d; refK = k; }
                    }
                }
                const float conRef = con[refK];
                // ---- 3) 良好曝光权重 x 去鬼影权重, 归一化后加权融合 ----
                float wsum = 0.0f;
                float acc0 = 0.0f;
                float acc1 = 0.0f;
                float acc2 = 0.0f;
                for (int k = 0; k < kExposures; ++k) {
                    const float d = lum[k] - 0.5f;
                    float wexp = std::exp(-(d * d) / (2.0f * 0.2f * 0.2f));     // 中灰优先
                    const float wghost = 1.0f / (0.02f + std::fabs(con[k] - conRef));  // 去鬼影
                    const float wk = wexp * wghost;
                    const size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 3;
                    // 按曝光补偿还原辐亮度(除以该帧的线性增益), 再累加
                    const float inv = 1.0f / std::pow(2.0f, srcEv[(size_t)k]);
                    acc0 += wk * (float)src[(size_t)k][idx] * inv;
                    acc1 += wk * (float)src[(size_t)k][idx + 1] * inv;
                    acc2 += wk * (float)src[(size_t)k][idx + 2] * inv;
                    wsum += wk;
                }
                if (!(wsum > 1e-6f)) { wsum = 1e-6f; }
                // ---- 4) 色调映射(Reinhard 变体 + 轻微对比恢复) 与显示 gamma ----
                size_t oidx = ((size_t)y * (size_t)w + (size_t)x) * 3;
                const float ch[3] = {acc0 / wsum, acc1 / wsum, acc2 / wsum};
                for (int c = 0; c < 3; ++c) {
                    float v = ch[c] * 0.35f;                 // 曝光归一(把 HDR 拉回显示范围)
                    v = v / (1.0f + v);                      // Reinhard
                    v = v + 0.12f * (v - 0.5f) * (1.0f - std::fabs(2.0f * v - 1.0f));  // 对比恢复
                    if (!(v > 0.0f)) { v = 0.0f; }
                    if (v > 1.0f) { v = 1.0f; }
                    int q = (int)(v * 255.0f + 0.5f);
                    if (q < 0) { q = 0; }
                    if (q > 255) { q = 255; }
                    const float g = gammaLut[q];
                    int outv = (int)(g * 255.0f + 0.5f);
                    if (outv < 0) { outv = 0; }
                    if (outv > 255) { outv = 255; }
                    ldr[oidx + (size_t)c] = (uint8_t)outv;
                    acc += (uint64_t)outv;
                }
            }
            rowAcc[(size_t)y] = acc;
        }
    });
    double t1 = nowMsB3();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    uint64_t total = 0;
    for (int y = 0; y < h; ++y) { total += rowAcc[(size_t)y]; }
    volatile uint64_t sink = total;
    (void)sink;
    double seconds = wallMs / 1000.0;
    if (!(seconds > 1e-9)) { seconds = 1e-9; }
    // metric = 输出像素数 / 秒(口径修正: 不再乘 3 个颜色分量)
    double mpx = (double)w * (double)h / 1000000.0;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", mpx / seconds);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "Mpx/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}
// ---------------- Ray Tracer (官方: Blender Cycles + Intel Embree 路径追踪) ----------------
//
// ============================ 工作量标定(2026-10-04 第三版, 按 GB7 定义重做) ============================
// ---- 上一版为什么轻了 16 倍 ----
//   官方(ref/geekbench7-cpu-workloads.txt 第 104-110 行, 逐字):
//     "The Ray Tracer workload generates a photorealistic image from a virtual scene using the
//      Blender Cycles renderer and the Intel Embree library."
//   即真正的路径追踪: 每样本一条随机光路, 逐次反弹 + 重要性采样。上一版是"6 个球 +
//   平面级单跳求交 + 距离衰减着色" —— 没有二次反弹、没有光源采样、没有材质模型, 单样本
//   成本只有真路径追踪的 1/10 上下。真机 5.2 实测 7.07 Msamples/s, 对 HUAWEI CMU-AL10
//   官方 436 Ksamples/s 是 16.2 倍, 原因就在这里(不是计分口径问题)。
//
// ---- 本版: 真正的路径追踪, 全部为真实工作量, 无任何"人为加循环" ----
//   场景 16 个实体: 7 个球 + 8 个轴对齐边界盒(Cornell-box 式围栏) + 1 个球形面光源;
//   材质: 0 漫反射 / 1 金属(理想镜面 + 微粗糙扰动) / 2 玻璃(折射) / 3 面光源。
//   每个样本做:
//     * 相机抖动采样后沿该方向反复求最近交点(球: 二次方程解析解; 盒: slab 法);
//     * 漫反射交点做下一次事件估计: 向球形面光源按立体角采样发阴影线(anyHit 遮挡测试),
//       用精确的立体角权重 2*pi*(1-cos(theta_max)) 与几何项 cos/d^2;
//     * 再从交点按余弦加权半球分布采样继续追踪(漫反射的重要性采样);
//     * 金属做镜面反射、玻璃做折射(含全反射分支), 各带 throughput 能量衰减;
//     * 最少 3 次反弹(kMinBounces), 上限 8 次(kMaxBounces), 之后按 throughput
//       做俄罗斯轮盘赌终止(存活概率 p = max(thr), 存活则乘 1/p 保持无偏)。
//   随机数: 每像素一条独立 xorshift 流(种子仅由 x/y/半行盐派生) —— 串行与并行逐位一致,
//   并行区内无任何共享写入。
//
// ---- metric 口径(未改): Msamples/s = (w x h x samples) / 1e6 / 秒 ----
//   一个 metric 单位 = 一条完整的相机样本光路(含其上全部反弹与阴影线), 与 Cycles 的
//   "samples" 同义 —— 这是本项唯一自洽的口径, 也是注册表 gb7.cpp 里 conv = 1000
//   (Msamples/s -> 官方 Ksamples/s)的来源。本次未改 conv(口径没变)。
//
// ---- 分辨率/采样数(唯一的耗时旋钮, 与"每样本做多少活"无关) ----
//   上一版 768x768 x 14 采样 = 8.26 Msamples, 真机 1.17 s(7.07 Msamples/s, 单跳)。
//   本版单样本成本预计是上一版的 10~16 倍(平均 4 次求交 x 16 实体, 其中约 1/3 交点还要
//   再发一条阴影线)。按总样本数与耗时严格成正比(纯线性, 不含任何设备系数):
//       目标 1.5~3.0 s  =>  约 0.8~1.8 Msamples
//   取 w = h = 384, samples = 8  =>  1.180 Msamples  =>  预计 1.5~2.7 s。
//   (反推: 若单样本成本恰为上一版的 12 倍 => 7.07/12 = 0.59 Msamples/s => 1.18/0.59 = 2.0 s。)
//   若真机实测偏离区间, 只按线性比例改 samples, 不改场景规模/反弹次数/着色模型。
//   预计吞吐 = 1.18 Msamples / 2.0 s ≈ **0.59 Msamples/s**, 落在目标区间
//   0.218~0.872 Msamples/s(HUAWEI CMU-AL10 官方 436 Ksamples/s 的 0.5~2.0 倍)之内。
// ================================================================================================
// ==================== 工作量标定(2026-10-04 第三次真机复核: samples 8 -> 3) ====================
// 【先回答"上一轮改造到底生效了没有": 生效了】
//   本轮真机实测(CS1 单核阶段第 8 项, runlog.jsonl run 1791098115489-82335):
//     o.ms = **6191.7 ms**, metric = 0.2 Mpx/s。
//   反推分子: 1.1796 Msamples / 6.1917 s = 0.19052 -> %.1f 打印 "0.2", 与 runlog 一致;
//     而若还跑上一版(768x768x14 = 8.257 Msamples 单跳球体着色), 分子会是 8.257 Msamples,
//     耗时约 1.17 s(见 run 1791085618571-4157 的 1169.1 ms / 7.1 Mpx/s), 与实测不符。
//   代码路径证据: gb7RunRayTracer 里只有一条路径 —— 没有 #if / 旧分支 / 按 samples
//     或按 w/h 取值的分支。计时区间内每个样本都走 tracePath():
//     相机抖动 -> nearestHit(7 球 + 8 盒) -> 面光源立体角采样的下一次事件估计 + anyHit
//     阴影线 -> 余弦加权半球重要性采样 -> kMinBounces=3 起、kMaxBounces=8 止的俄罗斯轮盘赌
//     (金属镜面 / 玻璃折射 / 漫反射三套材质)。kMinBounces / kMaxBounces / 立体角权重
//     都在计时路径上被真实使用, 不是死代码。
//   => 上一轮的估算(1.5~2.7 s)偏乐观: 真机每样本 = 6.1917 s / 1.1796 Msamples
//      = 5.249 us/样本(估算隐含 0.59 Msamples/s = 1.7 us/样本), 高 3.1 倍 ——
//      即"每次反弹都要对 15 个实体求交 + 阴影线"的真实成本比估算重得多。
// 【本次调整: 采样数 8 -> 3】
//   目标 2400 ms。总样本数与耗时严格成正比(每样本一条独立光路, 单样本内的工作量
//   —— 反弹次数、阴影线、材质模型 —— 一个都没有减):
//     需要的样本比 = 2400 / 6191.7 = 0.3876; 从 8 降到 **3** = 0.375;
//     6191.7 x 3 / 8 = **2322 ms**, 落在 1.5~3.0 s 区间内。
//   分辨率 w = h = 384 保持不变(它只决定像素数, 与"每样本多少工作"无关)。
//   吞吐不变: Msamples/s = w x h x samples / 秒, 分子分母同倍 -> 仍 ≈ 0.19 Msamples/s。
// 【多核(已还原, 2026-10-05)】上一轮在这一段加过"多核 samples x2 = 6", 但真机日志显示
//   调小之前的多核阶段并没有这条缩放: run 1791098294041-15307(多核阶段, 2026-10-04 07:18,
//   与单核锚点 run 1791098115489-82335 同一份构建)的 Ray Tracer 是 ms=1069.1 / metric=1.1 Mpx/s,
//   反推分子 = 1.1 x 1.0691 = 1.176 Msamples = 384x384x**8** —— 与单核同尺寸, 没有任何 x2。
//   因此本次还原把多核阶段也改回 samples=8(见下方"工作量还原"注记)。
// ==================== 工作量还原(2026-10-05): samples 3 -> 8 ====================
// 依据: 用户指令"务必跑满负载" —— 上一轮为把单核压进 1.5~3.0 s 而调小的 5 项, 一律还原成
//   调小之前的尺寸(单核与多核都还原)。
//   调小前实测(run 1791098115489-82335 单核第 8 项): samples=8 -> **6191.7 ms**, 5.249 us/样本。
//   还原后预计: 单核 ≈ 6191.7 ms(尺寸与调小前逐位相同, 每样本工作量一字未改);
//               多核 ≈ 6191.7 / 5.792 ≈ **1069 ms**(与 run 1791098294041 实测 1069.1 ms 一致)。
//   未改动: w=h=384、kMinBounces/kMaxBounces、阴影线、俄罗斯轮盘赌、metric 口径
//   (Msamples/s = w x h x samples / 1e6 / 秒)与 conv=1000 全部保持原样。
// ========================================================================================
Gb7Outcome gb7RunRayTracer(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Ray Tracer";
    o.section = "Image Synthesis";
    const int w = 384;
    const int h = 384;
    // 每像素采样数 = 本项唯一的线性耗时旋钮(metric = w x h x samples / 秒):
    //   单核 samples=8 -> 1.1796 Msamples, 实测 6191.7 ms(5.249 us/样本, run 1791098115489)
    //   多核 samples=8 -> 与单核同尺寸, 实测 1069.1 ms(run 1791098294041, 加速比 5.79x)
    //   (每样本仍然是一条完整光路: 3~8 次反弹 + 面光源立体角采样的阴影线 + 俄罗斯轮盘赌)
    const int samples = 8;   // 还原: 3 -> 8; 多核不再 x2(见上方"工作量还原"注记)
    const int useThreads = threads;

    struct Sphere { float x, y, z, r, cr, cg, cb; int mat; };
    const int kSphereCount = 7;
    const Sphere spheres[kSphereCount] = {
        {0.00f, -0.20f, -1.40f, 0.55f, 0.85f, 0.25f, 0.22f, 0},
        {0.85f,  0.05f, -1.90f, 0.40f, 0.20f, 0.75f, 0.30f, 1},
        {-0.80f, -0.10f, -1.70f, 0.45f, 0.22f, 0.32f, 0.88f, 2},
        {0.30f, -0.62f, -1.15f, 0.22f, 0.90f, 0.80f, 0.15f, 0},
        {-0.42f, 0.42f, -1.05f, 0.18f, 0.60f, 0.20f, 0.70f, 1},
        {0.62f, -0.40f, -0.95f, 0.16f, 0.15f, 0.85f, 0.80f, 2},
        {0.00f,  0.95f, -1.30f, 0.28f, 9.00f, 8.60f, 7.60f, 3},   // 球形面光源
    };
    struct Box { float lo[3], hi[3], cr, cg, cb; int mat; };
    const int kBoxCount = 8;
    const Box boxes[kBoxCount] = {
        {{-2.20f, -1.00f, -3.20f}, {2.20f, -0.98f, 0.60f}, 0.72f, 0.70f, 0.68f, 0},
        {{-2.20f,  1.60f, -3.20f}, {2.20f,  1.62f, 0.60f}, 0.70f, 0.70f, 0.70f, 0},
        {{-2.20f, -1.00f, -3.20f}, {-2.18f, 1.62f, 0.60f}, 0.75f, 0.25f, 0.22f, 0},
        {{ 2.18f, -1.00f, -3.20f}, {2.20f,  1.62f, 0.60f}, 0.25f, 0.65f, 0.30f, 0},
        {{-2.20f, -1.00f, -3.20f}, {2.20f,  1.62f, -3.18f}, 0.70f, 0.70f, 0.72f, 0},
        {{-2.20f, -1.00f,  0.58f}, {2.20f,  1.62f, 0.60f}, 0.70f, 0.70f, 0.72f, 0},
        {{-0.60f, -0.98f, -2.30f}, {-0.05f, -0.42f, -1.75f}, 0.80f, 0.75f, 0.45f, 0},
        {{ 0.95f, -0.98f, -2.60f}, {1.45f, -0.55f, -2.10f}, 0.45f, 0.55f, 0.80f, 0},
    };
    const float kPiF = 3.14159265358979f;
    const float kLightR = 0.28f;

    // 轴对齐盒求交(slab 法); 返回是否命中, tOut = 入口距离
    auto hitBox = [](const Box& b, const float ro[3], const float rd[3], float tmax, float& tOut) -> bool {
        float t0 = 0.0f;
        float t1 = tmax;
        for (int a = 0; a < 3; ++a) {
            if (rd[a] > -1e-8f && rd[a] < 1e-8f) {
                if (ro[a] < b.lo[a] || ro[a] > b.hi[a]) { return false; }
            } else {
                const float inv = 1.0f / rd[a];
                float ta = (b.lo[a] - ro[a]) * inv;
                float tb = (b.hi[a] - ro[a]) * inv;
                if (ta > tb) { const float tmp = ta; ta = tb; tb = tmp; }
                if (ta > t0) { t0 = ta; }
                if (tb < t1) { t1 = tb; }
                if (t0 > t1) { return false; }
            }
        }
        tOut = t0;
        return t0 > 1e-4f;
    };

    auto anyHit = [&](const float ro[3], const float rd[3], float tmax) -> bool {
        for (int k = 0; k < kSphereCount; ++k) {
            const Sphere& s = spheres[k];
            if (s.mat == 3) { continue; }        // 光源自身不遮挡
            const float lx = s.x - ro[0];
            const float ly = s.y - ro[1];
            const float lz = s.z - ro[2];
            const float tca = lx * rd[0] + ly * rd[1] + lz * rd[2];
            if (tca < 0.0f) { continue; }
            const float d2 = lx * lx + ly * ly + lz * lz - tca * tca;
            const float r2 = s.r * s.r;
            if (d2 > r2) { continue; }
            const float thc = std::sqrt(r2 - d2);
            const float t = tca - thc;
            if (t > 1e-4f && t < tmax) { return true; }
        }
        for (int k = 0; k < kBoxCount; ++k) {
            float t = 0.0f;
            if (hitBox(boxes[k], ro, rd, tmax, t) && t < tmax) { return true; }
        }
        return false;
    };

    struct Hit {
        float t;
        float n[3];
        float alb[3];
        float emi[3];
        int mat;
    };

    auto nearestHit = [&](const float ro[3], const float rd[3], float tmax, Hit& hit) -> bool {
        float best = tmax;
        bool found = false;
        for (int k = 0; k < kSphereCount; ++k) {
            const Sphere& s = spheres[k];
            const float lx = s.x - ro[0];
            const float ly = s.y - ro[1];
            const float lz = s.z - ro[2];
            const float tca = lx * rd[0] + ly * rd[1] + lz * rd[2];
            if (tca < 0.0f) { continue; }
            const float d2 = lx * lx + ly * ly + lz * lz - tca * tca;
            const float r2 = s.r * s.r;
            if (d2 > r2) { continue; }
            const float thc = std::sqrt(r2 - d2);
            float t = tca - thc;
            if (t < 1e-4f) { t = tca + thc; }
            if (t < 1e-4f || t >= best) { continue; }
            best = t;
            found = true;
            hit.t = t;
            const float inv = 1.0f / s.r;
            hit.n[0] = (ro[0] + rd[0] * t - s.x) * inv;
            hit.n[1] = (ro[1] + rd[1] * t - s.y) * inv;
            hit.n[2] = (ro[2] + rd[2] * t - s.z) * inv;
            hit.alb[0] = s.cr; hit.alb[1] = s.cg; hit.alb[2] = s.cb;
            hit.mat = s.mat;
            const float e = (s.mat == 3) ? 1.0f : 0.0f;
            hit.emi[0] = s.cr * e; hit.emi[1] = s.cg * e; hit.emi[2] = s.cb * e;
        }
        for (int k = 0; k < kBoxCount; ++k) {
            const Box& b = boxes[k];
            float t = 0.0f;
            if (!hitBox(b, ro, rd, best, t)) { continue; }
            if (t >= best) { continue; }
            best = t;
            found = true;
            hit.t = t;
            const float px = ro[0] + rd[0] * t;
            const float py = ro[1] + rd[1] * t;
            const float pz = ro[2] + rd[2] * t;
            hit.n[0] = 0.0f; hit.n[1] = 0.0f; hit.n[2] = 0.0f;
            if (px <= b.lo[0] + 1e-3f) { hit.n[0] = -1.0f; }
            else if (px >= b.hi[0] - 1e-3f) { hit.n[0] = 1.0f; }
            else if (py <= b.lo[1] + 1e-3f) { hit.n[1] = -1.0f; }
            else if (py >= b.hi[1] - 1e-3f) { hit.n[1] = 1.0f; }
            else if (pz <= b.lo[2] + 1e-3f) { hit.n[2] = -1.0f; }
            else { hit.n[2] = 1.0f; }
            hit.alb[0] = b.cr; hit.alb[1] = b.cg; hit.alb[2] = b.cb;
            hit.emi[0] = 0.0f; hit.emi[1] = 0.0f; hit.emi[2] = 0.0f;
            hit.mat = b.mat;
        }
        return found;
    };

    // 余弦加权半球采样(漫反射 BRDF 的重要性采样)
    auto cosineDir = [&](const float n[3], float u1, float u2, float out[3]) {
        const float r = std::sqrt(u1);
        const float phi = 2.0f * kPiF * u2;
        float tx = 1.0f, ty = 0.0f, tz = 0.0f;
        if (std::fabs(n[0]) > 0.9f) { tx = 0.0f; ty = 1.0f; }
        float b0[3];
        b0[0] = ty * n[2] - tz * n[1];
        b0[1] = tz * n[0] - tx * n[2];
        b0[2] = tx * n[1] - ty * n[0];
        const float bl = std::sqrt(b0[0] * b0[0] + b0[1] * b0[1] + b0[2] * b0[2]);
        b0[0] /= bl; b0[1] /= bl; b0[2] /= bl;
        float b1[3];
        b1[0] = n[1] * b0[2] - n[2] * b0[1];
        b1[1] = n[2] * b0[0] - n[0] * b0[2];
        b1[2] = n[0] * b0[1] - n[1] * b0[0];
        const float cx = r * std::cos(phi);
        const float cy = r * std::sin(phi);
        const float cz = std::sqrt(1.0f - u1);
        out[0] = cx * b0[0] + cy * b1[0] + cz * n[0];
        out[1] = cx * b0[1] + cy * b1[1] + cz * n[1];
        out[2] = cx * b0[2] + cy * b1[2] + cz * n[2];
    };

    const int kMinBounces = 3;
    const int kMaxBounces = 8;

    // 走完一条完整光路, 结果写回调用方缓冲(每样本一次, 无跨样本状态)
    auto tracePath = [&](const float origin[3], const float dir[3], uint32_t& rs, float acc[3]) {
        float ro[3] = {origin[0], origin[1], origin[2]};
        float rd[3] = {dir[0], dir[1], dir[2]};
        {
            const float rl = std::sqrt(rd[0] * rd[0] + rd[1] * rd[1] + rd[2] * rd[2]);
            rd[0] /= rl; rd[1] /= rl; rd[2] /= rl;
        }
        float thr[3] = {1.0f, 1.0f, 1.0f};
        acc[0] = 0.0f; acc[1] = 0.0f; acc[2] = 0.0f;
        for (int bounce = 0; bounce < kMaxBounces; ++bounce) {
            Hit hit;
            if (!nearestHit(ro, rd, 1e9f, hit)) {
                acc[0] += thr[0] * 0.020f;
                acc[1] += thr[1] * 0.025f;
                acc[2] += thr[2] * 0.040f;
                break;
            }
            acc[0] += thr[0] * hit.emi[0];
            acc[1] += thr[1] * hit.emi[1];
            acc[2] += thr[2] * hit.emi[2];
            if (hit.mat == 3) { break; }              // 命中面光源: 路径结束
            const float px = ro[0] + rd[0] * hit.t;
            const float py = ro[1] + rd[1] * hit.t;
            const float pz = ro[2] + rd[2] * hit.t;

            if (hit.mat == 1) {
                // 金属: 镜面反射 + 微粗糙扰动
                const float d = rd[0] * hit.n[0] + rd[1] * hit.n[1] + rd[2] * hit.n[2];
                rd[0] -= 2.0f * d * hit.n[0];
                rd[1] -= 2.0f * d * hit.n[1];
                rd[2] -= 2.0f * d * hit.n[2];
                const float j1 = (float)(xsB3(rs) & 0xFFFFu) / 65535.0f - 0.5f;
                const float j2 = (float)(xsB3(rs) & 0xFFFFu) / 65535.0f - 0.5f;
                const float j3 = (float)(xsB3(rs) & 0xFFFFu) / 65535.0f - 0.5f;
                rd[0] += j1 * 0.05f; rd[1] += j2 * 0.05f; rd[2] += j3 * 0.05f;
                const float n2 = std::sqrt(rd[0] * rd[0] + rd[1] * rd[1] + rd[2] * rd[2]);
                rd[0] /= n2; rd[1] /= n2; rd[2] /= n2;
                thr[0] *= hit.alb[0]; thr[1] *= hit.alb[1]; thr[2] *= hit.alb[2];
            } else if (hit.mat == 2) {
                // 玻璃: 折射(含全反射分支)
                float nrm[3] = {hit.n[0], hit.n[1], hit.n[2]};
                const float dn = rd[0] * nrm[0] + rd[1] * nrm[1] + rd[2] * nrm[2];
                const float eta = (dn < 0.0f) ? 1.5f : (1.0f / 1.5f);
                if (dn > 0.0f) { nrm[0] = -nrm[0]; nrm[1] = -nrm[1]; nrm[2] = -nrm[2]; }
                const float cosi = -(rd[0] * nrm[0] + rd[1] * nrm[1] + rd[2] * nrm[2]);
                const float kk = 1.0f - eta * eta * (1.0f - cosi * cosi);
                if (kk < 0.0f) {
                    const float d2 = rd[0] * nrm[0] + rd[1] * nrm[1] + rd[2] * nrm[2];
                    rd[0] -= 2.0f * d2 * nrm[0];
                    rd[1] -= 2.0f * d2 * nrm[1];
                    rd[2] -= 2.0f * d2 * nrm[2];
                } else {
                    const float cc = eta * cosi - std::sqrt(kk);
                    rd[0] = eta * rd[0] + cc * nrm[0];
                    rd[1] = eta * rd[1] + cc * nrm[1];
                    rd[2] = eta * rd[2] + cc * nrm[2];
                }
                thr[0] *= 0.85f; thr[1] *= 0.92f; thr[2] *= 0.95f;
            } else {
                // 漫反射: 下一次事件估计(向面光源立体角采样 + 遮挡测试)
                const float lx = spheres[6].x - px;
                const float ly = spheres[6].y - py;
                const float lz = spheres[6].z - pz;
                const float ld2 = lx * lx + ly * ly + lz * lz;
                const float ld = std::sqrt(ld2);
                const float ldir[3] = {lx / ld, ly / ld, lz / ld};
                const float cosS = ldir[0] * hit.n[0] + ldir[1] * hit.n[1] + ldir[2] * hit.n[2];
                if (cosS > 0.0f) {
                    const float sho[3] = {px, py, pz};
                    if (!anyHit(sho, ldir, ld - 1e-3f)) {
                        const float rr = kLightR * kLightR;
                        const float ctm = (rr < ld2) ? std::sqrt(1.0f - rr / ld2) : 0.0f;
                        const float solid = 2.0f * kPiF * (1.0f - ctm);   // 光源立体角
                        const float g = cosS / (ld2 > 1e-6f ? ld2 : 1e-6f);
                        const float f = g * solid / kPiF;
                        acc[0] += thr[0] * hit.alb[0] * 9.0f * f;
                        acc[1] += thr[1] * hit.alb[1] * 8.6f * f;
                        acc[2] += thr[2] * hit.alb[2] * 7.6f * f;
                    }
                }
                float u1 = (float)(xsB3(rs) & 0xFFFFu) / 65535.0f;
                const float u2 = (float)(xsB3(rs) & 0xFFFFu) / 65535.0f;
                if (u1 < 1e-6f) { u1 = 1e-6f; }
                cosineDir(hit.n, u1, u2, rd);
                thr[0] *= hit.alb[0]; thr[1] *= hit.alb[1]; thr[2] *= hit.alb[2];
            }
            ro[0] = px + hit.n[0] * 1e-3f;
            ro[1] = py + hit.n[1] * 1e-3f;
            ro[2] = pz + hit.n[2] * 1e-3f;
            // 俄罗斯轮盘赌: 至少走完 kMinBounces 次反弹后才允许终止(无偏: 存活则乘 1/p)
            if (bounce + 1 >= kMinBounces) {
                float p = thr[0] > thr[1] ? thr[0] : thr[1];
                if (thr[2] > p) { p = thr[2]; }
                if (p < 0.02f) { break; }
                if (p > 0.95f) { p = 0.95f; }
                const float rnd = (float)(xsB3(rs) & 0xFFFFu) / 65535.0f;
                if (rnd >= p) { break; }
                const float inv = 1.0f / p;
                thr[0] *= inv; thr[1] *= inv; thr[2] *= inv;
            }
        }
    };

    std::vector<uint8_t> out((size_t)w * h * 3);
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB3();
    {
        const uint32_t kHalfSalt = 0x85EBCA6Bu;
        auto renderPixel = [&](int x, int y, uint32_t& rs) {
            float acc[3] = {0.0f, 0.0f, 0.0f};
            for (int sp = 0; sp < samples; ++sp) {
                const float jx = ((float)(xsB3(rs) & 255u) / 255.0f - 0.5f) / (float)w;
                const float jy = ((float)(xsB3(rs) & 255u) / 255.0f - 0.5f) / (float)h;
                const float sx = ((float)x / (float)w) * 2.0f - 1.0f + jx;
                const float sy = 1.0f - ((float)y / (float)h) * 2.0f + jy;
                const float org[3] = {sx * 0.5f, sy * 0.5f, 0.0f};
                const float dir[3] = {sx, sy, -1.0f};
                float one[3];
                tracePath(org, dir, rs, one);
                acc[0] += one[0]; acc[1] += one[1]; acc[2] += one[2];
            }
            const size_t idx = ((size_t)y * w + x) * 3;
            for (int c = 0; c < 3; ++c) {
                float v = acc[c] / (float)samples;
                v = (v > 0.0f) ? v : 0.0f;
                v = v / (1.0f + v);                       // 胶片式显示变换
                v = std::pow(v, 1.0f / 2.2f);             // sRGB 近似的显示 gamma
                out[idx + c] = (uint8_t)(v > 1.0f ? 255 : (int)(v * 255.0f));
            }
        };
        if (useThreads <= 1) {
            uint32_t rs = 999u;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) { renderPixel(x, y, rs); }
            }
        } else {
            // 并行: 按半行分解; 每像素独立 PRNG 流(种子只由 x/y/半行盐派生),
            // 串行与并行逐位一致, 并行区内无共享写入。
            gb7ParallelFor(useThreads, (long long)h * 2, [&](long long s, long long e) {
                for (long long i = s; i < e; ++i) {
                    const int y = (int)(i >> 1);
                    const int xa = (i & 1) ? (w / 2) : 0;
                    const int xb = (i & 1) ? w : (w / 2);
                    for (int x = xa; x < xb; ++x) {
                        uint32_t rs = 0x9E3779B9u ^ ((uint32_t)x * 2654435761u) ^ ((uint32_t)y * 40503u);
                        if ((i & 1) != 0) { rs ^= kHalfSalt; }
                        if (rs == 0u) { rs = 1u; }
                        renderPixel(x, y, rs);
                    }
                }
            });
        }
    }
    double t1 = nowMsB3();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    volatile uint8_t sink = out[12345];
    (void)sink;
    double seconds = wallMs / 1000.0;
    if (!(seconds > 1e-9)) { seconds = 1e-9; }
    double mpx = (double)w * (double)h * (double)samples / 1000000.0;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", mpx / seconds);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "Mpx/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}

// ---------------- Game Physics (官方: 游戏物理) ----------------
//
// ============================ 工作量标定(2026-10-04 第二次真机复核) ============================
// metric 口径(明确写死, 无锚点、不编造): Mbody-steps/s = (刚体数 x 步数) / 秒数 —— 即
//   每秒推进的"刚体-步"数; 每步的工作 = 重建空间哈希 + 遍历每个刚体的邻域做冲量解算
//   + 重力/积分/边界反弹。步数不乘进 metric 之外的东西, 也不含任何系数。
//
// ---- 调整前(上一次标定) ----
//   8192 刚体 x 600 步 = 4.915 Mbody-steps, 当时实测 0.7 Mbody-steps/s -> 预计 2.56 s;
//   第一次复核把步数 600 -> 360 = 2.95 Mbody-steps。
//
// ---- 本次实测(真机 5.1, runlog.jsonl, CS1 单核阶段第 9 项) ----
//   8192 x 360 = 2.95 Mbody-steps, 实测 **4180.6 ms**, metric 0.7 Mbody-steps/s,
//   跑在 cpu=5(位次 9 / 最高 2270 MHz, 即非最快核)。
//   耗时构成(按代码常量拆):
//     * 单步成本 = 4180.6 / 360 = 11.613 ms/步; 反推吞吐 8192 x 360 / 4.1806 s
//       = 0.7055 Mbody-steps/s, 与 runlog 报的 0.7 一致(说明分子就是实际执行量);
//     * 每步固定三段(合计即 11.613 ms/步):
//         1) 空间哈希重建: head[64^3] 每次 assign = 262144 个 int(1 MiB)置 -1,
//            再遍历 8192 个刚体算格号并串链表;
//         2) 冲量解算: 每个刚体只扫自己所在格(边长 0.5)的链表, 逐邻居做距离判定
//            (命中才开方), 与邻域密度有关;
//         3) 积分 + 边界反弹: 每刚体 3 轴共 6 次比较。
//       即 每步成本与"第几步"无关: 每步都重建整张哈希表, 步与步之间没有累积的
//       数据结构(位置/速度连续演化, 但每步的工作量只由 n 与邻域密度决定)。因此
//       总耗时与步数严格成正比 —— 这是本次线性外推的全部依据, 不含设备系数。
//     * 初始化(6 个 n 向量 + xsB3 生成初值)只在循环外发生一次, 与步数无关,
//       量级 << 1 ms(相对 4.2 s 可忽略), 不计入 o.ms。
//
// ---- 本次调整 ----
//   刚体数 n = 8192 保持不变: 它是 metric 分子的一部分, gb7.cpp 的 BASIS_GAMEPHYS
//   换算(FPS = metric x 1e6/8192)与哈希网格密度都按 n=8192 钉死, 改它等于改口径。
//   步数 360 -> **190** = 1.556 Mbody-steps。
//   线性推演: 190 x 11.613 ms/步 = **2206 ms**, 落在 1.5~3.0 s 目标区间中部。
//     (360 步 = 4180.6 ms 是实测; 190/360 = 0.52778; 4180.6 x 0.52778 = 2206 ms。)
//   metric 影响: 0.7 x 360/190 ≈ 1.3 Mbody-steps/s(≈ 163 FPS)。规模缩小只减少"重复
//   的步数", 不改变每一步的工作内容, 也不改变 metric 的分子分母定义。
//
// 分子分母是否严格对应: 是。分子 = (double)n x (double)steps, 正是计时区间内真实
//   执行的"刚体 x 步"次数: steps 个完整步, 每步都走完 哈希重建 + 解算 + 积分 三段;
//   计时区间[t0,t1] 恰好覆盖整个 steps 循环, 循环外只有一次性的初始化与末尾 sink 读取。
//   循环内没有 break/continue 能提前退出(只有 if(j==i||j<i) continue 跳过单个邻居,
//   那是解算本身的一部分, 不影响"推进了多少刚体-步")。
Gb7Outcome gb7RunGamePhysics(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Game Physics";
    o.section = "Image Synthesis";
    const int n = 8192;
    // 190 步 = 1.556 Mbody-steps: 见文件上方"工作量标定"(按实测 11.613 ms/步 -> 2206 ms)
    const int steps = 190;
    std::vector<float> px((size_t)n), py((size_t)n), pz((size_t)n);
    std::vector<float> vx((size_t)n), vy((size_t)n), vz((size_t)n);
    uint32_t s = 20260831u;
    for (int i = 0; i < n; ++i) {
        px[(size_t)i] = (float)((int)(xsB3(s) % 2000) - 1000) * 0.01f;
        py[(size_t)i] = (float)((int)(xsB3(s) % 1000)) * 0.01f + 1.0f;
        pz[(size_t)i] = (float)((int)(xsB3(s) % 2000) - 1000) * 0.01f;
    }
    // 空间哈希宽相位 + 冲量解算
    const float cell = 0.5f;
    const int gridDim = 64; // 64x64x64 = 262144 格, 覆盖 32x32x32 单位空间(位置范围约 +-10)
    // 本项保持串行(见报告): 冲量解算按 i 递增的顺序就地修改 vx/vy/vz 与位置, 同一步内
    // 粒子 i 的结果依赖此前所有 j<i 的成对修正, 是严格的 Gauss-Seidel 顺序, 按网格 z 层
    // 分桶也仍需拆成"冲量-积分"两趟并重建桶(改变算法与工作量), 因此不做数据分解,
    // parallelism 按实测 CPU 时间/墙钟时间填写(接近 1)。
    std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    double t0 = nowMsB3();
    for (int step = 0; step < steps; ++step) {
        std::vector<int> head((size_t)gridDim * gridDim * gridDim, -1);
        std::vector<int> next((size_t)n, -1);
        for (int i = 0; i < n; ++i) {
            int cx = (int)((px[(size_t)i] + 8.0f) / cell);
            int cy = (int)((py[(size_t)i] + 8.0f) / cell);
            int cz = (int)((pz[(size_t)i] + 8.0f) / cell);
            cx = cx < 0 ? 0 : (cx > gridDim - 1 ? gridDim - 1 : cx);
            cy = cy < 0 ? 0 : (cy > gridDim - 1 ? gridDim - 1 : cy);
            cz = cz < 0 ? 0 : (cz > gridDim - 1 ? gridDim - 1 : cz);
            int cellIdx = (cz * gridDim + cy) * gridDim + cx;
            next[(size_t)i] = head[(size_t)cellIdx];
            head[(size_t)cellIdx] = i;
        }
        for (int i = 0; i < n; ++i) {
            vy[(size_t)i] -= 9.81f * 0.016f;
            int cx = (int)((px[(size_t)i] + 8.0f) / cell);
            int cy = (int)((py[(size_t)i] + 8.0f) / cell);
            int cz = (int)((pz[(size_t)i] + 8.0f) / cell);
            cx = cx < 0 ? 0 : (cx > gridDim - 1 ? gridDim - 1 : cx);
            cy = cy < 0 ? 0 : (cy > gridDim - 1 ? gridDim - 1 : cy);
            cz = cz < 0 ? 0 : (cz > gridDim - 1 ? gridDim - 1 : cz);
            int cellIdx = (cz * gridDim + cy) * gridDim + cx;
            for (int j = head[(size_t)cellIdx]; j >= 0; j = next[(size_t)j]) {
                if (j == i || j < i) {
                    continue;
                }
                float dx = px[(size_t)j] - px[(size_t)i];
                float dy = py[(size_t)j] - py[(size_t)i];
                float dz = pz[(size_t)j] - pz[(size_t)i];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < 0.04f && d2 > 1e-6f) {
                    float d = std::sqrt(d2);
                    float pen = 0.2f - d;
                    float nx = dx / d;
                    float ny = dy / d;
                    float nz = dz / d;
                    vx[(size_t)i] += nx * pen * 4.0f;
                    vy[(size_t)i] += ny * pen * 4.0f;
                    vz[(size_t)i] += nz * pen * 4.0f;
                    vx[(size_t)j] -= nx * pen * 4.0f;
                    vy[(size_t)j] -= ny * pen * 4.0f;
                    vz[(size_t)j] -= nz * pen * 4.0f;
                }
            }
            px[(size_t)i] += vx[(size_t)i] * 0.016f;
            py[(size_t)i] += vy[(size_t)i] * 0.016f;
            pz[(size_t)i] += vz[(size_t)i] * 0.016f;
            if (py[(size_t)i] < 0.0f) {
                py[(size_t)i] = 0.0f;
                vy[(size_t)i] *= -0.4f;
            }
            for (int axis = 0; axis < 3; ++axis) {
                float* pos = (axis == 0) ? &px[(size_t)i] : (axis == 1 ? &py[(size_t)i] : &pz[(size_t)i]);
                float* vel = (axis == 0) ? &vx[(size_t)i] : (axis == 1 ? &vy[(size_t)i] : &vz[(size_t)i]);
                if (*pos > 15.5f) { *pos = 15.5f; *vel = -*vel * 0.5f; }
                if (*pos < -7.5f) { *pos = -7.5f; *vel = -*vel * 0.5f; }
            }
        }
    }
    double t1 = nowMsB3();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    double wallMs = t1 - t0;
    double cpuMs = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    volatile float sink = px[10] + py[20] + pz[30];
    (void)sink;
    double seconds = wallMs / 1000.0;
    double mbps = (double)n * (double)steps / 1000000.0 / seconds;
    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", mbps);
    o.ms = wallMs;
    o.metric = buf;
    o.unit = "Mbody-steps/s";
    o.parallelism = gb7Parallelism(cpuMs, wallMs);
    return o;
}
