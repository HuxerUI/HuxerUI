if (ANDROID)
    add_library(huxerui_ui_testing_smoke SHARED android.cpp smoke.cpp)
elseif (IOS)
    find_package(XCTest REQUIRED)
    xctest_add_bundle(huxerui_ui_testing_smoke huxerui_static ios.mm smoke.cpp)
    set_target_properties(huxerui_ui_testing_smoke PROPERTIES
            XCODE_GENERATE_SCHEME TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER org.huxerui.testing.smoke
    )
elseif (EMSCRIPTEN)
    add_executable(huxerui_ui_testing_smoke web.cpp smoke.cpp)
else ()
    add_executable(huxerui_ui_testing_smoke desktop.cpp smoke.cpp)
    enable_testing()
    add_test(NAME HuxerUIUiTestingSmoke COMMAND huxerui_ui_testing_smoke)
    set_tests_properties(HuxerUIUiTestingSmoke PROPERTIES TIMEOUT 30 LABELS "testing;smoke")
endif ()
target_link_libraries(huxerui_ui_testing_smoke PRIVATE HuxerUI::testing)
target_compile_options(huxerui_ui_testing_smoke PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>"
)
if (ANDROID AND HUXERUI_TESTING_RESOURCE_OUTPUT)
    set_target_properties(huxerui_ui_testing_smoke PROPERTIES
            HUXERUI_RESOURCE_OUTPUT_DIRECTORY "${HUXERUI_TESTING_RESOURCE_OUTPUT}/${ANDROID_ABI}")
endif ()
huxerui_add_resources(huxerui_ui_testing_smoke
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/application_resources"
        NAMESPACE ui_testing_smoke
)
_huxerui_append_resource_package_input(huxerui_ui_testing_smoke "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}")
get_target_property(_HUXERUI_SMOKE_PACKAGE huxerui_ui_testing_smoke HUXERUI_RESOURCE_PACKAGE)
target_compile_definitions(huxerui_ui_testing_smoke PRIVATE "HUXERUI_SMOKE_PACKAGE=\"${_HUXERUI_SMOKE_PACKAGE}\"")
if (IOS)
    add_custom_command(TARGET huxerui_ui_testing_smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_HUXERUI_SMOKE_PACKAGE}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:huxerui_ui_testing_smoke>/package"
    )
elseif (EMSCRIPTEN)
    target_link_options(huxerui_ui_testing_smoke PRIVATE "SHELL:--preload-file \"${_HUXERUI_SMOKE_PACKAGE}@/package\"")
    add_custom_command(TARGET huxerui_ui_testing_smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/web.html"
                    "$<TARGET_FILE_DIR:huxerui_ui_testing_smoke>/index.html"
    )
endif ()
unset(_HUXERUI_SMOKE_PACKAGE)
