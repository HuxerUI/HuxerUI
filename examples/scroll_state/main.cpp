#include <huxerui/huxerui.h>

#include <numeric>
#include <vector>

using namespace huxerui;

[[huxerui::scope]]
View ScrollToolbar(ScrollState scroll) {
  return Row{
        Button("Top").OnClick(
            [scroll] { static_cast<void>(scroll.ScrollTo(0.0F)); }),
        Button("Item 5000").OnClick([scroll] {
          static_cast<void>(scroll.ScrollToItem(4999, ScrollAlignment::Center));
        }),
        Spacer(),
        Text::Format("Offset {}", static_cast<int>(scroll.Offset())).With(
            Foreground{Color::Rgb(91, 98, 106)}
        ),
    }.With(
        Spacing{12.0F},
        CrossAlign{CrossAxisAlignment::Center}
  );
}

View ScrollItem(int index) {
  return Text::Format("Item {}", index).With(
      Padding{12.0F},
      Background{Color::Rgb(246, 248, 250)},
      CornerRadius{8.0F}
    );
}

View App() {
  auto scroll = UseScrollState();
  std::vector<int> items(10000);
  std::iota(items.begin(), items.end(), 1);

  return Column{
      ScrollToolbar(scroll),
      VirtualList(items, [](int index) { return ScrollItem(index).Key(index); })
          .ItemExtent(48.0F)
          .ScrollState(scroll)
          .With(
              Spacing{8.0F},
              ScrollBar{},
              Grow{}
          ),
  }.With(
      Padding{24.0F},
      Spacing{12.0F}
  );
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Scroll State",
        .width = 640.0F,
        .height = 560.0F,
    })
