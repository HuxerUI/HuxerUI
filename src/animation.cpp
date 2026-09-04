#include <huxerui/animation.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <huxerui/theme.h>

#include "mounted_node_internal.h"

namespace huxerui {

namespace {

double EvaluateCubicBezier(const CubicBezierCurve& curve, double progress) {
  const auto coordinate = [](double t, double first, double second) {
    const double inverse = 1.0 - t;
    return 3.0 * inverse * inverse * t * first + 3.0 * inverse * t * t * second + t * t * t;
  };
  const auto derivative = [](double t, double first, double second) {
    const double inverse = 1.0 - t;
    return 3.0 * inverse * inverse * first + 6.0 * inverse * t * (second - first) + 3.0 * t * t * (1.0 - second);
  };

  double parameter = progress;
  for (int iteration = 0; iteration < 8; ++iteration) {
    const double error = coordinate(parameter, curve.X1(), curve.X2()) - progress;
    const double slope = derivative(parameter, curve.X1(), curve.X2());
    if (std::abs(error) < 1.0e-7 || std::abs(slope) < 1.0e-7) {
      break;
    }
    parameter = std::clamp(parameter - error / slope, 0.0, 1.0);
  }

  double low = 0.0;
  double high = 1.0;
  for (int iteration = 0; iteration < 12; ++iteration) {
    const double x = coordinate(parameter, curve.X1(), curve.X2());
    if (std::abs(x - progress) < 1.0e-7) {
      break;
    }
    if (x < progress) {
      low = parameter;
    } else {
      high = parameter;
    }
    parameter = (low + high) * 0.5;
  }
  return coordinate(parameter, curve.Y1(), curve.Y2());
}

double EvaluateTimingCurve(const TimingCurve& curve, double progress) {
  progress = std::clamp(progress, 0.0, 1.0);
  if (const auto* easing = std::get_if<Easing>(&curve)) {
    if (*easing == Easing::EaseIn) {
      return progress * progress * progress;
    }
    if (*easing == Easing::EaseOut) {
      const double inverse = 1.0 - progress;
      return 1.0 - inverse * inverse * inverse;
    }
    if (*easing == Easing::EaseInOut) {
      return progress < 0.5 ? 4.0 * progress * progress * progress : 1.0 - std::pow(-2.0 * progress + 2.0, 3.0) * 0.5;
    }
    return progress;
  }
  return EvaluateCubicBezier(std::get<CubicBezierCurve>(curve), progress);
}

void ValidateTimingCurve(const TimingCurve& curve) {
  if (const auto* easing = std::get_if<Easing>(&curve)) {
    if (*easing != Easing::Linear && *easing != Easing::EaseIn && *easing != Easing::EaseOut &&
        *easing != Easing::EaseInOut) {
      throw std::invalid_argument("HuxerUI easing value is invalid");
    }
  }
}

double AnimationDuration(const AnimationSpec& animation) {
  if (const auto* tween = std::get_if<TweenSpec>(&animation)) {
    return tween->duration;
  }
  if (const auto* keyframes = std::get_if<KeyframeSpec>(&animation)) {
    return keyframes->Duration();
  }
  return 0.0;
}

double EvaluateAnimationProgress(const AnimationSpec& animation, double progress) {
  if (const auto* tween = std::get_if<TweenSpec>(&animation)) {
    return EvaluateTimingCurve(tween->easing, progress);
  }
  const auto& keyframes = std::get<KeyframeSpec>(animation).Keyframes();
  const auto upper = std::upper_bound(
      keyframes.begin(),
      keyframes.end(),
      static_cast<float>(progress),
      [](float value, const ProgressKeyframe& keyframe) { return value < keyframe.fraction; }
  );
  if (upper == keyframes.begin()) {
    return keyframes.front().progress;
  }
  if (upper == keyframes.end()) {
    return keyframes.back().progress;
  }
  const ProgressKeyframe& next = *upper;
  const ProgressKeyframe& previous = *(upper - 1);
  const double segment_progress =
      (progress - previous.fraction) / static_cast<double>(next.fraction - previous.fraction);
  const double eased = EvaluateTimingCurve(previous.curve_to_next, segment_progress);
  return previous.progress + (next.progress - previous.progress) * eased;
}

void ValidateAnimation(const AnimationSpec& animation, const AnimationPlayback& playback) {
  if (!std::isfinite(playback.delay) || playback.delay < 0.0) {
    throw std::invalid_argument("HuxerUI animation delay must be finite and non-negative");
  }
  if (playback.iterations.has_value() && *playback.iterations == 0) {
    throw std::invalid_argument("HuxerUI animation iterations must be positive or unbounded");
  }
  if (playback.repeat_mode != RepeatMode::Restart && playback.repeat_mode != RepeatMode::Reverse) {
    throw std::invalid_argument("HuxerUI animation repeat mode is invalid");
  }
  if (const auto* tween = std::get_if<TweenSpec>(&animation)) {
    if (!std::isfinite(tween->duration) || tween->duration < 0.0) {
      throw std::invalid_argument("HuxerUI tween duration must be finite and non-negative");
    }
    if (tween->duration == 0.0 &&
        (playback.iterations != std::optional<std::uint32_t>{1} || playback.repeat_mode != RepeatMode::Restart)) {
      throw std::invalid_argument("HuxerUI zero-duration tween does not support repeated playback");
    }
    ValidateTimingCurve(tween->easing);
  } else if (const auto* spring = std::get_if<SpringSpec>(&animation)) {
    if (!std::isfinite(spring->stiffness) || spring->stiffness <= 0.0F || !std::isfinite(spring->damping_ratio) ||
        spring->damping_ratio < 0.0F) {
      throw std::invalid_argument("HuxerUI spring stiffness must be positive and damping ratio must be non-negative");
    }
    if (playback.iterations != std::optional<std::uint32_t>{1} || playback.repeat_mode != RepeatMode::Restart) {
      throw std::invalid_argument("HuxerUI spring animation does not support repeated playback");
    }
  } else if (
      std::holds_alternative<SnapSpec>(animation) &&
      (playback.iterations != std::optional<std::uint32_t>{1} || playback.repeat_mode != RepeatMode::Restart)
  ) {
    throw std::invalid_argument("HuxerUI snap animation does not support repeated playback");
  }
}

std::pair<float, float>
EvaluateSpring(const SpringSpec& spring, float start, float target, float start_velocity, double elapsed) {
  const double angular_frequency = std::sqrt(static_cast<double>(spring.stiffness));
  const double damping_ratio = spring.damping_ratio;
  const double displacement = static_cast<double>(start - target);
  const double velocity = start_velocity;
  double resolved_displacement = displacement;
  double resolved_velocity = velocity;

  if (damping_ratio < 1.0 - 1.0e-5) {
    const double damped_frequency = angular_frequency * std::sqrt(1.0 - damping_ratio * damping_ratio);
    const double a = displacement;
    const double b = (velocity + damping_ratio * angular_frequency * displacement) / damped_frequency;
    const double exponential = std::exp(-damping_ratio * angular_frequency * elapsed);
    const double cosine = std::cos(damped_frequency * elapsed);
    const double sine = std::sin(damped_frequency * elapsed);
    resolved_displacement = exponential * (a * cosine + b * sine);
    resolved_velocity = exponential * (-damping_ratio * angular_frequency * (a * cosine + b * sine) -
                                       a * damped_frequency * sine + b * damped_frequency * cosine);
  } else if (damping_ratio <= 1.0 + 1.0e-5) {
    const double a = displacement;
    const double b = velocity + angular_frequency * displacement;
    const double exponential = std::exp(-angular_frequency * elapsed);
    resolved_displacement = (a + b * elapsed) * exponential;
    resolved_velocity = (b - angular_frequency * (a + b * elapsed)) * exponential;
  } else {
    const double root = std::sqrt(damping_ratio * damping_ratio - 1.0);
    const double first_rate = -angular_frequency * (damping_ratio - root);
    const double second_rate = -angular_frequency * (damping_ratio + root);
    const double first = (velocity - second_rate * displacement) / (first_rate - second_rate);
    const double second = displacement - first;
    const double first_exponential = std::exp(first_rate * elapsed);
    const double second_exponential = std::exp(second_rate * elapsed);
    resolved_displacement = first * first_exponential + second * second_exponential;
    resolved_velocity = first_rate * first * first_exponential + second_rate * second * second_exponential;
  }

  return {
      target + static_cast<float>(resolved_displacement),
      static_cast<float>(resolved_velocity),
  };
}

void ValidateOrigin(TransformOrigin origin) {
  if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) {
    throw std::invalid_argument("HuxerUI transform origin must be finite");
  }
}

Point ResolveOrigin(const MountedNode& node, TransformOrigin origin) {
  const Rect frame = node.Bounds();
  return {
      frame.x + frame.width * origin.x,
      frame.y + frame.height * origin.y,
  };
}

class OpacityExtension final : public NodeExtension {
public:
  OpacityExtension(MountedNode& node, const Opacity& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const Opacity& modifier) {
    static_cast<void>(node);
    if (const auto* immediate = std::get_if<float>(&modifier.value)) {
      value_.Set(std::clamp(*immediate, 0.0F, 1.0F));
      initialized_ = true;
      return;
    }
    const auto& animated = std::get<Animated<float>>(modifier.value);
    const float target = std::clamp(animated.target, 0.0F, 1.0F);
    if (!initialized_) {
      value_.Set(target);
      initialized_ = true;
    } else {
      value_.AnimateTo(target, animated.animation, animated.playback);
    }
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const MotionAdvanceResult result = value_.Advance(frame);
    mounted.presentation.local_opacity *= value_.Value();
    return {result.needs_frame, result.wake_after};
  }

private:
  MotionController value_;
  bool initialized_ = false;
};

class OffsetExtension final : public NodeExtension {
public:
  OffsetExtension(MountedNode& node, const Offset& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const Offset& modifier) {
    static_cast<void>(node);
    if (const auto* immediate = std::get_if<Point>(&modifier.value)) {
      x_.Set(immediate->x);
      y_.Set(immediate->y);
      initialized_ = true;
      return;
    }
    const auto& animated = std::get<Animated<Point>>(modifier.value);
    if (!initialized_) {
      x_.Set(animated.target.x);
      y_.Set(animated.target.y);
      initialized_ = true;
    } else {
      x_.AnimateTo(animated.target.x, animated.animation, animated.playback);
      y_.AnimateTo(animated.target.y, animated.animation, animated.playback);
    }
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const MotionAdvanceResult x_result = x_.Advance(frame);
    const MotionAdvanceResult y_result = y_.Advance(frame);
    mounted.presentation.local_transform = detail::ComposeTransform(
        detail::TranslationTransform({x_.Value(), y_.Value()}),
        mounted.presentation.local_transform
    );
    return {
        x_result.needs_frame || y_result.needs_frame,
        detail::EarliestWakeAfter(x_result.wake_after, y_result.wake_after),
    };
  }

private:
  MotionController x_;
  MotionController y_;
  bool initialized_ = false;
};

class ScaleExtension final : public NodeExtension {
public:
  ScaleExtension(MountedNode& node, const Scale& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const Scale& modifier) {
    static_cast<void>(node);
    ValidateOrigin(modifier.origin);
    origin_ = modifier.origin;
    if (const auto* immediate = std::get_if<float>(&modifier.value)) {
      ValidateScale(*immediate);
      value_.Set(*immediate);
      initialized_ = true;
      return;
    }
    const auto& animated = std::get<Animated<float>>(modifier.value);
    ValidateScale(animated.target);
    if (!initialized_) {
      value_.Set(animated.target);
      initialized_ = true;
    } else {
      value_.AnimateTo(animated.target, animated.animation, animated.playback);
    }
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const MotionAdvanceResult result = value_.Advance(frame);
    const Point origin = ResolveOrigin(node, origin_);
    const float value = value_.Value();
    const Transform2D scale{
        value,
        0.0F,
        0.0F,
        value,
    };
    mounted.presentation.local_transform =
        detail::ComposeTransform(detail::AroundOriginTransform(scale, origin), mounted.presentation.local_transform);
    return {result.needs_frame, result.wake_after};
  }

private:
  static void ValidateScale(float value) {
    if (!std::isfinite(value) || value < 0.0F) {
      throw std::invalid_argument("HuxerUI scale must be finite and non-negative");
    }
  }

  MotionController value_;
  TransformOrigin origin_;
  bool initialized_ = false;
};

class RotationExtension final : public NodeExtension {
public:
  RotationExtension(MountedNode& node, const Rotation& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const Rotation& modifier) {
    static_cast<void>(node);
    ValidateOrigin(modifier.origin);
    origin_ = modifier.origin;
    if (const auto* immediate = std::get_if<float>(&modifier.degrees)) {
      ValidateDegrees(*immediate);
      degrees_.Set(*immediate);
      initialized_ = true;
      return;
    }
    const auto& animated = std::get<Animated<float>>(modifier.degrees);
    ValidateDegrees(animated.target);
    if (!initialized_) {
      degrees_.Set(animated.target);
      initialized_ = true;
    } else {
      degrees_.AnimateTo(animated.target, animated.animation, animated.playback);
    }
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const MotionAdvanceResult result = degrees_.Advance(frame);
    constexpr float degrees_to_radians = 3.14159265358979323846F / 180.0F;
    const float radians = degrees_.Value() * degrees_to_radians;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const Transform2D rotation{
        cosine,
        sine,
        -sine,
        cosine,
    };
    mounted.presentation.local_transform = detail::ComposeTransform(
        detail::AroundOriginTransform(rotation, ResolveOrigin(node, origin_)),
        mounted.presentation.local_transform
    );
    return {result.needs_frame, result.wake_after};
  }

private:
  static void ValidateDegrees(float value) {
    if (!std::isfinite(value)) {
      throw std::invalid_argument("HuxerUI rotation must be finite");
    }
  }

  MotionController degrees_;
  TransformOrigin origin_;
  bool initialized_ = false;
};

} // namespace

CubicBezierCurve::CubicBezierCurve(float x1, float y1, float x2, float y2) : x1_(x1), y1_(y1), x2_(x2), y2_(y2) {
  if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2) || x1 < 0.0F || x1 > 1.0F ||
      x2 < 0.0F || x2 > 1.0F) {
    throw std::invalid_argument("HuxerUI cubic Bezier control points must be finite with x in the unit interval");
  }
}

float CubicBezierCurve::X1() const noexcept {
  return x1_;
}

float CubicBezierCurve::Y1() const noexcept {
  return y1_;
}

float CubicBezierCurve::X2() const noexcept {
  return x2_;
}

float CubicBezierCurve::Y2() const noexcept {
  return y2_;
}

KeyframeSpec::KeyframeSpec(double duration, std::vector<ProgressKeyframe> keyframes)
    : duration_(duration), keyframes_(std::move(keyframes)) {
  if (!std::isfinite(duration) || duration <= 0.0) {
    throw std::invalid_argument("HuxerUI keyframe duration must be finite and positive");
  }
  if (keyframes_.size() < 2 || keyframes_.front().fraction != 0.0F || keyframes_.front().progress != 0.0F ||
      keyframes_.back().fraction != 1.0F || keyframes_.back().progress != 1.0F) {
    throw std::invalid_argument("HuxerUI keyframes must begin at {0, 0} and end at {1, 1}");
  }
  for (std::size_t index = 0; index < keyframes_.size(); ++index) {
    const ProgressKeyframe& keyframe = keyframes_[index];
    if (!std::isfinite(keyframe.fraction) || !std::isfinite(keyframe.progress) || keyframe.fraction < 0.0F ||
        keyframe.fraction > 1.0F || keyframe.progress < 0.0F || keyframe.progress > 1.0F ||
        (index > 0 && keyframe.fraction <= keyframes_[index - 1].fraction)) {
      throw std::invalid_argument(
          "HuxerUI keyframe fractions must increase and progress must stay in the unit interval"
      );
    }
    ValidateTimingCurve(keyframe.curve_to_next);
  }
}

double KeyframeSpec::Duration() const noexcept {
  return duration_;
}

const std::vector<ProgressKeyframe>& KeyframeSpec::Keyframes() const noexcept {
  return keyframes_;
}

MotionController::MotionController(float value) {
  Set(value);
}

float MotionController::Value() const noexcept {
  return value_;
}

float MotionController::Target() const noexcept {
  return target_;
}

float MotionController::Velocity() const noexcept {
  return velocity_;
}

bool MotionController::IsRunning() const noexcept {
  return running_ || pending_;
}

void MotionController::Set(float value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("HuxerUI animation value must be finite");
  }
  Resolve(value);
}

void MotionController::Resolve(float value) noexcept {
  value_ = value;
  start_ = value;
  target_ = value;
  velocity_ = 0.0F;
  start_velocity_ = 0.0F;
  pending_ = false;
  running_ = false;
}

void MotionController::Finish(float value) noexcept {
  value_ = value;
  start_ = value;
  velocity_ = 0.0F;
  start_velocity_ = 0.0F;
  pending_ = false;
  running_ = false;
}

void MotionController::Seek(float value, float velocity) {
  if (!std::isfinite(velocity)) {
    throw std::invalid_argument("HuxerUI animation velocity must be finite");
  }
  Set(value);
  velocity_ = velocity;
  start_velocity_ = velocity;
}

void MotionController::AnimateTo(float target, AnimationSpec animation, AnimationPlayback playback) {
  if (!std::isfinite(target)) {
    throw std::invalid_argument("HuxerUI animation target must be finite");
  }
  ValidateAnimation(animation, playback);
  const bool target_changed = target != target_;
  const bool animation_changed = animation != animation_ || playback != playback_;
  if (!target_changed && (!animation_changed || (!pending_ && !running_))) {
    return;
  }
  animation_ = std::move(animation);
  playback_ = playback;
  target_ = target;
  pending_ = true;
}

MotionAdvanceResult MotionController::Advance(const FrameInfo& frame) noexcept {
  const float previous_value = value_;
  if (pending_) {
    pending_ = false;
    if (frame.reduced_motion) {
      Finish(target_);
      return {value_ != previous_value, false, std::nullopt};
    }
    start_ = value_;
    start_velocity_ = velocity_;
    start_time_ = frame.timestamp + playback_.delay;
    running_ = true;
  }
  if (!running_) {
    return {};
  }
  if (frame.reduced_motion) {
    Finish(target_);
    return {value_ != previous_value, false, std::nullopt};
  }
  if (frame.timestamp < start_time_) {
    return {false, false, start_time_ - frame.timestamp};
  }
  if (std::holds_alternative<SnapSpec>(animation_)) {
    Finish(target_);
    return {value_ != previous_value, false, std::nullopt};
  }

  const double elapsed = std::max(0.0, frame.timestamp - start_time_);
  if (const auto* spring = std::get_if<SpringSpec>(&animation_)) {
    const auto [resolved_value, resolved_velocity] = EvaluateSpring(*spring, start_, target_, start_velocity_, elapsed);
    value_ = resolved_value;
    velocity_ = resolved_velocity;
    if (std::abs(target_ - value_) < 0.001F && std::abs(velocity_) < 0.001F) {
      Finish(target_);
    }
    return {value_ != previous_value, running_, std::nullopt};
  }

  const double duration = AnimationDuration(animation_);
  if (duration == 0.0) {
    Finish(target_);
    return {value_ != previous_value, false, std::nullopt};
  }
  const double raw_iteration = elapsed / duration;
  const bool finite = playback_.iterations.has_value();
  const double total_iterations =
      finite ? static_cast<double>(*playback_.iterations) : std::numeric_limits<double>::infinity();
  const bool complete = raw_iteration >= total_iterations;
  const double iteration = complete ? total_iterations - 1.0 : std::floor(raw_iteration);
  double progress = complete ? 1.0 : raw_iteration - iteration;
  const bool reverse_iteration = playback_.repeat_mode == RepeatMode::Reverse && std::fmod(iteration, 2.0) >= 1.0;
  if (reverse_iteration) {
    progress = 1.0 - progress;
  }
  const double animated_progress = EvaluateAnimationProgress(animation_, progress);
  value_ = start_ + (target_ - start_) * static_cast<float>(animated_progress);
  if (frame.delta_time > 0.0) {
    velocity_ = (value_ - previous_value) / static_cast<float>(frame.delta_time);
  }
  if (complete) {
    const bool finishes_at_target = playback_.repeat_mode == RepeatMode::Restart || !reverse_iteration;
    Finish(finishes_at_target ? target_ : start_);
  }
  return {value_ != previous_value, running_, std::nullopt};
}

Transition::Transition(float progress) : progress_(progress) {
  if (!std::isfinite(progress) || progress < 0.0F || progress > 1.0F) {
    throw std::invalid_argument("HuxerUI transition progress must be in the unit interval");
  }
}

Transition::Transition(Animated<float> progress) : progress_(std::move(progress)) {
  const float target = std::get<Animated<float>>(progress_).target;
  if (!std::isfinite(target) || target < 0.0F || target > 1.0F) {
    throw std::invalid_argument("HuxerUI transition progress must be in the unit interval");
  }
}

Transition Transition::Opacity(float from, float to) && {
  if (!std::isfinite(from) || !std::isfinite(to) || from < 0.0F || from > 1.0F || to < 0.0F || to > 1.0F) {
    throw std::invalid_argument("HuxerUI transition opacity must be in the unit interval");
  }
  opacity_ = ScalarTrack{from, to};
  return std::move(*this);
}

Transition Transition::Offset(Point from, Point to) && {
  if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(to.x) || !std::isfinite(to.y)) {
    throw std::invalid_argument("HuxerUI transition offset must be finite");
  }
  offset_ = PointTrack{from, to};
  return std::move(*this);
}

Transition Transition::Scale(float from, float to, TransformOrigin origin) && {
  ValidateOrigin(origin);
  if (!std::isfinite(from) || !std::isfinite(to) || from < 0.0F || to < 0.0F) {
    throw std::invalid_argument("HuxerUI transition scale must be finite and non-negative");
  }
  scale_ = ScalarTrack{from, to};
  scale_origin_ = origin;
  return std::move(*this);
}

Transition Transition::Rotation(float from_degrees, float to_degrees, TransformOrigin origin) && {
  ValidateOrigin(origin);
  if (!std::isfinite(from_degrees) || !std::isfinite(to_degrees)) {
    throw std::invalid_argument("HuxerUI transition rotation must be finite");
  }
  rotation_ = ScalarTrack{from_degrees, to_degrees};
  rotation_origin_ = origin;
  return std::move(*this);
}

namespace detail {

class TransitionExtension final : public NodeExtension {
public:
  TransitionExtension(huxerui::MountedNode& node, const Transition& modifier) {
    Update(node, modifier);
  }

  void Update(huxerui::MountedNode& node, const Transition& modifier) {
    static_cast<void>(node);
    opacity_ = modifier.opacity_;
    offset_ = modifier.offset_;
    scale_ = modifier.scale_;
    rotation_ = modifier.rotation_;
    scale_origin_ = modifier.scale_origin_;
    rotation_origin_ = modifier.rotation_origin_;
    if (const auto* immediate = std::get_if<float>(&modifier.progress_)) {
      progress_.Set(*immediate);
      initialized_ = true;
      return;
    }
    const auto& animated = std::get<Animated<float>>(modifier.progress_);
    if (!initialized_) {
      progress_.Set(animated.target);
      initialized_ = true;
    } else {
      progress_.AnimateTo(animated.target, animated.animation, animated.playback);
    }
  }

  FrameResult OnFrame(huxerui::MountedNode& node, const FrameInfo& frame) override {
    const MotionAdvanceResult result = progress_.Advance(frame);
    const float progress = std::clamp(progress_.Value(), 0.0F, 1.0F);
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (opacity_.has_value()) {
      mounted.presentation.local_opacity *= Interpolate(*opacity_, progress);
    }
    if (offset_.has_value()) {
      const Point offset{
          offset_->from.x + (offset_->to.x - offset_->from.x) * progress,
          offset_->from.y + (offset_->to.y - offset_->from.y) * progress,
      };
      mounted.presentation.local_transform =
          ComposeTransform(TranslationTransform(offset), mounted.presentation.local_transform);
    }
    if (rotation_.has_value()) {
      constexpr float degrees_to_radians = 3.14159265358979323846F / 180.0F;
      const float radians = Interpolate(*rotation_, progress) * degrees_to_radians;
      const Transform2D rotation{std::cos(radians), std::sin(radians), -std::sin(radians), std::cos(radians)};
      mounted.presentation.local_transform = ComposeTransform(
          AroundOriginTransform(rotation, ResolveOrigin(node, rotation_origin_)),
          mounted.presentation.local_transform
      );
    }
    if (scale_.has_value()) {
      const float scale = Interpolate(*scale_, progress);
      const Transform2D transform{scale, 0.0F, 0.0F, scale};
      mounted.presentation.local_transform = ComposeTransform(
          AroundOriginTransform(transform, ResolveOrigin(node, scale_origin_)),
          mounted.presentation.local_transform
      );
    }
    return {result.needs_frame, result.wake_after};
  }

private:
  static float Interpolate(const Transition::ScalarTrack& track, float progress) noexcept {
    return track.from + (track.to - track.from) * progress;
  }

  MotionController progress_;
  std::optional<Transition::ScalarTrack> opacity_;
  std::optional<Transition::PointTrack> offset_;
  std::optional<Transition::ScalarTrack> scale_;
  std::optional<Transition::ScalarTrack> rotation_;
  TransformOrigin scale_origin_;
  TransformOrigin rotation_origin_;
  bool initialized_ = false;
};

} // namespace detail

const detail::ModifierDescriptor& Opacity::Descriptor() {
  return detail::ModifierDescriptorFor<Opacity, OpacityExtension>();
}

const detail::ModifierDescriptor& Offset::Descriptor() {
  return detail::ModifierDescriptorFor<Offset, OffsetExtension>();
}

const detail::ModifierDescriptor& Scale::Descriptor() {
  return detail::ModifierDescriptorFor<Scale, ScaleExtension>();
}

const detail::ModifierDescriptor& Rotation::Descriptor() {
  return detail::ModifierDescriptorFor<Rotation, RotationExtension>();
}

const detail::ModifierDescriptor& Transition::Descriptor() {
  return detail::ModifierDescriptorFor<Transition, detail::TransitionExtension>();
}

} // namespace huxerui
