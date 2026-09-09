#include "effects.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

#include <huxerui/paint.h>

namespace transition_studio {
namespace {

using namespace huxerui;

Transform2D AroundCenter(Rect bounds, float scale, float radians = 0.0F) {
  const float cosine = std::cos(radians) * scale;
  const float sine = std::sin(radians) * scale;
  const Point center{bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
  return {
      cosine, sine, -sine, cosine,
      center.x - cosine * center.x + sine * center.y,
      center.y - sine * center.x - cosine * center.y,
  };
}

float StaggerProgress(float progress, float delay) {
  const float value = std::clamp((progress - delay) / (1.0F - delay), 0.0F, 1.0F);
  return value * value * (3.0F - 2.0F * value);
}

Path PrismBand(Rect bounds, int index) {
  const float skew = bounds.width * 0.18F;
  const float left = bounds.x + bounds.width * static_cast<float>(index) / 5.0F - (index == 0 ? skew * 2.0F : 0.0F);
  const float right = bounds.x + bounds.width * static_cast<float>(index + 1) / 5.0F + (index == 4 ? skew * 2.0F : 0.0F);
  Path path;
  path.MoveTo({left + skew, bounds.y})
      .LineTo({right + skew, bounds.y})
      .LineTo({right - skew, bounds.y + bounds.height})
      .LineTo({left - skew, bounds.y + bounds.height})
      .Close();
  return path;
}

Transform2D PrismTransform(Rect bounds, int index, float progress) {
  const float remaining = 1.0F - StaggerProgress(progress, static_cast<float>(index) * 0.04F);
  return {1.0F, 0.0F, 0.0F, 1.0F, -remaining * (bounds.width + bounds.height * 0.4F),
          remaining * bounds.height * (index % 2 == 0 ? 0.12F : -0.12F)};
}

struct PrismSweep {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    if (context.progress >= 1.0F) { return FadeTransition{}.Evaluate(context); }
    TransitionFrame frame;
    for (int index = 0; index < 5; ++index) {
      frame.incoming.fragments.push_back({
          .source_clip = ClipShape::FromPath(PrismBand(context.bounds, index)),
          .transform = PrismTransform(context.bounds, index, context.progress),
      });
    }
    return frame;
  }

  void Paint(PaintContext& paint, const TransitionContext& context) const {
    for (int index = 0; index < 5; ++index) {
      const float phase = StaggerProgress(context.progress, static_cast<float>(index) * 0.04F);
      const float alpha = std::sin(phase * std::numbers::pi_v<float>);
      if (alpha <= 0.001F) { continue; }
      const Path band = PrismBand(context.bounds, index);
      paint.PushTransform(PrismTransform(context.bounds, index, context.progress));
      const Color edge = index % 2 == 0 ? Color::Rgb(88, 220, 255, alpha) : Color::Rgb(223, 139, 255, alpha);
      paint.StrokePath(band, Color{edge.red, edge.green, edge.blue, alpha * 0.14F}, StrokeStyle{.width = 14.0F});
      paint.StrokePath(band, edge, StrokeStyle{.width = 2.0F});
      paint.PopTransform();
    }
  }

  bool operator==(const PrismSweep&) const = default;
};

Transform2D GateTransform(Rect bounds, bool right, float progress) {
  auto transform = AroundCenter(bounds, 1.0F, (right ? 1.0F : -1.0F) * progress * 0.12F);
  transform.translate_x += (right ? 1.0F : -1.0F) * progress * (bounds.width * 0.7F + bounds.height * 0.12F);
  return transform;
}

struct SplitGate {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);
    TransitionFrame frame;
    frame.order = TransitionOrder::OutgoingAbove;
    for (int index = 0; index < 2; ++index) {
      const Rect panel{context.bounds.x + context.bounds.width * 0.5F * static_cast<float>(index), context.bounds.y,
                       context.bounds.width * 0.5F, context.bounds.height};
      frame.outgoing.fragments.push_back({
          .source_clip = ClipShape::Rectangle(panel),
          .transform = GateTransform(context.bounds, index == 1, progress),
      });
    }
    frame.incoming.transform = AroundCenter(context.bounds, 0.92F + 0.08F * progress);
    return frame;
  }

  void Paint(PaintContext& paint, const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);
    const float alpha = std::sin(progress * std::numbers::pi_v<float>);
    const float center = context.bounds.x + context.bounds.width * 0.5F;
    for (bool right : {false, true}) {
      paint.PushTransform(GateTransform(context.bounds, right, progress));
      paint.DrawLine({center, context.bounds.y}, {center, context.bounds.y + context.bounds.height},
                     Color::Rgb(249, 194, 110, alpha * 0.8F), StrokeStyle{.width = 3.0F});
      paint.PopTransform();
    }
  }

  bool operator==(const SplitGate&) const = default;
};

struct CascadeMosaic {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    if (context.progress >= 1.0F) { return FadeTransition{}.Evaluate(context); }
    TransitionFrame frame;
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 4; ++column) {
        const float phase = StaggerProgress(context.progress, static_cast<float>(row + column) * 0.055F);
        const Rect tile{context.bounds.x + context.bounds.width * static_cast<float>(column) / 4.0F,
                        context.bounds.y + context.bounds.height * static_cast<float>(row) / 3.0F,
                        context.bounds.width / 4.0F, context.bounds.height / 3.0F};
        auto transform = AroundCenter(tile, 0.60F + 0.40F * phase,
                                     (column % 2 == 0 ? -0.18F : 0.18F) * (1.0F - phase));
        transform.translate_y += context.bounds.height * 0.28F * (1.0F - phase);
        frame.incoming.fragments.push_back({
            .source_clip = ClipShape::Rectangle(tile),
            .transform = transform,
            .opacity = phase,
        });
      }
    }
    return frame;
  }

  bool operator==(const CascadeMosaic&) const = default;
};

struct CenteredReveal {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    auto input = context;
    // Navigation has no implicit origin; SceneTransition can supply the activation position.
    if (!input.origin) {
      input.origin = Point{input.bounds.x + input.bounds.width * 0.5F, input.bounds.y + input.bounds.height * 0.5F};
    }
    return CircularRevealTransition{}.Evaluate(input);
  }
  bool operator==(const CenteredReveal&) const = default;
};

struct DiagonalSweep {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);
    const Rect bounds = context.bounds;
    const float reach = (bounds.width + bounds.height) * progress;
    Path aperture;
    aperture.MoveTo({bounds.x, bounds.y})
        .LineTo({bounds.x + reach, bounds.y})
        .LineTo({bounds.x, bounds.y + reach})
        .Close();
    TransitionFrame frame;
    frame.incoming.clip = ClipShape::FromPath(std::move(aperture));
    return frame;
  }
  bool operator==(const DiagonalSweep&) const = default;
};

struct VenetianBlinds {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);
    const Rect bounds = context.bounds;
    constexpr int strips = 12;
    const float band = bounds.height / strips;
    Path apertures;
    for (int index = 0; index < strips; ++index) {
      const float top = bounds.y + band * (static_cast<float>(index) + (1.0F - progress) * 0.5F);
      const float bottom = top + band * progress;
      apertures.MoveTo({bounds.x, top})
          .LineTo({bounds.x + bounds.width, top})
          .LineTo({bounds.x + bounds.width, bottom})
          .LineTo({bounds.x, bottom})
          .Close();
    }
    TransitionFrame frame;
    frame.incoming.clip = ClipShape::FromPath(std::move(apertures));
    return frame;
  }
  bool operator==(const VenetianBlinds&) const = default;
};

struct OrbitAndSettle {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    const float progress = context.progress;
    auto frame = FadeTransition{}.Evaluate(context);
    frame.incoming.transform = AroundCenter(
        context.bounds, 0.72F + 0.28F * progress, (progress - 1.0F) * std::numbers::pi_v<float> / 9.0F
    );
    frame.outgoing.transform = AroundCenter(
        context.bounds, 1.0F - 0.12F * progress, progress * std::numbers::pi_v<float> / 18.0F
    );
    return frame;
  }
  bool operator==(const OrbitAndSettle&) const = default;
};

struct CurtainLift {
  TransitionFrame Evaluate(const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);
    const Rect bounds = context.bounds;
    TransitionFrame frame;
    frame.order = TransitionOrder::OutgoingAbove;
    frame.outgoing.clip = ClipShape::Rectangle({bounds.x, bounds.y, bounds.width, bounds.height * (1.0F - progress)});
    frame.outgoing.transform.translate_y = -bounds.height * progress * 0.15F;
    frame.incoming.transform = AroundCenter(bounds, 0.94F + 0.06F * progress);
    return frame;
  }
  bool operator==(const CurtainLift&) const = default;
};

float TearNoise(std::uint32_t seed, std::uint32_t index) {
  std::uint32_t value = seed ^ (index * 0x9E3779B9U);
  value ^= value >> 16U;
  value *= 0x7FEB352DU;
  value ^= value >> 15U;
  return static_cast<float>(value & 0xFFFFU) / 65535.0F;
}

struct TearGeometry {
  std::array<Point, 13> seam;
  Path left;
  Path right;
  Transform2D left_transform;
  Transform2D right_transform;
  float opening;
};

TearGeometry SampleTear(const TransitionContext& context, std::uint32_t seed) {
  const Rect bounds = context.bounds;
  const float phase = std::clamp((context.progress - 0.12F) / 0.76F, 0.0F, 1.0F);
  const float opening = phase * phase * (3.0F - 2.0F * phase);
  TearGeometry tear{};
  tear.opening = opening;
  for (std::size_t index = 0; index < tear.seam.size(); ++index) {
    const float y = static_cast<float>(index) / static_cast<float>(tear.seam.size() - 1);
    const float jitter = (TearNoise(seed, static_cast<std::uint32_t>(index)) - 0.5F) * 0.10F;
    tear.seam[index] = {bounds.x + bounds.width * (0.54F - y * 0.08F + jitter), bounds.y + bounds.height * y};
  }
  tear.left.MoveTo({bounds.x, bounds.y});
  tear.right.MoveTo({bounds.x + bounds.width, bounds.y});
  for (Point point : tear.seam) {
    tear.left.LineTo(point);
    tear.right.LineTo(point);
  }
  tear.left.LineTo({bounds.x, bounds.y + bounds.height}).Close();
  tear.right.LineTo({bounds.x + bounds.width, bounds.y + bounds.height}).Close();
  const float angle = opening * std::numbers::pi_v<float> / 30.0F;
  const float travel = opening * (bounds.width * 0.9F + bounds.height * 0.12F);
  tear.left_transform = AroundCenter(bounds, 1.0F, -angle);
  tear.right_transform = AroundCenter(bounds, 1.0F, angle);
  tear.left_transform.translate_x -= travel;
  tear.right_transform.translate_x += travel;
  tear.left_transform.translate_y += opening * bounds.height * 0.025F;
  tear.right_transform.translate_y -= opening * bounds.height * 0.025F;
  return tear;
}

struct TearTransition {
  std::uint32_t seed = 17;

  TransitionFrame Evaluate(const TransitionContext& context) const {
    const auto tear = SampleTear(context, seed);
    TransitionFrame frame;
    frame.order = TransitionOrder::OutgoingAbove;
    if (tear.opening > 0.0F) {
      frame.outgoing.fragments = {
        TransitionFragment{.source_clip = ClipShape::FromPath(tear.left), .transform = tear.left_transform},
        TransitionFragment{.source_clip = ClipShape::FromPath(tear.right), .transform = tear.right_transform},
      };
    }
    frame.incoming.transform = AroundCenter(context.bounds, 0.97F + 0.03F * tear.opening);
    return frame;
  }

  void Paint(PaintContext& paint, const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);
    if (progress <= 0.0F || progress >= 1.0F) { return; }
    const auto tear = SampleTear(context, seed);
    const float trace = std::clamp(progress / 0.20F, 0.0F, 1.0F);
    const float glow = std::clamp(progress / 0.08F, 0.0F, 1.0F) * (1.0F - tear.opening);
    Path seam;
    seam.MoveTo(tear.seam.front());
    const float reached = trace * static_cast<float>(tear.seam.size() - 1);
    for (std::size_t index = 1; index < tear.seam.size(); ++index) {
      const float part = std::clamp(reached - static_cast<float>(index - 1), 0.0F, 1.0F);
      const Point from = tear.seam[index - 1];
      const Point to = tear.seam[index];
      seam.LineTo({from.x + (to.x - from.x) * part, from.y + (to.y - from.y) * part});
      if (part < 1.0F) { break; }
    }
    for (const Transform2D& transform : {tear.left_transform, tear.right_transform}) {
      paint.PushTransform(transform);
      paint.StrokePath(seam, Color::Rgb(95, 0, 18, 0.24F * glow),
                       StrokeStyle{.width = 24.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
      paint.StrokePath(seam, Color::Rgb(205, 8, 38, 0.45F * glow),
                       StrokeStyle{.width = 10.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
      paint.StrokePath(seam, Color::Rgb(255, 92, 80, 0.9F * glow),
                       StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
      paint.PopTransform();
    }
    // Fixed seeds and closed-form trajectories keep seeking, cancellation, and reversal deterministic.
    for (std::uint32_t index = 0; index < 12; ++index) {
      const float age = std::clamp((progress - 0.24F - 0.009F * static_cast<float>(index)) / 0.66F, 0.0F, 1.0F);
      if (age <= 0.0F || age >= 1.0F) { continue; }
      const Point source = tear.seam[1 + index % 11];
      const float direction = index % 2 == 0 ? -1.0F : 1.0F;
      const float speed = 0.14F + TearNoise(seed, index + 21) * 0.18F;
      const Point center{
          source.x + direction * context.bounds.width * speed * age,
          source.y - context.bounds.height * 0.10F * age + context.bounds.height * 0.24F * age * age,
      };
      const float radius = (6.0F + TearNoise(seed, index + 45) * 6.0F) * (1.0F - age * 0.35F);
      const float alpha = std::min(age / 0.08F, 1.0F) * (1.0F - std::pow(age, 3.0F));
      const float previous = std::max(0.0F, age - 0.08F);
      const Point tail{
          source.x + direction * context.bounds.width * speed * previous,
          source.y - context.bounds.height * 0.10F * previous + context.bounds.height * 0.24F * previous * previous,
      };
      paint.DrawLine(tail, center, Color::Rgb(244, 35, 43, alpha * 0.55F),
                     StrokeStyle{.width = radius * 0.65F, .cap = StrokeCap::Round});
      paint.DrawCircle(center, radius * 1.7F, Color::Rgb(170, 8, 30, alpha * 0.16F));
      const float angle = direction * age * 7.0F;
      Path shard;
      for (int corner = 0; corner < 3; ++corner) {
        const float radians = angle + static_cast<float>(corner) * std::numbers::pi_v<float> * 2.0F / 3.0F;
        const Point point{center.x + std::cos(radians) * radius, center.y + std::sin(radians) * radius};
        if (corner == 0) { shard.MoveTo(point); }
        else { shard.LineTo(point); }
      }
      shard.Close();
      paint.FillPath(shard, Color::Rgb(250, 68, 51, alpha));
      paint.StrokePath(shard, Color::Rgb(255, 164, 94, alpha * 0.9F), StrokeStyle{.width = 1.0F});
    }
  }

  bool operator==(const TearTransition&) const = default;
};

} // namespace

const EffectInfo& Describe(Effect effect) {
  for (const auto& item : effects) {
    if (item.effect == effect) {
      return item;
    }
  }
  throw std::invalid_argument("HuxerUI transition example effect is invalid");
}

huxerui::TransitionSpec MakeTransition(Effect effect, bool slow) {
  using namespace huxerui;
  const double duration = slow ? 1.8 : 0.65;
  const TweenSpec smooth{duration, CubicBezierCurve{0.22F, 0.0F, 0.12F, 1.0F}};
  switch (effect) {
  case Effect::Prism:
    return {PrismSweep{}, TweenSpec{slow ? 2.0 : 0.80, Easing::Linear}};
  case Effect::SplitGate:
    return {SplitGate{}, TweenSpec{duration, Easing::EaseInOut}};
  case Effect::Mosaic:
    return {CascadeMosaic{}, TweenSpec{slow ? 2.2 : 0.85, Easing::Linear}};
  case Effect::Iris:
    return {CenteredReveal{}, smooth};
  case Effect::IrisClose:
    return TransitionSpec{CenteredReveal{}, smooth}.Reversed();
  case Effect::Diagonal:
    return {DiagonalSweep{}, smooth};
  case Effect::Shutters:
    return {VenetianBlinds{}, smooth};
  case Effect::Orbit:
    return {OrbitAndSettle{}, smooth};
  case Effect::Curtain:
    return {CurtainLift{}, smooth};
  case Effect::Tear:
    return {TearTransition{}, TweenSpec{slow ? 2.2 : 0.78, Easing::EaseInOut}};
  }
  throw std::invalid_argument("HuxerUI transition example effect is invalid");
}

} // namespace transition_studio
