#pragma once

#include <napi/native_api.h>

#include <initializer_list>
#include <map>
#include <string>
#include <string_view>

#include "application/application_internal.h"

namespace huxerui::detail {

std::optional<NotificationActivation> DecodeOhosNotificationActivation(std::string identifier, std::string_view payload);

class OhosSystemServices final : public PermissionTransport, public LocalNotificationTransport {
public:
  OhosSystemServices(napi_env env, napi_value host);
  ~OhosSystemServices() override;

  std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) override;
  std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) override;
  std::function<void()> OpenSettings(Permission permission, PermissionSettingsTarget target,
                                     PermissionSettingsCompletion completion) override;

  LocalNotificationCapabilities Capabilities() const noexcept override;
  std::function<void()> CheckAuthorization(PermissionStatusCompletion completion) override;
  std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) override;
  std::function<void()> Show(ResolvedLocalNotification notification,
                             LocalNotificationOperationCompletion completion) override;
  std::function<void()> Schedule(ResolvedLocalNotification notification, std::chrono::system_clock::time_point delivery,
                                 LocalNotificationOperationCompletion completion) override;
  std::function<void()> Cancel(std::string identifier, LocalNotificationOperationCompletion completion) override;

  napi_value Complete(napi_callback_info info);
  void Disconnect() noexcept;

private:
  enum class Operation { Check, Request, Settings, Show, Cancel, Schedule };

  std::function<void()> StartPermission(Operation operation, Permission permission,
                                         PermissionStatusCompletion completion);
  void Start(Operation operation, std::initializer_list<std::string_view> args, std::function<void(int)> completion);

  napi_env env_;
  napi_ref host_ = nullptr;
  bool can_schedule_ = false;
  std::map<std::string, std::function<void(int)>, std::less<>> pending_;
};

} // namespace huxerui::detail
