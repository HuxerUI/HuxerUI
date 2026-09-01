#pragma once

#include <jni.h>

#include <memory>
#include <optional>

#include <huxerui/app.h>

namespace huxerui::detail {

class PermissionTransport;

// JNI object references are borrowed from the current native call and are decoded synchronously.
struct AndroidApplicationActivationInput {
  jint kind = 0;
  jstring value = nullptr;
  jstring file_name = nullptr;
  jlong file_size = -1;
  jstring content_type = nullptr;
  jboolean writable = JNI_FALSE;
};

[[nodiscard]] std::optional<ApplicationActivation> DecodeAndroidApplicationActivation(
    JavaVM* virtual_machine,
    JNIEnv* environment,
    jobject context,
    const AndroidApplicationActivationInput& input
);

[[nodiscard]] std::shared_ptr<PermissionTransport>
CreateAndroidPermissionTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view);

} // namespace huxerui::detail
