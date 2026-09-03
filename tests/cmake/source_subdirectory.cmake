foreach (required_variable IN ITEMS SOURCE_DIRECTORY WORK_DIRECTORY HOST_GENERATOR)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/huxerui-source-subdirectory-${TEST_NONCE}")
set(CONSUMER_SOURCE "${TEST_ROOT}/source")
set(CONSUMER_BUILD "${TEST_ROOT}/build")
set(CONSUMER_INSTALL "${TEST_ROOT}/install")
file(MAKE_DIRECTORY "${CONSUMER_SOURCE}")
file(TO_CMAKE_PATH "${SOURCE_DIRECTORY}" SOURCE_DIRECTORY_CMAKE)

file(WRITE "${CONSUMER_SOURCE}/library.cpp"
        "#include <huxerui/huxerui.h>\n"
        "huxerui::View SourceSubdirectoryView() { return huxerui::Text(\"source\"); }\n"
)
set(CONSUMER_CMAKE [=[
cmake_minimum_required(VERSION 3.20)
project(huxerui_source_subdirectory_consumer LANGUAGES CXX)

set(HUXERUI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_CLI ON CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("@SOURCE_DIRECTORY_CMAKE@" huxerui)

if (NOT TARGET HuxerUI::huxerui OR NOT TARGET HuxerUI::huxerui_static)
  message(FATAL_ERROR "Source subdirectory did not create both canonical target names")
endif ()
foreach (framework_target IN ITEMS HuxerUI::huxerui HuxerUI::huxerui_static)
  get_target_property(framework_type ${framework_target} TYPE)
  if (NOT framework_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Source subdirectory created a non-static target while shared output is disabled: ${framework_target}")
  endif ()
endforeach ()
foreach (required_command IN ITEMS huxerui_add_app huxerui_add_library huxerui_add_resources huxerui_enable_codegen)
  if (NOT COMMAND ${required_command})
    message(FATAL_ERROR "Source subdirectory did not load ${required_command}")
  endif ()
endforeach ()

get_target_property(framework_resources HuxerUI::huxerui_static HUXERUI_RESOURCE_PACKAGE)
if (NOT framework_resources OR framework_resources MATCHES "-NOTFOUND$")
  message(FATAL_ERROR "Source target is missing its built-in resource package")
endif ()

huxerui_add_library(source_subdirectory_library SOURCES library.cpp)
get_target_property(consumer_links source_subdirectory_library LINK_LIBRARIES)
list(FIND consumer_links "HuxerUI::huxerui_static" static_link_index)
if (static_link_index EQUAL -1)
  message(FATAL_ERROR "Source library did not select the static framework target")
endif ()

huxerui_add_app(source_subdirectory_app SOURCES library.cpp)
get_target_property(app_install_component source_subdirectory_app HUXERUI_APPLICATION_INSTALL_COMPONENT)
if (NOT app_install_component STREQUAL "HuxerUIApplication")
  message(FATAL_ERROR "Source application target has an invalid install component")
endif ()

get_cmake_property(install_components COMPONENTS)
if ("HuxerUILibraries" IN_LIST install_components)
  message(FATAL_ERROR "Source subdirectory registered SDK installation components")
endif ()
]=])
string(CONFIGURE "${CONSUMER_CMAKE}" CONSUMER_CMAKE @ONLY)
file(WRITE "${CONSUMER_SOURCE}/CMakeLists.txt" "${CONSUMER_CMAKE}")

set(CONFIGURE_COMMAND
        "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${CONSUMER_BUILD}"
        -G "${HOST_GENERATOR}"
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
execute_process(
        COMMAND ${CONFIGURE_COMMAND}
        RESULT_VARIABLE CONFIGURE_RESULT
        OUTPUT_VARIABLE CONFIGURE_OUTPUT
        ERROR_VARIABLE CONFIGURE_ERROR
)
if (NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Source subdirectory consumer configuration failed:\n${CONFIGURE_OUTPUT}${CONFIGURE_ERROR}"
    )
endif ()

foreach (unexpected_file IN ITEMS CPackConfig.cmake CPackSourceConfig.cmake HuxerUIInstallWindowsDebug.cmake)
    if (EXISTS "${CONSUMER_BUILD}/${unexpected_file}")
        message(FATAL_ERROR "Source subdirectory generated SDK packaging file: ${unexpected_file}")
    endif ()
endforeach ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${CONSUMER_BUILD}" --prefix "${CONSUMER_INSTALL}"
        RESULT_VARIABLE INSTALL_RESULT
        OUTPUT_VARIABLE INSTALL_OUTPUT
        ERROR_VARIABLE INSTALL_ERROR
)
if (NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "Source subdirectory installation probe failed:\n${INSTALL_OUTPUT}${INSTALL_ERROR}")
endif ()
file(GLOB_RECURSE INSTALLED_FILES LIST_DIRECTORIES FALSE "${CONSUMER_INSTALL}/*")
if (INSTALLED_FILES)
    message(FATAL_ERROR "Source subdirectory registered unexpected install artifacts: ${INSTALLED_FILES}")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
