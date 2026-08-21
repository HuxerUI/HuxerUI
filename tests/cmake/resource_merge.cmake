foreach (required_variable IN ITEMS SOURCE_DIRECTORY WORK_DIRECTORY HOST_GENERATOR)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/huxerui-resource-merge-${TEST_NONCE}")
set(PROJECT_ROOT "${TEST_ROOT}/source")
set(BUILD_ROOT "${TEST_ROOT}/build")
file(MAKE_DIRECTORY
        "${PROJECT_ROOT}/base/raw"
        "${PROJECT_ROOT}/framework-override/raw"
        "${PROJECT_ROOT}/module/resources/raw"
        "${PROJECT_ROOT}/override/raw"
        "${PROJECT_ROOT}/resources/raw"
)
file(WRITE "${PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${PROJECT_ROOT}/base/raw/config.txt" "base")
file(WRITE "${PROJECT_ROOT}/base/raw/kept.txt" "kept")
file(WRITE "${PROJECT_ROOT}/module/resources/raw/tool.txt" "module")
file(WRITE "${PROJECT_ROOT}/override/raw/config.txt" "override")
file(WRITE "${PROJECT_ROOT}/resources/raw/framework.txt" "framework")
file(WRITE "${PROJECT_ROOT}/resources/raw/framework-kept.txt" "framework-kept")
file(WRITE "${PROJECT_ROOT}/framework-override/raw/framework.txt" "application")
file(WRITE "${PROJECT_ROOT}/module/CMakeLists.txt"
        "huxerui_add_resources(resource_app\n"
        "        ROOT \"${PROJECT_ROOT}/module/resources\"\n"
        "        NAMESPACE editor\n"
        ")\n"
)
file(WRITE "${PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(huxerui_resource_merge LANGUAGES CXX)\n"
        "set(HUXERUI_HOST_TOOL_ROOT \"${SOURCE_DIRECTORY}/tools/prebuilt\")\n"
        "set(HUXERUI_PROJECT_DIR \"${PROJECT_ROOT}\")\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUIBuild.cmake\")\n"
        "add_executable(no_resource_app main.cpp)\n"
        "_huxerui_schedule_resources(no_resource_app)\n"
        "add_executable(resource_app MACOSX_BUNDLE main.cpp)\n"
        "_huxerui_configure_builtin_resources(resource_app)\n"
        "set_property(TARGET resource_app APPEND PROPERTY HUXERUI_RESOURCE_PACKAGES \"\${HUXERUI_BUILTIN_RESOURCE_PACKAGE}\")\n"
        "_huxerui_schedule_resources(resource_app)\n"
        "add_subdirectory(module)\n"
        "huxerui_add_resources(resource_app ROOT base NAMESPACE app)\n"
        "huxerui_add_resources(resource_app ROOT override NAMESPACE app)\n"
        "huxerui_add_resources(resource_app ROOT framework-override NAMESPACE huxerui)\n"
)

execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${PROJECT_ROOT}"
                -B "${BUILD_ROOT}"
                -G "${HOST_GENERATOR}"
                -DCMAKE_BUILD_TYPE=Debug
        RESULT_VARIABLE CONFIGURE_RESULT
        OUTPUT_VARIABLE CONFIGURE_OUTPUT
        ERROR_VARIABLE CONFIGURE_ERROR
)
if (NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Resource merge configure failed:\n${CONFIGURE_OUTPUT}${CONFIGURE_ERROR}"
    )
endif ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}"
                --config Debug
                --target no_resource_app resource_app
        RESULT_VARIABLE BUILD_RESULT
        OUTPUT_VARIABLE BUILD_OUTPUT
        ERROR_VARIABLE BUILD_ERROR
)
if (NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Resource merge build failed:\n${BUILD_OUTPUT}${BUILD_ERROR}"
    )
endif ()
if (EXISTS "${BUILD_ROOT}/huxerui-resources/no_resource_app")
    message(FATAL_ERROR "Resource scheduling created output for a target without resources")
endif ()

set(RESOURCE_OUTPUT "${BUILD_ROOT}/huxerui-resources/resource_app")
set(BUILTIN_RESOURCE_OUTPUT "${BUILD_ROOT}/huxerui-builtin-resources")
file(READ "${RESOURCE_OUTPUT}/package/huxerui/app/raw/config.txt" CONFIG_VALUE)
file(READ "${RESOURCE_OUTPUT}/package/huxerui/app/raw/kept.txt" KEPT_VALUE)
file(READ "${RESOURCE_OUTPUT}/package/huxerui/editor/raw/tool.txt" MODULE_VALUE)
file(READ "${RESOURCE_OUTPUT}/package/huxerui/huxerui/raw/framework.txt" FRAMEWORK_VALUE)
file(READ "${RESOURCE_OUTPUT}/package/huxerui/huxerui/raw/framework-kept.txt" FRAMEWORK_KEPT_VALUE)
file(READ "${RESOURCE_OUTPUT}/include/app_resources.h" APP_HEADER)
file(READ "${RESOURCE_OUTPUT}/include/editor_resources.h" MODULE_HEADER)
file(READ "${RESOURCE_OUTPUT}/include/huxerui_resources.h" FRAMEWORK_HEADER)
file(READ "${BUILTIN_RESOURCE_OUTPUT}/include/huxerui_builtin_resources.h" BUILTIN_HEADER)
if (NOT CONFIG_VALUE STREQUAL "override"
        OR NOT KEPT_VALUE STREQUAL "kept"
        OR NOT MODULE_VALUE STREQUAL "module"
        OR NOT FRAMEWORK_VALUE STREQUAL "application"
        OR NOT FRAMEWORK_KEPT_VALUE STREQUAL "framework-kept")
    message(FATAL_ERROR "Ordered resource merge produced unexpected payloads")
endif ()
if (NOT APP_HEADER MATCHES "namespace app \\{"
        OR APP_HEADER MATCHES "namespace app_resources"
        OR NOT MODULE_HEADER MATCHES "namespace editor \\{"
        OR NOT FRAMEWORK_HEADER MATCHES "framework_txt"
        OR NOT FRAMEWORK_HEADER MATCHES "framework_kept_txt")
    message(FATAL_ERROR "Ordered resource merge produced unexpected headers")
endif ()
if (NOT EXISTS "${BUILTIN_RESOURCE_OUTPUT}/package/huxerui/huxerui/raw/framework.txt"
        OR NOT BUILTIN_HEADER MATCHES "framework_txt")
    message(FATAL_ERROR "Built-in resource generation produced unexpected output")
endif ()
if (EXISTS "${RESOURCE_OUTPUT}/resources.stamp"
        OR EXISTS "${RESOURCE_OUTPUT}/fragments"
        OR EXISTS "${RESOURCE_OUTPUT}/roots")
    message(FATAL_ERROR "Resource merge retained intermediate output")
endif ()

file(REMOVE "${PROJECT_ROOT}/base/raw/kept.txt")
file(REMOVE "${PROJECT_ROOT}/resources/raw/framework-kept.txt")
execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}"
                --config Debug
                --target resource_app
        RESULT_VARIABLE REBUILD_RESULT
        OUTPUT_VARIABLE REBUILD_OUTPUT
        ERROR_VARIABLE REBUILD_ERROR
)
if (NOT REBUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Resource merge rebuild failed:\n${REBUILD_OUTPUT}${REBUILD_ERROR}"
    )
endif ()
file(READ "${RESOURCE_OUTPUT}/include/app_resources.h" UPDATED_APP_HEADER)
file(READ "${RESOURCE_OUTPUT}/include/huxerui_resources.h" UPDATED_FRAMEWORK_HEADER)
file(READ "${BUILTIN_RESOURCE_OUTPUT}/include/huxerui_builtin_resources.h" UPDATED_BUILTIN_HEADER)
if (EXISTS "${RESOURCE_OUTPUT}/package/huxerui/app/raw/kept.txt"
        OR UPDATED_APP_HEADER MATCHES "kept_txt")
    message(FATAL_ERROR "Resource merge retained a deleted resource")
endif ()
if (EXISTS "${RESOURCE_OUTPUT}/package/huxerui/huxerui/raw/framework-kept.txt"
        OR UPDATED_FRAMEWORK_HEADER MATCHES "framework_kept_txt"
        OR EXISTS "${BUILTIN_RESOURCE_OUTPUT}/package/huxerui/huxerui/raw/framework-kept.txt"
        OR UPDATED_BUILTIN_HEADER MATCHES "framework_kept_txt")
    message(FATAL_ERROR "Built-in resource generation retained a deleted resource")
endif ()

set(OVERLAP_PROJECT_ROOT "${TEST_ROOT}/overlap-source")
set(OVERLAP_BUILD_ROOT "${TEST_ROOT}/overlap-build")
file(MAKE_DIRECTORY
        "${OVERLAP_PROJECT_ROOT}/resources/raw"
        "${OVERLAP_PROJECT_ROOT}/resources/nested/raw"
)
file(WRITE "${OVERLAP_PROJECT_ROOT}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${OVERLAP_PROJECT_ROOT}/resources/raw/value.txt" "value")
file(WRITE "${OVERLAP_PROJECT_ROOT}/resources/nested/raw/nested.txt" "nested")
file(WRITE "${OVERLAP_PROJECT_ROOT}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(huxerui_overlapping_resource_roots LANGUAGES CXX)\n"
        "set(HUXERUI_HOST_TOOL_ROOT \"${SOURCE_DIRECTORY}/tools/prebuilt\")\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUIResources.cmake\")\n"
        "add_executable(resource_app main.cpp)\n"
        "huxerui_add_resources(resource_app ROOT resources NAMESPACE app)\n"
        "huxerui_add_resources(resource_app ROOT resources/nested NAMESPACE editor)\n"
)
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${OVERLAP_PROJECT_ROOT}"
                -B "${OVERLAP_BUILD_ROOT}"
                -G "${HOST_GENERATOR}"
                -DCMAKE_BUILD_TYPE=Debug
        RESULT_VARIABLE OVERLAP_CONFIGURE_RESULT
        OUTPUT_VARIABLE OVERLAP_CONFIGURE_OUTPUT
        ERROR_VARIABLE OVERLAP_CONFIGURE_ERROR
)
if (OVERLAP_CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Overlapping resource roots configure unexpectedly succeeded")
endif ()
if (NOT "${OVERLAP_CONFIGURE_OUTPUT}${OVERLAP_CONFIGURE_ERROR}"
        MATCHES "ROOT overlaps a root already registered")
    message(FATAL_ERROR
            "Overlapping resource roots configure failed for an unexpected reason:\n"
            "${OVERLAP_CONFIGURE_OUTPUT}${OVERLAP_CONFIGURE_ERROR}"
    )
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
