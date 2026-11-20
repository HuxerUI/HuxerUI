export module app;

import std;
import huxerui;
import app.resources;
import cts.probe;
import ctskit.widgets;

using namespace huxerui;

View App() {
  cts::Probe probe;
  probe.Record("app.title", UseString(app::strings::title));
  probe.Record("widgets.caption", ctskit::UseWidgetsCaption());
  probe.Record("core.name", ctskit::UseWidgetsCoreName());
  return Column {
    Text(app::strings::title),
    ctskit::Caption(),
  };
}

const Application application{App, {.window = {.title = "cts_app_modules"}}};
