#include <huxerui/theme.h>

#include <huxerui/modifier.h>
#include <huxerui/presentation.h>

#include "internal.h"

namespace huxerui {

namespace {

ButtonStyle MaterialButtonStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.primary,
      .label_style = TextStyle{Font::System(theme.typography.label), theme.colors.on_primary},
      .padding = EdgeInsets::Symmetric(24.0F, 10.0F),
      .corner_radius = 20.0F,
  };
}

TextFieldStyle MaterialTextFieldStyle(const ThemeSpec& theme) {
  Color placeholder = theme.colors.on_surface;
  placeholder.alpha *= 0.6F;
  Color border = theme.colors.on_surface;
  border.alpha *= 0.38F;
  return {
      .background = theme.colors.surface,
      .text_style = TextStyle{Font::System(theme.typography.body), theme.colors.on_surface},
      .placeholder_style = TextStyle{Font::System(theme.typography.body), placeholder},
      .selection =
          Color{
              theme.colors.primary.red,
              theme.colors.primary.green,
              theme.colors.primary.blue,
              0.24F,
          },
      .caret = theme.colors.primary,
      .composition = theme.colors.primary,
      .border = border,
      .focused_border = theme.colors.primary,
      .border_width = 1.0F,
      .focused_border_width = 2.0F,
      .corner_radius = theme.shapes.small,
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small + theme.spacing.extra_small),
      .minimum_height = 56.0F,
      .caret_blink_interval = theme.motion.reduced_motion ? 0.0 : 0.5,
      .validation_error = theme.colors.error,
      .validation_border_width = 2.0F,
      .validation_text_style = TextStyle{Font::System(theme.typography.label), theme.colors.error},
      .validation_spacing = theme.spacing.extra_small,
  };
}

CheckboxStyle MaterialCheckboxStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.6F;
  return {
      .size = 20.0F,
      .checked_background = theme.colors.primary,
      .checkmark = theme.colors.on_primary,
      .unchecked_border = border,
      .border_width = 2.0F,
      .corner_radius = 2.0F,
  };
}

SwitchStyle MaterialSwitchStyle(const ThemeSpec& theme) {
  Color track = theme.colors.on_surface;
  track.alpha *= 0.32F;
  return {
      .width = 52.0F,
      .height = 32.0F,
      .unchecked_track = track,
      .checked_track = theme.colors.primary,
      .thumb = theme.colors.surface,
      .thumb_radius = 12.0F,
      .track_padding = 4.0F,
      .corner_radius = 16.0F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
  };
}

ProgressCircleStyle MaterialProgressCircleStyle(const ThemeSpec& theme) {
  Color track = theme.colors.on_surface;
  track.alpha *= 0.12F;
  return {
      .size = 40.0F,
      .stroke_width = 4.0F,
      .track_color = track,
      .indicator_color = theme.colors.primary,
      .indeterminate_arc_fraction = 0.25F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow * 3.0,
  };
}

ScrollBarStyle MaterialScrollBarStyle(const ThemeSpec& theme) {
  Color thumb = theme.colors.on_surface;
  thumb.alpha *= 0.38F;
  return {
      .thickness = 4.0F,
      .minimum_thumb_extent = 24.0F,
      .margin = 4.0F,
      .corner_radius = 2.0F,
      .fade_in_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.fast),
      .fade_out_delay = 0.8F,
      .fade_out_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.normal),
      .track_color = Color::Transparent(),
      .thumb_color = thumb,
  };
}

ThemeDefinition MaterialDefinition(ThemeSpec theme) {
  ThemeDefinition definition{theme};
  definition.Set(MaterialButtonStyle(theme));
  definition.Set(MaterialTextFieldStyle(theme));
  definition.Set(MaterialCheckboxStyle(theme));
  definition.Set(MaterialSwitchStyle(theme));
  definition.Set(MaterialProgressCircleStyle(theme));
  definition.Set(ToastStyle{
      .background = theme.colors.inverse_surface,
      .foreground = theme.colors.inverse_on_surface,
      .padding = theme.spacing.small + theme.spacing.extra_small,
      .corner_radius = theme.shapes.small,
  });
  definition.Set(DialogStyle{
      .scrim = theme.colors.scrim,
  });
  definition.Set(MaterialScrollBarStyle(theme));
  return definition;
}

} // namespace

ThemeSpec ThemeSpec::Default() {
  return FlatLightThemeSpec();
}

namespace detail {

void ApplyThemeDefinition(EnvironmentValues& values, const ThemeDefinition& definition) {
  if (definition.theme_.has_value()) {
    values.Set(*definition.theme_);
  }
  MergeEnvironmentValues(values, definition.values_);
}

ThemeSpec ResolveThemeSpec(std::shared_ptr<const EnvironmentFrame> environment) {
  if (const std::any* value = FindEnvironmentValue(std::move(environment), typeid(ThemeSpec))) {
    if (const auto* theme = std::any_cast<ThemeSpec>(value)) {
      return *theme;
    }
    throw std::logic_error("HuxerUI theme environment value has an invalid type");
  }
  return ThemeSpec::Default();
}

const std::any* FindThemeStyleValue(std::shared_ptr<const EnvironmentFrame> environment, std::type_index key) {
  for (auto frame = std::move(environment); frame != nullptr; frame = frame->parent) {
    if (const std::any* value = FindLocalEnvironmentValue(frame->overrides, key)) {
      return value;
    }
    if (FindLocalEnvironmentValue(frame->overrides, typeid(ThemeSpec))) {
      return nullptr;
    }
  }
  return nullptr;
}

TextStyle DefaultTextStyle(const ThemeSpec& theme, TextRole role) {
  float font_size = theme.typography.body;
  if (role == TextRole::Label) {
    font_size = theme.typography.label;
  } else if (role == TextRole::Title) {
    font_size = theme.typography.title;
  }
  return {
      Font::System(font_size),
      theme.colors.on_surface,
  };
}

ButtonStyle DefaultButtonStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.primary,
      .label_style = TextStyle{Font::System(theme.typography.label), theme.colors.on_primary},
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .corner_radius = theme.shapes.medium,
  };
}

TextFieldStyle DefaultTextFieldStyle(const ThemeSpec& theme) {
  Color placeholder = theme.colors.on_surface;
  placeholder.alpha *= 0.55F;
  Color border = theme.colors.on_surface;
  border.alpha *= 0.4F;
  return {
      .background = theme.colors.surface,
      .text_style = TextStyle{Font::System(theme.typography.body), theme.colors.on_surface},
      .placeholder_style = TextStyle{Font::System(theme.typography.body), placeholder},
      .selection =
          Color{
              theme.colors.primary.red,
              theme.colors.primary.green,
              theme.colors.primary.blue,
              0.22F,
          },
      .caret = theme.colors.primary,
      .composition = theme.colors.primary,
      .border = border,
      .focused_border = theme.colors.primary,
      .border_width = 1.0F,
      .focused_border_width = 2.0F,
      .corner_radius = theme.shapes.small + 2.0F,
      .padding = EdgeInsets::Symmetric(10.0F, theme.spacing.small),
      .minimum_height = 36.0F,
      .caret_blink_interval = theme.motion.reduced_motion ? 0.0 : 0.5,
      .validation_error = theme.colors.error,
      .validation_border_width = 2.0F,
      .validation_text_style = TextStyle{Font::System(theme.typography.label), theme.colors.error},
      .validation_spacing = theme.spacing.extra_small,
  };
}

CheckboxStyle DefaultCheckboxStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.55F;
  return {
      .size = 20.0F,
      .checked_background = theme.colors.primary,
      .checkmark = theme.colors.on_primary,
      .unchecked_border = border,
      .border_width = 2.0F,
      .corner_radius = theme.shapes.small,
  };
}

SwitchStyle DefaultSwitchStyle(const ThemeSpec& theme) {
  Color track = theme.colors.on_surface;
  track.alpha *= 0.28F;
  return {
      .width = 40.0F,
      .height = 24.0F,
      .unchecked_track = track,
      .checked_track = theme.colors.primary,
      .thumb = theme.colors.surface,
      .thumb_radius = 8.0F,
      .track_padding = 4.0F,
      .corner_radius = 12.0F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
  };
}

ProgressCircleStyle DefaultProgressCircleStyle(const ThemeSpec& theme) {
  Color track = theme.colors.on_surface;
  track.alpha *= 0.16F;
  return {
      .size = 24.0F,
      .stroke_width = 3.0F,
      .track_color = track,
      .indicator_color = theme.colors.primary,
      .indeterminate_arc_fraction = 0.28F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow * 3.0,
  };
}

} // namespace detail

TextStyle TextStyle::Default() {
  return detail::DefaultTextStyle(ThemeSpec::Default());
}

ButtonStyle ButtonStyle::Default() {
  return detail::DefaultButtonStyle(ThemeSpec::Default());
}

TextFieldStyle TextFieldStyle::Default() {
  return detail::DefaultTextFieldStyle(ThemeSpec::Default());
}

CheckboxStyle CheckboxStyle::Default() {
  return detail::DefaultCheckboxStyle(ThemeSpec::Default());
}

SwitchStyle SwitchStyle::Default() {
  return detail::DefaultSwitchStyle(ThemeSpec::Default());
}

ProgressCircleStyle ProgressCircleStyle::Default() {
  return detail::DefaultProgressCircleStyle(ThemeSpec::Default());
}

ThemeSpec FlatLightThemeSpec() {
  ThemeSpec theme;
  theme.interactions = {
      .hover_overlay = Color::Rgb(0, 0, 0, 0.10F),
      .pressed_overlay = Color::Rgb(0, 0, 0, 0.16F),
      .ripple = Color::Rgb(255, 255, 255, 0.28F),
      .indication = IndicationKind::StateOverlay,
      .focus_ring = std::nullopt,
      .focus_ring_width = 2.0F,
      .disabled_opacity = 0.42F,
  };
  return theme;
}

ThemeSpec FlatDarkThemeSpec() {
  ThemeSpec theme;
  theme.colors = {
      .primary = Color::Rgb(88, 166, 255),
      .on_primary = Color::Rgb(13, 17, 23),
      .background = Color::Rgb(13, 17, 23),
      .surface = Color::Rgb(22, 27, 34),
      .on_surface = Color::Rgb(230, 237, 243),
      .inverse_surface = Color::Rgb(230, 237, 243),
      .inverse_on_surface = Color::Rgb(22, 27, 34),
      .scrim = Color::Rgb(0, 0, 0, 0.62F),
      .error = Color::Rgb(248, 81, 73),
  };
  theme.interactions = {
      .hover_overlay = Color::Rgb(255, 255, 255, 0.12F),
      .pressed_overlay = Color::Rgb(255, 255, 255, 0.18F),
      .ripple = Color::Rgb(255, 255, 255, 0.28F),
      .indication = IndicationKind::StateOverlay,
      .focus_ring = std::nullopt,
      .focus_ring_width = 2.0F,
      .disabled_opacity = 0.42F,
  };
  return theme;
}

ThemeSpec MaterialLightThemeSpec() {
  ThemeSpec theme;
  theme.colors = {
      .primary = Color::Rgb(103, 80, 164),
      .on_primary = Color::White(),
      .background = Color::Rgb(255, 251, 254),
      .surface = Color::Rgb(255, 251, 254),
      .on_surface = Color::Rgb(28, 27, 31),
      .inverse_surface = Color::Rgb(50, 47, 53),
      .inverse_on_surface = Color::Rgb(245, 239, 247),
      .scrim = Color::Rgb(0, 0, 0, 0.32F),
      .error = Color::Rgb(179, 38, 30),
  };
  theme.typography = {
      .body = 14.0F,
      .label = 14.0F,
      .title = 22.0F,
  };
  theme.shapes = {
      .small = 8.0F,
      .medium = 12.0F,
      .large = 28.0F,
  };
  theme.elevation = {
      .low = 1.0F,
      .medium = 3.0F,
      .high = 6.0F,
  };
  theme.motion = {
      .fast = 0.1,
      .normal = 0.2,
      .slow = 0.35,
  };
  theme.interactions = {
      .hover_overlay = Color::Rgb(255, 255, 255, 0.08F),
      .pressed_overlay = Color::Rgb(255, 255, 255, 0.12F),
      .ripple = Color::Rgb(255, 255, 255, 0.24F),
      .indication = IndicationKind::Ripple,
      .focus_ring = Color::Rgb(103, 80, 164),
      .focus_ring_width = 3.0F,
      .disabled_opacity = 0.38F,
  };
  return theme;
}

ThemeSpec MaterialDarkThemeSpec() {
  ThemeSpec theme;
  theme.colors = {
      .primary = Color::Rgb(208, 188, 255),
      .on_primary = Color::Rgb(56, 30, 114),
      .background = Color::Rgb(28, 27, 31),
      .surface = Color::Rgb(28, 27, 31),
      .on_surface = Color::Rgb(230, 225, 229),
      .inverse_surface = Color::Rgb(230, 225, 229),
      .inverse_on_surface = Color::Rgb(49, 48, 51),
      .scrim = Color::Rgb(0, 0, 0, 0.5F),
      .error = Color::Rgb(242, 184, 181),
  };
  theme.typography = {
      .body = 14.0F,
      .label = 14.0F,
      .title = 22.0F,
  };
  theme.shapes = {
      .small = 8.0F,
      .medium = 12.0F,
      .large = 28.0F,
  };
  theme.elevation = {
      .low = 1.0F,
      .medium = 3.0F,
      .high = 6.0F,
  };
  theme.motion = {
      .fast = 0.1,
      .normal = 0.2,
      .slow = 0.35,
  };
  theme.interactions = {
      .hover_overlay = Color::Rgb(56, 30, 114, 0.08F),
      .pressed_overlay = Color::Rgb(56, 30, 114, 0.12F),
      .ripple = Color::Rgb(56, 30, 114, 0.24F),
      .indication = IndicationKind::Ripple,
      .focus_ring = Color::Rgb(208, 188, 255),
      .focus_ring_width = 3.0F,
      .disabled_opacity = 0.38F,
  };
  return theme;
}

ThemeDefinition FlatThemeDefinition() {
  return ThemeDefinition{FlatLightThemeSpec()};
}

ThemeDefinition FlatDarkThemeDefinition() {
  return ThemeDefinition{FlatDarkThemeSpec()};
}

ThemeDefinition MaterialThemeDefinition(ThemeSpec theme) {
  return MaterialDefinition(std::move(theme));
}

ThemeDefinition MaterialThemeDefinition() {
  return MaterialThemeDefinition(MaterialLightThemeSpec());
}

ThemeDefinition MaterialDarkThemeDefinition() {
  return MaterialThemeDefinition(MaterialDarkThemeSpec());
}

} // namespace huxerui
