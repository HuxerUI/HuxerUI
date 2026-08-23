#pragma once

#include <X11/Xlib.h>

#undef None
#undef Above
#undef Always
#undef Below
#undef Bool
#undef True
#undef False
#undef Success
#undef Status
#undef Unsorted

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <huxerui/render_scene.h>
#include <huxerui/window.h>

namespace huxerui::detail {

// Framework-rendered caption buttons match the modern 46-DIP interaction width used on Windows.
inline constexpr float kLinuxCaptionButtonWidth = 46.0F;
// Resize hit-test border when the window manager reports no frame extents.
inline constexpr float kLinuxResizeBorderDips = 6.0F;
// Minimum logical title-bar height so framework caption controls remain usable.
inline constexpr float kLinuxMinTitleBarHeight = 32.0F;

// Mirrors the EWMH _NET_WM_MOVERESIZE direction constants; Move requests window movement.
enum class LinuxResizeDirection : long {
  None = -1,
  TopLeft = 0,
  Top = 1,
  TopRight = 2,
  Right = 3,
  BottomRight = 4,
  Bottom = 5,
  BottomLeft = 6,
  Left = 7,
  Move = 8,
};

struct LinuxFrameExtents {
  long left = 0;
  long right = 0;
  long top = 0;
  long bottom = 0;
};

struct LinuxDamageRegion {
  bool full = false;
  std::vector<XRectangle> rects;
};

enum class LinuxFrameRenderAction {
  Skip,
  Repaint,
  PresentRetained,
};

struct LinuxTextureUploadPlan {
  bool full = false;
  std::vector<XRectangle> rects;
  std::uint64_t pixel_count = 0;
};

inline int ResolveLinuxPollTimeout(std::optional<double> deadline, double now) noexcept {
  if (!deadline.has_value()) {
    return -1;
  }
  if (std::isnan(*deadline) || std::isnan(now) || *deadline <= now) {
    return 0;
  }
  const double remaining_milliseconds = (*deadline - now) * 1000.0;
  if (!std::isfinite(remaining_milliseconds) ||
      remaining_milliseconds >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return std::max(1, static_cast<int>(std::ceil(remaining_milliseconds)));
}

inline LinuxFrameRenderAction ResolveLinuxFrameRenderAction(
    const LinuxDamageRegion& damage, bool expose_pending, bool can_present_retained
) noexcept {
  if (damage.full || !damage.rects.empty()) {
    return LinuxFrameRenderAction::Repaint;
  }
  if (!expose_pending) {
    return LinuxFrameRenderAction::Skip;
  }
  return can_present_retained ? LinuxFrameRenderAction::PresentRetained : LinuxFrameRenderAction::Repaint;
}

inline WindowTitleBarMetrics ResolveLinuxTitleBarMetrics(
    float preferred_height, Size viewport, bool maximized
) noexcept {
  const float width = std::isfinite(viewport.width) ? std::max(0.0F, viewport.width) : 0.0F;
  const float viewport_height = std::isfinite(viewport.height) ? std::max(0.0F, viewport.height) : 0.0F;
  const float preferred = std::isfinite(preferred_height) ? std::max(0.0F, preferred_height) : 0.0F;
  return {
      .height = std::min(viewport_height, std::max(preferred, kLinuxMinTitleBarHeight)),
      .left_inset = 0.0F,
      .right_inset = std::min(width, 3.0F * kLinuxCaptionButtonWidth),
      .maximized = maximized,
  };
}

inline LinuxResizeDirection ResolveLinuxResizeDirection(
    Point point, float border, Size viewport, bool maximized, std::optional<Rect> caption_bounds = std::nullopt
) noexcept {
  if (maximized || !std::isfinite(border) || border <= 0.0F) {
    return LinuxResizeDirection::None;
  }
  const float width = std::isfinite(viewport.width) ? std::max(0.0F, viewport.width) : 0.0F;
  const float height = std::isfinite(viewport.height) ? std::max(0.0F, viewport.height) : 0.0F;
  if (width <= 0.0F || height <= 0.0F) {
    return LinuxResizeDirection::None;
  }
  const float x = std::clamp(point.x, 0.0F, width);
  const float y = std::clamp(point.y, 0.0F, height);
  if (caption_bounds.has_value() &&
      x >= caption_bounds->x && x <= caption_bounds->x + caption_bounds->width &&
      y >= caption_bounds->y && y <= caption_bounds->y + caption_bounds->height) {
    return LinuxResizeDirection::None;
  }
  const bool left = x <= border;
  const bool right = x >= width - border;
  const bool top = y <= border;
  const bool bottom = y >= height - border;
  if (top && left) {
    return LinuxResizeDirection::TopLeft;
  }
  if (top && right) {
    return LinuxResizeDirection::TopRight;
  }
  if (bottom && left) {
    return LinuxResizeDirection::BottomLeft;
  }
  if (bottom && right) {
    return LinuxResizeDirection::BottomRight;
  }
  if (left) {
    return LinuxResizeDirection::Left;
  }
  if (right) {
    return LinuxResizeDirection::Right;
  }
  if (top) {
    return LinuxResizeDirection::Top;
  }
  if (bottom) {
    return LinuxResizeDirection::Bottom;
  }
  return LinuxResizeDirection::None;
}

inline bool LinuxMaximizedFromAtoms(const std::vector<Atom>& states, Atom max_h, Atom max_v) noexcept {
  bool has_horizontal = false;
  bool has_vertical = false;
  for (const Atom atom : states) {
    has_horizontal = has_horizontal || atom == max_h;
    has_vertical = has_vertical || atom == max_v;
    if (has_horizontal && has_vertical) {
      return true;
    }
  }
  return has_horizontal && has_vertical;
}

inline LinuxFrameExtents LinuxReadFrameExtents(const long* values, int count) noexcept {
  LinuxFrameExtents extents{};
  if (values != nullptr && count > 0) {
    extents.left = values[0];
    if (count > 1) {
      extents.right = values[1];
    }
    if (count > 2) {
      extents.top = values[2];
    }
    if (count > 3) {
      extents.bottom = values[3];
    }
  }
  return extents;
}

inline float LinuxResizeBorderDips(const LinuxFrameExtents& extents, float scale, float fallback) noexcept {
  const long max_extent = std::max(extents.left, extents.right);
  if (max_extent <= 0 || !std::isfinite(scale) || scale <= 0.0F) {
    return fallback;
  }
  return static_cast<float>(max_extent) / scale;
}

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

inline LinuxTextureUploadPlan ResolveLinuxTextureUpload(
    bool full_damage, const std::vector<XRectangle>& damage_rects, int width, int height, bool texture_initialized
) {
  LinuxTextureUploadPlan result;
  if (width <= 0 || height <= 0) {
    result.full = true;
    return result;
  }
  const std::uint64_t full_pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  if (full_damage || !texture_initialized || damage_rects.empty()) {
    result.full = true;
    result.pixel_count = full_pixels;
    return result;
  }

  constexpr std::size_t maximum_partial_rects = 32;
  for (const XRectangle& rect : damage_rects) {
    const int raw_left = static_cast<int>(rect.x);
    const int raw_top = static_cast<int>(rect.y);
    const int left = std::clamp(raw_left, 0, width);
    const int top = std::clamp(raw_top, 0, height);
    const int right = std::clamp(raw_left + static_cast<int>(rect.width), 0, width);
    const int bottom = std::clamp(raw_top + static_cast<int>(rect.height), 0, height);
    const int rect_width = right - left;
    const int rect_height = bottom - top;
    if (rect_width <= 0 || rect_height <= 0) {
      continue;
    }
    result.rects.push_back(
        XRectangle{
            static_cast<short>(left),
            static_cast<short>(top),
            static_cast<unsigned short>(rect_width),
            static_cast<unsigned short>(rect_height),
        }
    );
    result.pixel_count += static_cast<std::uint64_t>(rect_width) * static_cast<std::uint64_t>(rect_height);
  }

  if (result.rects.empty() || result.rects.size() > maximum_partial_rects || result.pixel_count * 2U >= full_pixels) {
    result.full = true;
    result.rects.clear();
    result.pixel_count = full_pixels;
  }
  return result;
}

} // namespace huxerui::detail
