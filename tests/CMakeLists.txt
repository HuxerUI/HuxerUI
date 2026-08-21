if (TARGET HuxerUI::huxerui_static)
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui_static)
else ()
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui)
endif ()

include("${HUXERUI_PROJECT_DIR}/3dparty/catch2/huxerui_3p.cmake")

file(TO_CMAKE_PATH "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}" HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE)
add_executable(huxerui_tests
        tests_main.cpp
        unit/animation.cpp
        support/builtin_resources.cpp
        unit/application.cpp
        unit/external_texture.cpp
        unit/file.cpp
        unit/platform_frame.cpp
        unit/paint.cpp
        unit/path.cpp
        unit/platform_payload.cpp
        unit/resource.cpp
        unit/semantics.cpp
        unit/text.cpp
        unit/text_input.cpp
        unit/validation.cpp
        unit/vector.cpp
        platform/android_accessibility.cpp
        "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/cpp/android_accessibility.cpp"
        platform/android_text_input.cpp
        platform/web_text.cpp
        runtime/animation.cpp
        runtime/composition.cpp
        runtime/file.cpp
        runtime/file_picker.cpp
        runtime/http.cpp
        runtime/interaction.cpp
        runtime/layers.cpp
        runtime/layout.cpp
        runtime/lifecycle.cpp
        runtime/navigation.cpp
        runtime/navigation_ui.cpp
        runtime/platform_module.cpp
        runtime/platform_view.cpp
        runtime/presentation.cpp
        runtime/presentation_semantics.cpp
        runtime/render_scene.cpp
        runtime/resources.cpp
        runtime/scrolling.cpp
        runtime/selection_area.cpp
        runtime/task.cpp
        runtime/text_field.cpp
        runtime/text_input.cpp
        runtime/tooltip.cpp
        runtime/virtual_layout.cpp
        runtime/window.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/apple_timer.mm"
            platform/macos_platform_module.mm
            platform/macos_platform_view.mm
            platform/macos_external_texture.mm
            platform/macos_file_picker.mm
            platform/macos_text_input.mm
            platform/macos_window_chrome.cpp
    )
    set_source_files_properties(
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/apple_timer.mm"
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )
    target_include_directories(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module"
    )
endif ()

if (WIN32)
    target_sources(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/color_stream.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/windows_color_stream.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/windows_timer.cpp"
            platform/windows_accessibility.cpp
            platform/windows_damage.cpp
            platform/windows_external_texture.cpp
            platform/windows_file.cpp
            platform/windows_http.cpp
            platform/windows_platform_module.cpp
            platform/windows_platform_view.cpp
            platform/windows_renderer.cpp
            platform/windows_window_chrome.cpp
    )
    target_include_directories(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module"
    )
    target_link_libraries(huxerui_tests PRIVATE ws2_32)
    target_compile_definitions(huxerui_tests PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
    )
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/linux_timer.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
            platform/linux_damage.cpp
            platform/linux_external_texture.cpp
            platform/linux_file.cpp
            platform/linux_file_picker.cpp
            platform/linux_http.cpp
            platform/linux_platform_module.cpp
            platform/linux_renderer.cpp
            platform/linux_text_input.cpp
            platform/linux_window_chrome.cpp
    )
    target_include_directories(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module"
    )
    target_link_libraries(huxerui_tests PRIVATE PkgConfig::HUXERUI_GIO)
endif ()

if (EMSCRIPTEN)
    target_sources(huxerui_tests PRIVATE
            platform/web_external_texture.cpp
    )
endif ()

target_include_directories(huxerui_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
        "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux"
        "${HUXERUI_PROJECT_DIR}/platform/macos"
        "${HUXERUI_PROJECT_DIR}/platform/windows"
        "${HUXERUI_PROJECT_DIR}/platform/web"
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
)

target_compile_definitions(huxerui_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)

target_link_libraries(huxerui_tests PRIVATE
        HuxerUI3p::Catch2
        ${HUXERUI_TEST_LIBRARY}
)
target_compile_options(huxerui_tests PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
)

add_test(NAME HuxerUITests COMMAND huxerui_tests)

set(HUXERUI_PUBLIC_HEADERS
        animation.h
        app.h
        clipboard.h
        color.h
        environment.h
        event.h
        external_texture.h
        file.h
        geometry.h
        http.h
        huxerui.h
        indication.h
        layer.h
        layout.h
        lifecycle.h
        modifier.h
        navigation.h
        paint.h
        platform_module.h
        platform_view.h
        presentation.h
        render_scene.h
        resource.h
        root.h
        scroll.h
        semantics.h
        state.h
        task.h
        text.h
        text_input.h
        theme.h
        validation.h
        vector.h
        view.h
        virtual_layout.h
        window.h
        android/external_texture.h
        android/jni.h
        android/platform_module.h
        android/platform_view.h
        ios/platform_view.h
        macos/platform_view.h
)

if (WIN32)
    list(APPEND HUXERUI_PUBLIC_HEADERS
            windows/external_texture.h
            windows/platform_view.h
    )
endif ()

if (APPLE)
    list(APPEND HUXERUI_PUBLIC_HEADERS
            ios/external_texture.h
            macos/external_texture.h
    )
endif ()

if (EMSCRIPTEN)
    list(APPEND HUXERUI_PUBLIC_HEADERS
            web/external_texture.h
            web/navigation.h
            web/platform_view.h
    )
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND HUXERUI_PUBLIC_HEADERS
            linux/external_texture.h
    )
endif ()

set(HUXERUI_HEADER_TEST_SOURCES)
foreach (HUXERUI_PUBLIC_HEADER IN LISTS HUXERUI_PUBLIC_HEADERS)
    string(REPLACE "." "_" HUXERUI_HEADER_TEST_NAME
            "${HUXERUI_PUBLIC_HEADER}"
    )
    string(REPLACE "/" "_" HUXERUI_HEADER_TEST_NAME
            "${HUXERUI_HEADER_TEST_NAME}"
    )
    set(HUXERUI_HEADER_TEST_SOURCE
            "${CMAKE_CURRENT_BINARY_DIR}/header_compile/${HUXERUI_HEADER_TEST_NAME}.cpp"
    )
    file(GENERATE
            OUTPUT "${HUXERUI_HEADER_TEST_SOURCE}"
            CONTENT "#include <huxerui/${HUXERUI_PUBLIC_HEADER}>\n"
    )
    list(APPEND HUXERUI_HEADER_TEST_SOURCES
            "${HUXERUI_HEADER_TEST_SOURCE}"
    )
endforeach ()

add_library(huxerui_header_checks OBJECT
        ${HUXERUI_HEADER_TEST_SOURCES}
)

target_link_libraries(
        huxerui_header_checks
        PRIVATE ${HUXERUI_TEST_LIBRARY}
)

add_executable(huxerui_codegen_tests
        tests_main.cpp
        codegen/transform.cpp
        "${HUXERUI_PROJECT_DIR}/tools/codegen/transform.cpp"
)

target_compile_features(huxerui_codegen_tests PRIVATE cxx_std_20)
target_include_directories(huxerui_codegen_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/tools/codegen"
)
target_compile_options(huxerui_codegen_tests PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)

target_link_libraries(huxerui_codegen_tests PRIVATE HuxerUI3p::Catch2)

add_test(NAME HuxerUICodegenTests COMMAND huxerui_codegen_tests)

add_executable(huxerui_resource_compiler_tests
        tests_main.cpp
        resource_compiler/compiler.cpp
        "${HUXERUI_PROJECT_DIR}/tools/resource_compiler/compiler.cpp"
        "${HUXERUI_PROJECT_DIR}/tools/resource_compiler/svg_compiler.cpp"
)

target_compile_features(huxerui_resource_compiler_tests PRIVATE cxx_std_20)
target_include_directories(huxerui_resource_compiler_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
        "${HUXERUI_PROJECT_DIR}/tools/resource_compiler"
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
)
target_compile_options(huxerui_resource_compiler_tests PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)

target_link_libraries(huxerui_resource_compiler_tests PRIVATE
        HuxerUI3p::Catch2
        ${HUXERUI_TEST_LIBRARY}
)

add_test(NAME HuxerUIResourceCompilerTests COMMAND huxerui_resource_compiler_tests)

add_test(NAME HuxerUIResourceMergeTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/resource_merge.cmake"
)

add_test(NAME HuxerUIModuleValidationTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/module_validation.cmake"
)

if (TARGET huxerui_cli_core)
    add_executable(huxerui_cli_tests
            tests_main.cpp
            cli/cli.cpp
    )
    target_compile_features(huxerui_cli_tests PRIVATE cxx_std_20)
    target_compile_options(huxerui_cli_tests PRIVATE
            "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
            "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
            "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
    )
    target_link_libraries(huxerui_cli_tests PRIVATE
            HuxerUI3p::Catch2
            huxerui_cli_core
    )
    add_test(NAME HuxerUICliTests COMMAND huxerui_cli_tests)
endif ()

if (TARGET huxerui_cli AND (WIN32 OR APPLE OR (UNIX AND NOT APPLE AND NOT EMSCRIPTEN)))
    if (WIN32)
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM windows)
        set(HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY "$ENV{TEMP}")
    elseif (APPLE)
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM macos)
        set(HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    else ()
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM linux)
        set(HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif ()
    add_test(NAME HuxerUIInstalledConsumerTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
                    "-DBUILD_CONFIG=$<CONFIG>"
                    "-DWORK_DIRECTORY=${HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY}"
                    "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
                    "-DCLI_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
                    "-DPLATFORM_ID=${HUXERUI_INSTALLED_CONSUMER_PLATFORM}"
                    "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/installed_consumer.cmake"
    )
endif ()

if (WIN32)
    find_program(HUXERUI_POWERSHELL_COMMAND NAMES pwsh powershell)
    if (HUXERUI_POWERSHELL_COMMAND)
        add_test(NAME HuxerUISdkPackageScriptTests
                COMMAND "${HUXERUI_POWERSHELL_COMMAND}"
                        -NoProfile
                        -ExecutionPolicy Bypass
                        -File "${HUXERUI_PROJECT_DIR}/scripts/package_sdk.ps1"
                        -Help
        )
    endif ()
elseif (UNIX AND NOT EMSCRIPTEN)
    add_test(NAME HuxerUISdkPackageScriptTests
            COMMAND sh "${HUXERUI_PROJECT_DIR}/scripts/package_sdk.sh" --help
    )
endif ()

if (TARGET huxerui_cli AND HUXERUI_SDK_PACKAGE_FILE_NAME)
    if (WIN32)
        set(HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY "$ENV{TEMP}")
    elseif (DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
        set(HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY "$ENV{TMPDIR}")
    else ()
        set(HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY "/tmp")
    endif ()
    add_test(NAME HuxerUISdkArchiveTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
                    "-DBUILD_CONFIG=$<CONFIG>"
                    "-DWORK_DIRECTORY=${HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY}"
                    "-DCPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
                    "-DPACKAGE_FILE_NAME=${HUXERUI_SDK_PACKAGE_FILE_NAME}"
                    "-DPACKAGE_GENERATOR=${HUXERUI_SDK_PACKAGE_GENERATOR}"
                    "-DPACKAGE_EXTENSION=${HUXERUI_SDK_PACKAGE_EXTENSION}"
                    "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
                    "-DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
                    "-DCLI_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
                    "-DHOST_SYSTEM=${HUXERUI_SDK_HOST_SYSTEM}"
                    "-DHOST_ARCHITECTURE=${HUXERUI_SDK_HOST_ARCHITECTURE}"
                    "-DPLATFORM_ARTIFACTS_INCLUDED=$<BOOL:${HUXERUI_SDK_PLATFORM_ARTIFACT_ROOT}>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk_archive.cmake"
    )
    set_tests_properties(HuxerUISdkArchiveTests PROPERTIES RUN_SERIAL TRUE)

    add_test(NAME HuxerUISdkReleaseTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                    "-DWORK_DIRECTORY=${HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY}"
                    "-DVALIDATOR_PATH=${HUXERUI_PROJECT_DIR}/cmake/ValidateSdkRelease.cmake"
                    "-DPROJECT_VERSION=${PROJECT_VERSION}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk_release.cmake"
    )

    if (APPLE OR (UNIX AND NOT APPLE AND NOT EMSCRIPTEN))
        add_test(NAME HuxerUISdkInstallerTests
                COMMAND "${CMAKE_COMMAND}"
                        "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
                        "-DBUILD_CONFIG=$<CONFIG>"
                        "-DWORK_DIRECTORY=${HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY}"
                        "-DCPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
                        "-DPACKAGE_FILE_NAME=${HUXERUI_SDK_PACKAGE_FILE_NAME}"
                        "-DPACKAGE_GENERATOR=${HUXERUI_SDK_PACKAGE_GENERATOR}"
                        "-DPACKAGE_EXTENSION=${HUXERUI_SDK_PACKAGE_EXTENSION}"
                        "-DINSTALLER_PATH=${HUXERUI_PROJECT_DIR}/install.sh"
                        "-DHOST_SYSTEM=${HUXERUI_SDK_HOST_SYSTEM}"
                        "-DHOST_ARCHITECTURE=${HUXERUI_SDK_HOST_ARCHITECTURE}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk_installer.cmake"
        )
        set_tests_properties(HuxerUISdkInstallerTests PROPERTIES RUN_SERIAL TRUE)
    endif ()
endif ()

add_executable(huxerui_module_tests
        tests_main.cpp
        runtime/modules.cpp
)

target_include_directories(huxerui_module_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
)
target_link_libraries(huxerui_module_tests PRIVATE
        HuxerUI3p::Catch2
        ${HUXERUI_TEST_LIBRARY}
)
huxerui_use_module(huxerui_module_tests
        TARGET HuxerUI::test_module
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/modules/root_service"
)
huxerui_add_resources(huxerui_module_tests
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/modules/application_resources"
        NAMESPACE huxerui_test_module
)
target_compile_definitions(huxerui_module_tests PRIVATE
        "HUXERUI_MODULE_TEST_RESOURCE_PACKAGE=\"${CMAKE_CURRENT_BINARY_DIR}/huxerui-resources/huxerui_module_tests/package\""
)
add_test(NAME HuxerUIModuleTests COMMAND huxerui_module_tests)

add_executable(huxerui_codegen_runtime_tests
        tests_main.cpp
        codegen/runtime.cpp
)

target_include_directories(huxerui_codegen_runtime_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
)

target_link_libraries(
        huxerui_codegen_runtime_tests
        PRIVATE
        HuxerUI3p::Catch2
        ${HUXERUI_TEST_LIBRARY}
)

huxerui_enable_codegen(huxerui_codegen_runtime_tests)

add_test(
        NAME HuxerUICodegenRuntimeTests
        COMMAND huxerui_codegen_runtime_tests
)
