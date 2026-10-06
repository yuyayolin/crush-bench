#ifndef AURORA_REFERENCE_H
#define AURORA_REFERENCE_H

// ===========================================================================
//  极光跑分 · 参考分对照模块 (libauroraref.so / auroraref)
// ---------------------------------------------------------------------------
//  验收标准第 3、4、5 条要的东西:
//    * 第 3 条: 把本机每一项/复合分与公开真值并列, 每条都带来源与来源种类;
//              第三方数据不写成官方; 查不到的项写"查不到"。
//    * 第 4 条: 每一项一句话说明它代表什么真实用途。
//    * 第 5 条: 每一项都能回答"这个分数是怎么来的"(metric x conv x k)、单位是什么、
//              为什么与官方同口径或不同口径。
//
//   本模块 完全只读: 不跑任何负载、不改任何分数、不参与计分。
//    它的输入是 ArkTS 已经拿到的结果(GB7 单项 JSON / 复合分 / GPU-SNL 的 JSON),
//    输出是一张"对照 + 解释"的表。**没有传入的东西, 它会写 null / 查不到,
//    不替用户猜一个数字出来。**
//
//  导出:
//    version()  -> string  本模块与真值表的版本(改动真值表就要 +1)
//    compare(json) -> string
//      json(全部可选, 扁平):
//        { "singleCore": <CS1 单核结果数组的 JSON 字符串>,
//          "multiCore":  <CS1 多核结果数组的 JSON 字符串>,
//          "singleComposite": <数>, "multiComposite": <数>,
//          "gpuSnl":     <GPU-SNL 的 run() 返回值 JSON 字符串>,
//          "gpu7Items":  <gpu7 11 项的 JSON 数组字符串> }
//      返回:
//        { "ok":true, "truthVersion":1, "referenceDevice":"...",
//          "referenceDeviceSource":"...", "sourceKinds":{...},
//          "singleCore":{ "items":[{name,realUse,howComputed,unit,ours:{...},
//                                  reference:{score,metric,source,sourceKind},
//                                  deltaPercent,comparable,caveat}],
//                         "composite":{ours,reference,deltaPercent} },
//          "multiCore":{...}, "gpu":{...},
//          "notAvailable":[...], "caveats":[...] }
//
//  为什么真值表写在注释与 JSON 里而不是"藏起来": 公开真值必须可以被任何人核对,
//  来源、口径、查证强度都要跟着数字一起走 —— 这是"跑分软件要可信"的最低要求。
// ===========================================================================

#include <string>

#include "napi/native_api.h"

// 本模块 + 真值表的版本。 改动真值表 / 增删对照项都要 +1(旧的对照结论届时不可比)。
extern const int kReferenceVersion;

std::string auroraReferenceVersionText();

// 见文件头的 JSON 契约。任何字段缺失都只会变成 null / "查不到", 不编造。
std::string auroraReferenceCompare(const std::string& optionsJson);

// 单项真实用途查询(给 UI 单条显示用): 查不到返回空串。
std::string auroraReferenceRealUse(const std::string& itemName);

// ---------------------------------------------------------------------------
//   两份报告对比(2026-08-31 新增) —— 纯计算 / 只读, 不跑负载 / 不连设备 / 不写文件
// ---------------------------------------------------------------------------
// 用途: 用户在两台设备上各跑一次(一键全部跑分 -> files/report-latest.json),
//       把两份报告全文传进来, 直接拿到:
//         ① 逐项对照表: 设备A 分 / 设备B 分 / 分数比(设备B / 设备A) / 该项是否计分 /
//            两边的取得条件(可用核数 allowedCores、可用最高频档 cpuMaxKhz、线程数
//            threadsRequested / threadsEffective / workersBound(实际用到核数)、
//            运行时频率中位(从 runFreq 文本解析));
//         ② 原始分数比 + 归一化后的比: perCoreRatio(每核吞吐比)与 sameFreqRatio(同频归一化比)
//            —— 核数差会让总分比虚高 12.5%(9 vs 8), 频率档差能凭空造出 13.5% 的假提升;
//         ③ 与参考真值的对照: 有真值的项给出偏差百分比, 并判定是否线性(阈值 5%,
//            阈值是我们自己定的, 不是官方阈值 —— 输出里每一处都标了);
//         ④ 对照真值锚点(9000S -> 9020 -> 9030 Pro)给出"实测比 vs 真值比"的偏差。
//
// 输入:
//   jsonA / jsonB —— 两份报告 JSON 全文(schema = aurora-fullreport/1; 直接传文件内容即可)。
//   optionsJson(可选, 扁平 JSON, 可为空串):
//     { "labelA":"设备A", "labelB":"设备B",          // 显示名(可选)
//       "socA":"Kirin 9030 Pro", "socB":"Kirin 9020" } // 显式指定 SoC(推荐; 不指定时本模块
//                                                      //   只按报告文本/机型名推断, 判不出来就写判不出来)
//  2026-10-08 补的三块(用户点名的三条硬要求):
//   1) selfCheckAudit —— 两边各自的「随芯片变化自检」排除集, 逐项给项名 + 排除原因(判定依据
//      原文) + 判定口径 + 判据, 并直接给出"两边排除集是否一致"的结论。排除集不同 => 两个复合分
//      含的不是同一批项 => 复合分原始比不可用(本模块会明写 rawMultiCompositeRatioUsable=false,
//      并额外给出 compositeParticipants.sameItemSetComparison —— 只用两边共同参与项重算的比)。
//   2) 频率证据 —— 逐项 conditions.runtimeKhzMedian / nominalTopKhz / frequencyRawText(runFreq 原文) /
//      frequencyNullReason(拿不到时写为什么); 每台一份 runtimeKhzMedianOfItems(min/max/中位/平均)
//      与 machineLevelFromReport(报告 inputsNote 原话的 runtimeKhzMedian= / nominalTopKhz=)。
//      sameFreqRatio 就是靠"运行时频率中位 x 标称最高频档"算出来的。
//   3) conditionGaps —— 两台跑分条件的量化差: 多核线程数与线程数比、SMT 状态、可用核集合、
//      物理/逻辑核, 以及它对多核比的影响方向; 另给每核比的适用前提。有一侧拿不到线程数就写 null
//      并说明"条件差算不出来", 不写同条件。
//   4) "两台都把芯片跑满了吗"(2026-10-09 新增)—— 用户点名的最后一条:
//        * 逐项取报告 runFreq 文本里的『占标称比 中位 X%』(多核项取口径A: 当刻有负载线程
//          落上的核; 不是把空闲核一起算进去的口径B), 单核阶段与多核阶段分开统计;
//        * 阈值直接取同一段文本里的『占比中位 >= X%』(X = 90, 我们自己定的, 不是任何
//          官方阈值); 文本里没有阈值原文(旧报告)时才回退到本模块写死的 90, 并标注是回退;
//        * 两台单核与多核都达到判据 -> 结论就是原始分数比(rawScoreRatioIsTheConclusion=true,
//          conclusion 里给出单核/多核两个原始比; 归一化比降级为参考与诊断);
//        * 只要有一侧没达到 -> conclusion 明写"原始分比不能直接当芯片性能比", 归一化比只作
//          辅助与诊断, 并逐项列出没跑满的项(项名 + 占比), 指向真正的修法
//          (每项 runFreq 里的预热取证与上限取证);
//        * 落地字段: chipPerformanceRatio.saturationCheck(单核/多核 x A/B 四份统计 + bothFull
//          + rawScoreRatioIsTheConclusion + normalizationRole) 与顶层 rawVsNormalized(同一个结论
//          的独立展开), 逐项字段在 items[].a/b.conditions.saturationPctOfNominal / saturationVerdict。
//      本模块只搬报告里的原文数字, 不重新定义任何频率量, 也不写 sysfs / 不锁频 / 不锁核。
//
// 返回 JSON(节选):
//   { "ok":true, "ratioDefinition":"ratio = 设备B / 设备A",
//     "inputs":{ "a":{...}, "b":{...}, "missing":[...] },
//     "conditions":{ "a":{...}, "b":{...}, "observedDifferences":{...},
//                    "conditionGaps":{...}, "selfCheckAudit":{...} },
//     "compositeParticipants":{ "single":{...}, "multi":{...} },
//     "knownInflationSources":[...], "normalizationDefinition":{...},
//     "items":[ { section, name, a:{score,metric,unit,conditions{...}}, b:{...},
//                 ratio, normalized:{perCoreRatio,sameFreqRatio,perCoreSameFreqRatio,why},
//                 truth:{referenceA,referenceB,deltaPercentA,deltaPercentB,
//                        truthRatioBA,measuredRatioBA,deviationFromTruthPercent,within5Percent,why} } ],
//     "composites":{ gb7Single:{a,b,ratio,reference,deltaPercentA,deltaPercentB},
//                    gb7Multi:{...}, normalized:{...} },
//     "truthAnchors":{ thresholdPercent:5, thresholdIsOursNotOfficial:true, rows:[...] },
//     "verdicts":{...}, "notComparable":[...], "caveats":[...] }
//
// 缺失处理: 报告缺失 / 格式不符 / 字段缺失 / 机型判不出来 -> 一律 null / 空数组 + 一句话说明
//   "缺什么"。任何情况下都不会编一个数字出来。
std::string auroraReferenceCompareReports(const std::string& jsonA, const std::string& jsonB,
                                          const std::string& optionsJson);

// napi 导出注册(由 napi_init.cpp 的 Init 调用一次)
void auroraReferenceRegisterNapi(napi_env env, napi_value exports);

#endif
