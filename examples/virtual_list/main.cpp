#include <huxerui/huxerui.h>

#include <numeric>
#include <vector>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(31, 35, 40);
constexpr Color secondary_text_color = Color::Rgb(91, 98, 106);

[[huxerui::scope]]
View VirtualListToolbar(ScrollController scroll) {
  const auto offset = static_cast<int>(scroll.Offset());

  return Row{
      Button("Top").OnClick([scroll] { static_cast<void>(scroll.ScrollTo(0.0F)); }),
      Button("Item 5000").OnClick([scroll] { static_cast<void>(scroll.ScrollToItem(4999, ScrollAlignment::Center)); }),
      Spacer(),
      Text::Format("Offset {}", offset).With(Foreground(secondary_text_color)),
  }
      .With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::scope]]
View VirtualListItem(int index) {
  auto taps = UseState(0);

  return Row{
      Text::Format("Item {}", index).With(FontSize(17.0F), Foreground(primary_text_color)),
      Spacer(),
      Text::Format("Taps {}", taps).With(FontSize(14.0F), Foreground(secondary_text_color)),
      Button("Tap").OnClick([taps] { taps += 1; }),
  }
      .With(
          Padding(index % 3 == 0 ? 20.0F : 12.0F),
          Spacing(12.0F),
          Background(Color::Rgb(246, 248, 250)),
          CornerRadius(8.0F),
          CrossAlign(CrossAxisAlignment::Center)
      );
}

View App() {
  auto scroll = UseScrollController();
  std::vector<int> items(10000);
  std::iota(items.begin(), items.end(), 1);

  return Column{
      VirtualListToolbar(scroll),
      VirtualList(items, [](int index) { return VirtualListItem(index).Key(index); })
          .Controller(scroll)
          .With(ScrollBar(), Spacing(8.0F), Grow()),
  }
      .With(Padding(24.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Virtual List",
        .width = 640.0F,
        .height = 560.0F,
    }
)
