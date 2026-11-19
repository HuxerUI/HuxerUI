#include "greeting.h"

#include <app_resources.h>

#include "probe.h"

using namespace huxerui;

namespace cts {

[[huxerui::composable]]
View Greeting() {
  auto visits = UseState(1);
  Probe probe;
  probe.Record("app.greeting", UseString(app::strings::greeting));
  probe.Record("composable.state", std::to_string(static_cast<int>(visits)));
  return Text(app::strings::greeting);
}

} // namespace cts
