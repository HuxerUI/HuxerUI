#include "timer.h"

#include <huxerui/ohos/platform_registry.h>

namespace huxerui::example {

void InstallTimer(ApplicationContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<TimerService>>(timer::type,
      ohos::ArkTsWindowPlatformModuleFactory<std::shared_ptr<TimerService>>{
          .factory_name = timer::type,
          .create = MakeChannelTimer,
      });
}

} // namespace huxerui::example
