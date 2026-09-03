huxerui_add_example(example_external_texture "HuxerUI External Texture" "com.huxerui.external-texture")

if (WIN32 OR (UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN))
    target_sources(example_external_texture PRIVATE pixel_texture_demo.cpp)
elseif (ANDROID)
    target_sources(example_external_texture PRIVATE android_texture_demo.cpp)
    target_link_libraries(example_external_texture PRIVATE log)
elseif (APPLE)
    target_sources(example_external_texture PRIVATE apple_texture_demo.mm)
    target_compile_options(example_external_texture PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(example_external_texture PRIVATE
            "-framework CoreVideo" "-framework Foundation" "-framework Metal"
    )
elseif (EMSCRIPTEN)
    target_sources(example_external_texture PRIVATE web_texture_demo.cpp)
else ()
    message(FATAL_ERROR "HuxerUI external texture example is not implemented for this platform")
endif ()
