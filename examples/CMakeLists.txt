function(huxerui_add_example target_name bundle_name bundle_identifier)
    add_executable(${target_name}
            main.cpp
    )

    if (TARGET HuxerUI::huxerui_static)
        target_link_libraries(${target_name} PRIVATE HuxerUI::huxerui_static)
    else ()
        target_link_libraries(${target_name} PRIVATE HuxerUI::huxerui)
    endif ()

    huxerui_enable_codegen(${target_name})

    set_target_properties(${target_name} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_identifier}"
    )
endfunction()

add_subdirectory(counter)
add_subdirectory(layout_gallery)
add_subdirectory(dynamic_list)
add_subdirectory(scroll_view)
add_subdirectory(virtual_list)
add_subdirectory(horizontal_virtual_list)
add_subdirectory(virtual_grid)
add_subdirectory(scroll_state)
add_subdirectory(custom_event)
add_subdirectory(toast)
add_subdirectory(dialog)
add_subdirectory(theme)
