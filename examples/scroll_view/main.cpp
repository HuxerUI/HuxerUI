#include <huxerui/huxerui.h>

#include <numeric>
#include <vector>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(31, 35, 40);
constexpr Color secondary_text_color = Color::Rgb(91, 98, 106);

[[huxerui::composable]]
View ScrollToolbar(ScrollController scroll) {
  const ScrollMetrics metrics = scroll.Metrics();

  return Row{
      Button("Top").OnClick([scroll] { static_cast<void>(scroll.ScrollTo(0.0F)); }),
      Button("+120").OnClick([scroll] { static_cast<void>(scroll.ScrollBy(120.0F)); }),
      Button("Bottom").OnClick([scroll] { static_cast<void>(scroll.ScrollTo(scroll.MaxOffset())); }),
      Spacer(),
      Text::Format("Offset {} / {}", static_cast<int>(metrics.offset), static_cast<int>(metrics.maximum_offset))
          .With(Foreground(secondary_text_color)),
  }
      .With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View ScrollRow(int index) {
  auto taps = UseState(0);

  return Row{
      Text::Format("Item {}", index).With(FontSize(18.0F), Foreground(primary_text_color)),
      Spacer(),
      Text::Format("Taps {}", taps).With(FontSize(14.0F), Foreground(secondary_text_color)),
      Button("Tap").OnClick([taps] { taps += 1; }),
  }
      .With(
          Padding(12.0F),
          Spacing(12.0F),
          Background(Color::Rgb(246, 248, 250)),
          CornerRadius(8.0F),
          CrossAlign(CrossAxisAlignment::Center)
      );
}

View HorizontalScrollSection() {
  std::vector<int> items(10);
  std::iota(items.begin(), items.end(), 1);

  return Column{
      Text("Horizontal ScrollView").With(FontSize(20.0F), Foreground(primary_text_color)),
      Text("Drag horizontally or use a trackpad to scroll through every card.")
          .With(FontSize(14.0F), Foreground(secondary_text_color)),
      ScrollView{
          Row{
              ForEach(
                  items,
                  [](int index) {
                    return Text::Format("Card {}", index)
                        .With(
                            Frame(112.0F, 72.0F),
                            Background(Color::White()),
                            CornerRadius(8.0F),
                            Align(HorizontalAlignment::Center, VerticalAlignment::Center)
                        )
                        .Key(index);
                  }
              ),
          }
              .With(Spacing(8.0F)),
      }
          .ScrollAxis(Axis::Horizontal)
          .With(Frame{.height = 96.0F}, Padding(8.0F), ScrollBar()),
  }
      .With(
          Padding(16.0F),
          Spacing(8.0F),
          Background(Color::Rgb(234, 242, 255)),
          CornerRadius(12.0F),
          CrossAlign(CrossAxisAlignment::Stretch)
      );
}

[[huxerui::composable]]
View NestedScrollSection(ScrollController scroll) {
  const ScrollMetrics metrics = scroll.Metrics();
  std::vector<int> items(12);
  std::iota(items.begin(), items.end(), 101);

  return Column{
      Row{
          Text("Nested ScrollView").With(FontSize(20.0F), Foreground(primary_text_color)),
          Spacer(),
          Text::Format(
              "Inner offset {} / {}",
              static_cast<int>(metrics.offset),
              static_cast<int>(metrics.maximum_offset)
          )
              .With(Foreground(secondary_text_color)),
      }
          .With(CrossAlign(CrossAxisAlignment::Center)),
      Text(
          "Scroll inside this panel. When it reaches an edge, continued "
          "scrolling moves the outer ScrollView."
      )
          .With(FontSize(14.0F), Foreground(secondary_text_color)),
      ScrollView{
          Column{
              ForEach(items, [](int index) { return ScrollRow(index).Key(index); }),
          }
              .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
      }
          .Controller(scroll)
          .With(Frame(520.0F, 220.0F), Padding(8.0F), ScrollBar(), Background(Color::White()), CornerRadius(8.0F)),
  }
      .With(
          Padding(16.0F),
          Spacing(8.0F),
          Background(Color::Rgb(234, 242, 255)),
          CornerRadius(12.0F),
          CrossAlign(CrossAxisAlignment::Stretch)
      );
}

View App() {
  auto outer_scroll = UseScrollController();
  auto inner_scroll = UseScrollController();
  std::vector<int> leading_items(6);
  std::vector<int> trailing_items(10);
  std::iota(leading_items.begin(), leading_items.end(), 1);
  std::iota(trailing_items.begin(), trailing_items.end(), 7);

  return Column{
      ScrollToolbar(outer_scroll),
      ScrollView{
          Column{
              Text("ScrollView").With(FontSize(30.0F), Foreground(primary_text_color)),
              Text(
                  "Scroll with a trackpad or mouse wheel, observe its offset, or "
                  "move it programmatically. The nested panel demonstrates scroll "
                  "handoff at container boundaries."
              )
                  .With(FontSize(14.0F), Foreground(secondary_text_color)),
              ForEach(leading_items, [](int index) { return ScrollRow(index).Key(index); }),
              HorizontalScrollSection(),
              NestedScrollSection(inner_scroll),
              ForEach(trailing_items, [](int index) { return ScrollRow(index).Key(index); }),
          }
              .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
      }
          .Controller(outer_scroll)
          .With(ScrollBar(), Grow()),
  }
      .With(Padding(24.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Scroll View",
            .initial_size = {640.0F, 560.0F},
        },
    }
};
