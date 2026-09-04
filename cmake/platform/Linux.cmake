find_package(PkgConfig REQUIRED)

# GTK owns the native display connection, X11/Wayland selection, event loop,
# input method integration, clipboard, and presentation
# surface. Pango and Cairo arrive through GTK's public pkg-config closure.
pkg_check_modules(HUXERUI_GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.14)
pkg_check_modules(HUXERUI_EPOXY REQUIRED IMPORTED_TARGET epoxy>=1.5)
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
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_file_drop.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_http.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_renderer.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_system_tray.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_text_input.cpp"
        "${HUXERUI_PROJECT_DIR}/platform/linux/linux_ui_dispatcher.cpp"
)
set(HUXERUI_PLATFORM_INCLUDE_DIRECTORIES
        ${HUXERUI_GTK4_INCLUDE_DIRS}
        ${HUXERUI_EPOXY_INCLUDE_DIRS}
        ${HUXERUI_GIO_INCLUDE_DIRS}
        ${HUXERUI_LIBSOUP_INCLUDE_DIRS}
)
set(HUXERUI_PLATFORM_COMPILE_OPTIONS
        ${HUXERUI_GTK4_CFLAGS_OTHER}
        ${HUXERUI_EPOXY_CFLAGS_OTHER}
        ${HUXERUI_GIO_CFLAGS_OTHER}
        ${HUXERUI_LIBSOUP_CFLAGS_OTHER}
)
set(HUXERUI_PLATFORM_LINK_LIBRARIES
        PkgConfig::HUXERUI_GTK4
        PkgConfig::HUXERUI_EPOXY
        PkgConfig::HUXERUI_GIO
        PkgConfig::HUXERUI_LIBSOUP
)
