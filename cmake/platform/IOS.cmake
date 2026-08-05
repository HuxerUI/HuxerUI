function(huxerui_platform_configure)
    set(HUXERUI_PLATFORM_SOURCE_FILES
            "${HUXERUI_PROJECT_DIR}/platform/ios/uikit_adapter.mm"
            "${HUXERUI_PROJECT_DIR}/platform/ios/uikit_renderer.mm"
            "${HUXERUI_PROJECT_DIR}/platform/ios/uikit_text_input.mm"
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_COMPILE_OPTIONS
            -fobjc-arc
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_LINK_LIBRARIES
            "-framework CoreFoundation"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework Foundation"
            "-framework ImageIO"
            "-framework QuartzCore"
            "-framework UIKit"
            PARENT_SCOPE
    )
endfunction()
