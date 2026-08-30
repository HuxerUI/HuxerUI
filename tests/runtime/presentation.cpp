#include "runtime_test_support.h"

#include <limits>

#include "indication_internal.h"

namespace huxerui::test {

struct TestEnvironmentValue {
  std::string value;

  static TestEnvironmentValue Default() {
    return {"fallback"};
  }

  bool operator==(const TestEnvironmentValue&) const = default;
};

std::vector<std::string> observed_environment_values;
State<bool> alternate_theme;
State<bool> alternate_disabled_button_style;
Color observed_theme_color;
Color observed_nested_theme_color;

struct TestRootService {
  LayerController* layers = nullptr;
  int value = 0;
};

std::shared_ptr<TestRootService> installed_root_service;
int observed_root_service_value = 0;
int root_app_clicks = 0;
ViewportClass observed_layer_viewport_class = ViewportClass::Compact;
int layer_viewport_compositions = 0;
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
State<bool> radio_selected;
State<bool> switch_checked;
State<bool> labeled_checkbox_checked;
State<bool> labeled_radio_selected;
State<bool> labeled_switch_checked;
State<bool> chip_selected;
State<std::size_t> segmented_button_selection;
State<std::size_t> tabs_selection;
int checkbox_changes = 0;
int radio_changes = 0;
int switch_changes = 0;
int labeled_checkbox_changes = 0;
int labeled_radio_changes = 0;
int labeled_switch_changes = 0;
int action_chip_clicks = 0;
int icon_button_clicks = 0;
int selectable_chip_changes = 0;
int disabled_chip_changes = 0;
int segmented_button_changes = 0;
int disabled_segmented_button_changes = 0;
int rejected_segmented_button_changes = 0;
int tabs_changes = 0;
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

const detail::MountedNode* FindMountedKind(const detail::MountedNode& node, detail::NodeKind kind) {
  if (node.kind == kind) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const detail::MountedNode* found = FindMountedKind(*child, kind)) {
      return found;
    }
  }
  return nullptr;
}

const Color* FillColor(const std::optional<VisualFill>& fill) {
  return fill.has_value() ? std::get_if<Color>(&fill->Get()) : nullptr;
}

const Color* LayerFillColor(const std::optional<IndicationLayer>& layer) {
  return layer.has_value() ? FillColor(layer->fill) : nullptr;
}

const Indication* MountedIndication(const detail::MountedNode& node) {
  for (const detail::NodeExtensionEntry& entry : node.extensions) {
    if (detail::IsExplicitIndicationDescriptor(entry.descriptor)) {
      return static_cast<const Indication*>(entry.value.get());
    }
    if (detail::IsDefaultIndicationDescriptor(entry.descriptor)) {
      const auto* value = static_cast<const detail::DefaultIndication*>(entry.value.get());
      return value != nullptr && value->value.has_value() ? &*value->value : nullptr;
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

VectorAsset ControlIcon() {
  static const VectorAsset icon = VectorAsset::Create({12.0F, 12.0F}, [](VectorBuilder& builder) {
    builder.FillPath(
        Path{}.MoveTo({1.0F, 1.0F}).LineTo({11.0F, 6.0F}).LineTo({1.0F, 11.0F}).Close(),
        Color::Black()
    );
  });
  return icon;
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
    huxerui::ProvideEnvironment(
        std::move(outer),
        Column {
          EnvironmentReader(),
          huxerui::ProvideEnvironment(TestEnvironmentValue{"inner"}, EnvironmentReader()),
        }
    ),
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

View MaterialLabeledToggleApp() {
  auto checkbox = UseState(false);
  auto radio = UseState(false);
  auto switch_value = UseState(false);
  labeled_checkbox_checked = checkbox;
  labeled_radio_selected = radio;
  labeled_switch_checked = switch_value;
  return huxerui::MaterialTheme {
    Row {
      Checkbox("Checkbox label", checkbox).OnChanged([checkbox](bool checked) {
        ++labeled_checkbox_changes;
        checkbox = checked;
      }),
      RadioButton("Radio label", radio).OnChanged([radio](bool selected) {
        ++labeled_radio_changes;
        radio = selected;
      }),
      Switch("Switch label", switch_value).OnChanged([switch_value](bool checked) {
        ++labeled_switch_changes;
        switch_value = checked;
      }),
    }.With(Spacing(16.0F)),
  };
}

View MaterialPaddedLabeledToggleApp() {
  return huxerui::MaterialTheme {
    Checkbox("Padded label", false).With(Padding({.top = 3.0F, .right = 11.0F, .bottom = 5.0F, .left = 7.0F})),
  };
}

View MaterialChipApp() {
  return huxerui::MaterialTheme {
    Row {
      Chip("Action").OnClick([] {}),
      Chip("Selected", true).OnChanged([](bool) {}),
    }.With(Spacing(8.0F)),
  };
}

View MaterialSegmentedButtonApp() {
  return huxerui::MaterialTheme {
    Row {
      SegmentedButton({"Day", "Week", "Month"}, 1).OnChanged([](std::size_t) {}),
    },
  };
}

View MaterialTabsApp() {
  return huxerui::MaterialTheme {
    Tabs(
        std::vector<TabItem>{
            TabItem(ControlIcon(), "Overview"),
            TabItem::IconOnly(ControlIcon(), "Activity"),
            TabItem("Settings"),
        },
        1
    )
        .OnChanged([](std::size_t) {}),
  };
}

View MaterialIconControlsApp() {
  return huxerui::MaterialTheme {
    Row {
      Chip(ControlIcon(), "With icon", false).OnChanged([](bool) {}),
      SegmentedButton(
          std::vector<SegmentedButtonItem>{
              SegmentedButtonItem(ControlIcon(), "Mixed"),
              SegmentedButtonItem::IconOnly(ControlIcon(), "Icon only"),
          },
          1
      ).OnChanged([](std::size_t) {}),
    }.With(Spacing(12.0F)),
  };
}

View MaterialIconButtonApp() {
  return huxerui::MaterialTheme {
    Row {
      IconButton(ControlIcon(), "Play").OnClick([] { ++icon_button_clicks; }),
      IconButton(ControlIcon(), "Disabled play")
          .OnClick([] { ++icon_button_clicks; })
          .With(Enabled{false}),
    }.With(Spacing(8.0F)),
  };
}

View FlatIconButtonApp() {
  return Row {
    IconButton(ControlIcon(), "Flat play").OnClick([] {}),
  };
}

View MaterialControlledSwitchApp() {
  auto value = UseState(false);
  switch_checked = value;
  return huxerui::MaterialTheme {
    Switch(value).OnChanged([value](bool checked) { value = checked; }),
  };
}

View MaterialDarkThemeApp() {
  return huxerui::MaterialDarkTheme {Button("material dark button")};
}

View ToggleApp() {
  auto checkbox = UseState(false);
  auto radio = UseState(false);
  auto switch_value = UseState(false);
  checkbox_checked = checkbox;
  radio_selected = radio;
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
    RadioButton(radio).OnChanged([radio](bool selected) {
      ++radio_changes;
      radio = selected;
    }),
  }.With(huxerui::Spacing{8.0F});
}

View ChipApp() {
  auto selected = UseState(false);
  chip_selected = selected;
  return Row {
    Chip("Action").OnClick([] { ++action_chip_clicks; }),
    Chip("Selectable", selected).OnChanged([selected](bool value) {
      ++selectable_chip_changes;
      selected = value;
    }),
    Chip("Disabled", false)
        .OnChanged([](bool) { ++disabled_chip_changes; })
        .With(Enabled(false)),
  }.With(Spacing(8.0F));
}

View DividerApp() {
  return Column {
    Divider().With(Padding(EdgeInsets::Symmetric(8.0F, 0.0F))),
    Divider(Axis::Vertical).With(Frame{.height = 24.0F}),
  }.With(Frame{.width = 120.0F}, Spacing(4.0F));
}

View SegmentedButtonApp() {
  auto selected = UseState<std::size_t>(0);
  segmented_button_selection = selected;
  return Column {
    SegmentedButton({"Day", "Week", "Month"}, selected).OnChanged([selected](std::size_t index) {
      ++segmented_button_changes;
      selected = index;
    }),
    SegmentedButton({"One", "Two"}, 0)
        .On<SegmentedButtonEvents::Changed>([](std::size_t) { ++disabled_segmented_button_changes; })
        .With(Enabled(false)),
    SegmentedButton({"Keep", "Reject"}, 0).OnChanged([](std::size_t) { ++rejected_segmented_button_changes; }),
    SegmentedButton({"A", "B", "C"}, 0).OnChanged([](std::size_t) {}).With(Frame{.width = 0.5F}),
  }.With(Spacing(8.0F));
}

View TabsApp() {
  auto selected = UseState<std::size_t>(0);
  tabs_selection = selected;
  return Column{
      Tabs(
          std::vector<TabItem>{
              TabItem("Overview"),
              std::move(TabItem("Disabled")).Enabled(false),
              TabItem("Activity"),
              TabItem("Settings"),
          },
          selected
      )
          .OnChanged([selected](std::size_t index) {
            ++tabs_changes;
            selected = index;
          })
          .With(Frame{.width = 260.0F}),
  };
}

View DisabledRadioButtonApp() {
  return RadioButton(false).OnChanged([](bool) { ++radio_changes; }).With(Enabled(false));
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

View ReducedMotionProgressTheme(View content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  return Theme {ThemeDefinition{spec}, std::move(content)};
}

View ReducedMotionProgressCircleApp() {
  return ReducedMotionProgressTheme(ProgressCircle());
}

View MaterialDeterminateProgressCircleApp() {
  return huxerui::MaterialTheme {
    Row {
      ProgressCircle(0.25F),
    },
  };
}

View MaterialIndeterminateProgressCircleApp() {
  return huxerui::MaterialTheme {
    Row {
      ProgressCircle(),
    },
  };
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
  return ReducedMotionProgressTheme(Row {ProgressBar()});
}

View MaterialDeterminateProgressBarApp() {
  return huxerui::MaterialTheme {ProgressBar(0.25F)};
}

View MaterialIndeterminateProgressBarApp() {
  return huxerui::MaterialTheme {ProgressBar()};
}

View ReducedMotionMaterialTheme(View content) {
  ThemeSpec spec = huxerui::MaterialLightThemeSpec();
  spec.motion.reduced_motion = true;
  return Theme {huxerui::MaterialThemeDefinition(std::move(spec)), std::move(content)};
}

View ReducedMotionMaterialProgressBarApp() {
  return ReducedMotionMaterialTheme(ProgressBar());
}

View AdjustableProgressBarApp() {
  auto duration = UseState(ProgressBarStyle::Default().animation_duration);
  progress_bar_animation_duration = duration;
  ProgressBarStyle style = ProgressBarStyle::Default();
  style.animation_duration = duration.Get();
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    Row {
      ProgressBar(),
    },
  };
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
  return huxerui::MaterialTheme {
    Row {
      Slider(4.0F).Range(0.0F, 10.0F).Step(2.0F),
    },
  };
}

View DisabledSliderApp() {
  return huxerui::MaterialTheme {
    Slider(0.5F).OnChanged([](float) { ++slider_changes; }).With(Enabled{false}),
  };
}

View InteractionTestTheme(View content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.indication.hover = IndicationLayer{.fill = Color::Rgb(20, 80, 160, 0.2F)};
  spec.interactions.indication.press = IndicationLayer{.fill = Color::Rgb(200, 40, 60, 0.3F)};
  return Theme {ThemeDefinition{spec}, std::move(content)};
}

View ThemedIndicationApp() {
  return InteractionTestTheme(Button("themed indication").OnClick([] {}));
}

View FallbackIndicationApp() {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.indication.hover = IndicationLayer{.fill = Color::Rgb(20, 80, 160, 0.2F)};
  spec.interactions.indication.press.reset();
  return Theme {ThemeDefinition{spec}, Button("fallback indication").OnClick([] {})};
}

View SurfaceIndicationApp() {
  const Color normal_border = Color::Rgb(160, 40, 60);
  const Color pressed_border = Color::Rgb(20, 100, 200);
  return Button("surface indication")
      .OnClick([] {})
      .With(
          Frame{160.0F, 48.0F},
          Border{normal_border, 2.0F},
          CornerRadius{8.0F},
          Indication{
              .press = IndicationLayer{
                  .border = Border{pressed_border, 4.0F},
                  .corner_radii = CornerRadii{20.0F},
                  .enter = SnapSpec{},
                  .exit = SnapSpec{},
              },
          }
      );
}

View FocusTestTheme(View content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.focus_ring = FocusRing{Color::Rgb(40, 180, 90), 3.0F, 4.0F};
  spec.interactions.disabled_opacity = 0.3F;
  return Theme {ThemeDefinition{spec}, std::move(content)};
}

View FlatSliderFocusApp() {
  return FocusTestTheme(Slider(0.5F));
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
  return FocusTestTheme(FocusContent());
}

View FocusOnlyApp() {
  return FocusTestTheme(Text("focus only").With(Focusable{}));
}

View InvalidFocusRingApp() {
  ThemeSpec spec = FlatLightThemeSpec();
  spec.interactions.focus_ring.width = -1.0F;
  return Theme {ThemeDefinition{spec}, Text("invalid focus ring").With(Focusable{})};
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

View DisabledButtonStyleUpdateApp() {
  auto alternate = UseState(false);
  alternate_disabled_button_style = alternate;
  ButtonStyle style = ButtonStyle::Default();
  style.disabled_background = alternate ? Color::Rgb(180, 40, 60) : Color::Rgb(30, 80, 170);
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), Button("disabled style").With(Enabled{false})};
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
  return Theme {std::move(definition), PresentationApp()};
}

View MaterialPresentationApp() {
  return huxerui::MaterialTheme {PresentationApp()};
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
  return Theme {std::move(definition), PresentationApp()};
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
  return Theme {std::move(definition), PresentationApp()};
}

View FlatDarkPresentationApp() {
  return huxerui::FlatDarkTheme {PresentationApp()};
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
  return Theme {
    std::move(definition),
    Text("content").With(
        Dialog {
            .visible = declarative_dialog_visible,
            .content = [] { return Text("motion dialog"); },
        }
    ),
  };
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
  return Button("explicit").OnClick([] { ++indication_clicks; }).With(huxerui::Indication{});
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
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(observed_environment_values.size() == 3);
  REQUIRE(observed_environment_values[0] == "fallback");
  REQUIRE(observed_environment_values[1] == "outer");
  REQUIRE(observed_environment_values[2] == "inner");
}

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
  REQUIRE(background->color.blue == dark.colors.primary.blue);
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
  REQUIRE(toast_style.background.red == light.colors.inverse_surface.red);
  REQUIRE_FALSE(toast_style.motion.has_value());

  const TooltipStyle tooltip_style = ThemeDefinitionValue<TooltipStyle>(definition);
  REQUIRE(tooltip_style.background.red == light.colors.inverse_surface.red);
  REQUIRE(tooltip_style.background.alpha == light.colors.inverse_surface.alpha * 0.94F);
  REQUIRE(tooltip_style.text_style.foreground == light.colors.inverse_on_surface);
  REQUIRE(tooltip_style.maximum_width == 320.0F);
  REQUIRE(tooltip_style.shadow.blur_radius == light.elevation.low);

  const DialogStyle dialog_style = ThemeDefinitionValue<DialogStyle>(definition);
  REQUIRE(dialog_style.background.red == light.colors.surface.red);
  REQUIRE(dialog_style.motion.has_value());
  REQUIRE(dialog_style.positive_action_indication.press.has_value());
  REQUIRE(dialog_style.negative_action_indication.press.has_value());

  const BottomSheetStyle bottom_sheet_style = ThemeDefinitionValue<BottomSheetStyle>(definition);
  REQUIRE(bottom_sheet_style.background.red == light.colors.surface.red);

  const MenuStyle menu_style = ThemeDefinitionValue<MenuStyle>(definition);
  REQUIRE(menu_style.separator_mode == MenuSeparatorMode::BetweenItems);
  REQUIRE(menu_style.icon_tint == light.colors.on_surface);
  REQUIRE(menu_style.content_padding == EdgeInsets{});
  REQUIRE_FALSE(menu_style.motion.has_value());
  REQUIRE(menu_style.item_indication.press.has_value());

  const ThemeDefinition dark_definition = huxerui::FlatDarkThemeDefinition();
  REQUIRE(ThemeDefinitionValue<DialogStyle>(dark_definition).background.red == dark.colors.surface.red);

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
  REQUIRE(button_style.corner_radius == 20.0F);
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
  REQUIRE(checkbox_style.corner_radius == 2.0F);
  REQUIRE(checkbox_style.checked_background.red == light.colors.primary.red);

  const ChipStyle chip_style = ThemeDefinitionValue<ChipStyle>(definition);
  REQUIRE(chip_style.background == Color::Transparent());
  REQUIRE(chip_style.selected_background == light.colors.secondary_container);
  REQUIRE(chip_style.label_style.foreground == light.colors.on_surface_variant);
  REQUIRE(chip_style.selected_label == light.colors.on_secondary_container);
  REQUIRE(chip_style.icon_size == 18.0F);
  REQUIRE(chip_style.icon_spacing == light.spacing.small);
  REQUIRE(chip_style.minimum_height == 32.0F);
  REQUIRE(chip_style.corner_radius == light.shapes.small);
  REQUIRE(chip_style.border == light.colors.outline);
  REQUIRE(chip_style.indication.has_value());
  REQUIRE(chip_style.selected_indication.has_value());
  REQUIRE(chip_style.selected_indication->ripple.has_value());
  REQUIRE(chip_style.selected_indication->ripple->color.red == light.colors.on_secondary_container.red);

  const SegmentedButtonStyle segmented_button_style = ThemeDefinitionValue<SegmentedButtonStyle>(definition);
  REQUIRE(segmented_button_style.background == Color::Transparent());
  REQUIRE(segmented_button_style.selected_background == light.colors.secondary_container);
  REQUIRE(segmented_button_style.selected_label == light.colors.on_secondary_container);
  REQUIRE(segmented_button_style.icon_size == 18.0F);
  REQUIRE(segmented_button_style.icon_spacing == light.spacing.small);
  REQUIRE(segmented_button_style.minimum_height == 40.0F);
  REQUIRE(segmented_button_style.corner_radius == 20.0F);
  REQUIRE(segmented_button_style.border == light.colors.outline);
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
  REQUIRE(toast_style.background.red == Color::Rgb(50, 47, 53).red);

  const huxerui::TooltipStyle tooltip_style = ThemeDefinitionValue<huxerui::TooltipStyle>(definition);
  REQUIRE(tooltip_style.background == light.colors.inverse_surface);
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
  REQUIRE(bottom_sheet_style.background.red == light.colors.surface_container_low.red);

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
  REQUIRE(brand_button_style.background.green == brand.colors.primary.green);

  const ThemeDefinition dark_definition = huxerui::MaterialDarkThemeDefinition();
  REQUIRE(
      ThemeDefinitionValue<huxerui::DialogStyle>(dark_definition).background.red ==
      dark.colors.surface_container_high.red
  );

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
  REQUIRE(dark_background->color.red == dark.colors.primary.red);

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

TEST_CASE("TestMaterialSwitchStateLayerFollowsTheAnimatedThumb") {
  TestPlatform platform;
  Runtime runtime{MaterialControlledSwitchApp, platform};
  runtime.SetWindowMetrics({.viewport = {80.0F, 64.0F}});
  runtime.BuildFrame();

  const auto* switch_node = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Switch);
  REQUIRE(switch_node != nullptr);
  REQUIRE(switch_node->indication_bounds_override.has_value());
  const float initial_center =
      switch_node->indication_bounds_override->x + switch_node->indication_bounds_override->width * 0.5F;

  const Rect bounds = switch_node->PresentationBounds();
  ClickAt(runtime, {bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F}, 120);
  runtime.BuildFrame();
  platform.AdvanceTime(ThemeDefinitionValue<SwitchStyle>(MaterialThemeDefinition()).animation_duration * 0.5);
  runtime.BuildFrame();

  switch_node = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Switch);
  REQUIRE(switch_node != nullptr);
  REQUIRE(switch_node->indication_bounds_override.has_value());
  const float animated_center =
      switch_node->indication_bounds_override->x + switch_node->indication_bounds_override->width * 0.5F;
  REQUIRE(animated_center > initial_center);
  REQUIRE(animated_center < initial_center + 20.0F);
}

TEST_CASE("TestLabeledTogglesUseVisualSpacingAndOneActivationTarget") {
  labeled_checkbox_changes = 0;
  labeled_radio_changes = 0;
  labeled_switch_changes = 0;

  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{MaterialLabeledToggleApp, platform};
  runtime.SetWindowMetrics({.viewport = {560.0F, 64.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto* checkbox = FindMountedKind(*root, detail::NodeKind::Checkbox);
  const auto* radio = FindMountedKind(*root, detail::NodeKind::RadioButton);
  const auto* switch_node = FindMountedKind(*root, detail::NodeKind::Switch);
  REQUIRE(checkbox != nullptr);
  REQUIRE(radio != nullptr);
  REQUIRE(switch_node != nullptr);
  REQUIRE(checkbox->measured_size.width > 48.0F);
  REQUIRE(checkbox->measured_size.height == 48.0F);
  REQUIRE(radio->measured_size.width > 48.0F);
  REQUIRE(radio->measured_size.height == 48.0F);
  REQUIRE(switch_node->measured_size.width > 52.0F);
  REQUIRE(switch_node->measured_size.height == 48.0F);

  const auto checkbox_label = FindPresentedTextRect(scene, "Checkbox label");
  const auto radio_label = FindPresentedTextRect(scene, "Radio label");
  const auto switch_label = FindPresentedTextRect(scene, "Switch label");
  REQUIRE(checkbox_label.has_value());
  REQUIRE(radio_label.has_value());
  REQUIRE(switch_label.has_value());
  const ThemeSpec material = MaterialLightThemeSpec();
  const CheckboxStyle checkbox_style = ThemeDefinitionValue<CheckboxStyle>(MaterialThemeDefinition());
  const RadioButtonStyle radio_style = ThemeDefinitionValue<RadioButtonStyle>(MaterialThemeDefinition());
  const SwitchStyle switch_style = ThemeDefinitionValue<SwitchStyle>(MaterialThemeDefinition());
  const float checkbox_label_x = checkbox_style.size + material.spacing.small;
  const float radio_label_x = radio_style.size + material.spacing.small;
  const float checkbox_control_center_x = checkbox_style.size * 0.5F;
  REQUIRE(std::abs(checkbox_label->x - checkbox->PresentationBounds().x - checkbox_label_x) < 0.01F);
  REQUIRE(std::abs(radio_label->x - radio->PresentationBounds().x - radio_label_x) < 0.01F);
  REQUIRE(
      std::abs(
          switch_label->x - switch_node->PresentationBounds().x - switch_style.width - material.spacing.small
      ) < 0.01F
  );
  REQUIRE(checkbox->indication_bounds_override.has_value());
  REQUIRE(
      std::abs(
          checkbox->indication_bounds_override->x + checkbox->indication_bounds_override->width * 0.5F -
          checkbox_control_center_x
      ) < 0.01F
  );

  const Point checkbox_label_point{checkbox_label->x + checkbox_label->width * 0.5F, checkbox_label->y + 1.0F};
  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 123, checkbox_label_point});
  runtime.BuildFrame();
  platform.AdvanceTime(material.motion.slow * 0.5);
  const FlattenedScene& pressed = runtime.BuildFrame();
  const bool indication_centered_on_checkbox = std::ranges::any_of(
      pressed.Commands(),
      [checkbox_control_center_x](const PaintCommand& command) {
        const auto* circle = std::get_if<DrawCircleCommand>(&command);
        return circle != nullptr && circle->radius > 0.0F &&
               std::abs(circle->center.x - checkbox_control_center_x) < 0.01F &&
               std::abs(circle->center.y - 24.0F) < 0.01F;
      }
  );
  REQUIRE(indication_centered_on_checkbox);
  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Up, 123, checkbox_label_point});
  ClickAt(runtime, {radio_label->x + radio_label->width * 0.5F, radio_label->y + 1.0F}, 124);
  ClickAt(runtime, {switch_label->x + switch_label->width * 0.5F, switch_label->y + 1.0F}, 125);
  REQUIRE(labeled_checkbox_changes == 1);
  REQUIRE(labeled_radio_changes == 1);
  REQUIRE(labeled_switch_changes == 1);
  REQUIRE(labeled_checkbox_checked.Get());
  REQUIRE(labeled_radio_selected.Get());
  REQUIRE(labeled_switch_checked.Get());
}

TEST_CASE("TestLabeledToggleGeometryUsesContentBounds") {
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{MaterialPaddedLabeledToggleApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto* checkbox = FindMountedKind(*root, detail::NodeKind::Checkbox);
  REQUIRE(checkbox != nullptr);
  REQUIRE(checkbox->ContentBounds().x == 7.0F);
  REQUIRE(checkbox->ContentBounds().y == 3.0F);
  REQUIRE(checkbox->indication_bounds_override.has_value());

  const ThemeSpec material = MaterialLightThemeSpec();
  const CheckboxStyle style = ThemeDefinitionValue<CheckboxStyle>(MaterialThemeDefinition());
  const float expected_control_center = checkbox->ContentBounds().x + style.size * 0.5F;
  REQUIRE(
      std::abs(
          checkbox->indication_bounds_override->x + checkbox->indication_bounds_override->width * 0.5F -
          expected_control_center
      ) < 0.01F
  );

  const auto label = FindPresentedTextRect(scene, "Padded label");
  REQUIRE(label.has_value());
  const float expected_label_x =
      checkbox->PresentationBounds().x + checkbox->ContentBounds().x + style.size + material.spacing.small;
  REQUIRE(std::abs(label->x - expected_label_x) < 0.01F);
  REQUIRE(
      label->x + label->width <=
      checkbox->PresentationBounds().x + checkbox->ContentBounds().x + checkbox->ContentBounds().width + 0.01F
  );
}

TEST_CASE("TestControlledTogglesAndAnimation") {
  checkbox_changes = 0;
  radio_changes = 0;
  switch_changes = 0;

  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{ToggleApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 64.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 3);
  const auto* checkbox = root->children[0].get();
  const auto* switch_node = root->children[1].get();
  const auto* radio = root->children[2].get();
  REQUIRE(checkbox->kind == huxerui::detail::NodeKind::Checkbox);
  REQUIRE(switch_node->kind == huxerui::detail::NodeKind::Switch);
  REQUIRE(radio->kind == huxerui::detail::NodeKind::RadioButton);
  REQUIRE(checkbox->focusable);
  REQUIRE(switch_node->focusable);
  REQUIRE(radio->focusable);
  REQUIRE(checkbox->measured_size.width == 20.0F);
  REQUIRE(switch_node->measured_size.width == 40.0F);
  REQUIRE(radio->measured_size.width == 20.0F);

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
  REQUIRE(FindText(checked_display, "✓") == nullptr);
  REQUIRE(FindPresentedStrokePathRect(checked_display, CheckboxStyle::Default().checkmark).has_value());
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
      .modifiers = {
          .shift = true,
      },
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

  runtime.BuildFrame();
  radio = runtime.RootNode()->children[2].get();
  const std::uint64_t radio_identity = radio->identity;
  const Rect radio_bounds = radio->PresentationBounds();
  const Point radio_center{
      radio_bounds.x + radio_bounds.width * 0.5F,
      radio_bounds.y + radio_bounds.height * 0.5F,
  };
  ClickAt(runtime, radio_center);
  runtime.BuildFrame();
  REQUIRE(radio_changes == 1);
  REQUIRE(radio_selected.Get());
  REQUIRE(runtime.RootNode()->children[2]->identity == radio_identity);

  platform.AdvanceTime(RadioButtonStyle::Default().animation_duration);
  const FlattenedScene& selected_radio = runtime.BuildFrame();
  const bool paints_dot = std::ranges::any_of(
      selected_radio.Commands(),
      [&selected_radio](const PaintCommand& command) {
        const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
        if (circle == nullptr || std::abs(circle->radius - RadioButtonStyle::Default().dot_radius) >= 0.001F) {
          return false;
        }
        return std::ranges::any_of(selected_radio.Commands(), [circle](const PaintCommand& candidate) {
          const auto* arc = std::get_if<huxerui::DrawArcCommand>(&candidate);
          return arc != nullptr && arc->center == circle->center;
        });
      }
  );
  REQUIRE(paints_dot);

  ClickAt(runtime, radio_center);
  runtime.BuildFrame();
  REQUIRE(radio_changes == 1);
  REQUIRE(radio_selected.Get());
}

TEST_CASE("TestActionSelectableAndDisabledChips") {
  action_chip_clicks = 0;
  selectable_chip_changes = 0;
  disabled_chip_changes = 0;

  TestPlatform platform;
  Runtime runtime{ChipApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 64.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 3);
  const auto* action = root->children[0].get();
  const auto* selectable = root->children[1].get();
  const auto* disabled = root->children[2].get();
  REQUIRE(action->kind == detail::NodeKind::Chip);
  REQUIRE(selectable->kind == detail::NodeKind::Chip);
  REQUIRE(disabled->kind == detail::NodeKind::Chip);
  REQUIRE(action->focusable);
  REQUIRE(selectable->focusable);
  REQUIRE(disabled->focusable);
  REQUIRE(action->measured_size.height == ChipStyle::Default().minimum_height);

  const DrawTextCommand* initial_selectable = FindText(initial, "Selectable");
  REQUIRE(initial_selectable != nullptr);
  REQUIRE(initial_selectable->style.foreground == ChipStyle::Default().label_style.foreground);
  const DrawTextCommand* initial_disabled = FindText(initial, "Disabled");
  REQUIRE(initial_disabled != nullptr);
  REQUIRE(initial_disabled->style.foreground == ChipStyle::Default().disabled_label);

  const Rect action_bounds = action->PresentationBounds();
  ClickAt(runtime, {action_bounds.x + action_bounds.width * 0.5F, action_bounds.y + action_bounds.height * 0.5F});
  REQUIRE(action_chip_clicks == 1);

  const std::uint64_t selectable_identity = selectable->identity;
  const Rect selectable_bounds = selectable->PresentationBounds();
  ClickAt(
      runtime,
      {
          selectable_bounds.x + selectable_bounds.width * 0.5F,
          selectable_bounds.y + selectable_bounds.height * 0.5F,
      }
  );
  const FlattenedScene& selected = runtime.BuildFrame();
  REQUIRE(selectable_chip_changes == 1);
  REQUIRE(chip_selected.Get());
  REQUIRE(runtime.RootNode()->children[1]->identity == selectable_identity);
  REQUIRE(FindRectWithColor(selected, ChipStyle::Default().selected_background) != nullptr);
  const DrawTextCommand* selected_label = FindText(selected, "Selectable");
  REQUIRE(selected_label != nullptr);
  REQUIRE(selected_label->style.foreground == ChipStyle::Default().selected_label);

  disabled = runtime.RootNode()->children[2].get();
  const Rect disabled_bounds = disabled->PresentationBounds();
  ClickAt(
      runtime,
      {
          disabled_bounds.x + disabled_bounds.width * 0.5F,
          disabled_bounds.y + disabled_bounds.height * 0.5F,
      }
  );
  REQUIRE(disabled_chip_changes == 0);

  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Enter});
  REQUIRE(action_chip_clicks == 2);
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Space});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Up, .key = Key::Space});
  REQUIRE(selectable_chip_changes == 2);
  REQUIRE(!chip_selected.Get());
}

TEST_CASE("TestMaterialChipGeometryAndColors") {
  TestPlatform platform;
  Runtime runtime{MaterialChipApp, platform};
  runtime.SetWindowMetrics({.viewport = {260.0F, 64.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const ChipStyle style = ThemeDefinitionValue<ChipStyle>(MaterialThemeDefinition());
  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* row = root->children[0].get();
  REQUIRE(row->children.size() == 2);
  REQUIRE(row->children[0]->measured_size.height == style.minimum_height);
  REQUIRE(row->children[1]->measured_size.height == style.minimum_height);
  REQUIRE(MountedIndication(*row->children[1]) != nullptr);
  REQUIRE(*MountedIndication(*row->children[1]) == *style.selected_indication);
  REQUIRE(FindRectWithColor(scene, style.selected_background) != nullptr);
  const DrawTextCommand* action = FindText(scene, "Action");
  const DrawTextCommand* selected = FindText(scene, "Selected");
  REQUIRE(action != nullptr);
  REQUIRE(selected != nullptr);
  REQUIRE(action->style.foreground == style.label_style.foreground);
  REQUIRE(selected->style.foreground == style.selected_label);
  const bool paints_outline = std::ranges::any_of(scene.Commands(), [&style](const PaintCommand& command) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    return border != nullptr && border->color == style.border && border->style.width == style.border_width;
  });
  REQUIRE(paints_outline);
}

TEST_CASE("TestHorizontalAndVerticalDividerGeometry") {
  TestPlatform platform;
  Runtime runtime{DividerApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 40.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  const auto* horizontal = root->children[0].get();
  const auto* vertical = root->children[1].get();
  REQUIRE(horizontal->kind == detail::NodeKind::Divider);
  REQUIRE(vertical->kind == detail::NodeKind::Divider);
  REQUIRE(horizontal->measured_size == Size{120.0F, DividerStyle::Default().thickness});
  REQUIRE(horizontal->ContentBounds() == Rect{8.0F, 0.0F, 104.0F, DividerStyle::Default().thickness});
  REQUIRE(vertical->measured_size == Size{DividerStyle::Default().thickness, 24.0F});
  REQUIRE(vertical->layout_offset == Point{0.0F, 5.0F});

  const std::vector<DrawRectCommand> rectangles = DrawRectangles(scene);
  REQUIRE(rectangles.size() == 2);
  REQUIRE(rectangles[0].rect == Rect{8.0F, 0.0F, 104.0F, DividerStyle::Default().thickness});
  REQUIRE(rectangles[0].color == DividerStyle::Default().color);
  REQUIRE(rectangles[1].rect == Rect{0.0F, 0.0F, DividerStyle::Default().thickness, 24.0F});
  REQUIRE(rectangles[1].color == DividerStyle::Default().color);
}

TEST_CASE("TestSegmentedButtonSelectionLayoutAndKeyboard") {
  segmented_button_changes = 0;
  disabled_segmented_button_changes = 0;
  rejected_segmented_button_changes = 0;

  TestPlatform platform;
  Runtime runtime{SegmentedButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 240.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 4);
  const auto* group_scope = root->children[0].get();
  const auto* disabled_scope = root->children[1].get();
  const auto* rejected_scope = root->children[2].get();
  const auto* narrow_scope = root->children[3].get();
  REQUIRE(group_scope->children.size() == 1);
  REQUIRE(disabled_scope->children.size() == 1);
  REQUIRE(rejected_scope->children.size() == 1);
  REQUIRE(narrow_scope->children.size() == 1);
  const auto* group = group_scope->children[0].get();
  const auto* disabled_group = disabled_scope->children[0].get();
  const auto* rejected_group = rejected_scope->children[0].get();
  const auto* narrow_group = narrow_scope->children[0].get();
  REQUIRE(group->kind == detail::NodeKind::Layout);
  REQUIRE(group->focusable);
  REQUIRE(group->children.size() == 3);
  REQUIRE(group->children[0]->measured_size == group->children[1]->measured_size);
  REQUIRE(group->children[1]->measured_size == group->children[2]->measured_size);
  REQUIRE(group->children[0]->measured_size.height >= SegmentedButtonStyle::Default().minimum_height);
  REQUIRE(
      group->children[1]->layout_offset.x ==
      group->children[0]->measured_size.width - SegmentedButtonStyle::Default().border_width
  );
  REQUIRE(group->children[0]->properties.background == SegmentedButtonStyle::Default().selected_background);
  REQUIRE(group->children[1]->properties.background == SegmentedButtonStyle::Default().background);
  REQUIRE(disabled_scope->render_node.opacity == ThemeSpec::Default().interactions.disabled_opacity);
  REQUIRE(narrow_group->measured_size.width == 0.5F);
  REQUIRE(narrow_group->children.size() == 3);
  REQUIRE(narrow_group->children[1]->layout_offset.x >= narrow_group->children[0]->layout_offset.x);
  REQUIRE(narrow_group->children[2]->layout_offset.x >= narrow_group->children[1]->layout_offset.x);

  const DrawTextCommand* initial_day = FindText(initial, "Day");
  const DrawTextCommand* initial_week = FindText(initial, "Week");
  REQUIRE(initial_day != nullptr);
  REQUIRE(initial_week != nullptr);
  REQUIRE(initial_day->style.foreground == SegmentedButtonStyle::Default().selected_label);
  REQUIRE(initial_week->style.foreground == SegmentedButtonStyle::Default().label_style.foreground);

  const Rect month_bounds = group->children[2]->PresentationBounds();
  const Point month_center{
      month_bounds.x + month_bounds.width * 0.5F,
      month_bounds.y + month_bounds.height * 0.5F,
  };
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      120,
      month_center,
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Cancel,
      120,
      month_center,
  });
  REQUIRE(segmented_button_changes == 0);
  REQUIRE(segmented_button_selection.Get() == 0);

  const Rect initial_selected_bounds = group->children[0]->PresentationBounds();
  ClickAt(
      runtime,
      {
          initial_selected_bounds.x + initial_selected_bounds.width * 0.5F,
          initial_selected_bounds.y + initial_selected_bounds.height * 0.5F,
      }
  );
  REQUIRE(segmented_button_changes == 0);

  const std::uint64_t group_identity = group->identity;
  const Rect week_bounds = group->children[1]->PresentationBounds();
  ClickAt(
      runtime,
      {
          week_bounds.x + week_bounds.width * 0.5F,
          week_bounds.y + week_bounds.height * 0.5F,
      }
  );
  REQUIRE(segmented_button_changes == 1);
  REQUIRE(segmented_button_selection.Get() == 1);

  const FlattenedScene& selected_week = runtime.BuildFrame();
  group = runtime.RootNode()->children[0]->children[0].get();
  REQUIRE(group->identity == group_identity);
  REQUIRE(group->children[1]->properties.background == SegmentedButtonStyle::Default().selected_background);
  const DrawTextCommand* selected_week_label = FindText(selected_week, "Week");
  REQUIRE(selected_week_label != nullptr);
  REQUIRE(selected_week_label->style.foreground == SegmentedButtonStyle::Default().selected_label);

  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
  REQUIRE(segmented_button_changes == 2);
  REQUIRE(segmented_button_selection.Get() == 2);
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
  REQUIRE(segmented_button_changes == 3);
  REQUIRE(segmented_button_selection.Get() == 0);

  const Rect disabled_bounds = disabled_group->PresentationBounds();
  ClickAt(
      runtime,
      {
          disabled_bounds.x + disabled_bounds.width * 0.75F,
          disabled_bounds.y + disabled_bounds.height * 0.5F,
      }
  );
  REQUIRE(disabled_segmented_button_changes == 0);

  const Rect rejected_bounds = rejected_group->children[1]->PresentationBounds();
  const Point rejected_center{
      rejected_bounds.x + rejected_bounds.width * 0.5F,
      rejected_bounds.y + rejected_bounds.height * 0.5F,
  };
  ClickAt(runtime, rejected_center, 126);
  ClickAt(runtime, rejected_center, 127);
  REQUIRE(rejected_segmented_button_changes == 2);
}

TEST_CASE("TestMaterialSegmentedButtonStyleAndValidation") {
  REQUIRE_THROWS_AS(
      SegmentedButton(std::vector<StringVariant>{}, 0),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      SegmentedButton(std::vector<StringVariant>{"One", "Two"}, 2),
      std::invalid_argument
  );

  TestPlatform platform;
  Runtime runtime{MaterialSegmentedButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 64.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const SegmentedButtonStyle style = ThemeDefinitionValue<SegmentedButtonStyle>(MaterialThemeDefinition());
  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* row = root->children[0].get();
  REQUIRE(row->children.size() == 1);
  const auto* group_scope = row->children[0].get();
  REQUIRE(group_scope->children.size() == 1);
  const auto* group = group_scope->children[0].get();
  REQUIRE(group->children.size() == 3);
  REQUIRE(group->children[0]->measured_size.height == style.minimum_height);
  REQUIRE(FillColor(group->children[1]->properties.background) != nullptr);
  REQUIRE(*FillColor(group->children[1]->properties.background) == style.selected_background);
  REQUIRE(MountedIndication(*group->children[1]) != nullptr);
  REQUIRE(*MountedIndication(*group->children[1]) == *style.selected_indication);
  REQUIRE(group->children[0]->properties.border == Border{style.border, style.border_width});
  const DrawTextCommand* selected = FindText(scene, "Week");
  REQUIRE(selected != nullptr);
  REQUIRE(selected->style.foreground == style.selected_label);

  REQUIRE(style.indication.has_value());
  REQUIRE(style.indication->ripple.has_value());
  const RippleEffect& ripple_style = *style.indication->ripple;
  const Rect first_bounds = group->children[0]->PresentationBounds();
  const Point first_center{
      first_bounds.x + first_bounds.width * 0.5F,
      first_bounds.y + first_bounds.height * 0.5F,
  };
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      121,
      first_center,
  });
  runtime.BuildFrame();
  REQUIRE(group->children[0]->interaction.pressed);
  REQUIRE_FALSE(group->interaction.pressed);
  platform.AdvanceTime(std::get<TweenSpec>(ripple_style.expansion).duration * 0.5);
  const FlattenedScene& pressed = runtime.BuildFrame();
  const DrawCircleCommand* ripple = nullptr;
  for (const PaintCommand& command : pressed.Commands()) {
    const auto* circle = std::get_if<DrawCircleCommand>(&command);
    if (circle && circle->color.alpha > 0.0F) {
      ripple = circle;
      break;
    }
  }
  REQUIRE(ripple != nullptr);
  REQUIRE(ripple->radius > 0.0F);
  REQUIRE(ripple->color == ripple_style.color);
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Cancel,
      121,
      first_center,
  });
}

TEST_CASE("TestTabsSelectionOverflowAndKeyboard") {
  tabs_changes = 0;

  TestPlatform platform;
  Runtime runtime{TabsApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == detail::NodeKind::Layout);
  REQUIRE(root->children.size() == 1);
  const auto* tabs_scope = root->children[0].get();
  REQUIRE(tabs_scope->kind == detail::NodeKind::Scope);
  REQUIRE(tabs_scope->children.size() == 1);
  const auto* scroll = tabs_scope->children[0].get();
  REQUIRE(scroll->kind == detail::NodeKind::ScrollView);
  REQUIRE(scroll->measured_size.width == 260.0F);
  REQUIRE(scroll->children.size() == 1);
  const auto* tabs = scroll->children[0].get();
  REQUIRE(tabs->kind == detail::NodeKind::Layout);
  REQUIRE(tabs->focusable);
  REQUIRE(tabs->children.size() == 4);
  REQUIRE(tabs->measured_size.width > scroll->measured_size.width);
  REQUIRE(tabs->children[0]->measured_size.height >= TabsStyle::Default().minimum_height);
  REQUIRE_FALSE(tabs->children[1]->interaction.enabled);

  const detail::MountedNode* selected = FindMountedText(*tabs, "Overview");
  const detail::MountedNode* unselected = FindMountedText(*tabs, "Activity");
  REQUIRE(selected != nullptr);
  REQUIRE(unselected != nullptr);
  REQUIRE(selected->properties.text_style.foreground == TabsStyle::Default().selected_label);
  REQUIRE(unselected->properties.text_style.foreground == TabsStyle::Default().label_style.foreground);

  const Rect disabled_bounds = tabs->children[1]->PresentationBounds();
  ClickAt(
      runtime,
      {
          disabled_bounds.x + disabled_bounds.width * 0.5F,
          disabled_bounds.y + disabled_bounds.height * 0.5F,
      }
  );
  REQUIRE(tabs_changes == 0);
  REQUIRE(tabs_selection.Get() == 0);

  const Rect activity_bounds = tabs->children[2]->PresentationBounds();
  ClickAt(
      runtime,
      {
          activity_bounds.x + 8.0F,
          activity_bounds.y + activity_bounds.height * 0.5F,
      }
  );
  REQUIRE(tabs_changes == 1);
  REQUIRE(tabs_selection.Get() == 2);

  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
  REQUIRE(tabs_changes == 2);
  REQUIRE(tabs_selection.Get() == 3);
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
  REQUIRE(tabs_changes == 3);
  REQUIRE(tabs_selection.Get() == 0);
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::End});
  REQUIRE(tabs_changes == 4);
  REQUIRE(tabs_selection.Get() == 3);
  runtime.BuildFrame();
  runtime.BuildFrame();

  scroll = runtime.RootNode()->children[0]->children[0].get();
  REQUIRE(scroll->scroll_state->offset_x > 0.0F);
}

TEST_CASE("TestMaterialTabsStyleAndValidation") {
  REQUIRE_THROWS_AS(Tabs(std::vector<StringVariant>{}, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(Tabs(std::vector<StringVariant>{"One", "Two"}, 2), std::invalid_argument);
  REQUIRE_THROWS_AS(Tabs(std::vector<TabItem>{TabItem("")}, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(Tabs(std::vector<TabItem>{TabItem::IconOnly(ImageAsset{}, "Invalid")}, 0), std::invalid_argument);

  TestPlatform platform;
  Runtime runtime{MaterialTabsApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 80.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const TabsStyle style = ThemeDefinitionValue<TabsStyle>(MaterialThemeDefinition());
  REQUIRE(style.expand_items);
  REQUIRE(style.indication.has_value());
  REQUIRE(style.indicator_sizing == TabIndicatorSizing::Content);
  REQUIRE(style.indicator_min_width == 24.0F);
  REQUIRE(style.divider_height == 1.0F);
  REQUIRE(FindText(scene, "Activity") == nullptr);

  const auto* theme_scope = runtime.RootNode();
  REQUIRE(theme_scope != nullptr);
  REQUIRE(theme_scope->children.size() == 1);
  const auto* tabs_scope = theme_scope->children[0].get();
  REQUIRE(tabs_scope->children.size() == 1);
  const auto* scroll = tabs_scope->children[0].get();
  REQUIRE(scroll->children.size() == 1);
  const auto* tabs = scroll->children[0].get();
  REQUIRE(tabs->children.size() == 3);
  REQUIRE(tabs->children[0]->measured_size.width == tabs->children[1]->measured_size.width);
  REQUIRE(tabs->children[1]->measured_size.width == tabs->children[2]->measured_size.width);
  REQUIRE(tabs->children[0]->measured_size.height >= style.minimum_height);
  REQUIRE(tabs->children[0]->image_properties.HasValue());
  REQUIRE(tabs->children[1]->image_properties.HasValue());
  REQUIRE(tabs->children[1]->properties.text_style.foreground == style.selected_label);
  REQUIRE_FALSE(tabs->children[1]->LayoutValueOr<detail::LabelContentMetrics>({}).show_label);

  const DrawRectCommand* indicator = nullptr;
  const DrawRectCommand* divider = nullptr;
  const std::vector<DrawRectCommand> rectangles = DrawRectangles(scene);
  for (const DrawRectCommand& rectangle : rectangles) {
    if (rectangle.color == style.indicator && rectangle.rect.height == style.indicator_height) {
      indicator = &rectangle;
    }
    if (rectangle.color == style.divider_color && rectangle.rect.height == style.divider_height) {
      divider = &rectangle;
    }
  }
  REQUIRE(indicator != nullptr);
  REQUIRE(indicator->rect.width == style.indicator_min_width);
  REQUIRE(divider != nullptr);
  REQUIRE(divider->rect.width == tabs->measured_size.width);

  const TabsStyle flat_style = TabsStyle::Default();
  REQUIRE(flat_style.indicator_sizing == TabIndicatorSizing::Item);
  REQUIRE(flat_style.divider_height == 0.0F);

  TestPlatform overflow_platform;
  Runtime overflow{MaterialTabsApp, overflow_platform};
  overflow.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  const std::vector<DrawRectCommand> overflow_rectangles = DrawRectangles(overflow.BuildFrame());
  REQUIRE_FALSE(std::ranges::any_of(overflow_rectangles, [&style](const DrawRectCommand& rectangle) {
    return rectangle.color == style.divider_color && rectangle.rect.height == style.divider_height;
  }));
}

TEST_CASE("TestChipAndSegmentedButtonIconContent") {
  REQUIRE_THROWS_AS(Chip(ImageAsset{}, "Invalid"), std::invalid_argument);
  REQUIRE_THROWS_AS(
      SegmentedButton(
          std::vector<SegmentedButtonItem>{
              SegmentedButtonItem::IconOnly(ControlIcon(), ""),
          },
          0
      ),
      std::invalid_argument
  );

  TestPlatform platform;
  Runtime runtime{MaterialIconControlsApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 64.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* row = root->children[0].get();
  REQUIRE(row->children.size() == 2);
  const auto* chip = row->children[0].get();
  const auto* segments_scope = row->children[1].get();
  REQUIRE(segments_scope->children.size() == 1);
  const auto* segments = segments_scope->children[0].get();
  REQUIRE(chip->kind == detail::NodeKind::Chip);
  REQUIRE(chip->image_properties.HasValue());
  REQUIRE(segments->children.size() == 2);
  REQUIRE(segments->children[0]->image_properties.HasValue());
  REQUIRE(segments->children[1]->image_properties.HasValue());

  const ChipStyle chip_style = ThemeDefinitionValue<ChipStyle>(MaterialThemeDefinition());
  const detail::LabelContentMetrics chip_content = chip->LayoutValueOr<detail::LabelContentMetrics>({});
  REQUIRE(chip_content.icon_size == Size{chip_style.icon_size, chip_style.icon_size});
  REQUIRE(chip_content.icon_spacing == chip_style.icon_spacing);
  REQUIRE(chip_content.show_label);

  const SegmentedButtonStyle segmented_style =
      ThemeDefinitionValue<SegmentedButtonStyle>(MaterialThemeDefinition());
  const detail::LabelContentMetrics mixed_content =
      segments->children[0]->LayoutValueOr<detail::LabelContentMetrics>({});
  const detail::LabelContentMetrics icon_only_content =
      segments->children[1]->LayoutValueOr<detail::LabelContentMetrics>({});
  REQUIRE(mixed_content.icon_size == Size{segmented_style.icon_size, segmented_style.icon_size});
  REQUIRE(mixed_content.icon_spacing == segmented_style.icon_spacing);
  REQUIRE(mixed_content.show_label);
  REQUIRE_FALSE(icon_only_content.show_label);
  REQUIRE(FindText(scene, "With icon") != nullptr);
  REQUIRE(FindText(scene, "Mixed") != nullptr);
  REQUIRE(FindText(scene, "Icon only") == nullptr);
  REQUIRE(FindPresentedRectWithColor(scene, segmented_style.selected_label).has_value());
}

TEST_CASE("TestIconButtonGeometryInteractionAndValidation") {
  REQUIRE_THROWS_AS(IconButton(ImageAsset{}, "Invalid"), std::invalid_argument);
  REQUIRE_THROWS_AS(IconButton(ControlIcon(), " \t"), std::invalid_argument);
  icon_button_clicks = 0;

  TestPlatform platform;
  Runtime runtime{MaterialIconButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 64.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* row = root->children[0].get();
  REQUIRE(row->children.size() == 2);
  const auto* active = row->children[0].get();
  const auto* disabled = row->children[1].get();
  REQUIRE(active->kind == detail::NodeKind::IconButton);
  REQUIRE(disabled->kind == detail::NodeKind::IconButton);
  REQUIRE(active->focusable);
  REQUIRE(active->measured_size == Size{48.0F, 48.0F});
  REQUIRE(disabled->measured_size == Size{48.0F, 48.0F});
  REQUIRE(MountedIndication(*active) != nullptr);
  REQUIRE(MountedIndication(*active)->geometry.layer_size == Size{40.0F, 40.0F});
  REQUIRE(MountedIndication(*active)->geometry.clip_corner_radii == CornerRadii{20.0F});
  const detail::LabelContentMetrics content = active->LayoutValueOr<detail::LabelContentMetrics>({});
  REQUIRE(content.icon_size == Size{24.0F, 24.0F});
  REQUIRE_FALSE(content.show_label);
  REQUIRE(FindText(scene, "Play") == nullptr);
  REQUIRE(FindText(scene, "Disabled play") == nullptr);

  const IconButtonStyle style = ThemeDefinitionValue<IconButtonStyle>(MaterialThemeDefinition());
  REQUIRE(FindPresentedRectWithColor(scene, style.foreground).has_value());
  REQUIRE(FindPresentedRectWithColor(scene, style.disabled_foreground).has_value());

  const Rect active_bounds = active->PresentationBounds();
  ClickAt(runtime, {active_bounds.x + active_bounds.width * 0.5F, active_bounds.y + active_bounds.height * 0.5F});
  REQUIRE(icon_button_clicks == 1);
  const Rect disabled_bounds = disabled->PresentationBounds();
  ClickAt(
      runtime,
      {disabled_bounds.x + disabled_bounds.width * 0.5F, disabled_bounds.y + disabled_bounds.height * 0.5F}
  );
  REQUIRE(icon_button_clicks == 1);

  TestPlatform flat_platform;
  Runtime flat_runtime{FlatIconButtonApp, flat_platform};
  flat_runtime.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  flat_runtime.BuildFrame();
  const auto* flat_root = flat_runtime.RootNode();
  REQUIRE(flat_root != nullptr);
  REQUIRE(flat_root->children.size() == 1);
  const auto* flat = flat_root->children[0].get();
  REQUIRE(flat->kind == detail::NodeKind::IconButton);
  REQUIRE(flat->measured_size == Size{40.0F, 40.0F});
  REQUIRE(MountedIndication(*flat) != nullptr);
  REQUIRE(MountedIndication(*flat)->geometry.layer_size == Size{32.0F, 32.0F});
  REQUIRE(MountedIndication(*flat)->geometry.clip_corner_radii == CornerRadii{4.0F});
  REQUIRE(flat->properties.corner_radii == CornerRadii{4.0F});
  const detail::LabelContentMetrics flat_content = flat->LayoutValueOr<detail::LabelContentMetrics>({});
  REQUIRE(flat_content.icon_size == Size{20.0F, 20.0F});
}

TEST_CASE("TestDisabledRadioButtonDoesNotSelect") {
  radio_changes = 0;
  TestPlatform platform;
  Runtime runtime{DisabledRadioButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  runtime.BuildFrame();
  const auto* radio = runtime.RootNode();
  REQUIRE(radio != nullptr);
  REQUIRE(radio->kind == huxerui::detail::NodeKind::RadioButton);
  const Rect bounds = radio->PresentationBounds();
  ClickAt(runtime, {bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F});
  REQUIRE(radio_changes == 0);
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
  platform.platform_resources = BuiltinTestResources();
  Runtime determinate{DeterminateProgressCircleApp, platform};
  determinate.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  const FlattenedScene& initial = determinate.BuildFrame();
  const auto initial_arcs = arcs(initial);
  REQUIRE(initial_arcs.size() == 2);
  REQUIRE(std::abs(initial_arcs[0].sweep_angle - pi * 2.0F) < 0.001F);
  REQUIRE(initial_arcs[0].style.cap == StrokeCap::Butt);
  REQUIRE(std::abs(initial_arcs[1].sweep_angle - pi * 0.5F) < 0.001F);
  REQUIRE(initial_arcs[1].style.cap == StrokeCap::Round);

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
  empty.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  REQUIRE(arcs(empty.BuildFrame()).size() == 1);

  Runtime full{FullProgressCircleApp, platform};
  full.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  const auto full_arcs = arcs(full.BuildFrame());
  REQUIRE(full_arcs.size() == 2);
  REQUIRE(std::abs(full_arcs[1].sweep_angle - pi * 2.0F) < 0.001F);

  TestPlatform animated_platform;
  animated_platform.platform_resources = BuiltinTestResources();
  Runtime animated{IndeterminateProgressCircleApp, animated_platform};
  animated.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
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
  reduced_platform.platform_resources = BuiltinTestResources();
  Runtime reduced{ReducedMotionProgressCircleApp, reduced_platform};
  reduced.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  const int reduced_requests_before = reduced_platform.requested_frames;
  const auto reduced_arcs = arcs(reduced.BuildFrame());
  REQUIRE(reduced_arcs.size() == 2);
  REQUIRE(reduced_platform.requested_frames == reduced_requests_before);
  REQUIRE_FALSE(reduced.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestMaterialProgressCircleUsesVisibleGapAndPulsingArcMotion") {
  constexpr float pi = 3.14159265358979323846F;
  const auto arcs = [](const FlattenedScene& scene) {
    std::vector<DrawArcCommand> result;
    for (const PaintCommand& command : scene.Commands()) {
      if (const auto* arc = std::get_if<DrawArcCommand>(&command)) {
        result.push_back(*arc);
      }
    }
    return result;
  };
  const ProgressCircleStyle style = ThemeDefinitionValue<ProgressCircleStyle>(MaterialThemeDefinition());

  TestPlatform determinate_platform;
  determinate_platform.platform_resources = BuiltinTestResources();
  Runtime determinate{MaterialDeterminateProgressCircleApp, determinate_platform};
  determinate.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  const auto determinate_arcs = arcs(determinate.BuildFrame());
  REQUIRE(determinate_arcs.size() == 2);
  const DrawArcCommand& track = determinate_arcs[0];
  const DrawArcCommand& indicator = determinate_arcs[1];
  const float expected_gap_angle =
      (style.track_gap + style.stroke_width) / (style.size * 0.5F - style.stroke_width * 0.5F);
  REQUIRE(track.color == style.track_color);
  REQUIRE(track.style.cap == StrokeCap::Round);
  REQUIRE(indicator.style.cap == StrokeCap::Round);
  REQUIRE(std::abs(track.start_angle - (indicator.start_angle + indicator.sweep_angle + expected_gap_angle)) < 0.001F);
  REQUIRE(std::abs(track.sweep_angle - (pi * 2.0F - indicator.sweep_angle - expected_gap_angle * 2.0F)) < 0.001F);

  TestPlatform animated_platform;
  animated_platform.platform_resources = BuiltinTestResources();
  Runtime animated{MaterialIndeterminateProgressCircleApp, animated_platform};
  animated.SetWindowMetrics({.viewport = {64.0F, 64.0F}});
  const auto initial = arcs(animated.BuildFrame());
  REQUIRE(initial.size() == 1);
  REQUIRE(initial[0].color == style.indicator_color);
  REQUIRE(std::abs(initial[0].sweep_angle - pi * 2.0F * style.minimum_indeterminate_arc_fraction) < 0.001F);

  animated_platform.AdvanceTime(style.animation_duration * 0.1);
  const auto advanced = arcs(animated.BuildFrame());
  REQUIRE(advanced.size() == 1);
  REQUIRE(advanced[0].sweep_angle > initial[0].sweep_angle);
  REQUIRE(std::abs(advanced[0].start_angle - initial[0].start_angle) > 0.1F);
}

TEST_CASE("TestProgressBarDrawingStateAndAnimation") {
  const ProgressBarStyle style = ProgressBarStyle::Default();
  const float indeterminate_width = style.width * style.indeterminate_fraction;

  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime determinate{DeterminateProgressBarApp, platform};
  determinate.SetWindowMetrics({.viewport = {200.0F, 20.0F}});
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
  empty.SetWindowMetrics({.viewport = {200.0F, 20.0F}});
  REQUIRE(DrawRectangles(empty.BuildFrame()).size() == 1);

  Runtime full{FullProgressBarApp, platform};
  full.SetWindowMetrics({.viewport = {200.0F, 20.0F}});
  const auto full_rectangles = DrawRectangles(full.BuildFrame());
  REQUIRE(full_rectangles.size() == 2);
  REQUIRE(full_rectangles[1].rect.width == style.width);

  TestPlatform animated_platform;
  animated_platform.platform_resources = BuiltinTestResources();
  Runtime animated{IndeterminateProgressBarApp, animated_platform};
  animated.SetWindowMetrics({.viewport = {200.0F, 20.0F}});
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
  reduced_platform.platform_resources = BuiltinTestResources();
  Runtime reduced{ReducedMotionProgressBarApp, reduced_platform};
  reduced.SetWindowMetrics({.viewport = {200.0F, 20.0F}});
  const int reduced_requests_before = reduced_platform.requested_frames;
  const auto reduced_rectangles = DrawRectangles(reduced.BuildFrame());
  REQUIRE(reduced_rectangles.size() == 2);
  REQUIRE(reduced_platform.requested_frames == reduced_requests_before);
  REQUIRE_FALSE(reduced.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestProgressBarStyleChangesSpeedWithoutResettingPhase") {
  const ProgressBarStyle style = ProgressBarStyle::Default();
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{AdjustableProgressBarApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 20.0F}});
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

TEST_CASE("TestMaterialProgressBarUsesSeparatedTrackStopAndSegmentedMotion") {
  const ThemeDefinition definition = huxerui::MaterialThemeDefinition();
  const ProgressBarStyle style = ThemeDefinitionValue<ProgressBarStyle>(definition);

  TestPlatform determinate_platform;
  determinate_platform.platform_resources = BuiltinTestResources();
  Runtime determinate{MaterialDeterminateProgressBarApp, determinate_platform};
  determinate.SetWindowMetrics({.viewport = {280.0F, 20.0F}});
  const FlattenedScene& determinate_scene = determinate.BuildFrame();
  const detail::MountedNode* progress_node = FindMountedKind(*determinate.RootNode(), detail::NodeKind::ProgressBar);
  REQUIRE(progress_node != nullptr);
  const float bar_width = progress_node->Bounds().width;
  const DrawRectCommand* track = nullptr;
  const DrawRectCommand* indicator = nullptr;
  const DrawCircleCommand* stop = nullptr;
  for (const PaintCommand& command : determinate_scene.Commands()) {
    if (const auto* rectangle = std::get_if<DrawRectCommand>(&command)) {
      if (rectangle->color == style.track_color) {
        track = rectangle;
      } else if (rectangle->color == style.indicator_color) {
        indicator = rectangle;
      }
    } else if (const auto* circle = std::get_if<DrawCircleCommand>(&command)) {
      if (circle->color == style.indicator_color && circle->radius == style.stop_indicator_size * 0.5F) {
        stop = circle;
      }
    }
  }
  REQUIRE(track != nullptr);
  REQUIRE(indicator != nullptr);
  REQUIRE(stop != nullptr);
  REQUIRE(std::abs(indicator->rect.width - bar_width * 0.25F) < 0.001F);
  REQUIRE(std::abs(track->rect.x - (bar_width * 0.25F + style.track_gap)) < 0.001F);
  REQUIRE(std::abs(track->rect.width - (bar_width * 0.75F - style.track_gap)) < 0.001F);
  REQUIRE(std::abs(stop->center.x - (bar_width - style.stop_indicator_size * 0.5F)) < 0.001F);

  TestPlatform animated_platform;
  animated_platform.platform_resources = BuiltinTestResources();
  Runtime animated{MaterialIndeterminateProgressBarApp, animated_platform};
  animated.SetWindowMetrics({.viewport = {280.0F, 20.0F}});
  animated.BuildFrame();
  animated_platform.AdvanceTime(style.animation_duration * 0.6);
  const FlattenedScene& animated_scene = animated.BuildFrame();
  const auto indicator_segments = std::ranges::count_if(animated_scene.Commands(), [&](const PaintCommand& command) {
    const auto* rectangle = std::get_if<DrawRectCommand>(&command);
    return rectangle != nullptr && rectangle->color == style.indicator_color;
  });
  REQUIRE(indicator_segments == 2);

  TestPlatform reduced_platform;
  reduced_platform.platform_resources = BuiltinTestResources();
  Runtime reduced{ReducedMotionMaterialProgressBarApp, reduced_platform};
  reduced.SetWindowMetrics({.viewport = {280.0F, 20.0F}});
  const int requests_before = reduced_platform.requested_frames;
  const FlattenedScene& reduced_scene = reduced.BuildFrame();
  REQUIRE(std::ranges::count_if(reduced_scene.Commands(), [&](const PaintCommand& command) {
            const auto* rectangle = std::get_if<DrawRectCommand>(&command);
            return rectangle != nullptr && rectangle->color == style.indicator_color;
          }) == 2);
  REQUIRE(reduced_platform.requested_frames == requests_before);
  REQUIRE_FALSE(reduced.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestControlledSliderPointerKeyboardAndDrawing") {
  slider_changes = 0;
  const SliderStyle style = SliderStyle::Default();

  TestPlatform platform;
  Runtime runtime{SliderApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
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

  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  runtime.BuildFrame();
  platform.AdvanceTime(style.animation_duration);
  const FlattenedScene& focused = runtime.BuildFrame();
  const bool drew_node_focus_ring = std::ranges::any_of(focused.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<DrawBorderCommand>(command);
  });
  const bool drew_focused_handle = std::ranges::any_of(focused.Commands(), [&](const PaintCommand& command) {
    const auto* rectangle = std::get_if<DrawRectCommand>(&command);
    return rectangle != nullptr && rectangle->color == style.thumb &&
           rectangle->rect.width == style.pressed_thumb_width;
  });
  REQUIRE_FALSE(drew_node_focus_ring);
  REQUIRE(drew_focused_handle);
}

TEST_CASE("TestDisabledSliderIgnoresPointerInput") {
  slider_changes = 0;
  TestPlatform platform;
  Runtime runtime{DisabledSliderApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto* slider = FindMountedKind(*runtime.RootNode(), huxerui::detail::NodeKind::Slider);
  REQUIRE(slider != nullptr);
  REQUIRE(slider->kind == huxerui::detail::NodeKind::Slider);
  REQUIRE_FALSE(slider->interaction.enabled);
  const Rect bounds = slider->PresentationBounds();
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Down,
      .pointer_id = 85,
      .position = {bounds.x + bounds.width * 0.75F, bounds.y + bounds.height * 0.5F},
      .device_kind = PointerDeviceKind::Mouse,
  });
  REQUIRE(slider_changes == 0);

  const SliderStyle style = ThemeDefinitionValue<SliderStyle>(huxerui::MaterialThemeDefinition());
  bool drew_disabled_active_track = false;
  bool drew_disabled_inactive_track = false;
  bool drew_disabled_thumb = false;
  for (const PaintCommand& command : scene.Commands()) {
    if (const auto* fill = std::get_if<FillPathCommand>(&command)) {
      drew_disabled_active_track |= fill->color == style.disabled_active_track;
      drew_disabled_inactive_track |= fill->color == style.disabled_inactive_track;
    } else if (const auto* rectangle = std::get_if<DrawRectCommand>(&command)) {
      drew_disabled_thumb |= rectangle->color == style.disabled_thumb;
    }
  }
  REQUIRE(drew_disabled_active_track);
  REQUIRE(drew_disabled_inactive_track);
  REQUIRE(drew_disabled_thumb);
}

TEST_CASE("TestFlatSliderRetainsThemeFocusRing") {
  TestPlatform platform;
  Runtime runtime{FlatSliderFocusApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  const FlattenedScene& focused = runtime.BuildFrame();

  const bool drew_focus_ring = std::ranges::any_of(focused.Commands(), [](const PaintCommand& command) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    return border != nullptr && border->color == Color::Rgb(40, 180, 90) && border->style.width == 3.0F;
  });
  REQUIRE(drew_focus_ring);
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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

TEST_CASE("TestPressedInteractionFallsBackToAvailableHoverLayer") {
  TestPlatform platform;
  Runtime runtime{FallbackIndicationApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  const Color hover = Color::Rgb(20, 80, 160, 0.2F);
  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Move, 103, {20.0F, 20.0F}});
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 103, {20.0F, 20.0F}});
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), hover) != nullptr);
}

TEST_CASE("TestIndicationReplacesResolvedBorderAndCornerRadii") {
  TestPlatform platform;
  Runtime runtime{SurfaceIndicationApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});

  const Color normal_border = Color::Rgb(160, 40, 60);
  const Color pressed_border = Color::Rgb(20, 100, 200);
  const FlattenedScene& normal = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(normal, normal_border) != nullptr);
  REQUIRE(FindBorderWithColor(normal, pressed_border) == nullptr);

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 104, {20.0F, 20.0F}});
  const FlattenedScene& pressed = runtime.BuildFrame();
  const DrawBorderCommand* border = FindBorderWithColor(pressed, pressed_border);
  REQUIRE(border != nullptr);
  REQUIRE(border->style.width == 4.0F);
  REQUIRE(border->corner_radius == 20.0F);
  REQUIRE(FindBorderWithColor(pressed, normal_border) == nullptr);
}

TEST_CASE("TestEnabledInheritanceAndHitTestBlocking") {
  disabled_clicks = 0;
  underlying_clicks = 0;

  TestPlatform platform;
  Runtime overlay{DisabledHitTestApp, platform};
  overlay.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& scene = overlay.BuildFrame();
  const DrawTextCommand* disabled = FindText(scene, "disabled overlay");
  REQUIRE(disabled != nullptr);
  REQUIRE(std::abs(disabled->style.foreground.alpha - 0.42F) < 0.001F);

  const auto* overlay_root = overlay.RootNode();
  REQUIRE(overlay_root != nullptr);
  REQUIRE(overlay_root->children.size() == 2);
  REQUIRE(!overlay_root->children[1]->IsEnabled());
  REQUIRE(overlay_root->children[1]->render_node.opacity == 1.0F);
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
  subtree.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& subtree_display = subtree.BuildFrame();
  const auto* subtree_root = subtree.RootNode();
  REQUIRE(subtree_root != nullptr);
  REQUIRE(!subtree_root->IsEnabled());
  REQUIRE(subtree_root->applies_disabled_appearance);
  REQUIRE(subtree_root->render_node.opacity == Catch::Approx(0.42F));
  REQUIRE(subtree_root->children.size() == 1);
  REQUIRE(!subtree_root->children[0]->IsEnabled());
  REQUIRE_FALSE(subtree_root->children[0]->applies_disabled_appearance);
  REQUIRE(subtree_root->children[0]->render_node.opacity == 1.0F);
  const DrawTextCommand* child = FindText(subtree_display, "disabled child");
  REQUIRE(child != nullptr);
  REQUIRE(child->style.foreground.alpha == 1.0F);
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

TEST_CASE("TestDisabledButtonStyleChangeInvalidatesContentPaint") {
  TestPlatform platform;
  Runtime runtime{DisabledButtonStyleUpdateApp, platform};
  runtime.SetWindowMetrics({.viewport = {180.0F, 64.0F}});

  const Color initial = Color::Rgb(30, 80, 170);
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), initial) != nullptr);

  alternate_disabled_button_style = true;
  const Color updated = Color::Rgb(180, 40, 60);
  const FlattenedScene& scene = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(scene, initial) == nullptr);
  REQUIRE(FindRectWithColor(scene, updated) != nullptr);
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
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
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
  REQUIRE(first_border->style.width == 3.0F);
  const detail::MountedNode* focused_first_node = FindMountedText(*runtime.RootNode(), "first");
  REQUIRE(focused_first_node != nullptr);
  const Rect focused_bounds = focused_first_node->PresentationBounds();
  const float focus_ring_outset = 7.0F;
  const Rect expected_focus_ring{
      focused_bounds.x - focus_ring_outset,
      focused_bounds.y - focus_ring_outset,
      focused_bounds.width + focus_ring_outset * 2.0F,
      focused_bounds.height + focus_ring_outset * 2.0F,
  };
  REQUIRE(first_border->rect == expected_focus_ring);
  REQUIRE(first_border->corner_radius == focused_first_node->properties.corner_radii.top_left + focus_ring_outset);

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
  REQUIRE(std::abs(first_text->style.foreground.alpha - 0.3F) < 0.001F);
  const detail::MountedNode* first_node = FindMountedText(*runtime.RootNode(), "first");
  REQUIRE(first_node != nullptr);
  REQUIRE(first_node->render_node.opacity == 1.0F);

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
  const Color* keyboard_press = LayerFillColor(huxerui::FlatLightThemeSpec().interactions.indication.press);
  REQUIRE(keyboard_press != nullptr);
  REQUIRE(FindRectWithColor(space_down, *keyboard_press) != nullptr);

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
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
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

TEST_CASE("TestFocusableViewWithoutClickPaintsFocusRing") {
  TestPlatform platform;
  Runtime runtime{FocusOnlyApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const FlattenedScene& focused = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(focused, Color::Rgb(40, 180, 90)) != nullptr);
}

TEST_CASE("TestVisualFillIndicationAndFocusRingValidateDuringViewResolution") {
  const auto rejects = [](RootFactory factory) {
    TestPlatform platform;
    Runtime runtime{factory, platform};
    runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  };

  rejects(+[] { return Spacer().With(Background{LinearGradient{.stops = {{0.0F, Color::Black()}}}}); });
  rejects(+[] { return Spacer().With(Background{ImageFill{.source = ImageAsset{}}}); });
  rejects(+[] {
    return Button("invalid indication").With(Indication{.geometry = {.layer_size = Size{-1.0F, 20.0F}}});
  });
  rejects(+[] {
    return Button("invalid ripple")
        .With(Indication{
            .ripple = RippleEffect{
                .color = {0.0F, 0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()},
            },
        });
  });

  TestPlatform platform;
  Runtime runtime{InvalidFocusRingApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
}

TEST_CASE("TestModalDialogTrapsAndRestoresFocusTraversal") {
  saved_dialogs.reset();
  background_dialog_clicks = 0;
  first_dialog_clicks = 0;
  second_dialog_clicks = 0;

  TestPlatform platform;
  Runtime runtime{FocusDialogApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

TEST_CASE("TestViewportClassRecomposesExistingLayersAcrossBreakpoints") {
  installed_root_service.reset();
  observed_layer_viewport_class = ViewportClass::Compact;
  layer_viewport_compositions = 0;

  AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](RootContext& root) {
    installed_root_service = std::make_shared<TestRootService>(TestRootService{
        &root.Layers(),
        0,
    });
    root.Provide(installed_root_service);
  });

  TestPlatform platform;
  Runtime runtime{RootHookApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {480.0F, 600.0F}});
  runtime.BuildFrame();

  installed_root_service->layers->Attach({}, [] {
    ++layer_viewport_compositions;
    observed_layer_viewport_class = UseViewportClass();
    return Text("responsive layer");
  });
  runtime.BuildFrame();
  REQUIRE(layer_viewport_compositions == 1);
  REQUIRE(observed_layer_viewport_class == ViewportClass::Compact);

  runtime.SetWindowMetrics({.viewport = {560.0F, 600.0F}});
  runtime.BuildFrame();
  REQUIRE(layer_viewport_compositions == 1);

  runtime.SetWindowMetrics({.viewport = {600.0F, 600.0F}});
  runtime.BuildFrame();
  REQUIRE(layer_viewport_compositions == 2);
  REQUIRE(observed_layer_viewport_class == ViewportClass::Medium);
}

TEST_CASE("TestToastAndDialogPresentation") {
  saved_toast.reset();
  saved_dialogs.reset();
  saved_dialog_context.reset();

  TestPlatform platform;
  Runtime runtime{PresentationThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(saved_toast->Show(""), std::invalid_argument);
  REQUIRE_NOTHROW(runtime.BuildFrame());
}

TEST_CASE("TestToastRetainsItsLayerUntilExitMotionCompletes") {
  saved_toast.reset();

  TestPlatform platform;
  Runtime runtime{MaterialPresentationApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
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
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
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
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  saved_dialogs->Show("Save changes?", "The current document has unsaved changes.", "OK", [] {
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
  runtime.SetWindowMetrics({.viewport = {800.0F, 480.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(saved_dialogs->Show("", "Message", "OK"), std::invalid_argument);
  REQUIRE_THROWS_AS(saved_dialogs->Show("Title", "", "OK"), std::invalid_argument);

  saved_dialogs->Show("Short", "Message", "OK");
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
  runtime.SetWindowMetrics({.viewport = {640.0F, 360.0F}});
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

  const Indication expected =
      ThemeDefinitionValue<DialogStyle>(MaterialThemeDefinition()).positive_action_indication;
  REQUIRE(expected.ripple.has_value());
  const DrawCircleCommand* ripple = nullptr;
  for (const auto& command : pressed.Commands()) {
    const auto* circle = std::get_if<DrawCircleCommand>(&command);
    if (circle && circle->radius > 0.0F && circle->color == expected.ripple->color) {
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
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

  saved_dialogs->Show([] { return Text("dark dialog"); }, huxerui::DialogOptions{.dismiss_on_outside_press = false});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {240.0F, 200.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
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
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
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
