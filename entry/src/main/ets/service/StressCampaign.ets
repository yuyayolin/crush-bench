import { BenchStore, STRESS_SESSION_KEY, STRESS_REPORT_KEY } from './BenchStore';
import { CrashGuard, crumbWhen } from './CrashGuard';
import { Gb7Result, BenchRecord } from '../model/BenchModel';
// metric(吞吐)的显示口径: 与 native 的 "%.4g" 同口径的 4 位有效数字(见 common/NumberFmt.ets)。
// 失败原因里那个 metric 必须与卡片上的吞吐、与 native 算分数用的那一串同一口径,
// 否则会出现"卡片写 0.2、失败原因写 0.20"这种对不上的写法。
import { metricUnit } from '../common/NumberFmt';

// ---------------------------------------------------------------------------
// 稳定性压测会话(连续 N 轮 CS1 单核全套)
//
// 要解决的问题: native 越界 / OOM 被系统杀会直接杀掉进程, ArkTS 完全捕获不到。
// 既然拦不住, 至少要做到 "崩了也知道崩在哪、下一轮能接着跑、最后能自证成功率"。
//
// 机制:
//   1. 会话状态整份写进 preferences(键 = stress_campaign), 每次开跑一项前 flush 到盘,
//      所以进程无论怎么死, 盘上永远有"第几轮 / 哪一项"。
//   2. running = true 是唯一的"上次没结束"判据(不依赖面包屑, 面包屑只用于更精确的归因)。
//   3. completed 语义 = 已经完整跑完的轮数。第 N 轮跑到一半被杀时盘上是 completed = N-1,
//      因此下次启动无条件从 completed + 1 轮重跑(被中断的那一轮整轮重跑, 不拼半轮分数)。
//   4. 压测期间不写历史记录, 只在会话结束时汇总写一条 BenchRecord。
// ---------------------------------------------------------------------------

// 会话状态键名(与 BenchStore 里的常量同源, 这里再导出一次方便 UI/文档引用)
export const STRESS_KEY = STRESS_SESSION_KEY;
export const STRESS_REPORT = STRESS_REPORT_KEY;

// 逐次失败/跳过明细的条数上限: 防止"每轮都失败"时状态 JSON 无限膨胀(1000 轮 x 16 项 = 16000 条)
export const FAIL_MAX = 200;
export const SKIP_MAX = 200;
// 崩溃次数上限(崩溃 200 次以上基本已经不是"跑分"问题了)
export const CRASH_MAX = 200;
// 每轮逐项状态存多少项(CS1 单核 16 项, 留一倍冗余)
export const ROUND_ITEM_MAX = 64;

export const GATE_STAT_ROUNDS = 300;
// 报告文本里"逐条明细"的显示上限: 统计是完整的(按项聚合), 但明细只列最近若干条,
// 否则 1000 轮 x 多项失败会把报告撑到几十 KB, 复制和阅读都不现实。
export const TEXT_DETAIL_MAX = 60;
// 报告里"全部成功轮分数"最多列多少个(1000 轮全列出来是 6000+ 字符)
export const TEXT_SCORE_MAX = 30;

export interface FailItemStat {
  item: string;      // 粗粒度名: unit 里 failed/partial 标记去掉
  runs: number[];    // 出现过该失败的轮次
  count: number;
}

export interface FailItemDetail {
  run: number;
  item: string;
  phase: string;
  reason: string;
}

export interface SkipItemStat {
  item: string;
  runs: number[];
  count: number;
}

export interface SkipItemDetail {
  run: number;
  item: string;
  reason: string;
}

export interface CrashItem {
  run: number;
  item: string;
  ts: number;
  runId: string;     // 崩溃前那一次的跑分会话 id(与面包屑 runId 对齐)
}

export interface RoundStatus {
  item: string;
  status: string;    // ok / fail / skip
}

export interface CampaignState {
  target: number;          // 目标轮数
  completed: number;       // 已完成(整轮跑完)的轮数
  startedAt: number;
  failures: FailItemDetail[];
  crashes: CrashItem[];
  scores: number[];        // 每完成一轮的综合分(几何平均, 与首页 CS1 单核卡一致)
  durations: number[];     // 每完成一轮的耗时 ms
  running: boolean;        // true = 上次压测没结束(崩溃恢复的唯一判据)
  runId: string;
  roundOpen: boolean;      // 某轮已经"开跑"但还没跑完(用于区分"真被打断"和"轮次之间的正常停摆")
  lastRun: number;         // 最后开始的那一轮(1-based)
  lastItem: string;        // 最后启动的项名(崩溃归因)
  lastTs: number;
  failStats: FailItemStat[];
  skipItems: SkipItemDetail[];
  skipStats: SkipItemStat[];
  gateUsed: boolean;       // 做过内存闸门
  gateUnavailable: boolean;// 内存接口不可用(降级: 本次未做闸门)
  thermalFirst: number;    // 首轮热状态(采样点)
  thermalLast: number;     // 末轮热状态(采样点)
  interrupted: number;     // 被崩溃打断、后来重跑过的轮次(计数)
  interruptedRuns: number[]; // 被打断过的轮次号(去重用, 避免重复计数)
  roundItems: RoundStatus[];// 最后启动那一轮的逐项状态(崩溃后看得到"哪一项没跑完")
  endedAt: number;
}

export interface CampaignReport {
  target: number;
  completed: number;
  successRounds: number;
  successRate: number;
  failStats: FailItemStat[];
  failures: FailItemDetail[];
  skipStats: SkipItemStat[];
  skipItems: SkipItemDetail[];
  crashes: CrashItem[];
  scores: number[];
  minScore: number;
  maxScore: number;
  medianScore: number;
  meanScore: number;
  durations: number[];
  totalMs: number;
  startedAt: number;
  endedAt: number;
  running: boolean;
  gateUsed: boolean;
  gateUnavailable: boolean;
  interrupted: number;
  text: string;
}

// 一轮的汇总结果(由压测循环产出, 再交给 recordRound 落盘)
export interface RoundOutcome {
  run: number;
  score: number;
  elapsedMs: number;
  finished: boolean;
  failCount: number;
  skipCount: number;
  failures: FailItemDetail[];
  skips: SkipItemDetail[];
  statuses: RoundStatus[];
  thermal: number;
}

// 与 CrashGuard.crumbWhen 同一格式, 便于两份日志对照
export function stamp(ts: number): string {
  if (ts <= 0) {
    return '--';
  }
  return crumbWhen(ts);
}

function fmtMs(ms: number): string {
  if (!(ms > 0)) {
    return '0 ms';
  }
  const s: number = ms / 1000;
  if (s < 60) {
    return s.toFixed(1) + ' s';
  }
  const m: number = Math.floor(s / 60);
  return m.toString() + ' 分 ' + (s - m * 60).toFixed(1) + ' 秒';
}

function median(values: number[]): number {
  if (values.length === 0) {
    return 0;
  }
  const copy: number[] = values.slice();
  copy.sort((a: number, b: number) => a - b);
  const mid: number = Math.floor(copy.length / 2);
  if (copy.length % 2 === 1) {
    return copy[mid];
  }
  return (copy[mid - 1] + copy[mid]) / 2;
}

// 单项"非崩溃失败"判据(纯读 unit / score, 不改任何数值):
//   a) unit 里带 native 自己打的 "failed:" / "partial:" 标记(clang / sfm / video 会打)
//   b) 该项没拿到分, 且 unit 里没有 "未计分" 说明(未计分是设计如此, 不算失败)
export function itemFailure(result: Gb7Result): string {
  const u: string = result.unit;
  const iFailed: number = u.indexOf('failed:');
  const iPartial: number = u.indexOf('partial:');
  if (iFailed >= 0 || iPartial >= 0) {
    // u 形如 "Mpx/s (partial: 3/12 frames decoded, rejected at frame 3 of pass 2)"。
    // 直接原样给出 native 自己的诊断, 不自己造词。
    return u.trim();
  }
  if (result.score <= 0 && u.indexOf('未计分') < 0) {
    if (u.length > 0) {
      return '未取得分数(metric ' + metricUnit(result.metric, u) + ')';
    }
    return '未取得分数(score=0)';
  }
  return '';
}

// 失败统计里显示用的短名: 去掉 unit 后缀, 只留负载名
export function statItemName(result: Gb7Result): string {
  const u: string = result.unit;
  const iFailed: number = u.indexOf('failed:');
  const iPartial: number = u.indexOf('partial:');
  let i: number = -1;
  if (iFailed >= 0 && iPartial >= 0) {
    i = iFailed < iPartial ? iFailed : iPartial;
  } else if (iFailed >= 0) {
    i = iFailed;
  } else {
    i = iPartial;
  }
  if (i < 0) {
    return result.name;
  }
  // 取 unit 的类型部分(跳过 "(" 之前的内容, 丢掉半个括号), 例如
  // "Mpx/s (partial: ...)" -> "Mpx/s"; 聚合统计里显示成 "Video Encoder Mpx/s"
  let head: string = u.slice(0, i).trim();
  const paren: number = head.indexOf('(');
  if (paren >= 0) {
    head = head.slice(0, paren).trim();
  }
  if (head.length === 0) {
    head = result.name;
  }
  return head === result.name ? result.name : (result.name + ' ' + head);
}

function emptyState(): CampaignState {
  return {
    target: 0, completed: 0, startedAt: Date.now(),
    failures: [], crashes: [], scores: [], durations: [],
    running: false, runId: '', roundOpen: false, lastRun: 0, lastItem: '', lastTs: 0,
    failStats: [], skipItems: [], skipStats: [],
    gateUsed: false, gateUnavailable: false,
    thermalFirst: -1, thermalLast: -1, interrupted: 0, interruptedRuns: [],
    roundItems: [], endedAt: 0
  };
}

// 兼容旧数据: 任何字段缺失都补默认值, 避免历史 JSON 结构不一致时崩在读取上
function normalize(s: CampaignState): CampaignState {
  s.target = (s.target as number) ?? 0;
  s.completed = (s.completed as number) ?? 0;
  s.startedAt = (s.startedAt as number) ?? 0;
  s.failures = (s.failures as FailItemDetail[]) ?? [];
  s.crashes = (s.crashes as CrashItem[]) ?? [];
  s.scores = (s.scores as number[]) ?? [];
  s.durations = (s.durations as number[]) ?? [];
  s.running = (s.running as boolean) ?? false;
  s.runId = (s.runId as string) ?? '';
  s.roundOpen = (s.roundOpen as boolean) ?? false;
  s.lastRun = (s.lastRun as number) ?? 0;
  s.lastItem = (s.lastItem as string) ?? '';
  s.lastTs = (s.lastTs as number) ?? 0;
  s.failStats = (s.failStats as FailItemStat[]) ?? [];
  s.skipItems = (s.skipItems as SkipItemDetail[]) ?? [];
  s.skipStats = (s.skipStats as SkipItemStat[]) ?? [];
  s.gateUsed = (s.gateUsed as boolean) ?? false;
  s.gateUnavailable = (s.gateUnavailable as boolean) ?? false;
  s.thermalFirst = (s.thermalFirst as number) ?? -1;
  s.thermalLast = (s.thermalLast as number) ?? -1;
  s.interrupted = (s.interrupted as number) ?? 0;
  s.interruptedRuns = (s.interruptedRuns as number[]) ?? [];
  s.roundItems = (s.roundItems as RoundStatus[]) ?? [];
  s.endedAt = (s.endedAt as number) ?? 0;
  return s;
}

export class StressSession {
  private static inst: StressSession | null = null;
  // 内存镜像: 避免每次 mark 都先读一次盘; 盘上始终是同一份内容
  private cur: CampaignState | null = null;

  static get(): StressSession {
    if (!StressSession.inst) {
      StressSession.inst = new StressSession();
    }
    return StressSession.inst;
  }

  // 读盘: 有会话就返回, 没有就 null(不会新建)
  async load(): Promise<CampaignState | null> {
    const raw: string = await BenchStore.get().getText(STRESS_SESSION_KEY, '');
    if (raw.length === 0) {
      this.cur = null;
      return null;
    }
    try {
      const s: CampaignState = normalize(JSON.parse(raw) as CampaignState);
      this.cur = s;
      return s;
    } catch (e) {
      console.warn('[AB] stress session parse failed');
      this.cur = null;
      return null;
    }
  }

  // 启动自检: 盘上 running = true 就说明上次压测没结束(进程被杀)
  async interrupted(): Promise<boolean> {
    const s: CampaignState | null = await this.load();
    return s !== null && s.running;
  }

  // 被崩溃打断的那一轮 = 已完整跑完的轮数 + 1
  interruptedRound(s: CampaignState): number {
    return s.completed + 1;
  }

  async save(s: CampaignState): Promise<void> {
    this.cur = s;
    await BenchStore.get().putText(STRESS_SESSION_KEY, JSON.stringify(s));
  }

  // 新开一轮压测: 清掉旧面包屑 + 生成新的跑分会话 id, 从第 1 轮开始
  async begin(target: number): Promise<CampaignState> {
    await CrashGuard.get().clear();
    const s: CampaignState = emptyState();
    s.target = target;
    s.running = true;
    s.runId = CrashGuard.get().currentRunId();
    await this.save(s);
    return s;
  }

  // 继续上次被打断的压测: 保留 completed / failures / crashes / scores, 只把 running 重新点亮。
  // 被中断的那一轮会整轮重跑, 这里把它记进 interruptedRuns, 报告里才说得清"为什么重跑了"。
  async resume(): Promise<CampaignState> {
    const s: CampaignState | null = await this.load();
    if (s === null) {
      return await this.begin(0);
    }
    return await this.resumeFrom(s);
  }

  async resumeFrom(s: CampaignState): Promise<CampaignState> {
    const run: number = s.completed + 1;
    if (run <= s.target && s.interruptedRuns.indexOf(run) < 0) {
      s.interruptedRuns.push(run);
      s.interrupted = s.interruptedRuns.length;
    }
    s.running = true;
    if (s.runId.length === 0) {
      s.runId = CrashGuard.get().currentRunId();
    }
    await this.save(s);
    return s;
  }

  // 崩溃恢复: 把"上次崩溃"记进压测会话(轮次 + 项名 + 时间 + 会话 id)
  async noteCrash(s: CampaignState, item: string, ts: number): Promise<CampaignState> {
    const run: number = s.completed + 1;
    for (const c of s.crashes) {
      if (c.run === run && c.item === item) {
        return s;   // 同一次崩溃只记一条(detect 可能被多次调用)
      }
    }
    s.crashes.unshift({ run: run, item: item, ts: ts, runId: s.runId });
    while (s.crashes.length > CRASH_MAX) {
      s.crashes.pop();
    }
    await this.save(s);
    return s;
  }

  // 每项开跑前调用: 把"第几轮 / 哪一项"flush 到盘。
  //
  // 写盘节流: 同一轮里最多写一次。原因是压测 1000 轮时, "每项都写一次会话状态"
  // 会把整份累计分数组重复落盘约 16000 次(纯属浪费 I/O), 而崩溃归因靠的是
  // CrashGuard 的面包屑(它本来就是每项都写、内容很小), 会话状态只需要"轮次级"精度。
  async mark(s: CampaignState, run: number, item: string): Promise<CampaignState> {
    if (s.lastRun === run && s.running) {
      s.lastItem = item;
      return s;
    }
    s.running = true;
    s.lastRun = run;
    s.lastItem = item;
    s.lastTs = Date.now();
    if (s.thermalFirst < 0) {
      s.thermalFirst = s.thermalLast;
    }
    await this.save(s);
    return s;
  }

  // 一轮"开跑"标记 + 立刻落盘。
  // 这一笔很关键: 第 N 轮跑完时会话盘上是 running=false/roundOpen=false, 只有重新开跑才置回 true。
  // 于是"在轮次之间被系统回收 / 用户划掉"不会被误判成压测崩溃(只有真跑在半路才算中断)。
  async markRoundStart(s: CampaignState, run: number, item: string): Promise<CampaignState> {
    s.running = true;
    s.roundOpen = true;
    s.lastRun = run;
    s.lastItem = item;
    s.lastTs = Date.now();
    if (s.thermalFirst < 0) {
      s.thermalFirst = s.thermalLast;
    }
    await this.save(s);
    return s;
  }

  // 一轮跑完(16 项全部走完, 不管有没有失败项, 只要没崩), 记分并落盘
  async recordRound(s: CampaignState, outcome: RoundOutcome): Promise<CampaignState> {
    if (!outcome.finished) {
      return s;
    }
    if (outcome.run !== s.completed + 1) {
      // 双保险: 只接受"当前该跑的那一轮"的结果, 防止重复计分
      console.warn('[AB] stress recordRound out of sequence: ' + outcome.run.toString() +
        ' vs ' + (s.completed + 1).toString());
      return s;
    }
    s.completed = outcome.run;
    s.scores.push(outcome.score);
    s.durations.push(outcome.elapsedMs);
    s.thermalLast = outcome.thermal;
    s.roundItems = [];
    for (const st of outcome.statuses) {
      if (s.roundItems.length >= ROUND_ITEM_MAX) {
        break;
      }
      s.roundItems.push(st);
    }
    for (const f of outcome.failures) {
      // 环形缓冲: 满了就丢最旧的, 保证报告里列的是"最近"的失败明细
      if (s.failures.length >= FAIL_MAX) {
        s.failures.shift();
      }
      s.failures.push(f);
      StressSession.bumpFail(s, f.item, f.run);
    }
    for (const k of outcome.skips) {
      if (s.skipItems.length >= SKIP_MAX) {
        s.skipItems.shift();
      }
      s.skipItems.push(k);
      StressSession.bumpSkip(s, k.item, k.run);
    }
    if (outcome.skipCount > 0) {
      s.gateUsed = true;
    }
    // 整轮结束: 关掉"轮次进行中"标志并落盘。此刻进程若被杀, 不该算成压测崩溃。
    s.roundOpen = false;
    await this.save(s);
    return s;
  }

  private static bumpFail(s: CampaignState, item: string, run: number): void {
    for (const st of s.failStats) {
      if (st.item === item) {
        st.count = st.count + 1;
        if (st.runs.length < 40) {
          st.runs.push(run);
        }
        return;
      }
    }
    s.failStats.push({ item: item, runs: [run], count: 1 });
  }

  private static bumpSkip(s: CampaignState, item: string, run: number): void {
    for (const st of s.skipStats) {
      if (st.item === item) {
        st.count = st.count + 1;
        if (st.runs.length < 40) {
          st.runs.push(run);
        }
        return;
      }
    }
    s.skipStats.push({ item: item, runs: [run], count: 1 });
  }

  // 结束(或中断后放弃)压测: 落盘最终报告 + 清掉进行中的会话状态
  async finish(s: CampaignState, endedAt: number): Promise<string> {
    s.running = false;
    s.endedAt = endedAt > 0 ? endedAt : Date.now();
    await this.save(s);
    const text: string = StressSession.buildText(s);
    await BenchStore.get().putText(STRESS_REPORT_KEY, text);
    await BenchStore.get().removeText(STRESS_SESSION_KEY);
    this.cur = null;
    return text;
  }

  // 上次结束后的报告文本(与进行中的会话分开存, 进程再被杀也还在)
  async storedReport(): Promise<string> {
    return await BenchStore.get().getText(STRESS_REPORT_KEY, '');
  }

  async clearReport(): Promise<void> {
    await BenchStore.get().removeText(STRESS_REPORT_KEY);
  }

  // 压测会话 -> 一条历史记录(BenchRecord)。
  // 压测期间不写历史, 只在结束时写这一条, 避免历史列表被 1000 轮刷爆。
  static sessionRecord(s: CampaignState, device: string): BenchRecord {
    const scores: number[] = s.scores;
    const geomeanAll: number = StressSession.geomean(scores);
    const last: number = scores.length > 0 ? scores[scores.length - 1] : 0;
    const failCount: number = s.failures.length;
    const crashCount: number = s.crashes.length;
    const record: BenchRecord = {
      time: s.endedAt > 0 ? s.endedAt : Date.now(),
      total: Math.round(geomeanAll),
      single: Math.round(last),
      multi: 0,
      gpu: 0,
      intSingle: s.completed,
      floatSingle: s.target,
      intMulti: crashCount,
      floatMulti: Math.round(StressSession.successRate(s) * 10),
      device: device + ' (稳定性压测 CS1 单核 ' + s.target.toString() + ' 轮：完成 ' +
        s.completed.toString() + ' · 成功 ' + StressSession.successRounds(s).toString() +
        ' · 失败项 ' + failCount.toString() + ' · 崩溃 ' + crashCount.toString() +
        (s.interrupted > 0 ? (' · 中断重跑 ' + s.interrupted.toString() + ' 轮') : '') + ')',
      tests: [],
      multiTests: [],
      gpuScenes: []
    };
    return record;
  }

  static geomean(values: number[]): number {
    let prod = 1;
    let n = 0;
    for (const v of values) {
      if (v > 0) {
        prod *= v;
        n++;
      }
    }
    return n > 0 ? Math.pow(prod, 1 / n) : 0;
  }

  // 第 run 轮(1-based)的综合分; 没有(被崩溃吞掉)时返回 0
  static scoreAt(s: CampaignState, run: number): number {
    const v: number | undefined = s.scores[run - 1];
    if (v === undefined || !(v > 0)) {
      return 0;
    }
    return v;
  }

  // 成功轮 = 该轮 16 项全部走完, 且没有任何失败项、也没崩过
  static successRounds(s: CampaignState): number {
    const bad: number[] = [];
    for (const f of s.failures) {
      if (bad.indexOf(f.run) < 0) {
        bad.push(f.run);
      }
    }
    const crashed: number[] = [];
    for (const c of s.crashes) {
      if (crashed.indexOf(c.run) < 0) {
        crashed.push(c.run);
      }
    }
    let ok = 0;
    for (let r = 1; r <= s.completed; r++) {
      if (bad.indexOf(r) < 0 && crashed.indexOf(r) < 0) {
        ok++;
      }
    }
    return ok;
  }

  static successRate(s: CampaignState): number {
    if (s.completed <= 0) {
      return 0;
    }
    return StressSession.successRounds(s) / s.completed;
  }

  // ---- 报告 ----
  static buildReport(s: CampaignState, device: string, nativeVer: string): CampaignReport {
    const successes: number[] = [];
    const bad: number[] = [];
    for (const f of s.failures) {
      if (bad.indexOf(f.run) < 0) {
        bad.push(f.run);
      }
    }
    const crashed: number[] = [];
    for (const c of s.crashes) {
      if (crashed.indexOf(c.run) < 0) {
        crashed.push(c.run);
      }
    }
    for (let r = 1; r <= s.completed; r++) {
      if (bad.indexOf(r) < 0 && crashed.indexOf(r) < 0) {
        const v: number = StressSession.scoreAt(s, r);
        if (v > 0) {
          successes.push(v);
        }
      }
    }
    let minScore: number = 0;
    let maxScore: number = 0;
    let sum: number = 0;
    for (const v of successes) {
      if (minScore <= 0 || v < minScore) {
        minScore = v;
      }
      if (v > maxScore) {
        maxScore = v;
      }
      sum += v;
    }
    let totalMs = 0;
    for (const d of s.durations) {
      totalMs += d;
    }
    const report: CampaignReport = {
      target: s.target,
      completed: s.completed,
      successRounds: StressSession.successRounds(s),
      successRate: StressSession.successRate(s),
      failStats: s.failStats,
      failures: s.failures,
      skipStats: s.skipStats,
      skipItems: s.skipItems,
      crashes: s.crashes,
      scores: s.scores,
      minScore: minScore,
      maxScore: maxScore,
      medianScore: median(successes),
      meanScore: successes.length > 0 ? (sum / successes.length) : 0,
      durations: s.durations,
      totalMs: totalMs,
      startedAt: s.startedAt,
      endedAt: s.endedAt,
      running: s.running,
      gateUsed: s.gateUsed,
      gateUnavailable: s.gateUnavailable,
      interrupted: s.interrupted,
      text: StressSession.buildText(s, device, nativeVer)
    };
    return report;
  }

  // 可复制报告文本。device / nativeVer 不传时留空(调用方一般都会传)。
  static buildText(s: CampaignState, device: string = '', nativeVer: string = ''): string {
    const successes: number[] = [];
    const bad: number[] = [];
    for (const f of s.failures) {
      if (bad.indexOf(f.run) < 0) {
        bad.push(f.run);
      }
    }
    const crashed: number[] = [];
    for (const c of s.crashes) {
      if (crashed.indexOf(c.run) < 0) {
        crashed.push(c.run);
      }
    }
    let okRounds = 0;
    const okScores: number[] = [];
    for (let r = 1; r <= s.completed; r++) {
      const isBad: boolean = bad.indexOf(r) >= 0 || crashed.indexOf(r) >= 0;
      if (!isBad) {
        okRounds++;
        const v: number = StressSession.scoreAt(s, r);
        if (v > 0) {
          okScores.push(v);
        }
      }
    }
    let minScore: number = 0;
    let maxScore: number = 0;
    let sum: number = 0;
    for (const v of okScores) {
      if (minScore <= 0 || v < minScore) {
        minScore = v;
      }
      if (v > maxScore) {
        maxScore = v;
      }
      sum += v;
    }
    let totalMs = 0;
    for (const d of s.durations) {
      totalMs += d;
    }

    let t = '[Crush分 · 稳定性压测报告]\n';
    t += '设备：' + (device.length > 0 ? device : '未知') + '\n';
    t += '版本：Crush分 4.0 · Native ' + nativeVer + '\n';
    t += '负载：CS1单核全套(16 项，1 线程)\n';
    t += '压测会话 id：' + s.runId + '\n';
    t += '起止：' + stamp(s.startedAt) + ' -> ' + stamp(s.endedAt) + '\n';
    t += '总耗时：' + fmtMs(totalMs) + '\n';
    t += '\n【一、轮次】\n';
    t += '目标轮数：' + s.target.toString() + '\n';
    t += '完成轮数：' + s.completed.toString() + '\n';
    if (s.interrupted > 0) {
      t += '被进程崩溃打断并整轮重跑的轮数：' + s.interrupted.toString() + '\n';
    }
    t += '成功轮数：' + okRounds.toString() + '（该轮全部项无失败、未崩溃）\n';
    const rate: number = s.completed > 0 ? (okRounds / s.completed) * 100 : 0;
    t += '成功率：' + (s.completed > 0 ? rate.toFixed(2) + '%' : '无已完成轮') + '\n';
    t += '崩溃次数：' + s.crashes.length.toString() + '\n';
    t += '状态：' + (s.running ? '仍在进行/被中断(未正常结束)' : '已结束') + '\n';

    t += '\n【二、分数（只统计成功轮，几何平均综合分）】\n';
    if (okScores.length > 0) {
      t += 'min / median / max：' + minScore.toFixed(1) + ' / ' + median(okScores).toFixed(1) +
        ' / ' + maxScore.toFixed(1) + '\n';
      t += '平均：' + (sum / okScores.length).toFixed(1) + '（样本 ' + okScores.length.toString() + ' 轮）\n';
      const first: number = okScores[0];
      const last: number = okScores[okScores.length - 1];
      t += '首轮：' + first.toFixed(1) + ' · 末轮：' + last.toFixed(1) + '\n';
      if (first > 0) {
        t += '末轮/首轮：' + ((last / first) * 100).toFixed(1) + '%（持续性能衰减参考）\n';
      }
    } else {
      t += '无成功轮，分数不可用\n';
    }
    if (okScores.length <= TEXT_SCORE_MAX) {
      t += '全部成功轮分数：' + StressSession.joinNumbers(okScores) + '\n';
    } else {
      t += '成功轮分数（末 ' + TEXT_SCORE_MAX.toString() + ' 个，共 ' + okScores.length.toString() +
        ' 个）：' + StressSession.joinNumbers(okScores.slice(okScores.length - TEXT_SCORE_MAX)) + '\n';
    }

    t += '\n【三、失败项统计（非崩溃，按项名聚合）】\n';
    if (s.failStats.length === 0) {
      t += '无\n';
    } else {
      for (const st of s.failStats) {
        t += '· ' + st.item + '：' + st.count.toString() + ' 次（轮次 ' +
          StressSession.joinNumbers(st.runs) + (st.count > st.runs.length ? ' …' : '') + '）\n';
      }
      t += '明细(环形保留最近 ' + s.failures.length.toString() + ' / 上限 ' + FAIL_MAX.toString() +
        ' 条；下面只列最近 ' + TEXT_DETAIL_MAX.toString() + ' 条)：\n';
      const failFrom: number = s.failures.length > TEXT_DETAIL_MAX ? s.failures.length - TEXT_DETAIL_MAX : 0;
      for (let i = failFrom; i < s.failures.length; i++) {
        const f: FailItemDetail = s.failures[i];
        t += '  第 ' + f.run.toString() + ' 轮 · ' + f.item + ' · ' + f.reason + '\n';
      }
    }

    t += '\n【四、崩溃记录（进程被系统/native 终止，ArkTS 捕获不到）】\n';
    if (s.crashes.length === 0) {
      t += '无\n';
    } else {
      for (const c of s.crashes) {
        t += '· 第 ' + c.run.toString() + ' 轮 · 最后启动的项「' + c.item + '」· ' +
          crumbWhen(c.ts) + ' · 会话 ' + c.runId + '\n';
      }
      t += '说明：最后启动的项不一定是肇事项，也可能是上一项遗留的大内存或驱动状态导致。\n';
    }
    if (s.roundItems.length > 0) {
      t += '最后一次启动的那一轮的逐项状态：\n';
      for (const st of s.roundItems) {
        const label: string = st.status === 'fail' ? '失败' : (st.status === 'skip' ? '跳过' : '完成');
        t += '  ' + st.item + ' = ' + label + '\n';
      }
    }

    t += '\n【五、内存闸门】\n';
    if (s.gateUnavailable) {
      t += '本次未做内存闸门：可用内存接口(@ohos.hidebug.getSystemMemInfo)不可用，已降级为不做闸门。\n';
    } else if (s.gateUsed) {
      t += '已启用逐项内存闸门：可用内存 < 该项预估峰值 + 512 MB 余量时跳过该项(不计分、不中断整轮)。\n';
    } else {
      t += '本会话未记录闸门动作(未发生跳过)。\n';
    }
    if (s.skipStats.length > 0) {
      t += '跳过项统计：\n';
      for (const st of s.skipStats) {
        t += '· ' + st.item + '：' + st.count.toString() + ' 次（轮次 ' +
          StressSession.joinNumbers(st.runs) + (st.count > st.runs.length ? ' …' : '') + '）\n';
      }
      t += '跳过明细(环形保留最近 ' + s.skipItems.length.toString() + ' / 上限 ' +
        SKIP_MAX.toString() + ' 条；下面只列最近 ' + TEXT_DETAIL_MAX.toString() + ' 条)：\n';
      const skipFrom: number = s.skipItems.length > TEXT_DETAIL_MAX ? s.skipItems.length - TEXT_DETAIL_MAX : 0;
      for (let i = skipFrom; i < s.skipItems.length; i++) {
        const k: SkipItemDetail = s.skipItems[i];
        t += '  第 ' + k.run.toString() + ' 轮 · ' + k.item + ' · ' + k.reason + '\n';
      }
    } else {
      t += '没有被跳过的项。\n';
    }

    t += '\n【六、逐轮耗时】\n';
    if (s.durations.length > 0) {
      let slowest = 0;
      let slowestRun = 0;
      let fastest = 0;
      let fastestRun = 0;
      for (let i = 0; i < s.durations.length; i++) {
        const d: number = s.durations[i];
        if (slowestRun === 0 || d > slowest) {
          slowest = d;
          slowestRun = i + 1;
        }
        if (fastestRun === 0 || d < fastest) {
          fastest = d;
          fastestRun = i + 1;
        }
      }
      t += '中位轮耗时：' + fmtMs(median(s.durations)) + '\n';
      t += '最慢：第 ' + slowestRun.toString() + ' 轮 ' + fmtMs(slowest) + '\n';
      t += '最快：第 ' + fastestRun.toString() + ' 轮 ' + fmtMs(fastest) + '\n';
    }

    t += '\n【七、热状态采样】\n';
    t += '首轮：' + s.thermalFirst.toString() + ' · 末轮：' + s.thermalLast.toString() +
      '（0 凉爽 / 1 正常 / 2 偏热 / 3 过热 / 4 高温降级 / 5 警告 / 6 紧急）\n';

    t += '\n【八、口径与限制】\n';
    t += '一、压测只重复调用现有的 CS1 单核实现，负载顺序、参数、分数公式一行未改；\n';
    t += '二、综合分 = 已计分项单项分的几何平均(与首页 CS1 单核卡片同一口径)；\n';
    t += '三、崩溃项直接杀掉进程，这一轮没有分数，因此分数统计只用成功轮；\n';
    t += '四、目标轮数与成功率的分母是"已完成轮数"，中断在最后一轮时该轮不计入分母；\n';
    t += '五、压测期间不写历史记录，只在会话结束时写入一条汇总历史。\n';
    return t;
  }

  static joinNumbers(values: number[]): string {
    if (values.length === 0) {
      return '--';
    }
    let s = '';
    for (const v of values) {
      if (s.length > 0) {
        s += ', ';
      }
      s += (Math.round(v * 10) / 10).toString();
    }
    return s;
  }
}
