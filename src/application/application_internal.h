#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <huxerui/app.h>
#include <huxerui/data.h>
#include <huxerui/platform_adapter.h>
#include <huxerui/state.h>
#include <huxerui/system.h>
#include <huxerui/task.h>

namespace huxerui::detail {

using PermissionStatusCompletion = std::function<void(PermissionStatus)>;
using PermissionSettingsCompletion = std::function<void(bool)>;

class PermissionTransport {
public:
  virtual ~PermissionTransport() = default;

  virtual std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> OpenSettings(Permission permission, PermissionSettingsCompletion completion) = 0;
};

class PermissionController final {
public:
  struct State;

  PermissionController(std::shared_ptr<PermissionTransport> transport, UIThreadDispatcher dispatch_to_ui_thread);
  ~PermissionController();

  PermissionController(const PermissionController&) = delete;
  PermissionController& operator=(const PermissionController&) = delete;
  PermissionController(PermissionController&&) = delete;
  PermissionController& operator=(PermissionController&&) = delete;

  [[nodiscard]] Task<PermissionStatus> Check(Permission permission) const;
  [[nodiscard]] Task<PermissionStatus> Request(Permission permission) const;
  [[nodiscard]] Task<bool> OpenSettings(Permission permission) const;
  void Disconnect() noexcept;

private:
  std::shared_ptr<State> state_;
};

class AppResources;

struct ResolvedLocalNotification {
  std::string identifier;
  std::string title;
  std::string body;
  LocalNotificationPresentation presentation;
  Bytes data;
};

inline constexpr std::size_t max_local_notification_data_bytes = 64U * 1024U;

// Only self-contained envelopes can survive scheduled delivery after the originating process exits.
[[nodiscard]] Bytes EncodeLocalNotificationData(const PlatformPayload& data);
[[nodiscard]] PlatformPayload DecodeLocalNotificationData(std::span<const std::byte> bytes);

using LocalNotificationOperationCompletion = std::function<void(LocalNotificationOperationStatus)>;

class LocalNotificationTransport {
public:
  virtual ~LocalNotificationTransport() = default;

  [[nodiscard]] virtual LocalNotificationCapabilities Capabilities() const noexcept = 0;

  // An empty cancellation callback means an in-flight operation must finish before the ordered queue advances.
  // A non-empty callback stops the operation; it does not withdraw already accepted native notifications.
  virtual std::function<void()> CheckAuthorization(PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> Show(ResolvedLocalNotification notification,
                                     LocalNotificationOperationCompletion completion) = 0;
  virtual std::function<void()> Schedule(ResolvedLocalNotification notification,
                                         std::chrono::system_clock::time_point delivery_time,
                                         LocalNotificationOperationCompletion completion) = 0;
  virtual std::function<void()> Cancel(std::string identifier, LocalNotificationOperationCompletion completion) = 0;
};

class LocalNotificationService final {
public:
  struct State;

  static std::shared_ptr<LocalNotificationService> Create(std::shared_ptr<LocalNotificationTransport> transport,
                                                          UIThreadDispatcher dispatch_to_ui_thread,
                                                          std::shared_ptr<AppResources> resources);
  ~LocalNotificationService();

  [[nodiscard]] LocalNotificationCapabilities Capabilities() const;
  [[nodiscard]] Task<PermissionStatus> CheckAuthorization() const;
  [[nodiscard]] Task<PermissionStatus> RequestAuthorization() const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> Show(LocalNotification notification,
                                                            std::shared_ptr<const Environment> environment) const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> Schedule(LocalNotification notification,
                                                                std::chrono::system_clock::time_point delivery_time,
                                                                std::shared_ptr<const Environment> environment) const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> Cancel(std::string_view identifier) const;
  void Disconnect() noexcept;

private:
  LocalNotificationService(std::shared_ptr<LocalNotificationTransport> transport,
                           UIThreadDispatcher dispatch_to_ui_thread, std::shared_ptr<AppResources> resources);

  [[nodiscard]] ResolvedLocalNotification Resolve(LocalNotification notification,
                                                  std::shared_ptr<const Environment> environment) const;

  std::shared_ptr<State> state_;
  std::shared_ptr<AppResources> resources_;
};

class ApplicationService final : public std::enable_shared_from_this<ApplicationService> {
public:
  ApplicationService(Runtime& runtime, ApplicationActivation startup_activation,
                     std::shared_ptr<PermissionController> permissions,
                     std::shared_ptr<LocalNotificationService> local_notifications,
                     std::shared_ptr<SystemTrayService> system_tray, PlatformClipboard* platform_clipboard);

  [[nodiscard]] const ApplicationActivation& StartupActivation() const noexcept;
  [[nodiscard]] ApplicationLifecycleState LifecycleState() const;
  [[nodiscard]] std::function<void()> ConnectActivation(std::function<void(ApplicationActivation)> handler);
  [[nodiscard]] std::function<void()> ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler);
  [[nodiscard]] Task<PermissionStatus> CheckPermission(Permission permission) const;
  [[nodiscard]] Task<PermissionStatus> RequestPermission(Permission permission) const;
  [[nodiscard]] Task<bool> OpenPermissionSettings(Permission permission) const;
  [[nodiscard]] const std::shared_ptr<LocalNotificationService>& LocalNotifications() const noexcept;
  [[nodiscard]] const std::shared_ptr<SystemTrayService>& SystemTray() const noexcept;
  [[nodiscard]] const std::shared_ptr<huxerui::Clipboard>& Clipboard() const noexcept;
  void Quit() const;
  void Enqueue(ApplicationActivation activation);
  void UpdateLifecycleState(ApplicationLifecycleState lifecycle_state);
  void DispatchPending();
  void Disconnect() noexcept;

private:
  void DisconnectActivationHandler(std::uint64_t connection) noexcept;
  void DisconnectLifecycleHandler(std::uint64_t connection) noexcept;

  Runtime* runtime_;
  ApplicationActivation startup_activation_;
  std::shared_ptr<StateCell<ApplicationLifecycleState>> lifecycle_state_;
  std::shared_ptr<PermissionController> permissions_;
  std::shared_ptr<LocalNotificationService> local_notifications_;
  std::shared_ptr<SystemTrayService> system_tray_;
  std::shared_ptr<huxerui::Clipboard> clipboard_;
  std::deque<ApplicationActivation> pending_activations_;
  std::deque<ApplicationLifecycleState> pending_lifecycle_states_;
  std::function<void(ApplicationActivation)> activation_handler_;
  std::function<void(ApplicationLifecycleState)> lifecycle_handler_;
  std::uint64_t activation_connection_ = 0;
  std::uint64_t lifecycle_connection_ = 0;
  std::uint64_t next_connection_ = 1;
};

} // namespace huxerui::detail
