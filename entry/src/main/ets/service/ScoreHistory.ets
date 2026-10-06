// ---------------------------------------------------------------------------
// 历史成绩.txt —— 人能直接看、能直接打开的那份成绩单
//
// 为什么要有它(与已有的两份记录分清楚, 三份各司其职, 谁都不替代谁):
//   runlog.jsonl     逐项诊断日志, 沙箱内, 512 KB 就轮转 —— 排障用;
//   runhistory.jsonl 永久账本, 沙箱内, 每轮一个 JSON 行 —— 机器读用(对比工具 / 上传);
//   历史成绩.txt      这一份: **纯文本、一行一次跑分、只放最直观的分数**, 落在与报告
//                     同一个「下载」目录里 —— 用户不进 App、不接电脑, 打开文件管理就能看。
//
// 落盘位置: 与报告完全同一处 —— ReportSink 解析出来的 <下载>/<包名>/(文件管理器里
//   显示为「下载/Crush分」)。目录解析、UTF-8 硬断言、失败原因回传全部复用 ReportSink,
//   不另写一套目录逻辑(两处各写一套迟早各说各话)。
//   手机上第一次可能还没拿到那个目录的写权限 —— 那时如实记下原因, 报告落盘之后再补试一次
//   (见 pages/Index.ets 的 startFullRun), 并且同时在沙箱里留一份兜底副本, 绝不静默丢。
//
// 写法: **追加**。文件不存在(或长度为 0)时连表头与口径一起写; 已有内容只追加一行。
//   **不做"只留最近 N 次"的裁剪** —— 历次记录全部保留。
//   去重: 同一次跑分只写一行。进程内记住上次写过的 runId, 并把 runId 落进沙箱里的
//   小状态文件(scorehistory.state, 重启后依然有效); 另外再用"这一行的起始时间是否
//   已经在文件里"兜一道。
//
// 时机: 整轮跑完之后调用一次。**不在任何计时区间里**, 也不参与任何分数 —— 它只读结果。
// ---------------------------------------------------------------------------
import fs from '@ohos.file.fs';
import { common } from '@kit.AbilityKit';
import { ReportSink, ReportWriteResult } from './ReportSink';
import { assertTxtUtf8Safe, sanitizeReportText } from '../common/Utf8Guard';

// 文件名(与报告 report-YYYYMMDD-HHMMSS.txt 一起躺在同一个目录里)
export const SCORE_HISTORY_FILE: string = '历史成绩.txt';
// 沙箱内的去重状态(不是给用户看的; 只记最近一次写进去的那一行)
const STATE_FILE: string = 'scorehistory.state';

// 表头(一行)与口径(一句)。
// 表头不写成死字符串, 而是用与数据行**同一套**宽度算法拼出来(见 headText())——
// 加一列、改一个标题就不会再出现"表头与数据对不上"。
const HEAD_LABELS: string[] = ['时间', '版本', '设备', 'CS1 单核', 'CS1 多核', 'CS1 GPU',
  '自研 单核', '自研 多核', '自研 GPU', '总分', '耗时', '备注'];
// 每一列的宽度(显示宽度, 中文算 2): 与 ScoreHistory.rowLine 里那一行逐列对应
const HEAD_WIDTHS: number[] = [19, 4, 30, 8, 8, 8, 9, 9, 9, 6, 8, 4];
// 哪些列右对齐(数字列); 其余左对齐
const HEAD_RIGHT: boolean[] = [false, false, false, true, true, true, true, true, true, true, true, false];
// 口径只说三件事: 哪一列能跨设备比 / 哪一列跨版本不能比 / 其余只能同机比。
// ★「CS1 多核」那半句是这一版新加的: 这张表里同时存着 8.0 / 8.1 / 8.2 三套口径的多核值
// (参与项集合不同), 不写这句用户会直接拿它们比。**不重算老数据、不删老行**。
const CALIB_LINE: string =
  '口径：只有「CS1 单核」适合跨设备比；CS1 多核那一列在 8.2 之前口径不同（参与项集合不一样），不要跨版本比多核；CS1 GPU 与自研套件那几列只适合同一台机器的前后对比。';

export interface ScoreHistoryRow {
  runId: string;
  ts: number;          // 本轮开始时间(与报告同名同源)
  device: string;
  appVer: string;      // 写这一行的 App 版本(例 'Crush分 8.3'; 表里只显版本号)
  gb8Single: number;   // 0 = 本轮没拿到这一项
  gb8Multi: number;
  gb8Gpu: number;
  ownSingle: number;
  ownMulti: number;
  ownGpu: number;
  ownTotal: number;
  durationMs: number;
  note: string;        // 只放必要的异常: 崩溃 / 有项失败 / 中途停止; 一切正常留空
}

export interface ScoreHistoryResult {
  ok: boolean;         // true = 已经写进「下载」目录
  duplicate: boolean;  // true = 判定为同一次跑分, 按幂等跳过(没有写)
  path: string;
  method: string;
  sandboxPath: string; // 兜底副本(写不进「下载」目录时才用; '' = 没写)
  err: string;
}

interface ScoreHistoryState {
  runId: string;
  ts: number;
  lines: number;
}

function two(n: number): string {
  return n < 10 ? ('0' + n.toString()) : n.toString();
}

// 与 runlog.jsonl 的 stamp() 同一套写法, 一眼能对上是哪一轮
function stamp(ts: number): string {
  const d = new Date(ts);
  return d.getFullYear().toString() + '-' + two(d.getMonth() + 1) + '-' + two(d.getDate()) + ' ' +
    two(d.getHours()) + ':' + two(d.getMinutes()) + ':' + two(d.getSeconds());
}

// 中日韩全角字符在等宽终端 / 记事本里占两格; 不按它对齐, 列就会歪。
function wide(c: number): boolean {
  return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
    (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
    (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
    (c >= 0xFFE0 && c <= 0xFFE6);
}

// n 个空格(不用 String.repeat: 这里只依赖最基础的写法)
function sp(n: number): string {
  let s: string = '';
  for (let i = 0; i < n; i++) {
    s += ' ';
  }
  return s;
}

function dispWidth(s: string): number {
  let w: number = 0;
  for (let i = 0; i < s.length; i++) {
    w += wide(s.charCodeAt(i)) ? 2 : 1;
  }
  return w;
}

// 按显示宽度左侧对齐补齐(超宽就按显示宽度截断, 不切出半个字符)
function padEndW(s: string, width: number): string {
  if (dispWidth(s) > width) {
    let w: number = 0;
    let i: number = 0;
    while (i < s.length) {
      const cw: number = wide(s.charCodeAt(i)) ? 2 : 1;
      if (w + cw > width - 1) {
        break;
      }
      w += cw;
      i++;
    }
    s = s.substring(0, i) + '…';
    return s + sp(Math.max(0, width - dispWidth(s)));
  }
  return s + sp(width - dispWidth(s));
}

// 按显示宽度右侧对齐补齐(数字列)
function padStartW(s: string, width: number): string {
  const w: number = dispWidth(s);
  return w >= width ? s : (sp(width - w) + s);
}

// 'Crush分 8.3' -> '8.3'(表里只要版本号;拿不到就写 '--')
function shVer(s: string): string {
  const at: number = s.lastIndexOf(' ');
  const v: string = at >= 0 ? s.substring(at + 1) : s;
  return v.length > 0 ? v : '--';
}

function f1(v: number): string {
  return v > 0 ? v.toFixed(1) : '--';
}

function f0(v: number): string {
  return v > 0 ? Math.round(v).toString() : '--';
}

export class ScoreHistory {
  private static inst: ScoreHistory | null = null;
  private dir: string = '';
  private lastRunId: string = '';
  private lastNote: string = '';

  static get(): ScoreHistory {
    if (!ScoreHistory.inst) {
      ScoreHistory.inst = new ScoreHistory();
    }
    return ScoreHistory.inst;
  }

  // filesDir(沙箱)只用来放去重状态与兜底副本; 用户看到的那一份在「下载」目录里。
  init(ctx: common.Context): void {
    if (this.dir.length === 0) {
      this.dir = ctx.filesDir;
    }
  }

  isReady(): boolean {
    return this.dir.length > 0;
  }

  // 一行数据(列顺序固定: 与表头逐列对应)
  static rowLine(r: ScoreHistoryRow): string {
    const dur: string = r.durationMs > 0 ? ((r.durationMs / 1000).toFixed(1) + ' s') : '--';
    return stamp(r.ts) + ' | ' + padStartW(shVer(r.appVer), 4) + ' | ' + padEndW(r.device, 30) +
      ' | ' + padStartW(f1(r.gb8Single), 8) +
      ' | ' + padStartW(f1(r.gb8Multi), 8) +
      ' | ' + padStartW(f1(r.gb8Gpu), 8) +
      ' | ' + padStartW(f0(r.ownSingle), 9) +
      ' | ' + padStartW(f0(r.ownMulti), 9) +
      ' | ' + padStartW(f0(r.ownGpu), 9) +
      ' | ' + padStartW(f0(r.ownTotal), 6) +
      ' | ' + padStartW(dur, 8) +
      ' | ' + r.note + '\n';
  }

  // 表头: 用与数据行同一套宽度/对齐算法拼出来, 因此加列改列都不会与数据对不上。
  static headText(): string {
    let s: string = '';
    for (let i = 0; i < HEAD_LABELS.length; i++) {
      const cell: string = HEAD_RIGHT[i]
        ? padStartW(HEAD_LABELS[i], HEAD_WIDTHS[i])
        : padEndW(HEAD_LABELS[i], HEAD_WIDTHS[i]);
      s += (i === 0 ? '' : ' | ') + cell;
    }
    return s + '\n' + CALIB_LINE + '\n';
  }

  // 一句人话(报告落盘回执与界面共用同一句, 不各写各的)
  static describe(res: ScoreHistoryResult): string {
    if (res.duplicate) {
      return '本轮已记录过，按同一次跑分跳过（不重复写）。';
    }
    if (res.ok) {
      return '已写入：' + res.path;
    }
    let s: string = '未写入：' + (res.err.length > 0 ? res.err : '原因未取到');
    if (res.sandboxPath.length > 0) {
      s += '（沙箱内留了兜底副本：' + res.sandboxPath + '）';
    }
    return s;
  }

  private statePath(): string {
    return this.dir + '/' + STATE_FILE;
  }

  private readState(): ScoreHistoryState {
    const st: ScoreHistoryState = { runId: '', ts: 0, lines: 0 };
    if (this.dir.length === 0) {
      return st;
    }
    try {
      const p: string = this.statePath();
      if (!fs.accessSync(p)) {
        return st;
      }
      const text: string = fs.readTextSync(p);
      if (text.length === 0) {
        return st;
      }
      const o = JSON.parse(text) as ScoreHistoryState;
      st.runId = o.runId !== undefined ? o.runId : '';
      st.ts = o.ts !== undefined ? o.ts : 0;
      st.lines = o.lines !== undefined ? o.lines : 0;
    } catch (e) {
      // 状态文件坏了最多是"可能重复写一行", 不影响跑分: 当作没有状态
    }
    return st;
  }

  private writeState(s: ScoreHistoryState): void {
    if (this.dir.length === 0) {
      return;
    }
    try {
      const f = fs.openSync(this.statePath(),
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      try {
        fs.writeSync(f.fd, JSON.stringify(s));
      } finally {
        fs.closeSync(f);
      }
    } catch (e) {
      console.warn('[AB] scorehistory state write failed');
    }
  }

  // 写沙箱兜底副本(与「下载」目录那一份同一个格式、同样追加)
  private appendSandbox(head: string, line: string): string {
    if (this.dir.length === 0) {
      return '';
    }
    const p: string = this.dir + '/' + SCORE_HISTORY_FILE;
    let fresh: boolean = true;
    try {
      if (fs.accessSync(p)) {
        fresh = fs.statSync(p).size === 0;
      }
    } catch (e) {
      fresh = true;
    }
    let text: string = fresh ? (head + line) : line;
    if (assertTxtUtf8Safe(text).length > 0) {
      text = sanitizeReportText(text);
    }
    try {
      const f = fs.openSync(p, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
      try {
        fs.writeSync(f.fd, text);
      } finally {
        fs.closeSync(f);
      }
      return p;
    } catch (e) {
      return '';
    }
  }

  // 追加一行到 <下载>/<包名>/历史成绩.txt。
  // 幂等: 同一次跑分(同一个 runId, 或同一行时间已经文件里)只写一行。
  append(r: ScoreHistoryRow): ScoreHistoryResult {
    const res: ScoreHistoryResult = {
      ok: false, duplicate: false, path: '', method: '未写入', sandboxPath: '', err: ''
    };
    // ① 幂等(进程内)
    if (r.runId.length > 0 && r.runId === this.lastRunId) {
      res.duplicate = true;
      return res;
    }
    // ② 幂等(跨进程: 上次写到哪一行)
    const st: ScoreHistoryState = this.readState();
    if (r.runId.length > 0 && st.runId === r.runId) {
      this.lastRunId = r.runId;
      res.duplicate = true;
      return res;
    }
    const line: string = ScoreHistory.rowLine(r);
    // ③ 幂等(兜底: 这一行的起始时间已经在文件里了)
    const timeText: string = stamp(r.ts);
    const tail: string = ReportSink.tailOf(SCORE_HISTORY_FILE, 8000);
    if (tail.indexOf(timeText + ' | ') >= 0) {
      this.lastRunId = r.runId;
      res.duplicate = true;
      return res;
    }
    const head: string = ScoreHistory.headText();
    const w: ReportWriteResult = ReportSink.appendText(SCORE_HISTORY_FILE, head, line);
    res.path = w.path;
    res.method = w.method;
    res.ok = w.ok;
    res.err = w.err;
    if (!res.ok) {
      // 写不进「下载」目录不静默: 沙箱留一份兜底副本, 原因原样带回去(报告与界面都会印)
      res.sandboxPath = this.appendSandbox(head, line);
      this.lastNote = res.err;
      return res;
    }
    this.lastRunId = r.runId;
    this.writeState({ runId: r.runId, ts: r.ts, lines: st.lines + 1 });
    return res;
  }

  // 最近一次失败原因(界面兜底显示用)
  lastError(): string {
    return this.lastNote;
  }
}
