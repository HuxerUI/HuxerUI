include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/HuxerUIResources.cmake")

set(HUXERUI_BUILD_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(huxerui_configure_compile_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_include_directories(${target_name} PRIVATE
            "${HUXERUI_PUBLIC_INCLUDE_DIR}"
            "${HUXERUI_PROJECT_DIR}/src"
            ${HUXERUI_PLATFORM_INCLUDE_DIRECTORIES}
    )
    target_compile_options(${target_name} PRIVATE
            "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
            "$<$<CXX_COMPILER_ID:MSVC>:/permissive->"
            "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>"
            ${HUXERUI_PLATFORM_COMPILE_OPTIONS}
    )
    target_compile_definitions(${target_name} PRIVATE
            ${HUXERUI_PLATFORM_COMPILE_DEFINITIONS}
    )
endfunction()

function(huxerui_configure_public_target target_name)
    set_property(TARGET ${target_name} PROPERTY
            HUXERUI_PLATFORM_ID "${HUXERUI_PLATFORM_ID}"
    )
    set_property(TARGET ${target_name} PROPERTY
            HUXERUI_BUILTIN_RESOURCE_PACKAGE "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}"
    )
    target_include_directories(${target_name}
            PUBLIC
            $<BUILD_INTERFACE:${HUXERUI_PUBLIC_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_options(${target_name} INTERFACE ${HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS})
    if (HUXERUI_PLATFORM_ID STREQUAL "linux")
        foreach (HUXERUI_PLATFORM_LINK_LIBRARY IN LISTS HUXERUI_PLATFORM_LINK_LIBRARIES)
            target_link_libraries(${target_name} PRIVATE "$<BUILD_INTERFACE:${HUXERUI_PLATFORM_LINK_LIBRARY}>")
        endforeach ()
        get_target_property(HUXERUI_TARGET_TYPE ${target_name} TYPE)
        if (HUXERUI_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
            target_link_libraries(${target_name} PRIVATE "$<INSTALL_INTERFACE:HuxerUI::linux_dependencies>")
        endif ()
    else ()
        target_link_libraries(${target_name} PRIVATE ${HUXERUI_PLATFORM_LINK_LIBRARIES})
    endif ()
    target_link_options(${target_name} INTERFACE ${HUXERUI_PLATFORM_LINK_OPTIONS})
    if (EMSCRIPTEN)
        set_property(TARGET ${target_name} APPEND PROPERTY
                INTERFACE_LINK_DEPENDS
                "$<BUILD_INTERFACE:${HUXERUI_PROJECT_DIR}/platform/web/web_file.js>"
        )
    endif ()

    if (WIN32)
        get_target_property(HUXERUI_TARGET_TYPE ${target_name} TYPE)
        if (HUXERUI_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
            set_target_properties(${target_name} PROPERTIES
                    WINDOWS_EXPORT_ALL_SYMBOLS ON
            )
        endif ()
    endif ()
endfunction()

function(_huxerui_configure_builtin_resources target_name)
    set(HUXERUI_BUILTIN_RESOURCE_OUTPUT
            "${PROJECT_BINARY_DIR}/huxerui-builtin-resources"
    )
    set(HUXERUI_BUILTIN_RESOURCE_PACKAGE
            "${HUXERUI_BUILTIN_RESOURCE_OUTPUT}/package"
    )
    set(HUXERUI_BUILTIN_RESOURCE_HEADER
            "${HUXERUI_BUILTIN_RESOURCE_OUTPUT}/include/huxerui_builtin_resources.h"
    )
    set(HUXERUI_BUILTIN_RESOURCE_INDEX
            "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}/huxerui/resources.bin"
    )
    file(GLOB_RECURSE HUXERUI_BUILTIN_RESOURCE_INPUTS
            CONFIGURE_DEPENDS
            LIST_DIRECTORIES FALSE
            "${HUXERUI_PROJECT_DIR}/resources/*"
    )
    set(HUXERUI_BUILTIN_RESOURCE_PLAN
            "${HUXERUI_BUILTIN_RESOURCE_OUTPUT}/resource-plan-$<CONFIG>.txt"
    )
    file(GENERATE
            OUTPUT "${HUXERUI_BUILTIN_RESOURCE_PLAN}"
            CONTENT "inputs=${HUXERUI_BUILTIN_RESOURCE_INPUTS}\n"
    )
    huxerui_resolve_host_tool("hrc" HUXERUI_BUILTIN_RESOURCE_COMPILER_COMMAND)
    add_custom_command(
            OUTPUT
                    "${HUXERUI_BUILTIN_RESOURCE_HEADER}"
                    "${HUXERUI_BUILTIN_RESOURCE_INDEX}"
            COMMAND "${HUXERUI_BUILTIN_RESOURCE_COMPILER_COMMAND}"
                    --root "${HUXERUI_PROJECT_DIR}/resources"
                    --output "${HUXERUI_BUILTIN_RESOURCE_OUTPUT}"
                    --namespace huxerui
                    --header-name huxerui_builtin_resources.h
            DEPENDS
                    ${HUXERUI_BUILTIN_RESOURCE_INPUTS}
                    "${HUXERUI_BUILTIN_RESOURCE_PLAN}"
                    "${HUXERUI_BUILTIN_RESOURCE_COMPILER_COMMAND}"
            COMMENT "Generating HuxerUI built-in resources"
            VERBATIM
    )
    set_source_files_properties(
            "${HUXERUI_BUILTIN_RESOURCE_HEADER}"
            PROPERTIES
                    GENERATED TRUE
                    HEADER_FILE_ONLY TRUE
    )
    target_sources(${target_name} PRIVATE
            "${HUXERUI_BUILTIN_RESOURCE_HEADER}"
    )
    target_include_directories(${target_name} PRIVATE
            "${HUXERUI_BUILTIN_RESOURCE_OUTPUT}/include"
    )
    set(HUXERUI_BUILTIN_RESOURCE_PACKAGE
            "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}"
            PARENT_SCOPE
    )
endfunction()

function(huxerui_configure_targets)
    if (HUXERUI_LIBRARY_GRAPH_ONLY)
        add_library(huxerui_library_graph_framework INTERFACE)
        set_property(TARGET huxerui_library_graph_framework PROPERTY
                HUXERUI_PLATFORM_ID "generic"
        )
        target_include_directories(huxerui_library_graph_framework INTERFACE
                $<BUILD_INTERFACE:${HUXERUI_PUBLIC_INCLUDE_DIR}>
                $<INSTALL_INTERFACE:include>
        )
        add_library(HuxerUI::huxerui ALIAS huxerui_library_graph_framework)
        add_library(HuxerUI::huxerui_static ALIAS huxerui_library_graph_framework)
        set(HUXERUI_PLATFORM_ID "generic" PARENT_SCOPE)
        return()
    endif ()
    if (NOT HUXERUI_BUILD_SHARED AND NOT HUXERUI_BUILD_STATIC)
        message(FATAL_ERROR "At least one HuxerUI library target must be enabled")
    endif ()
    if ((EMSCRIPTEN OR IOS) AND HUXERUI_BUILD_SHARED)
        message(FATAL_ERROR "HuxerUI Web and iOS support the static library target only")
    endif ()

    set(HUXERUI_PLATFORM_ID "generic")
    set(HUXERUI_PLATFORM_SOURCE_FILES)
    set(HUXERUI_PLATFORM_COMPILE_OPTIONS)
    set(HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS)
    set(HUXERUI_PLATFORM_COMPILE_DEFINITIONS)
    set(HUXERUI_PLATFORM_LINK_LIBRARIES)
    set(HUXERUI_PLATFORM_INCLUDE_DIRECTORIES)
    set(HUXERUI_PLATFORM_LINK_OPTIONS)

    if (EMSCRIPTEN)
        set(HUXERUI_PLATFORM_ID "web")
        include("${HUXERUI_BUILD_CMAKE_DIR}/platform/Web.cmake")
    elseif (ANDROID)
        set(HUXERUI_PLATFORM_ID "android")
        include("${HUXERUI_BUILD_CMAKE_DIR}/platform/Android.cmake")
    elseif (IOS)
        set(HUXERUI_PLATFORM_ID "ios")
        include("${HUXERUI_BUILD_CMAKE_DIR}/platform/IOS.cmake")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(HUXERUI_PLATFORM_ID "macos")
        include("${HUXERUI_BUILD_CMAKE_DIR}/platform/MacOS.cmake")
    elseif (WIN32)
        set(HUXERUI_PLATFORM_ID "windows")
        include("${HUXERUI_BUILD_CMAKE_DIR}/platform/Windows.cmake")
    elseif (UNIX AND NOT APPLE)
        set(HUXERUI_PLATFORM_ID "linux")
        include("${HUXERUI_BUILD_CMAKE_DIR}/platform/Linux.cmake")
    else ()
        message(FATAL_ERROR "HuxerUI currently supports Android, iOS, macOS, Windows, Linux, and Web only")
    endif ()

    file(GLOB HUXERUI_CORE_SOURCE_FILES CONFIGURE_DEPENDS
            "${HUXERUI_PROJECT_DIR}/src/*.cpp"
    )

    if (IOS)
        set(HUXERUI_LIBRARY_SOURCE_FILES
                ${HUXERUI_CORE_SOURCE_FILES}
                ${HUXERUI_PLATFORM_SOURCE_FILES}
        )
    else ()
        add_library(huxerui_core_objects OBJECT
                ${HUXERUI_CORE_SOURCE_FILES}
                ${HUXERUI_PLATFORM_SOURCE_FILES}
        )
        set_target_properties(huxerui_core_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
        huxerui_configure_compile_target(huxerui_core_objects)
        _huxerui_configure_builtin_resources(huxerui_core_objects)
        set(HUXERUI_LIBRARY_SOURCE_FILES $<TARGET_OBJECTS:huxerui_core_objects>)
    endif ()

    if (HUXERUI_BUILD_SHARED)
        add_library(${HUXERUI_SHARED_LIB_NAME} SHARED
                ${HUXERUI_LIBRARY_SOURCE_FILES}
        )
        if (WIN32)
            set_target_properties(${HUXERUI_SHARED_LIB_NAME} PROPERTIES DEBUG_POSTFIX "_debug")
        endif ()
        huxerui_configure_public_target(${HUXERUI_SHARED_LIB_NAME})
        add_library(HuxerUI::huxerui ALIAS ${HUXERUI_SHARED_LIB_NAME})
    endif ()

    if (HUXERUI_BUILD_STATIC)
        add_library(${HUXERUI_STATIC_LIB_NAME} STATIC
                ${HUXERUI_LIBRARY_SOURCE_FILES}
        )
        if (IOS)
            set_target_properties(${HUXERUI_STATIC_LIB_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
            huxerui_configure_compile_target(${HUXERUI_STATIC_LIB_NAME})
            _huxerui_configure_builtin_resources(${HUXERUI_STATIC_LIB_NAME})
        endif ()
        if (WIN32)
            set_target_properties(${HUXERUI_STATIC_LIB_NAME} PROPERTIES DEBUG_POSTFIX "_debug")
        endif ()
        huxerui_configure_public_target(${HUXERUI_STATIC_LIB_NAME})
        add_library(HuxerUI::huxerui_static ALIAS ${HUXERUI_STATIC_LIB_NAME})
        if (NOT TARGET HuxerUI::huxerui)
            add_library(HuxerUI::huxerui ALIAS ${HUXERUI_STATIC_LIB_NAME})
        endif ()
    endif ()

    set(HUXERUI_PLATFORM_ID "${HUXERUI_PLATFORM_ID}" PARENT_SCOPE)
    set(HUXERUI_LINUX_FCITX5_ENABLED "${HUXERUI_LINUX_FCITX5_ENABLED}" PARENT_SCOPE)
    set(HUXERUI_LINUX_STATIC_ARCHIVES "${HUXERUI_LINUX_STATIC_ARCHIVES}" PARENT_SCOPE)
    set(HUXERUI_LINUX_STATIC_DEPENDENCY_FILES "${HUXERUI_LINUX_STATIC_DEPENDENCY_FILES}" PARENT_SCOPE)
    set(HUXERUI_BUILTIN_RESOURCE_PACKAGE "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}" PARENT_SCOPE)
endfunction()
