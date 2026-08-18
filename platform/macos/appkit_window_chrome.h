#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include <huxerui/window.h>

namespace huxerui::detail {

inline WindowTitleBarMetrics ResolveMacTitleBarMetrics(
    float preferred_height, float system_height, Size viewport, const std::optional<Rect>& system_controls, bool zoomed
) noexcept {
  const float width = std::isfinite(viewport.width) ? std::max(0.0F, viewport.width) : 0.0F;
  const float viewport_height = std::isfinite(viewport.height) ? std::max(0.0F, viewport.height) : 0.0F;
  const float preferred = std::isfinite(preferred_height) ? std::max(0.0F, preferred_height) : 0.0F;
  const float system = std::isfinite(system_height) ? std::max(0.0F, system_height) : 0.0F;
  float controls_right = 0.0F;
  float controls_height = 0.0F;
  if (system_controls.has_value() && std::isfinite(system_controls->x) && std::isfinite(system_controls->y) &&
      std::isfinite(system_controls->width) && std::isfinite(system_controls->height)) {
    controls_right = std::max(0.0F, system_controls->x + std::max(0.0F, system_controls->width));
    controls_height = std::max(0.0F, system_controls->height);
  }
  return {
      .height = std::min(viewport_height, std::max({preferred, system, controls_height})),
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
