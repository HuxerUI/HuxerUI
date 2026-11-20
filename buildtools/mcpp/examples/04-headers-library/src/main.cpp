// A unit that includes the library's header keeps its macros, which `#if`
// needs; a unit that imports the module names the mapped constants instead, as
// src/app.cppm does. A unit that does both writes the macro, never
// gauge::macros::<name>, inside which the macro would expand.
#include <gauge/version.h>

#if GAUGE_VERSION_MAJOR < 1
#error "this application needs gauge 1 or later"
#endif

import huxerui;
import app;

#if defined(_WIN32) && defined(_MSC_VER)
// Keep GUI linking local to this entry; mcpp package link flags also reach tests and consumers.
#pragma comment(linker, "/subsystem:windows")
#pragma comment(linker, "/entry:mainCRTStartup")
#endif

int main() { return huxerui::RunApplication(); }
