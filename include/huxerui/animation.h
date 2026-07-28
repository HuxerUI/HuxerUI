#pragma once

#include <variant>

#include <huxerui/geometry.h>
#include <huxerui/modifier.h>

namespace huxerui {

enum class Easing {
  Linear,
  EaseOut,
};

struct SnapSpec {};

struct TweenSpec {
  double duration = 0.2;
  Easing easing = Easing::EaseOut;
};

struct SpringSpec {
  float stiffness = 320.0F;
  float damping_ratio = 0.82F;
};

using AnimationSpec =
    std::variant<SnapSpec, TweenSpec, SpringSpec>;

template <class T> struct Animated {
  T target;
  AnimationSpec animation;
};

template <class T, class Spec>
  requires std::constructible_from<AnimationSpec, Spec>
Animated<T> AnimateTo(T target, Spec &&animation) {
  return {
      std::move(target),
      AnimationSpec(std::forward<Spec>(animation)),
  };
}

struct Opacity {
  explicit Opacity(float value) : value(value) {}
  explicit Opacity(Animated<float> value)
      : value(std::move(value)) {}

  static const detail::ModifierDescriptor &Descriptor();

  std::variant<float, Animated<float>> value;
};

struct Offset {
  explicit Offset(Point value) : value(value) {}
  explicit Offset(Animated<Point> value)
      : value(std::move(value)) {}

  static const detail::ModifierDescriptor &Descriptor();

  std::variant<Point, Animated<Point>> value;
};

} // namespace huxerui
