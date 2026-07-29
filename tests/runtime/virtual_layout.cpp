#include "runtime_test_support.h"

namespace huxerui::test {

int virtual_item_factory_calls = 0;
int virtual_grid_factory_calls = 0;
int built_in_grid_factory_calls = 0;
State<std::vector<int>> virtual_reorder_items;
State<std::vector<int>> virtual_unkeyed_items;
State<bool> variable_height_expanded;
State<bool> variable_grid_height_expanded;
State<bool> horizontal_virtual_list;
ScrollState custom_virtual_scroll;

class TestVirtualStrip final : public VirtualLayout<TestVirtualStrip> {
public:
  using VirtualLayout::VirtualLayout;

  static VirtualLayoutResult Measure(VirtualLayoutContext &context, MountedNode &node,
                                     huxerui::Constraints constraints) {
    constexpr float item_extent = 25.0F;
    const auto viewport = context.Viewport();
    const std::size_t count = context.ItemCount();
    const std::size_t first =
        count == 0 ? 0
                   : std::min(count - 1,
                              static_cast<std::size_t>(std::floor(std::max(0.0F, viewport.offset.y) / item_extent)));
    const std::size_t visible_count = static_cast<std::size_t>(std::ceil(viewport.size.height / item_extent)) + 1;
    const std::size_t last = std::min(count, first + visible_count);

    VirtualLayoutResult result;
    for (std::size_t index = first; index < last; ++index) {
      MountedNode &item = context.Item(index);
      static_cast<void>(context.Measure(item, constraints.LooseHeight().TightHeight(item_extent)));
      result.Place(item, {0.0F, static_cast<float>(index) * item_extent});
    }

    const float content_height = static_cast<float>(count) * item_extent;
    const Size size = constraints.Constrain({constraints.max_width, content_height});
    result.SetAxis(Axis::Vertical).SetSize(size).SetContentSize({size.width, content_height});
    static_cast<void>(node);
    return result;
  }

  static std::optional<float> ScrollOffsetForItem(MountedNode &node, std::size_t index, ScrollAlignment alignment,
                                                  float viewport_extent) {
    constexpr float item_extent = 25.0F;
    const float start = static_cast<float>(index) * item_extent;
    static_cast<void>(node);
    switch (alignment) {
    case ScrollAlignment::Center:
      return start - (viewport_extent - item_extent) * 0.5F;
    case ScrollAlignment::End:
      return start - (viewport_extent - item_extent);
    case ScrollAlignment::Start:
      return start;
    }
    return start;
  }
};

struct TestGridSpan {
  using Value = std::size_t;
};

struct TestGridSpans {
  using Value = std::vector<std::size_t>;
};

class TestVirtualGrid final : public VirtualLayout<TestVirtualGrid> {
public:
  using VirtualLayout::VirtualLayout;

  TestVirtualGrid Spans(std::vector<std::size_t> spans) && {
    SetLayoutValue(typeid(TestGridSpans), std::move(spans));
    return std::move(*this);
  }

  static VirtualLayoutResult Measure(VirtualLayoutContext &context, MountedNode &node,
                                     huxerui::Constraints constraints) {
    constexpr float minimum_cell_width = 30.0F;
    constexpr float row_height = 20.0F;
    constexpr float cache_extent = row_height;
    const huxerui::VirtualViewport viewport = context.Viewport();
    const std::size_t columns =
        std::max(std::size_t{1}, static_cast<std::size_t>(std::floor(viewport.size.width / minimum_cell_width)));
    const float cell_width = viewport.size.width / static_cast<float>(columns);
    const auto *spans = node.LayoutValue<TestGridSpans>();
    auto &cache = node.Cache<GridCache>();

    const bool had_layout = cache.initialized;
    const bool columns_changed = had_layout && cache.columns != columns;
    const std::size_t previous_anchor = cache.anchor_index;
    const float previous_anchor_delta = cache.anchor_delta;
    cache.Prepare(context.ItemCount(), columns, spans == nullptr ? std::vector<std::size_t>{} : *spans);

    float scroll_offset = viewport.offset.y;
    if (columns_changed && previous_anchor < cache.cells.size()) {
      scroll_offset = static_cast<float>(cache.cells[previous_anchor].row) * row_height + previous_anchor_delta;
    }
    const float content_height = static_cast<float>(cache.row_count) * row_height;
    scroll_offset = std::clamp(scroll_offset, 0.0F, std::max(0.0F, content_height - viewport.size.height));

    const std::size_t visible_row = static_cast<std::size_t>(std::floor(scroll_offset / row_height));
    cache.anchor_index = cache.FirstIndexInRow(visible_row);
    cache.anchor_delta = scroll_offset - static_cast<float>(visible_row) * row_height;

    const std::size_t first_row =
        static_cast<std::size_t>(std::floor(std::max(0.0F, scroll_offset - cache_extent) / row_height));
    const std::size_t last_row = std::min(
        cache.row_count,
        static_cast<std::size_t>(std::ceil((scroll_offset + viewport.size.height + cache_extent) / row_height)));

    VirtualLayoutResult result;
    for (std::size_t index = 0; index < cache.cells.size(); ++index) {
      const GridCell &cell = cache.cells[index];
      if (cell.row < first_row || cell.row >= last_row) {
        continue;
      }

      MountedNode &item = context.Item(index);
      if (item.LayoutValueOr<TestGridSpan>(std::size_t{1}) != cell.span) {
        throw std::logic_error("HuxerUI test virtual grid item span does not match its plan");
      }
      static_cast<void>(context.Measure(
          item, constraints.Loose().TightWidth(cell_width * static_cast<float>(cell.span)).TightHeight(row_height)));
      result.Place(item, {
                             static_cast<float>(cell.column) * cell_width,
                             static_cast<float>(cell.row) * row_height,
                         });
    }

    const Size size = constraints.Constrain({viewport.size.width, content_height});
    return result.SetAxis(Axis::Vertical)
        .SetSize(size)
        .SetContentSize({size.width, content_height})
        .SetScrollOffset(scroll_offset);
  }

private:
  struct GridCell {
    std::size_t row;
    std::size_t column;
    std::size_t span;
  };

  struct GridCache {
    void Prepare(std::size_t item_count, std::size_t next_columns, const std::vector<std::size_t> &next_spans) {
      if (initialized && columns == next_columns && spans == next_spans && cells.size() == item_count) {
        return;
      }

      columns = next_columns;
      spans = next_spans;
      cells.clear();
      cells.reserve(item_count);
      std::size_t row = 0;
      std::size_t column = 0;
      for (std::size_t index = 0; index < item_count; ++index) {
        const std::size_t requested_span = index < spans.size() ? spans[index] : std::size_t{1};
        const std::size_t span = std::clamp(requested_span, std::size_t{1}, columns);
        if (column > 0 && column + span > columns) {
          ++row;
          column = 0;
        }
        cells.push_back({row, column, span});
        column += span;
        if (column == columns) {
          ++row;
          column = 0;
        }
      }
      row_count = row + (column > 0 ? 1 : 0);
      initialized = true;
    }

    [[nodiscard]] std::size_t FirstIndexInRow(std::size_t row) const {
      const auto found =
          std::find_if(cells.begin(), cells.end(), [row](const GridCell &cell) { return cell.row >= row; });
      return found == cells.end() ? cells.size() : static_cast<std::size_t>(found - cells.begin());
    }

    bool initialized = false;
    std::size_t columns = 0;
    std::vector<std::size_t> spans;
    std::vector<GridCell> cells;
    std::size_t row_count = 0;
    std::size_t anchor_index = 0;
    float anchor_delta = 0.0F;
  };
};

View StatefulListRow(int index) {
  HUXERUI_SCOPE({
    auto taps = UseState(0);
    return Button(std::to_string(index) + ":" + std::to_string(taps.Get()))
        .With(huxerui::Frame{100.0F, 20.0F})
        .OnClick([taps] { taps += 1; });
  });
}

View StatefulForEachScrollApp() {
  std::vector<int> items(100);
  std::iota(items.begin(), items.end(), 0);
  return ScrollView{
      Column{
          ForEach(items, [](int index) { return StatefulListRow(index).Key(index); }),
      },
  };
}

View VirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items,
                     [](int index) {
                       ++virtual_item_factory_calls;
                       return Text(std::to_string(index)).Key(index);
                     })
      .ItemExtent(20.0F);
}

View StatefulVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items, [](int index) { return StatefulListRow(index).Key(index); }).ItemExtent(20.0F);
}

View ReorderableStatefulVirtualListApp() {
  std::vector<int> initial_items(100);
  std::iota(initial_items.begin(), initial_items.end(), 0);
  auto items = UseState(std::move(initial_items));
  virtual_reorder_items = items;
  return VirtualList(items, [](int index) { return StatefulListRow(index).Key(index); }).ItemExtent(20.0F);
}

View UnkeyedStatefulVirtualListApp() {
  std::vector<int> initial_items(100);
  std::iota(initial_items.begin(), initial_items.end(), 0);
  auto items = UseState(std::move(initial_items));
  virtual_unkeyed_items = items;
  return VirtualList(items, [](int index) { return StatefulListRow(index); }).ItemExtent(20.0F);
}

View VariableVirtualListApp() {
  auto expanded = UseState(false);
  variable_height_expanded = expanded;
  const bool first_expanded = expanded.Get();
  std::vector<int> items(100);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items, [first_expanded](int index) {
    const float height = index == 0 && first_expanded ? 40.0F : index % 2 == 0 ? 20.0F : 40.0F;
    return Text(std::to_string(index)).With(huxerui::Frame{100.0F, height}).Key(index);
  });
}

View TinyVariableVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(
      items, [](int index) { return Text(std::to_string(index)).With(huxerui::Frame{100.0F, 1.0F}).Key(index); });
}

View FixedHorizontalVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(
             items,
             [](int index) { return Text(std::to_string(index)).With(huxerui::Frame{20.0F, 100.0F}).Key(index); })
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(20.0F);
}

View VariableHorizontalVirtualListApp() {
  std::vector<int> items(100);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items,
                     [](int index) {
                       const float width = index % 2 == 0 ? 20.0F : 40.0F;
                       return Text(std::to_string(index)).With(huxerui::Frame{width, 100.0F}).Key(index);
                     })
      .ScrollAxis(Axis::Horizontal);
}

View StatefulHorizontalVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items, [](int index) { return StatefulListRow(index).Key(index); })
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(100.0F);
}

View VirtualStateListApp() {
  auto items = UseState(std::vector<int>{
      7,
      8,
      9,
  });
  return VirtualList(items, [](int index) { return Text(std::to_string(index)).Key(index); }).ItemExtent(20.0F);
}

View CustomVirtualLayoutApp() {
  auto scroll = UseScrollState();
  custom_virtual_scroll = scroll;
  return TestVirtualStrip(std::size_t{100}, [](std::size_t index) { return Text(std::to_string(index)).Key(index); })
      .ScrollState(scroll)
      .With(huxerui::ScrollBar{}, huxerui::Padding{0.0F});
}

View CustomVirtualGridApp() {
  std::vector<std::size_t> spans(200, std::size_t{1});
  for (std::size_t index = 0; index < spans.size(); index += 7) {
    spans[index] = 2;
  }

  return TestVirtualGrid(
             spans.size(),
             [spans](std::size_t index) {
               ++virtual_grid_factory_calls;
               return StatefulListRow(static_cast<int>(index)).LayoutValue<TestGridSpan>(spans[index]).Key(index);
             })
      .With(huxerui::Padding{0.0F})
      .Spans(std::move(spans));
}

View BuiltInVirtualGridApp() {
  std::vector<std::size_t> spans(200, std::size_t{1});
  for (std::size_t index = 0; index < spans.size(); index += 7) {
    spans[index] = 2;
  }

  return VirtualGrid(spans.size(),
                     [](std::size_t index) {
                       ++built_in_grid_factory_calls;
                       return StatefulListRow(static_cast<int>(index)).Key(index);
                     })
      .Columns(GridColumns::Adaptive(30.0F))
      .RowExtent(20.0F)
      .RowSpacing(4.0F)
      .ColumnSpacing(5.0F)
      .CacheExtent(0.0F)
      .ItemSpans(std::move(spans));
}

View VariableVirtualGridApp() {
  auto expanded = UseState(false);
  variable_grid_height_expanded = expanded;
  const bool first_expanded = expanded.Get();
  return VirtualGrid(std::size_t{100},
                     [first_expanded](std::size_t index) {
                       float height = 0.0F;
                       switch (index % 4) {
                       case 0:
                         height = index == 0 && first_expanded ? 60.0F : 20.0F;
                         break;
                       case 1:
                         height = 40.0F;
                         break;
                       case 2:
                         height = 30.0F;
                         break;
                       case 3:
                         height = 10.0F;
                         break;
                       }
                       return Text(std::to_string(index)).With(huxerui::Frame{100.0F, height}).Key(index);
                     })
      .Columns(GridColumns::Fixed(2))
      .EstimatedRowExtent(35.0F)
      .RowSpacing(5.0F);
}

View AdaptiveAxisVirtualListApp() {
  auto horizontal = UseState(false);
  horizontal_virtual_list = horizontal;
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(
             items, [](int index) { return Text(std::to_string(index)).With(huxerui::Frame{20.0F, 20.0F}).Key(index); })
      .ScrollAxis(horizontal.Get() ? Axis::Horizontal : Axis::Vertical)
      .ItemExtent(20.0F);
}

TEST_CASE("TestForEachStateSurvivesScrolling") {
  TestPlatform platform;
  Runtime runtime{StatefulForEachScrollApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  auto *first_button = root->children[0]->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->children[0]->text == "0:4");

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      -1000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 0.0F);
  REQUIRE(root->children[0]->children[0]->children[0]->text == "0:4");
}

TEST_CASE("TestVirtualListVirtualization") {
  virtual_item_factory_calls = 0;

  TestPlatform platform;
  Runtime runtime{VirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  const DisplayList &initial = runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->virtual_state->source.size == 1000);
  REQUIRE(root->children.size() < root->virtual_state->source.size);
  const auto materialized_items = root->virtual_state->item_views.size();
  REQUIRE(materialized_items == root->children.size());
  REQUIRE(virtual_item_factory_calls == static_cast<int>(materialized_items));
  REQUIRE(!root->virtual_state->child_indices.empty());
  REQUIRE(root->virtual_state->child_indices.front() == 0);
  REQUIRE(root->scroll_content_height == 20000.0F);
  REQUIRE(FirstText(initial) == "0");
  REQUIRE(ContainsText(initial, "4"));
  REQUIRE(!ContainsText(initial, "5"));

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  const DisplayList &scrolled = runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 1000.0F);
  REQUIRE(root->virtual_state->child_indices.front() == 40);
  REQUIRE(root->virtual_state->child_indices.back() == 65);
  REQUIRE(!root->virtual_state->saved_state);
  REQUIRE(virtual_item_factory_calls < 1000);
  REQUIRE(FirstText(scrolled) == "50");
  REQUIRE(ContainsText(scrolled, "54"));
  REQUIRE(!ContainsText(scrolled, "55"));

  const std::size_t visible_position = 50 - root->virtual_state->child_indices.front();
  REQUIRE(root->children[visible_position]->frame.y == 0.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      50000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 19900.0F);
  REQUIRE(root->virtual_state->child_indices.back() == 999);

  Runtime state_runtime{VirtualStateListApp, platform};
  state_runtime.SetViewport({100.0F, 40.0F});
  const DisplayList &state_list = state_runtime.BuildFrame();
  REQUIRE(FirstText(state_list) == "7");
}

TEST_CASE("TestVirtualListStateSurvivesCacheEviction") {
  TestPlatform platform;
  Runtime runtime{StatefulVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  auto *first_button = root->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->text == "0:4");

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->virtual_state->child_indices.front() > 0);
  REQUIRE(root->children.size() < root->virtual_state->source.size);
  REQUIRE(root->virtual_state->saved_state != nullptr);
  REQUIRE(root->virtual_state->saved_state->keyed.contains(std::int64_t{0}));

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      -1000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 0.0F);
  REQUIRE(root->virtual_state->child_indices.front() == 0);
  REQUIRE((!root->virtual_state->saved_state || !root->virtual_state->saved_state->keyed.contains(std::int64_t{0})));
  REQUIRE(root->children[0]->children[0]->text == "0:4");
}

TEST_CASE("TestVirtualListStateSurvivesKeyRemovalAndReinsertion") {
  TestPlatform platform;
  Runtime runtime{ReorderableStatefulVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  auto *first_button = root->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->virtual_state->saved_state->keyed.contains(std::int64_t{0}));

  virtual_reorder_items.Update([](auto &items) { items.erase(items.begin()); });
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->virtual_state->saved_state->keyed.contains(std::int64_t{0}));

  virtual_reorder_items.Update([](auto &items) { items.push_back(0); });
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      5000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  const auto found = std::find_if(root->children.begin(), root->children.end(), [](const auto &child) {
    return child->key == huxerui::detail::ViewKey{std::int64_t{0}};
  });
  REQUIRE(found != root->children.end());
  REQUIRE((*found)->children[0]->text == "0:4");
}

TEST_CASE("TestVirtualListPrunesOutOfRangeIndexState") {
  TestPlatform platform;
  Runtime runtime{UnkeyedStatefulVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      5000.0F,
  });
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  const auto position =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{99});
  REQUIRE(position != root->virtual_state->child_indices.end());
  const std::size_t child_index = static_cast<std::size_t>(position - root->virtual_state->child_indices.begin());
  InvokeClick(*root->children[child_index]->children[0]);
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      -5000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->virtual_state->saved_state != nullptr);
  REQUIRE(root->virtual_state->saved_state->indexed.contains(std::size_t{99}));

  virtual_unkeyed_items.Update([](auto &items) { items.resize(10); });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE((!root->virtual_state->saved_state || !root->virtual_state->saved_state->indexed.contains(std::size_t{99})));
}

TEST_CASE("TestVariableVirtualListMeasurementAndAnchor") {
  TestPlatform platform;
  Runtime runtime{VariableVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->measured_size.height == 20.0F);
  REQUIRE(root->children[1]->measured_size.height == 40.0F);
  REQUIRE(root->children[2]->frame.y == 60.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      70.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 70.0F);
  const auto item_two =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{2});
  REQUIRE(item_two != root->virtual_state->child_indices.end());
  std::size_t child_index = static_cast<std::size_t>(item_two - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[child_index]->frame.y == -10.0F);

  variable_height_expanded = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 90.0F);
  const auto anchored_item =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{2});
  REQUIRE(anchored_item != root->virtual_state->child_indices.end());
  child_index = static_cast<std::size_t>(anchored_item - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[child_index]->frame.y == -10.0F);
}

TEST_CASE("TestVariableVirtualListRefinesEstimatedExtent") {
  TestPlatform platform;
  Runtime runtime{TinyVariableVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->scroll_content_height == 1000.0F);
  REQUIRE(root->virtual_state->child_indices.back() >= 99);
}

TEST_CASE("TestFixedHorizontalVirtualListLayoutAndScrolling") {
  TestPlatform platform;
  Runtime runtime{FixedHorizontalVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  REQUIRE(root->virtual_state->axis == Axis::Horizontal);
  REQUIRE(root->scroll_content_width == 20000.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      1000.0F,
      0.0F,
  });
  const DisplayList &scrolled = runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_x == 1000.0F);
  REQUIRE(root->virtual_state->child_indices.front() == 40);
  REQUIRE(root->virtual_state->child_indices.back() == 65);
  REQUIRE(FirstText(scrolled) == "50");
  const std::size_t visible_position = 50 - root->virtual_state->child_indices.front();
  REQUIRE(root->children[visible_position]->frame.x == 0.0F);
}

TEST_CASE("TestVariableHorizontalVirtualListMeasurementAndScrolling") {
  TestPlatform platform;
  Runtime runtime{VariableHorizontalVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->measured_size.width == 20.0F);
  REQUIRE(root->children[1]->measured_size.width == 40.0F);
  REQUIRE(root->children[2]->frame.x == 60.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      70.0F,
      0.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_x == 70.0F);
  const auto item_two =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{2});
  REQUIRE(item_two != root->virtual_state->child_indices.end());
  const std::size_t child_index = static_cast<std::size_t>(item_two - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[child_index]->frame.x == -10.0F);
}

TEST_CASE("TestHorizontalVirtualListStateSurvivesCacheEviction") {
  TestPlatform platform;
  Runtime runtime{StatefulHorizontalVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  auto *first_button = root->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      2000.0F,
      0.0F,
  });
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->virtual_state->child_indices.front() > 0);
  REQUIRE(root->virtual_state->saved_state != nullptr);
  REQUIRE(root->virtual_state->saved_state->keyed.contains(std::int64_t{0}));

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      -2000.0F,
      0.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_x == 0.0F);
  REQUIRE(root->children[0]->children[0]->text == "0:4");
}

TEST_CASE("TestCustomVirtualLayoutProtocol") {
  TestPlatform platform;
  Runtime runtime{CustomVirtualLayoutApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  const DisplayList &initial = runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  REQUIRE(root->virtual_layout->type == typeid(TestVirtualStrip));
  REQUIRE(root->children.size() == 5);
  REQUIRE(root->virtual_state->child_indices.front() == 0);
  REQUIRE(FirstText(initial) == "0");
  REQUIRE(huxerui::detail::ResolveScrollBarGeometry(*root).has_value());

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      250.0F,
  });
  const DisplayList &scrolled = runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 250.0F);
  REQUIRE(root->virtual_state->child_indices.front() == 10);
  REQUIRE(root->children.size() == 5);
  REQUIRE(FirstText(scrolled) == "10");

  REQUIRE(custom_virtual_scroll.ScrollToItem(std::size_t{20}, ScrollAlignment::Center));
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 462.5F);
  const auto centered =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{20});
  REQUIRE(centered != root->virtual_state->child_indices.end());
  const std::size_t centered_position = static_cast<std::size_t>(centered - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[centered_position]->frame.y == 37.5F);
}

TEST_CASE("TestCustomVirtualGridProtocol") {
  virtual_grid_factory_calls = 0;

  TestPlatform platform;
  Runtime runtime{CustomVirtualGridApp, platform};
  runtime.SetViewport({90.0F, 40.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  REQUIRE(root->virtual_layout->type == typeid(TestVirtualGrid));
  REQUIRE(root->children.size() < root->virtual_state->source.size);

  const auto first =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{0});
  const auto second =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{1});
  REQUIRE(first != root->virtual_state->child_indices.end());
  REQUIRE(second != root->virtual_state->child_indices.end());
  const std::size_t first_position = static_cast<std::size_t>(first - root->virtual_state->child_indices.begin());
  const std::size_t second_position = static_cast<std::size_t>(second - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[first_position]->frame.width == 60.0F);
  REQUIRE(root->children[second_position]->frame.x == 60.0F);
  REQUIRE(root->children[second_position]->frame.width == 30.0F);

  auto *first_button = root->children[first_position]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {45.0F, 20.0F},
      0.0F,
      200.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->virtual_state->saved_state != nullptr);
  REQUIRE(root->virtual_state->saved_state->keyed.contains(std::uint64_t{0}));

  std::size_t anchor_index = root->virtual_state->source.size;
  for (std::size_t position = 0; position < root->children.size(); ++position) {
    if (root->children[position]->frame.y == 0.0F) {
      anchor_index = root->virtual_state->child_indices[position];
      break;
    }
  }
  REQUIRE(anchor_index < root->virtual_state->source.size);
  const std::uint64_t identity = root->identity;

  runtime.SetViewport({60.0F, 40.0F});
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->identity == identity);
  const auto resized_anchor =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), anchor_index);
  REQUIRE(resized_anchor != root->virtual_state->child_indices.end());
  const std::size_t resized_position =
      static_cast<std::size_t>(resized_anchor - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[resized_position]->frame.y == 0.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {30.0F, 20.0F},
      0.0F,
      -10000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  const auto restored =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{0});
  REQUIRE(restored != root->virtual_state->child_indices.end());
  const std::size_t restored_position = static_cast<std::size_t>(restored - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[restored_position]->frame.width == 60.0F);
  REQUIRE(root->children[restored_position]->children[0]->text == "0:4");
  REQUIRE(virtual_grid_factory_calls < 200);
}

TEST_CASE("TestBuiltInVirtualGridLayoutStateAndResizeAnchor") {
  built_in_grid_factory_calls = 0;

  TestPlatform platform;
  Runtime runtime{BuiltInVirtualGridApp, platform};
  runtime.SetViewport({100.0F, 48.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  REQUIRE(root->virtual_layout->type == typeid(VirtualGrid));
  REQUIRE(root->children.size() < root->virtual_state->source.size);

  const auto first =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{0});
  const auto second =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{1});
  REQUIRE(first != root->virtual_state->child_indices.end());
  REQUIRE(second != root->virtual_state->child_indices.end());
  const std::size_t first_position = static_cast<std::size_t>(first - root->virtual_state->child_indices.begin());
  const std::size_t second_position = static_cast<std::size_t>(second - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[first_position]->frame.width == 65.0F);
  REQUIRE(root->children[second_position]->frame.x == 70.0F);
  REQUIRE(root->children[second_position]->frame.width == 30.0F);

  auto *first_button = root->children[first_position]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 24.0F},
      0.0F,
      240.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 240.0F);
  REQUIRE(root->virtual_state->saved_state != nullptr);
  REQUIRE(root->virtual_state->saved_state->keyed.contains(std::uint64_t{0}));

  std::size_t anchor_index = root->virtual_state->source.size;
  for (std::size_t position = 0; position < root->children.size(); ++position) {
    if (root->children[position]->frame.y == 0.0F) {
      anchor_index = root->virtual_state->child_indices[position];
      break;
    }
  }
  REQUIRE(anchor_index < root->virtual_state->source.size);
  const std::uint64_t identity = root->identity;

  runtime.SetViewport({65.0F, 48.0F});
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->identity == identity);
  const auto resized_anchor =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), anchor_index);
  REQUIRE(resized_anchor != root->virtual_state->child_indices.end());
  const std::size_t resized_position =
      static_cast<std::size_t>(resized_anchor - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[resized_position]->frame.y == 0.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {32.0F, 24.0F},
      0.0F,
      -10000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  const auto restored =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{0});
  REQUIRE(restored != root->virtual_state->child_indices.end());
  const std::size_t restored_position = static_cast<std::size_t>(restored - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[restored_position]->frame.width == 65.0F);
  REQUIRE(root->children[restored_position]->children[0]->text == "0:4");
  REQUIRE(built_in_grid_factory_calls < 200);

  bool invalid_columns_rejected = false;
  try {
    static_cast<void>(GridColumns::Fixed(0));
  } catch (const std::invalid_argument &) {
    invalid_columns_rejected = true;
  }
  REQUIRE(invalid_columns_rejected);
}

TEST_CASE("TestVariableVirtualGridMeasurementAndAnchor") {
  TestPlatform platform;
  Runtime runtime{VariableVirtualGridApp, platform};
  runtime.SetViewport({100.0F, 60.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto item_two =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{2});
  REQUIRE(item_two != root->virtual_state->child_indices.end());
  std::size_t item_two_position = static_cast<std::size_t>(item_two - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[item_two_position]->frame.y == 45.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 30.0F},
      0.0F,
      80.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 80.0F);
  const auto item_four =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{4});
  REQUIRE(item_four != root->virtual_state->child_indices.end());
  std::size_t item_four_position = static_cast<std::size_t>(item_four - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[item_four_position]->frame.y == 0.0F);

  variable_grid_height_expanded = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->scroll_offset_y == 100.0F);
  const auto anchored_item =
      std::find(root->virtual_state->child_indices.begin(), root->virtual_state->child_indices.end(), std::size_t{4});
  REQUIRE(anchored_item != root->virtual_state->child_indices.end());
  item_four_position = static_cast<std::size_t>(anchored_item - root->virtual_state->child_indices.begin());
  REQUIRE(root->children[item_four_position]->frame.y == 0.0F);
}

TEST_CASE("TestVirtualListAxisChangePreservesAnchorAndIdentity") {
  TestPlatform platform;
  Runtime runtime{AdaptiveAxisVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  const std::uint64_t identity = root->identity;
  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  REQUIRE(FirstText(runtime.BuildFrame()) == "50");

  horizontal_virtual_list = true;
  const DisplayList &horizontal = runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->identity == identity);
  REQUIRE(root->virtual_state->axis == Axis::Horizontal);
  REQUIRE(root->scroll_offset_x == 1000.0F);
  REQUIRE(root->virtual_state->child_indices.front() == 40);
  REQUIRE(FirstText(horizontal) == "50");
  const std::size_t visible_position = 50 - root->virtual_state->child_indices.front();
  REQUIRE(root->children[visible_position]->frame.x == 0.0F);
}

} // namespace huxerui::test
