import { common } from '@kit.AbilityKit';
import { BenchStore, CRUMB_KEY, CRUMB_LOG_KEY } from './BenchStore';
import { RunLog } from './RunLog';

// ---------------------------------------------------------------------------
// 崩溃面包屑 + 崩溃自诊断
//
// 原理: native 层 SIGSEGV / OOM 会直接杀进程, JS 侧收不到任何回调, 也写不了日志。
// 所以唯一的办法是"事前写盘": 每开始跑一个负载项之前, 先把
// 「阶段 + 项名 + 序号 + 时间戳」flush 到 preferences; 整轮成功返回后清除。
// 下次启动若发现面包屑还在, 就说明上一次进程是异常结束的。
//
// 注意: 最后写下的那一项不一定是肇事者(可能是它上一项留下的大内存 / 驱动状态),
// 因此 UI 上的措辞统一是"最后启动的项", 不说"崩溃的项"。
// ---------------------------------------------------------------------------

export interface BenchCrumb {
  runId: string;
  phase: string;
  item: string;
  index: number;
  total: number;
  ts: number;
  reported: boolean;
  // 该项"开跑前"由 native 取证装置采下的那口内存足迹(BenchRunner 侧 sampleMemory() +
  // lastMemorySample()), 形如 "rss=1234kB hwm=5678kB size=90123kB threads=9 sampled=...Z"。
  // 进程被系统 SIGKILL 时不会留下任何崩溃记录, 这一行就是盘上唯一的"临死内存足迹"。
  // 空串 = 没采到(native 未就绪 / 旧版本写下的面包屑没有这个字段)。
  mem: string;
}

export interface CrashHit {
  phase: string;
  item: string;
  index: number;
  total: number;
  ts: number;
  // 形如 "CS1 单核 · Photo Library"
  where: string;
  // 形如 "06-01 10:42:03"
  when: string;
  // 一行摘要
  line: string;
  // 完整可复制文本(含分割线, 用户可直接截图/复制发回)
  text: string;
  // 该项开跑前采到的 native 内存足迹(可能为空串)
  mem: string;
}

const LOG_MAX = 5;
// preferences 的单个字符串值上限是 8192 字符(自诊断历史是"一个 JSON 数组"存成一个值),
// 所以序列化之后超过这个长度就丢掉最旧的一条, 保证最新的一条一定存得下。
const LOG_JSON_MAX = 7600;

function two(n: number): string {
  return n < 10 ? '0' + n.toString() : n.toString();
}

export function crumbWhen(ts: number): string {
  const d = new Date(ts);
  return two(d.getMonth() + 1) + '-' + two(d.getDate()) + ' ' +
    two(d.getHours()) + ':' + two(d.getMinutes()) + ':' + two(d.getSeconds());
}

export function crumbWhere(c: BenchCrumb): string {
  return c.phase + ' · ' + c.item;
}

export function crumbLine(c: BenchCrumb): string {
  return '最后启动的项：「' + crumbWhere(c) + '」（第 ' + c.index.toString() + '/' +
    c.total.toString() + ' 项）· ' + crumbWhen(c.ts);
}

export function crumbText(c: BenchCrumb): string {
  let s = '[Crush分 · 崩溃自诊断]\n';
  s += '状态：上次运行未正常结束（进程被系统或 native 层终止）\n';
  s += '最后启动的项：' + crumbWhere(c) + '\n';
  s += '该项序号：第 ' + c.index.toString() + ' / ' + c.total.toString() + ' 项\n';
  s += '最后写入时间：' + crumbWhen(c.ts) + '\n';
  s += '运行 ID：' + c.runId + '\n';
  const mem: string = crumbMem(c);
  if (mem.length > 0) {
    s += '该项开跑前内存足迹：' + mem + '\n';
    s += '（这一行是崩溃前最后写进盘的内存足迹：即使进程被系统 SIGKILL、native 侧一无所获，它也能说明当时离内存上限还有多远）\n';
  }
  s += '说明：最后启动的项不一定是崩溃项，也可能是上一项遗留的大内存或驱动状态导致。\n';
  return s;
}

// 兼容早期版本写下的面包屑(没有 mem 字段): 统一读成空串, 不抛异常。
export function crumbMem(c: BenchCrumb): string {
  const m: string | undefined = c.mem;
  if (m === undefined) {
    return '';
  }
  return m;
}

export class CrashGuard {
  private static inst: CrashGuard | null = null;
  private ready: Promise<void> | null = null;
  // 一次"跑分会话"的 id: 由第一次 mark 生成, clear 后失效(下次 mark 重新生成)
  private runId: string = '';

  // ---- native 取证装置(crash_guard.cpp)的状态与现场 ----
  // 说明: 原始 napi 调用只发生在 import 了 libaurorabench.so 的文件里
  // (BenchRunner 负责 onCreate 的初始化, Index 负责启动自检与自检卡),
  // 这里只负责"组织": 装置状态 / 启动时读回的现场 / 写进 runlog 的一行摘要。
  private guardInited: boolean = false;
  private guardDir: string = '';
  private guardDirOk: boolean = false;
  private guardReady: boolean = false;
  private guardPath: string = '';
  private nativeText: string = '';
  private nativePath: string = '';

  static get(): CrashGuard {
    if (!CrashGuard.inst) {
      CrashGuard.inst = new CrashGuard();
    }
    return CrashGuard.inst;
  }

  // ---- native 取证装置: 状态 ----

  // EntryAbility.onCreate 拿到 filesDir 后立刻调 setLogDir, 结果由这里记下来, 供 UI 显示。
  setGuardState(dir: string, dirOk: boolean, ready: boolean, path: string): void {
    this.guardDir = dir;
    this.guardDirOk = dirOk;
    this.guardReady = ready;
    this.guardPath = path;
    this.guardInited = true;
  }

  guardInitedOnce(): boolean {
    return this.guardInited;
  }

  guardIsReady(): boolean {
    return this.guardDirOk && this.guardReady;
  }

  guardLogPath(): string {
    if (this.guardPath.length > 0) {
      return this.guardPath;
    }
    if (this.guardDir.length > 0) {
      return this.guardDir + '/native_crash.txt';
    }
    return '（未设置：setLogDir 没有成功）';
  }

  guardStatusText(): string {
    if (this.guardIsReady()) {
      return '已就绪（setLogDir 成功 · 信号处理器已武装）';
    }
    if (!this.guardInited) {
      return '未初始化（onCreate 里还没拿到 filesDir）';
    }
    if (!this.guardDirOk) {
      return '未就绪（setLogDir 返回 false：证据文件打不开）';
    }
    return '未就绪（信号处理器未武装：nativeCrashGuardReady() = false）';
  }

  // '' = 装置正常; 非空 = 崩溃横幅里必须显示的那一行(不静默)。
  guardNotReadyLine(): string {
    if (this.guardIsReady()) {
      return '';
    }
    return 'native 取证装置未就绪：' + this.guardStatusText() +
      '。这次若真是 native 崩溃，盘上不会有 native 现场，只能以 ArkTS 面包屑为准。';
  }

  // ---- native 取证装置: 启动时读回的现场 ----

  setNativeEvidence(text: string, path: string): void {
    this.nativeText = text;
    this.nativePath = path;
  }

  nativeEvidence(): string {
    return this.nativeText;
  }

  nativeEvidencePath(): string {
    if (this.nativePath.length > 0) {
      return this.nativePath;
    }
    return this.guardLogPath();
  }

  // 真崩溃(native 处理器写下的记录头) vs 自检记录(crashSelfTest(0))
  nativeLooksLikeCrash(): boolean {
    return this.nativeText.indexOf('AURORA NATIVE CRASH') >= 0;
  }

  // 取记录里的某一行("signal : ..." / "module : ..." / "item   : ...")
  private reportLine(text: string, key: string): string {
    const lines: string[] = text.split('\n');
    for (const l of lines) {
      if (l.startsWith(key)) {
        return l.substring(key.length).trim();
      }
    }
    return '';
  }

  // 一行摘要: 信号 + 模块归属(pc 是否落在我们自己的 .so 里) + 当时标记的负载项。
  // 这一行进 hilog 与 runlog 的 crash 行, 用于和面包屑互相印证。
  nativeEvidenceOneLine(): string {
    if (this.nativeText.length === 0) {
      return '';
    }
    const parts: string[] = [];
    const sig: string = this.reportLine(this.nativeText, 'signal :');
    if (sig.length > 0) {
      parts.push('信号 ' + sig);
    }
    const mod: string = this.reportLine(this.nativeText, 'module :');
    if (mod.length > 0) {
      parts.push(mod);
    }
    const item: string = this.reportLine(this.nativeText, 'item   :');
    if (item.length > 0) {
      parts.push('当时标记 ' + item);
    }
    const mem: string = this.reportLine(this.nativeText, 'memory :');
    if (mem.length > 0) {
      parts.push(mem);
    }
    return parts.join(' | ');
  }

  // 现场读回后会立刻清盘(避免下次启动重复报同一条), 所以先把摘要落进「自诊断历史」
  // (preferences, 可长期保留), 用户即使当场没截图, 之后也能复制回来。
  nativePersistText(): string {
    if (this.nativeText.length === 0) {
      return '';
    }
    // 只留尾部这段(记录最关键的部分在末尾的 memory / at_item 行);
    // 上限压到 600 字符是为了和「自诊断历史」的其它条目一起塞进 preferences 的单值上限里。
    const cap: number = 600;
    let body: string = this.nativeText;
    if (body.length > cap) {
      body = body.substring(body.length - cap);
    }
    let s: string = '[Crush分 · native 崩溃现场（来自取证装置）]\n';
    s += '证据文件：' + this.nativeEvidencePath() + '\n';
    s += '摘要：' + this.nativeEvidenceOneLine() + '\n';
    s += '（磁盘上的记录已在本次启动读取后清除，避免每次启动重复报同一条）\n';
    s += '----------------------------------------\n';
    s += body;
    return s;
  }

  // native 有记录、但 ArkTS 面包屑不在(崩在两项之间的胶水代码里, 或面包屑收尾时已被清):
  // 也给 runlog 补一条 crash 行, 便于用 hdc 直接把日志拉下来归因。
  logNativeOnly(): void {
    if (this.nativeText.length === 0) {
      return;
    }
    const item: string = this.reportLine(this.nativeText, 'item   :');
    RunLog.get().crash('', 'native', item.length > 0 ? item : '（native 记录未带项名）', 0, 0, 0,
      '上次运行未正常结束：只有 native 取证记录，没有 ArkTS 面包屑（可能崩在两项之间的胶水代码里，或面包屑已被清除）；' +
      this.nativeEvidenceOneLine());
  }

  // 与 BenchStore 共用同一个 preferences(name = aurora_bench)
  init(ctx: common.Context): Promise<void> {
    if (!this.ready) {
      this.ready = BenchStore.get().init(ctx);
    }
    return this.ready;
  }

  private async ensure(): Promise<boolean> {
    if (this.ready) {
      await this.ready;
    }
    return BenchStore.get().isReady();
  }

  // 只读当前跑分会话 id: 没有就返回空串, 不新建。
  // 稳定性压测用它与面包屑的 runId 对齐, 判断"这次崩溃到底属不属于压测会话"。
  runIdIfAny(): string {
    return this.runId;
  }

  // 当前跑分会话的 id。稳定性压测在开跑前把它记进压测会话状态,
  // 这样即使进程被杀, 下次启动也能把"崩溃面包屑"和"压测会话"对上号。
  currentRunId(): string {
    if (this.runId.length === 0) {
      this.runId = Date.now().toString() + '-' + Math.floor(Math.random() * 100000).toString();
    }
    return this.runId;
  }

  // 只读面包屑, 不写盘、不入历史、不改 reported 标志。
  // 稳定性压测/首页刷新用它做归因, 不会产生副作用。
  async peek(): Promise<BenchCrumb | null> {
    try {
      const ok: boolean = await this.ensure();
      if (!ok) {
        return null;
      }
      const raw: string = await BenchStore.get().getText(CRUMB_KEY, '');
      if (raw.length === 0) {
        return null;
      }
      return JSON.parse(raw) as BenchCrumb;
    } catch (e) {
      console.warn('[AB] crumb peek failed');
      return null;
    }
  }

  // 每次开始跑"任一"负载项之前调用。内部先 flush 再返回, 保证崩溃时盘上已有记录。
  // mem = 调用点刚采的 native 内存足迹(BenchRunner 侧 sampleMemory() + lastMemorySample()):
  //       一并写进面包屑, 并同步给 RunLog 作为逐项行 note 的后缀。
  //       被系统 SIGKILL 时盘上不会有崩溃记录, 这一口内存就是唯一能带出来的临死证据。
  async mark(phase: string, item: string, index: number, total: number, mem: string = ''): Promise<void> {
    try {
      const ok: boolean = await this.ensure();
      if (!ok) {
        console.warn('[AB] crumb skipped: store not ready');
        return;
      }
      if (this.runId.length === 0) {
        this.runId = Date.now().toString() + '-' + Math.floor(Math.random() * 100000).toString();
      }
      // 运行日志(runlog.jsonl)与本会话共用同一个 runId:
      // 面包屑整轮结束后会被 clear, 而日志的"整轮汇总行"是在 clear 之后才写的,
      // 所以把 id 同步给 RunLog 并留在 RunLog 里, 保证逐项行与汇总行落在同一个 runId 下。
      RunLog.get().setRunId(this.runId);
      RunLog.get().setMemNote(mem);
      const c: BenchCrumb = {
        runId: this.runId,
        phase: phase,
        item: item,
        index: index,
        total: total,
        ts: Date.now(),
        reported: false,
        mem: mem
      };
      await BenchStore.get().putText(CRUMB_KEY, JSON.stringify(c));
    } catch (e) {
      console.warn('[AB] crumb write failed');
    }
  }

  // 整轮跑完并成功返回(或 JS 侧捕获到异常)后清除面包屑。
  // native 崩溃时进程直接消失, 这里不会被执行 —— 面包屑因此得以保留。
  async clear(): Promise<void> {
    try {
      const ok: boolean = await this.ensure();
      if (!ok) {
        return;
      }
      await BenchStore.get().removeText(CRUMB_KEY);
      this.runId = '';
    } catch (e) {
      console.warn('[AB] crumb clear failed');
    }
  }

  // 启动自检: 面包屑还在 => 上次没跑完就异常结束
  async detect(): Promise<CrashHit | null> {
    try {
      const ok: boolean = await this.ensure();
      if (!ok) {
        return null;
      }
      const raw: string = await BenchStore.get().getText(CRUMB_KEY, '');
      if (raw.length === 0) {
        return null;
      }
      const c: BenchCrumb = JSON.parse(raw) as BenchCrumb;
      // 运行日志: 每次启动只要还看得到面包屑, 就补一条 kind='crash' 的记录
      // (含最后启动的项 / 该次会话的 runId / 最后写入时间), 便于直接用 hdc 拉下来归因。
      // note 里再串上两条互相印证的临死证据: ArkTS 侧的内存足迹 + native 现场摘要。
      let crashNote: string = '上次运行未正常结束（进程被系统或 native 层终止）；最后启动的项不一定是肇事项';
      const crumbMemText: string = crumbMem(c);
      if (crumbMemText.length > 0) {
        crashNote += '；该项开跑前内存足迹 ' + crumbMemText;
      }
      const nativeOneLine: string = this.nativeEvidenceOneLine();
      if (nativeOneLine.length > 0) {
        crashNote += '；native 现场 ' + nativeOneLine;
      } else if (this.guardIsReady()) {
        crashNote += '；native 侧无现场记录（不是可捕获的致命信号：更可能是被系统 SIGKILL / 后台管制）';
      }
      RunLog.get().crash(c.runId, c.phase, c.item, c.index, c.total, c.ts, crashNote);
      if (!c.reported) {
        // 只在第一次发现时入历史, 避免每次启动都重复记一条
        c.reported = true;
        await BenchStore.get().putText(CRUMB_KEY, JSON.stringify(c));
        await this.pushLog(crumbText(c));
      }
      const hit: CrashHit = {
        phase: c.phase,
        item: c.item,
        index: c.index,
        total: c.total,
        ts: c.ts,
        where: crumbWhere(c),
        when: crumbWhen(c.ts),
        line: crumbLine(c),
        text: crumbText(c),
        mem: crumbMem(c)
      };
      return hit;
    } catch (e) {
      console.warn('[AB] crumb detect failed');
      return null;
    }
  }

  async pushLog(text: string): Promise<void> {
    const raw: string = await BenchStore.get().getText(CRUMB_LOG_KEY, '');
    let lines: string[] = [];
    if (raw.length > 0) {
      try {
        lines = JSON.parse(raw) as string[];
      } catch (e) {
        lines = [];
      }
    }
    lines.unshift(text);
    while (lines.length > LOG_MAX) {
      lines.pop();
    }
    let json: string = JSON.stringify(lines);
    while (lines.length > 1 && json.length > LOG_JSON_MAX) {
      lines.pop();
      json = JSON.stringify(lines);
    }
    await BenchStore.get().putText(CRUMB_LOG_KEY, json);
  }

  async log(): Promise<string[]> {
    const ok: boolean = await this.ensure();
    if (!ok) {
      return [];
    }
    const raw: string = await BenchStore.get().getText(CRUMB_LOG_KEY, '');
    if (raw.length === 0) {
      return [];
    }
    try {
      return JSON.parse(raw) as string[];
    } catch (e) {
      return [];
    }
  }

  async clearLog(): Promise<void> {
    try {
      const ok: boolean = await this.ensure();
      if (!ok) {
        return;
      }
      await BenchStore.get().removeText(CRUMB_LOG_KEY);
    } catch (e) {
      console.warn('[AB] crash log clear failed');
    }
  }
}
