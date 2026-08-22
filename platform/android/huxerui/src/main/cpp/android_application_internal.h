#pragma once

#include <jni.h>

#include <huxerui/app.h>

namespace huxerui::detail {

// JNI object references are borrowed from the current native call and are decoded synchronously.
struct AndroidApplicationActivationInput {
  jint kind = 0;
  jstring value = nullptr;
  jstring file_name = nullptr;
  jlong file_size = -1;
  jstring content_type = nullptr;
  jboolean writable = JNI_FALSE;
};

[[nodiscard]] ApplicationActivation DecodeAndroidApplicationActivation(
    JavaVM* virtual_machine,
    JNIEnv* environment,
    jobject context,
    const AndroidApplicationActivationInput& input
);

} // namespace huxerui::detail
