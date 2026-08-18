#include "runtime_test_support.h"

#include <type_traits>

namespace huxerui::test {

namespace {

int semantic_button_clicks = 0;
int semantic_icon_button_clicks = 0;
int semantic_lifecycle_clicks = 0;
int semantic_segmented_button_changes = 0;
int semantic_virtual_list_clicks = 0;
int semantic_virtual_list_factory_calls = 0;
int virtual_semantic_activations = 0;
State<float> semantic_slider_value;
State<bool> semantic_alternate_content;
State<bool> semantic_alternate_label;
State<bool> semantic_virtual_visible;
State<std::size_t> semantic_segmented_button_selection;
State<std::size_t> semantic_tabs_selection;
State<std::size_t> semantic_navigation_selection;
State<bool> semantic_navigation_expanded;
State<TextEditingValue> semantic_text_field_value;
State<TextEditingValue> semantic_secure_text_field_value;

static_assert(!std::is_copy_constructible_v<SemanticBuilder>);
static_assert(!std::is_move_constructible_v<SemanticBuilder>);

struct VirtualSemantics;

class VirtualSemanticsExtension final : public NodeExtension {
public:
  VirtualSemanticsExtension(MountedNode& node, const VirtualSemantics& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const VirtualSemantics& modifier) {
    static_cast<void>(node);
    static_cast<void>(modifier);
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    builder.SetOwner(Semantics{.label = "Chart"});
    builder.AddChild(
        7,
        {8.0F, 6.0F, 24.0F, 12.0F},
        Semantics{
            .role = SemanticRole::Button,
            .label = "April",
        }
    );
    builder.AddAction(7, SemanticActionKind::Activate);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 7 || action.kind != SemanticActionKind::Activate) {
      return false;
    }
    ++virtual_semantic_activations;
    return true;
  }
};

struct VirtualSemantics {
  using Extension = VirtualSemanticsExtension;

  bool operator==(const VirtualSemantics&) const = default;
};

VectorAsset SemanticActionIcon() {
  static const VectorAsset icon = VectorAsset::Create({12.0F, 12.0F}, [](VectorBuilder& builder) {
    builder.FillPath(
        Path{}.MoveTo({1.0F, 1.0F}).LineTo({11.0F, 6.0F}).LineTo({1.0F, 11.0F}).Close(),
        Color::Black()
    );
  });
  return icon;
}

View SemanticBasicsApp() {
  auto slider = UseState(0.25F);
  semantic_slider_value = slider;
  return Column {
    Text("Status"),
    Button("Continue").OnClick([] { ++semantic_button_clicks; }),
    IconButton(SemanticActionIcon(), "Play").OnClick([] { ++semantic_icon_button_clicks; }),
    Checkbox("Remember", true),
    Slider(slider.Get()).Range(0.0F, 10.0F).Step(0.5F).OnChanged([slider](float value) mutable {
      slider = value;
    }),
  };
}

View SemanticProgressApp() {
  return Column {
    ProgressCircle(),
    ProgressCircle(0.25F),
    ProgressBar(),
    ProgressBar(0.75F),
  };
}

View SemanticOverrideApp() {
  return Canvas([](PaintContext&, Size) {}).With(
      Frame{40.0F, 20.0F},
      Semantics{
          .role = SemanticRole::Image,
          .label = "Revenue chart",
      }
  );
}

View SecureSemanticTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("secret"));
  semantic_secure_text_field_value = value;
  return TextField(value)
      .Label("Password")
      .InputConfiguration({.secure = true})
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; });
}

View EditableSemanticTextFieldApp() {
  auto value = UseState(
      TextEditingValue::FromText(
          "a\xF0\x9F\x98\x80"
          "b"
      )
  );
  semantic_text_field_value = value;
  return TextField(value)
      .Label("Editor")
      .MaxLength(4)
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; });
}

View ReadOnlySemanticTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("readonly"));
  semantic_text_field_value = value;
  return TextField(value)
      .Label("Read only")
      .InputConfiguration({.read_only = true})
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; });
}

View VirtualSemanticApp() {
  return Canvas([](PaintContext&, Size) {}).With(VirtualSemantics{});
}

View VirtualSemanticLifecycleApp() {
  auto visible = UseState(true);
  semantic_virtual_visible = visible;
  View canvas = Canvas([](PaintContext&, Size) {});
  if (visible.Get()) {
    return std::move(canvas).With(VirtualSemantics{});
  }
  return std::move(canvas).With(Semantics{.label = "Fallback"});
}

View EmptySemanticApp() {
  return Canvas([](PaintContext&, Size) {}).With(Semantics{});
}

View SemanticPrecedenceApp() {
  return Button("Component").OnClick([] {}).With(VirtualSemantics{}, Semantics{.label = "Author"});
}

View SemanticLifecycleApp() {
  auto alternate = UseState(false);
  semantic_alternate_content = alternate;
  View active = alternate.Get()
                    ? Button("Replacement").OnClick([] { ++semantic_lifecycle_clicks; }).Key("replacement")
                    : Button("Primary").OnClick([] { ++semantic_lifecycle_clicks; }).Key("primary");
  return Column {
    std::move(active),
    Button("Disabled").OnClick([] { ++semantic_lifecycle_clicks; }).With(Enabled{false}),
    Column {
      Text("Excluded"),
    }.With(Semantics{
        .label = "Owner",
        .descendants = SemanticDescendantPolicy::Exclude,
    }),
    Text("Hidden").With(Semantics{.hidden = true}),
  };
}

View SemanticCompatibleUpdateApp() {
  auto alternate = UseState(false);
  semantic_alternate_label = alternate;
  return Canvas([](PaintContext&, Size) {}).With(Semantics{.label = alternate.Get() ? "After" : "Before"});
}

View SemanticTabsApp() {
  auto selected = UseState<std::size_t>(0);
  semantic_tabs_selection = selected;
  return Tabs(
             std::vector<TabItem>{
                 TabItem("Overview"),
                 TabItem::IconOnly(SemanticActionIcon(), "Activity"),
                 std::move(TabItem("Disabled")).Enabled(false),
             },
             selected
  )
      .OnChanged([selected](std::size_t index) mutable { selected = index; });
}

View SemanticSegmentedButtonApp() {
  auto selected = UseState<std::size_t>(0);
  semantic_segmented_button_selection = selected;
  return SegmentedButton(
             std::vector<SegmentedButtonItem>{
                 SegmentedButtonItem("Day"),
                 SegmentedButtonItem("Week"),
                 SegmentedButtonItem::IconOnly(SemanticActionIcon(), "Month"),
             },
             selected
  )
      .OnChanged([selected](std::size_t index) mutable {
        ++semantic_segmented_button_changes;
        selected = index;
      });
}

View DisabledSemanticSegmentedButtonApp() {
  return SegmentedButton({"Day", "Week"}, 0)
      .OnChanged([](std::size_t) { ++semantic_segmented_button_changes; })
      .With(Enabled{false});
}

View SemanticNavigationBarApp() {
  auto selected = UseState<std::size_t>(0);
  semantic_navigation_selection = selected;
  return huxerui::NavigationBar(
             {
                 huxerui::NavigationItem(SemanticActionIcon(), "Home"),
                 huxerui::NavigationItem(SemanticActionIcon(), "Library"),
                 std::move(huxerui::NavigationItem(SemanticActionIcon(), "Disabled")).Enabled(false),
             },
             selected
  )
      .OnChanged([selected](std::size_t index) mutable { selected = index; });
}

View SemanticNavigationPaneApp() {
  auto expanded = UseState(false);
  semantic_navigation_expanded = expanded;
  return huxerui::NavigationPane(
      {
          huxerui::NavigationItem(SemanticActionIcon(), "Home"),
          huxerui::NavigationItem(SemanticActionIcon(), "Library"),
      },
      0,
      expanded.Get()
  );
}

View SemanticVerticalScrollApp() {
  return ScrollView {
    Column {
      Text("First").With(Frame{100.0F, 40.0F}),
      Text("Second").With(Frame{100.0F, 40.0F}),
      Text("Third").With(Frame{100.0F, 40.0F}),
    },
  };
}

View SemanticHorizontalScrollApp() {
  return ScrollView {
    Row {
      Text("Left").With(Frame{60.0F, 40.0F}),
      Text("Center").With(Frame{60.0F, 40.0F}),
      Text("Right").With(Frame{60.0F, 40.0F}),
    },
  }.ScrollAxis(Axis::Horizontal);
}

View SemanticVirtualListApp() {
  return VirtualList(100, [](std::size_t index) {
    ++semantic_virtual_list_factory_calls;
    return Button("Item " + std::to_string(index)).OnClick([index] {
      semantic_virtual_list_clicks += static_cast<int>(index + 1);
    });
  })
      .ItemExtent(20.0F)
      .CacheExtent(40.0F);
}

View SemanticHorizontalVirtualListApp() {
  return VirtualList(5, [](std::size_t index) { return Text("Horizontal " + std::to_string(index)); })
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(30.0F)
      .CacheExtent(0.0F);
}

View SemanticVirtualGridApp() {
  return VirtualGrid(8, [](std::size_t index) {
    std::string label = "Cell " + std::to_string(index);
    return Column {
      Text(label),
    }.With(Semantics{
        .label = std::move(label),
        .descendants = SemanticDescendantPolicy::Exclude,
    });
  })
      .Columns(GridColumns::Adaptive(30.0F))
      .RowExtent(20.0F)
      .CacheExtent(0.0F)
      .ItemSpans({2, 1, 1, 2});
}

View SemanticEmptyVirtualListApp() {
  return VirtualList(0, [](std::size_t) { return Text("Unrealized"); }).ItemExtent(20.0F);
}

View SemanticNestedScrollApp() {
  return ScrollView {
    Column {
      Text("Outer start").With(Frame{100.0F, 40.0F}),
      ScrollView {
        Column {
          Text("Inner first").With(Frame{100.0F, 40.0F}),
          Text("Inner second").With(Frame{100.0F, 40.0F}),
          Text("Inner third").With(Frame{100.0F, 40.0F}),
        },
      }.With(Frame{100.0F, 60.0F}),
      Text("Outer end").With(Frame{100.0F, 40.0F}),
    },
  };
}

View SemanticClippedTransformApp() {
  return Column {
    Stack {
      Text("Clipped").With(Frame{80.0F, 20.0F}, Offset{Point{0.0F, 50.0F}}),
    }.With(Frame{100.0F, 40.0F}, ClipChildren{}),
  };
}

const SemanticNode& FindSemanticNode(const SemanticFrame& frame, std::string_view label) {
  const auto found = std::ranges::find(frame.nodes, label, &SemanticNode::label);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

const SemanticNode* FindSemanticNodeOrNull(const SemanticFrame& frame, std::string_view label) {
  const auto found = std::ranges::find(frame.nodes, label, &SemanticNode::label);
  return found == frame.nodes.end() ? nullptr : &*found;
}

const SemanticNode& FindSemanticRole(const SemanticFrame& frame, SemanticRole role) {
  const auto found = std::ranges::find(frame.nodes, role, &SemanticNode::role);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

std::size_t SemanticLabelCount(const SemanticFrame& frame, std::string_view label) {
  return static_cast<std::size_t>(std::ranges::count(frame.nodes, label, &SemanticNode::label));
}

SemanticCollection ItemCollection(std::size_t item_count) {
  SemanticCollection collection;
  collection.item_count = item_count;
  return collection;
}

SemanticCollectionItem CollectionItem(std::size_t index) {
  SemanticCollectionItem item;
  item.index = index;
  return item;
}

} // namespace

TEST_CASE("SemanticFramePublishesBuiltInComponentMeaningAndReusesUnchangedData") {
  semantic_button_clicks = 0;
  semantic_icon_button_clicks = 0;
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime(SemanticBasicsApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const FrameCommit& first = runtime.BuildCommit();
  REQUIRE(first.semantic_frame);
  REQUIRE(first.semantic_frame->revision != 0);
  REQUIRE(first.semantic_frame->root != 0);
  const std::shared_ptr<const SemanticFrame> first_frame = first.semantic_frame;

  const SemanticNode& text = FindSemanticNode(*first_frame, "Status");
  REQUIRE(text.role == SemanticRole::Text);
  const SemanticNode& button = FindSemanticNode(*first_frame, "Continue");
  REQUIRE(button.role == SemanticRole::Button);
  REQUIRE((button.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);
  const SemanticNode& icon_button = FindSemanticNode(*first_frame, "Play");
  REQUIRE(icon_button.role == SemanticRole::Button);
  REQUIRE((icon_button.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);
  const SemanticNode& checkbox = FindSemanticNode(*first_frame, "Remember");
  REQUIRE(checkbox.checked == SemanticCheckedState::Checked);

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(button.id, {SemanticActionKind::Activate, std::monostate{}}));
  REQUIRE(semantic_button_clicks == 1);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      icon_button.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_icon_button_clicks == 1);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(button.id, {SemanticActionKind::Activate, 1.0}));

  const FrameCommit& unchanged = runtime.BuildCommit();
  REQUIRE(unchanged.semantic_frame == first_frame);
}

TEST_CASE("IndeterminateProgressPublishesLocalizedBusyState") {
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime(SemanticProgressApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  std::vector<const SemanticNode*> progress_nodes;
  for (const SemanticNode& node : frame->nodes) {
    if (node.role == SemanticRole::ProgressIndicator) {
      progress_nodes.push_back(&node);
    }
  }
  REQUIRE(progress_nodes.size() == 4);
  REQUIRE(progress_nodes[0]->busy == true);
  REQUIRE(progress_nodes[0]->state_description == "In progress");
  REQUIRE_FALSE(progress_nodes[0]->range.has_value());
  REQUIRE(progress_nodes[1]->busy == false);
  REQUIRE(progress_nodes[1]->state_description.empty());
  REQUIRE(progress_nodes[1]->range == SemanticRange{0.0, 1.0, 0.25, std::nullopt});
  REQUIRE(progress_nodes[2]->busy == true);
  REQUIRE(progress_nodes[2]->state_description == "In progress");
  REQUIRE_FALSE(progress_nodes[2]->range.has_value());
  REQUIRE(progress_nodes[3]->busy == false);
  REQUIRE(progress_nodes[3]->state_description.empty());
  REQUIRE(progress_nodes[3]->range == SemanticRange{0.0, 1.0, 0.75, std::nullopt});
}

TEST_CASE("SemanticActionsRouteToRetainedControlBehavior") {
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime(SemanticBasicsApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  runtime.BuildCommit();
  const auto slider_node = std::ranges::find_if(runtime.LastCommit().semantic_frame->nodes, [](const SemanticNode& node) {
    return node.role == SemanticRole::Slider;
  });
  REQUIRE(slider_node != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      slider_node->id,
      {SemanticActionKind::SetValue, 7.5}
  ));
  REQUIRE(semantic_slider_value.Get() == 7.5F);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      slider_node->id,
      {SemanticActionKind::SetValue, std::string("7.5")}
  ));

  const SemanticNodeId slider_id = slider_node->id;
  const std::shared_ptr<const SemanticFrame> updated = runtime.BuildCommit().semantic_frame;
  const auto updated_slider = std::ranges::find(updated->nodes, slider_id, &SemanticNode::id);
  REQUIRE(updated_slider != updated->nodes.end());
  REQUIRE(updated_slider->range.has_value());
  REQUIRE(updated_slider->range->current == 7.5);
}

TEST_CASE("TabsPublishAStableAccessibleSelectionGroup") {
  TestPlatform platform;
  Runtime runtime(SemanticTabsApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& tab_list = FindSemanticRole(*before, SemanticRole::TabList);
  REQUIRE(tab_list.collection == ItemCollection(3));
  REQUIRE(tab_list.children.size() == 3);
  REQUIRE((tab_list.actions & SemanticActionMask(SemanticActionKind::Focus)) != 0);

  const SemanticNode& overview = FindSemanticNode(*before, "Overview");
  const SemanticNode& activity = FindSemanticNode(*before, "Activity");
  const SemanticNode& disabled = FindSemanticNode(*before, "Disabled");
  REQUIRE(overview.role == SemanticRole::Tab);
  REQUIRE(overview.selected == true);
  REQUIRE(overview.collection_item == CollectionItem(0));
  REQUIRE(activity.role == SemanticRole::Tab);
  REQUIRE(activity.selected == false);
  REQUIRE(activity.collection_item == CollectionItem(1));
  REQUIRE(SemanticLabelCount(*before, "Activity") == 1);
  REQUIRE_FALSE(disabled.enabled);
  REQUIRE(disabled.actions == 0);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      disabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));

  const SemanticNodeId overview_id = overview.id;
  const SemanticNodeId activity_id = activity.id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      activity.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_tabs_selection.Get() == 1);

  const std::shared_ptr<const SemanticFrame> after = runtime.BuildCommit().semantic_frame;
  const SemanticNode& updated_overview = FindSemanticNode(*after, "Overview");
  const SemanticNode& updated_activity = FindSemanticNode(*after, "Activity");
  REQUIRE(updated_overview.id == overview_id);
  REQUIRE(updated_activity.id == activity_id);
  REQUIRE(updated_overview.selected == false);
  REQUIRE(updated_activity.selected == true);
}

TEST_CASE("SegmentedButtonPublishesStableRadioButtonItems") {
  semantic_segmented_button_changes = 0;
  TestPlatform platform;
  Runtime runtime(SemanticSegmentedButtonApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 80.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& day = FindSemanticNode(*before, "Day");
  const SemanticNode& week = FindSemanticNode(*before, "Week");
  const SemanticNode& month = FindSemanticNode(*before, "Month");
  REQUIRE(day.parent.has_value());
  REQUIRE(day.parent == week.parent);
  REQUIRE(week.parent == month.parent);
  const auto group = std::ranges::find(before->nodes, *day.parent, &SemanticNode::id);
  REQUIRE(group != before->nodes.end());
  REQUIRE((group->collection == SemanticCollection{.item_count = 3, .row_count = 1, .column_count = 3}));
  REQUIRE(group->children.size() == 3);
  REQUIRE((group->actions & SemanticActionMask(SemanticActionKind::Focus)) != 0);

  REQUIRE(day.role == SemanticRole::RadioButton);
  REQUIRE(day.checked == SemanticCheckedState::Checked);
  REQUIRE(day.selected == true);
  REQUIRE((day.collection_item == SemanticCollectionItem{.index = 0, .row_index = 0, .column_index = 0}));
  REQUIRE(week.role == SemanticRole::RadioButton);
  REQUIRE(week.checked == SemanticCheckedState::Unchecked);
  REQUIRE(week.selected == false);
  REQUIRE((week.collection_item == SemanticCollectionItem{.index = 1, .row_index = 0, .column_index = 1}));
  REQUIRE((month.collection_item == SemanticCollectionItem{.index = 2, .row_index = 0, .column_index = 2}));
  REQUIRE(SemanticLabelCount(*before, "Month") == 1);
  REQUIRE(day.bounds.width > 0.0F);
  REQUIRE(week.bounds.width > 0.0F);
  REQUIRE(month.bounds.width > 0.0F);
  REQUIRE(day.bounds.x + day.bounds.width <= week.bounds.x);
  REQUIRE(week.bounds.x + week.bounds.width <= month.bounds.x);
  REQUIRE((week.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);

  const SemanticNodeId day_id = day.id;
  const SemanticNodeId week_id = week.id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(day.id, {SemanticActionKind::Activate, std::monostate{}}));
  REQUIRE(semantic_segmented_button_changes == 0);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(week.id, {SemanticActionKind::Activate, std::monostate{}}));
  REQUIRE(semantic_segmented_button_changes == 1);
  REQUIRE(semantic_segmented_button_selection.Get() == 1);

  const std::shared_ptr<const SemanticFrame> after = runtime.BuildCommit().semantic_frame;
  const SemanticNode& updated_day = FindSemanticNode(*after, "Day");
  const SemanticNode& updated_week = FindSemanticNode(*after, "Week");
  REQUIRE(updated_day.id == day_id);
  REQUIRE(updated_week.id == week_id);
  REQUIRE(updated_day.checked == SemanticCheckedState::Unchecked);
  REQUIRE(updated_day.selected == false);
  REQUIRE(updated_week.checked == SemanticCheckedState::Checked);
  REQUIRE(updated_week.selected == true);

  TestPlatform disabled_platform;
  Runtime disabled(DisabledSemanticSegmentedButtonApp, disabled_platform);
  disabled.SetWindowMetrics({.viewport = {240.0F, 80.0F}});
  const std::shared_ptr<const SemanticFrame> disabled_frame = disabled.BuildCommit().semantic_frame;
  const SemanticNode& disabled_week = FindSemanticNode(*disabled_frame, "Week");
  REQUIRE_FALSE(disabled_week.enabled);
  REQUIRE(disabled_week.actions == 0);
  REQUIRE_FALSE(
      disabled.CoreRuntime().PerformSemanticAction(disabled_week.id, {SemanticActionKind::Activate, std::monostate{}})
  );
  REQUIRE(semantic_segmented_button_changes == 1);
}

TEST_CASE("NavigationSelectorsPublishRealAccessibleItems") {
  TestPlatform platform;
  Runtime runtime(SemanticNavigationBarApp, platform);
  runtime.SetWindowMetrics({.viewport = {360.0F, 120.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& navigation = FindSemanticRole(*before, SemanticRole::Navigation);
  REQUIRE(navigation.collection == ItemCollection(3));
  REQUIRE(navigation.children.size() == 3);
  REQUIRE((navigation.actions & SemanticActionMask(SemanticActionKind::Focus)) != 0);

  const SemanticNode& home = FindSemanticNode(*before, "Home");
  const SemanticNode& library = FindSemanticNode(*before, "Library");
  const SemanticNode& disabled = FindSemanticNode(*before, "Disabled");
  REQUIRE(home.role == SemanticRole::Button);
  REQUIRE(home.selected == true);
  REQUIRE(home.collection_item == CollectionItem(0));
  REQUIRE(library.role == SemanticRole::Button);
  REQUIRE(library.selected == false);
  REQUIRE(library.collection_item == CollectionItem(1));
  REQUIRE(SemanticLabelCount(*before, "Home") == 1);
  REQUIRE(SemanticLabelCount(*before, "Library") == 1);
  REQUIRE_FALSE(disabled.enabled);
  REQUIRE(disabled.actions == 0);

  const SemanticNodeId home_id = home.id;
  const SemanticNodeId library_id = library.id;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      library.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_navigation_selection.Get() == 1);

  const std::shared_ptr<const SemanticFrame> after = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindSemanticNode(*after, "Home").id == home_id);
  REQUIRE(FindSemanticNode(*after, "Library").id == library_id);
  REQUIRE(FindSemanticNode(*after, "Home").selected == false);
  REQUIRE(FindSemanticNode(*after, "Library").selected == true);
}

TEST_CASE("NavigationPaneKeepsItsSemanticsAcrossVisualModes") {
  TestPlatform platform;
  Runtime runtime(SemanticNavigationPaneApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 180.0F}});

  const std::shared_ptr<const SemanticFrame> compact = runtime.BuildCommit().semantic_frame;
  const SemanticNode& compact_navigation = FindSemanticRole(*compact, SemanticRole::Navigation);
  const SemanticNode& compact_home = FindSemanticNode(*compact, "Home");
  const SemanticNode& compact_library = FindSemanticNode(*compact, "Library");
  REQUIRE(compact_navigation.collection == ItemCollection(2));
  REQUIRE(compact_navigation.children.size() == 2);
  REQUIRE(SemanticLabelCount(*compact, "Home") == 1);
  REQUIRE(SemanticLabelCount(*compact, "Library") == 1);

  const SemanticNodeId navigation_id = compact_navigation.id;
  const SemanticNodeId home_id = compact_home.id;
  const SemanticNodeId library_id = compact_library.id;
  semantic_navigation_expanded = true;
  const std::shared_ptr<const SemanticFrame> expanded = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindSemanticRole(*expanded, SemanticRole::Navigation).id == navigation_id);
  REQUIRE(FindSemanticNode(*expanded, "Home").id == home_id);
  REQUIRE(FindSemanticNode(*expanded, "Library").id == library_id);
  REQUIRE(SemanticLabelCount(*expanded, "Home") == 1);
  REQUIRE(SemanticLabelCount(*expanded, "Library") == 1);
}

TEST_CASE("ScrollViewPublishesMetricsAndRoutesScrollAndShowOnScreen") {
  TestPlatform platform;
  Runtime runtime(SemanticVerticalScrollApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 60.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& scroll = FindSemanticRole(*before, SemanticRole::ScrollView);
  const ScrollMetrics expected_metrics{
      .axis = Axis::Vertical,
      .offset = 0.0F,
      .maximum_offset = 60.0F,
      .viewport_extent = 60.0F,
      .content_extent = 120.0F,
  };
  REQUIRE(scroll.scroll == expected_metrics);
  REQUIRE((scroll.actions & SemanticActionMask(SemanticActionKind::Scroll)) != 0);

  const SemanticNode& first = FindSemanticNode(*before, "First");
  const SemanticNode& second = FindSemanticNode(*before, "Second");
  const SemanticNode& third = FindSemanticNode(*before, "Third");
  REQUIRE(scroll.children == std::vector<SemanticNodeId>{first.id, second.id, third.id});
  REQUIRE(std::ranges::none_of(before->nodes, [&](const SemanticNode& node) {
    return node.id != before->root && node.role == SemanticRole::Generic;
  }));
  REQUIRE_FALSE(first.offscreen);
  REQUIRE(third.offscreen);
  REQUIRE((third.actions & SemanticActionMask(SemanticActionKind::ShowOnScreen)) != 0);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      third.id,
      {SemanticActionKind::ShowOnScreen, std::monostate{}}
  ));

  const std::shared_ptr<const SemanticFrame> revealed = runtime.BuildCommit().semantic_frame;
  const SemanticNode& revealed_scroll = FindSemanticRole(*revealed, SemanticRole::ScrollView);
  REQUIRE(revealed_scroll.scroll->offset == 60.0F);
  REQUIRE_FALSE(FindSemanticNode(*revealed, "Third").offscreen);
  REQUIRE(FindSemanticNode(*revealed, "First").offscreen);

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      revealed_scroll.id,
      {SemanticActionKind::Scroll, Point{0.0F, -20.0F}}
  ));
  const SemanticNode& scrolled = FindSemanticRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ScrollView);
  REQUIRE(scrolled.scroll->offset == 40.0F);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      scrolled.id,
      {SemanticActionKind::Scroll, Point{20.0F, 0.0F}}
  ));
}

TEST_CASE("HorizontalScrollViewUsesHorizontalSemanticDeltas") {
  TestPlatform platform;
  Runtime runtime(SemanticHorizontalScrollApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});

  const SemanticNode& scroll = FindSemanticRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ScrollView);
  REQUIRE(scroll.scroll->axis == Axis::Horizontal);
  REQUIRE(scroll.scroll->maximum_offset == 80.0F);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      scroll.id,
      {SemanticActionKind::Scroll, Point{45.0F, 0.0F}}
  ));
  REQUIRE(FindSemanticRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ScrollView).scroll->offset == 45.0F);
}

TEST_CASE("VirtualListPublishesRealizedCollectionItemsAndRoutesExistingActions") {
  semantic_virtual_list_clicks = 0;
  semantic_virtual_list_factory_calls = 0;
  TestPlatform platform;
  Runtime runtime(SemanticVirtualListApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& list = FindSemanticRole(*before, SemanticRole::List);
  REQUIRE((list.collection == SemanticCollection{.item_count = 100, .row_count = 100, .column_count = 1}));
  REQUIRE(list.scroll.has_value());
  REQUIRE(list.scroll->axis == Axis::Vertical);
  REQUIRE(list.children.size() < 100);
  REQUIRE(semantic_virtual_list_factory_calls == static_cast<int>(list.children.size()));

  const SemanticNode& first = FindSemanticNode(*before, "Item 0");
  REQUIRE(first.parent == list.id);
  REQUIRE(first.role == SemanticRole::Button);
  REQUIRE((first.collection_item ==
           SemanticCollectionItem{.index = 0, .row_index = 0, .column_index = 0}));
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      first.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_virtual_list_clicks == 1);

  const SemanticNode& cached = FindSemanticNode(*before, "Item 3");
  REQUIRE(cached.offscreen);
  REQUIRE((cached.actions & SemanticActionMask(SemanticActionKind::ShowOnScreen)) != 0);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      cached.id,
      {SemanticActionKind::ShowOnScreen, std::monostate{}}
  ));
  const std::shared_ptr<const SemanticFrame> revealed = runtime.BuildCommit().semantic_frame;
  REQUIRE_FALSE(FindSemanticNode(*revealed, "Item 3").offscreen);

  runtime.HandleScrollEvent(ScrollEvent{{50.0F, 20.0F}, 0.0F, 1000.0F});
  runtime.BuildCommit();
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      first.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_virtual_list_factory_calls < 100);

  runtime.HandleScrollEvent(ScrollEvent{{50.0F, 20.0F}, 0.0F, -1000.0F});
  const SemanticNode& returned = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Item 0");
  REQUIRE(returned.id != first.id);
}

TEST_CASE("HorizontalVirtualListPublishesOneSemanticRow") {
  TestPlatform platform;
  Runtime runtime(SemanticHorizontalVirtualListApp, platform);
  runtime.SetWindowMetrics({.viewport = {60.0F, 30.0F}});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& list = FindSemanticRole(*frame, SemanticRole::List);
  REQUIRE((list.collection == SemanticCollection{.item_count = 5, .row_count = 1, .column_count = 5}));
  REQUIRE(list.scroll->axis == Axis::Horizontal);
  const SemanticNode& third = FindSemanticNode(*frame, "Horizontal 2");
  REQUIRE(third.role == SemanticRole::Text);
  REQUIRE((third.collection_item ==
           SemanticCollectionItem{.index = 2, .row_index = 0, .column_index = 2}));
}

TEST_CASE("VirtualGridPublishesResolvedCellsAndKeepsRealizedIdentityAcrossReflow") {
  TestPlatform platform;
  Runtime runtime(SemanticVirtualGridApp, platform);
  runtime.SetWindowMetrics({.viewport = {90.0F, 40.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& grid = FindSemanticRole(*before, SemanticRole::Grid);
  REQUIRE((grid.collection == SemanticCollection{.item_count = 8, .row_count = 4, .column_count = 3}));
  REQUIRE(grid.children.size() < 8);

  const SemanticNode& first = FindSemanticNode(*before, "Cell 0");
  const SemanticNodeId first_id = first.id;
  REQUIRE(first.parent == grid.id);
  REQUIRE(first.role == SemanticRole::GridCell);
  const SemanticCollectionItem first_position{
      .index = 0,
      .row_index = 0,
      .column_index = 0,
      .column_span = 2,
  };
  REQUIRE(first.collection_item == first_position);
  const SemanticNode& fourth = FindSemanticNode(*before, "Cell 3");
  const SemanticCollectionItem fourth_position{
      .index = 3,
      .row_index = 1,
      .column_index = 1,
      .column_span = 2,
  };
  REQUIRE(fourth.collection_item == fourth_position);

  runtime.SetWindowMetrics({.viewport = {60.0F, 40.0F}});
  const std::shared_ptr<const SemanticFrame> reflowed = runtime.BuildCommit().semantic_frame;
  const SemanticNode& resized_grid = FindSemanticRole(*reflowed, SemanticRole::Grid);
  REQUIRE((resized_grid.collection == SemanticCollection{.item_count = 8, .row_count = 5, .column_count = 2}));
  const SemanticNode& resized_first = FindSemanticNode(*reflowed, "Cell 0");
  REQUIRE(resized_first.id == first_id);
  REQUIRE(resized_first.collection_item->column_span == 2);
}

TEST_CASE("EmptyVirtualListStillPublishesItsCollection") {
  TestPlatform platform;
  Runtime runtime(SemanticEmptyVirtualListApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});

  const SemanticNode& list = FindSemanticRole(*runtime.BuildCommit().semantic_frame, SemanticRole::List);
  REQUIRE((list.collection == SemanticCollection{.item_count = 0, .row_count = 0, .column_count = 1}));
  REQUIRE(list.children.empty());
}

TEST_CASE("ShowOnScreenRevealsContentThroughNestedScrollContainers") {
  TestPlatform platform;
  Runtime runtime(SemanticNestedScrollApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});

  const SemanticNode& target = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Inner third");
  REQUIRE(target.offscreen);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      target.id,
      {SemanticActionKind::ShowOnScreen, std::monostate{}}
  ));

  const std::shared_ptr<const SemanticFrame> revealed = runtime.BuildCommit().semantic_frame;
  std::vector<float> offsets;
  for (const SemanticNode& node : revealed->nodes) {
    if (node.role == SemanticRole::ScrollView) {
      REQUIRE(node.scroll.has_value());
      offsets.push_back(node.scroll->offset);
    }
  }
  REQUIRE(offsets == std::vector<float>{20.0F, 60.0F});
  REQUIRE_FALSE(FindSemanticNode(*revealed, "Inner third").offscreen);
}

TEST_CASE("SemanticOffscreenStateHonorsPresentationTransformsAndClipping") {
  TestPlatform platform;
  Runtime runtime(SemanticClippedTransformApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});

  const SemanticNode& clipped = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Clipped");
  REQUIRE(clipped.bounds == Rect{0.0F, 50.0F, 80.0F, 20.0F});
  REQUIRE(clipped.offscreen);
  REQUIRE((clipped.actions & SemanticActionMask(SemanticActionKind::ShowOnScreen)) == 0);
}

TEST_CASE("SemanticsModifierPublishesCustomMeaning") {
  TestPlatform platform;
  Runtime runtime(SemanticOverrideApp, platform);
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});

  const SemanticNode& image = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Revenue chart");
  REQUIRE(image.role == SemanticRole::Image);
  REQUIRE(image.bounds == Rect{0.0F, 0.0F, 120.0F, 80.0F});
}

TEST_CASE("ExplicitEmptySemanticsPublishesGenericOwner") {
  TestPlatform platform;
  Runtime runtime(EmptySemanticApp, platform);
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(frame->nodes.size() == 2);
  REQUIRE(frame->nodes.back().role == SemanticRole::Generic);
}

TEST_CASE("AuthorSemanticsOverrideExtensionAndPreserveComponentMeaning") {
  TestPlatform platform;
  Runtime runtime(SemanticPrecedenceApp, platform);
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});

  const SemanticNode& button = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Author");
  REQUIRE(button.role == SemanticRole::Button);
  REQUIRE((button.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);
}

TEST_CASE("SecureTextFieldDoesNotPublishItsValue") {
  TestPlatform platform;
  Runtime runtime(SecureSemanticTextFieldApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& field = FindSemanticNode(*before, "Password");
  REQUIRE(field.role == SemanticRole::TextField);
  REQUIRE(field.secure);
  REQUIRE(field.value.empty());
  REQUIRE_FALSE(field.text_selection.has_value());
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetText)) != 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetSelection)) == 0);

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetText, std::string("changed")}
  ));
  REQUIRE(semantic_secure_text_field_value.Get().text == "changed");
  const SemanticNode& changed = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Password");
  REQUIRE(changed.value.empty());
  REQUIRE_FALSE(changed.text_selection.has_value());
}

TEST_CASE("TextFieldPublishesUtf16SelectionAndRoutesAccessibleEditing") {
  TestPlatform platform;
  Runtime runtime(EditableSemanticTextFieldApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& field = FindSemanticNode(*before, "Editor");
  REQUIRE(
      field.value == "a\xF0\x9F\x98\x80"
                     "b"
  );
  REQUIRE(field.text_selection == TextRange{4, 4});
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetText)) != 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetSelection)) != 0);

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetSelection, TextRange{1, 3}}
  ));
  REQUIRE(semantic_text_field_value.Get().selection.Range() == TextRange{1, 3});
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetSelection, TextRange{2, 3}}
  ));

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetText, std::string("longer")}
  ));
  REQUIRE(semantic_text_field_value.Get() == TextEditingValue::FromText("long"));
  const SemanticNode& changed = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Editor");
  REQUIRE(changed.value == "long");
  REQUIRE(changed.text_selection == TextRange{4, 4});
}

TEST_CASE("ReadOnlyTextFieldAllowsSelectionButRejectsAccessibleReplacement") {
  TestPlatform platform;
  Runtime runtime(ReadOnlySemanticTextFieldApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});

  const SemanticNode& field = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Read only");
  REQUIRE(field.read_only == true);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetText)) == 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetSelection)) != 0);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetText, std::string("changed")}
  ));
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetSelection, TextRange{0, 4}}
  ));
  REQUIRE(semantic_text_field_value.Get().selection.Range() == TextRange{0, 4});
}

TEST_CASE("SemanticBuilderPublishesStableVirtualChildrenAndRoutesActions") {
  virtual_semantic_activations = 0;
  TestPlatform platform;
  Runtime runtime(VirtualSemanticApp, platform);
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});

  const std::shared_ptr<const SemanticFrame> first = runtime.BuildCommit().semantic_frame;
  const SemanticNode& child = FindSemanticNode(*first, "April");
  REQUIRE(child.role == SemanticRole::Button);
  REQUIRE(child.bounds == Rect{8.0F, 6.0F, 24.0F, 12.0F});
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      child.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(virtual_semantic_activations == 1);

  const SemanticNodeId child_id = child.id;
  const SemanticNode& stable_child = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April");
  REQUIRE(stable_child.id == child_id);
}

TEST_CASE("ReplacingSemanticExtensionInvalidatesVirtualIdentityAndActionRoute") {
  virtual_semantic_activations = 0;
  TestPlatform platform;
  Runtime runtime(VirtualSemanticLifecycleApp, platform);
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});

  const SemanticNodeId first_id = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April").id;
  semantic_virtual_visible = false;
  const std::shared_ptr<const SemanticFrame> fallback = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindSemanticNodeOrNull(*fallback, "April") == nullptr);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      first_id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));

  semantic_virtual_visible = true;
  const SemanticNode& replacement = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April");
  REQUIRE(replacement.id != first_id);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      replacement.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(virtual_semantic_activations == 1);
}

TEST_CASE("SemanticLifecycleHonorsVisibilityExclusionDisabledStateAndStaleActions") {
  semantic_lifecycle_clicks = 0;
  TestPlatform platform;
  Runtime runtime(SemanticLifecycleApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const std::shared_ptr<const SemanticFrame> first = runtime.BuildCommit().semantic_frame;
  const SemanticNode& primary = FindSemanticNode(*first, "Primary");
  const SemanticNodeId primary_id = primary.id;
  const SemanticNode& disabled = FindSemanticNode(*first, "Disabled");
  REQUIRE(disabled.actions == 0);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      disabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_lifecycle_clicks == 0);

  const SemanticNode& owner = FindSemanticNode(*first, "Owner");
  REQUIRE(owner.children.empty());
  REQUIRE(FindSemanticNodeOrNull(*first, "Excluded") == nullptr);
  REQUIRE(FindSemanticNodeOrNull(*first, "Hidden") == nullptr);

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(primary_id, {SemanticActionKind::Focus, std::monostate{}}));
  const SemanticNode& focused = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Primary");
  REQUIRE(focused.focused);

  semantic_alternate_content = true;
  const std::shared_ptr<const SemanticFrame> replacement_frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& replacement = FindSemanticNode(*replacement_frame, "Replacement");
  REQUIRE(replacement.id != primary_id);
  REQUIRE(FindSemanticNodeOrNull(*replacement_frame, "Primary") == nullptr);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      primary_id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
}

TEST_CASE("CompatibleSemanticUpdatesPreserveIdentityAndAdvanceRevision") {
  TestPlatform platform;
  Runtime runtime(SemanticCompatibleUpdateApp, platform);
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNodeId id = FindSemanticNode(*before, "Before").id;

  semantic_alternate_label = true;
  const std::shared_ptr<const SemanticFrame> after = runtime.BuildCommit().semantic_frame;
  REQUIRE(after->revision > before->revision);
  REQUIRE(FindSemanticNode(*after, "After").id == id);
}

TEST_CASE("SemanticsModifierRejectsInvalidSharedValues") {
  REQUIRE_THROWS_AS(
      Canvas([](PaintContext&, Size) {}).With(Semantics{.heading_level = 7}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Canvas([](PaintContext&, Size) {}).With(Semantics{.range = SemanticRange{1.0, 0.0, 0.5}}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Canvas([](PaintContext&, Size) {}).With(Semantics{.text_selection = TextRange{2, 1}}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Canvas([](PaintContext&, Size) {})
          .With(Semantics{.scroll = ScrollMetrics{.offset = 2.0F, .maximum_offset = 1.0F}}),
      std::invalid_argument
  );
}

} // namespace huxerui::test
