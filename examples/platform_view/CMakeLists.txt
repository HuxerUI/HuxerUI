huxerui_add_example(
        example_platform_view
        "HuxerUI Platform View"
        "com.huxerui.platform-view"
)

target_sources(example_platform_view PRIVATE native_text_field.cpp)

if (ANDROID)
    target_sources(example_platform_view PRIVATE native_text_field_android.cpp)
elseif (IOS)
    target_sources(example_platform_view PRIVATE native_text_field_ios.mm)
    target_compile_options(example_platform_view PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(example_platform_view PRIVATE "-framework UIKit")
else ()
    target_sources(example_platform_view PRIVATE native_text_field_macos.mm)
    target_compile_options(example_platform_view PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(example_platform_view PRIVATE "-framework AppKit")
endif ()
