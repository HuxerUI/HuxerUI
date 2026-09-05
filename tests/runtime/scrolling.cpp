#include "runtime_test_support.h"

namespace huxerui::test {

std::string scroll_clicked;
State<bool> show_controlled_scroll;
ScrollController controlled_list_scroll;
ScrollController variable_list_scroll;
ScrollController controlled_grid_scroll;
ScrollController controlled_view_scroll;
ScrollController horizontal_view_scroll;
ScrollController example_scroll;
ScrollController scoped_grow_scroll;
ScrollController first_indexed_page_scroll;
ScrollController second_indexed_page_scroll;
State<bool> scoped_scroll_content_changed;
State<float> scoped_scroll_content_height;
State<std::size_t> indexed_scroll_page;
State<std::size_t> interactive_pager_page;
State<bool> interactive_pager_drag_enabled;
State<std::size_t> reversed_pager_page;
std::vector<std::size_t> pager_proposals;
bool accept_pager_proposals = true;
int scroll_observer_compositions = 0;
bool consume_scroll_input = false;
std::optional<ScrollInputEvent> received_scroll_input;
ScrollController scroll_input_controller;
ScrollController cross_axis_outer_scroll;
ScrollController cross_axis_inner_scroll;
ScrollController hook_outer_scroll;
ScrollController hook_inner_scroll;
ScrollController activity_scroll;
ScrollController refresh_content_scroll;
State<bool> disableable_scroll_enabled;
State<bool> refresh_box_refreshing;
std::vector<std::string> scroll_hook_calls;
std::vector<ScrollActivity> scroll_activities;
int refresh_requests = 0;
bool accept_refresh_requests = true;

struct ScrollHook {
  class Extension;

  std::string name;
  float pre_consumption = 0.0F;
  float post_consumption = 0.0F;

  bool operator==(const ScrollHook&) const = default;
};

class ScrollHook::Extension final : public NodeExtension {
public:
  Extension(MountedNode& node, const ScrollHook& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode&, const ScrollHook& modifier) {
    name_ = modifier.name;
    pre_consumption_ = modifier.pre_consumption;
    post_consumption_ = modifier.post_consumption;
  }

  float OnPreScroll(MountedNode&, Axis, float, ScrollSource) override {
    scroll_hook_calls.push_back(name_ + ".pre");
    return pre_consumption_;
  }

  float OnPostScroll(MountedNode&, Axis, float, float, ScrollSource) override {
    scroll_hook_calls.push_back(name_ + ".post");
    return post_consumption_;
  }

private:
  std::string name_;
  float pre_consumption_ = 0.0F;
  float post_consumption_ = 0.0F;
};

struct ScrollActivityProbe {
  class Extension;

  bool operator==(const ScrollActivityProbe&) const = default;
};

class ScrollActivityProbe::Extension final : public NodeExtension {
public:
  Extension(MountedNode&, const ScrollActivityProbe&) {}

  void Update(MountedNode&, const ScrollActivityProbe&) {}

  void OnScrollActivity(MountedNode&, const ScrollActivity& activity) override {
    scroll_activities.push_back(activity);
  }
};

class ScrollDefaultsPlatform final : public TestPlatform {
public:
  ScrollPhysics ScrollDefaults() const noexcept override {
    return scroll_defaults;
  }

  ScrollPhysics scroll_defaults;
};

View ScrollViewApp() {
  return ScrollView{
      Column{
          Button("First").With(huxerui::Frame{100.0F, 40.0F}).OnClick([] { scroll_clicked = "First"; }),
          Button("Second").With(huxerui::Frame{100.0F, 40.0F}).OnClick([] { scroll_clicked = "Second"; }),
          Button("Third").With(huxerui::Frame{100.0F, 40.0F}).OnClick([] { scroll_clicked = "Third"; }),
      },
  };
}

View ScrollObserverItem(int index, ScrollController scroll) {
  HUXERUI_SCOPE({
    if (index == 0) {
      ++scroll_observer_compositions;
    }
    return Text::Format("{}:{}", index, scroll.Offset()).With(huxerui::Frame{100.0F, 20.0F});
  });
}

View ControlledVirtualListApp() {
  auto visible = UseState(true);
  auto scroll = UseScrollController(40.0F);
  show_controlled_scroll = visible;
  controlled_list_scroll = scroll;
  if (!visible.Get()) {
    return Text("Hidden");
  }

  return VirtualList(
             std::size_t{1000},
             [scroll](std::size_t index) { return ScrollObserverItem(static_cast<int>(index), scroll).Key(index); }
  )
      .ItemExtent(20.0F)
      .CacheExtent(40.0F)
      .Controller(scroll);
}

View ScrollControllerToolbar(ScrollController scroll) {
  HUXERUI_SCOPE({
    return Row{
        Button("Top").OnClick([scroll] { static_cast<void>(scroll.ScrollTo(0.0F)); }),
        Button("Item 500").OnClick([scroll] { static_cast<void>(scroll.ScrollToItem(499, ScrollAlignment::Center)); }),
        Spacer(),
        Text::Format("Offset {}", static_cast<int>(scroll.Offset())),
    }
        .With(huxerui::Spacing{12.0F}, huxerui::CrossAlign{CrossAxisAlignment::Center});
  });
}

View VariableExtentScrollApp() {
  auto scroll = UseScrollController();
  variable_list_scroll = scroll;
  return VirtualList(std::size_t{1001}, [](std::size_t index) {
    return Text::Format("Block {}", index).With(Frame{.height = index == 1000 ? 120.0F : 46.0F}).Key(index);
  }).EstimatedItemExtent(84.0F).CacheExtent(100.0F).Controller(scroll);
}

View ScrollControllerExampleApp() {
  auto scroll = UseScrollController();
  example_scroll = scroll;
  return Column{
      ScrollControllerToolbar(scroll),
      VirtualList(
          std::size_t{1000},
          [](std::size_t index) {
            return Text::Format("Item {}", index + 1).With(huxerui::Frame{100.0F, 40.0F}).Key(index);
          }
      )
          .ItemExtent(48.0F)
          .Controller(scroll)
          .With(huxerui::Spacing{8.0F}, huxerui::Grow{}),
  }
      .With(huxerui::Padding{24.0F}, huxerui::Spacing{12.0F});
}

View ControlledVirtualGridApp() {
  auto scroll = UseScrollController();
  controlled_grid_scroll = scroll;
  return VirtualGrid(
             std::size_t{300},
             [](std::size_t index) {
               return Text(std::to_string(index)).With(huxerui::Frame{100.0F, 20.0F}).Key(index);
             }
  )
      .Columns(GridColumns::Fixed(3))
      .RowExtent(20.0F)
      .RowSpacing(4.0F)
      .Controller(scroll);
}

View ControlledScrollViewApp() {
  auto scroll = UseScrollController(20.0F);
  controlled_view_scroll = scroll;
  std::vector<int> items(20);
  std::iota(items.begin(), items.end(), 0);
  return ScrollView{
      Column{
          ForEach(items, [](int index) { return Text(std::to_string(index)).With(huxerui::Frame{100.0F, 20.0F}); }),
      },
  }
      .Controller(scroll);
}

View HorizontalScrollViewApp() {
  auto scroll = UseScrollController();
  horizontal_view_scroll = scroll;
  return ScrollView{
      Row{
          Button("First").With(huxerui::Frame{60.0F, 40.0F}),
          Button("Second").With(huxerui::Frame{60.0F, 40.0F}),
          Button("Third").With(huxerui::Frame{60.0F, 40.0F}),
      },
  }
      .ScrollAxis(Axis::Horizontal)
      .Controller(scroll)
      .With(huxerui::ScrollBar{});
}

View ScopedScrollContent() {
  HUXERUI_SCOPE({
    auto changed = UseState(false);
    auto content_height = UseState(70.0F);
    scoped_scroll_content_changed = changed;
    scoped_scroll_content_height = content_height;
    return Column {
      Text(changed ? "Changed" : "Initial").With(huxerui::Frame{100.0F, 20.0F}),
      Spacer().With(huxerui::Frame{100.0F, content_height.Get()}),
    };
  });
}

View ScopedScrollViewApp() {
  auto scroll = UseScrollController();
  scoped_grow_scroll = scroll;
  return Column {
    Text("Header").With(huxerui::Frame{100.0F, 40.0F}),
    ScrollView {
      Column {
        ScopedScrollContent(),
      },
    }.Controller(scroll).With(huxerui::Grow{}),
  }.With(huxerui::CrossAlign{CrossAxisAlignment::Stretch});
}

View IndexedScrollablePage(std::size_t index) {
  HUXERUI_SCOPE({
    auto scroll = UseScrollController();
    if (index == 0) {
      first_indexed_page_scroll = scroll;
    } else {
      second_indexed_page_scroll = scroll;
    }
    return ScrollView {
      Column {
        Text::Format("Page {}", index),
        Spacer().With(huxerui::Frame{100.0F, 300.0F}),
      },
    }.Controller(scroll);
  });
}

View IndexedScrollingApp() {
  auto selected = UseState<std::size_t>(0);
  indexed_scroll_page = selected;
  return IndexedPages(
      {
          IndexedScrollablePage(0),
          IndexedScrollablePage(1),
      },
      selected
  );
}

View InteractivePagerApp() {
  auto selected = UseState<std::size_t>(1);
  auto drag_enabled = UseState(true);
  interactive_pager_page = selected;
  interactive_pager_drag_enabled = drag_enabled;
  return Pager(
             {
                 Text("Previous"),
                 Text("Current"),
                 Text("Next"),
             },
             selected
  )
      .DragEnabled(drag_enabled)
      .OnChanged([selected](std::size_t index) {
        pager_proposals.push_back(index);
        if (accept_pager_proposals) {
          selected = index;
        }
      });
}

View ReversedVerticalPagerApp() {
  auto selected = UseState<std::size_t>(1);
  reversed_pager_page = selected;
  return Pager(
             {
                 Text("First"),
                 Text("Second"),
                 Text("Third"),
             },
             selected
  )
      .ScrollAxis(Axis::Vertical)
      .Reverse()
      .OnChanged([selected](std::size_t index) {
        pager_proposals.push_back(index);
        selected = index;
      });
}

View ScrollInputApp() {
  auto scroll = UseScrollController();
  scroll_input_controller = scroll;
  return ScrollView {
    Spacer().With(huxerui::Frame{100.0F, 300.0F}),
  }.Controller(scroll)
      .On<ViewEvents::ScrollInput>([](const ScrollInputEvent& event) {
        received_scroll_input = event;
        return consume_scroll_input;
      });
}

View CrossAxisNestedScrollApp() {
  auto outer = UseScrollController();
  auto inner = UseScrollController();
  cross_axis_outer_scroll = outer;
  cross_axis_inner_scroll = inner;
  return ScrollView {
    Column {
      ScrollView {
        Spacer().With(huxerui::Frame{200.0F, 40.0F}),
      }.ScrollAxis(Axis::Horizontal).Controller(inner).With(huxerui::Frame{100.0F, 40.0F}),
      Spacer().With(huxerui::Frame{100.0F, 200.0F}),
    },
  }.Controller(outer);
}

View NestedScrollHookApp() {
  auto outer = UseScrollController();
  auto inner = UseScrollController();
  hook_outer_scroll = outer;
  hook_inner_scroll = inner;
  return ScrollView {
    Column {
      Spacer().With(huxerui::Frame{100.0F, 200.0F}),
      ScrollView {
        Spacer().With(huxerui::Frame{100.0F, 200.0F}),
      }.Controller(inner).With(huxerui::Frame{100.0F, 60.0F}, ScrollHook{"inner", 3.0F, 4.0F}),
    },
  }.Controller(outer).With(ScrollHook{"outer", 2.0F, 5.0F});
}

View ScrollActivityApp() {
  auto scroll = UseScrollController();
  activity_scroll = scroll;
  return ScrollView {
    Spacer().With(huxerui::Frame{100.0F, 300.0F}),
  }.Controller(scroll).With(ScrollActivityProbe{}, ScrollPhysics{.fling_enabled = false});
}

View DisableableOverscrollApp() {
  auto enabled = UseState(true);
  disableable_scroll_enabled = enabled;
  return ScrollView {
    Spacer().With(huxerui::Frame{100.0F, 300.0F}),
  }.With(Enabled(enabled.Get()), ScrollPhysics{.fling_enabled = false});
}

View ReducedMotionOverscrollApp() {
  ThemeSpec theme = huxerui::FlatLightThemeSpec();
  theme.motion.reduced_motion = true;
  return Theme {ThemeDefinition{theme}, ScrollActivityApp()};
}

View RefreshBoxApp() {
  auto refreshing = UseState(false);
  refresh_box_refreshing = refreshing;
  return RefreshBox(Spacer().With(huxerui::Frame{100.0F, 100.0F}), refreshing)
      .OnRefresh([refreshing] {
        ++refresh_requests;
        if (accept_refresh_requests) {
          refreshing = true;
        }
      });
}

View InvalidRefreshBoxStyleApp() {
  ThemeDefinition definition = FlatThemeDefinition();
  huxerui::RefreshBoxStyle style = ThemeDefinitionValue<huxerui::RefreshBoxStyle>(definition);
  style.maximum_pull_distance = 40.0F;
  style.trigger_distance = 60.0F;
  definition.Set(style);
  return Theme {
    std::move(definition),
    RefreshBox(Spacer().With(huxerui::Frame{100.0F, 100.0F}), false).OnRefresh([] {}),
  };
}

View NestedRefreshBoxApp() {
  auto scroll = UseScrollController(80.0F);
  refresh_content_scroll = scroll;
  return RefreshBox(
             ScrollView {
               Spacer().With(huxerui::Frame{100.0F, 300.0F}),
             }.Controller(scroll),
             false
  )
      .OnRefresh([] { ++refresh_requests; });
}

TEST_CASE("TestScrollViewLayoutClipAndHitTest") {
  scroll_clicked.clear();

  TestPlatform platform;
  Runtime runtime{ScrollViewApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 60.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->measured_size.width == 100.0F);
  REQUIRE(root->measured_size.height == 60.0F);
  REQUIRE(root->scroll_state != nullptr);
  REQUIRE(root->scroll_state->content_height == 120.0F);
  REQUIRE(root->scroll_state->offset_y == 0.0F);
  REQUIRE(root->children[0]->scroll_state == nullptr);
  REQUIRE(root->children[0]->layout_offset.y == 0.0F);

  int push_clips = 0;
  int pop_clips = 0;
  for (const auto& command : initial.Commands()) {
    push_clips += std::holds_alternative<PushClipCommand>(command) ? 1 : 0;
    pop_clips += std::holds_alternative<PopClipCommand>(command) ? 1 : 0;
  }
  REQUIRE(push_clips == 1);
  REQUIRE(pop_clips == 1);
  REQUIRE(ContainsText(initial, "First"));
  REQUIRE(ContainsText(initial, "Second"));
  REQUIRE(!ContainsText(initial, "Third"));
  const std::uint64_t scroll_measure_revision = root->measure_revision;
  const std::uint64_t scroll_layout_revision = root->layout_revision;
  const std::uint64_t content_measure_revision = root->children[0]->measure_revision;
  const std::uint64_t content_layout_revision = root->children[0]->layout_revision;

  const int requested_frames = platform.requested_frames;
  runtime.HandleScrollInput(
      ScrollInputEvent{
          {50.0F, 30.0F},
          0.0F,
          45.0F,
      }
  );
  REQUIRE(platform.requested_frames == requested_frames + 1);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 45.0F);
  REQUIRE(root->children[0]->layout_offset.y == 0.0F);
  REQUIRE(root->render_node.children_transform.translate_y == -45.0F);
  REQUIRE(root->measure_revision == scroll_measure_revision);
  REQUIRE(root->layout_revision == scroll_layout_revision);
  REQUIRE(root->children[0]->measure_revision == content_measure_revision);
  REQUIRE(root->children[0]->layout_revision == content_layout_revision);

  ClickAt(runtime, {50.0F, 50.0F});
  REQUIRE(scroll_clicked == "Third");

  runtime.InvalidateRoot();
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 45.0F);

  runtime.HandleScrollInput(
      ScrollInputEvent{
          {50.0F, 30.0F},
          0.0F,
          100.0F,
      }
  );
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 60.0F);

  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 20.0F);
}

TEST_CASE("Scroll input may consume the complete platform update before default scrolling") {
  consume_scroll_input = true;
  received_scroll_input.reset();

  TestPlatform platform;
  Runtime runtime{ScrollInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  const ScrollInputEvent input{
      .position = {50.0F, 40.0F},
      .delta_x = 12.0F,
      .delta_y = 30.0F,
      .modifiers = {.control = true},
  };
  REQUIRE(runtime.HandleScrollInput(input) == Point{12.0F, 30.0F});
  REQUIRE(received_scroll_input == input);
  REQUIRE(scroll_input_controller.Offset() == 0.0F);

  consume_scroll_input = false;
  REQUIRE(runtime.HandleScrollInput(input) == Point{0.0F, 30.0F});
  REQUIRE(scroll_input_controller.Offset() == 30.0F);

  REQUIRE(scroll_input_controller.ScrollTo(0.0F));
  runtime.BuildFrame();
  const Point boundary_consumption = runtime.HandleScrollInput({{50.0F, 40.0F}, 0.0F, -20.0F});
  REQUIRE(boundary_consumption == Point{});
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset == 0.0F);
}

TEST_CASE("Diagonal scroll input coordinates each axis independently") {
  TestPlatform platform;
  Runtime runtime{CrossAxisNestedScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(runtime.HandleScrollInput({{50.0F, 20.0F}, 25.0F, 30.0F}) == Point{25.0F, 30.0F});
  REQUIRE(cross_axis_inner_scroll.Offset() == 25.0F);
  REQUIRE(cross_axis_outer_scroll.Offset() == 30.0F);
}

TEST_CASE("Nested scroll hooks preserve pre offset post order and actual consumption") {
  scroll_hook_calls.clear();

  TestPlatform platform;
  Runtime runtime{NestedScrollHookApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE(hook_inner_scroll.ScrollTo(1000.0F));
  REQUIRE(hook_outer_scroll.ScrollTo(1000.0F));
  runtime.BuildFrame();
  scroll_hook_calls.clear();

  const Point consumed = runtime.HandleScrollInput({{50.0F, 50.0F}, 0.0F, 30.0F});
  REQUIRE(consumed.x == 0.0F);
  REQUIRE(consumed.y == 14.0F);
  REQUIRE(scroll_hook_calls == std::vector<std::string>{"outer.pre", "inner.pre", "inner.post", "outer.post"});

  Runtime invalid{
      +[]() -> View {
        return ScrollView {
          Spacer().With(huxerui::Frame{100.0F, 200.0F}),
        }.With(ScrollHook{"invalid", 20.0F, 0.0F});
      },
      platform,
  };
  invalid.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  invalid.BuildFrame();
  REQUIRE_THROWS_AS(invalid.HandleScrollInput({{50.0F, 30.0F}, 0.0F, 10.0F}), std::logic_error);
}

TEST_CASE("Scroll activity unifies indirect programmatic and direct changes") {
  scroll_activities.clear();

  TestPlatform platform;
  Runtime runtime{ScrollActivityApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandleScrollInput({{50.0F, 40.0F}, 0.0F, 20.0F});
  REQUIRE(scroll_activities.back().source == ScrollSource::Wheel);
  REQUIRE(scroll_activities.back().phase == ScrollPhase::Update);
  REQUIRE(scroll_activities.back().delta == 20.0F);
  REQUIRE(scroll_activities.back().metrics.offset == 20.0F);

  REQUIRE(activity_scroll.ScrollBy(10.0F));
  REQUIRE(scroll_activities.back().source == ScrollSource::Programmatic);
  REQUIRE(scroll_activities.back().phase == ScrollPhase::Update);
  REQUIRE(scroll_activities.back().metrics.offset == 30.0F);

  scroll_activities.clear();
  runtime.HandlePointerEvent({PointerEventType::Down, 100, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 100, {50.0F, 30.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 100, {50.0F, 30.0F}, PointerDeviceKind::Touch});
  REQUIRE(scroll_activities.size() >= 3);
  REQUIRE(scroll_activities.front().source == ScrollSource::Drag);
  REQUIRE(scroll_activities.front().phase == ScrollPhase::Begin);
  REQUIRE(scroll_activities[1].phase == ScrollPhase::Update);
  REQUIRE(scroll_activities.back().phase == ScrollPhase::End);
}

TEST_CASE("Direct touch overscroll keeps metrics clamped and settles its presentation displacement") {
  scroll_activities.clear();

  TestPlatform platform;
  Runtime runtime{ScrollActivityApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 101, {50.0F, 30.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 101, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  REQUIRE(activity_scroll.Offset() == 0.0F);
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset < 0.0F);
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->render_node.children_transform.translate_y > 0.0F);

  runtime.HandlePointerEvent({PointerEventType::Up, 101, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  for (int frame = 0; frame < 10 && runtime.RootNode()->scroll_state->overscroll_offset != 0.0F; ++frame) {
    platform.AdvanceTime(0.05);
    runtime.BuildFrame();
  }
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset == 0.0F);
  REQUIRE(activity_scroll.Offset() == 0.0F);
  const auto settlement = std::ranges::find_if(scroll_activities, [](const ScrollActivity& activity) {
    return activity.source == ScrollSource::Overscroll && activity.phase == ScrollPhase::Begin;
  });
  REQUIRE(settlement != scroll_activities.end());
  REQUIRE(scroll_activities.back().source == ScrollSource::Overscroll);
  REQUIRE(scroll_activities.back().phase == ScrollPhase::End);
}

TEST_CASE("Disabling an active scroll clears overscroll and cancels direct activity") {
  TestPlatform platform;
  Runtime runtime{DisableableOverscrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 103, {50.0F, 30.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 103, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset < 0.0F);

  disableable_scroll_enabled = false;
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset == 0.0F);
  REQUIRE_FALSE(runtime.RootNode()->interaction.enabled);
}

TEST_CASE("Reduced motion clears released overscroll without retained settlement") {
  TestPlatform platform;
  Runtime runtime{ReducedMotionOverscrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 104, {50.0F, 30.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 104, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 104, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  REQUIRE(runtime.RootNode()->children.front()->scroll_state->overscroll_offset < 0.0F);

  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children.front()->scroll_state->overscroll_offset == 0.0F);
}

TEST_CASE("Platform scroll defaults apply when a container has no explicit physics") {
  ScrollDefaultsPlatform platform;
  platform.scroll_defaults.overscroll_enabled = false;
  Runtime runtime{ScrollInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE_FALSE(runtime.RootNode()->layout_values.contains(typeid(ScrollPhysics)));
  REQUIRE_FALSE(detail::ResolveScrollPhysics(*runtime.RootNode()).overscroll_enabled);

  runtime.HandlePointerEvent({PointerEventType::Down, 102, {50.0F, 30.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 102, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 102, {50.0F, 60.0F}, PointerDeviceKind::Touch});
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset == 0.0F);
}

TEST_CASE("TestHorizontalScrollViewLayoutAndState") {
  TestPlatform platform;
  Runtime runtime{HorizontalScrollViewApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->scroll_state->axis == Axis::Horizontal);
  REQUIRE(root->scroll_state->content_width == 180.0F);
  REQUIRE(root->scroll_state->content_height == 40.0F);
  REQUIRE(horizontal_view_scroll.Metrics().axis == Axis::Horizontal);
  REQUIRE(horizontal_view_scroll.ViewportExtent() == 100.0F);
  REQUIRE(horizontal_view_scroll.ContentExtent() == 180.0F);
  REQUIRE(horizontal_view_scroll.MaxOffset() == 80.0F);
  const auto scroll_bar = huxerui::detail::ResolveScrollBarGeometry(*root);
  REQUIRE(scroll_bar.has_value());
  REQUIRE(scroll_bar->axis == Axis::Horizontal);

  runtime.HandleScrollInput(
      ScrollInputEvent{
          {50.0F, 20.0F},
          45.0F,
          0.0F,
      }
  );
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_x == 45.0F);
  REQUIRE(root->scroll_state->offset_y == 0.0F);
  REQUIRE(root->children[0]->layout_offset.x == 0.0F);
  REQUIRE(root->render_node.children_transform.translate_x == -45.0F);
  REQUIRE(horizontal_view_scroll.Offset() == 45.0F);
}

TEST_CASE("TestScrollControllerControlsVirtualListAndDisconnects") {
  scroll_observer_compositions = 0;

  TestPlatform platform;
  Runtime runtime{ControlledVirtualListApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(controlled_list_scroll.IsConnected());
  REQUIRE(root->scroll_state->offset_y == 40.0F);
  REQUIRE(controlled_list_scroll.Offset() == 40.0F);
  REQUIRE(controlled_list_scroll.MaxOffset() == 19900.0F);
  REQUIRE(controlled_list_scroll.ViewportExtent() == 100.0F);
  REQUIRE(controlled_list_scroll.ContentExtent() == 20000.0F);

  const int compositions_before_scroll = scroll_observer_compositions;
  REQUIRE(controlled_list_scroll.ScrollBy(20.0F));
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 60.0F);
  REQUIRE(controlled_list_scroll.Offset() == 60.0F);
  REQUIRE(scroll_observer_compositions > compositions_before_scroll);

  REQUIRE(controlled_list_scroll.ScrollToItem(std::size_t{50}, ScrollAlignment::Center));
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 960.0F);
  const auto centered =
      std::find(root->virtual_state->realized_indices.begin(), root->virtual_state->realized_indices.end(), std::size_t{50});
  REQUIRE(centered != root->virtual_state->realized_indices.end());
  const std::size_t centered_position = static_cast<std::size_t>(centered - root->virtual_state->realized_indices.begin());
  REQUIRE(root->children[centered_position]->layout_offset.y == 40.0F);

  REQUIRE(controlled_list_scroll.ScrollTo(0.0F));
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll_state->offset_y == 0.0F);

  show_controlled_scroll = false;
  runtime.BuildFrame();
  REQUIRE(!controlled_list_scroll.IsConnected());
  REQUIRE(!controlled_list_scroll.ScrollTo(100.0F));
}

TEST_CASE("TestScrollControllerExampleButtonsAndFollowUpFrame") {
  TestPlatform platform;
  Runtime runtime{ScrollControllerExampleApp, platform};
  runtime.SetWindowMetrics({.viewport = {640.0F, 560.0F}});
  const int frames_before_build = platform.requested_frames;
  runtime.BuildFrame();

  REQUIRE(platform.requested_frames == frames_before_build);
  REQUIRE(runtime.LastCommit().next_frame_deadline == platform.current_time);
  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->children.size() == 1);
  const auto* toolbar = root->children[0]->children[0].get();
  REQUIRE(toolbar->children.size() == 4);
  const auto* item_button = toolbar->children[1].get();
  const Rect item_button_bounds = item_button->PresentationBounds();

  ClickAt(
      runtime,
      {
          item_button_bounds.x + item_button_bounds.width * 0.5F,
          item_button_bounds.y + item_button_bounds.height * 0.5F,
      }
  );
  REQUIRE(example_scroll.Offset() > 0.0F);
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children[1]->scroll_state->offset_y > 0.0F);

  root = runtime.RootNode();
  toolbar = root->children[0]->children[0].get();
  const auto* top_button = toolbar->children[0].get();
  const Rect top_button_bounds = top_button->PresentationBounds();
  ClickAt(
      runtime,
      {
          top_button_bounds.x + top_button_bounds.width * 0.5F,
          top_button_bounds.y + top_button_bounds.height * 0.5F,
      }
  );
  REQUIRE(example_scroll.Offset() == 0.0F);
  REQUIRE(runtime.RootNode()->children[1]->scroll_state->offset_y == 0.0F);
}

TEST_CASE("TestScrollControllerRefinesVariableItemAlignmentAfterRealization") {
  TestPlatform platform;
  Runtime runtime{VariableExtentScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 400.0F}});
  runtime.BuildFrame();
  REQUIRE(variable_list_scroll.ScrollToItem(1000, ScrollAlignment::End));
  for (int frame = 0; frame < 8; ++frame) {
    runtime.BuildFrame();
  }
  REQUIRE(variable_list_scroll.Offset() == variable_list_scroll.MaxOffset());
  const auto* root = runtime.RootNode();
  REQUIRE(root->virtual_state->realized_indices.back() == 1000);
  REQUIRE(root->children.back()->layout_offset.y + root->children.back()->bounds.height == 400.0F);
  const auto settled = variable_list_scroll.Metrics();
  runtime.BuildFrame();
  REQUIRE(variable_list_scroll.Metrics() == settled);
}

TEST_CASE("TestScrollControllerDirectScrollingCancelsPendingItemAlignment") {
  TestPlatform platform;
  Runtime runtime{VariableExtentScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 400.0F}});
  runtime.BuildFrame();
  REQUIRE(variable_list_scroll.ScrollToItem(1000, ScrollAlignment::End));
  runtime.BuildFrame();
  SECTION("New offset") { REQUIRE(variable_list_scroll.ScrollTo(0.0F)); }
  SECTION("Wheel input") {
    runtime.HandleScrollInput({{40.0F, 40.0F}, 0.0F, -500.0F});
  }
  for (int frame = 0; frame < 8; ++frame) {
    runtime.BuildFrame();
  }
  REQUIRE(variable_list_scroll.Offset() < variable_list_scroll.MaxOffset() - 100.0F);
  const auto settled = variable_list_scroll.Metrics();
  runtime.BuildFrame();
  REQUIRE(variable_list_scroll.Metrics() == settled);
}

TEST_CASE("TestScrollControllerDoesNotReplayPendingAlignmentAfterRemount") {
  static ScrollController scroll;
  static bool visible;
  scroll = ScrollController{};
  visible = true;
  TestPlatform platform;
  Runtime runtime{[]() -> View {
    if (!visible) {
      return Text("Hidden");
    }
    return VirtualList(std::size_t{1001}, [](std::size_t index) {
      return Text::Format("Block {}", index).With(Frame{.height = index == 1000 ? 120.0F : 46.0F}).Key(index);
    }).EstimatedItemExtent(84.0F).Controller(scroll);
  }, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 400.0F}});
  runtime.BuildFrame();
  REQUIRE(scroll.ScrollToItem(1000, ScrollAlignment::End));
  visible = false;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE_FALSE(scroll.IsConnected());
  visible = true;
  runtime.InvalidateRoot();
  for (int frame = 0; frame < 8; ++frame) {
    runtime.BuildFrame();
  }
  REQUIRE(scroll.IsConnected());
  REQUIRE(scroll.Offset() == 0.0F);
}

TEST_CASE("TestScrollControllerControlsVirtualGridItems") {
  TestPlatform platform;
  Runtime runtime{ControlledVirtualGridApp, platform};
  runtime.SetWindowMetrics({.viewport = {90.0F, 48.0F}});
  runtime.BuildFrame();

  REQUIRE(controlled_grid_scroll.IsConnected());
  REQUIRE(controlled_grid_scroll.ScrollToItem(std::size_t{50}));
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 384.0F);
  const auto item =
      std::find(root->virtual_state->realized_indices.begin(), root->virtual_state->realized_indices.end(), std::size_t{50});
  REQUIRE(item != root->virtual_state->realized_indices.end());
  std::size_t item_position = static_cast<std::size_t>(item - root->virtual_state->realized_indices.begin());
  REQUIRE(root->children[item_position]->layout_offset.y == 0.0F);

  REQUIRE(controlled_grid_scroll.ScrollToItem(std::size_t{50}, ScrollAlignment::Center));
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 370.0F);
  const auto centered =
      std::find(root->virtual_state->realized_indices.begin(), root->virtual_state->realized_indices.end(), std::size_t{50});
  REQUIRE(centered != root->virtual_state->realized_indices.end());
  item_position = static_cast<std::size_t>(centered - root->virtual_state->realized_indices.begin());
  REQUIRE(root->children[item_position]->layout_offset.y == 14.0F);
}

TEST_CASE("TestScrollControllerControlsScrollView") {
  TestPlatform platform;
  Runtime runtime{ControlledScrollViewApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(controlled_view_scroll.IsConnected());
  REQUIRE(root->scroll_state->offset_y == 20.0F);
  REQUIRE(controlled_view_scroll.Offset() == 20.0F);
  REQUIRE(controlled_view_scroll.MaxOffset() == 300.0F);
  REQUIRE(controlled_view_scroll.ViewportExtent() == 100.0F);
  REQUIRE(controlled_view_scroll.ContentExtent() == 400.0F);

  REQUIRE(controlled_view_scroll.ScrollBy(30.0F));
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll_state->offset_y == 50.0F);
  REQUIRE(controlled_view_scroll.Offset() == 50.0F);
  REQUIRE(!controlled_view_scroll.ScrollToItem(std::size_t{3}));
}

TEST_CASE("IndexedPages retains an independent ScrollView offset for each page") {
  TestPlatform platform;
  Runtime runtime{IndexedScrollingApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(first_indexed_page_scroll.IsConnected());
  REQUIRE_FALSE(second_indexed_page_scroll.IsConnected());
  REQUIRE(first_indexed_page_scroll.ScrollTo(60.0F));
  runtime.BuildFrame();
  REQUIRE(first_indexed_page_scroll.Offset() == 60.0F);

  indexed_scroll_page = 1;
  runtime.BuildFrame();
  REQUIRE(second_indexed_page_scroll.IsConnected());
  REQUIRE(second_indexed_page_scroll.ScrollTo(35.0F));
  runtime.BuildFrame();
  REQUIRE(second_indexed_page_scroll.Offset() == 35.0F);

  indexed_scroll_page = 0;
  runtime.BuildFrame();
  REQUIRE(first_indexed_page_scroll.Offset() == 60.0F);
  REQUIRE(second_indexed_page_scroll.Offset() == 35.0F);
}

TEST_CASE("Pager uses direct drag without mapping wheel input and honors DragEnabled") {
  pager_proposals.clear();
  accept_pager_proposals = true;
  TestPlatform platform;
  Runtime runtime{InteractivePagerApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});
  runtime.BuildFrame();

  REQUIRE(runtime.HandleScrollInput({{50.0F, 40.0F}, 60.0F, 0.0F}) == Point{});
  REQUIRE(interactive_pager_page.Get() == 1);

  runtime.HandlePointerEvent({PointerEventType::Down, 301, {80.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Move, 301, {20.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children[2]->PresentationBounds().x == Catch::Approx(40.0F));
  REQUIRE_FALSE(runtime.RootNode()->children[2]->interaction.enabled);
  REQUIRE(std::ranges::none_of(runtime.LastCommit().semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Next";
  }));
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children[2]->PresentationBounds().x == Catch::Approx(80.0F));
  runtime.HandlePointerEvent({PointerEventType::Up, 301, {20.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  REQUIRE(pager_proposals == std::vector<std::size_t>{2});
  REQUIRE(interactive_pager_page.Get() == 2);

  runtime.HandlePointerEvent({PointerEventType::Down, 305, {80.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Move, 305, {20.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 305, {20.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  REQUIRE(pager_proposals == std::vector<std::size_t>{2});
  REQUIRE(runtime.RootNode()->children[2]->PresentationBounds().x == Catch::Approx(80.0F));
  REQUIRE(runtime.RootNode()->children[2]->interaction.enabled);
  platform.AdvanceTime(0.25);
  runtime.BuildFrame();
  runtime.BuildFrame();
  interactive_pager_drag_enabled = false;
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 302, {20.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Move, 302, {80.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 302, {80.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  REQUIRE(pager_proposals == std::vector<std::size_t>{2});
}

TEST_CASE("Pager returns to controlled selection when a proposal is rejected") {
  pager_proposals.clear();
  accept_pager_proposals = false;
  TestPlatform platform;
  Runtime runtime{InteractivePagerApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 303, {80.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Move, 303, {20.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 303, {20.0F, 40.0F}, PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  REQUIRE(pager_proposals == std::vector<std::size_t>{2});
  REQUIRE(interactive_pager_page.Get() == 1);

  runtime.BuildFrame();
  runtime.BuildFrame();
  platform.AdvanceTime(0.25);
  runtime.BuildFrame();
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children[1]->PresentationBounds().x == Catch::Approx(0.0F));
  REQUIRE_FALSE(runtime.RootNode()->children[2]->participates_in_layout);
}

TEST_CASE("Pager composes vertical paging with explicit reversal") {
  pager_proposals.clear();
  TestPlatform platform;
  Runtime runtime{ReversedVerticalPagerApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 304, {50.0F, 70.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Move, 304, {50.0F, 10.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 304, {50.0F, 10.0F}, PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  REQUIRE(pager_proposals == std::vector<std::size_t>{0});
  REQUIRE(reversed_pager_page.Get() == 0);
}

TEST_CASE("Pager maps semantic scrolling to one controlled page proposal") {
  pager_proposals.clear();
  accept_pager_proposals = true;
  TestPlatform platform;
  Runtime runtime{InteractivePagerApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});
  runtime.BuildFrame();

  REQUIRE(runtime.LastCommit().semantic_frame != nullptr);
  const auto pager = std::ranges::find(
      runtime.LastCommit().semantic_frame->nodes, SemanticRole::ScrollView, &SemanticNode::role
  );
  REQUIRE(pager != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE((pager->actions & SemanticActionMask(SemanticActionKind::Scroll)) != 0);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      pager->id, SemanticAction{SemanticActionKind::Scroll, Point{100.0F, 0.0F}}
  ));
  REQUIRE(pager_proposals == std::vector<std::size_t>{2});
  REQUIRE(interactive_pager_page.Get() == 2);
}

TEST_CASE("Pager uses touch release velocity and returns after pointer cancellation") {
  SECTION("velocity") {
    pager_proposals.clear();
    accept_pager_proposals = true;
    TestPlatform platform;
    Runtime runtime{InteractivePagerApp, platform};
    runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});
    runtime.BuildFrame();

    runtime.HandlePointerEvent({PointerEventType::Down, 306, {80.0F, 40.0F}, PointerDeviceKind::Touch});
    platform.AdvanceTime(0.01);
    runtime.HandlePointerEvent({PointerEventType::Move, 306, {60.0F, 40.0F}, PointerDeviceKind::Touch});
    runtime.HandlePointerEvent({PointerEventType::Up, 306, {60.0F, 40.0F}, PointerDeviceKind::Touch});
    runtime.BuildFrame();
    REQUIRE(pager_proposals == std::vector<std::size_t>{2});
  }

  SECTION("cancellation") {
    pager_proposals.clear();
    accept_pager_proposals = true;
    TestPlatform platform;
    Runtime runtime{InteractivePagerApp, platform};
    runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});
    runtime.BuildFrame();

    runtime.HandlePointerEvent({PointerEventType::Down, 307, {80.0F, 40.0F}, PointerDeviceKind::Touch});
    runtime.HandlePointerEvent({PointerEventType::Move, 307, {20.0F, 40.0F}, PointerDeviceKind::Touch});
    runtime.HandlePointerEvent({PointerEventType::Cancel, 307, {20.0F, 40.0F}, PointerDeviceKind::Touch});
    runtime.BuildFrame();
    REQUIRE(pager_proposals.empty());
    REQUIRE(interactive_pager_page.Get() == 1);
  }
}

TEST_CASE("RefreshBox transfers direct pull displacement into controlled refresh state") {
  refresh_requests = 0;
  accept_refresh_requests = true;
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{RefreshBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 401, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 401, {50.0F, 240.0F}, PointerDeviceKind::Touch});
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset < 0.0F);
  runtime.HandlePointerEvent({PointerEventType::Up, 401, {50.0F, 240.0F}, PointerDeviceKind::Touch});

  REQUIRE(refresh_requests == 1);
  REQUIRE(refresh_box_refreshing.Get());
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset == 0.0F);
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children.front()->PresentationBounds().y > 0.0F);

  const auto refresh_node = std::ranges::find(
      runtime.LastCommit().semantic_frame->nodes, SemanticRole::ScrollView, &SemanticNode::role
  );
  REQUIRE(refresh_node != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(refresh_node->busy == true);
  REQUIRE(refresh_node->custom_actions.empty());

  refresh_box_refreshing = false;
  runtime.BuildFrame();
  platform.AdvanceTime(0.25);
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children.front()->PresentationBounds().y == Catch::Approx(0.0F));
}

TEST_CASE("RefreshBox ignores short, canceled, trailing, and non-drag input") {
  refresh_requests = 0;
  accept_refresh_requests = false;
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{RefreshBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 402, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 402, {50.0F, 40.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 402, {50.0F, 40.0F}, PointerDeviceKind::Touch});
  REQUIRE(refresh_requests == 0);

  runtime.HandlePointerEvent({PointerEventType::Down, 403, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 403, {50.0F, 240.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Cancel, 403, {50.0F, 240.0F}, PointerDeviceKind::Touch});
  REQUIRE(refresh_requests == 0);

  runtime.HandlePointerEvent({PointerEventType::Down, 404, {50.0F, 80.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 404, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 404, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  REQUIRE(refresh_requests == 0);
  static_cast<void>(runtime.HandleScrollInput({{50.0F, 50.0F}, 0.0F, -200.0F}));
  REQUIRE(refresh_requests == 0);
}

TEST_CASE("RefreshBox preserves an active pull when controlled refreshing starts programmatically") {
  refresh_requests = 0;
  accept_refresh_requests = true;
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{RefreshBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 406, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 406, {50.0F, 80.0F}, PointerDeviceKind::Touch});
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset < 0.0F);

  refresh_box_refreshing = true;
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll_state->overscroll_offset == 0.0F);
  REQUIRE(runtime.RootNode()->children.front()->PresentationBounds().y > 0.0F);
  REQUIRE(refresh_requests == 0);
}

TEST_CASE("RefreshBox exposes one localized semantic refresh action") {
  refresh_requests = 0;
  accept_refresh_requests = true;
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{RefreshBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  const auto refresh_node = std::ranges::find(
      runtime.LastCommit().semantic_frame->nodes, SemanticRole::ScrollView, &SemanticNode::role
  );
  REQUIRE(refresh_node != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(refresh_node->busy == false);
  REQUIRE(refresh_node->custom_actions == std::vector<std::pair<std::uint64_t, std::string>>{{1, "Refresh"}});
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      refresh_node->id, SemanticAction{SemanticActionKind::Custom, std::uint64_t{1}}
  ));
  REQUIRE(refresh_requests == 1);
  REQUIRE(refresh_box_refreshing.Get());
}

TEST_CASE("RefreshBox receives only the pull remaining after nested content reaches its leading edge") {
  refresh_requests = 0;
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{NestedRefreshBoxApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE(refresh_content_scroll.Offset() == 80.0F);

  runtime.HandlePointerEvent({PointerEventType::Down, 405, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 405, {50.0F, 400.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 405, {50.0F, 400.0F}, PointerDeviceKind::Touch});

  REQUIRE(refresh_content_scroll.Offset() == 0.0F);
  REQUIRE(refresh_requests == 1);
}

TEST_CASE("RefreshBox validates its required content") {
  REQUIRE_THROWS_AS(RefreshBox(View{}, false), std::invalid_argument);

  TestPlatform platform;
  Runtime runtime{InvalidRefreshBoxStyleApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
}

TEST_CASE("TestGrowScrollViewRetainsOffsetWhenDescendantScopeRecomposes") {
  TestPlatform platform;
  Runtime runtime{ScopedScrollViewApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  auto* scroll = runtime.RootNode()->children[1].get();
  REQUIRE(scroll->scroll_state->content_height - scroll->measured_size.height == 30.0F);
  const ScrollMetrics initial_metrics{
      .axis = Axis::Vertical,
      .offset = 0.0F,
      .maximum_offset = 30.0F,
      .viewport_extent = 60.0F,
      .content_extent = 90.0F,
  };
  REQUIRE(scoped_grow_scroll.Metrics() == initial_metrics);

  runtime.HandleScrollInput({{50.0F, 60.0F}, 0.0F, 20.0F});
  runtime.BuildFrame();
  REQUIRE(scroll->scroll_state->offset_y == 20.0F);
  REQUIRE(scoped_grow_scroll.Offset() == 20.0F);

  scoped_scroll_content_changed = true;
  runtime.BuildFrame();
  REQUIRE(scroll->scroll_state->offset_y == 20.0F);
  REQUIRE(scoped_grow_scroll.Offset() == 20.0F);

  scoped_scroll_content_height = 10.0F;
  runtime.BuildFrame();
  REQUIRE(scroll->scroll_state->offset_y == 0.0F);
  const ScrollMetrics clamped_metrics{
      .axis = Axis::Vertical,
      .offset = 0.0F,
      .maximum_offset = 0.0F,
      .viewport_extent = 60.0F,
      .content_extent = 30.0F,
  };
  REQUIRE(scoped_grow_scroll.Metrics() == clamped_metrics);
}

} // namespace huxerui::test
