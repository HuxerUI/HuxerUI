#pragma once

#include <algorithm>
#include <memory>
#include <map>
#include <optional>
#include <utility>

#include <huxerui/window.h>

namespace huxerui {

class Environment;
class UiWindow;

namespace detail {

struct MountedNode;
struct ApplicationRuntimeState;

inline Size ResolveInitialWindowSize(const WindowOptions& options) noexcept {
  if (!options.minimum_size.has_value()) {
    return options.initial_size;
  }
  return {
      std::max(options.initial_size.width, options.minimum_size->width),
      std::max(options.initial_size.height, options.minimum_size->height),
  };
}

struct WindowState {
  explicit WindowState(const WindowOptions& options)
      : content_mode(options.content_mode), chrome_mode(options.chrome_mode), caption_labels(options.caption_labels) {}

  WindowContentMode content_mode = WindowContentMode::SafeArea;
  WindowChromeMode chrome_mode = WindowChromeMode::System;
  WindowCaptionLabels caption_labels;
  WindowMetrics metrics;
  SystemBarsAppearance appearance = SystemBarsAppearance::Default();
  Color caption_foreground = Color::Rgb(32, 32, 32);
  // This cache suppresses redundant native system-bar updates; the resolved appearance remains the paint authority.
  std::optional<std::pair<SystemBarContentBrightness, SystemBarContentBrightness>> committed_system_bar_brightness;
};

class WindowService : public std::enable_shared_from_this<WindowService> {
public:
  explicit WindowService(UiWindow& ui_window);

  void Request(WindowCommand command);
  [[nodiscard]] bool HandleRequest(WindowCommand command);
  [[nodiscard]] std::function<void()>
  ConnectRequest(WindowCommand command, std::function<bool()> handler);
  void Disconnect() noexcept;
  /// Reads the current native window state and subscribes active composition.
  /// @return This window's state, independent of application foreground aggregation.
  [[nodiscard]] WindowLifecycleState LifecycleState() const;
  /// Updates this window and queues distinct transitions through the original application dispatcher.
  /// @param state Native window/scene state on the application thread; equal values produce no event.
  /// Recipient identities are captured at submission so later observers do not receive earlier transitions.
  void UpdateLifecycleState(WindowLifecycleState state);
  /// Connects one observer to future window transitions.
  /// @param handler Nonempty application-thread callback; the current value is not replayed.
  /// @return Idempotent disconnection callback using weak service ownership; retiring the window also drops delivery.
  [[nodiscard]] std::function<void()> ConnectLifecycle(std::function<void(WindowLifecycleState)> handler);

private:
  struct RequestHandler {
    std::function<bool()> handler;
    std::uint64_t connection = 0;
  };

  RequestHandler& Handler(WindowCommand command);
  void DisconnectRequest(WindowCommand command, std::uint64_t connection) noexcept;

  UiWindow* ui_window_;
  RequestHandler minimize_handler_;
  RequestHandler close_handler_;
  std::uint64_t next_connection_ = 1;
  State<WindowLifecycleState> lifecycle_state_{WindowLifecycleState::Background};
  std::map<std::uint64_t, std::function<void(WindowLifecycleState)>> lifecycle_handlers_;
  std::weak_ptr<ApplicationRuntimeState> application_;
};

View MakeWindowControls(
    const std::shared_ptr<WindowService>& service,
    const std::shared_ptr<WindowState>& window,
    std::shared_ptr<const Environment> environment,
    bool visible
);
bool IsWindowControlsNode(const MountedNode& node) noexcept;
bool IsValidSystemBarsAppearance(const SystemBarsAppearance& appearance) noexcept;

} // namespace detail

} // namespace huxerui
