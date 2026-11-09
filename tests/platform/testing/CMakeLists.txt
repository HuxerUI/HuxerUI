if (ANDROID)
    add_library(huxerui_ui_testing_smoke SHARED android.cpp smoke.cpp
            ../android/ui_testing/file_reference.cpp ../android/ui_testing/activity_result.cpp)
    target_include_directories(huxerui_ui_testing_smoke PRIVATE
            "${HUXERUI_PROJECT_DIR}/src" "${HUXERUI_PROJECT_DIR}/platform/android/huxerui/src/main/cpp")
elseif (IOS)
    find_package(XCTest REQUIRED)
    add_executable(huxerui_ui_testing_host MACOSX_BUNDLE ../ios/host.m)
    target_link_libraries(huxerui_ui_testing_host PRIVATE "-framework UIKit")
    set_target_properties(huxerui_ui_testing_host PROPERTIES
            XCODE_GENERATE_SCHEME TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER org.huxerui.testing.host
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER org.huxerui.testing.host
            XCODE_ATTRIBUTE_INFOPLIST_KEY_UILaunchScreen_Generation YES
    )
    xctest_add_bundle(huxerui_ui_testing_smoke huxerui_ui_testing_host ios.mm smoke.cpp
            ../ios/file_reference.mm ../ios/presentation.mm)
    set_source_files_properties(../ios/file_reference.mm ../ios/presentation.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # Xcode embeds the test bundle itself; making PlugIns a build directory duplicates its mkdir output.
    set_target_properties(huxerui_ui_testing_smoke PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
            XCODE_GENERATE_SCHEME TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER org.huxerui.testing.smoke
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER org.huxerui.testing.smoke
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
    set(_HUXERUI_SMOKE_BUNDLE "$<TARGET_BUNDLE_CONTENT_DIR:huxerui_ui_testing_smoke>")
    if (XCODE)
        set(_HUXERUI_SMOKE_BUNDLE
                "$<TARGET_BUNDLE_DIR:huxerui_ui_testing_host>/PlugIns/huxerui_ui_testing_smoke.xctest")
    endif ()
    add_custom_command(TARGET huxerui_ui_testing_smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_HUXERUI_SMOKE_PACKAGE}"
                    "${_HUXERUI_SMOKE_BUNDLE}/package"
    )
    unset(_HUXERUI_SMOKE_BUNDLE)
elseif (EMSCRIPTEN)
    target_link_options(huxerui_ui_testing_smoke PRIVATE "SHELL:--preload-file \"${_HUXERUI_SMOKE_PACKAGE}@/package\"")
    add_custom_command(TARGET huxerui_ui_testing_smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/web.html"
                    "$<TARGET_FILE_DIR:huxerui_ui_testing_smoke>/index.html"
    )
endif ()
unset(_HUXERUI_SMOKE_PACKAGE)
