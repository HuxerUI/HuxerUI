cmake_minimum_required(VERSION 3.20)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(huxerui_cli VERSION 0.1.0 LANGUAGES CXX)
endif ()

include("${CMAKE_CURRENT_LIST_DIR}/../../3dparty/cmrc/CMakeRC.cmake")

file(GLOB_RECURSE HUXERUI_CLI_TEMPLATE_FILES CONFIGURE_DEPENDS
        LIST_DIRECTORIES FALSE
        "${CMAKE_CURRENT_SOURCE_DIR}/templates/*"
)
cmrc_add_resource_library(huxerui_cli_template_resources
        NAMESPACE huxerui_cli_templates
        WHENCE "${CMAKE_CURRENT_SOURCE_DIR}/templates"
        ${HUXERUI_CLI_TEMPLATE_FILES}
)

add_library(huxerui_cli_core STATIC
        cli.cpp
        ios_project.cpp
        platform.cpp
        process_runner.cpp
        project.cpp
        sdk.cpp
        template.cpp
)
target_compile_features(huxerui_cli_core PUBLIC cxx_std_20)
target_compile_definitions(huxerui_cli_core PRIVATE
        HUXERUI_CLI_VERSION="${PROJECT_VERSION}"
)
target_include_directories(huxerui_cli_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(huxerui_cli_core PRIVATE huxerui_cli_template_resources)
target_compile_options(huxerui_cli_core PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)
set_target_properties(huxerui_cli_core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(huxerui_cli main.cpp)
target_link_libraries(huxerui_cli PRIVATE huxerui_cli_core)
target_compile_options(huxerui_cli PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
        "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
)
set_target_properties(huxerui_cli PROPERTIES
        CXX_EXTENSIONS OFF
        OUTPUT_NAME "huxerui"
)

include(GNUInstallDirs)
install(TARGETS huxerui_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
