huxerui_add_library(ctskit_widgets
        SOURCES
            src/widgets.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            ctskit_widgets
)

add_library(CtsKit::Widgets ALIAS ctskit_widgets)

huxerui_use_library(ctskit_widgets
        TARGET CtsKit::Core
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../core"
)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(ctskit_widgets PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
