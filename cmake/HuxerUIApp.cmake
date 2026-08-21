include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/HuxerUIModules.cmake")

function(_huxerui_configure_ios_app_core target_name)
    get_property(HUXERUI_IOS_APP_FRAMEWORK_TARGET
            TARGET ${target_name}
            PROPERTY HUXERUI_APP_FRAMEWORK_TARGET
    )
    get_property(HUXERUI_IOS_MODULE_TARGETS
            TARGET ${target_name}
            PROPERTY HUXERUI_MODULES
    )
    find_program(HUXERUI_IOS_LIBTOOL libtool REQUIRED)
    set(HUXERUI_IOS_CORE_DIRECTORY
            "${CMAKE_BINARY_DIR}/huxerui-ios/${target_name}"
    )
    set(HUXERUI_IOS_CORE_ARCHIVE
            "${HUXERUI_IOS_CORE_DIRECTORY}/lib${target_name}_huxerui.a"
    )
    set(HUXERUI_IOS_LINK_OPTIONS_FILE
            "${HUXERUI_IOS_CORE_DIRECTORY}/link.rsp"
    )
    get_target_property(HUXERUI_IOS_LINK_LIBRARIES
            ${HUXERUI_IOS_APP_FRAMEWORK_TARGET}
            INTERFACE_LINK_LIBRARIES
    )
    get_target_property(HUXERUI_IOS_LINK_OPTIONS
            ${HUXERUI_IOS_APP_FRAMEWORK_TARGET}
            INTERFACE_LINK_OPTIONS
    )
    if (NOT HUXERUI_IOS_LINK_LIBRARIES
            OR HUXERUI_IOS_LINK_LIBRARIES MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
                "huxerui_add_app() requires iOS platform link arguments"
        )
    endif ()
    if (HUXERUI_IOS_LINK_OPTIONS MATCHES "-NOTFOUND$")
        set(HUXERUI_IOS_LINK_OPTIONS)
    endif ()
    set(HUXERUI_IOS_LINK_OPTIONS_CONTENT
            "-force_load\n\"${HUXERUI_IOS_CORE_ARCHIVE}\"\n"
    )
    foreach (HUXERUI_IOS_LINK_ARGUMENT IN LISTS
            HUXERUI_IOS_LINK_LIBRARIES
            HUXERUI_IOS_LINK_OPTIONS
    )
        string(APPEND HUXERUI_IOS_LINK_OPTIONS_CONTENT
                "${HUXERUI_IOS_LINK_ARGUMENT}\n"
        )
    endforeach ()
    file(MAKE_DIRECTORY "${HUXERUI_IOS_CORE_DIRECTORY}")
    file(WRITE "${HUXERUI_IOS_LINK_OPTIONS_FILE}"
            "${HUXERUI_IOS_LINK_OPTIONS_CONTENT}"
    )

    set(HUXERUI_IOS_CORE_INPUTS
            "$<TARGET_FILE:${target_name}>"
            "$<TARGET_FILE:${HUXERUI_IOS_APP_FRAMEWORK_TARGET}>"
    )
    set(HUXERUI_IOS_CORE_DEPENDENCIES
            ${target_name}
            ${HUXERUI_IOS_APP_FRAMEWORK_TARGET}
    )
    foreach (HUXERUI_IOS_MODULE_TARGET IN LISTS HUXERUI_IOS_MODULE_TARGETS)
        list(APPEND HUXERUI_IOS_CORE_INPUTS
                "$<TARGET_FILE:${HUXERUI_IOS_MODULE_TARGET}>"
        )
        list(APPEND HUXERUI_IOS_CORE_DEPENDENCIES
                ${HUXERUI_IOS_MODULE_TARGET}
        )
    endforeach ()

    add_custom_command(
            OUTPUT "${HUXERUI_IOS_CORE_ARCHIVE}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${HUXERUI_IOS_CORE_DIRECTORY}"
            COMMAND "${HUXERUI_IOS_LIBTOOL}" -static
                    -o "${HUXERUI_IOS_CORE_ARCHIVE}"
                    ${HUXERUI_IOS_CORE_INPUTS}
            DEPENDS ${HUXERUI_IOS_CORE_DEPENDENCIES}
            COMMENT "Linking HuxerUI iOS application core ${target_name}"
            VERBATIM
    )
    add_custom_target(${target_name}_huxerui_ios_core
            DEPENDS "${HUXERUI_IOS_CORE_ARCHIVE}"
    )
endfunction()

function(_huxerui_configure_scheduled_ios_app_cores)
    get_property(HUXERUI_IOS_APP_TARGETS
            DIRECTORY
            PROPERTY HUXERUI_IOS_APP_TARGETS
    )
    foreach (HUXERUI_IOS_APP_TARGET IN LISTS HUXERUI_IOS_APP_TARGETS)
        _huxerui_configure_ios_app_core(${HUXERUI_IOS_APP_TARGET})
    endforeach ()
endfunction()

function(_huxerui_schedule_ios_app_core target_name)
    get_target_property(HUXERUI_IOS_APP_SOURCE_DIRECTORY
            ${target_name}
            SOURCE_DIR
    )
    set_property(DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}" APPEND PROPERTY
            HUXERUI_IOS_APP_TARGETS
            "${target_name}"
    )
    get_property(HUXERUI_IOS_APP_CALLBACK_SCHEDULED
            DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}"
            PROPERTY HUXERUI_IOS_APP_CALLBACK_SCHEDULED
    )
    if (NOT HUXERUI_IOS_APP_CALLBACK_SCHEDULED)
        set_property(DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}" PROPERTY
                HUXERUI_IOS_APP_CALLBACK_SCHEDULED TRUE
        )
        cmake_language(DEFER
                DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}"
                CALL _huxerui_configure_scheduled_ios_app_cores
        )
    endif ()
endfunction()

function(huxerui_add_app target_name)
    cmake_parse_arguments(HUXERUI_APP
            ""
            "RESOURCE_NAMESPACE;RESOURCE_OUTPUT_DIRECTORY;BUNDLE_NAME;BUNDLE_IDENTIFIER"
            "SOURCES;RESOURCES"
            ${ARGN}
    )

    if (TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_add_app() target already exists: ${target_name}"
        )
    endif ()
    if (NOT HUXERUI_APP_SOURCES)
        message(FATAL_ERROR
                "huxerui_add_app() requires at least one source"
        )
    endif ()
    if (HUXERUI_APP_RESOURCES AND NOT HUXERUI_APP_RESOURCE_NAMESPACE)
        message(FATAL_ERROR
                "huxerui_add_app() requires RESOURCE_NAMESPACE when RESOURCES is present"
        )
    endif ()
    list(LENGTH HUXERUI_APP_RESOURCES HUXERUI_APP_RESOURCE_ROOT_COUNT)
    if (HUXERUI_APP_RESOURCE_ROOT_COUNT GREATER 1)
        message(FATAL_ERROR
                "huxerui_add_app() currently accepts one resource root"
        )
    endif ()

    if (HUXERUI_MODULE_GRAPH_ONLY)
        if (NOT HUXERUI_MODULE_GRAPH_OUTPUT)
            message(FATAL_ERROR
                    "HUXERUI_MODULE_GRAPH_ONLY requires HUXERUI_MODULE_GRAPH_OUTPUT"
            )
        endif ()
        add_library(${target_name} INTERFACE)
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_MODULE_GRAPH_ONLY TRUE
        )
        get_filename_component(HUXERUI_APP_MODULE_GRAPH_OUTPUT
                "${HUXERUI_MODULE_GRAPH_OUTPUT}"
                ABSOLUTE
                BASE_DIR "${CMAKE_BINARY_DIR}"
        )
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_MODULE_GRAPH_OUTPUT
                "${HUXERUI_APP_MODULE_GRAPH_OUTPUT}"
        )
        _huxerui_write_module_graph(
                ${target_name}
                "${HUXERUI_APP_MODULE_GRAPH_OUTPUT}"
        )
        return()
    endif ()

    if (NOT HUXERUI_PLATFORM_ID AND TARGET HuxerUI::huxerui)
        get_target_property(HUXERUI_PLATFORM_ID
                HuxerUI::huxerui
                HUXERUI_PLATFORM_ID
        )
    endif ()
    if (NOT HUXERUI_PLATFORM_ID OR HUXERUI_PLATFORM_ID MATCHES "-NOTFOUND$")
        message(FATAL_ERROR "huxerui_add_app() requires a configured HuxerUI platform")
    endif ()

    if (IOS)
        add_library(${target_name} STATIC ${HUXERUI_APP_SOURCES})
        set_target_properties(${target_name} PROPERTIES
                ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        )
    elseif (ANDROID)
        add_library(${target_name} SHARED ${HUXERUI_APP_SOURCES})
        set_target_properties(${target_name} PROPERTIES
                OUTPUT_NAME "huxerui_app"
        )
    else ()
        add_executable(${target_name} ${HUXERUI_APP_SOURCES})
    endif ()

    if (TARGET HuxerUI::huxerui_static AND NOT ANDROID)
        set(HUXERUI_APP_FRAMEWORK_TARGET HuxerUI::huxerui_static)
    elseif (TARGET HuxerUI::huxerui)
        set(HUXERUI_APP_FRAMEWORK_TARGET HuxerUI::huxerui)
    else ()
        message(FATAL_ERROR
                "huxerui_add_app() requires the HuxerUI::huxerui target"
        )
    endif ()

    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_link_libraries(${target_name} PRIVATE
            ${HUXERUI_APP_FRAMEWORK_TARGET}
    )

    if (HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY)
        get_filename_component(HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY
                "${HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY}"
                ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}"
        )
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_RESOURCE_OUTPUT_DIRECTORY
                "${HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY}"
        )
    endif ()

    if (IOS)
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_RESOURCE_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/huxerui-ios/${target_name}/resources"
        )
    endif ()

    if (APPLE AND NOT IOS)
        set_target_properties(${target_name} PROPERTIES MACOSX_BUNDLE TRUE)
        if (HUXERUI_APP_BUNDLE_NAME)
            set_target_properties(${target_name} PROPERTIES
                    MACOSX_BUNDLE_BUNDLE_NAME "${HUXERUI_APP_BUNDLE_NAME}"
            )
        endif ()
        if (HUXERUI_APP_BUNDLE_IDENTIFIER)
            set_target_properties(${target_name} PROPERTIES
                    MACOSX_BUNDLE_GUI_IDENTIFIER "${HUXERUI_APP_BUNDLE_IDENTIFIER}"
            )
        endif ()
    endif ()

    huxerui_enable_codegen(${target_name})
    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_RESOURCE_PACKAGES
            "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}"
    )
    _huxerui_schedule_resources(${target_name})
    if (HUXERUI_APP_RESOURCES)
        list(GET HUXERUI_APP_RESOURCES 0 HUXERUI_APP_RESOURCE_ROOT)
        huxerui_add_resources(${target_name}
                ROOT "${HUXERUI_APP_RESOURCE_ROOT}"
                NAMESPACE "${HUXERUI_APP_RESOURCE_NAMESPACE}"
        )
    endif ()

    if (HUXERUI_MODULE_GRAPH_OUTPUT)
        get_filename_component(HUXERUI_APP_MODULE_GRAPH_OUTPUT
                "${HUXERUI_MODULE_GRAPH_OUTPUT}"
                ABSOLUTE
                BASE_DIR "${CMAKE_BINARY_DIR}"
        )
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_MODULE_GRAPH_OUTPUT
                "${HUXERUI_APP_MODULE_GRAPH_OUTPUT}"
        )
        _huxerui_write_module_graph(
                ${target_name}
                "${HUXERUI_APP_MODULE_GRAPH_OUTPUT}"
        )
    endif ()

    if (IOS)
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_APP_FRAMEWORK_TARGET
                "${HUXERUI_APP_FRAMEWORK_TARGET}"
        )
        _huxerui_schedule_ios_app_core(${target_name})
        return()
    endif ()

    _huxerui_json_escape("${target_name}" HUXERUI_APP_JSON_TARGET)
    _huxerui_json_escape("${HUXERUI_PLATFORM_ID}" HUXERUI_APP_JSON_PLATFORM)
    _huxerui_json_escape("${HUXERUI_APP_BUNDLE_IDENTIFIER}" HUXERUI_APP_JSON_BUNDLE_IDENTIFIER)

    set(HUXERUI_APP_INTEGRATION_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/huxerui-integration/${target_name}"
    )
    set(HUXERUI_APP_INTEGRATION_PLAN
            "${HUXERUI_APP_INTEGRATION_DIRECTORY}/$<CONFIG>/app.json"
    )
    set(HUXERUI_APP_BUNDLE_PATH)
    if (APPLE)
        set(HUXERUI_APP_BUNDLE_PATH "$<TARGET_BUNDLE_DIR:${target_name}>")
    endif ()
    file(MAKE_DIRECTORY "${HUXERUI_APP_INTEGRATION_DIRECTORY}")
    file(GENERATE
            OUTPUT "${HUXERUI_APP_INTEGRATION_PLAN}"
            CONTENT "{\n  \"schema\": 1,\n  \"target\": \"${HUXERUI_APP_JSON_TARGET}\",\n  \"platform\": \"${HUXERUI_APP_JSON_PLATFORM}\",\n  \"artifact\": \"$<TARGET_FILE:${target_name}>\",\n  \"bundle\": \"${HUXERUI_APP_BUNDLE_PATH}\",\n  \"bundleIdentifier\": \"${HUXERUI_APP_JSON_BUNDLE_IDENTIFIER}\"\n}\n"
    )
endfunction()
