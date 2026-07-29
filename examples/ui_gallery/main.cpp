#include <huxerui/huxerui.h>

#include <string>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(31, 35, 40);
constexpr Color secondary_text_color = Color::Rgb(91, 98, 106);

View Chip(std::string label, Color color) {
  return Text(std::move(label)).With(Padding(10.0F), Background(color), Foreground(Color::White()), CornerRadius(8.0F));
}

View Distribution(std::string title, MainAxisAlignment alignment) {
  return Column {
    Text(std::move(title)).With(FontSize(14.0F), Foreground(secondary_text_color)),
    Row {
      Chip("A", Color::Rgb(9, 105, 218)),
      Chip("B", Color::Rgb(130, 80, 223)),
      Chip("C", Color::Rgb(26, 127, 55)),
    }.With(
        Frame(640.0F, 56.0F),
        Padding(8.0F),
        Spacing(8.0F),
        Background(Color::Rgb(246, 248, 250)),
        CornerRadius(8.0F),
        MainAlign(alignment),
        CrossAlign(CrossAxisAlignment::Center)
    ),
  }.With(Spacing(6.0F));
}

[[huxerui::scope]] View ControlsDemo() {
  auto checkbox_checked = UseState(true);
  auto switch_checked = UseState(false);
  auto progress = UseState(0.35F);

  return Column {
    Text("Controls").With(FontSize(20.0F), Foreground(primary_text_color)),
    Row {
      Button("Button").OnClick([] {}),
      Button("Disabled").With(Enabled(false)).OnClick([] {}),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
    Row {
      Checkbox(checkbox_checked).OnChanged([checkbox_checked](bool checked) { checkbox_checked = checked; }),
      Text::Format("Checkbox: {}", checkbox_checked ? "checked" : "unchecked"),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
    Row {
      Switch(switch_checked).OnChanged([switch_checked](bool checked) { switch_checked = checked; }),
      Text::Format("Switch: {}", switch_checked ? "on" : "off"),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
    Row {
      ProgressCircle(),
      Text("Indeterminate"),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
    Row {
      ProgressCircle(progress),
      Text::Format("Progress: {}", progress),
      Button("Advance").OnClick([progress] {
        progress.Update([](float& value) { value = value >= 0.95F ? 0.15F : value + 0.2F; });
      }),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(
      Frame{.width = 640.0F},
      Padding(16.0F),
      Spacing(14.0F),
      Background(Color::Rgb(246, 248, 250)),
      CornerRadius(12.0F),
      CrossAlign(CrossAxisAlignment::Start)
  );
}

[[huxerui::scope]] View TransformDemo() {
  auto transformed = UseState(false);
  return Column {
    Text("Animated presentation").With(FontSize(14.0F), Foreground(secondary_text_color)),
    Row {
      Button(transformed ? "Reset" : "Transform").OnClick([transformed] { transformed = !transformed; }),
      Chip("Scale + rotation", Color::Rgb(130, 80, 223)).With(
          Scale(AnimateTo(transformed ? 1.25F : 1.0F, TweenSpec(0.24, Easing::EaseOut))),
          Rotation(AnimateTo(transformed ? 12.0F : 0.0F, SpringSpec()))
      ),
    }.With(Spacing(20.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(10.0F));
}

View GalleryContent() {
  return Column {
    Text("UI Gallery").With(FontSize(30.0F), Foreground(primary_text_color)),
    Text("Built-in controls, layout containers, typography, and presentation animations")
        .With(FontSize(14.0F), Foreground(secondary_text_color)),
    Column {
      Text("Typography").With(FontSize(20.0F), Foreground(primary_text_color)),
      Text("Title", TextRole::Title),
      Text("Body text uses the active theme's semantic typography.", TextRole::Body),
      Text("Label", TextRole::Label),
    }.With(
        Frame{.width = 640.0F},
        Padding(16.0F),
        Spacing(8.0F),
        Background(Color::Rgb(246, 248, 250)),
        CornerRadius(12.0F),
        CrossAlign(CrossAxisAlignment::Start)
    ),
    ControlsDemo(),
    Distribution("SpaceBetween", MainAxisAlignment::SpaceBetween),
    Distribution("Center", MainAxisAlignment::Center),
    Column {
      Text("Spacer and Grow").With(FontSize(14.0F), Foreground(secondary_text_color)),
      Row {
        Chip("Fixed", Color::Rgb(188, 76, 0)),
        Text("Grow").With(
            Padding(10.0F),
            Background(Color::Rgb(9, 105, 218)),
            Foreground(Color::White()),
            CornerRadius(8.0F),
            Grow()
        ),
        Spacer(),
        Chip("Trailing", Color::Rgb(26, 127, 55)),
      }.With(
          Frame(640.0F, 56.0F),
          Padding(8.0F),
          Spacing(8.0F),
          Background(Color::Rgb(246, 248, 250)),
          CornerRadius(8.0F),
          CrossAlign(CrossAxisAlignment::Center)
      ),
    }.With(Spacing(6.0F)),
    Column {
      Text("Responsive Flow").With(FontSize(14.0F), Foreground(secondary_text_color)),
      Flow {
        Chip("Android", Color::Rgb(26, 127, 55)),
        Chip("macOS", Color::Rgb(9, 105, 218)),
        Chip("Windows", Color::Rgb(130, 80, 223)),
        Chip("Desktop", Color::Rgb(188, 76, 0)),
        Chip("Mobile", Color::Rgb(9, 105, 218)),
        Chip("Declarative", Color::Rgb(26, 127, 55)),
        Chip("Native", Color::Rgb(130, 80, 223)),
        Chip("C++", Color::Rgb(188, 76, 0)),
      }.With(
          Frame{.max_width = 640.0F},
          Padding(12.0F),
          Spacing(8.0F),
          Background(Color::Rgb(246, 248, 250)),
          CornerRadius(8.0F),
          CrossAlign(CrossAxisAlignment::Center)
      ),
    }.With(Spacing(6.0F)),
    Column {
      Text("Centered Stack").With(FontSize(14.0F), Foreground(secondary_text_color)),
      Stack {
        Chip("Centered", Color::Rgb(130, 80, 223)),
      }.With(
          Frame(640.0F, 88.0F),
          Background(Color::Rgb(246, 248, 250)),
          CornerRadius(8.0F),
          Align(HorizontalAlignment::Center, VerticalAlignment::Center)
      ),
    }.With(Spacing(6.0F)),
    TransformDemo(),
    Text("This text verifies width constraints and automatic wrapping. "
         "When the parent limits the available width, HuxerUI measures the text "
         "in the C++ layout layer while the platform layer only provides "
         "CoreText services and canvas drawing.")
        .With(FontSize(15.0F), Foreground(primary_text_color)),
  }.With(Padding(32.0F), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Center));
}

View App() {
  return ScrollView {
    GalleryContent(),
  }.With(ScrollBar());
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI UI Gallery",
        .width = 720.0F,
        .height = 660.0F,
    }
)
