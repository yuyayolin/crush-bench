#ifndef AURORA_SN_RENDERER_H
#define AURORA_SN_RENDERER_H

// ============================================================================
//  极光跑分 · GPU 小节 "Aurora Nomad Light" (SNL) —— 3DMark Steel Nomad 风格
//  纯原生 C++ / OpenGL ES 3.0 离屏(EGL pbuffer, 无 XComponent, 无线程)
// ----------------------------------------------------------------------------
//   本小节 完全独立: 不并入 CS1 GPU 的 11 项, 不并入 GB7 CPU 16 项,
//     不参与任何 CS1 单项分 / 复合分 / 总分。UI 必须单独一节展示。
//     导出方式与 gpu7 一致(独立 napi 模块 libaurorasn.so), 因此它与
//     libauroragpu7.so / libaurorabench.so 之间没有任何硬 DT_NEEDED:
//     本模块加载失败不会拖死跑分主模块(工程对 NNRt 已有一模一样的结论,
//     见 CMakeLists.txt 里那段长注释)。
//
//  ===========================================================================
//  为什么要有这一节(缺口证据, 不是"多做一个功能")
//  ---------------------------------------------------------------------------
//  现有的 GPU 小节是 11 项套件(light compute / 图像处理型)。公开的
//  3DMark Steel Nomad Light(下称 SNL)代际差是
//        991 (Mate 80 Pro Max / 麒麟 9030 Pro, 用户拍屏一手证据) / 454 (9020) / 303 (9000S) = 3.27 倍
//  而本工程 CS1 GPU 11 项在手机 / 平板之间只跑出 5064 / 2563 = 1.98 倍, 差 1.65 倍。
//  两者测的不是同一种负载: GB7 的 11 项以 1080p 图像算子为主, 而 SNL 是
//  game-like 的 deferred/PBR 着色 + GPU-driven 几何, 吃的是 GPU 的
//  片元着色吞吐与带宽, 而不是"算子启动开销"。本小节补的就是这一半。
//
//  ===========================================================================
//  设计目标: 一把跨代可比的尺子(9000S -> 9050 Pro -> 9060 Pro -> ...)
//  ---------------------------------------------------------------------------
//  1) 负载与设备完全无关: 同一份工作、同一份常量、同一份 GLSL。
//     本文件里没有任何按机型 / SoC / 年份 / 分辨率 / 屏幕尺寸的分支,
//     也没有任何 "如果是麒麟就..." 的判断。判据: 源码里不出现设备名字符串,
//     整份负载只有 loadShape 等编译期常量(见 verify_sn_unified_ruler.py)。
//  2) 吞吐量口径是长期可比的根本: 报的量全部是 "工作量 / 时间"。
//     芯片快 10 倍 -> 数字大 10 倍 -> 不需要改任何东西。
//       主口径 throughput = (MpxPerFrame x 帧率)  单位 Mpx/s
//       帧率             = 帧 / 秒
//       分数             = 帧率 x kScorePerFps     (与 3DMark 公开口径同形, 见下)
//  3) 规模自适应, 但不改"每单位工作量": 未来芯片可能让一整趟 1080p
//     着色在 1 ms 内跑完, 那样的数字不可信(启动开销占比过高)。
//     自适应机制只有一个自由度 —— 每帧重复多少趟 (passesPerFrame),
//     每一趟永远是"1920x1080 全屏幕 x 固定的两趟管线", 每像素指令数是
//     编译期常量, 不随趟数变。详见 kPassLadder / snRun() 里的注释。
//     不允许通过"改每像素运算量 / 改分辨率 / 改场景规模"来调时间:
//     那会让新旧分数不可比。
//  4) benchmark 级版本号: kBenchVersion (见下) 与 App 版本号是两回事。
//     任何一次改动负载或口径都必须 递增 kBenchVersion, 因为递增之后
//     新旧分数 不可比(这是 3DMark "benchmark version" 的同一条规则)。
//     结果 JSON 与报告文本里都带这个号, 并明确写出"跨版本不可比"。
//  5) 为未来留出空间: 结果里写出这一轮用到了什么、没用到什么
//     (核数 / SMT / 向量宽度 / GPU 标识 / GL 版本 / 扩展), 让 16 核 / SVE /
//     新 GPU 出现时一眼能看出尺子有没有被喂满。
//  6) 不许为了数字好看做任何设备相关调优: 尺子的价值在于诚实。
//
//  ===========================================================================
//  负载构成(为什么它不会撞 CPU 递交瓶颈)
//  ---------------------------------------------------------------------------
//  一帧 = passesPerFrame 趟, 每趟固定两趟 1920x1080 全屏管线:
//
//    趟 A  "G-buffer / 表面求解"(GL 一次 instanced 绘制, 见下)
//          顶点着色器用 gl_InstanceID 在 GPU 侧切出一张 1920x1080 的
//          屏幕空间网格(每实例 32x32 px = 1024 个片元), CPU 不递交任何顶点数据;
//          片元着色器对每个像素做: 相机光线 -> 对程序化高度场(值噪声 fBm)
//          做固定 9 步光线步进 -> 命中(或天空) -> 写出 G-buffer
//          (位置 / 法线 / 反照率+粗糙度 / 线性深度), RGBA8 x2 + D24 depth。
//    趟 B  "deferred PBR 着色"
//          同样的 instanced 全屏网格, 读回 G-buffer, 做:
//          太阳直射 + 固定 4 抽样 PCF 阴影 + 3 盏点光源(GGX 高光) +
//          程序化天空环境光 + ACES 色调映射 + 暗角 + 颗粒, 写出到颜色目标。
//
//  * **GPU-driven**: 几何完全不来自 CPU —— 顶点数据为零(无 VBO 属性,
//    glDrawArraysInstanced 只用 gl_VertexID / gl_InstanceID 生成位置),
//    整个场景只由 GPU 侧的噪声场与网格划分决定。
//  * 极少 draw call: 每趟 2 次 draw call, 与设备无关、与工作规模无关,
//    与 pass 数成正比(每次都是 glDrawArraysInstanced(GL_TRIANGLES, 0, 6, N)),
//    CPU 侧的 GL 调用总数是常量级(见 gpuBoundEvidence.drawCallsPerFrame 与
//    submission.apiCallsPerFrame)。这就是"不撞 CPU 递交瓶颈"的直接证据之一。
//  * 同步点极少: 每帧 1 次 glFinish(计时用), 没有 glReadPixels 回读,
//    没有纹理上传, 没有逐单元 CPU 循环。
//  * 重着色: 每像素两趟共约数百次 ALU + 少量纹理采样, 全部是 modern
//    deferred/PBR 形态的着色(见 workload.opsPerPixelPerPass 的说明)。
//  * 每帧工作量固定: 同一帧内所有趟做完全相同的工作(只换种子 uniform),
//    pass 数在一轮运行内是常量。
//
//  ===========================================================================
//  分数口径与 3DMark 公开值的对照依据(诚实说明 + 标定方法)
//  ---------------------------------------------------------------------------
//  UL 官方公开(支持文章 44002528075 "How is the 3DMark Steel Nomad Light score
//  calculated?"): 总分 = 图形分 = 图形测试的平均帧率 x **135**。
//  即 3DMark SNL 分数与 FPS 严格成正比, 比例常数官方明文为 135。
//
//  本小节的分数与它 同形:  score = fps x kScorePerFps。
//  kScorePerFps 的取值必须一次说清, 因为它决定了能不能和公开值直接比:
//
//      score_snl    = fps_snl x 135                       (UL 官方)
//      fps_aurora   = fps_snl / kResolutionScaleRatio     (像素数少 -> 帧率高)
//      两者相等  =>  kScorePerFps = 135 x kResolutionScaleRatio = 135 x 0.5625
//                                = 75.9375
//
//      * k3dmarkNomadScale = 135 —— UL 官方明文, 不是我们编的。
//      * kResolutionScaleRatio = (1920x1080) / (2560x1440) = 0.5625 ——
//        UL 官方"Steel Nomad Light Graphics Test"页面写明 SNL 的渲染分辨率是
//        2560x1440; 本小节固定在 1920x1080。
//      *  这里是乘 r 而不是除 r: 除以 r 会得到 240, 那等于把分辨率差算了
//        两遍, 分数会凭空大 3.16 倍, 与公开的 991/454/303 对不上。
//        "两个负载每像素速度相同"这一步是假设, 已写进 risks; 标定改的是
//        kWorkloadScale(唯一自由参数), 且不影响任何比值。
//
//   诚实的边界(必须一起报给用户) 
//    a) 三条公开值的查证强度完全不同(2026-10 离线核对之后的结论, 逐条写进了
//       结果 JSON 的 reference.points[].verification / note):
//         K 9020 = 454      已证实: UL 官方成绩库机型行(Huawei Pura 80 Pro+ / Ultra,
//                           Kirin 9020, Maleoon 920 -> 454, 为用户提交结果中位数),
//                           nanoreview 同为 454。这一条可以当标定锚点。
//         K 9030 Pro = 991  用户拍屏的一手证据(2026-08-31 截图): 3DMark 应用在
//                           HUAWEI Mate 80 Pro Max 上跑 Steel Nomad Light, 总分 991 /
//                           平均帧率 7.34 FPS; 自洽核对 991 / 135 = 7.3407 FPS ≈ 7.34 FPS。
//                           保留的 caveat: 公开渠道(UL 官方成绩库)查不到该机型条目;
//                           与其他 9030 Pro 读数 956(MatePad Pro Max)/ 998(Mate X7)/
//                           993(转载页)不完全一致(950~1000); 极客湾/ITHome 曾推算相对 9020
//                           只快 76%(约 799), 与这条一手读数冲突 —— 冲突记录, 不替用户取舍。
//         K 9000S = 303     未证实: 只有 nanoreview 一处; UL 成绩库没有 Mate 60
//                           系列条目, Notebookcheck 的 Maleoon 910 页为空白/0。
//       另外 UL 公开库明确是用户提交结果的中位数(含过热/后台等不理想条件下的
//       结果), 同一颗 SoC 在不同机型上本就能差出一倍(9020: 368~557)。
//       => 本小节只保证"同一把尺子上的比值", 绝对分数在标定前不要当 SNL 绝对值。
//       另: UL 官方 44002528070 / 44002528074 明文 SNL 渲染分辨率是 2560x1440
//       所有平台一致(不是设备屏幕分辨率), 这一点是已证实的, kScorePerFps 的
//       分辨率归一化建立在这条明文上。
//    b) 本小节的负载与 SNL 不是同一份负载, 因此绝对的像素级速度不同,
//       分数不会与 SNL 逐分相等(与工程对 GB7 的口径完全一致:
//       同公式、同锚点缩放关系, 不同实现)。
//    c) 真正长期可靠的是比值与吞吐量: 只用本小节的分数做跨代比较
//       (同一把尺子), 不要在没标定的情况下把它当 SNL 的绝对值。
//    d) 标定方法(用户可在真机上一次做完, 之后永久有效):
//       在同一台设备上先后跑 3DMark SNL 与本小节, 记 fps_snl 与
//       fps_aurora, 令 kScorePerFps := 135 x fps_snl / (fps_aurora x 0.5625)
//       即可把两把尺子在这台设备上对齐; 若多台设备算出的 k 一致,
//       说明这个线性假设成立, 之后按 k 上报即可与公开值直接对照。
//       在标定之前, 结果 JSON 里 calibration.status = "UNCALIBRATED",
//       并给出 predictionAtNomadReference 表(公开值 -> 预期帧率 / 预期本小节分数)
//       供用户在真机上一行对一行地核对。
//
//  ===========================================================================
//  结果 JSON 契约(napi 层只做字符串搬运, 这里写全)
//  ---------------------------------------------------------------------------
//  snRun(optionsJson?) / snPrepare() / snLastError() 返回字符串; 失败时是
//  {"ok":false,"error":"...","lastError":"..."} —— 不静默返回 0。
//  成功时的完整形状(字段含义见 sn_renderer.cpp 的 buildJson):
//  {
//    "ok": true,
//    "section": "GPU-SNL",              // ← UI 用这个值把本小节单独显示
//    "scored": false, "gb7Item": false, // ← 明确: 不进 GB7 任何分数
//    "kind": "gpu-nomad-light",
//    "benchVersion": 1,                 //  benchmark 级版本号(与 App 版本号无关)
//    "benchVersionText": "...跨版本不可比...",
//    "format": { "revision": "snl-1", "hash": "..." },
//    "score": {...}, "fps": {...}, "throughput": {...},
//    "work": {...}, "scale": {...}, "gpuBoundEvidence": {...},
//    "selfProof": {...}, "crossGeneration": {...}, "reference": {...},
//    "ruler": {...}, "rendering": {...}, "gpu": {...}, "notes": "..."
//
//  完整样例见本目录 sn_json_sample.json(由 verify_sn_unified_ruler.py 用
//  离线镜像生成, 保证与实现里的字段名逐字一致)。
// ============================================================================

#include <string>

// ===========================================================================
//   追加(验收标准: "要有跑分的意义, 像其他主流跑分软件一样")
// ---------------------------------------------------------------------------
//  主流跑分软件普遍具备的性质, 本小节逐条对照:
//
//  (1) 可重复性被量化: 默认把整个小节跑 REPEATS 次(默认 3, 可用 optionsJson 的
//      "repeats" 改), 每一轮都完整预热+计时; 结果里给出每一轮的原始值, 以及
//      中位 / 最小 / 最大 / 相对离散度(离散度 = (max-min)/中位 x 100%)。
//      同时给出一个醒目的可信度判断 credibility.verdict:
//          RELIABLE   离散度 <= 2%
//          FAIR       2% < 离散度 <= 5%
//          UNRELIABLE 离散度 > 5%  -> 该项数字仅供参考(仍然照样报出来, 但不冒充可信)
//      不用"取最好一次"掩盖不稳定; 稳态值取中位(不是最好)。
//  (2) 分数只随硬件变: environment 块记录本次的 CPU 频率(中位/最小/最大, 逐核与
//      可用核集合口径分开)、SoC 热区温度(最小/中位/最大)、采样窗口, 并给 state:
//          STABLE / WARMING_UP / THROTTLED / UNKNOWN
//      判据全部是算出来的, 且首轮 vs 末轮的吞吐差会单独列出(热身效应记录)。
//       诚实边界: 麒麟平台的 GPU 频率/温度没有可读的 sysfs 节点(已核实), 本块报的
//        是 CPU 频率 + SoC 热区温度 —— 它们是"整机 DVFS/热状态"的间接证据, 不是
//        GPU 自己的频率。真正直接的 GPU 侧证据是同一次运行内每趟 GPU 耗时的漂移
//        (scale.perPassMsMedian) 与 cpuGpuScaling 的比值。这一段必须一起看。
//  (3) 参考分对照表: referenceComparison 把本机结果与公开真值并列, 每条都带
//      source / sourceKind(OFFICIAL_3DMARK_SUPPORT / THIRD_PARTY / ...) / scope / caveat;
//      查不到的项写 "查不到", 第三方数据不写成官方。
//  (4) 每项一句话真实用途: realUse(见 sn_renderer.cpp 的 kRealUse 表)。本小节是
//      "非光追的 game-like 场景: 地形求交 + 延迟着色 + 多光源 + 阴影", 代表现代手游的
//      主渲染通路。
//  (5) 可解释性: score.basis 里写明公式 score = fps x kScorePerFps、k 的来源、
//      单位(fps / points)与"为什么与官方同形/不同口径"。
//  (6) 不被跑分模式污染: environment.probe 里带允许核集合(sched_getaffinity 原文)、
//      逻辑核/物理核、governor 原文、温度原文与逐核频率原文, 让任何人可以据此质疑
//      这个分数; 宁可暴露不利事实, 也不许让分数看起来好看。
// ===========================================================================

// benchmark 级版本号。 任何一次改动负载 / 口径 / 单位 / 计分公式, 都必须 +1。
// 递增之后新旧分数不可比; 这一点写在结果 JSON 的 benchVersionText 里。
// 1 = 首版(Aurora Nomad Light, 两趟 1080p 固定管线 + 每帧趟数自适应)。
extern const int kSnBenchVersion;

// 本小节的名字与版本文本(供 UI 直接显示)
std::string snSectionName();      // "GPU-SNL"
std::string snWorkloadName();     // "Aurora Nomad Light"
std::string snBenchVersionText(); // 含"跨版本不可比"的完整中文说明

// 初始化 EGL/GLES3 离屏上下文(懒加载, 只初始化一次)。成功返回 "", 失败返回错误文本。
std::string snPrepare();

// 是否已经成功初始化
bool snReady();

// 同步跑完整个小节, 返回结果 JSON(形状见文件头契约)。
// optionsJson: 扁平、值全为数字的 JSON 对象, 全部可选:
//   "passesPerFrame"  固定每帧趟数(0/缺省 = 自适应, 这是默认且推荐的做法)
//   "measureFrames"   每轮计时帧数(缺省 8, 上限 16)
//   "repeats"         整个小节重复几轮(缺省 3, 1..9) —— 可重复性/离散度就是靠它量的
//   "gapMs"           轮与轮之间的冷却间隔毫秒(缺省 1500, 0..30000)
//   "targetMs"        自适应每帧目标毫秒数(缺省 12, 夹在 2..200)
//   "maxPasses"       趟数上限(缺省 64, 夹在 1..4096)
// 传空串 = 全默认。生效值原样回写在结果 JSON 的 config 里。
std::string snRun(const std::string& optionsJson);

// 最近一次错误文本(无错误时为空)
std::string snLastError();

// 上一轮 snRun 的单项分(供上层做跨行/跨次比较; 未跑过时为 0)
double snLastScore();

#endif
