add_executable(huxerui_tests
        runtime_tests.cpp
)

target_include_directories(huxerui_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
)

if (TARGET HuxerUI::huxerui_static)
    target_link_libraries(huxerui_tests PRIVATE HuxerUI::huxerui_static)
else ()
    target_link_libraries(huxerui_tests PRIVATE HuxerUI::huxerui)
endif ()

add_test(NAME HuxerUIRuntimeTests COMMAND huxerui_tests)

add_executable(huxerui_codegen_tests
        codegen_tests.cpp
        "${HUXERUI_PROJECT_DIR}/tools/codegen/transform.cpp"
)

target_compile_features(huxerui_codegen_tests PRIVATE cxx_std_20)
target_include_directories(huxerui_codegen_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/tools/codegen"
)
target_compile_options(huxerui_codegen_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(NAME HuxerUICodegenTests COMMAND huxerui_codegen_tests)

add_executable(huxerui_codegen_runtime_tests
        codegen_runtime_tests.cpp
)

target_include_directories(huxerui_codegen_runtime_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src"
)

if (TARGET HuxerUI::huxerui_static)
    target_link_libraries(
            huxerui_codegen_runtime_tests
            PRIVATE HuxerUI::huxerui_static
    )
else ()
    target_link_libraries(
            huxerui_codegen_runtime_tests
            PRIVATE HuxerUI::huxerui
    )
endif ()

huxerui_enable_codegen(huxerui_codegen_runtime_tests)
huxerui_enable_codegen(huxerui_codegen_runtime_tests)

add_test(
        NAME HuxerUICodegenRuntimeTests
        COMMAND huxerui_codegen_runtime_tests
)
