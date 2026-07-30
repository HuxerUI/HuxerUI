#include "internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace huxerui::detail {

namespace {

struct RuntimeFillsViewport {
  using Value = bool;
};

class RuntimeRootLayout final : public huxerui::Layout<RuntimeRootLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, huxerui::MountedNode& node, Constraints constraints) {
    LayoutResult result;
    std::size_t index = 0;
    for (huxerui::MountedNode& child : node.Children()) {
      const bool tight = index == 0 || child.LayoutValueOr<RuntimeFillsViewport>(false);
      static_cast<void>(context.Measure(child, tight ? constraints : constraints.Loose()));
      result.Place(child, {});
      ++index;
    }
    result.SetSize(constraints.Constrain({
        constraints.max_width,
        constraints.max_height,
    }));
    return result;
  }
};

bool ContainsStateSlots(const SavedNodeState& saved) {
  if (saved.state_slots.has_value() && !saved.state_slots->slots.empty()) {
    return true;
  }
  return std::ranges::any_of(saved.children, ContainsStateSlots);
}

bool SameLayoutType(const LayoutDescriptor* left, const LayoutDescriptor* right) {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  return left->type == right->type;
}

bool SameVirtualLayoutType(const VirtualLayoutDescriptor* left, const VirtualLayoutDescriptor* right) {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  return left->type == right->type;
}

bool SameNodeType(const MountedNode& mounted, const ViewSpec& incoming) {
  return mounted.kind == incoming.kind && SameLayoutType(mounted.layout, incoming.layout) &&
         SameVirtualLayoutType(mounted.virtual_layout, incoming.virtual_layout);
}

bool SameSavedNodeType(const MountedNode& mounted, const SavedNodeState& saved) {
  return mounted.kind == saved.kind && SameLayoutType(mounted.layout, saved.layout) &&
         SameVirtualLayoutType(mounted.virtual_layout, saved.virtual_layout);
}

void ReconcileNodeExtensions(MountedNode& mounted, const std::vector<ModifierSpec>& incoming) {
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
  for (std::size_t index = 0; index < incoming.size(); ++index) {
    const ModifierSpec& spec = incoming[index];
    if (index < mounted.extensions.size() && mounted.extensions[index].descriptor == spec.descriptor) {
      if (!mounted.extensions[index].extension) {
        throw std::logic_error("HuxerUI node extension entry must not be empty");
      }
      continue;
    }
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
    spec.descriptor->update_extension(*mounted.extensions[index].extension, mounted, spec.value.get());
  }

  for (std::size_t index = 0; index < incoming.size(); ++index) {
    if (index < mounted.extensions.size() && mounted.extensions[index].descriptor == incoming[index].descriptor) {
      next.push_back(std::move(mounted.extensions[index]));
    } else {
      next.push_back({
          incoming[index].descriptor,
          std::move(created[index]),
      });
    }
  }
  mounted.extensions = std::move(next);
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

void ResolveEnabledTree(MountedNode& node, bool parent_enabled) {
  node.enabled = parent_enabled && node.local_enabled;
  for (auto& child : node.children) {
    ResolveEnabledTree(*child, node.enabled);
  }
}

void ResolveFocusedFlags(MountedNode& node, const std::optional<std::uint64_t>& focused_identity, bool focus_visible) {
  node.focused = focused_identity.has_value() && node.identity == *focused_identity;
  node.focus_visible = node.focused && focus_visible;
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

Runtime::Runtime(AppDefinition definition, PlatformHost& platform) {
  if (definition.root_factory == nullptr) {
    throw std::invalid_argument("HuxerUI root factory must not be null");
  }
  state_ = std::make_unique<State>(
      definition.root_factory,
      &platform,
      std::make_shared<RecomposeScope>(*this, 1),
      LayerController(*this)
  );
  RootContext root{
      state_->layer_controller_,
      state_->root_environment_values_,
      state_->root_service_types_,
      state_->root_services_
  };
  InstallBuiltinPresentation(root);
  for (RootHook& hook : definition.options.root_hooks) {
    if (!hook) {
      throw std::invalid_argument("HuxerUI root hook must not be empty");
    }
    hook(root);
  }
  state_->root_environment_ = std::make_shared<EnvironmentFrame>(EnvironmentFrame{
      nullptr,
      state_->root_environment_values_,
  });
}

Runtime::~Runtime() {
  try {
    StopTextInputSession(TextInputEndReason::RuntimeDestroyed);
  } catch (...) {
  }
  state_->pointer_sessions_.clear();
  state_->hovered_extension_.reset();
  state_->mounted_root_.reset();
  state_->layers_.clear();
  state_->root_environment_.reset();
  state_->root_environment_values_ = {};
  for (auto service = state_->root_services_.rbegin(); service != state_->root_services_.rend(); ++service) {
    service->reset();
  }
  state_->root_services_.clear();
  state_->layer_controller_.Disconnect();
}

LayerId
Runtime::AttachLayer(LayerOptions options, ViewFactory content, std::shared_ptr<const EnvironmentFrame> environment) {
  if (!content) {
    throw std::invalid_argument("HuxerUI layer content factory must not be empty");
  }
  const LayerId id = state_->next_layer_id_++;
  state_->layers_.push_back(
      LayerEntry{
          id,
          options,
          std::move(content),
          environment ? std::move(environment) : state_->root_environment_,
      }
  );
  if (!state_->composing_root_ || state_->layer_snapshot_taken_) {
    InvalidateRoot();
  }
  return id;
}

bool Runtime::UpdateLayer(LayerId id, ViewFactory content) {
  if (!content) {
    throw std::invalid_argument("HuxerUI layer content factory must not be empty");
  }
  const auto found = std::find_if(state_->layers_.begin(), state_->layers_.end(), [id](const LayerEntry& entry) {
    return entry.id == id;
  });
  if (found == state_->layers_.end()) {
    return false;
  }
  found->content = std::move(content);
  if (!state_->composing_root_ || state_->layer_snapshot_taken_) {
    InvalidateRoot();
  }
  return true;
}

bool Runtime::UpdateLayer(LayerId id, LayerOptions options, ViewFactory content) {
  if (!content) {
    throw std::invalid_argument("HuxerUI layer content factory must not be empty");
  }
  const auto found = std::find_if(state_->layers_.begin(), state_->layers_.end(), [id](const LayerEntry& entry) {
    return entry.id == id;
  });
  if (found == state_->layers_.end()) {
    return false;
  }
  found->options = std::move(options);
  found->content = std::move(content);
  if (!state_->composing_root_ || state_->layer_snapshot_taken_) {
    InvalidateRoot();
  }
  return true;
}

bool Runtime::DismissLayer(LayerId id) {
  const auto found = std::find_if(state_->layers_.begin(), state_->layers_.end(), [id](const LayerEntry& entry) {
    return entry.id == id;
  });
  if (found == state_->layers_.end()) {
    return false;
  }
  state_->layers_.erase(found);
  if (!state_->composing_root_ || state_->layer_snapshot_taken_) {
    InvalidateRoot();
  }
  return true;
}

void Runtime::SetViewport(Size viewport) {
  if (state_->viewport_.width == viewport.width && state_->viewport_.height == viewport.height) {
    return;
  }
  state_->viewport_ = viewport;
  RequestFrame();
}

void Runtime::RequestFrame() {
  const double now = state_->platform_->Now();
  if (!state_->frame_requested_ || state_->frame_request_deadline_ > now) {
    state_->frame_requested_ = true;
    state_->frame_request_deadline_ = now;
    state_->platform_->RequestFrame(0.0);
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
    state_->platform_->RequestFrame(delay_seconds);
  }
}

void Runtime::NotifyScrollActivity(detail::MountedNode& node) {
  DispatchScrollActivity(node);
  RequestFrame();
}

const DisplayList& Runtime::BuildFrame() {
  const double timestamp = state_->platform_->Now();
  const double delta_time = state_->previous_frame_timestamp_.has_value()
                                ? std::clamp(timestamp - *state_->previous_frame_timestamp_, 0.0, 0.25)
                                : 0.0;
  return BuildFrame({timestamp, delta_time});
}

const DisplayList& Runtime::BuildFrame(FrameInfo frame) {
  if (!std::isfinite(frame.timestamp)) {
    frame.timestamp = state_->platform_->Now();
  }
  if (!std::isfinite(frame.delta_time)) {
    frame.delta_time = 0.0;
  }
  frame.delta_time = std::clamp(frame.delta_time, 0.0, 0.25);
  state_->previous_frame_timestamp_ = frame.timestamp;
  state_->frame_requested_ = false;
  if (state_->composition_dirty_) {
    ComposeRoot();
  } else if (state_->mounted_root_) {
    RecomposeDirtyScopes(*state_->mounted_root_);
  }

  state_->display_list_.Clear();
  if (!state_->mounted_root_ || state_->viewport_.width <= 0.0F || state_->viewport_.height <= 0.0F) {
    RefreshInteractionTree();
    RefreshTextInputSession();
    return state_->display_list_;
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
  MeasureNode(*state_->mounted_root_, constraints, *state_->platform_, *this);
  LayoutNode(*state_->mounted_root_, {0.0F, 0.0F});
  if (BringTextInputIntoView()) {
    LayoutNode(*state_->mounted_root_, {0.0F, 0.0F});
  }
  RefreshInteractionTree();
  RefreshTextInputSession();
  AdvanceTextSelectionLongPress(frame.timestamp);

  std::optional<double> next_wakeup;
  UpdateNodeExtensions(*state_->mounted_root_, frame, needs_frame, next_wakeup, state_->extension_tree_dirty_);
  state_->extension_tree_dirty_ = false;
  AdvanceTextSelectionOverlay(frame);
  PaintNode(*state_->mounted_root_, state_->display_list_);
  PaintTextSelectionOverlay();
  if (needs_frame) {
    RequestFrame();
  } else if (next_wakeup.has_value()) {
    RequestFrameAfter(*next_wakeup);
  }
  return state_->display_list_;
}

const detail::MountedNode* Runtime::RootNode() const noexcept {
  return state_->has_application_root_ && state_->mounted_root_ && !state_->mounted_root_->children.empty()
             ? state_->mounted_root_->children.front().get()
             : nullptr;
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
  const std::optional<LayerId> modal_layer = ActiveModalLayerId();
  if (state_->active_modal_focus_layer_ != modal_layer) {
    if (state_->active_modal_focus_layer_.has_value()) {
      const bool previous_still_present = std::ranges::any_of(state_->layers_, [this](const LayerEntry& entry) {
        return entry.id == *state_->active_modal_focus_layer_;
      });
      if (!previous_still_present) {
        const auto restore = state_->modal_focus_restore_.find(*state_->active_modal_focus_layer_);
        if (restore != state_->modal_focus_restore_.end()) {
          SetFocusedNode(restore->second);
          state_->modal_focus_restore_.erase(restore);
        }
      }
    }
    if (modal_layer.has_value() && !state_->modal_focus_restore_.contains(*modal_layer)) {
      state_->modal_focus_restore_.insert_or_assign(*modal_layer, state_->focused_node_identity_);
    }
    state_->active_modal_focus_layer_ = modal_layer;
  }
  detail::MountedNode* modal_focus_root = ActiveModalFocusRoot();
  if (state_->focused_node_identity_.has_value()) {
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    if (!focused || !focused->enabled || !focused->focusable ||
        (modal_focus_root && !ContainsNodeIdentity(*modal_focus_root, focused->identity))) {
      SetFocusedNode(std::nullopt);
    }
  }
  if (modal_focus_root && !state_->focused_node_identity_.has_value()) {
    std::vector<detail::MountedNode*> modal_focusable;
    CollectFocusableNodes(*modal_focus_root, modal_focusable);
    if (!modal_focusable.empty()) {
      SetFocusedNode(modal_focusable.front()->identity);
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

detail::MountedNode* Runtime::ActiveModalFocusRoot() {
  if (!state_->mounted_root_) {
    return nullptr;
  }
  const std::optional<LayerId> modal_id = ActiveModalLayerId();
  if (!modal_id.has_value()) {
    return nullptr;
  }

  const std::string key = "__huxerui_layer_" + std::to_string(*modal_id);
  for (auto& child : state_->mounted_root_->children) {
    if (!child->key.has_value()) {
      continue;
    }
    const auto* value = std::get_if<std::string>(&*child->key);
    if (value && *value == key) {
      return child.get();
    }
  }
  return nullptr;
}

std::optional<LayerId> Runtime::ActiveModalLayerId() const {
  std::optional<LayerId> modal_id;
  for (const LayerEntry& entry : state_->layers_) {
    if (entry.options.input_policy == LayerInputPolicy::Modal) {
      modal_id = entry.id;
    }
  }
  return modal_id;
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
  detail::MountedNode* root = ActiveModalFocusRoot();
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

void Runtime::HandleScrollEvent(const ScrollEvent& event) {
  if (!state_->mounted_root_) {
    return;
  }
  ScrollEventResult result = ApplyScrollEvent(*state_->mounted_root_, event);
  for (detail::MountedNode* node : result.scroll_chain) {
    node->scroll->motion.Stop();
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

  const bool next_action =
      event.type == KeyEventType::Down && event.key == Key::Enter && !event.modifiers.shift &&
      !event.modifiers.control && !event.modifiers.alt && !event.modifiers.meta &&
      active.configuration.action == TextInputAction::Next;
  if (next_action && event.repeat) {
    return true;
  }
  if (active.client->HandleTextKey(event) != TextInputKeyResult::Handled) {
    return false;
  }

  RequestFrame();
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

  if (event.type == KeyEventType::Down) {
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
  state_->composition_dirty_ = true;
  RequestFrame();
}

void Runtime::InvalidateScope(std::uint64_t scope_id) {
  if (scope_id == state_->root_scope_->Id()) {
    state_->composition_dirty_ = true;
  }
  RequestFrame();
}

void Runtime::RecomposeDirtyScopes(detail::MountedNode& mounted) {
  if (mounted.kind == NodeKind::Scope && mounted.recompose_scope && mounted.recompose_scope->IsDirty()) {
    ComposeScope(mounted);
    return;
  }

  for (auto& child : mounted.children) {
    RecomposeDirtyScopes(*child);
  }
}

void Runtime::ComposeRoot() {
  state_->composing_root_ = true;
  state_->layer_snapshot_taken_ = false;
  state_->composition_dirty_ = false;
  bool scope_composing = false;
  bool children_split = false;
  bool application_reconciled = false;
  const bool previous_has_application = state_->has_application_root_;
  bool next_has_application = previous_has_application;
  std::vector<std::unique_ptr<detail::MountedNode>> application_nodes;
  std::vector<std::unique_ptr<detail::MountedNode>> layer_nodes;

  const auto restore_root_children = [&] {
    if (!children_split || !state_->mounted_root_) {
      return;
    }
    auto& children = state_->mounted_root_->children;
    children.clear();
    children.reserve(application_nodes.size() + layer_nodes.size());
    for (auto& node : application_nodes) {
      children.push_back(std::move(node));
    }
    for (auto& node : layer_nodes) {
      children.push_back(std::move(node));
    }
    children_split = false;
  };

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
    next_has_application = static_cast<bool>(application);

    if (!state_->mounted_root_) {
      View root = RuntimeRootLayout{};
      state_->mounted_root_ = Mount(root.spec_);
    }

    auto previous = std::move(state_->mounted_root_->children);
    if (previous_has_application && !previous.empty()) {
      application_nodes.push_back(std::move(previous.front()));
    }
    const std::size_t layer_start = previous_has_application && !previous.empty() ? 1 : 0;
    layer_nodes.reserve(previous.size() - layer_start);
    for (std::size_t index = layer_start; index < previous.size(); ++index) {
      layer_nodes.push_back(std::move(previous[index]));
    }
    children_split = true;

    std::vector<View> application_children;
    if (application) {
      application_children.push_back(std::move(application));
    }
    ReconcileChildren(application_nodes, application_children);
    application_reconciled = true;

    state_->layer_snapshot_taken_ = true;
    std::vector<const LayerEntry*> ordered_layers;
    ordered_layers.reserve(state_->layers_.size());
    for (const LayerEntry& entry : state_->layers_) {
      ordered_layers.push_back(&entry);
    }
    std::stable_sort(ordered_layers.begin(), ordered_layers.end(), [](const LayerEntry* left, const LayerEntry* right) {
      return left->options.kind < right->options.kind;
    });

    std::vector<View> layer_children;
    layer_children.reserve(ordered_layers.size());
    for (const LayerEntry* entry : ordered_layers) {
      auto environment = entry->environment ? entry->environment : state_->root_environment_;
      View layer = Scope([factory = entry->content, environment = std::move(environment)]() mutable {
        Composer::EnvironmentGuard guard{environment};
        return factory();
      });
      if (entry->options.input_policy == LayerInputPolicy::PassThrough) {
        layer.spec_->pointer_events_enabled = false;
      }
      if (entry->options.kind == LayerKind::Toast) {
        layer = Stack{std::move(layer)}
                    .With(
                        Padding{EdgeInsets{
                            0.0F,
                            16.0F,
                            24.0F,
                            16.0F,
                        }},
                        Align{
                            HorizontalAlignment::Center,
                            VerticalAlignment::End,
                        }
                    )
                    .LayoutValue<RuntimeFillsViewport>(true);
      }
      if (entry->options.input_policy == LayerInputPolicy::Modal) {
        layer = std::move(layer).On<ViewEvents::PointerDown>([](const PointerEvent&) {});
        View modal = Stack{std::move(layer)}
                         .With(
                             Align{
                                 HorizontalAlignment::Center,
                                 VerticalAlignment::Center,
                             }
                         )
                         .LayoutValue<RuntimeFillsViewport>(true)
                         .On<ViewEvents::PointerDown>([this,
                                                       id = entry->id,
                                                       dismiss = entry->options.dismiss_on_outside_press,
                                                       on_dismiss_request =
                                                           entry->options.on_dismiss_request](const PointerEvent&) {
                           if (dismiss) {
                             if (on_dismiss_request) {
                               on_dismiss_request();
                             } else {
                               DismissLayer(id);
                             }
                           }
                         });
        if (entry->options.modal_scrim.has_value()) {
          modal = std::move(modal).With(Background{*entry->options.modal_scrim});
        }
        layer = std::move(modal);
      }
      layer_children.push_back(std::move(layer).Key("__huxerui_layer_" + std::to_string(entry->id)));
    }

    ReconcileChildren(layer_nodes, layer_children);
    restore_root_children();
    state_->has_application_root_ = next_has_application;
    state_->composing_root_ = false;
    state_->layer_snapshot_taken_ = false;
  } catch (...) {
    if (scope_composing) {
      state_->root_scope_->AbortComposition();
    } else {
      state_->root_scope_->Invalidate();
    }
    restore_root_children();
    state_->has_application_root_ = application_reconciled ? next_has_application : previous_has_application;
    state_->composing_root_ = false;
    state_->layer_snapshot_taken_ = false;
    InvalidateRoot();
    throw;
  }
}

void Runtime::ComposeScope(detail::MountedNode& mounted) {
  if (!mounted.scope_factory) {
    mounted.children.clear();
    return;
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
    ReconcileChildren(mounted.children, children);
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

void Runtime::Reconcile(std::unique_ptr<detail::MountedNode>& mounted, const std::shared_ptr<ViewSpec>& incoming) {
  state_->extension_tree_dirty_ = true;
  const bool compatible = mounted && SameNodeType(*mounted, *incoming) && mounted->key == incoming->key;
  if (!compatible) {
    mounted = Mount(incoming);
    return;
  }

  mounted->text = incoming->text;
  mounted->style = incoming->style;
  mounted->scope_factory = incoming->scope_factory;
  mounted->layout = incoming->layout;
  mounted->virtual_layout = incoming->virtual_layout;
  mounted->layout_values = incoming->layout_values;
  mounted->event_bindings = incoming->event_bindings;
  mounted->activation = incoming->activation;
  mounted->environment = incoming->environment;
  mounted->pointer_events_enabled = incoming->pointer_events_enabled;
  mounted->local_enabled = incoming->local_enabled;
  mounted->focusable = incoming->focusable;
  ReconcileNodeExtensions(*mounted, incoming->modifiers);
  if (mounted->kind == NodeKind::Scope) {
    ComposeScope(*mounted);
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state->source = incoming->virtual_items;
    mounted->virtual_state->item_views.clear();
    mounted->virtual_state->source_dirty = true;
    if (mounted->virtual_state->saved_state) {
      std::erase_if(
          mounted->virtual_state->saved_state->indexed,
          [item_count = incoming->virtual_items.size](const auto& entry) { return entry.first >= item_count; }
      );
      if (mounted->virtual_state->saved_state->keyed.empty() && mounted->virtual_state->saved_state->indexed.empty()) {
        mounted->virtual_state->saved_state.reset();
      }
    }
  } else {
    ReconcileChildren(mounted->children, incoming->children);
  }
}

std::unique_ptr<detail::MountedNode> Runtime::Mount(const std::shared_ptr<ViewSpec>& incoming) {
  auto mounted = std::make_unique<detail::MountedNode>();
  mounted->kind = incoming->kind;
  mounted->identity = state_->next_node_identity_++;
  if (incoming->kind == NodeKind::ScrollView || incoming->kind == NodeKind::VirtualLayout) {
    mounted->scroll = std::make_unique<ScrollNodeState>();
  }
  mounted->key = incoming->key;
  mounted->text = incoming->text;
  mounted->style = incoming->style;
  mounted->scope_factory = incoming->scope_factory;
  mounted->layout = incoming->layout;
  mounted->virtual_layout = incoming->virtual_layout;
  mounted->layout_values = incoming->layout_values;
  mounted->event_bindings = incoming->event_bindings;
  mounted->activation = incoming->activation;
  mounted->environment = incoming->environment;
  mounted->pointer_events_enabled = incoming->pointer_events_enabled;
  mounted->local_enabled = incoming->local_enabled;
  mounted->focusable = incoming->focusable;
  ReconcileNodeExtensions(*mounted, incoming->modifiers);
  if (mounted->kind == NodeKind::Scope) {
    ComposeScope(*mounted);
  } else if (IsVirtualLayoutNode(*mounted)) {
    mounted->virtual_state = std::make_unique<VirtualNodeState>();
    mounted->virtual_state->source = incoming->virtual_items;
  } else {
    ReconcileChildren(mounted->children, incoming->children);
  }
  return mounted;
}

void Runtime::ReconcileChildren(
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
          if (old_child && SameNodeType(*old_child, *child_view.spec_) && old_child->key == child_view.spec_->key) {
            origin = previous_index;
            break;
          }
        }
      } else if (
          index < previous.size() && previous[index] && !previous[index]->key.has_value() &&
          SameNodeType(*previous[index], *child_view.spec_)
      ) {
        origin = index;
      }

      if (origin.has_value()) {
        Reconcile(previous[*origin], child_view.spec_);
        next.push_back(std::move(previous[*origin]));
      } else {
        std::unique_ptr<detail::MountedNode> candidate;
        Reconcile(candidate, child_view.spec_);
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

  mounted_children = std::move(next);
}

SavedNodeState Runtime::SaveNodeState(detail::MountedNode& mounted) {
  SavedNodeState saved{
      mounted.kind,
      mounted.key,
      mounted.layout,
      mounted.virtual_layout,
      mounted.recompose_scope ? std::optional<StateSlotStorage>{mounted.recompose_scope->TakeStateSlots()}
                              : std::nullopt,
      {},
  };
  saved.children.reserve(mounted.children.size());
  for (auto& child : mounted.children) {
    saved.children.push_back(SaveNodeState(*child));
  }
  return saved;
}

void Runtime::RestoreNodeState(detail::MountedNode& mounted, SavedNodeState& saved) {
  if (!SameSavedNodeType(mounted, saved) || mounted.key != saved.key) {
    return;
  }

  if (mounted.kind == NodeKind::Scope && saved.state_slots) {
    mounted.recompose_scope =
        std::make_shared<RecomposeScope>(*this, state_->next_scope_identity_++, std::move(*saved.state_slots));
    ComposeScope(mounted);
  }

  std::vector<bool> restored(saved.children.size(), false);
  for (std::size_t index = 0; index < mounted.children.size(); ++index) {
    detail::MountedNode& child = *mounted.children[index];
    SavedNodeState* saved_child = nullptr;
    std::size_t saved_index = 0;

    if (child.key.has_value()) {
      for (; saved_index < saved.children.size(); ++saved_index) {
        if (!restored[saved_index] && SameSavedNodeType(child, saved.children[saved_index]) &&
            saved.children[saved_index].key == child.key) {
          saved_child = &saved.children[saved_index];
          break;
        }
      }
    } else if (
        index < saved.children.size() && !restored[index] && !saved.children[index].key.has_value() &&
        SameSavedNodeType(child, saved.children[index])
    ) {
      saved_index = index;
      saved_child = &saved.children[index];
    }

    if (saved_child) {
      restored[saved_index] = true;
      RestoreNodeState(child, *saved_child);
    }
  }
}

} // namespace huxerui

namespace huxerui::detail {

VirtualMeasureSession::VirtualMeasureSession(Runtime& runtime, MountedNode& owner)
    : runtime_(&runtime), owner_(&owner), previous_(std::move(owner.children)),
      previous_indices_(std::move(owner.virtual_state->child_indices)) {
  previous_identities_.reserve(previous_.size());
  for (const auto& node : previous_) {
    previous_identities_.push_back(node ? node->identity : 0);
  }
}

VirtualMeasureSession::~VirtualMeasureSession() {
  if (!committed_) {
    RestoreOwner();
  }
}

std::size_t VirtualMeasureSession::ItemCount() const noexcept {
  return owner_->virtual_state->source.size;
}

MountedNode& VirtualMeasureSession::Item(std::size_t index) {
  if (index >= ItemCount()) {
    throw std::out_of_range("HuxerUI virtual item index is out of range");
  }
  if (const auto found = requested_positions_.find(index); found != requested_positions_.end()) {
    return *requested_[found->second];
  }

  auto& state = *owner_->virtual_state;
  auto item_view = state.item_views.find(index);
  if (item_view == state.item_views.end()) {
    if (!state.source.factory) {
      throw std::logic_error("HuxerUI virtual item factory must not be empty");
    }
    item_view = state.item_views.emplace(index, state.source.factory(index)).first;
  }
  const View& item = item_view->second;
  if (!item.spec_) {
    throw std::logic_error("HuxerUI virtual item factory must return a non-empty view");
  }
  if (item.spec_->key.has_value() && !requested_keys_.insert(*item.spec_->key).second) {
    throw std::logic_error("HuxerUI mounted virtual items must not use duplicate keys");
  }

  std::unique_ptr<MountedNode> candidate;
  if (item.spec_->key.has_value()) {
    for (auto& previous : previous_) {
      if (previous && SameNodeType(*previous, *item.spec_) && previous->key == item.spec_->key) {
        candidate = std::move(previous);
        break;
      }
    }
  } else {
    for (std::size_t position = 0; position < previous_.size(); ++position) {
      if (previous_[position] && !previous_[position]->key.has_value() && position < previous_indices_.size() &&
          previous_indices_[position] == index) {
        candidate = std::move(previous_[position]);
        break;
      }
    }
  }

  std::optional<SavedNodeState> saved;
  if (!candidate && state.saved_state) {
    if (item.spec_->key.has_value()) {
      const auto found = state.saved_state->keyed.find(*item.spec_->key);
      if (found != state.saved_state->keyed.end()) {
        saved.emplace(std::move(found->second));
        state.saved_state->keyed.erase(found);
      }
    } else {
      const auto found = state.saved_state->indexed.find(index);
      if (found != state.saved_state->indexed.end()) {
        saved.emplace(std::move(found->second));
        state.saved_state->indexed.erase(found);
      }
    }
    if (state.saved_state->keyed.empty() && state.saved_state->indexed.empty()) {
      state.saved_state.reset();
    }
  }

  if (!candidate || state.source_dirty) {
    runtime_->Reconcile(candidate, item.spec_);
  }
  if (saved.has_value()) {
    runtime_->RestoreNodeState(*candidate, *saved);
  }

  const std::size_t position = requested_.size();
  requested_positions_.emplace(index, position);
  requested_.push_back(std::move(candidate));
  requested_indices_.push_back(index);
  return *requested_.back();
}

void VirtualMeasureSession::SaveUnmounted(std::unique_ptr<MountedNode> node, std::size_t index) {
  if (!node) {
    return;
  }
  SavedNodeState saved = runtime_->SaveNodeState(*node);
  if (!ContainsStateSlots(saved)) {
    return;
  }

  auto& state = *owner_->virtual_state;
  if (!state.saved_state) {
    state.saved_state = std::make_unique<VirtualStateCache>();
  }
  if (node->key.has_value()) {
    state.saved_state->keyed.insert_or_assign(*node->key, std::move(saved));
  } else if (index < state.source.size) {
    state.saved_state->indexed.insert_or_assign(index, std::move(saved));
  }
}

void VirtualMeasureSession::Commit(const std::vector<VirtualLayoutResult::Placement>& placements) {
  std::vector<std::unique_ptr<MountedNode>> next;
  std::vector<std::size_t> next_indices;
  next.reserve(placements.size());
  next_indices.reserve(placements.size());
  std::unordered_set<huxerui::MountedNode*> placed;

  for (const auto& placement : placements) {
    if (placement.item == nullptr || !placed.insert(placement.item).second) {
      throw std::logic_error("HuxerUI virtual layout must place each requested item at most once");
    }
    const auto found = std::find_if(requested_.begin(), requested_.end(), [&placement](const auto& item) {
      return item.get() == placement.item;
    });
    if (found == requested_.end()) {
      throw std::logic_error(
          "HuxerUI virtual layout can only place items "
          "requested from its context"
      );
    }
    const std::size_t position = static_cast<std::size_t>(found - requested_.begin());
    next.push_back(std::move(*found));
    next_indices.push_back(requested_indices_[position]);
  }

  for (std::size_t position = 0; position < requested_.size(); ++position) {
    if (requested_[position]) {
      owner_->virtual_state->item_views.erase(requested_indices_[position]);
      SaveUnmounted(std::move(requested_[position]), requested_indices_[position]);
    }
  }
  for (std::size_t position = 0; position < previous_.size(); ++position) {
    if (previous_[position]) {
      const std::size_t index = position < previous_indices_.size() ? previous_indices_[position] : 0;
      owner_->virtual_state->item_views.erase(index);
      SaveUnmounted(std::move(previous_[position]), index);
    }
  }

  bool structure_changed = next_indices != previous_indices_ || next.size() != previous_identities_.size();
  if (!structure_changed) {
    for (std::size_t index = 0; index < next.size(); ++index) {
      if (!next[index] || next[index]->identity != previous_identities_[index]) {
        structure_changed = true;
        break;
      }
    }
  }

  owner_->children = std::move(next);
  owner_->virtual_state->child_indices = std::move(next_indices);
  owner_->virtual_state->source_dirty = false;
  runtime_->state_->extension_tree_dirty_ = runtime_->state_->extension_tree_dirty_ || structure_changed;
  committed_ = true;
}

void VirtualMeasureSession::RestoreOwner() noexcept {
  owner_->children.clear();
  owner_->virtual_state->child_indices.clear();
  for (std::size_t position = 0; position < requested_.size(); ++position) {
    if (requested_[position]) {
      owner_->children.push_back(std::move(requested_[position]));
      owner_->virtual_state->child_indices.push_back(requested_indices_[position]);
    }
  }
  for (std::size_t position = 0; position < previous_.size(); ++position) {
    if (previous_[position]) {
      owner_->children.push_back(std::move(previous_[position]));
      owner_->virtual_state->child_indices.push_back(
          position < previous_indices_.size() ? previous_indices_[position] : 0
      );
    }
  }
}

} // namespace huxerui::detail
