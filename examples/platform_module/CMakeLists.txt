huxerui_add_example(
        example_platform_module
        "HuxerUI Platform Module"
        "com.huxerui.platform-module"
)

target_sources(example_platform_module PRIVATE appkit_timer.mm)
target_compile_options(example_platform_module PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_link_libraries(example_platform_module PRIVATE "-framework AppKit")
