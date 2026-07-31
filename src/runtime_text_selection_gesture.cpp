#include "internal.h"

#include <cmath>
#include <optional>

namespace huxerui {

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
  PointerEvent cancel = event;
  cancel.type = PointerEventType::Cancel;
  HandlePointerCancel(cancel);
  SelectFocusedTextWord(event.position, false);
}

void Runtime::TrackTouchTextSelectionGesture(const PointerEvent& event) {
  detail::TextSelectionGestureState& gesture = state_->text_selection_gesture_;
  if (event.device_kind != PointerDeviceKind::Touch) {
    return;
  }
  if (event.type == PointerEventType::Down) {
    gesture.long_press_pending = false;
    gesture.tap_pending = false;
    gesture.double_tap_pending = false;
    gesture.double_tap_node.reset();
    if (!state_->mounted_root_) {
      return;
    }
    std::optional<std::uint64_t> gesture_node = state_->focused_node_identity_;
    if (const auto session = state_->pointer_sessions_.find(event.pointer_id);
        session != state_->pointer_sessions_.end() && session->second.focus_pending) {
      gesture_node = session->second.pending_focus_identity;
    }
    if (!gesture_node.has_value()) {
      return;
    }
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *gesture_node);
    if (!focused || !detail::FindTextSelectionClient(*focused)) {
      return;
    }
    constexpr double double_tap_interval = 0.4;
    constexpr float double_tap_slop = 18.0F;
    const double now = state_->platform_->Now();
    if (gesture.previous_tap_time.has_value() && gesture.previous_tap_node == gesture_node &&
        now - *gesture.previous_tap_time >= 0.0 && now - *gesture.previous_tap_time <= double_tap_interval &&
        std::hypot(
            event.position.x - gesture.previous_tap_position.x,
            event.position.y - gesture.previous_tap_position.y
        ) <= double_tap_slop) {
      gesture.previous_tap_time.reset();
      gesture.previous_tap_node.reset();
      gesture.double_tap_pending = true;
      gesture.double_tap_pointer_id = event.pointer_id;
      gesture.double_tap_node = gesture_node;
      return;
    }
    constexpr double delay = 0.5;
    gesture.long_press_pending = true;
    gesture.long_press_pointer_id = event.pointer_id;
    gesture.long_press_position = event.position;
    gesture.long_press_deadline = now + delay;
    gesture.tap_pending = true;
    gesture.tap_pointer_id = event.pointer_id;
    gesture.tap_position = event.position;
    RequestFrameAfter(delay);
    return;
  }
  if (event.type == PointerEventType::Move) {
    const float distance =
        std::hypot(event.position.x - gesture.tap_position.x, event.position.y - gesture.tap_position.y);
    if (gesture.long_press_pending && gesture.long_press_pointer_id == event.pointer_id &&
        distance >= detail::touch_gesture_slop) {
      gesture.long_press_pending = false;
    }
    if (gesture.tap_pending && gesture.tap_pointer_id == event.pointer_id && distance >= 18.0F) {
      gesture.tap_pending = false;
    }
    if (gesture.double_tap_pending && gesture.double_tap_pointer_id == event.pointer_id &&
        distance >= detail::touch_gesture_slop) {
      gesture.double_tap_pending = false;
      gesture.double_tap_node.reset();
    }
    return;
  }
  if (event.type == PointerEventType::Up) {
    if (gesture.double_tap_pending && gesture.double_tap_pointer_id == event.pointer_id &&
        gesture.double_tap_node == state_->focused_node_identity_) {
      gesture.double_tap_pending = false;
      gesture.double_tap_node.reset();
      SelectFocusedTextWord(event.position);
      return;
    }
    if (gesture.tap_pending && gesture.tap_pointer_id == event.pointer_id) {
      gesture.previous_tap_time = state_->platform_->Now();
      gesture.previous_tap_position = event.position;
      gesture.previous_tap_node = state_->focused_node_identity_;
    }
    gesture.tap_pending = false;
    gesture.long_press_pending = false;
    return;
  }
  if (event.type == PointerEventType::Cancel) {
    gesture.tap_pending = false;
    gesture.long_press_pending = false;
    gesture.double_tap_pending = false;
    gesture.double_tap_node.reset();
  }
}

void Runtime::AdvanceTextSelectionLongPress(double timestamp) {
  detail::TextSelectionGestureState& gesture = state_->text_selection_gesture_;
  if (!gesture.long_press_pending) {
    return;
  }
  if (timestamp < gesture.long_press_deadline) {
    RequestFrameAfter(gesture.long_press_deadline - timestamp);
    return;
  }

  const std::int64_t pointer_id = gesture.long_press_pointer_id;
  const Point position = gesture.long_press_position;
  gesture.long_press_pending = false;
  if (const auto session = state_->pointer_sessions_.find(pointer_id); session != state_->pointer_sessions_.end()) {
    CommitPendingTouchFocus(session->second, position);
    RefreshTextInputSession();
  }
  HandlePointerCancel({
      PointerEventType::Cancel,
      pointer_id,
      position,
      PointerDeviceKind::Touch,
  });
  SelectFocusedTextWord(position);
}

} // namespace huxerui
