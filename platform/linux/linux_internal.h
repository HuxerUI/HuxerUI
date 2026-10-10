#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_set>

#include <huxerui/app.h>
#include <huxerui/window.h>

namespace huxerui::detail {

inline constexpr float kLinuxCaptionButtonWidth = 46.0F;
inline constexpr float kLinuxMinTitleBarHeight = 32.0F;
// Keep the visible border unchanged while making custom-chrome resize
// targets practical to acquire.
inline constexpr float kLinuxResizeBorderDips = 10.0F;
inline constexpr float kLinuxResizeCornerDips = 14.0F;

enum class LinuxResizeEdge {
  NorthWest,
  North,
  NorthEast,
  East,
  SouthEast,
  South,
  SouthWest,
  West,
};

inline std::optional<LinuxResizeEdge>
ResolveLinuxResizeEdge(Point point, Size viewport, bool disabled) noexcept {
  if (disabled || !std::isfinite(point.x) || !std::isfinite(point.y) ||
      !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
      viewport.width <= 0.0F || viewport.height <= 0.0F || point.x < 0.0F || point.y < 0.0F ||
      point.x > viewport.width || point.y > viewport.height) {
    return std::nullopt;
  }
  const bool left = point.x <= kLinuxResizeBorderDips;
  const bool right = point.x >= viewport.width - kLinuxResizeBorderDips;
  const bool top = point.y <= kLinuxResizeBorderDips;
  const bool bottom = point.y >= viewport.height - kLinuxResizeBorderDips;
  const bool left_corner = point.x <= kLinuxResizeCornerDips;
  const bool right_corner = point.x >= viewport.width - kLinuxResizeCornerDips;
  const bool top_corner = point.y <= kLinuxResizeCornerDips;
  const bool bottom_corner = point.y >= viewport.height - kLinuxResizeCornerDips;
  if (top_corner && left_corner) return LinuxResizeEdge::NorthWest;
  if (top_corner && right_corner) return LinuxResizeEdge::NorthEast;
  if (bottom_corner && left_corner) return LinuxResizeEdge::SouthWest;
  if (bottom_corner && right_corner) return LinuxResizeEdge::SouthEast;
  if (left) return LinuxResizeEdge::West;
  if (right) return LinuxResizeEdge::East;
  if (top) return LinuxResizeEdge::North;
  if (bottom) return LinuxResizeEdge::South;
  return std::nullopt;
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

inline ApplicationLifecycleState
ResolveLinuxApplicationLifecycleState(bool mapped, bool active, bool minimized) noexcept {
  if (!mapped || minimized) {
    return ApplicationLifecycleState::Background;
  }
  return active ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive;
}

struct LinuxKeyPressResult {
  bool dispatch = false;
  bool repeat = false;

  bool operator==(const LinuxKeyPressResult&) const = default;
};

class LinuxKeyTracker final {
public:
  LinuxKeyPressResult Press(std::uint32_t key_code, bool filtered_by_input_method) {
    if (filtered_by_input_method) {
      filtered_keys_.insert(key_code);
      pressed_keys_.erase(key_code);
      return {};
    }
    filtered_keys_.erase(key_code);
    return {.dispatch = true, .repeat = !pressed_keys_.insert(key_code).second};
  }

  bool Release(std::uint32_t key_code, bool filtered_by_input_method) {
    const bool filtered_press = filtered_keys_.erase(key_code) != 0;
    pressed_keys_.erase(key_code);
    return !filtered_by_input_method && !filtered_press;
  }

  void Reset() noexcept {
    pressed_keys_.clear();
    filtered_keys_.clear();
  }

private:
  std::unordered_set<std::uint32_t> pressed_keys_;
  std::unordered_set<std::uint32_t> filtered_keys_;
};

} // namespace huxerui::detail
