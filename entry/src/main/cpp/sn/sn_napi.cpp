#include "napi/native_api.h"

#include "sn_renderer.h"

#include <dlfcn.h>
#include <hilog/log.h>

#include <string>

// ---------------------------------------------------------------------------
//  Aurora Nomad Light (SNL) 小节的 NAPI 模块: libaurorasn.so / aurorasn
//  导出(风格与 auroragpu7 一致, 便于 ArkTS 侧接线):
//      benchVersion : number         benchmark 级版本号(与 App 版本号无关)
//      versionText  : string         含"跨版本不可比"的完整说明
//      sectionName  : string         "GPU-SNL"(UI 用它把本小节单独显示)
//      workloadName : string         "Aurora Nomad Light"
//      ready        : boolean        离屏上下文是否已就绪
//      prepare      : string         "" = 成功, 否则错误文本(同步)
//      run          : string         run(optionsJson?: string) -> 结果 JSON(同步)
//      lastError    : string         最近一次错误文本
//      lastScore    : number         上一轮分数(0 = 还没跑过)
//
//   本模块与 libaurorabench.so / libauroragpu7.so 之间没有任何硬依赖:
//    独立 .so = 本小节加载失败不会拖死跑分主模块(与工程对 NNRt 的结论一致)。
//
//  看门狗标记: 与 gpu7 一样, 本模块也用"第一次用到时解析一次"的方式去调
//  libaurorabench.so 导出的 auroraSetIdleMarker(), 让卡死取证里"当前项"的标记
//  在本项跑完后回到空闲 —— 否则看门狗会对已经跑完的 GPU 小节每秒误报卡死
//  (真机现场见 gpu7_napi.cpp 顶部那段说明)。解析不到就只记一条 warn, 整条降级。
// ---------------------------------------------------------------------------

namespace {

const char* kTag = "AuroraSnNapi";

// 前置声明: 定义在下面(清理待处理异常, 免得 napi 失败留下的一条无关异常冒到 JS 层)。
void DropPendingException(napi_env env);

// napi_status -> 可读名字(只用于失败文本, 不含任何引号/反斜杠, 拼进 JSON 是安全的)
const char* NapiStatusName(napi_status st)
{
    switch (st) {
        case napi_ok: return "napi_ok";
        case napi_invalid_arg: return "napi_invalid_arg";
        case napi_object_expected: return "napi_object_expected";
        case napi_string_expected: return "napi_string_expected";
        case napi_name_expected: return "napi_name_expected";
        case napi_function_expected: return "napi_function_expected";
        case napi_number_expected: return "napi_number_expected";
        case napi_boolean_expected: return "napi_boolean_expected";
        case napi_array_expected: return "napi_array_expected";
        case napi_generic_failure: return "napi_generic_failure";
        case napi_pending_exception: return "napi_pending_exception";
        case napi_cancelled: return "napi_cancelled";
        case napi_escape_called_twice: return "napi_escape_called_twice";
        case napi_handle_scope_mismatch: return "napi_handle_scope_mismatch";
        default: return "napi_status(未知)";
    }
}

//  2026-10-05 加固  —— 这个函数以前完全忽略 napi_create_string_utf8 的返回值:
//   一旦构造失败(参数非法 / 内存不足), out 会一直是 nullptr, 函数就把 nullptr 交回 JS。
//   JS 侧拿到的是 undefined, 而 BenchRunner.runSn() -> parseSn() 会把它当成"解析失败",
//   最后界面上只剩一句「native 返回失败：（native 未给文本）」——
//   真机上"native 返回失败但一个字都没有"的观感就是从这类地方来的。
//   现在: 主串建不出来就退到一条能读的失败 JSON, 再建不出来退到手写常量,
//   实在不行返回 undefined(此时已经没有别的办法, 但至少不会返回 nullptr 让 JS 误解)。
napi_value MakeString(napi_env env, const std::string& s)
{
    napi_value out = nullptr;
    const napi_status st = napi_create_string_utf8(env, s.c_str(), s.size(), &out);
    if (st == napi_ok && out != nullptr) {
        return out;
    }
    DropPendingException(env);
    const std::string fallback =
        std::string("{\"ok\":false,\"section\":\"GPU-SNL\",\"error\":\"native 结果字符串构造失败"
                    "(napi_create_string_utf8 -> ") + NapiStatusName(st) +
        ", 结果长度 " + std::to_string(s.size()) +
        " 字节); 本节结果不可用, 其它小节不受影响。\",\"lastError\":\"\"}";
    napi_value out2 = nullptr;
    if (napi_create_string_utf8(env, fallback.c_str(), fallback.size(), &out2) == napi_ok && out2 != nullptr) {
        return out2;
    }
    DropPendingException(env);
    static const char kHard[] =
        "{\"ok\":false,\"section\":\"GPU-SNL\",\"error\":\"native 结果字符串构造失败(连失败文本也建不出来)\",\"lastError\":\"\"}";
    napi_value out3 = nullptr;
    if (napi_create_string_utf8(env, kHard, sizeof(kHard) - 1, &out3) == napi_ok && out3 != nullptr) {
        return out3;
    }
    DropPendingException(env);
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

// napi 失败若留下待处理异常, 必须就地清掉, 否则会冒到 JS 层变成一条无关报错
void DropPendingException(napi_env env)
{
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        napi_value ignored = nullptr;
        napi_get_and_clear_last_exception(env, &ignored);
    }
}

typedef void (*AuroraSetIdleMarkerFn)(void);

int g_idleMode = 0;                        // 0 = 未解析; 1 = 走 JS 模块; 2 = 走 dlopen 符号; -1 = 不可用
napi_ref g_idleFnRef = nullptr;
napi_ref g_idleNsRef = nullptr;
AuroraSetIdleMarkerFn g_idleSym = nullptr;

void SnMarkIdle(napi_env env)
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
                         "idle marker unreachable (napi module and dlopen both failed): "
                         "GPU-SNL 结束后不写空闲标记, 看门狗可能对已结束的项误报; 其余行为不受影响");
        } else {
            OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "idle marker bound via %{public}s",
                         (g_idleMode == 1) ? "napi module" : "dlopen");
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

// 作用域守卫: 正常返回 / 提前 return / C++ 异常展开三条路径都会收尾
struct SnIdleGuard {
    napi_env env;
    explicit SnIdleGuard(napi_env e) : env(e) {}
    ~SnIdleGuard() { SnMarkIdle(env); }
};

// optionsJson 参数 -> std::string(缺省 / 类型不对时当空串 = 全默认)
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

// 兜底失败结果: 保证 ok=false 且 error 非空(契约: 失败必须带原因, 不许出现
// 「native 返回失败：（native 未给文本）」这种"没有原因"的状态)。
std::string SnFallbackFailJson(const std::string& why)
{
    std::string e;
    for (size_t i = 0; i < why.size(); ++i) {
        const char c = why[i];
        if (c == '"' || c == '\\') {
            e.push_back('\'');
        } else if ((unsigned char)c < 0x20) {
            e.push_back(' ');
        } else {
            e.push_back(c);
        }
    }
    if (e.empty()) {
        e = "snRun() 返回失败但没有给出原因";
    }
    std::string s = "{\"ok\":false,\"section\":\"GPU-SNL\",\"error\":\"" + e +
                    "\",\"lastError\":\"本结果由 libaurorasn.so 的 napi 兜底层生成";
    s += "\"}";
    return s;
}

napi_value SnRun(napi_env env, napi_callback_info info)
{
    const std::string opts = ReadStringArg(env, info);
    SnIdleGuard idleGuard(env); // 本项跑完(成功/失败/异常)都把看门狗标记置回空闲
    std::string json;
    try {
        json = snRun(opts);
    } catch (const std::exception& e) {
        json = SnFallbackFailJson(std::string("C++ 异常: ") + e.what());
    } catch (...) {
        json = SnFallbackFailJson("C++ 未知异常");
    }
    //  交付前最后一次检查 : 结果必须是一段非空、且带 "ok" 字段的文本。
    //   否则调用方的 JSON.parse 会抛异常, 界面上就只剩「native 未给文本」。
    if (json.empty() || json.find("\"ok\"") == std::string::npos) {
        json = SnFallbackFailJson("snRun() 返回了空文本或不含 ok 字段的结果(长度 " +
                                  std::to_string(json.size()) + " 字节)");
    }
    return MakeString(env, json);
}

// 故障隔离舱自检(手动入口, 不参与任何跑分流程): 故意在隔离舱里制造一次空指针读,
// 用来在真机上一行证明"这一段崩了, App 依然活着并拿到一条带原因的失败"。
// 它不跑负载、不改任何工作量/口径/分数。
napi_value SnFaultSelfTest(napi_env env, napi_callback_info info)
{
    (void)info;
    SnIdleGuard idleGuard(env);
    std::string json;
    try {
        json = snRun("{\"faultSelfTest\":1}");
    } catch (const std::exception& e) {
        json = SnFallbackFailJson(std::string("C++ 异常: ") + e.what());
    } catch (...) {
        json = SnFallbackFailJson("C++ 未知异常");
    }
    if (json.empty() || json.find("\"ok\"") == std::string::npos) {
        json = SnFallbackFailJson("自检返回了空文本或不含 ok 字段的结果");
    }
    return MakeString(env, json);
}

napi_value SnPrepare(napi_env env, napi_callback_info info)
{
    (void)info;
    SnIdleGuard idleGuard(env);
    return MakeString(env, snPrepare());
}

napi_value SnReady(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_get_boolean(env, snReady(), &out);
    return out;
}

napi_value SnLastError(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, snLastError());
}

napi_value SnLastScore(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_create_double(env, snLastScore(), &out);
    return out;
}

napi_value SnBenchVersion(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_create_int32(env, kSnBenchVersion, &out);
    return out;
}

napi_value SnVersionText(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, snBenchVersionText());
}

napi_value SnSectionName(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, snSectionName());
}

napi_value SnWorkloadName(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, snWorkloadName());
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"benchVersion", nullptr, SnBenchVersion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"versionText", nullptr, SnVersionText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sectionName", nullptr, SnSectionName, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"workloadName", nullptr, SnWorkloadName, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ready", nullptr, SnReady, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepare", nullptr, SnPrepare, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"run", nullptr, SnRun, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"faultSelfTest", nullptr, SnFaultSelfTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"lastError", nullptr, SnLastError, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"lastScore", nullptr, SnLastScore, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "aurorasn module registered (benchVersion=%{public}d)",
                 kSnBenchVersion);
    return exports;
}
EXTERN_C_END

static napi_module g_snModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "aurorasn",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterAuroraSn(void)
{
    napi_module_register(&g_snModule);
}
