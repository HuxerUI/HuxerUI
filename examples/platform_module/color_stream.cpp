#include "color_stream.h"

namespace huxerui::example {

std::shared_ptr<ColorStreamService> UseColorStream() {
  return UseService<ColorStreamService>();
}

} // namespace huxerui::example
