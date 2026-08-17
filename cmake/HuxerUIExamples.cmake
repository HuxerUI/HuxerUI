include_guard(GLOBAL)

function(huxerui_add_example target_name bundle_name bundle_identifier)
    if (IOS)
        huxerui_add_app(${target_name}
                SOURCES main.cpp
                BUNDLE_NAME "${bundle_name}"
                BUNDLE_IDENTIFIER "${bundle_identifier}"
        )
        return()
    elseif (ANDROID)
        add_library(${target_name} SHARED
                main.cpp
        )
        set_target_properties(${target_name} PROPERTIES
                OUTPUT_NAME "huxerui_app"
        )
        target_link_libraries(${target_name} PRIVATE HuxerUI::huxerui)
    else ()
        add_executable(${target_name}
                main.cpp
        )
        if (NOT EMSCRIPTEN)
            target_sources(${target_name} PRIVATE
                    "${HUXERUI_PROJECT_DIR}/examples/main.cpp"
            )
        endif ()

        if (TARGET HuxerUI::huxerui_static)
            target_link_libraries(${target_name} PRIVATE HuxerUI::huxerui_static)
        else ()
            target_link_libraries(${target_name} PRIVATE HuxerUI::huxerui)
        endif ()

        if (EMSCRIPTEN)
            set_target_properties(${target_name} PROPERTIES SUFFIX ".mjs")
            set(HUXERUI_WEB_MODULE_FILE "${target_name}.mjs")
            configure_file(
                    "${HUXERUI_PROJECT_DIR}/platform/web/example.html.in"
                    "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.html"
                    @ONLY
            )
            get_target_property(HUXERUI_WEB_OUTPUT_DIRECTORY ${target_name} RUNTIME_OUTPUT_DIRECTORY)
            if (NOT HUXERUI_WEB_OUTPUT_DIRECTORY)
                set(HUXERUI_WEB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
            endif ()
            set(HUXERUI_WEB_ENTRY_FILE "${HUXERUI_WEB_OUTPUT_DIRECTORY}/${target_name}.html")
            add_custom_command(OUTPUT "${HUXERUI_WEB_ENTRY_FILE}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${HUXERUI_WEB_OUTPUT_DIRECTORY}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.html"
                            "${HUXERUI_WEB_ENTRY_FILE}"
                    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.html"
                    VERBATIM
            )
            add_custom_target(${target_name}_web_entry DEPENDS "${HUXERUI_WEB_ENTRY_FILE}")
            add_dependencies(${target_name} ${target_name}_web_entry)
        elseif (APPLE)
            set_target_properties(${target_name} PROPERTIES
                    MACOSX_BUNDLE TRUE
                    MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
                    MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_identifier}"
            )
        endif ()
    endif ()

    if (ANDROID AND NOT HUXERUI_BUILTIN_RESOURCE_PACKAGE)
        _huxerui_configure_builtin_resources(${target_name})
    endif ()

    target_compile_features(${target_name} PRIVATE cxx_std_20)
    huxerui_enable_codegen(${target_name})
    set_property(TARGET ${target_name} APPEND PROPERTY
            HUXERUI_RESOURCE_PACKAGES
            "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}"
    )
    _huxerui_schedule_resources(${target_name})
endfunction()
