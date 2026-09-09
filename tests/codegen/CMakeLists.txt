huxerui_add_test_suite(huxerui_codegen_tests HuxerUICodegenTests
        LABELS tools codegen
        SOURCES transform.cpp "${HUXERUI_PROJECT_DIR}/tools/codegen/transform.cpp"
)
target_include_directories(huxerui_codegen_tests PRIVATE "${HUXERUI_PROJECT_DIR}/tools/codegen")

huxerui_add_test_suite(huxerui_codegen_runtime_tests HuxerUICodegenRuntimeTests
        LABELS tools codegen runtime
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES runtime.cpp
)
huxerui_enable_codegen(huxerui_codegen_runtime_tests)
