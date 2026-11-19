// The application imports the headers library as the module `gauge`. Its
// constant, its function and the version the library maps from its macros all
// arrive through `import gauge;`; this unit includes no header.

export module app;

import std;
import huxerui;
import gauge;

using namespace huxerui;

View App() {
  const std::string version = std::to_string(gauge::macros::GAUGE_VERSION_MAJOR) + "." +
                              std::to_string(gauge::macros::GAUGE_VERSION_MINOR);
  return Column {
    Text("import gauge;  (gauge " + version + ")", TextRole::Title),
    gauge::Meter(7),
    gauge::Meter(12),
    Text("A meter holds at most " + std::to_string(gauge::max_level) + "."),
  }.With(Padding(32.0F), Spacing(16.0F));
}

const Application application{
    App,
    {.window = {.title = "04 - headers library", .initial_size = {560.0F, 360.0F}}},
};
