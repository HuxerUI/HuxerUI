#pragma once

#include <cstddef>
#include <memory>
#include <source_location>

#include <huxerui/geometry.h>
#include <huxerui/state.h>

namespace huxerui {

namespace detail {
struct ModifierDescriptor;
class ScrollConnection;
class ScrollControllerState;
} // namespace detail

/// Defines how an item is positioned when a virtual layout scrolls to it.
enum class ScrollAlignment {
  /// Aligns the item's leading edge with the viewport's leading edge.
  Start,
  /// Centers the item in the viewport.
  Center,
  /// Aligns the item's trailing edge with the viewport's trailing edge.
  End,
};

/// Identifies the operation associated with mounted scroll activity.
enum class ScrollSource {
  /// Direct pointer dragging of scrollable content.
  Drag,
  /// A platform-recognized wheel or trackpad update.
  Wheel,
  /// Retained motion after direct input ends.
  Momentum,
  /// Retained overscroll settlement after direct input ends.
  Overscroll,
  /// Direct manipulation of a visible scrollbar.
  Scrollbar,
  /// A ScrollController request.
  Programmatic,
  /// A platform accessibility scroll action.
  Accessibility,
  /// Automatic focus or text-input reveal.
  FocusReveal,
  /// Drag-and-drop edge auto-scrolling.
  DragDrop,
};

/// Identifies the lifecycle phase of transient scroll activity.
enum class ScrollPhase {
  /// Begins an activity with an explicit lifecycle.
  Begin,
  /// Reports an actual offset or overscroll displacement change.
  Update,
  /// Completes the activity normally.
  End,
  /// Cancels the activity because ownership or mounted availability changed.
  Cancel,
};

/// Reports the authoritative content offset and current scroll geometry.
struct ScrollMetrics {
  /// Axis owned by the connected scroll container.
  Axis axis = Axis::Vertical;
  /// Current clamped content offset in logical pixels.
  float offset = 0.0F;
  /// Largest valid content offset in logical pixels.
  float maximum_offset = 0.0F;
  /// Visible extent along the scrolling axis in logical pixels.
  float viewport_extent = 0.0F;
  /// Complete content extent along the scrolling axis in logical pixels.
  float content_extent = 0.0F;

  bool operator==(const ScrollMetrics&) const = default;
};

/// Describes one transient mounted scroll activity notification.
///
/// Application code observes `ScrollController::Metrics()` instead. This value lets retained extensions coordinate
/// presentation such as scrollbar visibility without creating another scroll state.
struct ScrollActivity {
  /// Operation that produced the activity.
  ScrollSource source = ScrollSource::Programmatic;
  /// Lifecycle phase of the activity.
  ScrollPhase phase = ScrollPhase::Update;
  /// Axis changed by the activity.
  Axis axis = Axis::Vertical;
  /// Actual signed content-offset or logical overscroll change in logical pixels.
  float delta = 0.0F;
  /// Metrics after applying the change.
  ScrollMetrics metrics;

  bool operator==(const ScrollActivity&) const = default;
};

/// Configures momentum and overscroll for one scroll container.
///
/// An explicit modifier replaces the platform adapter's default physics for that container.
/// @code
/// ScrollView(content).With(ScrollPhysics{
///     .deceleration_rate = 4.0F,
///     .overscroll_enabled = true,
/// });
/// @endcode
struct ScrollPhysics {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Whether release velocity may continue scrolling after direct input ends.
  bool fling_enabled = true;
  /// Exponential velocity decay rate per second; larger values stop sooner.
  float deceleration_rate = 3.0F;
  /// Smallest release velocity that starts a fling, in logical pixels per second.
  float minimum_fling_velocity = 40.0F;
  /// Largest release velocity accepted by the fling simulation, in logical pixels per second.
  float maximum_fling_velocity = 6000.0F;
  /// Whether direct touch dragging may retain terminal overscroll displacement.
  bool overscroll_enabled = true;
  /// Fraction of unconsumed direct drag applied near a resting boundary.
  float overscroll_resistance = 0.45F;
  /// Largest retained overscroll displacement in logical pixels.
  float maximum_overscroll = 96.0F;
  /// Exponential return rate per second after direct input ends.
  float overscroll_settle_rate = 18.0F;

  bool operator==(const ScrollPhysics&) const = default;
};

/// Controls and observes one mounted scroll container.
///
/// Keep a controller stable with `UseScrollController()` when a component recomposes.
class ScrollController {
public:
  explicit ScrollController(float initial_offset = 0.0F);

  /// Returns the latest observable offset and geometry projection.
  [[nodiscard]] ScrollMetrics Metrics() const;
  /// Returns the current clamped content offset.
  [[nodiscard]] float Offset() const;
  /// Returns the largest valid content offset.
  [[nodiscard]] float MaxOffset() const;
  /// Returns the current viewport extent along the scrolling axis.
  [[nodiscard]] float ViewportExtent() const;
  /// Returns the complete content extent along the scrolling axis.
  [[nodiscard]] float ContentExtent() const;
  /// Returns whether this controller is connected to a mounted scroll container.
  [[nodiscard]] bool IsConnected() const noexcept;

  /// Requests an absolute content offset and returns whether a current connection accepted the request.
  bool ScrollTo(float offset) const;
  /// Requests a relative content offset and returns whether a current connection accepted the request.
  bool ScrollBy(float delta) const;
  /// Requests item alignment from a connected virtual layout.
  bool ScrollToItem(std::size_t index, ScrollAlignment alignment = ScrollAlignment::Start) const;

  bool operator==(const ScrollController&) const = default;

private:
  std::shared_ptr<detail::ScrollControllerState> state_;

  friend class detail::ScrollConnection;
};

/// Remembers one ScrollController in the current composition scope.
/// @code
/// auto controller = UseScrollController();
/// return VirtualList(items, BuildItem).With(controller);
/// @endcode
inline ScrollController UseScrollController(float initial_offset = 0.0F,
                                            const std::source_location& location = std::source_location::current()) {
  return UseState(ScrollController{initial_offset}, location).Get();
}

namespace detail {

struct ScrollControllerBinding {
  using Value = ScrollController;
};

} // namespace detail

} // namespace huxerui
