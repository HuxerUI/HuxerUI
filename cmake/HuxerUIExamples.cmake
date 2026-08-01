include_guard(GLOBAL)

function(huxerui_add_example target_name bundle_name bundle_identifier)
    if (ANDROID)
        add_library(${target_name} SHARED
                main.cpp
        )
        set_target_properties(${target_name} PROPERTIES
                OUTPUT_NAME "huxerui_app"
        )
        target_link_libraries(${target_name} PRIVATE huxerui::huxerui)
    else ()
        add_executable(${target_name}
                main.cpp
        )

        if (TARGET HuxerUI::huxerui_static)
            target_link_libraries(${target_name} PRIVATE HuxerUI::huxerui_static)
        else ()
            target_link_libraries(${target_name} PRIVATE HuxerUI::huxerui)
        endif ()

        set_target_properties(${target_name} PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
                MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_identifier}"
        )
    endif ()

    target_compile_features(${target_name} PRIVATE cxx_std_20)
    huxerui_enable_codegen(${target_name})
endfunction()
