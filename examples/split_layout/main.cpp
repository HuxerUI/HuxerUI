#include <huxerui/huxerui.h>

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace huxerui;

namespace {

std::array<Frame, 3> InitialFrames() {
  return {
      Frame{.width = 200.0F, .height = 140.0F, .min_width = 140.0F, .max_width = 360.0F,
            .min_height = 80.0F, .max_height = 280.0F},
      Frame{.min_width = 200.0F, .min_height = 160.0F},
      Frame{.width = 240.0F, .height = 160.0F, .min_width = 140.0F, .max_width = 400.0F,
            .min_height = 80.0F, .max_height = 320.0F},
  };
}

std::array<Grow, 3> InitialGrows() {
  return {Grow(0.0F), Grow(), Grow(0.0F)};
}

View PaneSurface(std::string title, View content, const ThemeSpec& theme) {
  return ScrollView(Column {
    Text(std::move(title), TextRole::Title),
    std::move(content),
  }.With(
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.medium),
      CrossAlign(CrossAxisAlignment::Stretch)
  )).With(Background(theme.colors.surface), CornerRadius(theme.shapes.medium));
}

[[huxerui::composable]]
View EditorPane() {
  const ThemeSpec& theme = UseTheme();
  auto edits = UseState<std::size_t>(0);
  return PaneSurface(
      "Editor",
      Column {
        Text("Drag either divider to resize the workspace. Text wraps as the available width changes."),
        Text::Format("Edits recorded: {}", edits.Get()),
        Button("Record edit").OnClick([edits] { edits += 1; }),
        Text("Hide and show the inspector: this pane keeps its local counter and scroll position.")
            .With(Foreground(theme.colors.on_surface_variant)),
      }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch)),
      theme
  );
}

[[huxerui::composable]]
View SplitDemo() {
  const ThemeSpec& theme = UseTheme();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  auto frames = UseState(InitialFrames());
  auto grows = UseState(InitialGrows());
  auto show_inspector = UseState(true);

  View inspector;
  if (show_inspector.Get()) {
    inspector = PaneSurface(
        "Inspector",
        Text("Pane sizes belong to the application. Reset sizes to restore the initial allocation."),
        theme
    ).With(frames.Get()[2], grows.Get()[2]).Key("inspector");
  }

  return Column {
    Text("SplitLayout", TextRole::Title),
    Text("Drag a divider, or Tab to it and use arrow keys. Escape cancels the current drag.")
        .With(Foreground(theme.colors.on_surface_variant)),
    Flow {
      Button(show_inspector.Get() ? "Hide inspector" : "Show inspector")
          .OnClick([show_inspector] { show_inspector = !show_inspector.Get(); }),
      Button("Reset sizes").OnClick([=] {
        frames = InitialFrames();
        grows = InitialGrows();
      }),
    }.With(Spacing(theme.spacing.small)),
    SplitLayout {
      PaneSurface(
          "Project",
          Column {
            Text("src/"),
            Text("  application.cpp"),
            Text("  workspace.cpp"),
            Text("resources/"),
          }.With(Spacing(theme.spacing.small)),
          theme
      ).With(frames.Get()[0], grows.Get()[0]).Key("project"),
      EditorPane().With(frames.Get()[1], grows.Get()[1]).Key("editor"),
      std::move(inspector),
    }.Orientation(compact ? Axis::Vertical : Axis::Horizontal)
        .OnChanged([=](const std::vector<SplitChange>& changes) {
          auto next_frames = frames.Get();
          auto next_grows = grows.Get();
          // The optional inspector is last, so visible pane indices keep the same array entries.
          for (const auto& change : changes) {
            next_frames[change.index] = change.frame;
            next_grows[change.index] = change.grow;
          }
          frames = std::move(next_frames);
          grows = std::move(next_grows);
        })
        .With(Grow()),
    Text(compact ? "Vertical panes on compact windows" : "Horizontal panes on wider windows")
        .With(Foreground(theme.colors.on_surface_variant)),
  }.With(
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface_container_low)
  );
}

} // namespace

View App() {
  return MaterialTheme {
    SplitDemo(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI SplitLayout",
            .initial_size = {1040.0F, 720.0F},
        },
    }
};
