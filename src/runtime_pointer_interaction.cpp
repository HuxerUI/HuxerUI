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

void RecordScrollVelocitySample(PointerSession& session, Point position, double timestamp) {
  if (!std::isfinite(timestamp)) {
    return;
  }
  if (session.scroll_velocity_sample_count > 0) {
    ScrollVelocitySample& latest = session.scroll_velocity_samples[session.scroll_velocity_sample_count - 1];
    if (timestamp < latest.timestamp) {
      session.scroll_velocity_sample_count = 0;
    } else if (timestamp == latest.timestamp) {
      latest.position = position;
      return;
    }
  }

  if (session.scroll_velocity_sample_count == session.scroll_velocity_samples.size()) {
    for (std::size_t index = 1; index < session.scroll_velocity_samples.size(); ++index) {
      session.scroll_velocity_samples[index - 1] = session.scroll_velocity_samples[index];
    }
    --session.scroll_velocity_sample_count;
  }
  session.scroll_velocity_samples[session.scroll_velocity_sample_count++] = {position, timestamp};
}

std::optional<float> EstimateScrollVelocity(const PointerSession& session, Axis axis, double release_timestamp) {
  constexpr double maximum_sample_age = 0.1;
  if (session.scroll_velocity_sample_count < 2 || !std::isfinite(release_timestamp)) {
    return std::nullopt;
  }

  const ScrollVelocitySample& latest = session.scroll_velocity_samples[session.scroll_velocity_sample_count - 1];
  const double release_age = release_timestamp - latest.timestamp;
  if (!std::isfinite(release_age) || release_age < 0.0 || release_age > maximum_sample_age) {
    return std::nullopt;
  }

  const double cutoff = latest.timestamp - maximum_sample_age;
  std::size_t first = 0;
  while (first < session.scroll_velocity_sample_count && session.scroll_velocity_samples[first].timestamp < cutoff) {
    ++first;
  }
  const std::size_t sample_count = session.scroll_velocity_sample_count - first;
  if (sample_count < 2) {
    return std::nullopt;
  }

  const ScrollVelocitySample& previous = session.scroll_velocity_samples[session.scroll_velocity_sample_count - 2];
  const double latest_elapsed = latest.timestamp - previous.timestamp;
  if (!std::isfinite(latest_elapsed) || latest_elapsed <= 0.0) {
    return std::nullopt;
  }
  const double fallback_velocity =
      static_cast<double>(PointerDelta(previous.position, latest.position, axis)) / latest_elapsed;
  if (!std::isfinite(fallback_velocity)) {
    return std::nullopt;
  }
  if (sample_count == 2) {
    return static_cast<float>(fallback_velocity);
  }

  double weighted_velocity = 0.0;
  double weight_sum = 0.0;
  for (std::size_t index = first + 1; index < session.scroll_velocity_sample_count; ++index) {
    const ScrollVelocitySample& segment_start = session.scroll_velocity_samples[index - 1];
    const ScrollVelocitySample& segment_end = session.scroll_velocity_samples[index];
    const double elapsed = segment_end.timestamp - segment_start.timestamp;
    if (!std::isfinite(elapsed) || elapsed <= 0.0) {
      continue;
    }
    const double weight = static_cast<double>(index - first);
    weighted_velocity +=
        static_cast<double>(PointerDelta(segment_start.position, segment_end.position, axis)) / elapsed * weight;
    weight_sum += weight;
  }
  if (weight_sum <= 0.0) {
    return static_cast<float>(fallback_velocity);
  }
  const double trend_velocity = weighted_velocity / weight_sum;
  if (!std::isfinite(trend_velocity) || trend_velocity == 0.0) {
    return std::nullopt;
  }

  // A quadratic position fit estimates the derivative at release, so an accelerating gesture is not reduced to its
  // average speed. Time is normalized to the sampling window to keep the normal equation well-conditioned.
  double time_sum = 0.0;
  double time2_sum = 0.0;
  double time3_sum = 0.0;
  double time4_sum = 0.0;
  double position_sum = 0.0;
  double time_position_sum = 0.0;
  double time2_position_sum = 0.0;
  for (std::size_t index = first; index < session.scroll_velocity_sample_count; ++index) {
    const ScrollVelocitySample& sample = session.scroll_velocity_samples[index];
    const double time = (sample.timestamp - latest.timestamp) / maximum_sample_age;
    const double time2 = time * time;
    const double position = axis == Axis::Vertical ? -sample.position.y : -sample.position.x;
    time_sum += time;
    time2_sum += time2;
    time3_sum += time2 * time;
    time4_sum += time2 * time2;
    position_sum += position;
    time_position_sum += time * position;
    time2_position_sum += time2 * position;
  }

  const double count = static_cast<double>(sample_count);
  const double determinant = count * (time2_sum * time4_sum - time3_sum * time3_sum) -
                             time_sum * (time_sum * time4_sum - time2_sum * time3_sum) +
                             time2_sum * (time_sum * time3_sum - time2_sum * time2_sum);
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
    return static_cast<float>(trend_velocity);
  }
  const double velocity_determinant = count * (time_position_sum * time4_sum - time3_sum * time2_position_sum) -
                                      position_sum * (time_sum * time4_sum - time2_sum * time3_sum) +
                                      time2_sum * (time_sum * time2_position_sum - time_position_sum * time2_sum);
  const double velocity = velocity_determinant / determinant / maximum_sample_age;
  if (!std::isfinite(velocity)) {
    return static_cast<float>(trend_velocity);
  }
  if (std::signbit(velocity) != std::signbit(trend_velocity)) {
    return std::signbit(fallback_velocity) == std::signbit(trend_velocity)
               ? std::optional<float>{static_cast<float>(fallback_velocity)}
               : std::nullopt;
  }

  constexpr double maximum_extrapolation_ratio = 2.0;
  const double maximum_velocity = std::abs(trend_velocity) * maximum_extrapolation_ratio;
  return static_cast<float>(std::clamp(velocity, -maximum_velocity, maximum_velocity));
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
  if (event.device_kind == PointerDeviceKind::Touch) {
    RecordScrollVelocitySample(session, event.position, state_->platform_->Now());
  }
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

  if (session.device_kind == PointerDeviceKind::Touch) {
    RecordScrollVelocitySample(session, event.position, state_->platform_->Now());
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
  const std::optional<float> scroll_velocity =
      was_dragging && captured->second.device_kind == PointerDeviceKind::Touch
          ? EstimateScrollVelocity(captured->second, *captured->second.drag_axis, state_->platform_->Now())
          : std::nullopt;
  if (!was_dragging) {
    CommitPendingTouchFocus(captured->second, event.position, true);
  }
  ReleaseScrollGesture(captured->second);
  state_->pointer_sessions_.erase(captured);
  if (scroll_velocity.has_value() && momentum_identity.has_value()) {
    if (detail::MountedNode* node = FindNode(*state_->mounted_root_, *momentum_identity);
        node && node->scroll_state->motion.StartMomentum(*node, *scroll_velocity)) {
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
