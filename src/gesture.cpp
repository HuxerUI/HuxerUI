#include <huxerui/gesture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "internal.h"
#include "numeric_constants.h"

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
  double maximum_interval_ = detail::long_press_minimum_interval;
  float maximum_movement_ = detail::multi_tap_movement_slop;
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
  std::shared_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(MountedNode&, const PointerEvent&, double, const GestureSettings& settings,
                          Transform2D) override {
    return std::make_shared<MultiTapRecognizer>(gesture_, settings);
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
  float maximum_movement_ = detail::touch_gesture_slop;
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
  std::shared_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(MountedNode&, const PointerEvent& event, double timestamp,
                          const GestureSettings& settings, Transform2D) override {
    return std::make_shared<LongPressRecognizer>(gesture_, settings, event, timestamp);
  }

  LongPressGesture gesture_;
};

class DragRecognizer final : public detail::DragSourceRecognizer {
public:
  DragRecognizer(const DragGesture& gesture, const GestureSettings& settings, const PointerEvent& event,
                 double timestamp, bool publish_events = true)
      : axis_(gesture.axis),
        minimum_distance_(gesture.minimum_distance.value_or(settings.pointer_slop)),
        delayed_tolerance_(settings.pointer_slop),
        down_origin_(event.position),
        origin_(event.position),
        previous_(event.position),
        publish_events_(publish_events) {
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
    current_event_ = BuildEvent(input, {});
    if (publish_events_) {
      detail::EmitEvent<DragEvents::Started>(node.event_bindings, current_event_);
    }
    const Point translation = Difference(Constrain(input.event.position), origin_);
    if (accepted_ && !accepted_from_deadline_ && (translation.x != 0.0F || translation.y != 0.0F)) {
      if (publish_events_) {
        current_event_ = BuildEvent(input, translation);
        detail::EmitEvent<DragEvents::Changed>(node.event_bindings, current_event_);
      }
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
    current_event_ = BuildEvent(input, delta);
    if (input.event.type == PointerEventType::Move) {
      if (publish_events_) {
        detail::EmitEvent<DragEvents::Changed>(node.event_bindings, current_event_);
      }
      previous_ = position;
    } else if (input.event.type == PointerEventType::Up) {
      accepted_ = false;
      if (publish_events_) {
        detail::EmitEvent<DragEvents::Ended>(node.event_bindings, current_event_);
      }
    }
  }

  void Canceled(detail::MountedNode& node, NodeExtension&, const detail::GestureRecognizerInput& input) override {
    if (!accepted_) {
      return;
    }
    accepted_ = false;
    current_event_ = BuildEvent(input, {});
    if (publish_events_) {
      detail::EmitEvent<DragEvents::Canceled>(node.event_bindings, current_event_);
    }
  }

  const DragEvent& CurrentEvent() const noexcept override {
    return current_event_;
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
    constexpr double maximum_age = detail::velocity_sample_max_age;
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
  float minimum_distance_ = detail::touch_gesture_slop;
  float delayed_tolerance_ = detail::touch_gesture_slop;
  Point down_origin_;
  Point origin_;
  Point previous_;
  std::optional<double> deadline_;
  std::array<Sample, 8> samples_;
  std::size_t sample_count_ = 0;
  bool accepted_ = false;
  bool accepted_from_deadline_ = false;
  bool publish_events_ = true;
  DragEvent current_event_;
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
  std::shared_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(MountedNode&, const PointerEvent& event, double timestamp,
                          const GestureSettings& settings, Transform2D) override {
    return std::make_shared<DragRecognizer>(gesture_, settings, event, timestamp);
  }

  DragGesture gesture_;
};

class TransformRecognizer final : public detail::GestureRecognizer {
public:
  TransformRecognizer(PointerDeviceKind device_kind, Transform2D frozen_node_to_window)
      : device_kind_(device_kind), frozen_node_to_window_(frozen_node_to_window) {}

  [[nodiscard]] bool CanJoin(PointerDeviceKind device_kind) const noexcept {
    return !closed_ && device_kind == device_kind_;
  }

  detail::GestureDecision Update(const detail::GestureRecognizerInput& input) override {
    if (!CanJoin(input.event.device_kind)) {
      return detail::GestureDecision::Reject;
    }
    if (input.event.type == PointerEventType::Down) {
      contacts_.push_back({input.event.pointer_id, input.window_position});
      return contacts_.size() >= 2 ? detail::GestureDecision::Accept : detail::GestureDecision::Continue;
    }

    Contact* contact = FindContact(input.event.pointer_id);
    if (!contact) {
      return detail::GestureDecision::Reject;
    }
    contact->position = input.window_position;
    if (input.event.type == PointerEventType::Up || input.event.type == PointerEventType::Cancel) {
      std::erase_if(contacts_, [&](const Contact& value) { return value.pointer_id == input.event.pointer_id; });
      return detail::GestureDecision::Reject;
    }
    return active_ ? detail::GestureDecision::Accept : detail::GestureDecision::Continue;
  }

  void Accepted(detail::MountedNode& node, NodeExtension&, const detail::GestureRecognizerInput&) override {
    if (closed_ || contacts_.size() < 2) {
      return;
    }
    Rebase();
    const TransformEvent event = IdentityEvent();
    if (active_) {
      detail::EmitEvent<TransformEvents::Changed>(node.event_bindings, event);
      return;
    }
    active_ = true;
    detail::EmitEvent<TransformEvents::Started>(node.event_bindings, event);
  }

  void UpdateAccepted(detail::MountedNode& node, NodeExtension&,
                      const detail::GestureRecognizerInput& input) override {
    if (!active_ || closed_) {
      return;
    }
    Contact* contact = FindContact(input.event.pointer_id);
    if (!contact) {
      return;
    }
    contact->position = input.window_position;
    if (input.event.type == PointerEventType::Move) {
      const TransformEvent event = DeltaEvent();
      Rebase();
      detail::EmitEvent<TransformEvents::Changed>(node.event_bindings, event);
      return;
    }
    if (input.event.type != PointerEventType::Up) {
      return;
    }

    std::erase_if(contacts_, [&](const Contact& value) { return value.pointer_id == input.event.pointer_id; });
    if (contacts_.size() >= 2) {
      Rebase();
      detail::EmitEvent<TransformEvents::Changed>(node.event_bindings, IdentityEvent());
      return;
    }

    const TransformEvent event = IdentityEvent();
    active_ = false;
    closed_ = true;
    contacts_.clear();
    previous_contacts_.clear();
    detail::EmitEvent<TransformEvents::Ended>(node.event_bindings, event);
  }

  void Canceled(detail::MountedNode& node, NodeExtension&,
                const detail::GestureRecognizerInput& input) override {
    if (!active_) {
      std::erase_if(contacts_, [&](const Contact& value) { return value.pointer_id == input.event.pointer_id; });
      return;
    }
    if (Contact* contact = FindContact(input.event.pointer_id)) {
      contact->position = input.window_position;
    }
    const TransformEvent event = IdentityEvent();
    active_ = false;
    closed_ = true;
    contacts_.clear();
    previous_contacts_.clear();
    detail::EmitEvent<TransformEvents::Canceled>(node.event_bindings, event);
  }

private:
  struct Contact {
    std::int64_t pointer_id = 0;
    Point position;
  };

  struct Geometry {
    Point centroid;
    float mean_square_radius = 0.0F;
  };

  Contact* FindContact(std::int64_t pointer_id) {
    const auto found = std::ranges::find(contacts_, pointer_id, &Contact::pointer_id);
    return found == contacts_.end() ? nullptr : &*found;
  }

  static Geometry Measure(const std::vector<Contact>& contacts) {
    Geometry geometry;
    if (contacts.empty()) {
      return geometry;
    }
    for (const Contact& contact : contacts) {
      geometry.centroid.x += contact.position.x;
      geometry.centroid.y += contact.position.y;
    }
    const float count = static_cast<float>(contacts.size());
    geometry.centroid.x /= count;
    geometry.centroid.y /= count;
    for (const Contact& contact : contacts) {
      const float x = contact.position.x - geometry.centroid.x;
      const float y = contact.position.y - geometry.centroid.y;
      geometry.mean_square_radius += x * x + y * y;
    }
    geometry.mean_square_radius /= count;
    return geometry;
  }

  Point Local(Point window_position) const {
    return frozen_node_to_window_.Inverse(window_position).value_or(window_position);
  }

  TransformEvent IdentityEvent() const {
    const Point window_centroid = Measure(contacts_).centroid;
    return {
        device_kind_,
        static_cast<std::uint32_t>(contacts_.size()),
        Local(window_centroid),
        window_centroid,
        {},
        1.0F,
        0.0F,
    };
  }

  TransformEvent DeltaEvent() const {
    if (contacts_.size() != previous_contacts_.size() || contacts_.size() < 2) {
      return IdentityEvent();
    }
    const Geometry previous = Measure(previous_contacts_);
    const Geometry current = Measure(contacts_);
    float dot = 0.0F;
    float cross = 0.0F;
    for (std::size_t index = 0; index < contacts_.size(); ++index) {
      const Point from{
          previous_contacts_[index].position.x - previous.centroid.x,
          previous_contacts_[index].position.y - previous.centroid.y,
      };
      const Point to{
          contacts_[index].position.x - current.centroid.x,
          contacts_[index].position.y - current.centroid.y,
      };
      dot += from.x * to.x + from.y * to.y;
      cross += from.x * to.y - from.y * to.x;
    }
    const float scale = previous.mean_square_radius > detail::transform_epsilon
                            ? std::sqrt(current.mean_square_radius / previous.mean_square_radius)
                            : 1.0F;
    const float rotation = std::abs(dot) > detail::transform_epsilon || std::abs(cross) > detail::transform_epsilon
                               ? std::atan2(cross, dot)
                               : 0.0F;
    const Point previous_local = Local(previous.centroid);
    const Point current_local = Local(current.centroid);
    return {
        device_kind_,
        static_cast<std::uint32_t>(contacts_.size()),
        current_local,
        current.centroid,
        {current_local.x - previous_local.x, current_local.y - previous_local.y},
        scale,
        rotation,
    };
  }

  void Rebase() {
    previous_contacts_ = contacts_;
  }

  PointerDeviceKind device_kind_ = PointerDeviceKind::Touch;
  Transform2D frozen_node_to_window_;
  std::vector<Contact> contacts_;
  std::vector<Contact> previous_contacts_;
  bool active_ = false;
  bool closed_ = false;
};

class TransformExtension final : public NodeExtension {
public:
  TransformExtension(MountedNode&, const TransformGesture&) {}

  void Update(MountedNode&, const TransformGesture&) {}

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

private:
  static std::size_t DeviceIndex(PointerDeviceKind device_kind) {
    switch (device_kind) {
    case PointerDeviceKind::Mouse:
      return 0;
    case PointerDeviceKind::Touch:
      return 1;
    case PointerDeviceKind::Pen:
      return 2;
    }
    throw std::logic_error("HuxerUI pointer device kind is invalid");
  }

  std::shared_ptr<detail::GestureRecognizer>
  CreateGestureRecognizer(MountedNode&, const PointerEvent& event, double, const GestureSettings&,
                          Transform2D frozen_node_to_window) override {
    const std::size_t device_index = DeviceIndex(event.device_kind);
    std::shared_ptr<TransformRecognizer> recognizer = recognizers_[device_index].lock();
    if (!recognizer || !recognizer->CanJoin(event.device_kind)) {
      recognizer = std::make_shared<TransformRecognizer>(event.device_kind, frozen_node_to_window);
      recognizers_[device_index] = recognizer;
    }
    return recognizer;
  }

  std::array<std::weak_ptr<TransformRecognizer>, 3> recognizers_;
};

} // namespace

namespace detail {

class DragSourceExtension final : public NodeExtension {
public:
  DragSourceExtension(huxerui::MountedNode&, const DragSource& source) {
    UpdateCapability(source);
  }

  void Update(huxerui::MountedNode&, const DragSource& source) {
    UpdateCapability(source);
  }

  bool HitTest(huxerui::MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

private:
  std::shared_ptr<GestureRecognizer>
  CreateGestureRecognizer(huxerui::MountedNode&, const PointerEvent& event, double timestamp,
                          const GestureSettings& settings, Transform2D) override {
    return std::make_shared<DragRecognizer>(gesture_, settings, event, timestamp, false);
  }

  const DragSourceCapability* GetDragSourceCapability() const noexcept override {
    return &capability_;
  }

  void UpdateCapability(const DragSource& source) {
    Validate(source.gesture_);
    gesture_ = source.gesture_;
    capability_.payload_type = source.payload_type_;
    capability_.payload = source.payload_;
    capability_.preview = source.preview_;
  }

  DragGesture gesture_;
  DragSourceCapability capability_;
};

class DropTargetExtension final : public NodeExtension {
public:
  DropTargetExtension(huxerui::MountedNode&, const DropTarget& target) {
    UpdateCapability(target);
  }

  void Update(huxerui::MountedNode&, const DropTarget& target) {
    UpdateCapability(target);
  }

  bool HitTest(huxerui::MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

private:
  const DropTargetCapability* GetDropTargetCapability() const noexcept override {
    return &capability_;
  }

  void UpdateCapability(const DropTarget& target) {
    capability_.payload_type = target.payload_type_;
    capability_.accepts = target.accepts_;
    capability_.dispatch = target.dispatch_;
  }

  DropTargetCapability capability_;
};

} // namespace detail

std::shared_ptr<detail::GestureRecognizer>
NodeExtension::CreateGestureRecognizer(MountedNode&, const PointerEvent&, double, const GestureSettings&,
                                       Transform2D) {
  return {};
}

const detail::DragSourceCapability* NodeExtension::GetDragSourceCapability() const noexcept {
  return nullptr;
}

const detail::DropTargetCapability* NodeExtension::GetDropTargetCapability() const noexcept {
  return nullptr;
}

DragSource::DragSource(std::type_index payload_type, std::shared_ptr<const void> payload,
                       std::function<View()> preview, DragGesture gesture)
    : payload_type_(payload_type), payload_(std::move(payload)), preview_(std::move(preview)),
      gesture_(std::move(gesture)) {
  if (!payload_) {
    throw std::invalid_argument("HuxerUI drag source payload must not be empty");
  }
  Validate(gesture_);
}

DropTarget::DropTarget(std::type_index payload_type, std::function<bool(const void*)> accepts,
                       detail::DropTargetDispatch dispatch)
    : payload_type_(payload_type), accepts_(std::move(accepts)), dispatch_(dispatch) {
  if (!accepts_) {
    throw std::invalid_argument("HuxerUI drop target predicate must not be empty");
  }
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

const detail::ModifierDescriptor& DragSource::Descriptor() {
  return detail::ModifierDescriptorFor<DragSource, detail::DragSourceExtension>();
}

const detail::ModifierDescriptor& DropTarget::Descriptor() {
  return detail::ModifierDescriptorFor<DropTarget, detail::DropTargetExtension>();
}

const detail::ModifierDescriptor& TransformGesture::Descriptor() {
  return detail::ModifierDescriptorFor<TransformGesture, TransformExtension>();
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
