#include <huxerui/animation.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <huxerui/theme.h>

#include "internal.h"

namespace huxerui {

namespace {

bool ReducedMotion(const detail::MountedNode& node) {
  const std::any* value = detail::FindEnvironmentValue(node.environment, typeid(ThemeKey));
  const auto* theme = value ? std::any_cast<ThemeSpec>(value) : nullptr;
  return theme && theme->motion.reduced_motion;
}

void ValidateOrigin(TransformOrigin origin) {
  if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) {
    throw std::invalid_argument("HuxerUI transform origin must be finite");
  }
}

Point ResolveOrigin(const MountedNode& node, TransformOrigin origin) {
  const Rect frame = node.Frame();
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
      return;
    }
    const auto& animated = std::get<Animated<float>>(modifier.value);
    value_.Update(std::clamp(animated.target, 0.0F, 1.0F), animated.animation);
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool running = value_.Advance(frame.timestamp, frame.delta_time, ReducedMotion(mounted));
    mounted.presentation.local_opacity *= value_.Value();
    return {running, std::nullopt};
  }

private:
  detail::AnimatedValue<float> value_;
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
      return;
    }
    const auto& animated = std::get<Animated<Point>>(modifier.value);
    x_.Update(animated.target.x, animated.animation);
    y_.Update(animated.target.y, animated.animation);
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool reduced = ReducedMotion(mounted);
    const bool x_running = x_.Advance(frame.timestamp, frame.delta_time, reduced);
    const bool y_running = y_.Advance(frame.timestamp, frame.delta_time, reduced);
    mounted.presentation.local_transform = detail::ComposeTransform(
        detail::TranslationTransform({x_.Value(), y_.Value()}),
        mounted.presentation.local_transform
    );
    return {
        x_running || y_running,
        std::nullopt,
    };
  }

private:
  detail::AnimatedValue<float> x_;
  detail::AnimatedValue<float> y_;
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
      return;
    }
    const auto& animated = std::get<Animated<float>>(modifier.value);
    ValidateScale(animated.target);
    value_.Update(animated.target, animated.animation);
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool running = value_.Advance(frame.timestamp, frame.delta_time, ReducedMotion(mounted));
    const Point origin = ResolveOrigin(node, origin_);
    const float value = value_.Value();
    const detail::PresentationTransform scale{
        value,
        0.0F,
        0.0F,
        value,
    };
    mounted.presentation.local_transform =
        detail::ComposeTransform(detail::AroundOriginTransform(scale, origin), mounted.presentation.local_transform);
    return {running, std::nullopt};
  }

private:
  static void ValidateScale(float value) {
    if (!std::isfinite(value) || value < 0.0F) {
      throw std::invalid_argument("HuxerUI scale must be finite and non-negative");
    }
  }

  detail::AnimatedValue<float> value_;
  TransformOrigin origin_;
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
      return;
    }
    const auto& animated = std::get<Animated<float>>(modifier.degrees);
    ValidateDegrees(animated.target);
    degrees_.Update(animated.target, animated.animation);
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool running = degrees_.Advance(frame.timestamp, frame.delta_time, ReducedMotion(mounted));
    constexpr float degrees_to_radians = 3.14159265358979323846F / 180.0F;
    const float radians = degrees_.Value() * degrees_to_radians;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const detail::PresentationTransform rotation{
        cosine,
        sine,
        -sine,
        cosine,
    };
    mounted.presentation.local_transform = detail::ComposeTransform(
        detail::AroundOriginTransform(rotation, ResolveOrigin(node, origin_)),
        mounted.presentation.local_transform
    );
    return {running, std::nullopt};
  }

private:
  static void ValidateDegrees(float value) {
    if (!std::isfinite(value)) {
      throw std::invalid_argument("HuxerUI rotation must be finite");
    }
  }

  detail::AnimatedValue<float> degrees_;
  TransformOrigin origin_;
};

} // namespace

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

} // namespace huxerui
