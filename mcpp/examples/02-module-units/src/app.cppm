// The primary interface unit. It imports the others rather than including
// anything, and mcpp works out the order from the imports alone -- there is no
// list of module units in mcpp.toml, and no header to keep in step.

export module app;

import std;
import huxerui;

import banner;
import counter;

using namespace huxerui;

View App() {
  return Column {
    Text("module units", TextRole::Title),
    Divider(),
    Banner(),
    Counter(),
  }.With(Padding(32.0F), Spacing(16.0F));
}

const Application application{
    App,
    {.window = {.title = "02 - module units", .initial_size = {560.0F, 360.0F}}},
};
