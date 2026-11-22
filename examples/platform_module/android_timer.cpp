#include "timer.h"

#include <utility>

#include <huxerui/android/platform_registry.h>

namespace huxerui::example {

void InstallTimer(ApplicationContext& root) {
  android::JavaPlatformModuleFactory<std::shared_ptr<TimerService>> factory;
  factory.class_name = "org.huxerui.examples.platformmodule.PlatformTimer";
  factory.create = MakeChannelTimer;
  root.RegisterPlatformModule<std::shared_ptr<TimerService>>(timer::type, std::move(factory));
}

} // namespace huxerui::example
