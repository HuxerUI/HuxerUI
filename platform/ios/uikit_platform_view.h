#pragma once

#import <CoreGraphics/CoreGraphics.h>
#import <UIKit/UIKit.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <huxerui/platform_registry.h>
#include <huxerui/render_scene.h>

namespace huxerui {
class UiWindow;
}

namespace huxerui::detail {

class UIKitRenderer;
class FrameCoordinator;

class UIKitPlatformViews final {
public:
  UIKitPlatformViews(UIKitRenderer& renderer, FrameCoordinator& frame_coordinator, PlatformRegistry& registry,
                     UiWindow& ui_window);
  ~UIKitPlatformViews();

  UIKitPlatformViews(const UIKitPlatformViews&) = delete;
  UIKitPlatformViews& operator=(const UIKitPlatformViews&) = delete;

  [[nodiscard]] bool Commit(UIView* root, const RenderFrame& frame);
  std::optional<double> DrawBase(CGContextRef context, CGRect dirty_rect, bool platform_ready);
  void DrawSlice(CGContextRef context, CGRect dirty_rect, std::size_t first_command, std::size_t command_count);
  void InvalidateSlices();
  UIView* HitTest(Point point, UIEvent* event);
  [[nodiscard]] UIView* AccessibilityView(std::uint64_t identity) const noexcept;
  void ClearFocus();
  void Shutdown();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::detail
