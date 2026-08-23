cmake_minimum_required(VERSION 3.20)
project(huxerui_android_example_runner LANGUAGES CXX)

if (NOT HUXERUI_ROOT_DIR)
    message(FATAL_ERROR "HUXERUI_ROOT_DIR is required")
endif ()
file(TO_CMAKE_PATH "${HUXERUI_ROOT_DIR}" HUXERUI_ROOT_DIR)

set(HUXERUI_PROJECT_DIR "${HUXERUI_ROOT_DIR}")
set(HUXERUI_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${HUXERUI_ROOT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/huxerui" EXCLUDE_FROM_ALL)
include("${HUXERUI_ROOT_DIR}/cmake/HuxerUIExamples.cmake")

set(HUXERUI_EXAMPLE "ui_gallery" CACHE STRING "Example displayed by the Android example runner")
if (NOT HUXERUI_EXAMPLE MATCHES "^[a-z0-9_]+$")
    message(FATAL_ERROR "HUXERUI_EXAMPLE must be a snake-case example directory name")
endif ()

set(HUXERUI_EXAMPLE_DIR
        "${HUXERUI_ROOT_DIR}/examples/${HUXERUI_EXAMPLE}"
)
if (NOT EXISTS "${HUXERUI_EXAMPLE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "HuxerUI Android example does not exist: ${HUXERUI_EXAMPLE}"
    )
endif ()

add_subdirectory(
        "${HUXERUI_EXAMPLE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/example"
)
