#include <huxerui/animation.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <huxerui/theme.h>

#include "internal.h"

namespace huxerui {

namespace {

class AnimatedScalar {
public:
  void Set(float value) noexcept {
    current_ = value;
    start_ = value;
    target_ = value;
    velocity_ = 0.0F;
    running_ = false;
    initialized_ = true;
    pending_ = false;
  }

  void Update(float target, AnimationSpec animation) {
    if (!initialized_) {
      Set(target);
      animation_ = std::move(animation);
      return;
    }
    animation_ = std::move(animation);
    if (target == target_ && !pending_) {
      return;
    }
    target_ = target;
    pending_ = true;
  }

  bool Advance(double timestamp, double delta_time,
               bool reduced_motion) {
    if (pending_) {
      pending_ = false;
      if (reduced_motion ||
          std::holds_alternative<SnapSpec>(animation_)) {
        Set(target_);
        return false;
      }
      start_ = current_;
      start_time_ = timestamp;
      running_ = true;
    }
    if (!running_) {
      return false;
    }

    if (const auto *tween =
            std::get_if<TweenSpec>(&animation_)) {
      if (!std::isfinite(tween->duration) ||
          tween->duration <= 0.0) {
        Set(target_);
        return false;
      }
      const double progress = std::clamp(
          (timestamp - start_time_) / tween->duration,
          0.0, 1.0);
      double eased = progress;
      if (tween->easing == Easing::EaseOut) {
        const double inverse = 1.0 - progress;
        eased = 1.0 - inverse * inverse * inverse;
      }
      current_ = start_ +
                 (target_ - start_) *
                     static_cast<float>(eased);
      if (progress >= 1.0) {
        Set(target_);
      }
      return running_;
    }

    const auto &spring = std::get<SpringSpec>(animation_);
    if (!std::isfinite(spring.stiffness) ||
        spring.stiffness <= 0.0F ||
        !std::isfinite(spring.damping_ratio) ||
        spring.damping_ratio < 0.0F) {
      Set(target_);
      return false;
    }
    const float step = static_cast<float>(
        std::clamp(delta_time, 0.0, 1.0 / 30.0));
    const float damping =
        2.0F * std::sqrt(spring.stiffness) *
        spring.damping_ratio;
    const float acceleration =
        spring.stiffness * (target_ - current_) -
        damping * velocity_;
    velocity_ += acceleration * step;
    current_ += velocity_ * step;
    if (std::abs(target_ - current_) < 0.001F &&
        std::abs(velocity_) < 0.001F) {
      Set(target_);
    }
    return running_;
  }

  [[nodiscard]] float Value() const noexcept {
    return current_;
  }

private:
  AnimationSpec animation_ = SnapSpec{};
  float current_ = 0.0F;
  float start_ = 0.0F;
  float target_ = 0.0F;
  float velocity_ = 0.0F;
  double start_time_ = 0.0;
  bool initialized_ = false;
  bool pending_ = false;
  bool running_ = false;
};

bool ReducedMotion(const detail::MountedNode &node) {
  const std::any *value = detail::FindEnvironmentValue(
      node.environment, typeid(ThemeKey));
  const auto *theme = value
                          ? std::any_cast<ThemeSpec>(value)
                          : nullptr;
  return theme && theme->motion.reduced_motion;
}

class MountedOpacity final : public MountedModifier {
public:
  MountedOpacity(MountedNode &node, const Opacity &modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode &node, const Opacity &modifier) {
    static_cast<void>(node);
    if (const auto *immediate =
            std::get_if<float>(&modifier.value)) {
      value_.Set(std::clamp(*immediate, 0.0F, 1.0F));
      return;
    }
    const auto &animated =
        std::get<Animated<float>>(modifier.value);
    value_.Update(
        std::clamp(animated.target, 0.0F, 1.0F),
        animated.animation);
  }

  ModifierFrameResult OnFrame(
      MountedNode &node, const FrameInfo &frame) override {
    auto &mounted = static_cast<detail::MountedNode &>(node);
    const bool running = value_.Advance(
        frame.timestamp, frame.delta_time,
        ReducedMotion(mounted));
    mounted.presentation_opacity *= value_.Value();
    return {running, std::nullopt};
  }

private:
  AnimatedScalar value_;
};

class MountedOffset final : public MountedModifier {
public:
  MountedOffset(MountedNode &node, const Offset &modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode &node, const Offset &modifier) {
    static_cast<void>(node);
    if (const auto *immediate =
            std::get_if<Point>(&modifier.value)) {
      x_.Set(immediate->x);
      y_.Set(immediate->y);
      return;
    }
    const auto &animated =
        std::get<Animated<Point>>(modifier.value);
    x_.Update(animated.target.x, animated.animation);
    y_.Update(animated.target.y, animated.animation);
  }

  ModifierFrameResult OnFrame(
      MountedNode &node, const FrameInfo &frame) override {
    auto &mounted = static_cast<detail::MountedNode &>(node);
    const bool reduced = ReducedMotion(mounted);
    const bool x_running = x_.Advance(
        frame.timestamp, frame.delta_time, reduced);
    const bool y_running = y_.Advance(
        frame.timestamp, frame.delta_time, reduced);
    mounted.presentation_offset.x += x_.Value();
    mounted.presentation_offset.y += y_.Value();
    return {
        x_running || y_running,
        std::nullopt,
    };
  }

private:
  AnimatedScalar x_;
  AnimatedScalar y_;
};

} // namespace

const detail::ModifierDescriptor &Opacity::Descriptor() {
  return detail::ModifierDescriptorFor<Opacity, MountedOpacity>();
}

const detail::ModifierDescriptor &Offset::Descriptor() {
  return detail::ModifierDescriptorFor<Offset, MountedOffset>();
}

} // namespace huxerui
