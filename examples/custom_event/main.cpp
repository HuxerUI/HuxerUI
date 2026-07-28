#include <iostream>
#include <format>

#include <huxerui/huxerui.h>

using namespace huxerui;

struct CounterEvents {
  struct CountChanged : Event<CounterEvents, void(int)> {};
};

[[huxerui::scope]]
auto Counter(int value) {
  auto count = UseState(1);
  auto events = UseEvents<CounterEvents>();

  return Button("+1").OnClick([events, count, value] {
    count += value;
    events.Emit<CounterEvents::CountChanged>(count);
  });
}

View App() {
  auto toast = UseToast();
  return Column {
      Counter(1)
        .On<CounterEvents::CountChanged>([toast](int count) {
          toast.Show(std::format("count: {}", count));
        })
  }.With(Padding{32.0F});
}

int main() {
  return RunApp(
      App,
      {
          .title = "HuxerUI Custom Event",
          .width = 520.0F,
          .height = 360.0F,
          .root_hooks = {InstallToast()}
      });
}
