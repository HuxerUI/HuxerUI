huxerui_add_example(
        example_platform_module
        "HuxerUI Platform Module"
        "com.huxerui.platform-module"
)

target_sources(example_platform_module PRIVATE timer.cpp)

if (ANDROID)
    target_sources(example_platform_module PRIVATE android_timer.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(example_platform_module PRIVATE
            apple_color_stream.mm
            apple_timer.mm
            color_stream.cpp
    )
    target_compile_options(example_platform_module PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(example_platform_module PRIVATE
            "-framework CoreVideo"
            "-framework Foundation"
    )
elseif (APPLE)
    target_sources(example_platform_module PRIVATE apple_timer.mm)
    target_compile_options(example_platform_module PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(example_platform_module PRIVATE "-framework Foundation")
elseif (UNIX AND NOT APPLE)
    target_sources(example_platform_module PRIVATE linux_timer.cpp)
else ()
    message(FATAL_ERROR "HuxerUI platform module example is not implemented for this platform")
endif ()
