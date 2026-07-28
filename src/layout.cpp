#include "internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <huxerui/theme.h>

namespace huxerui::detail {

struct LayoutContextAccess {
  static LayoutContext Create(void *state,
                              LayoutContext::MeasureFunction measure) {
    return LayoutContext{state, measure};
  }
};

struct VirtualLayoutContextAccess {
  static VirtualLayoutContext
  Create(void *state, VirtualLayoutContext::ItemCountFunction item_count,
         VirtualLayoutContext::ViewportFunction viewport,
         VirtualLayoutContext::ItemFunction item,
         VirtualLayoutContext::MeasureFunction measure) {
    return VirtualLayoutContext{state, item_count, viewport, item, measure};
  }
};

namespace {

struct MeasureEnvironment {
  TextService *text_service;
  Runtime *runtime;
};

struct VirtualContextState {
  VirtualMeasureSession *session;
  MeasureEnvironment *environment;
  VirtualViewport viewport;
};

float ResolveFontSize(const MountedNode &node) {
  return node.style.font_size.value_or(
      node.kind == NodeKind::Button
          ? ButtonStyleKey::Default().font_size
          : TextStyleKey::Default().font_size);
}

Constraints ResolveConstraints(const ViewStyle &style,
                               const Constraints &constraints) {
  Constraints resolved = constraints;
  if (style.width.has_value()) {
    const float width =
        std::clamp(std::max(0.0F, *style.width), constraints.min_width,
                   constraints.max_width);
    resolved.min_width = width;
    resolved.max_width = width;
  }
  if (style.height.has_value()) {
    const float height =
        std::clamp(std::max(0.0F, *style.height), constraints.min_height,
                   constraints.max_height);
    resolved.min_height = height;
    resolved.max_height = height;
  }
  return resolved;
}

Size MeasureScopeChild(MountedNode &node, const Constraints &constraints,
                       MeasureEnvironment &environment) {
  if (node.children.empty()) {
    return constraints.Constrain({});
  }
  return MeasureNode(*node.children.front(), constraints,
                     *environment.text_service, *environment.runtime);
}

Size MeasureScrollChild(MountedNode &node, const Constraints &constraints,
                        MeasureEnvironment &environment) {
  if (node.children.empty()) {
    node.scroll_content_height = 0.0F;
    return constraints.Constrain({});
  }

  const Constraints child_constraints{
      constraints.min_width,
      constraints.max_width,
      0.0F,
      std::numeric_limits<float>::infinity(),
  };
  const Size child_size =
      MeasureNode(*node.children.front(), child_constraints,
                  *environment.text_service, *environment.runtime);
  node.scroll_content_height = child_size.height;
  return constraints.Constrain(child_size);
}

Rect ContentRect(const MountedNode &node) {
  return {
      node.frame.x + node.style.padding.left,
      node.frame.y + node.style.padding.top,
      std::max(0.0F, node.frame.width - node.style.padding.Horizontal()),
      std::max(0.0F, node.frame.height - node.style.padding.Vertical()),
  };
}

bool IsScrollable(const MountedNode &node) {
  return node.kind == NodeKind::ScrollView || IsVirtualLayoutNode(node);
}

bool HandlesPointer(const MountedNode &node) {
  return static_cast<bool>(node.activation) ||
         HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerDown>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerMove>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerUp>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerCancel>(node.event_bindings);
}

bool BuildPointerRouteImpl(MountedNode &node, Point position,
                           std::vector<MountedNode *> &route,
                           Point inherited_offset) {
  if (!node.pointer_events_enabled) {
    return false;
  }
  const Point offset{
      inherited_offset.x + node.presentation_offset.x,
      inherited_offset.y + node.presentation_offset.y,
  };
  Rect frame = node.frame;
  frame.x += offset.x;
  frame.y += offset.y;
  if (!frame.Contains(position)) {
    return false;
  }

  route.push_back(&node);
  Rect content = ContentRect(node);
  content.x += offset.x;
  content.y += offset.y;
  const bool can_hit_children =
      !IsScrollable(node) || content.Contains(position);
  if (can_hit_children) {
    for (auto child = node.children.rbegin(); child != node.children.rend();
         ++child) {
      if (BuildPointerRouteImpl(
              **child, position, route, offset)) {
        return true;
      }
    }
  }

  if (HandlesPointer(node) || IsScrollable(node) ||
      node.focusable) {
    return true;
  }
  route.pop_back();
  return false;
}

} // namespace

namespace {

Size MeasureLayoutChild(void *state, huxerui::MountedNode &child,
                        Constraints constraints) {
  auto &environment = *static_cast<MeasureEnvironment *>(state);
  return MeasureNode(static_cast<MountedNode &>(child), constraints,
                     *environment.text_service, *environment.runtime);
}

std::size_t VirtualItemCount(void *state) {
  return static_cast<VirtualContextState *>(state)->session->ItemCount();
}

VirtualViewport CurrentVirtualViewport(void *state) {
  return static_cast<VirtualContextState *>(state)->viewport;
}

huxerui::MountedNode &ObtainVirtualItem(void *state, std::size_t index) {
  return static_cast<VirtualContextState *>(state)->session->Item(index);
}

Size MeasureVirtualItem(void *state, huxerui::MountedNode &item,
                        Constraints constraints) {
  auto &environment = *static_cast<VirtualContextState *>(state)->environment;
  return MeasureNode(static_cast<MountedNode &>(item), constraints,
                     *environment.text_service, *environment.runtime);
}

} // namespace

Size MeasureNode(MountedNode &node, const Constraints &constraints,
                 TextService &text_service, Runtime &runtime) {
  MeasureEnvironment environment{&text_service, &runtime};
  if (IsScrollable(node)) {
    PrepareScrollState(node, runtime);
  }
  const Constraints resolved_constraints =
      ResolveConstraints(node.style, constraints);
  const Constraints content_constraints =
      resolved_constraints.Deflate(node.style.padding);
  Size content_size;

  switch (node.kind) {
  case NodeKind::Text:
    content_size = text_service.MeasureText(node.text, ResolveFontSize(node),
                                            content_constraints.max_width);
    break;
  case NodeKind::Button:
    content_size = text_service.MeasureText(node.text, ResolveFontSize(node));
    break;
  case NodeKind::Checkbox:
  case NodeKind::Switch:
  case NodeKind::ProgressCircle:
  case NodeKind::Spacer:
    break;
  case NodeKind::Layout: {
    if (node.layout == nullptr || node.layout->measure == nullptr) {
      throw std::logic_error("HuxerUI layout node has no measure function");
    }
    LayoutContext context =
        LayoutContextAccess::Create(&environment, MeasureLayoutChild);
    LayoutResult result =
        node.layout->measure(context, node, content_constraints);
    content_size = content_constraints.Constrain(result.MeasuredSize());
    node.layout_placements = result.Placements();
    break;
  }
  case NodeKind::Scope:
    content_size = MeasureScopeChild(node, content_constraints, environment);
    break;
  case NodeKind::ScrollView:
    content_size = MeasureScrollChild(node, content_constraints, environment);
    break;
  case NodeKind::VirtualLayout: {
    if (node.virtual_layout == nullptr ||
        node.virtual_layout->measure == nullptr || !node.virtual_state) {
      throw std::logic_error(
          "HuxerUI virtual layout node has no measure function");
    }
    const Size provisional_viewport{
        content_constraints.HasBoundedWidth()
            ? content_constraints.max_width
            : std::max(content_constraints.min_width, node.measured_size.width),
        content_constraints.HasBoundedHeight()
            ? content_constraints.max_height
            : std::max(content_constraints.min_height,
                       node.measured_size.height),
    };
    VirtualMeasureSession session{runtime, node};
    VirtualContextState state{
        &session,
        &environment,
        {
            {node.scroll_offset_x, node.scroll_offset_y},
            provisional_viewport,
        },
    };
    VirtualLayoutContext context = VirtualLayoutContextAccess::Create(
        &state, VirtualItemCount, CurrentVirtualViewport, ObtainVirtualItem,
        MeasureVirtualItem);
    VirtualLayoutResult result =
        node.virtual_layout->measure(context, node, content_constraints);
    session.Commit(result.Placements());
    node.virtual_state->placements = result.Placements();
    node.virtual_state->axis = result.ScrollAxis();
    node.virtual_state->content_size = result.ContentSize();
    node.scroll_content_width = result.ContentSize().width;
    node.scroll_content_height = result.ContentSize().height;
    if (result.CorrectedScrollOffset().has_value()) {
      if (result.ScrollAxis() == Axis::Vertical) {
        node.scroll_offset_y = *result.CorrectedScrollOffset();
      } else {
        node.scroll_offset_x = *result.CorrectedScrollOffset();
      }
    }
    content_size = content_constraints.Constrain(result.MeasuredSize());
    break;
  }
  }

  const Size measured{
      content_size.width + node.style.padding.Horizontal(),
      content_size.height + node.style.padding.Vertical(),
  };
  node.measured_size = resolved_constraints.Constrain(measured);
  if (IsScrollable(node)) {
    const bool vertical = node.kind == NodeKind::ScrollView ||
                          node.virtual_state->axis == Axis::Vertical;
    const float viewport_extent = std::max(
        0.0F, vertical
                  ? node.measured_size.height - node.style.padding.Vertical()
                  : node.measured_size.width - node.style.padding.Horizontal());
    const float content_extent =
        vertical ? node.scroll_content_height : node.scroll_content_width;
    float &scroll_offset =
        vertical ? node.scroll_offset_y : node.scroll_offset_x;
    const float max_offset = std::max(0.0F, content_extent - viewport_extent);
    scroll_offset = std::clamp(scroll_offset, 0.0F, max_offset);
    CompleteScrollState(node);
  }
  return node.measured_size;
}

void LayoutNode(MountedNode &node, Point origin) {
  node.frame = {
      origin.x,
      origin.y,
      node.measured_size.width,
      node.measured_size.height,
  };

  const Point content_origin{
      origin.x + node.style.padding.left,
      origin.y + node.style.padding.top,
  };
  switch (node.kind) {
  case NodeKind::Layout:
    for (const auto &placement : node.layout_placements) {
      LayoutNode(static_cast<MountedNode &>(*placement.child),
                 {
                     content_origin.x + placement.offset.x,
                     content_origin.y + placement.offset.y,
                 });
    }
    break;
  case NodeKind::Scope:
    for (auto &child : node.children) {
      LayoutNode(*child, content_origin);
    }
    break;
  case NodeKind::ScrollView:
    for (auto &child : node.children) {
      LayoutNode(*child, {
                             content_origin.x,
                             content_origin.y - node.scroll_offset_y,
                         });
    }
    break;
  case NodeKind::VirtualLayout: {
    const bool vertical = node.virtual_state->axis == Axis::Vertical;
    const float scroll_offset =
        vertical ? node.scroll_offset_y : node.scroll_offset_x;
    for (const auto &placement : node.virtual_state->placements) {
      const Point offset =
          vertical
              ? Point{placement.offset.x, placement.offset.y - scroll_offset}
              : Point{placement.offset.x - scroll_offset, placement.offset.y};
      LayoutNode(static_cast<MountedNode &>(*placement.item),
                 {
                     content_origin.x + offset.x,
                     content_origin.y + offset.y,
                 });
    }
    break;
  }
  case NodeKind::Text:
  case NodeKind::Button:
  case NodeKind::Checkbox:
  case NodeKind::Switch:
  case NodeKind::ProgressCircle:
  case NodeKind::Spacer:
    break;
  }
}

bool BuildPointerRoute(MountedNode &node, Point position,
                       std::vector<MountedNode *> &route) {
  route.clear();
  return BuildPointerRouteImpl(node, position, route, {});
}

MountedNode *HitTestPointer(MountedNode &node, Point position) {
  std::vector<MountedNode *> route;
  if (!BuildPointerRoute(node, position, route)) {
    return nullptr;
  }
  const auto target = std::find_if(
      route.rbegin(), route.rend(),
      [](const MountedNode *candidate) { return HandlesPointer(*candidate); });
  return target == route.rend() ? nullptr : *target;
}

std::optional<ScrollBarGeometry>
ResolveScrollBarGeometry(const MountedNode &node) {
  if (!IsScrollable(node)) {
    return std::nullopt;
  }
  const auto binding =
      node.layout_values.find(typeid(ScrollBarBinding));
  if (binding == node.layout_values.end()) {
    return std::nullopt;
  }
  const auto *style = std::any_cast<ScrollBarStyle>(&binding->second);
  if (!style) {
    throw std::logic_error(
        "HuxerUI scroll bar binding type mismatch");
  }

  const bool vertical = node.kind == NodeKind::ScrollView ||
                        node.virtual_state->axis == Axis::Vertical;
  const Rect viewport = ContentRect(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent =
      vertical ? node.scroll_content_height : node.scroll_content_width;
  const float scroll_offset =
      vertical ? node.scroll_offset_y : node.scroll_offset_x;
  const float maximum_offset =
      std::max(0.0F, content_extent - viewport_extent);
  if (maximum_offset <= 0.0F || viewport_extent <= 0.0F) {
    return std::nullopt;
  }

  const float cross_extent = vertical ? viewport.width : viewport.height;
  const float available_cross =
      std::max(0.0F, cross_extent - style->margin * 2.0F);
  const float thickness = std::min(style->thickness, available_cross);
  const float track_extent =
      std::max(0.0F, viewport_extent - style->margin * 2.0F);
  if (thickness <= 0.0F || track_extent <= 0.0F) {
    return std::nullopt;
  }

  Rect track;
  if (vertical) {
    track = {
        viewport.x + viewport.width - style->margin - thickness,
        viewport.y + style->margin,
        thickness,
        track_extent,
    };
  } else {
    track = {
        viewport.x + style->margin,
        viewport.y + viewport.height - style->margin - thickness,
        track_extent,
        thickness,
    };
  }

  const float thumb_extent = std::clamp(
      std::max(style->minimum_thumb_extent,
               track_extent * viewport_extent / content_extent),
      0.0F, track_extent);
  const float thumb_travel = track_extent - thumb_extent;
  const float thumb_offset =
      thumb_travel * std::clamp(scroll_offset / maximum_offset,
                                0.0F, 1.0F);
  const Rect thumb =
      vertical
          ? Rect{track.x, track.y + thumb_offset,
                 track.width, thumb_extent}
          : Rect{track.x + thumb_offset, track.y,
                 thumb_extent, track.height};
  return ScrollBarGeometry{
      vertical ? Axis::Vertical : Axis::Horizontal,
      track,
      thumb,
      *style,
      scroll_offset,
      maximum_offset,
      thumb_travel,
  };
}

bool CanScrollNode(const MountedNode &node, float delta) {
  if (!node.enabled || !IsScrollable(node) || delta == 0.0F) {
    return false;
  }
  const bool vertical = node.kind == NodeKind::ScrollView ||
                        node.virtual_state->axis == Axis::Vertical;
  const Rect viewport = ContentRect(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent =
      vertical ? node.scroll_content_height : node.scroll_content_width;
  const float scroll_offset =
      vertical ? node.scroll_offset_y : node.scroll_offset_x;
  const float max_offset = std::max(0.0F, content_extent - viewport_extent);
  return delta < 0.0F ? scroll_offset > 0.0F : scroll_offset < max_offset;
}

float ScrollNodeBy(MountedNode &node, float delta) {
  if (!node.enabled || !IsScrollable(node)) {
    return 0.0F;
  }
  const bool vertical = node.kind == NodeKind::ScrollView ||
                        node.virtual_state->axis == Axis::Vertical;
  const Rect viewport = ContentRect(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent =
      vertical ? node.scroll_content_height : node.scroll_content_width;
  float &scroll_offset = vertical ? node.scroll_offset_y : node.scroll_offset_x;
  const float max_offset = std::max(0.0F, content_extent - viewport_extent);
  const float previous = scroll_offset;
  scroll_offset = std::clamp(scroll_offset + delta, 0.0F, max_offset);
  if (scroll_offset != previous && node.scroll_connection) {
    node.scroll_connection->PublishMetrics();
  }
  return scroll_offset - previous;
}

MountedNode *ScrollNode(MountedNode &node, const ScrollEvent &event) {
  if (!node.enabled || !node.frame.Contains(event.position)) {
    return nullptr;
  }

  const bool can_scroll_children =
      !IsScrollable(node) || ContentRect(node).Contains(event.position);
  if (can_scroll_children) {
    for (auto child = node.children.rbegin(); child != node.children.rend();
         ++child) {
      if (MountedNode *scrolled = ScrollNode(**child, event)) {
        return scrolled;
      }
    }
  }

  if (!IsScrollable(node)) {
    return nullptr;
  }

  const bool vertical = node.kind == NodeKind::ScrollView ||
                        node.virtual_state->axis == Axis::Vertical;
  const float delta = vertical ? event.delta_y : event.delta_x;
  return ScrollNodeBy(node, delta) != 0.0F ? &node : nullptr;
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

class VirtualListMetrics {
public:
  void Prepare(std::size_t item_count, Axis axis, float spacing,
               std::optional<float> fixed_extent, float estimated_extent,
               bool source_dirty) {
    const bool axis_changed = initialized_ && axis_ != axis;
    const bool structure_changed =
        !initialized_ || item_count_ != item_count || axis_changed ||
        spacing_ != spacing || fixed_extent_ != fixed_extent ||
        configured_estimate_ != estimated_extent || source_dirty;
    if (!structure_changed) {
      return;
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
  }

  [[nodiscard]] bool Initialized() const noexcept { return initialized_; }

  [[nodiscard]] Axis CurrentAxis() const noexcept { return axis_; }

  [[nodiscard]] std::size_t ItemCount() const noexcept { return item_count_; }

  [[nodiscard]] float Estimate() const noexcept { return estimate_; }

  [[nodiscard]] float Offset(std::size_t index) const {
    index = std::min(index, item_count_);
    if (fixed_extent_.has_value()) {
      return static_cast<float>(index) * (*fixed_extent_ + spacing_);
    }
    const float measured_sum = Prefix(measured_sum_tree_, index);
    const float measured_count = Prefix(measured_count_tree_, index);
    return measured_sum +
           (static_cast<float>(index) - measured_count) * estimate_ +
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
      return std::min(item_count_ - 1,
                      static_cast<std::size_t>(std::floor(
                          std::max(0.0F, offset) / std::max(stride, 0.0001F))));
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
      const float block = known_sum +
                          (static_cast<float>(step) - known_count) * estimate_ +
                          static_cast<float>(step) * spacing_;
      if (accumulated + block <= std::max(0.0F, offset)) {
        position = next;
        accumulated += block;
      }
    }
    return std::min(position, item_count_ - 1);
  }

  void RestoreKey(std::size_t index,
                  const std::optional<detail::ViewKey> &key) {
    if (fixed_extent_.has_value() || !key.has_value()) {
      return;
    }
    if (const auto found = keyed_extents_.find(*key);
        found != keyed_extents_.end()) {
      Update(index, found->second, key);
    }
  }

  void Update(std::size_t index, float extent,
              const std::optional<detail::ViewKey> &key) {
    if (fixed_extent_.has_value() || index >= item_count_ ||
        !std::isfinite(extent) || extent <= 0.0F) {
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
  static void Add(std::vector<float> &tree, std::size_t index, float delta) {
    for (++index; index < tree.size(); index += index & (~index + 1)) {
      tree[index] += delta;
    }
  }

  static float Prefix(const std::vector<float> &tree, std::size_t count) {
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
  [[nodiscard]] bool Initialized() const noexcept { return initialized_; }

  [[nodiscard]] std::size_t ItemCount() const noexcept { return item_count_; }

  [[nodiscard]] std::size_t RowCount() const noexcept { return row_count_; }

  [[nodiscard]] const VirtualGridCell &Cell(std::size_t index) const {
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

  [[nodiscard]] float ContentExtent() const { return rows_.ContentExtent(); }

  [[nodiscard]] float RowExtent(std::size_t row) const {
    return rows_.Extent(row);
  }

  void UpdateRow(std::size_t row, float extent) {
    rows_.Update(row, extent, std::nullopt);
  }

  bool Prepare(std::size_t item_count, std::size_t columns, float track_width,
               float row_spacing, std::optional<float> fixed_row_extent,
               float estimated_row_extent,
               const std::vector<std::size_t> &spans, bool source_dirty) {
    const bool plan_changed = !initialized_ || item_count_ != item_count ||
                              columns_ != columns || spans_ != spans;
    if (plan_changed) {
      BuildPlan(item_count, columns, spans);
    }

    const bool geometry_changed =
        plan_changed || track_width_ != track_width ||
        row_spacing_ != row_spacing || fixed_row_extent_ != fixed_row_extent ||
        estimated_row_extent_ != estimated_row_extent || source_dirty;
    rows_.Prepare(row_count_, Axis::Vertical, row_spacing, fixed_row_extent,
                  estimated_row_extent, geometry_changed);
    track_width_ = track_width;
    row_spacing_ = row_spacing;
    fixed_row_extent_ = fixed_row_extent;
    estimated_row_extent_ = estimated_row_extent;
    initialized_ = true;
    return geometry_changed;
  }

private:
  void BuildPlan(std::size_t item_count, std::size_t columns,
                 const std::vector<std::size_t> &spans) {
    item_count_ = item_count;
    columns_ = columns;
    spans_ = spans;
    cells_.clear();
    cells_.reserve(item_count);
    row_starts_.clear();

    std::size_t row = 0;
    std::size_t column = 0;
    for (std::size_t index = 0; index < item_count; ++index) {
      const std::size_t requested_span =
          index < spans.size() ? spans[index] : std::size_t{1};
      const std::size_t span =
          std::clamp(requested_span, std::size_t{1}, columns);
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
  return vertical ? constraints.TightHeight(value)
                  : constraints.TightWidth(value);
}

Constraints TightCross(Constraints constraints, bool vertical, float value) {
  return vertical ? constraints.TightWidth(value)
                  : constraints.TightHeight(value);
}

float MinimumMain(const Constraints &constraints, bool vertical) {
  return vertical ? constraints.min_height : constraints.min_width;
}

float MaximumMain(const Constraints &constraints, bool vertical) {
  return vertical ? constraints.max_height : constraints.max_width;
}

float MinimumCross(const Constraints &constraints, bool vertical) {
  return vertical ? constraints.min_width : constraints.min_height;
}

float MaximumCross(const Constraints &constraints, bool vertical) {
  return vertical ? constraints.max_width : constraints.max_height;
}

float TotalSpacing(const MountedNode &node) {
  return node.ChildCount() < 2
             ? 0.0F
             : node.Spacing() * static_cast<float>(node.ChildCount() - 1);
}

float SumMainSizes(const MountedNode &node, bool vertical) {
  float result = 0.0F;
  for (std::size_t index = 0; index < node.ChildCount(); ++index) {
    result += LayoutMainSize(node.ChildAt(index).MeasuredSize(), vertical);
  }
  return result;
}

float MaxCrossSize(const MountedNode &node, bool vertical) {
  float result = 0.0F;
  for (std::size_t index = 0; index < node.ChildCount(); ++index) {
    result = std::max(
        result, LayoutCrossSize(node.ChildAt(index).MeasuredSize(), vertical));
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

float AlignedScrollOffset(float start, float extent, float viewport_extent,
                          ScrollAlignment alignment) {
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

float HorizontalOffset(float available, float child,
                       HorizontalAlignment alignment) {
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

float VerticalOffset(float available, float child,
                     VerticalAlignment alignment) {
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

AxisPlacement ResolveAxisPlacement(const MountedNode &node, float available,
                                   bool vertical) {
  AxisPlacement result{0.0F, node.Spacing()};
  const std::size_t count = node.ChildCount();
  if (count == 0) {
    return result;
  }

  const float used = SumMainSizes(node, vertical) + TotalSpacing(node);
  const float remaining = std::max(0.0F, available - used);
  switch (node.MainAlignment()) {
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

LayoutResult MeasureAxisLayout(LayoutContext &context, MountedNode &node,
                               Constraints constraints, bool vertical) {
  const Constraints loose = constraints.Loose();
  float total_grow = 0.0F;

  for (MountedNode &child : node.Children()) {
    static_cast<void>(context.Measure(child, loose));
    total_grow += child.GrowFactor();
  }

  const bool stretch = node.CrossAlignment() == CrossAxisAlignment::Stretch;
  float target_cross = std::clamp(MaxCrossSize(node, vertical),
                                  MinimumCross(constraints, vertical),
                                  MaximumCross(constraints, vertical));

  if (stretch) {
    for (MountedNode &child : node.Children()) {
      static_cast<void>(
          context.Measure(child, TightCross(loose, vertical, target_cross)));
    }
  }

  const float spacing = TotalSpacing(node);
  float fixed_main = 0.0F;
  for (MountedNode &child : node.Children()) {
    if (child.GrowFactor() <= 0.0F) {
      fixed_main += LayoutMainSize(child.MeasuredSize(), vertical);
    }
  }

  const float max_main = MaximumMain(constraints, vertical);
  float target_main = fixed_main + spacing;
  if (total_grow > 0.0F && std::isfinite(max_main)) {
    target_main = max_main;
    const float remaining = std::max(0.0F, target_main - fixed_main - spacing);
    for (MountedNode &child : node.Children()) {
      if (child.GrowFactor() <= 0.0F) {
        continue;
      }
      const float share = remaining * child.GrowFactor() / total_grow;
      Constraints child_constraints = TightMain(loose, vertical, share);
      if (stretch) {
        child_constraints =
            TightCross(child_constraints, vertical, target_cross);
      }
      static_cast<void>(context.Measure(child, child_constraints));
    }
  } else {
    target_main = SumMainSizes(node, vertical) + spacing;
    if (node.MainAlignment() != MainAxisAlignment::Start &&
        std::isfinite(max_main)) {
      target_main = max_main;
    }
  }

  target_main =
      std::clamp(target_main, MinimumMain(constraints, vertical), max_main);
  if (!stretch) {
    target_cross = std::clamp(MaxCrossSize(node, vertical),
                              MinimumCross(constraints, vertical),
                              MaximumCross(constraints, vertical));
  }

  LayoutResult result;
  const AxisPlacement placement =
      ResolveAxisPlacement(node, target_main, vertical);
  float main = placement.leading;
  for (MountedNode &child : node.Children()) {
    const Size child_size = child.MeasuredSize();
    const float cross =
        CrossOffset(target_cross, LayoutCrossSize(child_size, vertical),
                    node.CrossAlignment());
    result.Place(child, vertical ? Point{cross, main} : Point{main, cross});
    main += LayoutMainSize(child_size, vertical) + placement.gap;
  }

  result.SetSize(MakeAxisSize(target_main, target_cross, vertical));
  return result;
}

} // namespace

VirtualLayoutResult VirtualList::Measure(VirtualLayoutContext &context,
                                         MountedNode &node,
                                         Constraints constraints) {
  const Axis axis = node.LayoutValueOr<detail::VirtualListAxis>(Axis::Vertical);
  const bool vertical = axis == Axis::Vertical;
  if ((vertical && !constraints.HasBoundedHeight()) ||
      (!vertical && !constraints.HasBoundedWidth())) {
    throw std::logic_error(
        "HuxerUI VirtualList requires bounded constraints on its scroll axis");
  }

  std::optional<float> fixed_extent;
  if (const float *value = node.LayoutValue<detail::VirtualListItemExtent>()) {
    fixed_extent = *value;
  }
  const float configured_estimate =
      node.LayoutValueOr<detail::VirtualListEstimatedItemExtent>(56.0F);
  const VirtualViewport viewport = context.Viewport();
  const float viewport_extent =
      vertical ? viewport.size.height : viewport.size.width;
  const float configured_cache =
      node.LayoutValueOr<detail::VirtualListCacheExtent>(
          std::max(200.0F, viewport_extent * 0.5F));
  const std::size_t item_count = context.ItemCount();

  auto &internal_node = static_cast<detail::MountedNode &>(node);
  auto &metrics = node.Cache<VirtualListMetrics>();
  const bool had_metrics = metrics.Initialized();
  const bool source_dirty = internal_node.virtual_state->source_dirty;
  const bool axis_changed =
      metrics.Initialized() && metrics.CurrentAxis() != axis;
  const float previous_scroll =
      metrics.Initialized()
          ? (metrics.CurrentAxis() == Axis::Vertical ? viewport.offset.y
                                                     : viewport.offset.x)
          : 0.0F;
  const std::size_t previous_anchor =
      metrics.Initialized() ? metrics.IndexAt(previous_scroll) : 0;
  const float previous_anchor_delta =
      metrics.Initialized() ? previous_scroll - metrics.Offset(previous_anchor)
                            : 0.0F;

  metrics.Prepare(item_count, axis, node.Spacing(), fixed_extent,
                  configured_estimate, source_dirty);

  float scroll_offset = vertical ? viewport.offset.y : viewport.offset.x;
  std::size_t anchor =
      item_count == 0 ? 0 : std::min(previous_anchor, item_count - 1);
  float anchor_delta = previous_anchor_delta;
  if (axis_changed) {
    scroll_offset = metrics.Offset(anchor);
    anchor_delta = 0.0F;
  } else if (!source_dirty || !had_metrics) {
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
    const float end =
        std::min(content_extent, offset + viewport_extent + configured_cache);
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
  if (node.CrossAlignment() == CrossAxisAlignment::Stretch &&
      std::isfinite(MaximumCross(constraints, vertical))) {
    item_constraints = TightCross(item_constraints, vertical,
                                  MaximumCross(constraints, vertical));
  }
  if (fixed_extent.has_value()) {
    item_constraints = TightMain(item_constraints, vertical, *fixed_extent);
  }

  std::pair<std::size_t, std::size_t> range = resolve_range(scroll_offset);
  for (int pass = 0; pass < 4 && range.second > range.first; ++pass) {
    for (std::size_t index = range.first; index < range.second; ++index) {
      MountedNode &item = context.Item(index);
      const auto &key = static_cast<detail::MountedNode &>(item).key;
      metrics.RestoreKey(index, key);
      const Size item_size = context.Measure(item, item_constraints);
      metrics.Update(index, LayoutMainSize(item_size, vertical), key);
    }

    scroll_offset =
        item_count == 0
            ? 0.0F
            : metrics.Offset(std::min(anchor, item_count - 1)) + anchor_delta;
    const float maximum =
        std::max(0.0F, metrics.ContentExtent() - viewport_extent);
    scroll_offset = std::clamp(scroll_offset, 0.0F, maximum);
    const auto refined = resolve_range(scroll_offset);
    if (refined == range) {
      break;
    }
    range = refined;
  }

  float cross_extent = 0.0F;
  for (std::size_t index = range.first; index < range.second; ++index) {
    MountedNode &item = context.Item(index);
    const auto &key = static_cast<detail::MountedNode &>(item).key;
    metrics.RestoreKey(index, key);
    const Size item_size = context.Measure(item, item_constraints);
    metrics.Update(index, LayoutMainSize(item_size, vertical), key);
    cross_extent = std::max(cross_extent, LayoutCrossSize(item_size, vertical));
  }

  scroll_offset =
      item_count == 0
          ? 0.0F
          : metrics.Offset(std::min(anchor, item_count - 1)) + anchor_delta;
  const float content_extent = metrics.ContentExtent();
  const Size measured_size = constraints.Constrain(
      MakeAxisSize(content_extent, cross_extent, vertical));
  const float measured_cross = LayoutCrossSize(measured_size, vertical);

  VirtualLayoutResult result;
  result.SetAxis(axis)
      .SetSize(measured_size)
      .SetContentSize(MakeAxisSize(content_extent, measured_cross, vertical))
      .SetScrollOffset(scroll_offset);

  for (std::size_t index = range.first; index < range.second; ++index) {
    MountedNode &item = context.Item(index);
    const Size item_size = item.MeasuredSize();
    const float cross =
        CrossOffset(measured_cross, LayoutCrossSize(item_size, vertical),
                    node.CrossAlignment());
    const float main = metrics.Offset(index);
    result.Place(item, vertical ? Point{cross, main} : Point{main, cross});
  }
  return result;
}

std::optional<float> VirtualList::ScrollOffsetForItem(MountedNode &node,
                                                      std::size_t index,
                                                      ScrollAlignment alignment,
                                                      float viewport_extent) {
  auto &metrics = node.Cache<VirtualListMetrics>();
  if (!metrics.Initialized() || index >= metrics.ItemCount()) {
    return std::nullopt;
  }
  return AlignedScrollOffset(metrics.Offset(index), metrics.Extent(index),
                             viewport_extent, alignment);
}

VirtualLayoutResult VirtualGrid::Measure(VirtualLayoutContext &context,
                                         MountedNode &node,
                                         Constraints constraints) {
  if (!constraints.HasBoundedWidth() || !constraints.HasBoundedHeight()) {
    throw std::logic_error(
        "HuxerUI VirtualGrid requires bounded width and height");
  }

  const VirtualViewport viewport = context.Viewport();
  const GridColumns column_configuration =
      node.LayoutValueOr<detail::VirtualGridColumns>(
          GridColumns::Adaptive(160.0F));
  const float configured_column_spacing =
      node.LayoutValueOr<detail::VirtualGridColumnSpacing>(node.Spacing());
  const std::size_t columns = column_configuration.Resolve(
      viewport.size.width, configured_column_spacing);
  const float column_spacing =
      columns > 1
          ? std::min(configured_column_spacing,
                     viewport.size.width / static_cast<float>(columns - 1))
          : 0.0F;
  const float track_width =
      std::max(0.0F, viewport.size.width -
                         column_spacing * static_cast<float>(columns - 1)) /
      static_cast<float>(columns);
  const float row_spacing =
      node.LayoutValueOr<detail::VirtualGridRowSpacing>(node.Spacing());
  std::optional<float> fixed_row_extent;
  if (const float *value = node.LayoutValue<detail::VirtualGridRowExtent>()) {
    fixed_row_extent = *value;
  }
  const float estimated_row_extent =
      node.LayoutValueOr<detail::VirtualGridEstimatedRowExtent>(56.0F);
  const float cache_extent = node.LayoutValueOr<detail::VirtualGridCacheExtent>(
      std::max(200.0F, viewport.size.height * 0.5F));
  const auto *configured_spans =
      node.LayoutValue<detail::VirtualGridItemSpans>();
  const std::vector<std::size_t> empty_spans;
  const auto &spans =
      configured_spans == nullptr ? empty_spans : *configured_spans;
  const std::size_t item_count = context.ItemCount();

  auto &internal_node = static_cast<detail::MountedNode &>(node);
  auto &metrics = node.Cache<VirtualGridMetrics>();
  const bool had_layout = metrics.Initialized();
  const std::size_t previous_row = had_layout && metrics.RowCount() > 0
                                       ? metrics.RowAt(viewport.offset.y)
                                       : 0;
  const std::size_t previous_anchor = had_layout && metrics.RowCount() > 0
                                          ? metrics.FirstItem(previous_row)
                                          : 0;
  const float previous_anchor_delta =
      had_layout && metrics.RowCount() > 0
          ? viewport.offset.y - metrics.Offset(previous_row)
          : 0.0F;
  const bool geometry_changed = metrics.Prepare(
      item_count, columns, track_width, row_spacing, fixed_row_extent,
      estimated_row_extent, spans, internal_node.virtual_state->source_dirty);

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
    const float maximum =
        std::max(0.0F, metrics.ContentExtent() - viewport.size.height);
    return std::clamp(offset, 0.0F, maximum);
  };
  auto resolve_rows = [&](float offset) {
    std::pair<std::size_t, std::size_t> rows;
    if (metrics.RowCount() == 0) {
      return rows;
    }
    offset = clamp_scroll_offset(offset);
    const float start = std::max(0.0F, offset - cache_extent);
    const float end = std::min(metrics.ContentExtent(),
                               offset + viewport.size.height + cache_extent);
    rows.first = metrics.RowAt(start);
    rows.second =
        std::min(metrics.RowCount(), metrics.RowAt(end) + std::size_t{1});
    rows.second = std::max(rows.second, rows.first + 1);
    return rows;
  };
  auto measure_rows = [&](const std::pair<std::size_t, std::size_t> &rows) {
    std::vector<float> row_extents(rows.second - rows.first, 0.0F);
    for (std::size_t row = rows.first; row < rows.second; ++row) {
      for (std::size_t index = metrics.FirstItem(row);
           index < metrics.EndItem(row); ++index) {
        const VirtualGridCell &cell = metrics.Cell(index);
        const float item_width =
            track_width * static_cast<float>(cell.span) +
            column_spacing * static_cast<float>(cell.span - 1);
        Constraints item_constraints{
            item_width,
            item_width,
            0.0F,
            std::numeric_limits<float>::infinity(),
        };
        if (fixed_row_extent.has_value()) {
          item_constraints = item_constraints.TightHeight(*fixed_row_extent);
        }
        MountedNode &item = context.Item(index);
        const Size item_size = context.Measure(item, item_constraints);
        row_extents[row - rows.first] =
            std::max(row_extents[row - rows.first], item_size.height);
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
    scroll_offset =
        item_count == 0
            ? 0.0F
            : metrics.Offset(metrics.RowForItem(anchor)) + anchor_delta;
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

  scroll_offset = item_count == 0 ? 0.0F
                                  : metrics.Offset(metrics.RowForItem(anchor)) +
                                        anchor_delta;
  scroll_offset = clamp_scroll_offset(scroll_offset);
  const float content_height = metrics.ContentExtent();
  const Size measured_size =
      constraints.Constrain({viewport.size.width, content_height});

  VirtualLayoutResult result;
  result.SetAxis(Axis::Vertical)
      .SetSize(measured_size)
      .SetContentSize({measured_size.width, content_height})
      .SetScrollOffset(scroll_offset);
  for (std::size_t row = rows.first; row < rows.second; ++row) {
    const float y = metrics.Offset(row);
    for (std::size_t index = metrics.FirstItem(row);
         index < metrics.EndItem(row); ++index) {
      const VirtualGridCell &cell = metrics.Cell(index);
      MountedNode &item = context.Item(index);
      result.Place(item, {
                             static_cast<float>(cell.column) *
                                 (track_width + column_spacing),
                             y,
                         });
    }
  }
  return result;
}

std::optional<float> VirtualGrid::ScrollOffsetForItem(MountedNode &node,
                                                      std::size_t index,
                                                      ScrollAlignment alignment,
                                                      float viewport_extent) {
  auto &metrics = node.Cache<VirtualGridMetrics>();
  if (!metrics.Initialized() || index >= metrics.ItemCount()) {
    return std::nullopt;
  }
  const std::size_t row = metrics.RowForItem(index);
  return AlignedScrollOffset(metrics.Offset(row), metrics.RowExtent(row),
                             viewport_extent, alignment);
}

LayoutResult Column::Measure(LayoutContext &context, MountedNode &node,
                             Constraints constraints) {
  return MeasureAxisLayout(context, node, constraints, true);
}

LayoutResult Row::Measure(LayoutContext &context, MountedNode &node,
                          Constraints constraints) {
  return MeasureAxisLayout(context, node, constraints, false);
}

LayoutResult Stack::Measure(LayoutContext &context, MountedNode &node,
                            Constraints constraints) {
  const Constraints loose = constraints.Loose();
  for (MountedNode &child : node.Children()) {
    static_cast<void>(context.Measure(child, loose));
  }

  float width = 0.0F;
  float height = 0.0F;
  for (MountedNode &child : node.Children()) {
    width = std::max(width, child.MeasuredSize().width);
    height = std::max(height, child.MeasuredSize().height);
  }
  width = constraints.ConstrainWidth(width);
  height = constraints.ConstrainHeight(height);

  const bool stretch_width =
      node.HorizontalAlignmentValue() == HorizontalAlignment::Stretch;
  const bool stretch_height =
      node.VerticalAlignmentValue() == VerticalAlignment::Stretch;
  if (stretch_width) {
    for (MountedNode &child : node.Children()) {
      static_cast<void>(context.Measure(child, loose.TightWidth(width)));
    }
    height = 0.0F;
    for (MountedNode &child : node.Children()) {
      height = std::max(height, child.MeasuredSize().height);
    }
    height = constraints.ConstrainHeight(height);
  }
  if (stretch_height) {
    for (MountedNode &child : node.Children()) {
      Constraints child_constraints = loose.TightHeight(height);
      if (stretch_width) {
        child_constraints = child_constraints.TightWidth(width);
      }
      static_cast<void>(context.Measure(child, child_constraints));
    }
  }

  LayoutResult result;
  for (MountedNode &child : node.Children()) {
    const Size child_size = child.MeasuredSize();
    result.Place(child, {
                            HorizontalOffset(width, child_size.width,
                                             node.HorizontalAlignmentValue()),
                            VerticalOffset(height, child_size.height,
                                           node.VerticalAlignmentValue()),
                        });
  }
  result.SetSize({width, height});
  return result;
}

} // namespace huxerui
