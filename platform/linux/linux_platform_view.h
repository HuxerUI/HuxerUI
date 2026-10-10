#pragma once

#include <gtk/gtk.h>

#include <huxerui/render_scene.h>
#include <huxerui/window.h>

#include <functional>
#include <memory>

namespace huxerui::detail {

class LinuxRenderer;
class PlatformRegistry;

/// Owns one GTK child hierarchy for every native PlatformView in a Linux window.
class LinuxPlatformViews final {
public:
  LinuxPlatformViews(GtkWidget* host, LinuxRenderer& renderer, PlatformRegistry& registry, UiWindow& ui_window);
  ~LinuxPlatformViews();

  LinuxPlatformViews(const LinuxPlatformViews&) = delete;
  LinuxPlatformViews& operator=(const LinuxPlatformViews&) = delete;

  void Commit(const RenderFrame& frame);
  void Allocate(int width, int height);
  void Snapshot(GtkSnapshot* snapshot, const RenderFrame& frame);
  [[nodiscard]] bool RouteToHuxerUI(Point position, PointerEventType type, bool huxerui_pointer_capture);
  [[nodiscard]] bool HasPlatformFocus() const noexcept;
  void SetFocusChangedHandler(std::function<void(bool)> handler);
  void Shutdown() noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
