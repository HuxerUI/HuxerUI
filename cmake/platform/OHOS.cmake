set(HUXERUI_OHOS_CPP_DIR "${HUXERUI_PROJECT_DIR}/platform/ohos/huxerui/src/main/cpp")
set(HUXERUI_PLATFORM_SOURCE_FILES
        "${HUXERUI_OHOS_CPP_DIR}/ohos_adapter.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_renderer.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_text_layout.cpp"
)
set(HUXERUI_PLATFORM_LINK_LIBRARIES ace_napi.z ace_ndk.z hilog_ndk.z icu native_drawing native_window)
