set(HUXERUI_OHOS_CPP_DIR "${HUXERUI_PROJECT_DIR}/platform/ohos/huxerui/src/main/cpp")
set(HUXERUI_PLATFORM_SOURCE_FILES
        "${HUXERUI_OHOS_CPP_DIR}/ohos_accessibility.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_adapter.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_background_tasks.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_bitmap.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_clipboard.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_external_texture.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_file_picker.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_http.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_platform_registry.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_platform_view.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_renderer.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_system_services.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_text_input.cpp"
        "${HUXERUI_OHOS_CPP_DIR}/ohos_text_layout.cpp"
)
set(HUXERUI_PLATFORM_COMPILE_OPTIONS -fexperimental-library)
set(HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
set(HUXERUI_PLATFORM_LINK_OPTIONS -fexperimental-library)
_huxerui_ohos_link_libraries(HUXERUI_PLATFORM_LINK_LIBRARIES)
