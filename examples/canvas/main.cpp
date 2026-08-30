#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
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
  paint.StrokePath(outline, colors.primary,
                   StrokeStyle{
                       .width = std::max(2.0F, radius * 0.025F),
                       .cap = StrokeCap::Round,
                       .join = StrokeJoin::Round,
                   });
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
  paint.DrawArc(center, radius * 0.70F, -pi * 0.10F, pi * 1.28F, colors.primary,
                StrokeStyle{.width = radius * 0.055F, .cap = StrokeCap::Round});
  paint.DrawArc(center, radius * 0.46F, pi * 0.80F, pi * 1.12F, colors.secondary_container,
                StrokeStyle{.width = radius * 0.045F, .cap = StrokeCap::Round});
  paint.DrawArc(
      center, radius * 0.86F, pi * 0.58F, pi * 0.58F, colors.inverse_on_surface,
      StrokeStyle{
          .width = radius * 0.025F,
          .cap = StrokeCap::Round,
          .dash_pattern = {radius * 0.08F, radius * 0.05F},
          .dash_offset = radius * 0.03F,
      });
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
  paint.StrokePath(shape, colors.on_primary,
                   StrokeStyle{
                       .width = 3.0F,
                       .cap = StrokeCap::Round,
                       .join = StrokeJoin::Round,
                       .dash_pattern = {12.0F, 7.0F},
                       .dash_offset = 3.0F,
                   });

  paint.DrawBorder(
      {inset * 0.5F, inset * 0.5F, size.width - inset, size.height - inset},
      Color::Rgb(255, 255, 255, 0.32F),
      StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {7.0F, 5.0F}},
      CornerRadii{12.0F});
  paint.DrawLine({inset, size.height * 0.18F}, {size.width - inset, size.height * 0.18F},
                 Color::Rgb(255, 255, 255, 0.54F),
                 StrokeStyle{.width = 4.0F, .cap = StrokeCap::Round, .dash_pattern = {0.0F, 10.0F}});

  Path highlight;
  highlight.MoveTo({size.width * 0.22F, size.height * 0.52F})
      .QuadraticTo({size.width * 0.50F, size.height * 0.18F}, {size.width * 0.78F, size.height * 0.48F});
  paint.PushPathClip(shape);
  paint.StrokePath(highlight, Color::Rgb(255, 255, 255, 0.60F),
                   StrokeStyle{.width = 8.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
  paint.PopClip();
}

constexpr double taiji_acceleration_duration = 3.0;
constexpr double taiji_cruise_duration = 2.5;
constexpr double taiji_deceleration_duration = 3.0;
constexpr double taiji_cycle_duration =
    taiji_acceleration_duration + taiji_cruise_duration + taiji_deceleration_duration;
constexpr float taiji_acceleration_rotation = 1440.0F;
constexpr float taiji_cruise_rotation = 3600.0F;
constexpr float taiji_cycle_rotation = 6480.0F;

KeyframeSpec TaijiAnimation() {
  return KeyframeSpec{
      taiji_cycle_duration,
      {
          {0.0F, 0.0F, Easing::EaseIn},
          {
              static_cast<float>(taiji_acceleration_duration / taiji_cycle_duration),
              taiji_acceleration_rotation / taiji_cycle_rotation,
              Easing::Linear,
          },
          {
              static_cast<float>(
                  (taiji_acceleration_duration + taiji_cruise_duration) / taiji_cycle_duration
              ),
              (taiji_acceleration_rotation + taiji_cruise_rotation) / taiji_cycle_rotation,
              Easing::EaseOut,
          },
          {1.0F, 1.0F, Easing::Linear},
      },
  };
}

[[huxerui::composable]]
View TaijiEffect() {
  const ThemeSpec& theme = UseTheme();
  auto started = UseState(false);
  Lifecycle([started] { started = true; });
  const bool animated = started.Get() && !theme.motion.reduced_motion;
  const ColorScheme colors = theme.colors;
  const Animated<float> rotation = animated
                                        ? AnimateTo(
                                              taiji_cycle_rotation,
                                              TaijiAnimation(),
                                              AnimationPlayback{.iterations = std::nullopt}
                                          )
                                        : AnimateTo(0.0F, SnapSpec{});

  return Canvas([colors](PaintContext& paint, Size size) { PaintTaiji(paint, size, colors); })
      .With(
          Grow(),
          Rotation(rotation)
      );
}

[[huxerui::composable]]
View OrbitEffect() {
  const ThemeSpec& theme = UseTheme();
  auto started = UseState(false);
  Lifecycle([started] { started = true; });
  const bool animated = started.Get() && !theme.motion.reduced_motion;
  const ColorScheme colors = theme.colors;
  const AnimationPlayback loop{.iterations = std::nullopt};
  const AnimationPlayback pulse{.iterations = std::nullopt, .repeat_mode = RepeatMode::Reverse};

  return Canvas([colors](PaintContext& paint, Size size) { PaintOrbit(paint, size, colors); })
      .With(
          Grow(),
          Rotation(
              animated ? AnimateTo(-360.0F, TweenSpec{3.15, Easing::Linear}, loop)
                       : AnimateTo(0.0F, SnapSpec{})
          ),
          Scale(
              animated ? AnimateTo(1.035F, TweenSpec{1.05, Easing::EaseInOut}, pulse)
                       : AnimateTo(1.0F, SnapSpec{})
          )
      );
}

[[huxerui::composable]]
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

[[huxerui::composable]]
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
  return MaterialTheme {
    CanvasDemo(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Canvas",
            .initial_size = {640.0F, 560.0F},
        },
    }
};
