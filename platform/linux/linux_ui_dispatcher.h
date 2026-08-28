#pragma once

#include <memory>

#include <huxerui/platform_module.h>

namespace huxerui::detail {

class LinuxUIThreadDispatcher final {
public:
  LinuxUIThreadDispatcher();
  ~LinuxUIThreadDispatcher();

  LinuxUIThreadDispatcher(const LinuxUIThreadDispatcher&) = delete;
  LinuxUIThreadDispatcher& operator=(const LinuxUIThreadDispatcher&) = delete;

  [[nodiscard]] UIThreadDispatcher Bind() const;
  void DrainPending();
  void Shutdown() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace huxerui::detail
