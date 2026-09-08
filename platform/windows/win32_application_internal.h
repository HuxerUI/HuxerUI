#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/platform_adapter.h>
#include <huxerui/system.h>

namespace huxerui::detail {

class PermissionTransport;
class LocalNotificationTransport;
struct ResolvedLocalNotification;
struct Win32NotificationInbox;

// The process owns COM activation; adapters only borrow its notification transport.
class Win32LocalNotificationHost final {
public:
  explicit Win32LocalNotificationHost(UIThreadDispatcher dispatcher);
  ~Win32LocalNotificationHost();
  Win32LocalNotificationHost(const Win32LocalNotificationHost&) = delete;
  Win32LocalNotificationHost& operator=(const Win32LocalNotificationHost&) = delete;

  [[nodiscard]] std::shared_ptr<LocalNotificationTransport> Transport() const;
  [[nodiscard]] std::optional<NotificationActivation> WaitForActivation();
  void SetActivationHandler(std::function<void(NotificationActivation)> handler);

private:
  std::shared_ptr<Win32NotificationInbox> inbox_;
  std::shared_ptr<LocalNotificationTransport> transport_;
  unsigned long registration_ = 0;
  bool apartment_initialized_ = false;
};

[[nodiscard]] std::wstring EncodeWin32NotificationActivation(const ResolvedLocalNotification& notification);
[[nodiscard]] std::optional<NotificationActivation> DecodeWin32NotificationActivation(std::wstring_view arguments);
[[nodiscard]] std::optional<std::wstring> BuildWin32NotificationXml(const ResolvedLocalNotification& notification,
    const windows::LocalNotificationTemplateProvider& template_provider = {});
inline constexpr std::wstring_view win32_notification_launch_flag = L"--huxerui-notification-activate";
inline constexpr std::wstring_view win32_notification_payload_flag = L"--huxerui-notification-payload";

inline constexpr std::uintptr_t win32_application_activation_data_id = 0x48555841U;
inline constexpr std::size_t win32_application_activation_max_characters = 32768;

struct Win32StartupInput {
  std::vector<std::wstring> arguments;
  ApplicationActivation activation;
  bool notification_server = false;
};

[[nodiscard]] ApplicationActivation ParseWin32ApplicationActivation(std::span<const std::wstring> arguments);
[[nodiscard]] std::vector<wchar_t> EncodeWin32ApplicationArguments(std::span<const std::wstring> arguments);
[[nodiscard]] std::optional<ApplicationActivation> DecodeWin32ApplicationActivation(
    std::span<const wchar_t> payload
) noexcept;
[[nodiscard]] Win32StartupInput CurrentWin32StartupInput();
[[nodiscard]] std::wstring Win32ApplicationWindowClassName();
[[nodiscard]] bool TryForwardWin32ApplicationActivation(
    std::wstring_view window_class_name,
    std::span<const std::wstring> arguments
);
[[nodiscard]] std::shared_ptr<PermissionTransport> CreateWin32PermissionTransport();

} // namespace huxerui::detail
