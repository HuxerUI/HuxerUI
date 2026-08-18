cmake_minimum_required(VERSION 3.20)

if (HUXERUI_PROJECT_PLAN_OUTPUT)
    get_filename_component(HUXERUI_PROJECT_PLAN_DIRECTORY
            "${HUXERUI_PROJECT_PLAN_OUTPUT}"
            DIRECTORY
    )
    file(MAKE_DIRECTORY "${HUXERUI_PROJECT_PLAN_DIRECTORY}")
    file(WRITE "${HUXERUI_PROJECT_PLAN_OUTPUT}" [=[
{
  "schema": 1,
  "kind": "app",
  "name": "@PROJECT_NAME@",
  "id": "@PROJECT_ID@",
  "target": "@TARGET_NAME@"
}
]=])
endif ()
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

project(@TARGET_NAME@ VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT TARGET HuxerUI::huxerui)
    set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI SDK or source directory")
    if (HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
            AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
        set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        if (ANDROID)
            set(HUXERUI_BUILD_SHARED ON CACHE BOOL "" FORCE)
            set(HUXERUI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
        endif ()
        add_subdirectory("${HUXERUI_HOME}" "${CMAKE_BINARY_DIR}/huxerui-sdk" EXCLUDE_FROM_ALL)
    else ()
        if (HUXERUI_HOME)
            find_package(HuxerUI CONFIG REQUIRED
                    PATHS "${HUXERUI_HOME}"
                    NO_DEFAULT_PATH
                    NO_CMAKE_FIND_ROOT_PATH
            )
        else ()
            find_package(HuxerUI CONFIG REQUIRED)
        endif ()
    endif ()
endif ()


if (WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/huxerui.cmake" OPTIONAL)
elseif (APPLE AND NOT IOS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/huxerui.cmake" OPTIONAL)
elseif (EMSCRIPTEN)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/web/huxerui.cmake" OPTIONAL)
endif ()

set(HUXERUI_APP_BUNDLE_IDENTIFIER "@PROJECT_ID@")
if (APPLE AND NOT IOS AND HUXERUI_MACOS_BUNDLE_IDENTIFIER)
    set(HUXERUI_APP_BUNDLE_IDENTIFIER "${HUXERUI_MACOS_BUNDLE_IDENTIFIER}")
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (WIN32)
    list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
elseif (APPLE AND NOT IOS)
    list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
endif ()

set(HUXERUI_MODULE_GRAPH_OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/.huxerui/generated/modules.json"
)

huxerui_add_app(@TARGET_NAME@
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        BUNDLE_NAME
            "@PROJECT_NAME@"
        BUNDLE_IDENTIFIER
            "${HUXERUI_APP_BUNDLE_IDENTIFIER}"
)

if (WIN32 AND HUXERUI_WINDOWS_MANIFEST)
    target_sources(@TARGET_NAME@ PRIVATE "${HUXERUI_WINDOWS_MANIFEST}")
endif ()
if (APPLE AND NOT IOS AND HUXERUI_MACOS_INFO_PLIST)
    set_target_properties(@TARGET_NAME@ PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${HUXERUI_MACOS_INFO_PLIST}"
    )
endif ()
if (EMSCRIPTEN AND COMMAND huxerui_configure_web_app)
    huxerui_configure_web_app(@TARGET_NAME@)
endif ()

huxerui_use_module(@TARGET_NAME@
        TARGET @MODULE_PRODUCT_NAME@::@MODULE_PRODUCT_NAME@
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
