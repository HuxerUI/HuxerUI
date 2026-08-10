#include "internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <huxerui/theme.h>

#include "text_field_internal.h"
#include "selection_area_internal.h"

namespace huxerui::detail {

struct LayoutContextAccess {
  static LayoutContext Create(
      void* state,
      LayoutContext::MeasureFunction measure,
      EdgeInsets safe_area,
      const WindowTitleBarMetrics* title_bar_metrics
  ) {
    return LayoutContext{state, measure, safe_area, title_bar_metrics};
  }
};

struct VirtualLayoutContextAccess {
  static VirtualLayoutContext Create(
      void* state,
      VirtualLayoutContext::ItemCountFunction item_count,
      VirtualLayoutContext::ViewportFunction viewport,
      VirtualLayoutContext::ItemFunction item,
      VirtualLayoutContext::MeasureFunction measure
  ) {
    return VirtualLayoutContext{state, item_count, viewport, item, measure};
  }
};

struct VirtualLayoutResultAccess {
  static std::optional<VirtualCollectionSemantics> CollectionSemantics(const VirtualLayoutResult& result) {
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
};

bool IsScrollContainer(const MountedNode& node) noexcept {
  return static_cast<bool>(node.scroll_state);
}

Axis ScrollAxis(const MountedNode& node) noexcept {
  return node.scroll_state ? node.scroll_state->axis : Axis::Vertical;
}

Rect ScrollViewport(const MountedNode& node) noexcept {
  if (!node.scroll_state) {
    return {};
  }
  if (node.scroll_state->viewport_override.has_value()) {
    return *node.scroll_state->viewport_override;
  }
  return {
      node.resolved_padding.left,
      node.resolved_padding.top,
      std::max(0.0F, node.measured_size.width - node.resolved_padding.Horizontal()),
      std::max(0.0F, node.measured_size.height - node.resolved_padding.Vertical()),
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

std::pair<float, float> ResolveAxisConstraints(
    float parent_min,
    float parent_max,
    std::optional<float> preferred,
    std::optional<float> local_min,
    std::optional<float> local_max
) {
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
  const auto [min_width, max_width] = ResolveAxisConstraints(
      constraints.min_width,
      constraints.max_width,
      properties.frame.width,
      properties.frame.min_width,
      properties.frame.max_width
  );
  const auto [min_height, max_height] = ResolveAxisConstraints(
      constraints.min_height,
      constraints.max_height,
      properties.frame.height,
      properties.frame.min_height,
      properties.frame.max_height
  );
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
  return MeasureNode(
      *node.children.front(),
      constraints,
      *state.platform,
      *state.runtime,
      state.safe_area,
      state.title_bar_metrics
  );
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
  const Size child_size = MeasureNode(
      *node.children.front(),
      child_constraints,
      *state.platform,
      *state.runtime,
      state.safe_area,
      state.title_bar_metrics
  );
  node.scroll_state->content_width = child_size.width;
  node.scroll_state->content_height = child_size.height;
  return constraints.Constrain(child_size);
}

bool HandlesPointer(const MountedNode& node) {
  return static_cast<bool>(node.activation) || HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerDown>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerMove>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerUp>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerCancel>(node.event_bindings);
}

bool ExtensionHandlesPointer(MountedNode& node, Point position) {
  return std::any_of(node.extensions.begin(), node.extensions.end(), [&](const NodeExtensionEntry& entry) {
    return entry.extension && entry.extension->HitTest(node, position);
  });
}

bool ExtensionHandlesHover(MountedNode& node, Point position) {
  return std::any_of(node.extensions.begin(), node.extensions.end(), [&](const NodeExtensionEntry& entry) {
    return entry.extension && (node.enabled || entry.extension->HoverWhenDisabled()) &&
           entry.extension->HoverHitTest(node, position);
  });
}

bool BuildPointerRouteImpl(MountedNode& node, Point position, std::vector<MountedNode*>& route) {
  if (!node.pointer_events_enabled) {
    return false;
  }
  const auto local_position = node.presentation.resolved_transform.Inverse(position);
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
      if (BuildPointerRouteImpl(**child, position, route)) {
        return true;
      }
    }
  }

  if (within_node &&
      (HandlesPointer(node) || ExtensionHandlesPointer(node, *local_position) ||
       ExtensionHandlesHover(node, *local_position) || IsScrollContainer(node) || node.focusable)) {
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
  if (!node.pointer_events_enabled) {
    return WindowHitTarget::None;
  }
  const auto local_position = node.presentation.resolved_transform.Inverse(position);
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

Size MeasureLayoutChild(void* state, huxerui::MountedNode& child, Constraints constraints) {
  auto& layout_state = *static_cast<LayoutContextState*>(state);
  return MeasureNode(
      static_cast<MountedNode&>(child),
      constraints,
      *layout_state.platform,
      *layout_state.runtime,
      layout_state.safe_area,
      layout_state.title_bar_metrics
  );
}

std::size_t VirtualItemCount(void* state) {
  return static_cast<VirtualLayoutContextState*>(state)->session->ItemCount();
}

VirtualViewport CurrentVirtualViewport(void* state) {
  return static_cast<VirtualLayoutContextState*>(state)->viewport;
}

huxerui::MountedNode& ObtainVirtualItem(void* state, std::size_t index) {
  return static_cast<VirtualLayoutContextState*>(state)->session->Item(index);
}

Size MeasureVirtualItem(void* state, huxerui::MountedNode& item, Constraints constraints) {
  auto& layout_state = *static_cast<VirtualLayoutContextState*>(state)->layout_state;
  return MeasureNode(
      static_cast<MountedNode&>(item),
      constraints,
      *layout_state.platform,
      *layout_state.runtime,
      layout_state.safe_area,
      layout_state.title_bar_metrics
  );
}

Size MeasureLabelContent(
    MountedNode& node,
    PlatformAdapter& platform,
    const Constraints& constraints,
    TextLayoutOptions text_options
) {
  const LabelContentMetrics metrics = node.LayoutValueOr<LabelContentMetrics>({});
  const Size icon_size{
      std::max(0.0F, metrics.icon_size.width),
      std::max(0.0F, metrics.icon_size.height),
  };
  const bool show_label = metrics.show_label && !node.text.empty();
  const float spacing = show_label && icon_size.width > 0.0F ? std::max(0.0F, metrics.icon_spacing) : 0.0F;
  const float maximum_text_width = constraints.HasBoundedWidth()
                                       ? std::max(0.0F, constraints.max_width - icon_size.width - spacing)
                                       : std::numeric_limits<float>::infinity();
  TextLayoutMetrics text;
  if (show_label) {
    text = platform.MeasureText(node.text, node.properties.text_style, maximum_text_width, text_options);
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

} // namespace

Size MeasureNode(
    MountedNode& node,
    const Constraints& constraints,
    PlatformAdapter& platform,
    Runtime& runtime,
    EdgeInsets safe_area,
    const WindowTitleBarMetrics* title_bar_metrics
) {
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
    PrepareScrollController(node, runtime);
  }
  const Constraints resolved_constraints = ResolveConstraints(node.properties, constraints);
  const Constraints content_constraints = resolved_constraints.Deflate(node.resolved_padding);
  Size content_size;

  switch (node.kind) {
  case NodeKind::Text:
    if (node.image_properties.HasValue() || node.layout_values.contains(typeid(LabelContentMetrics))) {
      content_size = MeasureLabelContent(
          node,
          platform,
          content_constraints,
          TextLayoutOptions{.wrap = TextWrap::NoWrap}
      );
    } else {
      content_size = platform
                         .MeasureText(
                             node.text,
                             node.properties.text_style,
                             content_constraints.max_width,
                             node.properties.text_layout_options
                         )
                         .size;
    }
    break;
  case NodeKind::Button:
    content_size = platform
                       .MeasureText(
                           node.text,
                           node.properties.text_style,
                           std::numeric_limits<float>::infinity(),
                           TextLayoutOptions{.wrap = TextWrap::NoWrap}
                       )
                       .size;
    break;
  case NodeKind::IconButton:
    content_size = MeasureLabelContent(
        node,
        platform,
        content_constraints,
        TextLayoutOptions{.wrap = TextWrap::NoWrap}
    );
    break;
  case NodeKind::Chip:
    if (node.image_properties.HasValue()) {
      content_size = MeasureLabelContent(
          node,
          platform,
          content_constraints,
          TextLayoutOptions{.wrap = TextWrap::NoWrap}
      );
    } else {
      content_size = platform
                         .MeasureText(
                             node.text,
                             node.properties.text_style,
                             std::numeric_limits<float>::infinity(),
                             TextLayoutOptions{.wrap = TextWrap::NoWrap}
                         )
                         .size;
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
    if (!node.text.empty()) {
      const detail::ToggleLayoutMetrics metrics = node.LayoutValueOr<detail::ToggleLayoutMetrics>({});
      const float label_leading = detail::ToggleLabelLeading(metrics);
      const float maximum_label_width = content_constraints.HasBoundedWidth()
                                            ? std::max(0.0F, content_constraints.max_width - label_leading)
                                            : std::numeric_limits<float>::infinity();
      const Size label_size = platform
                                  .MeasureText(
                                      node.text,
                                      node.properties.text_style,
                                      maximum_label_width,
                                      node.properties.text_layout_options
                                  )
                                  .size;
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
  case NodeKind::Canvas:
  case NodeKind::Spacer:
    break;
  case NodeKind::Layout: {
    if (node.layout_descriptor == nullptr || node.layout_descriptor->measure == nullptr) {
      throw std::logic_error("HuxerUI layout node has no measure function");
    }
    LayoutContext context =
        LayoutContextAccess::Create(&layout_state, MeasureLayoutChild, safe_area, title_bar_metrics);
    LayoutResult result = node.layout_descriptor->measure(context, node, content_constraints);
    content_size = content_constraints.Constrain(result.MeasuredSize());
    node.layout_placements = result.Placements();
    break;
  }
  case NodeKind::Scope:
    content_size = MeasureScopeChild(node, content_constraints, layout_state);
    break;
  case NodeKind::SelectionArea:
    content_size = MeasureSelectionArea(node, platform, runtime, content_constraints, safe_area, title_bar_metrics);
    break;
  case NodeKind::ScrollView:
    content_size = MeasureScrollChild(node, content_constraints, layout_state);
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
    VirtualLayoutContext context = VirtualLayoutContextAccess::Create(
        &virtual_context_state,
        VirtualItemCount,
        CurrentVirtualViewport,
        ObtainVirtualItem,
        MeasureVirtualItem
    );
    VirtualLayoutResult result = node.virtual_layout_descriptor->measure(context, node, content_constraints);
    std::optional<VirtualCollectionSemantics> collection_semantics =
        VirtualLayoutResultAccess::CollectionSemantics(result);
    session.CommitRealization(result.Placements());
    node.virtual_state->realized_placements = result.Placements();
    node.virtual_state->collection_semantics = std::move(collection_semantics);
    node.scroll_state->axis = result.ScrollAxis();
    node.scroll_state->content_width = result.ContentSize().width;
    node.scroll_state->content_height = result.ContentSize().height;
    if (result.CorrectedScrollOffset().has_value()) {
      if (result.ScrollAxis() == Axis::Vertical) {
        node.scroll_state->offset_y = *result.CorrectedScrollOffset();
      } else {
        node.scroll_state->offset_x = *result.CorrectedScrollOffset();
      }
    }
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
  if (IsScrollContainer(node) && node.kind != NodeKind::ScrollView && node.kind != NodeKind::TextField) {
    ClampScrollOffsetAndCompleteController(node);
  }
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
    for (const auto& placement : node.layout_placements) {
      LayoutNode(
          static_cast<MountedNode&>(*placement.child),
          {
              content_origin.x + placement.offset.x,
              content_origin.y + placement.offset.y,
          }
      );
    }
    break;
  case NodeKind::Scope:
  case NodeKind::SelectionArea:
    for (auto& child : node.children) {
      LayoutNode(*child, content_origin);
    }
    break;
  case NodeKind::ScrollView:
    for (auto& child : node.children) {
      LayoutNode(*child, content_origin);
    }
    break;
  case NodeKind::VirtualLayout: {
    // Virtual scrolling changes which items are realized and their viewport-relative placements, so its offset is
    // resolved during layout. A regular ScrollView retains its subtree and moves it with children_transform instead.
    const bool vertical = ScrollAxis(node) == Axis::Vertical;
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
  case NodeKind::Canvas:
  case NodeKind::Spacer:
    break;
  }
  node.layout_dirty = false;
  ++node.layout_revision;
}

bool BuildPointerRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route) {
  route.clear();
  return BuildPointerRouteImpl(node, position, route);
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

std::optional<ScrollBarGeometry> ResolveScrollBarGeometry(const MountedNode& node) {
  if (!IsScrollContainer(node)) {
    return std::nullopt;
  }
  const auto binding = node.layout_values.find(typeid(ScrollBarBinding));
  if (binding == node.layout_values.end()) {
    return std::nullopt;
  }
  const auto* style = std::any_cast<ScrollBarStyle>(&binding->second.value);
  if (!style) {
    throw std::logic_error("HuxerUI scroll bar binding type mismatch");
  }

  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = node.ContentBounds();
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  const float scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  const float maximum_offset = std::max(0.0F, content_extent - viewport_extent);
  if (maximum_offset <= 0.0F || viewport_extent <= 0.0F) {
    return std::nullopt;
  }

  const float cross_extent = vertical ? viewport.width : viewport.height;
  const float available_cross = std::max(0.0F, cross_extent - style->margin * 2.0F);
  const float thickness = std::min(style->thickness, available_cross);
  const float track_extent = std::max(0.0F, viewport_extent - style->margin * 2.0F);
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
      std::max(style->minimum_thumb_extent, track_extent * viewport_extent / content_extent),
      0.0F,
      track_extent
  );
  const float thumb_travel = track_extent - thumb_extent;
  const float thumb_offset = thumb_travel * std::clamp(scroll_offset / maximum_offset, 0.0F, 1.0F);
  const Rect thumb = vertical ? Rect{track.x, track.y + thumb_offset, track.width, thumb_extent}
                              : Rect{track.x + thumb_offset, track.y, thumb_extent, track.height};
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

bool CanScrollNode(const MountedNode& node, float delta) {
  if (!node.enabled || !IsScrollContainer(node) || delta == 0.0F) {
    return false;
  }
  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  const float scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  const float max_offset = std::max(0.0F, content_extent - viewport_extent);
  return delta < 0.0F ? scroll_offset > 0.0F : scroll_offset < max_offset;
}

float ScrollNodeBy(MountedNode& node, float delta) {
  if (!node.enabled || !IsScrollContainer(node)) {
    return 0.0F;
  }
  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float content_extent = vertical ? node.scroll_state->content_height : node.scroll_state->content_width;
  float& scroll_offset = vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  const float max_offset = std::max(0.0F, content_extent - viewport_extent);
  const float previous = scroll_offset;
  scroll_offset = std::clamp(scroll_offset + delta, 0.0F, max_offset);
  if (scroll_offset != previous) {
    if (node.virtual_state) {
      node.virtual_state->viewport_dirty = true;
    }
    if (node.scroll_state->connection) {
      node.scroll_state->connection->PublishMetrics();
    }
  }
  return scroll_offset - previous;
}

bool ScrollNodeRectIntoView(MountedNode& node, Rect& rect) {
  if (!node.enabled || !IsScrollContainer(node)) {
    return false;
  }

  const std::optional<Rect> local_rect = InverseTransformBounds(node.presentation.resolved_transform, rect);
  if (!local_rect.has_value()) {
    return false;
  }

  const bool vertical = ScrollAxis(node) == Axis::Vertical;
  const Rect viewport = ScrollViewport(node);
  const float viewport_start = vertical ? viewport.y : viewport.x;
  const float viewport_extent = vertical ? viewport.height : viewport.width;
  const float viewport_end = viewport_start + viewport_extent;
  const float rect_start = vertical ? local_rect->y : local_rect->x;
  const float rect_extent = vertical ? local_rect->height : local_rect->width;
  const float rect_end = rect_start + rect_extent;

  float delta = 0.0F;
  if (rect_extent <= viewport_extent && rect_start < viewport_start) {
    delta = rect_start - viewport_start;
  } else if (rect_end > viewport_end) {
    delta = rect_end - viewport_end;
  }
  const float applied = ScrollNodeBy(node, delta);
  if (applied == 0.0F) {
    return false;
  }
  Rect moved = *local_rect;
  if (vertical) {
    moved.y -= applied;
  } else {
    moved.x -= applied;
  }
  rect = TransformBounds(node.presentation.resolved_transform, moved);
  return true;
}

const ScrollPhysics& ResolveScrollPhysics(const MountedNode& node) {
  const auto binding = node.layout_values.find(typeid(ScrollPhysics));
  if (binding == node.layout_values.end()) {
    static const ScrollPhysics default_physics;
    return default_physics;
  }
  const auto* physics = std::any_cast<ScrollPhysics>(&binding->second.value);
  if (!physics) {
    throw std::logic_error("HuxerUI scroll physics binding type mismatch");
  }
  return *physics;
}

void ScrollMotion::Stop() noexcept {
  velocity_ = 0.0F;
  previous_timestamp_.reset();
  momentum_active_ = false;
}

bool ScrollMotion::StartMomentum(MountedNode& node, float velocity) {
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!physics.fling_enabled || !std::isfinite(velocity) || std::abs(velocity) < physics.minimum_fling_velocity ||
      !CanScrollNode(node, velocity)) {
    Stop();
    return false;
  }
  velocity_ = std::clamp(velocity, -physics.maximum_fling_velocity, physics.maximum_fling_velocity);
  previous_timestamp_.reset();
  momentum_active_ = true;
  return true;
}

ScrollMotionFrameResult ScrollMotion::Advance(MountedNode& node, const FrameInfo& frame) {
  if (!momentum_active_) {
    return {};
  }
  const ScrollPhysics& physics = ResolveScrollPhysics(node);
  if (!physics.fling_enabled) {
    Stop();
    return {};
  }
  const float stop_velocity = physics.minimum_fling_velocity * 0.3F;
  if (!node.enabled || !IsScrollContainer(node)) {
    Stop();
    return {};
  }
  if (!previous_timestamp_.has_value()) {
    previous_timestamp_ = frame.timestamp;
    return {
        .needs_frame = true,
        .transfer_velocity = std::nullopt,
    };
  }

  const double elapsed = std::clamp(frame.timestamp - *previous_timestamp_, 0.0, 0.25);
  previous_timestamp_ = frame.timestamp;
  if (elapsed <= 0.0) {
    return {
        .needs_frame = true,
        .transfer_velocity = std::nullopt,
    };
  }

  const float decay = std::exp(-physics.deceleration_rate * static_cast<float>(elapsed));
  const float next_velocity = velocity_ * decay;
  const float delta = (velocity_ - next_velocity) / physics.deceleration_rate;
  const float consumed = ScrollNodeBy(node, delta);
  if (consumed != 0.0F) {
    for (NodeExtensionEntry& entry : node.extensions) {
      if (entry.extension) {
        entry.extension->OnScrollActivity(node);
      }
    }
  }
  if (std::abs(consumed - delta) > 0.001F) {
    const float transfer_velocity = velocity_ - physics.deceleration_rate * consumed;
    Stop();
    if (std::abs(transfer_velocity) >= stop_velocity) {
      return {
          .needs_frame = false,
          .transfer_velocity = transfer_velocity,
      };
    }
    return {};
  }
  if (std::abs(next_velocity) < stop_velocity) {
    Stop();
    return {};
  }
  velocity_ = next_velocity;
  return {
      .needs_frame = true,
      .transfer_velocity = std::nullopt,
  };
}

namespace {

bool AdvanceMountedNodeFrameImpl(
    MountedNode& node, const FrameInfo& frame, std::vector<MountedNode*>& scroll_ancestors
) {
  const bool scrollable = IsScrollContainer(node);
  if (scrollable) {
    scroll_ancestors.push_back(&node);
  }

  bool needs_frame = false;
  for (auto& child : node.children) {
    needs_frame = AdvanceMountedNodeFrameImpl(*child, frame, scroll_ancestors) || needs_frame;
  }

  const ScrollMotionFrameResult result =
      scrollable ? node.scroll_state->motion.Advance(node, frame) : ScrollMotionFrameResult{};
  needs_frame = needs_frame || result.needs_frame;
  if (scrollable && result.transfer_velocity.has_value()) {
    const Axis axis = ScrollAxis(node);
    for (std::size_t index = scroll_ancestors.size(); index > 1; --index) {
      MountedNode& ancestor = *scroll_ancestors[index - 2];
      if (ScrollAxis(ancestor) != axis || !CanScrollNode(ancestor, *result.transfer_velocity)) {
        continue;
      }
      if (ancestor.scroll_state->motion.StartMomentum(ancestor, *result.transfer_velocity)) {
        needs_frame = true;
        break;
      }
    }
  }

  if (scrollable) {
    scroll_ancestors.pop_back();
  }
  return needs_frame;
}

} // namespace

bool AdvanceMountedNodeFrame(MountedNode& node, const FrameInfo& frame) {
  std::vector<MountedNode*> scroll_ancestors;
  return AdvanceMountedNodeFrameImpl(node, frame, scroll_ancestors);
}

ScrollEventResult ApplyScrollEvent(MountedNode& node, const ScrollEvent& event) {
  std::vector<MountedNode*> route;
  if (!BuildPointerRoute(node, event.position, route)) {
    return {};
  }

  const Axis axis = std::abs(event.delta_x) > std::abs(event.delta_y) ? Axis::Horizontal : Axis::Vertical;
  float remaining = axis == Axis::Vertical ? event.delta_y : event.delta_x;
  ScrollEventResult result;

  for (auto candidate = route.rbegin(); candidate != route.rend(); ++candidate) {
    if (!(*candidate)->enabled || !IsScrollContainer(**candidate) || ScrollAxis(**candidate) != axis) {
      continue;
    }
    result.scroll_chain.push_back(*candidate);
    if (std::abs(remaining) < 0.001F) {
      continue;
    }
    const float consumed = ScrollNodeBy(**candidate, remaining);
    if (consumed != 0.0F) {
      remaining -= consumed;
    }
  }

  return result;
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

class VirtualListMetrics {
public:
  void Prepare(
      std::size_t item_count,
      Axis axis,
      float spacing,
      std::optional<float> fixed_extent,
      float estimated_extent,
      bool source_dirty
  ) {
    const bool axis_changed = initialized_ && axis_ != axis;
    const bool structure_changed = !initialized_ || item_count_ != item_count || axis_changed || spacing_ != spacing ||
                                   fixed_extent_ != fixed_extent || configured_estimate_ != estimated_extent ||
                                   source_dirty;
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

  bool Prepare(
      std::size_t item_count,
      std::size_t columns,
      float track_width,
      float row_spacing,
      std::optional<float> fixed_row_extent,
      float estimated_row_extent,
      const std::vector<std::size_t>& spans,
      bool source_dirty
  ) {
    const bool plan_changed = !initialized_ || item_count_ != item_count || columns_ != columns || spans_ != spans;
    if (plan_changed) {
      BuildPlan(item_count, columns, spans);
    }

    const bool geometry_changed = plan_changed || track_width_ != track_width || row_spacing_ != row_spacing ||
                                  fixed_row_extent_ != fixed_row_extent ||
                                  estimated_row_extent_ != estimated_row_extent || source_dirty;
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

float TotalSpacing(const MountedNode& node) {
  return node.ChildCount() < 2 ? 0.0F : node.Spacing() * static_cast<float>(node.ChildCount() - 1);
}

float SumMainSizes(const MountedNode& node, bool vertical) {
  float result = 0.0F;
  for (std::size_t index = 0; index < node.ChildCount(); ++index) {
    result += LayoutMainSize(node.ChildAt(index).LayoutSize(), vertical);
  }
  return result;
}

float MaxCrossSize(const MountedNode& node, bool vertical) {
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

AxisPlacement
ResolveAxisPlacement(MainAxisAlignment alignment, float spacing, std::size_t count, float used, float available) {
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

AxisPlacement ResolveAxisPlacement(const MountedNode& node, float available, bool vertical) {
  return ResolveAxisPlacement(
      node.MainAlignment(),
      node.Spacing(),
      node.ChildCount(),
      SumMainSizes(node, vertical) + TotalSpacing(node),
      available
  );
}

struct FlowLine {
  std::vector<MountedNode*> children;
  float natural_width = 0.0F;
  float height = 0.0F;
  float total_grow = 0.0F;
};

AxisPlacement ResolveFlowLinePlacement(const MountedNode& node, const FlowLine& line, float available) {
  const std::size_t count = line.children.size();
  float used = count < 2 ? 0.0F : node.Spacing() * static_cast<float>(count - 1);
  for (const MountedNode* child : line.children) {
    used += child->LayoutSize().width;
  }
  return ResolveAxisPlacement(node.MainAlignment(), node.Spacing(), count, used, available);
}

std::vector<FlowLine>
BuildFlowLines(LayoutContext& context, MountedNode& node, const Constraints& loose, float maximum_width) {
  std::vector<FlowLine> lines;
  FlowLine current;
  const bool bounded = std::isfinite(maximum_width);
  for (MountedNode& child : node.Children()) {
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

float ResolveFlowWidth(const MountedNode& node, const std::vector<FlowLine>& lines, const Constraints& constraints) {
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

void MeasureFlowLine(
    LayoutContext& context, const MountedNode& node, FlowLine& line, const Constraints& loose, float width
) {
  const float spacing = node.Spacing() * static_cast<float>(line.children.size() - 1);
  float fixed_width = 0.0F;
  for (const MountedNode* child : line.children) {
    if (child->GrowFactor() <= 0.0F) {
      fixed_width += child->LayoutSize().width;
    }
  }

  if (line.total_grow > 0.0F && std::isfinite(width)) {
    const float remaining = std::max(0.0F, width - fixed_width - spacing);
    for (MountedNode* child : line.children) {
      if (child->GrowFactor() <= 0.0F) {
        continue;
      }
      const float share = remaining * child->GrowFactor() / line.total_grow;
      static_cast<void>(context.Measure(*child, loose.TightWidth(share)));
    }
  }

  line.height = 0.0F;
  for (const MountedNode* child : line.children) {
    line.height = std::max(line.height, child->LayoutSize().height);
  }
  if (node.CrossAlignment() != CrossAxisAlignment::Stretch) {
    return;
  }
  for (MountedNode* child : line.children) {
    Constraints child_constraints = loose.TightHeight(line.height);
    if (child->GrowFactor() > 0.0F && line.total_grow > 0.0F && std::isfinite(width)) {
      child_constraints = child_constraints.TightWidth(child->LayoutSize().width);
    }
    static_cast<void>(context.Measure(*child, child_constraints));
  }
}

LayoutResult MeasureAxisLayout(LayoutContext& context, MountedNode& node, Constraints constraints, bool vertical) {
  const Constraints loose = constraints.Loose();
  float total_grow = 0.0F;

  for (MountedNode& child : node.Children()) {
    static_cast<void>(context.Measure(child, loose));
    total_grow += child.GrowFactor();
  }

  const bool stretch = node.CrossAlignment() == CrossAxisAlignment::Stretch;
  float target_cross = std::clamp(
      MaxCrossSize(node, vertical),
      MinimumCross(constraints, vertical),
      MaximumCross(constraints, vertical)
  );

  if (stretch) {
    for (MountedNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, TightCross(loose, vertical, target_cross)));
    }
  }

  const float spacing = TotalSpacing(node);
  float fixed_main = 0.0F;
  for (MountedNode& child : node.Children()) {
    if (child.GrowFactor() <= 0.0F) {
      fixed_main += LayoutMainSize(child.LayoutSize(), vertical);
    }
  }

  const float max_main = MaximumMain(constraints, vertical);
  float target_main = fixed_main + spacing;
  if (total_grow > 0.0F && std::isfinite(max_main)) {
    target_main = max_main;
    const float remaining = std::max(0.0F, target_main - fixed_main - spacing);
    for (MountedNode& child : node.Children()) {
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
    target_cross = std::clamp(
        MaxCrossSize(node, vertical),
        MinimumCross(constraints, vertical),
        MaximumCross(constraints, vertical)
    );
  }

  LayoutResult result;
  const AxisPlacement placement = ResolveAxisPlacement(node, target_main, vertical);
  float main = placement.leading;
  for (MountedNode& child : node.Children()) {
    const Size child_size = child.LayoutSize();
    const float cross = CrossOffset(target_cross, LayoutCrossSize(child_size, vertical), node.CrossAlignment());
    result.Place(child, vertical ? Point{cross, main} : Point{main, cross});
    main += LayoutMainSize(child_size, vertical) + placement.gap;
  }

  result.SetSize(MakeAxisSize(target_main, target_cross, vertical));
  return result;
}

} // namespace

VirtualLayoutResult VirtualList::Measure(VirtualLayoutContext& context, MountedNode& node, Constraints constraints) {
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

  auto& internal_node = static_cast<detail::MountedNode&>(node);
  auto& metrics = node.Cache<VirtualListMetrics>();
  const bool had_metrics = metrics.Initialized();
  const bool source_dirty = internal_node.virtual_state->source_dirty;
  const bool axis_changed = metrics.Initialized() && metrics.CurrentAxis() != axis;
  const float previous_scroll =
      metrics.Initialized() ? (metrics.CurrentAxis() == Axis::Vertical ? viewport.offset.y : viewport.offset.x) : 0.0F;
  const std::size_t previous_anchor = metrics.Initialized() ? metrics.IndexAt(previous_scroll) : 0;
  const float previous_anchor_delta = metrics.Initialized() ? previous_scroll - metrics.Offset(previous_anchor) : 0.0F;

  metrics.Prepare(item_count, axis, node.Spacing(), fixed_extent, configured_estimate, source_dirty);

  float scroll_offset = vertical ? viewport.offset.y : viewport.offset.x;
  std::size_t anchor = item_count == 0 ? 0 : std::min(previous_anchor, item_count - 1);
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
      MountedNode& item = context.Item(index);
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
    MountedNode& item = context.Item(index);
    const auto& key = static_cast<detail::MountedNode&>(item).key;
    metrics.RestoreKey(index, key);
    const Size item_size = context.Measure(item, item_constraints);
    metrics.Update(index, LayoutMainSize(item_size, vertical), key);
    cross_extent = std::max(cross_extent, LayoutCrossSize(item_size, vertical));
  }

  scroll_offset = item_count == 0 ? 0.0F : metrics.Offset(std::min(anchor, item_count - 1)) + anchor_delta;
  const float content_extent = metrics.ContentExtent();
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
    MountedNode& item = context.Item(index);
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
    MountedNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent
) {
  auto& metrics = node.Cache<VirtualListMetrics>();
  if (!metrics.Initialized() || index >= metrics.ItemCount()) {
    return std::nullopt;
  }
  return AlignedScrollOffset(metrics.Offset(index), metrics.Extent(index), viewport_extent, alignment);
}

VirtualLayoutResult VirtualGrid::Measure(VirtualLayoutContext& context, MountedNode& node, Constraints constraints) {
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

  auto& internal_node = static_cast<detail::MountedNode&>(node);
  auto& metrics = node.Cache<VirtualGridMetrics>();
  const bool had_layout = metrics.Initialized();
  const std::size_t previous_row = had_layout && metrics.RowCount() > 0 ? metrics.RowAt(viewport.offset.y) : 0;
  const std::size_t previous_anchor = had_layout && metrics.RowCount() > 0 ? metrics.FirstItem(previous_row) : 0;
  const float previous_anchor_delta =
      had_layout && metrics.RowCount() > 0 ? viewport.offset.y - metrics.Offset(previous_row) : 0.0F;
  const bool geometry_changed = metrics.Prepare(
      item_count,
      columns,
      track_width,
      row_spacing,
      fixed_row_extent,
      estimated_row_extent,
      spans,
      internal_node.virtual_state->source_dirty
  );

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
        MountedNode& item = context.Item(index);
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
      MountedNode& item = context.Item(index);
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
    MountedNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent
) {
  auto& metrics = node.Cache<VirtualGridMetrics>();
  if (!metrics.Initialized() || index >= metrics.ItemCount()) {
    return std::nullopt;
  }
  const std::size_t row = metrics.RowForItem(index);
  return AlignedScrollOffset(metrics.Offset(row), metrics.RowExtent(row), viewport_extent, alignment);
}

LayoutResult Column::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  return MeasureAxisLayout(context, node, constraints, true);
}

LayoutResult Row::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  return MeasureAxisLayout(context, node, constraints, false);
}

LayoutResult WindowTitleBar::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  const WindowTitleBarMetrics* metrics = context.TitleBarMetrics();
  const auto& internal_node = static_cast<const detail::MountedNode&>(node);
  const float requested_left = metrics == nullptr ? 0.0F : metrics->left_inset;
  const float requested_right = metrics == nullptr ? 0.0F : metrics->right_inset;
  const float maximum_width = constraints.max_width;
  const float left = std::isfinite(maximum_width) ? std::min(requested_left, maximum_width) : requested_left;
  const float right =
      std::isfinite(maximum_width) ? std::min(requested_right, std::max(0.0F, maximum_width - left)) : requested_right;
  const float reserved_width = left + right;

  Constraints content_constraints{
      std::max(0.0F, constraints.min_width - reserved_width),
      std::isfinite(maximum_width) ? std::max(0.0F, maximum_width - reserved_width) : maximum_width,
      std::max(
          constraints.min_height,
          metrics == nullptr ? 0.0F : std::max(0.0F, metrics->height - internal_node.resolved_padding.Vertical())
      ),
      constraints.max_height,
  };
  if (content_constraints.min_height > content_constraints.max_height) {
    content_constraints.min_height = content_constraints.max_height;
  }
  if (std::isfinite(content_constraints.max_width)) {
    content_constraints.min_width = content_constraints.max_width;
  }

  const LayoutResult content = MeasureAxisLayout(context, node, content_constraints, false);
  LayoutResult result;
  for (const LayoutResult::Placement& placement : content.Placements()) {
    result.Place(*placement.child, {placement.offset.x + left, placement.offset.y});
  }
  result.SetSize(constraints.Constrain({content.MeasuredSize().width + reserved_width, content.MeasuredSize().height}));
  return result;
}

LayoutResult Flow::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
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
    for (MountedNode* child : line.children) {
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

LayoutResult Stack::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  const Constraints loose = constraints.Loose();
  for (MountedNode& child : node.Children()) {
    static_cast<void>(context.Measure(child, loose));
  }

  float width = 0.0F;
  float height = 0.0F;
  for (MountedNode& child : node.Children()) {
    width = std::max(width, child.LayoutSize().width);
    height = std::max(height, child.LayoutSize().height);
  }
  width = constraints.ConstrainWidth(width);
  height = constraints.ConstrainHeight(height);

  const bool stretch_width = node.HorizontalAlignmentValue() == HorizontalAlignment::Stretch;
  const bool stretch_height = node.VerticalAlignmentValue() == VerticalAlignment::Stretch;
  if (stretch_width) {
    for (MountedNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, loose.TightWidth(width)));
    }
    height = 0.0F;
    for (MountedNode& child : node.Children()) {
      height = std::max(height, child.LayoutSize().height);
    }
    height = constraints.ConstrainHeight(height);
  }
  if (stretch_height) {
    for (MountedNode& child : node.Children()) {
      Constraints child_constraints = loose.TightHeight(height);
      if (stretch_width) {
        child_constraints = child_constraints.TightWidth(width);
      }
      static_cast<void>(context.Measure(child, child_constraints));
    }
  }

  LayoutResult result;
  for (MountedNode& child : node.Children()) {
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

} // namespace huxerui
