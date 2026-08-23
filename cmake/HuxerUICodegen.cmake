include_guard(GLOBAL)

function(_huxerui_resolve_host output_system output_architecture)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" host_system)
    if (host_system STREQUAL "darwin")
        set(host_system "macos")
    elseif (NOT host_system STREQUAL "windows"
            AND NOT host_system STREQUAL "linux")
        message(FATAL_ERROR
                "HuxerUI host tools do not support ${CMAKE_HOST_SYSTEM_NAME}"
        )
    endif ()

    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" host_architecture)
    if (host_architecture MATCHES "^(amd64|x64|x86_64)$")
        set(host_architecture "x86_64")
    elseif (host_architecture MATCHES "^(aarch64|arm64)$")
        if (host_system STREQUAL "linux")
            set(host_architecture "aarch64")
        else ()
            set(host_architecture "arm64")
        endif ()
    else ()
        message(FATAL_ERROR
                "HuxerUI host tools do not support ${CMAKE_HOST_SYSTEM_PROCESSOR}"
        )
    endif ()

    set(${output_system} "${host_system}" PARENT_SCOPE)
    set(${output_architecture} "${host_architecture}" PARENT_SCOPE)
endfunction()

function(huxerui_resolve_host_tool tool_name output_variable)
    _huxerui_resolve_host(HUXERUI_HOST_SYSTEM HUXERUI_HOST_ARCHITECTURE)

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
