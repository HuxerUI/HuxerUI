#include "internal.h"

#include <algorithm>
#include <cmath>

namespace huxerui::detail {

namespace {

bool HandlesPointerEvents(const MountedNode &node) {
  return static_cast<bool>(node.activation) ||
         HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerDown>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerMove>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerUp>(node.event_bindings) ||
         HasEventBinding<ViewEvents::PointerCancel>(node.event_bindings);
}

bool IsScrollContainer(const MountedNode &node) {
  return node.enabled &&
         (node.kind == NodeKind::ScrollView ||
          IsVirtualLayoutNode(node));
}

Axis ScrollAxis(const MountedNode &node) {
  return node.kind == NodeKind::ScrollView ? Axis::Vertical
                                           : node.virtual_state->axis;
}

float PointerDelta(Point previous, Point current, Axis axis) {
  return axis == Axis::Vertical ? previous.y - current.y
                                : previous.x - current.x;
}

void SetScrollGesture(MountedNode &node, bool active) {
  for (MountedModifierEntry &entry : node.modifiers) {
    if (entry.mounted) {
      entry.mounted->OnScrollGesture(node, active);
    }
  }
}

} // namespace

} // namespace huxerui::detail

namespace huxerui {

using namespace detail;

detail::MountedNode *Runtime::FindNode(
    detail::MountedNode &node, std::uint64_t identity) {
  if (node.identity == identity) {
    return &node;
  }
  for (auto &child : node.children) {
    if (detail::MountedNode *found = FindNode(*child, identity)) {
      return found;
    }
  }
  return nullptr;
}

MountedModifier *Runtime::FindModifier(
    detail::MountedNode &root,
    const ModifierPointerCapture &capture) {
  detail::MountedNode *node =
      FindNode(root, capture.node_identity);
  if (!node || capture.modifier_index >= node->modifiers.size()) {
    return nullptr;
  }
  MountedModifierEntry &entry =
      node->modifiers[capture.modifier_index];
  if (entry.descriptor != capture.descriptor || !entry.mounted) {
    return nullptr;
  }
  return entry.mounted.get();
}

void Runtime::ActivateNode(detail::MountedNode &node) {
  EmitEvent<ViewEvents::Click>(node.event_bindings);
  if (node.activation) {
    node.activation(node.event_bindings);
  }
}

void Runtime::ReleaseScrollGesture(
    PointerSession &session) {
  if (!session.active_scroll_node.has_value()) {
    return;
  }
  if (detail::MountedNode *node = FindNode(
          *state_->mounted_root_, *session.active_scroll_node)) {
    SetScrollGesture(*node, false);
  }
  session.active_scroll_node.reset();
}

void Runtime::DispatchModifierObservers(
    PointerSession &session,
    const PointerEvent &event,
    bool clear) {
  for (const ModifierPointerCapture &observer :
       session.modifier_observers) {
    MountedModifier *modifier =
        FindModifier(*state_->mounted_root_, observer);
    detail::MountedNode *node =
        FindNode(*state_->mounted_root_, observer.node_identity);
    if (modifier && node) {
      modifier->OnPointer(*node, event);
    }
  }
  if (clear) {
    session.modifier_observers.clear();
  }
}

std::optional<std::size_t>
Runtime::FindScrollCandidate(
    const PointerSession &session,
    Axis axis, float delta) {
  for (std::size_t index = 0;
       index < session.scroll_chain.size(); ++index) {
    detail::MountedNode *candidate =
        FindNode(*state_->mounted_root_, session.scroll_chain[index]);
    if (candidate && candidate->enabled &&
        ScrollAxis(*candidate) == axis &&
        CanScrollNode(*candidate, delta)) {
      return index;
    }
  }
  return std::nullopt;
}

detail::MountedNode *Runtime::ApplyDragScroll(
    PointerSession &session, float delta) {
  if (!session.drag_axis.has_value() || delta == 0.0F) {
    return nullptr;
  }

  detail::MountedNode *last_changed = nullptr;
  float remaining = delta;
  for (std::size_t index = session.active_scroll;
       index < session.scroll_chain.size(); ++index) {
    detail::MountedNode *candidate =
        FindNode(*state_->mounted_root_, session.scroll_chain[index]);
    if (!candidate || !candidate->enabled ||
        ScrollAxis(*candidate) != *session.drag_axis) {
      continue;
    }
    const float consumed =
        ScrollNodeBy(*candidate, remaining);
    if (consumed != 0.0F) {
      last_changed = candidate;
      session.active_scroll = index;
      remaining -= consumed;
    }
    if (std::abs(remaining) < 0.001F) {
      break;
    }
  }
  return last_changed;
}

void Runtime::HandlePointerEvent(
    const PointerEvent &event) {
  if (!state_->mounted_root_) {
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
}

void Runtime::HandlePointerDown(
    const PointerEvent &event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured != state_->pointer_sessions_.end()) {
    const std::optional<std::uint64_t> previous_identity =
        captured->second.target_identity;
    PointerEvent cancel = event;
    cancel.type = PointerEventType::Cancel;
    DispatchModifierObservers(
        captured->second, cancel, true);
    if (captured->second.modifier_capture.has_value()) {
      const ModifierPointerCapture modifier_capture =
          *captured->second.modifier_capture;
      if (MountedModifier *modifier =
              FindModifier(*state_->mounted_root_, modifier_capture)) {
        if (detail::MountedNode *node =
                FindNode(
                    *state_->mounted_root_,
                    modifier_capture.node_identity)) {
          modifier->OnPointer(*node, cancel);
        }
      }
    }
    ReleaseScrollGesture(captured->second);
    state_->pointer_sessions_.erase(captured);
    if (previous_identity.has_value()) {
      if (detail::MountedNode *previous =
              FindNode(*state_->mounted_root_, *previous_identity)) {
        EmitEvent<ViewEvents::PointerCancel>(
            previous->event_bindings, cancel);
      }
    }
  }

  std::vector<detail::MountedNode *> route;
  if (!BuildPointerRoute(
          *state_->mounted_root_, event.position, route)) {
    return;
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
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if (!session.target_identity.has_value() &&
        (*node)->enabled &&
        HandlesPointerEvents(**node)) {
      session.target_identity = (*node)->identity;
    }
    if (IsScrollContainer(**node)) {
      session.scroll_chain.push_back((*node)->identity);
    }
  }

  bool modifier_handled = false;
  for (auto node = route.rbegin();
       node != route.rend() && !modifier_handled; ++node) {
    for (std::size_t index = (*node)->modifiers.size();
         index > 0; --index) {
      if (!(*node)->enabled) {
        continue;
      }
      MountedModifierEntry &entry =
          (*node)->modifiers[index - 1];
      if (!entry.mounted ||
          !entry.mounted->HitTest(**node, event.position)) {
        continue;
      }
      const ModifierPointerResult result =
          entry.mounted->OnPointer(**node, event);
      if (result == ModifierPointerResult::Ignored) {
        continue;
      }
      if (result == ModifierPointerResult::Observe) {
        session.modifier_observers.push_back(
            ModifierPointerCapture{
                (*node)->identity,
                index - 1,
                entry.descriptor,
            });
        RequestFrame();
        continue;
      }
      modifier_handled = true;
      session.target_identity.reset();
      if (result == ModifierPointerResult::Capture) {
        session.modifier_capture = ModifierPointerCapture{
            (*node)->identity,
            index - 1,
            entry.descriptor,
        };
      }
      RequestFrame();
      break;
    }
  }

  if (modifier_handled &&
      !session.modifier_capture.has_value()) {
    PointerEvent cancel = event;
    cancel.type = PointerEventType::Cancel;
    DispatchModifierObservers(session, cancel, true);
    return;
  }
  if (!session.target_identity.has_value() &&
      session.scroll_chain.empty() &&
      !session.modifier_capture.has_value() &&
      session.modifier_observers.empty()) {
    return;
  }

  const std::optional<std::uint64_t> target_identity =
      session.target_identity;
  state_->pointer_sessions_.insert_or_assign(
      event.pointer_id, std::move(session));
  if (target_identity.has_value()) {
    if (detail::MountedNode *target =
            FindNode(*state_->mounted_root_, *target_identity)) {
      EmitEvent<ViewEvents::PointerDown>(
          target->event_bindings, event);
    }
  }
}

void Runtime::HandlePointerMove(
    const PointerEvent &event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    UpdateHoveredModifier(event.position);
    if (detail::MountedNode *target =
            HitTestPointer(*state_->mounted_root_, event.position);
        target && target->enabled) {
      EmitEvent<ViewEvents::PointerMove>(
          target->event_bindings, event);
    }
    return;
  }

  PointerSession &session = captured->second;
  DispatchModifierObservers(session, event, false);
  if (session.modifier_capture.has_value()) {
    const ModifierPointerCapture modifier_capture =
        *session.modifier_capture;
    MountedModifier *modifier =
        FindModifier(*state_->mounted_root_, modifier_capture);
    detail::MountedNode *node =
        FindNode(
            *state_->mounted_root_, modifier_capture.node_identity);
    if (!modifier || !node) {
      ReleaseScrollGesture(session);
      state_->pointer_sessions_.erase(captured);
      return;
    }
    if (modifier->OnPointer(*node, event) !=
        ModifierPointerResult::Ignored) {
      RequestFrame();
    }
    session.last_position = event.position;
    return;
  }

  if (!session.drag_axis.has_value()) {
    if (session.target_identity.has_value()) {
      if (detail::MountedNode *target =
              FindNode(
                  *state_->mounted_root_, *session.target_identity)) {
        EmitEvent<ViewEvents::PointerMove>(
            target->event_bindings, event);
      } else {
        session.target_identity.reset();
      }
    }

    const float distance_x =
        event.position.x - session.down_position.x;
    const float distance_y =
        event.position.y - session.down_position.y;
    constexpr float drag_slop = 6.0F;
    if (std::max(
            std::abs(distance_x), std::abs(distance_y)) <
        drag_slop) {
      session.last_position = event.position;
      return;
    }

    const Axis axis =
        std::abs(distance_y) >= std::abs(distance_x)
            ? Axis::Vertical
            : Axis::Horizontal;
    const float delta =
        PointerDelta(
            session.down_position, event.position, axis);
    const std::optional<std::size_t> scroll_candidate =
        FindScrollCandidate(session, axis, delta);
    if (!scroll_candidate.has_value()) {
      session.last_position = event.position;
      return;
    }

    if (session.target_identity.has_value()) {
      if (detail::MountedNode *target =
              FindNode(
                  *state_->mounted_root_, *session.target_identity)) {
        PointerEvent cancel = event;
        cancel.type = PointerEventType::Cancel;
        EmitEvent<ViewEvents::PointerCancel>(
            target->event_bindings, cancel);
      }
      session.target_identity.reset();
    }
    PointerEvent cancel = event;
    cancel.type = PointerEventType::Cancel;
    DispatchModifierObservers(session, cancel, true);

    session.drag_axis = axis;
    session.active_scroll = *scroll_candidate;
    if (detail::MountedNode *scrolled =
            ApplyDragScroll(session, delta)) {
      NotifyScrollActivity(*scrolled);
      session.active_scroll_node = scrolled->identity;
      SetScrollGesture(*scrolled, true);
      RequestFrame();
    }
    session.last_position = event.position;
    return;
  }

  const float delta =
      PointerDelta(
          session.last_position, event.position,
          *session.drag_axis);
  if (detail::MountedNode *scrolled =
          ApplyDragScroll(session, delta)) {
    NotifyScrollActivity(*scrolled);
    if (session.active_scroll_node != scrolled->identity) {
      if (session.active_scroll_node.has_value()) {
        if (detail::MountedNode *previous =
                FindNode(
                    *state_->mounted_root_,
                    *session.active_scroll_node)) {
          SetScrollGesture(*previous, false);
        }
      }
      session.active_scroll_node = scrolled->identity;
      SetScrollGesture(*scrolled, true);
    }
    RequestFrame();
  }
  session.last_position = event.position;
}

void Runtime::HandlePointerCancel(
    const PointerEvent &event) {
  if (state_->hovered_modifier_.has_value()) {
    const ModifierPointerCapture hovered =
        *state_->hovered_modifier_;
    if (MountedModifier *modifier =
            FindModifier(*state_->mounted_root_, hovered)) {
      if (detail::MountedNode *node =
              FindNode(
                  *state_->mounted_root_, hovered.node_identity)) {
        modifier->OnHoverChanged(*node, false);
      }
    }
    state_->hovered_modifier_.reset();
    RequestFrame();
  }

  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    return;
  }
  const std::optional<std::uint64_t> identity =
      captured->second.target_identity;
  DispatchModifierObservers(
      captured->second, event, true);
  if (captured->second.modifier_capture.has_value()) {
    const ModifierPointerCapture modifier_capture =
        *captured->second.modifier_capture;
    if (MountedModifier *modifier =
            FindModifier(*state_->mounted_root_, modifier_capture)) {
      if (detail::MountedNode *node =
              FindNode(
                  *state_->mounted_root_,
                  modifier_capture.node_identity)) {
        modifier->OnPointer(*node, event);
      }
    }
    RequestFrame();
  }
  ReleaseScrollGesture(captured->second);
  state_->pointer_sessions_.erase(captured);
  if (identity.has_value()) {
    if (detail::MountedNode *target =
            FindNode(*state_->mounted_root_, *identity)) {
      EmitEvent<ViewEvents::PointerCancel>(
          target->event_bindings, event);
    }
  }
}

void Runtime::HandlePointerUp(
    const PointerEvent &event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    if (detail::MountedNode *target =
            HitTestPointer(*state_->mounted_root_, event.position);
        target && target->enabled) {
      EmitEvent<ViewEvents::PointerUp>(
          target->event_bindings, event);
    }
    UpdateHoveredModifier(event.position);
    return;
  }

  DispatchModifierObservers(
      captured->second, event, true);
  if (captured->second.modifier_capture.has_value()) {
    const ModifierPointerCapture modifier_capture =
        *captured->second.modifier_capture;
    if (MountedModifier *modifier =
            FindModifier(*state_->mounted_root_, modifier_capture)) {
      if (detail::MountedNode *node =
              FindNode(
                  *state_->mounted_root_,
                  modifier_capture.node_identity)) {
        modifier->OnPointer(*node, event);
      }
    }
    ReleaseScrollGesture(captured->second);
    state_->pointer_sessions_.erase(captured);
    UpdateHoveredModifier(event.position);
    RequestFrame();
    return;
  }

  const std::optional<std::uint64_t> identity =
      captured->second.target_identity;
  const bool was_dragging =
      captured->second.drag_axis.has_value();
  ReleaseScrollGesture(captured->second);
  state_->pointer_sessions_.erase(captured);
  UpdateHoveredModifier(event.position);
  if (was_dragging || !identity.has_value()) {
    return;
  }

  detail::MountedNode *target =
      FindNode(*state_->mounted_root_, *identity);
  if (!target || !target->enabled) {
    return;
  }

  EmitEvent<ViewEvents::PointerUp>(
      target->event_bindings, event);
  if (detail::MountedNode *released =
          HitTestPointer(*state_->mounted_root_, event.position);
      released && released->enabled &&
      released->identity == *identity) {
    ActivateNode(*target);
  }
}

} // namespace huxerui
