#include "ui_window_internal.h"
#include "internal_access.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <huxerui/theme.h>

#include "components/text_field_internal.h"
#include "components/selection_area_internal.h"
#include "profiling_internal.h"
#include "text/text_internal.h"

namespace huxerui::detail {

bool MountedNode::IsFocusable() const noexcept {
  return declared_focusable || std::ranges::any_of(text.LinkRanges(), [this](const auto& link) {
    return !cached_paragraph_layout || cached_paragraph_layout->RetainsSourceRange(link.range);
  });
}

LayoutContext InternalAccess::CreateLayoutContext(
    void* state, LayoutContext::MeasureFunction measure, EdgeInsets safe_area,
    const WindowTitleBarMetrics* title_bar_metrics) {
  return LayoutContext{state, measure, safe_area, title_bar_metrics};
}

VirtualLayoutContext InternalAccess::CreateVirtualLayoutContext(
    void* state, VirtualLayoutContext::ItemCountFunction item_count, VirtualLayoutContext::ViewportFunction viewport,
    VirtualLayoutContext::ItemFunction item, VirtualLayoutContext::MeasureFunction measure) {
  return VirtualLayoutContext{state, item_count, viewport, item, measure};
}

std::optional<VirtualCollectionSemantics> InternalAccess::CollectionSemantics(const VirtualLayoutResult& result) {
  if (!result.collection_.has_value()) {
    if (std::ranges::any_of(result.placements_, [](const VirtualLayoutResult::Placement& placement) {
          return placement.collection_item.has_value();
        })) {
      throw std::logic_error("HuxerUI virtual collection item metadata requires collection semantics");
    }
    return std::nullopt;
  }

  for (const VirtualLayoutResult::Placement& placement : result.placements_) {
    if (!placement.collection_item.has_value()) {
      continue;
    }
    const SemanticCollectionItem& item = *placement.collection_item;
    if (item.row_span == 0 || item.column_span == 0 ||
        (item.index.has_value() && result.collection_->item_count.has_value() &&
         *item.index >= *result.collection_->item_count) ||
        (item.row_index.has_value() && result.collection_->row_count.has_value() &&
         (*item.row_index >= *result.collection_->row_count ||
          item.row_span > *result.collection_->row_count - *item.row_index)) ||
        (item.column_index.has_value() && result.collection_->column_count.has_value() &&
         (*item.column_index >= *result.collection_->column_count ||
          item.column_span > *result.collection_->column_count - *item.column_index))) {
      throw std::logic_error("HuxerUI virtual collection item metadata is outside its collection bounds");
    }
  }
  return VirtualCollectionSemantics{
      result.collection_role_,
      result.collection_item_role_,
      *result.collection_,
  };
}

std::array<std::optional<float>, 2> VirtualMeasureSession::ScrollCorrections(const VirtualLayoutResult& result) const {
  if (owner_->scroll_state->both_axes) {
    if (result.axis_ || result.scroll_offset_) {
      throw std::logic_error("HuxerUI two-axis virtual layout must report corrections by axis");
    }
    return result.axis_offsets_;
  }
  if (result.axis_offsets_[0] || result.axis_offsets_[1]) {
    throw std::logic_error("HuxerUI single-axis virtual layout must report a scalar correction");
  }
  std::array<std::optional<float>, 2> offsets{};
  offsets[ScrollAxisIndex(result.ScrollAxis())] = result.scroll_offset_;
  return offsets;
}

namespace {

struct LayoutContextState {
  UiWindow* ui_window;
  EdgeInsets safe_area;
  const WindowTitleBarMetrics* title_bar_metrics;
};

struct VirtualLayoutContextState {
  VirtualMeasureSession* session;
  LayoutContextState* layout_state;
  VirtualViewport viewport;
};

std::pair<float, float> ResolveAxisConstraints(float parent_min, float parent_max, std::optional<float> preferred,
                                               std::optional<float> local_min, std::optional<float> local_max) {
  const float requested_min = local_min.value_or(0.0F);
  const float requested_max = local_max.value_or(std::numeric_limits<float>::infinity());
  float resolved_min = std::max(parent_min, requested_min);
  float resolved_max = std::min(parent_max, requested_max);
  if (resolved_min > resolved_max) {
    const float parent_edge = requested_max < parent_min ? parent_min : parent_max;
    resolved_min = parent_edge;
    resolved_max = parent_edge;
  }
  if (preferred.has_value()) {
    const float value = std::clamp(*preferred, resolved_min, resolved_max);
    resolved_min = value;
    resolved_max = value;
  }
  return {resolved_min, resolved_max};
}

Size MeasureScopeChild(MountedNode& node, const Constraints& constraints, LayoutContextState& state) {
  if (node.children.empty()) {
    return constraints.Constrain({});
  }
  return MeasureNode(*node.children.front(), constraints, *state.ui_window, state.safe_area,
                     state.title_bar_metrics);
}

void SetScrollContentSize(MountedNode& node, Size size) {
  auto& scroll = *node.scroll_state;
  if (scroll.content_width != size.width || scroll.content_height != size.height) {
    scroll.content_width = size.width;
    scroll.content_height = size.height;
    node.foreground_paint_dirty = true;
  }
}

Size MeasureScrollChild(MountedNode& node, const Constraints& constraints, LayoutContextState& state) {
  if (node.children.empty()) {
    SetScrollContentSize(node, {});
    return constraints.Constrain({});
  }

  const bool horizontal = node.scroll_state->Supports(Axis::Horizontal);
  const bool vertical = node.scroll_state->Supports(Axis::Vertical);
  const bool fill_viewport = node.LayoutValueOr<detail::ScrollFillViewport>(false);
  const float viewport_width = constraints.HasBoundedWidth() ? constraints.max_width : constraints.min_width;
  const float viewport_height = constraints.HasBoundedHeight() ? constraints.max_height : constraints.min_height;
  const Constraints child_constraints{
      horizontal ? (fill_viewport ? viewport_width : 0.0F) : constraints.min_width,
      horizontal ? std::numeric_limits<float>::infinity() : constraints.max_width,
      vertical ? (fill_viewport ? viewport_height : 0.0F) : constraints.min_height,
      vertical ? std::numeric_limits<float>::infinity() : constraints.max_height,
  };
  const Size child_size = MeasureNode(*node.children.front(), child_constraints, *state.ui_window,
                                      state.safe_area, state.title_bar_metrics);
  SetScrollContentSize(node, child_size);
  return constraints.Constrain(child_size);
}

bool HandlesPointer(const MountedNode& node) {
  return static_cast<bool>(node.activation) || HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
         HasEventBinding<ViewEvents::Pointer>(node.event_bindings);
}

bool ExtensionHandlesPointer(MountedNode& node, Point position) {
  return std::any_of(node.extensions.begin(), node.extensions.end(), [&](const NodeExtensionEntry& entry) {
    return entry.extension && entry.extension->HitTest(node, position);
  });
}

bool ExtensionHandlesHover(MountedNode& node, Point position) {
  return std::any_of(node.extensions.begin(), node.extensions.end(), [&](const NodeExtensionEntry& entry) {
    return entry.extension && (node.interaction.enabled || entry.extension->HoverWhenDisabled()) &&
           entry.extension->HoverHitTest(node, position);
  });
}

enum class PointerRoutePurpose {
  Input,
  Cursor,
  Hover,
};

bool AllowsChildHit(const MountedNode& node, Point local_position) {
  if (IsScrollContainer(node) && !node.ContentBounds().Contains(local_position)) {
    return false;
  }
  if (node.properties.clip_children &&
      !RoundedRectContains(node.bounds, node.properties.corner_radii, local_position)) {
    return false;
  }
  return std::all_of(
      node.presentation.children_clips.begin(), node.presentation.children_clips.end(),
      [local_position](const ClipShape& shape) { return InternalAccess::ClipContains(shape, local_position); }
  );
}

bool BuildPointerRouteImpl(MountedNode& node, Point position, std::vector<MountedNode*>& route,
                           PointerRoutePurpose purpose) {
  if (!node.participates_in_layout || !node.pointer_events_enabled) {
    return false;
  }
  const auto local_position = node.WindowToLocal(position);
  if (!local_position.has_value()) {
    return false;
  }

  route.push_back(&node);
  const bool within_node = node.bounds.Contains(*local_position);
  if (AllowsChildHit(node, *local_position)) {
    for (std::size_t index = node.children.size(); index > 0; --index) {
      if (BuildPointerRouteImpl(ChildInPaintOrder(node, index - 1), position, route, purpose)) {
        return true;
      }
    }
  }

  if (within_node &&
      (HandlesPointer(node) || ExtensionHandlesPointer(node, *local_position) ||
       ExtensionHandlesHover(node, *local_position) || IsScrollContainer(node) || node.IsFocusable() ||
       (purpose == PointerRoutePurpose::Cursor && node.properties.pointer_cursor.has_value()) ||
       (purpose == PointerRoutePurpose::Hover && HasEventBinding<ViewEvents::Hover>(node.event_bindings)) ||
       node.kind == NodeKind::PlatformView)) {
    return true;
  }
  route.pop_back();
  return false;
}

enum class WindowHitTarget {
  None,
  Client,
  Drag,
};

WindowHitTarget HitTestWindowTarget(MountedNode& node, Point position) {
  if (!node.participates_in_layout || !node.pointer_events_enabled) {
    return WindowHitTarget::None;
  }
  const auto local_position = node.WindowToLocal(position);
  if (!local_position.has_value()) {
    return WindowHitTarget::None;
  }

  const bool within_node = node.bounds.Contains(*local_position);
  if (AllowsChildHit(node, *local_position)) {
    for (std::size_t index = node.children.size(); index > 0; --index) {
      const WindowHitTarget target = HitTestWindowTarget(ChildInPaintOrder(node, index - 1), position);
      if (target != WindowHitTarget::None) {
        return target;
      }
    }
  }

  if (!within_node) {
    return WindowHitTarget::None;
  }
  if (HandlesPointer(node) || ExtensionHandlesPointer(node, *local_position) || IsScrollContainer(node) ||
      node.IsFocusable()) {
    return WindowHitTarget::Client;
  }
  return node.properties.window_drag_region ? WindowHitTarget::Drag : WindowHitTarget::None;
}

} // namespace

namespace {

Size MeasureLayoutChild(void* state, huxerui::ViewNode& child, Constraints constraints) {
  auto& layout_state = *static_cast<LayoutContextState*>(state);
  return MeasureNode(static_cast<MountedNode&>(child), constraints, *layout_state.ui_window,
                     layout_state.safe_area, layout_state.title_bar_metrics);
}

std::size_t VirtualItemCount(void* state) {
  return static_cast<VirtualLayoutContextState*>(state)->session->ItemCount();
}

VirtualViewport CurrentVirtualViewport(void* state) {
  return static_cast<VirtualLayoutContextState*>(state)->viewport;
}

huxerui::ViewNode& ObtainVirtualItem(void* state, std::size_t index) {
  return static_cast<VirtualLayoutContextState*>(state)->session->Item(index);
}

Size MeasureVirtualItem(void* state, huxerui::ViewNode& item, Constraints constraints) {
  auto& layout_state = *static_cast<VirtualLayoutContextState*>(state)->layout_state;
  return MeasureNode(static_cast<MountedNode&>(item), constraints, *layout_state.ui_window,
                     layout_state.safe_area, layout_state.title_bar_metrics);
}

Size MeasureVirtualLayout(MountedNode& node, const Constraints& constraints, LayoutContextState& state) {
  if (!node.virtual_layout_descriptor || !node.virtual_layout_descriptor->measure || !node.virtual_state) {
    throw std::logic_error("HuxerUI virtual layout node has no measure function");
  }
  auto& scroll = *node.scroll_state;
  if (scroll.both_axes && (!constraints.HasBoundedWidth() || !constraints.HasBoundedHeight())) {
    throw std::logic_error("HuxerUI two-axis virtual layout requires bounded width and height");
  }

  VirtualMeasureSession session{*state.ui_window, node};
  VirtualLayoutContextState context_state{
      &session,
      &state,
      {
          {scroll.offset_x, scroll.offset_y},
          {
              constraints.HasBoundedWidth() ? constraints.max_width
                                           : std::max(constraints.min_width, node.measured_size.width),
              constraints.HasBoundedHeight() ? constraints.max_height
                                            : std::max(constraints.min_height, node.measured_size.height),
          },
      },
  };
  VirtualLayoutContext context = InternalAccess::CreateVirtualLayoutContext(
      &context_state, VirtualItemCount, CurrentVirtualViewport, ObtainVirtualItem, MeasureVirtualItem);

  const auto item_offset = [&](Axis axis, Size viewport) -> std::optional<float> {
    const auto* connection = scroll.Connection(axis);
    return connection ? connection->PendingItemOffset(axis == Axis::Vertical ? viewport.height : viewport.width)
                      : std::nullopt;
  };
  if (scroll.both_axes) {
    auto& viewport = context_state.viewport;
    viewport.offset.x = std::max(0.0F, item_offset(Axis::Horizontal, viewport.size).value_or(viewport.offset.x));
    viewport.offset.y = std::max(0.0F, item_offset(Axis::Vertical, viewport.size).value_or(viewport.offset.y));
  }

  constexpr int maximum_passes = 8;
  for (int pass = 0; pass < maximum_passes; ++pass) {
    VirtualLayoutResult result = node.virtual_layout_descriptor->measure(context, node, constraints);
    const Size size = constraints.Constrain(result.MeasuredSize());
    const Size content_size = result.ContentSize();
    const auto offsets = session.ScrollCorrections(result);
    auto& viewport = context_state.viewport;
    std::optional<Point> corrected_offset;
    if (scroll.both_axes) {
      const auto valid_size = [](Size value) {
        return std::isfinite(value.width) && std::isfinite(value.height) && value.width >= 0.0F && value.height >= 0.0F;
      };
      if (!valid_size(size) || !valid_size(content_size)) {
        throw std::logic_error("HuxerUI two-axis virtual layout must report finite non-negative sizes");
      }
      const auto resolve_offset = [&](Axis axis, float current, float viewport_extent, float content_extent) {
        const auto correction = offsets[ScrollAxisIndex(axis)];
        if (correction && !std::isfinite(*correction)) {
          throw std::logic_error("HuxerUI virtual scroll correction must be finite");
        }
        const float requested = item_offset(axis, size).value_or(correction.value_or(current));
        return std::clamp(requested, 0.0F, std::max(0.0F, content_extent - viewport_extent));
      };
      const VirtualViewport resolved{
          {
              resolve_offset(Axis::Horizontal, viewport.offset.x, size.width, content_size.width),
              resolve_offset(Axis::Vertical, viewport.offset.y, size.height, content_size.height),
          },
          size,
      };
      if (resolved != viewport) {
        // Keep all measurement attempts in one realization session and commit only the settled viewport.
        viewport = resolved;
        continue;
      }
      corrected_offset = resolved.offset;
    } else {
      ConfigureScrollAxes(node, result.ScrollAxis(), false);
      if (result.CorrectedScrollOffset()) {
        corrected_offset = Point{offsets[0].value_or(scroll.offset_x), offsets[1].value_or(scroll.offset_y)};
      }
    }

    auto collection_semantics = InternalAccess::CollectionSemantics(result);
    session.CommitRealization(result.Placements());
    node.virtual_state->realized_placements = result.Placements();
    node.virtual_state->collection_semantics = std::move(collection_semantics);
    node.virtual_state->pending_scroll_offset = corrected_offset;
    node.virtual_state->viewport_dirty = false;
    SetScrollContentSize(node, content_size);
    return size;
  }
  throw std::logic_error("HuxerUI two-axis virtual layout did not settle its viewport");
}

Size MeasureLabelContent(MountedNode& node, UiWindow& ui_window, const Constraints& constraints) {
  const LabelContentMetrics metrics = node.LayoutValueOr<LabelContentMetrics>({});
  const Size icon_size{
      std::max(0.0F, metrics.icon_size.width),
      std::max(0.0F, metrics.icon_size.height),
  };
  const bool show_label = metrics.show_label && !node.text.PlainText().empty();
  const float spacing = show_label && icon_size.width > 0.0F ? std::max(0.0F, metrics.icon_spacing) : 0.0F;
  const float maximum_text_width = constraints.HasBoundedWidth()
                                       ? std::max(0.0F, constraints.max_width - icon_size.width - spacing)
                                       : std::numeric_limits<float>::infinity();
  TextLayoutMetrics text;
  if (show_label) {
    text = ui_window.MeasureText(node.text, node.properties.text_style, maximum_text_width,
                                node.properties.text_layout_options);
  }
  node.layout_cache.insert_or_assign(typeid(LabelLayoutCache), LabelLayoutCache{text});
  return {
      icon_size.width + spacing + text.size.width,
      std::max(icon_size.height, text.size.height),
  };
}

void ClampScrollOffsetAndCompleteController(MountedNode& node) {
  for (Axis axis : node.scroll_state->Axes()) {
    float& offset = node.scroll_state->Offset(axis);
    const float clamped = std::clamp(offset, 0.0F, ResolveScrollMetrics(node, axis).maximum_offset);
    if (offset != clamped) {
      offset = clamped;
      node.foreground_paint_dirty = true;
    }
  }
  CompleteScrollController(node);
}

void StopScrollMotionTree(MountedNode& node) {
  StopScrollNodeMotion(node);
  for (auto& child : node.children) {
    StopScrollMotionTree(*child);
  }
}

void CommitLayoutParticipation(MountedNode& node, const std::vector<LayoutResult::Placement>& placements) {
  // A mounted or measured child is not necessarily part of the committed UI; the parent's placements are the single
  // source of truth for direct-child participation.
  const auto placed_in_order = [](const LayoutResult::Placement& placement, const auto& child) {
    return placement.child == child.get();
  };
  const bool all_placed_in_order =
      placements.size() == node.children.size() && std::ranges::equal(placements, node.children, placed_in_order);
  if (all_placed_in_order) {
    for (auto& child : node.children) {
      child->participates_in_layout = true;
    }
    return;
  }

  std::unordered_set<const huxerui::ViewNode*> placed;
  placed.reserve(placements.size());
  for (const LayoutResult::Placement& placement : placements) {
    placed.insert(placement.child);
  }
  for (auto& child : node.children) {
    const bool participating = placed.contains(child.get());
    if (child->participates_in_layout && !participating) {
      // Invisible momentum must not keep scheduling frames or resume later with stale velocity.
      StopScrollMotionTree(*child);
    }
    child->participates_in_layout = participating;
  }
}

} // namespace

namespace {

Size MeasureWithLayoutDescriptor(MountedNode& node, const Constraints& constraints, LayoutContextState& layout_state,
                                 EdgeInsets safe_area, const WindowTitleBarMetrics* title_bar_metrics) {
  LayoutContext context =
      InternalAccess::CreateLayoutContext(&layout_state, MeasureLayoutChild, safe_area, title_bar_metrics);
  LayoutResult result = node.layout_descriptor->measure(context, node, constraints);
  const Size measured_size = constraints.Constrain(result.MeasuredSize());
  CommitLayoutParticipation(node, result.Placements());
  node.layout_placements = result.Placements();
  return measured_size;
}

void LayoutPlacedChildren(MountedNode& node, Point content_origin) {
  for (const auto& placement : node.layout_placements) {
    LayoutNode(
        static_cast<MountedNode&>(*placement.child),
        {
            content_origin.x + placement.offset.x,
            content_origin.y + placement.offset.y,
        }
    );
  }
}

} // namespace

Constraints ResolveFrameConstraints(const Frame& frame, const Constraints& constraints) {
  const auto [min_width, max_width] =
      ResolveAxisConstraints(constraints.min_width, constraints.max_width, frame.width,
                             frame.min_width, frame.max_width);
  const auto [min_height, max_height] =
      ResolveAxisConstraints(constraints.min_height, constraints.max_height, frame.height,
                             frame.min_height, frame.max_height);
  return {
      min_width,
      max_width,
      min_height,
      max_height,
  };
}

Size MeasureNode(MountedNode& node, const Constraints& constraints, UiWindow& ui_window,
                 EdgeInsets safe_area, const WindowTitleBarMetrics* title_bar_metrics) {
  HUXERUI_PROFILE_SCOPE(profile_measure, Measure, node.identity);
  HUXERUI_PROFILE_COUNT(MeasureRequests);
  // An invalidated ancestor may revisit a clean child. The child's cached result remains valid only for the exact
  // parent constraints under which it was measured.
  const std::optional<WindowTitleBarMetrics> inherited_title_bar =
      title_bar_metrics == nullptr ? std::nullopt : std::optional<WindowTitleBarMetrics>{*title_bar_metrics};
  if (!node.measure_dirty && node.measured_constraints.has_value() && *node.measured_constraints == constraints &&
      node.measured_safe_area.has_value() && *node.measured_safe_area == safe_area &&
      node.measured_title_bar == inherited_title_bar) {
    HUXERUI_PROFILE_COUNT(MeasureCacheHits);
    HUXERUI_PROFILE_FLAG(profile_measure, ProfileFlag::MeasureCacheHit);
    return node.measured_size;
  }

  const EdgeInsets inherited_safe_area = safe_area;
  EdgeInsets consumed_safe_area;
  if (node.properties.safe_area_padding.has_value()) {
    const SafeAreaPadding& edges = *node.properties.safe_area_padding;
    consumed_safe_area = {
        edges.top ? safe_area.top : 0.0F,
        edges.right ? safe_area.right : 0.0F,
        edges.bottom ? safe_area.bottom : 0.0F,
        edges.left ? safe_area.left : 0.0F,
    };
    if (edges.top) {
      safe_area.top = 0.0F;
    }
    if (edges.right) {
      safe_area.right = 0.0F;
    }
    if (edges.bottom) {
      safe_area.bottom = 0.0F;
    }
    if (edges.left) {
      safe_area.left = 0.0F;
    }
  }
  node.resolved_padding = {
      node.properties.padding.top + consumed_safe_area.top,
      node.properties.padding.right + consumed_safe_area.right,
      node.properties.padding.bottom + consumed_safe_area.bottom,
      node.properties.padding.left + consumed_safe_area.left,
  };
  LayoutContextState layout_state{&ui_window, safe_area, title_bar_metrics};
  if (node.kind == NodeKind::ScrollView || node.kind == NodeKind::VirtualLayout) {
    const Axis fallback = node.kind == NodeKind::ScrollView ? Axis::Vertical : ScrollAxis(node);
    ConfigureScrollAxes(node, node.LayoutValueOr<detail::ScrollAxisBinding>(fallback),
                        node.LayoutValueOr<detail::ScrollBothAxesBinding>(false));
  }
  if (IsScrollContainer(node)) {
    PrepareScrollController(node);
  }
  const Constraints resolved_constraints = ResolveFrameConstraints(node.properties.frame, constraints);
  const Constraints content_constraints = resolved_constraints.Deflate(node.resolved_padding);
  Size content_size;

  switch (node.kind) {
  case NodeKind::Text:
    if (node.image_properties.HasValue() || node.layout_values.contains(typeid(LabelContentMetrics))) {
      content_size = MeasureLabelContent(node, ui_window, content_constraints);
    } else {
      content_size = MeasureTextParagraph(node, ui_window, content_constraints);
    }
    break;
  case NodeKind::Button:
    content_size =
        ui_window.MeasureText(node.text, node.properties.text_style, std::numeric_limits<float>::infinity(),
                             node.properties.text_layout_options).size;
    break;
  case NodeKind::IconButton:
    content_size = MeasureLabelContent(node, ui_window, content_constraints);
    break;
  case NodeKind::Chip:
    if (node.image_properties.HasValue()) {
      content_size = MeasureLabelContent(node, ui_window, content_constraints);
    } else {
      content_size =
          ui_window.MeasureText(node.text, node.properties.text_style, std::numeric_limits<float>::infinity(),
                               node.properties.text_layout_options).size;
    }
    break;
  case NodeKind::Divider: {
    const Axis axis = node.LayoutValueOr<detail::DividerAxisBinding>(Axis::Horizontal);
    const float thickness = std::max(0.0F, node.LayoutValueOr<detail::DividerThicknessBinding>(1.0F));
    if (axis == Axis::Horizontal) {
      content_size = {
          content_constraints.HasBoundedWidth() ? content_constraints.max_width : content_constraints.min_width,
          thickness,
      };
    } else {
      content_size = {
          thickness,
          content_constraints.HasBoundedHeight() ? content_constraints.max_height : content_constraints.min_height,
      };
    }
    break;
  }
  case NodeKind::TextField:
    content_size = MeasureTextField(node, ui_window, content_constraints);
    break;
  case NodeKind::Checkbox:
  case NodeKind::RadioButton:
  case NodeKind::Switch: {
    if (!node.text.PlainText().empty()) {
      const detail::ToggleLayoutMetrics metrics = node.LayoutValueOr<detail::ToggleLayoutMetrics>({});
      const float label_leading = detail::ToggleLabelLeading(metrics);
      const float maximum_label_width = content_constraints.HasBoundedWidth()
                                            ? std::max(0.0F, content_constraints.max_width - label_leading)
                                            : std::numeric_limits<float>::infinity();
      const Size label_size =
          ui_window.MeasureText(node.text, node.properties.text_style, maximum_label_width,
                               node.properties.text_layout_options).size;
      content_size = {
          std::max(metrics.interactive_size.width, label_leading + label_size.width),
          std::max(metrics.interactive_size.height, label_size.height),
      };
    }
    break;
  }
  case NodeKind::ProgressCircle:
  case NodeKind::ProgressBar:
  case NodeKind::Slider:
    break;
  case NodeKind::Image: {
    content_size = node.image_properties.IntrinsicSize();
    float scale = 1.0F;
    if (content_size.width > 0.0F && content_constraints.HasBoundedWidth()) {
      scale = std::min(scale, content_constraints.max_width / content_size.width);
    }
    if (content_size.height > 0.0F && content_constraints.HasBoundedHeight()) {
      scale = std::min(scale, content_constraints.max_height / content_size.height);
    }
    content_size.width *= scale;
    content_size.height *= scale;
    break;
  }
  case NodeKind::PlatformView:
    content_size = content_constraints.Constrain({});
    break;
  case NodeKind::Canvas:
  case NodeKind::Spacer:
    break;
  case NodeKind::Layout: {
    if (node.layout_descriptor == nullptr || node.layout_descriptor->measure == nullptr) {
      throw std::logic_error("HuxerUI layout node has no measure function");
    }
    content_size = MeasureWithLayoutDescriptor(node, content_constraints, layout_state, safe_area, title_bar_metrics);
    break;
  }
  case NodeKind::Scope:
  case NodeKind::Environment:
    content_size = MeasureScopeChild(node, content_constraints, layout_state);
    break;
  case NodeKind::SelectionArea:
    content_size = MeasureSelectionArea(node, ui_window, content_constraints, safe_area, title_bar_metrics);
    break;
  case NodeKind::ScrollView:
    if (node.layout_descriptor) {
      if (!node.layout_descriptor->measure) {
        throw std::logic_error("HuxerUI scroll layout node has no measure function");
      }
      content_size = MeasureWithLayoutDescriptor(node, content_constraints, layout_state, safe_area, title_bar_metrics);
    } else {
      content_size = MeasureScrollChild(node, content_constraints, layout_state);
    }
    break;
  case NodeKind::VirtualLayout:
    content_size = MeasureVirtualLayout(node, content_constraints, layout_state);
    break;
  }

  const Size measured{
      content_size.width + node.resolved_padding.Horizontal(),
      content_size.height + node.resolved_padding.Vertical(),
  };
  node.measured_size = resolved_constraints.Constrain(measured);
  node.measured_constraints = constraints;
  node.measured_safe_area = inherited_safe_area;
  node.measured_title_bar = inherited_title_bar;
  node.measure_dirty = false;
  node.layout_dirty = true;
  ++node.measure_revision;
  return node.measured_size;
}

void LayoutNode(MountedNode& node, Point offset) {
  HUXERUI_PROFILE_SCOPE(profile_place, Place, node.identity);
  const bool size_changed =
      node.bounds.width != node.measured_size.width || node.bounds.height != node.measured_size.height;
  const bool offset_changed = node.layout_offset != offset;
  if (size_changed) {
    node.content_paint_dirty = true;
    node.foreground_paint_dirty = true;
  }
  node.layout_offset = offset;
  node.bounds = {
      0.0F,
      0.0F,
      node.measured_size.width,
      node.measured_size.height,
  };
  if (node.kind == NodeKind::ScrollView) {
    ClampScrollOffsetAndCompleteController(node);
  }
  // Moving a node in its parent does not change descendant placements. The revision still exposes the new resolved
  // geometry to presentation, hit testing, and text-input queries.
  if (!node.layout_dirty && !size_changed) {
    if (offset_changed) {
      ++node.layout_revision;
    }
    return;
  }

  const Point content_origin{
      node.resolved_padding.left,
      node.resolved_padding.top,
  };
  switch (node.kind) {
  case NodeKind::Layout:
    LayoutPlacedChildren(node, content_origin);
    break;
  case NodeKind::Scope:
  case NodeKind::Environment:
  case NodeKind::SelectionArea:
    for (auto& child : node.children) {
      LayoutNode(*child, content_origin);
    }
    break;
  case NodeKind::ScrollView:
    if (node.layout_descriptor) {
      LayoutPlacedChildren(node, content_origin);
    } else {
      for (auto& child : node.children) {
        LayoutNode(*child, content_origin);
      }
    }
    break;
  case NodeKind::VirtualLayout: {
    // Virtual scrolling changes which items are realized and their viewport-relative placements, so its offset is
    // resolved during layout. A regular ScrollView retains its subtree and moves it with children_transform instead.
    auto& scroll = *node.scroll_state;
    if (const auto offset = std::exchange(node.virtual_state->pending_scroll_offset, std::nullopt)) {
      if (Point{scroll.offset_x, scroll.offset_y} != *offset) {
        node.foreground_paint_dirty = true;
        scroll.offset_x = offset->x;
        scroll.offset_y = offset->y;
      }
    }
    ClampScrollOffsetAndCompleteController(node);
    const Point scroll_offset{
        scroll.Supports(Axis::Horizontal) ? scroll.offset_x : 0.0F,
        scroll.Supports(Axis::Vertical) ? scroll.offset_y : 0.0F,
    };
    for (const auto& placement : node.virtual_state->realized_placements) {
      const Point item_offset = placement.offset - scroll_offset;
      LayoutNode(
          static_cast<MountedNode&>(*placement.item),
          {
              content_origin.x + item_offset.x,
              content_origin.y + item_offset.y,
          }
      );
    }
    break;
  }
  case NodeKind::Text:
  case NodeKind::Button:
  case NodeKind::IconButton:
  case NodeKind::Chip:
  case NodeKind::Divider:
  case NodeKind::TextField:
  case NodeKind::Checkbox:
  case NodeKind::RadioButton:
  case NodeKind::Switch:
  case NodeKind::ProgressCircle:
  case NodeKind::ProgressBar:
  case NodeKind::Slider:
  case NodeKind::Image:
  case NodeKind::PlatformView:
  case NodeKind::Canvas:
  case NodeKind::Spacer:
    break;
  }
  node.layout_dirty = false;
  ++node.layout_revision;
}

bool BuildPointerRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route) {
  route.clear();
  return BuildPointerRouteImpl(node, position, route, PointerRoutePurpose::Input);
}

bool BuildPointerCursorRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route) {
  route.clear();
  return BuildPointerRouteImpl(node, position, route, PointerRoutePurpose::Cursor);
}

bool BuildHoverRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route) {
  route.clear();
  return BuildPointerRouteImpl(node, position, route, PointerRoutePurpose::Hover);
}

MountedNode* HitTestPointer(MountedNode& node, Point position) {
  std::vector<MountedNode*> route;
  if (!BuildPointerRoute(node, position, route)) {
    return nullptr;
  }
  const auto target = std::find_if(route.rbegin(), route.rend(), [](const MountedNode* candidate) {
    return HandlesPointer(*candidate);
  });
  return target == route.rend() ? nullptr : *target;
}

bool HitTestWindowDragRegion(MountedNode& node, Point position) {
  return HitTestWindowTarget(node, position) == WindowHitTarget::Drag;
}

} // namespace huxerui::detail
