#pragma once

#include <cstdint>
#include <memory>

#include <emscripten/val.h>

#include <huxerui/geometry.h>
#include <huxerui/platform_module.h>
#include <huxerui/render_scene.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class WebRenderer;

class WebPlatformViews final {
public:
  WebPlatformViews(
      WebRenderer& renderer,
      PlatformModules& modules,
      Runtime& runtime,
      UIThreadDispatcher dispatch_to_ui_thread,
      emscripten::val root,
      emscripten::val base_canvas
  );
  ~WebPlatformViews();

  WebPlatformViews(const WebPlatformViews&) = delete;
  WebPlatformViews& operator=(const WebPlatformViews&) = delete;

  void SetViewport(Size viewport, float display_scale);
  void Commit(const RenderFrame& frame);
  [[nodiscard]] bool HitTest(std::uint32_t token, Point point) const;
  void SynchronizeFocus(std::uint32_t token, bool focus_visible);
  void MoveFocus(std::uint32_t token, bool reverse);
  void Shutdown() noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
