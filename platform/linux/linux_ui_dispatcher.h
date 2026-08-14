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
  LinuxUIThreadDispatcher(LinuxUIThreadDispatcher&&) = delete;
  LinuxUIThreadDispatcher& operator=(LinuxUIThreadDispatcher&&) = delete;

  [[nodiscard]] UIThreadDispatcher Bind() const;
  [[nodiscard]] int FileDescriptor() const noexcept;
  void RunPending();

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace huxerui::detail
