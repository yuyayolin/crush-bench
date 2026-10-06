#ifndef AURORA_GPU7_H
#define AURORA_GPU7_H

// ============================================================================
//  Aurora Bench · GPU 负载套件 (原生 C++ / OpenGL ES 3.0 离屏)
// ----------------------------------------------------------------------------
//  本模块完全独立于 gpu/ 子目录: 无 XComponent、无窗口, 只用 EGL pbuffer 离屏
//  上下文 + FBO 渲染。所有 GL 调用都在调用 gpu7Run() 的那个线程里同步完成,
//  不创建任何线程。
//
//  计时协议(每一项都相同):
//      预热 WAR 帧 -> 测量 FRAMES 帧 -> glFinish() -> 取耗时
//      报告同时给出 metric(吞吐量, 越大越快) 与 fps。
//
//  分辨率与 metric 口径(与 CS1 / 3DMark 的固定分辨率口径一致):
//      项 0  Background Blur      1920x1080 (两趟可分离高斯, 31 抽样, 半径 15)          Mpx/s
//      项 1  Face Tracking        1920x1080 (4 级金字塔, 每级 5 次 LK 迭代, 共 20 次)   Mpx/s
//      项 2  Feature Matching     描述子 4096x64, 匹配每帧 8192 万对(4096 查询 x 4096)  Gpair/s
//      项 3  Fluid Simulation     1024x1024 (>=40 次 Jacobi 压力迭代/帧)                Mcell/s
//      项 4  Horizon Detection    1920x1080 -> Hough 180 角度 x 1024 rho 桶              Mvote/s
//      项 5  Particle Physics     1024x1024 状态纹理 = 1048576 粒子, 每帧 524288 粒子   Mparticle/s
//      项 6  Path Tracer          1280x720, 1 spp/帧, 2 次反弹, 累积到 RGBA32F           Mray/s
//      项 7  Photo Filter         4000x3000 (LUT -> 饱和度 -> 3x3 锐化 -> 暗角 -> 褐色) Mpx/s
//      项 8  RAW                   4000x3000 (Bayer 去马赛克 + 白平衡 + 3x3 矩阵 + gamma) Mpx/s
//      项 9  Super Resolution     960x540 -> 3840x2160 (3 层 3x3 卷积, 16 通道, ReLU)   Mpx/s
//      项 10 Video Filter         1280x720 (5 帧时域降噪 -> 锐化 -> 调色 -> 插帧)       Mpx/s
//
//  浮点渲染: 优先使用 GL_RGBA16F / GL_RGBA32F。每个离屏目标都会真正建好 FBO 并用
//  glCheckFramebufferStatus 验证, 失败则按 RGBA32F -> RGBA16F -> RGBA8 逐级降级
//  (GL_EXT_color_buffer_float 的支持情况会记录在日志里); 全部失败时错误串里会带上
//  真实的 glCheckFramebufferStatus 返回值(十六进制)、尺寸与内部格式。
//  需要浮点累积的负载在降级后精度下降, 但仍按同样口径计时。
//
//  程序表: 每个 ProgId 一项 {顶点着色器, 片元着色器} (见 gpu7_progTable), 表项数量
//  由 static_assert 与 ProgId 枚举强绑定; 用 GL_POINTS 绘制的程序必须配会写
//  gl_PointSize、且顶点位置只由 gl_VertexID 决定的顶点着色器。
//
//  显存: 所有 4000x3000 / 3840x2160 负载在 gpu7Run() 内创建、函数返回前释放,
//  同一时刻只有一项负载占用显存, 峰值约 600MB(SR), 其余项 < 350MB。
// ============================================================================

#include <string>

// 负载数量(固定 11)
int gpu7Count();

// 负载英文名; id 越界返回空串
std::string gpu7Name(int id);

// 初始化 EGL/GLES3 离屏上下文(懒加载, 只初始化一次)
// 成功返回 "", 失败返回错误文本
std::string gpu7Prepare();

// 是否已经成功初始化
bool gpu7Ready();

// 同步跑完第 id 项, 返回 JSON:
//   成功: {"ok":true,"ms":123.4,"metric":"87.2","unit":"Mpx/s","fps":45.6,
//          "score":12345.6,"basis":"官方单位 / 换算 / 系数来源说明"}
//         score = 0 表示该项未计分(单位语义不可比, 原因写在 basis)
//   失败: {"ok":false,"error":"..."}
std::string gpu7Run(int id);

// GPU「跑满判据」(2026-10 新增): 在**计时区间之外**用同一份负载(同一个负载函数、同一份
// 尺寸与算法、一个字都没改)跑两档(半量帧 / 满量帧), 两档的耗时与读数一律丢弃, 只用来判:
//   ① 满量档每帧墙钟里"等 GPU 排水"占多少(其余是我们在 CPU 侧递交 GL 命令 + 驱动背压);
//   ② 加量之后吞吐还涨不涨(涨 = 还没进平台期)。
// 返回 JSON:
//   {"ok":true,"loadId":N,"name":"..","verdict":"REACHED|NOT_REACHED|NOT_APPLICABLE",
//    "text":"GPU 跑满判据 -> ...(一行中文, 可直接显示)", 以及各原始读数}
//   {"ok":false,"error":"..."} = 参数不对或 GL 上下文起不来(调用方按"本项不适用"处理)。
// 阈值(排水占比 >= 90% / 两档吞吐比 >= 0.97)是我们自己定的, 文本里原样写出。
// 它不改任何 GPU 负载的尺寸 / 算法 / 计分, 也不进 metric / k / conv / 复合分。
std::string gpu7Fullness(int id);

// GPU 复合分 = 已计分项的几何平均(11 项里 Feature Matching / Horizon Detection 未计分)。
// scores 传 nullptr / count<=0 时用最近一次 gpu7Run 的单项分。
// 返回 JSON: {"ok":true,"mode":"gpu","composite":..,"count":..,"items":[..],"basis":".."}
std::string gpu7Composite(const double* scores, int count);

// 最近一次错误文本(无错误时为空)
std::string gpu7LastError();

#endif
