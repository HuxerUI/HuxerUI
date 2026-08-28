#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

#include <huxerui/app.h>
#include <huxerui/state.h>

namespace huxerui::detail {

class ApplicationService final : public std::enable_shared_from_this<ApplicationService> {
public:
  ApplicationService(
      Runtime& runtime,
      ApplicationActivation startup_activation,
      std::shared_ptr<SystemTrayService> system_tray
  );

  [[nodiscard]] const ApplicationActivation& StartupActivation() const noexcept;
  [[nodiscard]] ApplicationLifecycleState LifecycleState() const;
  [[nodiscard]] std::function<void()> ConnectActivation(std::function<void(ApplicationActivation)> handler);
  [[nodiscard]] std::function<void()> ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler);
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
