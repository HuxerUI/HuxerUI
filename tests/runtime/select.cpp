#include "runtime_test_support.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "components/indication_internal.h"

namespace huxerui::test {

namespace {

struct SelectOption {
  int id = 0;
  std::string label;
  bool enabled = true;

  bool operator==(const SelectOption&) const = default;
};

State<std::vector<SelectOption>> select_options;
State<std::size_t> select_index;
State<bool> select_enabled;
State<bool> select_configuration_updated;
State<bool> select_visible;
int select_changes = 0;
int invalid_select_style_case = 0;

constexpr Color custom_select_foreground = Color::Rgb(173, 47, 91);

View SelectApp() {
  select_options = UseState(std::vector<SelectOption>{
      {1, "One", true},
      {2, "Two", false},
      {3, "Three", true},
  });
  select_index = UseState<std::size_t>(0);
  select_enabled = UseState(true);
  return Select(select_options, select_index, [](const SelectOption& option) {
           return Text(option.label).Key(option.id).With(Enabled{option.enabled});
         })
      .Label("Number")
      .OnChanged([](std::size_t index) {
        ++select_changes;
        select_index = index;
      })
      .With(Enabled{select_enabled.Get()});
}

View InvalidSelectApp() {
  return Select(std::vector<std::string>{"One", "Two"}, 0, [](const std::string& label) { return Text(label); })
      .Label("Number")
      .OnChanged([](std::size_t) {})
      .With(Frame{.width = 180.0F})
      .Validation(ValidationResult::Invalid("Choose another value"));
}

View ReconfiguredSelectApp() {
  select_configuration_updated = UseState(false);
  const bool updated = select_configuration_updated.Get();
  return Select(std::array{"One", "Two"}, 0, [](const char* label) { return Text(label); })
      .Label(updated ? "Updated number" : "Number")
      .Validation(updated ? ValidationResult::Invalid("Updated error") : ValidationResult{});
}

View CopiedSelectConfigurationApp() {
  Select original(std::array{"One", "Two"}, 0, [](const char* label) { return Text(label); });
  Select first = original;
  Select second = original;
  return Column {
    std::move(first).Label("First"),
    std::move(second).Label("Second").Validation(ValidationResult::Invalid("Second error")),
  };
}

View DisabledSelectApp() {
  return Select(std::array{"One", "Two"}, 0, [](const char* label) { return Text(label); })
      .Label("Number")
      .With(Enabled{false});
}

View ConditionalSelectApp() {
  select_visible = UseState(true);
  if (!select_visible.Get()) {
    return Text("Replacement");
  }
  return Select(std::array{"One", "Two"}, 0, [](const char* label) { return Text(label); }).Label("Number");
}

View ThemedSelectApp() {
  SelectStyle style = SelectStyle::Default();
  style.foreground = custom_select_foreground;
  style.indication = std::nullopt;
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    Select(std::array{"One", "Two", "Three"}, 0, [](const char* label) { return Text(label); }).Label("Number"),
  };
}

View InvalidSelectStyleApp() {
  SelectStyle style = SelectStyle::Default();
  if (invalid_select_style_case == 0) {
    style.content_spacing = -1.0F;
  } else if (invalid_select_style_case == 1) {
    style.trigger_padding.left = -1.0F;
  } else {
    style.popup_shadow.blur_radius = -1.0F;
  }
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    Select(std::array{"One"}, 0, [](const char* label) { return Text(label); }).Label("Number"),
  };
}

View EmptySelectItemApp() {
  return Select(std::array{1}, 0, [](int) { return View{}; }).Label("Number");
}

View UnlabeledSelectItemApp() {
  return Select(std::array{1}, 0, [](int) { return Column{Text("One")}; }).Label("Number");
}

View InteractiveSelectItemApp() {
  return Select(std::array{1}, 0, [](int) { return Button("One"); }).Label("Number");
}

View NestedInteractiveSelectItemApp() {
  return Select(std::array{1}, 0, [](int) {
           return Column{Text("One"), Button("Details")}.With(Semantics{.label = "One"});
         })
      .Label("Number");
}

const SemanticNode& FindRole(const SemanticFrame& frame, SemanticRole role) {
  const auto found = std::ranges::find(frame.nodes, role, &SemanticNode::role);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

const SemanticNode& FindRoleWithLabel(const SemanticFrame& frame, SemanticRole role, const char* label) {
  const auto found = std::ranges::find_if(frame.nodes, [role, label](const SemanticNode& node) {
    return node.role == role && node.label == label;
  });
  REQUIRE(found != frame.nodes.end());
  return *found;
}

const SemanticNode& FindNode(const SemanticFrame& frame, SemanticNodeId id) {
  const auto found = std::ranges::find(frame.nodes, id, &SemanticNode::id);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

const SemanticNode& ListItemAt(const SemanticFrame& frame, const SemanticNode& list, std::size_t index) {
  REQUIRE(index < list.children.size());
  const SemanticNode& item = FindNode(frame, list.children[index]);
  REQUIRE(item.role == SemanticRole::ListItem);
  return item;
}

bool HasRole(const SemanticFrame& frame, SemanticRole role) {
  return std::ranges::any_of(frame.nodes, [role](const SemanticNode& node) { return node.role == role; });
}

const detail::DefaultIndication* FindDefaultIndication(const detail::MountedNode& node) {
  for (const detail::NodeExtensionEntry& entry : node.extensions) {
    if (detail::IsDefaultIndicationDescriptor(entry.descriptor)) {
      return static_cast<const detail::DefaultIndication*>(entry.value.get());
    }
  }
  for (const std::unique_ptr<detail::MountedNode>& child : node.children) {
    if (const detail::DefaultIndication* indication = FindDefaultIndication(*child)) {
      return indication;
    }
  }
  return nullptr;
}

void OpenSelect(Runtime& runtime) {
  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& select = FindRole(*frame, SemanticRole::ComboBox);
  ClickAt(runtime, {select.bounds.x + select.bounds.width * 0.5F, select.bounds.y + select.bounds.height * 0.5F});
  runtime.BuildCommit();
  runtime.BuildCommit();
}

} // namespace

TEST_CASE("SelectValidatesItsControlledIndex") {
  REQUIRE_THROWS_AS(
      Select(std::vector<int>{}, 0, [](int value) { return Text(std::to_string(value)); }),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Select(std::vector<int>{1, 2}, 2, [](int value) { return Text(std::to_string(value)); }),
      std::invalid_argument
  );
}

TEST_CASE("SelectRejectsInvalidFactoryResults") {
  TestPlatform platform{BuiltinTestResources()};
  for (const auto app :
       {EmptySelectItemApp, UnlabeledSelectItemApp, InteractiveSelectItemApp, NestedInteractiveSelectItemApp}) {
    Runtime runtime{app, platform};
    runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
}

TEST_CASE("SelectValidationPreservesItsFluentSurface") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{InvalidSelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const SemanticNode& select = FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::ComboBox);
  REQUIRE(select.invalid == true);
  REQUIRE(select.error == "Choose another value");
  REQUIRE(FindText(scene, "Choose another value") != nullptr);
  REQUIRE(FindBorderWithColor(scene, SelectStyle::Default().validation_error) != nullptr);
}

TEST_CASE("SelectConfigurationUpdatesThroughRecomposition") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ReconfiguredSelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});

  runtime.BuildFrame();
  const SemanticNode& initial = FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::ComboBox);
  REQUIRE(initial.label == "Number");
  REQUIRE(initial.invalid == false);

  select_configuration_updated = true;
  const FlattenedScene& scene = runtime.BuildFrame();
  const SemanticNode& updated = FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::ComboBox);
  REQUIRE(updated.label == "Updated number");
  REQUIRE(updated.invalid == true);
  REQUIRE(updated.error == "Updated error");
  REQUIRE(FindText(scene, "Updated error") != nullptr);
}

TEST_CASE("CopiedSelectConfigurationsRemainIndependent") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{CopiedSelectConfigurationApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  runtime.BuildFrame();
  const SemanticFrame& frame = *runtime.LastCommit().semantic_frame;
  const SemanticNode& first = FindRoleWithLabel(frame, SemanticRole::ComboBox, "First");
  const SemanticNode& second = FindRoleWithLabel(frame, SemanticRole::ComboBox, "Second");
  REQUIRE(first.invalid == false);
  REQUIRE(second.invalid == true);
  REQUIRE(second.error == "Second error");
}

TEST_CASE("SelectRejectsInvalidThemeGeometry") {
  TestPlatform platform{BuiltinTestResources()};
  for (invalid_select_style_case = 0; invalid_select_style_case < 3; ++invalid_select_style_case) {
    Runtime runtime{InvalidSelectStyleApp, platform};
    runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
}

TEST_CASE("SelectProvidesFlatAndMaterialLightAndDarkStyles") {
  const ThemeSpec flat_light_spec = FlatLightThemeSpec();
  const ThemeSpec flat_dark_spec = FlatDarkThemeSpec();
  const SelectStyle flat_light = detail::DefaultSelectStyle(flat_light_spec);
  const SelectStyle flat_dark = detail::DefaultSelectStyle(flat_dark_spec);
  const ThemeSpec material_light_spec = MaterialLightThemeSpec();
  const ThemeSpec material_dark_spec = MaterialDarkThemeSpec();
  const SelectStyle material_light = ThemeDefinitionValue<SelectStyle>(MaterialThemeDefinition());
  const SelectStyle material_dark = ThemeDefinitionValue<SelectStyle>(MaterialDarkThemeDefinition());

  REQUIRE(flat_light.background == flat_light_spec.colors.surface);
  REQUIRE(flat_dark.foreground == flat_dark_spec.colors.on_surface);
  REQUIRE_FALSE(flat_light.indication.has_value());
  REQUIRE_FALSE(flat_dark.item_indication.has_value());
  REQUIRE(material_light.background == material_light_spec.colors.surface_container_highest);
  REQUIRE(material_dark.popup_background == material_dark_spec.colors.surface_container);
  REQUIRE(material_light.indication.has_value());
  REQUIRE(material_dark.item_indication.has_value());
}

TEST_CASE("SelectUsesTheThemeIndicationAndForegroundAcrossItsPopup") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ThemedSelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const detail::DefaultIndication* indication = FindDefaultIndication(*root);
  REQUIRE(indication != nullptr);
  REQUIRE(indication->value.has_value());

  OpenSelect(runtime);
  const FlattenedScene& popup = runtime.BuildFrame();
  const DrawTextCommand* item = FindText(popup, "Three");
  REQUIRE(item != nullptr);
  REQUIRE(item->style.foreground == custom_select_foreground);
}

TEST_CASE("SelectOpensAnAccessibleChoiceListAndEmitsControlledChanges") {
  select_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const std::shared_ptr<const SemanticFrame> collapsed = runtime.BuildCommit().semantic_frame;
  const SemanticNode& collapsed_select = FindRole(*collapsed, SemanticRole::ComboBox);
  REQUIRE(collapsed_select.label == "Number");
  REQUIRE(collapsed_select.expanded == false);
  REQUIRE(collapsed_select.value == "One");
  REQUIRE(collapsed_select.read_only == true);
  REQUIRE((collapsed_select.actions & SemanticActionMask(SemanticActionKind::Expand)) != 0);

  OpenSelect(runtime);
  const std::shared_ptr<const SemanticFrame> first_expanded = runtime.LastCommit().semantic_frame;
  const SemanticNode& first_list = FindRole(*first_expanded, SemanticRole::List);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      ListItemAt(*first_expanded, first_list, 0).id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(select_changes == 0);
  REQUIRE(FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox).expanded == false);

  OpenSelect(runtime);
  const std::shared_ptr<const SemanticFrame> expanded = runtime.LastCommit().semantic_frame;
  const SemanticNode& list = FindRole(*expanded, SemanticRole::List);
  REQUIRE(list.collection == SemanticCollection{.item_count = 3});
  REQUIRE(list.children.size() == 3);
  REQUIRE(ListItemAt(*expanded, list, 0).selected == true);
  REQUIRE(ListItemAt(*expanded, list, 0).focused);
  REQUIRE((ListItemAt(*expanded, list, 0).actions & SemanticActionMask(SemanticActionKind::Focus)) != 0);
  REQUIRE_FALSE(ListItemAt(*expanded, list, 1).enabled);
  REQUIRE(ListItemAt(*expanded, list, 2).label == "Three");

  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      ListItemAt(*expanded, list, 1).id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(select_changes == 0);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      ListItemAt(*expanded, list, 2).id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(select_changes == 1);
  REQUIRE(select_index.Get() == 2);

  const std::shared_ptr<const SemanticFrame> changed = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindRole(*changed, SemanticRole::ComboBox).expanded == false);
  REQUIRE(FindRole(*changed, SemanticRole::ComboBox).value == "Three");
  REQUIRE(FindRole(*changed, SemanticRole::ComboBox).focused);
}

TEST_CASE("SelectSemanticActivateTogglesItsPopup") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const SemanticNodeId collapsed_id = FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox).id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      collapsed_id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      collapsed_id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  const std::shared_ptr<const SemanticFrame> collapsed = runtime.BuildCommit().semantic_frame;
  REQUIRE_FALSE(HasRole(*collapsed, SemanticRole::List));
  REQUIRE(FindRole(*collapsed, SemanticRole::ComboBox).expanded == false);

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      FindRole(*collapsed, SemanticRole::ComboBox).id, {SemanticActionKind::Expand, std::monostate{}}
  ));
  runtime.BuildCommit();
  REQUIRE(HasRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List));
}

TEST_CASE("DisabledSelectCannotOpenAndDynamicDisableDismissesItsPopup") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime disabled{DisabledSelectApp, platform};
  disabled.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const std::shared_ptr<const SemanticFrame> disabled_frame = disabled.BuildCommit().semantic_frame;
  const SemanticNode& disabled_select = FindRole(*disabled_frame, SemanticRole::ComboBox);
  REQUIRE_FALSE(disabled_select.enabled);
  REQUIRE(disabled_select.actions == 0);
  ClickAt(disabled, {disabled_select.bounds.x + 4.0F, disabled_select.bounds.y + 4.0F});
  REQUIRE_FALSE(HasRole(*disabled.BuildCommit().semantic_frame, SemanticRole::List));

  Runtime dynamic{SelectApp, platform};
  dynamic.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  OpenSelect(dynamic);
  REQUIRE(HasRole(*dynamic.LastCommit().semantic_frame, SemanticRole::List));
  select_enabled = false;
  dynamic.BuildCommit();
  const std::shared_ptr<const SemanticFrame> dismissed = dynamic.BuildCommit().semantic_frame;
  REQUIRE_FALSE(HasRole(*dismissed, SemanticRole::List));
  REQUIRE(FindRole(*dismissed, SemanticRole::ComboBox).expanded == false);
}

TEST_CASE("SelectUnmountDismissesItsPopup") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ConditionalSelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  OpenSelect(runtime);
  REQUIRE(HasRole(*runtime.LastCommit().semantic_frame, SemanticRole::List));

  select_visible = false;
  runtime.BuildCommit();
  const std::shared_ptr<const SemanticFrame> replaced = runtime.BuildCommit().semantic_frame;
  REQUIRE_FALSE(HasRole(*replaced, SemanticRole::ComboBox));
  REQUIRE_FALSE(HasRole(*replaced, SemanticRole::List));
}

TEST_CASE("SelectClearsItsActiveChoiceWhenEveryItemIsDisabled") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  OpenSelect(runtime);

  select_options = std::vector<SelectOption>{{1, "One", false}, {2, "Two", false}, {3, "Three", false}};
  runtime.BuildCommit();
  runtime.BuildCommit();
  runtime.BuildCommit();
  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& list = FindRole(*frame, SemanticRole::List);
  REQUIRE(std::ranges::none_of(list.children, [&frame](SemanticNodeId id) { return FindNode(*frame, id).focused; }));
}

TEST_CASE("SelectPointerChoiceCommitsAndRestoresTriggerFocus") {
  select_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  OpenSelect(runtime);
  const std::shared_ptr<const SemanticFrame> expanded = runtime.LastCommit().semantic_frame;
  const SemanticNode& list = FindRole(*expanded, SemanticRole::List);
  const Rect choice = ListItemAt(*expanded, list, 2).bounds;
  ClickAt(runtime, {choice.x + choice.width * 0.5F, choice.y + choice.height * 0.5F});

  REQUIRE(select_changes == 1);
  REQUIRE(select_index.Get() == 2);
  const SemanticNode& collapsed = FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox);
  REQUIRE(collapsed.expanded == false);
  REQUIRE(collapsed.focused);
}

TEST_CASE("SelectPointerCancelDoesNotOpenItsPopup") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const Rect bounds = FindRole(*frame, SemanticRole::ComboBox).bounds;
  const Point position{bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};

  runtime.HandlePointerEvent({PointerEventType::Down, 801, position});
  runtime.HandlePointerEvent({PointerEventType::Cancel, 801, position});
  runtime.HandlePointerEvent({PointerEventType::Up, 801, position});

  REQUIRE_FALSE(HasRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List));
}

TEST_CASE("SelectKeyboardSkipsDisabledItemsAndCancelDoesNotChangeSelection") {
  select_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  OpenSelect(runtime);
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowDown});
  runtime.BuildCommit();
  const std::shared_ptr<const SemanticFrame> moved = runtime.LastCommit().semantic_frame;
  const SemanticNode& moved_list = FindRole(*moved, SemanticRole::List);
  REQUIRE(ListItemAt(*moved, moved_list, 2).focused);
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Enter});
  REQUIRE(select_changes == 1);
  REQUIRE(select_index.Get() == 2);
  runtime.BuildCommit();

  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowDown});
  runtime.BuildCommit();
  REQUIRE(FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::List).children.size() == 3);
  REQUIRE(runtime.HandleBack());
  runtime.BuildCommit();
  REQUIRE(select_changes == 1);
  REQUIRE(select_index.Get() == 2);
  REQUIRE(FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::ComboBox).expanded == false);
}

TEST_CASE("SelectPreservesAKeyedActiveItemAcrossReordering") {
  select_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  OpenSelect(runtime);
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::End});
  runtime.BuildCommit();
  const SemanticNode& before_list = FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::List);
  const SemanticNodeId active_item_id = ListItemAt(*runtime.LastCommit().semantic_frame, before_list, 2).id;

  select_options = std::vector<SelectOption>{
      {3, "Three", true},
      {1, "One", true},
      {2, "Two", false},
  };
  select_index = 1;
  runtime.BuildCommit();
  runtime.BuildCommit();
  runtime.BuildCommit();
  const SemanticNode& after_list = FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::List);
  REQUIRE(ListItemAt(*runtime.LastCommit().semantic_frame, after_list, 0).id == active_item_id);
  REQUIRE(ListItemAt(*runtime.LastCommit().semantic_frame, after_list, 0).focused);

  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Enter});
  REQUIRE(select_changes == 1);
  REQUIRE(select_index.Get() == 0);
}

TEST_CASE("SelectFallsBackWhenItsActiveKeyedItemIsRemoved") {
  select_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{SelectApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  OpenSelect(runtime);
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::End});
  runtime.BuildCommit();
  const SemanticNode& before_list = FindRole(*runtime.LastCommit().semantic_frame, SemanticRole::List);
  REQUIRE(ListItemAt(*runtime.LastCommit().semantic_frame, before_list, 2).focused);

  select_options = std::vector<SelectOption>{{1, "One", true}, {2, "Two", false}};
  select_index = 0;
  runtime.BuildCommit();
  runtime.BuildCommit();
  runtime.BuildCommit();
  const std::shared_ptr<const SemanticFrame> after = runtime.BuildCommit().semantic_frame;
  const SemanticNode& after_list = FindRole(*after, SemanticRole::List);
  REQUIRE(after_list.children.size() == 2);
  REQUIRE(ListItemAt(*after, after_list, 0).focused);
  REQUIRE(select_changes == 0);
}

} // namespace huxerui::test
