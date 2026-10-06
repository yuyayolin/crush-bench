// libauroraref.so —— 参考分对照 + 两份报告对比 的类型声明(只读模块)
//
//  本模块只读: 不跑任何负载、不连设备、不读 /sys、不写任何文件、不改任何分数、
//   不计分。它只做两件事:
//     ① 把本机结果与公开/一手真值并列(带来源与来源种类, 第三方不写成官方);
//     ② 对比两份报告(两台设备各跑一次之后算分数比 + 归一化比 + 与真值的偏差)。
//
// 典型接线(ArkTS):
//   import { version, compare, realUse, compareReports } from 'libauroraref.so';
//
//   // ① 单机对照表(输入是 GB7 单项 JSON / 复合分 / GPU-SNL JSON)
//   const one = JSON.parse(compare(JSON.stringify({ singleCore: ..., multiCore: ...,
//                                                    singleComposite: 1633, multiComposite: 6802,
//                                                    gpuSnl: snJson })));
//   // one.singleCore.items[].{ours,reference,deltaPercent,sourceKind,source,caveat}
//
//   // ② 两台设备的报告对比(输入就是 files/report-latest.json 的全文)
//   const a = /* 设备A 的 report-latest.json 文本 */;
//   const b = /* 设备B 的 report-latest.json 文本 */;
//   const cmp = JSON.parse(compareReports(a, b, JSON.stringify({ labelA: '我的手机', labelB: '参考机' })));
//   // cmp.items[].{a,b,ratio,normalized{perCoreRatio,sameFreqRatio,perCoreSameFreqRatio},truth{...}}
//   // cmp.composites.{gb7Single,gb7Multi}.ratio
//   // cmp.truthAnchors.rows[].{truthRatioBA,measuredRatioBA,deviationPercent,linearWithin5Percent}
//   // cmp.inputs.missing[]  -> 缺什么就写什么(报告缺失 / 字段缺失 / 机型判不出来), 不编数
//
//  三条必须与数字一起展示的口径:
//   * ratio = 设备B / 设备A(> 1 表示设备B 更高);
//   * 原始分数比会被"可用核数差(9 vs 8 核 = 虚高 12.5%)"与"频率档差(1.135x = 13.5% 假提升)"
//     污染 —— 要拿芯片性能比请看 normalized 里的三个量; 判定阈值(5%)与重复性阈值(2% / 5%)
//     都是我们自己定的, 不是官方阈值。

/** 真值表版本(改动真值表会 +1; 跨版本的对照结论不可比) */
export const version: () => string;

/**
 * 单机对照表(只读)。
 * optionsJson(扁平 JSON 字符串, 全部可选):
 *   singleCore / multiCore : GB7 单项结果数组的 JSON 字符串
 *   singleComposite / multiComposite : 复合分(数)
 *   gpuSnl : GPU-SNL run() 的返回值 JSON 字符串
 *   gpu7Items : GPU7 11 项的 JSON 数组字符串
 *   threadsUsed / workerCpus / workerCpusList / allowedCores / logicalCores / physicalCores /
 *   runtimeKhzMedian / nominalTopKhz : 条件化可比性用的上下文
 * 返回 JSON: { ok, truthVersion, referenceDevice, sourceKinds, singleCore{items[],composite},
 *              multiCore, linearityAudit, comparabilityAudit, gpu{snl,gb7GpuItems},
 *              notAvailable[], caveats[] }
 */
export const compare: (optionsJson?: string) => string;

/** 单项的真实用途一句话(给 UI 单条显示用); 查不到返回空串(不编) */
export const realUse: (itemName: string) => string;

/**
 *  两份报告对比(纯计算 / 只读) —— 在两台设备上各跑一次之后, 自动算出:
 *   ① 逐项对照表: A 分 / B 分 / 分数比 / 该项是否计分 / 两边的取得条件
 *      (可用核数、可用最高频档、线程数、实际用到核数、运行时频率中位);
 *   ② 原始分数比 + 归一化比(每核吞吐比、同频归一化比、两个一起);
 *   ③ 与真值的偏差百分比 + 线性判定(阈值 5%, 输出里标明是我们自己定的);
 *   ④ 缺什么写什么(报告缺失 / 格式不符 / 机型判不出来 -> null + 说明, 不编数)。
 *
 * @param jsonA 设备A 的报告 JSON 全文(本工程一键跑分落盘的 files/report-latest.json)
 * @param jsonB 设备B 的报告 JSON 全文
 * @param optionsJson 可选, 扁平 JSON 字符串:
 *        { "labelA": "我的手机", "labelB": "参考机",
 *          "socA": "Kirin 9030 Pro", "socB": "Kirin 9020" }   // 显式指定 SoC(推荐)
 * @returns 结果 JSON 字符串(契约见 reference_compare.h; 任何异常都返回 {"ok":false,...})
 */
export const compareReports: (jsonA: string, jsonB: string, optionsJson?: string) => string;
