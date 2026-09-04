#pragma once

#include <any>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <typeindex>
#include <utility>

#include <huxerui/color.h>
#include <huxerui/environment.h>
#include <huxerui/indication.h>
#include <huxerui/text.h>
#include <huxerui/view.h>
#include <huxerui/window.h>

namespace huxerui {

struct ColorScheme {
  Color primary = Color::Rgb(31, 111, 235);
  Color on_primary = Color::White();
  Color primary_container = Color::Rgb(221, 235, 255);
  Color on_primary_container = Color::Rgb(12, 65, 128);
  Color secondary = Color::Rgb(87, 96, 106);
  Color on_secondary = Color::White();
  Color secondary_container = Color::Rgb(218, 225, 232);
  Color on_secondary_container = Color::Rgb(31, 35, 40);
  Color tertiary_container = Color::Rgb(236, 226, 247);
  Color on_tertiary_container = Color::Rgb(74, 45, 98);
  Color background = Color::Rgb(246, 248, 250);
  Color surface = Color::White();
  Color surface_container_low = Color::White();
  Color surface_container = Color::White();
  Color surface_container_high = Color::White();
  Color surface_container_highest = Color::Rgb(239, 241, 243);
  Color on_surface = Color::Rgb(31, 35, 40);
  Color on_surface_variant = Color::Rgb(87, 96, 106);
  Color outline = Color::Rgb(87, 96, 106);
  Color inverse_surface = Color::Rgb(31, 35, 40);
  Color inverse_on_surface = Color::White();
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);
  Color error = Color::Rgb(207, 34, 46);

  bool operator==(const ColorScheme&) const = default;
};

struct TypographyScheme {
  float body_large = 16.0F;
  float body_medium = 14.0F;
  float body_small = 12.0F;
  float label_large = 14.0F;
  float title_large = 20.0F;
  float headline_small = 24.0F;

  bool operator==(const TypographyScheme&) const = default;
};

struct ShapeScheme {
  float extra_small = 4.0F;
  float small = 8.0F;
  float medium = 12.0F;
  float large = 16.0F;
  float extra_large = 28.0F;
  float full = 10000.0F;

  bool operator==(const ShapeScheme&) const = default;
};

struct SpacingScheme {
  float extra_small = 4.0F;
  float small = 8.0F;
  float medium = 16.0F;
  float large = 24.0F;
  float extra_large = 32.0F;

  bool operator==(const SpacingScheme&) const = default;
};

struct ElevationScheme {
  float low = 2.0F;
  float medium = 8.0F;
  float high = 20.0F;

  bool operator==(const ElevationScheme&) const = default;
};

struct MotionScheme {
  double fast = 0.12;
  double normal = 0.2;
  double slow = 0.32;
  bool reduced_motion = false;

  bool operator==(const MotionScheme&) const = default;
};

struct InteractionScheme {
  Indication indication;
  FocusRing focus_ring{Color::Rgb(31, 111, 235), 2.0F, 2.0F};
  float disabled_opacity = 0.42F;

  bool operator==(const InteractionScheme&) const = default;
};

struct ThemeSpec {
  ColorScheme colors;
  TypographyScheme typography;
  ShapeScheme shapes;
  SpacingScheme spacing;
  ElevationScheme elevation;
  MotionScheme motion;
  InteractionScheme interactions;

  static ThemeSpec Default();

  bool operator==(const ThemeSpec&) const = default;
};

struct ButtonStyle {
  Color background = Color::Rgb(31, 111, 235);
  TextStyle label_style{Font::System(14.0F), Color::White()};
  Color disabled_background = Color::Rgb(31, 35, 40, 0.1F);
  Color disabled_label = Color::Rgb(31, 35, 40, 0.38F);
  EdgeInsets padding = EdgeInsets::Symmetric(14.0F, 8.0F);
  float minimum_width = 0.0F;
  float minimum_height = 0.0F;
  float corner_radius = 8.0F;
  std::optional<Indication> indication;

  static ButtonStyle Default();

  bool operator==(const ButtonStyle&) const = default;
};

struct IconButtonStyle {
  Color foreground = Color::Rgb(31, 35, 40);
  Color disabled_foreground = Color::Rgb(31, 35, 40, 0.38F);
  float icon_size = 20.0F;
  float minimum_interactive_size = 40.0F;
  float state_layer_size = 32.0F;
  float corner_radius = 4.0F;
  std::optional<Indication> indication;

  static IconButtonStyle Default();

  bool operator==(const IconButtonStyle&) const = default;
};

struct ChipStyle {
  Color background = Color::White();
  Color selected_background = Color::Rgb(31, 111, 235);
  TextStyle label_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  Color selected_label = Color::White();
  Color disabled_background = Color::Rgb(31, 35, 40, 0.08F);
  Color disabled_selected_background = Color::Rgb(31, 35, 40, 0.12F);
  Color disabled_label = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_selected_label = Color::Rgb(31, 35, 40, 0.38F);
  Color border = Color::Rgb(31, 35, 40, 0.24F);
  Color selected_border = Color::Transparent();
  Color disabled_border = Color::Rgb(31, 35, 40, 0.12F);
  Color disabled_selected_border = Color::Transparent();
  EdgeInsets padding = EdgeInsets::Symmetric(12.0F, 5.0F);
  float icon_size = 16.0F;
  float icon_spacing = 8.0F;
  float minimum_height = 28.0F;
  float corner_radius = 14.0F;
  float border_width = 1.0F;
  std::optional<Indication> indication;
  std::optional<Indication> selected_indication;

  static ChipStyle Default();

  bool operator==(const ChipStyle&) const = default;
};

struct DividerStyle {
  Color color = Color::Rgb(31, 35, 40, 0.12F);
  float thickness = 1.0F;

  static DividerStyle Default();

  bool operator==(const DividerStyle&) const = default;
};

struct SegmentedButtonStyle {
  Color background = Color::White();
  Color selected_background = Color::Rgb(31, 111, 235);
  TextStyle label_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  Color selected_label = Color::White();
  Color border = Color::Rgb(31, 35, 40, 0.24F);
  Color selected_border = Color::Rgb(31, 111, 235);
  EdgeInsets padding = EdgeInsets::Symmetric(14.0F, 7.0F);
  float icon_size = 16.0F;
  float icon_spacing = 8.0F;
  float minimum_segment_width = 48.0F;
  float minimum_height = 32.0F;
  float corner_radius = 8.0F;
  float border_width = 1.0F;
  std::optional<Indication> indication;
  std::optional<Indication> selected_indication;

  static SegmentedButtonStyle Default();

  bool operator==(const SegmentedButtonStyle&) const = default;
};

enum class TabIndicatorSizing {
  Item,
  Content,
};

struct TabsStyle {
  Color background = Color::Transparent();
  TextStyle label_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  Color selected_label = Color::Rgb(31, 111, 235);
  Color disabled_label = Color::Rgb(31, 35, 40, 0.38F);
  Color indicator = Color::Rgb(31, 111, 235);
  TabIndicatorSizing indicator_sizing = TabIndicatorSizing::Item;
  float indicator_min_width = 0.0F;
  float indicator_height = 2.0F;
  float indicator_corner_radius = 1.0F;
  Color divider_color = Color::Transparent();
  float divider_height = 0.0F;
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  float icon_size = 18.0F;
  float icon_spacing = 8.0F;
  float minimum_item_width = 48.0F;
  float minimum_height = 36.0F;
  bool expand_items = false;
  std::optional<Indication> indication;
  double indicator_animation_duration = 0.16;

  static TabsStyle Default();

  bool operator==(const TabsStyle&) const = default;
};

struct SelectStyle {
  Color background = Color::White();
  Color foreground = Color::Rgb(31, 35, 40);
  Color border = Color::Rgb(87, 96, 106, 0.55F);
  Color indicator = Color::Rgb(87, 96, 106);
  Color popup_background = Color::White();
  Color active_item_background = Color::Rgb(31, 111, 235, 0.10F);
  Color selected_item_background = Color::Rgb(31, 111, 235, 0.14F);
  Color validation_error = Color::Rgb(207, 34, 46);
  TextStyle validation_text_style{Font::System(12.0F), Color::Rgb(207, 34, 46)};
  EdgeInsets trigger_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  EdgeInsets popup_padding;
  Shadow popup_shadow{Color::Rgb(0, 0, 0, 0.2F), {}, 8.0F, 0.0F};
  float content_spacing = 8.0F;
  float validation_spacing = 4.0F;
  float minimum_width = 120.0F;
  float minimum_height = 36.0F;
  float minimum_item_height = 36.0F;
  float maximum_popup_height = 320.0F;
  float indicator_size = 16.0F;
  float corner_radius = 6.0F;
  float popup_corner_radius = 6.0F;
  float border_width = 1.0F;
  std::optional<Indication> indication;
  std::optional<Indication> item_indication;

  static SelectStyle Default();

  bool operator==(const SelectStyle&) const = default;
};

struct DatePickerStyle {
  Color background = Color::White();
  Color foreground = Color::Rgb(31, 35, 40);
  Color secondary_foreground = Color::Rgb(87, 96, 106);
  Color disabled_foreground = Color::Rgb(31, 35, 40, 0.38F);
  Color selected_background = Color::Rgb(31, 111, 235);
  Color selected_foreground = Color::White();
  Color hover_background = Color::Rgb(31, 111, 235, 0.08F);
  Color border = Color::Rgb(31, 35, 40, 0.18F);
  Color validation_error = Color::Rgb(207, 34, 46);
  TextStyle title_style{Font::System(16.0F).WithWeight(FontWeight::Medium), Color::Rgb(31, 35, 40)};
  TextStyle weekday_style{Font::System(12.0F), Color::Rgb(87, 96, 106)};
  TextStyle day_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  TextStyle label_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  TextStyle validation_text_style{Font::System(12.0F), Color::Rgb(207, 34, 46)};
  EdgeInsets padding = EdgeInsets::All(8.0F);
  float cell_size = 32.0F;
  float column_spacing = 2.0F;
  float row_spacing = 2.0F;
  float header_height = 44.0F;
  float corner_radius = 8.0F;
  float selection_corner_radius = 4.0F;
  float border_width = 1.0F;
  float label_spacing = 6.0F;
  float validation_spacing = 4.0F;

  static DatePickerStyle Default();

  bool operator==(const DatePickerStyle&) const = default;
};

struct TimePickerStyle {
  Color background = Color::White();
  Color dial_background = Color::Rgb(239, 241, 243);
  Color foreground = Color::Rgb(31, 35, 40);
  Color disabled_foreground = Color::Rgb(31, 35, 40, 0.38F);
  Color selected_background = Color::Rgb(31, 111, 235);
  Color selected_foreground = Color::White();
  /// Background of the inactive hour or minute field.
  Color field_background = Color::Rgb(239, 241, 243);
  Color selected_field_background = Color::Rgb(221, 235, 255);
  Color selected_field_foreground = Color::Rgb(12, 65, 128);
  Color selected_period_background = Color::Rgb(221, 235, 255);
  Color selected_period_foreground = Color::Rgb(12, 65, 128);
  Color period_border = Color::Rgb(31, 35, 40, 0.18F);
  Color hand = Color::Rgb(31, 111, 235);
  Color border = Color::Rgb(31, 35, 40, 0.18F);
  Color validation_error = Color::Rgb(207, 34, 46);
  TextStyle header_style{Font::System(32.0F), Color::Rgb(31, 35, 40)};
  TextStyle period_style{Font::System(12.0F).WithWeight(FontWeight::Medium), Color::Rgb(87, 96, 106)};
  TextStyle dial_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  TextStyle label_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  TextStyle validation_text_style{Font::System(12.0F), Color::Rgb(207, 34, 46)};
  EdgeInsets padding = EdgeInsets::All(12.0F);
  float dial_size = 208.0F;
  float header_height = 52.0F;
  float field_width = 64.0F;
  float separator_width = 16.0F;
  float field_corner_radius = 4.0F;
  /// AM/PM is one outlined group, omitted entirely for a 24-hour locale.
  float period_width = 44.0F;
  float period_spacing = 8.0F;
  float period_corner_radius = 4.0F;
  float period_border_width = 1.0F;
  float content_spacing = 16.0F;
  float selection_radius = 16.0F;
  float hand_width = 2.0F;
  float corner_radius = 8.0F;
  float border_width = 1.0F;
  float label_spacing = 6.0F;
  float validation_spacing = 4.0F;

  static TimePickerStyle Default();

  bool operator==(const TimePickerStyle&) const = default;
};

struct TextFieldVariantStyle {
  Color background = Color::Transparent();
  std::optional<Color> disabled_background;
  Color border = Color::Rgb(87, 96, 106, 0.55F);
  Color hovered_border = Color::Rgb(31, 35, 40);
  Color focused_border = Color::Rgb(31, 111, 235);
  Color disabled_border = Color::Rgb(31, 35, 40, 0.12F);
  float minimum_height = 36.0F;

  bool operator==(const TextFieldVariantStyle&) const = default;
};

struct TextFieldStyle {
  TextFieldVariant variant = TextFieldVariant::Standard;
  // Visual label presentation is independent from the label exposed through accessibility semantics.
  bool show_label = true;
  TextFieldVariantStyle standard{.minimum_height = 40.0F};
  TextFieldVariantStyle filled{
      .background = Color::Rgb(239, 241, 243),
      .minimum_height = 44.0F,
  };
  TextFieldVariantStyle outlined;
  TextStyle text_style;
  TextStyle label_style{Font::System(14.0F), Color::Rgb(87, 96, 106)};
  TextStyle floating_label_style{Font::System(12.0F), Color::Rgb(87, 96, 106)};
  TextStyle placeholder_style{Font::System(14.0F), Color::Rgb(87, 96, 106)};
  Color disabled_text = Color::Rgb(31, 35, 40, 0.38F);
  Color focused_label = Color::Rgb(31, 111, 235);
  Color disabled_label = Color::Rgb(31, 35, 40, 0.38F);
  Color error_label = Color::Rgb(207, 34, 46);
  Color disabled_placeholder = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_supporting_text = Color::Rgb(31, 35, 40, 0.38F);
  Color leading_icon = Color::Rgb(87, 96, 106);
  Color focused_leading_icon = Color::Rgb(87, 96, 106);
  Color disabled_leading_icon = Color::Rgb(31, 35, 40, 0.38F);
  Color error_leading_icon = Color::Rgb(207, 34, 46);
  Color trailing_icon = Color::Rgb(87, 96, 106);
  Color focused_trailing_icon = Color::Rgb(87, 96, 106);
  Color disabled_trailing_icon = Color::Rgb(31, 35, 40, 0.38F);
  Color error_trailing_icon = Color::Rgb(207, 34, 46);
  Color selection = Color::Rgb(31, 111, 235, 0.24F);
  Color caret = Color::Rgb(31, 111, 235);
  Color error_caret = Color::Rgb(207, 34, 46);
  Color composition = Color::Rgb(31, 111, 235);
  float caret_width = 2.0F;
  float border_width = 1.0F;
  float focused_border_width = 2.0F;
  float corner_radius = 6.0F;
  EdgeInsets padding = EdgeInsets::Symmetric(10.0F, 8.0F);
  float leading_icon_size = 18.0F;
  float trailing_icon_size = 18.0F;
  float icon_spacing = 8.0F;
  float label_spacing = 4.0F;
  float label_cutout_padding = 4.0F;
  double label_animation_duration = 0.12;
  double caret_blink_interval = 0.5;
  Color validation_error = Color::Rgb(207, 34, 46);
  float validation_border_width = 1.0F;
  float focused_validation_border_width = 2.0F;
  TextStyle validation_text_style{Font::System(12.0F), Color::Rgb(207, 34, 46)};
  float validation_spacing = 4.0F;

  static TextFieldStyle Default();

  bool operator==(const TextFieldStyle&) const = default;
};

/// Defines the suggestion popup appearance for ComboBox.
///
/// The editable field continues to use `TextFieldStyle`; this style owns only popup-specific presentation.
/// @code
/// ComboBoxStyle style = ComboBoxStyle::Default();
/// style.maximum_popup_height = 240.0F;
/// ThemeDefinition theme;
/// theme.Set(style);
/// @endcode
struct ComboBoxStyle {
  Color popup_background = Color::White();
  Color foreground = Color::Rgb(31, 35, 40);
  Color active_item_background = Color::Rgb(31, 111, 235, 0.10F);
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  EdgeInsets popup_padding;
  Shadow popup_shadow{Color::Rgb(0, 0, 0, 0.2F), {}, 8.0F, 0.0F};
  float minimum_item_height = 36.0F;
  float maximum_popup_height = 320.0F;
  float popup_corner_radius = 6.0F;
  std::optional<Indication> item_indication;

  static ComboBoxStyle Default();

  bool operator==(const ComboBoxStyle&) const = default;
};

struct CheckboxStyle {
  float size = 20.0F;
  float minimum_interactive_size = 20.0F;
  float state_layer_size = 20.0F;
  Color checked_background = Color::Rgb(31, 111, 235);
  Color checkmark = Color::White();
  Color unchecked_border = Color::Rgb(87, 96, 106);
  Color disabled_checked_background = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_checkmark = Color::White();
  Color disabled_unchecked_border = Color::Rgb(31, 35, 40, 0.38F);
  float border_width = 2.0F;
  float corner_radius = 4.0F;

  static CheckboxStyle Default();

  bool operator==(const CheckboxStyle&) const = default;
};

struct RadioButtonStyle {
  float size = 20.0F;
  float minimum_interactive_size = 20.0F;
  float state_layer_size = 20.0F;
  Color selected_color = Color::Rgb(31, 111, 235);
  Color unselected_color = Color::Rgb(87, 96, 106);
  Color disabled_selected_color = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_unselected_color = Color::Rgb(31, 35, 40, 0.38F);
  float border_width = 2.0F;
  float dot_radius = 5.0F;
  double animation_duration = 0.1;

  static RadioButtonStyle Default();

  bool operator==(const RadioButtonStyle&) const = default;
};

struct SwitchStyle {
  float width = 40.0F;
  float height = 24.0F;
  float minimum_interactive_height = 24.0F;
  float state_layer_size = 24.0F;
  Color unchecked_track = Color::Rgb(87, 96, 106, 0.38F);
  Color checked_track = Color::Rgb(31, 111, 235);
  Color unchecked_track_border = Color::Transparent();
  Color checked_track_border = Color::Transparent();
  Color unchecked_thumb = Color::White();
  Color checked_thumb = Color::White();
  Color disabled_unchecked_track = Color::Rgb(31, 35, 40, 0.12F);
  Color disabled_checked_track = Color::Rgb(31, 35, 40, 0.12F);
  Color disabled_unchecked_track_border = Color::Rgb(31, 35, 40, 0.12F);
  Color disabled_checked_track_border = Color::Transparent();
  Color disabled_unchecked_thumb = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_checked_thumb = Color::Rgb(31, 35, 40, 0.38F);
  float unchecked_thumb_radius = 8.0F;
  float checked_thumb_radius = 8.0F;
  float track_border_width = 0.0F;
  float corner_radius = 12.0F;
  double animation_duration = 0.2;

  static SwitchStyle Default();

  bool operator==(const SwitchStyle&) const = default;
};

enum class ProgressCircleIndeterminateMotion {
  Sweep,
  PulsingArc,
};

struct ProgressCircleStyle {
  float size = 24.0F;
  float stroke_width = 3.0F;
  Color track_color = Color::Rgb(87, 96, 106, 0.16F);
  Color indeterminate_track_color = Color::Rgb(87, 96, 106, 0.16F);
  Color indicator_color = Color::Rgb(31, 111, 235);
  float track_gap = 0.0F;
  ProgressCircleIndeterminateMotion indeterminate_motion = ProgressCircleIndeterminateMotion::Sweep;
  float minimum_indeterminate_arc_fraction = 0.28F;
  float maximum_indeterminate_arc_fraction = 0.28F;
  double animation_duration = 0.9;

  static ProgressCircleStyle Default();

  bool operator==(const ProgressCircleStyle&) const = default;
};

/// Defines the built-in indicator and retained motion used by RefreshBox.
struct RefreshBoxStyle {
  /// Appearance of the determinate pull and indeterminate refresh indicator.
  ProgressCircleStyle indicator;
  /// Fill behind the circular indicator.
  Color container_color = Color::White();
  /// Diameter of the indicator container in logical pixels.
  float container_size = 40.0F;
  /// Fraction of unconsumed direct pull retained near the leading boundary.
  float pull_resistance = 0.45F;
  /// Largest pull displacement retained before release.
  float maximum_pull_distance = 96.0F;
  /// Leading-edge displacement required to request a refresh.
  float trigger_distance = 72.0F;
  /// Content displacement retained while refreshing.
  float refresh_distance = 56.0F;
  /// Motion used to reveal or settle the retained displacement.
  TweenSpec settle_motion{0.2, Easing::EaseOut};

  static RefreshBoxStyle Default();

  bool operator==(const RefreshBoxStyle&) const = default;
};

enum class ProgressBarIndeterminateMotion {
  Sweep,
  Segmented,
};

struct ProgressBarStyle {
  float width = 160.0F;
  float height = 4.0F;
  Color track_color = Color::Rgb(87, 96, 106, 0.16F);
  Color indicator_color = Color::Rgb(31, 111, 235);
  float corner_radius = 2.0F;
  float track_gap = 0.0F;
  float stop_indicator_size = 0.0F;
  ProgressBarIndeterminateMotion indeterminate_motion = ProgressBarIndeterminateMotion::Sweep;
  // Sweep motion uses this fraction as its moving segment width; segmented motion owns its keyframed extents.
  float indeterminate_fraction = 0.35F;
  double animation_duration = 1.2;

  static ProgressBarStyle Default();

  bool operator==(const ProgressBarStyle&) const = default;
};

struct SliderStyle {
  float width = 160.0F;
  float height = 32.0F;
  float track_height = 4.0F;
  Color inactive_track = Color::Rgb(87, 96, 106, 0.2F);
  Color active_track = Color::Rgb(31, 111, 235);
  Color thumb = Color::Rgb(31, 111, 235);
  Color stop_indicator = Color::Rgb(31, 111, 235);
  Color active_tick = Color::Rgb(218, 225, 232);
  Color inactive_tick = Color::Rgb(31, 111, 235);
  Color disabled_inactive_track = Color::Rgb(31, 35, 40, 0.12F);
  Color disabled_active_track = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_thumb = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_stop_indicator = Color::Rgb(31, 35, 40, 0.38F);
  Color disabled_active_tick = Color::Rgb(31, 35, 40, 0.12F);
  Color disabled_inactive_tick = Color::Rgb(31, 35, 40, 0.38F);
  float thumb_width = 16.0F;
  float thumb_height = 16.0F;
  float hovered_thumb_width = 17.0F;
  float hovered_thumb_height = 17.0F;
  float pressed_thumb_width = 18.0F;
  float pressed_thumb_height = 18.0F;
  float thumb_track_gap = 0.0F;
  float track_inside_corner_radius = 2.0F;
  float stop_indicator_size = 0.0F;
  float tick_size = 0.0F;
  // An empty override inherits the Theme ring; zero width keeps focus in the slider's handle treatment.
  std::optional<FocusRing> focus_ring;
  double animation_duration = 0.12;

  static SliderStyle Default();

  bool operator==(const SliderStyle&) const = default;
};

class ThemeDefinition;

namespace detail {

void ApplyThemeDefinition(Environment& environment, const ThemeDefinition& definition);

} // namespace detail

class ThemeDefinition {
public:
  ThemeDefinition() = default;
  explicit ThemeDefinition(ThemeSpec theme) {
    overrides_.Set(std::move(theme));
  }

  template <EnvironmentValue Value> ThemeDefinition& Set(Value value) {
    overrides_.Set(std::move(value));
    return *this;
  }

private:
  Environment overrides_;

  friend void detail::ApplyThemeDefinition(Environment& environment, const ThemeDefinition& definition);
};

inline const ThemeSpec& UseTheme() {
  return UseEnvironment<ThemeSpec>();
}

class Theme final : public detail::TypedView<Theme> {
public:
  Theme(ThemeDefinition definition, View content);
};

ThemeSpec FlatLightThemeSpec();
ThemeSpec FlatDarkThemeSpec();
ThemeDefinition FlatThemeDefinition(ThemeSpec theme);
ThemeDefinition FlatThemeDefinition();
ThemeDefinition FlatDarkThemeDefinition();
ThemeSpec MaterialLightThemeSpec();
ThemeSpec MaterialDarkThemeSpec();
ThemeDefinition MaterialThemeDefinition(ThemeSpec theme);
ThemeDefinition MaterialThemeDefinition();
ThemeDefinition MaterialDarkThemeDefinition();

class FlatTheme final : public detail::TypedView<FlatTheme> {
public:
  explicit FlatTheme(View content);
  FlatTheme(ThemeSpec theme, View content);
};

class FlatDarkTheme final : public detail::TypedView<FlatDarkTheme> {
public:
  explicit FlatDarkTheme(View content);
};

class MaterialTheme final : public detail::TypedView<MaterialTheme> {
public:
  explicit MaterialTheme(View content);
  MaterialTheme(ThemeSpec theme, View content);
};

class MaterialDarkTheme final : public detail::TypedView<MaterialDarkTheme> {
public:
  explicit MaterialDarkTheme(View content);
};

namespace detail {

const ThemeSpec& ResolveThemeSpec(std::shared_ptr<const Environment> environment);
const std::any* FindThemeStyleValue(std::shared_ptr<const Environment> environment, std::type_index key);
TextStyle DefaultTextStyle(const ThemeSpec& theme, TextRole role = TextRole::Body);
ButtonStyle DefaultButtonStyle(const ThemeSpec& theme);
IconButtonStyle DefaultIconButtonStyle(const ThemeSpec& theme);
ChipStyle DefaultChipStyle(const ThemeSpec& theme);
DividerStyle DefaultDividerStyle(const ThemeSpec& theme);
SegmentedButtonStyle DefaultSegmentedButtonStyle(const ThemeSpec& theme);
TabsStyle DefaultTabsStyle(const ThemeSpec& theme);
SelectStyle DefaultSelectStyle(const ThemeSpec& theme);
DatePickerStyle DefaultDatePickerStyle(const ThemeSpec& theme);
TimePickerStyle DefaultTimePickerStyle(const ThemeSpec& theme);
TextFieldStyle DefaultTextFieldStyle(const ThemeSpec& theme);
ComboBoxStyle DefaultComboBoxStyle(const ThemeSpec& theme);
CheckboxStyle DefaultCheckboxStyle(const ThemeSpec& theme);
RadioButtonStyle DefaultRadioButtonStyle(const ThemeSpec& theme);
SwitchStyle DefaultSwitchStyle(const ThemeSpec& theme);
ProgressCircleStyle DefaultProgressCircleStyle(const ThemeSpec& theme);
RefreshBoxStyle DefaultRefreshBoxStyle(const ThemeSpec& theme);
ProgressBarStyle DefaultProgressBarStyle(const ThemeSpec& theme);
SliderStyle DefaultSliderStyle(const ThemeSpec& theme);

} // namespace detail

} // namespace huxerui
