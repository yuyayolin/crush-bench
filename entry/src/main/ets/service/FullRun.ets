import fs from '@ohos.file.fs';
import { common } from '@kit.AbilityKit';
import {
  BenchRunner, ProgressInfo, GpuProgress, NpuProbeResult, NpuRunResult,
  StorageRunResult, StorageItemRow, SmtCapabilities, CpuAffinityPanelData,
  SnRunResult,
  smtTopologyLine } from './BenchRunner';
import { BenchRecord, TestResult, GpuSceneResult, Gb7Result } from '../model/BenchModel';
// metric(吞吐)的显示口径: 与 native 的 "%.4g" 同口径的 4 位有效数字(见 common/NumberFmt.ets)。
// 报告正文里的 metric= 一律走 metricUnrun —— 它是 numOrDash(v, 4) 的有效数字版:
//   NaN -> '—'(native 给了空串), 负数 -> '—（未跑）'(native 用 -1 表示没跑), 其余 4 位有效数字。
// 旧写法 numOrDash(r.metric, 4) 是"小数位=4": 0.4831 够用, 但 226.04 会印成 '226.0400' —— 口径不一致。
import { metricUnrun } from '../common/NumberFmt';
import { DeviceProfile } from './DeviceProfile';
import { CrashGuard } from './CrashGuard';
import { RunLog } from './RunLog';
// 参考对照 / 线性-可比性审计(libauroraref.so, 只读): 把本轮的旁路量传进去(见 RefAudit.ets)
import { RefAuditInput, RefAuditResult, runRefAudit, extractObject, looksLikeJsonObject } from './RefAudit';
// 「本项是否真的随芯片性能变化」的自检(纯 ArkTS, 只用本轮已有数据; 不跑负载、不改任何负载)
import { auditSingleStage, auditMultiStage, summarize, isComparable, SC_FAIL, SC_PASS,
  SC_UNPROVEN, ScalingSummary } from './ScalingAudit';
// 报告 .txt 的 UTF-8 合法性守卫(2026-10 真机报告缺陷 4): 孤立代理项 -> U+FFFD,
// 并把"替换了几个、第一处在哪"写进报告正文; 写盘处另有一道防御性断言。
// 口径与现场证据(偏移 546380 的 12 个非法字节 = 4 个 CESU-8 序列)见 common/Utf8Guard.ets 的文件头。
import { checkReportText, sanitizeReportText, assertTxtUtf8Safe, Utf8Check } from '../common/Utf8Guard';

// ===========================================================================
// 「一键全部跑分」—— 编排 + 报告
//
// 本文件只做编排、记录与报告, 一行都不碰:
//   k / conv / 计分公式 / metric 口径 / 单位 / 任何负载的工作量与尺寸 / 线程数与绑核策略。
//   所有负载都通过 BenchRunner 既有的静态方法原样调用(参数与单跑时完全一致)。
//
// 执行顺序(一次点击跑完):
//   1) 自研套件(单核 n + 多核 n + GPU m)  -> BenchRunner.runAll()
//   2) CS1 单核 16 项                      -> BenchRunner.runGb7All(1, …)
//   3) CS1 多核 8 项                       -> BenchRunner.runGb7Multi(threads, …)
//   4) CS1 GPU 11 项                       -> BenchRunner.runGb7Gpu(…)
//   5) GPU-SNL(Aurora Nomad Light)         -> BenchRunner.runSn()（独立模块 libaurorasn.so）
//   6) NPU 小节: probeNpu() -> runNpu(scale, warmup, iters, budgetMs)
//   7) 存储 I/O 小节: storagePrepare(filesDir) -> runStorage('') -> 无论如何 storageCleanupNow()
//
// GPU-SNL / NPU / 存储不并入 CS1 / 自研套件的复合分: native 侧本身就是 scored:false,
// 报告里各自单独成节, 并注明"不计分及原因"; 任一小节失败都只记录原因并继续下一节。
//
// 任一小节失败不中断整轮: 记录失败原因后继续下一节。
// 中断(用户点停止)只在"项与项之间 / 节与节之间"生效: 已得的结果照常出报告,
// 并注明"未跑完"以及从哪一节开始没执行。
// ===========================================================================

// ---- 小节 id(顺序即执行顺序) ----
export const SEC_OWN: string = 'own';
export const SEC_GB7S: string = 'gb7s';
export const SEC_GB7M: string = 'gb7m';
export const SEC_GB7G: string = 'gb7g';
// GPU-SNL(Aurora Nomad Light): 独立小节, 排在 CS1 GPU 之后、NPU 之前; native 侧 scored=false
export const SEC_SN: string = 'sn';
export const SEC_NPU: string = 'npu';
export const SEC_STORAGE: string = 'storage';

export const ST_PENDING: string = '未开始';
export const ST_RUNNING: string = '进行中';
export const ST_DONE: string = '已完成';
export const ST_FAILED: string = '失败';
export const ST_SKIPPED: string = '已跳过（停止后未执行）';

// NPU 小节参数: native 文档里的默认档(中 / 2 次预热 / 10 次计时 / 1200ms 预算)。
// 原样透传, native 自己把越界值夹回合法区间并回写生效值。
export const NPU_SCALE: number = 1;
export const NPU_WARMUP: number = 2;
export const NPU_ITERS: number = 10;
export const NPU_BUDGET_MS: number = 1200;
// 存储小节在"进度条总量"里的名义单位数(真实项数以 native 返回的 items 为准)
const STORAGE_NOMINAL_UNITS: number = 10;

function sleepMs(ms: number): Promise<void> {
  return new Promise<void>((resolve: () => void) => setTimeout(resolve, ms));
}

function repeat(ch: string, n: number): string {
  let s: string = '';
  for (let i = 0; i < n; i++) {
    s += ch;
  }
  return s;
}

function pad2(n: number): string {
  return n < 10 ? ('0' + n.toString()) : n.toString();
}

function stampFull(ts: number): string {
  const d = new Date(ts);
  return d.getFullYear().toString() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) + ' ' +
    pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
}

function stampFile(ts: number): string {
  const d = new Date(ts);
  return d.getFullYear().toString() + pad2(d.getMonth() + 1) + pad2(d.getDate()) + '-' +
    pad2(d.getHours()) + pad2(d.getMinutes()) + pad2(d.getSeconds());
}

function durText(ms: number): string {
  if (ms < 1000) {
    return ms.toFixed(0) + ' ms';
  }
  const totalSec: number = Math.floor(ms / 1000);
  const h: number = Math.floor(totalSec / 3600);
  const m: number = Math.floor((totalSec % 3600) / 60);
  const s: number = totalSec % 60;
  let t: string = '';
  if (h > 0) {
    t += h.toString() + ' 小时 ';
  }
  if (h > 0 || m > 0) {
    t += m.toString() + ' 分 ';
  }
  t += s.toString() + ' 秒';
  return ms.toFixed(0) + ' ms（' + t + '）';
}

// native 用 -1 表示"该项没跑/无此读数", 用 0 表示"读到了但为 0" —— 报告里分开写
function numOrDash(v: number, digits: number): string {
  if (v !== v) {
    return '—';
  }
  if (v < 0) {
    return '—（未跑）';
  }
  return v.toFixed(digits);
}

function mhzText(khz: number): string {
  if (khz <= 0) {
    return '—';
  }
  return (khz / 1000).toFixed(0) + ' MHz';
}

// GPU-SNL 用: -1 = native 没给这个读数(与"读到了 0"区分开; 例如 cpuVertexDataBytesPerFrame 应为 0)
function snNum(v: number, digits: number): string {
  if (v !== v) {
    return '—';
  }
  if (v < 0) {
    return '—（无读数）';
  }
  return v.toFixed(digits);
}

// GPU-SNL 用: native 的枚举字符串, 空串写成"(native 未给)"
function snEnum(s: string): string {
  return s.length > 0 ? s : '(native 未给)';
}

// GPU-SNL 的标定口径: 只写"未标定"这一条事实, 不做任何换算
function snCalibrationLine(status: string, k: number): string {
  if (status.length === 0) {
    return '标定状态：(native 未给)';
  }
  if (status === 'CALIBRATED') {
    return '标定状态：CALIBRATED（k = ' + k.toFixed(4) + '）';
  }
  return '标定状态：' + status + '（k = ' + k.toFixed(4) + '）—— 绝对值未经标定, ' +
    '不可当 3DMark 分数用, 只可用于同设备两次运行的比值。';
}

// ===========================================================================
// 进度(UI 只按"每项一次"的节奏收到回调; 界面上没有任何动画)
// ===========================================================================
export interface FullRunProgress {
  sectionId: string;
  sectionTitle: string;
  sectionIndex: number;      // 1 起
  sectionTotal: number;      // 6
  itemName: string;          // 当前项名(静态文字)
  itemIndex: number;         // 本节内第几项(1 起; 0 = 小节级事件)
  itemTotal: number;         // 本节总项数
  doneItems: number;         // 整轮已完成项数
  totalItems: number;        // 整轮总项数(确定值进度条的分母)
  doneSections: number;      // 已完成小节数
  lastMs: number;
  line: string;              // 刚完成项的一行紧凑摘要(空串 = 本节级事件)
  sectionDone: boolean;      // 本节是否刚刚结束
  sectionNote: string;       // 本节结束时的结论一行
}

// ===========================================================================
// 小节状态与整轮结果
// ===========================================================================
export class FullRunSection {
  id: string = '';
  title: string = '';
  status: string = ST_PENDING;
  itemDone: number = 0;
  itemTotal: number = 0;
  elapsedMs: number = 0;
  note: string = '';

  constructor(id: string, title: string) {
    this.id = id;
    this.title = title;
  }
}

export class FullRunOutcome {
  startedAt: number = 0;
  finishedAt: number = 0;
  interrupted: boolean = false;
  stopReason: string = '';
  sections: FullRunSection[] = [];

  own: BenchRecord | null = null;
  ownError: string = '';

  gb7Single: Gb7Result[] = [];
  gb7Multi: Gb7Result[] = [];
  gb7Gpu: Gb7Result[] = [];

  // GB7 两个 CPU 小节各自的耗时与多核线程数 —— 只用于在小节 note 里重算"综合分"那一行
  // (多核阶段跑完会回填单核那一行, 保证三处口径一致; 见 refreshGb7Notes)。
  gb7SingleElapsedMs: number = 0;
  gb7MultiElapsedMs: number = 0;
  gb7Threads: number = 0;

  gpuSnl: SnRunResult | null = null;
  gpuSnlError: string = '';

  npuProbe: NpuProbeResult | null = null;
  npuRun: NpuRunResult | null = null;
  npuError: string = '';

  storageInfo: string = '';
  storage: StorageRunResult | null = null;
  storageError: string = '';
  storagePrepared: boolean = false;
  storageCleanupOk: boolean = false;

  reportChars: number = 0;
  jsonChars: number = 0;
  reportTextPath: string = '';
  reportJsonPath: string = '';
  reportLatestTextPath: string = '';
  reportLatestJsonPath: string = '';
  reportSaveError: string = '';
  // 历史成绩.txt(下载目录里那份人能直接看的成绩单)本轮的写入结果 —— 一句话。
  // 它在报告生成之前就被填好, 所以报告正文的「落盘回执」一段能如实印出来(失败也印, 不静默)。
  scoreHistoryStatus: string = '';

  elapsedMs(): number {
    const end: number = this.finishedAt > 0 ? this.finishedAt : Date.now();
    return end - this.startedAt;
  }

  sectionById(id: string): FullRunSection | null {
    for (const s of this.sections) {
      if (s.id === id) {
        return s;
      }
    }
    return null;
  }

  // 报告里数出来的"实际拿到的项数"
  itemCount(): number {
    let n: number = this.gb7Single.length + this.gb7Multi.length + this.gb7Gpu.length;
    if (this.own !== null) {
      n += this.own.tests.length + this.own.multiTests.length + this.own.gpuScenes.length;
    }
    if (this.gpuSnl !== null) {
      n++;
    }
    if (this.npuProbe !== null) {
      n++;
    }
    if (this.npuRun !== null) {
      n++;
    }
    if (this.storage !== null) {
      n += this.storage.items.length;
    }
    return n;
  }
}

// ===========================================================================
// 编排器
// ===========================================================================
export class FullRunSession {
  private stopped: boolean = false;
  private running: boolean = false;
  private totalItems: number = 0;
  private doneItems: number = 0;
  private doneSections: number = 0;

  isRunning(): boolean {
    return this.running;
  }

  // 请求停止: 只置一个标志。正在跑的那一项会跑完, 之后在项与项 / 节与节之间收工。
  // 为什么不强杀: 负载跑一半被打断会得到一个不可比的中途数, 还不如让它跑完这一项。
  stop(): void {
    this.stopped = true;
  }

  stopRequested(): boolean {
    return this.stopped;
  }

  async run(onProgress: (p: FullRunProgress) => void): Promise<FullRunOutcome> {
    this.running = true;
    this.stopped = false;
    const out: FullRunOutcome = new FullRunOutcome();
    out.startedAt = Date.now();
    out.sections = [
      new FullRunSection(SEC_OWN, '自研套件（单核 + 多核 + GPU 场景）'),
      new FullRunSection(SEC_GB7S, 'CS1 · 单核'),
      new FullRunSection(SEC_GB7M, 'CS1 · 多核'),
      new FullRunSection(SEC_GB7G, 'CS1 · GPU'),
      new FullRunSection(SEC_SN, 'GPU-SNL · Aurora Nomad Light（不计分小节）'),
      new FullRunSection(SEC_NPU, 'NPU（AI 加速器）小节'),
      new FullRunSection(SEC_STORAGE, '存储 I/O 小节')
    ];
    out.sections[0].itemTotal = BenchRunner.ownItemCount();
    out.sections[1].itemTotal = BenchRunner.gb7ItemCount();
    out.sections[2].itemTotal = BenchRunner.gb7MultiItemCount();
    out.sections[3].itemTotal = BenchRunner.gb7GpuItemCount();
    out.sections[4].itemTotal = BenchRunner.snItemCount();
    out.sections[5].itemTotal = 2;
    out.sections[6].itemTotal = STORAGE_NOMINAL_UNITS;
    this.totalItems = 0;
    for (const s of out.sections) {
      this.totalItems += s.itemTotal;
    }
    this.doneItems = 0;
    this.doneSections = 0;

    const n: number = out.sections.length;
    for (let i = 0; i < n; i++) {
      const sec: FullRunSection = out.sections[i];
      if (this.stopped) {
        sec.status = ST_SKIPPED;
        sec.note = '用户点了「停止」：本小节没有开始。';
        continue;
      }
      sec.status = ST_RUNNING;
      const t0: number = Date.now();
      onProgress(this.mkProgress(out, sec, i, 0, 0, '正在开始…', '', false, ''));
      try {
        await this.runSection(out, sec, i, onProgress);
        if (sec.status === ST_RUNNING) {
          sec.status = ST_DONE;
        }
      } catch (e) {
        // 单节失败不中断整轮: 记下原因, 继续下一节
        sec.status = ST_FAILED;
        sec.note = '本小节抛出异常（已记录并继续下一节）：' + e.message;
      }
      sec.elapsedMs = Date.now() - t0;
      this.doneSections++;
      onProgress(this.mkProgress(out, sec, i, sec.itemDone, 0, sec.title, '', true, sec.note));
    }

    out.finishedAt = Date.now();
    out.interrupted = this.stopped;
    if (this.stopped) {
      out.stopReason = '用户点了「停止」：当前项跑完后收工，本轮未跑完。';
    }
    this.running = false;
    return out;
  }

  private mkProgress(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    itemIndex: number, lastMs: number, itemName: string, line: string,
    sectionDone: boolean, sectionNote: string): FullRunProgress {
    const p: FullRunProgress = {
      sectionId: sec.id,
      sectionTitle: sec.title,
      sectionIndex: secIdx + 1,
      sectionTotal: out.sections.length,
      itemName: itemName,
      itemIndex: itemIndex,
      itemTotal: sec.itemTotal,
      doneItems: this.doneItems,
      totalItems: this.totalItems,
      doneSections: this.doneSections,
      lastMs: lastMs,
      line: line,
      sectionDone: sectionDone,
      sectionNote: sectionNote
    };
    return p;
  }

  private async runSection(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    if (sec.id === SEC_OWN) {
      await this.runOwn(out, sec, secIdx, onProgress);
      return;
    }
    if (sec.id === SEC_GB7S) {
      await this.runGb7Single(out, sec, secIdx, onProgress);
      return;
    }
    if (sec.id === SEC_GB7M) {
      await this.runGb7Multi(out, sec, secIdx, onProgress);
      return;
    }
    if (sec.id === SEC_GB7G) {
      await this.runGb7Gpu(out, sec, secIdx, onProgress);
      return;
    }
    if (sec.id === SEC_SN) {
      await this.runSnSection(out, sec, secIdx, onProgress);
      return;
    }
    if (sec.id === SEC_NPU) {
      await this.runNpuSection(out, sec, secIdx, onProgress);
      return;
    }
    if (sec.id === SEC_STORAGE) {
      await this.runStorageSection(out, sec, secIdx, onProgress);
      return;
    }
    sec.status = ST_FAILED;
    sec.note = '未知小节 id：' + sec.id;
  }

  // ---- 1) 自研套件: 单核 n + 多核 n + GPU m(与首页「单次完整跑分」同一个实现) ----
  private async runOwn(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    const tests: number = BenchRunner.ownTestCount();
    const scenes: number = BenchRunner.ownSceneCount();
    const t0: number = Date.now();
    let lastScene: number = -1;
    try {
      const rec: BenchRecord = await BenchRunner.runAll((p: ProgressInfo) => {
        this.doneItems++;
        sec.itemDone++;
        let tag: string = '单核';
        let idx: number = p.index;
        if (p.phase.indexOf('多核') >= 0) {
          tag = '多核';
          idx = p.index - tests;
        }
        const line: string = '[' + secIdx + 1 + '/7] 自研' + tag + ' ' + idx.toString() + '/' +
          tests.toString() + ' · ' + p.testName + ' · ' + p.lastScore.toFixed(0) + ' 分 · ' +
          p.lastMs.toFixed(0) + ' ms';
        onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, p.lastMs, p.testName, line,
          false, ''));
      }, (g: GpuProgress) => {
        // GPU 场景: 只在换场景时发一次静态事件 —— 渲染过程中不刷新(性能护栏: 无持续动画)
        if (g.sceneIndex === lastScene) {
          return;
        }
        lastScene = g.sceneIndex;
        this.doneItems++;
        sec.itemDone++;
        const line: string = '[' + secIdx + 1 + '/7] 自研 GPU ' + (g.sceneIndex + 1).toString() +
          '/' + scenes.toString() + ' · ' + g.sceneName + ' · 离屏渲染中';
        onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, 0, g.sceneName, line,
          false, ''));
      }, '');
      out.own = rec;
      sec.note = '总分 ' + rec.total.toFixed(0) + '（单核 ' + rec.single.toFixed(0) +
        ' / 多核 ' + rec.multi.toFixed(0) + ' / GPU ' + rec.gpu.toFixed(0) + '）· 项数 ' +
        rec.tests.length.toString() + ' 单核 + ' + rec.multiTests.length.toString() +
        ' 多核 + ' + rec.gpuScenes.length.toString() + ' GPU · 耗时 ' + durText(Date.now() - t0);
      RunLog.get().item('一键全跑·自研套件', '自研套件汇总', 1, 1, Date.now() - t0, 0, '',
        rec.total, 0, rec.total > 0, '总分 ' + rec.total.toFixed(0) + ' · 单核 ' +
        rec.single.toFixed(0) + ' · 多核 ' + rec.multi.toFixed(0) + ' · GPU ' + rec.gpu.toFixed(0));
    } catch (e) {
      out.ownError = e.message;
      sec.status = ST_FAILED;
      sec.note = '自研套件运行失败（已记录并继续下一节）：' + e.message;
    }
  }

  // ---- 2) CS1 单核 ----
  private async runGb7Single(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    const n: number = sec.itemTotal;
    const t0: number = Date.now();
    try {
      out.gb7Single = await BenchRunner.runGb7All(1, (r: Gb7Result) => {
        this.doneItems++;
        sec.itemDone++;
        const line: string = '[' + secIdx + 1 + '/7] CS1 单核 ' + sec.itemDone.toString() + '/' +
          n.toString() + ' · ' + r.name + ' · ' + BenchRunner.itemCellText(r) + ' · ' +
          r.ms.toFixed(0) + ' ms';
        onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, r.ms, r.name, line, false, ''));
      });
      out.gb7SingleElapsedMs = Date.now() - t0;
      // 随芯片变化自检(单核口径): 旁路结论, 不改任何 score/metric/unit。
      // 单核阶段只能判 SC2(轮间离散度); SC1(并行加速比)要等多核阶段的数据, 见 runGb7Multi。
      auditSingleStage(out.gb7Single);
      this.refreshGb7Notes(out);
    } catch (e) {
      out.gb7Single = [];
      sec.status = ST_FAILED;
      sec.note = 'CS1 单核运行失败（已记录并继续下一节）：' + e.message;
    }
  }

  // ---- 3) CS1 多核(线程数按 SMT 开关, 与首页多核按钮同一个函数) ----
  private async runGb7Multi(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    const cores: number = BenchRunner.cores();
    const threads: number = BenchRunner.benchMultiThreads(cores > 0 ? cores : 4);
    const t0: number = Date.now();
    try {
      out.gb7Multi = await BenchRunner.runGb7Multi(threads, (r: Gb7Result) => {
        this.doneItems++;
        sec.itemDone++;
        const line: string = '[' + secIdx + 1 + '/7] CS1 多核 ' + sec.itemDone.toString() + '/' +
          sec.itemTotal.toString() + ' · ' + r.name + ' · ' + BenchRunner.itemCellText(r) +
          ' · ' + r.ms.toFixed(0) + ' ms';
        onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, r.ms, r.name, line, false, ''));
      });
      out.gb7MultiElapsedMs = Date.now() - t0;
      out.gb7Threads = threads;
      // 随芯片变化自检(多核口径): SC1(并行加速比 = 多核分/单核分, 工作量完全相同)
      // + SC2(轮间离散度)。跑完之后两个小节的综合分一起重算 —— 这样"小节 note 里的数"
      // 与"报告里的数"永远同口径, 不会再出现 328 与 561 各说各话的情况。
      auditMultiStage(out.gb7Single, out.gb7Multi);
      this.refreshGb7Notes(out);
    } catch (e) {
      out.gb7Multi = [];
      sec.status = ST_FAILED;
      sec.note = 'CS1 多核运行失败（已记录并继续下一节）：' + e.message;
    }
  }

  // ---- 4) CS1 GPU ----
  private async runGb7Gpu(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    const t0: number = Date.now();
    try {
      out.gb7Gpu = await BenchRunner.runGb7Gpu((r: Gb7Result) => {
        this.doneItems++;
        sec.itemDone++;
        const line: string = '[' + secIdx + 1 + '/7] CS1 GPU ' + sec.itemDone.toString() + '/' +
          sec.itemTotal.toString() + ' · ' + r.name + ' · ' + BenchRunner.itemCellText(r) +
          ' · ' + r.ms.toFixed(0) + ' ms';
        onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, r.ms, r.name, line, false, ''));
      });
      const scored: number = this.scoredCount(out.gb7Gpu);
      sec.note = '综合分（已计分项几何平均）' + this.geo(out.gb7Gpu).toFixed(0) + ' · 计分 ' +
        scored.toString() + ' 项 · 未拿到分 ' +
        (out.gb7Gpu.length - scored).toString() + ' 项 · ' +
        FullReportBuilder.gpuFullnessTally(out.gb7Gpu) + ' · 耗时 ' + durText(Date.now() - t0);
    } catch (e) {
      out.gb7Gpu = [];
      sec.status = ST_FAILED;
      sec.note = 'CS1 GPU 运行失败（已记录并继续下一节）：' + e.message;
    }
  }

  // ---- 5) GPU-SNL(Aurora Nomad Light): 独立小节, 排在 CS1 GPU 之后、NPU 之前 ----
  //
  // native 侧: section="GPU-SNL" / scored=false / gb7Item=false, 自带 section 字段, 单独成节。
  // 硬要求: prepare() 返回非空错误串时原样显示驱动原文, 把本节标为失败, 但不中断整轮
  //         (既不在本节里抛异常, 也不改变后面小节的执行)。
  private async runSnSection(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    onProgress(this.mkProgress(out, sec, secIdx, 0, 0, 'GPU-SNL 初始化(离屏 EGL/GLES3)', '', false, ''));
    const t0: number = Date.now();
    try {
      out.gpuSnl = await BenchRunner.runSn();
    } catch (e) {
      // runSn() 自己已经把异常兜成失败结果; 走到这里说明连兜底都出问题了
      out.gpuSnlError = 'GPU-SNL 抛出异常：' + e.message;
      out.gpuSnl = null;
    }
    const runMs: number = Date.now() - t0;
    this.doneItems++;
    sec.itemDone++;

    const r: SnRunResult | null = out.gpuSnl;
    let line: string = '';
    if (r === null) {
      line = '未拿到结果：' + (out.gpuSnlError.length > 0 ? out.gpuSnlError : '本节未执行');
    } else if (r.prepError.length > 0) {
      //  驱动 / EGL 原文原样带出, 一个字都不改
      line = '初始化失败（驱动原文）：' + r.prepError;
    } else if (!r.ok) {
      // 2026-10 真机报告缺陷 3 修: 这里以前是 "native 返回失败：" + (error / lastError /
      //   "（native 未给文本）") —— 于是"JSON 解析失败"这种真实原因被印成"native 未给文本",
      //   而同一份报告里又原样附着一份自称 "ok":true 的原始 JSON, 两者自相矛盾。
      //   现在: 原因一律走 FullReportBuilder.snFailReason()(保证非空且真实), 并在
      //   "原文自称 ok:true 但 JSON 解析不了"时把矛盾显式写出来, 再补一句"本节确实
      //   执行过(耗时 xx ms), 但返回值读不出来 —— 数字一律按未取到处理, 不从损坏的
      //   JSON 里猜一个数"。
      line = FullReportBuilder.snFailReason(r) +
        (r.rawParseOk ? '' :
          (' ｜ 本节 native 侧确实执行过（本节耗时 ' + durText(runMs) +
           '），但返回值读不出来：分数 / 帧率 / 判定一律按未取到处理，不从损坏的 JSON 里猜数。'));
    } else {
      line = 'score=' + snNum(r.score, 1) + ' 分 · fps=' + snNum(r.fps, 2) + ' FPS · 吞吐=' +
        snNum(r.throughputMpx, 2) + ' Mpx/s · ' + snEnum(r.verdict) + ' · 标定=' +
        snEnum(r.calibrationStatus) + ' · 可信度=' + snEnum(r.credibility) +
        ' · benchVersion=' + r.benchVersion.toString();
    }
    const okAll: boolean = (r !== null) && r.ok && r.prepError.length === 0;

    onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, runMs, 'GPU-SNL 跑分',
      '[' + (secIdx + 1).toString() + '/7] GPU-SNL · ' + line, false, ''));

    RunLog.get().item('一键全跑·GPU-SNL', 'Aurora Nomad Light', 1, 1, runMs,
      r !== null ? r.score : 0, r !== null ? '分' : '', 0, 1, okAll, line);

    const calNote: string = (r !== null && r.calibrationStatus.length > 0 &&
      r.calibrationStatus !== 'CALIBRATED')
      ? ' ｜ 标定状态=' + r.calibrationStatus +
        '：绝对值未经标定, 不可当 3DMark 分数用, 只可用于同设备两次运行的比值。'
      : '';
    sec.note = line + calNote +
      ' ｜ 本小节 scored=false（native 标记），不并入 CS1 / 自研套件复合分。';
    if (!okAll) {
      sec.status = ST_FAILED;
    }
  }

  // ---- 6) NPU: 先探测, 再跑分 ----
  private async runNpuSection(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    onProgress(this.mkProgress(out, sec, secIdx, 0, 0, 'NPU 设备探测', '', false, ''));
    try {
      out.npuProbe = await BenchRunner.probeNpu();
    } catch (e) {
      out.npuError = 'NPU 探测抛出异常：' + e.message;
      out.npuProbe = null;
    }
    this.doneItems++;
    sec.itemDone++;
    let probeText: string = '';
    if (out.npuProbe !== null) {
      probeText = '探测 status=' + out.npuProbe.status + ' · ' +
        (out.npuProbe.text.length > 0 ? out.npuProbe.text
          : (out.npuProbe.error.length > 0 ? out.npuProbe.error : '（native 未给文本）'));
    } else {
      probeText = '探测失败：' + out.npuError;
    }
    onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, 0, 'NPU 设备探测',
      '[' + secIdx + 1 + '/7] NPU 探测 · ' + probeText, false, ''));
    RunLog.get().item('一键全跑·NPU', 'NPU 设备探测', 1, 2, 0, 0, '', 0, 1,
      out.npuProbe !== null && out.npuProbe.ok, probeText);

    if (this.stopped) {
      sec.status = ST_FAILED;
      sec.note = '用户点了「停止」：NPU 推理没有开始。' + probeText;
      return;
    }

    // 无论探测结论如何都跑一次: native 自带 ACCELERATOR -> GPU -> CPU 回退阶梯,
    // 探测说"没开放"时它会返回失败原因, 这本身就是要记录的事实。
    onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, 0, 'NPU 推理跑分', '', false, ''));
    const t0: number = Date.now();
    try {
      out.npuRun = await BenchRunner.runNpu(NPU_SCALE, NPU_WARMUP, NPU_ITERS, NPU_BUDGET_MS);
    } catch (e) {
      out.npuError = (out.npuError.length > 0 ? (out.npuError + ' / ') : '') +
        'NPU 推理抛出异常：' + e.message;
      out.npuRun = null;
    }
    this.doneItems++;
    sec.itemDone++;
    const runMs: number = Date.now() - t0;
    let runLine: string = '';
    if (out.npuRun !== null) {
      runLine = 'status=' + out.npuRun.status + ' · device=' + out.npuRun.deviceTypeName +
        ' · metric=' + numOrDash(out.npuRun.goPs, 3) + ' GOPs/s · ' +
        (out.npuRun.text.length > 0 ? out.npuRun.text
          : (out.npuRun.error.length > 0 ? out.npuRun.error : '（native 未给文本）'));
    } else {
      runLine = out.npuError;
    }
    onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, runMs, 'NPU 推理跑分',
      '[' + secIdx + 1 + '/7] NPU 推理 · ' + runLine, false, ''));
    RunLog.get().item('一键全跑·NPU', 'NPU 推理跑分', 2, 2, runMs,
      out.npuRun !== null ? out.npuRun.goPs : 0, out.npuRun !== null ? 'GOPs/s' : '', 0, 1,
      out.npuRun !== null && out.npuRun.ok, runLine);

    sec.note = probeText + ' ｜ ' + runLine +
      ' ｜ 本小节 scored=false：native 侧标记为不计分，不并入 CS1 / 自研套件复合分。';
    if (out.npuRun !== null && !out.npuRun.ok) {
      sec.status = ST_FAILED;
    }
  }

  // ---- 6) 存储 I/O: prepare -> info -> run -> 无论如何 cleanup ----
  private async runStorageSection(out: FullRunOutcome, sec: FullRunSection, secIdx: number,
    onProgress: (p: FullRunProgress) => void): Promise<void> {
    const dir: string = FullReportStore.get().filesDir();
    if (dir.length === 0) {
      out.storageError = '拿不到 filesDir（页面 context 还没就绪），存储小节没能准备目录。';
      sec.status = ST_FAILED;
      sec.note = out.storageError;
      this.doneItems += STORAGE_NOMINAL_UNITS;
      sec.itemDone = sec.itemTotal;
      return;
    }
    onProgress(this.mkProgress(out, sec, secIdx, 0, 0, '存储目录准备', '', false, ''));
    out.storagePrepared = BenchRunner.storagePrepare(dir);
    out.storageInfo = BenchRunner.storageInfoText();
    this.doneItems++;
    sec.itemDone++;
    onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, 0, '存储目录准备',
      '[' + secIdx + 1 + '/7] 存储 · 目录准备=' + (out.storagePrepared ? 'OK' : '失败') +
      ' · ' + (out.storageInfo.length > 0 ? out.storageInfo : '（只读探测无返回）'),
      false, ''));

    const t0: number = Date.now();
    try {
      out.storage = await BenchRunner.runStorage('');
    } catch (e) {
      out.storageError = '存储跑分抛出异常：' + e.message;
      out.storage = null;
    } finally {
      // 兜底清理: 无论成功 / 失败 / 异常, 都把 <filesDir>/storage_bench 下的临时文件删干净
      out.storageCleanupOk = BenchRunner.storageCleanupNow();
    }
    const usedMs: number = Date.now() - t0;
    const realItems: number = out.storage !== null ? out.storage.items.length : 0;
    this.doneItems += (realItems > 0 ? realItems : STORAGE_NOMINAL_UNITS);
    sec.itemDone = sec.itemTotal;
    if (out.storage !== null) {
      const st: StorageRunResult = out.storage;
      for (const it of st.items) {
        RunLog.get().item('一键全跑·存储IO', it.name, 0, realItems,
          it.ms, it.iops > 0 ? it.iops : it.mbps,
          it.iops > 0 ? 'IOPS' : 'MiB/s', 0, 1, it.ok, it.error.length > 0 ? it.error : it.note);
      }
      const head: string = '顺序写(落盘) ' + numOrDash(st.seqWriteDurableMbps, 1) +
        ' MiB/s · 顺序读(冷) ' + numOrDash(st.seqReadColdMbps, 1) + ' MiB/s · 4K 随机读 ' +
        numOrDash(st.randRead4kIops, 1) + ' IOPS · 4K 随机写 ' + numOrDash(st.randWrite4kIops, 1) +
        ' IOPS';
      sec.note = (st.skipped ? ('已跳过：' + st.skipReason + ' · ') : '') + head + ' · 项 ' +
        realItems.toString() + ' · 耗时 ' + durText(usedMs) + ' · 清理=' +
        (out.storageCleanupOk ? 'OK' : '失败/有残留') +
        ' ｜ 本小节不计分，不并入任何复合分。';
      if (st.fatal.length > 0) {
        sec.status = ST_FAILED;
        sec.note = '致命错误：' + st.fatal + ' · ' + sec.note;
      }
    } else {
      sec.note = out.storageError + ' · 清理=' + (out.storageCleanupOk ? 'OK' : '失败/有残留');
      sec.status = ST_FAILED;
    }
    onProgress(this.mkProgress(out, sec, secIdx, sec.itemDone, usedMs, '存储小节结束',
      '[' + secIdx + 1 + '/7] 存储 · ' + sec.note, false, ''));
  }

  private scoredCount(list: Gb7Result[]): number {
    let n = 0;
    for (const r of list) {
      if (r.score > 0) {
        n++;
      }
    }
    return n;
  }

  // 与小节 note 用的是同一个口径函数(见 FullReportBuilder.geoOf), 不再各算各的。
  private geo(list: Gb7Result[]): number {
    return FullReportBuilder.geoOf(list);
  }

  // 「不可用于跨芯片比较」的项数(来源只有"未通过随芯片变化自检"这一种; 见 ScalingAudit)。
  //
  // 2026-10-05 改措辞: 这些项不再被排除出复合分(见 geoOf 的说明), 所以这里只数
  // "有几项要打提示", 不能再写成"已排除" —— 旧措辞 "，已排除 5 项未通过随芯片变化自检的项"
  //  正是 8.1 真机那份自相矛盾报告的来源: 小节结论说"计分 8 项 / 未拿到分 0 项",
  //  同时又说"已排除 5 项", 而 items[] 里那 5 项明明是正分、也真的算进了复合分。
  private notComparableCount(list: Gb7Result[]): number {
    let n = 0;
    for (const r of list) {
      if (r.score > 0 && !isComparable(r)) {
        n++;
      }
    }
    return n;
  }

  // 附在小节结论末尾的提示(不是复合分口径的一部分; '' = 无)。
  private exclusionNote(list: Gb7Result[]): string {
    const n: number = this.notComparableCount(list);
    return n > 0
      ? (' · 其中 ' + n.toString() + ' 项随芯片变化自检不通过（不可用于跨芯片比较，仅提示，' +
        '仍按同一口径计入上面的复合分）')
      : '';
  }

  // GB7 两个 CPU 小节的 note 由这里统一生成。为什么放在一处:
  //   ① 综合分走同一个口径函数 FullReportBuilder.geoOf;
  //   ② 多核阶段跑完会重算单核那一行 —— 否则"单核小节 note 里的综合分"会是上一次算的数,
  //      与报告正文里的数对不上, 又变成一次"核对不一致"。
  //
  // 2026-10-05 修(自相矛盾的那一行) 旧写法是
  //   '综合分（计分项几何平均，已排除 5 项未通过随芯片变化自检的项）2072 · 线程 8 · 计分 8 项 ·
  //    未拿到分 0 项'
  // 这三截互相打架: "计分 8 项 / 未拿到分 0 项"说的是 8 项全都拿到分了, 同一行却说排除了 5 项;
  // 而 items[] 里那 5 项确实是正分、也确实是复合分的参与项。现在:
  //   ① 括号里只说口径(计分项几何平均), 不再出现"已排除";
  //   ② "参与复合分 N 项"与 geoOf 的参与项集合逐项一致(participantOf), 且必须等于 items[] 里
  //      该阶段有正分的项数 —— 两个数一起印, 谁对不上了一眼就能看出来;
  //   ③ 自检不通过写成句尾提示, 明说"仅提示, 仍计入复合分"。
  private refreshGb7Notes(out: FullRunOutcome): void {
    const s: FullRunSection | null = out.sectionById(SEC_GB7S);
    if (s !== null && out.gb7Single.length > 0) {
      const scored: number = this.scoredCount(out.gb7Single);
      s.note = '综合分（计分项几何平均）' + this.geo(out.gb7Single).toFixed(0) +
        ' · 计分项 ' + scored.toString() + '/' + out.gb7Single.length.toString() + ' 有正分' +
        ' · 参与复合分 ' + FullReportBuilder.participantOf(out.gb7Single).toString() + ' 项' +
        ' · 未拿到分 ' + (out.gb7Single.length - scored).toString() + ' 项' +
        this.exclusionNote(out.gb7Single) + ' · 耗时 ' + durText(out.gb7SingleElapsedMs);
    }
    const m: FullRunSection | null = out.sectionById(SEC_GB7M);
    if (m !== null && out.gb7Multi.length > 0) {
      const scored: number = this.scoredCount(out.gb7Multi);
      m.note = '综合分（计分项几何平均）' + this.geo(out.gb7Multi).toFixed(0) +
        ' · 线程 ' + out.gb7Threads.toString() + '（' + BenchRunner.smtSummaryFlag(out.gb7Threads) +
        '）· 计分项 ' + scored.toString() + '/' + out.gb7Multi.length.toString() + ' 有正分' +
        ' · 参与复合分 ' + FullReportBuilder.participantOf(out.gb7Multi).toString() + ' 项' +
        ' · 未拿到分 ' + (out.gb7Multi.length - scored).toString() + ' 项' +
        this.exclusionNote(out.gb7Multi) + ' · 耗时 ' + durText(out.gb7MultiElapsedMs);
    }
  }
}

// ===========================================================================
// 报告: 生成(分块) + 落盘(分块)
// ===========================================================================

// 界面侧的事实(只有页面知道的东西: 内存 / 热状态 / 崩溃横幅 / 版本串)
export interface FullRunUiFacts {
  appVersion: string;
  device: string;
  nativeVersion: string;
  memText: string;
  thermalText: string;
  crashBannerFound: boolean;      // 首页崩溃横幅是否命中(面包屑)
  crashBannerLine: string;
  nativeCrashFound: boolean;      // 本次启动是否从取证装置读到过 native 现场
  nativeCrashChars: number;
  nativeCrashSummary: string;
}

// 取证文件的一条事实
export interface EvidenceFileFact {
  label: string;
  path: string;
  exists: boolean;
  bytes: number;
  err: string;
}

function evidenceFact(label: string, path: string): EvidenceFileFact {
  const f: EvidenceFileFact = { label: label, path: path, exists: false, bytes: 0, err: '' };
  if (path.length === 0) {
    f.err = 'native 未返回路径（取证装置没就绪 / 接口不可用）';
    return f;
  }
  try {
    if (!fs.accessSync(path)) {
      f.err = '文件不存在（没有崩溃/卡死记录，或已被读取后清除）';
      return f;
    }
    f.exists = true;
    f.bytes = fs.statSync(path).size;
    if (f.bytes === 0) {
      f.err = '存在但大小为 0（没有写入任何记录）';
    }
  } catch (e) {
    f.err = '探测失败：' + e.message;
  }
  return f;
}

// ---- 分块缓冲: 报告不用"一次拼一个巨大字符串", 而是攒成一个块数组, 逐块写盘 ----
export class TextBuf {
  parts: string[] = [];
  chars: number = 0;

  add(s: string): void {
    if (s.length === 0) {
      return;
    }
    this.parts.push(s);
    this.chars += s.length;
  }

  ln(s: string): void {
    this.add(s);
    this.add('\n');
  }

  rule(): void {
    this.ln(repeat('-', 78));
  }

  head(title: string): void {
    this.ln('');
    this.rule();
    this.ln('【' + title + '】');
    this.rule();
  }
}

// ===========================================================================
// JSON 报告的字段(全部显式声明, 不用 any/unknown)
// ===========================================================================
// 落盘事实(写进 JSON 报告头): 路径 + 结果。见 ReportSinkInfo 的说明 ——
// 报告里必须能读到"这份报告到底有没有写进盘、写到哪一条路径", 而不是一个恒为空的占位。
interface RepSinkJson {
  textPath: string;
  jsonPath: string;
  latestTextPath: string;
  latestJsonPath: string;
  written: boolean;
  status: string;
}

interface RepHeaderJson {
  schema: string;
  appVersion: string;
  nativeVersion: string;
  device: string;
  startedAt: number;
  startedAtText: string;
  finishedAt: number;
  elapsedMs: number;
  interrupted: boolean;
  stopReason: string;
  sectionCount: number;
  itemCount: number;
  scoredNote: string;
  // CS1 可比性声明(与报告正文、「关于」页同一句话)
  comparabilityNote: string;
  // 「本机性能天花板」一行(2026-10 追加): native 唯一来源(BenchRunner.ceiling())。
  //   放在 header 里是为了让对比工具 / 历史记录 / App 内对比页也能直接读到
  //  "这台机器被什么卡住"—— 只看两个分数是无法判断"差 30% 是芯片差还是系统不给用"的。
  ceilingNote: string;
  reportSink: RepSinkJson;
}

interface RepSectionJson {
  id: string;
  title: string;
  status: string;
  note: string;
  elapsedMs: number;
  itemDone: number;
  itemTotal: number;
}

interface RepItemJson {
  section: string;
  name: string;
  metric: number;
  unit: string;
  score: number;
  ms: number;
  parallelism: number;
  threadsRequested: number;
  threadsEffective: number;
  workersBound: number;
  runFreq: string;
  // ---- QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 补齐) ----
  // 与 runFreq / diag 一样是 native 的旁路字段(native runGb7 JSON 的 "qos" / "cpuset" 键),
  // 【纯展示】: 不参与 metric / unit / k / conv / 计分公式, 也不改变任何负载的算法与尺寸。
  // 为什么必须在 items[] 里(2026-10 真机报告缺陷 2): 报告对比工具(verify_report_compare.py /
  // App 内的对比页)与历史记录只读 items[]; 这两个键以前在这里被丢掉, 于是"这台机器的限制在哪
  // 一层""QoS 到底生效没有"的证据在 JSON 报告里彻底看不见(只在 .txt 的 note 里露一半)。
  // '' = native 未上报(老 native / GPU 路径 / 自研套件项 / 非 CS1 项)。
  qos: string;
  cpuset: string;
  cpu: number;
  cpuMaxKhz: number;
  cpuAtStart: number;
  // ---- 自研套件也补齐的落核取证(2026-10): 该核位次 / 是否已绑到快簇 / 绑核目标核数 ----
  //  以前自研套件项的这几个键全是默认值(0/''), 于是 JSON 报告与 .txt 都看不出
  //  "这一项跑在哪颗核上"; CS1 侧一直有, 两条路径口径不一致正是 37.7% 那个 bug 的形态。
  cpuRank: number;
  cpuBound: boolean;
  singleTargetCores: number;
  machineTopTierCores: number;
  machineTopTierKhz: number;
  // ---- GPU「跑满判据」一行(native gpu7Fullness 的 text; 只有 CS1 GPU 项有) ----
  //  形如 "GPU 跑满判据 -> 已达到(...)" / "... -> 未达到(还差 x.x 个百分点: ...)" /
  //  "... -> 本项不适用(原因: ...)"。旁路结论, 不计分; '' = 不是 GPU 项 / 未上报。
  gpuFullness: string;
  fastClusterMask: string;
  fastClusterCores: number;
  fastClusterMaxKhz: number;
  inFastCluster: boolean;
  inMachineTopTier: boolean;
  allowedOk: boolean;
  allowedCount: number;
  allowedMask: string;
  appliedMask: string;
  fastClusterFallback: boolean;
  fastClusterTierIndex: number;
  diag: string;
  scoredByPolicy: boolean;
  scoredNote: string;
  ok: boolean;
  // ---- 可重复性 / 离散度(2026-08-31 起 native 默认跑 2 轮; 【纯展示】, 不计分) ----
  //   rounds         : 本项实际跑了几轮(0 = 未上报 / 运行失败)
  //   repeatVerdict  : RELIABLE / FAIR / UNRELIABLE / SINGLE_ROUND_NO_DATA / NO_VALID_ROUNDS /
  //                    RUN_FAILED / ''(老 native 未上报 —— 报告里写空串, 不编)
  //   scoreSpreadPct : 相对离散度 = (最大 - 最小) / 中位 x 100%(0 且 verdict 为空 = 没数据)
  rounds: number;
  repeatVerdict: string;
  scoreSpreadPct: number;
  // ---- 本项是否通过"随芯片性能变化"自检(2026-10-08) ----
  //   PASS     = 有正向证据(吞吐确实随可用算力变化);
  //   FAIL     = 有反向证据 -> 不可用于跨芯片比较(仅标注; 仍计入复合分, 2026-10-05);
  //   UNPROVEN = 现有数据判不出来, 写"未自证";
  //   ''       = 本路径没做这项自检。
  scalingVerdict: string;
  scalingEvidence: string;
  loadId: number;
}

interface RepCompositeJson {
  ownTotal: number;
  ownSingle: number;
  ownMulti: number;
  ownGpu: number;
  ownIntSingle: number;
  ownFloatSingle: number;
  ownIntMulti: number;
  ownFloatMulti: number;
  gb7SingleComposite: number;
  gb7MultiComposite: number;
  gb7GpuComposite: number;
  gb7MultiThreads: number;
  note: string;
  // ---- 复合分的两个数都要报, 不许只挑好看的那个(2026-08-31) ----
  //   gb7*Composite               = 由各项中位分算出的几何平均(卡片上那个数, 历史口径);
  //   gb7*CompositeMedianOfRounds = 逐轮复合分的中位(native repeatability.median)。
  //   GM 单调但不可加, 两个数很接近但不保证相等 —— 报告里并列, 谁都不许被隐掉。
  //   0 / '' = 没拿到(该阶段没跑 / 老 native), 报告里写'没有这一列', 不编数。
  gb7SingleCompositeMedianOfRounds: number;
  gb7MultiCompositeMedianOfRounds: number;
  gb7SingleCompositeSpreadPct: number;
  gb7MultiCompositeSpreadPct: number;
  gb7SingleCompositeRounds: number;
  gb7MultiCompositeRounds: number;
  gb7SingleCompositeVerdict: string;
  gb7MultiCompositeVerdict: string;
  gb7SingleCompositeVerdictText: string;
  gb7MultiCompositeVerdictText: string;
  repeatabilityNote: string;
  // ---- 口径证据(2026-10-08): 这个复合分由哪些项、用哪些值、按什么规则排除算出来的 ----
  // 全部是 native 的原话(不加工): 参与项 items[] / 排除项 excluded[] / 取值来源 scoreSource。
  gb7SingleScoreSource: string;
  gb7MultiScoreSource: string;
  gb7SingleItems: string;
  gb7MultiItems: string;
  gb7SingleExcluded: string;
  gb7MultiExcluded: string;
  gb7SingleCount: number;
  gb7MultiCount: number;
  gb7SingleAuditExcluded: string;   // 未通过自检 -> 标注"不可用于跨芯片比较"的项(ArkTS 侧; 仍计入复合分)
  gb7MultiAuditExcluded: string;
  gb7SingleComparableCount: number; // 复合分参与项里"可用于跨芯片比较"的项数(是参与项的子集)
  gb7MultiComparableCount: number;
  // 永久断言(2026-10-05) 复合分的参与项集合 == 报告 items[] 里该阶段有正分的项集合。
  // 两边只要有一边少算/多算了项, 这里就是 false —— 不用人去逐字读三句话才发现矛盾。
  gb7SingleParticipants: number;    // 参与项数(= geoOf 实际用于几何平均的项数)
  gb7MultiParticipants: number;
  gb7SingleScopeOk: boolean;
  gb7MultiScopeOk: boolean;
  gb7SingleScopeText: string;
  gb7MultiScopeText: string;
}

// 排除清单的逐项记录(项名 + 排除原因 + 判定依据原文)。
// 为什么要有: 以前报告里只有一个 excludedNames 名单, 用户看得到"排除了哪几项"却看不到"为什么",
// 于是无法核对两台设备排除的是不是同一批项 —— 而这件事直接决定两个复合分能不能相除。
interface RepScalingExcluded {
  name: string;      // 项名(与 CS1 注册表同名)
  loadId: number;    // 负载 id(-1 = 报告里没有)
  stage: string;     // 在哪一个口径上判的 FAIL(单核 / 多核)
  verdict: string;   // FAIL / UNPROVEN / PASS
  reason: string;    // 判定依据原文(判据 + 数字 + 结论), 可直接贴给用户看
}

// 逐项「本项是否随芯片性能变化」自检在 JSON 报告里的一块(旁路结论, 不参与计分)
interface RepScalingJson {
  question: string;
  verdicts: string[];
  passCount: number;
  failCount: number;
  unprovenCount: number;
  hasAnyProbe: boolean;
  excludedIds: number[];
  excludedNames: string[];
  excludedDetail: RepScalingExcluded[];   // 新增(2026-10-08): 项名 + 原因 + 依据, 逐条印全
  excludedDetailNote: string;
  // 2026-10-05: 这份清单对复合分参与项集合的影响 = 无(恒 false)。
  // 字段留着是为了让 JSON 的读者不必猜"这些 excluded 到底有没有从复合分里扣掉"。
  affectsCompositeScope: boolean;
  criteria: string[];
  thresholdsAreOursNotOfficial: boolean;
  thresholdNote: string;
  text: string;
  singleRows: string[];
  multiRows: string[];
}

// ---- GPU-SNL(Aurora Nomad Light)小节的结构化结论块(2026-10 真机报告缺陷 3 修) ----
// 为什么要有它: 以前 JSON 报告里根本没有 SNL 小节的结构化数据(原始 JSON 只作为字符串
// 塞在 referenceAudit.inputsJson.gpuSnl 里), 于是"这一节到底成功没有、为什么失败"在 .json
// 里查不到, 只有 .txt 的一句话 —— 而那句话在真机上还是与原始 JSON 自相矛盾的
// 「native 返回失败：（native 未给文本）」。
// 现在把两个事实位(解析成功没有 / 原文自称什么)与非空失败原因单独成块,
// 对比工具与历史记录可以直接读; 原始 JSON 仍然只在 referenceAudit 里存一份(不重复占体积)。
interface RepSnJson {
  section: string;
  workload: string;
  scored: boolean;
  gb7Item: boolean;
  ran: boolean;                 // 拿到了 native 的返回字符串(prepare 之后 run 被调用过)
  parseOk: boolean;             // 原始 JSON 解析成功没有(false = 本节所有数值都是"未取到")
  selfClaimedOk: boolean;       // 原始 JSON 文本自己是不是以 "ok":true 开头(仅记录原文)
  selfClaimNote: string;        // 两者不一致时的人话说明(一致时为空串)
  status: string;               // '成功' / '失败' / '未执行'
  failReason: string;           // 失败时的非空原因(成功时为空串)
  prepError: string;            // prepare() 的驱动原文(空串 = 初始化成功)
  valuesAvailable: boolean;     // 下面那几个数是不是真读数(false = 一个都没取到, 不要当 0 用)
  score: number;
  fps: number;
  throughputMpx: number;
  measuredFrames: number;
  rawChars: number;             // 原始 JSON 的字符数(内容本身见 rawWhere)
  rawWhere: string;
  note: string;
}

interface RepNpuJson {
  sectionScored: boolean;
  sectionScoredNote: string;
  probeRan: boolean;
  probeOk: boolean;
  probeStatus: string;
  probeText: string;
  probeError: string;
  thirdPartyNpuOpen: boolean;
  probeDeviceCount: number;
  nnrtAvailable: boolean;
  nnrtBound: number;
  nnrtExpected: number;
  nnrtMissingSymbols: string;
  nnrtDlerror: string;
  runRan: boolean;
  runOk: boolean;
  runStatus: string;
  runText: string;
  runError: string;
  deviceTypeName: string;
  deviceLabel: string;
  deviceId: number;
  deviceName: string;
  opSetName: string;
  mainOp: string;
  opSetFallbackUsed: boolean;
  precisionShort: string;
  scaleLabel: string;
  macsPerInference: number;
  flopsPerInference: number;
  goPs: number;
  macsPerSec: number;
  meanMs: number;
  medianMs: number;
  minMs: number;
  maxMs: number;
  measuredRuns: number;
  warmupRuns: number;
  budgetHit: boolean;
  hint: string;
  jsException: string;
  probeRawJson: string;
  runRawJson: string;
}

interface RepStorageItemJson {
  id: string;
  name: string;
  ok: boolean;
  ms: number;
  bytes: number;
  mbps: number;
  iops: number;
  ops: number;
  auxMs: number;
  auxName: string;
  mode: string;
  comparable: string;
  error: string;
  note: string;
}

interface RepStorageJson {
  sectionScored: boolean;
  sectionScoredNote: string;
  filesDir: string;
  prepared: boolean;
  infoJson: string;
  ran: boolean;
  ok: boolean;
  skipped: boolean;
  skipReason: string;
  fatal: string;
  sectionTitle: string;
  dir: string;
  totalMs: number;
  budgetMs: number;
  stoppedEarly: boolean;
  stopReason: string;
  cleanupOk: boolean;
  seqWriteDurableMbps: number;
  seqReadColdMbps: number;
  randRead4kIops: number;
  randWrite4kIops: number;
  filesPerSecCreate: number;
  filesPerSecDelete: number;
  totalBytes: number;
  freeBytes: number;
  availBytes: number;
  blockSize: number;
  oDirectWorks: boolean;
  fadviseDontNeedWorks: boolean;
  bypassMethod: string;
  warnings: string[];
  items: RepStorageItemJson[];
  rawJson: string;
}

interface RepEvidenceJson {
  label: string;
  path: string;
  exists: boolean;
  bytes: number;
  note: string;
}

interface RepCrashJson {
  guardReady: boolean;
  guardStatus: string;
  guardLogPath: string;
  crashFileExists: boolean;
  crashFileBytes: number;
  hangFileExists: boolean;
  hangFileBytes: number;
  crashRecordFoundThisSession: boolean;
  crashRecordChars: number;
  crashRecordSummary: string;
  crumbFound: boolean;
  crumbLine: string;
  evidenceFiles: RepEvidenceJson[];
}

interface RepEnvJson {
  memText: string;
  thermalText: string;
  smtTopology: string;
  affinityPanel: string;
  perfGuard: string[];
  untouched: string[];
}

// 参考对照 / 线性-可比性审计(libauroraref.so, 只读)在 JSON 报告里的两块:
//   · inputs  = 我们真正传进去的量(原样 options + 逐项来源说明) —— 报告里能直接看到
//               "哪些量接上了、哪些没接上、没接上的为什么没接上"(而不是只看到一堆 null);
//   · table   = native 原样返回的对照表(合法 JSON 才原样嵌入; 不合法就写原因, 不硬塞)。
interface RepRefAuditJson {
  moduleAvailable: boolean;
  moduleVersion: string;
  ok: boolean;
  error: string;
  inputsJson: string;
  inputsNote: string;
  tableValid: boolean;
  table: Object | null;
}

// ===========================================================================
// 复合分的可重复性 —— 这两个数必须都报, 不许只挑好看的那个
//
// 为什么必须调 native(libaurorabench.so 的 gb7CompositeSingle / gb7CompositeMulti):
//   "逐轮复合分"要的是每一项每一轮的单项分矩阵, 那个矩阵只在 native 里(g_roundScore)。
//   ArkTS 侧只有"各项最终分(中位)"这一列, 自己算不出逐轮复合分。
// 口径(与 gb7.cpp 的 compositeJson 完全一致, 本文件不做任何再加工):
//   ① 报告里的 composite.gb7*Composite = 由各项中位分算出的几何平均(卡片上那个数);
//   ② composite.gb7*CompositeMedianOfRounds = 逐轮复合分的中位(native repeatability.median)。
//   GM 单调但不可加, 两个数很接近但不保证相等 —— 两个都写进报告, 谁都不许被隐掉。
// 拿不到(没跑该阶段 / 老 native)时全部写 0 / 空串 + 一句话说明, 不编一个离散度出来。
// ===========================================================================
export interface CompositeRepeatFacts {
  singleJson: string;      // native 原样 JSON('' = 没拿到)
  multiJson: string;
  // ---- 口径证据(2026-10-08 新增) ----
  // 这一段存在的唯一目的: 让"这个复合分到底由哪些项、用哪些值、按什么规则排除算出来的"
  // 在报告里一眼可见, 不会再出现"报告 328.2 / native 561.1 却看不出差在哪"的情况。
  singleSource: string;    // native 原话: 这份复合分用的是哪一份单项分(explicit-array / stage-cache)
  multiSource: string;
  singleItems: string;     // native 给的参与项逐项对照(名字 + 分 + 负载 id)
  multiItems: string;
  singleExcluded: string;  // native 给的排除项逐条对照(名字 + 排除原因)
  multiExcluded: string;
  singleCount: number;     // 参与项数(native count)
  multiCount: number;
  singleAuditExcluded: string;  // 因"未通过随芯片变化自检"而被标注"不可用于跨芯片比较"的项(名字; '' = 无)
  multiAuditExcluded: string;   // 注意: 它们仍然是复合分的参与项(2026-10-05 起口径如此)
  singleValue: number;     // 由中位分算出的复合分(native composite 字段; 0 = 没拿到)
  multiValue: number;
  singleMedian: number;    // 逐轮复合分的中位(0 = 没有这一列)
  multiMedian: number;
  singleSpread: number;    // 逐轮复合分的相对离散度 %
  multiSpread: number;
  singleRounds: number;    // 逐轮复合分有几个(0 = 没有这一列)
  multiRounds: number;
  singleVerdict: string;   // RELIABLE / FAIR / UNRELIABLE / SINGLE_ROUND_NO_DATA / NO_VALID_ROUNDS
  multiVerdict: string;
  singleText: string;      // native 给的一句话人话结论(含"阈值是我们自己定的")
  multiText: string;
  // ---- 永久断言(2026-10-05): 参与项集合必须与报告 items[] 里该阶段有正分的项集合逐项一致 ----
  // false = 复合分的参与项集合与 items[] 对不上(有人少算了项 / 改了取值口径) -> 报告里必须打 FAIL。
  singleScopeOk: boolean;
  multiScopeOk: boolean;
  singleScopeText: string; // 逐项对照: 本阶段有正分的 id 集合 vs native 参与项的 id 集合
  multiScopeText: string;
  note: string;            // 两个数分别是什么 / 拿不到时为什么
}

// 「复合分的参与项集合 == 报告 items[] 里该阶段有正分的项集合」的运行时硬核对结果。
interface ScopeCheck {
  ok: boolean;
  text: string;
  expectedN: number;
  actualN: number;
}

// 逐项比对两个 id 集合, 结果写成可读文本(两个方向都写: 少了谁 / 多了谁)。
// 为什么这条断言必须存在: 8.1 真机上"多核复合分只用了 3 项"这件事, 在报告里只有三句
// 互相矛盾的话(小节结论 / 排除项理由 / 参与项), 没有任何一行是"机器判定"的 ——
// 人得逐字读才能发现。现在把它变成一条会自己喊出来的断言。
function compositeScopeCheck(list: Gb7Result[], json: string): ScopeCheck {
  const expected: number[] = [];
  for (const r of list) {
    if (r.score > 0 && r.loadId >= 0 && expected.indexOf(r.loadId) < 0) {
      expected.push(r.loadId);
    }
  }
  const actual: number[] = BenchRunner.compositeItemIds(json);
  const missing: number[] = [];
  const extra: number[] = [];
  for (const id of expected) {
    if (actual.indexOf(id) < 0) {
      missing.push(id);
    }
  }
  for (const id of actual) {
    if (expected.indexOf(id) < 0) {
      extra.push(id);
    }
  }
  const ok: boolean = json.length > 0 && missing.length === 0 && extra.length === 0;
  const text: string = (json.length === 0)
    ? '（没拿到 native 的复合分 JSON，无法核对）'
    : ('本阶段有正分的项 ' + expected.length.toString() + ' 个 [' + expected.join(',') + '] vs ' +
      'native 参与项 ' + actual.length.toString() + ' 个 [' + actual.join(',') + ']' +
      (missing.length > 0 ? ' · native 少算了 id=' + missing.join(',') + '' : '') +
      (extra.length > 0 ? ' · native 多算了 id=' + extra.join(',') + '' : '') +
      (missing.length === 0 && extra.length === 0 ? ' -> 逐项一致' : ''));
  return { ok: ok, text: text, expectedN: expected.length, actualN: actual.length };
}

// 统一口径(2026-10-08 修, 2026-10-05 再修)
// 把"本阶段的逐项分"按负载 id 显式传给 native 的 gb7CompositeSingle/Multi ——
// 于是 native 算出来的 composite 与报告卡片上那个数由构造保证同源。
//
// 以前这里两个参数都不传, native 就去读它自己那份 g_lastScore 缓存。那份缓存当时没有阶段
// 维度, 多核阶段(只跑 8 项)会把单核那一遍的 8 项覆盖成多核分, 于是 native 算出来的
// "单核复合分"= {8 项多核分} ∪ {7 项单核分} 的几何平均 = 真机 561.1, 而卡片口径是 328.2。
// 根因已在 native 侧修掉(阶段维度), 这里再额外传数组, 把口径钉死在两边的同一份数上。
//
//  2026-10-05 再修: 传下去的数组不再做任何置 0 
// 旧写法把"未通过随芯片变化自检(FAIL)"的项置 0 再传给 native, 想表达"两边排除同一批项"。
// 真机 8.1(Pura X Max, 2026-10-05 17:52)把它的三个后果全暴露了:
//   ① 同一份报告自相矛盾: items[] 里那 5 项明明是正分(FC 176.9 / PL 5446.1 / HDR 39.2 /
//      RT 3957.8 / Clang 14761.3), native 的「排除项」却写"本次没有拿到正的分数",
//      而 [3/7] 小节结论同时印着"未拿到分 0 项" —— 三句话互相打架;
//   ② 置 0 用的是单核 ∪ 多核的并集: 只在多核口径判 FAIL 的项连单核数组也一起被抹掉,
//      单核复合分因此少了 5 项 —— 报告自己那行「核对不一致 native 409.4 / 本报告 416.2」
//      就是它(7.5 / 8.0 也有同一行, 8.0 是 287.8 / 418.7);
//   ③ 同一台设备 8.0 -> 8.1: 全口径几何平均只差 0.26%(1447.06 -> 1450.76),
//      被"自检排除"之后却差 160%(795.81 -> 2071.86) —— 复合分变成"这一轮的噪声抽签"。
// 现在: 数组逐位等于本阶段各项的真实分数, 于是
//   复合分参与项集合 == 报告 items[] 里该阶段有正分的项集合   (逐项相等, 由构造保证)
// "未通过随芯片变化自检"只作提示(标注"不可用于跨芯片比较"), 不再改写参与项集合。
// 这条不变量由下面的 compositeScopeCheck() 在运行时硬核对, 对不上就在报告里打 FAIL。
export function compositeRepeatFacts(single: Gb7Result[], multi: Gb7Result[]): CompositeRepeatFacts {
  const sJson: string = BenchRunner.compositeRepeatJson(false, BenchRunner.gb7ScoreArray(single));
  const mJson: string = BenchRunner.compositeRepeatJson(true, BenchRunner.gb7ScoreArray(multi));
  const sScope: ScopeCheck = compositeScopeCheck(single, sJson);
  const mScope: ScopeCheck = compositeScopeCheck(multi, mJson);
  const facts: CompositeRepeatFacts = {
    singleJson: sJson, multiJson: mJson,
    singleSource: BenchRunner.compositeSourceText(sJson),
    multiSource: BenchRunner.compositeSourceText(mJson),
    singleItems: BenchRunner.compositeItemsText(sJson),
    multiItems: BenchRunner.compositeItemsText(mJson),
    singleExcluded: BenchRunner.compositeExcludedText(sJson),
    multiExcluded: BenchRunner.compositeExcludedText(mJson),
    singleCount: BenchRunner.compositeCount(sJson),
    multiCount: BenchRunner.compositeCount(mJson),
    singleAuditExcluded: FullReportBuilder.auditExcludedText(single),
    multiAuditExcluded: FullReportBuilder.auditExcludedText(multi),
    singleValue: BenchRunner.compositeValue(sJson),
    multiValue: BenchRunner.compositeValue(mJson),
    singleMedian: BenchRunner.compositeNum(sJson, 'median'),
    multiMedian: BenchRunner.compositeNum(mJson, 'median'),
    singleSpread: BenchRunner.compositeNum(sJson, 'dispersion'),
    multiSpread: BenchRunner.compositeNum(mJson, 'dispersion'),
    singleRounds: BenchRunner.compositeRounds(sJson),
    multiRounds: BenchRunner.compositeRounds(mJson),
    singleVerdict: BenchRunner.compositeVerdict(sJson),
    multiVerdict: BenchRunner.compositeVerdict(mJson),
    singleText: BenchRunner.compositeVerdictText(sJson),
    multiText: BenchRunner.compositeVerdictText(mJson),
    singleScopeOk: sScope.ok,
    multiScopeOk: mScope.ok,
    singleScopeText: sScope.text,
    multiScopeText: mScope.text,
    note: ''
  };
  const parts: string[] = [];
  parts.push('口径定义(只此一个) 复合分 = 本报告「该阶段」各项中位分的几何平均; ' +
    '参与项必须同时满足: ① native 注册表 scored=true; ② 本阶段拿到正分数; ' +
    '③ 多核口径还要求属于官方多核 8 项(inMulti=true); ' +
    '④ 参与项集合 == 报告 items[] 里该阶段有正分的项集合(逐项一致, 见「参与项核对」)');
  parts.push('把项写进「排除项」只有两个原因: 它本来就不属于这个阶段口径(scored=false / ' +
    'inMulti=false), 或者本阶段确实没有正分 —— 不存在"有正分却被排除"的第三种情况');
  parts.push('「未通过随芯片变化自检(FAIL)」的项照常参与复合分, 只额外标注"不可用于跨芯片比较"; ' +
    '它不再改变参与项集合(否则同一台设备两轮之间的复合分会因为噪声抽签而差出几倍)');
  parts.push('① 由各项中位分算出的几何平均 = composite.gb7*Composite(卡片上那个数)');
  parts.push('② 逐轮复合分的中位 = composite.gb7*CompositeMedianOfRounds(native repeatability)');
  parts.push('两个数很接近但不保证相等(GM 单调但不可加) —— 两个都报, 不挑好看的');
  if (sJson.length === 0) {
    parts.push(' CS1 单核复合分：没拿到 native 的 gb7CompositeSingle() 返回值(该阶段没跑 / 模块不可用), 逐轮复合分这一列写 0');
  } else {
    parts.push('native gb7CompositeSingle().composite = ' + facts.singleValue.toFixed(1) +
      ' · 参与 ' + facts.singleCount.toString() + ' 项 · 取值口径 = ' +
      (facts.singleSource.length > 0 ? facts.singleSource : '（native 未上报）') +
      ' —— 与 composite.gb7SingleComposite 必须一致(同一个数组、同一套规则)');
    parts.push('native 给的参与项逐项对照: ' +
      (facts.singleItems.length > 0 ? facts.singleItems : '（未上报）'));
    parts.push('native 给的排除项逐条对照: ' +
      (facts.singleExcluded.length > 0 ? facts.singleExcluded : '（无）'));
    parts.push((facts.singleScopeOk ? '参与项核对 单核：' : '参与项核对不通过 单核：') +
      facts.singleScopeText);
  }
  if (mJson.length === 0) {
    parts.push(' CS1 多核复合分：没拿到 native 的 gb7CompositeMulti() 返回值(该阶段没跑 / 模块不可用), 逐轮复合分这一列写 0');
  } else {
    parts.push('native gb7CompositeMulti().composite = ' + facts.multiValue.toFixed(1) +
      ' · 参与 ' + facts.multiCount.toString() + ' 项 · 取值口径 = ' +
      (facts.multiSource.length > 0 ? facts.multiSource : '（native 未上报）') +
      ' —— 与 composite.gb7MultiComposite 必须一致(同一个数组、同一套规则)');
    parts.push('native 给的参与项逐项对照: ' +
      (facts.multiItems.length > 0 ? facts.multiItems : '（未上报）'));
    parts.push('native 给的排除项逐条对照: ' +
      (facts.multiExcluded.length > 0 ? facts.multiExcluded : '（无）'));
    parts.push((facts.multiScopeOk ? '参与项核对 多核：' : '参与项核对不通过 多核：') +
      facts.multiScopeText);
  }
  parts.push('阈值(2% / 5%)是我们自己定的, 不是官方阈值');
  facts.note = parts.join('；');
  return facts;
}

interface RepReportJson {
  schema: string;
  jsonVersion: number;
  header: RepHeaderJson;
  sections: RepSectionJson[];
  items: RepItemJson[];
  composite: RepCompositeJson;
  npu: RepNpuJson;
  storage: RepStorageJson;
  crash: RepCrashJson;
  env: RepEnvJson;
  referenceAudit: RepRefAuditJson;
  scalingAudit: RepScalingJson;
}

// 一个复合分的"口径 + 可重复性"视图(报告里单核 / 多核各一份)。
// 全部字段来自同一份 native JSON 与同一个 geoOf 口径, 这里不做任何换算。
export interface CompositeScope {
  title: string;
  reportValue: number;    // 报告卡片口径: geoOf(本阶段各项分)
  nativeValue: number;    // native 用同一份逐项分数组算出来的 composite
  count: number;          // native 报的参与项数
  itemsText: string;      // 参与项逐项对照(native items[])
  excludedText: string;   // 排除项逐条对照(native excluded[])
  sourceText: string;     // 取值口径原文(native scoreSource/scoreSourceText)
  auditExcluded: string;  // 因未通过随芯片变化自检被标注"不可用于跨芯片比较"的项(ArkTS 侧; 仍计入复合分)
  scopeOk: boolean;       // 永久断言 参与项集合 == 本报告 items[] 里该阶段有正分的项集合
  scopeText: string;      // 该断言的逐项对照原文
  medianOfRounds: number;
  spread: number;
  rounds: number;
  verdict: string;
  verdictText: string;
  got: boolean;
}

// ===========================================================================
// 报告生成器
// ===========================================================================
export class FullReportBuilder {

  // ---- 该项在报告里怎么标"计分 / 不计分" ----
  private static gb7ScoredNote(r: Gb7Result): string {
    if (BenchRunner.itemScoredButFailed(r)) {
      return '计分项，但本次没有拿到任何吞吐 -> 按运行失败显示（原因见 unit）';
    }
    if (r.scoredByPolicy) {
      return '计分项';
    }
    return '按政策不计分（native 注册表 scored=false / k=0；得 0 是设计如此，不是失败）';
  }

  private static gb7ResultText(r: Gb7Result): string {
    if (r.score > 0) {
      return '得分';
    }
    return BenchRunner.itemScoredButFailed(r) ? '失败' : '未计分';
  }

  // 「未通过随芯片变化自检」的项(项名 + 判定 + 原因原文; '' = 无)
  //
  // 2026-10-08 改: 以前只印 "项名(FAIL)", 用户看得到有哪几项却看不到为什么,
  // 于是无法核对两台设备的这份清单是不是同一批项 —— 而那件事直接决定两个复合分能不能相除。
  // 现在把报告里那一项的判定依据原文(scalingEvidence)一并印出来, 逐条可核对。
  //
  // 2026-10-05 改: 这些项不再被排除出复合分(见 geoOf)。这一段现在是"可比性提示",
  // 不是"参与项集合的一部分" —— 措辞必须与 D2 节、与 JSON 的 affectsCompositeScope=false 一致。
  static auditExcludedText(list: Gb7Result[]): string {
    const names: string[] = [];
    for (const r of list) {
      if (r.score > 0 && !isComparable(r)) {
        const why: string = (r.scalingEvidence.length > 0)
          ? r.scalingEvidence
          : '（该报告没有给出这一项的判定依据原文）';
        names.push(r.name + '(' + r.scalingVerdict + '；判定依据=' + why + ')');
      }
    }
    return names.join(' · ');
  }

  // 复合分的"口径 + 可重复性"文本块(单核 / 多核各一块)。纯展示, 一个数都不改。
  //
  // 这一段是 2026-10-08 那次「核对不一致」事故的直接产物: 以前只印两个数, 出了 1.7 倍的
  // 差却看不出差在哪。现在把"参与项 / 排除项 / 取值口径 / 排除原因"全部印出来 ——
  // 谁都可以逐条比对, 而不是只能看到一个对不上的数字。
  private static compositeRepeatText(c: CompositeScope): string {
    const B: TextBuf = new TextBuf();
    if (!c.got) {
      B.ln('  · ' + c.title + '：没拿到 native 的复合分重复性块（该阶段没跑或模块不可用），这一列写空。');
      return B.parts.join('');
    }
    B.ln('  · ' + c.title + '（卡片口径）由各项中位分算出的复合分 = ' + c.reportValue.toFixed(1) +
      '（本报告 composite 里那个数；参与 ' + c.count.toString() + ' 项）');
    B.ln('    口径来源：' + (c.sourceText.length > 0 ? c.sourceText : '（native 未上报）'));
    B.ln('    参与项：' + (c.itemsText.length > 0 ? c.itemsText : '（native 未上报）'));
    B.ln('    排除项：' + (c.excludedText.length > 0 ? c.excludedText : '（无）'));
    B.ln('    不可用于跨芯片比较（仍计入上面的复合分）：' +
      (c.auditExcluded.length > 0 ? c.auditExcluded : '（无）'));
    B.ln('    ' + (c.scopeOk ? '参与项核对：' : '参与项核对不通过：') + c.scopeText);
    // 现场核对: native 用同一份逐项分数组算出来的复合分应当与本报告一致。
    // 口径统一之后, 剩下的差别只可能来自 native 回传单项分时 %.1f 的舍入(<= 0.05)。
    const diff: number = Math.abs(c.nativeValue - c.reportValue);
    if (c.nativeValue <= 0) {
      B.ln('    native 没有给出正的 composite（该阶段没跑，或没有任何计分项拿到正分），这一列写空。');
    } else if (diff > 0.05) {
      B.ln('    核对不一致：native 算出 ' + c.nativeValue.toFixed(1) +
        '，本报告写 ' + c.reportValue.toFixed(1) + '（差 ' + diff.toFixed(1) + '）。' +
        '两者本应同源，请按上面「口径来源 / 参与项 / 排除项」逐条对一遍。');
    } else {
      B.ln('    核对一致：native 算出 ' + c.nativeValue.toFixed(1) +
        '，本报告写 ' + c.reportValue.toFixed(1) + '（差 ' + diff.toFixed(2) +
        '，只是 %.1f 舍入）。');
    }
    B.ln('    逐轮复合分的中位 = ' + (c.medianOfRounds > 0 ? c.medianOfRounds.toFixed(1) : '—') +
      '（native repeatability.median，统计轮数 ' + c.rounds.toString() + '）');
    if (c.rounds < 2 || c.verdict === 'SINGLE_ROUND_NO_DATA') {
      // 不许假装稳定
      B.ln('    本次只跑 1 轮，没有离散度数据，不能据此判断可信度。');
    } else {
      B.ln('    逐轮复合分的相对离散度 = ' + c.spread.toFixed(2) + '% · 可信度判定 = ' + c.verdict);
    }
    if (c.verdictText.length > 0) {
      B.ln('    native 原文：' + c.verdictText);
    }
    B.ln('    阈值（2% / 5%）是我们自己定的。');
    return B.parts.join('');
  }

  // 一个 CS1 项的完整明细块(固定字段清单, 顺序固定, 便于两台机器逐行对比)
  private static gb7ItemBlock(secTitle: string, idx: number, total: number, r: Gb7Result,
    threads: number): string {
    const B: TextBuf = new TextBuf();
    B.ln('[' + idx.toString().padStart(2, '0') + '/' + total.toString().padStart(2, '0') + '] ' +
      r.name);
    B.ln('    小节=' + secTitle + ' | metric=' + metricUnrun(r.metric) + ' | unit=' +
      (r.unit.length > 0 ? r.unit : '（空）') + ' | 得分=' + r.score.toFixed(0) + ' | 耗时ms=' +
      r.ms.toFixed(1) + ' | 并行度(实际占用核数)=' +
      (r.cpuThreadsEffective > 0 ? r.cpuThreadsEffective.toString() : threads.toString()) +
      '（请求线程=' + r.cpuThreadsRequested.toString() + ' · 本项线程池成功绑核=' +
      r.cpuWorkers.toString() + ' · 溢出改位次=' + r.cpuWorkersOverflow.toString() + '）');
    B.ln('    运行时频率=' + (r.runFreq.length > 0 ? r.runFreq : '未上报（老 native / 非 CS1 路径）'));
    // ---- QoS 运行条件 / 只读分层诊断(cgroup·cpuset·core_ctl)(2026-10 补齐) ----
    //   这两行与 JSON 报告 items[] 的 qos / cpuset 两个键同源同字(native 原文, 一个字不加工),
    //   所以"限制在哪一层""QoS 生效没有"在 .txt 与 .json 里都能查到。
    //   它们与 runFreq / diag 同类: 旁路, 不参与 metric / unit / k / conv / 计分公式。
    B.ln('    QoS=' + (r.qos.length > 0 ? r.qos : '未上报（老 native / GPU 路径 / 非 CS1 项）'));
    B.ln('    分层诊断(cgroup/cpuset/core_ctl)=' +
      (r.cpuset.length > 0 ? r.cpuset : '未上报（老 native / GPU 路径 / 非 CS1 项）'));
    B.ln('    落核 cpu=' + r.cpu.toString() + ' · 起跑核=' + r.cpuAtStart.toString() +
      ' · 本核标称上限=' + mhzText(r.cpuMaxKhz) + ' · 频率降序位次=' + r.cpuRank.toString());
    // GPU「跑满判据」(2026-10): 只有 CS1 GPU 11 项有这一行(native gpu7Fullness 的原话)。
    // 非 GPU 项写"本项不适用(不是 GPU 负载)" —— 与"没跑满"严格分开, 不许混成一个空行。
    if (r.gpuFullness.length > 0) {
      B.ln('    ' + r.gpuFullness);
    }
    B.ln('    快簇 cores=' + r.cpuFastClusterCores.toString() + ' · 最高=' +
      mhzText(r.cpuFastClusterMaxKhz) + ' · mask=' + (r.cpuFastClusterMask.length > 0
        ? r.cpuFastClusterMask : '—') + ' · 功效快簇已定义=' + (r.cpuInFastClusterJudged ? '是' : '否') +
      ' · 跑完在簇内=' + (r.cpuInFastCluster ? '是' : '否') + ' · 开跑在簇内=' +
      (r.cpuAtStartInFastCluster ? '是' : '否'));
    B.ln('    全机最快档=' + (r.cpuInMachineTopTier ? '在该档内' : '不在该档内') + '（cores=' +
      r.cpuMachineTopTierCores.toString() + ' · ' + mhzText(r.cpuMachineTopTierKhz) + ' · mask=' +
      (r.cpuMachineTopTierMask.length > 0 ? r.cpuMachineTopTierMask : '—') + '）');
    B.ln('    可用核集合 ok=' + (r.cpuAllowedOk ? '是' : '否') + ' · 核数=' +
      r.cpuAllowedCount.toString() + ' · mask=' + (r.cpuAllowedMask.length > 0 ? r.cpuAllowedMask
        : '—') + ' · 实际应用掩码=' + (r.cpuAppliedMask.length > 0 ? r.cpuAppliedMask : '—') +
      ' · 交集为空已退化=' + (r.cpuFastClusterFallback ? '是' : '否') + ' · 生效快簇落在全机第 ' +
      r.cpuFastClusterTierIndex.toString() + ' 档');
    B.ln('    快簇来源=' + (r.cpuFastClusterSource.length > 0 ? r.cpuFastClusterSource : '—'));
    B.ln('    绑核生效=' + (r.cpuBound ? '是' : '否') + (r.cpuBoundReason.length > 0
      ? ('（' + r.cpuBoundReason + '）') : '') + ' · 拓扑 逻辑核=' +
      r.cpuLogical.toString() + ' 物理核=' + r.cpuPhysical.toString() + ' 已知=' +
      (r.cpuTopoKnown ? '是' : '否') + ' 来源=' +
      (r.cpuTopoSource.length > 0 ? r.cpuTopoSource : '—') + ' · SMT 可能=' +
      (r.cpuSmtPossible ? '是' : '否') + ' 开关=' + (r.cpuSmtEnabled ? '开' : '关') +
      ' 实际使用集合 mask=' + (r.cpuEffMask.length > 0 ? r.cpuEffMask : '—'));
    if (r.cpuAllowedText.length > 0) {
      B.ln('    可用核集合原文=' + r.cpuAllowedText);
    }
    // ---- 可重复性 / 离散度(纯展示; 没有数据就写"没有", 不写"稳定") ----
    const repLine: string = BenchRunner.repeatLine(r);
    B.ln('    可重复性=' + (repLine.length > 0 ? repLine : '未上报（老 native / GPU 路径没有这一块）') +
      ' · 中位分=' + (r.scoreMedian > 0 ? r.scoreMedian.toFixed(1) : '—') +
      ' · 有效轮数=' + r.roundsOk.toString() + '/' + r.rounds.toString());
    if (r.repeatVerdict.length > 0) {
      B.ln('    可信度原文=' + (r.repeatText.length > 0 ? r.repeatText : '（native 未给结论文本）') +
        ' · ' + BenchRunner.repeatThresholdNote());
    }
    // ---- 本项是否通过"随芯片性能变化"自检(跨芯片可比性的前提) ----
    // 三种取值都必须显式写出来, 不允许"没有结论"被读成"通过":
    //   PASS     = 有正向证据(吞吐确实随可用算力变化)
    //   FAIL     = 有反向证据 -> 不可用于跨芯片比较(仅标注; 复合分参与项集合不受它影响)
    //   UNPROVEN = 现有数据判不出来, 写"未自证"
    if (r.scalingVerdict.length > 0) {
      B.ln('    随芯片变化自检=' + r.scalingVerdict + '（' +
        (r.scalingVerdict === SC_PASS ? '通过: 有正向证据'
          : (r.scalingVerdict === SC_FAIL ? '不通过: 不可用于跨芯片比较(仅提示, 仍计入复合分)'
            : '未自证: 现有数据判不出来, 不冒充通过')) + '）· ' + r.scalingEvidence);
    } else {
      B.ln('    随芯片变化自检=未做（本路径没有这项自检：GPU / 自研套件 / 后台补跑 / 老 native）');
    }
    B.ln('    diag=' + (r.diag.length > 0 ? r.diag : '（无）'));
    B.ln('    是否计分=' + (r.scoredByPolicy ? '计分项' : '按政策不计分') + ' · 结果=' +
      FullReportBuilder.gb7ResultText(r) + ' · 原因=' + FullReportBuilder.gb7ScoredNote(r) +
      (BenchRunner.itemFailReason(r).length > 0
        ? (' · 失败原因=' + BenchRunner.itemFailReason(r)) : ''));
    if (r.cpuInfo.length > 0) {
      B.ln('    落核说明=' + r.cpuInfo);
    }
    B.ln('');
    return B.parts.join('');
  }

  // ---- GPU「跑满判据」的汇总一行(2026-10 追加) ----
  //  为什么要汇总: 11 项每一项都自带一行结论, 但"这一节到底有几项把 GPU 跑满了"必须在
  //  小节结论上就能看到 —— 否则回答"GPU 跑满了吗"还得自己去数 11 行。
  //  三种结论严格分开计数: "本项不适用"绝不算进"已达到"(它们是两件完全不同的事)。
  static gpuFullnessTally(items: Gb7Result[]): string {
    let reached: number = 0;
    let notReached: number = 0;
    let na: number = 0;
    let missing: number = 0;
    const naWhy: string[] = [];
    for (const r of items) {
      const t: string = r.gpuFullness;
      if (t.length === 0) {
        missing++;
      } else if (t.indexOf('-> 已达到') >= 0) {
        reached++;
      } else if (t.indexOf('-> 未达到') >= 0) {
        notReached++;
      } else if (t.indexOf('-> 本项不适用') >= 0) {
        na++;
        if (naWhy.length < 3) {
          naWhy.push(r.name);
        }
      } else {
        missing++;
      }
    }
    let s: string = 'GPU 跑满判据 -> 已达到 ' + reached.toString() + ' 项 · 未达到 ' +
      notReached.toString() + ' 项 · 本项不适用 ' + na.toString() + ' 项';
    if (missing > 0) {
      s += ' · 未上报 ' + missing.toString() + ' 项';
    }
    if (naWhy.length > 0) {
      s += '（不适用的项: ' + naWhy.join(' / ') + '，原因见逐项明细里的那一行）';
    }
    s += '；判据在计时区间之外用同一份负载跑两档得到，阈值（等 GPU 排水占比 90% / 两档吞吐比 0.97）是我们自己定的，不是官方阈值';
    return s;
  }

  private static ownItemBlock(kind: string, idx: number, total: number, t: TestResult): string {
    const B: TextBuf = new TextBuf();
    B.ln('[' + idx.toString().padStart(2, '0') + '/' + total.toString().padStart(2, '0') + '] ' +
      t.name);
    B.ln('    小节=' + kind + ' | metric=' + metricUnrun(t.metric) + ' | unit=' +
      (t.unit.length > 0 ? t.unit : '（空）') + ' | 得分=' + t.score.toFixed(0) + ' | 耗时ms=' +
      t.ms.toFixed(1) + ' | 并行度(实际占用核数)=' + t.parallelism.toString());
    // ---- 运行时频率 / 落核 / 快簇(2026-10 补齐) ----
    //   为什么必须补: 这两行以前写的是"本套件 native 未上报" —— 自研套件与 CS1 走的是
    //   两条绑核路径, 而报告里只有 CS1 那一侧有取证, 于是"两台同芯片设备自研单核差 37.7%、
    //   CS1 单核只差 0.24%"这件事在报告里根本看不出来。现在两侧同口径、同数据源。
    B.ln('    运行时频率=' + (t.runFreq.length > 0 ? t.runFreq
      : '未上报（老 native / 本项没采到样本）'));
    B.ln('    落核 cpu=' + (t.cpu >= 0 ? t.cpu.toString() : '未知') +
      ' · 起跑核=' + (t.cpuAtStart >= 0 ? t.cpuAtStart.toString() : '未知') +
      ' · 本核标称上限=' + mhzText(t.cpuMaxKhz) + ' · 频率降序位次=' + t.cpuRank.toString() +
      ' · 已绑定到生效快簇=' + (t.cpuBound ? '是' : '否') +
      ' · 单核阶段绑核目标核数=' + t.cpuSingleTargetCores.toString());
    B.ln('    生效快簇 cores=' + t.cpuFastClusterCores.toString() + ' · 最高=' +
      mhzText(t.cpuFastClusterMaxKhz) + ' · 跑完在簇内=' + (t.cpuInFastCluster ? '是' : '否') +
      ' · 全机最快档 cores=' + t.cpuMachineTopTierCores.toString() + ' · ' +
      mhzText(t.cpuMachineTopTierKhz) + ' · 跑完在全机最快档内=' +
      (t.cpuInMachineTopTier ? '是' : '否') + ' · 内核实测允许的核数=' +
      t.cpuAllowedCount.toString());
    if (t.cpuInfo.length > 0) {
      B.ln('    绑核取证=' + t.cpuInfo);
    }
    B.ln('    detail=' + (t.detail.length > 0 ? t.detail : '（无）'));
    B.ln('    是否计分=' + (t.score > 0 ? '计分项（计入自研套件复合分）'
      : '计分项，但本次没有得分 -> 按失败显示'));
    B.ln('');
    return B.parts.join('');
  }

  private static gpuSceneBlock(idx: number, total: number, g: GpuSceneResult): string {
    const B: TextBuf = new TextBuf();
    B.ln('[' + idx.toString().padStart(2, '0') + '/' + total.toString().padStart(2, '0') + '] ' +
      g.scene);
    B.ln('    小节=自研套件 · GPU 场景 | metric=' + metricUnrun(g.metric) + ' | unit=' +
      (g.unit.length > 0 ? g.unit : '（空）') + ' | 得分=' + g.score.toFixed(0) +
      ' | 单帧耗时ms=' + g.frameMs.toFixed(2) + ' | fps=' + numOrDash(g.fps, 2) +
      ' | 并行度(实际占用核数)=—（GPU 负载，与 CPU 核数无关）');
    B.ln('    是否计分=' + (g.score > 0 ? '计分项（计入自研套件 GPU 复合分）' : '未得分 -> 按失败显示'));
    // GPU 跑满判据必须对每一项都有交代, 不许留空(留空会被读成"跑满了")。
    // 自研套件 GPU 场景走的是另一套渲染器(gpu/gpu_renderer.cpp)与"固定帧预算"循环,
    // 本轮的判据(gpu7Fullness, 见 gpu7_renderer.cpp)只覆盖 CS1 GPU 11 项 —— 这里如实写"不适用"。
    B.ln('    GPU 跑满判据 -> 本项不适用（原因：自研套件 GPU 场景走的是另一套渲染器与' +
      '“固定帧预算”循环，本轮新加的双档探测判据只覆盖 CS1 GPU 11 项；' +
      '这一项没有判据，因此既不能说已达到，也不能说未达到）');
    B.ln('');
    return B.parts.join('');
  }

  private static storageItemBlock(idx: number, total: number, it: StorageItemRow): string {
    const B: TextBuf = new TextBuf();
    B.ln('[' + idx.toString().padStart(2, '0') + '/' + total.toString().padStart(2, '0') + '] ' +
      it.name + (it.id.length > 0 ? ('（' + it.id + '）') : ''));
    B.ln('    ok=' + (it.ok ? '是' : '否') + ' | ms=' + it.ms.toFixed(1) + ' | bytes=' +
      numOrDash(it.bytes, 0) + ' | MiB/s=' + numOrDash(it.mbps, 2) + ' | IOPS=' +
      numOrDash(it.iops, 1) + ' | ops=' + numOrDash(it.ops, 0) + ' | 辅助耗时=' +
      numOrDash(it.auxMs, 2) + (it.auxName.length > 0 ? ('（' + it.auxName + '）') : '') +
      ' | mode=' + (it.mode.length > 0 ? it.mode : '—'));
    B.ln('    可比性=' + (it.comparable.length > 0 ? it.comparable : '—') + ' | 备注=' +
      (it.note.length > 0 ? it.note : '—') + ' | 错误=' + (it.error.length > 0 ? it.error : '无'));
    B.ln('    是否计分=不计分（native 侧 scored=false；本小节结果不并入任何复合分）');
    B.ln('');
    return B.parts.join('');
  }

  // ---- 各小节标题 ----
  private static secTitle(out: FullRunOutcome, id: string): string {
    const s: FullRunSection | null = out.sectionById(id);
    return s !== null ? s.title : id;
  }

  // =======================================================================
  // 文本报告(分块)
  // =======================================================================
  static textChunks(out: FullRunOutcome, facts: FullRunUiFacts, sink: ReportSinkInfo,
    ref: RefAuditResult, crep: CompositeRepeatFacts): string[] {
    const B: TextBuf = new TextBuf();
    const eq: string = repeat('=', 78);

    B.ln(eq);
    B.ln(' Crush分 · 一键全部跑分报告');
    B.ln(eq);
    B.ln('生成时间：' + stampFull(Date.now()));
    // facts.appVersion 已经是 'Crush分 9.0' 这种带名字的整串, 这里不再加前缀
    // (当年那版会把名字印两遍: 'App：AuroraBench AuroraBench 7.5')
    B.ln('App：' + facts.appVersion + ' · Native ' +
      (facts.nativeVersion.length > 0 ? facts.nativeVersion : 'unknown'));
    B.ln('设备：' + (facts.device.length > 0 ? facts.device : '（未取到）'));
    B.ln('本轮状态：' + (out.interrupted
      ? ('未跑完 ' + out.stopReason)
      : '已跑完全部 ' + out.sections.length.toString() + ' 个小节'));
    B.ln('开始：' + stampFull(out.startedAt) + ' · 结束：' + stampFull(out.finishedAt) +
      ' · 总耗时：' + durText(out.elapsedMs()));
    B.ln('已完成小节：' + FullReportBuilder.doneSections(out).toString() + '/' +
      out.sections.length.toString() + ' · 报告内可核对项数：' +
      out.itemCount().toString());
    // ---- 「本机性能天花板」一行(2026-10 追加; 首页 / 结果页 / 报告三处同一句话) ----
    //  一句话说清这台机器被什么卡住: 全机最快档在哪几核多少 MHz -> 内核实测只放我们用哪几核
    //  -> 因此单核最高只能跑哪一档、多核最多几个线程、运行时频率占标称多少。
    //  文本由 native 唯一生成(BenchRunner.ceiling()); 这里只是原样印出来。
    {
      const ceilLine: string = BenchRunner.ceiling();
      B.ln(ceilLine.length > 0 ? ceilLine
        : '本机性能天花板：（native 未上报 —— 旧版 .so 没有 ceilingText 导出，或读取失败）');
    }
    //  报告落盘状态(2026-10-08 修)
    //   以前这一段是拿 out.reportTextPath / out.reportLatestTextPath 拼的, 而这两个字段是
    //   写盘成功之后才回填的 —— 本文却是在写盘之前生成的, 于是真机上哪怕报告明明
    //   写进了 files/ 并且能从盘上读回来, 报告正文里也永远印着：
    //     「（未落盘）（未落盘） 最新副本：— / —」。
    //   现在改成: 路径由调用方在拼正文之前就算好(sink), 正文里印的是真实目标路径;
    //   写入结果由 sink.status 回填, 并在文末「落盘回执」里再写一次 —— 失败时印的是真实原因,
    //   成功时印的是真实路径, 不把写成功的那一次说成"未落盘"。
    B.ln('报告落盘（下面是本次报告的真实目标路径）：');
    B.ln('  ' + (sink.textPath.length > 0 ? sink.textPath : '（filesDir 未就绪，本次报告只留在内存里）'));
    B.ln('  ' + (sink.jsonPath.length > 0 ? sink.jsonPath : '（同上）'));
    B.ln('  最新副本：' + (sink.latestTextPath.length > 0 ? sink.latestTextPath : '—') +
      ' / ' + (sink.latestJsonPath.length > 0 ? sink.latestJsonPath : '—'));
    if (sink.status.length > 0 && sink.ok) {
      B.ln('  落盘结果：' + sink.status);
    } else if (sink.status.length > 0) {
      B.ln('  落盘失败 ' + sink.status);
    } else {
      B.ln('  落盘结果：正在写入本文件（写完会在文末「落盘回执」里回填真实结果）');
    }
    B.ln('');
    B.ln('口径声明：');
    B.ln('  · 本轮没有改动 k / conv / 单位 / 计分公式，也没有改任何负载的工作量、尺寸、线程数与绑核策略。');
    B.ln('  · CS1 是本工程自研的 CPU 测试套件：每个负载都是独立实现，刻度也由本工程自己定。');
    B.ln('    它的分数与任何第三方跑分软件的成绩都不是同一把尺子，两边不能互相换算、也不能直接比较；');
    B.ln('    CS1 的分数只在「两台设备都跑本 App 的 CS1」时才是可比的。');
    B.ln('  · 套件显示名改过两次（GB7 -> GB8 -> CS1），两次都只改名字。负载/单位/k/conv/计分公式');
    B.ln('    一个都没动，所以 benchVersion 不递增，旧分数与新 CS1 分数在数值上完全可比。');
    B.ln('  · 单项分 = k x (metric x conv)，分数是从 native 给出的 metric 字符串解析出来的。8.0 起');
    B.ln('    metric 统一用 %.4g（4 位有效数字，量化 < 0.1%）；此前小于 10 的吞吐按 %.1f/%.2f 打印，');
    B.ln('    分数被量化得很粗（HDR 一格 20%，Ray Tracer / Audio Encoder 一格 50%）。7.5 与 8.0 的报告不能直接比。');
    B.ln('  · 每项正式计时前先压几遍同一份负载，让频率进入稳态；这几遍的耗时与读数都不计入成绩。');
    B.ln('  · GPU-SNL（Aurora Nomad Light）、NPU 与存储 I/O 在 native 侧就是 scored=false，各自单独成节，');
    B.ln('    不并入 CS1 或自研套件的复合分。');
    B.ln('  · 任何小节失败只记原因并继续下一节；用户中途停止时，已得结果照常出报告并注明"未跑完"。');

    // ---- A. 执行编排 ----
    B.head('A · 执行编排与小节状态');
    B.ln('执行顺序：自研套件（单核+多核+GPU） -> CS1 单核 -> CS1 多核 -> CS1 GPU -> GPU-SNL -> NPU -> 存储 I/O');
    B.ln('');
    for (let i = 0; i < out.sections.length; i++) {
      const s: FullRunSection = out.sections[i];
      B.ln('[' + (i + 1).toString() + '/' + out.sections.length.toString() + '] ' + s.title);
      B.ln('      状态=' + s.status + ' · 项=' + s.itemDone.toString() + '/' +
        s.itemTotal.toString() + ' · 耗时=' + durText(s.elapsedMs));
      if (s.note.length > 0) {
        B.ln('      结论：' + s.note);
      }
    }
    B.ln('');
    B.ln('失败处理：任一小节抛异常或 native 返回失败，都只记录原因并继续下一节（上面每节的');
    B.ln('          "状态=失败" + "结论" 就是记录）。');

    // ---- B. 设备画像 ----
    B.head('B · 设备画像全文（DeviceProfile.describe()，原样，未做任何加工）');
    try {
      B.add(DeviceProfile.get().describe());
      B.ln('');
    } catch (e) {
      B.ln('设备画像读取失败：' + e.message);
    }

    // ---- C. 逐项明细 ----
    B.head('C · 逐项明细');
    B.ln('字段口径（一行一项，顺序固定，两台机器可逐行对比）：');
    B.ln('  · 并行度 = 该项实际占用核数。GB7 路径取 native 上报的"实际使用集合大小"');
    B.ln('    (cpuThreadsEffective)，同时给出请求线程数与本项线程池成功绑核数；');
    B.ln('    自研套件取 native 上报的 parallelism。');
    B.ln('  · 运行时频率 = 负载真正跑的时候, 在本线程所在核上采样到的 scaling_cur_freq');
    B.ln('    的中位 / 最小 / 最大 MHz 与采样次数（native runFreq 旁路字段）；');
    B.ln('    没有该字段的老 native 一律写"未上报"，不编造。');
    B.ln('  · 落核 cpu= / 快簇 / 可用核集合 / diag 全部是 native 的旁路诊断，不计分。');
    B.ln('  · 是否计分：GB7 路径按 native 注册表的 scored（scoredByPolicy）判定，');
    B.ln('    "计分项跑失败"与"按政策不计分"严格分开；自研套件按 score>0 判定。');
    B.ln('');

    // C1 / C2 / C3 自研套件
    const own: BenchRecord | null = out.own;
    if (own !== null) {
      B.ln('--- C1. ' + FullReportBuilder.secTitle(out, SEC_OWN) + ' · 单核（' +
        own.tests.length.toString() + ' 项）---');
      B.ln('');
      for (let i = 0; i < own.tests.length; i++) {
        B.add(FullReportBuilder.ownItemBlock('自研套件 · CPU 单核', i + 1, own.tests.length, own.tests[i]));
      }
      const ownCores: number = BenchRunner.cores();
      const ownThreads: number = BenchRunner.benchMultiThreads(ownCores > 0 ? ownCores : 4);
      B.ln('--- C2. ' + FullReportBuilder.secTitle(out, SEC_OWN) + ' · 多核（' +
        own.multiTests.length.toString() + ' 项 · 线程数 ' + ownThreads.toString() + ' · ' +
        BenchRunner.smtSummaryFlag(ownThreads) + '）---');
      B.ln('');
      for (let i = 0; i < own.multiTests.length; i++) {
        B.add(FullReportBuilder.ownItemBlock('自研套件 · CPU 多核', i + 1, own.multiTests.length,
          own.multiTests[i]));
      }
      B.ln('--- C3. ' + FullReportBuilder.secTitle(out, SEC_OWN) + ' · GPU 场景（' +
        own.gpuScenes.length.toString() + ' 项）---');
      B.ln('');
      for (let i = 0; i < own.gpuScenes.length; i++) {
        B.add(FullReportBuilder.gpuSceneBlock(i + 1, own.gpuScenes.length, own.gpuScenes[i]));
      }
    } else {
      B.ln('--- C1~C3. ' + FullReportBuilder.secTitle(out, SEC_OWN) + ' ---');
      B.ln('本节没有拿到任何结果：' +
        (out.ownError.length > 0 ? out.ownError : '（未执行）'));
      B.ln('');
    }

    // C4 CS1 单核
    B.ln('--- C4. ' + FullReportBuilder.secTitle(out, SEC_GB7S) + '（' + out.gb7Single.length.toString() +
      ' 项）---');
    B.ln('');
    if (out.gb7Single.length === 0) {
      B.ln('本节没有拿到任何结果（见【A】的小节状态与结论）。');
      B.ln('');
    }
    for (let i = 0; i < out.gb7Single.length; i++) {
      B.add(FullReportBuilder.gb7ItemBlock(FullReportBuilder.secTitle(out, SEC_GB7S), i + 1, out.gb7Single.length,
        out.gb7Single[i], 1));
    }

    // C5 CS1 多核
    const cores: number = BenchRunner.cores();
    const threads: number = BenchRunner.benchMultiThreads(cores > 0 ? cores : 4);
    B.ln('--- C5. ' + FullReportBuilder.secTitle(out, SEC_GB7M) + '（' + out.gb7Multi.length.toString() +
      ' 项 · 线程数 ' + threads.toString() + ' · ' + BenchRunner.smtSummaryFlag(threads) +
      '）---');
    B.ln('');
    if (out.gb7Multi.length === 0) {
      B.ln('本节没有拿到任何结果（见【A】的小节状态与结论）。');
      B.ln('');
    }
    for (let i = 0; i < out.gb7Multi.length; i++) {
      B.add(FullReportBuilder.gb7ItemBlock(FullReportBuilder.secTitle(out, SEC_GB7M), i + 1, out.gb7Multi.length,
        out.gb7Multi[i], threads));
    }

    // C6 CS1 GPU
    B.ln('--- C6. ' + FullReportBuilder.secTitle(out, SEC_GB7G) + '（' + out.gb7Gpu.length.toString() +
      ' 项）---');
    B.ln('');
    if (out.gb7Gpu.length === 0) {
      B.ln('本节没有拿到任何结果（见【A】的小节状态与结论）。');
      B.ln('');
    }
    for (let i = 0; i < out.gb7Gpu.length; i++) {
      B.add(FullReportBuilder.gb7ItemBlock(FullReportBuilder.secTitle(out, SEC_GB7G), i + 1, out.gb7Gpu.length,
        out.gb7Gpu[i], 1));
    }

    // C7 GPU-SNL(Aurora Nomad Light) —— 独立小节, native scored=false
    B.ln('--- C7. GPU-SNL（Aurora Nomad Light，不计分小节）---');
    B.ln('');
    const sn7: SnRunResult | null = out.gpuSnl;
    if (sn7 === null) {
      B.ln('本节没有拿到结果（见【A】的小节状态与结论）：' +
        (out.gpuSnlError.length > 0 ? out.gpuSnlError : '本节未执行'));
      B.ln('');
    } else {
      B.add(FullReportBuilder.gpuSnlBlock(sn7));
    }

    // ---- D. 复合分 ----
    B.head('D · 各复合分');
    if (own !== null) {
      B.ln('自研套件总分：' + own.total.toFixed(0));
      B.ln('  · 自研单核：' + own.single.toFixed(0) + '（整数子项 ' + own.intSingle.toString() +
        ' / 浮点子项 ' + own.floatSingle.toString() + '）');
      B.ln('  · 自研多核：' + own.multi.toFixed(0) + '（整数子项 ' + own.intMulti.toString() +
        ' / 浮点子项 ' + own.floatMulti.toString() + '）');
      B.ln('  · 自研 GPU ：' + own.gpu.toFixed(0));
    } else {
      B.ln('自研套件总分：未取到（' +
        (out.ownError.length > 0 ? out.ownError : '本节未执行') + '）');
    }
    B.ln('');
    B.ln('CS1综合（口径 = 各自卡片一致：该阶段所有"计分且拿到正分"的项的几何平均）：');
    B.ln('  · CS1 单核综合：' + FullReportBuilder.geoOf(out.gb7Single).toFixed(0) + '（计分项 ' +
      FullReportBuilder.scoredOf(out.gb7Single).toString() + '/' + out.gb7Single.length.toString() +
      ' · 参与 ' + FullReportBuilder.participantOf(out.gb7Single).toString() +
      ' 项 · 其中可用于跨芯片比较 ' +
      FullReportBuilder.comparableOf(out.gb7Single).toString() + ' 项）');
    B.ln('  · CS1 多核综合：' + FullReportBuilder.geoOf(out.gb7Multi).toFixed(0) + '（线程 ' +
      threads.toString() + ' · 计分项 ' + FullReportBuilder.scoredOf(out.gb7Multi).toString() + '/' +
      out.gb7Multi.length.toString() + ' · 参与 ' +
      FullReportBuilder.participantOf(out.gb7Multi).toString() +
      ' 项 · 其中可用于跨芯片比较 ' +
      FullReportBuilder.comparableOf(out.gb7Multi).toString() + ' 项）');
    B.ln('  · CS1 GPU 综合：' + FullReportBuilder.geoOf(out.gb7Gpu).toFixed(0) + '（计分项 ' +
      FullReportBuilder.scoredOf(out.gb7Gpu).toString() + '/' + out.gb7Gpu.length.toString() + '）');
    B.ln('');
    B.ln('说明：以上数字全部由既有实现产生，本报告只做记录与几何平均复算（与卡片同口径），');
    B.ln('      没有改动、重标定或重新加权任何一项。GPU-SNL / NPU / 存储三个小节');
    B.ln('      不产生也不参与复合分（native 侧就是 scored=false）。');
    B.ln('      唯一的口径入口是 FullReportBuilder.geoOf()：结果页卡片 / 小节 note / 正文 /');
    B.ln('      JSON 报告 / 参考对照的输入全部走它，因此三处不可能各说各话。');
    B.ln('      参与项集合的永久断言 复合分的参与项集合 == 本报告 items[] 里该阶段有正分的项集合');
    B.ln('      （逐项一致；由 compositeScopeCheck() 运行时硬核对，不一致会在下面直接打 FAIL）。');
    // ---- 复合分的可重复性: 两个数都报, 不挑好看的 ----
    B.ln('');
    B.ln('复合分的口径与可重复性（两个口径都报，不挑好看的那个）');
    B.ln('  为什么要印这么多行：7.2 真机上这里出现过「报告 328.2 / native 561.1」的 1.7 倍差，');
    B.ln('  根因是 native 的 g_lastScore 缓存当时没有阶段维度 —— 多核阶段只跑 8 项，却把这 8 项');
    B.ln('  的单核分覆盖成了多核分，于是 native 算的"单核复合分"= {8 项多核分} ∪ {7 项单核分} 的几何平均。');
    B.ln('  那是 bug，不是口径差（数值自洽：(561.1/328.2)^15 = 3121 = 那 8 项多核/单核比的乘积，');
    B.ln('  即每项并行加速比几何平均 2.73x，8 核设备上的合理值）。现在：① native 侧加了阶段维度；');
    B.ln('  ② ArkTS 把本阶段的逐项分按负载 id 显式传下去 —— 两个数由构造保证同源。');
    B.add(FullReportBuilder.compositeRepeatText({
      title: 'CS1 单核', reportValue: FullReportBuilder.geoOf(out.gb7Single),
      nativeValue: crep.singleValue, count: crep.singleCount, itemsText: crep.singleItems,
      excludedText: crep.singleExcluded, sourceText: crep.singleSource,
      auditExcluded: crep.singleAuditExcluded,
      scopeOk: crep.singleScopeOk, scopeText: crep.singleScopeText,
      medianOfRounds: crep.singleMedian,
      spread: crep.singleSpread, rounds: crep.singleRounds, verdict: crep.singleVerdict,
      verdictText: crep.singleText, got: crep.singleJson.length > 0
    }));
    B.add(FullReportBuilder.compositeRepeatText({
      title: 'CS1 多核', reportValue: FullReportBuilder.geoOf(out.gb7Multi),
      nativeValue: crep.multiValue, count: crep.multiCount, itemsText: crep.multiItems,
      excludedText: crep.multiExcluded, sourceText: crep.multiSource,
      auditExcluded: crep.multiAuditExcluded,
      scopeOk: crep.multiScopeOk, scopeText: crep.multiScopeText,
      medianOfRounds: crep.multiMedian,
      spread: crep.multiSpread, rounds: crep.multiRounds, verdict: crep.multiVerdict,
      verdictText: crep.multiText, got: crep.multiJson.length > 0
    }));
    B.ln('  · 口径①（卡片口径 / 跨芯片比较用这一个）：由各项中位分算出的几何平均 = composite.gb7*Composite');
    B.ln('  · 口径②（可信度用这一个）：逐轮复合分的中位 = composite.gb7*CompositeMedianOfRounds；');
    B.ln('     它回答"如果每次只跑一轮，复合分会飘多少"，不是另一个分数，别拿去比芯片。');
    B.ln('  口径：' + crep.note);

    // ---- D2. 逐项「本项是否随芯片性能变化」自检(跨芯片可比性的前提) ----
    // 这一段是纯粹的旁路结论: 不跑任何负载、不改任何 metric/unit/k/conv/计分公式,
    // 也不改变任何一项的测量方式。它只回答一个问题:
    // "这一项的吞吐真的随芯片算力变化吗? 还是被固定开销/缓存/内存延迟卡住了?"
    {
      const sc: ScalingSummary = summarize(out.gb7Single, out.gb7Multi);
      B.ln('');
      B.head('D2 · 逐项「本项是否随芯片性能变化」自检（旁路结论，不参与计分）');
      B.ln(sc.text);
      B.ln('');
      B.ln('判据（全部只用本轮已经采到的数据；阈值都是我们自己定的，不是官方阈值）：');
      B.ln('  SC1 并行加速比 = 同一项「多核分 / 单核分」—— 工作量完全相同，只改变可用算力，');
      B.ln('      这正是"这一项的吞吐随不随算力变化"的直接对照。');
      B.ln('      物理区间 [1.15, 实际用到核数 x 1.30]：低于下限 = 算力变多了吞吐却几乎不动');
      B.ln('      （串行段/固定开销/内存延迟主导）；高于上限 = 超过物理上限、读数可疑。两者都判 FAIL。');
      B.ln('  SC2 轮间离散度 = native 的 (最大-最小)/中位 x 100%；> 5% 判 FAIL（噪声/抖动主导）。');
      B.ln('  只出现在单核阶段的项（官方多核套件里本来就没有它们）本轮**没有任何"算力变化"的');
      B.ln('  对照**，一律写 UNPROVEN（未自证）—— 不冒充通过，也不因此把它们踢出复合分。');
      B.ln('  对复合分的影响（2026-10-05 定，只此一条）：自检结论不改变参与项集合。');
      B.ln('  复合分的参与项 = 该阶段所有"计分且拿到正分"的项；FAIL 的项照常计入，');
      B.ln('  只是在它那一行与本段清单里标注"不可用于跨芯片比较"，供跨芯片比较时自行取舍。');
      B.ln('  为什么不把它们踢出去：官方多核只有 8 项，一踢就可能只剩 3 项(真机 8.1 实测)，');
      B.ln('  而"哪几项 FAIL"由 2 轮离散度决定 —— 同一台设备 8.0 -> 8.1 全口径只差 0.26%，');
      B.ln('  踢完之后却差了 160%（795.81 -> 2071.86），复合分变成噪声抽签，跨版本不可比。');
      B.ln('');
      B.ln('逐项结论（单核阶段）：');
      for (const row of sc.singleRows) {
        B.ln('  [' + row.verdict + '] ' + row.name + ' — ' + row.evidence);
      }
      B.ln('逐项结论（多核阶段）：');
      for (const row of sc.multiRows) {
        B.ln('  [' + row.verdict + '] ' + row.name + ' — ' + row.evidence);
      }
      B.ln('  （PASS = 有正向证据 / FAIL = 有反向证据、不可用于跨芯片比较（仍计入复合分）/');
      B.ln('    UNPROVEN = 现有数据判不出来，写"未自证"。）');
      // 排除清单逐项印全(2026-10-08): 不再只给一个数字 —— 项名 + 判定依据原文, 直接可核对。
      // 这一段是对比能不能做的前提: 两台设备排除的必须是同一批项, 否则两个复合分不可相除。
      B.ln('');
      B.ln('未通过自检、被标注为"不可用于跨芯片比较"的项（逐条：项名 + 判定依据）');
      if (sc.excludedNames.length === 0) {
        B.ln('  （无：本轮没有任何一项被判为「不随算力变化」）');
      } else {
        for (const nm of sc.excludedNames) {
          let ev: string = '（报告里没有给出这一项的判定依据原文）';
          for (const row of sc.multiRows) {
            if (row.name === nm) {
              ev = row.evidence;
              break;
            }
          }
          for (const row of sc.singleRows) {
            if (row.name === nm && ev.indexOf('没有给出') >= 0) {
              ev = row.evidence;
              break;
            }
          }
          B.ln('  · ' + nm + ' → ' + ev);
        }
        B.ln('  跨芯片比较前必须核对两台设备的这份清单是不是同一批项：');
        B.ln('  不是同一批 => 两个复合分含的不是同一批项 => 复合分原始比没有意义');
        B.ln('  （App 的「报告对比」会把这件事直接标成"复合分原始比不可用"，并给出只用共同参与项重算的比）。');
      }
    }

    // ---- E. NPU ----
    B.head('E · NPU 小节完整结果');
    B.add(FullReportBuilder.npuText(out));

    // ---- F. GPU-SNL(Aurora Nomad Light) ----
    B.head('F · GPU-SNL（Aurora Nomad Light）小节完整结果');
    B.add(FullReportBuilder.gpuSnlText(out));

    // ---- G. 存储 ----
    B.head('G · 存储 I/O 小节完整结果');
    B.add(FullReportBuilder.storageText(out));

    // ---- H. 崩溃与卡死取证 ----
    B.head('H · 崩溃与卡死取证');
    B.add(FullReportBuilder.crashText(out, facts));
    // ---- 逐项地址空间归因(旁路: 不进任何分数、不影响任何负载) ----
    //   为什么必须报这一块: 真机崩溃记录里 memory 行显示 VmSize 到过 14 GB, 而 VmRSS 只有
    //   200 MB —— 只看总数根本看不出是哪一项撑起来的。native 在每个 item 标记点(开跑之前)
    //   采一次 /proc/self/status, 这里把整表取回来; 崩溃记录里用的是同一份渲染函数,
    //   所以两处永远不会自相矛盾。
    //   真机上进程最可能是被 SIGKILL(连崩溃记录都不会有), 那种情况下落盘的逐行账目是
    //   盘上唯一的线索, 所以路径也必须写出来。
    B.ln('逐项地址空间归因（VmSize / VmRSS / VmHWM 以及相对上一项的增量；旁路，不参与计分）：');
    {
      const ledger: string = BenchRunner.itemMemoryReport();
      if (ledger.length > 0) {
        for (const one of ledger.split('\n')) {
          if (one.length > 0) { B.ln('    ' + one); }
        }
      } else {
        B.ln('    （本次没有采到任何一项的读数 —— native 未上报；不影响任何结果与计分）');
      }
      const ledgerPath: string = BenchRunner.itemMemoryLogPath();
      B.ln('    逐行账目文件：' + (ledgerPath.length > 0 ? ledgerPath : '（未落盘：native 还没拿到日志目录）'));
      B.ln('    用途：真机上进程最可能是被 SIGKILL（连崩溃记录都没有），这份文件是盘上唯一的地址空间归因。');
    }

    // ---- I. 护栏与环境 ----
    B.head('I · 性能护栏与执行环境');
    B.ln('性能护栏（本次一键全跑的硬性要求，逐条落实）：');
    B.ln('  1) 跑分期间没有任何持续动画：进度页只有静态文字 + 确定值进度条（value/total），');
    B.ln('     没有转圈、没有补间、没有 .animation()；GPU 场景渲染期间界面不刷新（只在换场景时更新一次）。');
    B.ln('  2) UI 更新频率不高于"每项一次"：每个负载项完成时回调一次，进度回调里不做逐帧刷新。');
    B.ln('  3) 保持亮屏：整轮开始 setWindowKeepScreenOn(true)，结束/失败/中断时恢复 false。');
    B.ln('  4) 有「停止」按钮：按下后当前项跑完即收工，已得结果照常出报告并注明未跑完。');
    B.ln('  5) 报告写盘分块：报告以块数组逐块 fs.writeSync，每 8 块让出一次 UI 线程，');
    B.ln('     不一次性拼一个巨大字符串。');
    B.ln('');
    B.ln('执行环境：');
    B.ln('  · 可用内存：' + (facts.memText.length > 0 ? facts.memText : '（未取到）'));
    B.ln('  · 热状态：' + (facts.thermalText.length > 0 ? facts.thermalText : '（未取到）'));
    const caps: SmtCapabilities | null = BenchRunner.benchCapabilities();
    B.ln('  · CPU 拓扑：' + smtTopologyLine(caps));
    const panel: CpuAffinityPanelData = BenchRunner.cpuAffinityPanel();
    B.ln('  · 允许核集合 / 快簇：' + (panel.copyText.length > 0 ? panel.copyText : '（未上报）'));
    B.ln('  · Native 版本：' + BenchRunner.nativeVersion());
    // ---- J. 参考对照 / 线性-可比性审计(libauroraref.so, 只读) ----
    //   这一节回答的是"分数比 == 性能比"这条最高优先级验收标准在本次条件下成不成立:
    //   条件化可比性(可用核集合 / 频率档 / 每核与同频归一化) + 逐项线性自检。
    //   几个只能由 ArkTS 侧提供的量(threadsUsed / workerCpus / allowedCores /
    //   runtimeKhzMedian / nominalTopKhz)在这里逐条写明传了什么、没传的为什么没传 ——
    //   以前这几个量根本没接上, 报告里只有 null, 看不出是"没有数据"还是"没人接"。
    B.head('J · 参考对照与线性/可比性审计（libauroraref.so，只读，不参与计分）');
    B.ln('跨芯片要看的是「分数比 → 芯片性能比」：单份报告只能给出本机的取得条件；');
    B.ln('  两台设备的换算要用本 App 的「报告对比」（各跑一份 report-latest.json 再对比）：');
    B.ln('  它的输出里有一块 chipPerformanceRatio，直接给出结论行：');
    B.ln('  「原始多核分数比 = 芯片性能比 x 核数比 x 频率档比」—— 把核数比与频率档比除回去，');
    B.ln('  剩下的 perCoreSameFreqRatioGM 才是芯片性能比（bestEstimateOfChipPerformanceRatio），');
    B.ln('  并写明用的是哪一个归一化量、缺输入时写 null（不拿分数比冒充）。');
    B.ln('  本节下面的 comparabilityAudit 给的就是这一份「本机的条件」（允许核集合 / 频率档 / 线程数）。');
    if (!ref.moduleAvailable) {
      B.ln(' 本块不可用：' + (ref.error.length > 0 ? ref.error : 'native 模块未就绪'));
      B.ln('（对照表是只读旁路: 拿不到它不影响上面的任何一项结果与计分。）');
    } else {
      B.ln('模块：libauroraref.so · 真值表版本 ' +
        (ref.moduleVersion.length > 0 ? ref.moduleVersion : '（未返回）') + ' · 调用结果=' +
        (ref.ok ? '成功' : '失败') + ' · 这一块不跑负载、不改分数、不计分');
      B.ln('');
      B.ln('① 我们传进去的量（线性/可比性审计要的那几个，全部来自本轮实测的旁路字段）：');
      B.ln('   options=' + (ref.optionsJson.length > 0 ? ref.optionsJson : '（空）'));
      B.ln('   逐项来源：' + (ref.note.length > 0 ? ref.note : '（无）'));
      B.ln('   口径：拿不到的键不传（native 侧写 null）；不传 0、不编数。');
      B.ln('');
      if (!ref.ok) {
        B.ln('② 对照表没有生成：' + (ref.error.length > 0 ? ref.error : '（native 未给原因）'));
      } else {
        const comp: string = extractObject(ref.json, 'comparabilityAudit');
        const lin: string = extractObject(ref.json, 'linearityAudit');
        B.ln('② 条件化可比性（comparabilityAudit，native 原样摘录）：');
        B.ln(comp.length > 0 ? comp : '（摘不出来：对照表结构与预期不同，全文见 JSON 报告里的 referenceAudit.table）');
        B.ln('');
        B.ln('③ 线性自检（linearityAudit，native 原样摘录）：');
        B.ln(lin.length > 0 ? lin : '（摘不出来：对照表结构与预期不同，全文见 JSON 报告里的 referenceAudit.table）');
        B.ln('');
        B.ln('④ 完整对照表（与公开真值并列的逐项表）：见 JSON 报告 referenceAudit.table（native 原样，未做加工）。');
      }
    }
    B.ln('');
    B.ln('（报告结束）');
    // ---- 落盘回执: 正文最后一段。它在写盘之后才填上真实结果(见 ReportSinkInfo),
    //      所以"盘上这份报告说自己有没有写成功"这句话一定是真的。
    B.ln('');
    B.ln(eq);
    B.ln(' 落盘回执（本报告文件自己说的落盘结果）');
    B.ln(eq);
    if (sink.status.length > 0 && sink.ok) {
      B.ln('结果：成功 —— 已写入（分块 writeSync + close；未调用 fsync，因为这里要保证的是' +
        '"进程被杀也不丢"，页缓存由内核持有，不需要 fsync）：');
      B.ln('  ' + sink.textPath);
      B.ln('  ' + sink.jsonPath);
      B.ln('最新副本：');
      B.ln('  ' + sink.latestTextPath);
      B.ln('  ' + sink.latestJsonPath);
      B.ln('复核方式：hdc file recv ' + (sink.textPath.length > 0 ? sink.textPath : '<filesDir>/report-*.txt'));
    } else if (sink.status.length > 0) {
      B.ln('结果：未写成功 —— ' + sink.status);
      B.ln('（界面上那份报告仍然完整；可用「复制全文 / 导出到文件」把它保存出去。）');
    } else {
      B.ln('结果：写入中（本段是写盘前的版本；最终回执由写入方在写完后回填本行）');
    }
    // 「历史成绩.txt」(同一目录里那份一行一次跑分的纯文本成绩单)的落盘结果。
    // 它是与报告同一时刻写的, 失败原因原样印在这里(不静默); 写不进「下载」目录时
    // 报告落盘之后还会再补试一次, 最终结果以界面提示为准。
    B.ln('');
    B.ln('历史成绩.txt（同目录下的纯文本成绩单）：' +
      (out.scoreHistoryStatus.length > 0 ? out.scoreHistoryStatus : '本轮没有写（没有可写的成绩）'));

    // ---- 报告 .txt 的 UTF-8 合法性守卫(2026-10 真机报告缺陷 4 修) ----
    // 为什么放在这里(全文拼完、返回之前): 这是唯一能同时看到"全文"与"文末回执"的位置,
    // 而且正文里的 native 原文此时都已在 parts 里 —— 扫一遍就知道有没有残缺码元。
    // 做什么: 每个分块里的孤立代理项换成 U+FFFD(合法代理对/emoji 一个都不动),
    //   并把"替换了几个、第一处在哪、为什么不是分块写入的锅"写进正文最后一段。
    // 依据: 真机 r80 报告偏移 546380 处的 12 个非法字节是 4 个完整的 CESU-8 序列(每个 3 字节),
    //   而"分块写入切断多字节字符"最多只会产生 1 个字符的残片(<= 3 字节), 不可能连续 12 个 ——
    //   所以根因是源字符串里本来就有孤立代理项(.json 里同一处就是 4 个 \uXXXX 转义),
    //   写盘只是把它们在 .txt 里按 CESU-8 编了出来。见 common/Utf8Guard.ets 的文件头。
    let loneTotal: number = 0;
    let firstLoneAt: number = -1;
    let scannedChars: number = 0;
    for (let ci = 0; ci < B.parts.length; ci++) {
      const piece: string = B.parts[ci];
      const chk: Utf8Check = checkReportText(piece);
      if (chk.ok) {
        scannedChars += piece.length;
        continue;
      }
      if (firstLoneAt < 0) {
        // 位置按替换前的码元下标算(replace 会把 2 个码元的代理对换成 1 个 U+FFFD,
        // 用替换后的长度去累加会让后面几块的下标漂移 —— 报出来的位置就不准了)。
        firstLoneAt = scannedChars + chk.firstIndex;
      }
      loneTotal += chk.loneSurrogates;
      scannedChars += piece.length;
      B.parts[ci] = sanitizeReportText(piece);
    }
    B.ln('');
    B.ln(eq);
    B.ln(' 文本编码自检（硬断言：报告 .txt 必须是合法 UTF-8）');
    B.ln(eq);
    if (loneTotal === 0) {
      B.ln('结果：通过 —— 全文 ' + scannedChars.toString() + ' 个 UTF-16 码元里没有孤立代理项（合法代理对不受影响），');
      B.ln('      交给 fs.writeSync 写出去就是合法 UTF-8（标准编码器只会在孤立代理项上写出非法字节）。');
    } else {
      B.ln('结果：修掉了 ' + loneTotal.toString() + ' 个孤立代理项（第一处在全文第 ' +
        firstLoneAt.toString() + ' 个 UTF-16 码元）—— 已按 U+FFFD（UTF-8 = EF BF BD）替换，');
      B.ln('      所以本文件（以及它的 report-latest.txt 副本）一定是合法 UTF-8。');
      B.ln('      它们来自 native 侧某个字符串（本轮真机上是 GPU-SNL 的 environment.stability 文本），');
      B.ln('      不是"分块写入把多字节字符切在块边界上"：切断只会产生 1 个字符的残片（<= 3 个非法字节），');
      B.ln('      而这里是整段残缺码元（真机现场 = 12 个非法字节 = 4 个完整的 3 字节 CESU-8 序列）。');
      B.ln('      同一份内容在 JSON 报告里是 \\uXXXX 转义（JSON.stringify 对孤立代理项就是这么写的），');
      B.ln('      所以 .json 一直是合法 UTF-8 —— 两份报告说的是同一件事，只是编码路径不同。');
      B.ln('      原文未做任何"美化"：替换只发生在孤立代理项上，其余字节一个都没动，替换个数与位置就在这里。');
    }

    return B.parts;
  }

  private static doneSections(out: FullRunOutcome): number {
    let n = 0;
    for (const s of out.sections) {
      if (s.status === ST_DONE || s.status === ST_FAILED) {
        n++;
      }
    }
    return n;
  }

  // 可见性由 private 改 public: 供结果页「三层呈现」复用同一份几何平均口径(算法一字未改)。
  //
  // 唯一的口径入口(2026-10-08): 报告正文、JSON 报告、结果页卡片、小节 note、参考对照的
  // 输入, 全部走这一个函数。以前 ArkTS 有 3 处各算各的几何平均(这里 / FullRun.geo /
  // Index.ets 的卡片), 加上 native 又自己算一份, 一共 4 份 —— 只要有任何一处条件不同,
  // 就会出现"两个数对不上"。现在:
  //   ① 参与条件与 native compositeJson 的逐字一致: scored 且(多核时)inMulti 且分数 > 0;
  //      ArkTS 侧的等价判据 = "分数 > 0"(未计分项分数恒 0, 自然被排除; 多核列表里本来就
  //      只有官方那 8 项, 所以 inMulti 条件由"传进来的是哪一份列表"承担);
  //   ② 2026-10-05 起不再额外排除"未通过随芯片变化自检(FAIL)"的项
  //      旧写法多一个 isComparable(r) 条件, 结果是"报告卡片口径"与"native 用传下去的数组
  //      算出来的口径"不是同一批项(8.1 真机: 卡片 15 项 / native 10 项 -> 报告自己打出
  //      核对不一致 416.2 vs 409.4)。现在两边都是"该阶段所有有正分的计分项",
  //      于是 geoOf(list) 与 native 的 count/items[] 必然逐项相等。
  //      自检 FAIL 的项只被标注"不可用于跨芯片比较", 不再被踢出复合分。
  static geoOf(list: Gb7Result[]): number {
    let prod = 1;
    let n = 0;
    for (const r of list) {
      if (r.score > 0) {
        prod *= r.score;
        n++;
      }
    }
    return n > 0 ? Math.pow(prod, 1 / n) : 0;
  }

  // 可见性由 private 改 public: 供结果页标注「计分项 n/N」(算法一字未改)
  static scoredOf(list: Gb7Result[]): number {
    let n = 0;
    for (const r of list) {
      if (r.score > 0) {
        n++;
      }
    }
    return n;
  }

  // 永久断言的口径(2026-10-05): 复合分的参与项数 —— 就是 geoOf 实际拿去算几何平均的
  // 那一批项(有正分数), 与 native compositeJson 的 count/items[] 由构造逐项相等。
  // 报告里的小节结论、正文 D 节、JSON composite 全部用这一个数, 不许再各处各写一个。
  static participantOf(list: Gb7Result[]): number {
    return FullReportBuilder.scoredOf(list);
  }

  // 「可用于跨芯片比较」的项数 = 复合分参与项(拿到分)里, 自检不是 FAIL 的那些。
  // 2026-10-05: 它是参与项集合的子集(participantOf 的子集), 不再是复合分的过滤器 ——
  // 所以报告里必须写成"参与 N 项 · 其中可用于跨芯片比较 M 项", 两个数一起印, 不许只印一个。
  static comparableOf(list: Gb7Result[]): number {
    let n = 0;
    for (const r of list) {
      if (r.score > 0 && isComparable(r)) {
        n++;
      }
    }
    return n;
  }

  // 「未通过随芯片变化自检」的项数。这些项仍进复合分(口径不变), 只是不能拿去跨芯片比。
  static notComparableOf(list: Gb7Result[]): number {
    let n = 0;
    for (const r of list) {
      if (r.score > 0 && !isComparable(r)) {
        n++;
      }
    }
    return n;
  }

  // ---- NPU 全文 ----
  // ---- GPU-SNL(Aurora Nomad Light): 一行摘要(JSON 逐项明细的 diag 用) ----
  // ---- GPU-SNL: 一句必有原因的失败结论(2026-10 缺陷 3 修) ----
  // 硬要求(用户原话: "失败必须有非空原因"): 只要本节不是成功, 返回的字符串里必须带上
  // 真实原因。原因按"最靠近根因"的顺序取:
  //   ① prepare() 的驱动原文(初始化失败, run() 根本没跑);
  //   ② 原始 JSON 解析失败(原因来自解析器: JSON.parse 的异常 + 原文前 120 字符);
  //      此时若原文自称 "ok":true, 必须把"原文这么说, 但它不是合法 JSON, 所以不算数"写明;
  //   ③ native 明说 ok=false 时它给的原因; 它没给原因就写"它没给原因"(不留空)。
  static snFailReason(sn: SnRunResult): string {
    if (sn.prepError.length > 0) {
      return '初始化失败（驱动原文）：' + sn.prepError;
    }
    if (!sn.rawParseOk) {
      const why: string = sn.error.length > 0 ? sn.error
        : 'native 返回的 JSON 无法解析（解析器没有给出更详细的原因）';
      if (sn.rawSelfClaimOk) {
        return 'native 返回的 JSON 无法解析：' + why +
          ' ｜ 这与原始 JSON 的自称相矛盾, 已核实并记录: 原始 JSON 文本开头写的是 ' +
          '"ok":true, 但这份 JSON 本身不是合法 JSON（解析不出任何字段）, 所以不能据此认定' +
          '本节成功 —— 本节的结论一律以"解析失败 = 失败"为准, 原始 JSON 已原样附在本报告里' +
          '（文本报告见"原始 JSON="一行, JSON 报告见 referenceAudit.inputsJson.gpuSnl 与 sn 块）,' +
          '谁都可以拿它复核。';
      }
      return 'native 返回的 JSON 无法解析：' + why;
    }
    if (!sn.ok) {
      return 'native 返回 ok=false：' + (sn.error.length > 0 ? sn.error
        : (sn.lastError.length > 0 ? sn.lastError
          : '但 native 没有给出任何原因文本（error 与 lastError 都是空）—— 记录, 不编造原因'));
    }
    return '';
  }

  static gpuSnlOneLine(sn: SnRunResult): string {
    if (sn.prepError.length > 0) {
      return '初始化失败（驱动原文）：' + sn.prepError;
    }
    if (!sn.ok) {
      return '失败：' + FullReportBuilder.snFailReason(sn);
    }
    return 'score=' + snNum(sn.score, 1) + ' 分 · fps=' + snNum(sn.fps, 2) + ' FPS · 吞吐=' +
      snNum(sn.throughputMpx, 2) + ' Mpx/s · ' + snEnum(sn.verdict) + ' · ' +
      snCalibrationLine(sn.calibrationStatus, sn.calibrationK);
  }

  // ---- GPU-SNL: C7 段里的单节块 ----
  static gpuSnlBlock(sn: SnRunResult): string {
    const B: TextBuf = new TextBuf();
    B.ln('[01/01] ' + sn.section + ' · ' + sn.workload +
      '（native scored=' + (sn.scored ? 'true' : 'false') + ' · gb7Item=' +
      (sn.gb7Item ? 'true' : 'false') + '）');
    if (sn.prepError.length > 0) {
      B.ln('    ok=否 · 初始化失败, 驱动原文如下（原样, 未做任何加工）：');
      B.ln('    ' + sn.prepError);
      B.ln('');
      return B.parts.join('');
    }
    B.ln('    ok=' + (sn.ok ? '是' : '否') + ' · 分数=' + snNum(sn.score, 1) + ' ' + sn.scoreUnit +
      ' · FPS=' + snNum(sn.fps, 2) + ' · 吞吐=' + snNum(sn.throughputMpx, 2) + ' ' +
      sn.throughputUnit);
    B.ln('    口径: ' + (sn.scoreFormula.length > 0 ? sn.scoreFormula : '—') + ' · k=' +
      sn.kScorePerFps.toFixed(4));
    B.ln('    自证: ' + snEnum(sn.verdict) + '（conclusive=' + (sn.gpuBoundConclusive ? 'true' : 'false') +
      '）· drawCallsPerFrame=' + snNum(sn.drawCallsPerFrame, 0) +
      ' · cpuVertexDataBytesPerFrame=' + snNum(sn.cpuVertexDataBytesPerFrame, 0) +
      ' · submissionDuty=' + snNum(sn.submissionDuty, 4));
    B.ln('    ' + snCalibrationLine(sn.calibrationStatus, sn.calibrationK));
    B.ln('    可重复性: ' + snEnum(sn.credibility) + ' · 离散度=' + snNum(sn.dispersion, 2) +
      '% · 轮数=' + sn.repeats.toString() + ' · 环境=' + snEnum(sn.envState));
    B.ln('    可比较性: ' + (sn.refAnchor.length > 0 ? ('公开真值锚点 ' + sn.refAnchor) : '（无锚点）') +
      ' · 本机 benchVersion=' + sn.benchVersion.toString());
    B.ln('    单次结果: 绝对值未经标定, 不可当 3DMark 分数用, 只可用于同设备两次运行的比值。');
    B.ln('');
    return B.parts.join('');
  }

  // ---- GPU-SNL(Aurora Nomad Light)小节全文 ----
  private static gpuSnlText(out: FullRunOutcome): string {
    const B: TextBuf = new TextBuf();
    const sn: SnRunResult | null = out.gpuSnl;
    B.ln('计分标记：scored = false / gb7Item = false（native 侧固定）。本小节单独成节，');
    B.ln('          不并入 CS1 分数，也不并入自研套件复合分，也不改变任何 CS1 项的口径。');
    B.ln('本小节来源：独立 napi 模块 libaurorasn.so（napi 名 aurorasn），与 CS1 的 CPU 16 项 /');
    B.ln('            GPU 11 项完全独立；它在 native 侧自带 section 字段，界面按 section 单独成节。');
    B.ln('');
    if (sn === null) {
      B.ln('没有拿到结果：' + (out.gpuSnlError.length > 0 ? out.gpuSnlError : '（未执行）'));
      B.ln('');
      return B.parts.join('');
    }
    if (sn.prepError.length > 0) {
      B.ln('prepare() 返回了非空错误串 —— 驱动原文（原样, 一个字都没有改）：');
      B.ln('  ' + sn.prepError);
      B.ln('');
      B.ln('处理：本节按"失败"记录, 但不中断整轮（后面各节照常执行）；run() 没有被调用。');
      B.ln('');
      return B.parts.join('');
    }
    B.ln('[一] 结果与口径');
    B.ln('  ok=' + (sn.ok ? 'true' : 'false') + ' · section=' + sn.section + ' · workload=' +
      sn.workload + ' · scored=' + (sn.scored ? 'true' : 'false') + ' · gb7Item=' +
      (sn.gb7Item ? 'true' : 'false'));
    B.ln('  error=' + (sn.error.length > 0 ? sn.error : '（空）') + ' · lastError=' +
      (sn.lastError.length > 0 ? sn.lastError : '（空）'));
    // ---- "这个结论是从哪来的"两个事实位(2026-10 真机报告缺陷 3 修) ----
    //   解析成功没有 + 原始 JSON 自己说了什么。两者不一致(原文自称 ok:true 但 JSON 解析不了)
    //   时, 这里必须把矛盾写清楚 —— 否则读者会把下面的 0 当成真读数, 或者以为报告自己错了。
    B.ln('  原始 JSON 解析=' + (sn.rawParseOk
      ? '成功（下面各项就是它里面的取值）'
      : '失败（下面各项一律是未取到的默认值, 不是"读到了 0"；原因见上一行 error）'));
    B.ln('  原始 JSON 自称=' + (sn.rawSelfClaimOk ? '"ok":true' : '（不是以 "ok":true 开头）') +
      (sn.rawParseOk ? ''
        : ' ← 这只是"原文说了什么"的记录, 不是本节结论: JSON 解析不了时, 它里面写的 ' +
          'ok:true 不能证明本节成功；两者不一致时以"解析失败 = 失败"为准'));
    B.ln('  benchVersion=' + sn.benchVersion.toString() +
      (sn.benchVersionText.length > 0 ? (' · ' + sn.benchVersionText) : ''));
    B.ln('  分数 = ' + snNum(sn.score, 2) + ' ' + sn.scoreUnit + ' · 公式=' +
      (sn.scoreFormula.length > 0 ? sn.scoreFormula : '—') + ' · kScorePerFps=' +
      sn.kScorePerFps.toFixed(4));
    B.ln('  帧率 = ' + snNum(sn.fps, 3) + ' FPS · 平均帧 ' + snNum(sn.meanFrameMs, 3) + ' ms · 中位 ' +
      snNum(sn.medianFrameMs, 3) + ' ms · 最好 ' + snNum(sn.bestFrameMs, 3) + ' ms · 最差 ' +
      snNum(sn.worstFrameMs, 3) + ' ms · 稳定性 ' + snNum(sn.frameStability, 4));
    B.ln('  计时帧数 = ' + sn.measuredFrames.toString() + '（预热 ' + sn.warmupFrames.toString() + '）');
    B.ln('  吞吐 = ' + snNum(sn.throughputMpx, 3) + ' ' + sn.throughputUnit + ' · 每帧像素=' +
      snNum(sn.pixelsPerFrame, 0));
    B.ln('[二] 工作量与自适应趟数（scale.*）');
    B.ln('  每趟像素=' + sn.pixelsPerPass.toString() + ' · 每帧趟数=' + sn.passesPerFrame.toString() +
      ' · 每像素操作=' + sn.opsPerPixelPerPass.toString() + ' · 每帧操作=' + snNum(sn.opsPerFrame, 0));
    B.ln('  趟数依据=' + (sn.scaleSource.length > 0 ? sn.scaleSource : '—') + ' · 自适应=' +
      (sn.scaleAdaptive ? '是' : '否') + ' · 阶梯=' +
      (sn.scaleLadder.length > 0 ? sn.scaleLadder : '—'));
    B.ln('  目标每帧毫秒=' + snNum(sn.scaleTargetMs, 3) + ' · 趟数上限=' +
      sn.scaleMaxPasses.toString() + ' · 撞上限=' + (sn.scaleLimitReached ? '是' : '否'));
    B.ln('[三] 自证：这一轮确实是 GPU 在跑（gpuBoundEvidence）');
    B.ln('  verdict=' + snEnum(sn.verdict) + ' · conclusive=' +
      (sn.gpuBoundConclusive ? 'true' : 'false'));
    B.ln('  drawCallsPerFrame=' + snNum(sn.drawCallsPerFrame, 0) +
      ' · cpuVertexDataBytesPerFrame=' + snNum(sn.cpuVertexDataBytesPerFrame, 0) +
      '（本负载由 GPU 侧生成几何, 期望 0）');
    B.ln('  submissionDuty=' + snNum(sn.submissionDuty, 4) + ' · GPU 忙碌占每帧=' +
      snNum(sn.gpuBusyPercentOfFrame, 2) + '% · 每帧同步点=' + snNum(sn.syncPointsPerFrame, 0));
    B.ln('[四] 标定状态（绝对值能不能当 3DMark 分数用）');
    B.ln('  ' + snCalibrationLine(sn.calibrationStatus, sn.calibrationK));
    B.ln('  推导=' + (sn.calibrationDerivation.length > 0 ? sn.calibrationDerivation : '—'));
    B.ln('  影响范围：只影响绝对值 = ' + (sn.affectsAbsoluteScoreOnly ? '是' : '否') +
      ' · 不标定也可用比值 = ' + (sn.ratioValidWithoutCalibration ? '是' : '否'));
    B.ln('[五] 标尺：本节的 CPU / SIMD 条件（ruler.*）');
    B.ln('  CPU: 逻辑核=' + sn.cpuLogical.toString() + ' · 物理核=' + sn.cpuPhysical.toString() +
      ' · SMT 可能存在=' + (sn.smtPossible ? '是' : '否') + ' · SMT 已开=' +
      (sn.smtEnabled ? '是' : '否') + ' · 线程上限=' + sn.threadCap.toString());
    B.ln('  SIMD: arch=' + sn.simdArch.toString() + ' · neon=' + (sn.simdNeon ? '是' : '否') +
      ' · sve=' + (sn.simdSve ? '是' : '否') + ' · sve2=' + (sn.simdSve2 ? '是' : '否') +
      ' · i8mm=' + (sn.simdI8mm ? '是' : '否'));
    B.ln('  该小节自己用的线程数 = 1（compare 时按同条件比值）');
    B.ln('[六] 可重复性与环境');
    B.ln('  可信度=' + snEnum(sn.credibility) + ' · 离散度=' + snNum(sn.dispersion, 2) +
      '% · 轮数=' + sn.repeats.toString());
    B.ln('  FPS 中位/最小/最大 = ' + snNum(sn.fpsMedian, 3) + ' / ' + snNum(sn.fpsMin, 3) + ' / ' +
      snNum(sn.fpsMax, 3));
    B.ln('  分数 中位/最小/最大 = ' + snNum(sn.scoreMedian, 2) + ' / ' + snNum(sn.scoreMin, 2) +
      ' / ' + snNum(sn.scoreMax, 2));
    B.ln('  环境状态=' + snEnum(sn.envState) + ' · 温度中位/最大=' + snNum(sn.tempMedian, 1) + '/' +
      snNum(sn.tempMax, 1) + ' degC · CPU 频率中位/最大=' + mhzText(sn.cpuKhzMedian) + ' / ' +
      mhzText(sn.cpuKhzMax));
    for (const rd of sn.rounds) {
      B.ln('    第 ' + rd.round.toString() + ' 轮: ok=' + (rd.ok ? '是' : '否') + ' · 趟数=' +
        rd.passesPerFrame.toString() + ' · FPS=' + snNum(rd.fps, 3) + ' · 每趟=' +
        snNum(rd.perPassMs, 4) + ' ms · 吞吐=' + snNum(rd.throughputMpxPerSec, 2) + ' Mpx/s · 分数=' +
        snNum(rd.score, 2) + ' · 温度中位=' + snNum(rd.tempMedian, 1) + ' · 频率中位=' +
        mhzText(rd.cpuKhzMedian) + ' · 环境=' + snEnum(rd.envState));
    }
    B.ln('[七] 图形栈与真实用途');
    B.ln('  GL 版本=' + (sn.glVersion.length > 0 ? sn.glVersion : '—') + ' · 渲染器=' +
      (sn.glRenderer.length > 0 ? sn.glRenderer : '—'));
    B.ln('  这一项代表：' + (sn.realUseShort.length > 0 ? sn.realUseShort : '—'));
    B.ln('  综合是否可信（validity.pass）=' + (sn.validityPass ? '是' : '否'));
    B.ln('[八] 公开真值并列（不做任何官方换算）');
    B.ln('  对照设备：' + (sn.refDevice.length > 0 ? sn.refDevice : '—') +
      (sn.refAnchor.length > 0 ? (' · 公开锚点 ' + sn.refAnchor) : ''));
    if (sn.refRows.length === 0) {
      B.ln('    （native 未给对照行）');
    }
    for (const rf of sn.refRows) {
      B.ln('    ' + rf.name + '：公开分 ' + snNum(rf.referenceScore, 1) + ' · 本机 ' +
        snNum(rf.ourScore, 1) + ' · 差 ' + snNum(rf.deltaPercent, 1) + '%' +
        (rf.source.length > 0 ? (' · 来源 ' + rf.source) : ''));
    }
    B.ln('  注意：本机未标定, 上表只做并列展示, 不是官方换算结果。');
    B.ln('[九] native 的 warnings / risks / notes（原样）');
    if (sn.warnings.length === 0) {
      B.ln('  warnings=（无）');
    }
    for (const w of sn.warnings) {
      B.ln('  warning: ' + w);
    }
    B.ln('  risks=' + (sn.risks.length > 0 ? sn.risks : '（空）'));
    B.ln('  notes=' + (sn.notes.length > 0 ? sn.notes : '（空）'));
    B.ln('');
    B.ln('为什么不计分：本小节与 CS1 的任何一项都不同源, 不是同一把尺子,');
    B.ln('            把它并进 CS1 复合分会立刻毁掉 CS1 分数自身的可比性 ——');
    B.ln('            native 侧因此固定 scored=false / gb7Item=false, 这里也不重标定。');
    B.ln('');
    B.ln('原始 JSON=' + (sn.raw.length > 0 ? sn.raw : '（空）'));
    return B.parts.join('');
  }

  private static npuText(out: FullRunOutcome): string {
    const B: TextBuf = new TextBuf();
    const pr: NpuProbeResult | null = out.npuProbe;
    const rn: NpuRunResult | null = out.npuRun;
    B.ln('计分标记：scored = false（native 侧固定）。本小节结果单独展示，');
    B.ln('          不并入 CS1 分数，也不并入自研套件复合分。');
    B.ln('');
    B.ln('[第一步] 设备探测 probeNpu()');
    if (pr === null) {
      B.ln('  没有拿到探测结果：' + (out.npuError.length > 0 ? out.npuError : '（未执行）'));
    } else {
      B.ln('  ok=' + (pr.ok ? 'true' : 'false') + ' · status=' + pr.status);
      B.ln('  text=' + (pr.text.length > 0 ? pr.text : '（空）'));
      B.ln('  error=' + (pr.error.length > 0 ? pr.error : '（空）'));
      B.ln('  这台机器是否向第三方应用开放 NPU (thirdPartyNpuOpen)=' +
        (pr.thirdPartyNpuOpen ? '是' : '否'));
      B.ln('  设备数=' + pr.deviceCount.toString() + ' · 有 CPU=' + (pr.hasCpu ? '是' : '否') +
        ' · 有 GPU=' + (pr.hasGpu ? '是' : '否') + ' · 有 ACCELERATOR=' +
        (pr.hasAccelerator ? '是' : '否'));
      B.ln('  NNRt 接口 available=' + (pr.nnrtAvailable ? '是' : '否') + ' · 绑定符号 ' +
        pr.nnrtBound.toString() + '/' + pr.nnrtExpected.toString());
      if (pr.nnrtBinding.length > 0) {
        B.ln('  NNRt binding=' + pr.nnrtBinding);
      }
      if (pr.missingSymbols.length > 0) {
        B.ln('  NNRt 缺失符号=' + pr.missingSymbols);
      }
      if (pr.runtimeDlerror.length > 0) {
        B.ln('  NNRt dlopen 错误=' + pr.runtimeDlerror);
      }
      if (pr.notes.length > 0) {
        B.ln('  notes=' + pr.notes);
      }
      B.ln('  设备列表：');
      if (pr.devices.length === 0) {
        B.ln('    （无）');
      }
      for (const d of pr.devices) {
        B.ln('    id=' + d.id.toString() + ' · ' + d.typeName + '（' + d.typeEnum + '）· ' +
          d.name);
      }
      B.ln('  探测原始 JSON=' + (pr.raw.length > 0 ? pr.raw : '（空）'));
    }
    B.ln('');
    B.ln('[第二步] 推理跑分 runNpu(scale=' + NPU_SCALE.toString() + ', warmup=' +
      NPU_WARMUP.toString() + ', iters=' + NPU_ITERS.toString() + ', budgetMs=' +
      NPU_BUDGET_MS.toString() + ')');
    if (rn === null) {
      B.ln('  没有拿到跑分结果：' + (out.npuError.length > 0 ? out.npuError : '（未执行）'));
    } else {
      B.ln('  ok=' + (rn.ok ? 'true' : 'false') + ' · status=' + rn.status + ' · scored=false');
      B.ln('  text=' + (rn.text.length > 0 ? rn.text : '（空）'));
      B.ln('  error=' + (rn.error.length > 0 ? rn.error : '（空）'));
      B.ln('  实际设备：typeName=' + (rn.deviceTypeName.length > 0 ? rn.deviceTypeName : '—') +
        ' · deviceLabel=' + (rn.deviceLabel.length > 0 ? rn.deviceLabel : '—') + ' · id=' +
        rn.deviceId.toString() + ' · name=' + (rn.deviceName.length > 0 ? rn.deviceName : '—') +
        ' · 尝试次数=' + rn.attemptCount.toString());
      if (rn.fallbackText.length > 0) {
        B.ln('  设备回退说明=' + rn.fallbackText);
      }
      B.ln('  模型：scaleLabel=' + (rn.scaleLabel.length > 0 ? rn.scaleLabel : '—') +
        ' · 算子组=' + (rn.opSetName.length > 0 ? rn.opSetName : '—') + ' · 主算子=' +
        (rn.mainOp.length > 0 ? rn.mainOp : '—') + ' · 算子组回退=' +
        (rn.opSetFallbackUsed ? '是' : '否'));
      if (rn.opSetText.length > 0) {
        B.ln('  算子组说明=' + rn.opSetText);
      }
      B.ln('  工作量：每次推理 MACs=' + rn.macsPerInference.toString() + ' · FLOPs=' +
        rn.flopsPerInference.toString() + ' · 口径=' +
        (rn.macFormula.length > 0 ? rn.macFormula : '—'));
      B.ln('  精度=' + (rn.precisionShort.length > 0 ? rn.precisionShort : '—'));
      B.ln('  计时：可用=' + (rn.timingAvailable ? '是' : '否') + ' · 平均=' +
        numOrDash(rn.meanMs, 3) + ' ms · 中位=' + numOrDash(rn.medianMs, 3) + ' ms · 最小=' +
        numOrDash(rn.minMs, 3) + ' ms · 最大=' + numOrDash(rn.maxMs, 3) + ' ms');
      B.ln('  吞吐：' + numOrDash(rn.goPs, 4) + ' GOPs/s · ' + numOrDash(rn.macsPerSec, 1) +
        ' MACs/s');
      B.ln('  次数：预热=' + rn.warmupRuns.toString() + ' · 计时=' +
        rn.measuredRuns.toString() + ' · 撞到预算=' + (rn.budgetHit ? '是' : '否'));
      if (rn.hint.length > 0) {
        B.ln('  hint=' + rn.hint);
      }
      if (rn.notes.length > 0) {
        B.ln('  notes=' + rn.notes);
      }
      B.ln('  跑分原始 JSON=' + (rn.raw.length > 0 ? rn.raw : '（空）'));
    }
    B.ln('');
    B.ln('为什么不计分：CS1 的 CPU/GPU 项里没有任何 NPU 对应项，把这一节混进 CS1 复合分');
    B.ln('会立刻毁掉 CS1 分数自身的可比性 —— native 侧因此固定 scored=false / gb7Item=false。');
    return B.parts.join('');
  }

  // ---- 存储全文 ----
  private static storageText(out: FullRunOutcome): string {
    const B: TextBuf = new TextBuf();
    B.ln('计分标记：scored = false（native 侧固定）。本小节结果单独展示，');
    B.ln('          不并入 CS1 分数，也不并入自研套件复合分。');
    B.ln('');
    B.ln('沙箱目录准备 storagePrepare(filesDir)：' +
      (out.storagePrepared ? '成功' : '失败（native 拒绝或接口不可用）'));
    B.ln('只读探测 storageInfo()=' + (out.storageInfo.length > 0 ? out.storageInfo : '（空）'));
    const st: StorageRunResult | null = out.storage;
    if (st === null) {
      B.ln('');
      B.ln('跑分结果：没有拿到（' +
        (out.storageError.length > 0 ? out.storageError : '（未执行）') + '）');
    } else {
      B.ln('');
      B.ln('ok=' + (st.ok ? 'true' : 'false') + ' · 小节=' +
        (st.sectionTitle.length > 0 ? st.sectionTitle : '存储 I/O') + ' · 目录=' +
        (st.dir.length > 0 ? st.dir : '—'));
      B.ln('skipped=' + (st.skipped ? 'true' : 'false') + ' · skipReason=' +
        (st.skipReason.length > 0 ? st.skipReason : '（无）'));
      B.ln('fatal=' + (st.fatal.length > 0 ? st.fatal : '（无）'));
      B.ln('总耗时=' + durText(st.totalMs) + ' · 预算=' + st.budgetMs.toString() + ' ms · 提前收工=' +
        (st.stoppedEarly ? ('是（' + st.stopReason + '）') : '否'));
      B.ln('');
      B.ln('头条数字（native headline）：');
      B.ln('  · 顺序写（write+fsync，更接近真实落盘）：' +
        numOrDash(st.seqWriteDurableMbps, 2) + ' MiB/s');
      B.ln('  · 顺序读（绕过页缓存）：' + numOrDash(st.seqReadColdMbps, 2) + ' MiB/s');
      B.ln('  · 4K 随机读：' + numOrDash(st.randRead4kIops, 1) + ' IOPS');
      B.ln('  · 4K 随机写：' + numOrDash(st.randWrite4kIops, 1) + ' IOPS');
      B.ln('  · 小文件创建：' + numOrDash(st.filesPerSecCreate, 1) + ' 个/秒 · 删除：' +
        numOrDash(st.filesPerSecDelete, 1) + ' 个/秒');
      B.ln('  （单位是 MiB/s：1 MiB = 1048576 B；换算成 MB/s 请 ×1.048576）');
      B.ln('');
      B.ln('环境（native env）：总量=' + numOrDash(st.totalBytes, 0) + ' B · 空闲=' +
        numOrDash(st.freeBytes, 0) + ' B · 可用=' + numOrDash(st.availBytes, 0) +
        ' B · 块大小=' + numOrDash(st.blockSize, 0) + ' B');
      B.ln('  绕过页缓存：O_DIRECT 可用=' + (st.oDirectWorks ? '是' : '否') +
        ' · fadvise(DONTNEED) 可用=' + (st.fadviseDontNeedWorks ? '是' : '否') +
        ' · 实际采用=' + (st.bypassMethod.length > 0 ? st.bypassMethod : '—'));
      B.ln('');
      B.ln('逐项明细（' + st.items.length.toString() + ' 项）：');
      B.ln('');
      for (let i = 0; i < st.items.length; i++) {
        B.add(FullReportBuilder.storageItemBlock(i + 1, st.items.length, st.items[i]));
      }
      if (st.warnings.length > 0) {
        B.ln('警告（native warnings）：');
        for (const w of st.warnings) {
          B.ln('  · ' + w);
        }
      } else {
        B.ln('警告：无');
      }
      B.ln('');
      B.ln('原始 JSON=' + (st.raw.length > 0 ? st.raw : '（空）'));
    }
    B.ln('');
    B.ln('兜底清理 storageCleanupNow()：' + (out.storageCleanupOk ? '成功（无残留）'
      : '失败或仍有残留条目 —— 请检查 <filesDir>/storage_bench'));
    B.ln('为什么不计分：存储 I/O 与 CS1 的任何一项都不同源，混进复合分会破坏');
    B.ln('CS1 分数自身的可比性 —— native 侧因此固定 scored=false。');
    return B.parts.join('');
  }

  // ---- 崩溃/卡死取证 ----
  private static crashText(out: FullRunOutcome, facts: FullRunUiFacts): string {
    const B: TextBuf = new TextBuf();
    const crashPath: string = BenchRunner.nativeGuardPath();
    const hangPath: string = BenchRunner.nativeHangPath();
    const crashFile: EvidenceFileFact = evidenceFact('native 崩溃现场', crashPath);
    const hangFile: EvidenceFileFact = evidenceFact('native 卡死现场（看门狗栈采样）', hangPath);
    B.ln('取证装置（native crash_guard / 看门狗）：');
    B.ln('  · 信号处理器已武装=' + (BenchRunner.nativeGuardReady() ? '是' : '否') +
      ' · 状态=' + CrashGuard.get().guardStatusText());
    B.ln('  · 崩溃证据文件路径=' + (crashPath.length > 0 ? crashPath : '（未返回）'));
    B.ln('  · 卡死证据文件路径=' + (hangPath.length > 0 ? hangPath : '（未返回）'));
    B.ln('');
    B.ln('文件级事实（本轮报告生成时刻，只读探测，不消费内容）：');
    B.ln('  · ' + crashFile.label + '：存在=' + (crashFile.exists ? '是' : '否') +
      ' · 大小=' + crashFile.bytes.toString() + ' 字节 · ' + crashFile.err);
    B.ln('  · ' + hangFile.label + '：存在=' + (hangFile.exists ? '是' : '否') +
      ' · 大小=' + hangFile.bytes.toString() + ' 字节 · ' + hangFile.err);
    B.ln('');
    B.ln('有没有崩溃记录：');
    B.ln('  · 本次启动从取证装置读到的 native 现场=' + (facts.nativeCrashFound ? ('有（' +
      facts.nativeCrashChars.toString() + ' 字符）') : '无'));
    if (facts.nativeCrashSummary.length > 0) {
      B.ln('    摘要=' + facts.nativeCrashSummary);
    }
    B.ln('  · 首页崩溃横幅（ArkTS 面包屑 preferences）=' + (facts.crashBannerFound
      ? '命中：上次运行没跑完就异常结束' : '未命中'));
    if (facts.crashBannerLine.length > 0) {
      B.ln('    面包屑=' + facts.crashBannerLine);
    }
    B.ln('  · 本轮跑分期间进程是否存活到报告生成：' + (out.finishedAt > 0 ? '是（整轮正常收尾）'
      : '否'));
    B.ln('  · 本次一键全跑记录到的失败小节数：' + FullReportBuilder.failedSections(out).toString() +
      '（失败原因见【A】）');
    B.ln('');
    B.ln('怎么解读：');
    B.ln('  · 证据文件为空 + 面包屑命中 = 被系统 SIGKILL（内存压力 / 后台管制）——');
    B.ln('    SIGKILL 不可捕获，所以"盘上什么都没有"本身就是证据；');
    B.ln('  · 证据文件里有 SIGSEGV/SIGBUS/… + pc 落在 libaurorabench.so = 我们自己的代码崩了；');
    B.ln('  · 卡死证据文件非空 = 看门狗抓到过长任务栈（此时机器可能只是被限频/暂停）。');
    B.ln('  · 命令行取回：hdc file recv ' + (crashPath.length > 0 ? crashPath
      : '<filesDir>/native_crash.txt'));
    return B.parts.join('');
  }

  private static failedSections(out: FullRunOutcome): number {
    let n = 0;
    for (const s of out.sections) {
      if (s.status === ST_FAILED) {
        n++;
      }
    }
    return n;
  }

  // =======================================================================
  // JSON 报告(分块)
  // =======================================================================
  static jsonChunks(out: FullRunOutcome, facts: FullRunUiFacts, sink: ReportSinkInfo,
    ref: RefAuditResult, crep: CompositeRepeatFacts): string[] {
    // 「本机性能天花板」: native 唯一来源, 三处界面与报告正文用的都是它(见 BenchRunner.ceiling)
    const ceilRaw: string = BenchRunner.ceiling();
    const ceilTxt: string = ceilRaw.length > 0 ? ceilRaw
      : '（native 未上报：旧版 .so 没有 ceilingText 导出, 或读取失败）';
    const header: RepHeaderJson = {
      schema: 'aurora-fullreport/1',
      appVersion: facts.appVersion,
      nativeVersion: facts.nativeVersion,
      device: facts.device,
      startedAt: out.startedAt,
      startedAtText: stampFull(out.startedAt),
      finishedAt: out.finishedAt,
      elapsedMs: out.elapsedMs(),
      interrupted: out.interrupted,
      stopReason: out.stopReason,
      sectionCount: out.sections.length,
      itemCount: out.itemCount(),
      scoredNote: '本报告不改动任何 k / conv / 计分公式 / metric口径 / 单位 / 负载工作量与尺寸 / 线程与绑核策略。',
      // CS1 口径声明(与报告正文、「关于」页同一句;进 JSON 是为了让对比工具与历史记录也能读到)
      // 「本机性能天花板」: 与首页设备卡 / 结果页首屏 / 报告正文逐字同一句(native 唯一来源)
      ceilingNote: ceilTxt,
      comparabilityNote: '★CS1 是本工程自研的 CPU 测试套件: 每个负载都是独立实现, 刻度也由本工程自己定, 因此 CS1 的分数与任何第三方跑分软件的成绩都不是同一把尺子, 两边不能互相换算、也不能直接比较。CS1 的分数只在「两台设备都跑本 App 的 CS1」时才是可比的。套件显示名改过两次(GB7 -> GB8 -> CS1), 两次都只改名字: 负载 / 单位 / k / conv / 计分公式一个都没动, 所以 benchVersion 不递增, 旧分数与新 CS1 分数在数值上完全可比。',
      // 落盘事实: 路径在写盘前就知道(真实目标), written/status 写完后回填(见 ReportSinkInfo)
      reportSink: {
        textPath: sink.textPath, jsonPath: sink.jsonPath,
        latestTextPath: sink.latestTextPath, latestJsonPath: sink.latestJsonPath,
        written: sink.ok, status: sink.status
      }
    };

    const secs: RepSectionJson[] = [];
    for (const s of out.sections) {
      secs.push({
        id: s.id, title: s.title, status: s.status, note: s.note,
        elapsedMs: s.elapsedMs, itemDone: s.itemDone, itemTotal: s.itemTotal
      });
    }

    const items: RepItemJson[] = [];
    const own: BenchRecord | null = out.own;
    if (own !== null) {
      for (const t of own.tests) {
        items.push(FullReportBuilder.ownItemJson('自研套件 · CPU 单核', t));
      }
      for (const t of own.multiTests) {
        items.push(FullReportBuilder.ownItemJson('自研套件 · CPU 多核', t));
      }
      for (const g of own.gpuScenes) {
        const j: RepItemJson = FullReportBuilder.emptyItemJson('自研套件 · GPU 场景', g.scene);
        j.metric = g.metric;
        j.unit = g.unit;
        j.score = g.score;
        j.ms = g.frameMs;
        j.parallelism = 0;
        j.scoredByPolicy = true;
        j.scoredNote = g.score > 0 ? '计分项（自研套件 GPU 复合分）' : '未得分 -> 失败';
        j.ok = g.score > 0;
        items.push(j);
      }
    }
    const cores: number = BenchRunner.cores();
    const threads: number = BenchRunner.benchMultiThreads(cores > 0 ? cores : 4);
    for (const r of out.gb7Single) {
      items.push(FullReportBuilder.gb7ItemJson('CS1 单核', r, 1));
    }
    for (const r of out.gb7Multi) {
      items.push(FullReportBuilder.gb7ItemJson('CS1 多核', r, threads));
    }
    for (const r of out.gb7Gpu) {
      items.push(FullReportBuilder.gb7ItemJson('CS1 GPU', r, 1));
    }
    if (out.gpuSnl !== null) {
      const sn: SnRunResult = out.gpuSnl;
      const j: RepItemJson = FullReportBuilder.emptyItemJson('GPU-SNL 小节', 'Aurora Nomad Light');
      j.metric = sn.throughputMpx;
      j.unit = 'Mpx/s';
      j.score = sn.score;
      j.ms = sn.elapsedMs;
      j.parallelism = 1;
      j.scoredByPolicy = false;
      j.scoredNote = '不计分（native scored=false / gb7Item=false）';
      j.ok = sn.ok && sn.prepError.length === 0;
      j.diag = FullReportBuilder.gpuSnlOneLine(sn);
      items.push(j);
    }
    if (out.npuProbe !== null) {
      const j: RepItemJson = FullReportBuilder.emptyItemJson('NPU 小节', '设备探测');
      j.unit = out.npuProbe.status;
      j.scoredNote = '不计分（native scored=false）';
      j.ok = out.npuProbe.ok;
      items.push(j);
    }
    if (out.npuRun !== null) {
      const j: RepItemJson = FullReportBuilder.emptyItemJson('NPU 小节', '推理跑分');
      j.metric = out.npuRun.goPs;
      j.unit = 'GOPs/s';
      j.ms = out.npuRun.meanMs;
      j.parallelism = 1;
      j.scoredNote = '不计分（native scored=false）';
      j.ok = out.npuRun.ok;
      j.diag = out.npuRun.text;
      items.push(j);
    }
    if (out.storage !== null) {
      for (const it of out.storage.items) {
        const j: RepItemJson = FullReportBuilder.emptyItemJson('存储 I/O 小节', it.name);
        j.metric = it.iops > 0 ? it.iops : it.mbps;
        j.unit = it.iops > 0 ? 'IOPS' : 'MiB/s';
        j.ms = it.ms;
        j.parallelism = 1;
        j.scoredNote = '不计分（native scored=false）';
        j.ok = it.ok;
        j.diag = it.error.length > 0 ? it.error : it.note;
        items.push(j);
      }
    }

    // ===========================================================================
    //  显示名 = CS1; 内部键仍用 gb7 前缀
    // ---------------------------------------------------------------------------
    //  套件显示名改过两次: 8.4 把 GB7 改成 GB8, 本次开源改名再把 GB8 改成 CS1。
    //  代码里的内部标识符与 JSON 键名一律保留 gb7 前缀
    //  (gb7RunTest / gb7SingleComposite / gb7Items / 'gb7s' 等等)。
    //  为什么不改: 报告对比工具、历史记录与 runhistory 都按这些键名读,
    //  改名会直接断掉跨版本对比。显示名与内部键指的是同一套东西。
    //  另: 两次改名都只动显示名, 不改任何负载 / 单位 / k / conv / 计分公式, 所以
    //  benchVersion 不递增, 旧 GB7 / GB8 分数与新 CS1 分数在数值上完全可比。
    // ===========================================================================
    const composite: RepCompositeJson = {
      ownTotal: own !== null ? own.total : 0,
      ownSingle: own !== null ? own.single : 0,
      ownMulti: own !== null ? own.multi : 0,
      ownGpu: own !== null ? own.gpu : 0,
      ownIntSingle: own !== null ? own.intSingle : 0,
      ownFloatSingle: own !== null ? own.floatSingle : 0,
      ownIntMulti: own !== null ? own.intMulti : 0,
      ownFloatMulti: own !== null ? own.floatMulti : 0,
      gb7SingleComposite: FullReportBuilder.geoOf(out.gb7Single),
      gb7MultiComposite: FullReportBuilder.geoOf(out.gb7Multi),
      gb7GpuComposite: FullReportBuilder.geoOf(out.gb7Gpu),
      gb7MultiThreads: threads,
      note: '与各自卡片同口径（已计分项几何平均）；NPU / 存储不参与。',
      // 复合分的两个数都报(卡片的"由中位分算出的复合分" + native 的"逐轮复合分中位")
      gb7SingleCompositeMedianOfRounds: crep.singleMedian,
      gb7MultiCompositeMedianOfRounds: crep.multiMedian,
      gb7SingleCompositeSpreadPct: crep.singleSpread,
      gb7MultiCompositeSpreadPct: crep.multiSpread,
      gb7SingleCompositeRounds: crep.singleRounds,
      gb7MultiCompositeRounds: crep.multiRounds,
      gb7SingleCompositeVerdict: crep.singleVerdict,
      gb7MultiCompositeVerdict: crep.multiVerdict,
      gb7SingleCompositeVerdictText: crep.singleText,
      gb7MultiCompositeVerdictText: crep.multiText,
      repeatabilityNote: crep.note,
      // 口径证据: 让"这个复合分由哪些项、用哪些值、按什么规则排除"在 JSON 里也一眼可见。
      gb7SingleScoreSource: crep.singleSource,
      gb7MultiScoreSource: crep.multiSource,
      gb7SingleItems: crep.singleItems,
      gb7MultiItems: crep.multiItems,
      gb7SingleExcluded: crep.singleExcluded,
      gb7MultiExcluded: crep.multiExcluded,
      gb7SingleCount: crep.singleCount,
      gb7MultiCount: crep.multiCount,
      gb7SingleAuditExcluded: crep.singleAuditExcluded,
      gb7MultiAuditExcluded: crep.multiAuditExcluded,
      gb7SingleComparableCount: FullReportBuilder.comparableOf(out.gb7Single),
      gb7MultiComparableCount: FullReportBuilder.comparableOf(out.gb7Multi),
      gb7SingleParticipants: FullReportBuilder.participantOf(out.gb7Single),
      gb7MultiParticipants: FullReportBuilder.participantOf(out.gb7Multi),
      gb7SingleScopeOk: crep.singleScopeOk,
      gb7MultiScopeOk: crep.multiScopeOk,
      gb7SingleScopeText: crep.singleScopeText,
      gb7MultiScopeText: crep.multiScopeText
    };

    // 逐项「本项是否随芯片性能变化」自检(旁路结论): 报告正文 D2 与这里同一份数据。
    const scSum: ScalingSummary = summarize(out.gb7Single, out.gb7Multi);
    const scVerdicts: string[] = [];
    const scSingleRows: string[] = [];
    const scMultiRows: string[] = [];
    for (const row of scSum.singleRows) {
      scVerdicts.push(row.verdict);
      scSingleRows.push(row.name + ' [' + row.verdict + '] ' + row.evidence);
    }
    for (const row of scSum.multiRows) {
      scVerdicts.push(row.verdict);
      scMultiRows.push(row.name + ' [' + row.verdict + '] ' + row.evidence);
    }
    // 排除清单逐项印全(项名 + 原因 + 判定依据原文) —— 报告里不再只有一个"数字/名单"。
    //   来源与 excludedNames 完全同源: 判 FAIL 的那些行(多核口径那一行是"改变可用算力"的对照)。
    const scExcludedDetail: RepScalingExcluded[] = [];
    for (const row of scSum.multiRows) {
      if (row.verdict === SC_FAIL) {
        scExcludedDetail.push({ name: row.name, loadId: row.loadId, stage: row.stage,
          verdict: row.verdict, reason: row.evidence });
      }
    }
    for (const row of scSum.singleRows) {
      if (row.verdict === SC_FAIL && scExcludedDetail.filter((x: RepScalingExcluded) =>
        x.name === row.name).length === 0) {
        scExcludedDetail.push({ name: row.name, loadId: row.loadId, stage: row.stage,
          verdict: row.verdict, reason: row.evidence });
      }
    }
    const scalingAudit: RepScalingJson = {
      question: '这一项的吞吐真的随芯片算力变化吗? 还是被固定开销/缓存/内存延迟卡住了?',
      verdicts: scVerdicts,
      passCount: scSum.passN,
      failCount: scSum.failN,
      unprovenCount: scSum.unprovenN,
      hasAnyProbe: scSum.hasAnyProbe,
      excludedIds: scSum.excludedIds,
      excludedNames: scSum.excludedNames,
      excludedDetail: scExcludedDetail,
      excludedDetailNote: '每一项都带项名 + 判定口径 + 判定依据原文。跨芯片比较前请核对两台设备的' +
        '这份清单是不是同一批项: 清单不同 => 两台设备上"哪几项可信"不同, 原始比要连这份清单一起看。' +
        '注意(2026-10-05): 这份清单不改变复合分的参与项集合 —— 复合分的参与项 = 该阶段所有' +
        '"计分且拿到正分"的项(见 composite.gb7*Participants / gb7*ScopeOk)。' +
        '(判据与阈值都是我们自己定的, 不是官方阈值)',
      affectsCompositeScope: false,
      criteria: [
        'SC1 并行加速比 = 同一项多核分/单核分(工作量完全相同, 只改变可用算力); ' +
        '物理区间 [1.15, 实际用到核数 x 1.30], 区间外判 FAIL',
        'SC2 轮间离散度 = (最大-最小)/中位 x 100%, > 5% 判 FAIL(与 native 的 UNRELIABLE 同源)',
        '只出现在单核阶段的项(官方多核套件里没有它们)本轮没有算力变化的对照 -> 一律 UNPROVEN(未自证)'
      ],
      thresholdsAreOursNotOfficial: true,
      thresholdNote: 'SC1 的 1.15 / 1.30 与 SC2 的 5% 都是我们自己定的, 不是官方阈值',
      text: scSum.text,
      singleRows: scSingleRows,
      multiRows: scMultiRows
    };

    const npu: RepNpuJson = FullReportBuilder.npuJson(out);
    const storage: RepStorageJson = FullReportBuilder.storageJson(out);
    const crash: RepCrashJson = FullReportBuilder.crashJson(facts);

    const caps: SmtCapabilities | null = BenchRunner.benchCapabilities();
    const panel: CpuAffinityPanelData = BenchRunner.cpuAffinityPanel();
    const guard: string[] = [];
    guard.push('跑分期间无任何持续动画（静态文字 + 确定值进度条）');
    guard.push('UI 更新频率不高于每项一次');
    guard.push('整轮保持亮屏（keepScreen on/off 包夹）');
    guard.push('提供停止按钮；中断后已得结果照常出报告并注明未跑完');
    guard.push('报告分块写盘（逐块 writeSync，每 8 块让出 UI 线程）');
    const untouched: string[] = [];
    untouched.push('k / conv / 计分公式');
    untouched.push('metric 口径与单位');
    untouched.push('任何负载的工作量与尺寸');
    untouched.push('线程数与绑核策略');
    const env: RepEnvJson = {
      memText: facts.memText,
      thermalText: facts.thermalText,
      smtTopology: smtTopologyLine(caps),
      affinityPanel: panel.copyText,
      perfGuard: guard,
      untouched: untouched
    };

    // 逐块输出: 每块都很小(单节 stringify), 拼装时不会出现一个巨大的字符串
    const chunks: string[] = [];
    chunks.push('{"schema":"aurora-fullreport/1","jsonVersion":1,"header":');
    chunks.push(JSON.stringify(header));
    chunks.push(',"sections":');
    chunks.push(JSON.stringify(secs));
    chunks.push(',"items":');
    chunks.push(JSON.stringify(items));
    chunks.push(',"composite":');
    chunks.push(JSON.stringify(composite));
    // GPU-SNL 小节的结构化结论(2026-10 缺陷 3 修): 状态 / 非空失败原因 / 两个事实位。
    // 放在 npu 之前, 与执行顺序(SNL -> NPU)一致。
    chunks.push(',"sn":');
    chunks.push(JSON.stringify(FullReportBuilder.snJson(out)));
    chunks.push(',"npu":');
    chunks.push(JSON.stringify(npu));
    chunks.push(',"storage":');
    chunks.push(JSON.stringify(storage));
    chunks.push(',"crash":');
    chunks.push(JSON.stringify(crash));
    chunks.push(',"env":');
    chunks.push(JSON.stringify(env));
    // ---- 参考对照 / 线性-可比性审计(libauroraref.so, 只读) ----
    //   native 原样返回的对照表: 合法 JSON 才原样嵌(不合法就 table=null + error 里写明原因),
    //   免得"看不了对照表"变成"整个报告 JSON 打不开"。
    {
      const tableOk: boolean = ref.ok && looksLikeJsonObject(ref.json);
      const ra: RepRefAuditJson = {
        moduleAvailable: ref.moduleAvailable,
        moduleVersion: ref.moduleVersion,
        ok: ref.ok,
        error: ref.error,
        inputsJson: ref.optionsJson,
        inputsNote: ref.note,
        tableValid: tableOk,
        table: tableOk ? (JSON.parse(ref.json) as Object) : null
      };
      chunks.push(',"referenceAudit":');
      chunks.push(JSON.stringify(ra));
    }
    chunks.push(',"scalingAudit":');
    chunks.push(JSON.stringify(scalingAudit));
    chunks.push('}');
    return chunks;
  }

  private static emptyItemJson(section: string, name: string): RepItemJson {
    const j: RepItemJson = {
      section: section, name: name, metric: 0, unit: '', score: 0, ms: 0,
      parallelism: 0, threadsRequested: 0, threadsEffective: 0, workersBound: 0,
      runFreq: '', qos: '', cpuset: '', cpu: -1, cpuMaxKhz: 0, cpuAtStart: -1,
      cpuRank: -1, cpuBound: false, singleTargetCores: 0,
      machineTopTierCores: 0, machineTopTierKhz: 0, gpuFullness: '',
      fastClusterMask: '', fastClusterCores: 0, fastClusterMaxKhz: 0,
      inFastCluster: false, inMachineTopTier: false,
      allowedOk: false, allowedCount: 0, allowedMask: '', appliedMask: '',
      fastClusterFallback: false, fastClusterTierIndex: 0,
      diag: '', scoredByPolicy: false, scoredNote: '', ok: false,
      rounds: 0, repeatVerdict: '', scoreSpreadPct: 0,
      scalingVerdict: '', scalingEvidence: '', loadId: -1
    };
    return j;
  }

  private static ownItemJson(section: string, t: TestResult): RepItemJson {
    const j: RepItemJson = FullReportBuilder.emptyItemJson(section, t.name);
    j.metric = t.metric;
    j.unit = t.unit;
    j.score = t.score;
    j.ms = t.ms;
    j.parallelism = t.parallelism;
    j.scoredByPolicy = true;
    j.scoredNote = t.score > 0 ? '计分项（自研套件复合分）' : '未得分 -> 失败';
    j.ok = t.score > 0;
    j.diag = t.detail;
    // ---- 落核 / 该核标称上限 / 运行时频率取证(2026-10 补; 旁路, 不计分) ----
    //  与 CS1 项走的是同一批键名(见 gb7ItemJson): 报告对比工具与 App 内对比页只读 items[],
    //  以前这里被丢掉 -> "自研套件跑在哪颗核上"在 JSON 报告里彻底看不见。
    j.runFreq = t.runFreq;
    j.cpu = t.cpu;
    j.cpuMaxKhz = t.cpuMaxKhz;
    j.cpuAtStart = t.cpuAtStart;
    j.cpuRank = t.cpuRank;
    j.cpuBound = t.cpuBound;
    j.singleTargetCores = t.cpuSingleTargetCores;
    j.machineTopTierCores = t.cpuMachineTopTierCores;
    j.machineTopTierKhz = t.cpuMachineTopTierKhz;
    j.fastClusterCores = t.cpuFastClusterCores;
    j.fastClusterMaxKhz = t.cpuFastClusterMaxKhz;
    j.inFastCluster = t.cpuInFastCluster;
    j.inMachineTopTier = t.cpuInMachineTopTier;
    j.allowedCount = t.cpuAllowedCount;
    j.allowedOk = t.cpuAllowedCount > 0;
    return j;
  }

  private static gb7ItemJson(section: string, r: Gb7Result, threads: number): RepItemJson {
    const j: RepItemJson = FullReportBuilder.emptyItemJson(section, r.name);
    j.metric = r.metric;
    j.unit = r.unit;
    j.score = r.score;
    j.ms = r.ms;
    j.parallelism = r.cpuThreadsEffective > 0 ? r.cpuThreadsEffective : threads;
    j.threadsRequested = r.cpuThreadsRequested;
    j.threadsEffective = r.cpuThreadsEffective;
    j.workersBound = r.cpuWorkers;
    j.runFreq = r.runFreq;
    // QoS / 只读分层诊断: 原样搬 native 的文本(与 .txt 的 item 块同源, 保证 .json 与 .txt 都有)
    j.qos = r.qos;
    j.cpuset = r.cpuset;
    j.cpu = r.cpu;
    j.cpuMaxKhz = r.cpuMaxKhz;
    j.cpuAtStart = r.cpuAtStart;
    j.fastClusterMask = r.cpuFastClusterMask;
    j.fastClusterCores = r.cpuFastClusterCores;
    j.fastClusterMaxKhz = r.cpuFastClusterMaxKhz;
    j.inFastCluster = r.cpuInFastCluster;
    j.inMachineTopTier = r.cpuInMachineTopTier;
    j.allowedOk = r.cpuAllowedOk;
    j.allowedCount = r.cpuAllowedCount;
    j.allowedMask = r.cpuAllowedMask;
    j.appliedMask = r.cpuAppliedMask;
    j.fastClusterFallback = r.cpuFastClusterFallback;
    j.fastClusterTierIndex = r.cpuFastClusterTierIndex;
    j.diag = r.diag;
    j.scoredByPolicy = r.scoredByPolicy;
    j.scoredNote = FullReportBuilder.gb7ScoredNote(r);
    j.ok = r.score > 0;
    // 可重复性 / 离散度【纯展示】: 原样搬 native 的值, 一个字都不加工。
    // 老 native / GPU 路径没有这一块 -> 保持 0 / 空串(报告里能一眼看出"没有这一列")。
    j.rounds = r.rounds;
    j.repeatVerdict = r.repeatVerdict;
    j.scoreSpreadPct = r.scoreSpreadPct;
    // 随芯片变化自检【旁路结论】: 原样搬, 一个字都不加工('' = 本路径没做这项自检)。
    j.scalingVerdict = r.scalingVerdict;
    j.scalingEvidence = r.scalingEvidence;
    j.loadId = r.loadId;
    // GPU「跑满判据」一行(native gpu7Fullness 的 text, 原样搬; 只有 GPU 项有, 其余为空串)
    j.gpuFullness = r.gpuFullness;
    return j;
  }

  // ---- GPU-SNL 的结构化结论块(2026-10 缺陷 3 修; 见 RepSnJson 的说明) ----
  private static snJson(out: FullRunOutcome): RepSnJson {
    const sn: SnRunResult | null = out.gpuSnl;
    if (sn === null) {
      return {
        section: 'GPU-SNL', workload: 'Aurora Nomad Light', scored: false, gb7Item: false,
        ran: false, parseOk: false, selfClaimedOk: false,
        selfClaimNote: '', status: '未执行',
        failReason: out.gpuSnlError.length > 0 ? out.gpuSnlError : '本节未执行（没有拿到结果）',
        prepError: '', valuesAvailable: false, score: 0, fps: 0, throughputMpx: 0,
        measuredFrames: 0, rawChars: 0, rawWhere: '',
        note: '本小节在 native 侧固定 scored=false / gb7Item=false, 不并入任何复合分。'
      };
    }
    const ran: boolean = sn.prepError.length === 0;
    const okAll: boolean = ran && sn.ok;
    // 三个事实互相独立, 一个都不许互相冒充:
    //   ran          = run() 真的被调用过(prepare 没失败);
    //   parseOk      = 它的返回值是不是合法 JSON;
    //   selfClaimedOk= 原文自己写了什么(不是结论)。
    const selfClaimNote: string = (!sn.rawParseOk && sn.rawSelfClaimOk)
      ? ('矛盾已核实 原始 JSON 文本自称 "ok":true, 但这份 JSON 本身不是合法 JSON, '
        + '解析不出任何字段 —— 本节按失败记录(不以原文的自称为结论); 原文未做任何加工, '
        + '见 rawWhere 指的位置。')
      : '';
    return {
      section: sn.section, workload: sn.workload, scored: sn.scored, gb7Item: sn.gb7Item,
      ran: ran,
      parseOk: sn.rawParseOk,
      selfClaimedOk: sn.rawSelfClaimOk,
      selfClaimNote: selfClaimNote,
      status: okAll ? '成功' : '失败',
      failReason: okAll ? '' : FullReportBuilder.snFailReason(sn),
      prepError: sn.prepError,
      // 只有"解析成功"时这些数才是真读数; 解析失败时一律 valuesAvailable=false,
      // 读者据此知道下面那几个 0 是"没取到", 不是"读到了 0"。
      valuesAvailable: sn.rawParseOk && ran,
      score: sn.rawParseOk ? sn.score : 0,
      fps: sn.rawParseOk ? sn.fps : 0,
      throughputMpx: sn.rawParseOk ? sn.throughputMpx : 0,
      measuredFrames: sn.rawParseOk ? sn.measuredFrames : 0,
      rawChars: sn.raw.length,
      rawWhere: sn.raw.length > 0
        ? 'referenceAudit.inputsJson.gpuSnl（native 原样返回的原始 JSON, 未做任何加工）' : '',
      note: '本小节在 native 侧固定 scored=false / gb7Item=false, 不并入 CS1 / 自研套件复合分。'
        + 'parseOk=false 时, 本块里的分数/帧率一律是"未取到"的默认值, 不是实测 0。'
    };
  }

  private static npuJson(out: FullRunOutcome): RepNpuJson {
    const pr: NpuProbeResult | null = out.npuProbe;
    const rn: NpuRunResult | null = out.npuRun;
    const j: RepNpuJson = {
      sectionScored: false,
      sectionScoredNote: 'native 侧固定 scored=false / gb7Item=false：本小节不并入任何复合分。',
      probeRan: pr !== null,
      probeOk: pr !== null ? pr.ok : false,
      probeStatus: pr !== null ? pr.status : '',
      probeText: pr !== null ? pr.text : '',
      probeError: pr !== null ? pr.error : out.npuError,
      thirdPartyNpuOpen: pr !== null ? pr.thirdPartyNpuOpen : false,
      probeDeviceCount: pr !== null ? pr.deviceCount : 0,
      nnrtAvailable: pr !== null ? pr.nnrtAvailable : false,
      nnrtBound: pr !== null ? pr.nnrtBound : 0,
      nnrtExpected: pr !== null ? pr.nnrtExpected : 0,
      nnrtMissingSymbols: pr !== null ? pr.missingSymbols : '',
      nnrtDlerror: pr !== null ? pr.runtimeDlerror : '',
      runRan: rn !== null,
      runOk: rn !== null ? rn.ok : false,
      runStatus: rn !== null ? rn.status : '',
      runText: rn !== null ? rn.text : '',
      runError: rn !== null ? rn.error : out.npuError,
      deviceTypeName: rn !== null ? rn.deviceTypeName : '',
      deviceLabel: rn !== null ? rn.deviceLabel : '',
      deviceId: rn !== null ? rn.deviceId : -1,
      deviceName: rn !== null ? rn.deviceName : '',
      opSetName: rn !== null ? rn.opSetName : '',
      mainOp: rn !== null ? rn.mainOp : '',
      opSetFallbackUsed: rn !== null ? rn.opSetFallbackUsed : false,
      precisionShort: rn !== null ? rn.precisionShort : '',
      scaleLabel: rn !== null ? rn.scaleLabel : '',
      macsPerInference: rn !== null ? rn.macsPerInference : 0,
      flopsPerInference: rn !== null ? rn.flopsPerInference : 0,
      goPs: rn !== null ? rn.goPs : 0,
      macsPerSec: rn !== null ? rn.macsPerSec : 0,
      meanMs: rn !== null ? rn.meanMs : 0,
      medianMs: rn !== null ? rn.medianMs : 0,
      minMs: rn !== null ? rn.minMs : 0,
      maxMs: rn !== null ? rn.maxMs : 0,
      measuredRuns: rn !== null ? rn.measuredRuns : 0,
      warmupRuns: rn !== null ? rn.warmupRuns : 0,
      budgetHit: rn !== null ? rn.budgetHit : false,
      hint: rn !== null ? rn.hint : '',
      jsException: out.npuError,
      probeRawJson: pr !== null ? pr.raw : '',
      runRawJson: rn !== null ? rn.raw : ''
    };
    return j;
  }

  private static storageJson(out: FullRunOutcome): RepStorageJson {
    const st: StorageRunResult | null = out.storage;
    const list: RepStorageItemJson[] = [];
    const warns: string[] = [];
    if (st !== null) {
      for (const it of st.items) {
        list.push({
          id: it.id, name: it.name, ok: it.ok, ms: it.ms, bytes: it.bytes, mbps: it.mbps,
          iops: it.iops, ops: it.ops, auxMs: it.auxMs, auxName: it.auxName, mode: it.mode,
          comparable: it.comparable, error: it.error, note: it.note
        });
      }
      for (const w of st.warnings) {
        warns.push(w);
      }
    }
    const j: RepStorageJson = {
      sectionScored: false,
      sectionScoredNote: 'native 侧固定 scored=false：本小节不并入任何复合分。',
      filesDir: FullReportStore.get().filesDir(),
      prepared: out.storagePrepared,
      infoJson: out.storageInfo,
      ran: st !== null,
      ok: st !== null ? st.ok : false,
      skipped: st !== null ? st.skipped : false,
      skipReason: st !== null ? st.skipReason : '',
      fatal: st !== null ? st.fatal : out.storageError,
      sectionTitle: st !== null ? st.sectionTitle : '',
      dir: st !== null ? st.dir : '',
      totalMs: st !== null ? st.totalMs : 0,
      budgetMs: st !== null ? st.budgetMs : 0,
      stoppedEarly: st !== null ? st.stoppedEarly : false,
      stopReason: st !== null ? st.stopReason : '',
      cleanupOk: out.storageCleanupOk,
      seqWriteDurableMbps: st !== null ? st.seqWriteDurableMbps : 0,
      seqReadColdMbps: st !== null ? st.seqReadColdMbps : 0,
      randRead4kIops: st !== null ? st.randRead4kIops : 0,
      randWrite4kIops: st !== null ? st.randWrite4kIops : 0,
      filesPerSecCreate: st !== null ? st.filesPerSecCreate : 0,
      filesPerSecDelete: st !== null ? st.filesPerSecDelete : 0,
      totalBytes: st !== null ? st.totalBytes : 0,
      freeBytes: st !== null ? st.freeBytes : 0,
      availBytes: st !== null ? st.availBytes : 0,
      blockSize: st !== null ? st.blockSize : 0,
      oDirectWorks: st !== null ? st.oDirectWorks : false,
      fadviseDontNeedWorks: st !== null ? st.fadviseDontNeedWorks : false,
      bypassMethod: st !== null ? st.bypassMethod : '',
      warnings: warns,
      items: list,
      rawJson: st !== null ? st.raw : ''
    };
    return j;
  }

  private static crashJson(facts: FullRunUiFacts): RepCrashJson {
    const crashPath: string = BenchRunner.nativeGuardPath();
    const hangPath: string = BenchRunner.nativeHangPath();
    const cf: EvidenceFileFact = evidenceFact('native 崩溃现场', crashPath);
    const hf: EvidenceFileFact = evidenceFact('native 卡死现场', hangPath);
    const evs: RepEvidenceJson[] = [];
    evs.push({ label: cf.label, path: cf.path, exists: cf.exists, bytes: cf.bytes, note: cf.err });
    evs.push({ label: hf.label, path: hf.path, exists: hf.exists, bytes: hf.bytes, note: hf.err });
    // 逐项地址空间账目(旁路)。放在证据文件表里而不是另起一套结构: 它就是一份证据文件,
    // 而且这样不需要改 RepCrashJson 的接口(接口一改, 解析侧与对比侧都要跟着动)。
    const mf: EvidenceFileFact = evidenceFact('逐项地址空间账目', BenchRunner.itemMemoryLogPath());
    evs.push({ label: mf.label, path: mf.path, exists: mf.exists, bytes: mf.bytes, note: mf.err });
    const j: RepCrashJson = {
      guardReady: BenchRunner.nativeGuardReady(),
      guardStatus: CrashGuard.get().guardStatusText(),
      guardLogPath: crashPath,
      crashFileExists: cf.exists,
      crashFileBytes: cf.bytes,
      hangFileExists: hf.exists,
      hangFileBytes: hf.bytes,
      crashRecordFoundThisSession: facts.nativeCrashFound,
      crashRecordChars: facts.nativeCrashChars,
      crashRecordSummary: facts.nativeCrashSummary,
      crumbFound: facts.crashBannerFound,
      crumbLine: facts.crashBannerLine,
      evidenceFiles: evs
    };
    return j;
  }
}

// ===========================================================================
// 报告落盘: files/ 下的 report-<时间戳>.txt / .json + report-latest.txt / .json
// 写盘分块(逐块 fs.writeSync, 每 8 块让出一次 UI 线程), 不一次性拼巨大字符串。
// ===========================================================================
// 落盘目标 + 落盘结果的载体(见 FullReportStore.save)。
// 为什么需要它: 报告正文里要写"这份报告落到哪", 而路径只有写盘前先算出来才是真话 ——
// 以前正文是拿写盘后才回填的 out.reportTextPath 拼的, 于是永远印「（未落盘）」。
//   · stamp/paths: 写盘前就算好(只取决于本轮开始时间), 正文里印的就是真实路径;
//   · ok/status   : 写盘后回填, 再用同一份 sink 重建一次正文(内存 + 盘上都说真话)。
export interface ReportSinkInfo {
  stamp: string;
  textPath: string;
  jsonPath: string;
  latestTextPath: string;
  latestJsonPath: string;
  ok: boolean;
  status: string;
}

export interface FullReportSaveResult {
  ok: boolean;
  textPath: string;
  jsonPath: string;
  latestTextPath: string;
  latestJsonPath: string;
  textChars: number;
  jsonChars: number;
  err: string;
}

// ---- 写盘兜底断言(2026-10 真机报告缺陷 4) ----
// 正文拼装阶段(FullReportBuilder.textChunks)已经把所有孤立代理项换成了 U+FFFD, 所以
// 正常路径下这里永远不触发。它存在的意义是: 将来若有别的路径(或 native 新字段)绕过
// 正文拼装, 写盘这一步也不会写出非法 UTF-8 —— 并且触发次数会被记下来写进落盘回执,
// 让"报告 .txt 必须是合法 UTF-8"这条硬断言不会被静默绕过。
let g_txtGuardHits: number = 0;
let g_txtGuardReplaced: number = 0;

function txtGuardPiece(c: string): string {
  const why: string = assertTxtUtf8Safe(c);
  if (why.length === 0) {
    return c;
  }
  g_txtGuardHits++;
  g_txtGuardReplaced += checkReportText(c).loneSurrogates;
  return sanitizeReportText(c);
}

export class FullReportStore {
  private static inst: FullReportStore | null = null;
  private dir: string = '';
  private textParts: string[] = [];
  private jsonParts: string[] = [];
  private textCharsTotal: number = 0;
  private jsonCharsTotal: number = 0;
  private textPathValue: string = '';
  private jsonPathValue: string = '';
  private saveNote: string = '';

  static get(): FullReportStore {
    if (!FullReportStore.inst) {
      FullReportStore.inst = new FullReportStore();
    }
    return FullReportStore.inst;
  }

  init(ctx: common.Context): void {
    if (ctx !== null && ctx.filesDir.length > 0) {
      this.dir = ctx.filesDir;
    }
  }

  filesDir(): string {
    return this.dir;
  }

  isReady(): boolean {
    return this.dir.length > 0;
  }

  // 当前内存里那份报告全文(复制 / 导出用; 只在用户按下按钮时拼一次)
  text(): string {
    return this.textParts.join('');
  }

  jsonText(): string {
    return this.jsonParts.join('');
  }

  textChars(): number {
    return this.textCharsTotal;
  }

  jsonChars(): number {
    return this.jsonCharsTotal;
  }

  mainTextPath(): string {
    return this.textPathValue;
  }

  mainJsonPath(): string {
    return this.jsonPathValue;
  }

  saveNoteText(): string {
    return this.saveNote;
  }

  hasReport(): boolean {
    return this.textParts.length > 0;
  }

  // 生成 + 落盘。
  // 两趟写法的原因(2026-10-08 修"报告明明落盘成功却显示未落盘")
  //   报告正文里要写"这份报告落到哪、写成功没有", 而"写成功没有"这件事只有写完之后才知道:
  //   第一趟先用已知的真实目标路径 + "正在写入"生成正文并写盘; 拿到结果后回填 sink.ok/status,
  //   再用同一份 sink 重建正文 —— 内存副本(界面/复制/导出用)与盘上那份就都是真话了。
  //   首趟全部写成功时把四个文件按重建后的正文再写一遍(多一次分块写, 换来"盘上那份与
  //   界面上那份说的一样"); 首趟就失败时不再重写(重写也写不进去), 内存副本仍写明失败原因。
  async save(out: FullRunOutcome, facts: FullRunUiFacts): Promise<FullReportSaveResult> {
    const res: FullReportSaveResult = {
      ok: false, textPath: '', jsonPath: '', latestTextPath: '', latestJsonPath: '',
      textChars: 0, jsonChars: 0, err: ''
    };
    // ① 目标路径: 只取决于本轮开始时间, 拼正文之前就能算出来
    const sink: ReportSinkInfo = {
      stamp: '', textPath: '', jsonPath: '', latestTextPath: '', latestJsonPath: '',
      ok: false, status: ''
    };
    if (this.dir.length > 0) {
      sink.stamp = stampFile(out.startedAt > 0 ? out.startedAt : Date.now());
      sink.textPath = this.dir + '/report-' + sink.stamp + '.txt';
      sink.jsonPath = this.dir + '/report-' + sink.stamp + '.json';
      sink.latestTextPath = this.dir + '/report-latest.txt';
      sink.latestJsonPath = this.dir + '/report-latest.json';
    }
    // ①b 参考对照 / 线性-可比性审计(libauroraref.so, 只读): 每轮只调一次, 结果同时进
    //     文本报告与 JSON 报告。几个只能由 ArkTS 提供的量在这里从既有旁路字段取(见 RefAudit.ets):
    //     threadsUsed / workerCpus / workerCpusList / allowedCores / logicalCores /
    //     physicalCores / runtimeKhzMedian / nominalTopKhz —— 拿不到就不传(null 由 native 写)。
    const cores: number = BenchRunner.cores();
    const reqThreads: number = BenchRunner.benchMultiThreads(cores > 0 ? cores : 4);
    const refInput: RefAuditInput = {
      single: out.gb7Single,
      multi: out.gb7Multi,
      gpu7: out.gb7Gpu,
      snRaw: out.gpuSnl !== null ? out.gpuSnl.raw : '',
      singleComposite: FullReportBuilder.geoOf(out.gb7Single),
      multiComposite: FullReportBuilder.geoOf(out.gb7Multi),
      requestedThreads: reqThreads
    };
    const ref: RefAuditResult = runRefAudit(refInput);
    // ①c 复合分的可重复性(两个数都报): 只调 native 的纯计算接口(不跑负载), 取它那一刻的
    //     逐轮复合分矩阵 —— 与本轮刚跑完的 GB7 结果同源。
    // 口径统一: 把本轮单核/多核各自的逐项分数组传下去, native 与报告由构造同源。
    const crep: CompositeRepeatFacts = compositeRepeatFacts(out.gb7Single, out.gb7Multi);
    // ② 正文(此时 sink.status 为空 = "正在写入")
    let textChunks: string[] = FullReportBuilder.textChunks(out, facts, sink, ref, crep);
    let jsonChunks: string[] = FullReportBuilder.jsonChunks(out, facts, sink, ref, crep);
    for (const c of textChunks) {
      res.textChars += c.length;
    }
    for (const c of jsonChunks) {
      res.jsonChars += c.length;
    }
    this.textParts = textChunks;
    this.jsonParts = jsonChunks;
    this.textCharsTotal = res.textChars;
    this.jsonCharsTotal = res.jsonChars;
    out.reportChars = res.textChars;
    out.jsonChars = res.jsonChars;

    if (this.dir.length === 0) {
      res.err = 'filesDir 还没就绪，报告只留在内存里（可复制/导出，但没有落盘）。';
      sink.status = '未落盘：' + res.err;
      // 重建一次: 内存里这份正文要写出"没落盘"以及原因
      textChunks = FullReportBuilder.textChunks(out, facts, sink, ref, crep);
      jsonChunks = FullReportBuilder.jsonChunks(out, facts, sink, ref, crep);
      this.textParts = textChunks;
      this.jsonParts = jsonChunks;
      this.saveNote = res.err;
      out.reportSaveError = res.err;
      return res;
    }

    const textPath: string = sink.textPath;
    const jsonPath: string = sink.jsonPath;
    const latestText: string = sink.latestTextPath;
    const latestJson: string = sink.latestJsonPath;
    const errs: string[] = [];
    try {
      await this.writeChunks(textPath, textChunks);
    } catch (e) {
      errs.push('写 ' + textPath + ' 失败: ' + e.message);
    }
    try {
      await this.writeChunks(jsonPath, jsonChunks);
    } catch (e) {
      errs.push('写 ' + jsonPath + ' 失败: ' + e.message);
    }
    // 最新副本(latest)是"打开就能看最近一次"的便利路径; 失败不影响主文件
    try {
      await this.writeChunks(latestText, textChunks);
    } catch (e) {
      errs.push('写 report-latest.txt 失败: ' + e.message);
    }
    try {
      await this.writeChunks(latestJson, jsonChunks);
    } catch (e) {
      errs.push('写 report-latest.json 失败: ' + e.message);
    }

    res.ok = errs.length === 0;
    res.err = errs.length > 0 ? errs.join(' / ') : '';
    res.textPath = textPath;
    res.jsonPath = jsonPath;
    res.latestTextPath = latestText;
    res.latestJsonPath = latestJson;
    this.textPathValue = textPath;
    this.jsonPathValue = jsonPath;
    // ③ 回填真实结果
    sink.ok = res.ok;
    sink.status = res.ok
      ? ('成功：正文与 JSON 均已写入（分块 writeSync + close）—— ' + textPath + ' · ' + jsonPath +
        '（并已更新 report-latest.txt / report-latest.json）')
      : ('未写成功：' + res.err);
    // ---- 写盘兜底断言的自证(2026-10 缺陷 4): 正常永远不触发; 一旦触发就明写在回执里,
    //      不让"报告 .txt 必须是合法 UTF-8"这条断言被静默绕过。
    if (g_txtGuardHits > 0) {
      sink.status += ' ｜ 文本编码兜底断言触发 ' + g_txtGuardHits.toString() + ' 次（替换 ' +
        g_txtGuardReplaced.toString() + ' 个孤立代理项）: 有路径绕过了正文拼装阶段的替换 —— ' +
        '本文件仍是合法 UTF-8，但来源要查（见正文末「文本编码自检」一段）';
    }
    this.saveNote = res.ok
      ? ('报告已落盘：' + textPath + ' · ' + jsonPath + '（并更新 report-latest.*）')
      : ('落盘部分失败：' + res.err);
    out.reportTextPath = textPath;
    out.reportJsonPath = jsonPath;
    out.reportLatestTextPath = latestText;
    out.reportLatestJsonPath = latestJson;
    out.reportSaveError = res.err;
    // ④ 用真实结果重建正文(内存副本立刻)
    textChunks = FullReportBuilder.textChunks(out, facts, sink, ref, crep);
    jsonChunks = FullReportBuilder.jsonChunks(out, facts, sink, ref, crep);
    this.textParts = textChunks;
    this.jsonParts = jsonChunks;
    res.textChars = 0;
    res.jsonChars = 0;
    for (const c of textChunks) {
      res.textChars += c.length;
    }
    for (const c of jsonChunks) {
      res.jsonChars += c.length;
    }
    this.textCharsTotal = res.textChars;
    this.jsonCharsTotal = res.jsonChars;
    out.reportChars = res.textChars;
    out.jsonChars = res.jsonChars;
    // ⑤ 首趟已写成功 -> 把盘上那份也换成带真实回执的正文; 复写失败记下来(不假装成功)
    if (res.ok) {
      const errs2: string[] = [];
      try {
        await this.writeChunks(textPath, textChunks);
        await this.writeChunks(latestText, textChunks);
        await this.writeChunks(jsonPath, jsonChunks);
        await this.writeChunks(latestJson, jsonChunks);
      } catch (e) {
        errs2.push('回填落盘结果时复写失败: ' + e.message);
      }
      if (errs2.length > 0) {
        res.ok = false;
        res.err = errs2.join(' / ');
        sink.ok = false;
        sink.status = '未写成功（复写回执失败，盘上可能是上一次的半截内容）：' + res.err;
        this.saveNote = '落盘部分失败：' + res.err;
        out.reportSaveError = res.err;
        textChunks = FullReportBuilder.textChunks(out, facts, sink, ref, crep);
        jsonChunks = FullReportBuilder.jsonChunks(out, facts, sink, ref, crep);
        this.textParts = textChunks;
        this.jsonParts = jsonChunks;
      }
    }
    return res;
  }

  // 分块写盘: 每块一次 writeSync, 每 8 块 await 一次让 UI 线程喘口气
  private async writeChunks(path: string, chunks: string[]): Promise<number> {
    let f: fs.File | null = null;
    let written: number = 0;
    let i: number = 0;
    try {
      f = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      // .txt 与 .json 分两类: 文本报告走 UTF-8 合法性硬断言(兜底), JSON 报告由
      // JSON.stringify 保证(孤立代理项会被它写成 \\uXXXX 转义, 本来就是合法 UTF-8)。
      const isText: boolean = path.endsWith('.txt');
      for (const c of chunks) {
        if (c.length === 0) {
          continue;
        }
        written += fs.writeSync(f.fd, isText ? txtGuardPiece(c) : c);
        i++;
        if (i % 8 === 0) {
          await sleepMs(0);
        }
      }
    } finally {
      if (f !== null) {
        fs.closeSync(f);
      }
    }
    return written;
  }

  // 进程被系统杀掉后再打开 App: 把上一次的 report-latest.txt 读回来。
  // 说明: 这里是把整个文件读成一个字符串(报告通常几百 KB), 只在"用户点了查看/复制/导出"或
  // "跑完出报告"这三处各读一次 —— 只把尾部 maxChars 交给界面渲染, 不把全文塞进 UI。
  loadLatestText(maxChars: number): string {
    if (this.dir.length === 0) {
      return '';
    }
    const p: string = this.dir + '/report-latest.txt';
    try {
      if (!fs.accessSync(p)) {
        return '';
      }
      const size: number = fs.statSync(p).size;
      const all: string = fs.readTextSync(p);
      if (all.length <= maxChars) {
        return all;
      }
      return '…（前面还有 ' + (all.length - maxChars).toString() + ' 字符，全文 ' +
        all.length.toString() + ' 字符 / ' + size.toString() + ' 字节，见 ' + p + '）\n' +
        all.substring(all.length - maxChars);
    } catch (e) {
      return '';
    }
  }

  latestTextPath(): string {
    return this.dir.length > 0 ? (this.dir + '/report-latest.txt') : '';
  }

  // 盘上有没有上一次的报告(latest 副本); 只看存在与大小, 不把内容读进来
  hasLatestOnDisk(): boolean {
    if (this.dir.length === 0) {
      return false;
    }
    try {
      const p: string = this.dir + '/report-latest.txt';
      if (!fs.accessSync(p)) {
        return false;
      }
      return fs.statSync(p).size > 0;
    } catch (e) {
      return false;
    }
  }

  latestTextBytes(): number {
    if (this.dir.length === 0) {
      return 0;
    }
    try {
      const p: string = this.dir + '/report-latest.txt';
      if (!fs.accessSync(p)) {
        return 0;
      }
      return fs.statSync(p).size;
    } catch (e) {
      return 0;
    }
  }

  latestJsonPath(): string {
    return this.dir.length > 0 ? (this.dir + '/report-latest.json') : '';
  }

  // 导出用的文件名(带时间戳, 便于收件人区分)
  exportName(): string {
    return 'aurora-fullreport-' + stampFile(Date.now()) + '.txt';
  }
}
