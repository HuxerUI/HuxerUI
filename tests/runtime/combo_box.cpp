#include "runtime_test_support.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace huxerui::test {

namespace {

struct Suggestion {
  std::string text;
  bool enabled = true;

  bool operator==(const Suggestion&) const = default;
};

State<TextEditingValue> combo_box_value;
State<std::vector<Suggestion>> combo_box_suggestions;
State<bool> combo_box_visible;
State<TextEditingValue> observable_combo_box_value;
State<bool> observable_combo_box_enabled;
State<bool> observable_combo_box_visible;
std::vector<TextEditingValue> combo_box_changes;
std::vector<std::pair<std::size_t, TextEditingValue>> combo_box_selections;
std::vector<bool> combo_box_expansion_changes;
std::vector<std::string> combo_box_event_order;
int combo_box_submissions = 0;
int invalid_combo_box_style_case = 0;

View ComboBoxApp() {
  combo_box_value = UseState(TextEditingValue::FromText(""));
  combo_box_suggestions = UseState(std::vector<Suggestion>{{"Alpha"}, {"Beta", false}, {"Gamma"}});
  return ComboBox(
             combo_box_value, combo_box_suggestions.Get(), [](const Suggestion& suggestion) { return suggestion.text; },
             [](const Suggestion& suggestion) {
               return Text(suggestion.text).Key(suggestion.text).With(Enabled{suggestion.enabled});
             }
         )
      .Label("Language")
      .Placeholder("Search")
      .OnChanged([](const TextEditingValue& value) {
        combo_box_changes.push_back(value);
        combo_box_value = value;
      })
      .OnSelected([](std::size_t index, const TextEditingValue& value) {
        combo_box_selections.emplace_back(index, value);
        combo_box_value = value;
      })
      .OnSubmitted([] { ++combo_box_submissions; })
      .With(Frame{.width = 220.0F});
}

View EmptyComboBoxApp() {
  return ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{})
      .Label("Language")
      .EmptyContent([] { return Text("No suggestions").With(Padding{8.0F}); })
      .With(Frame{.width = 220.0F});
}

View EmptyComboBoxWithoutContentApp() {
  return ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{})
      .Label("Language")
      .With(Frame{.width = 220.0F});
}

View ConditionalComboBoxApp() {
  combo_box_visible = UseState(true);
  if (!combo_box_visible.Get()) {
    return Text("Replacement");
  }
  return ComboBox(TextEditingValue::FromText(""), {"Alpha", "Beta"}).Label("Language");
}

VectorAsset ComboBoxTrailingIcon() {
  static const VectorAsset icon = VectorAsset::Create({12.0F, 8.0F}, [](VectorBuilder& builder) {
    Path path;
    path.MoveTo({1.0F, 1.0F}).LineTo({11.0F, 1.0F}).LineTo({6.0F, 7.0F}).Close();
    builder.FillPath(std::move(path), Color::Black());
  });
  return icon;
}

View DefaultComboBoxIconApp() {
  return ComboBox(TextEditingValue::FromText(""), {"Alpha"}).With(Frame{.width = 220.0F});
}

View CustomComboBoxIconApp() {
  return ComboBox(TextEditingValue::FromText(""), {"Alpha"})
      .TrailingIcon(ComboBoxTrailingIcon())
      .With(Frame{.width = 220.0F});
}

View ObservableComboBoxApp() {
  observable_combo_box_value = UseState(TextEditingValue::FromText(""));
  observable_combo_box_enabled = UseState(true);
  return ComboBox(observable_combo_box_value, {"Alpha", "Beta"})
      .OnChanged([](const TextEditingValue& value) { observable_combo_box_value = value; })
      .OnSelected([](std::size_t, const TextEditingValue& value) {
        combo_box_event_order.push_back("selected");
        observable_combo_box_value = value;
      })
      .OnSubmitted([] { combo_box_event_order.push_back("submitted"); })
      .OnExpandedChanged([](bool expanded) {
        combo_box_expansion_changes.push_back(expanded);
        combo_box_event_order.push_back(expanded ? "expanded" : "collapsed");
      })
      .With(Frame{.width = 220.0F}, Enabled{observable_combo_box_enabled.Get()});
}

View ObservableConditionalComboBoxApp() {
  observable_combo_box_visible = UseState(true);
  if (!observable_combo_box_visible.Get()) {
    return Text("Replacement");
  }
  return ComboBox(TextEditingValue::FromText(""), {"Alpha"})
      .OnExpandedChanged([](bool expanded) { combo_box_expansion_changes.push_back(expanded); })
      .With(Frame{.width = 220.0F});
}

View EmptySuggestionViewApp() {
  return ComboBox(
      TextEditingValue::FromText(""), std::vector<std::string>{"Alpha"}, [](const std::string& value) { return value; },
      [](const std::string&) { return View{}; }
  );
}

View InteractiveSuggestionViewApp() {
  return ComboBox(
      TextEditingValue::FromText(""), std::vector<std::string>{"Alpha"}, [](const std::string& value) { return value; },
      [](const std::string& value) { return Button(value); }
  );
}

View InteractiveEmptyContentApp() {
  return ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{})
      .EmptyContent([] { return Button("Retry"); });
}

View InvalidComboBoxStyleApp() {
  ComboBoxStyle style = ComboBoxStyle::Default();
  if (invalid_combo_box_style_case == 0) {
    style.item_padding.left = -1.0F;
  } else if (invalid_combo_box_style_case == 1) {
    style.maximum_popup_height = 0.0F;
  } else {
    style.popup_shadow.blur_radius = -1.0F;
  }
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    ComboBox(TextEditingValue::FromText(""), {"Alpha"}),
  };
}

const SemanticNode& FindRole(const SemanticFrame& frame, SemanticRole role) {
  const auto found = std::ranges::find(frame.nodes, role, &SemanticNode::role);
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

void FocusComboBox(Runtime& runtime) {
  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const Rect bounds = FindRole(*frame, SemanticRole::ComboBox).bounds;
  ClickAt(runtime, {bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F});
  runtime.BuildCommit();
  runtime.BuildCommit();
}

} // namespace

TEST_CASE("ComboBoxValidatesItsEditingAndFactoryContracts") {
  TextEditingValue invalid = TextEditingValue::FromText("value");
  invalid.selection.active = 99;
  REQUIRE_THROWS_AS(ComboBox(invalid, std::vector<std::string>{"value"}), std::invalid_argument);

  TextInputConfiguration configuration;
  configuration.multiline = true;
  REQUIRE_THROWS_AS(
      std::move(ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{"value"}))
          .InputConfiguration(configuration),
      std::invalid_argument
  );
  configuration.multiline = false;
  configuration.read_only = true;
  REQUIRE_THROWS_AS(
      std::move(ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{"value"}))
          .InputConfiguration(configuration),
      std::invalid_argument
  );
  configuration.read_only = false;
  configuration.secure = true;
  REQUIRE_THROWS_AS(
      std::move(ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{"value"}))
          .InputConfiguration(configuration),
      std::invalid_argument
  );
  configuration.secure = false;
  configuration.action = TextInputAction::Newline;
  REQUIRE_THROWS_AS(
      std::move(ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{"value"}))
          .InputConfiguration(configuration),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      std::move(ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{"value"}))
          .EmptyContent(std::function<View()>{}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      std::move(ComboBox(TextEditingValue::FromText(""), std::vector<std::string>{"value"}))
          .TrailingIcon(VectorAsset{}),
      std::invalid_argument
  );
}

TEST_CASE("ComboBoxRejectsEmptyOrIndependentlyInteractivePopupContent") {
  TestPlatform platform{BuiltinTestResources()};
  for (const auto app : {EmptySuggestionViewApp, InteractiveSuggestionViewApp, InteractiveEmptyContentApp}) {
    Runtime runtime{app, platform};
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
    const Rect field = FindRole(*frame, SemanticRole::ComboBox).bounds;
    ClickAt(runtime, {field.x + field.width * 0.5F, field.y + field.height * 0.5F});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
}

TEST_CASE("ComboBoxProvidesFlatAndMaterialPopupStyles") {
  const ThemeSpec flat_light_spec = FlatLightThemeSpec();
  const ThemeSpec flat_dark_spec = FlatDarkThemeSpec();
  const ComboBoxStyle flat_light = detail::DefaultComboBoxStyle(flat_light_spec);
  const ComboBoxStyle flat_dark = detail::DefaultComboBoxStyle(flat_dark_spec);
  const ThemeSpec material_light_spec = MaterialLightThemeSpec();
  const ThemeSpec material_dark_spec = MaterialDarkThemeSpec();
  const ComboBoxStyle material_light = ThemeDefinitionValue<ComboBoxStyle>(MaterialThemeDefinition());
  const ComboBoxStyle material_dark = ThemeDefinitionValue<ComboBoxStyle>(MaterialDarkThemeDefinition());

  REQUIRE(flat_light.popup_background == flat_light_spec.colors.surface);
  REQUIRE(flat_dark.foreground == flat_dark_spec.colors.on_surface);
  REQUIRE_FALSE(flat_light.item_indication.has_value());
  REQUIRE(material_light.popup_background == material_light_spec.colors.surface_container);
  REQUIRE(material_dark.foreground == material_dark_spec.colors.on_surface);
  REQUIRE(material_light.item_indication.has_value());
}

TEST_CASE("ComboBoxUsesItsDefaultOrDeclaredTrailingIcon") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime default_icon{DefaultComboBoxIconApp, platform};
  default_icon.SetWindowMetrics({.viewport = {260.0F, 100.0F}});
  const FlattenedScene& default_scene = default_icon.BuildFrame();

  Runtime custom_icon{CustomComboBoxIconApp, platform};
  custom_icon.SetWindowMetrics({.viewport = {260.0F, 100.0F}});
  const FlattenedScene& custom_scene = custom_icon.BuildFrame();
  const Color tint = TextFieldStyle::Default().trailing_icon;

  REQUIRE(std::ranges::any_of(default_scene.Commands(), [tint](const PaintCommand& command) {
    const auto* path = std::get_if<StrokePathCommand>(&command);
    return path && BrushIsColor(path->brush, tint);
  }));
  REQUIRE(std::ranges::any_of(custom_scene.Commands(), [tint](const PaintCommand& command) {
    const auto* path = std::get_if<FillPathCommand>(&command);
    return path && BrushIsColor(path->brush, tint);
  }));
}

TEST_CASE("ComboBoxRejectsInvalidThemeGeometry") {
  TestPlatform platform{BuiltinTestResources()};
  for (invalid_combo_box_style_case = 0; invalid_combo_box_style_case < 3; ++invalid_combo_box_style_case) {
    Runtime runtime{InvalidComboBoxStyleApp, platform};
    runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
}

TEST_CASE("ComboBoxReusesControlledTextEditingAndPublishesEditableSemantics") {
  combo_box_changes.clear();
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  FocusComboBox(runtime);
  const std::shared_ptr<const SemanticFrame> expanded = runtime.LastCommit().semantic_frame;
  const SemanticNode& field = FindRole(*expanded, SemanticRole::ComboBox);
  REQUIRE(field.label == "Language");
  REQUIRE(field.placeholder == "Search");
  REQUIRE(field.read_only == false);
  REQUIRE(field.expanded == true);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetText)) != 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetSelection)) != 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::Collapse)) != 0);

  TextInputCommand insert;
  insert.kind = TextInputCommandKind::CommitText;
  insert.text = "a";
  REQUIRE(runtime.HandleTextInputCommands({1, {insert}}).result_code == TextInputResultCode::Ok);
  REQUIRE(combo_box_changes.size() == 1);
  REQUIRE(combo_box_changes.back().text == "a");

  const SemanticNode& updated = FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox);
  REQUIRE(updated.value == "a");
  REQUIRE(updated.expanded == true);
}

TEST_CASE("ComboBoxKeyboardNavigationSkipsDisabledSuggestionsAndProposesSelection") {
  combo_box_changes.clear();
  combo_box_selections.clear();
  combo_box_submissions = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(runtime);

  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowDown, .modifiers = {.shift = true}});
  const std::shared_ptr<const SemanticFrame> unchanged = runtime.BuildCommit().semantic_frame;
  const SemanticNode& unchanged_list = FindRole(*unchanged, SemanticRole::List);
  REQUIRE(std::ranges::none_of(unchanged_list.children, [&unchanged](SemanticNodeId id) {
    return FindNode(*unchanged, id).selected.value_or(false);
  }));

  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowDown});
  runtime.BuildCommit();
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowDown});
  const std::shared_ptr<const SemanticFrame> moved = runtime.BuildCommit().semantic_frame;
  const SemanticNode& list = FindRole(*moved, SemanticRole::List);
  REQUIRE(ListItemAt(*moved, list, 0).selected == false);
  REQUIRE_FALSE(ListItemAt(*moved, list, 1).enabled);
  REQUIRE(ListItemAt(*moved, list, 2).selected == true);

  REQUIRE_FALSE(runtime.HandleKeyEvent({
      .type = KeyEventType::Down,
      .key = Key::Enter,
      .modifiers = {.alt = true},
  }));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter, .repeat = true}));
  REQUIRE(combo_box_selections.empty());
  REQUIRE(combo_box_submissions == 0);

  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(combo_box_selections.size() == 1);
  REQUIRE(combo_box_selections.back().first == 2);
  REQUIRE(combo_box_selections.back().second == TextEditingValue::FromText("Gamma"));
  REQUIRE(combo_box_changes.empty());

  const SemanticNode& collapsed = FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox);
  REQUIRE(collapsed.value == "Gamma");
  REQUIRE(collapsed.expanded == false);
  REQUIRE(collapsed.focused);
}

TEST_CASE("ComboBoxPreservesSuggestionKeysAcrossReordering") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(runtime);

  const std::shared_ptr<const SemanticFrame> initial = runtime.LastCommit().semantic_frame;
  const SemanticNode& initial_list = FindRole(*initial, SemanticRole::List);
  const SemanticNodeId alpha_id = ListItemAt(*initial, initial_list, 0).id;

  combo_box_suggestions = std::vector<Suggestion>{{"Gamma"}, {"Alpha"}, {"Beta", false}};
  runtime.BuildCommit();
  const std::shared_ptr<const SemanticFrame> reordered = runtime.BuildCommit().semantic_frame;
  const SemanticNode& reordered_list = FindRole(*reordered, SemanticRole::List);
  const SemanticNode& alpha = ListItemAt(*reordered, reordered_list, 1);
  REQUIRE(alpha.label == "Alpha");
  REQUIRE(alpha.id == alpha_id);
}

TEST_CASE("ComboBoxPointerSelectionUsesTheSameControlledProposal") {
  combo_box_changes.clear();
  combo_box_selections.clear();
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(runtime);

  const std::shared_ptr<const SemanticFrame> expanded = runtime.LastCommit().semantic_frame;
  const SemanticNode& list = FindRole(*expanded, SemanticRole::List);
  const Rect item = ListItemAt(*expanded, list, 2).bounds;
  const Point position{item.x + item.width * 0.5F, item.y + item.height * 0.5F};
  runtime.HandlePointerEvent({PointerEventType::Down, 913, position});
  REQUIRE(FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox).focused);
  runtime.HandlePointerEvent({PointerEventType::Up, 913, position});

  REQUIRE(combo_box_selections.size() == 1);
  REQUIRE(combo_box_selections.back().first == 2);
  REQUIRE(combo_box_selections.back().second == TextEditingValue::FromText("Gamma"));
  REQUIRE(combo_box_changes.empty());
  REQUIRE_FALSE(HasRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List));
}

TEST_CASE("ComboBoxOutsidePressDismissesAndReleasesFieldFocus") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(runtime);

  ClickAt(runtime, {319.0F, 1.0F});
  const std::shared_ptr<const SemanticFrame> dismissed = runtime.BuildCommit().semantic_frame;
  REQUIRE_FALSE(FindRole(*dismissed, SemanticRole::ComboBox).focused);
  REQUIRE_FALSE(HasRole(*dismissed, SemanticRole::List));
}

TEST_CASE("ComboBoxPreservesImeCompositionBeforeSuggestionNavigation") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(runtime);

  TextInputCommand begin;
  begin.kind = TextInputCommandKind::BeginComposition;
  begin.target = TextRange{0, 0};
  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "a";
  update.selection_after = TextSelection{1, 1};
  REQUIRE(runtime.HandleTextInputCommands({1, {begin, update}}).result_code == TextInputResultCode::Ok);
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowDown});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& list = FindRole(*frame, SemanticRole::List);
  REQUIRE(std::ranges::none_of(list.children, [&frame](SemanticNodeId id) {
    return FindNode(*frame, id).selected.value_or(false);
  }));
}

TEST_CASE("ComboBoxSubmitsWithoutAnActiveSuggestionAndDismissesExplicitly") {
  combo_box_submissions = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(runtime);

  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(combo_box_submissions == 1);
  REQUIRE_FALSE(HasRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List));

  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowDown}));
  runtime.BuildCommit();
  REQUIRE(HasRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List));
  REQUIRE_FALSE(runtime.HandleKeyEvent({
      .type = KeyEventType::Down,
      .key = Key::Escape,
      .modifiers = {.alt = true},
  }));
  REQUIRE(HasRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Escape}));
  REQUIRE_FALSE(HasRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List));
}

TEST_CASE("ComboBoxExpansionEventsFollowKeyboardSelectionAndSubmission") {
  combo_box_expansion_changes.clear();
  combo_box_event_order.clear();
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ObservableComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  FocusComboBox(runtime);
  runtime.BuildCommit();
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true});

  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Escape}));
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true, false});

  TextInputCommand edit;
  edit.kind = TextInputCommandKind::CommitText;
  edit.text = "a";
  REQUIRE(runtime.HandleTextInputCommands({1, {edit}}).result_code == TextInputResultCode::Ok);
  runtime.BuildCommit();
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowDown}));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true, false, true, false});
  REQUIRE(combo_box_event_order ==
          std::vector<std::string>{"expanded", "collapsed", "expanded", "collapsed", "selected"});

  const SemanticNodeId field = FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox).id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field, {SemanticActionKind::Expand, std::monostate{}}
  ));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true, false, true, false, true, false});
  REQUIRE(combo_box_event_order.back() == "submitted");
  REQUIRE(combo_box_event_order[combo_box_event_order.size() - 2] == "collapsed");
}

TEST_CASE("ComboBoxExpansionEventsFollowSemanticsOutsideDismissalAndDisable") {
  combo_box_expansion_changes.clear();
  combo_box_event_order.clear();
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ObservableComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const SemanticNodeId field = FindRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ComboBox).id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field, {SemanticActionKind::Expand, std::monostate{}}
  ));
  runtime.BuildCommit();
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field, {SemanticActionKind::Collapse, std::monostate{}}
  ));
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true, false});

  FocusComboBox(runtime);
  ClickAt(runtime, {319.0F, 1.0F});
  runtime.BuildCommit();
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true, false, true, false});

  FocusComboBox(runtime);
  observable_combo_box_enabled = false;
  runtime.BuildFrame();
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true, false, true, false, true, false});
}

TEST_CASE("ComboBoxExpansionEventClosesOnUnmount") {
  combo_box_expansion_changes.clear();
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ObservableConditionalComboBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  FocusComboBox(runtime);
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true});
  observable_combo_box_visible = false;
  runtime.BuildCommit();
  runtime.BuildCommit();
  REQUIRE(combo_box_expansion_changes == std::vector<bool>{true, false});
}

TEST_CASE("ComboBoxEmptyContentAndUnmountFollowPopupLifetime") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime empty{EmptyComboBoxApp, platform};
  empty.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(empty);
  REQUIRE(HasRole(*empty.LastCommit().semantic_frame, SemanticRole::List));
  REQUIRE(FindText(empty.BuildFrame(), "No suggestions") != nullptr);

  Runtime absent{EmptyComboBoxWithoutContentApp, platform};
  absent.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(absent);
  REQUIRE_FALSE(HasRole(*absent.LastCommit().semantic_frame, SemanticRole::List));
  REQUIRE((FindRole(*absent.LastCommit().semantic_frame, SemanticRole::ComboBox).actions &
           SemanticActionMask(SemanticActionKind::Expand)) == 0);

  Runtime conditional{ConditionalComboBoxApp, platform};
  conditional.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  FocusComboBox(conditional);
  REQUIRE(HasRole(*conditional.LastCommit().semantic_frame, SemanticRole::List));
  combo_box_visible = false;
  conditional.BuildCommit();
  const std::shared_ptr<const SemanticFrame> replaced = conditional.BuildCommit().semantic_frame;
  REQUIRE_FALSE(HasRole(*replaced, SemanticRole::ComboBox));
  REQUIRE_FALSE(HasRole(*replaced, SemanticRole::List));
}

} // namespace huxerui::test
