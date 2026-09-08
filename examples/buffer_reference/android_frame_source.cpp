#include "frame_source.h"

#include <utility>

#include <huxerui/android/platform_registry.h>

namespace huxerui::example::buffer_reference {

void Install(RootContext& root) {
  android::JavaPlatformModuleFactory<std::shared_ptr<FrameSource>> factory;
  factory.class_name = "org.huxerui.examples.bufferreference.PlatformFrameSource";
  factory.create = CreateBridgeSource;
  root.RegisterPlatformModule<std::shared_ptr<FrameSource>>(module_name, std::move(factory));
  root.Provide(root.OpenPlatformModule<std::shared_ptr<FrameSource>>(module_name));
}

} // namespace huxerui::example::buffer_reference
