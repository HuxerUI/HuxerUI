#include <huxerui/huxerui.h>

#include <string>

using namespace huxerui;

View Chip(std::string label, Color color) {
  return Text(std::move(label)).With(
      Padding{10.0F},
      Background{color},
      Foreground{Color::White()},
      CornerRadius{8.0F}
  );
}

View Distribution(std::string title, MainAxisAlignment alignment) {
  return Column {
    Text(std::move(title)).With(
      FontSize{14.0F},
      Foreground{Color::Rgb(91, 98, 106)}
    ),
    Row {
      Chip("A", Color::Rgb(9, 105, 218)),
      Chip("B", Color::Rgb(130, 80, 223)),
      Chip("C", Color::Rgb(26, 127, 55)),
    }.With(
      Frame{640.0F, 56.0F},
      Padding{8.0F},
      Spacing{8.0F},
      Background{Color::Rgb(246, 248, 250)},
      CornerRadius{8.0F},
      MainAlign{alignment},
      CrossAlign{CrossAxisAlignment::Center}
    ),
  }.With(Spacing{6.0F});
}

View App() {
  return Column {
    Text("Layout Gallery").With(
      FontSize{30.0F},
      Foreground{Color::Rgb(31, 35, 40)}
    ),
    Text("Row, Column, Spacer, Grow, Stack alignment, and multiline text").With(
      FontSize{14.0F},
      Foreground{Color::Rgb(91, 98, 106)}
    ),
    Distribution("SpaceBetween", MainAxisAlignment::SpaceBetween),
    Distribution("Center", MainAxisAlignment::Center),
    Column {
      Text("Spacer and Grow").With(
        FontSize{14.0F},
        Foreground{Color::Rgb(91, 98, 106)}
      ),
      Row{
        Chip("Fixed", Color::Rgb(188, 76, 0)),
        Text("Grow").With(
          Padding{10.0F},
          Background{Color::Rgb(9, 105, 218)},
          Foreground{Color::White()},
          CornerRadius{8.0F},
          Grow{}
        ),
        Spacer(),
        Chip("Trailing", Color::Rgb(26, 127, 55)),
      }.With(
        Frame{640.0F, 56.0F},
        Padding{8.0F},
        Spacing{8.0F},
        Background{Color::Rgb(246, 248, 250)},
        CornerRadius{8.0F},
        CrossAlign{CrossAxisAlignment::Center}
      ),
    }.With(Spacing{6.0F}),
    Column {
      Text("Centered Stack").With(
        FontSize{14.0F},
        Foreground{Color::Rgb(91, 98, 106)}
      ),
      Stack {
        Chip("Centered", Color::Rgb(130, 80, 223)),
      }.With(
        Frame{640.0F, 88.0F},
        Background{Color::Rgb(246, 248, 250)},
        CornerRadius{8.0F},
        Align{
          HorizontalAlignment::Center,
          VerticalAlignment::Center,
        }
      ),
    }.With(Spacing{6.0F}),
    Text(
      "This text verifies width constraints and automatic wrapping. "
      "When the parent limits the available width, HuxerUI measures the text "
      "in the C++ layout layer while the platform layer only provides "
      "CoreText services and canvas drawing.")
      .With(
        FontSize{15.0F},
        Foreground{Color::Rgb(31, 35, 40)}
      ),
  }.With(
    Padding{32.0F},
    Spacing{16.0F},
    CrossAlign{CrossAxisAlignment::Center}
  );
}

int main() {
  return RunApp(
    App,
    {
      .title = "HuxerUI Layout Gallery",
      .width = 720.0F,
      .height = 660.0F,
    });
}
