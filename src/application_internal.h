#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

#include <huxerui/app.h>
#include <huxerui/state.h>

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

class ApplicationService final : public std::enable_shared_from_this<ApplicationService> {
public:
  ApplicationService(
      Runtime& runtime,
      ApplicationActivation startup_activation,
      std::shared_ptr<PermissionController> permissions,
      std::shared_ptr<SystemTrayService> system_tray
  );

  [[nodiscard]] const ApplicationActivation& StartupActivation() const noexcept;
  [[nodiscard]] ApplicationLifecycleState LifecycleState() const;
  [[nodiscard]] std::function<void()> ConnectActivation(std::function<void(ApplicationActivation)> handler);
  [[nodiscard]] std::function<void()> ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler);
  [[nodiscard]] Task<PermissionStatus> CheckPermission(Permission permission) const;
  [[nodiscard]] Task<PermissionStatus> RequestPermission(Permission permission) const;
  [[nodiscard]] Task<bool> OpenPermissionSettings(Permission permission) const;
  [[nodiscard]] const std::shared_ptr<SystemTrayService>& SystemTray() const noexcept;
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
  std::shared_ptr<SystemTrayService> system_tray_;
  std::deque<ApplicationActivation> pending_activations_;
  std::deque<ApplicationLifecycleState> pending_lifecycle_states_;
  std::function<void(ApplicationActivation)> activation_handler_;
  std::function<void(ApplicationLifecycleState)> lifecycle_handler_;
  std::uint64_t activation_connection_ = 0;
  std::uint64_t lifecycle_connection_ = 0;
  std::uint64_t next_connection_ = 1;
};

} // namespace huxerui::detail
