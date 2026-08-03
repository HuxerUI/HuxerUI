function(huxerui_platform_configure)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(HUXERUI_X11 REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(HUXERUI_XEXT REQUIRED IMPORTED_TARGET xext)
    pkg_check_modules(HUXERUI_XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
    pkg_check_modules(HUXERUI_VULKAN REQUIRED IMPORTED_TARGET vulkan)
    pkg_check_modules(HUXERUI_CAIRO REQUIRED IMPORTED_TARGET cairo)
    pkg_check_modules(HUXERUI_FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(HUXERUI_HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(HUXERUI_FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(HUXERUI_PNG REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(HUXERUI_JPEG REQUIRED IMPORTED_TARGET libjpeg)
    pkg_check_modules(HUXERUI_XRANDR REQUIRED IMPORTED_TARGET xrandr)

    set(HUXERUI_PLATFORM_SOURCE_FILES
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_adapter.cpp"
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_renderer.cpp"
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_text_input.cpp"
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_INCLUDE_DIRECTORIES
            ${HUXERUI_X11_INCLUDE_DIRS}
            ${HUXERUI_XKBCOMMON_INCLUDE_DIRS}
            ${HUXERUI_VULKAN_INCLUDE_DIRS}
            ${HUXERUI_CAIRO_INCLUDE_DIRS}
            ${HUXERUI_FREETYPE_INCLUDE_DIRS}
            ${HUXERUI_HARFBUZZ_INCLUDE_DIRS}
            ${HUXERUI_FONTCONFIG_INCLUDE_DIRS}
            ${HUXERUI_PNG_INCLUDE_DIRS}
            ${HUXERUI_JPEG_INCLUDE_DIRS}
            ${HUXERUI_XRANDR_INCLUDE_DIRS}
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_LINK_LIBRARIES
            PkgConfig::HUXERUI_X11
            PkgConfig::HUXERUI_XEXT
            PkgConfig::HUXERUI_XKBCOMMON
            PkgConfig::HUXERUI_VULKAN
            PkgConfig::HUXERUI_CAIRO
            PkgConfig::HUXERUI_FREETYPE
            PkgConfig::HUXERUI_HARFBUZZ
            PkgConfig::HUXERUI_FONTCONFIG
            PkgConfig::HUXERUI_PNG
            PkgConfig::HUXERUI_JPEG
            PkgConfig::HUXERUI_XRANDR
            PARENT_SCOPE
    )
endfunction()
