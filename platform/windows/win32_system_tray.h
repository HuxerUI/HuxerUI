#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>

#include "application/system_tray_internal.h"

namespace huxerui::detail {

class Win32SystemTrayTransport final : public SystemTrayTransport {
public:
  Win32SystemTrayTransport();
  ~Win32SystemTrayTransport() override;

  [[nodiscard]] bool IsAvailable() const noexcept override;
  void SetEventHandler(std::function<void(SystemTrayEvent)> handler) override;
  void Show(const ResolvedSystemTrayPresentation& presentation) override;
  void Hide() noexcept override;

  void SetWindow(HWND window) noexcept;
  [[nodiscard]] std::optional<LRESULT> HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
