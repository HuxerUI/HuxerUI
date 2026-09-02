if (HUXERUI_PROJECT_PLAN_OUTPUT)
    get_filename_component(HUXERUI_PROJECT_PLAN_DIRECTORY
            "${HUXERUI_PROJECT_PLAN_OUTPUT}"
            DIRECTORY
    )
    file(MAKE_DIRECTORY "${HUXERUI_PROJECT_PLAN_DIRECTORY}")
    file(WRITE "${HUXERUI_PROJECT_PLAN_OUTPUT}" [=[
{
  "schema": 1,
  "kind": "library",
  "name": "@PROJECT_NAME@",
  "id": "@PROJECT_ID@",
  "target": "@TARGET_NAME@",
  "namespace": "@LIBRARY_NAMESPACE@",
  "publicTarget": "@LIBRARY_PUBLIC_TARGET@"
}
]=])
endif ()

if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target" FORCE)
endif ()

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_language(CXX)
    if (APPLE AND NOT IOS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src")
        enable_language(OBJCXX)
    endif ()
endif ()

if (NOT TARGET HuxerUI::huxerui)
    set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI SDK or source directory")
    if (HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
            AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
        set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        if (ANDROID)
            set(HUXERUI_BUILD_SHARED ON CACHE BOOL "" FORCE)
            set(HUXERUI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
        else ()
            set(HUXERUI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
            set(HUXERUI_BUILD_STATIC ON CACHE BOOL "" FORCE)
        endif ()
        add_subdirectory("${HUXERUI_HOME}" "${CMAKE_BINARY_DIR}/huxerui-sdk" EXCLUDE_FROM_ALL)
    elseif (HUXERUI_HOME)
        find_package(HuxerUI CONFIG REQUIRED
                PATHS "${HUXERUI_HOME}"
                NO_DEFAULT_PATH
                NO_CMAKE_FIND_ROOT_PATH
        )
    else ()
        find_package(HuxerUI CONFIG REQUIRED)
    endif ()
endif ()
