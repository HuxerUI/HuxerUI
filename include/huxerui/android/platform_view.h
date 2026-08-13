#pragma once

#include <functional>

#include <huxerui/android/jni.h>
#include <huxerui/platform_module.h>

namespace huxerui::android {

struct PlatformViewFactory {
  std::function<jobject(JNIEnv*, jobject, const PlatformPayload&, PlatformEventSink)> create;
  std::function<void(JNIEnv*, jobject, const PlatformPayload&)> update;
  std::function<void(JNIEnv*, jobject)> dispose;
};

} // namespace huxerui::android
