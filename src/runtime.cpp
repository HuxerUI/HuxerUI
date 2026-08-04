#include "internal.h"
#include "resource_internal.h"
#include "text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

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
      opacity_.Update(target_visible_ ? 1.0F : state_->hidden_opacity, target_visible_ ? state_->enter : state_->exit);
      if (target_visible_) {
        completion_sent_ = false;
      }
    }

    auto& mounted = static_cast<MountedNode&>(node);
    const bool running = opacity_.Advance(frame.timestamp, frame.delta_time, state_->reduced_motion);
    mounted.presentation.local_opacity *= opacity_.Value();
    if (!running && target_visible_) {
      state_->enter_on_mount = false;
    }
    if (!running && !target_visible_ && !completion_sent_) {
      completion_sent_ = true;
      if (state_->on_exit_complete) {
        state_->on_exit_complete();
      }
    }
    return {running, std::nullopt};
  }

private:
  std::shared_ptr<LayerTransitionState> state_;
  AnimatedValue<float> opacity_;
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

MountedNode* FindLayerStack(MountedNode& root) {
  const auto found =
      std::ranges::find_if(root.children, [](const auto& child) { return child && IsLayerStack(*child); });
  return found == root.children.end() ? nullptr : found->get();
}

MountedNode* FindLayerEntryNode(MountedNode& root, LayerId id) {
  MountedNode* layer_stack = FindLayerStack(root);
  if (!layer_stack) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(layer_stack->children, [id](const auto& child) {
    return child && child->template LayoutValueOr<LayerEntryIdValue>(0) == id;
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
      // Anchors use final presentation coordinates; LayerEntryLayout itself shares the host-view coordinate space.
      child_size = context.Measure(child, inset_constraints);
      LayerAnchorSide side = placement.preferred_side;
      const float anchor_right = placement.anchor.x + placement.anchor.width;
      const float anchor_bottom = placement.anchor.y + placement.anchor.height;
      const float gap = std::max(0.0F, placement.gap);
      const float below = viewport_height - vertical_margin - anchor_bottom - gap;
      const float above = placement.anchor.y - vertical_margin - gap;
      const float right = viewport_width - horizontal_margin - anchor_right - gap;
      const float left = placement.anchor.x - horizontal_margin - gap;
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
        child_offset.y = placement.anchor.y - child_size.height - gap;
        break;
      case LayerAnchorSide::Right:
        child_offset.x = anchor_right + gap;
        break;
      case LayerAnchorSide::Left:
        child_offset.x = placement.anchor.x - child_size.width - gap;
        break;
      }
      if (side == LayerAnchorSide::Below || side == LayerAnchorSide::Above) {
        switch (placement.alignment) {
        case LayerAnchorAlignment::Start:
          child_offset.x = placement.anchor.x;
          break;
        case LayerAnchorAlignment::Center:
          child_offset.x = placement.anchor.x + (placement.anchor.width - child_size.width) * 0.5F;
          break;
        case LayerAnchorAlignment::End:
          child_offset.x = anchor_right - child_size.width;
          break;
        }
      } else {
        switch (placement.alignment) {
        case LayerAnchorAlignment::Start:
          child_offset.y = placement.anchor.y;
          break;
        case LayerAnchorAlignment::Center:
          child_offset.y = placement.anchor.y + (placement.anchor.height - child_size.height) * 0.5F;
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
         IsCompatibleVirtualLayout(mounted.virtual_layout_descriptor, incoming.virtual_layout_descriptor);
}

bool IsCompatibleVirtualItemState(const MountedNode& mounted, const VirtualItemState& state) {
  return mounted.kind == state.kind && IsCompatibleLayout(mounted.layout_descriptor, state.layout_descriptor) &&
         IsCompatibleVirtualLayout(mounted.virtual_layout_descriptor, state.virtual_layout_descriptor);
}

bool ContentPaintInputsEqual(const MountedNode& mounted, const ViewSpec& incoming) {
  if (incoming.kind == NodeKind::Canvas) {
    return false;
  }
  return mounted.text == incoming.text && mounted.image_properties == incoming.image_properties &&
         mounted.properties.ContentPaintEquals(incoming.properties);
}

bool ForegroundPaintInputsEqual(const MountedNode& mounted, const ViewSpec& incoming) {
  return mounted.properties.ForegroundPaintEquals(incoming.properties);
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

bool LayoutInputsEqual(const MountedNode& mounted, const ViewSpec& incoming) {
  return mounted.text == incoming.text && mounted.image_properties.LayoutEquals(incoming.image_properties) &&
         mounted.properties.LayoutEquals(incoming.properties) &&
         LayoutValuesEquivalent(mounted.layout_values, incoming.layout_values);
}

bool ExtensionNodeInputsEqual(const MountedNode& mounted, const ViewSpec& incoming) {
  return mounted.text == incoming.text && mounted.image_properties == incoming.image_properties &&
         mounted.properties == incoming.properties &&
         LayoutValuesEquivalent(mounted.layout_values, incoming.layout_values) &&
         mounted.event_bindings == incoming.event_bindings && !mounted.activation && !incoming.activation &&
         mounted.environment == incoming.environment &&
         mounted.pointer_events_enabled == incoming.pointer_events_enabled &&
         mounted.local_enabled == incoming.local_enabled && mounted.focusable == incoming.focusable;
}

// Child structure, virtual sources, and retained modifiers reconcile separately because they carry mounted state.
void ApplyViewDeclaration(MountedNode& mounted, const ViewSpec& incoming) {
  mounted.kind = incoming.kind;
  mounted.key = incoming.key;
  mounted.text = incoming.text;
  mounted.properties = incoming.properties;
  mounted.scope_factory = incoming.scope_factory;
  mounted.canvas_painter = incoming.canvas_painter;
  mounted.image_properties = incoming.image_properties;
  mounted.layout_descriptor = incoming.layout_descriptor;
  mounted.virtual_layout_descriptor = incoming.virtual_layout_descriptor;
  mounted.layout_values = incoming.layout_values;
  mounted.event_bindings = incoming.event_bindings;
  mounted.activation = incoming.activation;
  mounted.environment = incoming.environment;
  mounted.pointer_events_enabled = incoming.pointer_events_enabled;
  mounted.local_enabled = incoming.local_enabled;
  mounted.focusable = incoming.focusable;
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

ModifierChanges
ReconcileNodeExtensions(MountedNode& mounted, const std::vector<ModifierSpec>& incoming, bool node_inputs_equal) {
  for (const ModifierSpec& spec : incoming) {
    if (spec.descriptor == nullptr || !spec.value) {
      throw std::logic_error("HuxerUI modifier descriptor and value must not be empty");
    }
    if (spec.descriptor->create_extension == nullptr) {
      throw std::logic_error("HuxerUI retained modifier descriptor must create a node extension");
    }
  }

  std::vector<std::unique_ptr<NodeExtension>> created(incoming.size());
  std::vector<NodeExtensionEntry> next;
  next.reserve(incoming.size());
  ModifierChanges changes{
      mounted.extensions.size() != incoming.size(),
      false,
      mounted.extensions.size() != incoming.size(),
  };
  for (std::size_t index = 0; index < incoming.size(); ++index) {
    const ModifierSpec& spec = incoming[index];
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

  for (std::size_t index = 0; index < incoming.size(); ++index) {
    const ModifierSpec& spec = incoming[index];
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
    const bool layout_equal = !spec.descriptor->layout_affecting ||
                              (spec.descriptor->layout_equals != nullptr && mounted.extensions[index].value &&
                               spec.descriptor->layout_equals(mounted.extensions[index].value.get(), spec.value.get()));
    changes.layout_changed = changes.layout_changed || !layout_equal;
    spec.descriptor->update_extension(*mounted.extensions[index].extension, mounted, spec.value.get());
  }

  for (std::size_t index = incoming.size(); index < mounted.extensions.size(); ++index) {
    if (mounted.extensions[index].descriptor != nullptr) {
      changes.layout_changed = changes.layout_changed || mounted.extensions[index].descriptor->layout_affecting;
    }
  }

  for (std::size_t index = 0; index < incoming.size(); ++index) {
    if (index < mounted.extensions.size() && mounted.extensions[index].descriptor == incoming[index].descriptor) {
      next.push_back(std::move(mounted.extensions[index]));
      next.back().value = incoming[index].value;
    } else {
      next.push_back({
          incoming[index].descriptor,
          std::move(created[index]),
          incoming[index].value,
      });
    }
  }
  mounted.extensions = std::move(next);
  return changes;
}

std::optional<NodeExtensionHandle> HitTestExtension(const std::vector<MountedNode*>& route, Point position) {
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if (!(*node)->enabled) {
      continue;
    }
    for (std::size_t index = (*node)->extensions.size(); index > 0; --index) {
      const auto local_position = (*node)->presentation.resolved_transform.Inverse(position);
      if (!local_position.has_value()) {
        continue;
      }
      NodeExtensionEntry& entry = (*node)->extensions[index - 1];
      if (entry.extension && entry.extension->HoverHitTest(**node, *local_position)) {
        return NodeExtensionHandle{
            (*node)->identity,
            index - 1,
            entry.descriptor,
        };
      }
    }
  }
  return std::nullopt;
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

void PrepareExtensionGeometry(MountedNode& node) {
  if (!node.subtree_has_extensions) {
    return;
  }
  for (NodeExtensionEntry& entry : node.extensions) {
    if (entry.extension && entry.extension->PrepareGeometry(node)) {
      node.foreground_paint_dirty = true;
    }
  }
  for (const std::unique_ptr<MountedNode>& child : node.children) {
    PrepareExtensionGeometry(*child);
  }
}

void ResolveEnabledTree(MountedNode& node, bool parent_enabled) {
  const bool enabled = parent_enabled && node.local_enabled;
  if (node.enabled != enabled) {
    node.foreground_paint_dirty = true;
  }
  node.enabled = enabled;
  for (auto& child : node.children) {
    ResolveEnabledTree(*child, node.enabled);
  }
}

void ResolveFocusedFlags(MountedNode& node, const std::optional<std::uint64_t>& focused_identity, bool focus_visible) {
  const bool focused = focused_identity.has_value() && node.identity == *focused_identity;
  const bool resolved_focus_visible = focused && focus_visible;
  if (node.focused != focused || node.focus_visible != resolved_focus_visible) {
    node.foreground_paint_dirty = true;
  }
  node.focused = focused;
  node.focus_visible = resolved_focus_visible;
  for (auto& child : node.children) {
    ResolveFocusedFlags(*child, focused_identity, focus_visible);
  }
}

void CollectFocusableNodes(MountedNode& node, std::vector<MountedNode*>& nodes) {
  if (node.enabled && node.focusable) {
    nodes.push_back(&node);
  }
  for (auto& child : node.children) {
    CollectFocusableNodes(*child, nodes);
  }
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
  const auto contains = [&root](const std::optional<std::uint64_t>& identity) {
    return identity.has_value() && ContainsNodeIdentity(root, *identity);
  };
  if (contains(session.target_identity) || contains(session.pending_focus_identity) ||
      contains(session.active_scroll_node)) {
    return true;
  }
  if (session.extension_capture.has_value() &&
      ContainsNodeIdentity(root, session.extension_capture->node_identity)) {
    return true;
  }
  return std::ranges::any_of(session.scroll_chain, [&root](std::uint64_t identity) {
           return ContainsNodeIdentity(root, identity);
         }) ||
         std::ranges::any_of(session.extension_observers, [&root](const NodeExtensionHandle& observer) {
           return ContainsNodeIdentity(root, observer.node_identity);
         });
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

Runtime::Runtime(AppDefinition definition, PlatformAdapter& platform) {
  if (definition.root_factory == nullptr) {
    throw std::invalid_argument("HuxerUI root factory must not be null");
  }
  state_ = std::make_unique<State>(
      definition.root_factory,
      &platform,
      std::make_shared<RecomposeScope>(*this, 1),
      LayerController(*this)
  );
  state_->root_environment_ = std::make_shared<Environment>();
  RootContext
      root{state_->layer_controller_, *state_->root_environment_, state_->root_service_types_, state_->root_services_};
  state_->app_resources_ = std::make_shared<AppResources>(platform.Resources());
  const ResourceConfiguration resource_configuration = state_->app_resources_->Configuration();
  state_->root_environment_->Set(resource_configuration.locale);
  root.Provide(state_->app_resources_);
  root.Provide(std::make_shared<TextMeasurerService>(TextMeasurerService{&platform}));
  InstallBuiltinPresentation(root);
  for (RootHook& hook : definition.options.root_hooks) {
    if (!hook) {
      throw std::invalid_argument("HuxerUI root hook must not be empty");
    }
    hook(root);
  }
  if (definition.options.show_debug_overlay) {
    state_->debug_metrics_ = std::make_shared<DebugMetricsState>(platform);
    InstallDebugOverlay(root, state_->debug_metrics_);
  }
}

Runtime::~Runtime() {
  try {
    StopTextInputSession(TextInputEndReason::RuntimeDestroyed);
  } catch (...) {
  }
  state_->pointer_sessions_.clear();
  state_->hovered_extension_.reset();
  state_->layer_controller_.Disconnect();
  state_->mounted_root_.reset();
  state_->root_environment_.reset();
  for (auto service = state_->root_services_.rbegin(); service != state_->root_services_.rend(); ++service) {
    service->reset();
  }
  state_->root_services_.clear();
}

void Runtime::SetViewport(Size viewport) {
  if (state_->viewport_ == viewport) {
    return;
  }
  state_->viewport_ = viewport;
  if (state_->text_selection_overlay_.state.visible) {
    state_->text_selection_overlay_.state.paint_dirty = true;
  }
  RequestFrame();
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

void Runtime::NotifyScrollActivity(detail::MountedNode& node) {
  DispatchScrollActivity(node);
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
  // Mutate the shared root so environments already captured by layers observe the new system locale.
  state_->root_environment_->Set(configuration.locale);
  for (LayerEntry& entry : state_->layer_controller_.state_->entries) {
    ++entry.revision;
  }
  InvalidateRoot();
  InvalidateLayers();
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
        state_->viewport_
    );
  };
  if (!std::isfinite(frame.timestamp)) {
    frame.timestamp = state_->platform_->Now();
  }
  if (!std::isfinite(frame.delta_time)) {
    frame.delta_time = 0.0;
  }
  frame.delta_time = std::clamp(frame.delta_time, 0.0, 0.25);
  state_->previous_frame_timestamp_ = frame.timestamp;
  state_->frame_requested_ = false;
  // Application and LayerStack composition are independent so transient presentation never executes the root factory.
  if (state_->application_dirty_) {
    ComposeApplication();
  }
  if (state_->layers_dirty_) {
    ComposeLayers();
  }
  if (state_->mounted_root_) {
    RecomposeDirtyScopes(*state_->mounted_root_);
  }

  if (!state_->mounted_root_ || state_->viewport_.width <= 0.0F || state_->viewport_.height <= 0.0F) {
    RefreshInteractionTree();
    RefreshTextInputSession();
    state_->frame_commit_.render_frame.scene.root = nullptr;
    state_->frame_commit_.render_frame.damage = {};
    state_->committed_scene_snapshot_.clear();
    state_->has_committed_scene_snapshot_ = false;
    ++state_->frame_commit_.render_frame.revision;
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
      state_->viewport_.width,
      state_->viewport_.width,
      state_->viewport_.height,
      state_->viewport_.height,
  };
  PropagateVirtualLayoutInvalidation(*state_->mounted_root_);
  MeasureNode(*state_->mounted_root_, constraints, *state_->platform_, *this);
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
      MeasureNode(*state_->mounted_root_, constraints, *state_->platform_, *this);
    }
    LayoutNode(*state_->mounted_root_, {0.0F, 0.0F});
    ResolvePresentationTree(*state_->mounted_root_);
  }
  // Text input clients prepare node-local geometry after the final layout, then the runtime converts it to host-view
  // coordinates while synchronizing the platform IME session.
  PrepareExtensionGeometry(*state_->mounted_root_);
  // Anchors can be nested inside other anchored layers. Settle the bounded dependency chain in this commit so a child
  // presentation does not retain geometry from its parent's previous placement.
  const std::size_t maximum_geometry_layout_passes = state_->layer_controller_.state_->entries.size() + 1;
  std::size_t geometry_layout_passes = 0;
  while (state_->mounted_root_->measure_dirty && geometry_layout_passes < maximum_geometry_layout_passes) {
    ++geometry_layout_passes;
    PropagateVirtualLayoutInvalidation(*state_->mounted_root_);
    MeasureNode(*state_->mounted_root_, constraints, *state_->platform_, *this);
    LayoutNode(*state_->mounted_root_, {0.0F, 0.0F});
    ResolvePresentationTree(*state_->mounted_root_);
    PrepareExtensionGeometry(*state_->mounted_root_);
  }
  if (state_->mounted_root_->measure_dirty) {
    RequestFrame();
  }
  RefreshTextInputSession();
  // A completed long press can focus a client and change its selection. Resolve it before building the shared overlay
  // so the handles and editing toolbar use the resulting selection geometry in this commit.
  AdvanceTextSelectionLongPress(frame.timestamp);

  AdvanceTextSelectionOverlay(frame);
  PaintTextSelectionOverlay();
  UpdateRenderScene(
      *state_->mounted_root_,
      state_->mounted_root_->bounds,
      &state_->text_selection_overlay_.render_node
  );
  state_->frame_commit_.render_frame.scene.root = &state_->mounted_root_->render_node;
  state_->frame_commit_.render_frame.damage = ComputeDamageRegion(
      state_->frame_commit_.render_frame.scene.root,
      state_->viewport_,
      state_->committed_scene_snapshot_,
      state_->committed_viewport_,
      state_->has_committed_scene_snapshot_
  );
  ++state_->frame_commit_.render_frame.revision;
  if (needs_frame) {
    RequestFrame();
  } else if (next_wakeup.has_value()) {
    RequestFrameAfter(*next_wakeup);
  }
  state_->frame_commit_.next_frame_deadline =
      state_->frame_requested_ ? std::optional{state_->frame_request_deadline_} : std::nullopt;
  record_debug_commit();
  state_->building_frame_ = false;
  return state_->frame_commit_;
}

const detail::MountedNode* Runtime::RootNode() const noexcept {
  if (!state_->mounted_root_) {
    return nullptr;
  }
  const detail::MountedNode* layer_stack = FindLayerStack(*state_->mounted_root_);
  for (const auto& child : state_->mounted_root_->children) {
    if (child.get() != layer_stack) {
      return child.get();
    }
  }
  return nullptr;
}

void Runtime::UpdateHoveredExtension(Point position) {
  std::vector<detail::MountedNode*> route;
  std::optional<NodeExtensionHandle> next_hovered;
  if (state_->mounted_root_ && BuildPointerRoute(*state_->mounted_root_, position, route)) {
    next_hovered = HitTestExtension(route, position);
  }

  if (state_->hovered_extension_ == next_hovered) {
    return;
  }
  if (state_->hovered_extension_.has_value() && state_->mounted_root_) {
    if (NodeExtension* previous = FindExtension(*state_->mounted_root_, *state_->hovered_extension_)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, state_->hovered_extension_->node_identity)) {
        previous->OnHoverChanged(*node, false);
      }
    }
  }
  state_->hovered_extension_ = next_hovered;
  if (state_->hovered_extension_.has_value() && state_->mounted_root_) {
    if (NodeExtension* next = FindExtension(*state_->mounted_root_, *state_->hovered_extension_)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, state_->hovered_extension_->node_identity)) {
        next->OnHoverChanged(*node, true);
      }
    }
  }
  RequestFrame();
}

void Runtime::RefreshInteractionTree() {
  if (!state_->mounted_root_) {
    state_->focused_node_identity_.reset();
    state_->focus_visible_ = false;
    state_->keyboard_activation_identity_.reset();
    state_->hovered_extension_.reset();
    return;
  }

  ResolveEnabledTree(*state_->mounted_root_, true);
  const std::optional<LayerId> focus_layer = ActiveFocusLayerId();
  const std::optional<LayerId> previous_focus_layer =
      state_->layer_focus_stack_.empty() ? std::nullopt : std::optional{state_->layer_focus_stack_.back().id};
  if (previous_focus_layer != focus_layer) {
    // Keep nested restoration frames until every higher focus layer leaves, even if a lower layer was removed first.
    const auto existing = focus_layer.has_value()
                              ? std::ranges::find(state_->layer_focus_stack_, *focus_layer, &LayerFocusFrame::id)
                              : state_->layer_focus_stack_.end();
    if (existing == state_->layer_focus_stack_.end() && focus_layer.has_value()) {
      state_->layer_focus_stack_.push_back({*focus_layer, state_->focused_node_identity_});
    } else {
      std::optional<std::uint64_t> restore_identity;
      while (!state_->layer_focus_stack_.empty() &&
             (!focus_layer.has_value() || state_->layer_focus_stack_.back().id != *focus_layer)) {
        restore_identity = state_->layer_focus_stack_.back().restore_identity;
        state_->layer_focus_stack_.pop_back();
      }
      SetFocusedNode(restore_identity);
    }
  }
  detail::MountedNode* focus_root = ActiveFocusLayerRoot();
  if (state_->focused_node_identity_.has_value()) {
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    if (!focused || !focused->enabled || !focused->focusable ||
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

  if (state_->hovered_extension_.has_value()) {
    detail::MountedNode* hovered = FindNode(*state_->mounted_root_, state_->hovered_extension_->node_identity);
    if (!hovered || !hovered->enabled) {
      if (hovered) {
        if (NodeExtension* extension = FindExtension(*state_->mounted_root_, *state_->hovered_extension_)) {
          extension->OnHoverChanged(*hovered, false);
        }
      }
      state_->hovered_extension_.reset();
    }
  }

  ResolveFocusedFlags(*state_->mounted_root_, state_->focused_node_identity_, state_->focus_visible_);
}

detail::MountedNode* Runtime::ActiveFocusLayerRoot() {
  if (!state_->mounted_root_ || state_->layer_focus_stack_.empty()) {
    return nullptr;
  }
  const LayerId focus_id = state_->layer_focus_stack_.back().id;

  const auto entry = std::ranges::find(state_->layer_controller_.state_->entries, focus_id, &LayerEntry::id);
  if (entry == state_->layer_controller_.state_->entries.end() ||
      (entry->transition && !entry->transition->target_visible)) {
    return nullptr;
  }

  detail::MountedNode* layer_stack = FindLayerStack(*state_->mounted_root_);
  if (!layer_stack) {
    return nullptr;
  }
  for (auto& child : layer_stack->children) {
    if (child->LayoutValueOr<LayerEntryIdValue>(0) == focus_id) {
      return child.get();
    }
  }
  return nullptr;
}

std::optional<std::uint64_t> Runtime::ResolvePointerFocusTarget(const std::vector<detail::MountedNode*>& route) {
  std::optional<std::uint64_t> candidate;
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if ((*node)->enabled && (*node)->focusable) {
      candidate = (*node)->identity;
      break;
    }
  }

  detail::MountedNode* focus_root = ActiveFocusLayerRoot();
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

std::optional<LayerId> Runtime::ActiveFocusLayerId() const {
  const LayerEntry* active = nullptr;
  for (const LayerEntry& entry : state_->layer_controller_.state_->entries) {
    if (entry.options.trap_focus && (active == nullptr || LayerPaintsAbove(entry, *active))) {
      active = &entry;
    }
  }
  return active == nullptr ? std::nullopt : std::optional{active->id};
}

bool Runtime::HandleTopLayerBack() {
  const LayerEntry* topmost = nullptr;
  for (const LayerEntry& entry : state_->layer_controller_.state_->entries) {
    if (entry.options.cancel_policy != LayerCancelPolicy::PassThrough &&
        (topmost == nullptr || LayerPaintsAbove(entry, *topmost))) {
      topmost = &entry;
    }
  }
  if (topmost == nullptr) {
    return false;
  }
  if (topmost->options.cancel_policy == LayerCancelPolicy::Consume) {
    return true;
  }
  const LayerId id = topmost->id;
  const std::function<void()> dismiss = topmost->options.on_dismiss_request;
  if (dismiss) {
    dismiss();
  } else {
    state_->layer_controller_.Dismiss(id);
  }
  return true;
}

bool Runtime::HandleBack() {
  if (state_->text_selection_overlay_.state.visible) {
    HideTextSelectionOverlay();
    RequestFrame();
    return true;
  }
  return HandleTopLayerBack();
}

void Runtime::SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible) {
  if (identity.has_value()) {
    if (!state_->mounted_root_) {
      identity.reset();
    } else {
      detail::MountedNode* candidate = FindNode(*state_->mounted_root_, *identity);
      if (!candidate || !candidate->enabled || !candidate->focusable) {
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
        focused->focus_visible = state_->focus_visible_;
        focused->foreground_paint_dirty = true;
      }
    }
    RequestFrame();
    return;
  }

  HideTextSelectionOverlay();
  if (state_->focused_node_identity_.has_value() && state_->mounted_root_) {
    if (detail::MountedNode* previous = FindNode(*state_->mounted_root_, *state_->focused_node_identity_)) {
      previous->focused = false;
      previous->focus_visible = false;
      previous->foreground_paint_dirty = true;
      DispatchFocusChanged(*previous, false);
    }
  }
  state_->keyboard_activation_identity_.reset();
  state_->focused_node_identity_ = identity;
  state_->focus_visible_ = next_focus_visible;
  if (state_->focused_node_identity_.has_value() && state_->mounted_root_) {
    if (detail::MountedNode* next = FindNode(*state_->mounted_root_, *state_->focused_node_identity_)) {
      next->focused = true;
      next->focus_visible = state_->focus_visible_;
      next->foreground_paint_dirty = true;
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
  detail::MountedNode* root = ActiveFocusLayerRoot();
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
  if (!rebuild_cache && !node.subtree_has_extensions) {
    return false;
  }

  node.presentation.local_transform = {};
  node.presentation.local_opacity = 1.0F;
  bool subtree_has_extensions = false;
  for (NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    subtree_has_extensions = true;
    const NodeExtension::FrameResult result = entry.extension->OnFrame(node, frame);
    needs_frame = needs_frame || result.needs_frame;
    if (result.wake_after.has_value() && (!next_wakeup.has_value() || *result.wake_after < *next_wakeup)) {
      next_wakeup = *result.wake_after;
    }
  }

  for (auto& child : node.children) {
    subtree_has_extensions =
        UpdateNodeExtensions(*child, frame, needs_frame, next_wakeup, rebuild_cache) || subtree_has_extensions;
  }
  node.subtree_has_extensions = subtree_has_extensions;
  return subtree_has_extensions;
}

void Runtime::BindExtensionPaintInvalidation(detail::MountedNode& node) {
  for (NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    entry.extension->BindPaintInvalidation([this, owner = &node] {
      owner->foreground_paint_dirty = true;
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
    NotifyScrollActivity(*node);
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
  if (!state_->layer_focus_stack_.empty() && ActiveFocusLayerRoot() == nullptr) {
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
  if (!focused || !focused->enabled || !focused->focusable) {
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
    } else if (activatable && event.key == Key::Space && !event.repeat) {
      state_->keyboard_activation_identity_ = focused->identity;
    }
  } else if (event.key == Key::Space) {
    if (activatable && state_->keyboard_activation_identity_.has_value() &&
        *state_->keyboard_activation_identity_ == focused->identity) {
      ActivateNode(*focused);
    }
    state_->keyboard_activation_identity_.reset();
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
    HandlePointerCancel(cancellation);
    TrackTouchTextSelectionGesture(cancellation);
  }

  if (state_->hovered_extension_.has_value() &&
      ContainsNodeIdentity(*layer, state_->hovered_extension_->node_identity)) {
    const detail::NodeExtensionHandle hovered = *state_->hovered_extension_;
    if (NodeExtension* extension = FindExtension(*state_->mounted_root_, hovered)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, hovered.node_identity)) {
        extension->OnHoverChanged(*node, false);
      }
    }
    state_->hovered_extension_.reset();
  }

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
    return ComposeScope(mounted);
  }

  bool layout_changed = false;
  for (auto& child : mounted.children) {
    layout_changed = RecomposeDirtyScopes(*child) || layout_changed;
  }
  if (layout_changed) {
    mounted.measure_dirty = true;
  }
  return layout_changed;
}

void Runtime::ComposeApplication() {
  state_->application_dirty_ = false;
  bool scope_composing = false;
  std::unique_ptr<detail::MountedNode> layer_stack;
  std::vector<std::unique_ptr<detail::MountedNode>> application_nodes;

  try {
    state_->root_scope_->BeginComposition();
    scope_composing = true;
    Composer composer{state_->root_scope_, state_->root_environment_};

    View application;
    {
      Composer::Guard guard{composer};
      application = state_->root_factory_();
    }

    state_->root_scope_->EndComposition();
    scope_composing = false;
    if (!state_->mounted_root_) {
      View root = RuntimeRootLayout{};
      state_->mounted_root_ = Mount(root.spec_);
      View stack = LayerStackLayout{};
      state_->mounted_root_->children.push_back(Mount(stack.spec_));
    }

    auto previous = std::move(state_->mounted_root_->children);
    for (auto& child : previous) {
      if (child && IsLayerStack(*child)) {
        layer_stack = std::move(child);
      } else if (child) {
        application_nodes.push_back(std::move(child));
      }
    }
    if (!layer_stack) {
      View stack = LayerStackLayout{};
      layer_stack = Mount(stack.spec_);
    }

    std::vector<View> application_children;
    if (application) {
      application_children.push_back(std::move(application));
    }
    const bool application_layout_changed = ReconcileChildren(application_nodes, application_children);
    auto& children = state_->mounted_root_->children;
    children.clear();
    children.reserve(application_nodes.size() + 1);
    for (auto& node : application_nodes) {
      children.push_back(std::move(node));
    }
    children.push_back(std::move(layer_stack));
    if (application_layout_changed) {
      state_->mounted_root_->measure_dirty = true;
    }
  } catch (...) {
    if (scope_composing) {
      state_->root_scope_->AbortComposition();
    } else {
      state_->root_scope_->Invalidate();
    }
    if (state_->mounted_root_ && layer_stack) {
      auto& children = state_->mounted_root_->children;
      children.clear();
      for (auto& node : application_nodes) {
        children.push_back(std::move(node));
      }
      children.push_back(std::move(layer_stack));
    }
    InvalidateRoot();
    throw;
  }
}

void Runtime::ComposeLayers() {
  state_->layers_dirty_ = false;
  if (!state_->mounted_root_) {
    View root = RuntimeRootLayout{};
    state_->mounted_root_ = Mount(root.spec_);
    View stack = LayerStackLayout{};
    state_->mounted_root_->children.push_back(Mount(stack.spec_));
  }
  detail::MountedNode* layer_stack = FindLayerStack(*state_->mounted_root_);
  if (!layer_stack) {
    throw std::logic_error("HuxerUI RuntimeRoot is missing its LayerStack");
  }

  try {
    // Factories may mutate the controller while composing; those mutations mark layers dirty for the next frame.
    const std::vector<LayerEntry> ordered = OrderedLayerEntries(state_->layer_controller_.state_->entries);
    std::vector<View> layer_children;
    layer_children.reserve(ordered.size());
    for (const LayerEntry& entry : ordered) {
      auto environment = entry.environment ? entry.environment : state_->root_environment_;
      View content = Scope([factory = entry.content, environment = std::move(environment)]() mutable {
        Composer::EnvironmentGuard guard{environment};
        return factory();
      });
      const bool barrier = entry.options.pointer_policy == LayerPointerPolicy::Barrier;
      const bool exiting = entry.transition && !entry.transition->target_visible;
      if (exiting) {
        content.spec_->pointer_events_enabled = false;
      }
      if (barrier) {
        content = std::move(content).On<ViewEvents::PointerDown>([](const PointerEvent&) {});
      }

      View layer = LayerEntryLayout{std::move(content)}
                       .LayoutValue<LayerPlacementValue>(entry.placement)
                       .LayoutValue<LayerEntryIdValue>(entry.id)
                       .LayoutValue<LayerEntryRevisionValue>(entry.revision);
      if (barrier) {
        layer =
            std::move(layer).On<ViewEvents::PointerDown>([controller = state_->layer_controller_,
                                                          id = entry.id,
                                                          dismiss = entry.options.dismiss_on_outside_press && !exiting,
                                                          on_dismiss_request =
                                                              entry.options.on_dismiss_request](const PointerEvent&) {
              if (!dismiss) {
                return;
              }
              if (on_dismiss_request) {
                on_dismiss_request();
              } else {
                controller.Dismiss(id);
              }
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
      layer_children.push_back(std::move(layer));
    }

    if (ReconcileLayerChildren(layer_stack->children, layer_children)) {
      layer_stack->measure_dirty = true;
      state_->mounted_root_->measure_dirty = true;
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

    View content;
    {
      Composer::Guard guard{composer};
      content = mounted.scope_factory();
    }

    mounted.recompose_scope->EndComposition();
    scope_composing = false;

    std::vector<View> children;
    if (content) {
      children.push_back(std::move(content));
    }
    const bool layout_changed = ReconcileChildren(mounted.children, children);
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

bool Runtime::Reconcile(std::unique_ptr<detail::MountedNode>& mounted, const std::shared_ptr<ViewSpec>& incoming) {
  if (state_->text_selection_overlay_.state.visible) {
    state_->text_selection_overlay_.state.paint_dirty = true;
  }
  const bool compatible = mounted && IsCompatibleNode(*mounted, *incoming) && mounted->key == incoming->key;
  if (!compatible) {
    state_->extension_tree_dirty_ = true;
    mounted = Mount(incoming);
    return true;
  }

  bool layout_changed = !LayoutInputsEqual(*mounted, *incoming);
  if (!ContentPaintInputsEqual(*mounted, *incoming)) {
    mounted->content_paint_dirty = true;
  }
  if (!ForegroundPaintInputsEqual(*mounted, *incoming)) {
    mounted->foreground_paint_dirty = true;
  }
  const bool extension_node_inputs_equal = ExtensionNodeInputsEqual(*mounted, *incoming);
  ApplyViewDeclaration(*mounted, *incoming);
  const ModifierChanges modifier_changes =
      ReconcileNodeExtensions(*mounted, incoming->retained_modifiers, extension_node_inputs_equal);
  layout_changed = layout_changed || modifier_changes.layout_changed;
  if (modifier_changes.changed) {
    mounted->foreground_paint_dirty = true;
  }
  if (modifier_changes.structure_changed) {
    state_->extension_tree_dirty_ = true;
    BindExtensionPaintInvalidation(*mounted);
  }
  if (mounted->kind == NodeKind::Scope) {
    layout_changed = ComposeScope(*mounted) || layout_changed;
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state->source = incoming->virtual_items;
    mounted->virtual_state->item_declarations.clear();
    mounted->virtual_state->source_dirty = true;
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
    layout_changed = ReconcileChildren(mounted->children, incoming->children) || layout_changed;
  }
  mounted->measure_dirty = mounted->measure_dirty || layout_changed;
  return layout_changed;
}

std::unique_ptr<detail::MountedNode> Runtime::Mount(const std::shared_ptr<ViewSpec>& incoming) {
  auto mounted = std::make_unique<detail::MountedNode>();
  mounted->identity = state_->next_node_identity_++;
  ApplyViewDeclaration(*mounted, *incoming);
  if (mounted->kind == NodeKind::ScrollView || mounted->kind == NodeKind::VirtualLayout) {
    mounted->scroll_state = std::make_unique<ScrollNodeState>();
  }
  static_cast<void>(ReconcileNodeExtensions(*mounted, incoming->retained_modifiers, false));
  BindExtensionPaintInvalidation(*mounted);
  if (mounted->kind == NodeKind::Scope) {
    static_cast<void>(ComposeScope(*mounted));
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state = std::make_unique<VirtualNodeState>();
    mounted->virtual_state->source = incoming->virtual_items;
  } else {
    static_cast<void>(ReconcileChildren(mounted->children, incoming->children));
  }
  return mounted;
}

bool Runtime::ReconcileChildren(
    std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children, const std::vector<View>& incoming_children
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
        layout_changed = Reconcile(previous[*origin], child_view.spec_) || layout_changed;
        layout_changed = *origin != next.size() || layout_changed;
        structure_changed = *origin != next.size() || structure_changed;
        next.push_back(std::move(previous[*origin]));
      } else {
        std::unique_ptr<detail::MountedNode> candidate;
        layout_changed = Reconcile(candidate, child_view.spec_) || layout_changed;
        structure_changed = true;
        next.push_back(std::move(candidate));
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
    std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children, const std::vector<View>& incoming_children
) {
  const auto declaration_value = [](const View& view, std::type_index key) -> const std::any* {
    if (!view.spec_) {
      return nullptr;
    }
    const auto found = view.spec_->layout_values.find(key);
    return found == view.spec_->layout_values.end() ? nullptr : &found->second.value;
  };
  const auto declaration_id = [&declaration_value](const View& view) {
    const std::any* value = declaration_value(view, typeid(LayerEntryIdValue));
    const auto* id = value ? std::any_cast<LayerId>(value) : nullptr;
    if (!id) {
      throw std::logic_error("HuxerUI LayerStack child is missing its entry identity");
    }
    return *id;
  };
  const auto declaration_revision = [&declaration_value](const View& view) {
    const std::any* value = declaration_value(view, typeid(LayerEntryRevisionValue));
    const auto* revision = value ? std::any_cast<std::uint64_t>(value) : nullptr;
    if (!revision) {
      throw std::logic_error("HuxerUI LayerStack child is missing its declaration revision");
    }
    return *revision;
  };

  std::vector<std::unique_ptr<detail::MountedNode>> next;
  std::vector<std::optional<std::size_t>> origins;
  next.reserve(incoming_children.size());
  origins.reserve(incoming_children.size());
  auto previous = std::move(mounted_children);
  bool layout_changed = previous.size() != incoming_children.size();
  bool structure_changed = layout_changed;

  try {
    for (const View& incoming : incoming_children) {
      const LayerId id = declaration_id(incoming);
      const std::uint64_t revision = declaration_revision(incoming);
      std::optional<std::size_t> origin;
      for (std::size_t index = 0; index < previous.size(); ++index) {
        if (previous[index] && previous[index]->LayoutValueOr<LayerEntryIdValue>(0) == id) {
          origin = index;
          break;
        }
      }

      if (!origin.has_value()) {
        std::unique_ptr<detail::MountedNode> candidate;
        layout_changed = Reconcile(candidate, incoming.spec_) || layout_changed;
        next.push_back(std::move(candidate));
        structure_changed = true;
      } else if (previous[*origin]->LayoutValueOr<LayerEntryRevisionValue>(0) == revision) {
        layout_changed = *origin != next.size() || layout_changed;
        structure_changed = *origin != next.size() || structure_changed;
        next.push_back(std::move(previous[*origin]));
      } else {
        layout_changed = Reconcile(previous[*origin], incoming.spec_) || layout_changed;
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
  auto item_declaration = state.item_declarations.find(index);
  if (item_declaration == state.item_declarations.end()) {
    if (!state.source.factory) {
      throw std::logic_error("HuxerUI virtual item factory must not be empty");
    }
    item_declaration = state.item_declarations.emplace(index, state.source.factory(index)).first;
  }
  const View& item = item_declaration->second;
  if (!item.spec_) {
    throw std::logic_error("HuxerUI virtual item factory must return a non-empty view");
  }
  if (item.spec_->key.has_value() && !requested_item_keys_.insert(*item.spec_->key).second) {
    throw std::logic_error("HuxerUI mounted virtual items must not use duplicate keys");
  }

  std::unique_ptr<MountedNode> candidate;
  if (item.spec_->key.has_value()) {
    for (auto& previous : previous_nodes_) {
      if (previous && IsCompatibleNode(*previous, *item.spec_) && previous->key == item.spec_->key) {
        candidate = std::move(previous);
        break;
      }
    }
  } else {
    for (std::size_t position = 0; position < previous_nodes_.size(); ++position) {
      if (previous_nodes_[position] && !previous_nodes_[position]->key.has_value() &&
          position < previous_realized_indices_.size() && previous_realized_indices_[position] == index) {
        candidate = std::move(previous_nodes_[position]);
        break;
      }
    }
  }

  std::optional<VirtualItemState> retained_state;
  if (!candidate && state.item_state_cache) {
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

  if (!candidate || state.source_dirty) {
    runtime_->Reconcile(candidate, item.spec_);
  }
  if (retained_state.has_value()) {
    RestoreItemState(*candidate, *retained_state);
  }

  const std::size_t position = requested_nodes_.size();
  requested_positions_by_index_.emplace(index, position);
  requested_nodes_.push_back(std::move(candidate));
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
  owner_->virtual_state->source_dirty = false;
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
