#include <iostream>
#include <format>

#include <huxerui/huxerui.h>

using namespace huxerui;

struct CounterChanged : Event<int> {};

[[huxerui::scope]]
auto Counter(int value) {
  auto count = UseState(1);
  auto events = UseEvents();

  return Button("+1").OnClick([events, count, value] {
    count += value;
    events.Emit<CounterChanged>(count);
  });
}

View App() {
  auto toast = UseToast();
  return Column {
    Counter(1).On<CounterChanged>([toast](int count) {
      toast.Show(std::format("count: {}", count));
    })
  }.With(Padding(32.0F));
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Custom Event",
        .width = 520.0F,
        .height = 360.0F,
    }
)
