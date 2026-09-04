#pragma once

#include <jni.h>

#include <memory>
#include <string_view>

#include <huxerui/file_drop.h>

namespace huxerui {
class FileReference;
class FileSystem;
} // namespace huxerui

namespace huxerui::detail {

class FilePickerTransport;
struct FileReferenceMetadata;

[[nodiscard]] FileDropPreparation CaptureAndroidFileDrop(JNIEnv* environment, jobject operation);

[[nodiscard]] std::shared_ptr<FileSystem> CreateAndroidFileSystem(JNIEnv* environment, jobject context);
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateAndroidFilePickerTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view, jobject context);
[[nodiscard]] FileReference CreateAndroidFileReference(
    JavaVM* virtual_machine, JNIEnv* environment, jobject context, FileReferenceMetadata metadata, std::string_view uri
);

} // namespace huxerui::detail
