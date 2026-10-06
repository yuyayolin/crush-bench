#ifndef AURORA_NPU_BENCH_H
#define AURORA_NPU_BENCH_H

// ===========================================================================
// 极光跑分 · NPU(AI 加速器)独立小节 —— 接口声明
//
// 设计原则(与工程其它部分一致):
//   * 结果不参与 GB7 计分(GB7 里没有 NPU 对应项, 混进去会毁掉可比性);
//     返回的 JSON 里带 "scored":false / "gb7Item":false, 由 ArkTS 侧单独显示。
//   * 失败不返回 0 或静默跳过: 任何一步失败都把 OH_NN_ReturnCode 的原始枚举名
//     与数值写进返回的 JSON("error" 字段), 界面与 runlog 都能看到。
//   * 设备类型是这一节最重要的元信息: 结果里永远写明实际用的是
//     OH_NN_ACCELERATOR(NPU) / OH_NN_GPU / OH_NN_CPU 中的哪一个, 以及回退过程。
//
// 实现见 npu_bench.cpp。两个函数都是纯计算 + 返回 JSON 字符串, 不碰 napi;
// napi 包装(异步 Promise)在 napi_init.cpp 里。
//
// ---- NNRt 是运行期 dlopen/dlsym 绑定的, 不在链接期依赖里 ----
// 理由与 crash_guard.cpp 对 _Unwind_Backtrace 用 dlsym + 降级完全同一条原则:
// 可选功能不该有让主功能加载失败的能力。
// sysroot 里的 libneural_network_runtime.so / libneural_network_core.so 是链接桩
// (函数体只有 ret, 无 DT_NEEDED); 硬链会让 libaurorabench.so 带上硬 DT_NEEDED, 真机一旦
// 没有这两个库, 失效方式就是 整个 requireNapi("aurorabench") 加载失败 ——
// 一个可选小节把 CS1/CoreMark 全部拖死。所以这里自己 dlopen + dlsym 38 个函数指针,
// 拿不到就在本小节内报不可用; CMakeLists 里故意没有 neural_network 的链接项。
//
// 两种"不可用"必须分清(结果里是两个不同的 status):
//   "NNRT_UNAVAILABLE" -> 本 App 拿不到 NNRt 接口(dlopen 失败 / 缺符号), 原因见 nnrt.error
//   "NOT_OPEN"         -> 接口拿到了, 但这台机器没向第三方应用开放 NPU
// ===========================================================================

#include <string>

// ---- 第一步: 设备探测 --------------------------------------------------------
// 用 OH_NNDevice_GetAllDevicesID / OH_NNDevice_GetName / OH_NNDevice_GetType
// 枚举设备。返回 JSON(人可读, 可直接显示):
// {
//   "ok": bool,                       // 枚举接口本身是否成功
//   "kind": "npu-probe",
//   "status": "OPEN"|"NOT_OPEN"|"NO_DEVICE"|"ENUM_FAILED"|"NNRT_UNAVAILABLE",
//   "nnrt": { "available":bool, "binding":"dlopen + dlsym 运行时绑定(不硬链)",
//             "runtimeLibrary":"libneural_network_runtime.so","runtimeOpened":bool,
//             "runtimeDlerror":"...", "coreLibrary":"libneural_network_core.so",
//             "coreOpened":bool, "coreDlerror":"...",
//             "expectedFunctionCount":38, "boundFunctionCount":n,
//             "missingSymbols":"缺失符号名(逗号分隔)", "error":"..." },
//   "thirdPartyNpuOpen": bool,        // 关键结论 这台机器是否向第三方应用开放了 NPU
//   "text": "...",                    // 一行中文结论(可直接显示)
//   "error": "...",                   // status=ENUM_FAILED 时是 "NPU 探测失败: OH_NN_XXX(n)"
//   "enumRc": n, "enumRcName": "OH_NN_XXX(n)",
//   "deviceCount": n,
//   "devices": [ {"id":n,"name":"...","nameRc":n,"nameRcName":"...",
//                 "type":n,"typeName":"CPU|GPU|ACCELERATOR|OTHERS",
//                 "typeEnum":"OH_NN_XXX(n)","typeRc":n,"typeRcName":"..."} , ...],
//   "hasCpu": bool, "hasGpu": bool, "hasAccelerator": bool,
//   "notes": "..."
// }
std::string npuProbeJson();

// ---- 第二步: 推理跑分 --------------------------------------------------------
// 纯代码构造 MATMUL 链 + RELU 模型(见 npu_bench.cpp 顶部的乘加次数推导),
// 按 ACCELERATOR -> GPU -> CPU 顺序尝试编译与执行, 预热后计时。
// 若 MATMUL+RELU 这一组算子在所有设备上都没成功, 再把整条设备阶梯用
// FULL_CONNECTION+RELU 走一遍(乘加次数口径相同, 仍是 MACs = layers * dim^3);
// 实际用的是哪一组写在 model.opSet / model.opSetName / model.opSetText 里。
// 返回 JSON:
// {
//   "ok": bool, "kind":"npu-bench", "section":"NPU",
//   "scored": false, "gb7Item": false,      // 明确不可混进 GB7
//   "text": "...",  "error": "...",         // 失败时 error 带原始错误码
//   "metric": "...", "unit": "GOPs/s",      // 失败时 metric="0" unit="运行失败: ..."
//   "device": {"chosenDeviceId":n,"typeName":"ACCELERATOR","typeEnum":"OH_NN_ACCELERATOR(3)",
//              "name":"...","preferredTypeName":"ACCELERATOR","preferredOrder":"...",
//              "fallbackUsed":false,"fallbackText":"...","attempts":[{...}]},
//   "precision": {"available":bool,"modelDataType":"FLOAT32","float16Requested":bool,
//                 "float16Rc":"OH_NN_XXX(n)","float16Enabled":bool,
//                 "execInputDataType":"OH_NN_FLOAT32(11)","execInputFormat":"...",
//                 "text":"...","known":bool},
//   "model": {"dim":n,"layers":n,"scale":n,"scaleLabel":"...",
//             "opSet":n,"opSetName":"MATMUL+RELU|FULL_CONNECTION+RELU","mainOp":"OH_NN_OPS_*",
//             "opSetFallbackUsed":bool,"opSetVerifiedOnHost":false,"opSetText":"...","opSets":[..],
//             "ops":"...","matmulOps":n,"reluOps":n,
//             "macsPerInference":n,"flopsPerInference":n,"macFormula":"...",
//             "weightBytes":n,"inputBytes":n},
//   "device": {..., "attempts":[{"deviceId":n,"opSet":n,"opSetName":"...","mainOp":"...",
//              "typeName":"...","ok":bool,"failStage":"...","failRc":n,"failRcName":"...",
//              "rcTrail":"...","execInputDataTypeRc":"...", ...}]},
//   "timing": {"available":bool,"buildMs":f,"firstRunMs":f,"firstInferenceMs":f,
//              "warmupRuns":n,"measuredRuns":n,"itersPlanned":n,"budgetMs":n,
//              "budgetHit":bool,"meanMs":f,"minMs":f,"medianMs":f,"maxMs":f,
//              "goPs":f,"macsPerSec":f,"hint":"..."},
//   "deviceCount":n, "devices":[...], "scalePresets":[...],
//   "nnrt": { 同 probe 的那个对象 },
//   "notes":"..."
// }
struct NpuBenchRequest {
    // 模型规模档位(见 npu_bench.cpp 的 kPresets): 0 小 / 1 中(默认) / 2 大 / 3 特大。
    // 目的就是把单次推理耗时定在 10~100 ms 量级; 结果里会给 "hint" 提示该调哪一档。
    int scale = 1;
    // 预热次数(0~50)
    int warmup = 2;
    // 计时次数上限(1~500), 实际跑多少次还受 budgetMs 约束
    int iters = 10;
    // 整个跑分(含建模/编译)的时间预算, 毫秒(200~5000), 保证单次运行 1~3 秒量级
    int budgetMs = 1200;
};
std::string npuBenchJson(const NpuBenchRequest& req);

// 兜底: 把未捕获的 C++ 异常变成一条带原因的失败 JSON(不允许静默返回 0)。
std::string npuFailureJson(const std::string& why);

#endif // AURORA_NPU_BENCH_H
