huxerui_add_example(
        example_custom_editor
        "HuxerUI Custom Editor"
        "com.huxerui.custom-editor"
)

huxerui_add_resources(example_custom_editor
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        NAMESPACE "custom_editor"
)
