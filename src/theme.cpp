#include <huxerui/theme.h>

#include <optional>

#include <huxerui/modifier.h>
#include <huxerui/navigation.h>
#include <huxerui/presentation.h>

#include "view_internal.h"

namespace huxerui {

namespace {

constexpr float material_shadow_blur_per_elevation = 4.0F;

AnimationSpec InteractionTween(double duration) {
  return TweenSpec{.duration = duration, .easing = Easing::EaseOut};
}

Indication FlatIndication(Color color, const ThemeSpec& theme, float hover_opacity = 0.10F,
                          float press_opacity = 0.16F) {
  Color hover = color;
  color.alpha *= press_opacity;
  hover.alpha *= hover_opacity;
  return {
      .hover = IndicationLayer{
          .fill = hover,
          .enter = InteractionTween(theme.motion.reduced_motion ? 0.0 : theme.motion.fast),
          .exit = InteractionTween(theme.motion.reduced_motion ? 0.0 : theme.motion.normal),
      },
      .press = IndicationLayer{
          .fill = color,
          .enter = InteractionTween(theme.motion.reduced_motion ? 0.0 : theme.motion.fast),
          .exit = InteractionTween(theme.motion.reduced_motion ? 0.0 : theme.motion.normal),
      },
  };
}

Indication MaterialIndication(Color color, const ThemeSpec& theme) {
  Color hover = color;
  color.alpha *= 0.12F;
  hover.alpha *= 0.08F;
  return {
      .hover = IndicationLayer{
          .fill = hover,
          .enter = InteractionTween(theme.motion.reduced_motion ? 0.0 : theme.motion.fast),
          .exit = InteractionTween(theme.motion.reduced_motion ? 0.0 : theme.motion.normal),
      },
      .ripple = RippleEffect{
          .color = color,
          .expansion = TweenSpec{
              .duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow,
              .easing = Easing::Linear,
          },
          .fade_out = TweenSpec{
              .duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
              .easing = Easing::Linear,
          },
      },
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
      .text_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.inverse_on_surface},
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small + theme.spacing.extra_small),
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.24F), {}, theme.elevation.medium, 0.0F},
      .corner_radius = theme.shapes.small,
      .maximum_width = 480.0F,
      .viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F},
      .placement = VerticalPlacement::Bottom,
      .motion = std::nullopt,
  };
}

SnackBarStyle FlatSnackBarStyle(const ThemeSpec& theme) {
  Color background = theme.colors.inverse_surface;
  background.alpha *= 0.98F;
  return {
      .background = background,
      .message_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.inverse_on_surface},
      .action_text_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.primary},
      .action_background = Color::Transparent(),
      .action_padding = EdgeInsets::Symmetric(theme.spacing.small + theme.spacing.extra_small, theme.spacing.small),
      .action_minimum_height = 36.0F,
      .action_corner_radius = theme.shapes.small,
      .action_indication = FlatIndication(theme.colors.primary, theme),
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .content_spacing = theme.spacing.small,
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.24F), {}, theme.elevation.medium, 0.0F},
      .corner_radius = theme.shapes.small,
      .minimum_height = 48.0F,
      .maximum_width = 600.0F,
      .viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F},
      .motion = std::nullopt,
  };
}

TooltipStyle FlatTooltipStyle(const ThemeSpec& theme) {
  Color background = theme.colors.inverse_surface;
  background.alpha *= 0.94F;
  return {
      .background = background,
      .text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.inverse_on_surface},
      .padding = EdgeInsets::Symmetric(theme.spacing.small, theme.spacing.extra_small),
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.18F), {}, theme.elevation.low, 0.0F},
      .corner_radius = theme.shapes.extra_small,
      .minimum_height = 24.0F,
      .maximum_width = 320.0F,
      .placement = AnchorPlacement{AnchorSide::Above, AnchorAlignment::Center},
      .gap = theme.spacing.extra_small,
      .viewport_margin = theme.spacing.small,
      .hover_delay = 0.5,
      .exit_delay = 0.1,
      .long_press_delay = 0.5,
      .touch_show_duration = 1.5,
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
          TextStyle{Font::System(theme.typography.title_large).WithWeight(FontWeight::Bold), theme.colors.on_surface},
      .message_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .positive_action_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_primary},
      .negative_action_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_surface},
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
      .action_corner_radius = theme.shapes.extra_small,
      .minimum_action_height = 36.0F,
      .corner_radius = theme.shapes.large,
      .minimum_width = 0.0F,
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
      .corner_radii = CornerRadii::Top(theme.shapes.large),
      .drag_handle = Color::Transparent(),
      .drag_handle_size = {},
      .drag_handle_padding = {},
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
      .icon_tint = theme.colors.on_surface,
      .item_indication = FlatIndication(theme.colors.on_surface, theme),
      .separator_color = separator,
      .separator_mode = MenuSeparatorMode::BetweenItems,
      .separator_thickness = 1.0F,
      .separator_padding = {},
      .content_padding = {},
      .item_padding = EdgeInsets::Symmetric(theme.spacing.small + theme.spacing.extra_small, theme.spacing.small),
      .item_content_spacing = theme.spacing.small,
      .icon_size = 18.0F,
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.2F), {}, theme.elevation.medium, 0.0F},
      .corner_radius = theme.shapes.small,
      .minimum_width = 180.0F,
      .minimum_item_height = 36.0F,
      .motion = std::nullopt,
  };
}

NavigationStyle FlatNavigationStyle(const ThemeSpec& theme) {
  return {
      .motion = NavigationMotion{
          .entering_offset_fraction = {1.0F, 0.0F},
          .covered_offset_fraction = {-1.0F, 0.0F},
          .push = TweenSpec{.duration = theme.motion.normal},
          .pop = TweenSpec{.duration = theme.motion.normal},
      },
  };
}

TopAppBarStyle FlatTopAppBarStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.surface,
      .title_style =
          TextStyle{Font::System(theme.typography.title_large).WithWeight(FontWeight::Bold), theme.colors.on_surface},
      .height = 48.0F,
      .horizontal_padding = theme.spacing.small,
      .title_inset = theme.spacing.large,
      .title_spacing = theme.spacing.extra_small,
      .action_spacing = 0.0F,
  };
}

NavigationBarStyle FlatNavigationBarStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = theme.colors.surface,
      .label_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.on_surface_variant},
      .selected_content = theme.colors.primary,
      .disabled_content = disabled,
      .indicator = theme.colors.surface_container_highest,
      .indicator_size = {56.0F, 32.0F},
      .item_padding = EdgeInsets::Symmetric(theme.spacing.extra_small, theme.spacing.extra_small),
      .icon_size = 22.0F,
      .icon_spacing = theme.spacing.extra_small,
      .minimum_item_width = 64.0F,
      .height = 64.0F,
      .indicator_corner_radius = theme.shapes.small,
      .show_unselected_labels = true,
      .indication = FlatIndication(theme.colors.on_surface, theme),
      .selection_animation = TweenSpec{.duration = theme.motion.fast},
  };
}

NavigationPaneStyle FlatNavigationPaneStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = theme.colors.surface,
      .label_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface_variant},
      .selected_content = theme.colors.primary,
      .disabled_content = disabled,
      .indicator = theme.colors.surface_container_highest,
      .item_margin = EdgeInsets::Symmetric(theme.spacing.small, 0.0F),
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, 0.0F),
      .compact_width = 72.0F,
      .expanded_min_width = 256.0F,
      .item_height = 48.0F,
      .icon_size = 22.0F,
      .icon_spacing = theme.spacing.medium,
      .compact_indicator_size = {56.0F, 32.0F},
      .indicator_corner_radius = theme.shapes.small,
      .indication = FlatIndication(theme.colors.on_surface, theme),
      .selection_animation = TweenSpec{.duration = theme.motion.fast},
  };
}

DrawerStyle FlatDrawerStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.surface,
      .scrim = theme.colors.scrim,
      .shadow = Shadow{Color::Rgb(0, 0, 0, 0.22F), {}, theme.elevation.medium, 0.0F},
      .preferred_width = 320.0F,
      .minimum_width = 240.0F,
      .minimum_content_width = 360.0F,
      .modal_content_reveal = 56.0F,
      .edge_drag_width = 24.0F,
      .corner_radius = theme.shapes.large,
      .motion = theme.motion.reduced_motion ? std::optional<DrawerMotion>{std::nullopt}
                                            : std::optional<DrawerMotion>{DrawerMotion{
                                                  .open = TweenSpec{.duration = theme.motion.slow},
                                                  .close = TweenSpec{.duration = theme.motion.normal},
                                              }},
  };
}

ThemeDefinition FlatDefinition(ThemeSpec theme) {
  ThemeDefinition definition{theme};
  definition.Set(
      SystemBarsAppearance{
          .status_bar_background = theme.colors.surface,
          .navigation_bar_background = theme.colors.surface,
      }
  );
  definition.Set(FlatToastStyle(theme));
  definition.Set(FlatSnackBarStyle(theme));
  definition.Set(FlatTooltipStyle(theme));
  definition.Set(FlatDialogStyle(theme));
  definition.Set(FlatBottomSheetStyle(theme));
  definition.Set(FlatMenuStyle(theme));
  definition.Set(FlatNavigationStyle(theme));
  definition.Set(FlatTopAppBarStyle(theme));
  definition.Set(FlatNavigationBarStyle(theme));
  definition.Set(FlatNavigationPaneStyle(theme));
  definition.Set(FlatDrawerStyle(theme));
  definition.Set(detail::DefaultRefreshBoxStyle(theme));
  return definition;
}

ButtonStyle MaterialButtonStyle(const ThemeSpec& theme) {
  Color disabled_background = theme.colors.on_surface;
  disabled_background.alpha *= 0.12F;
  Color disabled_label = theme.colors.on_surface;
  disabled_label.alpha *= 0.38F;
  return {
      .background = theme.colors.primary,
      .label_style =
          TextStyle{Font::System(theme.typography.label_large).WithWeight(FontWeight::Medium), theme.colors.on_primary},
      .disabled_background = disabled_background,
      .disabled_label = disabled_label,
      .padding = EdgeInsets::Symmetric(24.0F, 8.0F),
      .minimum_width = 58.0F,
      .minimum_height = 40.0F,
      .corner_radius = 20.0F,
      .indication = MaterialIndication(theme.colors.on_primary, theme),
  };
}

IconButtonStyle MaterialIconButtonStyle(const ThemeSpec& theme) {
  Color disabled_foreground = theme.colors.on_surface;
  disabled_foreground.alpha *= 0.38F;
  return {
      .foreground = theme.colors.on_surface_variant,
      .disabled_foreground = disabled_foreground,
      .icon_size = 24.0F,
      .minimum_interactive_size = 48.0F,
      .state_layer_size = 40.0F,
      .corner_radius = 24.0F,
      .indication = MaterialIndication(theme.colors.on_surface_variant, theme),
  };
}

ChipStyle MaterialChipStyle(const ThemeSpec& theme) {
  Color disabled_container = theme.colors.on_surface;
  disabled_container.alpha *= 0.12F;
  Color disabled_content = theme.colors.on_surface;
  disabled_content.alpha *= 0.38F;
  Color disabled_border = theme.colors.on_surface;
  disabled_border.alpha *= 0.12F;
  return {
      .background = Color::Transparent(),
      .selected_background = theme.colors.secondary_container,
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_surface_variant},
      .selected_label = theme.colors.on_secondary_container,
      .disabled_background = Color::Transparent(),
      .disabled_selected_background = disabled_container,
      .disabled_label = disabled_content,
      .disabled_selected_label = disabled_content,
      .border = theme.colors.outline,
      .selected_border = Color::Transparent(),
      .disabled_border = disabled_border,
      .disabled_selected_border = Color::Transparent(),
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.extra_small),
      .icon_size = 18.0F,
      .icon_spacing = theme.spacing.small,
      .minimum_height = 32.0F,
      .corner_radius = theme.shapes.small,
      .border_width = 1.0F,
      .indication = MaterialIndication(theme.colors.on_surface_variant, theme),
      .selected_indication = MaterialIndication(theme.colors.on_secondary_container, theme),
  };
}

SegmentedButtonStyle MaterialSegmentedButtonStyle(const ThemeSpec& theme) {
  return {
      .background = Color::Transparent(),
      .selected_background = theme.colors.secondary_container,
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_surface},
      .selected_label = theme.colors.on_secondary_container,
      .border = theme.colors.outline,
      .selected_border = theme.colors.outline,
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .icon_size = 18.0F,
      .icon_spacing = theme.spacing.small,
      .minimum_segment_width = 48.0F,
      .minimum_height = 40.0F,
      .corner_radius = 20.0F,
      .border_width = 1.0F,
      .indication = MaterialIndication(theme.colors.on_surface, theme),
      .selected_indication = MaterialIndication(theme.colors.on_secondary_container, theme),
  };
}

TabsStyle MaterialTabsStyle(const ThemeSpec& theme) {
  Color disabled_label = theme.colors.on_surface;
  disabled_label.alpha *= 0.38F;
  Color divider_color = theme.colors.outline;
  divider_color.alpha *= 0.4F;
  return {
      .background = theme.colors.surface,
      .label_style = TextStyle{
          Font::System(theme.typography.label_large).WithWeight(FontWeight::Medium),
          theme.colors.on_surface_variant,
      },
      .selected_label = theme.colors.primary,
      .disabled_label = disabled_label,
      .indicator = theme.colors.primary,
      .indicator_sizing = TabIndicatorSizing::Content,
      .indicator_min_width = 24.0F,
      .indicator_height = 3.0F,
      .indicator_corner_radius = 1.5F,
      .divider_color = divider_color,
      .divider_height = 1.0F,
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .icon_size = 24.0F,
      .icon_spacing = theme.spacing.small,
      .minimum_item_width = 64.0F,
      .minimum_height = 48.0F,
      .expand_items = true,
      .indication = MaterialIndication(theme.colors.primary, theme),
      .indicator_animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
  };
}

SelectStyle MaterialSelectStyle(const ThemeSpec& theme) {
  Color active_item_background = theme.colors.primary;
  active_item_background.alpha *= 0.08F;
  Color selected_item_background = theme.colors.primary;
  selected_item_background.alpha *= 0.12F;
  return {
      .background = theme.colors.surface_container_highest,
      .foreground = theme.colors.on_surface,
      .border = theme.colors.outline,
      .indicator = theme.colors.on_surface_variant,
      .popup_background = theme.colors.surface_container,
      .active_item_background = active_item_background,
      .selected_item_background = selected_item_background,
      .validation_error = theme.colors.error,
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .trigger_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .popup_padding = {},
      .popup_shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.24F), theme.elevation.medium),
      .content_spacing = theme.spacing.small,
      .validation_spacing = theme.spacing.extra_small,
      .minimum_width = 120.0F,
      .minimum_height = 48.0F,
      .minimum_item_height = 40.0F,
      .maximum_popup_height = 320.0F,
      .indicator_size = 20.0F,
      .corner_radius = theme.shapes.extra_small,
      .popup_corner_radius = theme.shapes.small,
      .border_width = 1.0F,
      .indication = MaterialIndication(theme.colors.on_surface, theme),
      .item_indication = MaterialIndication(theme.colors.on_surface, theme),
  };
}

DatePickerStyle MaterialDatePickerStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  Color hover = theme.colors.primary;
  hover.alpha *= 0.08F;
  return {
      .background = theme.colors.surface_container_high,
      .foreground = theme.colors.on_surface,
      .secondary_foreground = theme.colors.on_surface_variant,
      .disabled_foreground = disabled,
      .selected_background = theme.colors.primary,
      .selected_foreground = theme.colors.on_primary,
      .hover_background = hover,
      .border = theme.colors.outline,
      .validation_error = theme.colors.error,
      .title_style = TextStyle{
          Font::System(theme.typography.body_large).WithWeight(FontWeight::Medium),
          theme.colors.on_surface,
      },
      .weekday_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface_variant},
      .day_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .label_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .padding = EdgeInsets::All(theme.spacing.small + theme.spacing.extra_small),
      .cell_size = 40.0F,
      .column_spacing = 4.0F,
      .row_spacing = 4.0F,
      .header_height = 48.0F,
      .corner_radius = theme.shapes.extra_large,
      .selection_corner_radius = theme.shapes.full,
      .border_width = 0.0F,
      .label_spacing = theme.spacing.small,
      .validation_spacing = theme.spacing.extra_small,
  };
}

TimePickerStyle MaterialTimePickerStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = theme.colors.surface_container_high,
      .dial_background = theme.colors.surface_container_highest,
      .foreground = theme.colors.on_surface,
      .disabled_foreground = disabled,
      .selected_background = theme.colors.primary,
      .selected_foreground = theme.colors.on_primary,
      .field_background = theme.colors.surface_container_highest,
      .selected_field_background = theme.colors.primary_container,
      .selected_field_foreground = theme.colors.on_primary_container,
      .selected_period_background = theme.colors.tertiary_container,
      .selected_period_foreground = theme.colors.on_tertiary_container,
      .period_border = theme.colors.outline,
      .hand = theme.colors.primary,
      .border = theme.colors.outline,
      .validation_error = theme.colors.error,
      .header_style = TextStyle{
          Font::System(56.0F),
          theme.colors.on_surface,
      },
      .period_style = TextStyle{
          Font::System(theme.typography.body_large).WithWeight(FontWeight::Medium),
          theme.colors.on_surface_variant,
      },
      .dial_style = TextStyle{Font::System(theme.typography.body_large), theme.colors.on_surface},
      .label_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .padding = EdgeInsets::All(theme.spacing.large),
      .dial_size = 256.0F,
      .header_height = 80.0F,
      .field_width = 96.0F,
      .separator_width = 24.0F,
      .field_corner_radius = theme.shapes.small,
      .period_width = 52.0F,
      .period_spacing = theme.spacing.small + theme.spacing.extra_small,
      .period_corner_radius = theme.shapes.small,
      .period_border_width = 1.0F,
      .content_spacing = theme.spacing.large,
      .selection_radius = 20.0F,
      .hand_width = 2.0F,
      .corner_radius = theme.shapes.extra_large,
      .border_width = 0.0F,
      .label_spacing = theme.spacing.small,
      .validation_spacing = theme.spacing.extra_small,
  };
}

DividerStyle MaterialDividerStyle(const ThemeSpec& theme) {
  Color color = theme.colors.outline;
  color.alpha *= 0.4F;
  return {
      .color = color,
      .thickness = 1.0F,
  };
}

TextFieldStyle MaterialTextFieldStyle(const ThemeSpec& theme) {
  Color disabled_content = theme.colors.on_surface;
  disabled_content.alpha *= 0.38F;
  Color disabled_indicator = theme.colors.on_surface;
  disabled_indicator.alpha *= 0.38F;
  Color disabled_outline = theme.colors.on_surface;
  disabled_outline.alpha *= 0.12F;
  Color disabled_container = theme.colors.on_surface;
  disabled_container.alpha *= 0.04F;
  return {
      .variant = TextFieldVariant::Filled,
      .show_label = true,
      .standard =
          {
              .background = Color::Transparent(),
              .border = theme.colors.on_surface_variant,
              .hovered_border = theme.colors.on_surface,
              .focused_border = theme.colors.primary,
              .disabled_border = disabled_indicator,
              .minimum_height = 56.0F,
          },
      .filled =
          {
              .background = theme.colors.surface_container_highest,
              .disabled_background = disabled_container,
              .border = theme.colors.on_surface_variant,
              .hovered_border = theme.colors.on_surface,
              .focused_border = theme.colors.primary,
              .disabled_border = disabled_indicator,
              .minimum_height = 56.0F,
          },
      .outlined =
          {
              .background = Color::Transparent(),
              .border = theme.colors.outline,
              .hovered_border = theme.colors.on_surface,
              .focused_border = theme.colors.primary,
              .disabled_border = disabled_outline,
              .minimum_height = 56.0F,
          },
      .text_style = TextStyle{Font::System(theme.typography.body_large), theme.colors.on_surface},
      .label_style = TextStyle{Font::System(theme.typography.body_large), theme.colors.on_surface_variant},
      .floating_label_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.on_surface_variant},
      .placeholder_style = TextStyle{Font::System(theme.typography.body_large), theme.colors.on_surface_variant},
      .disabled_text = disabled_content,
      .focused_label = theme.colors.primary,
      .disabled_label = disabled_content,
      .error_label = theme.colors.error,
      .disabled_placeholder = disabled_content,
      .disabled_supporting_text = disabled_content,
      .leading_icon = theme.colors.on_surface_variant,
      .focused_leading_icon = theme.colors.on_surface_variant,
      .disabled_leading_icon = disabled_content,
      .error_leading_icon = theme.colors.on_surface_variant,
      .trailing_icon = theme.colors.on_surface_variant,
      .focused_trailing_icon = theme.colors.on_surface_variant,
      .disabled_trailing_icon = disabled_content,
      .error_trailing_icon = theme.colors.error,
      .selection =
          Color{
              theme.colors.primary.red,
              theme.colors.primary.green,
              theme.colors.primary.blue,
              0.4F,
          },
      .caret = theme.colors.primary,
      .error_caret = theme.colors.error,
      .composition = theme.colors.primary,
      .caret_width = 2.0F,
      .border_width = 1.0F,
      .focused_border_width = 2.0F,
      .corner_radius = theme.shapes.extra_small,
      .padding = EdgeInsets::All(theme.spacing.medium),
      .leading_icon_size = 24.0F,
      .trailing_icon_size = 24.0F,
      .icon_spacing = theme.spacing.small,
      .label_spacing = theme.spacing.extra_small,
      .label_cutout_padding = theme.spacing.extra_small,
      .label_animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
      .caret_blink_interval = theme.motion.reduced_motion ? 0.0 : 0.5,
      .validation_error = theme.colors.error,
      .validation_border_width = 1.0F,
      .focused_validation_border_width = 2.0F,
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .validation_spacing = theme.spacing.extra_small,
  };
}

ComboBoxStyle MaterialComboBoxStyle(const ThemeSpec& theme) {
  Color active_item_background = theme.colors.primary;
  active_item_background.alpha *= 0.08F;
  return {
      .popup_background = theme.colors.surface_container,
      .foreground = theme.colors.on_surface,
      .active_item_background = active_item_background,
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .popup_padding = {},
      .popup_shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.24F), theme.elevation.medium),
      .minimum_item_height = 40.0F,
      .maximum_popup_height = 320.0F,
      .popup_corner_radius = theme.shapes.small,
      .item_indication = MaterialIndication(theme.colors.on_surface, theme),
  };
}

CheckboxStyle MaterialCheckboxStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= 0.38F;
  return {
      .size = 18.0F,
      .minimum_interactive_size = 48.0F,
      .state_layer_size = 40.0F,
      .checked_background = theme.colors.primary,
      .checkmark = theme.colors.on_primary,
      .unchecked_border = theme.colors.on_surface_variant,
      .disabled_checked_background = disabled,
      .disabled_checkmark = theme.colors.surface,
      .disabled_unchecked_border = disabled,
      .border_width = 2.0F,
      .corner_radius = 2.0F,
  };
}

RadioButtonStyle MaterialRadioButtonStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= 0.38F;
  return {
      .size = 20.0F,
      .minimum_interactive_size = 48.0F,
      .state_layer_size = 40.0F,
      .selected_color = theme.colors.primary,
      .unselected_color = theme.colors.on_surface_variant,
      .disabled_selected_color = disabled,
      .disabled_unselected_color = disabled,
      .border_width = 2.0F,
      .dot_radius = 5.0F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
  };
}

SwitchStyle MaterialSwitchStyle(const ThemeSpec& theme) {
  Color disabled_track = theme.colors.on_surface;
  disabled_track.alpha *= 0.12F;
  Color disabled_thumb = theme.colors.on_surface;
  disabled_thumb.alpha *= 0.38F;
  return {
      .width = 52.0F,
      .height = 32.0F,
      .minimum_interactive_height = 48.0F,
      .state_layer_size = 40.0F,
      .unchecked_track = theme.colors.surface_container_highest,
      .checked_track = theme.colors.primary,
      .unchecked_track_border = theme.colors.outline,
      .checked_track_border = Color::Transparent(),
      .unchecked_thumb = theme.colors.outline,
      .checked_thumb = theme.colors.on_primary,
      .disabled_unchecked_track = disabled_track,
      .disabled_checked_track = disabled_track,
      .disabled_unchecked_track_border = disabled_track,
      .disabled_checked_track_border = Color::Transparent(),
      .disabled_unchecked_thumb = disabled_thumb,
      .disabled_checked_thumb = theme.colors.surface,
      .unchecked_thumb_radius = 8.0F,
      .checked_thumb_radius = 12.0F,
      .track_border_width = 2.0F,
      .corner_radius = 16.0F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
  };
}

ProgressCircleStyle MaterialProgressCircleStyle(const ThemeSpec& theme) {
  return {
      .size = 40.0F,
      .stroke_width = 4.0F,
      .track_color = theme.colors.secondary_container,
      .indeterminate_track_color = Color::Transparent(),
      .indicator_color = theme.colors.primary,
      .track_gap = 4.0F,
      .indeterminate_motion = ProgressCircleIndeterminateMotion::PulsingArc,
      .minimum_indeterminate_arc_fraction = 0.1F,
      .maximum_indeterminate_arc_fraction = 0.87F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : 6.0,
  };
}

RefreshBoxStyle MaterialRefreshBoxStyle(const ThemeSpec& theme) {
  ProgressCircleStyle indicator = MaterialProgressCircleStyle(theme);
  indicator.size = 24.0F;
  indicator.stroke_width = 3.0F;
  return {
      .indicator = std::move(indicator),
      .container_color = theme.colors.surface_container_high,
      .container_size = 40.0F,
      .pull_resistance = 0.45F,
      .maximum_pull_distance = 96.0F,
      .trigger_distance = 72.0F,
      .refresh_distance = 56.0F,
      .settle_motion = TweenSpec{
          .duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
          .easing = Easing::EaseOut,
      },
  };
}

ProgressBarStyle MaterialProgressBarStyle(const ThemeSpec& theme) {
  return {
      .width = 240.0F,
      .height = 4.0F,
      .track_color = theme.colors.secondary_container,
      .indicator_color = theme.colors.primary,
      .corner_radius = 2.0F,
      .track_gap = 4.0F,
      .stop_indicator_size = 4.0F,
      .indeterminate_motion = ProgressBarIndeterminateMotion::Segmented,
      .indeterminate_fraction = 0.35F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : 1.75,
  };
}

SliderStyle MaterialSliderStyle(const ThemeSpec& theme) {
  Color disabled_active = theme.colors.on_surface;
  disabled_active.alpha *= 0.38F;
  Color disabled_inactive = theme.colors.on_surface;
  disabled_inactive.alpha *= 0.12F;
  return {
      .width = 160.0F,
      .height = 48.0F,
      .track_height = 16.0F,
      .inactive_track = theme.colors.secondary_container,
      .active_track = theme.colors.primary,
      .thumb = theme.colors.primary,
      .stop_indicator = theme.colors.primary,
      .active_tick = theme.colors.secondary_container,
      .inactive_tick = theme.colors.primary,
      .disabled_inactive_track = disabled_inactive,
      .disabled_active_track = disabled_active,
      .disabled_thumb = disabled_active,
      .disabled_stop_indicator = disabled_active,
      .disabled_active_tick = disabled_inactive,
      .disabled_inactive_tick = disabled_active,
      .thumb_width = 4.0F,
      .thumb_height = 44.0F,
      .hovered_thumb_width = 4.0F,
      .hovered_thumb_height = 44.0F,
      .pressed_thumb_width = 2.0F,
      .pressed_thumb_height = 44.0F,
      .thumb_track_gap = 6.0F,
      .track_inside_corner_radius = 2.0F,
      .stop_indicator_size = 4.0F,
      .tick_size = 4.0F,
      .focus_ring = FocusRing{theme.colors.primary, 0.0F, 0.0F},
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
  };
}

ScrollBarStyle MaterialScrollBarStyle(const ThemeSpec& theme) {
  Color thumb = theme.colors.on_surface;
  thumb.alpha *= 0.38F;
  return {
      .thickness = 4.0F,
      .minimum_thumb_extent = 24.0F,
      .margin = 2.0F,
      .corner_radius = 2.0F,
      .fade_in_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.fast),
      .fade_out_delay = 0.4F,
      .fade_out_duration = theme.motion.reduced_motion ? 0.0F : 0.25F,
      .track_color = Color::Transparent(),
      .thumb_color = thumb,
  };
}

ToastStyle MaterialToastStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.inverse_surface,
      .text_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.inverse_on_surface},
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, 14.0F),
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.18F), theme.elevation.medium),
      .corner_radius = theme.shapes.extra_small,
      .minimum_height = 48.0F,
      .maximum_width = 600.0F,
      .viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F},
      .placement = VerticalPlacement::Bottom,
      .motion = PresentationMotion{
          .slide_distance = 12.0F,
          .enter = TweenSpec{.duration = theme.motion.normal},
          .exit = TweenSpec{.duration = theme.motion.fast},
      },
  };
}

SnackBarStyle MaterialSnackBarStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.inverse_surface,
      .message_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.inverse_on_surface},
      .action_text_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.primary},
      .action_background = Color::Transparent(),
      .action_padding = EdgeInsets::Symmetric(12.0F, 8.0F),
      .action_minimum_height = 40.0F,
      .action_corner_radius = theme.shapes.full,
      .action_indication = MaterialIndication(theme.colors.primary, theme),
      .padding = EdgeInsets::Symmetric(16.0F, 8.0F),
      .content_spacing = 8.0F,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.18F), theme.elevation.medium),
      .corner_radius = theme.shapes.extra_small,
      .minimum_height = 48.0F,
      .maximum_width = 600.0F,
      .viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F},
      .motion = PresentationMotion{
          .slide_distance = 12.0F,
          .enter = TweenSpec{.duration = theme.motion.normal},
          .exit = TweenSpec{.duration = theme.motion.fast},
      },
  };
}

TooltipStyle MaterialTooltipStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.inverse_surface,
      .text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.inverse_on_surface},
      .padding = EdgeInsets::Symmetric(8.0F, 4.0F),
      .shadow = {},
      .corner_radius = theme.shapes.extra_small,
      .minimum_height = 24.0F,
      .maximum_width = 200.0F,
      .placement = AnchorPlacement{AnchorSide::Above, AnchorAlignment::Center},
      .gap = 4.0F,
      .viewport_margin = 8.0F,
      .hover_delay = 0.5,
      .exit_delay = 0.1,
      .long_press_delay = 0.5,
      .touch_show_duration = 1.5,
  };
}

DialogStyle MaterialDialogStyle(const ThemeSpec& theme) {
  return {
      .scrim = theme.colors.scrim,
      .background = theme.colors.surface_container_high,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.16F), theme.elevation.medium),
      .title_style = TextStyle{Font::System(theme.typography.headline_small), theme.colors.on_surface},
      .message_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface_variant},
      .positive_action_style =
          TextStyle{Font::System(theme.typography.label_large).WithWeight(FontWeight::Medium), theme.colors.primary},
      .negative_action_style =
          TextStyle{Font::System(theme.typography.label_large).WithWeight(FontWeight::Medium), theme.colors.primary},
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
      .action_corner_radius = 20.0F,
      .minimum_action_height = 40.0F,
      .corner_radius = theme.shapes.extra_large,
      .minimum_width = 280.0F,
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
      .background = theme.colors.surface_container_low,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.16F), theme.elevation.low),
      .corner_radii = CornerRadii::Top(theme.shapes.extra_large),
      .drag_handle = theme.colors.on_surface_variant,
      .drag_handle_size = {32.0F, 4.0F},
      .drag_handle_padding =
          EdgeInsets{theme.spacing.medium, 0.0F, theme.spacing.small + theme.spacing.extra_small, 0.0F},
      .maximum_width = 640.0F,
      .enter = TweenSpec{.duration = theme.motion.slow},
      .exit = TweenSpec{.duration = theme.motion.normal},
  };
}

MenuStyle MaterialMenuStyle(const ThemeSpec& theme) {
  Color separator = theme.colors.on_surface;
  separator.alpha *= 0.12F;
  return {
      .background = theme.colors.surface_container,
      .foreground = theme.colors.on_surface,
      .icon_tint = theme.colors.on_surface_variant,
      .item_indication = MaterialIndication(theme.colors.on_surface, theme),
      .separator_color = separator,
      .separator_mode = MenuSeparatorMode::None,
      .separator_thickness = 1.0F,
      .separator_padding = {},
      .content_padding = {},
      .item_padding = EdgeInsets::Symmetric(theme.spacing.small + theme.spacing.extra_small, theme.spacing.small),
      .item_content_spacing = theme.spacing.small,
      .icon_size = 24.0F,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.18F), theme.elevation.medium),
      .corner_radius = theme.shapes.extra_small,
      .minimum_width = 112.0F,
      .minimum_item_height = 48.0F,
      .motion = PresentationMotion{
          .initial_scale = 0.96F,
          .enter = TweenSpec{.duration = theme.motion.fast},
          .exit = TweenSpec{.duration = theme.motion.fast},
      },
  };
}

NavigationStyle MaterialNavigationStyle(const ThemeSpec& theme) {
  return {
      .motion = NavigationMotion{
          .entering_offset_fraction = {1.0F, 0.0F},
          .covered_offset_fraction = {-0.2F, 0.0F},
          .push = TweenSpec{.duration = theme.motion.slow},
          .pop = TweenSpec{.duration = theme.motion.normal},
      },
  };
}

TopAppBarStyle MaterialTopAppBarStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.surface,
      .title_style = TextStyle{Font::System(theme.typography.title_large), theme.colors.on_surface},
      .height = 64.0F,
      .horizontal_padding = 4.0F,
      .title_inset = 16.0F,
      .title_spacing = 4.0F,
      .action_spacing = 0.0F,
  };
}

NavigationBarStyle MaterialNavigationBarStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = theme.colors.surface_container,
      .label_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.on_surface_variant},
      .selected_content = theme.colors.on_secondary_container,
      .disabled_content = disabled,
      .indicator = theme.colors.secondary_container,
      .indicator_size = {64.0F, 32.0F},
      .item_padding = EdgeInsets::Symmetric(0.0F, theme.spacing.small),
      .icon_size = 24.0F,
      .icon_spacing = theme.spacing.extra_small,
      .minimum_item_width = 64.0F,
      .height = 80.0F,
      .indicator_corner_radius = theme.shapes.full,
      .show_unselected_labels = true,
      .indication = MaterialIndication(theme.colors.on_secondary_container, theme),
      .selection_animation = TweenSpec{.duration = theme.motion.normal},
  };
}

NavigationPaneStyle MaterialNavigationPaneStyle(const ThemeSpec& theme) {
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = theme.colors.surface_container_low,
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_surface_variant},
      .selected_content = theme.colors.on_secondary_container,
      .disabled_content = disabled,
      .indicator = theme.colors.secondary_container,
      .item_margin = EdgeInsets::Symmetric(theme.spacing.small + theme.spacing.extra_small, 0.0F),
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, 0.0F),
      .compact_width = 80.0F,
      .expanded_min_width = 256.0F,
      .item_height = 56.0F,
      .icon_size = 24.0F,
      .icon_spacing = theme.spacing.medium,
      .compact_indicator_size = {56.0F, 32.0F},
      .indicator_corner_radius = theme.shapes.full,
      .indication = MaterialIndication(theme.colors.on_secondary_container, theme),
      .selection_animation = TweenSpec{.duration = theme.motion.normal},
  };
}

DrawerStyle MaterialDrawerStyle(const ThemeSpec& theme) {
  return {
      .background = theme.colors.surface_container_low,
      .scrim = theme.colors.scrim,
      .shadow = MaterialShadow(Color::Rgb(0, 0, 0, 0.18F), theme.elevation.medium),
      .preferred_width = 360.0F,
      .minimum_width = 240.0F,
      .minimum_content_width = 360.0F,
      .modal_content_reveal = 56.0F,
      .edge_drag_width = 24.0F,
      .corner_radius = theme.shapes.extra_large,
      .motion = theme.motion.reduced_motion ? std::optional<DrawerMotion>{std::nullopt}
                                            : std::optional<DrawerMotion>{DrawerMotion{
                                                  .open = TweenSpec{.duration = theme.motion.slow},
                                                  .close = TweenSpec{.duration = theme.motion.normal},
                                              }},
  };
}

ThemeDefinition MaterialDefinition(ThemeSpec theme) {
  ThemeDefinition definition{theme};
  definition.Set(
      SystemBarsAppearance{
          .status_bar_background = theme.colors.surface,
          .navigation_bar_background = theme.colors.surface_container,
      }
  );
  definition.Set(MaterialButtonStyle(theme));
  definition.Set(MaterialIconButtonStyle(theme));
  definition.Set(MaterialChipStyle(theme));
  definition.Set(MaterialSegmentedButtonStyle(theme));
  definition.Set(MaterialTabsStyle(theme));
  definition.Set(MaterialSelectStyle(theme));
  definition.Set(MaterialDatePickerStyle(theme));
  definition.Set(MaterialTimePickerStyle(theme));
  definition.Set(MaterialDividerStyle(theme));
  definition.Set(MaterialTextFieldStyle(theme));
  definition.Set(MaterialComboBoxStyle(theme));
  definition.Set(MaterialCheckboxStyle(theme));
  definition.Set(MaterialRadioButtonStyle(theme));
  definition.Set(MaterialSwitchStyle(theme));
  definition.Set(MaterialProgressCircleStyle(theme));
  definition.Set(MaterialRefreshBoxStyle(theme));
  definition.Set(MaterialProgressBarStyle(theme));
  definition.Set(MaterialSliderStyle(theme));
  definition.Set(MaterialScrollBarStyle(theme));
  definition.Set(MaterialToastStyle(theme));
  definition.Set(MaterialSnackBarStyle(theme));
  definition.Set(MaterialTooltipStyle(theme));
  definition.Set(MaterialDialogStyle(theme));
  definition.Set(MaterialBottomSheetStyle(theme));
  definition.Set(MaterialMenuStyle(theme));
  definition.Set(MaterialNavigationStyle(theme));
  definition.Set(MaterialTopAppBarStyle(theme));
  definition.Set(MaterialNavigationBarStyle(theme));
  definition.Set(MaterialNavigationPaneStyle(theme));
  definition.Set(MaterialDrawerStyle(theme));
  return definition;
}

} // namespace

ThemeSpec ThemeSpec::Default() {
  return FlatLightThemeSpec();
}

namespace {

View MakeThemeView(ThemeDefinition definition, View content) {
  Environment environment;
  detail::ApplyThemeDefinition(environment, definition);
  return ProvideEnvironment(std::move(environment), std::move(content));
}

} // namespace

Theme::Theme(ThemeDefinition definition, View content)
    : detail::TypedView<Theme>(MakeThemeView(std::move(definition), std::move(content))) {}

FlatTheme::FlatTheme(View content)
    : detail::TypedView<FlatTheme>(MakeThemeView(FlatThemeDefinition(), std::move(content))) {}

FlatTheme::FlatTheme(ThemeSpec theme, View content)
    : detail::TypedView<FlatTheme>(MakeThemeView(FlatThemeDefinition(std::move(theme)), std::move(content))) {}

FlatDarkTheme::FlatDarkTheme(View content)
    : detail::TypedView<FlatDarkTheme>(MakeThemeView(FlatDarkThemeDefinition(), std::move(content))) {}

MaterialTheme::MaterialTheme(View content)
    : detail::TypedView<MaterialTheme>(MakeThemeView(MaterialThemeDefinition(), std::move(content))) {}

MaterialTheme::MaterialTheme(ThemeSpec theme, View content)
    : detail::TypedView<MaterialTheme>(MakeThemeView(MaterialThemeDefinition(std::move(theme)), std::move(content))) {}

MaterialDarkTheme::MaterialDarkTheme(View content)
    : detail::TypedView<MaterialDarkTheme>(MakeThemeView(MaterialDarkThemeDefinition(), std::move(content))) {}

namespace detail {

void ApplyThemeDefinition(Environment& environment, const ThemeDefinition& definition) {
  MergeEnvironment(environment, definition.overrides_);
}

const ThemeSpec& ResolveThemeSpec(std::shared_ptr<const Environment> environment) {
  if (const std::any* value = FindEnvironmentValue(std::move(environment), typeid(ThemeSpec))) {
    if (const auto* theme = std::any_cast<ThemeSpec>(value)) {
      return *theme;
    }
    throw std::logic_error("HuxerUI theme environment value has an invalid type");
  }
  static const ThemeSpec default_theme = ThemeSpec::Default();
  return default_theme;
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
  float font_size = theme.typography.body_medium;
  if (role == TextRole::Label) {
    font_size = theme.typography.label_large;
  } else if (role == TextRole::Title) {
    font_size = theme.typography.title_large;
  }
  return {
      Font::System(font_size),
      theme.colors.on_surface,
  };
}

ButtonStyle DefaultButtonStyle(const ThemeSpec& theme) {
  Color disabled_background = theme.colors.primary;
  disabled_background.alpha *= theme.interactions.disabled_opacity;
  Color disabled_label = theme.colors.on_primary;
  disabled_label.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = theme.colors.primary,
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_primary},
      .disabled_background = disabled_background,
      .disabled_label = disabled_label,
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .minimum_width = 0.0F,
      .minimum_height = 0.0F,
      .corner_radius = theme.shapes.extra_small,
      .indication = std::nullopt,
  };
}

IconButtonStyle DefaultIconButtonStyle(const ThemeSpec& theme) {
  Color disabled_foreground = theme.colors.on_surface;
  disabled_foreground.alpha *= theme.interactions.disabled_opacity;
  return {
      .foreground = theme.colors.on_surface,
      .disabled_foreground = disabled_foreground,
      .icon_size = 20.0F,
      .minimum_interactive_size = 40.0F,
      .state_layer_size = 32.0F,
      .corner_radius = theme.shapes.extra_small,
      .indication = std::nullopt,
  };
}

ChipStyle DefaultChipStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.24F;
  Color disabled_background = theme.colors.surface;
  disabled_background.alpha *= theme.interactions.disabled_opacity;
  Color disabled_selected_background = theme.colors.primary;
  disabled_selected_background.alpha *= theme.interactions.disabled_opacity;
  Color disabled_label = theme.colors.on_surface;
  disabled_label.alpha *= theme.interactions.disabled_opacity;
  Color disabled_selected_label = theme.colors.on_primary;
  disabled_selected_label.alpha *= theme.interactions.disabled_opacity;
  Color disabled_border = theme.colors.on_surface;
  disabled_border.alpha *= theme.interactions.disabled_opacity * 0.5F;
  return {
      .background = theme.colors.surface,
      .selected_background = theme.colors.primary,
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_surface},
      .selected_label = theme.colors.on_primary,
      .disabled_background = disabled_background,
      .disabled_selected_background = disabled_selected_background,
      .disabled_label = disabled_label,
      .disabled_selected_label = disabled_selected_label,
      .border = border,
      .selected_border = Color::Transparent(),
      .disabled_border = disabled_border,
      .disabled_selected_border = Color::Transparent(),
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.extra_small),
      .icon_size = 16.0F,
      .icon_spacing = theme.spacing.small,
      .minimum_height = 28.0F,
      .corner_radius = 14.0F,
      .border_width = 1.0F,
      .indication = std::nullopt,
      .selected_indication = std::nullopt,
  };
}

SegmentedButtonStyle DefaultSegmentedButtonStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.24F;
  return {
      .background = theme.colors.surface,
      .selected_background = theme.colors.primary,
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_surface},
      .selected_label = theme.colors.on_primary,
      .border = border,
      .selected_border = theme.colors.primary,
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .icon_size = 16.0F,
      .icon_spacing = theme.spacing.small,
      .minimum_segment_width = 48.0F,
      .minimum_height = 32.0F,
      .corner_radius = theme.shapes.small,
      .border_width = 1.0F,
      .indication = std::nullopt,
      .selected_indication = std::nullopt,
  };
}

TabsStyle DefaultTabsStyle(const ThemeSpec& theme) {
  Color disabled_label = theme.colors.on_surface;
  disabled_label.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = Color::Transparent(),
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_surface},
      .selected_label = theme.colors.primary,
      .disabled_label = disabled_label,
      .indicator = theme.colors.primary,
      .indicator_sizing = TabIndicatorSizing::Item,
      .indicator_min_width = 0.0F,
      .indicator_height = 2.0F,
      .indicator_corner_radius = 1.0F,
      .divider_color = Color::Transparent(),
      .divider_height = 0.0F,
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .icon_size = 18.0F,
      .icon_spacing = theme.spacing.small,
      .minimum_item_width = 48.0F,
      .minimum_height = 36.0F,
      .expand_items = false,
      .indication = std::nullopt,
      .indicator_animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
  };
}

SelectStyle DefaultSelectStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.4F;
  Color active_item_background = theme.colors.primary;
  active_item_background.alpha *= 0.10F;
  Color selected_item_background = theme.colors.primary;
  selected_item_background.alpha *= 0.14F;
  return {
      .background = theme.colors.surface,
      .foreground = theme.colors.on_surface,
      .border = border,
      .indicator = theme.colors.on_surface_variant,
      .popup_background = theme.colors.surface,
      .active_item_background = active_item_background,
      .selected_item_background = selected_item_background,
      .validation_error = theme.colors.error,
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .trigger_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .popup_padding = {},
      .popup_shadow = Shadow{Color::Rgb(0, 0, 0, 0.2F), {}, theme.elevation.medium, 0.0F},
      .content_spacing = theme.spacing.small,
      .validation_spacing = theme.spacing.extra_small,
      .minimum_width = 120.0F,
      .minimum_height = 36.0F,
      .minimum_item_height = 36.0F,
      .maximum_popup_height = 320.0F,
      .indicator_size = 16.0F,
      .corner_radius = theme.shapes.extra_small,
      .popup_corner_radius = theme.shapes.small,
      .border_width = 1.0F,
      .indication = std::nullopt,
      .item_indication = std::nullopt,
  };
}

DatePickerStyle DefaultDatePickerStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.18F;
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  Color hover = theme.colors.primary;
  hover.alpha *= 0.08F;
  return {
      .background = theme.colors.surface,
      .foreground = theme.colors.on_surface,
      .secondary_foreground = theme.colors.on_surface_variant,
      .disabled_foreground = disabled,
      .selected_background = theme.colors.primary,
      .selected_foreground = theme.colors.on_primary,
      .hover_background = hover,
      .border = border,
      .validation_error = theme.colors.error,
      .title_style = TextStyle{
          Font::System(theme.typography.body_large).WithWeight(FontWeight::Medium),
          theme.colors.on_surface,
      },
      .weekday_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.on_surface_variant},
      .day_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .label_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .padding = EdgeInsets::All(theme.spacing.small),
      .cell_size = 32.0F,
      .column_spacing = 2.0F,
      .row_spacing = 2.0F,
      .header_height = 44.0F,
      .corner_radius = theme.shapes.small,
      .selection_corner_radius = theme.shapes.extra_small,
      .border_width = 1.0F,
      .label_spacing = theme.spacing.small,
      .validation_spacing = theme.spacing.extra_small,
  };
}

TimePickerStyle DefaultTimePickerStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.18F;
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .background = theme.colors.surface,
      .dial_background = theme.colors.surface_container_highest,
      .foreground = theme.colors.on_surface,
      .disabled_foreground = disabled,
      .selected_background = theme.colors.primary,
      .selected_foreground = theme.colors.on_primary,
      .field_background = theme.colors.surface_container_highest,
      .selected_field_background = theme.colors.primary_container,
      .selected_field_foreground = theme.colors.on_primary_container,
      .selected_period_background = theme.colors.primary_container,
      .selected_period_foreground = theme.colors.on_primary_container,
      .period_border = border,
      .hand = theme.colors.primary,
      .border = border,
      .validation_error = theme.colors.error,
      .header_style = TextStyle{
          Font::System(32.0F),
          theme.colors.on_surface,
      },
      .period_style = TextStyle{
          Font::System(theme.typography.body_small).WithWeight(FontWeight::Medium),
          theme.colors.on_surface_variant,
      },
      .dial_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .label_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .padding = EdgeInsets::All(theme.spacing.small + theme.spacing.extra_small),
      .dial_size = 208.0F,
      .header_height = 52.0F,
      .field_width = 64.0F,
      .separator_width = 16.0F,
      .field_corner_radius = theme.shapes.extra_small,
      .period_width = 44.0F,
      .period_spacing = theme.spacing.small,
      .period_corner_radius = theme.shapes.extra_small,
      .period_border_width = 1.0F,
      .content_spacing = theme.spacing.medium,
      .selection_radius = 16.0F,
      .hand_width = 2.0F,
      .corner_radius = theme.shapes.small,
      .border_width = 1.0F,
      .label_spacing = theme.spacing.small,
      .validation_spacing = theme.spacing.extra_small,
  };
}

DividerStyle DefaultDividerStyle(const ThemeSpec& theme) {
  Color color = theme.colors.on_surface;
  color.alpha *= 0.12F;
  return {
      .color = color,
      .thickness = 1.0F,
  };
}

TextFieldStyle DefaultTextFieldStyle(const ThemeSpec& theme) {
  Color placeholder = theme.colors.on_surface;
  placeholder.alpha *= 0.55F;
  Color border = theme.colors.on_surface;
  border.alpha *= 0.4F;
  Color hovered_border = theme.colors.on_surface;
  hovered_border.alpha *= 0.7F;
  Color disabled_content = theme.colors.on_surface;
  disabled_content.alpha *= theme.interactions.disabled_opacity;
  Color disabled_border = border;
  disabled_border.alpha *= theme.interactions.disabled_opacity;
  return {
      .variant = TextFieldVariant::Standard,
      .show_label = true,
      .standard =
          {
              .background = Color::Transparent(),
              .border = border,
              .hovered_border = hovered_border,
              .focused_border = theme.colors.primary,
              .disabled_border = disabled_border,
              .minimum_height = 40.0F,
          },
      .filled =
          {
              .background = theme.colors.surface_container_highest,
              .border = border,
              .hovered_border = hovered_border,
              .focused_border = theme.colors.primary,
              .disabled_border = disabled_border,
              .minimum_height = 44.0F,
          },
      .outlined =
          {
              .background = Color::Transparent(),
              .border = border,
              .hovered_border = hovered_border,
              .focused_border = theme.colors.primary,
              .disabled_border = disabled_border,
              .minimum_height = 36.0F,
          },
      .text_style = TextStyle{Font::System(theme.typography.body_medium), theme.colors.on_surface},
      .label_style = TextStyle{Font::System(theme.typography.body_medium), placeholder},
      .floating_label_style = TextStyle{Font::System(theme.typography.body_small), placeholder},
      .placeholder_style = TextStyle{Font::System(theme.typography.body_medium), placeholder},
      .disabled_text = disabled_content,
      .focused_label = theme.colors.primary,
      .disabled_label = disabled_content,
      .error_label = theme.colors.error,
      .disabled_placeholder = disabled_content,
      .disabled_supporting_text = disabled_content,
      .leading_icon = placeholder,
      .focused_leading_icon = theme.colors.primary,
      .disabled_leading_icon = disabled_content,
      .error_leading_icon = theme.colors.error,
      .trailing_icon = placeholder,
      .focused_trailing_icon = theme.colors.primary,
      .disabled_trailing_icon = disabled_content,
      .error_trailing_icon = theme.colors.error,
      .selection =
          Color{
              theme.colors.primary.red,
              theme.colors.primary.green,
              theme.colors.primary.blue,
              0.22F,
          },
      .caret = theme.colors.primary,
      .error_caret = theme.colors.error,
      .composition = theme.colors.primary,
      .caret_width = 2.0F,
      .border_width = 1.0F,
      .focused_border_width = 2.0F,
      .corner_radius = theme.shapes.extra_small + 2.0F,
      .padding = EdgeInsets::Symmetric(10.0F, theme.spacing.small),
      .leading_icon_size = 18.0F,
      .trailing_icon_size = 18.0F,
      .icon_spacing = theme.spacing.small,
      .label_spacing = theme.spacing.extra_small,
      .label_cutout_padding = theme.spacing.extra_small,
      .label_animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
      .caret_blink_interval = theme.motion.reduced_motion ? 0.0 : 0.5,
      .validation_error = theme.colors.error,
      .validation_border_width = 1.0F,
      .focused_validation_border_width = 2.0F,
      .validation_text_style = TextStyle{Font::System(theme.typography.body_small), theme.colors.error},
      .validation_spacing = theme.spacing.extra_small,
  };
}

ComboBoxStyle DefaultComboBoxStyle(const ThemeSpec& theme) {
  Color active_item_background = theme.colors.primary;
  active_item_background.alpha *= 0.10F;
  return {
      .popup_background = theme.colors.surface,
      .foreground = theme.colors.on_surface,
      .active_item_background = active_item_background,
      .item_padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .popup_padding = {},
      .popup_shadow = Shadow{Color::Rgb(0, 0, 0, 0.2F), {}, theme.elevation.medium, 0.0F},
      .minimum_item_height = 36.0F,
      .maximum_popup_height = 320.0F,
      .popup_corner_radius = theme.shapes.small,
      .item_indication = std::nullopt,
  };
}

CheckboxStyle DefaultCheckboxStyle(const ThemeSpec& theme) {
  Color border = theme.colors.on_surface;
  border.alpha *= 0.55F;
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .size = 20.0F,
      .minimum_interactive_size = 20.0F,
      .state_layer_size = 20.0F,
      .checked_background = theme.colors.primary,
      .checkmark = theme.colors.on_primary,
      .unchecked_border = border,
      .disabled_checked_background = disabled,
      .disabled_checkmark = theme.colors.surface,
      .disabled_unchecked_border = disabled,
      .border_width = 2.0F,
      .corner_radius = theme.shapes.extra_small,
  };
}

RadioButtonStyle DefaultRadioButtonStyle(const ThemeSpec& theme) {
  Color unselected = theme.colors.on_surface;
  unselected.alpha *= 0.55F;
  Color disabled = theme.colors.on_surface;
  disabled.alpha *= theme.interactions.disabled_opacity;
  return {
      .size = 20.0F,
      .minimum_interactive_size = 20.0F,
      .state_layer_size = 20.0F,
      .selected_color = theme.colors.primary,
      .unselected_color = unselected,
      .disabled_selected_color = disabled,
      .disabled_unselected_color = disabled,
      .border_width = 2.0F,
      .dot_radius = 5.0F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
  };
}

SwitchStyle DefaultSwitchStyle(const ThemeSpec& theme) {
  Color track = theme.colors.on_surface;
  track.alpha *= 0.28F;
  Color disabled_track = theme.colors.on_surface;
  disabled_track.alpha *= 0.12F;
  Color disabled_thumb = theme.colors.on_surface;
  disabled_thumb.alpha *= theme.interactions.disabled_opacity;
  return {
      .width = 40.0F,
      .height = 24.0F,
      .minimum_interactive_height = 24.0F,
      .state_layer_size = 24.0F,
      .unchecked_track = track,
      .checked_track = theme.colors.primary,
      .unchecked_track_border = Color::Transparent(),
      .checked_track_border = Color::Transparent(),
      .unchecked_thumb = theme.colors.surface,
      .checked_thumb = theme.colors.surface,
      .disabled_unchecked_track = disabled_track,
      .disabled_checked_track = disabled_track,
      .disabled_unchecked_track_border = Color::Transparent(),
      .disabled_checked_track_border = Color::Transparent(),
      .disabled_unchecked_thumb = disabled_thumb,
      .disabled_checked_thumb = disabled_thumb,
      .unchecked_thumb_radius = 8.0F,
      .checked_thumb_radius = 8.0F,
      .track_border_width = 0.0F,
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
      .indeterminate_track_color = track,
      .indicator_color = theme.colors.primary,
      .track_gap = 0.0F,
      .indeterminate_motion = ProgressCircleIndeterminateMotion::Sweep,
      .minimum_indeterminate_arc_fraction = 0.28F,
      .maximum_indeterminate_arc_fraction = 0.28F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow * 3.0,
  };
}

RefreshBoxStyle DefaultRefreshBoxStyle(const ThemeSpec& theme) {
  ProgressCircleStyle indicator = DefaultProgressCircleStyle(theme);
  indicator.size = 20.0F;
  return {
      .indicator = std::move(indicator),
      .container_color = theme.colors.surface,
      .container_size = 36.0F,
      .pull_resistance = 0.45F,
      .maximum_pull_distance = 96.0F,
      .trigger_distance = 68.0F,
      .refresh_distance = 52.0F,
      .settle_motion = TweenSpec{
          .duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
          .easing = Easing::EaseOut,
      },
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
      .track_gap = 0.0F,
      .stop_indicator_size = 0.0F,
      .indeterminate_motion = ProgressBarIndeterminateMotion::Sweep,
      .indeterminate_fraction = 0.35F,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow * 4.0,
  };
}

SliderStyle DefaultSliderStyle(const ThemeSpec& theme) {
  Color inactive_track = theme.colors.on_surface;
  inactive_track.alpha *= 0.2F;
  Color disabled_active = theme.colors.on_surface;
  disabled_active.alpha *= 0.38F;
  Color disabled_inactive = theme.colors.on_surface;
  disabled_inactive.alpha *= 0.12F;
  return {
      .width = 160.0F,
      .height = 32.0F,
      .track_height = 4.0F,
      .inactive_track = inactive_track,
      .active_track = theme.colors.primary,
      .thumb = theme.colors.primary,
      .stop_indicator = theme.colors.primary,
      .active_tick = theme.colors.surface,
      .inactive_tick = theme.colors.primary,
      .disabled_inactive_track = disabled_inactive,
      .disabled_active_track = disabled_active,
      .disabled_thumb = disabled_active,
      .disabled_stop_indicator = disabled_active,
      .disabled_active_tick = disabled_inactive,
      .disabled_inactive_tick = disabled_active,
      .thumb_width = 16.0F,
      .thumb_height = 16.0F,
      .hovered_thumb_width = 17.0F,
      .hovered_thumb_height = 17.0F,
      .pressed_thumb_width = 18.0F,
      .pressed_thumb_height = 18.0F,
      .thumb_track_gap = 0.0F,
      .track_inside_corner_radius = 2.0F,
      .stop_indicator_size = 0.0F,
      .tick_size = 0.0F,
      .focus_ring = std::nullopt,
      .animation_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
  };
}

} // namespace detail

TextStyle TextStyle::Default() {
  return detail::DefaultTextStyle(ThemeSpec::Default());
}

ButtonStyle ButtonStyle::Default() {
  return detail::DefaultButtonStyle(ThemeSpec::Default());
}

IconButtonStyle IconButtonStyle::Default() {
  return detail::DefaultIconButtonStyle(ThemeSpec::Default());
}

ChipStyle ChipStyle::Default() {
  return detail::DefaultChipStyle(ThemeSpec::Default());
}

SegmentedButtonStyle SegmentedButtonStyle::Default() {
  return detail::DefaultSegmentedButtonStyle(ThemeSpec::Default());
}

TabsStyle TabsStyle::Default() {
  return detail::DefaultTabsStyle(ThemeSpec::Default());
}

SelectStyle SelectStyle::Default() {
  return detail::DefaultSelectStyle(ThemeSpec::Default());
}

DatePickerStyle DatePickerStyle::Default() {
  return detail::DefaultDatePickerStyle(ThemeSpec::Default());
}

TimePickerStyle TimePickerStyle::Default() {
  return detail::DefaultTimePickerStyle(ThemeSpec::Default());
}

DividerStyle DividerStyle::Default() {
  return detail::DefaultDividerStyle(ThemeSpec::Default());
}

TextFieldStyle TextFieldStyle::Default() {
  return detail::DefaultTextFieldStyle(ThemeSpec::Default());
}

ComboBoxStyle ComboBoxStyle::Default() {
  return detail::DefaultComboBoxStyle(ThemeSpec::Default());
}

CheckboxStyle CheckboxStyle::Default() {
  return detail::DefaultCheckboxStyle(ThemeSpec::Default());
}

RadioButtonStyle RadioButtonStyle::Default() {
  return detail::DefaultRadioButtonStyle(ThemeSpec::Default());
}

SwitchStyle SwitchStyle::Default() {
  return detail::DefaultSwitchStyle(ThemeSpec::Default());
}

ProgressCircleStyle ProgressCircleStyle::Default() {
  return detail::DefaultProgressCircleStyle(ThemeSpec::Default());
}

RefreshBoxStyle RefreshBoxStyle::Default() {
  return detail::DefaultRefreshBoxStyle(ThemeSpec::Default());
}

ProgressBarStyle ProgressBarStyle::Default() {
  return detail::DefaultProgressBarStyle(ThemeSpec::Default());
}

SliderStyle SliderStyle::Default() {
  return detail::DefaultSliderStyle(ThemeSpec::Default());
}

ToastStyle ToastStyle::Default() {
  return FlatToastStyle(FlatLightThemeSpec());
}

SnackBarStyle SnackBarStyle::Default() {
  return FlatSnackBarStyle(FlatLightThemeSpec());
}

TooltipStyle TooltipStyle::Default() {
  return FlatTooltipStyle(FlatLightThemeSpec());
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
  theme.shapes.large = 14.0F;
  theme.interactions = {
      .indication = FlatIndication(Color::Black(), theme),
      .focus_ring = FocusRing{theme.colors.primary, 2.0F, 2.0F},
      .disabled_opacity = 0.42F,
  };
  return theme;
}

ThemeSpec FlatDarkThemeSpec() {
  ThemeSpec theme;
  theme.shapes.large = 14.0F;
  theme.colors = {
      .primary = Color::Rgb(88, 166, 255),
      .on_primary = Color::Rgb(13, 17, 23),
      .primary_container = Color::Rgb(25, 53, 85),
      .on_primary_container = Color::Rgb(202, 225, 255),
      .secondary = Color::Rgb(139, 148, 158),
      .on_secondary = Color::Rgb(13, 17, 23),
      .secondary_container = Color::Rgb(48, 54, 61),
      .on_secondary_container = Color::Rgb(230, 237, 243),
      .tertiary_container = Color::Rgb(67, 47, 84),
      .on_tertiary_container = Color::Rgb(236, 216, 255),
      .background = Color::Rgb(13, 17, 23),
      .surface = Color::Rgb(22, 27, 34),
      .surface_container_low = Color::Rgb(22, 27, 34),
      .surface_container = Color::Rgb(27, 33, 42),
      .surface_container_high = Color::Rgb(33, 40, 50),
      .surface_container_highest = Color::Rgb(39, 47, 58),
      .on_surface = Color::Rgb(230, 237, 243),
      .on_surface_variant = Color::Rgb(177, 186, 196),
      .outline = Color::Rgb(139, 148, 158),
      .inverse_surface = Color::Rgb(230, 237, 243),
      .inverse_on_surface = Color::Rgb(22, 27, 34),
      .scrim = Color::Rgb(0, 0, 0, 0.62F),
      .error = Color::Rgb(248, 81, 73),
  };
  theme.interactions = {
      .indication = FlatIndication(Color::White(), theme, 0.12F, 0.18F),
      .focus_ring = FocusRing{theme.colors.primary, 2.0F, 2.0F},
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
      .primary_container = Color::Rgb(234, 221, 255),
      .on_primary_container = Color::Rgb(33, 0, 93),
      .secondary = Color::Rgb(98, 91, 113),
      .on_secondary = Color::White(),
      .secondary_container = Color::Rgb(232, 222, 248),
      .on_secondary_container = Color::Rgb(29, 25, 43),
      .tertiary_container = Color::Rgb(255, 216, 228),
      .on_tertiary_container = Color::Rgb(49, 17, 29),
      .background = Color::Rgb(255, 251, 254),
      .surface = Color::Rgb(255, 251, 254),
      .surface_container_low = Color::Rgb(247, 242, 250),
      .surface_container = Color::Rgb(243, 237, 247),
      .surface_container_high = Color::Rgb(236, 230, 240),
      .surface_container_highest = Color::Rgb(230, 224, 233),
      .on_surface = Color::Rgb(28, 27, 31),
      .on_surface_variant = Color::Rgb(73, 69, 79),
      .outline = Color::Rgb(121, 116, 126),
      .inverse_surface = Color::Rgb(50, 47, 53),
      .inverse_on_surface = Color::Rgb(245, 239, 247),
      .scrim = Color::Rgb(0, 0, 0, 0.32F),
      .error = Color::Rgb(179, 38, 30),
  };
  theme.typography = {
      .body_large = 16.0F,
      .body_medium = 14.0F,
      .body_small = 12.0F,
      .label_large = 14.0F,
      .title_large = 22.0F,
      .headline_small = 24.0F,
  };
  theme.shapes = {
      .extra_small = 4.0F,
      .small = 8.0F,
      .medium = 12.0F,
      .large = 16.0F,
      .extra_large = 28.0F,
      .full = 10000.0F,
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
      .indication = MaterialIndication(theme.colors.primary, theme),
      .focus_ring = FocusRing{theme.colors.secondary, 3.0F, 2.0F},
      .disabled_opacity = 0.38F,
  };
  return theme;
}

ThemeSpec MaterialDarkThemeSpec() {
  ThemeSpec theme;
  theme.colors = {
      .primary = Color::Rgb(208, 188, 255),
      .on_primary = Color::Rgb(56, 30, 114),
      .primary_container = Color::Rgb(79, 55, 139),
      .on_primary_container = Color::Rgb(234, 221, 255),
      .secondary = Color::Rgb(204, 194, 220),
      .on_secondary = Color::Rgb(51, 45, 65),
      .secondary_container = Color::Rgb(74, 68, 88),
      .on_secondary_container = Color::Rgb(232, 222, 248),
      .tertiary_container = Color::Rgb(99, 59, 72),
      .on_tertiary_container = Color::Rgb(255, 216, 228),
      .background = Color::Rgb(28, 27, 31),
      .surface = Color::Rgb(28, 27, 31),
      .surface_container_low = Color::Rgb(29, 27, 32),
      .surface_container = Color::Rgb(33, 31, 38),
      .surface_container_high = Color::Rgb(43, 41, 48),
      .surface_container_highest = Color::Rgb(54, 52, 59),
      .on_surface = Color::Rgb(230, 225, 229),
      .on_surface_variant = Color::Rgb(202, 196, 208),
      .outline = Color::Rgb(147, 143, 153),
      .inverse_surface = Color::Rgb(230, 225, 229),
      .inverse_on_surface = Color::Rgb(49, 48, 51),
      .scrim = Color::Rgb(0, 0, 0, 0.5F),
      .error = Color::Rgb(242, 184, 181),
  };
  theme.typography = {
      .body_large = 16.0F,
      .body_medium = 14.0F,
      .body_small = 12.0F,
      .label_large = 14.0F,
      .title_large = 22.0F,
      .headline_small = 24.0F,
  };
  theme.shapes = {
      .extra_small = 4.0F,
      .small = 8.0F,
      .medium = 12.0F,
      .large = 16.0F,
      .extra_large = 28.0F,
      .full = 10000.0F,
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
      .indication = MaterialIndication(theme.colors.primary, theme),
      .focus_ring = FocusRing{theme.colors.secondary, 3.0F, 2.0F},
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
