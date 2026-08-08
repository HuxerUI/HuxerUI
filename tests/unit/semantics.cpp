#include "runtime_test_support.h"

#include <type_traits>

namespace huxerui::test {

namespace {

int semantic_button_clicks = 0;
int semantic_icon_button_clicks = 0;
int semantic_lifecycle_clicks = 0;
int virtual_semantic_activations = 0;
State<float> semantic_slider_value;
State<bool> semantic_alternate_content;
State<bool> semantic_alternate_label;
State<bool> semantic_virtual_visible;
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
  Runtime runtime(SemanticBasicsApp, platform);
  runtime.SetViewport({320.0F, 240.0F});

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

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(button.id, {SemanticActionKind::Activate, std::monostate{}}));
  REQUIRE(semantic_button_clicks == 1);
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      icon_button.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_icon_button_clicks == 1);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(button.id, {SemanticActionKind::Activate, 1.0}));

  const FrameCommit& unchanged = runtime.BuildCommit();
  REQUIRE(unchanged.semantic_frame == first_frame);
}

TEST_CASE("SemanticActionsRouteToRetainedControlBehavior") {
  TestPlatform platform;
  Runtime runtime(SemanticBasicsApp, platform);
  runtime.SetViewport({320.0F, 240.0F});

  runtime.BuildCommit();
  const auto slider_node = std::ranges::find_if(runtime.LastCommit().semantic_frame->nodes, [](const SemanticNode& node) {
    return node.role == SemanticRole::Slider;
  });
  REQUIRE(slider_node != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      slider_node->id,
      {SemanticActionKind::SetValue, 7.5}
  ));
  REQUIRE(semantic_slider_value.Get() == 7.5F);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
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
  runtime.SetViewport({320.0F, 120.0F});

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
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      disabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));

  const SemanticNodeId overview_id = overview.id;
  const SemanticNodeId activity_id = activity.id;
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
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

TEST_CASE("NavigationSelectorsPublishRealAccessibleItems") {
  TestPlatform platform;
  Runtime runtime(SemanticNavigationBarApp, platform);
  runtime.SetViewport({360.0F, 120.0F});

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
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
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
  runtime.SetViewport({320.0F, 180.0F});

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
  runtime.SetViewport({100.0F, 60.0F});

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
  const SemanticNode& third = FindSemanticNode(*before, "Third");
  REQUIRE_FALSE(first.offscreen);
  REQUIRE(third.offscreen);
  REQUIRE((third.actions & SemanticActionMask(SemanticActionKind::ShowOnScreen)) != 0);
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      third.id,
      {SemanticActionKind::ShowOnScreen, std::monostate{}}
  ));

  const std::shared_ptr<const SemanticFrame> revealed = runtime.BuildCommit().semantic_frame;
  const SemanticNode& revealed_scroll = FindSemanticRole(*revealed, SemanticRole::ScrollView);
  REQUIRE(revealed_scroll.scroll->offset == 60.0F);
  REQUIRE_FALSE(FindSemanticNode(*revealed, "Third").offscreen);
  REQUIRE(FindSemanticNode(*revealed, "First").offscreen);

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      revealed_scroll.id,
      {SemanticActionKind::Scroll, Point{0.0F, -20.0F}}
  ));
  const SemanticNode& scrolled = FindSemanticRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ScrollView);
  REQUIRE(scrolled.scroll->offset == 40.0F);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      scrolled.id,
      {SemanticActionKind::Scroll, Point{20.0F, 0.0F}}
  ));
}

TEST_CASE("HorizontalScrollViewUsesHorizontalSemanticDeltas") {
  TestPlatform platform;
  Runtime runtime(SemanticHorizontalScrollApp, platform);
  runtime.SetViewport({100.0F, 40.0F});

  const SemanticNode& scroll = FindSemanticRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ScrollView);
  REQUIRE(scroll.scroll->axis == Axis::Horizontal);
  REQUIRE(scroll.scroll->maximum_offset == 80.0F);
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      scroll.id,
      {SemanticActionKind::Scroll, Point{45.0F, 0.0F}}
  ));
  REQUIRE(FindSemanticRole(*runtime.BuildCommit().semantic_frame, SemanticRole::ScrollView).scroll->offset == 45.0F);
}

TEST_CASE("ShowOnScreenRevealsContentThroughNestedScrollContainers") {
  TestPlatform platform;
  Runtime runtime(SemanticNestedScrollApp, platform);
  runtime.SetViewport({100.0F, 80.0F});

  const SemanticNode& target = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Inner third");
  REQUIRE(target.offscreen);
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
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
  runtime.SetViewport({100.0F, 80.0F});

  const SemanticNode& clipped = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Clipped");
  REQUIRE(clipped.bounds == Rect{0.0F, 50.0F, 80.0F, 20.0F});
  REQUIRE(clipped.offscreen);
  REQUIRE((clipped.actions & SemanticActionMask(SemanticActionKind::ShowOnScreen)) == 0);
}

TEST_CASE("SemanticsModifierPublishesCustomMeaning") {
  TestPlatform platform;
  Runtime runtime(SemanticOverrideApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const SemanticNode& image = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Revenue chart");
  REQUIRE(image.role == SemanticRole::Image);
  REQUIRE(image.bounds == Rect{0.0F, 0.0F, 120.0F, 80.0F});
}

TEST_CASE("ExplicitEmptySemanticsPublishesGenericOwner") {
  TestPlatform platform;
  Runtime runtime(EmptySemanticApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(frame->nodes.size() == 2);
  REQUIRE(frame->nodes.back().role == SemanticRole::Generic);
}

TEST_CASE("AuthorSemanticsOverrideExtensionAndPreserveComponentMeaning") {
  TestPlatform platform;
  Runtime runtime(SemanticPrecedenceApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const SemanticNode& button = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Author");
  REQUIRE(button.role == SemanticRole::Button);
  REQUIRE((button.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);
}

TEST_CASE("SecureTextFieldDoesNotPublishItsValue") {
  TestPlatform platform;
  Runtime runtime(SecureSemanticTextFieldApp, platform);
  runtime.SetViewport({320.0F, 120.0F});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& field = FindSemanticNode(*before, "Password");
  REQUIRE(field.role == SemanticRole::TextField);
  REQUIRE(field.secure);
  REQUIRE(field.value.empty());
  REQUIRE_FALSE(field.text_selection.has_value());
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetText)) != 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetSelection)) == 0);

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
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
  runtime.SetViewport({320.0F, 120.0F});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNode& field = FindSemanticNode(*before, "Editor");
  REQUIRE(
      field.value == "a\xF0\x9F\x98\x80"
                     "b"
  );
  REQUIRE(field.text_selection == TextRange{4, 4});
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetText)) != 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetSelection)) != 0);

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetSelection, TextRange{1, 3}}
  ));
  REQUIRE(semantic_text_field_value.Get().selection.Range() == TextRange{1, 3});
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetSelection, TextRange{2, 3}}
  ));

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
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
  runtime.SetViewport({320.0F, 120.0F});

  const SemanticNode& field = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Read only");
  REQUIRE(field.read_only == true);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetText)) == 0);
  REQUIRE((field.actions & SemanticActionMask(SemanticActionKind::SetSelection)) != 0);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetText, std::string("changed")}
  ));
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      field.id,
      {SemanticActionKind::SetSelection, TextRange{0, 4}}
  ));
  REQUIRE(semantic_text_field_value.Get().selection.Range() == TextRange{0, 4});
}

TEST_CASE("SemanticBuilderPublishesStableVirtualChildrenAndRoutesActions") {
  virtual_semantic_activations = 0;
  TestPlatform platform;
  Runtime runtime(VirtualSemanticApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const std::shared_ptr<const SemanticFrame> first = runtime.BuildCommit().semantic_frame;
  const SemanticNode& child = FindSemanticNode(*first, "April");
  REQUIRE(child.role == SemanticRole::Button);
  REQUIRE(child.bounds == Rect{8.0F, 6.0F, 24.0F, 12.0F});
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
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
  runtime.SetViewport({120.0F, 80.0F});

  const SemanticNodeId first_id = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April").id;
  semantic_virtual_visible = false;
  const std::shared_ptr<const SemanticFrame> fallback = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindSemanticNodeOrNull(*fallback, "April") == nullptr);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      first_id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));

  semantic_virtual_visible = true;
  const SemanticNode& replacement = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April");
  REQUIRE(replacement.id != first_id);
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      replacement.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(virtual_semantic_activations == 1);
}

TEST_CASE("SemanticLifecycleHonorsVisibilityExclusionDisabledStateAndStaleActions") {
  semantic_lifecycle_clicks = 0;
  TestPlatform platform;
  Runtime runtime(SemanticLifecycleApp, platform);
  runtime.SetViewport({320.0F, 240.0F});

  const std::shared_ptr<const SemanticFrame> first = runtime.BuildCommit().semantic_frame;
  const SemanticNode& primary = FindSemanticNode(*first, "Primary");
  const SemanticNodeId primary_id = primary.id;
  const SemanticNode& disabled = FindSemanticNode(*first, "Disabled");
  REQUIRE(disabled.actions == 0);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      disabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_lifecycle_clicks == 0);

  const SemanticNode& owner = FindSemanticNode(*first, "Owner");
  REQUIRE(owner.children.empty());
  REQUIRE(FindSemanticNodeOrNull(*first, "Excluded") == nullptr);
  REQUIRE(FindSemanticNodeOrNull(*first, "Hidden") == nullptr);

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(primary_id, {SemanticActionKind::Focus, std::monostate{}}));
  const SemanticNode& focused = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Primary");
  REQUIRE(focused.focused);

  semantic_alternate_content = true;
  const std::shared_ptr<const SemanticFrame> replacement_frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& replacement = FindSemanticNode(*replacement_frame, "Replacement");
  REQUIRE(replacement.id != primary_id);
  REQUIRE(FindSemanticNodeOrNull(*replacement_frame, "Primary") == nullptr);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      primary_id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
}

TEST_CASE("CompatibleSemanticUpdatesPreserveIdentityAndAdvanceRevision") {
  TestPlatform platform;
  Runtime runtime(SemanticCompatibleUpdateApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

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
