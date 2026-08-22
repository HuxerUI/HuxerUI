#include "internal.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace huxerui::detail {

namespace {

bool HandlesPointerEvents(const MountedNode& node) {
  return static_cast<bool>(node.activation) || HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerDown>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerMove>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerUp>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerCancel>(node.event_bindings);
}

float PointerDelta(Point previous, Point current, Axis axis) {
  return axis == Axis::Vertical ? previous.y - current.y : previous.x - current.x;
}

bool SupportsHover(PointerDeviceKind device_kind) {
  return device_kind == PointerDeviceKind::Mouse || device_kind == PointerDeviceKind::Pen;
}

void RecordScrollVelocity(PointerSession& session, float delta, double timestamp) {
  const double elapsed = timestamp - session.velocity_sample_timestamp;
  session.velocity_sample_timestamp = timestamp;
  if (!std::isfinite(elapsed) || elapsed <= 0.0 || elapsed > 0.15) {
    session.scroll_velocity = 0.0F;
    session.has_velocity_sample = false;
    return;
  }
  const float sample = delta / static_cast<float>(elapsed);
  session.scroll_velocity = session.has_velocity_sample ? session.scroll_velocity * 0.25F + sample * 0.75F : sample;
  session.has_velocity_sample = true;
}

void SetScrollGesture(MountedNode& node, bool active) {
  for (NodeExtensionEntry& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->OnScrollGesture(node, active);
    }
  }
}

PointerEvent LocalPointerEvent(const MountedNode& node, const PointerEvent& event) {
  PointerEvent local = event;
  if (const auto position = node.presentation.resolved_transform.Inverse(event.position)) {
    local.position = *position;
  } else {
    local.position = {
        node.bounds.x + node.bounds.width * 0.5F,
        node.bounds.y + node.bounds.height * 0.5F,
    };
  }
  return local;
}

} // namespace

void UpdateInteraction(MountedNode& node, InteractionState state, std::optional<InteractionEvent> event) {
  if (node.interaction == state && !event.has_value()) {
    return;
  }
  node.interaction = state;
  for (NodeExtensionEntry& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->OnInteraction(node, node.interaction, event);
    }
  }
}

} // namespace huxerui::detail

namespace huxerui {

using namespace detail;

detail::MountedNode* Runtime::FindNode(detail::MountedNode& node, std::uint64_t identity) {
  if (node.identity == identity) {
    return &node;
  }
  for (auto& child : node.children) {
    if (detail::MountedNode* found = FindNode(*child, identity)) {
      return found;
    }
  }
  return nullptr;
}

NodeExtension* Runtime::FindExtension(detail::MountedNode& root, const NodeExtensionHandle& handle) {
  detail::MountedNode* node = FindNode(root, handle.node_identity);
  if (!node || handle.extension_index >= node->extensions.size()) {
    return nullptr;
  }
  NodeExtensionEntry& entry = node->extensions[handle.extension_index];
  if (entry.descriptor != handle.descriptor || !entry.extension) {
    return nullptr;
  }
  return entry.extension.get();
}

void Runtime::ActivateNode(detail::MountedNode& node) {
  if (node.activation) {
    node.activation(node.event_bindings);
  } else {
    EmitEvent<ViewEvents::Click>(node.event_bindings);
  }
}

std::uint64_t Runtime::BeginInteraction(detail::MountedNode& node, InteractionEvent::Source source,
                                        std::optional<Point> position) {
  const std::uint64_t press_id = state_->next_press_id_++;
  ++node.active_press_count;
  InteractionState interaction = node.interaction;
  interaction.pressed = true;
  UpdateInteraction(node, interaction, InteractionEvent{InteractionEvent::Type::Press, source, press_id, position});
  return press_id;
}

void Runtime::EndInteraction(detail::MountedNode& node, InteractionEvent::Type type,
                             InteractionEvent::Source source, std::uint64_t press_id,
                             std::optional<Point> position) {
  if (node.active_press_count > 0) {
    --node.active_press_count;
  }
  InteractionState interaction = node.interaction;
  interaction.pressed = node.active_press_count != 0;
  UpdateInteraction(node, interaction, InteractionEvent{type, source, press_id, position});
}

void Runtime::BeginPointerInteraction(PointerSession& session, std::uint64_t node_identity,
                                      const PointerEvent& event) {
  detail::MountedNode* node = FindNode(*state_->mounted_root_, node_identity);
  if (node == nullptr || !node->interaction.enabled) {
    return;
  }
  const PointerEvent local = LocalPointerEvent(*node, event);
  session.interaction = detail::ActivePointerInteraction{
      node_identity,
      BeginInteraction(*node, InteractionEvent::Source::Pointer, local.position),
  };
}

void Runtime::EndPointerInteraction(PointerSession& session, InteractionEvent::Type type,
                                    const PointerEvent& event) {
  if (!session.interaction.has_value()) {
    return;
  }
  const detail::ActivePointerInteraction interaction = *session.interaction;
  session.interaction.reset();
  if (detail::MountedNode* node = FindNode(*state_->mounted_root_, interaction.node_identity)) {
    const PointerEvent local = LocalPointerEvent(*node, event);
    EndInteraction(*node, type, InteractionEvent::Source::Pointer, interaction.press_id, local.position);
  }
}

void Runtime::CancelPointerTarget(PointerSession& session, const PointerEvent& event) {
  EndPointerInteraction(session, InteractionEvent::Type::Cancel, event);
  if (session.target_identity.has_value()) {
    if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *session.target_identity)) {
      PointerEvent cancel = event;
      cancel.type = PointerEventType::Cancel;
      EmitEvent<ViewEvents::PointerCancel>(target->event_bindings, cancel);
    }
    session.target_identity.reset();
  }
  session.focus_pending = false;
  session.pending_focus_identity.reset();
  session.scroll_chain.clear();
  RequestFrame();
}

void Runtime::ReleaseScrollGesture(PointerSession& session) {
  if (!session.active_scroll_node.has_value()) {
    return;
  }
  if (detail::MountedNode* node = FindNode(*state_->mounted_root_, *session.active_scroll_node)) {
    SetScrollGesture(*node, false);
  }
  session.active_scroll_node.reset();
}

bool Runtime::DispatchExtensionObservers(PointerSession& session, const PointerEvent& event, bool clear) {
  bool cancel_target = false;
  for (const NodeExtensionHandle& observer : session.extension_observers) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, observer);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, observer.node_identity);
    if (extension && node) {
      cancel_target = extension->OnPointer(*node, LocalPointerEvent(*node, event)) ==
                          NodeExtension::PointerResult::CancelTarget ||
                      cancel_target;
    }
  }
  if (clear) {
    session.extension_observers.clear();
  }
  return cancel_target;
}

std::optional<std::size_t> Runtime::FindScrollCandidate(const PointerSession& session, Axis axis, float delta) {
  for (std::size_t index = 0; index < session.scroll_chain.size(); ++index) {
    detail::MountedNode* candidate = FindNode(*state_->mounted_root_, session.scroll_chain[index]);
    if (candidate && candidate->interaction.enabled && ScrollAxis(*candidate) == axis &&
        CanScrollNode(*candidate, delta)) {
      return index;
    }
  }
  return std::nullopt;
}

std::vector<detail::MountedNode*> Runtime::ApplyDragScroll(PointerSession& session, float delta) {
  if (!session.drag_axis.has_value() || delta == 0.0F) {
    return {};
  }

  std::vector<detail::MountedNode*> changed;
  float remaining = delta;
  for (std::size_t index = session.active_scroll; index < session.scroll_chain.size(); ++index) {
    detail::MountedNode* candidate = FindNode(*state_->mounted_root_, session.scroll_chain[index]);
    if (!candidate || !candidate->interaction.enabled || ScrollAxis(*candidate) != *session.drag_axis) {
      continue;
    }
    const float consumed = ScrollNodeBy(*candidate, remaining);
    if (consumed != 0.0F) {
      changed.push_back(candidate);
      session.active_scroll = index;
      remaining -= consumed;
    }
    if (std::abs(remaining) < 0.001F) {
      break;
    }
  }
  return changed;
}

void Runtime::HandlePointerEvent(const PointerEvent& event) {
  if (!state_->mounted_root_) {
    return;
  }
  if (HandleTextSelectionOverlayPointer(event)) {
    RefreshTextInputSession();
    return;
  }

  switch (event.type) {
  case PointerEventType::Down:
    HandlePointerDown(event);
    break;
  case PointerEventType::Move:
    HandlePointerMove(event);
    break;
  case PointerEventType::Cancel:
    HandlePointerCancel(event);
    break;
  case PointerEventType::Up:
    HandlePointerUp(event);
    break;
  }
  HandleTextSelectionClick(event);
  TrackTouchTextSelectionGesture(event);
  RefreshTextInputSession();
}

bool Runtime::CommitPendingTouchFocus(PointerSession& session, Point position, bool record_tap) {
  if (!session.focus_pending) {
    return false;
  }
  session.focus_pending = false;
  const std::optional<std::uint64_t> pending = std::exchange(session.pending_focus_identity, std::nullopt);

  std::vector<detail::MountedNode*> route;
  const std::optional<std::uint64_t> released =
      state_->mounted_root_ && BuildPointerRoute(*state_->mounted_root_, position, route)
          ? ResolvePointerFocusTarget(route)
          : std::nullopt;
  if (released != pending) {
    return false;
  }

  std::optional<TextInputSessionId> session_to_show;
  if (pending.has_value() && state_->focused_node_identity_ == pending && state_->text_input_session_.has_value() &&
      state_->text_input_session_->node_identity == *pending) {
    session_to_show = state_->text_input_session_->session_id;
  }
  SetFocusedNode(pending, false);
  if (record_tap && pending.has_value()) {
    if (detail::MountedNode* focused = FindNode(*state_->mounted_root_, *pending);
        focused && detail::FindTextSelectionClient(*focused)) {
      state_->text_selection_gesture_.previous_tap_time = state_->platform_->Now();
      state_->text_selection_gesture_.previous_tap_position = position;
      state_->text_selection_gesture_.previous_tap_node = pending;
    }
  }
  if (session_to_show.has_value()) {
    if (PlatformTextInput* text_input = state_->platform_->TextInput()) {
      text_input->RequestShow(*session_to_show);
    }
  }
  return true;
}

void Runtime::HandlePointerDown(const PointerEvent& event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured != state_->pointer_sessions_.end()) {
    const std::optional<std::uint64_t> previous_identity = captured->second.target_identity;
    PointerEvent cancel = event;
    cancel.type = PointerEventType::Cancel;
    EndPointerInteraction(captured->second, InteractionEvent::Type::Cancel, cancel);
    DispatchExtensionObservers(captured->second, cancel, true);
    if (captured->second.extension_capture.has_value()) {
      const NodeExtensionHandle extension_capture = *captured->second.extension_capture;
      if (NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_capture)) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, extension_capture.node_identity)) {
          extension->OnPointer(*node, LocalPointerEvent(*node, cancel));
        }
      }
    }
    if (previous_identity.has_value()) {
      if (detail::MountedNode* previous = FindNode(*state_->mounted_root_, *previous_identity)) {
        EmitEvent<ViewEvents::PointerCancel>(previous->event_bindings, cancel);
      }
    }
    ReleaseScrollGesture(captured->second);
    state_->pointer_sessions_.erase(captured);
  }

  std::vector<detail::MountedNode*> route;
  if (!BuildPointerRoute(*state_->mounted_root_, event.position, route)) {
    return;
  }
  for (detail::MountedNode* node : route) {
    if (IsScrollContainer(*node)) {
      node->scroll_state->motion.Stop();
    }
  }

  PointerSession session;
  session.down_position = event.position;
  session.last_position = event.position;
  session.device_kind = event.device_kind;
  session.velocity_sample_timestamp = state_->platform_->Now();
  const std::optional<std::uint64_t> focus_target = ResolvePointerFocusTarget(route);
  if (event.device_kind == PointerDeviceKind::Touch) {
    session.focus_pending = true;
    session.pending_focus_identity = focus_target;
  } else {
    SetFocusedNode(focus_target, false);
  }
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if (!session.target_identity.has_value() && (*node)->interaction.enabled && HandlesPointerEvents(**node)) {
      session.target_identity = (*node)->identity;
    }
    if ((*node)->interaction.enabled && IsScrollContainer(**node) &&
        (!(*node)->scroll_state->touch_drag_only || event.device_kind == PointerDeviceKind::Touch)) {
      session.scroll_chain.push_back((*node)->identity);
    }
  }

  bool extension_handled = false;
  for (auto node = route.rbegin(); node != route.rend() && !extension_handled; ++node) {
    for (std::size_t index = (*node)->extensions.size(); index > 0; --index) {
      if (!(*node)->interaction.enabled) {
        continue;
      }
      NodeExtensionEntry& entry = (*node)->extensions[index - 1];
      const auto local_position = (*node)->presentation.resolved_transform.Inverse(event.position);
      if (!entry.extension || !local_position.has_value() || !entry.extension->HitTest(**node, *local_position)) {
        continue;
      }
      const PointerEvent local_event = LocalPointerEvent(**node, event);
      const NodeExtension::PointerResult result = entry.extension->OnPointer(**node, local_event);
      if (result == NodeExtension::PointerResult::Ignored) {
        continue;
      }
      if (result == NodeExtension::PointerResult::Observe) {
        session.extension_observers.push_back(
            NodeExtensionHandle{
                (*node)->identity,
                index - 1,
                entry.descriptor,
            }
        );
        RequestFrame();
        continue;
      }
      extension_handled = true;
      session.target_identity.reset();
      if (result == NodeExtension::PointerResult::Capture) {
        session.extension_capture = NodeExtensionHandle{
            (*node)->identity,
            index - 1,
            entry.descriptor,
        };
      }
      RequestFrame();
      break;
    }
  }

  if (extension_handled && !session.extension_capture.has_value()) {
    PointerEvent cancel = event;
    cancel.type = PointerEventType::Cancel;
    DispatchExtensionObservers(session, cancel, true);
    if (!session.focus_pending) {
      return;
    }
  }
  if (!session.target_identity.has_value() && session.scroll_chain.empty() && !session.extension_capture.has_value() &&
      session.extension_observers.empty() && !session.focus_pending) {
    return;
  }

  const std::optional<std::uint64_t> target_identity = session.target_identity;
  std::optional<std::uint64_t> interaction_identity = target_identity;
  if (!interaction_identity.has_value() && session.extension_capture.has_value()) {
    interaction_identity = session.extension_capture->node_identity;
  }
  if (!interaction_identity.has_value() && !session.extension_observers.empty()) {
    interaction_identity = session.extension_observers.front().node_identity;
  }
  if (interaction_identity.has_value()) {
    BeginPointerInteraction(session, *interaction_identity, event);
  }
  state_->pointer_sessions_.insert_or_assign(event.pointer_id, std::move(session));
  if (target_identity.has_value()) {
    if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *target_identity)) {
      EmitEvent<ViewEvents::PointerDown>(target->event_bindings, event);
    }
  }
}

void Runtime::HandlePointerMove(const PointerEvent& event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    if (SupportsHover(event.device_kind)) {
      UpdateHoveredExtensions(event.position);
    }
    if (detail::MountedNode* target = HitTestPointer(*state_->mounted_root_, event.position);
        target && target->interaction.enabled) {
      EmitEvent<ViewEvents::PointerMove>(target->event_bindings, event);
    }
    return;
  }

  PointerSession& session = captured->second;
  const bool cancel_target = DispatchExtensionObservers(session, event, false);
  if (cancel_target) {
    CancelPointerTarget(session, event);
  }
  if (session.extension_capture.has_value()) {
    const NodeExtensionHandle extension_capture = *session.extension_capture;
    NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_capture);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, extension_capture.node_identity);
    if (!extension || !node) {
      EndPointerInteraction(session, InteractionEvent::Type::Cancel, event);
      ReleaseScrollGesture(session);
      state_->pointer_sessions_.erase(captured);
      return;
    }
    if (extension->OnPointer(*node, LocalPointerEvent(*node, event)) != NodeExtension::PointerResult::Ignored) {
      RequestFrame();
    }
    session.last_position = event.position;
    return;
  }

  if (!session.drag_axis.has_value()) {
    if (session.target_identity.has_value()) {
      if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *session.target_identity)) {
        EmitEvent<ViewEvents::PointerMove>(target->event_bindings, event);
      } else {
        EndPointerInteraction(session, InteractionEvent::Type::Cancel, event);
        session.target_identity.reset();
      }
    }

    const float distance_x = event.position.x - session.down_position.x;
    const float distance_y = event.position.y - session.down_position.y;
    if (std::max(std::abs(distance_x), std::abs(distance_y)) < detail::touch_gesture_slop) {
      session.last_position = event.position;
      return;
    }
    if (session.device_kind == PointerDeviceKind::Touch) {
      session.focus_pending = false;
      session.pending_focus_identity.reset();
    }

    const Axis axis = std::abs(distance_y) >= std::abs(distance_x) ? Axis::Vertical : Axis::Horizontal;
    const float delta = PointerDelta(session.down_position, event.position, axis);
    const std::optional<std::size_t> scroll_candidate = FindScrollCandidate(session, axis, delta);
    if (!scroll_candidate.has_value()) {
      session.last_position = event.position;
      return;
    }

    EndPointerInteraction(session, InteractionEvent::Type::Cancel, event);
    if (session.target_identity.has_value()) {
      if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *session.target_identity)) {
        PointerEvent cancel = event;
        cancel.type = PointerEventType::Cancel;
        EmitEvent<ViewEvents::PointerCancel>(target->event_bindings, cancel);
      }
      session.target_identity.reset();
    }
    PointerEvent cancel = event;
    cancel.type = PointerEventType::Cancel;
    DispatchExtensionObservers(session, cancel, true);

    RecordScrollVelocity(session, delta, state_->platform_->Now());
    session.drag_axis = axis;
    session.active_scroll = *scroll_candidate;
    const std::vector<detail::MountedNode*> scrolled = ApplyDragScroll(session, delta);
    if (!scrolled.empty()) {
      for (std::size_t index = 0; index <= session.active_scroll && index < session.scroll_chain.size(); ++index) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, session.scroll_chain[index])) {
          NotifyScrollActivity(*node, ScrollActivitySource::External);
        }
      }
      detail::MountedNode& active = *scrolled.back();
      session.active_scroll_node = active.identity;
      SetScrollGesture(active, true);
    }
    session.last_position = event.position;
    return;
  }

  const float delta = PointerDelta(session.last_position, event.position, *session.drag_axis);
  RecordScrollVelocity(session, delta, state_->platform_->Now());
  const std::vector<detail::MountedNode*> scrolled = ApplyDragScroll(session, delta);
  if (!scrolled.empty()) {
    for (std::size_t index = 0; index <= session.active_scroll && index < session.scroll_chain.size(); ++index) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, session.scroll_chain[index])) {
        NotifyScrollActivity(*node, ScrollActivitySource::External);
      }
    }
    detail::MountedNode& active = *scrolled.back();
    if (session.active_scroll_node != active.identity) {
      if (session.active_scroll_node.has_value()) {
        if (detail::MountedNode* previous = FindNode(*state_->mounted_root_, *session.active_scroll_node)) {
          SetScrollGesture(*previous, false);
        }
      }
      session.active_scroll_node = active.identity;
      SetScrollGesture(active, true);
    }
  }
  session.last_position = event.position;
}

void Runtime::HandlePointerCancel(const PointerEvent& event) {
  if (SupportsHover(event.device_kind) && !state_->hovered_extensions_.empty()) {
    std::vector<std::uint64_t> cleared_nodes;
    for (const NodeExtensionHandle& hovered : state_->hovered_extensions_) {
      if (NodeExtension* extension = FindExtension(*state_->mounted_root_, hovered)) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, hovered.node_identity)) {
          extension->OnHoverChanged(*node, false);
          if (std::ranges::find(cleared_nodes, node->identity) == cleared_nodes.end()) {
            InteractionState interaction = node->interaction;
            interaction.hovered = false;
            UpdateInteraction(*node, interaction);
            cleared_nodes.push_back(node->identity);
          }
        }
      }
    }
    state_->hovered_extensions_.clear();
    RequestFrame();
  }

  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    return;
  }
  const std::optional<std::uint64_t> identity = captured->second.target_identity;
  EndPointerInteraction(captured->second, InteractionEvent::Type::Cancel, event);
  DispatchExtensionObservers(captured->second, event, true);
  if (captured->second.extension_capture.has_value()) {
    const NodeExtensionHandle extension_capture = *captured->second.extension_capture;
    if (NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_capture)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, extension_capture.node_identity)) {
        extension->OnPointer(*node, LocalPointerEvent(*node, event));
      }
    }
    RequestFrame();
  }
  ReleaseScrollGesture(captured->second);
  state_->pointer_sessions_.erase(captured);
  if (identity.has_value()) {
    if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *identity)) {
      EmitEvent<ViewEvents::PointerCancel>(target->event_bindings, event);
    }
  }
}

void Runtime::HandlePointerUp(const PointerEvent& event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    if (detail::MountedNode* target = HitTestPointer(*state_->mounted_root_, event.position);
        target && target->interaction.enabled) {
      EmitEvent<ViewEvents::PointerUp>(target->event_bindings, event);
    }
    if (SupportsHover(event.device_kind)) {
      UpdateHoveredExtensions(event.position);
    }
    return;
  }

  EndPointerInteraction(captured->second, InteractionEvent::Type::Release, event);
  const bool cancel_target = DispatchExtensionObservers(captured->second, event, true);
  if (captured->second.extension_capture.has_value()) {
    const NodeExtensionHandle extension_capture = *captured->second.extension_capture;
    if (NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_capture)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, extension_capture.node_identity)) {
        extension->OnPointer(*node, LocalPointerEvent(*node, event));
      }
    }
    CommitPendingTouchFocus(captured->second, event.position, true);
    ReleaseScrollGesture(captured->second);
    state_->pointer_sessions_.erase(captured);
    if (SupportsHover(event.device_kind)) {
      UpdateHoveredExtensions(event.position);
    }
    RequestFrame();
    return;
  }

  if (cancel_target) {
    CancelPointerTarget(captured->second, event);
  }
  const std::optional<std::uint64_t> identity = captured->second.target_identity;
  const bool was_dragging = captured->second.drag_axis.has_value();
  const std::optional<std::uint64_t> momentum_identity = captured->second.active_scroll_node;
  const double velocity_age = state_->platform_->Now() - captured->second.velocity_sample_timestamp;
  const bool should_start_momentum = was_dragging && captured->second.device_kind == PointerDeviceKind::Touch &&
                                     captured->second.has_velocity_sample && velocity_age >= 0.0 && velocity_age <= 0.1;
  const float scroll_velocity = captured->second.scroll_velocity;
  if (!was_dragging) {
    CommitPendingTouchFocus(captured->second, event.position, true);
  }
  ReleaseScrollGesture(captured->second);
  state_->pointer_sessions_.erase(captured);
  if (should_start_momentum && momentum_identity.has_value()) {
    if (detail::MountedNode* node = FindNode(*state_->mounted_root_, *momentum_identity);
        node && node->scroll_state->motion.StartMomentum(*node, scroll_velocity)) {
      state_->scroll_motion_active_ = true;
      RequestFrame();
    }
  }
  if (SupportsHover(event.device_kind)) {
    UpdateHoveredExtensions(event.position);
  }
  if (was_dragging || !identity.has_value()) {
    return;
  }

  detail::MountedNode* target = FindNode(*state_->mounted_root_, *identity);
  if (!target || !target->interaction.enabled) {
    return;
  }

  EmitEvent<ViewEvents::PointerUp>(target->event_bindings, event);
  if (detail::MountedNode* released = HitTestPointer(*state_->mounted_root_, event.position);
      released && released->interaction.enabled && released->identity == *identity) {
    ActivateNode(*target);
  }
}

} // namespace huxerui
