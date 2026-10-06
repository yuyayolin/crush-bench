// ---------------------------------------------------------------------------
// 布局计划(纯函数, 无任何 import、无任何 @kit / ohos 运行时依赖)
//
//   总原则(2026-10 重写, 优先级最高)
//   1) 布局决策只依赖实时窗口的 vp 宽高(以及由它派生的一切: 宽高比、可用高度)。
//      不许有任何设备型号 / 芯片 / 机型的判断参与布局; 1:sqrt(2) / 16:10 / 9:16 / 1:1 /
//      21:9 …… 任何比例都不许被当成"预期" —— 识别到什么就是什么, 并按这个窗口挑最合适的排法。
//   2) 设备型号表(service/DeviceModelTable.ets)只允许用于显示(把型号码翻成传播名给人看),
//      不参与布局决策。DeviceProfile 把 deviceType / tableFormFactor 传进来只为了让
//      文案说得清来源, 本文件里没有任何一处用它们改变布局参数。
//   3) 形态"编号 / 名字"只用于给人看与日志: 它们不参与任何分支判断 ——
//      把 nameOf / indexOf 整个删掉, 布局结果也必须一个字段都不变
//      (离线用例 ref/layout_cases.test.mjs 的 K 节会把这件事断言出来)。
//   4) 对给定窗口要"自适应成最佳显示效果": 列数按可用宽度定、竖向预算按可用高度定、
//      字号与间距按可读性定。任意窗口都不溢出、不裁切、关键内容可见、留白合理。
//
//   折叠状态(display.getFoldStatus)是系统给的运行时事实, 可以用, 但它:
//     · 不参与几何判定(不决定编号, 不改任何布局参数);
//     · 不能覆盖实测窗口尺寸 —— 一旦它与窗口尺寸冲突(例如系统说 folded 而窗口是 900vp 宽),
//       一律以窗口尺寸为准, 并把冲突写进 basisOf / degradeOf(见 foldConflictOf)。
//
//  为什么单独放一个文件、且做成纯函数:
//   1) PC(2in1)上窗口可自由缩放、折叠屏可随时展开/折叠、分屏会改变宽高比 ——
//      "这台机器是什么型号"不足以决定布局, 必须以实时窗口宽高为唯一依据。
//   2) 本文件不 import 任何东西, 因此可以原样抽出来在 Node 里跑离线用例
//      (见 ref/layout_cases.test.mjs), 判定表可核对、可回归。
//   3) 同一个输入必然得到同一个输出(幂等); resolveShapeId 额外带 ±HYST_VP 的
//      边界抖动抑制 —— 它只影响显示的编号, 不影响布局参数。
//
//  本文件只影响观感(列数 / 宽度 / 按钮尺寸 / 字号缩放), 不参与任何测试项的
//  计算、顺序与计分。
//
//   关于"典型窗口尺寸"的诚实说明(2026-10-05, 2026-10 更正)
//   以前本文件里写过"阔折叠展开约 700x900vp" —— 那是编的举例, 不是实测。
//   后来又把 Pura X(非 Max) 的面板数据(内屏 2120x1320px = 1.606 = 16:10, 出处官网
//   参数页, 本地留档 ref/consumer.huawei.com_cn_phones_pura-x_specs_)套到了
//   Pura X Max 上, 写成"阔折叠内屏就是 16:10、展开后是横向窗口" —— 这条假设是错的。
//   目标机型 Pura X Max(HOP-AL00/AL10)的内屏是 **2584 x 1828 px**,
//   2584/1828 = 1.4136 ≈ sqrt(2) —— 即 1:sqrt(2) 的竖长内屏
//   (出处: 官网参数页留档 ref/consumer.huawei.com_cn_phones_pura-x-max_specs_ 第 1933-1937 行
//   "分辨率 / 外屏：1848 × 1264 像素 / 内屏：2584 × 1828 像素"; 机型行见 ref/DEVICE_MODELS.md:7)。
//   真机数据交叉印证: 设备行实测 "窗口=1828x2584px ÷ 密度 2.750 = 664x940vp",
//   664/940 = 0.706 ≈ 1/sqrt(2)(0.7071) —— 展开态就是竖长的。
//    但这些数值一个都不进判定: 判定只吃实时窗口宽高。上面这些只是为了让人知道
//     "阈值落在真实机器上是哪一档", 以及不要再把错误的前提抄回来。
//   (设备行的 "窗口=WxHvp 形态=..." 就是为了一跑完就能从日志里读到真机尺寸, 见
//    service/BenchRunner.ets 的 deviceLabel()。)
// ---------------------------------------------------------------------------

// ---- 几何阈值(vp / 宽高比) ----
//  改这里 = 改"编号怎么叫", 不改布局参数(布局参数用的是下面那批自适应常量与公式)。
//  仍然写成公理式阈值(与任何具体面板无关), 任何真实尺寸进来都能算出编号。
const AR_ULTRA_NARROW: number = 0.40; // 以下 = 极窄(300x800 这种)
const AR_TALL: number = 0.80;         // 以下 = 竖长
const AR_WIDE: number = 1.25;         // 以上 = 横向
const AR_FLAT: number = 1.70;         // 以上 = 扁宽
const AR_ULTRA_FLAT: number = 2.40;   // 以上 = 极扁(且高 < 400vp 时按"矮"处理)
const AR_ULTRA_FLAT_EXTREME: number = 3.0;  // 以上: 无论多高都算极扁(高度一定不够)
const W_TINY: number = 320;           // 以下 = 极窄窗口(只允许单列满宽)
const H_FLAT: number = 340;           // 以下 = 高度极矮(关键内容优先)
const H_SHORT: number = 400;          // 以下且宽高比 >= 2.4 = "矮窗口"
const W_ULTRA_WIDE: number = 1100;    // 横向时宽 >= 它的窗口单列一档(可用宽度足够三列)
const AR_OUTER_LIKE: number = 0.50;   // 窄窗里宽高比 <= 它的按"极窄竖长"叫(形如外屏/窄分屏)
const MIN_SIDE_SQUARE: number = 730;  // 近方形时, 最小边小于它按"小内屏"叫, 否则按"大内屏"叫

// ---- 自适应布局常量(全部由窗口宽高算出, 不看机型 / 不看比例标签) ----
const MIN_CARD_VP: number = 300;      // 结果卡最小可读宽度
//   为什么是 300: 结果卡内 "GB7 明细行" 有 54 + 88 + 60 = 202vp 固定列(已按最小可读宽收紧),
//   加上左右内边距与至少 4 个汉字的项名。低于 300vp 项名就只能显示省略号 ——
//   那属于"内容被裁切", 不允许。这个下限同时也是 columns 的计算依据:
//   结果区放不下 columns 张 300vp 的卡片时会自动退成更少列, 因此永远不会有横向溢出。
const MIN_LEFT_COL_VP: number = 240;  // 分栏时左栏(操作区)的最小可读宽度
const MIN_RIGHT_COL_VP: number = 280; // 分栏时右栏(结果区)的最小可读宽度
const DIV_MIN_W: number = 600;        // 窗口宽 < 它就不分栏(两栏都太窄, 按钮长文案会折行)
// 分栏比例写成 12 栅格的整数比 —— 因为界面就是用 GridRow/GridCol 的 12 栅格排的。
//    这一点必须严格一致: 计划里算出"右栏 636vp、放得下 2 列", 界面就必须真的给右栏 636vp,
//     否则会出现"计划说不溢出、界面却溢出"。所以左右栏宽度直接由 span 换算:
//        左栏 = 内容列宽 x leftSpan / 12, 右栏 = 内容列宽 - 左栏。
//   7:12 与 5:12 的差值不是四舍五入误差, 是 8.33% 的宽度 —— 不能混用。
const DIV_SPAN_LEFT: number = 5;      // 常规分栏: 左 5 / 右 7
const DIV_SPAN_RIGHT: number = 7;
const DIV_SPAN_LEFT_WIDE: number = 4; // 内容列很宽(>= 1100vp)时: 左 4 / 右 8
const DIV_SPAN_RIGHT_WIDE: number = 8;
const DIV_SPAN_WIDE_AT: number = 1100;  // 内容列宽 >= 它的窗口用 4:8
const COL_CAP_WIDE: number = 560;     // 宽 >= 它允许 2 列结果卡
const COL_CAP_XWIDE: number = 760;    // 宽 >= 它允许 3 列结果卡
const COL_CAP: number = 3;            // 再多列会窄到读不了, 硬上限 3
// 内容列最大宽度阶梯(vp): 窗口越宽留白越多, 但内容不至于宽到需要转头去读 ——
//   [0,560)->自适应(最多 480) / [560,720)->680 / [720,960)->820 / [960,1240)->1000
//   / [1240,1600)->1240 / [1600,+) -> 1440(与 2in1 同一档)
const CONTENT_LADDER_W: number[] = [560, 720, 960, 1240, 1600];
const CONTENT_LADDER_V: number[] = [680, 820, 1000, 1240, 1440];
const CONTENT_NARROW_MAX: number = 480;
// 按钮直径的上下限(实际公式见下面 heroDiaFor: clamp(窗口宽^0.58 x 6.5, 72, 300),
//   再乘一道"不超过窗口宽 85%"的硬约束; 窗口太矮时还会被 fitHeight 的第 ① 步压小到下限为止)。
//   这里以前留着一句"clamp(窗口宽 x 系数, 88, 340) + 分段系数 0.38/0.32/0.26/0.20" —— 那是旧版设计,
//    与本文件的 heroDiaFor 和下面那段说明(SDK 里逐条核过)都不符, 已删除, 免得被后来的改动当成规格抄回去。
const HERO_MIN_VP: number = 72;       // 主按钮最小直径(见下面 heroDiaFor 的说明)
const HERO_MAX_VP: number = 300;
// 结果明细行高: 窗口越矮行越紧(但不能低于 28, 否则 12~13fp 文字会挤)
const ROW_MIN_VP: number = 28;
const ROW_MAX_VP: number = 44;
// 字号缩放: 按 min(宽 x 0.0016, 高 x 0.0011) 的较小者(观看距离更远 -> 略放大; 小窗 -> 略缩小)
const FONT_MIN: number = 0.92;
const FONT_MAX: number = 1.08;
const HYST_VP: number = 8;            // 编号边界抖动抑制带宽(vp), 只影响编号显示

// ---- 竖向空间预算(vp) ----
//  解决的是"窗口又宽又矮"这一类: 高度才是稀缺资源时, 必须单独算一笔账。
//  预算的口径(这就是"放得下"的定义, 自检块里逐项显示, 离线用例逐条断言):
//      needVp = 顶栏+标签栏(CHROME_VP) + 顶部设备信息(TOP_INFO_VP)
//             + 主按钮块(heroDia + HERO_TAIL_VP) + 至少 2 行结果(MIN_RESULT_ROWS x 行高)
//  放不下时按固定顺序压, 每一步都可核对, 且不隐藏任何内容:
//     ① 压主按钮直径(下限 = HERO_MIN_VP, 不再往下压)
//     ② 压卡片间距与内边距(各 -4vp, 有下限)
//     ③ 顶部「设备」卡切紧凑版(150 -> 70vp: 只留传播名 + 型号码 + 核心数)
//     ④ 压到底仍然放不下 -> scrollOnly = true: 结果区靠纵向滚动(内容一个都不隐藏)
const CHROME_VP: number = 96;       // 顶栏(约 44vp) + 底部标签栏(52vp)
const TOP_INFO_VP: number = 150;    // 顶部"设备"卡(传播名 + 型号码 + SMT + 拓扑 + hardwareModel)
const TOP_INFO_COMPACT_VP: number = 70;  // 第 ③ 步: 设备卡切"紧凑版"之后的高度
const HERO_TAIL_VP: number = 16;    // 主按钮块与结果区之间的间隔
const RESULT_ROW_VP: number = 34;   // 一条结果明细行的估算高度(基准档; 实际值按窗口高算, 见 rowVpOf)
const MIN_RESULT_ROWS: number = 2;  // 首屏至少要能看到 2 行结果
// 紧凑档(第 ② 步生效时)的取值: 间隔 16 -> 12, 每条结果行 -4(有下限)。
// 这两项都要在 HeightBudget 里记出来, 保证"各项之和 == needVp"这条恒等式成立。
const TAIL_COMPACT_VP: number = 12;
const ROW_COMPACT_DELTA: number = 4;

// ---- 形态编号(只用于显示与日志; 关于页「布局自检」直接显示这个编号, 与需求表一一对应) ----
//   编号由纯几何规则选出(见 shapeIdOf), 不参与任何布局参数的计算。
export const SHAPE_S0: string = 'S0';   // 窗口尺寸未取到(兜底)
export const SHAPE_S1: string = 'S1';   // 窄窗竖长
export const SHAPE_S2: string = 'S2';   // 窄窗阔直
export const SHAPE_S3: string = 'S3';   // 窄窗里极窄(0.40 < 宽高比 <= 0.50, 宽 < 600vp)
export const SHAPE_S4: string = 'S4';   // 近方形(小边 < 730vp)
export const SHAPE_S5: string = 'S5';   // 竖长宽窗(>= 600vp)
export const SHAPE_S6: string = 'S6';   // 近方形(小边 >= 730vp)
export const SHAPE_S7: string = 'S7';   // 横向(1.25 ~ 1.70)
export const SHAPE_S8: string = 'S8';   // 扁宽(1.70 ~ 2.40)
export const SHAPE_S9: string = 'S9';   // 极窄(宽 < 320vp 或 宽高比 < 0.40)
export const SHAPE_S10: string = 'S10'; // 极扁(宽高比 >= 3.0) 或 矮(高 < 400vp 且 宽高比 >= 2.40)
export const SHAPE_S11: string = 'S11'; // 横向且窗口很宽(1.25 ~ 1.70 且 宽 >= 1100vp)
export const SHAPE_S12: string = 'S12'; // 已废弃的编号(旧"展开态"档); 现在不会再被选中, 保留仅为兼容字段

// 折叠状态取值(与 service/DeviceProfile.ets 的 FOLD_* 文案解耦: 这里用稳定英文键)
export const FS_NONE: string = 'none';
export const FS_EXPANDED: string = 'expanded';
export const FS_FOLDED: string = 'folded';
export const FS_HALF: string = 'half';
export const FS_UNKNOWN: string = 'unknown';

// ---- 判定输入 ----
//   只有 widthVp / heightVp 参与布局决策; 其余三个字段只用于"把话说清楚"(命名 / 依据 / 降级),
//    它们在任何分支里都不会改变一个布局参数(离线用例 K 节专门断言这一点)。
export interface ShapeInput {
  widthVp: number;          // 实时窗口宽(vp); 0 = 还没取到(此时所有布局都用兜底窗口, 并标注)
  heightVp: number;         // 实时窗口高(vp); 0 = 还没取到
  foldState: string;        // FS_* 之一(系统给的运行时事实; 不参与几何判定, 也不能覆盖窗口尺寸)
  foldApiOk: boolean;       // 折叠屏接口(display.isFoldable / getFoldStatus)是否可用
  deviceType: string;       // deviceInfo.deviceType(已小写); 只用于显示
  tableFormFactor: string;  // 内置机型对照表标注的形态; 只用于显示, 不参与布局决策
}

// ---- 竖向空间预算(口径见文件头 CHROME_VP 那一段; 自检块逐项显示, 离线用例逐条断言) ----
export interface HeightBudget {
  needVp: number;           // 压缩之后仍然需要的高度
  availVp: number;          // 可用高度(= 实时窗口高)
  chromeVp: number;         // 其中: 顶栏 + 底部标签栏
  topInfoVp: number;        // 其中: 顶部"设备"卡(压缩后取实际值)
  heroBlockVp: number;      // 其中: 主按钮块(直径 + 下间隔)
  resultRowsVp: number;     // 其中: 首屏至少 2 行结果
  fits: boolean;            // needVp <= availVp
  heroCompressed: boolean;  // 主按钮被第 ① 步压过
  compactY: boolean;        // 间距 / 内边距被第 ② 步压过
  topInfoCompressed: boolean; // 顶部「设备」卡被第 ③ 步切成紧凑版(UI 必须照此渲染)
  scrollOnly: boolean;      // 三步压到底仍放不下 -> 结果区只能靠纵向滚动(内容一个都不隐藏)
  text: string;             // 一行结论(自检块 / 复制文本直接显示)
}

// ---- 纯几何事实(布局的唯一依据; 自检块照实显示, 谁都能拿尺子核对) ----
export interface GeometryFacts {
  widthVp: number;        // 实测窗口宽(0 = 未取到, 此时用的是兜底窗口, 见 degrade)
  heightVp: number;       // 实测窗口高
  usedWidthVp: number;    // 实际用于计算的宽(未取到时 = 兜底值 FALLBACK_W)
  usedHeightVp: number;   // 实际用于计算的高
  aspect: number;         // 宽高比(未取到 = 0)
  shortSideVp: number;    // 短边 = min(宽, 高)
  longSideVp: number;     // 长边 = max(宽, 高)
  contentVp: number;      // 内容列宽度 = min(阶梯限宽, 窗口宽)(0 = 不限)
  splitWanted: boolean;   // 几何上"够不够分栏"
  leftColVp: number;      // 分栏时左栏宽度(未分栏 = 0)
  rightColVp: number;     // 分栏时右栏宽度(未分栏 = 0); 结果卡的列数就是按它算的
  resultZoneVp: number;   // 结果区实际可用宽度(不分栏 = 内容列宽度)
  columnCap: number;      // 这个宽度下允许的最大列数
}

// ---- 布局计划(关于页「布局自检」逐项显示, 离线用例逐项核对) ----
//   geometry 里的每一个字段都由实时窗口宽高算出(或由它们派生的量算出);
//    id / index / name 三个字段是给人看的标签, 删掉它们不影响 geometry 里的任何一个数。
export interface LayoutPlan {
  id: string;             // S0 ~ S12(纯几何编号, 只用于显示)
  index: number;          // 需求表里的形态编号(1..9); 0 = 未取到窗口(只用于显示)
  name: string;           // 形态名(只用于显示)
  columns: number;        // 结果卡列数(按可用宽度算)
  split: boolean;         // 首页是否"左操作 / 右结果"左右分栏(按可用宽度与可读下限算)
  leftSpan: number;       // GridRow 12 栅格: 左栏 span
  rightSpan: number;      // GridRow 12 栅格: 右栏 span
  contentMaxVp: number;   // 内容列最大宽度(0 = 不限, 满宽)
  cardBasis: string;      // 结果卡 Flex 基准宽度
  minCardVp: number;      // 结果卡最小宽度(0 = 不设下限; 宽度不够时自动退成更少列, 永不溢出)
  heroDia: number;        // 开始按钮直径(vp, 已按当前窗口算好, 无需 UI 再算)
  btnH: number;           // 按钮最小高度(>= 40vp: 触摸目标下限)
  fontScale: number;      // 字号缩放(按窗口宽高算)
  gutter: number;         // 卡片间距(vp)
  pad: number;            // 卡片内边距(vp)
  radius: number;         // 卡片圆角(vp)
  basis: string;          // 判定依据(逐条说明"这个窗口凭什么排成这样")
  note: string;           // 这一档的布局说明
  degrade: string;        // 降级原因(空串 = 没有降级); 接口不可用 / 尺寸未取到 / 与折叠状态冲突都在这里
  height: HeightBudget;   // 竖向空间预算(高度不够时靠它兜底, 见 fitHeight)
  geometry: GeometryFacts; // 纯几何事实(给"一眼核对识别到什么"用)
  // 结果页(分数界面)的排版密度: 只给三处结果界面用(一键跑分结果页 / 单次跑分结果浮层 /
  // 首页分数卡), 与首页其它卡片的 gutter/pad 是两个独立的口径 —— 结果页要"一目了然",
  // 所以它比首页更紧。见 resultDecoFor。
  result: ResultDeco;
}

// ===================== 布局算术开始(LAYOUT-CORE-BEGIN) =====================
//  一、纯几何工具(布局的唯一依据)
//  这一段(到 LAYOUT-CORE-END 为止)只吃宽/高两个数, 不看设备、不看编号。
// ---------------------------------------------------------------------------

// 宽高比(高为 0 时按 0 处理, 不产生 NaN / Infinity 泄漏到 UI)
export function aspectOf(widthVp: number, heightVp: number): number {
  if (widthVp <= 0 || heightVp <= 0) {
    return 0;
  }
  return widthVp / heightVp;
}

function clampNum(v: number, lo: number, hi: number): number {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

function round3(v: number): number {
  return Math.round(v * 1000) / 1000;
}

// 实测窗口没取到时用的兜底窗口。它不是任何真机的尺寸, 只是一个"不至于排崩"的中性值;
// 用了它一定会被写进 degrade(见 degradeOf), 不静默。
const FALLBACK_W: number = 360;
const FALLBACK_H: number = 780;

// ---- 几何量(布局算术只吃这些数, 一个设备字段都不吃) ----
interface ZoneFacts {
  widthVp: number;      // 实测窗口宽(0 = 未取到)
  heightVp: number;     // 实测窗口高
  usedWidthVp: number;  // 实际参与计算的宽(未取到时 = 兜底值)
  usedHeightVp: number;
  aspect: number;
  shortSideVp: number;
  longSideVp: number;
  contentMaxVp: number;
  contentVp: number;
  resultZoneVp: number;
  columnCap: number;
  hasSize: boolean;     // 宽与高都 > 0
}

// 由宽高两个数算出这份几何量(唯一的入口; 其余几何函数都从它派生)
export function geometryFactsFor(widthVp: number, heightVp: number): ZoneFacts {
  const hasW: boolean = widthVp > 0;
  const hasH: boolean = heightVp > 0;
  const w: number = hasW ? widthVp : FALLBACK_W;
  const h: number = hasH ? heightVp : FALLBACK_H;
  const contentMax: number = contentMaxFor(w);
  const content: number = contentWidthFor(w);
  const zone: number = resultZoneFor(w);
  const out: ZoneFacts = {
    widthVp: hasW ? widthVp : 0,
    heightVp: hasH ? heightVp : 0,
    usedWidthVp: w,
    usedHeightVp: h,
    aspect: aspectOf(widthVp, heightVp),
    shortSideVp: Math.min(w, h),
    longSideVp: Math.max(w, h),
    contentMaxVp: contentMax,
    contentVp: content,
    resultZoneVp: zone,
    columnCap: columnCapFor(content),
    hasSize: hasW && hasH
  };
  return out;
}

// ===================== 布局算术到此结束(LAYOUT-CORE-END) =====================
//  下面(三 ~ 七)全部是文案层: 把"识别到了什么"讲给人听。它们只读上面这些数,
//  反过来不影响任何一个布局参数。离线用例 K 节会把这一点逐字段断言出来。
//  为什么显式标一行: 离线用例 M 节会按这两个标记切出"布局算术段"做静态检查 ——
//  那一段里不许出现形态编号、设备字段、写死的比例或机型。改结构时这行别删。
// ===========================================================================
//  三、编号(纯几何; 只用于显示与日志)
//
//   这里不做任何"比例 -> 应该是什么形态"的反向映射: 每一档就是一条"宽/高/宽高比落在哪个区间"
//    的区间判断, 识别到哪个区间就是哪个编号。1:sqrt(2) / 16:10 / 9:16 / 1:1 / 21:9 ……
//    没有任何一个比例被特殊对待(它们最多只是在 aspectHint 里被标注一下名字)。
//   设备型号 / 芯片 / 内置机型表完全不参与(它们连函数入参都不是)。
//   折叠状态也不参与: 它是运行时事实, 只影响名字与依据文案(见 nameOf/basisOf)。
//   这个函数只被 resolvePlan / resolvePlanPlain 用来填一个显示字段; planFor 一次都不调它。
// ---------------------------------------------------------------------------
export function shapeIdOf(inp: ShapeInput): string {
  const w: number = inp.widthVp;
  const h: number = inp.heightVp;
  if (w <= 0 || h <= 0) {
    return SHAPE_S0;
  }
  const ar: number = w / h;
  const shortSide: number = Math.min(w, h);
  // ① 极窄(先把"不横向溢出"放在第一位): 宽 < 320vp 或 宽高比 < 0.40
  if (w < W_TINY || ar < AR_ULTRA_NARROW) {
    return SHAPE_S9;
  }
  // ② 极扁(宽高比 >= 3.0) 或 矮(高 < 400vp 且宽高比 >= 2.4): 高度是稀缺资源
  if (ar >= AR_ULTRA_FLAT_EXTREME || (h < H_SHORT && ar >= AR_ULTRA_FLAT)) {
    return SHAPE_S10;
  }
  // ③ 扁宽(1.70 ~ 3.0)
  if (ar >= AR_FLAT) {
    return SHAPE_S8;
  }
  // ④ 横向(1.25 ~ 1.70): 窗口很宽(>= 1100vp)时单列一档, 否则与平板/横屏手机同档
  if (ar >= AR_WIDE) {
    return w >= W_ULTRA_WIDE ? SHAPE_S11 : SHAPE_S7;
  }
  // ⑤ 近方形(0.80 ~ 1.25): 按最小边再分大小
  if (ar >= AR_TALL) {
    return shortSide < MIN_SIDE_SQUARE ? SHAPE_S4 : SHAPE_S6;
  }
  // ⑥ 竖长(< 0.80): 宽 >= 600vp 才值得叫"竖长宽窗"; 窄窗再按"够不够阔"分两档
  if (w >= DIV_MIN_W) {
    return SHAPE_S5;
  }
  if (ar <= AR_OUTER_LIKE) {
    return SHAPE_S3;   // 窄窗里极窄的(0.40 ~ 0.50, 含两端): 名字只说"极窄竖长", 不提折叠
  }
  return ar < AR_TALL ? SHAPE_S1 : SHAPE_S2;
}

// 带抖动抑制的编号(仍然是纯函数: 上一次的编号是显式入参)。
//   它只影响显示的编号, 不影响任何布局参数 —— 参数由 planFor 直接吃窗口尺寸算。
//  窗口在阈值附近来回 1~2vp 时(PC 拖拽缩放、分屏吸附), 编号会左右横跳。这里做一件很小的事:
//  若新编号与上一次不同, 先把窗口尺寸往两侧各挪 HYST_VP 再判一次; 只要任一侧回到上一次的编号,
//  就保持上一次的编号。它不是"时间上的防抖"(不用定时器), 因此不延迟响应、不引入额外刷新。
export function resolveShapeId(inp: ShapeInput, prevId: string): string {
  const direct: string = shapeIdOf(inp);
  if (prevId.length === 0 || direct === prevId) {
    return direct;
  }
  const offsets: number[] = [-HYST_VP, HYST_VP];
  for (const dw of offsets) {
    for (const dh of offsets) {
      const alt: ShapeInput = {
        widthVp: Math.max(1, inp.widthVp + dw),
        heightVp: Math.max(1, inp.heightVp + dh),
        foldState: inp.foldState,
        foldApiOk: inp.foldApiOk,
        deviceType: inp.deviceType,
        tableFormFactor: inp.tableFormFactor
      };
      if (shapeIdOf(alt) === prevId) {
        return prevId;
      }
    }
  }
  return direct;
}

// ---------------------------------------------------------------------------
//  三、几何 -> 布局参数(每一行都写清"凭哪个数算的")
//   这些函数只吃 vp 宽高(或由它们派生的量), 连编号都不吃 ——
//    因此"把名字/编号全删掉, 布局结果不变"这件事在结构上就是成立的。
// ---------------------------------------------------------------------------

// 内容列最大宽度: 越宽的窗口留白越多, 但内容不至于宽到需要转头去读。
//   窄窗(未到第一步): 用"窗口宽 - 40"与 480 取小 —— 窄屏上多留一点边就排不下内容;
//   之后按阶梯: 680 / 820 / 1000 / 1240 / 1440vp。
export function contentMaxFor(widthVp: number): number {
  const w: number = widthVp > 0 ? widthVp : FALLBACK_W;
  if (w < CONTENT_LADDER_W[0]) {
    return Math.min(CONTENT_NARROW_MAX, Math.round(w - 40));
  }
  for (let i = 0; i < CONTENT_LADDER_W.length; i++) {
    if (w < CONTENT_LADDER_W[i]) {
      return CONTENT_LADDER_V[i];
    }
  }
  return CONTENT_LADDER_V[CONTENT_LADDER_V.length - 1];
}

// 内容列宽度 = min(限宽, 窗口宽); 0 表示"不限"(由调用方按满宽处理)
export function contentWidthFor(widthVp: number): number {
  const w: number = widthVp > 0 ? widthVp : FALLBACK_W;
  const cap: number = contentMaxFor(w);
  return cap > 0 ? Math.min(cap, w) : w;
}

// 这个宽度下允许的最大列数(按内容列宽度分档)
export function columnCapFor(contentVp: number): number {
  if (contentVp < COL_CAP_WIDE) {
    return 1;
  }
  if (contentVp < COL_CAP_XWIDE) {
    return 2;
  }
  return COL_CAP;
}

// 分栏结果(宽度算出来的, 不是"形态决定的")
//  leftSpan/rightSpan 就是界面 GridCol 用的 12 栅格值 —— 计划与实际渲染共用同一对数。
interface SplitInfo {
  wanted: boolean;    // 够不够分栏
  leftVp: number;     // 左栏宽度(不分栏 = 0)
  rightVp: number;    // 右栏宽度(不分栏 = 0)
  leftSpan: number;   // 12 栅格: 左栏(不分栏 = 12)
  rightSpan: number;  // 12 栅格: 右栏(不分栏 = 12)
}

// 几何上"够不够分栏": 宽度够 + 左栏(操作区)与右栏(结果区)都在可读下限之上。
//  为什么要加后两条: 一个 700x2400 的窄长窗口宽度够 600vp, 但分栏后两栏都又窄又长,
//  读起来反而更差 —— 那属于"这个窗口不适合分栏", 于是一律单列(这是布局决策, 只看几何)。
export function splitInfoFor(widthVp: number): SplitInfo {
  const w: number = widthVp > 0 ? widthVp : FALLBACK_W;
  const content: number = contentWidthFor(w);
  const widish: boolean = content >= DIV_SPAN_WIDE_AT;
  const spanL: number = widish ? DIV_SPAN_LEFT_WIDE : DIV_SPAN_LEFT;
  const spanR: number = widish ? DIV_SPAN_RIGHT_WIDE : DIV_SPAN_RIGHT;
  const left: number = Math.round(content * spanL / 12);
  const right: number = content - left;
  const wanted: boolean = w >= DIV_MIN_W && left >= MIN_LEFT_COL_VP && right >= MIN_RIGHT_COL_VP;
  let out: SplitInfo = { wanted: wanted, leftVp: wanted ? left : 0, rightVp: wanted ? right : 0,
    leftSpan: wanted ? spanL : 12, rightSpan: wanted ? spanR : 12 };
  return out;
}

// 结果区可用宽度(分栏 = 右栏宽度; 不分栏 = 内容列宽度)。结果卡的列数就是按它算的。
export function resultZoneFor(widthVp: number): number {
  const w: number = widthVp > 0 ? widthVp : FALLBACK_W;
  const content: number = contentWidthFor(w);
  const sp: SplitInfo = splitInfoFor(w);
  return sp.wanted ? sp.rightVp : content;
}

// (列数的最终计算在 planFor 里: 它必须在竖向预算压过间距之后再算一次,
//  这样"列数 x 最小宽 + 间距 <= 结果区宽度"这条不溢出判据用的才是最终生效的间距。
//  这里只保留它用到的两个原子量: 列数上限 columnCapFor 与结果区宽度 resultZoneFor。)

// 卡片间距 / 内边距 / 圆角: 由短边分档(空间越小越紧), 有下限
export function gutterFor(shortSideVp: number): number {
  const s: number = shortSideVp > 0 ? shortSideVp : FALLBACK_W;
  if (s < 420) {
    return 8;
  }
  if (s < 560) {
    return 10;
  }
  if (s < 760) {
    return 14;
  }
  if (s < 1100) {
    return 18;
  }
  return 20;
}

export function padFor(shortSideVp: number): number {
  const s: number = shortSideVp > 0 ? shortSideVp : FALLBACK_W;
  if (s < 420) {
    return 12;
  }
  if (s < 560) {
    return 14;
  }
  if (s < 760) {
    return 16;
  }
  if (s < 1100) {
    return 18;
  }
  return 20;
}

export function radiusFor(shortSideVp: number): number {
  const s: number = shortSideVp > 0 ? shortSideVp : FALLBACK_W;
  if (s < 420) {
    return 12;
  }
  if (s < 560) {
    return 14;
  }
  if (s < 900) {
    return 16;
  }
  return 18;
}

// 按钮最小高度: 触摸目标下限 40vp; 短边够大时给到 44~48vp(更好按)
export function btnHFor(shortSideVp: number): number {
  const s: number = shortSideVp > 0 ? shortSideVp : FALLBACK_W;
  return Math.round(clampNum(40 + (s - 420) * 0.012, 40, 48));
}

// 字号缩放: 观看距离随窗口变大而变远 -> 略放大; 小窗 -> 略缩小
export function fontScaleFor(usedWidthVp: number, usedHeightVp: number): number {
  const w: number = usedWidthVp > 0 ? usedWidthVp : FALLBACK_W;
  const h: number = usedHeightVp > 0 ? usedHeightVp : FALLBACK_H;
  const f: number = Math.min(w * 0.0016, h * 0.0011);
  return round3(clampNum(f, FONT_MIN, FONT_MAX));
}

// 开始按钮直径 = clamp(窗口宽^0.58 x 6.5, 72, 300)。
//  为什么用幂函数而不是分段系数: 这样它对任意窗口宽都单调、连续(不会在某个宽度上突然跳):
//    360vp -> 137vp(手机) · 665vp -> 200vp(目标机型展开态) · 1000vp -> 253vp · 1400vp -> 300vp(封顶)
//   下限 72vp 而不是 88vp: 在 280~320vp 的窄窗里, 88vp 的按钮会占掉窗口宽的 1/3, 反而更挤。
//    72vp 仍能放下缩小的"开始"两字(字号随窗口缩放, 见 fontScaleFor)。
export function heroDiaFor(widthVp: number): number {
  const w: number = widthVp > 0 ? widthVp : FALLBACK_W;
  const byW: number = Math.pow(w, 0.58) * 6.5;
  const capped: number = clampNum(byW, HERO_MIN_VP, HERO_MAX_VP);
  //  还有一条硬约束: 按钮不许比窗口还宽(窄窗上 280vp 的按钮会直接横向溢出)。
  //   所以再压一道"不超过窗口宽的 85%", 下限仍是 72vp —— 窄窗里字号已经跟着缩小,
  //   72vp 的按钮仍放得下"开始"两个字。
  const fitW: number = Math.max(HERO_MIN_VP, w * 0.85);
  return Math.round(Math.min(capped, fitW));
}

// 一条结果明细行的估算高度: 窗口越矮行越紧(下限 28vp, 否则 12~13fp 文字会挤)
export function rowVpFor(usedHeightVp: number): number {
  const h: number = usedHeightVp > 0 ? usedHeightVp : FALLBACK_H;
  return Math.round(clampNum(28 + (h - 500) * 0.06, ROW_MIN_VP, ROW_MAX_VP));
}

// 顶部「设备」卡是否需要切紧凑版: 窗口矮到连完整设备卡都放不下时, 收起的只是诊断细节。
//   这是布局决策(只看高度), 不是"识别成什么设备" —— 任何窗口矮到这个程度都会收。
function topInfoCompactFor(usedHeightVp: number): boolean {
  const h: number = usedHeightVp > 0 ? usedHeightVp : FALLBACK_H;
  return h < CHROME_VP + TOP_INFO_VP + HERO_MIN_VP + HERO_TAIL_VP + MIN_RESULT_ROWS * rowVpFor(h);
}

// ---------------------------------------------------------------------------
//  四、竖向空间预算
//
//  为什么需要它: 高度才是稀缺资源时必须单独算一笔账 —— 几百 vp 的高度里要塞下
//  "顶栏+标签栏 + 设备卡 + 主按钮 + 2 行结果 + 底部导航"。
//  四步压缩的顺序固定, 每步都可核对, 而且不隐藏任何内容:
//   ① 压主按钮直径(下限 HERO_MIN_VP)
//   ② 压卡片间距与内边距(各 -4vp, 有下限)
//   ③ 顶部「设备」卡切紧凑版(150 -> 70vp: 只留传播名 + 型号码 + 核心数)
//   ④ 压到底仍放不下 -> scrollOnly = true(结果区靠纵向滚动, 内容一个都不隐藏)
// ---------------------------------------------------------------------------
// 逐项相加必须精确等于 needVp —— 离线用例会照着这条恒等式复算, 不许有"账外"的魔法减免。
function heightNeedOf(heroDia: number, tailVp: number, rowVp: number): number {
  return CHROME_VP + TOP_INFO_VP + heroDia + tailVp + MIN_RESULT_ROWS * rowVp;
}

interface HeightFit {
  heroDia: number;
  gutter: number;
  pad: number;
  budget: HeightBudget;
}

function fitHeight(heightVp: number, heroIn: number, gutterIn: number, padIn: number,
                   rowVpIn: number, topCompact: boolean): HeightFit {
  const avail: number = heightVp > 0 ? Math.round(heightVp) : FALLBACK_H;
  let hero: number = heroIn;
  let gutter: number = gutterIn;
  let pad: number = padIn;
  let compactY: boolean = false;
  let tail: number = HERO_TAIL_VP;
  let rowVp: number = rowVpIn;
  let need: number = heightNeedOf(hero, tail, rowVp);

  // ① 压主按钮
  if (need > avail && hero > HERO_MIN_VP) {
    const next: number = Math.max(HERO_MIN_VP, Math.round(hero - (need - avail)));
    if (next < hero) {
      hero = next;
      need = heightNeedOf(hero, tail, rowVp);
    }
  }
  // ② 压间距 / 内边距 / 行高(账目: 间隔 16->12, 每条结果行 -4, 有下限)
  if (need > avail) {
    const g2: number = Math.max(6, gutter - 4);
    const p2: number = Math.max(10, pad - 4);
    const r2: number = Math.max(ROW_MIN_VP, rowVp - ROW_COMPACT_DELTA);
    if (g2 < gutter || p2 < pad || r2 < rowVp) {
      compactY = true;
      gutter = g2;
      pad = p2;
      tail = TAIL_COMPACT_VP;
      rowVp = r2;
      need = heightNeedOf(hero, tail, rowVp);
    }
  }
  // ③ 顶部「设备」卡切紧凑版(只留传播名 + 型号码 + 核心数, 诊断用的几行收起来)
  let topInfo: number = TOP_INFO_VP;
  if (need > avail && topCompact) {
    topInfo = TOP_INFO_COMPACT_VP;
    need = need - (TOP_INFO_VP - TOP_INFO_COMPACT_VP);
  }

  const topInfoCompressed: boolean = topInfo < TOP_INFO_VP;
  const heroCompressed: boolean = hero < heroIn;
  const fits: boolean = need <= avail;
  const rowsVp: number = MIN_RESULT_ROWS * rowVp;
  let text: string = '';
  if (fits && !heroCompressed && !compactY && !topInfoCompressed) {
    text = '首屏放得下：顶栏+标签栏 ' + CHROME_VP.toString() + ' + 设备卡 ' +
      topInfo.toString() + ' + 主按钮 ' + hero.toString() + ' + ' +
      MIN_RESULT_ROWS.toString() + ' 行结果 ' + rowsVp.toString() + ' = ' +
      need.toString() + ' <= 窗口高 ' + avail.toString() + 'vp';
  } else if (fits) {
    let how: string = '';
    if (heroCompressed) {
      how += '主按钮 ' + heroIn.toString() + '→' + hero.toString() + 'vp';
    }
    if (compactY) {
      how += (how.length > 0 ? '，' : '') + '间距/内边距/行高收紧(间隔 16→' + tail.toString() +
        '，行高 ' + rowVpIn.toString() + '→' + rowVp.toString() + ')';
    }
    if (topInfoCompressed) {
      how += (how.length > 0 ? '，' : '') + '设备卡切紧凑版(-80vp)';
    }
    text = '首屏放得下（已压缩：' + how + '）：需要 ' + need.toString() +
      ' <= 窗口高 ' + avail.toString() + 'vp';
  } else {
    text = '首屏放不下：四步压到底仍需 ' + need.toString() + 'vp > 窗口高 ' +
      avail.toString() + 'vp → 结果区只能靠纵向滚动查看（内容一个都不隐藏）';
  }

  const budget: HeightBudget = {
    needVp: need,
    availVp: avail,
    chromeVp: CHROME_VP,
    topInfoVp: topInfo,
    heroBlockVp: hero + tail,
    resultRowsVp: rowsVp,
    fits: fits,
    heroCompressed: heroCompressed,
    compactY: compactY,
    topInfoCompressed: topInfoCompressed,
    scrollOnly: !fits,
    text: text
  };
  const out: HeightFit = { heroDia: hero, gutter: gutter, pad: pad, budget: budget };
  return out;
}

// ---------------------------------------------------------------------------
//  四·B、结果页(分数界面)排版密度 —— "三个大数字 + 各小节关键分一屏一眼可见"
//
//  用户反馈(2026-10): "在显示分数的界面间隔太远了, 必须要一目了然。"
//  这里把结果页的块间距 / 内边距 / 行内边距 / 各档字号 / 是否左右并排 / 列数
//  一次性算出来, 三处结果界面(一键跑分结果页 / 单次跑分结果浮层 / 首页分数卡)共用同一份。
//
//   仍然是纯几何: 只吃实时窗口宽高。不看机型、不看编号、不看折叠状态
//    (所以名字全删掉, 这里的每一个数也逐位不变 —— 与 K 节同一条性质)。
//   只影响结果界面的观感: 不参与任何测试项的计算 / 顺序 / 计分, 也不改首页与
//    其它页面的布局参数(那些由上面 fitHeight / gutterFor 等决定, 一行没动)。
//
//  "一眼可见"的口径(自检块照实显示, 谁都能复核):
//      firstScreenVp = 结果页上下内边距 + 总分块 + 间距 + 三个大数字块 + 间距
//                      + 关键得分块(标题 + KEY_ROWS 行 + 一行脚注)
//      side(左右并排)时两项取较大者。firstScreenVp <= 可用高度 才算"一眼可见";
//      放不下时 fits = false 并写明"仍需滚动", 不假装放得下。
// ---------------------------------------------------------------------------
const KEY_ROWS: number = 10;        // L2「关键得分」表的固定行数(见 ResultLayers.buildKeyRows)
const L1_CARDS: number = 3;         // 第一层固定三张(单核 / 多核 / GPU)
const RES_MIN_HUGE: number = 26;    // 三个大数字的字号下限(再小就不叫"大数字"了)
const RES_MAX_HUGE: number = 64;
const RES_MIN_GIANT: number = 34;   // 「总分」字号下限 / 上限
const RES_MAX_GIANT: number = 76;
const RES_SIDE_MIN: number = 820;   // 内容列宽 >= 它才做"左总分+大数字 / 右关键得分"左右并排
const RES_WIDE_AT: number = 1100;   // 内容列宽 >= 它左右各让一半(6:6), 两边都更宽
const RES_KEYCOL_AT: number = 870;  // 关键得分所在栏 >= 它才把 10 行排成 2 列。
                                    // 为什么卡得这么严: 一行必须放得下"项名(最长 'GPU-SNL ·
                                    // Nomad Light' 约 215vp) + 数字与单位列(约 150vp)"。两列
                                    // 意味着单行宽度减半 —— 不够宽就排两列, 只会把项名截成
                                    // 省略号, 那是用可读性换列数, 不划算。内容列本身有
                                    // 1440vp 的限宽阶梯, 所以实际上只有超宽窗口才会两列。
const RES_INLINE_AT: number = 520;  // 一行可用宽 >= 它才考虑把"取得条件"并到同一行(否则会被截断)
const RES_NAME_MIN: number = 240;   // 关键得分一行的项名列至少要留这么宽(最长项名约 215vp)
const RES_COND_MIN: number = 140;   // 并排时取得条件列的最小 / 最大宽度(vp)
const RES_COND_MAX: number = 360;
const RES_INSET_VP: number = 44;    // 状态栏 + 导航栏的保守预留(实测 insets 由 window.getAvoidArea 给)
const DIGIT_EM: number = 0.58;      // 粗体数字的宽度经验系数(em)
const L1_DIGITS: number = 3;        // 三个大数字按3 位数定字号; 万一出现 4 位数,
                                    //   界面侧用 minFontSize + maxLines(1) 自适应缩小兜底
                                    //   (不靠"猜一个更大的余量把好端端的 3 位数压小")
const L1_INNER_GAP: number = 8;     // 三个大数字卡之间的最小间隙(也用于按宽度算"一行放几张")
const RES_TITLE_VP: number = 16;    // 关键得分块标题行高
const RES_FOOT_VP: number = 17;     // 关键得分块脚注行高(口径 / 来源, 合成一行)
// 「总分块」的分项(分块账本里逐项印出来, 界面上一一对应):
const TOTAL_LABEL_VP: number = 17;  // "总分" 标签行
const TOTAL_NUM_LH: number = 1.15;  // 总分数字的行高系数(字号 = giant)
const TOTAL_SUB_VP: number = 34;    // "相对参考基线 x 倍" 标签(含自身上下内边距)
const TOTAL_DEV_VP: number = 15;    // "设备 · 时间" 一行

// ---- 第一层「三个大数字」的固定高度模型(2026-10 修真机缺陷) ----
//  用户真机反馈(7.3, 665x940vp 竖长窗口): 结果页第一层的三张卡(单核 2723 / 多核 10489 /
//  GPU 1123)被纵向拉伸到几乎占满整屏, 数字缩在巨大的空块中间, 卡与卡之间隔着几百 vp,
//  底下的关键得分列表被挤到最下面一条。根因是这里同时有两处"会沿纵向撑开"的写法:
//    ① 卡片根 Column 上挂着 layoutWeight(1)。它的父节点是 Column —— 主轴是竖直的,
//       所以 ArkUI 线性布局把这张卡的主轴尺寸按"父容器理想主轴尺寸 - 已分配"来给(见
//       linear_layout_utils.cpp: Measure 里 remainSize = idealSize - allocatedSize,
//       再 SetIdealMainSize(childConstraint, remainSize x weight/totalWeight)) ——
//       卡的高度不再由内容决定, 而是"父容器给多少就吃多少"; 窗口越高, 空块越大。
//    ② 三张卡的 flex 基准宽取自首页结果卡的列数(plan.columns: 竖屏手机上 = 1 → 基准宽
//       100% + 最小宽 0), 三张卡于是各占一行 —— 行数一多, 上面那个 layoutWeight 就有了
//       成倍的纵向空间可以吃。
//  修法: 这一层的尺寸全部在这里算死, 界面直接照抄(ResultDeco 的 l1Cards / l1Lines /
//  l1CardH / l1Basis / l1MinW / l1CondLines):
//    · 每张卡是固定高度块(高度按标签 / 大数字 / 条件的字号算出来), 不参与"剩余高度"分配;
//    · 一行放几张由可用宽度算, 放不下才换行, 换行只增加"行数 x 固定卡高", 不会被拉伸;
//    · 多余高度只给结果列表 / 滚动区(见 ResultBlock.grow), 不给数字卡。
//   估算与渲染同源: 界面里这一层每张卡都写 .height(l1CardH), 所以 firstScreenVp 里
//    l1BlockH 那一段是精确值(= 上下内边距 + 行数 x 卡高), 不再是"按内容猜一个"。
//    (这正是上一轮 firstScreenVp 显示通过、真机却完全不是那么回事的原因: 估算按"三张卡一行"
//     算, 界面却按"每张卡一行 + layoutWeight 撑开"渲染 —— 两套模型对不上, 数字再准也没意义。)
const L1_CARD_MIN_W: number = 104;   // 一张大数字卡的最小可读宽度(界面同值: Index.bigScoreCard)
const L1_CARD_MIN_H: number = 24;    // 卡高的绝对下限(再小就不成卡了; 只有极端窗口会触到)
const L1_CARD_PAD_V: number = 6;     // 卡内上下内边距(左右另有 3vp, 不参与高度)
const L1_LABEL_LH: number = 1.3;     // 标签行高系数(字号 = keyName)
const L1_NUM_LH: number = 1.2;       // 大数字行高系数(字号 = huge)
const L1_COND_LH: number = 1.25;     // "取得条件"行高系数(与界面 Text.lineHeight 同值)
const L1_COND_TOP: number = 2;       // 条件行与数字行之间的间隔(与界面 margin.top 同值)
const L1_SHARE_MAX: number = 0.30;   // 不变量: 三个大数字块总高 <= 可用高度的 30%
                                     //   (离线用例 ref/layout_cases.test.mjs 的 N 节逐窗口断言)

// 一张大数字卡的内容高度: 标签 + 大数字 + 条件行(1~2 行) + 卡内上下内边距。
//  它是纯函数, 也是"卡为什么这么高"的唯一口径 —— 自检页会把这一串逐项印出来。
function l1CardHeightOf(keyName: number, huge: number, keyUnit: number, condLines: number): number {
  return Math.round(keyName * L1_LABEL_LH) + Math.round(huge * L1_NUM_LH) + L1_COND_TOP +
    Math.round(keyUnit * L1_COND_LH) * condLines + 2 * L1_CARD_PAD_V;
}

// ---- 结果页的"分块账本"(自检页逐块显示, 离线用例逐块断言) ----
//   为什么要分块: 上一轮只给了一个总数(firstScreenVp), 总数对不上时看不出是哪一块算错,
//    更看不出"多余的高度被谁吃掉了"。这里把结果页首屏拆成若干块, 每块都写清:
//      · 预测高度(vp)   · 高度是固定的还是会拉伸的   · 纵向拉伸权重   · 这一块凭什么这么高
//   grow > 0 的块 = "多余高度给了谁"。当前设计里只有结果列表 / 滚动区是拉伸块
//    (结果页整页在 Scroll 里, Scroll 用 layoutWeight(1) 吃掉剩余高度 —— 这是有意的,
//     而且它的父容器高度确定, 与上面那个"卡在无确定高度父容器里 layoutWeight"是两回事);
//    三个大数字块 / 总分块 / 关键得分块 / 内边距 全是固定高度块, grow 一律为 0。
export interface ResultBlock {
  key: string;      // 块标识(稳定英文键, 供离线用例核对)
  label: string;    // 中文名(自检页直接显示)
  vp: number;       // 预测高度(vp)
  fixed: boolean;   // true = 固定高度(按内容与字号算死, 不被拉伸); false = 承接剩余高度
  grow: number;     // 纵向拉伸权重(0 = 不参与剩余高度分配)
  col: string;      // 这一块在哪一栏:'单列' / '左栏' / '右栏'(并排时)
  detail: string;   // 这一块的高度是怎么算出来的(逐项写出, 便于一眼核对)
}

export interface ResultDeco {
  giant: number;        // 「总分」字号(fp)
  huge: number;         // 单核 / 多核 / GPU 三个大数字的字号(fp)
  keyNum: number;       // 关键得分行的数字字号(fp)
  keyName: number;      // 关键得分行的项名字号(fp)
  keyUnit: number;      // 单位 / 取得条件的字号(fp)
  gap: number;          // 结果页各块之间的垂直间距(vp)
  pad: number;          // 结果页卡片内边距(vp)
  rowPy: number;        // 关键得分行的上下内边距(vp)
  l1CondLines: number;  // 三个大数字下面那行"取得条件"最多显示几行(窄屏 1 行, 宽屏 2 行)
  metricW: number;      // 关键得分一行里"数字 + 单位"这一列的固定宽度(vp):
                        //   同类数字因此落在同一列并右对齐(而不是跟着项名长度左右乱跑)
  detailMetricW: number;// 明细行(GB7 单项 / 自研套件项 / GPU 场景)里"数字 + 单位"这一列的固定宽度(vp)。
                        //   为什么不能直接用 metricW: metricW 是按关键得分行的字号与短单位
                        //   (分 / GOPs/s / MiB/s)算出来的, 而明细行的字号更大(fz(11) -> 约 12fp)、
                        //   单位也更长(最长 Mbody-steps/s = 13 字符)。真机上 88vp 那一列会把
                        //   Game Physics 截成「0.9 Mbody-ste…」—— 7.5 之前一直如此。
  condW: number;        // "取得条件"并到同一行时的固定列宽(vp); 0 = 条件不在同一行
  inlineCond: boolean;  // 取得条件是否与"数字 + 单位"并到同一行(true = 不另起一行)
  side: boolean;        // 首屏左右并排(左: 总分 + 三大数字 / 右: 关键得分)
  leftSpan: number;     // 并排时左栏的 12 栅格 span(不并排 = 12)
  rightSpan: number;    // 并排时右栏的 12 栅格 span
  keyCols: number;      // 关键得分的列数(1 或 2)
  detailCols: number;   // 逐项明细区的列数(1 / 2 / 3): 大屏并排更多列, 而不是把小屏排法放大
  firstScreenVp: number; // 首屏"总分 + 三大数字 + 关键得分"的估算高度(vp)
  availVp: number;      // 可用高度(vp) = 实时窗口高 - RES_INSET_VP 的保守预留
  fits: boolean;        // firstScreenVp <= availVp
  // ---- 第一层「三个大数字」的固定尺寸(界面必须照抄这一份, 不许自己再算) ----
  l1Cards: number;      // 一行放几张(1 ~ 3): 由可用宽度与 L1_CARD_MIN_W 算出
  l1Lines: number;      // 三张卡总共占几行 = ceil(3 / l1Cards)
  l1CardH: number;      // 每张大数字卡的固定高度(vp) 界面写 .height(l1CardH), 不再拉伸
  l1BlockH: number;     // 三个大数字块的总高 = 上下内边距 + 行数 x 卡高(精确值, 见文件里那段说明)
  l1Basis: string;      // 大数字卡的 flex 基准宽('31%' / '48%' / '100%', 与 l1Cards 一一对应)
  l1MinW: number;       // 大数字卡的最小宽(vp; 单列时 0 = 不设下限)
  //  (条件行数 l1CondLines 见上面第 778 行: 它本来就是结果页密度的一部分,
  //   现在还会被"三个大数字块 <= 可用高度 30%"这条预算复核一遍 —— 放不下两行就退回一行。)
  l1Share: number;      // l1BlockH / availVp(不变量: <= L1_SHARE_MAX = 0.30)
  l1InvariantOk: boolean; // 不变量 l1BlockH <= 0.30 x availVp(数字卡不许吃满屏幕)
  // ---- 分块账本 + 结论 ----
  blocks: ResultBlock[];   // 首屏分块(自检页逐块显示; 离线用例逐块核对)
  blocksText: string;      // 分块账本的纯文本(自检页 / 复制文本共用)
  growBlockKey: string;    // 多余高度给了谁(当前 = 'scroll' 结果列表 / 滚动区)
  slackVp: number;         // 多余高度(vp) = availVp - firstScreenVp(负值 = 还要滚动)
  text: string;         // 一行结论(自检块直接显示)
}

// 结果页密度: 唯一的入口(全部由窗口宽高算出)
export function resultDecoFor(widthVp: number, heightVp: number): ResultDeco {
  const w: number = widthVp > 0 ? widthVp : FALLBACK_W;
  const h: number = heightVp > 0 ? heightVp : FALLBACK_H;
  const content: number = contentWidthFor(w);

  // ① 间距 / 内边距 / 行内边距: 由可用高度分档 —— 高度是结果页最稀缺的资源。
  //    四档从紧到松, 每一档都可核对; 窄内容列再各收 2vp(竖屏手机上 4~6vp 就够分区了)。
  let gap: number = 10;
  let pad: number = 15;
  let rowPy: number = 5;
  if (h < 600) {
    gap = 4;
    pad = 8;
    rowPy = 2;
  } else if (h < 760) {
    gap = 5;
    pad = 10;
    rowPy = 3;
  } else if (h < 900) {
    gap = 7;
    pad = 12;
    rowPy = 4;
  }
  if (content < RES_SIDE_MIN) {
    gap = Math.max(4, gap - 2);
    pad = Math.max(8, pad - 2);
  }

  // ② 左右并排: 内容列够宽才并排(宽到一屏两栏都有可读宽度)。
  //    不并排时"左栏宽度"就是内容列去掉左右内边距之后的宽度。
  const side: boolean = content >= RES_SIDE_MIN;
  const leftSpan: number = content >= RES_WIDE_AT ? 6 : 5;
  const rightSpan: number = 12 - leftSpan;
  const leftW: number = side ? Math.round(content * leftSpan / 12) : Math.max(120, content - 2 * pad);
  const rightW: number = side ? (content - leftW) : content;
  const keyCols: number = (side && rightW >= RES_KEYCOL_AT) ? 2 : 1;
  const rowW: number = keyCols === 2 ? Math.floor((rightW - 2 * pad) / 2) : (rightW - 2 * pad);

  // ③ 第一层「三个大数字」的可用宽度与一行放几张(界面直接照抄这三个数)。
  //     这里不再借用首页"结果卡"的列数: 结果页第一层是在左栏 / 内容列里排三张卡,
  //      与首页结果区的列数是两码事(上一轮把两者混用, 竖屏手机上算出"基准宽 100%",
  //      三张卡各占一行 —— 正是真机那张"烂布局"的一半原因)。
  const l1Inner: number = side ? leftW : Math.max(160, content - 2 * pad);
  //    一行放几张: 每张至少 L1_CARD_MIN_W, 卡之间留 L1_INNER_GAP 的间隙, 取 1~3。
  //    宽度不够就换行(行数 = ceil(3/每行张数)), 换行只增加"固定卡高 x 行数", 不会被拉伸。
  const l1Cards: number = Math.max(1, Math.min(L1_CARDS,
    Math.floor((l1Inner + L1_INNER_GAP) / (L1_CARD_MIN_W + L1_INNER_GAP))));
  const l1Lines: number = Math.ceil(L1_CARDS / l1Cards);
  //    卡的 flex 基准宽 = 这张卡占可用宽度的百分比(扣掉卡间间隙), 与界面 .flexBasis 同值;
  //    flexGrow(1) 再把剩余的几个百分点摊掉 —— 所以"一行放几张"在渲染时是可预测的。
  const l1Basis: string = l1Cards <= 1
    ? '100%'
    : ((Math.floor(100 * (l1Inner - (l1Cards - 1) * L1_INNER_GAP) / (l1Cards * l1Inner))).toString() + '%');
  const l1MinW: number = l1Cards >= 2 ? L1_CARD_MIN_W : 0;
  //    单卡宽度: 按"每行 l1Cards 张 + 两侧各留一个间隙"保守估(字体只许偏小, 不许偏大)
  const l1W: number = Math.floor((l1Inner - (l1Cards + 1) * L1_INNER_GAP) / l1Cards);

  // ④ 字号: 三个大数字既要"大", 又不能在自己的卡里放不下。所以取"高度的 7.5%"与
  //    "单卡宽 / (L1_DIGITS 位 x 数字宽系数)"两者的较小者, 再夹上下限 —— 按 3 位数定字号,
  //    万一是 4 位数, 由界面侧的 minFontSize + maxLines(1) 自动缩小兜底(见 bigScoreCard)。
  //    ⑤(下一节)还会用"三个大数字块 <= 可用高度 30%"这条不变量再压一次。
  const byHeight: number = h * 0.075;
  const byWidth: number = l1W / (L1_DIGITS * DIGIT_EM);
  //  最后统一乘上"观看距离缩放"(fontScaleFor, 0.92~1.08; 平板 / 电脑略放大, 极窄略缩小),
  //  这样界面拿到就是最终字号, 不需要再乘一次(乘两次会把大屏放大 16%, 也会让
  //  下面那份首屏高度估算对不上)。字号单位是 fp —— 它本身还会跟随系统字体大小设置。
  const fs: number = fontScaleFor(w, h);
  const hugeWant: number = Math.round(clampNum(Math.min(byHeight, byWidth), RES_MIN_HUGE, RES_MAX_HUGE) * fs);
  let huge: number = hugeWant;
  const keyNum: number = Math.round(clampNum(Math.min(h * 0.024, content * 0.05), 15, 26) * fs);
  const keyName: number = Math.round(clampNum(keyNum / fs * 0.70, 11, 17) * fs);
  const keyUnit: number = Math.round(clampNum(keyNum / fs * 0.58, 9, 14) * fs);
  //  一行里"数字 + 单位"整组的固定列宽: 两位到三位数字 + 一个短单位(分 / GOPs/s / MiB/s)。
  //  做成固定列是为了对齐: 项名长短不一, 没有固定列的话每个数字的右边缘都会不一样。
  const metricW: number = Math.round(clampNum(keyNum * 3.2 + keyUnit * 7.0, 84, 260));
  //  明细行的"吞吐 + 单位"列宽。必须与关键得分行的字号解耦 —— 第一版写成
  //  clamp(max(metricW,120),84,260) 是错的: metricW 随 keyNum 涨(大屏三列时到 195vp),
  //  而明细行的 metric 文本只有 fz(11)(约 10~12fp), 最长真实串只需 98~118vp。多出来的
  //  75vp 全部从同一行的项名列里扣, 实测在大屏三列上项名列被压成负数(内容溢出卡片)。
  //  口径: 4 位有效数字 + 一个空格 + 最长单位(Mbody-steps/s, 13 字符)= 9.82 em(见 NumberFmt.ets),
  //  留一点余量取 10.4 em; 字号用与界面同一个缩放系数; 上限 130 保证任何窗口下
  //  这一列都不会反过来挤压项名(下限仍是既有列宽的下限 84)。
  const detailFont: number = Math.round(clampNum(11 * fs, 9, 14));
  const detailMetricW: number = Math.round(clampNum(10.4 * detailFont, 84, 130));
  //  取得条件并到同一行必须两头都够: 行本身够宽(>= RES_INLINE_AT), 而且把项名列与
  //  "数字 + 单位"列扣掉之后还剩下 RES_NAME_MIN 给项名、RES_COND_MIN 给条件 ——
  //  否则并排的代价就是把项名或条件截成省略号, 那还不如让它落到第二行完整显示。
  //  (宽屏上高度富余, 第二行不心疼; 窄屏上才需要靠并排来省高度。)
  const inlineCond: boolean = rowW >= RES_INLINE_AT &&
    (rowW - metricW) >= (RES_NAME_MIN + RES_COND_MIN);
  const condW: number = inlineCond
    ? Math.round(clampNum(rowW - metricW - RES_NAME_MIN, RES_COND_MIN, RES_COND_MAX)) : 0;

  // ⑤ 三个大数字块的固定高度 + "不许吃满屏幕"的不变量(本文件最上面那一段的落地)
  //  可用高度: 窗口高扣掉状态栏 / 导航栏的保守预留。
  const avail: number = Math.round(h - RES_INSET_VP);
  //  这一块的总高上限 = 可用高度 x L1_SHARE_MAX(30%)。上下内边距也算在里面 ——
  //  所以"每张卡的可用高度上限" = (上限 - 容器上下内边距) / 行数, 逐行摊。
  const l1BudgetH: number = Math.floor(avail * L1_SHARE_MAX);
  const l1CardCap: number = Math.floor((l1BudgetH - 2 * pad) / l1Lines);
  //  条件的行数先按宽度定(内容列 >= 480vp 才放得下两行), 再用高度预算复核: 放不下就退回一行,
  //  而不是把卡片拉高 —— 长度预算优先于"多显示一行条件"。
  let l1CondLines: number = content >= 480 ? 2 : 1;
  if (l1CondLines > 1 && l1CardHeightOf(keyName, huge, keyUnit, l1CondLines) > l1CardCap) {
    l1CondLines = 1;
  }
  //  还不够就把大数字那一行按预算压下来(只压大数字字号; 标签 / 单位 / 条件一个都不动 ——
  //  那三样是信息, 压了就看不清了)。下限是 RES_MIN_HUGE(26fp, 再小就不叫"大数字"了)。
  if (l1CardCap > 0 && l1CardHeightOf(keyName, huge, keyUnit, l1CondLines) > l1CardCap) {
    const others: number = Math.round(keyName * L1_LABEL_LH) + L1_COND_TOP +
      Math.round(keyUnit * L1_COND_LH) * l1CondLines + 2 * L1_CARD_PAD_V;
    const numLineMax: number = l1CardCap - others;
    const byBudget: number = Math.floor(numLineMax / L1_NUM_LH);
    huge = Math.max(RES_MIN_HUGE, Math.min(huge, byBudget));
  }
  //  卡高: 固定值(内容与字号算出), 再用预算上限兜一道 —— 于是"总高 <= 30% 可用高度"这条
  //    不变量在构造上成立(不是靠"希望它成立"): l1BlockH = 2 x pad + 行数 x l1CardH
  //    <= 2 x pad + 行数 x l1CardCap <= l1BudgetH。界面写的就是这个 l1CardH。
  const l1CardHContent: number = l1CardHeightOf(keyName, huge, keyUnit, l1CondLines);
  const l1CardH: number = l1CardCap > 0
    ? Math.max(L1_CARD_MIN_H, Math.min(l1CardHContent, l1CardCap))
    : Math.max(L1_CARD_MIN_H, l1CardHContent);
  const l1BlockH: number = pad * 2 + l1Lines * l1CardH;
  const l1Share: number = avail > 0 ? l1BlockH / avail : 0;
  const l1InvariantOk: boolean = l1BlockH <= l1BudgetH + 0.5;
  //  总分字号随大数字字号一起动(它本来就是 huge x 1.2 派生的), 这样两张"大数字"不会打架。
  const giant: number = Math.round(clampNum(huge * 1.2, RES_MIN_GIANT, RES_MAX_GIANT) * fs);

  // ⑥ 逐项明细的列数: 大屏真的并排更多列(而不是把小屏的排法整体放大)
  let detailCols: number = 1;
  if (content >= 1240) {
    detailCols = 3;
  } else if (content >= RES_SIDE_MIN) {
    detailCols = 2;
  }

  // ⑦ 首屏分块账本(逐块相加, 没有"账外"的魔法减免)
  //    上一轮这里只印一个总数(firstScreenVp): 总数对不上时看不出是哪一块算错, 更看不出
  //     "多余的高度被谁吃掉了"。现在每一块都写清: 预测 vp / 是否固定高度 / 纵向拉伸权重 /
  //     这一块凭什么这么高 —— 自检页逐块列出, 离线用例逐块核对(见 layout_cases.test.mjs N 节)。
  //    "多余高度给了谁"直接写进账本: 只有结果列表 / 滚动区(key = 'slack', grow = 1)会吃
  //     剩余高度; 总分块 / 三个大数字块 / 关键得分块 / 内边距 的 grow 一律是 0。
  //     也就是说: 窗口再高, 三个大数字块的高度也只由 l1CardH(固定)与行数决定 —— 不跟着变大。
  //    与界面一一对应(界面里每一块的实际高度就是这么写的):
  //       padTop    ← GridRow 的 margin.top
  //       total     ← "总分" 标签 + 总分数字 + 倍数标签 + 设备·时间行
  //       gap1/gap2 ← GridRow 的纵向 gutter(并排时是两栏之间的横向 gutter)
  //       l1        ← 三个大数字容器: 上下内边距 + 行数 x 每张固定卡高
  //       l2        ← 关键得分卡: 上下内边距 + 标题 + 行 x 行高 + 脚注
  //       padBottom ← 内容与底部(导航条)之间的留白
  const padTop: number = pad;
  const padBottom: number = pad;
  const totalH: number = TOTAL_LABEL_VP + Math.round(giant * TOTAL_NUM_LH) +
    TOTAL_SUB_VP + TOTAL_DEV_VP;
  const rowH: number = inlineCond
    ? (Math.round(keyNum * 1.2) + rowPy * 2 + 1)
    : (Math.round(keyNum * 1.2) + Math.round(keyUnit * 1.25) + rowPy * 2 + 2);
  const rowsPerCol: number = Math.ceil(KEY_ROWS / keyCols);
  const l2BlockH: number = pad * 2 + RES_TITLE_VP + rowsPerCol * rowH + RES_FOOT_VP;
  const stackSingle: number = padTop + totalH + gap + l1BlockH + gap + l2BlockH + padBottom;
  const stackLeft: number = padTop + totalH + gap + l1BlockH;
  const stackRight: number = padTop + l2BlockH;
  const firstScreen: number = side ? (Math.max(stackLeft, stackRight) + padBottom) : stackSingle;
  const fits: boolean = firstScreen <= avail;
  const slack: number = avail - firstScreen;

  const colA: string = side ? '左栏' : '单列';
  const colB: string = side ? '右栏' : '单列';
  const both: string = side ? '两栏' : '单列';
  const tallCol: string = side ? (stackLeft >= stackRight ? '左栏' : '右栏') : '单列';
  const l1Detail: string = '上下内边距 ' + (pad * 2).toString() + 'vp + ' + l1Lines.toString() +
    ' 行 x 每张 ' + l1CardH.toString() + 'vp（固定高度：标签 ' + Math.round(keyName * L1_LABEL_LH).toString() +
    ' + 大数字 ' + Math.round(huge * L1_NUM_LH).toString() + ' + 间隔 ' + L1_COND_TOP.toString() +
    ' + 条件 ' + l1CondLines.toString() + ' 行 x ' + Math.round(keyUnit * L1_COND_LH).toString() +
    ' + 卡内 ' + (2 * L1_CARD_PAD_V).toString() + '）· 一行 ' + l1Cards.toString() +
    ' 张' + (l1Lines > 1 ? ('（宽度不够，换成 ' + l1Lines.toString() + ' 行）') : '');
  const blocks: ResultBlock[] = [];
  blocks.push({ key: 'padTop', label: '结果区上内边距', vp: padTop, fixed: true, grow: 0, col: both,
    detail: '内容不贴着状态栏（窗口越小这一档越紧）' });
  blocks.push({ key: 'total', label: '总分块', vp: totalH, fixed: true, grow: 0, col: colA,
    detail: '"总分" 标签 ' + TOTAL_LABEL_VP.toString() + ' + 总分数字 ' +
      Math.round(giant * TOTAL_NUM_LH).toString() + '（' + giant.toString() + 'fp x ' +
      TOTAL_NUM_LH.toString() + '）+ 倍数标签 ' + TOTAL_SUB_VP.toString() + ' + 设备·时间行 ' +
      TOTAL_DEV_VP.toString() });
  blocks.push({ key: 'gap1', label: '块间距', vp: gap, fixed: true, grow: 0, col: colA,
    detail: '总分块与三个大数字块之间（结果页唯一的块间距来源）' });
  blocks.push({ key: 'l1', label: '三个大数字块', vp: l1BlockH, fixed: true, grow: 0, col: colA,
    detail: l1Detail });
  if (!side) {
    blocks.push({ key: 'gap2', label: '块间距', vp: gap, fixed: true, grow: 0, col: both,
      detail: '三个大数字块与关键得分块之间' });
  }
  blocks.push({ key: 'l2', label: '关键得分块', vp: l2BlockH, fixed: true, grow: 0, col: colB,
    detail: '上下内边距 ' + (pad * 2).toString() + 'vp + 标题 ' + RES_TITLE_VP.toString() +
      'vp + ' + rowsPerCol.toString() + ' 行 x ' + rowH.toString() + 'vp + 脚注 ' +
      RES_FOOT_VP.toString() + 'vp（' + keyCols.toString() + ' 列）' });
  blocks.push({ key: 'padBottom', label: '结果区下内边距', vp: padBottom, fixed: true, grow: 0, col: both,
    detail: '内容与底部导航条之间的留白' });
  blocks.push({ key: 'slack', label: '多余高度 → 结果列表 / 滚动区', vp: slack > 0 ? slack : 0,
    fixed: false, grow: 1, col: both,
    detail: slack > 0
      ? ('可用 ' + avail.toString() + 'vp - 首屏 ' + firstScreen.toString() + 'vp = ' +
        slack.toString() + 'vp：全部留给列表 / 滚动区（数字卡一点都不要）')
      : ('首屏 ' + firstScreen.toString() + 'vp 已超过可用 ' + avail.toString() + 'vp 共 ' +
        Math.abs(slack).toString() + 'vp：列表要滚动才能看全（内容一个都不隐藏）') });
  let blocksText: string = '◆ 结果页首屏分块（预测，单位 vp｜' +
    (side ? ('左右并排：' + tallCol + '较高') : '单列纵向') + '）\n';
  for (let i = 0; i < blocks.length; i++) {
    const b: ResultBlock = blocks[i];
    blocksText += '　· ' + b.label + '（' + b.col + '）：' + b.vp.toString() + 'vp · ' +
      (b.fixed ? '固定高度' : ('拉伸块 grow=' + b.grow.toString())) + ' —— ' + b.detail + '\n';
  }
  blocksText += '　· 合计：首屏 ' + firstScreen.toString() + 'vp（' +
    (side ? (tallCol + ' + 下内边距') : '全部块相加') + '）' +
    (fits ? ' <= ' : ' > ') + '可用 ' + avail.toString() + 'vp';

  let sideSay: string = side
    ? ('左右并排（左 ' + leftSpan.toString() + ' : 右 ' + rightSpan.toString() +
      '，左 ' + Math.round(leftW).toString() + 'vp / 右 ' + Math.round(rightW).toString() + 'vp）')
    : '单列纵向';
  let text: string = '结果页密度：块间距 ' + gap.toString() + 'vp · 内边距 ' + pad.toString() +
    'vp · 行内边距 ' + rowPy.toString() + 'vp · ' + sideSay +
    '；总分 ' + giant.toString() + 'fp / 三个大数字 ' + huge.toString() + 'fp / 关键分数字 ' +
    keyNum.toString() + 'fp（项名 ' + keyName.toString() + 'fp，单位与条件 ' + keyUnit.toString() + 'fp）' +
    '；三个大数字块 一行 ' + l1Cards.toString() + ' 张 x ' + l1Lines.toString() +
    ' 行 · 每张固定 ' + l1CardH.toString() + 'vp（不拉伸）· 整块 ' + l1BlockH.toString() +
    'vp · 占可用高度 ' + (l1Share * 100).toFixed(1) + '%（不变量 <= ' +
    (L1_SHARE_MAX * 100).toFixed(0) + '%：' + (l1InvariantOk ? '满足' : '不满足') + '）· 基准宽 ' +
    l1Basis + ' · 最小宽 ' + l1MinW.toString() + 'vp · 条件行 ' + l1CondLines.toString() + ' 行' +
    '；关键得分 ' + keyCols.toString() + ' 列 x ' + rowsPerCol.toString() + ' 行 · 单行可用 ' +
    Math.round(rowW).toString() + 'vp → 取得条件' + (inlineCond ? '紧跟单位之后（不另起一行）' : '另起一行（这一行放不下会截断，故不并排）') +
    '；明细 ' + detailCols.toString() + ' 列' +
    '；首屏 ' + firstScreen.toString() + 'vp ' +
    (fits ? '<= 可用 ' : '> 可用 ') + avail.toString() + 'vp（窗口高 ' + Math.round(h).toString() +
    'vp 减去状态栏/导航栏预留 ' + RES_INSET_VP.toString() + 'vp）' +
    (fits ? ' → 一眼可见，不需要滚动' : ' → 仍需滚动查看末尾几行') +
    '；多余高度 ' + (slack > 0 ? slack.toString() : '0') + 'vp 全部给结果列表 / 滚动区';

  const out: ResultDeco = {
    giant: giant, huge: huge, keyNum: keyNum, keyName: keyName, keyUnit: keyUnit,
    gap: gap, pad: pad, rowPy: rowPy, l1CondLines: l1CondLines,
    metricW: metricW, detailMetricW: detailMetricW, condW: condW, inlineCond: inlineCond,
    side: side, leftSpan: leftSpan, rightSpan: rightSpan,
    keyCols: keyCols, detailCols: detailCols,
    l1Cards: l1Cards, l1Lines: l1Lines, l1CardH: l1CardH, l1BlockH: l1BlockH,
    l1Basis: l1Basis, l1MinW: l1MinW,
    l1Share: l1Share, l1InvariantOk: l1InvariantOk,
    blocks: blocks, blocksText: blocksText, growBlockKey: 'slack', slackVp: slack,
    firstScreenVp: firstScreen, availVp: avail, fits: fits, text: text
  };
  return out;
}

// ---------------------------------------------------------------------------
//  五、命名 / 依据 / 降级(纯文本层: 只用窗口尺寸 + 折叠状态的说辞)
//
//   这一整节都不参与布局: 它只把"识别到了什么"讲清楚。把这一节全删掉,
//    planFor 算出来的每一个布局参数都不变(K 节用例断言)。
// ---------------------------------------------------------------------------
// 已知比例标注: 宽高比落在哪个"有名有姓"的比例附近就补一句人话。
//   它只是标注(给读日志/看自检页的人一个参照), 不参与任何判定, 更不是"预期比例":
//    判定只看实测宽高比落在哪两条阈值之间(见 shapeIdOf), 与这些名字无关。
//    (放在文案层而不是布局层, 就是为了让 M 节那条静态检查能机械证明这一点。)
//  为什么值得有: 光写 "0.706" 读者还得自己心算 —— 而 1:sqrt(2) 与 16:10 在窗口尺寸上
//  只差 0.08, 恰恰是最容易被资料带偏的地方(本项目就出过这个错, 见文件头)。
export function aspectHint(widthVp: number, heightVp: number): string {
  const ar: number = aspectOf(widthVp, heightVp);
  if (ar <= 0) {
    return '';
  }
  const known: number[] = [
    0.5625,               // 9:16 竖屏视频
    0.70710678,           // 1:sqrt(2)
    0.75,                 // 3:4
    1.0,                  // 1:1 近方形
    1.41421356,           // sqrt(2):1
    1.6,                  // 16:10
    1.77777778            // 16:9
  ];
  const labels: string[] = ['≈9:16', '≈1:√2', '≈3:4', '≈1:1', '≈√2:1', '≈16:10', '≈16:9'];
  const tol: number = 0.008;   // 容差 ±0.008(约 ±0.8%): 容纳"px ÷ 密度再取整"的误差
                               // (664x940=0.7064 与 1/sqrt(2) 差 0.0007; 1060x660=1.6061 与 16:10 差 0.0061),
                               // 又不会认错(0.80 与 1/sqrt(2) 差 0.093, 16:9 与 16:10 差 0.178)。
  for (let i = 0; i < known.length; i++) {
    if (Math.abs(ar - known[i]) <= tol) {
      return '（' + labels[i] + '）';
    }
  }
  return '';
}

function foldSay(fs: string): string {
  if (fs === FS_EXPANDED) {
    return '展开';
  }
  if (fs === FS_FOLDED) {
    return '折叠';
  }
  if (fs === FS_HALF) {
    return '半折叠';
  }
  return '未知';
}

// 折叠状态与实测窗口尺寸是否冲突。
//  口径(见文件头): 窗口尺寸为准; 冲突时照样按窗口尺寸排, 但必须写出来。
//  判据用的是"方向"而不是某个具体尺寸: folded(折叠态)却给出了宽 > 高 的窗口,
//  或 expanded(展开态)却给出了窄到像外屏的窗口(宽 < 400vp), 都算冲突。
export function foldConflictOf(inp: ShapeInput): string {
  if (!inp.foldApiOk) {
    return '';
  }
  const w: number = inp.widthVp;
  const h: number = inp.heightVp;
  if (w <= 0 || h <= 0) {
    return '';
  }
  if (inp.foldState === FS_FOLDED && w > h) {
    return '系统报折叠态(display.getFoldStatus = folded), 但实测窗口是横向的 ' +
      Math.round(w).toString() + 'x' + Math.round(h).toString() +
      'vp —— 以窗口尺寸为准(折叠只影响文案, 不覆盖实测尺寸)';
  }
  if (inp.foldState === FS_EXPANDED && w < 400) {
    return '系统报展开态(display.getFoldStatus = expanded), 但实测窗口只有 ' +
      Math.round(w).toString() + 'x' + Math.round(h).toString() +
      'vp 宽(窄得像外屏或分屏) —— 以窗口尺寸为准(展开只影响文案, 不覆盖实测尺寸)';
  }
  return '';
}

// 形态名: 同一套布局参数在不同来由下叫法不同, 名字必须反映实际来由,
//  否则关于页截图会误导人。名字里只印实时算出来的宽高比, 不写机型、不写面板数值。
function nameOf(id: string, inp: ShapeInput): string {
  const expanded: boolean = inp.foldApiOk && inp.foldState === FS_EXPANDED;
  const ratio: string = aspectOf(inp.widthVp, inp.heightVp).toFixed(3);
  const label: string = '，宽高比 ' + ratio;
  if (id === SHAPE_S0) {
    return '窗口尺寸未取到（兜底：按 ' + FALLBACK_W.toString() + 'x' + FALLBACK_H.toString() + 'vp 排）';
  }
  if (id === SHAPE_S1) {
    return '窄窗 · 竖长' + (expanded ? '（系统报展开态）' : '');
  }
  if (id === SHAPE_S2) {
    return '窄窗 · 阔直' + (expanded ? '（系统报展开态）' : '');
  }
  if (id === SHAPE_S3) {
    return '窄窗 · 竖长(极窄)' + (expanded ? '（系统报展开态）' : '');
  }
  if (id === SHAPE_S4) {
    return '近方形窗口（小边 < ' + MIN_SIDE_SQUARE.toString() + 'vp）';
  }
  if (id === SHAPE_S5) {
    //  2026-10 更正: 以前这里写"竖直内屏", 还写过按机型判断的名字。现在只说窗口:
    //   方向 + 实测比例。目标机型(Pura X Max)展开态内屏是 1:sqrt(2) 的竖长屏(2584x1828 px),
    //   实测窗口 664x940vp = 0.706 —— 一眼就能对上; 但名字里不写这个数值, 比例随窗口变。
    return '竖长宽窗' + label + (expanded ? '（系统报展开态）' : '');
  }
  if (id === SHAPE_S6) {
    return '近方形宽窗' + label + (expanded ? '（系统报展开态）' : '');
  }
  if (id === SHAPE_S7) {
    return '横向窗口' + label;
  }
  if (id === SHAPE_S8) {
    return '扁宽窗口' + label;
  }
  if (id === SHAPE_S9) {
    return '极窄窗口' + label;
  }
  if (id === SHAPE_S10) {
    return '极扁 / 矮窗口' + label;
  }
  if (id === SHAPE_S11) {
    return '横向宽窗' + label;
  }
  if (id === SHAPE_S12) {
    // 展开态(系统给的运行时事实)。名字里只说"展开 + 实测比例", 不说面板是 16:10 之类 ——
    // 面板数值会过时、也会被抄错(本项目就抄错过一次), 而且判定里根本没用它。
    return '展开态窗口' + label;
  }
  return '未识别窗口' + label;
}

// 形态编号(需求表里的 1..9); 0 = 未取到窗口。只用于显示。
function indexOf(id: string): number {
  if (id === SHAPE_S1) {
    return 1;
  }
  if (id === SHAPE_S2) {
    return 2;
  }
  if (id === SHAPE_S3) {
    return 3;
  }
  if (id === SHAPE_S4) {
    return 4;
  }
  if (id === SHAPE_S5 || id === SHAPE_S12) {
    return 5;
  }
  if (id === SHAPE_S6) {
    return 6;
  }
  if (id === SHAPE_S7 || id === SHAPE_S11) {
    return 7;
  }
  if (id === SHAPE_S8) {
    return 8;
  }
  if (id === SHAPE_S9 || id === SHAPE_S10) {
    return 9;
  }
  return 0;
}

function noteOf(id: string): string {
  if (id === SHAPE_S0) {
    return '窗口尺寸未取到：按最保守的单列参数兜底, 等 windowSizeChange / getWindowProperties 到达后立即重算';
  }
  if (id === SHAPE_S1 || id === SHAPE_S2 || id === SHAPE_S3) {
    return '窄窗：单列、内容满宽(最多 480vp)、紧凑间距, 开始按钮按窗口宽收缩';
  }
  if (id === SHAPE_S4 || id === SHAPE_S6) {
    return '近方形：左右分栏(5:7), 结果卡按可用宽度 1~2 列, 近方形的富余空间分给结果区';
  }
  if (id === SHAPE_S5) {
    return '竖长宽窗：左右分栏(5:7), 富余的纵向空间交给结果区(不是让内容列旁边空着)';
  }
  if (id === SHAPE_S7) {
    return '横向窗口：左右分栏(5:7), 内容居中限宽, 结果卡按可用宽度 1~3 列';
  }
  if (id === SHAPE_S8 || id === SHAPE_S11) {
    return '扁宽 / 宽窗：左右分栏(内容列 >= ' + DIV_SPAN_WIDE_AT.toString() +
      'vp 时 4:8, 否则 5:7), 内容居中限宽, 结果卡按可用宽度最多 3 列';
  }
  if (id === SHAPE_S9) {
    return '极窄：强制单列满宽、不设卡片最小宽度(宁可少列也不横向溢出), 字号略缩';
  }
  if (id === SHAPE_S10) {
    return '极扁 / 矮：开始按钮收到最小、行高收紧, 关键内容(分数与结果区)优先, 必要时纵向滚动';
  }
  if (id === SHAPE_S12) {
    return '展开态窗口(系统报 expanded)：按实测窗口照常排 —— 横向就分栏, 竖长就单列, 不做任何特例';
  }
  return '按实测窗口自适应';
}

// 判定依据: 逐条写清"这个窗口凭什么排成这样"。只讲几何(宽度落在哪档 / 高度够不够),
//  外加折叠状态这条运行时事实 —— 而且写明它与窗口尺寸冲突时以窗口尺寸为准。
function basisOf(id: string, inp: ShapeInput, p: LayoutPlan): string {
  const w: number = p.geometry.usedWidthVp;
  const h: number = p.geometry.usedHeightVp;
  const ar: number = p.geometry.aspect;
  let s: string = '窗口 ' + Math.round(w).toString() + 'x' + Math.round(h).toString() +
    'vp（宽高比 ' + ar.toFixed(3) + aspectHint(inp.widthVp, inp.heightVp) + '）';
  if (id === SHAPE_S0) {
    return '窗口尺寸未取到（windowSizeChange / getWindowProperties 都还没有结果）' +
      ' → 按兜底 ' + FALLBACK_W.toString() + 'x' + FALLBACK_H.toString() + 'vp 单列排';
  }
  s += ' → ';
  if (id === SHAPE_S9) {
    s += '宽 < ' + W_TINY.toString() + 'vp 或 宽高比 < ' + AR_ULTRA_NARROW.toString() + '（极窄：单列满宽）';
  } else if (id === SHAPE_S10) {
    s += '宽高比 >= ' + AR_ULTRA_FLAT_EXTREME.toString() + ' 或（高 ' + Math.round(h).toString() +
      'vp < ' + H_SHORT.toString() + 'vp 且 宽高比 >= ' + AR_ULTRA_FLAT.toString() +
      '）（极扁 / 矮：关键内容优先）';
  } else if (id === SHAPE_S8) {
    s += '宽高比 [' + AR_FLAT.toString() + ', ' + AR_ULTRA_FLAT_EXTREME.toString() + ')（扁宽）';
  } else if (id === SHAPE_S7 || id === SHAPE_S11) {
    s += '宽高比 [' + AR_WIDE.toString() + ', ' + AR_FLAT.toString() + ')（横向），窗口宽 ' +
      Math.round(w).toString() + 'vp' + (id === SHAPE_S11
        ? (' >= ' + W_ULTRA_WIDE.toString() + 'vp（很宽）') : (' < ' + W_ULTRA_WIDE.toString() + 'vp'));
  } else if (id === SHAPE_S4 || id === SHAPE_S6) {
    s += '宽高比 [' + AR_TALL.toString() + ', ' + AR_WIDE.toString() + ')（近方形），最小边 ' +
      Math.round(p.geometry.shortSideVp).toString() + 'vp ' +
      (p.geometry.shortSideVp < MIN_SIDE_SQUARE ? '< ' : '>= ') + MIN_SIDE_SQUARE.toString() + 'vp';
  } else if (id === SHAPE_S5) {
    s += '宽高比 < ' + AR_TALL.toString() + '（竖长）且窗口宽 ' + Math.round(w).toString() +
      'vp >= ' + DIV_MIN_W.toString() + 'vp（竖长宽窗）';
  } else {
    s += '窗口宽 ' + Math.round(w).toString() + 'vp < ' + DIV_MIN_W.toString() + 'vp（窄窗），宽高比 ' +
      (ar <= AR_OUTER_LIKE ? '<= ' + AR_OUTER_LIKE.toString() + '（极窄竖长）'
        : (ar < AR_TALL ? '< ' + AR_TALL.toString() + '（竖长）'
          : '>= ' + AR_TALL.toString() + '（阔）'));
  }
  // 横向排布: 内容限宽 + 分栏 + 列数 —— 全部由宽度算出
  s += '；内容列 ' + Math.round(p.geometry.contentVp).toString() + 'vp（限宽 ' +
    p.contentMaxVp.toString() + 'vp）；' +
    (p.split
      ? ('宽 ' + Math.round(w).toString() + 'vp >= ' + DIV_MIN_W.toString() +
        'vp 且左 ' + Math.round(p.geometry.leftColVp).toString() + 'vp >= ' +
        MIN_LEFT_COL_VP.toString() + '、右 ' + Math.round(p.geometry.rightColVp).toString() +
        'vp >= ' + MIN_RIGHT_COL_VP.toString() + ' → 分栏 ' + p.leftSpan.toString() + ':' +
        p.rightSpan.toString() + '（12 栅格）')
      : ('不分栏（宽 ' + Math.round(w).toString() + 'vp 不够或两栏会窄到不可读）')) +
    '；结果区 ' + Math.round(p.geometry.resultZoneVp).toString() + 'vp 放得下 ' +
    p.columns.toString() + ' 张 ' + MIN_CARD_VP.toString() + 'vp 的卡';
  // 竖向: 高度够不够, 压了哪几步
  s += '；窗口高 ' + Math.round(h).toString() + 'vp ' +
    (p.height.fits ? '放得下' : '放不下(结果区纵向滚动)');
  // 折叠状态这条运行时事实(它不参与几何, 只影响文案; 冲突时以窗口尺寸为准)
  if (inp.foldApiOk && (inp.foldState === FS_EXPANDED || inp.foldState === FS_FOLDED ||
    inp.foldState === FS_HALF)) {
    s += '；系统折叠状态 = ' + foldSay(inp.foldState) + '（display.getFoldStatus，仅影响文案）';
  }
  return s;
}

// 降级 / 冲突说明(空串 = 没有降级)。任何"用了默认值"都必须在这里写明, 不许静默。
function degradeOf(id: string, inp: ShapeInput): string {
  const parts: string[] = [];
  if (id === SHAPE_S0) {
    parts.push('窗口尺寸未取到(windowSizeChange 与 getWindowProperties 都没有结果) → ' +
      '布局按兜底 ' + FALLBACK_W.toString() + 'x' + FALLBACK_H.toString() +
      'vp 计算, 不是实测值; 尺寸到达后会立即重算');
  }
  const conflict: string = foldConflictOf(inp);
  if (conflict.length > 0) {
    parts.push('折叠状态与窗口尺寸冲突 → ' + conflict);
  }
  if (!inp.foldApiOk) {
    parts.push('折叠屏接口不可用(display.isFoldable / getFoldStatus 抛异常) → ' +
      '本机是否折叠屏未知; 布局不受影响(它本来就只看窗口尺寸), 但文案里不会出现折叠/展开字样');
  }
  return parts.join('；');
}

// ---------------------------------------------------------------------------
//  六、由实时窗口算出一整份布局计划
//
//   入参里的 id 只被"写进结果给人看"(以及给文案层当索引), 不参与任何计算 ——
//    所以把名字/编号全删掉, 布局结果不变。
// ---------------------------------------------------------------------------
export function planFor(id: string, inp: ShapeInput): LayoutPlan {
  //  布局算术的全部输入就是这两个数 
  const facts: ZoneFacts = geometryFactsFor(inp.widthVp, inp.heightVp);
  const w: number = facts.usedWidthVp;
  const h: number = facts.usedHeightVp;
  const s: number = facts.shortSideVp;
  const zone: number = facts.resultZoneVp;
  const cap: number = facts.columnCap;
  const contentMax: number = facts.contentMaxVp;
  const content: number = facts.contentVp;
  const sp: SplitInfo = splitInfoFor(w);
  const gutter0: number = gutterFor(s);
  const pad0: number = padFor(s);
  const row0: number = rowVpFor(h);
  // 竖向预算先算: 它可能把主按钮压小、把间距与内边距压紧, 后面的横向排布要用压过之后的值
  const fit: HeightFit = fitHeight(h, heroDiaFor(w), gutter0, pad0, row0, topInfoCompactFor(h));
  const gutter: number = fit.gutter;
  // 列数用最终生效的间距(gutter, 已被第 ② 步压过)复算:
  //  列数、最小宽、间距三者必须自洽 —— 离线用例会拿 plan 自己的三个字段再乘一遍, 必须 <= 结果区宽度。
  let columns: number = 1;
  let probe: number = cap;
  while (probe >= 1) {
    //  "放得下"这一条就是"不溢出"的定义
    if (probe * MIN_CARD_VP + (probe - 1) * gutter <= zone + 0.5) {
      columns = probe;
      break;
    }
    probe--;
  }
  // 窗口太矮时多列会让每张卡只剩一两行可见高度 -> 宁可单列(纵向滚动里内容完整)
  if (h < H_FLAT && columns > 1) {
    columns = 1;
  }

  let cardBasis: string = '100%';
  if (columns === 2) {
    cardBasis = '48%';
  } else if (columns >= 3) {
    cardBasis = '31%';
  }

  // 左/右栅格 span: 直接取 splitInfoFor 算出来的那一对(左右栏宽度就是由它换算的)
  const spanL: number = sp.leftSpan;
  const spanR: number = sp.rightSpan;

  const geometry: GeometryFacts = {
    widthVp: facts.widthVp,
    heightVp: facts.heightVp,
    usedWidthVp: w,
    usedHeightVp: h,
    aspect: facts.aspect,
    shortSideVp: s,
    longSideVp: facts.longSideVp,
    contentVp: content,
    splitWanted: sp.wanted,
    leftColVp: sp.leftVp,
    rightColVp: sp.rightVp,
    resultZoneVp: zone,
    columnCap: cap
  };

  const plan: LayoutPlan = {
    id: id,
    index: indexOf(id),
    name: nameOf(id, inp),
    columns: columns,
    split: sp.wanted,
    leftSpan: spanL,
    rightSpan: spanR,
    contentMaxVp: contentMax,
    cardBasis: cardBasis,
    minCardVp: columns >= 2 ? MIN_CARD_VP : 0,
    heroDia: fit.heroDia,
    btnH: btnHFor(s),
    fontScale: fontScaleFor(w, h),
    gutter: gutter,
    pad: fit.pad,
    radius: radiusFor(s),
    basis: '',
    note: noteOf(id),
    degrade: degradeOf(id, inp),
    height: fit.budget,
    geometry: geometry,
    // 结果页密度: 同样只吃实测窗口宽高(与 id / 折叠状态 / 机型无关)
    result: resultDecoFor(inp.widthVp, inp.heightVp)
  };
  // basis 里要引用算好的分栏/列数, 所以放在对象建好之后填(纯文本层, 不影响任何数值)
  plan.basis = basisOf(id, inp, plan);
  return plan;
}

// 一次性算完(带抖动抑制): UI 只调这一个
export function resolvePlan(inp: ShapeInput, prevId: string): LayoutPlan {
  return planFor(resolveShapeId(inp, prevId), inp);
}

// 不带抖动抑制的版本(离线用例用它, 结果只取决于输入本身)
export function resolvePlanPlain(inp: ShapeInput): LayoutPlan {
  return planFor(shapeIdOf(inp), inp);
}

// ---------------------------------------------------------------------------
//  七、可复制文本(关于页「布局自检」的一块 + 复制按钮共用同一份文本)
// ---------------------------------------------------------------------------
export function planLines(inp: ShapeInput, p: LayoutPlan, extra: string): string {
  const g: GeometryFacts = p.geometry;
  const measured: boolean = g.widthVp > 0 && g.heightVp > 0;
  let s: string = '[Crush分 · 布局自检]\n';
  s += '实测窗口 vp：' + (measured
    ? (Math.round(g.widthVp).toString() + ' x ' + Math.round(g.heightVp).toString() + ' vp')
    : ('（未取到 —— 本次用兜底 ' + Math.round(g.usedWidthVp).toString() + ' x ' +
      Math.round(g.usedHeightVp).toString() + 'vp 计算）')) + '\n';
  if (measured) {
    s += '宽高比：' + (g.aspect > 0 ? (g.aspect.toFixed(3) + aspectHint(inp.widthVp, inp.heightVp)) : '（未取到）') + '\n';
    s += '短边 / 长边：' + Math.round(g.shortSideVp).toString() + ' / ' +
      Math.round(g.longSideVp).toString() + ' vp\n';
  }
  s += '命中编号：' + p.id + ' · 编号 ' + p.index.toString() + ' · ' + p.name +
    '（只用于显示：布局参数全部由上面的窗口尺寸算出，与编号无关）\n';
  s += '内容列宽：' + (p.contentMaxVp > 0 ? ('限宽 ' + p.contentMaxVp.toString() + 'vp') : '不限（满宽）') +
    ' · 实际 ' + Math.round(g.contentVp).toString() + 'vp\n';
  s += '主分栏：' + (p.split
    ? ('是 —— 左操作 / 右结果 两栏（' + p.leftSpan.toString() + ':' + p.rightSpan.toString() +
      '，左 ' + Math.round(g.leftColVp).toString() + 'vp / 右 ' +
      Math.round(g.rightColVp).toString() + 'vp）')
    : '否（单栏）') + '\n';
  s += '结果卡列数：' + p.columns.toString() + '（结果区 ' +
    Math.round(g.resultZoneVp).toString() + 'vp，列数上限 ' + g.columnCap.toString() +
    '）· 基准宽 ' + p.cardBasis + ' · 最小宽 ' +
    (p.minCardVp > 0 ? (p.minCardVp.toString() + ' vp') : '不设下限') + '\n';
  s += '开始按钮直径：' + p.heroDia.toString() + ' vp\n';
  s += '按钮最小高度：' + p.btnH.toString() + ' vp\n';
  s += '字号缩放：x' + p.fontScale.toFixed(2) + '\n';
  s += '卡片间距 / 内边距 / 圆角：' + p.gutter.toString() + ' / ' + p.pad.toString() +
    ' / ' + p.radius.toString() + ' vp\n';
  s += '竖向空间预算：' + p.height.text + '\n';
  s += '结果页密度：' + p.result.text + '\n';
  // 首屏分块账本(逐块 vp / 固定还是拉伸 / 凭什么这么高; 见 ResultDeco.blocks)
  //  以前只有一个总数: 总数对不上时看不出是哪一块错 —— 现在一块一行, 谁都能核对。
  s += p.result.blocksText;
  s += '　· 三个大数字块不变量：' + p.result.l1BlockH.toString() + 'vp <= 可用 ' +
    p.result.availVp.toString() + 'vp x 30% = ' + Math.floor(p.result.availVp * 0.3).toString() +
    'vp → ' + (p.result.l1InvariantOk ? '满足' : '不满足') +
    '（占 ' + (p.result.l1Share * 100).toFixed(1) + '%）\n';
  s += '　　压缩标志：主按钮 ' + (p.height.heroCompressed ? '已压' : '未压') +
    ' · 间距/内边距 ' + (p.height.compactY ? '已压' : '未压') +
    ' · 顶部设备卡 ' + (p.height.topInfoCompressed ? '紧凑版' : '完整版') +
    ' · 结果区 ' + (p.height.scrollOnly ? '只能纵向滚动' : '首屏可见 2 行') + '\n';
  s += '判定依据：' + p.basis + '\n';
  s += '形态说明：' + p.note + '\n';
  s += '降级说明：' + (p.degrade.length > 0 ? p.degrade : '无（所有判定接口都可用）') + '\n';
  if (extra.length > 0) {
    s += extra;
  }
  return s;
}
