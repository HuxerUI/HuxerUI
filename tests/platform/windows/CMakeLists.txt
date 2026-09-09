huxerui_add_test_suite(huxerui_windows_tests HuxerUIWindowsTests
        LABELS platform windows native
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_sources(huxerui_windows_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/color_stream.cpp"
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/windows_color_stream.cpp"
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/windows_timer.cpp"
        accessibility.cpp
        application.cpp
        damage.cpp
        external_texture.cpp
        file.cpp
        http.cpp
        platform_module.cpp
        platform_view.cpp
        renderer.cpp
        window_chrome.cpp
)
target_include_directories(huxerui_windows_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module"
)
target_link_libraries(huxerui_windows_tests PRIVATE ws2_32)
target_compile_definitions(huxerui_windows_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)
target_include_directories(huxerui_windows_tests PRIVATE "${HUXERUI_PROJECT_DIR}/platform/windows")
target_compile_definitions(huxerui_windows_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)
