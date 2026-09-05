#include "runtime_internal.h"
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

namespace huxerui::detail {

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

namespace {

struct LayoutContextState {
  PlatformAdapter* platform;
  Runtime* runtime;
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

Constraints ResolveConstraints(const ViewProperties& properties, const Constraints& constraints) {
  const auto [min_width, max_width] =
      ResolveAxisConstraints(constraints.min_width, constraints.max_width, properties.frame.width,
                             properties.frame.min_width, properties.frame.max_width);
  const auto [min_height, max_height] =
      ResolveAxisConstraints(constraints.min_height, constraints.max_height, properties.frame.height,
                             properties.frame.min_height, properties.frame.max_height);
  return {
      min_width,
      max_width,
      min_height,
      max_height,
  };
}

Size MeasureScopeChild(MountedNode& node, const Constraints& constraints, LayoutContextState& state) {
  if (node.children.empty()) {
    return constraints.Constrain({});
  }
  return MeasureNode(*node.children.front(), constraints, *state.platform, *state.runtime, state.safe_area,
                     state.title_bar_metrics);
}

Size MeasureScrollChild(MountedNode& node, const Constraints& constraints, LayoutContextState& state) {
  if (node.children.empty()) {
    node.scroll_state->content_width = 0.0F;
    node.scroll_state->content_height = 0.0F;
    return constraints.Constrain({});
  }

  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const bool fill_viewport = node.LayoutValueOr<detail::ScrollFillViewport>(false);
  const float viewport_width = std::isfinite(constraints.max_width) ? constraints.max_width : constraints.min_width;
  const float viewport_height = std::isfinite(constraints.max_height) ? constraints.max_height : constraints.min_height;
  const Constraints child_constraints =
      vertical
          ? Constraints{
                constraints.min_width,
                constraints.max_width,
                fill_viewport ? viewport_height : 0.0F,
                std::numeric_limits<float>::infinity(),
            }
          : Constraints{
                fill_viewport ? viewport_width : 0.0F,
                std::numeric_limits<float>::infinity(),
                constraints.min_height,
                constraints.max_height,
            };
  const Size child_size = MeasureNode(*node.children.front(), child_constraints, *state.platform, *state.runtime,
                                      state.safe_area, state.title_bar_metrics);
  node.scroll_state->content_width = child_size.width;
  node.scroll_state->content_height = child_size.height;
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
  const Rect content = node.ContentBounds();
  const bool within_scroll_viewport = !IsScrollContainer(node) || content.Contains(*local_position);
  const bool within_child_clip =
      !node.properties.clip_children || RoundedRectContains(node.bounds, node.properties.corner_radii, *local_position);
  const bool can_hit_children = within_scroll_viewport && within_child_clip;
  if (can_hit_children) {
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
      if (BuildPointerRouteImpl(**child, position, route, purpose)) {
        return true;
      }
    }
  }

  if (within_node &&
      (HandlesPointer(node) || ExtensionHandlesPointer(node, *local_position) ||
       ExtensionHandlesHover(node, *local_position) || IsScrollContainer(node) || node.focusable ||
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
  const Rect content = node.ContentBounds();
  const bool within_scroll_viewport = !IsScrollContainer(node) || content.Contains(*local_position);
  const bool within_child_clip =
      !node.properties.clip_children || RoundedRectContains(node.bounds, node.properties.corner_radii, *local_position);
  if (within_scroll_viewport && within_child_clip) {
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
      const WindowHitTarget target = HitTestWindowTarget(**child, position);
      if (target != WindowHitTarget::None) {
        return target;
      }
    }
  }

  if (!within_node) {
    return WindowHitTarget::None;
  }
  if (HandlesPointer(node) || ExtensionHandlesPointer(node, *local_position) || IsScrollContainer(node) ||
      node.focusable) {
    return WindowHitTarget::Client;
  }
  return node.properties.window_drag_region ? WindowHitTarget::Drag : WindowHitTarget::None;
}

} // namespace

namespace {

Size MeasureLayoutChild(void* state, huxerui::ViewNode& child, Constraints constraints) {
  auto& layout_state = *static_cast<LayoutContextState*>(state);
  return MeasureNode(static_cast<MountedNode&>(child), constraints, *layout_state.platform, *layout_state.runtime,
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
  return MeasureNode(static_cast<MountedNode&>(item), constraints, *layout_state.platform, *layout_state.runtime,
                     layout_state.safe_area, layout_state.title_bar_metrics);
}

Size MeasureLabelContent(MountedNode& node, PlatformAdapter& platform, const Constraints& constraints) {
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
    text = platform.MeasureText(node.text, node.properties.text_style, maximum_text_width,
                                node.properties.text_layout_options);
  }
  node.layout_cache.insert_or_assign(typeid(LabelLayoutCache), LabelLayoutCache{text});
  return {
      icon_size.width + spacing + text.size.width,
      std::max(icon_size.height, text.size.height),
  };
}

void ClampScrollOffsetAndCompleteController(MountedNode& node) {
  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  float& scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  const float max_offset = std::max(0.0F, content_extent - viewport_extent);
  scroll_offset = std::clamp(scroll_offset, 0.0F, max_offset);
  CompleteScrollController(node);
}

void StopScrollMotionTree(MountedNode& node) {
  if (node.scroll_state) {
    node.scroll_state->motion.Stop(node);
  }
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

Size MeasureNode(MountedNode& node, const Constraints& constraints, PlatformAdapter& platform, Runtime& runtime,
                 EdgeInsets safe_area, const WindowTitleBarMetrics* title_bar_metrics) {
  // An invalidated ancestor may revisit a clean child. The child's cached result remains valid only for the exact
  // parent constraints under which it was measured.
  const std::optional<WindowTitleBarMetrics> inherited_title_bar =
      title_bar_metrics == nullptr ? std::nullopt : std::optional<WindowTitleBarMetrics>{*title_bar_metrics};
  if (!node.measure_dirty && node.measured_constraints.has_value() && *node.measured_constraints == constraints &&
      node.measured_safe_area.has_value() && *node.measured_safe_area == safe_area &&
      node.measured_title_bar == inherited_title_bar) {
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
  LayoutContextState layout_state{&platform, &runtime, safe_area, title_bar_metrics};
  if (node.kind == NodeKind::ScrollView) {
    node.scroll_state->axis = node.LayoutValueOr<detail::ScrollAxisBinding>(Axis::Vertical);
  }
  if (IsScrollContainer(node)) {
    PrepareScrollController(node);
  }
  const Constraints resolved_constraints = ResolveConstraints(node.properties, constraints);
  const Constraints content_constraints = resolved_constraints.Deflate(node.resolved_padding);
  Size content_size;

  switch (node.kind) {
  case NodeKind::Text:
    if (node.image_properties.HasValue() || node.layout_values.contains(typeid(LabelContentMetrics))) {
      content_size = MeasureLabelContent(node, platform, content_constraints);
    } else {
      content_size =
          platform.MeasureText(node.text, node.properties.text_style, content_constraints.max_width,
                               node.properties.text_layout_options).size;
    }
    break;
  case NodeKind::Button:
    content_size =
        platform.MeasureText(node.text, node.properties.text_style, std::numeric_limits<float>::infinity(),
                             node.properties.text_layout_options).size;
    break;
  case NodeKind::IconButton:
    content_size = MeasureLabelContent(node, platform, content_constraints);
    break;
  case NodeKind::Chip:
    if (node.image_properties.HasValue()) {
      content_size = MeasureLabelContent(node, platform, content_constraints);
    } else {
      content_size =
          platform.MeasureText(node.text, node.properties.text_style, std::numeric_limits<float>::infinity(),
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
    content_size = MeasureTextField(node, platform, content_constraints);
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
          platform.MeasureText(node.text, node.properties.text_style, maximum_label_width,
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
    content_size = MeasureSelectionArea(node, platform, runtime, content_constraints, safe_area, title_bar_metrics);
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
  case NodeKind::VirtualLayout: {
    if (node.virtual_layout_descriptor == nullptr || node.virtual_layout_descriptor->measure == nullptr ||
        !node.virtual_state) {
      throw std::logic_error("HuxerUI virtual layout node has no measure function");
    }
    const Size provisional_viewport{
        content_constraints.HasBoundedWidth() ? content_constraints.max_width
                                             : std::max(content_constraints.min_width, node.measured_size.width),
        content_constraints.HasBoundedHeight() ? content_constraints.max_height
                                              : std::max(content_constraints.min_height, node.measured_size.height),
    };
    VirtualMeasureSession session{runtime, node};
    VirtualLayoutContextState virtual_context_state{
        &session,
        &layout_state,
        {
            {node.scroll_state->offset_x, node.scroll_state->offset_y},
            provisional_viewport,
        },
    };
    VirtualLayoutContext context = InternalAccess::CreateVirtualLayoutContext(
        &virtual_context_state, VirtualItemCount, CurrentVirtualViewport, ObtainVirtualItem, MeasureVirtualItem);
    VirtualLayoutResult result = node.virtual_layout_descriptor->measure(context, node, content_constraints);
    std::optional<VirtualCollectionSemantics> collection_semantics = InternalAccess::CollectionSemantics(result);
    session.CommitRealization(result.Placements());
    node.virtual_state->realized_placements = result.Placements();
    node.virtual_state->collection_semantics = std::move(collection_semantics);
    node.virtual_state->pending_scroll_offset = result.CorrectedScrollOffset();
    node.scroll_state->axis = result.ScrollAxis();
    node.scroll_state->content_width = result.ContentSize().width;
    node.scroll_state->content_height = result.ContentSize().height;
    node.virtual_state->viewport_dirty = false;
    content_size = content_constraints.Constrain(result.MeasuredSize());
    break;
  }
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
    const bool vertical = ScrollAxis(node) == Axis::Vertical;
    if (node.virtual_state->pending_scroll_offset.has_value()) {
      float& committed_scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
      committed_scroll_offset = *node.virtual_state->pending_scroll_offset;
      node.virtual_state->pending_scroll_offset.reset();
    }
    ClampScrollOffsetAndCompleteController(node);
    const float scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
    for (const auto& placement : node.virtual_state->realized_placements) {
      const Point item_offset = vertical ? Point{placement.offset.x, placement.offset.y - scroll_offset}
                                        : Point{placement.offset.x - scroll_offset, placement.offset.y};
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
