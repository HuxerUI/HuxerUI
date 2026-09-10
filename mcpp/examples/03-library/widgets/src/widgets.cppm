// What this library publishes. A consumer writes `import widgets;` and reaches
// exactly the names marked `export` here -- nothing else, and no headers.

export module widgets;

import std;
import huxerui;

using namespace huxerui;

// Composable, because it owns state across recompositions. hcg transforms it
// HERE, in the library's own build, so a consumer imports something already
// wrapped in its scope.
export
[[huxerui::composable]]
View LabelledCounter(std::string label) {
  auto count = UseState(0);

  return Row {
    Text(std::move(label)),
    Text(count).With(FontSize(20.0F)),
    Button("+1").OnClick([count] { count += 1; }),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

// Not every export is composable. This one holds no state, so it needs no
// marker and no scope.
export View Badge(std::string_view text) {
  return Text(text).With(FontSize(12.0F), Padding(4.0F));
}
