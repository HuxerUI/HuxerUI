// The entry instantiates nothing, so it needs neither headers nor a global
// module fragment. Every page lives in a module unit of its own.

import huxerui;
import app;

#if defined(_WIN32) && defined(_MSC_VER)
// Keep GUI linking local to this entry; mcpp package link flags also reach tests and consumers.
#pragma comment(linker, "/subsystem:windows")
#pragma comment(linker, "/entry:mainCRTStartup")
#endif

int main() { return huxerui::RunApplication(); }
