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
        unit/data.cpp
        unit/external_texture.cpp
        unit/file.cpp
        unit/geometry.cpp
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
        runtime/application.cpp
        runtime/composition.cpp
        runtime/file.cpp
        runtime/file_picker.cpp
        runtime/file_drop.cpp
        runtime/gestures.cpp
        runtime/http.cpp
        runtime/interaction.cpp
        runtime/layers.cpp
        runtime/layout.cpp
        runtime/lifecycle.cpp
        runtime/navigation.cpp
        runtime/navigation_ui.cpp
        runtime/permission.cpp
        runtime/platform_module.cpp
        runtime/platform_view.cpp
        runtime/presentation.cpp
        runtime/presentation_semantics.cpp
        runtime/render_scene.cpp
        runtime/resources.cpp
        runtime/scrolling.cpp
        runtime/combo_box.cpp
        runtime/date_time_picker.cpp
        runtime/select.cpp
        runtime/system_tray.cpp
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
            platform/macos_application.mm
            platform/macos_platform_module.mm
            platform/macos_platform_bridge.mm
            platform/macos_platform_view.mm
            platform/macos_external_texture.mm
            platform/macos_file_picker.mm
            platform/macos_text_input.mm
            platform/macos_window_chrome.cpp
    )
    set_source_files_properties(
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/apple_timer.mm"
            platform/macos_platform_bridge.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )
    target_include_directories(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module"
    )
    if (TARGET ${HUXERUI_STATIC_LIB_NAME})
        add_test(NAME HuxerUIMacStaticPlatformBridgeImport
                COMMAND sh "${HUXERUI_PROJECT_DIR}/tests/platform/check_macos_platform_bridge.sh"
                        "${HUXERUI_PROJECT_DIR}"
                        "${HUXERUI_PUBLIC_INCLUDE_DIR}"
                        "$<TARGET_FILE:${HUXERUI_STATIC_LIB_NAME}>"
                        "${CMAKE_CURRENT_BINARY_DIR}/macos_platform_bridge_static"
        )
    endif ()
    if (TARGET ${HUXERUI_SHARED_LIB_NAME})
        add_test(NAME HuxerUIMacSharedPlatformBridgeImport
                COMMAND sh "${HUXERUI_PROJECT_DIR}/tests/platform/check_macos_platform_bridge.sh"
                        "${HUXERUI_PROJECT_DIR}"
                        "${HUXERUI_PUBLIC_INCLUDE_DIR}"
                        "$<TARGET_FILE:${HUXERUI_SHARED_LIB_NAME}>"
                        "${CMAKE_CURRENT_BINARY_DIR}/macos_platform_bridge_shared"
        )
    endif ()
endif ()

if (WIN32)
    target_sources(huxerui_tests PRIVATE
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/color_stream.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/windows_color_stream.cpp"
            "${HUXERUI_PROJECT_DIR}/examples/platform_module/windows_timer.cpp"
            platform/windows_accessibility.cpp
            platform/windows_application.cpp
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
            "$<TARGET_PROPERTY:huxerui_core_objects,INCLUDE_DIRECTORIES>"
    )
    target_link_libraries(huxerui_tests PRIVATE
            PkgConfig::HUXERUI_GTK4
            PkgConfig::HUXERUI_GIO
    )
endif ()

if (EMSCRIPTEN)
    target_sources(huxerui_tests PRIVATE
            platform/web_external_texture.cpp
    )
    if (CMAKE_CROSSCOMPILING_EMULATOR)
        set(HUXERUI_WEB_TEST_COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR})
    elseif (NODE_JS_EXECUTABLE)
        set(HUXERUI_WEB_TEST_COMMAND "${NODE_JS_EXECUTABLE}")
    else ()
        message(FATAL_ERROR "HuxerUI Web tests require a JavaScript runtime")
    endif ()
    add_test(NAME HuxerUIWebPlatformRegistryTests
            COMMAND ${HUXERUI_WEB_TEST_COMMAND}
                    "${CMAKE_CURRENT_SOURCE_DIR}/platform/web_platform_registry.js"
                    "${HUXERUI_PROJECT_DIR}/platform/web/web_platform_registry.js"
    )
    add_test(NAME HuxerUIWebFilePickerTests
            COMMAND ${HUXERUI_WEB_TEST_COMMAND}
                    "${CMAKE_CURRENT_SOURCE_DIR}/platform/web_file_picker.js"
                    "${HUXERUI_PROJECT_DIR}/platform/web/web_file_picker.cpp"
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
        data.h
        environment.h
        event.h
        external_texture.h
        file.h
        file_drop.h
        geometry.h
        gesture.h
        http.h
        huxerui.h
        indication.h
        layer.h
        layout.h
        lifecycle.h
        modifier.h
        navigation.h
        paint.h
        platform_adapter.h
        platform_registry.h
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
        android/platform_registry.h
        ios/platform_registry.h
        macos/platform_registry.h
)

if (WIN32)
    list(APPEND HUXERUI_PUBLIC_HEADERS
            windows/external_texture.h
            windows/installer.h
            windows/platform_registry.h
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
            web/platform_registry.h
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
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-missing-field-initializers>"
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
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-missing-field-initializers>"
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

add_test(NAME HuxerUILibraryValidationTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/library_validation.cmake"
)

if (WIN32)
    set(HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY "$ENV{TEMP}")
else ()
    set(HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif ()
add_test(NAME HuxerUISourceSubdirectoryTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                "-DHOST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                "-DHOST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                "-DHOST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/source_subdirectory.cmake"
)
if (MSVC AND CMAKE_GENERATOR MATCHES "^Ninja")
    set(HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT)
    foreach (HUXERUI_MSVC_ENVIRONMENT_VARIABLE IN ITEMS PATH INCLUDE LIB LIBPATH)
        string(REPLACE ";" "\\;" HUXERUI_MSVC_ENVIRONMENT_VALUE
                "$ENV{${HUXERUI_MSVC_ENVIRONMENT_VARIABLE}}"
        )
        list(APPEND HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT
                "${HUXERUI_MSVC_ENVIRONMENT_VARIABLE}=${HUXERUI_MSVC_ENVIRONMENT_VALUE}"
        )
    endforeach ()
    set_tests_properties(HuxerUISourceSubdirectoryTests PROPERTIES
            ENVIRONMENT "${HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT}"
    )
endif ()

if (TARGET huxerui_cli_core)
    add_executable(huxerui_cli_tests
            tests_main.cpp
            cli/cli.cpp
    )
    target_compile_features(huxerui_cli_tests PRIVATE cxx_std_20)
    file(TO_CMAKE_PATH "${HUXERUI_PROJECT_DIR}" HUXERUI_TEST_SOURCE_DIRECTORY)
    target_compile_definitions(huxerui_cli_tests PRIVATE
            HUXERUI_TEST_SOURCE_DIRECTORY="${HUXERUI_TEST_SOURCE_DIRECTORY}"
    )
    target_compile_options(huxerui_cli_tests PRIVATE
            "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
            "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
            "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
            "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-missing-field-initializers>"
    )
    target_link_libraries(huxerui_cli_tests PRIVATE
            HuxerUI3p::Catch2
            huxerui_cli_core
    )
    add_test(NAME HuxerUICliTests COMMAND huxerui_cli_tests)

    if (WIN32)
        set(HUXERUI_GENERATED_LIBRARY_PLATFORM windows)
    elseif (APPLE)
        set(HUXERUI_GENERATED_LIBRARY_PLATFORM macos)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(HUXERUI_GENERATED_LIBRARY_PLATFORM linux)
    else ()
        unset(HUXERUI_GENERATED_LIBRARY_PLATFORM)
    endif ()
    if (TARGET huxerui_cli AND NOT CMAKE_CROSSCOMPILING AND HUXERUI_GENERATED_LIBRARY_PLATFORM)
        add_test(NAME HuxerUIGeneratedLibraryTests
                COMMAND "${CMAKE_COMMAND}"
                        "-DCLI_EXECUTABLE=$<TARGET_FILE:huxerui_cli>"
                        "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                        "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                        "-DBUILD_CONFIG=$<CONFIG>"
                        "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                        "-DHOST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                        "-DHOST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                        "-DHOST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                        "-DPLATFORM_ID=${HUXERUI_GENERATED_LIBRARY_PLATFORM}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated_library.cmake"
        )
        if (MSVC AND CMAKE_GENERATOR MATCHES "^Ninja")
            set_tests_properties(HuxerUIGeneratedLibraryTests PROPERTIES
                    ENVIRONMENT "${HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT}"
            )
        endif ()
    endif ()
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
        add_test(NAME HuxerUIToolBuildScriptTests
                COMMAND "${HUXERUI_POWERSHELL_COMMAND}"
                        -NoProfile
                        -ExecutionPolicy Bypass
                        -File "${HUXERUI_PROJECT_DIR}/scripts/build_tools.ps1"
                        -h
        )
        add_test(NAME HuxerUISdkPackageScriptTests
                COMMAND "${HUXERUI_POWERSHELL_COMMAND}"
                        -NoProfile
                        -ExecutionPolicy Bypass
                        -File "${HUXERUI_PROJECT_DIR}/scripts/package_sdk.ps1"
                        -Help
        )
    endif ()
elseif (UNIX AND NOT EMSCRIPTEN)
    add_test(NAME HuxerUIToolBuildScriptTests
            COMMAND sh "${HUXERUI_PROJECT_DIR}/scripts/build_tools.sh" -h
    )
    add_test(NAME HuxerUIToolBuildGeneratorTests
            COMMAND sh
                    "${HUXERUI_PROJECT_DIR}/tests/scripts/build_tools_generator.sh"
                    "${HUXERUI_PROJECT_DIR}"
    )
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
                    "-DHOST_PLATFORM=${HUXERUI_SDK_HOST_PLATFORM}"
                    "-DHOST_ARCHITECTURE=${HUXERUI_SDK_HOST_ARCHITECTURE}"
                    "-DPLATFORM_ARTIFACTS_INCLUDED=$<BOOL:${HUXERUI_INTERNAL_SDK_ARTIFACT_ROOT}>"
                    "-DWINDOWS_DEBUG_INCLUDED=$<BOOL:${HUXERUI_INTERNAL_PACKAGE_WINDOWS_DEBUG}>"
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
                        "-DHOST_PLATFORM=${HUXERUI_SDK_HOST_PLATFORM}"
                        "-DHOST_ARCHITECTURE=${HUXERUI_SDK_HOST_ARCHITECTURE}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk_installer.cmake"
        )
        set_tests_properties(HuxerUISdkInstallerTests PROPERTIES RUN_SERIAL TRUE)
    endif ()
endif ()

add_executable(huxerui_library_tests
        tests_main.cpp
        runtime/libraries.cpp
)

target_include_directories(huxerui_library_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
)
target_link_libraries(huxerui_library_tests PRIVATE
        HuxerUI3p::Catch2
        ${HUXERUI_TEST_LIBRARY}
)
huxerui_use_library(huxerui_library_tests
        TARGET HuxerUI::test_library
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/libraries/root_service"
)
huxerui_add_resources(huxerui_library_tests
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libraries/application_resources"
        NAMESPACE huxerui_test_library
)
target_compile_definitions(huxerui_library_tests PRIVATE
        "HUXERUI_LIBRARY_TEST_RESOURCE_PACKAGE=\"${CMAKE_CURRENT_BINARY_DIR}/huxerui-resources/huxerui_library_tests/package\""
)
add_test(NAME HuxerUILibraryTests COMMAND huxerui_library_tests)

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
