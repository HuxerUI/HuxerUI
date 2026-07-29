#include <huxerui/huxerui.h>

#include <numeric>
#include <vector>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(31, 35, 40);
constexpr Color secondary_text_color = Color::Rgb(91, 98, 106);

[[huxerui::scope]]
View HorizontalVirtualListItem(int index) {
  auto taps = UseState(0);

  return Column {
    Text::Format("Item {}", index).With(FontSize(18.0F), Foreground(primary_text_color)),
    Text::Format("Taps {}", taps).With(FontSize(14.0F), Foreground(secondary_text_color)),
    Button("Tap").OnClick([taps] { taps += 1; }),
  }.With(
      Padding(16.0F),
      Spacing(12.0F),
      Background(Color::Rgb(246, 248, 250)),
      CornerRadius(8.0F),
      CrossAlign(CrossAxisAlignment::Center)
  );
}

View App() {
  std::vector<int> items(10000);
  std::iota(items.begin(), items.end(), 1);

  return VirtualList(items, [](int index) { return HorizontalVirtualListItem(index).Key(index); })
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(160.0F)
      .With(ScrollBar(), Padding(24.0F), Spacing(8.0F));
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Horizontal Virtual List",
        .width = 760.0F,
        .height = 320.0F,
    }
)
