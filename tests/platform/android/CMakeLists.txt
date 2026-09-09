huxerui_add_test_suite(huxerui_android_logic_tests HuxerUIAndroidLogicTests
        LABELS platform android portable
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES accessibility.cpp text_input.cpp
                "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/cpp/android_accessibility.cpp"
)
target_include_directories(huxerui_android_logic_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/cpp"
)
