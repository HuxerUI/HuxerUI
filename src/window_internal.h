#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <huxerui/window.h>

namespace huxerui {

class Environment;
class PlatformAdapter;

namespace detail {

struct MountedNode;

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
  explicit WindowService(PlatformAdapter& platform);

  void Request(WindowCommand command);
  [[nodiscard]] bool HandleRequest(WindowCommand command);
  [[nodiscard]] std::function<void()>
  ConnectRequest(WindowCommand command, std::function<bool()> handler);
  void Disconnect() noexcept;

private:
  struct RequestHandler {
    std::function<bool()> handler;
    std::uint64_t connection = 0;
  };

  RequestHandler& Handler(WindowCommand command);
  void DisconnectRequest(WindowCommand command, std::uint64_t connection) noexcept;

  PlatformAdapter* platform_;
  RequestHandler minimize_handler_;
  RequestHandler close_handler_;
  std::uint64_t next_connection_ = 1;
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
