#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/resource.h>
#include <huxerui/view.h>

namespace huxerui {

namespace detail {
struct ModifierDescriptor;
class WindowService;
} // namespace detail

enum class WindowContentMode {
  SafeArea,
  EdgeToEdge,
};

enum class WindowChromeMode {
  System,
  Custom,
};

enum class WindowCommand {
  Minimize,
  Maximize,
  Restore,
  ToggleMaximize,
  Close,
};

enum class SystemBarContentBrightness {
  Automatic,
  Light,
  Dark,
};

struct WindowTitleBarMetrics {
  float height = 0.0F;
  // Platform-resolved logical reservations already account for native control placement and layout direction.
  float left_inset = 0.0F;
  float right_inset = 0.0F;
  bool maximized = false;

  bool operator==(const WindowTitleBarMetrics&) const = default;
};

struct WindowCaptionLabels {
  StringVariant minimize = "Minimize";
  StringVariant toggle_maximize = "Maximize or restore";
  StringVariant close = "Close";

  bool operator==(const WindowCaptionLabels&) const = default;
};

struct WindowOptions {
  std::string title = "HuxerUI";
  Size initial_size = {520.0F, 360.0F};
  // Root geometry remains stable for one Runtime; pages may still override SystemBarsAppearance independently.
  WindowContentMode content_mode = WindowContentMode::SafeArea;
  // Native window chrome ownership remains stable for one Runtime.
  WindowChromeMode chrome_mode = WindowChromeMode::System;
  // Custom chrome uses this preferred logical height while preserving any larger native control minimum.
  float title_bar_height = 40.0F;
  // Accessibility labels for framework-rendered desktop caption controls.
  WindowCaptionLabels caption_labels;

  bool operator==(const WindowOptions&) const = default;
};

struct WindowMetrics {
  // The complete logical drawing surface after any platform-owned IME viewport adjustment.
  Size viewport;
  // Remaining physical obstructions relative to viewport; software-keyboard occlusion is not included.
  EdgeInsets safe_area;
  // Present only when application content actually occupies native title-bar space.
  std::optional<WindowTitleBarMetrics> title_bar;

  bool operator==(const WindowMetrics&) const = default;
};

struct SystemBarsAppearance {
  // HuxerUI paints these colors; the platform operation applies only the resolved foreground brightness.
  Color status_bar_background = Color::White();
  Color navigation_bar_background = Color::White();
  SystemBarContentBrightness status_bar_content = SystemBarContentBrightness::Automatic;
  SystemBarContentBrightness navigation_bar_content = SystemBarContentBrightness::Automatic;

  static SystemBarsAppearance Default();
  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const SystemBarsAppearance&) const = default;
};

struct SafeAreaPadding {
  // Selected edges consume only the layout-time safe area not already consumed by an ancestor.
  bool top = true;
  bool right = true;
  bool bottom = true;
  bool left = true;

  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const SafeAreaPadding&) const = default;
};

struct WindowDragRegion {
  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const WindowDragRegion&) const = default;
};

// Reserves framework- or platform-managed standard controls while leaving title-bar content application-defined.
class WindowTitleBar final : public Layout<WindowTitleBar> {
public:
  explicit WindowTitleBar(std::vector<View> children) : Layout(std::move(children)) {
    this->ApplyModifiers(WindowDragRegion{}, CrossAlign(CrossAxisAlignment::Center));
  }

  template <class... Children>
    requires(detail::ViewChild<Children> && ...)
  explicit WindowTitleBar(Children&&... children)
      : WindowTitleBar(detail::CollectChildren(std::forward<Children>(children)...)) {}

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

class WindowHandle {
public:
  void Minimize() const;
  void Maximize() const;
  void Restore() const;
  void ToggleMaximize() const;
  void Close() const;

private:
  explicit WindowHandle(std::shared_ptr<detail::WindowService> service) : service_(std::move(service)) {}

  std::shared_ptr<detail::WindowService> service_;

  friend WindowHandle UseWindow();
};

WindowHandle UseWindow();

} // namespace huxerui
