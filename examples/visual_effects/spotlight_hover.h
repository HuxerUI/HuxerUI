#pragma once

#include <huxerui/huxerui.h>

namespace visual_effects {

struct SpotlightHover {
  class Extension;

  float radius = 90.0F;
  float corner_radius = 14.0F;
  double fade_in_duration = 0.4;
  double fade_out_duration = 0.18;
  huxerui::Color hover_tint = huxerui::Color::Rgb(88, 115, 168, 0.16F);

  bool operator==(const SpotlightHover&) const = default;
};

huxerui::View SpotlightButton(huxerui::StringVariant label);

} // namespace visual_effects
