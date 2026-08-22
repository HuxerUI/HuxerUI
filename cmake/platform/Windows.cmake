if (HUXERUI_WINDOWS_7_COMPAT)
    set(HUXERUI_WINDOWS_VERSION_DEFINITIONS
            HUXERUI_WINDOWS_7_COMPAT=1
            WINVER=0x0601
            _WIN32_WINNT=0x0601
    )
else ()
    set(HUXERUI_WINDOWS_VERSION_DEFINITIONS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
    )
endif ()

set(HUXERUI_PLATFORM_SOURCE_FILES
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_adapter.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_accessibility.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_application.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_external_texture.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_file.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_file_picker.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_http.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_platform_view.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_renderer.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_text_input.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/windows/win32_ui_dispatcher.cpp"
)
set(HUXERUI_PLATFORM_COMPILE_DEFINITIONS
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        ${HUXERUI_WINDOWS_VERSION_DEFINITIONS}
)
set(HUXERUI_PLATFORM_LINK_LIBRARIES
        d2d1
        d3d11
        dwrite
        dwmapi
        dxguid
        dxgi
        imm32
        ole32
        oleaut32
        psapi
        shell32
        uiautomationcore
        user32
        winhttp
        windowscodecs
)
if (NOT HUXERUI_WINDOWS_7_COMPAT)
    list(APPEND HUXERUI_PLATFORM_LINK_LIBRARIES dcomp)
endif ()
