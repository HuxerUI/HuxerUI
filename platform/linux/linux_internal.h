#pragma once

#include <X11/Xlib.h>

// Xlib defines C macros that collide with huxerui scoped enumerators and
// standard library identifiers. Undefine them immediately after including X
// headers so shared headers below are unaffected; platform sources include
// this header first.
#undef None
#undef Bool
#undef True
#undef False
#undef Success
#undef Status
#undef Unsorted

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <huxerui/render_scene.h>

namespace huxerui::detail {

struct LinuxDamageRegion {
  bool full = false;
  std::vector<XRectangle> rects;
};

// Damage rectangles arrive in logical coordinates relative to the host view.
// Resolve them into pixel-aligned X rectangles clamped to the client area,
// mirroring the Win32 damage resolution. A full region supersedes any rects.
inline LinuxDamageRegion ResolveLinuxDamage(const DamageRegion& damage, float scale, int width, int height) noexcept {
  LinuxDamageRegion result;
  if (damage.full || !std::isfinite(scale) || scale <= 0.0F || width <= 0 || height <= 0) {
    result.full = true;
    return result;
  }

  const double scale_value = scale;
  for (const Rect& rect : damage.rects) {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) || !std::isfinite(rect.height)) {
      result.full = true;
      result.rects.clear();
      return result;
    }
    if (rect.width <= 0.0F || rect.height <= 0.0F) {
      continue;
    }

    const int left = static_cast<int>(std::clamp(std::floor(rect.x * scale_value), 0.0, static_cast<double>(width)));
    const int top = static_cast<int>(std::clamp(std::floor(rect.y * scale_value), 0.0, static_cast<double>(height)));
    const int right =
        static_cast<int>(std::clamp(std::ceil((rect.x + rect.width) * scale_value), 0.0, static_cast<double>(width)));
    const int bottom =
        static_cast<int>(std::clamp(std::ceil((rect.y + rect.height) * scale_value), 0.0, static_cast<double>(height)));
    if (right > left && bottom > top) {
      result.rects.push_back(
          XRectangle{
              static_cast<short>(left),
              static_cast<short>(top),
              static_cast<unsigned short>(right - left),
              static_cast<unsigned short>(bottom - top),
          }
      );
    }
  }
  return result;
}

// Converts a DIP position into device pixels, mirroring Win32PixelRectToDips.
inline Point LinuxPixelsToDips(const Point& pixel, float scale) noexcept {
  if (!std::isfinite(scale) || scale <= 0.0F) {
    return pixel;
  }
  return {
      static_cast<float>(pixel.x) / scale,
      static_cast<float>(pixel.y) / scale,
  };
}

} // namespace huxerui::detail
