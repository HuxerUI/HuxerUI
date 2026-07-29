if (TARGET HuxerUI3p::Catch2)
    return()
endif ()

add_library(huxerui_3p_catch2
        ${CMAKE_CURRENT_LIST_DIR}/src/catch_amalgamated.cpp
)

target_compile_features(huxerui_3p_catch2 PUBLIC cxx_std_14)
target_compile_definitions(huxerui_3p_catch2 PUBLIC CATCH_AMALGAMATED_CUSTOM_MAIN=ON)

target_include_directories(huxerui_3p_catch2
        PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
        PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/include/catch2
)

add_library(HuxerUI3p::Catch2 ALIAS huxerui_3p_catch2)
