// A whole HuxerUI application in one module unit, with no headers at all.
//
// `import std;` is what removes the last one. std::type_info has to be visible
// wherever a View is built, which a CMake project gets from
// #include <huxerui/huxerui.h> and an importer cannot: a global module
// fragment's includes do not reach whoever imports it.

export module app;

import std;
import huxerui;

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count).With(FontSize(24.0F)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}

View App() {
  return Column {
    Text("import huxerui;", TextRole::Title),
    Divider(),
    Counter(),
  }.With(Padding(32.0F), Spacing(16.0F));
}

const Application application{
    App,
    {.window = {.title = "01 - import", .initial_size = {560.0F, 360.0F}}},
};
