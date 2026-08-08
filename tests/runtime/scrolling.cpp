#include "runtime_test_support.h"

namespace huxerui::test {

std::string scroll_clicked;
State<bool> show_controlled_scroll;
ScrollController controlled_list_scroll;
ScrollController controlled_grid_scroll;
ScrollController controlled_view_scroll;
ScrollController horizontal_view_scroll;
ScrollController example_scroll;
ScrollController scoped_grow_scroll;
State<bool> scoped_scroll_content_changed;
State<float> scoped_scroll_content_height;
int scroll_observer_compositions = 0;

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

TEST_CASE("TestScrollViewLayoutClipAndHitTest") {
  scroll_clicked.clear();

  TestPlatform platform;
  Runtime runtime{ScrollViewApp, platform};
  runtime.SetViewport({100.0F, 60.0F});
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
  runtime.HandleScrollEvent(
      ScrollEvent{
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

  runtime.HandleScrollEvent(
      ScrollEvent{
          {50.0F, 30.0F},
          0.0F,
          100.0F,
      }
  );
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 60.0F);

  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_y == 20.0F);
}

TEST_CASE("TestHorizontalScrollViewLayoutAndState") {
  TestPlatform platform;
  Runtime runtime{HorizontalScrollViewApp, platform};
  runtime.SetViewport({100.0F, 40.0F});
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

  runtime.HandleScrollEvent(
      ScrollEvent{
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
  runtime.SetViewport({100.0F, 100.0F});
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
  runtime.SetViewport({640.0F, 560.0F});
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

TEST_CASE("TestScrollControllerControlsVirtualGridItems") {
  TestPlatform platform;
  Runtime runtime{ControlledVirtualGridApp, platform};
  runtime.SetViewport({90.0F, 48.0F});
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
  runtime.SetViewport({100.0F, 100.0F});
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

TEST_CASE("TestGrowScrollViewRetainsOffsetWhenDescendantScopeRecomposes") {
  TestPlatform platform;
  Runtime runtime{ScopedScrollViewApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
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

  runtime.HandleScrollEvent({{50.0F, 60.0F}, 0.0F, 20.0F});
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
