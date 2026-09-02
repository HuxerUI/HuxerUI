#include <huxerui/huxerui.h>

#include <cstddef>
#include <string>
#include <utility>

using namespace huxerui;

namespace {

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

View PageDot(bool selected, const ThemeSpec& theme) {
  return Column {}.With(
      Frame{.width = selected ? 28.0F : 8.0F, .height = 8.0F},
      Background(selected ? theme.colors.primary : theme.colors.surface_container_highest),
      CornerRadius(theme.shapes.full)
  );
}

View PageSurface(std::string eyebrow, std::string title, std::string description, View content, Color accent,
                 const ThemeSpec& theme) {
  return Column {
    Row {
      Text(std::move(eyebrow), TextRole::Label).With(Foreground(accent)),
      Spacer(),
      Text("MOUNTED").With(Foreground(theme.colors.on_surface_variant)),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Text(std::move(title), TextRole::Title),
    Text(std::move(description)).With(Foreground(theme.colors.on_surface_variant)),
    Spacer(),
    std::move(content),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.small),
      Background(WithAlpha(accent, 0.10F)),
      Border{WithAlpha(accent, 0.24F), 1.0F},
      CornerRadius(theme.shapes.extra_large),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

[[huxerui::composable]]
View DiscoverPage() {
  const ThemeSpec& theme = UseTheme();
  auto saved = UseState<std::size_t>(0);
  return PageSurface(
      "DISCOVER",
      "Retained page state",
      "Increment the counter, visit another page, and return. The mounted page keeps its local State.",
      Row {
        Text::Format(TextRole::Title, "{} saved", saved.Get()),
        Spacer(),
        Button("Save one").OnClick([saved] { saved += 1; }),
      }.With(CrossAlign(CrossAxisAlignment::Center)),
      theme.colors.primary,
      theme
  );
}

[[huxerui::composable]]
View TunePage() {
  const ThemeSpec& theme = UseTheme();
  auto intensity = UseState(0.62F);
  return PageSurface(
      "TUNE",
      "Interactive content",
      "Child controls retain their own interaction while the surrounding surface participates in paging.",
      Column {
        Row {
          Text("Intensity"),
          Spacer(),
          Text::Format("{}%", static_cast<int>(intensity.Get() * 100.0F)),
        }.With(CrossAlign(CrossAxisAlignment::Center)),
        Slider(intensity).OnChanged([intensity](float value) { intensity = value; }),
      }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch)),
      theme.colors.secondary,
      theme
  );
}

[[huxerui::composable]]
View ComposePage() {
  const ThemeSpec& theme = UseTheme();
  auto note = UseState(TextEditingValue::FromText("Pager keeps this draft"));
  return PageSurface(
      "COMPOSE",
      "Editing survives paging",
      "Text editing value, selection, and mounted input identity remain owned by the retained page.",
      TextField(note)
          .Label("Draft note")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([note](const TextEditingValue& value) { note = value; }),
      theme.colors.error,
      theme
  );
}

[[huxerui::composable]]
View PagerDemo() {
  const ThemeSpec& theme = UseTheme();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  auto selected = UseState<std::size_t>(0);
  auto axis = UseState<std::size_t>(0);
  auto direct_drag = UseState(true);
  auto reverse = UseState(false);
  auto accept_requests = UseState(true);
  auto status = UseState(std::string{"Waiting for a page request"});

  const auto select_programmatically = [selected, status](std::size_t index) {
    selected = index;
    status = "Programmatic selection committed page " + std::to_string(index + 1);
  };

  return ScrollView(
      Column {
        Column {
          Text("Pager", TextRole::Title),
          Text("Controlled, animated, and gesture-driven paging across retained content.")
              .With(Foreground(theme.colors.on_surface_variant)),
        }.With(Spacing(theme.spacing.extra_small), CrossAlign(CrossAxisAlignment::Start)),
        Column {
          SegmentedButton({"Horizontal", "Vertical"}, axis)
              .OnChanged([axis](std::size_t index) { axis = index; }),
          Flow {
            Switch("Direct drag", direct_drag)
                .OnChanged([direct_drag](bool enabled) { direct_drag = enabled; }),
            Switch("Reverse", reverse).OnChanged([reverse](bool enabled) { reverse = enabled; }),
            Switch("Accept requests", accept_requests)
                .OnChanged([accept_requests](bool enabled) { accept_requests = enabled; }),
          }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
        }.With(
            Padding(theme.spacing.medium),
            Spacing(theme.spacing.medium),
            Background(theme.colors.surface),
            Border{WithAlpha(theme.colors.outline, 0.18F), 1.0F},
            CornerRadius(theme.shapes.large),
            CrossAlign(CrossAxisAlignment::Stretch)
        ),
        Pager(
            {
                DiscoverPage(),
                TunePage(),
                ComposePage(),
            },
            selected
        )
            .ScrollAxis(axis.Get() == 0 ? Axis::Horizontal : Axis::Vertical)
            .Reverse(reverse.Get())
            .DragEnabled(direct_drag.Get())
            .OnChanged([=](std::size_t index) {
              status = "Direct interaction requested page " + std::to_string(index + 1);
              if (accept_requests.Get()) {
                selected = index;
              }
            })
            .With(Frame{.height = compact ? 360.0F : 320.0F}),
        Row {
          Row {
            PageDot(selected.Get() == 0, theme),
            PageDot(selected.Get() == 1, theme),
            PageDot(selected.Get() == 2, theme),
          }.With(Spacing(theme.spacing.extra_small), CrossAlign(CrossAxisAlignment::Center)),
          Spacer(),
          Text::Format(TextRole::Label, "Page {} of 3", selected.Get() + 1),
        }.With(CrossAlign(CrossAxisAlignment::Center)),
        Flow {
          Button("Previous")
              .OnClick([=] { select_programmatically(selected.Get() - 1); })
              .With(Enabled(selected.Get() > 0)),
          Button("Next")
              .OnClick([=] { select_programmatically(selected.Get() + 1); })
              .With(Enabled(selected.Get() < 2)),
          Button("Jump to first").OnClick([=] { select_programmatically(0); }),
        }.With(Spacing(theme.spacing.small)),
        Text(status.Get()).With(Foreground(theme.colors.on_surface_variant)),
      }.With(
          Padding(compact ? theme.spacing.medium : theme.spacing.extra_large),
          Spacing(theme.spacing.large),
          CrossAlign(CrossAxisAlignment::Stretch)
      )
  ).With(Background(theme.colors.surface_container_low));
}

} // namespace

View App() {
  return MaterialTheme {
    PagerDemo(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Pager",
            .initial_size = {820.0F, 820.0F},
        },
    }
};
