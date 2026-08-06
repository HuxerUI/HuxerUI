#pragma once

#include <concepts>
#include <utility>
#include <variant>

#include <huxerui/geometry.h>
#include <huxerui/modifier.h>

namespace huxerui {

enum class Easing {
  Linear,
  EaseIn,
  EaseOut,
};

struct SnapSpec {
  bool operator==(const SnapSpec&) const = default;
};

struct TweenSpec {
  double duration = 0.2;
  Easing easing = Easing::EaseOut;

  bool operator==(const TweenSpec&) const = default;
};

struct SpringSpec {
  float stiffness = 320.0F;
  float damping_ratio = 0.82F;

  bool operator==(const SpringSpec&) const = default;
};

using AnimationSpec = std::variant<SnapSpec, TweenSpec, SpringSpec>;

struct TransformOrigin {
  float x = 0.5F;
  float y = 0.5F;

  bool operator==(const TransformOrigin&) const = default;
};

template <class T> struct Animated {
  T target;
  AnimationSpec animation;

  bool operator==(const Animated&) const = default;
};

template <class T, class Spec>
  requires std::constructible_from<AnimationSpec, Spec>
Animated<T> AnimateTo(T target, Spec&& animation) {
  return {
      std::move(target),
      AnimationSpec(std::forward<Spec>(animation)),
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

} // namespace huxerui
