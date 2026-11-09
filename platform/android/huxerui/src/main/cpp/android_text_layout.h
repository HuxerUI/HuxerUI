#pragma once

#include <jni.h>

#include <memory>

namespace huxerui::detail {

class TextLayoutBackend;

[[nodiscard]] std::unique_ptr<TextLayoutBackend>
CreateAndroidTextLayout(JavaVM* virtual_machine, JNIEnv* environment, jobject layout);

} // namespace huxerui::detail
