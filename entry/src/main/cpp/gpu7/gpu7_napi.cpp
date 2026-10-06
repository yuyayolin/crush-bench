#include "napi/native_api.h"

#include "gpu7_renderer.h"

#include <dlfcn.h>
#include <hilog/log.h>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
//  gpu7 NAPI 模块: libauroragpu7.so / auroragpu7
//  导出属性与 gpu/gpu_napi.cpp 保持一致风格:
//      count / name / ready / prepare / run / lastError
//  离屏渲染, 不需要 XComponent 回调(保留一个空的注册分支以防上层仍然传入)。
// ---------------------------------------------------------------------------

// 需要 XComponent 时才引入(本模块不使用窗口渲染), 用 __has_include 兜住
#if defined(__has_include)
#if __has_include(<ace/xcomponent/native_interface_xcomponent.h>)
#define GPU7_HAS_XCOMPONENT 1
#endif
#endif

#ifdef GPU7_HAS_XCOMPONENT
#include <ace/xcomponent/native_interface_xcomponent.h>
#endif

namespace {

const char* kTag = "AuroraGpu7Napi";

#ifdef GPU7_HAS_XCOMPONENT

// 离屏渲染不需要任何真实的 surface 回调, 这里只是保留空实现,
// 以便上层仍然按老方式把 XComponent 传进来时不会崩。
void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window)
{
    (void)component;
    (void)window;
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "gpu7 ignores xcomponent surface (offscreen)");
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window)
{
    (void)component;
    (void)window;
}

void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* window)
{
    (void)component;
    (void)window;
}

void DispatchTouchEventCB(OH_NativeXComponent* component, void* window)
{
    (void)component;
    (void)window;
}

OH_NativeXComponent_Callback* getCallback()
{
    static OH_NativeXComponent_Callback cb;
    cb.OnSurfaceCreated = OnSurfaceCreatedCB;
    cb.OnSurfaceChanged = OnSurfaceChangedCB;
    cb.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
    cb.DispatchTouchEvent = DispatchTouchEventCB;
    return &cb;
}

#endif // GPU7_HAS_XCOMPONENT

napi_value MakeString(napi_env env, const std::string& s)
{
    napi_value out = nullptr;
    napi_create_string_utf8(env, s.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// ---------------------------------------------------------------------------
// GPU 阶段的"收尾"标记 —— 修的是看门狗对已结束的 GPU 项每秒误报卡死(2026-10-07)
//
// 现场(hang64.txt, 160,606 字节): 最后 4 条记录的 marker 全是
//   phase="CS1 GPU" item="Video Filter" index=11/11 elapsed_s=203/204/205/206
// 即 CS1 GPU 阶段跑完之后, 最后一项的标记没有被置成空闲: 看门狗看到"Video Filter
// 已经跑了 203 秒", 于是判卡死、采样、把现场写盘(每条几十 KB)。
//
// 根因: 卡死取证的"当前项"标记(phase/item/index/total/g_idle)住在 **libaurorabench.so**
// 的 crash_guard.cpp 里, 而 CS1 GPU 的每一项是在 本模块(libauroragpu7.so)里跑完的。
// ArkTS 只在本项开跑前调一次 setCurrentItem('CS1 GPU', ..)(那是 libaurorabench 的导出),
// 跑完这一项之后没有任何人把标记清成空闲 —— 单核 / 多核 / CoreMark / 存储小节都在 native
// 里各自 markIdle(), 只有走本模块的 GPU 路径漏了这一步。
//
// 为什么不能直接链接: libauroragpu7 与 libaurorabench 是两个独立的 napi 模块, 给 GPU 模块
// 加一条对跑分主模块的硬 DT_NEEDED, 就等于让"GPU 模块加载失败"有能力把整轮跑分一起拖死 ——
// 本工程对 NNRt 已经有过一模一样的结论(见 CMakeLists 里那段长注释)。
// 所以改成"第一次用到时解析一次, 解析不到就整条降级(只记一次 warn)", 解析分三级:
//   ① napi_load_module("aurorabench") + 取导出 setIdleMarker() —— 走 JS 运行时的模块注册表,
//      拿到的一定是 ArkTS 正在用的那一个实例(napi_ref 常驻, 不随 handle scope 失效);
//   ② dlopen("libaurorabench.so") + dlsym("auroraSetIdleMarker") —— 这一步只是兜底:
//      libaurorabench.so 此刻必然已加载(ArkTS 先 import 它才会走到 GPU 阶段), 但"按名字
//      dlopen 是否命中已加载实例"取决于运行时的装载命名, 所以它排在 napi 之后, 而不是之前;
//   ③ dlsym(RTLD_DEFAULT, ..) —— 运行时若以 RTLD_GLOBAL 装载, 这一步直接就命中。
// 三级都失败时不静默: 记一条 warn(见 gpu7MarkIdle)。
// ---------------------------------------------------------------------------
typedef void (*AuroraSetIdleMarkerFn)(void);

// 解析分三级, 只用前一级失败时才知道下一级有没有用:
//   级别 1(首选): napi_load_module("aurorabench") -> 取导出 setIdleMarker() 再调用。
//       走的是 JS 运行时自己的模块注册表, 所以拿到的一定是 ArkTS 正在用的那一个
//       libaurorabench.so 实例 —— 从根上排除了"dlopen 出第二份副本、标记写到别的实例里去"。
//   级别 2(兜底): dlopen("libaurorabench.so") + dlsym("auroraSetIdleMarker")。
//   级别 3(兜底): dlsym(RTLD_DEFAULT, "auroraSetIdleMarker")(运行时若以 RTLD_GLOBAL 装载)。
//   三级都失败: 只记一条 hilog warn(不静默), 之后什么也不做 —— 不因为"写不了标记"
//   而影响任何 GPU 负载的执行与结果。
int g_idleMode = 0;                        // 0 = 还没解析; 1 = 走 JS 模块; 2 = 走符号; -1 = 不可用
napi_ref g_idleFnRef = nullptr;            // 级别 1: setIdleMarker 的持久引用
napi_ref g_idleNsRef = nullptr;            // 级别 1: 模块命名空间的持久引用(调用时的 this)
AuroraSetIdleMarkerFn g_idleSym = nullptr; // 级别 2/3: 直接函数地址

// napi 失败时若留下"待处理异常", 必须就地清掉 —— 否则这条异常会顺着 napi 边界冒到 JS 层,
// 变成一条与本功能毫不相干的报错。
void gpu7DropPendingException(napi_env env)
{
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        napi_value ignored = nullptr;
        napi_get_and_clear_last_exception(env, &ignored);
    }
}

// 把标记置成"无负载运行中"(空闲): 看门狗据此不判定卡死(见 crash_guard.cpp 的 g_idle)。
void gpu7MarkIdle(napi_env env)
{
    if (g_idleMode == 0) {
        g_idleMode = -1;
        if (env != nullptr) {
            napi_value mod = nullptr;
            if (napi_load_module(env, "aurorabench", &mod) == napi_ok && mod != nullptr) {
                napi_value fn = nullptr;
                if (napi_get_named_property(env, mod, "setIdleMarker", &fn) == napi_ok && fn != nullptr) {
                    napi_valuetype vt = napi_undefined;
                    if (napi_typeof(env, fn, &vt) == napi_ok && vt == napi_function &&
                        napi_create_reference(env, fn, 1, &g_idleFnRef) == napi_ok &&
                        napi_create_reference(env, mod, 1, &g_idleNsRef) == napi_ok) {
                        g_idleMode = 1;
                    }
                }
            }
            gpu7DropPendingException(env);
        }
        if (g_idleMode != 1) {
            void* handle = dlopen("libaurorabench.so", RTLD_NOW | RTLD_LOCAL);
            if (handle != nullptr) {
                g_idleSym = (AuroraSetIdleMarkerFn)dlsym(handle, "auroraSetIdleMarker");
            }
            if (g_idleSym == nullptr) {
                g_idleSym = (AuroraSetIdleMarkerFn)dlsym(RTLD_DEFAULT, "auroraSetIdleMarker");
            }
            if (g_idleSym != nullptr) {
                g_idleMode = 2;
            }
        }
        if (g_idleMode == -1) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0x1234, kTag,
                         "idle marker unreachable (napi module and dlopen both failed): "
                         "GPU 项结束后不写空闲标记, 看门狗可能对已结束的 GPU 项误报; "
                         "其余行为不受影响");
        } else {
            OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag,
                         "idle marker bound via %{public}s", (g_idleMode == 1) ? "napi module" : "dlopen");
        }
    }
    if (g_idleMode == 1 && env != nullptr) {
        napi_value self = nullptr;
        napi_value fn = nullptr;
        if (napi_get_reference_value(env, g_idleNsRef, &self) == napi_ok &&
            napi_get_reference_value(env, g_idleFnRef, &fn) == napi_ok && fn != nullptr) {
            napi_value res = nullptr;
            if (napi_call_function(env, self, fn, 0, nullptr, &res) != napi_ok) {
                gpu7DropPendingException(env);
            }
        }
        return;
    }
    if (g_idleMode == 2 && g_idleSym != nullptr) {
        g_idleSym();
    }
}

// 作用域守卫: 正常返回 / 提前 return / C++ 异常展开, 三条路径都会走到析构 -> 一定收尾。
struct Gpu7IdleGuard {
    napi_env env;
    explicit Gpu7IdleGuard(napi_env e) : env(e) {}
    ~Gpu7IdleGuard() { gpu7MarkIdle(env); }
};

napi_value Gpu7Count(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_create_int32(env, gpu7Count(), &out);
    return out;
}

napi_value Gpu7Name(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    if (argc > 0 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &id);
    }
    return MakeString(env, gpu7Name(id));
}

napi_value Gpu7Ready(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_get_boolean(env, gpu7Ready(), &out);
    return out;
}

napi_value Gpu7Prepare(napi_env env, napi_callback_info info)
{
    (void)info;
    // 整个 GPU 阶段从这里开始: 无论 prepare 成功还是失败(失败时 ArkTS 会直接放弃本轮 GPU),
    // 都必须在出口把标记收尾 —— 失败路径同样不能让标记停在"有负载在跑"。
    Gpu7IdleGuard idleGuard(env);
    return MakeString(env, gpu7Prepare());
}

napi_value Gpu7Run(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    if (argc > 0 && args[0] != nullptr) {
        if (napi_get_value_int32(env, args[0], &id) != napi_ok) {
            id = -1;
        }
    } else {
        id = -1;
    }
    // 每一项跑完就置空闲标记(正常返回 / 提前 return / 异常展开都覆盖, 见 Gpu7IdleGuard):
    //   ArkTS 只在本项开跑前写一次 setCurrentItem, 收尾必须由这里补上 ——
    //   否则最后一项(index=11/11 "Video Filter")的标记会一直留着, 看门狗按
    //   "已跑 N 秒"每秒误报一次卡死(真机现场见 hang64.txt)。
    //   注意: 本项执行中标记仍是 busy(ArkTS 在调用 gpu7Run 之前刚写过),
    //   所以 GPU 负载真的卡死时, 看门狗照常判得出来。
    Gpu7IdleGuard idleGuard(env);
    return MakeString(env, gpu7Run((int)id));
}

// fullness(id) -> JSON 字符串: GPU「跑满判据」的一次探测(计时区间之外, 结果丢弃)。
//   与 run() 的关系: run() 照旧只跑正式那一遍并计分; fullness() 在它之前单独跑两档探测,
//   探测的读数一个都不进 metric / score / 复合分。两项都不改负载的尺寸 / 算法 / 帧数。
//   收尾标记同样由 Gpu7IdleGuard 保证(探测也是"有负载在跑", 卡死判据必须照常有效)。
napi_value Gpu7Fullness(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    if (argc > 0 && args[0] != nullptr) {
        if (napi_get_value_int32(env, args[0], &id) != napi_ok) {
            id = -1;
        }
    } else {
        id = -1;
    }
    Gpu7IdleGuard idleGuard(env);
    return MakeString(env, gpu7Fullness((int)id));
}

// composite(scores?: number[]) -> JSON 字符串
//   不传数组时用最近一次 run() 的单项分(按负载 id 索引)
napi_value Gpu7Composite(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double buf[16];
    double* scores = nullptr;
    uint32_t n = 0;
    bool isArray = false;
    if (argc > 0 && args[0] != nullptr &&
        napi_is_array(env, args[0], &isArray) == napi_ok && isArray &&
        napi_get_array_length(env, args[0], &n) == napi_ok && n > 0) {
        if (n > 16) {
            n = 16;
        }
        for (uint32_t i = 0; i < n; ++i) {
            napi_value v = nullptr;
            buf[i] = 0.0;
            if (napi_get_element(env, args[0], i, &v) == napi_ok) {
                napi_get_value_double(env, v, &buf[i]);
            }
        }
        scores = buf;
    }
    return MakeString(env, gpu7Composite(scores, (int)n));
}

napi_value Gpu7LastError(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, gpu7LastError());
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"count", nullptr, Gpu7Count, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"name", nullptr, Gpu7Name, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ready", nullptr, Gpu7Ready, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepare", nullptr, Gpu7Prepare, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"run", nullptr, Gpu7Run, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"fullness", nullptr, Gpu7Fullness, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"composite", nullptr, Gpu7Composite, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"lastError", nullptr, Gpu7LastError, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

#ifdef GPU7_HAS_XCOMPONENT
    // 空的 XComponent 注册分支: 上层若仍然把组件传进来, 只登记回调不做窗口渲染
    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) == napi_ok) {
        OH_NativeXComponent* component = nullptr;
        if (napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&component)) == napi_ok &&
            component != nullptr) {
            OH_NativeXComponent_RegisterCallback(component, getCallback());
            OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "gpu7 xcomponent branch (no-op)");
        }
    }
#endif
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "auroragpu7 module registered");
    return exports;
}
EXTERN_C_END

static napi_module gpu7Module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "auroragpu7",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterAuroraGpu7(void)
{
    napi_module_register(&gpu7Module);
}
