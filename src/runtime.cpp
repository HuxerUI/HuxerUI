#include "internal.h"
#include "application_internal.h"
#include "external_texture_internal.h"
#include "resource_internal.h"
#include "task_internal.h"
#include "text_input_internal.h"
#include "window_internal.h"

#include <algorithm>
#include <stdexcept>

#include <huxerui/file.h>
#include <huxerui/http.h>
#include <huxerui/theme.h>

namespace huxerui::detail {

namespace {

int LayerLevelRank(LayerLevel level) noexcept {
  switch (level) {
  case LayerLevel::Presentation:
    return 0;
  case LayerLevel::Notification:
    return 1;
  case LayerLevel::System:
    return 2;
  }
  return 0;
}

bool LayerPaintsAbove(const LayerEntry& candidate, const LayerEntry& current) noexcept {
  const int candidate_level = LayerLevelRank(candidate.options.level);
  const int current_level = LayerLevelRank(current.options.level);
  return candidate_level != current_level ? candidate_level > current_level : candidate.sequence > current.sequence;
}

bool IsModifierKey(Key key) noexcept {
  return key == Key::Shift || key == Key::Control || key == Key::Alt || key == Key::Meta;
}

void ValidateViewportBreakpoints(const ViewportBreakpoints& breakpoints) {
  if (!std::isfinite(breakpoints.medium_width) || breakpoints.medium_width <= 0.0F ||
      !std::isfinite(breakpoints.expanded_width) || breakpoints.expanded_width <= breakpoints.medium_width) {
    throw std::invalid_argument("HuxerUI viewport breakpoints must be finite, positive, and increasing");
  }
}

ViewportClass ResolveViewportClass(float width, const ViewportBreakpoints& breakpoints) noexcept {
  if (width >= breakpoints.expanded_width) {
    return ViewportClass::Expanded;
  }
  if (width >= breakpoints.medium_width) {
    return ViewportClass::Medium;
  }
  return ViewportClass::Compact;
}

std::vector<LayerEntry> OrderedLayerEntries(const std::vector<LayerEntry>& entries) {
  std::vector<LayerEntry> ordered = entries;
  std::stable_sort(ordered.begin(), ordered.end(), [](const LayerEntry& left, const LayerEntry& right) {
    return LayerPaintsAbove(right, left);
  });
  return ordered;
}

class RuntimeRootLayout final : public huxerui::Layout<RuntimeRootLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, huxerui::MountedNode& node, Constraints constraints) {
    LayoutResult result;
    for (huxerui::MountedNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, constraints));
      result.Place(child, {});
    }
    result.SetSize(constraints.Constrain({
        constraints.max_width,
        constraints.max_height,
    }));
    return result;
  }
};

class ApplicationContentLayout final : public huxerui::Layout<ApplicationContentLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, huxerui::MountedNode& node, Constraints constraints) {
    LayoutResult result;
    if (node.ChildCount() > 1) {
      throw std::logic_error("HuxerUI application content container must not contain multiple roots");
    }
    if (node.ChildCount() == 1) {
      static_cast<void>(context.Measure(node.ChildAt(0), constraints));
      result.Place(node.ChildAt(0), {});
    }
    return result.SetSize(constraints.Constrain({constraints.max_width, constraints.max_height}));
  }
};

struct WindowBackplaneValue {
  using Value = bool;
};

class LayerStackLayout final : public huxerui::Layout<LayerStackLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, huxerui::MountedNode& node, Constraints constraints) {
    LayoutResult result;
    for (huxerui::MountedNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, constraints));
      result.Place(child, {});
    }
    result.SetSize(constraints.Constrain({
        constraints.max_width,
        constraints.max_height,
    }));
    return result;
  }
};

struct LayerTransition {
  std::shared_ptr<LayerTransitionState> state;

  static const ModifierDescriptor& Descriptor();
};

class LayerTransitionExtension final : public NodeExtension {
public:
  LayerTransitionExtension(huxerui::MountedNode& node, const LayerTransition& modifier) {
    Update(node, modifier);
  }

  void Update(huxerui::MountedNode& node, const LayerTransition& modifier) {
    static_cast<void>(node);
    if (state_ == modifier.state) {
      return;
    }
    state_ = modifier.state;
    initialized_ = false;
    completion_sent_ = false;
  }

  FrameResult OnFrame(huxerui::MountedNode& node, const FrameInfo& frame) override {
    if (!state_) {
      return {};
    }
    if (!initialized_) {
      if (state_->target_visible && state_->enter_on_mount) {
        opacity_.Set(state_->hidden_opacity);
        target_visible_ = false;
      } else {
        opacity_.Set(1.0F);
        target_visible_ = true;
      }
      initialized_ = true;
    }
    if (target_visible_ != state_->target_visible) {
      target_visible_ = state_->target_visible;
      opacity_.AnimateTo(
          target_visible_ ? 1.0F : state_->hidden_opacity,
          target_visible_ ? state_->enter : state_->exit
      );
      if (target_visible_) {
        completion_sent_ = false;
      }
    }

    auto& mounted = static_cast<MountedNode&>(node);
    const MotionAdvanceResult result = opacity_.Advance(frame);
    mounted.presentation.local_opacity *= opacity_.Value();
    if (!result.needs_frame && !result.wake_after.has_value() && target_visible_) {
      state_->enter_on_mount = false;
    }
    if (!result.needs_frame && !result.wake_after.has_value() && !target_visible_ && !completion_sent_) {
      completion_sent_ = true;
      if (state_->on_exit_complete) {
        state_->on_exit_complete();
      }
    }
    return {result.needs_frame, result.wake_after};
  }

private:
  std::shared_ptr<LayerTransitionState> state_;
  MotionController opacity_;
  bool initialized_ = false;
  bool target_visible_ = false;
  bool completion_sent_ = false;
};

const ModifierDescriptor& LayerTransition::Descriptor() {
  return ModifierDescriptorFor<LayerTransition, LayerTransitionExtension>();
}

bool IsLayerStack(const MountedNode& node) {
  return node.layout_descriptor != nullptr && node.layout_descriptor->type == typeid(LayerStackLayout);
}

Color CompositeOver(Color foreground, Color background) noexcept {
  const float foreground_alpha = std::clamp(foreground.alpha, 0.0F, 1.0F);
  const float background_alpha = std::clamp(background.alpha, 0.0F, 1.0F);
  const float alpha = foreground_alpha + background_alpha * (1.0F - foreground_alpha);
  if (alpha <= 0.0F) {
    return Color::Transparent();
  }
  return {
      (foreground.red * foreground_alpha + background.red * background_alpha * (1.0F - foreground_alpha)) / alpha,
      (foreground.green * foreground_alpha + background.green * background_alpha * (1.0F - foreground_alpha)) / alpha,
      (foreground.blue * foreground_alpha + background.blue * background_alpha * (1.0F - foreground_alpha)) / alpha,
      alpha,
  };
}

float LinearColorChannel(float value) noexcept {
  value = std::clamp(value, 0.0F, 1.0F);
  return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

SystemBarContentBrightness ResolveBrightness(SystemBarContentBrightness configured, Color background) noexcept {
  if (configured != SystemBarContentBrightness::Automatic) {
    return configured;
  }
  const float luminance = 0.2126F * LinearColorChannel(background.red) +
                          0.7152F * LinearColorChannel(background.green) +
                          0.0722F * LinearColorChannel(background.blue);
  return luminance > 0.45F ? SystemBarContentBrightness::Dark : SystemBarContentBrightness::Light;
}

Color ResolveCaptionForeground(Color background) noexcept {
  return ResolveBrightness(SystemBarContentBrightness::Automatic, background) == SystemBarContentBrightness::Dark
             ? Color::Rgb(32, 32, 32)
             : Color::Rgb(245, 245, 245);
}

void CollectWindowDragRegionBackground(
    const MountedNode& node, Color inherited_background, float title_bar_height, std::optional<Color>& candidate
) {
  if (!node.participates_in_layout) {
    return;
  }
  Color background = inherited_background;
  if (node.properties.background.has_value()) {
    if (const Color* color = std::get_if<Color>(&node.properties.background->Get())) {
      background = CompositeOver(*color, inherited_background);
    }
  }
  if (node.properties.window_drag_region && node.presentation.resolved_opacity > 0.001F) {
    const Rect bounds = TransformBounds(node.presentation.resolved_transform, node.bounds);
    if (bounds.y < title_bar_height && bounds.y + bounds.height > 0.0F) {
      candidate = background;
    }
  }
  for (const auto& child : node.children) {
    if (child) {
      CollectWindowDragRegionBackground(*child, background, title_bar_height, candidate);
    }
  }
}

void MarkContentPaintDirtyTree(MountedNode& node) {
  node.content_paint_dirty = true;
  for (auto& child : node.children) {
    if (child) {
      MarkContentPaintDirtyTree(*child);
    }
  }
}

const SystemBarsAppearance* FindSystemBarsAppearance(const MountedNode& node) {
  const std::any* value = FindEnvironmentValue(node.environment, typeid(SystemBarsAppearance));
  if (!value) {
    return nullptr;
  }
  const auto* appearance = std::any_cast<SystemBarsAppearance>(value);
  if (!appearance) {
    throw std::logic_error("HuxerUI system bars appearance environment value has an invalid type");
  }
  return appearance;
}

const SystemBarsAppearance* FindApplicationSystemBarsFallback(const MountedNode& node) {
  if (!node.participates_in_layout) {
    return nullptr;
  }
  if (const SystemBarsAppearance* appearance = FindSystemBarsAppearance(node)) {
    return appearance;
  }
  for (const auto& child : node.children) {
    if (child) {
      if (const SystemBarsAppearance* appearance = FindApplicationSystemBarsFallback(*child)) {
        return appearance;
      }
    }
  }
  return nullptr;
}

struct SystemBarCandidates {
  std::optional<SystemBarsAppearance> status;
  std::optional<SystemBarsAppearance> navigation;
};

void CollectSystemBarCandidates(
    const MountedNode& node, float status_boundary, float navigation_boundary, SystemBarCandidates& candidates
) {
  if (!node.participates_in_layout) {
    return;
  }
  if (node.presentation.resolved_opacity > 0.001F && node.properties.system_bars_appearance.has_value()) {
    const Rect bounds = TransformBounds(node.presentation.resolved_transform, node.bounds);
    const SystemBarsAppearance& appearance = *node.properties.system_bars_appearance;
    if (appearance.status_bar_background.alpha > 0.0F && bounds.y <= status_boundary + 0.5F &&
        bounds.y + bounds.height > status_boundary) {
      candidates.status = appearance;
    }
    if (appearance.navigation_bar_background.alpha > 0.0F && bounds.y < navigation_boundary &&
        bounds.y + bounds.height >= navigation_boundary - 0.5F) {
      candidates.navigation = appearance;
    }
  }
  for (const auto& child : node.children) {
    if (child) {
      CollectSystemBarCandidates(*child, status_boundary, navigation_boundary, candidates);
    }
  }
}

void ValidateWindowMetrics(const WindowMetrics& metrics) {
  const bool viewport_valid = std::isfinite(metrics.viewport.width) && metrics.viewport.width >= 0.0F &&
                              std::isfinite(metrics.viewport.height) && metrics.viewport.height >= 0.0F;
  const bool safe_area_valid = std::isfinite(metrics.safe_area.top) && metrics.safe_area.top >= 0.0F &&
                               std::isfinite(metrics.safe_area.right) && metrics.safe_area.right >= 0.0F &&
                               std::isfinite(metrics.safe_area.bottom) && metrics.safe_area.bottom >= 0.0F &&
                               std::isfinite(metrics.safe_area.left) && metrics.safe_area.left >= 0.0F;
  bool title_bar_valid = true;
  if (metrics.title_bar.has_value()) {
    const WindowTitleBarMetrics& title_bar = *metrics.title_bar;
    title_bar_valid =
        std::isfinite(title_bar.height) && title_bar.height >= 0.0F && title_bar.height <= metrics.viewport.height &&
        std::isfinite(title_bar.left_inset) && title_bar.left_inset >= 0.0F && std::isfinite(title_bar.right_inset) &&
        title_bar.right_inset >= 0.0F && title_bar.left_inset + title_bar.right_inset <= metrics.viewport.width;
  }
  if (!viewport_valid || !safe_area_valid || !title_bar_valid) {
    throw std::invalid_argument("HuxerUI window metrics must contain finite, non-negative values");
  }
}

bool HasWindowControlGeometry(const WindowMetrics& metrics) noexcept {
  return metrics.title_bar.has_value() && metrics.title_bar->height > 0.0F &&
         metrics.title_bar->right_inset > 0.0F;
}

bool IsWindowBackplane(const MountedNode& node) {
  return node.LayoutValueOr<WindowBackplaneValue>(false);
}

bool IsApplicationContent(const MountedNode& node) {
  return node.layout_descriptor != nullptr && node.layout_descriptor->type == typeid(ApplicationContentLayout);
}

MountedNode* FindLayerStack(MountedNode& root) {
  const auto found =
      std::ranges::find_if(root.children, [](const auto& child) { return child && IsLayerStack(*child); });
  return found == root.children.end() ? nullptr : found->get();
}

MountedNode* FindWindowBackplane(MountedNode& root) {
  const auto found =
      std::ranges::find_if(root.children, [](const auto& child) { return child && IsWindowBackplane(*child); });
  return found == root.children.end() ? nullptr : found->get();
}

MountedNode* FindApplicationContent(MountedNode& root) {
  const auto found =
      std::ranges::find_if(root.children, [](const auto& child) { return child && IsApplicationContent(*child); });
  return found == root.children.end() ? nullptr : found->get();
}

MountedNode* FindWindowControls(MountedNode& root) {
  const auto found =
      std::ranges::find_if(root.children, [](const auto& child) { return child && IsWindowControlsNode(*child); });
  return found == root.children.end() ? nullptr : found->get();
}

MountedNode* FindApplicationRoot(MountedNode& root) {
  MountedNode* content = FindApplicationContent(root);
  return content == nullptr || content->children.empty() ? nullptr : content->children.front().get();
}

MountedNode* FindLayerEntryNode(MountedNode& root, LayerId id) {
  MountedNode* layer_stack = FindLayerStack(root);
  if (!layer_stack) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(layer_stack->children, [id](const auto& child) {
    if (!child) {
      return false;
    }
    const auto* snapshot = child->template LayoutValue<LayerEntrySnapshotValue>();
    return snapshot != nullptr && snapshot->id == id;
  });
  return found == layer_stack->children.end() ? nullptr : found->get();
}

class LayerEntryLayout final : public huxerui::Layout<LayerEntryLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, huxerui::MountedNode& node, Constraints constraints) {
    LayoutResult result;
    if (node.ChildCount() == 0) {
      result.SetSize(constraints.Constrain({constraints.max_width, constraints.max_height}));
      return result;
    }

    huxerui::MountedNode& child = node.ChildAt(0);
    const auto* placement_value = node.LayoutValue<LayerPlacementValue>();
    const LayerPlacement fallback;
    const LayerPlacement& placement = placement_value && *placement_value ? **placement_value : fallback;
    const float viewport_width = constraints.max_width;
    const float viewport_height = constraints.max_height;
    const Constraints loose = constraints.Loose();
    const float horizontal_margin =
        std::min(std::max(0.0F, placement.viewport_margin), std::max(0.0F, viewport_width * 0.5F));
    const float vertical_margin =
        std::min(std::max(0.0F, placement.viewport_margin), std::max(0.0F, viewport_height * 0.5F));
    const Constraints inset_constraints{
        0.0F,
        std::max(0.0F, viewport_width - horizontal_margin * 2.0F),
        0.0F,
        std::max(0.0F, viewport_height - vertical_margin * 2.0F),
    };

    Size child_size;
    Point child_offset;
    switch (placement.kind) {
    case LayerPlacementKind::Natural:
      child_size = context.Measure(child, loose);
      break;
    case LayerPlacementKind::Fill:
      child_size = context.Measure(child, constraints);
      break;
    case LayerPlacementKind::Center:
      child_size = context.Measure(child, inset_constraints);
      child_offset = {
          (viewport_width - child_size.width) * 0.5F,
          (viewport_height - child_size.height) * 0.5F,
      };
      break;
    case LayerPlacementKind::TopCenter:
      child_size = context.Measure(child, inset_constraints);
      child_offset = {
          (viewport_width - child_size.width) * 0.5F,
          vertical_margin,
      };
      break;
    case LayerPlacementKind::BottomCenter:
      if (placement.fill_cross_axis) {
        const float available = std::max(0.0F, viewport_width - horizontal_margin * 2.0F);
        const float width = std::min(available, std::max(0.0F, placement.maximum_cross_axis_extent));
        child_size = context.Measure(
            child,
            Constraints{width, width, 0.0F, std::max(0.0F, viewport_height - vertical_margin * 2.0F)}
        );
      } else {
        child_size = context.Measure(child, inset_constraints);
      }
      child_offset = {
          (viewport_width - child_size.width) * 0.5F,
          viewport_height - vertical_margin - child_size.height,
      };
      break;
    case LayerPlacementKind::Anchored: {
      // Layer anchors are captured in host-view coordinates. Safe-area padding establishes this layout's local
      // coordinate space, so subtract only the padding that LayoutNode adds back after placement.
      child_size = context.Measure(child, inset_constraints);
      LayerAnchorSide side = placement.preferred_side;
      Rect anchor = placement.anchor;
      const EdgeInsets layer_padding = static_cast<detail::MountedNode&>(node).resolved_padding;
      anchor.x -= layer_padding.left;
      anchor.y -= layer_padding.top;
      const float anchor_right = anchor.x + anchor.width;
      const float anchor_bottom = anchor.y + anchor.height;
      const float gap = std::max(0.0F, placement.gap);
      const float below = viewport_height - vertical_margin - anchor_bottom - gap;
      const float above = anchor.y - vertical_margin - gap;
      const float right = viewport_width - horizontal_margin - anchor_right - gap;
      const float left = anchor.x - horizontal_margin - gap;
      if (side == LayerAnchorSide::Below && child_size.height > below && above > below) {
        side = LayerAnchorSide::Above;
      } else if (side == LayerAnchorSide::Above && child_size.height > above && below > above) {
        side = LayerAnchorSide::Below;
      } else if (side == LayerAnchorSide::Right && child_size.width > right && left > right) {
        side = LayerAnchorSide::Left;
      } else if (side == LayerAnchorSide::Left && child_size.width > left && right > left) {
        side = LayerAnchorSide::Right;
      }

      switch (side) {
      case LayerAnchorSide::Below:
        child_offset.y = anchor_bottom + gap;
        break;
      case LayerAnchorSide::Above:
        child_offset.y = anchor.y - child_size.height - gap;
        break;
      case LayerAnchorSide::Right:
        child_offset.x = anchor_right + gap;
        break;
      case LayerAnchorSide::Left:
        child_offset.x = anchor.x - child_size.width - gap;
        break;
      }
      if (side == LayerAnchorSide::Below || side == LayerAnchorSide::Above) {
        switch (placement.alignment) {
        case LayerAnchorAlignment::Start:
          child_offset.x = anchor.x;
          break;
        case LayerAnchorAlignment::Center:
          child_offset.x = anchor.x + (anchor.width - child_size.width) * 0.5F;
          break;
        case LayerAnchorAlignment::End:
          child_offset.x = anchor_right - child_size.width;
          break;
        }
      } else {
        switch (placement.alignment) {
        case LayerAnchorAlignment::Start:
          child_offset.y = anchor.y;
          break;
        case LayerAnchorAlignment::Center:
          child_offset.y = anchor.y + (anchor.height - child_size.height) * 0.5F;
          break;
        case LayerAnchorAlignment::End:
          child_offset.y = anchor_bottom - child_size.height;
          break;
        }
      }
      child_offset.x += placement.offset.x;
      child_offset.y += placement.offset.y;
      child_offset.x = std::clamp(
          child_offset.x,
          horizontal_margin,
          std::max(horizontal_margin, viewport_width - horizontal_margin - child_size.width)
      );
      child_offset.y = std::clamp(
          child_offset.y,
          vertical_margin,
          std::max(vertical_margin, viewport_height - vertical_margin - child_size.height)
      );
      break;
    }
    }
    result.Place(child, child_offset);
    result.SetSize(constraints.Constrain({viewport_width, viewport_height}));
    return result;
  }
};

bool ContainsStateSlots(const VirtualItemState& state) {
  if (state.state_slots.has_value() && !state.state_slots->slots.empty()) {
    return true;
  }
  return std::ranges::any_of(state.children, ContainsStateSlots);
}

bool IsCompatibleLayout(const LayoutDescriptor* left, const LayoutDescriptor* right) {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  return left->type == right->type;
}

bool IsCompatibleVirtualLayout(const VirtualLayoutDescriptor* left, const VirtualLayoutDescriptor* right) {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  return left->type == right->type;
}

bool IsCompatibleNode(const MountedNode& mounted, const ViewSpec& incoming) {
  return mounted.kind == incoming.kind && IsCompatibleLayout(mounted.layout_descriptor, incoming.layout_descriptor) &&
         IsCompatibleVirtualLayout(mounted.virtual_layout_descriptor, incoming.virtual_layout_descriptor) &&
         (incoming.kind != NodeKind::PlatformView ||
          (mounted.platform_view && incoming.platform_view &&
           mounted.platform_view->type == incoming.platform_view->type));
}

void ValidateEnvironmentDeclaration(const ViewSpec& declaration) {
  if (!declaration.local_environment) {
    throw std::logic_error("HuxerUI Environment node has no declared values");
  }
  if (declaration.children.size() != 1 || !declaration.children.front()) {
    throw std::logic_error("HuxerUI Environment node must have exactly one child");
  }
  if (declaration.scope_factory || !declaration.modifiers.empty() || !declaration.layout_values.empty() ||
      !declaration.event_bindings.empty() || declaration.activation || declaration.author_semantics.has_value() ||
      !declaration.pointer_events_enabled || !declaration.local_enabled || declaration.focusable ||
      declaration.trap_focus) {
    throw std::logic_error("HuxerUI Environment node cannot declare view behavior");
  }
}

bool IsCompatibleVirtualItemState(const MountedNode& mounted, const VirtualItemState& state) {
  return mounted.kind == state.kind && IsCompatibleLayout(mounted.layout_descriptor, state.layout_descriptor) &&
         IsCompatibleVirtualLayout(mounted.virtual_layout_descriptor, state.virtual_layout_descriptor);
}

bool LayoutValuesEquivalent(
    const std::unordered_map<std::type_index, ErasedLayoutValue>& left,
    const std::unordered_map<std::type_index, ErasedLayoutValue>& right
) {
  if (left.size() != right.size()) {
    return false;
  }
  return std::ranges::all_of(left, [&right](const auto& entry) {
    const auto found = right.find(entry.first);
    return found != right.end() && entry.second.EquivalentForReconciliation(found->second);
  });
}

bool ContentPaintInputsEqual(const MountedNode& mounted, const ViewSpec& incoming) {
  if (incoming.kind == NodeKind::Canvas) {
    return false;
  }
  return mounted.text == StringLiteral(incoming.text) && mounted.image_properties == incoming.image_properties &&
         PlatformViewPropertiesEqual(mounted.platform_view, incoming.platform_view) &&
         mounted.properties.ContentPaintEquals(incoming.properties);
}

bool ForegroundPaintInputsEqual(const MountedNode& mounted, const ViewSpec& incoming) {
  return mounted.properties.ForegroundPaintEquals(incoming.properties);
}

bool LayoutInputsEqual(const MountedNode& mounted, const ViewSpec& incoming) {
  return mounted.text == StringLiteral(incoming.text) &&
         mounted.image_properties.LayoutEquals(incoming.image_properties) &&
         mounted.properties.LayoutEquals(incoming.properties) &&
         LayoutValuesEquivalent(mounted.layout_values, incoming.layout_values);
}

bool ExtensionNodeInputsEqual(
    const MountedNode& mounted,
    const ViewSpec& incoming,
    const std::shared_ptr<const Environment>& environment
) {
  return mounted.text == StringLiteral(incoming.text) && mounted.image_properties == incoming.image_properties &&
         PlatformViewPropertiesEqual(mounted.platform_view, incoming.platform_view) &&
         mounted.properties == incoming.properties && mounted.component_semantics == incoming.component_semantics &&
         mounted.author_semantics == incoming.author_semantics &&
         LayoutValuesEquivalent(mounted.layout_values, incoming.layout_values) &&
         mounted.event_bindings == incoming.event_bindings && mounted.environment == environment &&
         mounted.pointer_events_enabled == incoming.pointer_events_enabled &&
         mounted.local_enabled == incoming.local_enabled && mounted.focusable == incoming.focusable &&
         mounted.trap_focus == incoming.trap_focus;
}

// Child structure, virtual sources, and node extensions reconcile separately because they carry mounted state.
void ApplyViewDeclaration(
    MountedNode& mounted,
    ViewSpec& compiled,
    std::shared_ptr<const Environment> environment
) {
  std::string compiled_text = StringLiteral(std::move(compiled.text));
  if (!PlatformViewPropertiesEqual(mounted.platform_view, compiled.platform_view)) {
    ++mounted.platform_view_properties_revision;
  }
  mounted.kind = compiled.kind;
  mounted.key = std::move(compiled.key);
  mounted.text = std::move(compiled_text);
  mounted.resolved_border = compiled.properties.border;
  mounted.resolved_corner_radii = compiled.properties.corner_radii;
  mounted.properties = std::move(compiled.properties);
  mounted.component_semantics = std::move(compiled.component_semantics);
  mounted.author_semantics = std::move(compiled.author_semantics);
  mounted.scope_factory = std::move(compiled.scope_factory);
  mounted.canvas_painter = std::move(compiled.canvas_painter);
  mounted.image_properties = std::move(compiled.image_properties);
  mounted.platform_view = std::move(compiled.platform_view);
  mounted.layout_descriptor = compiled.layout_descriptor;
  mounted.virtual_layout_descriptor = compiled.virtual_layout_descriptor;
  mounted.layout_values = std::move(compiled.layout_values);
  mounted.event_bindings = std::move(compiled.event_bindings);
  mounted.activation = std::move(compiled.activation);
  mounted.environment = std::move(environment);
  mounted.reduced_motion = ResolveThemeSpec(mounted.environment).motion.reduced_motion;
  mounted.pointer_events_enabled = compiled.pointer_events_enabled;
  mounted.local_enabled = compiled.local_enabled;
  mounted.focusable = compiled.focusable;
  mounted.trap_focus = compiled.trap_focus;
}

bool MarkLayoutDirtyPath(MountedNode& node, std::uint64_t identity) {
  if (node.identity == identity) {
    node.measure_dirty = true;
    return true;
  }
  for (auto& child : node.children) {
    if (child && MarkLayoutDirtyPath(*child, identity)) {
      node.measure_dirty = true;
      return true;
    }
  }
  return false;
}

bool PropagateVirtualLayoutInvalidation(MountedNode& node) {
  bool subtree_dirty = node.virtual_state && node.virtual_state->viewport_dirty;
  for (auto& child : node.children) {
    subtree_dirty = PropagateVirtualLayoutInvalidation(*child) || subtree_dirty;
  }
  if (subtree_dirty) {
    node.measure_dirty = true;
  }
  return subtree_dirty;
}

struct ModifierChanges {
  bool changed = false;
  bool layout_changed = false;
  bool structure_changed = false;
};

ModifierChanges ReconcileNodeExtensions(
    MountedNode& mounted, const std::vector<ModifierSpec>& modifiers, bool node_inputs_equal
) {
  std::vector<std::unique_ptr<NodeExtension>> created(modifiers.size());
  std::vector<NodeExtensionEntry> next;
  next.reserve(modifiers.size());
  ModifierChanges changes{
      mounted.extensions.size() != modifiers.size(),
      false,
      mounted.extensions.size() != modifiers.size(),
  };
  for (std::size_t index = 0; index < modifiers.size(); ++index) {
    const ModifierSpec& spec = modifiers[index];
    if (index < mounted.extensions.size() && mounted.extensions[index].descriptor == spec.descriptor) {
      if (!mounted.extensions[index].extension) {
        throw std::logic_error("HuxerUI node extension entry must not be empty");
      }
      continue;
    }
    changes.changed = true;
    changes.structure_changed = true;
    changes.layout_changed =
        changes.layout_changed || spec.descriptor->layout_affecting ||
        (index < mounted.extensions.size() && mounted.extensions[index].descriptor->layout_affecting);
    created[index] = spec.descriptor->create_extension(mounted, spec.value.get());
    if (!created[index]) {
      throw std::logic_error("HuxerUI modifier must create a node extension");
    }
  }

  for (std::size_t index = 0; index < modifiers.size(); ++index) {
    const ModifierSpec& spec = modifiers[index];
    if (index >= mounted.extensions.size() || mounted.extensions[index].descriptor != spec.descriptor ||
        spec.descriptor->update_extension == nullptr) {
      continue;
    }
    const bool equal = node_inputs_equal && spec.descriptor->equals != nullptr && mounted.extensions[index].value &&
                       spec.descriptor->equals(mounted.extensions[index].value.get(), spec.value.get());
    if (equal) {
      continue;
    }
    changes.changed = true;
    const bool layout_equal =
        !spec.descriptor->layout_affecting ||
        (spec.descriptor->layout_equals != nullptr && mounted.extensions[index].value &&
         spec.descriptor->layout_equals(mounted.extensions[index].value.get(), spec.value.get()));
    changes.layout_changed = changes.layout_changed || !layout_equal;
    spec.descriptor->update_extension(*mounted.extensions[index].extension, mounted, spec.value.get());
  }

  for (std::size_t index = modifiers.size(); index < mounted.extensions.size(); ++index) {
    if (mounted.extensions[index].descriptor != nullptr) {
      changes.layout_changed = changes.layout_changed || mounted.extensions[index].descriptor->layout_affecting;
    }
  }

  for (std::size_t index = 0; index < modifiers.size(); ++index) {
    const ModifierSpec& spec = modifiers[index];
    if (index < mounted.extensions.size() && mounted.extensions[index].descriptor == spec.descriptor) {
      next.push_back(std::move(mounted.extensions[index]));
      next.back().value = spec.value;
    } else {
      next.push_back({
          spec.descriptor,
          std::move(created[index]),
          spec.value,
      });
    }
  }
  mounted.extensions = std::move(next);
  return changes;
}

std::vector<NodeExtensionHandle> HitTestHoverExtensions(const std::vector<MountedNode*>& route, Point position) {
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    MountedNode& current = **node;
    const auto local_position = current.presentation.resolved_transform.Inverse(position);
    if (!local_position.has_value()) {
      continue;
    }
    std::vector<NodeExtensionHandle> matches;
    for (std::size_t index = 0; index < current.extensions.size(); ++index) {
      NodeExtensionEntry& entry = current.extensions[index];
      if (entry.extension && (current.interaction.enabled || entry.extension->HoverWhenDisabled()) &&
          entry.extension->HoverHitTest(current, *local_position)) {
        matches.push_back(NodeExtensionHandle{
            current.identity,
            index,
            entry.descriptor,
        });
      }
    }
    if (!matches.empty()) {
      return matches;
    }
  }
  return {};
}

void DispatchScrollActivity(MountedNode& node) {
  for (NodeExtensionEntry& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->OnScrollActivity(node);
    }
  }
}

void DispatchFocusChanged(MountedNode& node, bool focused) {
  for (NodeExtensionEntry& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->OnFocusChanged(node, focused);
    }
  }
  EmitEvent<ViewEvents::FocusChanged>(node.event_bindings, focused);
}

void DispatchKey(MountedNode& node, const KeyEvent& event) {
  for (NodeExtensionEntry& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->OnKey(node, event);
    }
  }
  if (event.type == KeyEventType::Down) {
    EmitEvent<ViewEvents::KeyDown>(node.event_bindings, event);
  } else {
    EmitEvent<ViewEvents::KeyUp>(node.event_bindings, event);
  }
}

bool RefreshExtensionPresence(MountedNode& node) {
  bool subtree_has_extensions = !node.extensions.empty();
  for (auto& child : node.children) {
    subtree_has_extensions = RefreshExtensionPresence(*child) || subtree_has_extensions;
  }
  node.subtree_has_extensions = subtree_has_extensions;
  return subtree_has_extensions;
}

void PrepareExtensionGeometry(MountedNode& node) {
  if (!node.participates_in_layout) {
    return;
  }
  if (!node.subtree_has_extensions) {
    if (node.indication_bounds_override.has_value()) {
      node.indication_bounds_override.reset();
      node.content_paint_dirty = true;
      node.foreground_paint_dirty = true;
    }
    return;
  }
  const std::optional<Rect> previous_indication_bounds = node.indication_bounds_override;
  node.indication_bounds_override.reset();
  for (NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    switch (entry.extension->PrepareGeometry(node)) {
    case NodeExtension::PaintInvalidation::None:
      break;
    case NodeExtension::PaintInvalidation::Content:
      node.content_paint_dirty = true;
      break;
    case NodeExtension::PaintInvalidation::Foreground:
      node.foreground_paint_dirty = true;
      break;
    case NodeExtension::PaintInvalidation::Both:
      node.content_paint_dirty = true;
      node.foreground_paint_dirty = true;
      break;
    }
  }
  if (node.indication_bounds_override != previous_indication_bounds) {
    node.content_paint_dirty = true;
    node.foreground_paint_dirty = true;
  }
  for (const std::unique_ptr<MountedNode>& child : node.children) {
    PrepareExtensionGeometry(*child);
  }
}

void ResolveEnabledTree(MountedNode& node, bool parent_enabled) {
  const bool enabled = parent_enabled && node.participates_in_layout && node.local_enabled;
  const bool applies_disabled_appearance = parent_enabled && node.participates_in_layout && !node.local_enabled;
  const bool disabled_appearance_changed = node.applies_disabled_appearance != applies_disabled_appearance;
  if (disabled_appearance_changed) {
    node.content_paint_dirty = true;
    node.foreground_paint_dirty = true;
    node.resolved_border = applies_disabled_appearance && node.properties.disabled_border.has_value()
                               ? node.properties.disabled_border
                               : node.properties.border;
    node.resolved_corner_radii = node.properties.corner_radii;
  }
  node.applies_disabled_appearance = applies_disabled_appearance;
  InteractionState interaction = node.interaction;
  interaction.enabled = enabled;
  if (!enabled) {
    node.active_press_count = 0;
    interaction.pressed = false;
    interaction.hovered = false;
  }
  UpdateInteraction(node, interaction);
  for (auto& child : node.children) {
    ResolveEnabledTree(*child, node.interaction.enabled);
  }
}

std::optional<bool> DeclaredEnabled(const MountedNode& node, std::uint64_t identity, bool parent_enabled = true) {
  const bool enabled = parent_enabled && node.participates_in_layout && node.local_enabled;
  if (node.identity == identity) {
    return enabled;
  }
  for (const auto& child : node.children) {
    if (const std::optional<bool> child_enabled = DeclaredEnabled(*child, identity, enabled);
        child_enabled.has_value()) {
      return child_enabled;
    }
  }
  return std::nullopt;
}

void ResolveFocusedFlags(MountedNode& node, const std::optional<std::uint64_t>& focused_identity, bool focus_visible) {
  const bool focused = focused_identity.has_value() && node.identity == *focused_identity;
  const bool resolved_focus_visible = focused && focus_visible;
  if (node.interaction.focused != focused || node.interaction.focus_visible != resolved_focus_visible) {
    node.foreground_paint_dirty = true;
    InteractionState interaction = node.interaction;
    interaction.focused = focused;
    interaction.focus_visible = resolved_focus_visible;
    UpdateInteraction(node, interaction);
  }
  for (auto& child : node.children) {
    ResolveFocusedFlags(*child, focused_identity, focus_visible);
  }
}

void CollectFocusableNodes(MountedNode& node, std::vector<MountedNode*>& nodes) {
  if (node.interaction.enabled && node.focusable) {
    nodes.push_back(&node);
  }
  for (auto& child : node.children) {
    CollectFocusableNodes(*child, nodes);
  }
}

MountedNode* FindTopmostFocusTrap(MountedNode& node) {
  if (!node.interaction.enabled) {
    return nullptr;
  }
  for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
    if (MountedNode* root = FindTopmostFocusTrap(**child)) {
      return root;
    }
  }
  return node.trap_focus ? &node : nullptr;
}

bool ContainsNodeIdentity(const MountedNode& node, std::uint64_t identity) {
  if (node.identity == identity) {
    return true;
  }
  return std::ranges::any_of(node.children, [identity](const auto& child) {
    return ContainsNodeIdentity(*child, identity);
  });
}

bool PointerSessionReferencesNode(const PointerSession& session, const MountedNode& root) {
  return std::ranges::any_of(session.route, [&root](std::uint64_t identity) {
    return ContainsNodeIdentity(root, identity);
  });
}

bool RouteBackTarget(MountedNode& node, const BackEvent& event, BackTarget& target, bool& already_dispatched) {
  if (!node.interaction.enabled) {
    return false;
  }
  for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
    if (RouteBackTarget(**child, event, target, already_dispatched)) {
      return true;
    }
  }
  if (HasEventBinding<ViewEvents::BackRequested>(node.event_bindings)) {
    target.kind = BackTargetKind::Event;
    target.node_identity = node.identity;
    return true;
  }
  for (std::size_t index = node.extensions.size(); index > 0; --index) {
    NodeExtensionEntry& entry = node.extensions[index - 1];
    if (entry.extension && entry.extension->OnBack(node, event)) {
      target.kind = BackTargetKind::Extension;
      target.extension = NodeExtensionHandle{node.identity, index - 1, entry.descriptor};
      already_dispatched = true;
      return true;
    }
  }
  return false;
}

bool IsActivatable(const MountedNode& node) {
  return static_cast<bool>(node.activation) || HasEventBinding<ViewEvents::Click>(node.event_bindings);
}

} // namespace

bool IsVirtualLayoutNode(const MountedNode& node) noexcept {
  return node.kind == NodeKind::VirtualLayout;
}

} // namespace huxerui::detail

namespace huxerui {

using namespace detail;

Runtime::Runtime(const Application& application, PlatformAdapter& platform, ApplicationActivation startup_activation) {
  ValidateViewportBreakpoints(application.options.viewport_breakpoints);
  const GestureSettings gesture_settings = platform.GestureDefaults();
  detail::ValidateGestureSettings(gesture_settings);
  const WindowOptions& window_options = application.options.window;
  if (!std::isfinite(window_options.initial_size.width) || window_options.initial_size.width <= 0.0F ||
      !std::isfinite(window_options.initial_size.height) || window_options.initial_size.height <= 0.0F) {
    throw std::invalid_argument("HuxerUI initial window size must be finite and positive");
  }
  if (window_options.content_mode != WindowContentMode::SafeArea &&
      window_options.content_mode != WindowContentMode::EdgeToEdge) {
    throw std::invalid_argument("HuxerUI window content mode is invalid");
  }
  if (window_options.chrome_mode != WindowChromeMode::System &&
      window_options.chrome_mode != WindowChromeMode::Custom) {
    throw std::invalid_argument("HuxerUI window chrome mode is invalid");
  }
  if (!std::isfinite(window_options.title_bar_height) || window_options.title_bar_height <= 0.0F) {
    throw std::invalid_argument("HuxerUI window title-bar height must be finite and positive");
  }
  auto window = std::make_shared<WindowState>(window_options);
  state_ = std::make_unique<State>(
      application.root_factory,
      &platform,
      std::make_shared<RecomposeScope>(*this, 1),
      LayerController(*this),
      application.options.viewport_breakpoints,
      std::move(window)
  );
  state_->application_service_ = std::make_shared<ApplicationService>(*this, std::move(startup_activation));
  state_->gesture_settings_ = gesture_settings;
  state_->task_delay_scheduler_ = detail::MakeTaskDelayScheduler(platform);
  state_->root_environment_ = std::make_shared<Environment>();
  state_->root_environment_->Set(detail::ViewportEnvironment{state_->viewport_class_});
  RootContext root{
      state_->layer_controller_,
      platform.Modules(),
      *state_->root_environment_,
      state_->root_service_types_,
      state_->root_services_,
  };
  state_->app_resources_ = std::make_shared<AppResources>(platform.Resources());
  const ResourceConfiguration resource_configuration = state_->app_resources_->Configuration();
  state_->root_environment_->Set(resource_configuration.locale);
  root.Provide(state_->app_resources_);
  root.Provide(state_->application_service_);
  root.Provide(std::make_shared<TextMeasurerService>(TextMeasurerService{&platform}));
  state_->window_service_ = std::make_shared<WindowService>(platform);
  root.Provide(state_->window_service_);
  state_->scene_transition_service_ = std::make_shared<SceneTransitionService>(*this);
  root.Provide(state_->scene_transition_service_);
  if (std::shared_ptr<FileSystem> file_system = platform.CreateFileSystem()) {
    root.Provide(std::move(file_system));
  }
  root.Provide(
      std::shared_ptr<FilePicker>(new FilePicker(platform.CreateFilePickerTransport(), platform.ui_thread_dispatcher_))
  );
  root.Provide(std::shared_ptr<HttpClient>(new HttpClient(platform.CreateHttpTransport())));
  InstallBuiltinPresentation(root);
  for (const RootHook& hook : application.options.root_hooks) {
    if (!hook) {
      throw std::invalid_argument("HuxerUI root hook must not be empty");
    }
    hook(root);
  }
  if (application.options.show_debug_overlay) {
    state_->debug_metrics_ = std::make_shared<DebugMetricsState>(platform);
    InstallDebugOverlay(root, state_->debug_metrics_);
  }
}

Runtime::~Runtime() {
  try {
    StopTextInputSession(TextInputEndReason::RuntimeDestroyed);
  } catch (...) {
  }
  DeactivateExternalTextures(state_->committed_scene_snapshot_, state_->platform_->external_texture_surface_);
  state_->pointer_sessions_.clear();
  state_->hovered_extensions_.clear();
  state_->layer_controller_.Disconnect();
  state_->application_service_->Disconnect();
  state_->window_service_->Disconnect();
  state_->scene_transition_service_->Disconnect();
  DiscardLifecycleCommits();
  state_->mounted_root_.reset();
  state_->root_scope_.reset();
  for (detail::LifecycleCleanup& cleanup : state_->retired_lifecycle_cleanups_) {
    cleanup.Run();
  }
  state_->retired_lifecycle_cleanups_.clear();
  CommitTaskScopes();
  state_->root_environment_.reset();
  for (auto service = state_->root_services_.rbegin(); service != state_->root_services_.rend(); ++service) {
    service->reset();
  }
  state_->root_services_.clear();
}

void Runtime::QueueLifecycleCommit(const std::shared_ptr<detail::RecomposeScope>& scope) {
  state_->lifecycle_commits_.push_back(scope);
}

void Runtime::RetireLifecycles(detail::RecomposeScope& scope) noexcept {
  for (auto key = scope.lifecycle_order_.rbegin(); key != scope.lifecycle_order_.rend(); ++key) {
    auto slot = scope.lifecycle_slots_.find(*key);
    if (slot == scope.lifecycle_slots_.end()) {
      continue;
    }
    try {
      state_->retired_lifecycle_cleanups_.push_back(std::move(slot->second.cleanup));
    } catch (...) {
      slot->second.cleanup.Run();
    }
  }
  scope.lifecycle_slots_.clear();
  scope.lifecycle_order_.clear();
}

void Runtime::CommitLifecycles() {
  std::vector<std::weak_ptr<detail::RecomposeScope>> commits = std::move(state_->lifecycle_commits_);
  state_->lifecycle_commits_.clear();

  try {
    for (const std::weak_ptr<detail::RecomposeScope>& scope : commits) {
      if (auto active = scope.lock()) {
        active->PrepareLifecycleCommit();
      }
    }
    for (detail::LifecycleCleanup& cleanup : state_->retired_lifecycle_cleanups_) {
      cleanup.Run();
    }
    state_->retired_lifecycle_cleanups_.clear();
    for (auto scope = commits.rbegin(); scope != commits.rend(); ++scope) {
      if (auto active = scope->lock()) {
        active->CommitLifecycleCleanups();
      }
    }
    for (const std::weak_ptr<detail::RecomposeScope>& scope : commits) {
      if (auto active = scope.lock()) {
        active->CommitLifecycleSetups();
      }
    }
  } catch (...) {
    for (const std::weak_ptr<detail::RecomposeScope>& scope : commits) {
      if (auto active = scope.lock()) {
        active->DiscardLifecycleCommit();
      }
    }
    throw;
  }
}

void Runtime::DiscardLifecycleCommits() noexcept {
  for (const std::weak_ptr<detail::RecomposeScope>& scope : state_->lifecycle_commits_) {
    if (auto active = scope.lock()) {
      active->DiscardLifecycleCommit();
    }
  }
  state_->lifecycle_commits_.clear();
}

std::shared_ptr<detail::TaskScopeState> Runtime::CreateTaskScope() {
  return detail::MakeTaskScopeState(state_->platform_->ui_thread_dispatcher_, state_->task_delay_scheduler_);
}

void Runtime::RetireTaskScope(std::shared_ptr<detail::TaskScopeState> scope) noexcept {
  if (!scope) {
    return;
  }
  try {
    state_->retired_task_scopes_.push_back(scope);
  } catch (...) {
    detail::CloseTaskScope(scope);
  }
}

void Runtime::CommitTaskScopes() noexcept {
  for (const std::shared_ptr<detail::TaskScopeState>& scope : state_->retired_task_scopes_) {
    detail::CloseTaskScope(scope);
  }
  state_->retired_task_scopes_.clear();
}

void Runtime::SetWindowMetrics(WindowMetrics metrics) {
  ValidateWindowMetrics(metrics);
  if (state_->window_->metrics == metrics) {
    return;
  }
  const bool window_controls_visibility_changed =
      HasWindowControlGeometry(state_->window_->metrics) != HasWindowControlGeometry(metrics);
  const bool previous_maximized =
      state_->window_->metrics.title_bar.has_value() && state_->window_->metrics.title_bar->maximized;
  const bool maximized = metrics.title_bar.has_value() && metrics.title_bar->maximized;
  const bool maximize_state_changed = previous_maximized != maximized;
  state_->window_->metrics = metrics;
  if (state_->mounted_root_) {
    state_->mounted_root_->measure_dirty = true;
    if ((window_controls_visibility_changed || maximize_state_changed) &&
        state_->window_->chrome_mode == WindowChromeMode::Custom) {
      ReconcileWindowControls();
    }
    if (detail::MountedNode* backplane = FindWindowBackplane(*state_->mounted_root_)) {
      backplane->content_paint_dirty = true;
    }
  }
  if (state_->text_selection_overlay_.state.visible) {
    state_->text_selection_overlay_.state.paint_dirty = true;
  }
  const ViewportClass viewport_class = ResolveViewportClass(metrics.viewport.width, state_->viewport_breakpoints_);
  if (viewport_class != state_->viewport_class_) {
    state_->viewport_class_ = viewport_class;
    static_cast<void>(state_->root_environment_->Update(detail::ViewportEnvironment{viewport_class}));
    return;
  }
  RequestFrame();
}

bool Runtime::IsWindowDragRegion(Point position) const {
  return state_->window_->metrics.title_bar.has_value() && state_->mounted_root_ &&
         detail::HitTestWindowDragRegion(*state_->mounted_root_, position);
}

std::optional<std::uint64_t> Runtime::HitTestPlatformView(Point position) const {
  if (!state_->mounted_root_) {
    return std::nullopt;
  }
  std::vector<detail::MountedNode*> route;
  if (!detail::BuildPointerRoute(*state_->mounted_root_, position, route) || route.empty()) {
    return std::nullopt;
  }
  const detail::MountedNode* target = route.back();
  return target->kind == detail::NodeKind::PlatformView ? std::optional{target->identity} : std::nullopt;
}

std::optional<std::uint64_t> Runtime::FocusedPlatformView() const {
  if (!state_->focused_node_identity_.has_value() || !state_->mounted_root_) {
    return std::nullopt;
  }
  const detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (focused == nullptr || focused->kind != detail::NodeKind::PlatformView) {
    return std::nullopt;
  }
  return state_->focused_node_identity_;
}

void Runtime::SynchronizePlatformViewFocus(std::optional<std::uint64_t> identity, bool focus_visible) {
  if (identity.has_value()) {
    if (!state_->mounted_root_) {
      return;
    }
    const detail::MountedNode* node = FindNode(*state_->mounted_root_, *identity);
    if (node == nullptr || node->kind != detail::NodeKind::PlatformView) {
      return;
    }
    SetFocusedNode(identity, focus_visible);
    return;
  }
  if (FocusedPlatformView().has_value()) {
    SetFocusedNode(std::nullopt, focus_visible);
  }
}

void Runtime::MoveFocusFromPlatformView(std::uint64_t identity, bool reverse) {
  if (FocusedPlatformView() != identity) {
    return;
  }
  MoveFocus(reverse);
}

bool Runtime::DispatchPlatformViewEvent(
    std::uint64_t identity, std::string_view name, const PlatformPayload& payload
) {
  if (!state_->mounted_root_) {
    return false;
  }
  detail::MountedNode* node = FindNode(*state_->mounted_root_, identity);
  if (node == nullptr || node->kind != detail::NodeKind::PlatformView || !node->platform_view ||
      !node->interaction.enabled) {
    return false;
  }
  const auto event = std::ranges::find(node->platform_view->events, name, &detail::PlatformEventDescriptor::name);
  if (event == node->platform_view->events.end() || !node->event_bindings.contains(event->key)) {
    return false;
  }
  try {
    detail::BindExternalTextures(payload, state_->platform_->external_texture_surface_);
    event->dispatch(payload, node->event_bindings);
  } catch (...) {
    return false;
  }
  return true;
}

// Requests raised while a frame is being built are retained for its FrameCommit instead of re-entering the platform
// scheduler. Requests raised outside a build notify the platform immediately.
void Runtime::RequestFrame() {
  const double now = state_->platform_->Now();
  if (!state_->frame_requested_ || state_->frame_request_deadline_ > now) {
    state_->frame_requested_ = true;
    state_->frame_request_deadline_ = now;
    if (!state_->building_frame_) {
      state_->platform_->RequestFrameAt(now);
    }
  }
}

void Runtime::RequestFrameAfter(double delay_seconds) {
  if (!std::isfinite(delay_seconds)) {
    return;
  }
  delay_seconds = std::max(0.0, delay_seconds);
  const double deadline = state_->platform_->Now() + delay_seconds;
  if (!state_->frame_requested_ || deadline < state_->frame_request_deadline_) {
    state_->frame_requested_ = true;
    state_->frame_request_deadline_ = deadline;
    if (!state_->building_frame_) {
      state_->platform_->RequestFrameAt(deadline);
    }
  }
}

void Runtime::NotifyScrollActivity(detail::MountedNode& node, ScrollActivitySource source) {
  DispatchScrollActivity(node);
  if (source == ScrollActivitySource::External && state_->text_input_session_.has_value() &&
      state_->text_input_session_->node_identity != node.identity) {
    // External scrolling may move a focused editor off screen; later editing explicitly requests caret reveal again.
    if (detail::MountedNode* text_input = FindNode(node, state_->text_input_session_->node_identity)) {
      DispatchScrollActivity(*text_input);
    }
  }
  if (state_->text_selection_overlay_.state.visible) {
    state_->text_selection_overlay_.state.paint_dirty = true;
  }
  RequestFrame();
}

const FrameCommit& Runtime::BuildFrame() {
  const double timestamp = state_->platform_->Now();
  const double delta_time = state_->previous_frame_timestamp_.has_value()
                                ? std::clamp(timestamp - *state_->previous_frame_timestamp_, 0.0, 0.25)
                                : 0.0;
  state_->building_frame_ = true;
  try {
    return BuildFrame({timestamp, delta_time});
  } catch (...) {
    DiscardLifecycleCommits();
    state_->building_frame_ = false;
    state_->frame_requested_ = false;
    throw;
  }
}

void Runtime::UpdateResourceConfiguration(ResourceConfiguration configuration) {
  if (state_->app_resources_->Configuration() == configuration) {
    return;
  }
  state_->app_resources_->UpdateConfiguration(configuration);
  static_cast<void>(state_->root_environment_->Update(configuration.locale));
  ReconcileWindowControls();
  if (state_->text_selection_overlay_.state.visible) {
    state_->text_selection_overlay_.state.paint_dirty = true;
  }
}

const FrameCommit& Runtime::BuildFrame(FrameInfo frame) {
  detail::DebugMetricsState* const debug_metrics = state_->debug_metrics_.get();
  const double build_started_at = debug_metrics != nullptr ? state_->platform_->Now() : 0.0;
  const auto record_debug_commit = [&] {
    if (debug_metrics == nullptr) {
      return;
    }
    debug_metrics->RecordCommit(
        std::max(0.0, state_->platform_->Now() - build_started_at),
        state_->frame_commit_.render_frame.damage,
        state_->window_->metrics.viewport
    );
  };
  if (!std::isfinite(frame.timestamp)) {
    frame.timestamp = state_->platform_->Now();
  }
  if (!std::isfinite(frame.delta_time)) {
    frame.delta_time = 0.0;
  }
  frame.delta_time = std::clamp(frame.delta_time, 0.0, 0.25);
  // Consume the scheduled frame before callbacks run so callbacks can retain a new request in this commit.
  state_->frame_requested_ = false;
  detail::AdvanceTaskDelays(state_->task_delay_scheduler_, frame.timestamp);
  state_->application_service_->DispatchPending();
  state_->previous_frame_timestamp_ = frame.timestamp;
  // Application and LayerStack composition are independent so transient presentation never executes the root factory.
  if (state_->application_dirty_) {
    ComposeApplication();
  }
  if (state_->mounted_root_) {
    RecomposeDirtyScopes(*state_->mounted_root_);
  }
  if (state_->layers_dirty_) {
    ComposeLayers();
  }

  if (!state_->mounted_root_ || state_->window_->metrics.viewport.width <= 0.0F ||
      state_->window_->metrics.viewport.height <= 0.0F) {
    RefreshInteractionTree();
    RefreshTextInputSession();
    BuildSemantics();
    state_->frame_commit_.render_frame.scene.root = nullptr;
    state_->frame_commit_.render_frame.damage = {};
    DeactivateExternalTextures(state_->committed_scene_snapshot_, state_->platform_->external_texture_surface_);
    state_->has_committed_scene_snapshot_ = false;
    ++state_->frame_commit_.render_frame.revision;
    CommitLifecycles();
    CommitTaskScopes();
    state_->frame_commit_.next_frame_deadline =
        state_->frame_requested_ ? std::optional{state_->frame_request_deadline_} : std::nullopt;
    record_debug_commit();
    state_->building_frame_ = false;
    return state_->frame_commit_;
  }

  bool needs_frame = false;
  if (state_->scroll_motion_active_) {
    state_->scroll_motion_active_ = detail::AdvanceMountedNodeFrame(*state_->mounted_root_, frame);
    needs_frame = state_->scroll_motion_active_;
  }
  const Constraints constraints{
      state_->window_->metrics.viewport.width,
      state_->window_->metrics.viewport.width,
      state_->window_->metrics.viewport.height,
      state_->window_->metrics.viewport.height,
  };
  PropagateVirtualLayoutInvalidation(*state_->mounted_root_);
  MeasureNode(
      *state_->mounted_root_,
      constraints,
      *state_->platform_,
      *this,
      state_->window_->metrics.safe_area,
      state_->window_->metrics.title_bar ? &*state_->window_->metrics.title_bar : nullptr
  );
  LayoutNode(*state_->mounted_root_, {0.0F, 0.0F});
  RefreshInteractionTree();

  std::optional<double> next_wakeup;
  UpdateNodeExtensions(*state_->mounted_root_, frame, needs_frame, next_wakeup, state_->extension_tree_dirty_);
  state_->extension_tree_dirty_ = false;
  ResolvePresentationTree(*state_->mounted_root_);

  // The first layout establishes resolved caret geometry. Revealing that caret can change ancestor scroll offsets and
  // virtual realization, so the incremental layout pipeline must settle those changes before geometry is published.
  if (BringTextInputIntoView()) {
    if (PropagateVirtualLayoutInvalidation(*state_->mounted_root_)) {
      MeasureNode(
          *state_->mounted_root_,
          constraints,
          *state_->platform_,
          *this,
          state_->window_->metrics.safe_area,
          state_->window_->metrics.title_bar ? &*state_->window_->metrics.title_bar : nullptr
      );
    }
    LayoutNode(*state_->mounted_root_, {0.0F, 0.0F});
    ResolvePresentationTree(*state_->mounted_root_);
  }
  // Text input clients prepare node-local geometry after the final layout, then the runtime converts it to host-view
  // coordinates while synchronizing the platform IME session.
  PrepareExtensionGeometry(*state_->mounted_root_);
  // Anchors can be nested inside other anchored layers. Settle the bounded dependency chain in this commit so a child
  // presentation does not retain geometry from its parent's previous placement.
  const detail::MountedNode* const committed_layer_stack = FindLayerStack(*state_->mounted_root_);
  const std::size_t maximum_geometry_layout_passes =
      (committed_layer_stack == nullptr ? 0 : committed_layer_stack->children.size()) + 1;
  std::size_t geometry_layout_passes = 0;
  while (state_->mounted_root_->measure_dirty && geometry_layout_passes < maximum_geometry_layout_passes) {
    ++geometry_layout_passes;
    PropagateVirtualLayoutInvalidation(*state_->mounted_root_);
    MeasureNode(
        *state_->mounted_root_,
        constraints,
        *state_->platform_,
        *this,
        state_->window_->metrics.safe_area,
        state_->window_->metrics.title_bar ? &*state_->window_->metrics.title_bar : nullptr
    );
    LayoutNode(*state_->mounted_root_, {0.0F, 0.0F});
    ResolvePresentationTree(*state_->mounted_root_);
    PrepareExtensionGeometry(*state_->mounted_root_);
  }
  if (state_->mounted_root_->measure_dirty) {
    RequestFrame();
  }
  RefreshTextInputSession();
  AdvancePointerRecognition(frame.timestamp);
  AdvanceDragDrop(frame);
  // A completed long press can focus a client and change its selection. Resolve it before building the shared overlay
  // so the handles and editing toolbar use the resulting selection geometry in this commit.
  AdvanceTextSelectionLongPress(frame.timestamp);
  BuildSemantics();

  AdvanceTextSelectionOverlay(frame);
  PaintTextSelectionOverlay();
  CommitWindowAppearance();
  UpdateRenderScene(
      *state_->mounted_root_,
      state_->mounted_root_->bounds,
      &state_->text_selection_overlay_.render_node
  );
  const RenderNode* scene_root = &state_->mounted_root_->render_node;
  const bool scene_transition_was_active = state_->scene_transition_.has_value();
  if (state_->scene_transition_.has_value()) {
    ActiveSceneTransition& transition = *state_->scene_transition_;
    if (transition.viewport != state_->window_->metrics.viewport) {
      state_->scene_transition_.reset();
    } else {
      const MotionAdvanceResult result = transition.progress.Advance(frame);
      needs_frame = needs_frame || result.needs_frame;
      if (result.wake_after.has_value() && (!next_wakeup.has_value() || *result.wake_after < *next_wakeup)) {
        next_wakeup = result.wake_after;
      }
      if (!transition.progress.IsRunning()) {
        state_->scene_transition_.reset();
      } else {
        scene_root = ComposeSceneTransition(transition, scene_root, transition.progress.Value());
      }
    }
  }
  state_->frame_commit_.render_frame.scene.root = scene_root;
  state_->frame_commit_.render_frame.damage = ComputeDamageRegion(
      state_->frame_commit_.render_frame.scene.root,
      state_->window_->metrics.viewport,
      state_->committed_scene_snapshot_,
      state_->committed_viewport_,
      state_->has_committed_scene_snapshot_,
      state_->platform_->external_texture_surface_
  );
  if (scene_transition_was_active) {
    state_->frame_commit_.render_frame.damage.full = true;
    state_->frame_commit_.render_frame.damage.rects.clear();
  }
  ++state_->frame_commit_.render_frame.revision;
  if (needs_frame) {
    RequestFrame();
  } else if (next_wakeup.has_value()) {
    RequestFrameAfter(*next_wakeup);
  }
  CommitLifecycles();
  CommitTaskScopes();
  state_->frame_commit_.next_frame_deadline =
      state_->frame_requested_ ? std::optional{state_->frame_request_deadline_} : std::nullopt;
  record_debug_commit();
  state_->building_frame_ = false;
  return state_->frame_commit_;
}

void Runtime::HandleApplicationActivation(ApplicationActivation activation) {
  state_->application_service_->Enqueue(std::move(activation));
}

void Runtime::UpdateApplicationLifecycleState(ApplicationLifecycleState lifecycle_state) {
  state_->application_service_->UpdateLifecycleState(lifecycle_state);
}

const detail::MountedNode* Runtime::RootNode() const noexcept {
  if (!state_->mounted_root_) {
    return nullptr;
  }
  return FindApplicationRoot(*state_->mounted_root_);
}

void Runtime::UpdateHoveredExtensions(Point position) {
  std::vector<detail::MountedNode*> route;
  std::vector<NodeExtensionHandle> next_hovered;
  if (state_->mounted_root_ && BuildPointerRoute(*state_->mounted_root_, position, route)) {
    next_hovered = HitTestHoverExtensions(route, position);
  }

  if (state_->hovered_extensions_ == next_hovered) {
    return;
  }
  if (state_->mounted_root_) {
    for (const detail::NodeExtensionHandle& previous : state_->hovered_extensions_) {
      if (std::ranges::find(next_hovered, previous) != next_hovered.end()) {
        continue;
      }
      if (NodeExtension* extension = FindExtension(*state_->mounted_root_, previous)) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, previous.node_identity)) {
          extension->OnHoverChanged(*node, false);
        }
      }
      const bool node_remains_hovered = std::ranges::any_of(next_hovered, [&](const NodeExtensionHandle& handle) {
        return handle.node_identity == previous.node_identity;
      });
      if (!node_remains_hovered) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, previous.node_identity)) {
          InteractionState interaction = node->interaction;
          interaction.hovered = false;
          UpdateInteraction(*node, interaction);
        }
      }
    }
  }
  if (state_->mounted_root_) {
    for (const detail::NodeExtensionHandle& next : next_hovered) {
      if (std::ranges::find(state_->hovered_extensions_, next) != state_->hovered_extensions_.end()) {
        continue;
      }
      if (NodeExtension* extension = FindExtension(*state_->mounted_root_, next)) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, next.node_identity)) {
          extension->OnHoverChanged(*node, true);
        }
      }
      const bool node_was_hovered =
          std::ranges::any_of(state_->hovered_extensions_, [&](const NodeExtensionHandle& handle) {
            return handle.node_identity == next.node_identity;
          });
      if (!node_was_hovered) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, next.node_identity)) {
          InteractionState interaction = node->interaction;
          interaction.hovered = true;
          UpdateInteraction(*node, interaction);
        }
      }
    }
  }
  state_->hovered_extensions_ = std::move(next_hovered);
  RequestFrame();
}

void Runtime::RefreshInteractionTree() {
  if (!state_->mounted_root_) {
    state_->focused_node_identity_.reset();
    state_->focus_visible_ = false;
    state_->keyboard_activation_identity_.reset();
    state_->keyboard_press_id_.reset();
    state_->hovered_extensions_.clear();
    return;
  }

  std::vector<PointerEvent> cancellations;
  for (const auto& [pointer_id, session] : state_->pointer_sessions_) {
    const bool references_inactive = std::ranges::any_of(session.route, [this](std::uint64_t identity) {
      return !DeclaredEnabled(*state_->mounted_root_, identity).value_or(false);
    });
    const bool recognition_removed =
        std::ranges::any_of(session.recognitions, [this](const PointerRecognition& recognition) {
          if (!recognition.started) {
            return false;
          }
          if (const auto* extension = std::get_if<ExtensionRecognitionState>(&recognition.state)) {
            return FindExtension(*state_->mounted_root_, extension->extension) == nullptr;
          }
          if (const auto* gesture = std::get_if<GestureRecognitionState>(&recognition.state)) {
            return FindExtension(*state_->mounted_root_, gesture->extension) == nullptr;
          }
          if (const auto* source = std::get_if<DragSourceRecognitionState>(&recognition.state)) {
            return FindExtension(*state_->mounted_root_, source->extension) == nullptr;
          }
          if (const auto* tap = std::get_if<TapRecognitionState>(&recognition.state)) {
            return std::ranges::any_of(
                tap->consumers,
                [this](const GestureRecognitionState& consumer) {
                  return FindExtension(*state_->mounted_root_, consumer.extension) == nullptr;
                }
            );
          }
          if (const auto* selection = std::get_if<TextSelectionRecognitionState>(&recognition.state)) {
            detail::MountedNode* node = FindNode(*state_->mounted_root_, selection->node_identity);
            return node == nullptr || FindTextSelectionClient(*node) == nullptr;
          }
          return false;
        });
    if (!session.quarantined && (references_inactive || recognition_removed)) {
      cancellations.push_back(
          PointerEvent{
              PointerEventType::Cancel,
              pointer_id,
              session.last_position,
              session.device_kind,
          }
      );
    }
  }
  for (const PointerEvent& cancellation : cancellations) {
    QuarantinePointerSession(cancellation.pointer_id, cancellation);
  }
  if (state_->keyboard_activation_identity_.has_value() && state_->keyboard_press_id_.has_value()) {
    const std::optional<bool> enabled =
        DeclaredEnabled(*state_->mounted_root_, *state_->keyboard_activation_identity_);
    if (!enabled.value_or(false)) {
      if (detail::MountedNode* pressed = FindNode(*state_->mounted_root_, *state_->keyboard_activation_identity_)) {
        EndInteraction(*pressed, InteractionEvent::Type::Cancel, InteractionEvent::Source::Keyboard,
                       *state_->keyboard_press_id_);
      }
      state_->keyboard_activation_identity_.reset();
      state_->keyboard_press_id_.reset();
    }
  }
  ResolveEnabledTree(*state_->mounted_root_, true);
  detail::MountedNode* focus_root = ActiveFocusTrapRoot();
  const std::optional<std::uint64_t> focus_identity =
      focus_root == nullptr ? std::nullopt : std::optional{focus_root->identity};
  const std::optional<std::uint64_t> previous_focus_identity =
      state_->focus_trap_stack_.empty() ? std::nullopt : std::optional{state_->focus_trap_stack_.back().identity};
  if (previous_focus_identity != focus_identity) {
    // Nested traps retain the focus that each one displaced so removing the topmost trap restores in stack order.
    const auto existing = focus_identity.has_value()
                              ? std::ranges::find(state_->focus_trap_stack_, *focus_identity, &FocusTrapFrame::identity)
                              : state_->focus_trap_stack_.end();
    if (existing == state_->focus_trap_stack_.end() && focus_identity.has_value()) {
      state_->focus_trap_stack_.push_back({*focus_identity, state_->focused_node_identity_});
    } else {
      std::optional<std::uint64_t> restore_identity;
      while (!state_->focus_trap_stack_.empty() &&
             (!focus_identity.has_value() || state_->focus_trap_stack_.back().identity != *focus_identity)) {
        restore_identity = state_->focus_trap_stack_.back().restore_identity;
        state_->focus_trap_stack_.pop_back();
      }
      SetFocusedNode(restore_identity);
    }
  }
  focus_root = ActiveFocusTrapRoot();
  if (state_->focused_node_identity_.has_value()) {
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    if (!focused || !focused->interaction.enabled || !focused->focusable ||
        (focus_root && !ContainsNodeIdentity(*focus_root, focused->identity))) {
      SetFocusedNode(std::nullopt);
    }
  }
  if (focus_root && !state_->focused_node_identity_.has_value()) {
    std::vector<detail::MountedNode*> layer_focusable;
    CollectFocusableNodes(*focus_root, layer_focusable);
    if (!layer_focusable.empty()) {
      SetFocusedNode(layer_focusable.front()->identity);
    }
  }

  std::erase_if(state_->hovered_extensions_, [this](const detail::NodeExtensionHandle& hovered) {
    detail::MountedNode* node = FindNode(*state_->mounted_root_, hovered.node_identity);
    NodeExtension* extension = FindExtension(*state_->mounted_root_, hovered);
    const bool remove = !node || !extension || (!node->interaction.enabled && !extension->HoverWhenDisabled());
    if (remove && node && extension) {
      extension->OnHoverChanged(*node, false);
    }
    return remove;
  });

  ResolveFocusedFlags(*state_->mounted_root_, state_->focused_node_identity_, state_->focus_visible_);
}

detail::MountedNode* Runtime::ActiveFocusTrapRoot() {
  if (!state_->mounted_root_) {
    return nullptr;
  }
  return FindTopmostFocusTrap(*state_->mounted_root_);
}

std::optional<std::uint64_t> Runtime::ResolvePointerFocusTarget(const std::vector<detail::MountedNode*>& route) {
  std::optional<std::uint64_t> candidate;
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if ((*node)->interaction.enabled && (*node)->focusable) {
      candidate = (*node)->identity;
      break;
    }
  }

  detail::MountedNode* focus_root = ActiveFocusTrapRoot();
  if (!focus_root || (candidate.has_value() && ContainsNodeIdentity(*focus_root, *candidate))) {
    return candidate;
  }
  if (state_->focused_node_identity_.has_value() &&
      ContainsNodeIdentity(*focus_root, *state_->focused_node_identity_)) {
    return state_->focused_node_identity_;
  }

  std::vector<detail::MountedNode*> focusable;
  CollectFocusableNodes(*focus_root, focusable);
  return focusable.empty() ? std::nullopt : std::optional{focusable.front()->identity};
}

bool Runtime::HandleBack() {
  return HandleBack(BackEvent{});
}

bool Runtime::HandleBack(const BackEvent& incoming) {
  BackEvent event = incoming;
  if (!std::isfinite(event.progress)) {
    event.progress = event.phase == BackPhase::Commit ? 1.0F : 0.0F;
  }
  event.progress = std::clamp(event.progress, 0.0F, 1.0F);

  const auto dispatch_captured = [this, &event](const detail::BackTarget& target) {
    switch (target.kind) {
    case detail::BackTargetKind::SelectionOverlay:
      if (event.phase == BackPhase::Commit) {
        HideTextSelectionOverlay();
      }
      return true;
    case detail::BackTargetKind::Layer: {
      if (event.phase != BackPhase::Commit) {
        return true;
      }
      const auto found = std::ranges::find(state_->layer_controller_.state_->entries, target.layer_id, &LayerEntry::id);
      if (found == state_->layer_controller_.state_->entries.end()) {
        return true;
      }
      if (found->options.cancel_policy == LayerCancelPolicy::Consume) {
        return true;
      }
      static_cast<void>(state_->layer_controller_.RequestDismiss(found->id).handled);
      return true;
    }
    case detail::BackTargetKind::Event: {
      if (event.phase != BackPhase::Commit || !state_->mounted_root_) {
        return true;
      }
      detail::MountedNode* node = FindNode(*state_->mounted_root_, target.node_identity);
      if (node != nullptr && node->interaction.enabled) {
        static_cast<void>(EmitEvent<ViewEvents::BackRequested>(node->event_bindings));
      }
      return true;
    }
    case detail::BackTargetKind::Extension: {
      if (!state_->mounted_root_) {
        return true;
      }
      detail::MountedNode* node = FindNode(*state_->mounted_root_, target.extension.node_identity);
      NodeExtension* extension = FindExtension(*state_->mounted_root_, target.extension);
      if (node != nullptr && node->interaction.enabled && extension != nullptr) {
        static_cast<void>(extension->OnBack(*node, event));
      }
      // A target captured at Begin owns the whole transaction even when it disappears before Commit.
      return true;
    }
    }
    return false;
  };

  if (event.phase == BackPhase::Begin && state_->back_target_.has_value()) {
    const BackEvent begin = event;
    event = {BackPhase::Cancel, 0.0F};
    static_cast<void>(dispatch_captured(*state_->back_target_));
    state_->back_target_.reset();
    event = begin;
  }

  if (event.phase != BackPhase::Begin && state_->back_target_.has_value()) {
    const detail::BackTarget target = *state_->back_target_;
    const bool handled = dispatch_captured(target);
    if (event.phase == BackPhase::Cancel || event.phase == BackPhase::Commit || !handled) {
      state_->back_target_.reset();
    }
    if (handled) {
      RequestFrame();
    }
    return handled;
  }

  if (event.phase == BackPhase::Update || event.phase == BackPhase::Cancel) {
    return false;
  }
  state_->back_target_.reset();
  if (!state_->mounted_root_) {
    return false;
  }
  detail::MountedNode* application_root = FindApplicationRoot(*state_->mounted_root_);
  if (!application_root) {
    return false;
  }

  detail::BackTarget target;
  bool already_dispatched = false;
  if (state_->text_selection_overlay_.state.visible) {
    target.kind = detail::BackTargetKind::SelectionOverlay;
  } else {
    const LayerEntry* topmost = nullptr;
    for (const LayerEntry& entry : state_->layer_controller_.state_->entries) {
      const bool exiting = entry.transition && !entry.transition->target_visible;
      if (!exiting && entry.options.cancel_policy != LayerCancelPolicy::PassThrough &&
          (topmost == nullptr || LayerPaintsAbove(entry, *topmost))) {
        topmost = &entry;
      }
    }
    if (topmost != nullptr) {
      target.kind = detail::BackTargetKind::Layer;
      target.layer_id = topmost->id;
    } else if (!RouteBackTarget(*application_root, event, target, already_dispatched)) {
      return false;
    }
  }

  if (event.phase == BackPhase::Begin) {
    state_->back_target_ = target;
    RequestFrame();
    return true;
  }
  if (already_dispatched) {
    RequestFrame();
    return true;
  }
  const bool handled = dispatch_captured(target);
  if (handled) {
    RequestFrame();
  }
  return handled;
}

void Runtime::SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible) {
  if (identity.has_value()) {
    if (!state_->mounted_root_) {
      identity.reset();
    } else {
      detail::MountedNode* candidate = FindNode(*state_->mounted_root_, *identity);
      if (!candidate || !candidate->interaction.enabled || !candidate->focusable) {
        identity.reset();
      }
    }
  }
  const bool next_focus_visible = focus_visible.value_or(state_->focus_visible_);
  if (state_->focused_node_identity_ == identity && state_->focus_visible_ == next_focus_visible) {
    return;
  }
  if (state_->focused_node_identity_ == identity) {
    state_->focus_visible_ = next_focus_visible;
    if (identity.has_value() && state_->mounted_root_) {
      if (detail::MountedNode* focused = FindNode(*state_->mounted_root_, *identity)) {
        InteractionState interaction = focused->interaction;
        interaction.focus_visible = state_->focus_visible_;
        focused->foreground_paint_dirty = true;
        UpdateInteraction(*focused, interaction);
      }
    }
    RequestFrame();
    return;
  }

  HideTextSelectionOverlay();
  if (state_->focused_node_identity_.has_value() && state_->mounted_root_) {
    if (detail::MountedNode* previous = FindNode(*state_->mounted_root_, *state_->focused_node_identity_)) {
      InteractionState interaction = previous->interaction;
      interaction.focused = false;
      interaction.focus_visible = false;
      previous->foreground_paint_dirty = true;
      UpdateInteraction(*previous, interaction);
      DispatchFocusChanged(*previous, false);
    }
  }
  if (state_->keyboard_activation_identity_.has_value() && state_->keyboard_press_id_.has_value() &&
      state_->mounted_root_) {
    if (detail::MountedNode* pressed = FindNode(*state_->mounted_root_, *state_->keyboard_activation_identity_)) {
      EndInteraction(*pressed, InteractionEvent::Type::Cancel, InteractionEvent::Source::Keyboard,
                     *state_->keyboard_press_id_);
    }
  }
  state_->keyboard_activation_identity_.reset();
  state_->keyboard_press_id_.reset();
  state_->focused_node_identity_ = identity;
  state_->focus_visible_ = next_focus_visible;
  if (state_->focused_node_identity_.has_value() && state_->mounted_root_) {
    if (detail::MountedNode* next = FindNode(*state_->mounted_root_, *state_->focused_node_identity_)) {
      InteractionState interaction = next->interaction;
      interaction.focused = true;
      interaction.focus_visible = state_->focus_visible_;
      next->foreground_paint_dirty = true;
      UpdateInteraction(*next, interaction);
      DispatchFocusChanged(*next, true);
    }
  }
  RequestFrame();
}

void Runtime::MoveFocus(bool reverse, bool wrap) {
  if (!state_->mounted_root_) {
    return;
  }
  std::vector<detail::MountedNode*> focusable;
  detail::MountedNode* root = ActiveFocusTrapRoot();
  CollectFocusableNodes(root ? *root : *state_->mounted_root_, focusable);
  if (focusable.empty()) {
    SetFocusedNode(std::nullopt, true);
    return;
  }

  auto current = focusable.end();
  if (state_->focused_node_identity_.has_value()) {
    current = std::find_if(focusable.begin(), focusable.end(), [this](const detail::MountedNode* node) {
      return node->identity == *state_->focused_node_identity_;
    });
  }

  if (current == focusable.end()) {
    SetFocusedNode((reverse ? focusable.back() : focusable.front())->identity, true);
    return;
  }
  if (reverse) {
    if (current == focusable.begin()) {
      if (!wrap) {
        return;
      }
      current = focusable.end();
    }
    --current;
  } else {
    ++current;
    if (current == focusable.end()) {
      if (!wrap) {
        return;
      }
      current = focusable.begin();
    }
  }
  SetFocusedNode((*current)->identity, true);
}

bool Runtime::UpdateNodeExtensions(
    detail::MountedNode& node,
    const FrameInfo& frame,
    bool& needs_frame,
    std::optional<double>& next_wakeup,
    bool rebuild_cache
) {
  // Extension presence is a structural cache. Layout participation gates this frame's callbacks without discarding
  // retained extensions, allowing them to resume when their subtree participates again.
  if (rebuild_cache) {
    RefreshExtensionPresence(node);
  }
  if (!rebuild_cache && !node.subtree_has_extensions) {
    return false;
  }
  if (!node.participates_in_layout) {
    return node.subtree_has_extensions;
  }

  node.presentation.local_transform = {};
  node.presentation.local_opacity = 1.0F;
  FrameInfo node_frame = frame;
  if (!node.extensions.empty()) {
    node_frame.reduced_motion = node_frame.reduced_motion || node.reduced_motion;
  }
  for (NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    if (entry.interaction_sync_pending) {
      entry.extension->OnInteraction(node, node.interaction, std::nullopt);
      entry.interaction_sync_pending = false;
    }
    const NodeExtension::FrameResult result = entry.extension->OnFrame(node, node_frame);
    needs_frame = needs_frame || result.needs_frame;
    if (result.wake_after.has_value() && (!next_wakeup.has_value() || *result.wake_after < *next_wakeup)) {
      next_wakeup = *result.wake_after;
    }
  }

  for (auto& child : node.children) {
    UpdateNodeExtensions(*child, frame, needs_frame, next_wakeup, false);
  }
  return node.subtree_has_extensions;
}

void Runtime::BindExtensionInvalidation(detail::MountedNode& node) {
  for (NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    entry.extension->BindPaintInvalidation([this, owner = &node](NodeExtension::PaintInvalidation invalidation) {
      if (invalidation == NodeExtension::PaintInvalidation::Content ||
          invalidation == NodeExtension::PaintInvalidation::Both) {
        owner->content_paint_dirty = true;
      }
      if (invalidation == NodeExtension::PaintInvalidation::Foreground ||
          invalidation == NodeExtension::PaintInvalidation::Both) {
        owner->foreground_paint_dirty = true;
      }
      if (!state_->building_frame_) {
        RequestFrame();
      }
    });
    entry.extension->BindSemanticsInvalidation([this] {
      if (!state_->building_frame_) {
        RequestFrame();
      }
    });
  }
}

void Runtime::HandleScrollEvent(const ScrollEvent& event) {
  if (!state_->mounted_root_) {
    return;
  }
  ScrollEventResult result = ApplyScrollEvent(*state_->mounted_root_, event);
  for (detail::MountedNode* node : result.scroll_chain) {
    node->scroll_state->motion.Stop();
  }
  for (detail::MountedNode* node : result.scroll_chain) {
    NotifyScrollActivity(*node, ScrollActivitySource::External);
  }
}

bool Runtime::HandleFocusedTextInputKey(const KeyEvent& event) {
  if (!state_->text_input_session_.has_value() || !state_->focused_node_identity_.has_value()) {
    return false;
  }
  const detail::ActiveTextInputSession& active = *state_->text_input_session_;
  if (active.node_identity != *state_->focused_node_identity_) {
    return false;
  }
  const std::uint64_t node_identity = active.node_identity;
  const TextInputSessionId session_id = active.session_id;
  const std::shared_ptr<TextInputClient> client = active.client;
  const TextInputState previous = active.state;

  const bool next_action = event.type == KeyEventType::Down && event.key == Key::Enter && !event.modifiers.shift &&
                           !event.modifiers.control && !event.modifiers.alt && !event.modifiers.meta &&
                           active.configuration.action == TextInputAction::Next;
  if (next_action && event.repeat) {
    return true;
  }
  if (client->HandleTextKey(event) != TextInputKeyResult::Handled) {
    return false;
  }

  const TextInputState current = client->State();
  if (!detail::IsValidTextInputState(current, session_id) ||
      !detail::IsValidTextInputStateTransition(previous, current)) {
    throw std::logic_error("HuxerUI text input client returned invalid state after handling a key event");
  }
  InvalidateTextInputStateChange(node_identity, previous, current);
  if (next_action) {
    MoveFocus(false, false);
  }
  RefreshTextInputSession();
  return true;
}

void Runtime::HandleKeyEvent(const KeyEvent& event) {
  if (!state_->mounted_root_) {
    return;
  }
  if (event.type == KeyEventType::Down && event.key == Key::Escape && !event.repeat && HandleBack()) {
    return;
  }
  if (event.type == KeyEventType::Down && event.key == Key::Tab && !event.repeat) {
    MoveFocus(event.modifiers.shift);
    RefreshTextInputSession();
    return;
  }
  if (!state_->focused_node_identity_.has_value()) {
    return;
  }

  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused || !focused->interaction.enabled || !focused->focusable) {
    SetFocusedNode(std::nullopt);
    RefreshTextInputSession();
    return;
  }

  if (event.type == KeyEventType::Down && !IsModifierKey(event.key)) {
    SetFocusedNode(focused->identity, true);
  }
  if (event.type == KeyEventType::Down && !event.repeat && !event.modifiers.alt &&
      (event.modifiers.control || event.modifiers.meta)) {
    std::optional<TextEditingAction> action;
    switch (event.key) {
    case Key::A:
      action = TextEditingAction::SelectAll;
      break;
    case Key::C:
      action = TextEditingAction::Copy;
      break;
    case Key::V:
      action = TextEditingAction::Paste;
      break;
    case Key::X:
      action = TextEditingAction::Cut;
      break;
    default:
      break;
    }
    if (action.has_value() && (state_->text_input_session_.has_value() || CanPerformTextEditingAction(*action))) {
      PerformTextEditingAction(*action);
      RequestFrame();
      RefreshTextInputSession();
      return;
    }
  }
  if (HandleFocusedTextInputKey(event)) {
    return;
  }
  DispatchKey(*focused, event);
  const bool activatable = IsActivatable(*focused);
  if (event.type == KeyEventType::Down) {
    if (activatable && event.key == Key::Enter && !event.repeat) {
      ActivateNode(*focused);
    } else if (activatable && event.key == Key::Space && !event.repeat &&
               !state_->keyboard_activation_identity_.has_value()) {
      state_->keyboard_activation_identity_ = focused->identity;
      state_->keyboard_press_id_ = BeginInteraction(*focused, InteractionEvent::Source::Keyboard);
    }
  } else if (event.key == Key::Space) {
    if (state_->keyboard_activation_identity_.has_value() &&
        *state_->keyboard_activation_identity_ == focused->identity) {
      if (state_->keyboard_press_id_.has_value()) {
        EndInteraction(*focused, activatable ? InteractionEvent::Type::Release : InteractionEvent::Type::Cancel,
                       InteractionEvent::Source::Keyboard, *state_->keyboard_press_id_);
      }
      if (activatable) {
        ActivateNode(*focused);
      }
    }
    state_->keyboard_activation_identity_.reset();
    state_->keyboard_press_id_.reset();
  }
  RequestFrame();
  RefreshTextInputSession();
}

void Runtime::InvalidateRoot() {
  state_->application_dirty_ = true;
  RequestFrame();
}

void Runtime::InvalidateLayers() {
  state_->layers_dirty_ = true;
  RequestFrame();
}

void Runtime::DeactivateLayerInput(LayerId id) {
  if (!state_->mounted_root_) {
    return;
  }
  detail::MountedNode* layer = FindLayerEntryNode(*state_->mounted_root_, id);
  if (!layer) {
    return;
  }

  std::vector<PointerEvent> cancellations;
  for (const auto& [pointer_id, session] : state_->pointer_sessions_) {
    if (PointerSessionReferencesNode(session, *layer)) {
      cancellations.push_back(PointerEvent{
          PointerEventType::Cancel,
          pointer_id,
          session.last_position,
          session.device_kind,
      });
    }
  }
  for (const PointerEvent& cancellation : cancellations) {
    QuarantinePointerSession(cancellation.pointer_id, cancellation);
  }

  std::erase_if(state_->hovered_extensions_, [this, layer](const detail::NodeExtensionHandle& hovered) {
    if (!ContainsNodeIdentity(*layer, hovered.node_identity)) {
      return false;
    }
    if (NodeExtension* extension = FindExtension(*state_->mounted_root_, hovered)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, hovered.node_identity)) {
        extension->OnHoverChanged(*node, false);
      }
    }
    return true;
  });

  const bool text_input_belongs_to_layer =
      state_->text_input_session_.has_value() &&
      ContainsNodeIdentity(*layer, state_->text_input_session_->node_identity);
  if (state_->focused_node_identity_.has_value() && ContainsNodeIdentity(*layer, *state_->focused_node_identity_)) {
    SetFocusedNode(std::nullopt);
  }
  if (text_input_belongs_to_layer) {
    StopTextInputSession(TextInputEndReason::FocusLost);
  }
}

void Runtime::InvalidateLayerPlacement(LayerId id) {
  if (state_->mounted_root_) {
    if (detail::MountedNode* layer = FindLayerEntryNode(*state_->mounted_root_, id)) {
      MarkLayoutDirtyPath(*state_->mounted_root_, layer->identity);
      if (!state_->building_frame_) {
        RequestFrame();
      }
      return;
    }
  }
  RequestFrame();
}

void Runtime::InvalidateScope(std::uint64_t scope_id) {
  if (scope_id == state_->root_scope_->Id()) {
    state_->application_dirty_ = true;
  }
  RequestFrame();
}

void Runtime::InvalidateLayout(detail::MountedNode& mounted) {
  if (state_->mounted_root_) {
    MarkLayoutDirtyPath(*state_->mounted_root_, mounted.identity);
  }
  RequestFrame();
}

bool Runtime::RecomposeDirtyScopes(detail::MountedNode& mounted) {
  if (mounted.kind == NodeKind::Scope && mounted.recompose_scope && mounted.recompose_scope->IsDirty()) {
    const bool layout_changed = ComposeScope(mounted);
    mounted.render_structure_dirty = mounted.render_structure_dirty || layout_changed;
    return layout_changed;
  }

  bool layout_changed = false;
  for (auto& child : mounted.children) {
    layout_changed = RecomposeDirtyScopes(*child) || layout_changed;
  }
  if (layout_changed) {
    mounted.measure_dirty = true;
    mounted.render_structure_dirty = true;
  }
  return layout_changed;
}

void Runtime::EnsureRootStructure() {
  if (state_->mounted_root_) {
    if (!FindWindowBackplane(*state_->mounted_root_) || !FindApplicationContent(*state_->mounted_root_) ||
        !FindLayerStack(*state_->mounted_root_) ||
        (state_->window_->chrome_mode == WindowChromeMode::Custom && !FindWindowControls(*state_->mounted_root_))) {
      throw std::logic_error("HuxerUI RuntimeRoot has an invalid child structure");
    }
    return;
  }

  View root = RuntimeRootLayout{};
  state_->mounted_root_ = Mount(root.spec_, state_->root_environment_);
  View backplane = Canvas([window = state_->window_](PaintContext& context, Size size) {
                     const float top = std::min(std::max(0.0F, window->metrics.safe_area.top), size.height);
                     const float bottom =
                         std::min(std::max(0.0F, window->metrics.safe_area.bottom), std::max(0.0F, size.height - top));
                     if (top > 0.0F) {
                       context.DrawRect({0.0F, 0.0F, size.width, top}, window->appearance.status_bar_background);
                     }
                     if (bottom > 0.0F) {
                       context.DrawRect(
                           {0.0F, size.height - bottom, size.width, bottom},
                           window->appearance.navigation_bar_background
                       );
                     }
                   }).LayoutValue<WindowBackplaneValue>(true);
  View application = ApplicationContentLayout{};
  if (state_->window_->content_mode == WindowContentMode::SafeArea) {
    application = std::move(application).With(SafeAreaPadding{});
  }
  View layers = LayerStackLayout{};
  state_->mounted_root_->children.push_back(Mount(backplane.spec_, state_->root_environment_));
  state_->mounted_root_->children.push_back(Mount(application.spec_, state_->root_environment_));
  state_->mounted_root_->children.push_back(Mount(layers.spec_, state_->root_environment_));
  if (state_->window_->chrome_mode == WindowChromeMode::Custom) {
    View controls = MakeWindowControls(
        state_->window_service_,
        state_->window_,
        state_->root_environment_,
        HasWindowControlGeometry(state_->window_->metrics)
    );
    state_->mounted_root_->children.push_back(Mount(controls.spec_, state_->root_environment_));
  }
}

void Runtime::ReconcileWindowControls() {
  if (!state_->mounted_root_ || state_->window_->chrome_mode != WindowChromeMode::Custom) {
    return;
  }
  const auto found = std::ranges::find_if(state_->mounted_root_->children, [](const auto& child) {
    return child && IsWindowControlsNode(*child);
  });
  if (found == state_->mounted_root_->children.end()) {
    return;
  }
  View controls = MakeWindowControls(
      state_->window_service_,
      state_->window_,
      state_->root_environment_,
      HasWindowControlGeometry(state_->window_->metrics)
  );
  Reconcile(*found, controls.spec_, state_->root_environment_);
}

void Runtime::CommitWindowAppearance() {
  if (!state_->mounted_root_) {
    return;
  }
  const detail::MountedNode* application = FindApplicationRoot(*state_->mounted_root_);
  const SystemBarsAppearance* theme_appearance =
      application == nullptr ? nullptr : FindApplicationSystemBarsFallback(*application);
  const SystemBarsAppearance fallback =
      theme_appearance == nullptr ? SystemBarsAppearance::Default() : *theme_appearance;
  if (!IsValidSystemBarsAppearance(fallback)) {
    throw std::invalid_argument("HuxerUI system bars appearance is invalid");
  }

  const WindowMetrics& metrics = state_->window_->metrics;
  const float status_boundary =
      state_->window_->content_mode == WindowContentMode::SafeArea ? metrics.safe_area.top : 0.0F;
  const float navigation_boundary = state_->window_->content_mode == WindowContentMode::SafeArea
                                        ? metrics.viewport.height - metrics.safe_area.bottom
                                        : metrics.viewport.height;
  SystemBarCandidates candidates;
  CollectSystemBarCandidates(*state_->mounted_root_, status_boundary, navigation_boundary, candidates);

  SystemBarsAppearance resolved = fallback;
  if (candidates.status.has_value()) {
    if (!IsValidSystemBarsAppearance(*candidates.status)) {
      throw std::invalid_argument("HuxerUI system bars appearance is invalid");
    }
    resolved.status_bar_background = candidates.status->status_bar_background;
    resolved.status_bar_content = candidates.status->status_bar_content;
  }
  if (candidates.navigation.has_value()) {
    if (!IsValidSystemBarsAppearance(*candidates.navigation)) {
      throw std::invalid_argument("HuxerUI system bars appearance is invalid");
    }
    resolved.navigation_bar_background = candidates.navigation->navigation_bar_background;
    resolved.navigation_bar_content = candidates.navigation->navigation_bar_content;
  }

  const Color status_background = CompositeOver(resolved.status_bar_background, fallback.status_bar_background);
  const Color navigation_background =
      CompositeOver(resolved.navigation_bar_background, fallback.navigation_bar_background);
  const auto brightness = std::pair{
      ResolveBrightness(resolved.status_bar_content, status_background),
      ResolveBrightness(resolved.navigation_bar_content, navigation_background),
  };
  if (state_->window_->committed_system_bar_brightness != brightness) {
    state_->window_->committed_system_bar_brightness = brightness;
    state_->platform_->SetSystemBarsContentBrightness(brightness.first, brightness.second);
  }
  std::optional<Color> title_bar_background;
  if (application != nullptr && metrics.title_bar.has_value()) {
    CollectWindowDragRegionBackground(*application, status_background, metrics.title_bar->height, title_bar_background);
  }
  const Color caption_background = title_bar_background.value_or(status_background);
  const Color caption_foreground = ResolveCaptionForeground(caption_background);
  const bool appearance_changed = state_->window_->appearance != resolved;
  const bool caption_foreground_changed = state_->window_->caption_foreground != caption_foreground;
  if (appearance_changed) {
    state_->window_->appearance = resolved;
    if (detail::MountedNode* backplane = FindWindowBackplane(*state_->mounted_root_)) {
      backplane->content_paint_dirty = true;
    }
  }
  if (caption_foreground_changed) {
    state_->window_->caption_foreground = caption_foreground;
    if (detail::MountedNode* controls = FindWindowControls(*state_->mounted_root_)) {
      MarkContentPaintDirtyTree(*controls);
    }
  }
}

void Runtime::ComposeApplication() {
  state_->application_dirty_ = false;
  bool scope_composing = false;

  try {
    state_->root_scope_->BeginComposition();
    scope_composing = true;
    Composer composer{state_->root_scope_, state_->root_environment_};
    Composer::Guard guard{composer};

    View application = state_->root_factory_();
    EnsureRootStructure();
    detail::MountedNode* application_content = FindApplicationContent(*state_->mounted_root_);
    if (!application_content) {
      throw std::logic_error("HuxerUI RuntimeRoot is missing its application content container");
    }

    std::vector<View> application_children;
    if (application) {
      application_children.push_back(std::move(application));
    }
    if (ReconcileChildren(application_content->children, application_children, state_->root_environment_)) {
      application_content->measure_dirty = true;
      application_content->render_structure_dirty = true;
      state_->mounted_root_->measure_dirty = true;
      state_->mounted_root_->render_structure_dirty = true;
    }
    state_->root_scope_->EndComposition();
    scope_composing = false;
  } catch (...) {
    if (scope_composing) {
      state_->root_scope_->AbortComposition();
    } else {
      state_->root_scope_->Invalidate();
    }
    InvalidateRoot();
    throw;
  }
}

void Runtime::ComposeLayers() {
  state_->layers_dirty_ = false;
  EnsureRootStructure();
  detail::MountedNode* layer_stack = FindLayerStack(*state_->mounted_root_);
  if (!layer_stack) {
    throw std::logic_error("HuxerUI RuntimeRoot is missing its LayerStack");
  }

  try {
    // Factories may mutate the controller while composing; those mutations mark layers dirty for the next frame.
    const std::vector<LayerEntry> ordered = OrderedLayerEntries(state_->layer_controller_.state_->entries);
    std::vector<std::pair<View, std::shared_ptr<const Environment>>> layer_children;
    layer_children.reserve(ordered.size());
    for (const LayerEntry& entry : ordered) {
      const auto environment = entry.environment ? entry.environment : state_->root_environment_;
      View content = Scope(entry.content);
      const bool barrier = entry.options.pointer_policy == LayerPointerPolicy::Barrier;
      const bool exiting = entry.transition && !entry.transition->target_visible;
      if (exiting) {
        content.spec_->pointer_events_enabled = false;
        content.spec_->local_enabled = false;
      }
      if (barrier) {
        content = std::move(content).On<ViewEvents::PointerDown>([](const PointerEvent&) {});
      }

      View layer = LayerEntryLayout{std::move(content)}
                       .LayoutValue<LayerPlacementValue>(entry.placement)
                       .LayoutValue<LayerEntrySnapshotValue>(LayerEntrySnapshot{
                           .id = entry.id,
                           .revision = entry.revision,
                           .exiting = exiting,
                           .semantic_modal_group = entry.semantic_modal_group,
                       });
      if (entry.placement->safe_area_policy == LayerSafeAreaPolicy::Constrain) {
        layer = std::move(layer).With(SafeAreaPadding{});
      } else if (entry.placement->safe_area_policy == LayerSafeAreaPolicy::ExtendBottom) {
        layer = std::move(layer).With(SafeAreaPadding{.bottom = false});
      }
      // An exiting modal keeps its focus barrier until removal so input cannot fall through while its content fades.
      layer.spec_->trap_focus = entry.options.trap_focus;
      if (barrier) {
        layer = std::move(layer).On<ViewEvents::PointerDown>([controller = state_->layer_controller_,
                                                              id = entry.id,
                                                              dismiss = entry.options.dismiss_on_outside_press &&
                                                                        !exiting](const PointerEvent&) {
          if (!dismiss) {
            return;
          }
          static_cast<void>(controller.RequestDismiss(id).handled);
        });
        if (entry.options.barrier_color.has_value()) {
          layer = std::move(layer).With(Background{*entry.options.barrier_color});
        }
      } else if (entry.options.pointer_policy == LayerPointerPolicy::PassThrough) {
        layer.spec_->pointer_events_enabled = false;
      }
      if (entry.transition) {
        layer = std::move(layer).With(LayerTransition{entry.transition});
      }
      layer_children.emplace_back(std::move(layer), environment);
    }

    if (ReconcileLayerChildren(layer_stack->children, layer_children)) {
      layer_stack->measure_dirty = true;
      layer_stack->render_structure_dirty = true;
      state_->mounted_root_->measure_dirty = true;
      state_->mounted_root_->render_structure_dirty = true;
    }
  } catch (...) {
    InvalidateLayers();
    throw;
  }
}

bool Runtime::ComposeScope(detail::MountedNode& mounted) {
  if (!mounted.scope_factory) {
    const bool layout_changed = !mounted.children.empty();
    mounted.children.clear();
    mounted.measure_dirty = mounted.measure_dirty || layout_changed;
    return layout_changed;
  }
  if (!mounted.recompose_scope) {
    mounted.recompose_scope = std::make_shared<RecomposeScope>(*this, state_->next_scope_identity_++);
  }
  mounted.recompose_scope->SetEventBindings(mounted.event_bindings);

  bool scope_composing = false;
  try {
    mounted.recompose_scope->BeginComposition();
    scope_composing = true;
    Composer composer{mounted.recompose_scope, mounted.environment ? mounted.environment : state_->root_environment_};
    Composer::Guard guard{composer};

    View content = mounted.scope_factory();

    std::vector<View> children;
    if (content) {
      children.push_back(std::move(content));
    }
    const bool layout_changed = ReconcileChildren(mounted.children, children, mounted.environment);
    mounted.recompose_scope->EndComposition();
    scope_composing = false;
    mounted.measure_dirty = mounted.measure_dirty || layout_changed;
    return layout_changed;
  } catch (...) {
    if (scope_composing) {
      mounted.recompose_scope->AbortComposition();
      InvalidateScope(mounted.recompose_scope->Id());
    } else {
      mounted.recompose_scope->Invalidate();
    }
    throw;
  }
}

bool Runtime::Reconcile(std::unique_ptr<detail::MountedNode>& mounted, const std::shared_ptr<ViewSpec>& incoming,
                        const std::shared_ptr<const Environment>& environment) {
  if (state_->text_selection_overlay_.state.visible) {
    state_->text_selection_overlay_.state.paint_dirty = true;
  }
  const bool compatible = mounted && IsCompatibleNode(*mounted, *incoming) && mounted->key == incoming->key;
  if (!compatible) {
    state_->extension_tree_dirty_ = true;
    mounted = Mount(incoming, environment);
    return true;
  }

  std::shared_ptr<const Environment> mounted_environment = environment;
  std::optional<EnvironmentTransaction> environment_transaction;
  if (incoming->kind == NodeKind::Environment) {
    ValidateEnvironmentDeclaration(*incoming);
    if (!mounted->owned_environment) {
      throw std::logic_error("HuxerUI mounted Environment node has no owned values");
    }
    environment_transaction.emplace(*mounted->owned_environment, *incoming->local_environment, environment);
    mounted_environment = mounted->owned_environment;
  }
  ViewSpec compiled = CompileViewSpec(*incoming, mounted_environment, *state_->app_resources_);
  if (environment_transaction.has_value()) {
    environment_transaction->Commit();
    environment_transaction.reset();
  }

  if (compiled.platform_view &&
      !detail::PlatformViewPropertiesEqual(mounted->platform_view, compiled.platform_view)) {
    detail::BindExternalTextures(compiled.platform_view->properties, state_->platform_->external_texture_surface_);
  }

  bool layout_changed = !LayoutInputsEqual(*mounted, compiled);
  if (!ContentPaintInputsEqual(*mounted, compiled)) {
    mounted->content_paint_dirty = true;
  }
  if (!ForegroundPaintInputsEqual(*mounted, compiled)) {
    mounted->foreground_paint_dirty = true;
  }
  std::vector<const ModifierDescriptor*> previous_extension_descriptors;
  previous_extension_descriptors.reserve(mounted->extensions.size());
  for (const NodeExtensionEntry& entry : mounted->extensions) {
    previous_extension_descriptors.push_back(entry.descriptor);
  }
  const bool extension_node_inputs_equal = ExtensionNodeInputsEqual(*mounted, compiled, mounted_environment);
  ApplyViewDeclaration(*mounted, compiled, mounted_environment);
  const ModifierChanges modifier_changes =
      ReconcileNodeExtensions(*mounted, compiled.modifiers, extension_node_inputs_equal);
  layout_changed = layout_changed || modifier_changes.layout_changed;
  if (modifier_changes.changed) {
    mounted->foreground_paint_dirty = true;
  }
  if (modifier_changes.structure_changed) {
    std::erase_if(
        mounted->virtual_semantic_identities,
        [&previous_extension_descriptors, owner = mounted.get()](const auto& entry) {
          const std::size_t index = entry.first.extension_index;
          return index >= owner->extensions.size() || index >= previous_extension_descriptors.size() ||
                 owner->extensions[index].descriptor != previous_extension_descriptors[index];
        }
    );
    state_->extension_tree_dirty_ = true;
    BindExtensionInvalidation(*mounted);
  }
  if (mounted->kind == NodeKind::Scope) {
    layout_changed = ComposeScope(*mounted) || layout_changed;
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state->dependency_capture->Clear();
    mounted->virtual_state->source = incoming->virtual_items;
    mounted->virtual_state->item_declarations.clear();
    mounted->virtual_state->pending_scroll_offset.reset();
    if (mounted->virtual_state->item_state_cache) {
      std::erase_if(
          mounted->virtual_state->item_state_cache->indexed,
          [item_count = incoming->virtual_items.size](const auto& entry) { return entry.first >= item_count; }
      );
      if (mounted->virtual_state->item_state_cache->keyed.empty() &&
          mounted->virtual_state->item_state_cache->indexed.empty()) {
        mounted->virtual_state->item_state_cache.reset();
      }
    }
    layout_changed = true;
  } else {
    layout_changed = ReconcileChildren(mounted->children, incoming->children, mounted->environment) || layout_changed;
  }
  mounted->measure_dirty = mounted->measure_dirty || layout_changed;
  mounted->render_structure_dirty = mounted->render_structure_dirty || layout_changed;
  return layout_changed;
}

std::unique_ptr<detail::MountedNode>
Runtime::Mount(const std::shared_ptr<ViewSpec>& incoming, const std::shared_ptr<const Environment>& environment) {
  std::shared_ptr<Environment> owned_environment;
  std::shared_ptr<const Environment> mounted_environment = environment;
  if (incoming->kind == NodeKind::Environment) {
    ValidateEnvironmentDeclaration(*incoming);
    owned_environment = std::make_shared<Environment>(*incoming->local_environment);
    owned_environment->parent_ = environment;
    mounted_environment = owned_environment;
  }
  ViewSpec compiled = CompileViewSpec(*incoming, mounted_environment, *state_->app_resources_);
  if (compiled.platform_view) {
    detail::BindExternalTextures(compiled.platform_view->properties, state_->platform_->external_texture_surface_);
  }
  auto mounted = std::make_unique<detail::MountedNode>();
  mounted->identity = state_->next_node_identity_++;
  mounted->owned_environment = std::move(owned_environment);
  ApplyViewDeclaration(*mounted, compiled, mounted_environment);
  if (mounted->kind == NodeKind::ScrollView || mounted->kind == NodeKind::VirtualLayout) {
    mounted->scroll_state = std::make_unique<ScrollNodeState>();
  }
  static_cast<void>(ReconcileNodeExtensions(*mounted, compiled.modifiers, false));
  BindExtensionInvalidation(*mounted);
  if (mounted->kind == NodeKind::Scope) {
    static_cast<void>(ComposeScope(*mounted));
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state = std::make_unique<VirtualNodeState>();
    mounted->virtual_state->source = incoming->virtual_items;
    std::shared_ptr<RecomposeScope> dependency_scope;
    if (Composer* composer = Composer::Current()) {
      dependency_scope = composer->Scope();
    } else if (VirtualItemDependencyCapture* capture = VirtualItemDependencyCapture::Current()) {
      dependency_scope = capture->Scope();
    }
    mounted->virtual_state->dependency_capture =
        std::make_unique<VirtualItemDependencyCapture>(std::move(dependency_scope), mounted->identity);
  } else {
    static_cast<void>(ReconcileChildren(mounted->children, incoming->children, mounted->environment));
  }
  return mounted;
}

bool Runtime::ReconcileChildren(
    std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children, const std::vector<View>& incoming_children,
    const std::shared_ptr<const Environment>& environment
) {
  std::unordered_set<ViewKey> incoming_keys;
  for (const auto& child_view : incoming_children) {
    if (!child_view.spec_ || !child_view.spec_->key.has_value()) {
      continue;
    }
    if (!incoming_keys.insert(*child_view.spec_->key).second) {
      throw std::logic_error("HuxerUI sibling views must not use duplicate keys");
    }
  }

  std::vector<std::unique_ptr<detail::MountedNode>> next;
  std::vector<std::optional<std::size_t>> origins;
  next.reserve(incoming_children.size());
  origins.reserve(incoming_children.size());
  auto previous = std::move(mounted_children);
  bool layout_changed = false;
  bool structure_changed = previous.size() != incoming_children.size();

  try {
    for (std::size_t index = 0; index < incoming_children.size(); ++index) {
      const auto& child_view = incoming_children[index];
      if (!child_view.spec_) {
        continue;
      }

      std::optional<std::size_t> origin;
      if (child_view.spec_->key.has_value()) {
        for (std::size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
          const auto& old_child = previous[previous_index];
          if (old_child && IsCompatibleNode(*old_child, *child_view.spec_) && old_child->key == child_view.spec_->key) {
            origin = previous_index;
            break;
          }
        }
      } else if (
          index < previous.size() && previous[index] && !previous[index]->key.has_value() &&
          IsCompatibleNode(*previous[index], *child_view.spec_)
      ) {
        origin = index;
      }

      if (origin.has_value()) {
        layout_changed = Reconcile(previous[*origin], child_view.spec_, environment) || layout_changed;
        layout_changed = *origin != next.size() || layout_changed;
        structure_changed = *origin != next.size() || structure_changed;
        next.push_back(std::move(previous[*origin]));
      } else {
        std::unique_ptr<detail::MountedNode> child;
        layout_changed = Reconcile(child, child_view.spec_, environment) || layout_changed;
        structure_changed = true;
        next.push_back(std::move(child));
      }
      origins.push_back(origin);
    }
  } catch (...) {
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (origins[index].has_value()) {
        previous[*origins[index]] = std::move(next[index]);
      }
    }
    mounted_children = std::move(previous);
    throw;
  }

  const bool removed = std::ranges::any_of(previous, [](const auto& child) { return child != nullptr; });
  layout_changed = previous.size() != next.size() || removed || layout_changed;
  structure_changed = previous.size() != next.size() || removed || structure_changed;
  mounted_children = std::move(next);
  state_->extension_tree_dirty_ = state_->extension_tree_dirty_ || structure_changed;
  return layout_changed;
}

bool Runtime::ReconcileLayerChildren(
    std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children,
    const std::vector<std::pair<View, std::shared_ptr<const Environment>>>& incoming_children
) {
  const auto declaration_snapshot = [](const View& view) {
    if (!view.spec_) {
      throw std::logic_error("HuxerUI LayerStack child is missing its entry identity");
    }
    const auto found = view.spec_->layout_values.find(typeid(LayerEntrySnapshotValue));
    const auto* snapshot =
        found == view.spec_->layout_values.end() ? nullptr : std::any_cast<LayerEntrySnapshot>(&found->second.value);
    if (!snapshot) {
      throw std::logic_error("HuxerUI LayerStack child is missing its entry identity");
    }
    return *snapshot;
  };

  std::vector<std::unique_ptr<detail::MountedNode>> next;
  std::vector<std::optional<std::size_t>> origins;
  next.reserve(incoming_children.size());
  origins.reserve(incoming_children.size());
  auto previous = std::move(mounted_children);
  bool layout_changed = previous.size() != incoming_children.size();
  bool structure_changed = layout_changed;

  try {
    for (std::size_t incoming_index = 0; incoming_index < incoming_children.size(); ++incoming_index) {
      const auto& [incoming, environment] = incoming_children[incoming_index];
      const LayerEntrySnapshot incoming_snapshot = declaration_snapshot(incoming);
      std::optional<std::size_t> origin;
      for (std::size_t index = 0; index < previous.size(); ++index) {
        const auto* snapshot = previous[index] ? previous[index]->LayoutValue<LayerEntrySnapshotValue>() : nullptr;
        if (snapshot != nullptr && snapshot->id == incoming_snapshot.id) {
          origin = index;
          break;
        }
      }

      if (!origin.has_value()) {
        std::unique_ptr<detail::MountedNode> child;
        layout_changed = Reconcile(child, incoming.spec_, environment) || layout_changed;
        next.push_back(std::move(child));
        structure_changed = true;
      } else if (previous[*origin]->LayoutValue<LayerEntrySnapshotValue>()->revision == incoming_snapshot.revision) {
        layout_changed = *origin != next.size() || layout_changed;
        structure_changed = *origin != next.size() || structure_changed;
        next.push_back(std::move(previous[*origin]));
      } else {
        layout_changed = Reconcile(previous[*origin], incoming.spec_, environment) || layout_changed;
        layout_changed = *origin != next.size() || layout_changed;
        structure_changed = *origin != next.size() || structure_changed;
        next.push_back(std::move(previous[*origin]));
      }
      origins.push_back(origin);
    }
  } catch (...) {
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (origins[index].has_value()) {
        previous[*origins[index]] = std::move(next[index]);
      }
    }
    mounted_children = std::move(previous);
    throw;
  }

  const bool removed = std::ranges::any_of(previous, [](const auto& child) { return child != nullptr; });
  layout_changed = removed || layout_changed;
  structure_changed = removed || structure_changed;
  mounted_children = std::move(next);
  state_->extension_tree_dirty_ = state_->extension_tree_dirty_ || structure_changed;
  return layout_changed;
}

} // namespace huxerui

namespace huxerui::detail {

VirtualMeasureSession::VirtualMeasureSession(Runtime& runtime, MountedNode& owner)
    : runtime_(&runtime), owner_(&owner), previous_nodes_(std::move(owner.children)),
      previous_realized_indices_(std::move(owner.virtual_state->realized_indices)) {
  previous_node_identities_.reserve(previous_nodes_.size());
  for (const auto& node : previous_nodes_) {
    previous_node_identities_.push_back(node ? node->identity : 0);
  }
}

VirtualMeasureSession::~VirtualMeasureSession() {
  if (!committed_) {
    RestoreOwner();
  }
}

VirtualItemState VirtualMeasureSession::CaptureItemState(MountedNode& mounted) {
  VirtualItemState state{
      mounted.kind,
      mounted.key,
      mounted.layout_descriptor,
      mounted.virtual_layout_descriptor,
      mounted.recompose_scope ? std::optional<StateSlotStorage>{mounted.recompose_scope->TakeStateSlots()}
                              : std::nullopt,
      {},
  };
  state.children.reserve(mounted.children.size());
  for (auto& child : mounted.children) {
    state.children.push_back(CaptureItemState(*child));
  }
  return state;
}

void VirtualMeasureSession::RestoreItemState(MountedNode& mounted, VirtualItemState& state) {
  if (!IsCompatibleVirtualItemState(mounted, state) || mounted.key != state.key) {
    return;
  }

  if (mounted.kind == NodeKind::Scope && state.state_slots) {
    mounted.recompose_scope = std::make_shared<RecomposeScope>(
        *runtime_,
        runtime_->state_->next_scope_identity_++,
        std::move(*state.state_slots)
    );
    runtime_->ComposeScope(mounted);
  }

  std::vector<bool> restored(state.children.size(), false);
  for (std::size_t index = 0; index < mounted.children.size(); ++index) {
    MountedNode& child = *mounted.children[index];
    VirtualItemState* child_state = nullptr;
    std::size_t state_index = 0;

    if (child.key.has_value()) {
      for (; state_index < state.children.size(); ++state_index) {
        if (!restored[state_index] && IsCompatibleVirtualItemState(child, state.children[state_index]) &&
            state.children[state_index].key == child.key) {
          child_state = &state.children[state_index];
          break;
        }
      }
    } else if (
        index < state.children.size() && !restored[index] && !state.children[index].key.has_value() &&
        IsCompatibleVirtualItemState(child, state.children[index])
    ) {
      state_index = index;
      child_state = &state.children[index];
    }

    if (child_state) {
      restored[state_index] = true;
      RestoreItemState(child, *child_state);
    }
  }
}

std::size_t VirtualMeasureSession::ItemCount() const noexcept {
  return owner_->virtual_state->source.size;
}

MountedNode& VirtualMeasureSession::Item(std::size_t index) {
  if (index >= ItemCount()) {
    throw std::out_of_range("HuxerUI virtual item index is out of range");
  }
  if (const auto found = requested_positions_by_index_.find(index); found != requested_positions_by_index_.end()) {
    return *requested_nodes_[found->second];
  }

  auto& state = *owner_->virtual_state;
  bool declaration_created = false;
  auto item_declaration = state.item_declarations.find(index);
  if (item_declaration == state.item_declarations.end()) {
    if (!state.source.factory) {
      throw std::logic_error("HuxerUI virtual item factory must not be empty");
    }
    item_declaration = state.item_declarations.emplace(index, state.source.factory(index)).first;
    declaration_created = true;
  }
  const View& item = item_declaration->second;
  if (!item.spec_) {
    throw std::logic_error("HuxerUI virtual item factory must return a non-empty view");
  }
  if (item.spec_->key.has_value() && !requested_item_keys_.insert(*item.spec_->key).second) {
    throw std::logic_error("HuxerUI mounted virtual items must not use duplicate keys");
  }

  std::unique_ptr<MountedNode> node;
  if (item.spec_->key.has_value()) {
    for (auto& previous : previous_nodes_) {
      if (previous && IsCompatibleNode(*previous, *item.spec_) && previous->key == item.spec_->key) {
        node = std::move(previous);
        break;
      }
    }
  } else {
    for (std::size_t position = 0; position < previous_nodes_.size(); ++position) {
      if (previous_nodes_[position] && !previous_nodes_[position]->key.has_value() &&
          position < previous_realized_indices_.size() && previous_realized_indices_[position] == index) {
        node = std::move(previous_nodes_[position]);
        break;
      }
    }
  }

  std::optional<VirtualItemState> retained_state;
  if (!node && state.item_state_cache) {
    if (item.spec_->key.has_value()) {
      const auto found = state.item_state_cache->keyed.find(*item.spec_->key);
      if (found != state.item_state_cache->keyed.end()) {
        retained_state.emplace(std::move(found->second));
        state.item_state_cache->keyed.erase(found);
      }
    } else {
      const auto found = state.item_state_cache->indexed.find(index);
      if (found != state.item_state_cache->indexed.end()) {
        retained_state.emplace(std::move(found->second));
        state.item_state_cache->indexed.erase(found);
      }
    }
    if (state.item_state_cache->keyed.empty() && state.item_state_cache->indexed.empty()) {
      state.item_state_cache.reset();
    }
  }

  if (!node || declaration_created) {
    VirtualItemDependencyCapture::Guard dependency_guard{*state.dependency_capture};
    runtime_->Reconcile(node, item.spec_, owner_->environment);
  }
  if (retained_state.has_value()) {
    RestoreItemState(*node, *retained_state);
  }

  const std::size_t position = requested_nodes_.size();
  requested_positions_by_index_.emplace(index, position);
  requested_nodes_.push_back(std::move(node));
  requested_item_indices_.push_back(index);
  return *requested_nodes_.back();
}

void VirtualMeasureSession::SaveUnmounted(std::unique_ptr<MountedNode> node, std::size_t index) {
  if (!node) {
    return;
  }
  VirtualItemState retained_state = CaptureItemState(*node);
  if (!ContainsStateSlots(retained_state)) {
    return;
  }

  auto& state = *owner_->virtual_state;
  if (!state.item_state_cache) {
    state.item_state_cache = std::make_unique<VirtualItemStateCache>();
  }
  if (node->key.has_value()) {
    state.item_state_cache->keyed.insert_or_assign(*node->key, std::move(retained_state));
  } else if (index < state.source.size) {
    state.item_state_cache->indexed.insert_or_assign(index, std::move(retained_state));
  }
}

void VirtualMeasureSession::CommitRealization(const std::vector<VirtualLayoutResult::Placement>& placements) {
  std::vector<std::unique_ptr<MountedNode>> next;
  std::vector<std::size_t> next_indices;
  next.reserve(placements.size());
  next_indices.reserve(placements.size());
  std::unordered_set<huxerui::MountedNode*> placed;

  for (const auto& placement : placements) {
    if (placement.item == nullptr || !placed.insert(placement.item).second) {
      throw std::logic_error("HuxerUI virtual layout must place each requested item at most once");
    }
    const auto found = std::find_if(requested_nodes_.begin(), requested_nodes_.end(), [&placement](const auto& item) {
      return item.get() == placement.item;
    });
    if (found == requested_nodes_.end()) {
      throw std::logic_error(
          "HuxerUI virtual layout can only place items "
          "requested from its context"
      );
    }
    const std::size_t position = static_cast<std::size_t>(found - requested_nodes_.begin());
    next.push_back(std::move(*found));
    next_indices.push_back(requested_item_indices_[position]);
  }

  for (std::size_t position = 0; position < requested_nodes_.size(); ++position) {
    if (requested_nodes_[position]) {
      owner_->virtual_state->item_declarations.erase(requested_item_indices_[position]);
      SaveUnmounted(std::move(requested_nodes_[position]), requested_item_indices_[position]);
    }
  }
  for (std::size_t position = 0; position < previous_nodes_.size(); ++position) {
    if (previous_nodes_[position]) {
      const std::size_t index = position < previous_realized_indices_.size() ? previous_realized_indices_[position] : 0;
      owner_->virtual_state->item_declarations.erase(index);
      SaveUnmounted(std::move(previous_nodes_[position]), index);
    }
  }

  bool structure_changed =
      next_indices != previous_realized_indices_ || next.size() != previous_node_identities_.size();
  if (!structure_changed) {
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (!next[index] || next[index]->identity != previous_node_identities_[index]) {
        structure_changed = true;
        break;
      }
    }
  }

  owner_->children = std::move(next);
  owner_->virtual_state->realized_indices = std::move(next_indices);
  runtime_->state_->extension_tree_dirty_ = runtime_->state_->extension_tree_dirty_ || structure_changed;
  committed_ = true;
}

void VirtualMeasureSession::RestoreOwner() noexcept {
  owner_->children.clear();
  owner_->virtual_state->realized_indices.clear();
  for (std::size_t position = 0; position < requested_nodes_.size(); ++position) {
    if (requested_nodes_[position]) {
      owner_->children.push_back(std::move(requested_nodes_[position]));
      owner_->virtual_state->realized_indices.push_back(requested_item_indices_[position]);
    }
  }
  for (std::size_t position = 0; position < previous_nodes_.size(); ++position) {
    if (previous_nodes_[position]) {
      owner_->children.push_back(std::move(previous_nodes_[position]));
      owner_->virtual_state->realized_indices.push_back(
          position < previous_realized_indices_.size() ? previous_realized_indices_[position] : 0
      );
    }
  }
}

} // namespace huxerui::detail
