#include <huxerui/app.h>

#if defined(HUXERUI_EXAMPLE_WINDOWS_LOCAL_NOTIFICATIONS) || defined(HUXERUI_EXAMPLE_WINDOWS_URL_SCHEME)
#include <huxerui/system.h>

#include <exception>
#include <iostream>
#include <string_view>

#if defined(HUXERUI_EXAMPLE_WINDOWS_LOCAL_NOTIFICATIONS)
#include "local_notification/windows/notification_template.h"
#endif

int main(int argc, char** argv) {
  try {
#if defined(HUXERUI_EXAMPLE_WINDOWS_LOCAL_NOTIFICATIONS)
    // Keep development and installed application identities separate, and keep each CLSID stable across launches.
    constexpr std::string_view app_id = "com.huxerui.localnotification.development";
    constexpr std::string_view activator_clsid = "{89FC9ADA-6C7C-5E8C-A8F5-242E5CB84563}";
    // Cleanup must bypass registration and host construction, including when no application window is open.
    if (argc == 2 && std::string_view(argv[1]) == "--unregister-notifications") {
      huxerui::windows::UnregisterLocalNotifications(app_id, activator_clsid);
      return 0;
    }
    huxerui::windows::RegisterLocalNotifications(app_id, "HuxerUI Local Notifications", activator_clsid,
                                                local_notification_example::BuildNotificationTemplate);
#elif defined(HUXERUI_EXAMPLE_WINDOWS_URL_SCHEME)
    constexpr std::string_view scheme = "huxerui-example";
    if (argc == 2 && std::string_view(argv[1]) == "--unregister-url-scheme") {
      huxerui::windows::UnregisterUrlScheme(scheme);
      return 0;
    }
    huxerui::windows::RegisterUrlScheme(scheme, "HuxerUI Application Example");
#endif
    return huxerui::RunApplication();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
#else
int main() {
  return huxerui::RunApplication();
}
#endif
