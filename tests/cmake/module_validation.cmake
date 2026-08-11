foreach (required_variable IN ITEMS SOURCE_DIRECTORY WORK_DIRECTORY HOST_GENERATOR)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/huxerui-module-validation-${TEST_NONCE}")

function(expect_module_configure_failure case_name invocation expected_error)
    set(PROJECT_ROOT "${TEST_ROOT}/${case_name}-source")
    set(BUILD_ROOT "${TEST_ROOT}/${case_name}-build")
    file(MAKE_DIRECTORY "${PROJECT_ROOT}")
    file(WRITE "${PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
    file(WRITE "${PROJECT_ROOT}/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(${case_name} LANGUAGES CXX)\n"
            "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUIModules.cmake\")\n"
            "add_executable(module_app main.cpp)\n"
            "${invocation}\n"
    )
    execute_process(
            COMMAND "${CMAKE_COMMAND}"
                    -S "${PROJECT_ROOT}"
                    -B "${BUILD_ROOT}"
                    -G "${HOST_GENERATOR}"
            RESULT_VARIABLE CONFIGURE_RESULT
            OUTPUT_VARIABLE CONFIGURE_OUTPUT
            ERROR_VARIABLE CONFIGURE_ERROR
    )
    if (CONFIGURE_RESULT EQUAL 0)
        message(FATAL_ERROR "${case_name} unexpectedly configured")
    endif ()
    if (NOT "${CONFIGURE_OUTPUT}${CONFIGURE_ERROR}" MATCHES "${expected_error}")
        message(FATAL_ERROR
                "${case_name} failed for an unexpected reason:\n${CONFIGURE_OUTPUT}${CONFIGURE_ERROR}"
        )
    endif ()
endfunction()

expect_module_configure_failure(
        module_path_and_url
        "huxerui_use_module(module_app TARGET HuxerUI::camera PATH camera URL https://example.com/camera.git)"
        "PATH and URL are mutually exclusive"
)
expect_module_configure_failure(
        module_insecure_url
        "huxerui_use_module(module_app TARGET HuxerUI::camera URL http://example.com/camera.git REVISION 0123456789abcdef0123456789abcdef01234567)"
        "URL must use HTTPS"
)
expect_module_configure_failure(
        module_missing_revision
        "huxerui_use_module(module_app TARGET HuxerUI::camera URL https://example.com/camera.git)"
        "URL requires REVISION"
)
expect_module_configure_failure(
        module_short_revision
        "huxerui_use_module(module_app TARGET HuxerUI::camera URL https://example.com/camera.git REVISION 0123456789abcdef)"
        "REVISION must be a full commit SHA"
)

set(PREEXISTING_MODULE_ROOT "${TEST_ROOT}/preexisting-module")
file(MAKE_DIRECTORY "${PREEXISTING_MODULE_ROOT}")
file(WRITE "${PREEXISTING_MODULE_ROOT}/CMakeLists.txt" "add_library(unused INTERFACE)\n")
expect_module_configure_failure(
        module_ambiguous_existing_target
        "add_library(existing_module INTERFACE)\nset_property(TARGET existing_module PROPERTY HUXERUI_MODULE TRUE)\nhuxerui_use_module(module_app TARGET existing_module PATH \"${PREEXISTING_MODULE_ROOT}\")"
        "TARGET already exists without a matching acquisition"
)

set(REMOTE_MODULE_URL "https://github.com/example/huxerui-test-module.git")
set(REMOTE_MODULE_REVISION "0123456789abcdef0123456789abcdef01234567")
set(REMOTE_MODULE_ROOT "${TEST_ROOT}/remote-module")
set(REMOTE_PROJECT_ROOT "${TEST_ROOT}/remote-source")
set(REMOTE_BUILD_ROOT "${TEST_ROOT}/remote-build")
file(MAKE_DIRECTORY "${REMOTE_MODULE_ROOT}" "${REMOTE_PROJECT_ROOT}")
file(WRITE "${REMOTE_MODULE_ROOT}/CMakeLists.txt"
        "add_library(huxerui_remote_module INTERFACE)\n"
        "set_property(TARGET huxerui_remote_module PROPERTY HUXERUI_MODULE TRUE)\n"
        "add_library(HuxerUI::remote_module ALIAS huxerui_remote_module)\n"
)
file(WRITE "${REMOTE_PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
string(SHA256 REMOTE_MODULE_HASH
        "URL:${REMOTE_MODULE_URL}@${REMOTE_MODULE_REVISION}"
)
string(SUBSTRING "${REMOTE_MODULE_HASH}" 0 16 REMOTE_MODULE_HASH)
string(TOUPPER
        "huxerui_module_${REMOTE_MODULE_HASH}"
        REMOTE_MODULE_FETCH_NAME
)
file(WRITE "${REMOTE_PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(remote_module LANGUAGES CXX)\n"
        "set(FETCHCONTENT_SOURCE_DIR_${REMOTE_MODULE_FETCH_NAME} \"${REMOTE_MODULE_ROOT}\" CACHE PATH \"\" FORCE)\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUIModules.cmake\")\n"
        "add_executable(module_app main.cpp)\n"
        "huxerui_use_module(module_app\n"
        "        TARGET HuxerUI::remote_module\n"
        "        URL \"${REMOTE_MODULE_URL}\"\n"
        "        REVISION \"${REMOTE_MODULE_REVISION}\"\n"
        ")\n"
)
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${REMOTE_PROJECT_ROOT}"
                -B "${REMOTE_BUILD_ROOT}"
                -G "${HOST_GENERATOR}"
        RESULT_VARIABLE REMOTE_CONFIGURE_RESULT
        OUTPUT_VARIABLE REMOTE_CONFIGURE_OUTPUT
        ERROR_VARIABLE REMOTE_CONFIGURE_ERROR
)
if (NOT REMOTE_CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "HTTPS module target configure failed:\n${REMOTE_CONFIGURE_OUTPUT}${REMOTE_CONFIGURE_ERROR}"
    )
endif ()

set(PREDECLARED_PROJECT_ROOT "${TEST_ROOT}/predeclared-source")
set(PREDECLARED_BUILD_ROOT "${TEST_ROOT}/predeclared-build")
file(MAKE_DIRECTORY "${PREDECLARED_PROJECT_ROOT}")
file(WRITE "${PREDECLARED_PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${PREDECLARED_PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(predeclared_module LANGUAGES CXX)\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUIModules.cmake\")\n"
        "add_executable(module_app main.cpp)\n"
        "add_library(HuxerUI::predeclared INTERFACE IMPORTED)\n"
        "set_property(TARGET HuxerUI::predeclared PROPERTY HUXERUI_MODULE TRUE)\n"
        "huxerui_use_module(module_app TARGET HuxerUI::predeclared)\n"
)
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${PREDECLARED_PROJECT_ROOT}"
                -B "${PREDECLARED_BUILD_ROOT}"
                -G "${HOST_GENERATOR}"
        RESULT_VARIABLE PREDECLARED_CONFIGURE_RESULT
        OUTPUT_VARIABLE PREDECLARED_CONFIGURE_OUTPUT
        ERROR_VARIABLE PREDECLARED_CONFIGURE_ERROR
)
if (NOT PREDECLARED_CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Predeclared module target configure failed:\n${PREDECLARED_CONFIGURE_OUTPUT}${PREDECLARED_CONFIGURE_ERROR}"
    )
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
