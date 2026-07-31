cmake_minimum_required(VERSION 3.20)
project(huxerui_codegen LANGUAGES CXX)

add_executable(huxerui_codegen
        main.cpp
        transform.cpp
)

target_compile_features(huxerui_codegen PRIVATE cxx_std_20)
target_include_directories(huxerui_codegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(huxerui_codegen PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)

set_target_properties(huxerui_codegen PROPERTIES
        OUTPUT_NAME "huxerui-codegen"
)
