#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

using namespace huxerui;

namespace {

constexpr std::size_t row_count = 10000;
constexpr std::size_t column_count = 100;
constexpr Size normal_cell{128.0F, 32.0F};
constexpr Size compact_cell{88.0F, 24.0F};

struct TableGeometry {
  using Value = TableGeometry;

  Size cell;
  ScrollController horizontal;
  ScrollController vertical;

  bool operator==(const TableGeometry&) const = default;
};

std::pair<std::size_t, std::size_t> VisibleRange(float offset, float viewport, float extent, std::size_t count) {
  const float limit = static_cast<float>(count);
  return {
      static_cast<std::size_t>(std::clamp(std::floor(offset / extent) - 1.0F, 0.0F, limit)),
      static_cast<std::size_t>(std::clamp(std::ceil((offset + viewport) / extent) + 1.0F, 0.0F, limit)),
  };
}

class ReadOnlyTable final : public VirtualLayout<ReadOnlyTable> {
public:
  ReadOnlyTable(TableGeometry geometry, Color text, Color even, Color odd)
      : VirtualLayout(row_count * column_count, [=](std::size_t index) {
          const std::size_t row = index / column_count;
          const std::size_t column = index % column_count;
          return Text::Format("R{} · C{}", row + 1, column + 1)
              .With(FontSize(12.0F), Foreground(text), Padding(EdgeInsets{.right = 6.0F, .left = 8.0F}),
                    Background((row + column) % 2 == 0 ? even : odd), Semantics{.role = SemanticRole::GridCell})
              .Key(index);
        }) {
    EnableBothAxes();
    SetLayoutValue(typeid(TableGeometry), std::move(geometry));
  }

  static std::optional<Rect> ItemBounds(ViewNode& node, std::size_t index) {
    const Size cell = node.LayoutValue<TableGeometry>()->cell;
    return Rect{static_cast<float>(index % column_count) * cell.width,
                static_cast<float>(index / column_count) * cell.height, cell.width, cell.height};
  }

  static VirtualLayoutResult Measure(VirtualLayoutContext& context, ViewNode& node, Constraints) {
    const VirtualViewport viewport = context.Viewport();
    const auto& geometry = *node.LayoutValue<TableGeometry>();
    const Size cell = geometry.cell;
    const Size content{column_count * cell.width, row_count * cell.height};
    VirtualLayoutResult result;
    result.SetSize(viewport.size)
        .SetContentSize(content)
        .SetCollectionSemantics(SemanticRole::Grid, SemanticRole::GridCell,
                                {.item_count = context.ItemCount(), .row_count = row_count,
                                 .column_count = column_count});

    // Committed extents and offsets stay stable across tentative measurements with different constraints.
    const auto x = geometry.horizontal.Metrics();
    const auto y = geometry.vertical.Metrics();
    if (x.content_extent > 0.0F && x.content_extent != content.width) {
      result.SetScrollOffset(Axis::Horizontal, x.offset / x.content_extent * content.width);
    }
    if (y.content_extent > 0.0F && y.content_extent != content.height) {
      result.SetScrollOffset(Axis::Vertical, y.offset / y.content_extent * content.height);
    }

    const auto [first_row, last_row] = VisibleRange(viewport.offset.y, viewport.size.height, cell.height, row_count);
    const auto [first_column, last_column] =
        VisibleRange(viewport.offset.x, viewport.size.width, cell.width, column_count);
    for (std::size_t row = first_row; row < last_row; ++row) {
      for (std::size_t column = first_column; column < last_column; ++column) {
        const std::size_t index = row * column_count + column;
        auto& item = context.Item(index);
        context.Measure(item, {cell.width, cell.width, cell.height, cell.height});
        result.Place(item, {column * cell.width, row * cell.height},
                     {.index = index, .row_index = row, .column_index = column});
      }
    }
    return result;
  }
};

[[huxerui::composable]]
View TablePosition(ScrollController horizontal, ScrollController vertical) {
  const auto x = horizontal.Metrics();
  const auto y = vertical.Metrics();
  return Text::Format("X {} / {}    Y {} / {}", std::lround(x.offset), std::lround(x.maximum_offset),
                      std::lround(y.offset), std::lround(y.maximum_offset))
      .With(FontSize(12.0F));
}

View App() {
  auto horizontal = UseScrollController();
  auto vertical = UseScrollController();
  auto compact = UseState(false);
  const auto& colors = UseTheme().colors;
  return Column {
    Text("Virtual table", TextRole::Title),
    Text("10,000 rows · 100 columns").With(FontSize(13.0F)),
    Row {
      Button("Start").OnClick([=] {
        horizontal.ScrollTo(0.0F);
        vertical.ScrollTo(0.0F);
      }),
      Button("Far corner").OnClick([=] {
        horizontal.ScrollToItem(row_count * column_count - 1, ScrollAlignment::End);
        vertical.ScrollToItem(row_count * column_count - 1, ScrollAlignment::End);
      }),
      Button(compact.Get() ? "Roomy" : "Compact").OnClick([=] { compact = !compact.Get(); }),
    }.With(Spacing(8.0F)),
    TablePosition(horizontal, vertical),
    ReadOnlyTable({compact.Get() ? compact_cell : normal_cell, horizontal, vertical}, colors.on_surface, colors.surface,
                  colors.surface_container_highest)
        .Controller(Axis::Horizontal, horizontal)
        .Controller(Axis::Vertical, vertical)
        .With(Grow(1.0F), ScrollBar(), Border{.color = colors.outline}, CornerRadius(8.0F)),
  }.With(Padding(12.0F), Spacing(8.0F));
}

} // namespace

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Virtual Table",
            .initial_size = {800.0F, 560.0F},
        },
    }
};
