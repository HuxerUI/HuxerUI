huxerui_add_example(
        example_application
        "HuxerUI Application"
        "com.huxerui.application"
)

if (WIN32)
    target_compile_definitions(example_application PRIVATE HUXERUI_EXAMPLE_WINDOWS_URL_SCHEME=1)
endif ()

if (APPLE AND NOT IOS)
    set_target_properties(example_application PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    )
endif ()
