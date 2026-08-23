include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/HuxerUIResources.cmake")

function(_huxerui_select_framework_target output_variable)
    if (ANDROID)
        if (NOT TARGET HuxerUI::huxerui)
            message(FATAL_ERROR "HuxerUI Android integration requires the shared component")
        endif ()
        set(HUXERUI_FRAMEWORK_TARGET HuxerUI::huxerui)
    elseif (TARGET HuxerUI::huxerui_static)
        set(HUXERUI_FRAMEWORK_TARGET HuxerUI::huxerui_static)
    elseif (TARGET HuxerUI::huxerui)
        set(HUXERUI_FRAMEWORK_TARGET HuxerUI::huxerui)
    else ()
        message(FATAL_ERROR "HuxerUI framework target is unavailable")
    endif ()
    set(${output_variable} "${HUXERUI_FRAMEWORK_TARGET}" PARENT_SCOPE)
endfunction()

function(_huxerui_resolve_library_target requested_target output_variable)
    if (NOT TARGET ${requested_target})
        message(FATAL_ERROR
                "HuxerUI library target does not exist: ${requested_target}"
        )
    endif ()

    get_target_property(HUXERUI_LIBRARY_ALIASED_TARGET
            ${requested_target}
            ALIASED_TARGET
    )
    if (HUXERUI_LIBRARY_ALIASED_TARGET)
        set(HUXERUI_LIBRARY_TARGET "${HUXERUI_LIBRARY_ALIASED_TARGET}")
    else ()
        set(HUXERUI_LIBRARY_TARGET "${requested_target}")
    endif ()
    set(${output_variable} "${HUXERUI_LIBRARY_TARGET}" PARENT_SCOPE)
endfunction()

function(_huxerui_json_escape input output)
    string(REPLACE "\\" "\\\\" value "${input}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_huxerui_write_library_graph target_name output_file)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR
                "HuxerUI library graph application target does not exist: ${target_name}"
        )
    endif ()

    get_property(HUXERUI_GRAPH_LIBRARIES
            TARGET ${target_name}
            PROPERTY HUXERUI_LIBRARIES
    )
    get_property(HUXERUI_GRAPH_REQUESTED_TARGETS
            TARGET ${target_name}
            PROPERTY HUXERUI_REQUESTED_LIBRARY_TARGETS
    )
    list(LENGTH HUXERUI_GRAPH_LIBRARIES HUXERUI_GRAPH_LIBRARY_COUNT)
    list(LENGTH HUXERUI_GRAPH_REQUESTED_TARGETS
            HUXERUI_GRAPH_REQUESTED_TARGET_COUNT
    )
    if (NOT HUXERUI_GRAPH_LIBRARY_COUNT EQUAL
            HUXERUI_GRAPH_REQUESTED_TARGET_COUNT)
        message(FATAL_ERROR
                "HuxerUI library graph target identity is inconsistent for ${target_name}"
        )
    endif ()
    set(HUXERUI_GRAPH_ENTRIES)
    set(HUXERUI_GRAPH_INDEX 0)
    foreach (HUXERUI_GRAPH_LIBRARY IN LISTS HUXERUI_GRAPH_LIBRARIES)
        list(GET HUXERUI_GRAPH_REQUESTED_TARGETS
                ${HUXERUI_GRAPH_INDEX}
                HUXERUI_GRAPH_REQUESTED_TARGET
        )
        math(EXPR HUXERUI_GRAPH_INDEX "${HUXERUI_GRAPH_INDEX} + 1")
        get_property(HUXERUI_GRAPH_SOURCE_ROOT_SET
                TARGET ${HUXERUI_GRAPH_LIBRARY}
                PROPERTY HUXERUI_LIBRARY_SOURCE_ROOT
                SET
        )
        if (NOT HUXERUI_GRAPH_SOURCE_ROOT_SET)
            continue()
        endif ()
        get_property(HUXERUI_GRAPH_SOURCE_ROOT
                TARGET ${HUXERUI_GRAPH_LIBRARY}
                PROPERTY HUXERUI_LIBRARY_SOURCE_ROOT
        )
        _huxerui_json_escape(
                "${HUXERUI_GRAPH_REQUESTED_TARGET}"
                HUXERUI_GRAPH_JSON_TARGET
        )
        _huxerui_json_escape(
                "${HUXERUI_GRAPH_SOURCE_ROOT}"
                HUXERUI_GRAPH_JSON_SOURCE_ROOT
        )
        if (HUXERUI_GRAPH_ENTRIES)
            string(APPEND HUXERUI_GRAPH_ENTRIES ",\n")
        endif ()
        string(APPEND HUXERUI_GRAPH_ENTRIES
                "    {\"target\": \"${HUXERUI_GRAPH_JSON_TARGET}\", \"sourceRoot\": \"${HUXERUI_GRAPH_JSON_SOURCE_ROOT}\"}"
        )
    endforeach ()

    get_filename_component(HUXERUI_GRAPH_DIRECTORY
            "${output_file}"
            DIRECTORY
    )
    file(MAKE_DIRECTORY "${HUXERUI_GRAPH_DIRECTORY}")
    file(WRITE "${output_file}"
            "{\n  \"schema\": 1,\n  \"libraries\": [\n${HUXERUI_GRAPH_ENTRIES}\n  ]\n}\n"
    )
endfunction()

function(huxerui_add_library target_name)
    cmake_parse_arguments(HUXERUI_LIBRARY
            ""
            "RESOURCE_NAMESPACE"
            "SOURCES;RESOURCES"
            ${ARGN}
    )
    if (HUXERUI_LIBRARY_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
                "huxerui_add_library() received unknown arguments: ${HUXERUI_LIBRARY_UNPARSED_ARGUMENTS}"
        )
    endif ()
    if (TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_add_library() target already exists: ${target_name}"
        )
    endif ()
    if (NOT HUXERUI_LIBRARY_SOURCES)
        message(FATAL_ERROR
                "huxerui_add_library() requires at least one source"
        )
    endif ()
    if (HUXERUI_LIBRARY_RESOURCES AND NOT HUXERUI_LIBRARY_RESOURCE_NAMESPACE)
        message(FATAL_ERROR
                "huxerui_add_library() requires RESOURCE_NAMESPACE when RESOURCES is present"
        )
    endif ()
    if (HUXERUI_LIBRARY_RESOURCE_NAMESPACE AND NOT HUXERUI_LIBRARY_RESOURCES)
        message(FATAL_ERROR
                "huxerui_add_library() RESOURCE_NAMESPACE requires RESOURCES"
        )
    endif ()
    list(LENGTH HUXERUI_LIBRARY_RESOURCES HUXERUI_LIBRARY_RESOURCE_ROOT_COUNT)
    if (HUXERUI_LIBRARY_RESOURCE_ROOT_COUNT GREATER 1)
        message(FATAL_ERROR
                "huxerui_add_library() currently accepts one resource root"
        )
    endif ()
    add_library(${target_name} STATIC ${HUXERUI_LIBRARY_SOURCES})
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_target_properties(${target_name} PROPERTIES
            CXX_EXTENSIONS OFF
            POSITION_INDEPENDENT_CODE ON
    )
    file(REAL_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}"
            HUXERUI_LIBRARY_SOURCE_ROOT
    )
    set_property(TARGET ${target_name} PROPERTY HUXERUI_LIBRARY TRUE)
    set_property(TARGET ${target_name} PROPERTY
            HUXERUI_LIBRARY_SOURCE_ROOT
            "${HUXERUI_LIBRARY_SOURCE_ROOT}"
    )
    set_property(TARGET ${target_name} APPEND PROPERTY EXPORT_PROPERTIES
            HUXERUI_LIBRARY
    )
    if (HUXERUI_LIBRARY_GRAPH_ONLY)
        return()
    endif ()
    _huxerui_select_framework_target(HUXERUI_LIBRARY_FRAMEWORK_TARGET)
    target_link_libraries(${target_name} PRIVATE ${HUXERUI_LIBRARY_FRAMEWORK_TARGET})
    huxerui_enable_codegen(${target_name})

    if (HUXERUI_LIBRARY_RESOURCES)
        list(GET HUXERUI_LIBRARY_RESOURCES 0 HUXERUI_LIBRARY_RESOURCE_ROOT)
        huxerui_add_resources(${target_name}
                ROOT "${HUXERUI_LIBRARY_RESOURCE_ROOT}"
                NAMESPACE "${HUXERUI_LIBRARY_RESOURCE_NAMESPACE}"
        )
    endif ()
endfunction()

function(huxerui_use_library target_name)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_use_library() application target does not exist: ${target_name}"
        )
    endif ()
    cmake_parse_arguments(HUXERUI_USE_LIBRARY
            ""
            "TARGET;PATH;URL;REVISION"
            ""
            ${ARGN}
    )
    if (HUXERUI_USE_LIBRARY_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
                "huxerui_use_library() received unknown arguments: ${HUXERUI_USE_LIBRARY_UNPARSED_ARGUMENTS}"
        )
    endif ()
    if (NOT HUXERUI_USE_LIBRARY_TARGET)
        message(FATAL_ERROR "huxerui_use_library() requires TARGET")
    endif ()
    if (HUXERUI_USE_LIBRARY_PATH AND HUXERUI_USE_LIBRARY_URL)
        message(FATAL_ERROR
                "huxerui_use_library() PATH and URL are mutually exclusive"
        )
    endif ()
    if (HUXERUI_USE_LIBRARY_REVISION AND NOT HUXERUI_USE_LIBRARY_URL)
        message(FATAL_ERROR
                "huxerui_use_library() REVISION requires URL"
        )
    endif ()

    set(HUXERUI_LIBRARY_TARGET_PREEXISTED FALSE)
    if (TARGET ${HUXERUI_USE_LIBRARY_TARGET})
        set(HUXERUI_LIBRARY_TARGET_PREEXISTED TRUE)
    endif ()
    set(HUXERUI_LIBRARY_ORIGIN)
    if (HUXERUI_USE_LIBRARY_PATH)
        file(REAL_PATH
                "${HUXERUI_USE_LIBRARY_PATH}"
                HUXERUI_LIBRARY_SOURCE_PATH
                BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        if (NOT EXISTS "${HUXERUI_LIBRARY_SOURCE_PATH}/CMakeLists.txt")
            message(FATAL_ERROR
                    "huxerui_use_library() PATH does not contain CMakeLists.txt: ${HUXERUI_LIBRARY_SOURCE_PATH}"
            )
        endif ()
        set(HUXERUI_LIBRARY_ORIGIN "PATH:${HUXERUI_LIBRARY_SOURCE_PATH}")
        if (NOT TARGET ${HUXERUI_USE_LIBRARY_TARGET})
            string(SHA256 HUXERUI_LIBRARY_SOURCE_HASH
                    "${HUXERUI_LIBRARY_SOURCE_PATH}"
            )
            string(SUBSTRING "${HUXERUI_LIBRARY_SOURCE_HASH}" 0 16
                    HUXERUI_LIBRARY_SOURCE_HASH
            )
            get_target_property(HUXERUI_LIBRARY_APP_BINARY_DIR
                    ${target_name}
                    BINARY_DIR
            )
            add_subdirectory(
                    "${HUXERUI_LIBRARY_SOURCE_PATH}"
                    "${HUXERUI_LIBRARY_APP_BINARY_DIR}/huxerui-libraries/sources/${HUXERUI_LIBRARY_SOURCE_HASH}"
            )
        endif ()
    elseif (HUXERUI_USE_LIBRARY_URL)
        if (NOT HUXERUI_USE_LIBRARY_URL MATCHES "^https://")
            message(FATAL_ERROR
                    "huxerui_use_library() URL must use HTTPS"
            )
        endif ()
        if (HUXERUI_USE_LIBRARY_URL MATCHES "^https://[^/]*@")
            message(FATAL_ERROR
                    "huxerui_use_library() URL must not contain credentials"
            )
        endif ()
        if (NOT HUXERUI_USE_LIBRARY_REVISION)
            message(FATAL_ERROR
                    "huxerui_use_library() URL requires REVISION"
            )
        endif ()
        string(LENGTH "${HUXERUI_USE_LIBRARY_REVISION}"
                HUXERUI_LIBRARY_REVISION_LENGTH
        )
        if (NOT HUXERUI_LIBRARY_REVISION_LENGTH EQUAL 40
                OR NOT HUXERUI_USE_LIBRARY_REVISION MATCHES "^[0-9A-Fa-f]+$")
            message(FATAL_ERROR
                    "huxerui_use_library() REVISION must be a full commit SHA"
            )
        endif ()
        set(HUXERUI_LIBRARY_ORIGIN
                "URL:${HUXERUI_USE_LIBRARY_URL}@${HUXERUI_USE_LIBRARY_REVISION}"
        )
        if (NOT TARGET ${HUXERUI_USE_LIBRARY_TARGET})
            include(FetchContent)
            string(SHA256 HUXERUI_LIBRARY_SOURCE_HASH
                    "${HUXERUI_LIBRARY_ORIGIN}"
            )
            string(SUBSTRING "${HUXERUI_LIBRARY_SOURCE_HASH}" 0 16
                    HUXERUI_LIBRARY_SOURCE_HASH
            )
            set(HUXERUI_LIBRARY_FETCH_NAME
                    "huxerui_library_${HUXERUI_LIBRARY_SOURCE_HASH}"
            )
            FetchContent_Declare(${HUXERUI_LIBRARY_FETCH_NAME}
                    GIT_REPOSITORY "${HUXERUI_USE_LIBRARY_URL}"
                    GIT_TAG "${HUXERUI_USE_LIBRARY_REVISION}"
                    GIT_SHALLOW FALSE
            )
            FetchContent_MakeAvailable(${HUXERUI_LIBRARY_FETCH_NAME})
        endif ()
    endif ()

    if (NOT TARGET ${HUXERUI_USE_LIBRARY_TARGET})
        message(FATAL_ERROR
                "huxerui_use_library() acquisition did not create target: ${HUXERUI_USE_LIBRARY_TARGET}"
        )
    endif ()
    _huxerui_resolve_library_target(
            ${HUXERUI_USE_LIBRARY_TARGET}
            HUXERUI_RESOLVED_LIBRARY_TARGET
    )
    get_property(HUXERUI_TARGET_IS_LIBRARY
            TARGET ${HUXERUI_RESOLVED_LIBRARY_TARGET}
            PROPERTY HUXERUI_LIBRARY
    )
    if (NOT HUXERUI_TARGET_IS_LIBRARY)
        message(FATAL_ERROR
                "huxerui_use_library() TARGET is not a HuxerUI library: ${HUXERUI_USE_LIBRARY_TARGET}"
        )
    endif ()

    get_property(HUXERUI_EXISTING_LIBRARIES
            TARGET ${target_name}
            PROPERTY HUXERUI_LIBRARIES
    )
    if (HUXERUI_RESOLVED_LIBRARY_TARGET IN_LIST HUXERUI_EXISTING_LIBRARIES)
        message(FATAL_ERROR
                "huxerui_use_library() library is already used by ${target_name}: ${HUXERUI_USE_LIBRARY_TARGET}"
        )
    endif ()
    get_property(HUXERUI_EXISTING_LIBRARY_ORIGIN_SET
            TARGET ${HUXERUI_RESOLVED_LIBRARY_TARGET}
            PROPERTY HUXERUI_LIBRARY_ORIGIN
            SET
    )
    if (HUXERUI_LIBRARY_ORIGIN AND HUXERUI_EXISTING_LIBRARY_ORIGIN_SET)
        get_property(HUXERUI_EXISTING_LIBRARY_ORIGIN
                TARGET ${HUXERUI_RESOLVED_LIBRARY_TARGET}
                PROPERTY HUXERUI_LIBRARY_ORIGIN
        )
        if (NOT "${HUXERUI_EXISTING_LIBRARY_ORIGIN}" STREQUAL "${HUXERUI_LIBRARY_ORIGIN}")
            message(FATAL_ERROR
                    "huxerui_use_library() target was acquired from a different source: ${HUXERUI_USE_LIBRARY_TARGET}"
            )
        endif ()
    elseif (HUXERUI_LIBRARY_ORIGIN AND HUXERUI_LIBRARY_TARGET_PREEXISTED)
        message(FATAL_ERROR
                "huxerui_use_library() TARGET already exists without a matching acquisition origin; omit PATH and URL: ${HUXERUI_USE_LIBRARY_TARGET}"
        )
    elseif (HUXERUI_LIBRARY_ORIGIN)
        set_property(TARGET ${HUXERUI_RESOLVED_LIBRARY_TARGET} PROPERTY
                HUXERUI_LIBRARY_ORIGIN
                "${HUXERUI_LIBRARY_ORIGIN}"
        )
    endif ()

    get_property(HUXERUI_LIBRARY_GRAPH_ONLY
            TARGET ${target_name}
            PROPERTY HUXERUI_LIBRARY_GRAPH_ONLY
    )
    if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
        target_link_libraries(${target_name} PRIVATE ${HUXERUI_USE_LIBRARY_TARGET})
    endif ()
    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_LIBRARIES
            "${HUXERUI_RESOLVED_LIBRARY_TARGET}"
    )
    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_REQUESTED_LIBRARY_TARGETS
            "${HUXERUI_USE_LIBRARY_TARGET}"
    )
    get_property(HUXERUI_LIBRARY_GRAPH_OUTPUT_SET
            TARGET ${target_name}
            PROPERTY HUXERUI_LIBRARY_GRAPH_OUTPUT
            SET
    )
    if (HUXERUI_LIBRARY_GRAPH_OUTPUT_SET)
        get_property(HUXERUI_LIBRARY_GRAPH_OUTPUT
                TARGET ${target_name}
                PROPERTY HUXERUI_LIBRARY_GRAPH_OUTPUT
        )
        _huxerui_write_library_graph(
                ${target_name}
                "${HUXERUI_LIBRARY_GRAPH_OUTPUT}"
        )
    endif ()
    if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
        get_property(HUXERUI_LIBRARY_RESOURCE_PACKAGE_SET
                TARGET ${HUXERUI_RESOLVED_LIBRARY_TARGET}
                PROPERTY HUXERUI_RESOURCE_PACKAGE
                SET
        )
        if (HUXERUI_LIBRARY_RESOURCE_PACKAGE_SET)
            get_property(HUXERUI_LIBRARY_RESOURCE_PACKAGE
                    TARGET ${HUXERUI_RESOLVED_LIBRARY_TARGET}
                    PROPERTY HUXERUI_RESOURCE_PACKAGE
            )
            _huxerui_append_resource_package_input(
                    ${target_name}
                    "${HUXERUI_LIBRARY_RESOURCE_PACKAGE}"
            )
        endif ()
    endif ()
endfunction()
