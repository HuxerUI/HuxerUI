#pragma once

#include <jni.h>

#include <memory>

namespace huxerui::detail {

class TextLayoutBackend;

[[nodiscard]] std::unique_ptr<TextLayoutBackend>
CreateAndroidTextLayout(JavaVM* virtual_machine, JNIEnv* environment, jobject layout);
// Borrows the Java layout from the retained paragraph for the duration of replay.
jobject AndroidTextLayoutObject(const TextLayoutBackend& layout) noexcept;

} // namespace huxerui::detail
