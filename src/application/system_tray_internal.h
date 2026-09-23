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

#include "resources/resource_internal.h"

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
  void Show(ImageVariant icon, SystemTrayOptions options);
  void Hide();
  /// Retains one application-lifetime handler after validating the original application thread and context.
  /// @param handler Nonempty callback; duplicate registration is rejected without replacing the current handler.
  void OnActivate(std::function<void()> handler);
  void Disconnect() noexcept;
  void RefreshPresentation();

private:
  struct DesiredPresentation {
    ImageVariant icon;
    SystemTrayOptions options;
  };

  SystemTrayService(std::shared_ptr<SystemTrayTransport> transport, std::shared_ptr<AppResources> resources);
  void EnsureInitialized();
  void HandleEvent(const SystemTrayEvent& event);
  ResolvedSystemTrayPresentation ResolvePresentation(
      const DesiredPresentation& desired,
      std::uint64_t generation,
      std::uint64_t& next_command,
      std::unordered_map<std::uint64_t, std::function<void()>>& callbacks
  );
  std::vector<ResolvedSystemTrayMenuEntry> ResolveMenu(
      const std::vector<MenuEntry>& entries,
      const ResourceConfiguration& configuration,
      std::uint64_t& next_command,
      std::unordered_map<std::uint64_t, std::function<void()>>& callbacks
  );

  std::shared_ptr<SystemTrayTransport> transport_;
  std::shared_ptr<AppResources> resources_;
  std::shared_ptr<StateCell<bool>> available_;
  std::optional<DesiredPresentation> desired_;
  std::unordered_map<std::uint64_t, std::function<void()>> callbacks_;
  std::function<void()> activation_handler_;
  std::uint64_t generation_ = 0;
  std::uint64_t next_command_ = 1;
  bool initialized_ = false;
  bool connected_ = true;
  std::shared_ptr<ExecutionContext> execution_;
};

} // namespace huxerui::detail
