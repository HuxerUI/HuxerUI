huxerui_add_test_suite(huxerui_runtime_tests HuxerUIRuntimeTests
        LABELS common runtime
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES
            animation.cpp
            application.cpp
            clipboard.cpp
            composition.cpp
            file_picker.cpp
            file_drop.cpp
            gestures.cpp
            http.cpp
            interaction.cpp
            layers.cpp
            layout.cpp
            lifecycle.cpp
            navigation.cpp
            navigation_ui.cpp
            local_notification.cpp
            permission.cpp
            platform_module.cpp
            platform_view.cpp
            presentation.cpp
            presentation_semantics.cpp
            profiling.cpp
            render_scene.cpp
            resources.cpp
            scrolling.cpp
            combo_box.cpp
            date_time_picker.cpp
            select.cpp
            tree.cpp
            system_tray.cpp
            selection_area.cpp
            task.cpp
            text.cpp
            text_field.cpp
            text_input.cpp
            tooltip.cpp
            virtual_layout.cpp
            window.cpp
            "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
if (TARGET HuxerUI::testing)
    huxerui_add_test_suite(huxerui_ui_tests HuxerUIUiTests
            LABELS common runtime testing
            LIBRARIES HuxerUI::testing
            SOURCES ui_testing.cpp
    )
    huxerui_add_resources(huxerui_ui_tests
            ROOT "${HUXERUI_TEST_DIR}/libraries/application_resources"
            NAMESPACE ui_test_fixture
    )
    _huxerui_append_resource_package_input(huxerui_ui_tests "${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}")
    target_compile_definitions(huxerui_ui_tests PRIVATE
            "HUXERUI_UI_TEST_PACKAGE=\"${CMAKE_CURRENT_BINARY_DIR}/huxerui-resources/huxerui_ui_tests/package\""
    )
endif ()
target_compile_definitions(huxerui_runtime_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
        $<$<BOOL:${HUXERUI_ENABLE_PROFILING}>:HUXERUI_ENABLE_PROFILING=1>
)

huxerui_add_test_suite(huxerui_library_tests HuxerUILibraryTests
        LABELS common runtime libraries
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES libraries.cpp
)
huxerui_use_library(huxerui_library_tests
        TARGET HuxerUI::test_library
        PATH "${HUXERUI_TEST_DIR}/libraries/root_service"
)
huxerui_add_resources(huxerui_library_tests
        ROOT "${HUXERUI_TEST_DIR}/libraries/application_resources"
        NAMESPACE huxerui_test_library
)
target_compile_definitions(huxerui_library_tests PRIVATE
        "HUXERUI_LIBRARY_TEST_RESOURCE_PACKAGE=\"${CMAKE_CURRENT_BINARY_DIR}/huxerui-resources/huxerui_library_tests/package\""
)
