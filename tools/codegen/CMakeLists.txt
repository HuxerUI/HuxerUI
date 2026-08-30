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
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-missing-field-initializers>"
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(huxerui_codegen PRIVATE -static-libstdc++ -static-libgcc)
endif ()

set_target_properties(huxerui_codegen PROPERTIES
        CXX_EXTENSIONS OFF
        OUTPUT_NAME "hcg"
)

install(TARGETS huxerui_codegen RUNTIME DESTINATION ".")
