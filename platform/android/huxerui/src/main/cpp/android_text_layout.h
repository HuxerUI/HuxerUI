#pragma once

#include <jni.h>

#include <memory>

namespace huxerui::detail {

class TextLayout;

[[nodiscard]] std::unique_ptr<TextLayout>
CreateAndroidTextLayout(JavaVM* virtual_machine, JNIEnv* environment, jobject layout);

} // namespace huxerui::detail
