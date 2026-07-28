function(huxerui_platform_configure)
    set(HUXERUI_PLATFORM_SOURCE_FILES
            "${HUXERUI_PROJECT_DIR}/platform/macos/appkit_host.mm"
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_COMPILE_OPTIONS
            -fobjc-arc
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_LINK_LIBRARIES
            "-framework AppKit"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework QuartzCore"
            PARENT_SCOPE
    )
endfunction()
