foreach (required_variable IN ITEMS
        BUILD_DIRECTORY BUILD_CONFIG WORK_DIRECTORY INSTALL_BINDIR CLI_SUFFIX PLATFORM_ID HOST_GENERATOR
)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

get_filename_component(CMAKE_BIN_DIRECTORY "${CMAKE_COMMAND}" DIRECTORY)
if (WIN32)
    set(ENV{PATH} "${CMAKE_BIN_DIRECTORY};$ENV{PATH}")
else ()
    set(ENV{PATH} "${CMAKE_BIN_DIRECTORY}:$ENV{PATH}")
endif ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/hux-${TEST_NONCE}")
set(SDK_ROOT "${TEST_ROOT}/sdk")
set(PROJECT_PARENT "${TEST_ROOT}")
set(PROJECT_ROOT "${PROJECT_PARENT}/app")
file(MAKE_DIRECTORY "${PROJECT_PARENT}")

set(INSTALL_COMMAND
        "${CMAKE_COMMAND}" --install "${BUILD_DIRECTORY}"
        --prefix "${SDK_ROOT}"
)
if (BUILD_CONFIG)
    list(APPEND INSTALL_COMMAND --config "${BUILD_CONFIG}")
endif ()
execute_process(
        COMMAND ${INSTALL_COMMAND}
        RESULT_VARIABLE INSTALL_RESULT
        OUTPUT_VARIABLE INSTALL_OUTPUT
        ERROR_VARIABLE INSTALL_ERROR
)
if (NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "SDK installation failed:\n${INSTALL_OUTPUT}${INSTALL_ERROR}")
endif ()
set(HUXERUI_CLI "${SDK_ROOT}/${INSTALL_BINDIR}/huxerui${CLI_SUFFIX}")
string(TOLOWER "${BUILD_CONFIG}" BUILD_PROFILE)
if (NOT BUILD_PROFILE)
    set(BUILD_PROFILE debug)
endif ()
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=HUXERUI_SDK_ROOT
                "${HUXERUI_CLI}" create app --platform "${PLATFORM_ID}"
        WORKING_DIRECTORY "${PROJECT_PARENT}"
        RESULT_VARIABLE CREATE_RESULT
        OUTPUT_VARIABLE CREATE_OUTPUT
        ERROR_VARIABLE CREATE_ERROR
)
if (NOT CREATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Installed CLI project creation failed:\n${CREATE_OUTPUT}${CREATE_ERROR}")
endif ()

set(APP_MAIN "${PROJECT_ROOT}/src/main.cpp")
file(READ "${APP_MAIN}" APP_MAIN_CONTENT)
string(REPLACE
        "using namespace huxerui;\n"
        "using namespace huxerui;\n\nint AdditionalSource();\n"
        APP_MAIN_CONTENT
        "${APP_MAIN_CONTENT}"
)
string(REPLACE
        "View App() {\n"
        "View App() {\n  static_cast<void>(AdditionalSource());\n"
        APP_MAIN_CONTENT
        "${APP_MAIN_CONTENT}"
)
file(WRITE "${APP_MAIN}" "${APP_MAIN_CONTENT}")
file(WRITE "${PROJECT_ROOT}/src/extra.cpp" "int AdditionalSource() {\n  return 42;\n}\n")

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=HUXERUI_SDK_ROOT
                "${HUXERUI_CLI}" build "${PLATFORM_ID}"
                --profile "${BUILD_PROFILE}"
                --generator "${HOST_GENERATOR}"
        WORKING_DIRECTORY "${PROJECT_ROOT}"
        RESULT_VARIABLE BUILD_RESULT
        OUTPUT_VARIABLE BUILD_OUTPUT
        ERROR_VARIABLE BUILD_ERROR
)
if (NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Installed SDK consumer build failed:\n${BUILD_OUTPUT}${BUILD_ERROR}")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
