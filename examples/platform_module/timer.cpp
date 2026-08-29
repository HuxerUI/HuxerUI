#include "timer.h"

namespace huxerui::example {

std::shared_ptr<TimerService> UseTimer() {
  return UseService<TimerService>();
}

} // namespace huxerui::example
