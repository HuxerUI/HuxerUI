#include "runtime_test_support.h"

#include <limits>

#include "indication_internal.h"

namespace huxerui::test {

struct TestEnvironmentValue {
  std::string value;

  static TestEnvironmentValue Default() {
    return {"fallback"};
  }
};

std::vector<std::string> observed_environment_values;
State<bool> alternate_theme;
Color observed_theme_color;
Color observed_nested_theme_color;

struct TestRootService {
  LayerController* layers = nullptr;
  int value = 0;
};

std::shared_ptr<TestRootService> installed_root_service;
int observed_root_service_value = 0;
int root_app_clicks = 0;
std::optional<ToastHandle> saved_toast;
std::optional<DialogHandle> saved_dialogs;
std::optional<DialogContext> saved_dialog_context;
State<bool> declarative_dialog_visible;
State<int> declarative_dialog_value;
State<bool> declarative_dialog_motion_enabled;
State<bool> alternate_dialog_update_environment;
State<bool> animation_target;
State<bool> transform_animation_target;
int indication_clicks = 0;
int transformed_clicks = 0;
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
int positive_dialog_clicks = 0;
State<bool> checkbox_checked;
State<bool> switch_checked;
int checkbox_changes = 0;
int switch_changes = 0;
State<float> progress_circle_value;
State<float> progress_bar_value;
State<double> progress_bar_animation_duration;
State<float> slider_value;
int slider_changes = 0;
constexpr Color policy_dialog_background = Color::Rgb(32, 84, 132);
constexpr Color policy_dialog_separator = Color::Rgb(220, 120, 30);
constexpr Color policy_toast_background = Color::Rgb(42, 52, 62);
constexpr Color initial_dialog_update_theme = Color::Rgb(40, 100, 220);
constexpr Color updated_dialog_update_theme = Color::Rgb(220, 70, 50);
constexpr Color initial_dialog_update_scrim = Color::Rgb(20, 30, 40, 0.2F);
constexpr Color updated_dialog_update_scrim = Color::Rgb(80, 20, 100, 0.45F);

const detail::MountedNode* FindMountedText(const detail::MountedNode& node, std::string_view text) {
  if (node.text == text) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindMountedText(*child, text)) {
      return found;
    }
  }
  return nullptr;
}

bool PaintsText(const PaintSequence& sequence, std::string_view text) {
  return std::ranges::any_of(sequence.Commands(), [text](const PaintCommand& command) {
    const auto* draw_text = std::get_if<DrawTextCommand>(&command);
    return draw_text != nullptr && draw_text->text == text;
  });
}

std::vector<DrawRectCommand> DrawRectangles(const FlattenedScene& scene) {
  std::vector<DrawRectCommand> result;
  for (const auto& command : scene.Commands()) {
    if (const auto* rectangle = std::get_if<DrawRectCommand>(&command)) {
      result.push_back(*rectangle);
    }
  }
  return result;
}

std::optional<float>
RenderedTextOpacity(const RenderNode& node, std::string_view text, float inherited_opacity = 1.0F) {
  const float opacity = inherited_opacity * node.opacity;
  if (PaintsText(node.content, text) || PaintsText(node.foreground, text)) {
    return opacity;
  }
  for (const RenderNode* child : node.children) {
    if (child == nullptr) {
      continue;
    }
    if (const std::optional<float> found = RenderedTextOpacity(*child, text, opacity); found.has_value()) {
      return found;
    }
  }
  return std::nullopt;
}

View EnvironmentReader() {
  HUXERUI_SCOPE({
    observed_environment_values.push_back(UseEnvironment<TestEnvironmentValue>().value);
    return Text(UseEnvironment<TestEnvironmentValue>().value);
  });
}

View EnvironmentApp() {
  Environment outer;
  outer.Set(TestEnvironmentValue{"outer"});
  return Column {
    EnvironmentReader(),
    huxerui::ProvideEnvironment(std::move(outer), [] {
      return Column {
        EnvironmentReader(),
        huxerui::ProvideEnvironment(TestEnvironmentValue{"inner"}, EnvironmentReader),
      };
    }),
  };
}

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
          .corner_radius = 13.0F,
      }
  );
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
  return HUXERUI_THEME(
      TestButtonTheme,
      HUXERUI_THEME(
          huxerui::FlatDarkTheme,
          Column {
            Text("dark body"),
            Text("dark title", TextRole::Title),
            Button("dark button"),
          }
      )
  );
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
  return Row {
    Checkbox(checkbox).OnChanged([checkbox](bool checked) {
      ++checkbox_changes;
      checkbox = checked;
    }),
    Switch(switch_value).On<ToggleEvents::Changed>([switch_value](bool checked) {
      ++switch_changes;
      switch_value = checked;
    }),
  }.With(huxerui::Spacing{8.0F});
}

View DeterminateProgressCircleApp() {
  auto progress = UseState(0.25F);
  progress_circle_value = progress;
  return Row {
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

template <class Factory> View ReducedMotionProgressTheme(Factory&& content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  return Theme(ThemeDefinition{spec}, std::forward<Factory>(content));
}

View ReducedMotionProgressCircleApp() {
  return HUXERUI_THEME(ReducedMotionProgressTheme, ProgressCircle());
}

View DeterminateProgressBarApp() {
  auto progress = UseState(0.25F);
  progress_bar_value = progress;
  return Row {
    ProgressBar(progress),
  };
}

View IndeterminateProgressBarApp() {
  return Row {
    ProgressBar(),
  };
}

View EmptyProgressBarApp() {
  return Row {
    ProgressBar(-1.0F),
  };
}

View FullProgressBarApp() {
  return Row {
    ProgressBar(2.0F),
  };
}

View ReducedMotionProgressBarApp() {
  return HUXERUI_THEME(
      ReducedMotionProgressTheme,
      Row {
        ProgressBar(),
      }
  );
}

View AdjustableProgressBarApp() {
  auto duration = UseState(ProgressBarStyle::Default().animation_duration);
  progress_bar_animation_duration = duration;
  ProgressBarStyle style = ProgressBarStyle::Default();
  style.animation_duration = duration.Get();
  ThemeDefinition definition;
  definition.Set(style);
  return Theme(std::move(definition), [] {
    return Row {
      ProgressBar(),
    };
  });
}

View SliderApp() {
  auto value = UseState(4.0F);
  slider_value = value;
  return Row {
    Slider(value).Range(0.0F, 10.0F).Step(2.0F).OnChanged([value](float changed) {
      ++slider_changes;
      value = changed;
    }),
  };
}

View MaterialSliderApp() {
  return HUXERUI_THEME(
      huxerui::MaterialTheme,
      Row {
        Slider(4.0F).Range(0.0F, 10.0F).Step(2.0F),
      }
  );
}

View DisabledSliderApp() {
  return Slider(0.5F).OnChanged([](float) { ++slider_changes; }).With(Enabled{false});
}

template <class Factory> View InteractionTestTheme(Factory&& content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.hover_overlay = Color::Rgb(20, 80, 160, 0.2F);
  spec.interactions.pressed_overlay = Color::Rgb(200, 40, 60, 0.3F);
  return Theme(ThemeDefinition{spec}, std::forward<Factory>(content));
}

View ThemedIndicationApp() {
  return HUXERUI_THEME(InteractionTestTheme, Button("themed indication").OnClick([] {}));
}

template <class Factory> View FocusTestTheme(Factory&& content) {
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
    return Column {
      Button("first")
          .With(Enabled{first_focus_enabled})
          .OnClick([] { ++first_keyboard_clicks; })
          .On<ViewEvents::FocusChanged>([](bool focused) {
            focus_changes.push_back(focused ? "first:on" : "first:off");
          }),
      Button("disabled").With(Enabled{false}).OnClick([] { ++disabled_clicks; }),
      Button("third").OnClick([] { ++third_keyboard_clicks; }).On<ViewEvents::FocusChanged>([](bool focused) {
        focus_changes.push_back(focused ? "third:on" : "third:off");
      }),
      Text("custom focus")
          .With(Focusable{})
          .OnClick([] { ++custom_keyboard_clicks; })
          .On<ViewEvents::KeyDown>([](const KeyEvent& event) { received_keys.push_back(event.key); }),
    };
  });
}

View FocusApp() {
  return FocusTestTheme(FocusContent);
}

View DisabledHitTestApp() {
  return Stack {
    Button("underlying").OnClick([] { ++underlying_clicks; }),
    Button("disabled overlay").With(Enabled{false}).OnClick([] { ++disabled_clicks; }),
  };
}

View DisabledSubtreeApp() {
  return Column {
    Button("disabled child").With(Enabled{true}).OnClick([] { ++disabled_clicks; }),
  }.With(Enabled{false});
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
  definition.Set(
      huxerui::ToastStyle{
          .background = Color::Rgb(20, 30, 40, 0.9F),
          .text_style = TextStyle{Font::System(14.0F), Color::Rgb(240, 245, 250)},
          .padding = EdgeInsets::All(10.0F),
          .corner_radius = 9.0F,
      }
  );
  definition.Set(
      huxerui::DialogStyle{
          .scrim = Color::Rgb(180, 20, 20, 0.3F),
      }
  );
  return Theme(std::move(definition), PresentationApp);
}

View MaterialPresentationApp() {
  return huxerui::MaterialTheme(PresentationApp);
}

View DialogUpdateEnvironmentContent() {
  return Text(
      UseTheme().colors.primary == updated_dialog_update_theme ? "updated dialog environment"
                                                               : "initial dialog environment"
  );
}

View DialogUpdateEnvironmentApp() {
  alternate_dialog_update_environment = UseState(false);
  ThemeSpec theme = FlatLightThemeSpec();
  theme.colors.primary =
      alternate_dialog_update_environment ? updated_dialog_update_theme : initial_dialog_update_theme;

  DialogStyle dialog_style = DialogStyle::Default();
  dialog_style.scrim = alternate_dialog_update_environment ? updated_dialog_update_scrim : initial_dialog_update_scrim;
  dialog_style.motion.reset();

  ThemeDefinition definition{std::move(theme)};
  definition.Set(std::move(dialog_style));
  return Theme(std::move(definition), PresentationApp);
}

View ThemedPresentationPolicyApp() {
  DialogStyle dialog_style = DialogStyle::Default();
  dialog_style.background = policy_dialog_background;
  dialog_style.action_separator_color = policy_dialog_separator;
  dialog_style.action_separator_thickness = 2.0F;
  dialog_style.placement = VerticalPlacement::Bottom;
  dialog_style.action_layout = Axis::Vertical;
  dialog_style.action_alignment = HorizontalAlignment::Stretch;
  dialog_style.viewport_margin = 10.0F;
  dialog_style.motion.reset();

  ToastStyle toast_style = ToastStyle::Default();
  toast_style.background = policy_toast_background;
  toast_style.placement = VerticalPlacement::Top;
  toast_style.motion.reset();

  ThemeDefinition definition;
  definition.Set(std::move(dialog_style));
  definition.Set(std::move(toast_style));
  return Theme(std::move(definition), PresentationApp);
}

View FlatDarkPresentationApp() {
  return huxerui::FlatDarkTheme(PresentationApp);
}

View DeclarativeDialogApp() {
  declarative_dialog_visible = UseState(false);
  declarative_dialog_value = UseState(1);
  const std::string label = "declarative dialog " + std::to_string(declarative_dialog_value.Get());
  return Text("content").With(
      Dialog {
          .visible = declarative_dialog_visible,
          .content = [label] { return Text(label); },
          .dismiss_on_outside_press = true,
          .on_dismiss_request = [visible = declarative_dialog_visible] { visible = false; },
      }
  );
}

View DeclarativeDialogMotionApp() {
  declarative_dialog_visible = UseState(false);
  declarative_dialog_motion_enabled = UseState(false);

  DialogStyle style = DialogStyle::Default();
  if (!declarative_dialog_motion_enabled) {
    style.motion.reset();
  }
  ThemeDefinition definition;
  definition.Set(std::move(style));
  return Theme(std::move(definition), [] {
    return Text("content").With(
        Dialog {
            .visible = declarative_dialog_visible,
            .content = [] { return Text("motion dialog"); },
        }
    );
  });
}

View AnimationApp() {
  animation_target = UseState(false);
  const bool moved = animation_target.Get();
  return Text("animated")
      .With(
          Offset{AnimateTo(Point{moved ? 100.0F : 0.0F, 0.0F}, TweenSpec{1.0, Easing::Linear})},
          Opacity{AnimateTo(moved ? 0.0F : 1.0F, TweenSpec{1.0, Easing::Linear})}
      );
}

View TransformAnimationApp() {
  transform_animation_target = UseState(false);
  const bool transformed = transform_animation_target.Get();
  return Stack {
    Text("transform")
        .With(
            huxerui::Frame{80.0F, 40.0F},
            Scale{AnimateTo(transformed ? 2.0F : 1.0F, TweenSpec{1.0, Easing::Linear})},
            Rotation{AnimateTo(transformed ? 90.0F : 0.0F, TweenSpec{1.0, Easing::Linear})}
        ),
  };
}

View TransformedHitTestApp() {
  return Stack {
    Button("transformed").With(huxerui::Frame{80.0F, 40.0F}, Scale{1.5F}, Rotation{45.0F}).OnClick([] {
      ++transformed_clicks;
    }),
  };
}

View IndicationApp() {
  return Button("press").OnClick([] { ++indication_clicks; });
}

View PresentedIndicationApp() {
  return Stack {
    Button("presented").With(huxerui::Frame{80.0F, 40.0F}, Offset{Point{50.0F, 0.0F}}, Opacity{0.5F}).OnClick([] {}),
  };
}

View ExplicitIndicationApp() {
  return Button("explicit")
      .OnClick([] { ++indication_clicks; })
      .With(
          huxerui::Indication{
              huxerui::NoIndication{},
          }
      );
}

View NodeExtensionPruningApp() {
  auto visible = UseState(true);
  show_modifier_branch = visible;
  if (visible.Get()) {
    return Column {
      Text("plain"),
      Button("interactive").OnClick([] {}),
    };
  }
  return Column {
    Text("plain"),
  };
}

TEST_CASE("TestNestedEnvironment") {
  observed_environment_values.clear();

  TestPlatform platform;
  Runtime runtime{EnvironmentApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  REQUIRE(observed_environment_values.size() == 3);
  REQUIRE(observed_environment_values[0] == "fallback");
  REQUIRE(observed_environment_values[1] == "outer");
  REQUIRE(observed_environment_values[2] == "inner");
}

TEST_CASE("TestThemeProviderUpdatesNestedContent") {
  TestPlatform platform;
  Runtime runtime{ThemeApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
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
  REQUIRE(theme_button_background->color.blue == Color::Rgb(40, 100, 220).blue);

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
  REQUIRE(updated_button_background->color.red == Color::Rgb(220, 70, 50).red);
}

TEST_CASE("TestFlatDarkThemeAndSemanticTextRoles") {
  TestPlatform platform;
  Runtime runtime{FlatDarkThemeApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  const FlattenedScene& scene = runtime.BuildFrame();

  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  const DrawTextCommand* body = FindText(scene, "dark body");
  REQUIRE(body != nullptr);
  REQUIRE(body->style.foreground.red == dark.colors.on_surface.red);
  REQUIRE(body->style.font.Size() == dark.typography.body);

  const DrawTextCommand* title = FindText(scene, "dark title");
  REQUIRE(title != nullptr);
  REQUIRE(title->style.font.Size() == dark.typography.title);

  const DrawTextCommand* button = FindText(scene, "dark button");
  REQUIRE(button != nullptr);
  REQUIRE(button->style.foreground.red == dark.colors.on_primary.red);
  const DrawRectCommand* background = FindRect(scene, button->rect);
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

  const ThemeDefinition definition = huxerui::FlatThemeDefinition();
  const ToastStyle toast_style = ThemeDefinitionValue<ToastStyle>(definition);
  REQUIRE(toast_style.background.red == light.colors.inverse_surface.red);
  REQUIRE_FALSE(toast_style.motion.has_value());

  const DialogStyle dialog_style = ThemeDefinitionValue<DialogStyle>(definition);
  REQUIRE(dialog_style.background.red == light.colors.surface.red);
  REQUIRE(dialog_style.motion.has_value());
  REQUIRE(std::holds_alternative<StateOverlayIndication>(dialog_style.positive_action_indication));
  REQUIRE(std::holds_alternative<StateOverlayIndication>(dialog_style.negative_action_indication));

  const BottomSheetStyle bottom_sheet_style = ThemeDefinitionValue<BottomSheetStyle>(definition);
  REQUIRE(bottom_sheet_style.background.red == light.colors.surface.red);

  const MenuStyle menu_style = ThemeDefinitionValue<MenuStyle>(definition);
  REQUIRE(menu_style.separator_mode == MenuSeparatorMode::BetweenItems);
  REQUIRE_FALSE(menu_style.motion.has_value());
  REQUIRE(std::holds_alternative<StateOverlayIndication>(menu_style.item_indication));

  const ThemeDefinition dark_definition = huxerui::FlatDarkThemeDefinition();
  REQUIRE(ThemeDefinitionValue<DialogStyle>(dark_definition).background.red == dark.colors.surface.red);

  TestPlatform platform;
  Runtime runtime{FlatThemeInteractionApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
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
  REQUIRE(FindRectWithColor(hovered, light.interactions.hover_overlay) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      105,
      pointer,
  });
  const FlattenedScene& pressed = runtime.BuildFrame();
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
  const ButtonStyle button_style = ThemeDefinitionValue<ButtonStyle>(definition);
  REQUIRE(button_style.corner_radius == 20.0F);
  REQUIRE(button_style.padding.left == 24.0F);
  REQUIRE(button_style.padding.top == 10.0F);

  const CheckboxStyle checkbox_style = ThemeDefinitionValue<CheckboxStyle>(definition);
  REQUIRE(checkbox_style.size == 20.0F);
  REQUIRE(checkbox_style.corner_radius == 2.0F);
  REQUIRE(checkbox_style.checked_background.red == light.colors.primary.red);

  const SwitchStyle switch_style = ThemeDefinitionValue<SwitchStyle>(definition);
  REQUIRE(switch_style.width == 52.0F);
  REQUIRE(switch_style.height == 32.0F);
  REQUIRE(switch_style.thumb_radius == 12.0F);

  const ProgressCircleStyle progress_circle_style = ThemeDefinitionValue<ProgressCircleStyle>(definition);
  REQUIRE(progress_circle_style.size == 40.0F);
  REQUIRE(progress_circle_style.stroke_width == 4.0F);
  REQUIRE(progress_circle_style.indicator_color.red == light.colors.primary.red);

  const ProgressBarStyle progress_bar_style = ThemeDefinitionValue<ProgressBarStyle>(definition);
  REQUIRE(progress_bar_style.width == 160.0F);
  REQUIRE(progress_bar_style.height == 4.0F);
  REQUIRE(progress_bar_style.indicator_color.red == light.colors.primary.red);

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

  const huxerui::ToastStyle toast_style = ThemeDefinitionValue<huxerui::ToastStyle>(definition);
  REQUIRE(toast_style.background.red == Color::Rgb(50, 47, 53).red);

  const huxerui::DialogStyle dialog_style = ThemeDefinitionValue<huxerui::DialogStyle>(definition);
  REQUIRE(dialog_style.scrim.alpha == light.colors.scrim.alpha);
  REQUIRE(dialog_style.shadow.offset == Point{});
  REQUIRE(dialog_style.shadow.blur_radius == light.elevation.high * 4.0F);
  REQUIRE(dialog_style.shadow.spread == 0.0F);
  REQUIRE(dialog_style.motion.has_value());
  REQUIRE(dialog_style.motion->initial_scale == 0.94F);
  REQUIRE(std::get<TweenSpec>(dialog_style.motion->enter).duration == light.motion.normal);
  REQUIRE(std::get<TweenSpec>(dialog_style.motion->exit).duration == light.motion.fast);
  const auto* positive_indication = std::get_if<RippleIndication>(&dialog_style.positive_action_indication);
  REQUIRE(positive_indication != nullptr);
  REQUIRE(positive_indication->color.red == light.colors.primary.red);
  REQUIRE(positive_indication->color.alpha < light.colors.primary.alpha);

  const huxerui::BottomSheetStyle bottom_sheet_style = ThemeDefinitionValue<huxerui::BottomSheetStyle>(definition);
  REQUIRE(bottom_sheet_style.background.red == light.colors.surface.red);

  const huxerui::MenuStyle menu_style = ThemeDefinitionValue<huxerui::MenuStyle>(definition);
  REQUIRE(menu_style.separator_mode == huxerui::MenuSeparatorMode::None);
  REQUIRE(menu_style.motion.has_value());
  REQUIRE(std::holds_alternative<RippleIndication>(menu_style.item_indication));

  const huxerui::ScrollBarStyle scroll_bar_style = ThemeDefinitionValue<huxerui::ScrollBarStyle>(definition);
  REQUIRE(scroll_bar_style.thickness == 4.0F);
  REQUIRE(scroll_bar_style.corner_radius == 2.0F);

  ThemeSpec brand = light;
  brand.colors.primary = Color::Rgb(20, 110, 90);
  const ThemeDefinition brand_definition = huxerui::MaterialThemeDefinition(brand);
  const ButtonStyle brand_button_style = ThemeDefinitionValue<ButtonStyle>(brand_definition);
  REQUIRE(brand_button_style.background.green == brand.colors.primary.green);

  const ThemeDefinition dark_definition = huxerui::MaterialDarkThemeDefinition();
  REQUIRE(ThemeDefinitionValue<huxerui::DialogStyle>(dark_definition).background.red == dark.colors.surface.red);

  TestPlatform platform;
  Runtime runtime{MaterialThemeApp, platform};
  runtime.SetViewport({240.0F, 80.0F});
  const FlattenedScene& initial = runtime.BuildFrame();
  const DrawTextCommand* button = FindText(initial, "material button");
  REQUIRE(button != nullptr);
  REQUIRE(button->style.foreground.red == light.colors.on_primary.red);
  REQUIRE(button->style.font.Size() == light.typography.label);
  const DrawRectCommand* background = FindRect(initial, button->rect);
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
  const FlattenedScene& hovered = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(hovered, light.interactions.hover_overlay) != nullptr);

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
  dark_runtime.SetViewport({240.0F, 80.0F});
  const FlattenedScene& dark_display = dark_runtime.BuildFrame();
  const DrawTextCommand* dark_button = FindText(dark_display, "material dark button");
  REQUIRE(dark_button != nullptr);
  const DrawRectCommand* dark_background = FindRect(dark_display, dark_button->rect);
  REQUIRE(dark_background != nullptr);
  REQUIRE(dark_background->color.red == dark.colors.primary.red);
}

TEST_CASE("TestControlledTogglesAndAnimation") {
  checkbox_changes = 0;
  switch_changes = 0;

  TestPlatform platform;
  Runtime runtime{ToggleApp, platform};
  runtime.SetViewport({160.0F, 64.0F});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  const auto* checkbox = root->children[0].get();
  const auto* switch_node = root->children[1].get();
  REQUIRE(checkbox->kind == huxerui::detail::NodeKind::Checkbox);
  REQUIRE(switch_node->kind == huxerui::detail::NodeKind::Switch);
  REQUIRE(checkbox->focusable);
  REQUIRE(switch_node->focusable);
  REQUIRE(checkbox->measured_size.width == 20.0F);
  REQUIRE(switch_node->measured_size.width == 40.0F);

  const huxerui::DrawCircleCommand* initial_thumb = nullptr;
  for (const auto& command : initial.Commands()) {
    if (const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      initial_thumb = circle;
      break;
    }
  }
  REQUIRE(initial_thumb != nullptr);
  const float initial_thumb_x = initial_thumb->center.x;

  const std::uint64_t checkbox_identity = checkbox->identity;
  const Rect checkbox_bounds = checkbox->PresentationBounds();
  ClickAt(
      runtime,
      {
          checkbox_bounds.x + checkbox_bounds.width * 0.5F,
          checkbox_bounds.y + checkbox_bounds.height * 0.5F,
      }
  );
  const FlattenedScene& checked_display = runtime.BuildFrame();
  REQUIRE(checkbox_changes == 1);
  REQUIRE(checkbox_checked.Get());
  REQUIRE(FindText(checked_display, "✓") != nullptr);
  REQUIRE(runtime.RootNode()->children[0]->identity == checkbox_identity);

  switch_node = runtime.RootNode()->children[1].get();
  const Rect switch_bounds = switch_node->PresentationBounds();
  ClickAt(
      runtime,
      {
          switch_bounds.x + switch_bounds.width * 0.5F,
          switch_bounds.y + switch_bounds.height * 0.5F,
      }
  );
  const FlattenedScene& switch_start = runtime.BuildFrame();
  REQUIRE(switch_changes == 1);
  REQUIRE(switch_checked.Get());

  const huxerui::DrawCircleCommand* start_thumb = nullptr;
  for (const auto& command : switch_start.Commands()) {
    if (const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      start_thumb = circle;
      break;
    }
  }
  REQUIRE(start_thumb != nullptr);
  REQUIRE(std::abs(start_thumb->center.x - initial_thumb_x) < 0.001F);

  platform.AdvanceTime(0.1);
  const FlattenedScene& switch_middle = runtime.BuildFrame();
  const huxerui::DrawCircleCommand* middle_thumb = nullptr;
  for (const auto& command : switch_middle.Commands()) {
    if (const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      middle_thumb = circle;
      break;
    }
  }
  REQUIRE(middle_thumb != nullptr);
  REQUIRE(middle_thumb->center.x > initial_thumb_x);
  const float middle_thumb_x = middle_thumb->center.x;

  platform.AdvanceTime(0.2);
  const FlattenedScene& switch_end = runtime.BuildFrame();
  const huxerui::DrawCircleCommand* end_thumb = nullptr;
  for (const auto& command : switch_end.Commands()) {
    if (const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
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
  const auto arcs = [](const FlattenedScene& scene) {
    std::vector<DrawArcCommand> result;
    for (const auto& command : scene.Commands()) {
      if (const auto* arc = std::get_if<DrawArcCommand>(&command)) {
        result.push_back(*arc);
      }
    }
    return result;
  };

  TestPlatform platform;
  Runtime determinate{DeterminateProgressCircleApp, platform};
  determinate.SetViewport({64.0F, 64.0F});
  const FlattenedScene& initial = determinate.BuildFrame();
  const auto initial_arcs = arcs(initial);
  REQUIRE(initial_arcs.size() == 2);
  REQUIRE(std::abs(initial_arcs[0].sweep_angle - pi * 2.0F) < 0.001F);
  REQUIRE(initial_arcs[0].cap == StrokeCap::Butt);
  REQUIRE(std::abs(initial_arcs[1].sweep_angle - pi * 0.5F) < 0.001F);
  REQUIRE(initial_arcs[1].cap == StrokeCap::Round);

  const auto* root = determinate.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* progress_node = root->children[0].get();
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
  REQUIRE(animated_platform.requested_frames == requests_before);
  REQUIRE(animated.LastCommit().next_frame_deadline == animated_platform.current_time);
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
  REQUIRE_FALSE(reduced.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestProgressBarDrawingStateAndAnimation") {
  const ProgressBarStyle style = ProgressBarStyle::Default();
  const float indeterminate_width = style.width * style.indeterminate_fraction;

  TestPlatform platform;
  Runtime determinate{DeterminateProgressBarApp, platform};
  determinate.SetViewport({200.0F, 20.0F});
  const auto initial_rectangles = DrawRectangles(determinate.BuildFrame());
  REQUIRE(initial_rectangles.size() == 2);
  REQUIRE(initial_rectangles[0].rect.width == style.width);
  REQUIRE(initial_rectangles[0].rect.height == style.height);
  REQUIRE(initial_rectangles[0].corner_radius == style.corner_radius);
  REQUIRE(initial_rectangles[1].rect.width == style.width * 0.25F);

  const auto* root = determinate.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* progress_node = root->children[0].get();
  REQUIRE(progress_node->kind == huxerui::detail::NodeKind::ProgressBar);
  REQUIRE(progress_node->measured_size.width == style.width);
  REQUIRE(progress_node->measured_size.height == style.height);
  const std::uint64_t identity = progress_node->identity;

  progress_bar_value = 0.75F;
  const auto updated_rectangles = DrawRectangles(determinate.BuildFrame());
  REQUIRE(updated_rectangles.size() == 2);
  REQUIRE(updated_rectangles[1].rect.width == style.width * 0.75F);
  REQUIRE(determinate.RootNode()->children[0]->identity == identity);

  Runtime empty{EmptyProgressBarApp, platform};
  empty.SetViewport({200.0F, 20.0F});
  REQUIRE(DrawRectangles(empty.BuildFrame()).size() == 1);

  Runtime full{FullProgressBarApp, platform};
  full.SetViewport({200.0F, 20.0F});
  const auto full_rectangles = DrawRectangles(full.BuildFrame());
  REQUIRE(full_rectangles.size() == 2);
  REQUIRE(full_rectangles[1].rect.width == style.width);

  TestPlatform animated_platform;
  Runtime animated{IndeterminateProgressBarApp, animated_platform};
  animated.SetViewport({200.0F, 20.0F});
  const int requests_before = animated_platform.requested_frames;
  const auto animated_initial = DrawRectangles(animated.BuildFrame());
  REQUIRE(animated_initial.size() == 2);
  REQUIRE(animated_platform.requested_frames == requests_before);
  REQUIRE(animated.LastCommit().next_frame_deadline == animated_platform.current_time);
  const float initial_x = animated_initial[1].rect.x;
  const double animation_duration = style.animation_duration;

  animated_platform.AdvanceTime(animation_duration * 0.4);
  const auto animated_next = DrawRectangles(animated.BuildFrame());
  REQUIRE(animated_next.size() == 2);
  REQUIRE(animated_next[1].rect.x > initial_x);

  animated_platform.AdvanceTime(animation_duration * 0.59);
  const FlattenedScene& wrapped_scene = animated.BuildFrame();
  const auto wrapped = DrawRectangles(wrapped_scene);
  REQUIRE(wrapped.size() == 3);
  REQUIRE(wrapped[1].rect.width == indeterminate_width);
  REQUIRE(wrapped[2].rect.width == indeterminate_width);
  REQUIRE(std::abs(wrapped[2].rect.x - (wrapped[1].rect.x - style.width)) < 0.001F);
  REQUIRE(
      std::ranges::count_if(wrapped_scene.Commands(), [](const auto& command) {
        return std::holds_alternative<PushClipCommand>(command);
      }) == 1
  );
  REQUIRE(
      std::ranges::count_if(wrapped_scene.Commands(), [](const auto& command) {
        return std::holds_alternative<PopClipCommand>(command);
      }) == 1
  );

  animated_platform.AdvanceTime(animation_duration * 0.02);
  const auto after_wrap = DrawRectangles(animated.BuildFrame());
  REQUIRE(after_wrap.size() == 2);
  REQUIRE(after_wrap[1].rect.x > 0.0F);
  REQUIRE(after_wrap[1].rect.x < style.width * 0.025F);
  REQUIRE(after_wrap[1].rect.width == indeterminate_width);

  TestPlatform reduced_platform;
  Runtime reduced{ReducedMotionProgressBarApp, reduced_platform};
  reduced.SetViewport({200.0F, 20.0F});
  const int reduced_requests_before = reduced_platform.requested_frames;
  const auto reduced_rectangles = DrawRectangles(reduced.BuildFrame());
  REQUIRE(reduced_rectangles.size() == 2);
  REQUIRE(reduced_platform.requested_frames == reduced_requests_before);
  REQUIRE_FALSE(reduced.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestProgressBarStyleChangesSpeedWithoutResettingPhase") {
  const ProgressBarStyle style = ProgressBarStyle::Default();
  TestPlatform platform;
  Runtime runtime{AdjustableProgressBarApp, platform};
  runtime.SetViewport({200.0F, 20.0F});
  runtime.BuildFrame();

  platform.AdvanceTime(style.animation_duration * 0.25);
  const auto before_change = DrawRectangles(runtime.BuildFrame());
  REQUIRE(before_change.size() == 2);
  REQUIRE(std::abs(before_change[1].rect.x - style.width * 0.25F) < 0.001F);

  const double faster_duration = style.animation_duration * 0.5;
  progress_bar_animation_duration = faster_duration;
  const auto duration_changed = DrawRectangles(runtime.BuildFrame());
  REQUIRE(duration_changed.size() == 2);
  REQUIRE(std::abs(duration_changed[1].rect.x - before_change[1].rect.x) < 0.001F);

  platform.AdvanceTime(faster_duration * 0.1);
  const auto faster = DrawRectangles(runtime.BuildFrame());
  REQUIRE(faster.size() == 2);
  REQUIRE(std::abs(faster[1].rect.x - style.width * 0.35F) < 0.001F);
}

TEST_CASE("TestControlledSliderPointerKeyboardAndDrawing") {
  slider_changes = 0;
  const SliderStyle style = SliderStyle::Default();

  TestPlatform platform;
  Runtime runtime{SliderApp, platform};
  runtime.SetViewport({200.0F, 64.0F});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* slider = root->children[0].get();
  REQUIRE(slider->kind == huxerui::detail::NodeKind::Slider);
  REQUIRE(slider->focusable);
  REQUIRE(slider->measured_size.width == style.width);
  REQUIRE(slider->measured_size.height == style.height);

  const huxerui::DrawRectCommand* initial_thumb = nullptr;
  for (const PaintCommand& command : initial.Commands()) {
    if (const auto* rectangle = std::get_if<huxerui::DrawRectCommand>(&command)) {
      if (rectangle->color == style.thumb && rectangle->rect.width == style.thumb_width &&
          rectangle->rect.height == style.thumb_height) {
        initial_thumb = rectangle;
        break;
      }
    }
  }
  REQUIRE(initial_thumb != nullptr);
  const float initial_thumb_width = initial_thumb->rect.width;
  const float maximum_thumb_width = std::max({style.thumb_width, style.hovered_thumb_width, style.pressed_thumb_width});
  const float inset = maximum_thumb_width * 0.5F;
  const Rect bounds = slider->PresentationBounds();
  const float travel = bounds.width - inset * 2.0F;
  REQUIRE(
      std::abs(initial_thumb->rect.x + initial_thumb->rect.width * 0.5F - (bounds.x + inset + travel * 0.4F)) < 0.001F
  );

  const Point pointer{
      bounds.x + inset + travel * 0.74F,
      bounds.y + bounds.height * 0.5F,
  };
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Down,
      .pointer_id = 42,
      .position = pointer,
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(slider_changes == 1);
  REQUIRE(slider_value.Get() == 8.0F);
  runtime.BuildFrame();
  platform.AdvanceTime(style.animation_duration);
  const FlattenedScene& pressed = runtime.BuildFrame();
  const huxerui::DrawRectCommand* pressed_thumb = nullptr;
  for (const PaintCommand& command : pressed.Commands()) {
    if (const auto* rectangle = std::get_if<huxerui::DrawRectCommand>(&command)) {
      if (rectangle->color == style.thumb && rectangle->rect.height == style.pressed_thumb_height &&
          rectangle->rect.width > style.track_height) {
        pressed_thumb = rectangle;
        break;
      }
    }
  }
  REQUIRE(pressed_thumb != nullptr);
  REQUIRE(pressed_thumb->rect.width > initial_thumb_width);

  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Move,
      .pointer_id = 42,
      .position = {bounds.x + bounds.width + 20.0F, pointer.y},
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(slider_changes == 2);
  REQUIRE(slider_value.Get() == 10.0F);
  runtime.BuildFrame();
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Up,
      .pointer_id = 42,
      .position = {bounds.x + bounds.width - inset, pointer.y},
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(slider_changes == 2);

  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowLeft});
  REQUIRE(slider_changes == 3);
  REQUIRE(slider_value.Get() == 8.0F);
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowLeft, .repeat = true});
  REQUIRE(slider_changes == 4);
  REQUIRE(slider_value.Get() == 6.0F);
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Home});
  REQUIRE(slider_changes == 5);
  REQUIRE(slider_value.Get() == 0.0F);
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::End});
  REQUIRE(slider_changes == 6);
  REQUIRE(slider_value.Get() == 10.0F);

  const Point cancelled_pointer{
      bounds.x + inset + travel * 0.25F,
      pointer.y,
  };
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Down,
      .pointer_id = 84,
      .position = cancelled_pointer,
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(slider_changes == 7);
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Cancel,
      .pointer_id = 84,
      .position = cancelled_pointer,
      .device_kind = PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Move,
      .pointer_id = 84,
      .position = {bounds.x + bounds.width, pointer.y},
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(slider_changes == 7);
}

TEST_CASE("TestMaterialSliderUsesSplitTrackAndVerticalHandle") {
  const ThemeDefinition definition = huxerui::MaterialThemeDefinition();
  const SliderStyle style = ThemeDefinitionValue<SliderStyle>(definition);
  TestPlatform platform;
  Runtime runtime{MaterialSliderApp, platform};
  runtime.SetViewport({200.0F, 64.0F});
  const FlattenedScene& scene = runtime.BuildFrame();

  const huxerui::DrawRectCommand* thumb = nullptr;
  bool drew_inactive_track = false;
  int indicators = 0;
  for (const PaintCommand& command : scene.Commands()) {
    if (const auto* rectangle = std::get_if<huxerui::DrawRectCommand>(&command)) {
      if (rectangle->color == style.thumb && rectangle->rect.width == style.thumb_width &&
          rectangle->rect.height == style.thumb_height) {
        thumb = rectangle;
      }
    }
    if (const auto* fill = std::get_if<huxerui::FillPathCommand>(&command)) {
      if (fill->color == style.inactive_track && fill->path.Bounds().height == style.track_height) {
        drew_inactive_track = true;
      }
    }
    if (const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command)) {
      if (circle->radius == style.tick_size * 0.5F || circle->radius == style.stop_indicator_size * 0.5F) {
        ++indicators;
      }
    }
  }
  REQUIRE(thumb != nullptr);
  REQUIRE(thumb->corner_radius == style.thumb_width * 0.5F);
  REQUIRE(drew_inactive_track);
  REQUIRE(indicators >= 3);
}

TEST_CASE("TestDisabledSliderIgnoresPointerInput") {
  slider_changes = 0;
  TestPlatform platform;
  Runtime runtime{DisabledSliderApp, platform};
  runtime.SetViewport({200.0F, 64.0F});
  runtime.BuildFrame();

  const auto* slider = runtime.RootNode();
  REQUIRE(slider != nullptr);
  REQUIRE(slider->kind == huxerui::detail::NodeKind::Slider);
  REQUIRE_FALSE(slider->enabled);
  const Rect bounds = slider->PresentationBounds();
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Down,
      .pointer_id = 85,
      .position = {bounds.x + bounds.width * 0.75F, bounds.y + bounds.height * 0.5F},
      .device_kind = PointerDeviceKind::Mouse,
  });
  REQUIRE(slider_changes == 0);
}

TEST_CASE("TestSliderRejectsInvalidConfiguration") {
  REQUIRE_THROWS_AS(Slider(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
  REQUIRE_THROWS_AS(Slider(0.5F).Range(1.0F, 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(Slider(0.5F).Range(2.0F, 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(Slider(0.5F).Step(0.0F), std::invalid_argument);
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
  const FlattenedScene& hovered = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(hovered, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      101,
      {20.0F, 20.0F},
  });
  const FlattenedScene& down = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(down, pressed) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      101,
      {20.0F, 20.0F},
  });
  const FlattenedScene& released = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(released, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      101,
      {240.0F, 120.0F},
  });
  const FlattenedScene& outside = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(outside, hover) == nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      102,
      {20.0F, 20.0F},
      huxerui::PointerDeviceKind::Touch,
  });
  const FlattenedScene& touch_down = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(touch_down, pressed) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      102,
      {20.0F, 20.0F},
      huxerui::PointerDeviceKind::Touch,
  });
  const FlattenedScene& touch_released = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(touch_released, pressed) == nullptr);
  REQUIRE(FindRectWithColor(touch_released, hover) == nullptr);
}

TEST_CASE("TestEnabledInheritanceAndHitTestBlocking") {
  disabled_clicks = 0;
  underlying_clicks = 0;

  TestPlatform platform;
  Runtime overlay{DisabledHitTestApp, platform};
  overlay.SetViewport({200.0F, 80.0F});
  const FlattenedScene& scene = overlay.BuildFrame();
  const DrawTextCommand* disabled = FindText(scene, "disabled overlay");
  REQUIRE(disabled != nullptr);
  REQUIRE(disabled->style.foreground.alpha == 1.0F);

  const auto* overlay_root = overlay.RootNode();
  REQUIRE(overlay_root != nullptr);
  REQUIRE(overlay_root->children.size() == 2);
  REQUIRE(!overlay_root->children[1]->IsEnabled());
  REQUIRE(std::abs(overlay_root->children[1]->render_node.opacity - 0.42F) < 0.001F);
  ClickAt(
      overlay,
      {
          disabled->rect.x + disabled->rect.width * 0.5F,
          disabled->rect.y + disabled->rect.height * 0.5F,
      },
      102
  );
  REQUIRE(disabled_clicks == 0);
  REQUIRE(underlying_clicks == 0);

  Runtime subtree{DisabledSubtreeApp, platform};
  subtree.SetViewport({200.0F, 80.0F});
  const FlattenedScene& subtree_display = subtree.BuildFrame();
  const auto* subtree_root = subtree.RootNode();
  REQUIRE(subtree_root != nullptr);
  REQUIRE(!subtree_root->IsEnabled());
  REQUIRE(subtree_root->children.size() == 1);
  REQUIRE(!subtree_root->children[0]->IsEnabled());
  const DrawTextCommand* child = FindText(subtree_display, "disabled child");
  REQUIRE(child != nullptr);
  ClickAt(
      subtree,
      {
          child->rect.x + child->rect.width * 0.5F,
          child->rect.y + child->rect.height * 0.5F,
      },
      103
  );
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
  const FlattenedScene& first_focused = runtime.BuildFrame();
  REQUIRE(focus_changes.size() == 1);
  REQUIRE(focus_changes.back() == "first:on");
  const DrawBorderCommand* first_border = FindBorderWithColor(first_focused, Color::Rgb(40, 180, 90));
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
  const FlattenedScene& disabled_first = runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "first:off");
  const DrawTextCommand* first_text = FindText(disabled_first, "first");
  REQUIRE(first_text != nullptr);
  REQUIRE(first_text->style.foreground.alpha == 1.0F);
  const detail::MountedNode* first_node = FindMountedText(*runtime.RootNode(), "first");
  REQUIRE(first_node != nullptr);
  REQUIRE(std::abs(first_node->render_node.opacity - 0.3F) < 0.001F);

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
  const FlattenedScene& space_down = runtime.BuildFrame();
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
      .modifiers = {
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
  const FlattenedScene& initial = runtime.BuildFrame();
  const DrawTextCommand* first = FindText(initial, "first");
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
  const FlattenedScene& pointer_focused = runtime.BuildFrame();
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
      .key = Key::Control,
      .modifiers = {
          .control = true,
      },
  });
  const FlattenedScene& modifier_only = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(modifier_only, Color::Rgb(40, 180, 90)) == nullptr);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Unknown,
  });
  const FlattenedScene& unknown_key = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(unknown_key, Color::Rgb(40, 180, 90)) != nullptr);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const FlattenedScene& keyboard_focused = runtime.BuildFrame();
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
        return Column {
          Button("first dialog focus").OnClick([] { ++first_dialog_clicks; }),
          Button("second dialog focus").OnClick([] { ++second_dialog_clicks; }),
        };
      },
      huxerui::DialogOptions{.dismiss_on_outside_press = false}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);

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
  SettlePresentation(platform, runtime);
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
  int toast_compositions = 0;
  int modal_compositions = 0;

  huxerui::AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](huxerui::RootContext& root) {
    installed_root_service = std::make_shared<TestRootService>(TestRootService{
        &root.Layers(),
        42,
    });
    root.Provide(installed_root_service);
  });

  TestPlatform platform;
  Runtime runtime{RootHookApp, platform, std::move(options)};
  runtime.SetViewport({200.0F, 100.0F});
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(observed_root_service_value == 42);
  REQUIRE(ContainsText(initial, "application"));

  const LayerId toast = installed_root_service->layers->Attach(
      LayerOptions{
          .level = LayerLevel::Notification,
          .pointer_policy = LayerPointerPolicy::PassThrough,
      },
      [&toast_compositions] {
        ++toast_compositions;
        return Text("toast");
      }
  );
  const FlattenedScene& with_toast = runtime.BuildFrame();
  REQUIRE(toast_compositions == 1);
  REQUIRE(ContainsText(with_toast, "application"));
  REQUIRE(ContainsText(with_toast, "toast"));

  const LayerId modal = installed_root_service->layers->Attach(
      LayerOptions{
          .level = LayerLevel::Presentation,
          .pointer_policy = LayerPointerPolicy::Barrier,
          .trap_focus = true,
      },
      [&modal_compositions] {
        ++modal_compositions;
        return Text("modal");
      }
  );
  const FlattenedScene& with_modal = runtime.BuildFrame();
  REQUIRE(toast_compositions == 1);
  REQUIRE(modal_compositions == 1);
  std::vector<std::string> painted_text;
  for (const PaintCommand& command : with_modal.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      painted_text.push_back(text->text);
    }
  }
  const auto modal_position = std::ranges::find(painted_text, "modal");
  const auto toast_position = std::ranges::find(painted_text, "toast");
  REQUIRE(modal_position != painted_text.end());
  REQUIRE(toast_position != painted_text.end());
  REQUIRE(modal_position < toast_position);
  ClickAt(runtime, {20.0F, 20.0F}, 82);
  REQUIRE(root_app_clicks == 0);

  REQUIRE(installed_root_service->layers->Update(modal, [&modal_compositions] {
    ++modal_compositions;
    return Text("updated modal");
  }));
  const FlattenedScene& updated_modal = runtime.BuildFrame();
  REQUIRE(ContainsText(updated_modal, "updated modal"));
  REQUIRE(toast_compositions == 1);
  REQUIRE(modal_compositions == 2);

  REQUIRE(installed_root_service->layers->Dismiss(modal));
  runtime.BuildFrame();
  REQUIRE(toast_compositions == 1);
  ClickAt(runtime, {20.0F, 20.0F}, 83);
  REQUIRE(root_app_clicks == 1);

  REQUIRE(installed_root_service->layers->Dismiss(toast));
  const FlattenedScene& dismissed = runtime.BuildFrame();
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
  const FlattenedScene& toast = runtime.BuildFrame();
  REQUIRE(ContainsText(toast, "saved"));
  const DrawRectCommand* toast_background = FindRectWithColor(toast, Color::Rgb(20, 30, 40, 0.9F));
  REQUIRE(toast_background != nullptr);
  REQUIRE(toast_background->rect.width == 70.0F);
  REQUIRE(toast_background->rect.height == 40.0F);
  const std::optional<Rect> presented_toast_background =
      FindPresentedRectWithColor(toast, Color::Rgb(20, 30, 40, 0.9F));
  REQUIRE(presented_toast_background.has_value());
  REQUIRE(presented_toast_background->x == 65.0F);
  REQUIRE(presented_toast_background->y == 36.0F);
  const DrawTextCommand* toast_text = FindText(toast, "saved");
  REQUIRE(toast_text != nullptr);
  REQUIRE(toast_text->style.foreground.green == Color::Rgb(240, 245, 250).green);
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  const FlattenedScene& expired = runtime.BuildFrame();
  REQUIRE(!ContainsText(expired, "saved"));

  const LayerId dialog = saved_dialogs->Show(
      [] { return Text("command dialog"); },
      huxerui::DialogOptions{.dismiss_on_outside_press = false}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "command dialog"));
  const DrawRectCommand* scrim = FindRect(shown, Rect{0.0F, 0.0F, 200.0F, 100.0F});
  REQUIRE(scrim != nullptr);
  REQUIRE(scrim->color.red == Color::Rgb(180, 20, 20, 0.3F).red);
  REQUIRE(scrim->color.alpha == 0.3F);
  REQUIRE(saved_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "command dialog"));

  const LayerId contextual_dialog = saved_dialogs->Show(
      [](DialogContext dialog_context) {
        saved_dialog_context = dialog_context;
        return Text("context dialog");
      },
      huxerui::DialogOptions{.dismiss_on_outside_press = false}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& contextual = runtime.BuildFrame();
  REQUIRE(ContainsText(contextual, "context dialog"));
  REQUIRE(saved_dialog_context.has_value());
  REQUIRE(saved_dialog_context->Id() == contextual_dialog);

  saved_dialog_context.reset();
  REQUIRE(saved_dialogs->Update(contextual_dialog, [](DialogContext dialog_context) {
    saved_dialog_context = dialog_context;
    return Text("updated context dialog");
  }));
  const FlattenedScene& updated_contextual = runtime.BuildFrame();
  REQUIRE(ContainsText(updated_contextual, "updated context dialog"));
  REQUIRE(saved_dialog_context.has_value());
  REQUIRE(saved_dialog_context->Id() == contextual_dialog);
  REQUIRE(saved_dialog_context->Dismiss());
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& context_dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(context_dismissed, "updated context dialog"));

  const LayerId outside_dialog = saved_dialogs->Show([] { return Text("outside dismiss dialog"); });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& outside_shown = runtime.BuildFrame();
  REQUIRE(ContainsText(outside_shown, "outside dismiss dialog"));
  ClickAt(runtime, {1.0F, 1.0F}, 85);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& outside_dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(outside_dismissed, "outside dismiss dialog"));
  REQUIRE(!saved_dialogs->Dismiss(outside_dialog));
}

TEST_CASE("TestToastRejectsAnEmptyLiteralBeforeAttachingALayer") {
  saved_toast.reset();

  TestPlatform platform;
  Runtime runtime{PresentationApp, platform};
  runtime.SetViewport({240.0F, 160.0F});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(saved_toast->Show(""), std::invalid_argument);
  REQUIRE_NOTHROW(runtime.BuildFrame());
}

TEST_CASE("TestToastRetainsItsLayerUntilExitMotionCompletes") {
  saved_toast.reset();

  TestPlatform platform;
  Runtime runtime{MaterialPresentationApp, platform};
  runtime.SetViewport({240.0F, 160.0F});
  runtime.BuildFrame();

  const LayerId toast = saved_toast->Show("animated toast", ToastOptions{10.0});
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "animated toast"));

  REQUIRE(saved_toast->Dismiss(toast));
  REQUIRE(ContainsText(runtime.BuildFrame(), "animated toast"));
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "animated toast"));
}

TEST_CASE("TestCommandDialogUpdateRefreshesCapturedEnvironmentAndBarrier") {
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{DialogUpdateEnvironmentApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const LayerId dialog = saved_dialogs->Show(
      DialogUpdateEnvironmentContent,
      DialogOptions{
          .dismiss_on_outside_press = false,
      }
  );
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(ContainsText(initial, "initial dialog environment"));
  REQUIRE(FindRectWithColor(initial, initial_dialog_update_scrim) != nullptr);

  alternate_dialog_update_environment = true;
  runtime.BuildFrame();
  REQUIRE(saved_dialogs->Update(dialog, DialogUpdateEnvironmentContent));
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(!ContainsText(updated, "initial dialog environment"));
  REQUIRE(ContainsText(updated, "updated dialog environment"));
  REQUIRE(FindRectWithColor(updated, updated_dialog_update_scrim) != nullptr);

  ClickAt(runtime, {1.0F, 1.0F}, 146);
  REQUIRE(ContainsText(runtime.BuildFrame(), "updated dialog environment"));
}

TEST_CASE("TestStandardDialogUsesDefaultLabelsAndTwoActions") {
  saved_dialogs.reset();
  positive_dialog_clicks = 0;

  TestPlatform platform;
  Runtime runtime{PresentationThemeApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  saved_dialogs->Show("Save changes?", "The current document has unsaved changes.", {}, [] {
    ++positive_dialog_clicks;
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shortcut = runtime.BuildFrame();
  REQUIRE(ContainsText(shortcut, "Save changes?"));
  REQUIRE(ContainsText(shortcut, "The current document has unsaved changes."));
  const std::optional<Rect> positive = FindPresentedTextRect(shortcut, "OK");
  REQUIRE(positive.has_value());

  ClickAt(runtime, {positive->x + positive->width * 0.5F, positive->y + positive->height * 0.5F}, 141);
  REQUIRE(positive_dialog_clicks == 1);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "Save changes?"));

  saved_dialogs->Show("Remove item?", "This action cannot be undone.", "Remove", "Cancel");
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& two_actions = runtime.BuildFrame();
  REQUIRE(ContainsText(two_actions, "Remove item?"));
  REQUIRE(ContainsText(two_actions, "Cancel"));
  REQUIRE(ContainsText(two_actions, "Remove"));
  const std::optional<Rect> negative = FindPresentedTextRect(two_actions, "Cancel");
  REQUIRE(negative.has_value());
  ClickAt(runtime, {negative->x + negative->width * 0.5F, negative->y + negative->height * 0.5F}, 142);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "Remove item?"));
}

TEST_CASE("TestStandardDialogKeepsNaturalWidthAndRejectsEmptyLiteralContent") {
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{MaterialPresentationApp, platform};
  runtime.SetViewport({800.0F, 480.0F});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(saved_dialogs->Show("", "Message"), std::invalid_argument);
  REQUIRE_THROWS_AS(saved_dialogs->Show("Title", ""), std::invalid_argument);

  saved_dialogs->Show("Short", "Message");
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const DialogStyle style = ThemeDefinitionValue<DialogStyle>(MaterialThemeDefinition());
  const std::optional<Rect> surface = FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  REQUIRE(surface.has_value());
  REQUIRE(surface->width < style.maximum_width);
}

TEST_CASE("TestMaterialDialogActionUsesThemeRipple") {
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{MaterialPresentationApp, platform};
  runtime.SetViewport({640.0F, 360.0F});
  runtime.BuildFrame();

  saved_dialogs->Show("Save changes?", "The current document has unsaved changes.", "Save");
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  const std::optional<Rect> save = FindPresentedTextRect(shown, "Save");
  REQUIRE(save.has_value());

  const Point pointer{
      save->x + save->width * 0.5F,
      save->y + save->height * 0.5F,
  };
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      143,
      pointer,
  });
  runtime.BuildFrame();
  const ThemeSpec material = MaterialLightThemeSpec();
  platform.AdvanceTime(material.motion.slow * 0.5);
  const FlattenedScene& pressed = runtime.BuildFrame();

  const auto expected = std::get<RippleIndication>(
      ThemeDefinitionValue<DialogStyle>(MaterialThemeDefinition()).positive_action_indication
  );
  const DrawCircleCommand* ripple = nullptr;
  for (const auto& command : pressed.Commands()) {
    const auto* circle = std::get_if<DrawCircleCommand>(&command);
    if (circle && circle->radius > 0.0F && circle->color == expected.color) {
      ripple = circle;
      break;
    }
  }
  REQUIRE(ripple != nullptr);
}

TEST_CASE("TestPresentationThemeControlsDialogLayoutAndVerticalPlacement") {
  saved_toast.reset();
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{ThemedPresentationPolicyApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const LayerId toast_id = saved_toast->Show("top toast", ToastOptions{10.0});
  const FlattenedScene& toast = runtime.BuildFrame();
  const std::optional<Rect> toast_surface = FindPresentedRectWithColor(toast, policy_toast_background);
  REQUIRE(toast_surface.has_value());
  REQUIRE(toast_surface->y < 40.0F);
  REQUIRE(saved_toast->Dismiss(toast_id));

  const LayerId policy_dialog =
      saved_dialogs->Show("Policy dialog", "Theme-owned placement and actions", "Second", "First");
  const FlattenedScene& dialog = runtime.BuildFrame();
  const std::optional<Rect> dialog_surface = FindPresentedRectWithColor(dialog, policy_dialog_background);
  REQUIRE(dialog_surface.has_value());
  REQUIRE(dialog_surface->y + dialog_surface->height <= 230.0F);
  REQUIRE(dialog_surface->y + dialog_surface->height > 220.0F);
  REQUIRE(FindRectWithColor(dialog, policy_dialog_separator) != nullptr);

  const std::optional<Rect> first = FindPresentedTextRect(dialog, "First");
  const std::optional<Rect> second = FindPresentedTextRect(dialog, "Second");
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first->y < second->y);

  REQUIRE(saved_dialogs->Dismiss(policy_dialog));
  const LayerId custom_dialog = saved_dialogs->Show([] { return Text("before update"); });
  REQUIRE(ContainsText(runtime.BuildFrame(), "before update"));
  REQUIRE(saved_dialogs->Update(custom_dialog, [] { return Text("after update"); }));
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(!ContainsText(updated, "before update"));
  REQUIRE(ContainsText(updated, "after update"));
}

TEST_CASE("TestDialogRetainsExitPresentationWithoutRetainingInput") {
  saved_dialogs.reset();
  first_dialog_clicks = 0;

  TestPlatform platform;
  Runtime runtime{PresentationThemeApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  const LayerId dialog =
      saved_dialogs->Show([] { return Button("animated dialog").OnClick([] { ++first_dialog_clicks; }); });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "animated dialog"));

  REQUIRE(saved_dialogs->Dismiss(dialog));
  const FlattenedScene& exiting = runtime.BuildFrame();
  REQUIRE(ContainsText(exiting, "animated dialog"));
  ClickAt(runtime, {100.0F, 50.0F}, 133);
  REQUIRE(first_dialog_clicks == 0);

  REQUIRE(saved_dialogs->Update(dialog, [] { return Text("revived dialog"); }));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "revived dialog"));

  REQUIRE(saved_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "animated dialog"));
  REQUIRE(!ContainsText(runtime.BuildFrame(), "revived dialog"));
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
  const FlattenedScene& toast = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(toast, toast_background) != nullptr);
  const DrawTextCommand* toast_text = FindText(toast, "dark toast");
  REQUIRE(toast_text != nullptr);
  REQUIRE(toast_text->style.foreground.red == dark.colors.surface.red);

  saved_dialogs->Show(
      [] { return Text("dark dialog"); },
      huxerui::DialogOptions{.dismiss_on_outside_press = false}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& dialog = runtime.BuildFrame();
  const DrawRectCommand* scrim = FindRect(dialog, Rect{0.0F, 0.0F, 200.0F, 100.0F});
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
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "declarative dialog 1"));

  declarative_dialog_value = 2;
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(ContainsText(updated, "declarative dialog 2"));

  ClickAt(runtime, {1.0F, 1.0F}, 84);
  REQUIRE(!declarative_dialog_visible);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& hidden = runtime.BuildFrame();
  REQUIRE(!ContainsText(hidden, "declarative dialog 2"));

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "declarative dialog 2"));

  ClickAt(runtime, {1.0F, 1.0F}, 85);
  REQUIRE(!declarative_dialog_visible);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "declarative dialog 2"));

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "declarative dialog 2"));
}

TEST_CASE("TestDeclarativeDialogMotionStyleUpdatesWithoutReentering") {
  TestPlatform platform;
  Runtime runtime{DeclarativeDialogMotionApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const std::optional<float> unanimated_opacity =
      RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog");
  REQUIRE(unanimated_opacity.has_value());
  REQUIRE(*unanimated_opacity == Catch::Approx(1.0F));

  declarative_dialog_motion_enabled = true;
  runtime.BuildFrame();
  const std::optional<float> updated_opacity =
      RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog");
  REQUIRE(updated_opacity.has_value());
  REQUIRE(*updated_opacity == Catch::Approx(1.0F));

  declarative_dialog_visible = false;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE_FALSE(RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog").has_value());

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  runtime.BuildFrame();
  const std::optional<float> entering_opacity =
      RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog");
  REQUIRE(entering_opacity.has_value());
  REQUIRE(*entering_opacity < 1.0F);
}

TEST_CASE("TestDeclarativeDialogCanRemoveMotionWhileReentering") {
  TestPlatform platform;
  Runtime runtime{DeclarativeDialogMotionApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  declarative_dialog_motion_enabled = true;
  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "motion dialog"));

  declarative_dialog_visible = false;
  runtime.BuildFrame();
  platform.AdvanceTime(1.0);
  declarative_dialog_motion_enabled = false;
  declarative_dialog_visible = true;
  runtime.BuildFrame();
  REQUIRE(ContainsText(runtime.BuildFrame(), "motion dialog"));
}

TEST_CASE("TestAnimatedOffsetAndOpacityModifiers") {
  TestPlatform platform;
  Runtime runtime{AnimationApp, platform};
  runtime.SetViewport({240.0F, 100.0F});
  runtime.BuildFrame();

  animation_target = true;
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  const FlattenedScene& middle = runtime.BuildFrame();

  const DrawTextCommand* animated = nullptr;
  for (const auto& command : middle.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command); text && text->text == "animated") {
      animated = text;
      break;
    }
  }
  REQUIRE(animated != nullptr);
  const PushTransformCommand* transform = nullptr;
  for (const auto& command : middle.Commands()) {
    if (const auto* candidate = std::get_if<PushTransformCommand>(&command);
        candidate && std::abs(candidate->transform.translate_x - 50.0F) < 0.01F) {
      transform = candidate;
      break;
    }
  }
  REQUIRE(transform != nullptr);
  REQUIRE(animated->style.foreground.alpha == 1.0F);
  REQUIRE(std::abs(runtime.RootNode()->render_node.opacity - 0.5F) < 0.01F);

  platform.AdvanceTime(0.5);
  const FlattenedScene& finished = runtime.BuildFrame();
  REQUIRE(!ContainsText(finished, "animated"));
}

TEST_CASE("TestAnimatedScaleAndRotationModifiers") {
  TestPlatform platform;
  Runtime runtime{TransformAnimationApp, platform};
  runtime.SetViewport({240.0F, 200.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* content = root->children[0].get();
  REQUIRE(content->PresentationBounds().width == 80.0F);
  REQUIRE(content->PresentationBounds().height == 40.0F);

  transform_animation_target = true;
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  const FlattenedScene& middle = runtime.BuildFrame();

  root = runtime.RootNode();
  content = root->children[0].get();
  constexpr float middle_extent = 127.27922F;
  REQUIRE(std::abs(content->PresentationBounds().width - middle_extent) < 0.01F);
  REQUIRE(std::abs(content->PresentationBounds().height - middle_extent) < 0.01F);

  const PushTransformCommand* transform = nullptr;
  for (const auto& command : middle.Commands()) {
    if (const auto* candidate = std::get_if<PushTransformCommand>(&command);
        candidate && std::abs(candidate->transform.m12) > 0.01F) {
      transform = candidate;
      break;
    }
  }
  REQUIRE(transform != nullptr);
  REQUIRE(std::abs(transform->transform.m11 - 1.06066F) < 0.01F);
  REQUIRE(std::abs(transform->transform.m12 - 1.06066F) < 0.01F);

  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  root = runtime.RootNode();
  content = root->children[0].get();
  REQUIRE(std::abs(content->PresentationBounds().width - 80.0F) < 0.01F);
  REQUIRE(std::abs(content->PresentationBounds().height - 160.0F) < 0.01F);
}

TEST_CASE("TestTransformedControlUsesVisualHitRegion") {
  transformed_clicks = 0;

  TestPlatform platform;
  Runtime runtime{TransformedHitTestApp, platform};
  runtime.SetViewport({200.0F, 200.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* button = root->children[0].get();
  REQUIRE(button->Bounds().height == 40.0F);
  REQUIRE(std::abs(button->PresentationBounds().height - 127.27922F) < 0.01F);

  ClickAt(runtime, {1.0F, 39.0F}, 94);
  REQUIRE(transformed_clicks == 0);

  ClickAt(runtime, {80.0F, 60.0F}, 95);
  REQUIRE(transformed_clicks == 1);
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
  const FlattenedScene& pressed = runtime.BuildFrame();

  std::size_t rectangles = 0;
  for (const auto& command : pressed.Commands()) {
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

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* button = root->children[0].get();
  REQUIRE(std::abs(button->PresentationBounds().x - 50.0F) < 0.01F);
  REQUIRE(std::abs(button->PresentationOpacity() - 0.5F) < 0.01F);
  REQUIRE(std::abs(button->render_node.opacity - 0.5F) < 0.01F);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      92,
      {60.0F, 20.0F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.04);
  const FlattenedScene& pressed = runtime.BuildFrame();

  std::size_t presented_rectangles = 0;
  bool translated = false;
  for (const auto& command : pressed.Commands()) {
    if (const auto* transform = std::get_if<PushTransformCommand>(&command);
        transform && std::abs(transform->transform.translate_x - 50.0F) < 0.01F) {
      translated = true;
    }
    if (const auto* rectangle = std::get_if<DrawRectCommand>(&command);
        rectangle && std::abs(rectangle->rect.x) < 0.01F) {
      ++presented_rectangles;
    }
  }
  const DrawTextCommand* presented_text = FindText(pressed, "presented");
  REQUIRE(presented_text != nullptr);
  REQUIRE(presented_text->style.foreground.alpha == 1.0F);
  REQUIRE(translated);
  REQUIRE(presented_rectangles >= 2);
}

TEST_CASE("TestExplicitIndicationOverridesAutomaticDefault") {
  indication_clicks = 0;
  TestPlatform platform;
  Runtime runtime{ExplicitIndicationApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->extensions.size() == 1);
  REQUIRE(huxerui::detail::IsExplicitIndicationDescriptor(root->extensions[0].descriptor));

  ClickAt(runtime, {20.0F, 20.0F}, 93);
  REQUIRE(indication_clicks == 1);
}

TEST_CASE("TestNodeExtensionFrameSubtreeCache") {
  TestPlatform platform;
  Runtime runtime{NodeExtensionPruningApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->subtree_has_extensions);
  REQUIRE(root->children.size() == 2);
  REQUIRE(!root->children[0]->subtree_has_extensions);
  REQUIRE(root->children[1]->subtree_has_extensions);

  show_modifier_branch = false;
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children.size() == 1);
  REQUIRE(!root->subtree_has_extensions);
}

} // namespace huxerui::test
