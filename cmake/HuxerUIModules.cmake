include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/HuxerUIResources.cmake")

function(_huxerui_resolve_module_target requested_target output_variable)
    if (NOT TARGET ${requested_target})
        message(FATAL_ERROR
                "HuxerUI module target does not exist: ${requested_target}"
        )
    endif ()

    get_target_property(HUXERUI_MODULE_ALIASED_TARGET
            ${requested_target}
            ALIASED_TARGET
    )
    if (HUXERUI_MODULE_ALIASED_TARGET)
        set(HUXERUI_MODULE_TARGET "${HUXERUI_MODULE_ALIASED_TARGET}")
    else ()
        set(HUXERUI_MODULE_TARGET "${requested_target}")
    endif ()
    set(${output_variable} "${HUXERUI_MODULE_TARGET}" PARENT_SCOPE)
endfunction()

function(_huxerui_json_escape input output)
    string(REPLACE "\\" "\\\\" value "${input}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_huxerui_write_module_graph target_name output_file)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR
                "HuxerUI module graph application target does not exist: ${target_name}"
        )
    endif ()

    get_property(HUXERUI_GRAPH_MODULES
            TARGET ${target_name}
            PROPERTY HUXERUI_MODULES
    )
    get_property(HUXERUI_GRAPH_REQUESTED_TARGETS
            TARGET ${target_name}
            PROPERTY HUXERUI_REQUESTED_MODULE_TARGETS
    )
    list(LENGTH HUXERUI_GRAPH_MODULES HUXERUI_GRAPH_MODULE_COUNT)
    list(LENGTH HUXERUI_GRAPH_REQUESTED_TARGETS
            HUXERUI_GRAPH_REQUESTED_TARGET_COUNT
    )
    if (NOT HUXERUI_GRAPH_MODULE_COUNT EQUAL
            HUXERUI_GRAPH_REQUESTED_TARGET_COUNT)
        message(FATAL_ERROR
                "HuxerUI module graph target identity is inconsistent for ${target_name}"
        )
    endif ()
    set(HUXERUI_GRAPH_ENTRIES)
    set(HUXERUI_GRAPH_INDEX 0)
    foreach (HUXERUI_GRAPH_MODULE IN LISTS HUXERUI_GRAPH_MODULES)
        list(GET HUXERUI_GRAPH_REQUESTED_TARGETS
                ${HUXERUI_GRAPH_INDEX}
                HUXERUI_GRAPH_REQUESTED_TARGET
        )
        math(EXPR HUXERUI_GRAPH_INDEX "${HUXERUI_GRAPH_INDEX} + 1")
        get_property(HUXERUI_GRAPH_SOURCE_ROOT_SET
                TARGET ${HUXERUI_GRAPH_MODULE}
                PROPERTY HUXERUI_MODULE_SOURCE_ROOT
                SET
        )
        if (NOT HUXERUI_GRAPH_SOURCE_ROOT_SET)
            continue()
        endif ()
        get_property(HUXERUI_GRAPH_SOURCE_ROOT
                TARGET ${HUXERUI_GRAPH_MODULE}
                PROPERTY HUXERUI_MODULE_SOURCE_ROOT
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
            "{\n  \"schema\": 1,\n  \"modules\": [\n${HUXERUI_GRAPH_ENTRIES}\n  ]\n}\n"
    )
endfunction()

function(huxerui_add_module target_name)
    cmake_parse_arguments(HUXERUI_MODULE
            ""
            "RESOURCE_NAMESPACE"
            "SOURCES;RESOURCES"
            ${ARGN}
    )
    if (HUXERUI_MODULE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
                "huxerui_add_module() received unknown arguments: ${HUXERUI_MODULE_UNPARSED_ARGUMENTS}"
        )
    endif ()
    if (TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_add_module() target already exists: ${target_name}"
        )
    endif ()
    if (NOT HUXERUI_MODULE_SOURCES)
        message(FATAL_ERROR
                "huxerui_add_module() requires at least one source"
        )
    endif ()
    if (HUXERUI_MODULE_RESOURCES AND NOT HUXERUI_MODULE_RESOURCE_NAMESPACE)
        message(FATAL_ERROR
                "huxerui_add_module() requires RESOURCE_NAMESPACE when RESOURCES is present"
        )
    endif ()
    if (HUXERUI_MODULE_RESOURCE_NAMESPACE AND NOT HUXERUI_MODULE_RESOURCES)
        message(FATAL_ERROR
                "huxerui_add_module() RESOURCE_NAMESPACE requires RESOURCES"
        )
    endif ()
    list(LENGTH HUXERUI_MODULE_RESOURCES HUXERUI_MODULE_RESOURCE_ROOT_COUNT)
    if (HUXERUI_MODULE_RESOURCE_ROOT_COUNT GREATER 1)
        message(FATAL_ERROR
                "huxerui_add_module() currently accepts one resource root"
        )
    endif ()
    add_library(${target_name} STATIC ${HUXERUI_MODULE_SOURCES})
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_target_properties(${target_name} PROPERTIES
            CXX_EXTENSIONS OFF
            POSITION_INDEPENDENT_CODE ON
    )
    file(REAL_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}"
            HUXERUI_MODULE_SOURCE_ROOT
    )
    set_property(TARGET ${target_name} PROPERTY HUXERUI_MODULE TRUE)
    set_property(TARGET ${target_name} PROPERTY
            HUXERUI_MODULE_SOURCE_ROOT
            "${HUXERUI_MODULE_SOURCE_ROOT}"
    )
    set_property(TARGET ${target_name} APPEND PROPERTY EXPORT_PROPERTIES
            HUXERUI_MODULE
    )
    if (HUXERUI_MODULE_GRAPH_ONLY)
        return()
    endif ()
    if (TARGET HuxerUI::huxerui_static AND NOT ANDROID)
        set(HUXERUI_MODULE_FRAMEWORK_TARGET HuxerUI::huxerui_static)
    elseif (TARGET HuxerUI::huxerui)
        set(HUXERUI_MODULE_FRAMEWORK_TARGET HuxerUI::huxerui)
    else ()
        message(FATAL_ERROR
                "huxerui_add_module() requires the HuxerUI::huxerui target"
        )
    endif ()
    get_target_property(HUXERUI_MODULE_FRAMEWORK_ALIASED_TARGET
            ${HUXERUI_MODULE_FRAMEWORK_TARGET}
            ALIASED_TARGET
    )
    if (HUXERUI_MODULE_FRAMEWORK_ALIASED_TARGET)
        set(HUXERUI_MODULE_FRAMEWORK_TARGET
                "${HUXERUI_MODULE_FRAMEWORK_ALIASED_TARGET}"
        )
    endif ()
    # The application owns final HuxerUI linkage; module archives inherit compile usage only.
    get_property(HUXERUI_MODULE_FRAMEWORK_INCLUDE_DIRECTORIES
            TARGET ${HUXERUI_MODULE_FRAMEWORK_TARGET}
            PROPERTY INTERFACE_INCLUDE_DIRECTORIES
    )
    get_property(HUXERUI_MODULE_FRAMEWORK_COMPILE_OPTIONS
            TARGET ${HUXERUI_MODULE_FRAMEWORK_TARGET}
            PROPERTY INTERFACE_COMPILE_OPTIONS
    )
    get_property(HUXERUI_MODULE_FRAMEWORK_COMPILE_DEFINITIONS
            TARGET ${HUXERUI_MODULE_FRAMEWORK_TARGET}
            PROPERTY INTERFACE_COMPILE_DEFINITIONS
    )
    target_include_directories(${target_name} PRIVATE
            ${HUXERUI_MODULE_FRAMEWORK_INCLUDE_DIRECTORIES}
    )
    target_compile_options(${target_name} PRIVATE
            ${HUXERUI_MODULE_FRAMEWORK_COMPILE_OPTIONS}
    )
    target_compile_definitions(${target_name} PRIVATE
            ${HUXERUI_MODULE_FRAMEWORK_COMPILE_DEFINITIONS}
    )
    huxerui_enable_codegen(${target_name})

    if (HUXERUI_MODULE_RESOURCES)
        list(GET HUXERUI_MODULE_RESOURCES 0 HUXERUI_MODULE_RESOURCE_ROOT)
        huxerui_add_resources(${target_name}
                ROOT "${HUXERUI_MODULE_RESOURCE_ROOT}"
                NAMESPACE "${HUXERUI_MODULE_RESOURCE_NAMESPACE}"
        )
    endif ()
endfunction()

function(huxerui_use_module target_name)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_use_module() application target does not exist: ${target_name}"
        )
    endif ()
    cmake_parse_arguments(HUXERUI_USE_MODULE
            ""
            "TARGET;PATH;URL;REVISION"
            ""
            ${ARGN}
    )
    if (HUXERUI_USE_MODULE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
                "huxerui_use_module() received unknown arguments: ${HUXERUI_USE_MODULE_UNPARSED_ARGUMENTS}"
        )
    endif ()
    if (NOT HUXERUI_USE_MODULE_TARGET)
        message(FATAL_ERROR "huxerui_use_module() requires TARGET")
    endif ()
    if (HUXERUI_USE_MODULE_PATH AND HUXERUI_USE_MODULE_URL)
        message(FATAL_ERROR
                "huxerui_use_module() PATH and URL are mutually exclusive"
        )
    endif ()
    if (HUXERUI_USE_MODULE_REVISION AND NOT HUXERUI_USE_MODULE_URL)
        message(FATAL_ERROR
                "huxerui_use_module() REVISION requires URL"
        )
    endif ()

    set(HUXERUI_MODULE_TARGET_PREEXISTED FALSE)
    if (TARGET ${HUXERUI_USE_MODULE_TARGET})
        set(HUXERUI_MODULE_TARGET_PREEXISTED TRUE)
    endif ()
    set(HUXERUI_MODULE_ORIGIN)
    if (HUXERUI_USE_MODULE_PATH)
        file(REAL_PATH
                "${HUXERUI_USE_MODULE_PATH}"
                HUXERUI_MODULE_SOURCE_PATH
                BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        if (NOT EXISTS "${HUXERUI_MODULE_SOURCE_PATH}/CMakeLists.txt")
            message(FATAL_ERROR
                    "huxerui_use_module() PATH does not contain CMakeLists.txt: ${HUXERUI_MODULE_SOURCE_PATH}"
            )
        endif ()
        set(HUXERUI_MODULE_ORIGIN "PATH:${HUXERUI_MODULE_SOURCE_PATH}")
        if (NOT TARGET ${HUXERUI_USE_MODULE_TARGET})
            string(SHA256 HUXERUI_MODULE_SOURCE_HASH
                    "${HUXERUI_MODULE_SOURCE_PATH}"
            )
            string(SUBSTRING "${HUXERUI_MODULE_SOURCE_HASH}" 0 16
                    HUXERUI_MODULE_SOURCE_HASH
            )
            get_target_property(HUXERUI_MODULE_APP_BINARY_DIR
                    ${target_name}
                    BINARY_DIR
            )
            add_subdirectory(
                    "${HUXERUI_MODULE_SOURCE_PATH}"
                    "${HUXERUI_MODULE_APP_BINARY_DIR}/huxerui-modules/sources/${HUXERUI_MODULE_SOURCE_HASH}"
            )
        endif ()
    elseif (HUXERUI_USE_MODULE_URL)
        if (NOT HUXERUI_USE_MODULE_URL MATCHES "^https://")
            message(FATAL_ERROR
                    "huxerui_use_module() URL must use HTTPS"
            )
        endif ()
        if (HUXERUI_USE_MODULE_URL MATCHES "^https://[^/]*@")
            message(FATAL_ERROR
                    "huxerui_use_module() URL must not contain credentials"
            )
        endif ()
        if (NOT HUXERUI_USE_MODULE_REVISION)
            message(FATAL_ERROR
                    "huxerui_use_module() URL requires REVISION"
            )
        endif ()
        string(LENGTH "${HUXERUI_USE_MODULE_REVISION}"
                HUXERUI_MODULE_REVISION_LENGTH
        )
        if (NOT HUXERUI_MODULE_REVISION_LENGTH EQUAL 40
                OR NOT HUXERUI_USE_MODULE_REVISION MATCHES "^[0-9A-Fa-f]+$")
            message(FATAL_ERROR
                    "huxerui_use_module() REVISION must be a full commit SHA"
            )
        endif ()
        set(HUXERUI_MODULE_ORIGIN
                "URL:${HUXERUI_USE_MODULE_URL}@${HUXERUI_USE_MODULE_REVISION}"
        )
        if (NOT TARGET ${HUXERUI_USE_MODULE_TARGET})
            include(FetchContent)
            string(SHA256 HUXERUI_MODULE_SOURCE_HASH
                    "${HUXERUI_MODULE_ORIGIN}"
            )
            string(SUBSTRING "${HUXERUI_MODULE_SOURCE_HASH}" 0 16
                    HUXERUI_MODULE_SOURCE_HASH
            )
            set(HUXERUI_MODULE_FETCH_NAME
                    "huxerui_module_${HUXERUI_MODULE_SOURCE_HASH}"
            )
            FetchContent_Declare(${HUXERUI_MODULE_FETCH_NAME}
                    GIT_REPOSITORY "${HUXERUI_USE_MODULE_URL}"
                    GIT_TAG "${HUXERUI_USE_MODULE_REVISION}"
                    GIT_SHALLOW FALSE
            )
            FetchContent_MakeAvailable(${HUXERUI_MODULE_FETCH_NAME})
        endif ()
    endif ()

    if (NOT TARGET ${HUXERUI_USE_MODULE_TARGET})
        message(FATAL_ERROR
                "huxerui_use_module() acquisition did not create target: ${HUXERUI_USE_MODULE_TARGET}"
        )
    endif ()
    _huxerui_resolve_module_target(
            ${HUXERUI_USE_MODULE_TARGET}
            HUXERUI_RESOLVED_MODULE_TARGET
    )
    get_property(HUXERUI_TARGET_IS_MODULE
            TARGET ${HUXERUI_RESOLVED_MODULE_TARGET}
            PROPERTY HUXERUI_MODULE
    )
    if (NOT HUXERUI_TARGET_IS_MODULE)
        message(FATAL_ERROR
                "huxerui_use_module() TARGET is not a HuxerUI module: ${HUXERUI_USE_MODULE_TARGET}"
        )
    endif ()

    get_property(HUXERUI_EXISTING_MODULES
            TARGET ${target_name}
            PROPERTY HUXERUI_MODULES
    )
    if (HUXERUI_RESOLVED_MODULE_TARGET IN_LIST HUXERUI_EXISTING_MODULES)
        message(FATAL_ERROR
                "huxerui_use_module() module is already used by ${target_name}: ${HUXERUI_USE_MODULE_TARGET}"
        )
    endif ()
    get_property(HUXERUI_EXISTING_MODULE_ORIGIN_SET
            TARGET ${HUXERUI_RESOLVED_MODULE_TARGET}
            PROPERTY HUXERUI_MODULE_ORIGIN
            SET
    )
    if (HUXERUI_MODULE_ORIGIN AND HUXERUI_EXISTING_MODULE_ORIGIN_SET)
        get_property(HUXERUI_EXISTING_MODULE_ORIGIN
                TARGET ${HUXERUI_RESOLVED_MODULE_TARGET}
                PROPERTY HUXERUI_MODULE_ORIGIN
        )
        if (NOT "${HUXERUI_EXISTING_MODULE_ORIGIN}" STREQUAL "${HUXERUI_MODULE_ORIGIN}")
            message(FATAL_ERROR
                    "huxerui_use_module() target was acquired from a different source: ${HUXERUI_USE_MODULE_TARGET}"
            )
        endif ()
    elseif (HUXERUI_MODULE_ORIGIN AND HUXERUI_MODULE_TARGET_PREEXISTED)
        message(FATAL_ERROR
                "huxerui_use_module() TARGET already exists without a matching acquisition origin; omit PATH and URL: ${HUXERUI_USE_MODULE_TARGET}"
        )
    elseif (HUXERUI_MODULE_ORIGIN)
        set_property(TARGET ${HUXERUI_RESOLVED_MODULE_TARGET} PROPERTY
                HUXERUI_MODULE_ORIGIN
                "${HUXERUI_MODULE_ORIGIN}"
        )
    endif ()

    get_property(HUXERUI_MODULE_GRAPH_ONLY
            TARGET ${target_name}
            PROPERTY HUXERUI_MODULE_GRAPH_ONLY
    )
    if (NOT HUXERUI_MODULE_GRAPH_ONLY)
        target_link_libraries(${target_name} PRIVATE ${HUXERUI_USE_MODULE_TARGET})
    endif ()
    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_MODULES
            "${HUXERUI_RESOLVED_MODULE_TARGET}"
    )
    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_REQUESTED_MODULE_TARGETS
            "${HUXERUI_USE_MODULE_TARGET}"
    )
    get_property(HUXERUI_MODULE_GRAPH_OUTPUT_SET
            TARGET ${target_name}
            PROPERTY HUXERUI_MODULE_GRAPH_OUTPUT
            SET
    )
    if (HUXERUI_MODULE_GRAPH_OUTPUT_SET)
        get_property(HUXERUI_MODULE_GRAPH_OUTPUT
                TARGET ${target_name}
                PROPERTY HUXERUI_MODULE_GRAPH_OUTPUT
        )
        _huxerui_write_module_graph(
                ${target_name}
                "${HUXERUI_MODULE_GRAPH_OUTPUT}"
        )
    endif ()
    if (NOT HUXERUI_MODULE_GRAPH_ONLY)
        get_property(HUXERUI_MODULE_RESOURCE_PACKAGE_SET
                TARGET ${HUXERUI_RESOLVED_MODULE_TARGET}
                PROPERTY HUXERUI_RESOURCE_PACKAGE
                SET
        )
        if (HUXERUI_MODULE_RESOURCE_PACKAGE_SET)
            get_property(HUXERUI_MODULE_RESOURCE_PACKAGE
                    TARGET ${HUXERUI_RESOLVED_MODULE_TARGET}
                    PROPERTY HUXERUI_RESOURCE_PACKAGE
            )
            set_property(TARGET ${target_name} APPEND PROPERTY
                    HUXERUI_RESOURCE_PACKAGES
                    "${HUXERUI_MODULE_RESOURCE_PACKAGE}"
            )
            _huxerui_schedule_resources(${target_name})
        endif ()
    endif ()
endfunction()
