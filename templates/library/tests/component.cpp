// A test is any tests/**/*.cpp: mcpp compiles each one into its own program
// and a zero exit is a pass. No framework, no registration. Reach for one with
// [dev-dependencies] when the assertions get interesting.
//
// What this one checks is small and worth checking: that the module imports at
// all, that both exports are visible through it, and that the composable
// compiles. Those are the three ways a library like this breaks without any
// consumer noticing until much later.

import std;
import huxerui;
import component;

int main() {
  const huxerui::View badge   = Badge("new");
  const huxerui::View counter = LabelledCounter("items");
  std::println("built {} views", 2);
  return 0;
}
