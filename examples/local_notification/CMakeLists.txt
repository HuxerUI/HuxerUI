huxerui_add_example(example_local_notification "HuxerUI Local Notifications" "com.huxerui.localnotification")

if (WIN32 AND NOT HUXERUI_WINDOWS_7_COMPAT)
    target_compile_definitions(example_local_notification PRIVATE HUXERUI_EXAMPLE_WINDOWS_LOCAL_NOTIFICATIONS=1)
    target_sources(example_local_notification PRIVATE windows/notification_template.cpp)
endif ()

if (IOS AND TARGET huxerui_static)
    # The extension links the SDK alone, without the app core's force-loaded Application declaration.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/huxerui-ios/notification-extension-link.rsp"
            CONTENT "\"$<TARGET_FILE:huxerui_static>\"\n$<JOIN:$<TARGET_PROPERTY:huxerui_static,INTERFACE_LINK_LIBRARIES>,\n>\n-lc++\n-framework\nUserNotificationsUI\n")
endif ()
