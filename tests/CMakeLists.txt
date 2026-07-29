if (TARGET HuxerUI::huxerui_static)
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui_static)
else ()
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui)
endif ()

include("${HUXERUI_PROJECT_DIR}/cmake/HuxerUIThirdParty.cmake")

add_executable(huxerui_tests
        tests_main.cpp
        runtime/composition.cpp
        runtime/interaction.cpp
        runtime/layout.cpp
        runtime/presentation.cpp
        runtime/scrolling.cpp
        runtime/virtual_layout.cpp
)

target_include_directories(huxerui_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
)

target_link_libraries(huxerui_tests PRIVATE
        HuxerUI3p::Catch2
        ${HUXERUI_TEST_LIBRARY}
)

add_test(NAME HuxerUIRuntimeTests COMMAND huxerui_tests)

set(HUXERUI_PUBLIC_HEADERS
        animation.h
        app.h
        color.h
        display_list.h
        environment.h
        event.h
        foreach.h
        geometry.h
        huxerui.h
        interaction.h
        layer.h
        layout.h
        modifier.h
        presentation.h
        root.h
        scope.h
        scroll_state.h
        state.h
        theme.h
        view.h
        virtual_layout.h
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
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)

target_link_libraries(huxerui_codegen_tests PRIVATE HuxerUI3p::Catch2)

add_test(NAME HuxerUICodegenTests COMMAND huxerui_codegen_tests)

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
