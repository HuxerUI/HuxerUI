#include <huxerui/gesture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "internal.h"

namespace huxerui {
namespace {

bool FiniteNonnegative(float value) {
  return std::isfinite(value) && value >= 0.0F;
}

bool FiniteNonnegative(std::chrono::duration<double> value) {
  return std::isfinite(value.count()) && value.count() >= 0.0;
}

bool FinitePositive(std::chrono::duration<double> value) {
  return std::isfinite(value.count()) && value.count() > 0.0;
}

void Validate(const MultiTapGesture& gesture) {
  if (gesture.count < 2) {
    throw std::invalid_argument("HuxerUI multi-tap count must be at least two");
  }
  if (gesture.maximum_interval.has_value() && !FinitePositive(*gesture.maximum_interval)) {
    throw std::invalid_argument("HuxerUI multi-tap interval must be finite and positive");
  }
  if (gesture.maximum_movement.has_value() && !FiniteNonnegative(*gesture.maximum_movement)) {
    throw std::invalid_argument("HuxerUI multi-tap movement must be finite and nonnegative");
  }
}

void Validate(const LongPressGesture& gesture) {
  if (gesture.minimum_duration.has_value() && !FiniteNonnegative(*gesture.minimum_duration)) {
    throw std::invalid_argument("HuxerUI long-press duration must be finite and nonnegative");
  }
  if (gesture.maximum_movement.has_value() && !FiniteNonnegative(*gesture.maximum_movement)) {
    throw std::invalid_argument("HuxerUI long-press movement must be finite and nonnegative");
  }
}

void Validate(const DragGesture& gesture) {
  if (gesture.minimum_distance.has_value() && !FiniteNonnegative(*gesture.minimum_distance)) {
    throw std::invalid_argument("HuxerUI drag distance must be finite and nonnegative");
  }
  if (gesture.minimum_press_duration.has_value() && !FiniteNonnegative(*gesture.minimum_press_duration)) {
    throw std::invalid_argument("HuxerUI drag press duration must be finite and nonnegative");
  }
}

class MultiTapExtension;

class MultiTapRecognizer final : public detail::GestureRecognizer {
public:
  MultiTapRecognizer(MultiTapGesture gesture, const GestureSettings& settings)
      : count_(gesture.count),
        maximum_interval_(gesture.maximum_interval.value_or(settings.multi_tap_interval).count()),
        maximum_movement_(gesture.maximum_movement.value_or(settings.multi_tap_slop)) {}

  bool SharesTap() const noexcept override {
    return true;
  }

  detail::GestureDecision Update(const detail::GestureRecognizerInput&) override {
    return detail::GestureDecision::Continue;
  }

  void Canceled(detail::MountedNode& node, NodeExtension& extension,
                const detail::GestureRecognizerInput& input) override;

  void TapAccepted(detail::MountedNode& node, NodeExtension& extension,
                   const detail::GestureRecognizerInput& input) override;

private:
  std::uint32_t count_ = 2;
  double maximum_interval_ = 0.3;
  float maximum_movement_ = 18.0F;
};

class MultiTapExtension final : public NodeExtension {
public:
  MultiTapExtension(MountedNode&, const MultiTapGesture& gesture) : gesture_(gesture) {
    Validate(gesture_);
  }

  void Update(MountedNode&, const MultiTapGesture& gesture) {
    Validate(gesture);
    if (gesture_ != gesture) {
      Reset();
      gesture_ = gesture;
    }
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  void Reset() {
    completed_taps_ = 0;
    previous_timestamp_.reset();
  }

  void RecordTap(detail::MountedNode& node, const detail::GestureRecognizerInput& input, std::uint32_t count,
                 double maximum_interval, float maximum_movement) {
    const bool continues = previous_timestamp_.has_value() && previous_device_ == input.event.device_kind &&
                           input.timestamp >= *previous_timestamp_ &&
                           input.timestamp - *previous_timestamp_ <= maximum_interval &&
                           std::hypot(input.window_position.x - previous_window_position_.x,
                                      input.window_position.y - previous_window_position_.y) <= maximum_movement;
    completed_taps_ = continues ? completed_taps_ + 1 : 1;
    previous_timestamp_ = input.timestamp;
    previous_window_position_ = input.window_position;
    previous_device_ = input.event.device_kind;
    if (completed_taps_ < count) {
      return;
    }

    const MultiTapEvent event{
        input.event.pointer_id,
        input.event.device_kind,
        input.event.position,
        input.window_position,
        count,
    };
    Reset();
    detail::EmitEvent<MultiTapEvents::Recognized>(node.event_bindings, event);
  }

private:
  std::unique_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(MountedNode&, const PointerEvent&, double, const GestureSettings& settings) override {
    return std::make_unique<MultiTapRecognizer>(gesture_, settings);
  }

  MultiTapGesture gesture_;
  std::uint32_t completed_taps_ = 0;
  std::optional<double> previous_timestamp_;
  Point previous_window_position_;
  PointerDeviceKind previous_device_ = PointerDeviceKind::Mouse;
};

void MultiTapRecognizer::Canceled(detail::MountedNode&, NodeExtension& extension,
                                  const detail::GestureRecognizerInput&) {
  static_cast<MultiTapExtension&>(extension).Reset();
}

void MultiTapRecognizer::TapAccepted(detail::MountedNode& node, NodeExtension& extension,
                                     const detail::GestureRecognizerInput& input) {
  static_cast<MultiTapExtension&>(extension).RecordTap(node, input, count_, maximum_interval_, maximum_movement_);
}

class LongPressRecognizer final : public detail::GestureRecognizer {
public:
  LongPressRecognizer(const LongPressGesture& gesture, const GestureSettings& settings, const PointerEvent& event,
                      double timestamp)
      : origin_(event.position),
        maximum_movement_(gesture.maximum_movement.value_or(settings.pointer_slop)),
        deadline_(timestamp + gesture.minimum_duration.value_or(settings.long_press_duration).count()) {}

  detail::GestureDecision Update(const detail::GestureRecognizerInput& input) override {
    if (input.event.type == PointerEventType::Move &&
        std::hypot(input.event.position.x - origin_.x, input.event.position.y - origin_.y) > maximum_movement_) {
      deadline_.reset();
      return detail::GestureDecision::Reject;
    }
    if (input.event.type == PointerEventType::Up || input.event.type == PointerEventType::Cancel) {
      deadline_.reset();
      return detail::GestureDecision::Reject;
    }
    return detail::GestureDecision::Continue;
  }

  std::optional<double> Deadline() const noexcept override {
    return deadline_;
  }

  detail::GestureDecision AdvanceDeadline(double timestamp) override {
    if (!deadline_.has_value() || timestamp < *deadline_) {
      return detail::GestureDecision::Continue;
    }
    deadline_.reset();
    return detail::GestureDecision::Accept;
  }

  void Accepted(detail::MountedNode& node, NodeExtension&, const detail::GestureRecognizerInput& input) override {
    accepted_ = true;
    const LongPressEvent event{
        input.event.pointer_id, input.event.device_kind, input.event.position, input.window_position,
    };
    detail::EmitEvent<LongPressEvents::Started>(node.event_bindings, event);
  }

  void UpdateAccepted(detail::MountedNode& node, NodeExtension&,
                      const detail::GestureRecognizerInput& input) override {
    if (input.event.type != PointerEventType::Up || !accepted_) {
      return;
    }
    accepted_ = false;
    const LongPressEvent event{
        input.event.pointer_id, input.event.device_kind, input.event.position, input.window_position,
    };
    detail::EmitEvent<LongPressEvents::Ended>(node.event_bindings, event);
  }

  void Canceled(detail::MountedNode& node, NodeExtension&, const detail::GestureRecognizerInput& input) override {
    if (!accepted_) {
      return;
    }
    accepted_ = false;
    const LongPressEvent event{
        input.event.pointer_id, input.event.device_kind, input.event.position, input.window_position,
    };
    detail::EmitEvent<LongPressEvents::Canceled>(node.event_bindings, event);
  }

private:
  Point origin_;
  float maximum_movement_ = 6.0F;
  std::optional<double> deadline_;
  bool accepted_ = false;
};

class LongPressExtension final : public NodeExtension {
public:
  LongPressExtension(MountedNode&, const LongPressGesture& gesture) : gesture_(gesture) {
    Validate(gesture_);
  }

  void Update(MountedNode&, const LongPressGesture& gesture) {
    Validate(gesture);
    gesture_ = gesture;
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

private:
  std::unique_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(MountedNode&, const PointerEvent& event, double timestamp,
                          const GestureSettings& settings) override {
    return std::make_unique<LongPressRecognizer>(gesture_, settings, event, timestamp);
  }

  LongPressGesture gesture_;
};

class DragRecognizer final : public detail::GestureRecognizer {
public:
  DragRecognizer(const DragGesture& gesture, const GestureSettings& settings, const PointerEvent& event,
                 double timestamp)
      : axis_(gesture.axis),
        minimum_distance_(gesture.minimum_distance.value_or(settings.pointer_slop)),
        delayed_tolerance_(settings.pointer_slop),
        down_origin_(event.position),
        origin_(event.position),
        previous_(event.position) {
    if (gesture.minimum_press_duration.has_value() && gesture.minimum_press_duration->count() > 0.0) {
      deadline_ = timestamp + gesture.minimum_press_duration->count();
    }
    Record(event.position, timestamp);
  }

  detail::GestureDecision Update(const detail::GestureRecognizerInput& input) override {
    Record(input.event.position, input.timestamp);
    if (input.event.type == PointerEventType::Up || input.event.type == PointerEventType::Cancel) {
      deadline_.reset();
      return detail::GestureDecision::Reject;
    }
    if (input.event.type != PointerEventType::Move) {
      return detail::GestureDecision::Continue;
    }

    const Point translation = Difference(input.event.position, down_origin_);
    const float distance = axis_.has_value()
                               ? std::abs(*axis_ == Axis::Horizontal ? translation.x : translation.y)
                               : std::hypot(translation.x, translation.y);
    if (deadline_.has_value()) {
      if (input.timestamp >= *deadline_) {
        deadline_.reset();
        accepted_from_deadline_ = true;
        return detail::GestureDecision::Accept;
      }
      if (std::hypot(translation.x, translation.y) > delayed_tolerance_) {
        deadline_.reset();
        return detail::GestureDecision::Reject;
      }
      return detail::GestureDecision::Continue;
    }
    if (axis_.has_value()) {
      const float cross = std::abs(*axis_ == Axis::Horizontal ? translation.y : translation.x);
      if (cross > distance) {
        return detail::GestureDecision::Continue;
      }
    }
    return distance >= minimum_distance_ ? detail::GestureDecision::Accept : detail::GestureDecision::Continue;
  }

  std::optional<double> Deadline() const noexcept override {
    return deadline_;
  }

  detail::GestureDecision AdvanceDeadline(double timestamp) override {
    if (!deadline_.has_value() || timestamp < *deadline_) {
      return detail::GestureDecision::Continue;
    }
    deadline_.reset();
    accepted_from_deadline_ = true;
    return detail::GestureDecision::Accept;
  }

  void Accepted(detail::MountedNode& node, NodeExtension&, const detail::GestureRecognizerInput& input) override {
    accepted_ = true;
    if (accepted_from_deadline_) {
      origin_ = input.event.position;
      previous_ = origin_;
      sample_count_ = 0;
      Record(origin_, input.timestamp);
    }
    detail::EmitEvent<DragEvents::Started>(node.event_bindings, BuildEvent(input, {}));
    const Point translation = Difference(Constrain(input.event.position), origin_);
    if (accepted_ && !accepted_from_deadline_ && (translation.x != 0.0F || translation.y != 0.0F)) {
      detail::EmitEvent<DragEvents::Changed>(node.event_bindings, BuildEvent(input, translation));
      previous_ = Constrain(input.event.position);
    }
  }

  void UpdateAccepted(detail::MountedNode& node, NodeExtension&,
                      const detail::GestureRecognizerInput& input) override {
    if (!accepted_) {
      return;
    }
    Record(input.event.position, input.timestamp);
    const Point position = Constrain(input.event.position);
    const Point delta = Difference(position, previous_);
    if (input.event.type == PointerEventType::Move) {
      detail::EmitEvent<DragEvents::Changed>(node.event_bindings, BuildEvent(input, delta));
      previous_ = position;
    } else if (input.event.type == PointerEventType::Up) {
      accepted_ = false;
      detail::EmitEvent<DragEvents::Ended>(node.event_bindings, BuildEvent(input, delta));
    }
  }

  void Canceled(detail::MountedNode& node, NodeExtension&, const detail::GestureRecognizerInput& input) override {
    if (!accepted_) {
      return;
    }
    accepted_ = false;
    detail::EmitEvent<DragEvents::Canceled>(node.event_bindings, BuildEvent(input, {}));
  }

private:
  struct Sample {
    Point position;
    double timestamp = 0.0;
  };

  static Point Difference(Point value, Point origin) {
    return {value.x - origin.x, value.y - origin.y};
  }

  Point Constrain(Point value) const {
    if (axis_ == Axis::Horizontal) {
      value.y = origin_.y;
    } else if (axis_ == Axis::Vertical) {
      value.x = origin_.x;
    }
    return value;
  }

  void Record(Point position, double timestamp) {
    if (!std::isfinite(timestamp)) {
      return;
    }
    position = Constrain(position);
    if (sample_count_ > 0 && timestamp == samples_[sample_count_ - 1].timestamp) {
      samples_[sample_count_ - 1].position = position;
      return;
    }
    if (sample_count_ > 0 && timestamp < samples_[sample_count_ - 1].timestamp) {
      sample_count_ = 0;
    }
    if (sample_count_ == samples_.size()) {
      std::move(samples_.begin() + 1, samples_.end(), samples_.begin());
      --sample_count_;
    }
    samples_[sample_count_++] = {position, timestamp};
  }

  Point Velocity(double timestamp) const {
    constexpr double maximum_age = 0.1;
    if (sample_count_ < 2 || !std::isfinite(timestamp)) {
      return {};
    }
    const Sample& latest = samples_[sample_count_ - 1];
    std::size_t first = sample_count_ - 2;
    while (first > 0 && latest.timestamp - samples_[first - 1].timestamp <= maximum_age) {
      --first;
    }
    const double elapsed = latest.timestamp - samples_[first].timestamp;
    if (!std::isfinite(elapsed) || elapsed <= 0.0 || timestamp - latest.timestamp > maximum_age) {
      return {};
    }
    const Point distance = Difference(latest.position, samples_[first].position);
    return {
        static_cast<float>(distance.x / elapsed),
        static_cast<float>(distance.y / elapsed),
    };
  }

  DragEvent BuildEvent(const detail::GestureRecognizerInput& input, Point delta) const {
    const Point position = Constrain(input.event.position);
    return {
        input.event.pointer_id,
        input.event.device_kind,
        origin_,
        position,
        input.window_position,
        delta,
        Difference(position, origin_),
        Velocity(input.timestamp),
    };
  }

  std::optional<Axis> axis_;
  float minimum_distance_ = 6.0F;
  float delayed_tolerance_ = 6.0F;
  Point down_origin_;
  Point origin_;
  Point previous_;
  std::optional<double> deadline_;
  std::array<Sample, 8> samples_;
  std::size_t sample_count_ = 0;
  bool accepted_ = false;
  bool accepted_from_deadline_ = false;
};

class DragExtension final : public NodeExtension {
public:
  DragExtension(MountedNode&, const DragGesture& gesture) : gesture_(gesture) {
    Validate(gesture_);
  }

  void Update(MountedNode&, const DragGesture& gesture) {
    Validate(gesture);
    gesture_ = gesture;
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

private:
  std::unique_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(MountedNode&, const PointerEvent& event, double timestamp,
                          const GestureSettings& settings) override {
    return std::make_unique<DragRecognizer>(gesture_, settings, event, timestamp);
  }

  DragGesture gesture_;
};

} // namespace

std::unique_ptr<detail::GestureRecognizer>
NodeExtension::CreateGestureRecognizer(MountedNode&, const PointerEvent&, double, const GestureSettings&) {
  return {};
}

const detail::ModifierDescriptor& MultiTapGesture::Descriptor() {
  return detail::ModifierDescriptorFor<MultiTapGesture, MultiTapExtension>();
}

const detail::ModifierDescriptor& LongPressGesture::Descriptor() {
  return detail::ModifierDescriptorFor<LongPressGesture, LongPressExtension>();
}

const detail::ModifierDescriptor& DragGesture::Descriptor() {
  return detail::ModifierDescriptorFor<DragGesture, DragExtension>();
}

namespace detail {

void ValidateGestureSettings(const GestureSettings& settings) {
  if (!FiniteNonnegative(settings.pointer_slop) || !FiniteNonnegative(settings.multi_tap_slop) ||
      !FinitePositive(settings.multi_tap_interval) || !FiniteNonnegative(settings.long_press_duration)) {
    throw std::logic_error("HuxerUI platform gesture defaults must be finite and valid");
  }
}

} // namespace detail
} // namespace huxerui
