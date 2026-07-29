#include <huxerui/huxerui.h>

#include <numeric>
#include <vector>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(31, 35, 40);
constexpr Color secondary_text_color = Color::Rgb(91, 98, 106);

[[huxerui::scope]]
View ScrollToolbar(ScrollState scroll) {
  const ScrollMetrics metrics = scroll.Metrics();

  return Row {
    Button("Top").OnClick([scroll] {
      static_cast<void>(scroll.ScrollTo(0.0F));
    }),
    Button("+120").OnClick([scroll] {
      static_cast<void>(scroll.ScrollBy(120.0F));
    }),
    Button("Bottom").OnClick([scroll] {
      static_cast<void>(scroll.ScrollTo(scroll.MaxOffset()));
    }),
    Spacer(),
    Text::Format(
        "Offset {} / {}",
        static_cast<int>(metrics.offset),
        static_cast<int>(metrics.maximum_offset)
    ).With(Foreground(secondary_text_color)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::scope]]
View ScrollRow(int index) {
  auto taps = UseState(0);

  return Row {
    Text::Format("Item {}", index).With(FontSize(18.0F), Foreground(primary_text_color)),
    Spacer(),
    Text::Format("Taps {}", taps).With(FontSize(14.0F), Foreground(secondary_text_color)),
    Button("Tap").OnClick([taps] { taps += 1; }),
  }.With(
      Padding(12.0F),
      Spacing(12.0F),
      Background(Color::Rgb(246, 248, 250)),
      CornerRadius(8.0F),
      CrossAlign(CrossAxisAlignment::Center)
  );
}

View App() {
  auto scroll = UseScrollState();
  std::vector<int> items(24);
  std::iota(items.begin(), items.end(), 1);

  return Column {
    ScrollToolbar(scroll),
    ScrollView {
      Column {
          Text("ScrollView").With(FontSize(30.0F), Foreground(primary_text_color)),
          Text(
              "Scroll with a trackpad or mouse wheel, observe its offset, or "
              "move it programmatically. Item state persists after scrolling."
          )
              .With(FontSize(14.0F), Foreground(secondary_text_color)),
          ForEach(items, [](int index) { return ScrollRow(index).Key(index); }),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }
        .ScrollState(scroll)
        .With(ScrollBar(), Grow()),
  }.With(
      Padding(24.0F),
      Spacing(12.0F),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Scroll View",
        .width = 640.0F,
        .height = 560.0F,
    }
)
