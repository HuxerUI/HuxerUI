foreach (required_variable IN ITEMS CLI_EXECUTABLE SOURCE_DIRECTORY WORK_DIRECTORY HOST_GENERATOR PLATFORM_ID)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

file(TO_CMAKE_PATH "${WORK_DIRECTORY}" WORK_DIRECTORY)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/huxerui-generated-library-${TEST_NONCE}")
set(LIBRARY_SOURCE "${TEST_ROOT}/CameraKit")
set(PREVIEW_SOURCE "${LIBRARY_SOURCE}/examples/preview")
set(PREVIEW_BUILD "${TEST_ROOT}/build")
file(MAKE_DIRECTORY "${TEST_ROOT}")

execute_process(
        COMMAND "${CLI_EXECUTABLE}" create library CameraKit
                --namespace huxerui::camera
                --target HuxerUI::Camera
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

set(LIBRARY_GRAPH "${PREVIEW_SOURCE}/.huxerui/generated/libraries.json")
set(GRAPH_SDK "${TEST_ROOT}/graph-sdk")
file(MAKE_DIRECTORY "${GRAPH_SDK}/lib/cmake/HuxerUI")
file(COPY "${SOURCE_DIRECTORY}/cmake/" DESTINATION "${GRAPH_SDK}/lib/cmake/HuxerUI"
        FILES_MATCHING PATTERN "*.cmake"
)
include(CMakePackageConfigHelpers)
set(CMAKE_INSTALL_DATADIR share)
set(CMAKE_INSTALL_INCLUDEDIR include)
configure_package_config_file("${SOURCE_DIRECTORY}/cmake/HuxerUIConfig.cmake.in"
        "${GRAPH_SDK}/lib/cmake/HuxerUI/HuxerUIConfig.cmake"
        INSTALL_DESTINATION lib/cmake/HuxerUI
)

# These files deliberately fail if graph discovery enters a host platform or enables a compiler.
set(GRAPH_GUARD "${TEST_ROOT}/graph-guard.cmake")
file(WRITE "${GRAPH_GUARD}" [=[
if (HUXERUI_LIBRARY_GRAPH_ONLY)
    get_property(enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    if (NOT enabled_languages STREQUAL "NONE")
        message(FATAL_ERROR "Graph discovery enabled a build language")
    endif ()
endif ()
]=])
file(APPEND "${PREVIEW_SOURCE}/CMakeLists.txt" "\ninclude(\"${GRAPH_GUARD}\")\n")
file(READ "${PREVIEW_SOURCE}/platform/${PLATFORM_ID}/huxerui.cmake" PLATFORM_CMAKE)
file(WRITE "${PREVIEW_SOURCE}/platform/${PLATFORM_ID}/huxerui.cmake"
        "if(HUXERUI_LIBRARY_GRAPH_ONLY)\nmessage(FATAL_ERROR \"Graph discovery configured the host platform\")\nendif()\n"
        "${PLATFORM_CMAKE}"
)
foreach (GRAPH_HOME IN ITEMS "${SOURCE_DIRECTORY}" "${GRAPH_SDK}")
    string(SHA256 GRAPH_HOME_HASH "${GRAPH_HOME}")
    set(GRAPH_CONFIGURE_COMMAND
            "${CMAKE_COMMAND}" -S "${PREVIEW_SOURCE}" -B "${TEST_ROOT}/graph-${GRAPH_HOME_HASH}"
            -G "${HOST_GENERATOR}"
            -DHUXERUI_LIBRARY_GRAPH_ONLY=ON
            "-DHUXERUI_LIBRARY_GRAPH_OUTPUT=${LIBRARY_GRAPH}"
            "-DHUXERUI_HOME=${GRAPH_HOME}"
            "-DCMAKE_CXX_COMPILER=${TEST_ROOT}/unavailable-compiler"
    )
    foreach (GRAPH_PASS RANGE 1 2)
        execute_process(COMMAND ${GRAPH_CONFIGURE_COMMAND}
                RESULT_VARIABLE GRAPH_RESULT OUTPUT_VARIABLE GRAPH_OUTPUT ERROR_VARIABLE GRAPH_ERROR
        )
        if (NOT GRAPH_RESULT EQUAL 0)
            message(FATAL_ERROR "Generated library graph failed for ${GRAPH_HOME}:\n${GRAPH_OUTPUT}${GRAPH_ERROR}")
        endif ()
        file(READ "${LIBRARY_GRAPH}" GRAPH_CONTENT)
        string(JSON GRAPH_TARGET GET "${GRAPH_CONTENT}" libraries 0 target)
        string(JSON GRAPH_SOURCE GET "${GRAPH_CONTENT}" libraries 0 sourceRoot)
        if (NOT GRAPH_TARGET STREQUAL "HuxerUI::Camera" OR NOT GRAPH_SOURCE STREQUAL "${LIBRARY_SOURCE}")
            message(FATAL_ERROR "Generated library graph did not preserve the public target and source root")
        endif ()
        file(TIMESTAMP "${LIBRARY_GRAPH}" GRAPH_TIMESTAMP "%s.%f")
        if (GRAPH_PASS EQUAL 1)
            set(GRAPH_FIRST_TIMESTAMP "${GRAPH_TIMESTAMP}")
            execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
        elseif (NOT GRAPH_TIMESTAMP STREQUAL GRAPH_FIRST_TIMESTAMP)
            message(FATAL_ERROR "Unchanged graph discovery rewrote its output")
        endif ()
    endforeach ()
endforeach ()
file(TIMESTAMP "${LIBRARY_GRAPH}" GRAPH_TIMESTAMP_BEFORE_BUILD "%s.%f")

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

file(TIMESTAMP "${LIBRARY_GRAPH}" GRAPH_TIMESTAMP_AFTER_CONFIGURE "%s.%f")
if (NOT GRAPH_TIMESTAMP_AFTER_CONFIGURE STREQUAL GRAPH_TIMESTAMP_BEFORE_BUILD)
    message(FATAL_ERROR "Native configuration overwrote the platform library graph")
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
file(TIMESTAMP "${LIBRARY_GRAPH}" GRAPH_TIMESTAMP_AFTER_BUILD "%s.%f")
if (NOT GRAPH_TIMESTAMP_AFTER_BUILD STREQUAL GRAPH_TIMESTAMP_BEFORE_BUILD)
    message(FATAL_ERROR "Native build overwrote the platform library graph")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
