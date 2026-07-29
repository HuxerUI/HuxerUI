#include "runtime_test_support.h"

namespace huxerui::test {

struct TestEnvironmentKey {
  using Value = std::string;

  static Value Default() {
    return "fallback";
  }
};

std::vector<std::string> observed_environment_values;
State<bool> alternate_theme;
Color observed_theme_color;
Color observed_nested_theme_color;

struct TestRootService {
  LayerController *layers = nullptr;
  int value = 0;
};

std::shared_ptr<TestRootService> installed_root_service;
int observed_root_service_value = 0;
int root_app_clicks = 0;
std::optional<ToastHandle> saved_toast;
std::optional<DialogHandle> saved_dialogs;
std::optional<DialogContext> saved_dialog_context;
State<bool> declarative_dialog_visible;
State<bool> animation_target;
int indication_clicks = 0;
State<bool> show_modifier_branch;
State<bool> first_focus_enabled;
std::vector<std::string> focus_changes;
std::vector<Key> received_keys;
int first_keyboard_clicks = 0;
int third_keyboard_clicks = 0;
int custom_keyboard_clicks = 0;
int disabled_clicks = 0;
int underlying_clicks = 0;
int background_dialog_clicks = 0;
int first_dialog_clicks = 0;
int second_dialog_clicks = 0;
State<bool> checkbox_checked;
State<bool> switch_checked;
int checkbox_changes = 0;
int switch_changes = 0;
State<float> progress_circle_value;

View EnvironmentReader() {
  HUXERUI_SCOPE({
    observed_environment_values.push_back(UseEnvironment<TestEnvironmentKey>());
    return Text(UseEnvironment<TestEnvironmentKey>());
  });
}

View EnvironmentApp() {
  EnvironmentValues outer;
  outer.Set<TestEnvironmentKey>("outer");
  return huxerui::ProvideEnvironment(std::move(outer), [] {
    return Column{
        EnvironmentReader(),
        huxerui::ProvideEnvironment<TestEnvironmentKey>(
            "inner", EnvironmentReader),
    };
  });
}

View NestedThemeReader();
View TestButtonTheme(std::function<View()> content);

View ThemedReader() {
  HUXERUI_SCOPE({
    observed_theme_color = UseTheme().colors.primary;
    return Column{
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
  definition.Set<ButtonStyleKey>(ButtonStyle{
      .background = Color::Rgb(130, 80, 210),
      .foreground = Color::White(),
      .font_size = 21.0F,
      .padding = huxerui::EdgeInsets::All(11.0F),
      .corner_radius = 13.0F,
  });
  return Theme(std::move(definition), std::move(content));
}

View TestThemeProvider(std::function<View()> content) {
  ThemeSpec spec;
  spec.colors.primary = alternate_theme ? Color::Rgb(220, 70, 50) : Color::Rgb(40, 100, 220);
  spec.colors.on_surface = Color::Rgb(30, 90, 55);
  spec.typography.body = 18.0F;
  spec.typography.label = 16.0F;
  spec.typography.title = 25.0F;
  return Theme(ThemeDefinition{spec}, std::move(content));
}

View ThemeApp() {
  alternate_theme = UseState(false);
  return TestThemeProvider(ThemedReader);
}

View FlatDarkThemeApp() {
  return HUXERUI_THEME(TestButtonTheme, HUXERUI_THEME(huxerui::FlatDarkTheme, Column{
                                                                                  Text("dark body"),
                                                                                  Text("dark title", TextRole::Title),
                                                                                  Button("dark button"),
                                                                              }));
}

View FlatThemeInteractionApp() {
  return HUXERUI_THEME(huxerui::FlatTheme, Button("flat interaction").OnClick([] {}));
}

View MaterialThemeApp() {
  return HUXERUI_THEME(huxerui::MaterialTheme, Button("material button").OnClick([] {}));
}

View MaterialDarkThemeApp() {
  return HUXERUI_THEME(huxerui::MaterialDarkTheme, Button("material dark button"));
}

View ToggleApp() {
  auto checkbox = UseState(false);
  auto switch_value = UseState(false);
  checkbox_checked = checkbox;
  switch_checked = switch_value;
  return Row{
      Checkbox(checkbox).OnChanged([checkbox](bool checked) {
        ++checkbox_changes;
        checkbox = checked;
      }),
      Switch(switch_value).On<ToggleEvents::Changed>([switch_value](bool checked) {
        ++switch_changes;
        switch_value = checked;
      }),
  }
      .With(huxerui::Spacing{8.0F});
}

View DeterminateProgressCircleApp() {
  auto progress = UseState(0.25F);
  progress_circle_value = progress;
  return Row{
      ProgressCircle(progress),
  };
}

View IndeterminateProgressCircleApp() {
  return ProgressCircle();
}

View EmptyProgressCircleApp() {
  return ProgressCircle(-1.0F);
}

View FullProgressCircleApp() {
  return ProgressCircle(2.0F);
}

template <class Factory> View ReducedMotionProgressTheme(Factory &&content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  return Theme(ThemeDefinition{spec}, std::forward<Factory>(content));
}

View ReducedMotionProgressCircleApp() {
  return HUXERUI_THEME(ReducedMotionProgressTheme, ProgressCircle());
}

template <class Factory> View InteractionTestTheme(Factory &&content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.hover_overlay = Color::Rgb(20, 80, 160, 0.2F);
  spec.interactions.pressed_overlay = Color::Rgb(200, 40, 60, 0.3F);
  return Theme(ThemeDefinition{spec}, std::forward<Factory>(content));
}

View ThemedIndicationApp() {
  return HUXERUI_THEME(InteractionTestTheme, Button("themed indication").OnClick([] {}));
}

template <class Factory> View FocusTestTheme(Factory &&content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.focus_ring = Color::Rgb(40, 180, 90);
  spec.interactions.focus_ring_width = 3.0F;
  spec.interactions.disabled_opacity = 0.3F;
  return Theme(ThemeDefinition{spec}, std::forward<Factory>(content));
}

View FocusContent() {
  HUXERUI_SCOPE({
    first_focus_enabled = UseState(true);
    return Column{
        Button("first")
            .With(Enabled{first_focus_enabled})
            .OnClick([] { ++first_keyboard_clicks; })
            .On<ViewEvents::FocusChanged>(
                [](bool focused) { focus_changes.push_back(focused ? "first:on" : "first:off"); }),
        Button("disabled").With(Enabled{false}).OnClick([] { ++disabled_clicks; }),
        Button("third").OnClick([] { ++third_keyboard_clicks; }).On<ViewEvents::FocusChanged>([](bool focused) {
          focus_changes.push_back(focused ? "third:on" : "third:off");
        }),
        Text("custom focus")
            .With(Focusable{})
            .OnClick([] { ++custom_keyboard_clicks; })
            .On<ViewEvents::KeyDown>([](const KeyEvent &event) { received_keys.push_back(event.key); }),
    };
  });
}

View FocusApp() {
  return FocusTestTheme(FocusContent);
}

View DisabledHitTestApp() {
  return Stack{
      Button("underlying").OnClick([] { ++underlying_clicks; }),
      Button("disabled overlay").With(Enabled{false}).OnClick([] { ++disabled_clicks; }),
  };
}

View DisabledSubtreeApp() {
  return Column{
      Button("disabled child").With(Enabled{true}).OnClick([] { ++disabled_clicks; }),
  }
      .With(Enabled{false});
}

View FocusDialogApp() {
  HUXERUI_SCOPE({
    saved_dialogs = UseDialog();
    return Button("background focus").OnClick([] { ++background_dialog_clicks; });
  });
}

View RootHookApp() {
  HUXERUI_SCOPE({
    observed_root_service_value = UseService<TestRootService>()->value;
    return Button("application").OnClick([] { ++root_app_clicks; });
  });
}

View PresentationApp() {
  HUXERUI_SCOPE({
    saved_toast = UseToast();
    saved_dialogs = UseDialog();
    return Text("content");
  });
}

View PresentationThemeApp() {
  ThemeDefinition definition;
  definition.Set<huxerui::ToastStyleKey>(huxerui::ToastStyle{
      .background = Color::Rgb(20, 30, 40, 0.9F),
      .foreground = Color::Rgb(240, 245, 250),
      .padding = 10.0F,
      .corner_radius = 9.0F,
  });
  definition.Set<huxerui::DialogStyleKey>(huxerui::DialogStyle{
      .scrim = Color::Rgb(180, 20, 20, 0.3F),
  });
  return Theme(std::move(definition), PresentationApp);
}

View FlatDarkPresentationApp() {
  return huxerui::FlatDarkTheme(PresentationApp);
}

View DeclarativeDialogApp() {
  declarative_dialog_visible = UseState(false);
  return Text("content").With(Dialog{
      .visible = declarative_dialog_visible,
      .content = [] { return Text("declarative dialog"); },
      .dismiss_on_outside_press = true,
      .on_dismiss_request = [visible = declarative_dialog_visible] { visible = false; },
  });
}

View AnimationApp() {
  animation_target = UseState(false);
  const bool moved = animation_target.Get();
  return Text("animated")
      .With(Offset{AnimateTo(Point{moved ? 100.0F : 0.0F, 0.0F}, TweenSpec{1.0, Easing::Linear})},
            Opacity{AnimateTo(moved ? 0.0F : 1.0F, TweenSpec{1.0, Easing::Linear})});
}

View IndicationApp() {
  return Button("press").OnClick([] { ++indication_clicks; });
}

View PresentedIndicationApp() {
  return Stack{
      Button("presented").With(huxerui::Frame{80.0F, 40.0F}, Offset{Point{50.0F, 0.0F}}, Opacity{0.5F}).OnClick([] {}),
  };
}

View ExplicitIndicationApp() {
  return Button("explicit")
      .OnClick([] { ++indication_clicks; })
      .With(huxerui::Indication{
          huxerui::NoIndication{},
      });
}

View ModifierPruningApp() {
  auto visible = UseState(true);
  show_modifier_branch = visible;
  if (visible.Get()) {
    return Column{
        Text("plain"),
        Button("interactive").OnClick([] {}),
    };
  }
  return Column{
      Text("plain"),
  };
}

TEST_CASE("TestNestedEnvironmentValues") {
  observed_environment_values.clear();

  TestPlatform platform;
  Runtime runtime{EnvironmentApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  REQUIRE(observed_environment_values.size() == 2);
  REQUIRE(observed_environment_values[0] == "outer");
  REQUIRE(observed_environment_values[1] == "inner");
}

TEST_CASE("TestThemeProviderUpdatesNestedContent") {
  TestPlatform platform;
  Runtime runtime{ThemeApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  const DisplayList &initial = runtime.BuildFrame();

  REQUIRE(observed_theme_color.red == Color::Rgb(40, 100, 220).red);
  REQUIRE(observed_nested_theme_color.red == observed_theme_color.red);

  const DrawTextCommand *theme_text = FindText(initial, "theme text");
  REQUIRE(theme_text != nullptr);
  REQUIRE(theme_text->color.green == Color::Rgb(30, 90, 55).green);
  REQUIRE(theme_text->font_size == 18.0F);

  const DrawTextCommand *theme_title = FindText(initial, "theme title");
  REQUIRE(theme_title != nullptr);
  REQUIRE(theme_title->font_size == 25.0F);

  const DrawTextCommand *theme_label = FindText(initial, "theme label");
  REQUIRE(theme_label != nullptr);
  REQUIRE(theme_label->font_size == 16.0F);

  const DrawTextCommand *theme_button = FindText(initial, "theme button");
  REQUIRE(theme_button != nullptr);
  REQUIRE(theme_button->font_size == 16.0F);
  const DrawRectCommand *theme_button_background = FindRect(initial, theme_button->rect);
  REQUIRE(theme_button_background != nullptr);
  REQUIRE(theme_button_background->color.blue == Color::Rgb(40, 100, 220).blue);

  const DrawTextCommand *nested_button = FindText(initial, "nested button");
  REQUIRE(nested_button != nullptr);
  REQUIRE(nested_button->font_size == 21.0F);
  const DrawRectCommand *nested_button_background = FindRect(initial, nested_button->rect);
  REQUIRE(nested_button_background != nullptr);
  REQUIRE(nested_button_background->corner_radius == 13.0F);

  const DrawTextCommand *explicit_text = FindText(initial, "explicit text");
  REQUIRE(explicit_text != nullptr);
  REQUIRE(explicit_text->font_size == 29.0F);
  REQUIRE(explicit_text->color.red == Color::Rgb(255, 140, 0).red);

  alternate_theme = true;
  const DisplayList &updated = runtime.BuildFrame();
  REQUIRE(observed_theme_color.red == Color::Rgb(220, 70, 50).red);
  const DrawTextCommand *updated_button = FindText(updated, "theme button");
  REQUIRE(updated_button != nullptr);
  const DrawRectCommand *updated_button_background = FindRect(updated, updated_button->rect);
  REQUIRE(updated_button_background != nullptr);
  REQUIRE(updated_button_background->color.red == Color::Rgb(220, 70, 50).red);
}

TEST_CASE("TestFlatDarkThemeAndSemanticTextRoles") {
  TestPlatform platform;
  Runtime runtime{FlatDarkThemeApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  const DisplayList &display_list = runtime.BuildFrame();

  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  const DrawTextCommand *body = FindText(display_list, "dark body");
  REQUIRE(body != nullptr);
  REQUIRE(body->color.red == dark.colors.on_surface.red);
  REQUIRE(body->font_size == dark.typography.body);

  const DrawTextCommand *title = FindText(display_list, "dark title");
  REQUIRE(title != nullptr);
  REQUIRE(title->font_size == dark.typography.title);

  const DrawTextCommand *button = FindText(display_list, "dark button");
  REQUIRE(button != nullptr);
  REQUIRE(button->color.red == dark.colors.on_primary.red);
  const DrawRectCommand *background = FindRect(display_list, button->rect);
  REQUIRE(background != nullptr);
  REQUIRE(background->color.blue == dark.colors.primary.blue);
}

TEST_CASE("TestFlatThemeHoverAndPressedIndication") {
  const ThemeSpec light = huxerui::FlatLightThemeSpec();
  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  REQUIRE(std::abs(light.interactions.hover_overlay.alpha - 0.10F) < 0.001F);
  REQUIRE(std::abs(light.interactions.pressed_overlay.alpha - 0.16F) < 0.001F);
  REQUIRE(std::abs(dark.interactions.hover_overlay.alpha - 0.12F) < 0.001F);
  REQUIRE(std::abs(dark.interactions.pressed_overlay.alpha - 0.18F) < 0.001F);

  TestPlatform platform;
  Runtime runtime{FlatThemeInteractionApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  const DisplayList &initial = runtime.BuildFrame();
  const DrawTextCommand *button = FindText(initial, "flat interaction");
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
  const DisplayList &hovered = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(hovered, light.interactions.hover_overlay) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      105,
      pointer,
  });
  const DisplayList &pressed = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(pressed, light.interactions.pressed_overlay) != nullptr);
}

TEST_CASE("TestMaterialThemeDefinitionsAndIndication") {
  const ThemeSpec light = huxerui::MaterialLightThemeSpec();
  const ThemeSpec dark = huxerui::MaterialDarkThemeSpec();
  REQUIRE(light.colors.primary.red == Color::Rgb(103, 80, 164).red);
  REQUIRE(dark.colors.primary.blue == Color::Rgb(208, 188, 255).blue);
  REQUIRE(light.typography.title == 22.0F);
  REQUIRE(light.shapes.large == 28.0F);
  REQUIRE(light.elevation.medium == 3.0F);
  REQUIRE(light.interactions.indication == huxerui::IndicationKind::Ripple);

  const ThemeDefinition definition = huxerui::MaterialThemeDefinition();
  const auto *button_style = std::any_cast<ButtonStyle>(definition.Values().Find(typeid(ButtonStyleKey)));
  REQUIRE(button_style != nullptr);
  REQUIRE(button_style->corner_radius == 20.0F);
  REQUIRE(button_style->padding.left == 24.0F);
  REQUIRE(button_style->padding.top == 10.0F);

  const auto *checkbox_style = std::any_cast<CheckboxStyle>(definition.Values().Find(typeid(CheckboxStyleKey)));
  REQUIRE(checkbox_style != nullptr);
  REQUIRE(checkbox_style->size == 20.0F);
  REQUIRE(checkbox_style->corner_radius == 2.0F);
  REQUIRE(checkbox_style->checked_background.red == light.colors.primary.red);

  const auto *switch_style = std::any_cast<SwitchStyle>(definition.Values().Find(typeid(SwitchStyleKey)));
  REQUIRE(switch_style != nullptr);
  REQUIRE(switch_style->width == 52.0F);
  REQUIRE(switch_style->height == 32.0F);
  REQUIRE(switch_style->thumb_radius == 12.0F);

  const auto *progress_circle_style =
      std::any_cast<ProgressCircleStyle>(definition.Values().Find(typeid(ProgressCircleStyleKey)));
  REQUIRE(progress_circle_style != nullptr);
  REQUIRE(progress_circle_style->size == 40.0F);
  REQUIRE(progress_circle_style->stroke_width == 4.0F);
  REQUIRE(progress_circle_style->indicator_color.red == light.colors.primary.red);

  const auto *toast_style =
      std::any_cast<huxerui::ToastStyle>(definition.Values().Find(typeid(huxerui::ToastStyleKey)));
  REQUIRE(toast_style != nullptr);
  REQUIRE(toast_style->background.red == Color::Rgb(50, 47, 53).red);

  const auto *dialog_style =
      std::any_cast<huxerui::DialogStyle>(definition.Values().Find(typeid(huxerui::DialogStyleKey)));
  REQUIRE(dialog_style != nullptr);
  REQUIRE(dialog_style->scrim.alpha == light.colors.scrim.alpha);

  const auto *scroll_bar_style =
      std::any_cast<huxerui::ScrollBarStyle>(definition.Values().Find(typeid(huxerui::ScrollBarStyleKey)));
  REQUIRE(scroll_bar_style != nullptr);
  REQUIRE(scroll_bar_style->thickness == 4.0F);
  REQUIRE(scroll_bar_style->corner_radius == 2.0F);

  ThemeSpec brand = light;
  brand.colors.primary = Color::Rgb(20, 110, 90);
  const ThemeDefinition brand_definition = huxerui::MaterialThemeDefinition(brand);
  const auto *brand_button_style = std::any_cast<ButtonStyle>(brand_definition.Values().Find(typeid(ButtonStyleKey)));
  REQUIRE(brand_button_style != nullptr);
  REQUIRE(brand_button_style->background.green == brand.colors.primary.green);

  TestPlatform platform;
  Runtime runtime{MaterialThemeApp, platform};
  runtime.SetViewport({240.0F, 80.0F});
  const DisplayList &initial = runtime.BuildFrame();
  const DrawTextCommand *button = FindText(initial, "material button");
  REQUIRE(button != nullptr);
  REQUIRE(button->color.red == light.colors.on_primary.red);
  REQUIRE(button->font_size == light.typography.label);
  const DrawRectCommand *background = FindRect(initial, button->rect);
  REQUIRE(background != nullptr);
  REQUIRE(background->color.red == light.colors.primary.red);
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
  const DisplayList &hovered = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(hovered, light.interactions.hover_overlay) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      110,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.slow * 0.5);
  const DisplayList &pressed = runtime.BuildFrame();
  const huxerui::DrawCircleCommand *ripple = nullptr;
  const PushClipCommand *ripple_clip = nullptr;
  for (const auto &command : pressed.Commands()) {
    if (const auto *clip = std::get_if<PushClipCommand>(&command); clip && clip->corner_radius > 0.0F) {
      ripple_clip = clip;
    }
    const auto *circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    if (circle && circle->color.alpha > 0.0F) {
      ripple = circle;
      break;
    }
  }
  REQUIRE(ripple != nullptr);
  REQUIRE(ripple_clip != nullptr);
  REQUIRE(ripple_clip->corner_radius == 20.0F);
  REQUIRE(ripple->radius > 0.0F);
  REQUIRE(ripple->color.alpha == light.interactions.ripple.alpha);

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
  const DisplayList &keyboard_pressed = runtime.BuildFrame();
  const huxerui::DrawCircleCommand *keyboard_ripple = nullptr;
  for (const auto &command : keyboard_pressed.Commands()) {
    const auto *circle = std::get_if<huxerui::DrawCircleCommand>(&command);
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
  dark_runtime.SetViewport({240.0F, 80.0F});
  const DisplayList &dark_display = dark_runtime.BuildFrame();
  const DrawTextCommand *dark_button = FindText(dark_display, "material dark button");
  REQUIRE(dark_button != nullptr);
  const DrawRectCommand *dark_background = FindRect(dark_display, dark_button->rect);
  REQUIRE(dark_background != nullptr);
  REQUIRE(dark_background->color.red == dark.colors.primary.red);
}

TEST_CASE("TestControlledTogglesAndAnimation") {
  checkbox_changes = 0;
  switch_changes = 0;

  TestPlatform platform;
  Runtime runtime{ToggleApp, platform};
  runtime.SetViewport({160.0F, 64.0F});
  const DisplayList &initial = runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  const auto *checkbox = root->children[0].get();
  const auto *switch_node = root->children[1].get();
  REQUIRE(checkbox->kind == huxerui::detail::NodeKind::Checkbox);
  REQUIRE(switch_node->kind == huxerui::detail::NodeKind::Switch);
  REQUIRE(checkbox->focusable);
  REQUIRE(switch_node->focusable);
  REQUIRE(checkbox->measured_size.width == 20.0F);
  REQUIRE(switch_node->measured_size.width == 40.0F);

  const huxerui::DrawCircleCommand *initial_thumb = nullptr;
  for (const auto &command : initial.Commands()) {
    if (const auto *circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      initial_thumb = circle;
      break;
    }
  }
  REQUIRE(initial_thumb != nullptr);
  const float initial_thumb_x = initial_thumb->center.x;

  const std::uint64_t checkbox_identity = checkbox->identity;
  ClickAt(runtime, {
                       checkbox->frame.x + checkbox->frame.width * 0.5F,
                       checkbox->frame.y + checkbox->frame.height * 0.5F,
                   });
  const DisplayList &checked_display = runtime.BuildFrame();
  REQUIRE(checkbox_changes == 1);
  REQUIRE(checkbox_checked.Get());
  REQUIRE(FindText(checked_display, "✓") != nullptr);
  REQUIRE(runtime.RootNode()->children[0]->identity == checkbox_identity);

  switch_node = runtime.RootNode()->children[1].get();
  ClickAt(runtime, {
                       switch_node->frame.x + switch_node->frame.width * 0.5F,
                       switch_node->frame.y + switch_node->frame.height * 0.5F,
                   });
  const DisplayList &switch_start = runtime.BuildFrame();
  REQUIRE(switch_changes == 1);
  REQUIRE(switch_checked.Get());

  const huxerui::DrawCircleCommand *start_thumb = nullptr;
  for (const auto &command : switch_start.Commands()) {
    if (const auto *circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      start_thumb = circle;
      break;
    }
  }
  REQUIRE(start_thumb != nullptr);
  REQUIRE(std::abs(start_thumb->center.x - initial_thumb_x) < 0.001F);

  platform.AdvanceTime(0.1);
  const DisplayList &switch_middle = runtime.BuildFrame();
  const huxerui::DrawCircleCommand *middle_thumb = nullptr;
  for (const auto &command : switch_middle.Commands()) {
    if (const auto *circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      middle_thumb = circle;
      break;
    }
  }
  REQUIRE(middle_thumb != nullptr);
  REQUIRE(middle_thumb->center.x > initial_thumb_x);
  const float middle_thumb_x = middle_thumb->center.x;

  platform.AdvanceTime(0.2);
  const DisplayList &switch_end = runtime.BuildFrame();
  const huxerui::DrawCircleCommand *end_thumb = nullptr;
  for (const auto &command : switch_end.Commands()) {
    if (const auto *circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      end_thumb = circle;
      break;
    }
  }
  REQUIRE(end_thumb != nullptr);
  REQUIRE(end_thumb->center.x > middle_thumb_x);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Space,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Space,
  });
  REQUIRE(checkbox_changes == 2);
  REQUIRE(!checkbox_checked.Get());

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(switch_changes == 2);
  REQUIRE(!switch_checked.Get());
}

TEST_CASE("TestProgressCircleDrawingStateAndAnimation") {
  constexpr float pi = 3.14159265358979323846F;
  const auto arcs = [](const DisplayList &display_list) {
    std::vector<DrawArcCommand> result;
    for (const auto &command : display_list.Commands()) {
      if (const auto *arc = std::get_if<DrawArcCommand>(&command)) {
        result.push_back(*arc);
      }
    }
    return result;
  };

  TestPlatform platform;
  Runtime determinate{DeterminateProgressCircleApp, platform};
  determinate.SetViewport({64.0F, 64.0F});
  const DisplayList &initial = determinate.BuildFrame();
  const auto initial_arcs = arcs(initial);
  REQUIRE(initial_arcs.size() == 2);
  REQUIRE(std::abs(initial_arcs[0].sweep_angle - pi * 2.0F) < 0.001F);
  REQUIRE(initial_arcs[0].cap == StrokeCap::Butt);
  REQUIRE(std::abs(initial_arcs[1].sweep_angle - pi * 0.5F) < 0.001F);
  REQUIRE(initial_arcs[1].cap == StrokeCap::Round);

  const auto *root = determinate.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto *progress_node = root->children[0].get();
  REQUIRE(progress_node->kind == huxerui::detail::NodeKind::ProgressCircle);
  REQUIRE(progress_node->measured_size.width == 24.0F);
  REQUIRE(progress_node->measured_size.height == 24.0F);
  const std::uint64_t identity = progress_node->identity;

  progress_circle_value = 0.75F;
  const auto updated_arcs = arcs(determinate.BuildFrame());
  REQUIRE(updated_arcs.size() == 2);
  REQUIRE(std::abs(updated_arcs[1].sweep_angle - pi * 1.5F) < 0.001F);
  REQUIRE(determinate.RootNode()->children[0]->identity == identity);

  Runtime empty{EmptyProgressCircleApp, platform};
  empty.SetViewport({64.0F, 64.0F});
  REQUIRE(arcs(empty.BuildFrame()).size() == 1);

  Runtime full{FullProgressCircleApp, platform};
  full.SetViewport({64.0F, 64.0F});
  const auto full_arcs = arcs(full.BuildFrame());
  REQUIRE(full_arcs.size() == 2);
  REQUIRE(std::abs(full_arcs[1].sweep_angle - pi * 2.0F) < 0.001F);

  TestPlatform animated_platform;
  Runtime animated{IndeterminateProgressCircleApp, animated_platform};
  animated.SetViewport({64.0F, 64.0F});
  const int requests_before = animated_platform.requested_frames;
  const auto animated_initial = arcs(animated.BuildFrame());
  REQUIRE(animated_initial.size() == 2);
  REQUIRE(animated_platform.requested_frames > requests_before);
  const float initial_start = animated_initial[1].start_angle;

  animated_platform.AdvanceTime(0.48);
  const auto animated_next = arcs(animated.BuildFrame());
  REQUIRE(animated_next.size() == 2);
  REQUIRE(std::abs(animated_next[1].start_angle - initial_start) > 0.1F);

  TestPlatform reduced_platform;
  Runtime reduced{ReducedMotionProgressCircleApp, reduced_platform};
  reduced.SetViewport({64.0F, 64.0F});
  const int reduced_requests_before = reduced_platform.requested_frames;
  const auto reduced_arcs = arcs(reduced.BuildFrame());
  REQUIRE(reduced_arcs.size() == 2);
  REQUIRE(reduced_platform.requested_frames == reduced_requests_before);
}

TEST_CASE("TestThemeDrivesHoverAndPressedIndication") {
  TestPlatform platform;
  Runtime runtime{ThemedIndicationApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  const Color hover = Color::Rgb(20, 80, 160, 0.2F);
  const Color pressed = Color::Rgb(200, 40, 60, 0.3F);
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      101,
      {20.0F, 20.0F},
  });
  const DisplayList &hovered = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(hovered, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      101,
      {20.0F, 20.0F},
  });
  const DisplayList &down = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(down, pressed) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      101,
      {20.0F, 20.0F},
  });
  const DisplayList &released = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(released, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      101,
      {240.0F, 120.0F},
  });
  const DisplayList &outside = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(outside, hover) == nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      102,
      {20.0F, 20.0F},
      huxerui::PointerDeviceKind::Touch,
  });
  const DisplayList &touch_down = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(touch_down, pressed) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      102,
      {20.0F, 20.0F},
      huxerui::PointerDeviceKind::Touch,
  });
  const DisplayList &touch_released = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(touch_released, pressed) == nullptr);
  REQUIRE(FindRectWithColor(touch_released, hover) == nullptr);
}

TEST_CASE("TestEnabledInheritanceAndHitTestBlocking") {
  disabled_clicks = 0;
  underlying_clicks = 0;

  TestPlatform platform;
  Runtime overlay{DisabledHitTestApp, platform};
  overlay.SetViewport({200.0F, 80.0F});
  const DisplayList &display_list = overlay.BuildFrame();
  const DrawTextCommand *disabled = FindText(display_list, "disabled overlay");
  REQUIRE(disabled != nullptr);
  REQUIRE(std::abs(disabled->color.alpha - 0.42F) < 0.001F);

  const auto *overlay_root = overlay.RootNode();
  REQUIRE(overlay_root != nullptr);
  REQUIRE(overlay_root->children.size() == 2);
  REQUIRE(!overlay_root->children[1]->IsEnabled());
  ClickAt(overlay,
          {
              disabled->rect.x + disabled->rect.width * 0.5F,
              disabled->rect.y + disabled->rect.height * 0.5F,
          },
          102);
  REQUIRE(disabled_clicks == 0);
  REQUIRE(underlying_clicks == 0);

  Runtime subtree{DisabledSubtreeApp, platform};
  subtree.SetViewport({200.0F, 80.0F});
  const DisplayList &subtree_display = subtree.BuildFrame();
  const auto *subtree_root = subtree.RootNode();
  REQUIRE(subtree_root != nullptr);
  REQUIRE(!subtree_root->IsEnabled());
  REQUIRE(subtree_root->children.size() == 1);
  REQUIRE(!subtree_root->children[0]->IsEnabled());
  const DrawTextCommand *child = FindText(subtree_display, "disabled child");
  REQUIRE(child != nullptr);
  ClickAt(subtree,
          {
              child->rect.x + child->rect.width * 0.5F,
              child->rect.y + child->rect.height * 0.5F,
          },
          103);
  REQUIRE(disabled_clicks == 0);
}

TEST_CASE("TestFocusTraversalKeyboardAndThemeVisuals") {
  focus_changes.clear();
  received_keys.clear();
  first_keyboard_clicks = 0;
  third_keyboard_clicks = 0;
  custom_keyboard_clicks = 0;
  disabled_clicks = 0;

  TestPlatform platform;
  Runtime runtime{FocusApp, platform};
  runtime.SetViewport({240.0F, 180.0F});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const DisplayList &first_focused = runtime.BuildFrame();
  REQUIRE(focus_changes.size() == 1);
  REQUIRE(focus_changes.back() == "first:on");
  const DrawBorderCommand *first_border = FindBorderWithColor(first_focused, Color::Rgb(40, 180, 90));
  REQUIRE(first_border != nullptr);
  REQUIRE(first_border->width == 3.0F);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(first_keyboard_clicks == 1);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Enter,
  });

  first_focus_enabled = false;
  const DisplayList &disabled_first = runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "first:off");
  const DrawTextCommand *first_text = FindText(disabled_first, "first");
  REQUIRE(first_text != nullptr);
  REQUIRE(std::abs(first_text->color.alpha - 0.3F) < 0.001F);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "third:on");

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Space,
  });
  REQUIRE(third_keyboard_clicks == 0);
  const DisplayList &space_down = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(space_down, huxerui::FlatLightThemeSpec().interactions.pressed_overlay) != nullptr);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Space,
  });
  REQUIRE(third_keyboard_clicks == 1);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::ArrowRight,
  });
  REQUIRE(received_keys.size() == 1);
  REQUIRE(received_keys.front() == Key::ArrowRight);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(custom_keyboard_clicks == 1);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
      .modifiers =
          {
              .shift = true,
          },
  });
  runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "third:on");
}

TEST_CASE("TestPointerFocusDoesNotPaintFocusRing") {
  focus_changes.clear();

  TestPlatform platform;
  Runtime runtime{FocusApp, platform};
  runtime.SetViewport({240.0F, 180.0F});
  const DisplayList &initial = runtime.BuildFrame();
  const DrawTextCommand *first = FindText(initial, "first");
  REQUIRE(first != nullptr);
  const Point pointer{
      first->rect.x + first->rect.width * 0.5F,
      first->rect.y + first->rect.height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      104,
      pointer,
  });
  const DisplayList &pointer_focused = runtime.BuildFrame();
  REQUIRE(focus_changes.size() == 1);
  REQUIRE(focus_changes.back() == "first:on");
  REQUIRE(FindBorderWithColor(pointer_focused, Color::Rgb(40, 180, 90)) == nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      104,
      pointer,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const DisplayList &keyboard_focused = runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "third:on");
  REQUIRE(FindBorderWithColor(keyboard_focused, Color::Rgb(40, 180, 90)) != nullptr);
}

TEST_CASE("TestModalDialogTrapsAndRestoresFocusTraversal") {
  saved_dialogs.reset();
  background_dialog_clicks = 0;
  first_dialog_clicks = 0;
  second_dialog_clicks = 0;

  TestPlatform platform;
  Runtime runtime{FocusDialogApp, platform};
  runtime.SetViewport({240.0F, 160.0F});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.BuildFrame();

  const LayerId dialog = saved_dialogs->Show(
      [] {
        return Column{
            Button("first dialog focus").OnClick([] { ++first_dialog_clicks; }),
            Button("second dialog focus").OnClick([] { ++second_dialog_clicks; }),
        };
      },
      huxerui::DialogOptions{false});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(first_dialog_clicks == 1);
  REQUIRE(background_dialog_clicks == 0);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(second_dialog_clicks == 1);
  REQUIRE(background_dialog_clicks == 0);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(first_dialog_clicks == 2);

  REQUIRE(saved_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(background_dialog_clicks == 1);
}

TEST_CASE("TestRootHooksServicesAndLayers") {
  installed_root_service.reset();
  observed_root_service_value = 0;
  root_app_clicks = 0;

  huxerui::AppOptions options;
  options.root_hooks.push_back([](huxerui::RootContext &root) {
    installed_root_service = std::make_shared<TestRootService>(TestRootService{
        &root.Layers(),
        42,
    });
    root.Provide(installed_root_service);
  });

  TestPlatform platform;
  Runtime runtime{RootHookApp, platform, std::move(options)};
  runtime.SetViewport({200.0F, 100.0F});
  const DisplayList &initial = runtime.BuildFrame();
  REQUIRE(observed_root_service_value == 42);
  REQUIRE(ContainsText(initial, "application"));

  const LayerId toast = installed_root_service->layers->Attach(LayerKind::Toast, [] { return Text("toast"); });
  const DisplayList &with_toast = runtime.BuildFrame();
  REQUIRE(ContainsText(with_toast, "application"));
  REQUIRE(ContainsText(with_toast, "toast"));

  const LayerId modal = installed_root_service->layers->Attach(LayerKind::Modal, [] { return Text("modal"); });
  runtime.BuildFrame();
  ClickAt(runtime, {20.0F, 20.0F}, 82);
  REQUIRE(root_app_clicks == 0);

  REQUIRE(installed_root_service->layers->Dismiss(modal));
  runtime.BuildFrame();
  ClickAt(runtime, {20.0F, 20.0F}, 83);
  REQUIRE(root_app_clicks == 1);

  REQUIRE(installed_root_service->layers->Dismiss(toast));
  const DisplayList &dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "toast"));
}

TEST_CASE("TestToastAndDialogPresentation") {
  saved_toast.reset();
  saved_dialogs.reset();
  saved_dialog_context.reset();

  TestPlatform platform;
  Runtime runtime{PresentationThemeApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  REQUIRE(saved_toast.has_value());
  REQUIRE(saved_dialogs.has_value());

  saved_toast->Show("saved", huxerui::ToastOptions{0.5});
  const DisplayList &toast = runtime.BuildFrame();
  REQUIRE(ContainsText(toast, "saved"));
  const DrawRectCommand *toast_background = FindRectWithColor(toast, Color::Rgb(20, 30, 40, 0.9F));
  REQUIRE(toast_background != nullptr);
  REQUIRE(toast_background->rect.x == 65.0F);
  REQUIRE(toast_background->rect.y == 36.0F);
  REQUIRE(toast_background->rect.width == 70.0F);
  REQUIRE(toast_background->rect.height == 40.0F);
  const DrawTextCommand *toast_text = FindText(toast, "saved");
  REQUIRE(toast_text != nullptr);
  REQUIRE(toast_text->color.green == Color::Rgb(240, 245, 250).green);
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  const DisplayList &expired = runtime.BuildFrame();
  REQUIRE(!ContainsText(expired, "saved"));

  const LayerId dialog = saved_dialogs->Show([] { return Text("command dialog"); }, huxerui::DialogOptions{false});
  const DisplayList &shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "command dialog"));
  const DrawRectCommand *scrim = FindRect(shown, Rect{0.0F, 0.0F, 200.0F, 100.0F});
  REQUIRE(scrim != nullptr);
  REQUIRE(scrim->color.red == Color::Rgb(180, 20, 20, 0.3F).red);
  REQUIRE(scrim->color.alpha == 0.3F);
  REQUIRE(saved_dialogs->Dismiss(dialog));
  const DisplayList &dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "command dialog"));

  const LayerId contextual_dialog = saved_dialogs->Show(
      [](DialogContext dialog_context) {
        saved_dialog_context = dialog_context;
        return Text("context dialog");
      },
      huxerui::DialogOptions{false});
  const DisplayList &contextual = runtime.BuildFrame();
  REQUIRE(ContainsText(contextual, "context dialog"));
  REQUIRE(saved_dialog_context.has_value());
  REQUIRE(saved_dialog_context->Id() == contextual_dialog);

  saved_dialog_context.reset();
  REQUIRE(saved_dialogs->Update(contextual_dialog, [](DialogContext dialog_context) {
    saved_dialog_context = dialog_context;
    return Text("updated context dialog");
  }));
  const DisplayList &updated_contextual = runtime.BuildFrame();
  REQUIRE(ContainsText(updated_contextual, "updated context dialog"));
  REQUIRE(saved_dialog_context.has_value());
  REQUIRE(saved_dialog_context->Id() == contextual_dialog);
  REQUIRE(saved_dialog_context->Dismiss());
  const DisplayList &context_dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(context_dismissed, "updated context dialog"));

  const LayerId outside_dialog = saved_dialogs->Show([] { return Text("outside dismiss dialog"); });
  const DisplayList &outside_shown = runtime.BuildFrame();
  REQUIRE(ContainsText(outside_shown, "outside dismiss dialog"));
  ClickAt(runtime, {1.0F, 1.0F}, 85);
  const DisplayList &outside_dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(outside_dismissed, "outside dismiss dialog"));
  REQUIRE(!saved_dialogs->Dismiss(outside_dialog));
}

TEST_CASE("TestFlatDarkPresentationStyles") {
  saved_toast.reset();
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{FlatDarkPresentationApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  Color toast_background = dark.colors.on_surface;
  toast_background.alpha *= 0.94F;
  saved_toast->Show("dark toast", huxerui::ToastOptions{10.0});
  const DisplayList &toast = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(toast, toast_background) != nullptr);
  const DrawTextCommand *toast_text = FindText(toast, "dark toast");
  REQUIRE(toast_text != nullptr);
  REQUIRE(toast_text->color.red == dark.colors.surface.red);

  saved_dialogs->Show([] { return Text("dark dialog"); }, huxerui::DialogOptions{false});
  const DisplayList &dialog = runtime.BuildFrame();
  const DrawRectCommand *scrim = FindRect(dialog, Rect{0.0F, 0.0F, 200.0F, 100.0F});
  REQUIRE(scrim != nullptr);
  REQUIRE(scrim->color.alpha == dark.colors.scrim.alpha);
}

TEST_CASE("TestDeclarativeDialogModifier") {
  TestPlatform platform;
  Runtime runtime{DeclarativeDialogApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  const DisplayList &shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "declarative dialog"));

  ClickAt(runtime, {1.0F, 1.0F}, 84);
  REQUIRE(!declarative_dialog_visible);
  runtime.BuildFrame();
  const DisplayList &hidden = runtime.BuildFrame();
  REQUIRE(!ContainsText(hidden, "declarative dialog"));
}

TEST_CASE("TestAnimatedOffsetAndOpacityModifiers") {
  TestPlatform platform;
  Runtime runtime{AnimationApp, platform};
  runtime.SetViewport({240.0F, 100.0F});
  runtime.BuildFrame();

  animation_target = true;
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  const DisplayList &middle = runtime.BuildFrame();

  const DrawTextCommand *animated = nullptr;
  for (const auto &command : middle.Commands()) {
    if (const auto *text = std::get_if<DrawTextCommand>(&command); text && text->text == "animated") {
      animated = text;
      break;
    }
  }
  REQUIRE(animated != nullptr);
  REQUIRE(std::abs(animated->rect.x - 50.0F) < 0.01F);
  REQUIRE(std::abs(animated->color.alpha - 0.5F) < 0.01F);

  platform.AdvanceTime(0.5);
  const DisplayList &finished = runtime.BuildFrame();
  REQUIRE(!ContainsText(finished, "animated"));
}

TEST_CASE("TestClickIndicationUsesPointerObservation") {
  indication_clicks = 0;
  TestPlatform platform;
  Runtime runtime{IndicationApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      91,
      {20.0F, 20.0F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.04);
  const DisplayList &pressed = runtime.BuildFrame();

  std::size_t rectangles = 0;
  for (const auto &command : pressed.Commands()) {
    if (std::holds_alternative<DrawRectCommand>(command)) {
      ++rectangles;
    }
  }
  REQUIRE(rectangles >= 2);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      91,
      {20.0F, 20.0F},
  });
  REQUIRE(indication_clicks == 1);
}

TEST_CASE("TestModifierPresentationGeometry") {
  TestPlatform platform;
  Runtime runtime{PresentedIndicationApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto *button = root->children[0].get();
  REQUIRE(std::abs(button->PresentationFrame().x - 50.0F) < 0.01F);
  REQUIRE(std::abs(button->PresentationOpacity() - 0.5F) < 0.01F);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      92,
      {60.0F, 20.0F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.04);
  const DisplayList &pressed = runtime.BuildFrame();

  std::size_t presented_rectangles = 0;
  for (const auto &command : pressed.Commands()) {
    if (const auto *rectangle = std::get_if<DrawRectCommand>(&command);
        rectangle && std::abs(rectangle->rect.x - 50.0F) < 0.01F) {
      ++presented_rectangles;
      REQUIRE(rectangle->color.alpha <= 0.5F);
    }
  }
  REQUIRE(presented_rectangles >= 2);
}

TEST_CASE("TestExplicitIndicationOverridesAutomaticDefault") {
  indication_clicks = 0;
  TestPlatform platform;
  Runtime runtime{ExplicitIndicationApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->modifiers.size() == 1);
  REQUIRE(huxerui::detail::IsExplicitIndicationDescriptor(root->modifiers[0].descriptor));

  ClickAt(runtime, {20.0F, 20.0F}, 93);
  REQUIRE(indication_clicks == 1);
}

TEST_CASE("TestModifierFrameSubtreeCache") {
  TestPlatform platform;
  Runtime runtime{ModifierPruningApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->subtree_has_mounted_modifiers);
  REQUIRE(root->children.size() == 2);
  REQUIRE(!root->children[0]->subtree_has_mounted_modifiers);
  REQUIRE(root->children[1]->subtree_has_mounted_modifiers);

  show_modifier_branch = false;
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children.size() == 1);
  REQUIRE(!root->subtree_has_mounted_modifiers);
}

} // namespace huxerui::test
