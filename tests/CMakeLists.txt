if (TARGET HuxerUI::huxerui_static)
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui_static)
else ()
    set(HUXERUI_TEST_LIBRARY HuxerUI::huxerui)
endif ()

include("${HUXERUI_PROJECT_DIR}/3dparty/catch2/huxerui_3p.cmake")

file(TO_CMAKE_PATH "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}" HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE)
set(HUXERUI_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(huxerui_add_test_suite target_name test_name)
    cmake_parse_arguments(TEST "" "MAIN" "SOURCES;LIBRARIES;LABELS" ${ARGN})
    if (NOT TEST_MAIN)
        set(TEST_MAIN "${HUXERUI_TEST_DIR}/tests_main.cpp")
    endif ()
    add_executable(${target_name} "${TEST_MAIN}" ${TEST_SOURCES})
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_options(${target_name} PRIVATE
            "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>"
    )
    target_include_directories(${target_name} PRIVATE
            "${HUXERUI_PROJECT_DIR}/src"
            "${HUXERUI_TEST_DIR}/support"
    )
    target_link_libraries(${target_name} PRIVATE HuxerUI3p::Catch2 ${TEST_LIBRARIES})
    add_test(NAME ${test_name} COMMAND ${target_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "${TEST_LABELS}")
endfunction()

add_subdirectory(unit)
add_subdirectory(runtime)
add_subdirectory(platform)
add_subdirectory(codegen)
add_subdirectory(resource_compiler)
add_subdirectory(cli)
add_subdirectory(cmake)
add_subdirectory(scripts)
include(support/header_checks.cmake)
