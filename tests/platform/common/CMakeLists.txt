string(TOLOWER "${CMAKE_SYSTEM_NAME}" filesystem_platform)
if (EMSCRIPTEN)
    set(filesystem_platform web)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(filesystem_platform macos)
endif ()
huxerui_add_test_suite(huxerui_filesystem_tests HuxerUIFileSystemTests
        LABELS platform ${filesystem_platform} native filesystem
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES file.cpp file_reference.cpp
                "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_include_directories(huxerui_filesystem_tests PRIVATE "${HUXERUI_TEST_DIR}/platform/support")
target_compile_definitions(huxerui_filesystem_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)
if (WIN32)
    target_sources(huxerui_filesystem_tests PRIVATE ../windows/file_reference.cpp ../windows/file_uri.cpp)
else ()
    target_sources(huxerui_filesystem_tests PRIVATE file_reference_import.cpp ../posix/file_uri.cpp)
    if (NOT EMSCRIPTEN)
        target_sources(huxerui_filesystem_tests PRIVATE ../posix/file_reference.cpp)
    endif ()
endif ()
