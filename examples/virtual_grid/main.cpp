#include <huxerui/huxerui.h>

#include <numeric>
#include <vector>

using namespace huxerui;

[[huxerui::scope]]
View VirtualGridItem(int index) {
  auto taps = UseState(0);

  return Column{
        Text::Format("Item {}", index).With(
            FontSize{17.0F},
            Foreground{Color::Rgb(31, 35, 40)}
        ),
        Spacer(),
        Text::Format("Taps {}", taps).With(
            FontSize{14.0F},
            Foreground{Color::Rgb(91, 98, 106)}
        ),
        Button("Tap").OnClick([taps] { taps += 1; }),
    }.With(
        Padding{12.0F},
        Spacing{8.0F},
        Background{Color::Rgb(246, 248, 250)},
        CornerRadius{8.0F}
  );
}

View App() {
  std::vector<int> items(10000);
  std::iota(items.begin(), items.end(), 1);
  std::vector<std::size_t> spans(items.size(), std::size_t{1});
  for (std::size_t index = 0; index < spans.size(); index += 17) {
    spans[index] = 2;
  }

  return VirtualGrid(items, [](int index) { return VirtualGridItem(index).Key(index); })
      .Columns(GridColumns::Adaptive(180.0F))
      .RowExtent(132.0F)
      .ItemSpans(std::move(spans))
      .With(
          Spacing{12.0F},
          Padding{24.0F}
      );
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Virtual Grid",
        .width = 760.0F,
        .height = 600.0F,
    })
