// ============================================================================
// CS1 第五批(之二): Structure from Motion —— 分组 "Image Synthesis"
//
// 负载内容(单线程, 数据全部在内存中生成, 不读文件/网络/环境变量, 不使用真值):
//   [计时外] 场景生成:
//     程序内合成 kScenePoints=20000 个三维点, 分布在 4 个物体表面上(球面 / 立方体 /
//     圆环 / 侧墙), 每个点带一个 3x3 的随机灰度"斑点纹理"(确定性伪随机), 用于产生
//     视角间可重复定位、且局部邻域各不相同(描述子可区分)的真实纹理;
//     10 个相机位姿沿一段圆弧(±52 度)绕物体运动, 朝向场景中心;
//     针孔模型 K=[1100,0,640; 0,1100,480; 0,0,1] 把点云投影成 10 张 1280x960 灰度图,
//     每张图叠加高斯噪声(sigma=3.5 灰阶, 查表)以及逐相机的亮度增益/偏置
//     (gain 在 [0.9,1.1], bias 在 [-8,8] 灰阶), 用来检验描述子对亮度变化的鲁棒性。
//     真值点云与真值位姿只用于第 9 步精度校验, 重建流程本身完全不使用真值。
//   [计时内]
//   1) 特征检测: 3x3 二项式平滑 -> 中心差分梯度 -> 结构张量(Ixx,Iyy,Ixy) ->
//      5x5 盒式窗(可分离滑动和)平滑 -> Harris 响应 R=det-k*trace^2 (k=0.04) ->
//      3x3 非极大值抑制(带确定性 tie-break) -> 自适应阈值(0.02*maxR) ->
//      按响应部分排序 + 12px 网格分散 -> 每图最多 kMaxFeat=900 个 -> 3x3 二次曲面
//      亚像素细化(带 det/isfinite/±0.75px 保护)。
//   2) 描述子: 旋转 BRIEF(ORB 式), 256 bit = 32 字节 = 4 个 uint64;
//      256 组采样点对由固定种子伪随机生成(近似高斯, 幅度 <=10px);
//      用 patch(半径 15)内强度矩计算主方向, 把采样点对旋转到主方向后双线性采样比较;
//      比较的是"符号", 对正的亮度增益与偏置不变 -> 对亮度变化鲁棒。
//   3) 匹配: 只匹配 |i-j|<=2 的视角对(10 相机 -> 17 对, 与真实 SfM 只匹配有重叠的
//      视角一致); 暴力 Hamming 匹配, 4 个 64bit 字分块计算并在"部分距离 >= 当前
//      次近邻"时提前退出(精确剪枝, 不改变结果); Lowe 比值检验 0.8(整数比较
//      5*best < 4*second) + 双向互惠检验(反向最近邻必须回到同一点)。
//   4) 几何验证: 归一化(Hartley)八点法 + RANSAC 固定 200 次迭代(固定种子 -> 确定性)
//      + Sampson 距离 2px 内点计数 + 全体内点重估 + 秩 2 投影(3x3 Jacobi SVD);
//      求解 9 维零空间用 A^T A 的 9x9 对称 Jacobi 特征分解(不引入任何第三方库)。
//   5) 位姿初始化: E = K^T F K -> 3x3 SVD 分解出 4 组 (R,t) -> 用 cheirality
//      (三角化点必须在两个相机前方)选一组 -> 沿相机链累乘旋转;
//      平移方向用线性平移平均: 每个视角对给出 skew(t_hat)*(t_j - R_ij t_i)=0 的
//      3 个线性方程, 固定相机 0 (t_0=0) 与一个锚点相机(取单位长度)消除规范自由度,
//      解 3*(C-2) 维稠密最小二乘。
//   6) 建 track: 对几何验证通过的匹配做并查集(节点 = 相机 x 特征), 得到多视图 track;
//      DLT(不是简单的两视图中点法): 由 2n 行 A 组成 4 元齐次方程, 取 A^T A 的 4x4
//      对称特征分解的最小特征向量; 要求所有观测相机深度 > 0.05 且平均重投影误差 < 6px。
//   7) 光束法平差: Levenberg-Marquardt + Schur 补(先消去全部三维点, 只解
//      6*(C-1)=54 维稠密相机系统, Cholesky 分解, 失败则加抖动重试; 再逐点回代);
//      相机 0 固定以消除规范自由度; 相机 6 参数用中心差分雅可比(局部参数化
//      R <- Rodrigues(dr)*R, t <- t+dt), 三维点 3 参数用解析雅可比;
//      每次迭代都真实计算残差/雅可比/法方程/阻尼/求解/评估, 步长被拒绝则回滚并放大 lambda。
//   8) 精度校验(计时外, 仅写诊断结构体): 用 Umeyama 相似变换把重建相机中心对齐到
//      真值相机中心, 输出对齐后相机中心 RMS、重建点到真值点云的最近邻 RMS、
//      BA 前后平均重投影误差、各视角对 RANSAC 内点数。
//
// metric 口径(写死在注释里, 便于人工核对):
//   Mpts/s = ( BA 三维点数 nPt * 参与 BA 的相机数 nCam * BA 迭代次数 nIter ) / 1e6 / 秒
//     nPt   = 进入 BA 的 track 数(上限 4000)
//     nCam  = 10(全部相机都进入 BA, 其中相机 0 固定不优化)
//     nIter = 实际执行的 BA 迭代次数 = LM 的"线性化-求解-评估"次数(含被拒绝的试探步),
//             上限 kBaIters=20
//     秒    = 计时区(上面 1~7 步)的墙钟时间(steady_clock, 毫秒/1000)
//   该数值越大越快, 是绝对吞吐量, 不含任何针对设备的标定系数。
//   (若按"场景三维点数"口径计算: 分子把 nPt 换成 20000, 即包含成像/匹配等全部阶段的工作量。)
//
// 诊断与日志: 结果写入文件末尾的全局结构体 gGb7SfmDiag(人工可从宿主读取), 不向
//   stdout 打印任何内容(warning: 本文件不含 printf/fprintf/hilog 调用)。
//   参考量级(单核 aarch64 手机, 1280x960 x 10 视角, 900 特征/图):
//     相邻视角对匹配数 ~200-600, RANSAC 内点率 > 60%, 有效视角对 15-17 个,
//     track 数 ~1500-4000, BA 观测 ~4000-12000;
//     BA 后平均重投影误差预期 < 1.0 px(典型 0.1-0.5 px); 若该值远大于 2 px,
//     或内点率异常低, 或对齐后相机中心 RMS 与场景尺度同量级(>0.5), 说明实现有问题。
// 保真度妥协(见文件末尾说明): 纹理是程序合成的斑点纹理而非真实照片; 相机内参已知且
//   无畸变; 未做回环检测/全局旋转平均, 位姿初值用两两相对位姿链式累乘。
//
// ============================================================================
// 【真机 0.0 Mpts/s 的定位结论 —— 用 Python 复刻整条流水线逐级量出来的】
// ============================================================================
// 现象: 真机上 metric 显示 0.0 Mpts/s, 且 unit 里没有出现 (np<=0 || baIters<=0) 的
//       失败文案 -> 说明 np>0、baIters>0, 只是 np*nCam*baIters 小于约 500。
//
// 因为本机跑不了 aarch64, 这里用 numpy 按 gb7_sfm.cpp 逐行复刻了
//   generateScene / generateCameras / renderViews / detectFeatures /
//   computeDescriptors / matchPair / estimateFundamental / relativePoseFromF /
//   triangulateDlt(含 Jacobi 特征分解的逐行移植, 已与 numpy 特征分解对过,
//   特征值相对误差 3e-15、零空间向量夹角 0.0 度)
// 跑同一条合成数据, 逐级量出来的结果是:
//
//   [1] 特征检测: 每图稳定得到 560 个特征(与 kMaxFeat 一致), 检测器没问题。
//   [2] 描述子匹配: 每个视角对 91~145 个匹配(设计注释预期 200~600, 按 560/900 折算
//       约 125~375, 量级相符)。
//   [3] 【关键】匹配的正确率只有 20%~63%(全体 1920 个匹配里 732 个落在真值极线
//       2 px 内 = 38%), 而文件顶部设计注释假设的是"内点率 > 60%"。
//   [4] 【根因】RANSAC 用的是 8 点最小样本 + 固定 200 次迭代(kRansacIters)。
//       一次采样 8 个点全为内点的概率 = 内点率^8:
//         内点率 63% -> 0.63^8 = 2.5e-2  -> 200 次迭代成功率 99.4%  (能成)
//         内点率 38% -> 0.38^8 = 4.4e-4  -> 200 次迭代成功率  8.4%  (基本不成)
//         内点率 20% -> 0.20^8 = 2.6e-6  -> 200 次迭代成功率  0.05% (完全不成)
//       实测: 把每个视角对估计出来的 F 拿去和真值做对比(用理想对应点算 Sampson
//       距离, 真值 F 上是 0.00 px), 估计出来的 F 有 3~117 px 的误差 —— 也就是
//       每个视角对的 F 都是错的。迭代次数从 200 加到 2000 也只是从 3~83 px
//       改善到 3~55 px, 仍然是错的(因为按上面的概率, 2000 次也只够一部分视角对
//       抽到干净样本)。
//   [5] 连锁后果: F 错 -> E = K^T F K 错 -> relativePoseFromF 的四组 (R,t) 全错
//       -> 旋转链 rels[m].R 与平移平均全错 -> 相机位姿没有一个是可用的
//       -> 第 6 步 DLT 三角化的"所有相机深度 > 0.05"几乎处处不成立 -> 只剩个位数的
//       三维点 -> np 极小 -> metric 显示 0.00, 而 (np<=0 || baIters<=0) 又不成立,
//       于是 unit 里连一句失败原因都没有。这与真机现象完全一致。
//
// 结论: 三角化本身(DLT/深度判据/重投影阈值)没有发现退化或写错; 问题在它上游的
//   "由匹配估计 F" 这一步 —— 8 点 RANSAC 的迭代预算(kRansacIters=200)对当前
//   匹配正确率(38%)来说差了两个数量级。文件顶部"内点率 > 60%"的设计假设与
//   实际数据不符(特征数从 900 标定到 560 之后匹配更少、更模糊, 正确率进一步下降)。
//
// ---------------------------------------------------------------------------
// 【决定性的对照实验: 只差"采样质量"这一步】
// ---------------------------------------------------------------------------
// 把 8 个点样本换成"真值内点匹配"(即用真值极线筛出来的匹配)之后, 解出来的 F 在
// 独立的理想对应点集上的中位 Sampson 误差:
//     (0,1) 0.400px   (2,4) 0.390px   (4,5) 0.114px
//     (7,8) 1.858px   (8,9) 0.426px   (1,3) 0.265px
//   (对照组: 直接用 8 组理想对应点 -> 0.000000px)
// 也就是说: **求解器本身没问题, 这组匹配也完全能约束住 F; 唯一缺的就是"抽到干净
// 样本"这件事本身**。而 8 点最小样本 + 38% 正确率的组合, 200 次迭代只有 8.4% 的
// 成功率 —— 这是纯算术, 与实现无关。
//
// ---------------------------------------------------------------------------
// 【已实测并否决的两条改法(不要重复尝试)】
// ---------------------------------------------------------------------------
// (c) 缩小描述子 patch / 加大特征最小间距 —— 实测没有帮助, 反而略差, 而且会把
//     某些视角检出的特征数压到 560 以下(等于改了工作量), 因此没有改:
//       patchR=15 minSep=10px  特征 560/560  匹配 1920  正确率 38.1%   <- 现状
//       patchR=10 minSep=20px  特征 484/560  匹配 1616  正确率 33.4%
//       patchR=12 minSep=24px  特征 400/478  匹配 1323  正确率 33.9%
//       patchR= 8 minSep=16px  特征 560/560  匹配 1693  正确率 32.7%
//     原因: patch 变小后描述子的区分度下降, 收益被抵消; 而最小间距变大直接减少了
//     可用特征数。原假设("相邻描述子高度相关导致误匹配")在实测里不成立。
// (b) PROSAC 式引导采样(按描述子 Hamming 距离从好到差逐层扩样, kRansacIters 仍是
//     200) —— 实测只能把 0/17 个视角对救到 1~3/17 个, 中位 F 误差仍有 16~25px,
//     端到端指标反而变差:
//       均匀采样 8点RANSAC x200: 有效视角对 14/17  track 451  三角化 243  保留 207
//                                BA 前平均重投影 8.136px   metric≈0.041 Mpts/s
//       PROSAC m0=16 geomhalf  : 有效视角对 16/17  track 437  三角化 140  保留 109
//                                BA 前平均重投影 4.377px   metric≈0.022 Mpts/s
//     (另外试了 geomall / linearall / geom75 / fixedfrac 0.25~0.5 等调度, 都是
//      1~3/17, 中位误差 16~25px, 没有量级差别。)
//     原因: 描述子距离对"是否几何正确"的区分力只在前 ~10 个匹配上有效, 而 8 点样本
//     需要从一个足够大的池子里抽 8 个 —— 池子一放大正确率就掉到 40%~60%,
//     0.5^8≈3.9e-3, 200 次迭代仍然不够。
//
// ---------------------------------------------------------------------------
// 【结论与唯一可行的修法】
// ---------------------------------------------------------------------------
// 在"不改工作量常量、不放水阈值"的约束下, 这个负载无法重建出上千个点。上限不是
// 三角化、不是 DLT、不是内参, 而是"8 点最小样本 x 200 次迭代 vs 38% 匹配正确率"。
// 按 99% 成功率反算需要的迭代次数:
//     8 点最小样本 (F, 线性八点法): ceil(log(0.01)/log(1-0.38^8)) ≈ 10500 次
//     7 点最小样本 (F)             : ≈ 2100 次
//     5 点最小样本 (E, K 已知)     : ≈ 340 次
// 因此若要真正修好, 必须动下面之一(都超出"让实现自洽"的范围, 需要另行批准):
//   (a) kRansacIters 200 -> ~10500; 或
//   (b) 把最小样本从 8 点改成 5 点(Nister 五点法, K 已知), 配 (a) 的 340 次迭代;
//       这同时是标定相机下更正确的做法(现在用像素坐标估 F 再转 E, 白扔了内参已知
//       这个条件, 这也是必须用 8 点而不是 5 点的原因)。
// 在那之前, 本文件保持"报出退化结果"的行为: 见下面 gb7RunStructureFromMotion
// 里 rate < 0.005 时往 unit 写的那行摘要。
// ============================================================================

#include "gb7.h"
#include "gb7_parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 全局诊断结构体(供人工/宿主判断重建是否可信; 不参与计时)
// ---------------------------------------------------------------------------
struct Gb7SfmDiag {
    int    scenePoints;        // 真值三维点数
    int    cameras;            // 相机数
    long long featuresTotal;   // 检测到的特征总数(含描述子)
    long long pairsTried;      // 参与匹配的视角对数
    long long matchesTotal;    // 比值+互惠检验后的匹配总数
    long long inliersTotal;    // 各视角对 RANSAC 内点之和
    long long tracks;          // 并查集得到的 track 数(观测数 >= 2)
    long long triangulated;    // 三角化成功(深度为正、误差达标)的三维点数
    int    baPoints;           // 进入 BA 的三维点数
    int    baCameras;          // 进入 BA 的相机数
    int    baIterations;       // BA 实际迭代次数
    int    baObservations;     // BA 观测数
    double baRmseBefore;       // BA 前平均重投影误差(像素)
    double baRmseAfter;        // BA 后平均重投影误差(像素)
    double truthCenterRms;     // 对齐后相机中心 RMS(场景单位)
    double truthPointRms;      // 对齐后重建点到真值点云最近邻 RMS(场景单位)
    double metricMpts;         // metric 数值(Mpts/s)
};
Gb7SfmDiag gGb7SfmDiag = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0};

namespace {

// ---------------------------------------------------------------- 常量
constexpr int    kImgW          = 1280;
constexpr int    kImgH          = 960;
constexpr int    kCamCount      = 10;
constexpr int    kScenePointNum = 20000;
// ======================== 工作量标定(首次真机复核后) ========================
// 每图特征数 900 -> 560(仍在本项设计区间 500~2000 内)。
// metric 口径沿用原有公式: Mpts/s = (重建点数 x 相机数 x 捆绑调整实际迭代数) / 秒数,
//   定义见第 48 行起的口径注释; 本项未锚定, 只做耗时对齐, 不编造锚点。
// 调整前: 10 相机 x 900 特征, 实测耗时约 4.2 s(超 1.5~3.0 s 上限)。
// 调整依据: 检测/描述/匹配的工作量与特征数成正比, 重建点数(metric 分子)也近似正比于
//   特征数, 因此"耗时"与"metric"同倍变化 —— 把特征数按 560/900 ≈ 0.62 缩放, 预计耗时
//   ≈ 4.2 x 0.62 ≈ 2.6 s(落在区间中部), 而吞吐 Mpts/s 基本不变(分子分母同倍)。
//   (该外推假设"捆绑调整的花费同样正比于特征数": BA 的观测数来自 tracks, 而 tracks
//    由特征数决定, 所以这个假设在本实现里成立。)
// ==================== 工作量还原(2026-10-05): 560 -> 560(撤销上一轮的 560 -> 380) =========
// 依据: 用户指令"务必跑满负载" —— 上一轮为把单核压进 1.5~3.0 s 而调小的 5 项, 一律还原成
//   调小之前的尺寸。本项上一轮把每图特征数 560 -> 380。
//   调小前实测(run 1791098115489-82335 单核第 15 项): 10 相机 x 560 特征 -> **3713.8 ms**,
//   metric 打印 0.01 Mpts/s。
//   还原后预计 ≈ **3713.8 ms**(kMaxFeat 是硬上限且检测器稳定打满它, 因此它同时线性控制
//   特征检测/描述子的像素级工作量、匹配对数、RANSAC 内点数、track 数与 BA 的观测数;
//   每一级算法本身一字未改, 也没有跳过任何一级)。
//   本项未计分(gb7.cpp 的 scored=false, k=0), metric = 三维点数 x 相机数 x BA 迭代数
//   /1e6/秒 的口径未改, 因此规模变化不影响任何分数。
//   本项不在官方多核 8 项里, 没有"多核尺寸"需要还原(kMaxFeat 是全局常量, 不按线程分支)。
// ==================== 工作量标定(2026-10-04 第三次真机复核: 560 -> 380, 保留备查) ============
// 本轮真机实测(CS1 单核阶段第 15 项, runlog.jsonl run 1791098115489-82335):
//   10 相机 x 560 特征, o.ms = **3713.8 ms**, metric 打印 0.01 Mpts/s, unit 是干净的
//   "Mpts/s"(不是 degenerate/failed 分支) -> 说明 0.005 <= rate < 0.015,
//   即分子 = 三维点数 x 10 相机 x 20 次 BA ≈ 0.01 x 3.7138 = 0.037 Mpts(约 185 个点)。
// 问题: 3713.8 ms 超出 1.5~3.0 s 上限 24%。
// 调整: 每图特征数 560 -> **380(kMaxFeat 是硬上限**且检测器稳定打满它 ——
//   见本文件第 81 行的复核结论"每图稳定得到 560 个特征(与 kMaxFeat 一致)",
//   因此它同时线性控制: 特征检测/描述子的像素级工作量、匹配对数、RANSAC 内点数、
//   track 数与 BA 的观测数 —— 也就是整条流水线的每一级)。
//   线性推演(纯比例, 不含任何设备系数): 380 / 560 = 0.6786;
//     3713.8 x 0.6786 = **2520 ms**, 落在 1.5~3.0 s 区间中部。
//   注意: 减少的是重建规模(更少的特征 -> 更少的匹配/更少的 track/更小的 BA),
//   每一级算法本身一字未改, 也没有跳过任何一级; 这不属于"空转凑时间"的反面 ——
//   它是把"做多少真实重建工作"调到目标区间。
//   本项未计分(gb7.cpp 的 scored=false, k=0), metric = 三维点数 x 相机数 x BA 迭代
//   /1e6/秒 的口径未改, 因此规模变化不影响任何分数。
// =========================================================================================
constexpr int    kMaxFeat       = 560;    // 每图特征数(唯一线性旋钮; 560 实测 3713.8 ms; 还原 380->560)
constexpr int    kFeatMargin    = 18;     // 特征到图像边界的最小距离(保证描述子采样安全)
constexpr int    kDescBits      = 256;    // 32 字节描述子
constexpr int    kDescWords     = 4;
constexpr int    kPatchRadius   = 15;
constexpr int    kMatchJump     = 2;      // 匹配 |i-j| <= 2
constexpr int    kRansacIters   = 200;
constexpr int    kMaxTracks     = 4000;
constexpr int    kBaIters       = 20;
constexpr double kInlierPixel   = 2.0;
constexpr double kFocal         = 1100.0;
constexpr double kCx            = 640.0;
constexpr double kCy            = 480.0;
constexpr double kJacStep       = 1e-6;
constexpr double kNoiseSigma    = 3.5;
constexpr int    kNoiseTabSize  = 4096;
constexpr int    kBlobKerSize   = 128;

// ---------------------------------------------------------------- 基础工具
double nowMsSfm()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

inline uint32_t xsSfm(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline double uniSfm(uint32_t& s)
{
    return (double)(xsSfm(s) >> 8) * (1.0 / 16777216.0);   // [0,1)
}

// 把 tasks 个互相独立的任务交给 gb7ParallelFor。
// gb7ParallelFor 的块粒度固定为 64: 直接以任务数作索引规模时, 任务数 < 64(例如 17 个
// 视角对)会让全部工作落到单线程。这里把索引空间放大 64 倍, 因块起止点始终是 64 的
// 整数倍, 每个线程收到的区间都对齐到整任务边界 -> 任务级动态负载均衡。
// threads <= 1 时 gb7ParallelFor 直接以 body(0, tasks) 调用, 与串行循环逐位一致。
inline void gb7SfmParTasks(int threads, long long tasks, const std::function<void(long long, long long)>& body)
{
    if (tasks <= 0) {
        return;
    }
    if ((long long)threads < 1) {
        threads = 1;
    }
    gb7ParallelFor(threads, tasks * 64, [&body](long long s, long long e) {
        body(s / 64, e / 64);
    });
}

// 精确的 2*pi
inline double twoPiSfm() { return 6.283185307179586476925286766559; }

void fillNoiseTable(std::vector<double>& tab, int n, uint32_t seed)
{
    tab.resize((size_t)n);
    uint32_t s = seed | 1u;
    for (int i = 0; i < n; ++i) {
        double u1 = uniSfm(s);
        if (u1 < 1e-12) { u1 = 1e-12; }
        double u2 = uniSfm(s);
        double g = std::sqrt(-2.0 * std::log(u1)) * std::cos(twoPiSfm() * u2);
        if (!std::isfinite(g)) { g = 0.0; }
        tab[(size_t)i] = g;
    }
}

// 无 intrinsics 的 64bit popcount(SWAR)
inline uint32_t popcount64(uint64_t v)
{
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (uint32_t)((v * 0x0101010101010101ull) >> 56);
}

// ---------------------------------------------------------------- 3x3 线性代数
inline void mat3Identity(double* M)
{
    M[0] = 1.0; M[1] = 0.0; M[2] = 0.0;
    M[3] = 0.0; M[4] = 1.0; M[5] = 0.0;
    M[6] = 0.0; M[7] = 0.0; M[8] = 1.0;
}

inline void mat3Mul(const double* A, const double* B, double* C)   // C = A*B (允许 C 与 A/B 同一指针)
{
    double r[9];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i * 3 + j] = A[i * 3 + 0] * B[0 * 3 + j]
                         + A[i * 3 + 1] * B[1 * 3 + j]
                         + A[i * 3 + 2] * B[2 * 3 + j];
        }
    }
    for (int k = 0; k < 9; ++k) { C[k] = r[k]; }
}

inline void mat3MulTVec(const double* A, const double* v, double* out)   // out = A^T * v
{
    double r[3];
    r[0] = A[0] * v[0] + A[3] * v[1] + A[6] * v[2];
    r[1] = A[1] * v[0] + A[4] * v[1] + A[7] * v[2];
    r[2] = A[2] * v[0] + A[5] * v[1] + A[8] * v[2];
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

inline void mat3MulVec(const double* A, const double* v, double* out)    // out = A * v
{
    double r[3];
    r[0] = A[0] * v[0] + A[1] * v[1] + A[2] * v[2];
    r[1] = A[3] * v[0] + A[4] * v[1] + A[5] * v[2];
    r[2] = A[6] * v[0] + A[7] * v[1] + A[8] * v[2];
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

inline double mat3Det(const double* A)
{
    return A[0] * (A[4] * A[8] - A[5] * A[7])
         - A[1] * (A[3] * A[8] - A[5] * A[6])
         + A[2] * (A[3] * A[7] - A[4] * A[6]);
}

// 3x3 求逆(伴随矩阵法), 失败(奇异)返回 false
bool invert3(const double* A, double* out)
{
    double d = mat3Det(A);
    if (!std::isfinite(d) || std::fabs(d) < 1e-300) { return false; }
    double id = 1.0 / d;
    double r[9];
    r[0] = (A[4] * A[8] - A[5] * A[7]) * id;
    r[1] = (A[2] * A[7] - A[1] * A[8]) * id;
    r[2] = (A[1] * A[5] - A[2] * A[4]) * id;
    r[3] = (A[5] * A[6] - A[3] * A[8]) * id;
    r[4] = (A[0] * A[8] - A[2] * A[6]) * id;
    r[5] = (A[2] * A[3] - A[0] * A[5]) * id;
    r[6] = (A[3] * A[7] - A[4] * A[6]) * id;
    r[7] = (A[1] * A[6] - A[0] * A[7]) * id;
    r[8] = (A[0] * A[4] - A[1] * A[3]) * id;
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(r[i])) { return false; }
    }
    for (int i = 0; i < 9; ++i) { out[i] = r[i]; }
    return true;
}

// Rodrigues: out = exp(skew(v))
void rodrigues(const double* v, double* out)
{
    const double x = v[0], y = v[1], z = v[2];
    const double th2 = x * x + y * y + z * z;
    const double th = std::sqrt(th2);
    double a = 1.0, b = 0.5;
    if (th > 1e-9) {
        a = std::sin(th) / th;
        b = (1.0 - std::cos(th)) / th2;
    }
    const double K[9] = {0.0, -z, y, z, 0.0, -x, -y, x, 0.0};
    double K2[9];
    mat3Mul(K, K, K2);
    for (int i = 0; i < 9; ++i) {
        const double id = ((i % 3) == (i / 3)) ? 1.0 : 0.0;
        out[i] = id + a * K[i] + b * K2[i];
    }
}

// 对称矩阵 Jacobi 特征分解(A: n*n 行主序, 输入输出; evals: n; evecs: n*n, 第 k 列为第 k 个特征向量)
void jacobiEigenN(double* A, int n, double* evals, double* evecs, int maxSweeps)
{
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            evecs[(size_t)i * n + j] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (int sweep = 0; sweep < maxSweeps; ++sweep) {
        double off = 0.0, diag = 0.0;
        for (int p = 0; p < n; ++p) {
            diag += std::fabs(A[(size_t)p * n + p]);
            for (int q = p + 1; q < n; ++q) {
                off += A[(size_t)p * n + q] * A[(size_t)p * n + q];
            }
        }
        double scale = (diag > 1e-300) ? diag : 1e-300;
        if (off <= 1e-26 * scale * scale) { break; }
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                double apq = A[(size_t)p * n + q];
                if (std::fabs(apq) <= 1e-300 * scale) { continue; }
                double theta = (A[(size_t)q * n + q] - A[(size_t)p * n + p]) / (2.0 * apq);
                double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                double c = 1.0 / std::sqrt(t * t + 1.0);
                double s = t * c;
                for (int k = 0; k < n; ++k) {
                    double akp = A[(size_t)k * n + p], akq = A[(size_t)k * n + q];
                    A[(size_t)k * n + p] = c * akp - s * akq;
                    A[(size_t)k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {
                    double apk = A[(size_t)p * n + k], aqk = A[(size_t)q * n + k];
                    A[(size_t)p * n + k] = c * apk - s * aqk;
                    A[(size_t)q * n + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    double vkp = evecs[(size_t)k * n + p], vkq = evecs[(size_t)k * n + q];
                    evecs[(size_t)k * n + p] = c * vkp - s * vkq;
                    evecs[(size_t)k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int i = 0; i < n; ++i) { evals[i] = A[(size_t)i * n + i]; }
}

// 3x3 矩阵的 SVD: M = U * diag(s) * V^T, s 降序
void svd3(const double* M, double* U, double* s, double* V)
{
    double MtM[9];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) { sum += M[k * 3 + i] * M[k * 3 + j]; }
            MtM[i * 3 + j] = sum;
        }
    }
    double ev[3], evec[9];
    double A[9];
    for (int i = 0; i < 9; ++i) { A[i] = MtM[i]; }
    jacobiEigenN(A, 3, ev, evec, 20);
    int ord[3] = {0, 1, 2};
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (ev[ord[j]] > ev[ord[i]]) { std::swap(ord[i], ord[j]); }
        }
    }
    for (int i = 0; i < 3; ++i) {
        double lam = ev[ord[i]];
        s[i] = (lam > 0.0) ? std::sqrt(lam) : 0.0;
        for (int k = 0; k < 3; ++k) { V[k * 3 + i] = evec[k * 3 + ord[i]]; }
    }
    // U = M V / s (对 s>0 的列); s==0 的列用叉积补全, 保证正交
    double u0[3], u1[3], u2[3];
    for (int k = 0; k < 3; ++k) {
        double a0 = 0.0, a1 = 0.0;
        for (int m = 0; m < 3; ++m) {
            a0 += M[k * 3 + m] * V[m * 3 + 0];
            a1 += M[k * 3 + m] * V[m * 3 + 1];
        }
        u0[k] = (s[0] > 1e-300) ? a0 / s[0] : 0.0;
        u1[k] = (s[1] > 1e-300) ? a1 / s[1] : 0.0;
    }
    u2[0] = u0[1] * u1[2] - u0[2] * u1[1];
    u2[1] = u0[2] * u1[0] - u0[0] * u1[2];
    u2[2] = u0[0] * u1[1] - u0[1] * u1[0];
    double n2 = std::sqrt(u2[0] * u2[0] + u2[1] * u2[1] + u2[2] * u2[2]);
    if (!(n2 > 1e-12)) {
        // 退化: 直接构造一组正交基
        u0[0] = (s[0] > 1e-300) ? u0[0] : 1.0;
        u1[0] = (s[1] > 1e-300) ? u1[0] : 0.0;
        u1[1] = (s[1] > 1e-300) ? u1[1] : 1.0;
        u2[0] = 0.0; u2[1] = 0.0; u2[2] = 1.0;
        n2 = 1.0;
    }
    for (int k = 0; k < 3; ++k) { u2[k] /= n2; }
    for (int k = 0; k < 3; ++k) {
        U[k * 3 + 0] = u0[k];
        U[k * 3 + 1] = u1[k];
        U[k * 3 + 2] = u2[k];
    }
}

// n 元线性方程组(行主序 A, 右端 b 同时作为解返回); 高斯消元 + 部分主元
bool solveLinearN(std::vector<double>& A, std::vector<double>& b, int n)
{
    double maxAbs = 0.0;
    for (size_t i = 0; i < (size_t)n * n; ++i) {
        double v = std::fabs(A[i]);
        if (v > maxAbs) { maxAbs = v; }
    }
    const double tol = 1e-13 * (maxAbs + 1e-300);
    for (int col = 0; col < n; ++col) {
        int piv = col;
        double best = std::fabs(A[(size_t)col * n + col]);
        for (int r = col + 1; r < n; ++r) {
            double v = std::fabs(A[(size_t)r * n + col]);
            if (v > best) { best = v; piv = r; }
        }
        if (!(best > tol)) { return false; }
        if (piv != col) {
            for (int c = col; c < n; ++c) { std::swap(A[(size_t)col * n + c], A[(size_t)piv * n + c]); }
            std::swap(b[(size_t)col], b[(size_t)piv]);
        }
        const double d = A[(size_t)col * n + col];
        for (int r = col + 1; r < n; ++r) {
            const double f = A[(size_t)r * n + col] / d;
            if (f == 0.0) { continue; }
            for (int c = col; c < n; ++c) { A[(size_t)r * n + c] -= f * A[(size_t)col * n + c]; }
            b[(size_t)r] -= f * b[(size_t)col];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double sum = b[(size_t)r];
        for (int c = r + 1; c < n; ++c) { sum -= A[(size_t)r * n + c] * b[(size_t)c]; }
        const double d = A[(size_t)r * n + r];
        if (!(std::fabs(d) > tol)) { return false; }
        b[(size_t)r] = sum / d;
    }
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(b[(size_t)i])) { return false; }
    }
    return true;
}

// 对称正定矩阵 Cholesky 求解(原地分解, 解在 b 中)
bool solveCholesky(std::vector<double>& A, std::vector<double>& b, int n)
{
    double dmax = 0.0;
    for (int i = 0; i < n; ++i) {
        double v = std::fabs(A[(size_t)i * n + i]);
        if (v > dmax) { dmax = v; }
    }
    const double tol = 1e-12 * (dmax + 1e-300);
    for (int j = 0; j < n; ++j) {
        double d = A[(size_t)j * n + j];
        for (int k = 0; k < j; ++k) { d -= A[(size_t)j * n + k] * A[(size_t)j * n + k]; }
        if (!(d > tol)) { return false; }
        d = std::sqrt(d);
        A[(size_t)j * n + j] = d;
        for (int i = j + 1; i < n; ++i) {
            double s = A[(size_t)i * n + j];
            for (int k = 0; k < j; ++k) { s -= A[(size_t)i * n + k] * A[(size_t)j * n + k]; }
            A[(size_t)i * n + j] = s / d;
        }
    }
    for (int i = 0; i < n; ++i) {
        double s = b[(size_t)i];
        for (int k = 0; k < i; ++k) { s -= A[(size_t)i * n + k] * b[(size_t)k]; }
        b[(size_t)i] = s / A[(size_t)i * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = b[(size_t)i];
        for (int k = i + 1; k < n; ++k) { s -= A[(size_t)k * n + i] * b[(size_t)k]; }
        b[(size_t)i] = s / A[(size_t)i * n + i];
    }
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(b[(size_t)i])) { return false; }
    }
    return true;
}

// ---------------------------------------------------------------- 场景 / 相机 / 成像
struct ScenePoint {
    double p[3];
    double intensity;      // 0.55 ~ 1.0
    double texel[9];       // 3x3 斑点纹理(0.2 ~ 1.0)
};

struct Camera {
    double R[9];           // 行主序: Xc = R * X + t
    double t[3];
    double gain, bias;     // 成像用亮度变化(重建流程不使用)
    double eye[3];         // 真值相机中心(仅诊断用)
};

void generateScene(std::vector<ScenePoint>& pts)
{
    pts.clear();
    pts.reserve((size_t)kScenePointNum);
    uint32_t s = 0x51ED2701u;
    const double tp = twoPiSfm();

    // 1) 球面: 6000 点, 中心 (-0.60, 0.55, 0.10), 半径 0.45
    for (int i = 0; i < 6000; ++i) {
        const double u = uniSfm(s) * 2.0 - 1.0;
        const double ph = uniSfm(s) * tp;
        const double rr = std::sqrt(std::max(0.0, 1.0 - u * u));
        ScenePoint p;
        p.p[0] = -0.60 + 0.45 * rr * std::cos(ph);
        p.p[1] =  0.55 + 0.45 * u;
        p.p[2] =  0.10 + 0.45 * rr * std::sin(ph);
        p.intensity = 0.55 + 0.45 * uniSfm(s);
        for (int k = 0; k < 9; ++k) {
            p.texel[k] = 0.20 + 0.80 * uniSfm(s);
        }
        pts.push_back(p);
    }
    // 2) 立方体: 6000 点, 中心 (0.55, 0.50, -0.10), 半边长 0.42, 绕 y 轴转 0.45rad
    const double ca = std::cos(0.45), sa = std::sin(0.45);
    for (int i = 0; i < 6000; ++i) {
        const int face = (int)(xsSfm(s) % 6u);
        const double a = (uniSfm(s) * 2.0 - 1.0) * 0.42;
        const double b = (uniSfm(s) * 2.0 - 1.0) * 0.42;
        double q[3] = {0.0, 0.0, 0.0};
        if (face == 0)      { q[0] =  0.42; q[1] = a; q[2] = b; }
        else if (face == 1) { q[0] = -0.42; q[1] = a; q[2] = b; }
        else if (face == 2) { q[1] =  0.42; q[0] = a; q[2] = b; }
        else if (face == 3) { q[1] = -0.42; q[0] = a; q[2] = b; }
        else if (face == 4) { q[2] =  0.42; q[0] = a; q[1] = b; }
        else                { q[2] = -0.42; q[0] = a; q[1] = b; }
        ScenePoint p;
        p.p[0] = 0.55 + (ca * q[0] + sa * q[2]);
        p.p[1] = 0.50 + q[1];
        p.p[2] = -0.10 + (-sa * q[0] + ca * q[2]);
        p.intensity = 0.55 + 0.45 * uniSfm(s);
        for (int k = 0; k < 9; ++k) {
            p.texel[k] = 0.20 + 0.80 * uniSfm(s);
        }
        pts.push_back(p);
    }
    // 3) 圆环: 5000 点, 中心 (-0.02, -0.45, 0.15), 大半径 0.38, 小半径 0.14(轴为 z)
    for (int i = 0; i < 5000; ++i) {
        const double th = uniSfm(s) * tp;
        const double ph = uniSfm(s) * tp;
        const double rad = 0.38 + 0.14 * std::cos(ph);
        ScenePoint p;
        p.p[0] = -0.02 + rad * std::cos(th);
        p.p[1] = -0.45 + rad * std::sin(th);
        p.p[2] =  0.15 + 0.14 * std::sin(ph);
        p.intensity = 0.55 + 0.45 * uniSfm(s);
        for (int k = 0; k < 9; ++k) {
            p.texel[k] = 0.20 + 0.80 * uniSfm(s);
        }
        pts.push_back(p);
    }
    // 4) 侧墙: 3000 点, 平面 z=-1.15, x in [-1.4,1.4], y in [-1.1,1.1]
    for (int i = 0; i < 3000; ++i) {
        ScenePoint p;
        p.p[0] = -1.40 + 2.80 * uniSfm(s);
        p.p[1] = -1.10 + 2.20 * uniSfm(s);
        p.p[2] = -1.15;
        p.intensity = 0.45 + 0.40 * uniSfm(s);
        for (int k = 0; k < 9; ++k) {
            p.texel[k] = 0.25 + 0.75 * uniSfm(s);
        }
        pts.push_back(p);
    }
}

void generateCameras(std::vector<Camera>& cams)
{
    cams.assign((size_t)kCamCount, Camera());
    uint32_t s = 0x2F1B3C5Du;
    const double radius = 4.30;
    const double target[3] = {0.0, 0.0, -0.15};
    const double upW[3] = {0.0, 1.0, 0.0};
    for (int c = 0; c < kCamCount; ++c) {
        const double frac = (double)c / (double)(kCamCount - 1);       // 0..1
        const double theta = -0.9076 + frac * 1.8152;                  // ±52 度
        double eye[3];
        eye[0] = radius * std::sin(theta);
        eye[1] = 0.12 * std::sin(theta * 2.0) + 0.05;
        eye[2] = radius * std::cos(theta);
        // 视线
        double fwd[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        double fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (!(fl > 1e-9)) { fl = 1.0; }
        fwd[0] /= fl; fwd[1] /= fl; fwd[2] /= fl;
        // 图像 x 轴: right = up x fwd (右手系: right x up = fwd)
        double rt[3] = {upW[1] * fwd[2] - upW[2] * fwd[1],
                        upW[2] * fwd[0] - upW[0] * fwd[2],
                        upW[0] * fwd[1] - upW[1] * fwd[0]};
        double rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
        if (!(rl > 1e-9)) { rt[0] = 1.0; rt[1] = 0.0; rt[2] = 0.0; rl = 1.0; }
        rt[0] /= rl; rt[1] /= rl; rt[2] /= rl;
        // 图像 y 轴: up = fwd x right
        double up[3] = {fwd[1] * rt[2] - fwd[2] * rt[1],
                        fwd[2] * rt[0] - fwd[0] * rt[2],
                        fwd[0] * rt[1] - fwd[1] * rt[0]};
        Camera& cam = cams[(size_t)c];
        // 相机坐标系: xc = right.(X-eye), yc = up.(X-eye), zc = fwd.(X-eye) -> 前方 zc > 0
        cam.R[0] = rt[0];  cam.R[1] = rt[1];  cam.R[2] = rt[2];
        cam.R[3] = up[0];  cam.R[4] = up[1];  cam.R[5] = up[2];
        cam.R[6] = fwd[0]; cam.R[7] = fwd[1]; cam.R[8] = fwd[2];
        // t = -R * eye
        cam.t[0] = -(cam.R[0] * eye[0] + cam.R[1] * eye[1] + cam.R[2] * eye[2]);
        cam.t[1] = -(cam.R[3] * eye[0] + cam.R[4] * eye[1] + cam.R[5] * eye[2]);
        cam.t[2] = -(cam.R[6] * eye[0] + cam.R[7] * eye[1] + cam.R[8] * eye[2]);
        cam.eye[0] = eye[0]; cam.eye[1] = eye[1]; cam.eye[2] = eye[2];
        cam.gain = 0.90 + 0.20 * uniSfm(s);        // 亮度增益变化(检验描述子鲁棒性)
        cam.bias = -8.0 + 16.0 * uniSfm(s);        // 亮度偏置变化
    }
}

// 5x5 高斯斑点核(索引 = d2*8)
void makeBlobKernel(std::vector<float>& ker)
{
    ker.assign((size_t)kBlobKerSize, 0.0f);
    for (int i = 0; i < kBlobKerSize; ++i) {
        const double d2 = (double)i / 8.0;
        ker[(size_t)i] = (float)std::exp(-d2 / 2.0);   // sigma = 1 像素
    }
}

void splatBlob(float* acc, int W, int H, double u, double v, double amp, const float* ker)
{
    const int iu = (int)std::floor(u);
    const int iv = (int)std::floor(v);
    if (iu < -2 || iu > W + 1 || iv < -2 || iv > H + 1) { return; }
    const double fu = u - (double)iu;
    const double fv = v - (double)iv;
    for (int dy = -2; dy <= 2; ++dy) {
        const int py = iv + dy;
        if (py < 0 || py >= H) { continue; }
        const double ddy = (double)dy - fv;
        const double ddy2 = ddy * ddy;
        for (int dx = -2; dx <= 2; ++dx) {
            const int px = iu + dx;
            if (px < 0 || px >= W) { continue; }
            const double ddx = (double)dx - fu;
            const double d2 = ddx * ddx + ddy2;
            const int idx = (int)(d2 * 8.0 + 0.5);
            if (idx < 0 || idx >= kBlobKerSize) { continue; }
            const float val = (float)(amp * (double)ker[(size_t)idx]);
            float* a = acc + (size_t)py * W + px;
            if (val > *a) { *a = val; }
        }
    }
}

// 把点云渲染成 kCamCount 张灰度图(计时外)
void renderViews(const std::vector<ScenePoint>& pts, const std::vector<Camera>& cams,
                 std::vector<uint8_t>& images, const std::vector<float>& ker,
                 const std::vector<double>& noiseTab)
{
    const int W = kImgW, H = kImgH;
    const size_t npix = (size_t)W * (size_t)H;
    std::vector<float> acc(npix, 0.0f);
    std::vector<float> bg(npix, 0.0f);
    for (int y = 0; y < H; ++y) {
        const double ry = 22.0 + 9.0 * std::sin((double)y * 0.0085 + 0.4);
        for (int x = 0; x < W; ++x) {
            const double cx2 = 22.0 + 9.0 * std::sin((double)x * 0.0069 + 1.7);
            bg[(size_t)y * W + x] = (float)(0.5 * (ry + cx2));
        }
    }
    for (int c = 0; c < (int)cams.size(); ++c) {
        const Camera& cam = cams[(size_t)c];
        for (size_t i = 0; i < npix; ++i) { acc[i] = 0.0f; }
        for (size_t i = 0; i < pts.size(); ++i) {
            const ScenePoint& p = pts[i];
            const double xc = cam.R[0] * p.p[0] + cam.R[1] * p.p[1] + cam.R[2] * p.p[2] + cam.t[0];
            const double yc = cam.R[3] * p.p[0] + cam.R[4] * p.p[1] + cam.R[5] * p.p[2] + cam.t[1];
            const double zc = cam.R[6] * p.p[0] + cam.R[7] * p.p[1] + cam.R[8] * p.p[2] + cam.t[2];
            if (!(zc > 0.2)) { continue; }
            const double iz = 1.0 / zc;
            const double u = kFocal * xc * iz + kCx;
            const double v = kFocal * yc * iz + kCy;
            if (u < -4.0 || u > (double)W + 3.0 || v < -4.0 || v > (double)H + 3.0) { continue; }
            const double amp = 210.0 * p.intensity;
            for (int ty = -1; ty <= 1; ++ty) {
                for (int tx = -1; tx <= 1; ++tx) {
                    const double tex = p.texel[(size_t)((ty + 1) * 3 + (tx + 1))];
                    if (tex <= 0.001) { continue; }
                    splatBlob(acc.data(), W, H, u + (double)tx, v + (double)ty, amp * tex, ker.data());
                }
            }
        }
        uint8_t* img = images.data() + (size_t)c * npix;
        uint32_t ns = 0x9E3779B9u + (uint32_t)c * 0x85EBCA6Bu;
        if (ns == 0u) { ns = 1u; }
        for (size_t i = 0; i < npix; ++i) {
            const uint32_t r = xsSfm(ns);
            const double n = noiseTab[(size_t)(r & (uint32_t)(kNoiseTabSize - 1))];
            double val = ((double)acc[i] + (double)bg[i]) * cam.gain + cam.bias + n * kNoiseSigma;
            if (!(val > 0.0)) { val = 0.0; }
            if (val > 255.0) { val = 255.0; }
            img[i] = (uint8_t)val;
        }
    }
}

// ---------------------------------------------------------------- 特征检测
struct FeatSet {
    int n = 0;
    std::vector<float> x, y, resp, angle;
    std::vector<uint64_t> desc;     // n * kDescWords
};

struct Cand {
    float r;
    int idx;
};

inline bool candGreater(const Cand& a, const Cand& b)
{
    if (a.r != b.r) { return a.r > b.r; }
    return a.idx < b.idx;
}

struct HarrisWorkspace {
    std::vector<float> t1, t2, t3, resp, tmp;
    std::vector<uint8_t> blur;
    std::vector<int> grid;
    std::vector<Cand> cand;
    void init()
    {
        const size_t n = (size_t)kImgW * (size_t)kImgH;
        t1.assign(n, 0.0f); t2.assign(n, 0.0f); t3.assign(n, 0.0f);
        resp.assign(n, 0.0f); tmp.assign(n, 0.0f);
        blur.assign(n, 0);
        grid.assign((size_t)((kImgW + 11) / 12) * (size_t)((kImgH + 11) / 12), -1);
        cand.reserve(20000);
    }
};

// 5x5 盒式窗(可分离滑动和), a 原地输出, tmp 为同尺寸临时缓冲。
// 行方向: 每行只读 a 的同一行、只写 tmp 的同一行 -> 按行分解;
// 列方向: 每列只读 tmp 的同一列、只写 a 的同一列 -> 按列分解。
// 每个输出只依赖自己那一行/列的滑动和, 因此任意线程数下结果与串行逐位一致。
void boxFilter5(std::vector<float>& a, std::vector<float>& tmp, int W, int H, int threads)
{
    gb7ParallelFor(threads, (long long)H, [&a, &tmp, W](long long ys, long long ye) {
        for (long long y = ys; y < ye; ++y) {
            const float* row = a.data() + (size_t)y * W;
            float* out = tmp.data() + (size_t)y * W;
            double sum = 0.0;
            for (int k = -2; k <= 2; ++k) {
                int xx = (k < 0) ? 0 : k;
                if (xx > W - 1) { xx = W - 1; }
                sum += (double)row[xx];
            }
            out[0] = (float)sum;
            for (int x = 1; x < W; ++x) {
                int add = x + 2; if (add > W - 1) { add = W - 1; }
                int sub = x - 3; if (sub < 0) { sub = 0; }
                sum += (double)row[add] - (double)row[sub];
                out[x] = (float)sum;
            }
        }
    });
    gb7ParallelFor(threads, (long long)W, [&a, &tmp, W, H](long long xs, long long xe) {
        for (long long x = xs; x < xe; ++x) {
            double sum = 0.0;
            for (int k = -2; k <= 2; ++k) {
                int yy = (k < 0) ? 0 : k;
                if (yy > H - 1) { yy = H - 1; }
                sum += (double)tmp[(size_t)yy * W + x];
            }
            a[(size_t)0 * W + x] = (float)sum;
            for (int y = 1; y < H; ++y) {
                int add = y + 2; if (add > H - 1) { add = H - 1; }
                int sub = y - 3; if (sub < 0) { sub = 0; }
                sum += (double)tmp[(size_t)add * W + x] - (double)tmp[(size_t)sub * W + x];
                a[(size_t)y * W + x] = (float)sum;
            }
        }
    });
}

// 说明: HarrisWorkspace(hw) 是图像级的共享暂存(约 26MB), 为避免"每线程复制大缓冲",
// 相机之间不做并行(否则每个线程都要一份 hw); 这里把 detectFeatures 内部各阶段
// 天然独立的部分按行/按像素分解 —— 各阶段共用同一份 hw, 但每个阶段写的是互不重叠的
// 输出区, 且阶段之间由 gb7ParallelFor 的 join 形成屏障, 因此无数据竞争。
void detectFeatures(const uint8_t* img, HarrisWorkspace& ws, FeatSet& fs, int threads)
{
    if (threads < 1) { threads = 1; }
    const int W = kImgW, H = kImgH;

    // 1) 3x3 二项式平滑(边界 clamp): 每行只读 img 相邻三行、只写 blur 的同一行
    gb7ParallelFor(threads, (long long)H, [&ws, img](long long ys, long long ye) {
    for (long long y = ys; y < ye; ++y) {
        const int ym = (y > 0) ? (int)(y - 1) : 0;
        const int yp = (y < H - 1) ? (int)(y + 1) : (H - 1);
        const uint8_t* r0 = img + (size_t)ym * W;
        const uint8_t* r1 = img + (size_t)y * W;
        const uint8_t* r2 = img + (size_t)yp * W;
        uint8_t* out = ws.blur.data() + (size_t)y * W;
        for (int x = 0; x < W; ++x) {
            const int xm = (x > 0) ? (x - 1) : 0;
            const int xp = (x < W - 1) ? (x + 1) : (W - 1);
            const int s = (int)r0[xm] + 2 * (int)r0[x] + (int)r0[xp]
                        + 2 * ((int)r1[xm] + 2 * (int)r1[x] + (int)r1[xp])
                        + (int)r2[xm] + 2 * (int)r2[x] + (int)r2[xp];
            out[x] = (uint8_t)((s + 8) >> 4);
        }
    }
    });

    // 2) 中心差分梯度 + 结构张量: 每行只读 blur 相邻三行、只写 t1/t2/t3 的同一行
    gb7ParallelFor(threads, (long long)H, [&ws](long long ys, long long ye) {
    for (long long y = ys; y < ye; ++y) {
        const int ym = (y > 0) ? (int)(y - 1) : 0;
        const int yp = (y < H - 1) ? (int)(y + 1) : (H - 1);
        const uint8_t* row = ws.blur.data() + (size_t)y * W;
        const uint8_t* rowm = ws.blur.data() + (size_t)ym * W;
        const uint8_t* rowp = ws.blur.data() + (size_t)yp * W;
        float* o1 = ws.t1.data() + (size_t)y * W;
        float* o2 = ws.t2.data() + (size_t)y * W;
        float* o3 = ws.t3.data() + (size_t)y * W;
        for (int x = 0; x < W; ++x) {
            const int xm = (x > 0) ? (x - 1) : 0;
            const int xp = (x < W - 1) ? (x + 1) : (W - 1);
            const float gx = 0.5f * (float)((int)row[xp] - (int)row[xm]);
            const float gy = 0.5f * (float)((int)rowp[x] - (int)rowm[x]);
            o1[x] = gx * gx;
            o2[x] = gy * gy;
            o3[x] = gx * gy;
        }
    }
    });

    // 3) 5x5 盒式窗平滑(内部再按行/列分解)
    boxFilter5(ws.t1, ws.tmp, W, H, threads);
    boxFilter5(ws.t2, ws.tmp, W, H, threads);
    boxFilter5(ws.t3, ws.tmp, W, H, threads);

    // 4) Harris 响应: 按行分解, 每行只写 resp 的同一行并维护自己的行内最大值;
    //    全局最大值 = 各行最大值的最大值(最大值与合并顺序无关, 结果与串行逐位一致)。
    std::vector<double> rowMax((size_t)H, 0.0);
    gb7ParallelFor(threads, (long long)H, [&ws, &rowMax](long long ys, long long ye) {
        for (long long y = ys; y < ye; ++y) {
            double local = 0.0;
            const size_t base = (size_t)y * (size_t)W;
            for (int x = 0; x < W; ++x) {
                const size_t i = base + (size_t)x;
                const double xx = (double)ws.t1[i];
                const double yy = (double)ws.t2[i];
                const double xy = (double)ws.t3[i];
                double r = xx * yy - xy * xy - 0.04 * (xx + yy) * (xx + yy);
                if (!(r > 0.0)) { r = 0.0; }
                ws.resp[i] = (float)r;
                if (r > local) { local = r; }
            }
            rowMax[(size_t)y] = local;
        }
    });
    double maxR = 0.0;
    for (int y = 0; y < H; ++y) {
        if (rowMax[(size_t)y] > maxR) { maxR = rowMax[(size_t)y]; }
    }

    fs.n = 0;
    fs.x.clear(); fs.y.clear(); fs.resp.clear(); fs.angle.clear(); fs.desc.clear();
    if (!(maxR > 1e-12)) { return; }

    // 5) 非极大值抑制 + 自适应阈值 + 候选收集
    const float thr = (float)(maxR * 0.02);
    const float* R = ws.resp.data();
    ws.cand.clear();
    for (int y = 2; y < H - 2; ++y) {
        for (int x = 2; x < W - 2; ++x) {
            const int i = y * W + x;
            const float r = R[i];
            if (r < thr) { continue; }
            if (x < kFeatMargin || x > W - 1 - kFeatMargin ||
                y < kFeatMargin || y > H - 1 - kFeatMargin) { continue; }
            bool ok = true;
            for (int dy = -1; dy <= 1 && ok; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) { continue; }
                    const int ni = i + dy * W + dx;
                    const float nv = R[ni];
                    if (nv > r || (nv == r && ni < i)) { ok = false; break; }
                }
            }
            if (!ok) { continue; }
            Cand cd;
            cd.r = r;
            cd.idx = i;
            ws.cand.push_back(cd);
        }
    }
    // 部分排序: 只保留响应最高的若干候选, 再全排序
    const size_t cap = (size_t)kMaxFeat * 12;
    if (ws.cand.size() > cap) {
        std::nth_element(ws.cand.begin(), ws.cand.begin() + (std::ptrdiff_t)cap, ws.cand.end(), candGreater);
        ws.cand.resize(cap);
    }
    std::sort(ws.cand.begin(), ws.cand.end(), candGreater);

    // 6) 12px 网格分散 + 3x3 二次曲面亚像素细化
    const int cell = 12;
    const int gw = (W + cell - 1) / cell;
    const int gh = (H + cell - 1) / cell;
    ws.grid.assign((size_t)gw * (size_t)gh, -1);
    for (size_t k = 0; k < ws.cand.size() && fs.n < kMaxFeat; ++k) {
        const int idx = ws.cand[k].idx;
        const int px = idx % W;
        const int py = idx / W;
        const int gx = px / cell;
        const int gy = py / cell;
        bool ok = true;
        for (int dy = -1; dy <= 1 && ok; ++dy) {
            const int yy = gy + dy;
            if (yy < 0 || yy >= gh) { continue; }
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = gx + dx;
                if (xx < 0 || xx >= gw) { continue; }
                const int fi = ws.grid[(size_t)yy * gw + xx];
                // 防御性边界: grid 槽位里存的是写入时的 fs.n(从 0 递增)。标定把每图特征数
                // 从 900 改成 560 后, 两个数字都由 kMaxFeat 推导(遍历上界 kMaxFeat、
                // 写入 fi=fs.n<kMaxFeat), 不会出现 fi >= fs.n; 但 fs.x/fs.y 是按 fs.n 增长
                // 的 vector, 一旦槽位里的下标超过当前 fs.n 就是堆越界读 —— 这里补上界检查,
                // 正常路径下该分支永不命中, 不改变任何特征/描述子/匹配结果。
                if (fi < 0 || fi >= fs.n) { continue; }
                const double ddx = (double)fs.x[(size_t)fi] - (double)px;
                const double ddy = (double)fs.y[(size_t)fi] - (double)py;
                if (ddx * ddx + ddy * ddy < 100.0) { ok = false; break; }
            }
        }
        if (!ok) { continue; }

        const size_t ii = (size_t)idx;
        const double r0 = (double)R[ii];
        const double rx = 0.5 * ((double)R[ii + 1] - (double)R[ii - 1]);
        const double ry = 0.5 * ((double)R[ii + W] - (double)R[ii - W]);
        const double rxx = (double)R[ii + 1] - 2.0 * r0 + (double)R[ii - 1];
        const double ryy = (double)R[ii + W] - 2.0 * r0 + (double)R[ii - W];
        const double rxy = 0.25 * ((double)R[ii + W + 1] - (double)R[ii + W - 1]
                                 - (double)R[ii - W + 1] + (double)R[ii - W - 1]);
        const double det = rxx * ryy - rxy * rxy;
        double ox = 0.0, oy = 0.0;
        if (std::fabs(det) > 1e-12) {
            ox = -(ryy * rx - rxy * ry) / det;
            oy = -(-rxy * rx + rxx * ry) / det;
            if (!std::isfinite(ox) || std::fabs(ox) > 0.75) { ox = 0.0; }
            if (!std::isfinite(oy) || std::fabs(oy) > 0.75) { oy = 0.0; }
        }
        fs.x.push_back((float)((double)px + ox));
        fs.y.push_back((float)((double)py + oy));
        fs.resp.push_back(ws.cand[k].r);
        ws.grid[(size_t)gy * gw + gx] = fs.n;
        ++fs.n;
    }
}

// ---------------------------------------------------------------- 描述子(旋转 BRIEF)
inline double sampleBilinear(const uint8_t* img, int W, int H, double x, double y)
{
    const double maxX = (double)(W - 2);
    const double maxY = (double)(H - 2);
    if (!(x > 0.0)) { x = 0.0; } else if (x > maxX) { x = maxX; }
    if (!(y > 0.0)) { y = 0.0; } else if (y > maxY) { y = maxY; }
    const int ix = (int)x;
    const int iy = (int)y;
    const double fx = x - (double)ix;
    const double fy = y - (double)iy;
    const uint8_t* p = img + (size_t)iy * (size_t)W + (size_t)ix;
    const double v00 = (double)p[0];
    const double v10 = (double)p[1];
    const double v01 = (double)p[W];
    const double v11 = (double)p[W + 1];
    const double a = v00 + (v10 - v00) * fx;
    const double b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fy;
}

void makeBriefPattern(std::vector<int8_t>& pat, uint32_t seed)
{
    pat.assign((size_t)kDescBits * 4, 0);
    uint32_t s = seed | 1u;
    for (int i = 0; i < kDescBits; ++i) {
        for (int k = 0; k < 4; ++k) {
            // 近似高斯(3 个均匀分布之和), 标准差约 5 像素, 幅度限制在 ±10
            const double u = (uniSfm(s) + uniSfm(s) + uniSfm(s)) * (1.0 / 3.0) - 0.5;
            double g = u * 10.0;
            if (!(g > -10.0)) { g = -10.0; }
            if (g > 10.0) { g = 10.0; }
            pat[(size_t)i * 4 + k] = (int8_t)std::lround(g);
        }
    }
}

// 按特征点分解: 每个特征只写自己的 desc[i*kDescWords .. +kDescWords) 与 angle[i],
// blur / pat 全程只读, 因此任意线程数下结果与串行逐位一致。
void computeDescriptors(const uint8_t* blur, FeatSet& fs, const std::vector<int8_t>& pat, int threads)
{
    if (threads < 1) { threads = 1; }
    const int W = kImgW, H = kImgH;
    fs.desc.assign((size_t)fs.n * kDescWords, 0ull);
    // 修复原有越界写: 下面的循环会写 fs.angle[i], 但 angle 从未被分配(空 vector, data()==nullptr,
    // 首次写入即空指针解引用)。angle 只被写、不被任何代码读取, 因此补上分配不改变任何计算结果,
    // 只是把未定义行为变成定义行为, 保证本负载能真正跑起来。
    fs.angle.assign((size_t)fs.n, 0.0f);
    const int8_t* pp = pat.data();
    const int nFeat = fs.n;
    gb7ParallelFor(threads, (long long)nFeat, [&](long long s, long long e) {
    for (long long i = s; i < e; ++i) {
        const double fx = (double)fs.x[(size_t)i];
        const double fy = (double)fs.y[(size_t)i];
        const int cx0 = (int)std::lround(fx);
        const int cy0 = (int)std::lround(fy);
        // 主方向: patch 内强度矩(ORB 式)
        double m10 = 0.0, m01 = 0.0;
        for (int dy = -kPatchRadius; dy <= kPatchRadius; ++dy) {
            const int py = cy0 + dy;
            if (py < 0 || py >= H) { continue; }
            const uint8_t* row = blur + (size_t)py * W;
            const double wy = (double)dy;
            for (int dx = -kPatchRadius; dx <= kPatchRadius; ++dx) {
                if (dx * dx + dy * dy > kPatchRadius * kPatchRadius) { continue; }
                const int px = cx0 + dx;
                if (px < 0 || px >= W) { continue; }
                const double v = (double)row[px];
                m10 += (double)dx * v;
                m01 += wy * v;
            }
        }
        double ang = 0.0;
        if (m10 != 0.0 || m01 != 0.0) { ang = std::atan2(m01, m10); }
        if (!std::isfinite(ang)) { ang = 0.0; }
        fs.angle[(size_t)i] = (float)ang;
        const double ca = std::cos(ang);
        const double sa = std::sin(ang);
        uint64_t words[kDescWords] = {0ull, 0ull, 0ull, 0ull};
        for (int b = 0; b < kDescBits; ++b) {
            const double x1 = (double)pp[(size_t)b * 4 + 0];
            const double y1 = (double)pp[(size_t)b * 4 + 1];
            const double x2 = (double)pp[(size_t)b * 4 + 2];
            const double y2 = (double)pp[(size_t)b * 4 + 3];
            const double r1x = ca * x1 - sa * y1;
            const double r1y = sa * x1 + ca * y1;
            const double r2x = ca * x2 - sa * y2;
            const double r2y = sa * x2 + ca * y2;
            const double v1 = sampleBilinear(blur, W, H, fx + r1x, fy + r1y);
            const double v2 = sampleBilinear(blur, W, H, fx + r2x, fy + r2y);
            if (v1 > v2) { words[b >> 6] |= (1ull << (uint64_t)(b & 63)); }
        }
        for (int k = 0; k < kDescWords; ++k) {
            fs.desc[(size_t)i * kDescWords + k] = words[k];
        }
    }
    });
}

// ---------------------------------------------------------------- 匹配
struct Match {
    int a;      // 相机 ia 中的特征
    int b;      // 相机 ib 中的特征
    int dist;   // Hamming 距离
};

// 暴力 Hamming 匹配(4 个 64bit 字分块 + 提前退出) + Lowe 比值 + 双向互惠
void matchPair(const FeatSet& A, const FeatSet& B, std::vector<Match>& out, std::vector<int>& fwdTmp)
{
    out.clear();
    if (A.n <= 0 || B.n <= 0) { return; }
    fwdTmp.assign((size_t)A.n, -1);
    std::vector<int> fwdDist((size_t)A.n, 0);
    for (int a = 0; a < A.n; ++a) {
        const uint64_t* da = &A.desc[(size_t)a * kDescWords];
        uint32_t best = 256u, second = 256u;
        int bi = -1;
        for (int b = 0; b < B.n; ++b) {
            const uint64_t* db = &B.desc[(size_t)b * kDescWords];
            uint32_t d = popcount64(da[0] ^ db[0]);
            if (d >= second) { continue; }
            d += popcount64(da[1] ^ db[1]);
            if (d >= second) { continue; }
            d += popcount64(da[2] ^ db[2]);
            if (d >= second) { continue; }
            d += popcount64(da[3] ^ db[3]);
            if (d >= second) { continue; }
            if (d < best) { second = best; best = d; bi = b; }
            else if (d < second) { second = d; }
        }
        // best < 0.8 * second  <=>  5*best < 4*second
        if (bi >= 0 && second < 256u && 5u * best < 4u * second) {
            fwdTmp[(size_t)a] = bi;
            fwdDist[(size_t)a] = (int)best;
        }
    }
    // 互惠检验: B 中该点的最近邻必须回到 A 中同一点
    for (int a = 0; a < A.n; ++a) {
        const int b = fwdTmp[(size_t)a];
        if (b < 0) { continue; }
        const uint64_t* db = &B.desc[(size_t)b * kDescWords];
        uint32_t best = 256u;
        int ai = -1;
        for (int aa = 0; aa < A.n; ++aa) {
            const uint64_t* da = &A.desc[(size_t)aa * kDescWords];
            uint32_t d = popcount64(da[0] ^ db[0]);
            if (d >= best) { continue; }
            d += popcount64(da[1] ^ db[1]);
            if (d >= best) { continue; }
            d += popcount64(da[2] ^ db[2]);
            if (d >= best) { continue; }
            d += popcount64(da[3] ^ db[3]);
            if (d >= best) { continue; }
            best = d;
            ai = aa;
        }
        if (ai == a) {
            Match m;
            m.a = a;
            m.b = b;
            m.dist = fwdDist[(size_t)a];
            out.push_back(m);
        }
    }
}

// ---------------------------------------------------------------- 基础矩阵(归一化八点 + RANSAC)
struct PairModel {
    int ia = -1;
    int ib = -1;
    bool valid = false;
    double F[9];
    int inliers = 0;
    std::vector<Match> matches;
    std::vector<int> inlierIdx;
};

// 秩 2 投影: 把 3x3 F 的最小奇异值置 0
void rank2Project(double* F)
{
    double A[9], ev[3], evec[9];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) { sum += F[k * 3 + i] * F[k * 3 + j]; }
            A[i * 3 + j] = sum;      // A = F^T F
        }
    }
    jacobiEigenN(A, 3, ev, evec, 20);
    int ord[3] = {0, 1, 2};
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (ev[ord[j]] > ev[ord[i]]) { std::swap(ord[i], ord[j]); }
        }
    }
    const double s0 = (ev[ord[0]] > 0.0) ? std::sqrt(ev[ord[0]]) : 0.0;
    const double s1 = (ev[ord[1]] > 0.0) ? std::sqrt(ev[ord[1]]) : 0.0;
    double u0[3], u1[3];
    for (int k = 0; k < 3; ++k) {
        double a0 = 0.0, a1 = 0.0;
        for (int m = 0; m < 3; ++m) {
            a0 += F[k * 3 + m] * evec[m * 3 + ord[0]];
            a1 += F[k * 3 + m] * evec[m * 3 + ord[1]];
        }
        u0[k] = (s0 > 1e-300) ? a0 / s0 : 0.0;
        u1[k] = (s1 > 1e-300) ? a1 / s1 : 0.0;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            F[i * 3 + j] = s0 * u0[i] * evec[j * 3 + ord[0]] + s1 * u1[i] * evec[j * 3 + ord[1]];
        }
    }
}

// 由指定对应点求 F(归一化 Hartley 坐标, 输出行主序)
bool solveFNormal(const double* nx1, const double* ny1, const double* nx2, const double* ny2,
                  const int* idx, int cnt, double* Mb, double* evb, double* evcb, double* F)
{
    if (cnt < 8) { return false; }
    for (int i = 0; i < 81; ++i) { Mb[i] = 0.0; }
    for (int k = 0; k < cnt; ++k) {
        const int i = (idx != nullptr) ? idx[k] : k;
        double row[9];
        row[0] = nx2[i] * nx1[i];
        row[1] = nx2[i] * ny1[i];
        row[2] = nx2[i];
        row[3] = ny2[i] * nx1[i];
        row[4] = ny2[i] * ny1[i];
        row[5] = ny2[i];
        row[6] = nx1[i];
        row[7] = ny1[i];
        row[8] = 1.0;
        for (int p = 0; p < 9; ++p) {
            for (int q = p; q < 9; ++q) { Mb[p * 9 + q] += row[p] * row[q]; }
        }
    }
    for (int p = 0; p < 9; ++p) {
        for (int q = p + 1; q < 9; ++q) { Mb[q * 9 + p] = Mb[p * 9 + q]; }
    }
    jacobiEigenN(Mb, 9, evb, evcb, 12);
    int mi = 0;
    for (int i = 1; i < 9; ++i) {
        if (evb[i] < evb[mi]) { mi = i; }
    }
    for (int i = 0; i < 9; ++i) { F[i] = evcb[i * 9 + mi]; }
    rank2Project(F);
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(F[i])) { return false; }
    }
    return true;
}

// Sampson 距离(归一化坐标单位)
inline double sampsonDist(const double* F, double x1, double y1, double x2, double y2)
{
    const double fx = F[0] * x1 + F[1] * y1 + F[2];
    const double fy = F[3] * x1 + F[4] * y1 + F[5];
    const double fz = F[6] * x1 + F[7] * y1 + F[8];
    const double gx = F[0] * x2 + F[3] * y2 + F[6];
    const double gy = F[1] * x2 + F[4] * y2 + F[7];
    const double num = x2 * fx + y2 * fy + fz;
    const double den = fx * fx + fy * fy + gx * gx + gy * gy;
    if (!(den > 1e-300)) { return 1e30; }
    const double d2 = (num * num) / den;
    if (!std::isfinite(d2)) { return 1e30; }
    return std::sqrt(d2);
}

struct RansacWorkspace {
    std::vector<double> nx1, ny1, nx2, ny2;
    std::vector<unsigned char> mask, bestMask;
    std::vector<int> sample, inlierIdx, bestIdx;
    double Mb[81];
    double evb[9];
    double evcb[81];
};

bool estimateFundamental(const FeatSet& A, const FeatSet& B, std::vector<Match>& matches,
                         RansacWorkspace& ws, double* Fout, int& inlierCount,
                         std::vector<int>& inliersOut, uint32_t seed)
{
    const int n = (int)matches.size();
    inlierCount = 0;
    inliersOut.clear();
    if (n < 12) { return false; }

    ws.nx1.resize((size_t)n); ws.ny1.resize((size_t)n);
    ws.nx2.resize((size_t)n); ws.ny2.resize((size_t)n);

    // Hartley 归一化
    double mx1 = 0.0, my1 = 0.0, mx2 = 0.0, my2 = 0.0;
    for (int i = 0; i < n; ++i) {
        mx1 += (double)A.x[(size_t)matches[(size_t)i].a];
        my1 += (double)A.y[(size_t)matches[(size_t)i].a];
        mx2 += (double)B.x[(size_t)matches[(size_t)i].b];
        my2 += (double)B.y[(size_t)matches[(size_t)i].b];
    }
    mx1 /= (double)n; my1 /= (double)n; mx2 /= (double)n; my2 /= (double)n;
    double d1 = 0.0, d2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double ax = (double)A.x[(size_t)matches[(size_t)i].a] - mx1;
        const double ay = (double)A.y[(size_t)matches[(size_t)i].a] - my1;
        const double bx = (double)B.x[(size_t)matches[(size_t)i].b] - mx2;
        const double by = (double)B.y[(size_t)matches[(size_t)i].b] - my2;
        d1 += std::sqrt(ax * ax + ay * ay);
        d2 += std::sqrt(bx * bx + by * by);
    }
    d1 /= (double)n;
    d2 /= (double)n;
    const double s1 = (d1 > 1e-9) ? (1.4142135623730951 / d1) : 1.0;
    const double s2 = (d2 > 1e-9) ? (1.4142135623730951 / d2) : 1.0;
    for (int i = 0; i < n; ++i) {
        ws.nx1[(size_t)i] = ((double)A.x[(size_t)matches[(size_t)i].a] - mx1) * s1;
        ws.ny1[(size_t)i] = ((double)A.y[(size_t)matches[(size_t)i].a] - my1) * s1;
        ws.nx2[(size_t)i] = ((double)B.x[(size_t)matches[(size_t)i].b] - mx2) * s2;
        ws.ny2[(size_t)i] = ((double)B.y[(size_t)matches[(size_t)i].b] - my2) * s2;
    }
    // 内点阈值(像素 -> 归一化单位)
    const double thr = kInlierPixel * 0.5 * (s1 + s2);

    ws.mask.assign((size_t)n, 0);
    ws.bestMask.assign((size_t)n, 0);
    ws.sample.assign(8, 0);
    int bestCount = 0;
    double bestF[9] = {0.0};

    uint32_t rs = seed | 1u;
    for (int it = 0; it < kRansacIters; ++it) {
        bool ok = true;
        for (int k = 0; k < 8; ++k) {
            int pick = -1;
            for (int tries = 0; tries < 32; ++tries) {
                const int candIdx = (int)((xsSfm(rs) >> 8) % (uint32_t)n);
                bool dup = false;
                for (int j = 0; j < k; ++j) {
                    if (ws.sample[(size_t)j] == candIdx) { dup = true; break; }
                }
                if (!dup) { pick = candIdx; break; }
            }
            if (pick < 0) { ok = false; break; }
            ws.sample[(size_t)k] = pick;
        }
        if (!ok) { continue; }

        double F[9];
        if (!solveFNormal(ws.nx1.data(), ws.ny1.data(), ws.nx2.data(), ws.ny2.data(),
                          ws.sample.data(), 8, ws.Mb, ws.evb, ws.evcb, F)) {
            continue;
        }
        int cnt = 0;
        for (int i = 0; i < n; ++i) {
            const double d = sampsonDist(F, ws.nx1[(size_t)i], ws.ny1[(size_t)i],
                                         ws.nx2[(size_t)i], ws.ny2[(size_t)i]);
            if (d < thr) { ws.mask[(size_t)i] = 1; ++cnt; }
            else { ws.mask[(size_t)i] = 0; }
        }
        if (cnt > bestCount) {
            bestCount = cnt;
            for (int i = 0; i < 9; ++i) { bestF[i] = F[i]; }
            ws.bestMask = ws.mask;
        }
    }
    if (bestCount < 8) { return false; }

    // 用全部内点重估一次
    ws.inlierIdx.clear();
    for (int i = 0; i < n; ++i) {
        if (ws.bestMask[(size_t)i]) { ws.inlierIdx.push_back(i); }
    }
    double Fref[9];
    bool refOk = false;
    if ((int)ws.inlierIdx.size() >= 12) {
        refOk = solveFNormal(ws.nx1.data(), ws.ny1.data(), ws.nx2.data(), ws.ny2.data(),
                             ws.inlierIdx.data(), (int)ws.inlierIdx.size(),
                             ws.Mb, ws.evb, ws.evcb, Fref);
    }
    if (refOk) {
        int cnt = 0;
        for (int i = 0; i < n; ++i) {
            const double d = sampsonDist(Fref, ws.nx1[(size_t)i], ws.ny1[(size_t)i],
                                         ws.nx2[(size_t)i], ws.ny2[(size_t)i]);
            if (d < thr) { ++cnt; }
        }
        if (cnt >= bestCount) {
            bestCount = cnt;
            for (int i = 0; i < 9; ++i) { bestF[i] = Fref[i]; }
            for (int i = 0; i < n; ++i) {
                const double d = sampsonDist(bestF, ws.nx1[(size_t)i], ws.ny1[(size_t)i],
                                             ws.nx2[(size_t)i], ws.ny2[(size_t)i]);
                ws.bestMask[(size_t)i] = (d < thr) ? 1 : 0;
            }
        }
    }

    // 反归一化: F = T2^T * Fhat * T1
    const double T1[9] = {s1, 0.0, -s1 * mx1, 0.0, s1, -s1 * my1, 0.0, 0.0, 1.0};
    const double T2[9] = {s2, 0.0, -s2 * mx2, 0.0, s2, -s2 * my2, 0.0, 0.0, 1.0};
    double tmp[9], Fpix[9];
    mat3Mul(bestF, T1, tmp);
    // Fpix = T2^T * tmp
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) { s += T2[k * 3 + i] * tmp[k * 3 + j]; }
            Fpix[i * 3 + j] = s;
        }
    }
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(Fpix[i])) { return false; }
        Fout[i] = Fpix[i];
    }

    inlierCount = 0;
    inliersOut.clear();
    for (int i = 0; i < n; ++i) {
        if (ws.bestMask[(size_t)i]) {
            inliersOut.push_back(i);
            ++inlierCount;
        }
    }
    return inlierCount >= 8;
}

// ---------------------------------------------------------------- 相对位姿 / 三角化
inline void makeProjection(const double* R, const double* t, double* P)
{
    for (int j = 0; j < 3; ++j) {
        P[0 * 4 + j] = kFocal * R[0 * 3 + j] + kCx * R[2 * 3 + j];
        P[1 * 4 + j] = kFocal * R[1 * 3 + j] + kCy * R[2 * 3 + j];
        P[2 * 4 + j] = R[2 * 3 + j];
    }
    P[0 * 4 + 3] = kFocal * t[0] + kCx * t[2];
    P[1 * 4 + 3] = kFocal * t[1] + kCy * t[2];
    P[2 * 4 + 3] = t[2];
}

// 多视图 DLT: 取 A^T A(4x4) 最小特征向量
bool triangulateDlt(const double* Ps, const double* us, const double* vs, int cnt, double* X)
{
    if (cnt < 2) { return false; }
    double A[16];
    for (int i = 0; i < 16; ++i) { A[i] = 0.0; }
    for (int k = 0; k < cnt; ++k) {
        const double* P = Ps + (size_t)k * 12;
        double a[4], b[4];
        for (int j = 0; j < 4; ++j) {
            a[j] = us[k] * P[2 * 4 + j] - P[0 * 4 + j];
            b[j] = vs[k] * P[2 * 4 + j] - P[1 * 4 + j];
        }
        for (int p = 0; p < 4; ++p) {
            for (int q = 0; q < 4; ++q) { A[p * 4 + q] += a[p] * a[q] + b[p] * b[q]; }
        }
    }
    double ev[4], evec[16];
    jacobiEigenN(A, 4, ev, evec, 16);
    int mi = 0;
    for (int i = 1; i < 4; ++i) {
        if (ev[i] < ev[mi]) { mi = i; }
    }
    const double w = evec[3 * 4 + mi];
    if (!(std::fabs(w) > 1e-14)) { return false; }
    const double iw = 1.0 / w;
    X[0] = evec[0 * 4 + mi] * iw;
    X[1] = evec[1 * 4 + mi] * iw;
    X[2] = evec[2 * 4 + mi] * iw;
    return std::isfinite(X[0]) && std::isfinite(X[1]) && std::isfinite(X[2]);
}

inline void normalizeVec3(double* v)
{
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// 由 F 得到相对位姿(相机 j 相对相机 i): E = K^T F K -> 4 组候选 -> cheirality 选择
struct RelPose {
    bool ok = false;
    double R[9];
    double t[3];       // 单位长度
};

void relativePoseFromF(const double* Fpix, const double* x1, const double* y1,
                       const double* x2, const double* y2, const std::vector<int>& inlierIdx,
                       RelPose& rel)
{
    rel.ok = false;
    const double K[9] = {kFocal, 0.0, kCx, 0.0, kFocal, kCy, 0.0, 0.0, 1.0};
    const double Kt[9] = {kFocal, 0.0, 0.0, 0.0, kFocal, 0.0, kCx, kCy, 1.0};
    double KtF[9], E[9];
    mat3Mul(Kt, Fpix, KtF);
    mat3Mul(KtF, K, E);

    double U[9], sv[3], V[9];
    svd3(E, U, sv, V);
    // E 的两个较大奇异值置为 1
    double u0[3], u1[3], v0[3], v1[3], u2[3];
    for (int k = 0; k < 3; ++k) {
        u0[k] = U[k * 3 + 0]; u1[k] = U[k * 3 + 1]; u2[k] = U[k * 3 + 2];
        v0[k] = V[k * 3 + 0]; v1[k] = V[k * 3 + 1];
    }
    // 保证 det(U)*det(V) > 0 (此时 R 才是旋转), 用 u3 的符号修正(不影响 E, 因为 sigma3=0)
    if (mat3Det(U) * mat3Det(V) < 0.0) {
        for (int k = 0; k < 3; ++k) { u2[k] = -u2[k]; }
    }
    const double W[9] = {0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    const double Wt[9] = {0.0, 1.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    double WVt[9], WtVt[9];
    // W * V^T
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s1v = 0.0, s2v = 0.0;
            for (int k = 0; k < 3; ++k) {
                s1v += W[i * 3 + k] * V[j * 3 + k];
                s2v += Wt[i * 3 + k] * V[j * 3 + k];
            }
            WVt[i * 3 + j] = s1v;
            WtVt[i * 3 + j] = s2v;
        }
    }
    double R1[9], R2[9];
    mat3Mul(U, WVt, R1);
    mat3Mul(U, WtVt, R2);
    if (mat3Det(R1) < 0.0) {
        for (int i = 0; i < 9; ++i) { R1[i] = -R1[i]; }
    }
    if (mat3Det(R2) < 0.0) {
        for (int i = 0; i < 9; ++i) { R2[i] = -R2[i]; }
    }

    // cheirality: 用内点在 4 组候选中选深度均为正的组
    const double P1[12] = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    const int maxTest = 120;
    const int nTest = ((int)inlierIdx.size() < maxTest) ? (int)inlierIdx.size() : maxTest;
    int bestCount = -1;
    for (int cand = 0; cand < 4; ++cand) {
        const double* Rc = (cand < 2) ? R1 : R2;
        const double sign = ((cand & 1) == 0) ? 1.0 : -1.0;
        double tc[3] = {sign * u2[0], sign * u2[1], sign * u2[2]};
        double P2[12];
        // 归一化坐标下的投影矩阵: P = [R | t]
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) { P2[i * 4 + j] = Rc[i * 3 + j]; }
            P2[i * 4 + 3] = tc[i];
        }
        int cnt = 0;
        for (int t = 0; t < nTest; ++t) {
            const int i = inlierIdx[(size_t)t];
            double us[2] = {x1[i], x2[i]};
            double vs[2] = {y1[i], y2[i]};
            double Ps[24];
            for (int k = 0; k < 12; ++k) { Ps[k] = P1[k]; }
            for (int k = 0; k < 12; ++k) { Ps[12 + k] = P2[k]; }
            double X[3];
            if (!triangulateDlt(Ps, us, vs, 2, X)) { continue; }
            if (!(X[2] > 1e-6)) { continue; }
            double X2[3];
            mat3MulVec(Rc, X, X2);
            X2[0] += tc[0]; X2[1] += tc[1]; X2[2] += tc[2];
            if (!(X2[2] > 1e-6)) { continue; }
            ++cnt;
        }
        if (cnt > bestCount) {
            bestCount = cnt;
            for (int i = 0; i < 9; ++i) { rel.R[i] = Rc[i]; }
            rel.t[0] = tc[0]; rel.t[1] = tc[1]; rel.t[2] = tc[2];
        }
    }
    if (bestCount <= 0) { return; }
    normalizeVec3(rel.t);
    if (!(std::fabs(mat3Det(rel.R)) > 1e-6)) { return; }
    rel.ok = true;
}

// 平移平均的目标函数: sum_over_pairs || t_ij x (t_j - R_ij t_i) ||^2
// 只约束"相机间平移的方向"(这正是相对位姿能提供的信息, 与整体尺度无关)。
// 用于在最小二乘解与链式初值之间做模型选择: 最小二乘是这一目标的最小化解, 若它
// 反而更大, 说明求解病态/塌缩, 此时必须换用链式初值, 否则后面所有三角化都退化
// (相机中心重合 -> DLT 无解 -> 0 个三维点 -> metric 0.00)。
double translationCostSfm(const std::vector<Camera>& cams, const std::vector<RelPose>& rels,
                          const std::vector<int>& relValid, const std::vector<PairModel>& models)
{
    double sum = 0.0;
    for (size_t m = 0; m < models.size(); ++m) {
        if (!relValid[m]) { continue; }
        const int ia = models[m].ia;
        const int ib = models[m].ib;
        const double* Rij = rels[m].R;
        const double* tij = rels[m].t;
        double d[3];
        for (int r = 0; r < 3; ++r) {
            d[r] = cams[(size_t)ib].t[r] - (Rij[r * 3 + 0] * cams[(size_t)ia].t[0] +
                                            Rij[r * 3 + 1] * cams[(size_t)ia].t[1] +
                                            Rij[r * 3 + 2] * cams[(size_t)ia].t[2]);
        }
        const double c0 = tij[1] * d[2] - tij[2] * d[1];
        const double c1 = tij[2] * d[0] - tij[0] * d[2];
        const double c2 = tij[0] * d[1] - tij[1] * d[0];
        sum += c0 * c0 + c1 * c1 + c2 * c2;
    }
    return sum;
}

// ---------------------------------------------------------------- 并查集
int ufFind(std::vector<int>& parent, int x)
{
    int r = x;
    while (parent[(size_t)r] != r) { r = parent[(size_t)r]; }
    while (parent[(size_t)x] != r) {
        const int nxt = parent[(size_t)x];
        parent[(size_t)x] = r;
        x = nxt;
    }
    return r;
}

// ---------------------------------------------------------------- 光束法平差
struct BaObs {
    int cam;
    int pt;
    double u;
    double v;
};

inline bool projectPoint(const double* R, const double* t, const double* X, double& u, double& v)
{
    const double xc = R[0] * X[0] + R[1] * X[1] + R[2] * X[2] + t[0];
    const double yc = R[3] * X[0] + R[4] * X[1] + R[5] * X[2] + t[1];
    const double zc = R[6] * X[0] + R[7] * X[1] + R[8] * X[2] + t[2];
    if (!(zc > 1e-6)) { return false; }
    const double iz = 1.0 / zc;
    u = kFocal * xc * iz + kCx;
    v = kFocal * yc * iz + kCy;
    return std::isfinite(u) && std::isfinite(v);
}

double reprojectionError(const std::vector<Camera>& cams, const std::vector<double>& pts,
                         const std::vector<BaObs>& obs, int& nInvalid)
{
    double sum = 0.0;
    int bad = 0;
    for (size_t k = 0; k < obs.size(); ++k) {
        const Camera& cam = cams[(size_t)obs[k].cam];
        const double* X = &pts[(size_t)obs[k].pt * 3];
        double u = 0.0, v = 0.0;
        if (!projectPoint(cam.R, cam.t, X, u, v)) {
            ++bad;
            sum += 1e6;                 // 退化观测给罚项, 保证不会被 LM 利用
            continue;
        }
        const double r0 = u - obs[k].u;
        const double r1 = v - obs[k].v;
        sum += r0 * r0 + r1 * r1;
    }
    nInvalid = bad;
    return sum;
}

// LM + Schur 补; 返回 BA 后平均重投影误差(像素)
double bundleAdjust(std::vector<Camera>& cams, std::vector<double>& pts, int np,
                    const std::vector<BaObs>& obs, int maxIters, int& itersDone,
                    double& rmseBefore, int& nObsUsed)
{
    const int nCam = (int)cams.size();
    const int nCamPar = 6 * (nCam - 1);
    const int nObs = (int)obs.size();
    itersDone = 0;
    nObsUsed = 0;
    rmseBefore = 0.0;
    if (np <= 0 || nObs <= 0) { return 0.0; }

    std::vector<double> U((size_t)nCam * 36, 0.0);
    std::vector<double> V((size_t)np * 9, 0.0);
    std::vector<double> Vinv((size_t)np * 9, 0.0);
    std::vector<double> W((size_t)nObs * 18, 0.0);
    std::vector<double> gcam((size_t)nCam * 6, 0.0);
    std::vector<double> gpt((size_t)np * 3, 0.0);
    std::vector<double> dcam((size_t)nCamPar, 0.0);
    std::vector<double> dpt((size_t)np * 3, 0.0);
    std::vector<double> gtmp((size_t)np * 3, 0.0);
    std::vector<double> S((size_t)nCamPar * (size_t)nCamPar, 0.0);
    std::vector<unsigned char> okObs((size_t)nObs, 0);
    std::vector<unsigned char> vbad((size_t)np, 0);

    int nInvalid = 0;
    double errCur = reprojectionError(cams, pts, obs, nInvalid);
    {
        int valid = 0;
        for (size_t k = 0; k < obs.size(); ++k) {
            const Camera& cam = cams[(size_t)obs[k].cam];
            const double* X = &pts[(size_t)obs[k].pt * 3];
            double u = 0.0, v = 0.0;
            if (projectPoint(cam.R, cam.t, X, u, v)) { ++valid; }
        }
        nObsUsed = valid;
        rmseBefore = (valid > 0) ? std::sqrt(errCur / (double)(2 * valid)) : 0.0;
    }

    std::vector<Camera> camsBackup = cams;
    std::vector<double> ptsBackup = pts;
    double lambda = 1e-3;

    for (int it = 0; it < maxIters; ++it) {
        bool improved = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            // ---------- 构建法方程 ----------
            for (size_t i = 0; i < U.size(); ++i) { U[i] = 0.0; }
            for (size_t i = 0; i < V.size(); ++i) { V[i] = 0.0; }
            for (size_t i = 0; i < gcam.size(); ++i) { gcam[i] = 0.0; }
            for (size_t i = 0; i < gpt.size(); ++i) { gpt[i] = 0.0; }

            for (int k = 0; k < nObs; ++k) {
                const BaObs& ob = obs[(size_t)k];
                const Camera& cam = cams[(size_t)ob.cam];
                const double* X = &pts[(size_t)ob.pt * 3];
                double u = 0.0, v = 0.0;
                if (!projectPoint(cam.R, cam.t, X, u, v)) { okObs[(size_t)k] = 0; continue; }
                okObs[(size_t)k] = 1;
                const double r0 = u - ob.u;
                const double r1 = v - ob.v;

                // 相机雅可比(中心差分, 局部参数化)
                double Jc[12];
                bool jok = true;
                for (int p = 0; p < 6; ++p) {
                    double dp[3] = {0.0, 0.0, 0.0};
                    double dt[3] = {0.0, 0.0, 0.0};
                    if (p < 3) { dp[p] = kJacStep; } else { dt[p - 3] = kJacStep; }
                    double Rdp[9], Rdm[9];
                    rodrigues(dp, Rdp);
                    mat3Mul(Rdp, cam.R, Rdp);
                    const double dpm[3] = {-dp[0], -dp[1], -dp[2]};
                    rodrigues(dpm, Rdm);
                    mat3Mul(Rdm, cam.R, Rdm);
                    const double tp[3] = {cam.t[0] + dt[0], cam.t[1] + dt[1], cam.t[2] + dt[2]};
                    const double tm[3] = {cam.t[0] - dt[0], cam.t[1] - dt[1], cam.t[2] - dt[2]};
                    double up = 0.0, vp = 0.0, um = 0.0, vm = 0.0;
                    if (!projectPoint(Rdp, tp, X, up, vp) || !projectPoint(Rdm, tm, X, um, vm)) { jok = false; break; }
                    Jc[0 * 6 + p] = (up - um) / (2.0 * kJacStep);
                    Jc[1 * 6 + p] = (vp - vm) / (2.0 * kJacStep);
                }
                if (!jok) { okObs[(size_t)k] = 0; continue; }

                // 三维点雅可比(解析)
                const double xc = cam.R[0] * X[0] + cam.R[1] * X[1] + cam.R[2] * X[2] + cam.t[0];
                const double yc = cam.R[3] * X[0] + cam.R[4] * X[1] + cam.R[5] * X[2] + cam.t[1];
                const double zc = cam.R[6] * X[0] + cam.R[7] * X[1] + cam.R[8] * X[2] + cam.t[2];
                const double iz = 1.0 / zc;
                const double iz2 = iz * iz;
                double Jp[6];
                Jp[0] = kFocal * iz * cam.R[0] - kFocal * xc * iz2 * cam.R[6];
                Jp[1] = kFocal * iz * cam.R[1] - kFocal * xc * iz2 * cam.R[7];
                Jp[2] = kFocal * iz * cam.R[2] - kFocal * xc * iz2 * cam.R[8];
                Jp[3] = kFocal * iz * cam.R[3] - kFocal * yc * iz2 * cam.R[6];
                Jp[4] = kFocal * iz * cam.R[4] - kFocal * yc * iz2 * cam.R[7];
                Jp[5] = kFocal * iz * cam.R[5] - kFocal * yc * iz2 * cam.R[8];

                double* Uc = &U[(size_t)ob.cam * 36];
                for (int a = 0; a < 6; ++a) {
                    for (int b = 0; b < 6; ++b) {
                        Uc[a * 6 + b] += Jc[a] * Jc[b] + Jc[6 + a] * Jc[6 + b];
                    }
                }
                double* Vp = &V[(size_t)ob.pt * 9];
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b) {
                        Vp[a * 3 + b] += Jp[a] * Jp[b] + Jp[3 + a] * Jp[3 + b];
                    }
                }
                double* Wk = &W[(size_t)k * 18];
                for (int a = 0; a < 6; ++a) {
                    for (int b = 0; b < 3; ++b) {
                        Wk[a * 3 + b] = Jc[a] * Jp[b] + Jc[6 + a] * Jp[3 + b];
                    }
                }
                double* gc = &gcam[(size_t)ob.cam * 6];
                for (int a = 0; a < 6; ++a) { gc[a] -= Jc[a] * r0 + Jc[6 + a] * r1; }
                double* gp = &gpt[(size_t)ob.pt * 3];
                for (int b = 0; b < 3; ++b) { gp[b] -= Jp[b] * r0 + Jp[3 + b] * r1; }
            }

            // ---------- 阻尼 + 三维点块求逆 ----------
            for (int c = 1; c < nCam; ++c) {
                double* Uc = &U[(size_t)c * 36];
                for (int a = 0; a < 6; ++a) {
                    Uc[a * 6 + a] += lambda * (Uc[a * 6 + a] + 1e-12);
                }
            }
            for (int p = 0; p < np; ++p) {
                double* Vp = &V[(size_t)p * 9];
                for (int a = 0; a < 3; ++a) {
                    Vp[a * 3 + a] += lambda * (Vp[a * 3 + a] + 1e-12);
                }
                double* Vi = &Vinv[(size_t)p * 9];
                if (!invert3(Vp, Vi)) {
                    vbad[(size_t)p] = 1;
                } else {
                    vbad[(size_t)p] = 0;
                }
            }

            // ---------- Schur 补 ----------
            for (size_t i = 0; i < S.size(); ++i) { S[i] = 0.0; }
            for (size_t i = 0; i < dcam.size(); ++i) { dcam[i] = 0.0; }
            for (int c = 1; c < nCam; ++c) {
                const int base = (c - 1) * 6;
                const double* Uc = &U[(size_t)c * 36];
                for (int a = 0; a < 6; ++a) {
                    for (int b = 0; b < 6; ++b) {
                        S[(size_t)(base + a) * nCamPar + (base + b)] += Uc[a * 6 + b];
                    }
                    dcam[(size_t)base + a] = gcam[(size_t)c * 6 + a];
                }
            }
            for (int k = 0; k < nObs; ++k) {
                if (!okObs[(size_t)k]) { continue; }
                const int pt = obs[(size_t)k].pt;
                if (vbad[(size_t)pt]) { continue; }
                const double* Wk = &W[(size_t)k * 18];
                const double* Vi = &Vinv[(size_t)pt * 9];
                double Y[18];
                for (int a = 0; a < 6; ++a) {
                    for (int b = 0; b < 3; ++b) {
                        Y[a * 3 + b] = Wk[a * 3 + 0] * Vi[0 * 3 + b]
                                     + Wk[a * 3 + 1] * Vi[1 * 3 + b]
                                     + Wk[a * 3 + 2] * Vi[2 * 3 + b];
                    }
                }
                const int c = obs[(size_t)k].cam;
                if (c >= 1) {
                    const int base = (c - 1) * 6;
                    for (int a = 0; a < 6; ++a) {
                        for (int b = 0; b < 6; ++b) {
                            double s2 = 0.0;
                            for (int m = 0; m < 3; ++m) { s2 += Y[a * 3 + m] * Wk[b * 3 + m]; }
                            S[(size_t)(base + a) * nCamPar + (base + b)] -= s2;
                        }
                    }
                    const double* gp = &gpt[(size_t)pt * 3];
                    for (int a = 0; a < 6; ++a) {
                        double s2 = 0.0;
                        for (int m = 0; m < 3; ++m) { s2 += Y[a * 3 + m] * gp[m]; }
                        dcam[(size_t)base + a] -= s2;
                    }
                }
            }
            // 相机系统阻尼
            for (int i = 0; i < nCamPar; ++i) {
                S[(size_t)i * nCamPar + i] += lambda * (S[(size_t)i * nCamPar + i] + 1e-12);
            }

            // ---------- 求解相机增量 ----------
            bool solved = false;
            std::vector<double> Sw = S;
            std::vector<double> rhs = dcam;
            if (solveCholesky(Sw, rhs, nCamPar)) {
                solved = true;
            } else {
                for (int retry = 0; retry < 3 && !solved; ++retry) {
                    double dmax = 0.0;
                    for (int i = 0; i < nCamPar; ++i) {
                        const double v = std::fabs(Sw[(size_t)i * nCamPar + i]);
                        if (v > dmax) { dmax = v; }
                    }
                    const double jitter = 1e-6 * (dmax + 1e-6) * (double)(retry + 1);
                    for (int i = 0; i < nCamPar; ++i) { Sw[(size_t)i * nCamPar + i] += jitter; }
                    rhs = dcam;
                    if (solveCholesky(Sw, rhs, nCamPar)) { solved = true; }
                }
            }
            ++itersDone;
            if (!solved) {
                lambda = std::min(lambda * 6.0, 1e9);
                cams = camsBackup;
                pts = ptsBackup;
                continue;
            }
            for (int i = 0; i < nCamPar; ++i) { dcam[(size_t)i] = rhs[(size_t)i]; }

            // ---------- 回代三维点增量 ----------
            gtmp = gpt;
            for (int k = 0; k < nObs; ++k) {
                if (!okObs[(size_t)k]) { continue; }
                const int c = obs[(size_t)k].cam;
                if (c < 1) { continue; }
                const int pt = obs[(size_t)k].pt;
                if (vbad[(size_t)pt]) { continue; }
                const double* Wk = &W[(size_t)k * 18];
                const double* dc = &dcam[(size_t)(c - 1) * 6];
                for (int m = 0; m < 3; ++m) {
                    double s2 = 0.0;
                    for (int a = 0; a < 6; ++a) { s2 += Wk[a * 3 + m] * dc[a]; }
                    gtmp[(size_t)pt * 3 + m] -= s2;
                }
            }
            for (int p = 0; p < np; ++p) {
                double* d = &dpt[(size_t)p * 3];
                if (vbad[(size_t)p]) { d[0] = 0.0; d[1] = 0.0; d[2] = 0.0; continue; }
                const double* Vi = &Vinv[(size_t)p * 9];
                const double* g = &gtmp[(size_t)p * 3];
                for (int a = 0; a < 3; ++a) {
                    d[a] = Vi[a * 3 + 0] * g[0] + Vi[a * 3 + 1] * g[1] + Vi[a * 3 + 2] * g[2];
                    if (!std::isfinite(d[a])) { d[a] = 0.0; }
                }
            }

            // ---------- 应用增量 ----------
            bool finiteStep = true;
            for (int i = 0; i < nCamPar; ++i) {
                if (!std::isfinite(dcam[(size_t)i])) { finiteStep = false; break; }
            }
            for (int p = 0; p < np && finiteStep; ++p) {
                for (int a = 0; a < 3; ++a) {
                    if (!std::isfinite(dpt[(size_t)p * 3 + a])) { finiteStep = false; break; }
                }
            }
            if (finiteStep) {
                for (int c = 1; c < nCam; ++c) {
                    const double dr[3] = {dcam[(size_t)(c - 1) * 6 + 0],
                                          dcam[(size_t)(c - 1) * 6 + 1],
                                          dcam[(size_t)(c - 1) * 6 + 2]};
                    const double dt[3] = {dcam[(size_t)(c - 1) * 6 + 3],
                                          dcam[(size_t)(c - 1) * 6 + 4],
                                          dcam[(size_t)(c - 1) * 6 + 5]};
                    double Rd[9];
                    rodrigues(dr, Rd);
                    mat3Mul(Rd, cams[(size_t)c].R, cams[(size_t)c].R);
                    cams[(size_t)c].t[0] += dt[0];
                    cams[(size_t)c].t[1] += dt[1];
                    cams[(size_t)c].t[2] += dt[2];
                }
                for (int p = 0; p < np; ++p) {
                    pts[(size_t)p * 3 + 0] += dpt[(size_t)p * 3 + 0];
                    pts[(size_t)p * 3 + 1] += dpt[(size_t)p * 3 + 1];
                    pts[(size_t)p * 3 + 2] += dpt[(size_t)p * 3 + 2];
                }
            } else {
                cams = camsBackup;
                pts = ptsBackup;
            }

            // ---------- 评估 ----------
            int bad2 = 0;
            const double errNew = reprojectionError(cams, pts, obs, bad2);
            if (finiteStep && std::isfinite(errNew) && errNew < errCur) {
                errCur = errNew;
                lambda = std::max(lambda * 0.3, 1e-9);
                camsBackup = cams;
                ptsBackup = pts;
                improved = true;
                break;
            }
            cams = camsBackup;
            pts = ptsBackup;
            lambda = std::min(lambda * 6.0, 1e9);
        }
        if (!improved) { break; }
    }

    // 最终误差(只统计有效观测)
    double sum = 0.0;
    int valid = 0;
    for (size_t k = 0; k < obs.size(); ++k) {
        const Camera& cam = cams[(size_t)obs[k].cam];
        const double* X = &pts[(size_t)obs[k].pt * 3];
        double u = 0.0, v = 0.0;
        if (!projectPoint(cam.R, cam.t, X, u, v)) { continue; }
        const double r0 = u - obs[k].u;
        const double r1 = v - obs[k].v;
        sum += r0 * r0 + r1 * r1;
        ++valid;
    }
    nObsUsed = valid;
    return (valid > 0) ? std::sqrt(sum / (double)(2 * valid)) : 0.0;
}

// ---------------------------------------------------------------- 诊断(Umeyama 对齐)
// 求 src -> dst 的最优相似变换: dst ~= s * R * src + t
bool similarityAlign(const std::vector<double>& src, const std::vector<double>& dst, int n,
                     double& s, double* R, double* t)
{
    if (n < 3) { return false; }
    double ms[3] = {0.0, 0.0, 0.0}, md[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            ms[k] += src[(size_t)i * 3 + k];
            md[k] += dst[(size_t)i * 3 + k];
        }
    }
    for (int k = 0; k < 3; ++k) { ms[k] /= (double)n; md[k] /= (double)n; }
    double var = 0.0;
    double H[9];
    for (int i = 0; i < 9; ++i) { H[i] = 0.0; }
    for (int i = 0; i < n; ++i) {
        const double a0 = src[(size_t)i * 3 + 0] - ms[0];
        const double a1 = src[(size_t)i * 3 + 1] - ms[1];
        const double a2 = src[(size_t)i * 3 + 2] - ms[2];
        const double b0 = dst[(size_t)i * 3 + 0] - md[0];
        const double b1 = dst[(size_t)i * 3 + 1] - md[1];
        const double b2 = dst[(size_t)i * 3 + 2] - md[2];
        var += a0 * a0 + a1 * a1 + a2 * a2;
        const double bb[3] = {b0, b1, b2};
        const double aa[3] = {a0, a1, a2};
        for (int p = 0; p < 3; ++p) {
            for (int q = 0; q < 3; ++q) { H[p * 3 + q] += bb[p] * aa[q]; }
        }
    }
    var /= (double)n;
    for (int i = 0; i < 9; ++i) { H[i] /= (double)n; }
    if (!(var > 1e-12)) { return false; }
    double U[9], sv[3], V[9];
    svd3(H, U, sv, V);
    double S[9];
    mat3Identity(S);
    const double d = mat3Det(U) * mat3Det(V);
    S[8] = (d >= 0.0) ? 1.0 : -1.0;
    // R = U * S * V^T
    double US[9], Vt[9];
    mat3Mul(U, S, US);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) { Vt[i * 3 + j] = V[j * 3 + i]; }
    }
    mat3Mul(US, Vt, R);
    double trace = sv[0] * S[0] + sv[1] * S[4] + sv[2] * S[8];
    s = trace / var;
    if (!std::isfinite(s) || !(s > 1e-12)) { return false; }
    double Rs[3];
    mat3MulVec(R, ms, Rs);
    t[0] = md[0] - s * Rs[0];
    t[1] = md[1] - s * Rs[1];
    t[2] = md[2] - s * Rs[2];
    return true;
}

} // namespace

// ===========================================================================
// 目标函数(全局作用域)
// ===========================================================================
Gb7Outcome gb7RunStructureFromMotion(int threads)
{
    if (threads < 1) { threads = 1; }
    Gb7Outcome o;
    o.name = "Structure from Motion";
    o.section = "Image Synthesis";
    o.score = 0.0;
    const int nt = threads;   // 实际可用的并行度(检测/描述子/匹配阶段使用)

    const int W = kImgW;
    const int H = kImgH;
    const size_t npix = (size_t)W * (size_t)H;

    // ---------------- 计时外: 场景 / 相机 / 成像 ----------------
    std::vector<ScenePoint> scene;
    generateScene(scene);
    std::vector<Camera> gtCams((size_t)kCamCount);
    generateCameras(gtCams);
    std::vector<float> blobKer;
    makeBlobKernel(blobKer);
    std::vector<double> noiseTab;
    fillNoiseTable(noiseTab, kNoiseTabSize, 0x2545F491u);
    std::vector<uint8_t> images(npix * (size_t)kCamCount);
    renderViews(scene, gtCams, images, blobKer, noiseTab);

    std::vector<int8_t> pattern;
    makeBriefPattern(pattern, 0x5F3A17C9u);

    // 工作缓冲(计时内不再分配图像级大块内存)
    HarrisWorkspace hw;
    hw.init();
    std::vector<FeatSet> feats((size_t)kCamCount);
    // 说明: 匹配/RANSAC 的暂存缓冲改为每个并行调用槽各一份(见下方 (3)(4)),
    // 因此这里不再需要共享的 pairMatches / fwdTmp / rw。
    std::vector<PairModel> models;
    models.reserve(32);

    uint64_t featTotal = 0;
    uint64_t matchTotal = 0;
    uint64_t inlierTotal = 0;

    // ======================= 计时区开始 =======================
    const std::clock_t cpu0 = std::clock();
    auroraFreqMarkStart();   // 运行时频率采样: 计时区间入口(写在 t0 之前, 不进 o.ms)
    const double t0 = nowMsSfm();

    // ---- (1)(2) 特征检测 + 描述子 ----
    // 相机之间串行(hw 是图像级共享暂存, 复制会带来每线程约 26MB 的额外内存),
    // 并行度放在 detectFeatures/computeDescriptors 内部(见函数内注释)。
    for (int c = 0; c < kCamCount; ++c) {
        const uint8_t* img = images.data() + (size_t)c * npix;
        detectFeatures(img, hw, feats[(size_t)c], nt);
        computeDescriptors(hw.blur.data(), feats[(size_t)c], pattern, nt);
        featTotal += (uint64_t)feats[(size_t)c].n;
    }

    // ---- (3)(4) 匹配 + 基础矩阵 RANSAC ----
    // 视角对之间完全独立: 各自的特征集只读, 各自的匹配表 / RANSAC 工作区 / 输出模型
    // 互不共享; RANSAC 的随机种子由 (i,j) 派生, 与线程调度无关 -> 结果逐位确定。
    // 视角对编号顺序与原来一致(i 外层、j 内层), 因此 models 的顺序(以及后续
    // 位姿初始化/track 构建读到的顺序)完全不变。
    struct PairJob {
        int i;
        int j;
    };
    std::vector<PairJob> pairJobs;
    pairJobs.reserve(32);
    for (int i = 0; i < kCamCount; ++i) {
        for (int j = i + 1; j <= i + kMatchJump && j < kCamCount; ++j) {
            PairJob pj;
            pj.i = i;
            pj.j = j;
            pairJobs.push_back(pj);
        }
    }
    models.assign(pairJobs.size(), PairModel());
    int ntPair = nt;
    if ((long long)ntPair > (long long)pairJobs.size()) {
        ntPair = (int)pairJobs.size();
    }
    gb7SfmParTasks(ntPair, (long long)pairJobs.size(), [&](long long s, long long e) {
        std::vector<Match> localMatches;
        std::vector<int> localFwd;
        RansacWorkspace localRw;
        for (long long m = s; m < e; ++m) {
            const int i = pairJobs[(size_t)m].i;
            const int j = pairJobs[(size_t)m].j;
            matchPair(feats[(size_t)i], feats[(size_t)j], localMatches, localFwd);
            PairModel& pm = models[(size_t)m];
            pm.ia = i;
            pm.ib = j;
            pm.matches.swap(localMatches);
            if ((int)pm.matches.size() >= 12) {
                const uint32_t seed = 0xA53C9E17u + (uint32_t)(i * 131 + j * 17);
                pm.valid = estimateFundamental(feats[(size_t)i], feats[(size_t)j], pm.matches, localRw,
                                               pm.F, pm.inliers, pm.inlierIdx, seed);
            }
        }
    });
    // 归约(整数和, 与并行合并顺序无关)
    for (size_t m = 0; m < models.size(); ++m) {
        matchTotal += (uint64_t)models[m].matches.size();
        if (models[m].valid) { inlierTotal += (uint64_t)models[m].inliers; }
    }

    // ---- (5) 位姿初始化: 相对位姿 -> 旋转链 -> 平移平均 ----
    std::vector<Camera> recCams((size_t)kCamCount);
    for (int c = 0; c < kCamCount; ++c) {
        mat3Identity(recCams[(size_t)c].R);
        recCams[(size_t)c].t[0] = 0.0;
        recCams[(size_t)c].t[1] = 0.0;
        recCams[(size_t)c].t[2] = 0.0;
    }
    std::vector<RelPose> rels(models.size());
    std::vector<int> relValid(models.size(), 0);
    bool anyRel = false;
    bool tSolveOk = false;      // 平移平均的线性最小二乘是否成功
    bool tFallback = false;     // 是否退回到链式平移初值(见下面的模型选择)
    for (size_t m = 0; m < models.size(); ++m) {
        const PairModel& pm = models[m];
        if (!pm.valid || pm.inliers < 15) { continue; }
        // 归一化坐标
        std::vector<double> x1(pm.matches.size()), y1(pm.matches.size());
        std::vector<double> x2(pm.matches.size()), y2(pm.matches.size());
        for (size_t k = 0; k < pm.matches.size(); ++k) {
            x1[k] = ((double)feats[(size_t)pm.ia].x[(size_t)pm.matches[k].a] - kCx) / kFocal;
            y1[k] = ((double)feats[(size_t)pm.ia].y[(size_t)pm.matches[k].a] - kCy) / kFocal;
            x2[k] = ((double)feats[(size_t)pm.ib].x[(size_t)pm.matches[k].b] - kCx) / kFocal;
            y2[k] = ((double)feats[(size_t)pm.ib].y[(size_t)pm.matches[k].b] - kCy) / kFocal;
        }
        relativePoseFromF(pm.F, x1.data(), y1.data(), x2.data(), y2.data(), pm.inlierIdx, rels[m]);
        if (rels[m].ok) { relValid[m] = 1; anyRel = true; }
    }

    if (anyRel) {
        // 旋转链(相邻视角)
        for (int c = 1; c < kCamCount; ++c) {
            int found = -1;
            for (size_t m = 0; m < models.size(); ++m) {
                if (relValid[m] && models[m].ia == c - 1 && models[m].ib == c) { found = (int)m; break; }
            }
            if (found < 0) {
                mat3Identity(recCams[(size_t)c].R);     // 退化: 该链断裂, 用单位旋转
                continue;
            }
            mat3Mul(rels[(size_t)found].R, recCams[(size_t)c - 1].R, recCams[(size_t)c].R);
        }
        // 链式平移初值(永远不退化): t_c = R_{c-1,c} t_{c-1} + t_{c-1,c}, 每段相对平移
        // 在 relativePoseFromF 里已归一化为单位长度, 而 10 个相机是等角度间隔的, 所以
        // 逐段单位基线给出的尺度与"锚点取单位长度"的最小二乘解同量级。它在最小二乘
        // 病态/塌缩时用来兜底(见下面的模型选择)。
        std::vector<Camera> chainCams = recCams;
        {
            int prev = 0;
            for (int c = 1; c < kCamCount; ++c) {
                int found = -1;
                for (size_t m = 0; m < models.size(); ++m) {
                    if (relValid[m] && models[m].ia == prev && models[m].ib == c) { found = (int)m; break; }
                }
                chainCams[(size_t)c].t[0] = chainCams[(size_t)prev].t[0];
                chainCams[(size_t)c].t[1] = chainCams[(size_t)prev].t[1];
                chainCams[(size_t)c].t[2] = chainCams[(size_t)prev].t[2];
                if (found >= 0) {
                    double rt[3];
                    mat3MulVec(rels[(size_t)found].R, chainCams[(size_t)prev].t, rt);
                    chainCams[(size_t)c].t[0] = rt[0] + rels[(size_t)found].t[0];
                    chainCams[(size_t)c].t[1] = rt[1] + rels[(size_t)found].t[1];
                    chainCams[(size_t)c].t[2] = rt[2] + rels[(size_t)found].t[2];
                }
                prev = c;
            }
        }

        // 平移平均: 未知量 t_1..t_{C-1}, 固定相机 0 (t=0) 与锚点相机
        int anchor = -1;
        for (size_t m = 0; m < models.size(); ++m) {
            if (relValid[m] && models[m].ia == 0) { anchor = models[m].ib; break; }
        }
        if (anchor > 0) {
            // 锚点平移取相对平移的单位方向
            double tan[3] = {0.0, 0.0, 0.0};
            for (size_t m = 0; m < models.size(); ++m) {
                if (relValid[m] && models[m].ia == 0 && models[m].ib == anchor) {
                    tan[0] = rels[m].t[0]; tan[1] = rels[m].t[1]; tan[2] = rels[m].t[2];
                    break;
                }
            }
            recCams[(size_t)anchor].t[0] = tan[0];
            recCams[(size_t)anchor].t[1] = tan[1];
            recCams[(size_t)anchor].t[2] = tan[2];
            std::vector<int> camIdx((size_t)kCamCount, -1);
            int nUnk = 0;
            for (int c = 1; c < kCamCount; ++c) {
                if (c == anchor) { continue; }
                camIdx[(size_t)c] = nUnk;
                ++nUnk;
            }
            if (nUnk > 0) {
                const int nu = nUnk * 3;
                std::vector<double> M((size_t)nu * (size_t)nu, 0.0);
                std::vector<double> b((size_t)nu, 0.0);
                for (size_t m = 0; m < models.size(); ++m) {
                    if (!relValid[m]) { continue; }
                    const int ci = models[m].ia;
                    const int cj = models[m].ib;
                    const double* Rij = rels[m].R;
                    const double* tij = rels[m].t;
                    const double S[9] = {0.0, -tij[2], tij[1], tij[2], 0.0, -tij[0], -tij[1], tij[0], 0.0};
                    double SR[9];
                    mat3Mul(S, Rij, SR);
                    const double* ti = recCams[(size_t)ci].t;
                    const double* tj = recCams[(size_t)cj].t;
                    const int iIdx = camIdx[(size_t)ci];
                    const int jIdx = camIdx[(size_t)cj];
                    for (int r = 0; r < 3; ++r) {
                        int rowIdx[6];
                        double rowVal[6];
                        int nn = 0;
                        for (int k2 = 0; k2 < 3; ++k2) {
                            if (jIdx >= 0) { rowIdx[nn] = jIdx * 3 + k2; rowVal[nn] = S[r * 3 + k2]; ++nn; }
                        }
                        for (int k2 = 0; k2 < 3; ++k2) {
                            if (iIdx >= 0) { rowIdx[nn] = iIdx * 3 + k2; rowVal[nn] = -SR[r * 3 + k2]; ++nn; }
                        }
                        double rhsV = 0.0;
                        if (jIdx < 0) { for (int k2 = 0; k2 < 3; ++k2) { rhsV -= S[r * 3 + k2] * tj[k2]; } }
                        if (iIdx < 0) { for (int k2 = 0; k2 < 3; ++k2) { rhsV += SR[r * 3 + k2] * ti[k2]; } }
                        if (nn == 0) { continue; }
                        for (int a = 0; a < nn; ++a) {
                            for (int b2 = 0; b2 < nn; ++b2) {
                                M[(size_t)rowIdx[a] * nu + rowIdx[b2]] += rowVal[a] * rowVal[b2];
                            }
                            b[(size_t)rowIdx[a]] += rowVal[a] * rhsV;
                        }
                    }
                }
                double tr = 0.0;
                for (int i = 0; i < nu; ++i) { tr += M[(size_t)i * nu + i]; }
                const double lam = 1e-9 * (tr / (double)nu + 1e-6);
                for (int i = 0; i < nu; ++i) { M[(size_t)i * nu + i] += lam; }
                tSolveOk = solveLinearN(M, b, nu);
                if (tSolveOk) {
                    for (int c = 1; c < kCamCount; ++c) {
                        if (camIdx[(size_t)c] < 0) { continue; }
                        recCams[(size_t)c].t[0] = b[(size_t)camIdx[(size_t)c] * 3 + 0];
                        recCams[(size_t)c].t[1] = b[(size_t)camIdx[(size_t)c] * 3 + 1];
                        recCams[(size_t)c].t[2] = b[(size_t)camIdx[(size_t)c] * 3 + 2];
                    }
                }
            }
        }
        // 模型选择(兜底): 线性最小二乘是下面这个目标的最小化解; 若它给出的解反而比
        // 链式初值更差(病态/塌缩), 就采用链式初值。塌缩的相机阵列会让后续所有三角化
        // 退化 -> 0 个三维点 -> metric 0.00, 这是必须避免的静默失败。
        {
            const double costLsq = translationCostSfm(recCams, rels, relValid, models);
            const double costChain = translationCostSfm(chainCams, rels, relValid, models);
            if (!(costLsq <= costChain * 1.0000001 + 1e-12)) {
                for (int c = 0; c < kCamCount; ++c) {
                    recCams[(size_t)c].t[0] = chainCams[(size_t)c].t[0];
                    recCams[(size_t)c].t[1] = chainCams[(size_t)c].t[1];
                    recCams[(size_t)c].t[2] = chainCams[(size_t)c].t[2];
                }
                tFallback = true;
            }
        }
    }

    // ---- (6) 建 track + 三角化 ----
    std::vector<double> points;         // 3 * n
    std::vector<BaObs> obs;
    uint64_t trackCount = 0;
    uint64_t triCount = 0;
    if (anyRel) {
        const int nNodes = kCamCount * kMaxFeat;
        std::vector<int> parent((size_t)nNodes);
        std::vector<unsigned char> used((size_t)nNodes, 0);
        for (int i = 0; i < nNodes; ++i) { parent[(size_t)i] = i; }
        for (size_t m = 0; m < models.size(); ++m) {
            const PairModel& pm = models[m];
            if (!pm.valid) { continue; }
            const int baseA = pm.ia * kMaxFeat;
            const int baseB = pm.ib * kMaxFeat;
            for (size_t k = 0; k < pm.inlierIdx.size(); ++k) {
                const Match& mt = pm.matches[(size_t)pm.inlierIdx[k]];
                if (mt.a < 0 || mt.a >= kMaxFeat || mt.b < 0 || mt.b >= kMaxFeat) { continue; }
                const int na = baseA + mt.a;
                const int nb = baseB + mt.b;
                used[(size_t)na] = 1;
                used[(size_t)nb] = 1;
                const int ra = ufFind(parent, na);
                const int rb = ufFind(parent, nb);
                if (ra != rb) { parent[(size_t)ra] = rb; }
            }
        }
        // 收集 track
        std::vector<int> bucketOf((size_t)nNodes, -1);
        std::vector<std::vector<int> > buckets;
        for (int i = 0; i < nNodes; ++i) {
            if (!used[(size_t)i]) { continue; }
            const int r = ufFind(parent, i);
            if (bucketOf[(size_t)r] < 0) {
                bucketOf[(size_t)r] = (int)buckets.size();
                buckets.push_back(std::vector<int>());
            }
            buckets[(size_t)bucketOf[(size_t)r]].push_back(i);
        }
        std::vector<double> Ps;
        std::vector<double> us, vs;
        std::vector<int> obsCam;
        // 三角化候选: 先全部收集, 再用自适应阈值筛选。
        // (初始位姿由链式相对位姿 + 平移平均得到, 存在链式漂移, 重投影误差量级可能
        //  远大于几像素; 若用固定的 6px 阈值会把大量正确点误删, 故阈值取
        //  max(8px, 4 * 中位数误差), 只剔除明显的粗差/外点。)
        std::vector<double> candX, candErr, candU, candV;
        std::vector<int> candCam, candStart;
        for (size_t b = 0; b < buckets.size(); ++b) {
            const std::vector<int>& nd = buckets[b];
            if (nd.size() < 2) { continue; }
            ++trackCount;
            Ps.clear(); us.clear(); vs.clear(); obsCam.clear();
            int lastCam = -1;
            for (size_t k = 0; k < nd.size(); ++k) {
                const int cam = nd[k] / kMaxFeat;
                const int fi = nd[k] % kMaxFeat;
                if (cam == lastCam) { continue; }      // 同一相机只取一个观测
                lastCam = cam;
                if (fi < 0 || fi >= feats[(size_t)cam].n) { continue; }
                double P[12];
                makeProjection(recCams[(size_t)cam].R, recCams[(size_t)cam].t, P);
                for (int q = 0; q < 12; ++q) { Ps.push_back(P[q]); }
                us.push_back((double)feats[(size_t)cam].x[(size_t)fi]);
                vs.push_back((double)feats[(size_t)cam].y[(size_t)fi]);
                obsCam.push_back(cam);
            }
            const int cnt = (int)us.size();
            if (cnt != (int)obsCam.size()) { continue; }
            if (cnt < 2) { continue; }
            double X[3];
            if (!triangulateDlt(Ps.data(), us.data(), vs.data(), cnt, X)) { continue; }
            // 所有观测相机深度必须为正
            bool depthOk = true;
            double errSum = 0.0;
            for (int k = 0; k < cnt; ++k) {
                const double* P = Ps.data() + (size_t)k * 12;
                const double xc = P[0] * X[0] + P[1] * X[1] + P[2] * X[2] + P[3];
                const double yc = P[4] * X[0] + P[5] * X[1] + P[6] * X[2] + P[7];
                const double zc = P[8] * X[0] + P[9] * X[1] + P[10] * X[2] + P[11];
                if (!(zc > 0.05)) { depthOk = false; break; }
                const double iw = 1.0 / zc;
                const double pu = xc * iw;
                const double pv = yc * iw;
                const double du = pu - us[(size_t)k];
                const double dv = pv - vs[(size_t)k];
                errSum += du * du + dv * dv;
            }
            if (!depthOk) { continue; }
            const double meanErr = std::sqrt(errSum / (double)cnt);
            if (!std::isfinite(meanErr)) { continue; }
            candStart.push_back((int)candCam.size());
            candX.push_back(X[0]);
            candX.push_back(X[1]);
            candX.push_back(X[2]);
            candErr.push_back(meanErr);
            for (int k = 0; k < cnt; ++k) {
                candCam.push_back(obsCam[(size_t)k]);
                candU.push_back(us[(size_t)k]);
                candV.push_back(vs[(size_t)k]);
            }
        }
        candStart.push_back((int)candCam.size());       // 哨兵
        double keepThr = 8.0;
        if (!candErr.empty()) {
            std::vector<double> tmpErr = candErr;
            const size_t mid = tmpErr.size() / 2;
            std::nth_element(tmpErr.begin(), tmpErr.begin() + (std::ptrdiff_t)mid, tmpErr.end());
            keepThr = 4.0 * tmpErr[mid];
            if (!(keepThr > 8.0)) { keepThr = 8.0; }
            if (!std::isfinite(keepThr) || keepThr > 1.0e6) { keepThr = 1.0e6; }
        }
        for (size_t i = 0; i < candErr.size(); ++i) {
            if (candErr[i] > keepThr) { continue; }
            if ((int)(points.size() / 3) >= kMaxTracks) { break; }
            const int ptIdx = (int)(points.size() / 3);
            points.push_back(candX[i * 3 + 0]);
            points.push_back(candX[i * 3 + 1]);
            points.push_back(candX[i * 3 + 2]);
            for (int k = candStart[i]; k < candStart[i + 1]; ++k) {
                BaObs ob;
                ob.cam = candCam[(size_t)k];
                ob.pt = ptIdx;
                ob.u = candU[(size_t)k];
                ob.v = candV[(size_t)k];
                obs.push_back(ob);
            }
            ++triCount;
        }
    }

    // ---- (7) 光束法平差 ----
    int baIters = 0;
    int baObsUsed = 0;
    double rmseBefore = 0.0;
    double rmseAfter = 0.0;
    const int np = (int)(points.size() / 3);
    if (np > 0 && !obs.empty()) {
        rmseAfter = bundleAdjust(recCams, points, np, obs, kBaIters, baIters, rmseBefore, baObsUsed);
    }
    // ======================= 计时区结束 =======================
    const double t1 = nowMsSfm();
    auroraFreqMarkStop();    // 运行时频率采样: 计时区间出口(写在 t1 之后, 不进 o.ms)
    // 并行度 = 进程 CPU 时间 / 墙钟时间(含仍未并行的 RANSAC 之外的三角化/BA 串行段)
    const double cpuMsSfm = (double)(std::clock() - cpu0) * 1000.0 / (double)CLOCKS_PER_SEC;
    const double wallMsSfm = t1 - t0;

    // ---------------- (8) 精度校验(计时外, 只用真值做诊断) ----------------
    double truthCenterRms = 0.0;
    double truthPointRms = 0.0;
    {
        std::vector<double> src, dst;
        for (int c = 0; c < kCamCount; ++c) {
            // 估计相机中心: C = -R^T t
            double tc[3] = {recCams[(size_t)c].t[0], recCams[(size_t)c].t[1], recCams[(size_t)c].t[2]};
            double C[3];
            mat3MulTVec(recCams[(size_t)c].R, tc, C);
            src.push_back(-C[0]);
            src.push_back(-C[1]);
            src.push_back(-C[2]);
            dst.push_back(gtCams[(size_t)c].eye[0]);
            dst.push_back(gtCams[(size_t)c].eye[1]);
            dst.push_back(gtCams[(size_t)c].eye[2]);
        }
        double s = 0.0, R[9], t[3];
        if (similarityAlign(src, dst, kCamCount, s, R, t)) {
            double acc = 0.0;
            for (int c = 0; c < kCamCount; ++c) {
                double q[3] = {src[(size_t)c * 3 + 0], src[(size_t)c * 3 + 1], src[(size_t)c * 3 + 2]};
                double rq[3];
                mat3MulVec(R, q, rq);
                for (int k = 0; k < 3; ++k) {
                    const double e = s * rq[k] + t[k] - dst[(size_t)c * 3 + k];
                    acc += e * e;
                }
            }
            truthCenterRms = std::sqrt(acc / (double)kCamCount);
            // 重建点的最近邻误差(抽样, 避免过慢)
            const int maxPt = 256;
            const int npt = (np < maxPt) ? np : maxPt;
            double pacc = 0.0;
            int pcnt = 0;
            for (int i = 0; i < npt; ++i) {
                double q[3] = {points[(size_t)i * 3 + 0], points[(size_t)i * 3 + 1], points[(size_t)i * 3 + 2]};
                double rq[3];
                mat3MulVec(R, q, rq);
                const double x = s * rq[0] + t[0];
                const double y = s * rq[1] + t[1];
                const double z = s * rq[2] + t[2];
                double best = 1e30;
                for (size_t k = 0; k < scene.size(); ++k) {
                    const double dx = scene[k].p[0] - x;
                    const double dy = scene[k].p[1] - y;
                    const double dz = scene[k].p[2] - z;
                    const double d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best) { best = d2; }
                }
                if (std::isfinite(best)) { pacc += std::sqrt(best); ++pcnt; }
            }
            truthPointRms = (pcnt > 0) ? (pacc / (double)pcnt) : 0.0;
        }
    }

    // ---------------- 结果 ----------------
    double seconds = (t1 - t0) / 1000.0;
    if (!(seconds > 1e-9) || !std::isfinite(seconds)) { seconds = 1e-9; }
    const int nPtMetric = (np > 0) ? np : 1;
    const int nCamMetric = kCamCount;
    const int nIterMetric = (baIters > 0) ? baIters : 1;
    double mpts = (double)nPtMetric * (double)nCamMetric * (double)nIterMetric / 1e6;
    double rate = mpts / seconds;
    if (!std::isfinite(rate) || rate < 0.0) { rate = 0.0; }

    char buf[32];
    // %.4g: 计分解析的就是这一串, 位数不足会把分数网格化(见 gb7.cpp 计分处)
    snprintf(buf, sizeof(buf), "%.4g", rate);
    o.ms = wallMsSfm;
    o.metric = buf;
    o.unit = "Mpts/s";
    o.parallelism = gb7Parallelism(cpuMsSfm, wallMsSfm);
    o.score = 0.0;

    // ---- 退化结果必须可见 ----
    // metric = (进入 BA 的三维点数 x 相机数 x BA 迭代次数)/1e6/秒。只要三维点数为 0
    // (或 BA 没跑), 分子就退化成 1x10x1, 界面只能看到一个 0.00, 完全看不出哪一步空了。
    // 这里把失败的那一级直接写进 unit(界面是 metric + unit 一起显示), 正常情况仍为
    // 纯数字 + "Mpts/s"。
    if (np <= 0 || baIters <= 0) {
        int validPairs = 0;
        for (size_t m = 0; m < models.size(); ++m) {
            if (models[m].valid) { ++validPairs; }
        }
        char why[224];
        if (featTotal == 0) {
            snprintf(why, sizeof(why),
                     "Mpts/s (failed: 0 features detected on %d views; match=%llu)",
                     kCamCount, (unsigned long long)matchTotal);
        } else if (!anyRel) {
            snprintf(why, sizeof(why),
                     "Mpts/s (failed: no usable relative pose; feat=%llu match=%llu inliers=%llu validPairs=%d/%d)",
                     (unsigned long long)featTotal, (unsigned long long)matchTotal,
                     (unsigned long long)inlierTotal, validPairs, (int)models.size());
        } else if (trackCount == 0) {
            snprintf(why, sizeof(why),
                     "Mpts/s (failed: 0 tracks; feat=%llu match=%llu inliers=%llu)",
                     (unsigned long long)featTotal, (unsigned long long)matchTotal,
                     (unsigned long long)inlierTotal);
        } else if (triCount == 0) {
            snprintf(why, sizeof(why),
                     "Mpts/s (failed: triangulation rejected all %llu tracks; tSolve=%s chainFallback=%s)",
                     (unsigned long long)trackCount, tSolveOk ? "ok" : "failed", tFallback ? "yes" : "no");
        } else {
            snprintf(why, sizeof(why),
                     "Mpts/s (failed: bundle adjustment did not run; pts=%d obs=%llu)",
                     np, (unsigned long long)obs.size());
        }
        o.unit = why;
    } else if (!(rate >= 0.005)) {
        // 【真机 0.0 的可见化】上面 4 个分支只在"硬失败"(np<=0 / BA 没跑)时触发。
        // 但真机上出现过另一种情况: np>0 且 baIters>0, 可 metric 小到界面 %.2f 只能显示
        // 0.00 —— 这时 unit 仍然是干净的 "Mpts/s", 界面上完全看不出哪一步空了。
        // print 口径没有任何变化(仍然是同一个 rate), 只是在这种"重建几乎为空"的情形下,
        // 把要人工核对的那几个量直接写进 unit:
        //   pts      进入 BA 的三维点数(metric 分子的 nPt)
        //   tracks   并查集得到的 track 总数(与 pts 的比值说明三角化淘汰了多少)
        //   inliers  各视角对 RANSAC 内点之和(说明匹配/几何验证这一级的规模)
        //   baIters  BA 实际迭代次数(metric 分子的 nIter)
        //   reproj   BA 后的平均重投影误差(px); 它应该被真实约束在 1.5 px 以内
        // 阈值 0.005 就是 printf("%.2f") 的舍入边界, 与 metric 口径无关。
        char why[224];
        snprintf(why, sizeof(why),
                 "Mpts/s (degenerate: pts=%d tracks=%llu inliers=%llu matches=%llu baIters=%d reproj=%.2fpx)",
                 np, (unsigned long long)trackCount, (unsigned long long)inlierTotal,
                 (unsigned long long)matchTotal, baIters, rmseAfter);
        o.unit = why;
    }

    gGb7SfmDiag.scenePoints = (int)scene.size();
    gGb7SfmDiag.cameras = kCamCount;
    gGb7SfmDiag.featuresTotal = (long long)featTotal;
    gGb7SfmDiag.pairsTried = (long long)models.size();
    gGb7SfmDiag.matchesTotal = (long long)matchTotal;
    gGb7SfmDiag.inliersTotal = (long long)inlierTotal;
    gGb7SfmDiag.tracks = (long long)trackCount;
    gGb7SfmDiag.triangulated = (long long)triCount;
    gGb7SfmDiag.baPoints = np;
    gGb7SfmDiag.baCameras = kCamCount;
    gGb7SfmDiag.baIterations = baIters;
    gGb7SfmDiag.baObservations = baObsUsed;
    gGb7SfmDiag.baRmseBefore = rmseBefore;
    gGb7SfmDiag.baRmseAfter = rmseAfter;
    gGb7SfmDiag.truthCenterRms = truthCenterRms;
    gGb7SfmDiag.truthPointRms = truthPointRms;
    gGb7SfmDiag.metricMpts = rate;

    // 防止优化器把整条流水线删掉(结果必须可观测)
    double sinkVal = rmseAfter + rmseBefore + truthCenterRms + truthPointRms;
    if (!points.empty()) { sinkVal += points[0]; }
    volatile double sink = sinkVal;
    (void)sink;
    return o;
}
