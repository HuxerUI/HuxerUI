// A scope written by hand, with no macro and no header.
//
// HUXERUI_SCOPE(...) exists for the header path, and it expands to exactly the
// line below:
//
//     #define HUXERUI_SCOPE(...) \
//         return ::huxerui::Scope([=]() -> ::huxerui::View __VA_ARGS__)
//
// Macros do not cross a module boundary, so `import huxerui;` cannot carry it
// -- but it does carry `Scope` and `View`, which is all the macro was hiding.
// Writing the expansion is the module-native spelling, and it is the same text
// hcg injects for a [[huxerui::composable]] function.
//
// Prefer the marker. This unit spells the scope out to show what the marker
// does; a real one would write:
//
//     [[huxerui::composable]]
//     View Banner() { return Text("..."); }

export module banner;

import std;
import huxerui;

using namespace huxerui;

export View Banner() {
  return Scope([=]() -> View { return Text("hand-written scope, no macro"); });
}
