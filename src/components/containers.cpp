#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <huxerui/scroll.h>
#include <huxerui/semantics.h>

#include "runtime/mounted_node_internal.h"

namespace huxerui {

namespace {

using detail::MakeContainerSpec;

class VirtualListMetrics {
public:
  bool Prepare(std::size_t item_count, Axis axis, float spacing, std::optional<float> fixed_extent,
               float estimated_extent, bool reset_measurements) {
    const bool axis_changed = initialized_ && axis_ != axis;
    const bool geometry_changed = !initialized_ || item_count_ != item_count || axis_changed || spacing_ != spacing ||
                                  fixed_extent_ != fixed_extent || configured_estimate_ != estimated_extent ||
                                  reset_measurements;
    if (!geometry_changed) {
      return false;
    }

    item_count_ = item_count;
    axis_ = axis;
    spacing_ = spacing;
    fixed_extent_ = fixed_extent;
    configured_estimate_ = estimated_extent;
    estimate_ = estimated_extent;
    if (fixed_extent_.has_value()) {
      measured_.clear();
      measured_sum_tree_.clear();
      measured_count_tree_.clear();
    } else {
      measured_.assign(item_count_, std::numeric_limits<float>::quiet_NaN());
      measured_sum_tree_.assign(item_count_ + 1, 0.0F);
      measured_count_tree_.assign(item_count_ + 1, 0.0F);
    }
    measured_total_ = 0.0F;
    measured_count_ = 0;
    if (axis_changed || fixed_extent_.has_value()) {
      keyed_extents_.clear();
    }
    initialized_ = true;
    return true;
  }

  [[nodiscard]] bool Initialized() const noexcept {
    return initialized_;
  }

  [[nodiscard]] Axis CurrentAxis() const noexcept {
    return axis_;
  }

  [[nodiscard]] std::size_t ItemCount() const noexcept {
    return item_count_;
  }

  [[nodiscard]] float Estimate() const noexcept {
    return estimate_;
  }

  [[nodiscard]] float Offset(std::size_t index) const {
    index = std::min(index, item_count_);
    if (fixed_extent_.has_value()) {
      return static_cast<float>(index) * (*fixed_extent_ + spacing_);
    }
    const float measured_sum = Prefix(measured_sum_tree_, index);
    const float measured_count = Prefix(measured_count_tree_, index);
    return measured_sum + (static_cast<float>(index) - measured_count) * estimate_ +
           static_cast<float>(index) * spacing_;
  }

  [[nodiscard]] float ContentExtent() const {
    if (item_count_ == 0) {
      return 0.0F;
    }
    return std::max(0.0F, Offset(item_count_) - spacing_);
  }

  [[nodiscard]] float Extent(std::size_t index) const {
    if (index >= item_count_) {
      return 0.0F;
    }
    return std::max(0.0F, Offset(index + 1) - Offset(index) - spacing_);
  }

  [[nodiscard]] std::size_t IndexAt(float offset) const {
    if (item_count_ == 0) {
      return 0;
    }
    if (fixed_extent_.has_value()) {
      const float stride = *fixed_extent_ + spacing_;
      return std::min(
          item_count_ - 1,
          static_cast<std::size_t>(std::floor(std::max(0.0F, offset) / std::max(stride, 0.0001F)))
      );
    }

    std::size_t position = 0;
    float accumulated = 0.0F;
    std::size_t step = 1;
    while (step < item_count_) {
      step <<= 1;
    }
    for (; step > 0; step >>= 1) {
      const std::size_t next = position + step;
      if (next > item_count_) {
        continue;
      }
      const float known_sum = measured_sum_tree_[next];
      const float known_count = measured_count_tree_[next];
      const float block =
          known_sum + (static_cast<float>(step) - known_count) * estimate_ + static_cast<float>(step) * spacing_;
      if (accumulated + block <= std::max(0.0F, offset)) {
        position = next;
        accumulated += block;
      }
    }
    return std::min(position, item_count_ - 1);
  }

  void RestoreKey(std::size_t index, const std::optional<detail::ViewKey>& key) {
    if (fixed_extent_.has_value() || !key.has_value()) {
      return;
    }
    if (const auto found = keyed_extents_.find(*key); found != keyed_extents_.end()) {
      Update(index, found->second, key);
    }
  }

  void Update(std::size_t index, float extent, const std::optional<detail::ViewKey>& key) {
    if (fixed_extent_.has_value() || index >= item_count_ || !std::isfinite(extent) || extent <= 0.0F) {
      return;
    }
    if (key.has_value()) {
      keyed_extents_.insert_or_assign(*key, extent);
    }

    const float previous = measured_[index];
    if (std::isfinite(previous)) {
      const float delta = extent - previous;
      measured_[index] = extent;
      measured_total_ += delta;
      Add(measured_sum_tree_, index, delta);
    } else {
      measured_[index] = extent;
      measured_total_ += extent;
      ++measured_count_;
      Add(measured_sum_tree_, index, extent);
      Add(measured_count_tree_, index, 1.0F);
    }
    if (measured_count_ > 0) {
      estimate_ = measured_total_ / static_cast<float>(measured_count_);
    }
  }

private:
  static void Add(std::vector<float>& tree, std::size_t index, float delta) {
    for (++index; index < tree.size(); index += index & (~index + 1)) {
      tree[index] += delta;
    }
  }

  static float Prefix(const std::vector<float>& tree, std::size_t count) {
    float result = 0.0F;
    for (; count > 0; count &= count - 1) {
      result += tree[count];
    }
    return result;
  }

  bool initialized_ = false;
  std::size_t item_count_ = 0;
  Axis axis_ = Axis::Vertical;
  float spacing_ = 0.0F;
  std::optional<float> fixed_extent_;
  float configured_estimate_ = 56.0F;
  float estimate_ = 56.0F;
  std::vector<float> measured_;
  std::vector<float> measured_sum_tree_;
  std::vector<float> measured_count_tree_;
  float measured_total_ = 0.0F;
  std::size_t measured_count_ = 0;
  std::unordered_map<detail::ViewKey, float> keyed_extents_;
};

struct VirtualGridCell {
  std::size_t row;
  std::size_t column;
  std::size_t span;
};

class VirtualGridMetrics {
public:
  [[nodiscard]] bool Initialized() const noexcept {
    return initialized_;
  }

  [[nodiscard]] std::size_t ItemCount() const noexcept {
    return item_count_;
  }

  [[nodiscard]] std::size_t RowCount() const noexcept {
    return row_count_;
  }

  [[nodiscard]] std::size_t ColumnCount() const noexcept {
    return columns_;
  }

  [[nodiscard]] const VirtualGridCell& Cell(std::size_t index) const {
    return cells_[index];
  }

  [[nodiscard]] std::size_t FirstItem(std::size_t row) const {
    return row < row_starts_.size() ? row_starts_[row] : cells_.size();
  }

  [[nodiscard]] std::size_t EndItem(std::size_t row) const {
    return row + 1 < row_starts_.size() ? row_starts_[row + 1] : cells_.size();
  }

  [[nodiscard]] std::size_t RowForItem(std::size_t index) const {
    return index < cells_.size() ? cells_[index].row : row_count_;
  }

  [[nodiscard]] std::size_t RowAt(float offset) const {
    return rows_.IndexAt(offset);
  }

  [[nodiscard]] float Offset(std::size_t row) const {
    return rows_.Offset(row);
  }

  [[nodiscard]] float ContentExtent() const {
    return rows_.ContentExtent();
  }

  [[nodiscard]] float RowExtent(std::size_t row) const {
    return rows_.Extent(row);
  }

  void UpdateRow(std::size_t row, float extent) {
    rows_.Update(row, extent, std::nullopt);
  }

  bool Prepare(std::size_t item_count, std::size_t columns, float track_width, float row_spacing,
               std::optional<float> fixed_row_extent, float estimated_row_extent,
               const std::vector<std::size_t>& spans) {
    const bool plan_changed = !initialized_ || item_count_ != item_count || columns_ != columns || spans_ != spans;
    if (plan_changed) {
      BuildPlan(item_count, columns, spans);
    }

    const bool geometry_changed = plan_changed || track_width_ != track_width || row_spacing_ != row_spacing ||
                                   fixed_row_extent_ != fixed_row_extent ||
                                   estimated_row_extent_ != estimated_row_extent;
    rows_.Prepare(row_count_, Axis::Vertical, row_spacing, fixed_row_extent, estimated_row_extent, geometry_changed);
    track_width_ = track_width;
    row_spacing_ = row_spacing;
    fixed_row_extent_ = fixed_row_extent;
    estimated_row_extent_ = estimated_row_extent;
    initialized_ = true;
    return geometry_changed;
  }

private:
  void BuildPlan(std::size_t item_count, std::size_t columns, const std::vector<std::size_t>& spans) {
    item_count_ = item_count;
    columns_ = columns;
    spans_ = spans;
    cells_.clear();
    cells_.reserve(item_count);
    row_starts_.clear();

    std::size_t row = 0;
    std::size_t column = 0;
    for (std::size_t index = 0; index < item_count; ++index) {
      const std::size_t requested_span = index < spans.size() ? spans[index] : std::size_t{1};
      const std::size_t span = std::clamp(requested_span, std::size_t{1}, columns);
      if (column > 0 && column + span > columns) {
        ++row;
        column = 0;
      }
      if (column == 0) {
        row_starts_.push_back(index);
      }
      cells_.push_back({row, column, span});
      column += span;
      if (column == columns) {
        ++row;
        column = 0;
      }
    }
    row_count_ = row + (column > 0 ? 1 : 0);
  }

  bool initialized_ = false;
  std::size_t item_count_ = 0;
  std::size_t columns_ = 0;
  std::size_t row_count_ = 0;
  float track_width_ = 0.0F;
  float row_spacing_ = 0.0F;
  std::optional<float> fixed_row_extent_;
  float estimated_row_extent_ = 56.0F;
  std::vector<std::size_t> spans_;
  std::vector<VirtualGridCell> cells_;
  std::vector<std::size_t> row_starts_;
  VirtualListMetrics rows_;
};

float LayoutMainSize(Size size, bool vertical) {
  return vertical ? size.height : size.width;
}

float LayoutCrossSize(Size size, bool vertical) {
  return vertical ? size.width : size.height;
}

Size MakeAxisSize(float main, float cross, bool vertical) {
  return vertical ? Size{cross, main} : Size{main, cross};
}

Constraints TightMain(Constraints constraints, bool vertical, float value) {
  return vertical ? constraints.TightHeight(value) : constraints.TightWidth(value);
}

Constraints TightCross(Constraints constraints, bool vertical, float value) {
  return vertical ? constraints.TightWidth(value) : constraints.TightHeight(value);
}

float MinimumMain(const Constraints& constraints, bool vertical) {
  return vertical ? constraints.min_height : constraints.min_width;
}

float MaximumMain(const Constraints& constraints, bool vertical) {
  return vertical ? constraints.max_height : constraints.max_width;
}

float MinimumCross(const Constraints& constraints, bool vertical) {
  return vertical ? constraints.min_width : constraints.min_height;
}

float MaximumCross(const Constraints& constraints, bool vertical) {
  return vertical ? constraints.max_width : constraints.max_height;
}

float TotalSpacing(const ViewNode& node) {
  return node.ChildCount() < 2 ? 0.0F : node.Spacing() * static_cast<float>(node.ChildCount() - 1);
}

float SumMainSizes(const ViewNode& node, bool vertical) {
  float result = 0.0F;
  for (std::size_t index = 0; index < node.ChildCount(); ++index) {
    result += LayoutMainSize(node.ChildAt(index).LayoutSize(), vertical);
  }
  return result;
}

float MaxCrossSize(const ViewNode& node, bool vertical) {
  float result = 0.0F;
  for (std::size_t index = 0; index < node.ChildCount(); ++index) {
    result = std::max(result, LayoutCrossSize(node.ChildAt(index).LayoutSize(), vertical));
  }
  return result;
}

float CrossOffset(float available, float child, CrossAxisAlignment alignment) {
  const float remaining = std::max(0.0F, available - child);
  switch (alignment) {
  case CrossAxisAlignment::Center:
    return remaining * 0.5F;
  case CrossAxisAlignment::End:
    return remaining;
  case CrossAxisAlignment::Start:
  case CrossAxisAlignment::Stretch:
    return 0.0F;
  }
  return 0.0F;
}

float AlignedScrollOffset(float start, float extent, float viewport_extent, ScrollAlignment alignment) {
  switch (alignment) {
  case ScrollAlignment::Center:
    return start - (viewport_extent - extent) * 0.5F;
  case ScrollAlignment::End:
    return start - (viewport_extent - extent);
  case ScrollAlignment::Start:
    return start;
  }
  return start;
}

float HorizontalOffset(float available, float child, HorizontalAlignment alignment) {
  const float remaining = std::max(0.0F, available - child);
  switch (alignment) {
  case HorizontalAlignment::Center:
    return remaining * 0.5F;
  case HorizontalAlignment::End:
    return remaining;
  case HorizontalAlignment::Start:
  case HorizontalAlignment::Stretch:
    return 0.0F;
  }
  return 0.0F;
}

float VerticalOffset(float available, float child, VerticalAlignment alignment) {
  const float remaining = std::max(0.0F, available - child);
  switch (alignment) {
  case VerticalAlignment::Center:
    return remaining * 0.5F;
  case VerticalAlignment::End:
    return remaining;
  case VerticalAlignment::Start:
  case VerticalAlignment::Stretch:
    return 0.0F;
  }
  return 0.0F;
}

struct AxisPlacement {
  float leading = 0.0F;
  float gap = 0.0F;
};

AxisPlacement ResolveAxisPlacement(MainAxisAlignment alignment, float spacing, std::size_t count, float used,
                                   float available) {
  AxisPlacement result{0.0F, spacing};
  if (count == 0) {
    return result;
  }

  const float remaining = std::max(0.0F, available - used);
  switch (alignment) {
  case MainAxisAlignment::Center:
    result.leading = remaining * 0.5F;
    break;
  case MainAxisAlignment::End:
    result.leading = remaining;
    break;
  case MainAxisAlignment::SpaceBetween:
    if (count > 1) {
      result.gap += remaining / static_cast<float>(count - 1);
    }
    break;
  case MainAxisAlignment::SpaceAround:
    result.gap += remaining / static_cast<float>(count);
    result.leading = remaining / (2.0F * static_cast<float>(count));
    break;
  case MainAxisAlignment::SpaceEvenly:
    result.gap += remaining / static_cast<float>(count + 1);
    result.leading = remaining / static_cast<float>(count + 1);
    break;
  case MainAxisAlignment::Start:
    break;
  }
  return result;
}

AxisPlacement ResolveAxisPlacement(const ViewNode& node, float available, bool vertical) {
  return ResolveAxisPlacement(node.MainAlignment(), node.Spacing(), node.ChildCount(),
                              SumMainSizes(node, vertical) + TotalSpacing(node), available);
}

struct FlowLine {
  std::vector<ViewNode*> children;
  float natural_width = 0.0F;
  float height = 0.0F;
  float total_grow = 0.0F;
};

AxisPlacement ResolveFlowLinePlacement(const ViewNode& node, const FlowLine& line, float available) {
  const std::size_t count = line.children.size();
  float used = count < 2 ? 0.0F : node.Spacing() * static_cast<float>(count - 1);
  for (const ViewNode* child : line.children) {
    used += child->LayoutSize().width;
  }
  return ResolveAxisPlacement(node.MainAlignment(), node.Spacing(), count, used, available);
}

std::vector<FlowLine> BuildFlowLines(LayoutContext& context, ViewNode& node, const Constraints& loose,
                                     float maximum_width) {
  std::vector<FlowLine> lines;
  FlowLine current;
  const bool bounded = std::isfinite(maximum_width);
  for (ViewNode& child : node.Children()) {
    const Size size = context.Measure(child, loose);
    const float candidate = current.children.empty() ? size.width : current.natural_width + node.Spacing() + size.width;
    if (bounded && !current.children.empty() && candidate > maximum_width) {
      lines.push_back(std::move(current));
      current = {};
    }
    if (!current.children.empty()) {
      current.natural_width += node.Spacing();
    }
    current.children.push_back(&child);
    current.natural_width += size.width;
    current.height = std::max(current.height, size.height);
    current.total_grow += child.GrowFactor();
  }
  if (!current.children.empty()) {
    lines.push_back(std::move(current));
  }
  return lines;
}

float ResolveFlowWidth(const ViewNode& node, const std::vector<FlowLine>& lines, const Constraints& constraints) {
  float natural_width = 0.0F;
  bool has_grow = false;
  for (const FlowLine& line : lines) {
    natural_width = std::max(natural_width, line.natural_width);
    has_grow = has_grow || line.total_grow > 0.0F;
  }
  if (std::isfinite(constraints.max_width) && (has_grow || node.MainAlignment() != MainAxisAlignment::Start)) {
    return constraints.max_width;
  }
  return constraints.ConstrainWidth(natural_width);
}

void MeasureFlowLine(LayoutContext& context, const ViewNode& node, FlowLine& line, const Constraints& loose,
                     float width) {
  const float spacing = node.Spacing() * static_cast<float>(line.children.size() - 1);
  float fixed_width = 0.0F;
  for (const ViewNode* child : line.children) {
    if (child->GrowFactor() <= 0.0F) {
      fixed_width += child->LayoutSize().width;
    }
  }

  if (line.total_grow > 0.0F && std::isfinite(width)) {
    const float remaining = std::max(0.0F, width - fixed_width - spacing);
    for (ViewNode* child : line.children) {
      if (child->GrowFactor() <= 0.0F) {
        continue;
      }
      const float share = remaining * child->GrowFactor() / line.total_grow;
      static_cast<void>(context.Measure(*child, loose.TightWidth(share)));
    }
  }

  line.height = 0.0F;
  for (const ViewNode* child : line.children) {
    line.height = std::max(line.height, child->LayoutSize().height);
  }
  if (node.CrossAlignment() != CrossAxisAlignment::Stretch) {
    return;
  }
  for (ViewNode* child : line.children) {
    Constraints child_constraints = loose.TightHeight(line.height);
    if (child->GrowFactor() > 0.0F && line.total_grow > 0.0F && std::isfinite(width)) {
      child_constraints = child_constraints.TightWidth(child->LayoutSize().width);
    }
    static_cast<void>(context.Measure(*child, child_constraints));
  }
}

LayoutResult MeasureAxisLayout(LayoutContext& context, ViewNode& node, Constraints constraints, bool vertical) {
  const Constraints loose = constraints.Loose();
  const bool stretch = node.CrossAlignment() == CrossAxisAlignment::Stretch;
  const float minimum_cross = MinimumCross(constraints, vertical);
  const float maximum_cross = MaximumCross(constraints, vertical);
  const bool tight_cross = stretch && std::isfinite(maximum_cross) && minimum_cross == maximum_cross;
  const Constraints initial = tight_cross ? TightCross(loose, vertical, maximum_cross) : loose;
  float total_grow = 0.0F;

  for (ViewNode& child : node.Children()) {
    // A tight cross axis already determines the stretch result. Measuring loose first would recursively double the
    // work of every nested stretching linear layout without contributing another layout decision.
    static_cast<void>(context.Measure(child, initial));
    total_grow += child.GrowFactor();
  }

  float target_cross = std::clamp(MaxCrossSize(node, vertical), minimum_cross, maximum_cross);

  if (stretch && !tight_cross) {
    for (ViewNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, TightCross(loose, vertical, target_cross)));
    }
  }

  const float spacing = TotalSpacing(node);
  float fixed_main = 0.0F;
  for (ViewNode& child : node.Children()) {
    if (child.GrowFactor() <= 0.0F) {
      fixed_main += LayoutMainSize(child.LayoutSize(), vertical);
    }
  }

  const float max_main = MaximumMain(constraints, vertical);
  float target_main = fixed_main + spacing;
  if (total_grow > 0.0F && std::isfinite(max_main)) {
    target_main = max_main;
    const float remaining = std::max(0.0F, target_main - fixed_main - spacing);
    for (ViewNode& child : node.Children()) {
      if (child.GrowFactor() <= 0.0F) {
        continue;
      }
      const float share = remaining * child.GrowFactor() / total_grow;
      Constraints child_constraints = TightMain(loose, vertical, share);
      if (stretch) {
        child_constraints = TightCross(child_constraints, vertical, target_cross);
      }
      static_cast<void>(context.Measure(child, child_constraints));
    }
  } else {
    target_main = SumMainSizes(node, vertical) + spacing;
    if (node.MainAlignment() != MainAxisAlignment::Start && std::isfinite(max_main)) {
      target_main = max_main;
    }
  }

  target_main = std::clamp(target_main, MinimumMain(constraints, vertical), max_main);
  if (!stretch) {
    target_cross = std::clamp(MaxCrossSize(node, vertical), minimum_cross, maximum_cross);
  }

  LayoutResult result;
  const AxisPlacement placement = ResolveAxisPlacement(node, target_main, vertical);
  float main = placement.leading;
  for (ViewNode& child : node.Children()) {
    const Size child_size = child.LayoutSize();
    const float cross = CrossOffset(target_cross, LayoutCrossSize(child_size, vertical), node.CrossAlignment());
    result.Place(child, vertical ? Point{cross, main} : Point{main, cross});
    main += LayoutMainSize(child_size, vertical) + placement.gap;
  }

  result.SetSize(MakeAxisSize(target_main, target_cross, vertical));
  return result;
}

} // namespace

VirtualLayoutResult VirtualList::Measure(VirtualLayoutContext& context, ViewNode& node, Constraints constraints) {
  const Axis axis = node.LayoutValueOr<detail::ScrollAxisBinding>(Axis::Vertical);
  const bool vertical = axis == Axis::Vertical;
  if ((vertical && !constraints.HasBoundedHeight()) || (!vertical && !constraints.HasBoundedWidth())) {
    throw std::logic_error("HuxerUI VirtualList requires bounded constraints on its scroll axis");
  }

  std::optional<float> fixed_extent;
  if (const float* value = node.LayoutValue<detail::VirtualListItemExtent>()) {
    fixed_extent = *value;
  }
  const float configured_estimate = node.LayoutValueOr<detail::VirtualListEstimatedItemExtent>(56.0F);
  const VirtualViewport viewport = context.Viewport();
  const float viewport_extent = vertical ? viewport.size.height : viewport.size.width;
  const float configured_cache =
      node.LayoutValueOr<detail::VirtualListCacheExtent>(std::max(200.0F, viewport_extent * 0.5F));
  const std::size_t item_count = context.ItemCount();

  auto& metrics = node.Cache<VirtualListMetrics>();
  const bool had_metrics = metrics.Initialized();
  const bool axis_changed = metrics.Initialized() && metrics.CurrentAxis() != axis;
  const float previous_scroll =
      metrics.Initialized() ? (metrics.CurrentAxis() == Axis::Vertical ? viewport.offset.y : viewport.offset.x) : 0.0F;
  const std::size_t previous_anchor = metrics.Initialized() ? metrics.IndexAt(previous_scroll) : 0;
  const float previous_anchor_delta = metrics.Initialized() ? previous_scroll - metrics.Offset(previous_anchor) : 0.0F;

  const bool geometry_changed =
      metrics.Prepare(item_count, axis, node.Spacing(), fixed_extent, configured_estimate, false);

  float scroll_offset = vertical ? viewport.offset.y : viewport.offset.x;
  std::size_t anchor = item_count == 0 ? 0 : std::min(previous_anchor, item_count - 1);
  float anchor_delta = previous_anchor_delta;
  if (axis_changed) {
    scroll_offset = metrics.Offset(anchor);
    anchor_delta = 0.0F;
  } else if (!geometry_changed || !had_metrics) {
    anchor = metrics.IndexAt(scroll_offset);
    anchor_delta = scroll_offset - metrics.Offset(anchor);
  }

  auto resolve_range = [&](float offset) {
    std::pair<std::size_t, std::size_t> range;
    if (item_count == 0) {
      return range;
    }
    const float content_extent = metrics.ContentExtent();
    const float maximum = std::max(0.0F, content_extent - viewport_extent);
    offset = std::clamp(offset, 0.0F, maximum);
    const float start = std::max(0.0F, offset - configured_cache);
    const float end = std::min(content_extent, offset + viewport_extent + configured_cache);
    range.first = metrics.IndexAt(start);
    range.second = std::min(item_count, metrics.IndexAt(end) + 1);
    range.second = std::max(range.second, range.first + 1);
    return range;
  };

  Constraints item_constraints =
      vertical
          ? Constraints{
                constraints.min_width,
                constraints.max_width,
                0.0F,
                std::numeric_limits<float>::infinity(),
            }
          : Constraints{
                0.0F,
                std::numeric_limits<float>::infinity(),
                constraints.min_height,
                constraints.max_height,
            };
  if (node.CrossAlignment() == CrossAxisAlignment::Stretch && std::isfinite(MaximumCross(constraints, vertical))) {
    item_constraints = TightCross(item_constraints, vertical, MaximumCross(constraints, vertical));
  }
  if (fixed_extent.has_value()) {
    item_constraints = TightMain(item_constraints, vertical, *fixed_extent);
  }

  std::pair<std::size_t, std::size_t> range = resolve_range(scroll_offset);
  for (int pass = 0; pass < 4 && range.second > range.first; ++pass) {
    for (std::size_t index = range.first; index < range.second; ++index) {
      ViewNode& item = context.Item(index);
      const auto& key = static_cast<detail::MountedNode&>(item).key;
      metrics.RestoreKey(index, key);
      const Size item_size = context.Measure(item, item_constraints);
      metrics.Update(index, LayoutMainSize(item_size, vertical), key);
    }

    scroll_offset = item_count == 0 ? 0.0F : metrics.Offset(std::min(anchor, item_count - 1)) + anchor_delta;
    const float maximum = std::max(0.0F, metrics.ContentExtent() - viewport_extent);
    scroll_offset = std::clamp(scroll_offset, 0.0F, maximum);
    const auto refined = resolve_range(scroll_offset);
    if (refined == range) {
      break;
    }
    range = refined;
  }

  float cross_extent = 0.0F;
  for (std::size_t index = range.first; index < range.second; ++index) {
    ViewNode& item = context.Item(index);
    const auto& key = static_cast<detail::MountedNode&>(item).key;
    metrics.RestoreKey(index, key);
    const Size item_size = context.Measure(item, item_constraints);
    metrics.Update(index, LayoutMainSize(item_size, vertical), key);
    cross_extent = std::max(cross_extent, LayoutCrossSize(item_size, vertical));
  }

  scroll_offset = item_count == 0 ? 0.0F : metrics.Offset(std::min(anchor, item_count - 1)) + anchor_delta;
  const float content_extent = metrics.ContentExtent();
  scroll_offset = std::clamp(scroll_offset, 0.0F, std::max(0.0F, content_extent - viewport_extent));
  const Size measured_size = constraints.Constrain(MakeAxisSize(content_extent, cross_extent, vertical));
  const float measured_cross = LayoutCrossSize(measured_size, vertical);

  VirtualLayoutResult result;
  result.SetAxis(axis)
      .SetSize(measured_size)
      .SetContentSize(MakeAxisSize(content_extent, measured_cross, vertical))
      .SetScrollOffset(scroll_offset)
      .SetCollectionSemantics(
          SemanticRole::List,
          SemanticRole::ListItem,
          SemanticCollection{
              .item_count = item_count,
              .row_count = vertical ? item_count : std::size_t{1},
              .column_count = vertical ? std::size_t{1} : item_count,
          }
      );

  for (std::size_t index = range.first; index < range.second; ++index) {
    ViewNode& item = context.Item(index);
    const Size item_size = item.LayoutSize();
    const float cross = CrossOffset(measured_cross, LayoutCrossSize(item_size, vertical), node.CrossAlignment());
    const float main = metrics.Offset(index);
    result.Place(
        item,
        vertical ? Point{cross, main} : Point{main, cross},
        SemanticCollectionItem{
            .index = index,
            .row_index = vertical ? index : std::size_t{0},
            .column_index = vertical ? std::size_t{0} : index,
        }
    );
  }
  return result;
}

std::optional<float> VirtualList::ScrollOffsetForItem(
    ViewNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent
) {
  auto& metrics = node.Cache<VirtualListMetrics>();
  if (!metrics.Initialized() || index >= metrics.ItemCount()) {
    return std::nullopt;
  }
  return AlignedScrollOffset(metrics.Offset(index), metrics.Extent(index), viewport_extent, alignment);
}

VirtualLayoutResult VirtualGrid::Measure(VirtualLayoutContext& context, ViewNode& node, Constraints constraints) {
  if (!constraints.HasBoundedWidth() || !constraints.HasBoundedHeight()) {
    throw std::logic_error("HuxerUI VirtualGrid requires bounded width and height");
  }

  const VirtualViewport viewport = context.Viewport();
  const GridColumns column_configuration =
      node.LayoutValueOr<detail::VirtualGridColumns>(GridColumns::Adaptive(160.0F));
  const float configured_column_spacing = node.LayoutValueOr<detail::VirtualGridColumnSpacing>(node.Spacing());
  const std::size_t columns = column_configuration.Resolve(viewport.size.width, configured_column_spacing);
  const float column_spacing =
      columns > 1 ? std::min(configured_column_spacing, viewport.size.width / static_cast<float>(columns - 1)) : 0.0F;
  const float track_width = std::max(0.0F, viewport.size.width - column_spacing * static_cast<float>(columns - 1)) /
                            static_cast<float>(columns);
  const float row_spacing = node.LayoutValueOr<detail::VirtualGridRowSpacing>(node.Spacing());
  std::optional<float> fixed_row_extent;
  if (const float* value = node.LayoutValue<detail::VirtualGridRowExtent>()) {
    fixed_row_extent = *value;
  }
  const float estimated_row_extent = node.LayoutValueOr<detail::VirtualGridEstimatedRowExtent>(56.0F);
  const float cache_extent =
      node.LayoutValueOr<detail::VirtualGridCacheExtent>(std::max(200.0F, viewport.size.height * 0.5F));
  const auto* configured_spans = node.LayoutValue<detail::VirtualGridItemSpans>();
  const std::vector<std::size_t> empty_spans;
  const auto& spans = configured_spans == nullptr ? empty_spans : *configured_spans;
  const std::size_t item_count = context.ItemCount();

  auto& metrics = node.Cache<VirtualGridMetrics>();
  const bool had_layout = metrics.Initialized();
  const std::size_t previous_row = had_layout && metrics.RowCount() > 0 ? metrics.RowAt(viewport.offset.y) : 0;
  const std::size_t previous_anchor = had_layout && metrics.RowCount() > 0 ? metrics.FirstItem(previous_row) : 0;
  const float previous_anchor_delta =
      had_layout && metrics.RowCount() > 0 ? viewport.offset.y - metrics.Offset(previous_row) : 0.0F;
  const bool geometry_changed =
      metrics.Prepare(item_count, columns, track_width, row_spacing, fixed_row_extent, estimated_row_extent, spans);

  float scroll_offset = viewport.offset.y;
  std::size_t anchor = 0;
  float anchor_delta = 0.0F;
  if (item_count > 0 && had_layout && geometry_changed) {
    anchor = std::min(previous_anchor, item_count - 1);
    anchor_delta = previous_anchor_delta;
    scroll_offset = metrics.Offset(metrics.RowForItem(anchor)) + anchor_delta;
  } else if (item_count > 0) {
    const std::size_t row = metrics.RowAt(scroll_offset);
    anchor = metrics.FirstItem(row);
    anchor_delta = scroll_offset - metrics.Offset(row);
  }

  auto clamp_scroll_offset = [&](float offset) {
    const float maximum = std::max(0.0F, metrics.ContentExtent() - viewport.size.height);
    return std::clamp(offset, 0.0F, maximum);
  };
  auto resolve_rows = [&](float offset) {
    std::pair<std::size_t, std::size_t> rows;
    if (metrics.RowCount() == 0) {
      return rows;
    }
    offset = clamp_scroll_offset(offset);
    const float start = std::max(0.0F, offset - cache_extent);
    const float end = std::min(metrics.ContentExtent(), offset + viewport.size.height + cache_extent);
    rows.first = metrics.RowAt(start);
    rows.second = std::min(metrics.RowCount(), metrics.RowAt(end) + std::size_t{1});
    rows.second = std::max(rows.second, rows.first + 1);
    return rows;
  };
  auto measure_rows = [&](const std::pair<std::size_t, std::size_t>& rows) {
    std::vector<float> row_extents(rows.second - rows.first, 0.0F);
    for (std::size_t row = rows.first; row < rows.second; ++row) {
      for (std::size_t index = metrics.FirstItem(row); index < metrics.EndItem(row); ++index) {
        const VirtualGridCell& cell = metrics.Cell(index);
        const float item_width =
            track_width * static_cast<float>(cell.span) + column_spacing * static_cast<float>(cell.span - 1);
        Constraints item_constraints{
            item_width,
            item_width,
            0.0F,
            std::numeric_limits<float>::infinity(),
        };
        if (fixed_row_extent.has_value()) {
          item_constraints = item_constraints.TightHeight(*fixed_row_extent);
        }
        ViewNode& item = context.Item(index);
        const Size item_size = context.Measure(item, item_constraints);
        row_extents[row - rows.first] = std::max(row_extents[row - rows.first], item_size.height);
      }
    }
    if (!fixed_row_extent.has_value()) {
      for (std::size_t row = rows.first; row < rows.second; ++row) {
        metrics.UpdateRow(row, row_extents[row - rows.first]);
      }
    }
  };

  scroll_offset = clamp_scroll_offset(scroll_offset);
  std::pair<std::size_t, std::size_t> rows = resolve_rows(scroll_offset);
  for (int pass = 0; pass < 4 && rows.second > rows.first; ++pass) {
    measure_rows(rows);
    scroll_offset = item_count == 0 ? 0.0F : metrics.Offset(metrics.RowForItem(anchor)) + anchor_delta;
    scroll_offset = clamp_scroll_offset(scroll_offset);
    const auto refined = resolve_rows(scroll_offset);
    if (refined == rows) {
      break;
    }
    rows = refined;
  }
  if (rows.second > rows.first) {
    measure_rows(rows);
  }

  scroll_offset = item_count == 0 ? 0.0F : metrics.Offset(metrics.RowForItem(anchor)) + anchor_delta;
  scroll_offset = clamp_scroll_offset(scroll_offset);
  const float content_height = metrics.ContentExtent();
  const Size measured_size = constraints.Constrain({viewport.size.width, content_height});

  VirtualLayoutResult result;
  result.SetAxis(Axis::Vertical)
      .SetSize(measured_size)
      .SetContentSize({measured_size.width, content_height})
      .SetScrollOffset(scroll_offset)
      .SetCollectionSemantics(
          SemanticRole::Grid,
          SemanticRole::GridCell,
          SemanticCollection{
              .item_count = item_count,
              .row_count = metrics.RowCount(),
              .column_count = metrics.ColumnCount(),
          }
      );
  for (std::size_t row = rows.first; row < rows.second; ++row) {
    const float y = metrics.Offset(row);
    for (std::size_t index = metrics.FirstItem(row); index < metrics.EndItem(row); ++index) {
      const VirtualGridCell& cell = metrics.Cell(index);
      ViewNode& item = context.Item(index);
      result.Place(
          item,
          {
              static_cast<float>(cell.column) * (track_width + column_spacing),
              y,
          },
          SemanticCollectionItem{
              .index = index,
              .row_index = cell.row,
              .column_index = cell.column,
              .column_span = cell.span,
          }
      );
    }
  }
  return result;
}

std::optional<float> VirtualGrid::ScrollOffsetForItem(
    ViewNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent
) {
  auto& metrics = node.Cache<VirtualGridMetrics>();
  if (!metrics.Initialized() || index >= metrics.ItemCount()) {
    return std::nullopt;
  }
  const std::size_t row = metrics.RowForItem(index);
  return AlignedScrollOffset(metrics.Offset(row), metrics.RowExtent(row), viewport_extent, alignment);
}

LayoutResult Column::Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
  return MeasureAxisLayout(context, node, constraints, true);
}

LayoutResult Row::Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
  return MeasureAxisLayout(context, node, constraints, false);
}

LayoutResult Flow::Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
  const Constraints loose = constraints.Loose();
  std::vector<FlowLine> lines = BuildFlowLines(context, node, loose, constraints.max_width);
  const float width = ResolveFlowWidth(node, lines, constraints);
  float content_height = 0.0F;
  for (FlowLine& line : lines) {
    MeasureFlowLine(context, node, line, loose, width);
    content_height += line.height;
  }
  if (lines.size() > 1) {
    content_height += node.Spacing() * static_cast<float>(lines.size() - 1);
  }
  const float height = constraints.ConstrainHeight(content_height);

  LayoutResult result;
  float y = 0.0F;
  for (const FlowLine& line : lines) {
    const AxisPlacement placement = ResolveFlowLinePlacement(node, line, width);
    float x = placement.leading;
    for (ViewNode* child : line.children) {
      result.Place(
          *child,
          {
              x,
              y + CrossOffset(line.height, child->LayoutSize().height, node.CrossAlignment()),
          }
      );
      x += child->LayoutSize().width + placement.gap;
    }
    y += line.height + node.Spacing();
  }
  result.SetSize({width, height});
  return result;
}

LayoutResult Stack::Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
  const Constraints loose = constraints.Loose();
  for (ViewNode& child : node.Children()) {
    static_cast<void>(context.Measure(child, loose));
  }

  float width = 0.0F;
  float height = 0.0F;
  for (ViewNode& child : node.Children()) {
    width = std::max(width, child.LayoutSize().width);
    height = std::max(height, child.LayoutSize().height);
  }
  width = constraints.ConstrainWidth(width);
  height = constraints.ConstrainHeight(height);

  const bool stretch_width = node.HorizontalAlignmentValue() == HorizontalAlignment::Stretch;
  const bool stretch_height = node.VerticalAlignmentValue() == VerticalAlignment::Stretch;
  if (stretch_width) {
    for (ViewNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, loose.TightWidth(width)));
    }
    height = 0.0F;
    for (ViewNode& child : node.Children()) {
      height = std::max(height, child.LayoutSize().height);
    }
    height = constraints.ConstrainHeight(height);
  }
  if (stretch_height) {
    for (ViewNode& child : node.Children()) {
      Constraints child_constraints = loose.TightHeight(height);
      if (stretch_width) {
        child_constraints = child_constraints.TightWidth(width);
      }
      static_cast<void>(context.Measure(child, child_constraints));
    }
  }

  LayoutResult result;
  for (ViewNode& child : node.Children()) {
    const Size child_size = child.LayoutSize();
    result.Place(
        child,
        {
            HorizontalOffset(width, child_size.width, node.HorizontalAlignmentValue()),
            VerticalOffset(height, child_size.height, node.VerticalAlignmentValue()),
        }
    );
  }
  result.SetSize({width, height});
  return result;
}

LayoutResult IndexedPages::Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
  const std::size_t selected_index = node.LayoutValueOr<detail::IndexedPagesSelection>(node.ChildCount());
  if (selected_index >= node.ChildCount()) {
    throw std::logic_error("HuxerUI mounted IndexedPages selected index is out of range");
  }
  ViewNode& selected = node.ChildAt(selected_index);
  const Size size = context.Measure(selected, constraints);
  return LayoutResult{}.Place(selected, {}).SetSize(constraints.Constrain(size));
}

namespace {

std::shared_ptr<detail::ViewSpec> MakeSpacerSpec() {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Spacer);
  spec->layout_values.emplace(typeid(detail::GrowFactorBinding), detail::MakeErasedLayoutValue(1.0F));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeScrollViewSpec(View content) {
  auto spec = MakeContainerSpec(detail::NodeKind::ScrollView, std::vector<View>{std::move(content)});
  spec->component_semantics.role = SemanticRole::ScrollView;
  return spec;
}

std::vector<View> ValidateIndexedPages(std::vector<View> pages, std::size_t selected_index) {
  if (pages.empty()) {
    throw std::invalid_argument("HuxerUI IndexedPages requires at least one page");
  }
  if (selected_index >= pages.size()) {
    throw std::invalid_argument("HuxerUI IndexedPages selected index is out of range");
  }
  if (std::ranges::any_of(pages, [](const View& page) { return !page; })) {
    throw std::invalid_argument("HuxerUI IndexedPages pages must not be empty Views");
  }
  return pages;
}

} // namespace

Spacer::Spacer() : View(MakeSpacerSpec()) {}

IndexedPages::IndexedPages(std::vector<View> pages, std::size_t selected_index)
    : Layout<IndexedPages>(ValidateIndexedPages(std::move(pages), selected_index)) {
  SetLayoutValue(typeid(detail::IndexedPagesSelection), selected_index);
}

ScrollView::ScrollView(View content) : detail::TypedView<ScrollView>(MakeScrollViewSpec(std::move(content))) {}

ScrollView ScrollView::ScrollAxis(Axis axis) && {
  SetLayoutValue(typeid(detail::ScrollAxisBinding), axis);
  return std::move(*this);
}

ScrollView ScrollView::Controller(huxerui::ScrollController controller) && {
  SetLayoutValue(typeid(detail::ScrollControllerBinding), std::move(controller));
  return std::move(*this);
}

VirtualList VirtualList::ScrollAxis(Axis axis) && {
  SetLayoutValue(typeid(detail::ScrollAxisBinding), axis);
  return std::move(*this);
}

VirtualList VirtualList::ItemExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument("HuxerUI virtual item extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualListItemExtent), extent);
  return std::move(*this);
}

VirtualList VirtualList::EstimatedItemExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument("HuxerUI estimated virtual item extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualListEstimatedItemExtent), extent);
  return std::move(*this);
}

VirtualList VirtualList::CacheExtent(float extent) && {
  if (!std::isfinite(extent) || extent < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual cache extent must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualListCacheExtent), extent);
  return std::move(*this);
}

GridColumns GridColumns::Fixed(std::size_t count) {
  if (count == 0) {
    throw std::invalid_argument("HuxerUI fixed grid column count must be positive");
  }
  return GridColumns{Mode::Fixed, count, 0.0F};
}

GridColumns GridColumns::Adaptive(float minimum_width) {
  if (!std::isfinite(minimum_width) || minimum_width <= 0.0F) {
    throw std::invalid_argument("HuxerUI adaptive grid column width must be finite and positive");
  }
  return GridColumns{Mode::Adaptive, 0, minimum_width};
}

std::size_t GridColumns::Resolve(float available_width, float spacing) const noexcept {
  if (mode_ == Mode::Fixed) {
    return count_;
  }
  const float stride = minimum_width_ + std::max(0.0F, spacing);
  return std::max(
      std::size_t{1},
      static_cast<std::size_t>(std::floor((std::max(0.0F, available_width) + std::max(0.0F, spacing)) / stride))
  );
}

VirtualGrid VirtualGrid::Columns(GridColumns columns) && {
  SetLayoutValue(typeid(detail::VirtualGridColumns), columns);
  return std::move(*this);
}

VirtualGrid VirtualGrid::RowExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid row extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualGridRowExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::EstimatedRowExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument(
        "HuxerUI estimated virtual grid row extent must be finite and "
        "positive"
    );
  }
  SetLayoutValue(typeid(detail::VirtualGridEstimatedRowExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::RowSpacing(float spacing) && {
  if (!std::isfinite(spacing) || spacing < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid row spacing must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridRowSpacing), spacing);
  return std::move(*this);
}

VirtualGrid VirtualGrid::ColumnSpacing(float spacing) && {
  if (!std::isfinite(spacing) || spacing < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid column spacing must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridColumnSpacing), spacing);
  return std::move(*this);
}

VirtualGrid VirtualGrid::CacheExtent(float extent) && {
  if (!std::isfinite(extent) || extent < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid cache extent must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridCacheExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::ItemSpans(std::vector<std::size_t> spans) && {
  if (std::ranges::any_of(spans, [](std::size_t span) { return span == 0; })) {
    throw std::invalid_argument("HuxerUI virtual grid item spans must be positive");
  }
  SetLayoutValue(typeid(detail::VirtualGridItemSpans), std::move(spans));
  return std::move(*this);
}

} // namespace huxerui
