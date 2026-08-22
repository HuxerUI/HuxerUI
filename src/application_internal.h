#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

#include <huxerui/app.h>

namespace huxerui::detail {

class ApplicationService final : public std::enable_shared_from_this<ApplicationService> {
public:
  ApplicationService(Runtime& runtime, ApplicationActivation startup_activation);

  [[nodiscard]] const ApplicationActivation& StartupActivation() const noexcept;
  [[nodiscard]] std::function<void()> Connect(std::function<void(ApplicationActivation)> handler);
  void Enqueue(ApplicationActivation activation);
  void DispatchPending();
  void Disconnect() noexcept;

private:
  void DisconnectHandler(std::uint64_t connection) noexcept;

  Runtime* runtime_;
  ApplicationActivation startup_activation_;
  std::deque<ApplicationActivation> pending_activations_;
  std::function<void(ApplicationActivation)> handler_;
  std::uint64_t connection_ = 0;
  std::uint64_t next_connection_ = 1;
};

} // namespace huxerui::detail
