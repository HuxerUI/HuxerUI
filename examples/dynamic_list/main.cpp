#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(31, 35, 40);
constexpr Color secondary_text_color = Color::Rgb(91, 98, 106);

struct Item {
  std::uint64_t id;
  std::string title;

  bool operator==(const Item&) const = default;
};

[[huxerui::scope]]
View ItemRow(Item item) {
  auto taps = UseState(0);

  return Row {
    Column {
        Text(item.title).With(FontSize(18.0F), Foreground(primary_text_color)),
        Text("Key " + std::to_string(item.id)).With(FontSize(12.0F), Foreground(secondary_text_color)),
    }.With(Spacing(4.0F)),
    Spacer(),
    Text::Format("Taps {}", taps).With(FontSize(16.0F), Foreground(primary_text_color)),
    Button("Tap").OnClick([taps] { taps += 1; }),
  }.With(
      Padding(12.0F),
      Spacing(12.0F),
      Background(Color::Rgb(246, 248, 250)),
      CornerRadius(8.0F),
      CrossAlign(CrossAxisAlignment::Center)
  );
}

[[huxerui::scope]]
View DynamicList() {
  auto items = UseState(
      std::vector<Item> {
          { 1, "Alpha" },
          { 2, "Bravo" },
          { 3, "Charlie" },
      }
  );
  auto next_id = UseState(std::uint64_t { 4 });

  return ScrollView {
    Column {
        Text("Dynamic List").With(FontSize(30.0F), Foreground(primary_text_color)),
        Text(
            "Tap any item, then add, remove, or reverse the list. Stable keys "
            "keep each item's local UseState state attached to its data."
        )
            .With(FontSize(14.0F), Foreground(secondary_text_color)),
        Row {
            Button("Add").OnClick([items, next_id] {
              const auto id = next_id.Get();
              next_id += 1;
              items.Update([id](auto& values) {
                values.push_back(
                    {
                        id,
                        "Item " + std::to_string(id),
                    }
                );
              });
            }),
            Button("Remove first").OnClick([items] {
              items.Update([](auto& values) {
                if (!values.empty()) {
                  values.erase(values.begin());
                }
              });
            }),
            Button("Reverse").OnClick([items] {
              items.Update([](auto& values) { std::reverse(values.begin(), values.end()); });
            }),
        }.With(Spacing(8.0F)),
        ForEach(items, [](const Item& item) { return ItemRow(item).Key(item.id); }),
    }.With(Padding(24.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  };
}

View App() {
  return DynamicList();
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Dynamic List",
        .width = 720.0F,
        .height = 620.0F,
    }
)
