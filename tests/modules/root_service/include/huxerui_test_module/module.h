#pragma once

#include <huxerui/root.h>

namespace huxerui_test_module {

struct Service {
  int value = 0;
};

void Install(huxerui::RootContext& root);

} // namespace huxerui_test_module
