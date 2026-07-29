#include <huxerui/huxerui.h>

using namespace huxerui;

constexpr Color explicit_button_color = Color::Rgb(88, 166, 255);

template <class Factory> View AccentTheme(Factory&& content) {
  const ThemeSpec& theme = UseTheme();
  ThemeDefinition definition;
  definition.Set<ButtonStyleKey>(ButtonStyle {
      .background = theme.colors.error,
      .foreground = theme.colors.on_primary,
      .font_size = theme.typography.label,
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .corner_radius = theme.shapes.large,
  });
  return Theme(std::move(definition), std::forward<Factory>(content));
}

[[huxerui::scope]]
View ThemeContent() {
  const auto theme = UseTheme();
  auto checkbox_checked = UseState(true);
  auto switch_checked = UseState(false);
  auto progress = UseState(0.35F);

  return Column {
    Text("Material Theme", TextRole::Title),
    Text(
        "Text and controls resolve semantic values from "
        "the nearest Theme."
    ),
    Button("Material button").OnClick([] {}),
    Row {
        Checkbox(checkbox_checked).OnChanged([checkbox_checked](bool checked) { checkbox_checked = checked; }),
        Text::Format("Checkbox: {}", checkbox_checked ? "checked" : "unchecked"),
    }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
    Row {
        Switch(switch_checked).OnChanged([switch_checked](bool checked) { switch_checked = checked; }),
        Text::Format("Switch: {}", switch_checked ? "on" : "off"),
    }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
    Row {
        ProgressCircle(),
        Text("Indeterminate progress"),
    }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
    Row {
        ProgressCircle(progress),
        Text::Format("Progress: {}", progress),
        Button("Advance").OnClick([progress] {
          progress.Update([](float& value) { value = value >= 0.95F ? 0.15F : value + 0.2F; });
        }),
    }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
    HUXERUI_THEME(AccentTheme, Button("Nested button style").OnClick([] {})),
    Button("Explicit modifier wins").With(Background(explicit_button_color)).OnClick([] {}),
    Button("Disabled button").With(Enabled(false)).OnClick([] {}),
    HUXERUI_THEME(
        MaterialDarkTheme,
        Column {
            Text("Nested Material dark theme", TextRole::Title),
            Text("A complete nested theme replaces every token."),
            Button("Dark theme button").OnClick([] {}),
        }.With(
            Padding(UseTheme().spacing.medium),
            Spacing(UseTheme().spacing.small),
            Background(UseTheme().colors.background)
        )
    ),
    HUXERUI_THEME(
        FlatTheme,
        Column {
            Text("Nested Flat theme", TextRole::Title),
            Button("Flat theme button").OnClick([] {}),
        }.With(
            Padding(UseTheme().spacing.medium),
            Spacing(UseTheme().spacing.small),
            Background(UseTheme().colors.background)
        )
    ),
  }.With(Padding(theme.spacing.extra_large), Spacing(theme.spacing.medium), Background(theme.colors.background));
}

View App() {
  return HUXERUI_THEME(MaterialTheme, ThemeContent());
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Theme",
        .width = 560.0F,
        .height = 820.0F,
    }
)
