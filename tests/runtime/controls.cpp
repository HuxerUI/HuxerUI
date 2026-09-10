#include "runtime_test_support.h"

#include <limits>

#include "components/indication_internal.h"

namespace huxerui::test {
namespace {

int slider_key_events = 0;
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

View MaterialSelectableChipApp() {
  auto selected = UseState(false);
  chip_selected = selected;
  return huxerui::MaterialTheme {
    Chip("Selectable", selected).OnChanged([selected](bool value) { selected = value; }),
  };
}

View AsymmetricSegmentedButtonApp() {
  SegmentedButtonStyle style = SegmentedButtonStyle::Default();
  style.corner_radii = {16.0F, 4.0F, 12.0F, 8.0F};
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), SegmentedButton({"Day", "Week", "Month"}, 1)};
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
      ).OnChanged([](std::size_t) {}).With(Frame{.width = 240.0F}),
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
    Slider(value)
        .Range(0.0F, 10.0F)
        .Step(2.0F)
        .OnChanged([value](float changed) {
          ++slider_changes;
          value = changed;
        })
        .On<ViewEvents::KeyDown>([](const KeyEvent&) {
          ++slider_key_events;
          return true;
        }),
  };
}

State<float> lifecycle_slider_value;
State<float> lifecycle_slider_maximum;
State<float> lifecycle_slider_step;
State<bool> lifecycle_slider_enabled;
State<bool> lifecycle_slider_visible;
std::vector<std::pair<std::string, float>> slider_lifecycle_events;
bool accept_slider_proposals = true;

View SliderLifecycleApp() {
  lifecycle_slider_value = UseState(4.0F);
  lifecycle_slider_maximum = UseState(10.0F);
  lifecycle_slider_step = UseState(2.0F);
  lifecycle_slider_enabled = UseState(true);
  lifecycle_slider_visible = UseState(true);
  if (!lifecycle_slider_visible.Get()) {
    return Text("Replacement");
  }
  return Slider(lifecycle_slider_value)
      .Range(0.0F, lifecycle_slider_maximum.Get())
      .Step(lifecycle_slider_step.Get())
      .OnStarted([](float value) { slider_lifecycle_events.emplace_back("started", value); })
      .OnChanged([](float value) {
        slider_lifecycle_events.emplace_back("changed", value);
        if (accept_slider_proposals) {
          lifecycle_slider_value = value;
        }
      })
      .OnCommitted([](float value) { slider_lifecycle_events.emplace_back("committed", value); })
      .OnCanceled([] { slider_lifecycle_events.emplace_back("canceled", 0.0F); })
      .With(Enabled{lifecycle_slider_enabled.Get()});
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

} // namespace

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
    return border != nullptr && border->color == style.border.color && border->style.width == style.border.width;
  });
  REQUIRE(paints_outline);
}

TEST_CASE("TestMaterialSelectableChipTransitionsBetweenPresentAndAbsentBorders") {
  TestPlatform platform;
  Runtime runtime{MaterialSelectableChipApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 64.0F}});
  runtime.BuildFrame();

  const ChipStyle style = ThemeDefinitionValue<ChipStyle>(MaterialThemeDefinition());
  const auto* chip = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Chip);
  REQUIRE(chip != nullptr);
  REQUIRE(chip->resolved_border == style.border);

  const Rect bounds = chip->PresentationBounds();
  const Point pointer{bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Move, 97, pointer});
  runtime.BuildFrame();
  ClickAt(runtime, pointer, 97);
  runtime.BuildFrame();
  REQUIRE(chip_selected.Get());
  platform.AdvanceTime(MaterialLightThemeSpec().motion.fast * 0.5);
  runtime.BuildFrame();
  chip = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Chip);
  REQUIRE(chip != nullptr);
  REQUIRE(chip->resolved_border.has_value());
  REQUIRE(chip->resolved_border->width < style.border.width);
  platform.AdvanceTime(MaterialLightThemeSpec().motion.fast);
  runtime.BuildFrame();
  chip = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Chip);
  REQUIRE(chip != nullptr);
  REQUIRE(chip->resolved_border == style.selected_border);

  ClickAt(runtime, pointer, 97);
  runtime.BuildFrame();
  REQUIRE_FALSE(chip_selected.Get());
  platform.AdvanceTime(MaterialLightThemeSpec().motion.fast * 0.5);
  runtime.BuildFrame();
  chip = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Chip);
  REQUIRE(chip != nullptr);
  REQUIRE(chip->resolved_border.has_value());
  REQUIRE(chip->resolved_border->width < style.border.width);
  platform.AdvanceTime(MaterialLightThemeSpec().motion.fast);
  runtime.BuildFrame();
  chip = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Chip);
  REQUIRE(chip != nullptr);
  REQUIRE(chip->resolved_border == style.border);
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
  REQUIRE(BrushIsColor(rectangles[0].brush, DividerStyle::Default().color));
  REQUIRE(rectangles[1].rect == Rect{0.0F, 0.0F, DividerStyle::Default().thickness, 24.0F});
  REQUIRE(BrushIsColor(rectangles[1].brush, DividerStyle::Default().color));
}

TEST_CASE("SegmentedButton preserves layout and controlled interaction contracts") {
  segmented_button_changes = 0;
  disabled_segmented_button_changes = 0;
  rejected_segmented_button_changes = 0;

  TestPlatform platform;
  Runtime runtime{SegmentedButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 240.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto label_center = [&](std::string_view label) {
    const auto bounds = FindPresentedTextRect(initial, label);
    REQUIRE(bounds.has_value());
    return Point{bounds->x + bounds->width * 0.5F, bounds->y + bounds->height * 0.5F};
  };

  SECTION("Layout and selected styling") {
    const auto* root = runtime.RootNode();
    REQUIRE(root != nullptr);
    REQUIRE(root->children.size() == 4);
    const auto* group_scope = root->children[0].get();
    REQUIRE(group_scope->children.size() == 1);
    const auto* disabled_scope = root->children[1].get();
    REQUIRE(disabled_scope->children.size() == 1);
    const auto* narrow_scope = root->children[3].get();
    REQUIRE(narrow_scope->children.size() == 1);
    const auto* group = group_scope->children[0].get();
    const auto* narrow_group = narrow_scope->children[0].get();
    REQUIRE(group->focusable);
    REQUIRE(group->children.size() == 3);
    REQUIRE(group->children[0]->measured_size == group->children[1]->measured_size);
    REQUIRE(group->children[1]->measured_size == group->children[2]->measured_size);
    REQUIRE(group->children[0]->measured_size.height >= SegmentedButtonStyle::Default().minimum_height);
    REQUIRE(
        group->children[1]->layout_offset.x ==
        group->children[0]->measured_size.width - SegmentedButtonStyle::Default().border.width
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
    for (const std::string_view label : {"Day", "Week", "Month", "One", "Two"}) {
      const DrawTextCommand* text = FindText(initial, label);
      REQUIRE(text != nullptr);
      REQUIRE(text->options.align == TextAlign::Center);
      REQUIRE(text->options.vertical_align == TextVerticalAlign::Center);
      REQUIRE(text->options.wrap == TextWrap::NoWrap);
    }
  }

  SECTION("Pointer cancellation leaves selection unchanged") {
    const Point pointer = label_center("Month");
    runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 120, pointer});
    runtime.HandlePointerEvent(PointerEvent{PointerEventType::Cancel, 120, pointer});
    REQUIRE(segmented_button_changes == 0);
    REQUIRE(segmented_button_selection.Get() == 0);
  }

  SECTION("Selecting the current item does not emit a change") {
    ClickAt(runtime, label_center("Day"));
    REQUIRE(segmented_button_changes == 0);
    REQUIRE(segmented_button_selection.Get() == 0);
  }

  SECTION("Accepted selection updates retained content") {
    const auto* week = FindMountedText(*runtime.RootNode(), "Week");
    REQUIRE(week != nullptr);
    const auto identity = week->identity;
    ClickAt(runtime, label_center("Week"));
    REQUIRE(segmented_button_changes == 1);
    REQUIRE(segmented_button_selection.Get() == 1);
    const auto& selected = runtime.BuildFrame();
    week = FindMountedText(*runtime.RootNode(), "Week");
    REQUIRE(week != nullptr);
    REQUIRE(week->identity == identity);
    REQUIRE(week->properties.background == SegmentedButtonStyle::Default().selected_background);
    const auto* label = FindText(selected, "Week");
    REQUIRE(label != nullptr);
    REQUIRE(label->style.foreground == SegmentedButtonStyle::Default().selected_label);
    REQUIRE(label->options.align == TextAlign::Center);
  }

  SECTION("Keyboard navigation wraps selection") {
    ClickAt(runtime, label_center("Week"));
    runtime.BuildFrame();
    runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
    REQUIRE(segmented_button_changes == 2);
    REQUIRE(segmented_button_selection.Get() == 2);
    runtime.BuildFrame();
    runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
    REQUIRE(segmented_button_changes == 3);
    REQUIRE(segmented_button_selection.Get() == 0);
  }

  SECTION("Disabled items ignore pointer input") {
    ClickAt(runtime, label_center("Two"));
    REQUIRE(disabled_segmented_button_changes == 0);
  }

  SECTION("Rejected selection remains a proposal on subsequent clicks") {
    const Point pointer = label_center("Reject");
    ClickAt(runtime, pointer, 126);
    ClickAt(runtime, pointer, 127);
    REQUIRE(rejected_segmented_button_changes == 2);
  }
}

TEST_CASE("TestSegmentedButtonPreservesOnlyOuterAsymmetricCorners") {
  TestPlatform platform;
  Runtime runtime{AsymmetricSegmentedButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 64.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const auto* group = root->children[0]->children[0].get();
  REQUIRE(group->children.size() == 3);
  REQUIRE(group->properties.corner_radii == CornerRadii{16.0F, 4.0F, 12.0F, 8.0F});
  REQUIRE(group->children[0]->properties.corner_radii == CornerRadii{16.0F, 0.0F, 0.0F, 8.0F});
  REQUIRE(group->children[1]->properties.corner_radii == CornerRadii{});
  REQUIRE(group->children[2]->properties.corner_radii == CornerRadii{0.0F, 4.0F, 12.0F, 0.0F});
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
  REQUIRE(SolidFillColor(group->children[1]->properties.background) != nullptr);
  REQUIRE(SolidFillColor(style.selected_background) != nullptr);
  REQUIRE(*SolidFillColor(group->children[1]->properties.background) == *SolidFillColor(style.selected_background));
  REQUIRE(MountedIndication(*group->children[1]) != nullptr);
  REQUIRE(*MountedIndication(*group->children[1]) == *style.selected_indication);
  REQUIRE(group->children[0]->properties.border == style.border);
  const DrawTextCommand* selected = FindText(scene, "Week");
  REQUIRE(selected != nullptr);
  REQUIRE(selected->style.foreground == style.selected_label);
  REQUIRE(selected->options.align == TextAlign::Center);
  REQUIRE(selected->options.vertical_align == TextVerticalAlign::Center);

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
    if (BrushIsColor(rectangle.brush, style.indicator) && rectangle.rect.height == style.indicator_height) {
      indicator = &rectangle;
    }
    if (BrushIsColor(rectangle.brush, style.divider_color) && rectangle.rect.height == style.divider_height) {
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
    return BrushIsColor(rectangle.brush, style.divider_color) && rectangle.rect.height == style.divider_height;
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
  const DrawTextCommand* mixed = FindText(scene, "Mixed");
  REQUIRE(mixed != nullptr);
  REQUIRE(mixed->options.align == TextAlign::Leading);
  REQUIRE(mixed->options.vertical_align == TextVerticalAlign::Center);
  REQUIRE(mixed->options.wrap == TextWrap::NoWrap);
  const Rect mixed_bounds = segments->children[0]->ContentBounds();
  const float leading_space =
      mixed->rect.x - mixed_content.icon_size.width - mixed_content.icon_spacing - mixed_bounds.x;
  const float trailing_space = mixed_bounds.x + mixed_bounds.width - mixed->rect.x - mixed->rect.width;
  REQUIRE(leading_space > 0.0F);
  REQUIRE(leading_space == Catch::Approx(trailing_space));
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
      if (BrushIsColor(rectangle->brush, style.track_color)) {
        track = rectangle;
      } else if (BrushIsColor(rectangle->brush, style.indicator_color)) {
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
    return rectangle != nullptr && BrushIsColor(rectangle->brush, style.indicator_color);
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
            return rectangle != nullptr && BrushIsColor(rectangle->brush, style.indicator_color);
          }) == 2);
  REQUIRE(reduced_platform.requested_frames == requests_before);
  REQUIRE_FALSE(reduced.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestControlledSliderPointerKeyboardAndDrawing") {
  slider_changes = 0;
  slider_key_events = 0;
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
      if (BrushIsColor(rectangle->brush, style.thumb) && rectangle->rect.width == style.thumb_width &&
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
      if (BrushIsColor(rectangle->brush, style.thumb) && rectangle->rect.height == style.pressed_thumb_height &&
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
  REQUIRE(slider_key_events == 0);
  REQUIRE((runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Unknown})));
  REQUIRE(slider_key_events == 1);

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
      if (BrushIsColor(rectangle->brush, style.thumb) && rectangle->rect.width == style.thumb_width &&
          rectangle->rect.height == style.thumb_height) {
        thumb = rectangle;
      }
    }
    if (const auto* fill = std::get_if<huxerui::FillPathCommand>(&command)) {
      if (BrushIsColor(fill->brush, style.inactive_track) && fill->path.Bounds().height == style.track_height) {
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
    return rectangle != nullptr && BrushIsColor(rectangle->brush, style.thumb) &&
           rectangle->rect.width == style.pressed_thumb_width;
  });
  REQUIRE_FALSE(drew_node_focus_ring);
  REQUIRE(drew_focused_handle);
}

TEST_CASE("Slider pointer adjustment preserves proposals across controlled writeback and recomposition") {
  slider_lifecycle_events.clear();
  accept_slider_proposals = true;
  TestPlatform platform;
  Runtime runtime{SliderLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  runtime.BuildFrame();
  REQUIRE(slider_lifecycle_events.empty());
  const Rect bounds = runtime.RootNode()->PresentationBounds();
  const Point end{bounds.x + bounds.width - 1.0F, bounds.y + bounds.height * 0.5F};
  runtime.HandlePointerEvent({PointerEventType::Down, 901, end});
  runtime.BuildFrame();
  lifecycle_slider_value = 2.0F;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Move, 901, end});
  runtime.HandlePointerEvent({PointerEventType::Up, 901, end});
  runtime.HandlePointerEvent({PointerEventType::Cancel, 901, end});
  const std::vector<std::pair<std::string, float>> expected{
      {"started", 4.0F}, {"changed", 10.0F}, {"committed", 10.0F},
  };
  REQUIRE(slider_lifecycle_events == expected);
  REQUIRE(lifecycle_slider_value.Get() == 2.0F);
}

TEST_CASE("Slider unchanged pointer clicks commit and rejected proposals remain proposals") {
  slider_lifecycle_events.clear();
  accept_slider_proposals = false;
  TestPlatform platform;
  Runtime runtime{SliderLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  runtime.BuildFrame();
  lifecycle_slider_value = 0.0F;
  runtime.BuildFrame();
  const Rect bounds = runtime.RootNode()->PresentationBounds();
  const Point start{bounds.x + 1.0F, bounds.y + bounds.height * 0.5F};
  const Point end{bounds.x + bounds.width - 1.0F, start.y};
  ClickAt(runtime, start);
  ClickAt(runtime, end);
  const std::vector<std::pair<std::string, float>> expected{
      {"started", 0.0F}, {"committed", 0.0F},
      {"started", 0.0F}, {"changed", 10.0F}, {"committed", 10.0F},
  };
  REQUIRE(slider_lifecycle_events == expected);
  REQUIRE(lifecycle_slider_value.Get() == 0.0F);
}

TEST_CASE("Slider cancellation terminates once without rollback") {
  slider_lifecycle_events.clear();
  accept_slider_proposals = true;
  TestPlatform platform;
  Runtime runtime{SliderLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  runtime.BuildFrame();
  const Rect bounds = runtime.RootNode()->PresentationBounds();
  const Point end{bounds.x + bounds.width - 1.0F, bounds.y + bounds.height * 0.5F};
  runtime.HandlePointerEvent({PointerEventType::Down, 902, end});
  runtime.BuildFrame();

  SECTION("pointer cancel") {
    runtime.HandlePointerEvent({PointerEventType::Cancel, 902, end});
  }
  SECTION("escape") {
    runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Escape});
  }
  SECTION("disabled") {
    lifecycle_slider_enabled = false;
  }
  SECTION("range change") {
    lifecycle_slider_maximum = 20.0F;
  }
  SECTION("step change") {
    lifecycle_slider_step = 1.0F;
  }
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Cancel, 902, end});
  runtime.HandlePointerEvent({PointerEventType::Up, 902, end});
  runtime.BuildFrame();
  const std::vector<std::pair<std::string, float>> expected{
      {"started", 4.0F}, {"changed", 10.0F}, {"canceled", 0.0F},
  };
  REQUIRE(slider_lifecycle_events == expected);
  REQUIRE(lifecycle_slider_value.Get() == 10.0F);
}

TEST_CASE("Slider ignores another pointer and does not emit a destructor event") {
  slider_lifecycle_events.clear();
  accept_slider_proposals = true;
  TestPlatform platform;
  Runtime runtime{SliderLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  runtime.BuildFrame();
  const Rect bounds = runtime.RootNode()->PresentationBounds();
  const Point end{bounds.x + bounds.width - 1.0F, bounds.y + bounds.height * 0.5F};
  const Point start{bounds.x + 1.0F, end.y};
  runtime.HandlePointerEvent({PointerEventType::Down, 903, end, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Down, 904, start, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 904, start, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 903, end, PointerDeviceKind::Touch});
  const std::vector<std::pair<std::string, float>> expected{
      {"started", 4.0F}, {"changed", 10.0F}, {"committed", 10.0F},
  };
  REQUIRE(slider_lifecycle_events == expected);
  lifecycle_slider_visible = false;
  runtime.BuildFrame();
  REQUIRE(slider_lifecycle_events == expected);
}

TEST_CASE("Slider keyboard repeats and accessibility adjustments commit independently") {
  slider_lifecycle_events.clear();
  accept_slider_proposals = true;
  TestPlatform platform;
  Runtime runtime{SliderLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  runtime.BuildFrame();
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab});
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowRight});
  runtime.BuildFrame();
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowRight, .repeat = true});
  runtime.BuildFrame();
  runtime.HandleKeyEvent({.type = KeyEventType::Up, .key = Key::ArrowRight});
  const auto& nodes = runtime.LastCommit().semantic_frame->nodes;
  const auto slider = std::ranges::find(nodes, SemanticRole::Slider, &SemanticNode::role);
  REQUIRE(slider != nodes.end());
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(slider->id, {SemanticActionKind::Increment, std::monostate{}}));
  runtime.BuildFrame();
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowRight, .repeat = true});
  const std::vector<std::pair<std::string, float>> expected{
      {"started", 4.0F}, {"changed", 6.0F}, {"committed", 6.0F},
      {"started", 6.0F}, {"changed", 8.0F}, {"committed", 8.0F},
      {"started", 8.0F}, {"changed", 10.0F}, {"committed", 10.0F},
  };
  REQUIRE(slider_lifecycle_events == expected);
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
      drew_disabled_active_track |= BrushIsColor(fill->brush, style.disabled_active_track);
      drew_disabled_inactive_track |= BrushIsColor(fill->brush, style.disabled_inactive_track);
    } else if (const auto* rectangle = std::get_if<DrawRectCommand>(&command)) {
      drew_disabled_thumb |= BrushIsColor(rectangle->brush, style.disabled_thumb);
    }
  }
  REQUIRE(drew_disabled_active_track);
  REQUIRE(drew_disabled_inactive_track);
  REQUIRE(drew_disabled_thumb);
}

TEST_CASE("TestSliderRejectsInvalidConfiguration") {
  REQUIRE_THROWS_AS(Slider(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
  REQUIRE_THROWS_AS(Slider(0.5F).Range(1.0F, 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(Slider(0.5F).Range(2.0F, 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(Slider(0.5F).Step(0.0F), std::invalid_argument);
}

} // namespace huxerui::test
