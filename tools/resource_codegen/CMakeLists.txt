cmake_minimum_required(VERSION 3.20)
project(huxerui_resource_codegen LANGUAGES CXX)

add_executable(huxerui_resource_codegen
        main.cpp
        generator.cpp
        svg_compiler.cpp
)

target_compile_features(huxerui_resource_codegen PRIVATE cxx_std_20)
target_compile_options(huxerui_resource_codegen PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)

set_target_properties(huxerui_resource_codegen PROPERTIES
        CXX_EXTENSIONS OFF
        OUTPUT_NAME "hapt"
)
