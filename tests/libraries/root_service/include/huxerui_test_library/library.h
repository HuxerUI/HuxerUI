#pragma once

#include <huxerui/window.h>

namespace huxerui_test_library {

struct Service {
  int value = 0;
};

void Install(huxerui::WindowContext& root);

} // namespace huxerui_test_library
