add_executable(huxerui_codegen EXCLUDE_FROM_ALL
        main.cpp
        transform.cpp
)

target_compile_features(huxerui_codegen PRIVATE cxx_std_20)
target_include_directories(huxerui_codegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(huxerui_codegen PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

set_target_properties(huxerui_codegen PROPERTIES
        OUTPUT_NAME "huxerui-codegen"
)
