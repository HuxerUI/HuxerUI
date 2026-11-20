#include <ctskit/core.h>

#include <huxerui/huxerui.h>

#include <ctskit_core_resources.h>

namespace ctskit {

std::string UseCoreName() {
  return huxerui::UseString(ctskit_core::strings::name);
}

} // namespace ctskit
