#pragma once

#include <string>
#include <vector>

#include <huxerui/root.h>
#include <huxerui/view.h>

namespace huxerui {

struct AppOptions {
  std::string title = "HuxerUI";
  float width = 520.0F;
  float height = 360.0F;
  std::vector<RootHook> root_hooks;
};

using RootFactory = View (*)();

int RunApp(RootFactory root_factory, AppOptions options = {});

}  // namespace huxerui
