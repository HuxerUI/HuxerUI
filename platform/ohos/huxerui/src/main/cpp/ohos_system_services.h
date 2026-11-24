#pragma once

#include <napi/native_api.h>

#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "application/application_internal.h"
#include "application/system_task_internal.h"

namespace huxerui::detail {

class OhosBackgroundTasks;

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

/// Correlates Runtime grants with the ArkTS host's continuous tasks.
class OhosSystemTaskTransport final : public SystemTaskTransport,
                                      public std::enable_shared_from_this<OhosSystemTaskTransport> {
public:
  OhosSystemTaskTransport(napi_env env, napi_value host, std::shared_ptr<OhosBackgroundTasks> background);
  ~OhosSystemTaskTransport() override;

  SystemTaskCapabilities Capabilities(SystemTaskKind kind) const noexcept override;
  void Install(std::span<const SystemTaskRegistration>, EventHandler on_event) override;
  SystemTaskResult<void> Submit(const SystemTaskRequest& request) override;
  SystemTaskResult<void> Withdraw(const SystemTaskRequest& request) override;
  bool ObservesWithdrawal(const SystemTaskRequest& request) const noexcept override;
  void Disconnect() noexcept override;

  void SetActive(bool active) noexcept { active_ = active; }
  OhosBackgroundTasks* Background() const noexcept { return background_.get(); }
  void Receive(std::string_view token, int event, int result, std::string message);
  void ReleaseHost() noexcept;

private:
  struct Request {
    SystemTaskRequest request;
    std::weak_ptr<SystemTaskGrant> grant{};
  };

  SystemTaskResult<void> Release(std::string_view token);
  SystemTaskResult<void> ReportProgress(std::string_view token, TaskProgress progress);
  SystemTaskResult<void> UpdatePresentation(std::string_view token, const ResolvedTaskPresentation& presentation);
  SystemTaskResult<void> Call(const char* method, std::span<napi_value> args = {});

  napi_env env_;
  napi_ref host_ = nullptr;
  bool can_continue_ = false;
  bool can_run_ongoing_ = false;
  bool active_ = false;
  std::shared_ptr<OhosBackgroundTasks> background_;
  EventHandler on_event_;
  std::map<std::string, Request, std::less<>> requests_;
};

} // namespace huxerui::detail
