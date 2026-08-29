huxerui_add_example(
        example_platform_view
        "HuxerUI Platform View"
        "com.huxerui.platform-view"
)

target_sources(example_platform_view PRIVATE platform_text_field.cpp)

if (WIN32)
    target_sources(example_platform_view PRIVATE platform_text_field_windows.cpp)
elseif (ANDROID)
    target_sources(example_platform_view PRIVATE platform_text_field_android.cpp)
elseif (IOS)
    target_sources(example_platform_view PRIVATE platform_text_field_ios.mm)
    target_compile_options(example_platform_view PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(example_platform_view PRIVATE "-framework UIKit")
elseif (APPLE)
    target_sources(example_platform_view PRIVATE platform_text_field_macos.mm)
    target_compile_options(example_platform_view PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(example_platform_view PRIVATE "-framework AppKit")
elseif (EMSCRIPTEN)
    target_sources(example_platform_view PRIVATE platform_text_field_web.cpp)
    target_link_options(example_platform_view PRIVATE
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/platform_text_field_web.js\""
    )
    set_property(TARGET example_platform_view APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform_text_field_web.js"
    )
else ()
    message(FATAL_ERROR "HuxerUI platform view example is not implemented for this platform")
endif ()
