/*
 * npu_bench.cpp —— 极光跑分 · NPU(AI 加速器)跑分(独立小节, 不参与 GB7 计分)
 *
 * 为什么独立成节: CS1 的 CPU/GPU 项里没有任何 NPU 对应项, 把这里的结果
 * 混进 CS1 复合分会立刻毁掉 CS1 分数自身的可比性。所以本文件只产出自己的
 * JSON, 由 ArkTS 侧单独显示(结果里带 "scored":false / "gb7Item":false 双重标记)。
 *
 * 依赖(全部在 API 18 sysroot 里, 零第三方库):
 *   <sysroot>/usr/include/neural_network_runtime/neural_network_runtime.h       建模
 *   <sysroot>/usr/include/neural_network_runtime/neural_network_core.h          设备/编译/执行器/张量
 *   <sysroot>/usr/include/neural_network_runtime/neural_network_runtime_type.h  枚举与结构体
 *   (只有头文件参与编译 —— 那两个 .so 不在链接期依赖里, 见下)
 *
 * ---- 为什么 NNRt 走 dlopen, 而不是写进 CMakeLists 的链接库 ----
 *
 * 事实一(在 sysroot 上用 llvm-objdump 核实过): libneural_network_runtime.so 与
 * libneural_network_core.so 在 sysroot 里是链接桩 —— 每个导出函数体只有一条 ret,
 * 且没有任何 DT_NEEDED。它们唯一的用途是让交叉链接器解析符号; 真机上由 /system/lib64
 * 提供真正的实现(与 libhilog_ndk.z.so / libace_napi.z.so 等其它 NDK 库同一机制)。
 *
 * 事实二: 如果按常规把它们写进 target_link_libraries, libaurorabench.so 就会带上对它们的
 * 硬 DT_NEEDED。而一旦设备上这两个库不存在(或 soname 对不上), 失效方式是
 * 整个 requireNapi("aurorabench") 加载失败 —— 也就是说, 一个可选的 NPU 小节
 * 会把 CS1 / CoreMark 全部一起拖死。这是最糟的失效方式。
 *
 * 本工程已有完全相同的先例: crash_guard.cpp 对 _Unwind_Backtrace 用的是
 * dlsym(RTLD_DEFAULT) + 降级。理由一字不差 —— 可选功能不该有让主功能加载失败的能力。
 * 这里照同一条原则办: NPU 小节自己 dlopen + dlsym, 拿不到就在本小节内报一个明确的
 * "NNRT_UNAVAILABLE", 不牵连 App 的加载, 也不崩。
 *
 * 因此: libaurorabench.so 的 DT_NEEDED 里没有任何 neural 相关项(已用 llvm-readelf 复核),
 * dlopen/dlsym 在符号表里是 U(运行期由 libc/libdl 解析)。
 * 另外注意: "dlopen 成功" 不等于 "设备上一定有 NPU" —— 后者只能由真机上的
 * OH_NNDevice_GetAllDevicesID 回答, 本文件把这句话原样写进返回值(见 probe 的
 * "thirdPartyNpuOpen" 与 bench 的 device.typeName)。两件事分得很清楚:
 *   status == "NNRT_UNAVAILABLE" -> 本 App 拿不到 NNRt 接口(库不在/缺符号);
 *   status == "NOT_OPEN"         -> 接口拿到了, 但这台机器没向第三方应用开放 NPU。
 *
 * ---- 多线程 ----
 * NNRt 头文件明确写着 "the APIs of Neural Network Runtime do not support
 * multi-thread calling", 所以本文件用一把互斥锁把 probe / bench 串行化, 防止
 * 用户连点两次按钮时两个 worker 线程同时进出 NNRt。
 */

#include "npu_bench.h"

#include <hilog/log.h>

#include <neural_network_runtime/neural_network_core.h>
#include <neural_network_runtime/neural_network_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>   // dlopen/dlsym: NNRt 走运行时绑定, 不硬链(理由见文件头)
#include <exception>
#include <mutex>
#include <string>
#include <vector>

// ===========================================================================
// 编译期核对: 下面每一条都逐个对照 neural_network_runtime_type.h 里的枚举定义写下。
// 作用不是"看起来严谨", 而是: SDK 一旦改动这些取值, 这里直接编译失败,
// 而不是在真机上把 NPU 静默认成 CPU、把 OH_NN_UNAVAILABLE_DEVICE(14) 报成别的。
// ===========================================================================
// OH_NN_ReturnCode(type 头 144-191 行)
static_assert(OH_NN_SUCCESS == 0, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_FAILED == 1, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_INVALID_PARAMETER == 2, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_MEMORY_ERROR == 3, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_OPERATION_FORBIDDEN == 4, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_NULL_PTR == 5, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_INVALID_FILE == 6, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_UNAVALIDABLE_DEVICE == 7, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_INVALID_PATH == 8, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_TIMEOUT == 9, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_UNSUPPORTED == 10, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_CONNECTION_EXCEPTION == 11, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_SAVE_CACHE_EXCEPTION == 12, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_DYNAMIC_SHAPE == 13, "OH_NN_ReturnCode 与 SDK 头文件不一致");
static_assert(OH_NN_UNAVAILABLE_DEVICE == 14, "OH_NN_ReturnCode 与 SDK 头文件不一致");

// OH_NN_DeviceType(type 头 273-282 行)
static_assert(OH_NN_OTHERS == 0, "OH_NN_DeviceType 与 SDK 头文件不一致");
static_assert(OH_NN_CPU == 1, "OH_NN_DeviceType 与 SDK 头文件不一致");
static_assert(OH_NN_GPU == 2, "OH_NN_DeviceType 与 SDK 头文件不一致");
static_assert(OH_NN_ACCELERATOR == 3, "OH_NN_DeviceType 与 SDK 头文件不一致");

// OH_NN_DataType(type 头 290-317 行)
static_assert(OH_NN_UNKNOWN == 0, "OH_NN_DataType 与 SDK 头文件不一致");
static_assert(OH_NN_INT32 == 4, "OH_NN_DataType 与 SDK 头文件不一致");
static_assert(OH_NN_FLOAT16 == 10, "OH_NN_DataType 与 SDK 头文件不一致");
static_assert(OH_NN_FLOAT32 == 11, "OH_NN_DataType 与 SDK 头文件不一致");

// OH_NN_Format(type 头 254-265 行)
static_assert(OH_NN_FORMAT_NONE == 0, "OH_NN_Format 与 SDK 头文件不一致");
static_assert(OH_NN_FORMAT_NCHW == 1, "OH_NN_Format 与 SDK 头文件不一致");
static_assert(OH_NN_FORMAT_NHWC == 2, "OH_NN_Format 与 SDK 头文件不一致");
static_assert(OH_NN_FORMAT_ND == 3, "OH_NN_Format 与 SDK 头文件不一致");

// OH_NN_OperationType(type 头 326-2785 行)
static_assert(OH_NN_OPS_FULL_CONNECTION == 15, "OH_NN_OperationType 与 SDK 头文件不一致");
static_assert(OH_NN_OPS_MATMUL == 19, "OH_NN_OperationType 与 SDK 头文件不一致");
static_assert(OH_NN_OPS_RELU == 47, "OH_NN_OperationType 与 SDK 头文件不一致");

// OH_NN_FuseType(type 头 239-246 行)
static_assert(OH_NN_FUSED_NONE == 0, "OH_NN_FuseType 与 SDK 头文件不一致");
static_assert(OH_NN_FUSED_RELU == 1, "OH_NN_FuseType 与 SDK 头文件不一致");

// OH_NN_PerformanceMode / OH_NN_Priority(type 头 108-136 行)
static_assert(OH_NN_PERFORMANCE_EXTREME == 4, "OH_NN_PerformanceMode 与 SDK 头文件不一致");
static_assert(OH_NN_PRIORITY_HIGH == 3, "OH_NN_Priority 与 SDK 头文件不一致");
namespace {

constexpr const char* kLogTag = "AuroraNpu";
constexpr unsigned int kLogDomain = 0x1234;

// NNRt 头文件明确不支持多线程调用 —— 这一把锁把 probe/bench 串行化,
// 同时保证"首次绑定 NNRt"这件事是单线程的(绑定只做一次, 结果缓存, 见下面 ensureNnrtBound)。
std::mutex g_npuLock;

// ===========================================================================
// NNRt 运行时绑定(dlopen + dlsym)—— 为什么不硬链
//
// sysroot 里的 libneural_network_runtime.so / libneural_network_core.so 是链接桩
// (用 llvm-objdump 反汇编核实: 每个导出函数体只有一条 ret, 且没有任何 DT_NEEDED)。
// 如果按常规把它们写进 target_link_libraries, libaurorabench.so 就会带上对它们的
// 硬 DT_NEEDED; 一旦真机 /system/lib64 里没有这两个库(或者 soname 与桩不一致),
// 失效方式是 整个 requireNapi("aurorabench") 加载失败 —— 也就是说一个可选的
// NPU 小节会把 CS1 / CoreMark 全部拖死。这是最糟的失效方式, 不能接受。
//
// 本工程已有完全相同的先例: crash_guard.cpp 对 _Unwind_Backtrace 就是
// dlsym(RTLD_DEFAULT) + 降级, 理由一字不差 —— 可选功能不该有让主功能加载失败的能力。
// 这里照同一个原则办: NPU 小节自己 dlopen, 拿不到就报一个明确的"不可用"状态,
// 不牵连 App 的加载, 也不崩。
//
// 做法:
//   * 首次进入 probe / bench(普通上下文, 不在信号处理器里)时绑定一次, 结果缓存;
//   * 调用方必须已持有 g_npuLock, 所以绑定天然是单线程的, 不需要额外锁;
//   * 两个 dlopen 分别做: 只要有一个成功就继续试着绑符号(设备上万一两个库合并/改名,
//     也能靠"主库失败就试另一个句柄"兜住);
//   * 任何一个符号 dlsym 失败 -> 整体判定不可用, 并把缺失的符号名逐个写进结果;
//   * 句柄进程期内不 dlclose(后续调用还要用)。
// ===========================================================================
struct NnrtApi {
    // --- libneural_network_core.so: 设备 / 编译 / 执行器 / 张量(31 个) ---
    OH_NN_ReturnCode (*OH_NNDevice_GetAllDevicesID)(const size_t** allDevicesID, uint32_t* deviceCount);
    OH_NN_ReturnCode (*OH_NNDevice_GetName)(size_t deviceID, const char** name);
    OH_NN_ReturnCode (*OH_NNDevice_GetType)(size_t deviceID, OH_NN_DeviceType* deviceType);

    OH_NNCompilation* (*OH_NNCompilation_Construct)(const OH_NNModel* model);
    OH_NN_ReturnCode (*OH_NNCompilation_SetDevice)(OH_NNCompilation* compilation, size_t deviceID);
    OH_NN_ReturnCode (*OH_NNCompilation_SetPerformanceMode)(OH_NNCompilation* compilation,
                                                            OH_NN_PerformanceMode performanceMode);
    OH_NN_ReturnCode (*OH_NNCompilation_SetPriority)(OH_NNCompilation* compilation,
                                                     OH_NN_Priority priority);
    OH_NN_ReturnCode (*OH_NNCompilation_EnableFloat16)(OH_NNCompilation* compilation, bool enableFloat16);
    OH_NN_ReturnCode (*OH_NNCompilation_Build)(OH_NNCompilation* compilation);
    void (*OH_NNCompilation_Destroy)(OH_NNCompilation** compilation);

    OH_NNExecutor* (*OH_NNExecutor_Construct)(OH_NNCompilation* compilation);
    void (*OH_NNExecutor_Destroy)(OH_NNExecutor** executor);
    OH_NN_ReturnCode (*OH_NNExecutor_GetInputCount)(const OH_NNExecutor* executor, size_t* inputCount);
    OH_NN_ReturnCode (*OH_NNExecutor_GetOutputCount)(const OH_NNExecutor* executor, size_t* outputCount);
    NN_TensorDesc* (*OH_NNExecutor_CreateInputTensorDesc)(const OH_NNExecutor* executor, size_t index);
    NN_TensorDesc* (*OH_NNExecutor_CreateOutputTensorDesc)(const OH_NNExecutor* executor, size_t index);
    OH_NN_ReturnCode (*OH_NNExecutor_RunSync)(OH_NNExecutor* executor,
                                              NN_Tensor* inputTensor[],
                                              size_t inputCount,
                                              NN_Tensor* outputTensor[],
                                              size_t outputCount);

    NN_TensorDesc* (*OH_NNTensorDesc_Create)();
    OH_NN_ReturnCode (*OH_NNTensorDesc_Destroy)(NN_TensorDesc** tensorDesc);
    OH_NN_ReturnCode (*OH_NNTensorDesc_SetName)(NN_TensorDesc* tensorDesc, const char* name);
    OH_NN_ReturnCode (*OH_NNTensorDesc_SetDataType)(NN_TensorDesc* tensorDesc, OH_NN_DataType dataType);
    OH_NN_ReturnCode (*OH_NNTensorDesc_GetDataType)(const NN_TensorDesc* tensorDesc, OH_NN_DataType* dataType);
    OH_NN_ReturnCode (*OH_NNTensorDesc_SetShape)(NN_TensorDesc* tensorDesc, const int32_t* shape,
                                                 size_t shapeLength);
    OH_NN_ReturnCode (*OH_NNTensorDesc_SetFormat)(NN_TensorDesc* tensorDesc, OH_NN_Format format);
    OH_NN_ReturnCode (*OH_NNTensorDesc_GetFormat)(const NN_TensorDesc* tensorDesc, OH_NN_Format* format);
    OH_NN_ReturnCode (*OH_NNTensorDesc_GetElementCount)(const NN_TensorDesc* tensorDesc,
                                                        size_t* elementCount);

    NN_Tensor* (*OH_NNTensor_Create)(size_t deviceID, NN_TensorDesc* tensorDesc);
    OH_NN_ReturnCode (*OH_NNTensor_Destroy)(NN_Tensor** tensor);
    void* (*OH_NNTensor_GetDataBuffer)(const NN_Tensor* tensor);
    OH_NN_ReturnCode (*OH_NNTensor_GetSize)(const NN_Tensor* tensor, size_t* size);
    OH_NN_ReturnCode (*OH_NNTensor_GetOffset)(const NN_Tensor* tensor, size_t* offset);

    // --- libneural_network_runtime.so: 建模(7 个) ---
    OH_NNModel* (*OH_NNModel_Construct)();
    void (*OH_NNModel_Destroy)(OH_NNModel** model);
    OH_NN_ReturnCode (*OH_NNModel_AddTensorToModel)(OH_NNModel* model, const NN_TensorDesc* tensorDesc);
    OH_NN_ReturnCode (*OH_NNModel_AddOperation)(OH_NNModel* model,
                                                OH_NN_OperationType op,
                                                const OH_NN_UInt32Array* paramIndices,
                                                const OH_NN_UInt32Array* inputIndices,
                                                const OH_NN_UInt32Array* outputIndices);
    OH_NN_ReturnCode (*OH_NNModel_SetTensorData)(OH_NNModel* model, uint32_t index,
                                                 const void* dataBuffer, size_t length);
    OH_NN_ReturnCode (*OH_NNModel_SpecifyInputsAndOutputs)(OH_NNModel* model,
                                                           const OH_NN_UInt32Array* inputIndices,
                                                           const OH_NN_UInt32Array* outputIndices);
    OH_NN_ReturnCode (*OH_NNModel_Finish)(OH_NNModel* model);
};

// 全 0 = 还没绑定。整个文件里的 NNRt 调用一律写成 g_nnrt.OH_NNxxx(...)。
NnrtApi g_nnrt{};

struct NnrtBinding {
    bool tried = false;
    bool ok = false;                  // 38 个符号全部绑上才算可用
    bool runtimeOpened = false;       // libneural_network_runtime.so dlopen 成功
    bool coreOpened = false;          // libneural_network_core.so dlopen 成功
    std::string dlerrorRuntime;       // dlopen 失败时 dlerror() 的原文
    std::string dlerrorCore;
    std::string missingSymbols;       // dlsym 失败的符号名(逗号分隔)
    int boundCount = 0;               // 成功绑定个数
    int totalCount = 0;               // 期望绑定个数
    std::string error;                // 不可用原因(可直接显示)
};

NnrtBinding g_nnrtState;

constexpr const char* kNnrtRuntimeLib = "libneural_network_runtime.so";
constexpr const char* kNnrtCoreLib = "libneural_network_core.so";

// 把某个符号从主句柄绑到备句柄; 两个都拿不到就记进 missing。
void bindOne(void* primary, void* secondary, const char* name, void** out, std::string* missing)
{
    void* p = nullptr;
    if (primary != nullptr) {
        p = dlsym(primary, name);
    }
    if (p == nullptr && secondary != nullptr) {
        p = dlsym(secondary, name);
    }
    if (p == nullptr) {
        if (!missing->empty()) {
            *missing += ", ";
        }
        *missing += name;
        return;
    }
    *out = p;
    ++g_nnrtState.boundCount;
}

// 只做一次。调用方必须已持有 g_npuLock。
void ensureNnrtBound()
{
    if (g_nnrtState.tried) {
        return;
    }
    g_nnrtState.tried = true;
    // 期望绑定数先写死: 失败路径也要报出"应该有 38 个、实际绑上几个",
    // 否则失败结果里会留下 0/0 这种读不出信息的数字。
    g_nnrtState.totalCount = 38;

    void* hRuntime = dlopen(kNnrtRuntimeLib, RTLD_NOW | RTLD_LOCAL);
    if (hRuntime == nullptr) {
        const char* e = dlerror();
        g_nnrtState.dlerrorRuntime = (e != nullptr) ? std::string(e) : std::string("(dlerror 返回空)");
    } else {
        g_nnrtState.runtimeOpened = true;
    }

    void* hCore = dlopen(kNnrtCoreLib, RTLD_NOW | RTLD_LOCAL);
    if (hCore == nullptr) {
        const char* e = dlerror();
        g_nnrtState.dlerrorCore = (e != nullptr) ? std::string(e) : std::string("(dlerror 返回空)");
    } else {
        g_nnrtState.coreOpened = true;
    }

    if (hRuntime == nullptr && hCore == nullptr) {
        g_nnrtState.error = std::string("NNRt 库不可用: dlopen 失败: ") + kNnrtRuntimeLib + " -> " +
                            g_nnrtState.dlerrorRuntime + "; " + kNnrtCoreLib + " -> " +
                            g_nnrtState.dlerrorCore;
        return;
    }

    std::string missing;
    // 表里的 38 个函数: 先按"库归属"从主句柄取, 取不到再试另一个句柄 ——
    // 设备上万一两个库合并或改过 soname, 这样也能绑上, 而不是白白判定不可用。
    auto B = [&](void* primary, void* secondary, const char* name, void** out) {
        bindOne(primary, secondary, name, out, &missing);
    };
    B(hCore, hRuntime, "OH_NNDevice_GetAllDevicesID", reinterpret_cast<void**>(&g_nnrt.OH_NNDevice_GetAllDevicesID));
    B(hCore, hRuntime, "OH_NNDevice_GetName", reinterpret_cast<void**>(&g_nnrt.OH_NNDevice_GetName));
    B(hCore, hRuntime, "OH_NNDevice_GetType", reinterpret_cast<void**>(&g_nnrt.OH_NNDevice_GetType));
    B(hCore, hRuntime, "OH_NNCompilation_Construct", reinterpret_cast<void**>(&g_nnrt.OH_NNCompilation_Construct));
    B(hCore, hRuntime, "OH_NNCompilation_SetDevice", reinterpret_cast<void**>(&g_nnrt.OH_NNCompilation_SetDevice));
    B(hCore, hRuntime, "OH_NNCompilation_SetPerformanceMode", reinterpret_cast<void**>(&g_nnrt.OH_NNCompilation_SetPerformanceMode));
    B(hCore, hRuntime, "OH_NNCompilation_SetPriority", reinterpret_cast<void**>(&g_nnrt.OH_NNCompilation_SetPriority));
    B(hCore, hRuntime, "OH_NNCompilation_EnableFloat16", reinterpret_cast<void**>(&g_nnrt.OH_NNCompilation_EnableFloat16));
    B(hCore, hRuntime, "OH_NNCompilation_Build", reinterpret_cast<void**>(&g_nnrt.OH_NNCompilation_Build));
    B(hCore, hRuntime, "OH_NNCompilation_Destroy", reinterpret_cast<void**>(&g_nnrt.OH_NNCompilation_Destroy));
    B(hCore, hRuntime, "OH_NNExecutor_Construct", reinterpret_cast<void**>(&g_nnrt.OH_NNExecutor_Construct));
    B(hCore, hRuntime, "OH_NNExecutor_Destroy", reinterpret_cast<void**>(&g_nnrt.OH_NNExecutor_Destroy));
    B(hCore, hRuntime, "OH_NNExecutor_GetInputCount", reinterpret_cast<void**>(&g_nnrt.OH_NNExecutor_GetInputCount));
    B(hCore, hRuntime, "OH_NNExecutor_GetOutputCount", reinterpret_cast<void**>(&g_nnrt.OH_NNExecutor_GetOutputCount));
    B(hCore, hRuntime, "OH_NNExecutor_CreateInputTensorDesc", reinterpret_cast<void**>(&g_nnrt.OH_NNExecutor_CreateInputTensorDesc));
    B(hCore, hRuntime, "OH_NNExecutor_CreateOutputTensorDesc", reinterpret_cast<void**>(&g_nnrt.OH_NNExecutor_CreateOutputTensorDesc));
    B(hCore, hRuntime, "OH_NNExecutor_RunSync", reinterpret_cast<void**>(&g_nnrt.OH_NNExecutor_RunSync));
    B(hCore, hRuntime, "OH_NNTensorDesc_Create", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_Create));
    B(hCore, hRuntime, "OH_NNTensorDesc_Destroy", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_Destroy));
    B(hCore, hRuntime, "OH_NNTensorDesc_SetName", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_SetName));
    B(hCore, hRuntime, "OH_NNTensorDesc_SetDataType", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_SetDataType));
    B(hCore, hRuntime, "OH_NNTensorDesc_GetDataType", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_GetDataType));
    B(hCore, hRuntime, "OH_NNTensorDesc_SetShape", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_SetShape));
    B(hCore, hRuntime, "OH_NNTensorDesc_SetFormat", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_SetFormat));
    B(hCore, hRuntime, "OH_NNTensorDesc_GetFormat", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_GetFormat));
    B(hCore, hRuntime, "OH_NNTensorDesc_GetElementCount", reinterpret_cast<void**>(&g_nnrt.OH_NNTensorDesc_GetElementCount));
    B(hCore, hRuntime, "OH_NNTensor_Create", reinterpret_cast<void**>(&g_nnrt.OH_NNTensor_Create));
    B(hCore, hRuntime, "OH_NNTensor_Destroy", reinterpret_cast<void**>(&g_nnrt.OH_NNTensor_Destroy));
    B(hCore, hRuntime, "OH_NNTensor_GetDataBuffer", reinterpret_cast<void**>(&g_nnrt.OH_NNTensor_GetDataBuffer));
    B(hCore, hRuntime, "OH_NNTensor_GetSize", reinterpret_cast<void**>(&g_nnrt.OH_NNTensor_GetSize));
    B(hCore, hRuntime, "OH_NNTensor_GetOffset", reinterpret_cast<void**>(&g_nnrt.OH_NNTensor_GetOffset));
    B(hRuntime, hCore, "OH_NNModel_Construct", reinterpret_cast<void**>(&g_nnrt.OH_NNModel_Construct));
    B(hRuntime, hCore, "OH_NNModel_Destroy", reinterpret_cast<void**>(&g_nnrt.OH_NNModel_Destroy));
    B(hRuntime, hCore, "OH_NNModel_AddTensorToModel", reinterpret_cast<void**>(&g_nnrt.OH_NNModel_AddTensorToModel));
    B(hRuntime, hCore, "OH_NNModel_AddOperation", reinterpret_cast<void**>(&g_nnrt.OH_NNModel_AddOperation));
    B(hRuntime, hCore, "OH_NNModel_SetTensorData", reinterpret_cast<void**>(&g_nnrt.OH_NNModel_SetTensorData));
    B(hRuntime, hCore, "OH_NNModel_SpecifyInputsAndOutputs", reinterpret_cast<void**>(&g_nnrt.OH_NNModel_SpecifyInputsAndOutputs));
    B(hRuntime, hCore, "OH_NNModel_Finish", reinterpret_cast<void**>(&g_nnrt.OH_NNModel_Finish));

    g_nnrtState.missingSymbols = missing;
    if (!missing.empty()) {
        g_nnrtState.ok = false;
        g_nnrtState.error = "NNRt 库不可用: dlopen 成功但缺少符号: " + missing;
        return;
    }
    g_nnrtState.ok = true;
}

// ---------------------------------------------------------------------------
// 小工具: JSON 输出
// ---------------------------------------------------------------------------
std::string jsonEscape(const std::string& s)
{
    // 与 napi_init.cpp 里的 escapeJson 同一口径: JSON 里不允许裸的 0x00-0x1F
    // (NNRt 设备名理论上不会带控制字符, 但厂商驱动返回什么都有可能)。
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            // 注意: 这里是"往 JSON 文本里写转义序列", 所以 C++ 字面量必须是双反斜杠 ——
            // 写成 "\"" 只会写进去一个裸引号, 结果是非法 JSON(而且正好砸在所有带引号的
            // 失败说明上)。口径与 napi_init.cpp 的 escapeJson 完全一致。
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case 0x08: out += "\\b"; break;
            case 0x0c: out += "\\f"; break;
            case 0x0a: out += "\\n"; break;
            case 0x0d: out += "\\r"; break;
            case 0x09: out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    (void)snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                    out += esc;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

std::string jstr(const std::string& v) { return std::string(1, '"') + jsonEscape(v) + '"'; }

std::string ji(long long v)
{
    char b[32];
    (void)snprintf(b, sizeof(b), "%lld", v);
    return std::string(b);
}

std::string ju64(unsigned long long v)
{
    char b[32];
    (void)snprintf(b, sizeof(b), "%llu", v);
    return std::string(b);
}

// 浮点: 非有限值(NaN/inf)不能进 JSON(NaN 不是合法 JSON 字面量, ArkTS 的
// JSON.parse 会直接抛异常)。这里统一兜成 0, 并由调用方把 nonFinite 标出来。
bool jfinite(double v) { return std::isfinite(v); }

std::string jf(double v, int prec)
{
    if (!jfinite(v)) {
        return std::string("0");
    }
    char b[64];
    (void)snprintf(b, sizeof(b), "%.*f", prec, v);
    return std::string(b);
}

std::string jbool(bool v) { return v ? std::string("true") : std::string("false"); }

// ---------------------------------------------------------------------------
// 枚举 -> 文本
//
// 关键点: 数值一律用 (int)枚举 现算后 snprintf 打进字符串, 不是我手抄的字面量。
// 所以不可能出现"名字写着 OH_NN_UNAVAILABLE_DEVICE 数字却是 6"这种对不上的情况。
// ---------------------------------------------------------------------------
std::string enumText(const char* name, int value)
{
    char b[96];
    (void)snprintf(b, sizeof(b), "%s(%d)", name, value);
    return std::string(b);
}

std::string rcText(OH_NN_ReturnCode rc)
{
    const char* n = "OH_NN_<本 SDK 未定义的返回码>";
    switch (rc) {
        case OH_NN_SUCCESS: n = "OH_NN_SUCCESS"; break;
        case OH_NN_FAILED: n = "OH_NN_FAILED"; break;
        case OH_NN_INVALID_PARAMETER: n = "OH_NN_INVALID_PARAMETER"; break;
        case OH_NN_MEMORY_ERROR: n = "OH_NN_MEMORY_ERROR"; break;
        case OH_NN_OPERATION_FORBIDDEN: n = "OH_NN_OPERATION_FORBIDDEN"; break;
        case OH_NN_NULL_PTR: n = "OH_NN_NULL_PTR"; break;
        case OH_NN_INVALID_FILE: n = "OH_NN_INVALID_FILE"; break;
        case OH_NN_UNAVALIDABLE_DEVICE: n = "OH_NN_UNAVALIDABLE_DEVICE"; break;
        case OH_NN_INVALID_PATH: n = "OH_NN_INVALID_PATH"; break;
        case OH_NN_TIMEOUT: n = "OH_NN_TIMEOUT"; break;
        case OH_NN_UNSUPPORTED: n = "OH_NN_UNSUPPORTED"; break;
        case OH_NN_CONNECTION_EXCEPTION: n = "OH_NN_CONNECTION_EXCEPTION"; break;
        case OH_NN_SAVE_CACHE_EXCEPTION: n = "OH_NN_SAVE_CACHE_EXCEPTION"; break;
        case OH_NN_DYNAMIC_SHAPE: n = "OH_NN_DYNAMIC_SHAPE"; break;
        case OH_NN_UNAVAILABLE_DEVICE: n = "OH_NN_UNAVAILABLE_DEVICE"; break;
        default: break;
    }
    return enumText(n, (int)rc);
}

std::string devTypeName(OH_NN_DeviceType t)
{
    switch (t) {
        case OH_NN_CPU: return "CPU";
        case OH_NN_GPU: return "GPU";
        case OH_NN_ACCELERATOR: return "ACCELERATOR";
        case OH_NN_OTHERS: return "OTHERS";
        default: return "UNKNOWN";
    }
}

std::string devTypeEnumText(OH_NN_DeviceType t)
{
    const char* n = "OH_NN_<本 SDK 未定义的设备类型>";
    switch (t) {
        case OH_NN_CPU: n = "OH_NN_CPU"; break;
        case OH_NN_GPU: n = "OH_NN_GPU"; break;
        case OH_NN_ACCELERATOR: n = "OH_NN_ACCELERATOR"; break;
        case OH_NN_OTHERS: n = "OH_NN_OTHERS"; break;
        default: break;
    }
    return enumText(n, (int)t);
}

std::string dtEnumText(OH_NN_DataType d)
{
    const char* n = "OH_NN_<本 SDK 未定义的数据类型>";
    switch (d) {
        case OH_NN_UNKNOWN: n = "OH_NN_UNKNOWN"; break;
        case OH_NN_BOOL: n = "OH_NN_BOOL"; break;
        case OH_NN_INT8: n = "OH_NN_INT8"; break;
        case OH_NN_INT16: n = "OH_NN_INT16"; break;
        case OH_NN_INT32: n = "OH_NN_INT32"; break;
        case OH_NN_INT64: n = "OH_NN_INT64"; break;
        case OH_NN_UINT8: n = "OH_NN_UINT8"; break;
        case OH_NN_UINT16: n = "OH_NN_UINT16"; break;
        case OH_NN_UINT32: n = "OH_NN_UINT32"; break;
        case OH_NN_UINT64: n = "OH_NN_UINT64"; break;
        case OH_NN_FLOAT16: n = "OH_NN_FLOAT16"; break;
        case OH_NN_FLOAT32: n = "OH_NN_FLOAT32"; break;
        case OH_NN_FLOAT64: n = "OH_NN_FLOAT64"; break;
        default: break;
    }
    return enumText(n, (int)d);
}

std::string fmtEnumText(OH_NN_Format f)
{
    const char* n = "OH_NN_<本 SDK 未定义的布局>";
    switch (f) {
        case OH_NN_FORMAT_NONE: n = "OH_NN_FORMAT_NONE"; break;
        case OH_NN_FORMAT_NCHW: n = "OH_NN_FORMAT_NCHW"; break;
        case OH_NN_FORMAT_NHWC: n = "OH_NN_FORMAT_NHWC"; break;
        case OH_NN_FORMAT_ND: n = "OH_NN_FORMAT_ND"; break;
        default: break;
    }
    return enumText(n, (int)f);
}

// 单个元素占几字节(用于输入数据按执行器实际类型写入 + 输出字节数核对)
size_t dtypeBytes(OH_NN_DataType d)
{
    switch (d) {
        case OH_NN_BOOL: return 1;
        case OH_NN_INT8: case OH_NN_UINT8: return 1;
        case OH_NN_INT16: case OH_NN_UINT16: case OH_NN_FLOAT16: return 2;
        case OH_NN_INT32: case OH_NN_UINT32: case OH_NN_FLOAT32: return 4;
        case OH_NN_INT64: case OH_NN_UINT64: case OH_NN_FLOAT64: return 8;
        default: return 0;
    }
}

std::string fmtText(OH_NN_Format f) { return fmtEnumText(f); }

// ---------------------------------------------------------------------------
// 设备枚举(probe 与 bench 共用)
// ---------------------------------------------------------------------------
struct DeviceInfo {
    size_t id = 0;
    std::string name;
    OH_NN_ReturnCode nameRc = OH_NN_SUCCESS;
    OH_NN_DeviceType type = OH_NN_OTHERS;
    OH_NN_ReturnCode typeRc = OH_NN_SUCCESS;
};

struct DeviceEnum {
    OH_NN_ReturnCode rc = OH_NN_SUCCESS;   // OH_NNDevice_GetAllDevicesID 的返回码
    uint32_t count = 0;                    // NNRt 报告的设备数
    std::vector<size_t> ids;               // 拷贝一份(见下面注释)
    std::vector<DeviceInfo> devices;
    std::string note;
};

// 逐个核对过的签名(neural_network_core.h):
//   OH_NN_ReturnCode OH_NNDevice_GetAllDevicesID(const size_t **allDevicesID, uint32_t *deviceCount);   // 1141 行
//   OH_NN_ReturnCode OH_NNDevice_GetName(size_t deviceID, const char **name);                           // 1165 行
//   OH_NN_ReturnCode OH_NNDevice_GetType(size_t deviceID, OH_NN_DeviceType *deviceType);                // 1188 行
// 两条硬性约束(头文件 1131/1150 行写明, 违反会直接返回 OH_NN_INVALID_PARAMETER):
//   * *allDevicesID 传进去时必须是 nullptr;
//   * *name 传进去时必须是 nullptr。
// 另外 allDevicesID 指向的内存由 NNRt 内部管理, "下次调用本函数前有效" —— 所以立刻
// 拷进 std::vector, 并且不 free。
DeviceEnum enumerateDevices()
{
    DeviceEnum e;
    const size_t* ids = nullptr;
    uint32_t count = 0;
    e.rc = g_nnrt.OH_NNDevice_GetAllDevicesID(&ids, &count);
    if (e.rc != OH_NN_SUCCESS) {
        e.note = "OH_NNDevice_GetAllDevicesID 返回 " + rcText(e.rc);
        return e;
    }
    e.count = count;
    if (ids == nullptr || count == 0) {
        e.note = "OH_NNDevice_GetAllDevicesID 成功, 但设备数为 0(这台机器上没有可用的 NNRt 后端)";
        return e;
    }
    e.ids.assign(ids, ids + count);
    for (uint32_t i = 0; i < count; ++i) {
        DeviceInfo d;
        d.id = e.ids[i];
        const char* nm = nullptr;
        d.nameRc = g_nnrt.OH_NNDevice_GetName(d.id, &nm);
        if (d.nameRc == OH_NN_SUCCESS && nm != nullptr) {
            d.name.assign(nm);
        }
        // 任何一步失败都保留失败信息, 但继续枚举剩下的设备(不能因为一个设备取名字失败
        // 就漏掉后面可能存在的 ACCELERATOR)。
        d.typeRc = g_nnrt.OH_NNDevice_GetType(d.id, &d.type);
        if (d.typeRc != OH_NN_SUCCESS) {
            d.type = OH_NN_OTHERS;
        }
        e.devices.push_back(d);
    }
    return e;
}

// 把一个设备数组写成 JSON 片段(probe 与 bench 共用同一形状)
void appendDevicesJson(std::string& out, const DeviceEnum& e)
{
    out += "[";
    for (size_t i = 0; i < e.devices.size(); ++i) {
        const DeviceInfo& d = e.devices[i];
        if (i != 0) {
            out += ",";
        }
        out += "{\"id\":" + ju64((unsigned long long)d.id);
        out += ",\"name\":" + jstr(d.name);
        out += ",\"nameRc\":" + ji((long long)d.nameRc);
        out += ",\"nameRcName\":" + jstr(rcText(d.nameRc));
        out += ",\"type\":" + ji((long long)d.type);
        out += ",\"typeName\":" + jstr(devTypeName(d.type));
        out += ",\"typeEnum\":" + jstr(devTypeEnumText(d.type));
        out += ",\"typeRc\":" + ji((long long)d.typeRc);
        out += ",\"typeRcName\":" + jstr(rcText(d.typeRc));
        out += "}";
    }
    out += "]";
}

// 把绑定结果写成 JSON(probe 与 bench 共用), 缺什么符号一目了然。
void appendNnrtJson(std::string& out)
{
    const NnrtBinding& s = g_nnrtState;
    out += "{\"available\":" + jbool(s.ok);
    out += ",\"binding\":\"dlopen + dlsym 运行时绑定(不硬链, 见 npu_bench.cpp 顶部说明)\"";
    out += ",\"runtimeLibrary\":" + jstr(std::string(kNnrtRuntimeLib));
    out += ",\"runtimeOpened\":" + jbool(s.runtimeOpened);
    out += ",\"runtimeDlerror\":" + jstr(s.dlerrorRuntime);
    out += ",\"coreLibrary\":" + jstr(std::string(kNnrtCoreLib));
    out += ",\"coreOpened\":" + jbool(s.coreOpened);
    out += ",\"coreDlerror\":" + jstr(s.dlerrorCore);
    out += ",\"expectedFunctionCount\":" + ji(s.totalCount);
    out += ",\"boundFunctionCount\":" + ji(s.boundCount);
    out += ",\"missingSymbols\":" + jstr(s.missingSymbols);
    if (s.error.empty() && !s.tried) {
        // 还没尝试过绑定就出了异常(例如 napi 层捕获到的 C++ 异常走 failureJson)。
        // 说"没试过", 不要留下一个空 error 让人误读成"绑定成功但没有错误"。
        out += ",\"error\":\"(尚未尝试绑定: 本次在绑定之前就异常退出了)\"";
    } else {
        out += ",\"error\":" + jstr(s.error);
    }
    out += "}";
}

const char* const kNotes =
    "本小节是自建模型(纯代码构造的 MATMUL 链 + RELU), 不是厂商模型库里的标准模型, "
    "因此 GOPs/s 只在同一版本的本 App 内可比(跨机型对比要连同 model.dim/layers 一起看)。"
    "它不参与 GB7 计分, 也不应被换算成任何第三方的分数。"
    "设备类型是这一节最重要的元信息: 若 device.typeName 不是 ACCELERATOR, "
    "说明这台机器没有向第三方应用开放 NPU, 数字来自 GPU 或 CPU —— 请照实显示。";

// ---------------------------------------------------------------------------
// 第一步: 设备探测
// ---------------------------------------------------------------------------
std::string probeJsonImpl()
{
    // 先确认 NNRt 能被运行时绑定(见文件头)。绑定不上时不能说成"这台机器没有 NPU" ——
    // 那是另一回事: 这里只能说"本 App 拿不到 NNRt 接口", 并给出 dlerror 原文/缺失符号。
    if (!g_nnrtState.ok) {
        std::string out;
        out.reserve(1024);
        out += "{\"ok\":false";
        out += ",\"kind\":\"npu-probe\"";
        out += ",\"status\":\"NNRT_UNAVAILABLE\"";
        out += ",\"thirdPartyNpuOpen\":false";
        out += ",\"text\":" + jstr(
            "NNRt 库不可用: " + g_nnrtState.error +
            " —— 这不是「这台机器没有 NPU」的结论, 只是本 App 拿不到 NNRt 接口; "
            "该小节已按不可用处理, 不影响其它跑分项。");
        out += ",\"error\":" + jstr(g_nnrtState.error);
        out += ",\"enumRc\":-1";
        out += ",\"enumRcName\":\"(未调用: NNRt 不可用)\"";
        out += ",\"deviceCount\":0";
        out += ",\"nnrtReportedCount\":0";
        out += ",\"devices\":[]";
        out += ",\"hasCpu\":false,\"hasGpu\":false,\"hasAccelerator\":false";
        out += ",\"enumNote\":" + jstr(g_nnrtState.error);
        out += ",\"nnrt\":";
        appendNnrtJson(out);
        out += ",\"notes\":" + jstr(std::string(kNotes));
        out += "}";
        return out;
    }
    DeviceEnum e = enumerateDevices();

    bool hasAcc = false;
    bool hasGpu = false;
    bool hasCpu = false;
    for (const DeviceInfo& d : e.devices) {
        if (d.typeRc != OH_NN_SUCCESS) {
            continue;   // 取不到类型的设备不计入"有/没有"的判断(它已在 devices[] 里列出)
        }
        if (d.type == OH_NN_ACCELERATOR) {
            hasAcc = true;
        } else if (d.type == OH_NN_GPU) {
            hasGpu = true;
        } else if (d.type == OH_NN_CPU) {
            hasCpu = true;
        }
    }

    std::string status;
    std::string text;
    if (e.rc != OH_NN_SUCCESS) {
        status = "ENUM_FAILED";
        text = "NPU 探测失败: " + rcText(e.rc) +
               " —— 设备枚举接口本身失败, 因此无法判断这台机器有没有可用的 NPU。";
    } else if (e.devices.empty()) {
        status = "NO_DEVICE";
        text = "未枚举到任何 NNRt 设备(deviceCount=0): 这台机器上没有可用的 NNRt 后端, "
               "因此也无从谈起 NPU。";
    } else if (hasAcc) {
        status = "OPEN";
        unsigned long long accCount = 0;
        for (const DeviceInfo& d : e.devices) {
            if (d.typeRc == OH_NN_SUCCESS && d.type == OH_NN_ACCELERATOR) {
                ++accCount;
            }
        }
        text = "已向第三方应用开放 NPU: 枚举到 " + ju64((unsigned long long)e.devices.size()) +
               " 个 NNRt 设备, 其中 " + ju64(accCount) +
               " 个是 OH_NN_ACCELERATOR(专用硬件加速器 / NPU)。";
    } else {
        status = "NOT_OPEN";
        text = "未向第三方开放 NPU: 枚举到 " + ju64((unsigned long long)e.devices.size()) +
               " 个 NNRt 设备, 类型只有" +
               std::string(hasCpu ? " CPU" : "") + std::string(hasGpu ? " GPU" : "") +
               ", 没有 OH_NN_ACCELERATOR(3)。也就是说这台机器要么没有 NPU, "
               "要么 NPU 没有通过 NNRt 向第三方应用开放 —— 本 App 只能报这个结论, "
               "不会把它当成 NPU 分数。";
    }

    std::string out;
    out.reserve(2048);
    out += "{\"ok\":" + jbool(e.rc == OH_NN_SUCCESS);
    out += ",\"kind\":\"npu-probe\"";
    out += ",\"status\":" + jstr(status);
    out += ",\"thirdPartyNpuOpen\":" + jbool(hasAcc);
    out += ",\"text\":" + jstr(text);
    out += ",\"error\":";
    if (e.rc != OH_NN_SUCCESS) {
        out += jstr("NPU 探测失败: " + rcText(e.rc));
    } else {
        out += "\"\"";
    }
    out += ",\"enumRc\":" + ji((long long)e.rc);
    out += ",\"enumRcName\":" + jstr(rcText(e.rc));
    out += ",\"deviceCount\":" + ju64((unsigned long long)e.devices.size());
    out += ",\"nnrtReportedCount\":" + ju64((unsigned long long)e.count);
    out += ",\"devices\":";
    appendDevicesJson(out, e);
    out += ",\"hasCpu\":" + jbool(hasCpu);
    out += ",\"hasGpu\":" + jbool(hasGpu);
    out += ",\"hasAccelerator\":" + jbool(hasAcc);
    out += ",\"enumNote\":" + jstr(e.note);
    out += ",\"nnrt\":";
    appendNnrtJson(out);
    out += ",\"notes\":" + jstr(std::string(kNotes) +
        " 探测口径: OH_NNDevice_GetAllDevicesID 枚举 -> 逐个 OH_NNDevice_GetName / "
        "OH_NNDevice_GetType; 每个设备的取名字/取类型返回码都单独保留在 devices[] 里。");
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------
// 第二步: 推理跑分
//
// ---- 模型与乘加次数的推导(必须能算清楚, 不许只报一个好看的数字) ----
// 模型 = L 层 MATMUL 串成的链, 层与层之间插一个 RELU:
//
//      x0 --MATMUL(W0)--> y0 --RELU--> a0 --MATMUL(W1)--> y1 --RELU--> a1 ... --> yL-1(=输出)
//
// 每个张量都是 [N, N] 的 FLOAT32 二维矩阵, 每层权重 W_i 是 [N, N] 常量张量。
// 一次 MATMUL 做的就是 N x N 的矩阵乘: 输出每个元素要 N 次乘加, 共 N*N 个输出元素,
// 所以
//     MACs(一次 MATMUL) = N * N * N = N^3
//     MACs(整个模型)    = L * N^3
//     FLOPs             = 2 * MACs          (一次"乘加"= 一次乘法 + 一次加法)
// RELU 只做比较, 每层 N*N 次, 相对 N^3 可忽略(结果里单独列出 op 个数, 不混进 MACs)。
// 举例(默认档 scale=1): N=256, L=6
//     MACs  = 6 * 256^3 = 6 * 16 777 216 = 100 663 296
//     FLOPs = 201 326 592
// 吞吐 GOPs/s = FLOPs / (稳态平均单次延迟秒数) / 1e9
//
// ---- 为什么用 MATMUL 而不是卷积 ----
// 卷积对输入布局(NCHW/NHWC)、padding/stride/dilation 属性和 kernel 张量形状都有额外要求,
// 不同后端接受度差别很大; MATMUL 只有两个输入、零参数、语义最死, 是最不容易被后端拒绝的
// 大算力算子。RELU 同理(单输入单输出、零参数)。
// ---------------------------------------------------------------------------

struct ScalePreset {
    int scale;
    uint32_t dim;
    uint32_t layers;
    const char* label;
    const char* macsText;
};

// 规模档位 —— 目的就是让单次推理落在 10~100 ms 量级(见 timing.hint)。
const ScalePreset kPresets[] = {
    {0, 128,  4, "小(128x128 x 4 层 MatMul)",   "8.39M MACs/次"},
    {1, 256,  6, "中(256x256 x 6 层 MatMul)",   "100.7M MACs/次(默认)"},
    {2, 256, 16, "大(256x256 x 16 层 MatMul)",  "268.4M MACs/次"},
    {3, 384,  8, "特大(384x384 x 8 层 MatMul)", "453.0M MACs/次"},
};
constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

void appendScalePresetsJson(std::string& out)
{
    out += "[";
    for (size_t i = 0; i < kPresetCount; ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "{\"scale\":" + ji(kPresets[i].scale);
        out += ",\"dim\":" + ju64(kPresets[i].dim);
        out += ",\"layers\":" + ju64(kPresets[i].layers);
        out += ",\"label\":" + jstr(kPresets[i].label);
        out += ",\"macs\":" + jstr(kPresets[i].macsText);
        out += "}";
    }
    out += "]";
}

const ScalePreset& presetFor(int scale)
{
    for (size_t i = 0; i < kPresetCount; ++i) {
        if (kPresets[i].scale == scale) {
            return kPresets[i];
        }
    }
    return kPresets[1];   // 越界一律按默认档, 且结果里会原样写出实际用的 dim/layers
}

// ---------------------------------------------------------------------------
// 算子组(operator set)
//
// 主用 MATMUL 链。若某个后端不接受 MATMUL(头文件 249 行允许"不写算子参数"的写法,
// 但后端是否真的接受 2 输入 0 参数 的 MatMul 是厂商实现的事), 就整条设备阶梯再走一遍
// FULL_CONNECTION 链 —— 它和 MATMUL 是同一族的矩阵乘算子, 但输入个数不同(3 个: 输入/权重/偏置),
// 有些后端只实现了这一种。
//
// 两组算子的乘加次数口径完全一样: 对 [dim,dim] 的输入 / [dim,dim] 的权重,
//   一次 MATMUL         : 输出 dim*dim 个元素, 每个元素 dim 次乘加 -> dim^3 MACs
//   一次 FULL_CONNECTION: 输出通道数 dim, 每个输出元素 = 输入的 dim 个分量线性组合
//                         -> dim(输入元素) * dim(输出通道) = dim^3 MACs
// 所以不管最终用的是哪一组, MACs = layers * dim^3 这个式子都成立, GOPs/s 可直接对比。
//
// 【重要: 无法在本机验证的部分】FULL_CONNECTION 的轴/布局语义(是否有 axis 参数、
// 权重是 [out,in] 还是 [in,out])由厂商后端决定, 头文件只描述"输入/权重/偏置 -> 输出"而不给
// 形状推导公式。因此本文件对 FULL_CONNECTION 组做了两道运行时自检(输入元素个数、输出元素个数
// 都必须等于 dim*dim), 任何不符立即报失败而不是继续测; 结果里 model.opSetVerifiedOnHost 会
// 明确写成 false, 提醒读者这一组的算子语义未经真机验证。
// ---------------------------------------------------------------------------
constexpr int kOpSetMatMul = 0;
constexpr int kOpSetFullConnection = 1;

// 尝试次数硬上限(算子组 x 设备的笛卡尔积最多做这么多次)。失败的那几次都会在
// 建模/编译阶段就返回(毫秒级), 所以上限存在的意义是保证总时长可控, 而不是拖慢成功路径。
constexpr size_t kMaxAttempts = 8;

struct OpSetDef {
    int id;
    const char* name;
    const char* desc;
    const char* mainOp;
};

const OpSetDef kOpSets[] = {
    {kOpSetMatMul, "MATMUL+RELU",
     "OhNnOpsMatMul 链: 输入 [dim,dim] x 权重 [dim,dim](常量), 层间插 OhNnOpsRelu; "
     "两组输入、零算子参数, 语义最死, 首选",
     "OH_NN_OPS_MATMUL"},
    {kOpSetFullConnection, "FULL_CONNECTION+RELU",
     "OhNnOpsFullConnection 链: 输入 [dim,dim] / 权重 [dim,dim](常量) / 偏置 [dim](常量 0), "
     "层间插 OhNnOpsRelu; 三组输入、零算子参数, 用于 MATMUL 组被后端拒绝时的回退",
     "OH_NN_OPS_FULL_CONNECTION"},
};
constexpr size_t kOpSetCount = sizeof(kOpSets) / sizeof(kOpSets[0]);

const OpSetDef& opSetDef(int id)
{
    for (size_t i = 0; i < kOpSetCount; ++i) {
        if (kOpSets[i].id == id) {
            return kOpSets[i];
        }
    }
    return kOpSets[0];
}

void appendOpSetsJson(std::string& out)
{
    out += "[";
    for (size_t i = 0; i < kOpSetCount; ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "{\"id\":" + ji(kOpSets[i].id);
        out += ",\"name\":" + jstr(kOpSets[i].name);
        out += ",\"desc\":" + jstr(kOpSets[i].desc);
        out += ",\"mainOp\":" + jstr(kOpSets[i].mainOp);
        out += "}";
    }
    out += "]";
}

// float32 -> IEEE-754 binary16 的位模式(自己写, 不引任何第三方库)。
// 只在"执行器把输入张量声明成 FP16"时才会用到 —— 那种情况下必须按 FP16 写数据,
// 否则就是往共享内存里灌错格式的字节。
uint16_t f32ToF16(float f)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) {
            return (uint16_t)sign;              // 小到连次正规数都放不下 -> 0
        }
        const uint32_t withHidden = man | 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half = withHidden >> shift;
        if (((withHidden >> (shift - 1)) & 1u) != 0u) {
            ++half;                              // 就近舍入
        }
        return (uint16_t)(sign | half);
    }
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7C00u);       // 溢出 -> inf
    }
    uint32_t half = ((uint32_t)exp << 10) | (man >> 13);
    if (((man >> 12) & 1u) != 0u) {
        ++half;
    }
    return (uint16_t)(sign | half);
}

// binary16 -> float32(读输出张量做合理性检查时用)
float f16ToF32(uint16_t h)
{
    const uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    const uint32_t exp = ((uint32_t)h >> 10) & 0x1Fu;
    const uint32_t man = (uint32_t)h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            int e = -1;
            uint32_t m = man;
            do {
                ++e;
                m <<= 1;
            } while ((m & 0x400u) == 0u);
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

double msBetween(const std::chrono::steady_clock::time_point& a,
                 const std::chrono::steady_clock::time_point& b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// 一次"在某台设备上跑完整流程"的全部资源。用析构函数兜底释放:
// 这样任何一条失败分支 return 出去都不会泄漏 NNRt 对象(失败路径最容易漏 free)。
//
// 释放顺序(有意为之):
//   张量 -> 执行器自己给的张量描述 -> 执行器 -> 编译实例 -> 模型 -> 建模型时创建的张量描述。
// 建模型用的 NN_TensorDesc 放在模型之后销毁: 头文件没有承诺 AddTensorToModel 会拷贝
// 描述对象(只承诺 NN_Tensor_Create 会拷贝), 放在模型之后销毁在任何一种实现下都安全。
struct AttemptResources {
    OH_NNModel* model = nullptr;
    std::vector<NN_TensorDesc*> modelDescs;
    OH_NNCompilation* comp = nullptr;
    OH_NNExecutor* exec = nullptr;
    NN_TensorDesc* inDesc = nullptr;
    NN_TensorDesc* outDesc = nullptr;
    NN_Tensor* inTensor = nullptr;
    NN_Tensor* outTensor = nullptr;

    // 每个调用前都判一次函数指针非空: 能走到这里说明绑定已经成功过, 但析构函数
    // 不应该假设任何外部状态 —— 空指针判断的代价是零, 而漏掉它的代价是崩溃。
    ~AttemptResources()
    {
        if (inTensor != nullptr && g_nnrt.OH_NNTensor_Destroy != nullptr) {
            (void)g_nnrt.OH_NNTensor_Destroy(&inTensor);
        }
        if (outTensor != nullptr && g_nnrt.OH_NNTensor_Destroy != nullptr) {
            (void)g_nnrt.OH_NNTensor_Destroy(&outTensor);
        }
        if (inDesc != nullptr && g_nnrt.OH_NNTensorDesc_Destroy != nullptr) {
            (void)g_nnrt.OH_NNTensorDesc_Destroy(&inDesc);
        }
        if (outDesc != nullptr && g_nnrt.OH_NNTensorDesc_Destroy != nullptr) {
            (void)g_nnrt.OH_NNTensorDesc_Destroy(&outDesc);
        }
        if (exec != nullptr && g_nnrt.OH_NNExecutor_Destroy != nullptr) {
            g_nnrt.OH_NNExecutor_Destroy(&exec);
        }
        if (comp != nullptr && g_nnrt.OH_NNCompilation_Destroy != nullptr) {
            g_nnrt.OH_NNCompilation_Destroy(&comp);
        }
        if (model != nullptr && g_nnrt.OH_NNModel_Destroy != nullptr) {
            g_nnrt.OH_NNModel_Destroy(&model);
        }
        for (NN_TensorDesc* d : modelDescs) {
            if (g_nnrt.OH_NNTensorDesc_Destroy != nullptr) {
                (void)g_nnrt.OH_NNTensorDesc_Destroy(&d);
            }
        }
    }

    AttemptResources() = default;
    AttemptResources(const AttemptResources&) = delete;
    AttemptResources& operator=(const AttemptResources&) = delete;
};

// 一次设备尝试的完整记录。所有字段都会进 JSON —— 包括每一次调用的返回码。
struct Attempt {
    size_t deviceId = 0;
    int opSet = 0;                 // 本次尝试用的算子组
    std::string opSetName;
    std::string mainOp;            // 本组的主算子枚举名
    std::string typeName;
    std::string typeEnum;
    std::string deviceName;
    std::string deviceNameRc;

    bool ok = false;
    std::string failStage;                 // 失败发生在哪一步(中文, 可直接显示)
    OH_NN_ReturnCode failRc = OH_NN_SUCCESS;
    bool failRcIsOurs = false;             // true = 失败来自本 App 自检, 不是 NNRt 返回码
    std::string failOursText;
    std::string rcTrail;                   // 一路上每一次调用的返回码(名字+数值)

    // 成功时才有意义 / 失败时保持 0
    double buildMs = 0.0;
    double firstRunMs = 0.0;
    int warmupDone = 0;
    bool warmupShortened = false;
    int runsDone = 0;
    bool budgetHit = false;
    bool sampleFromFirstRun = false;
    double meanMs = 0.0;
    double minMs = 0.0;
    double medianMs = 0.0;
    double maxMs = 0.0;
    double goPs = 0.0;
    double macsPerSec = 0.0;
    bool nonFinite = false;
    std::string hint;

    std::string fp16RcText;
    std::string fp16RcText2;
    bool fp16Accepted = false;
    std::string perfRcText;
    std::string prioRcText;
    std::string execInputDt;
    std::string execInputDtRc;
    std::string execInputFmt;
    std::string execInputFmtRc;
    std::string execOutputDt;
    std::string execOutputDtRc;
    std::string outputNote;
    bool outputSane = false;
    uint64_t macs = 0;
    uint64_t flops = 0;
    size_t weightBytes = 0;
    size_t inputBytes = 0;
    int matmulOps = 0;
    int reluOps = 0;
};

// ---------------------------------------------------------------------------
// 在一台指定设备上跑完整流程: 建模 -> 编译 -> 执行器 -> 张量 -> 预热 -> 计时
//
// 逐条核对过的签名(neural_network_runtime.h / neural_network_core.h):
//   OH_NNModel        *OH_NNModel_Construct(void);                                                  runtime.h 151
//   OH_NN_ReturnCode   OH_NNModel_AddTensorToModel(OH_NNModel*, const NN_TensorDesc*);              runtime.h 177
//   OH_NN_ReturnCode   OH_NNModel_AddOperation(OH_NNModel*, OH_NN_OperationType,
//                          const OH_NN_UInt32Array* param, const OH_NN_UInt32Array* in,
//                          const OH_NN_UInt32Array* out);                                          runtime.h 267
//   OH_NN_ReturnCode   OH_NNModel_SetTensorData(OH_NNModel*, uint32_t index,
//                          const void* dataBuffer, size_t length);                                 runtime.h 199
//   OH_NN_ReturnCode   OH_NNModel_SpecifyInputsAndOutputs(OH_NNModel*,
//                          const OH_NN_UInt32Array* in, const OH_NN_UInt32Array* out);              runtime.h 296
//   OH_NN_ReturnCode   OH_NNModel_Finish(OH_NNModel*);                                              runtime.h 323
//   OH_NNCompilation  *OH_NNCompilation_Construct(const OH_NNModel*);                               core.h     74
//   OH_NN_ReturnCode   OH_NNCompilation_SetDevice(OH_NNCompilation*, size_t deviceID);              core.h    227
//   OH_NN_ReturnCode   OH_NNCompilation_EnableFloat16(OH_NNCompilation*, bool);                     core.h    332
//   OH_NN_ReturnCode   OH_NNCompilation_SetPerformanceMode(OH_NNCompilation*, OH_NN_PerformanceMode); core.h  287
//   OH_NN_ReturnCode   OH_NNCompilation_SetPriority(OH_NNCompilation*, OH_NN_Priority);             core.h    310
//   OH_NN_ReturnCode   OH_NNCompilation_Build(OH_NNCompilation*);                                   core.h    353
//   OH_NNExecutor     *OH_NNExecutor_Construct(OH_NNCompilation*);                                  core.h    842
//   OH_NN_ReturnCode   OH_NNExecutor_GetInputCount(const OH_NNExecutor*, size_t*);                  core.h    912
//   OH_NN_ReturnCode   OH_NNExecutor_GetOutputCount(const OH_NNExecutor*, size_t*);                 core.h    929
//   NN_TensorDesc     *OH_NNExecutor_CreateInputTensorDesc(const OH_NNExecutor*, size_t);           core.h    945
//   NN_TensorDesc     *OH_NNExecutor_CreateOutputTensorDesc(const OH_NNExecutor*, size_t);          core.h    961
//   NN_Tensor         *OH_NNTensor_Create(size_t deviceID, NN_TensorDesc*);                         core.h    638
//   void              *OH_NNTensor_GetDataBuffer(const NN_Tensor*);                                 core.h    759
//   OH_NN_ReturnCode   OH_NNTensor_GetSize(const NN_Tensor*, size_t*);                              core.h    804
//   OH_NN_ReturnCode   OH_NNTensor_GetOffset(const NN_Tensor*, size_t*);                            core.h    826
//   OH_NN_ReturnCode   OH_NNExecutor_RunSync(OH_NNExecutor*, NN_Tensor*[], size_t,
//                          NN_Tensor*[], size_t);                                                  core.h   1062
// 注意 OH_NN_TensorDesc_* / OH_NNTensor_* / OH_NNCompilation_* / OH_NNExecutor_* / OH_NNDevice_*
// 全部声明在 neural_network_core.h(库 = libneural_network_core.so), 只有 OH_NNModel_*
// 与 OH_NNQuantParam_* 声明在 neural_network_runtime.h(库 = libneural_network_runtime.so)。
// 两个库在 CMakeLists 里都链了。
// ---------------------------------------------------------------------------
void runAttempt(size_t deviceId, const DeviceInfo& di, int opSet, uint32_t dim, uint32_t layers,
                int warmup, int iters, int budgetMs, Attempt& a)
{
    using Clock = std::chrono::steady_clock;

    a.deviceId = deviceId;
    a.opSet = opSet;
    a.opSetName = opSetDef(opSet).name;
    a.mainOp = opSetDef(opSet).mainOp;
    a.typeName = devTypeName(di.type);
    a.typeEnum = devTypeEnumText(di.type);
    a.deviceName = di.name;
    a.deviceNameRc = rcText(di.nameRc);

    const uint64_t dimU = (uint64_t)dim;
    a.macs = dimU * dimU * dimU * (uint64_t)layers;
    a.flops = a.macs * 2ULL;
    a.weightBytes = (size_t)layers * (size_t)dim * (size_t)dim * sizeof(float);
    a.inputBytes = (size_t)dim * (size_t)dim * sizeof(float);
    a.matmulOps = (int)layers;
    a.reluOps = (layers > 0) ? (int)(layers - 1) : 0;

    auto trail = [&a](const char* key, OH_NN_ReturnCode rc) {
        if (!a.rcTrail.empty()) {
            a.rcTrail += " ";
        }
        a.rcTrail += key;
        a.rcTrail += "=";
        a.rcTrail += rcText(rc);
        a.rcTrail += ";";
    };
    auto hardFail = [&a](const std::string& stage, OH_NN_ReturnCode rc) {
        a.ok = false;
        a.failStage = stage;
        a.failRc = rc;
        a.failRcIsOurs = false;
    };
    // 本 App 自己的自检失败(不是 NNRt 返回码), 单列一个标志, 免得被误读成 NNRt 的错误码。
    auto hardFailOurs = [&a](const char* stage, const std::string& why) {
        a.ok = false;
        a.failStage = stage;
        a.failRcIsOurs = true;
        a.failOursText = why;
    };

    const Clock::time_point t0 = Clock::now();
    const Clock::time_point deadline = t0 + std::chrono::milliseconds(budgetMs);

    AttemptResources res;

    // ================= 1) 纯代码建模 =================
    res.model = g_nnrt.OH_NNModel_Construct();
    if (res.model == nullptr) {
        hardFailOurs("建模: OH_NNModel_Construct 返回空指针", "内存不足或 NNRt 服务不可用");
        return;
    }

    const std::vector<int32_t> matDims = { (int32_t)dim, (int32_t)dim };
    const std::vector<int32_t> biasDims = { (int32_t)dim };

    // 建一个 FLOAT32 张量描述并加进模型; 成功后 *indexOut = 该张量在模型里的下标
    // (下标就是"加进模型的顺序", 头文件 159-160 行写明的规则)。
    auto addTensor = [&](const char* name, const std::vector<int32_t>& dims, uint32_t* indexOut) -> bool {
        NN_TensorDesc* d = g_nnrt.OH_NNTensorDesc_Create();
        if (d == nullptr) {
            hardFailOurs("建模: OH_NNTensorDesc_Create 返回空指针", "内存不足");
            return false;
        }
        res.modelDescs.push_back(d);
        const uint32_t idx = (uint32_t)(res.modelDescs.size() - 1);
        OH_NN_ReturnCode rc = g_nnrt.OH_NNTensorDesc_SetName(d, name);
        if (rc != OH_NN_SUCCESS) {
            trail("tensorDescSetName", rc);
            hardFail("建模: OH_NNTensorDesc_SetName", rc);
            return false;
        }
        rc = g_nnrt.OH_NNTensorDesc_SetDataType(d, OH_NN_FLOAT32);
        if (rc != OH_NN_SUCCESS) {
            trail("tensorDescSetDataType", rc);
            hardFail("建模: OH_NNTensorDesc_SetDataType(FLOAT32)", rc);
            return false;
        }
        rc = g_nnrt.OH_NNTensorDesc_SetShape(d, dims.data(), dims.size());
        if (rc != OH_NN_SUCCESS) {
            trail("tensorDescSetShape", rc);
            hardFail("建模: OH_NNTensorDesc_SetShape", rc);
            return false;
        }
        rc = g_nnrt.OH_NNTensorDesc_SetFormat(d, OH_NN_FORMAT_NONE);
        if (rc != OH_NN_SUCCESS) {
            trail("tensorDescSetFormat", rc);
            hardFail("建模: OH_NNTensorDesc_SetFormat(NONE)", rc);
            return false;
        }
        rc = g_nnrt.OH_NNModel_AddTensorToModel(res.model, d);
        trail("addTensor", rc);
        if (rc != OH_NN_SUCCESS) {
            hardFail("建模: OH_NNModel_AddTensorToModel", rc);
            return false;
        }
        if (indexOut != nullptr) {
            *indexOut = idx;
        }
        return true;
    };

    uint32_t xIndex = 0;
    if (!addTensor("npu_in", matDims, &xIndex)) {
        return;
    }
    std::vector<uint32_t> weightIdx;
    std::vector<uint32_t> biasIdx;
    std::vector<uint32_t> yIdx;
    std::vector<uint32_t> aIdx;
    weightIdx.reserve(layers);
    biasIdx.reserve(layers);
    yIdx.reserve(layers);
    aIdx.reserve(layers);
    for (uint32_t l = 0; l < layers; ++l) {
        char nm[48];
        uint32_t idx = 0;
        (void)snprintf(nm, sizeof(nm), "npu_w%u", (unsigned)l);
        if (!addTensor(nm, matDims, &idx)) {
            return;
        }
        weightIdx.push_back(idx);
        if (opSet == kOpSetFullConnection) {
            // FullConnection 的第三个输入是 bias(头文件 798 行)。对 [dim,dim] 的输入,
            // 输出通道数 = dim, 所以 bias 是 1 维长度 dim 的张量。
            (void)snprintf(nm, sizeof(nm), "npu_b%u", (unsigned)l);
            if (!addTensor(nm, biasDims, &idx)) {
                return;
            }
            biasIdx.push_back(idx);
        }
        (void)snprintf(nm, sizeof(nm), "npu_y%u", (unsigned)l);
        if (!addTensor(nm, matDims, &idx)) {
            return;
        }
        yIdx.push_back(idx);
        if (l + 1u < layers) {
            (void)snprintf(nm, sizeof(nm), "npu_a%u", (unsigned)l);
            if (!addTensor(nm, matDims, &idx)) {
                return;
            }
            aIdx.push_back(idx);
        }
    }

    // 加算子。MATMUL / FULL_CONNECTION / RELU 都不写算子参数 —— 头文件 249 行:
    // "If no operator parameter is set, the operator uses the default parameter value."
    // 所以参数数组给一个 data=nullptr / size=0 的合法结构体(数组指针本身不能是 nullptr,
    // 否则会返回 OH_NN_INVALID_PARAMETER)。
    const OH_NN_UInt32Array emptyParams{ nullptr, 0u };
    uint32_t prev = xIndex;
    uint32_t outIndex = 0;
    for (uint32_t l = 0; l < layers; ++l) {
        OH_NN_ReturnCode rc = OH_NN_SUCCESS;
        if (opSet == kOpSetFullConnection) {
            // FullConnection: 输入顺序 = {input, weight, bias}(头文件 794-800 行)。
            uint32_t fcIn[3] = { prev, weightIdx[l], biasIdx[l] };
            OH_NN_UInt32Array fcIns{ fcIn, 3u };
            OH_NN_UInt32Array fcOut{ &yIdx[l], 1u };
            rc = g_nnrt.OH_NNModel_AddOperation(res.model, OH_NN_OPS_FULL_CONNECTION, &emptyParams, &fcIns,
                                         &fcOut);
            trail("addOpFullConnection", rc);
            if (rc != OH_NN_SUCCESS) {
                hardFail("建模: OH_NNModel_AddOperation(FULL_CONNECTION)", rc);
                return;
            }
        } else {
            uint32_t mmIn[2] = { prev, weightIdx[l] };
            OH_NN_UInt32Array mmIns{ mmIn, 2u };
            OH_NN_UInt32Array mmOut{ &yIdx[l], 1u };
            rc = g_nnrt.OH_NNModel_AddOperation(res.model, OH_NN_OPS_MATMUL, &emptyParams, &mmIns, &mmOut);
            trail("addOpMatMul", rc);
            if (rc != OH_NN_SUCCESS) {
                hardFail("建模: OH_NNModel_AddOperation(MATMUL)", rc);
                return;
            }
        }
        if (l + 1u < layers) {
            OH_NN_UInt32Array rlIn{ &yIdx[l], 1u };
            OH_NN_UInt32Array rlOut{ &aIdx[l], 1u };
            rc = g_nnrt.OH_NNModel_AddOperation(res.model, OH_NN_OPS_RELU, &emptyParams, &rlIn, &rlOut);
            trail("addOpRelu", rc);
            if (rc != OH_NN_SUCCESS) {
                hardFail("建模: OH_NNModel_AddOperation(RELU)", rc);
                return;
            }
            prev = aIdx[l];
        } else {
            outIndex = yIdx[l];
        }
    }

    // 权重常量: 确定性小权重(幅度 ~1e-2, 带正负)。
    // 幅度小是为了让 L 层累乘后不会溢出 FP16/FP32; 带负值是为了让 RELU 真的有截断作用
    // (全正权重时 RELU 相当于恒等, 那这个算子就白加了)。
    std::vector<float> wData((size_t)dim * (size_t)dim);
    for (size_t k = 0; k < wData.size(); ++k) {
        const int t = (int)(k % 17u);
        wData[k] = 0.01f * (float)(t - 8) / 8.0f;
    }
    for (uint32_t l = 0; l < layers; ++l) {
        OH_NN_ReturnCode rc = g_nnrt.OH_NNModel_SetTensorData(res.model, weightIdx[l], wData.data(),
                                                       wData.size() * sizeof(float));
        trail("setTensorData", rc);
        if (rc != OH_NN_SUCCESS) {
            hardFail("建模: OH_NNModel_SetTensorData(第 " + std::to_string(l) + " 层权重)", rc);
            return;
        }
    }
    if (opSet == kOpSetFullConnection) {
        // 偏置取全 0: 偏置不参与乘加次数, 也不改变 MACs = layers * dim^3 这个口径;
        // 取 0 是为了让 FULL_CONNECTION 组的结果与 MATMUL 组尽量可比。
        const std::vector<float> bData((size_t)dim, 0.0f);
        for (uint32_t l = 0; l < layers; ++l) {
            OH_NN_ReturnCode rc = g_nnrt.OH_NNModel_SetTensorData(res.model, biasIdx[l], bData.data(),
                                                           bData.size() * sizeof(float));
            trail("setTensorDataBias", rc);
            if (rc != OH_NN_SUCCESS) {
                hardFail("建模: OH_NNModel_SetTensorData(第 " + std::to_string(l) + " 层偏置)", rc);
                return;
            }
        }
    }

    OH_NN_UInt32Array modelIns{ &xIndex, 1u };
    OH_NN_UInt32Array modelOuts{ &outIndex, 1u };
    OH_NN_ReturnCode rc = g_nnrt.OH_NNModel_SpecifyInputsAndOutputs(res.model, &modelIns, &modelOuts);
    trail("specifyInputsAndOutputs", rc);
    if (rc != OH_NN_SUCCESS) {
        hardFail("建模: OH_NNModel_SpecifyInputsAndOutputs", rc);
        return;
    }
    rc = g_nnrt.OH_NNModel_Finish(res.model);
    trail("finish", rc);
    if (rc != OH_NN_SUCCESS) {
        hardFail("建模: OH_NNModel_Finish", rc);
        return;
    }

    // ================= 2) 编译 =================
    res.comp = g_nnrt.OH_NNCompilation_Construct(res.model);
    if (res.comp == nullptr) {
        hardFailOurs("编译: OH_NNCompilation_Construct 返回空指针",
                     "模型参数非法或模型格式不被接受");
        return;
    }
    rc = g_nnrt.OH_NNCompilation_SetDevice(res.comp, deviceId);
    trail("setDevice", rc);
    if (rc != OH_NN_SUCCESS) {
        hardFail("编译: OH_NNCompilation_SetDevice", rc);
        return;
    }

    // FP16: 先请求打开。头文件 320 行写明"设备不支持时返回 OH_NN_UNAVALIDABLE_DEVICE",
    // 这时显式关掉再继续, 并把两次返回码都记进结果(不隐藏)。
    rc = g_nnrt.OH_NNCompilation_EnableFloat16(res.comp, true);
    a.fp16RcText = rcText(rc);
    trail("enableFloat16(true)", rc);
    if (rc == OH_NN_SUCCESS) {
        a.fp16Accepted = true;
    } else {
        OH_NN_ReturnCode rc2 = g_nnrt.OH_NNCompilation_EnableFloat16(res.comp, false);
        a.fp16RcText2 = rcText(rc2);
        trail("enableFloat16(false)", rc2);
        if (rc2 != OH_NN_SUCCESS) {
            hardFail("编译: OH_NNCompilation_EnableFloat16(false)", rc2);
            return;
        }
    }

    // 性能模式 / 优先级: 尽力而为。真机上不少后端不支持, 头文件 274/297 行说会返回
    // OH_NN_UNAVALIDABLE_DEVICE —— 这不算失败(它不影响能不能跑, 只影响快慢),
    // 但返回码一定记下来, 免得把"没设成"悄悄咽掉。
    rc = g_nnrt.OH_NNCompilation_SetPerformanceMode(res.comp, OH_NN_PERFORMANCE_EXTREME);
    a.perfRcText = rcText(rc);
    trail("setPerformanceMode", rc);
    rc = g_nnrt.OH_NNCompilation_SetPriority(res.comp, OH_NN_PRIORITY_HIGH);
    a.prioRcText = rcText(rc);
    trail("setPriority", rc);

    rc = g_nnrt.OH_NNCompilation_Build(res.comp);
    trail("build", rc);
    if (rc != OH_NN_SUCCESS) {
        hardFail("编译: OH_NNCompilation_Build", rc);
        return;
    }

    // ================= 3) 执行器与张量 =================
    res.exec = g_nnrt.OH_NNExecutor_Construct(res.comp);
    if (res.exec == nullptr) {
        hardFailOurs("执行器: OH_NNExecutor_Construct 返回空指针", "编译实例无效或内存不足");
        return;
    }

    size_t inCount = 0;
    size_t outCount = 0;
    rc = g_nnrt.OH_NNExecutor_GetInputCount(res.exec, &inCount);
    trail("getInputCount", rc);
    if (rc != OH_NN_SUCCESS) {
        hardFail("执行器: OH_NNExecutor_GetInputCount", rc);
        return;
    }
    rc = g_nnrt.OH_NNExecutor_GetOutputCount(res.exec, &outCount);
    trail("getOutputCount", rc);
    if (rc != OH_NN_SUCCESS) {
        hardFail("执行器: OH_NNExecutor_GetOutputCount", rc);
        return;
    }
    if (inCount != 1 || outCount != 1) {
        hardFailOurs("执行器: 输入/输出张量个数与模型不符(期望 1 进 1 出)",
                     "后端对模型的拆解结果与建模不一致");
        return;
    }

    // 张量描述一律取执行器自己给的那一份: 这样后端怎么改写形状/类型/布局都跟着走,
    // 不会出现"我们以为建的是 FLOAT32/NONE, 后端其实要 FLOAT16"的错配。
    res.inDesc = g_nnrt.OH_NNExecutor_CreateInputTensorDesc(res.exec, 0);
    if (res.inDesc == nullptr) {
        hardFailOurs("张量: OH_NNExecutor_CreateInputTensorDesc 返回空指针", "索引越界或内存不足");
        return;
    }
    res.outDesc = g_nnrt.OH_NNExecutor_CreateOutputTensorDesc(res.exec, 0);
    if (res.outDesc == nullptr) {
        hardFailOurs("张量: OH_NNExecutor_CreateOutputTensorDesc 返回空指针", "索引越界或内存不足");
        return;
    }

    // 三个查询都要把返回码本身留下: "查不到" 和 "查到了但值是别的" 是两回事,
    // 结果里必须分得清(否则就成了把默认值伪装成后端声明值)。
    OH_NN_DataType inDt = OH_NN_UNKNOWN;
    const OH_NN_ReturnCode inDtRc = g_nnrt.OH_NNTensorDesc_GetDataType(res.inDesc, &inDt);
    a.execInputDtRc = rcText(inDtRc);
    trail("execInputDataType", inDtRc);
    if (inDtRc != OH_NN_SUCCESS) {
        inDt = OH_NN_UNKNOWN;
    }
    a.execInputDt = dtEnumText(inDt);

    OH_NN_Format inFmt = OH_NN_FORMAT_NONE;
    const OH_NN_ReturnCode inFmtRc = g_nnrt.OH_NNTensorDesc_GetFormat(res.inDesc, &inFmt);
    a.execInputFmtRc = rcText(inFmtRc);
    trail("execInputFormat", inFmtRc);
    if (inFmtRc != OH_NN_SUCCESS) {
        inFmt = OH_NN_FORMAT_NONE;
    }
    a.execInputFmt = fmtText(inFmt);

    OH_NN_DataType outDt = OH_NN_UNKNOWN;
    const OH_NN_ReturnCode outDtRc = g_nnrt.OH_NNTensorDesc_GetDataType(res.outDesc, &outDt);
    a.execOutputDtRc = rcText(outDtRc);
    trail("execOutputDataType", outDtRc);
    if (outDtRc != OH_NN_SUCCESS) {
        outDt = OH_NN_UNKNOWN;
    }
    a.execOutputDt = dtEnumText(outDt);

    res.inTensor = g_nnrt.OH_NNTensor_Create(deviceId, res.inDesc);
    if (res.inTensor == nullptr) {
        hardFailOurs("张量: OH_NNTensor_Create(输入) 返回空指针", "设备共享内存分配失败");
        return;
    }
    res.outTensor = g_nnrt.OH_NNTensor_Create(deviceId, res.outDesc);
    if (res.outTensor == nullptr) {
        hardFailOurs("张量: OH_NNTensor_Create(输出) 返回空指针", "设备共享内存分配失败");
        return;
    }

    void* inBuf = g_nnrt.OH_NNTensor_GetDataBuffer(res.inTensor);
    size_t inSize = 0;
    size_t inOff = 0;
    const OH_NN_ReturnCode szRc = g_nnrt.OH_NNTensor_GetSize(res.inTensor, &inSize);
    const OH_NN_ReturnCode ofRc = g_nnrt.OH_NNTensor_GetOffset(res.inTensor, &inOff);
    trail("tensorGetSize", szRc);
    trail("tensorGetOffset", ofRc);
    // 头文件 748-749 行: 真正的张量数据只占共享内存的 [offset, size) 这一段。
    size_t usable = 0;
    if (szRc == OH_NN_SUCCESS && ofRc == OH_NN_SUCCESS && inSize > inOff) {
        usable = inSize - inOff;
    }
    const size_t elems = (size_t)dim * (size_t)dim;

    // 后端的张量描述必须和我们建模时声明的元素个数一致。不一致说明两边对模型的理解已经不同 ——
    // 这种情况宁可当场失败并报出来, 也不要往共享内存里灌错格式的字节(那会得到一个"跑得通但
    // 算的是垃圾"的结果, 是跑分里最坏的一种错)。
    size_t inElems = 0;
    const OH_NN_ReturnCode elRc = g_nnrt.OH_NNTensorDesc_GetElementCount(res.inDesc, &inElems);
    trail("inputElementCount", elRc);
    if (elRc != OH_NN_SUCCESS || inElems != elems) {
        hardFailOurs("张量: 执行器声明的输入元素个数与建模时的 [dim,dim] 不符",
                     "OH_NNTensorDesc_GetElementCount 的结果与模型不一致");
        return;
    }
    // 输出也必须是 [dim,dim]。这一步对 FULL_CONNECTION 组尤其重要: 该算子的轴/布局语义由
    // 厂商后端决定, 一旦输出形状不是我们建模时声明的样子, "MACs = layers * dim^3" 就不成立,
    // 那时候报出来的 GOPs/s 是错的 —— 宁可报失败, 也不报一个口径错掉的数字。
    size_t outElemsChk = 0;
    const OH_NN_ReturnCode oelRc = g_nnrt.OH_NNTensorDesc_GetElementCount(res.outDesc, &outElemsChk);
    trail("outputElementCount", oelRc);
    if (oelRc != OH_NN_SUCCESS || outElemsChk != elems) {
        hardFailOurs("张量: 执行器声明的输出元素个数与建模时的 [dim,dim] 不符",
                     "输出形状与模型不一致 -> 乘加次数口径不成立, 不做测量直接报失败");
        return;
    }

    // 按执行器实际声明的数据类型写入输入数据:
    //   FLOAT32 直接拷; FLOAT16 先做 binary16 转换(不能把 FP32 的字节原样怼进去)。
    const size_t elemBytes = dtypeBytes(inDt);
    if (elemBytes == 0 || (inDt != OH_NN_FLOAT32 && inDt != OH_NN_FLOAT16)) {
        hardFailOurs("张量: 执行器声明的输入数据类型不是 FLOAT32/FLOAT16",
                     (inDtRc != OH_NN_SUCCESS)
                         ? ("OH_NNTensorDesc_GetDataType 本身就失败了(返回 " + a.execInputDtRc +
                            ") —— 查不到类型时不猜, 直接报失败")
                         : ("查询成功(返回 " + a.execInputDtRc + "), 但后端声明的是 " +
                            a.execInputDt + ", 本 App 只实现了 FLOAT32/FLOAT16 两种浮点输入格式"));
        return;
    }
    const size_t writeBytes = elems * elemBytes;
    if (inBuf == nullptr || usable < writeBytes) {
        hardFailOurs("张量: 输入共享内存小于写入所需的字节数",
                     "共享内存大小/偏移与张量声明不一致");
        return;
    }

    std::vector<float> xData(elems);
    for (size_t k = 0; k < elems; ++k) {
        const int t = (int)(k % 13u);
        xData[k] = 0.1f * (float)(t - 6) / 6.0f;
    }
    if (inDt == OH_NN_FLOAT16) {
        std::vector<uint16_t> h(elems);
        for (size_t k = 0; k < elems; ++k) {
            h[k] = f32ToF16(xData[k]);
        }
        std::memcpy(inBuf, h.data(), writeBytes);
    } else {
        std::memcpy(inBuf, xData.data(), writeBytes);
    }

    // ================= 4) 计时 =================
    std::vector<NN_Tensor*> inArr{ res.inTensor };
    std::vector<NN_Tensor*> outArr{ res.outTensor };

    const Clock::time_point t1 = Clock::now();
    a.buildMs = msBetween(t0, t1);   // 建模 + 编译 + 执行器 + 张量分配 = "编译/加载"那一段

    rc = g_nnrt.OH_NNExecutor_RunSync(res.exec, inArr.data(), inArr.size(), outArr.data(), outArr.size());
    trail("runSync(first)", rc);
    const Clock::time_point t2 = Clock::now();
    a.firstRunMs = msBetween(t1, t2);   // 第一次推理(含驱动首次加载/初始化)
    if (rc != OH_NN_SUCCESS) {
        hardFail("执行: OH_NNExecutor_RunSync(首次推理)", rc);
        return;
    }

    for (int i = 0; i < warmup; ++i) {
        if (Clock::now() >= deadline) {
            a.warmupShortened = true;
            a.budgetHit = true;
            break;
        }
        rc = g_nnrt.OH_NNExecutor_RunSync(res.exec, inArr.data(), inArr.size(), outArr.data(), outArr.size());
        if (rc != OH_NN_SUCCESS) {
            trail("runSync(warmup)", rc);
            hardFail("执行: OH_NNExecutor_RunSync(预热)", rc);
            return;
        }
        ++a.warmupDone;
    }

    std::vector<double> lat;
    lat.reserve((size_t)(iters > 0 ? iters : 1));
    for (int i = 0; i < iters; ++i) {
        // 时间预算: 保证"单次运行 1~3 秒"这个约束真的成立(不靠模型恰好够小)。
        if (Clock::now() >= deadline) {
            a.budgetHit = true;
            break;
        }
        const Clock::time_point s = Clock::now();
        rc = g_nnrt.OH_NNExecutor_RunSync(res.exec, inArr.data(), inArr.size(), outArr.data(), outArr.size());
        const Clock::time_point e2 = Clock::now();
        if (rc != OH_NN_SUCCESS) {
            trail("runSync", rc);
            hardFail("执行: OH_NNExecutor_RunSync(计时)", rc);
            return;
        }
        lat.push_back(msBetween(s, e2));
    }

    if (lat.empty()) {
        // 预算已被建模/编译/首次推理吃掉 —— 那就只用首次那一次, 并标出来。
        a.sampleFromFirstRun = true;
        lat.push_back(a.firstRunMs);
    }
    a.runsDone = (int)lat.size();

    std::sort(lat.begin(), lat.end());
    a.minMs = lat.front();
    a.maxMs = lat.back();
    // 样本数为偶数时取"上中位数"(第 n/2 个, 0 基), 口径写进结果的 medianNote。
    a.medianMs = lat[lat.size() / 2];
    double sum = 0.0;
    for (double v : lat) {
        sum += v;
    }
    a.meanMs = sum / (double)lat.size();
    if (!jfinite(a.meanMs) || a.meanMs <= 0.0) {
        a.nonFinite = true;
        a.goPs = 0.0;
        a.macsPerSec = 0.0;
        a.hint = "稳态延迟取到了非法值(时钟异常), 吞吐按 0 上报, 不当成好成绩。";
    } else {
        a.macsPerSec = (double)a.macs / (a.meanMs / 1000.0);
        a.goPs = (double)a.flops / (a.meanMs / 1000.0) / 1e9;
        if (a.meanMs < 8.0) {
            a.hint = "稳态单次推理 " + jf(a.meanMs, 2) +
                     " ms, 偏短(< 8 ms): 可以把 scale 调大一档, 让计时更抗噪。";
        } else if (a.meanMs > 120.0) {
            a.hint = "稳态单次推理 " + jf(a.meanMs, 2) +
                     " ms, 偏长(> 120 ms): 可以把 scale 调小一档。";
        } else {
            a.hint = "稳态单次推理 " + jf(a.meanMs, 2) + " ms, 落在建议的 10~100 ms 量级内。";
        }
    }

    // 输出合理性检查: 只用来证明"这次推理真的产出了数据", 不参与计分。
    // 全 0 / 全 NaN 都说明后端虽然没报错但结果不可信, 这个事实必须显示出来。
    size_t outBytes = 0;
    if (outDt != OH_NN_FLOAT32 && outDt != OH_NN_FLOAT16) {
        a.outputNote = "执行器声明的输出数据类型是 " + a.execOutputDt +
                       ", 本 App 未实现该格式的读取, 不做合理性检查";
    } else if (g_nnrt.OH_NNTensor_GetSize(res.outTensor, &outBytes) == OH_NN_SUCCESS) {
        void* ob = g_nnrt.OH_NNTensor_GetDataBuffer(res.outTensor);
        const size_t outElems = elems;
        const size_t needOut = outElems * dtypeBytes(outDt);
        if (ob == nullptr || outBytes < needOut) {
            a.outputNote = "输出共享内存小于预期, 未能做合理性检查";
        } else if (outDt == OH_NN_FLOAT16) {
            const uint16_t* p = static_cast<const uint16_t*>(ob);
            bool allZero = true;
            bool allFinite = true;
            for (size_t k = 0; k < outElems; ++k) {
                const float v = f16ToF32(p[k]);
                if (v != 0.0f) {
                    allZero = false;
                }
                if (!jfinite((double)v)) {
                    allFinite = false;
                }
            }
            a.outputSane = (!allZero && allFinite);
            a.outputNote = std::string("输出按 FP16 读(元素数 ") + ju64((unsigned long long)outElems) +
                           "): allZero=" + jbool(allZero) + " allFinite=" + jbool(allFinite);
        } else {
            const float* p = static_cast<const float*>(ob);
            bool allZero = true;
            bool allFinite = true;
            for (size_t k = 0; k < outElems; ++k) {
                if (p[k] != 0.0f) {
                    allZero = false;
                }
                if (!jfinite((double)p[k])) {
                    allFinite = false;
                }
            }
            a.outputSane = (!allZero && allFinite);
            a.outputNote = std::string("输出按 FP32 读(元素数 ") + ju64((unsigned long long)outElems) +
                           "): allZero=" + jbool(allZero) + " allFinite=" + jbool(allFinite);
        }
    } else {
        a.outputNote = "OH_NNTensor_GetSize(输出) 失败, 未做合理性检查";
    }

    a.ok = true;
}

// ---------------------------------------------------------------------------
// 把一次尝试写成 JSON(每一次调用的返回码都在 rcTrail 里, 一个都不省)
// ---------------------------------------------------------------------------
void appendAttemptJson(std::string& out, const Attempt& a)
{
    out += "{\"deviceId\":" + ju64((unsigned long long)a.deviceId);
    out += ",\"opSet\":" + ji(a.opSet);
    out += ",\"opSetName\":" + jstr(a.opSetName);
    out += ",\"mainOp\":" + jstr(a.mainOp);
    out += ",\"typeName\":" + jstr(a.typeName);
    out += ",\"typeEnum\":" + jstr(a.typeEnum);
    out += ",\"deviceName\":" + jstr(a.deviceName);
    out += ",\"deviceNameRc\":" + jstr(a.deviceNameRc);
    out += ",\"ok\":" + jbool(a.ok);
    out += ",\"failStage\":" + jstr(a.failStage);
    out += ",\"failRc\":" + ji((long long)a.failRc);
    out += ",\"failRcIsAppSelfCheck\":" + jbool(a.failRcIsOurs);
    out += ",\"failRcName\":";
    out += jstr(a.failRcIsOurs ? std::string("APP_SELF_CHECK(本 App 自检失败, 不是 NNRt 返回码)")
                               : rcText(a.failRc));
    out += ",\"failOursText\":" + jstr(a.failOursText);
    out += ",\"rcTrail\":" + jstr(a.rcTrail);
    out += ",\"buildMs\":" + jf(a.buildMs, 3);
    out += ",\"firstRunMs\":" + jf(a.firstRunMs, 3);
    out += ",\"warmupDone\":" + ji(a.warmupDone);
    out += ",\"runsDone\":" + ji(a.runsDone);
    out += ",\"meanMs\":" + jf(a.meanMs, 3);
    out += ",\"minMs\":" + jf(a.minMs, 3);
    out += ",\"medianMs\":" + jf(a.medianMs, 3);
    out += ",\"maxMs\":" + jf(a.maxMs, 3);
    out += ",\"goPs\":" + jf(a.goPs, 4);
    out += ",\"fp16Rc\":" + jstr(a.fp16RcText);
    out += ",\"fp16RcFallback\":" + jstr(a.fp16RcText2);
    out += ",\"fp16Enabled\":" + jbool(a.fp16Accepted);
    out += ",\"setPerformanceModeRc\":" + jstr(a.perfRcText);
    out += ",\"setPriorityRc\":" + jstr(a.prioRcText);
    out += ",\"execInputDataType\":" + jstr(a.execInputDt);
    out += ",\"execInputDataTypeRc\":" + jstr(a.execInputDtRc);
    out += ",\"execInputFormat\":" + jstr(a.execInputFmt);
    out += ",\"execInputFormatRc\":" + jstr(a.execInputFmtRc);
    out += ",\"execOutputDataType\":" + jstr(a.execOutputDt);
    out += ",\"execOutputDataTypeRc\":" + jstr(a.execOutputDtRc);
    out += ",\"outputSane\":" + jbool(a.outputSane);
    out += ",\"outputNote\":" + jstr(a.outputNote);
    out += "}";
}

std::string deviceLabel(const std::string& typeName)
{
    if (typeName == "ACCELERATOR") {
        return "NPU(专用硬件加速器)";
    }
    if (typeName == "GPU") {
        return "GPU(图形处理器通用计算)";
    }
    if (typeName == "CPU") {
        return "CPU(纯软件推理)";
    }
    return "其它设备类型";
}

// ---------------------------------------------------------------------------
// 完整结果 JSON(成功与失败同一形状: 失败时 available=false + error 带原始错误码,
// 这样 ArkTS 侧读任何字段都不会拿到 undefined)
// ---------------------------------------------------------------------------
std::string buildBenchJson(bool ok, const std::string& errText, const NpuBenchRequest& req,
                           const ScalePreset& ps, const DeviceEnum& e,
                           const std::vector<Attempt>& attempts, size_t chosenIdx,
                           double totalMs)
{
    const bool haveChosen = ok && chosenIdx < attempts.size();
    const Attempt* ch = haveChosen ? &attempts[chosenIdx] : nullptr;

    std::string out;
    out.reserve(8192);
    out += "{\"ok\":" + jbool(ok);
    out += ",\"kind\":\"npu-bench\"";
    out += ",\"section\":\"NPU\"";
    // 双重标记: NPU 在 GB7 里没有对应项, 这一节不能被算进 CS1 分数。
    out += ",\"scored\":false";
    out += ",\"gb7Item\":false";
    out += ",\"error\":" + jstr(errText);

    // ---- 设备(最重要的元信息) ----
    std::string preferredOrder = "OH_NN_ACCELERATOR -> OH_NN_GPU -> OH_NN_CPU -> OH_NN_OTHERS";
    out += ",\"device\":{\"preferredOrder\":" + jstr(preferredOrder);
    out += ",\"preferredTypeName\":";
    out += jstr(attempts.empty() ? std::string() : attempts.front().typeName);
    out += ",\"chosenDeviceId\":";
    out += (ch != nullptr) ? ju64((unsigned long long)ch->deviceId) : std::string("-1");
    out += ",\"typeName\":" + jstr(ch != nullptr ? ch->typeName : std::string());
    out += ",\"typeEnum\":" + jstr(ch != nullptr ? ch->typeEnum : std::string());
    out += ",\"deviceLabel\":" + jstr(ch != nullptr ? deviceLabel(ch->typeName) : std::string());
    out += ",\"name\":" + jstr(ch != nullptr ? ch->deviceName : std::string());
    out += ",\"nameRc\":" + jstr(ch != nullptr ? ch->deviceNameRc : std::string());
    const bool fallbackUsed = (ok && chosenIdx > 0);
    out += ",\"fallbackUsed\":" + jbool(fallbackUsed);
    std::string fallbackText;
    if (!ok) {
        fallbackText = "未选中任何设备(全部尝试失败, 明细见 attempts)";
    } else if (fallbackUsed) {
        fallbackText = "首选设备(" + attempts.front().typeName + ")不可用, 已回退到第 " +
                       ju64((unsigned long long)(chosenIdx + 1)) + " 个候选: " + ch->typeName +
                       " / " + ch->deviceName +
                       " —— 本次数字不是 NPU 的数字, 请照实显示。";
    } else {
        fallbackText = "已使用首选设备 " + ch->typeName + " / " + ch->deviceName + ", 未发生设备回退。";
    }
    if (ok && ch != nullptr) {
        fallbackText += (ch->opSet == kOpSetMatMul)
            ? " 算子组: MATMUL+RELU(首选, 未回退)。"
            : (" 算子组: 首选 MATMUL+RELU 全部设备失败, 已回退到 " + ch->opSetName + "。");
    }
    out += ",\"fallbackText\":" + jstr(fallbackText);
    out += ",\"attemptCount\":" + ju64((unsigned long long)attempts.size());
    out += ",\"attempts\":[";
    for (size_t i = 0; i < attempts.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        appendAttemptJson(out, attempts[i]);
    }
    out += "]}";

    // ---- 精度 ----
    out += ",\"precision\":{\"available\":" + jbool(ch != nullptr);
    out += ",\"modelDataType\":\"OH_NN_FLOAT32(11) —— 模型张量一律按 FP32 构建\"";
    out += ",\"float16Requested\":" + jbool(true);
    out += ",\"float16Rc\":" + jstr(ch != nullptr ? ch->fp16RcText : std::string());
    out += ",\"float16RcFallback\":" + jstr(ch != nullptr ? ch->fp16RcText2 : std::string());
    out += ",\"float16Enabled\":" + jbool(ch != nullptr && ch->fp16Accepted);
    out += ",\"execInputDataType\":" + jstr(ch != nullptr ? ch->execInputDt : std::string());
    out += ",\"execInputDataTypeRc\":" + jstr(ch != nullptr ? ch->execInputDtRc : std::string());
    out += ",\"execInputFormat\":" + jstr(ch != nullptr ? ch->execInputFmt : std::string());
    out += ",\"execInputFormatRc\":" + jstr(ch != nullptr ? ch->execInputFmtRc : std::string());
    out += ",\"execOutputDataType\":" + jstr(ch != nullptr ? ch->execOutputDt : std::string());
    out += ",\"execOutputDataTypeRc\":" + jstr(ch != nullptr ? ch->execOutputDtRc : std::string());
    std::string precText;
    bool precKnown = false;
    std::string precShort = "未知";
    if (ch == nullptr) {
        precText = "本次没有成功执行的设备, 精度无意义(记为未知)。";
    } else if (ch->fp16Accepted) {
        precText = "模型按 FP32 构建; 已向设备请求 FP16 计算, OH_NNCompilation_EnableFloat16(true) 返回 " +
                   ch->fp16RcText + "; 执行器声明的输入数据类型 = " + ch->execInputDt +
                   "。设备内部累加是否全程 FP16, NNRt 没有提供查询接口, 本 App 无法确认 —— "
                   "因此精度记为「已开启 FP16(实际累加精度未知)」。";
        precShort = "FP16(已请求, 实际累加精度未知)";
    } else {
        precText = "模型按 FP32 构建; 请求 FP16 返回 " + ch->fp16RcText +
                   ", 已显式关闭(EnableFloat16(false) 返回 " + ch->fp16RcText2 +
                   "), 按 FP32 计算; 执行器声明的输入数据类型 = " + ch->execInputDt + "。精度记为 FP32。";
        precShort = "FP32";
        precKnown = (ch->execInputDt == "OH_NN_FLOAT32(11)");
    }
    out += ",\"short\":" + jstr(precShort);
    out += ",\"known\":" + jbool(precKnown);
    out += ",\"text\":" + jstr(precText);
    out += "}";

    // ---- 模型 ----
    out += ",\"model\":{\"dim\":" + ju64((unsigned long long)ps.dim);
    out += ",\"layers\":" + ju64((unsigned long long)ps.layers);
    out += ",\"scale\":" + ji(req.scale);
    out += ",\"scaleLabel\":" + jstr(ps.label);
    const int usedOpSet = (ch != nullptr) ? ch->opSet : -1;
    out += ",\"opSet\":" + ji(usedOpSet);
    out += ",\"opSetName\":" + jstr(ch != nullptr ? ch->opSetName : std::string());
    out += ",\"mainOp\":" + jstr(ch != nullptr ? ch->mainOp : std::string());
    out += ",\"opSetFallbackUsed\":" + jbool(ok && ch != nullptr && ch->opSet != kOpSetMatMul);
    out += ",\"opSetVerifiedOnHost\":false";
    out += ",\"opSetText\":";
    if (!ok || ch == nullptr) {
        out += jstr("本次没有成功执行的算子组(全部失败, 明细见 device.attempts[])");
    } else if (ch->opSet == kOpSetMatMul) {
        out += jstr("实际使用首选算子组 MATMUL+RELU, 未发生算子组回退");
    } else {
        out += jstr("首选算子组 MATMUL+RELU 在本机全部设备上都没成功, 已回退到算子组 " +
                    ch->opSetName + " —— 两组算子的乘加次数口径相同(MACs = layers * dim^3), "
                    "但 FULL_CONNECTION 的轴/布局语义由厂商后端决定, 本机无法验证, "
                    "所以这一组的数字请连同 opSet 一起解读。");
    }
    out += ",\"opSets\":";
    appendOpSetsJson(out);
    out += ",\"ops\":" + jstr(ch != nullptr
        ? (ch->opSetName + " x " + ju64((unsigned long long)ch->matmulOps) + " + RELU x " +
           ju64((unsigned long long)ch->reluOps) + ", 全部张量 FLOAT32")
        : std::string("(未执行)"));
    out += ",\"matmulOps\":" + ji(ch != nullptr ? ch->matmulOps : (int)ps.layers);
    out += ",\"reluOps\":" + ji(ch != nullptr ? ch->reluOps : (int)(ps.layers > 0 ? ps.layers - 1 : 0));
    out += ",\"macsPerInference\":" + ju64(ch != nullptr ? ch->macs : 0ULL);
    out += ",\"flopsPerInference\":" + ju64(ch != nullptr ? ch->flops : 0ULL);
    out += ",\"macFormula\":" + jstr(std::string("MACs = layers * dim^3。两种算子组都成立: "
        "一次 MATMUL = dim*dim 个输出元素 x 每个 dim 次乘加 = dim^3; "
        "一次 FULL_CONNECTION = dim(输入分量) x dim(输出通道) = dim^3。"
        "FLOPs = 2 * MACs(一次乘加记 2 次浮点运算)。本次档位 = ") + ps.label);
    out += ",\"weightBytes\":" + ju64((unsigned long long)(ch != nullptr ? ch->weightBytes
                                                                          : (size_t)ps.layers * ps.dim * ps.dim * sizeof(float)));
    out += ",\"inputBytes\":" + ju64((unsigned long long)(ch != nullptr ? ch->inputBytes
                                                                         : (size_t)ps.dim * ps.dim * sizeof(float)));
    out += "}";

    // ---- 计时 ----
    out += ",\"timing\":{\"available\":" + jbool(ch != nullptr);
    out += ",\"buildMs\":" + jf(ch != nullptr ? ch->buildMs : 0.0, 3);
    out += ",\"firstRunMs\":" + jf(ch != nullptr ? ch->firstRunMs : 0.0, 3);
    out += ",\"firstInferenceMs\":" +
           jf(ch != nullptr ? (ch->buildMs + ch->firstRunMs) : 0.0, 3);
    out += ",\"firstInferenceNote\":\"首次推理延迟 = buildMs(建模+编译+执行器构造+张量分配) + "
           "firstRunMs(第一次 RunSync, 含驱动首次加载/初始化); 与下面的稳态延迟分开报, 不混在一起\"";
    out += ",\"warmupRuns\":" + ji(ch != nullptr ? ch->warmupDone : 0);
    out += ",\"warmupShortened\":" + jbool(ch != nullptr && ch->warmupShortened);
    out += ",\"measuredRuns\":" + ji(ch != nullptr ? ch->runsDone : 0);
    out += ",\"itersPlanned\":" + ji(req.iters);
    out += ",\"budgetMs\":" + ji(req.budgetMs);
    out += ",\"budgetHit\":" + jbool(ch != nullptr && ch->budgetHit);
    out += ",\"sampleFromFirstRun\":" + jbool(ch != nullptr && ch->sampleFromFirstRun);
    out += ",\"meanMs\":" + jf(ch != nullptr ? ch->meanMs : 0.0, 3);
    out += ",\"minMs\":" + jf(ch != nullptr ? ch->minMs : 0.0, 3);
    out += ",\"medianMs\":" + jf(ch != nullptr ? ch->medianMs : 0.0, 3);
    out += ",\"maxMs\":" + jf(ch != nullptr ? ch->maxMs : 0.0, 3);
    out += ",\"medianNote\":\"样本数为偶数时取上中位数(排序后第 n/2 个, 0 基)\"";
    out += ",\"goPs\":" + jf(ch != nullptr ? ch->goPs : 0.0, 4);
    out += ",\"macsPerSec\":" + jf(ch != nullptr ? ch->macsPerSec : 0.0, 1);
    out += ",\"goPsDefinition\":\"GOPs/s = FLOPs / 稳态平均单次延迟(秒) / 1e9, 其中 FLOPs = 2 x MACs"
           "(一次乘加记 2 次浮点运算); 同时给出 macsPerSec, 两种口径都摆出来, 谁也别猜\"";
    out += ",\"nonFinite\":" + jbool(ch != nullptr && ch->nonFinite);
    out += ",\"hint\":" + jstr(ch != nullptr ? ch->hint : std::string());
    out += "}";

    // ---- 一行中文总结 + metric/unit(失败时也给, 只是写明失败原因) ----
    std::string text;
    std::string metric = "0";
    std::string unit = "GOPs/s";
    if (ok && ch != nullptr) {
        metric = jf(ch->goPs, 4);
        text = "NPU 跑分: 使用 " + ch->typeEnum + " 设备 \"" + ch->deviceName + "\"(" +
               deviceLabel(ch->typeName) + "), 稳态 " + jf(ch->meanMs, 3) + " ms/次, " +
               jf(ch->goPs, 4) + " GOPs/s(" + ju64(ch->flops) + " FLOPs/次); 首次推理(含编译/加载) " +
               jf(ch->buildMs + ch->firstRunMs, 1) + " ms; 精度 " + precShort +
               "; 算子组 " + ch->opSetName + "。";
        if (ch->typeName != "ACCELERATOR") {
            text += "【注意】本次没有用上 NPU: 这台设备没有向第三方应用开放 OH_NN_ACCELERATOR, "
                    "上面的数字来自 " + ch->typeName + ", 请照实显示, 不要当成 NPU 成绩。";
        }
        if (ch->opSet != kOpSetMatMul) {
            text += "【注意】首选算子组 MATMUL+RELU 全部设备都没成功, 已回退到 " + ch->opSetName +
                    "(乘加次数口径相同, 但该算子组的轴/布局语义未经真机验证)。";
        }
        if (!ch->outputSane && !ch->outputNote.empty()) {
            text += "【注意】输出合理性检查未通过: " + ch->outputNote + "。";
        }
    } else {
        unit = "运行失败: " + errText;
        text = errText;
    }
    out += ",\"metric\":" + jstr(metric);
    out += ",\"unit\":" + jstr(unit);
    out += ",\"text\":" + jstr(text);
    out += ",\"totalRunMs\":" + jf(totalMs, 1);

    out += ",\"deviceCount\":" + ju64((unsigned long long)e.devices.size());
    out += ",\"devices\":";
    appendDevicesJson(out, e);
    out += ",\"scalePresets\":";
    appendScalePresetsJson(out);
    out += ",\"nnrt\":";
    appendNnrtJson(out);
    out += ",\"notes\":" + jstr(std::string(kNotes) +
        " 交叉验证提示: results 里的 device.attempts[].rcTrail 是一路上每一次 NNRt 调用的"
        "返回码(枚举名+数值), 编译失败时会明确写出是哪一步失败的, 不会静默跳过。");
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------
// 候选设备排序: OH_NN_ACCELERATOR(NPU) -> OH_NN_GPU -> OH_NN_CPU -> 其它
// 同优先级按 deviceID 升序(稳定排序, 保证同一台机器上结果可复现)。
// 取不到类型的设备排在最后(它的 type 已被置成 OH_NN_OTHERS)。
// ---------------------------------------------------------------------------
int devicePriority(OH_NN_DeviceType t)
{
    switch (t) {
        case OH_NN_ACCELERATOR: return 0;
        case OH_NN_GPU: return 1;
        case OH_NN_CPU: return 2;
        default: return 3;
    }
}

std::string benchJsonImpl(const NpuBenchRequest& reqIn)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tStart = Clock::now();

    // 参数钳制。实际生效的值会原样写进结果的 model/timing 里, 免得"界面上看到的"和"真跑的"
    // 不是一回事。
    NpuBenchRequest req;
    req.scale = reqIn.scale;
    if (req.scale < 0) {
        req.scale = 0;
    }
    if (req.scale > (int)kPresetCount - 1) {
        req.scale = (int)kPresetCount - 1;
    }
    req.warmup = std::max(0, std::min(reqIn.warmup, 50));
    req.iters = std::max(1, std::min(reqIn.iters, 500));
    req.budgetMs = std::max(200, std::min(reqIn.budgetMs, 5000));
    const ScalePreset& ps = presetFor(req.scale);

    // NNRt 绑定不上 -> 明确失败, 不返回 0 分、不静默跳过。注意这条路径不会让
    // App 加载失败: 本模块对 NNRt 没有任何链接期依赖(见文件头说明)。
    if (!g_nnrtState.ok) {
        return buildBenchJson(false, "NPU 跑分失败: " + g_nnrtState.error, req, ps, DeviceEnum(),
                              std::vector<Attempt>(), 0, msBetween(tStart, Clock::now()));
    }

    const DeviceEnum e = enumerateDevices();
    if (e.rc != OH_NN_SUCCESS) {
        return buildBenchJson(false, "NPU 跑分失败: NPU 探测失败: " + rcText(e.rc), req, ps, e,
                              std::vector<Attempt>(), 0, msBetween(tStart, Clock::now()));
    }
    if (e.devices.empty()) {
        return buildBenchJson(false,
                              "NPU 跑分失败: 未枚举到任何 NNRt 设备(deviceCount=0), 没有可用的推理后端",
                              req, ps, e, std::vector<Attempt>(), 0, msBetween(tStart, Clock::now()));
    }

    std::vector<size_t> order(e.devices.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&e](size_t x, size_t y) {
        const int px = devicePriority(e.devices[x].type);
        const int py = devicePriority(e.devices[y].type);
        if (px != py) {
            return px < py;
        }
        return e.devices[x].id < e.devices[y].id;
    });

    // 两级回退, 每次尝试的完整返回码都留在 attempts[] 里 —— 回退过就说回退过, 不藏。
    // 外层 = 算子组: 先用 MATMUL+RELU(首选); 该组在所有设备上都没成功, 才换
    //              FULL_CONNECTION+RELU 把整条设备阶梯再走一遍。
    // 内层 = 设备: 首选 OH_NN_ACCELERATOR(NPU), 不存在/编译失败/执行失败就退 GPU, 再退 CPU。
    // kMaxAttempts 是硬上限: 保证"单次运行 1~3 秒"这个约束在任何回退组合下都成立。
    std::vector<Attempt> attempts;
    size_t chosen = 0;
    bool ok = false;
    for (size_t os = 0; os < kOpSetCount && !ok; ++os) {
        for (size_t k = 0; k < order.size(); ++k) {
            if (attempts.size() >= kMaxAttempts) {
                break;
            }
            Attempt a;
            runAttempt(e.devices[order[k]].id, e.devices[order[k]], kOpSets[os].id, ps.dim, ps.layers,
                       req.warmup, req.iters, req.budgetMs, a);
            attempts.push_back(a);
            if (a.ok) {
                chosen = attempts.size() - 1;
                ok = true;
                break;
            }
            const std::string why = a.failRcIsOurs
                ? ("APP_SELF_CHECK(" + a.failOursText + ")")
                : rcText(a.failRc);
            const std::string line = "NNRt 尝试失败: opSet=" + a.opSetName + " type=" + a.typeName +
                " id=" + ju64((unsigned long long)a.deviceId) + " stage=" + a.failStage +
                " rc=" + why;
            OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, kLogTag, "%{public}s", line.c_str());
        }
    }

    std::string err;
    if (!ok) {
        const Attempt& last = attempts.back();
        const std::string why = last.failRcIsOurs
            ? ("APP_SELF_CHECK(" + last.failOursText + ")")
            : rcText(last.failRc);
        err = "NPU 跑分失败: 枚举到 " + ju64((unsigned long long)e.devices.size()) +
              " 个 NNRt 设备, 设备按 ACCELERATOR -> GPU -> CPU、算子组按 MATMUL+RELU -> "
              "FULL_CONNECTION+RELU 依次尝试, 全部失败(共 " +
              ju64((unsigned long long)attempts.size()) + " 次)。最后一次: 算子组 " + last.opSetName +
              ", 设备 " + last.typeEnum + " / \"" + last.deviceName + "\", 失败阶段 = " +
              last.failStage + ", 返回 " + why +
              "。每个\"算子组 x 设备\"组合的完整返回码串见 device.attempts[].rcTrail。";
    }

    return buildBenchJson(ok, err, req, ps, e, attempts, chosen, msBetween(tStart, Clock::now()));
}

// 长 JSON 不要整条灌进 hilog(单条有长度上限, 会把后面冲掉)。
void logJson(const char* what, const std::string& s)
{
    constexpr size_t kCap = 900;
    std::string t = s.size() > kCap ? (s.substr(0, kCap) + "...(已截断, 完整内容在返回值里)") : s;
    OH_LOG_Print(LOG_APP, LOG_INFO, kLogDomain, kLogTag, "%{public}s: %{public}s", what, t.c_str());
}

std::string failureJson(const std::string& why, const char* kind)
{
    std::string out;
    out.reserve(1024);
    out += "{\"ok\":false";
    out += ",\"kind\":" + jstr(kind);
    out += ",\"section\":\"NPU\"";
    out += ",\"scored\":false";
    out += ",\"gb7Item\":false";
    out += ",\"error\":" + jstr(why);
    out += ",\"text\":" + jstr(why);
    out += ",\"metric\":\"0\"";
    out += ",\"unit\":" + jstr("运行失败: " + why);
    out += ",\"deviceCount\":0";
    out += ",\"devices\":[]";
    out += ",\"scalePresets\":";
    appendScalePresetsJson(out);
    out += ",\"nnrt\":";
    appendNnrtJson(out);
    out += ",\"notes\":" + jstr(std::string(kNotes) +
        " 注意: 这是兜底失败结果(异常路径), 不是 0 分, 请按失败显示。");
    out += "}";
    return out;
}

} // namespace

// ===========================================================================
// 对外接口(napi 层只做 Promise 包装, 不做任何逻辑)
// ===========================================================================
std::string npuProbeJson()
{
    // NNRt 不支持多线程调用 -> 串行化, 防止连点两次时两个 worker 同时进出。
    std::lock_guard<std::mutex> lk(g_npuLock);
    // 运行时绑定 NNRt(只做一次, 结果缓存)。放在锁内 -> 天然单线程;
    // 这是普通上下文, 与 crash_guard.cpp 的信号处理器场景不同, dlopen 在这里是安全的。
    ensureNnrtBound();
    std::string out;
    try {
        out = probeJsonImpl();
    } catch (const std::exception& ex) {
        out = failureJson(std::string("NPU 探测异常: ") + ex.what(), "npu-probe");
    } catch (...) {
        out = failureJson("NPU 探测异常: 未知 C++ 异常", "npu-probe");
    }
    logJson("npuProbe", out);
    return out;
}

std::string npuBenchJson(const NpuBenchRequest& req)
{
    std::lock_guard<std::mutex> lk(g_npuLock);
    ensureNnrtBound();
    std::string out;
    try {
        out = benchJsonImpl(req);
    } catch (const std::exception& ex) {
        out = failureJson(std::string("NPU 跑分异常: ") + ex.what(), "npu-bench");
    } catch (...) {
        out = failureJson("NPU 跑分异常: 未知 C++ 异常(可能是内存不足)", "npu-bench");
    }
    logJson("npuBench", out);
    return out;
}

std::string npuFailureJson(const std::string& why)
{
    return failureJson(why, "npu-bench");
}
