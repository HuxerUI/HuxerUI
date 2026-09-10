#include <array>
#include <string>
#include <utility>

#include <huxerui/huxerui.h>

namespace {

using namespace huxerui;

struct Story {
  const char* key;
  const char* title;
  const char* summary;
  const char* description;
  Color color;
};

const std::array<Story, 2> stories{{
    {"horizons", "Quiet horizons", "A field guide to slowing down.",
     "Follow the coastline, leave the clock behind, and find a little room to breathe.", Color::Rgb(47, 107, 102)},
    {"rain", "After the rain", "Small moments, closer to home.",
     "A walk through familiar streets, soft reflections, and the colors we usually pass by.", Color::Rgb(157, 81, 59)},
}};

ThemeDefinition ReadingTheme() {
  auto theme = FlatLightThemeSpec();
  theme.colors.primary = Color::Rgb(47, 107, 102);
  theme.colors.background = Color::Rgb(245, 242, 235);
  theme.colors.surface = Color::Rgb(255, 253, 248);
  theme.colors.on_surface = Color::Rgb(37, 47, 45);
  theme.colors.on_surface_variant = Color::Rgb(104, 113, 105);
  return FlatThemeDefinition(theme);
}

TweenSpec Timing(bool slow) { return {slow ? 2.4 : 0.7, Easing::EaseInOut}; }

struct ArcBounds {
  float lift = 20.0F;
  Rect Evaluate(Rect from, Rect to, float progress) const {
    return {
        from.x + (to.x - from.x) * progress,
        from.y + (to.y - from.y) * progress - lift * 4.0F * progress * (1.0F - progress),
        from.width + (to.width - from.width) * progress,
        from.height + (to.height - from.height) * progress,
    };
  }
  bool operator==(const ArcBounds&) const = default;
};

View Cover(const Story& story, float width) {
  return Canvas([color = story.color](PaintContext& paint, Size size) {
    paint.DrawRect({0.0F, 0.0F, size.width, size.height}, color, 8.0F);
    paint.PushClip({0.0F, 0.0F, size.width, size.height}, 8.0F);
    paint.DrawCircle({size.width * 0.68F, size.height * 0.3F}, size.width * 0.24F, Color::Rgb(245, 217, 160));
    Path hill;
    hill.MoveTo({0.0F, size.height * 0.6F})
        .CubicTo({size.width * 0.3F, size.height * 0.32F}, {size.width * 0.7F, size.height * 0.9F},
                 {size.width, size.height * 0.53F})
        .LineTo({size.width, size.height})
        .LineTo({0.0F, size.height})
        .Close();
    paint.FillPath(std::move(hill), Color::Rgb(18, 44, 46, 0.65F));
    paint.DrawRect({size.width * 0.15F, size.height * 0.82F, size.width * 0.4F, 2.0F}, Color::White());
    paint.PopClip();
  }).With(Frame{width, width * 1.25F}, Semantics{.label = std::string(story.title)});
}

View StoryTitle(const Story& story, const ThemeSpec& theme) {
  // Both pages use identical text styling; only its location changes.
  return Text(story.title)
      .Style({Font::System(20.0F).WithWeight(FontWeight::Bold), theme.colors.on_surface})
      .With(SharedElement(std::string(story.key) + "/title"));
}

View StoryDescription(const Story& story, bool detailed, const ThemeSpec& theme) {
  return Text(detailed ? story.description : story.summary)
      .Style({Font::System(detailed ? 15.0F : 13.0F), theme.colors.on_surface_variant})
      .With(SharedBounds(std::string(story.key) + "/description"));
}

View StoryPage(int index, State<bool> slow);

[[huxerui::composable]]
View Library(State<bool> slow) {
  const auto navigation = UseNavigation();
  const ThemeSpec& theme = UseTheme();
  Views cards;
  for (int index = 0; index < static_cast<int>(stories.size()); ++index) {
    const Story& story = stories[static_cast<std::size_t>(index)];
    cards.Add(Row {
      Cover(story, 64.0F).With(SharedElement(std::string(story.key) + "/cover")),
      Column {
        StoryTitle(story, theme),
        StoryDescription(story, false, theme),
        Text("Read story").Style({Font::System(12.0F), theme.colors.primary}),
      }.With(Spacing(8.0F), Grow(), CrossAlign(CrossAxisAlignment::Start)),
    }.With(
        Padding(16.0F), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Center),
        Background(theme.colors.surface), CornerRadius(16.0F)
    ).OnClick([navigation, index, slow] {
      navigation.Push(StoryPage, index, slow);
    }).Key(story.key));
  }
  return Column {
    Text("THE READING ROOM").Style({Font::System(11.0F).WithWeight(FontWeight::Bold), theme.colors.primary}),
    Text("Open a story. Follow its title and cover.")
        .Style({Font::System(14.0F), theme.colors.on_surface_variant}),
    Column {std::move(cards)}.With(Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    Spacer(),
  }.With(Padding(16.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View StoryPage(int index, State<bool> slow) {
  const auto navigation = UseNavigation();
  const ThemeSpec& theme = UseTheme();
  const Story& story = stories[static_cast<std::size_t>(index)];
  const TransitionSpec fade{FadeTransition{}, Timing(slow.Get())};
  return Column {
    Row {
      Button("Back").OnClick([navigation] { navigation.Pop(); }).Key("back"),
      Spacer(),
      Text("FIELD NOTES").Style({Font::System(11.0F), theme.colors.on_surface_variant}),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    StoryTitle(story, theme),
    Cover(story, 128.0F).With(SharedElement(std::string(story.key) + "/cover")),
    StoryDescription(story, true, theme),
    Text("Return to the shelf to watch the same journey in reverse.")
        .Style({Font::System(12.0F), theme.colors.on_surface_variant}),
    Spacer(),
  }.With(
      Padding(16.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Start),
      Background(theme.colors.background),
      PageTransition{.push = fade, .pop = fade.Reversed(), .replace = fade}
  );
}

[[huxerui::composable]]
View LocalCard(State<bool> slow) {
  const ThemeSpec& theme = UseTheme();
  const auto expanded = UseState(false);
  const auto transition = UseSharedTransition();
  const bool open = expanded.Get();
  const Story& story = stories.front();
  return Column {
    Text("STAY ON THIS PAGE").Style({Font::System(11.0F).WithWeight(FontWeight::Bold), theme.colors.primary}),
    Text("Expand the card, or tap again while it moves.")
        .Style({Font::System(14.0F), theme.colors.on_surface_variant}),
    // The control stays outside Scope so an in-flight operation can be interrupted.
    Button(open ? "Make compact" : "Expand card").OnClick([transition, expanded, slow] {
      transition.Run(Timing(slow.Get()), [expanded] { expanded = !expanded.Get(); });
    }).Key("expand-card"),
    Stack {
      Cover(story, open ? 112.0F : 64.0F).With(
          Offset{Point{open ? 152.0F : 16.0F, 24.0F}},
          SharedElement("local-cover").BoundsTransform(ArcBounds{})
      ),
      Column {
        Text(story.title)
            .Style({Font::System(open ? 22.0F : 16.0F).WithWeight(FontWeight::Bold), theme.colors.on_surface}),
        Text(open ? story.description : story.summary)
            .Style({Font::System(13.0F), theme.colors.on_surface_variant}),
      }.With(
          Frame{.width = open ? 248.0F : 160.0F},
          Offset{Point{open ? 16.0F : 100.0F, open ? 188.0F : 30.0F}},
          Spacing(8.0F), SharedBounds("local-description")
      ),
    }.With(
        Frame{.height = 290.0F}, Background(theme.colors.surface), CornerRadius(16.0F), transition.Scope()
    ),
    Spacer(),
  }.With(Padding(16.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View Studio() {
  const ThemeSpec& theme = UseTheme();
  const auto local = UseState(false);
  const auto slow = UseState(false);
  View content = local.Get() ? LocalCard(slow).Key("local") : NavigationStack([slow] {
    return Library(slow);
  }).Key("pages");
  return Column {
    Row {
      Text("Shared transitions").Style({Font::System(22.0F).WithWeight(FontWeight::Bold), theme.colors.on_surface}),
      Spacer(),
    },
    Flow {
      Chip("Between pages", !local.Get()).OnChanged([local](bool selected) { if (selected) { local = false; } }),
      Chip("On this page", local.Get()).OnChanged([local](bool selected) { if (selected) { local = true; } }),
      Switch("Slow motion", slow).OnChanged([slow](bool value) { slow = value; }),
    }.With(Spacing(8.0F)),
    Divider(),
    std::move(content).With(Grow()),
  }.With(
      Padding(16.0F), Spacing(10.0F), Frame{.max_width = 720.0F},
      CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background)
  );
}

View App() { return Theme(ReadingTheme(), Studio()); }

} // namespace

const huxerui::Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Shared Transitions",
            .initial_size = {720.0F, 760.0F},
            .minimum_size = huxerui::Size{360.0F, 640.0F},
            .content_mode = huxerui::WindowContentMode::SafeArea,
        },
        .show_debug_overlay = false,
    }
};
