huxerui_add_example(
        example_application
        "HuxerUI Application"
        "com.huxerui.application"
)

if (WIN32)
    target_sources(example_application PRIVATE application_windows.cpp)
    target_link_libraries(example_application PRIVATE advapi32)
endif ()
