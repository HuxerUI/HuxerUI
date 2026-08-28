find_package(PkgConfig REQUIRED)
find_package(SDL3 3.4 REQUIRED CONFIG)
find_package(SDL3_image 3.4 REQUIRED CONFIG)
find_package(SDL3_ttf 3.2 REQUIRED CONFIG)

# SDL owns the native display connection, X11/Wayland selection, event loop,
# input method integration, clipboard, and presentation. HuxerUI rasterizes
# PaintCommands into a CPU backbuffer; SDL_image and SDL_ttf provide decoding
# and text shaping/rasterization.
pkg_check_modules(HUXERUI_GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(HUXERUI_LIBSOUP QUIET IMPORTED_TARGET libsoup-3.0>=3.0)
if (NOT TARGET PkgConfig::HUXERUI_LIBSOUP)
    message(FATAL_ERROR
            "HuxerUI Linux HTTP requires the system libsoup 3 development package (minimum 3.0).")
endif ()

set(HUXERUI_PLATFORM_SOURCE_FILES
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_adapter.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_external_texture.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_file.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_file_picker.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_http.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_renderer.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_system_tray.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_text_renderer.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_text_input.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_ui_dispatcher.cpp"
)
set(HUXERUI_PLATFORM_INCLUDE_DIRECTORIES
        $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:SDL3_image::SDL3_image,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:SDL3_ttf::SDL3_ttf,INTERFACE_INCLUDE_DIRECTORIES>
        ${HUXERUI_GIO_INCLUDE_DIRS}
        ${HUXERUI_LIBSOUP_INCLUDE_DIRS}
)
set(HUXERUI_PLATFORM_COMPILE_OPTIONS
        ${HUXERUI_GIO_CFLAGS_OTHER}
        ${HUXERUI_LIBSOUP_CFLAGS_OTHER}
)
set(HUXERUI_PLATFORM_LINK_LIBRARIES
        SDL3::SDL3
        SDL3_image::SDL3_image
        SDL3_ttf::SDL3_ttf
        PkgConfig::HUXERUI_GIO
        PkgConfig::HUXERUI_LIBSOUP
)
