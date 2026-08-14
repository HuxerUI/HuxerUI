#include <huxerui/huxerui.h>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(27, 31, 36);

[[huxerui::scope]]
View Counter(const std::string& button_text, int value) {
  auto count = UseState(1);

  return Column {
    Text(count).With(FontSize(32.0F), Foreground(primary_text_color)),
    Button(button_text).OnClick([count, value] { count += value; }),
  }.With(Spacing(16.0F));
}

View App() {
  return Column {
    Counter("+1", 1),
    Counter("+2", 2),
  }.With(Padding(32.0F));
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Counter",
            .initial_size = {520.0F, 360.0F},
        },
    }
};
