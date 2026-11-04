function(huxerui_configure_web_app target_name)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR "huxerui_configure_web_app() target does not exist: ${target_name}")
    endif ()

    set_target_properties(${target_name} PROPERTIES SUFFIX ".js")
    set(HUXERUI_WEB_MODULE_FILE "${target_name}.js")
    set(HUXERUI_WEB_STORAGE_KEY "@PROJECT_ID@")
    set(HUXERUI_WEB_ENTRY_FILE "${target_name}.html")
    set(HUXERUI_WEB_FAVICON_FILE "${target_name}.favicon.svg")
    set(HUXERUI_WEB_APPLE_TOUCH_ICON_FILE "${target_name}.apple-touch-icon.png")
    set(HUXERUI_WEB_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/huxerui-web")
    file(MAKE_DIRECTORY "${HUXERUI_WEB_GENERATED_DIRECTORY}")
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/index.html.in"
            "${HUXERUI_WEB_GENERATED_DIRECTORY}/${target_name}.html"
            @ONLY
    )
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/favicon.svg"
            "${HUXERUI_WEB_GENERATED_DIRECTORY}/${HUXERUI_WEB_FAVICON_FILE}"
            COPYONLY
    )
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apple-touch-icon.png"
            "${HUXERUI_WEB_GENERATED_DIRECTORY}/${HUXERUI_WEB_APPLE_TOUCH_ICON_FILE}"
            COPYONLY
    )
    # Shell-only edits and deleted output files must not require relinking the application.
    add_custom_target(${target_name}_web_assets
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HUXERUI_WEB_GENERATED_DIRECTORY}/${target_name}.html"
                    "${HUXERUI_WEB_GENERATED_DIRECTORY}/${HUXERUI_WEB_FAVICON_FILE}"
                    "${HUXERUI_WEB_GENERATED_DIRECTORY}/${HUXERUI_WEB_APPLE_TOUCH_ICON_FILE}"
                    "$<TARGET_FILE_DIR:${target_name}>"
            VERBATIM
    )
    add_dependencies(${target_name} ${target_name}_web_assets)
endfunction()
