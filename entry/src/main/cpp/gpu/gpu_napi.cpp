#include "napi/native_api.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include "gpu_renderer.h"
#include <hilog/log.h>
#include <cstdint>
#include <string>

namespace {

const char* kTag = "AuroraGpuNapi";
OH_NativeXComponent* g_component = nullptr;

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window)
{
    char idStr[OH_XCOMPONENT_ID_LEN_MAX + 1] = {0};
    uint64_t idSize = OH_XCOMPONENT_ID_LEN_MAX + 1;
    if (OH_NativeXComponent_GetXComponentId(component, idStr, &idSize) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x1234, kTag, "get id failed");
        return;
    }
    uint64_t w = 0;
    uint64_t h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    gpuSetWindow(window, (int)w, (int)h);
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window)
{
    uint64_t w = 0;
    uint64_t h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    gpuSetSize((int)w, (int)h);
}

void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* window)
{
    (void)component;
    (void)window;
    gpuDestroySurface();
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

napi_value MakeString(napi_env env, const std::string& s)
{
    napi_value out = nullptr;
    napi_create_string_utf8(env, s.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value GpuSceneCount(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_create_int32(env, gpuSceneCount(), &out);
    return out;
}

napi_value GpuSceneName(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t id = 0;
    napi_get_value_int32(env, args[0], &id);
    return MakeString(env, gpuSceneName(id));
}

napi_value GpuReady(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out = nullptr;
    napi_get_boolean(env, gpuSurfaceReady(), &out);
    return out;
}

// 无窗口初始化: 不依赖 XComponent 画布
napi_value GpuPrepare(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, gpuInitHeadless());
}

napi_value GpuStart(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t scene = 0;
    int32_t frames = 240;
    napi_get_value_int32(env, args[0], &scene);
    if (argc > 1) {
        napi_get_value_int32(env, args[1], &frames);
    }
    return MakeString(env, gpuStartScene(scene, frames));
}

napi_value GpuPoll(napi_env env, napi_callback_info info)
{
    (void)info;
    return MakeString(env, gpuPoll());
}

napi_value GpuStop(napi_env env, napi_callback_info info)
{
    (void)info;
    gpuStop();
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"sceneCount", nullptr, GpuSceneCount, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sceneName", nullptr, GpuSceneName, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ready", nullptr, GpuReady, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepare", nullptr, GpuPrepare, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"start", nullptr, GpuStart, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"poll", nullptr, GpuPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, GpuStop, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) == napi_ok) {
        OH_NativeXComponent* component = nullptr;
        if (napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&component)) == napi_ok && component != nullptr) {
            g_component = component;
            OH_NativeXComponent_RegisterCallback(component, getCallback());
            OH_LOG_Print(LOG_APP, LOG_INFO, 0x1234, kTag, "xcomponent registered");
        }
    }
    return exports;
}
EXTERN_C_END

static napi_module gpuModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "auroragpu",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterAuroraGpu(void)
{
    napi_module_register(&gpuModule);
}
