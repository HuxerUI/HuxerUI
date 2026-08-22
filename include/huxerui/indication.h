#pragma once

#include <optional>

#include <huxerui/animation.h>
#include <huxerui/modifier.h>
#include <huxerui/paint.h>

namespace huxerui {

enum class IndicationPlacement {
  BehindContent,
  AboveContent,
};

struct IndicationLayer {
  std::optional<VisualFill> fill;
  std::optional<Border> border;
  std::optional<CornerRadii> corner_radii;
  IndicationPlacement placement = IndicationPlacement::AboveContent;
  AnimationSpec enter = TweenSpec{.duration = 0.08, .easing = Easing::EaseOut};
  AnimationSpec exit = TweenSpec{.duration = 0.16, .easing = Easing::EaseOut};

  bool operator==(const IndicationLayer&) const = default;
};

struct RippleEffect {
  Color color = Color::Rgb(255, 255, 255, 0.28F);
  IndicationPlacement placement = IndicationPlacement::AboveContent;
  AnimationSpec expansion = TweenSpec{.duration = 0.32, .easing = Easing::Linear};
  AnimationSpec fade_out = TweenSpec{.duration = 0.2, .easing = Easing::Linear};

  bool operator==(const RippleEffect&) const = default;
};

struct IndicationGeometry {
  std::optional<Size> layer_size;
  std::optional<CornerRadii> clip_corner_radii;

  bool operator==(const IndicationGeometry&) const = default;
};

struct Indication {
  static const detail::ModifierDescriptor& Descriptor();

  IndicationGeometry geometry;
  std::optional<IndicationLayer> focus;
  std::optional<IndicationLayer> hover;
  std::optional<IndicationLayer> press;
  std::optional<RippleEffect> ripple;

  bool operator==(const Indication&) const = default;
};

struct FocusRing {
  Color color;
  float width = 2.0F;
  float offset = 2.0F;

  bool operator==(const FocusRing&) const = default;
};

} // namespace huxerui
