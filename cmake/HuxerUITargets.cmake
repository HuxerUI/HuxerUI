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
    set(HUXERUI_PLATFORM_LINK_OPTIONS)

    if (EMSCRIPTEN)
        set(HUXERUI_PLATFORM_ID "web")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Web.cmake")
    elseif (ANDROID)
        set(HUXERUI_PLATFORM_ID "android")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Android.cmake")
    elseif (APPLE)
        set(HUXERUI_PLATFORM_ID "macos")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Apple.cmake")
    elseif (WIN32)
        set(HUXERUI_PLATFORM_ID "windows")
        include("${HUXERUI_TARGETS_CMAKE_DIR}/platform/Windows.cmake")
    else ()
        message(FATAL_ERROR "HuxerUI currently supports Android, macOS, Windows, and Web only")
    endif ()

    huxerui_platform_configure()

    set(HUXERUI_PLATFORM_ID "${HUXERUI_PLATFORM_ID}" PARENT_SCOPE)
    set(HUXERUI_PLATFORM_SOURCE_FILES ${HUXERUI_PLATFORM_SOURCE_FILES} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_COMPILE_OPTIONS ${HUXERUI_PLATFORM_COMPILE_OPTIONS} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS ${HUXERUI_PLATFORM_INTERFACE_COMPILE_OPTIONS} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_COMPILE_DEFINITIONS ${HUXERUI_PLATFORM_COMPILE_DEFINITIONS} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_LINK_LIBRARIES ${HUXERUI_PLATFORM_LINK_LIBRARIES} PARENT_SCOPE)
    set(HUXERUI_PLATFORM_LINK_OPTIONS ${HUXERUI_PLATFORM_LINK_OPTIONS} PARENT_SCOPE)
endfunction()

function(huxerui_configure_compile_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_include_directories(${target_name} PRIVATE
            "${HUXERUI_PUBLIC_INCLUDE_DIR}"
            "${HUXERUI_PROJECT_DIR}/src"
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

    if (WIN32)
        get_target_property(HUXERUI_TARGET_TYPE ${target_name} TYPE)
        if (HUXERUI_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
            set_target_properties(${target_name} PROPERTIES
                    WINDOWS_EXPORT_ALL_SYMBOLS ON
            )
        endif ()
    endif ()
endfunction()

function(huxerui_configure_targets)
    if (NOT HUXERUI_BUILD_SHARED AND NOT HUXERUI_BUILD_STATIC)
        message(FATAL_ERROR "At least one HuxerUI library target must be enabled")
    endif ()
    if (EMSCRIPTEN AND HUXERUI_BUILD_SHARED)
        message(FATAL_ERROR "HuxerUI Web supports the static library target only")
    endif ()

    huxerui_configure_platform()

    file(GLOB HUXERUI_CORE_SOURCE_FILES CONFIGURE_DEPENDS
            "${HUXERUI_PROJECT_DIR}/src/*.cpp"
    )

    add_library(huxerui_core_objects OBJECT
            ${HUXERUI_CORE_SOURCE_FILES}
            ${HUXERUI_PLATFORM_SOURCE_FILES}
    )
    set_target_properties(huxerui_core_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
    huxerui_configure_compile_target(huxerui_core_objects)

    if (HUXERUI_BUILD_SHARED)
        add_library(${HUXERUI_SHARED_LIB_NAME} SHARED
                $<TARGET_OBJECTS:huxerui_core_objects>
        )
        huxerui_configure_public_target(${HUXERUI_SHARED_LIB_NAME})
        add_library(HuxerUI::huxerui ALIAS ${HUXERUI_SHARED_LIB_NAME})
    endif ()

    if (HUXERUI_BUILD_STATIC)
        add_library(${HUXERUI_STATIC_LIB_NAME} STATIC
                $<TARGET_OBJECTS:huxerui_core_objects>
        )
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
    set(HUXERUI_HOST_TOOL
            "${HUXERUI_PROJECT_DIR}/tools/prebuilt/${HUXERUI_HOST_SYSTEM}/${HUXERUI_HOST_ARCHITECTURE}/huxerui-${tool_name}${HUXERUI_HOST_TOOL_SUFFIX}"
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

    huxerui_resolve_host_tool("codegen" HUXERUI_CODEGEN_COMMAND)

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

        set(HUXERUI_CODEGEN_OUTPUT
                "${HUXERUI_CODEGEN_BINARY_DIR}/huxerui-codegen/${target_name}/${HUXERUI_CODEGEN_SOURCE_HASH}/${HUXERUI_CODEGEN_SOURCE_NAME}"
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

    get_target_property(HUXERUI_RESOURCES_ALREADY_ENABLED
            ${target_name}
            HUXERUI_RESOURCES_ENABLED
    )
    if (HUXERUI_RESOURCES_ALREADY_ENABLED)
        message(FATAL_ERROR
                "huxerui_add_resources() may only be called once for ${target_name}"
        )
    endif ()
    set_property(TARGET ${target_name} PROPERTY HUXERUI_RESOURCES_ENABLED TRUE)

    get_filename_component(HUXERUI_RESOURCE_ROOT
            "${HUXERUI_RESOURCES_ROOT}"
            ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if (NOT IS_DIRECTORY "${HUXERUI_RESOURCE_ROOT}")
        message(FATAL_ERROR
                "huxerui_add_resources() ROOT is not a directory: ${HUXERUI_RESOURCE_ROOT}"
        )
    endif ()

    huxerui_resolve_host_tool("resource-codegen" HUXERUI_RESOURCE_CODEGEN_COMMAND)
    set(HUXERUI_RESOURCE_OUTPUT
            "${CMAKE_CURRENT_BINARY_DIR}/huxerui-resources/${target_name}"
    )
    set(HUXERUI_RESOURCE_STAMP
            "${HUXERUI_RESOURCE_OUTPUT}/resources.stamp"
    )
    set(HUXERUI_RESOURCE_HEADER
            "${HUXERUI_RESOURCE_OUTPUT}/include/${HUXERUI_RESOURCES_NAMESPACE}_resources.h"
    )
    set(HUXERUI_RESOURCE_INDEX
            "${HUXERUI_RESOURCE_OUTPUT}/package/huxerui/resources.bin"
    )
    set(HUXERUI_RESOURCE_INPUT_STAMP
            "${HUXERUI_RESOURCE_OUTPUT}/resource-inputs.stamp"
    )

    file(GLOB_RECURSE HUXERUI_RESOURCE_INPUTS
            CONFIGURE_DEPENDS
            LIST_DIRECTORIES FALSE
            "${HUXERUI_RESOURCE_ROOT}/*"
    )
    list(SORT HUXERUI_RESOURCE_INPUTS)
    string(REPLACE ";" "\n"
            HUXERUI_RESOURCE_INPUT_LIST
            "${HUXERUI_RESOURCE_INPUTS}"
    )
    file(MAKE_DIRECTORY "${HUXERUI_RESOURCE_OUTPUT}")
    # The generated membership stamp makes resource additions and removals invalidate the custom command after CMake
    # reconfigures the glob.
    file(GENERATE
            OUTPUT "${HUXERUI_RESOURCE_INPUT_STAMP}"
            CONTENT "${HUXERUI_RESOURCE_INPUT_LIST}\n"
    )
    add_custom_command(
            OUTPUT
                    "${HUXERUI_RESOURCE_STAMP}"
                    "${HUXERUI_RESOURCE_HEADER}"
                    "${HUXERUI_RESOURCE_INDEX}"
            COMMAND "${HUXERUI_RESOURCE_CODEGEN_COMMAND}"
                    --root "${HUXERUI_RESOURCE_ROOT}"
                    --output "${HUXERUI_RESOURCE_OUTPUT}"
                    --namespace "${HUXERUI_RESOURCES_NAMESPACE}"
            DEPENDS
                    ${HUXERUI_RESOURCE_INPUTS}
                    "${HUXERUI_RESOURCE_INPUT_STAMP}"
                    "${HUXERUI_RESOURCE_CODEGEN_COMMAND}"
            COMMENT "Generating HuxerUI resources for ${target_name}"
            VERBATIM
    )
    add_custom_target(${target_name}_huxerui_resources
            DEPENDS
                    "${HUXERUI_RESOURCE_STAMP}"
                    "${HUXERUI_RESOURCE_HEADER}"
                    "${HUXERUI_RESOURCE_INDEX}"
    )
    add_dependencies(${target_name} ${target_name}_huxerui_resources)
    target_include_directories(${target_name} PRIVATE
            "${HUXERUI_RESOURCE_OUTPUT}/include"
    )

    set(HUXERUI_RESOURCE_STAGE_DIRECTORY)
    # Gradle stages Android packages after all ABI builds; CMake stages desktop targets with one output package.
    if (EMSCRIPTEN)
        target_link_options(${target_name} PRIVATE
                "SHELL:--preload-file \"${HUXERUI_RESOURCE_OUTPUT}/package@/\""
        )
        set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS
                "${HUXERUI_RESOURCE_STAMP}"
                "${HUXERUI_RESOURCE_INDEX}"
        )
    elseif (APPLE)
        set(HUXERUI_RESOURCE_STAGE_DIRECTORY
                "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Resources/HuxerUI"
        )
    elseif (WIN32)
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
                DEPENDS
                        "${HUXERUI_RESOURCE_STAMP}"
                        "${HUXERUI_RESOURCE_INDEX}"
                COMMENT "Staging HuxerUI resources for ${target_name}"
                VERBATIM
        )
        add_custom_target(${target_name}_huxerui_resource_staging
                DEPENDS "${HUXERUI_RESOURCE_STAGE_STAMP}"
        )
        add_dependencies(${target_name}
                ${target_name}_huxerui_resource_staging
        )
    endif ()
endfunction()
