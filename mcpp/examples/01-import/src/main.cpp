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

#if defined(_WIN32) && defined(_MSC_VER)
// Keep GUI linking local to this entry; mcpp package link flags also reach tests and consumers.
#pragma comment(linker, "/subsystem:windows")
#pragma comment(linker, "/entry:mainCRTStartup")
#endif

int main() { return huxerui::RunApplication(); }
