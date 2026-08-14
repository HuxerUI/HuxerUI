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
set(SDK_INSTALL_ROOT "${TEST_ROOT}/sdk-install")
set(SDK_ROOT "${TEST_ROOT}/sdk-relocated")
set(PROJECT_PARENT "${TEST_ROOT}")
set(PROJECT_ROOT "${PROJECT_PARENT}/app")
file(MAKE_DIRECTORY "${PROJECT_PARENT}")

set(INSTALL_COMMAND
        "${CMAKE_COMMAND}" --install "${BUILD_DIRECTORY}"
        --prefix "${SDK_INSTALL_ROOT}"
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
file(GLOB_RECURSE INSTALLED_BUILTIN_RESOURCE_INDEXES
        "${SDK_INSTALL_ROOT}/*/huxerui/resources/huxerui/resources.bin"
)
if (NOT INSTALLED_BUILTIN_RESOURCE_INDEXES)
    message(FATAL_ERROR "Installed SDK is missing the HuxerUI built-in resource package")
endif ()
file(RENAME "${SDK_INSTALL_ROOT}" "${SDK_ROOT}")
file(REAL_PATH "${SDK_ROOT}" SDK_ROOT)
set(HUXERUI_CLI "${SDK_ROOT}/${INSTALL_BINDIR}/huxerui${CLI_SUFFIX}")
string(TOLOWER "${BUILD_CONFIG}" BUILD_PROFILE)
if (NOT BUILD_PROFILE)
    set(BUILD_PROFILE debug)
endif ()
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "HUXERUI_HOME=${SDK_ROOT}"
                "${HUXERUI_CLI}" doctor
        WORKING_DIRECTORY "${PROJECT_PARENT}"
        OUTPUT_VARIABLE DOCTOR_OUTPUT
        ERROR_VARIABLE DOCTOR_ERROR
)
if (NOT DOCTOR_OUTPUT MATCHES "HUXERUI_HOME \\(environment\\): ${SDK_ROOT}"
        OR DOCTOR_ERROR)
    message(FATAL_ERROR "Relocated SDK environment discovery failed:\n${DOCTOR_OUTPUT}${DOCTOR_ERROR}")
endif ()
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "HUXERUI_HOME=${TEST_ROOT}/missing"
                "${HUXERUI_CLI}" --version
        RESULT_VARIABLE INVALID_HOME_RESULT
        OUTPUT_VARIABLE INVALID_HOME_OUTPUT
        ERROR_VARIABLE INVALID_HOME_ERROR
)
if (INVALID_HOME_RESULT EQUAL 0
        OR NOT INVALID_HOME_ERROR MATCHES "HUXERUI_HOME is not a HuxerUI SDK or source checkout")
    message(FATAL_ERROR "Invalid HUXERUI_HOME was not rejected:\n${INVALID_HOME_OUTPUT}${INVALID_HOME_ERROR}")
endif ()
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=HUXERUI_HOME
                "HUXERUI_SDK_ROOT=${TEST_ROOT}/missing"
                "${HUXERUI_CLI}" create app app --platform "${PLATFORM_ID}"
        WORKING_DIRECTORY "${PROJECT_PARENT}"
        RESULT_VARIABLE CREATE_RESULT
        OUTPUT_VARIABLE CREATE_OUTPUT
        ERROR_VARIABLE CREATE_ERROR
)
if (NOT CREATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Installed CLI project creation failed:\n${CREATE_OUTPUT}${CREATE_ERROR}")
endif ()

set(APP_SOURCE "${PROJECT_ROOT}/src/app.cpp")
file(READ "${APP_SOURCE}" APP_SOURCE_CONTENT)
string(REPLACE
        "using namespace huxerui;\n"
        "using namespace huxerui;\n\nint AdditionalSource();\n"
        APP_SOURCE_CONTENT
        "${APP_SOURCE_CONTENT}"
)
string(REPLACE
        "View App() {\n"
        "View App() {\n  static_cast<void>(AdditionalSource());\n"
        APP_SOURCE_CONTENT
        "${APP_SOURCE_CONTENT}"
)
file(WRITE "${APP_SOURCE}" "${APP_SOURCE_CONTENT}")
file(WRITE "${PROJECT_ROOT}/src/extra.cpp" "int AdditionalSource() {\n  return 42;\n}\n")

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=HUXERUI_HOME
                "HUXERUI_SDK_ROOT=${TEST_ROOT}/missing"
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
file(GLOB_RECURSE APP_INTEGRATION_PLANS
        "${PROJECT_ROOT}/.huxerui/build/*/*/huxerui-integration/*/app.json"
)
list(LENGTH APP_INTEGRATION_PLANS APP_INTEGRATION_PLAN_COUNT)
if (NOT APP_INTEGRATION_PLAN_COUNT EQUAL 1)
    message(FATAL_ERROR "Installed SDK consumer generated an unexpected number of application integration plans")
endif ()
list(GET APP_INTEGRATION_PLANS 0 APP_INTEGRATION_PLAN)
file(READ "${APP_INTEGRATION_PLAN}" APP_INTEGRATION_JSON)
string(JSON APP_INTEGRATION_PLATFORM GET "${APP_INTEGRATION_JSON}" platform)
if (NOT APP_INTEGRATION_PLATFORM STREQUAL PLATFORM_ID)
    message(FATAL_ERROR
            "Installed SDK consumer reported platform ${APP_INTEGRATION_PLATFORM}, expected ${PLATFORM_ID}"
    )
endif ()
file(GLOB_RECURSE BUILTIN_RESOURCE_HEADERS
        "${PROJECT_ROOT}/.huxerui/build/*/huxerui_resources.h"
)
if (NOT BUILTIN_RESOURCE_HEADERS)
    message(FATAL_ERROR "Installed SDK consumer did not generate the HuxerUI resource header")
endif ()
list(GET BUILTIN_RESOURCE_HEADERS 0 BUILTIN_RESOURCE_HEADER)
file(READ "${BUILTIN_RESOURCE_HEADER}" BUILTIN_RESOURCE_HEADER_CONTENT)
foreach (EXPECTED_CONTENT IN ITEMS
        "namespace huxerui \\{"
        dialog_ok
        text_selection_copy
        validation_required
        window_restore
)
    if (NOT BUILTIN_RESOURCE_HEADER_CONTENT MATCHES "${EXPECTED_CONTENT}")
        message(FATAL_ERROR "Installed SDK consumer generated an invalid HuxerUI resource header")
    endif ()
endforeach ()

file(REMOVE_RECURSE "${TEST_ROOT}")
