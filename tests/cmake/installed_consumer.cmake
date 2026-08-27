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
file(TO_NATIVE_PATH "${SDK_ROOT}" SDK_ROOT_NATIVE)

file(GLOB_RECURSE INSTALLED_CMAKE_FILES "${SDK_ROOT}/*/cmake/HuxerUI/*.cmake")
foreach (INSTALLED_CMAKE_FILE IN LISTS INSTALLED_CMAKE_FILES)
    file(READ "${INSTALLED_CMAKE_FILE}" INSTALLED_CMAKE_CONTENT)
    string(FIND "${INSTALLED_CMAKE_CONTENT}" "${BUILD_DIRECTORY}" BUILD_PATH_POSITION)
    if (NOT BUILD_PATH_POSITION EQUAL -1)
        message(FATAL_ERROR "Installed CMake package contains the build path: ${INSTALLED_CMAKE_FILE}")
    endif ()
endforeach ()

foreach (CONSUMER_LINKAGE IN ITEMS shared static default both)
    set(CONSUMER_SOURCE "${TEST_ROOT}/consumer-${CONSUMER_LINKAGE}")
    set(CONSUMER_BUILD "${TEST_ROOT}/consumer-${CONSUMER_LINKAGE}-build")
    file(MAKE_DIRECTORY "${CONSUMER_SOURCE}")
    file(WRITE "${CONSUMER_SOURCE}/main.cpp"
            "#include <huxerui/huxerui.h>\nint main() { huxerui::Bytes bytes{std::byte{1}}; huxerui::View view; return view ? static_cast<int>(bytes.size()) : 0; }\n")
    set(CONSUMER_COMPONENT)
    set(CONSUMER_CONFIGURE_ARGUMENTS)
    if (CONSUMER_LINKAGE STREQUAL "shared")
        set(CONSUMER_COMPONENT " COMPONENTS shared")
        set(CONSUMER_TARGET huxerui)
        set(CONSUMER_TARGET_EXPECTATIONS [=[
if (TARGET HuxerUI::huxerui_static)
  message(FATAL_ERROR "Shared component imported the static target")
endif ()
]=])
        if (PLATFORM_ID STREQUAL "linux")
            list(APPEND CONSUMER_CONFIGURE_ARGUMENTS -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE)
        endif ()
    elseif (CONSUMER_LINKAGE STREQUAL "static")
        set(CONSUMER_COMPONENT " COMPONENTS static")
        set(CONSUMER_TARGET huxerui_static)
        set(CONSUMER_TARGET_EXPECTATIONS [=[
if (TARGET HuxerUI::huxerui)
  message(FATAL_ERROR "Static component imported the shared target")
endif ()
]=])
    elseif (CONSUMER_LINKAGE STREQUAL "both")
        set(CONSUMER_COMPONENT " COMPONENTS shared static")
        set(CONSUMER_TARGET huxerui_static)
        set(CONSUMER_TARGET_EXPECTATIONS [=[
if (NOT TARGET HuxerUI::huxerui OR NOT TARGET HuxerUI::huxerui_static)
  message(FATAL_ERROR "Combined components did not import both targets")
endif ()
]=])
    else ()
        set(CONSUMER_TARGET huxerui_static)
        set(CONSUMER_TARGET_EXPECTATIONS [=[
if (NOT TARGET HuxerUI::huxerui OR NOT TARGET HuxerUI::huxerui_static)
  message(FATAL_ERROR "Default package loading did not import both targets")
endif ()
]=])
    endif ()
    file(WRITE "${CONSUMER_SOURCE}/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(huxerui_installed_consumer LANGUAGES CXX)\n"
            "find_package(HuxerUI CONFIG REQUIRED${CONSUMER_COMPONENT})\n"
            "${CONSUMER_TARGET_EXPECTATIONS}"
            "add_executable(consumer main.cpp)\n"
            "target_compile_features(consumer PRIVATE cxx_std_20)\n"
            "target_link_libraries(consumer PRIVATE HuxerUI::${CONSUMER_TARGET})\n")
    execute_process(
            COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${CONSUMER_BUILD}"
                    -G "${HOST_GENERATOR}" "-DCMAKE_PREFIX_PATH=${SDK_ROOT}"
                    ${CONSUMER_CONFIGURE_ARGUMENTS}
            RESULT_VARIABLE CONSUMER_CONFIGURE_RESULT
            OUTPUT_VARIABLE CONSUMER_CONFIGURE_OUTPUT
            ERROR_VARIABLE CONSUMER_CONFIGURE_ERROR
    )
    execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD}" --config "${BUILD_CONFIG}"
            RESULT_VARIABLE CONSUMER_BUILD_RESULT
            OUTPUT_VARIABLE CONSUMER_BUILD_OUTPUT
            ERROR_VARIABLE CONSUMER_BUILD_ERROR
    )
    if (NOT CONSUMER_CONFIGURE_RESULT EQUAL 0 OR NOT CONSUMER_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR
                "Relocated installed ${CONSUMER_LINKAGE} consumer failed:\n"
                "${CONSUMER_CONFIGURE_OUTPUT}${CONSUMER_CONFIGURE_ERROR}"
                "${CONSUMER_BUILD_OUTPUT}${CONSUMER_BUILD_ERROR}")
    endif ()
endforeach ()

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
string(FIND "${DOCTOR_OUTPUT}" "[ok] HUXERUI_HOME (environment): ${SDK_ROOT_NATIVE}" SDK_OUTPUT_POSITION)
if (SDK_OUTPUT_POSITION LESS 0 OR DOCTOR_ERROR)
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

set(BUILD_COMMAND
        "${CMAKE_COMMAND}" -E env --unset=HUXERUI_HOME
        "HUXERUI_SDK_ROOT=${TEST_ROOT}/missing"
        "${HUXERUI_CLI}" build "${PLATFORM_ID}"
        --profile "${BUILD_PROFILE}"
)
if (NOT PLATFORM_ID STREQUAL "windows")
    list(APPEND BUILD_COMMAND --generator "${HOST_GENERATOR}")
endif ()
execute_process(
        COMMAND ${BUILD_COMMAND}
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
        progress_in_progress
        text_selection_copy
        validation_required
        window_restore
)
    if (NOT BUILTIN_RESOURCE_HEADER_CONTENT MATCHES "${EXPECTED_CONTENT}")
        message(FATAL_ERROR "Installed SDK consumer generated an invalid HuxerUI resource header")
    endif ()
endforeach ()

file(REMOVE_RECURSE "${TEST_ROOT}")
