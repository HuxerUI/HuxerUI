// The entry, and it instantiates NOTHING -- which is why it needs no includes
// and no imports beyond these two.
//
// UseState(), View and Layout instantiate `typeid` in their CALLER, and GCC
// checks that per translation unit. Keeping the entry to RunApplication() puts
// that requirement in the module unit next door, where `import std;` answers
// it without a header.
//
// huxerui.rules leaves the entry alone for a second reason: a build program can
// add a source but cannot replace one, so a transformed copy of this file would
// link beside the original as `multiple definition of main`.

import huxerui;
import app;

int main() { return huxerui::RunApplication(); }
