import preferences from '@ohos.data.preferences';
import { common } from '@kit.AbilityKit';
import { BenchRecord } from '../model/BenchModel';

// 崩溃面包屑与跑分记录共用同一套 preferences(同一个 name = 同一个 store 文件),
// 这样 init(ctx) 一次即可同时服务 records 与 crumb。
export const PREF_NAME = 'aurora_bench';
// 崩溃面包屑: 每次开始跑任一负载项前写入, 整轮成功返回后清除
export const CRUMB_KEY = 'run_crumb';
// 崩溃自诊断历史(可复制的文本), 保留最近若干条
export const CRUMB_LOG_KEY = 'crash_log';
// 稳定性压测会话(连续 N 轮 CS1 单核): 只有这一条状态需要跨进程存活,
// 崩溃恢复全靠它 —— running = true 就说明上次压测没结束。
export const STRESS_SESSION_KEY = 'stress_campaign';
// 压测结束/中断后的最终报告文本(可复制): 与进行中的会话分开存,
// 这样"结束并查看报告"之后即使进程再被杀, 报告也还在。
export const STRESS_REPORT_KEY = 'stress_report';
// 超线程(SMT)开关偏好: '1' = 开(默认) / '0' = 关。
// 为什么默认开: 关闭会降低多核分数(每个物理核只用一条线程), 属于"更接近物理核扩展性"的
// 另一种口径, 不该在用户不知情时生效 —— 所以默认值与历史行为一致(全部逻辑核)。
export const SMT_ENABLED_KEY = 'smt_enabled';


const STORE = PREF_NAME;
const KEY = 'records';

export class BenchStore {
  private static inst: BenchStore | null = null;
  private store: preferences.Preferences | null = null;

  static get(): BenchStore {
    if (!BenchStore.inst) {
      BenchStore.inst = new BenchStore();
    }
    return BenchStore.inst;
  }

  async init(ctx: common.Context): Promise<void> {
    if (!this.store) {
      this.store = await preferences.getPreferences(ctx, STORE);
    }
  }

  isReady(): boolean {
    return this.store !== null;
  }

  async load(): Promise<BenchRecord[]> {
    if (!this.store) {
      return [];
    }
    const text: string = await this.store.get(KEY, '') as string;
    if (!text) {
      return [];
    }
    return JSON.parse(text) as BenchRecord[];
  }

  async append(record: BenchRecord): Promise<BenchRecord[]> {
    const list = await this.load();
    list.unshift(record);
    while (list.length > 20) {
      list.pop();
    }
    if (this.store) {
      await this.store.put(KEY, JSON.stringify(list));
      await this.store.flush();
    }
    return list;
  }

  async best(): Promise<number> {
    const list = await this.load();
    let best = 0;
    for (const r of list) {
      if (r.total > best) {
        best = r.total;
      }
    }
    return best;
  }

  // ---- 通用文本读写: 崩溃面包屑专用 ----
  // 每次写入后立刻 flush 落盘: native 层 SIGSEGV / OOM 直接杀进程,
  // 不 flush 的话数据可能还停在内存缓存里, 下次启动就读不到了。
  async putText(key: string, value: string): Promise<boolean> {
    if (!this.store) {
      return false;
    }
    await this.store.put(key, value);
    await this.store.flush();
    return true;
  }

  async getText(key: string, def: string): Promise<string> {
    if (!this.store) {
      return def;
    }
    return await this.store.get(key, def) as string;
  }

  async removeText(key: string): Promise<void> {
    if (!this.store) {
      return;
    }
    await this.store.delete(key);
    await this.store.flush();
  }
}
