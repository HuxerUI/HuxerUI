huxerui_add_test_suite(huxerui_macos_tests HuxerUIMacOSTests
        LABELS platform macos native
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_sources(huxerui_macos_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/apple_timer.mm"
        local_notification.mm
        application.mm
        platform_module.mm
        platform_bridge.mm
        platform_view.mm
        external_texture.mm
        file_picker.mm
        text_input.mm
        window_chrome.cpp
)
set_source_files_properties(
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/apple_timer.mm"
        platform_bridge.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
)
target_include_directories(huxerui_macos_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module"
        "${HUXERUI_PROJECT_DIR}/platform/macos"
)
if (TARGET ${HUXERUI_STATIC_LIB_NAME})
    add_test(NAME HuxerUIMacStaticPlatformBridgeImport
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_platform_bridge.sh"
                    "${HUXERUI_PROJECT_DIR}"
                    "${HUXERUI_PUBLIC_INCLUDE_DIR}"
                    "$<TARGET_FILE:${HUXERUI_STATIC_LIB_NAME}>"
                    "${CMAKE_CURRENT_BINARY_DIR}/macos_platform_bridge_static"
    )
endif ()
if (TARGET ${HUXERUI_SHARED_LIB_NAME})
    add_test(NAME HuxerUIMacSharedPlatformBridgeImport
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_platform_bridge.sh"
                    "${HUXERUI_PROJECT_DIR}"
                    "${HUXERUI_PUBLIC_INCLUDE_DIR}"
                    "$<TARGET_FILE:${HUXERUI_SHARED_LIB_NAME}>"
                    "${CMAKE_CURRENT_BINARY_DIR}/macos_platform_bridge_shared"
    )
endif ()
target_compile_definitions(huxerui_macos_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)
foreach (bridge_test IN ITEMS HuxerUIMacStaticPlatformBridgeImport HuxerUIMacSharedPlatformBridgeImport)
    if (TEST ${bridge_test})
        set_tests_properties(${bridge_test} PROPERTIES LABELS "platform;macos;native;headers")
    endif ()
endforeach ()
