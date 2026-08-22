include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/HuxerUIApp.cmake")

function(huxerui_add_example target_name bundle_name bundle_identifier)
    set(HUXERUI_EXAMPLE_SOURCES main.cpp)
    if (NOT IOS AND NOT ANDROID AND NOT EMSCRIPTEN)
        list(APPEND HUXERUI_EXAMPLE_SOURCES
                "${HUXERUI_PROJECT_DIR}/examples/main.cpp"
        )
    endif ()

    set(HUXERUI_EXAMPLE_RESOURCE_OUTPUT_ARGUMENTS)
    if (ANDROID AND HUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT)
        list(APPEND HUXERUI_EXAMPLE_RESOURCE_OUTPUT_ARGUMENTS
                RESOURCE_OUTPUT_DIRECTORY
                "${HUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT}/${ANDROID_ABI}"
        )
    endif ()

    huxerui_add_app(${target_name}
            SOURCES ${HUXERUI_EXAMPLE_SOURCES}
            BUNDLE_NAME "${bundle_name}"
            BUNDLE_IDENTIFIER "${bundle_identifier}"
            ${HUXERUI_EXAMPLE_RESOURCE_OUTPUT_ARGUMENTS}
    )

    # Android Prefab exports native targets but not the built-in resource package consumed by source-tree examples.
    if (ANDROID AND (NOT HUXERUI_BUILTIN_RESOURCE_PACKAGE
            OR NOT EXISTS "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}/huxerui/resources.bin"))
        huxerui_add_resources(${target_name}
                ROOT "${HUXERUI_PROJECT_DIR}/resources"
                NAMESPACE huxerui
        )
    endif ()

    if (EMSCRIPTEN)
        set_target_properties(${target_name} PROPERTIES SUFFIX ".mjs")
        set(HUXERUI_WEB_MODULE_FILE "${target_name}.mjs")
        set(HUXERUI_WEB_STORAGE_KEY "${bundle_identifier}")
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
    endif ()
endfunction()
