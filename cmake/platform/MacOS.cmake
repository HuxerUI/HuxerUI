function(huxerui_platform_configure)
    set(HUXERUI_PLATFORM_SOURCE_FILES
            "${HUXERUI_PROJECT_DIR}/platform/macos/appkit_accessibility.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/appkit_adapter.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/appkit_platform_view.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/appkit_renderer.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/appkit_text_input.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/macos_external_texture.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/macos_file.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/macos_file_picker.mm"
            "${HUXERUI_PROJECT_DIR}/platform/macos/macos_http.mm"
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_COMPILE_OPTIONS
            -fobjc-arc
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_LINK_LIBRARIES
            "-framework AppKit"
            "-framework Carbon"
            "-framework CoreGraphics"
            "-framework CoreImage"
            "-framework CoreText"
            "-framework CoreVideo"
            "-framework ImageIO"
            "-framework Foundation"
            "-framework QuartzCore"
            "-weak_framework UniformTypeIdentifiers"
            PARENT_SCOPE
    )
endfunction()
