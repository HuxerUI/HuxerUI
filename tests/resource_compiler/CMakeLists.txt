huxerui_add_test_suite(huxerui_resource_compiler_tests HuxerUIResourceCompilerTests
        LABELS tools resources
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES compiler.cpp
                "${HUXERUI_PROJECT_DIR}/tools/resource_compiler/compiler.cpp"
                "${HUXERUI_PROJECT_DIR}/tools/resource_compiler/svg_compiler.cpp"
)
target_include_directories(huxerui_resource_compiler_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/src/resources"
        "${HUXERUI_PROJECT_DIR}/tools/resource_compiler"
)
