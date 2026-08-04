#include <huxerui/theme.h>

#include <optional>

#include <huxerui/modifier.h>
#include <huxerui/presentation.h>

#include "internal.h"

namespace huxerui {

namespace {

constexpr float material_shadow_blur_per_elevation = 4.0F;

StateOverlayIndication FlatIndication(Color color, const ThemeSpec& theme) {
  Color hover = color;
  color.alpha *= 0.16F;
  hover.alpha *= 0.1F;
  return {
      .color = color,
      .fade_in_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
      .fade_out_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
      .hover_color = hover,
  };
}

RippleIndication MaterialIndication(Color color, const ThemeSpec& theme) {
  Color hover = color;
  color.alpha *= 0.16F;
  hover.alpha *= 0.08F;
  return {
      .color = color,
      .expansion_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow,
      .fade_out_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
      .hover_color = hover,
      .hover_fade_in_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
      .hover_fade_out_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
  };
}

Shadow MaterialShadow(Color color, float elevation) {
  return {color, {}, elevation * material_shadow_blur_per_elevation, 0.0F};
}

ToastStyle FlatToastStyle(const ThemeSpec& theme) {
  Color background = theme.colors.inverse_surface;
  background.alpha *= 0.94F;
  return {
      .background = background,
      .text_style = TextStyle{Font::System(theme.typography.body), theme.colors.inverse_on_surface},
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small + theme.spacing.extra_small),
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.24F), {}, theme.elevation.medium, 0.0F},
      .corner_radius = theme.shapes.medium,
      .maximum_width = 480.0F,
      .viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F},
      .placement = VerticalPlacement::Bottom,
      .motion = std::nullopt,
  };
}

DialogStyle FlatDialogStyle(const ThemeSpec& theme) {
  Color separator = theme.colors.on_surface;
  separator.alpha *= 0.12F;
  return {
      .scrim = theme.colors.scrim,
      .background = theme.colors.surface,
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.24F), {}, theme.elevation.high, 0.0F},
      .title_style =
          TextStyle{Font::System(theme.typography.title).WithWeight(FontWeight::Bold), theme.colors.on_surface},
      .message_style = TextStyle{Font::System(theme.typography.body), theme.colors.on_surface},
      .positive_action_style = TextStyle{Font::System(theme.typography.label), theme.colors.on_primary},
      .negative_action_style = TextStyle{Font::System(theme.typography.label), theme.colors.on_surface},
      .positive_action_background = theme.colors.primary,
      .negative_action_background = Color::Transparent(),
      .positive_action_indication = FlatIndication(theme.colors.on_primary, theme),
      .negative_action_indication = FlatIndication(theme.colors.on_surface, theme),
      .action_separator_color = separator,
      .content_padding = EdgeInsets::All(theme.spacing.large),
      .action_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .content_spacing = theme.spacing.small + theme.spacing.extra_small,
      .action_spacing = theme.spacing.small,
      .action_separator_thickness = 0.0F,
      .action_corner_radius = theme.shapes.small,
      .minimum_action_height = 36.0F,
      .corner_radius = theme.shapes.large,
      .maximum_width = 480.0F,
      .viewport_margin = theme.spacing.large,
      .placement = VerticalPlacement::Center,
      .content_alignment = HorizontalAlignment::Start,
      .action_layout = Axis::Horizontal,
      .action_alignment = HorizontalAlignment::End,
      .motion = PresentationMotion{
          .enter = TweenSpec{.duration = theme.motion.normal},
          .exit = TweenSpec{.duration = theme.motion.fast},
      },
  };
}

BottomSheetStyle FlatBottomSheetStyle(const ThemeSpec& theme) {
  return {
      .scrim = theme.colors.scrim,
      .background = theme.colors.surface,
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.22F), {}, theme.elevation.high, 0.0F},
      .corner_radius = theme.shapes.large,
      .maximum_width = 640.0F,
      .enter = TweenSpec{.duration = theme.motion.slow},
      .exit = TweenSpec{.duration = theme.motion.normal},
  };
}

MenuStyle FlatMenuStyle(const ThemeSpec& theme) {
  Color separator = theme.colors.on_surface;
  separator.alpha *= 0.12F;
  return {
      .background = theme.colors.surface,
      .foreground = theme.colors.on_surface,
      .item_indication = FlatIndication(theme.colors.on_surface, theme),
      .separator_color = separator,
      .separator_mode = MenuSeparatorMode::BetweenItems,
      .separator_thickness = 1.0F,
      .separator_padding = {},
      .content_padding = EdgeInsets::All(theme.spacing.extra_small),
      .item_padding = EdgeInsets::Symmetric(theme.spacing.small + theme.spacing.extra_small, theme.spacing.small),
      .item_content_spacing = theme.spacing.small,
      .icon_size = 18.0F,
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.2F), {}, theme.elevation.medium, 0.0F},
      .corner_radius = theme.shapes.medium,
      .minimum_width = 180.0F,
      .minimum_item_height = 36.0F,
      .motion = std::nullopt,
  };
}

ThemeDefinition FlatDefinition(ThemeSpec theme) {
  ThemeDefinition definition{theme};
  definition.Set(FlatToastStyle(theme));
  definition.Set(FlatDialogStyle(theme));
  definition.Set(FlatBottomSheetStyle(theme));
  definition.Set(FlatMenuStyle(theme));
  return definition;
}

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

ProgressBarStyle MaterialProgressBarStyle(const ThemeSpec& theme) {
  Color track = theme.colors.on_surface;
  track.alpha *= 0.12F;
  return {
      .width = 160.0F,
      .height = 4.0F,
      .track_color = track,
      .indicator_color = theme.colors.primary,
      .corner_radius = 2.0F,
      .indeterminate_fraction = 0.35F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow * 4.0,
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

ToastStyle MaterialToastStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.inverse_surface,
      .text_style = TextStyle{Font::System(theme.typography.body), theme.colors.inverse_on_surface},
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small + theme.spacing.extra_small),
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.18F), theme.elevation.medium),
      .corner_radius = theme.shapes.small,
      .maximum_width = 480.0F,
      .viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F},
      .placement = VerticalPlacement::Bottom,
      .motion = PresentationMotion{
          .slide_distance = 12.0F,
          .enter = TweenSpec{.duration = theme.motion.normal},
          .exit = TweenSpec{.duration = theme.motion.fast},
      },
  };
}

DialogStyle MaterialDialogStyle(const ThemeSpec& theme) {
  return {
      .scrim = theme.colors.scrim,
      .background = theme.colors.surface,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.16F), theme.elevation.high),
      .title_style = TextStyle{Font::System(24.0F), theme.colors.on_surface},
      .message_style = TextStyle{Font::System(theme.typography.body), theme.colors.on_surface},
      .positive_action_style = TextStyle{Font::System(theme.typography.label), theme.colors.primary},
      .negative_action_style = TextStyle{Font::System(theme.typography.label), theme.colors.primary},
      .positive_action_background = Color::Transparent(),
      .negative_action_background = Color::Transparent(),
      .positive_action_indication = MaterialIndication(theme.colors.primary, theme),
      .negative_action_indication = MaterialIndication(theme.colors.primary, theme),
      .action_separator_color = Color::Transparent(),
      .content_padding = EdgeInsets::All(theme.spacing.large),
      .action_padding = EdgeInsets::Symmetric(theme.spacing.small + theme.spacing.extra_small, theme.spacing.small),
      .content_spacing = theme.spacing.medium,
      .action_spacing = theme.spacing.small,
      .action_separator_thickness = 0.0F,
      .action_corner_radius = theme.shapes.large,
      .minimum_action_height = 40.0F,
      .corner_radius = theme.shapes.large,
      .maximum_width = 560.0F,
      .viewport_margin = theme.spacing.large,
      .placement = VerticalPlacement::Center,
      .content_alignment = HorizontalAlignment::Start,
      .action_layout = Axis::Horizontal,
      .action_alignment = HorizontalAlignment::End,
      .motion = PresentationMotion{
          .initial_scale = 0.94F,
          .enter = TweenSpec{.duration = theme.motion.normal},
          .exit = TweenSpec{.duration = theme.motion.fast},
      },
  };
}

BottomSheetStyle MaterialBottomSheetStyle(const ThemeSpec& theme) {
  return {
      .scrim = theme.colors.scrim,
      .background = theme.colors.surface,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.16F), theme.elevation.high),
      .corner_radius = theme.shapes.large,
      .maximum_width = 640.0F,
      .enter = TweenSpec{.duration = theme.motion.slow},
      .exit = TweenSpec{.duration = theme.motion.normal},
  };
}

MenuStyle MaterialMenuStyle(const ThemeSpec& theme) {
  Color separator = theme.colors.on_surface;
  separator.alpha *= 0.12F;
  return {
      .background = theme.colors.surface,
      .foreground = theme.colors.on_surface,
      .item_indication = MaterialIndication(theme.colors.on_surface, theme),
      .separator_color = separator,
      .separator_mode = MenuSeparatorMode::None,
      .separator_thickness = 1.0F,
      .separator_padding = {},
      .content_padding = EdgeInsets::All(theme.spacing.extra_small),
      .item_padding = EdgeInsets::Symmetric(theme.spacing.small + theme.spacing.extra_small, theme.spacing.small),
      .item_content_spacing = theme.spacing.small,
      .icon_size = 18.0F,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.18F), theme.elevation.medium),
      .corner_radius = theme.shapes.medium,
      .minimum_width = 180.0F,
      .minimum_item_height = 36.0F,
      .motion = PresentationMotion{
          .initial_scale = 0.96F,
          .enter = TweenSpec{.duration = theme.motion.fast},
          .exit = TweenSpec{.duration = theme.motion.fast},
      },
  };
}

ThemeDefinition MaterialDefinition(ThemeSpec theme) {
  ThemeDefinition definition{theme};
  definition.Set(MaterialButtonStyle(theme));
  definition.Set(MaterialTextFieldStyle(theme));
  definition.Set(MaterialCheckboxStyle(theme));
  definition.Set(MaterialSwitchStyle(theme));
  definition.Set(MaterialProgressCircleStyle(theme));
  definition.Set(MaterialProgressBarStyle(theme));
  definition.Set(MaterialScrollBarStyle(theme));
  definition.Set(MaterialToastStyle(theme));
  definition.Set(MaterialDialogStyle(theme));
  definition.Set(MaterialBottomSheetStyle(theme));
  definition.Set(MaterialMenuStyle(theme));
  return definition;
}

} // namespace

ThemeSpec ThemeSpec::Default() {
  return FlatLightThemeSpec();
}

namespace detail {

void ApplyThemeDefinition(Environment& environment, const ThemeDefinition& definition) {
  MergeEnvironment(environment, definition.overrides_);
}

ThemeSpec ResolveThemeSpec(std::shared_ptr<const Environment> environment) {
  if (const std::any* value = FindEnvironmentValue(std::move(environment), typeid(ThemeSpec))) {
    if (const auto* theme = std::any_cast<ThemeSpec>(value)) {
      return *theme;
    }
    throw std::logic_error("HuxerUI theme environment value has an invalid type");
  }
  return ThemeSpec::Default();
}

const std::any* FindThemeStyleValue(std::shared_ptr<const Environment> environment, std::type_index key) {
  for (auto current = std::move(environment); current != nullptr; current = EnvironmentParent(*current)) {
    if (const std::any* value = FindLocalEnvironmentValue(*current, key)) {
      return value;
    }
    if (FindLocalEnvironmentValue(*current, typeid(ThemeSpec))) {
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

ProgressBarStyle DefaultProgressBarStyle(const ThemeSpec& theme) {
  Color track = theme.colors.on_surface;
  track.alpha *= 0.16F;
  return {
      .width = 160.0F,
      .height = 4.0F,
      .track_color = track,
      .indicator_color = theme.colors.primary,
      .corner_radius = 2.0F,
      .indeterminate_fraction = 0.35F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow * 4.0,
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

ProgressBarStyle ProgressBarStyle::Default() {
  return detail::DefaultProgressBarStyle(ThemeSpec::Default());
}

ToastStyle ToastStyle::Default() {
  return FlatToastStyle(FlatLightThemeSpec());
}

DialogStyle DialogStyle::Default() {
  return FlatDialogStyle(FlatLightThemeSpec());
}

BottomSheetStyle BottomSheetStyle::Default() {
  return FlatBottomSheetStyle(FlatLightThemeSpec());
}

MenuStyle MenuStyle::Default() {
  return FlatMenuStyle(FlatLightThemeSpec());
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

ThemeDefinition FlatThemeDefinition(ThemeSpec theme) {
  return FlatDefinition(std::move(theme));
}

ThemeDefinition FlatThemeDefinition() {
  return FlatThemeDefinition(FlatLightThemeSpec());
}

ThemeDefinition FlatDarkThemeDefinition() {
  return FlatDefinition(FlatDarkThemeSpec());
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
