#pragma once

#include <functional>

#include <huxerui/android/jni.h>
#include <huxerui/platform_module.h>

namespace huxerui::android {

struct PlatformModuleFactory {
  std::function<huxerui::PlatformModuleFactory::Instance(JNIEnv*, jobject, const PlatformPayload&, PlatformEventSink)>
      create;
};

} // namespace huxerui::android
