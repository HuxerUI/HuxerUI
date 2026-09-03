set(HUXERUI_MACOS_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in")

function(huxerui_configure_macos_project_package target_name install_component)
    if (HUXERUI_PACKAGE)
        install(TARGETS ${target_name}
                BUNDLE DESTINATION .
                COMPONENT "${install_component}"
        )
    endif ()
endfunction()
