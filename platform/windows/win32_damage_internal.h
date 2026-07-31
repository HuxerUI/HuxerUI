#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <huxerui/render_scene.h>

namespace huxerui::detail {

struct Win32DamageRegion {
  bool full = false;
  std::vector<RECT> rects;
};

inline Win32DamageRegion ResolveWin32Damage(const DamageRegion& damage, float scale, const RECT& client) noexcept {
  Win32DamageRegion result;
  if (damage.full || !std::isfinite(scale) || scale <= 0.0F) {
    result.full = true;
    return result;
  }

  for (const Rect& rect : damage.rects) {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height)) {
      result.full = true;
      result.rects.clear();
      return result;
    }
    if (rect.width <= 0.0F || rect.height <= 0.0F) {
      continue;
    }

    const double x = rect.x;
    const double y = rect.y;
    const double width = rect.width;
    const double height = rect.height;
    const double scale_value = scale;
    const double left = std::clamp(
        std::floor(x * scale_value) + client.left,
        static_cast<double>(client.left),
        static_cast<double>(client.right)
    );
    const double top = std::clamp(
        std::floor(y * scale_value) + client.top,
        static_cast<double>(client.top),
        static_cast<double>(client.bottom)
    );
    const double right = std::clamp(
        std::ceil((x + width) * scale_value) + client.left,
        static_cast<double>(client.left),
        static_cast<double>(client.right)
    );
    const double bottom = std::clamp(
        std::ceil((y + height) * scale_value) + client.top,
        static_cast<double>(client.top),
        static_cast<double>(client.bottom)
    );
    RECT pixel_rect{
        static_cast<LONG>(left),
        static_cast<LONG>(top),
        static_cast<LONG>(right),
        static_cast<LONG>(bottom),
    };
    if (pixel_rect.left < pixel_rect.right && pixel_rect.top < pixel_rect.bottom) {
      result.rects.push_back(pixel_rect);
    }
  }
  return result;
}

inline Rect Win32PixelRectToDips(const RECT& rect, float scale) noexcept {
  if (!std::isfinite(scale) || scale <= 0.0F) {
    return {};
  }
  return {
      static_cast<float>(rect.left) / scale,
      static_cast<float>(rect.top) / scale,
      static_cast<float>(rect.right - rect.left) / scale,
      static_cast<float>(rect.bottom - rect.top) / scale,
  };
}

inline bool Win32RectCovers(const RECT& rect, const RECT& bounds) noexcept {
  return rect.left <= bounds.left && rect.top <= bounds.top && rect.right >= bounds.right &&
         rect.bottom >= bounds.bottom;
}

} // namespace huxerui::detail
