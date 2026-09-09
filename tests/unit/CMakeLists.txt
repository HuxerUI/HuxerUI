huxerui_add_test_suite(huxerui_unit_tests HuxerUIUnitTests
        LABELS common unit
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES
            animation.cpp
            application.cpp
            buffer_reference_example.cpp
            data.cpp
            external_texture.cpp
            file.cpp
            geometry.cpp
            platform_frame.cpp
            paint.cpp
            path.cpp
            platform_payload.cpp
            resource.cpp
            semantics.cpp
            stream.cpp
            text.cpp
            text_input.cpp
            text_selection.cpp
            validation.cpp
            vector.cpp
            "${HUXERUI_PROJECT_DIR}/examples/buffer_reference/frame_source.cpp"
            "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_compile_definitions(huxerui_unit_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)
