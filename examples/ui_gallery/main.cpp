#include <huxerui/huxerui.h>

#include <string>
#include <utility>

using namespace huxerui;

constexpr float gallery_width = 560.0F;

View Tag(std::string label, Color color) {
  return Text(std::move(label)).With(
      Padding(EdgeInsets::Symmetric(10.0F, 6.0F)),
      Background(color),
      Foreground(Color::White()),
      CornerRadius(8.0F)
  );
}

View Panel(View content) {
  const ThemeSpec& theme = UseTheme();
  return std::move(content).With(
      Frame{.width = gallery_width},
      Padding(theme.spacing.medium),
      Background(theme.colors.surface),
      Shadow{
          .color = Color::Rgb(20, 28, 40, 0.14F),
          .offset = {0.0F, 6.0F},
          .blur_radius = 18.0F,
          .spread = -2.0F,
      },
      CornerRadius(theme.shapes.extra_large)
  );
}

[[huxerui::scope]]
View ControlsDemo() {
  const ThemeSpec& theme = UseTheme();
  auto checkbox_checked = UseState(true);
  auto chip_selected = UseState(false);
  auto radio_choice = UseState(0);
  auto switch_checked = UseState(false);
  auto progress = UseState(0.35F);
  auto password = UseState(TextEditingValue::FromText(""));
  auto message = UseState(TextEditingValue::FromText(""));

  return Panel(
      Column {
        Text("Controls", TextRole::Title),
        Row {
          Button("Button").OnClick([] {}),
          Button("Disabled").With(Enabled(false)).OnClick([] {}),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          Chip("Action").OnClick([] {}),
          Chip(chip_selected ? "Selected" : "Selectable", chip_selected)
              .OnChanged([chip_selected](bool selected) { chip_selected = selected; }),
          Chip("Disabled", false).OnChanged([](bool) {}).With(Enabled(false)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          Row {
            Checkbox(checkbox_checked).OnChanged(
                [checkbox_checked](bool checked) { checkbox_checked = checked; }
            ),
            Text(checkbox_checked ? "Checked" : "Unchecked"),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
          Row {
            Switch(switch_checked).OnChanged([switch_checked](bool checked) { switch_checked = checked; }),
            Text(switch_checked ? "On" : "Off"),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          Row {
            RadioButton(radio_choice == 0).OnChanged([radio_choice](bool selected) {
              if (selected) {
                radio_choice = 0;
              }
            }),
            Text("Option A"),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
          Row {
            RadioButton(radio_choice == 1).OnChanged([radio_choice](bool selected) {
              if (selected) {
                radio_choice = 1;
              }
            }),
            Text("Option B"),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          ProgressCircle(),
          Text("Indeterminate"),
          ProgressCircle(progress),
          Text::Format("{}%", static_cast<int>(progress * 100.0F)),
          Button("Advance").OnClick([progress] {
            progress.Update([](float& value) { value = value >= 0.95F ? 0.15F : value + 0.2F; });
          }),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          ProgressBar(),
          Text("Indeterminate"),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          ProgressBar(progress),
          Text::Format("{}%", static_cast<int>(progress * 100.0F)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          Slider(progress).Step(0.05F).OnChanged([progress](float value) { progress = value; }),
          Text::Format("{}%", static_cast<int>(progress * 100.0F)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        TextField(password)
            .Secure()
            .MaxLength(64)
            .Placeholder("Password")
            .Validation(Validate(password.Get().text, Required("Password is required")))
            .OnChanged([password](const TextEditingValue& value) { password = value; })
            .With(Frame{.width = gallery_width - theme.spacing.extra_large}),
        TextField(message)
            .LineLimits(TextFieldLineLimits::MultiLine(3, 5))
            .MaxLength(240)
            .Placeholder("Message")
            .OnChanged([message](const TextEditingValue& value) { message = value; })
            .With(Frame{.width = gallery_width - theme.spacing.extra_large}),
      }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Start))
  );
}

View LayoutDemo() {
  const ThemeSpec& theme = UseTheme();
  return Panel(
      Column {
        Text("Layout", TextRole::Title),
        Row {
          Tag("Fixed", theme.colors.error),
          Text("Grow").With(
              Padding(EdgeInsets::Symmetric(10.0F, 6.0F)),
              Background(theme.colors.primary),
              Foreground(theme.colors.on_primary),
              CornerRadius(theme.shapes.medium),
              Grow()
          ),
          Tag("Trailing", Color::Rgb(26, 127, 55)),
        }.With(
            Frame{.width = gallery_width - theme.spacing.extra_large},
            Spacing(theme.spacing.small),
            CrossAlign(CrossAxisAlignment::Center)
        ),
        Flow {
          Tag("Android", Color::Rgb(26, 127, 55)),
          Tag("macOS", theme.colors.primary),
          Tag("Windows", Color::Rgb(130, 80, 223)),
          Tag("Declarative", theme.colors.error),
          Tag("Native", theme.colors.primary),
          Tag("C++", Color::Rgb(26, 127, 55)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
      }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Start))
  );
}

[[huxerui::scope]]
View MotionDemo() {
  const ThemeSpec& theme = UseTheme();
  auto transformed = UseState(false);

  return Panel(
      Column {
        Text("Motion", TextRole::Title),
        Row {
          Button(transformed ? "Reset" : "Transform").OnClick([transformed] { transformed = !transformed; }),
          Tag("Scale + rotation", Color::Rgb(130, 80, 223)).With(
              Scale(AnimateTo(transformed ? 1.2F : 1.0F, TweenSpec(theme.motion.normal, Easing::EaseOut))),
              Rotation(AnimateTo(transformed ? 10.0F : 0.0F, SpringSpec()))
          ),
        }.With(Spacing(theme.spacing.large), CrossAlign(CrossAxisAlignment::Center)),
      }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Start))
  );
}

View GalleryContent() {
  auto& theme = UseTheme();
  return ScrollView {
    Column {
      Text("UI Gallery").With(FontSize(28.0F)),
      Text("Controls, layout, and motion in one compact overview", TextRole::Label),
      ControlsDemo(),
      LayoutDemo(),
      MotionDemo(),
    }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background),
      CrossAlign(CrossAxisAlignment::Center)
    )
  }.With(ScrollBar());
}

View App() {
  return MaterialTheme(GalleryContent);
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI UI Gallery",
        .width = 640.0F,
        .height = 640.0F,
    }
)
