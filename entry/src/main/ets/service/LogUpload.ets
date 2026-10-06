import http from '@ohos.net.http';
import { common } from '@kit.AbilityKit';
import { BenchStore } from './BenchStore';

// ---------------------------------------------------------------------------
// 可选: 把跑分日志 POST 到用户自己填的地址(默认关闭)
//
//  · 只有一种形态: 通用 JSON POST, body 就是 RunLog.buildPayload() 生成的 JSON 文本
//      { schema, uploadedAt, device, appVersion, nativeVersion, range, fileBytes, fileLines,
//        timeRange, lastRunId, lastRunCount, lastRun[], lastCrash, historyCount, history[], note }
//  · 三种「发送范围」: 最近一轮 / 最近一轮 + 崩溃记录 / 最近 N 条永久记录;
//  · 默认关闭: 开关关闭时不会自动发; 地址为空时连手动点都发不出去 —— 全程不产生任何网络请求;
//  · 不接任何第三方 SDK, 只用官方 @ohos.net.http, Content-Type: application/json;
//  · 连接 / 读取超时各 10 s; 失败只返回一条 note(交给 RunLog 记一行 upload 日志), 不阻塞跑分;
//  · 权限: 只用到 ohos.permission.INTERNET(普通权限, 系统自动授予, 不弹窗), 不涉及任何敏感权限。
// ---------------------------------------------------------------------------

export const UPLOAD_URL_KEY: string = 'runlog_upload_url';
export const UPLOAD_ON_KEY: string = 'runlog_upload_on';
export const UPLOAD_RANGE_KEY: string = 'runlog_upload_range';
export const UPLOAD_HISTN_KEY: string = 'runlog_upload_histn';
export const UPLOAD_TIMEOUT_MS: number = 10000;
export const UPLOAD_HISTN_DEFAULT: number = 20;
export const UPLOAD_HISTN_MAX: number = 500;

// ---- 三种发送范围 ----
export const UPLOAD_RANGE_LAST_RUN: string = 'last_run';
export const UPLOAD_RANGE_LAST_RUN_CRASH: string = 'last_run_crash';
export const UPLOAD_RANGE_HISTORY_N: string = 'history_n';

export interface UploadChoice {
  id: string;
  label: string;
}

export const UPLOAD_RANGE_CHOICES: UploadChoice[] = [
  { id: UPLOAD_RANGE_LAST_RUN, label: '最近一轮' },
  { id: UPLOAD_RANGE_LAST_RUN_CRASH, label: '最近一轮+崩溃' },
  { id: UPLOAD_RANGE_HISTORY_N, label: '最近 N 条永久记录' }
];

export interface UploadResult {
  ok: boolean;
  code: number;
  ms: number;
  note: string;
}

export class LogUpload {
  private static inst: LogUpload | null = null;
  private ready: Promise<void> | null = null;
  private urlValue: string = '';
  private onValue: boolean = false;
  private rangeValue: string = UPLOAD_RANGE_LAST_RUN;
  private histN: number = UPLOAD_HISTN_DEFAULT;
  private lastNote: string = '';
  private lastOk: boolean = false;
  private lastAt: number = 0;
  private saveTimer: number = -1;

  static get(): LogUpload {
    if (!LogUpload.inst) {
      LogUpload.inst = new LogUpload();
    }
    return LogUpload.inst;
  }

  init(ctx: common.Context): Promise<void> {
    if (!this.ready) {
      this.ready = this.load(ctx);
    }
    return this.ready;
  }

  private async load(ctx: common.Context): Promise<void> {
    try {
      await BenchStore.get().init(ctx);
      const u: string = await BenchStore.get().getText(UPLOAD_URL_KEY, '');
      const f: string = await BenchStore.get().getText(UPLOAD_ON_KEY, '');
      const rg: string = await BenchStore.get().getText(UPLOAD_RANGE_KEY, UPLOAD_RANGE_LAST_RUN);
      const hn: string = await BenchStore.get().getText(UPLOAD_HISTN_KEY, UPLOAD_HISTN_DEFAULT.toString());
      this.urlValue = u;
      this.onValue = f === '1';
      this.rangeValue = rg.length > 0 ? rg : UPLOAD_RANGE_LAST_RUN;
      const n: number = parseInt(hn, 10);
      this.histN = (isNaN(n) || n <= 0) ? UPLOAD_HISTN_DEFAULT : Math.min(n, UPLOAD_HISTN_MAX);
    } catch (e) {
      console.warn('[AB] upload settings load failed');
    }
  }

  url(): string {
    return this.urlValue;
  }

  enabled(): boolean {
    return this.onValue;
  }

  range(): string {
    return this.rangeValue;
  }

  // 「最近 N 条永久记录」里的 N(其它范围返回 0)
  historyCount(): number {
    return this.histN;
  }

  rangeLabel(): string {
    if (this.rangeValue === UPLOAD_RANGE_LAST_RUN_CRASH) {
      return '最近一轮 + 崩溃记录';
    }
    if (this.rangeValue === UPLOAD_RANGE_HISTORY_N) {
      return '最近 ' + this.histN.toString() + ' 条永久记录';
    }
    return '最近一轮';
  }

  // 只有"开关打开 且 地址非空"才认为真正启用(每轮跑分结束后才会自动发)
  active(): boolean {
    return this.onValue && this.urlValue.length > 0;
  }

  // 输入框每敲一个字符都会调到这里: 内存值立即生效, 落盘做 800 ms 去抖,
  // 避免"每按一个键就 flush 一次 preferences"把 UI 拖卡。
  setUrl(u: string): void {
    this.urlValue = u;
    if (this.saveTimer >= 0) {
      clearTimeout(this.saveTimer);
    }
    this.saveTimer = setTimeout(() => {
      this.saveTimer = -1;
      this.persistUrl();
    }, 800);
  }

  private persistUrl(): void {
    const v: string = this.urlValue;
    BenchStore.get().putText(UPLOAD_URL_KEY, v).then(() => {
      console.info('[AB] upload url saved');
    }).catch((e: Error) => {
      console.warn('[AB] upload url save failed ' + e.message);
    });
  }

  setEnabled(on: boolean): void {
    this.onValue = on;
    const v: string = on ? '1' : '0';
    BenchStore.get().putText(UPLOAD_ON_KEY, v).then(() => {
      console.info('[AB] upload switch saved');
    }).catch((e: Error) => {
      console.warn('[AB] upload switch save failed ' + e.message);
    });
  }

  setRange(id: string): void {
    this.rangeValue = id;
    BenchStore.get().putText(UPLOAD_RANGE_KEY, id).then(() => {
      console.info('[AB] upload range saved');
    }).catch((e: Error) => {
      console.warn('[AB] upload range save failed ' + e.message);
    });
  }

  setHistoryN(n: number): void {
    let v: number = Math.floor(n);
    if (isNaN(v) || v <= 0) {
      v = UPLOAD_HISTN_DEFAULT;
    }
    if (v > UPLOAD_HISTN_MAX) {
      v = UPLOAD_HISTN_MAX;
    }
    this.histN = v;
    BenchStore.get().putText(UPLOAD_HISTN_KEY, v.toString()).then(() => {
      console.info('[AB] upload histN saved');
    }).catch((e: Error) => {
      console.warn('[AB] upload histN save failed ' + e.message);
    });
  }

  lastResultNote(): string {
    return this.lastNote;
  }

  lastResultOk(): boolean {
    return this.lastOk;
  }

  lastResultAt(): number {
    return this.lastAt;
  }

  // 只用于日志/UI 展示: 只留主机名, 不把可能带 token 的完整 URL 写进日志
  targetLabel(): string {
    const u: string = this.urlValue;
    if (u.length === 0) {
      return '(未填写)';
    }
    const i: number = u.indexOf('://');
    let rest: string = i >= 0 ? u.substring(i + 3) : u;
    const j: number = rest.indexOf('/');
    if (j >= 0) {
      rest = rest.substring(0, j);
    }
    return rest;
  }

  // 一次发送 = 一次 JSON POST。
  // 注意: 这里只要求"地址非空" —— 手动点「立即发送一次」不需要先打开自动发送开关;
  // 开关(active())只决定"每轮跑分结束后是否自动发一次"。地址为空时一个请求都不会发。
  send(payload: string, rangeLabel: string): Promise<UploadResult> {
    const started: number = Date.now();
    if (this.urlValue.length === 0) {
      return Promise.resolve(this.done(false, 0, started, '未填写上传地址：本应用不发送任何数据'));
    }
    if (payload.length === 0) {
      return Promise.resolve(this.done(false, 0, started, '没有可发送的内容'));
    }
    return this.post(payload).then((res: UploadResult) => {
      const note: string = res.note + ' · ' + payload.length.toString() + ' 字符 · 范围 ' + rangeLabel;
      return this.done(res.ok, res.code, started, note);
    });
  }

  // 一次 POST; 任何失败都转换成 UploadResult(note 里带原因), 不向上抛
  post(payload: string): Promise<UploadResult> {
    const url: string = this.urlValue;
    const started: number = Date.now();
    if (url.length === 0) {
      return Promise.resolve(this.done(false, 0, started, '未填写上传地址'));
    }
    let req: http.HttpRequest | null = null;
    try {
      req = http.createHttp();
      const header: Record<string, string> = { 'Content-Type': 'application/json' };
      const opt: http.HttpRequestOptions = {
        method: http.RequestMethod.POST,
        header: header,
        extraData: payload,
        connectTimeout: UPLOAD_TIMEOUT_MS,
        readTimeout: UPLOAD_TIMEOUT_MS,
        expectDataType: http.HttpDataType.STRING
      };
      const r: http.HttpRequest = req;
      return r.request(url, opt).then((resp: http.HttpResponse) => {
        const ms: number = Date.now() - started;
        const code: number = resp.responseCode as number;
        const ok: boolean = code >= 200 && code < 300;
        // 把服务端返回的前 120 字符带进 note, 便于用户自己判断是收下了还是被拒了
        const body: string = typeof resp.result === 'string' ? (resp.result as string) : '';
        const brief: string = body.length > 120 ? (body.substring(0, 120) + '…') : body;
        r.destroy();
        return this.done(ok, code, started, ok
          ? ('HTTP ' + code.toString() + ' 发送成功' + (brief.length > 0 ? (' · 返回 ' + brief) : ''))
          : ('HTTP ' + code.toString() + ' 服务端未接受' + (brief.length > 0 ? (' · 返回 ' + brief) : '')));
      }).catch((e: Error) => {
        r.destroy();
        return this.done(false, 0, started, '发送失败: ' + e.message);
      });
    } catch (e) {
      if (req !== null) {
        req.destroy();
      }
      return Promise.resolve(this.done(false, 0, started, '发送发起失败'));
    }
  }

  private done(ok: boolean, code: number, started: number, note: string): UploadResult {
    this.lastOk = ok;
    this.lastNote = note;
    this.lastAt = Date.now();
    const res: UploadResult = { ok: ok, code: code, ms: Date.now() - started, note: note };
    return res;
  }
}
