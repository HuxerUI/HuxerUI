huxerui_add_example(
        example_platform_module
        "HuxerUI Platform Module"
        "com.huxerui.platform-module"
)

target_sources(example_platform_module PRIVATE timer.cpp)

if (WIN32)
    target_sources(example_platform_module PRIVATE
            color_stream.cpp
            windows_color_stream.cpp
            windows_timer.cpp
    )
elseif (ANDROID)
    target_sources(example_platform_module PRIVATE
            android_color_stream.cpp
            android_timer.cpp
            color_stream.cpp
    )
elseif (APPLE)
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
elseif (EMSCRIPTEN)
    target_sources(example_platform_module PRIVATE
            color_stream.cpp
            web_color_stream.cpp
            web_timer.cpp
    )
    target_link_options(example_platform_module PRIVATE
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/web_timer.js\""
    )
    set_property(TARGET example_platform_module APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web_timer.js"
    )
elseif (UNIX AND NOT APPLE)
    target_sources(example_platform_module PRIVATE
            color_stream.cpp
            linux_color_stream.cpp
            linux_timer.cpp
    )
else ()
    message(FATAL_ERROR "HuxerUI platform module example is not implemented for this platform")
endif ()
