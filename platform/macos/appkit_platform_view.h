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

class AppKitPlatformViewHost final {
public:
  AppKitPlatformViewHost(AppKitRenderer& renderer, PlatformModules& modules, Runtime& runtime);
  ~AppKitPlatformViewHost();

  AppKitPlatformViewHost(const AppKitPlatformViewHost&) = delete;
  AppKitPlatformViewHost& operator=(const AppKitPlatformViewHost&) = delete;

  [[nodiscard]] bool Commit(NSView* root, const RenderFrame& frame);
  void DrawBase(CGContextRef context, CGRect dirty_rect);
  NSView* HitTest(Point point) const;
  void Shutdown();

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
