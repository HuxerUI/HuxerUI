#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <typeindex>
#include <utility>

#include <huxerui/event.h>
#include <huxerui/layout.h>
#include <huxerui/modifier.h>

namespace huxerui {

class View;

// Platform-provided recognition defaults used when a gesture does not declare an explicit override.
struct GestureSettings {
  // General movement tolerance in logical pixels for long press, delayed drag, and similar recognition.
  float pointer_slop = 6.0F;
  // Maximum distance in window logical pixels between consecutive taps in one multi-tap sequence.
  float multi_tap_slop = 18.0F;
  // Maximum elapsed time between consecutive taps in one multi-tap sequence.
  std::chrono::duration<double> multi_tap_interval{0.3};
  // Time a pointer must remain within pointer_slop before a default long press is recognized.
  std::chrono::duration<double> long_press_duration{0.5};

  bool operator==(const GestureSettings&) const = default;
};

// Recognizes a configured number of consecutive physical taps without replacing ordinary Click handling.
struct MultiTapGesture {
  static const detail::ModifierDescriptor& Descriptor();

  // Required tap count. Values below two are invalid.
  std::uint32_t count = 2;
  // Per-gesture interval override; an empty value uses GestureSettings::multi_tap_interval.
  std::optional<std::chrono::duration<double>> maximum_interval;
  // Per-gesture window-space movement override; an empty value uses GestureSettings::multi_tap_slop.
  std::optional<float> maximum_movement;

  bool operator==(const MultiTapGesture&) const = default;
};

// Describes the final tap that completed a MultiTapGesture.
struct MultiTapEvent {
  // Pointer identifier of the final physical tap, not a stable identifier for the complete multi-tap sequence.
  std::int64_t pointer_id = 0;
  // Device kind shared by the successful taps; changing device kind starts a new sequence.
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  // Final tap position in the modifier owner's node-local logical coordinate space.
  Point position;
  // Final tap position in host-window logical coordinates.
  Point window_position;
  // Number of taps recognized, matching the corresponding MultiTapGesture::count.
  std::uint32_t count = 2;

  bool operator==(const MultiTapEvent&) const = default;
};

// Typed event keys emitted by MultiTapGesture.
struct MultiTapEvents {
  // Emitted once after the configured number of consecutive taps succeeds.
  struct Recognized : Event<void(const MultiTapEvent&)> {};
};

// Recognizes a pointer held within a movement tolerance for a minimum duration.
struct LongPressGesture {
  static const detail::ModifierDescriptor& Descriptor();

  // Per-gesture duration override; an empty value uses GestureSettings::long_press_duration.
  std::optional<std::chrono::duration<double>> minimum_duration;
  // Node-local movement override in logical pixels; an empty value uses GestureSettings::pointer_slop.
  std::optional<float> maximum_movement;

  bool operator==(const LongPressGesture&) const = default;
};

// Describes one lifecycle transition of a recognized LongPressGesture.
struct LongPressEvent {
  // Pointer sequence that owns the recognized long press.
  std::int64_t pointer_id = 0;
  // Device kind reported by the owning pointer sequence.
  PointerDeviceKind device_kind = PointerDeviceKind::Touch;
  // Current position in the modifier owner's node-local logical coordinate space.
  Point position;
  // Current position in host-window logical coordinates.
  Point window_position;

  bool operator==(const LongPressEvent&) const = default;
};

// Typed lifecycle event keys emitted after a LongPressGesture wins the pointer sequence.
struct LongPressEvents {
  // Emitted when the duration elapses and the long press obtains pointer ownership.
  struct Started : Event<void(const LongPressEvent&)> {};
  // Emitted on pointer Up after Started.
  struct Ended : Event<void(const LongPressEvent&)> {};
  // Emitted when an accepted long press loses ownership without a normal Up.
  struct Canceled : Event<void(const LongPressEvent&)> {};
};

// Recognizes pointer translation, optionally constrained to one axis or delayed until a press duration elapses.
struct DragGesture {
  static const detail::ModifierDescriptor& Descriptor();

  // Constrains recognition and reported local movement to one axis; an empty value allows two-dimensional movement.
  std::optional<Axis> axis;
  // Movement required to accept a normal drag, in logical pixels; an empty value uses GestureSettings::pointer_slop.
  std::optional<float> minimum_distance;
  // A positive value creates a press-then-drag gesture. The pointer must remain within the platform pointer slop until
  // the duration elapses, after which the current position becomes the drag origin.
  std::optional<std::chrono::duration<double>> minimum_press_duration;

  bool operator==(const DragGesture&) const = default;
};

// Describes one lifecycle update from a recognized DragGesture.
struct DragEvent {
  // Pointer sequence that owns the drag.
  std::int64_t pointer_id = 0;
  // Device kind reported by the owning pointer sequence.
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  // Node-local drag origin. This is Down for a normal drag and the accepted position for a delayed drag.
  Point origin;
  // Current node-local position, constrained to DragGesture::axis when one is configured.
  Point position;
  // Current unconstrained position in host-window logical coordinates.
  Point window_position;
  // Axis-constrained displacement since the preceding emitted drag update, in logical pixels.
  Point delta;
  // Axis-constrained displacement from origin to position, in logical pixels.
  Point translation;
  // Recent axis-constrained pointer velocity in logical pixels per second.
  Point velocity;

  bool operator==(const DragEvent&) const = default;
};

// Typed lifecycle event keys emitted after a DragGesture wins the pointer sequence.
struct DragEvents {
  // Emitted once after the distance threshold or optional press duration accepts the drag.
  struct Started : Event<void(const DragEvent&)> {};
  // Emitted for accepted movement; threshold acceptance may emit Changed immediately after Started.
  struct Changed : Event<void(const DragEvent&)> {};
  // Emitted on pointer Up after Started.
  struct Ended : Event<void(const DragEvent&)> {};
  // Emitted when an accepted drag loses ownership without a normal Up.
  struct Canceled : Event<void(const DragEvent&)> {};
};

// Describes one target-relative update during a typed drag-and-drop session.
struct DropEvent {
  // Pointer sequence that owns the drag-and-drop session.
  std::int64_t pointer_id = 0;
  // Device kind reported by the owning pointer sequence.
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  // Current position in the receiving DropTarget node's local logical coordinate space.
  Point position;
  // Current position in host-window logical coordinates.
  Point window_position;

  bool operator==(const DropEvent&) const = default;
};

// Describes normal completion of a drag source, including whether a target accepted the drop.
struct DragDropResult {
  DragEvent drag;
  bool dropped = false;

  bool operator==(const DragDropResult&) const = default;
};

// Typed lifecycle event keys emitted by DragSource.
struct DragSourceEvents {
  struct Started : Event<void(const DragEvent&)> {};
  struct Changed : Event<void(const DragEvent&)> {};
  struct Ended : Event<void(const DragDropResult&)> {};
  struct Canceled : Event<void(const DragEvent&)> {};
};

// Typed lifecycle event keys emitted by a compatible DropTarget.
template <class T>
  requires std::same_as<T, std::remove_cvref_t<T>>
struct DropEvents {
  struct Entered : Event<void(const T&, const DropEvent&)> {};
  struct Moved : Event<void(const T&, const DropEvent&)> {};
  struct Exited : Event<void(const T&, const DropEvent&)> {};
  struct Dropped : Event<void(const T&, const DropEvent&)> {};
};

namespace detail {

class DragSourceExtension;
class DropTargetExtension;

struct DropTargetDispatch {
  using Function = void (*)(const EventBindings&, const void*, const DropEvent&);

  Function entered = nullptr;
  Function moved = nullptr;
  Function exited = nullptr;
  Function dropped = nullptr;
};

} // namespace detail

// Starts a typed drag-and-drop session after its DragGesture wins pointer ownership.
class DragSource {
public:
  template <class T>
    requires std::same_as<T, std::remove_cvref_t<T>>
  explicit DragSource(T payload, DragGesture gesture = {})
      : DragSource(typeid(T), std::make_shared<const T>(std::move(payload)), {}, std::move(gesture)) {}

  template <class T>
    requires std::same_as<T, std::remove_cvref_t<T>>
  DragSource(T payload, std::function<View()> preview, DragGesture gesture = {})
      : DragSource(
            typeid(T), std::make_shared<const T>(std::move(payload)), std::move(preview), std::move(gesture)
        ) {}

  static const detail::ModifierDescriptor& Descriptor();

private:
  DragSource(std::type_index payload_type, std::shared_ptr<const void> payload, std::function<View()> preview,
             DragGesture gesture);

  std::type_index payload_type_ = typeid(void);
  std::shared_ptr<const void> payload_;
  std::function<View()> preview_;
  DragGesture gesture_;

  friend class detail::DragSourceExtension;
};

// Accepts exact typed payloads from DragSource without participating in pointer ownership recognition.
class DropTarget {
public:
  template <class T>
    requires std::same_as<T, std::remove_cvref_t<T>>
  static DropTarget Accepts() {
    return Accepts<T>([](const T&) { return true; });
  }

  template <class T, class Predicate>
    requires std::same_as<T, std::remove_cvref_t<T>> && std::copy_constructible<Predicate> &&
             std::predicate<Predicate&, const T&>
  static DropTarget Accepts(Predicate predicate) {
    const detail::DropTargetDispatch dispatch{
        [](const detail::EventBindings& bindings, const void* payload, const DropEvent& event) {
          detail::EmitEvent<typename DropEvents<T>::Entered>(bindings, *static_cast<const T*>(payload), event);
        },
        [](const detail::EventBindings& bindings, const void* payload, const DropEvent& event) {
          detail::EmitEvent<typename DropEvents<T>::Moved>(bindings, *static_cast<const T*>(payload), event);
        },
        [](const detail::EventBindings& bindings, const void* payload, const DropEvent& event) {
          detail::EmitEvent<typename DropEvents<T>::Exited>(bindings, *static_cast<const T*>(payload), event);
        },
        [](const detail::EventBindings& bindings, const void* payload, const DropEvent& event) {
          detail::EmitEvent<typename DropEvents<T>::Dropped>(bindings, *static_cast<const T*>(payload), event);
        },
    };
    return DropTarget{
        typeid(T),
        [predicate = std::move(predicate)](const void* payload) mutable {
          return std::invoke(predicate, *static_cast<const T*>(payload));
        },
        dispatch,
    };
  }

  static const detail::ModifierDescriptor& Descriptor();

private:
  DropTarget(std::type_index payload_type, std::function<bool(const void*)> accepts,
             detail::DropTargetDispatch dispatch);

  std::type_index payload_type_ = typeid(void);
  std::function<bool(const void*)> accepts_;
  detail::DropTargetDispatch dispatch_;

  friend class detail::DropTargetExtension;
};

// Recognizes the combined translation, scale, and rotation of two or more pointers.
struct TransformGesture {
  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const TransformGesture&) const = default;
};

// Describes one incremental update from a recognized TransformGesture.
struct TransformEvent {
  // Device kind shared by every pointer in the transform.
  PointerDeviceKind device_kind = PointerDeviceKind::Touch;
  // Number of physical pointers participating after this lifecycle transition.
  std::uint32_t pointer_count = 0;
  // Current centroid in the modifier owner's frozen node-local logical coordinate space.
  Point centroid;
  // Current centroid in host-window logical coordinates.
  Point window_centroid;
  // Node-local centroid displacement since the preceding update.
  Point pan;
  // Multiplicative spread change since the preceding update. One is the identity value.
  float scale = 1.0F;
  // Angular change since the preceding update, in radians. Positive values rotate clockwise.
  float rotation = 0.0F;

  bool operator==(const TransformEvent&) const = default;
};

// Typed lifecycle event keys emitted after a TransformGesture owns at least two pointers.
struct TransformEvents {
  // Emitted once when the second compatible pointer accepts the transform.
  struct Started : Event<void(const TransformEvent&)> {};
  // Emitted for geometry changes and identity rebases after the participating pointer set changes.
  struct Changed : Event<void(const TransformEvent&)> {};
  // Emitted when a normal pointer Up leaves fewer than two participating pointers.
  struct Ended : Event<void(const TransformEvent&)> {};
  // Emitted once when an accepted transform is canceled without normal completion.
  struct Canceled : Event<void(const TransformEvent&)> {};
};

} // namespace huxerui
