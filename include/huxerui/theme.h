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
};

struct TypographyScheme {
  float body = 14.0F;
  float label = 14.0F;
  float title = 20.0F;
};

struct ShapeScheme {
  float small = 4.0F;
  float medium = 8.0F;
  float large = 14.0F;
};

struct SpacingScheme {
  float extra_small = 4.0F;
  float small = 8.0F;
  float medium = 16.0F;
  float large = 24.0F;
  float extra_large = 32.0F;
};

struct ElevationScheme {
  float low = 2.0F;
  float medium = 8.0F;
  float high = 20.0F;
};

struct MotionScheme {
  double fast = 0.12;
  double normal = 0.2;
  double slow = 0.32;
  bool reduced_motion = false;
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
};

struct ThemeSpec {
  ColorScheme colors;
  TypographyScheme typography;
  ShapeScheme shapes;
  SpacingScheme spacing;
  ElevationScheme elevation;
  MotionScheme motion;
  InteractionScheme interactions;
};

struct TextStyle {
  Color foreground = Color::Rgb(31, 35, 40);
  float font_size = 14.0F;
};

struct TextStyleKey {
  using Value = TextStyle;

  static Value Default();
};

struct ButtonStyle {
  Color background = Color::Rgb(31, 111, 235);
  Color foreground = Color::White();
  float font_size = 14.0F;
  EdgeInsets padding = EdgeInsets::Symmetric(14.0F, 8.0F);
  float corner_radius = 8.0F;
};

struct ButtonStyleKey {
  using Value = ButtonStyle;

  static Value Default();
};

struct CheckboxStyle {
  float size = 20.0F;
  Color checked_background = Color::Rgb(31, 111, 235);
  Color checkmark = Color::White();
  Color unchecked_border = Color::Rgb(87, 96, 106);
  float border_width = 2.0F;
  float corner_radius = 4.0F;
};

struct CheckboxStyleKey {
  using Value = CheckboxStyle;

  static Value Default();
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
};

struct SwitchStyleKey {
  using Value = SwitchStyle;

  static Value Default();
};

struct ProgressCircleStyle {
  float size = 24.0F;
  float stroke_width = 3.0F;
  Color track_color = Color::Rgb(87, 96, 106, 0.16F);
  Color indicator_color = Color::Rgb(31, 111, 235);
  float indeterminate_arc_fraction = 0.28F;
  double animation_duration = 0.9;
};

struct ProgressCircleStyleKey {
  using Value = ProgressCircleStyle;

  static Value Default();
};

struct ThemeKey {
  using Value = ThemeSpec;

  static Value Default();
};

class ThemeDefinition {
public:
  ThemeDefinition() = default;
  explicit ThemeDefinition(ThemeSpec theme) : theme_(std::move(theme)) {}

  template <class Key>
    requires requires {
      typename Key::Value;
    }
  ThemeDefinition &Set(typename Key::Value value) {
    values_.Set<Key>(std::move(value));
    return *this;
  }

  [[nodiscard]] const std::optional<ThemeSpec> &
  Spec() const noexcept {
    return theme_;
  }

  [[nodiscard]] const EnvironmentValues &Values() const noexcept {
    return values_;
  }

private:
  std::optional<ThemeSpec> theme_;
  EnvironmentValues values_;
};

inline const ThemeSpec &UseTheme() {
  return UseEnvironment<ThemeKey>();
}

template <class Factory>
  requires std::invocable<Factory &> &&
           std::convertible_to<std::invoke_result_t<Factory &>, View>
View Theme(ThemeDefinition definition, Factory &&content) {
  EnvironmentValues values;
  if (definition.Spec().has_value()) {
    values.Set<ThemeKey>(*definition.Spec());
  }
  values.Merge(definition.Values());
  return ProvideEnvironment(
      std::move(values), std::forward<Factory>(content));
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
  requires std::invocable<Factory &> &&
           std::convertible_to<std::invoke_result_t<Factory &>, View>
View FlatTheme(Factory &&content) {
  return Theme(
      FlatThemeDefinition(),
      std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory &> &&
           std::convertible_to<std::invoke_result_t<Factory &>, View>
View FlatDarkTheme(Factory &&content) {
  return Theme(
      FlatDarkThemeDefinition(),
      std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory &> &&
           std::convertible_to<std::invoke_result_t<Factory &>, View>
View MaterialTheme(ThemeSpec theme, Factory &&content) {
  return Theme(
      MaterialThemeDefinition(std::move(theme)),
      std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory &> &&
           std::convertible_to<std::invoke_result_t<Factory &>, View>
View MaterialTheme(Factory &&content) {
  return Theme(
      MaterialThemeDefinition(),
      std::forward<Factory>(content));
}

template <class Factory>
  requires std::invocable<Factory &> &&
           std::convertible_to<std::invoke_result_t<Factory &>, View>
View MaterialDarkTheme(Factory &&content) {
  return Theme(
      MaterialDarkThemeDefinition(),
      std::forward<Factory>(content));
}

namespace detail {

ThemeSpec ResolveThemeSpec(
    std::shared_ptr<const EnvironmentFrame> environment);
const std::any *FindThemeStyleValue(
    std::shared_ptr<const EnvironmentFrame> environment,
    std::type_index key);
TextStyle DefaultTextStyle(
    const ThemeSpec &theme, TextRole role = TextRole::Body);
ButtonStyle DefaultButtonStyle(const ThemeSpec &theme);
CheckboxStyle DefaultCheckboxStyle(const ThemeSpec &theme);
SwitchStyle DefaultSwitchStyle(const ThemeSpec &theme);
ProgressCircleStyle DefaultProgressCircleStyle(const ThemeSpec &theme);

} // namespace detail

} // namespace huxerui

#define HUXERUI_THEME(ThemeProvider, ...)                              \
  (ThemeProvider)([=]() -> ::huxerui::View { return (__VA_ARGS__); })
