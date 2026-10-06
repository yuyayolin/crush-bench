import fs from '@ohos.file.fs';
import { util } from '@kit.ArkTS';
import { common } from '@kit.AbilityKit';
import { picker, fileUri } from '@kit.CoreFileKit';
import { compareTwoReports, RefCompareResult } from './RefAudit';
import { FullReportStore } from './FullRun';

// ===========================================================================
// 「两份报告对比」的 ArkTS 接线(libauroraref.so 的 compareReports, 只读 / 纯计算)
//
//  这一块回答的是用户的原始问题: "两个设备的分数比, 能不能定量算出芯片进步的比例?"
//  native 侧的做法(见 cpp/reference_compare.cpp 的 compareReports):
//    ① 逐项给出 A 分 / B 分 / 原始分数比;
//    ② 再把"系统策略差"从原始比里消掉, 给出两个归一化比 ——
//         perCoreRatio = (B 吞吐 / B 实际用到核数) / (A 吞吐 / A 实际用到核数)
//         sameFreqRatio = (B 吞吐 / B 运行时频率中位 x B 标称最高频) / (A 同理)
//       剩下的才更接近"芯片性能比";
//    ③ 与公开真值的偏差%(阈值 5%, 我们自己定的, 不是官方);
//    ④ 缺什么写什么(inputs.missing[] + 每一条的 why), 不编一个数。
//
//  本文件只做三件事: 读文件 / 调 native / 把 native 的 JSON 摊平成界面好用的行。
//    不做任何计算、不改 native 的一个数字、不计分、不写任何文件。
//  为什么 native 桥在 RefAudit.ets: ArkTS 每多一个文件 import 同一个 .so 就多一条
//    "module ... is not verified" 警告(本工程基线 11 条), 所以走 RefAudit 的同义直通桥。
//
//  性能: 全部是"用户按下按钮才发生一次"的只读操作(读盘 + 毫秒级纯计算),
//        没有定时器、没有动画、不进入任何渲染循环。
// ===========================================================================

// ---------------------------------------------------------------------------
// 一份被选中的报告文件
// ---------------------------------------------------------------------------
export interface CmpFile {
  label: string;      // 用户可读的标签(默认 = 文件名)
  name: string;       // 文件名
  uri: string;        // 来源(picker 给的 uri 或本机路径)
  text: string;       // 报告 JSON 全文
  ok: boolean;        // 是否读到了内容
  err: string;        // 读失败的真实原因(不吞)
}

function emptyFile(label: string, name: string, uri: string, err: string): CmpFile {
  return { label: label, name: name, uri: uri, text: '', ok: false, err: err };
}

// ---------------------------------------------------------------------------
// 读文本: openSync 直接开 uri / 路径都行(从 picker 拿到的 uri 自带授权, 用 fd 读最稳)。
// 上限 32 MB: 报告 JSON 通常只有几百 KB, 超过这个量级一定是选错了文件。
// ---------------------------------------------------------------------------
const MAX_REPORT_BYTES = 32 * 1024 * 1024;

export function readText(uriOrPath: string): string {
  let f: fs.File | null = null;
  try {
    f = fs.openSync(uriOrPath, fs.OpenMode.READ_ONLY);
    const st = fs.statSync(f.fd);
    let size: number = st.size;
    if (size <= 0) {
      return '';
    }
    if (size > MAX_REPORT_BYTES) {
      size = MAX_REPORT_BYTES;
    }
    const buf: ArrayBuffer = new ArrayBuffer(size);
    const n: number = fs.readSync(f.fd, buf);
    const decoder = util.TextDecoder.create('utf-8', { ignoreBOM: true });
    return decoder.decodeToString(new Uint8Array(buf.slice(0, n)));
  } finally {
    if (f !== null) {
      try {
        fs.closeSync(f);
      } catch (e2) {
        // 关不掉不影响"内容已读到"这个事实; 只记不抛
      }
    }
  }
}

/** 按本机路径读一份报告(本机 files/ 下已有的报告走这条) */
export function loadReportByPath(path: string): CmpFile {
  const name: string = fileNameOf(path);
  try {
    const t: string = readText(path);
    if (t.length === 0) {
      return emptyFile(name, name, path, '文件是空的（0 字节）。');
    }
    const f: CmpFile = { label: name, name: name, uri: path, text: t, ok: true, err: '' };
    return f;
  } catch (e) {
    return emptyFile(name, name, path, '读 ' + path + ' 失败：' + e.message);
  }
}

function fileNameOf(p: string): string {
  const at: number = p.lastIndexOf('/');
  return at >= 0 ? p.substring(at + 1) : p;
}

/** 从选择器返回的 uri 读一份报告 */
export function loadReportByUri(uri: string): CmpFile {
  const name: string = fileNameOf(uri);
  try {
    const t: string = readText(uri);
    if (t.length === 0) {
      return emptyFile(name, name, uri, '文件是空的（0 字节）。');
    }
    const f: CmpFile = { label: name, name: name, uri: uri, text: t, ok: true, err: '' };
    return f;
  } catch (e) {
    return emptyFile(name, name, uri, '读所选文件失败：' + e.message +
      '（若这是别的应用目录下的文件, 请重新用「从文件中选择」再选一次, 让系统重新授权）');
  }
}

// ---------------------------------------------------------------------------
// 本机 files/ 下已有的报告清单(一键全部跑分每次都会写 report-<时间戳>.json 与 report-latest.json)
// ---------------------------------------------------------------------------
export function listLocalReports(max: number): CmpFile[] {
  const out: CmpFile[] = [];
  const dir: string = FullReportStore.get().filesDir();
  if (dir.length === 0) {
    return out;
  }
  let names: string[] = [];
  try {
    names = fs.listFileSync(dir);
  } catch (e) {
    return out;
  }
  for (const n of names) {
    if (!n.endsWith('.json') || n.indexOf('report-') !== 0) {
      continue;
    }
    // latest 放最前面(它是"最近一次", 也是本工程推荐用来对比的那一份)
    if (n === 'report-latest.json') {
      out.unshift(loadReportByPath(dir + '/' + n));
    } else {
      out.push(loadReportByPath(dir + '/' + n));
    }
    if (out.length >= max) {
      break;
    }
  }
  return out;
}

/** 官方文档选择器(单选 .json): 用户选完返回报告全文; 取消 -> ok:false + 原因 */
export async function pickReportFile(ctx: common.UIAbilityContext): Promise<CmpFile> {
  try {
    const doc = new picker.DocumentViewPicker(ctx);
    const opt = new picker.DocumentSelectOptions();
    opt.maxSelectNumber = 1;
    opt.fileSuffixFilters = ['.json'];
    const uris: string[] = await doc.select(opt);
    if (uris.length === 0) {
      return emptyFile('', '', '', '没有选择文件（可能被取消）。');
    }
    const uri: string = uris[0];
    let path: string = '';
    try {
      path = new fileUri.FileUri(uri).path;
    } catch (e) {
      path = '';
    }
    if (path.length > 0) {
      // 有真实路径就先用路径读: 本工程自己目录下的文件走这条最直接
      const byPath: CmpFile = loadReportByPath(path);
      if (byPath.ok) {
        byPath.uri = uri;
        return byPath;
      }
    }
    return loadReportByUri(uri);
  } catch (e) {
    return emptyFile('', '', '', '文件选择器打不开：' + e.message);
  }
}

// ---------------------------------------------------------------------------
// 对比结果的展示模型(native 的 JSON 摊平成界面能直接渲染的行)
//   所有字段都带"有没有这个数"的标志: null 一律显示成「—(缺输入)」, 不显示 0 ——
//   0 和"没有数据"必须分得开, 否则用户会把"没测到"读成"比值为 0"。
// ---------------------------------------------------------------------------
export interface CmpSideView {
  present: boolean;
  score: number;
  hasScore: boolean;
  metric: number;
  hasMetric: boolean;
  unit: string;
  scoredByPolicy: boolean;
  scoredNote: string;
  condText: string;      // 取得条件一行(允许核集合 / 线程 / 实际用到核 / 运行时频率中位 / 标称最高频)
}

export interface CmpItemView {
  section: string;
  name: string;
  onlyIn: string;
  a: CmpSideView;
  b: CmpSideView;
  ratio: number;
  hasRatio: boolean;
  perCore: number;
  hasPerCore: boolean;
  sameFreq: number;
  hasSameFreq: boolean;
  perCoreSameFreq: number;
  hasPerCoreSameFreq: boolean;
  devPct: number;
  hasDev: boolean;
  within5: number;       // 1 = 在 5% 内 / 0 = 超出 / -1 = 没有这个判定(null)
  truthA: number;
  truthB: number;
  hasTruthA: boolean;
  hasTruthB: boolean;
  why: string;           // normalized.why(缺什么) 或 truth.why
}

export interface CmpCompositeView {
  key: string;           // 'gb7Single' / 'gb7Multi'
  title: string;
  a: number;
  hasA: boolean;
  b: number;
  hasB: boolean;
  ratio: number;
  hasRatio: boolean;
  refValue: number;
  hasRef: boolean;
  deltaA: number;
  hasDeltaA: boolean;
  deltaB: number;
  hasDeltaB: boolean;
}

export interface CmpAnchorView {
  label: string;
  from: string;
  to: string;
  unit: string;
  truthRatio: number;
  hasTruthRatio: boolean;
  measuredRatio: number;
  hasMeasured: boolean;
  devPct: number;
  hasDev: boolean;
  within5: number;
  sourceKind: string;
  source: string;
  why: string;
}

export interface CmpInflationView {
  name: string;
  howMuch: string;
  handledBy: string;
}

// ---------------------------------------------------------------------------
// 任务一: 两边各自的「随芯片变化自检」清单(项名 + 原因 + 判定依据)
//   为什么单列: 两台设备上"哪几项不可用于跨芯片比较"不同 => 读比时要留意的地方不同。
//   2026-10-05 更正: 这份清单不决定复合分的参与项集合(那件事由 compositeParticipants
//   按报告自己给出的参与项名单回答), 两者是两件事, 界面上也分成两段显示。
// ---------------------------------------------------------------------------
export interface CmpAuditRow {
  name: string;
  id: number;
  stage: string;      // 在哪一个口径上判的(单核 / 多核)
  reason: string;     // 判定依据原文(报告里那一行)
  hasReason: boolean;
}

export interface CmpAuditSide {
  present: boolean;
  excludedCount: number;
  passCount: number;
  failCount: number;
  unprovenCount: number;
  rows: CmpAuditRow[];
}

// 任务一(续): 复合分的参与项集合与「只用共同参与项」重算的比
export interface CmpStageParts {
  stage: string;              // 单核 / 多核
  aCount: number;
  bCount: number;
  commonCount: number;
  onlyInA: string[];
  onlyInB: string[];
  identical: number;          // 1 = 同一批项 / 0 = 不同 / -1 = 判不出来
  rawScoreRatio: number;      // 复合分原始比
  hasRawScoreRatio: boolean;
  sameScoreRatio: number;     // 只用共同参与项重算的分比
  hasSameScoreRatio: boolean;
  samePerCore: number;
  hasSamePerCore: boolean;
  sameSameFreq: number;
  hasSameSameFreq: boolean;
  why: string;
}

// 任务三: 跑分条件差 + 任务二: 两台的频率证据
export interface CmpCondSide {
  threads: number;            // 多核阶段实际线程数(threadsEffective 最大值)
  smt: number;                // 1 = SMT 开 / 0 = 关 / -1 = 没上报
  allowedCores: number;       // 可用核集合
  physicalCores: number;
  logicalCores: number;
  khzMedian: number;          // 逐项运行时频率中位的中位
  khzTop: number;             // 可用最高频档(逐项 cpuMaxKhz 最大值)
  khzMin: number;
  khzSamples: number;
  noteKhzMedian: number;      // 报告 inputsNote 原话
  noteKhzTop: number;
  smtText: string;
}

export interface CmpOutcome {
  ok: boolean;
  error: string;
  rawJson: string;
  labelA: string;
  labelB: string;
  deviceA: string;
  deviceB: string;
  socA: string;
  socB: string;
  verA: string;
  verB: string;
  itemsA: number;
  itemsB: number;
  interruptedA: boolean;
  interruptedB: boolean;
  ratioDefinition: string;
  whatThisAnswers: string;
  missing: string[];
  missingRule: string;
  problemA: string;      // native 对设备A 那份报告的原话(native 自己写"哪里不对")
  problemB: string;
  matched: number;
  onlyA: number;
  onlyB: number;
  comps: CmpCompositeView[];
  gmPerCore: number;
  hasGmPerCore: boolean;
  gmPerCoreItems: number;
  gmSameFreq: number;
  hasGmSameFreq: boolean;
  gmSameFreqItems: number;
  gmWhy: string;
  items: CmpItemView[];
  anchors: CmpAnchorView[];
  anchorThresholdNote: string;
  anchorThreshold: number;
  inflations: CmpInflationView[];
  coreRatioBA: number;
  hasCoreRatioBA: boolean;
  freqRatioBA: number;
  hasFreqRatioBA: boolean;
  allowedA: number;
  allowedB: number;
  coreNote: string;
  howToUse: string[];
  notComparable: string[];
  caveats: string[];
  truthRule: string;
  // ---- 用户真正要的那个数: 「分数比换算成芯片性能比是多少」(2026-10-08) ----
  // 全部来自 native 的 chipPerformanceRatio 块, 原样搬运, 一个数都不改。
  // why: 归一化量以前只散落在逐项表与 composites 里, 用户要的是一句话结论。
  chipRatioBA: number;          // 原始多核分数比(设备B/设备A)
  hasChipRatioBA: boolean;
  chipSingleRatioBA: number;    // 原始单核分数比(不含核数污染)
  hasChipSingleRatioBA: boolean;
  chipPerCoreSameFreqGM: number; // 两个污染源一起消掉 -> 最接近芯片性能比的那个数
  hasChipPerCoreSameFreqGM: boolean;
  chipPerCoreSameFreqItems: number;
  chipBest: number;             // 结论取的那个数(逐级降级: both > perCore > null)
  hasChipBest: boolean;
  chipBestWhich: string;        // 用的是哪一个(必须与数字一起显示)
  chipConclusion: string;       // native 写的一句话结论(原样显示)
  // ---- QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 真机报告缺陷 2) ----
  // 全部原样搬运 native 的 output.environmentEvidence: 两段文本来自各自报告 items[].qos / cpuset。
  // 这两个问题以前在对比页上完全看不到("这台机器的限制在哪一层""QoS 到底生效没有")。
  envWhatItAnswers: string;
  envNote: string;
  envQosPresentA: boolean;
  envQosPresentB: boolean;
  envQosTextA: string;
  envQosTextB: string;
  envQosFromA: string;          // 这段全文取自哪一项(section · name)
  envQosFromB: string;
  envQosNullA: string;          // present=false 时的原因(原话)
  envQosNullB: string;
  envCpusetPresentA: boolean;
  envCpusetPresentB: boolean;
  envCpusetTextA: string;
  envCpusetTextB: string;
  envCpusetFromA: string;
  envCpusetFromB: string;
  envCpusetNullA: string;
  envCpusetNullB: string;
  // ---- 任务一: 自检清单对照(哪几项不可用于跨芯片比较) ----
  auditIdentical: number;       // 1 = 两边排除集一致 / 0 = 不一致 / -1 = 判不出来
  auditConclusion: string;      // native 写的结论(原样显示)
  auditA: CmpAuditSide;
  auditB: CmpAuditSide;
  auditOnlyA: string[];
  auditOnlyB: string[];
  auditCommon: string[];
  auditCriteria: string[];
  auditWhatItIs: string;
  // ---- 任务一(续): 参与项同项性 ----
  partsNote: string;
  parts: CmpStageParts[];
  rawMultiUsable: number;       // 1/0/-1
  rawMultiUnusableReason: string;
  sameItemMultiRatio: number;   // 只用共同参与项重算的多核分比
  hasSameItemMultiRatio: boolean;
  sameItemMultiItems: number;
  // ---- 任务二: 频率证据 ----
  condA: CmpCondSide;
  condB: CmpCondSide;
  sameFreqNullReason: string;   // native 给不出同频比时的一句话原因
  // ---- 任务三: 条件差 ----
  condThreadsRatio: number;
  hasCondThreadsRatio: boolean;
  condSame: number;             // 1 = 条件相同 / 0 = 不同 / -1 = 判不出来
  condDirection: string;
  condPerCorePrecondition: string;
}

// ---- JSON 取值小工具(一律带默认值, 缺字段不抛) ----
function rec(o: Record<string, Object>, k: string): Record<string, Object> {
  return (o[k] as Record<string, Object>) ?? ({} as Record<string, Object>);
}

// native 的 observedDifferences / conditionGaps / selfCheckAudit / compositeParticipants
//   这几个块按契约(见 cpp/reference_compare.h)挂在 conditions 里; 界面这一层不假设位置:
//   顶层找不到就再到 conditions 里找一次 —— 少一次"界面上那一栏永远是空的"的静默失败。
function pick(o: Record<string, Object>, k: string): Record<string, Object> {
  const at: Object = o[k] as Object;
  if (at !== null && at !== undefined) {
    return at as Record<string, Object>;
  }
  return rec(rec(o, 'conditions'), k);
}

function pickArr(o: Record<string, Object>, k: string): Object[] {
  const at: Object = o[k] as Object;
  if (at !== null && at !== undefined) {
    return at as Object[];
  }
  return arr(rec(o, 'conditions'), k);
}

// 三态: true -> 1 / false -> 0 / 缺 -> -1(与 truth.within5Percent 同一套写法)
function triOf(o: Record<string, Object>, k: string): number {
  const raw: Object = o[k] as Object;
  if (raw === null || raw === undefined) {
    return -1;
  }
  return (raw as boolean) ? 1 : 0;
}

function arr(o: Record<string, Object>, k: string): Object[] {
  return (o[k] as Object[]) ?? [];
}

function num(o: Record<string, Object>, k: string): number {
  const v: number = (o[k] as number) ?? 0;
  return (v === null || v === undefined || isNaN(v)) ? 0 : v;
}

function hasNum(o: Record<string, Object>, k: string): boolean {
  const raw: Object = o[k] as Object;
  if (raw === null || raw === undefined) {
    return false;
  }
  const v: number = raw as number;
  return !isNaN(v);
}

function str(o: Record<string, Object>, k: string): string {
  return (o[k] as string) ?? '';
}

function bool(o: Record<string, Object>, k: string): boolean {
  return (o[k] as boolean) ?? false;
}

function numArr(o: Record<string, Object>, k: string): string[] {
  const out: string[] = [];
  for (const it of arr(o, k)) {
    if (typeof it === 'string') {
      out.push(it as string);
    }
  }
  return out;
}

// 5% 判定的三态: true -> 1 / false -> 0 / null -> -1(没有这个判定)
function tri(o: Record<string, Object>, k: string): number {
  const raw: Object = o[k] as Object;
  if (raw === null || raw === undefined) {
    return -1;
  }
  return (raw as boolean) ? 1 : 0;
}

function mhz(khz: number): string {
  return khz > 0 ? (Math.round(khz / 1000).toString() + 'MHz') : '—';
}

// 一侧的"取得条件"一行(全部来自报告里逐项落盘的条件字段; 缺就写缺)
function sideView(o: Record<string, Object>, present: boolean): CmpSideView {
  const c: Record<string, Object> = rec(o, 'conditions');
  const allowed: number = num(c, 'allowedCores');
  const cores: number = num(c, 'coresActuallyUsed');
  const thEff: number = num(c, 'threadsEffective');
  const khz: number = num(c, 'runtimeKhzMedian');
  const top: number = num(c, 'nominalTopKhz');
  const parts: string[] = [];
  if (allowed > 0) {
    parts.push('允许核集合 ' + allowed.toString() + ' 核');
  } else {
    parts.push('允许核集合 未上报');
  }
  parts.push('线程 ' + (thEff > 0 ? thEff.toString() : '未上报'));
  parts.push('实际用到核 ' + (cores > 0 ? cores.toString() : '未上报'));
  parts.push('运行时频率中位 ' + mhz(khz));
  parts.push('标称最高频 ' + mhz(top));
  const v: CmpSideView = {
    present: present,
    score: num(o, 'score'),
    hasScore: hasNum(o, 'score'),
    metric: num(o, 'metric'),
    hasMetric: hasNum(o, 'metric'),
    unit: str(o, 'unit'),
    scoredByPolicy: bool(o, 'scoredByPolicy'),
    scoredNote: str(o, 'scoredNote'),
    condText: parts.join(' · ')
  };
  return v;
}

function itemView(o: Record<string, Object>): CmpItemView {
  const norm: Record<string, Object> = rec(o, 'normalized');
  const truth: Record<string, Object> = rec(o, 'truth');
  const why: string = str(norm, 'why');
  const v: CmpItemView = {
    section: str(o, 'section'),
    name: str(o, 'name'),
    onlyIn: str(o, 'onlyIn'),
    a: sideView(rec(o, 'a'), true),
    b: sideView(rec(o, 'b'), true),
    ratio: num(o, 'ratio'),
    hasRatio: hasNum(o, 'ratio'),
    perCore: num(norm, 'perCoreRatio'),
    hasPerCore: hasNum(norm, 'perCoreRatio'),
    sameFreq: num(norm, 'sameFreqRatio'),
    hasSameFreq: hasNum(norm, 'sameFreqRatio'),
    perCoreSameFreq: num(norm, 'perCoreSameFreqRatio'),
    hasPerCoreSameFreq: hasNum(norm, 'perCoreSameFreqRatio'),
    devPct: num(truth, 'deviationFromTruthPercent'),
    hasDev: hasNum(truth, 'deviationFromTruthPercent'),
    within5: tri(truth, 'within5Percent'),
    truthA: num(truth, 'referenceA'),
    truthB: num(truth, 'referenceB'),
    hasTruthA: hasNum(truth, 'referenceA'),
    hasTruthB: hasNum(truth, 'referenceB'),
    why: why.length > 0 ? why : str(truth, 'why')
  };
  return v;
}

function compositeView(o: Record<string, Object>, key: string, title: string): CmpCompositeView {
  const v: CmpCompositeView = {
    key: key,
    title: title,
    a: num(o, 'a'),
    hasA: hasNum(o, 'a'),
    b: num(o, 'b'),
    hasB: hasNum(o, 'b'),
    ratio: num(o, 'ratio'),
    hasRatio: hasNum(o, 'ratio'),
    refValue: num(o, 'reference'),
    hasRef: hasNum(o, 'reference'),
    deltaA: num(o, 'deltaPercentA'),
    hasDeltaA: hasNum(o, 'deltaPercentA'),
    deltaB: num(o, 'deltaPercentB'),
    hasDeltaB: hasNum(o, 'deltaPercentB')
  };
  return v;
}

function anchorView(o: Record<string, Object>): CmpAnchorView {
  const v: CmpAnchorView = {
    label: str(o, 'label'),
    from: str(o, 'from'),
    to: str(o, 'to'),
    unit: str(o, 'unit'),
    truthRatio: num(o, 'truthRatioBA'),
    hasTruthRatio: hasNum(o, 'truthRatioBA'),
    measuredRatio: num(o, 'measuredRatioBA'),
    hasMeasured: hasNum(o, 'measuredRatioBA'),
    devPct: num(o, 'deviationPercent'),
    hasDev: hasNum(o, 'deviationPercent'),
    within5: tri(o, 'linearWithin5Percent'),
    sourceKind: str(o, 'sourceKind'),
    source: str(o, 'source'),
    why: str(o, 'why')
  };
  return v;
}

// 已知污染源: native 端给什么显示什么; 万一没给(native 更老), 用同一套固定文本兜底
const FALLBACK_INFLATIONS: CmpInflationView[] = [
  {
    name: '可用核数不同',
    howMuch: '核数多的那台总分比会虚高 12.5%（9 核 vs 8 核 = 1.125x）',
    handledBy: 'normalized.perCoreRatio（每核吞吐比 = 吞吐 / 实际用到核数）'
  },
  {
    name: '最高频档不同',
    howMuch: '跑在不同频率档上能凭空造出 13.5% 的假提升（1.135x）',
    handledBy: 'normalized.sameFreqRatio（吞吐 / 运行时频率中位 x 标称最高频）'
  },
  {
    name: '核数与频率同时不同',
    howMuch: '两个污染源会相乘（1.125 x 1.135 ≈ 1.277）',
    handledBy: 'normalized.perCoreSameFreqRatio（两个一起消掉）'
  }
];

const EMPTY_OUTCOME_EXTRA: string =
  '（我们自己定的阈值 / 口径：线性判定 5%、重复性 2% 与 5% 都不是官方阈值。）';

function emptyCondSide(): CmpCondSide {
  return {
    threads: 0, smt: -1, allowedCores: 0, physicalCores: 0, logicalCores: 0,
    khzMedian: 0, khzTop: 0, khzMin: 0, khzSamples: 0,
    noteKhzMedian: 0, noteKhzTop: 0, smtText: ''
  };
}

// 一侧的取得条件(线程数 / SMT / 核集合 / 频率证据)。缺的一律 0 或 -1, 不编。
function condSide(o: Record<string, Object>): CmpCondSide {
  const m: Record<string, Object> = rec(o, 'machine');
  const f: Record<string, Object> = rec(o, 'runtimeKhzMedianOfItems');
  const note: Record<string, Object> = rec(o, 'machineLevelFromReport');
  const v: CmpCondSide = {
    threads: num(m, 'multiThreads'),
    smt: triOf(m, 'smtEnabled'),
    allowedCores: num(m, 'allowedCores'),
    physicalCores: num(m, 'physicalCores'),
    logicalCores: num(m, 'logicalCores'),
    khzMedian: num(f, 'median'),
    khzTop: num(o, 'nominalTopKhzMax'),
    khzMin: num(f, 'min'),
    khzSamples: num(f, 'samples'),
    noteKhzMedian: num(note, 'runtimeKhzMedian'),
    noteKhzTop: num(note, 'nominalTopKhz'),
    smtText: str(m, 'smtTopologyText')
  };
  return v;
}

// 一侧的自检清单(逐项: 项名 + 口径 + 判定依据原文; 它不改写复合分的参与项集合)
function auditSide(o: Record<string, Object>): CmpAuditSide {
  const v: CmpAuditSide = {
    present: bool(o, 'present'),
    excludedCount: num(o, 'excludedCount'),
    passCount: num(o, 'passCount'),
    failCount: num(o, 'failCount'),
    unprovenCount: num(o, 'unprovenCount'),
    rows: []
  };
  for (const it of arr(o, 'excluded')) {
    const d = it as Record<string, Object>;
    v.rows.push({
      name: str(d, 'name'),
      id: num(d, 'id'),
      stage: str(d, 'stage'),
      reason: str(d, 'reason'),
      hasReason: bool(d, 'hasReason')
    });
  }
  return v;
}

function strArrOf(o: Record<string, Object>, k: string): string[] {
  return numArr(o, k);
}

/**
 * 调 native 做对比, 并把结果摊平成界面行。不做任何计算, 只搬运 + 摊平。
 * @param aJson 设备A 报告 JSON 全文
 * @param bJson 设备B 报告 JSON 全文
 * @param labelA / labelB 用户可读标签(进 native 的 options, 也会显示在结果里)
 * @param socA / socB 可选: 显式指定 SoC(拿不到就传空串, 让 native 自己从报告里判)
 */
export function runCompare(aJson: string, bJson: string, labelA: string, labelB: string,
  socA: string, socB: string): CmpOutcome {
  const out: CmpOutcome = {
    ok: false, error: '', rawJson: '',
    labelA: labelA, labelB: labelB, deviceA: '', deviceB: '', socA: '', socB: '',
    verA: '', verB: '', itemsA: 0, itemsB: 0, interruptedA: false, interruptedB: false,
    ratioDefinition: '', whatThisAnswers: '', missing: [], missingRule: '',
    problemA: '', problemB: '',
    matched: 0, onlyA: 0, onlyB: 0, comps: [],
    gmPerCore: 0, hasGmPerCore: false, gmPerCoreItems: 0,
    gmSameFreq: 0, hasGmSameFreq: false, gmSameFreqItems: 0, gmWhy: '',
    items: [], anchors: [], anchorThresholdNote: '', anchorThreshold: 5,
    inflations: [], coreRatioBA: 0, hasCoreRatioBA: false, freqRatioBA: 0, hasFreqRatioBA: false,
    allowedA: 0, allowedB: 0, coreNote: '',
    howToUse: [], notComparable: [], caveats: [], truthRule: '',
    chipRatioBA: 0, hasChipRatioBA: false,
    chipSingleRatioBA: 0, hasChipSingleRatioBA: false,
    chipPerCoreSameFreqGM: 0, hasChipPerCoreSameFreqGM: false, chipPerCoreSameFreqItems: 0,
    chipBest: 0, hasChipBest: false, chipBestWhich: '', chipConclusion: '',
    envWhatItAnswers: '', envNote: '',
    envQosPresentA: false, envQosPresentB: false, envQosTextA: '', envQosTextB: '',
    envQosFromA: '', envQosFromB: '', envQosNullA: '', envQosNullB: '',
    envCpusetPresentA: false, envCpusetPresentB: false, envCpusetTextA: '', envCpusetTextB: '',
    envCpusetFromA: '', envCpusetFromB: '', envCpusetNullA: '', envCpusetNullB: '',
    auditIdentical: -1, auditConclusion: '', auditOnlyA: [], auditOnlyB: [], auditCommon: [],
    auditCriteria: [], auditWhatItIs: '',
    auditA: { present: false, excludedCount: 0, passCount: 0, failCount: 0, unprovenCount: 0, rows: [] },
    auditB: { present: false, excludedCount: 0, passCount: 0, failCount: 0, unprovenCount: 0, rows: [] },
    partsNote: '', parts: [],
    rawMultiUsable: -1, rawMultiUnusableReason: '', sameItemMultiRatio: 0, hasSameItemMultiRatio: false,
    sameItemMultiItems: 0,
    condA: emptyCondSide(), condB: emptyCondSide(), sameFreqNullReason: '',
    condThreadsRatio: 0, hasCondThreadsRatio: false, condSame: -1, condDirection: '',
    condPerCorePrecondition: ''
  };
  // options: 拿不到的键一个都不写(native 侧读不到就写 null, 不会编 0)
  const opts: string[] = [];
  if (labelA.length > 0) {
    opts.push('"labelA":' + JSON.stringify(labelA));
  }
  if (labelB.length > 0) {
    opts.push('"labelB":' + JSON.stringify(labelB));
  }
  if (socA.length > 0) {
    opts.push('"socA":' + JSON.stringify(socA));
  }
  if (socB.length > 0) {
    opts.push('"socB":' + JSON.stringify(socB));
  }
  const optionsJson: string = '{' + opts.join(',') + '}';
  const r: RefCompareResult = compareTwoReports(aJson, bJson, optionsJson);
  if (r.json.length === 0) {
    out.error = r.error.length > 0 ? r.error : '没有拿到对比结果。';
    return out;
  }
  out.rawJson = r.json;
  if (!r.ok) {
    // cmp.ok !== true 时原样显示 native 的原文 —— 缺什么写什么, 我们不许自己编
    out.error = r.error.length > 0 ? r.error : r.json;
    // 即便 ok !== true, 也尽力把 native 原文里的 inputs.missing / 两侧 problem 摘成清单,
    // 让用户一眼看到"缺的是什么"; 摘不出来就只显示上面那段原文(不硬凑)。
    try {
      const bad = JSON.parse(r.json) as Record<string, Object>;
      const bi: Record<string, Object> = rec(bad, 'inputs');
      out.missing = numArr(bi, 'missing');
      out.missingRule = str(bi, 'missingRule');
      out.problemA = str(rec(bi, 'a'), 'problem');
      out.problemB = str(rec(bi, 'b'), 'problem');
    } catch (e) {
      // 连 JSON 都解析不了: 保持只显示原文(上面的 error), 不猜内容
    }
    return out;
  }
  let o: Record<string, Object> = {} as Record<string, Object>;
  try {
    o = JSON.parse(r.json) as Record<string, Object>;
  } catch (e) {
    out.error = 'native 返回的对比 JSON 无法解析: ' + e.message;
    return out;
  }
  out.ok = true;
  out.ratioDefinition = str(o, 'ratioDefinition');
  out.whatThisAnswers = str(o, 'whatThisAnswers');
  out.truthRule = str(o, 'truthRule');
  // ---- 输入与缺什么 ----
  const inputs: Record<string, Object> = rec(o, 'inputs');
  const ia: Record<string, Object> = rec(inputs, 'a');
  const ib: Record<string, Object> = rec(inputs, 'b');
  out.deviceA = str(ia, 'device');
  out.deviceB = str(ib, 'device');
  out.itemsA = num(ia, 'items');
  out.itemsB = num(ib, 'items');
  out.missing = numArr(inputs, 'missing');
  out.missingRule = str(inputs, 'missingRule');
  out.problemA = str(ia, 'problem');
  out.problemB = str(ib, 'problem');
  // ---- 两份报告各自的取得条件 ----
  const cond: Record<string, Object> = rec(o, 'conditions');
  const ca: Record<string, Object> = rec(cond, 'a');
  const cb: Record<string, Object> = rec(cond, 'b');
  out.socA = str(ca, 'soc');
  out.socB = str(cb, 'soc');
  out.verA = str(ca, 'appVersion') + ' / native ' + str(ca, 'nativeVersion');
  out.verB = str(cb, 'appVersion') + ' / native ' + str(cb, 'nativeVersion');
  if (out.deviceA.length === 0) {
    out.deviceA = str(ca, 'device');
  }
  if (out.deviceB.length === 0) {
    out.deviceB = str(cb, 'device');
  }
  out.interruptedA = bool(ca, 'interrupted');
  out.interruptedB = bool(cb, 'interrupted');
  // ---- QoS / 限制层级证据(2026-10 真机报告缺陷 2) ----
  //   原样搬运 native 的 environmentEvidence:a/b 各自一段 qos 与一段 cpuset(都是报告 items[] 的原文)。
  //   present=false 时显示 native 给的 nullReason 原话, 界面上不显示成"空字符串 = 正常"。
  {
    const env: Record<string, Object> = rec(o, 'environmentEvidence');
    out.envWhatItAnswers = str(env, 'whatItAnswers');
    out.envNote = str(env, 'note');
    const ea: Record<string, Object> = rec(env, 'a');
    const eb: Record<string, Object> = rec(env, 'b');
    const qa: Record<string, Object> = rec(ea, 'qos');
    const qb: Record<string, Object> = rec(eb, 'qos');
    const sa: Record<string, Object> = rec(ea, 'cpuset');
    const sb: Record<string, Object> = rec(eb, 'cpuset');
    out.envQosPresentA = bool(qa, 'present');
    out.envQosPresentB = bool(qb, 'present');
    out.envQosTextA = str(qa, 'text');
    out.envQosTextB = str(qb, 'text');
    out.envQosFromA = str(qa, 'fromItem');
    out.envQosFromB = str(qb, 'fromItem');
    out.envQosNullA = str(qa, 'nullReason');
    out.envQosNullB = str(qb, 'nullReason');
    out.envCpusetPresentA = bool(sa, 'present');
    out.envCpusetPresentB = bool(sb, 'present');
    out.envCpusetTextA = str(sa, 'text');
    out.envCpusetTextB = str(sb, 'text');
    out.envCpusetFromA = str(sa, 'fromItem');
    out.envCpusetFromB = str(sb, 'fromItem');
    out.envCpusetNullA = str(sa, 'nullReason');
    out.envCpusetNullB = str(sb, 'nullReason');
  }
  // ---- 任务一/二/三: 排除集 / 参与项同项性 / 频率证据 / 条件差(2026-10-08) ----
  //   全部原样搬运 native 的字段, 界面不做任何计算。
  const audit: Record<string, Object> = pick(o, 'selfCheckAudit');
  out.auditWhatItIs = str(audit, 'whatItIs');
  out.auditA = auditSide(rec(audit, 'a'));
  out.auditB = auditSide(rec(audit, 'b'));
  out.auditOnlyA = strArrOf(audit, 'onlyExcludedInA');
  out.auditOnlyB = strArrOf(audit, 'onlyExcludedInB');
  out.auditCommon = strArrOf(audit, 'commonExcludedNames');
  out.auditConclusion = str(audit, 'conclusion');
  out.auditIdentical = triOf(audit, 'excludedSetsIdentical');
  out.auditCriteria = strArrOf(rec(audit, 'a'), 'criteria');
  const partsBlock: Record<string, Object> = pick(o, 'compositeParticipants');
  out.partsNote = str(partsBlock, 'whatItIs');
  const partKeys: string[] = ['single', 'multi'];
  const compsForParts: Record<string, Object> = rec(o, 'composites');
  for (const pk of partKeys) {
    const sp: Record<string, Object> = rec(partsBlock, pk);
    const cmpBlock: Record<string, Object> = rec(sp, 'sameItemSetComparison');
    const compKey: string = (pk === 'single') ? 'gb7Single' : 'gb7Multi';
    const compBlock: Record<string, Object> = rec(compsForParts, compKey);
    out.parts.push({
      stage: str(sp, 'stage'),
      aCount: num(rec(sp, 'a'), 'count'),
      bCount: num(rec(sp, 'b'), 'count'),
      commonCount: num(sp, 'commonCount'),
      onlyInA: strArrOf(sp, 'onlyInA'),
      onlyInB: strArrOf(sp, 'onlyInB'),
      identical: triOf(sp, 'identical'),
      rawScoreRatio: num(compBlock, 'ratio'), hasRawScoreRatio: hasNum(compBlock, 'ratio'),
      sameScoreRatio: num(cmpBlock, 'compositeScoreRatioBA'),
      hasSameScoreRatio: hasNum(cmpBlock, 'compositeScoreRatioBA'),
      samePerCore: num(cmpBlock, 'perCoreRatioGM'),
      hasSamePerCore: hasNum(cmpBlock, 'perCoreRatioGM'),
      sameSameFreq: num(cmpBlock, 'sameFreqRatioGM'),
      hasSameSameFreq: hasNum(cmpBlock, 'sameFreqRatioGM'),
      why: str(sp, 'why')
    });
  }
  const gaps: Record<string, Object> = pick(o, 'conditionGaps');
  out.condA = condSide(ca);
  out.condB = condSide(cb);
  out.condThreadsRatio = num(gaps, 'threadsRatioBA');
  out.hasCondThreadsRatio = hasNum(gaps, 'threadsRatioBA');
  out.condSame = triOf(gaps, 'sameRunConditions');
  out.condDirection = str(gaps, 'direction');
  out.condPerCorePrecondition = str(gaps, 'perCorePrecondition');
  // ---- 观测到的差异 + 已知污染源 ----
  const obs: Record<string, Object> = pick(o, 'observedDifferences');
  out.allowedA = num(obs, 'allowedCoresA');
  out.allowedB = num(obs, 'allowedCoresB');
  out.coreRatioBA = num(obs, 'coreCountRatioBA');
  out.hasCoreRatioBA = hasNum(obs, 'coreCountRatioBA');
  out.freqRatioBA = num(obs, 'freqTierRatioBA');
  out.hasFreqRatioBA = hasNum(obs, 'freqTierRatioBA');
  out.coreNote = str(obs, 'note');
  let infl = arr(obs, 'knownInflationSources');
  if (infl.length === 0) {
    infl = pickArr(o, 'knownInflationSources');
  }
  if (infl.length === 0) {
    for (const f of FALLBACK_INFLATIONS) {
      out.inflations.push(f);
    }
  } else {
    for (const it of infl) {
      const d = it as Record<string, Object>;
      out.inflations.push({
        name: str(d, 'name'), howMuch: str(d, 'howMuch'), handledBy: str(d, 'handledBy')
      });
    }
  }
  // ---- 逐项 ----
  for (const it of arr(o, 'items')) {
    out.items.push(itemView(it as Record<string, Object>));
  }
  out.matched = num(o, 'itemsMatched');
  out.onlyA = num(o, 'itemsOnlyInA');
  out.onlyB = num(o, 'itemsOnlyInB');
  // ---- 复合分 ----
  const comps: Record<string, Object> = rec(o, 'composites');
  out.comps.push(compositeView(rec(comps, 'gb7Single'), 'gb7Single', 'CS1 单核复合分'));
  out.comps.push(compositeView(rec(comps, 'gb7Multi'), 'gb7Multi', 'CS1 多核复合分'));
  const norm: Record<string, Object> = rec(comps, 'normalized');
  out.gmPerCore = num(norm, 'perCoreRatioGM');
  out.hasGmPerCore = hasNum(norm, 'perCoreRatioGM');
  out.gmPerCoreItems = num(norm, 'perCoreRatioGMItems');
  out.gmSameFreq = num(norm, 'sameFreqRatioGM');
  out.hasGmSameFreq = hasNum(norm, 'sameFreqRatioGM');
  out.gmSameFreqItems = num(norm, 'sameFreqRatioGMItems');
  out.gmWhy = str(norm, 'why');
  // ---- 分数比 -> 芯片性能比: 用户真正要的那个数(native 原样搬运) ----
  // 优先读 native 新给的 chipPerformanceRatio 块; 老 native 没有这一块时, 用 composites 里
  // 已有的两个归一化 GM 兜底(同一套口径, 只是少了"两个一起消"的那个数) —— 兜底也标注。
  const cpr: Record<string, Object> = rec(o, 'chipPerformanceRatio');
  out.chipRatioBA = num(cpr, 'rawMultiCompositeRatioBA');
  out.hasChipRatioBA = hasNum(cpr, 'rawMultiCompositeRatioBA');
  out.chipSingleRatioBA = num(cpr, 'rawSingleCompositeRatioBA');
  out.hasChipSingleRatioBA = hasNum(cpr, 'rawSingleCompositeRatioBA');
  const cprNorm: Record<string, Object> = rec(cpr, 'normalized');
  out.chipPerCoreSameFreqGM = num(cprNorm, 'perCoreSameFreqRatioGM');
  out.hasChipPerCoreSameFreqGM = hasNum(cprNorm, 'perCoreSameFreqRatioGM');
  out.chipPerCoreSameFreqItems = num(cprNorm, 'perCoreSameFreqRatioGMItems');
  out.chipBest = num(cpr, 'bestEstimateOfChipPerformanceRatio');
  out.hasChipBest = hasNum(cpr, 'bestEstimateOfChipPerformanceRatio');
  out.chipBestWhich = str(cpr, 'bestEstimateWhich');
  out.chipConclusion = str(cpr, 'conclusion');
  // 原始多核复合分比可用吗(参与项不同的两个复合分不能相除)
  out.rawMultiUsable = triOf(cpr, 'rawMultiCompositeRatioUsable');
  out.rawMultiUnusableReason = str(cpr, 'rawMultiCompositeRatioUnusableReason');
  out.sameItemMultiRatio = num(cpr, 'sameItemSetMultiCompositeRatioBA');
  out.hasSameItemMultiRatio = hasNum(cpr, 'sameItemSetMultiCompositeRatioBA');
  out.sameItemMultiItems = num(cpr, 'sameItemSetMultiCompositeItems');
  // 真值锚点 ----
  const anchors: Record<string, Object> = rec(o, 'truthAnchors');
  out.anchorThreshold = num(anchors, 'thresholdPercent');
  out.anchorThresholdNote = str(anchors, 'thresholdNote');
  for (const it of arr(anchors, 'rows')) {
    out.anchors.push(anchorView(it as Record<string, Object>));
  }
  // ---- 结论 ----
  const v: Record<string, Object> = rec(o, 'verdicts');
  out.howToUse = numArr(v, 'howToUse');
  out.notComparable = numArr(o, 'notComparable');
  out.caveats = numArr(o, 'caveats');
  return out;
}

/** 一行显示用的数字(缺 -> '—'; 有 -> 保留 n 位) */
export function fmtNum(v: number, digits: number, has: boolean): string {
  if (!has) {
    return '—';
  }
  return v.toFixed(digits);
}

/** 比值的一行文本: 1.234x（> 1 = 设备B 更高） */
export function fmtRatio(v: number, has: boolean): string {
  if (!has) {
    return '—（缺输入）';
  }
  return v.toFixed(4) + 'x';
}

/** 5% 判定的显示文本 */
export function within5Text(t: number): string {
  if (t < 0) {
    return '—';
  }
  return t === 1 ? '是' : '否';
}

/** 口径声明: 界面上必须与数字一起显示的那句 */
export function extraCaveat(): string {
  return EMPTY_OUTCOME_EXTRA;
}
