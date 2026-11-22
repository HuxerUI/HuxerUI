set(HUXERUI_OHOS_CPP_DIR "${HUXERUI_PROJECT_DIR}/platform/ohos/huxerui/src/main/cpp")
set(HUXERUI_PLATFORM_SOURCE_FILES
        "${HUXERUI_OHOS_CPP_DIR}/ohos_adapter.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_bitmap.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_clipboard.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_external_texture.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_file_picker.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_http.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_platform_view.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_renderer.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_system_services.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_text_input.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_text_layout.cpp"
)
set(HUXERUI_PLATFORM_LINK_LIBRARIES
        ace_napi.z ace_ndk.z dl hilog_ndk.z icu image_packer image_source native_buffer native_drawing native_window
        ohfileuri pasteboard pixelmap rawfile.z udmf EGL GLESv3
)
