#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>

#include <huxerui/platform_adapter.h>
#include <huxerui/platform_registry.h>
#include <huxerui/render_scene.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class Win32PlatformViews final {
public:
  using OverlayMessageHandler = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;

  Win32PlatformViews(HINSTANCE instance, HWND root, PlatformRegistry& registry, Runtime& runtime,
                     UIThreadDispatcher dispatch_to_ui_thread, OverlayMessageHandler overlay_message_handler);
  ~Win32PlatformViews();

  Win32PlatformViews(const Win32PlatformViews&) = delete;
  Win32PlatformViews& operator=(const Win32PlatformViews&) = delete;

  [[nodiscard]] bool Commit(const RenderFrame& frame, float dpi_scale);
  void DidPresent();
  void Resize();
  [[nodiscard]] bool HandleFocusTraversal(const MSG& message);
  void SynchronizeFocus(HWND focused);
  // Accessibility captures this UI-thread-owned handle into the same snapshot as its SemanticFrame.
  [[nodiscard]] HWND AccessibilityView(std::uint64_t identity) const noexcept;
  void Shutdown() noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
