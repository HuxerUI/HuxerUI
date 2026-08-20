function(huxerui_configure_web_app target_name)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR "huxerui_configure_web_app() target does not exist: ${target_name}")
    endif ()

    set_target_properties(${target_name} PROPERTIES SUFFIX ".mjs")
    set(HUXERUI_WEB_MODULE_FILE "${target_name}.mjs")
    set(HUXERUI_WEB_STORAGE_KEY "@PROJECT_ID@")
    set(HUXERUI_WEB_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/huxerui-web")
    file(MAKE_DIRECTORY "${HUXERUI_WEB_GENERATED_DIRECTORY}")
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/index.html.in"
            "${HUXERUI_WEB_GENERATED_DIRECTORY}/${target_name}.html"
            @ONLY
    )
    add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HUXERUI_WEB_GENERATED_DIRECTORY}/${target_name}.html"
                    "$<TARGET_FILE_DIR:${target_name}>/${target_name}.html"
            VERBATIM
    )
endfunction()
