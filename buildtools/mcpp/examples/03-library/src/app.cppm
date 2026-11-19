// The application, importing a library that is a separate mcpp package.
//
// `import widgets;` is the whole integration. No include path, no link line,
// no header: the dependency edge in mcpp.toml is what makes the module
// importable, and the library's own build already transformed its composables.

export module app;

import std;
import huxerui;
import widgets;

using namespace huxerui;

View App() {
  return Column {
    Row {
      Text("import widgets;", TextRole::Title),
      Badge("lib"),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    Divider(),
    LabelledCounter("apples"),
    LabelledCounter("pears"),
  }.With(Padding(32.0F), Spacing(16.0F));
}

const Application application{
    App,
    {.window = {.title = "03 - library", .initial_size = {560.0F, 400.0F}}},
};
