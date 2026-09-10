#pragma once

#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <huxerui/file_drop.h>

namespace huxerui {
class FileReference;
} // namespace huxerui

namespace huxerui::detail {

class FilePickerTransport;
struct FileReferenceMetadata;

// PlatformPayload wrappers expose Android metadata but retain the original FileReference for access and lifetime.
// The capability key is a process-local comparison key derived from FileReferenceState, never an owning handle.
struct AndroidFileReferenceProjection {
  std::string uri;
  std::uintptr_t capability_key = 0;
};

[[nodiscard]] FileDropPreparation CaptureAndroidFileDrop(JNIEnv* environment, jobject operation);

[[nodiscard]] AppDirectories CreateAndroidAppDirectories(JNIEnv* environment, jobject context);
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateAndroidFilePickerTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view, jobject context);
[[nodiscard]] FileReference CreateAndroidFileReference(
    JavaVM* virtual_machine, JNIEnv* environment, jobject context, FileReferenceMetadata metadata, std::string_view uri
);
[[nodiscard]] AndroidFileReferenceProjection ProjectAndroidFileReference(const FileReference& reference);

} // namespace huxerui::detail
