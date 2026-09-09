huxerui_add_test_suite(huxerui_web_logic_tests HuxerUIWebLogicTests
        LABELS platform web portable
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES text.cpp
)
target_include_directories(huxerui_web_logic_tests PRIVATE "${HUXERUI_PROJECT_DIR}/platform/web")

if (NODE_JS_EXECUTABLE)
    set(HUXERUI_WEB_TEST_COMMAND "${NODE_JS_EXECUTABLE}")
elseif (EMSCRIPTEN AND CMAKE_CROSSCOMPILING_EMULATOR)
    set(HUXERUI_WEB_TEST_COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR})
else ()
    find_program(HUXERUI_NODE_COMMAND NAMES node nodejs)
    set(HUXERUI_WEB_TEST_COMMAND "${HUXERUI_NODE_COMMAND}")
endif ()
if (HUXERUI_WEB_TEST_COMMAND)
    add_test(NAME HuxerUIWebPlatformRegistryTests
            COMMAND ${HUXERUI_WEB_TEST_COMMAND}
                    "${CMAKE_CURRENT_SOURCE_DIR}/platform_registry.js"
                    "${HUXERUI_PROJECT_DIR}/platform/web/web_platform_registry.js"
    )
    add_test(NAME HuxerUIWebFilePickerTests
            COMMAND ${HUXERUI_WEB_TEST_COMMAND}
                    "${CMAKE_CURRENT_SOURCE_DIR}/file_picker.js"
                    "${HUXERUI_PROJECT_DIR}/platform/web/web_file_picker.cpp"
    )
    set_tests_properties(HuxerUIWebPlatformRegistryTests HuxerUIWebFilePickerTests
            PROPERTIES LABELS "platform;web;portable"
    )
else ()
    message(STATUS "HuxerUI Web JavaScript tests are unavailable: Node.js was not found")
endif ()

if (EMSCRIPTEN)
    include(native.cmake)
endif ()
