#pragma once

#include <algorithm>

#include <huxerui/paint.h>

namespace huxerui::detail {

struct ResolvedShadow {
  Rect caster;
  Rect bounds;
  float corner_radius = 0.0F;
  float standard_deviation = 0.0F;

  [[nodiscard]] bool IsEmpty() const noexcept {
    return caster.IsEmpty();
  }
};

inline ResolvedShadow ResolveShadow(const DrawShadowCommand& command) noexcept {
  const Rect caster{
      command.rect.x + command.offset.x - command.spread,
      command.rect.y + command.offset.y - command.spread,
      command.rect.width + command.spread * 2.0F,
      command.rect.height + command.spread * 2.0F,
  };
  if (caster.IsEmpty()) {
    return {};
  }

  const float corner_radius =
      std::clamp(command.corner_radius + command.spread, 0.0F, std::min(caster.width, caster.height) * 0.5F);
  return {
      caster,
      {
          caster.x - command.blur_radius,
          caster.y - command.blur_radius,
          caster.width + command.blur_radius * 2.0F,
          caster.height + command.blur_radius * 2.0F,
      },
      corner_radius,
      command.blur_radius / 3.0F,
  };
}

} // namespace huxerui::detail
