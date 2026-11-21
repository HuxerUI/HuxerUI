#include <napi/native_api.h>

namespace huxerui::detail {
napi_value InitializeOhosModule(napi_env env, napi_value exports);
} // namespace huxerui::detail

namespace {

napi_module app_module{
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = huxerui::detail::InitializeOhosModule,
    .nm_modname = HUXERUI_OHOS_APP_MODULE_NAME,
    .nm_priv = nullptr,
    .reserved = {},
};

__attribute__((constructor)) void RegisterHuxerUIAppModule() {
  napi_module_register(&app_module);
}

} // namespace
