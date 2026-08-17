#pragma once

#include <windows.h>

#include <memory>

#include <huxerui/platform_module.h>

namespace huxerui::detail {

class Win32UIThreadDispatcher final {
public:
  static constexpr UINT task_message = WM_APP + 4;

  Win32UIThreadDispatcher();
  ~Win32UIThreadDispatcher();

  Win32UIThreadDispatcher(const Win32UIThreadDispatcher&) = delete;
  Win32UIThreadDispatcher& operator=(const Win32UIThreadDispatcher&) = delete;
  Win32UIThreadDispatcher(Win32UIThreadDispatcher&&) = delete;
  Win32UIThreadDispatcher& operator=(Win32UIThreadDispatcher&&) = delete;

  [[nodiscard]] UIThreadDispatcher Bind() const;
  void Attach(HWND window);
  void RunPending();
  void Shutdown() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace huxerui::detail
