# The library's CMake declaration, which a CMake application reaches through
# huxerui_use_library; mcpp.toml beside it declares the same library to mcpp.
# Neither changes the sources, and only the mcpp build adds the module.
huxerui_add_library(gauge
        SOURCES
            src/gauge.cpp
)

add_library(Gauge::Gauge ALIAS gauge)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(gauge PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
