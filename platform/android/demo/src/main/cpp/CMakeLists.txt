cmake_minimum_required(VERSION 3.20)
project(huxerui_android_demo LANGUAGES CXX)

if (NOT HUXERUI_ROOT_DIR)
    message(FATAL_ERROR "HUXERUI_ROOT_DIR is required")
endif ()
find_package(HuxerUI REQUIRED CONFIG)

set(HUXERUI_PROJECT_DIR "${HUXERUI_ROOT_DIR}")
include("${HUXERUI_ROOT_DIR}/cmake/HuxerUITargets.cmake")
include("${HUXERUI_ROOT_DIR}/cmake/HuxerUIExamples.cmake")

set(HUXERUI_DEMO_EXAMPLE "ui_gallery" CACHE STRING "Example displayed by the Android demo")
if (NOT HUXERUI_DEMO_EXAMPLE MATCHES "^[a-z0-9_]+$")
    message(FATAL_ERROR "HUXERUI_DEMO_EXAMPLE must be a snake-case example directory name")
endif ()

set(HUXERUI_DEMO_EXAMPLE_DIR
        "${HUXERUI_ROOT_DIR}/examples/${HUXERUI_DEMO_EXAMPLE}"
)
if (NOT EXISTS "${HUXERUI_DEMO_EXAMPLE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "HuxerUI Android demo example does not exist: ${HUXERUI_DEMO_EXAMPLE}"
    )
endif ()

add_subdirectory(
        "${HUXERUI_DEMO_EXAMPLE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/example"
)
