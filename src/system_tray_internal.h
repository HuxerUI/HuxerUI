#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/state.h>

#include "resource_internal.h"

namespace huxerui::detail {

struct ResolvedSystemTrayMenuEntry {
  bool section = false;
  std::string label;
  std::optional<ImageAsset> icon;
  std::vector<ResolvedSystemTrayMenuEntry> children;
  std::uint64_t command = 0;
  bool enabled = true;
  std::optional<bool> checked;
  std::optional<Color> icon_tint;
};

struct ResolvedSystemTrayPresentation {
  ImageAsset icon;
  std::string tooltip;
  std::vector<ResolvedSystemTrayMenuEntry> menu;
  std::uint64_t generation = 0;
};

enum class SystemTrayEventType {
  AvailabilityChanged,
  Activate,
  Command,
};

struct SystemTrayEvent {
  SystemTrayEventType type = SystemTrayEventType::AvailabilityChanged;
  bool available = false;
  std::uint64_t generation = 0;
  std::uint64_t command = 0;
};

class SystemTrayTransport {
public:
  virtual ~SystemTrayTransport() = default;

  [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;
  virtual void SetEventHandler(std::function<void(SystemTrayEvent)> handler) = 0;
  virtual void Show(const ResolvedSystemTrayPresentation& presentation) = 0;
  virtual void Hide() noexcept = 0;
};

class SystemTrayService final : public std::enable_shared_from_this<SystemTrayService> {
public:
  static std::shared_ptr<SystemTrayService>
  Create(std::shared_ptr<SystemTrayTransport> transport, std::shared_ptr<AppResources> resources);
  ~SystemTrayService();

  [[nodiscard]] bool IsAvailable();
  void Show(
      std::uint64_t owner, ImageVariant icon, SystemTrayOptions options, std::shared_ptr<const Environment> environment
  );
  void Hide(std::uint64_t owner) noexcept;
  [[nodiscard]] std::function<void()> ConnectActivate(std::function<void()> handler);
  void Disconnect() noexcept;

private:
  struct DesiredPresentation {
    std::uint64_t owner = 0;
    ImageVariant icon;
    SystemTrayOptions options;
    std::shared_ptr<const Environment> environment;
  };

  SystemTrayService(std::shared_ptr<SystemTrayTransport> transport, std::shared_ptr<AppResources> resources);
  void EnsureInitialized();
  void HandleEvent(const SystemTrayEvent& event);
  void RefreshPresentation();
  ResolvedSystemTrayPresentation ResolvePresentation(
      const DesiredPresentation& desired,
      std::uint64_t generation,
      std::uint64_t& next_command,
      std::unordered_map<std::uint64_t, std::function<void()>>& callbacks
  );
  std::vector<ResolvedSystemTrayMenuEntry> ResolveMenu(
      const std::vector<MenuEntry>& entries,
      const Locale& locale,
      std::uint64_t& next_command,
      std::unordered_map<std::uint64_t, std::function<void()>>& callbacks
  );
  void DisconnectActivate(std::uint64_t connection) noexcept;

  std::shared_ptr<SystemTrayTransport> transport_;
  std::shared_ptr<AppResources> resources_;
  std::shared_ptr<StateCell<bool>> available_;
  std::optional<DesiredPresentation> desired_;
  std::unordered_map<std::uint64_t, std::function<void()>> callbacks_;
  std::function<void()> activation_handler_;
  std::uint64_t generation_ = 0;
  std::uint64_t next_command_ = 1;
  std::uint64_t activation_connection_ = 0;
  std::uint64_t next_connection_ = 1;
  bool initialized_ = false;
  bool connected_ = true;
};

} // namespace huxerui::detail
