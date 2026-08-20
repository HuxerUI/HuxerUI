set(HUXERUI_TARGETS_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(huxerui_platform_configure)
endfunction()

function(huxerui_configure_platform)
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
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Web.cmake")
    elseif (ANDROID)
        set(HUXERUI_PLATFORM_ID "android")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Android.cmake")
    elseif (IOS)
        set(HUXERUI_PLATFORM_ID "ios")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/IOS.cmake")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(HUXERUI_PLATFORM_ID "macos")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/MacOS.cmake")
    elseif (WIN32)
        set(HUXERUI_PLATFORM_ID "windows")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Windows.cmake")
    elseif (UNIX AND NOT APPLE)
        set(HUXERUI_PLATFORM_ID "linux")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Linux.cmake")
    else ()
        message(FATAL_ERROR "HuxerUI currently supports Android, iOS, macOS, Windows, Linux, and Web only")
    endif ()

    huxerui_platform_configure()

    set(HUXERUI_PLATFORM_ID "${HUXERUI_PLATFORM_ID}" PARENT_SCOPE)
    set(HUXERUI_PLATFORM_SOURCE_FILES ${HUXERUI_PLATFORM_SOURCE_FILES} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_COMPILE_OPTIONS ${HUXERUI_PLATFORM_COMPILE_OPTIONS} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS ${HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_COMPILE_DEFINITIONS ${HUXERUI_PLATFORM_COMPILE_DEFINITIONS} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_LINK_LIBRARIES ${HUXERUI_PLATFORM_LINK_LIBRARIES} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_INCLUDE_DIRECTORIES ${HUXERUI_PLATFORM_INCLUDE_DIRECTORIES} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_LINK_OPTIONS ${HUXERUI_PLATFORM_LINK_OPTIONS} PARENT_SCOPE)
endfunction()

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
    target_include_directories(${target_name}
            PUBLIC
            $<BUILD_INTERFACE:${HUXERUI_PUBLIC_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_options(${target_name} INTERFACE ${HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS})
    target_link_libraries(${target_name} PRIVATE ${HUXERUI_PLATFORM_LINK_LIBRARIES})
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
    huxerui_resolve_host_tool("hapt" HUXERUI_BUILTIN_RESOURCE_CODEGEN_COMMAND)
    add_custom_command(
            OUTPUT
                    "${HUXERUI_BUILTIN_RESOURCE_HEADER}"
                    "${HUXERUI_BUILTIN_RESOURCE_INDEX}"
            COMMAND "${HUXERUI_BUILTIN_RESOURCE_CODEGEN_COMMAND}"
                    --root "${HUXERUI_PROJECT_DIR}/resources"
                    --output "${HUXERUI_BUILTIN_RESOURCE_OUTPUT}"
                    --namespace huxerui
                    --header-name huxerui_builtin_resources.h
            DEPENDS
                    ${HUXERUI_BUILTIN_RESOURCE_INPUTS}
                    "${HUXERUI_BUILTIN_RESOURCE_PLAN}"
                    "${HUXERUI_BUILTIN_RESOURCE_CODEGEN_COMMAND}"
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
    if (NOT HUXERUI_BUILD_SHARED AND NOT HUXERUI_BUILD_STATIC)
        message(FATAL_ERROR "At least one HuxerUI library target must be enabled")
    endif ()
    if ((EMSCRIPTEN OR IOS) AND HUXERUI_BUILD_SHARED)
        message(FATAL_ERROR "HuxerUI Web and iOS support the static library target only")
    endif ()

    huxerui_configure_platform()

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
        huxerui_configure_public_target(${HUXERUI_STATIC_LIB_NAME})
        add_library(HuxerUI::huxerui_static ALIAS ${HUXERUI_STATIC_LIB_NAME})
        if (NOT TARGET HuxerUI::huxerui)
            add_library(HuxerUI::huxerui ALIAS ${HUXERUI_STATIC_LIB_NAME})
        endif ()
    endif ()

    set(HUXERUI_PLATFORM_ID "${HUXERUI_PLATFORM_ID}" PARENT_SCOPE)
    set(HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS ${HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_LINK_LIBRARIES ${HUXERUI_PLATFORM_LINK_LIBRARIES} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_LINK_OPTIONS ${HUXERUI_PLATFORM_LINK_OPTIONS} PARENT_SCOPE)
    set(HUXERUI_BUILTIN_RESOURCE_PACKAGE "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}" PARENT_SCOPE)
endfunction()

function(huxerui_resolve_host_tool tool_name output_variable)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" HUXERUI_HOST_SYSTEM)
    if (HUXERUI_HOST_SYSTEM STREQUAL "darwin")
        set(HUXERUI_HOST_SYSTEM "macos")
    elseif (NOT HUXERUI_HOST_SYSTEM STREQUAL "windows"
            AND NOT HUXERUI_HOST_SYSTEM STREQUAL "linux")
        message(FATAL_ERROR
                "HuxerUI host tools do not support ${CMAKE_HOST_SYSTEM_NAME}"
        )
    endif ()

    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" HUXERUI_HOST_ARCHITECTURE)
    if (HUXERUI_HOST_ARCHITECTURE MATCHES "^(amd64|x64|x86_64)$")
        set(HUXERUI_HOST_ARCHITECTURE "x86_64")
    elseif (HUXERUI_HOST_ARCHITECTURE MATCHES "^(aarch64|arm64)$")
        set(HUXERUI_HOST_ARCHITECTURE "arm64")
    else ()
        message(FATAL_ERROR
                "HuxerUI host tools do not support ${CMAKE_HOST_SYSTEM_PROCESSOR}"
        )
    endif ()

    set(HUXERUI_HOST_TOOL_SUFFIX)
    if (HUXERUI_HOST_SYSTEM STREQUAL "windows")
        set(HUXERUI_HOST_TOOL_SUFFIX ".exe")
    endif ()
    if (HUXERUI_HOST_TOOL_ROOT)
        set(HUXERUI_RESOLVED_HOST_TOOL_ROOT "${HUXERUI_HOST_TOOL_ROOT}")
    else ()
        # Source builds resolve tools beside this module; an installed package supplies HUXERUI_HOST_TOOL_ROOT.
        get_filename_component(HUXERUI_RESOLVED_HOST_TOOL_ROOT
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/prebuilt"
                ABSOLUTE
        )
    endif ()
    set(HUXERUI_HOST_TOOL_DIRECTORY
            "${HUXERUI_RESOLVED_HOST_TOOL_ROOT}/${HUXERUI_HOST_SYSTEM}/${HUXERUI_HOST_ARCHITECTURE}"
    )
    set(HUXERUI_HOST_TOOL
            "${HUXERUI_HOST_TOOL_DIRECTORY}/${tool_name}${HUXERUI_HOST_TOOL_SUFFIX}"
    )
    if (NOT EXISTS "${HUXERUI_HOST_TOOL}")
        message(FATAL_ERROR
                "HuxerUI host tool is missing: ${HUXERUI_HOST_TOOL}"
        )
    endif ()
    set(${output_variable} "${HUXERUI_HOST_TOOL}" PARENT_SCOPE)
endfunction()

function(huxerui_enable_codegen target_name)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_enable_codegen() target does not exist: ${target_name}"
        )
    endif ()

    huxerui_resolve_host_tool("hcg" HUXERUI_CODEGEN_COMMAND)

    get_target_property(HUXERUI_CODEGEN_ALREADY_ENABLED
            ${target_name}
            HUXERUI_CODEGEN_ENABLED
    )
    if (HUXERUI_CODEGEN_ALREADY_ENABLED)
        return()
    endif ()

    get_target_property(HUXERUI_CODEGEN_TARGET_TYPE ${target_name} TYPE)
    if (HUXERUI_CODEGEN_TARGET_TYPE STREQUAL "INTERFACE_LIBRARY"
            OR HUXERUI_CODEGEN_TARGET_TYPE STREQUAL "UTILITY")
        message(FATAL_ERROR
                "huxerui_enable_codegen() requires a compilable target: ${target_name}"
        )
    endif ()

    get_target_property(HUXERUI_CODEGEN_SOURCE_DIR ${target_name} SOURCE_DIR)
    get_target_property(HUXERUI_CODEGEN_BINARY_DIR ${target_name} BINARY_DIR)
    get_target_property(HUXERUI_CODEGEN_SOURCES ${target_name} SOURCES)

    if (NOT HUXERUI_CODEGEN_SOURCES)
        message(FATAL_ERROR
                "huxerui_enable_codegen() target has no sources: ${target_name}"
        )
    endif ()

    set(HUXERUI_CODEGEN_REWRITTEN_SOURCES)
    set(HUXERUI_CODEGEN_GENERATED_SOURCES)

    foreach (HUXERUI_CODEGEN_SOURCE IN LISTS HUXERUI_CODEGEN_SOURCES)
        if (HUXERUI_CODEGEN_SOURCE MATCHES "^\\$<")
            list(APPEND HUXERUI_CODEGEN_REWRITTEN_SOURCES
                    "${HUXERUI_CODEGEN_SOURCE}"
            )
            continue()
        endif ()

        get_filename_component(HUXERUI_CODEGEN_EXTENSION
                "${HUXERUI_CODEGEN_SOURCE}"
                EXT
        )
        string(TOLOWER
                "${HUXERUI_CODEGEN_EXTENSION}"
                HUXERUI_CODEGEN_EXTENSION
        )
        if (NOT HUXERUI_CODEGEN_EXTENSION STREQUAL ".cpp"
                AND NOT HUXERUI_CODEGEN_EXTENSION STREQUAL ".cc"
                AND NOT HUXERUI_CODEGEN_EXTENSION STREQUAL ".cxx")
            list(APPEND HUXERUI_CODEGEN_REWRITTEN_SOURCES
                    "${HUXERUI_CODEGEN_SOURCE}"
            )
            continue()
        endif ()

        get_filename_component(HUXERUI_CODEGEN_ABSOLUTE_SOURCE
                "${HUXERUI_CODEGEN_SOURCE}"
                ABSOLUTE
                BASE_DIR "${HUXERUI_CODEGEN_SOURCE_DIR}"
        )
        if (NOT EXISTS "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}")
            list(APPEND HUXERUI_CODEGEN_REWRITTEN_SOURCES
                    "${HUXERUI_CODEGEN_SOURCE}"
            )
            continue()
        endif ()

        set_property(DIRECTORY "${HUXERUI_CODEGEN_SOURCE_DIR}"
                APPEND
                PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}"
        )
        file(READ
                "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}"
                HUXERUI_CODEGEN_SOURCE_CONTENT
        )
        string(FIND
                "${HUXERUI_CODEGEN_SOURCE_CONTENT}"
                "[[huxerui::scope]]"
                HUXERUI_CODEGEN_MARKER_INDEX
        )
        if (HUXERUI_CODEGEN_MARKER_INDEX EQUAL -1)
            list(APPEND HUXERUI_CODEGEN_REWRITTEN_SOURCES
                    "${HUXERUI_CODEGEN_SOURCE}"
            )
            continue()
        endif ()

        string(SHA256
                HUXERUI_CODEGEN_SOURCE_HASH
                "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}"
        )
        string(SUBSTRING
                "${HUXERUI_CODEGEN_SOURCE_HASH}"
                0
                16
                HUXERUI_CODEGEN_SOURCE_HASH
        )
        get_filename_component(HUXERUI_CODEGEN_SOURCE_NAME
                "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}"
                NAME
        )
        get_filename_component(HUXERUI_CODEGEN_SOURCE_DIRECTORY
                "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}"
                DIRECTORY
        )

        set(HUXERUI_CODEGEN_OUTPUT_DIRECTORY
                "${HUXERUI_CODEGEN_BINARY_DIR}/hcg/${target_name}/${HUXERUI_CODEGEN_SOURCE_HASH}"
        )
        set(HUXERUI_CODEGEN_OUTPUT
                "${HUXERUI_CODEGEN_OUTPUT_DIRECTORY}/${HUXERUI_CODEGEN_SOURCE_NAME}"
        )
        add_custom_command(
                OUTPUT "${HUXERUI_CODEGEN_OUTPUT}"
                COMMAND "${HUXERUI_CODEGEN_COMMAND}"
                        --input "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}"
                        --output "${HUXERUI_CODEGEN_OUTPUT}"
                DEPENDS
                        "${HUXERUI_CODEGEN_ABSOLUTE_SOURCE}"
                        "${HUXERUI_CODEGEN_COMMAND}"
                COMMENT
                        "Generating HuxerUI scope source ${HUXERUI_CODEGEN_SOURCE_NAME}"
                VERBATIM
        )

        set_source_files_properties(
                "${HUXERUI_CODEGEN_OUTPUT}"
                PROPERTIES GENERATED TRUE
        )
        target_include_directories(${target_name}
                PRIVATE "${HUXERUI_CODEGEN_SOURCE_DIRECTORY}"
        )
        list(APPEND HUXERUI_CODEGEN_REWRITTEN_SOURCES
                "${HUXERUI_CODEGEN_OUTPUT}"
        )
        list(APPEND HUXERUI_CODEGEN_GENERATED_SOURCES
                "${HUXERUI_CODEGEN_OUTPUT}"
        )
    endforeach ()

    set_property(TARGET ${target_name}
            PROPERTY SOURCES ${HUXERUI_CODEGEN_REWRITTEN_SOURCES}
    )
    set_property(TARGET ${target_name}
            PROPERTY HUXERUI_CODEGEN_ENABLED TRUE
    )
    set_property(TARGET ${target_name}
            PROPERTY HUXERUI_CODEGEN_GENERATED_SOURCES
                     "${HUXERUI_CODEGEN_GENERATED_SOURCES}"
    )

    target_compile_options(${target_name} PRIVATE
            "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-Wno-unknown-attributes>"
            "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-attributes>"
            "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/wd5030>"
    )
endfunction()

function(_huxerui_get_resource_output target_name output_variable)
    get_property(HUXERUI_RESOURCE_OUTPUT_DIRECTORY_SET
            TARGET ${target_name}
            PROPERTY HUXERUI_RESOURCE_OUTPUT_DIRECTORY
            SET
    )
    if (HUXERUI_RESOURCE_OUTPUT_DIRECTORY_SET)
        get_property(HUXERUI_RESOURCE_OUTPUT_DIRECTORY
                TARGET ${target_name}
                PROPERTY HUXERUI_RESOURCE_OUTPUT_DIRECTORY
        )
    else ()
        get_target_property(HUXERUI_RESOURCE_TARGET_BINARY_DIR
                ${target_name}
                BINARY_DIR
        )
        set(HUXERUI_RESOURCE_OUTPUT_DIRECTORY
                "${HUXERUI_RESOURCE_TARGET_BINARY_DIR}/huxerui-resources/${target_name}"
        )
    endif ()
    set(${output_variable} "${HUXERUI_RESOURCE_OUTPUT_DIRECTORY}" PARENT_SCOPE)
endfunction()

function(_huxerui_configure_resources target_name)
    get_property(HUXERUI_RESOURCE_PACKAGES
            TARGET ${target_name}
            PROPERTY HUXERUI_RESOURCE_PACKAGES
    )
    get_property(HUXERUI_RESOURCE_ROOTS
            TARGET ${target_name}
            PROPERTY HUXERUI_RESOURCE_ROOTS
    )
    get_property(HUXERUI_RESOURCE_NAMESPACES
            TARGET ${target_name}
            PROPERTY HUXERUI_RESOURCE_NAMESPACES
    )
    if (NOT HUXERUI_RESOURCE_PACKAGES AND NOT HUXERUI_RESOURCE_ROOTS)
        return()
    endif ()

    set(HUXERUI_RESOURCE_PACKAGE_INDEXES)
    foreach (HUXERUI_RESOURCE_PACKAGE IN LISTS HUXERUI_RESOURCE_PACKAGES)
        list(APPEND HUXERUI_RESOURCE_PACKAGE_INDEXES
                "${HUXERUI_RESOURCE_PACKAGE}/huxerui/resources.bin"
        )
    endforeach ()

    set(HUXERUI_RESOURCE_INPUTS)
    foreach (HUXERUI_RESOURCE_ROOT IN LISTS HUXERUI_RESOURCE_ROOTS)
        file(GLOB_RECURSE HUXERUI_RESOURCE_ROOT_INPUTS
                CONFIGURE_DEPENDS
                LIST_DIRECTORIES FALSE
                "${HUXERUI_RESOURCE_ROOT}/*"
        )
        list(APPEND HUXERUI_RESOURCE_INPUTS ${HUXERUI_RESOURCE_ROOT_INPUTS})
    endforeach ()
    list(SORT HUXERUI_RESOURCE_INPUTS)

    _huxerui_get_resource_output(${target_name} HUXERUI_RESOURCE_OUTPUT)
    set(HUXERUI_RESOURCE_INDEX
            "${HUXERUI_RESOURCE_OUTPUT}/package/huxerui/resources.bin"
    )
    set(HUXERUI_RESOURCE_PLAN
            "${HUXERUI_RESOURCE_OUTPUT}/resource-plan-$<CONFIG>.txt"
    )
    set(HUXERUI_RESOURCE_DRIVER
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/HuxerUIResources.cmake"
    )
    string(CONCAT HUXERUI_RESOURCE_PLAN_CONTENT
            "packages=${HUXERUI_RESOURCE_PACKAGES}\n"
            "roots=${HUXERUI_RESOURCE_ROOTS}\n"
            "namespaces=${HUXERUI_RESOURCE_NAMESPACES}\n"
            "inputs=${HUXERUI_RESOURCE_INPUTS}\n"
    )
    file(GENERATE
            OUTPUT "${HUXERUI_RESOURCE_PLAN}"
            CONTENT "${HUXERUI_RESOURCE_PLAN_CONTENT}"
    )
    huxerui_resolve_host_tool("hapt" HUXERUI_RESOURCE_CODEGEN_COMMAND)
    add_custom_command(
            OUTPUT "${HUXERUI_RESOURCE_INDEX}"
            COMMAND ${CMAKE_COMMAND}
                    "-DHUXERUI_HAPT=${HUXERUI_RESOURCE_CODEGEN_COMMAND}"
                    "-DHUXERUI_RESOURCE_PACKAGES=${HUXERUI_RESOURCE_PACKAGES}"
                    "-DHUXERUI_RESOURCE_ROOTS=${HUXERUI_RESOURCE_ROOTS}"
                    "-DHUXERUI_RESOURCE_NAMESPACES=${HUXERUI_RESOURCE_NAMESPACES}"
                    "-DHUXERUI_RESOURCE_OUTPUT=${HUXERUI_RESOURCE_OUTPUT}"
                    -P "${HUXERUI_RESOURCE_DRIVER}"
            DEPENDS
                    ${HUXERUI_RESOURCE_PACKAGE_INDEXES}
                    ${HUXERUI_RESOURCE_INPUTS}
                    "${HUXERUI_RESOURCE_PLAN}"
                    "${HUXERUI_RESOURCE_CODEGEN_COMMAND}"
                    "${HUXERUI_RESOURCE_DRIVER}"
            COMMENT "Generating HuxerUI resources for ${target_name}"
            VERBATIM
    )
    target_include_directories(${target_name} PRIVATE
            "${HUXERUI_RESOURCE_OUTPUT}/include"
    )

    set(HUXERUI_RESOURCE_TARGET_OUTPUT "${HUXERUI_RESOURCE_INDEX}")
    set(HUXERUI_RESOURCE_STAGE_DIRECTORY)
    # Gradle stages Android packages after all ABI builds; CMake stages desktop targets with one output package.
    if (EMSCRIPTEN)
        target_link_options(${target_name} PRIVATE
                "SHELL:--preload-file \"${HUXERUI_RESOURCE_OUTPUT}/package@/\""
        )
        set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS
                "${HUXERUI_RESOURCE_INDEX}"
        )
    elseif (APPLE AND NOT IOS)
        get_target_property(HUXERUI_RESOURCE_TARGET_IS_BUNDLE
                ${target_name}
                MACOSX_BUNDLE
        )
        if (HUXERUI_RESOURCE_TARGET_IS_BUNDLE)
            set(HUXERUI_RESOURCE_STAGE_DIRECTORY
                    "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Resources/HuxerUI"
            )
        endif ()
    elseif (WIN32)
        set(HUXERUI_RESOURCE_STAGE_DIRECTORY
                "$<TARGET_FILE_DIR:${target_name}>/$<TARGET_FILE_BASE_NAME:${target_name}>.resources"
        )
    elseif (UNIX AND NOT APPLE)
        set(HUXERUI_RESOURCE_STAGE_DIRECTORY
                "$<TARGET_FILE_DIR:${target_name}>/$<TARGET_FILE_BASE_NAME:${target_name}>.resources"
        )
    endif ()

    if (HUXERUI_RESOURCE_STAGE_DIRECTORY)
        set(HUXERUI_RESOURCE_STAGE_STAMP
                "${HUXERUI_RESOURCE_OUTPUT}/stage-$<CONFIG>.stamp"
        )
        add_custom_command(
                OUTPUT "${HUXERUI_RESOURCE_STAGE_STAMP}"
                COMMAND ${CMAKE_COMMAND} -E rm -f
                        "${HUXERUI_RESOURCE_STAGE_STAMP}"
                COMMAND ${CMAKE_COMMAND} -E remove_directory
                        "${HUXERUI_RESOURCE_STAGE_DIRECTORY}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${HUXERUI_RESOURCE_STAGE_DIRECTORY}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${HUXERUI_RESOURCE_OUTPUT}/package"
                        "${HUXERUI_RESOURCE_STAGE_DIRECTORY}"
                COMMAND ${CMAKE_COMMAND} -E touch
                        "${HUXERUI_RESOURCE_STAGE_STAMP}"
                DEPENDS "${HUXERUI_RESOURCE_INDEX}"
                COMMENT "Staging HuxerUI resources for ${target_name}"
                VERBATIM
        )
        set(HUXERUI_RESOURCE_TARGET_OUTPUT
                "${HUXERUI_RESOURCE_STAGE_STAMP}"
        )
    endif ()

    set_source_files_properties(
            "${HUXERUI_RESOURCE_TARGET_OUTPUT}"
            PROPERTIES
                    GENERATED TRUE
                    HEADER_FILE_ONLY TRUE
    )
    target_sources(${target_name} PRIVATE
            "${HUXERUI_RESOURCE_TARGET_OUTPUT}"
    )
endfunction()

function(_huxerui_configure_scheduled_resources)
    get_property(HUXERUI_RESOURCE_TARGETS
            DIRECTORY
            PROPERTY HUXERUI_RESOURCE_TARGETS
    )
    foreach (HUXERUI_RESOURCE_TARGET IN LISTS HUXERUI_RESOURCE_TARGETS)
        _huxerui_configure_resources(${HUXERUI_RESOURCE_TARGET})
    endforeach ()
endfunction()

function(_huxerui_schedule_resources target_name)
    get_property(HUXERUI_RESOURCES_ALREADY_SCHEDULED
            TARGET ${target_name}
            PROPERTY HUXERUI_RESOURCES_SCHEDULED
    )
    if (HUXERUI_RESOURCES_ALREADY_SCHEDULED)
        return()
    endif ()

    get_target_property(HUXERUI_RESOURCE_TARGET_SOURCE_DIR ${target_name} SOURCE_DIR)
    set_property(TARGET ${target_name} PROPERTY HUXERUI_RESOURCES_SCHEDULED TRUE)
    set_property(DIRECTORY "${HUXERUI_RESOURCE_TARGET_SOURCE_DIR}" APPEND PROPERTY
            HUXERUI_RESOURCE_TARGETS
            "${target_name}"
    )

    get_property(HUXERUI_RESOURCE_CALLBACK_SCHEDULED
            DIRECTORY "${HUXERUI_RESOURCE_TARGET_SOURCE_DIR}"
            PROPERTY HUXERUI_RESOURCE_CALLBACK_SCHEDULED
    )
    if (NOT HUXERUI_RESOURCE_CALLBACK_SCHEDULED)
        set_property(DIRECTORY "${HUXERUI_RESOURCE_TARGET_SOURCE_DIR}" PROPERTY
                HUXERUI_RESOURCE_CALLBACK_SCHEDULED TRUE
        )
        cmake_language(DEFER
                DIRECTORY "${HUXERUI_RESOURCE_TARGET_SOURCE_DIR}"
                CALL _huxerui_configure_scheduled_resources
        )
    endif ()
endfunction()

function(huxerui_add_resources target_name)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_add_resources() target does not exist: ${target_name}"
        )
    endif ()

    cmake_parse_arguments(HUXERUI_RESOURCES
            ""
            "ROOT;NAMESPACE"
            ""
            ${ARGN}
    )
    if (NOT HUXERUI_RESOURCES_ROOT OR NOT HUXERUI_RESOURCES_NAMESPACE)
        message(FATAL_ERROR
                "huxerui_add_resources() requires ROOT and NAMESPACE"
        )
    endif ()
    if (NOT HUXERUI_RESOURCES_NAMESPACE MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
                "huxerui_add_resources() NAMESPACE must be a C++ identifier"
        )
    endif ()

    file(REAL_PATH
            "${HUXERUI_RESOURCES_ROOT}"
            HUXERUI_RESOURCE_ROOT
            BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if (NOT IS_DIRECTORY "${HUXERUI_RESOURCE_ROOT}")
        message(FATAL_ERROR
                "huxerui_add_resources() ROOT is not a directory: ${HUXERUI_RESOURCE_ROOT}"
        )
    endif ()

    get_property(HUXERUI_RESOURCE_ROOTS
            TARGET ${target_name}
            PROPERTY HUXERUI_RESOURCE_ROOTS
    )
    foreach (HUXERUI_EXISTING_RESOURCE_ROOT IN LISTS HUXERUI_RESOURCE_ROOTS)
        set(HUXERUI_EXISTING_RESOURCE_ROOT_PATH
                "${HUXERUI_EXISTING_RESOURCE_ROOT}"
        )
        cmake_path(IS_PREFIX
                HUXERUI_EXISTING_RESOURCE_ROOT_PATH
                "${HUXERUI_RESOURCE_ROOT}"
                NORMALIZE
                HUXERUI_EXISTING_RESOURCE_ROOT_IS_PREFIX
        )
        set(HUXERUI_RESOURCE_ROOT_PATH "${HUXERUI_RESOURCE_ROOT}")
        cmake_path(IS_PREFIX
                HUXERUI_RESOURCE_ROOT_PATH
                "${HUXERUI_EXISTING_RESOURCE_ROOT}"
                NORMALIZE
                HUXERUI_RESOURCE_ROOT_IS_PREFIX
        )
        if (HUXERUI_EXISTING_RESOURCE_ROOT_IS_PREFIX
                OR HUXERUI_RESOURCE_ROOT_IS_PREFIX)
            message(FATAL_ERROR
                    "huxerui_add_resources() ROOT overlaps a root already registered for ${target_name}: ${HUXERUI_RESOURCE_ROOT}"
            )
        endif ()
    endforeach ()

    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_RESOURCE_ROOTS
            "${HUXERUI_RESOURCE_ROOT}"
    )
    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_RESOURCE_NAMESPACES
            "${HUXERUI_RESOURCES_NAMESPACE}"
    )
    _huxerui_get_resource_output(${target_name} HUXERUI_RESOURCE_OUTPUT)
    set_property(TARGET ${target_name} PROPERTY
            HUXERUI_RESOURCE_PACKAGE
            "${HUXERUI_RESOURCE_OUTPUT}/package"
    )
    _huxerui_schedule_resources(${target_name})
endfunction()
