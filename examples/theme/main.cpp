#include <huxerui/huxerui.h>

using namespace huxerui;

template <class Factory>
View AccentTheme(Factory &&content) {
  const ThemeSpec &theme = UseTheme();
  ThemeDefinition definition;
  definition.Set<ButtonStyleKey>(ButtonStyle{
      .background = theme.colors.error,
      .foreground = theme.colors.on_primary,
      .font_size = theme.typography.label,
      .padding = EdgeInsets::Symmetric(
          theme.spacing.medium,
          theme.spacing.small),
      .corner_radius = theme.shapes.large,
  });
  return Theme(
      std::move(definition),
      std::forward<Factory>(content));
}

[[huxerui::scope]]
View ThemeContent() {
  const auto theme = UseTheme();

  return Column{
        Text("Material Theme", TextRole::Title),
        Text("Text and controls resolve semantic values from "
             "the nearest Theme."),
        Button("Material button").OnClick([] {}),
        HUXERUI_THEME(
            AccentTheme,
            Button("Nested button style").OnClick([] {})),
        Button("Explicit modifier wins")
            .With(Background{Color::Rgb(88, 166, 255)})
            .OnClick([] {}),
        Button("Disabled button")
            .With(Enabled{false})
            .OnClick([] {}),
        HUXERUI_THEME(
            MaterialDarkTheme,
            Column{
                Text("Nested Material dark theme", TextRole::Title),
                Text("A complete nested theme replaces every token."),
                Button("Dark theme button").OnClick([] {}),
            }.With(
                Padding{UseTheme().spacing.medium},
                Spacing{UseTheme().spacing.small},
                Background{UseTheme().colors.background}
            )),
        HUXERUI_THEME(
            FlatTheme,
            Column{
                Text("Nested Flat theme", TextRole::Title),
                Button("Flat theme button").OnClick([] {}),
            }.With(
                Padding{UseTheme().spacing.medium},
                Spacing{UseTheme().spacing.small},
                Background{UseTheme().colors.background}
            )),
    }.With(
        Padding{theme.spacing.extra_large},
        Spacing{theme.spacing.medium},
        Background{theme.colors.background}
  );
}

auto App() {
  return HUXERUI_THEME(MaterialTheme, ThemeContent());
}

int main() {
  return RunApp(
      App,
      {
          .title = "HuxerUI Theme",
          .width = 560.0F,
          .height = 560.0F,
      });
}
