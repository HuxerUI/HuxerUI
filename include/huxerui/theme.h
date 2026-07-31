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
#include <huxerui/view.h>

namespace huxerui {

struct ColorScheme {
  Color primary = Color::Rgb(31, 111, 235);
  Color on_primary = Color::White();
  Color background = Color::Rgb(246, 248, 250);
  Color surface = Color::White();
  Color on_surface = Color::Rgb(31, 35, 40);
  Color inverse_surface = Color::Rgb(31, 35, 40);
  Color inverse_on_surface = Color::White();
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);
  Color error = Color::Rgb(207, 34, 46);

  bool operator==(const ColorScheme&) const = default;
};

struct TypographyScheme {
  float body = 14.0F;
  float label = 14.0F;
  float title = 20.0F;

  bool operator==(const TypographyScheme&) const = default;
};

struct ShapeScheme {
  float small = 4.0F;
  float medium = 8.0F;
  float large = 14.0F;

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

enum class IndicationKind {
  StateOverlay,
  Ripple,
};

struct InteractionScheme {
  Color hover_overlay = Color::Rgb(0, 0, 0, 0.06F);
  Color pressed_overlay = Color::Rgb(0, 0, 0, 0.12F);
  Color ripple = Color::Rgb(255, 255, 255, 0.28F);
  IndicationKind indication = IndicationKind::StateOverlay;
  std::optional<Color> focus_ring;
  float focus_ring_width = 2.0F;
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

struct TextStyle {
  Color foreground = Color::Rgb(31, 35, 40);
  float font_size = 14.0F;

  static TextStyle Default();

  bool operator==(const TextStyle&) const = default;
};

struct ButtonStyle {
  Color background = Color::Rgb(31, 111, 235);
  Color foreground = Color::White();
  float font_size = 14.0F;
  EdgeInsets padding = EdgeInsets::Symmetric(14.0F, 8.0F);
  float corner_radius = 8.0F;

  static ButtonStyle Default();

  bool operator==(const ButtonStyle&) const = default;
};

struct TextFieldStyle {
  Color background = Color::White();
  Color foreground = Color::Rgb(31, 35, 40);
  Color placeholder = Color::Rgb(87, 96, 106);
  Color selection = Color::Rgb(31, 111, 235, 0.24F);
  Color caret = Color::Rgb(31, 111, 235);
  Color composition = Color::Rgb(31, 111, 235);
  Color border = Color::Rgb(87, 96, 106, 0.55F);
  Color focused_border = Color::Rgb(31, 111, 235);
  float border_width = 1.0F;
  float focused_border_width = 2.0F;
  float font_size = 14.0F;
  float corner_radius = 6.0F;
  EdgeInsets padding = EdgeInsets::Symmetric(10.0F, 8.0F);
  float minimum_height = 36.0F;
  double caret_blink_interval = 0.5;
  Color validation_error = Color::Rgb(207, 34, 46);
  float validation_border_width = 2.0F;
  float validation_font_size = 12.0F;
  float validation_spacing = 4.0F;

  static TextFieldStyle Default();

  bool operator==(const TextFieldStyle&) const = default;
};

struct CheckboxStyle {
  float size = 20.0F;
  Color checked_background = Color::Rgb(31, 111, 235);
  Color checkmark = Color::White();
  Color unchecked_border = Color::Rgb(87, 96, 106);
  float border_width = 2.0F;
  float corner_radius = 4.0F;

  static CheckboxStyle Default();

  bool operator==(const CheckboxStyle&) const = default;
};

struct SwitchStyle {
  float width = 40.0F;
  float height = 24.0F;
  Color unchecked_track = Color::Rgb(87, 96, 106, 0.38F);
  Color checked_track = Color::Rgb(31, 111, 235);
  Color thumb = Color::White();
  float thumb_radius = 8.0F;
  float track_padding = 4.0F;
  float corner_radius = 12.0F;
  double animation_duration = 0.2;

  static SwitchStyle Default();

  bool operator==(const SwitchStyle&) const = default;
};

struct ProgressCircleStyle {
  float size = 24.0F;
  float stroke_width = 3.0F;
  Color track_color = Color::Rgb(87, 96, 106, 0.16F);
  Color indicator_color = Color::Rgb(31, 111, 235);
  float indeterminate_arc_fraction = 0.28F;
  double animation_duration = 0.9;

  static ProgressCircleStyle Default();

  bool operator==(const ProgressCircleStyle&) const = default;
};

class ThemeDefinition;

namespace detail {

void ApplyThemeDefinition(EnvironmentValues& values, const ThemeDefinition& definition);

} // namespace detail

class ThemeDefinition {
public:
  ThemeDefinition() = default;
  explicit ThemeDefinition(ThemeSpec theme) : theme_(std::move(theme)) {}

  template <EnvironmentValue Value> ThemeDefinition& Set(Value value) {
    values_.Set(std::move(value));
    return *this;
  }

private:
  std::optional<ThemeSpec> theme_;
  EnvironmentValues values_;

  friend void detail::ApplyThemeDefinition(EnvironmentValues& values, const ThemeDefinition& definition);
};

inline const ThemeSpec& UseTheme() {
  return UseEnvironment<ThemeSpec>();
}

template <class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View Theme(ThemeDefinition definition, Factory&& content) {
  EnvironmentValues values;
  detail::ApplyThemeDefinition(values, definition);
  return ProvideEnvironment(std::move(values), std::forward<Factory>(content));
}

ThemeSpec FlatLightThemeSpec();
ThemeSpec FlatDarkThemeSpec();
ThemeDefinition FlatThemeDefinition();
ThemeDefinition FlatDarkThemeDefinition();
ThemeSpec MaterialLightThemeSpec();
ThemeSpec MaterialDarkThemeSpec();
ThemeDefinition MaterialThemeDefinition(ThemeSpec theme);
ThemeDefinition MaterialThemeDefinition();
ThemeDefinition MaterialDarkThemeDefinition();

template <class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View FlatTheme(Factory&& content) {
  return Theme(FlatThemeDefinition(), std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View FlatDarkTheme(Factory&& content) {
  return Theme(FlatDarkThemeDefinition(), std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View MaterialTheme(ThemeSpec theme, Factory&& content) {
  return Theme(MaterialThemeDefinition(std::move(theme)), std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View MaterialTheme(Factory&& content) {
  return Theme(MaterialThemeDefinition(), std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View MaterialDarkTheme(Factory&& content) {
  return Theme(MaterialDarkThemeDefinition(), std::forward<Factory>(content));
}

namespace detail {

ThemeSpec ResolveThemeSpec(std::shared_ptr<const EnvironmentFrame> environment);
const std::any* FindThemeStyleValue(std::shared_ptr<const EnvironmentFrame> environment, std::type_index key);
TextStyle DefaultTextStyle(const ThemeSpec& theme, TextRole role = TextRole::Body);
ButtonStyle DefaultButtonStyle(const ThemeSpec& theme);
TextFieldStyle DefaultTextFieldStyle(const ThemeSpec& theme);
CheckboxStyle DefaultCheckboxStyle(const ThemeSpec& theme);
SwitchStyle DefaultSwitchStyle(const ThemeSpec& theme);
ProgressCircleStyle DefaultProgressCircleStyle(const ThemeSpec& theme);

} // namespace detail

} // namespace huxerui

#define HUXERUI_THEME(ThemeProvider, ...) (ThemeProvider)([=]() -> ::huxerui::View { return (__VA_ARGS__); })
