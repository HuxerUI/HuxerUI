if (TARGET HuxerUI::huxerui_static)
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui_static)
else ()
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui)
endif ()

include("${HUXERUI_PROJECT_DIR}/cmake/HuxerUIThirdParty.cmake")

file(TO_CMAKE_PATH "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}" HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE)
add_executable(huxerui_tests
        tests_main.cpp
        support/builtin_resources.cpp
        unit/platform_frame.cpp
        unit/paint.cpp
        unit/path.cpp
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
        runtime/composition.cpp
        runtime/interaction.cpp
        runtime/layers.cpp
        runtime/layout.cpp
        runtime/navigation.cpp
        runtime/navigation_ui.cpp
        runtime/presentation.cpp
        runtime/presentation_semantics.cpp
        runtime/render_scene.cpp
        runtime/resources.cpp
        runtime/scrolling.cpp
        runtime/selection_area.cpp
        runtime/text_field.cpp
        runtime/text_input.cpp
        runtime/tooltip.cpp
        runtime/virtual_layout.cpp
        runtime/window.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(huxerui_tests PRIVATE
            platform/macos_text_input.mm
            platform/macos_window_chrome.cpp
    )
endif ()

if (WIN32)
    target_sources(huxerui_tests PRIVATE
            platform/windows_accessibility.cpp
            platform/windows_damage.cpp
            platform/windows_renderer.cpp
            platform/windows_window_chrome.cpp
    )
    target_compile_definitions(huxerui_tests PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
    )
endif ()

if (UNIX AND NOT APPLE)
    target_sources(huxerui_tests PRIVATE
            platform/linux_damage.cpp
            platform/linux_renderer.cpp
            platform/linux_text_input.cpp
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
        geometry.h
        huxerui.h
        indication.h
        layer.h
        layout.h
        modifier.h
        navigation.h
        paint.h
        presentation.h
        render_scene.h
        resource.h
        root.h
        scroll.h
        semantics.h
        state.h
        text.h
        text_input.h
        theme.h
        validation.h
        vector.h
        view.h
        virtual_layout.h
        window.h
)

set(HUXERUI_HEADER_TEST_SOURCES)
foreach (HUXERUI_PUBLIC_HEADER IN LISTS HUXERUI_PUBLIC_HEADERS)
    string(REPLACE "." "_" HUXERUI_HEADER_TEST_NAME
            "${HUXERUI_PUBLIC_HEADER}"
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

add_executable(huxerui_resource_codegen_tests
        tests_main.cpp
        resource_codegen/generator.cpp
        "${HUXERUI_PROJECT_DIR}/tools/resource_codegen/generator.cpp"
        "${HUXERUI_PROJECT_DIR}/tools/resource_codegen/svg_compiler.cpp"
)

target_compile_features(huxerui_resource_codegen_tests PRIVATE cxx_std_20)
target_include_directories(huxerui_resource_codegen_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
        "${HUXERUI_PROJECT_DIR}/tools/resource_codegen"
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
)
target_compile_options(huxerui_resource_codegen_tests PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)

target_link_libraries(huxerui_resource_codegen_tests PRIVATE
        HuxerUI3p::Catch2
        ${HUXERUI_TEST_LIBRARY}
)

add_test(NAME HuxerUIResourceCodegenTests COMMAND huxerui_resource_codegen_tests)

add_test(NAME HuxerUIResourceMergeTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/resource_merge.cmake"
)

add_test(NAME HuxerUIAndroidPlanTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/android_platform_plan.cmake"
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

if (TARGET huxerui_cli AND (WIN32 OR APPLE))
    if (WIN32)
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM windows)
        set(HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY "$ENV{TEMP}")
    else ()
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM macos)
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
