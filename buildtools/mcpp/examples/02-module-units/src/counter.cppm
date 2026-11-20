// A module unit with no headers.
//
// `import std;` makes std::type_info visible, which UseState() requires -- GCC
// checks `typeid` per translation unit and UseState() instantiates it here, in
// the caller, not inside HuxerUI.

export module counter;

import std;
import huxerui;

using namespace huxerui;

export
[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count).With(FontSize(24.0F)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}
