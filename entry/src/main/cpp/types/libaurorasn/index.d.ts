// libaurorasn.so —— Aurora Nomad Light (GPU-SNL) 小节的类型声明
//
//  本小节完全独立: 不并入 GB7 的 GPU 11 项 / CPU 16 项, 不参与任何 CS1 分数。
//   结果 JSON 里带 "section":"GPU-SNL" / "scored":false / "gb7Item":false,
//   界面请用 section 这个值把它单独显示成一小节。
//
// 典型接线(ArkTS):
//   import { ready, prepare, run, benchVersion, versionText, sectionName,
//            workloadName, lastScore, lastError } from 'libaurorasn.so';
//
//   const err = prepare();                    // "" = 成功
//   if (err !== '') { /* 显示 err, 跳过本小节 */ }
//   const json = run();                       // 全默认: 自适应趟数
//   const r = JSON.parse(json);
//   // 单独一节显示: r.section / r.score.value / r.fps.value / r.throughput.value
//   // 自证:        r.gpuBoundEvidence.verdict / r.work.pixelsPerFrame
//   // 跨代比对:    两次结果都取同一个 r.benchVersion 下的 score.value 相除
//
// run 的 optionsJson(扁平、值全为数字, 全部可选; 传空 = 全默认):
//   passesPerFrame : 固定每帧趟数(0/缺省 = 自适应, 推荐)
//   measureFrames  : 每轮计时帧数(缺省 8, 2..16)
//   repeats        : 整个小节重复几轮(缺省 3, 1..9) —— 可重复性/离散度就是靠它量的
//   gapMs          : 轮与轮之间的冷却间隔毫秒(缺省 1500, 0..30000)
//   targetMs       : 自适应每帧目标毫秒(缺省 12, 2..200)
//   maxPasses      : 趟数上限(缺省 64, 1..4096)
//
//  验收标准相关(报告里必须一起展示, 否则"跑分"就没有意义):
//   r.repeatability.credibility.verdict  -> RELIABLE / FAIR / UNRELIABLE(>5% 仅供参考)
//   r.repeatability.statistics.{fps,score} -> 中位 / 最小 / 最大 / 相对离散度(代表值取中位)
//   r.repeatability.stability              -> 首轮 vs 后续轮(热身效应)、每趟耗时漂移
//   r.environment.stability.state          -> STABLE / WARMING_UP / THROTTLED / UNKNOWN
//   r.environment.temperature / cpuKhz     -> 温度与运行时频率(中位/最大)
//   r.referenceComparison                  -> 与公开真值并列(带 source / sourceKind)
//   r.realUse                              -> 这项代表什么真实用途(一句话)
//   r.validity.pass                        -> 综合是否可信(false 时务必在界面上标出来)

/** benchmark 级版本号(与 App 版本号无关)。改动负载/口径会 +1; 版本不同不可比。 */
export const benchVersion: () => number;

/** 含"跨版本不可比"的完整中文说明, 可直接显示。 */
export const versionText: () => string;

/** "GPU-SNL" —— 界面用它把本小节单独成节。 */
export const sectionName: () => string;

/** "Aurora Nomad Light"。 */
export const workloadName: () => string;

/** 离屏 EGL/GLES3 上下文是否已就绪。 */
export const ready: () => boolean;

/** 初始化离屏上下文与着色器。返回 "" = 成功, 否则是错误文本。 */
export const prepare: () => string;

/**
 * 跑完整个小节。返回结果 JSON(同步, 通常 0.3~2 秒)。
 * 失败时是 {"ok":false,"error":"...","lastError":"..."} —— 不静默返回 0。
 * 关键字段: score.value / fps.value / throughput.value(Mpx/s) /
 *          work.passesPerFrame / scale.basisText / gpuBoundEvidence.verdict /
 *          validity.pass / benchVersion
 */
export const run: (optionsJson?: string) => string;

/** 最近一次错误文本(无错误时为空串)。 */
export const lastError: () => string;

/** 上一轮分数(0 = 还没跑过)。 */
export const lastScore: () => number;
