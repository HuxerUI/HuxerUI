#include "runtime_test_support.h"

#include <limits>

namespace huxerui::test {
namespace {

State<bool> alternate_theme;
Color observed_theme_color;
Color observed_nested_theme_color;
View NestedThemeReader();
View TestButtonTheme(std::function<View()> content);

View ThemedReader() {
  HUXERUI_SCOPE({
    observed_theme_color = UseTheme().colors.primary;
    return Column {
      Text("theme text"),
      Text("theme title", TextRole::Title),
      Text("theme label", TextRole::Label),
      Button("theme button"),
      Text("explicit text").With(huxerui::Foreground{Color::Rgb(255, 140, 0)}, huxerui::FontSize{29.0F}),
      TestButtonTheme(NestedThemeReader),
    };
  });
}

View NestedThemeReader() {
  HUXERUI_SCOPE({
    observed_nested_theme_color = UseTheme().colors.primary;
    return Button("nested button");
  });
}

View TestButtonTheme(std::function<View()> content) {
  ThemeDefinition definition;
  definition.Set(
      ButtonStyle{
          .background = Color::Rgb(130, 80, 210),
          .label_style =
              huxerui::TextStyle{
                  huxerui::Font::Monospace(21.0F)
                      .WithWeight(huxerui::FontWeight::Bold)
                      .WithSlant(huxerui::FontSlant::Italic),
                  Color::White(),
                  huxerui::TextDecoration::Underline,
              },
          .padding = huxerui::EdgeInsets::All(11.0F),
          .corner_radii = CornerRadii{13.0F},
      }
  );
  return Theme {std::move(definition), Scope(std::move(content))};
}

View TestThemeProvider(std::function<View()> content) {
  ThemeSpec spec;
  spec.colors.primary = alternate_theme ? Color::Rgb(220, 70, 50) : Color::Rgb(40, 100, 220);
  spec.colors.on_surface = Color::Rgb(30, 90, 55);
  spec.typography.body_medium = 18.0F;
  spec.typography.label_large = 16.0F;
  spec.typography.title_large = 25.0F;
  return Theme {ThemeDefinition{spec}, Scope(std::move(content))};
}

View ThemeApp() {
  alternate_theme = UseState(false);
  return TestThemeProvider(ThemedReader);
}

View FlatDarkThemeApp() {
  return TestButtonTheme([=] {
    return huxerui::FlatDarkTheme {
      Column {
        Text("dark body"),
        Text("dark title", TextRole::Title),
        Button("dark button"),
      },
    };
  });
}

View FlatThemeInteractionApp() {
  return huxerui::FlatTheme {Button("flat interaction").OnClick([] {})};
}

View MaterialThemeApp() {
  return huxerui::MaterialTheme {Button("material button").OnClick([] {})};
}

View MaterialToggleApp() {
  return huxerui::MaterialTheme {
    Row {
      Checkbox(false),
      RadioButton(false),
      Switch(false),
    },
  };
}

View MaterialDarkThemeApp() {
  return huxerui::MaterialDarkTheme {Button("material dark button")};
}

} // namespace

TEST_CASE("TestThemeProviderUpdatesNestedContent") {
  TestPlatform platform;
  Runtime runtime{ThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  REQUIRE(observed_theme_color.red == Color::Rgb(40, 100, 220).red);
  REQUIRE(observed_nested_theme_color.red == observed_theme_color.red);

  const DrawTextCommand* theme_text = FindText(initial, "theme text");
  REQUIRE(theme_text != nullptr);
  REQUIRE(theme_text->style.foreground.green == Color::Rgb(30, 90, 55).green);
  REQUIRE(theme_text->style.font.Size() == 18.0F);

  const DrawTextCommand* theme_title = FindText(initial, "theme title");
  REQUIRE(theme_title != nullptr);
  REQUIRE(theme_title->style.font.Size() == 25.0F);

  const DrawTextCommand* theme_label = FindText(initial, "theme label");
  REQUIRE(theme_label != nullptr);
  REQUIRE(theme_label->style.font.Size() == 16.0F);

  const DrawTextCommand* theme_button = FindText(initial, "theme button");
  REQUIRE(theme_button != nullptr);
  REQUIRE(theme_button->style.font.Size() == 16.0F);
  const DrawRectCommand* theme_button_background = FindRect(initial, theme_button->rect);
  REQUIRE(theme_button_background != nullptr);
  REQUIRE(SolidBrushColor(theme_button_background->brush) != nullptr);
  REQUIRE(SolidBrushColor(theme_button_background->brush)->blue == Color::Rgb(40, 100, 220).blue);

  const DrawTextCommand* nested_button = FindText(initial, "nested button");
  REQUIRE(nested_button != nullptr);
  REQUIRE(nested_button->style.font.Size() == 21.0F);
  REQUIRE(nested_button->style.font.FamilyKind() == FontFamilyKind::Monospace);
  REQUIRE(nested_button->style.font.Weight() == FontWeight::Bold);
  REQUIRE(nested_button->style.font.Slant() == FontSlant::Italic);
  REQUIRE(nested_button->style.decoration == TextDecoration::Underline);
  const DrawRectCommand* nested_button_background = FindRect(initial, nested_button->rect);
  REQUIRE(nested_button_background != nullptr);
  REQUIRE(nested_button_background->corner_radius == 13.0F);

  const DrawTextCommand* explicit_text = FindText(initial, "explicit text");
  REQUIRE(explicit_text != nullptr);
  REQUIRE(explicit_text->style.font.Size() == 29.0F);
  REQUIRE(explicit_text->style.foreground.red == Color::Rgb(255, 140, 0).red);

  alternate_theme = true;
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(observed_theme_color.red == Color::Rgb(220, 70, 50).red);
  const DrawTextCommand* updated_button = FindText(updated, "theme button");
  REQUIRE(updated_button != nullptr);
  const DrawRectCommand* updated_button_background = FindRect(updated, updated_button->rect);
  REQUIRE(updated_button_background != nullptr);
  REQUIRE(SolidBrushColor(updated_button_background->brush) != nullptr);
  REQUIRE(SolidBrushColor(updated_button_background->brush)->red == Color::Rgb(220, 70, 50).red);
}

TEST_CASE("TestFlatDarkThemeAndSemanticTextRoles") {
  TestPlatform platform;
  Runtime runtime{FlatDarkThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  const DrawTextCommand* body = FindText(scene, "dark body");
  REQUIRE(body != nullptr);
  REQUIRE(body->style.foreground.red == dark.colors.on_surface.red);
  REQUIRE(body->style.font.Size() == dark.typography.body_medium);

  const DrawTextCommand* title = FindText(scene, "dark title");
  REQUIRE(title != nullptr);
  REQUIRE(title->style.font.Size() == dark.typography.title_large);

  const DrawTextCommand* button = FindText(scene, "dark button");
  REQUIRE(button != nullptr);
  REQUIRE(button->style.foreground.red == dark.colors.on_primary.red);
  const DrawRectCommand* background = FindRect(scene, button->rect);
  REQUIRE(background != nullptr);
  REQUIRE(SolidBrushColor(background->brush) != nullptr);
  REQUIRE(SolidBrushColor(background->brush)->blue == dark.colors.primary.blue);
}

TEST_CASE("TestFlatThemeHoverAndPressedIndication") {
  const ThemeSpec light = huxerui::FlatLightThemeSpec();
  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  const Color* light_hover = LayerFillColor(light.interactions.indication.hover);
  const Color* light_press = LayerFillColor(light.interactions.indication.press);
  const Color* dark_hover = LayerFillColor(dark.interactions.indication.hover);
  const Color* dark_press = LayerFillColor(dark.interactions.indication.press);
  REQUIRE(light_hover != nullptr);
  REQUIRE(light_press != nullptr);
  REQUIRE(dark_hover != nullptr);
  REQUIRE(dark_press != nullptr);
  REQUIRE(std::abs(light_hover->alpha - 0.10F) < 0.001F);
  REQUIRE(std::abs(light_press->alpha - 0.16F) < 0.001F);
  REQUIRE(light.interactions.focus_ring.width == 2.0F);
  REQUIRE(light.interactions.focus_ring.offset == 2.0F);
  REQUIRE(std::abs(dark_hover->alpha - 0.12F) < 0.001F);
  REQUIRE(std::abs(dark_press->alpha - 0.18F) < 0.001F);

  const ThemeDefinition definition = huxerui::FlatThemeDefinition();
  const ToastStyle toast_style = ThemeDefinitionValue<ToastStyle>(definition);
  REQUIRE(SolidFillColor(toast_style.background) != nullptr);
  REQUIRE(SolidFillColor(toast_style.background)->red == light.colors.inverse_surface.red);
  REQUIRE_FALSE(toast_style.motion.has_value());

  const SnackBarStyle snack_bar_style = ThemeDefinitionValue<SnackBarStyle>(definition);
  REQUIRE(SolidFillColor(snack_bar_style.background) != nullptr);
  REQUIRE(SolidFillColor(snack_bar_style.background)->red == light.colors.inverse_surface.red);
  REQUIRE(snack_bar_style.action_text_style.foreground == light.colors.primary);
  REQUIRE_FALSE(snack_bar_style.motion.has_value());

  const TooltipStyle tooltip_style = ThemeDefinitionValue<TooltipStyle>(definition);
  REQUIRE(SolidFillColor(tooltip_style.background) != nullptr);
  REQUIRE(SolidFillColor(tooltip_style.background)->red == light.colors.inverse_surface.red);
  REQUIRE(SolidFillColor(tooltip_style.background)->alpha == light.colors.inverse_surface.alpha * 0.94F);
  REQUIRE(tooltip_style.text_style.foreground == light.colors.inverse_on_surface);
  REQUIRE(tooltip_style.maximum_width == 320.0F);
  REQUIRE(tooltip_style.shadow.blur_radius == light.elevation.low);

  const DialogStyle dialog_style = ThemeDefinitionValue<DialogStyle>(definition);
  REQUIRE(SolidFillColor(dialog_style.background) != nullptr);
  REQUIRE(SolidFillColor(dialog_style.background)->red == light.colors.surface.red);
  REQUIRE(dialog_style.motion.has_value());
  REQUIRE(dialog_style.positive_action_indication.press.has_value());
  REQUIRE(dialog_style.negative_action_indication.press.has_value());

  const BottomSheetStyle bottom_sheet_style = ThemeDefinitionValue<BottomSheetStyle>(definition);
  REQUIRE(SolidFillColor(bottom_sheet_style.background) != nullptr);
  REQUIRE(SolidFillColor(bottom_sheet_style.background)->red == light.colors.surface.red);

  const MenuStyle menu_style = ThemeDefinitionValue<MenuStyle>(definition);
  REQUIRE(menu_style.separator_mode == MenuSeparatorMode::BetweenItems);
  REQUIRE(menu_style.icon_tint == light.colors.on_surface);
  REQUIRE(menu_style.content_padding == EdgeInsets{});
  REQUIRE_FALSE(menu_style.motion.has_value());
  REQUIRE(menu_style.item_indication.press.has_value());

  const ThemeDefinition dark_definition = huxerui::FlatDarkThemeDefinition();
  const DialogStyle dark_dialog_style = ThemeDefinitionValue<DialogStyle>(dark_definition);
  REQUIRE(SolidFillColor(dark_dialog_style.background) != nullptr);
  REQUIRE(SolidFillColor(dark_dialog_style.background)->red == dark.colors.surface.red);

  TestPlatform platform;
  Runtime runtime{FlatThemeInteractionApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();
  const DrawTextCommand* button = FindText(initial, "flat interaction");
  REQUIRE(button != nullptr);
  const Point pointer{
      button->rect.x + button->rect.width * 0.5F,
      button->rect.y + button->rect.height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      105,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.fast);
  const FlattenedScene& hovered = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(hovered, *light_hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      105,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.fast);
  const FlattenedScene& pressed = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(pressed, *light_press) != nullptr);
}

TEST_CASE("TestMaterialThemeDefinitionsAndIndication") {
  const ThemeSpec light = huxerui::MaterialLightThemeSpec();
  const ThemeSpec dark = huxerui::MaterialDarkThemeSpec();
  REQUIRE(light.colors.primary.red == Color::Rgb(103, 80, 164).red);
  REQUIRE(dark.colors.primary.blue == Color::Rgb(208, 188, 255).blue);
  REQUIRE(light.typography.title_large == 22.0F);
  REQUIRE(light.shapes.extra_large == 28.0F);
  REQUIRE(light.elevation.medium == 3.0F);
  REQUIRE(light.interactions.indication.ripple.has_value());
  REQUIRE(light.interactions.focus_ring.color == light.colors.secondary);
  REQUIRE(light.interactions.focus_ring.width == 3.0F);
  REQUIRE(light.interactions.focus_ring.offset == 2.0F);
  const TweenSpec& ripple_expansion = std::get<TweenSpec>(light.interactions.indication.ripple->expansion);
  const TweenSpec& ripple_fade = std::get<TweenSpec>(light.interactions.indication.ripple->fade_out);
  REQUIRE(std::get<Easing>(ripple_expansion.easing) == Easing::Linear);
  REQUIRE(std::get<Easing>(ripple_fade.easing) == Easing::Linear);

  const ThemeDefinition definition = huxerui::MaterialThemeDefinition();
  const ButtonStyle button_style = ThemeDefinitionValue<ButtonStyle>(definition);
  REQUIRE(button_style.corner_radii == CornerRadii{20.0F});
  REQUIRE(button_style.padding.left == 24.0F);
  REQUIRE(button_style.padding.top == 8.0F);
  REQUIRE(button_style.minimum_width == 58.0F);
  REQUIRE(button_style.minimum_height == 40.0F);
  REQUIRE(button_style.indication.has_value());
  REQUIRE(button_style.indication->ripple.has_value());
  const RippleEffect& button_ripple = *button_style.indication->ripple;
  REQUIRE(button_ripple.color.red == light.colors.on_primary.red);

  const IconButtonStyle icon_button_style = ThemeDefinitionValue<IconButtonStyle>(definition);
  REQUIRE(icon_button_style.foreground == light.colors.on_surface_variant);
  REQUIRE(icon_button_style.icon_size == 24.0F);
  REQUIRE(icon_button_style.minimum_interactive_size == 48.0F);
  REQUIRE(icon_button_style.state_layer_size == 40.0F);
  REQUIRE(icon_button_style.corner_radius == 24.0F);
  REQUIRE(icon_button_style.indication.has_value());
  REQUIRE(icon_button_style.indication->ripple.has_value());
  const RippleEffect& icon_button_ripple = *icon_button_style.indication->ripple;
  REQUIRE(icon_button_ripple.color.red == light.colors.on_surface_variant.red);
  REQUIRE(icon_button_ripple.color.green == light.colors.on_surface_variant.green);
  REQUIRE(icon_button_ripple.color.blue == light.colors.on_surface_variant.blue);
  REQUIRE(icon_button_ripple.color.alpha == light.colors.on_surface_variant.alpha * 0.12F);

  const CheckboxStyle checkbox_style = ThemeDefinitionValue<CheckboxStyle>(definition);
  REQUIRE(checkbox_style.size == 18.0F);
  REQUIRE(checkbox_style.minimum_interactive_size == 48.0F);
  REQUIRE(checkbox_style.corner_radii == CornerRadii{2.0F});
  REQUIRE(SolidFillColor(checkbox_style.checked_background) != nullptr);
  REQUIRE(SolidFillColor(checkbox_style.checked_background)->red == light.colors.primary.red);

  const ChipStyle chip_style = ThemeDefinitionValue<ChipStyle>(definition);
  REQUIRE(SolidFillColor(chip_style.background) != nullptr);
  REQUIRE(*SolidFillColor(chip_style.background) == Color::Transparent());
  REQUIRE(SolidFillColor(chip_style.selected_background) != nullptr);
  REQUIRE(*SolidFillColor(chip_style.selected_background) == light.colors.secondary_container);
  REQUIRE(chip_style.label_style.foreground == light.colors.on_surface_variant);
  REQUIRE(chip_style.selected_label == light.colors.on_secondary_container);
  REQUIRE(chip_style.icon_size == 18.0F);
  REQUIRE(chip_style.icon_spacing == light.spacing.small);
  REQUIRE(chip_style.minimum_height == 32.0F);
  REQUIRE(chip_style.corner_radii == CornerRadii{light.shapes.small});
  REQUIRE(chip_style.border.color == light.colors.outline);
  REQUIRE(chip_style.indication.has_value());
  REQUIRE(chip_style.selected_indication.has_value());
  REQUIRE(chip_style.selected_indication->ripple.has_value());
  REQUIRE(chip_style.selected_indication->ripple->color.red == light.colors.on_secondary_container.red);

  const SegmentedButtonStyle segmented_button_style = ThemeDefinitionValue<SegmentedButtonStyle>(definition);
  REQUIRE(SolidFillColor(segmented_button_style.background) != nullptr);
  REQUIRE(*SolidFillColor(segmented_button_style.background) == Color::Transparent());
  REQUIRE(SolidFillColor(segmented_button_style.selected_background) != nullptr);
  REQUIRE(*SolidFillColor(segmented_button_style.selected_background) == light.colors.secondary_container);
  REQUIRE(segmented_button_style.selected_label == light.colors.on_secondary_container);
  REQUIRE(segmented_button_style.icon_size == 18.0F);
  REQUIRE(segmented_button_style.icon_spacing == light.spacing.small);
  REQUIRE(segmented_button_style.minimum_height == 40.0F);
  REQUIRE(segmented_button_style.corner_radii == CornerRadii{20.0F});
  REQUIRE(segmented_button_style.border.color == light.colors.outline);
  REQUIRE(segmented_button_style.indication.has_value());
  REQUIRE(segmented_button_style.selected_indication.has_value());

  const DividerStyle divider_style = ThemeDefinitionValue<DividerStyle>(definition);
  REQUIRE(divider_style.color.red == light.colors.outline.red);
  REQUIRE(divider_style.color.alpha == light.colors.outline.alpha * 0.4F);
  REQUIRE(divider_style.thickness == 1.0F);

  const RadioButtonStyle radio_button_style = ThemeDefinitionValue<RadioButtonStyle>(definition);
  REQUIRE(radio_button_style.size == 20.0F);
  REQUIRE(radio_button_style.minimum_interactive_size == 48.0F);
  REQUIRE(radio_button_style.state_layer_size == 40.0F);
  REQUIRE(radio_button_style.selected_color.red == light.colors.primary.red);
  REQUIRE(radio_button_style.unselected_color.red == light.colors.on_surface_variant.red);

  const SwitchStyle switch_style = ThemeDefinitionValue<SwitchStyle>(definition);
  REQUIRE(switch_style.width == 52.0F);
  REQUIRE(switch_style.height == 32.0F);
  REQUIRE(switch_style.unchecked_thumb_radius == 8.0F);
  REQUIRE(switch_style.checked_thumb_radius == 12.0F);

  const ProgressCircleStyle progress_circle_style = ThemeDefinitionValue<ProgressCircleStyle>(definition);
  REQUIRE(progress_circle_style.size == 40.0F);
  REQUIRE(progress_circle_style.stroke_width == 4.0F);
  REQUIRE(progress_circle_style.track_color == light.colors.secondary_container);
  REQUIRE(progress_circle_style.indeterminate_track_color == Color::Transparent());
  REQUIRE(progress_circle_style.indicator_color.red == light.colors.primary.red);
  REQUIRE(progress_circle_style.track_gap == 4.0F);
  REQUIRE(progress_circle_style.indeterminate_motion == huxerui::ProgressCircleIndeterminateMotion::PulsingArc);
  REQUIRE(progress_circle_style.minimum_indeterminate_arc_fraction == 0.1F);
  REQUIRE(progress_circle_style.maximum_indeterminate_arc_fraction == 0.87F);
  REQUIRE(progress_circle_style.animation_duration == 6.0);

  const ProgressBarStyle progress_bar_style = ThemeDefinitionValue<ProgressBarStyle>(definition);
  REQUIRE(progress_bar_style.width == 240.0F);
  REQUIRE(progress_bar_style.height == 4.0F);
  REQUIRE(progress_bar_style.track_color == light.colors.secondary_container);
  REQUIRE(progress_bar_style.indicator_color.red == light.colors.primary.red);
  REQUIRE(progress_bar_style.track_gap == 4.0F);
  REQUIRE(progress_bar_style.stop_indicator_size == 4.0F);
  REQUIRE(progress_bar_style.indeterminate_motion == huxerui::ProgressBarIndeterminateMotion::Segmented);
  REQUIRE(progress_bar_style.animation_duration == 1.75);

  const SliderStyle slider_style = ThemeDefinitionValue<SliderStyle>(definition);
  REQUIRE(slider_style.width == 160.0F);
  REQUIRE(slider_style.height == 48.0F);
  REQUIRE(slider_style.track_height == 16.0F);
  REQUIRE(slider_style.thumb_width == 4.0F);
  REQUIRE(slider_style.thumb_height == 44.0F);
  REQUIRE(slider_style.pressed_thumb_width == 2.0F);
  REQUIRE(slider_style.thumb_track_gap == 6.0F);
  REQUIRE(slider_style.stop_indicator_size == 4.0F);
  REQUIRE(slider_style.tick_size == 4.0F);
  REQUIRE(slider_style.active_track.red == light.colors.primary.red);
  REQUIRE(slider_style.inactive_track == light.colors.secondary_container);
  REQUIRE(slider_style.disabled_active_track.alpha == 0.38F);
  REQUIRE(slider_style.disabled_inactive_track.alpha == 0.12F);
  REQUIRE(slider_style.disabled_thumb.alpha == 0.38F);
  REQUIRE(slider_style.focus_ring.has_value());
  REQUIRE(slider_style.focus_ring->width == 0.0F);
  const huxerui::ToastStyle toast_style = ThemeDefinitionValue<huxerui::ToastStyle>(definition);
  REQUIRE(SolidFillColor(toast_style.background) != nullptr);
  REQUIRE(SolidFillColor(toast_style.background)->red == Color::Rgb(50, 47, 53).red);

  const huxerui::SnackBarStyle snack_bar_style = ThemeDefinitionValue<huxerui::SnackBarStyle>(definition);
  REQUIRE(SolidFillColor(snack_bar_style.background) != nullptr);
  REQUIRE(*SolidFillColor(snack_bar_style.background) == light.colors.inverse_surface);
  REQUIRE(snack_bar_style.action_text_style.foreground == light.colors.primary);
  REQUIRE(snack_bar_style.motion.has_value());

  const huxerui::TooltipStyle tooltip_style = ThemeDefinitionValue<huxerui::TooltipStyle>(definition);
  REQUIRE(SolidFillColor(tooltip_style.background) != nullptr);
  REQUIRE(*SolidFillColor(tooltip_style.background) == light.colors.inverse_surface);
  REQUIRE(tooltip_style.text_style.foreground == light.colors.inverse_on_surface);
  REQUIRE(tooltip_style.maximum_width == 200.0F);
  REQUIRE(tooltip_style.shadow == Shadow{});

  const huxerui::DialogStyle dialog_style = ThemeDefinitionValue<huxerui::DialogStyle>(definition);
  REQUIRE(dialog_style.scrim.alpha == light.colors.scrim.alpha);
  REQUIRE(dialog_style.shadow.offset == Point{});
  REQUIRE(dialog_style.shadow.blur_radius == light.elevation.medium * 4.0F);
  REQUIRE(dialog_style.minimum_width == 280.0F);
  REQUIRE(dialog_style.shadow.spread == 0.0F);
  REQUIRE(dialog_style.motion.has_value());
  REQUIRE(dialog_style.motion->initial_scale == 0.94F);
  REQUIRE(std::get<TweenSpec>(dialog_style.motion->enter).duration == light.motion.normal);
  REQUIRE(std::get<TweenSpec>(dialog_style.motion->exit).duration == light.motion.fast);
  REQUIRE(dialog_style.positive_action_indication.ripple.has_value());
  REQUIRE(dialog_style.positive_action_indication.ripple->color.red == light.colors.primary.red);
  REQUIRE(dialog_style.positive_action_indication.ripple->color.alpha < light.colors.primary.alpha);

  const huxerui::BottomSheetStyle bottom_sheet_style = ThemeDefinitionValue<huxerui::BottomSheetStyle>(definition);
  REQUIRE(SolidFillColor(bottom_sheet_style.background) != nullptr);
  REQUIRE(SolidFillColor(bottom_sheet_style.background)->red == light.colors.surface_container_low.red);

  const huxerui::MenuStyle menu_style = ThemeDefinitionValue<huxerui::MenuStyle>(definition);
  REQUIRE(menu_style.separator_mode == huxerui::MenuSeparatorMode::None);
  REQUIRE(menu_style.icon_tint == light.colors.on_surface_variant);
  REQUIRE(menu_style.content_padding == EdgeInsets{});
  REQUIRE(menu_style.minimum_width == 112.0F);
  REQUIRE(menu_style.minimum_item_height == 48.0F);
  REQUIRE(menu_style.motion.has_value());
  REQUIRE(menu_style.item_indication.ripple.has_value());

  const huxerui::ScrollBarStyle scroll_bar_style = ThemeDefinitionValue<huxerui::ScrollBarStyle>(definition);
  REQUIRE(scroll_bar_style.thickness == 4.0F);
  REQUIRE(scroll_bar_style.corner_radius == 2.0F);

  ThemeSpec brand = light;
  brand.colors.primary = Color::Rgb(20, 110, 90);
  const ThemeDefinition brand_definition = huxerui::MaterialThemeDefinition(brand);
  const ButtonStyle brand_button_style = ThemeDefinitionValue<ButtonStyle>(brand_definition);
  REQUIRE(SolidFillColor(brand_button_style.background) != nullptr);
  REQUIRE(SolidFillColor(brand_button_style.background)->green == brand.colors.primary.green);

  const ThemeDefinition dark_definition = huxerui::MaterialDarkThemeDefinition();
  const DialogStyle dark_dialog_style = ThemeDefinitionValue<DialogStyle>(dark_definition);
  REQUIRE(SolidFillColor(dark_dialog_style.background) != nullptr);
  REQUIRE(SolidFillColor(dark_dialog_style.background)->red == dark.colors.surface_container_high.red);

  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{MaterialThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 80.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();
  const DrawTextCommand* button = FindText(initial, "material button");
  REQUIRE(button != nullptr);
  REQUIRE(button->style.foreground.red == light.colors.on_primary.red);
  REQUIRE(button->style.font.Size() == light.typography.label_large);
  const DrawRectCommand* background = FindRect(initial, button->rect);
  REQUIRE(background != nullptr);
  REQUIRE(SolidBrushColor(background->brush) != nullptr);
  REQUIRE(SolidBrushColor(background->brush)->red == light.colors.primary.red);
  REQUIRE(background->corner_radius == 20.0F);

  const Point pointer{
      button->rect.x + button->rect.width * 0.5F,
      button->rect.y + button->rect.height * 0.5F,
  };
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      110,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.fast);
  const FlattenedScene& hovered = runtime.BuildFrame();
  const Color* button_hover = LayerFillColor(button_style.indication->hover);
  REQUIRE(button_hover != nullptr);
  REQUIRE(FindRectWithColor(hovered, *button_hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      110,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.slow * 0.5);
  const FlattenedScene& pressed = runtime.BuildFrame();
  const huxerui::DrawCircleCommand* ripple = nullptr;
  const PushClipCommand* ripple_clip = nullptr;
  for (const auto& command : pressed.Commands()) {
    if (const auto* clip = std::get_if<PushClipCommand>(&command); clip && clip->corner_radius > 0.0F) {
      ripple_clip = clip;
    }
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    if (circle && circle->color.alpha > 0.0F) {
      ripple = circle;
      break;
    }
  }
  REQUIRE(ripple != nullptr);
  REQUIRE(ripple_clip != nullptr);
  REQUIRE(ripple_clip->corner_radius == 20.0F);
  REQUIRE(ripple->radius > 0.0F);
  REQUIRE(ripple->color == button_ripple.color);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      110,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.normal);
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Space,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.slow * 0.5);
  const FlattenedScene& keyboard_pressed = runtime.BuildFrame();
  const huxerui::DrawCircleCommand* keyboard_ripple = nullptr;
  for (const auto& command : keyboard_pressed.Commands()) {
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    if (circle && circle->radius > 0.0F) {
      keyboard_ripple = circle;
      break;
    }
  }
  REQUIRE(keyboard_ripple != nullptr);
  REQUIRE(std::abs(keyboard_ripple->center.x - pointer.x) < 0.01F);
  REQUIRE(std::abs(keyboard_ripple->center.y - pointer.y) < 0.01F);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Space,
  });

  Runtime dark_runtime{MaterialDarkThemeApp, platform};
  dark_runtime.SetWindowMetrics({.viewport = {240.0F, 80.0F}});
  const FlattenedScene& dark_display = dark_runtime.BuildFrame();
  const DrawTextCommand* dark_button = FindText(dark_display, "material dark button");
  REQUIRE(dark_button != nullptr);
  const DrawRectCommand* dark_background = FindRect(dark_display, dark_button->rect);
  REQUIRE(dark_background != nullptr);
  REQUIRE(SolidBrushColor(dark_background->brush) != nullptr);
  REQUIRE(SolidBrushColor(dark_background->brush)->red == dark.colors.primary.red);

  Runtime toggle_runtime{MaterialToggleApp, platform};
  toggle_runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  toggle_runtime.BuildFrame();
  const detail::MountedNode* toggle_root = toggle_runtime.RootNode();
  REQUIRE(toggle_root != nullptr);
  const detail::MountedNode* material_checkbox = FindMountedKind(*toggle_root, detail::NodeKind::Checkbox);
  const detail::MountedNode* material_radio = FindMountedKind(*toggle_root, detail::NodeKind::RadioButton);
  const detail::MountedNode* material_switch = FindMountedKind(*toggle_root, detail::NodeKind::Switch);
  REQUIRE(material_checkbox != nullptr);
  REQUIRE(material_radio != nullptr);
  REQUIRE(material_switch != nullptr);
  REQUIRE(material_checkbox->measured_size == Size{48.0F, 48.0F});
  REQUIRE(material_radio->measured_size == Size{48.0F, 48.0F});
  REQUIRE(material_switch->measured_size == Size{52.0F, 48.0F});
}

SystemColorScheme observed_system_color_scheme = SystemColorScheme::Light;
int system_color_scheme_compositions = 0;

View SystemColorSchemeReader() {
  HUXERUI_SCOPE({
    observed_system_color_scheme = UseSystemColorScheme();
    ++system_color_scheme_compositions;
    return Text("system scheme");
  });
}

TEST_CASE("SystemColorSchemeDefaultsToLight") {
  system_color_scheme_compositions = 0;
  observed_system_color_scheme = SystemColorScheme::Dark;
  TestPlatform platform;
  Runtime runtime{SystemColorSchemeReader, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(observed_system_color_scheme == SystemColorScheme::Light);
}

TEST_CASE("SystemColorSchemeSeedsFromPlatformAdapter") {
  system_color_scheme_compositions = 0;
  observed_system_color_scheme = SystemColorScheme::Light;
  TestPlatform platform;
  platform.system_color_scheme = SystemColorScheme::Dark;
  Runtime runtime{SystemColorSchemeReader, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(observed_system_color_scheme == SystemColorScheme::Dark);
}

TEST_CASE("UpdateSystemColorSchemeRecomposesSubscribers") {
  system_color_scheme_compositions = 0;
  observed_system_color_scheme = SystemColorScheme::Dark;
  TestPlatform platform;
  Runtime runtime{SystemColorSchemeReader, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(observed_system_color_scheme == SystemColorScheme::Light);
  const int initial_compositions = system_color_scheme_compositions;

  runtime.UpdateSystemColorScheme(SystemColorScheme::Light);
  runtime.BuildFrame();
  REQUIRE(observed_system_color_scheme == SystemColorScheme::Light);
  REQUIRE(system_color_scheme_compositions == initial_compositions);

  runtime.UpdateSystemColorScheme(SystemColorScheme::Dark);
  runtime.BuildFrame();
  REQUIRE(observed_system_color_scheme == SystemColorScheme::Dark);
  REQUIRE(system_color_scheme_compositions > initial_compositions);
}

} // namespace huxerui::test
