#include <huxerui/huxerui.h>

using namespace huxerui;

View App() {
  return MaterialTheme([] {
    return Text("Hello, HuxerUI");
  });
}

const Application application{App, {.window = {.title = "@PROJECT_NAME@"}}};
