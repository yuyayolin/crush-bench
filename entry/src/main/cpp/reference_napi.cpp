#include "napi/native_api.h"

#include "reference_compare.h"

#include <dlfcn.h>
#include <hilog/log.h>

#include <string>

// ---------------------------------------------------------------------------
//  参考分对照模块的 NAPI 包装: libauroraref.so / auroraref
//  导出:
//    version()        -> string   真值表版本(改动真值表要 +1)
//    compare(json)    -> string   对照表(契约见 reference_compare.h)
//    realUse(name)    -> string   单项真实用途(查不到返回空串)
//    compareReports(jsonA, jsonB, options?) -> string
//                              两份报告(两台设备各跑一次生成的 report-latest.json 全文)的
//                              纯计算对比: 逐项分数比 + 每核/同频归一化比 + 与真值的偏差百分比。
//                             缺输入就写缺什么, 不编数(契约见 reference_compare.h)。
//
//   本模块只读: 不跑负载、不改分数、不参与计分。它也不硬链
//    libaurorabench.so / libauroragpu7.so / libaurorasn.so —— 独立 .so 的原因与
//    工程对 NNRt 的结论一致: 可选功能不该有让主功能加载失败的能力。
//   看门狗标记: 与其它模块一样, 第一次用到时解析一次 libaurorabench 的
//    auroraSetIdleMarker(), 让卡死取证的"当前项"在对照表生成后回到空闲。
// ---------------------------------------------------------------------------

namespace {

const char* kTag = "AuroraRefNapi";

napi_value MakeString(napi_env env, const std::string& s)
{
    napi_value out = nullptr;
    napi_create_string_utf8(env, s.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

void DropPendingException(napi_env env)
{
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        napi_value ignored = nullptr;
        napi_get_and_clear_last_exception(env, &ignored);
    }
}

typedef void (*AuroraSetIdleMarkerFn)(void);
int g_idleMode = 0;
napi_ref g_idleFnRef = nullptr;
napi_ref g_idleNsRef = nullptr;
AuroraSetIdleMarkerFn g_idleSym = nullptr;

void RefMarkIdle(napi_env env)
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
            DropPendingException(env);
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
                         "idle marker unreachable: 对照表生成后不写空闲标记, 其余行为不受影响");
        }
    }
    if (g_idleMode == 1 && env != nullptr) {
        napi_value self = nullptr;
        napi_value fn = nullptr;
        if (napi_get_reference_value(env, g_idleNsRef, &self) == napi_ok &&
            napi_get_reference_value(env, g_idleFnRef, &fn) == napi_ok && fn != nullptr) {
            napi_value res = nullptr;
            if (napi_call_function(env, self, fn, 0, nullptr, &res) != napi_ok) {
                DropPendingException(env);
            }
        }
        return;
    }
    if (g_idleMode == 2 && g_idleSym != nullptr) {
        g_idleSym();
    }
}

struct RefIdleGuard {
    napi_env env;
    explicit RefIdleGuard(napi_env e) : env(e) {}
    ~RefIdleGuard() { RefMarkIdle(env); }
};

// 把一个 JS 参数读成字符串(拿不到 -> 空串)
std::string StringOf(napi_env env, napi_value v)
{
    std::string out;
    if (v == nullptr) {
        return out;
    }
    napi_valuetype vt = napi_undefined;
    if (napi_typeof(env, v, &vt) != napi_ok || vt != napi_string) {
        return out;
    }
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok || len == 0) {
        return out;
    }
    std::string buf(len, '\0');
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, v, buf.data(), len + 1, &copied) != napi_ok) {
        return std::string();
    }
    buf.resize(copied);
    return buf;
}

std::string ReadStringArg(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string out;
    if (argc > 0 && args[0] != nullptr) {
        napi_valuetype vt = napi_undefined;
        if (napi_typeof(env, args[0], &vt) == napi_ok && vt == napi_string) {
            size_t len = 0;
            if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) == napi_ok && len > 0) {
                std::string buf(len, '\0');
                size_t copied = 0;
                if (napi_get_value_string_utf8(env, args[0], buf.data(), len + 1, &copied) == napi_ok) {
                    buf.resize(copied);
                    out = buf;
                }
            }
        }
    }
    return out;
}

napi_value RefVersion(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, auroraReferenceVersionText());
}

napi_value RefCompare(napi_env env, napi_callback_info info)
{
    const std::string opts = ReadStringArg(env, info);
    RefIdleGuard guard(env);
    std::string json;
    try {
        json = auroraReferenceCompare(opts);
    } catch (const std::exception& e) {
        json = std::string("{\"ok\":false,\"error\":\"exception: ") + e.what() + "\"}";
    } catch (...) {
        json = "{\"ok\":false,\"error\":\"unknown exception\"}";
    }
    return MakeString(env, json);
}

napi_value RefRealUse(napi_env env, napi_callback_info info)
{
    const std::string name = ReadStringArg(env, info);
    return MakeString(env, auroraReferenceRealUse(name));
}

// compareReports(jsonA, jsonB, options?) -> string
//   两份报告(report-latest.json 全文)的纯计算对比: 逐项分数比 + 归一化比 + 与真值的偏差。
//    只读: 不跑负载 / 不连设备 / 不写文件 
napi_value RefCompareReports(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string jsonA = StringOf(env, args[0]);
    const std::string jsonB = StringOf(env, args[1]);
    const std::string opts = StringOf(env, args[2]);
    RefIdleGuard guard(env);
    std::string json;
    try {
        json = auroraReferenceCompareReports(jsonA, jsonB, opts);
    } catch (const std::exception& e) {
        json = std::string("{\"ok\":false,\"error\":\"exception: ") + e.what() + "\"}";
    } catch (...) {
        json = "{\"ok\":false,\"error\":\"unknown exception\"}";
    }
    return MakeString(env, json);
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"version", nullptr, RefVersion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"compare", nullptr, RefCompare, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"realUse", nullptr, RefRealUse, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 两份报告对比(纯计算 / 只读); 详见 reference_compare.h 的契约
        {"compareReports", nullptr, RefCompareReports, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "auroraref module registered (truth v%d)",
                 kReferenceVersion);
    return exports;
}
EXTERN_C_END

static napi_module g_refModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "auroraref",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterAuroraRef(void)
{
    napi_module_register(&g_refModule);
}
