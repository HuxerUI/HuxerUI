#include "smoke.h"

#include <iostream>

int main(int argc, char** argv) {
  const auto error = RunUiTestingSmoke(argc > 1 ? argv[1] : HUXERUI_SMOKE_PACKAGE);
  if (!error.empty()) std::cerr << error << '\n';
  return error.empty() ? 0 : 1;
}
