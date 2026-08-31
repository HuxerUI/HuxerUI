huxerui_add_example(
        example_visual_effects
        "HuxerUI Visual Effects"
        "com.huxerui.visual-effects"
)

target_sources(example_visual_effects PRIVATE advanced_effects.cpp spotlight_hover.cpp)

huxerui_add_resources(example_visual_effects
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        NAMESPACE "visual_effects"
)
