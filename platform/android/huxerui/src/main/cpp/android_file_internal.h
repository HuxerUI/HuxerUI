#pragma once

#include <jni.h>

#include <memory>

namespace huxerui {
class FileSystem;
}

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] std::shared_ptr<FileSystem> CreateAndroidFileSystem(JNIEnv* environment, jobject context);
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateAndroidFilePickerTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view, jobject context);

} // namespace huxerui::detail
