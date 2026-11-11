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
        installer_command_policy.cpp
        platform_module.cpp
        platform_view.cpp
        renderer.cpp
        window_chrome.cpp
)

# The installer command policy is declared in terms of WiX Burn engine enums, so the test target
# needs the pinned bootstrapper application API headers (definitions only; no engine link).
include("${HUXERUI_PROJECT_DIR}/cmake/HuxerUIWindowsInstaller.cmake")
if (NOT HUXERUI_WIX_ROOT)
    set(HUXERUI_WIX_ROOT "${CMAKE_BINARY_DIR}/huxerui-wix-tests")
endif ()
set(HUXERUI_WIX_VERSION "5.0.2")
_huxerui_restore_wix_package(
        "WixToolset.BootstrapperApplicationApi"
        "6e0d3c68a68dcedde4a3a68de896f124a7b19c4a823fac49856e2ee77cb16256"
        "${HUXERUI_WIX_ROOT}/bootstrapper"
        "build/native/include/BootstrapperApplication.h"
)
target_include_directories(huxerui_windows_tests PRIVATE
        "${HUXERUI_WIX_ROOT}/bootstrapper/build/native/include"
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
