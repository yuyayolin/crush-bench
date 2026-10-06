import { common, bundleManager } from '@kit.AbilityKit';
import { thermal, BusinessError } from '@kit.BasicServicesKit';
import { window, mediaquery, promptAction, display } from '@kit.ArkUI';
import { pasteboard } from '@kit.BasicServicesKit';
import { BenchStore, SMT_ENABLED_KEY } from '../service/BenchStore';
import { BenchRunner, ProgressInfo, GpuProgress, Gb7ItemOutcome, SmtCapabilities,
  ChipJudgmentSnapshot, CpuAffinitySnapshot, SnRunResult, smtTopologyLine } from '../service/BenchRunner';
import { CrashGuard, CrashHit, BenchCrumb } from '../service/CrashGuard';
import { MemoryProbe, MemStatus } from '../service/MemoryProbe';
import { StressSession, CampaignState, CampaignReport, RoundOutcome, RoundStatus, FailItemDetail,
  SkipItemDetail, itemFailure, statItemName } from '../service/StressCampaign';
// 本文件不再直接 import libaurorabench.so: ArkTS 每多一处 .so 的 import 就多一条
// "module ... is not verified" 警告(本工程基线 11 条), 新增的 GPU-SNL 小节已经占掉了额度,
// 所以这里改用 BenchRunner 里既有的同义直通桥(它们本来就是为"少一条警告"而存在的)。
import { BenchRecord, TestResult, GpuSceneResult, Gb7Result } from '../model/BenchModel';
import { ScoreHistory, ScoreHistoryRow, ScoreHistoryResult, SCORE_HISTORY_FILE } from '../service/ScoreHistory';
import { RunLog, RunSummaryInput, RunHistoryExtra, RunHistoryRow, RunLogStats, RunHistoryStats,
  RunHistorySnapshot, APP_VER,
  RUNLOG_TAIL, RUNLOG_FILE,
  RUNLOG_MAX_BYTES, HISTORY_FILE, HISTORY_MAX_BYTES, HISTORY_KEEP,
  PH_GB7_SINGLE, PH_GB7_MULTI, PH_GB7_GPU, PH_OWN, PH_STRESS } from '../service/RunLog';
import { LogUpload, UploadResult, UploadChoice, UPLOAD_RANGE_CHOICES,
  UPLOAD_RANGE_LAST_RUN, UPLOAD_RANGE_HISTORY_N, UPLOAD_HISTN_DEFAULT } from '../service/LogUpload';
import { picker } from '@kit.CoreFileKit';
import { FullRunSession, FullRunOutcome, FullRunProgress, FullRunSection, ST_FAILED,
  FullReportStore, FullReportSaveResult, FullRunUiFacts, FullReportBuilder } from '../service/FullRun';
// 「本项是否真的随芯片性能变化」自检(纯 ArkTS, 只用本轮已有数据; 不跑负载、不改任何负载)
import { auditSingleStage, auditMultiStage, isComparable, summarize, SC_PASS, SC_FAIL,
  SC_UNPROVEN, ScalingSummary } from '../service/ScalingAudit';
import { DeviceProfile, DeviceProfileData, WindowSizeVp } from '../service/DeviceProfile';
import { TABLE_ENTRY_COUNT as DEVICE_TABLE_COUNT } from '../service/DeviceModelTable';
// 设计令牌(单一来源) 与 形态计划类型
import { C, Sp, Rd, Ft, scaledFont, edgeTop, edgeBottom, outline, accentGradient, barGradient, brandGradient, HAIRLINE, SHADOW_NONE } from '../common/Theme';
// 真正的沉浸光感: 鸿蒙 HDS 的点光源(@kit.UIDesignKit 的 hdsEffect)。
//  它是 6.0.0(20) 才进入 SDK 可用面的能力, 本工程的 compatibleSdkVersion 已抬到 "6.0.0(20)"
//  (见 build-profile.json5)—— 但"门槛抬上去了"不等于"每台机器都给得出这个能力",
//  所以调它之前照样先用 canIUse() 探(HDS_CAPABLE), 探不到就静默退回上一轮那套 ArkUI 绘制方案。
import { hdsEffect } from '@kit.UIDesignKit';
// "什么效果都没加"的 VisualEffect: 官方 createEffect() 造出来的那一只
import { uiEffect } from '@kit.ArkGraphics2D';
// 材质 / 磨砂用的半透明派生色(纯函数, 从令牌派生, 不引入任何十六进制字面量)
import { withAlpha } from '../common/Theme';
import { LayoutPlan, ResultBlock, ResultDeco, aspectHint } from '../common/LayoutPlan';
// metric(吞吐)的显示口径: 与 native 的 "%.4g" 同口径的 4 位有效数字(见 common/NumberFmt.ets)。
// 所有显示 metric 的地方都必须用它 —— 不许再写 toFixed, 否则屏幕上的吞吐解释不了屏幕上的分数。
import { metricUnit } from '../common/NumberFmt';
// 主行动卡的 2vp 描边令牌(仍然是静态描边, 不是阴影)
import { BORDER_THICK } from '../common/Theme';
// 结果页「三层呈现」的纯数据层(第一层三个大数字 / 第二层每个小节一行 / 第三层逐项明细)
import { ScoreCard, KeyRow, DetailRow, DetailGroup, CondFacts,
  buildCards, buildBigThree, buildCs1Cards, buildOwnKeyRows, buildKeyRows, buildDetailGroups, detailCount,
  num1, num2,
  condFromOutcome, condFromRecord } from '../common/ResultLayers';
// 「下载」目录落盘(报告自动写入 <下载>/Crush分/, 权限受限时给出用户可见的兜底路径)
import { ReportSink, ReportWriteResult, reportStampName } from '../service/ReportSink';
// 两份报告对比(libauroraref.so 的 compareReports, 只读纯计算): 回答用户的原始问题
//   ——「两个设备的分数比, 能不能定量算出芯片进步的比例」。界面里原始比与归一化比并排显示,
//   并写明原始比会被核数 / 频率档污染。
//   走 ReportCompare 这层桥(而不是本文件直接 import 'libauroraref.so'): ArkTS 每多一个文件
//   import 同一个 .so 就多一条 "module ... is not verified" 警告(本工程基线 11 条)。
import { CmpFile, CmpOutcome, CmpItemView, CmpAnchorView, CmpCompositeView, CmpInflationView,
  CmpAuditRow, CmpAuditSide, CmpStageParts, CmpCondSide,
  listLocalReports, pickReportFile, runCompare, fmtNum, fmtRatio, within5Text,
  readText } from '../service/ReportCompare';

// ---- 颜色令牌 ----
//  单一来源是 ../common/Theme.ets(设计令牌)。这里只是一层短别名, 免得把几千行调用点
//  全改一遍; 新写的代码请直接用 Theme 的令牌(C.xxx / Sp.x4 / Rd.lg), 不要再写十六进制字面量。
const BG: string = C.bg;
const CARD: string = C.card;
const CARD_HI: string = C.cardHi;
const CARD_EDGE: string = C.cardEdge;
const CHIP: string = C.chip;
const INSET: string = C.inset;
const TRACK: string = C.track;
const ACCENT: string = C.accent;
const ACCENT2: string = C.accent2;
const ACCENT_SOFT: string = C.accentSoft;
const GOOD: string = C.good;
const WARN: string = C.warn;
const DANGER: string = C.danger;
const TEXT: string = C.text;
const SUB: string = C.subText;
const DIM: string = C.dim;
const FAINT: string = C.faint;
const ON_ACCENT: string = C.onAccent;
const DIVIDER: string = C.divider;

// ---- 断点常量(只作旁证与兜底) ----
//  sm = (width<600vp) / md = (600vp<=width<840vp) / lg = (width>=840vp)
//  形态判定不看断点: 它看的是实时窗口 vp 宽高 + 宽高比 + 折叠状态 + 设备类型,
//   规则与参数表在 ../common/LayoutPlan.ets, 关于页「布局自检」逐项显示并可复制。
const BP_LG: number = 840;
const BP_MD: number = 600;
// 窗口尺寸一次都没回调到时的兜底宽度(取最窄手机, 保证首帧不会按宽屏排版)
const WIN_W_SM: number = 360;
// 标签栏高度
const TAB_H: number = 52;
// 开始按钮外圈的描边宽度(vp)。描边是骑在路径上的(路径内侧与外侧各一半), 所以
//   「外圈画笔直径 = 容器直径 - 描边宽」时, 圆环正好完全落在容器里 —— 容器不会切到圆环。
//   (改前容器比外圈小 8vp, 圆环被自己的容器切掉: 真机上就是"黑块上下露出一段断开的蓝弧"。)
const HERO_STROKE_VP: number = 4;

// ---- 旋转策略: 取值 -> (枚举名, 是否跟随传感器, 是否受系统旋转锁定约束) ----
//   依据(逐条抄自本机 SDK API 18, 不是凭记忆):
//    · module.json5 的合法取值: D:\ohos-tools\sdk\18\toolchains\modulecheck\module.json
//      里 abilities[].orientation.enum, 共 15 个;
//    · 语义: @ohos.window.d.ts 的 window.Orientation 逐条注释 ——
//        AUTO_ROTATION            = 5  "Follow the rotation of the sensor, **ignore** auto rotation lock."
//        AUTO_ROTATION_RESTRICTED = 8  "Follow the rotation of the sensor, **controlled by** auto rotation lock."
//        AUTO_ROTATION_UNSPECIFIED= 12 "Follow the rotation of the sensor, determined by the system, **controlled by** auto rotation lock."
//    · 运行时回读用的枚举: @ohos.bundle.bundleManager.d.ts 的 DisplayOrientation(下表下标就是它的取值)。
//  本表只用于「关于」页把回读到的数字翻译成人话, 不参与任何布局或计分。
const ROT_ENUM_NAMES: string[] = [
  'UNSPECIFIED', 'LANDSCAPE', 'PORTRAIT', 'FOLLOW_RECENT', 'LANDSCAPE_INVERTED',
  'PORTRAIT_INVERTED', 'AUTO_ROTATION', 'AUTO_ROTATION_LANDSCAPE', 'AUTO_ROTATION_PORTRAIT',
  'AUTO_ROTATION_RESTRICTED', 'AUTO_ROTATION_LANDSCAPE_RESTRICTED',
  'AUTO_ROTATION_PORTRAIT_RESTRICTED', 'LOCKED', 'AUTO_ROTATION_UNSPECIFIED', 'FOLLOW_DESKTOP'
];
const ROT_ENUM_FOLLOWS_SENSOR: boolean[] = [
  false, false, false, false, false, false, true, true, true, true, true, true, false, true, false
];
const ROT_ENUM_LOCKS: boolean[] = [
  false, true, true, true, true, true, false, false, false, true, true, true, true, true, false
];
// 本项目在 module.json5 里声明的那个取值(声明与实现必须一致; 不一致时关于页会直接指出来)
const ROT_DECLARED_STRING: string = 'auto_rotation_restricted';
const ROT_DECLARED_ENUM_INDEX: number = 9;   // AUTO_ROTATION_RESTRICTED
// window.Orientation(getPreferredOrientation 的返回值)的枚举名; 它的取值顺序与上面不同
const WIN_ORIENT_NAMES: string[] = [
  'UNSPECIFIED', 'PORTRAIT', 'LANDSCAPE', 'PORTRAIT_INVERTED', 'LANDSCAPE_INVERTED',
  'AUTO_ROTATION', 'AUTO_ROTATION_PORTRAIT', 'AUTO_ROTATION_LANDSCAPE',
  'AUTO_ROTATION_RESTRICTED', 'AUTO_ROTATION_PORTRAIT_RESTRICTED',
  'AUTO_ROTATION_LANDSCAPE_RESTRICTED', 'LOCKED', 'AUTO_ROTATION_UNSPECIFIED',
  'USER_ROTATION_PORTRAIT', 'USER_ROTATION_LANDSCAPE'
];
// 屏幕旋转角(display.getDefaultDisplaySync().rotation 是 0~3 的序号, 不是角度)
const ROT_SCREEN_DEG: string[] = ['0°', '90°', '180°', '270°'];

// =========================================================================
// 美化层(沉浸光感 + 粒子消散): 能力探测、粒子规格、粒子引擎
//
//  这个工程被真机缺陷咬过一次 —— 一个"装饰性滤镜"在当时的运行时上把首页主按钮
//  整块画成了纯黑(取证写在 syncFx() 上方)。所以美化这一层从今天起只有一条铁律:
//    **拿不到能力就什么都不做 —— 控件的颜色 / 尺寸 / 文字一个字节都不许变。**
//  探测只问一次(模块加载时), 不轮询、不监听。拿不到时 FX_CAPABLE = false,
//  于是 fxOn() 恒为 false, 整条美化链路(光感节点 / 粒子 Canvas / tick)一次都走不到。
// =========================================================================
function fxProbe(): boolean {
  try {
    // 这一层用的全部是 ArkUI 自带的绘制能力(阴影 / 径向渐变 / Canvas), 都属于同一个 syscap。
    return canIUse('SystemCapability.ArkUI.ArkUI.Full');
  } catch (e) {
    return false;
  }
}
const FX_CAPABLE: boolean = fxProbe();

// =========================================================================
// 真正的沉浸光感: 鸿蒙 HDS 的点光源(hdsEffect.pointLight)
//
//  这套 API 在 6.0.0(20) 进入 SDK 可用面。本工程的 compatibleSdkVersion 已抬到
//  "6.0.0(20)"(build-profile.json5), 所以编译面是通的 —— 但**抬了门槛不等于每台机器
//  都给得出这个能力**, 所以这里的规矩和上面那层 ArkUI 绘制方案一字不差:
//    ① 只有 fxOn() 为真(不跑分 + ArkUI 绘制能力可用)时才构造点光 —— 见 fxLight();
//    ② 拿不到 HDS 能力就静默退回 ArkUI 那套(径向辉光 / 光晕 / 渐变光带),
//       那套一个字都没删, 它现在同时是"兜底"和"点光不在时的主效果"。
// =========================================================================
function hdsProbe(): boolean {
  try {
    return canIUse('SystemCapability.UIDesign.HDSComponent.Core');
  } catch (e) {
    return false;
  }
}
const HDS_CAPABLE: boolean = hdsProbe();

// 关掉美化 / 拿不到 HDS 能力时 .visualEffect() 收的那个值。
//  为什么需要一只"空效果": ArkUI 的 .visualEffect(effect) 只收 VisualEffect, 不收 null;
//  而关掉时我们又**绝不能**去调 hdsEffect 现造一只 —— 那就等于"跑分期间还在调 hdsEffect"。
//  所以给的是官方 createEffect() 造出来、一个效果都没加的那一只: 渲染上与"没有这条属性"等价,
//  而点光一次都没被构造过。它是**惰性**造的 —— 只在真的需要退回时才第一次被造出来。
//  (SystemCapability.Graphics.Drawing 在本 App 声明的 default / tablet / 2in1 三种形态上都有,
//   见 SDK 的 device-define 数据, 所以这里不会走到"造不出来"的那条路。)
let FX_NO_LIGHT_CACHE: VisualEffect | null = null;
function fxNoLight(): VisualEffect {
  let v: VisualEffect | null = FX_NO_LIGHT_CACHE;
  if (v === null) {
    v = uiEffect.createEffect();
    FX_NO_LIGHT_CACHE = v;
  }
  return v;
}

// 粒子规格: 克制是硬指标。
//  粒子画在 UI 线程上, 而这个 App 的全部价值是"分数可复现", 所以宁可少画几个圆,
//  也不许让任何一帧的绘制时间有肉眼可感的抖动。上限 = 一场最多几个 / 一帧多少毫秒 / 一场几帧。
const FX_P_MAX: number = 32;
const FX_TICK_MS: number = 34;      // ≈29 帧/秒: 够顺, 又不跟任何东西抢调度
const FX_FRAMES: number = 18;       // 一场粒子的寿命(帧) —— 34ms x 18 ≈ 0.61 秒

// 一个粒子的全部状态(纯数据, 无副作用)。
class FxParticle {
  x: number = 0;
  y: number = 0;
  vx: number = 0;
  vy: number = 0;
  r0: number = 2.4;
  spin: number = 0;
}

// 固定种子的线性同余发生器。
//  为什么不用 Math.random: 这个工程的每一次界面变化都要可复现 —— 同一场粒子的轨迹
//  每次跑出来完全一样, 截图对比才有意义(用例里也是同样的做法)。
class FxRand {
  private s: number = 1;
  constructor(seed: number) {
    this.s = seed >>> 0;
    if (this.s === 0) {
      this.s = 1;
    }
  }
  next(): number {
    this.s = (this.s * 1664525 + 1013904223) >>> 0;
    return this.s / 4294967296;
  }
}


// 报告文本在界面上最多显示多少字符(完整文本走"复制报告")
const REPORT_PREVIEW = 1500;

// App 名 + 版本(顶栏显示与「一键全部跑分」报告抬头共用这一处)
//  版本号不在这里写死  —— 唯一来源是 service/RunLog.ets 的 APP_VER(与 AppScope/app.json5 同步升级)。
// 当年这里写死过 '极光跑分 7.5' 而 RunLog 里写死 '5.2', runlog 与报告互相矛盾
// (那会儿显示名还是中文、报告抬头还是英文, 两个名字不是同一个)。
const APP_LABEL: string = 'Crush分 ' + APP_VER;
// 「一键全部跑分」报告在浮层里最多预览多少字符(完整文本走 复制/导出)
const FULL_REPORT_PREVIEW: number = 4000;
// 进度浮层里最多保留多少行已完成项(静态文字, 不滚动动画)
const FULL_LINE_KEEP: number = 40;
// 「历史」页最多渲染多少条(账本是永久的、可能很长; 渲染上限只影响这一页显示多少行,
// 一条都不会删 —— 完整的历次记录在「下载/Crush分/历史成绩.txt」与账本文件里)
const HISTORY_PAGE_MAX: number = 200;

// 每轮耗时的总和(压测会话内的实测值)
function totalDurationMs(s: CampaignState): number {
  let sum = 0;
  for (const d of s.durations) {
    sum += d;
  }
  return sum;
}

// 导出文件名里的时间戳: 20261004-0250
function fileStamp(): string {
  const d = new Date();
  const p = (n: number): string => n < 10 ? '0' + n.toString() : n.toString();
  return d.getFullYear().toString() + p(d.getMonth() + 1) + p(d.getDate()) + '-' +
    p(d.getHours()) + p(d.getMinutes());
}

// 「最近一次发送」那一行前面的时间戳: 10-04 02:50
function stampText(): string {
  return fmtTime(Date.now());
}

function exportFileName(isHistory: boolean): string {
  return isHistory ? ('aurora-runhistory-' + fileStamp() + '.jsonl')
    : ('aurora-runlog-' + fileStamp() + '.txt');
}

function fmtTime(t: number): string {
  const d = new Date(t);
  const mm = (d.getMonth() + 1).toString().padStart(2, '0');
  const dd = d.getDate().toString().padStart(2, '0');
  const hh = d.getHours().toString().padStart(2, '0');
  const mi = d.getMinutes().toString().padStart(2, '0');
  return mm + '-' + dd + ' ' + hh + ':' + mi;
}

@Entry
@Component
struct Index {
  @State tab: number = 0;
  // 跑分标志位一律挂 @Watch('onRunFlagChange'): 任何一个负载开跑/收工, 都会走
  //  onRunFlagChange() -> syncFx() -> 立刻关掉一切装饰性视效(材质模糊 / 转场),
  //  跑完再恢复。这是"跑分期间零装饰开销"的唯一开关, 详见 syncFx()。
  @State @Watch('onRunFlagChange') running: boolean = false;
  // ---- 沉浸式(安全区 insets)与装饰性视效开关 ----
  //  ① insets 由 window.getWindowAvoidArea() 给出(px), 这里存换算好的 vp; 只在系统栏
  //     变化(旋转 / 折叠 / 手势条显隐 / 分屏)时更新, 没有任何轮询;
  //  ② 装饰性视效(材质 / 转场 / 光感 / 粒子)的总开关是 **fxOn() 这个函数**(不是字段):
  //     跑分期间它恒为 false —— 材质模糊改用 ALWAYS_INACTIVE 策略, 转场退化成 IDENTITY,
  //     光感节点与粒子 Canvas 干脆不创建。跑分期间没有任何一样会占用 CPU / GPU。
  @State insetTopVp: number = 0;
  @State insetBottomVp: number = 0;
  @State immersiveOn: boolean = false;      // 窗口是否真的铺满整屏(没铺满就不需要 insets padding)
  // ★ 装饰性视效(材质 / 转场 / 光感 / 粒子)没有"开关状态"字段 ——
  //   唯一判据是下面的 fxOn() 函数, 它直接读既有的跑分标志位(见该函数的说明)。
  //   这里只放"粒子层要不要出现在界面上"这一个 @State: 它是唯一需要驱动节点增删的东西。
  @State fxLive: boolean = false;           // 粒子 Canvas 是否挂在界面上(跑分期间恒为 false)
  // 下面全部是命令式的绘制状态, 刻意**不**做成 @State: 粒子逐帧画在 Canvas 上,
  //   每帧写一次 @State 只会让框架每帧重排界面 —— 那才是真的掉性能。
  private fxCtx: CanvasRenderingContext2D | null = null;   // 惰性创建: 没有粒子时连它都不建
  private fxReady: boolean = false;          // Canvas.onReady 是否已经回调过
  private fxTimer: number = -1;              // setInterval 句柄; -1 = 没有注册任何 tick
  private fxParts: FxParticle[] = [];        // 当前这一场的粒子
  private fxFrame: number = 0;               // 这一场已经画了多少帧
  private fxColor: string = ACCENT;          // 这一场粒子的颜色(令牌派生, 不写十六进制字面量)
  private fxRand: FxRand = new FxRand(20261006);  // 固定种子(界面动效也要可复现)
  private fxHx: number = -1;                 // 「开始跑分」圆心(窗口坐标 vp; -1 = 还没量到)
  private fxHy: number = -1;
  private fxBarX: number = -1;               // 底栏左边缘的窗口 x(vp)
  private fxBarY: number = -1;               // 底栏上边缘的窗口 y(vp)
  private fxBarW: number = 0;                // 底栏宽(vp)
  @State phase: string = '';
  @State doneCount: number = 0;
  @State totalCount: number = 0;
  @State curTest: string = '准备中';
  @State curMs: number = 0;
  @State curScore: number = 0;
  @State live: TestResult[] = [];
  @State records: BenchRecord[] = [];
  @State best: number = 0;
  @State shown: BenchRecord | null = null;
  @State device: string = '';
  @State cores: number = 0;
  @State nativeVer: string = '';
  @State gpuActive: boolean = false;
  @State gpuScene: string = '';
  @State gpuFps: number = 0;
  @State gpuSceneIdx: number = 0;
  @State gpuSceneTotal: number = 0;
  @State @Watch('onRunFlagChange') stressRunning: boolean = false;
  @State stressRound: number = 0;
  @State stressTotals: number[] = [];
  @State thermalLevel: number = -1;
  @State stressThermal: number[] = [];
  @State @Watch('onRunFlagChange') coremarkRunning: boolean = false;
  @State coremarkResult: string = '';
  @State @Watch('onRunFlagChange') gb7Running: boolean = false;
  @State gb7Results: Gb7Result[] = [];
  @State gb7Composite: number = 0;
  @State @Watch('onRunFlagChange') gb7MultiRunning: boolean = false;
  @State gb7MultiResults: Gb7Result[] = [];
  @State gb7Threads: number = 1;
  @State @Watch('onRunFlagChange') gb7GpuRunning: boolean = false;
  @State gb7GpuResults: Gb7Result[] = [];
  @State gb7GpuComposite: number = 0;
  // GPU-SNL(Aurora Nomad Light): 独立小节 libaurorasn.so, 只服务"单项测试"里的手动入口。
  // 结果刻意拆成非空标量(而不是一个可空对象): @Builder 里对可空成员做类型收窄不可靠,
  // 而这一节的每个数字(分数 / FPS / 吞吐 / 自证 / 标定)都必须显示。
  @State @Watch('onRunFlagChange') snRunning: boolean = false;
  @State snHave: boolean = false;        // 是否拿到了 native 的返回(无论成败)
  @State snOk: boolean = false;
  @State snPrepErr: string = '';         // prepare() 返回的驱动原文(空串 = 初始化成功), 原样显示
  @State snErrMsg: string = '';
  @State snScoreV: number = 0;
  @State snFpsV: number = 0;
  @State snTputV: number = 0;
  @State snVerdictV: string = '';
  @State snConclusiveV: boolean = false;
  @State snDrawV: number = -1;
  @State snCpuVertV: number = -1;
  @State snDutyV: number = 0;
  @State snCalV: string = '';
  @State snCalKV: number = 0;
  @State snPassesV: number = 0;
  @State snScaleSrcV: string = '';
  @State snCredV: string = '';
  @State snDispV: number = 0;
  @State snEnvV: string = '';
  @State snTempV: number = 0;
  @State snKhzV: number = 0;
  @State snVerV: number = 0;
  @State snWarnV: string = '';
  @State snLogiV: number = 0;
  @State snPhysV: number = 0;
  @State snArchV: number = -1;
  @State snNeonV: boolean = false;
  @State snSve2V: boolean = false;
  // ---- 响应式状态(全部由 ../common/LayoutPlan.ets 的形态计划推导; 这里只做"重排信号") ----
  @State bp: string = 'sm';          // mediaquery 三档, 只作旁证
  @State winW: number = WIN_W_SM;
  @State winH: number = 0;
  @State startDia: number = 137;     // 开始按钮直径(形态计划已按窗口宽算好并夹好上下限)
  @State planId: string = '';        // 当前命中的形态编号 S0 ~ S12(只用于显示/日志, 不参与布局决策)
  @State profileSplit: number = 0;   // 1 = 首页"左操作 / 右结果"左右分栏
  @State spanLeft: number = 12;      // GridRow 12 栅格: 左栏
  @State spanRight: number = 12;     // GridRow 12 栅格: 右栏
  @State uiGutterVp: number = 8;     // 卡片间距
  @State uiPadVp: number = 14;       // 卡片内边距
  @State uiRadiusVp: number = 16;    // 卡片圆角
  @State uiFontScale: number = 1;    // 字号缩放(平板 / 电脑观看距离更远 -> 略放大)
  @State uiBtnH: number = 38;        // 按钮最小高度
  // ---- 结果页(分数界面)的排版密度(见 common/LayoutPlan.ets 的 resultDecoFor) ----
  //  用户反馈"显示分数的界面间隔太远, 必须要一目了然": 结果页三处界面(一键跑分结果页 /
  //  单次跑分结果浮层 / 首页分数卡)共用这一份, 所以它们的间距与字号永远一致。
  //   它只吃实测窗口宽高: 大屏左右并排 + 更大字号 + 更多列, 小屏紧凑 + 结论放前面。
  //  初值必须与其余布局状态同源: 它取的是 DeviceProfile 里那份唯一的形态计划
  //    (plan().result), 而不是另算一遍 resultDecoFor(0, 0)。
  //    为什么这件事重要: 开始页(还没有任何跑分结果时)也要用这里的字号与宽高去画控件。
  //    以前这里写的是 resultDecoFor(0, 0) —— 它绕开了 DeviceProfile, 于是首帧可能出现
  //    "结果页密度按兜底 360x780 算、内容列宽 / 按钮直径 / 分栏却按已有计划算" 的自相矛盾画面;
  //    一旦窗口尺寸那条路(probeWindowSize)还没回来, 这个矛盾就会一直留在屏幕上。
  //    现在全页只有一个来源: DeviceProfile.get().plan() —— 窗口尺寸一到达,
  //    syncProfile() 把它整份换掉, uiRes 与其余 @State 永远同进退。
  //   不许改回与窗口无关的常量(哪怕是 resultDecoFor(0, 0)):
  //    ref/layout_cases.test.mjs 的 P 节会把这一行当成断言来核对。
  @State uiRes: ResultDeco = DeviceProfile.get().plan().result;
  // ---- 旋转策略回读(只读; 「关于」页照实显示) ----
  //  回答的是用户那句"系统开了旋转锁定, 为什么在这个 App 里还能转": 把声明的策略
  //  (从系统里回读, 不是把 module.json5 抄一遍)、运行时的首选方向、当前屏幕方向一并摆出来。
  //   系统"旋转锁定"开关本身没有公开 API(本机 SDK 全量搜过 rotationLock / autoRotation,
  //    公开 d.ts 里一条都没有), 应用读不到它的实时状态 —— 这一点会写在界面上, 不假装知道。
  @State rotDeclared: string = '';        // 声明的旋转策略(人话)
  @State rotDeclaredRaw: string = '';     // 声明值的原始读数(枚举名 + 数字, 供核对)
  @State rotRespect: number = -1;         // 1 = 尊重系统锁定 / 0 = 无视 / -1 = 未知
  @State rotEffective: string = '';       // window.getPreferredOrientation() 读数
  @State rotScreen: string = '';          // 当前屏幕旋转角
  @State rotNote: string = '';            // 读不到时的原因 / 一行结论
  // 本次启动以来实测窗口方向(横/竖)变化了几次 —— 这是"到底还会不会自己转"的硬证据:
  //  系统旋转锁定开着时, 用户把机器转 90 度, 这个数应当保持不动。
  //  (系统那个锁定开关本身读不到, 所以给一个可自证的计数器, 而不是替系统下结论。)
  @State rotFlips: number = 0;
  private rotLastPortrait: number = -1;
  @State devShapeLine: string = '';  // 「布局自检」: 命中形态一行
  @State devShapeBasis: string = ''; // 「布局自检」: 判定依据
  @State devShapeNote: string = '';  // 「布局自检」: 形态说明
  @State devShapeDegrade: string = ''; // 「布局自检」: 降级原因(空 = 无降级)
  @State devSelfCheck: string = '';  // 「布局自检」可复制全文
  @State devSelfCheckCopied: boolean = false;
  // ---- 竖向空间预算(见 common/LayoutPlan.ets 的 fitHeight) ----
  //  tightY = 顶栏+标签栏+设备卡+主按钮+2 行结果 在窗口高度里放不下(展开态且横向的窗口、
  //  横屏手机、用户拖出来的扁窗口就是这一档): 顶部「设备」卡切成紧凑版, 诊断用的几行收起来。
  //   例子里不写"阔折叠内屏 16:10": 那是被证伪的假设(Pura X 的数据被套到了 Pura X Max
  //    上)。本机展开态是 1:sqrt(2) 的竖长内屏(664x940vp), 高度很宽裕, 根本走不到这一档。
  @State tightY: boolean = false;
  @State devHeightBudget: string = '';   // 「布局自检」: 竖向预算那一行结论
  // ---- 设备画像(传播名 + 形态 + 由它推导出的布局参数) ----
  @State devName: string = '';
  @State devModelLine: string = '';
  // 「本机性能天花板」一行(2026-10 追加): native 唯一来源(BenchRunner.ceiling()),
  //   首页设备卡 / 结果页首屏 / 一键跑分报告三处显示的是同一句话。
  //   它回答的是"这台机器到底被什么卡住": 全机最快档在哪几核多少 MHz -> 内核实测只放用哪几核
  //   -> 因此单核最高只能跑哪一档 / 多核最多几个线程 / 运行时频率占标称多少。
  //   空串 = native 未上报(旧版 .so / 读取失败), 界面按"未上报"显示, 不编一句近似的冒充。
  @State ceilingLine: string = '';
  @State devFoldTag: string = '';
  @State devProfileText: string = '';
  @State devProfileCopied: boolean = false;
  // 内置机型对照表(110 条, 由 ref/device_models.json 生成)查到的: 匹配方式 / 屏幕参数 / 来源域名
  @State devMatchMethod: string = '';
  @State devTableScreen: string = '';
  @State devTableSource: string = '';
  @State devTableHit: boolean = false;
  // 最近一次跑分的绑核状态(native runGb7 的 cpuInfo 文本; 没跑过为空)
  @State devCpuInfo: string = '';
  // ---- 内核允许的核集合(2026-10-05): 回答"为什么这台机器快/慢"的三行事实 ----
  //   devAffinityLine = 一行结论(可用核集合 vs 快簇 vs 交集);
  //   devAllowedDetail = 逐路径实测事实(Cpus_allowed / sched_getaffinity / cgroup / cpuset);
  //   devAffinityFull = native 给的完整一行(可用核集合 + 快簇来源 + cpuset + Mems_allowed_list)。
  @State devAffinityLine: string = '';
  @State devAffinityFull: string = '';
  @State devAllowedDetail: string = '';
  // 全机频率档(逐核口径, 读失败的核也带 errno): 与可用核集合放一起, 两台机器一眼可比
  @State devFreqTiers: string = '';
  // 多核阶段的线程数上限被可用核集合夹过时的那一句(空 = 没被夹, 保持历史行为)
  @State devThreadCapNote: string = '';
  // ---- 芯片判读(三段, 顺序固定: 本机实测 -> 逐核原始读数 -> 第三方资料推断) ----
  @State devChipMeasured: string = '';
  @State devChipPerCore: string = '';
  @State devChipInfer: string = '';
  // ---- 超线程(SMT)开关: 默认开(与历史行为一致); 关掉 = 多核阶段每个物理核只用一条线程 ----
  @State smtOn: boolean = true;
  // 拓扑读不到时(或 native 接口不可用)的界面提示: 非空 = 开关无效, 界面上要写明原因
  @State smtNote: string = '';
  // 只作为"重排信号": 折叠状态变化时列数可能变, 用这个 @State 触发重建
  @State profileColumns: number = 1;
  // ---- 「布局自检」的纯几何事实(全部来自 LayoutPlan 的 geometry, 不在这里另算一遍) ----
  @State geoW: number = 0;          // 实测窗口宽(0 = 未取到)
  @State geoH: number = 0;          // 实测窗口高
  @State geoShort: number = 0;      // 短边
  @State geoLong: number = 0;       // 长边
  @State geoContent: number = 0;    // 内容列宽度
  @State geoZone: number = 0;       // 结果区可用宽度
  @State geoLeft: number = 0;       // 左栏宽度(不分栏 = 0)
  @State geoCap: number = 1;        // 这个宽度下允许的最大列数
  // ---- 崩溃面包屑 / 内存护栏 ----
  @State crumbFound: boolean = false;
  @State crumbLine: string = '';
  @State crumbDetail: string = '';
  @State crumbLog: string[] = [];
  @State crumbLogOpen: boolean = false;
  @State memChecked: boolean = false;
  @State memSupported: boolean = false;
  @State memAvailMb: number = 0;
  @State memTotalMb: number = 0;
  @State memLow: boolean = false;
  @State memNote: string = '';
  @State memAt: number = 0;
  // ---- 稳定性压测(连续 N 轮 CS1 单核) ----
  @State @Watch('onRunFlagChange') scRunning: boolean = false;
  @State scPickOpen: boolean = false;
  @State scTarget: number = 50;
  @State scDone: number = 0;
  @State scCurRound: number = 0;
  @State scCurItem: string = '';
  @State scLastRoundMs: number = 0;
  @State scLastRoundFails: number = 0;
  @State scLive: Gb7Result[] = [];
  @State scReport: CampaignReport | null = null;
  @State scReportText: string = '';
  @State scReportOpen: boolean = false;
  @State scInterrupted: boolean = false;
  // 压测会话还没结束, 但当前没在跑(例如轮次之间被系统回收 / 用户划掉): 只给"继续", 不算崩溃
  @State scPaused: boolean = false;
  @State scInterruptRound: number = 0;
  @State scInterruptItem: string = '';
  @State scMemNote: string = '';
  @State scGateUsed: boolean = false;
  @State scGateDown: boolean = false;
  // 上次崩溃的归因信息(内存态): 用户点"继续压测"时才写进压测会话, 避免把一次崩溃登记两遍
  @State scCopied: boolean = false;
  // 本会话已登记的崩溃轮次数(崩溃恢复时从盘上会话读回来)
  @State scCrashRuns: number = 0;
  // ---- 运行日志(runlog.jsonl): 「关于」页查看 / 复制 / 清空 + 可选联网上传 ----
  @State logSizeText: string = '--';
  @State logLines: number = 0;
  @State logFiles: number = 0;
  @State logRangeText: string = '';
  @State logOk: boolean = true;
  @State logNote: string = '';
  @State logTail: string = '';
  @State logCopied: boolean = false;
  @State logClearArm: boolean = false;
  @State logStatus: string = '';
  @State uploadUrl: string = '';
  @State uploadOn: boolean = false;
  @State uploadNote: string = '';
  @State uploadBusy: boolean = false;
  // ---- native 崩溃取证装置(crash_guard.cpp): 装置状态 + 启动自检读回的现场 ----
  @State nGuardReady: boolean = false;
  @State nGuardPath: string = '';
  @State nGuardStatus: string = '';
  @State nGuardNote: string = '';
  @State nCrashFound: boolean = false;
  @State nCrashIsCrash: boolean = false;
  @State nCrashPreview: string = '';
  @State nCrashChars: number = 0;
  @State nCrashSummary: string = '';
  @State nCrashCopied: boolean = false;
  // ---- 「关于」页 · 崩溃取证实测卡(排障用; 按钮 2/3 会故意让 App 崩溃) ----
  // 默认不可见: 只有在「关于」页底部对版本号连点 5 次才展开 —— 那是"一点就崩"的按钮,
  // 收起来是为了防止平时误触把 App 弄崩, 不是别的原因。
  @State showDebugTools: boolean = false;
  @State verTaps: number = 0;
  // 「关于」页的二级导航: 0 = 一级(只有入口列表), 1..7 = 对应的二级页面。
  //  用户要求"一级极简、东西全放二级", 所以一级一个字的说明都不放。
  @State aboutPage: number = 0;
  @State verHint: string = '';
  @State stNote: string = '';
  @State stReport: string = '';
  @State stCopied: boolean = false;
  // ---- 永久账本 runhistory.jsonl(每完成一轮一行, 5 MB 才轮转) ----
  @State histLines: number = 0;
  @State histSizeText: string = '--';
  @State histRangeText: string = '';
  @State histFiles: number = 0;
  @State histPreview: string = '';
  @State histStatus: string = '';
  // ---- 上传: 发送范围 / 最近一次结果 / 接口说明 ----
  @State uploadRange: string = UPLOAD_RANGE_LAST_RUN;
  @State uploadHistN: number = UPLOAD_HISTN_DEFAULT;
  @State uploadLastResult: string = '';
  @State uploadIfaceOpen: boolean = false;
  @State exportStatus: string = '';
  // ---- 「一键全部跑分」(编排 + 报告): 全部是静态文字状态, 没有任何动画状态 ----
  @State @Watch('onRunFlagChange') fullRunning: boolean = false;
  @State fullStopArmed: boolean = false;
  @State fullSectionName: string = '准备中';
  @State fullSectionIdx: number = 0;
  @State fullSectionCount: number = 7;
  @State fullItemName: string = '';
  @State fullItemIdx: number = 0;
  @State fullItemTotal: number = 0;
  @State fullDoneItems: number = 0;
  @State fullTotalItems: number = 0;
  @State fullDoneSections: number = 0;
  @State fullLines: string[] = [];
  @State fullSectionLines: string[] = [];
  @State fullOutcome: FullRunOutcome | null = null;
  @State fullReportOpen: boolean = false;
  @State fullReportPreview: string = '';
  @State fullReportChars: number = 0;
  @State fullReportPaths: string = '';
  @State fullReportStatus: string = '';
  @State fullReportCopied: boolean = false;
  @State fullReportSaved: boolean = true;
  @State fullHasOldReport: boolean = false;
  @State fullOldReportBytes: number = 0;
  // ---- 结果页「三层呈现」的数据(在跑完那一刻算一次; 纯数据, 不参与任何渲染循环) ----
  @State fullL1: ScoreCard[] = [];          // 第一层: 单核 / 多核 / GPU 三个大数字
  @State fullL2: KeyRow[] = [];             // 第二层: 每个小节的关键得分一行
  @State fullL3: DetailGroup[] = [];        // 第三层: 逐项明细(界面默认收起)
  @State fullDetailOpen: boolean = false;   // 第三层展开开关(默认 false = 收起)
  @State fullCondText: string = '';         // 取得条件来源一行
  // 单次完整跑分的结果浮层也用同一套三层结构
  @State resL1: ScoreCard[] = [];
  @State resL2: KeyRow[] = [];
  @State resCondText: string = '';
  // 首页「单项测试」次要入口: 默认收起(一键全部跑分已经包含它们)
  @State showSolo: boolean = false;
  // ---- 两份报告对比(compareReports, 只读): 选中文件 / 对比结果 / 状态 ----
  //  文件全文(几十~几百 KB)放在 CmpFile 里, 只在用户按下按钮时才读; @State 只做引用变化通知。
  @State cmpOpen: boolean = false;
  @State cmpA: CmpFile | null = null;
  @State cmpB: CmpFile | null = null;
  @State cmpOut: CmpOutcome | null = null;
  @State cmpStatus: string = '';
  @State cmpBusy: boolean = false;
  @State cmpLocal: CmpFile[] = [];
  @State cmpCopied: boolean = false;
  @State cmpSocA: string = '';        // 可选的显式 SoC(留空 = 让 native 自己从报告里判)
  @State cmpSocB: string = '';
  // ---- 「下载」目录落盘: 用户可见的成功/失败路径(不静默失败) ----
  @State dlPath: string = '';               // 已经写进去的「下载」目录路径
  @State dlStatus: string = '';             // 一行用户可见的说明
  @State dlFiles: string[] = [];            // 该目录下最近写成功的文件名
  // 历史成绩.txt(同目录下的纯文本成绩单)本轮的写入结果 —— 独立一行显示, 不埋进 dlStatus 里:
  // 它和报告是两份文件, 一份写成功一份写失败时必须看得出来是哪一份。
  @State dlHistoryStatus: string = '';
  // ---- 「历史」页 ----
  // histRows: 账本行的只读视图(与「历史成绩.txt」同一批行, 时间倒序)。
  // 只在切到历史页 / 整轮跑完之后读一次盘; 跑分期间不读、不重绘。
  @State histRows: RunHistoryRow[] = [];
  // 点某一条 -> 打开那一次的报告(只读全文预览)
  @State histOpen: boolean = false;
  @State histRepTitle: string = '';
  @State histRepText: string = '';
  @State histRepNote: string = '';
  // 历史二级界面(点开一条记录)要看的是**那一行的数**, 不是一屏文本:
  //   histRepRow 存整行(分数从它里面取, 与列表卡片同一套排版零件);
  //   histRepOpen = 原始报告全文是否展开 —— **默认收起**, 这是用户说的"密密麻麻"的根治办法。
  @State histRepRow: RunHistoryRow | null = null;
  @State histRepOpen: boolean = false;
  // 非 @State: 正在跑的编排器(停止按钮要拿到它); 不参与渲染
  private fullSession: FullRunSession | null = null;
  // 非 @State: 只作为循环控制标志, 不需要触发 UI 刷新
  private scStopRequested: boolean = false;
  private scCrashItem: string = '';
  private scCrashTs: number = 0;
  private scCrashPending: boolean = false;
  // native 现场全文(磁盘上的记录读完即清, 全文留在内存里供"复制全文")
  private nCrashFull: string = '';
  private nPreviewMax: number = 1800;
  // 页面 context(导出文件用的文档选择器需要它; 存一份避免再引入已废弃的 getContext)
  private appCtx: common.UIAbilityContext | null = null;
  // 上一次灌给 RunLog 的窗口签名(形态@宽x高): 只有它变了才重新拼设备串, 避免每帧重算
  private runLogWinTag: string = '';
  // 上一次真正灌给 RunLog 的设备串原文。为什么也要比它: 设备串里除了窗口尺寸, 还有
  // CPU 拓扑 / 芯片判读 / 可用核集合那几段, 它们都是在 aboutToAppear 之后由 native 依次
  // 上报的 —— 只看窗口签名的话, 那些段永远灌不进去(真机 runlog 的设备行里
  // cpuTopology / chip 段一直是空的, 正是这个原因)。
  private runLogDevice: string = '';
  // 窗口尺寸探测的重试次数(getWindowProperties 在窗口完成布局前会返回 0x0)
  private winProbeTries: number = 0;
  // 窗口对象(启动时拿到的那一个): 用于"跑分前再主动问一次窗口尺寸"(见 ensureWindowKnown)。
  // 只读不写, 不参与任何 @State 渲染。
  private winObj: window.Window | null = null;

  private mqSm: mediaquery.MediaQueryListener = mediaquery.matchMediaSync('(width<600vp)');
  private mqMd: mediaquery.MediaQueryListener =
    mediaquery.matchMediaSync('(600vp<=width<840vp)');
  private mqLg: mediaquery.MediaQueryListener = mediaquery.matchMediaSync('(width>=840vp)');
  private onMqSm: (r: mediaquery.MediaQueryResult) => void = (r: mediaquery.MediaQueryResult) => {
    this.applyBp(r);
  };
  private onMqMd: (r: mediaquery.MediaQueryResult) => void = (r: mediaquery.MediaQueryResult) => {
    this.applyBp(r);
  };
  private onMqLg: (r: mediaquery.MediaQueryResult) => void = (r: mediaquery.MediaQueryResult) => {
    this.applyBp(r);
  };
  // 真实窗口尺寸变化(旋转 / 折叠 / 分屏 / PC 拖拽缩放)。PC 上拖拽时这个回调会很密,
  // 所以先做一次"尺寸没变就直接返回"的短路: 不产生任何 @State 赋值, 也就不会重排。
  //
  //  单位 bug 的根因与修法(2026-10-08)
  //   官方 API 参考写明: window.Size(windowSizeChange 回调的参数)的 width/height 单位是 px
  //   ("Size 窗口大小, 单位为px"; API 23 起另给了 SizeInVP —— 正说明 Size 本身是 px)。
  //   本回调以前把 px 当 vp 直接用了: 真机回调给 1828x2584(px) 于是设备行写成
  //   "窗口=1828x2584vp", 形态判定只看宽高比(0.707)就判成"折叠屏·展开·竖长内屏";
  //   (补一句免得又被误读: 0.707 = 1/sqrt(2) —— 这台机器展开态就是竖长内屏,
  //    这是对的; 错的只是单位。别再把"阔折叠内屏 = 16:10 横向"这条已被证伪的假设
  //    套回来, 那是 Pura X(非 Max)的面板数据, 见 common/LayoutPlan.ets 文件头的更正。)
  //   而同一台机器的 getWindowProperties 那条路是"px ÷ 屏幕密度"算的, 两条来源口径不一致 ——
  //   哪一个先到就按哪一个, 于是同一个窗口能给出两种尺寸与两种形态。
  //   修法: 回调值先按 px 换算成 vp(与另一条来源同一个口子 DeviceProfile.applyWindowPx),
  //   再用 mediaquery 三档(按真实 vp 宽判定的独立旁证)交叉验证; 只有验证通过(或原始值
  //   本身就落在档里, 说明它已经是 vp)才写进设备画像与形态判定, 否则只记录原因、不改状态。
  private onWinSize: (s: window.Size) => void = (s: window.Size) => {
    const rawW: number = s.width > 0 ? Math.round(s.width) : 0;
    const rawH: number = s.height > 0 ? Math.round(s.height) : 0;
    if (!(rawW > 0) || !(rawH > 0)) {
      DeviceProfile.get().setWindowProbe('windowSizeChange 回调给了 ' + rawW.toString() + 'x' +
        rawH.toString() + '(不是有效尺寸), 已忽略 —— 窗口尺寸仍以 getWindowProperties 那条路为准',
        '', '');
      return;
    }
    const conv: WindowSizeVp = DeviceProfile.get().applyWindowPx(rawW, rawH);
    if (!conv.ok) {
      // 换算不了(读不到屏幕密度等): 不把 px 当 vp 往下走, 只写下原因;
      // 形态判定继续用上一次已验证过的尺寸(没有就仍是兜底布局)。
      DeviceProfile.get().setWindowProbe(
        'windowSizeChange 回调给了 ' + rawW.toString() + 'x' + rawH.toString() + 'px, 但' +
        conv.why + ' —— 该值未写进设备画像与形态判定(不许把 px 当 vp 用)', '', '');
      this.syncProfile();
      return;
    }
    const convOk: boolean = this.windowUnitsConsistent(conv.widthVp);
    const rawOk: boolean = this.windowUnitsConsistent(rawW);
    if (!convOk && !rawOk) {
      DeviceProfile.get().setWindowProbe(
        'windowSizeChange 回调原始值 ' + rawW.toString() + 'x' + rawH.toString() + 'px; ' +
        conv.basis + '; 这两个宽度与 mediaquery 断点档(' + this.windowUnitsNote() +
        ')都不一致 —— 单位口径存疑, 该尺寸未写进设备画像与形态判定(宁可先用兜底布局, ' +
        '也不拿一个可疑的宽度去改形态判定)', '', '');
      this.syncProfile();
      return;
    }
    // convOk = "除以密度"的结果与断点档一致(首选); 否则说明回调给的这个数本身就是 vp
    // (密度口径与我们的假设不同), 这时按原始值当 vp 用, 并把这一步判断原样写进依据里。
    const asVp: boolean = !convOk;
    const w: number = asVp ? rawW : conv.widthVp;
    const h: number = asVp ? rawH : conv.heightVp;
    const basis: string = asVp
      ? (rawW.toString() + 'x' + rawH.toString() + '(回调原始值) 直接当 vp 采用: 除以密度后的 ' +
        conv.basis + ' 与 mediaquery ' + this.windowUnitsNote() +
        ' 档不一致, 而原始值一致 —— 按"回调值本身已是 vp"处理')
      : (conv.basis + ' · 与 mediaquery ' + this.windowUnitsNote() + ' 档相容(容差判据 ' +
        this.windowUnitsTolerance() + ')');
    const src: string = asVp ? 'windowSizeChange(回调值即 vp)' : 'windowSizeChange(px→vp)';
    DeviceProfile.get().setWindowProbe(
      '窗口尺寸来自 windowSizeChange 回调: 原始 ' + rawW.toString() + 'x' + rawH.toString() +
      'px → ' + w.toString() + 'x' + h.toString() + 'vp（' + basis + '）', src, basis);
    DeviceProfile.get().commitWindowVp(w, h, src, basis);
    if (w === this.winW && h === this.winH) {
      return;
    }
    this.winW = w;
    this.winH = h;
    this.refreshBp();
    // 窗口尺寸变了(首次布局 / 旋转 / 折叠 / 分屏): 系统栏 insets 可能跟着变, 重读一次
    this.refreshInsets();
  };
  // 系统栏(状态栏 / 导航条)区域变化: 旋转、折叠、手势条显隐、分屏都会来一次。
  // 事件驱动, 不轮询; 只有 insets 真的变了才会写 @State(见 refreshInsets 的夹取)。
  private onAvoidArea: (o: window.AvoidAreaOptions) => void = (o: window.AvoidAreaOptions) => {
    this.refreshInsets();
  };

  aboutToDisappear(): void {
    // 粒子层: 页面销毁时把 tick 与画布一起收干净(不留任何 setInterval 在后台)
    this.fxStop();
    DeviceProfile.get().unwatchFold();
    DeviceProfile.get().setOnChange(null);
    this.mqSm.off('change', this.onMqSm);
    this.mqMd.off('change', this.onMqMd);
    this.mqLg.off('change', this.onMqLg);
    window.getLastWindow(getContext(this) as common.UIAbilityContext).then((w: window.Window) => {
      w.off('windowSizeChange', this.onWinSize);
      w.off('avoidAreaChange', this.onAvoidArea);
    }).catch((e: Error) => {
      console.warn('[AB] off windowSizeChange failed ' + e.message);
    });
    thermal.unregisterThermalLevelCallback();
    this.keepScreen(false);
  }

  // UIAbilityContext 的现代取法(官方替代已废弃的 getContext(this))。
  // 为什么要换: getContext 在本 SDK 已废弃, 每一次调用都会多出一条 ArkTS 警告, 而本工程的硬
  // 约束是"不新增警告"(基线 11 条) —— 本次新增了 libauroraref.so 的 napi 导入(+1 警告),
  // 所以这里把 aboutToAppear 里那处 getContext 换成官方替代 API, 收支平衡在基线上。
  // 返回 null = 拿不到(理论上不该发生: 本页面由 UIAbility 承载); 调用方自己决定怎么办。
  // (另外三处 getContext 调用(窗口监听解绑 / 保持亮屏 / 窗口尺寸探测)保持原样: 它们都是
  //  "拿不到 context 就会掉功能"的关键路径, 不在本次改动范围内冒险。)
  private hostCtx(): common.UIAbilityContext | null {
    try {
      const c = this.getUIContext().getHostContext();
      if (c === undefined || c === null) {
        return null;
      }
      return c as common.UIAbilityContext;
    } catch (e) {
      return null;
    }
  }

  private keepScreen(on: boolean): void {
    // 装饰性视效护栏的第二道(冗余): 每一个负载开跑前都会先 keepScreen(true)、收工后
    // keepScreen(false), 与 @Watch('onRunFlagChange') 完全同刻 —— 两条路里任何一条
    // 单独成立, 都足以保证"跑分期间没有任何装饰性视效"。syncFx() 幂等, 重复调用只比一次布尔。
    this.syncFx();
    window.getLastWindow(getContext(this) as common.UIAbilityContext).then((w: window.Window) => {
      w.setWindowKeepScreenOn(on);
    }).catch((e: Error) => {
      console.warn('[AB] keepScreen failed ' + e.message);
    });
  }

  private thermalText(): string {
    const names: string[] = ['凉爽', '正常', '偏热', '过热', '高温降级', '警告', '紧急'];
    if (this.thermalLevel < 0 || this.thermalLevel > 6) {
      return '未知';
    }
    return names[this.thermalLevel];
  }

  // ---- 断点系统: 官方 @ohos.mediaquery(只作旁证与兜底) ----
  //
  //   形态判定不看断点: 它看的是实时窗口 vp 宽高 + 宽高比 + 折叠状态 + 设备类型,
  //    规则与参数表都在 ../common/LayoutPlan.ets, 关于页「布局自检」逐项显示并可复制。
  //   媒体查询回调从此不再把断点的"代表宽度"(360/720/1000)写进 winW ——
  //    那会和 windowSizeChange 互相覆盖, 在 PC 上拖窗口时能看到布局来回跳。
  private setBp(name: string): void {
    if (name === this.bp) {
      return;
    }
    this.bp = name;
    this.syncProfile();
  }

  // 把「实时窗口尺寸 + 断点」交给设备画像, 并把形态计划同步到 @State 触发重排。
  // 窗口变化 / 折叠状态变化 / 切页时调用。开销很小: 输入没变时 DeviceProfile 一个字段都不动,
  // 下面的 @State 赋值是同一个值, ArkUI 不会产生任何重绘。
  private syncProfile(): void {
    const profile: DeviceProfile = DeviceProfile.get();
    profile.applyWindow(this.winW, this.winH, this.bp);
    const d: DeviceProfileData = profile.data();
    const p: LayoutPlan = d.plan;
    this.planId = p.id;
    this.profileColumns = p.columns;
    this.profileSplit = p.split ? 1 : 0;
    this.spanLeft = p.leftSpan;
    this.spanRight = p.rightSpan;
    this.uiGutterVp = p.gutter;
    this.uiPadVp = p.pad;
    this.uiRadiusVp = p.radius;
    this.uiFontScale = p.fontScale;
    this.uiBtnH = p.btnH;
    this.uiRes = p.result;
    this.startDia = p.heroDia;
    // 实测方向变化计数(只在"横 -> 竖"或"竖 -> 横"真的发生时 +1; 同方向重复回调不算)
    if (this.winW > 0 && this.winH > 0) {
      const portraitNow: number = this.winH >= this.winW ? 1 : 0;
      if (this.rotLastPortrait >= 0 && portraitNow !== this.rotLastPortrait) {
        this.rotFlips = this.rotFlips + 1;
      }
      this.rotLastPortrait = portraitNow;
    }
    // 纯几何事实(自检块照实显示; 布局参数就是从这几个数里算出来的)
    this.geoW = p.geometry.widthVp;
    this.geoH = p.geometry.heightVp;
    this.geoShort = Math.round(p.geometry.shortSideVp);
    this.geoLong = Math.round(p.geometry.longSideVp);
    this.geoContent = Math.round(p.geometry.contentVp);
    this.geoZone = Math.round(p.geometry.resultZoneVp);
    this.geoLeft = Math.round(p.geometry.leftColVp);
    this.geoCap = p.geometry.columnCap;
    this.devName = d.marketingName;
    this.devModelLine = profile.modelLine();
    this.devFoldTag = profile.foldTag();
    this.devMatchMethod = profile.matchMethodLine();
    this.devTableScreen = profile.tableScreenLine();
    this.devTableSource = profile.tableSourceLine();
    this.devTableHit = d.tableHit;
    this.devCpuInfo = d.lastCpuInfo;
    this.devChipMeasured = d.chipMeasured;
    this.devChipPerCore = d.chipPerCore;
    this.devChipInfer = d.chipInfer;
    // 「布局自检」的几个即时字段: 任何页面切过来都能一眼看到"算成了哪个形态"
    this.devShapeLine = p.id + ' · 编号 ' + p.index.toString() + ' · ' + p.name;
    this.devShapeBasis = p.basis;
    this.devShapeNote = p.note;
    this.devShapeDegrade = p.degrade;
    this.devSelfCheck = d.layoutSelfCheck;
    // 「本机性能天花板」: 设备级事实, 与窗口/形态无关; 每次同步都重取一次(native 侧是纯只读
    // 快照 + 一次采样结果的读取, 没有缓存不一致的问题)。跑完一轮之后也会再取一次(见收尾处)。
    this.ceilingLine = BenchRunner.ceiling();
    // 竖向空间预算: 放不下时把顶部「设备」卡切成紧凑版(内容不隐藏, 只是收起来)
    this.tightY = p.height.topInfoCompressed;
    this.devHeightBudget = p.height.text;
    // 「设备行」里带窗口尺寸与形态(见 BenchRunner.deviceLabel / windowTagOf):
    // 窗口尺寸比启动晚很多才知道, 所以这里在窗口或形态变化时把设备串重新灌给 RunLog,
    // 否则 runlog 里会永远停在"窗口=（尚未取到）"。
    const winTag: string = p.id + '@' + this.winW.toString() + 'x' + this.winH.toString();
    // 重灌的两个条件(任一成立就重灌):
    //   ① 窗口签名变了(形态 / 宽 / 高) —— 窗口尺寸比启动晚才知道;
    //   ② 设备串原文变了 —— CPU 拓扑 / 芯片判读 / 可用核集合 / 窗口来源标注都是在启动之后
    //      由 native 依次上报的, 只看①的话它们永远进不了 runlog 的设备行。
    // 开销: 一次字符串拼接 + 一次比较; 内容没变时一个字段都不写(与原来的行为一致)。
    const label: string = BenchRunner.deviceLabel();
    if (winTag !== this.runLogWinTag || label !== this.runLogDevice) {
      this.runLogWinTag = winTag;
      this.runLogDevice = label;
      this.device = label;
      RunLog.get().setContext(this.device, this.nativeVer);
    }
    // 设备画像全文只有「关于」页要用(几千字符的字符串拼接), 别的页面不重建
    if (this.tab === 2) {
      this.devProfileText = profile.describe();
    }
  }

  // 折叠状态变了(展开 / 折叠 / 半折叠): 重算形态与布局并重排
  private onProfileChanged(): void {
    this.syncProfile();
    console.info('[AB] fold state changed → ' + DeviceProfile.get().foldTag());
  }

  // 媒体查询回调: 只更新"断点"这一档, 不碰窗口尺寸
  private applyBp(r: mediaquery.MediaQueryResult): void {
    if (!r.matches) {
      return;
    }
    const name: string = r.media as string;
    if (name === '(width<600vp)') {
      this.setBp('sm');
    } else if (name === '(600vp<=width<840vp)') {
      this.setBp('md');
    } else {
      this.setBp('lg');
    }
  }

  // 用真实窗口宽度定断点(windowSizeChange / 启动时各调一次)
  private refreshBp(): void {
    let name: string = 'sm';
    if (this.winW >= BP_LG) {
      name = 'lg';
    } else if (this.winW >= BP_MD) {
      name = 'md';
    }
    this.bp = name;
    this.syncProfile();
  }

  // 是否采用"左操作 / 右结果"左右分栏。
  //   分流与否只由形态计划给 —— 而计划里那个 split 是由实测窗口宽与两栏可读下限算出来的,
  //    不是"设备型号说了算", 也不是"编号说了算"。同理, 栅格 span 也直接取计划的 leftSpan/rightSpan。
  private wide(): boolean {
    return this.profileSplit === 1;
  }

  // ---- 自适应尺寸: 全部由形态计划给, 不再按断点写死 ----
  private startInnerDia(): number {
    return Math.max(72, this.startDia - 20);
  }

  // 圆环容器(= 那个 Stack)的边长。
  //  必须 >= 外圈直径 —— 这里以前写的是 max(84, startDia - 8):
  //   300vp 的外圈被塞进 292vp 的盒子里, 圆环被自己的容器切掉(真机上看到的就是
  //   "黑块上下各露出一段断开的蓝弧"); 更糟的是 Flex 按 292vp 给这一项排版,
  //   圆却画到 300vp —— 多出来的 8vp 直接压在右边那列说明文字上(字形重影)。
  //   现在两个值来自同一个数(startDia = 形态计划给的 heroDia), 容器与内容不可能再打架;
  //    内圈仍是 startDia - 20, 它的描边(2vp)也在外圈之内。
  //  (静态检查: ref/layout_cases.test.mjs 的 P 节会断言这里不许再出现比 startDia 小的容器)
  private startRingDia(): number {
    return this.startDia;
  }

  private startMainFont(): number {
    return Math.max(18, Math.min(30, Math.round(this.startDia * 0.155)));
  }

  private startSubFont(): number {
    return Math.max(12, Math.min(22, Math.round(this.startDia * 0.107)));
  }

  // 字号缩放: 形态计划给的系数(平板 / 电脑观看距离更远 -> 略放大; 极窄分屏 -> 略缩小)。
  // 纯计算, 不带任何状态。
  private fz(base: number): number {
    return scaledFont(base, this.uiFontScale);
  }

  // 按钮最小高度: 由形态计划给(平板 / 电脑 48vp, 极扁窗口 34vp)
  private btnH(): number {
    return this.uiBtnH;
  }

  private cardPad(): number {
    return this.uiPadVp;
  }

  private cardRadius(): number {
    return this.uiRadiusVp;
  }

  private rowGap(): number {
    return this.uiGutterVp;
  }

  // 内容列最大宽度: 形态计划给 0 = 不限(满宽), 这里换成一个足够大的数给 constraintSize
  private contentMax(): number {
    const v: number = DeviceProfile.get().contentMaxVp();
    return v > 0 ? v : 100000;
  }

  // 普通卡片宽度: 单栏 92%(左右各留一点边), 分栏 100%(宽度交给外层 GridCol)
  private cardW(): string {
    return this.wide() ? '100%' : '92%';
  }

  // 结果卡 flex 基准宽度与最小宽度: 放不下就自动换成更少的列, 永不横向溢出
  private cardBasis(): string {
    return DeviceProfile.get().cardBasis();
  }

  private minCardW(): number {
    return DeviceProfile.get().minCardW();
  }

  // 逐项明细区的列数(结果页第三层): 大屏真的并排更多列 —— 而不是把小屏的单列排法
  // 整体放大。列数与"每列最小宽"同样由 LayoutPlan 的结果页密度按实测窗口宽给出;
  // 放不下时 Flex 会自动退成更少列(基准宽 + 最小宽两条一起兜底, 永不横向溢出)。
  private detailBasis(): string {
    const c: number = this.uiRes.detailCols;
    if (c >= 3) {
      return '31%';
    }
    return c === 2 ? '48%' : '100%';
  }

  private detailMinW(): number {
    return this.uiRes.detailCols >= 2 ? 300 : 0;
  }

  // 第一层「三个大数字」的 flex 基准宽与最小宽
  //   这里不再借用首页「结果卡」的列数(cardBasis/minCardW): 结果页第一层是在结果页自己的
  //    可用宽度里排三张卡, 与首页结果区的列数是两码事。2026-10 的真机缺陷就是这么来的 ——
  //    竖屏手机上首页结果卡是 1 列, 于是三张大数字卡的基准宽被算成 100%, 三张卡各占一行,
  //    再加上卡上挂着的 layoutWeight(1), 每张卡被拉伸成一个吃满屏的巨大空块。
  //    现在这两个数由 common/LayoutPlan.ets 的 resultDecoFor 按"结果页自己的可用宽度"算好
  //    (ResultDeco.l1Basis / l1MinW), 界面直接照抄, 与首屏高度估算同源。
  private l1Basis(): string {
    return this.uiRes.l1Basis;
  }

  private l1MinW(): number {
    return this.uiRes.l1MinW;
  }

  // 跑分写汇总行之前确保窗口尺寸已经取到: 这是"跑一次 runlog 里就必须有窗口尺寸"的最后一道保险。
  // 为什么放在这里: ① 跑分时窗口一定已经布局完成(启动期那 4 次重试若都太早, 这里必然成功);
  //   ② getWindowProperties() 是同步接口, 它写进 DeviceProfile 之后, 紧接着的
  //   RunLog.summary() 拿到的就是带窗口尺寸的设备串 —— 不需要等任何异步回调。
  private ensureWindowKnown(): void {
    if (this.winObj === null) {
      return;
    }
    const d: DeviceProfileData = DeviceProfile.get().data();
    if (d.windowWidthVp > 0 && d.windowHeightVp > 0) {
      return;   // 已经有了, 不重复问
    }
    this.probeWindowTry(this.winObj, 1);
  }

  // 主动问一次窗口尺寸(px -> vp), 并把它交给 syncProfile 重灌设备行。
  // 重试: getWindowProperties 在窗口完成首帧布局之前会返回 0x0(不是错误, 只是还没布局),
  // 所以最多问 4 次(每次间隔 500ms), 每次都会把"当前为什么还没拿到"写进 windowProbe。
  private probeWindowSize(w: window.Window): void {
    this.probeWindowTry(w, 1);
  }

  // px->vp 换算口径的交叉验证(纯读 mediaquery, 不产生任何副作用)。
  //   mediaquery 的三档 (width<600vp) / (600vp<=width<840vp) / (width>=840vp) 是按**真实 vp
  //   宽度**判定的; 我们算出来的 vp 宽必须落在同一档里。三档都没结果时无法验证 -> 放行(不阻断)。
  private windowUnitsConsistent(wVp: number): boolean {
    const sm: boolean = this.mqSm.matches;
    const md: boolean = this.mqMd.matches;
    const lg: boolean = this.mqLg.matches;
    if (sm === md && md === lg) {
      return true;   // 全 false(还没结果) 或全 true(异常) -> 不做判断
    }
    if (sm) {
      return wVp > 0 && wVp < 900;    // 真机 vp 宽 < 600, 留一档容差
    }
    if (md) {
      return wVp >= 400 && wVp < 1200;
    }
    return wVp >= 600;                // lg: 真实 vp 宽 >= 840
  }

  // 上面那个交叉验证命中的到底是哪一档(写进设备行/报告, 让"验证通过"这句话可复核)
  private windowUnitsNote(): string {
    const sm: boolean = this.mqSm.matches;
    const md: boolean = this.mqMd.matches;
    const lg: boolean = this.mqLg.matches;
    if (sm && !md && !lg) {
      return 'sm(<600vp)';
    }
    if (md && !sm && !lg) {
      return 'md(600~840vp)';
    }
    if (lg && !sm && !md) {
      return 'lg(>=840vp)';
    }
    return '三档都还没结果/异常(无法交叉验证, 已按放行处理)';
  }

  // 交叉验证用的容差判据原文(windowUnitsConsistent 里那三个区间)。
  // 为什么要把它写进依据: "与断点档一致"这句话必须可复核 —— mediaquery 的档是
  // 600/840vp 的硬边界, 而这里判定时留了一档容差(目的是抓"差一个密度倍数"这种量级错误,
  // 不是做精确匹配), 那就把区间原文写出来, 谁都能算一遍。
  private windowUnitsTolerance(): string {
    const sm: boolean = this.mqSm.matches;
    const md: boolean = this.mqMd.matches;
    const lg: boolean = this.mqLg.matches;
    if (sm && !md && !lg) {
      return '0 < 宽 < 900vp';
    }
    if (md && !sm && !lg) {
      return '400 <= 宽 < 1200vp';
    }
    if (lg && !sm && !md) {
      return '宽 >= 600vp';
    }
    return '不判定(三档都没有结果)';
  }

  private probeWindowTry(w: window.Window, tries: number): void {
    if (tries > 4) {
      this.syncProfile();   // 最终失败: windowProbe 里留着最后一次的原因
      return;
    }
    // getWindowProperties() 在本 SDK 里是同步接口(返回 WindowProperties, 不是 Promise),
    // 失败时抛 BusinessError(1300002 窗口状态异常 / 801 能力不支持)。属性是快照, 不会自动刷新,
    // 所以窗口刚创建时可能读到 0x0(还没布局) —— 那种情况不是错误, 隔一会儿再问一次。
    let rectW: number = 0;
    let rectH: number = 0;
    let threw: string = '';
    try {
      const props: window.WindowProperties = w.getWindowProperties();
      const rect: window.Rect = props.windowRect;
      rectW = rect.width;
      rectH = rect.height;
    } catch (e) {
      const err: BusinessError = e as BusinessError;
      const code: number = (err.code !== undefined) ? err.code : 0;
      const msg: string = (err.message !== undefined) ? err.message : '';
      threw = 'getWindowProperties 抛异常 code=' + code.toString() +
        (msg.length > 0 ? (', message=' + msg) : '');
    }
    if (threw.length > 0) {
      DeviceProfile.get().setWindowProbe(
        threw + '（窗口状态异常或能力不支持; 仍依赖 windowSizeChange 回调）', '', '');
      this.syncProfile();
      return;
    }
    const res: WindowSizeVp = DeviceProfile.get().applyWindowPx(rectW, rectH);
    if (res.ok) {
      // 单位口径的交叉验证: mediaquery 的三个监听器是按真实 vp 宽度判定的, 与我们按
      // "windowRect 是 px、再除以屏幕密度"算出来的 vp 宽应当落在同一档。不一致说明
      // windowRect 的单位不是 px(那样除以密度就会小好几倍) —— 这种情况**只记录尺寸、
      // 不改布局状态**, 并在原因里写明, 免得把一个错口径的宽度喂进形态判定。
      //   与 windowSizeChange 那条路完全同一套判据(见 onWinSize): 首选"除以密度"的结果;
      //   它不过关而原始值过关 -> 原始值按 vp 采用; 两个都不过关 -> 谁都不用, 只记原因。
      const convOk: boolean = this.windowUnitsConsistent(res.widthVp);
      const rawOk: boolean = this.windowUnitsConsistent(rectW);
      const sizeText: string = '窗口尺寸来自 getWindowProperties: 原始 ' +
        Math.round(rectW).toString() + 'x' + Math.round(rectH).toString() + 'px; ' + res.basis;
      if (convOk) {
        // 验证通过: 落库(设备画像 + 形态/布局计划)并按真实窗口宽重算断点
        const basis: string = res.basis + ' · 与 mediaquery ' + this.windowUnitsNote() +
          ' 档相容(容差判据 ' + this.windowUnitsTolerance() + ')';
        DeviceProfile.get().commitWindowVp(res.widthVp, res.heightVp, 'getWindowProperties(px→vp)', basis);
        DeviceProfile.get().setWindowProbe(sizeText + ' · 与 mediaquery ' +
          this.windowUnitsNote() + ' 档相容(容差判据 ' + this.windowUnitsTolerance() + ')',
          'getWindowProperties(px→vp)', basis);
        this.winW = res.widthVp;
        this.winH = res.heightVp;
        this.refreshBp();
      } else if (rawOk) {
        // 除以密度后不过关、原始值过关: 说明 windowRect 这个口径下"原始值本身就是 vp"
        // (密度与我们的假设不同)。按原始值当 vp 采用, 并把这一步判断原样写进依据。
        const basis: string = Math.round(rectW).toString() + 'x' + Math.round(rectH).toString() +
          '(原始值) 直接当 vp 采用: 除以密度后的 ' + res.basis + ' 与 mediaquery ' +
          this.windowUnitsNote() + ' 档不一致, 而原始值一致';
        DeviceProfile.get().commitWindowVp(Math.round(rectW), Math.round(rectH),
          'getWindowProperties(原始值即 vp)', basis);
        DeviceProfile.get().setWindowProbe(sizeText + ' · ' + basis,
          'getWindowProperties(原始值即 vp)', basis);
        this.winW = Math.round(rectW);
        this.winH = Math.round(rectH);
        this.refreshBp();
      } else {
        // 单位口径存疑: 不落库(既不改设备画像的窗口尺寸, 也不改形态/布局),
        // 但把量到的 px 与按密度换算出的 vp 都写进原因里 —— runlog 的设备行会带上这一段,
        // 用户一眼就能看出是"px 还是 vp"这个口径问题, 而不是又一句"尚未取到"。
        DeviceProfile.get().setWindowProbe(
          sizeText + '(注意: 与 mediaquery 的断点档(' + this.windowUnitsNote() +
          ')不一致, 单位口径待核 —— 该尺寸未写进设备画像与布局, 以免错口径改掉形态判定)', '', '');
        this.syncProfile();
      }
      return;
    }
    // 还没布局 / 换算不了: 写清原因, 隔一会儿再问一次(最多 4 次)
    DeviceProfile.get().setWindowProbe(
      'getWindowProperties 第 ' + tries.toString() + ' 次: ' + res.why +
      (tries < 4 ? '（稍后重试）' : '（已停止重试, 仍依赖 windowSizeChange 回调）'), '', '');
    if (tries < 4) {
      setTimeout(() => {
        this.probeWindowTry(w, tries + 1);
      }, 500);
    } else {
      this.syncProfile();
    }
  }

  // ---- 旋转策略回读(纯读; 本项目从不调用 setPreferredOrientation 去改方向) ----
  //  回答的是用户那句"系统开了旋转锁定, 为什么在这个 App 里还能转":
  //   ① 声明值: bundleManager 回读本 ability 的 orientation —— 这是"当前旋转策略"的权威来源,
  //      与 module.json5 同源(不是把字符串再抄一遍: 抄错了这里立刻就能看出来并报警)。
  //   ② 运行时首选方向: window.getPreferredOrientation() —— 只有被 setPreferredOrientation
  //      动过才会与声明值不同。
  //   ③ 当前屏幕旋转角: display.getDefaultDisplaySync().rotation。
  //  任何一步失败都只写原因, 不抛到界面上、不影响任何跑分流程。
  private probeRotationPolicy(w: window.Window | null): void {
    let idx: number = -1;
    try {
      const flags: number = bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_HAP_MODULE |
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_ABILITY;
      const info: bundleManager.BundleInfo = bundleManager.getBundleInfoForSelfSync(flags);
      for (const m of info.hapModulesInfo) {
        for (const ab of m.abilitiesInfo) {
          if (ab.name === 'EntryAbility') {
            idx = ab.orientation as number;
          }
        }
      }
    } catch (e) {
      this.rotNote = 'bundleManager 回读声明值失败：' + (e as Error).message;
    }
    if (idx >= 0 && idx < ROT_ENUM_NAMES.length) {
      const name: string = ROT_ENUM_NAMES[idx];
      const follows: boolean = ROT_ENUM_FOLLOWS_SENSOR[idx];
      const locks: boolean = ROT_ENUM_LOCKS[idx];
      this.rotDeclaredRaw = name + '（DisplayOrientation = ' + idx.toString() + '）';
      this.rotRespect = follows ? (locks ? 1 : 0) : 1;
      if (!follows) {
        this.rotDeclared = name + '：固定方向，不跟随传感器（与旋转锁定无关）';
      } else if (locks) {
        this.rotDeclared = name + '：跟随传感器自动旋转，受系统旋转锁定控制';
      } else {
        this.rotDeclared = name + '：跟随传感器自动旋转，无视系统旋转锁定';
      }
      if (idx !== ROT_DECLARED_ENUM_INDEX) {
        this.rotNote = (this.rotNote.length > 0 ? (this.rotNote + '；') : '') +
          '⚠ 回读到的声明值(' + name + ')与源码里写的 "' + ROT_DECLARED_STRING +
          '" 不一致 —— 以回读值为准，请核对 module.json5';
      }
    } else if (idx < 0 && this.rotNote.length === 0) {
      this.rotNote = '声明值没读到（bundleManager 没给出 EntryAbility 的 orientation）';
    }
    if (w !== null) {
      try {
        const o: number = w.getPreferredOrientation() as number;
        const nm: string = (o >= 0 && o < WIN_ORIENT_NAMES.length)
          ? WIN_ORIENT_NAMES[o] : ('未知(' + o.toString() + ')');
        this.rotEffective = nm + '（window.Orientation = ' + o.toString() + '）' +
          (o === 0 ? ' —— 未显式设置过首选方向，方向由上面的声明值决定' : '');
      } catch (e) {
        this.rotEffective = '读取失败：' + (e as Error).message;
      }
    }
    try {
      const d: display.Display = display.getDefaultDisplaySync();
      const rr: number = d.rotation;
      this.rotScreen = (rr >= 0 && rr < ROT_SCREEN_DEG.length)
        ? ROT_SCREEN_DEG[rr] : ('未知(' + rr.toString() + ')');
    } catch (e) {
      this.rotScreen = '读取失败：' + (e as Error).message;
    }
  }

  aboutToAppear(): void {
    // 设备画像先起来(带缓存): 布局参数、设备卡、RunLog 的设备串都读它。
    // 折叠状态变化时它回调 onProfileChanged(), 界面随之重排。
    DeviceProfile.get().setOnChange(() => {
      this.onProfileChanged();
    });
    DeviceProfile.get().init();
    this.syncProfile();
    this.mqSm.on('change', this.onMqSm);
    this.mqMd.on('change', this.onMqMd);
    this.mqLg.on('change', this.onMqLg);
    window.getLastWindow(getContext(this) as common.UIAbilityContext).then((w: window.Window) => {
      this.winObj = w;   // 留给 ensureWindowKnown(跑分前再问一次)
      w.on('windowSizeChange', this.onWinSize);
      // 沉浸式: 系统栏 insets(状态栏 / 导航条)一次读全, 之后只在变化事件里更新
      w.on('avoidAreaChange', this.onAvoidArea);
      // 先读一次; EntryAbility 的 setWindowLayoutFullScreen 是异步的, 首帧可能还没生效,
      // 所以按既有做法做有界重试(最多 3 次 / 600ms 一次, 生效即停) —— 不是周期任务。
      this.probeInsets(1);
      // 主动探测一次窗口尺寸(2026-10-06)
      //   真机 305 行 runlog 里"窗口=（尚未取到）"一次都没填上 —— 因为窗口尺寸只有
      //   windowSizeChange 回调这一个来源, 而它在这台机器上一直没来。
      //   getWindowProperties() 不依赖任何回调, 这里问一次就把它喂给 DeviceProfile;
      //   拿不到时把原因写进 windowProbe, 由 runlog 的设备行原样带出去。
      this.probeWindowSize(w);
      // 旋转策略回读(只读): 与窗口尺寸同一次拿到窗口对象时问一次, 不额外起任何任务
      this.probeRotationPolicy(w);
    }).catch((e: Error) => {
      this.probeRotationPolicy(null);
      DeviceProfile.get().setWindowProbe('getLastWindow 失败: ' + e.message +
        '（窗口对象都拿不到, windowSizeChange 与 getWindowProperties 两条路都不可用）', '', '');
      this.syncProfile();   // 把原因同步进设备串(否则日志里只剩一句"尚未取到")
      console.warn('[AB] on windowSizeChange failed ' + e.message);
    });
    thermal.registerThermalLevelCallback((level: thermal.ThermalLevel) => {
      this.thermalLevel = level as number;
    });
    this.device = BenchRunner.deviceLabel();
    this.cores = BenchRunner.cores();
    this.nativeVer = BenchRunner.nativeVersion();
    // UIAbilityContext: 原先这里用 getContext(this)(已废弃, 每调用一次多一条 ArkTS 警告, 而工程
    // 的硬约束是"不新增警告"); 官方替代是 UIContext.getHostContext()。本页面由 UIAbility 承载,
    // 拿到的就是那个 ability context —— 页面上后面所有 init(BenchStore / RunLog / CrashGuard /
    // FullReportStore)都要用它, 所以这里按"必然拿得到"处理, 万一没拿到就打一条明确的日志。
    const hostMaybe = this.hostCtx();
    if (hostMaybe === null) {
      console.warn('[AB] getHostContext() 没有返回 context: 报告 / 历史记录 / runlog 的目录可能不可用');
    }
    const ctx: common.UIAbilityContext = hostMaybe as common.UIAbilityContext;
    // 导出文件(文档选择器)要用 context: 存一份, 免得别处再引一次已废弃的 getContext
    this.appCtx = ctx;
    // 「一键全部跑分」的报告落盘目录(files/report-*.txt|json); 顺手看一眼盘上有没有上一次的报告
    // (目录已经在 EntryAbility.onCreate 里就位; 这里再确认一次, 幂等)
    FullReportStore.get().init(ctx);
    this.fullHasOldReport = FullReportStore.get().hasLatestOnDisk();
    this.fullOldReportBytes = FullReportStore.get().latestTextBytes();
    // 运行日志: 把设备/版本串灌给 RunLog(它自己不 import native 模块, 免得再多一条 ArkTS 警告),
    // 并把「发送范围 / 上传开关 / 地址」这些持久化设置读回来。
    RunLog.get().setContext(this.device, this.nativeVer);
    // 历史成绩.txt 的沙箱侧状态(去重状态文件 + 兜底副本目录); 用户看到的那一份在「下载」目录。
    ScoreHistory.get().init(ctx);
    RunLog.get().init(ctx).then(() => {
      this.uploadUrl = LogUpload.get().url();
      this.uploadOn = LogUpload.get().enabled();
      this.uploadRange = LogUpload.get().range();
      this.uploadHistN = LogUpload.get().historyCount();
      this.refreshLogStats();
    }).catch((e: Error) => {
      console.warn('[AB] runlog init failed ' + e.message);
    });
    BenchStore.get().init(ctx).then(() => {
      BenchStore.get().load().then((list: BenchRecord[]) => {
        this.records = list;
        this.refreshBest();
      }).catch((e: Error) => {
        console.warn('[AB] load records failed ' + e.message);
      });
      // 超线程(SMT)开关: store 一就绪就把持久化的偏好读回来, 立刻作用到 native。
      // 顺序很重要 —— 必须在任何负载开跑之前完成(跑分按钮由用户点击, 这里提前很多)。
      this.initSmtPref();
    }).catch((e: Error) => {
      console.warn('[AB] store init failed ' + e.message);
      // store 起不来时也要把开关落到 native: 用默认值(开)显式设置一次, 保证状态可预期
      this.applySmt(true, false);
    });
    // native 取证装置: EntryAbility.onCreate 已经把 filesDir 交给它(setLogDir)并记进 CrashGuard;
    // 这里先兜底一次(万一 onCreate 没走到), 再立刻读回"上次运行有没有留下 native 现场"。
    this.refreshNativeGuard(ctx);
    this.readNativeReport();
    // 启动自检: preferences 里还留着面包屑 => 上次没跑完就异常结束了(native 崩溃/OOM 直接杀进程)
    // 稳定性压测的自检(initStress)排在 init 之后, 保证读压测会话时 store 一定已经就绪
    CrashGuard.get().init(ctx).then(() => {
      this.initCrashEvidence();
    }).catch((e: Error) => {
      console.warn('[AB] crash guard init failed ' + e.message);
    });
    this.refreshMem();
    // 装饰性视效开关初始化: 正常启动时一定不在跑分, 这里只是把它对齐一次
    this.syncFx();
  }

  // ---- 稳定性压测: 启动自检与恢复 ----
  private async initStress(): Promise<void> {
    try {
      const s: CampaignState | null = await StressSession.get().load();
      if (s === null) {
        // 没有进行中的会话: 只把上次结束后留下的报告文本取回来展示
        const text: string = await StressSession.get().storedReport();
        if (text.length > 0) {
          this.scReportText = text;
        }
        return;
      }
      this.scTarget = s.target > 0 ? s.target : 50;
      this.scDone = s.completed;
      this.scGateUsed = s.gateUsed;
      this.scGateDown = s.gateUnavailable;
      this.gb7Composite = StressSession.geomean(s.scores);
      this.scCrashRuns = s.crashes.length;
      if (s.running && !s.roundOpen) {
        // running 还在但那一轮已经收尾(轮次之间被系统回收/划掉): 不算崩溃, 但可以接着跑
        this.scInterrupted = false;
        this.scPaused = s.completed < s.target;
        this.scInterruptRound = s.completed + 1;
        this.scInterruptItem = '';
      } else if (s.running) {
        // 被判据: running = true 且有一轮正跑在半路 -> 上次压测被杀了。归因用"最后启动的轮次 + 项名"。
        this.scInterrupted = true;
        this.scInterruptRound = StressSession.get().interruptedRound(s);
        // 面包屑比会话状态更精确(它是每项开跑前写的), 用它之前先确认它属于这个压测会话:
        // 否则"压测中途退出 -> 又单跑了一次普通跑分"之后, 普通跑分的面包屑会被误算成压测崩溃项。
        const crumb: BenchCrumb | null = await CrashGuard.get().peek();
        let crumbItem: string = '';
        let crumbTs: number = s.lastTs;
        if (crumb !== null) {
          if (s.runId.length > 0 && crumb.runId === s.runId) {
            crumbItem = crumb.item;
            crumbTs = crumb.ts;
          }
        }
        this.scInterruptItem = crumbItem.length > 0 ? crumbItem : s.lastItem;
        if (this.scInterruptItem.length === 0) {
          this.scInterruptItem = '（未记录）';
        }
        // 崩溃归因先放在内存里: 等用户点"继续压测"或"结束并查看报告"时才写进压测会话,
        // 否则一次崩溃会被 initStress 反复登记(进程重启多次就多记几条)。
        this.scCrashItem = this.scInterruptItem;
        this.scCrashTs = crumbTs;
        this.scCrashPending = true;
      } else {
        this.scInterrupted = false;
        this.scPaused = false;
      }
    } catch (e) {
      console.warn('[AB] stress init failed');
    }
  }

  // 可用内存采样(@ohos.hidebug.getSystemMemInfo, 失败则 supported = false)
  private refreshMem(): void {
    const s: MemStatus = MemoryProbe.sample();
    this.memChecked = true;
    this.memSupported = s.supported;
    this.memAvailMb = s.availableMb;
    this.memTotalMb = s.totalMb;
    this.memLow = s.low;
    this.memNote = s.note;
    this.memAt = s.at;
  }

  // =========================================================================
  // 超线程(SMT)开关 —— 界面状态 + 持久化 + 线程数
  //
  // 口径(与 native / BenchRunner 严格一致):
  //   * 只影响多核阶段使用哪些逻辑核(线程数 / 大核簇掩码 / 池线程落点);
  //     不改任何负载的算法、工作量、metric 口径与计分公式; 单核阶段完全不受影响;
  //   * 关掉时不会把两个线程绑到同一个物理核(native 侧按物理核分派落点);
  //   * 读不到拓扑时: 线程数维持全部逻辑核, 界面注明"拓扑未知, SMT 开关无效"。
  // 默认开: 不拨动开关时与历史行为完全一致。
  // =========================================================================

  // 启动时: 读持久化偏好 -> 写进 native -> 刷拓扑快照(刷新后界面的「设备画像」行同步更新)
  private initSmtPref(): void {
    BenchStore.get().getText(SMT_ENABLED_KEY, '1').then((v: string) => {
      const on: boolean = v !== '0';   // 默认开: 任何非 '0' 的值都按开处理
      this.applySmt(on, false);
    }).catch((e: Error) => {
      console.warn('[AB] smt pref read failed');
      this.applySmt(true, false);
    });
  }

  // 把开关状态落到 native, 并刷新界面上的拓扑快照
  //   persist = true 时同时写回 preferences(用户拨动开关走这条)
  private applySmt(on: boolean, persist: boolean): void {
    const applied: boolean = BenchRunner.benchSetSmtEnabled(on);
    this.smtOn = applied ? on : true;   // native 不可用(或写失败)时按默认开显示
    const caps: SmtCapabilities | null = BenchRunner.benchCapabilities();
    if (caps === null) {
      this.smtNote = BenchRunner.benchSmtApiOk()
        ? '拓扑读取失败：native 未返回 CPU 拓扑, SMT 开关不生效（多核阶段维持全部逻辑核）'
        : 'native 接口不可用：SMT 开关不生效（多核阶段维持全部逻辑核）';
    } else if (!caps.known) {
      this.smtNote = '拓扑未知（已按 1:1 处理, SMT 开关无效）：读不到 thread_siblings_list, ' +
        '也读不到 core_id / physical_package_id，多核阶段仍用全部逻辑核';
    } else if (!caps.smtPossible) {
      this.smtNote = '本机 ' + caps.logical.toString() + ' 逻辑核 / ' + caps.physical.toString() +
        ' 物理核：未检测到 SMT，开关开与关没有差别（多核阶段线程数相同）';
    } else {
      this.smtNote = '';
    }
    if (persist) {
      BenchStore.get().putText(SMT_ENABLED_KEY, applied && on ? '1' : '0').catch((e: Error) => {
        console.warn('[AB] smt pref write failed');
      });
    }
    // 设备画像的可复制文本里也带上这一行(旁路; 设备画像本身不 import native);
    // 逐路径实测结果(每个候选文件在 App 域下读到了什么 + errno)一并带上 —— 这是可核对的事实
    const line: string = this.benchTopologyLine();
    let topo: string = line.startsWith('CPU 拓扑：') ? line.substring(5) : line;
    if (caps !== null && caps.probes.length > 0) {
      topo += ' · 逐路径: ' + caps.probes;
    }
    DeviceProfile.get().setCpuTopology(topo);
    // 芯片判读(逐核频率原始读数 + /proc/cpuinfo 的 MIDR + 第三方对照表判读): 与拓扑同一时机刷新。
    // 旁路: 不参与任何测试项的计算 / 顺序 / 计分。
    this.refreshChipIdentity();
    // 内核允许的核集合(2026-10-05): 与拓扑/芯片判读同一时机刷新
    // 顺序必须放在 refreshChipIdentity() 之后: 可用核集合那一屏要一并显示"全机逐核频率档",
    // 而那份数据来自芯片判读探测的快照。旁路: 只影响「设备画像」的显示与可复制全文,
    // 不改任何负载、线程数与计分。
    this.refreshCpuAffinity();
  }

  // 刷新"内核允许的核集合"并同步到界面状态。
  // 为什么单独一段: 这一份事实回答的是"内核到底允不允许我们用最快的那几个核" ——
  // 用户质疑"代际差异没体现"时, 那三行(可用核集合 / 快簇 / 交集)就是第一手答案。
  private refreshCpuAffinity(): void {
    const snap: CpuAffinitySnapshot | null = BenchRunner.benchRefreshCpuAffinity();
    if (snap === null) {
      this.devAffinityLine = '';
      this.devAffinityFull = '';
      this.devAllowedDetail = '';
      this.devThreadCapNote = '';
      console.warn('[AB] cpu affinity probe unavailable');
      return;
    }
    this.devAffinityLine = snap.line;
    // 逐路径事实: source + cgroup + cpuset + 路径读数(读不到的带 errno)
    let detail: string = snap.source;
    if (snap.cgroupPath.length > 0) {
      detail += ' · cgroup=' + snap.cgroupPath;
    }
    if (snap.cpusetDir.length > 0) {
      detail += ' · cpuset 目录=' + snap.cpusetDir;
    }
    if (snap.cpusetCpus.length > 0) {
      detail += ' · cpuset.cpus=' + snap.cpusetCpus;
    } else {
      detail += ' · cpuset.cpus 读不到(errno=' + snap.cpusetCpusErrno.toString() + ')';
    }
    if (snap.cpusetEffective.length > 0) {
      detail += ' · cpuset.cpus.effective=' + snap.cpusetEffective;
    } else {
      detail += ' · cpuset.cpus.effective 读不到(errno=' + snap.cpusetEffErrno.toString() + ')';
    }
    if (snap.memsFound && snap.memsList.length > 0) {
      detail += ' · Mems_allowed_list=' + snap.memsList;
    } else {
      detail += ' · Mems_allowed_list 读不到';
    }
    this.devAllowedDetail = detail;
    // 快簇是怎么算出来的(native 的原话)
    this.devAffinityFull = snap.fastSource.length > 0 ? snap.fastSource : snap.text;
    // 全机频率档(逐核原始读数, 读失败的核带 errno): 与可用核集合放在同一屏, 两台机器可直接比。
    // 数据来自芯片判读探测(与 native 的 cpuIdentityProbe 同一份), 这里只做排版, 不改任何值。
    const chip: ChipJudgmentSnapshot | null = BenchRunner.benchChipIdentity();
    if (chip !== null && chip.probe !== null) {
      const one: string[] = [];
      for (const c of chip.probe.cores) {
        if (c.err === 0) {
          one.push('cpu' + c.cpu.toString() + '=' + c.raw);
        } else {
          one.push('cpu' + c.cpu.toString() + '=读不到(errno=' + c.err.toString() + ')');
        }
      }
      this.devFreqTiers = one.join(' · ');
      if (chip.probe.verdict.length > 0) {
        this.devFreqTiers += ' —— ' + chip.probe.verdict;
      }
    } else {
      this.devFreqTiers = '';
    }
    // 线程数被可用核集合夹过时写明(默认机器上 cap <= 0 -> 这一句不出现)
    if (snap.threadCap > 0 && snap.threadBasisFull > snap.threadCap) {
      this.devThreadCapNote = '多核阶段线程数上限被可用核集合夹到 ' + snap.threadCap.toString() +
        '（不看可用核集合时本应是 ' + snap.threadBasisFull.toString() + '）';
    } else {
      this.devThreadCapNote = '';
    }
    DeviceProfile.get().setCpuAffinity(snap.line, this.devAllowedDetail, this.devFreqTiers);
  }

  // 刷新芯片判读并同步到界面状态(关于页三行 + 可复制文本 + runlog 设备行都读 DeviceProfile)
  private refreshChipIdentity(): void {
    const snap: ChipJudgmentSnapshot = BenchRunner.benchRefreshChipIdentity();
    const d: DeviceProfileData = DeviceProfile.get().data();
    this.devChipMeasured = d.chipMeasured;
    this.devChipPerCore = d.chipPerCore;
    this.devChipInfer = d.chipInfer;
    if (!snap.ok && snap.note.length > 0) {
      console.warn('[AB] chip identity unavailable');
    }
  }

  // 是否正在跑任何负载(含长跑自检/压测): 用来屏蔽"跑分期间拨 SMT 开关"。
  private anyBenchRunning(): boolean {
    return this.running || this.gb7Running || this.gb7MultiRunning || this.gb7GpuRunning ||
      this.snRunning || this.coremarkRunning || this.stressRunning || this.scRunning ||
      this.fullRunning;
  }

  // =========================================================================
  // 装饰性视效总开关(材质模糊 / 转场) —— 与跑分严格互斥
  //
  //  一句话口径: 跑分期间界面上不存在任何"要 GPU 才画得出来"的东西。
  //  两样装饰效果各自怎么在跑分期间消失, 见下面各自的代码位置:
  //   ① 材质模糊(backgroundBlurStyle): 不改调用点, 只把 BlurStyleActivePolicy 从
  //      ALWAYS_ACTIVE 切到 ALWAYS_INACTIVE(官方 API, 等于让模糊层不参与合成),
  //      同时底色换成不透明令牌色 —— 见 materialColor()/blurPolicy();
  //   ② 转场动效(TransitionEffect): 只在"用户点了标签 / 跑完弹结果"这两类用户操作
  //      上出现, 且是一次性(iterations 默认 1); 跑分进度浮层与一键跑分浮层不加任何转场
  //      —— 它们在跑分开始时出现, 那正是绝对不许有动画的时刻。
  //
  //  ★ 曾经还有第三样 ".lightUpEffect(this.heroLift())"(挂在首页开始按钮的圆环 Stack 上),
  //    已于 8.4 真机缺陷修复中**整段删除**, 不要再加回来。理由(不是审美, 是两处硬证据):
  //      ① 它的语义被当时写代码的人理解反了。官方文档(zh-cn/application-dev/reference/
  //         apis-arkui/arkui-ts/ts-universal-attributes-image-effect.md, lightUpEffect):
  //         "如果 value 等于 0 则图像为全黑, 如果 value 等于 1 则图像为全亮效果";
  //         Rosen 的实现(graphic_graphic_2d/rosen/modules/render_service_base/src/render/
  //         rs_light_up_effect_filter.cpp)是一张 4x5 颜色矩阵, 偏移项 = lightUpDegree - 1.0
  //         直接加到 R/G/B 上 —— 也就是 value 0 = 三个通道各减 1.0 = 纯黑, 1 = 不变。
  //         RSProperties 的默认值就是 1.0f(不设 = 原样), IsLightUpEffectValid() = 0 <= v < 1。
  //         旧代码里 heroLift() 返回 0.18(平时)/ 0(跑分期间), 两个值都在 [0,1) 里 ——
  //         两种状态都会把按钮压成黑/近黑, 而且都会点亮一次离屏滤镜。
  //      ② 真机实测(Pura X Max, HOP-AL00, OpenHarmony-7.0.0.105): 按钮位置渲染成一个
  //         638x650px 的纯 #000000 矩形(直角), 连内切圆之外的四个角都是纯黑 ——
  //         说明这条属性把整个节点的包围盒当成一张不透明黑图合成上去了, 不只是"变暗"。
  //    按钮本来就不需要任何滤镜: 它是一个实心圆 + 一圈描边, 见 startCircleInner()。
  //
  //  触发点与传感器版一样是三条独立的路(标位置翻转 -> @Watch; 每个负载开跑前都会调的
  //  keepScreen(); 页面启动时初始化一次), 全部收敛到 syncFx()。
  // =========================================================================
  private onRunFlagChange(_prop: string): void {
    this.syncFx();
  }

  //  开关本身不再是"一个被写来写去的布尔", 而是 fxOn() 这个函数(下面是它的唯一实现)。
  //  syncFx() 只剩两件事: 记一行日志、以及在"刚刚变成关"的那一刻把粒子层收干净。
  private fxLogged: boolean = true;

  private syncFx(): void {
    const on: boolean = this.fxOn();
    if (this.fxLogged !== on) {
      this.fxLogged = on;
      console.info('[AB] decorative fx -> ' + (on ? 'ON' : 'OFF (benchmark running)'));
      if (!on) {
        // 跑分刚开始: 把可能还在飞的那一场粒子立刻收掉(正常路径下按下按钮时就已经收干净了,
        // 这一句是"绝对不允许装饰动效与负载重叠一帧"的兜底)。
        this.fxStop();
      }
    }
  }

  // =========================================================================
  // ★ 美化总开关(唯一判据) —— 界面里每一处美化都必须先问它。
  //   不许任何地方绕过它去直接看 running / gb7Running / fullRunning / stressRunning…
  //
  //   两道关, 任何一道不过就返回 false:
  //     ① this.anyBenchRunning() —— 跑分进行中恒为 false。
  //        它读的就是既有的那九个负载标志位, 与 @Watch('onRunFlagChange') -> syncFx() 同源,
  //        没有第二套状态(这一条是硬要求, 见文件顶部的说明)。
  //     ② FX_CAPABLE —— 运行时拿不到这套绘制能力时恒为 false。
  //        拿不到就**静默不做效果**: 控件的颜色 / 尺寸 / 文字一个字节都不变。
  //
  //   ★ fxOn() 返回 false 的后果不是"不播动画", 而是**这些元件根本不会被创建**:
  //     · 不创建粒子 Canvas 节点 —— 唯一的创建点 build() 末尾写的是
  //         if (this.fxOn() && this.fxLive) { this.fxParticleLayer() }
  //       跑分期间 fxLive 也被 fxStop() 压成 false, 两道里任何一道都足以让它不存在;
  //     · 不创建光感辉光节点 —— startCircleInner() 等处的 if (this.fxOn()) 分支;
  //     · 不注册任何 setInterval —— fxStart() 的第一句就是再问一次 fxOn();
  //     · 不挂任何阴影 / 渐变 —— fxAura() / fxSheen() 在关掉时返回"没有效果"的规格
  //       (半径 0 + 全透明 / 两端同色), 与没有这条属性在渲染上等价。
  // =========================================================================
  private fxOn(): boolean {
    return !this.anyBenchRunning() && FX_CAPABLE;
  }

  // =========================================================================
  // App 自己的画布(品牌氛围底) —— 与 fxOn() 无关, 这是**页面底色**, 不是装饰效果
  //
  //  2026-10 用户真机反馈原话:「这是效果, 勉勉强强, 但背景太难看了」。背景重做的两条:
  //    ① App 用自己的深色画布, 不再跟随系统浅色主题 —— 那一句写在 EntryAbility.onCreate:
  //       applicationContext.setColorMode(ColorMode.COLOR_MODE_DARK)。这里是它的"画"的那一半;
  //    ② 深底上叠一层很淡的品牌渐变(暗红 -> 深紫 -> 回落), 见 Theme.brandGradient()。
  //
  //  ★ 为什么它不受 fxOn() 控制: fxOn() 管的是"会花 GPU 的装饰效果"(点光源 / 光晕 / 光带 /
  //    粒子 —— 那些要么离屏、要么模糊、要么逐帧重绘)。画布渐变只是一次静态填充,
  //    与它替换掉的纯色是同一档代价, 所以跑分期间它也照常 —— 而且它必须照常,
  //    否则跑分浮层会突然变回另一块底色, 观感直接断掉。
  //  ★ 每一个整屏容器(主页面 + 5 个浮层)都要用它, 只改首页是不够的。
  // =========================================================================
  private appBackdropGrad(): LinearGradient {
    return brandGradient();
  }

  // 磨砂浮层用的半透明版本(理由见 Theme.brandGradient 的 alpha 参数说明):
  //  磨砂层是"模糊背后 + 自己再盖一层", 全不透明的渐变换上去会把玻璃感盖死。
  private appBackdropGradSoft(): LinearGradient {
    return brandGradient(0.86);
  }

  // 磨砂浮层的底色(令牌派生, 由 withAlpha() 生成, 不写任何十六进制字面量):
  //  · 视效开: 80% 不透明的页面底色 —— 叠加在模糊背景之上就是"磨砂玻璃", 但又保证了
  //    文字对比度; 万一设备不支持背景模糊, 退化成 80% 暗色蒙层, 仍然可读(不会全透明)。
  //  · 跑分期间(fxOn() 为假): 100% 不透明 = 与历史观感完全一致。
  private materialColor(): string {
    return withAlpha(BG, this.fxOn() ? 0.80 : 1);
  }

  // 材质策略: 跑分期间 ALWAYS_INACTIVE(官方语义 = 模糊层不激活, 用 inactiveColor 代替),
  // 非跑分期间 ALWAYS_ACTIVE。用策略切换而不是"条件调用属性", 避免增删节点带来的抖动。
  private blurPolicy(): BlurStyleActivePolicy {
    return this.fxOn() ? BlurStyleActivePolicy.ALWAYS_ACTIVE : BlurStyleActivePolicy.ALWAYS_INACTIVE;
  }

  //  ★ 这里原来有一个 heroLift(): number { return this.fxOn() ? 0.18 : 0; },
  //    专供已删除的 .lightUpEffect()。已整段删除 —— 不要再加回来:
  //    lightUpEffect 的取值 0 = 全黑、1 = 原样(见本文件 syncFx 上方的取证说明),
  //    0.18 与 0 都会把"开始跑分"大圆压成黑块。按钮本来就是画出来的实心圆 + 描边,
  //    不需要滤镜; 按钮尺寸/位置/点击行为与这个函数无关(它们只由 startDia / 布局决定)。

  // 官方点击反馈(clickEffect): 按下时的一次性微缩放, 属于"用户操作触发的一次性动效"。
  // 跑分期间返回 null = 完全关闭(连按下反馈也没有), 严格遵守"跑分期间没有任何动画"。
  private fxClick(): ClickEffect | null {
    if (!this.fxOn()) {
      return null;
    }
    const e: ClickEffect = { level: ClickEffectLevel.LIGHT, scale: 0.96 };
    return e;
  }

  // 用户操作触发的一次性转场(标签切换 / 跑完弹结果)。duration 很短且 iterations 默认 1,
  // 不是循环动画; 跑分进度浮层与一键跑分浮层完全不使用它。
  // 跑分期间(fxOn = false)直接退化成 TransitionEffect.IDENTITY = 没有任何动画,
  // 所以即使发生意料之外的切换, 跑分期间也不会有一帧动画。
  private fxTransition(): TransitionEffect {
    if (!this.fxOn()) {
      return TransitionEffect.IDENTITY;
    }
    return TransitionEffect.OPACITY
      .combine(TransitionEffect.translate({ y: 12 }))
      .animation({ duration: 180, curve: Curve.Friction });
  }

  // =========================================================================
  // 沉浸光感(全部是 ArkUI 自带的绘制属性, 不引任何三方库)
  //
  //  a) 辉光节点: 主按钮外圈那一层径向光。**只有 fxOn() 为真时才会被创建** ——
  //     跑分期间这个节点根本不存在, 谈不上"挂上去但不播放"。
  //  b) 光晕(阴影规格): 卡片 / 主按钮 / 大数字 / 选中的标签。关掉时返回
  //     Theme 的 SHADOW_NONE(半径 0 + 全透明) —— 本工程原本就把它当作"无阴影"的规格,
  //     渲染上与没有这条属性等价, 不会生成离屏模糊图层。
  //  c) 光带(渐变): 主按钮上的斜向渐变。关掉时两端同色 —— 与纯色填充逐像素相同。
  //
  //  ★ 这三样都走 fxOn(): 关掉时要么节点不存在(a), 要么属性退化成"没有"(b/c),
  //    绝不会出现"效果不可用 / 关掉了, 控件反而变黑或消失"。
  // =========================================================================
  // =========================================================================
  // 沉浸光感(真家伙): HDS 点光源 —— 全工程**唯一**构造点光的地方
  //
  //  ★ 第一句就是唯一判据 fxOn()(它自己已经含了 anyBenchRunning() 与 FX_CAPABLE),
  //    再叠一道 HDS_CAPABLE(本机/本运行时到底给不给得出 HDS 点光)。
  //    两道里任何一道不过, 立刻返回"没有效果"的那一只 —— hdsEffect 一次都不会被调用,
  //    点光源一次都不会被构造, 与"跑分期间不调 hdsEffect"这条硬要求逐字对应。
  //  ★ 各调用点一律写成 .visualEffect(this.fxLight(...)), 不许绕开这个入口自己去 new。
  //  ★ 参数一律"拉满": BRIGHT 光源 + BORDER_CONTENT(边框与内容一起点亮)。
  // =========================================================================
  //  ★ 这里试过官方的 @SuppressWarnings 装饰器(想把点光那批 syscap 告警压回去), 结论是
  //    本 SDK 的 ArkTS 编译器**不认这个装饰器**: "Cannot find name 'SuppressWarnings'",
  //    BUILD FAILED。抬到 API 20 也一样 —— 它只在 SDK 自己的 .d.ts 里用, 应用代码用不了。
  //    最后压住告警的是下面那个**原地写的 if (canIUse(...))**: 编译器认得它, 10 条 syscap
  //    告警一条不剩(63 -> 53, 与加这一层之前完全一致)。所以那两行不是可有可无的装饰。
  private fxLight(color: string, intensity: number, bloom: number): VisualEffect {
    if (!this.fxOn()) {
      return fxNoLight();
    }
    // 第二道门写在原地(和 canIUse 一起把这段围住): 官方要求"用到带 syscap 的 API 就用
    // canIUse 包住"。拿不到这套能力(老机器 / 该运行时没有 HDS)就静默退回 ArkUI 那套 ——
    // 控件的颜色 / 尺寸 / 文字一个字节都不变。
    if (canIUse('SystemCapability.UIDesign.HDSComponent.Core')) {
      const opt: hdsEffect.PointLightOptions = {
        color: color,
        intensity: intensity,
        height: 30,
        bloom: bloom
      };
      //  ★ 可读性硬标准(2026-10-10 用户真机反馈「你的光不是有质感而是很糊, 影响阅读」之后定下的):
      //    光只许出现在**元件的外沿**, 不许压到任何一行文字上。落地成三条, 以后先过这三条再谈好看:
      //      ① 点光源一律 illuminatedType = BORDER(只点亮边框); **不许再写回 BORDER_CONTENT**
      //         —— 后者会把光打进内容里, 卡片里的长段说明文字第一个糊(这就是用户看到的那句);
      //      ② bloom 只许留在"没有文字"的元件上; 凡是框里有文字的, bloom <= 0.30;
      //      ③ Text 上不许挂任何 .shadow()(辉光贴着字形 = 直接糊字)。
      //    宁可少一处光, 不许糊一行字。
      const pl: hdsEffect.PointLightEffect = {
        sourceType: hdsEffect.PointLightSourceType.BRIGHT,
        illuminatedType: hdsEffect.PointLightIlluminatedType.BORDER,
        options: opt
      };
      return new hdsEffect.HdsEffectBuilder().pointLight(pl).buildEffect();
    }
    return fxNoLight();
  }

  // 点光这一层是不是真的在生效。它只用来决定"两层里让谁上场":
  //  · 点光在场时, ArkUI 那圈径向辉光(fxHalo)**自动退位** —— 两者画的都是"元件外面那一圈光",
  //    叠在一起只会糊成一团, 这就是"打架"的那一处, 以点光优先;
  //  · 点光不在场时, fxHalo 就是主效果 —— 这就是"静默降级"的落点。
  //  · 其余两样(光晕 fxAura / 光带 fxSheen)与点光是互补关系(一个在外、一个在面),
  //    按"在那一层之上再加一层"叠着上, 不互相顶。
  private hdsLightOn(): boolean {
    return this.fxOn() && HDS_CAPABLE;
  }

  @Builder
  fxHalo(color: string, dia: number, alpha: number) {
    // 点光在场时这一层退位; 点光缺席(能力不可用)时它照常画 —— 那套一个字都没删。
    if (!this.hdsLightOn()) {
      Column()
        .width(dia)
        .height(dia)
        .borderRadius(dia / 2)
        .radialGradient({
          center: ['50%', '50%'],
          radius: dia / 2,
          colors: [[withAlpha(color, alpha), 0.0], [withAlpha(color, alpha * 0.35), 0.45],
            [withAlpha(color, 0.0), 1.0]]
        })
        .hitTestBehavior(HitTestMode.None)
    }
  }

  private fxAura(color: string, radius: number): ShadowOptions {
    const s: ShadowOptions = SHADOW_NONE;
    if (this.fxOn()) {
      s.radius = radius;
      s.color = color;
      s.offsetX = 0;
      s.offsetY = 0;
    }
    return s;
  }

  private fxSheen(from: string, to: string): LinearGradient {
    return barGradient(from, this.fxOn() ? to : from);
  }

  // =========================================================================
  // 粒子消散 / 聚拢(纯 ArkUI Canvas 自己画, 不引任何三方库)
  //
  //  唯一入口是 fxFire(), 它的第一句就是 fxOn() —— 跑分期间连粒子数组都不会被建,
  //  更不会注册 tick、更不会创建 Canvas 节点。
  //  四个时刻会用到它(见各自的调用点):
  //    · 首页「开始跑分」按下   -> 从圆心向外"消散"(mode 0)
  //    · 这一次跑分收工       -> 从外面"聚拢"回圆心(mode 1)
  //    · 切换标签页           -> 在底栏被点的那一格位置消散(mode 0)
  //    · 结果出现             -> 屏幕正中聚拢(mode 1)
  // =========================================================================
  private fxRnd(min: number, max: number): number {
    return min + this.fxRand.next() * (max - min);
  }

  private fxFire(cx: number, cy: number, n: number, mode: number, spread: number, color: string): void {
    if (!this.fxOn()) {
      return;                              // ★ 唯一判据: 跑分期间这一整段都不会执行
    }
    this.fxStop();                         // 同一时刻只允许一场(上一场没散完就直接重开)
    const count: number = n < 6 ? 6 : (n > FX_P_MAX ? FX_P_MAX : n);
    const list: FxParticle[] = [];
    for (let i = 0; i < count; i++) {
      const p: FxParticle = new FxParticle();
      const ang: number = (i / count) * Math.PI * 2 + this.fxRnd(-0.28, 0.28);
      const reach: number = spread * this.fxRnd(0.55, 1.25);
      if (mode === 1) {
        p.x = cx + Math.cos(ang) * reach;
        p.y = cy + Math.sin(ang) * reach;
        p.vx = -Math.cos(ang) * reach / FX_FRAMES;
        p.vy = -Math.sin(ang) * reach / FX_FRAMES;
      } else {
        p.x = cx + Math.cos(ang) * spread * 0.14;
        p.y = cy + Math.sin(ang) * spread * 0.14;
        p.vx = Math.cos(ang) * reach / FX_FRAMES;
        p.vy = Math.sin(ang) * reach / FX_FRAMES;
      }
      p.r0 = this.fxRnd(1.4, 3.4);
      p.spin = this.fxRnd(0.35, 1.0);
      list.push(p);
    }
    this.fxColor = color;
    this.fxParts = list;
    this.fxFrame = 0;
    this.fxLive = true;                    // 到这里才让 Canvas 节点出现
    this.fxStart();
  }

  // 注册 tick: 第一句就是问 fxOn() —— 跑分期间永远不会注册。
  private fxStart(): void {
    if (!this.fxOn() || !this.fxReady || this.fxParts.length === 0) {
      return;
    }
    if (this.fxTimer >= 0) {
      return;                              // 已经有一个 tick 了, 不重复注册
    }
    this.fxTimer = setInterval(() => {
      this.fxStep();
    }, FX_TICK_MS);
  }

  private fxCtxGet(): CanvasRenderingContext2D {
    let c: CanvasRenderingContext2D | null = this.fxCtx;
    if (c === null) {
      c = new CanvasRenderingContext2D(new RenderingContextSettings(true));
      this.fxCtx = c;
    }
    return c;
  }

  private fxStep(): void {
    if (!this.fxOn()) {
      this.fxStop();                       // 双保险: 跑分一开始立刻停
      return;
    }
    this.fxFrame++;
    const t: number = this.fxFrame / FX_FRAMES;          // 0 -> 1
    try {
      const ctx: CanvasRenderingContext2D = this.fxCtxGet();
      const w: number = ctx.width > 0 ? ctx.width : this.winW;
      const h: number = ctx.height > 0 ? ctx.height : this.winH;
      ctx.clearRect(0, 0, w, h);
      const fade: number = t < 1 ? (1 - t) : 0;
      ctx.fillStyle = this.fxColor;
      ctx.globalAlpha = fade;
      const parts: FxParticle[] = this.fxParts;
      for (let i = 0; i < parts.length; i++) {
        const p: FxParticle = parts[i];
        p.x = p.x + p.vx;
        p.y = p.y + p.vy;
        const r: number = p.r0 * (0.35 + p.spin * fade);
        ctx.beginPath();
        ctx.arc(p.x, p.y, r > 0.3 ? r : 0.3, 0, Math.PI * 2);
        ctx.fill();
      }
      ctx.globalAlpha = 1;
    } catch (e) {
      // 画不出来就当场收工 —— 装饰效果永远不许把界面拖住, 更不许把跑分的计时窗口搞脏
      this.fxStop();
      return;
    }
    if (this.fxFrame >= FX_FRAMES) {
      this.fxStop();
    }
  }

  // 收工: 清 tick、清画布、把 Canvas 节点从界面上摘掉(把 fxLive 压回 false)。
  //  跑分开始 / 页面销毁 / 一场粒子跑完, 三条路都走这里。
  private fxStop(): void {
    if (this.fxTimer >= 0) {
      clearInterval(this.fxTimer);
      this.fxTimer = -1;
    }
    if (this.fxParts.length > 0) {
      this.fxParts = [];
      const c: CanvasRenderingContext2D | null = this.fxCtx;
      if (c !== null) {
        try {
          // 节点可能已经被摘掉了 —— 清不掉就算了, 反正画布随节点一起消失
          c.clearRect(0, 0, c.width > 0 ? c.width : this.winW, c.height > 0 ? c.height : this.winH);
        } catch (e) {
          console.info('[AB] fx canvas already detached');
        }
      }
    }
    if (this.fxLive) {
      this.fxLive = false;
    }
  }

  // Length(可能是 number / string / Resource) -> vp 数值; 拿不到就当 0(静默, 不抛)。
  private fxNum(v: Length | undefined): number {
    if (typeof v === 'number') {
      return v;
    }
    return 0;
  }

  // 「开始跑分」圆心(窗口坐标 vp)。onAreaChange 还没来过时退回窗口正中 —— 只是位置略有偏差,
  //  绝不会因此让任何控件消失或变色。
  private heroCx(): number {
    return this.fxHx > 0 ? this.fxHx : (this.winW > 0 ? this.winW / 2 : 180);
  }

  private heroCy(): number {
    return this.fxHy > 0 ? this.fxHy : (this.winH > 0 ? this.winH / 2 : 390);
  }

  // ---- 四个"粒子时刻"(每一个的第一句都在 fxFire 里问 fxOn()) ----

  // ① 首页「开始跑分」按下: 先从圆心向外消散, 散完(约 0.6 秒)才真正开跑。
  //   为什么必须等: 美化层的铁律是"装饰动效绝不与负载重叠一帧"。这 0.6 秒里一个负载都还没开始,
  //   计时也还没开始(startRun 里才取 t0), 所以这个延迟既不影响任何测量值, 也不留任何东西在跑分期间。
  //   关掉美化时(跑分中 / 能力不可用): 一个粒子都不放, 立刻开跑 —— 行为与历史版本逐帧相同。
  private fxHeroStart(): void {
    if (!this.fxOn()) {
      this.startRun();
      return;
    }
    this.fxFire(this.heroCx(), this.heroCy(), 26, 0, this.startDia * 0.62, ACCENT);
    setTimeout(() => {
      this.startRun();
    }, FX_TICK_MS * (FX_FRAMES + 1));
  }

  // ② 这一次跑分收工: 从外面聚拢回按钮圆心。
  //   调用点在 running 被压回 false 之后 —— 此时 fxOn() 已经重新为真, 粒子只会出现在"跑完之后"。
  private fxFinishBurst(): void {
    this.fxFire(this.heroCx(), this.heroCy(), 22, 1, this.startDia * 0.70, GOOD);
  }

  // ③ 切换标签页: 在底栏被点的那一格位置消散。
  //   底栏是等分的三等份(见 build() 里的 Row + tabBarItem 的 layoutWeight(1)),
  //   所以第 idx 格的中心 x = 底栏左缘 + 底栏宽 x (idx + 0.5) / 3。
  private fxTabBurst(idx: number): void {
    if (!this.fxOn()) {
      return;
    }
    const n: number = 3;
    const w: number = this.fxBarW > 0 ? this.fxBarW : this.winW;
    const x: number = (this.fxBarX >= 0 ? this.fxBarX : 0) + w * (idx + 0.5) / n;
    const y: number = this.fxBarY > 0 ? this.fxBarY : (this.winH > 0 ? this.winH - TAB_H : 700);
    this.fxFire(x, y, 20, 0, 74, ACCENT2);
  }

  // ④ 结果出现: 屏幕正中聚拢。
  private fxResultBurst(): void {
    this.fxFire(this.winW > 0 ? this.winW / 2 : 180, this.winH > 0 ? this.winH / 2 : 390,
      30, 1, 168, ACCENT);
  }

  // 粒子层: 铺满整屏、永远不吃触摸(HitTestMode.None)。
  //  ★ 唯一创建点在 build() 末尾的 "if (this.fxOn() && this.fxLive)" ——
  //    跑分进行中 fxOn() 恒为 false, 这个 Canvas 节点根本不存在, 没有任何一帧重绘。
  @Builder
  fxParticleLayer() {
    Canvas(this.fxCtxGet())
      .width('100%')
      .height('100%')
      .hitTestBehavior(HitTestMode.None)
      .onReady(() => {
        // Canvas 刚被创建出来(它只在 fxLive 为真时才存在) —— 这里才可能注册 tick
        this.fxReady = true;
        this.fxStart();
      })
  }


  // =========================================================================
  // 沉浸式(边到边)与安全区
  //
  //  口径: 窗口铺满整屏(背景可以压到状态栏/导航栏下面), 可点内容一律用 insets 让开系统栏。
  //   insets 来自 window.getWindowAvoidArea()(px) -> px2vp() 换算; 只在系统栏变化时更新
  //   (窗口的 avoidAreaChange 事件), 不做任何轮询。
  //   为什么还要看 immersiveOn: 只有当窗口真的铺满整屏时, 系统栏才会压在页面上面, 这时才
  //   需要 insets padding; 万一 setWindowLayoutFullScreen 没成功(窗口本身已被系统栏挤开),
  //   再加 padding 就会白白多一条空白 —— 那时按 0 处理。
  // =========================================================================
  private refreshInsets(): void {
    if (this.winObj === null) {
      return;
    }
    let full: boolean = false;
    try {
      full = this.winObj.getWindowProperties().isLayoutFullScreen;
    } catch (e) {
      full = false;
    }
    this.immersiveOn = full;
    let top: number = 0;
    let bottom: number = 0;
    if (full) {
      top = this.avoidPxTop();
      bottom = this.avoidPxBottom();
    }
    // px -> vp: 走 UIContext 的换算(全局函数 px2vp 已标记废弃, 会多出两条 ArkTS 警告,
    // 而本工程的硬约束是"不新增警告")。换算失败时按 0 处理 —— 退化为系统自带的安全区避让。
    let t: number = 0;
    let b: number = 0;
    try {
      const ui = this.getUIContext();
      t = Math.round(ui.px2vp(top));
      b = Math.round(ui.px2vp(bottom));
    } catch (e) {
      t = 0;
      b = 0;
    }
    // 取整并夹到 [0, 200] —— 宁可留白也不遮控件, 但也不允许一个异常的 insets 值把界面顶下去。
    this.insetTopVp = t > 0 ? (t > 200 ? 200 : t) : 0;
    this.insetBottomVp = b > 0 ? (b > 200 ? 200 : b) : 0;
  }

  // 沉浸式 insets 的有界重读: 最多 tries 次(600ms 一次), 一旦确认窗口已铺满就停。
  // 为什么需要: 页面第一次读的时候 setWindowLayoutFullScreen 很可能还没生效(它是 Promise),
  // 那时 immersiveOn = false -> padding 为 0; 等系统真正切到铺满后再读一次才能拿到正确 insets。
  // 这条路径与既有的 probeWindowTry 完全同形: 一次性 setTimeout, 没有 setInterval, 不重绘。
  private probeInsets(tries: number): void {
    this.refreshInsets();
    if (this.immersiveOn || tries >= 3) {
      return;
    }
    setTimeout(() => {
      this.probeInsets(tries + 1);
    }, 600);
  }

  // 状态栏高度(px): TYPE_SYSTEM 的 topRect(visible = false 时按 0 处理)
  private avoidPxTop(): number {
    if (this.winObj === null) {
      return 0;
    }
    try {
      const a: window.AvoidArea = this.winObj.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM);
      return a.visible ? a.topRect.height : 0;
    } catch (e) {
      return 0;
    }
  }

  // 底部导航条 / 手势条高度(px): 优先 TYPE_NAVIGATION_INDICATOR(手势条),
  // 取不到再退回 TYPE_SYSTEM 的 bottomRect(三键导航栏)。两者都取不到就是 0(不额外留白)。
  private avoidPxBottom(): number {
    if (this.winObj === null) {
      return 0;
    }
    let h: number = 0;
    try {
      const a: window.AvoidArea =
        this.winObj.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR);
      h = a.visible ? a.bottomRect.height : 0;
    } catch (e) {
      h = 0;
    }
    if (!(h > 0)) {
      try {
        const s: window.AvoidArea = this.winObj.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM);
        h = s.visible ? s.bottomRect.height : 0;
      } catch (e) {
        h = 0;
      }
    }
    return h;
  }

  private padTop(): number {
    return this.insetTopVp;
  }

  private padBottom(): number {
    return this.insetBottomVp;
  }

  // 开关被拨动(界面回调)。
  // 已知的理论竞态: native 侧的 .so 由 8 个线程共享, 而"实际使用集合"是函数局部 static 单例
  // (既有设计, 见 cpu_affinity.h)。若在负载运行期间改开关触发重算, 严格说不是线程安全的 ——
  // 前提是跑分期间开关可被改动。所以这里用 anyBenchRunning() 把它挡死:
  // 跑分期间回调直接 return, 不写 native、也不落盘(不靠"UI 本来就禁用"这种想当然)。
  // 注意: Toggle 上不能再加 .enabled(...) —— 实测本版 ArkTS 编译器(5.2.2)对
  // "Toggle + .enabled" 会直接 panic(ArkTS: INTERNAL ERROR, Code: 10310015), 所以护栏放在回调里。
  private onSmtSwitch(on: boolean): void {
    if (this.anyBenchRunning()) {
      return;
    }
    this.applySmt(on, true);
  }

  // 开关旁边那一行小字(用户要求: 开/关各几个线程、会少几个, 必须在"按下去之前"就看到)。
  // 文本完全由 native 生成(smtSwitchHint() -> auroraSmtSwitchHint(), 见 cpu_affinity.h):
  //   里面的逻辑核 / 物理核 / 内核允许核数 / "开启 N 线程 → 关闭 M 线程(少 D 个)"全部来自
  //   native 读到的同一份拓扑与可用核集合。ArkTS 只把这行字符串显示出来, 一个数字都不自己拼 ——
  //   否则就会出现"界面说 9 个线程、native 实际只开 1 个"这种两边各说各话(真机踩过)。
  private smtHint(): string {
    const s: string = BenchRunner.benchSmtSwitchHint();
    if (s.length > 0) {
      return s;
    }
    // native 没给(旧 .so / 接口不可用): 说"没有这一行", 不编造任何一个数
    return '（native 未返回 SMT 开关约束说明：smtSwitchHint 不可用）';
  }

  // 「设备画像」里的 CPU 拓扑行(含"拓扑未知"与"检测到 SMT"两种写法)
  private benchTopologyLine(): string {
    if (!this.smtApiOk()) {
      return 'CPU 拓扑：（native 接口不可用, 未上报）';
    }
    return smtTopologyLine(BenchRunner.benchCapabilities());
  }

  private smtApiOk(): boolean {
    return BenchRunner.benchSmtApiOk() && BenchRunner.benchCapabilities() !== null;
  }

  // 内核是否把最快档挡在了可用核集合之外(交集为空 / 生效快簇低于全机最快档)——
  // 只用于给「设备画像」那一行上警告色, 不参与任何计算与计分。
  private affinityRestricted(): boolean {
    const a: CpuAffinitySnapshot | null = BenchRunner.benchCpuAffinity();
    if (a === null) {
      return false;
    }
    return a.fastFallback || a.fastTierIndex > 0;
  }

  // 多核阶段的线程数(按开关): 开 = 全部逻辑核, 关 = 物理核数, 拓扑未知 = 全部逻辑核。
  // 单核阶段不走这里(永远是 1)。
  private multiThreads(): number {
    return BenchRunner.benchMultiThreads(this.cores > 0 ? this.cores : 4);
  }

  // 多核按钮上那一小段 SMT 口径: "SMT 开" / "SMT 关" / "SMT 未知(1:1)" / "无 SMT"
  private smtButtonTag(): string {
    const th: number = this.gb7Threads > 0 ? this.gb7Threads : this.multiThreads();
    const f: string = BenchRunner.smtSummaryFlag(th);
    if (f === '' || f === '无 SMT') {
      return '无 SMT';
    }
    if (f === '未知(1:1)') {
      return 'SMT 未知（1:1）';
    }
    return 'SMT ' + f;
  }

  // 旁路: 把这一项的"跑在哪个核"文本交给设备画像(关于页显示最近一次; 不计分)
  private noteCpuPlacement(r: Gb7Result): void {
    if (r.cpuInfo.length > 0) {
      DeviceProfile.get().setLastCpuInfo(r.cpuInfo);
      this.devCpuInfo = r.cpuInfo;
    }
  }

  // 绑核诊断的四种状态(同一套判据给 note 与汇总行用, 只有一处定义):
  //   0 无信息: native 什么都没上报(cpuInfo 空 且 连 cpu 编号都没有; 老 native / GPU 负载)
  //   1 正常  : cpuBound && cpuInFastCluster(跑完仍在生效快簇内; 簇内迁移不算异常)
  //   2 异常  : cpuBound && !cpuInFastCluster(掩码应用成功却落在簇外) —— 只有这一种报警
  //   3 降级/受限: cpuBound = false。2026-10-05 起这一档包含两种不同的事实, 用
  //      cpuFastClusterFallback 区分, 汇总行会分开说(不糊成一句"未绑定"):
  //        a) fallback = true : 全机最快档 ∩ 可用核集合 = 空 —— 绑核目标不在可用核集合内,
  //           native 已退化为"可用核集合里最快的核"(这是系统策略决定的, 不是读数失败);
  //        b) fallback = false: 读不到 cpuinfo_max_freq 等读数失败, 已整个降级为不绑。
  //      两者都不报警(数值与计分不变), 但必须说清是哪一种 —— 这正是用户质疑的答案所在。
  // 判据变化(2026-10): 旧判据 "cpu != cpuAtStart" / "cpuRank > 0" 是误报(native 现在把线程绑到
  // 大核簇而不是单个最快核, 位次 4 = 最高频档, cpu4 → cpu7 本来就在同一个频率档里)。
  // 注意: 异常只看 cpuBound / cpuInFastCluster, 不以 cpuInfo 是否非空为前提 ——
  // cpuBound = true 只可能来自"确实上报过"的 native, 拿 cpuInfo 当门槛会把这种真异常当成
  // "无信息"漏报(而且汇总行还会误写成"所有项均跑在大核簇内")。
  private cpuPlacementState(r: Gb7Result): number {
    if (r.cpuBound) {
      return r.cpuInFastCluster ? 1 : 2;
    }
    if (r.cpuInfo.length === 0 && r.cpu === -1 && r.cpuFastClusterCores === 0) {
      return 0;
    }
    return 3;
  }

  // 结果卡底部的"绑核汇总"那一行(纯展示, 不改任何数值)。
  // 判据(2026-10 起): 只有 cpuBound === true && cpuInFastCluster === false 才是异常;
  // 簇内迁移(如 cpu4 → cpu7)不算异常, 只作信息; 未绑定(读不到频率表)不报警, 只用灰字说明。
  // 注意: cpu 编号与频率必须取同一项的读数, 不能各取最大值 —— 否则会拼出
  // "cpu7，2400 MHz" 这种根本不存在的组合(每个核有各自的最高频)。
  private cpuSummary(list: Gb7Result[]): string {
    let sawInfo: boolean = false;
    let outsideCount: number = 0;     // 跑到了生效快簇之外(唯一的告警情形)
    let outsideNames: string = '';
    let degradedCount: number = 0;    // 未绑定(读不到 cpufreq): 不报警, 灰字说明
    let degradedNames: string = '';
    // 2026-10-05 追加: "绑核目标不在可用核集合内"(交集为空)与"读不到频率表"必须分开统计
    let restrictedCount: number = 0;  // 交集为空 -> 已退化为可用核集合里最快的核
    let restrictedNames: string = '';
    let allowedCount: number = 0;     // 内核允许的核数(同一台机器上恒定)
    let allowedMask: string = '';
    let fastFallback: boolean = false;
    let fastTierIndex: number = 0;
    let machineTopCores: number = 0;  // 全机最快频率档的核数
    let machineTopKhz: number = 0;
    let notTopTierCount: number = 0;  // 跑完不在全机最快档的项数(独立事实, 只提示不报警)
    let topTierKnown: boolean = false;
    let inCount: number = 0;          // 正常: 跑完仍在大核簇内
    let movedNames: string = '';      // 簇内迁移(起跑与跑完不同核, 但都在簇内): 只作信息
    let worstCpu: number = -1;        // 异常项的落点(用来说明"实际跑在哪颗、最高频多少")
    let worstKhz: number = 0;
    let workers: number = 0;
    let clusterCores: number = 0;     // 大核簇的形状(同一台机器上恒定, 取第一项有效读数)
    let clusterKhz: number = 0;
    for (const r of list) {
      if (r.cpuInfo.length === 0 && r.cpu === -1 && r.cpuFastClusterCores === 0) {
        continue;
      }
      sawInfo = true;
      const st: number = this.cpuPlacementState(r);
      if (r.cpuWorkers > workers) {
        workers = r.cpuWorkers;
      }
      // 可用核集合 / 快簇形状 / 全机最快档(同一台机器上恒定, 取第一项有效读数)
      if (r.cpuAllowedCount > allowedCount) {
        allowedCount = r.cpuAllowedCount;
        allowedMask = r.cpuAllowedMask;
      }
      if (r.cpuFastClusterFallback) {
        fastFallback = true;
      }
      if (r.cpuFastClusterTierIndex > fastTierIndex) {
        fastTierIndex = r.cpuFastClusterTierIndex;
      }
      if (r.cpuMachineTopTierCores > 0) {
        machineTopCores = r.cpuMachineTopTierCores;
        machineTopKhz = r.cpuMachineTopTierKhz;
      }
      if (r.cpuInFastClusterJudged && r.cpu >= 0) {
        topTierKnown = true;
        if (!r.cpuInMachineTopTier) {
          notTopTierCount++;
        }
      }
      if (st === 2) {
        outsideCount++;
        outsideNames += (outsideNames.length > 0 ? '、' : '') + r.name;
        if (r.cpu >= 0) {
          worstCpu = r.cpu;
          worstKhz = r.cpuMaxKhz;
        }
      } else if (st === 3) {
        // 两种完全不同的"未绑定"分开统计
        if (r.cpuFastClusterFallback) {
          restrictedCount++;
          restrictedNames += (restrictedNames.length > 0 ? '、' : '') + r.name;
        } else {
          degradedCount++;
          degradedNames += (degradedNames.length > 0 ? '、' : '') + r.name;
        }
      } else if (st === 1) {
        inCount++;
        if (r.cpuFastClusterCores > 0) {
          clusterCores = r.cpuFastClusterCores;
          clusterKhz = r.cpuFastClusterMaxKhz;
        }
        if (r.cpuAtStart >= 0 && r.cpu >= 0 && r.cpuAtStart !== r.cpu) {
          // 起跑与跑完不是同一颗核, 但两端都在大核簇内 -> 无害迁移, 只作信息展示
          movedNames += (movedNames.length > 0 ? '、' : '') + r.name + ' cpu' +
            r.cpuAtStart.toString() + ' → cpu' + r.cpu.toString();
        }
      }
    }
    if (!sawInfo) {
      return '本次未拿到"跑在哪个核"信息（native 未上报 cpu 字段, 常见于 GPU 负载或不支持绑核的平台）';
    }
    // 可用核集合的一句话(只有 native 上报了才加, 免得凭空出现"allowed=0")
    let allowText: string = '';
    if (allowedCount > 0) {
      allowText = ' · 内核允许核集合 ' + allowedCount.toString() + ' 核(' +
        (allowedMask.length > 0 ? allowedMask : '掩码未上报') + ')';
    }
    // 全机最快档的一句话(与"绑核是否生效"是两件事, 单独说)
    let topTierText: string = '';
    if (machineTopCores > 0) {
      const mhz: string = machineTopKhz > 0
        ? ('/' + Math.round(machineTopKhz / 1000).toString() + ' MHz') : '';
      topTierText = ' · 全机最快档是 ' + machineTopCores.toString() + ' 核' + mhz;
      if (topTierKnown && notTopTierCount > 0) {
        topTierText += '，本次有 ' + notTopTierCount.toString() + ' 项没跑在全机最快档上';
      }
    }
    // ① 异常(警告色): 掩码绑上了, 负载却跑到了簇外
    if (outsideCount > 0) {
      return '有 ' + outsideCount.toString() + ' 项跑到了生效快簇之外（' + outsideNames +
        '）：实际跑在 ' + this.cpuPairText(worstCpu, worstKhz) +
        ' · 这几项的分数可能被小核拖低（只提示，数值与计分不变）' + allowText + topTierText;
    }
    const clusterText: string = this.cpuClusterText(clusterCores, clusterKhz);
    // ② 受限(灰字, 不报警, 但必须说清): 绑核目标不在可用核集合内 ═> 交集为空
    //    这一档以前会被糊成"未绑定(读不到 cpufreq)", 那样回答不了"为什么代际差异没体现" ——
    //    真机上的实际含义是: 内核不允许我们用最高频的那几个核, 所以分数被按在可用核集合的频率上。
    if (restrictedCount > 0) {
      let d: string = '有 ' + restrictedCount.toString() +
        ' 项未能绑定最快档（native 报告：绑核目标不在可用核集合内, 交集为空：' + restrictedNames + '）';
      if (allowedCount > 0) {
        d += ' · 内核只允许用 ' + allowedCount.toString() + ' 个核(' +
          (allowedMask.length > 0 ? allowedMask : '掩码未上报') + ')';
      }
      if (machineTopCores > 0) {
        const mhz: string = machineTopKhz > 0
          ? ('（' + Math.round(machineTopKhz / 1000).toString() + ' MHz）') : '';
        d += '，而全机最快档在另外 ' + machineTopCores.toString() + ' 个核上' + mhz;
      }
      d += ' —— 已改用可用核集合里最快的核（系统策略决定, 不是读数失败; 数值与计分不变）';
      if (inCount > 0) {
        d += ' · 其余 ' + inCount.toString() + ' 项跑在生效快簇内（' + clusterText + '）';
      }
      return d;
    }
    // ③ 降级(灰字, 不报警): 读不到频率表 -> 忽略一切簇字段
    if (degradedCount > 0) {
      let d: string = '有 ' + degradedCount.toString() +
        ' 项未能绑定大核簇（读不到 cpufreq，native 已静默降级：' + degradedNames + '）';
      if (inCount > 0) {
        d += ' · 其余 ' + inCount.toString() + ' 项均跑在大核簇内（' + clusterText + '）';
      }
      return d + ' · 这几项不参与绑核判定（只提示，数值与计分不变）';
    }
    // ④ 正常: 全部跑在生效快簇内
    const workerText: string = workers > 0 ? (' · 池线程 ' + workers.toString() + ' 个已绑定') : '';
    let out: string = '所有项均跑在生效快簇内（' + clusterText + '）' + workerText;
    if (movedNames.length > 0) {
      out += ' · 簇内迁移（不影响计分）：' + movedNames + ', 同在快簇(' +
        this.cpuClusterShortText(clusterCores, clusterKhz) + ')';
    }
    if (clusterCores > 0 && clusterCores >= BenchRunner.cores()) {
      // 大核簇覆盖全部核 = 本机只有一个频率档, 掩码等于没限制: 只注明, 不报警
      out += ' · 本机只有一个频率档（大核簇覆盖全部 ' + clusterCores.toString() +
        ' 核，等同于未限制）';
    }
    if (fastTierIndex > 0) {
      out += ' · 生效快簇低于全机最快档 ' + fastTierIndex.toString() +
        ' 档（可用核集合里没有更快档的核）';
    }
    return out + allowText + topTierText;
  }

  // "5 核 · 最高 2270 MHz"(汇总行括号里的写法; 簇形状未上报时退化成一句说明)
  private cpuClusterText(cores: number, khz: number): string {
    if (cores <= 0) {
      return '簇大小未上报';
    }
    if (khz > 0) {
      return cores.toString() + ' 核 · 最高 ' + Math.round(khz / 1000).toString() + ' MHz';
    }
    return cores.toString() + ' 核 · 频率未知';
  }

  // "5 核/2270MHz"(簇内迁移那句里的短写法)
  private cpuClusterShortText(cores: number, khz: number): string {
    if (cores <= 0) {
      return '簇大小未上报';
    }
    if (khz > 0) {
      return cores.toString() + ' 核/' + Math.round(khz / 1000).toString() + 'MHz';
    }
    return cores.toString() + ' 核';
  }

  // "cpu4，2400 MHz" / "cpu3，频率未知" / "cpu 未知" —— 编号与频率来自同一次读数
  private cpuPairText(cpu: number, khz: number): string {
    if (cpu < 0) {
      return 'cpu 未知';
    }
    if (khz > 0) {
      return 'cpu' + cpu.toString() + '，' + Math.round(khz / 1000).toString() + ' MHz';
    }
    return 'cpu' + cpu.toString() + '，频率未知';
  }

  // 汇总行是否要用警告色: 只有"绑上了却跑到大核簇之外"才算异常。
  // 未绑定(读不到 cpufreq)与簇内迁移都不报警(前者 native 已静默降级, 后者无害)。
  private cpuSummaryWarn(list: Gb7Result[]): boolean {
    for (const r of list) {
      if (this.cpuPlacementState(r) === 2) {
        return true;
      }
    }
    return false;
  }

  private slowCount(list: Gb7Result[]): number {
    let n: number = 0;
    for (const r of list) {
      if (BenchRunner.slowItem(r.ms)) {
        n++;
      }
    }
    return n;
  }

  // ===========================================================================
  // 可重复性 / 离散度的界面口径(纯读 native 旁路字段, 一个数字都不改)
  //
  // 两条硬规矩
  //   1) 离散度 > 阈值(verdict = UNRELIABLE)时必须显眼地写「本项重复性差, 该数字仅供参考」;
  //   2) 只跑 1 轮时(没有离散度数据)必须写明「本次只跑 1 轮, 没有离散度数据, 不能据此判断
  //      可信度」—— 不许把'没有数据'显示成'稳定'。
  //  阈值(2% / 5%)是我们自己定的, 界面上照 native 原文显示, 不写成官方阈值。
  // ===========================================================================

  /** 本卡实际跑了几轮(取各项最大值; 0 = native 没上报) */
  private repeatRounds(list: Gb7Result[]): number {
    let n: number = 0;
    for (const r of list) {
      if (r.rounds > n) {
        n = r.rounds;
      }
    }
    return n;
  }

  /** 重复性差(离散度超阈值)的项数 */
  private repeatBadCount(list: Gb7Result[]): number {
    let n: number = 0;
    for (const r of list) {
      if (BenchRunner.repeatBad(r) || BenchRunner.repeatNoValid(r)) {
        n++;
      }
    }
    return n;
  }

  /** 没有离散度数据(有效轮数 < 2)的项数 */
  private repeatNoDataCount(list: Gb7Result[]): number {
    let n: number = 0;
    for (const r of list) {
      if (!BenchRunner.repeatHasData(r)) {
        n++;
      }
    }
    return n;
  }

  /** 有离散度数据的项里最大的相对离散度(0 = 一项都没有数据) */
  private repeatWorstSpread(list: Gb7Result[]): number {
    let m: number = 0;
    for (const r of list) {
      if (BenchRunner.repeatHasData(r) && r.scoreSpreadPct > m) {
        m = r.scoreSpreadPct;
      }
    }
    return m;
  }

  /** 一行汇总(卡片底部): 轮数 · 好/一般/差 · 最差离散度; 没有数据就写没有 */
  private repeatSummary(list: Gb7Result[]): string {
    if (list.length === 0) {
      return '';
    }
    const rounds: number = this.repeatRounds(list);
    const noData: number = this.repeatNoDataCount(list);
    if (rounds === 0) {
      return '重复性：native 未上报（老 native / 该路径没有这一块），不能据此判断可信度。';
    }
    if (noData === list.length) {
      if (rounds <= 1) {
        return '重复性：本次只跑 1 轮, 没有离散度数据, 不能据此判断可信度。' +
          '（想判断可信度请把轮数调到 >= 2；阈值 2% / 5% 是我们自己定的，不是官方阈值。）';
      }
      return '重复性：本次每项跑了 ' + rounds.toString() + ' 轮，但没有一项给出 >= 2 轮的有效读数,' +
        ' 因此没有离散度数据, 不能据此判断可信度。';
    }
    let good: number = 0;
    let fair: number = 0;
    let bad: number = 0;
    for (const r of list) {
      if (!BenchRunner.repeatHasData(r)) {
        continue;
      }
      if (BenchRunner.repeatBad(r)) {
        bad++;
      } else if (r.repeatVerdict === 'FAIR') {
        fair++;
      } else {
        good++;
      }
    }
    return '重复性：每项 ' + rounds.toString() + ' 轮（有效 ' +
      (list.length - noData).toString() + '/' + list.length.toString() + ' 项有离散度数据）· 好 ' +
      good.toString() + ' / 一般 ' + fair.toString() + ' / 差 ' + bad.toString() +
      ' · 最大相对离散度 ' + this.repeatWorstSpread(list).toFixed(2) + '%' +
      '（阈值 2% / 5% 是我们自己定的，不是官方阈值）';
  }

  /** 汇总行是否要用报警色 */
  private repeatSummaryWarn(list: Gb7Result[]): boolean {
    return this.repeatBadCount(list) > 0;
  }

  /** 显眼的  行(重复性差的项名列出来; 没有就返回空串) */
  private repeatBadLine(list: Gb7Result[]): string {
    const names: string[] = [];
    for (const r of list) {
      if (BenchRunner.repeatBad(r) || BenchRunner.repeatNoValid(r)) {
        names.push(r.name + '（' + (BenchRunner.repeatBad(r) ? (r.scoreSpreadPct.toFixed(2) + '%') :
          (r.repeatVerdict === 'RUN_FAILED' ? '运行失败' : '无有效轮次')) + '）');
        if (names.length >= 5) {
          break;
        }
      }
    }
    if (names.length === 0) {
      return '';
    }
    return '本项重复性差, 该数字仅供参考：' + names.join('、') +
      (this.repeatBadCount(list) > names.length ? (' 等 ' + this.repeatBadCount(list).toString() + ' 项') : '') +
      '（相对离散度 > 5%；全部轮次原始值见报告里的 repeatability.roundsDetail）';
  }

  // ------------------------------------------------------------------
  //  「本项是否真的随芯片性能变化」自检在界面上的呈现(2026-10-08)
  //  判据与结论都来自 service/ScalingAudit.ets —— 与报告 D2 节、JSON scalingAudit
  //  是同一份结论, 界面上不另算一套(否则又是"两处各说各话")。
  //  三种取值必须长得不一样: 通过 / 不通过(不可用于跨芯片比较; 仍计入复合分, 仅提示) / 未自证。
  // ------------------------------------------------------------------
  private scalingItemLine(r: Gb7Result): string {
    if (r.scalingVerdict.length === 0) {
      return '';
    }
    const tag: string = r.scalingVerdict === SC_PASS ? '随芯片变化自检：通过'
      : (r.scalingVerdict === SC_FAIL
        ? '随芯片变化自检：不通过 —— 不可用于跨芯片比较（仅提示，仍按同一口径计入复合分）'
        : '随芯片变化自检：未自证（本轮没有"改变可用算力"的对照，不冒充通过）');
    return tag + ' · ' + r.scalingEvidence;
  }

  private scalingItemColor(r: Gb7Result): string {
    if (r.scalingVerdict === SC_FAIL) {
      return DANGER;
    }
    if (r.scalingVerdict === SC_UNPROVEN) {
      return WARN;
    }
    if (r.scalingVerdict === SC_PASS) {
      return GOOD;
    }
    return FAINT;
  }

  /** 汇总行(与报告 D2 节同一份结论; 多核卡把单核卡的结果一起传进来) */
  private scalingSummaryLine(single: Gb7Result[], multi: Gb7Result[]): string {
    const s: ScalingSummary = summarize(single, multi);
    if (s.multiRows.length === 0) {
      return '随芯片变化自检：本卡是单核阶段，只能判轮间离散度（' +
        '并行加速比对照要等多核阶段）—— 结论以多核卡 / 报告 D2 节为准';
    }
    return '随芯片变化自检：通过 ' + s.passN.toString() + ' · 不通过 ' + s.failN.toString() +
      ' · 未自证 ' + s.unprovenN.toString() +
      (s.failN > 0
        ? ('（不可用于跨芯片比较：' + s.excludedNames.join('、') +
          '；仅提示，仍按同一口径计入复合分）')
        : '（没有"不随芯片变化"的项；未自证的项不得当作已通过）');
  }

  private scalingSummaryWarn(single: Gb7Result[], multi: Gb7Result[]): boolean {
    return summarize(single, multi).failN > 0;
  }

  /** 单项那一行的颜色: 差/无有效轮次 = 报警色; 一般/没有数据 = 警告色; 好 = 次要色 */
  private repeatColor(r: Gb7Result): string {
    if (r.repeatVerdict.length === 0) {
      return FAINT;
    }
    if (BenchRunner.repeatBad(r) || BenchRunner.repeatNoValid(r)) {
      return DANGER;
    }
    if (!BenchRunner.repeatHasData(r) || r.repeatVerdict === 'FAIR') {
      return WARN;
    }
    return SUB;
  }

  /** 单项的可重复性一行文本(没有上报就写'未上报', 不写'稳定') */
  private repeatItemLine(r: Gb7Result): string {
    const line: string = BenchRunner.repeatLine(r);
    if (line.length > 0) {
      return '重复性：' + line;
    }
    return '重复性：未上报（老 native / GPU 路径没有这一块），不能据此判断可信度';
  }

  // ===========================================================================
  // 两份报告对比(compareReports, 只读纯计算) —— 用户最需要的那个入口
  //
  //  输入 = 两份 report-latest.json 全文(每台设备各跑一次「一键全部跑分」之后落盘的那份);
  //  输出 = 逐项 A/B 分 + 原始比 + 每核归一化比 + 同频归一化比 + 与真值的偏差% +
  //         是否在 5% 内, 并列出已知污染源与两个复合分的比。
  //  原始比会被核数(9 vs 8 核 = 虚高 12.5%)与频率档(1.135x = 13.5% 假提升)污染 ——
  //    界面把两个归一化比与原始比并排显示, 并写明这件事; 缺输入原样显示 native 写的缺什么。
  // ===========================================================================

  private cmpListLocal(): void {
    // 最多列 5 份(每份都要读全文; 报告 JSON 单份几百 KB, 不做无上限的同步读盘)
    this.cmpLocal = listLocalReports(5);
  }

  /** 非空视图(与结果页 `this.shown as BenchRecord` 同一套写法: @Builder 里不做可空收窄) */
  private cmpResult(): CmpOutcome {
    return this.cmpOut as CmpOutcome;
  }

  private cmpPickLabel(side: number): string {
    const f: CmpFile | null = side === 1 ? this.cmpA : this.cmpB;
    if (f === null) {
      return (side === 1 ? '设备A' : '设备B') + '：未选择';
    }
    const c: CmpFile = f as CmpFile;
    if (!c.ok) {
      return (side === 1 ? '设备A' : '设备B') + '：' + c.name + '（读取失败：' + c.err + '）';
    }
    return (side === 1 ? '设备A' : '设备B') + '：' + c.name + '（' +
      Math.round(c.text.length / 1024).toString() + ' KB）';
  }

  private cmpUseFile(f: CmpFile, side: number): void {
    if (side === 1) {
      this.cmpA = f;
    } else {
      this.cmpB = f;
    }
    this.cmpOut = null;
    this.cmpCopied = false;
    this.cmpStatus = '已把 ' + f.name + ' 设为' + (side === 1 ? '设备A' : '设备B') +
      (f.ok ? '。' : ('（这份没读到内容：' + f.err + '）'));
  }

  // 从系统文件选择器选一份报告(选完立刻读全文; 读失败带回原因)
  private async cmpPick(side: number): Promise<void> {
    if (this.cmpBusy) {
      return;
    }
    if (this.appCtx === null) {
      this.cmpStatus = '上下文还没就绪，稍后再试。';
      return;
    }
    this.cmpBusy = true;
    this.cmpStatus = '正在打开文件选择器…';
    const f: CmpFile = await pickReportFile(this.appCtx as common.UIAbilityContext);
    this.cmpBusy = false;
    if (!f.ok) {
      this.cmpStatus = '没有选中可用的报告：' + f.err;
      return;
    }
    this.cmpUseFile(f, side);
  }

  private openCompare(): void {
    this.cmpListLocal();
    this.cmpOut = null;
    this.cmpCopied = false;
    this.cmpOpen = true;
    if (this.cmpLocal.length === 0 && this.cmpA === null && this.cmpB === null) {
    this.cmpStatus = '本机 files/ 下还没有落盘的 report-*.json：先跑一次「一键全部跑分」，' +
      '或者用「从文件中选择」挑一份别处导出的报告 JSON。';
    }
  }

  private cmpRun(): void {
    if (this.cmpA === null || this.cmpB === null) {
      this.cmpStatus = '两份都要先选：设备A 与设备B 各一份报告 JSON' +
        '（一键全部跑分落盘的 report-latest.json 就是）。';
      return;
    }
    const a: CmpFile = this.cmpA as CmpFile;
    const b: CmpFile = this.cmpB as CmpFile;
    if (!a.ok || !b.ok) {
      this.cmpStatus = '有报告没读到内容 —— ' + (a.ok ? '' : ('A: ' + a.err + ' ')) +
        (b.ok ? '' : ('B: ' + b.err));
      return;
    }
    if (a.uri === b.uri) {
      this.cmpStatus = '设备A 与设备B 选的是同一份报告：自己跟自己比恒等于 1，没有意义。请换一份。';
      return;
    }
    this.cmpBusy = true;
    this.cmpStatus = '正在对比（纯计算，毫秒级）…';
    const out: CmpOutcome = runCompare(a.text, b.text, a.name, b.name, this.cmpSocA, this.cmpSocB);
    this.cmpOut = out;
    this.cmpBusy = false;
    this.cmpCopied = false;
    if (!out.ok) {
      this.cmpStatus = '对比没有成功：' + (out.error.length > 0 ? out.error : 'native 未给原因') +
        '（缺什么原样显示在下面，界面不替它编一个数）';
      return;
    }
    this.cmpStatus = '对比完成：匹配 ' + out.matched.toString() + ' 项 · 只在A ' +
      out.onlyA.toString() + ' 项 · 只在B ' + out.onlyB.toString() + ' 项' +
      (out.missing.length > 0 ? (' · 缺输入 ' + out.missing.length.toString() + ' 条（见下表）') : ' · 没有缺输入');
  }

  private async cmpCopyJson(): Promise<void> {
    const json: string = this.cmpOut !== null ? this.cmpResult().rawJson : '';
    if (json.length === 0) {
      this.cmpStatus = '还没有对比结果可复制。';
      return;
    }
    try {
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, json);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.cmpCopied = true;
      this.cmpStatus = '已复制对比结果 JSON ' + json.length.toString() + ' 字符到剪贴板';
    } catch (e) {
      this.cmpCopied = false;
      this.cmpStatus = '复制失败：' + e.message;
    }
  }

  // 逐项表: 该不该在它上面显示小节标题(第一项 / 换了小节)
  private cmpShowSection(i: number): boolean {
    const items: CmpItemView[] = this.cmpResult().items;
    if (i === 0) {
      return true;
    }
    if (i < 0 || i >= items.length) {
      return false;
    }
    return items[i - 1].section !== items[i].section;
  }

  private cmpSectionOf(i: number): string {
    const items: CmpItemView[] = this.cmpResult().items;
    return (i >= 0 && i < items.length) ? items[i].section : '';
  }

  /** 与真值偏差的显示文本(没有真值/算不出来就写'—', 不写 0.00%) */
  private cmpDevText(r: CmpItemView): string {
    if (!r.hasDev) {
      return '—';
    }
    return (r.devPct >= 0 ? '+' : '') + r.devPct.toFixed(1) + '%';
  }

  private cmpDevColor(r: CmpItemView): string {
    if (!r.hasDev) {
      return FAINT;
    }
    return r.within5 === 1 ? GOOD : DANGER;
  }

  /** 复合分一行的完整文本: A / B / 原始比 / 参考真值 / 两边各自的偏差 */
  private cmpCompLine(c: CmpCompositeView): string {
    return 'A ' + fmtNum(c.a, 0, c.hasA) + ' → B ' + fmtNum(c.b, 0, c.hasB) +
      ' · 原始比 ' + fmtRatio(c.ratio, c.hasRatio) +
      ' · 参考真值 ' + fmtNum(c.refValue, 0, c.hasRef) +
      ' · A 偏差 ' + (c.hasDeltaA ? ((c.deltaA >= 0 ? '+' : '') + c.deltaA.toFixed(1) + '%') : '—') +
      ' · B 偏差 ' + (c.hasDeltaB ? ((c.deltaB >= 0 ? '+' : '') + c.deltaB.toFixed(1) + '%') : '—');
  }

  private memHeadline(): string {
    if (!this.memSupported) {
      return '可用内存：' + this.memNote;
    }
    return '可用内存 ' + this.memAvailMb.toString() + ' MB';
  }

  // 「清除」: 只收起横幅; 崩溃自诊断历史保留(可复制发回)。
  // native 现场在启动读取时就已经从磁盘清掉了, 这里只是把内存里的副本与 UI 一起收起。
  private dismissCrumb(): void {
    this.crumbFound = false;
    this.crumbLine = '';
    this.crumbDetail = '';
    this.nCrashFound = false;
    this.nCrashPreview = '';
    this.nCrashSummary = '';
    this.nCrashFull = '';
    this.nCrashCopied = false;
    CrashGuard.get().clear().catch((e: Error) => {
      console.warn('[AB] crumb dismiss failed ' + e.message);
    });
  }

  // =========================================================================
  // native 崩溃取证装置(crash_guard.cpp) —— 尽早初始化 + 启动自检 + 现场显示
  //
  // 取证是旁路: 只记录, 不参与任何测试项的计算、顺序与计分。
  // 两条证据互相印证, 合起来才能定性:
  //   · ArkTS 面包屑(preferences)      = 崩溃前"最后启动的那一项" + 该项开跑前的内存足迹
  //   · native 现场(native_crash.txt) = 致命信号 / si_addr / pc 是否落在我们自己的 .so 里 / 内存快照
  // 被系统 SIGKILL(内存压力 · 后台管制)时 native 侧什么都写不出 —— "没有 native 记录"本身就是证据。
  // =========================================================================

  // 重新读一遍装置状态(启动时 / 切到「关于」页时调用)。
  // ctx 只在启动时传(用来兜底初始化); 页内刷新状态不需要 context, 免得再引入一处已废弃的 getContext。
  private refreshNativeGuard(ctx: common.UIAbilityContext | null = null): void {
    if (!CrashGuard.get().guardInitedOnce() && ctx !== null) {
      // EntryAbility.onCreate 里没调过(理论上不会发生): 用页面 context 补一次。
      // setLogDir 会新开一个常驻 fd, 所以这里必须判断"没调过"才补, 不重复调用。
      console.warn('[AB] native crash guard not inited in onCreate — fallback init from page');
      BenchRunner.initNativeGuard(ctx.filesDir);
    }
    try {
      this.nGuardReady = BenchRunner.nativeGuardReady();
      this.nGuardPath = BenchRunner.nativeGuardPath();
    } catch (e) {
      this.nGuardReady = false;
      this.nGuardPath = '';
    }
    this.nGuardStatus = CrashGuard.get().guardStatusText();
    this.nGuardNote = CrashGuard.get().guardNotReadyLine();
  }

  // 启动自检(第 1 步): 从 native 取证装置读回上次留下的现场, 并立即清盘。
  // 非空 = 上次是进程内致命信号(SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT/SIGSYS)打死的;
  // 空串 = 没有记录(可能是被系统 SIGKILL / 正常结束 / 装置没就绪)。
  private readNativeReport(): void {
    let text: string = '';
    try {
      text = BenchRunner.takeNativeReport();
      CrashGuard.get().setNativeEvidence(text, BenchRunner.nativeGuardPath());
      if (text.length > 0) {
        // 读完立刻清: 避免下次启动把同一条记录再报一遍(全文已经在内存与「自诊断历史」里)
        BenchRunner.clearNativeReport();
      }
    } catch (e) {
      console.warn('[AB] takeNativeCrashReport failed');
      return;
    }
    this.nCrashFull = text;
    this.nCrashChars = text.length;
    this.nCrashFound = text.length > 0;
    this.nCrashIsCrash = CrashGuard.get().nativeLooksLikeCrash();
    this.nCrashSummary = CrashGuard.get().nativeEvidenceOneLine();
    if (!this.nCrashFound) {
      this.nCrashPreview = '';
      console.info('[AB] native crash evidence: none');
      return;
    }
    const head: string = this.nCrashIsCrash
      ? '[native 崩溃现场 · 来自取证装置]'
      : '[native 取证装置自检记录 · 非崩溃]';
    let body: string = text;
    if (body.length > this.nPreviewMax) {
      body = body.substring(0, this.nPreviewMax) + '\n…（已截断，全文 ' + text.length.toString() +
        ' 字符：点下面的「复制 native 现场全文」）';
    }
    this.nCrashPreview = head + '\n' + body;
    console.info('[AB] native crash evidence found: ' + CrashGuard.get().nativeEvidenceOneLine());
  }

  // 启动自检(第 2 步): 先把 native 现场落进「自诊断历史」(磁盘上的已被清掉),
  // 再读 ArkTS 面包屑, 让两条证据一起展示、互相印证。
  private async initCrashEvidence(): Promise<void> {
    try {
      if (this.nCrashFound) {
        await CrashGuard.get().pushLog(CrashGuard.get().nativePersistText());
      }
    } catch (e) {
      console.warn('[AB] native evidence persist failed');
    }
    try {
      await this.initStress();
    } catch (e) {
      console.warn('[AB] stress init failed');
    }
    try {
      const hit: CrashHit | null = await CrashGuard.get().detect();
      if (hit !== null) {
        this.crumbFound = true;
        this.crumbLine = hit.line;
        this.crumbDetail = hit.text + '设备：' + this.device + '\n' + this.nativeVer + '\n可用内存快照：' +
          (this.memSupported ? (this.memAvailMb.toString() + ' MB') : '未取到');
      } else {
        this.crumbFound = false;
        if (this.nCrashFound) {
          // 只有 native 现场、没有面包屑(崩在两项之间的胶水代码里, 或面包屑已被清): 也给 runlog 补一条 crash 行
          CrashGuard.get().logNativeOnly();
        }
      }
    } catch (e) {
      console.warn('[AB] crumb detect failed ' + e.message);
    }
    try {
      this.crumbLog = await CrashGuard.get().log();
    } catch (e) {
      console.warn('[AB] crash log read failed');
    }
  }

  // 横幅标题: 面包屑 / native 现场两个来源都可能单独存在, 标题要说清是哪种
  private crumbHeadline(): string {
    if (this.crumbFound) {
      return '上次运行异常结束';
    }
    if (this.nCrashFound && this.nCrashIsCrash) {
      return '上次运行留下了 native 崩溃现场';
    }
    return 'native 取证装置自检记录（非崩溃）';
  }

  // 复制 native 现场全文(横幅上的预览是截断过的)
  private async copyNativeEvidence(): Promise<void> {
    if (this.nCrashFull.length === 0) {
      return;
    }
    const text: string = '[Crush分 · native 崩溃现场（来自取证装置）]\n' +
      '证据文件：' + this.nGuardPath + '\n' +
      '（磁盘上的记录已在本次启动读取后清除）\n' +
      '----------------------------------------\n' + this.nCrashFull;
    try {
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.nCrashCopied = true;
    } catch (e) {
      this.nCrashCopied = false;
      console.warn('[AB] native evidence copy failed');
    }
  }

  // =========================================================================
  // 「关于」页 · 崩溃取证实测卡(排障用)
  // 按钮 1 只写一条自检记录(不崩); 按钮 2/3 会让本进程立刻退出(预期行为, 带二次确认)。
  // =========================================================================

  // 按钮 1: crashSelfTest(0) -> 立刻 takeNativeCrashReport() 读回并显示, 用来确认链路是通的
  private runSelfTestWrite(): void {
    this.stCopied = false;
    try {
      BenchRunner.nativeSelfTest(0);
    } catch (e) {
      this.stNote = 'crashSelfTest(0) 调用失败';
      return;
    }
    let text: string = '';
    try {
      text = BenchRunner.takeNativeReport();
    } catch (e) {
      this.stNote = '自检记录已写入，但读回失败（takeNativeCrashReport 抛错）';
      return;
    }
    this.stReport = text;
    if (text.length === 0) {
      this.stNote = '读了空串：链路没通。先看上面的装置状态（setLogDir 未成功时 native 只能写 stderr，落不到文件）';
    } else {
      this.stNote = '链路已通：自检记录 ' + text.length.toString() + ' 字符已从 ' +
        (this.nGuardPath.length > 0 ? this.nGuardPath : 'native_crash.txt') + ' 读回';
    }
  }

  // 按钮 2/3: 官方二次确认弹窗(写明后果), 确认后才真的触发崩溃
  private async confirmNativeSelfTest(kind: string, mode: number): Promise<void> {
    let confirmed: boolean = false;
    try {
      const res: promptAction.ShowDialogSuccessResponse = await this.getUIContext().getPromptAction().showDialog({
        title: '二次确认：触发 ' + kind,
        message: '点「确认触发（立即闪退）」后，App 会立刻闪退，这是预期行为（这一条自检就是故意把进程崩掉）。' +
          '\n重启 App 后，首页会显示崩溃横幅，里面就是取证装置写下的现场（信号 / 地址 / pc 是否落在我们自己的 .so 里 / ' +
          '当时标记的负载项 / 内存快照）。\n请先确认没有正在进行的跑分或稳定性压测。',
        buttons: [
          { text: '取消', color: SUB },
          { text: '确认触发（立即闪退）', color: DANGER }
        ]
      });
      confirmed = res.index === 1;
    } catch (e) {
      this.stNote = '确认弹窗打不开，已取消（没有触发崩溃）';
      console.warn('[AB] native self test dialog failed');
      return;
    }
    if (!confirmed) {
      this.stNote = '已取消：没有触发崩溃';
      return;
    }
    this.stNote = '已确认：马上触发 ' + kind + ' —— App 会立刻闪退，重启后首页横幅显示取证结果';
    console.info('[AB] native self test trigger: mode=' + mode.toString());
    BenchRunner.nativeSelfTest(mode);
  }

  private async copySelfTestReport(): Promise<void> {
    if (this.stReport.length === 0) {
      return;
    }
    try {
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.stReport);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.stCopied = true;
    } catch (e) {
      this.stCopied = false;
      console.warn('[AB] self test report copy failed');
    }
  }

  // =========================================================================
  // 运行日志(runlog.jsonl) —— 「关于」页的查看 / 复制 / 清空 + 可选联网上传
  //
  // 说明: 日志是旁路记录, 不参与任何测试项的计算、顺序与计分。
  // =========================================================================

  private refreshLogStats(): void {
    const st: RunLogStats = RunLog.get().stats();
    this.logSizeText = st.sizeText;
    this.logLines = st.lines;
    this.logFiles = st.files;
    this.logRangeText = st.rangeText;
    this.logOk = st.ok;
    this.logNote = st.note;
    // 只取末尾 1500 字符做预览, 不把整个文件塞进 UI
    this.logTail = RunLog.get().tailText(RUNLOG_TAIL);
    // 「最近一次发送」那一行: 从盘上的 upload 行读回(重启后也看得到)
    if (this.uploadLastResult.length === 0) {
      this.uploadLastResult = RunLog.get().lastUploadLineText();
    }
    // 永久账本与上传设置一起刷新
    this.refreshHistoryStats();
  }

  // 永久账本(runhistory.jsonl): 条数 / 时间跨度 / 文件大小
  private refreshHistoryStats(): void {
    // 一次读盘同时拿统计与最近 3 条(读盘只发生在这里, 不放进 @Builder)
    const snap: RunHistorySnapshot = RunLog.get().historySnapshot(3);
    const h: RunHistoryStats = snap.stats;
    this.histLines = h.lines;
    this.histSizeText = h.sizeText;
    this.histRangeText = h.rangeText;
    this.histFiles = h.files;
    const out: string[] = [];
    for (const l of snap.recent) {
      out.push(RunLog.get().historyLineText(l));
    }
    this.histPreview = out.reverse().join('\n');
  }

  private logStatusText(): string {
    if (this.logNote.length > 0) {
      return '写入状态：异常 —— ' + this.logNote;
    }
    if (!this.logOk) {
      return '写入状态：异常';
    }
    return '写入状态：正常';
  }

  private logCopyPreviewText(): string {
    return '复制内容 = 设备 / App 版本 / Native 版本 / 时间范围 / 写入状态 + 日志末尾 ' +
      RUNLOG_TAIL.toString() + ' 字符（自带上下文，直接粘回即可用）';
  }

  private async copyRunLog(): Promise<void> {
    const text: string = RunLog.get().copyText();
    try {
      // pasteboard 不需要 context, 直接用系统剪贴板(避免再引入一处已废弃的 getContext)
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.logCopied = true;
      this.logStatus = '已复制 ' + text.length.toString() + ' 字符到剪贴板';
    } catch (e) {
      this.logCopied = false;
      this.logStatus = '复制失败';
      console.warn('[AB] runlog copy failed');
    }
  }

  private armLogClear(): void {
    this.logClearArm = true;
  }

  private cancelLogClear(): void {
    this.logClearArm = false;
  }

  private confirmLogClear(): void {
    const ok: boolean = RunLog.get().clear();
    this.logClearArm = false;
    this.logCopied = false;
    this.logStatus = ok ? '日志已清空（含 runlog.1 ~ runlog.3 历史文件）' : '清空失败';
    this.refreshLogStats();
  }

  // ---- 上传设置(默认关闭; 地址为空 = 完全不上传) ----

  private onUploadUrlChange(v: string): void {
    this.uploadUrl = v;
    LogUpload.get().setUrl(v);
    this.uploadNote = v.length === 0
      ? '地址为空：完全不上传任何数据。'
      : '地址已保存，开关打开后每轮跑分结束会自动 POST 一次。';
  }

  private onUploadSwitch(on: boolean): void {
    this.uploadOn = on;
    LogUpload.get().setEnabled(on);
    this.uploadNote = !on ? '已关闭：不会产生任何网络请求。'
      : (this.uploadUrl.length === 0 ? '开关已打开，但地址为空 —— 仍然不会上传。'
        : '已打开：每轮跑分结束后会 POST 一次日志。');
  }

  // 接口说明(折叠): 就一次 JSON POST, 写给"自己搭个接收端"的人看
  private uploadInterfaceText(): string {
    let s: string = '【发送方式】\n';
    s += '· 只有一种形态：一次 HTTP POST，Content-Type: application/json，body 是下面的 JSON；\n';
    s += '· 连接 / 读取超时各 10 秒；不重试；失败只在关于页留一行结果 + runlog 记一行 upload；\n';
    s += '· 地址留空 = 一个请求都不发；开关只控制"每轮跑分结束要不要自动发"。\n\n';
    s += '【报文字段】\n';
    s += '  schema        固定 "aurora-runlog/1"\n';
    s += '  uploadedAt    发送时间（epoch ms）\n';
    s += '  device        传播名（型号码）· 系统 · API，例如 HUAWEI Pura X Max（HOP-AL00）· OpenHarmony-7.0.0.105 · API 26\n';
    s += '  appVersion / nativeVersion\n';
    s += '  range         本次发送范围（最近一轮 / 最近一轮 + 崩溃记录 / 最近 N 条永久记录）\n';
    s += '  fileBytes / fileLines / timeRange   runlog 的大小、行数、时间跨度\n';
    s += '  lastRunId / lastRunCount / lastRun[]  最近一轮的原始行（每行一个 JSON 字符串）\n';
    s += '  lastCrash     最近一次崩溃行（没有则空串）\n';
    s += '  historyCount / history[]  永久记录行（范围选"最近 N 条永久记录"时才有）\n';
    s += '  note          说明本请求由用户主动触发\n\n';
    s += '【怎么试】\n';
    s += '填好地址 → 点「立即发送一次」（不需要先打开开关）→ 看下面"最近一次发送"那行的 HTTP 码；\n';
    s += '想先看报文长什么样，可以直接「导出日志文件」或「复制日志」对照字段。\n\n';
    s += '【权限】\n';
    s += '只用到 ohos.permission.INTERNET（普通权限，系统自动授予、不弹窗），不涉及任何敏感权限。';
    return s;
  }

  private async uploadLogNow(): Promise<void> {
    if (this.uploadBusy) {
      return;
    }
    if (LogUpload.get().url().length === 0) {
      this.uploadNote = '地址为空：本应用不会发送任何数据（先填好接收地址再点这个按钮）。';
      return;
    }
    this.uploadBusy = true;
    // manual = true: 立即发送一次不要求先打开自动发送开关
    const res: UploadResult = await RunLog.get().uploadNow(true);
    this.uploadBusy = false;
    this.uploadNote = (res.ok ? '发送成功' : '发送失败') + '：' + res.note +
      '（HTTP ' + res.code.toString() + ' · ' + res.ms.toString() + ' ms）';
    this.uploadLastResult = stampText() + ' · ' + this.uploadNote;
    this.refreshLogStats();
  }

  // ---- 发送范围(会落盘, 下次启动沿用) ----

  private onUploadRange(id: string): void {
    this.uploadRange = id;
    LogUpload.get().setRange(id);
    this.uploadNote = '发送范围已切换为「' + LogUpload.get().rangeLabel() + '」' +
      (id === UPLOAD_RANGE_HISTORY_N ? '（条数可在下面填）' : '');
  }

  private onUploadHistN(v: string): void {
    const n: number = parseInt(v, 10);
    if (isNaN(n) || n <= 0) {
      return;
    }
    this.uploadHistN = n;
    LogUpload.get().setHistoryN(n);
    this.uploadNote = '「最近 N 条永久记录」的 N 已设为 ' + LogUpload.get().historyCount().toString();
  }

  // ---- 导出文件(@ohos.file.picker 的 DocumentViewPicker) ----
  // 成功: 由用户在文件选择器里指定位置, 文件管理器可见(便于发给别人/留档);
  // 失败或取消: 降级为"复制全文到剪贴板", 并给出沙箱路径(hdc file recv 也能取走)。
  private async exportFile(kind: string): Promise<void> {
    const isHistory: boolean = kind === 'history';
    const text: string = isHistory ? RunLog.get().historyExportText() : RunLog.get().copyText();
    const name: string = exportFileName(isHistory);
    if (this.appCtx === null) {
      await this.exportFallback(kind, text, '页面 context 不可用');
      return;
    }
    let uri: string = '';
    try {
      const doc = new picker.DocumentViewPicker(this.appCtx);
      const opt = new picker.DocumentSaveOptions();
      opt.newFileNames = [name];
      const uris: string[] = await doc.save(opt);
      if (uris.length === 0) {
        await this.exportFallback(kind, text, '没有选择保存位置，可能已取消');
        return;
      }
      uri = uris[0];
    } catch (e) {
      console.warn('[AB] document picker failed');
      await this.exportFallback(kind, text, '文档选择器打不开');
      return;
    }
    if (RunLog.get().writeToUri(uri, text)) {
      const msg: string = '已导出 ' + text.length.toString() + ' 字符 → ' + name + '（位置由你在文件选择器里指定）';
      if (isHistory) {
        this.histStatus = msg;
      } else {
        this.logStatus = msg;
      }
      return;
    }
    await this.exportFallback(kind, text, '写入目标文件失败');
  }

  private async exportFallback(kind: string, text: string, why: string): Promise<void> {
    let copied: boolean = false;
    try {
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      await pasteboard.getSystemPasteboard().setData(paste);
      copied = true;
    } catch (e) {
      copied = false;
    }
    const sandbox: string = kind === 'history' ? RunLog.get().historyPath() : RunLog.get().mainPath();
    const msg: string = '导出未完成（' + why + '）：' +
      (copied ? '已改为把全文复制到剪贴板，粘贴保存即可。' : '复制到剪贴板也没成功。') +
      '沙箱内原始文件：' + sandbox + '（开发者可用 hdc file recv 取走）';
    if (kind === 'history') {
      this.histStatus = msg;
    } else {
      this.logStatus = msg;
    }
  }

  // ---- 版本号连点 5 次: 展开排障工具(崩溃取证实测) ----
  // 默认不可见: 这三个按钮里有两个会故意让进程崩掉, 平时不该出现在界面上(防误触)。
  private onVersionTap(): void {
    if (this.showDebugTools) {
      return;
    }
    this.verTaps++;
    if (this.verTaps >= 5) {
      this.showDebugTools = true;
      this.verHint = '';
      console.info('[AB] debug tools unlocked by 5 taps on version');
      return;
    }
    this.verHint = this.verTaps >= 3 ? ('再连点 ' + (5 - this.verTaps).toString() + ' 次可展开排障工具') : '';
  }

  // ---- 每轮跑分结束后的汇总行 ----

  private scoredCount(list: Gb7Result[]): number {
    let n = 0;
    for (const r of list) {
      if (r.score > 0) {
        n++;
      }
    }
    return n;
  }

  // 综合分口径与各卡片一致(已计分项且通过跨芯片变化自检的几何平均)。
  // 走 FullReportBuilder.geoOf —— 与报告正文 / JSON / 结果页卡片是同一个入口,
  // 所以卡片上的数与小节 note / 报告里的数不可能各说各话(见 FullRun.ets 的 geoOf 注释)。
  private logGeo(list: Gb7Result[]): number {
    return FullReportBuilder.geoOf(list);
  }

  // ---- 历史成绩.txt 那一行(人能直接看的那份成绩单) ---------------------------
  // 口径与报告**同一处来源**: CS1 三列就是报告 composite 里的 gb8*Composite(都由
  // FullReportBuilder.geoOf 算), 自研三列与总分就是首页那三个大数字与总分。
  // 这里不做任何再加工 —— 多算一套就会多出一处"两份记录对不上"的可能。
  // 成绩表里的设备列: 只要"能认出是哪台机器"就够了(传播名 + 型号码)。
  // 不用 BenchRunner.deviceLabel() —— 那一串是给日志/排障用的(几百字节, 还带逐核频率),
  // 放进表格里既占地方, 又会让这一列被截成一个省略号。
  private shortDevice(): string {
    try {
      const d: DeviceProfileData = DeviceProfile.get().data();
      let s: string = d.marketingName.length > 0 ? d.marketingName : '未知设备';
      if (d.modelCode.length > 0 && d.modelCode !== d.marketingName) {
        s += '（' + d.modelCode + '）';
      }
      return s;
    } catch (e) {
      return this.device.length > 0 ? this.device : '未知设备';
    }
  }

  private makeHistoryRow(out: FullRunOutcome, durationMs: number): ScoreHistoryRow {
    return {
      runId: RunLog.get().currentRunId(),
      ts: out.startedAt > 0 ? out.startedAt : Date.now(),
      device: this.shortDevice(),
      // 写这一行的 App 版本(单一来源: RunLog 的 APP_VER)。表格里靠它标出口径变更点 ——
      // 「CS1 多核」那一列在 8.2 之前参与项集合不一样, 不能跨版本比。
      appVer: 'Crush分 ' + APP_VER,
      gb8Single: FullReportBuilder.geoOf(out.gb7Single),
      gb8Multi: FullReportBuilder.geoOf(out.gb7Multi),
      gb8Gpu: FullReportBuilder.geoOf(out.gb7Gpu),
      ownSingle: out.own !== null ? out.own.single : 0,
      ownMulti: out.own !== null ? out.own.multi : 0,
      ownGpu: out.own !== null ? out.own.gpu : 0,
      ownTotal: out.own !== null ? out.own.total : 0,
      durationMs: durationMs,
      note: this.fullRunNote(out)
    };
  }

  // 备注列: 只放必要的异常, 一切正常就留空(空串)。
  // 崩溃写不进来是天然的: 进程被 SIGKILL / SIGSEGV 时这一行根本没机会写 —— 那种情况由
  // 报告里的崩溃取证与 runlog 的 crash 行负责, 不在这里编一个"崩溃"。
  private fullRunNote(out: FullRunOutcome): string {
    const parts: string[] = [];
    if (out.interrupted) {
      parts.push('未跑完');
    }
    let secFail: number = 0;
    for (const s of out.sections) {
      if (s.status === ST_FAILED) {
        secFail++;
      }
    }
    if (secFail > 0) {
      parts.push(secFail.toString() + ' 节失败');
    }
    const bad: number = this.fullRunBadCount(out);
    if (bad > 0) {
      parts.push(bad.toString() + ' 项没拿到分');
    }
    return parts.join(' · ');
  }

  // 这一轮有多少项没拿到分(CS1 的计分项 + 自研套件的项)。只数数, 不改任何分数。
  private fullRunBadCount(out: FullRunOutcome): number {
    let bad: number = 0;
    for (const r of out.gb7Single) {
      if (r.score <= 0 && r.scoredByPolicy) {
        bad++;
      }
    }
    for (const r of out.gb7Multi) {
      if (r.score <= 0 && r.scoredByPolicy) {
        bad++;
      }
    }
    for (const r of out.gb7Gpu) {
      if (r.score <= 0) {
        bad++;
      }
    }
    if (out.own !== null) {
      for (const t of out.own.tests) {
        if (t.score <= 0) {
          bad++;
        }
      }
      for (const t of out.own.multiTests) {
        if (t.score <= 0) {
          bad++;
        }
      }
    }
    return bad;
  }

  // crashes = 该轮相关的崩溃次数(只有稳定性压测会话有值; 其它阶段为 0)。
  // 汇总行写 runlog, 同时往永久账本 runhistory.jsonl 追加一行。
  // extra: 可选的账本扩展字段(只有「一键全跑」那一行填; 见 RunHistoryExtra)。
  //   它是**新增**的可选键, 不填时账本行的形状与以前逐字一致。
  private logRunSummary(phase: string, composite: number, scored: number, skipped: number,
    failed: number, durationMs: number, crashes: number = 0, threads: number = 0,
    extra?: RunHistoryExtra): void {
    // 汇总行是 runlog 里唯一带"设备行"的地方 —— 写它之前先把窗口尺寸补齐(见 ensureWindowKnown)
    this.ensureWindowKnown();
    const th: number = threads > 0 ? threads : 0;
    const s: RunSummaryInput = {
      phase: phase,
      composite: composite,
      scored: scored,
      skipped: skipped,
      failed: failed,
      durationMs: durationMs,
      availMemMB: this.memSupported ? this.memAvailMb : 0,
      thermal: this.thermalText(),
      crashes: crashes,
      // 多核阶段才带线程数与 SMT 口径(单核/GPU/CoreMark 阶段传 0, 汇总行里写 0 / 空串)
      threads: th,
      smt: th > 1 ? BenchRunner.smtSummaryFlag(th) : ''
    };
    if (extra !== undefined) {
      s.extra = extra;
    }
    RunLog.get().summary(s);
    // 账本一行就好了, 顺手刷新一下「关于」页的显示(不阻塞)
    this.refreshHistoryStats();
  }

  private refreshBest(): void {
    let b = 0;
    for (const r of this.records) {
      if (r.total > b) {
        b = r.total;
      }
    }
    this.best = b;
  }

  // =========================================================================
  // 「一键全部跑分」—— 编排 + 报告(实现全在 ../service/FullRun.ets)
  //
  // 按一次: 自研套件(单核+多核+GPU) -> CS1 单核 -> CS1 多核 -> CS1 GPU -> NPU -> 存储 I/O,
  // 跑完自动生成一份完整报告并落盘(files/report-<时间戳>.txt|.json + report-latest.*)。
  //
  // 性能护栏(硬性):
  //   · 跑分期间没有任何持续动画: 进度页只有静态文字 + 用两个静态色块拼出来的
  //     "确定值进度条"(value/total), 没有转圈、没有补间、没有 .animation()、
  //     也没有 Progress 组件自带的平滑动效(干脆不用 Progress 组件);
  //   · UI 更新频率不高于"每项一次": 编排器的回调就是每完成一项一次;
  //   · 整轮保持亮屏: keepScreen(true) / keepScreen(false) 把整轮包夹起来;
  //   · 有「停止」按钮: 按下后当前项跑完即收工, 已得结果照常出报告并注明未跑完。
  //
  // 口径: 本入口只做编排 + 界面 + 报告, 不改 k / conv / 计分公式 / metric 口径 / 单位 /
  //       任何负载的工作量与尺寸 / 线程数与绑核策略。NPU 与存储两个小节在 native 侧就是
  //       scored=false, 单独展示, 不并入任何复合分。
  // =========================================================================

  private fullUiFacts(): FullRunUiFacts {
    const facts: FullRunUiFacts = {
      appVersion: APP_LABEL,
      device: this.device,
      nativeVersion: this.nativeVer,
      memText: this.memSupported
        ? (this.memAvailMb.toString() + ' MB 可用 / ' + this.memTotalMb.toString() + ' MB 总量')
        : this.memNote,
      thermalText: this.thermalText(),
      crashBannerFound: this.crumbFound,
      crashBannerLine: this.crumbLine,
      nativeCrashFound: this.nCrashFound,
      nativeCrashChars: this.nCrashChars,
      nativeCrashSummary: this.nCrashSummary
    };
    return facts;
  }

  // 编排器每次回调 = 每完成一项一次(性能护栏 2)
  private onFullProgress(p: FullRunProgress): void {
    this.fullSectionName = p.sectionTitle;
    this.fullSectionIdx = p.sectionIndex;
    this.fullSectionCount = p.sectionTotal;
    this.fullDoneItems = p.doneItems;
    this.fullTotalItems = p.totalItems;
    this.fullDoneSections = p.doneSections;
    if (p.itemName.length > 0) {
      this.fullItemName = p.itemName;
    }
    this.fullItemIdx = p.itemIndex;
    this.fullItemTotal = p.itemTotal;
    if (p.line.length > 0) {
      let list: string[] = this.fullLines.concat([p.line]);
      if (list.length > FULL_LINE_KEEP) {
        list = list.slice(list.length - FULL_LINE_KEEP);
      }
      this.fullLines = list;
    }
    if (p.sectionDone) {
      // sectionDone 事件里 itemIndex 就是"本节实际完成的项数"(编排器传的是 sec.itemDone)
      this.fullSectionLines = this.fullSectionLines.concat([
        p.sectionIndex.toString() + '. ' + p.sectionTitle + ' · 完成 ' +
        p.itemIndex.toString() + '/' + p.itemTotal.toString() + ' 项 · ' + p.sectionNote
      ]);
    }
  }

  private fullProgressPercent(): string {
    if (this.fullTotalItems <= 0) {
      return '0%';
    }
    let pct: number = Math.floor(this.fullDoneItems * 100 / this.fullTotalItems);
    if (pct < 0) {
      pct = 0;
    }
    if (pct > 100) {
      pct = 100;
    }
    return pct.toString() + '%';
  }

  private fullProgressText(): string {
    return '第 ' + this.fullSectionIdx.toString() + '/' + this.fullSectionCount.toString() +
      ' 节 · ' + this.fullDoneItems.toString() + '/' +
      (this.fullTotalItems > 0 ? this.fullTotalItems.toString() : '?') + ' 项';
  }

  private async startFullRun(): Promise<void> {
    if (this.fullRunning || this.anyBenchRunning()) {
      return;
    }
    this.fullRunning = true;
    this.fullStopArmed = false;
    this.fullSectionName = '准备中';
    this.fullSectionIdx = 0;
    this.fullSectionCount = 7;
    this.fullItemName = '初始化负载';
    this.fullItemIdx = 0;
    this.fullItemTotal = 0;
    this.fullDoneItems = 0;
    this.fullTotalItems = 0;
    this.fullDoneSections = 0;
    this.fullLines = [];
    this.fullSectionLines = [];
    this.fullOutcome = null;
    this.fullReportOpen = false;
    this.fullReportPreview = '';
    this.fullReportChars = 0;
    this.fullReportPaths = '';
    this.fullReportStatus = '';
    this.fullReportCopied = false;
    // 落盘状态也一起重置: 它是"这一次跑完到底写没写进盘"的显示依据(见 reportFileCard)
    this.fullReportSaved = true;
    // 护栏 3: 整轮保持亮屏
    this.keepScreen(true);
    // 「下载」目录: 报告会写到 <下载>/<包名>/report-时间戳.txt,
    // 也就是系统「文件管理 → 下载」里显示的那个以应用名命名的文件夹(下载/Crush分)。
    // 手机上没有可申请的公共下载目录权限(READ_WRITE_DOWNLOAD_DIRECTORY 只支持
    // PC/2in1 | 平板; getUserDownloadDir() 在手机上返回 801), 所以走官方「下载模式」:
    // 首次跑完会自动保存并只弹一次系统确认, 之后就是静默写入。
    this.dlPath = '';
    this.dlFiles = [];
    this.dlHistoryStatus = '';
    const shown: string = ReportSink.dirShown();
    this.dlStatus = '跑完会自动把报告写进「' + shown +
      '」（文件管理里显示的就是这个名字）。首次可能会弹一次系统保存确认，之后不再打扰。';
    // 窗口尺寸尽量补齐(runlog 设备行与报告里的设备画像都要它); 不阻塞, 失败也没关系
    this.ensureWindowKnown();
    this.refreshMem();
    const t0: number = Date.now();
    const session: FullRunSession = new FullRunSession();
    this.fullSession = session;
    let out: FullRunOutcome | null = null;
    let orchestratorError: string = '';
    try {
      out = await session.run((p: FullRunProgress) => {
        this.onFullProgress(p);
      });
    } catch (e) {
      // 编排层自身抛错(每节都有 catch, 这里只是兜底): 也要出一份报告
      orchestratorError = e.message;
    }
    this.fullSession = null;
    this.fullRunning = false;
    this.keepScreen(false);
    if (out === null) {
      out = new FullRunOutcome();
      out.startedAt = t0;
      out.finishedAt = Date.now();
      out.interrupted = true;
      out.stopReason = '编排层出现未预期的异常：' + orchestratorError;
    }
    // 报告生成 + 分块落盘
    const facts: FullRunUiFacts = this.fullUiFacts();
    // 历史成绩.txt: 整轮已经跑完(所有计时区间都结束了), 在这里写一次。
    // 放在报告生成**之前**, 是为了让它的落盘结果能进报告正文的「落盘回执」一段(失败也印, 不静默)。
    // 它只读本轮结果: 不跑任何负载、不进任何计时区间、不影响任何分数。
    const histRow: ScoreHistoryRow = this.makeHistoryRow(out, Date.now() - t0);
    let hist: ScoreHistoryResult = ScoreHistory.get().append(histRow);
    out.scoreHistoryStatus = ScoreHistory.describe(hist);
    this.dlHistoryStatus = out.scoreHistoryStatus;
    let save: FullReportSaveResult | null = null;
    try {
      save = await FullReportStore.get().save(out, facts);
    } catch (e) {
      out.reportSaveError = '报告生成/落盘异常：' + e.message;
    }
    this.fullOutcome = out;
    this.fullReportChars = out.reportChars;
    if (save === null) {
      this.fullReportSaved = false;
      this.fullReportPaths = '报告落盘异常：' + out.reportSaveError;
    } else if (save.ok) {
      this.fullReportSaved = true;
      this.fullReportPaths = save.textPath + '  |  ' + save.jsonPath;
    } else {
      this.fullReportSaved = false;
      this.fullReportPaths = '落盘不完整：' + save.err + '（沙箱内：' +
        FullReportStore.get().latestTextPath() + '）';
    }
    // 三层呈现: 第一层三个大数字 / 第二层每个小节一行 / 第三层逐项明细(默认收起)。
    // 在这里算一次就好 —— 纯数据, 不参与任何渲染循环, 跑分期间零开销。
    this.buildFullLayers(out);
    this.fullReportPreview = FullReportStore.get().loadLatestText(FULL_REPORT_PREVIEW);
    // 「下载」目录: 报告自动写入 <下载>/Crush分/report-YYYYMMDD-HHMMSS.txt
    // 写不进去时这里会得到一条用户可见的真实原因, 不静默失败。
    try {
      await this.saveReportToDownloads(
        FullReportStore.get().hasReport() ? FullReportStore.get().text() : this.fullReportPreview,
        out.startedAt);
    } catch (e) {
      this.dlStatus = '写入「下载」目录时出错：' + e.message +
        ' —— 可在结果页点「导出到文件」手动保存一次。';
    }
    // 报告落盘那一步可能刚拿到「下载」目录的写权限 —— 上一次没写成就补试一次。
    // 幂等由 ScoreHistory 自己保证(同一次跑分只写一行), 所以这里补试不可能写出第二行。
    if (!hist.ok && !hist.duplicate) {
      const again: ScoreHistoryResult = ScoreHistory.get().append(histRow);
      if (again.ok) {
        this.dlHistoryStatus = ScoreHistory.describe(again) + '（报告落盘时拿到了写权限，补写成功）';
      } else {
        this.dlHistoryStatus = ScoreHistory.describe(again);
      }
    }
    this.fullReportOpen = true;
    // ④ 粒子: 结果出现, 屏幕正中聚拢
    this.fxResultBurst();
    this.fullReportStatus = out.interrupted
      ? '本轮未跑完（中断），报告已按"已得结果 + 未跑完"生成。'
      : '全部小节跑完，报告已生成。';
    this.fullHasOldReport = FullReportStore.get().hasLatestOnDisk();
    this.fullOldReportBytes = FullReportStore.get().latestTextBytes();
    // 运行日志(旁路): 一键全跑的整轮汇总一行(不改任何数值与计分)
    // 8.4 起这一行额外带上 CS1 三个复合分 / 自研套件三个分 / runId(只新增键, 老键名与含义不变),
    // 于是账本与「历史成绩.txt」记的是同一批数。
    const badCount: number = this.fullRunBadCount(out);
    const hex: RunHistoryExtra = {
      runId: histRow.runId,
      startedAt: histRow.ts,
      interrupted: out.interrupted,
      gb8Single: histRow.gb8Single, gb8Multi: histRow.gb8Multi, gb8Gpu: histRow.gb8Gpu,
      ownSingle: histRow.ownSingle, ownMulti: histRow.ownMulti, ownGpu: histRow.ownGpu,
      // GPU-SNL / NPU / 存储: 历史卡片下半部分那三行要用它们(2026-10-06 追加的两个/三个键)。
      //  取值与结果页的关键得分清单**逐字同源**: 同一个字段 + 同一套"没跑成就算没拿到"的判断
      //  (见 common/ResultLayers.ets 的 buildKeyRows: sn.ok && prepError 为空 / npuRun.ok / storage.ok),
      //  所以卡片上那个数与结果页、与报告里那个数不可能对不上; 没跑成的一律记 0 -> 卡片写 '--'。
      snlScore: (out.gpuSnl !== null && out.gpuSnl.ok && out.gpuSnl.prepError.length === 0)
        ? out.gpuSnl.score : 0,
      npuGops: (out.npuRun !== null && out.npuRun.ok) ? out.npuRun.goPs : 0,
      storeWriteMbps: (out.storage !== null && out.storage.ok) ? out.storage.seqWriteDurableMbps : 0
    };
    this.logRunSummary('一键全跑', out.own !== null ? out.own.total : 0,
      out.itemCount(), 0, badCount, Date.now() - t0, 0, 0, hex);
    // 「历史」页的数据与账本同源: 这一轮已经进了账本, 顺手让它读一次(切到那一页时也会再读)
    this.reloadHistory();
    // 自研套件那一节的记录与首页「单次完整跑分」同一份数据: 结果照常进历史与最高分
    if (out.own !== null) {
      const rec: BenchRecord = out.own;
      BenchStore.get().append(rec).then((list: BenchRecord[]) => {
        this.records = list;
        this.refreshBest();
      }).catch((e: Error) => {
        console.warn('[AB] full run save failed ' + e.message);
      });
    }
  }

  // 「停止」: 只置标志, 当前项跑完后在项与项 / 节与节之间收工(不强杀正在跑的负载)
  private stopFullRun(): void {
    if (!this.fullRunning) {
      return;
    }
    this.fullStopArmed = true;
    this.fullReportStatus = '已请求停止：当前这一项跑完后收工，已得结果照常出报告。';
    if (this.fullSession !== null) {
      this.fullSession.stop();
    }
  }

  private closeFullReport(): void {
    this.fullReportOpen = false;
  }

  private async copyFullReport(): Promise<void> {
    const text: string = FullReportStore.get().hasReport()
      ? FullReportStore.get().text() : this.fullReportPreview;
    if (text.length === 0) {
      this.fullReportStatus = '还没有报告，先点「一键全部跑分」。';
      return;
    }
    try {
      // pasteboard 不需要 context, 直接用系统剪贴板
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.fullReportCopied = true;
      this.fullReportStatus = '已复制全文 ' + text.length.toString() + ' 字符到剪贴板';
    } catch (e) {
      this.fullReportCopied = false;
      this.fullReportStatus = '复制失败';
      console.warn('[AB] full report copy failed');
    }
  }

  // 导出: 与「关于」页的日志导出同一套方式(@ohos.file.picker 的 DocumentViewPicker)
  private async exportFullReport(): Promise<void> {
    const text: string = FullReportStore.get().hasReport()
      ? FullReportStore.get().text() : this.fullReportPreview;
    if (text.length === 0) {
      this.fullReportStatus = '还没有报告，先点「一键全部跑分」。';
      return;
    }
    const name: string = FullReportStore.get().exportName();
    if (this.appCtx === null) {
      await this.fullExportFallback(text, '页面 context 不可用');
      return;
    }
    let uri: string = '';
    try {
      const doc = new picker.DocumentViewPicker(this.appCtx);
      const opt = new picker.DocumentSaveOptions();
      opt.newFileNames = [name];
      const uris: string[] = await doc.save(opt);
      if (uris.length === 0) {
        await this.fullExportFallback(text, '没有选择保存位置，可能已取消');
        return;
      }
      uri = uris[0];
    } catch (e) {
      console.warn('[AB] full report picker failed');
      await this.fullExportFallback(text, '文档选择器打不开');
      return;
    }
    if (RunLog.get().writeToUri(uri, text)) {
      this.fullReportStatus = '已导出 ' + text.length.toString() + ' 字符 → ' + name +
        '（位置由你在文件选择器里指定）';
      return;
    }
    await this.fullExportFallback(text, '写入目标文件失败');
  }

  private async fullExportFallback(text: string, why: string): Promise<void> {
    let copied: boolean = false;
    try {
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      await pasteboard.getSystemPasteboard().setData(paste);
      copied = true;
    } catch (e) {
      copied = false;
    }
    this.fullReportStatus = '导出未完成（' + why + '）：' +
      (copied ? '已改为把全文复制到剪贴板，粘贴保存即可。' : '复制到剪贴板也没成功。') +
      '沙箱内原始文件：' + FullReportStore.get().latestTextPath() + '（开发者可用 hdc file recv 取走）';
  }

  // 查看盘上上一次的报告(进程被杀 / 重启之后也拿得到)
  private openLatestReport(): void {
    const text: string = FullReportStore.get().loadLatestText(FULL_REPORT_PREVIEW);
    if (text.length === 0) {
      this.fullReportStatus = '盘上没有找到 report-latest.txt。';
      return;
    }
    this.fullReportPreview = text;
    if (this.fullOutcome !== null) {
      // 本进程里跑过一键全跑: 三层数据重新算一遍(纯函数, 只算不改任何数值)
      this.buildFullLayers(this.fullOutcome);
    }
    this.fullReportPaths = FullReportStore.get().latestTextPath() + '  |  ' +
      FullReportStore.get().latestJsonPath();
    this.fullReportOpen = true;
    // ④ 粒子: 结果出现(载入盘上最近一次报告), 屏幕正中聚拢
    this.fxResultBurst();
    this.fullReportStatus = '已载入盘上最近一次报告（' +
      FullReportStore.get().latestTextBytes().toString() + ' 字节）。';
  }

  // 报告浮层用的小取值器(避免在 @Builder 里对可空对象做窄化访问)
  private fullInterruptedFlag(): boolean {
    return this.fullOutcome !== null && this.fullOutcome.interrupted;
  }

  private fullOutcomeItems(): number {
    return this.fullOutcome !== null ? this.fullOutcome.itemCount() : 0;
  }

  private fullOutcomeElapsedText(): string {
    if (this.fullOutcome === null) {
      return '—';
    }
    const ms: number = this.fullOutcome.elapsedMs();
    const sec: number = Math.floor(ms / 1000);
    return sec >= 60
      ? (Math.floor(sec / 60).toString() + ' 分 ' + (sec % 60).toString() + ' 秒')
      : (sec.toString() + ' 秒');
  }

  private fullOutcomeSectionsText(): string {
    if (this.fullOutcome === null) {
      return '—';
    }
    let done = 0;
    let failed = 0;
    let skipped = 0;
    for (const s of this.fullOutcome.sections) {
      if (s.status === '已完成') {
        done++;
      } else if (s.status === '失败') {
        failed++;
      } else if (s.status === '已跳过（停止后未执行）') {
        skipped++;
      }
    }
    return '完成 ' + done.toString() + ' · 失败 ' + failed.toString() + ' · 未执行 ' +
      skipped.toString() + ' / 共 ' + this.fullOutcome.sections.length.toString() + ' 节';
  }

  private startRun(): void {
    if (this.running) {
      return;
    }
    this.running = true;
    this.live = [];
    this.doneCount = 0;
    this.totalCount = 0;
    this.curMs = 0;
    this.curScore = 0;
    this.phase = '准备中';
    this.curTest = '初始化负载';
    this.gpuActive = false;
    this.gpuScene = '';
    this.gpuFps = 0;
    this.gpuSceneIdx = 0;
    this.gpuSceneTotal = 0;
    this.keepScreen(true);
    const t0: number = Date.now();
    BenchRunner.runAll((p: ProgressInfo) => {
      this.phase = p.phase;
      this.doneCount = p.index;
      this.totalCount = p.total;
      this.curTest = p.testName;
      this.curMs = p.lastMs;
      this.curScore = p.lastScore;
      this.live = this.live.concat([{
        name: p.testName,
        detail: p.phase,
        ms: p.lastMs,
        score: p.lastScore,
        metric: 0,
        unit: '',
        parallelism: 1,
        // 进度用的占位行: 这些取证字段本轮还没有读数, 一律按"未上报"填
        // (真正的值由 BenchRunner.one() 从 native 读回, 见 model/BenchModel.ets)
        cpu: -1,
        cpuAtStart: -1,
        cpuMaxKhz: 0,
        cpuRank: -1,
        cpuBound: false,
        cpuInFastCluster: false,
        cpuInMachineTopTier: false,
        cpuFastClusterCores: 0,
        cpuFastClusterMaxKhz: 0,
        cpuMachineTopTierCores: 0,
        cpuMachineTopTierKhz: 0,
        cpuAllowedCount: 0,
        cpuSingleTargetCores: 0,
        cpuInfo: '',
        runFreq: ''
      }]);
    }, (g: GpuProgress) => {
      this.gpuActive = true;
      this.phase = 'GPU 渲染 3D';
      this.gpuScene = g.sceneName;
      this.gpuFps = g.fps;
      this.gpuSceneIdx = g.sceneIndex + 1;
      this.gpuSceneTotal = 4;
      this.curTest = g.sceneName;
      this.curMs = g.frameMs;
      this.curScore = g.fps;
      this.doneCount = g.framesDone;
      this.totalCount = g.framesTarget;
    }).then((rec: BenchRecord) => {
      this.running = false;
      this.keepScreen(false);
      this.shown = rec;
      // ② 粒子: 收工聚拢回「开始跑分」圆心(fxOn() 此时已经重新为真)
      this.fxFinishBurst();
      // 单次完整跑分的结果浮层也用同一套「三层呈现」的第一层 / 第二层
      this.buildResLayers(rec);
      // 运行日志(旁路): 自研套件(CPU 单核/多核 + GPU 场景)整轮汇总, 只写文件不影响任何计分
      let ownScored = 0;
      let ownFailed = 0;
      for (const t of rec.tests) {
        if (t.score > 0) {
          ownScored++;
        } else {
          ownFailed++;
        }
      }
      for (const g of rec.gpuScenes) {
        if (g.score > 0) {
          ownScored++;
        } else {
          ownFailed++;
        }
      }
      this.logRunSummary(PH_OWN, rec.total, ownScored, 0, ownFailed, Date.now() - t0);
      BenchStore.get().append(rec).then((list: BenchRecord[]) => {
        this.records = list;
        this.refreshBest();
      }).catch((e: Error) => {
        console.warn('[AB] save failed ' + e.message);
      });
    }).catch((e: Error) => {
      this.running = false;
      this.keepScreen(false);
      this.curTest = '跑分失败: ' + e.message;
      this.phase = '错误';
    });
  }

  // 3DMark 压力测试定义: 稳定性 = 最差轮 / 最好轮 x 100, 及格线 97%
  private stabilityText(): string {
    if (this.stressTotals.length < 2) {
      return '--';
    }
    let best = 0;
    let worst = 1e9;
    for (const v of this.stressTotals) {
      if (v > best) {
        best = v;
      }
      if (v < worst) {
        worst = v;
      }
    }
    if (best <= 0) {
      return '--';
    }
    return ((worst / best) * 100).toFixed(1) + '%';
  }

  private stabilityPass(): boolean {
    const t = this.stabilityText();
    if (t === '--') {
      return false;
    }
    return parseFloat(t) >= 97.0;
  }

  private async startCoreMark(): Promise<void> {
    if (this.coremarkRunning || this.running) {
      return;
    }
    this.coremarkRunning = true;
    this.coremarkResult = '运行中…';
    this.keepScreen(true);
    const t0: number = Date.now();
    const res: string = await BenchRunner.runCoreMark();
    this.coremarkResult = res;
    this.coremarkRunning = false;
    this.keepScreen(false);
    // 运行日志(旁路): CoreMark 单项没有可比的综合分, composite 记 0, 分数看 item 行
    const v: number = parseFloat(res);
    const okMark: boolean = !isNaN(v);
    this.logRunSummary(PH_OWN, 0, okMark ? 1 : 0, 0, okMark ? 0 : 1, Date.now() - t0);
  }

  private async startGb7(): Promise<void> {
    if (this.gb7Running || this.running) {
      return;
    }
    this.gb7Running = true;
    this.gb7Results = [];
    this.gb7Composite = 0;
    // 护栏: 开跑前读一次可用内存(只提示, 不阻止)
    this.refreshMem();
    this.keepScreen(true);
    const t0: number = Date.now();
    const results: Gb7Result[] = await BenchRunner.runGb7All(1, (r: Gb7Result) => {
      this.gb7Results = this.gb7Results.concat([r]);
      this.noteCpuPlacement(r);
    });
    // 随芯片变化自检(单核口径): 只判 SC2(轮间离散度), SC1 要等多核阶段的数据。
    // 旁路结论: 不改任何 score / metric / unit, 也不改任何负载。
    auditSingleStage(results);
    this.gb7Composite = FullReportBuilder.geoOf(results);
    this.gb7Running = false;
    this.keepScreen(false);
    // 运行日志(旁路): CS1 单核整轮汇总
    const scored: number = this.scoredCount(results);
    this.logRunSummary(PH_GB7_SINGLE, this.gb7Composite, scored, 0,
      results.length - scored, Date.now() - t0);
  }

  private async startGb7Multi(): Promise<void> {
    if (this.gb7MultiRunning || this.running || this.gb7Running) {
      return;
    }
    this.gb7MultiRunning = true;
    this.gb7MultiResults = [];
    // 护栏: 开跑前读一次可用内存(只提示, 不阻止)
    this.refreshMem();
    // 线程数按 SMT 开关算(开 = 全部逻辑核; 关 = 物理核数; 拓扑未知 = 全部逻辑核)。
    // 这里必须走这个函数, 不能再用 cpuCoreCount() —— 否则关掉开关后线程数仍然是逻辑核数。
    this.gb7Threads = this.multiThreads();
    this.keepScreen(true);
    const t0: number = Date.now();
    const results: Gb7Result[] = await BenchRunner.runGb7Multi(this.gb7Threads, (r: Gb7Result) => {
      this.gb7MultiResults = this.gb7MultiResults.concat([r]);
      this.noteCpuPlacement(r);
    });
    // 随芯片变化自检(多核口径): SC1(并行加速比 = 多核分/单核分) + SC2(轮间离散度)。
    // 单核卡片的综合分也要按同一份结论重算 —— 否则两张卡会说两套话。
    auditMultiStage(this.gb7Results, results);
    this.gb7Composite = FullReportBuilder.geoOf(this.gb7Results);
    this.gb7MultiRunning = false;
    this.keepScreen(false);
    // 运行日志(旁路): CS1 多核整轮汇总(composite 取已计分项的几何平均, 与单核卡同口径)
    const scored: number = this.scoredCount(results);
    this.logRunSummary(PH_GB7_MULTI, this.logGeo(results), scored, 0,
      results.length - scored, Date.now() - t0, 0, this.gb7Threads);
  }

  private gb7Speedup(name: string, metric: number): string {
    for (const s of this.gb7Results) {
      if (s.name === name && s.metric > 0 && metric > 0) {
        return (metric / s.metric).toFixed(2) + 'x';
      }
    }
    return '--';
  }

  private async startGb7Gpu(): Promise<void> {
    if (this.gb7GpuRunning || this.running) {
      return;
    }
    this.gb7GpuRunning = true;
    this.gb7GpuResults = [];
    this.gb7GpuComposite = 0;
    this.refreshMem();
    this.keepScreen(true);
    const t0: number = Date.now();
    const results: Gb7Result[] = await BenchRunner.runGb7Gpu((r: Gb7Result) => {
      this.gb7GpuResults = this.gb7GpuResults.concat([r]);
    });
    let prod = 1;
    let scored = 0;
    for (const r of results) {
      if (r.score > 0) {
        prod *= r.score;
        scored++;
      }
    }
    this.gb7GpuComposite = scored > 0 ? Math.pow(prod, 1 / scored) : 0;
    this.gb7GpuRunning = false;
    this.keepScreen(false);
    // 运行日志(旁路): CS1 GPU 整轮汇总
    this.logRunSummary(PH_GB7_GPU, this.gb7GpuComposite, scored, 0,
      results.length - scored, Date.now() - t0);
  }


  // GPU-SNL(Aurora Nomad Light)手动单项入口。
  // 硬要求: prepare() 返回非空错误串时显示驱动原文(存在 snResult.prepError / driverText, 不吞),
  //         并把本节按失败显示 —— 单项入口本来就只是跑一节, 不会影响任何其它负载。
  private async startSn(): Promise<void> {
    if (this.snRunning || this.running) {
      return;
    }
    this.snRunning = true;
    this.snHave = false;
    this.snOk = false;
    this.snPrepErr = '';
    this.snErrMsg = '';
    this.refreshMem();
    this.keepScreen(true);
    const t0: number = Date.now();
    let r: SnRunResult | null = null;
    try {
      r = await BenchRunner.runSn();
    } catch (e) {
      this.snErrMsg = 'GPU-SNL 抛出异常：' + e.message;
      r = null;
    }
    let line: string = '';
    let okAll: boolean = false;
    if (r === null) {
      this.snHave = false;
      line = '未拿到结果：' + this.snErrMsg;
    } else {
      // 无论成败都先原样落进标量(驱动原文一个字都不改)
      this.snHave = true;
      this.snOk = r.ok;
      this.snPrepErr = r.prepError;
      // 2026-10 缺陷 3 修: 这里以前只取 error/lastError, 两者都空时界面上就会印
      //   "GPU-SNL 未取得结果：（native 未给文本）" —— 与报告里原样附着的原始 JSON 矛盾。
      //   现在统一走 FullReportBuilder.snFailReason(): 失败一定有非空且真实的原因
      //   (JSON 解析失败 / native 明说 ok=false 但没给原因 / 驱动原文)。
      this.snErrMsg = r.prepError.length > 0 ? r.prepError
        : (r.ok ? '' : FullReportBuilder.snFailReason(r));
      this.snScoreV = r.score;
      this.snFpsV = r.fps;
      this.snTputV = r.throughputMpx;
      this.snVerdictV = r.verdict;
      this.snConclusiveV = r.gpuBoundConclusive;
      this.snDrawV = r.drawCallsPerFrame;
      this.snCpuVertV = r.cpuVertexDataBytesPerFrame;
      this.snDutyV = r.submissionDuty;
      this.snCalV = r.calibrationStatus;
      this.snCalKV = r.kScorePerFps;
      this.snPassesV = r.passesPerFrame;
      this.snScaleSrcV = r.scaleSource;
      this.snCredV = r.credibility;
      this.snDispV = r.dispersion;
      this.snEnvV = r.envState;
      this.snTempV = r.tempMedian;
      this.snKhzV = r.cpuKhzMedian;
      this.snVerV = r.benchVersion;
      this.snLogiV = r.cpuLogical;
      this.snPhysV = r.cpuPhysical;
      this.snArchV = r.simdArch;
      this.snNeonV = r.simdNeon;
      this.snSve2V = r.simdSve2;
      let wtxt: string = '';
      for (const w of r.warnings) {
        wtxt = wtxt.length > 0 ? (wtxt + ' | ' + w) : w;
      }
      this.snWarnV = r.risks.length > 0 ? (wtxt + '\nrisks：' + r.risks) : wtxt;
      if (r.prepError.length > 0) {
        line = '初始化失败（驱动原文）：' + r.prepError;
      } else if (!r.ok) {
        // 失败原因一律走同一个入口(2026-10 缺陷 3): 保证非空且真实, 并在
        // "原始 JSON 自称 ok:true 但解析不了"时把矛盾写出来, 不再印"（native 未给文本）"。
        line = FullReportBuilder.snFailReason(r);
      } else {
        okAll = true;
        line = r.score.toFixed(1) + ' 分 · ' + r.fps.toFixed(2) + ' FPS · ' +
          r.throughputMpx.toFixed(2) + ' Mpx/s · ' + r.verdict + ' · 标定=' + r.calibrationStatus +
          ' · benchVersion=' + r.benchVersion.toString();
      }
    }
    this.snRunning = false;
    this.keepScreen(false);
    RunLog.get().item('单项测试·GPU-SNL', 'Aurora Nomad Light', 1, 1, Date.now() - t0,
      r !== null ? r.score : 0, '分', 0, 1, okAll, line);
  }

  private excludedText(): string {
    const ex: string[] = (AppStorage.get<string[]>('multiExcluded') ?? []);
    if (ex.length === 0) {
      return '多核合成：全部负载并行度达标，全部计入';
    }
    let s = '多核合成已排除实测并行度不足的负载：';
    for (const n of ex) {
      s += n + ' ';
    }
    return s;
  }

  private async startStress(): Promise<void> {
    if (this.running || this.stressRunning) {
      return;
    }
    this.stressRunning = true;
    this.stressTotals = [];
    this.stressThermal = [];
    this.keepScreen(true);
    for (let round = 1; round <= 5; round++) {
      this.stressRound = round;
      this.running = true;
      this.live = [];
      this.doneCount = 0;
      this.totalCount = 0;
      this.phase = '压力测试 第' + round.toString() + ' 轮';
      this.gpuActive = false;
      const roundTag: string = '压力测试 第' + round.toString() + ' 轮 · ';
      const rec: BenchRecord = await BenchRunner.runAll((p: ProgressInfo) => {
        this.phase = '第' + round.toString() + ' 轮 · ' + p.phase;
        this.doneCount = p.index;
        this.totalCount = p.total;
        this.curTest = p.testName;
        this.curMs = p.lastMs;
        this.curScore = p.lastScore;
      }, (g: GpuProgress) => {
        this.gpuActive = true;
        this.phase = '第' + round.toString() + ' 轮 · GPU';
        this.curTest = g.sceneName;
        this.curMs = g.frameMs;
        this.curScore = g.fps;
        this.doneCount = g.framesDone;
        this.totalCount = g.framesTarget;
      }, roundTag);
      this.stressTotals = this.stressTotals.concat([rec.total]);
      this.stressThermal = this.stressThermal.concat([this.thermalLevel]);
      this.shown = rec;
      this.running = false;
    }
    this.stressRunning = false;
    this.keepScreen(false);
    this.shown = null;
    BenchStore.get().append({
      time: Date.now(),
      total: this.stressTotals[0],
      single: (AppStorage.get<number>('lastSingle') ?? 0),
      multi: (AppStorage.get<number>('lastMulti') ?? 0),
      gpu: 0,
      intSingle: (AppStorage.get<number>('intSingle') ?? 0),
      floatSingle: (AppStorage.get<number>('floatSingle') ?? 0),
      intMulti: (AppStorage.get<number>('intMulti') ?? 0),
      floatMulti: (AppStorage.get<number>('floatMulti') ?? 0),
      device: this.device + ' (压力测试首轮)',
      tests: [],
      multiTests: [],
      gpuScenes: []
    }).then((list: BenchRecord[]) => {
      this.records = list;
      this.refreshBest();
    });
  }

  // =========================================================================
  // 稳定性压测(连续 N 轮 CS1 单核全套)
  //
  // 只做三件事: 重复调用现有的 CS1 单核实现 + 记录 + 汇总。
  // 负载顺序、线程数、分数公式一行都没改 —— 综合分沿用首页 CS1 单核卡片的几何平均口径。
  // =========================================================================

  private openStressPicker(): void {
    this.scPickOpen = !this.scPickOpen;
  }

  // 入口: 按选定轮数新开一场压测
  private beginStress(target: number): void {
    if (this.scRunning || this.running) {
      return;
    }
    this.scTarget = target;
    this.scPickOpen = false;
    this.startStressCampaign(0);
  }

  // 从崩溃横幅"继续压测": 接着上次被打断的那一轮跑
  private async resumeStressCampaign(): Promise<void> {
    if (this.scRunning || this.running) {
      return;
    }
    if (this.scCrashPending) {
      const c: CampaignState | null = await StressSession.get().load();
      if (c !== null) {
        const after: CampaignState = await StressSession.get().noteCrash(c, this.scCrashItem, this.scCrashTs);
        this.scCrashRuns = after.crashes.length;
      }
      this.scCrashPending = false;
    }
    this.scInterrupted = false;
    this.startStressCampaign(0);
  }

  // target = 0 表示沿用会话里已有的目标轮数(继续); 否则新开一场
  private async startStressCampaign(target: number): Promise<void> {
    if (this.scRunning || this.running) {
      return;
    }
    this.scRunning = true;
    this.scStopRequested = false;
    this.scCopied = false;
    this.scPaused = false;
    this.scReport = null;
    this.scReportText = '';
    this.scReportOpen = false;
    this.running = true;
    this.live = [];
    this.gpuActive = false;
    this.keepScreen(true);

    try {
      let s: CampaignState;
      if (target > 0) {
        s = await StressSession.get().begin(target);
      } else {
        // 继续: 盘上已有会话就保留(completed / failures / crashes / scores 全部接着算)
        const existing: CampaignState | null = await StressSession.get().load();
        s = existing !== null ? await StressSession.get().resumeFrom(existing)
          : await StressSession.get().begin(this.scTarget);
      }
      this.scTarget = s.target;
      this.scDone = s.completed;
      this.scGateUsed = s.gateUsed;
      this.scGateDown = s.gateUnavailable;
      // 设备/版本在 aboutToAppear 已赋值, 这里再取一次兜底(压测报告里必须有)
      const device: string = this.device.length > 0 ? this.device : BenchRunner.deviceLabel();
      const nativeVer: string = this.nativeVer.length > 0 ? this.nativeVer : BenchRunner.nativeVersion();
      // 压测固定跑 CS1 单核(1 线程), 与首页「CS1测试」按钮完全一致
      const threads: number = 1;

      while (s.running && !this.scStopRequested && s.completed < s.target) {
        const run: number = s.completed + 1;
        this.scCurRound = run;
        this.phase = '稳定性压测 第' + run.toString() + '/' + s.target.toString() + ' 轮';
        this.curTest = 'CS1 单核全套（16 项）';
        this.doneCount = run - 1;
        this.totalCount = s.target;
        this.scLastRoundFails = 0;
        this.scLive = [];
        // 开跑标记(立刻落盘): 从这一刻起被杀才算"压测中断"
        s = await StressSession.get().markRoundStart(s, run, '（准备开跑第 ' + run.toString() + ' 轮）');
        // 内存闸门: 每轮采样一次可用内存, 逐项判定在 BenchRunner 里做
        const mem: MemStatus = MemoryProbe.sample();
        this.scMemNote = MemoryProbe.short(mem);
        if (mem.supported) {
          s.gateUsed = true;
        } else {
          s.gateUnavailable = true;
        }
        this.scGateDown = s.gateUnavailable;
        const started: number = Date.now();
        const items: Gb7ItemOutcome[] = await BenchRunner.runStressRound(run, threads, mem,
          (o: Gb7ItemOutcome) => {
            // 跳过的项没有 result; 跑过的项(哪怕是 score=0 的失败项)都要显示出来
            const r: Gb7Result | null = o.result;
            if (r !== null) {
              this.scLive = this.scLive.concat([r]);
            }
          },
          (runNo: number, item: string) => {
            this.scCurRound = runNo;
            this.scCurItem = item;
            StressSession.get().mark(s, runNo, item).catch((e: Error) => {
              console.warn('[AB] stress mark failed ' + e.message);
            });
          });
        const elapsed: number = Date.now() - started;
        const outcome: RoundOutcome = this.buildRoundOutcome(s, run, items, elapsed);
        s = await StressSession.get().recordRound(s, outcome);
        this.scDone = s.completed;
        this.scLastRoundMs = elapsed;
        this.scLastRoundFails = outcome.failCount;
        this.gb7Composite = StressSession.geomean(s.scores);
        this.scGateUsed = s.gateUsed;
        this.scGateDown = s.gateUnavailable;
        this.doneCount = s.completed;
        this.curMs = elapsed;
        this.curScore = outcome.score;
        // 运行日志(旁路): 压测每轮结束写一行汇总(逐项行由 BenchRunner 以 phase='压测' 写入)
        let roundScored = 0;
        for (const it of items) {
          const rr: Gb7Result | null = it.result;
          if (rr !== null && rr.score > 0) {
            roundScored++;
          }
        }
        this.logRunSummary(PH_STRESS, outcome.score, roundScored, outcome.skipCount,
          outcome.failCount, elapsed, this.scCrashRuns);
      }
      // 结束(跑满 / 用户停止 / 会话已被外部结束): 落盘报告 + 只写一条历史
      let totalMs = 0;
      for (const d of s.durations) {
        totalMs += d;
      }
      await this.appendStressHistory(s, totalMs, device);
      const ended: number = Date.now();
      // 先把会话标记成"已结束", 报告里的状态与统计口径才一致
      s.running = false;
      s.roundOpen = false;
      s.endedAt = ended;
      // 报告要在 finish() 之前算好: finish 会把进行中的会话从盘上删掉
      this.scReport = StressSession.buildReport(s, device, nativeVer);
      this.scReportText = await StressSession.get().finish(s, ended);
      this.scReportOpen = true;
    } catch (e) {
      console.warn('[AB] stress campaign failed');
      this.phase = '压测异常结束(JS 侧捕获)';
    } finally {
      this.scRunning = false;
      this.running = false;
      this.keepScreen(false);
    }
  }

  // 一轮结束后, 把逐项结果折算成"失败 / 跳过 / 完成"三态与统计
  private buildRoundOutcome(s: CampaignState, run: number,
    items: Gb7ItemOutcome[], elapsedMs: number): RoundOutcome {
    const failures: FailItemDetail[] = [];
    const skips: SkipItemDetail[] = [];
    const statuses: RoundStatus[] = [];
    const scores: number[] = [];
    let failCount = 0;
    let skipCount = 0;
    // 全程不中断: 单项失败只记录, 该轮其余项照跑(交给 BenchRunner 的逐项循环)
    for (const o of items) {
      if (o.skipped) {
        skipCount++;
        skips.push({ run: run, item: o.name, reason: o.skipReason });
        statuses.push({ item: o.name, status: 'skip' });
        continue;
      }
      const r: Gb7Result | null = o.result;
      if (r === null) {
        continue;
      }
      if (r.score > 0) {
        scores.push(r.score);
      }
      const reason: string = itemFailure(r);
      if (reason.length > 0) {
        failCount++;
        failures.push({ run: run, item: statItemName(r), phase: 'CS1 单核 · 第 ' +
          run.toString() + ' 轮', reason: reason });
        statuses.push({ item: r.name, status: 'fail' });
      } else {
        statuses.push({ item: r.name, status: 'ok' });
      }
    }
    // 综合分与首页 CS1 单核卡片完全同一口径: 已计分项的几何平均
    let prod = 1;
    let scored = 0;
    for (const v of scores) {
      prod *= v;
      scored++;
    }
    const score: number = scored > 0 ? Math.pow(prod, 1 / scored) : 0;
    const outcome: RoundOutcome = {
      run: run,
      score: score,
      elapsedMs: elapsedMs,
      finished: true,
      failCount: failCount,
      skipCount: skipCount,
      failures: failures,
      skips: skips,
      statuses: statuses,
      thermal: this.thermalLevel
    };
    return outcome;
  }

  // 压测期间不写历史, 只在这里写一条汇总记录(避免 1000 轮刷爆历史列表)
  private async appendStressHistory(s: CampaignState, totalMs: number, device: string): Promise<void> {
    try {
      const list: BenchRecord[] = await BenchStore.get().append(
        StressSession.sessionRecord(s, device + ' · 总耗时 ' + (totalMs / 1000).toFixed(0) + ' s'));
      this.records = list;
      this.refreshBest();
    } catch (e) {
      console.warn('[AB] stress history append failed');
    }
  }

  private stopStress(): void {
    if (!this.scRunning) {
      return;
    }
    // 只置停止标志: 当前这一轮会跑完(避免半轮数据), 然后落盘报告
    this.scStopRequested = true;
  }

  private toggleStressReport(): void {
    this.scReportOpen = !this.scReportOpen;
  }

  // 本轮已跑过的项一览(失败项标 x, 未计分/失败项按实际分数显示)
  private stressLiveText(): string {
    let s = '';
    for (const r of this.scLive) {
      if (s.length > 0) {
        s += ' · ';
      }
      // 计分项跑失败(注册表 scored=true 但没跑出吞吐)标"失败"; 按政策不计分的保持中性
      s += (r.score > 0 ? (r.name + ' ' + r.score.toFixed(0))
        : (r.name + (BenchRunner.itemScoredButFailed(r) ? ' 失败' : ' 未计分')));
    }
    return s;
  }

  // 报告预览(完整报告在 scReportText 里, 走剪贴板复制)
  private reportPreview(): string {
    const t: string = this.scReportText;
    if (t.length <= REPORT_PREVIEW) {
      return t;
    }
    return t.slice(0, REPORT_PREVIEW) + '\n…';
  }

  // 报告里"被中断过"的轮次文字
  private stressInterruptText(): string {
    if (!this.scInterrupted) {
      return '';
    }
    return '压测中断：第 ' + this.scInterruptRound.toString() + ' 轮，最后启动的项「' +
      this.scInterruptItem + '」';
  }

  private stressProgressText(): string {
    return '第 ' + this.scCurRound.toString() + ' / ' + this.scTarget.toString() + ' 轮 · 已完成 ' +
      this.scDone.toString() + ' 轮';
  }

  private stressRateText(): string {
    const r: CampaignReport | null = this.scReport;
    if (r === null || r.completed === 0) {
      return '';
    }
    return '成功率 ' + (r.successRate * 100).toFixed(2) + '%（成功 ' + r.successRounds.toString() +
      ' / 完成 ' + r.completed.toString() + '）';
  }

  private stressScoreText(): string {
    const r: CampaignReport | null = this.scReport;
    if (r === null || r.scores.length === 0) {
      return '';
    }
    return '分数 min/median/max：' + r.minScore.toFixed(1) + ' / ' + r.medianScore.toFixed(1) +
      ' / ' + r.maxScore.toFixed(1);
  }

  private stressCrashCount(): number {
    const r: CampaignReport | null = this.scReport;
    return r === null ? 0 : r.crashes.length;
  }

  private async copyStressReport(): Promise<void> {
    const text: string = this.scReportText;
    if (text.length === 0) {
      return;
    }
    try {
      // pasteboard 不需要 context, 直接用系统剪贴板(避免再引入一处已废弃的 getContext)
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.scCopied = true;
    } catch (e) {
      this.scCopied = false;
      console.warn('[AB] stress report copy failed');
    }
  }

  // 中断后用户选择"结束并查看报告": 把这次崩溃登记进会话, 然后落盘报告并关闭会话
  private async endStressCampaign(): Promise<void> {
    if (this.scRunning) {
      return;
    }
    try {
      const s: CampaignState | null = await StressSession.get().load();
      if (s !== null) {
        if (this.scCrashPending) {
          const after: CampaignState = await StressSession.get().noteCrash(s, this.scCrashItem, this.scCrashTs);
          this.scCrashRuns = after.crashes.length;
        }
        const ended: number = Date.now();
        s.endedAt = ended;
        // 报告要在 finish() 之前算好: finish 会把进行中的会话从盘上删掉
        this.scReport = StressSession.buildReport(s, this.device, this.nativeVer);
        this.scReportText = await StressSession.get().finish(s, ended);
        this.scReportOpen = true;
        await this.appendStressHistory(s, totalDurationMs(s), this.device);
      } else {
        const stored: string = await StressSession.get().storedReport();
        if (stored.length > 0) {
          this.scReportText = stored;
          this.scReportOpen = true;
        }
      }
    } catch (e) {
      console.warn('[AB] stress end failed');
    }
    this.scCrashPending = false;
    this.scInterrupted = false;
    this.scPaused = false;
    this.scDone = 0;
  }

  private async clearStressReport(): Promise<void> {
    this.scReport = null;
    this.scReportText = '';
    this.scReportOpen = false;
    await StressSession.get().clearReport();
  }

  // ---- 长跑自检卡(原「稳定性压测」; 首页 GB7 三张卡下方: 入口按钮 + 轮数选择 + 进行中状态 + 可复制报告) ----
  @Builder
  stressRoundsOption(n: number) {
    Button(n.toString() + ' 轮')
      .height(34)
      .fontSize(this.fz(12))
      .backgroundColor(this.scTarget === n ? ACCENT : CHIP)
      .fontColor(this.scTarget === n ? ON_ACCENT : TEXT)
      .margin({ right: 8 })
      .onClick(() => {
        this.scTarget = n;
      })
  }

  @Builder
  stressCampaignCard() {
    Column() {
      Row() {
        Text('长跑自检（可选）')
          .fontSize(this.fz(12))
          .fontColor(SUB)
        Blank()
        if (this.scTarget > 0) {
          Text('目标 ' + this.scTarget.toString() + ' 轮')
            .fontSize(this.fz(11))
            .fontColor(FAINT)
        }
      }
      .width('100%')

      Button(this.scRunning
        ? ('长跑自检进行中 · ' + this.stressProgressText())
        : '长跑自检（可选 · 每轮约 30 秒，1000 轮需约 8 小时）')
        .width('100%')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(WARN)
        .margin({ top: 10 })
        .enabled(!this.scRunning)
        .onClick(() => {
          this.openStressPicker();
        })

      if (this.scPickOpen && !this.scRunning) {
        Column() {
          Text('选择轮数（每轮 = CS1 单核全套 16 项，约 30 秒；默认 50 轮）')
            .fontSize(this.fz(11))
            .fontColor(SUB)
            .width('100%')
          Row() {
            this.stressRoundsOption(10)
            this.stressRoundsOption(50)
            this.stressRoundsOption(100)
            this.stressRoundsOption(1000)
          }
          .width('100%')
          .margin({ top: 8 })
          Text('1000 轮按每轮 30 秒估算约需 8 小时，期间请保持亮屏；跑不动了可以随时停止出报告。')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .width('100%')
            .margin({ top: 6 })
          Row() {
            Button('开始自检（' + this.scTarget.toString() + ' 轮）')
              .layoutWeight(1)
              .constraintSize({ minHeight: this.btnH() })
              .fontSize(this.fz(13))
              .backgroundColor(ACCENT)
              .fontColor(ON_ACCENT)
              .onClick(() => {
                this.beginStress(this.scTarget);
              })
            Button('取消')
              .constraintSize({ minHeight: this.btnH() })
              .fontSize(this.fz(13))
              .backgroundColor(CHIP)
              .fontColor(SUB)
              .margin({ left: 8 })
              .onClick(() => {
                this.openStressPicker();
              })
          }
          .width('100%')
          .margin({ top: 10 })
        }
        .width('100%')
        .margin({ top: 10 })
      }

      if (this.scRunning) {
        Column() {
          Text(this.stressProgressText())
            .fontSize(this.fz(12))
            .fontColor(ACCENT)
            .width('100%')
          Text('当前项：' + (this.scCurItem.length > 0 ? this.scCurItem : '准备中'))
            .fontSize(this.fz(12))
            .fontColor(TEXT)
            .width('100%')
            .margin({ top: 4 })
          Progress({
            value: this.scDone,
            total: this.scTarget > 0 ? this.scTarget : 1,
            type: ProgressType.Linear
          })
            .width('100%')
            .color(accentGradient())
            .backgroundColor(TRACK)
            .margin({ top: 8 })
          Text('上一轮：综合分 ' + this.gb7Composite.toFixed(0) + ' · 耗时 ' +
            this.scLastRoundMs.toString() + ' ms · 失败项 ' + this.scLastRoundFails.toString() +
            (this.scMemNote.length > 0 ? (' · ' + this.scMemNote) : ''))
            .fontSize(this.fz(10))
            .fontColor(SUB)
            .width('100%')
            .margin({ top: 6 })
          if (this.scLive.length > 0) {
            Text(this.stressLiveText())
              .fontSize(this.fz(10))
              .fontColor(this.scLastRoundFails > 0 ? WARN : GOOD)
              .width('100%')
              .margin({ top: 4 })
          }
          Text('压测期间不写历史记录（只在整场结束时写一条汇总），崩了下次启动可以接着跑。')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .width('100%')
            .margin({ top: 4 })
          Button('停止压测（跑完当前这一轮后收工并出报告）')
            .width('100%')
            .constraintSize({ minHeight: this.btnH() })
            .fontSize(this.fz(12))
            .backgroundColor(C.chipWarn)
            .fontColor(WARN)
            .margin({ top: 10 })
            .onClick(() => {
              this.stopStress();
            })
        }
        .width('100%')
        .margin({ top: 10 })
      }

      if (this.scReportText.length > 0 && !this.scRunning) {
        Column() {
          Row() {
            Text('稳定性压测报告')
              .fontSize(this.fz(12))
              .fontColor(SUB)
            Blank()
            Text(this.scReportOpen ? '收起' : '展开')
              .fontSize(this.fz(11))
              .fontColor(ACCENT)
              .onClick(() => {
                this.toggleStressReport();
              })
          }
          .width('100%')
          if (this.scReport !== null) {
            Text(this.stressRateText())
              .fontSize(this.fz(13))
              .fontColor(this.stressRatePass() ? GOOD : DANGER)
              .width('100%')
              .margin({ top: 6 })
            Text(this.stressScoreText())
              .fontSize(this.fz(11))
              .fontColor(TEXT)
              .width('100%')
              .margin({ top: 4 })
            Text('崩溃 ' + this.stressCrashCount().toString() + ' 次' +
              (this.scGateDown ? ' · 本次未做内存闸门（内存接口不可用）'
                : (this.scGateUsed ? ' · 已启用逐项内存闸门' : '')))
              .fontSize(this.fz(11))
              .fontColor(this.stressCrashCount() > 0 ? WARN : SUB)
              .width('100%')
              .margin({ top: 4 })
          }
          if (this.scReportOpen) {
            Text(this.reportPreview())
              .fontSize(this.fz(10))
              .fontColor(SUB)
              .copyOption(CopyOptions.LocalDevice)
              .width('100%')
              .padding(8)
              .backgroundColor(INSET)
              .borderRadius(8)
              .margin({ top: 8 })
            if (this.scReportText.length > REPORT_PREVIEW) {
              Text('（上面只显示前 ' + REPORT_PREVIEW.toString() + ' 个字符；点「复制报告」拿到完整报告，' +
                this.scReportText.length.toString() + ' 字符）')
                .fontSize(this.fz(10))
                .fontColor(FAINT)
                .width('100%')
                .margin({ top: 4 })
            }
          }
          Row() {
            Button(this.scCopied ? '已复制' : '复制报告')
              .layoutWeight(1)
              .height(36)
              .fontSize(this.fz(12))
              .backgroundColor(CHIP)
              .fontColor(GOOD)
              .onClick(() => {
                this.copyStressReport();
              })
            Button('关闭报告')
              .layoutWeight(1)
              .height(36)
              .fontSize(this.fz(12))
              .backgroundColor(CHIP)
              .fontColor(SUB)
              .margin({ left: 8 })
              .onClick(() => {
                this.clearStressReport();
              })
          }
          .width('100%')
          .margin({ top: 10 })
          Text('长按报告文本也可直接复制；同一份报告已落盘，进程被杀也不会丢。')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .width('100%')
            .margin({ top: 6 })
        }
        .width('100%')
        .padding(10)
        .backgroundColor(INSET)
        .borderRadius(10)
        .margin({ top: 10 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
  }

  private stressRatePass(): boolean {
    const r: CampaignReport | null = this.scReport;
    if (r === null) {
      return false;
    }
    return r.successRate >= 0.99;
  }

  // ---- 崩溃横幅(首页顶部; 统一两种崩溃: 单轮跑分崩溃 / 稳定性压测被中断) ----
  @Builder
  crashBanner() {
    if (this.scInterrupted && !this.scRunning) {
      // 情况二: 压测中断(native 崩溃/OOM 杀进程), 提供"继续压测"和"结束并查看报告"两个出口
      Column() {
        Row() {
          Text('压测中断')
            .fontSize(this.fz(14))
            .fontWeight(FontWeight.Bold)
            .fontColor(WARN)
          Blank()
          Button('清除')
            .height(28)
            .fontSize(this.fz(12))
            .backgroundColor(C.chipWarn)
            .fontColor(WARN)
            .onClick(() => {
              // 只收起横幅: 崩溃归因仍然保留, 下次点"继续压测"时会记进报告
              this.scInterrupted = false;
            })
        }
        .width('100%')
        Text(this.stressInterruptText())
          .fontSize(this.fz(13))
          .fontColor(TEXT)
          .width('100%')
          .margin({ top: 8 })
        Text('已完整跑完 ' + this.scDone.toString() + ' / ' + this.scTarget.toString() +
          ' 轮；点「继续压测」会从第 ' + (this.scDone + 1).toString() + ' 轮整轮重跑（被中断的半轮不拼分数）。')
          .fontSize(this.fz(11))
          .fontColor(SUB)
          .width('100%')
          .margin({ top: 6 })
        Text('崩溃不一定是这一项本身的问题：进程可能死在上一项遗留的大内存或驱动状态上。')
          .fontSize(this.fz(11))
          .fontColor(WARN)
          .width('100%')
          .margin({ top: 6 })
        if (this.nGuardNote.length > 0) {
          Text(this.nGuardNote)
            .fontSize(this.fz(11))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 6 })
        }
        Row() {
          Button('继续压测')
            .layoutWeight(1)
            .height(38)
            .fontSize(this.fz(13))
            .backgroundColor(ACCENT)
            .fontColor(ON_ACCENT)
            .onClick(() => {
              this.resumeStressCampaign();
            })
          Button('结束并查看报告')
            .layoutWeight(1)
            .height(38)
            .fontSize(this.fz(13))
            .backgroundColor(C.chipWarn)
            .fontColor(WARN)
            .margin({ left: 10 })
            .onClick(() => {
              this.endStressCampaign();
            })
        }
        .width('100%')
        .margin({ top: 12 })
      }
      .width(this.cardW())
      .padding(this.cardPad())
      .backgroundColor(C.chipWarnAlt)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
      .border({ width: 1, color: WARN })
      .margin({ top: 12 })
    } else if (this.scPaused && !this.scRunning) {
      // 情况三: 压测没结束, 但当前没在跑(轮次之间被系统回收/划掉): 不算崩溃, 但可以接着跑
      Column() {
        Row() {
          Text('压测未结束')
            .fontSize(this.fz(14))
            .fontWeight(FontWeight.Bold)
            .fontColor(ACCENT)
          Blank()
          Button('结束并查看报告')
            .height(28)
            .fontSize(this.fz(12))
            .backgroundColor(CHIP)
            .fontColor(ACCENT)
            .onClick(() => {
              this.endStressCampaign();
            })
        }
        .width('100%')
        Text('上一场稳定性压测还没跑完（已完成 ' + this.scDone.toString() + ' / ' +
          this.scTarget.toString() + ' 轮），这次启动没有发现崩溃痕迹。')
          .fontSize(this.fz(12))
          .fontColor(TEXT)
          .width('100%')
          .margin({ top: 8 })
        Row() {
          Button('继续压测')
            .layoutWeight(1)
            .height(38)
            .fontSize(this.fz(13))
            .backgroundColor(ACCENT)
            .fontColor(ON_ACCENT)
            .onClick(() => {
              this.startStressCampaign(0);
            })
          Button('结束并查看报告')
            .layoutWeight(1)
            .height(38)
            .fontSize(this.fz(13))
            .backgroundColor(CHIP)
            .fontColor(ACCENT)
            .margin({ left: 10 })
            .onClick(() => {
              this.endStressCampaign();
            })
        }
        .width('100%')
        .margin({ top: 10 })
      }
      .width(this.cardW())
      .padding(this.cardPad())
      .backgroundColor(INSET)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
      .border({ width: 1, color: ACCENT })
      .margin({ top: 12 })
    } else if ((this.crumbFound || this.nCrashFound) && !this.scRunning && !this.scPaused && !this.scInterrupted) {
      // 情况一: 单轮跑分崩溃(与历史行为一致)。
      // 除了 ArkTS 面包屑, 这里还接了 native 取证装置的现场(nCrashFound):
      // 崩在两项之间的胶水代码里时面包屑可能已经不在, 只有 native 记录能说明问题, 反之亦然。
      this.crumbBannerInner()
    }
  }

  // 情况一: 单轮跑分崩溃(面包屑还在 => 上次进程异常结束)
  //
  // 两个来源都会显示, 且刻意分开写、互相印证:
  //   · ArkTS 面包屑 = 「最后启动的项」+ 该项开跑前的内存足迹(只能说明"崩之前跑到哪了")
  //   · native 记录 = 致命信号 / si_addr / pc 是否落在我们自己的 .so 里 / 内存快照(说明"怎么死的")
  //   · 有面包屑、没有 native 记录 => 进程不是被可捕获的致命信号打死的: 更可能是系统 SIGKILL(内存压力 / 后台管制)
  //   · pc 落在自己 .so 内 + 自己标记的项 => 基本可以定性为"我们自己的 native 代码越界"
  @Builder
  crumbBannerInner() {
    Column() {
        Row() {
          Text(this.crumbHeadline())
            .fontSize(this.fz(14))
            .fontWeight(FontWeight.Bold)
            .fontColor(DANGER)
          Blank()
          Button('清除')
            .height(28)
            .fontSize(this.fz(12))
            .backgroundColor(C.chipDanger)
            .fontColor(DANGER)
            .onClick(() => {
              this.dismissCrumb();
            })
        }
        .width('100%')
        // ---- 来源 ①: ArkTS 面包屑(最后启动的项 + 该项开跑前的内存足迹) ----
        if (this.crumbFound) {
          Text('① ArkTS 面包屑（每开跑一项之前写盘）')
            .fontSize(this.fz(11))
            .fontColor(ACCENT)
            .width('100%')
            .margin({ top: 10 })
          Text(this.crumbLine)
            .fontSize(this.fz(13))
            .fontColor(TEXT)
            .width('100%')
            .margin({ top: 6 })
          Text('注意：这是崩溃前最后「启动」的那一项，不一定就是肇事项——也可能是它上一项留下的大内存或 GPU 驱动状态导致的。')
            .fontSize(this.fz(11))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 6 })
          Text(this.crumbDetail)
            .fontSize(this.fz(11))
            .fontColor(SUB)
            .copyOption(CopyOptions.LocalDevice)
            .width('100%')
            .margin({ top: 8 })
          Text('长按上面文字即可复制，直接发回即可；同一份内容也已写入下面的自诊断历史')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .width('100%')
            .margin({ top: 4 })
        } else if (this.nCrashIsCrash) {
          Text('① ArkTS 面包屑不在')
            .fontSize(this.fz(11))
            .fontColor(ACCENT)
            .width('100%')
            .margin({ top: 10 })
          Text('没有可对齐的「最后启动的项」：进程可能死在两项之间的胶水代码里（准备 / 收尾阶段），或面包屑已在上一轮收尾时被清除。这种情况以下面的 native 现场为准。')
            .fontSize(this.fz(11))
            .fontColor(SUB)
            .width('100%')
            .margin({ top: 6 })
        } else {
          Text('① 本条不是崩溃：这是「关于」页「写入自检记录（不崩溃）」按钮写下的链路自检记录，说明 setLogDir → 写盘 → 启动读回 → 清盘这条链路是通的。')
            .fontSize(this.fz(11))
            .fontColor(GOOD)
            .width('100%')
            .margin({ top: 10 })
        }

        // ---- 来源 ②: native 取证装置写下的现场(读完已立刻清盘, 全文留在内存里可复制) ----
        if (this.nCrashFound) {
          Text('② ' + (this.nCrashIsCrash ? 'native 崩溃现场（来自取证装置）'
            : 'native 取证装置自检记录（非崩溃，来自「关于」页的自检按钮）'))
            .fontSize(this.fz(13))
            .fontWeight(FontWeight.Bold)
            .fontColor(DANGER)
            .width('100%')
            .margin({ top: 12 })
          if (this.nCrashSummary.length > 0) {
            Text('关键字段：' + this.nCrashSummary)
              .fontSize(this.fz(11))
              .fontColor(TEXT)
              .width('100%')
              .padding(6)
              .backgroundColor(INSET)
              .borderRadius(6)
              .margin({ top: 6 })
          }
          Text(this.nCrashPreview)
            .fontSize(this.fz(10))
            .fontColor(SUB)
            .copyOption(CopyOptions.LocalDevice)
            .width('100%')
            .padding(8)
            .backgroundColor(INSET)
            .borderRadius(8)
            .margin({ top: 6 })
          Row() {
            Button(this.nCrashCopied ? '已复制全文' : '复制 native 现场全文')
              .height(32)
              .fontSize(this.fz(11))
              .backgroundColor(CHIP)
              .fontColor(GOOD)
              .onClick(() => {
                this.copyNativeEvidence();
              })
            Text('全文 ' + this.nCrashChars.toString() + ' 字符 · 证据文件：' +
              (this.nGuardPath.length > 0 ? this.nGuardPath : '（路径未设置）'))
              .fontSize(this.fz(10))
              .fontColor(FAINT)
              .layoutWeight(1)
              .margin({ left: 8 })
          }
          .width('100%')
          .margin({ top: 8 })
          Text('读到的记录已立刻从磁盘清除（避免下次启动重复报同一条）；同一条也已写进下面的自诊断历史与运行日志的 crash 行。' +
            '要判断是被系统 SIGKILL 还是我们自己越界，请把这一段和上面的面包屑一起看。')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .width('100%')
            .margin({ top: 6 })
        } else if (this.crumbFound) {
          Text('② native 侧没有现场记录')
            .fontSize(this.fz(12))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 12 })
          Text('进程不是被 SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT 这类可捕获的致命信号打死的：更可能是被系统直接 SIGKILL（内存压力、后台管制）或运行时长限制。这类结束 native 侧本来就写不出记录——「盘上没有记录」本身就是判据，可与上面面包屑里的内存足迹对照。')
            .fontSize(this.fz(11))
            .fontColor(SUB)
            .width('100%')
            .margin({ top: 6 })
        }
        if (this.nGuardNote.length > 0) {
          Text(this.nGuardNote)
            .fontSize(this.fz(11))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 10 })
        }
        if (this.crumbLog.length > 0) {
          Text(this.crumbLogOpen ? '收起自诊断历史' : ('自诊断历史 ' + this.crumbLog.length.toString() + ' 条（点击展开）'))
            .fontSize(this.fz(11))
            .fontColor(ACCENT)
            .width('100%')
            .margin({ top: 8 })
            .onClick(() => {
              this.crumbLogOpen = !this.crumbLogOpen;
            })
          if (this.crumbLogOpen) {
            ForEach(this.crumbLog, (s: string, i: number) => {
              Text(s)
                .fontSize(this.fz(10))
                .fontColor(SUB)
                .copyOption(CopyOptions.LocalDevice)
                .width('100%')
                .padding(8)
                .backgroundColor(INSET)
                .borderRadius(8)
                .margin({ top: 6 })
            }, (s: string, i: number) => 'clog' + i.toString())
          }
        }
      }
    .width(this.cardW())
    .padding(this.cardPad())
    .backgroundColor(C.chipDangerAlt)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .border({ width: 1, color: DANGER })
    .margin({ top: 12 })
  }

  // ---- 可用内存护栏卡(不阻止运行, 只提示) ----
  @Builder
  memCard() {
    if (this.memChecked) {
      Column() {
        Row() {
          Text(this.memHeadline())
            .fontSize(this.fz(12))
            .fontColor(this.memLow ? DANGER : (this.memSupported ? GOOD : SUB))
          Blank()
          Text(this.memSupported
            ? ('总 ' + this.memTotalMb.toString() + ' MB · ' + fmtTime(this.memAt) + ' · 点击重新检测')
            : '点击重新检测')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
        }
        .width('100%')
        if (this.memLow) {
          Text('警告：可用内存低于 1.5 GB，CS1 的大内存负载可能被系统回收。仅提示，不阻止运行。')
            .fontSize(this.fz(11))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 6 })
        }
      }
      .width(this.cardW())
      .padding(this.cardPad())
      .backgroundColor(CARD)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
      .margin({ top: 12 })
      .onClick(() => {
        this.refreshMem();
      })
    }
  }

  @Builder
  tabBarItem(idx: number, label: string) {
    Column() {
      Text(label)
        .fontSize(this.fz(13))
        .fontColor(this.tab === idx ? ACCENT : SUB)
        .fontWeight(this.tab === idx ? FontWeight.Bold : FontWeight.Normal)
        //  ★ 2026-10-10: 这里原来挂着一层 .shadow(半径 12) —— 那是**贴着一行字**画的辉光,
        //    在真机上就是"字糊"的直接来源之一, 已整条删除(可读性硬标准第 ③ 条)。
    }
    // 等分: 每一项吃掉 1 份剩余宽度。以前写的是 width('100%') —— 那样每个标签都按"整个
    // 底栏的宽度"要位置, 标签多于一页时后面的会被挤出屏幕(看起来就像"只有第一个标签, 另一半空着")。
    .layoutWeight(1)
    .height(TAB_H)
    .justifyContent(FlexAlign.Center)
    //  ★ 2026-10-10: 这里原来挂着 HDS 点光源 —— 它把**每一格**的边框都点亮,
    //    在真机上被用户看成"底部导航被白色的框框起来, 非常丑", 已整条撤掉。
    //    选中态改由一条静态的 2vp 底线表示(纯色, 没有光、不产生离屏层)。
    .border({
      width: { bottom: this.tab === idx ? 2 : 0 },
      color: this.tab === idx ? ACCENT : C.transparent
    })
    // 官方点击反馈(一次性, 跑分期间关闭)
    .clickEffect(this.fxClick())
    // 无障碍: 标签项本身只有文字, 补一个完整的朗读文本与选中状态
    .accessibilityText(label + (this.tab === idx ? '（当前页）' : '标签页'))
    .accessibilityLevel('yes')
    .onClick(() => {
      this.tab = idx;
      if (idx === 1) {
        // 切到「历史」页时读一次账本(本地文件读, 只读; 不在任何计时区间里)
        this.reloadHistory();
      }
      if (idx === 2) {
        // 切到「关于」页时刷新日志统计/尾部预览(只读 ≤512 KB 的日志文件, 不阻塞 UI)
        this.refreshLogStats();
        // 取证装置状态也顺手刷一次(自检卡上"固定显示"的那两行)
        this.refreshNativeGuard();
        // 设备画像也同步一次(窗口/折叠状态可能已变)
        this.syncProfile();
        // 上传设置(范围/N)与调试入口解锁状态都是内存态, 这里对齐一次
        this.uploadRange = LogUpload.get().range();
        this.uploadHistN = LogUpload.get().historyCount();
      } else {
        // 切回首页时对齐一次布局状态: 首页的操作区在左栏、结果区在右栏, 两者都由
        //  DeviceProfile 里的同一份计划决定(uiRes / startDia / 栅格 / 内容列宽)。
        //  窗口尺寸可能在别的页面上变过(分屏 / 旋转 / PC 拖拽), 这里重算一次,
        //  保证"回到首页看到的第一帧"就是当前窗口该有的样子 —— 幂等, 输入没变时一个字段都不写。
        this.syncProfile();
      }
      // ③ 粒子: 页面切换时在底栏被点的那一格消散(fxOn() 为假时一个粒子都不放)
      this.fxTabBurst(idx);
    })
  }

  // 设备卡: index = 0 表示放在单列容器里(卡片自带 92% 宽度), 1 表示填满 GridCol
  @Builder
  deviceCardInner(fill: number) {
    Column() {
      Row() {
        Text('设备')
          .fontSize(this.fz(12))
          .fontColor(SUB)
        Blank()
        Text(this.nativeVer)
          .fontSize(this.fz(11))
          .fontColor(FAINT)
      }
      .width('100%')
      // 主标题 = 传播名(识别不到才回退型号码), 下面一行小字跟型号码与系统
      Row() {
        Text(this.devName.length > 0 ? this.devName : this.device)
          .fontSize(this.fz(15))
          .fontWeight(FontWeight.Medium)
          .fontColor(TEXT)
          .layoutWeight(1)
        if (this.devFoldTag.length > 0) {
          Text('折叠屏 · ' + this.devFoldTag)
            .fontSize(this.fz(10))
            .fontColor(ACCENT2)
            .backgroundColor(C.chipViolet)
            .borderRadius(6)
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
        }
      }
      .width('100%')
      .margin({ top: 8 })
      Text(this.devModelLine)
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 4 })
      // ---- 「本机性能天花板」一行(2026-10 追加; 首页第一屏) ----
      //   为什么放在设备卡里: 它是设备级事实, 与"跑没跑过"无关 —— 打开 App 就能看到
      //   "这颗芯片最快的地方在哪、系统放不放我们进去、我们实际只能到哪一档"。
      //   措辞只有 native 一处(BenchRunner.ceiling()), 与结果页 / 报告完全同一句话。
      Text(this.ceilingShow())
        .fontSize(this.fz(10))
        .fontColor(this.ceilingLine.length > 0 ? ACCENT : FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 6 })
      // ---- 超线程(SMT)开关: 默认开 ----
      // 位置: 设备卡里(首页第一屏), 与"核心数"紧邻 —— 因为它改的正是"多核阶段用几个线程"。
      Row() {
        Toggle({ type: ToggleType.Switch, isOn: this.smtOn })
          .onChange((on: boolean) => {
            this.onSmtSwitch(on);
          })
        Text('使用超线程（SMT）')
          .fontSize(this.fz(12))
          .fontColor(TEXT)
          .margin({ left: 8 })
          .layoutWeight(1)
        Text(this.smtOn ? '开' : '关')
          .fontSize(this.fz(11))
          .fontColor(this.smtOn ? GOOD : WARN)
      }
      .width('100%')
      .margin({ top: 10 })
      // 无障碍: 整行作为一个可朗读单元(开关 + 说明文字 + 当前状态一起念)
      .accessibilityGroup(true)
      .accessibilityText('使用超线程（SMT）开关，当前' + (this.smtOn ? '已开启' : '已关闭'))
      // ---- 以下几行是"诊断信息": 竖向空间不够时(tightY)收起来 ----
      //  什么时候收: 顶栏+标签栏+设备卡+主按钮+2 行结果 在窗口高度里放不下 —— 典型是
      //  展开态且横向的窗口 / 横屏手机 / 用户拖出来的扁窗口(高度只有几百 vp)。
      //   别拿"阔折叠内屏 16:10"当例子: 那条是被证伪的假设(Pura X 的面板数据被
      //    套到了 Pura X Max 上)。目标机型展开态是 1:sqrt(2) 的竖长内屏(664x940vp),
      //    高度 940vp 很宽裕 —— 它走的是"放得下"那一条, 不是这里。
      //  收起的是诊断细节, 传播名 / 型号码 / SMT 开关 / 核心数都还在; 关于页里一行不少。
      if (!this.tightY) {
        Text(this.smtHint())
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
        if (this.smtNote.length > 0) {
          // 拓扑未知 / 未检测到 SMT / native 不可用: 写明"开关为什么不生效"
          Text(this.smtNote)
            .fontSize(this.fz(10))
            .fontColor(WARN)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 4 })
        }
        // CPU 拓扑事实(逻辑核 / 物理核 / 来源 / 逐路径 errno): 一行, 与关于页同一份文本
        Text(DeviceProfile.get().cpuTopologyLine())
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
        // hardwareModel: 设备自报的硬件型号(常常就是 SoC 名) —— 读不到就写"取不到"
        Text('hardwareModel = ' + DeviceProfile.get().hardwareModelLine())
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 2 })
      } else {
        // 收起来时给一行提示, 不让人以为信息丢了
        // （被收起的这几行在「关于 → 设备画像 → SMT 超线程开关 / CPU 与芯片 / deviceInfo 逐字段」
        //   里原样都在, 用的是同一份文本）
        Text('窗口高度不足：诊断细节已收起（「关于」页的设备画像里一行不少）')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
      }
      Row() {
        Text(this.cores.toString() + ' 核心')
          .fontSize(this.fz(12))
          .fontColor(ACCENT)
          .backgroundColor(CHIP)
          .borderRadius(6)
          .padding({ left: 8, right: 8, top: 3, bottom: 3 })
        Text('原生 C++ 负载')
          .fontSize(this.fz(12))
          .fontColor(ACCENT2)
          .backgroundColor(C.chipViolet)
          .borderRadius(6)
          .padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .margin({ left: 8 })
      }
      .width('100%')
      .margin({ top: 12 })
    }
    .width(fill === 1 ? '100%' : this.cardW())
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    // 沉浸光感 b: 主卡片的光晕(关掉时 SHADOW_NONE)
    .shadow(this.fxAura(withAlpha(ACCENT_SOFT, 0.26), 12))
    // 沉浸光感(真家伙): 卡片的点光(跑分期间由 fxLight() 的第一句挡掉)
    .visualEffect(this.fxLight(ACCENT, 0.48, 0.18))
  }

  // 开始圆按钮: 直径由形态计划给(common/LayoutPlan.ets 的 heroDiaFor:
  //   clamp(窗口宽^0.58 x 6.5, 72, 300), 再被窗口高的竖向预算压过 -> LayoutPlan.heroDia),
  //   内圈与字号按这个直径成比例。这里不重算直径, 只按它摆图形。
  //   容器(= Stack)的边长 = startRingDia() = startDia; 外圈画笔直径 = startDia - 描边宽,
  //    所以圆环完全落在容器里, 不会被切掉(见 HERO_STROKE_VP 的说明)。
  @Builder
  startCircleInner() {
    Stack() {
      // 沉浸光感 a: 圆外那一层径向辉光。
      //  ★ 这个节点只在 fxOn() 为真时**才被创建** —— 跑分期间它根本不存在, 不是"存在但不播"。
      //    它的直径比容器大, 但容器自己的尺寸仍由下面 .width/.height(startRingDia()) 钉死,
      //    所以按钮的位置与大小一点都不受它影响(投影/P3 的圆环断言照旧成立)。
      if (this.fxOn()) {
        this.fxHalo(ACCENT, this.startDia + 14, 0.16)
      }
      Circle()
        .width(this.startDia - HERO_STROKE_VP)
        .height(this.startDia - HERO_STROKE_VP)
        .fill(C.heroOuter)
        .stroke(ACCENT)
        .strokeWidth(HERO_STROKE_VP)
        // 沉浸光感 b: 外圈的光晕。关掉时 fxAura() 给的是 SHADOW_NONE(半径 0 + 全透明)。
        .shadow(this.fxAura(withAlpha(ACCENT, 0.42), 10))
      // 内圈: 直径 = startInnerDia() = max(72, startDia - 20) —— 比外圈小 20vp,
      //  它的描边(2vp)也在外圈之内, 永远不会顶到容器边上。
      Circle()
        .width(this.startInnerDia())
        .height(this.startInnerDia())
        .fill(C.heroInner)
        .stroke(ACCENT_SOFT)
        .strokeWidth(2)
        .shadow(this.fxAura(withAlpha(ACCENT2, 0.34), 7))
      Column() {
        Text('开始')
          .fontSize(this.startMainFont())
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
        Text('跑分')
          .fontSize(this.startSubFont())
          .fontColor(ACCENT)
          .margin({ top: 2 })
      }
    }
    .width(this.startRingDia())
    .height(this.startRingDia())
    // 沉浸光感(真家伙): HDS 点光源打在整圈圆环上 —— 边框与内容一起点亮, 拉满。
    //  跑分期间 / 拿不到 HDS 能力时, fxLight() 第一句就返回"空效果": 点光一次都没被构造过。
    .visualEffect(this.fxLight(ACCENT, 0.80, 0.26))
    // 把圆心记下来(窗口坐标 vp), 只给粒子当发射点用。
    //  写的是普通字段(不是 @State) —— 它不参与任何渲染, 不会引起一次重排。
    .onAreaChange((_o: Area, n: Area) => {
      this.fxHx = this.fxNum(n.globalPosition.x) + this.fxNum(n.width) / 2;
      this.fxHy = this.fxNum(n.globalPosition.y) + this.fxNum(n.height) / 2;
    })
    //  ★ 这里原来挂着 .lightUpEffect(this.heroLift()) —— 已于 8.4 修复中删除。
    //    它是"纯装饰"里唯一一个在此构建/设备上会把节点画成黑块的属性, 见 syncFx() 上方的取证。
    //    删掉之后这个 Stack 的属性只剩下面的点击反馈与无障碍标注:
    //    尺寸仍来自 startRingDia()、位置仍由父级 Flex/Column 决定、onClick 原样保留。
    // 官方点击反馈: 按下时轻微缩放(一次性, 跑分期间为 null = 关闭)
    .clickEffect(this.fxClick())
    // 无障碍: 这个按钮只有图形与两个短词, 朗读文本写全, 并把内部图形并成一个朗读单元
    .accessibilityText('开始跑分：CPU 单核与多核七项负载，外加 GPU 四个场景')
    .accessibilityGroup(true)
    .accessibilityLevel('yes')
    .onClick(() => {
      this.fxHeroStart();
    })
  }

  // 开始按钮大圆 + 说明文字的说明文字列(两种排法共用同一份文字, 不许各写一份)
  //   文字列必须拿到一个确定的宽度: 它在 wrap-Flex 里当普通项时, 宽度会被圆挤到
  //    "内容列 - 圆 - 内外边距"(460vp 窗口上只剩 ~90vp), 40 多个字在那里折成七八行,
  //    相邻行挤在一起 —— 用户看到的 "『多核』附近字形重影" 就是这么来的。
  @Builder
  startHeroInfo() {
    Column() {
      Text('CPU 单核 + 多核 · 7 项负载 + GPU 4 场景')
        .fontSize(this.fz(12))
        .fontColor(SUB)
        .textAlign(TextAlign.Center)
      Text('跑分期间请保持亮屏，勿切换到其他应用')
        .fontSize(this.fz(11))
        .fontColor(FAINT)
        .textAlign(TextAlign.Center)
        .margin({ top: 8 })
    }
    .alignItems(HorizontalAlign.Center)
  }

  // 开始按钮大圆 + 说明文字。
  //  两种排法由架构决定(this.wide() 来自形态计划的 split, 只看实测窗口宽):
  //    宽窗: 圆与文字左右并排(文字列不设宽度 -> 一行放不下就自己换行, 永远拿满整行);
  //    窄窗: 上下堆叠, 文字列 .width('100%') = 内容列满宽。
  //  以前两种窗口共用同一个 wrap-Flex, 窄窗上文字被挤成一条窄柱 —— 见 startHeroInfo 的说明。
  @Builder
  startHero() {
    if (this.wide()) {
      Flex({
        direction: FlexDirection.Row,
        wrap: FlexWrap.Wrap,
        justifyContent: FlexAlign.Center,
        alignItems: ItemAlign.Center,
        alignContent: FlexAlign.Center
      }) {
        this.startCircleInner()
        Column() {
          this.startHeroInfo()
        }
        .alignItems(HorizontalAlign.Center)
        .padding({ left: 18, right: 18 })
      }
      .width('100%')
      .margin({ top: 8 })
    } else {
      Column() {
        this.startCircleInner()
        //  满宽: 窄窗上文字列拿满内容列宽, 只能整行折行, 不会被压成"一字一列"
        Column() {
          this.startHeroInfo()
        }
        .width('100%')
        .margin({ top: 14 })
      }
      .width('100%')
      .alignItems(HorizontalAlign.Center)
      .margin({ top: 26 })
    }
  }

  // 手动基准测试入口按钮区(压力测试 / CoreMark / GB7 系列)
  @Builder
  testButtonColumn() {
    Column() {
      Button(this.stressRunning ? ('压力测试 ' + this.stressRound.toString() + '/5') : '压力测试（5 轮，测持续性能与衰减）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(ACCENT2)
        .onClick(() => {
          this.startStress();
        })
      Button(this.coremarkRunning ? 'CoreMark 运行中（约 10 秒）…' : 'CoreMark 标准分（官方负载，可与各平台公开数据对照）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(GOOD)
        .margin({ top: 8 })
        .onClick(() => {
          this.startCoreMark();
        })
      Button(this.gb7Running ? 'CS1测试运行中…' : 'CS1测试（官方负载 · 按 CS1 单项分锚定）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(ACCENT)
        .margin({ top: 8 })
        .onClick(() => {
          this.startGb7();
        })
      if (this.coremarkResult.length > 0) {
        Text('CoreMark: ' + this.coremarkResult)
          .fontSize(this.fz(12))
          .fontColor(GOOD)
          .margin({ top: 8 })
      }
      Button(this.gb7MultiRunning ? ('CS1 多核复刻运行中（' + this.gb7Threads.toString() + ' 线程 · ' + this.smtButtonTag() + '）…') : 'CS1 多核复刻（官方 8 项 · 内部并行 · 给出加速比）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(WARN)
        .margin({ top: 8 })
        .onClick(() => {
          this.startGb7Multi();
        })
      Button(this.gb7GpuRunning ? 'CS1 GPU 复刻运行中…' : 'CS1 GPU 复刻（11 项 GPU 负载 · 离屏渲染）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(ACCENT2)
        .margin({ top: 8 })
        .onClick(() => {
          this.startGb7Gpu();
        })
      // GPU-SNL(Aurora Nomad Light): 独立小节 libaurorasn.so, 与 GB7 的 CPU/GPU 项无关, 不计分
      Button(this.snRunning ? 'GPU-SNL 运行中…' : 'GPU-SNL · Aurora Nomad Light（Nomad 风格 GPU 小节 · 不计分）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(GOOD)
        .margin({ top: 8 })
        .onClick(() => {
          this.startSn();
        })
      this.snSoloResultBlock()
      Button(this.scRunning
        ? ('长跑自检进行中 · ' + this.stressProgressText())
        : '长跑自检（可选 · 每轮约 30 秒，1000 轮需约 8 小时）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(WARN)
        .margin({ top: 8 })
        .enabled(!this.scRunning)
        .onClick(() => {
          this.openStressPicker();
        })
    }
    .width('100%')
  }

  // 结果卡 1: CS1 单核 + 锚定说明
  @Builder
  gb7SingleCard() {
    if (this.gb7Results.length > 0) {
      Column() {
        Row() {
          Text('CS1（单核）')
            .fontSize(this.fz(12))
            .fontColor(SUB)
          Blank()
          Text('综合 ' + this.gb7Composite.toFixed(0))
            .fontSize(this.fz(20))
            .fontWeight(FontWeight.Bold)
            .fontColor(ACCENT)
            .backgroundColor(CHIP)
            .borderRadius(Rd.sm)
            .padding({ left: Sp.x2, right: Sp.x2, top: 2, bottom: 2 })
        }
        .width('100%')
        Divider()
          .strokeWidth(HAIRLINE)
          .color(DIVIDER)
          .margin({ top: Sp.x2, bottom: Sp.x1 })
        if (this.memLow) {
          Text('可用内存 ' + this.memAvailMb.toString() + ' MB 低于 1.5 GB：CS1 单核的大内存负载可能异常退出（仅提示，不阻止运行）')
            .fontSize(this.fz(10))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 6 })
        }
        ForEach(this.gb7Results, (r: Gb7Result) => {
          // 单项 = 一行明细(+ 仅当"计分项跑失败"时再加一行失败原因)。
          // 用 Column 包住: ForEach 的每一项必须只有一个根组件。
          Column() {
            Row() {
              Text(r.name)
                .fontSize(this.fz(12))
                .fontColor(TEXT)
                .layoutWeight(1)
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
              Text(r.ms > 0 ? (r.ms.toFixed(0) + ' ms') : '')
                .fontSize(this.fz(10))
                .fontColor(BenchRunner.slowItem(r.ms) ? DANGER : SUB)
                .width(54)
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
                .textAlign(TextAlign.End)
              Text(metricUnit(r.metric, r.unit))
                .fontSize(this.fz(11))
                .fontColor(GOOD)
                .width(this.uiRes.detailMetricW)
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
                .textAlign(TextAlign.End)
              // "未计分"与"运行失败"分开: 按政策不计分(scored=false, 如 Structure from Motion)
              //   -> 中性色 + "未计分"; 计分项没跑出吞吐(scored=true 且 metric=0, 如 Clang)
              //   -> 报警色 + "失败"(下一行写明 native 自己给的失败原因)。
              Text(BenchRunner.itemCellText(r))
                .fontSize(this.fz(13))
                .fontColor(r.score > 0 ? ACCENT : (BenchRunner.itemScoredButFailed(r) ? DANGER : SUB))
                .width(60)
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
                .textAlign(TextAlign.End)
            }
            .width('100%')
            .padding({ top: 6, bottom: 6 })
            if (BenchRunner.itemScoredButFailed(r)) {
              Text('运行失败（计分项未跑出吞吐）：' + BenchRunner.itemFailReason(r))
                .fontSize(this.fz(10))
                .fontColor(DANGER)
                .lineHeight(15)
                .width('100%')
                .margin({ bottom: 6 })
            }
            // 可重复性 / 离散度: 每项一行, 没有数据/不稳的时候必须自己说出来(不假装稳定)
            Text(this.repeatItemLine(r))
              .fontSize(this.fz(10))
              .fontColor(this.repeatColor(r))
              .lineHeight(15)
              .width('100%')
              .margin({ bottom: 6 })
            // 本项是否随芯片性能变化(跨芯片可比的前提): 没做自检的项这一行为空, 不占位。
            if (this.scalingItemLine(r).length > 0) {
              Text(this.scalingItemLine(r))
                .fontSize(this.fz(10))
                .fontColor(this.scalingItemColor(r))
                .lineHeight(15)
                .width('100%')
                .margin({ bottom: 6 })
            }
          }
          .width('100%')
        }, (r: Gb7Result, idx: number) => 'gb7' + idx.toString())
        if (this.slowCount(this.gb7Results) > 0) {
          Text('耗时标红：本卡有 ' + this.slowCount(this.gb7Results).toString() +
            ' 项单项耗时 > 8000 ms，可能是负载卡死或系统降频。仅作提示，数值与计分不变。')
            .fontSize(this.fz(10))
            .fontColor(DANGER)
            .width('100%')
            .margin({ top: 6 })
        }
        // 绑核汇总: 只加这一行(不逐行加, 会挤); 只有"跑到了大核簇之外"才用警告色
        Text(this.cpuSummary(this.gb7Results))
          .fontSize(this.fz(10))
          .fontColor(this.cpuSummaryWarn(this.gb7Results) ? WARN : SUB)
          .width('100%')
          .margin({ top: 6 })
        // 可重复性汇总: 离散度超阈值的项在这里再点一次名(单项那一行已经逐项写明了)
        Text(this.repeatSummary(this.gb7Results))
          .fontSize(this.fz(10))
          .fontColor(this.repeatSummaryWarn(this.gb7Results) ? DANGER : SUB)
          .width('100%')
          .margin({ top: 6 })
        if (this.repeatBadLine(this.gb7Results).length > 0) {
          Text(this.repeatBadLine(this.gb7Results))
            .fontSize(this.fz(11))
            .fontColor(DANGER)
            .lineHeight(16)
            .width('100%')
            .margin({ top: 6 })
        }
        // 随芯片变化自检汇总(单核卡: 只有离散度这一条判据, 并行加速比要等多核卡)
        Text(this.scalingSummaryLine(this.gb7Results, this.gb7MultiResults))
          .fontSize(this.fz(11))
          .fontColor(this.scalingSummaryWarn(this.gb7Results, this.gb7MultiResults) ? DANGER : SUB)
          .lineHeight(16)
          .width('100%')
          .margin({ top: 6 })
        Text('单项分 = k × 本机吞吐（k 是本工程自定的换算常量，写死在代码里，不随机型调整）；综合分 = 各项的几何平均。单位语义对不上的项标「未计分」；本来是计分项、这一轮没跑出吞吐的标「失败」（报警色 + 原因）。 —— CS1 的分数只能和同样跑本 App CS1 的设备比，不能和任何第三方跑分软件的成绩比。')
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .width('100%')
          .margin({ top: 8 })
      }
      .width('100%')
      .padding(this.cardPad())
      .backgroundColor(CARD)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    }
  }

  // 结果卡 2: CS1 多核
  @Builder
  gb7MultiCard() {
    if (this.gb7MultiResults.length > 0) {
      Column() {
        Text('CS1（多核 · ' + this.gb7Threads.toString() + ' 线程 · ' +
          (BenchRunner.smtSummaryFlag(this.gb7Threads).length > 0
            ? ('SMT ' + BenchRunner.smtSummaryFlag(this.gb7Threads) + ' · ')
            : '') + '官方多核套件 8 项）')
          .fontSize(this.fz(12))
          .fontColor(SUB)
        if (this.memLow) {
          Text('可用内存 ' + this.memAvailMb.toString() + ' MB 低于 1.5 GB：CS1 多核会同时占满所有核心，大内存负载可能异常退出（仅提示，不阻止运行）')
            .fontSize(this.fz(10))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 6 })
        }
        ForEach(this.gb7MultiResults, (r: Gb7Result) => {
          // 多核卡没有"分数"列, 所以"计分项跑失败"在这里只用一行原因写明(颜色报警)。
          // 用 Column 包住: ForEach 的每一项必须只有一个根组件。
          Column() {
            Row() {
              Text(r.name)
                .fontSize(this.fz(12))
                .fontColor(TEXT)
                .layoutWeight(1)
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
              Text(r.ms > 0 ? (r.ms.toFixed(0) + ' ms') : '')
                .fontSize(this.fz(10))
                .fontColor(BenchRunner.slowItem(r.ms) ? DANGER : SUB)
                .width(62)
                .textAlign(TextAlign.End)
              Text(metricUnit(r.metric, r.unit))
                .fontSize(this.fz(11))
                .fontColor(BenchRunner.itemScoredButFailed(r) ? DANGER : GOOD)
                .width(this.uiRes.detailMetricW)
                .textAlign(TextAlign.End)
              Text(this.gb7Speedup(r.name, r.metric))
                .fontSize(this.fz(12))
                .fontColor(WARN)
                .width(64)
                .textAlign(TextAlign.End)
            }
            .width('100%')
            .padding({ top: 6, bottom: 6 })
            if (BenchRunner.itemScoredButFailed(r)) {
              Text('运行失败（计分项未跑出吞吐）：' + BenchRunner.itemFailReason(r))
                .fontSize(this.fz(10))
                .fontColor(DANGER)
                .lineHeight(15)
                .width('100%')
                .margin({ bottom: 6 })
            }
            // 可重复性 / 离散度: 每项一行, 没有数据/不稳的时候必须自己说出来(不假装稳定)
            Text(this.repeatItemLine(r))
              .fontSize(this.fz(10))
              .fontColor(this.repeatColor(r))
              .lineHeight(15)
              .width('100%')
              .margin({ bottom: 6 })
            // 本项是否随芯片性能变化(多核口径: 并行加速比 + 轮间离散度)
            if (this.scalingItemLine(r).length > 0) {
              Text(this.scalingItemLine(r))
                .fontSize(this.fz(10))
                .fontColor(this.scalingItemColor(r))
                .lineHeight(15)
                .width('100%')
                .margin({ bottom: 6 })
            }
          }
          .width('100%')
        }, (r: Gb7Result, idx: number) => 'gb7m' + idx.toString())
        if (this.slowCount(this.gb7MultiResults) > 0) {
          Text('耗时标红：本卡有 ' + this.slowCount(this.gb7MultiResults).toString() +
            ' 项单项耗时 > 8000 ms，可能是负载卡死或系统降频。仅作提示，数值与计分不变。')
            .fontSize(this.fz(10))
            .fontColor(DANGER)
            .width('100%')
            .margin({ top: 6 })
        }
        // 绑核汇总(多核): 只有"跑到了大核簇之外"才用警告色
        Text(this.cpuSummary(this.gb7MultiResults))
          .fontSize(this.fz(10))
          .fontColor(this.cpuSummaryWarn(this.gb7MultiResults) ? WARN : SUB)
          .width('100%')
          .margin({ top: 6 })
        // 可重复性汇总(多核): 与单核卡同一套口径, 不假装稳定
        Text(this.repeatSummary(this.gb7MultiResults))
          .fontSize(this.fz(10))
          .fontColor(this.repeatSummaryWarn(this.gb7MultiResults) ? DANGER : SUB)
          .width('100%')
          .margin({ top: 6 })
        if (this.repeatBadLine(this.gb7MultiResults).length > 0) {
          Text(this.repeatBadLine(this.gb7MultiResults))
            .fontSize(this.fz(11))
            .fontColor(DANGER)
            .lineHeight(16)
            .width('100%')
            .margin({ top: 6 })
        }
        // 随芯片变化自检汇总(多核卡: 单核 + 多核两个口径合起来判, 与报告 D2 节同源)
        Text(this.scalingSummaryLine(this.gb7Results, this.gb7MultiResults))
          .fontSize(this.fz(11))
          .fontColor(this.scalingSummaryWarn(this.gb7Results, this.gb7MultiResults) ? DANGER : SUB)
          .lineHeight(16)
          .width('100%')
          .margin({ top: 6 })
      }
      .width('100%')
      .padding(this.cardPad())
      .backgroundColor(CARD)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    }
  }

  // 结果卡 3: CS1 GPU
  @Builder
  gb7GpuCard() {
    if (this.gb7GpuResults.length > 0) {
      Column() {
        Row() {
          Text('CS1（GPU）')
            .fontSize(this.fz(12))
            .fontColor(SUB)
          Blank()
          Text(this.gb7GpuComposite > 0 ? ('综合 ' + this.gb7GpuComposite.toFixed(0)) : '未计分')
            .fontSize(this.fz(20))
            .fontWeight(FontWeight.Bold)
            .fontColor(ACCENT)
            .backgroundColor(CHIP)
            .borderRadius(Rd.sm)
            .padding({ left: Sp.x2, right: Sp.x2, top: 2, bottom: 2 })
        }
        .width('100%')
        Divider()
          .strokeWidth(HAIRLINE)
          .color(DIVIDER)
          .margin({ top: Sp.x2, bottom: Sp.x1 })
        ForEach(this.gb7GpuResults, (r: Gb7Result) => {
          Row() {
            Text(r.name)
              .fontSize(this.fz(12))
              .fontColor(TEXT)
              .layoutWeight(1)
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Text(metricUnit(r.metric, r.unit))
              .fontSize(this.fz(11))
              .fontColor(GOOD)
              .width(this.uiRes.detailMetricW)
              .textAlign(TextAlign.End)
            Text(r.ms > 0 ? (r.ms.toFixed(0) + ' ms') : '')
              .fontSize(this.fz(11))
              .fontColor(BenchRunner.slowItem(r.ms) ? DANGER : SUB)
              .width(70)
              .textAlign(TextAlign.End)
          }
          .width('100%')
          .padding({ top: 6, bottom: 6 })
        }, (r: Gb7Result, idx: number) => 'gb7gpu' + idx.toString())
      }
      .width('100%')
      .padding(this.cardPad())
      .backgroundColor(CARD)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    }
  }

  // 结果卡 4: 压力测试各轮总分与稳定性
  @Builder
  stressCard() {
    if (this.stressTotals.length > 0) {
      Column() {
        Text('压力测试：各轮总分')
          .fontSize(this.fz(11))
          .fontColor(SUB)
        Row() {
          ForEach(this.stressTotals, (v: number, i: number) => {
            Column() {
              Text(v.toString())
                .fontSize(this.fz(12))
                .fontColor(i === 0 ? GOOD : TEXT)
              Text('第' + (i + 1).toString() + '轮')
                .fontSize(this.fz(10))
                .fontColor(FAINT)
            }
            .layoutWeight(1)
          }, (v: number, i: number) => i.toString())
        }
        .width('100%')
        .margin({ top: 6 })
        Row() {
          ForEach(this.stressThermal, (v: number, i: number) => {
            Text(['凉爽', '正常', '偏热', '过热', '降级', '警告', '紧急'][v >= 0 && v <= 6 ? v : 1])
              .fontSize(this.fz(9))
              .fontColor(v >= 3 ? DANGER : SUB)
              .layoutWeight(1)
              .textAlign(TextAlign.Center)
          }, (v: number, i: number) => 'th' + i.toString())
        }
        .width('100%')
        .margin({ top: 4 })
        Text('稳定性 ' + this.stabilityText() + '（最差轮 / 最好轮，3DMark 及格线 97%）')
          .fontSize(this.fz(11))
          .fontColor(this.stabilityPass() ? GOOD : WARN)
          .margin({ top: 4 })
        Text(this.stabilityPass() ? '通过：持续性能稳定' : '未通过：存在明显降频/热限频')
          .fontSize(this.fz(10))
          .fontColor(this.stabilityPass() ? GOOD : DANGER)
          .margin({ top: 2 })
      }
      .width('100%')
      .padding(this.cardPad())
      .backgroundColor(CARD)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    }
  }

  // 上次成绩卡: 宽屏下与其它结果卡一起两列排布
  @Builder
  lastCardInner(fill: number) {
    if (this.records.length > 0) {
      Column() {
        Row() {
          Text('上次成绩')
            .fontSize(this.fz(12))
            .fontColor(SUB)
            .maxLines(1)
          Blank()
          Text(fmtTime(this.records[0].time))
            .fontSize(this.fz(11))
            .fontColor(FAINT)
            .maxLines(1)
        }
        .width('100%')
        // 首页分数卡(历史里的"上次成绩") 与结果页同一份密度: 大数字用同一档字号,
        //  块间距用 uiRes.gap(改前这里写死 6 / 14, 在矮窗口上白占十几 vp)。
        //  这一行不许出现"文字压在文字上": 这一行里三个单元(总分大数字 / 相对基线 + 总分 /
        //    最高分)在窄卡片上会互相挤 —— 以前它们都没有 maxLines, 挤不下就各自折行,
        //    两行叠在一起就是用户看到的『相对基线 …』『最高 …』重影。现在:
        //      · 总分大数字: maxLines(1) + minFontSize —— 挤不下就缩字号, 不折行;
        //      · 相对基线 / 最高分: maxLines(1) + 省略号 —— 挤不下就截断, 不折成两行压住别人。
        Row() {
          Text(this.records[0].total.toString())
            .fontSize(this.uiRes.giant)
            .minFontSize(Math.max(24, Math.round(this.uiRes.giant * 0.55)))
            .maxFontSize(this.uiRes.giant)
            .heightAdaptivePolicy(TextHeightAdaptivePolicy.MIN_FONT_SIZE_FIRST)
            .fontWeight(FontWeight.Bold)
            .maxLines(1)
            .fontColor(BenchRunner.tierColor(this.records[0].total))
          Column() {
            Text('相对基线 ' + (this.records[0].total / 1000).toFixed(2) + ' 倍')
              .fontSize(this.fz(12))
              .fontColor(TEXT)
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Text('总分')
              .fontSize(this.fz(11))
              .fontColor(SUB)
              .maxLines(1)
          }
          .alignItems(HorizontalAlign.Start)
          .margin({ left: 8, top: 4 })
          Blank()
          Text('最高 ' + this.best.toString())
            .fontSize(this.fz(12))
            .fontColor(WARN)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%')
        .alignItems(VerticalAlign.Bottom)
        .margin({ top: this.uiRes.gap })
        Row() {
          this.miniScore('单核', this.records[0].single, ACCENT)
          this.miniScore('多核', this.records[0].multi, ACCENT2)
          this.miniScore('GPU', this.records[0].gpu, GOOD)
        }
        .width('100%')
        .margin({ top: this.uiRes.gap })
      }
      .width(fill === 1 ? '100%' : this.cardW())
      .padding(this.cardPad())
      .backgroundColor(CARD)
      .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
      .onClick(() => {
        this.shown = this.records[0];
        // ④ 粒子: 结果出现, 屏幕正中聚拢
        this.fxResultBurst();
      })
    }
  }

  // 首页「上次成绩」卡里的三个小数字块(单核 / 多核 / GPU): 一行三张, 等分(Row + layoutWeight(1))。
  //   两个硬约束, 缺一个就会出真机上那种"一位数字一行":
  //    ① 数字必须 maxLines(1): 没有它, 块一窄就按字符折行 ——
  //       用户看到的『单核』下面是竖排的 2/3/6、『多核』下面是 3/./8 就是这个;
  //    ② 块窄到放不下时就缩字号(minFontSize + MIN_FONT_SIZE_FIRST), 不折行、不溢出。
  //  字号取自结果页密度(uiRes.keyName / keyNum): 这张卡的注释一直写着"与结果页同一份密度",
  //  但改前这里写的是 fz(11) / fz(17) —— 与窗口无关的固定令牌, 与注释说的不是一回事。
  @Builder
  miniScore(label: string, value: number, color: string) {
    Column() {
      Text(label)
        .fontSize(this.uiRes.keyName)
        .fontColor(SUB)
        .maxLines(1)
      Text(value > 0 ? value.toString() : '--')
        .fontSize(this.uiRes.keyNum)
        .minFontSize(Math.max(12, Math.round(this.uiRes.keyNum * 0.7)))
        .maxFontSize(this.uiRes.keyNum)
        .heightAdaptivePolicy(TextHeightAdaptivePolicy.MIN_FONT_SIZE_FIRST)
        .maxLines(1)
        .fontColor(color)
        .margin({ top: 3 })
    }
    .layoutWeight(1)
    .alignItems(HorizontalAlign.Center)
  }

  // 本次会话有没有"结果区"的内容(GB7 三张卡 / 压力测试各轮 / 一键跑分的三个大数字)。
  //   为什么需要它: 宽窗口(1440x960 这种)首页是左右分栏, 右栏(7~8 栅格 = 一大半屏宽)
  //    在"还没跑过"时一张卡都没有 —— 用户看到的是一大片空 + 左边一列窄内容, 很像布局坏了。
  //    真实原因不是布局坏了, 而是"没有结果时右栏本来就空": 那就要给出有信息量的空态
  //    (这里会出现什么 / 现在该做什么 / 上次成绩与最高分), 而不是留一大片黑。
  private resultZoneEmpty(): boolean {
    return this.gb7Results.length === 0 && this.gb7MultiResults.length === 0 &&
      this.gb7GpuResults.length === 0 && this.stressTotals.length === 0 &&
      this.fullL1.length === 0;
  }

  // 结果区的空态卡: 说明"这里将来会出现什么"+"现在该做什么", 并把历史里的上次成绩摆在旁边
  //  (有历史就照实显示, 没有就只说该做什么 —— 不编数字)
  @Builder
  resultZoneEmptyCard() {
    Column() {
      Text('结果区 · 还没有本次结果')
        .fontSize(this.fz(12))
        .fontColor(SUB)
        .width('100%')
      Text('跑完一次「一键全部跑分」后，这里会出现 CS1 单核 / 多核 / GPU 三张卡（每项带单位、' +
        '耗时与取得条件）；下面的「长跑自检」也可以单独跑。')
        .fontSize(this.fz(11))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 8 })
      Text(this.records.length > 0
        ? ('上次成绩：总分 ' + this.records[0].total.toString() + '（' + fmtTime(this.records[0].time) +
          '）· 最高 ' + this.best.toString() + ' · 单核 ' + this.records[0].single.toString() +
          ' / 多核 ' + this.records[0].multi.toString() + ' / GPU ' + this.records[0].gpu.toString() +
          '（点这张卡看完整结果）')
        : '这一台还没有任何历史成绩：先点上面的「一键全部跑分」。')
        .fontSize(this.fz(11))
        .fontColor(this.records.length > 0 ? ACCENT : FAINT)
        .width('100%')
        .margin({ top: 8 })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .onClick(() => {
      if (this.records.length > 0) {
        this.shown = this.records[0];
        // ④ 粒子: 结果出现, 屏幕正中聚拢
        this.fxResultBurst();
      }
    })
  }

  // 结果卡区: sm 单列 / md lg 自动两列(每张卡 flexGrow 填满半行)
  @Builder
  homeResultZone() {
    Flex({
      direction: FlexDirection.Row,
      wrap: FlexWrap.Wrap,
      justifyContent: FlexAlign.Center
    }) {
      // 空态: 本会话还没有任何结果时, 先摆一张有信息量的卡(见 resultZoneEmptyCard),
      //  而不是让右栏空着 —— 宽窗口下右栏占屏幕一多半, 空着看起来就像布局坏了。
      if (this.resultZoneEmpty()) {
        Column() {
          this.resultZoneEmptyCard()
        }
        .flexGrow(1)
        .flexBasis('100%')
      }

      Column() {
        this.gb7SingleCard()
      }
      .flexGrow(1)
      .flexBasis(this.cardBasis())
      .constraintSize({ minWidth: this.minCardW() })

      Column() {
        this.gb7MultiCard()
      }
      .flexGrow(1)
      .flexBasis(this.cardBasis())
      .constraintSize({ minWidth: this.minCardW() })

      Column() {
        this.gb7GpuCard()
      }
      .flexGrow(1)
      .flexBasis(this.cardBasis())
      .constraintSize({ minWidth: this.minCardW() })

      // 长跑自检入口(原「稳定性压测」): 保留功能但降级 ——
      // 只放在 GB7 三张卡的下方, 不占开始区的位置、也不用醒目配色。
      Column() {
        this.stressCampaignCard()
      }
      .flexGrow(1)
      .flexBasis('100%')
      .margin({ top: this.rowGap() })

      Column() {
        this.stressCard()
      }
      .flexGrow(1)
      .flexBasis(this.cardBasis())
      .constraintSize({ minWidth: this.minCardW() })
    }
    .width('100%')
    .margin({ top: 10 })
  }

  @Builder
  homeTab() {
    Scroll() {
      Column() {
        Column() {
          this.crashBanner()
          // 第一优先 「一键全部跑分」放在整页最上面: 通栏 + 主色 2vp 描边 + 整行大按钮。
          // 打开首页第一眼看到的就是它 —— 用户不需要在几十个按钮里找"该按哪个"。
          this.fullRunCard()
          // 跑完之后: 首页直接给第一层的三个大数字(单核 / 多核 / GPU), 一眼就能读
          if (this.fullL1.length > 0) {
            Column() {
              this.oneTapPreview()
            }
            .width('100%')
            .margin({ top: this.rowGap() })
          }
          this.deviceCardInner(0)
          this.memCard()
          // 原有各项单独入口(单次完整跑分 / 压力测试 / CoreMark / CS1 单核 · 多核 · GPU / 长跑自检)
          // 一个都没删, 但降级为次要入口: 默认收起, 点这一行标题展开。
          this.soloEntryHeader()
          if (this.showSolo) {
            this.startHero()
          }
          // 首页主分栏: 左"操作" / 右"结果"。
          //   2026-10 改法: 栅格直接取形态计划给的 leftSpan/rightSpan, 不再按断点另写一套。
          //    以前这里按 mediaquery 断点写死了 5:7 / 4:8 / 3:9, 而计划里是按实测窗口宽
          //    算的比例 —— 两套规则只要有一点不一致, 就会出现"关于页说 5:7、界面排成 4:8",
          //    自检块就白做了。现在只有一个来源: LayoutPlan.planFor()。
          //    breakpoints 的边界(600 / 1240vp)与计划里的 W_SPLIT_MIN / DIV_SPAN_WIDE_AT 同值:
          //      < 600vp  -> 12:12(不分栏)
          //      600~1240 -> 5:7(计划: 内容列 < 1100vp -> 左 5 / 右 7)
          //      >= 1240  -> 4:8(计划: 内容列 >= 1100vp -> 左 4 / 右 8)
          if (this.showSolo) {
            GridRow({
              columns: 12,
              gutter: { x: this.rowGap(), y: this.rowGap() },
              breakpoints: { value: ['320vp', '600vp', '1240vp'] }
            }) {
              GridCol({
                span: { xs: 12, sm: 12, md: this.spanLeft, lg: this.spanLeft,
                  xl: this.spanLeft, xxl: this.spanLeft }
              }) {
                Column() {
                  this.startCardPanel()
                }
                .width('100%')
              }
              GridCol({
                span: { xs: 12, sm: 12, md: this.spanRight, lg: this.spanRight,
                  xl: this.spanRight, xxl: this.spanRight }
              }) {
                Column() {
                  this.homeResultZone()
                }
                .width('100%')
              }
            }
            .width('100%')
            .margin({ top: this.rowGap() })
          } else {
            // 单项入口收起时, 结果卡区独占整宽(不再给空的左栏留位置)
            Column() {
              this.homeResultZone()
            }
            .width('100%')
          }
          Text('跑分期间请保持亮屏，勿切换到其他应用')
            .fontSize(this.fz(11))
            .fontColor(FAINT)
            .margin({ top: 18, bottom: 24 })
        }
        .width('100%')
        .constraintSize({ maxWidth: this.contentMax() })
        .alignItems(HorizontalAlign.Center)
      }
      .width('100%')
    }
    .width('100%')
    .layoutWeight(1)
    .scrollBar(BarState.Off)
    .edgeEffect(EdgeEffect.Spring)
    // 官方边缘渐隐(fadingEdge): 内容在视口上下边缘自然淡出 —— 静态裁切效果, 不是动画,
    // 只在滚动时随内容重绘, 跑分期间(无人滚动)不产生任何开销。
    .fadingEdge(true)
    // 官方转场(一次性): 切换标签时淡入并轻微上移。用户操作触发, 180ms 结束;
    // 跑分期间 fxTransition() 返回 IDENTITY(无动画) —— 见该函数。
    .transition(this.fxTransition())
  }

  @Builder
  startCardPanel() {
    Column() {
      Text('单次完整跑分')
        .fontSize(this.fz(12))
        .fontColor(SUB)
        .width('100%')
      Text('CPU 单核 + 多核七项负载与 GPU 四场景一次跑完，结果自动计入历史与最高分。')
        .fontSize(this.fz(11))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 8 })
      Flex({
        direction: FlexDirection.Row,
        justifyContent: FlexAlign.Center
      }) {
        this.lastCardInner(1)
      }
      .width('100%')
      .margin({ top: 12 })
      Button(this.stressRunning ? ('压力测试 ' + this.stressRound.toString() + '/5') : '压力测试（5 轮，测持续性能与衰减）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(ACCENT2)
        .margin({ top: 12 })
        .onClick(() => {
          this.startStress();
        })
      Button(this.coremarkRunning ? 'CoreMark 运行中（约 10 秒）…' : 'CoreMark 标准分（官方负载，可与各平台公开数据对照）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(GOOD)
        .margin({ top: 8 })
        .onClick(() => {
          this.startCoreMark();
        })
      Button(this.gb7Running ? 'CS1测试运行中…' : 'CS1测试（官方负载 · 按 CS1 单项分锚定）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(ACCENT)
        .margin({ top: 8 })
        .onClick(() => {
          this.startGb7();
        })
      if (this.coremarkResult.length > 0) {
        Text('CoreMark: ' + this.coremarkResult)
          .fontSize(this.fz(12))
          .fontColor(GOOD)
          .margin({ top: 8 })
      }
      Button(this.gb7MultiRunning ? ('CS1 多核复刻运行中（' + this.gb7Threads.toString() + ' 线程 · ' + this.smtButtonTag() + '）…') : 'CS1 多核复刻（官方 8 项 · 内部并行 · 给出加速比）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(WARN)
        .margin({ top: 8 })
        .onClick(() => {
          this.startGb7Multi();
        })
      Button(this.gb7GpuRunning ? 'CS1 GPU 复刻运行中…' : 'CS1 GPU 复刻（11 项 GPU 负载 · 离屏渲染）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(ACCENT2)
        .margin({ top: 8 })
        .onClick(() => {
          this.startGb7Gpu();
        })
      // GPU-SNL(Aurora Nomad Light): 独立小节 libaurorasn.so, 与 GB7 的 CPU/GPU 项无关, 不计分
      Button(this.snRunning ? 'GPU-SNL 运行中…' : 'GPU-SNL · Aurora Nomad Light（Nomad 风格 GPU 小节 · 不计分）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(13))
        .backgroundColor(CHIP)
        .fontColor(GOOD)
        .margin({ top: 8 })
        .onClick(() => {
          this.startSn();
        })
      this.snSoloResultBlock()
      // 两份报告对比(用户最需要的那个入口): 放在「单项测试」折叠区里, 不抢主流程的位置。
      this.cmpCard()
      // 长跑自检(原「稳定性压测」)已降级: 不再放在这里抢眼, 入口移到 GB7 三张卡的下方。
      Text('需要长时间稳定性自检时，用结果区 CS1 三张卡下方的「长跑自检」入口。')
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 12 })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
  }

  // ---- 历史 ----
  // 一次跑分一张卡, 时间倒序。卡片里**四段**(2026-10-05 真机反馈后重做成大字版;
  //  2026-10-06 用户看了结果页截图后定稿: 上半部分照旧, 下半部分换成"一行一项"的清单):
  //   ① 一行小字: 时间(人话) + 设备(短名);
  //   ② 上半部分: 三个大数字(单核 / 多核 / GPU) —— 就是结果页第一层那三个数, 颜色与字体全部照旧;
  //   ③ 下半部分: 一行一项的清单(六行: 自研三项 / GPU-SNL / NPU / 存储) ——
  //      每行只有"项目名(白) + 分数(蓝)"两样, 没有单位、没有括号里的条件、没有灰色小字;
  //   ④ 有异常时再加一行极淡的异常说明, 正常就什么都不显示。
  //  ★ 2026-10(10.0)位置对调: **上面那三个大数字是 CS1**, 下面清单前三行是自研套件
  //    (用户:"把分数的主体突出部位变成 CS1…把这几个字眼的组件放到现在 CS1 的位置")。
  //    两边都是"换个取数就行", 排版零件一个都没动。
  //  ★ 三个大数字走的是同一个排版零件(histBigRow -> histBigNum): 字号(this.uiRes.huge)、
  //    颜色规则(cardTone)与结果页第一层 bigScoreCard() 逐个 token 同源(见 Q 节断言);
  //    "取哪三个数"一律走 ResultLayers.buildBigThree()。行首那枚极小的「CS1」标签是必要的:
  //    下面清单前三行是「自研 单核 / 多核 / GPU」, 三个大数字不写清是哪一组会看串。
  //  ★ 清单的项名与顺序照结果页的关键得分清单, 数字也用结果页那几个排版函数
  //    (buildBigThree -> num0 / num1 / num2), 所以卡片上那个数与结果页、与报告里的是同一批。
  //  Δ 改前(2026-10-05)只有自研那三个数 + 右下角总分; 2026-10-06 上午把 CS1 三个分加成第二行大数字;
  //    用户看到结果页截图后要求"精简一点, 只保留项目名和分数" —— 于是 CS1 三个分从"第二行大数字"
  //    变成清单里的三行, 右下角总分整行收掉(用户说它与上面三个大数字重复), 单位 / 条件 /
  //    "不计分"一律不上卡片。
  //    版本列、耗时列、密集表头仍然一个都没有; 口径那句仍然只有列表最后极淡的一行。
  //  ★ 版本号不在 App 内显示, 但**仍然写在「下载/Crush分/历史成绩.txt」里**(那张表是给文件看的,
  //    信息全一点是对的, 而且它承担着"8.2 之前多核口径不同、不要跨版本比"的提醒)。
  //  数据就是 runhistory.jsonl 的同一批行(RunLog.historyRowsNewestFirst) ——
  //  与「历史成绩.txt」同源, 不另建一份数据。
  //  这一页是按需渲染: 只在切到本页 / 整轮跑完之后读一次盘, 跑分期间不碰它, 也没有任何动画。
  @Builder
  historyRowItem(r: RunHistoryRow) {
    Column() {
      // ① 一行小字: 时间 + 设备
      Row() {
        Text(this.histTimeText(r.startedAt))
          .fontSize(this.uiRes.keyName)
          .fontColor(ACCENT)
        Text(' · ' + this.histDeviceText(r.device))
          .fontSize(this.uiRes.keyName)
          .fontColor(SUB)
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      // ② 上半部分: 三个大数字(单核 / 多核 / GPU) —— 就是结果页第一层那三个数,
      //    颜色(cardTone)与字体(this.uiRes.huge)一个 token 都没动。
      this.histBigRow('CS1', this.histBig(r))
      // ③ 下半部分: 一行一项的清单(六行) —— 每行只有"项目名(白) + 分数(蓝)"两样,
      //    没有单位、没有括号里的条件、没有灰色小字。
      //    原来右下角那个"总分"也收掉了: 用户说它就是上面那三个大数字("重复"), 去掉它
      //    才腾出了"一屏 2 张"要的那一行高度(见 histKeyRows 的注释)。
      Column() {
        ForEach(this.histKeyRows(r), (row: KeyRow, i: number) => {
          this.histKeyLine(row)
        }, (row: KeyRow, i: number) => 'hk' + i.toString())
      }
      .width('100%')
      .margin({ top: Math.round(this.uiRes.gap / 2) })
      // 异常才出现的一行(正常时这一行根本不存在 —— 不写"正常")
      if (this.histNoteText(r).length > 0) {
        Text(this.histNoteText(r))
          .fontSize(this.uiRes.keyUnit)
          .fontColor(DANGER)
          .width('100%')
          .margin({ top: Math.round(this.uiRes.gap / 2) })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .onClick(() => {
      this.openHistoryReport(r);
    })
  }

  // 历史卡片上半部分:**一组三个大数字** = 一枚极小的行标签 + 三个大数字(单核 / 多核 / GPU)。
  //  ★ 它只负责"怎么画": 字号 / 颜色规则 / 间距都在这里, 传进来的 cards 只管"是哪三个数"
  //    (由 buildBigThree() 决定)。要再摆一组别的三个数, 换一个 cards 就行 —— 样式不会分叉。
  //    行标签用单位那一档的字号(this.uiRes.keyUnit)、最淡的颜色(FAINT)、靠左: 它是"这三个数是谁"
  //    的界碑, 比数字本身小得多, 不跟三个大数字抢位置; 卡片下面清单第一行就是「CS1 单核」,
  //    没有这枚标签会把上面那三个数看成 CS1 的。
  @Builder
  histBigRow(tag: string, cards: ScoreCard[]) {
    Column() {
      Text(tag)
        .fontSize(this.uiRes.keyUnit)
        .fontColor(FAINT)
        // 显式行高: 标签这一行的高度变成"算得出来"的(1.25 x keyUnit), 卡片高度这笔账
        // 才能被 Q 节的断言核对着算 —— 也不至于按默认行高白白多占几 vp。
        .lineHeight(Math.round(this.uiRes.keyUnit * 1.25))
        .width('100%')
        .textAlign(TextAlign.Start)
        .padding({ left: 3 })
      // 2026-10-06 修: 这里原来是 wrap: Wrap + flexBasis(l1Basis) + minWidth(l1MinW) ——
      //  在真机(Pura X Max, 665x940vp)历史页是**分栏**, 一张卡只有 ~310vp 宽, 而
      //  l1MinW 比 310/3 大, 于是三个大数字**换行成了三行**, 卡片高度是估算值的 3 倍,
      //  一屏连一张都放不下(用户截图: 单核/多核/GPU 竖着排)。
      //  改成**不换行**的等分三列: 个数恒为 3, 每个 layoutWeight(1) 拿 1/3 宽,
      //  数字放不下时由 histBigNum 里已有的 minFontSize(= huge x 0.55)+ maxLines(1)
      //  + MIN_FONT_SIZE_FIRST 自己缩字号, 绝不会换行 —— 于是"一行大数字"这件事
      //  变成排版上不可能违反的, Q 节的高度账也才算得准。
      Row() {
        ForEach(cards, (c: ScoreCard) => {
          Column() {
            this.histBigNum(c)
          }
          .layoutWeight(1)
          .constraintSize({ minWidth: 0 })
        }, (c: ScoreCard) => tag + c.key)
      }
      .width('100%')
      .alignItems(VerticalAlign.Top)
    }
    .width('100%')
    .margin({ top: Math.round(this.uiRes.gap / 2) })
  }

  // 卡片**上面**那一行的三个大数字 —— **CS1**(单核 / 多核 / GPU)。
  //  ★ 2026-10(10.0)位置对调: 这一行原来是自研套件, 现在给 CS1; 自研那三个数搬到下面的清单里。
  //  走 ResultLayers.buildBigThree(): 与结果页第一层、首页「最近一次跑分」**同一个函数** ——
  //  标签、单位、数字排版(num0)、"没拿到分就写 --" 的口径不可能再分叉。
  //  取数只取账本行里已有的键(小写 gb8* —— 这一版**一个键名都没改**, 老记录照常读)。
  private histBig(r: RunHistoryRow): ScoreCard[] {
    return buildBigThree(r.gb8Single, r.gb8Multi, r.gb8Gpu);
  }

  // 历史卡片里那一个"大数字"的排版零件。
  //  ★ 它的 label / 数字 / 单位三段与结果页的 bigScoreCard() **逐个 token 相同**
  //    (keyName / huge / minFontSize = huge*0.55 / maxFontSize / MIN_FONT_SIZE_FIRST /
  //     maxLines(1) / Bold / cardTone / keyUnit / bottom = keyUnit*0.35 / padding 3),
  //    Q 节有一条断言在比对这两处用到的 this.uiRes.* 令牌集合, 少一个就变红。
  //  唯一刻意不同的是: 这里**不画**「取得条件」那一行 —— 历史卡片下面还有一整列清单,
  //    一屏仍要看得见至少 2 张(Q 节有一条断言按实测窗口把这笔高度账算过);
  //    "单核/多核/GPU 是在什么条件下拿到的"由点开卡片后的那一份完整报告回答。
  @Builder
  histBigNum(c: ScoreCard) {
    Column() {
      Text(c.label)
        .fontSize(this.uiRes.keyName)
        .fontColor(SUB)
      Row() {
        Text(c.value)
          .fontSize(this.uiRes.huge)
          .minFontSize(Math.max(16, Math.round(this.uiRes.huge * 0.55)))
          .maxFontSize(this.uiRes.huge)
          .heightAdaptivePolicy(TextHeightAdaptivePolicy.MIN_FONT_SIZE_FIRST)
          .maxLines(1)
          .fontWeight(FontWeight.Bold)
          .fontColor(c.has ? this.cardTone(c.key) : FAINT)
        Text(c.unit)
          .fontSize(this.uiRes.keyUnit)
          .fontColor(SUB)
          .margin({ left: 2, bottom: Math.round(this.uiRes.keyUnit * 0.35) })
      }
      .alignItems(VerticalAlign.Bottom)
      .justifyContent(FlexAlign.Center)
    }
    .width('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .padding({ left: 3, right: 3 })
  }

  // 下半部分清单里的**一行**: 只有两样东西 —— 项目名(白) + 分数(蓝)。
  //  ★ 这是用户 2026-10-06 明确要求的形态("只保留项目名和分数"): 所以这里**没有**单位、
  //    没有括号里的取得条件、没有任何灰色小字, 整行正好两个 Text。
  //    蓝色用的是结果页关键得分清单里那个蓝(ACCENT, 也就是 CS1 那几行分数的颜色);
  //    项名用 TEXT(白)。缺数时那一格是 '--', 仍然是蓝色 —— 一行里只有这两种颜色。
  //    字号仍取结果页清单的那两个令牌(项名 keyName / 分数 keyNum), 不另造一套。
  @Builder
  histKeyLine(row: KeyRow) {
    Row() {
      Text(row.name)
        .fontSize(this.uiRes.keyName)
        .fontColor(TEXT)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .layoutWeight(1)
      Text(row.value)
        .fontSize(this.uiRes.keyNum)
        .fontWeight(FontWeight.Bold)
        .fontColor(ACCENT)
        .maxLines(1)
    }
    .width('100%')
    .alignItems(VerticalAlign.Bottom)
  }

  // 这一条历史记录下半部分那六行(项名 + 分数)。
  //  ★ 项名与顺序照**结果页的关键得分清单**(buildKeyRows)里的那六项:
  //    自研 单核 / 自研 多核 / 自研 GPU / GPU-SNL / NPU 推理 / 存储 I/O。
  //    (2026-10 / 10.0 位置对调: 前三行原是 CS1, 现在 CS1 是上面那三个大数字。)
  //  ★ 结果页清单的第一行「自研套件总分」**刻意不放进这张卡**: 它会让卡片多出一行
  //    (18vp), 在 460x672 这样的窄窗上"一屏 2 张"就正好被挤掉了(Q9 按实测窗口算过这笔账)。
  //    总分本身没有丢 —— 它仍然在结果页清单第一行、「历史成绩.txt」的总分列、以及那一份报告里。
  //  ★ 数字一律走结果页同一套排版函数: buildBigThree()(它内部就是 num0 + "没拿到写 --")
  //    给自研那三个(键名 own*, 与上面大数字的 gb8* 是两个不同的小节, 别拿错);
  //    GPU-SNL / NPU / 存储 用 num1 / num2 —— 与结果页那三行逐字同源
  //    (结果页写 num1(sn.score) / num2(npuRun.goPs) / num1(storage.seqWriteDurableMbps))。
  //  ★ 取数只取账本行里已有的键, 不新建数据源; 老行没有的三个键 RunLog 给 0 -> 这里写 '--'。
  //    单位(分 / GOPs/s / MiB/s)与"不计分"都不进卡片: 用户只要名字和分数,
  //    完整口径在结果页与报告文件里(卡片最后那一句小字给了"能不能比"的结论)。
  private histKeyRows(r: RunHistoryRow): KeyRow[] {
    //  ★ 2026-10(10.0)位置对调: 这三行原来是 CS1(那时 CS1 在下面、自研在上面),
    //    现在大数字是 CS1, 于是这三行让给自研套件 —— 取数换成账本行里的 own*(键名没动)。
    //    规格一个字没改: 还是 histKeyLine 的"白项目名 + 蓝分数"两 Text, 不许放大。
    const own: ScoreCard[] = buildBigThree(r.ownSingle, r.ownMulti, r.ownGpu);
    const out: KeyRow[] = [];
    out.push(this.histKeyOne('自研 单核', own[0].value));
    out.push(this.histKeyOne('自研 多核', own[1].value));
    out.push(this.histKeyOne('自研 GPU', own[2].value));
    out.push(this.histKeyOne('GPU-SNL', num1(r.snlScore)));
    out.push(this.histKeyOne('NPU 推理', num2(r.npuGops)));
    out.push(this.histKeyOne('存储 I/O', num1(r.storeWriteMbps)));
    return out;
  }

  // 清单里的一个格子: 只有名字与分数, 单位 / 条件一律留空(histKeyLine 也不画它们)。
  private histKeyOne(name: string, value: string): KeyRow {
    return { name: name, value: value, unit: '', cond: '', has: value !== '--' };
  }

  // 时间: 今天 / 昨天写人话, 再早的写日期; 到分钟。
  private histTimeText(ts: number): string {
    const d: Date = new Date(ts);
    const now: Date = new Date();
    const hm: string = this.two(d.getHours()) + ':' + this.two(d.getMinutes());
    const dayOf = (x: Date): string => x.getFullYear().toString() + '-' +
      this.two(x.getMonth() + 1) + '-' + this.two(x.getDate());
    if (dayOf(d) === dayOf(now)) {
      return '今天 ' + hm;
    }
    if (dayOf(d) === dayOf(new Date(now.getTime() - 86400000))) {
      return '昨天 ' + hm;
    }
    const sameYear: boolean = d.getFullYear() === now.getFullYear();
    return (sameYear ? '' : (d.getFullYear().toString() + '-')) +
      this.two(d.getMonth() + 1) + '-' + this.two(d.getDate()) + ' ' + hm;
  }

  private two(n: number): string {
    return n < 10 ? ('0' + n.toString()) : n.toString();
  }

  // 设备: 只要能认出是哪台机器。账本里存的是带日志尾巴的长串,
  // 这里只取到第一个分隔符之前、去掉品牌与型号括号(例: HUAWEI Mate 60 Pro（ALN-AL00） -> Mate 60 Pro)。
  private histDeviceText(dev: string): string {
    let s: string = dev;
    const cut: number = s.indexOf(' · ');
    if (cut > 0) {
      s = s.substring(0, cut);
    }
    const par: number = s.indexOf('（');
    if (par > 0) {
      s = s.substring(0, par);
    }
    if (s.startsWith('HUAWEI ')) {
      s = s.substring(7);
    } else if (s.startsWith('HONOR ')) {
      s = s.substring(6);
    }
    return s.length > 0 ? s : '本机';
  }

  //  ★ 这里原来有三个只在 App 内历史页用的排版函数, 已随卡片重做一起删除:
  //    histVerText()  —— 把版本号塞进卡片那一行(现在 App 内不显示版本, 版本仍写在
  //                      「下载/Crush分/历史成绩.txt」的版本列里, 那张表一个字都没动);
  //    histNum() / histScoreLine() —— 拼 "CS1  单核 x · 多核 y · GPU z" 那种一行三个数的
  //                      密集行。CS1 的三个分现在回到卡片上了, 但走的是**下半部分清单的三行**
  //                      (buildBigThree 取数 + histKeyLine 排版), 而不是回到那种小字密集行。
  //    静态检查: ref/layout_cases.test.mjs 的 Q 节会断言这三个名字不许再回来。

  // 备注: 只在有异常时非空。一切正常 -> 空串, 界面上什么都不显示(不写"正常")。
  private histNoteText(r: RunHistoryRow): string {
    const parts: string[] = [];
    if (r.interrupted) {
      parts.push('未跑完');
    }
    if (r.failed > 0) {
      parts.push(r.failed.toString() + ' 项没拿到分');
    }
    if (r.skipped > 0) {
      parts.push(r.skipped.toString() + ' 项跳过');
    }
    if (r.crashes > 0) {
      parts.push(r.crashes.toString() + ' 次崩溃');
    }
    return parts.join(' · ');
  }

  // 读一次账本(本地文件读, 不在任何计时区间里)。
  // 卡片上现在是"三个大数字 + 六行清单", 所以这一页收两种行:
  //   ① 8.4 起「一键全部跑分」写下的行 —— 三套分都在(自研 / CS1 / SNL · NPU · 存储),
  //      也是「历史成绩.txt」里带分的那几行;
  //   ② 只记了一部分的老行 —— 缺的那些格子写 "--", 其它照常显示; 不因为缺几个就把整张卡藏掉。
  // 一组都没记的行(单阶段行、8.4 之前的整轮行)不进这一页: 它们连一个分都拿不出来,
  // 硬塞进来就是一整列 "--" 的空卡 —— 那与"一眼看到分数"正好相反。
  // (这些行仍然完整保留在账本与「历史成绩.txt」里, 一条都不删。)
  private reloadHistory(): void {
    try {
      const all: RunHistoryRow[] = RunLog.get().historyRowsNewestFirst(HISTORY_PAGE_MAX);
      const out: RunHistoryRow[] = [];
      for (const r of all) {
        if (this.histRowShowable(r)) {
          out.push(r);
        }
      }
      this.histRows = out;
    } catch (e) {
      this.histRows = [];
    }
  }

  // 这一行要不要占一张卡: 至少记了一组(CS1 或 自研), **并且**九个分里至少有一个 > 0。
  //  后半条是"放宽之后不许冒出一堆空卡"的闸门: 记了键但所有分都是 0 的那一轮
  //  (整轮什么都没拿到), 画出来就是一张全是 "--" 的卡, 只会把真正有分的那几张挤出屏幕。
  //  缺一部分不算"空": 缺的格子按各自的口径写 -- (CS1 走 buildBigThree, SNL/NPU/存储 走
  //  num1/num2), 其它照常显示这张卡。
  private histRowShowable(r: RunHistoryRow): boolean {
    if (!r.hasGb8 && !r.hasOwn) {
      return false;
    }
    return r.gb8Single > 0 || r.gb8Multi > 0 || r.gb8Gpu > 0 ||
      r.ownSingle > 0 || r.ownMulti > 0 || r.ownGpu > 0 ||
      r.snlScore > 0 || r.npuGops > 0 || r.storeWriteMbps > 0;
  }

  // 点一条 -> 打开那一次的报告。
  // 报告文件名就是 report-<本轮开始时间>.txt(见 ReportSink.reportStampName),
  // 所以只要账本行里记了 startedAt 就能直接定位; 老行没有就退回 report-latest。
  private openHistoryReport(r: RunHistoryRow): void {
    const dir: string = FullReportStore.get().filesDir();
    let text: string = '';
    let path: string = '';
    const stamp: string = reportStampName(r.startedAt);
    const tryRead = (p: string): string => {
      if (dir.length === 0) {
        return '';
      }
      try {
        return readText(dir + '/' + p);
      } catch (e) {
        return '';
      }
    };
    const names: string[] = [stamp + '.txt', stamp + '.json', 'report-latest.txt'];
    for (const n of names) {
      const t: string = tryRead(n);
      if (t.length > 0) {
        text = t;
        path = dir + '/' + n;
        break;
      }
    }
    this.histRepTitle = this.histTimeText(r.startedAt) + ' · ' + this.histDeviceText(r.device);
    this.histRepText = text.length > FULL_REPORT_PREVIEW ? text.substring(0, FULL_REPORT_PREVIEW) : text;
    this.histRepNote = text.length > 0
      ? path
      : '这一份报告不在本机了。';
    // 二级界面先画数(这一行本身), 报告原文默认收起(见 historyReportOverlay 的说明)
    this.histRepRow = r;
    this.histRepOpen = false;
    this.histOpen = true;
  }

  @Builder
  historyEmpty() {
    Column() {
      Text('还没有跑分记录')
        .fontSize(this.fz(15))
        .fontColor(SUB)
      Text('回首页点一次「一键全部跑分」，跑完这里就会出现一行。')
        .fontSize(this.fz(12))
        .fontColor(FAINT)
        .margin({ top: Sp.x2 })
    }
    .width(this.cardW())
    .padding({ top: Sp.x5, bottom: Sp.x5 })
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: Sp.x5 })
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  historyListWide() {
    Flex({
      direction: FlexDirection.Row,
      wrap: FlexWrap.Wrap,
      justifyContent: FlexAlign.Center
    }) {
      ForEach(this.histRows, (r: RunHistoryRow, i: number) => {
        Column() {
          this.historyRowItem(r)
        }
        .flexGrow(1)
        .flexBasis('45%')
        .constraintSize({ minWidth: this.minCardW() })
        .margin({ top: this.rowGap() })
      }, (r: RunHistoryRow, i: number) => 'hist' + i.toString())
    }
    .width('100%')
  }

  // 窄窗: 一列。用 Column(不是 List) —— 外层已经是 Scroll, 再套一个可滚动容器会变成
  //  "滚动里套滚动", 卡片高度还会被 List 的虚拟化尺寸规则牵走; 这一页本来就只在按需渲染时
  //  展开最多 HISTORY_PAGE_MAX 条, Column 反而更可控。
  @Builder
  historyListNarrow() {
    Column() {
      ForEach(this.histRows, (r: RunHistoryRow, i: number) => {
        Column() {
          this.historyRowItem(r)
        }
        .width('100%')
        .margin({ top: this.rowGap() })
      }, (r: RunHistoryRow, i: number) => 'hist' + i.toString())
    }
    .width('100%')
  }

  // 口径: **一句话、极淡、放列表最后**(不放表头)。**也是二级界面那一句的唯一来源。**
  //  ★ 2026-10(10.0)位置对调之后, 这一句的**语义整个反了过来**, 必须跟着改对:
  //    · 上面三个大数字现在是 **CS1** -> 里面**只有单核**能跨设备比
  //      (CS1 多核在 8.2 之前参与项集合不同, 不要跨版本比);
  //    · 下面那几行现在是 **自研三项 + GPU-SNL / NPU / 存储** -> 只适合同一台机器前后对比,
  //      其中 GPU-SNL / NPU / 存储 native 侧 scored=false, 本来就不计分。
  //  说错这一句比换位置本身严重得多 —— 用户会照着它去比不该比的数。所以这一段是唯一来源:
  //  历史列表最后一行与历史二级界面里的那一句, 都调这个 @Builder(不许各写一份)。
  //  完整版(逐列 / 逐项条件)仍然写在「下载/Crush分/历史成绩.txt」与那一份完整报告里。
  @Builder
  historyCalibLine() {
    Text('上面三个数是 CS1，只有单核能跨设备比（多核 8.2 之前口径不同，不要跨版本比）；' +
      '下面的自研三项与 GPU-SNL / NPU / 存储只适合同一台机器前后对比，后三者不计分。')
      .fontSize(this.uiRes.keyUnit)
      .fontColor(FAINT)
      .lineHeight(Math.round(this.uiRes.keyUnit * 1.6))
      .width('100%')
      .textAlign(TextAlign.Center)
      .margin({ top: this.rowGap() })
  }

  @Builder
  historyTab() {
    Scroll() {
      Column() {
        Column() {
          if (this.histRows.length === 0) {
            this.historyEmpty()
          } else if (this.wide()) {
            this.historyListWide()
            this.historyCalibLine()
          } else {
            this.historyListNarrow()
            this.historyCalibLine()
          }
        }
        .width('100%')
        .constraintSize({ maxWidth: this.contentMax() })
        .alignItems(HorizontalAlign.Center)
      }
      .width('100%')
    }
    .width('100%')
    .layoutWeight(1)
    .scrollBar(BarState.Off)
    .edgeEffect(EdgeEffect.Spring)
    .fadingEdge(true)
    .transition(this.fxTransition())
  }

  // 历史页里点开的那一份记录 —— 二级界面。
  //  2026-10 用户原话:「二级界面太难看了, 密密麻麻的」「要让人读得懂」。
  //  改前: 这一屏就是把整份报告的纯文本原样倒进一个滚动框 —— 几十行连成一片, 没有层次,
  //        想找"这次跑了多少分"要自己在那堆文本里翻。
  //  改后: 三段, 默认只看第一段。
  //    ① 先给最直观的数: 自研三个大数字 + 六行分数。用的是**列表卡片/结果页同一套排版零件**
  //       (histBigRow / histKeyLine), 不另造一套字号与颜色 —— 所以这一页和上一页对得上;
  //    ② 一句话口径(上面三个数是 CS1、只有单核能跨设备比 / 下面是自研三项与不计分的三项);
  //    ③ 「展开完整报告」—— 原始文本一字不少地留着, 但要点一下才出现。
  //  没有动画(历史页里不许有 animateTo / animation, Q 节断言钉着)。
  @Builder
  historyReportOverlay() {
    Column() {
      Row() {
        Text(this.histRepTitle)
          .fontSize(this.fz(Ft.title))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text('关闭')
          .fontSize(this.fz(Ft.sub))
          .fontColor(ACCENT)
          .padding({ left: 10, right: 2, top: 4, bottom: 4 })
          .onClick(() => {
            this.histOpen = false;
          })
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .alignItems(VerticalAlign.Center)
      .margin({ top: 14 })
      // 报告不在本机时才说这一句; 在的话路径收进"完整报告"里, 一级不铺长文本
      if (this.histRepText.length === 0) {
        Text(this.histRepNote)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('92%')
          .constraintSize({ maxWidth: this.contentMax() })
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .margin({ top: 4 })
      }
      Scroll() {
        Column() {
          // ① 最直观的数
          if (this.histRepRow !== null) {
            Column() {
              this.histBigRow('自研', this.histBig(this.histRepRow as RunHistoryRow))
              Column() {
                ForEach(this.histKeyRows(this.histRepRow as RunHistoryRow), (row: KeyRow, i: number) => {
                  this.histKeyLine(row)
                }, (row: KeyRow, i: number) => 'hr' + i.toString())
              }
              .width('100%')
              .margin({ top: Math.round(this.uiRes.gap / 2) })
              // 异常才出现的一行(正常时这一行根本不存在)
              if (this.histNoteText(this.histRepRow as RunHistoryRow).length > 0) {
                Text(this.histNoteText(this.histRepRow as RunHistoryRow))
                  .fontSize(this.uiRes.keyUnit)
                  .fontColor(DANGER)
                  .width('100%')
                  .margin({ top: Math.round(this.uiRes.gap / 2) })
              }
            }
            .width('100%')
            .padding(this.cardPad())
            .backgroundColor(CARD)
            .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
          }
          // ② 一句话口径 —— 与历史列表最后那一句**同一个来源**(historyCalibLine),
          //    不许在这里再写一份(位置对调之后语义反了, 两份迟早说岔)。
          this.historyCalibLine()
          // ③ 原始报告: 默认收起
          Text(this.histRepOpen ? '收起完整报告' : '展开完整报告')
            .fontSize(this.fz(Ft.sub))
            .fontColor(ACCENT)
            .width('100%')
            .textAlign(TextAlign.Center)
            .padding({ top: 12, bottom: 12 })
            .margin({ top: 12 })
            .backgroundColor(CHIP)
            .borderRadius(Rd.md)
            .onClick(() => {
              this.histRepOpen = !this.histRepOpen;
            })
          if (this.histRepOpen) {
            Column() {
              Text('完整报告')
                .fontSize(this.fz(Ft.cap))
                .fontColor(FAINT)
                .width('100%')
              Text(this.histRepText.length > 0 ? this.histRepText : '（没有取到报告正文）')
                .fontSize(this.fz(Ft.cap))
                .fontColor(SUB)
                .width('100%')
                .textAlign(TextAlign.Start)
                .margin({ top: 6 })
              if (this.histRepNote.length > 0) {
                Text(this.histRepNote)
                  .fontSize(this.fz(Ft.cap))
                  .fontColor(FAINT)
                  .width('100%')
                  .margin({ top: 8 })
              }
            }
            .width('100%')
            .alignItems(HorizontalAlign.Start)
            .padding(this.cardPad())
            .backgroundColor(CARD)
            .borderRadius(this.cardRadius())
            .margin({ top: 10 })
          }
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .layoutWeight(1)
      .margin({ top: 10 })
      .scrollBar(BarState.Auto)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(BG)
    // App 自己的画布: 暗红 -> 深紫 -> 回落到 BG 的静态渐变(见 appBackdropGrad 的说明)。
    //  它画在 backgroundColor 之上、内容之下, 所以不占布局、不改任何宽高。
    .linearGradient(this.appBackdropGrad())
    .padding({ top: this.padTop(), bottom: this.padBottom() })
  }

  // ---- 关于 ----
  // =========================================================================
  //  「关于」页 —— 一级只有入口, 内容全在二级(2026-10 用户要求:
  //   「关于里面把东西全部放进二级界面, 整体上要非常的简洁」)。
  //   一级: 一张卡 + 7 行入口 + 底部版本行。**没有任何一段说明文字。**
  //   二级: 点进去, 左上角一个「‹ 返回」, 内容是重写过的短句 —— 不是把一级删掉的长文搬过去。
  //   原来的「界面视效」整张卡**已删除**(用户原话:"完全可以删掉"): 连它的 builder 一起删,
  //   卡里只用到 this.fxOn() 与几个颜色令牌, 那些在美化层里还有别的调用点, 删掉不影响任何东西。
  // =========================================================================
  @Builder
  aboutTab() {
    Scroll() {
      Column() {
        Column() {
          if (this.aboutPage === 0) {
            this.aboutIndex()
            // 排障工具默认隐藏: 对版本号连点 5 次展开(防误触 —— 里面有两个"一点就崩"的按钮)
            if (this.showDebugTools) {
              this.nativeSelfTestCard()
            }
            Text(APP_LABEL + ' · CS1 16 项 CPU + 11 项 GPU · Native ' + this.nativeVer)
              .fontSize(this.fz(11))
              .fontColor(FAINT)
              .margin({ top: 18, bottom: 24 })
              .onClick(() => {
                this.onVersionTap();
              })
            if (this.verHint.length > 0) {
              Text(this.verHint)
                .fontSize(this.fz(10))
                .fontColor(ACCENT)
                .margin({ bottom: 24 })
            }
          } else {
            this.aboutSubHeader(this.aboutTitle())
            this.aboutSubBody()
          }
        }
        .width('100%')
        .constraintSize({ maxWidth: this.contentMax() })
        .alignItems(HorizontalAlign.Center)
      }
      .width('100%')
    }
    .width('100%')
    .layoutWeight(1)
    .scrollBar(BarState.Off)
    .edgeEffect(EdgeEffect.Spring)
    // 官方边缘渐隐(fadingEdge): 内容在视口上下边缘自然淡出 —— 静态裁切效果, 不是动画,
    // 只在滚动时随内容重绘, 跑分期间(无人滚动)不产生任何开销。
    .fadingEdge(true)
    // 官方转场(一次性): 切换标签时淡入并轻微上移。用户操作触发, 180ms 结束;
    // 跑分期间 fxTransition() 返回 IDENTITY(无动画) —— 见该函数。
    .transition(this.fxTransition())
  }

  // 一级: 入口卡。一行一个入口, 行与行之间一条极淡的分隔线, 右边一个 ›。
  @Builder
  aboutIndex() {
    Column() {
      this.aboutEntry(1, '关于本应用', false)
      this.aboutEntry(2, '计分方法', false)
      this.aboutEntry(3, '跑分条件与温控', false)
      this.aboutEntry(4, '设备画像', false)
      this.aboutEntry(5, '运行日志', false)
      this.aboutEntry(6, '永久记录', false)
      this.aboutEntry(7, '隐私与数据', true)
    }
    .width(this.cardW())
    .padding({ left: this.cardPad(), right: this.cardPad() })
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 12 })
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  aboutEntry(id: number, title: string, last: boolean) {
    Row() {
      Text(title)
        .fontSize(this.fz(Ft.sub))
        .fontColor(TEXT)
        .layoutWeight(1)
      Text('›')
        .fontSize(this.fz(Ft.title))
        .fontColor(FAINT)
    }
    .width('100%')
    .padding({ top: 15, bottom: 15 })
    .border(this.aboutRowBorder(last))
    .onClick(() => {
      this.aboutPage = id;
    })
  }

  // 最后一行不要那条分隔线(否则卡片底边会多出一道线)
  private aboutRowBorder(last: boolean): BorderOptions {
    if (last) {
      const none: BorderOptions = { width: 0, color: C.transparent };
      return none;
    }
    return edgeBottom(DIVIDER);
  }

  private aboutTitle(): string {
    if (this.aboutPage === 1) { return '关于本应用'; }
    if (this.aboutPage === 2) { return '计分方法'; }
    if (this.aboutPage === 3) { return '跑分条件与温控'; }
    if (this.aboutPage === 4) { return '设备画像'; }
    if (this.aboutPage === 5) { return '运行日志'; }
    if (this.aboutPage === 6) { return '永久记录'; }
    return '隐私与数据';
  }

  // 二级页面统一的抬头: 左边「‹ 返回」, 右边这一页的名字。
  @Builder
  aboutSubHeader(title: string) {
    Row() {
      Text('‹ 返回')
        .fontSize(this.fz(Ft.sub))
        .fontColor(ACCENT)
        .padding({ right: 12, top: 4, bottom: 4 })
        .onClick(() => {
          this.aboutPage = 0;
        })
      Text(title)
        .fontSize(this.fz(Ft.title))
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .layoutWeight(1)
        .maxLines(1)
    }
    .width('92%')
    .constraintSize({ maxWidth: this.contentMax() })
    .alignItems(VerticalAlign.Center)
    .margin({ top: 12, bottom: 2 })
  }

  // 七个二级页面。文字一律改写短了 —— 一级删掉的长文不许原样搬过来。
  @Builder
  aboutSubBody() {
    if (this.aboutPage === 1) {
      this.aboutBlock('Crush分',
        '鸿蒙原生性能基准测试工具。CPU 七类负载 + GPU 四场景，全部 C++ 原生实现。')
      this.aboutBlock('跨平台可比性',
        '同一份 C++17 源码，不用平台专有 API。负载规模、数据与随机种子写死，换算常量不按机型调整。')
      this.aboutBlock('声明',
        '自研工具，与任何第三方商业跑分软件无关联。分数不能与它们换算，也不能直接比较。')
    } else if (this.aboutPage === 2) {
      this.aboutBlock('分数是绝对的',
        '单项分 = 绝对吞吐量 / 公示常量 × 1000。1000 分对应一个固定吞吐量，不是某台设备的耗时。性能翻倍，分数翻倍。')
      this.aboutBlock('公示常量',
        'CPU：图像 22.62 Mpx/s · 压缩 37.28 MB/s · 物理 0.35305 Gpair/s · 寻路 24 次/s · AI 5.0529 GOPS · 文本 188.74 MB/s · 内存 3.84 GB/s\n' +
        'GPU：几何 120 Mtri/s · 粒子 8 Gpx/s · 着色 60 GFLOPS · 计算 40 GOPS')
      this.aboutBlock('合成方式',
        '单核分 = 七项单线程分的几何平均。多核分 = 七项并行分的几何平均。GPU 分 = 各场景分的几何平均。\n' +
        '总分 = 单核^0.30 × 多核^0.45 × GPU^0.25。')
      this.aboutBlock('测试说明',
        '单核：七项负载依次单线程跑。多核：同一批负载按可用核并行拆分。')
      this.aboutBlock('CS1 的刻度',
        '刻度由本工程自定。在一台参考设备上把实测吞吐与目标分对齐一次，得到常量单核 7.70 / 多核 11.55，写死在代码里，不随机型调整。')
      this.aboutBlock('CS1 能不能和别的软件比',
        '不能。每个负载都是独立实现，刻度也是自定的，与任何第三方跑分软件不是同一把尺子。只有两台设备都跑本 App 的 CS1 时，分数才可比。')
      this.aboutBlock('设计依据',
        '多核合成只计入实测并行度达标的负载。稳定性 = 最差轮 / 最好轮 × 100，及格线 97%。GPU 关垂直同步、固定 1920×1080 离屏渲染。')
    } else if (this.aboutPage === 3) {
      this.aboutBlock('热管理',
        '由内核与固件执行，App 解不开。我们只做三件事：线程按核心频率绑到大核、跑分期间保持亮屏、把热状态与衰减率显示出来。')
      this.aboutBlock('怎么跑出更高的持续性能',
        '开性能模式。拔掉充电线。摘掉保护壳或加散热背夹。降低室温。跑之前让机器静置降温。')
    } else if (this.aboutPage === 4) {
      this.deviceProfileCard()
    } else if (this.aboutPage === 5) {
      this.runLogCard()
      this.logUploadCard()
    } else if (this.aboutPage === 6) {
      this.runHistoryCard()
    } else {
      this.aboutBlock('隐私',
        '默认不联网、不采集数据、没有账号。只有你填了地址并主动发送时，本机跑分日志才会 POST 到那个地址。')
      this.aboutBlock('日志在哪',
        '只写在本应用沙箱里。你可以随时查看、复制、导出或清空。')
    }
  }

  // ---- 运行日志卡(放在「关于」页最上面) ----
  @Builder
  runLogCard() {
    Column() {
      Row() {
        Text('运行日志（' + RUNLOG_FILE + '）')
          .fontSize(this.fz(14))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
        Blank()
        Text(this.logSizeText + ' · ' + this.logLines.toString() + ' 行')
          .fontSize(this.fz(11))
          .fontColor(ACCENT)
      }
      .width('100%')

      Text('诊断日志：每完成一项负载追加一行 JSON（字段：ts/runId/phase/item/index/total/ms/metric/unit/score/parallelism/ok/note）；每轮结束再写一条 run_summary 汇总；上次异常结束时下次启动补一条 crash。单文件上限 ' +
        Math.round(RUNLOG_MAX_BYTES / 1024).toString() +
        ' KB，超出自动轮转（runlog.1 ~ runlog.3，最旧的删除）——所以它是"近期诊断"，长期账本见下面那张卡。')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .lineHeight(18)
        .width('100%')
        .margin({ top: 8 })

      Text('时间范围：' + this.logRangeText + ' · 文件数 ' + this.logFiles.toString())
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 6 })
      Text(this.logStatusText())
        .fontSize(this.fz(11))
        .fontColor(this.logOk ? GOOD : WARN)
        .width('100%')
        .margin({ top: 4 })

      Text('最近日志预览（末尾 ' + RUNLOG_TAIL.toString() + ' 字符）')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 10 })
      Text(this.logTail)
        .fontSize(this.fz(10))
        .fontColor(SUB)
        .copyOption(CopyOptions.LocalDevice)
        .width('100%')
        .padding(8)
        .backgroundColor(INSET)
        .borderRadius(8)
        .margin({ top: 6 })
      Text(this.logCopyPreviewText())
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 6 })

      Row() {
        Button(this.logCopied ? '已复制' : '复制日志')
          .layoutWeight(1)
          .height(36)
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(GOOD)
          .onClick(() => {
            this.copyRunLog();
          })
        Button('导出日志文件')
          .layoutWeight(1)
          .height(36)
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(ACCENT)
          .margin({ left: 8 })
          .onClick(() => {
            this.exportFile('runlog');
          })
      }
      .width('100%')
      .margin({ top: 10 })

      Row() {
        Button('刷新')
          .layoutWeight(1)
          .height(36)
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(ACCENT)
          .onClick(() => {
            this.refreshLogStats();
            this.logStatus = '已刷新';
          })
        Button(this.logClearArm ? '确认清空' : '清空日志')
          .layoutWeight(1)
          .height(36)
          .fontSize(this.fz(12))
          .backgroundColor(this.logClearArm ? C.chipDanger : CHIP)
          .fontColor(DANGER)
          .margin({ left: 8 })
          .onClick(() => {
            if (this.logClearArm) {
              this.confirmLogClear();
            } else {
              this.armLogClear();
            }
          })
      }
      .width('100%')
      .margin({ top: 8 })

      if (this.logClearArm) {
        Row() {
          Text('二次确认：清空只删诊断日志（当前文件 + 3 个轮转文件），永久记录不受影响，不可恢复。')
            .fontSize(this.fz(10))
            .fontColor(WARN)
            .layoutWeight(1)
          Button('取消')
            .height(30)
            .fontSize(this.fz(11))
            .backgroundColor(CHIP)
            .fontColor(SUB)
            .onClick(() => {
              this.cancelLogClear();
            })
        }
        .width('100%')
        .margin({ top: 8 })
      }

      if (this.logStatus.length > 0) {
        Text(this.logStatus)
          .fontSize(this.fz(10))
          .fontColor(GOOD)
          .width('100%')
          .margin({ top: 8 })
      }
    }
    .width(this.cardW())
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 12 })
    .alignItems(HorizontalAlign.Start)
  }

  // ---- 关于页 · 布局自检 + 设备画像(给用户和我核对用; 可复制) ----

  // 「布局自检」里的一行: 标签 + 值(值可长按复制)
  @Builder
  selfCheckRow(label: string, value: string, color: string) {
    Row() {
      Text(label)
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .width(92)
      Text(value)
        .fontSize(this.fz(Ft.cap))
        .fontColor(color)
        .layoutWeight(1)
        .copyOption(CopyOptions.LocalDevice)
    }
    .width('100%')
    .margin({ top: Sp.x1 })
  }

  // 「布局自检」: 显示当前实际生效的那一套参数, 以及它是凭什么算出来的。
  //  用户会在手机 / 平板 / PC / 折叠各形态下各看一次并截图 —— 这一块就是给"一眼判断
  //  它算成了哪个形态、参数对不对"用的, 所以判定依据与降级原因都照实显示。
  //  纯静态文本(没有任何动画 / 定时器 / 模糊), 不占跑分期间的 CPU / GPU。
  @Builder
  layoutSelfCheckBlock() {
    Column() {
      Row() {
        Text('布局自检')
          .fontSize(this.fz(14))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
          .layoutWeight(1)
        Text(this.devSelfCheckCopied ? '已复制' : '可复制')
          .fontSize(this.fz(10))
          .fontColor(this.devSelfCheckCopied ? GOOD : DIM)
      }
      .width('100%')

      Text('这一块只回答三件事：实测窗口 vp 是多少、由它算出的全部布局参数、凭什么这么判。' +
        '判定只看实测窗口宽高(型号 / 芯片 / 折叠状态都不参与), 识别到什么就排成什么 —— ' +
        '手机 / 折叠 / 平板 / 电脑 / 分屏各看一次, 截图即可核对。')
        .fontSize(this.fz(10))
        .fontColor(SUB)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 6 })

      this.selfCheckRow('实测窗口 vp',
        this.winW.toString() + ' x ' + this.winH.toString() + ' vp', TEXT)
      this.selfCheckRow('宽高比', this.aspectText(), TEXT)
      this.selfCheckRow('纯几何事实', this.geoText(), SUB)
      this.selfCheckRow('分栏/结果区宽', this.zoneText(), TEXT)
      this.selfCheckRow('断点', this.bp + '（mediaquery 三档, 只作旁证）', SUB)
      this.selfCheckRow('命中形态', this.devShapeLine, ACCENT)
      this.selfCheckRow('主分栏', this.profileSplit === 1
        ? ('左操作 / 右结果 两栏（' + this.spanLeft.toString() + ':' +
          this.spanRight.toString() + '）')
        : '单栏（不分栏）', TEXT)
      this.selfCheckRow('结果卡列数', this.profileColumns.toString() + ' 列', TEXT)
      this.selfCheckRow('内容最大宽度', this.contentMaxText(), TEXT)
      this.selfCheckRow('结果卡基准宽', this.cardBasis() + '（最小 ' + this.minCardText() + '）', TEXT)
      this.selfCheckRow('开始按钮直径', this.startDia.toString() + ' vp', TEXT)
      this.selfCheckRow('按钮最小高度', this.uiBtnH.toString() + ' vp', TEXT)
      this.selfCheckRow('字号缩放', 'x' + this.uiFontScale.toFixed(2), TEXT)
      this.selfCheckRow('间距/内边距/圆角', this.rowGap().toString() + ' / ' +
        this.cardPad().toString() + ' / ' + this.cardRadius().toString() + ' vp', TEXT)
      this.selfCheckRow('竖向预算', this.devHeightBudget,
        this.tightY ? WARN : GOOD)
      this.selfCheckRow('判定依据', this.devShapeBasis, ACCENT)
      this.selfCheckRow('形态说明', this.devShapeNote, SUB)
      this.selfCheckRow('降级说明',
        this.devShapeDegrade.length > 0 ? this.devShapeDegrade : '无（所有判定接口都可用）',
        this.devShapeDegrade.length > 0 ? WARN : GOOD)
      // ---- 结果页密度(三处结果界面共用同一份; 见 LayoutPlan.resultDecoFor) ----
      this.selfCheckRow('结果页密度', this.uiRes.text, this.uiRes.fits ? GOOD : WARN)
      // ---- 结果页首屏分块账本(预测): 逐块列出, 不只给一个总数 ----
      //  为什么非得分块: 上一轮这里只有一个总数(firstScreenVp), 自检页显示"833/896 通过",
      //  真机渲染却是三张大数字卡拉满首屏 —— 总数对不上时看不出是哪一块算错,
      //  也看不出"多余的高度被谁吃掉了"。现在一块一行: 块名 / 在哪一栏 / 预测 vp /
      //  固定高度还是拉伸块 / 这一块凭什么这么高(每一项都写出来)。
      ForEach(this.uiRes.blocks, (b: ResultBlock) => {
        this.selfCheckRow('分块·' + b.label,
          b.vp.toString() + ' vp（' + b.col + '）· ' +
          (b.fixed ? '固定高度' : ('拉伸块 grow=' + b.grow.toString())) + ' —— ' + b.detail,
          b.fixed ? TEXT : ACCENT)
      }, (b: ResultBlock) => 'blk' + b.key)
      //  界面这一层的实际取值(自检页显示的就是界面写进组件的那些数, 不是另算一份)
      this.selfCheckRow('大数字卡(界面实际值)',
        '一行 ' + this.uiRes.l1Cards.toString() + ' 张 x ' + this.uiRes.l1Lines.toString() +
        ' 行 · 每张 .height(' + this.uiRes.l1CardH.toString() + 'vp)（固定, 不拉伸）· 基准宽 ' +
        this.uiRes.l1Basis + ' · 最小宽 ' + this.uiRes.l1MinW.toString() + 'vp · 条件行 ' +
        this.uiRes.l1CondLines.toString() + ' 行 · 整块 ' + this.uiRes.l1BlockH.toString() + 'vp', ACCENT)
      //  不变量: 三个大数字块的总高 <= 可用高度的 30%(离线用例 N 节对 6 个典型窗口逐条断言)
      this.selfCheckRow('不变量·大数字块 <= 30% 可用高度',
        this.uiRes.l1BlockH.toString() + ' vp <= ' + this.uiRes.availVp.toString() +
        ' vp x 30% = ' + Math.floor(this.uiRes.availVp * 0.3).toString() + ' vp（实际占 ' +
        (this.uiRes.l1Share * 100).toFixed(1) + '%）→ ' +
        (this.uiRes.l1InvariantOk ? '满足' : '不满足'),
        this.uiRes.l1InvariantOk ? GOOD : DANGER)
      //  多余高度给了谁(当前设计: 只有结果列表 / 滚动区会吃剩余高度)
      this.selfCheckRow('多余高度去向',
        (this.uiRes.slackVp > 0
          ? (this.uiRes.slackVp.toString() + ' vp 全部给结果列表 / 滚动区（三个大数字卡一点都不要）')
          : ('没有多余高度：首屏比可用高度多 ' + Math.abs(this.uiRes.slackVp).toString() +
            ' vp，列表要滚动才能看全（内容一个都不隐藏）')), TEXT)
      // ---- 旋转策略(回答"系统开了旋转锁定, 为什么这个 App 里还能转") ----
      //  下面四行全部是回读来的, 不是把 module.json5 抄一遍:
      //  声明值来自 bundleManager(与系统实际生效的值同源), 运行时首选方向来自窗口对象,
      //  屏幕方向来自 display, 方向变化次数来自实测窗口。四行互相印证, 谁都能核对。
      this.selfCheckRow('旋转策略',
        this.rotDeclared.length > 0 ? this.rotDeclared : '（还没回读到）',
        this.rotRespect === 1 ? GOOD : (this.rotRespect === 0 ? DANGER : WARN))
      this.selfCheckRow('旋转·声明值', this.rotDeclaredRaw.length > 0
        ? (this.rotDeclaredRaw + '（module.json5 写的是 ' + ROT_DECLARED_STRING + '）') : '（未取到）', SUB)
      this.selfCheckRow('旋转·运行时', (this.rotEffective.length > 0 ? this.rotEffective : '（未取到）') +
        ' · 当前屏幕方向 ' + (this.rotScreen.length > 0 ? this.rotScreen : '（未取到）'), SUB)
      this.selfCheckRow('旋转·实测方向变化', this.rotFlips.toString() + ' 次（本次启动以来）—— ' +
        '系统旋转锁定开着时，把机器转 90 度这个数应当保持不动；' +
        '锁定的开关状态没有公开 API，应用读不到，所以这里给的是可自证的实测计数', SUB)
      this.selfCheckRow('旋转·读不到的部分', '系统「旋转锁定」开关的实时状态：本机 SDK（API 18）公开 ' +
        'd.ts 里没有任何 rotationLock / autoRotation 接口，读不到就是读不到，不编。' +
        '应用能保证的是：声明的策略是 auto_rotation_restricted（跟随传感器但受锁定约束）。' +
        (this.rotNote.length > 0 ? (' 备注：' + this.rotNote) : ''), FAINT)

      Text(this.devSelfCheck)
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .lineHeight(15)
        .copyOption(CopyOptions.LocalDevice)
        .width('100%')
        .padding(Sp.x2)
        .backgroundColor(INSET)
        .borderRadius(Rd.sm)
        .margin({ top: Sp.x2 })

      Button(this.devSelfCheckCopied ? '已复制布局自检' : '复制布局自检（连同设备与窗口信息）')
        .width('100%')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(12))
        .backgroundColor(CHIP)
        .fontColor(GOOD)
        .border(outline(ACCENT_SOFT))
        .margin({ top: 8 })
        .onClick(() => {
          this.copySelfCheck();
        })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD_HI)
    .border(edgeTop(C.cardEdgeHi))
    .borderRadius(this.cardRadius())
    .margin({ top: Sp.x3 })
    .alignItems(HorizontalAlign.Start)
  }

  // 「设备画像」卡里的分区标题: 只加一条 1vp 分隔线 + 一行小字, 纯静态, 零开销。
  //  为什么加: 这一卡现在装着布局自检 / 身份识别 / 芯片三段 / 内核绑核 / deviceInfo 逐字段 ——
  //  信息量很大, 分区只是把它读得下去, 没有折叠、没有隐藏任何一行。
  @Builder
  profileSection(title: string) {
    Column() {
      Divider()
        .strokeWidth(HAIRLINE)
        .color(DIVIDER)
        .margin({ top: Sp.x3, bottom: Sp.x2 })
      Text(title)
        .fontSize(this.fz(Ft.cap))
        .fontWeight(FontWeight.Bold)
        .fontColor(DIM)
        .width('100%')
    }
    .width('100%')
  }

  @Builder
  deviceProfileCard() {
    Column() {
      Row() {
        Text('设备画像')
          .fontSize(this.fz(14))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
          .layoutWeight(1)
        Text(this.planId + ' · ' + this.profileColumns.toString() + ' 列')
          .fontSize(this.fz(10))
          .fontColor(ACCENT)
          .backgroundColor(CHIP)
          .border(outline(ACCENT_SOFT))
          .borderRadius(6)
          .padding({ left: 6, right: 6, top: 2, bottom: 2 })
          .margin({ right: 6 })
        if (this.devFoldTag.length > 0) {
          Text('折叠屏 · ' + this.devFoldTag)
            .fontSize(this.fz(10))
            .fontColor(ACCENT2)
            .backgroundColor(C.chipViolet)
            .borderRadius(6)
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
        }
      }
      .width('100%')

      // ---- 布局自检: 任何设备上都先看这一块(截图核对用) ----
      this.layoutSelfCheckBlock()

      Text('传播名会用在首页设备卡与运行日志里（日志记的是"哪台机器跑的"，便于几年后对照）。' +
        '下面的参数决定界面怎么排：列数 / 内容最大宽度 / 开始按钮与按钮高度。' +
        '设备画像只影响观感，不参与任何测试项的计算、顺序与计分。')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .lineHeight(18)
        .width('100%')
        .margin({ top: 8 })

      this.profileSection('身份与识别')

      // 匹配方式: 实际命中的是 A 系统别名 / B 市场名 / C1 精确表 / C2 唯一前缀 / D 型号码回退
      Row() {
        Text('匹配方式')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.devMatchMethod.length > 0 ? this.devMatchMethod : '（未取到）')
          .fontSize(this.fz(10))
          .fontColor(ACCENT)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 8 })

      // 内置表屏幕参数(内屏/外屏英寸 · 分辨率 · 来源域名); 表里没有就明说, 不瞎填
      Row() {
        Text('表内屏幕')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.devTableScreen.length > 0 ? this.devTableScreen : '（未命中内置表）')
          .fontSize(this.fz(10))
          .fontColor(this.devTableHit ? TEXT : SUB)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      // CPU 拓扑(逻辑核 / 物理核 / 是否检测到 SMT / 来源): 显示, 读不到就明说"未知, 按 1:1 处理"
      Row() {
        Text('CPU 拓扑')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.benchTopologyLine())
          .fontSize(this.fz(10))
          .fontColor(this.smtNote.length > 0 ? WARN : TEXT)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      this.profileSection('CPU 与芯片（本机实测 → 逐核原始读数 → 第三方推断，三段分开）')

      // 芯片与频率(本机实测): 设备自己说的话 —— 逻辑核 / 物理核 / 逐核 cpuinfo_max_freq 档位
      Row() {
        Text('芯片/频率')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(DeviceProfile.get().chipMeasuredLine())
          .fontSize(this.fz(10))
          .fontColor(TEXT)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      this.profileSection('内核允许的核集合与绑核')

      // 内核允许的核集合(2026-10-05): "可用核集合 vs 快簇 vs 交集"一行结论 
      // 用户会在手机和平板上各看一次这一行 —— 它直接回答"为什么这台快/慢、为什么代际差异没体现"。
      // 颜色规则: 交集为空(绑核目标不在可用核集合内)= 警告色; 其余情况正常色。
      Row() {
        Text('允许核集合')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.devAffinityLine.length > 0 ? this.devAffinityLine : '（尚未读取：native 上报后显示）')
          .fontSize(this.fz(10))
          .fontColor(this.affinityRestricted() ? WARN : TEXT)
          .copyOption(CopyOptions.LocalDevice)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      // 快簇是怎么算出来的 + 逐路径实测事实(Cpus_allowed / sched_getaffinity / cgroup / cpuset / Mems)
      Row() {
        Text('快簇/来源')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.devAffinityFull.length > 0 ? this.devAffinityFull : '（尚未读取）')
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .copyOption(CopyOptions.LocalDevice)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      Row() {
        Text('内核限制')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.devAllowedDetail.length > 0 ? this.devAllowedDetail : '（尚未读取）')
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .copyOption(CopyOptions.LocalDevice)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      if (this.devThreadCapNote.length > 0) {
        Row() {
          Text('线程上限')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .width(64)
          Text(this.devThreadCapNote)
            .fontSize(this.fz(10))
            .fontColor(WARN)
            .layoutWeight(1)
        }
        .width('100%')
        .margin({ top: 4 })
      }

      // 逐核原始读数(读失败的核也在这里, 带 errno): 一眼看出"是漏了核还是真的只有这几个档"
      Row() {
        Text('逐核原始值')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(DeviceProfile.get().chipPerCoreLine())
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .copyOption(CopyOptions.LocalDevice)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      // 芯片判读(第三方资料推断, 非官方): 与上面的"本机实测"刻意分成两行 ——
      // 资料里 26 条麒麟 SoC 的 confidence: official = 0 条, 推断不能冒充官方结论。
      Row() {
        Text('芯片判读')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(DeviceProfile.get().chipInferLine())
          .fontSize(this.fz(10))
          .fontColor(ACCENT2)
          .copyOption(CopyOptions.LocalDevice)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      // hardwareModel: 鸿蒙上常常就是 SoC/硬件型号 —— 有值就是这台机器的权威答案(不用外部资料)
      Row() {
        Text('hardwareModel')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(84)
        Text(DeviceProfile.get().hardwareModelLine())
          .fontSize(this.fz(10))
          .fontColor(TEXT)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      this.profileSection('deviceInfo 逐字段（读到什么写什么）')

      // deviceInfo 逐字段(读到什么写什么, 取不到写"取不到：原因"): 可长按复制
      Text(DeviceProfile.get().hwFieldLines())
        .fontSize(this.fz(10))
        .fontColor(SUB)
        .lineHeight(15)
        .copyOption(CopyOptions.LocalDevice)
        .width('100%')
        .padding(8)
        .backgroundColor(INSET)
        .borderRadius(8)
        .margin({ top: 4 })

      // 当前绑核状态: 最近一次跑分的 native cpuInfo 文本(措辞由 native 给: 大核簇/掩码/落点)
      // 没跑过就写明"还没跑分"; 这里只做透传, 不自己拼判据(免得与 native 的口径不一致)
      Row() {
        Text('绑核状态')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.devCpuInfo.length > 0 ? this.devCpuInfo : '（还没跑分：跑完 CS1 单核 / 多核后这里会显示"跑在哪个核 / 哪个簇"）')
          .fontSize(this.fz(10))
          .fontColor(this.devCpuInfo.length > 0 ? TEXT : SUB)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      // ---- SMT 开关说明: 与首页设备卡里那一行同一份文本 ----
      //  为什么在这里再显示一次: 首页设备卡在"窗口高度不足"时会把这几行收起来(见 tightY),
      //  这里必须原样留住, 否则就变成"内容丢了"。两处共用 smtHint()/smtNote, 不会各说各话。
      this.profileSection('SMT 超线程开关')

      Row() {
        Text('开关说明')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.smtHint())
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .lineHeight(15)
          .copyOption(CopyOptions.LocalDevice)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      if (this.smtNote.length > 0) {
        Row() {
          Text('开关备注')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .width(64)
          Text(this.smtNote)
            .fontSize(this.fz(10))
            .fontColor(WARN)
            .lineHeight(15)
            .copyOption(CopyOptions.LocalDevice)
            .layoutWeight(1)
        }
        .width('100%')
        .margin({ top: 4 })
      }

      Row() {
        Text('拓扑旁路')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(DeviceProfile.get().cpuTopologyLine())
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .lineHeight(15)
          .copyOption(CopyOptions.LocalDevice)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      this.profileSection('机型对照表来源')

      Row() {
        Text('表内来源')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width(64)
        Text(this.devTableSource)
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .layoutWeight(1)
      }
      .width('100%')
      .margin({ top: 4 })

      Text(this.devProfileText)
        .fontSize(this.fz(10))
        .fontColor(SUB)
        .copyOption(CopyOptions.LocalDevice)
        .width('100%')
        .padding(8)
        .backgroundColor(INSET)
        .borderRadius(8)
        .margin({ top: 8 })

      Row() {
        Button(this.devProfileCopied ? '已复制' : '复制设备画像')
          .layoutWeight(1)
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(GOOD)
          .onClick(() => {
            this.copyDeviceProfile();
          })
        Button('重新识别')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(ACCENT)
          .margin({ left: 8 })
          .onClick(() => {
            this.reProfile();
          })
      }
      .width('100%')
      .margin({ top: 8 })

      Text('「芯片判读」用的是 ref/kirin_socs.json 的 26 条麒麟对照表 + ref/_build_kirin.py 的判读算法（已在 App 内按同一份指纹表重算）。' +
        ' 该表里 confidence: official = 0 条 —— 华为从未公布过任何麒麟 SoC 的 CPU 核心布局，所以判读一律写"疑似/推断"，' +
        '并与"本机实测"分成两行；数据不足或两个候选分不开时会直接写明，不猜。' +
        '「逐核原始值」是 cpu0..cpuN-1 的 cpufreq/cpuinfo_max_freq 原文（读失败的核带 errno）。')
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 8 })

      Text('「重新识别」会重读设备信息、折叠状态与屏幕参数（例如换姿势、插上键盘后想确认一下当前采用的布局）。' +
        '内置机型对照表共 ' + DEVICE_TABLE_COUNT.toString() + ' 条（由 ref/device_models.json 生成），只做"型号码 → 传播名"的兜底；' +
        '系统给了传播名就一直用系统的；前缀有冲突的型号码（本表 19 组）一律只显示原文，' +
        '三星带颜色/容量后缀的型号码会先去后缀再精确匹配（不猜机型）。')
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 6 })
    }
    .width(this.cardW())
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 12 })
    .alignItems(HorizontalAlign.Start)
  }

  // 「布局自检」块用到的三个小文本(纯计算, 无状态)
  // 「布局自检」: 纯几何事实一行 —— 布局的每一个数都是从这四个数里长出来的,
  //  所以先把它们摆出来, 再谈参数。数错了立刻能看出来(不是"看起来像哪个形态")。
  private geoText(): string {
    if (this.geoW <= 0 || this.geoH <= 0) {
      return '未取到窗口尺寸 —— 布局按兜底 ' + this.geoW.toString() + ' x ' +
        this.geoH.toString() + 'vp 计算（不是实测值，尺寸到达后立即重算）';
    }
    return '短边 ' + this.geoShort.toString() + 'vp · 长边 ' + this.geoLong.toString() +
      'vp · 内容列 ' + this.geoContent.toString() + 'vp';
  }

  // 「布局自检」: 分栏与结果区宽度 + 列数上限 —— 列数就是拿"结果区宽度"算出来的
  private zoneText(): string {
    if (this.profileSplit !== 1) {
      return '单栏 · 结果区 ' + this.geoZone.toString() + 'vp（= 内容列）· 列数上限 ' +
        this.geoCap.toString();
    }
    return '左 ' + this.geoLeft.toString() + 'vp / 右 ' + this.geoZone.toString() +
      'vp（分栏 ' + this.spanLeft.toString() + ':' + this.spanRight.toString() +
      '）· 列数上限 ' + this.geoCap.toString();
  }

  private aspectText(): string {
    if (this.winW <= 0 || this.winH <= 0) {
      return '（窗口尺寸未取到）';
    }
    // 标注用的是 LayoutPlan 里那个纯函数(离线用例逐条断言过), 与复制的自检文本、
    // 与「判定依据」里那一句同一份来源 —— 三处不会各说一套比例。
    // 例: 664x940vp -> "0.706（≈1:√2）" —— 一眼就能看出它是竖长的 1:sqrt(2), 不是 16:10。
    return (this.winW / this.winH).toFixed(3) + aspectHint(this.winW, this.winH);
  }

  private contentMaxText(): string {
    const v: number = DeviceProfile.get().contentMaxVp();
    return v > 0 ? (v.toString() + ' vp（居中限宽）') : '不限（满宽）';
  }

  private minCardText(): string {
    const v: number = this.minCardW();
    return v > 0 ? (v.toString() + ' vp') : '不设下限';
  }

  // 可复制的「旋转策略」一段(与布局自检一起复制出去; 纯字符串拼接, 无副作用)
  private rotSelfCheckText(): string {
    let s: string = '[Crush分 · 旋转策略]\n';
    s += '声明值（bundleManager 回读，与系统实际生效值同源）：' +
      (this.rotDeclaredRaw.length > 0 ? this.rotDeclaredRaw : '（未取到）') +
      '；module.json5 写的是 ' + ROT_DECLARED_STRING + '\n';
    s += '策略含义：' + (this.rotDeclared.length > 0 ? this.rotDeclared : '（未取到）') + '\n';
    s += '是否尊重系统旋转锁定：' + (this.rotRespect === 1 ? '是'
      : (this.rotRespect === 0 ? '否（会无视系统锁定）' : '未知（没读到声明值）')) + '\n';
    s += '运行时首选方向：' + (this.rotEffective.length > 0 ? this.rotEffective : '（未取到）') + '\n';
    s += '当前屏幕方向：' + (this.rotScreen.length > 0 ? this.rotScreen : '（未取到）') +
      '；本次启动以来实测方向变化 ' + this.rotFlips.toString() + ' 次\n';
    s += '说明：系统「旋转锁定」开关本身的实时状态没有公开 API（本机 SDK 里搜不到 ' +
      'rotationLock / autoRotation），应用读不到，写在这里，不编。\n';
    if (this.rotNote.length > 0) {
      s += '备注：' + this.rotNote + '\n';
    }
    return s;
  }

  private async copySelfCheck(): Promise<void> {
    try {
      const text: string = (this.devSelfCheck.length > 0
        ? this.devSelfCheck : DeviceProfile.get().selfCheck()) + this.rotSelfCheckText();
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.devSelfCheckCopied = true;
    } catch (e) {
      this.devSelfCheckCopied = false;
      console.warn('[AB] layout self-check copy failed');
    }
  }

  private async copyDeviceProfile(): Promise<void> {
    try {
      const paste = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.devProfileText);
      await pasteboard.getSystemPasteboard().setData(paste);
      this.devProfileCopied = true;
    } catch (e) {
      this.devProfileCopied = false;
      console.warn('[AB] device profile copy failed');
    }
  }

  private reProfile(): void {
    DeviceProfile.get().refreshAll();
    // 「重新识别」也把芯片判读重读一遍(逐核频率与 MIDR 都是设备自报的静态事实, 重读只为对齐)
    this.refreshChipIdentity();
    this.syncProfile();
    this.devProfileCopied = false;
  }

  // ---- 永久账本卡 runhistory.jsonl(每完成一轮一行; 5 MB 才轮转) ----
  @Builder
  runHistoryCard() {
    Column() {
      Row() {
        Text('永久记录（' + HISTORY_FILE + '）')
          .fontSize(this.fz(14))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
          .layoutWeight(1)
        Text('永久')
          .fontSize(this.fz(10))
          .fontColor(GOOD)
          .backgroundColor(C.chipGood)
          .borderRadius(6)
          .padding({ left: 6, right: 6, top: 2, bottom: 2 })
      }
      .width('100%')

      Text('和上面的诊断日志分开算：这里「每完成一轮」才追加一行，字段精简（ts/kind/phase/composite/scored/failed/skipped/crashes/durationMs/device/appVer/nativeVer）。' +
        '它不参与日常轮转，只有涨到 ' + Math.round(HISTORY_MAX_BYTES / 1024 / 1024).toString() +
        ' MB 才把最旧的挪成 runhistory.1 ~ .' + HISTORY_KEEP.toString() +
        '（共保留 ' + (HISTORY_KEEP + 1).toString() + ' 份），所以几年后还能查到你什么时候跑过多少分。' +
        '取证与日志都只是旁路记录，不参与任何测试项的计算、顺序与计分。')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .lineHeight(18)
        .width('100%')
        .margin({ top: 8 })

      Text('条数 ' + this.histLines.toString() + ' · 大小 ' + this.histSizeText +
        ' · 文件数 ' + this.histFiles.toString())
        .fontSize(this.fz(12))
        .fontColor(ACCENT)
        .width('100%')
        .margin({ top: 10 })
      Text('时间跨度：' + this.histRangeText)
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 4 })

      Text('最近 3 条')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 10 })
      Text(this.histPreview.length > 0 ? this.histPreview : '（还没有记录：跑完任意一轮就会写一行）')
        .fontSize(this.fz(10))
        .fontColor(SUB)
        .copyOption(CopyOptions.LocalDevice)
        .width('100%')
        .padding(8)
        .backgroundColor(INSET)
        .borderRadius(8)
        .margin({ top: 6 })

      Row() {
        Button('导出永久记录')
          .layoutWeight(1)
          .height(36)
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(GOOD)
          .onClick(() => {
            this.exportFile('history');
          })
        Button('刷新')
          .layoutWeight(1)
          .height(36)
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(ACCENT)
          .margin({ left: 8 })
          .onClick(() => {
            this.refreshHistoryStats();
            this.histStatus = '已刷新';
          })
      }
      .width('100%')
      .margin({ top: 10 })

      Text('导出 = 用系统文件选择器存成 .jsonl（开头几行 # 是注释，其余每行一个 JSON；可用「发送范围 = 最近 N 条永久记录」时把同一批数据发到群里）。' +
        '「清空日志」只清诊断日志，这份账本不受影响。')
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 6 })

      if (this.histStatus.length > 0) {
        Text(this.histStatus)
          .fontSize(this.fz(10))
          .fontColor(GOOD)
          .width('100%')
          .margin({ top: 8 })
      }
    }
    .width(this.cardW())
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 12 })
    .alignItems(HorizontalAlign.Start)
  }

  // ---- 可选联网上传卡(默认关闭) ----
  @Builder
  logUploadCard() {
    Column() {
      Text('日志自动上传（可选 · 默认关闭）')
        .fontSize(this.fz(14))
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .width('100%')

      Text('把日志"无线发送"到你自己的地址：一次通用 JSON POST，由你自己的服务端接收。' +
        '留空则完全不上传任何数据。不接任何第三方分析 SDK：只用系统自带的 @ohos.net.http，Content-Type: application/json，' +
        '连接 / 读取超时各 10 秒；发送失败只往日志里记一行 note 并在本页留一行结果，不阻塞或影响跑分。')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .lineHeight(18)
        .width('100%')
        .margin({ top: 8 })

      Row() {
        Toggle({ type: ToggleType.Switch, isOn: this.uploadOn })
          .onChange((on: boolean) => {
            this.onUploadSwitch(on);
          })
        Text(this.uploadOn ? '已开启' : '已关闭（默认）')
          .fontSize(this.fz(12))
          .fontColor(this.uploadOn ? GOOD : SUB)
          .margin({ left: 8 })
      }
      .width('100%')
      .margin({ top: 10 })
      // 无障碍: 整行作为一个可朗读单元
      .accessibilityGroup(true)
      .accessibilityText('日志自动上传开关，当前' + (this.uploadOn ? '已开启' : '已关闭'))

      TextInput({ text: this.uploadUrl, placeholder: 'https://example.com/aurora-log（留空 = 不上传）' })
        .height(40)
        .fontSize(this.fz(12))
        .backgroundColor(INSET)
        .fontColor(TEXT)
        .margin({ top: 8 })
        .onChange((v: string) => {
          this.onUploadUrlChange(v);
        })

      // ---- 发送范围 ----
      Text('发送范围')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 10 })
      Row() {
        ForEach(UPLOAD_RANGE_CHOICES, (c: UploadChoice) => {
          Button(c.label)
            .layoutWeight(1)
            .height(30)
            .fontSize(this.fz(10))
            .backgroundColor(this.uploadRange === c.id ? ACCENT2 : CHIP)
            .fontColor(this.uploadRange === c.id ? ON_ACCENT : SUB)
            .margin({ left: 2, right: 2 })
            .onClick(() => {
              this.onUploadRange(c.id);
            })
        }, (c: UploadChoice) => 'rng' + c.id)
      }
      .width('100%')
      .margin({ top: 6 })
      if (this.uploadRange === UPLOAD_RANGE_HISTORY_N) {
        Row() {
          Text('N =')
            .fontSize(this.fz(11))
            .fontColor(SUB)
          TextInput({ text: this.uploadHistN.toString(), placeholder: '20' })
            .type(InputType.Number)
            .height(34)
            .fontSize(this.fz(12))
            .layoutWeight(1)
            .backgroundColor(INSET)
            .fontColor(TEXT)
            .margin({ left: 8 })
            .onChange((v: string) => {
              this.onUploadHistN(v);
            })
          Text('条（1 ~ 500）')
            .fontSize(this.fz(10))
            .fontColor(FAINT)
            .margin({ left: 8 })
        }
        .width('100%')
        .margin({ top: 6 })
      }

      Row() {
        Button(this.uploadBusy ? '发送中…' : '立即发送一次')
          .layoutWeight(1)
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(ACCENT)
          .enabled(!this.uploadBusy)
          .onClick(() => {
            this.uploadLogNow();
          })
        Button(this.uploadIfaceOpen ? '收起接口说明' : '接口说明')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(ACCENT2)
          .margin({ left: 8 })
          .onClick(() => {
            this.uploadIfaceOpen = !this.uploadIfaceOpen;
          })
      }
      .width('100%')
      .margin({ top: 10 })

      Text('目标主机：' + LogUpload.get().targetLabel())
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 6 })

      if (this.uploadIfaceOpen) {
        Text(this.uploadInterfaceText())
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .lineHeight(17)
          .copyOption(CopyOptions.LocalDevice)
          .width('100%')
          .padding(8)
          .backgroundColor(INSET)
          .borderRadius(8)
          .margin({ top: 8 })
      }

      Text(this.uploadNote.length > 0 ? this.uploadNote
        : ('当前状态：' + (LogUpload.get().active() ? '已启用自动发送：每轮跑分结束发一次'
          : (LogUpload.get().url().length > 0
            ? '自动发送已关闭（点「立即发送一次」可手动发；不会自动联网）'
            : '未启用 —— 不会产生任何网络请求'))))
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 8 })

      if (this.uploadLastResult.length > 0) {
        Text('最近一次发送：' + this.uploadLastResult)
          .fontSize(this.fz(10))
          .fontColor(LogUpload.get().lastResultOk() || LogUpload.get().lastResultAt() === 0 ? GOOD : WARN)
          .width('100%')
          .margin({ top: 4 })
      }
    }
    .width(this.cardW())
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 12 })
    .alignItems(HorizontalAlign.Start)
  }


  // ---- 「关于」页 · 崩溃取证实测卡(排障用; 按钮 2/3 会故意让 App 崩溃一次) ----
  @Builder
  nativeSelfTestCard() {
    Column() {
      Row() {
        Text('崩溃取证实测（会故意让 App 崩溃一次）')
          .fontSize(this.fz(14))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
          .layoutWeight(1)
        Text('排障用')
          .fontSize(this.fz(10))
          .fontColor(DANGER)
          .backgroundColor(C.chipDanger)
          .borderRadius(6)
          .padding({ left: 6, right: 6, top: 2, bottom: 2 })
      }
      .width('100%')

      Text('用来验证 native「崩溃取证装置」这条链路是不是通的。按钮 ① 安全：只往证据文件写一条自检记录，不崩溃，' +
        '写完立刻用 takeNativeCrashReport() 读回来显示；按钮 ② ③ 会弹出二次确认，确认后本进程立刻退出（这是预期行为），' +
        '重启 App 后首页的崩溃横幅会显示取证装置写下的现场（信号 / 地址 / pc 是否落在我们自己的 .so 里 / 当时标记的负载项 / 内存快照）。' +
        '取证全程只记录，不参与任何测试项的计算、顺序与计分。')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .lineHeight(18)
        .width('100%')
        .margin({ top: 8 })

      Text('取证装置状态：' + this.nGuardStatus)
        .fontSize(this.fz(11))
        .fontColor(this.nGuardReady ? GOOD : WARN)
        .width('100%')
        .margin({ top: 10 })
      Text('证据文件：' + (this.nGuardPath.length > 0 ? this.nGuardPath : '（未设置：setLogDir 没有成功）'))
        .fontSize(this.fz(10))
        .fontColor(SUB)
        .copyOption(CopyOptions.LocalDevice)
        .width('100%')
        .margin({ top: 4 })
      Text('nativeCrashGuardReady = ' + (this.nGuardReady ? 'true（信号处理器已武装）' : 'false（未就绪）'))
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 4 })
      if (this.nGuardNote.length > 0) {
        Text(this.nGuardNote)
          .fontSize(this.fz(10))
          .fontColor(WARN)
          .width('100%')
          .margin({ top: 6 })
      }

      Button('① 写入自检记录（不崩溃）')
        .width('100%')
        .height(36)
        .fontSize(this.fz(12))
        .backgroundColor(CHIP)
        .fontColor(GOOD)
        .margin({ top: 12 })
        .onClick(() => {
          this.runSelfTestWrite();
        })

      Button('② 触发一次真崩溃（SIGSEGV）')
        .width('100%')
        .height(36)
        .fontSize(this.fz(12))
        .backgroundColor(C.chipDanger)
        .fontColor(DANGER)
        .margin({ top: 8 })
        .onClick(() => {
          this.confirmNativeSelfTest('SIGSEGV（故意空指针写）', 1);
        })

      Button('③ 触发 SIGABRT')
        .width('100%')
        .height(36)
        .fontSize(this.fz(12))
        .backgroundColor(C.chipDanger)
        .fontColor(DANGER)
        .margin({ top: 8 })
        .onClick(() => {
          this.confirmNativeSelfTest('SIGABRT（raise(SIGABRT)）', 2);
        })

      Text('② ③ 都会弹二次确认，文案里写明「App 会立刻闪退，这是预期行为；重启后首页会显示取证结果」；取消则什么都不会发生。')
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 6 })

      if (this.stNote.length > 0) {
        Text(this.stNote)
          .fontSize(this.fz(10))
          .fontColor(GOOD)
          .width('100%')
          .margin({ top: 10 })
      }

      if (this.stReport.length > 0) {
        Text('takeNativeCrashReport() 读回（' + this.stReport.length.toString() + ' 字符）')
          .fontSize(this.fz(11))
          .fontColor(SUB)
          .width('100%')
          .margin({ top: 10 })
        Text(this.stReport)
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .copyOption(CopyOptions.LocalDevice)
          .width('100%')
          .padding(8)
          .backgroundColor(INSET)
          .borderRadius(8)
          .margin({ top: 6 })
        Text('这条自检记录会留在 native_crash.txt 里：下次启动首页横幅会把它显示成「native 取证装置自检记录（非崩溃）」，' +
          '那就说明整条取证链路（setLogDir → 写盘 → 启动读回 → 清盘）全线打通了。')
          .fontSize(this.fz(10))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 6 })
        Button(this.stCopied ? '已复制' : '复制自检回读')
          .height(32)
          .fontSize(this.fz(11))
          .backgroundColor(CHIP)
          .fontColor(GOOD)
          .margin({ top: 8 })
          .onClick(() => {
            this.copySelfTestReport();
          })
      }
    }
    .width(this.cardW())
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .border({ width: 1, color: C.chipDanger })
    .margin({ top: 12 })
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  aboutBlock(title: string, body: string) {
    Column() {
      Text(title)
        .fontSize(this.fz(14))
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .width('100%')
      Text(body)
        .fontSize(this.fz(12))
        .fontColor(SUB)
        .lineHeight(20)
        .margin({ top: 8 })
        .width('100%')
    }
    .width(this.cardW())
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 12 })
  }

  // =========================================================================
  // 结果「三层呈现」的数据准备(在"跑完那一刻"算一次; 纯数据, 不在渲染循环里)
  //   L1 = 三个大数字(单核 / 多核 / GPU)
  //   L2 = 每个小节的关键得分一行(带单位 + 取得条件)
  //   L3 = 逐项明细(metric / unit / 得分 / 耗时 / 并行度 / 运行时频率 / 落核 / diag), 默认收起
  // 落盘的完整报告文件里仍然是全部内容 —— 界面只做分层, 不动任何数据。
  // =========================================================================
  private buildFullLayers(out: FullRunOutcome): void {
    const cond: CondFacts = condFromOutcome(out);
    this.fullCondText = cond.source;
    //  ★ 2026-10(10.0)位置对调: 大数字那一层给 **CS1**(用户:"把分数的主体突出部位变成 CS1")。
    //    取数与原来清单里那三行 CS1 同源(同一个 buildCs1Cards -> buildBigThree)。
    //    单次完整跑分那条路径(buildResLayers)不在这里 —— 它只跑自研套件, 没有 CS1 数据可用。
    this.fullL1 = buildCs1Cards(out, cond);
    this.fullL2 = buildKeyRows(out, cond);
    this.fullL3 = buildDetailGroups(out);
    this.fullDetailOpen = false;
  }

  // 单次完整跑分(自研套件)的结果浮层用同一套第一层 / 第二层
  private buildResLayers(rec: BenchRecord): void {
    const cond: CondFacts = condFromRecord(rec);
    this.resCondText = cond.source;
    this.resL1 = buildCards(rec, cond);
    this.resL2 = buildOwnKeyRows(rec, cond);
  }

  // L1 三张卡的主色焦点: 单核 = 主色 / 多核 = 次色 / GPU = 语义绿(与首页既有配色一致)
  private cardTone(key: string): string {
    if (key === 'multi') {
      return ACCENT2;
    }
    if (key === 'gpu') {
      return GOOD;
    }
    return ACCENT;
  }

  private detailTotal(): number {
    return detailCount(this.fullL3);
  }

  // ---- 「下载」目录落盘: 报告自动写入 <下载>/Crush分/report-YYYYMMDD-HHMMSS.txt ----
  // 权限没给 / 能力不支持 / 写不进去时, 这里返回的是真实原因, 并把它显示在界面上:
  // 不静默失败 —— 用户总能看到"写没写进去、没写进去是为什么、下一步点哪里"。
  private async saveReportToDownloads(text: string, ts: number): Promise<void> {
    if (text.length === 0) {
      this.dlStatus = '报告内容为空，没有写入「下载」目录。';
      return;
    }
    const name: string = reportStampName(ts);
    // ① 直接写 <下载>/<包名>/(已经授权过 / 系统允许访问自己包名目录时一次成功, 全程无弹框)
    let res: ReportWriteResult = ReportSink.writeText(name, text);
    let directErr: string = '';
    if (!res.ok) {
      directErr = res.err;
    }
    // ② ① 不行 -> 官方「下载模式」选择器(跳过文件选择界面, 只弹一次系统确认;
    //    返回的 URI 自带持久化权限, 之后就不用再弹了)
    if (!res.ok && this.appCtx !== null) {
      try {
        res = await ReportSink.saveViaDownloadPicker(this.appCtx, name, text);
      } catch (e) {
        res.err = '下载模式保存抛出异常：' + e.message;
      }
    }
    if (res.ok) {
      const latest: ReportWriteResult = ReportSink.writeLatest(text);
      this.dlPath = res.dir;
      this.dlFiles = ReportSink.listReports(8);
      this.dlStatus = '已写入「下载」目录（' + res.method + '）：' + res.path +
        (latest.ok ? ('（并更新了 ' + latest.path + '）') : '（report-latest.txt 更新失败）') +
        '。打开系统「文件管理 → ' + ReportSink.dirShown() + '」即可直接查看 / 分享。';
      return;
    }
    this.dlPath = '';
    const why: string = res.err.length > 0 ? res.err : ReportSink.lastError();
    this.dlStatus = '没能写进「下载」目录：' +
      (directErr.length > 0 ? ('直写失败（' + directErr + '）；') : '') + why +
      ' —— 兜底路径：点下面的「导出到文件」，用系统文档保存器自己选一次位置；' +
      '或者点「复制全文」直接粘贴。沙箱内那份报告一直都在，不会丢。';
  }

  // =========================================================================
  // 结果页三层的视图零件(全部是静态文字 + 静态色块, 没有任何动画 / 模糊 / 阴影)
  // =========================================================================
  // 第一层: 一个大数字卡(单核 / 多核 / GPU)
  //   字号与内边距由结果页密度给(common/LayoutPlan.ets 的 resultDecoFor): 大屏更大、
  //    小屏更紧; 三张卡横排时字号已经被"4 位数必须在自己的卡里放得下"这条硬约束夹过,
  //    再叠一层 minFontSize + maxLines(1) 的兜底 —— 任何窗口都不会换行、不会横向溢出。
  //   三处结果界面(一键跑分结果页 / 单次跑分结果浮层 / 首页分数卡)共用这一个零件,
  //    所以它们的层次与对齐永远一致: 标签(小) -> 大数字(大) + 单位(小, 同一基线) -> 条件(更小)。
  @Builder
  bigScoreCard(c: ScoreCard) {
    Column() {
      Text(c.label)
        .fontSize(this.uiRes.keyName)
        .fontColor(SUB)
      Row() {
        Text(c.value)
          .fontSize(this.uiRes.huge)
          .minFontSize(Math.max(16, Math.round(this.uiRes.huge * 0.55)))
          .maxFontSize(this.uiRes.huge)
          .heightAdaptivePolicy(TextHeightAdaptivePolicy.MIN_FONT_SIZE_FIRST)
          .maxLines(1)
          .fontWeight(FontWeight.Bold)
          .fontColor(c.has ? this.cardTone(c.key) : FAINT)
        Text(c.unit)
          .fontSize(this.uiRes.keyUnit)
          .fontColor(SUB)
          .margin({ left: 2, bottom: Math.round(this.uiRes.keyUnit * 0.35) })
      }
      .alignItems(VerticalAlign.Bottom)
      .justifyContent(FlexAlign.Center)
      Text(c.cond)
        .fontSize(this.uiRes.keyUnit)
        .fontColor(FAINT)
        .width('100%')
        .textAlign(TextAlign.Center)
        .maxLines(this.uiRes.l1CondLines)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .lineHeight(Math.round(this.uiRes.keyUnit * 1.25))
        // 与 LayoutPlan 的 L1_COND_TOP 同值(估算与渲染必须同源)
        .margin({ top: 2 })
    }
    //  这里是 2026-10 真机缺陷的根因, 改前写的是 .layoutWeight(1) 
    //  layoutWeight 分配的是父容器主轴上的尺寸, 而这个 Builder 的父节点是 **Column**
    //  (三处调用点都是 Column() { this.bigScoreCard(c) }), 主轴 = 竖直 —— 卡片高度于是变成
    //  "父容器给多少就吃多少", 与内容无关; 竖长窗口上每张卡被撑成吃满剩余高度的巨大空块,
    //  数字缩在中间, 卡与卡之间隔着几百 vp。改成固定高度(由结果页密度算好, 与首屏分块
    //  估算同源) + justifyContent 居中, 纵向再也不参与任何"剩余高度分配"。
    .width('100%')
    .height(this.uiRes.l1CardH)
    .justifyContent(FlexAlign.Center)
    .constraintSize({ minWidth: 104 })
    .alignItems(HorizontalAlign.Center)
    .padding({ left: 3, right: 3 })
    // 沉浸光感 b: 大数字的光晕(颜色跟着这个数本身的色调走); 关掉时是 SHADOW_NONE
    .shadow(this.fxAura(withAlpha(c.has ? this.cardTone(c.key) : FAINT, 0.28), 8))
    // 沉浸光感(真家伙): 大数字的点光(跑分期间由 fxLight() 的第一句挡掉)
    .visualEffect(this.fxLight(c.has ? this.cardTone(c.key) : FAINT, 0.70, 0.20))
  }

  // 第二层: 一行"名称 | 分值 | 单位 + 取得条件"
  //   改前: 名称占满一行、数字 20fp、单位另算, 取得条件另起一行 12fp, 上下各 7vp 空白
  //    —— 十行下来光是行内空白就有 140vp。改后: 行内上下只留 rowPy(2~5vp), 行与行之间
  //    用 1vp 分隔线区分, 数字与单位同一基线, 取得条件并到同一行(见 uiRes.inlineCond)。
  //   为什么条件要分两种排法: 这一行宽到能容下整条条件时并排(不截断, 也满足"紧跟单位之后");
  //    行太窄时并排一定会被省略号截掉半句话 —— 那种情况下宁可让它单独占一行完整显示,
  //    这样"少一行空白"换来的可读性不会被一个省略号又赔回去。窄到什么程度由计划算(520vp)。
  @Builder
  keyRowItem(r: KeyRow) {
    Column() {
      Row() {
        Text(r.name)
          .fontSize(this.uiRes.keyName)
          .fontColor(TEXT)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)
        // 同一列、右对齐 "数字 + 单位"整组放进一个固定宽的列里(宽度由结果页密度给),
        //  于是不管项名有长有短, 十个数字都落在同一条竖线上; 数字与单位同一基线。
        Row() {
          Text(r.value)
            .fontSize(this.uiRes.keyNum)
            .fontWeight(FontWeight.Bold)
            .fontColor(r.has ? ACCENT : FAINT)
            .maxLines(1)
          Text(r.unit)
            .fontSize(this.uiRes.keyUnit)
            .fontColor(SUB)
            .maxLines(1)
            .margin({ left: 2, bottom: Math.round(this.uiRes.keyUnit * 0.3) })
        }
        .width(this.uiRes.metricW)
        .justifyContent(FlexAlign.End)
        .alignItems(VerticalAlign.Bottom)
        if (this.uiRes.inlineCond) {
          Text(r.cond)
            .fontSize(this.uiRes.keyUnit)
            .fontColor(FAINT)
            .width(this.uiRes.condW)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .textAlign(TextAlign.End)
            .margin({ left: 6 })
        }
      }
      .width('100%')
      .alignItems(VerticalAlign.Bottom)
      if (!this.uiRes.inlineCond) {
        Text(r.cond)
          .fontSize(this.uiRes.keyUnit)
          .fontColor(FAINT)
          .width('100%')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .lineHeight(Math.round(this.uiRes.keyUnit * 1.25))
          .margin({ top: 1 })
      }
    }
    .width('100%')
    .padding({ top: this.uiRes.rowPy, bottom: this.uiRes.rowPy })
    .border(edgeBottom(DIVIDER))
  }

  // 第三层: 一组明细(自研单核 / 自研多核 / 自研 GPU / CS1 单核 / CS1 多核 / CS1 GPU / NPU / 存储)
  @Builder
  detailGroupBlock(g: DetailGroup) {
    Column() {
      Text(g.title)
        .fontSize(this.fz(Ft.small))
        .fontColor(ACCENT2)
        .width('100%')
        .margin({ top: 8, bottom: 2 })
      ForEach(g.rows, (d: DetailRow, i: number) => {
        Column() {
          Row() {
            Text(d.name)
              .fontSize(this.fz(Ft.body))
              .fontColor(TEXT)
              .layoutWeight(1)
            if (!d.scored) {
              Text('不计分')
                .fontSize(this.fz(Ft.cap))
                .fontColor(FAINT)
                .margin({ right: 6 })
            }
            Text(d.score)
              .fontSize(this.fz(Ft.sub))
              .fontWeight(FontWeight.Bold)
              .fontColor(d.ok ? GOOD : DANGER)
          }
          .width('100%')
          Text(d.meta)
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
            .width('100%')
            .margin({ top: 2 })
        }
        .width('100%')
        .padding(Sp.x2)
        .backgroundColor(INSET)
        .borderRadius(Rd.sm)
        .margin({ top: 4 })
      }, (d: DetailRow, i: number) => 'd' + g.title + '#' + i.toString())
    }
    .width('100%')
  }

  // 报告文件与「下载」目录(用户可见的落盘路径; 写不进去也有明确下一步)
  @Builder
  reportFileCard() {
    Column() {
      Text('报告文件（界面上只给你一眼能读懂的分层结论，完整内容全在文件里）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      // 落盘状态显示(2026-10-08) 以前这一行不管写成功还是失败都只显示"沙箱内：<路径>"
      //   (fullReportSaved 这个状态从来没有被显示过, 路径为空时才退化成"（未落盘）")。
      //   现在: 成功=绿色 + 真实路径; 失败/异常=警告色 + 真实原因; 还没生成=中性提示。
      Text(this.fullReportPaths.length === 0
        ? '沙箱内：（本次还没生成报告）'
        : (this.fullReportSaved ? ('已落盘：' + this.fullReportPaths)
          : ('未落盘 ' + this.fullReportPaths)))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.fullReportPaths.length === 0 ? FAINT : (this.fullReportSaved ? GOOD : WARN))
        .width('100%')
        .margin({ top: 6 })
      Text('「下载」目录：' + (this.dlStatus.length > 0 ? this.dlStatus : '（本次还没写）'))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.dlPath.length > 0 ? GOOD : WARN)
        .width('100%')
        .margin({ top: 6 })
      // 历史成绩.txt(同目录下的纯文本成绩单)单独一行: 与报告是两份文件, 各自说各自的落盘结果
      if (this.dlHistoryStatus.length > 0) {
        Text(SCORE_HISTORY_FILE + '：' + this.dlHistoryStatus)
          .fontSize(this.fz(Ft.cap))
          .fontColor(this.dlHistoryStatus.indexOf('已写入') >= 0 ? GOOD : WARN)
          .width('100%')
          .margin({ top: 4 })
      }
      if (this.dlFiles.length > 0) {
        Text('该文件夹里已有：' + this.dlFiles.join(' · '))
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 4 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE))
    .borderRadius(this.cardRadius())
    .margin({ top: this.rowGap() })
  }

  // =========================================================================
  // 「一键全部跑分」入口卡(首页)
  // =========================================================================
  @Builder
  fullRunCard() {
    Column() {
      // 第一优先 标题行: 页面里最大的一个字 + 主色"第一步"标
      Row() {
        Text('一键全部跑分')
          .fontSize(this.fz(24))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
        Blank()
        Text('第一步')
          .fontSize(this.fz(Ft.cap))
          .fontColor(ON_ACCENT)
          .backgroundColor(ACCENT)
          .borderRadius(Rd.pill)
          .padding({ left: 9, right: 9, top: 3, bottom: 3 })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      Text('按一下就跑完 7 个小节：自研套件（单核 + 多核 + GPU 场景）→ CS1 单核 → CS1 多核 → CS1 GPU → GPU-SNL（Aurora Nomad Light · 不计分）→ NPU → 存储 I/O。' +
        '跑完自动出报告，并自动写入系统「下载」目录下的本应用文件夹。')
        .fontSize(this.fz(Ft.body))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 8 })
      // 主行动按钮: 整行宽、主色填充、字形放大 —— 全页最显眼的那一个控件
      Button(this.fullRunning
        ? ('全部跑分进行中 · ' + this.fullProgressText())
        : '▶  开始一键全部跑分')
        .width('100%')
        .constraintSize({ minHeight: this.btnH() + 16 })
        .fontSize(this.fz(18))
        .fontWeight(FontWeight.Bold)
        .backgroundColor(ACCENT)
        // 沉浸光感 c: 主按钮上的光带(关掉时两端同色 —— 与纯色填充逐像素相同)
        .linearGradient(this.fxSheen(ACCENT, ACCENT2))
        // 沉浸光感 b: 主按钮的光晕(关掉时 fxAura 给的是 SHADOW_NONE: 半径 0 + 全透明)
        .shadow(this.fxAura(withAlpha(ACCENT, 0.40), 12))
        // 沉浸光感(真家伙): HDS 点光源再叠一层(点光优先; 关掉时给的是"空效果")
        .visualEffect(this.fxLight(ACCENT2, 0.80, 0.28))
        .fontColor(ON_ACCENT)
        .borderRadius(Rd.md)
        .margin({ top: 14 })
        .enabled(!this.fullRunning)
        .onClick(() => {
          this.startFullRun();
        })
      // 「下载」目录权限/落盘状态: 开跑前就把这件事说清楚
      if (this.dlStatus.length > 0) {
        Text(this.dlStatus)
          .fontSize(this.fz(Ft.cap))
          .fontColor(this.dlPath.length > 0 ? GOOD : FAINT)
          .width('100%')
          .margin({ top: 8 })
      }
      Text('跑分期间：无任何动画 · 每完成一项才刷新一次界面 · 保持亮屏 · 随时可点「停止」（当前项跑完即收工并出报告）。')
        .fontSize(this.fz(10))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 8 })
      if (this.fullHasOldReport) {
        Text('files/report-latest.txt 已有上一次的报告（' + this.fullOldReportBytes.toString() +
          ' 字节）：')
          .fontSize(this.fz(10))
          .fontColor(SUB)
          .width('100%')
          .margin({ top: 10 })
        Row() {
          Button('查看最近报告')
            .constraintSize({ minHeight: this.btnH() })
            .fontSize(this.fz(12))
            .backgroundColor(CHIP)
            .fontColor(ACCENT2)
            .layoutWeight(1)
            .onClick(() => {
              this.openLatestReport();
            })
          Button('导出最近报告')
            .constraintSize({ minHeight: this.btnH() })
            .fontSize(this.fz(12))
            .backgroundColor(CHIP)
            .fontColor(GOOD)
            .layoutWeight(1)
            .margin({ left: 8 })
            .onClick(() => {
              this.exportFullReport();
            })
        }
        .width('100%')
        .margin({ top: 6 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD_HI)
    // 主色 2vp 描边(BORDER_THICK): 让"按这个就对了"这件事在视觉上不可错过。
    // 描边本身仍然只是一条静态线; 那圈"光感"是下面的 shadow, 关掉美化时半径 0。
    .border({ width: BORDER_THICK, color: ACCENT })
    .borderRadius(this.cardRadius())
    .shadow(this.fxAura(withAlpha(ACCENT, 0.16), 14))
    // 沉浸光感(真家伙): 整张主行动卡的点光(HDS 拿不到时自动退回上面那层光晕)
    .visualEffect(this.fxLight(ACCENT, 0.55, 0.20))
  }

  // =========================================================================
  // 首页 · 「最近一次一键跑分」的一目了然预览: 单核 / 多核 / GPU 三个大数字
  //   (与结果页第一层同一套数据与同一个大数字卡, 不重复实现)
  // =========================================================================
  // 「本机性能天花板」的显示文本(2026-10 追加): 首页设备卡 / 结果页首屏 / 报告三处共用。
  //   优先用启动时同步下来的那一份(ceilingLine); 它还没取到时现场问一次 native;
  //   两次都拿不到就返回兜底文案 —— 绝不用本地拼的近似句冒充读数。
  private ceilingShow(): string {
    const s: string = this.ceilingLine.length > 0 ? this.ceilingLine : BenchRunner.ceiling();
    return s.length > 0 ? s
      : '本机性能天花板：（native 未上报 —— 旧版 .so 没有 ceilingText 导出，或读取失败）';
  }

  @Builder
  oneTapPreview() {
    Column() {
      Row() {
        Text('最近一次一键跑分')
          .fontSize(this.fz(Ft.sub))
          .fontColor(SUB)
        Blank()
        Text('看完整结果')
          .fontSize(this.fz(Ft.small))
          .fontColor(ACCENT)
      }
      .width('100%')
      .onClick(() => {
        this.openLatestReport();
      })
      // 首页分数卡 与结果页第一层同一个零件、同一份密度(this.uiRes):
      //  改前三个大数字上下只留 4vp、整卡用首页的 cardPad; 改后内边距与结果页一致,
      //  并且加了一圈高亮底 + 主色描边 —— 首页第一眼就是"单核 / 多核 / GPU"这三行结论。
      Flex({
        direction: FlexDirection.Row,
        wrap: FlexWrap.Wrap,
        justifyContent: FlexAlign.Center,
        // 只按自己的固定高度排, 交叉轴(竖直)不拉伸
        //  不写死这一条的话, 容器一旦拿到多余高度, 三张卡就有机会在竖直方向被撑开 ——
        //  显式 Start 让"多出来的高度"只可能留在容器外面(列表 / 滚动区)。
        alignItems: ItemAlign.Start
      }) {
        ForEach(this.fullL1, (c: ScoreCard) => {
          Column() {
            this.bigScoreCard(c)
          }
          // 固定高度: 由结果页密度算好(与首屏分块估算同源), 不参与剩余高度分配
          .height(this.uiRes.l1CardH)
          .flexGrow(1)
          // 基准宽 = "结果页自己的可用宽度里一行放几张"(不再是首页结果卡的列数)
          .flexBasis(this.l1Basis())
          .constraintSize({ minWidth: this.l1MinW() })
        }, (c: ScoreCard) => 'hp' + c.key)
      }
      .width('100%')
      .padding({ top: this.uiRes.pad, bottom: this.uiRes.pad })
      .backgroundColor(CARD_HI)
      .border({ width: BORDER_THICK, color: ACCENT })
      .borderRadius(this.cardRadius())
      .margin({ top: this.uiRes.gap })
    }
    .width('100%')
    .padding(this.uiRes.pad)
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE))
    .borderRadius(this.cardRadius())
  }

  // GPU-SNL 单项结果块: 分数 / FPS / 吞吐 / gpuBoundEvidence.verdict / 标定状态, 全部显示。
  // 未标定时必须写明"绝对值不可当 3DMark 分数用, 只可比同设备两次运行的比值"。
  @Builder
  snSoloResultBlock() {
    Column() {
      if (this.snHave && !this.snOk && this.snPrepErr.length === 0) {
        Text('GPU-SNL 未取得结果：' +
          (this.snErrMsg.length > 0 ? this.snErrMsg
            : '原因未记录（本节失败但界面没拿到原因文本；见报告 GPU-SNL 段的“原始 JSON 解析”一行）'))
          .fontSize(this.fz(Ft.cap))
          .fontColor(DANGER)
          .width('100%')
      }
      if (this.snHave) {
        Text('GPU-SNL · Aurora Nomad Light' +
          (this.snOk && this.snPrepErr.length === 0 ? '' : '（本节失败）'))
          .fontSize(this.fz(Ft.small))
          .fontColor(ACCENT2)
          .width('100%')
        if (this.snPrepErr.length > 0) {
          Text('初始化失败, 驱动 / EGL 原文（原样显示, 未做任何加工）：')
            .fontSize(this.fz(Ft.cap))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 4 })
          Text(this.snPrepErr)
            .fontSize(this.fz(Ft.cap))
            .fontColor(WARN)
            .width('100%')
            .padding(Sp.x2)
            .backgroundColor(C.chipWarn)
            .borderRadius(Rd.sm)
            .margin({ top: 2 })
        } else {
          Text('分数 ' + this.snScoreV.toFixed(1) + ' 分（不计分 · native scored=false）· ' +
            this.snFpsV.toFixed(2) + ' FPS · 吞吐 ' + this.snTputV.toFixed(2) + ' Mpx/s')
            .fontSize(this.fz(Ft.cap))
            .fontColor(TEXT)
            .width('100%')
            .margin({ top: 4 })
          Text('自证 ' + (this.snVerdictV.length > 0 ? this.snVerdictV : '—') +
            '（conclusive=' + (this.snConclusiveV ? 'true' : 'false') + '）· ' +
            '每帧 draw call ' + (this.snDrawV >= 0 ? this.snDrawV.toFixed(0) : '—') +
            ' · CPU 顶点数据 ' + (this.snCpuVertV >= 0 ? this.snCpuVertV.toFixed(0) : '—') +
            ' 字节/帧（应为 0）· 提交占空 ' + this.snDutyV.toFixed(3))
            .fontSize(this.fz(Ft.cap))
            .fontColor(SUB)
            .width('100%')
            .margin({ top: 2 })
          Text('自适应趟数 ' + this.snPassesV.toFixed(0) + '（依据 ' +
            (this.snScaleSrcV.length > 0 ? this.snScaleSrcV : '—') + '）· 标尺 CPU ' +
            this.snLogiV.toFixed(0) + ' 逻辑核 / ' + this.snPhysV.toFixed(0) + ' 物理核 · SIMD arch ' +
            this.snArchV.toString() + (this.snNeonV ? ' +NEON' : '') +
            (this.snSve2V ? ' +SVE2' : '') + ' · 可重复性 ' +
            (this.snCredV.length > 0 ? this.snCredV : '—') + '（离散度 ' +
            this.snDispV.toFixed(2) + '%）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(SUB)
            .width('100%')
            .margin({ top: 2 })
          Text(this.snCalV.length > 0 && this.snCalV !== 'CALIBRATED'
            ? ('标定状态：' + this.snCalV + '（k=' + this.snCalKV.toFixed(4) +
              '）—— 绝对值未经标定, 不可当 3DMark 分数用, 只可用于同设备两次运行的比值。')
            : ('标定状态：' + (this.snCalV.length > 0 ? this.snCalV : '(native 未给)') + '（k=' +
              this.snCalKV.toFixed(4) + '）'))
            .fontSize(this.fz(Ft.cap))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 4 })
          Text('环境 ' + (this.snEnvV.length > 0 ? this.snEnvV : '—') + ' · 温度中位 ' +
            this.snTempV.toFixed(1) + ' degC · CPU 频率中位 ' + (this.snKhzV / 1000).toFixed(0) +
            ' MHz · benchVersion ' + this.snVerV.toString() + '（跨版本不可比）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
            .width('100%')
            .margin({ top: 2 })
          if (this.snWarnV.length > 0) {
            Text('native warnings / risks：' + this.snWarnV)
              .fontSize(this.fz(Ft.cap))
              .fontColor(WARN)
              .width('100%')
              .margin({ top: 2 })
          }
        }
      }
    }
    .width('100%')
    .padding(Sp.x2)
    .backgroundColor(C.inset)
    .borderRadius(Rd.sm)
    .margin({ top: 6 })
  }

  // 首页 · 次要入口的折叠标题(原有各项单独入口一个都没删, 只是默认收起)
  @Builder
  soloEntryHeader() {
    Row() {
      Text(this.showSolo ? '▾' : '▸')
        .fontSize(this.fz(Ft.body))
        .fontColor(SUB)
      Text(this.showSolo
        ? '单项测试（点一下收起）'
        : '单项测试：单次完整跑分 / 压力测试 / CoreMark / CS1 单核 · 多核 · GPU / GPU-SNL / 长跑自检 / 两份报告对比（点一下展开）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .layoutWeight(1)
        .margin({ left: 6 })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE))
    .borderRadius(this.cardRadius())
    .margin({ top: this.rowGap() })
    .onClick(() => {
      this.showSolo = !this.showSolo;
      // 展开时顺手刷新一下本机报告清单(只读一次目录, 不轮询、不阻塞)
      if (this.showSolo) {
        this.cmpListLocal();
      }
    })
  }

  // =========================================================================
  // 「两份报告对比」入口卡 —— 放在首页「单项测试」折叠区里(默认收起, 不抢主流程的位置)
  // =========================================================================
  @Builder
  cmpCard() {
    Column() {
      Row() {
        Text('两份报告对比')
          .fontSize(this.fz(Ft.title))
          .fontColor(TEXT)
          .layoutWeight(1)
        Text('只读 · 纯计算')
          .fontSize(this.fz(Ft.cap))
          .fontColor(C.onAccent)
          .backgroundColor(GOOD)
          .borderRadius(Rd.pill)
          .padding({ left: 8, right: 8, top: 2, bottom: 2 })
      }
      .width('100%')
      Text('回答的是「两台设备的分数比，能不能定量算出芯片进步的比例」：在两台设备上各跑一次「一键全部跑分」，' +
        '把两份 report-latest.json 选进来，逐项给出 原始分数比 / 每核归一化比 / 同频归一化比 / 与真值的偏差%。' +
        '原始比会被核数（9 核 vs 8 核 = 虚高 12.5%）与频率档（1.135x = 13.5% 假提升）污染，' +
        '所以要拿芯片性能比请看两个归一化比。')
        .fontSize(this.fz(Ft.small))
        .fontColor(FAINT)
        .lineHeight(16)
        .width('100%')
        .margin({ top: 6 })
      Text('注：这是纯计算模块，不跑任何负载、不读 /sys、不写文件、不改任何分数；' +
        '归一化的前提是「吞吐 ∝ 频率」，跨架构时那只是假设，报告里已逐条标注。')
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 4 })
      Button(this.cmpBusy ? '对比进行中…' : '打开对比（选择两份报告）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(Ft.sub))
        .backgroundColor(C.chip)
        .fontColor(ACCENT)
        .margin({ top: 8 })
        .enabled(!this.cmpBusy)
        .onClick(() => {
          this.openCompare();
        })
      if (this.cmpA !== null || this.cmpB !== null) {
        Text(this.cmpPickLabel(1) + '  /  ' + this.cmpPickLabel(2))
          .fontSize(this.fz(Ft.cap))
          .fontColor(SUB)
          .width('100%')
          .margin({ top: 6 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 12 })
  }

  // ---- 结果浮层: 选择区 / 缺输入 / 复合分头行 / 污染源 / 逐项表 / 真值锚点 / 口径 ----
  @Builder
  cmpPickBlock() {
    Column() {
      Text('① 选两份报告（设备B / 设备A 就是下面所有比值的口径）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      Text(this.cmpPickLabel(1))
        .fontSize(this.fz(Ft.body))
        .fontColor(this.cmpA !== null ? TEXT : FAINT)
        .width('100%')
        .margin({ top: 6 })
      Text(this.cmpPickLabel(2))
        .fontSize(this.fz(Ft.body))
        .fontColor(this.cmpB !== null ? TEXT : FAINT)
        .width('100%')
        .margin({ top: 2 })
      Row() {
        Button('从文件中选择 A')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(Ft.small))
          .backgroundColor(C.chip)
          .fontColor(ACCENT)
          .layoutWeight(1)
          .enabled(!this.cmpBusy)
          .onClick(() => {
            this.cmpPick(1);
          })
        Button('从文件中选择 B')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(Ft.small))
          .backgroundColor(C.chip)
          .fontColor(ACCENT)
          .layoutWeight(1)
          .margin({ left: 8 })
          .enabled(!this.cmpBusy)
          .onClick(() => {
            this.cmpPick(2);
          })
      }
      .width('100%')
      .margin({ top: 8 })
      Text('SoC 显式指定（可选，推荐）：留空 = 让 native 从报告文本里判；' +
        '判不出来时与真值的对照整体不成立，这时请在这里填上，例如 Kirin 9030 Pro')
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 8 })
      TextInput({ text: this.cmpSocA, placeholder: '设备A 的 SoC（留空 = 自动判）' })
        .height(36)
        .fontSize(this.fz(Ft.cap))
        .backgroundColor(C.inset)
        .fontColor(TEXT)
        .margin({ top: 6 })
        .onChange((v: string) => {
          this.cmpSocA = v;
        })
      TextInput({ text: this.cmpSocB, placeholder: '设备B 的 SoC（留空 = 自动判）' })
        .height(36)
        .fontSize(this.fz(Ft.cap))
        .backgroundColor(C.inset)
        .fontColor(TEXT)
        .margin({ top: 4 })
        .onChange((v: string) => {
          this.cmpSocB = v;
        })
      Text('本机 files/ 下已有的报告（一键全部跑分的产物，点一下设为 A 或 B）')
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 10 })
      if (this.cmpLocal.length === 0) {
        Text('（本机还没有落盘的 report-*.json：先跑一次「一键全部跑分」，或用上面的按钮从别处选一份）')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 4 })
      }
      ForEach(this.cmpLocal, (cf: CmpFile) => {
        Row() {
          Text(cf.name + (cf.ok ? '' : '（读失败）'))
            .fontSize(this.fz(Ft.cap))
            .fontColor(cf.ok ? TEXT : DANGER)
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Button('设为A')
            .fontSize(this.fz(Ft.cap))
            .backgroundColor(C.chip)
            .fontColor(ACCENT)
            .onClick(() => {
              this.cmpUseFile(cf, 1);
            })
          Button('设为B')
            .fontSize(this.fz(Ft.cap))
            .backgroundColor(C.chip)
            .fontColor(ACCENT2)
            .margin({ left: 6 })
            .onClick(() => {
              this.cmpUseFile(cf, 2);
            })
        }
        .width('100%')
        .margin({ top: 4 })
      }, (cf: CmpFile, i: number) => 'cmpf' + i.toString())
      Button(this.cmpBusy ? '对比进行中…' : '② 开始对比')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(Ft.body))
        .backgroundColor(ACCENT)
        .fontColor(ON_ACCENT)
        .width('100%')
        .margin({ top: 10 })
        .enabled(!this.cmpBusy)
        .onClick(() => {
          this.cmpRun();
        })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }
  // cmp.ok !== true 时: 原样显示 native 说的缺什么 —— 界面不替它编一个数、不猜
  @Builder
  cmpFailBlock() {
    Column() {
      Text('对比没有成立（cmp.ok !== true）')
        .fontSize(this.fz(Ft.sub))
        .fontColor(DANGER)
        .width('100%')
      Text('下面这一行是 native 的原话（缺什么就写什么，界面不做任何加工与猜测）：')
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 4 })
      Text(this.cmpResult().error.length > 0 ? this.cmpResult().error : '（native 未给原因）')
        .fontSize(this.fz(Ft.cap))
        .fontColor(WARN)
        .lineHeight(16)
        .width('100%')
        .padding(Sp.x2)
        .backgroundColor(C.inset)
        .borderRadius(Rd.sm)
        .margin({ top: 6 })
      if (this.cmpResult().problemA.length > 0 || this.cmpResult().problemB.length > 0) {
        Text('设备A 的问题（native 原话）：' +
          (this.cmpResult().problemA.length > 0 ? this.cmpResult().problemA : '（native 没写）'))
          .fontSize(this.fz(Ft.cap))
          .fontColor(WARN)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 6 })
        Text('设备B 的问题（native 原话）：' +
          (this.cmpResult().problemB.length > 0 ? this.cmpResult().problemB : '（native 没写）'))
          .fontSize(this.fz(Ft.cap))
          .fontColor(WARN)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 2 })
      }
      if (this.cmpResult().missing.length > 0) {
        Text('inputs.missing（缺什么就写什么）：')
          .fontSize(this.fz(Ft.cap))
          .fontColor(SUB)
          .width('100%')
          .margin({ top: 8 })
        ForEach(this.cmpResult().missing, (m: string, i: number) => {
          Text('· ' + m)
            .fontSize(this.fz(Ft.cap))
            .fontColor(WARN)
            .width('100%')
            .margin({ top: 2 })
        }, (m: string, i: number) => 'cmpmiss' + i.toString())
        Text(this.cmpResult().missingRule)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 4 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // 头行: 两个复合分的比 + 归一化后的几何平均比(这是「芯片进步比例」最直接的两个数)
  @Builder
  cmpSummaryBlock() {
    Column() {
      Text('两者是谁')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      Text('设备A：' + (this.cmpResult().deviceA.length > 0 ? this.cmpResult().deviceA : '（报告里没写机型）') +
        ' · SoC ' + (this.cmpResult().socA.length > 0 ? this.cmpResult().socA : '判不出来') +
        ' · ' + this.cmpResult().verA + (this.cmpResult().interruptedA ? ' · 中断的报告' : ''))
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 2 })
      Text('设备B：' + (this.cmpResult().deviceB.length > 0 ? this.cmpResult().deviceB : '（报告里没写机型）') +
        ' · SoC ' + (this.cmpResult().socB.length > 0 ? this.cmpResult().socB : '判不出来') +
        ' · ' + this.cmpResult().verB + (this.cmpResult().interruptedB ? ' · 中断的报告' : ''))
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 2 })
      Text(this.cmpResult().ratioDefinition.length > 0 ? this.cmpResult().ratioDefinition
        : 'ratio = 设备B 的值 / 设备A 的值（> 1 表示设备B 更高）')
        .fontSize(this.fz(Ft.cap))
        .fontColor(ACCENT)
        .width('100%')
        .margin({ top: 6 })
      //  用户真正要的那个数: 「本次两台的分数比, 换算成芯片性能比是多少」
      // 放在复合分之前的最显眼位置: 前面那些数(原始比/每核比/同频比)都是过程,
      // 这一块才是结论。数字与措辞全部来自 native 的 chipPerformanceRatio, 界面不自己算。
      Text('本次两台：分数比 → 芯片性能比')
        .fontSize(this.fz(Ft.small))
        .fontColor(ACCENT)
        .width('100%')
        .margin({ top: 10 })
      Row() {
        Column() {
          Text('原始多核分数比（B/A）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
          Text(fmtRatio(this.cmpResult().chipRatioBA, this.cmpResult().hasChipRatioBA))
            .fontSize(this.fz(Ft.body))
            .fontColor(WARN)
            .margin({ top: 2 })
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
        Column() {
          Text('芯片性能比（消掉核数与频率档）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
          Text(fmtRatio(this.cmpResult().chipBest, this.cmpResult().hasChipBest))
            .fontSize(this.fz(Ft.title))
            .fontColor(GOOD)
            .margin({ top: 2 })
          Text(this.cmpResult().hasChipBest
            ? ('用 ' + (this.cmpResult().chipBestWhich.length > 0
              ? this.cmpResult().chipBestWhich : '归一化比'))
            : '缺输入，写 —，不拿分数比冒充')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
            .maxLines(2)
            .lineHeight(14)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .margin({ top: 4 })
      Text(this.cmpResult().chipConclusion.length > 0 ? this.cmpResult().chipConclusion
        : '（native 没给 chipPerformanceRatio 结论块——旧版 .so。请用下面的每核归一化比与同频归一化比自行判断：原始比 = 芯片性能比 × 核数比 × 频率档比。）')
        .fontSize(this.fz(Ft.cap))
        .fontColor(ACCENT)
        .lineHeight(16)
        .width('100%')
        .margin({ top: 6 })
      Text('复合分（两个数口径不同，都很接近但不保证相等）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 10 })
      ForEach(this.cmpResult().comps, (c: CmpCompositeView) => {
        Column() {
          Text(c.title)
            .fontSize(this.fz(Ft.body))
            .fontColor(TEXT)
            .width('100%')
          Text(this.cmpCompLine(c))
            .fontSize(this.fz(Ft.cap))
            .fontColor(c.hasRatio ? WARN : FAINT)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
        }
        .width('100%')
        .margin({ top: 6 })
      }, (c: CmpCompositeView) => 'cmpcomp' + c.key)
      Text('归一化之后的复合比（消掉核数差与频率档差；这才是芯片性能比的口径）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
        .margin({ top: 10 })
      Row() {
        Column() {
          Text('每核归一化比（GM）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
          Text(fmtRatio(this.cmpResult().gmPerCore, this.cmpResult().hasGmPerCore))
            .fontSize(this.fz(Ft.title))
            .fontColor(GOOD)
            .margin({ top: 2 })
          Text('参与 ' + this.cmpResult().gmPerCoreItems.toString() + ' 项')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
        Column() {
          Text('同频归一化比（GM）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
          Text(fmtRatio(this.cmpResult().gmSameFreq, this.cmpResult().hasGmSameFreq))
            .fontSize(this.fz(Ft.title))
            .fontColor(GOOD)
            .margin({ top: 2 })
          Text('参与 ' + this.cmpResult().gmSameFreqItems.toString() + ' 项')
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .margin({ top: 4 })
      Text(this.cmpResult().gmWhy)
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 6 })
      Text('原始比 = 芯片性能比 × 核数比 × 频率档比。核数不同会把分数拉高 12.5%（9 核 vs 8 核），频率档不同又会凭空多出 13.5%。要看芯片性能比，用上面两个归一化比。（归一化假设「吞吐 ∝ 频率」。）')
        .fontSize(this.fz(Ft.cap))
        .fontColor(WARN)
        .lineHeight(16)
        .width('100%')
        .margin({ top: 8 })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // 两台设备「取得条件」的实际差异 + 已知污染源(逐条列出 native 给的处理办法)
  @Builder
  cmpInflationBlock() {
    Column() {
      Text('已知污染源（为什么原始比不能直接当性能比）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      Text('本次两边的实际差异：允许核集合 A=' + (this.cmpResult().allowedA > 0 ? this.cmpResult().allowedA.toFixed(0) : '未上报') +
        ' / B=' + (this.cmpResult().allowedB > 0 ? this.cmpResult().allowedB.toFixed(0) : '未上报') +
        ' · 核数比(B/A) ' + fmtNum(this.cmpResult().coreRatioBA, 4, this.cmpResult().hasCoreRatioBA) +
        ' · 频率档比(B/A) ' + fmtNum(this.cmpResult().freqRatioBA, 4, this.cmpResult().hasFreqRatioBA))
        .fontSize(this.fz(Ft.cap))
        .fontColor(ACCENT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 4 })
      ForEach(this.cmpResult().inflations, (inf: CmpInflationView, i: number) => {
        Column() {
          Text('· ' + inf.name)
            .fontSize(this.fz(Ft.body))
            .fontColor(WARN)
            .width('100%')
          Text(inf.howMuch)
            .fontSize(this.fz(Ft.cap))
            .fontColor(TEXT)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
          Text('消掉它的办法：' + inf.handledBy)
            .fontSize(this.fz(Ft.cap))
            .fontColor(SUB)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
        }
        .width('100%')
        .margin({ top: 8 })
      }, (inf: CmpInflationView, i: number) => 'cmpinf' + i.toString())
      if (this.cmpResult().coreNote.length > 0) {
        Text(this.cmpResult().coreNote)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 6 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }
  // 逐项表的列宽(vp): 表头与数据行共用同一组, 保证"原始比 / 每核 / 同频 / 偏差 / 5%内"五列对齐
  @Builder
  cmpItemTable() {
    Column() {
      Text('逐项对照表（原始比与两个归一化比并排显示）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      Text('每项两行：① 项名 · 设备A 分 · 设备B 分   ② 原始比 | 每核归一化比 | 同频归一化比 | 与真值偏差% | 是否在 5% 内')
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      Text('原始比 = 芯片性能比 × 核数比 × 频率档比，会被核数与频率档污染；要看芯片性能比请看两个归一化比')
        .fontSize(this.fz(Ft.cap))
        .fontColor(WARN)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 4 })
      Row() {
        Text('项名')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .layoutWeight(1)
        Text('设备A')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width(58)
          .textAlign(TextAlign.End)
        Text('设备B')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width(58)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .margin({ top: 8 })
      Row() {
        Text('原始比')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width(76)
        Text('每核归一化比')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width(62)
        Text('同频归一化比')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width(62)
        Text('偏差%')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width(52)
        Text('5%内')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width(34)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .margin({ top: 2 })
      ForEach(this.cmpResult().items, (r: CmpItemView, i: number) => {
        this.cmpItemRow(r, i);
      }, (r: CmpItemView, i: number) => 'cmpit' + i.toString())
      Text('统计：匹配 ' + this.cmpResult().matched.toString() + ' 项 · 只在A ' +
        this.cmpResult().onlyA.toString() + ' 项 · 只在B ' + this.cmpResult().onlyB.toString() +
        ' 项（只在一侧的项照样列出来，不静默丢掉）')
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .width('100%')
        .margin({ top: 8 })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // 排除集结论的颜色: 不一致 = 危险(红) / 一致 = 通过(绿) / 判不出来 = 灰
  private cmpAuditColor(): string {
    const t: number = this.cmpResult().auditIdentical;
    return t === 0 ? DANGER : (t === 1 ? GOOD : FAINT);
  }

  // 同项性结论的颜色
  private cmpIdenticalColor(t: number): string {
    return t === 0 ? DANGER : (t === 1 ? GOOD : FAINT);
  }

  // 条件差结论的颜色: 条件不同 = 警告(黄) / 相同 = 通过(绿) / 判不出来 = 灰
  private cmpCondColor(): string {
    const t: number = this.cmpResult().condSame;
    return t === 0 ? WARN : (t === 1 ? GOOD : FAINT);
  }

  // 一侧的频率证据一行文本(native 给什么写什么; 缺就写缺)
  private cmpFreqLine(tag: string, c: CmpCondSide): string {
    const med: string = c.khzMedian > 0 ? (Math.round(c.khzMedian / 1000).toString() + 'MHz') : '—（没有读数）';
    const top: string = c.khzTop > 0 ? (Math.round(c.khzTop / 1000).toString() + 'MHz') : '—（没有读数）';
    const note: string = (c.noteKhzMedian > 0 || c.noteKhzTop > 0)
      ? ('（报告原话：代表项中位 ' + Math.round(c.noteKhzMedian / 1000).toString() + 'MHz · 全机最快档 ' +
        Math.round(c.noteKhzTop / 1000).toString() + 'MHz）') : '';
    return tag + '：逐项运行时频率中位 ' + med + '（' + c.khzSamples.toString() + ' 项有读数）· ' +
      '可用最高频档 ' + top + note;
  }

  // 条件差一行文本
  private cmpCondLine(tag: string, c: CmpCondSide): string {
    const smt: string = c.smt === 1 ? 'SMT 开' : (c.smt === 0 ? 'SMT 关' : 'SMT 状态未上报');
    return tag + '：多核线程 ' + (c.threads > 0 ? c.threads.toString() : '未上报') + ' · ' + smt +
      ' · 可用核集合 ' + (c.allowedCores > 0 ? c.allowedCores.toString() : '未上报') + ' 核' +
      '（物理 ' + c.physicalCores.toString() + ' / 逻辑 ' + c.logicalCores.toString() + '）';
  }

  // =========================================================================
  //  任务一: 两边的『随芯片变化自检』清单对照(哪几项不可用于跨芯片比较)
  //    2026-10-05 更正: 这份清单不改变复合分的参与项集合(复合分的参与项 = 该阶段所有
  //    "计分且拿到正分"的项)。它回答的是"两台设备上哪几项的数字不可信", 决定的是"读比时要留意
  //    什么"; "两个复合分含的是不是同一批项、原始比能不能相除"由下面「参与项集合(同项性)」那一段
  //    回答(它读的是报告自己给出的参与项名单)。两者都单列在最显眼的位置, 但不是同一件事。
  //    本块一个字都不算, 全部是 native 的原话 + 原字段。
  // =========================================================================
  @Builder
  cmpSelfCheckBlock() {
    Column() {
      Text('先看这个 两边的『随芯片变化自检』清单对照（哪几项不可用于跨芯片比较）')
        .fontSize(this.fz(Ft.body))
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .width('100%')
      Text(this.cmpResult().auditWhatItIs.length > 0 ? this.cmpResult().auditWhatItIs
        : '这份清单只说明"哪几项不可用于跨芯片比较"，它不改变复合分的参与项集合；' +
          '复合分由哪些项组成、原始比能不能相除，见下面「复合分的参与项集合（同项性）」。')
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      Text('结论：' + (this.cmpResult().auditConclusion.length > 0 ? this.cmpResult().auditConclusion
        : '（native 未上报结论）'))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.cmpAuditColor())
        .lineHeight(16)
        .width('100%')
        .margin({ top: 6 })
      if (this.cmpResult().auditOnlyA.length > 0) {
        Text('只在设备A 判不通过（' + this.cmpResult().auditOnlyA.length.toString() + ' 项）：' +
          this.cmpResult().auditOnlyA.join('、'))
          .fontSize(this.fz(Ft.cap))
          .fontColor(DANGER)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
      }
      if (this.cmpResult().auditOnlyB.length > 0) {
        Text('只在设备B 判不通过（' + this.cmpResult().auditOnlyB.length.toString() + ' 项）：' +
          this.cmpResult().auditOnlyB.join('、'))
          .fontSize(this.fz(Ft.cap))
          .fontColor(DANGER)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
      }
      if (this.cmpResult().auditCommon.length > 0) {
        Text('两边共同判不通过（' + this.cmpResult().auditCommon.length.toString() + ' 项）：' +
          this.cmpResult().auditCommon.join('、'))
          .fontSize(this.fz(Ft.cap))
          .fontColor(SUB)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
      }
      this.cmpAuditSideBlock('设备A', this.cmpResult().auditA);
      this.cmpAuditSideBlock('设备B', this.cmpResult().auditB);
      if (this.cmpResult().auditCriteria.length > 0) {
        Text('判据（阈值都是我们自己定的，不是官方阈值）：')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 6 })
        ForEach(this.cmpResult().auditCriteria, (c: string, i: number) => {
          Text('· ' + c)
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
        }, (c: string, i: number) => 'audc' + i.toString())
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE))
    .borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  @Builder
  cmpAuditSideBlock(tag: string, side: CmpAuditSide) {
    Column() {
      Text(tag + ' 的自检清单（' + side.excludedCount.toString() + ' 项不可用于跨芯片比较；通过 ' + side.passCount.toString() +
        ' · 不通过 ' + side.failCount.toString() + ' · 未自证 ' + side.unprovenCount.toString() + '）')
        .fontSize(this.fz(Ft.cap))
        .fontColor(side.present ? SUB : WARN)
        .width('100%')
        .margin({ top: 8 })
      if (!side.present) {
        Text('  这份报告里没有随芯片变化自检的结果 -> 哪几项不可用于跨芯片比较判不出来（不假定它与另一份相同）')
          .fontSize(this.fz(Ft.cap))
          .fontColor(WARN)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 2 })
      } else if (side.rows.length === 0) {
        Text('  （无：本轮没有任何一项被判为「不随算力变化」）')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 2 })
      }
      ForEach(side.rows, (r: CmpAuditRow, i: number) => {
        Column() {
          Text('· ' + r.name + '（id=' + r.id.toString() + ' · ' + r.stage + '口径）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(TEXT)
            .width('100%')
          Text('   判定依据：' + (r.hasReason ? r.reason : '报告里没有给出这一项的判定依据原文'))
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
            .lineHeight(15)
            .width('100%')
        }
        .width('100%')
        .margin({ top: 3 })
      }, (r: CmpAuditRow, i: number) => 'audr' + i.toString())
    }
    .width('100%')
  }

  // =========================================================================
  //  任务一(续): 复合分的参与项集合 + 只用共同参与项重算的比
  // =========================================================================
  @Builder
  cmpPartsBlock() {
    Column() {
      Text('复合分的参与项集合（同项性）')
        .fontSize(this.fz(Ft.body))
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .width('100%')
      Text(this.cmpResult().partsNote.length > 0 ? this.cmpResult().partsNote
        : '参与项不同的两个复合分不能直接相除；这一块给出交集与只用共同参与项重算的比。')
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      ForEach(this.cmpResult().parts, (s: CmpStageParts, i: number) => {
        Column() {
          Text('CS1 ' + s.stage + '：A 参与 ' + s.aCount.toString() + ' 项 / B 参与 ' +
            s.bCount.toString() + ' 项 / 共同 ' + s.commonCount.toString() + ' 项')
            .fontSize(this.fz(Ft.cap))
            .fontColor(TEXT)
            .width('100%')
            .margin({ top: 6 })
          Text('   复合分原始比 = ' + fmtRatio(s.rawScoreRatio, s.hasRawScoreRatio) +
            '（' + (s.identical === 1 ? '两边参与项一致，可用' :
              (s.identical === 0 ? '两边参与项不同 -> 不可用' : '参与项是否同项判不出来')) + '）')
            .fontSize(this.fz(Ft.cap))
            .fontColor(this.cmpIdenticalColor(s.identical))
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
          Text('   只用共同参与项重算：分比 = ' + fmtRatio(s.sameScoreRatio, s.hasSameScoreRatio) +
            ' · 每核比 = ' + fmtRatio(s.samePerCore, s.hasSamePerCore) +
            ' · 同频比 = ' + fmtRatio(s.sameSameFreq, s.hasSameSameFreq))
            .fontSize(this.fz(Ft.cap))
            .fontColor(GOOD)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
          if (s.onlyInA.length > 0) {
            Text('   只在 A 参与：' + s.onlyInA.join('、'))
              .fontSize(this.fz(Ft.cap))
              .fontColor(DANGER)
              .lineHeight(15)
              .width('100%')
              .margin({ top: 2 })
          }
          if (s.onlyInB.length > 0) {
            Text('   只在 B 参与：' + s.onlyInB.join('、'))
              .fontSize(this.fz(Ft.cap))
              .fontColor(DANGER)
              .lineHeight(15)
              .width('100%')
              .margin({ top: 2 })
          }
        }
        .width('100%')
      }, (s: CmpStageParts, i: number) => 'part' + i.toString())
      if (this.cmpResult().rawMultiUnusableReason.length > 0) {
        Text('原始多核复合分比：' + (this.cmpResult().rawMultiUsable === 1 ? '可用。' :
          (this.cmpResult().rawMultiUsable === 0 ? '不可用 ' : '判不出来：')) +
          this.cmpResult().rawMultiUnusableReason)
          .fontSize(this.fz(Ft.cap))
          .fontColor(this.cmpResult().rawMultiUsable === 0 ? DANGER : SUB)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 6 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE))
    .borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // =========================================================================
  //  任务二 + 任务三: 两台的频率证据与跑分条件差
  // =========================================================================
  @Builder
  cmpConditionBlock() {
    Column() {
      Text('两台的频率证据与跑分条件差')
        .fontSize(this.fz(Ft.body))
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .width('100%')
      Text('同频归一化靠的是「运行时频率中位」与「可用最高频档」这两个数；' +
        '多核比是否可比还要看线程数 / SMT / 可用核集合这三条条件。')
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      Text(this.cmpFreqLine('设备A', this.cmpResult().condA))
        .fontSize(this.fz(Ft.cap))
        .fontColor(TEXT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 6 })
      Text(this.cmpFreqLine('设备B', this.cmpResult().condB))
        .fontSize(this.fz(Ft.cap))
        .fontColor(TEXT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      Text(this.cmpCondLine('设备A', this.cmpResult().condA))
        .fontSize(this.fz(Ft.cap))
        .fontColor(TEXT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 6 })
      Text(this.cmpCondLine('设备B', this.cmpResult().condB))
        .fontSize(this.fz(Ft.cap))
        .fontColor(TEXT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      Text('线程数比（B/A）= ' + fmtRatio(this.cmpResult().condThreadsRatio,
        this.cmpResult().hasCondThreadsRatio) +
        ' · 条件是否相同：' + (this.cmpResult().condSame === 1 ? '相同' :
          (this.cmpResult().condSame === 0 ? '不同 -> 多核比不是纯芯片比' : '判不出来')))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.cmpCondColor())
        .lineHeight(15)
        .width('100%')
        .margin({ top: 6 })
      if (this.cmpResult().condDirection.length > 0) {
        Text('影响方向：' + this.cmpResult().condDirection)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
      }
      if (this.cmpResult().condPerCorePrecondition.length > 0) {
        Text('每核比的适用前提：' + this.cmpResult().condPerCorePrecondition)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 4 })
      }
      Text('逐项「同频归一化比」为 null 的原因：见逐项表里那一行的 why（' +
        '自研套件 / GPU / NPU / 存储 I/O 这几节本来就不做频率采样，它们的同频比只能是 null）。')
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 4 })
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE))
    .borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  @Builder
  cmpItemRow(r: CmpItemView, i: number) {
    Column() {
      if (this.cmpShowSection(i)) {
        Text(this.cmpSectionOf(i))
          .fontSize(this.fz(Ft.cap))
          .fontColor(ACCENT2)
          .backgroundColor(C.chipViolet)
          .borderRadius(Rd.sm)
          .padding({ left: 6, right: 6, top: 1, bottom: 1 })
          .margin({ top: 10 })
      }
      Row() {
        Text(r.name)
          .fontSize(this.fz(Ft.body))
          .fontColor(TEXT)
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(fmtNum(r.a.score, 1, r.a.hasScore))
          .fontSize(this.fz(Ft.body))
          .fontColor(SUB)
          .width(58)
          .textAlign(TextAlign.End)
          .maxLines(1)
        Text(fmtNum(r.b.score, 1, r.b.hasScore))
          .fontSize(this.fz(Ft.body))
          .fontColor(ACCENT)
          .width(58)
          .textAlign(TextAlign.End)
          .maxLines(1)
      }
      .width('100%')
      .margin({ top: 2 })
      Row() {
        Text(fmtRatio(r.ratio, r.hasRatio))
          .fontSize(this.fz(Ft.cap))
          .fontColor(r.hasRatio ? WARN : FAINT)
          .width(76)
          .maxLines(1)
        Text(fmtRatio(r.perCore, r.hasPerCore))
          .fontSize(this.fz(Ft.cap))
          .fontColor(r.hasPerCore ? GOOD : FAINT)
          .width(62)
          .maxLines(1)
        Text(fmtRatio(r.sameFreq, r.hasSameFreq))
          .fontSize(this.fz(Ft.cap))
          .fontColor(r.hasSameFreq ? GOOD : FAINT)
          .width(62)
          .maxLines(1)
        Text(this.cmpDevText(r))
          .fontSize(this.fz(Ft.cap))
          .fontColor(this.cmpDevColor(r))
          .width(52)
          .maxLines(1)
        Text(within5Text(r.within5))
          .fontSize(this.fz(Ft.cap))
          .fontColor(this.cmpDevColor(r))
          .width(34)
          .textAlign(TextAlign.End)
          .maxLines(1)
      }
      .width('100%')
      .margin({ top: 2 })
      if (r.onlyIn.length > 0) {
        Text(r.onlyIn)
          .fontSize(this.fz(Ft.cap))
          .fontColor(WARN)
          .width('100%')
          .margin({ top: 2 })
      }
      if (!r.hasPerCore || !r.hasSameFreq) {
        Text('缺输入：' + (r.why.length > 0 ? r.why : 'native 未说明'))
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 2 })
      }
    }
    .width('100%')
    .padding({ top: 4, bottom: 4 })
    .border(edgeBottom(DIVIDER))
  }
  // 缺输入横幅(ok 为真也可能有缺项: 某些项的频率/核数没上报 -> 那一项的归一化比是 null)
  @Builder
  cmpMissingBanner() {
    Column() {
      Text('这份对比里缺了 ' + this.cmpResult().missing.length.toString() + ' 样输入（下面逐条原样列出，界面不替它编数）')
        .fontSize(this.fz(Ft.body))
        .fontColor(WARN)
        .width('100%')
      ForEach(this.cmpResult().missing, (m: string, i: number) => {
        Text('· ' + m)
          .fontSize(this.fz(Ft.cap))
          .fontColor(WARN)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 2 })
      }, (m: string, i: number) => 'cmpmb' + i.toString())
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(C.chipWarn)
    .borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // 真值锚点表: 实测比 vs 公开真值比(只对两端都匹配上真值的锚点算得出)
  @Builder
  cmpAnchorBlock() {
    Column() {
      Text('真值锚点表（实测比 vs 公开真值比，用来校验「分数比能不能定量算进步」）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      Text('判定阈值 ' + this.cmpResult().anchorThreshold.toFixed(0) + '%：' +
        (this.cmpResult().anchorThresholdNote.length > 0 ? this.cmpResult().anchorThresholdNote
          : '该阈值是我们自己定的，不是官方阈值'))
        .fontSize(this.fz(Ft.cap))
        .fontColor(FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      ForEach(this.cmpResult().anchors, (t: CmpAnchorView, i: number) => {
        Column() {
          Text(t.label)
            .fontSize(this.fz(Ft.body))
            .fontColor(TEXT)
            .width('100%')
          Text(t.from + ' → ' + t.to + (t.unit.length > 0 ? ('（' + t.unit + '）') : ''))
            .fontSize(this.fz(Ft.cap))
            .fontColor(SUB)
            .width('100%')
            .margin({ top: 2 })
          Text('真值比(B/A) ' + fmtNum(t.truthRatio, 4, t.hasTruthRatio) +
            ' · 实测比(B/A) ' + fmtNum(t.measuredRatio, 4, t.hasMeasured) +
            ' · 偏差 ' + (t.hasDev ? ((t.devPct >= 0 ? '+' : '') + t.devPct.toFixed(1) + '%') : '—') +
            ' · 5% 内 ' + within5Text(t.within5))
            .fontSize(this.fz(Ft.cap))
            .fontColor(t.hasDev ? (t.within5 === 1 ? GOOD : DANGER) : FAINT)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
          if (t.why.length > 0) {
            Text(t.why)
              .fontSize(this.fz(Ft.cap))
              .fontColor(FAINT)
              .lineHeight(15)
              .width('100%')
              .margin({ top: 2 })
          }
          if (t.sourceKind.length > 0) {
            Text('来源种类 ' + t.sourceKind + (t.source.length > 0 ? (' · ' + t.source) : ''))
              .fontSize(this.fz(Ft.cap))
              .fontColor(FAINT)
              .lineHeight(15)
              .width('100%')
              .margin({ top: 2 })
          }
        }
        .width('100%')
        .padding({ top: 6, bottom: 6 })
        .border(edgeBottom(DIVIDER))
      }, (t: CmpAnchorView, i: number) => 'cmpanc' + i.toString())
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // 怎么用 / 什么时候不可比 / 提醒(native 原话, 一条不改)
  @Builder
  cmpNotesBlock() {
    Column() {
      Text('怎么用（native 给的四步）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      ForEach(this.cmpResult().howToUse, (s: string, i: number) => {
        Text(s)
          .fontSize(this.fz(Ft.cap))
          .fontColor(TEXT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 2 })
      }, (s: string, i: number) => 'cmphow' + i.toString())
      if (this.cmpResult().notComparable.length > 0) {
        Text('什么时候这份对比不成立')
          .fontSize(this.fz(Ft.small))
          .fontColor(DANGER)
          .width('100%')
          .margin({ top: 10 })
        ForEach(this.cmpResult().notComparable, (s: string, i: number) => {
          Text('· ' + s)
            .fontSize(this.fz(Ft.cap))
            .fontColor(DANGER)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
        }, (s: string, i: number) => 'cmpnc' + i.toString())
      }
      if (this.cmpResult().caveats.length > 0) {
        Text('口径提醒')
          .fontSize(this.fz(Ft.small))
          .fontColor(SUB)
          .width('100%')
          .margin({ top: 10 })
        ForEach(this.cmpResult().caveats, (s: string, i: number) => {
          Text('· ' + s)
            .fontSize(this.fz(Ft.cap))
            .fontColor(FAINT)
            .lineHeight(15)
            .width('100%')
            .margin({ top: 2 })
        }, (s: string, i: number) => 'cmpcv' + i.toString())
      }
      if (this.cmpResult().truthRule.length > 0) {
        Text(this.cmpResult().truthRule)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 8 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 汇报层缺陷 2 修)
  // 这一段回答两个此前对比页完全看不到的问题:
  //   ① 这两台设备的限制分别在哪一层(应用自己的策略 / cpuset cgroup / 内核 core_ctl)?
  //   ② 本次跑分到底有没有把 QoS 生效(库能不能加载 / canIUse 怎么说 / 设到哪一档 / 每个调用返回什么)?
  // 两段都是各自报告 items[].qos / items[].cpuset 的原文(native output.environmentEvidence 原样搬运),
  // 界面一个字都不加工; present=false 时显示 native 给的原因原话, 不显示成"空的 = 正常"。
  @Builder
  cmpEnvEvidenceBlock() {
    Column() {
      Text('QoS 与限制层级（两台的原文，取自各自报告的 items[]）')
        .fontSize(this.fz(Ft.small))
        .fontColor(SUB)
        .width('100%')
      if (this.cmpResult().envWhatItAnswers.length > 0) {
        Text(this.cmpResult().envWhatItAnswers)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 2 })
      }
      // ---- 设备A ----
      Text('设备A：' + (this.cmpResult().deviceA.length > 0 ? this.cmpResult().deviceA : '（未标注）'))
        .fontSize(this.fz(Ft.small))
        .fontColor(TEXT)
        .width('100%')
        .margin({ top: 8 })
      Text('QoS：' + (this.cmpResult().envQosPresentA
        ? (this.cmpResult().envQosTextA.length > 0 ? this.cmpResult().envQosTextA : '（原文为空串）')
        : ('未上报 —— ' + this.cmpResult().envQosNullA)))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.cmpResult().envQosPresentA ? TEXT : FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      Text('限制层级（cgroup / cpuset / core_ctl）：' + (this.cmpResult().envCpusetPresentA
        ? (this.cmpResult().envCpusetTextA.length > 0 ? this.cmpResult().envCpusetTextA : '（原文为空串）')
        : ('未上报 —— ' + this.cmpResult().envCpusetNullA)))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.cmpResult().envCpusetPresentA ? TEXT : FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      if (this.cmpResult().envQosPresentA && this.cmpResult().envQosFromA.length > 0) {
        Text('（QoS 全文取自 ' + this.cmpResult().envQosFromA + '）')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 2 })
      }
      // ---- 设备B ----
      Text('设备B：' + (this.cmpResult().deviceB.length > 0 ? this.cmpResult().deviceB : '（未标注）'))
        .fontSize(this.fz(Ft.small))
        .fontColor(TEXT)
        .width('100%')
        .margin({ top: 8 })
      Text('QoS：' + (this.cmpResult().envQosPresentB
        ? (this.cmpResult().envQosTextB.length > 0 ? this.cmpResult().envQosTextB : '（原文为空串）')
        : ('未上报 —— ' + this.cmpResult().envQosNullB)))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.cmpResult().envQosPresentB ? TEXT : FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      Text('限制层级（cgroup / cpuset / core_ctl）：' + (this.cmpResult().envCpusetPresentB
        ? (this.cmpResult().envCpusetTextB.length > 0 ? this.cmpResult().envCpusetTextB : '（原文为空串）')
        : ('未上报 —— ' + this.cmpResult().envCpusetNullB)))
        .fontSize(this.fz(Ft.cap))
        .fontColor(this.cmpResult().envCpusetPresentB ? TEXT : FAINT)
        .lineHeight(15)
        .width('100%')
        .margin({ top: 2 })
      if (this.cmpResult().envCpusetPresentB && this.cmpResult().envCpusetFromB.length > 0) {
        Text('（cpuset 全文取自 ' + this.cmpResult().envCpusetFromB + '）')
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .width('100%')
          .margin({ top: 2 })
      }
      if (this.cmpResult().envNote.length > 0) {
        Text(this.cmpResult().envNote)
          .fontSize(this.fz(Ft.cap))
          .fontColor(FAINT)
          .lineHeight(15)
          .width('100%')
          .margin({ top: 8 })
      }
    }
    .width('100%')
    .padding(this.cardPad())
    .backgroundColor(CARD)
    .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
    .margin({ top: 8 })
  }

  // 对比结果浮层(与「一键全部跑分」结果浮层同一套材质 / 安全区口径; 只在用户操作后出现)
  @Builder
  cmpOverlay() {
    Column() {
      Row() {
        Text('两份报告对比')
          .fontSize(this.fz(Ft.title))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
        Blank()
        Text('只读 · 纯计算')
          .fontSize(this.fz(Ft.cap))
          .fontColor(C.onAccent)
          .backgroundColor(GOOD)
          .borderRadius(Rd.pill)
          .padding({ left: 9, right: 9, top: 3, bottom: 3 })
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .alignItems(VerticalAlign.Center)
      .margin({ top: 14 })
      Text('显示的是【设备B / 设备A】。不跑负载、不读 /sys、不写文件、不改任何分数；' +
        '缺输入一律原样显示，不编数字。')
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .width('92%')
        .constraintSize({ maxWidth: this.contentMax() })
        .margin({ top: 4 })
      Scroll() {
        Column() {
          this.cmpPickBlock();
          if (this.cmpStatus.length > 0) {
            Text(this.cmpStatus)
              .fontSize(this.fz(Ft.cap))
              .fontColor(this.cmpCopied ? GOOD : SUB)
              .lineHeight(15)
              .width('100%')
              .margin({ top: 8 })
          }
          if (this.cmpOut !== null) {
            if (!this.cmpResult().ok) {
              this.cmpFailBlock();
            } else {
              if (this.cmpResult().missing.length > 0) {
                this.cmpMissingBanner();
              }
              this.cmpSummaryBlock();
              this.cmpSelfCheckBlock();
              this.cmpPartsBlock();
              this.cmpConditionBlock();
              this.cmpInflationBlock();
              this.cmpItemTable();
              this.cmpAnchorBlock();
              this.cmpEnvEvidenceBlock();
              this.cmpNotesBlock();
            }
          }
        }
        .width('92%')
        .constraintSize({ maxWidth: this.contentMax() })
        .margin({ bottom: 12 })
      }
      .width('100%')
      .layoutWeight(1)
      .margin({ top: 6 })
      .scrollBar(BarState.Off)
      Row() {
        Button(this.cmpCopied ? '已复制对比 JSON' : '复制对比 JSON')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(Ft.body))
          .backgroundColor(C.chip)
          .fontColor(GOOD)
          .layoutWeight(1)
          .onClick(() => {
            this.cmpCopyJson();
          })
        Button('关闭')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(Ft.body))
          .backgroundColor(C.chip)
          .fontColor(SUB)
          .layoutWeight(1)
          .margin({ left: 8 })
          .onClick(() => {
            this.cmpOpen = false;
          })
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .margin({ top: 6, bottom: 14 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.materialColor())
    // 磨砂浮层上的品牌底: 用半透明版本, 免得把玻璃感盖死(见 appBackdropGradSoft)
    .linearGradient(this.appBackdropGradSoft())
    .backgroundBlurStyle(BlurStyle.Thick, {
      colorMode: ThemeColorMode.DARK,
      policy: this.blurPolicy(),
      inactiveColor: this.materialColor()
    })
    .transition(this.fxTransition())
    .padding({ top: this.padTop(), bottom: this.padBottom() })
  }
  // =========================================================================
  // 跑分中的进度页(性能护栏: 静态文字 + 确定值进度条, 零动画; 每项才刷新一次)
  // =========================================================================
  @Builder
  fullRunOverlay() {
    Column() {
      Text('一键全部跑分')
        .fontSize(this.fz(13))
        .fontColor(ACCENT)
        .margin({ top: 36 })
      Text('第 ' + this.fullSectionIdx.toString() + '/' + this.fullSectionCount.toString() +
        ' 节 · ' + this.fullSectionName)
        .fontSize(this.fz(14))
        .fontColor(TEXT)
        .margin({ top: 10 })
      Text(this.fullItemTotal > 0
        ? ('本节第 ' + this.fullItemIdx.toString() + '/' + this.fullItemTotal.toString() + ' 项')
        : '正在准备本节的项…')
        .fontSize(this.fz(11))
        .fontColor(SUB)
        .margin({ top: 4 })
      Text(this.fullItemName)
        .fontSize(this.bp === 'sm' ? 24 : 28)
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .margin({ top: 16 })
      // 确定值进度条: 两个静态色块叠出来(不用 Progress 组件 —— 它自带平滑动效)
      Stack({ alignContent: Alignment.Start }) {
        Text('')
          .width('100%')
          .height(8)
          .backgroundColor(TRACK)
          .borderRadius(4)
        Text('')
          .width(this.fullProgressPercent())
          .height(8)
          .backgroundColor(ACCENT)
          .borderRadius(4)
      }
      .width('86%')
      .constraintSize({ maxWidth: this.contentMax() })
      .margin({ top: 20 })
      Text('已完成 ' + this.fullDoneItems.toString() + ' / ' +
        (this.fullTotalItems > 0 ? this.fullTotalItems.toString() : '?') + ' 项 · 已完成小节 ' +
        this.fullDoneSections.toString() + '/' + this.fullSectionCount.toString())
        .fontSize(this.fz(12))
        .fontColor(SUB)
        .margin({ top: 8 })
      Text('热状态：' + this.thermalText() + (this.thermalLevel >= 3 ? '（系统可能已限频）' : '') +
        ' · 请保持前台、勿切后台')
        .fontSize(this.fz(11))
        .fontColor(this.thermalLevel >= 3 ? DANGER : SUB)
        .margin({ top: 6 })
      Button(this.fullStopArmed ? '已请求停止：当前项跑完即收工并出报告' : '停止（当前项跑完后收工并出报告）')
        .constraintSize({ minHeight: this.btnH() })
        .fontSize(this.fz(12))
        .backgroundColor(C.chipWarn)
        .fontColor(WARN)
        .margin({ top: 12 })
        .onClick(() => {
          this.stopFullRun();
        })
      if (this.fullSectionLines.length > 0) {
        List({ space: 4 }) {
          ForEach(this.fullSectionLines, (s: string, i: number) => {
            ListItem() {
              Text(s)
                .fontSize(this.fz(10))
                .fontColor(GOOD)
                .width('100%')
            }
          }, (s: string, i: number) => 'sec' + i.toString())
        }
        .width('92%')
        .constraintSize({ maxWidth: this.contentMax() })
        .height(72)
        .margin({ top: 12 })
      }
      List({ space: 4 }) {
        ForEach(this.fullLines, (s: string, i: number) => {
          ListItem() {
            Text(s)
              .fontSize(this.fz(11))
              .fontColor(SUB)
              .width('100%')
          }
        }, (s: string, i: number) => 'itm' + i.toString())
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .layoutWeight(1)
      .margin({ top: 10, bottom: 12 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(BG)
    // App 自己的画布: 暗红 -> 深紫 -> 回落到 BG 的静态渐变(见 appBackdropGrad 的说明)。
    //  它画在 backgroundColor 之上、内容之下, 所以不占布局、不改任何宽高。
    .linearGradient(this.appBackdropGrad())
    // 浮层与主界面同一套安全区口径(背景铺满, 内容让开状态栏 / 导航条)
    .padding({ top: this.padTop(), bottom: this.padBottom() })
  }

  // =========================================================================
  // 报告页: 预览 + 复制全文 + 导出到文件
  // =========================================================================
  // 结果页(原「报告页」) —— 重做成三层:
  //   第一层: 单核 / 多核 / GPU 三个大数字(主色焦点, 一眼可见)
  //   第二层: 每个小节的关键得分一行(统一列表 + 单位 + 取得条件)
  //   第三层: 逐项明细(metric / unit / 得分 / 耗时 / 并行度 / 运行时频率 / 落核 / diag), 默认收起
  //   落盘的完整报告文件内容一字未改, 界面上只是分层呈现。
  @Builder
  fullReportOverlay() {
    Column() {
      Row() {
        Text('一键全部跑分 · 结果')
          .fontSize(this.fz(15))
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT)
        Blank()
        Text(this.fullInterruptedFlag() ? '未跑完' : '已跑完')
          .fontSize(this.fz(Ft.cap))
          .fontColor(this.fullInterruptedFlag() ? C.onWarn : ON_ACCENT)
          .backgroundColor(this.fullInterruptedFlag() ? WARN : GOOD)
          .borderRadius(Rd.pill)
          .padding({ left: 9, right: 9, top: 3, bottom: 3 })
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .alignItems(VerticalAlign.Center)
      .margin({ top: 14 })
      Text(this.fullOutcomeSectionsText() + ' · 项数 ' + this.fullOutcomeItems().toString() +
        ' · 耗时 ' + this.fullOutcomeElapsedText() + ' · 报告 ' +
        this.fullReportChars.toString() + ' 字符')
        .fontSize(this.fz(Ft.cap))
        .fontColor(SUB)
        .width('92%')
        .constraintSize({ maxWidth: this.contentMax() })
        .margin({ top: 4 })

      Scroll() {
        Column() {
          // 首屏「一眼可见」 与「单次跑分结果页」同一套排法(this.uiRes):
          //  宽窗口左右并排(左: 三个大数字 / 右: 关键得分), 窄窗口单列但间距压到最小。
          //  两处共用同一个 LayoutPlan 密度, 所以在同一台机器上它们的间距与字号永远一致。
          GridRow({
            columns: 12,
            gutter: { x: this.uiRes.side ? this.uiRes.gap : 0, y: this.uiRes.gap }
          }) {
            GridCol({ span: this.uiRes.side ? this.uiRes.leftSpan : 12 }) {
              Column() {
                // ============ 第一层: 三个大数字 ============
                Flex({
                  direction: FlexDirection.Row,
                  wrap: FlexWrap.Wrap,
                  justifyContent: FlexAlign.Center,
                  // 只按自己的固定高度排, 交叉轴(竖直)不拉伸(真机缺陷的第二道保险)
                  alignItems: ItemAlign.Start
                }) {
                  ForEach(this.fullL1, (c: ScoreCard) => {
                    Column() {
                      this.bigScoreCard(c)
                    }
                    // 固定高度(结果页密度算好): 纵向不参与剩余高度分配
                    .height(this.uiRes.l1CardH)
                    .flexGrow(1)
                    .flexBasis(this.l1Basis())
                    .constraintSize({ minWidth: this.l1MinW() })
                  }, (c: ScoreCard) => 'l1' + c.key)
                }
                .width('100%')
                .padding({ top: this.uiRes.pad, bottom: this.uiRes.pad })
                .backgroundColor(CARD_HI)
                .border({ width: BORDER_THICK, color: ACCENT })
                .borderRadius(this.cardRadius())
                if (this.fullL1.length === 0) {
                  Text('本进程里还没有跑过「一键全部跑分」：三个大数字要跑完一次才出得来。' +
                    '（盘上那份 report-latest.txt 仍可导出 / 复制，但界面上的分层数字需要重跑一次。）')
                    .fontSize(this.uiRes.keyUnit)
                    .fontColor(WARN)
                    .width('100%')
                    .margin({ top: this.uiRes.gap })
                }
              }
              .width('100%')
            }
            GridCol({ span: this.uiRes.side ? this.uiRes.rightSpan : 12 }) {
              // ============ 第二层: 每个小节的关键得分一行 ============
              Column() {
                Text('关键得分（每个分都带单位与取得条件）')
                  .fontSize(this.fz(Ft.small))
                  .fontColor(SUB)
                  .width('100%')
                ForEach(this.fullL2, (r: KeyRow) => {
                  this.keyRowItem(r)
                }, (r: KeyRow, i: number) => 'l2' + i.toString())
                Text('取得条件来源：' + (this.fullCondText.length > 0
                  ? this.fullCondText : '（native 未上报允许核集合）'))
                  .fontSize(this.uiRes.keyUnit)
                  .fontColor(FAINT)
                  .width('100%')
                  .lineHeight(Math.round(this.uiRes.keyUnit * 1.3))
                  .margin({ top: 4 })
              }
              .width('100%')
              .padding(this.uiRes.pad)
              .backgroundColor(CARD)
              .border(edgeTop(CARD_EDGE))
              .borderRadius(this.cardRadius())
            }
          }
          .width('100%')

          // ============ 第三层: 逐项明细(默认收起) ============
          Row() {
            Text(this.fullDetailOpen
              ? ('▾ 逐项明细（' + this.detailTotal().toString() + ' 项）· 点一下收起')
              : ('▸ 逐项明细（' + this.detailTotal().toString() +
                ' 项）· 默认收起；需要时点开看 metric / 单位 / 耗时 / 并行度 / 运行时频率 / 落核 / diag'))
              .fontSize(this.fz(Ft.small))
              .fontColor(ACCENT)
              .layoutWeight(1)
          }
          .width('100%')
          .padding(this.cardPad())
          .backgroundColor(CARD)
          .border(edgeTop(CARD_EDGE))
          .borderRadius(this.cardRadius())
          .margin({ top: this.rowGap() })
          .onClick(() => {
            this.fullDetailOpen = !this.fullDetailOpen;
          })
          if (this.fullDetailOpen) {
            Column() {
              // 大屏并排更多列 第三层逐项明细在宽窗口上排成 2~3 列(而不是把小屏的单列
              //  原样拉宽): 列数与每列最小宽同样由结果页密度按实测窗口宽给出, 放不下会自动
              //  退成更少列。窄窗口仍然是单列(一列 100% 满宽)。
              Flex({ direction: FlexDirection.Row, wrap: FlexWrap.Wrap,
                justifyContent: FlexAlign.Start }) {
                ForEach(this.fullL3, (g: DetailGroup, gi: number) => {
                  Column() {
                    this.detailGroupBlock(g)
                  }
                  .flexGrow(1)
                  .flexBasis(this.detailBasis())
                  .constraintSize({ minWidth: this.detailMinW() })
                  .margin({ top: this.uiRes.gap })
                }, (g: DetailGroup, gi: number) => 'l3' + gi.toString())
              }
              .width('100%')
              Text('原始报告全文（预览，最多 ' + FULL_REPORT_PREVIEW.toString() +
                ' 字符；完整内容见下面那个文件，也可以直接「复制全文」/「导出到文件」）')
                .fontSize(this.fz(Ft.cap))
                .fontColor(SUB)
                .width('100%')
                .margin({ top: 12 })
              Text(this.fullReportPreview.length > 0
                ? this.fullReportPreview
                : '（预览为空：先点「一键全部跑分」，或用首页卡片上的「查看最近报告」）')
                .fontSize(this.fz(Ft.cap))
                .fontColor(FAINT)
                .width('100%')
                .textAlign(TextAlign.Start)
                .margin({ top: 4 })
                .padding(Sp.x2)
                .backgroundColor(INSET)
                .borderRadius(Rd.sm)
            }
            .width('100%')
            .padding(this.cardPad())
            .backgroundColor(CARD)
            .border(edgeTop(CARD_EDGE))
            .borderRadius(this.cardRadius())
            .margin({ top: 6 })
          }

          // ============ 报告文件与「下载」目录 ============
          this.reportFileCard()

          if (this.fullReportStatus.length > 0) {
            Text(this.fullReportStatus)
              .fontSize(this.fz(Ft.cap))
              .fontColor(this.fullReportCopied ? GOOD : SUB)
              .width('100%')
              .margin({ top: 8, bottom: 12 })
          }
        }
        .width('92%')
        .constraintSize({ maxWidth: this.contentMax() })
      }
      .width('100%')
      .layoutWeight(1)
      .margin({ top: 6 })
      .scrollBar(BarState.Off)

      // 底部固定按钮条(始终可见, 不随内容滚走)
      Row() {
        Button('复制全文')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(ACCENT)
          .layoutWeight(1)
          .onClick(() => {
            this.copyFullReport();
          })
        Button('导出到文件')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(GOOD)
          .layoutWeight(1)
          .margin({ left: 8 })
          .onClick(() => {
            this.exportFullReport();
          })
        Button('关闭')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(CHIP)
          .fontColor(SUB)
          .layoutWeight(1)
          .margin({ left: 8 })
          .onClick(() => {
            this.closeFullReport();
          })
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .margin({ top: 8, bottom: 12 })
    }
    .width('100%')
    .height('100%')
    // 官方磨砂材质(backgroundBlurStyle, 深色模式): 只在跑分结束后才出现的浮层上使用。
    // 跑分期间 live 期不存在这个浮层; 万一存在, blurPolicy() 会切到 ALWAYS_INACTIVE,
    // 模糊层不参与合成, 底色退回不透明的 BG(与历史观感完全一致)。
    .backgroundColor(this.materialColor())
    // 磨砂浮层上的品牌底: 用半透明版本, 免得把玻璃感盖死(见 appBackdropGradSoft)
    .linearGradient(this.appBackdropGradSoft())
    .backgroundBlurStyle(BlurStyle.Thick, {
      colorMode: ThemeColorMode.DARK,
      policy: this.blurPolicy(),
      inactiveColor: this.materialColor()
    })
    // 一次性转场(用户操作: 打开/关闭报告, 或跑完自动弹出)
    .transition(this.fxTransition())
    // 结果页与主界面同一套安全区口径
    .padding({ top: this.padTop(), bottom: this.padBottom() })
  }

  @Builder
  runningOverlay() {
    Column() {
      Text(this.phase)
        .fontSize(this.fz(13))
        .fontColor(ACCENT)
        .margin({ top: 40 })
      Text('热状态：' + this.thermalText() + (this.thermalLevel >= 3 ? '（系统可能已限频）' : ''))
        .fontSize(this.fz(11))
        .fontColor(this.thermalLevel >= 3 ? DANGER : SUB)
      if (this.scRunning) {
        Text('稳定性压测 ' + this.scDone.toString() + ' / ' + this.scTarget.toString() +
          ' 轮已完成 · 上一轮失败项 ' + this.scLastRoundFails.toString() + ' · 崩溃 ' +
          this.scCrashRuns.toString() + ' 次')
          .fontSize(this.fz(11))
          .fontColor(WARN)
          .margin({ top: 6 })
        Button('停止压测（跑完当前这一轮后收工并出报告）')
          .constraintSize({ minHeight: this.btnH() })
          .fontSize(this.fz(12))
          .backgroundColor(C.chipWarn)
          .fontColor(WARN)
          .margin({ top: 8 })
          .onClick(() => {
            this.stopStress();
          })
      }
      Text(this.doneCount.toString() + ' / ' + this.totalCount.toString())
        .fontSize(this.fz(12))
        .fontColor(SUB)
        .margin({ top: 6 })
      Text(this.curTest)
        .fontSize(this.bp === 'sm' ? 26 : 30)
        .fontWeight(FontWeight.Bold)
        .fontColor(TEXT)
        .margin({ top: 26 })
      Row() {
        Column() {
          Text(this.curMs.toFixed(0) + ' ms')
            .fontSize(this.fz(22))
            .fontColor(WARN)
          Text('本次耗时')
            .fontSize(this.fz(11))
            .fontColor(SUB)
            .margin({ top: 4 })
        }
        .layoutWeight(1)
        Column() {
          Text(this.curScore.toFixed(0))
            .fontSize(this.fz(22))
            .fontColor(GOOD)
          Text('本项得分')
            .fontSize(this.fz(11))
            .fontColor(SUB)
            .margin({ top: 4 })
        }
        .layoutWeight(1)
      }
      .width('80%')
      .constraintSize({ maxWidth: this.contentMax() })
      .margin({ top: 30 })
      Progress({
        value: this.totalCount > 0 ? this.doneCount : 0,
        total: this.totalCount > 0 ? this.totalCount : 1,
        type: ProgressType.Linear
      })
        .width('80%')
        .constraintSize({ maxWidth: this.contentMax() })
        .color(accentGradient())
        .backgroundColor(TRACK)
        .margin({ top: 24 })

      if (this.gpuActive) {
        Text('GPU 场景 ' + this.gpuSceneIdx.toString() + '/4 · ' + this.gpuFps.toFixed(1) + ' FPS')
          .fontSize(this.fz(13))
          .fontColor(ACCENT2)
          .margin({ top: 8 })
      }
      List({ space: 6 }) {
        ForEach(this.live, (t: TestResult, idx: number) => {
          ListItem() {
            Row() {
              Text((idx + 1).toString().padStart(2, '0'))
                .fontSize(this.fz(11))
                .fontColor(FAINT)
                .width(26)
              Text(t.name)
                .fontSize(this.fz(13))
                .fontColor(TEXT)
                .layoutWeight(1)
              Text(t.ms.toFixed(0) + 'ms')
                .fontSize(this.fz(11))
                .fontColor(SUB)
                .width(70)
                .textAlign(TextAlign.End)
              Text(t.score.toFixed(0))
                .fontSize(this.fz(13))
                .fontColor(GOOD)
                .width(60)
                .textAlign(TextAlign.End)
            }
            .width('100%')
            .padding({ left: 12, right: 12, top: 8, bottom: 8 })
            .backgroundColor(CARD)
            .borderRadius(10)
          }
        }, (t: TestResult, idx: number) => idx.toString())
      }
      .width('92%')
      .constraintSize({ maxWidth: this.contentMax() })
      .layoutWeight(1)
      .margin({ top: 18, bottom: 12 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(BG)
    // App 自己的画布: 暗红 -> 深紫 -> 回落到 BG 的静态渐变(见 appBackdropGrad 的说明)。
    //  它画在 backgroundColor 之上、内容之下, 所以不占布局、不改任何宽高。
    .linearGradient(this.appBackdropGrad())
    // 跑分进度浮层与主界面同一套安全区口径
    .padding({ top: this.padTop(), bottom: this.padBottom() })
  }

  @Builder
  resultOverlay() {
    Scroll() {
      Column() {
        Column() {
          if (this.shown !== null) {
            // =====================================================================
            // 首屏「一眼可见」(2026-10)
            //  改前的问题: "总分" 上面留 30vp、"三个大数字" 上面留 20vp、"关键得分" 上面
            //  留 12vp、每行关键得分上下各 7vp + 条件另起一行 —— 十行下来光空白就 200vp 以上,
            //  在手机上必须滚动才看得到结论。
            //  改后: 只有一个间距来源 this.uiRes.gap / pad / rowPy(由实测窗口宽高算出),
            //  并且宽窗口(内容列 >= 820vp)直接左右并排 —— 左(总分 + 三个大数字) | 右(关键得分),
            //  两块同屏, 连滚都不用滚。窄窗口仍然单列, 但间距压到 4~6vp、每行只留 2~4vp。
            //   一个数字都没有少: 总分 / 三个大数字 / 十个关键得分行 / 口径与来源脚注全在首屏。
            // =====================================================================
            GridRow({
              columns: 12,
              gutter: { x: this.uiRes.side ? this.uiRes.gap : 0, y: this.uiRes.gap }
            }) {
              GridCol({ span: this.uiRes.side ? this.uiRes.leftSpan : 12 }) {
                Column() {
                  Text('总分')
                    .fontSize(this.fz(Ft.small))
                    .fontColor(SUB)
                  Text((this.shown as BenchRecord).total.toString())
                    .fontSize(this.uiRes.giant)
                    .fontWeight(FontWeight.Bold)
                    .maxLines(1)
                    .fontColor(BenchRunner.tierColor((this.shown as BenchRecord).total))
                  Text('相对参考基线 ' + ((this.shown as BenchRecord).total / 1000).toFixed(2) + ' 倍')
                    .fontSize(this.fz(Ft.sub))
                    .fontColor(TEXT)
                    .backgroundColor(CARD_HI)
                    .borderRadius(Rd.sm)
                    .padding({ left: 10, right: 10, top: 3, bottom: 3 })
                    .margin({ top: 4 })
                  Text((this.shown as BenchRecord).device + ' · ' + fmtTime((this.shown as BenchRecord).time))
                    .fontSize(this.fz(Ft.cap))
                    .fontColor(FAINT)
                    .maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                    .margin({ top: this.uiRes.gap })
                  // ---- 「本机性能天花板」一行(2026-10 追加; 与首页 / 报告同一句话) ----
                  //   放在三个大数字正上方: 读分数的第一眼就该知道这台机器被什么卡住。
                  Text(this.ceilingShow())
                    .fontSize(this.fz(Ft.cap))
                    .fontColor(this.ceilingShow().length > 0 ? ACCENT : FAINT)
                    .width('100%')
                    .lineHeight(Math.round(this.fz(Ft.cap) * 1.35))
                    .margin({ top: this.uiRes.gap })

                  // ============ 第一层: 单核 / 多核 / GPU 三个大数字 ============
                  Flex({
                    direction: FlexDirection.Row,
                    wrap: FlexWrap.Wrap,
                    justifyContent: FlexAlign.Center,
                    // 只按自己的固定高度排, 交叉轴(竖直)不拉伸(真机缺陷的第二道保险)
                    alignItems: ItemAlign.Start
                  }) {
                    ForEach(this.resL1, (c: ScoreCard) => {
                      Column() {
                        this.bigScoreCard(c)
                      }
                      // 固定高度(结果页密度算好): 纵向不参与剩余高度分配
                      .height(this.uiRes.l1CardH)
                      .flexGrow(1)
                      .flexBasis(this.l1Basis())
                      .constraintSize({ minWidth: this.l1MinW() })
                    }, (c: ScoreCard) => 'r1' + c.key)
                  }
                  .width('100%')
                  .padding({ top: this.uiRes.pad, bottom: this.uiRes.pad })
                  .backgroundColor(CARD_HI)
                  .border({ width: BORDER_THICK, color: ACCENT })
                  .borderRadius(this.cardRadius())
                  .shadow(this.fxAura(withAlpha(ACCENT, 0.24), 12))
                  // 沉浸光感(真家伙): 结果页三个大数字那一块的点光
                  .visualEffect(this.fxLight(ACCENT, 0.80, 0.22))
                  .margin({ top: this.uiRes.gap })
                }
                .width('100%')
                .alignItems(HorizontalAlign.Center)
              }
              GridCol({ span: this.uiRes.side ? this.uiRes.rightSpan : 12 }) {
                // ============ 第二层: 每个小节的关键得分一行 ============
                Column() {
                  Text('关键得分（每个分都带单位与取得条件）')
                    .fontSize(this.fz(Ft.small))
                    .fontColor(SUB)
                    .width('100%')
                  ForEach(this.resL2, (r: KeyRow) => {
                    this.keyRowItem(r)
                  }, (r: KeyRow, i: number) => 'r2' + i.toString())
                  // 口径与来源合成一行脚注(改前是两行 + 两个间距, 属于重复的说明行)
                  Text('子项分口径：整数 65% + 浮点 35%，子项内取几何平均，已并入上面各行 · ' +
                    '取得条件来源：' + (this.resCondText.length > 0
                      ? this.resCondText : '（native 未上报允许核集合）'))
                    .fontSize(this.uiRes.keyUnit)
                    .fontColor(FAINT)
                    .width('100%')
                    .lineHeight(Math.round(this.uiRes.keyUnit * 1.3))
                    .margin({ top: 4 })
                }
                .width('100%')
                .padding(this.uiRes.pad)
                .backgroundColor(CARD)
                .border(edgeTop(CARD_EDGE)).borderRadius(this.cardRadius())
              }
            }
            .width('100%')
            .margin({ top: this.uiRes.pad })
            Column() {
              Text('测试明细（单核阶段）')
                .fontSize(this.fz(12))
                .fontColor(SUB)
                .width('100%')
                .margin({ bottom: this.uiRes.pad })
              Flex({ direction: FlexDirection.Row, wrap: FlexWrap.Wrap,
                justifyContent: FlexAlign.Start }) {
              ForEach((this.shown as BenchRecord).tests, (t: TestResult) => {
                Column() {
                  Row() {
                    Text(t.name)
                      .fontSize(this.fz(14))
                      .fontColor(TEXT)
                      .layoutWeight(1)
                    Text(t.score.toFixed(0))
                      .fontSize(this.fz(15))
                      .fontColor(GOOD)
                  }
                  .width('100%')
                  Row() {
                    Text(t.detail)
                      .fontSize(this.fz(11))
                      .fontColor(FAINT)
                      .layoutWeight(1)
                    Text(t.ms.toFixed(0) + ' ms')
                      .fontSize(this.fz(11))
                      .fontColor(SUB)
                  }
                  .width('100%')
                  .margin({ top: 4 })
                  Row() {
                    Text('绝对吞吐')
                      .fontSize(this.fz(10))
                      .fontColor(FAINT)
                    Blank()
                    Text(metricUnit(t.metric, t.unit))
                      .fontSize(this.fz(11))
                      .fontColor(GOOD)
                  }
                  .width('100%')
                  .margin({ top: 2 })
                  Progress({
                    value: Math.min(100, t.score / 30),
                    total: 100,
                    type: ProgressType.Linear
                  })
                    .width('100%')
                    .color(accentGradient())
                    .backgroundColor(TRACK)
                    .margin({ top: 8 })
                }
                .width('100%')
                .padding(this.uiRes.pad)
                .backgroundColor(CARD)
                .borderRadius(this.cardRadius())
                .margin({ top: this.uiRes.gap })
                .flexGrow(1)
                .flexBasis(this.detailBasis())
                .constraintSize({ minWidth: this.detailMinW() })
              }, (t: TestResult, idx: number) => idx.toString())
              }
              .width('100%')
            }
            .width('100%')
            .margin({ top: this.uiRes.gap * 2 })
            if ((this.shown as BenchRecord).multiTests.length > 0) {
              Column() {
                Text('多核阶段明细（并行度 = 实际占用核心数）')
                  .fontSize(this.fz(12))
                  .fontColor(SUB)
                  .width('100%')
                  .margin({ bottom: 8 })
                Text(this.excludedText())
                  .fontSize(this.fz(10))
                  .fontColor(FAINT)
                  .width('100%')
                  .margin({ bottom: 6 })
                ForEach((this.shown as BenchRecord).multiTests, (t: TestResult) => {
                  Row() {
                    Text(t.name)
                      .fontSize(this.fz(13))
                      .fontColor(TEXT)
                      .layoutWeight(1)
                    Text(t.parallelism.toFixed(1) + ' 核')
                      .fontSize(this.fz(11))
                      .fontColor(WARN)
                      .width(70)
                      .textAlign(TextAlign.End)
                    Text(metricUnit(t.metric, t.unit))
                      .fontSize(this.fz(11))
                      .fontColor(GOOD)
                      .width(this.uiRes.detailMetricW)
                      .textAlign(TextAlign.End)
                    Text(t.score.toFixed(0))
                      .fontSize(this.fz(13))
                      .fontColor(ACCENT2)
                      .width(60)
                      .textAlign(TextAlign.End)
                  }
                  .width('100%')
                  .padding({ left: 10, right: 10, top: this.uiRes.rowPy + 2,
                    bottom: this.uiRes.rowPy + 2 })
                  .backgroundColor(CARD)
                  .borderRadius(Rd.sm)
                  .margin({ top: 4 })
                }, (t: TestResult, idx: number) => idx.toString())
              }
              .width('100%')
              .margin({ top: this.uiRes.gap * 2 })
            }
            if ((this.shown as BenchRecord).gpuScenes.length === 0) {
              Text('GPU 未运行：' + ((AppStorage.get<string>('gpuError') ?? '') === '' ? '未知原因' : (AppStorage.get<string>('gpuError') ?? '')))
                .fontSize(this.fz(12))
                .fontColor(DANGER)
                .width('100%')
                .margin({ top: this.uiRes.gap * 2 })
            }
            if ((this.shown as BenchRecord).gpuScenes.length > 0) {
              Column() {
                Text('GPU 场景明细（绝对吞吐量，跨平台可比）')
                  .fontSize(this.fz(12))
                  .fontColor(SUB)
                  .width('100%')
                  .margin({ bottom: this.uiRes.pad })
                Flex({ direction: FlexDirection.Row, wrap: FlexWrap.Wrap,
                  justifyContent: FlexAlign.Start }) {
                ForEach((this.shown as BenchRecord).gpuScenes, (g: GpuSceneResult) => {
                  Column() {
                    Row() {
                      Text(g.scene)
                        .fontSize(this.fz(14))
                        .fontColor(TEXT)
                        .layoutWeight(1)
                      Text(g.score.toFixed(0))
                        .fontSize(this.fz(15))
                        .fontColor(ACCENT2)
                    }
                    .width('100%')
                    Row() {
                      Text(g.fps.toFixed(1) + ' FPS · ' + g.frameMs.toFixed(2) + ' ms/帧')
                        .fontSize(this.fz(11))
                        .fontColor(SUB)
                        .layoutWeight(1)
                      Text(metricUnit(g.metric, g.unit))
                        .fontSize(this.fz(11))
                        .fontColor(GOOD)
                    }
                    .width('100%')
                    .margin({ top: 4 })
                  }
                  .width('100%')
                  .padding(this.uiRes.pad)
                  .backgroundColor(CARD)
                  .borderRadius(this.cardRadius())
                  .margin({ top: this.uiRes.gap })
                  .flexGrow(1)
                  .flexBasis(this.detailBasis())
                  .constraintSize({ minWidth: this.detailMinW() })
                }, (g: GpuSceneResult, idx: number) => idx.toString())
                }
                .width('100%')
              }
              .width('100%')
              .margin({ top: this.uiRes.gap * 2 })
            }
            Row() {
              Button('再跑一次')
                .layoutWeight(1)
                .constraintSize({ minHeight: this.btnH() })
                .fontSize(this.fz(14))
                .backgroundColor(CHIP)
                .fontColor(TEXT)
                .border(outline(C.chip))
                .onClick(() => {
                  this.shown = null;
                  this.startRun();
                })
              Button('返回')
                .layoutWeight(1)
                .constraintSize({ minHeight: this.btnH() })
                .fontSize(this.fz(14))
                .backgroundColor(ACCENT)
                .fontColor(ON_ACCENT)
                .margin({ left: Sp.x3 })
                .onClick(() => {
                  this.shown = null;
                })
            }
            .width('100%')
            .margin({ top: this.uiRes.gap * 2, bottom: this.uiRes.gap * 2 })
          }
        }
        .width('100%')
        .constraintSize({ maxWidth: this.contentMax() })
        .alignItems(HorizontalAlign.Center)
      }
      .width('100%')
    }
    .width('100%')
    .height('100%')
    // 官方磨砂材质: 结果浮层只在跑分结束后出现(条件里就带 !this.running),
    // 所以它的模糊 pass 永远不在跑分期间; 真跑分时 blurPolicy() 也会切到 ALWAYS_INACTIVE。
    .backgroundColor(this.materialColor())
    // 磨砂浮层上的品牌底: 用半透明版本, 免得把玻璃感盖死(见 appBackdropGradSoft)
    .linearGradient(this.appBackdropGradSoft())
    .backgroundBlurStyle(BlurStyle.Thick, {
      colorMode: ThemeColorMode.DARK,
      policy: this.blurPolicy(),
      inactiveColor: this.materialColor()
    })
    // 一次性转场(用户操作: 跑完弹出 / 点返回)
    .transition(this.fxTransition())
    // 结果浮层与主界面同一套安全区口径(内容滚到顶 / 底时也不会停在系统栏下面)
    .padding({ top: this.padTop(), bottom: this.padBottom() })
  }

  build() {
    // 沉浸式(边到边)的落地方式:
    //  · 根 Stack 用 expandSafeArea 铺满整屏(含状态栏 / 导航栏区域) —— 背景是沉浸的;
    //  · 根 Stack 里面的每一条内容流(主列 + 4 个浮层)都加 insets padding —— 控件不让系统栏压住;
    //  · 两个参数都来自 window.getWindowAvoidArea() 的实时读数, 窗口没铺满时自动退化为 0
    //    (此时系统本身已经避让过, 再加就成双重留白)。宁可留白, 不许遮住控件。
    Stack() {
      Column() {
        Column() {
          Row() {
            Text('Crush分')
              .fontSize(this.fz(20))
              .fontWeight(FontWeight.Bold)
              .fontColor(TEXT)
            // 这里**故意不用 APP_LABEL**: 左边那一格已经是 App 名了, 再用 APP_LABEL 会变成
            // 「Crush分  Crush分 9.0」(8.5 那会儿名字是中文「极光跑分」+ 英文 "AuroraBench 8.5",
            // 不重样; 9.0 两个名字合一之后就必须去掉一个)。APP_LABEL 仍然是报告抬头/账本里的那一个。
            Text('v' + APP_VER)
              .fontSize(this.fz(11))
              .fontColor(FAINT)
              .margin({ left: 8 })
            Blank()
            Text('总分上限 ' + (this.best > 0 ? this.best.toString() : '--'))
              .fontSize(this.fz(11))
              .fontColor(SUB)
          }
          .width('100%')
          .padding({ top: 14, bottom: 6 })
        }
        .width('92%')
        .constraintSize({ maxWidth: this.contentMax() })
        // 顶栏底部一条 1vp 分隔线(与卡片内分隔线同一规格); 纯色, 静态
        .border(edgeBottom(DIVIDER))

        if (this.tab === 0) {
          this.homeTab()
        } else if (this.tab === 1) {
          this.historyTab()
        } else {
          this.aboutTab()
        }

        Row() {
          this.tabBarItem(0, '首页')
          this.tabBarItem(1, '历史')
          this.tabBarItem(2, '关于')
        }
        .width('100%')
        .height(TAB_H)
        //  底栏用不透明的令牌色: 上一版留了 8% 的透(想让渐变延续到栏下), 但那会在
        //  "渐变边缘 + 半透明合成"之间留一条看得出来的硬边, 索性回到底色本身。
        .backgroundColor(C.tabBar)
        // 把底栏的窗口坐标记下来(只给"切标签时的粒子"当发射点; 写普通字段, 不引起重排)
        .onAreaChange((_o: Area, n: Area) => {
          this.fxBarX = this.fxNum(n.globalPosition.x);
          this.fxBarY = this.fxNum(n.globalPosition.y);
          this.fxBarW = this.fxNum(n.width);
        })
      }
      .width('100%')
      .height('100%')
      // 安全区: 顶部让开状态栏, 底部让开导航条 / 手势条(insets 来自系统, 见 refreshInsets)
      .padding({ top: this.padTop(), bottom: this.padBottom() })

      if (this.fullRunning) {
        this.fullRunOverlay()
      }
      if (this.fullReportOpen) {
        this.fullReportOverlay()
      }
      if (this.histOpen) {
        this.historyReportOverlay()
      }
      if (this.cmpOpen) {
        this.cmpOverlay()
      }
      if (this.running) {
        this.runningOverlay()
      }
      if (this.shown !== null && !this.running) {
        this.resultOverlay()
      }
      // 粒子层(消散 / 聚拢) —— ★ 唯一的创建点。
      //  两道门: fxOn()(跑分进行中恒为 false) + fxLive(真有一场粒子在飞时才为 true)。
      //  任何一道不成立, 这个 Canvas 节点就**不存在**: 跑分期间 GPU/CPU 上不会为它留
      //  任何一份资源, 也不会有任何一帧重绘(它永远不吃触摸: HitTestMode.None)。
      if (this.fxOn() && this.fxLive) {
        this.fxParticleLayer()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(BG)
    // App 自己的画布: 暗红 -> 深紫 -> 回落到 BG 的静态渐变(见 appBackdropGrad 的说明)。
    //  它画在 backgroundColor 之上、内容之下, 所以不占布局、不改任何宽高。
    .linearGradient(this.appBackdropGrad())
    // 根节点铺满整屏: 页面底色延伸到状态栏与导航栏之下(沉浸式), 内容由上面的 padding 让开
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
  }
}
