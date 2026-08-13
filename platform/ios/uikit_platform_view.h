#pragma once

#import <CoreGraphics/CoreGraphics.h>
#import <UIKit/UIKit.h>

#include <cstdint>
#include <memory>

#include <huxerui/platform_module.h>
#include <huxerui/render_scene.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class UIKitRenderer;

class UIKitPlatformViews final {
public:
  UIKitPlatformViews(UIKitRenderer& renderer, PlatformModules& modules, Runtime& runtime);
  ~UIKitPlatformViews();

  UIKitPlatformViews(const UIKitPlatformViews&) = delete;
  UIKitPlatformViews& operator=(const UIKitPlatformViews&) = delete;

  [[nodiscard]] bool Commit(UIView* root, const RenderFrame& frame);
  void DrawBase(CGContextRef context, CGRect dirty_rect);
  UIView* HitTest(Point point, UIEvent* event);
  void SynchronizeFocus(UIView* responder);
  void Shutdown();

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
