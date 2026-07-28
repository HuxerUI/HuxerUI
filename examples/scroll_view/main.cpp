#include <huxerui/huxerui.h>

#include <numeric>
#include <vector>

using namespace huxerui;

[[huxerui::scope]]
View ScrollRow(int index) {
  auto taps = UseState(0);

  return Row {
        Text::Format("Item {}", index).With(
            FontSize{18.0F},
            Foreground{Color::Rgb(31, 35, 40)}
        ),
        Spacer(),
        Text::Format("Taps {}", taps).With(
            FontSize{14.0F},
            Foreground{Color::Rgb(91, 98, 106)}
        ),
        Button("Tap")
            .OnClick([taps] {
                 taps += 1;
            }),
    }.With(
        Padding{12.0F},
        Spacing{12.0F},
        Background{Color::Rgb(246, 248, 250)},
        CornerRadius{8.0F},
        CrossAlign{CrossAxisAlignment::Center}
  );
}

View App() {
  std::vector<int> items(24);
  std::iota(items.begin(), items.end(), 1);

  return ScrollView {
      Column {
          Text("ScrollView").With(
              FontSize{30.0F},
              Foreground{Color::Rgb(31, 35, 40)}
            ),
          Text(
              "Scroll with a trackpad or mouse wheel. Item state persists after "
              "scrolling.")
              .With(
                  FontSize{14.0F},
                  Foreground{Color::Rgb(91, 98, 106)}
                ),
          ForEach(items, [](int index) {
            return ScrollRow(index).Key(index);
          }),
      }.With(
          Padding{24.0F},
          Spacing{8.0F},
          CrossAlign{CrossAxisAlignment::Stretch}),
  };
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Scroll View",
        .width = 640.0F,
        .height = 560.0F,
    })
