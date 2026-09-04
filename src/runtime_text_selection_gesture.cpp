#include "runtime_internal.h"
#include "runtime_pointer_internal.h"
#include "runtime_text_internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <variant>

namespace huxerui {
namespace {

class TextSelectionRecognizer final : public detail::GestureRecognizer {
public:
  TextSelectionRecognizer(bool consecutive, const GestureSettings& settings)
      : consecutive_(consecutive), settings_(settings) {}

  detail::GestureDecision Update(const detail::GestureRecognizerInput& input) override {
    using detail::GestureDecision;
    if (input.event.type == PointerEventType::Down) {
      down_position_ = input.window_position;
      if (consecutive_) {
        if (input.event.device_kind != PointerDeviceKind::Touch) {
          return GestureDecision::Accept;
        }
        double_tap_pending_ = true;
      } else if (input.event.device_kind == PointerDeviceKind::Touch) {
        deadline_ = input.timestamp + settings_.long_press_duration.count();
      }
      return GestureDecision::Continue;
    }
    if (std::hypot(input.window_position.x - down_position_.x, input.window_position.y - down_position_.y) >=
        settings_.pointer_slop) {
      deadline_.reset();
      double_tap_pending_ = false;
    }
    if (input.event.type == PointerEventType::Up) {
      deadline_.reset();
      return std::exchange(double_tap_pending_, false) ? GestureDecision::Accept : GestureDecision::Reject;
    }
    return GestureDecision::Continue;
  }

  std::optional<double> Deadline() const noexcept override {
    return deadline_;
  }

  detail::GestureDecision AdvanceDeadline(double timestamp) override {
    if (deadline_.has_value() && timestamp >= *deadline_) {
      deadline_.reset();
      return detail::GestureDecision::Accept;
    }
    return detail::GestureDecision::Continue;
  }

private:
  bool consecutive_;
  GestureSettings settings_;
  Point down_position_;
  bool double_tap_pending_ = false;
  std::optional<double> deadline_;
};

detail::TextSelectionRecognitionState* FindTextSelectionRecognition(detail::PointerSession& session) {
  const auto found = std::ranges::find_if(session.recognitions, [](const detail::PointerRecognition& recognition) {
    return recognition.active &&
           std::holds_alternative<detail::TextSelectionRecognitionState>(recognition.state);
  });
  return found == session.recognitions.end()
             ? nullptr
             : &std::get<detail::TextSelectionRecognitionState>(found->state);
}

} // namespace

std::shared_ptr<detail::GestureRecognizer> detail::TextInteraction::CreateSelectionRecognizer(
    std::uint64_t node, const PointerEvent& event, double timestamp, const GestureSettings& settings
) {
  const auto& gesture = text_selection_gesture_;
  const bool consecutive = gesture.previous_tap_time && gesture.previous_tap_node == node &&
      gesture.previous_tap_device == event.device_kind && timestamp - *gesture.previous_tap_time >= 0.0 &&
      timestamp - *gesture.previous_tap_time <= settings.multi_tap_interval.count() &&
      std::hypot(event.position.x - gesture.previous_tap_position.x,
                 event.position.y - gesture.previous_tap_position.y) <= settings.multi_tap_slop;
  ResetSelectionGesture();
  return std::make_shared<TextSelectionRecognizer>(consecutive, settings);
}

void detail::PointerInteraction::HandleTextSelectionPointerDown(const PointerEvent& event) {
  if (event.type != PointerEventType::Down) {
    return;
  }
  const auto session = pointer_sessions_.find(event.pointer_id);
  if (session == pointer_sessions_.end()) {
    return;
  }
  const auto recognition =
      std::ranges::find_if(session->second.recognitions, [](const detail::PointerRecognition& entry) {
        return std::holds_alternative<detail::TextSelectionRecognitionState>(entry.state);
      });
  if (recognition == session->second.recognitions.end()) {
    runtime_state_.text_->ResetSelectionGesture();
    return;
  }
  auto& selection = std::get<TextSelectionRecognitionState>(recognition->state);
  selection.recognizer = runtime_state_.text_->CreateSelectionRecognizer(
      selection.node_identity, event, runtime_state_.platform_->Now(), runtime_state_.gesture_settings_
  );
  recognition->active = true;
  if (!TrackTextSelectionGesture(event)) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(recognition - session->second.recognitions.begin());
  AcceptPointerRecognition(session->second, index, event);
}

bool detail::PointerInteraction::TrackTextSelectionGesture(const PointerEvent& event) {
  const auto session = pointer_sessions_.find(event.pointer_id);
  if (session == pointer_sessions_.end()) {
    return false;
  }
  detail::TextSelectionRecognitionState* recognition = FindTextSelectionRecognition(session->second);
  if (recognition == nullptr || !recognition->recognizer) {
    return false;
  }
  const double timestamp = runtime_state_.platform_->Now();
  const GestureDecision decision = recognition->recognizer->Update({event, event.position, timestamp});
  if (const std::optional<double> deadline = recognition->recognizer->Deadline()) {
    runtime_state_.owner_.RequestFrameAfter(*deadline - timestamp);
  }
  return decision == GestureDecision::Accept &&
         (event.type != PointerEventType::Up ||
          runtime_state_.focused_node_identity_ == std::optional{recognition->node_identity});
}

void detail::PointerInteraction::RecordTextSelectionTap(
    const detail::PointerSession& session, const PointerEvent& event
) {
  if (event.type != PointerEventType::Up || session.initiating_button != PointerButton::Primary || session.chorded ||
      event.device_kind != session.device_kind ||
      std::hypot(event.position.x - session.down_position.x, event.position.y - session.down_position.y) >
          runtime_state_.gesture_settings_.multi_tap_slop ||
      !runtime_state_.focused_node_identity_.has_value()) {
    return;
  }

  detail::MountedNode* focused = FindNode(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
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
      NodeExtension* node_extension = FindExtension(*runtime_state_.mounted_root_, extension->extension);
      if (extension->extension.node_identity != focused->identity || node_extension == nullptr ||
          node_extension->GetTextSelectionClient() == nullptr) {
        return;
      }
    } else if (!std::holds_alternative<detail::TapRecognitionState>(owner)) {
      return;
    }
  }

  std::vector<detail::MountedNode*> route;
  if (!BuildPointerRoute(*runtime_state_.mounted_root_, event.position, route) ||
      std::ranges::find(route, focused->identity, &detail::MountedNode::identity) == route.end()) {
    return;
  }

  runtime_state_.text_->RememberSelectionTap(event, focused->identity, runtime_state_.platform_->Now());
}

void detail::PointerInteraction::AdvanceTextSelectionLongPress(double timestamp) {
  std::optional<std::int64_t> ready_pointer;
  std::optional<double> ready_deadline;
  Point ready_position;
  std::optional<double> next_deadline;
  for (auto& [pointer_id, session] : pointer_sessions_) {
    if (session.quarantined || session.owner.has_value()) {
      continue;
    }
    detail::TextSelectionRecognitionState* recognition = FindTextSelectionRecognition(session);
    if (recognition == nullptr || !recognition->recognizer) {
      continue;
    }
    const std::optional<double> deadline = recognition->recognizer->Deadline();
    if (!deadline.has_value()) {
      continue;
    }
    if (timestamp >= *deadline) {
      if (!ready_deadline.has_value() || *deadline < *ready_deadline ||
          (*deadline == *ready_deadline && pointer_id < *ready_pointer)) {
        ready_pointer = pointer_id;
        ready_deadline = deadline;
        ready_position = session.down_position;
      }
      continue;
    }
    next_deadline = !next_deadline.has_value()
                        ? deadline
                        : std::optional{std::min(*next_deadline, *deadline)};
  }
  if (!ready_pointer.has_value()) {
    if (next_deadline.has_value()) {
      runtime_state_.owner_.RequestFrameAfter(*next_deadline - timestamp);
    }
    return;
  }

  auto session = pointer_sessions_.find(*ready_pointer);
  if (session == pointer_sessions_.end()) {
    return;
  }
  TextSelectionRecognitionState* pending = FindTextSelectionRecognition(session->second);
  const std::shared_ptr<GestureRecognizer> recognizer = pending ? pending->recognizer : nullptr;
  if (!recognizer || recognizer->AdvanceDeadline(timestamp) != GestureDecision::Accept) {
    return;
  }
  if (!CommitPendingTouchFocus(session->second, ready_position)) {
    return;
  }
  runtime_state_.text_->RefreshTextInputSession();

  // Focus and IME callbacks may cancel the sequence or replace it with another using the same pointer id.
  session = pointer_sessions_.find(*ready_pointer);
  if (session == pointer_sessions_.end() || session->second.quarantined || session->second.owner.has_value()) {
    return;
  }
  const auto recognition = std::ranges::find_if(session->second.recognitions, [&](const PointerRecognition& entry) {
    const auto* selection = std::get_if<TextSelectionRecognitionState>(&entry.state);
    return entry.active && selection && selection->recognizer == recognizer;
  });
  if (recognition != session->second.recognitions.end()) {
    const std::size_t index = static_cast<std::size_t>(recognition - session->second.recognitions.begin());
    const PointerEvent event{PointerEventType::Move, *ready_pointer, ready_position, PointerDeviceKind::Touch};
    AcceptPointerRecognition(session->second, index, event);
  }
}

} // namespace huxerui
