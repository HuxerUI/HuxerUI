include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/HuxerUICodegen.cmake")

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
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/HuxerUIResourceBuild.cmake"
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
    huxerui_resolve_host_tool("hrc" HUXERUI_RESOURCE_COMPILER_COMMAND)
    add_custom_command(
            OUTPUT "${HUXERUI_RESOURCE_INDEX}"
            COMMAND ${CMAKE_COMMAND}
                    "-DHUXERUI_HRC=${HUXERUI_RESOURCE_COMPILER_COMMAND}"
                    "-DHUXERUI_RESOURCE_PACKAGES=${HUXERUI_RESOURCE_PACKAGES}"
                    "-DHUXERUI_RESOURCE_ROOTS=${HUXERUI_RESOURCE_ROOTS}"
                    "-DHUXERUI_RESOURCE_NAMESPACES=${HUXERUI_RESOURCE_NAMESPACES}"
                    "-DHUXERUI_RESOURCE_OUTPUT=${HUXERUI_RESOURCE_OUTPUT}"
                    -P "${HUXERUI_RESOURCE_DRIVER}"
            DEPENDS
                    ${HUXERUI_RESOURCE_PACKAGE_INDEXES}
                    ${HUXERUI_RESOURCE_INPUTS}
                    "${HUXERUI_RESOURCE_PLAN}"
                    "${HUXERUI_RESOURCE_COMPILER_COMMAND}"
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
    elseif (WIN32 OR (UNIX AND NOT APPLE))
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
