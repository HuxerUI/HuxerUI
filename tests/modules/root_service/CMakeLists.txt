huxerui_add_module(huxerui_test_module
        SOURCES
            src/module.cpp
)

target_include_directories(huxerui_test_module PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
huxerui_add_resources(huxerui_test_module
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        NAMESPACE huxerui_test_module
)

add_library(HuxerUI::test_module ALIAS huxerui_test_module)
