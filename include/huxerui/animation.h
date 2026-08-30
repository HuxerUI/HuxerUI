#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/modifier.h>

namespace huxerui {

namespace detail {
class SceneTransitionAnchorExtension;
class SceneTransitionService;
struct SceneTransitionAnchorState;
class TransitionExtension;
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
  [[nodiscard]] float X1() const noexcept;
  /// Returns the first control point's y coordinate.
  [[nodiscard]] float Y1() const noexcept;
  /// Returns the second control point's x coordinate.
  [[nodiscard]] float X2() const noexcept;
  /// Returns the second control point's y coordinate.
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
struct TweenSpec {
  /// Duration in seconds. It must be finite and non-negative.
  double duration = 0.2;
  /// Curve used to transform normalized elapsed time.
  TimingCurve easing = Easing::EaseOut;

  /// Compares duration and easing.
  bool operator==(const TweenSpec&) const = default;
};

/// Describes a damped spring evaluated independently of frame rate.
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
  [[nodiscard]] double Duration() const noexcept;
  /// Returns the validated keyframes in increasing fraction order.
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
/// @code
/// MotionController motion{0.0F};
/// motion.AnimateTo(1.0F, TweenSpec{0.24, Easing::EaseOut});
///
/// NodeExtension::FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
///   const MotionAdvanceResult result = motion.Advance(frame);
///   return {result.needs_frame, result.wake_after};
/// }
/// @endcode
class MotionController {
public:
  /// Constructs a stopped controller at zero.
  MotionController() noexcept = default;
  /// Constructs a stopped controller at a finite initial value.
  explicit MotionController(float value);

  /// Returns the current visible value.
  [[nodiscard]] float Value() const noexcept;
  /// Returns the most recently requested target, which may differ from Value() while running.
  [[nodiscard]] float Target() const noexcept;
  /// Returns the current value velocity in units per second.
  [[nodiscard]] float Velocity() const noexcept;
  /// Returns true while motion is pending, delayed, or actively advancing.
  [[nodiscard]] bool IsRunning() const noexcept;

  /// Resolves immediately to a finite value, clears velocity, and stops existing motion.
  void Set(float value);
  /// Resolves immediately to a finite value and establishes a finite velocity for gesture handoff or retargeting.
  void Seek(float value, float velocity = 0.0F);
  /// Retargets from the current value and velocity using a validated animation and playback description.
  ///
  /// Throws `std::invalid_argument` for non-finite targets or invalid animation and playback combinations. SnapSpec and
  /// SpringSpec support one restart iteration; TweenSpec and KeyframeSpec also support repeated playback.
  void AnimateTo(float target, AnimationSpec animation, AnimationPlayback playback = {});

  /// Retargets through a concrete spec convertible to AnimationSpec.
  template <class Spec>
    requires std::constructible_from<AnimationSpec, Spec>
  void AnimateTo(float target, Spec&& animation, AnimationPlayback playback = {}) {
    AnimateTo(target, AnimationSpec(std::forward<Spec>(animation)), playback);
  }

  /// Advances from one Runtime frame, honoring FrameInfo::reduced_motion, and reports required scheduling.
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
/// return content.With(
///     Opacity(AnimateTo(visible ? 1.0F : 0.0F, TweenSpec{0.2, Easing::EaseOut}))
/// );
/// @endcode
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
/// return content.With(Opacity(AnimateTo(enabled ? 1.0F : 0.5F, TweenSpec{})));
/// @endcode
struct Opacity {
  /// Constructs immediate opacity. Applied values are clamped to the unit interval.
  explicit Opacity(float value) : value(value) {}
  /// Constructs retained animated opacity.
  explicit Opacity(Animated<float> value) : value(std::move(value)) {}

  /// Returns the property-modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Immediate value or declarative animated target.
  std::variant<float, Animated<float>> value;

  /// Compares the complete opacity description.
  bool operator==(const Opacity&) const = default;
};

/// Applies immediate or retained animated translation in logical units without changing layout.
/// @code
/// return content.With(Offset(AnimateTo(expanded ? Point{} : Point{0.0F, 12.0F}, TweenSpec{})));
/// @endcode
struct Offset {
  /// Constructs an immediate translation.
  explicit Offset(Point value) : value(value) {}
  /// Constructs a retained animated translation.
  explicit Offset(Animated<Point> value) : value(std::move(value)) {}

  /// Returns the property-modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Immediate translation or declarative animated target.
  std::variant<Point, Animated<Point>> value;

  /// Compares the complete offset description.
  bool operator==(const Offset&) const = default;
};

/// Applies immediate or retained uniform scale around a transform origin without changing layout.
/// @code
/// return content.With(Scale(AnimateTo(pressed ? 0.96F : 1.0F, SpringSpec{})));
/// @endcode
struct Scale {
  /// Constructs immediate non-negative scale around an origin.
  explicit Scale(float value, TransformOrigin origin = {}) : value(value), origin(origin) {}

  /// Constructs retained animated non-negative scale around an origin.
  explicit Scale(Animated<float> value, TransformOrigin origin = {}) : value(std::move(value)), origin(origin) {}

  /// Returns the property-modifier descriptor used by View::With().
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
/// return content.With(Rotation(AnimateTo(expanded ? 180.0F : 0.0F, TweenSpec{})));
/// @endcode
struct Rotation {
  /// Constructs an immediate finite rotation around an origin.
  explicit Rotation(float degrees, TransformOrigin origin = {}) : degrees(degrees), origin(origin) {}

  /// Constructs a retained animated finite rotation around an origin.
  explicit Rotation(Animated<float> degrees, TransformOrigin origin = {})
      : degrees(std::move(degrees)), origin(origin) {}

  /// Returns the property-modifier descriptor used by View::With().
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
/// return content.With(
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
  Transition Opacity(float from, float to) &&;
  /// Adds a finite translation track in logical units.
  Transition Offset(Point from, Point to) &&;
  /// Adds a finite non-negative uniform scale track around an origin.
  Transition Scale(float from, float to, TransformOrigin origin = {}) &&;
  /// Adds a finite rotation track in degrees around an origin.
  Transition Rotation(float from_degrees, float to_degrees, TransformOrigin origin = {}) &&;

  /// Returns the retained-modifier descriptor used by View::With().
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

/// Cross-fades the previously committed scene and the scene produced by a synchronous mutation.
///
/// Fade transitions do not require a SceneTransitionAnchor.
struct FadeSceneTransition {
  /// Motion used to advance normalized transition progress.
  AnimationSpec animation = TweenSpec{0.22, Easing::EaseInOut};
  /// Non-negative delay in seconds before the transition begins.
  double delay = 0.0;

  /// Compares motion and delay.
  bool operator==(const FadeSceneTransition&) const = default;
};

/// Reveals the scene produced by a synchronous mutation through an expanding circular clip.
///
/// SceneTransitionHandle::Run() obtains the circle origin from its mounted anchor. RunAt() and
/// RunFromCurrentInteraction() provide the origin explicitly or from current event delivery.
struct CircularRevealSceneTransition {
  /// Motion used to advance normalized transition progress.
  AnimationSpec animation = TweenSpec{0.36, Easing::EaseInOut};
  /// Non-negative delay in seconds before the transition begins.
  double delay = 0.0;

  /// Compares motion and delay.
  bool operator==(const CircularRevealSceneTransition&) const = default;
};

/// Retained modifier that supplies stable presentation geometry to a circular scene transition.
///
/// One anchor returned by SceneTransitionHandle::Anchor() may be mounted on only one View at a time.
/// @code
/// return Button("Change theme")
///     .With(scene_transition.Anchor())
///     .OnClick([scene_transition, dark] {
///       scene_transition.Run(CircularRevealSceneTransition{}, [dark] { dark = !dark; });
///     });
/// @endcode
class SceneTransitionAnchor {
public:
  /// Returns the retained-modifier descriptor used by View::With().
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
///   scene_transition.Run(FadeSceneTransition{}, [page] { page += 1; });
/// });
/// @endcode
class SceneTransitionHandle {
public:
  /// Returns the modifier that records one mounted View's current presentation bounds.
  [[nodiscard]] SceneTransitionAnchor Anchor() const;

  /// Cross-fades to the scene produced by a non-empty synchronous mutation.
  void Run(FadeSceneTransition transition, std::function<void()> mutation) const;

  /// Reveals the mutated scene from the center of the mounted Anchor().
  ///
  /// Throws `std::logic_error` when no anchor is mounted.
  void Run(CircularRevealSceneTransition transition, std::function<void()> mutation) const;

  /// Reveals the mutated scene from a finite window-local logical point.
  ///
  /// RunAt() is appropriate when the caller already owns stable geometry or resumes asynchronous work after an input
  /// callback has returned.
  void RunAt(Point origin, CircularRevealSceneTransition transition, std::function<void()> mutation) const;

  /// Runs a circular reveal from the current synchronous pointer, keyboard, or semantic interaction.
  ///
  /// Pointer-driven semantic callbacks inherit the exact window-local pointer position. Keyboard and accessibility
  /// activation use the activated View's center. Calling this after the interaction callback returns throws
  /// `std::logic_error`; retain explicit geometry and use RunAt() for asynchronous work.
  /// @code
  /// Button("Next").OnClick([transition, page] {
  ///   transition.RunFromCurrentInteraction(CircularRevealSceneTransition{}, [page] { page += 1; });
  /// });
  /// @endcode
  void RunFromCurrentInteraction(CircularRevealSceneTransition transition, std::function<void()> mutation) const;

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
///   transition.Run(FadeSceneTransition{}, [enabled] { enabled = !enabled; });
/// });
/// @endcode
SceneTransitionHandle UseSceneTransition();

} // namespace huxerui
