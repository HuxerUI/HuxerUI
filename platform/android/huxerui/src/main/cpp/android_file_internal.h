#pragma once

#include <jni.h>

#include <memory>

namespace huxerui {
class FileSystem;
}

namespace huxerui::detail {

[[nodiscard]] std::shared_ptr<FileSystem> CreateAndroidFileSystem(JNIEnv* environment, jobject context);

} // namespace huxerui::detail
