foreach (required_variable IN ITEMS SOURCE_DIRECTORY WORK_DIRECTORY HOST_GENERATOR)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/huxerui-library-validation-${TEST_NONCE}")

function(expect_library_configure_failure case_name invocation expected_error)
    set(PROJECT_ROOT "${TEST_ROOT}/${case_name}-source")
    set(BUILD_ROOT "${TEST_ROOT}/${case_name}-build")
    file(MAKE_DIRECTORY "${PROJECT_ROOT}")
    file(WRITE "${PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
    file(WRITE "${PROJECT_ROOT}/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(${case_name} LANGUAGES CXX)\n"
            "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUILibraries.cmake\")\n"
            "add_executable(library_app main.cpp)\n"
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

expect_library_configure_failure(
        library_path_and_url
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit PATH camera URL https://example.com/camera.git)"
        "PATH and URL are mutually exclusive"
)
expect_library_configure_failure(
        library_insecure_url
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit URL http://example.com/camera.git COMMIT 0123456789abcdef0123456789abcdef01234567)"
        "URL must use HTTPS"
)
expect_library_configure_failure(
        library_missing_checkout
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit URL https://example.com/camera.git)"
        "URL requires COMMIT or TAG"
)
expect_library_configure_failure(
        library_short_commit
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit URL https://example.com/camera.git COMMIT 0123456789abcdef)"
        "COMMIT must be a full commit SHA"
)
expect_library_configure_failure(
        library_commit_and_tag
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit URL https://example.com/camera.git COMMIT 0123456789abcdef0123456789abcdef01234567 TAG v1.2.3)"
        "COMMIT and TAG are mutually exclusive"
)
expect_library_configure_failure(
        library_commit_without_url
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit COMMIT 0123456789abcdef0123456789abcdef01234567)"
        "COMMIT requires URL"
)
expect_library_configure_failure(
        library_tag_without_url
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit TAG v1.2.3)"
        "TAG requires URL"
)
expect_library_configure_failure(
        library_qualified_tag
        "huxerui_use_library(library_app TARGET CameraKit::CameraKit URL https://example.com/camera.git TAG refs/tags/v1.2.3)"
        "TAG must not include the refs/tags/ prefix"
)
expect_library_configure_failure(
        android_missing_framework
        "set(ANDROID TRUE)\n_huxerui_select_framework_target(framework_target)"
        "HuxerUI Android integration requires the shared component"
)

set(ANDROID_SHARED_PROJECT_ROOT "${TEST_ROOT}/android-shared-source")
set(ANDROID_SHARED_BUILD_ROOT "${TEST_ROOT}/android-shared-build")
file(MAKE_DIRECTORY "${ANDROID_SHARED_PROJECT_ROOT}")
file(WRITE "${ANDROID_SHARED_PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${ANDROID_SHARED_PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(android_shared_framework LANGUAGES CXX)\n"
        "set(ANDROID TRUE)\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUILibraries.cmake\")\n"
        "add_library(HuxerUI::huxerui INTERFACE IMPORTED)\n"
        "_huxerui_select_framework_target(framework_target)\n"
        "if (NOT framework_target STREQUAL \"HuxerUI::huxerui\")\n"
        "  message(FATAL_ERROR \"Android shared framework target was not selected\")\n"
        "endif ()\n"
)
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${ANDROID_SHARED_PROJECT_ROOT}"
                -B "${ANDROID_SHARED_BUILD_ROOT}"
                -G "${HOST_GENERATOR}"
        RESULT_VARIABLE ANDROID_SHARED_CONFIGURE_RESULT
        OUTPUT_VARIABLE ANDROID_SHARED_CONFIGURE_OUTPUT
        ERROR_VARIABLE ANDROID_SHARED_CONFIGURE_ERROR
)
if (NOT ANDROID_SHARED_CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Android shared framework target configure failed:\n${ANDROID_SHARED_CONFIGURE_OUTPUT}${ANDROID_SHARED_CONFIGURE_ERROR}"
    )
endif ()

set(PREEXISTING_LIBRARY_ROOT "${TEST_ROOT}/preexisting-library")
file(MAKE_DIRECTORY "${PREEXISTING_LIBRARY_ROOT}")
file(WRITE "${PREEXISTING_LIBRARY_ROOT}/CMakeLists.txt" "add_library(unused INTERFACE)\n")
expect_library_configure_failure(
        library_ambiguous_existing_target
        "add_library(existing_library INTERFACE)\nset_property(TARGET existing_library PROPERTY HUXERUI_LIBRARY TRUE)\nhuxerui_use_library(library_app TARGET existing_library PATH \"${PREEXISTING_LIBRARY_ROOT}\")"
        "TARGET already exists without a matching acquisition"
)

set(REMOTE_LIBRARY_URL "https://github.com/example/huxerui-test-library.git")
set(REMOTE_LIBRARY_COMMIT "0123456789abcdef0123456789abcdef01234567")
set(REMOTE_LIBRARY_ROOT "${TEST_ROOT}/remote-library")
set(TAGGED_LIBRARY_URL "https://github.com/example/huxerui-tagged-library.git")
set(TAGGED_LIBRARY_TAG "0")
set(TAGGED_LIBRARY_ROOT "${TEST_ROOT}/tagged-library")
set(REMOTE_PROJECT_ROOT "${TEST_ROOT}/remote-source")
set(REMOTE_BUILD_ROOT "${TEST_ROOT}/remote-build")
file(MAKE_DIRECTORY
        "${REMOTE_LIBRARY_ROOT}"
        "${TAGGED_LIBRARY_ROOT}"
        "${REMOTE_PROJECT_ROOT}"
)
file(WRITE "${REMOTE_LIBRARY_ROOT}/CMakeLists.txt"
        "add_library(huxerui_remote_library INTERFACE)\n"
        "set_property(TARGET huxerui_remote_library PROPERTY HUXERUI_LIBRARY TRUE)\n"
        "add_library(RemoteLibrary::RemoteLibrary ALIAS huxerui_remote_library)\n"
)
file(WRITE "${TAGGED_LIBRARY_ROOT}/CMakeLists.txt"
        "add_library(huxerui_tagged_library INTERFACE)\n"
        "set_property(TARGET huxerui_tagged_library PROPERTY HUXERUI_LIBRARY TRUE)\n"
        "add_library(TaggedLibrary::TaggedLibrary ALIAS huxerui_tagged_library)\n"
)
file(WRITE "${REMOTE_PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
string(SHA256 REMOTE_LIBRARY_HASH
        "URL:${REMOTE_LIBRARY_URL}@COMMIT:${REMOTE_LIBRARY_COMMIT}"
)
string(SUBSTRING "${REMOTE_LIBRARY_HASH}" 0 16 REMOTE_LIBRARY_HASH)
string(TOUPPER
        "huxerui_library_${REMOTE_LIBRARY_HASH}"
        REMOTE_LIBRARY_FETCH_NAME
)
string(SHA256 TAGGED_LIBRARY_HASH
        "URL:${TAGGED_LIBRARY_URL}@TAG:${TAGGED_LIBRARY_TAG}"
)
string(SUBSTRING "${TAGGED_LIBRARY_HASH}" 0 16 TAGGED_LIBRARY_HASH)
string(TOUPPER
        "huxerui_library_${TAGGED_LIBRARY_HASH}"
        TAGGED_LIBRARY_FETCH_NAME
)
file(WRITE "${REMOTE_PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(remote_library LANGUAGES CXX)\n"
        "set(CMAKE_DISABLE_FIND_PACKAGE_Git TRUE)\n"
        "set(FETCHCONTENT_SOURCE_DIR_${REMOTE_LIBRARY_FETCH_NAME} \"${REMOTE_LIBRARY_ROOT}\" CACHE PATH \"\" FORCE)\n"
        "set(FETCHCONTENT_SOURCE_DIR_${TAGGED_LIBRARY_FETCH_NAME} \"${TAGGED_LIBRARY_ROOT}\" CACHE PATH \"\" FORCE)\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUILibraries.cmake\")\n"
        "add_executable(library_app main.cpp)\n"
        "huxerui_use_library(library_app\n"
        "        TARGET RemoteLibrary::RemoteLibrary\n"
        "        URL \"${REMOTE_LIBRARY_URL}\"\n"
        "        COMMIT \"${REMOTE_LIBRARY_COMMIT}\"\n"
        ")\n"
        "huxerui_use_library(library_app\n"
        "        TARGET TaggedLibrary::TaggedLibrary\n"
        "        URL \"${TAGGED_LIBRARY_URL}\"\n"
        "        TAG \"${TAGGED_LIBRARY_TAG}\"\n"
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
            "HTTPS library target configure failed:\n${REMOTE_CONFIGURE_OUTPUT}${REMOTE_CONFIGURE_ERROR}"
    )
endif ()

set(PREDECLARED_PROJECT_ROOT "${TEST_ROOT}/predeclared-source")
set(PREDECLARED_BUILD_ROOT "${TEST_ROOT}/predeclared-build")
file(MAKE_DIRECTORY "${PREDECLARED_PROJECT_ROOT}")
file(WRITE "${PREDECLARED_PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${PREDECLARED_PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(predeclared_library LANGUAGES CXX)\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUILibraries.cmake\")\n"
        "add_executable(library_app main.cpp)\n"
        "add_library(PredeclaredLibrary::PredeclaredLibrary INTERFACE IMPORTED)\n"
        "set_property(TARGET PredeclaredLibrary::PredeclaredLibrary PROPERTY HUXERUI_LIBRARY TRUE)\n"
        "huxerui_use_library(library_app TARGET PredeclaredLibrary::PredeclaredLibrary)\n"
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
            "Predeclared library target configure failed:\n${PREDECLARED_CONFIGURE_OUTPUT}${PREDECLARED_CONFIGURE_ERROR}"
    )
endif ()

set(LIBRARY_GRAPH_PROJECT_ROOT "${TEST_ROOT}/library-graph-source")
set(LIBRARY_GRAPH_BUILD_ROOT "${TEST_ROOT}/library-graph-build")
set(LIBRARY_GRAPH_FIRST_ROOT "${TEST_ROOT}/library-graph-first")
set(LIBRARY_GRAPH_SECOND_ROOT "${TEST_ROOT}/library-graph-second")
set(LIBRARY_GRAPH_OUTPUT "${TEST_ROOT}/library-graph.json")
file(MAKE_DIRECTORY
        "${LIBRARY_GRAPH_PROJECT_ROOT}"
        "${LIBRARY_GRAPH_FIRST_ROOT}"
        "${LIBRARY_GRAPH_SECOND_ROOT}"
)
file(WRITE "${LIBRARY_GRAPH_PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${LIBRARY_GRAPH_PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(library_graph LANGUAGES CXX)\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUILibraries.cmake\")\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUIApp.cmake\")\n"
        "set(HUXERUI_LIBRARY_GRAPH_ONLY ON)\n"
        "set(HUXERUI_LIBRARY_GRAPH_OUTPUT \"${LIBRARY_GRAPH_OUTPUT}\")\n"
        "huxerui_add_app(library_app SOURCES main.cpp)\n"
        "add_library(first_library INTERFACE)\n"
        "set_property(TARGET first_library PROPERTY HUXERUI_LIBRARY TRUE)\n"
        "set_property(TARGET first_library PROPERTY HUXERUI_LIBRARY_SOURCE_ROOT \"${LIBRARY_GRAPH_FIRST_ROOT}\")\n"
        "add_library(FirstLibrary::FirstLibrary ALIAS first_library)\n"
        "add_library(second_library INTERFACE)\n"
        "set_property(TARGET second_library PROPERTY HUXERUI_LIBRARY TRUE)\n"
        "set_property(TARGET second_library PROPERTY HUXERUI_LIBRARY_SOURCE_ROOT \"${LIBRARY_GRAPH_SECOND_ROOT}\")\n"
        "add_library(SecondLibrary::SecondLibrary ALIAS second_library)\n"
        "add_library(predeclared_library INTERFACE)\n"
        "set_property(TARGET predeclared_library PROPERTY HUXERUI_LIBRARY TRUE)\n"
        "huxerui_use_library(library_app TARGET SecondLibrary::SecondLibrary)\n"
        "huxerui_use_library(library_app TARGET predeclared_library)\n"
        "huxerui_use_library(library_app TARGET FirstLibrary::FirstLibrary)\n"
)
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${LIBRARY_GRAPH_PROJECT_ROOT}"
                -B "${LIBRARY_GRAPH_BUILD_ROOT}"
                -G "${HOST_GENERATOR}"
        RESULT_VARIABLE LIBRARY_GRAPH_CONFIGURE_RESULT
        OUTPUT_VARIABLE LIBRARY_GRAPH_CONFIGURE_OUTPUT
        ERROR_VARIABLE LIBRARY_GRAPH_CONFIGURE_ERROR
)
if (NOT LIBRARY_GRAPH_CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Library graph configure failed:\n${LIBRARY_GRAPH_CONFIGURE_OUTPUT}${LIBRARY_GRAPH_CONFIGURE_ERROR}"
    )
endif ()
file(READ "${LIBRARY_GRAPH_OUTPUT}" LIBRARY_GRAPH_JSON)
string(JSON LIBRARY_GRAPH_COUNT LENGTH "${LIBRARY_GRAPH_JSON}" libraries)
string(JSON LIBRARY_GRAPH_FIRST_TARGET GET "${LIBRARY_GRAPH_JSON}" libraries 0 target)
string(JSON LIBRARY_GRAPH_FIRST_SOURCE_ROOT GET "${LIBRARY_GRAPH_JSON}" libraries 0 sourceRoot)
string(JSON LIBRARY_GRAPH_SECOND_TARGET GET "${LIBRARY_GRAPH_JSON}" libraries 1 target)
string(JSON LIBRARY_GRAPH_SECOND_SOURCE_ROOT GET "${LIBRARY_GRAPH_JSON}" libraries 1 sourceRoot)
if (NOT LIBRARY_GRAPH_COUNT EQUAL 2
        OR NOT LIBRARY_GRAPH_FIRST_TARGET STREQUAL "SecondLibrary::SecondLibrary"
        OR NOT LIBRARY_GRAPH_FIRST_SOURCE_ROOT STREQUAL "${LIBRARY_GRAPH_SECOND_ROOT}"
        OR NOT LIBRARY_GRAPH_SECOND_TARGET STREQUAL "FirstLibrary::FirstLibrary"
        OR NOT LIBRARY_GRAPH_SECOND_SOURCE_ROOT STREQUAL "${LIBRARY_GRAPH_FIRST_ROOT}")
    message(FATAL_ERROR "Library graph does not preserve library order and source roots")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
