#pragma once

#import <TargetConditionals.h>

#include "timer.h"

#if TARGET_OS_IOS
#import <huxerui/ios/platform_registry.h>
#else
#import <huxerui/macos/platform_registry.h>
#endif

namespace huxerui::example {

std::shared_ptr<TimerService> CreateAppleTimerService(PlatformChannel channel);

#if TARGET_OS_IOS
id<HUXUIKitPlatformModuleFactory> CreateAppleTimerFactory();
#else
id<HUXAppKitPlatformModuleFactory> CreateAppleTimerFactory();
#endif

} // namespace huxerui::example
