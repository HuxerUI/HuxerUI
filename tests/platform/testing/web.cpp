#include "smoke.h"

#include <emscripten/bind.h>

EMSCRIPTEN_BINDINGS(huxerui_ui_testing_smoke) {
  emscripten::function("runUiTestingSmoke", +[] { return RunUiTestingSmoke("/package"); });
}
