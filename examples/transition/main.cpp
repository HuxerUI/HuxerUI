#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

#include <huxerui/huxerui.h>

#include "effects.h"

namespace transition_studio {
namespace {

using namespace huxerui;

ThemeDefinition StudioTheme(bool daylight) {
  auto theme = daylight ? FlatLightThemeSpec() : FlatDarkThemeSpec();
  theme.colors.primary = daylight ? Color::Rgb(158, 62, 30) : Color::Rgb(187, 168, 255);
  theme.colors.on_primary = daylight ? Color::White() : Color::Rgb(24, 15, 48);
  theme.colors.background = daylight ? Color::Rgb(247, 241, 230) : Color::Rgb(12, 13, 22);
  theme.colors.surface = daylight ? Color::Rgb(255, 251, 243) : Color::Rgb(21, 23, 36);
  theme.colors.surface_container = daylight ? Color::Rgb(239, 230, 213) : Color::Rgb(31, 33, 49);
  theme.colors.on_surface = daylight ? Color::Rgb(44, 35, 28) : Color::Rgb(244, 240, 255);
  theme.colors.on_surface_variant = daylight ? Color::Rgb(113, 95, 77) : Color::Rgb(165, 165, 192);
  theme.colors.outline = daylight ? Color::Rgb(89, 67, 45, 0.17F) : Color::Rgb(218, 207, 255, 0.15F);
  theme.shapes.medium = 14.0F;
  theme.shapes.large = 24.0F;
  return FlatThemeDefinition(theme);
}

View Artwork(bool daylight, bool destination) {
  return Canvas([daylight, destination](PaintContext& paint, Size size) {
    const Rect bounds{0.0F, 0.0F, size.width, size.height};
    const Color base = daylight ? Color::Rgb(245, 175, 109) : Color::Rgb(57, 40, 104);
    const Color glow = daylight ? Color::Rgb(255, 240, 181, 0.7F) : Color::Rgb(182, 149, 255, 0.7F);
    paint.DrawRect(bounds, LinearGradient{
        .start = {0.0F, 0.0F}, .end = {1.0F, 1.0F},
        .stops = {{0.0F, base}, {1.0F, daylight ? Color::Rgb(185, 81, 79) : Color::Rgb(14, 26, 55)}},
    });
    const float unit = std::min(size.width, size.height);
    const Point center{size.width * (destination ? 0.66F : 0.57F), size.height * 0.39F};
    paint.DrawRect(bounds, RadialGradient{
        .center = {center.x / std::max(size.width, 1.0F), center.y / std::max(size.height, 1.0F)},
        .radius = {0.64F, 0.78F},
        .stops = {{0.0F, glow}, {1.0F, Color::Transparent()}},
    });
    for (int index = 0; index < 7; ++index) {
      const float radius = unit * (0.21F + static_cast<float>(index) * 0.075F);
      paint.DrawArc(center, radius, -0.4F, 1.8F * std::numbers::pi_v<float>, Color::Rgb(255, 255, 255, 0.22F),
                    StrokeStyle{.width = 1.0F});
    }
    paint.DrawCircle(center, unit * 0.19F, daylight ? Color::Rgb(255, 237, 188) : Color::Rgb(224, 209, 255));
    paint.DrawCircle({center.x + unit * 0.054F, center.y - unit * 0.045F}, unit * 0.163F, base);
    for (int index = 0; index < 28; ++index) {
      const float x = static_cast<float>((index * 73 + 29) % 293) / 293.0F * size.width;
      const float y = static_cast<float>((index * 47 + 17) % 179) / 179.0F * size.height;
      paint.DrawCircle({x, y}, index % 4 == 0 ? 2.2F : 1.0F, Color::Rgb(255, 255, 255, 0.65F));
    }
    Path ridge;
    ridge.MoveTo({0.0F, size.height * 0.72F})
        .CubicTo({size.width * 0.32F, size.height * 0.48F}, {size.width * 0.62F, size.height},
                 {size.width, size.height * 0.57F})
        .LineTo({size.width, size.height})
        .LineTo({0.0F, size.height})
        .Close();
    paint.FillPath(std::move(ridge), daylight ? Color::Rgb(127, 54, 63, 0.75F) : Color::Rgb(14, 17, 40, 0.82F));
  }).With(Semantics{.hidden = true});
}

View Poster(bool daylight, bool destination, bool compact) {
  return Stack {
    Artwork(daylight, destination),
    Column {
      Row {
        Text(daylight ? "SOLAR ARCHIVE" : "LUNAR ARCHIVE")
            .Style({Font::System(11.0F).WithWeight(FontWeight::Bold), Color::White()}),
        Spacer(),
        Text(destination ? "VOL. 02" : "VOL. 01").Style({Font::System(11.0F), Color::White()}),
      }.With(CrossAlign(CrossAxisAlignment::Center)),
      Spacer(),
      Text(destination ? "Another\nperspective." : "Between\nworlds.")
          .Style({Font::System(compact ? 38.0F : 56.0F).WithWeight(FontWeight::Bold), Color::White()}),
      Text(daylight ? "A study in warmth, light and possibility." : "An exploration of space, rhythm and motion.")
          .Style({Font::System(13.0F), Color::Rgb(255, 255, 255, 0.8F)}),
    }.With(Padding(compact ? 24.0F : 32.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(
      Frame{.height = compact ? 350.0F : 440.0F},
      CornerRadius(24.0F),
      ClipChildren(),
      Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch)
  );
}

View Surface(View content, const ThemeSpec& theme) {
  return Stack {std::move(content)}.With(
      Padding(24.0F),
      Background(theme.colors.surface),
      Border{.color = theme.colors.outline},
      CornerRadius(24.0F),
      Align(HorizontalAlignment::Stretch, VerticalAlignment::Start)
  );
}

View DetailPage(Effect effect, State<bool> slow, State<bool> daylight, int edition);

[[huxerui::composable]]
View Catalog(State<Effect> selected, State<bool> slow, State<bool> daylight) {
  const ThemeSpec& theme = UseTheme();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto navigation = UseNavigation();
  const auto scene = UseSceneTransition();
  const Effect effect = selected.Get();
  const auto& info = Describe(effect);

  Views choices;
  for (const auto& item : effects) {
    choices.Add(Chip(std::string(item.name), effect == item.effect)
        .OnChanged([selected, next = item.effect](bool checked) {
          if (checked) {
            selected = next;
          }
        })
        .Key(static_cast<int>(item.effect)));
  }

  View controls = Surface(Column {
    Text("CHOOSE YOUR MOTION").Style({Font::System(11.0F).WithWeight(FontWeight::Bold), theme.colors.primary}),
    Flow {std::move(choices)}.With(Spacing(8.0F)),
    Divider(),
    Text(std::string(info.name)).Style({Font::System(27.0F).WithWeight(FontWeight::Bold), theme.colors.on_surface}),
    Text(std::string(info.description)).Style({Font::System(14.0F), theme.colors.on_surface_variant}),
    Switch("Slow motion", slow).OnChanged([slow](bool next) { slow = next; }).Key("slow-motion"),
    Button("Enter next page").OnClick([navigation, selected, slow, daylight] {
      navigation.Push(DetailPage, selected.Get(), slow, daylight, 2);
    }).Key("enter-page"),
    Button("Transform scene").OnClick([scene, selected, slow, daylight] {
      scene.RunFromCurrentInteraction(MakeTransition(selected.Get(), slow.Get()), [daylight] {
        daylight = !daylight.Get();
      });
    }).Key("transform-scene"),
    Text("Enter a page, reverse back, or transform this whole scene. Try the circular reveal from a different touch point.")
        .Style({Font::System(12.0F), theme.colors.on_surface_variant}),
  }.With(Spacing(18.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);

  View preview = Poster(daylight.Get(), false, compact);
  View stage;
  if (compact) {
    stage = Column {
      std::move(controls),
      std::move(preview),
    }.With(Spacing(20.0F), CrossAlign(CrossAxisAlignment::Stretch));
  } else {
    stage = Row {
      std::move(controls).With(Frame{.width = 340.0F}),
      std::move(preview).With(Grow()),
    }.With(Spacing(24.0F), CrossAlign(CrossAxisAlignment::Start));
  }

  return ScrollView {
    Column {
      Text("HUXERUI / TRANSITION STUDIO")
          .Style({Font::System(12.0F).WithWeight(FontWeight::Bold), theme.colors.primary}),
      Text("Make the change\npart of the experience.")
          .Style({Font::System(compact ? 34.0F : 48.0F).WithWeight(FontWeight::Bold), theme.colors.on_surface}),
      Text::Format("{} ways to move between worlds. One place to try them.", effects.size())
          .Style({Font::System(15.0F), theme.colors.on_surface_variant}),
      std::move(stage),
      Text("BUILT-IN EFFECTS  /  CUSTOM GEOMETRY  /  REVERSIBLE MOTION")
          .Style({Font::System(10.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface_variant}),
    }.With(
        Padding(compact ? 20.0F : 36.0F),
        Spacing(20.0F),
        Frame{.max_width = 1200.0F},
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(Background(theme.colors.background), ScrollBar()).Key("catalog");
}

[[huxerui::composable]]
View DetailPage(Effect effect, State<bool> slow, State<bool> daylight, int edition) {
  const ThemeSpec& theme = UseTheme();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto navigation = UseNavigation();
  const auto scene = UseSceneTransition();
  const auto& info = Describe(effect);
  const auto spec = MakeTransition(effect, slow.Get());
  const Effect next = effects[(static_cast<std::size_t>(effect) + 1) % effects.size()].effect;

  return ScrollView {
    Column {
      Text::Format("TRANSITION STUDIO / EDITION {}", edition)
          .Style({Font::System(12.0F).WithWeight(FontWeight::Bold), theme.colors.primary}),
      Text(std::string(info.name)).Style({Font::System(compact ? 36.0F : 52.0F).WithWeight(FontWeight::Bold),
                                         theme.colors.on_surface}),
      Text(std::string(info.description)).Style({Font::System(15.0F), theme.colors.on_surface_variant}),
      Flow {
        Button("Back / reverse").OnClick([navigation] { navigation.Pop(); }).Key("back"),
        Button("Try next effect").OnClick([navigation, next, slow, daylight, edition] {
          navigation.Replace(DetailPage, next, slow, daylight, edition + 1);
        }).Key("replace-page"),
        Button("Transform scene").OnClick([scene, effect, slow, daylight] {
          scene.RunFromCurrentInteraction(MakeTransition(effect, slow.Get()), [daylight] {
            daylight = !daylight.Get();
          });
        }).Key("transform-scene"),
      }.With(Spacing(10.0F)),
      Poster(daylight.Get(), true, compact),
      Row {
        Text(std::string(info.category))
            .Style({Font::System(11.0F).WithWeight(FontWeight::Bold), theme.colors.primary}),
        Spacer(),
        Text(slow.Get() ? "SLOW MOTION" : "FULL SPEED")
            .Style({Font::System(11.0F), theme.colors.on_surface_variant}),
      }.With(CrossAlign(CrossAxisAlignment::Center)),
      Text("Back plays this effect in reverse. Try next effect replaces the page without growing the navigation stack.")
          .Style({Font::System(13.0F), theme.colors.on_surface_variant}),
    }.With(
        Padding(compact ? 20.0F : 36.0F),
        Spacing(18.0F),
        Frame{.max_width = 1200.0F},
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(
      Background(theme.colors.background),
      ScrollBar(),
      PageTransition{.push = spec, .pop = spec.Reversed(), .replace = spec}
  ).Key("detail");
}

View App() {
  const auto selected = UseState(Effect::Tear);
  const auto slow = UseState(false);
  const auto daylight = UseState(false);
  return Theme(StudioTheme(daylight.Get()), NavigationStack([selected, slow, daylight] {
    return Catalog(selected, slow, daylight);
  }));
}

} // namespace
} // namespace transition_studio

const huxerui::Application application{
    transition_studio::App,
    {
        .window = {
            .title = "HuxerUI Transition Studio",
            .initial_size = {1160.0F, 880.0F},
            .minimum_size = huxerui::Size{360.0F, 560.0F},
            .content_mode = huxerui::WindowContentMode::SafeArea,
        },
        .show_debug_overlay = false,
    }
};
