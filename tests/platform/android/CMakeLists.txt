huxerui_add_test_suite(huxerui_android_logic_tests HuxerUIAndroidLogicTests
        LABELS platform android portable
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES accessibility.cpp text_input.cpp
                "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/cpp/android_accessibility.cpp"
)
target_include_directories(huxerui_android_logic_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/cpp"
)

if (NOT CMAKE_CROSSCOMPILING)
    find_package(Java 11 QUIET COMPONENTS Development)
    find_package(JNI QUIET)
    if (Java_FOUND AND JNI_FOUND)
        add_library(huxerui_android_http_test_jni SHARED http_jni.cpp)
        target_include_directories(huxerui_android_http_test_jni PRIVATE ${JNI_INCLUDE_DIRS})
        target_compile_options(huxerui_android_http_test_jni PRIVATE
                "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->"
                "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>")
        set(http_classes "${CMAKE_CURRENT_BINARY_DIR}/http_classes")
        set(http_request "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/java/org/huxerui/HuxerUIHttpRequest.java")
        set(http_test "${CMAKE_CURRENT_SOURCE_DIR}/java/HuxerUIHttpRequestTest.java")
        add_custom_command(
                OUTPUT "${http_classes}/org/huxerui/HuxerUIHttpRequestTest.class"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${http_classes}"
                COMMAND "${Java_JAVAC_EXECUTABLE}" --release 8 -d "${http_classes}" "${http_request}" "${http_test}"
                DEPENDS "${http_request}" "${http_test}"
                VERBATIM
        )
        add_custom_target(huxerui_android_http_tests ALL
                DEPENDS "${http_classes}/org/huxerui/HuxerUIHttpRequestTest.class" huxerui_android_http_test_jni)
        add_test(NAME HuxerUIAndroidHttpTests COMMAND "${Java_JAVA_EXECUTABLE}" -cp "${http_classes}"
                org.huxerui.HuxerUIHttpRequestTest "$<TARGET_FILE:huxerui_android_http_test_jni>")
        set_tests_properties(HuxerUIAndroidHttpTests PROPERTIES LABELS "platform;android;http" TIMEOUT 45)
    endif ()
endif ()
