#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>

using namespace huxerui;

namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr float circle_kappa = 0.55228475F;

Path CirclePath(Point center, float radius) {
  const float control = radius * circle_kappa;
  return Path()
      .MoveTo({center.x, center.y - radius})
      .CubicTo(
          {center.x + control, center.y - radius},
          {center.x + radius, center.y - control},
          {center.x + radius, center.y}
      )
      .CubicTo(
          {center.x + radius, center.y + control},
          {center.x + control, center.y + radius},
          {center.x, center.y + radius}
      )
      .CubicTo(
          {center.x - control, center.y + radius},
          {center.x - radius, center.y + control},
          {center.x - radius, center.y}
      )
      .CubicTo(
          {center.x - radius, center.y - control},
          {center.x - control, center.y - radius},
          {center.x, center.y - radius}
      )
      .Close();
}

Path LeftSemicirclePath(Point center, float radius) {
  const float control = radius * circle_kappa;
  return Path()
      .MoveTo({center.x, center.y - radius})
      .CubicTo(
          {center.x - control, center.y - radius},
          {center.x - radius, center.y - control},
          {center.x - radius, center.y}
      )
      .CubicTo(
          {center.x - radius, center.y + control},
          {center.x - control, center.y + radius},
          {center.x, center.y + radius}
      )
      .Close();
}

struct PhaseDriver;

class PhaseDriverExtension final : public NodeExtension {
public:
  PhaseDriverExtension(MountedNode& node, const PhaseDriver& modifier);

  void Update(MountedNode& node, const PhaseDriver& modifier);
  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override;

private:
  void AdvancePhase(double timestamp);

  State<std::uint64_t> step_;
  std::array<double, 3> durations_{1.0, 1.0, 1.0};
  bool enabled_ = false;
  std::optional<double> next_phase_at_;
};

struct PhaseDriver {
  using Extension = PhaseDriverExtension;

  State<std::uint64_t> step;
  std::array<double, 3> durations{1.0, 1.0, 1.0};
  bool enabled = true;
};

PhaseDriverExtension::PhaseDriverExtension(MountedNode& node, const PhaseDriver& modifier) {
  Update(node, modifier);
}

void PhaseDriverExtension::Update(MountedNode& node, const PhaseDriver& modifier) {
  static_cast<void>(node);
  if (enabled_ != modifier.enabled || durations_ != modifier.durations) {
    next_phase_at_.reset();
  }
  step_ = modifier.step;
  durations_ = modifier.durations;
  enabled_ = modifier.enabled;
}

void PhaseDriverExtension::AdvancePhase(double timestamp) {
  const std::uint64_t next_step = step_.Get() + 1;
  step_ = next_step;
  next_phase_at_ = timestamp + durations_[(next_step - 1) % durations_.size()];
}

NodeExtension::FrameResult PhaseDriverExtension::OnFrame(MountedNode& node, const FrameInfo& frame) {
  static_cast<void>(node);
  if (!enabled_ || !step_.IsValid()) {
    next_phase_at_.reset();
    return {};
  }

  if (!next_phase_at_.has_value()) {
    AdvancePhase(frame.timestamp);
    return {
        .needs_frame = true,
        .wake_after = *next_phase_at_ - frame.timestamp,
    };
  }

  const double remaining = *next_phase_at_ - frame.timestamp;
  if (remaining > 0.0) {
    return {
        .needs_frame = false,
        .wake_after = remaining,
    };
  }

  AdvancePhase(frame.timestamp);
  return {
      .needs_frame = true,
      .wake_after = *next_phase_at_ - frame.timestamp,
  };
}

void PaintTaiji(PaintContext& paint, Size size, const ColorScheme& colors) {
  const float extent = std::min(size.width, size.height);
  if (extent <= 0.0F) {
    return;
  }
  const Point center{size.width * 0.5F, size.height * 0.5F};
  const float radius = extent * 0.34F;
  const float half_radius = radius * 0.5F;
  const Path outline = CirclePath(center, radius);

  paint.DrawPathShadow(outline, Color::Rgb(0, 0, 0, 0.26F), {}, radius * 0.12F);
  paint.FillPath(outline, colors.surface_container_highest);
  paint.PushPathClip(outline);
  paint.FillPath(LeftSemicirclePath(center, radius), colors.on_surface);
  paint.DrawCircle({center.x, center.y - half_radius}, half_radius, colors.on_surface);
  paint.DrawCircle({center.x, center.y + half_radius}, half_radius, colors.surface_container_highest);
  paint.DrawCircle({center.x, center.y - half_radius}, radius * 0.105F, colors.surface_container_highest);
  paint.DrawCircle({center.x, center.y + half_radius}, radius * 0.105F, colors.on_surface);
  paint.PopClip();
  paint.StrokePath(outline, colors.primary, std::max(2.0F, radius * 0.025F), StrokeCap::Round, StrokeJoin::Round);
}

void PaintOrbit(PaintContext& paint, Size size, const ColorScheme& colors) {
  const float extent = std::min(size.width, size.height);
  if (extent <= 0.0F) {
    return;
  }
  const Point center{size.width * 0.5F, size.height * 0.5F};
  const float radius = extent * 0.35F;
  const Path field = CirclePath(center, radius);

  paint.DrawPathShadow(field, Color::Rgb(0, 0, 0, 0.24F), {}, radius * 0.12F);
  paint.FillPath(field, colors.inverse_surface);
  paint.DrawArc(center, radius * 0.70F, -pi * 0.10F, pi * 1.28F, colors.primary, radius * 0.055F, StrokeCap::Round);
  paint.DrawArc(
      center,
      radius * 0.46F,
      pi * 0.80F,
      pi * 1.12F,
      colors.secondary_container,
      radius * 0.045F,
      StrokeCap::Round
  );
  paint.DrawArc(
      center,
      radius * 0.86F,
      pi * 0.58F,
      pi * 0.58F,
      colors.inverse_on_surface,
      radius * 0.025F,
      StrokeCap::Round
  );
  paint.DrawCircle(center, radius * 0.14F, colors.primary);
  paint.DrawCircle(
      {center.x + std::cos(-pi * 0.10F) * radius * 0.70F, center.y + std::sin(-pi * 0.10F) * radius * 0.70F},
      radius * 0.09F,
      colors.on_primary
  );
  paint.DrawCircle(
      {center.x + std::cos(pi * 0.80F) * radius * 0.46F, center.y + std::sin(pi * 0.80F) * radius * 0.46F},
      radius * 0.065F,
      colors.secondary_container
  );
  paint.DrawCircle(
      {center.x + std::cos(pi * 0.58F) * radius * 0.86F, center.y + std::sin(pi * 0.58F) * radius * 0.86F},
      radius * 0.045F,
      colors.inverse_on_surface
  );
}

void PaintPathStudy(PaintContext& paint, Size size, const ColorScheme& colors) {
  const float extent = std::min(size.width, size.height);
  if (extent <= 0.0F) {
    return;
  }
  const float inset = extent * 0.10F;
  Path shape;
  shape.MoveTo({inset, size.height * 0.65F})
      .CubicTo(
          {size.width * 0.20F, -8.0F},
          {size.width * 0.72F, size.height + 8.0F},
          {size.width - inset, size.height * 0.35F}
      )
      .LineTo({size.width - inset, size.height - inset})
      .LineTo({inset, size.height - inset})
      .Close();

  paint.DrawPathShadow(shape, Color::Rgb(0, 0, 0, 0.24F), {}, 18.0F);
  paint.FillPath(shape, colors.primary);
  paint.StrokePath(shape, colors.on_primary, 3.0F, StrokeCap::Round, StrokeJoin::Round);

  Path highlight;
  highlight.MoveTo({size.width * 0.22F, size.height * 0.52F})
      .QuadraticTo({size.width * 0.50F, size.height * 0.18F}, {size.width * 0.78F, size.height * 0.48F});
  paint.PushPathClip(shape);
  paint.StrokePath(highlight, Color::Rgb(255, 255, 255, 0.60F), 8.0F, StrokeCap::Round, StrokeJoin::Round);
  paint.PopClip();
}

constexpr double taiji_acceleration_duration = 3.0;
constexpr double taiji_cruise_duration = 2.5;
constexpr double taiji_deceleration_duration = 3.0;
constexpr float taiji_acceleration_rotation = 1440.0F;
constexpr float taiji_cruise_rotation = 3600.0F;
constexpr float taiji_cycle_rotation = 6480.0F;

struct TaijiMotion {
  float target = 0.0F;
  AnimationSpec animation = SnapSpec{};
};

TaijiMotion ResolveTaijiMotion(std::uint64_t step) {
  if (step == 0) {
    return {};
  }

  const std::uint64_t completed_cycles = (step - 1) / 3;
  const std::uint64_t phase = (step - 1) % 3;
  const float cycle_start = static_cast<float>(completed_cycles) * taiji_cycle_rotation;
  if (phase == 0) {
    return {
        cycle_start + taiji_acceleration_rotation,
        TweenSpec{.duration = taiji_acceleration_duration, .easing = Easing::EaseIn},
    };
  }
  if (phase == 1) {
    return {
        cycle_start + taiji_acceleration_rotation + taiji_cruise_rotation,
        TweenSpec{.duration = taiji_cruise_duration, .easing = Easing::Linear},
    };
  }
  return {
      cycle_start + taiji_cycle_rotation,
      TweenSpec{.duration = taiji_deceleration_duration, .easing = Easing::EaseOut},
  };
}

[[huxerui::scope]]
View TaijiEffect() {
  const ThemeSpec& theme = UseTheme();
  auto step = UseState(std::uint64_t{0});
  const bool animated = !theme.motion.reduced_motion;
  const ColorScheme colors = theme.colors;
  const TaijiMotion motion = ResolveTaijiMotion(step.Get());

  return Canvas([colors](PaintContext& paint, Size size) { PaintTaiji(paint, size, colors); })
      .With(
          Grow(),
          Rotation(Animated<float>{motion.target, motion.animation}),
          PhaseDriver{
              .step = step,
              .durations = {
                  taiji_acceleration_duration,
                  taiji_cruise_duration,
                  taiji_deceleration_duration,
              },
              .enabled = animated,
          }
      );
}

[[huxerui::scope]]
View OrbitEffect() {
  const ThemeSpec& theme = UseTheme();
  auto cycle = UseState(std::uint64_t{0});
  const bool animated = !theme.motion.reduced_motion;
  const ColorScheme colors = theme.colors;
  const float target_rotation = static_cast<float>(cycle.Get()) * -120.0F;
  const bool pulse = cycle.Get() % 2 != 0;

  return Canvas([colors](PaintContext& paint, Size size) { PaintOrbit(paint, size, colors); })
      .With(
          Grow(),
          Rotation(AnimateTo(target_rotation, SpringSpec{.stiffness = 30.0F, .damping_ratio = 0.76F})),
          Scale(AnimateTo(
              animated ? (pulse ? 1.035F : 0.965F) : 1.0F,
              TweenSpec{.duration = 0.9, .easing = Easing::EaseOut}
          )),
          PhaseDriver{
              .step = cycle,
              .durations = {1.05, 1.05, 1.05},
              .enabled = animated,
          }
      );
}

View PathEffect() {
  const ColorScheme colors = UseTheme().colors;
  return Canvas([colors](PaintContext& paint, Size size) { PaintPathStudy(paint, size, colors); }).With(Grow());
}

View SelectedEffect(std::size_t selected) {
  switch (selected) {
  case 0:
    return TaijiEffect();
  case 1:
    return OrbitEffect();
  case 2:
    return PathEffect();
  default:
    return {};
  }
}

} // namespace

[[huxerui::scope]]
View CanvasDemo() {
  const ThemeSpec& theme = UseTheme();
  auto selected = UseState<std::size_t>(0);
  const std::size_t selected_index = selected.Get();

  return Column {
    Text("Canvas Effects", TextRole::Title),
    Text("Retained transforms animate reusable Canvas paint sequences."),
    Tabs({"Taiji", "Orbit", "Path"}, selected)
        .OnChanged([selected](std::size_t index) { selected = index; }),
    Stack {
      SelectedEffect(selected_index).Key(selected_index),
    }.With(
        Grow(),
        Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
        Padding(theme.spacing.medium),
        Background(theme.colors.surface_container_low),
        CornerRadius(theme.shapes.extra_large),
        Shadow{Color::Rgb(0, 0, 0, 0.16F), {}, theme.elevation.low}
    ),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View App() {
  return MaterialTheme(CanvasDemo);
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Canvas",
        .width = 640.0F,
        .height = 560.0F,
    }
)
