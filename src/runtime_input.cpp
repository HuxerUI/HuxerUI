#include "internal.h"

#include <algorithm>
#include <cmath>

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
        node.frame.x + node.frame.width * 0.5F,
        node.frame.y + node.frame.height * 0.5F,
    };
  }
  return local;
}

} // namespace

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
  EmitEvent<ViewEvents::Click>(node.event_bindings);
  if (node.activation) {
    node.activation(node.event_bindings);
  }
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

void Runtime::DispatchExtensionObservers(PointerSession& session, const PointerEvent& event, bool clear) {
  for (const NodeExtensionHandle& observer : session.extension_observers) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, observer);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, observer.node_identity);
    if (extension && node) {
      extension->OnPointer(*node, LocalPointerEvent(*node, event));
    }
  }
  if (clear) {
    session.extension_observers.clear();
  }
}

std::optional<std::size_t> Runtime::FindScrollCandidate(const PointerSession& session, Axis axis, float delta) {
  for (std::size_t index = 0; index < session.scroll_chain.size(); ++index) {
    detail::MountedNode* candidate = FindNode(*state_->mounted_root_, session.scroll_chain[index]);
    if (candidate && candidate->enabled && ScrollAxis(*candidate) == axis && CanScrollNode(*candidate, delta)) {
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
    if (!candidate || !candidate->enabled || ScrollAxis(*candidate) != *session.drag_axis) {
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

void Runtime::HandlePointerDown(const PointerEvent& event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured != state_->pointer_sessions_.end()) {
    const std::optional<std::uint64_t> previous_identity = captured->second.target_identity;
    PointerEvent cancel = event;
    cancel.type = PointerEventType::Cancel;
    DispatchExtensionObservers(captured->second, cancel, true);
    if (captured->second.extension_capture.has_value()) {
      const NodeExtensionHandle extension_capture = *captured->second.extension_capture;
      if (NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_capture)) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, extension_capture.node_identity)) {
          extension->OnPointer(*node, LocalPointerEvent(*node, cancel));
        }
      }
    }
    ReleaseScrollGesture(captured->second);
    state_->pointer_sessions_.erase(captured);
    if (previous_identity.has_value()) {
      if (detail::MountedNode* previous = FindNode(*state_->mounted_root_, *previous_identity)) {
        EmitEvent<ViewEvents::PointerCancel>(previous->event_bindings, cancel);
      }
    }
  }

  std::vector<detail::MountedNode*> route;
  if (!BuildPointerRoute(*state_->mounted_root_, event.position, route)) {
    return;
  }
  for (detail::MountedNode* node : route) {
    if (IsScrollContainer(*node)) {
      node->scroll->motion.Stop();
    }
  }

  std::optional<std::uint64_t> focus_target;
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if ((*node)->enabled && (*node)->focusable) {
      focus_target = (*node)->identity;
      break;
    }
  }
  SetFocusedNode(focus_target, false);

  PointerSession session;
  session.down_position = event.position;
  session.last_position = event.position;
  session.device_kind = event.device_kind;
  session.velocity_sample_timestamp = state_->platform_->Now();
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if (!session.target_identity.has_value() && (*node)->enabled && HandlesPointerEvents(**node)) {
      session.target_identity = (*node)->identity;
    }
    if ((*node)->enabled && IsScrollContainer(**node)) {
      session.scroll_chain.push_back((*node)->identity);
    }
  }

  bool extension_handled = false;
  for (auto node = route.rbegin(); node != route.rend() && !extension_handled; ++node) {
    for (std::size_t index = (*node)->extensions.size(); index > 0; --index) {
      if (!(*node)->enabled) {
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
    return;
  }
  if (!session.target_identity.has_value() && session.scroll_chain.empty() && !session.extension_capture.has_value() &&
      session.extension_observers.empty()) {
    return;
  }

  const std::optional<std::uint64_t> target_identity = session.target_identity;
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
      UpdateHoveredExtension(event.position);
    }
    if (detail::MountedNode* target = HitTestPointer(*state_->mounted_root_, event.position);
        target && target->enabled) {
      EmitEvent<ViewEvents::PointerMove>(target->event_bindings, event);
    }
    return;
  }

  PointerSession& session = captured->second;
  DispatchExtensionObservers(session, event, false);
  if (session.extension_capture.has_value()) {
    const NodeExtensionHandle extension_capture = *session.extension_capture;
    NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_capture);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, extension_capture.node_identity);
    if (!extension || !node) {
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
        session.target_identity.reset();
      }
    }

    const float distance_x = event.position.x - session.down_position.x;
    const float distance_y = event.position.y - session.down_position.y;
    constexpr float drag_slop = 6.0F;
    if (std::max(std::abs(distance_x), std::abs(distance_y)) < drag_slop) {
      session.last_position = event.position;
      return;
    }

    const Axis axis = std::abs(distance_y) >= std::abs(distance_x) ? Axis::Vertical : Axis::Horizontal;
    const float delta = PointerDelta(session.down_position, event.position, axis);
    const std::optional<std::size_t> scroll_candidate = FindScrollCandidate(session, axis, delta);
    if (!scroll_candidate.has_value()) {
      session.last_position = event.position;
      return;
    }

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
      for (detail::MountedNode* node : scrolled) {
        NotifyScrollActivity(*node);
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
    for (detail::MountedNode* node : scrolled) {
      NotifyScrollActivity(*node);
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
  if (SupportsHover(event.device_kind) && state_->hovered_extension_.has_value()) {
    const NodeExtensionHandle hovered = *state_->hovered_extension_;
    if (NodeExtension* extension = FindExtension(*state_->mounted_root_, hovered)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, hovered.node_identity)) {
        extension->OnHoverChanged(*node, false);
      }
    }
    state_->hovered_extension_.reset();
    RequestFrame();
  }

  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    return;
  }
  const std::optional<std::uint64_t> identity = captured->second.target_identity;
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
        target && target->enabled) {
      EmitEvent<ViewEvents::PointerUp>(target->event_bindings, event);
    }
    if (SupportsHover(event.device_kind)) {
      UpdateHoveredExtension(event.position);
    }
    return;
  }

  DispatchExtensionObservers(captured->second, event, true);
  if (captured->second.extension_capture.has_value()) {
    const NodeExtensionHandle extension_capture = *captured->second.extension_capture;
    if (NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_capture)) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, extension_capture.node_identity)) {
        extension->OnPointer(*node, LocalPointerEvent(*node, event));
      }
    }
    ReleaseScrollGesture(captured->second);
    state_->pointer_sessions_.erase(captured);
    if (SupportsHover(event.device_kind)) {
      UpdateHoveredExtension(event.position);
    }
    RequestFrame();
    return;
  }

  const std::optional<std::uint64_t> identity = captured->second.target_identity;
  const bool was_dragging = captured->second.drag_axis.has_value();
  const std::optional<std::uint64_t> momentum_identity = captured->second.active_scroll_node;
  const double velocity_age = state_->platform_->Now() - captured->second.velocity_sample_timestamp;
  const bool should_start_momentum = was_dragging && captured->second.device_kind == PointerDeviceKind::Touch &&
                                     captured->second.has_velocity_sample && velocity_age >= 0.0 && velocity_age <= 0.1;
  const float scroll_velocity = captured->second.scroll_velocity;
  ReleaseScrollGesture(captured->second);
  state_->pointer_sessions_.erase(captured);
  if (should_start_momentum && momentum_identity.has_value()) {
    if (detail::MountedNode* node = FindNode(*state_->mounted_root_, *momentum_identity);
        node && node->scroll->motion.StartMomentum(*node, scroll_velocity)) {
      state_->scroll_motion_active_ = true;
      RequestFrame();
    }
  }
  if (SupportsHover(event.device_kind)) {
    UpdateHoveredExtension(event.position);
  }
  if (was_dragging || !identity.has_value()) {
    return;
  }

  detail::MountedNode* target = FindNode(*state_->mounted_root_, *identity);
  if (!target || !target->enabled) {
    return;
  }

  EmitEvent<ViewEvents::PointerUp>(target->event_bindings, event);
  if (detail::MountedNode* released = HitTestPointer(*state_->mounted_root_, event.position);
      released && released->enabled && released->identity == *identity) {
    ActivateNode(*target);
  }
}

} // namespace huxerui
