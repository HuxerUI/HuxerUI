include_guard(GLOBAL)

function(_huxerui_escape_json input output)
    string(REPLACE "\\" "\\\\" value "${input}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(huxerui_add_app target_name)
    cmake_parse_arguments(HUXERUI_APP
            ""
            "RESOURCE_NAMESPACE;BUNDLE_NAME;BUNDLE_IDENTIFIER"
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
    if (HUXERUI_APP_RESOURCES)
        list(GET HUXERUI_APP_RESOURCES 0 HUXERUI_APP_RESOURCE_ROOT)
        huxerui_add_resources(${target_name}
                ROOT "${HUXERUI_APP_RESOURCE_ROOT}"
                NAMESPACE "${HUXERUI_APP_RESOURCE_NAMESPACE}"
        )
    endif ()

    if (IOS)
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
                ${HUXERUI_APP_FRAMEWORK_TARGET}
                INTERFACE_LINK_LIBRARIES
        )
        get_target_property(HUXERUI_IOS_LINK_OPTIONS
                ${HUXERUI_APP_FRAMEWORK_TARGET}
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
        add_custom_command(
                OUTPUT "${HUXERUI_IOS_CORE_ARCHIVE}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${HUXERUI_IOS_CORE_DIRECTORY}"
                COMMAND "${HUXERUI_IOS_LIBTOOL}" -static
                        -o "${HUXERUI_IOS_CORE_ARCHIVE}"
                        "$<TARGET_FILE:${target_name}>"
                        "$<TARGET_FILE:${HUXERUI_APP_FRAMEWORK_TARGET}>"
                DEPENDS
                        ${target_name}
                        ${HUXERUI_APP_FRAMEWORK_TARGET}
                COMMENT "Linking HuxerUI iOS application core ${target_name}"
                VERBATIM
        )
        add_custom_target(${target_name}_huxerui_ios_core
                DEPENDS "${HUXERUI_IOS_CORE_ARCHIVE}"
        )
        return()
    endif ()

    if (NOT HUXERUI_PLATFORM_ID)
        if (EMSCRIPTEN)
            set(HUXERUI_PLATFORM_ID "web")
        elseif (ANDROID)
            set(HUXERUI_PLATFORM_ID "android")
        elseif (APPLE)
            set(HUXERUI_PLATFORM_ID "macos")
        elseif (WIN32)
            set(HUXERUI_PLATFORM_ID "windows")
        else ()
            set(HUXERUI_PLATFORM_ID "generic")
        endif ()
    endif ()

    _huxerui_escape_json("${target_name}" HUXERUI_APP_JSON_TARGET)
    _huxerui_escape_json("${HUXERUI_PLATFORM_ID}" HUXERUI_APP_JSON_PLATFORM)
    _huxerui_escape_json("${HUXERUI_APP_BUNDLE_IDENTIFIER}" HUXERUI_APP_JSON_BUNDLE_IDENTIFIER)

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
