huxerui_add_library(huxerui_test_library
        SOURCES
            src/library.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_test_library
)

target_include_directories(huxerui_test_library PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
add_library(HuxerUI::test_library ALIAS huxerui_test_library)
