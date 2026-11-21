#pragma once

#include <arkui/native_node.h>

#include <functional>
#include <memory>

#include <huxerui/render_scene.h>

struct OH_Drawing_Canvas;

namespace huxerui {
class UiWindow;

namespace detail {

class PlatformRegistry;
class OhosRenderer;

class OhosPlatformViews final {
public:
  using ForwardTouch = std::function<void(const ArkUI_UIInputEvent*, Point)>;

  OhosPlatformViews(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeHandle root, UiWindow& window,
                    PlatformRegistry& registry, OhosRenderer& renderer, ForwardTouch forward_touch);
  ~OhosPlatformViews();

  void Commit(const RenderFrame& frame, float scale);
  void DrawBase(OH_Drawing_Canvas* canvas) const;
  void Clear() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace detail
} // namespace huxerui
