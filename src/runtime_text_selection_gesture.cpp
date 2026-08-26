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

void Runtime::HandleTextSelectionClick(const PointerEvent& event) {
  if (event.type != PointerEventType::Down || event.click_count < 2 ||
      (event.device_kind != PointerDeviceKind::Mouse && event.device_kind != PointerDeviceKind::Pen)) {
    return;
  }
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    return;
  }
  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused || !detail::FindTextSelectionClient(*focused)) {
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
    return;
  }
  const std::size_t index = static_cast<std::size_t>(recognition - session->second.recognitions.begin());
  ResolvePointerRecognition(session->second, index, event);
  SelectFocusedTextWord(event.position, false);
}

bool Runtime::TrackTouchTextSelectionGesture(const PointerEvent& event) {
  if (event.device_kind != PointerDeviceKind::Touch) {
    return false;
  }
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
    recognition->tap_pending = false;
    recognition->double_tap_pending = false;
    constexpr double double_tap_interval = 0.4;
    constexpr float double_tap_slop = 18.0F;
    const double now = state_->platform_->Now();
    if (gesture.previous_tap_time.has_value() &&
        gesture.previous_tap_node == std::optional{recognition->node_identity} &&
        now - *gesture.previous_tap_time >= 0.0 && now - *gesture.previous_tap_time <= double_tap_interval &&
        std::hypot(event.position.x - gesture.previous_tap_position.x,
                   event.position.y - gesture.previous_tap_position.y) <= double_tap_slop) {
      gesture.previous_tap_time.reset();
      gesture.previous_tap_node.reset();
      recognition->double_tap_pending = true;
      recognition->tap_position = event.position;
      return false;
    }
    constexpr double delay = 0.5;
    recognition->long_press_pending = true;
    recognition->long_press_deadline = now + delay;
    recognition->tap_pending = true;
    recognition->tap_position = event.position;
    RequestFrameAfter(delay);
    return false;
  }
  if (event.type == PointerEventType::Move) {
    const float distance =
        std::hypot(event.position.x - recognition->tap_position.x, event.position.y - recognition->tap_position.y);
    if (recognition->long_press_pending && distance >= detail::touch_gesture_slop) {
      recognition->long_press_pending = false;
    }
    if (recognition->tap_pending && distance >= 18.0F) {
      recognition->tap_pending = false;
    }
    if (recognition->double_tap_pending && distance >= detail::touch_gesture_slop) {
      recognition->double_tap_pending = false;
    }
    return false;
  }
  if (event.type == PointerEventType::Up) {
    if (recognition->double_tap_pending && state_->focused_node_identity_ == std::optional{recognition->node_identity}) {
      recognition->double_tap_pending = false;
      return true;
    }
    if (recognition->tap_pending) {
      gesture.previous_tap_time = state_->platform_->Now();
      gesture.previous_tap_position = event.position;
      gesture.previous_tap_node = recognition->node_identity;
    }
    recognition->tap_pending = false;
    recognition->long_press_pending = false;
    return false;
  }
  if (event.type == PointerEventType::Cancel) {
    recognition->tap_pending = false;
    recognition->long_press_pending = false;
    recognition->double_tap_pending = false;
  }
  return false;
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
