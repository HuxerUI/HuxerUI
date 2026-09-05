#pragma once

#include "linux_internal.h"

#include <memory>

#include "application/system_tray_internal.h"

namespace huxerui::detail {

class LinuxSystemTrayTransport final : public SystemTrayTransport {
public:
  LinuxSystemTrayTransport();
  ~LinuxSystemTrayTransport() override;

  [[nodiscard]] bool IsAvailable() const noexcept override;
  void SetEventHandler(std::function<void(SystemTrayEvent)> handler) override;
  void Show(const ResolvedSystemTrayPresentation& presentation) override;
  void Hide() noexcept override;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
