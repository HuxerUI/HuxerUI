#include "frame_source.h"

#include <utility>

#include <huxerui/android/platform_registry.h>

namespace huxerui::example::buffer_reference {

void Install(ApplicationContext& root) {
  android::JavaPlatformModuleFactory<std::shared_ptr<FrameSource>> factory;
  factory.class_name = "org.huxerui.examples.bufferreference.PlatformFrameSource";
  factory.create = CreateBridgeSource;
  root.RegisterPlatformModule<std::shared_ptr<FrameSource>>(module_name, std::move(factory));
}

} // namespace huxerui::example::buffer_reference
