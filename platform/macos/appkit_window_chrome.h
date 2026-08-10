#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include <huxerui/window.h>

namespace huxerui::detail {

inline WindowTitleBarMetrics ResolveMacTitleBarMetrics(
    float preferred_height, float native_height, Size viewport, const std::optional<Rect>& native_controls, bool zoomed
) noexcept {
  const float width = std::isfinite(viewport.width) ? std::max(0.0F, viewport.width) : 0.0F;
  const float viewport_height = std::isfinite(viewport.height) ? std::max(0.0F, viewport.height) : 0.0F;
  const float preferred = std::isfinite(preferred_height) ? std::max(0.0F, preferred_height) : 0.0F;
  const float native = std::isfinite(native_height) ? std::max(0.0F, native_height) : 0.0F;
  float controls_right = 0.0F;
  float controls_height = 0.0F;
  if (native_controls.has_value() && std::isfinite(native_controls->x) && std::isfinite(native_controls->y) &&
      std::isfinite(native_controls->width) && std::isfinite(native_controls->height)) {
    controls_right = std::max(0.0F, native_controls->x + std::max(0.0F, native_controls->width));
    controls_height = std::max(0.0F, native_controls->height);
  }
  return {
      .height = std::min(viewport_height, std::max({preferred, native, controls_height})),
      .left_inset = std::min(width, controls_right),
      .maximized = zoomed,
  };
}

inline float ResolveMacTitleBarControlOriginY(float title_bar_height, float controls_height) noexcept {
  const float height = std::isfinite(title_bar_height) ? std::max(0.0F, title_bar_height) : 0.0F;
  const float controls = std::isfinite(controls_height) ? std::max(0.0F, controls_height) : 0.0F;
  return std::max(0.0F, (height - controls) * 0.5F);
}

} // namespace huxerui::detail
