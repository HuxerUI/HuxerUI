foreach (required_variable IN ITEMS CLI_EXECUTABLE SOURCE_DIRECTORY WORK_DIRECTORY HOST_GENERATOR PLATFORM_ID)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/huxerui-generated-library-${TEST_NONCE}")
set(LIBRARY_SOURCE "${TEST_ROOT}/CameraKit")
set(PREVIEW_SOURCE "${LIBRARY_SOURCE}/examples/preview")
set(PREVIEW_BUILD "${TEST_ROOT}/build")
file(MAKE_DIRECTORY "${TEST_ROOT}")

execute_process(
        COMMAND "${CLI_EXECUTABLE}" create library CameraKit
                --namespace scave::camera
                --target Scave::Camera
                --platform "${PLATFORM_ID}"
                --agent none
        WORKING_DIRECTORY "${TEST_ROOT}"
        RESULT_VARIABLE CREATE_RESULT
        OUTPUT_VARIABLE CREATE_OUTPUT
        ERROR_VARIABLE CREATE_ERROR
)
if (NOT CREATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Generated library creation failed:\n${CREATE_OUTPUT}${CREATE_ERROR}")
endif ()

set(CONFIGURE_COMMAND
        "${CMAKE_COMMAND}" -S "${PREVIEW_SOURCE}" -B "${PREVIEW_BUILD}"
        -G "${HOST_GENERATOR}"
        "-DHUXERUI_HOME=${SOURCE_DIRECTORY}"
)
if (HOST_CXX_COMPILER AND NOT HOST_GENERATOR MATCHES "^(Visual Studio|Xcode)")
    list(APPEND CONFIGURE_COMMAND "-DCMAKE_CXX_COMPILER=${HOST_CXX_COMPILER}")
endif ()
if (HOST_GENERATOR_PLATFORM)
    list(APPEND CONFIGURE_COMMAND -A "${HOST_GENERATOR_PLATFORM}")
endif ()
if (HOST_GENERATOR_TOOLSET)
    list(APPEND CONFIGURE_COMMAND -T "${HOST_GENERATOR_TOOLSET}")
endif ()
if (BUILD_CONFIG AND NOT HOST_GENERATOR MATCHES "^(Visual Studio|Xcode|Ninja Multi-Config)")
    list(APPEND CONFIGURE_COMMAND "-DCMAKE_BUILD_TYPE=${BUILD_CONFIG}")
endif ()

execute_process(
        COMMAND ${CONFIGURE_COMMAND}
        RESULT_VARIABLE CONFIGURE_RESULT
        OUTPUT_VARIABLE CONFIGURE_OUTPUT
        ERROR_VARIABLE CONFIGURE_ERROR
)
if (NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Generated library configuration failed:\n${CONFIGURE_OUTPUT}${CONFIGURE_ERROR}")
endif ()

set(LIBRARY_GRAPH "${PREVIEW_SOURCE}/.huxerui/generated/libraries.json")
if (NOT EXISTS "${LIBRARY_GRAPH}")
    message(FATAL_ERROR "Generated library Preview did not emit its library graph")
endif ()
file(READ "${LIBRARY_GRAPH}" LIBRARY_GRAPH_CONTENT)
string(FIND "${LIBRARY_GRAPH_CONTENT}" "\"target\": \"Scave::Camera\"" TARGET_POSITION)
if (TARGET_POSITION EQUAL -1)
    message(FATAL_ERROR "Generated library graph did not retain the requested public target")
endif ()

set(BUILD_COMMAND "${CMAKE_COMMAND}" --build "${PREVIEW_BUILD}" --target example_camera_kit --parallel 4)
if (BUILD_CONFIG)
    list(APPEND BUILD_COMMAND --config "${BUILD_CONFIG}")
endif ()
execute_process(
        COMMAND ${BUILD_COMMAND}
        RESULT_VARIABLE BUILD_RESULT
        OUTPUT_VARIABLE BUILD_OUTPUT
        ERROR_VARIABLE BUILD_ERROR
)
if (NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Generated library build failed:\n${BUILD_OUTPUT}${BUILD_ERROR}")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
