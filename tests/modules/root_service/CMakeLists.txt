huxerui_add_module(huxerui_test_module
        SOURCES
            src/module.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_test_module
)

target_include_directories(huxerui_test_module PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
add_library(HuxerUI::test_module ALIAS huxerui_test_module)
