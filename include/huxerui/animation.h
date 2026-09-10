#pragma once

/// @file
/// Animation timing, retained presentation modifiers, shared elements, and custom page and Scene transition effects.

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/modifier.h>
#include <huxerui/vector.h>

namespace huxerui {

class PaintContext;

namespace detail {
struct InternalAccess;
class SceneTransitionAnchorExtension;
class SceneTransitionService;
struct SceneTransitionAnchorState;
class TransitionExtension;
class SharedTransitionState;

struct SharedBoundsEvaluator {
  virtual ~SharedBoundsEvaluator() = default;
  virtual Rect Evaluate(Rect from, Rect to, float progress) const = 0;
  virtual bool Equals(const SharedBoundsEvaluator& other) const = 0;
};

template <class T> struct SharedBoundsEvaluatorValue final : SharedBoundsEvaluator {
  explicit SharedBoundsEvaluatorValue(T value) : value(std::move(value)) {}
  Rect Evaluate(Rect from, Rect to, float progress) const override { return value.Evaluate(from, to, progress); }
  bool Equals(const SharedBoundsEvaluator& other) const override {
    const auto* typed = dynamic_cast<const SharedBoundsEvaluatorValue*>(&other);
    return typed && value == typed->value;
  }
  T value;
};

struct SharedMarkerData {
  std::variant<std::int64_t, std::uint64_t, std::string> key;
  std::shared_ptr<const SharedBoundsEvaluator> bounds_transform;
  bool operator==(const SharedMarkerData& other) const {
    return key == other.key && (bounds_transform == other.bounds_transform ||
        (bounds_transform && other.bounds_transform && bounds_transform->Equals(*other.bounds_transform)));
  }
};

template <class T>
concept SharedBoundsEffect = std::copy_constructible<T> && std::equality_comparable<T> &&
    requires(const T& effect, Rect from, Rect to, float progress) {
      { effect.Evaluate(from, to, progress) } -> std::same_as<Rect>;
    };
} // namespace detail

/// Identifies a built-in timing curve that maps normalized time to normalized progress.
enum class Easing {
  /// Advances at a constant rate.
  Linear,
  /// Starts slowly and accelerates toward the target.
  EaseIn,
  /// Starts quickly and decelerates toward the target.
  EaseOut,
  /// Accelerates through the first half and decelerates through the second half.
  EaseInOut,
};

/// Describes a cubic Bezier timing curve with fixed endpoints at `(0, 0)` and `(1, 1)`.
///
/// The x coordinates must remain in the unit interval so time maps to one progress value. The y coordinates may
/// overshoot that interval.
/// @code
/// TweenSpec emphasized{.duration = 0.3, .easing = CubicBezierCurve{0.2F, 0.0F, 0.0F, 1.0F}};
/// @endcode
class CubicBezierCurve {
public:
  /// Constructs a curve from two finite control points. Throws `std::invalid_argument` when either x coordinate is
  /// outside the unit interval or any coordinate is not finite.
  CubicBezierCurve(float x1, float y1, float x2, float y2);

  /// Returns the first control point's x coordinate.
  /// @return The first control point's normalized time coordinate.
  [[nodiscard]] float X1() const noexcept;
  /// Returns the first control point's y coordinate.
  /// @return The first control point's progress coordinate, which may overshoot.
  [[nodiscard]] float Y1() const noexcept;
  /// Returns the second control point's x coordinate.
  /// @return The second control point's normalized time coordinate.
  [[nodiscard]] float X2() const noexcept;
  /// Returns the second control point's y coordinate.
  /// @return The second control point's progress coordinate, which may overshoot.
  [[nodiscard]] float Y2() const noexcept;

  /// Compares both control points.
  bool operator==(const CubicBezierCurve&) const = default;

private:
  float x1_;
  float y1_;
  float x2_;
  float y2_;
};

/// Selects either a named easing curve or an explicit cubic Bezier curve.
using TimingCurve = std::variant<Easing, CubicBezierCurve>;

/// Resolves an animation to its target immediately after its optional playback delay.
struct SnapSpec {
  /// Compares two snap descriptions.
  bool operator==(const SnapSpec&) const = default;
};

/// Describes duration-based interpolation through one timing curve.
/// @code
/// TweenSpec ease_out{.duration = 0.24, .easing = Easing::EaseOut};
/// @endcode
struct TweenSpec {
  /// Duration in seconds. It must be finite and non-negative.
  double duration = 0.2;
  /// Curve used to transform normalized elapsed time.
  TimingCurve easing = Easing::EaseOut;

  /// Compares duration and easing.
  bool operator==(const TweenSpec&) const = default;
};

/// Describes a damped spring evaluated independently of frame rate.
///
/// A zero damping ratio is valid for general motion but is rejected by one-shot TransitionSpec timing.
/// @code
/// SpringSpec spring{.stiffness = 240.0F, .damping_ratio = 0.8F};
/// @endcode
struct SpringSpec {
  /// Positive spring stiffness controlling oscillation frequency.
  float stiffness = 320.0F;
  /// Non-negative damping ratio. Values below, equal to, and above one are underdamped, critical, and overdamped.
  float damping_ratio = 0.82F;

  /// Compares stiffness and damping ratio.
  bool operator==(const SpringSpec&) const = default;
};

/// Describes one normalized keyframe within a KeyframeSpec.
struct ProgressKeyframe {
  /// Strictly increasing time fraction in the unit interval.
  float fraction = 0.0F;
  /// Normalized output progress in the unit interval.
  float progress = 0.0F;
  /// Curve from this keyframe to the following keyframe.
  TimingCurve curve_to_next = Easing::Linear;

  /// Compares every keyframe field.
  bool operator==(const ProgressKeyframe&) const = default;
};

/// Describes normalized progress through ordered keyframes over one duration.
///
/// At least two keyframes are required. Fractions must increase from zero to one, and progress must begin at zero and
/// end at one.
/// @code
/// KeyframeSpec bounce{
///     0.5,
///     {
///         {.fraction = 0.0F, .progress = 0.0F, .curve_to_next = Easing::EaseOut},
///         {.fraction = 0.7F, .progress = 0.9F, .curve_to_next = Easing::EaseInOut},
///         {.fraction = 1.0F, .progress = 1.0F},
///     },
/// };
/// @endcode
class KeyframeSpec {
public:
  /// Constructs a keyframe animation. Throws `std::invalid_argument` for an invalid duration, endpoint, fraction,
  /// progress value, ordering, or timing curve.
  KeyframeSpec(double duration, std::vector<ProgressKeyframe> keyframes);

  /// Returns the positive duration in seconds.
  /// @return The positive duration in seconds.
  [[nodiscard]] double Duration() const noexcept;
  /// Returns the validated keyframes in increasing fraction order.
  /// @return A reference valid for the lifetime of this unmodified description.
  [[nodiscard]] const std::vector<ProgressKeyframe>& Keyframes() const noexcept;

  /// Compares duration and keyframes.
  bool operator==(const KeyframeSpec&) const = default;

private:
  double duration_;
  std::vector<ProgressKeyframe> keyframes_;
};

/// Selects immediate, duration-based, physical spring, or keyframed motion.
using AnimationSpec = std::variant<SnapSpec, TweenSpec, SpringSpec, KeyframeSpec>;

/// Identifies how another iteration begins after duration-based motion completes.
enum class RepeatMode {
  /// Starts every iteration from the original value and moves toward the target.
  Restart,
  /// Alternates forward and backward iterations.
  Reverse,
};

/// Configures delay and repetition independently from an AnimationSpec.
/// @code
/// AnimationPlayback pulse{
///     .delay = 0.1,
///     .iterations = std::nullopt,
///     .repeat_mode = RepeatMode::Reverse,
/// };
/// @endcode
struct AnimationPlayback {
  /// Non-negative delay in seconds before motion begins.
  double delay = 0.0;
  /// Positive iteration count, or an empty value for unbounded repetition.
  std::optional<std::uint32_t> iterations = 1;
  /// Repetition direction for TweenSpec and KeyframeSpec.
  RepeatMode repeat_mode = RepeatMode::Restart;

  /// Compares every playback field.
  bool operator==(const AnimationPlayback&) const = default;
};

/// Reports the observable and scheduling effects of one MotionController::Advance() call.
struct MotionAdvanceResult {
  /// True when the controller's visible value changed.
  bool changed = false;
  /// True when continuous animation requires the next frame.
  bool needs_frame = false;
  /// Delay in seconds until deferred work begins, when no continuous frame is needed yet.
  std::optional<double> wake_after;

  /// Compares every result field.
  bool operator==(const MotionAdvanceResult&) const = default;
};

/// Retains and advances one scalar animation using Runtime frame timing.
///
/// MotionController is useful inside NodeExtension implementations and other retained framework behavior. Application
/// state remains authoritative; declarative Views normally use AnimateTo() with a presentation modifier instead.
/// The frame below is supplied by NodeExtension::OnFrame(). Retain the controller across frames, call InvalidatePaint()
/// when result.changed is true, and return result.needs_frame and result.wake_after to propagate scheduling.
/// @code
/// MotionController motion{0.0F};
/// motion.AnimateTo(1.0F, TweenSpec{0.24, Easing::EaseOut});
/// const MotionAdvanceResult result = motion.Advance(frame);
/// @endcode
class MotionController {
public:
  /// Constructs a stopped controller at zero.
  MotionController() noexcept = default;
  /// Constructs a stopped controller at a finite initial value.
  explicit MotionController(float value);

  /// Returns the current visible value.
  /// @return The current scalar value, including finite spring overshoot.
  [[nodiscard]] float Value() const noexcept;
  /// Returns the most recently requested target, which may differ from Value() while running.
  /// @return The scalar destination of the most recent accepted animation.
  [[nodiscard]] float Target() const noexcept;
  /// Returns the current value velocity in units per second.
  /// @return Current scalar velocity in value units per second.
  [[nodiscard]] float Velocity() const noexcept;
  /// Returns true while motion is pending, delayed, or actively advancing.
  /// @return True for pending, delayed, or advancing motion; false after completion or Set().
  [[nodiscard]] bool IsRunning() const noexcept;

  /// Resolves immediately to a finite value, clears velocity, and stops existing motion.
  /// @param value Finite scalar value to publish immediately.
  /// @throws std::invalid_argument If value is not finite.
  void Set(float value);
  /// Resolves immediately to a finite value and establishes a finite velocity for gesture handoff or retargeting.
  /// @param value Finite scalar value established by an external driver, such as a gesture.
  /// @param velocity Finite initial velocity in value units per second; zero stops existing momentum.
  /// @throws std::invalid_argument If value or velocity is not finite.
  /// @code
  /// motion.Seek(0.4F, 0.8F);
  /// motion.AnimateTo(1.0F, SpringSpec{});
  /// @endcode
  void Seek(float value, float velocity = 0.0F);
  /// Retargets from the current value and velocity using a validated animation and playback description.
  ///
  /// Throws `std::invalid_argument` for non-finite targets or invalid animation and playback combinations. SnapSpec and
  /// SpringSpec support one restart iteration; TweenSpec and KeyframeSpec also support repeated playback.
  /// @param target Finite scalar destination.
  /// @param animation Timing and interpolation policy; it does not own a clock.
  /// @param playback Valid delay and iteration policy supported by animation.
  /// @throws std::invalid_argument If target, timing, or playback is invalid.
  void AnimateTo(float target, AnimationSpec animation, AnimationPlayback playback = {});

  /// Retargets through a concrete spec convertible to AnimationSpec.
  /// @tparam Spec Timing description constructible as AnimationSpec.
  /// @param target Finite scalar destination.
  /// @param animation Concrete timing description forwarded into the controller.
  /// @param playback Delay and iteration policy supported by animation.
  /// @throws std::invalid_argument If target, timing, or playback is invalid.
  template <class Spec>
    requires std::constructible_from<AnimationSpec, Spec>
  void AnimateTo(float target, Spec&& animation, AnimationPlayback playback = {}) {
    AnimateTo(target, AnimationSpec(std::forward<Spec>(animation)), playback);
  }

  /// Advances from one Runtime frame, honoring FrameInfo::reduced_motion, and reports required scheduling.
  /// @param frame Runtime timestamp and reduced-motion policy for this advance.
  /// @return Value-change and scheduling information to propagate from NodeExtension::OnFrame().
  MotionAdvanceResult Advance(const FrameInfo& frame) noexcept;

private:
  void Resolve(float value) noexcept;
  void Finish(float value) noexcept;

  AnimationSpec animation_ = SnapSpec{};
  AnimationPlayback playback_;
  float value_ = 0.0F;
  float start_ = 0.0F;
  float target_ = 0.0F;
  float velocity_ = 0.0F;
  float start_velocity_ = 0.0F;
  double start_time_ = 0.0;
  bool pending_ = false;
  bool running_ = false;
};

/// Identifies a transform pivot as fractions of the affected View's width and height.
///
/// `{0.5F, 0.5F}` is the center, `{0.0F, 0.0F}` is the top-left corner, and finite values outside the unit interval
/// place the pivot outside the View.
struct TransformOrigin {
  /// Horizontal fraction of the View width.
  float x = 0.5F;
  /// Vertical fraction of the View height.
  float y = 0.5F;

  /// Compares both origin fractions.
  bool operator==(const TransformOrigin&) const = default;
};

/// Carries a declarative target, motion description, and playback policy for a retained presentation modifier.
/// @tparam T Target value type supported by the consuming modifier.
/// @see AnimateTo
template <class T> struct Animated {
  /// Authoritative declarative target.
  T target;
  /// Motion used when the target changes.
  AnimationSpec animation;
  /// Delay and repetition applied to the motion.
  AnimationPlayback playback;

  /// Compares the target, motion, and playback policy.
  bool operator==(const Animated&) const = default;
};

/// Creates a declarative animated target for Opacity, Offset, Scale, Rotation, or Transition.
/// @code
/// return std::move(content).With(
///     Opacity(AnimateTo(visible ? 1.0F : 0.0F, TweenSpec{0.2, Easing::EaseOut}))
/// );
/// @endcode
/// @tparam T Value type supported by the consuming presentation modifier.
/// @tparam Spec Timing description constructible as AnimationSpec.
/// @param target Authoritative value to reach when the declaration changes.
/// @param animation Timing policy forwarded into the value description.
/// @param playback Delay and iteration policy interpreted by the consuming modifier.
/// @return An immutable description; creating it does not start a separate animation clock.
template <class T, class Spec>
  requires std::constructible_from<AnimationSpec, Spec>
Animated<T> AnimateTo(T target, Spec&& animation, AnimationPlayback playback = {}) {
  return {
      std::move(target),
      AnimationSpec(std::forward<Spec>(animation)),
      playback,
  };
}

/// Applies immediate or retained animated opacity without changing layout.
/// @code
/// return std::move(content).With(Opacity(AnimateTo(enabled ? 1.0F : 0.5F, TweenSpec{})));
/// @endcode
struct Opacity {
  /// Constructs immediate opacity. Applied values are clamped to the unit interval.
  explicit Opacity(float value) : value(value) {}
  /// Constructs retained animated opacity.
  explicit Opacity(Animated<float> value) : value(std::move(value)) {}

  /// Returns the retained-modifier descriptor used by View::With().
  /// @return The framework modifier descriptor; attach the value through With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Immediate value or declarative animated target.
  std::variant<float, Animated<float>> value;

  /// Compares the complete opacity description.
  bool operator==(const Opacity&) const = default;
};

/// Applies immediate or retained animated translation in logical units without changing layout.
/// @code
/// return std::move(content).With(Offset(AnimateTo(expanded ? Point{} : Point{0.0F, 12.0F}, TweenSpec{})));
/// @endcode
struct Offset {
  /// Constructs an immediate translation.
  explicit Offset(Point value) : value(value) {}
  /// Constructs a retained animated translation.
  explicit Offset(Animated<Point> value) : value(std::move(value)) {}

  /// Returns the retained-modifier descriptor used by View::With().
  /// @return The framework modifier descriptor; attach the value through With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Immediate translation or declarative animated target.
  std::variant<Point, Animated<Point>> value;

  /// Compares the complete offset description.
  bool operator==(const Offset&) const = default;
};

/// Applies immediate or retained uniform scale around a transform origin without changing layout.
/// @code
/// return std::move(content).With(Scale(AnimateTo(pressed ? 0.96F : 1.0F, SpringSpec{})));
/// @endcode
struct Scale {
  /// Constructs immediate non-negative scale around an origin.
  explicit Scale(float value, TransformOrigin origin = {}) : value(value), origin(origin) {}

  /// Constructs retained animated non-negative scale around an origin.
  explicit Scale(Animated<float> value, TransformOrigin origin = {}) : value(std::move(value)), origin(origin) {}

  /// Returns the retained-modifier descriptor used by View::With().
  /// @return The framework modifier descriptor; attach the value through With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Immediate scale or declarative animated target.
  std::variant<float, Animated<float>> value;
  /// Pivot used by the scale transform.
  TransformOrigin origin;

  /// Compares scale and origin.
  bool operator==(const Scale&) const = default;
};

/// Applies immediate or retained rotation in degrees around a transform origin without changing layout.
/// @code
/// return std::move(content).With(Rotation(AnimateTo(expanded ? 180.0F : 0.0F, TweenSpec{})));
/// @endcode
struct Rotation {
  /// Constructs an immediate finite rotation around an origin.
  explicit Rotation(float degrees, TransformOrigin origin = {}) : degrees(degrees), origin(origin) {}

  /// Constructs a retained animated finite rotation around an origin.
  explicit Rotation(Animated<float> degrees, TransformOrigin origin = {})
      : degrees(std::move(degrees)), origin(origin) {}

  /// Returns the retained-modifier descriptor used by View::With().
  /// @return The framework modifier descriptor; attach the value through With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Immediate degrees or declarative animated target.
  std::variant<float, Animated<float>> degrees;
  /// Pivot used by the rotation transform.
  TransformOrigin origin;

  /// Compares rotation and origin.
  bool operator==(const Rotation&) const = default;
};

/// Projects one immediate or animated unit-interval progress value onto synchronized presentation properties.
///
/// A Transition is an rvalue-qualified fluent modifier so all configured tracks share exactly one retained progress
/// controller and surrounding modifier position.
/// @code
/// return std::move(content).With(
///     Transition{AnimateTo(selected ? 1.0F : 0.0F, TweenSpec{0.2, Easing::EaseOut})}
///         .Opacity(0.6F, 1.0F)
///         .Offset({-8.0F, 0.0F}, {})
///         .Scale(0.96F, 1.0F)
/// );
/// @endcode
class Transition {
public:
  /// Constructs a transition at immediate progress in the unit interval.
  explicit Transition(float progress);
  /// Constructs a transition from animated progress whose target is in the unit interval.
  explicit Transition(Animated<float> progress);

  /// Adds an opacity track whose endpoints are in the unit interval.
  /// @param from Finite opacity at progress zero, in the unit interval.
  /// @param to Finite opacity at progress one, in the unit interval.
  /// @return The configured modifier; replaces an existing opacity track.
  /// @throws std::invalid_argument If either endpoint is invalid.
  Transition Opacity(float from, float to) &&;
  /// Adds a finite translation track in logical units.
  /// @param from Finite logical translation at progress zero.
  /// @param to Finite logical translation at progress one.
  /// @return The configured modifier; replaces an existing offset track.
  /// @throws std::invalid_argument If either endpoint is not finite.
  Transition Offset(Point from, Point to) &&;
  /// Adds a finite non-negative uniform scale track around an origin.
  /// @param from Finite non-negative uniform scale at progress zero.
  /// @param to Finite non-negative uniform scale at progress one.
  /// @param origin Finite pivot fractions relative to the affected View's bounds.
  /// @return The configured modifier; replaces an existing scale track.
  /// @throws std::invalid_argument If scale endpoints or origin are invalid.
  Transition Scale(float from, float to, TransformOrigin origin = {}) &&;
  /// Adds a finite rotation track in degrees around an origin.
  /// @param from_degrees Finite angle in degrees at progress zero.
  /// @param to_degrees Finite angle in degrees at progress one.
  /// @param origin Finite pivot fractions relative to the affected View's bounds.
  /// @return The configured modifier; replaces an existing rotation track.
  /// @throws std::invalid_argument If angles or origin are not finite.
  Transition Rotation(float from_degrees, float to_degrees, TransformOrigin origin = {}) &&;

  /// Returns the retained-modifier descriptor used by View::With().
  /// @return The framework modifier descriptor; attach the value through With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Compares progress and every configured projection track.
  bool operator==(const Transition&) const = default;

private:
  struct ScalarTrack {
    float from;
    float to;

    bool operator==(const ScalarTrack&) const = default;
  };

  struct PointTrack {
    Point from;
    Point to;

    bool operator==(const PointTrack&) const = default;
  };

  std::variant<float, Animated<float>> progress_;
  std::optional<ScalarTrack> opacity_;
  std::optional<PointTrack> offset_;
  std::optional<ScalarTrack> scale_;
  std::optional<ScalarTrack> rotation_;
  TransformOrigin scale_origin_;
  TransformOrigin rotation_origin_;

  friend class detail::TransitionExtension;
};

/// Inputs to a pure effect evaluation in container-local logical coordinates.
struct TransitionContext {
  /// Finite normalized progress; spring-driven execution may overshoot the unit interval.
  float progress = 0.0F;
  /// Finite container-local bounds with non-negative dimensions, independent of page layout changes.
  Rect bounds;
  /// Optional finite origin in the same coordinates as bounds; page transitions do not supply an implicit origin.
  std::optional<Point> origin;
};

/// One independently transformed region of a transition side, without duplicating its mounted content.
/// @code
/// TransitionFragment half{
///     .source_clip = ClipShape::Rectangle({0.0F, 0.0F, 160.0F, 240.0F}),
///     .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, -40.0F, 0.0F},
/// };
/// @endcode
struct TransitionFragment {
  /// Region in the original container-local coordinates, clipped before either fragment or side transforms.
  /// An empty shape hides this fragment; clipping does not relocate its coordinate origin.
  ClipShape source_clip;
  /// Finite affine transform applied after source clipping and before the side's common transform.
  Transform2D transform;
  /// Finite opacity factor, clamped to the unit interval and multiplied by the side's opacity.
  float opacity = 1.0F;

  bool operator==(const TransitionFragment&) const = default;
};

/// The sampled visual result for one side of a transition, with clipping applied after the transform.
struct TransitionSample {
  /// Finite affine transform applied to this side's content in container-local logical coordinates.
  Transform2D transform;
  /// Finite group opacity, clamped to the unit interval by TransitionSpec::Evaluate().
  float opacity = 1.0F;
  /// Optional clipping after the transform; absence adds no restriction and an empty shape hides the group.
  std::optional<ClipShape> clip;
  /// Independently drawn source regions, in back-to-front order. Empty draws the original content once.
  /// A non-empty list replaces the original drawing; regions may overlap or leave gaps.
  /// All fragments share this sample's transform, opacity factor, and final clip.
  std::vector<TransitionFragment> fragments;
  bool operator==(const TransitionSample&) const = default;
};

/// Selects which transition side paints above the other, independently from navigation history.
enum class TransitionOrder {
  /// Paints the departing content after the arriving content.
  OutgoingAbove,
  /// Paints the arriving content after the departing content.
  IncomingAbove
};

/// Combines the sampled output of both transition sides and their relative drawing order.
///
/// A custom effect returns this value from a const Evaluate() method. Sampling does not change layout or retain nodes.
/// @code
/// TransitionFrame frame;
/// frame.incoming.opacity = 0.5F;
/// frame.order = TransitionOrder::OutgoingAbove;
/// @endcode
struct TransitionFrame {
  /// Sample applied to the departing page or frozen scene.
  TransitionSample outgoing;
  /// Sample applied to the arriving page or live scene.
  TransitionSample incoming;
  /// Relative drawing order of the two sides.
  TransitionOrder order = TransitionOrder::IncomingAbove;
  bool operator==(const TransitionFrame&) const = default;
};

/// Crossfades outgoing and incoming opacity, clamping progress to the unit interval.
/// @code
/// const TransitionSpec fade{FadeTransition{}, TweenSpec{0.2}};
/// @endcode
struct FadeTransition {
  /// Samples this effect without advancing time or retaining execution state.
  /// @param context Container geometry and finite progress to sample.
  /// @return Both side samples with incoming content drawn above outgoing content.
  [[nodiscard]] TransitionFrame Evaluate(const TransitionContext& context) const;
  bool operator==(const FadeTransition&) const = default;
};

/// Slides both sides using offsets expressed as fractions of the container's width and height.
///
/// Opacity remains one, and finite progress overshoot is preserved.
/// @code
/// const TransitionSpec slide{
///     SlideTransition{.incoming_offset = {0.0F, 1.0F}, .outgoing_offset = {0.0F, -0.2F}}, TweenSpec{0.3}
/// };
/// @endcode
struct SlideTransition {
  /// Finite displacement of incoming content at progress zero, in container-size fractions.
  Point incoming_offset{1.0F, 0.0F};
  /// Finite displacement of outgoing content at progress one, in container-size fractions.
  Point outgoing_offset{-1.0F, 0.0F};
  /// Samples this effect without advancing time or retaining execution state.
  /// @param context Container geometry and finite progress to sample.
  /// @return Both side samples with incoming content drawn above outgoing content.
  /// @throws std::invalid_argument If the effect's configured geometry is invalid.
  [[nodiscard]] TransitionFrame Evaluate(const TransitionContext& context) const;
  bool operator==(const SlideTransition&) const = default;
};

/// Combines a crossfade with uniform scaling around a container-relative pivot.
///
/// Scale interpolation preserves finite overshoot, while opacity is clamped to the unit interval.
/// @code
/// const TransitionSpec zoom{ScaleFadeTransition{.incoming_scale = 0.9F}, TweenSpec{0.24}};
/// @endcode
struct ScaleFadeTransition {
  /// Finite non-negative incoming scale at progress zero; it reaches one at progress one.
  float incoming_scale = 0.92F;
  /// Finite non-negative outgoing scale at progress one; it begins at one at progress zero.
  float outgoing_scale = 1.0F;
  /// Finite pivot fractions relative to TransitionContext::bounds, centered by default.
  TransformOrigin origin;
  /// Samples this effect without advancing time or retaining execution state.
  /// @param context Container geometry and finite progress to sample.
  /// @return Both side samples with incoming content drawn above outgoing content.
  /// @throws std::invalid_argument If the effect's configured geometry is invalid.
  [[nodiscard]] TransitionFrame Evaluate(const TransitionContext& context) const;
  bool operator==(const ScaleFadeTransition&) const = default;
};

/// Reveals incoming content through an expanding circular clip above the outgoing content.
///
/// An explicit context origin or mounted Scene anchor is required. The circle expands to the farthest bounds corner;
/// progress is clamped to the unit interval. Page effects must provide their own origin data when using this effect.
/// @code
/// transition.RunAt({24.0F, 24.0F}, TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36}}, mutation);
/// @endcode
struct CircularRevealTransition {
  /// Samples this effect without advancing time or retaining execution state.
  /// @param context Container geometry and finite progress to sample.
  /// @return Both side samples with incoming content drawn above outgoing content.
  /// @throws std::logic_error If context.origin is absent.
  /// @throws std::invalid_argument If the origin or computed clipping geometry is invalid.
  [[nodiscard]] TransitionFrame Evaluate(const TransitionContext& context) const;
  bool operator==(const CircularRevealTransition&) const = default;
};

/// Describes a copyable one-shot effect with timing and an optional start delay.
///
/// Custom effects must be copyable and equality-comparable and provide a const Evaluate(const TransitionContext&)
/// returning TransitionFrame. An optional const Paint(PaintContext&, const TransitionContext&) method records decoration
/// above both sides at that same sample. Both methods must be pure apart from recording: do not mutate application state,
/// call composition hooks, or start Scene transitions. Descriptions share immutable storage and do not own progress.
/// Timing is validated on construction. Repetition is not supported and transition springs require positive damping.
/// A default description completes immediately; reduced-motion handling belongs to the executor.
/// @code
/// struct LiftEffect {
///   float distance = 24.0F;
///   TransitionFrame Evaluate(const TransitionContext& context) const {
///     auto frame = FadeTransition{}.Evaluate(context);
///     frame.incoming.transform.translate_y = distance * (1.0F - context.progress);
///     return frame;
///   }
///   bool operator==(const LiftEffect&) const = default;
/// };
/// const TransitionSpec enter{LiftEffect{}, TweenSpec{0.28}, 0.05};
/// const TransitionSpec leave = enter.Reversed(TweenSpec{0.2});
/// @endcode
class TransitionSpec {
public:
  TransitionSpec() = default;

  template <class Effect>
    requires (!std::same_as<std::remove_cvref_t<Effect>, TransitionSpec>) &&
             std::copy_constructible<std::remove_cvref_t<Effect>> &&
             std::equality_comparable<std::remove_cvref_t<Effect>> &&
             requires(const std::remove_cvref_t<Effect>& effect, const TransitionContext& context) {
               { effect.Evaluate(context) } -> std::same_as<TransitionFrame>;
             }
  TransitionSpec(Effect effect, AnimationSpec animation, double delay = 0.0)
      : effect_(std::make_shared<EffectValue<std::remove_cvref_t<Effect>>>(std::move(effect))),
        animation_(std::move(animation)), delay_(delay) {
    ValidateTiming();
  }

  /// Borrows the immutable timing description.
  /// @return Timing valid while this TransitionSpec remains alive and unmodified.
  [[nodiscard]] const AnimationSpec& Animation() const noexcept { return animation_; }
  /// Reports the configured start delay.
  /// @return Finite non-negative delay in seconds.
  [[nodiscard]] double Delay() const noexcept { return delay_; }
  /// Returns whether the timing completes immediately without a start delay.
  /// @return True for SnapSpec or a zero-duration TweenSpec with zero delay; independent of reduced-motion policy.
  [[nodiscard]] bool IsImmediate() const noexcept;
  /// Evaluates and validates both sides atomically. Custom effects must be pure.
  /// @param context Finite progress, finite non-negative bounds, and optional finite local origin.
  /// @return A fully validated frame with clamped opacity; no partial output is published on failure.
  /// @throws std::invalid_argument If context or sampled output is invalid.
  /// @throws std::logic_error If the selected effect requires an unavailable origin.
  /// @note Exceptions raised by a custom effect propagate to the caller.
  [[nodiscard]] TransitionFrame Evaluate(const TransitionContext& context) const;
  /// Records optional effect decorations above both sides using the same input as Evaluate().
  /// Effects may define void Paint(PaintContext&, const TransitionContext&) const; absence records nothing.
  /// Reversed descriptions supply one minus progress. Recording must not mutate application state or advance time.
  /// The caller owns the context and finishes it; the effect must balance its own clips and transforms.
  /// @param paint Borrowed recording context in the transition container's coordinates; do not retain or finish it.
  /// @param context The exact progress, bounds, and origin used to sample the accompanying frame.
  /// @throws std::invalid_argument If context is invalid. Exceptions raised by the effect propagate to the caller.
  void Paint(PaintContext& paint, const TransitionContext& context) const;
  /// Exchanges sides, including their fragments, and drawing order while evaluating at one minus progress.
  /// @return A reversed description preserving timing and delay; reversing twice restores the original description.
  [[nodiscard]] TransitionSpec Reversed() const;
  /// Reverses the effect while replacing its timing and preserving the configured delay.
  /// @param animation Valid one-shot timing, with positive damping for springs.
  /// @return A reversed description using animation and the original delay.
  /// @throws std::invalid_argument If animation is invalid.
  [[nodiscard]] TransitionSpec Reversed(AnimationSpec animation) const;
  bool operator==(const TransitionSpec& other) const;

private:
  struct EffectBase {
    virtual ~EffectBase() = default;
    virtual TransitionFrame Evaluate(const TransitionContext& context) const = 0;
    virtual void Paint(PaintContext& paint, const TransitionContext& context) const = 0;
    virtual bool HasPaint() const noexcept = 0;
    virtual bool Equals(const EffectBase& other) const = 0;
  };
  template <class Effect> struct EffectValue final : EffectBase {
    explicit EffectValue(Effect value) : value(std::move(value)) {}
    TransitionFrame Evaluate(const TransitionContext& context) const override { return value.Evaluate(context); }
    void Paint(PaintContext& paint, const TransitionContext& context) const override {
      if constexpr (requires { { value.Paint(paint, context) } -> std::same_as<void>; }) {
        value.Paint(paint, context);
      }
    }
    bool HasPaint() const noexcept override {
      return requires(PaintContext& paint, const TransitionContext& context) {
        { value.Paint(paint, context) } -> std::same_as<void>;
      };
    }
    bool Equals(const EffectBase& other) const override {
      const auto* typed = dynamic_cast<const EffectValue*>(&other);
      return typed != nullptr && value == typed->value;
    }
    Effect value;
  };
  void ValidateTiming() const;
  std::shared_ptr<const EffectBase> effect_;
  AnimationSpec animation_ = SnapSpec{};
  double delay_ = 0.0;
  bool reversed_ = false;
  friend struct detail::InternalAccess;
};

/// Marks corresponding visual content for navigation or an explicit local shared transition.
/// Keys are independent of View::Key and must be unique on each participating side.
/// The retained visual is mapped between bounds without intermediate layout; use SharedBounds for different content.
/// Unsupported native/texture content, partial ancestor clipping, and rotation/skew mappings skip the affected pair.
/// @code
/// Image(product.image).With(SharedElement(product.id));
/// @endcode
class SharedElement {
public:
  explicit SharedElement(std::string key) : data_{std::move(key), {}} {}
  explicit SharedElement(std::string_view key) : SharedElement(std::string(key)) {}
  explicit SharedElement(const char* key);
  template <std::integral T> explicit SharedElement(T key) {
    if constexpr (std::is_signed_v<T>) { data_.key = static_cast<std::int64_t>(key); }
    else { data_.key = static_cast<std::uint64_t>(key); }
  }
  template <class T> requires std::is_enum_v<T>
  explicit SharedElement(T key) : SharedElement(static_cast<std::underlying_type_t<T>>(key)) {}

  /// Selects a pure bounds calculation; its zero and one samples must preserve both endpoints.
  /// @param effect Copyable, equality-comparable value with Rect Evaluate(Rect, Rect, float) const.
  /// @return The configured marker. Timing remains owned by the surrounding operation.
  template <detail::SharedBoundsEffect T> SharedElement BoundsTransform(T effect) && {
    data_.bounds_transform = std::make_shared<detail::SharedBoundsEvaluatorValue<T>>(std::move(effect));
    return std::move(*this);
  }
  /// Returns the modifier descriptor used by View::With().
  /// @return The framework descriptor for a shared-element marker.
  static const detail::ModifierDescriptor& Descriptor();
  bool operator==(const SharedElement&) const = default;
private:
  detail::SharedMarkerData data_;
  friend struct detail::InternalAccess;
};

/// Marks corresponding regions whose different retained contents crossfade while their bounds move together.
/// A matched ancestor and descendant cannot both participate. Mark an enclosing region or its separate children.
/// @code
/// Column {Text(title), Text(description)}.With(SharedBounds("product"));
/// @endcode
class SharedBounds {
public:
  explicit SharedBounds(std::string key) : data_{std::move(key), {}} {}
  explicit SharedBounds(std::string_view key) : SharedBounds(std::string(key)) {}
  explicit SharedBounds(const char* key);
  template <std::integral T> explicit SharedBounds(T key) {
    if constexpr (std::is_signed_v<T>) { data_.key = static_cast<std::int64_t>(key); }
    else { data_.key = static_cast<std::uint64_t>(key); }
  }
  template <class T> requires std::is_enum_v<T>
  explicit SharedBounds(T key) : SharedBounds(static_cast<std::underlying_type_t<T>>(key)) {}

  /// Selects a pure bounds calculation shared by both retained contents.
  /// @param effect Copyable, equality-comparable value with Rect Evaluate(Rect, Rect, float) const.
  /// @return The configured marker; invalid sampled geometry is rejected before publication.
  template <detail::SharedBoundsEffect T> SharedBounds BoundsTransform(T effect) && {
    data_.bounds_transform = std::make_shared<detail::SharedBoundsEvaluatorValue<T>>(std::move(effect));
    return std::move(*this);
  }
  /// Returns the modifier descriptor used by View::With().
  /// @return The framework descriptor for a shared-bounds marker.
  static const detail::ModifierDescriptor& Descriptor();
  bool operator==(const SharedBounds&) const = default;
private:
  detail::SharedMarkerData data_;
  friend struct detail::InternalAccess;
};

/// Attaches one local shared-transition operation owner to a stable container.
/// Obtain this modifier through SharedTransitionHandle::Scope(); mount it on at most one View at a time.
/// Mark descendants inside the container. Nested local scopes are transparent to an outer active operation.
class SharedTransitionScope {
public:
  /// Returns the modifier descriptor used by View::With().
  /// @return The retained descriptor that binds the local operation owner.
  static const detail::ModifierDescriptor& Descriptor();
  bool operator==(const SharedTransitionScope&) const = default;
private:
  explicit SharedTransitionScope(std::shared_ptr<detail::SharedTransitionState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::SharedTransitionState> state_;
  friend class SharedTransitionHandle;
  friend struct detail::InternalAccess;
};

/// Runs shared-element motion over a synchronous state change inside one attached local scope.
/// Only matched visuals animate; normal layout and unmatched content adopt the new state immediately.
/// Scope content cannot interact during motion. Controls outside the scope remain available.
class SharedTransitionHandle {
public:
  /// Returns the modifier that attaches this handle to its participating container.
  /// @return A scope modifier; its mounted identity must survive Run's mutation.
  [[nodiscard]] SharedTransitionScope Scope() const;
  /// Captures committed shared visuals, executes mutation once, and matches the next prepared layout.
  /// @param animation One-shot timing; repetition and non-convergent springs are unsupported.
  /// @param mutation Non-empty synchronous state update; already-performed writes are not rolled back on failure.
  /// @throws std::invalid_argument If timing, mutation, keys, or sampled bounds are invalid.
  /// @throws std::logic_error If the scope is disconnected, the thread is wrong, or the request re-enters evaluation.
  /// @note A new request starts from committed shared visuals; velocity continuity is not guaranteed.
  void Run(AnimationSpec animation, std::function<void()> mutation) const;
private:
  explicit SharedTransitionHandle(std::shared_ptr<detail::SharedTransitionState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::SharedTransitionState> state_;
  friend SharedTransitionHandle UseSharedTransition();
};

/// Retains a local shared-transition handle in the calling composition scope.
/// @return A stable handle whose Scope() modifier must be attached before calling Run().
/// @throws std::logic_error If called outside composition.
/// @code
/// auto transition = UseSharedTransition();
/// return Stack {Content()}.With(transition.Scope());
/// @endcode
SharedTransitionHandle UseSharedTransition();

/// Retained modifier that supplies stable presentation geometry to a circular scene transition.
///
/// One anchor returned by SceneTransitionHandle::Anchor() may be mounted on only one View at a time.
/// @code
/// return Button("Change theme")
///     .With(scene_transition.Anchor())
///     .OnClick([scene_transition, dark] {
///       scene_transition.Run(TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36}}, [dark] { dark = !dark; });
///     });
/// @endcode
class SceneTransitionAnchor {
public:
  /// Returns the retained-modifier descriptor used by View::With().
  /// @return The framework modifier descriptor; attach the value through With().
  static const detail::ModifierDescriptor& Descriptor();

private:
  explicit SceneTransitionAnchor(std::shared_ptr<detail::SceneTransitionAnchorState> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::SceneTransitionAnchorState> state_;

  friend class SceneTransitionHandle;
  friend class detail::SceneTransitionAnchorExtension;
};

/// Starts window scene transitions and owns the optional geometry anchor associated with one composable call site.
///
/// The mutation must synchronously update authoritative application state so Runtime can capture the old scene and
/// compose its replacement. When reduced motion is enabled, the mutation still runs but the visual transition is
/// skipped.
/// @code
/// auto scene_transition = UseSceneTransition();
/// return Button("Next").OnClick([scene_transition, page] {
///   scene_transition.Run(TransitionSpec{FadeTransition{}, TweenSpec{0.22}}, [page] { page += 1; });
/// });
/// @endcode
class SceneTransitionHandle {
public:
  /// Returns the modifier that records one mounted View's current presentation bounds.
  /// @return A retained modifier associated with this handle's stable anchor state.
  /// @note Mount the returned anchor on at most one View at a time.
  [[nodiscard]] SceneTransitionAnchor Anchor() const;

  /// Runs a one-shot effect over a synchronous mutation, using Anchor() geometry when available.
  /// Required-origin validation happens before mutation; an effect requiring a missing origin throws.
  /// @param transition One-shot visual effect and timing for this operation.
  /// @param mutation Non-empty synchronous application-state update, executed once after preflight validation.
  /// @throws std::invalid_argument If mutation is empty or effect input/output is invalid.
  /// @throws std::logic_error If disconnected, required origin is missing, or the call re-enters mutation/evaluation.
  /// @note Mutation exceptions propagate without rolling back state writes; active visual retention is released.
  void Run(TransitionSpec transition, std::function<void()> mutation) const;

  /// Runs the effect with a finite window-local logical origin.
  ///
  /// RunAt() is appropriate when the caller already owns stable geometry or resumes asynchronous work after an input
  /// callback has returned.
  /// @param origin Finite window-local logical point; it overrides the handle's anchor geometry.
  /// @param transition One-shot visual effect and timing.
  /// @param mutation Non-empty synchronous application-state update.
  /// @throws std::invalid_argument If origin, mutation, or evaluated geometry is invalid.
  /// @throws std::logic_error If disconnected or called recursively from mutation/evaluation.
  /// @see Run
  void RunAt(Point origin, TransitionSpec transition, std::function<void()> mutation) const;

  /// Runs an effect from the current synchronous pointer, keyboard, or semantic interaction.
  ///
  /// Pointer-driven semantic callbacks inherit the exact window-local pointer position. Keyboard and accessibility
  /// activation use the activated View's center. Calling this after the interaction callback returns throws
  /// `std::logic_error`; retain explicit geometry and use RunAt() for asynchronous work.
  /// @code
  /// Button("Next").OnClick([transition, page] {
  ///   transition.RunFromCurrentInteraction(
  ///       TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36}}, [page] { page += 1; }
  ///   );
  /// });
  /// @endcode
  /// @param transition One-shot visual effect and timing.
  /// @param mutation Non-empty synchronous application-state update.
  /// @throws std::logic_error If no synchronous origin exists, the service is disconnected, or the call re-enters.
  /// @throws std::invalid_argument If mutation or evaluated geometry is invalid.
  /// @see Run
  void RunFromCurrentInteraction(TransitionSpec transition, std::function<void()> mutation) const;

private:
  SceneTransitionHandle(std::shared_ptr<detail::SceneTransitionService> service,
                        std::shared_ptr<detail::SceneTransitionAnchorState> anchor, bool reduced_motion)
      : service_(std::move(service)), anchor_(std::move(anchor)), reduced_motion_(reduced_motion) {}

  std::shared_ptr<detail::SceneTransitionService> service_;
  std::shared_ptr<detail::SceneTransitionAnchorState> anchor_;
  bool reduced_motion_ = false;

  friend SceneTransitionHandle UseSceneTransition();
};

/// Returns the scene-transition handle retained by the current composition scope.
///
/// Call this composition-bound hook from a reusable composable function. The returned handle may be captured by
/// synchronous event callbacks and remains stable across compatible recomposition.
/// @code
/// auto transition = UseSceneTransition();
/// return Button("Toggle").OnClick([transition, enabled] {
///   transition.Run(TransitionSpec{FadeTransition{}, TweenSpec{0.22}}, [enabled] { enabled = !enabled; });
/// });
/// @endcode
/// @return The current window's transition handle with a stable per-call-site anchor.
/// @throws std::logic_error If invoked outside an active composition context.
SceneTransitionHandle UseSceneTransition();

} // namespace huxerui
