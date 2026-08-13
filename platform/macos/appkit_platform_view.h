#pragma once

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <cstdint>
#include <memory>

#include <huxerui/platform_module.h>
#include <huxerui/render_scene.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class AppKitRenderer;

class AppKitPlatformViews final {
public:
  AppKitPlatformViews(AppKitRenderer& renderer, PlatformModules& modules, Runtime& runtime);
  ~AppKitPlatformViews();

  AppKitPlatformViews(const AppKitPlatformViews&) = delete;
  AppKitPlatformViews& operator=(const AppKitPlatformViews&) = delete;

  [[nodiscard]] bool Commit(NSView* root, const RenderFrame& frame);
  void DrawBase(CGContextRef context, CGRect dirty_rect);
  NSView* HitTest(Point point) const;
  NSView* AccessibilityView(std::uint64_t identity) const;
  [[nodiscard]] bool BeginFocusTraversal(NSResponder* responder, bool reverse);
  void EndFocusTraversal();
  void SynchronizeFocus(NSResponder* responder);
  void Shutdown();

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
