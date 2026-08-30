#include <iostream>
#include <format>

#include <huxerui/huxerui.h>

using namespace huxerui;

struct CounterChanged : Event<void(int)> {};

[[huxerui::composable]]
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

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Custom Event",
            .initial_size = {520.0F, 360.0F},
        },
    }
};
