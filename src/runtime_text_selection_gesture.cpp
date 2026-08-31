#include "internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>

namespace huxerui {
namespace {

detail::TextSelectionRecognitionState* FindTextSelectionRecognition(detail::PointerSession& session) {
  const auto found = std::ranges::find_if(session.recognitions, [](const detail::PointerRecognition& recognition) {
    return recognition.started &&
           std::holds_alternative<detail::TextSelectionRecognitionState>(recognition.state);
  });
  return found == session.recognitions.end()
             ? nullptr
             : &std::get<detail::TextSelectionRecognitionState>(found->state);
}

} // namespace

void Runtime::HandleTextSelectionPointerDown(const PointerEvent& event) {
  if (event.type != PointerEventType::Down) {
    return;
  }
  const auto session = state_->pointer_sessions_.find(event.pointer_id);
  if (session == state_->pointer_sessions_.end()) {
    return;
  }
  const auto recognition =
      std::ranges::find_if(session->second.recognitions, [](const detail::PointerRecognition& entry) {
        return std::holds_alternative<detail::TextSelectionRecognitionState>(entry.state);
      });
  if (recognition == session->second.recognitions.end()) {
    state_->text_selection_gesture_.previous_tap_time.reset();
    state_->text_selection_gesture_.previous_tap_node.reset();
    return;
  }
  recognition->started = true;
  if (!TrackTextSelectionGesture(event)) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(recognition - session->second.recognitions.begin());
  ResolvePointerRecognition(session->second, index, event);
  SelectFocusedTextWord(event.position, false);
}

bool Runtime::TrackTextSelectionGesture(const PointerEvent& event) {
  const auto session = state_->pointer_sessions_.find(event.pointer_id);
  if (session == state_->pointer_sessions_.end()) {
    return false;
  }
  detail::TextSelectionRecognitionState* recognition = FindTextSelectionRecognition(session->second);
  if (recognition == nullptr) {
    return false;
  }
  detail::TextSelectionGestureState& gesture = state_->text_selection_gesture_;
  if (event.type == PointerEventType::Down) {
    recognition->long_press_pending = false;
    recognition->double_tap_pending = false;
    const double now = state_->platform_->Now();
    const bool consecutive =
        gesture.previous_tap_time.has_value() &&
        gesture.previous_tap_node == std::optional{recognition->node_identity} &&
        gesture.previous_tap_device == event.device_kind && now - *gesture.previous_tap_time >= 0.0 &&
        now - *gesture.previous_tap_time <= state_->gesture_settings_.multi_tap_interval.count() &&
        std::hypot(event.position.x - gesture.previous_tap_position.x,
                   event.position.y - gesture.previous_tap_position.y) <= state_->gesture_settings_.multi_tap_slop;
    gesture.previous_tap_time.reset();
    gesture.previous_tap_node.reset();
    recognition->tap_position = event.position;
    if (consecutive) {
      if (event.device_kind != PointerDeviceKind::Touch) {
        return true;
      }
      recognition->double_tap_pending = true;
      return false;
    }
    if (event.device_kind == PointerDeviceKind::Touch) {
      const double delay = state_->gesture_settings_.long_press_duration.count();
      recognition->long_press_pending = true;
      recognition->long_press_deadline = now + delay;
      RequestFrameAfter(delay);
    }
    return false;
  }
  if (event.type == PointerEventType::Move || event.type == PointerEventType::Up) {
    const float distance =
        std::hypot(event.position.x - recognition->tap_position.x, event.position.y - recognition->tap_position.y);
    if (recognition->long_press_pending && distance >= state_->gesture_settings_.pointer_slop) {
      recognition->long_press_pending = false;
    }
    if (recognition->double_tap_pending && distance >= state_->gesture_settings_.pointer_slop) {
      recognition->double_tap_pending = false;
    }
    if (event.type == PointerEventType::Move) {
      return false;
    }
  }
  if (event.type == PointerEventType::Up) {
    if (recognition->double_tap_pending &&
        state_->focused_node_identity_ == std::optional{recognition->node_identity}) {
      recognition->double_tap_pending = false;
      return true;
    }
    recognition->long_press_pending = false;
    return false;
  }
  if (event.type == PointerEventType::Cancel) {
    recognition->long_press_pending = false;
    recognition->double_tap_pending = false;
  }
  return false;
}

void Runtime::RecordTextSelectionTap(const detail::PointerSession& session, const PointerEvent& event) {
  if (event.type != PointerEventType::Up || session.initiating_button != PointerButton::Primary || session.chorded ||
      event.device_kind != session.device_kind ||
      std::hypot(event.position.x - session.down_position.x, event.position.y - session.down_position.y) >
          state_->gesture_settings_.multi_tap_slop ||
      !state_->focused_node_identity_.has_value()) {
    return;
  }

  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (focused == nullptr || detail::FindTextSelectionClient(*focused) == nullptr) {
    return;
  }

  if (session.owner.has_value()) {
    const auto* owner_index = std::get_if<std::size_t>(&*session.owner);
    if (owner_index == nullptr || *owner_index >= session.recognitions.size()) {
      return;
    }
    const detail::PointerRecognitionState& owner = session.recognitions[*owner_index].state;
    if (const auto* extension = std::get_if<detail::ExtensionRecognitionState>(&owner)) {
      NodeExtension* node_extension = FindExtension(*state_->mounted_root_, extension->extension);
      if (extension->extension.node_identity != focused->identity || node_extension == nullptr ||
          node_extension->GetTextSelectionClient() == nullptr) {
        return;
      }
    } else if (!std::holds_alternative<detail::TapRecognitionState>(owner)) {
      return;
    }
  }

  std::vector<detail::MountedNode*> route;
  if (!BuildPointerRoute(*state_->mounted_root_, event.position, route) ||
      std::ranges::find(route, focused->identity, &detail::MountedNode::identity) == route.end()) {
    return;
  }

  detail::TextSelectionGestureState& gesture = state_->text_selection_gesture_;
  gesture.previous_tap_time = state_->platform_->Now();
  gesture.previous_tap_position = event.position;
  gesture.previous_tap_node = focused->identity;
  gesture.previous_tap_device = event.device_kind;
}

void Runtime::AdvanceTextSelectionLongPress(double timestamp) {
  std::optional<std::int64_t> ready_pointer;
  std::optional<double> ready_deadline;
  Point ready_position;
  std::optional<double> next_deadline;
  for (auto& [pointer_id, session] : state_->pointer_sessions_) {
    if (session.quarantined) {
      continue;
    }
    detail::TextSelectionRecognitionState* recognition = FindTextSelectionRecognition(session);
    if (recognition == nullptr || !recognition->long_press_pending) {
      continue;
    }
    if (timestamp >= recognition->long_press_deadline) {
      if (!ready_deadline.has_value() || recognition->long_press_deadline < *ready_deadline ||
          (recognition->long_press_deadline == *ready_deadline && pointer_id < *ready_pointer)) {
        ready_pointer = pointer_id;
        ready_deadline = recognition->long_press_deadline;
        ready_position = recognition->tap_position;
      }
      continue;
    }
    next_deadline = !next_deadline.has_value()
                        ? std::optional{recognition->long_press_deadline}
                        : std::optional{std::min(*next_deadline, recognition->long_press_deadline)};
  }
  if (!ready_pointer.has_value()) {
    if (next_deadline.has_value()) {
      RequestFrameAfter(*next_deadline - timestamp);
    }
    return;
  }

  if (const auto session = state_->pointer_sessions_.find(*ready_pointer);
      session != state_->pointer_sessions_.end()) {
    if (detail::TextSelectionRecognitionState* recognition = FindTextSelectionRecognition(session->second)) {
      recognition->long_press_pending = false;
    }
    CommitPendingTouchFocus(session->second, ready_position);
    RefreshTextInputSession();
    const auto recognition =
        std::ranges::find_if(session->second.recognitions, [](const detail::PointerRecognition& entry) {
          return entry.started && std::holds_alternative<detail::TextSelectionRecognitionState>(entry.state);
        });
    if (recognition != session->second.recognitions.end()) {
      const std::size_t index = static_cast<std::size_t>(recognition - session->second.recognitions.begin());
      const PointerEvent event{PointerEventType::Move, *ready_pointer, ready_position, PointerDeviceKind::Touch};
      ResolvePointerRecognition(session->second, index, event);
    }
  }
  SelectFocusedTextWord(ready_position);
}

} // namespace huxerui
