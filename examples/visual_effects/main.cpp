#include <algorithm>
#include <numbers>

#include <huxerui/huxerui.h>
#include <visual_effects_resources.h>

#include "advanced_effects.h"
#include "spotlight_hover.h"

namespace visual_effects {

using namespace huxerui;

namespace {

ThemeDefinition EffectsTheme() {
  ThemeSpec theme = FlatDarkThemeSpec();
  theme.colors.primary = Color::Rgb(196, 172, 255);
  theme.colors.on_primary = Color::White();
  theme.colors.background = Color::Rgb(7, 8, 13);
  theme.colors.surface = Color::Rgb(17, 20, 31);
  theme.colors.surface_container = Color::Rgb(24, 28, 43);
  theme.colors.on_surface = Color::Rgb(246, 244, 255);
  theme.colors.on_surface_variant = Color::Rgb(173, 177, 197);
  theme.colors.outline = Color::Rgb(255, 255, 255, 0.12F);
  theme.shapes.medium = 14.0F;
  theme.shapes.large = 28.0F;
  theme.motion.fast = 0.18;
  theme.motion.normal = 0.36;
  theme.interactions.focus_ring = FocusRing{Color::Rgb(211, 194, 255), 2.0F, 3.0F};

  ThemeDefinition definition = FlatThemeDefinition(theme);
  const SpotlightHover spotlight;
  ButtonStyle button = ButtonStyle::Default();
  button.background = Color::Rgb(31, 41, 55);
  button.label_style = TextStyle{Font::System(15.0F).WithWeight(FontWeight::SemiBold), Color::White()};
  button.disabled_background = Color::Rgb(31, 41, 55, 0.45F);
  button.disabled_label = Color::Rgb(255, 255, 255, 0.4F);
  button.padding = EdgeInsets::Symmetric(26.0F, 13.0F);
  button.minimum_height = 52.0F;
  button.corner_radius = spotlight.corner_radius;
  button.indication = Indication{
      .press = IndicationLayer{.fill = Color::Rgb(255, 255, 255, 0.1F)},
  };
  definition.Set(button);
  return definition;
}

View AmbientCanvas(bool active) {
  return Canvas([active](PaintContext& paint, Size size) {
    const Rect bounds{0.0F, 0.0F, size.width, size.height};
    paint.DrawRect(bounds, Color::Rgb(7, 8, 13));
    paint.DrawRadialGradient(
        bounds,
        RadialGradient{
            .center = {0.18F, 0.16F},
            .radius = {0.58F, 0.72F},
            .stops = {
                {0.0F, active ? Color::Rgb(242, 112, 89, 0.32F) : Color::Rgb(121, 87, 255, 0.34F)},
                {1.0F, Color::Transparent()},
            },
        }
    );
    paint.DrawRadialGradient(
        bounds,
        RadialGradient{
            .center = {0.82F, 0.78F},
            .radius = {0.52F, 0.68F},
            .stops = {
                {0.0F, active ? Color::Rgb(173, 100, 255, 0.3F) : Color::Rgb(255, 105, 180, 0.24F)},
                {1.0F, Color::Transparent()},
            },
        }
    );

    const Point center{size.width * 0.5F, size.height * 0.5F};
    const float orbit = std::min(size.width, size.height) * 0.31F;
    paint.DrawArc(
        center,
        orbit,
        -0.25F * std::numbers::pi_v<float>,
        1.45F * std::numbers::pi_v<float>,
        Color::Rgb(255, 255, 255, 0.09F),
        StrokeStyle{.width = 1.0F, .cap = StrokeCap::Round, .dash_pattern = {8.0F, 12.0F}}
    );
    paint.DrawCircle(
        {center.x + orbit * 0.7F, center.y - orbit * 0.72F},
        active ? 6.0F : 4.0F,
        active ? Color::Rgb(255, 198, 125, 0.9F) : Color::Rgb(205, 186, 255, 0.82F)
    );
  }).With(Semantics{.hidden = true});
}

View EffectBadge(StringVariant label, Color accent) {
  return Row {
    Stack {}.With(Background(accent), CornerRadius(4.0F), Frame{.width = 8.0F, .height = 8.0F}),
    Text(std::move(label)).Style({Font::System(11.0F).WithWeight(FontWeight::SemiBold), Color::Rgb(213, 215, 228)}),
  }.With(
      Spacing(8.0F),
      Padding(EdgeInsets::Symmetric(12.0F, 7.0F)),
      Background(Color::Rgb(255, 255, 255, 0.055F)),
      Border{.color = Color::Rgb(255, 255, 255, 0.09F)},
      CornerRadius(18.0F),
      CrossAlign(CrossAxisAlignment::Center)
  );
}

View EffectCard(StringVariant index, StringVariant title, StringVariant description, Color accent, View preview) {
  return Column {
    Row {
      Text(std::move(index)).Style({Font::System(11.0F).WithWeight(FontWeight::Bold), accent}),
      Spacer(),
      Stack {}.With(Background(accent), CornerRadius(5.0F), Frame{.width = 10.0F, .height = 10.0F}),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Text(std::move(title)).Style({Font::System(24.0F).WithWeight(FontWeight::Bold), Color::Rgb(246, 244, 255)}),
    Text(std::move(description)).Style({Font::System(14.0F), Color::Rgb(173, 177, 197)}),
    Spacer(),
    std::move(preview),
  }.With(
      Spacing(14.0F),
      Padding(EdgeInsets::All(24.0F)),
      Frame{.min_height = 310.0F},
      Background(LinearGradient{
          .start = {0.0F, 0.0F},
          .end = {1.0F, 1.0F},
          .stops = {
              {0.0F, Color::Rgb(28, 31, 47, 0.94F)},
              {1.0F, Color::Rgb(14, 17, 27, 0.9F)},
          },
      }),
      Border{.color = Color::Rgb(255, 255, 255, 0.12F)},
      CornerRadius(24.0F),
      Shadow{.color = Color::Rgb(0, 0, 0, 0.34F), .blur_radius = 30.0F},
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View SpotlightPreview(State<bool> active, State<int> activation_count, SceneTransitionHandle scene) {
  return Column {
    Flow {
      EffectBadge(visual_effects::strings::badge_pointer, Color::Rgb(196, 172, 255)),
      EffectBadge(visual_effects::strings::badge_canvas, Color::Rgb(255, 126, 103)),
      EffectBadge(visual_effects::strings::badge_reveal, Color::Rgb(255, 190, 104)),
    }.With(Spacing(8.0F)),
    SpotlightButton(
        active.Get() ? StringVariant(visual_effects::strings::button_active)
                     : StringVariant(visual_effects::strings::button_idle)
    )
        .OnClick([scene, active, activation_count] {
          scene.RunFromCurrentInteraction(
              CircularRevealSceneTransition{.animation = TweenSpec{0.46, Easing::EaseInOut}},
              [active, activation_count] {
                active = !active.Get();
                activation_count += 1;
              }
          );
        })
        .With(Frame{.min_width = 240.0F}),
    Row {
      Stack {}.With(
          Background(active.Get() ? Color::Rgb(255, 190, 104) : Color::Rgb(151, 117, 255)),
          CornerRadius(5.0F),
          Frame{.width = 10.0F, .height = 10.0F},
          Shadow{
              .color = active.Get() ? Color::Rgb(255, 190, 104, 0.5F) : Color::Rgb(151, 117, 255, 0.45F),
              .blur_radius = 12.0F,
          }
      ),
      Text(
          active.Get() ? StringVariant(visual_effects::strings::status_active)
                       : StringVariant(visual_effects::strings::status_idle)
      ).Style({Font::System(13.0F).WithWeight(FontWeight::Medium), Color::Rgb(173, 177, 197)}),
      Spacer(),
      Text::Format(visual_effects::strings::activation_count, activation_count.Get())
          .Style({Font::System(12.0F), Color::Rgb(137, 142, 164)}),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View GradientPreview() {
  return GradientGlowSurface(
      Stack {
        Column {
          Text(visual_effects::strings::gradient_preview_title)
              .Style({Font::System(15.0F).WithWeight(FontWeight::Bold), Color::White()}),
          Text(visual_effects::strings::gradient_preview_detail)
              .Style({Font::System(12.0F), Color::Rgb(181, 186, 207)}),
        }.With(Spacing(5.0F)),
      }.With(
          Padding(EdgeInsets::All(20.0F)),
          Frame{.height = 132.0F},
          Background(Color::Rgb(16, 19, 31)),
          CornerRadius(20.0F),
          Align(HorizontalAlignment::Center, VerticalAlignment::Center)
      )
  );
}

View ShimmerPreview() {
  return ShimmerSurface(
      Row {
        Stack {}.With(
            Background(Color::Rgb(83, 88, 111)),
            CornerRadius(24.0F),
            Frame{.width = 48.0F, .height = 48.0F}
        ),
        Column {
          Stack {}.With(
              Background(Color::Rgb(78, 83, 106)),
              CornerRadius(6.0F),
              Frame{.height = 12.0F}
          ),
          Stack {}.With(
              Background(Color::Rgb(63, 68, 90)),
              CornerRadius(5.0F),
              Frame{.width = 150.0F, .height = 10.0F}
          ),
          Stack {}.With(
              Background(Color::Rgb(63, 68, 90)),
              CornerRadius(5.0F),
              Frame{.width = 104.0F, .height = 10.0F}
          ),
        }.With(Spacing(9.0F), Grow()),
      }.With(
          Spacing(16.0F),
          Padding(EdgeInsets::All(22.0F)),
          Frame{.height = 132.0F},
          Background(Color::Rgb(34, 38, 57)),
          CornerRadius(18.0F),
          CrossAlign(CrossAxisAlignment::Center),
          Semantics{.hidden = true}
      )
  );
}

View ParticlePreview() {
  return ParticleFieldSurface(
      Stack {
        Column {
          Text(visual_effects::strings::particle_preview_title)
              .Style({Font::System(15.0F).WithWeight(FontWeight::Bold), Color::White()}),
          Text(visual_effects::strings::particle_preview_detail)
              .Style({Font::System(12.0F), Color::Rgb(195, 201, 225)}),
        }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center)),
      }.With(
          Padding(EdgeInsets::All(20.0F)),
          Frame{.height = 150.0F},
          Background(LinearGradient{
              .start = {0.0F, 0.0F},
              .end = {1.0F, 1.0F},
              .stops = {
                  {0.0F, Color::Rgb(14, 17, 31)},
                  {1.0F, Color::Rgb(24, 29, 52)},
              },
          }),
          Border{.color = Color::Rgb(255, 255, 255, 0.12F)},
          CornerRadius(18.0F),
          Align(HorizontalAlignment::Center, VerticalAlignment::Center)
      )
  );
}

View OrbitPreview() {
  return Stack {
    OrbitLoader(),
  }.With(
      Frame{.height = 132.0F},
      Background(Color::Rgb(16, 20, 32)),
      Border{.color = Color::Rgb(255, 255, 255, 0.08F)},
      CornerRadius(18.0F),
      Align(HorizontalAlignment::Center, VerticalAlignment::Center)
  );
}

View AuroraPreview() {
  return LiquidAuroraSurface(
      Stack {
        Column {
          Text(visual_effects::strings::aurora_preview_title)
              .Style({Font::System(15.0F).WithWeight(FontWeight::Bold), Color::White()}),
          Text(visual_effects::strings::aurora_preview_detail)
              .Style({Font::System(12.0F), Color::Rgb(240, 239, 255)}),
        }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center)),
      }.With(
          Padding(EdgeInsets::All(20.0F)),
          Frame{.height = 150.0F},
          Background(Color::Rgb(14, 17, 30)),
          Border{.color = Color::Rgb(255, 255, 255, 0.1F)},
          CornerRadius(18.0F),
          Align(HorizontalAlignment::Center, VerticalAlignment::Center)
      )
  );
}

[[huxerui::composable]]
View EffectsStage(State<bool> active, State<int> activation_count) {
  const ViewportClass viewport = UseViewportClass();
  const SceneTransitionHandle scene = UseSceneTransition();
  const State<bool> entered = UseState(false);
  Lifecycle([entered] { entered = true; });

  const bool compact = viewport == ViewportClass::Compact;
  const float page_padding = compact ? 16.0F : 36.0F;
  const float study_spacing = compact ? 16.0F : 24.0F;

  View spotlight = EffectCard(
      visual_effects::strings::study_spotlight,
      active.Get() ? StringVariant(visual_effects::strings::spotlight_title_active)
                   : StringVariant(visual_effects::strings::spotlight_title_idle),
      visual_effects::strings::spotlight_description,
      Color::Rgb(196, 172, 255),
      SpotlightPreview(active, activation_count, scene)
  );
  View gradient = EffectCard(
      visual_effects::strings::study_gradient,
      visual_effects::strings::gradient_title,
      visual_effects::strings::gradient_description,
      Color::Rgb(244, 114, 255),
      GradientPreview()
  );
  View shimmer = EffectCard(
      visual_effects::strings::study_shimmer,
      visual_effects::strings::shimmer_title,
      visual_effects::strings::shimmer_description,
      Color::Rgb(80, 224, 255),
      ShimmerPreview()
  );
  View particles = EffectCard(
      visual_effects::strings::study_particles,
      visual_effects::strings::particle_title,
      visual_effects::strings::particle_description,
      Color::Rgb(255, 126, 103),
      ParticlePreview()
  );
  View orbit = EffectCard(
      visual_effects::strings::study_orbit,
      visual_effects::strings::orbit_title,
      visual_effects::strings::orbit_description,
      Color::Rgb(255, 190, 104),
      OrbitPreview()
  );
  View aurora = EffectCard(
      visual_effects::strings::study_aurora,
      visual_effects::strings::aurora_title,
      visual_effects::strings::aurora_description,
      Color::Rgb(112, 245, 217),
      AuroraPreview()
  );

  View studies;
  if (compact) {
    studies = Column {
      std::move(spotlight),
      std::move(gradient),
      std::move(shimmer),
      std::move(particles),
      std::move(orbit),
      std::move(aurora),
    }.With(Spacing(study_spacing), CrossAlign(CrossAxisAlignment::Stretch));
  } else {
    studies = Column {
      Row {
        std::move(spotlight).With(Grow()),
        std::move(gradient).With(Grow()),
      }.With(Spacing(study_spacing), CrossAlign(CrossAxisAlignment::Stretch)),
      Row {
        std::move(shimmer).With(Grow()),
        std::move(particles).With(Grow()),
      }.With(Spacing(study_spacing), CrossAlign(CrossAxisAlignment::Stretch)),
      Row {
        std::move(orbit).With(Grow()),
        std::move(aurora).With(Grow()),
      }.With(Spacing(study_spacing), CrossAlign(CrossAxisAlignment::Stretch)),
    }.With(Spacing(study_spacing), CrossAlign(CrossAxisAlignment::Stretch));
  }

  View content = Column {
    Column {
      Text(visual_effects::strings::page_eyebrow)
          .Style({Font::System(12.0F).WithWeight(FontWeight::Bold), Color::Rgb(198, 174, 255)}),
      Text(visual_effects::strings::page_title)
          .Style({Font::System(compact ? 34.0F : 48.0F).WithWeight(FontWeight::Bold), Color::Rgb(246, 244, 255)}),
      Text(visual_effects::strings::page_description)
          .Style({Font::System(compact ? 14.0F : 16.0F), Color::Rgb(173, 177, 197)})
          .With(Frame{.max_width = 760.0F}),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Start)),
    std::move(studies),
  }.With(
      Spacing(compact ? 24.0F : 34.0F),
      Frame{.max_width = 1100.0F},
      CrossAlign(CrossAxisAlignment::Stretch),
      Transition{AnimateTo(entered.Get() ? 1.0F : 0.0F, TweenSpec{0.5, Easing::EaseOut})}
          .Opacity(0.0F, 1.0F)
          .Offset({0.0F, 18.0F}, {})
  );

  return Stack {
    AmbientCanvas(active.Get()),
    ScrollView {
      Stack {std::move(content)}.With(
          Padding(EdgeInsets::All(page_padding)),
          Align(HorizontalAlignment::Center, VerticalAlignment::Start)
      ),
    }.With(ScrollBar()),
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch));
}

[[huxerui::composable]]
View ExampleApp() {
  const State<bool> active = UseState(false);
  const State<int> activation_count = UseState(0);
  return Theme(EffectsTheme(), EffectsStage(active, activation_count));
}

AppOptions BuildOptions() {
  AppOptions options;
  options.window.title = "HuxerUI Visual Effects";
  options.window.initial_size = {1120.0F, 820.0F};
  options.window.minimum_size = Size{360.0F, 560.0F};
  options.window.content_mode = WindowContentMode::SafeArea;
  options.window.chrome_mode = WindowChromeMode::System;
  options.show_debug_overlay = false;
  return options;
}

} // namespace
} // namespace visual_effects

const huxerui::Application application{visual_effects::ExampleApp, visual_effects::BuildOptions()};
