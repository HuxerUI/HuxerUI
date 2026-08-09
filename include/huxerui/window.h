#pragma once

#include <huxerui/color.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
struct ModifierDescriptor;
}

enum class WindowContentMode {
  SafeArea,
  EdgeToEdge,
};

enum class SystemBarContentBrightness {
  Automatic,
  Light,
  Dark,
};

struct WindowMetrics {
  // The complete logical drawing surface after any platform-owned IME viewport adjustment.
  Size viewport;
  // Remaining physical obstructions relative to viewport; software-keyboard occlusion is not included.
  EdgeInsets safe_area;

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

} // namespace huxerui
