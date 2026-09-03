#include "advanced_effects.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace visual_effects {

using namespace huxerui;

namespace {

constexpr float pi = std::numbers::pi_v<float>;

Color ScaledAlpha(Color color, float opacity) {
  color.alpha *= std::clamp(opacity, 0.0F, 1.0F);
  return color;
}

bool AdvanceLoop(float& phase, double duration, float reduced_motion_phase, const FrameInfo& frame) {
  if (frame.reduced_motion || duration <= 0.0) {
    phase = reduced_motion_phase;
    return false;
  }
  phase = std::fmod(phase + static_cast<float>(std::max(0.0, frame.delta_time) / duration), 1.0F);
  return true;
}

Transform2D RotationAroundCenter(float radians) {
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  return {
      .m11 = cosine,
      .m12 = sine,
      .m21 = -sine,
      .m22 = cosine,
      .translate_x = 0.5F - 0.5F * cosine + 0.5F * sine,
      .translate_y = 0.5F - 0.5F * sine - 0.5F * cosine,
  };
}

Path CirclePath(Point center, float radius) {
  return Path()
      .MoveTo({center.x + radius, center.y})
      .ArcTo({radius, radius}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {center.x - radius, center.y})
      .ArcTo({radius, radius}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {center.x + radius, center.y})
      .Close();
}

Path AuroraPath(Size size, float phase) {
  const float angle = phase * 2.0F * pi;
  const Point top{
      size.width * (0.50F + std::sin(angle) * 0.045F),
      size.height * (0.10F + std::cos(angle * 1.3F) * 0.025F),
  };
  const Point right{
      size.width * (0.90F + std::sin(angle + 1.2F) * 0.025F),
      size.height * (0.48F + std::cos(angle * 0.9F) * 0.05F),
  };
  const Point bottom{
      size.width * (0.52F + std::cos(angle * 0.8F) * 0.055F),
      size.height * (0.90F + std::sin(angle + 2.1F) * 0.025F),
  };
  const Point left{
      size.width * (0.10F + std::cos(angle + 0.8F) * 0.025F),
      size.height * (0.52F + std::sin(angle * 1.1F) * 0.05F),
  };

  return Path()
      .MoveTo(top)
      .CubicTo(
          {size.width * 0.76F, size.height * (0.04F + std::sin(angle + 0.5F) * 0.03F)},
          {size.width * 0.97F, size.height * 0.25F},
          right
      )
      .CubicTo(
          {size.width * 0.96F, size.height * 0.75F},
          {size.width * 0.78F, size.height * (0.96F + std::cos(angle + 0.7F) * 0.025F)},
          bottom
      )
      .CubicTo(
          {size.width * 0.27F, size.height * 0.96F},
          {size.width * 0.04F, size.height * 0.76F},
          left
      )
      .CubicTo(
          {size.width * 0.04F, size.height * 0.25F},
          {size.width * 0.25F, size.height * (0.04F + std::cos(angle + 1.4F) * 0.025F)},
          top
      )
      .Close();
}

struct GradientGlow {
  class Extension;

  float corner_radius = 20.0F;
  float stroke_width = 3.0F;
  double cycle_duration = 4.2;

  bool operator==(const GradientGlow&) const = default;
};

class GradientGlow::Extension final : public NodeExtension {
public:
  Extension(MountedNode& node, const GradientGlow& value) {
    Update(node, value);
  }

  void Update(MountedNode&, const GradientGlow& value) {
    if (value_ == value) {
      return;
    }
    value_ = value;
    InvalidatePaint(PaintInvalidation::Both);
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
    const Size next = node.LayoutSize();
    if (next == size_) {
      return PaintInvalidation::None;
    }
    size_ = next;
    return PaintInvalidation::Both;
  }

  FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
    const float previous = phase_;
    const bool running = AdvanceLoop(phase_, value_.cycle_duration, 0.12F, frame);
    if (phase_ != previous) {
      InvalidatePaint(PaintInvalidation::Both);
    }
    return {.needs_frame = running};
  }

  void PaintBehindContent(const MountedNode&, PaintContext& context) const override {
    const Path outline = Outline();
    if (!outline.IsEmpty()) {
      context.DrawPathShadow(outline, Color::Rgb(151, 117, 255, 0.28F), {}, 22.0F);
    }
  }

  void PaintAboveContent(const MountedNode&, PaintContext& context) const override {
    const Path outline = Outline();
    if (outline.IsEmpty()) {
      return;
    }
    const Rect bounds{0.0F, 0.0F, size_.width, size_.height};
    context.StrokePath(
        outline,
        LinearGradient{
            .start = {0.0F, 0.5F},
            .end = {1.0F, 0.5F},
            .stops = {
                {0.0F, Color::Rgb(126, 103, 255)},
                {0.34F, Color::Rgb(244, 114, 255)},
                {0.68F, Color::Rgb(255, 174, 92)},
                {1.0F, Color::Rgb(80, 224, 255)},
            },
            .transform = RotationAroundCenter(phase_ * 2.0F * pi),
        },
        bounds,
        StrokeStyle{.width = value_.stroke_width, .cap = StrokeCap::Round, .join = StrokeJoin::Round}
    );
  }

private:
  [[nodiscard]] Path Outline() const {
    const float inset = value_.stroke_width * 0.5F + 1.0F;
    if (size_.width <= inset * 2.0F || size_.height <= inset * 2.0F) {
      return {};
    }
    return Path::RoundedRect(
        {inset, inset, size_.width - inset * 2.0F, size_.height - inset * 2.0F},
        CornerRadii{std::max(0.0F, value_.corner_radius - inset)}
    );
  }

  GradientGlow value_;
  Size size_;
  float phase_ = 0.0F;
};

struct Shimmer {
  class Extension;

  float corner_radius = 18.0F;
  double cycle_duration = 1.45;

  bool operator==(const Shimmer&) const = default;
};

class Shimmer::Extension final : public NodeExtension {
public:
  Extension(MountedNode& node, const Shimmer& value) {
    Update(node, value);
  }

  void Update(MountedNode&, const Shimmer& value) {
    if (value_ == value) {
      return;
    }
    value_ = value;
    InvalidatePaint();
  }

  FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
    const float previous = phase_;
    const bool running = AdvanceLoop(phase_, value_.cycle_duration, 0.52F, frame);
    if (phase_ != previous) {
      InvalidatePaint();
    }
    return {.needs_frame = running};
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const Size size = node.LayoutSize();
    if (size.width <= 0.0F || size.height <= 0.0F) {
      return;
    }
    const float center = -0.35F + phase_ * 1.7F;
    context.DrawRect(
        {0.0F, 0.0F, size.width, size.height},
        LinearGradient{
            .start = {center - 0.28F, 0.0F},
            .end = {center + 0.28F, 1.0F},
            .stops = {
                {0.0F, Color::Transparent()},
                {0.42F, Color::Transparent()},
                {0.5F, Color::Rgb(255, 255, 255, 0.22F)},
                {0.58F, Color::Transparent()},
                {1.0F, Color::Transparent()},
            },
        },
        CornerRadii{value_.corner_radius}
    );
  }

private:
  Shimmer value_;
  float phase_ = 0.0F;
};

struct ParticleField {
  class Extension;

  float corner_radius = 18.0F;
  std::size_t particle_count = 28;

  bool operator==(const ParticleField&) const = default;
};

class ParticleField::Extension final : public NodeExtension {
public:
  Extension(MountedNode&, const ParticleField& value) : value_(value) {
    InitializeParticles();
  }

  void Update(MountedNode&, const ParticleField& value) {
    if (value_ == value) {
      return;
    }
    const bool count_changed = value_.particle_count != value.particle_count;
    value_ = value;
    if (count_changed) {
      InitializeParticles();
    }
    InvalidatePaint(PaintInvalidation::Content);
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
    const Size next = node.LayoutSize();
    if (next == size_) {
      return PaintInvalidation::None;
    }
    size_ = next;
    return PaintInvalidation::Content;
  }

  [[nodiscard]] bool HoverHitTest(MountedNode& node, Point position) const override {
    const Size size = node.LayoutSize();
    return node.IsEnabled() && Rect{0.0F, 0.0F, size.width, size.height}.Contains(position);
  }

  void OnHover(MountedNode&, const HoverEvent& event) override {
    hovering_ = event.type != HoverEventType::Leave;
    if (hovering_ && size_.width > 0.0F && size_.height > 0.0F) {
      pointer_ = {
          std::clamp(event.position.x / size_.width, 0.0F, 1.0F),
          std::clamp(event.position.y / size_.height, 0.0F, 1.0F),
      };
    }
    InvalidatePaint(PaintInvalidation::Content);
  }

  FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
    if (frame.reduced_motion) {
      const bool changed = !reduced_motion_;
      reduced_motion_ = true;
      if (changed) {
        InvalidatePaint(PaintInvalidation::Content);
      }
      return {};
    }

    const float delta = static_cast<float>(std::min(1.0 / 30.0, std::max(0.0, frame.delta_time)));
    time_ += delta;
    reduced_motion_ = false;
    for (std::size_t index = 0; index < particles_.size(); ++index) {
      Particle& particle = particles_[index];
      const float curl = time_ * 0.55F + static_cast<float>(index) * 1.73F;
      Point acceleration{std::cos(curl) * 0.0028F, std::sin(curl * 0.91F) * 0.0028F};
      if (hovering_) {
        const Point direction = pointer_ - particle.position;
        const float distance_squared = direction.x * direction.x + direction.y * direction.y;
        const float force = std::min(0.12F, 0.0018F / std::max(0.012F, distance_squared));
        acceleration += direction * force;
        acceleration += Point{-direction.y, direction.x} * (force * 0.28F);
      }
      particle.velocity += acceleration * delta;
      particle.velocity *= std::pow(0.998F, delta * 60.0F);
      const float speed = std::hypot(particle.velocity.x, particle.velocity.y);
      if (speed > 0.11F) {
        particle.velocity *= 0.11F / speed;
      }
      particle.position += particle.velocity * delta;
      Bounce(particle.position.x, particle.velocity.x);
      Bounce(particle.position.y, particle.velocity.y);
    }
    InvalidatePaint(PaintInvalidation::Content);
    return {.needs_frame = true};
  }

  void PaintBehindContent(const MountedNode&, PaintContext& context) const override {
    if (size_.width <= 0.0F || size_.height <= 0.0F) {
      return;
    }
    const Rect bounds{0.0F, 0.0F, size_.width, size_.height};
    context.PushClip(bounds, CornerRadii{value_.corner_radius});
    if (hovering_) {
      context.DrawRect(
          bounds,
          RadialGradient{
              .center = pointer_,
              .radius = {0.32F, 0.62F},
              .stops = {
                  {0.0F, Color::Rgb(120, 99, 255, 0.32F)},
                  {0.45F, Color::Rgb(64, 190, 255, 0.12F)},
                  {1.0F, Color::Transparent()},
              },
          }
      );
    }
    for (std::size_t left = 0; left < particles_.size(); ++left) {
      const Point start = ToLocal(particles_[left].position);
      for (std::size_t right = left + 1; right < particles_.size(); ++right) {
        const Point end = ToLocal(particles_[right].position);
        const float distance = std::hypot(end.x - start.x, end.y - start.y);
        if (distance < 92.0F) {
          context.DrawLine(
              start,
              end,
              ScaledAlpha(Color::Rgb(148, 180, 255, 0.28F), 1.0F - distance / 92.0F),
              StrokeStyle{.width = 1.0F, .cap = StrokeCap::Round}
          );
        }
      }
    }
    for (std::size_t index = 0; index < particles_.size(); ++index) {
      const Color color = index % 3 == 0   ? Color::Rgb(244, 114, 255, 0.92F)
                          : index % 3 == 1 ? Color::Rgb(80, 224, 255, 0.9F)
                                           : Color::Rgb(255, 190, 104, 0.86F);
      context.DrawCircle(ToLocal(particles_[index].position), 1.8F + static_cast<float>(index % 3) * 0.55F, color);
    }
    context.PopClip();
  }

private:
  struct Particle {
    Point position;
    Point velocity;
  };

  void InitializeParticles() {
    particles_.clear();
    particles_.reserve(value_.particle_count);
    for (std::size_t index = 0; index < value_.particle_count; ++index) {
      const float ordinal = static_cast<float>(index);
      const float angle = ordinal * 2.399963F;
      const float speed = 0.026F + static_cast<float>(index % 5) * 0.004F;
      particles_.push_back({
          .position = {
              std::fmod(0.17F + ordinal * 0.618034F, 1.0F),
              std::fmod(0.31F + ordinal * 0.414214F, 1.0F),
          },
          .velocity = {std::cos(angle) * speed, std::sin(angle) * speed},
      });
    }
  }

  static void Bounce(float& position, float& velocity) {
    if (position < 0.02F) {
      position = 0.02F;
      velocity = std::abs(velocity);
    } else if (position > 0.98F) {
      position = 0.98F;
      velocity = -std::abs(velocity);
    }
  }

  [[nodiscard]] Point ToLocal(Point point) const {
    return {point.x * size_.width, point.y * size_.height};
  }

  ParticleField value_;
  std::vector<Particle> particles_;
  Size size_;
  Point pointer_{0.5F, 0.5F};
  float time_ = 0.0F;
  bool hovering_ = false;
  bool reduced_motion_ = false;
};

struct LiquidAurora {
  class Extension;

  float corner_radius = 18.0F;
  double cycle_duration = 6.4;

  bool operator==(const LiquidAurora&) const = default;
};

class LiquidAurora::Extension final : public NodeExtension {
public:
  Extension(MountedNode& node, const LiquidAurora& value) {
    Update(node, value);
  }

  void Update(MountedNode&, const LiquidAurora& value) {
    if (value_ == value) {
      return;
    }
    value_ = value;
    InvalidatePaint(PaintInvalidation::Content);
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
    const Size next = node.LayoutSize();
    if (next == size_) {
      return PaintInvalidation::None;
    }
    size_ = next;
    return PaintInvalidation::Content;
  }

  FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
    const float previous = phase_;
    const bool running = AdvanceLoop(phase_, value_.cycle_duration, 0.17F, frame);
    if (phase_ != previous) {
      InvalidatePaint(PaintInvalidation::Content);
    }
    return {.needs_frame = running};
  }

  void PaintBehindContent(const MountedNode&, PaintContext& context) const override {
    if (size_.width <= 0.0F || size_.height <= 0.0F) {
      return;
    }
    const Rect bounds{0.0F, 0.0F, size_.width, size_.height};
    const Path shape = AuroraPath(size_, phase_);
    const float angle = phase_ * 2.0F * pi;

    context.PushClip(bounds, CornerRadii{value_.corner_radius});
    context.DrawPathShadow(shape, Color::Rgb(126, 103, 255, 0.34F), {}, 24.0F);
    context.FillPath(
        shape,
        LinearGradient{
            .start = {0.0F, 0.15F},
            .end = {1.0F, 0.85F},
            .stops = {
                {0.0F, Color::Rgb(80, 224, 255)},
                {0.34F, Color::Rgb(126, 103, 255)},
                {0.68F, Color::Rgb(244, 114, 255)},
                {1.0F, Color::Rgb(255, 190, 104)},
            },
            .transform = RotationAroundCenter(angle * 0.45F),
        },
        bounds
    );
    context.PushPathClip(shape);
    context.DrawRect(
        bounds,
        RadialGradient{
            .center = {
                0.50F + std::cos(angle) * 0.24F,
                0.50F + std::sin(angle * 0.8F) * 0.18F,
            },
            .radius = {0.32F, 0.58F},
            .stops = {
                {0.0F, Color::Rgb(255, 255, 255, 0.52F)},
                {0.28F, Color::Rgb(112, 245, 217, 0.20F)},
                {1.0F, Color::Transparent()},
            },
        }
    );
    context.PopClip();
    context.StrokePath(
        shape,
        Color::Rgb(255, 255, 255, 0.22F),
        StrokeStyle{.width = 1.25F, .cap = StrokeCap::Round, .join = StrokeJoin::Round}
    );
    context.PopClip();
  }

private:
  LiquidAurora value_;
  Size size_;
  float phase_ = 0.0F;
};

struct OrbitMotion {
  class Extension;

  double cycle_duration = 1.8;

  bool operator==(const OrbitMotion&) const = default;
};

class OrbitMotion::Extension final : public NodeExtension {
public:
  Extension(MountedNode& node, const OrbitMotion& value) {
    Update(node, value);
  }

  void Update(MountedNode&, const OrbitMotion& value) {
    if (value_ == value) {
      return;
    }
    value_ = value;
    InvalidatePaint();
  }

  FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
    const float previous = phase_;
    const bool running = AdvanceLoop(phase_, value_.cycle_duration, 0.18F, frame);
    if (phase_ != previous) {
      InvalidatePaint();
    }
    return {.needs_frame = running};
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const Size size = node.LayoutSize();
    const float extent = std::min(size.width, size.height);
    if (extent <= 0.0F) {
      return;
    }
    const Point center{size.width * 0.5F, size.height * 0.5F};
    const float radius = extent * 0.34F;
    const float circumference = 2.0F * pi * radius;
    const Path orbit = CirclePath(center, radius);
    context.StrokePath(orbit, Color::Rgb(255, 255, 255, 0.1F), StrokeStyle{.width = 5.0F});
    context.StrokePath(
        orbit,
        LinearGradient{
            .start = {0.0F, 0.5F},
            .end = {1.0F, 0.5F},
            .stops = {
                {0.0F, Color::Rgb(126, 103, 255)},
                {0.5F, Color::Rgb(244, 114, 255)},
                {1.0F, Color::Rgb(80, 224, 255)},
            },
            .transform = RotationAroundCenter(phase_ * 2.0F * pi),
        },
        StrokeStyle{
            .width = 6.0F,
            .cap = StrokeCap::Round,
            .join = StrokeJoin::Round,
            .dash_pattern = {circumference * 0.27F, circumference * 0.73F},
            .dash_offset = -phase_ * circumference,
        }
    );
    context.DrawArc(
        center,
        radius * 0.62F,
        phase_ * -2.0F * pi,
        pi * 0.92F,
        Color::Rgb(255, 190, 104, 0.72F),
        StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round}
    );
    const float angle = phase_ * 2.0F * pi - pi * 0.5F;
    context.DrawCircle(
        {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius},
        4.5F,
        Color::White()
    );
    context.DrawCircle(center, 5.0F, Color::Rgb(196, 172, 255));
  }

private:
  OrbitMotion value_;
  float phase_ = 0.0F;
};

} // namespace

View GradientGlowSurface(View content) {
  return std::move(content).With(GradientGlow{});
}

View ShimmerSurface(View content) {
  return std::move(content).With(Shimmer{});
}

View ParticleFieldSurface(View content) {
  return std::move(content).With(ParticleField{}, PointerCursor(PointerCursorKind::Crosshair), ClipChildren());
}

View OrbitLoader() {
  return Stack {}.With(OrbitMotion{}, Frame{.width = 116.0F, .height = 116.0F}, Semantics{.hidden = true});
}

View LiquidAuroraSurface(View content) {
  return std::move(content).With(LiquidAurora{});
}

} // namespace visual_effects
