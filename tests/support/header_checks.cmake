set(HUXERUI_PUBLIC_HEADERS
        testing/ui_test.h
        animation.h
        app.h
        clipboard.h
        color.h
        data.h
        environment.h
        event.h
        external_texture.h
        file.h
        file_drop.h
        geometry.h
        gesture.h
        http.h
        huxerui.h
        indication.h
        layer.h
        layout.h
        lifecycle.h
        modifier.h
        navigation.h
        paint.h
        platform_adapter.h
        platform_registry.h
        presentation.h
        render_scene.h
        resource.h
        root.h
        scroll.h
        semantics.h
        state.h
        stream.h
        system.h
        task.h
        text.h
        text_input.h
        theme.h
        validation.h
        vector.h
        view.h
        virtual_layout.h
        window.h
)

set(HUXERUI_ANDROID_PUBLIC_HEADERS
        android/external_texture.h
        android/jni.h
        android/platform_registry.h
)

set(HUXERUI_IOS_PUBLIC_HEADERS
        ios/platform_registry.h
)

set(HUXERUI_MACOS_PUBLIC_HEADERS
        macos/platform_registry.h
)

if (WIN32)
    list(APPEND HUXERUI_WINDOWS_PUBLIC_HEADERS
            windows/external_texture.h
            windows/installer.h
            windows/platform_registry.h
    )
endif ()

if (APPLE)
    list(APPEND HUXERUI_IOS_PUBLIC_HEADERS ios/external_texture.h)
    list(APPEND HUXERUI_MACOS_PUBLIC_HEADERS macos/external_texture.h)
endif ()

if (EMSCRIPTEN)
    list(APPEND HUXERUI_WEB_PUBLIC_HEADERS
            web/external_texture.h
            web/navigation.h
            web/platform_registry.h
    )
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND HUXERUI_LINUX_PUBLIC_HEADERS
            linux/external_texture.h
    )
endif ()

function(huxerui_add_header_checks target_name)
    set(HUXERUI_HEADER_TEST_SOURCES)
    foreach (HUXERUI_PUBLIC_HEADER IN LISTS ARGN)
        string(REPLACE "." "_" HUXERUI_HEADER_TEST_NAME
                "${HUXERUI_PUBLIC_HEADER}"
        )
        string(REPLACE "/" "_" HUXERUI_HEADER_TEST_NAME
                "${HUXERUI_HEADER_TEST_NAME}"
        )
        set(HUXERUI_HEADER_TEST_SOURCE
                "${CMAKE_CURRENT_BINARY_DIR}/header_compile/${target_name}/${HUXERUI_HEADER_TEST_NAME}.cpp"
        )
        file(GENERATE
                OUTPUT "${HUXERUI_HEADER_TEST_SOURCE}"
                CONTENT "#include <huxerui/${HUXERUI_PUBLIC_HEADER}>\n"
        )
        list(APPEND HUXERUI_HEADER_TEST_SOURCES
                "${HUXERUI_HEADER_TEST_SOURCE}"
        )
    endforeach ()

    add_library(${target_name} OBJECT
            ${HUXERUI_HEADER_TEST_SOURCES}
    )

    target_link_libraries(
            ${target_name}
            PRIVATE ${HUXERUI_TEST_LIBRARY}
    )

endfunction()

huxerui_add_header_checks(huxerui_header_checks ${HUXERUI_PUBLIC_HEADERS})
if (HUXERUI_WINDOWS_PUBLIC_HEADERS)
    huxerui_add_header_checks(huxerui_windows_header_checks ${HUXERUI_WINDOWS_PUBLIC_HEADERS})
endif ()
if (HUXERUI_LINUX_PUBLIC_HEADERS)
    huxerui_add_header_checks(huxerui_linux_header_checks ${HUXERUI_LINUX_PUBLIC_HEADERS})
endif ()
if (HUXERUI_MACOS_PUBLIC_HEADERS)
    huxerui_add_header_checks(huxerui_macos_header_checks ${HUXERUI_MACOS_PUBLIC_HEADERS})
endif ()
if (HUXERUI_IOS_PUBLIC_HEADERS)
    huxerui_add_header_checks(huxerui_ios_header_checks ${HUXERUI_IOS_PUBLIC_HEADERS})
endif ()
if (HUXERUI_ANDROID_PUBLIC_HEADERS)
    huxerui_add_header_checks(huxerui_android_header_checks ${HUXERUI_ANDROID_PUBLIC_HEADERS})
endif ()
if (HUXERUI_WEB_PUBLIC_HEADERS)
    huxerui_add_header_checks(huxerui_web_header_checks ${HUXERUI_WEB_PUBLIC_HEADERS})
endif ()
