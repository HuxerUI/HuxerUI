#pragma once

#include <optional>

#include <huxerui/animation.h>
#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/modifier.h>
#include <huxerui/paint.h>

namespace huxerui {

/// Selects where an indication fill or ripple is drawn relative to a View's content.
enum class IndicationPlacement {
  /// Draws behind the content.
  BehindContent,
  /// Draws above the content.
  AboveContent,
};

/// Describes the appearance applied while an interaction layer is active.
struct IndicationLayer {
  /// Optional fill drawn within the indication bounds at the selected placement.
  std::optional<VisualFill> fill{};
  /// Overrides the View's ordinary border while this layer is active.
  std::optional<Border> border{};
  /// Overrides the corner radii of the View's surface while this layer is active.
  std::optional<CornerRadii> corner_radii{};
  /// Paint placement of the layer's fill.
  IndicationPlacement placement = IndicationPlacement::AboveContent;
  /// Transition into the layer's surface appearance.
  AnimationSpec enter = TweenSpec{.duration = 0.08, .easing = Easing::EaseOut};
  /// Transition away from the layer's surface appearance.
  AnimationSpec exit = TweenSpec{.duration = 0.16, .easing = Easing::EaseOut};
  /// Overrides the foreground of text and vector images while this layer is active.
  ///
  /// The color switches immediately as the interaction state changes. Descendants can opt in with InheritForeground.
  std::optional<Color> foreground{};

  bool operator==(const IndicationLayer&) const = default;
};

/// Describes a press ripple clipped to the indication shape.
///
/// A pointer press inside the indication bounds starts the ripple at the press position. Other presses use the center.
struct RippleEffect {
  /// Ripple color and starting opacity.
  Color color = Color::Rgb(255, 255, 255, 0.28F);
  /// Paint placement of the ripple.
  IndicationPlacement placement = IndicationPlacement::AboveContent;
  /// Motion that grows the ripple from its origin.
  AnimationSpec expansion = TweenSpec{.duration = 0.32, .easing = Easing::Linear};
  /// Motion that fades the ripple after the press is released or canceled.
  AnimationSpec fade_out = TweenSpec{.duration = 0.2, .easing = Easing::Linear};

  bool operator==(const RippleEffect&) const = default;
};

/// Configures the paint bounds and clipping shape of an indication in logical units.
struct IndicationGeometry {
  /// Optional size centered on the View. An omitted size uses the View's default indication bounds.
  std::optional<Size> layer_size{};
  /// Optional corner radii for the fill and ripple clip. An omitted value uses the View's resolved corner radii.
  std::optional<CornerRadii> clip_corner_radii{};

  bool operator==(const IndicationGeometry&) const = default;
};

/// Configures the visual feedback for a View's interaction states.
///
/// The active layer is selected in press, hover, then visible-focus order. An explicit Indication replaces the
/// default state layers and ripple. Use an empty Indication to remove those effects.
/// @code
/// return Text("Back")
///     .With(Indication{.press = IndicationLayer{.foreground = pressed_color}})
///     .OnClick(go_back);
/// @endcode
/// @see InheritForeground
struct Indication {
  /// Returns the retained-modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Bounds and clipping used to draw the feedback.
  IndicationGeometry geometry{};
  /// Optional layer shown while the View has visible focus.
  std::optional<IndicationLayer> focus{};
  /// Optional layer shown while a hover-capable pointer targets the View.
  std::optional<IndicationLayer> hover{};
  /// Optional layer shown while the View owns an active press.
  std::optional<IndicationLayer> press{};
  /// Optional ripple started by each press.
  std::optional<RippleEffect> ripple{};

  bool operator==(const Indication&) const = default;
};

/// Styles the outline drawn around an enabled View with visible focus.
struct FocusRing {
  /// Outline color, including opacity.
  Color color;
  /// Outline width in logical units. A width of zero hides the outline.
  float width = 2.0F;
  /// Gap in logical units from the indication bounds to the inside edge of the outline.
  float offset = 2.0F;

  bool operator==(const FocusRing&) const = default;
};

} // namespace huxerui
