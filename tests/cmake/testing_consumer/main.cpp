#include <huxerui/testing/ui_test.h>

namespace {
huxerui::View Content() {
  using namespace huxerui;
  auto count = UseState(0);
  return Button(std::to_string(count.Get())).OnClick([count]() mutable { count = count.Get() + 1; });
}
}

int main() {
  using namespace huxerui::testing;
  huxerui::Application application(Content, {.show_debug_overlay = false});
  UiTest ui(application);
  ui.Find(UiSelector::Text("0")).Tap();
  return ui.Find(UiSelector::Text("1")).Exists() ? 0 : 1;
}
