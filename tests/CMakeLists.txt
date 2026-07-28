if (TARGET HuxerUI::huxerui_static)
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui_static)
else ()
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui)
endif ()

add_executable(huxerui_tests
        runtime_tests.cpp
)

target_include_directories(huxerui_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
)

target_link_libraries(huxerui_tests PRIVATE ${HUXERUI_TEST_LIBRARY})

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

add_executable(huxerui_header_tests
        header_compile_tests.cpp
        ${HUXERUI_HEADER_TEST_SOURCES}
)

target_link_libraries(
        huxerui_header_tests
        PRIVATE ${HUXERUI_TEST_LIBRARY}
)

add_test(NAME HuxerUIHeaderTests COMMAND huxerui_header_tests)

add_executable(huxerui_codegen_tests
        codegen_tests.cpp
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

add_test(NAME HuxerUICodegenTests COMMAND huxerui_codegen_tests)

add_executable(huxerui_codegen_runtime_tests
        codegen_runtime_tests.cpp
)

target_include_directories(huxerui_codegen_runtime_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
)

target_link_libraries(
        huxerui_codegen_runtime_tests
        PRIVATE ${HUXERUI_TEST_LIBRARY}
)

huxerui_enable_codegen(huxerui_codegen_runtime_tests)
huxerui_enable_codegen(huxerui_codegen_runtime_tests)

add_test(
        NAME HuxerUICodegenRuntimeTests
        COMMAND huxerui_codegen_runtime_tests
)
