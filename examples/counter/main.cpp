#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::scope]]
View Counter(const std::string& button_text, int value) {
  auto count = UseState(1);

  return Column{
      Text(count).With(
        FontSize{32.0F},
        Foreground{Color::Rgb(27, 31, 36)}
      ),
      Button(button_text).OnClick([count, value] {
        count += value;
      }),
  }.With(Spacing{16.0F});
}

View App() {
  return Column{
    Counter("+1", 1),
    Counter("+2", 2),
  }.With(Padding{32.0F});
}

int main() {
  return RunApp(
    App,
    {
      .title = "HuxerUI Counter",
      .width = 520.0F,
      .height = 360.0F,
    });
}
