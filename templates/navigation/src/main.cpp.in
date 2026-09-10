// The entry translation unit, and it instantiates NOTHING.
//
// That is the whole trick behind it having no includes. HuxerUI's UseState(),
// View and Layout templates instantiate `typeid` in their CALLER, and GCC's
// typeid check is per-translation-unit -- so any TU that builds a View needs
// <typeinfo>. Keep the entry to `RunApplication()` and the work in a module
// unit, and the include goes where a module unit's includes belong: its own
// global module fragment.
//
// huxerui.rules leaves the entry alone for a separate reason -- a build program
// can add a source but cannot replace one, so a transformed copy of this file
// would link beside the original as `multiple definition of main`. The two
// constraints point the same way.

import huxerui;
import app;

int main() { return huxerui::RunApplication(); }
