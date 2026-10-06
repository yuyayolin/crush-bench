// ---------------------------------------------------------------------------
// 结果页「三层呈现」的纯数据层(没有任何 UI 组件, 也没有任何计时器 / 动画 / 副作用)
//
//  第一层 L1: 三个大数字卡片 —— 单核 / 多核 / GPU
//  ★ 2026-10(10.0)位置对调: **大数字那一层给 CS1**(用户:"把分数的主体突出部位变成 CS1"),
//    下面那一行一项的清单里前三行给自研套件。两边的取数来源都各自只有一处, 见下面两个函数。
//  第二层 L2: 每个小节的关键得分一行(统一表格) —— 自研套件总分 / 自研 单核 / 自研 多核 /
//             自研 GPU / GPU-SNL(Aurora Nomad Light, 不计分) / NPU / 存储,
//             每个分都带单位与「取得条件」; 不计分的一律写明"不计分"与原因
//  第三层 L3: 逐项明细(metric / unit / 得分 / 耗时 / 并行度 / 运行时频率 / 落核 / diag),
//             界面上默认收起; 落盘的完整报告文件里仍然是全部内容
//
//  规矩(与 Theme.ets 一致):
//   1) 这里只做取值与排版, 不改动任何 k / conv / 计分公式 / metric 口径 / 单位 /
//      线程数与绑核策略 —— 数值一律原样取自既有实现;
//   2) 纯函数, 无状态, 不监听任何东西, 不会在跑分期间产生任何开销(只在跑完那一刻算一次);
//   3) 每个分数旁边必须给「取得条件」: 可用核数 / 可用最高频档 / 线程数 / 实际用到几个核。
// ---------------------------------------------------------------------------
import { BenchRecord, Gb7Result, TestResult, GpuSceneResult } from '../model/BenchModel';
import { FullRunOutcome, FullReportBuilder } from '../service/FullRun';
import { BenchRunner, SnRunResult } from '../service/BenchRunner';
// metric(吞吐)的显示口径: 与 native 的 "%.4g" 同口径的 4 位有效数字(见 NumberFmt.ets)。
// 本文件里 num0/num1/num2 是分数 / 耗时那一类的排版(1~2 位小数), metric 一律走 metricOrDash,
// 这样"屏幕上这一项的绝对吞吐"才与"屏幕上这一项的分数"出自同一份有效数字。
import { metricOrDash } from './NumberFmt';

// ---- 数值排版(纯函数) ----
// 未取到一律写 '--', 不写 0 冒充真实读数
export function num0(v: number): string {
  return v > 0 ? v.toFixed(0) : '--';
}

export function num1(v: number): string {
  return v > 0 ? v.toFixed(1) : '--';
}

export function num2(v: number): string {
  return v > 0 ? v.toFixed(2) : '--';
}

// kHz -> '2620MHz'; 0 = 未知(写"未知", 不编造)
export function khzText(khz: number): string {
  if (khz <= 0) {
    return '未知';
  }
  return (khz / 1000).toFixed(0) + 'MHz';
}

// 运行 → '未知' 时原样透出原因(不吞掉)
export function orUnknown(s: string): string {
  return s.length > 0 ? s : '未知';
}

// ---- 取得条件(L1 与 L2 的每一行都要带) ----
export interface CondFacts {
  cores: number;      // 可用(逻辑)核数; 0 = 未知
  topKhz: number;     // 可用最高频档(kHz); 0 = 未知
  topCores: number;   // 最高频档上的核数; 0 = 未知
  threads: number;    // 多核阶段线程数(既有口径, 未改); 0 = 未知
  used: number;       // CS1 多核阶段实际用到的核数(实测最大值); 0 = 未知
  restricted: boolean; // 可用核集合把最快档挡在外面 / 快簇被夹
  source: string;     // 这些事实的来源一行(设备画像原文摘要)
}

// 单核项的取得条件
export function condSingle(c: CondFacts): string {
  return '1 线程 · 可用 ' + (c.cores > 0 ? c.cores.toString() : '?') + ' 核 · 最高频档 ' +
    khzText(c.topKhz) + (c.topCores > 0 ? ('(' + c.topCores.toString() + ' 核)') : '');
}

// 多核项的取得条件
export function condMulti(c: CondFacts): string {
  return (c.threads > 0 ? c.threads.toString() : '?') + ' 线程 · 实测占用 ' +
    (c.used > 0 ? c.used.toString() : '?') + ' 核 · 可用 ' +
    (c.cores > 0 ? c.cores.toString() : '?') + ' 核 · 最高频档 ' + khzText(c.topKhz);
}

// GPU 项的取得条件(GPU 不绑 CPU 核, 说明)
export function condGpu(c: CondFacts): string {
  return '离屏渲染 · 不绑 CPU 核 · 可用 ' + (c.cores > 0 ? c.cores.toString() : '?') + ' 核';
}

// 从一次「一键全部跑分」的结果里抽取条件事实(全部来自既有旁路字段, 不参与计分)
export function condFromOutcome(out: FullRunOutcome): CondFacts {
  const cores: number = BenchRunner.cores();
  const threads: number = BenchRunner.benchMultiThreads(cores > 0 ? cores : 4);
  const panel = BenchRunner.cpuAffinityPanel();
  let topKhz: number = 0;
  let topCores: number = 0;
  let restricted: boolean = panel.restricted;
  // 全机最快频率档: 先取 CS1 项上 native 上报的实测值, 取不到再退回可用核集合快照
  const all: Gb7Result[] = out.gb7Single.concat(out.gb7Multi).concat(out.gb7Gpu);
  for (const r of all) {
    if (r.cpuMachineTopTierKhz > topKhz) {
      topKhz = r.cpuMachineTopTierKhz;
      topCores = r.cpuMachineTopTierCores;
    }
    if (r.cpuFastClusterFallback) {
      restricted = true;
    }
  }
  const snap = BenchRunner.benchCpuAffinity();
  if (topKhz <= 0 && snap !== null) {
    topKhz = snap.machineTopTierKhz;
    topCores = snap.machineTopTierCores;
  }
  // 多核阶段实际用到的核数: 取 native 上报的 cpuThreadsEffective 的最大值
  let used: number = 0;
  for (const r of out.gb7Multi) {
    if (r.cpuThreadsEffective > used) {
      used = r.cpuThreadsEffective;
    }
  }
  if (used <= 0 && out.own !== null) {
    for (const t of out.own.multiTests) {
      const p: number = Math.round(t.parallelism);
      if (p > used) {
        used = p;
      }
    }
  }
  const f: CondFacts = {
    cores: cores,
    topKhz: topKhz,
    topCores: topCores,
    threads: threads,
    used: used,
    restricted: restricted,
    source: panel.available && panel.copyText.length > 0
      ? panel.copyText
      : '（native 未上报允许核集合：旧版 .so 或 cpuAffinityProbe 解析失败）'
  };
  return f;
}

// 只有自研套件(单次完整跑分)时, 条件事实只能来自实测的并行度
export function condFromRecord(rec: BenchRecord): CondFacts {
  const cores: number = BenchRunner.cores();
  const threads: number = BenchRunner.benchMultiThreads(cores > 0 ? cores : 4);
  const panel = BenchRunner.cpuAffinityPanel();
  const snap = BenchRunner.benchCpuAffinity();
  let used: number = 0;
  for (const t of rec.multiTests) {
    const p: number = Math.round(t.parallelism);
    if (p > used) {
      used = p;
    }
  }
  const f: CondFacts = {
    cores: cores,
    topKhz: snap !== null ? snap.machineTopTierKhz : 0,
    topCores: snap !== null ? snap.machineTopTierCores : 0,
    threads: threads,
    used: used,
    restricted: panel.restricted,
    source: panel.available && panel.copyText.length > 0 ? panel.copyText : '（native 未上报允许核集合）'
  };
  return f;
}

// ---- 第一层: 三个大数字 ----
export interface ScoreCard {
  key: string;
  label: string;
  value: string;      // 已排版好的数字文本('--' = 未取到)
  unit: string;
  cond: string;       // 取得条件
  has: boolean;       // false = 本轮没拿到(不是 0 分)
}

// 三个大数字的**唯一口径来源**: key / 标签 / 单位 / 数字排版(num0) / "有没有拿到分" 全在这里。
//  buildCards()(结果页第一层 + 首页「最近一次跑分」预览)与「历史」页的每一张卡片都必须走这一个函数 ——
//  两处各写一遍, 迟早会出现"同一个分数在两页上排版不一样"。
//  cond 留空: 结果页由 buildCards() 补上「取得条件」, 历史页按设计不显示它(卡片要矮)。
export function buildBigThree(single: number, multi: number, gpu: number): ScoreCard[] {
  const out: ScoreCard[] = [];
  out.push({ key: 'single', label: '单核', value: num0(single), unit: '分', cond: '', has: single > 0 });
  out.push({ key: 'multi', label: '多核', value: num0(multi), unit: '分', cond: '', has: multi > 0 });
  out.push({ key: 'gpu', label: 'GPU', value: num0(gpu), unit: '分', cond: '', has: gpu > 0 });
  return out;
}

// ★ 大数字那一层现在是 **CS1**(2026-10 / 10.0 位置对调):
//   取数与 buildKeyRows 里原来那三行 CS1 完全同源(同一个 FullReportBuilder.geoOf),
//   标签 / 单位 / 排版走同一个 buildBigThree —— 不另算一套、也不另起一种写法。
//   只有「一键全跑」有 CS1 数据; 单次完整跑分只跑自研套件, 所以单次跑分的结果浮层
//   仍然用 buildCards()(自研) —— 那里根本不存在 CS1 的数, 不能硬凑。
export function buildCs1Cards(out: FullRunOutcome, c: CondFacts): ScoreCard[] {
  const out3: ScoreCard[] = buildBigThree(
    FullReportBuilder.geoOf(out.gb7Single),
    FullReportBuilder.geoOf(out.gb7Multi),
    FullReportBuilder.geoOf(out.gb7Gpu));
  out3[0].cond = condSingle(c);
  out3[1].cond = condMulti(c);
  out3[2].cond = condGpu(c);
  return out3;
}

export function buildCards(rec: BenchRecord | null, c: CondFacts): ScoreCard[] {
  const single: number = rec !== null ? rec.single : 0;
  const multi: number = rec !== null ? rec.multi : 0;
  const gpu: number = rec !== null ? rec.gpu : 0;
  const out: ScoreCard[] = buildBigThree(single, multi, gpu);
  // 结果页的每一张卡都带「取得条件」; 三个大数字本身与历史页完全同源(同一个 buildBigThree)
  out[0].cond = condSingle(c);
  out[1].cond = condMulti(c);
  out[2].cond = condGpu(c);
  return out;
}

// ---- 第二层: 每个小节的关键得分一行 ----
export interface KeyRow {
  name: string;     // 小节名
  value: string;    // 分值(已排版)
  unit: string;     // 单位 / 口径
  cond: string;     // 取得条件
  has: boolean;
}

// 自研套件(单次完整跑分)的关键得分行 —— 结果浮层与首页预览共用
export function buildOwnKeyRows(rec: BenchRecord | null, c: CondFacts): KeyRow[] {
  const rows: KeyRow[] = [];
  if (rec === null) {
    rows.push({ name: '自研套件总分', value: '--', unit: '分', cond: '本轮未取得', has: false });
    rows.push({ name: '自研套件 · 单核', value: '--', unit: '分', cond: condSingle(c), has: false });
    rows.push({ name: '自研套件 · 多核', value: '--', unit: '分', cond: condMulti(c), has: false });
    rows.push({ name: '自研套件 · GPU', value: '--', unit: '分', cond: condGpu(c), has: false });
    return rows;
  }
  rows.push({
    name: '自研套件总分', value: num0(rec.total), unit: '分',
    cond: '单核 ' + num0(rec.single) + ' · 多核 ' + num0(rec.multi) + ' · GPU ' + num0(rec.gpu),
    has: rec.total > 0
  });
  rows.push({
    name: '自研套件 · 单核', value: num0(rec.single), unit: '分',
    cond: condSingle(c) + ' · 整数 ' + num0(rec.intSingle) + ' / 浮点 ' + num0(rec.floatSingle),
    has: rec.single > 0
  });
  rows.push({
    name: '自研套件 · 多核', value: num0(rec.multi), unit: '分',
    cond: condMulti(c) + ' · 整数 ' + num0(rec.intMulti) + ' / 浮点 ' + num0(rec.floatMulti),
    has: rec.multi > 0
  });
  rows.push({
    name: '自研套件 · GPU', value: num0(rec.gpu), unit: '分',
    cond: condGpu(c) + ' · 场景 ' + rec.gpuScenes.length.toString() + ' 个',
    has: rec.gpu > 0
  });
  return rows;
}

// 一键全部跑分的第二层: 七个小节各一行
export function buildKeyRows(out: FullRunOutcome, c: CondFacts): KeyRow[] {
  const rows: KeyRow[] = [];
  const own: BenchRecord | null = out.own;
  rows.push({
    name: '自研套件总分', value: own !== null ? num0(own.total) : '--', unit: '分',
    cond: own !== null
      ? ('单核 ' + num0(own.single) + ' · 多核 ' + num0(own.multi) + ' · GPU ' + num0(own.gpu))
      : ('本节未取得：' + orUnknown(out.ownError)),
    has: own !== null && own.total > 0
  });
  // ★ 2026-10(10.0)位置对调: 这三行原来是 CS1 —— 现在 CS1 是**上面那三个大数字**,
  //   于是这三行让给自研套件(用户:"下面那六行清单 -> CS1 那三行换成 自研 三行")。
  //   取数照旧只取本轮已有的结果(out.own), 不新建数据源; 排版仍是同一行"名称 + 分数"的规格。
  const os: number = own !== null ? own.single : 0;
  const om: number = own !== null ? own.multi : 0;
  const og: number = own !== null ? own.gpu : 0;
  rows.push({
    name: '自研 · 单核', value: own !== null ? num0(os) : '--', unit: '分',
    cond: own !== null
      ? (condSingle(c) + ' · 整数 ' + num0(own.intSingle) + ' / 浮点 ' + num0(own.floatSingle))
      : ('本节未取得：' + orUnknown(out.ownError)),
    has: os > 0
  });
  rows.push({
    name: '自研 · 多核', value: own !== null ? num0(om) : '--', unit: '分',
    cond: own !== null
      ? (condMulti(c) + ' · 整数 ' + num0(own.intMulti) + ' / 浮点 ' + num0(own.floatMulti))
      : ('本节未取得：' + orUnknown(out.ownError)),
    has: om > 0
  });
  rows.push({
    name: '自研 · GPU', value: own !== null ? num0(og) : '--', unit: '分',
    cond: own !== null
      ? (condGpu(c) + ' · 场景 ' + own.gpuScenes.length.toString() + ' 个')
      : ('本节未取得：' + orUnknown(out.ownError)),
    has: og > 0
  });
  // GPU-SNL(Aurora Nomad Light): native 侧 scored=false / gb7Item=false —— 单独一行, 标注"不计分"。
  // 标定状态原样带出: UNCALIBRATED 时必须写明"绝对值不可当 3DMark 分数用, 只可比同设备比值"。
  const sn: SnRunResult | null = out.gpuSnl;
  if (sn !== null && sn.ok && sn.prepError.length === 0) {
    const uncal: string = (sn.calibrationStatus.length > 0 && sn.calibrationStatus !== 'CALIBRATED')
      ? (' · 标定 ' + sn.calibrationStatus +
        '（绝对值不可当 3DMark 分数用, 只可比同设备两次运行的比值）')
      : (sn.calibrationStatus.length > 0 ? (' · 标定 ' + sn.calibrationStatus) : '');
    rows.push({
      name: 'GPU-SNL · Nomad Light', value: num1(sn.score), unit: '分（不计分）',
      cond: '不计分（native scored=false）· ' + num2(sn.fps) + ' FPS · ' +
        num2(sn.throughputMpx) + ' Mpx/s · ' + (sn.verdict.length > 0 ? sn.verdict : '—') +
        ' · 可信度 ' + (sn.credibility.length > 0 ? sn.credibility : '—') +
        ' · benchVersion ' + sn.benchVersion.toString() + uncal,
      has: sn.score > 0
    });
  } else {
    rows.push({
      name: 'GPU-SNL · Nomad Light', value: '--', unit: '分（不计分）',
      cond: '不计分 · ' + (sn !== null
        ? ('未跑成：' + FullReportBuilder.snFailReason(sn))
        : orUnknown(out.gpuSnlError)),
      has: false
    });
  }

  // NPU: native 侧 scored=false, 标注"不计分", 不并进任何复合分
  if (out.npuRun !== null && out.npuRun.ok) {
    rows.push({
      name: 'NPU 推理', value: num2(out.npuRun.goPs), unit: 'GOPs/s',
      cond: '不计分 · ' + orUnknown(out.npuRun.opSetName) + ' · 实测 ' +
        out.npuRun.measuredRuns.toString() + ' 次(预热 ' + out.npuRun.warmupRuns.toString() +
        ') · 均值 ' + num2(out.npuRun.meanMs) + ' ms',
      has: out.npuRun.goPs > 0
    });
  } else {
    rows.push({
      name: 'NPU 推理', value: '--', unit: 'GOPs/s',
      cond: '不计分 · ' + (out.npuRun !== null ? orUnknown(out.npuRun.status) : orUnknown(out.npuError)),
      has: false
    });
  }
  // 存储: 主值 = 顺序写; 条件里给读 / 随机读 IOPS
  if (out.storage !== null && out.storage.ok) {
    rows.push({
      name: '存储 I/O · 顺序写', value: num1(out.storage.seqWriteDurableMbps), unit: 'MiB/s',
      cond: '不计分 · 顺序读 ' + num1(out.storage.seqReadColdMbps) + ' MiB/s · 4K 随机读 ' +
        num1(out.storage.randRead4kIops) + ' IOPS · 项 ' +
        out.storage.items.length.toString() + ' 个',
      has: out.storage.seqWriteDurableMbps > 0
    });
  } else {
    rows.push({
      name: '存储 I/O · 顺序写', value: '--', unit: 'MiB/s',
      cond: '不计分 · ' + (out.storage !== null
        ? ('未跑成：' + (out.storage.fatal.length > 0 ? out.storage.fatal : out.storage.skipReason))
        : orUnknown(out.storageError)),
      has: false
    });
  }
  return rows;
}

// ---- 第三层: 逐项明细(默认收起) ----
export interface DetailRow {
  name: string;
  score: string;   // 得分(已排版)
  meta: string;    // metric / unit / 耗时 / 并行度 / 运行时频率 / 落核 / diag
  ok: boolean;     // false = 失败或未取得
  scored: boolean; // false = 按政策不计分(native scored=false)
}

export interface DetailGroup {
  title: string;
  rows: DetailRow[];
}

function ownDetail(t: TestResult): DetailRow {
  let meta: string = '绝对吞吐 ' + metricOrDash(t.metric) + ' ' + t.unit + ' · 耗时 ' +
    num0(t.ms) + ' ms · 并行度 ' + num1(t.parallelism) + ' 核';
  // ---- 运行时频率 / 落核 / 快簇(2026-10 补齐, 与 CS1 那一侧同一套字段) ----
  //   以前自研套件这一栏只有一行吞吐与耗时 —— 于是"这一项跑在哪颗核上"在结果页上完全看不见,
  //   而"两台同芯片设备自研单核差 37.7%、CS1 单核只差 0.24%"这件事的根因正藏在这里。
  meta += '\n运行时频率: ' + (t.runFreq.length > 0 ? t.runFreq : '未上报（老 native / 本项没采到样本）');
  meta += '\n落核 cpu=' + (t.cpu >= 0 ? t.cpu.toString() : '未知') +
    ' · 核最高频 ' + khzText(t.cpuMaxKhz) +
    ' · 位次 ' + t.cpuRank.toString() +
    ' · 已绑到生效快簇 ' + (t.cpuBound ? '是' : '否') +
    ' · 单核绑核目标 ' + t.cpuSingleTargetCores.toString() + ' 核';
  meta += ' · 快簇核数 ' + t.cpuFastClusterCores.toString() +
    '（最高 ' + khzText(t.cpuFastClusterMaxKhz) + '，本项在簇内 ' +
    (t.cpuInFastCluster ? '是' : '否') + '）' +
    ' · 允许核集合 ' + t.cpuAllowedCount.toString() + ' 核';
  meta += ' · 全机最快档 ' + khzText(t.cpuMachineTopTierKhz) + '(' +
    t.cpuMachineTopTierCores.toString() + ' 核)' +
    (t.cpuInMachineTopTier ? '（本项在最快档内）' : '（本项不在最快档内）');
  if (t.cpuInfo.length > 0) {
    meta += '\n绑核取证: ' + t.cpuInfo;
  }
  const r: DetailRow = {
    name: t.name,
    score: num0(t.score),
    meta: meta,
    ok: t.score > 0,
    scored: true
  };
  return r;
}

function gpuSceneDetail(g: GpuSceneResult): DetailRow {
  // GPU 跑满判据: 这一栏必须对每一项都有交代 —— 留空会被读成"跑满了"。
  // 自研套件 GPU 场景用的是另一套渲染器与"固定帧预算"循环, 本轮的双档探测判据
  // (gpu7Fullness) 只覆盖 CS1 GPU 11 项, 所以这里如实写"本项不适用"并给出原因。
  const r: DetailRow = {
    name: g.scene,
    score: num0(g.score),
    meta: metricOrDash(g.metric) + ' ' + g.unit + ' · ' + num1(g.fps) + ' FPS · ' +
      num2(g.frameMs) + ' ms/帧' +
      '\nGPU 跑满判据 -> 本项不适用（原因：自研套件 GPU 场景走的是另一套渲染器与' +
      '“固定帧预算”循环，本轮新加的双档探测判据只覆盖 CS1 GPU 11 项）',
    ok: g.score > 0,
    scored: true
  };
  return r;
}

function gb7Detail(r: Gb7Result, threads: number): DetailRow {
  let meta: string = '绝对吞吐 ' + metricOrDash(r.metric) + ' ' + r.unit + ' · 耗时 ' + num0(r.ms) + ' ms';
  meta += ' · 并行度 ' + num1(r.cpuThreadsEffective > 0 ? r.cpuThreadsEffective : threads) + ' 核';
  meta += ' · 请求线程 ' + r.cpuThreadsRequested.toString() + ' / 实际集合 ' +
    r.cpuThreadsEffective.toString();
  // 运行时频率: native 旁路字段原文, 没有就写"未上报"(不编造)
  meta += '\n运行时频率: ' + (r.runFreq.length > 0 ? r.runFreq : '未上报');
  // 落核与快簇
  meta += '\n落核 cpu=' + (r.cpu >= 0 ? r.cpu.toString() : '未知') +
    ' · 核最高频 ' + khzText(r.cpuMaxKhz) +
    ' · 快簇核数 ' + r.cpuFastClusterCores.toString() +
    ' · 允许核集合 ' + r.cpuAllowedCount.toString() + ' 核';
  meta += ' · 全机最快档 ' + khzText(r.cpuMachineTopTierKhz) +
    (r.cpuInMachineTopTier ? '（本项在最快档内）' : '（本项不在最快档内）');
  if (r.cpuInfo.length > 0) {
    meta += '\n' + r.cpuInfo;
  }
  if (r.cpuBoundReason.length > 0) {
    meta += '\n绑核说明: ' + r.cpuBoundReason;
  }
  // ---- GPU「跑满判据」一行(2026-10 追加; 只有 CS1 GPU 11 项有这一行) ----
  //   结论只有三种: 已达到 / 未达到(还差多少) / 本项不适用(为什么)。原样搬 native 的文本。
  if (r.gpuFullness.length > 0) {
    meta += '\n' + r.gpuFullness;
  }
  if (r.diag.length > 0) {
    meta += '\ndiag: ' + r.diag;
  }
  // ---- 可重复性 / 离散度(纯展示; 没有数据就写"没有", 不写"稳定") ----
  // 与 GB7 卡片、报告正文同一套口径: 界面上必须能看出"这个数稳不稳",
  // 只跑 1 轮时写"没有离散度数据, 不能据此判断可信度"。
  const rep: string = BenchRunner.repeatLine(r);
  meta += '\n可重复性: ' + (rep.length > 0 ? rep
    : '未上报（老 native / 该路径没有这一块），不能据此判断可信度');
  meta += ' · 中位分 ' + (r.scoreMedian > 0 ? num1(r.scoreMedian) : '—') +
    ' · 有效轮数 ' + r.roundsOk.toString() + '/' + r.rounds.toString();
  if (BenchRunner.repeatBad(r)) {
    meta += '\n本项重复性差, 该数字仅供参考（相对离散度 ' + num2(r.scoreSpreadPct) +
      '% > 5%；全部轮次的原始值见报告里的 repeatability.roundsDetail）';
  }
  const d: DetailRow = {
    name: r.name,
    score: num0(r.score),
    meta: meta,
    ok: r.score > 0,
    scored: r.scoredByPolicy
  };
  return d;
}

export function buildDetailGroups(out: FullRunOutcome): DetailGroup[] {
  const groups: DetailGroup[] = [];
  const cores: number = BenchRunner.cores();
  const threads: number = BenchRunner.benchMultiThreads(cores > 0 ? cores : 4);

  const own: BenchRecord | null = out.own;
  if (own !== null) {
    const g1: DetailGroup = { title: '自研套件 · 单核（' + own.tests.length.toString() + ' 项）', rows: [] };
    for (const t of own.tests) {
      g1.rows.push(ownDetail(t));
    }
    groups.push(g1);
    const g2: DetailGroup = {
      title: '自研套件 · 多核（' + own.multiTests.length.toString() + ' 项 · 线程 ' +
        threads.toString() + '）', rows: []
    };
    for (const t of own.multiTests) {
      g2.rows.push(ownDetail(t));
    }
    groups.push(g2);
    const g3: DetailGroup = { title: '自研套件 · GPU 场景（' + own.gpuScenes.length.toString() + ' 项）', rows: [] };
    for (const s of own.gpuScenes) {
      g3.rows.push(gpuSceneDetail(s));
    }
    groups.push(g3);
  }

  const mk = (title: string, list: Gb7Result[], th: number): DetailGroup => {
    const g: DetailGroup = { title: title + '（' + list.length.toString() + ' 项）', rows: [] };
    for (const r of list) {
      g.rows.push(gb7Detail(r, th));
    }
    return g;
  };
  groups.push(mk('CS1 · 单核', out.gb7Single, 1));
  groups.push(mk('CS1 · 多核', out.gb7Multi, threads));
  groups.push(mk('CS1 · GPU', out.gb7Gpu, 1));

  // GPU-SNL(不计分, 单独成组)
  const gsn: DetailGroup = {
    title: 'GPU-SNL · Aurora Nomad Light（native scored=false / gb7Item=false · 不计分）', rows: []
  };
  const snr: SnRunResult | null = out.gpuSnl;
  if (snr !== null) {
    if (snr.prepError.length > 0) {
      gsn.rows.push({
        name: 'GPU-SNL 初始化（prepare）', score: '--',
        meta: '驱动原文（原样）：' + snr.prepError + '\n' +
          '本节按失败记录, 但整轮继续执行（后续小节不受影响）。',
        ok: false, scored: false
      });
    } else {
      gsn.rows.push({
        name: 'Aurora Nomad Light', score: num1(snr.score),
        meta: '分（不计分）· ' + num2(snr.fps) + ' FPS · 平均帧 ' + num2(snr.meanFrameMs) +
          ' ms · 吞吐 ' + num2(snr.throughputMpx) + ' Mpx/s · 每帧像素 ' +
          num0(snr.pixelsPerFrame) + '\n' +
          '自证 ' + (snr.verdict.length > 0 ? snr.verdict : '—') + '（conclusive=' +
          (snr.gpuBoundConclusive ? 'true' : 'false') + '）· 每帧 draw call ' +
          num0(snr.drawCallsPerFrame) + ' · CPU 顶点数据 ' +
          (snr.cpuVertexDataBytesPerFrame >= 0 ? num0(snr.cpuVertexDataBytesPerFrame) : '—') +
          ' 字节/帧（应为 0）· 提交占空 ' + num2(snr.submissionDuty) + '\n' +
          '标定 ' + (snr.calibrationStatus.length > 0 ? snr.calibrationStatus : '—') +
          '（k=' + num2(snr.kScorePerFps) + '）' +
          (snr.calibrationStatus.length > 0 && snr.calibrationStatus !== 'CALIBRATED'
            ? '：绝对值未经标定, 不可当 3DMark 分数用, 只可用于同设备两次运行的比值' : '') + '\n' +
          '自适应趟数 ' + num0(snr.passesPerFrame) + '（依据 ' +
          (snr.scaleSource.length > 0 ? snr.scaleSource : '—') + '）· 标尺 CPU ' +
          num0(snr.cpuLogical) + ' 逻辑核 / ' + num0(snr.cpuPhysical) + ' 物理核 · SMT ' +
          (snr.smtEnabled ? '开' : '关') + ' · SIMD arch ' + snr.simdArch.toString() +
          (snr.simdNeon ? ' +NEON' : '') + (snr.simdSve ? ' +SVE' : '') +
          (snr.simdSve2 ? ' +SVE2' : '') + (snr.simdI8mm ? ' +I8MM' : '') + '\n' +
          '可重复性 ' + (snr.credibility.length > 0 ? snr.credibility : '—') + ' · 离散度 ' +
          num2(snr.dispersion) + '% · 轮数 ' + snr.repeats.toString() + ' · 环境 ' +
          (snr.envState.length > 0 ? snr.envState : '—') + ' · 温度中位 ' + num1(snr.tempMedian) +
          ' degC · 频率中位 ' + num0(snr.cpuKhzMedian / 1000) + ' MHz · benchVersion ' +
          snr.benchVersion.toString() + '\n' +
          '真实用途：' + (snr.realUseShort.length > 0 ? snr.realUseShort : '—') +
          (snr.glRenderer.length > 0 ? ('\nGL 渲染器：' + snr.glRenderer) : ''),
        ok: snr.ok, scored: false
      });
      for (const w of snr.warnings) {
        gsn.rows.push({ name: 'GPU-SNL warning', score: '--', meta: w, ok: false, scored: false });
      }
      if (snr.risks.length > 0) {
        gsn.rows.push({ name: 'GPU-SNL risks', score: '--', meta: snr.risks, ok: false, scored: false });
      }
      if (snr.lastError.length > 0) {
        gsn.rows.push({
          name: 'GPU-SNL lastError', score: '--', meta: snr.lastError, ok: false, scored: false
        });
      }
    }
  }
  if (out.gpuSnlError.length > 0) {
    gsn.rows.push({
      name: 'GPU-SNL 小节结论', score: '--', meta: out.gpuSnlError, ok: false, scored: false
    });
  }
  if (gsn.rows.length > 0) {
    groups.push(gsn);
  }

  // NPU(不计分, 单独成组)
  const gn: DetailGroup = { title: 'NPU（native scored=false · 不计分）', rows: [] };
  if (out.npuProbe !== null) {
    gn.rows.push({
      name: 'NPU 设备探测', score: out.npuProbe.devices.length.toString() + ' 设备',
      meta: out.npuProbe.text.length > 0 ? out.npuProbe.text : orUnknown(out.npuProbe.status),
      ok: out.npuProbe.ok, scored: false
    });
  }
  if (out.npuRun !== null) {
    gn.rows.push({
      name: out.npuRun.deviceLabel.length > 0 ? out.npuRun.deviceLabel : 'NPU 推理',
      score: num2(out.npuRun.goPs),
      meta: 'GOPs/s · ' + num2(out.npuRun.macsPerSec / 1000000000) + ' GMAC/s · 均值 ' +
        num2(out.npuRun.meanMs) + ' ms · 中位 ' + num2(out.npuRun.medianMs) + ' ms · 最小 ' +
        num2(out.npuRun.minMs) + ' ms · 最大 ' + num2(out.npuRun.maxMs) + ' ms · 精度 ' +
        orUnknown(out.npuRun.precisionShort) + '\n' +
        (out.npuRun.text.length > 0 ? out.npuRun.text : orUnknown(out.npuRun.status)),
      ok: out.npuRun.ok, scored: false
    });
  }
  if (out.npuError.length > 0) {
    gn.rows.push({ name: 'NPU 小节结论', score: '--', meta: out.npuError, ok: false, scored: false });
  }
  if (gn.rows.length > 0) {
    groups.push(gn);
  }

  // 存储(不计分, 单独成组)
  const gst: DetailGroup = { title: '存储 I/O（native scored=false · 不计分）', rows: [] };
  if (out.storage !== null) {
    for (const it of out.storage.items) {
      gst.rows.push({
        name: it.name,
        score: it.iops > 0 ? num1(it.iops) : num1(it.mbps),
        meta: (it.iops > 0 ? 'IOPS' : 'MiB/s') + ' · 耗时 ' + num0(it.ms) + ' ms · 字节 ' +
          it.bytes.toString() + ' · 模式 ' + orUnknown(it.mode) +
          (it.error.length > 0 ? ('\n错误: ' + it.error) : '') +
          (it.note.length > 0 ? ('\n说明: ' + it.note) : ''),
        ok: it.ok, scored: false
      });
    }
  } else if (out.storageError.length > 0) {
    gst.rows.push({ name: '存储小节结论', score: '--', meta: out.storageError, ok: false, scored: false });
  }
  if (gst.rows.length > 0) {
    groups.push(gst);
  }
  return groups;
}

// 明细总项数(折叠标题上用)
export function detailCount(groups: DetailGroup[]): number {
  let n: number = 0;
  for (const g of groups) {
    n += g.rows.length;
  }
  return n;
}
