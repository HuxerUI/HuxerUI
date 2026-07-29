#include "runtime_test_support.h"

namespace huxerui::test {

std::string scroll_clicked;
State<bool> show_controlled_scroll;
ScrollState controlled_list_scroll;
ScrollState controlled_grid_scroll;
ScrollState controlled_view_scroll;
ScrollState example_scroll;
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

View ScrollObserverItem(int index, ScrollState scroll) {
  HUXERUI_SCOPE({
    if (index == 0) {
      ++scroll_observer_compositions;
    }
    return Text::Format("{}:{}", index, scroll.Offset()).With(huxerui::Frame{100.0F, 20.0F});
  });
}

View ControlledVirtualListApp() {
  auto visible = UseState(true);
  auto scroll = UseScrollState(40.0F);
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
      .ScrollState(scroll);
}

View ScrollStateToolbar(ScrollState scroll) {
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

View ScrollStateExampleApp() {
  auto scroll = UseScrollState();
  example_scroll = scroll;
  return Column{
      ScrollStateToolbar(scroll),
      VirtualList(
          std::size_t{1000},
          [](std::size_t index) {
            return Text::Format("Item {}", index + 1).With(huxerui::Frame{100.0F, 40.0F}).Key(index);
          }
      )
          .ItemExtent(48.0F)
          .ScrollState(scroll)
          .With(huxerui::Spacing{8.0F}, huxerui::Grow{}),
  }
      .With(huxerui::Padding{24.0F}, huxerui::Spacing{12.0F});
}

View ControlledVirtualGridApp() {
  auto scroll = UseScrollState();
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
      .ScrollState(scroll);
}

View ControlledScrollViewApp() {
  auto scroll = UseScrollState(20.0F);
  controlled_view_scroll = scroll;
  std::vector<int> items(20);
  std::iota(items.begin(), items.end(), 0);
  return ScrollView{
      Column{
          ForEach(
              items,
              [](int index) {
                return Text(std::to_string(index)).With(huxerui::Frame{100.0F, 20.0F});
              }
          ),
      },
  }
      .ScrollState(scroll);
}

TEST_CASE("TestScrollViewLayoutClipAndHitTest") {
  scroll_clicked.clear();

  TestPlatform platform;
  Runtime runtime{ScrollViewApp, platform};
  runtime.SetViewport({100.0F, 60.0F});
  const DisplayList& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->measured_size.width == 100.0F);
  REQUIRE(root->measured_size.height == 60.0F);
  REQUIRE(root->scroll != nullptr);
  REQUIRE(root->scroll->content_height == 120.0F);
  REQUIRE(root->scroll->offset_y == 0.0F);
  REQUIRE(root->children[0]->scroll == nullptr);
  REQUIRE(root->children[0]->frame.y == 0.0F);

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

  const int requested_frames = platform.requested_frames;
  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 30.0F},
      0.0F,
      45.0F,
  });
  REQUIRE(platform.requested_frames == requested_frames + 1);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 45.0F);
  REQUIRE(root->children[0]->frame.y == -45.0F);

  ClickAt(runtime, {50.0F, 50.0F});
  REQUIRE(scroll_clicked == "Third");

  runtime.InvalidateRoot();
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 45.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 30.0F},
      0.0F,
      100.0F,
  });
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 60.0F);

  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 20.0F);
}

TEST_CASE("TestScrollStateControlsVirtualListAndDisconnects") {
  scroll_observer_compositions = 0;

  TestPlatform platform;
  Runtime runtime{ControlledVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(controlled_list_scroll.IsConnected());
  REQUIRE(root->scroll->offset_y == 40.0F);
  REQUIRE(controlled_list_scroll.Offset() == 40.0F);
  REQUIRE(controlled_list_scroll.MaxOffset() == 19900.0F);
  REQUIRE(controlled_list_scroll.ViewportExtent() == 100.0F);
  REQUIRE(controlled_list_scroll.ContentExtent() == 20000.0F);

  const int compositions_before_scroll = scroll_observer_compositions;
  REQUIRE(controlled_list_scroll.ScrollBy(20.0F));
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 60.0F);
  REQUIRE(controlled_list_scroll.Offset() == 60.0F);
  REQUIRE(scroll_observer_compositions > compositions_before_scroll);

  REQUIRE(controlled_list_scroll.ScrollToItem(std::size_t{50}, ScrollAlignment::Center));
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 960.0F);
  const auto centered =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{50});
  REQUIRE(centered != root->virtual_state->child_indices.end());
  const std::size_t centered_position = static_cast<std::size_t>(centered - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[centered_position]->frame.y == 40.0F);

  REQUIRE(controlled_list_scroll.ScrollTo(0.0F));
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll->offset_y == 0.0F);

  show_controlled_scroll = false;
  runtime.BuildFrame();
  REQUIRE(!controlled_list_scroll.IsConnected());
  REQUIRE(!controlled_list_scroll.ScrollTo(100.0F));
}

TEST_CASE("TestScrollStateExampleButtonsAndFollowUpFrame") {
  TestPlatform platform;
  Runtime runtime{ScrollStateExampleApp, platform};
  runtime.SetViewport({640.0F, 560.0F});
  const int frames_before_build = platform.requested_frames;
  runtime.BuildFrame();

  REQUIRE(platform.requested_frames == frames_before_build + 1);
  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->children.size() == 1);
  const auto* toolbar = root->children[0]->children[0].get();
  REQUIRE(toolbar->children.size() == 4);
  const auto* item_button = toolbar->children[1].get();

  ClickAt(
      runtime,
      {
          item_button->frame.x + item_button->frame.width * 0.5F,
          item_button->frame.y + item_button->frame.height * 0.5F,
      }
  );
  REQUIRE(example_scroll.Offset() > 0.0F);
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children[1]->scroll->offset_y > 0.0F);

  root = runtime.RootNode();
  toolbar = root->children[0]->children[0].get();
  const auto* top_button = toolbar->children[0].get();
  ClickAt(
      runtime,
      {
          top_button->frame.x + top_button->frame.width * 0.5F,
          top_button->frame.y + top_button->frame.height * 0.5F,
      }
  );
  REQUIRE(example_scroll.Offset() == 0.0F);
  REQUIRE(runtime.RootNode()->children[1]->scroll->offset_y == 0.0F);
}

TEST_CASE("TestScrollStateControlsVirtualGridItems") {
  TestPlatform platform;
  Runtime runtime{ControlledVirtualGridApp, platform};
  runtime.SetViewport({90.0F, 48.0F});
  runtime.BuildFrame();

  REQUIRE(controlled_grid_scroll.IsConnected());
  REQUIRE(controlled_grid_scroll.ScrollToItem(std::size_t{50}));
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 384.0F);
  const auto item =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{50});
  REQUIRE(item != root->virtual_state->child_indices.end());
  std::size_t item_position = static_cast<std::size_t>(item - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[item_position]->frame.y == 0.0F);

  REQUIRE(controlled_grid_scroll.ScrollToItem(std::size_t{50}, ScrollAlignment::Center));
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll->offset_y == 370.0F);
  const auto centered =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{50});
  REQUIRE(centered != root->virtual_state->child_indices.end());
  item_position = static_cast<std::size_t>(centered - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[item_position]->frame.y == 14.0F);
}

TEST_CASE("TestScrollStateControlsScrollView") {
  TestPlatform platform;
  Runtime runtime{ControlledScrollViewApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(controlled_view_scroll.IsConnected());
  REQUIRE(root->scroll->offset_y == 20.0F);
  REQUIRE(controlled_view_scroll.Offset() == 20.0F);
  REQUIRE(controlled_view_scroll.MaxOffset() == 300.0F);
  REQUIRE(controlled_view_scroll.ViewportExtent() == 100.0F);
  REQUIRE(controlled_view_scroll.ContentExtent() == 400.0F);

  REQUIRE(controlled_view_scroll.ScrollBy(30.0F));
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll->offset_y == 50.0F);
  REQUIRE(controlled_view_scroll.Offset() == 50.0F);
  REQUIRE(!controlled_view_scroll.ScrollToItem(std::size_t{3}));
}

} // namespace huxerui::test
