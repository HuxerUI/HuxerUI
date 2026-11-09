huxerui_add_test_suite(huxerui_web_tests HuxerUIWebTests
        LABELS platform web native
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_sources(huxerui_web_tests PRIVATE
        external_texture.cpp
        test_entry.cpp
        text_layout.cpp
)
target_include_directories(huxerui_web_tests PRIVATE "${HUXERUI_PROJECT_DIR}/platform/web")
target_compile_definitions(huxerui_web_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)

add_test(NAME HuxerUIWebTestFailureExitTests
        COMMAND "${CMAKE_COMMAND}"
                "-DTEST_EXECUTABLE=$<TARGET_FILE:huxerui_web_tests>"
                "-DTEST_EMULATOR=$<TARGET_PROPERTY:huxerui_web_tests,CROSSCOMPILING_EMULATOR>"
                -P "${CMAKE_CURRENT_LIST_DIR}/test_entry.cmake"
)
set_tests_properties(HuxerUIWebTestFailureExitTests PROPERTIES
        LABELS "build;platform;web"
)

find_program(HUXERUI_WEB_TEST_BROWSER
        NAMES chromium chromium-browser google-chrome google-chrome-stable chrome "Google Chrome"
        PATHS "/Applications/Google Chrome.app/Contents/MacOS"
)
if (HUXERUI_WEB_TEST_BROWSER AND HUXERUI_WEB_TEST_COMMAND)
    huxerui_add_app(huxerui_web_file_storage_test SOURCES file_storage.cpp)
    set_target_properties(huxerui_web_file_storage_test PROPERTIES SUFFIX ".js")
    add_test(NAME HuxerUIWebFileStorageTests
            COMMAND ${HUXERUI_WEB_TEST_COMMAND}
                    "${CMAKE_CURRENT_LIST_DIR}/file_storage_test.mjs"
                    "${HUXERUI_WEB_TEST_BROWSER}"
                    "$<TARGET_FILE:huxerui_web_file_storage_test>"
    )
    set_tests_properties(HuxerUIWebFileStorageTests PROPERTIES LABELS "platform;web;native" TIMEOUT 45)
endif ()
