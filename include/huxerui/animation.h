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

enum class Easing {
  Linear,
  EaseIn,
  EaseOut,
  EaseInOut,
};

class CubicBezierCurve {
public:
  CubicBezierCurve(float x1, float y1, float x2, float y2);

  [[nodiscard]] float X1() const noexcept;
  [[nodiscard]] float Y1() const noexcept;
  [[nodiscard]] float X2() const noexcept;
  [[nodiscard]] float Y2() const noexcept;

  bool operator==(const CubicBezierCurve&) const = default;

private:
  float x1_;
  float y1_;
  float x2_;
  float y2_;
};

using TimingCurve = std::variant<Easing, CubicBezierCurve>;

struct SnapSpec {
  bool operator==(const SnapSpec&) const = default;
};

struct TweenSpec {
  double duration = 0.2;
  TimingCurve easing = Easing::EaseOut;

  bool operator==(const TweenSpec&) const = default;
};

struct SpringSpec {
  float stiffness = 320.0F;
  float damping_ratio = 0.82F;

  bool operator==(const SpringSpec&) const = default;
};

struct ProgressKeyframe {
  float fraction = 0.0F;
  float progress = 0.0F;
  TimingCurve curve_to_next = Easing::Linear;

  bool operator==(const ProgressKeyframe&) const = default;
};

class KeyframeSpec {
public:
  KeyframeSpec(double duration, std::vector<ProgressKeyframe> keyframes);

  [[nodiscard]] double Duration() const noexcept;
  [[nodiscard]] const std::vector<ProgressKeyframe>& Keyframes() const noexcept;

  bool operator==(const KeyframeSpec&) const = default;

private:
  double duration_;
  std::vector<ProgressKeyframe> keyframes_;
};

using AnimationSpec = std::variant<SnapSpec, TweenSpec, SpringSpec, KeyframeSpec>;

enum class RepeatMode {
  Restart,
  Reverse,
};

struct AnimationPlayback {
  double delay = 0.0;
  std::optional<std::uint32_t> iterations = 1;
  RepeatMode repeat_mode = RepeatMode::Restart;

  bool operator==(const AnimationPlayback&) const = default;
};

struct MotionAdvanceResult {
  bool changed = false;
  bool needs_frame = false;
  std::optional<double> wake_after;

  bool operator==(const MotionAdvanceResult&) const = default;
};

class MotionController {
public:
  MotionController() noexcept = default;
  explicit MotionController(float value);

  [[nodiscard]] float Value() const noexcept;
  [[nodiscard]] float Target() const noexcept;
  [[nodiscard]] float Velocity() const noexcept;
  [[nodiscard]] bool IsRunning() const noexcept;

  void Set(float value);
  void Seek(float value, float velocity = 0.0F);
  void AnimateTo(float target, AnimationSpec animation, AnimationPlayback playback = {});

  template <class Spec>
    requires std::constructible_from<AnimationSpec, Spec>
  void AnimateTo(float target, Spec&& animation, AnimationPlayback playback = {}) {
    AnimateTo(target, AnimationSpec(std::forward<Spec>(animation)), playback);
  }

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

struct TransformOrigin {
  float x = 0.5F;
  float y = 0.5F;

  bool operator==(const TransformOrigin&) const = default;
};

template <class T> struct Animated {
  T target;
  AnimationSpec animation;
  AnimationPlayback playback;

  bool operator==(const Animated&) const = default;
};

template <class T, class Spec>
  requires std::constructible_from<AnimationSpec, Spec>
Animated<T> AnimateTo(T target, Spec&& animation, AnimationPlayback playback = {}) {
  return {
      std::move(target),
      AnimationSpec(std::forward<Spec>(animation)),
      playback,
  };
}

struct Opacity {
  explicit Opacity(float value) : value(value) {}
  explicit Opacity(Animated<float> value) : value(std::move(value)) {}

  static const detail::ModifierDescriptor& Descriptor();

  std::variant<float, Animated<float>> value;

  bool operator==(const Opacity&) const = default;
};

struct Offset {
  explicit Offset(Point value) : value(value) {}
  explicit Offset(Animated<Point> value) : value(std::move(value)) {}

  static const detail::ModifierDescriptor& Descriptor();

  std::variant<Point, Animated<Point>> value;

  bool operator==(const Offset&) const = default;
};

struct Scale {
  explicit Scale(float value, TransformOrigin origin = {}) : value(value), origin(origin) {}

  explicit Scale(Animated<float> value, TransformOrigin origin = {}) : value(std::move(value)), origin(origin) {}

  static const detail::ModifierDescriptor& Descriptor();

  std::variant<float, Animated<float>> value;
  TransformOrigin origin;

  bool operator==(const Scale&) const = default;
};

struct Rotation {
  explicit Rotation(float degrees, TransformOrigin origin = {}) : degrees(degrees), origin(origin) {}

  explicit Rotation(Animated<float> degrees, TransformOrigin origin = {})
      : degrees(std::move(degrees)), origin(origin) {}

  static const detail::ModifierDescriptor& Descriptor();

  std::variant<float, Animated<float>> degrees;
  TransformOrigin origin;

  bool operator==(const Rotation&) const = default;
};

class Transition {
public:
  explicit Transition(float progress);
  explicit Transition(Animated<float> progress);

  Transition Opacity(float from, float to) &&;
  Transition Offset(Point from, Point to) &&;
  Transition Scale(float from, float to, TransformOrigin origin = {}) &&;
  Transition Rotation(float from_degrees, float to_degrees, TransformOrigin origin = {}) &&;

  static const detail::ModifierDescriptor& Descriptor();

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

struct FadeSceneTransition {
  AnimationSpec animation = TweenSpec{0.22, Easing::EaseInOut};
  double delay = 0.0;

  bool operator==(const FadeSceneTransition&) const = default;
};

struct CircularRevealSceneTransition {
  AnimationSpec animation = TweenSpec{0.36, Easing::EaseInOut};
  double delay = 0.0;

  bool operator==(const CircularRevealSceneTransition&) const = default;
};

class SceneTransitionAnchor {
public:
  static const detail::ModifierDescriptor& Descriptor();

private:
  explicit SceneTransitionAnchor(std::shared_ptr<detail::SceneTransitionAnchorState> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::SceneTransitionAnchorState> state_;

  friend class SceneTransitionHandle;
  friend class detail::SceneTransitionAnchorExtension;
};

class SceneTransitionHandle {
public:
  [[nodiscard]] SceneTransitionAnchor Anchor() const;
  void Run(FadeSceneTransition transition, std::function<void()> mutation) const;
  void Run(CircularRevealSceneTransition transition, std::function<void()> mutation) const;
  void RunAt(Point origin, CircularRevealSceneTransition transition, std::function<void()> mutation) const;

private:
  SceneTransitionHandle(
      std::shared_ptr<detail::SceneTransitionService> service,
      std::shared_ptr<detail::SceneTransitionAnchorState> anchor,
      bool reduced_motion
  )
      : service_(std::move(service)), anchor_(std::move(anchor)), reduced_motion_(reduced_motion) {}

  std::shared_ptr<detail::SceneTransitionService> service_;
  std::shared_ptr<detail::SceneTransitionAnchorState> anchor_;
  bool reduced_motion_ = false;

  friend SceneTransitionHandle UseSceneTransition();
};

SceneTransitionHandle UseSceneTransition();

} // namespace huxerui
