cmake_minimum_required(VERSION 3.20)
project(huxerui_android_demo LANGUAGES CXX)

if (NOT HUXERUI_ROOT_DIR)
    message(FATAL_ERROR "HUXERUI_ROOT_DIR is required")
endif ()

find_package(huxerui REQUIRED CONFIG)

set(HUXERUI_PROJECT_DIR "${HUXERUI_ROOT_DIR}")
include("${HUXERUI_ROOT_DIR}/cmake/HuxerUITargets.cmake")
huxerui_resolve_host_tool("codegen" HUXERUI_CODEGEN_COMMAND)

set(HUXERUI_THEME_SOURCE
        "${HUXERUI_ROOT_DIR}/examples/theme/main.cpp"
)
set(HUXERUI_GENERATED_THEME_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/theme.cpp"
)

add_custom_command(
        OUTPUT "${HUXERUI_GENERATED_THEME_SOURCE}"
        COMMAND "${HUXERUI_CODEGEN_COMMAND}"
                --input "${HUXERUI_THEME_SOURCE}"
                --output "${HUXERUI_GENERATED_THEME_SOURCE}"
        DEPENDS
                "${HUXERUI_THEME_SOURCE}"
                "${HUXERUI_CODEGEN_COMMAND}"
        COMMENT "Generating HuxerUI Android demo source"
        VERBATIM
)

add_library(huxerui_app SHARED
        "${HUXERUI_GENERATED_THEME_SOURCE}"
)

target_compile_features(huxerui_app PRIVATE cxx_std_20)
target_link_libraries(huxerui_app PRIVATE
        huxerui::huxerui
)
