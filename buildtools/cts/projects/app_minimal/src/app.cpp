#include <huxerui/huxerui.h>

#include <app_resources.h>

#include "greeting.h"
#include "probe.h"

using namespace huxerui;

View App() {
  cts::Probe probe;
  probe.Record("app.title", UseString(app::strings::title));
  return Column {
    Text(app::strings::title),
    cts::Greeting(),
  };
}

const Application application{App, {.window = {.title = "cts_app_minimal"}}};
