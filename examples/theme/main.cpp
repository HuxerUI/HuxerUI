#include <huxerui/huxerui.h>

using namespace huxerui;

constexpr Color explicit_button_color = Color::Rgb(88, 166, 255);

template <class Factory> View AccentTheme(Factory&& content) {
  const ThemeSpec& theme = UseTheme();
  ThemeDefinition definition;
  definition.Set(ButtonStyle{
      .background = theme.colors.error,
      .label_style = TextStyle{Font::System(theme.typography.label_large), theme.colors.on_primary},
      .padding = EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small),
      .corner_radius = theme.shapes.large,
  });
  return Theme(std::move(definition), std::forward<Factory>(content));
}

[[huxerui::scope]]
View TextFieldDemo() {
  const auto theme = UseTheme();
  auto material_value = UseState(TextEditingValue::FromText(""));
  auto flat_value = UseState(TextEditingValue::FromText(""));
  auto message = UseState(TextEditingValue::FromText(""));
  auto submission = UseState(std::string{"Type, drag to select, then press Enter to submit."});

  return Column{
      Text("Text fields", TextRole::Title),
      TextField(material_value)
          .Placeholder("Material text field")
          .OnChanged([material_value](const TextEditingValue& value) { material_value = value; })
          .OnSubmitted([material_value, submission] {
            submission = std::string{"Material submitted: "} + material_value->text;
          }),
      Text(submission),
      TextField(message)
          .LineLimits(TextFieldLineLimits::MultiLine())
          .Placeholder("Multiline message")
          .OnChanged([message](const TextEditingValue& value) { message = value; })
          .With(Frame{.height = 140.0F}),
      HUXERUI_THEME(
          FlatTheme,
          Column{
              Text("Flat text field", TextRole::Label),
              TextField(flat_value)
                  .Placeholder("Flat text field")
                  .OnChanged([flat_value](const TextEditingValue& value) { flat_value = value; })
                  .OnSubmitted([flat_value, submission] {
                    submission = std::string{"Flat submitted: "} + flat_value->text;
                  }),
          }
              .With(
                  Padding(UseTheme().spacing.medium),
                  Spacing(UseTheme().spacing.small),
                  Background(UseTheme().colors.background)
              )
      ),
  }
      .With(Spacing(theme.spacing.small));
}

[[huxerui::scope]]
View ThemeContent() {
  const auto theme = UseTheme();
  auto checkbox_checked = UseState(true);
  auto switch_checked = UseState(false);
  auto progress = UseState(0.35F);

  return Column{
      Text("Material Theme", TextRole::Title),
      Text(
          "Text and controls resolve semantic values from "
          "the nearest Theme."
      ),
      SelectionArea{
          Column{
              Text("Selectable text", TextRole::Label),
              Text("Drag on desktop or long-press on Android to select and copy this text."),
          }
              .With(Spacing(theme.spacing.small)),
      },
      Button("Material button").OnClick([] {}),
      Row{
          Checkbox(checkbox_checked).OnChanged([checkbox_checked](bool checked) { checkbox_checked = checked; }),
          Text::Format("Checkbox: {}", checkbox_checked ? "checked" : "unchecked"),
      }
          .With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
      Row{
          Switch(switch_checked).OnChanged([switch_checked](bool checked) { switch_checked = checked; }),
          Text::Format("Switch: {}", switch_checked ? "on" : "off"),
      }
          .With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
      Row{
          ProgressCircle(),
          Text("Indeterminate progress"),
      }
          .With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
      Row{
          ProgressCircle(progress),
          Text::Format("Progress: {}", progress),
          Button("Advance").OnClick([progress] {
            progress.Update([](float& value) { value = value >= 0.95F ? 0.15F : value + 0.2F; });
          }),
      }
          .With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
      TextFieldDemo(),
      HUXERUI_THEME(AccentTheme, Button("Nested button style").OnClick([] {})),
      Button("Explicit modifier wins").With(Background(explicit_button_color)).OnClick([] {}),
      Button("Disabled button").With(Enabled(false)).OnClick([] {}),
      HUXERUI_THEME(
          MaterialDarkTheme,
          Column{
              Text("Nested Material dark theme", TextRole::Title),
              Text("A complete nested theme replaces every token."),
              Button("Dark theme button").OnClick([] {}),
          }
              .With(
                  Padding(UseTheme().spacing.medium),
                  Spacing(UseTheme().spacing.small),
                  Background(UseTheme().colors.background)
              )
      ),
      HUXERUI_THEME(
          FlatTheme,
          Column{
              Text("Nested Flat theme", TextRole::Title),
              Button("Flat theme button").OnClick([] {}),
          }
              .With(
                  Padding(UseTheme().spacing.medium),
                  Spacing(UseTheme().spacing.small),
                  Background(UseTheme().colors.background)
              )
      ),
  }
      .With(Padding(theme.spacing.extra_large), Spacing(theme.spacing.medium), Background(theme.colors.background));
}

View App() {
  return MaterialTheme([] {
    return ScrollView{
        ThemeContent(),
    }
        .With(ScrollBar());
  });
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Theme",
        .width = 560.0F,
        .height = 820.0F,
    }
)
