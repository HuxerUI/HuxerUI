include_guard(GLOBAL)

if (ANDROID)
    set(_HUXERUI_TESTING_LINK_TYPE SHARED)
else ()
    set(_HUXERUI_TESTING_LINK_TYPE STATIC)
endif ()
add_library(huxerui_testing ${_HUXERUI_TESTING_LINK_TYPE}
        "${HUXERUI_PROJECT_DIR}/testing/ui_test.cpp"
        "${HUXERUI_PROJECT_DIR}/testing/testing_platform_adapter.cpp"
        "${HUXERUI_PROJECT_DIR}/testing/ui_snapshot.cpp"
)
unset(_HUXERUI_TESTING_LINK_TYPE)
add_library(HuxerUI::testing ALIAS huxerui_testing)
set_target_properties(huxerui_testing PROPERTIES EXPORT_NAME testing POSITION_INDEPENDENT_CODE ON)
if (WIN32)
    set_target_properties(huxerui_testing PROPERTIES DEBUG_POSTFIX "_debug")
endif ()
target_compile_features(huxerui_testing PUBLIC cxx_std_20)
target_include_directories(huxerui_testing PRIVATE "${HUXERUI_PROJECT_DIR}/src")
target_compile_options(huxerui_testing PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>"
)
if (ANDROID OR NOT TARGET HuxerUI::huxerui_static)
    target_link_libraries(huxerui_testing PUBLIC "$<BUILD_INTERFACE:HuxerUI::huxerui>")
else ()
    target_link_libraries(huxerui_testing PUBLIC "$<BUILD_INTERFACE:HuxerUI::huxerui_static>")
endif ()
