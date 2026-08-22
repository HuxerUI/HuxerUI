cmake_minimum_required(VERSION 3.20)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum iOS deployment target")
endif ()

project(huxerui VERSION 0.1.0 LANGUAGES NONE)

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_language(CXX)
    if (APPLE)
        enable_language(OBJCXX)
    endif ()
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

if (PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND NOT ANDROID)
    set(HUXERUI_DEFAULT_BUILD_EXAMPLES ON)
    if (EMSCRIPTEN OR IOS)
        set(HUXERUI_DEFAULT_BUILD_TESTS OFF)
        set(HUXERUI_DEFAULT_BUILD_CLI OFF)
    else ()
        set(HUXERUI_DEFAULT_BUILD_TESTS ON)
        set(HUXERUI_DEFAULT_BUILD_CLI ON)
    endif ()
else ()
    set(HUXERUI_DEFAULT_BUILD_TESTS OFF)
    set(HUXERUI_DEFAULT_BUILD_EXAMPLES OFF)
    set(HUXERUI_DEFAULT_BUILD_CLI OFF)
endif ()

if (EMSCRIPTEN OR IOS)
    set(HUXERUI_DEFAULT_BUILD_SHARED OFF)
else ()
    set(HUXERUI_DEFAULT_BUILD_SHARED ON)
endif ()

option(HUXERUI_BUILD_SHARED "Build the HuxerUI shared library" ${HUXERUI_DEFAULT_BUILD_SHARED})
option(HUXERUI_BUILD_STATIC "Build the HuxerUI static library" ON)
option(HUXERUI_BUILD_TESTS "Build HuxerUI tests" ${HUXERUI_DEFAULT_BUILD_TESTS})
option(HUXERUI_BUILD_EXAMPLES "Build HuxerUI examples" ${HUXERUI_DEFAULT_BUILD_EXAMPLES})
option(HUXERUI_BUILD_CLI "Build the HuxerUI command-line tool" ${HUXERUI_DEFAULT_BUILD_CLI})
if (WIN32)
    option(HUXERUI_WINDOWS_7_COMPAT "Build the Windows backend for Windows 7 SP1 with Platform Update" OFF)
endif ()

set(HUXERUI_PROJECT_DIR "${PROJECT_SOURCE_DIR}")
set(HUXERUI_PUBLIC_INCLUDE_DIR "${HUXERUI_PROJECT_DIR}/include")
set(HUXERUI_PUBLIC_HEADER_DIR "${HUXERUI_PUBLIC_INCLUDE_DIR}/huxerui")
set(HUXERUI_SHARED_LIB_NAME "${PROJECT_NAME}")
set(HUXERUI_STATIC_LIB_NAME "${PROJECT_NAME}_static")

include(cmake/HuxerUIBuild.cmake)
huxerui_configure_targets()
include(cmake/HuxerUIApp.cmake)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

if (HUXERUI_BUILD_CLI)
    add_subdirectory(tools/huxerui_cli)
endif ()

if (PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR
        AND NOT ANDROID AND NOT IOS AND NOT EMSCRIPTEN)
    include(cmake/HuxerUISdk.cmake)
endif ()

if (HUXERUI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (HUXERUI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()
