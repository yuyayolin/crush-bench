// libauroraref.so —— 参考对照 / 线性-可比性审计模块的模块声明
//
// 为什么声明放在 ets 里而不是 cpp/types/ 下:
//   本任务的范围是"只改 ets, 不动 cpp"。这个 .d.ts 只是给 ArkTS 编译器一个类型说明,
//   运行时仍然是运行时按模块名加载已打进 HAP 的 libauroraref.so
//   (native 侧 reference_napi.cpp: napi_module_register, nm_modname = "auroraref"),
//   与 libaurorasn.so / libauroragpu7.so 完全同一个机制 —— 本模块只读:
//   只生成对照表, 不跑负载、不改分数、不计分。
//
// 导入写法(见 service/RefAudit.ets):
//   import { compare, version, realUse } from 'libauroraref.so';

declare module 'libauroraref.so' {
  /** 真值表版本(改动真值表才 +1) */
  export const version: () => string;

  /**
   * 生成对照表。optionsJson 是扁平 JSON 字符串(native 侧用字符串解析):
   *   singleCore / multiCore : 逐项结果的 JSON 数组原文(字符串; 每项 name/score/metric/unit)
   *   singleComposite / multiComposite : 复合分(数字)
   *   gpuSnl / gpu7Items     : GPU-SNL 原始 JSON / CS1 GPU 逐项 JSON(字符串)
   *   threadsUsed / workerCpus / workerCpusList / allowedCores / logicalCores /
   *   physicalCores / runtimeKhzMedian / nominalTopKhz : 条件化可比性审计要的量(数字)
   * 拿不到的键不要传(native 侧会写 null, 不会编 0)。
   * 返回对照表 JSON(同步, 毫秒级; 失败时是 {"ok":false,"error":"..."})。
   */
  export const compare: (optionsJson: string) => string;

  /** 单项真实用途一句话(查不到返回空串) */
  export const realUse: (name: string) => string;

  /**
   * 「两份报告对比」(纯计算 / 只读): 传入两台设备各自的一键全跑报告 JSON 原文。
   * 逐项给出 分数比 / 每核归一化比 / 同频归一化比 / 与真值的偏差, 并列出两边的取得条件
   * (可用核数、可用最高频档、线程数、实际用到核数、运行时频率中位)。
   * optionsJson 可选, 与 compare() 同一套键(拿不到的键不要传)。
   * 返回对比 JSON; 任一侧缺数据时写"缺什么", 不编数字。
   * (本工程目前的 ets 侧还没有调用点 —— 这里先把类型补上, 免得日后为了加一个 .so 依赖
   *  去动 cpp/types 目录, 那会破坏"ets 侧改动不需要碰 cpp"这条边界。)
   */
  export const compareReports: (jsonA: string, jsonB: string, optionsJson?: string) => string;
}
