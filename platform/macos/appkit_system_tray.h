#pragma once

#include <functional>
#include <memory>

#include "application/system_tray_internal.h"

namespace huxerui::detail {

class AppKitSystemTrayTransport final : public SystemTrayTransport {
public:
  AppKitSystemTrayTransport();
  ~AppKitSystemTrayTransport() override;

  [[nodiscard]] bool IsAvailable() const noexcept override;
  void SetEventHandler(std::function<void(SystemTrayEvent)> handler) override;
  void Show(const ResolvedSystemTrayPresentation& presentation) override;
  void Hide() noexcept override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::detail
