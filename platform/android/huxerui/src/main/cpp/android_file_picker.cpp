#include "android_file_internal.h"

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <huxerui/android/jni.h>
#include <huxerui/file_drop.h>

#include "file_internal.h"

namespace huxerui::detail {

namespace {

enum class AndroidReferenceResult : jint {
  Bytes,
  True,
  False,
  Error,
  Canceled,
  Directory,
};

enum class AndroidFileError : jint {
  NotFound,
  PermissionDenied,
  TooLarge,
  Io,
  NotDirectory,
  IsDirectory,
  Unsupported,
  AlreadyExists,
};

class JniEnvironment final {
public:
  explicit JniEnvironment(JavaVM* virtual_machine) : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr) {
      return;
    }
    if (virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment_), JNI_VERSION_1_6) == JNI_OK) {
      return;
    }
    if (virtual_machine_->AttachCurrentThread(&environment_, nullptr) == JNI_OK) {
      attached_ = true;
    } else {
      environment_ = nullptr;
    }
  }

  JniEnvironment(const JniEnvironment&) = delete;
  JniEnvironment& operator=(const JniEnvironment&) = delete;

  ~JniEnvironment() {
    if (attached_) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  [[nodiscard]] JNIEnv* Get() const noexcept {
    return environment_;
  }

private:
  JavaVM* virtual_machine_ = nullptr;
  JNIEnv* environment_ = nullptr;
  bool attached_ = false;
};

void ClearJavaException(JNIEnv* environment) noexcept {
  if (environment != nullptr && environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

std::string JavaStringOrFallback(JNIEnv* environment, jstring value, std::string fallback) {
  if (value == nullptr) {
    return fallback;
  }
  try {
    std::string result = android::JavaStringToUtf8(environment, value);
    return result.empty() ? std::move(fallback) : result;
  } catch (...) {
    return fallback;
  }
}

FileErrorCode ToFileErrorCode(jint error_code) noexcept {
  switch (static_cast<AndroidFileError>(error_code)) {
  case AndroidFileError::NotFound:
    return FileErrorCode::NotFound;
  case AndroidFileError::PermissionDenied:
    return FileErrorCode::PermissionDenied;
  case AndroidFileError::TooLarge:
    return FileErrorCode::TooLarge;
  case AndroidFileError::Io:
    return FileErrorCode::Io;
  case AndroidFileError::NotDirectory:
    return FileErrorCode::NotDirectory;
  case AndroidFileError::IsDirectory:
    return FileErrorCode::IsDirectory;
  case AndroidFileError::Unsupported:
    return FileErrorCode::Unsupported;
  case AndroidFileError::AlreadyExists:
    return FileErrorCode::AlreadyExists;
  }
  return FileErrorCode::Io;
}

class AndroidFileReferenceBridge;

using AndroidDirectoryCompletion = std::function<void(JNIEnv*, jint, jint, jobjectArray, jlong, bool)>;

// Owns one Java operation across the JNI call that starts it. Completion and cancellation can race;
// detach callbacks under the mutex, then call Java outside it because cancel() can reenter nativeComplete.
class AndroidReferenceOperationControl final {
public:
  AndroidReferenceOperationControl(JavaVM* virtual_machine, jmethodID cancel, FileReferenceBytesCompletion completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), bytes_completion_(std::move(completion)) {}

  AndroidReferenceOperationControl(JavaVM* virtual_machine, jmethodID cancel, FileReferenceBoolCompletion completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), bool_completion_(std::move(completion)) {}

  AndroidReferenceOperationControl(JavaVM* virtual_machine, jmethodID cancel, AndroidDirectoryCompletion completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), directory_completion_(std::move(completion)) {}

  AndroidReferenceOperationControl(JavaVM* virtual_machine, jmethodID cancel,
                                   FileReferenceCompletion<std::uint64_t> completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), import_completion_(std::move(completion)) {}

  ~AndroidReferenceOperationControl() {
    DeleteOperation(TakeOperation());
  }

  bool SetOperation(JNIEnv* environment, jobject operation) {
    jobject retained = environment->NewGlobalRef(operation);
    if (retained == nullptr) {
      ClearJavaException(environment);
      return false;
    }
    std::scoped_lock lock(mutex_);
    if (finished_) {
      environment->DeleteGlobalRef(retained);
      return false;
    }
    operation_ = retained;
    return true;
  }

  void Complete(JNIEnv* environment, jint result, jint error_code, jbyteArray bytes, jstring message,
                jobjectArray references, jlong transferred, bool created) noexcept {
    jobject operation = nullptr;
    FileReferenceBytesCompletion bytes_completion;
    FileReferenceBoolCompletion bool_completion;
    AndroidDirectoryCompletion directory_completion;
    FileReferenceCompletion<std::uint64_t> import_completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      operation = std::exchange(operation_, nullptr);
      bytes_completion = std::move(bytes_completion_);
      bool_completion = std::move(bool_completion_);
      directory_completion = std::move(directory_completion_);
      import_completion = std::move(import_completion_);
    }
    if (operation != nullptr) {
      environment->DeleteGlobalRef(operation);
    }

    if (import_completion) {
      import_completion(static_cast<AndroidReferenceResult>(result) == AndroidReferenceResult::True && transferred >= 0
                            ? FileResult<std::uint64_t>(transferred)
                            : FileResult<std::uint64_t>(
                                  FileError{ToFileErrorCode(error_code), "HuxerUI external file import failed"}));
      return;
    }
    if (directory_completion) {
      directory_completion(environment, result, error_code, references, transferred, created);
      return;
    }
    if (bytes_completion) {
      try {
        if (static_cast<AndroidReferenceResult>(result) == AndroidReferenceResult::Bytes && bytes != nullptr) {
          bytes_completion(FileResult<Bytes>(android::JavaByteArrayToBytes(environment, bytes)));
          return;
        }
        bytes_completion(FileResult<Bytes>(FileError{
            ToFileErrorCode(error_code),
            JavaStringOrFallback(environment, message, "HuxerUI external file read failed"),
        }));
      } catch (...) {
        bytes_completion(FileResult<Bytes>(FileError{
            FileErrorCode::Io,
            "HuxerUI external file result could not be decoded",
        }));
      }
      return;
    }

    if (bool_completion) {
      bool_completion(static_cast<AndroidReferenceResult>(result) == AndroidReferenceResult::True);
    }
  }

  void Fail() noexcept {
    FileReferenceBytesCompletion bytes_completion;
    FileReferenceBoolCompletion bool_completion;
    AndroidDirectoryCompletion directory_completion;
    FileReferenceCompletion<std::uint64_t> import_completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      bytes_completion = std::move(bytes_completion_);
      bool_completion = std::move(bool_completion_);
      directory_completion = std::move(directory_completion_);
      import_completion = std::move(import_completion_);
    }
    if (import_completion) {
      import_completion(
          FileResult<std::uint64_t>(FileError{FileErrorCode::Io, "HuxerUI file import could not be started"}));
    } else if (directory_completion) {
      directory_completion(nullptr, 3, 3, nullptr, 0, false);
    } else if (bytes_completion) {
      bytes_completion(FileResult<Bytes>(FileError{
          FileErrorCode::Io,
          "HuxerUI Android external file operation could not be started",
      }));
    } else if (bool_completion) {
      bool_completion(false);
    }
  }

  void Cancel() noexcept {
    jobject operation = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      bytes_completion_ = {};
      bool_completion_ = {};
      directory_completion_ = {};
      import_completion_ = {};
      operation = std::exchange(operation_, nullptr);
    }
    if (operation == nullptr) {
      return;
    }
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get()) {
      environment->CallVoidMethod(operation, cancel_);
      ClearJavaException(environment);
      environment->DeleteGlobalRef(operation);
    }
  }

private:
  jobject TakeOperation() noexcept {
    std::scoped_lock lock(mutex_);
    return std::exchange(operation_, nullptr);
  }

  void DeleteOperation(jobject operation) noexcept {
    if (operation == nullptr) {
      return;
    }
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get()) {
      environment->DeleteGlobalRef(operation);
    }
  }

  JavaVM* virtual_machine_ = nullptr;
  jmethodID cancel_ = nullptr;
  std::mutex mutex_;
  jobject operation_ = nullptr;
  FileReferenceBytesCompletion bytes_completion_;
  FileReferenceBoolCompletion bool_completion_;
  AndroidDirectoryCompletion directory_completion_;
  FileReferenceCompletion<std::uint64_t> import_completion_;
  bool finished_ = false;
};

using AndroidReferenceControlHandle = std::shared_ptr<AndroidReferenceOperationControl>;

// Shared by references returned from selection and directory operations. Retained classes and cached
// IDs let worker callbacks decode the same Metadata[] representation without repeated JNI lookup.
// The Context and bridge outlive the picker whenever a reference or operation still holds them.
class AndroidFileReferenceBridge final {
public:
  AndroidFileReferenceBridge(JavaVM* virtual_machine, JNIEnv* environment, jobject context,
                            std::shared_ptr<void> retained_access = {})
      : virtual_machine_(virtual_machine), retained_access_(std::move(retained_access)) {
    if (virtual_machine_ == nullptr || environment == nullptr || context == nullptr) {
      throw std::invalid_argument("HuxerUI Android file reference requires a Java VM, JNI environment, and Context");
    }
    context_ = environment->NewGlobalRef(context);
    android::LocalRef<jclass> reference_class(environment, environment->FindClass("org/huxerui/HuxerUIFileReference"));
    android::LocalRef<jclass> operation_class(
        environment,
        environment->FindClass("org/huxerui/HuxerUIFileReference$Operation")
    );
    android::LocalRef<jclass> metadata_class(
        environment, environment->FindClass("org/huxerui/HuxerUIFileReference$Metadata"));
    android::LocalRef<jclass> uri_class(environment, environment->FindClass("android/net/Uri"));
    if (context_ == nullptr || !reference_class || !operation_class || !metadata_class || !uri_class ||
        environment->ExceptionCheck()) {
      ClearJavaException(environment);
      if (context_ != nullptr) {
        environment->DeleteGlobalRef(context_);
        context_ = nullptr;
      }
      throw std::runtime_error("HuxerUI Android file reference Java implementation is unavailable");
    }
    reference_class_ = static_cast<jclass>(environment->NewGlobalRef(reference_class.Get()));
    operation_class_ = static_cast<jclass>(environment->NewGlobalRef(operation_class.Get()));
    metadata_class_ = static_cast<jclass>(environment->NewGlobalRef(metadata_class.Get()));
    if (reference_class_ == nullptr || operation_class_ == nullptr || metadata_class_ == nullptr ||
        environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android file reference Java classes could not be retained");
    }
    constructor_ =
        environment->GetMethodID(reference_class_, "<init>", "(Landroid/content/Context;Ljava/lang/String;Z)V");
    prepare_directory_ = environment->GetMethodID(
        reference_class_, "prepareDirectory",
        "(JILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)"
        "Lorg/huxerui/HuxerUIFileReference$Operation;");
    identity_ = environment->GetMethodID(reference_class_, "identity", "()Ljava/lang/String;");
    prepare_read_ =
        environment->GetMethodID(reference_class_, "prepareRead", "(J)Lorg/huxerui/HuxerUIFileReference$Operation;");
    prepare_import_ = environment->GetMethodID(
        reference_class_,
        "prepareImport",
        "(JLjava/lang/String;Z)Lorg/huxerui/HuxerUIFileReference$Operation;"
    );
    prepare_replace_ = environment->GetMethodID(
        reference_class_,
        "prepareReplace",
        "(JLjava/lang/String;)Lorg/huxerui/HuxerUIFileReference$Operation;"
    );
    start_ = environment->GetMethodID(operation_class_, "start", "()V");
    cancel_ = environment->GetMethodID(operation_class_, "cancel", "()V");
    metadata_uri_ = environment->GetFieldID(metadata_class_, "uri", "Landroid/net/Uri;");
    metadata_name_ = environment->GetFieldID(metadata_class_, "name", "Ljava/lang/String;");
    metadata_size_ = environment->GetFieldID(metadata_class_, "size", "J");
    metadata_content_type_ = environment->GetFieldID(metadata_class_, "contentType", "Ljava/lang/String;");
    metadata_writable_ = environment->GetFieldID(metadata_class_, "writable", "Z");
    metadata_write_allowed_ = environment->GetFieldID(metadata_class_, "writeAllowed", "Z");
    uri_string_ = environment->GetMethodID(uri_class.Get(), "toString", "()Ljava/lang/String;");
    if (!metadata_uri_ || !metadata_name_ || !metadata_size_ || !metadata_content_type_ || !metadata_writable_ ||
        !metadata_write_allowed_ ||
        !uri_string_ || identity_ == nullptr || prepare_directory_ == nullptr || constructor_ == nullptr ||
        prepare_read_ == nullptr || prepare_import_ == nullptr || prepare_replace_ == nullptr || start_ == nullptr || cancel_ == nullptr ||
        environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android file reference Java methods do not match the platform backend");
    }
  }

  ~AndroidFileReferenceBridge() {
    JniEnvironment attached(virtual_machine_);
    Release(attached.Get());
  }

  [[nodiscard]] static std::vector<FileReference> DecodeReferences(
      const std::shared_ptr<AndroidFileReferenceBridge>& bridge, JNIEnv* environment, jobjectArray values);

  [[nodiscard]] jobject CreateReference(JNIEnv* environment, std::string_view uri, bool write_allowed) const {
    android::LocalRef<jstring> java_uri = android::Utf8ToJavaString(environment, uri);
    if (!java_uri) {
      throw std::runtime_error("HuxerUI Android file reference URI could not be allocated");
    }
    android::LocalRef<jobject> reference(environment,
                                         environment->NewObject(reference_class_, constructor_, context_,
                                                                java_uri.Get(), write_allowed ? JNI_TRUE : JNI_FALSE));
    if (!reference || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      throw std::runtime_error("HuxerUI Android file reference could not be created");
    }
    jobject retained = environment->NewGlobalRef(reference.Get());
    if (retained == nullptr) {
      ClearJavaException(environment);
      throw std::runtime_error("HuxerUI Android file reference could not be retained");
    }
    return retained;
  }

  [[nodiscard]] JavaVM* VirtualMachine() const noexcept {
    return virtual_machine_;
  }

  [[nodiscard]] jmethodID PrepareDirectory() const noexcept {
    return prepare_directory_;
  }
  [[nodiscard]] jmethodID Identity() const noexcept {
    return identity_;
  }

  [[nodiscard]] jmethodID PrepareRead() const noexcept {
    return prepare_read_;
  }

  [[nodiscard]] jmethodID PrepareImport() const noexcept {
    return prepare_import_;
  }

  [[nodiscard]] jmethodID PrepareReplace() const noexcept {
    return prepare_replace_;
  }

  [[nodiscard]] jmethodID Start() const noexcept {
    return start_;
  }

  [[nodiscard]] jmethodID Cancel() const noexcept {
    return cancel_;
  }

private:
  void Release(JNIEnv* environment) noexcept {
    if (environment == nullptr) {
      return;
    }
    if (metadata_class_ != nullptr) {
      environment->DeleteGlobalRef(metadata_class_);
      metadata_class_ = nullptr;
    }
    if (operation_class_ != nullptr) {
      environment->DeleteGlobalRef(operation_class_);
      operation_class_ = nullptr;
    }
    if (reference_class_ != nullptr) {
      environment->DeleteGlobalRef(reference_class_);
      reference_class_ = nullptr;
    }
    if (context_ != nullptr) {
      environment->DeleteGlobalRef(context_);
      context_ = nullptr;
    }
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject context_ = nullptr;
  std::shared_ptr<void> retained_access_;
  jclass metadata_class_ = nullptr;
  jfieldID metadata_uri_ = nullptr;
  jfieldID metadata_name_ = nullptr;
  jfieldID metadata_size_ = nullptr;
  jfieldID metadata_content_type_ = nullptr;
  jfieldID metadata_writable_ = nullptr;
  jfieldID metadata_write_allowed_ = nullptr;
  jmethodID uri_string_ = nullptr;
  jclass reference_class_ = nullptr;
  jclass operation_class_ = nullptr;
  jmethodID constructor_ = nullptr;
  jmethodID identity_ = nullptr;
  jmethodID prepare_directory_ = nullptr;
  jmethodID prepare_read_ = nullptr;
  jmethodID prepare_import_ = nullptr;
  jmethodID prepare_replace_ = nullptr;
  jmethodID start_ = nullptr;
  jmethodID cancel_ = nullptr;
};

class AndroidFileReferenceState final : public FileReferenceState {
public:
  AndroidFileReferenceState(std::shared_ptr<AndroidFileReferenceBridge> bridge, JNIEnv* environment,
                            std::string_view uri, bool write_allowed)
      : bridge_(std::move(bridge)), uri_(uri), reference_(bridge_->CreateReference(environment, uri, write_allowed)) {
    android::LocalRef<jstring> identity(
        environment, static_cast<jstring>(environment->CallObjectMethod(reference_, bridge_->Identity())));
    if (!identity || environment->ExceptionCheck()) {
      environment->DeleteGlobalRef(reference_);
      ClearJavaException(environment);
      throw std::runtime_error("HuxerUI Android document identity is unavailable");
    }
    identity_ = android::JavaStringToUtf8(environment, identity.Get());
  }

  ~AndroidFileReferenceState() override {
    JniEnvironment attached(bridge_->VirtualMachine());
    if (JNIEnv* environment = attached.Get(); environment != nullptr && reference_ != nullptr) {
      environment->DeleteGlobalRef(reference_);
    }
  }

  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override {
    return Start(bridge_->PrepareRead(), nullptr, false, std::move(completion));
  }

  std::function<void()> ImportTo(File destination, bool overwrite,
                                 FileReferenceCompletion<std::uint64_t> completion) override {
    return Start(bridge_->PrepareImport(), &destination, overwrite, std::move(completion));
  }

  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override {
    return Start(bridge_->PrepareReplace(), &source, false, std::move(completion));
  }

  std::string Identity() const override {
    return identity_;
  }

  bool NeedsChildListingForLookup() const noexcept override { return true; }

  std::function<void()> ListChildren(FileReferenceCompletion<std::vector<FileReference>> completion) override {
    return Directory<std::vector<FileReference>>(
        list_children, {}, {}, {}, false, std::move(completion),
        [](std::vector<FileReference> references, jlong, bool, jint) { return references; });
  }

  std::function<void()> FindChild(std::string name,
                                  FileReferenceCompletion<std::optional<FileReference>> completion) override {
    return Directory<std::optional<FileReference>>(
        find_child, {}, {}, std::move(name), false, std::move(completion),
        [](std::vector<FileReference> references, jlong, bool, jint) -> std::optional<FileReference> {
          if (references.empty()) {
            return std::nullopt;
          }
          if (references.size() != 1) {
            throw std::logic_error("HuxerUI directory lookup is ambiguous");
          }
          return std::move(references.front());
        });
  }

  std::function<void()> CreateDirectory(std::string name, std::optional<FileReference> existing,
                                        FileReferenceCompletion<FileReferenceWriteResult> completion) override {
    return Directory<FileReferenceWriteResult>(create_directory, {}, {}, std::move(name), false,
                                               std::move(completion), DecodeWrite, std::move(existing));
  }

  std::function<void()> CopyFileFrom(FileReferenceSource source, std::string name, bool overwrite,
                                     std::optional<FileReference> existing,
                                     FileReferenceCompletion<FileReferenceWriteResult> completion) override {
    if (const auto* file = std::get_if<File>(&source)) {
      return Directory<FileReferenceWriteResult>(copy_file, file->Path(), {}, std::move(name), overwrite,
                                                 std::move(completion), DecodeWrite, std::move(existing));
    }
    if (auto input = std::dynamic_pointer_cast<AndroidFileReferenceState>(std::get<1>(source))) {
      return Directory<FileReferenceWriteResult>(copy_file, {}, input->uri_, std::move(name), overwrite,
                                                 std::move(completion), DecodeWrite, std::move(existing));
    }
    completion(FileResult<FileReferenceWriteResult>(
        FileError{FileErrorCode::Unsupported, "HuxerUI file source is unsupported"}));
    return {};
  }

  std::function<void()> CheckCopyDestination(FileReferenceSource destination,
                                             FileReferenceCompletion<bool> completion) override {
    std::string path;
    std::string uri;
    if (const auto* file = std::get_if<File>(&destination)) {
      path = file->Path();
    } else if (auto target = std::dynamic_pointer_cast<AndroidFileReferenceState>(std::get<1>(destination))) {
      uri = target->uri_;
    } else {
      completion(
          FileResult<bool>(FileError{FileErrorCode::Unsupported, "HuxerUI directory containment is unavailable"}));
      return {};
    }
    return Directory<bool>(check_destination, std::move(path), std::move(uri), {}, false, std::move(completion),
                           [](std::vector<FileReference>, jlong, bool, jint result) {
                             return static_cast<AndroidReferenceResult>(result) == AndroidReferenceResult::True;
                           });
  }

private:
  // Matches HuxerUIFileReference.Operation; read/import/replace use their dedicated Java entry points.
  static constexpr jint list_children = 3;
  static constexpr jint find_child = 4;
  static constexpr jint create_directory = 5;
  static constexpr jint copy_file = 6;
  static constexpr jint check_destination = 7;

  static FileReferenceWriteResult DecodeWrite(std::vector<FileReference> references, jlong bytes, bool created, jint) {
    if (references.size() != 1 || bytes < 0) {
      throw std::logic_error("HuxerUI file write result is invalid");
    }
    return {std::move(references.front()), static_cast<std::uint64_t>(bytes), created};
  }

  template <class T, class Decode>
  std::function<void()> Directory(jint kind, std::string path, std::string source_uri, std::string name, bool overwrite,
                                  FileReferenceCompletion<T> completion, Decode decode,
                                  std::optional<FileReference> existing = {}) {
    auto bridge = bridge_;
    AndroidDirectoryCompletion decoded = [bridge, completion = std::move(completion),
                                          decode](JNIEnv* environment, jint result, jint error, jobjectArray values,
                                                  jlong bytes, bool created) mutable {
      const auto outcome = static_cast<AndroidReferenceResult>(result);
      FileResult<T> output(FileError{outcome == AndroidReferenceResult::Error ? ToFileErrorCode(error) : FileErrorCode::Io,
                                     "HuxerUI external directory operation failed"});
      if (outcome == AndroidReferenceResult::Directory || outcome == AndroidReferenceResult::True ||
          outcome == AndroidReferenceResult::False) {
        try {
          auto references = AndroidFileReferenceBridge::DecodeReferences(bridge, environment, values);
          output = FileResult<T>(decode(std::move(references), bytes, created, result));
        } catch (...) {
          ClearJavaException(environment);
        }
      }
      completion(std::move(output));
    };
    return Start(std::move(decoded), [&](JNIEnv* environment, jlong handle) {
      auto java_path = android::Utf8ToJavaString(environment, path);
      auto java_source = android::Utf8ToJavaString(environment, source_uri);
      auto java_name = android::Utf8ToJavaString(environment, name);
      android::LocalRef<jstring> java_existing;
      if (existing) {
        auto state = std::dynamic_pointer_cast<AndroidFileReferenceState>(FileReferenceState::Of(*existing));
        if (!state) {
          throw std::logic_error("HuxerUI directory child belongs to another backend");
        }
        java_existing = android::Utf8ToJavaString(environment, state->uri_);
      }
      if (environment->ExceptionCheck()) {
        throw std::runtime_error("HuxerUI Android directory arguments could not be allocated");
      }
      return android::LocalRef<jobject>(
          environment, environment->CallObjectMethod(
                           reference_, bridge_->PrepareDirectory(), handle, kind,
                           path.empty() ? nullptr : java_path.Get(), source_uri.empty() ? nullptr : java_source.Get(),
                           name.empty() ? nullptr : java_name.Get(), java_existing.Get(),
                           overwrite ? JNI_TRUE : JNI_FALSE));
    });
  }

  template <class Completion>
  std::function<void()> Start(jmethodID prepare, const File* file, bool overwrite, Completion completion) {
    return Start(std::move(completion), [&](JNIEnv* environment, jlong handle) {
      if (!file) {
        return android::LocalRef<jobject>(environment, environment->CallObjectMethod(reference_, prepare, handle));
      }
      auto path = android::Utf8ToJavaString(environment, file->Path());
      if (!path || environment->ExceptionCheck()) {
        throw std::runtime_error("HuxerUI Android file path could not be allocated");
      }
      return android::LocalRef<jobject>(
          environment, prepare == bridge_->PrepareImport()
                           ? environment->CallObjectMethod(reference_, prepare, handle, path.Get(),
                                                           overwrite ? JNI_TRUE : JNI_FALSE)
                           : environment->CallObjectMethod(reference_, prepare, handle, path.Get()));
    });
  }

  template <class Completion, class Prepare>
  std::function<void()> Start(Completion completion, Prepare prepare) {
    // Prepare does not start Java work. Retain the operation and transfer its native callback holder
    // before start(), since execution can complete immediately (including executor rejection).
    auto control = std::make_shared<AndroidReferenceOperationControl>(bridge_->VirtualMachine(), bridge_->Cancel(),
                                                                      std::move(completion));
    JniEnvironment attached(bridge_->VirtualMachine());
    auto* environment = attached.Get();
    if (!environment) {
      control->Fail();
      return {};
    }
    try {
      auto native_handle = std::make_unique<AndroidReferenceControlHandle>(control);
      auto operation = prepare(environment, static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get())));
      if (!operation || environment->ExceptionCheck() || !control->SetOperation(environment, operation.Get())) {
        ClearJavaException(environment);
        control->Fail();
        return {};
      }
      native_handle.release();
      environment->CallVoidMethod(operation.Get(), bridge_->Start());
      if (environment->ExceptionCheck()) {
        ClearJavaException(environment);
        control->Cancel();
      }
      return [control] { control->Cancel(); };
    } catch (...) {
      ClearJavaException(environment);
      control->Fail();
      return {};
    }
  }

  std::shared_ptr<AndroidFileReferenceBridge> bridge_;
  std::string uri_;
  std::string identity_;
  jobject reference_ = nullptr;
};

std::vector<FileReference> AndroidFileReferenceBridge::DecodeReferences(
    const std::shared_ptr<AndroidFileReferenceBridge>& bridge, JNIEnv* environment, jobjectArray values) {
  // The URI stays in private state rather than becoming a File path. Each child keeps its own Java
  // reference and effective write restriction, independent of the parent's public value lifetime.
  std::vector<FileReference> references;
  const jsize count = values ? environment->GetArrayLength(values) : 0;
  references.reserve(static_cast<std::size_t>(count));
  for (jsize index = 0; index < count; ++index) {
    android::LocalRef<jobject> value(environment, environment->GetObjectArrayElement(values, index));
    if (!value || environment->ExceptionCheck() || !environment->IsInstanceOf(value.Get(), bridge->metadata_class_)) {
      throw std::runtime_error("HuxerUI Android file metadata is invalid");
    }
    android::LocalRef<jobject> uri(environment, environment->GetObjectField(value.Get(), bridge->metadata_uri_));
    android::LocalRef<jstring> name(
        environment, static_cast<jstring>(environment->GetObjectField(value.Get(), bridge->metadata_name_)));
    android::LocalRef<jstring> content_type(
        environment, static_cast<jstring>(environment->GetObjectField(value.Get(), bridge->metadata_content_type_)));
    const jlong size = environment->GetLongField(value.Get(), bridge->metadata_size_);
    const bool writable = environment->GetBooleanField(value.Get(), bridge->metadata_writable_) == JNI_TRUE;
    const bool write_allowed = environment->GetBooleanField(value.Get(), bridge->metadata_write_allowed_) == JNI_TRUE;
    if (!uri || !name || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android file metadata is incomplete");
    }
    android::LocalRef<jstring> uri_text(
        environment, static_cast<jstring>(environment->CallObjectMethod(uri.Get(), bridge->uri_string_)));
    if (!uri_text || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android file URI is invalid");
    }
    const auto uri_value = android::JavaStringToUtf8(environment, uri_text.Get());
    const auto type = content_type ? android::JavaStringToUtf8(environment, content_type.Get()) : std::string{};
    const bool directory = type == "vnd.android.document/directory";
    FileReferenceMetadata metadata{
        .name = android::JavaStringToUtf8(environment, name.Get()),
        .size = !directory && size >= 0 ? std::optional<std::uint64_t>(size) : std::nullopt,
        .content_type = !directory && !type.empty() ? std::optional<std::string>(type) : std::nullopt,
        .can_write = writable && write_allowed,
        .type = directory ? FileType::Directory : FileType::File};
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android file metadata could not be decoded");
    }
    auto state = std::make_shared<AndroidFileReferenceState>(bridge, environment, uri_value, write_allowed);
    references.push_back(MakeFileReference(std::move(metadata), std::move(state)));
  }
  return references;
}

android::LocalRef<jobjectArray>
MakeStringArray(JNIEnv* environment, jclass string_class, const std::vector<std::string>& values) {
  if (values.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android file picker filter count exceeds the JNI array range");
  }
  android::LocalRef<jobjectArray> result(
      environment,
      environment->NewObjectArray(static_cast<jsize>(values.size()), string_class, nullptr)
  );
  if (!result) {
    throw std::runtime_error("HuxerUI Android file picker filter array could not be allocated");
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    android::LocalRef<jstring> value = android::Utf8ToJavaString(environment, values[index]);
    if (!value) {
      throw std::runtime_error("HuxerUI Android file picker filter value could not be allocated");
    }
    environment->SetObjectArrayElement(result.Get(), static_cast<jsize>(index), value.Get());
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android file picker filter value could not be copied");
    }
  }
  return result;
}

class AndroidPickerOperationControl final {
public:
  AndroidPickerOperationControl(
      JavaVM* virtual_machine,
      jmethodID cancel,
      std::shared_ptr<AndroidFileReferenceBridge> bridge,
      FilePickerOpenCompletion completion
  )
      : virtual_machine_(virtual_machine), cancel_(cancel), bridge_(std::move(bridge)),
        open_completion_(std::move(completion)) {}

  AndroidPickerOperationControl(JavaVM* virtual_machine, jmethodID cancel, FilePickerSaveCompletion completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), save_completion_(std::move(completion)) {}

  ~AndroidPickerOperationControl() {
    DeleteOperation(TakeOperation());
  }

  bool SetOperation(JNIEnv* environment, jobject operation) {
    jobject retained = environment->NewGlobalRef(operation);
    if (retained == nullptr) {
      ClearJavaException(environment);
      return false;
    }
    std::scoped_lock lock(mutex_);
    if (finished_) {
      environment->DeleteGlobalRef(retained);
      return false;
    }
    operation_ = retained;
    return true;
  }

  void Complete(JNIEnv* environment, bool saved, jobjectArray values) noexcept {
    jobject operation = nullptr;
    FilePickerOpenCompletion open_completion;
    FilePickerSaveCompletion save_completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      operation = std::exchange(operation_, nullptr);
      open_completion = std::move(open_completion_);
      save_completion = std::move(save_completion_);
    }
    if (operation != nullptr) {
      environment->DeleteGlobalRef(operation);
    }
    if (save_completion) {
      save_completion(saved);
      return;
    }
    if (!open_completion) {
      return;
    }

    std::vector<FileReference> references;
    try {
      references = AndroidFileReferenceBridge::DecodeReferences(bridge_, environment, values);
    } catch (...) {
      ClearJavaException(environment);
      references.clear();
    }
    open_completion(std::move(references));
  }

  void Fail() noexcept {
    FilePickerOpenCompletion open_completion;
    FilePickerSaveCompletion save_completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      open_completion = std::move(open_completion_);
      save_completion = std::move(save_completion_);
    }
    if (open_completion) {
      open_completion({});
    } else if (save_completion) {
      save_completion(false);
    }
  }

  void Cancel() noexcept {
    jobject operation = nullptr;
    FilePickerOpenCompletion open_completion;
    FilePickerSaveCompletion save_completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      open_completion = std::move(open_completion_);
      save_completion = std::move(save_completion_);
      operation = std::exchange(operation_, nullptr);
    }
    if (operation != nullptr) {
      JniEnvironment attached(virtual_machine_);
      if (JNIEnv* environment = attached.Get()) {
        environment->CallVoidMethod(operation, cancel_);
        ClearJavaException(environment);
        environment->DeleteGlobalRef(operation);
      }
    }
    if (open_completion) {
      open_completion({});
    } else if (save_completion) {
      save_completion(false);
    }
  }

private:
  jobject TakeOperation() noexcept {
    std::scoped_lock lock(mutex_);
    return std::exchange(operation_, nullptr);
  }

  void DeleteOperation(jobject operation) noexcept {
    if (operation == nullptr) {
      return;
    }
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get()) {
      environment->DeleteGlobalRef(operation);
    }
  }

  JavaVM* virtual_machine_ = nullptr;
  jmethodID cancel_ = nullptr;
  std::shared_ptr<AndroidFileReferenceBridge> bridge_;
  std::mutex mutex_;
  jobject operation_ = nullptr;
  FilePickerOpenCompletion open_completion_;
  FilePickerSaveCompletion save_completion_;
  bool finished_ = false;
};

using AndroidPickerControlHandle = std::shared_ptr<AndroidPickerOperationControl>;

class AndroidDropAccess final {
public:
  AndroidDropAccess(JavaVM* virtual_machine, JNIEnv* environment, jobject access)
      : virtual_machine_(virtual_machine), access_(environment->NewGlobalRef(access)) {
    android::LocalRef<jclass> type(environment, environment->GetObjectClass(access));
    close_ = type ? environment->GetMethodID(type.Get(), "close", "()V") : nullptr;
    if (!access_ || !close_ || environment->ExceptionCheck()) {
      if (access_) {
        environment->DeleteGlobalRef(access_);
      }
      throw std::runtime_error("HuxerUI Android drop grant could not be retained");
    }
  }

  ~AndroidDropAccess() {
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get()) {
      android::LocalRef<jthrowable> pending(environment, environment->ExceptionOccurred());
      environment->ExceptionClear();
      environment->CallVoidMethod(access_, close_);
      ClearJavaException(environment);
      environment->DeleteGlobalRef(access_);
      if (pending) {
        environment->Throw(pending.Get());
      }
    }
  }

private:
  JavaVM* virtual_machine_;
  jobject access_;
  jmethodID close_ = nullptr;
};

class AndroidDropCapture final : public std::enable_shared_from_this<AndroidDropCapture> {
public:
  AndroidDropCapture(JNIEnv* environment, jobject operation) {
    if (!operation || environment->GetJavaVM(&virtual_machine_) != JNI_OK) {
      throw std::invalid_argument("HuxerUI Android drop requires an operation and Java VM");
    }
    android::LocalRef<jclass> type(environment, environment->GetObjectClass(operation));
    if (!type) {
      throw std::runtime_error("HuxerUI Android drop operation is unavailable");
    }
    const jfieldID context_field = environment->GetFieldID(type.Get(), "context", "Landroid/content/Context;");
    const jfieldID access_field = environment->GetFieldID(type.Get(), "access", "Lorg/huxerui/HuxerUIFileDrop$Access;");
    native_handle_ = environment->GetFieldID(type.Get(), "nativeHandle", "J");
    start_ = environment->GetMethodID(type.Get(), "start", "()V");
    cancel_ = environment->GetMethodID(type.Get(), "cancel", "()V");
    if (!context_field || !access_field || !native_handle_ || !start_ || !cancel_ || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android drop operation does not match the native backend");
    }
    android::LocalRef<jobject> context(environment, environment->GetObjectField(operation, context_field));
    android::LocalRef<jobject> access(environment, environment->GetObjectField(operation, access_field));
    if (!context || !access || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android drop has no retained access grant");
    }
    bridge_ = std::make_shared<AndroidFileReferenceBridge>(
        virtual_machine_, environment, context.Get(),
        std::make_shared<AndroidDropAccess>(virtual_machine_, environment, access.Get())
    );
    operation_ = environment->NewGlobalRef(operation);
    if (!operation_) {
      throw std::runtime_error("HuxerUI Android drop operation could not be retained");
    }
  }

  ~AndroidDropCapture() {
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get(); environment && operation_) {
      environment->DeleteGlobalRef(operation_);
    }
  }

  std::function<void()> Start(FileDropCompletion completion) {
    completion_ = std::move(completion);
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (!environment) {
      Complete(nullptr, nullptr, static_cast<jint>(AndroidFileError::Io));
      return {};
    }
    auto token = std::make_unique<std::weak_ptr<AndroidDropCapture>>(shared_from_this());
    environment->SetLongField(operation_, native_handle_,
                              static_cast<jlong>(reinterpret_cast<std::uintptr_t>(token.get())));
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Complete(environment, nullptr, static_cast<jint>(AndroidFileError::Io));
      return {};
    }
    // Java now owns the one-shot holder, including cancellation before worker submission.
    token.release();
    environment->CallVoidMethod(operation_, start_);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Complete(environment, nullptr, static_cast<jint>(AndroidFileError::Io));
      Cancel();
    }
    return [self = shared_from_this()] { self->Cancel(); };
  }

  void Complete(JNIEnv* environment, jobjectArray references, jint error_code) noexcept {
    FileDropCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      completion = std::move(completion_);
    }
    if (!completion) {
      return;
    }
    auto result = FileResult<std::vector<FileReference>>(
        FileError{ToFileErrorCode(error_code), "HuxerUI could not prepare the complete Android dropped file batch"}
    );
    if (environment && references) {
      try {
        result = FileResult<std::vector<FileReference>>(
            AndroidFileReferenceBridge::DecodeReferences(bridge_, environment, references)
        );
      } catch (...) {
        ClearJavaException(environment);
      }
    }
    completion(std::move(result));
  }

  void Cancel() {
    {
      std::scoped_lock lock(mutex_);
      completion_ = {};
    }
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get()) {
      environment->CallVoidMethod(operation_, cancel_);
      ClearJavaException(environment);
    }
  }

private:
  JavaVM* virtual_machine_ = nullptr;
  jobject operation_ = nullptr;
  jfieldID native_handle_ = nullptr;
  jmethodID start_ = nullptr;
  jmethodID cancel_ = nullptr;
  std::shared_ptr<AndroidFileReferenceBridge> bridge_;
  std::mutex mutex_;
  FileDropCompletion completion_;
};

class AndroidFilePickerTransport final : public FilePickerTransport {
public:
  AndroidFilePickerTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view, jobject context)
      : virtual_machine_(virtual_machine),
        bridge_(std::make_shared<AndroidFileReferenceBridge>(virtual_machine, environment, context)) {
    if (virtual_machine_ == nullptr || environment == nullptr || view == nullptr) {
      throw std::invalid_argument("HuxerUI Android file picker requires a Java VM, JNI environment, and View");
    }
    view_ = environment->NewGlobalRef(view);
    android::LocalRef<jclass> view_class(environment, environment->GetObjectClass(view));
    android::LocalRef<jclass> operation_class(
        environment,
        environment->FindClass("org/huxerui/HuxerUIFilePicker$Operation")
    );
    android::LocalRef<jclass> string_class(environment, environment->FindClass("java/lang/String"));
    if (view_ == nullptr || !view_class || !operation_class || !string_class || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android file picker Java implementation is unavailable");
    }
    operation_class_ = static_cast<jclass>(environment->NewGlobalRef(operation_class.Get()));
    string_class_ = static_cast<jclass>(environment->NewGlobalRef(string_class.Get()));
    if (operation_class_ == nullptr || string_class_ == nullptr || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android file picker Java classes could not be retained");
    }
    can_open_files_ = environment->GetMethodID(view_class.Get(), "canOpenFiles", "()Z");
    can_save_files_ = environment->GetMethodID(view_class.Get(), "canSaveFiles", "()Z");
    prepare_directory_ = environment->GetMethodID(view_class.Get(), "prepareOpenDirectory",
                                                  "(JZ)Lorg/huxerui/HuxerUIFilePicker$Operation;");
    prepare_open_files_ = environment->GetMethodID(
        view_class.Get(),
        "prepareOpenFiles",
        "(J[Ljava/lang/String;[Ljava/lang/String;Z)Lorg/huxerui/HuxerUIFilePicker$Operation;"
    );
    prepare_save_file_ = environment->GetMethodID(
        view_class.Get(),
        "prepareSaveFile",
        "(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)"
        "Lorg/huxerui/HuxerUIFilePicker$Operation;"
    );
    start_ = environment->GetMethodID(operation_class_, "start", "()V");
    cancel_ = environment->GetMethodID(operation_class_, "cancel", "()V");
    if (prepare_directory_ == nullptr || can_open_files_ == nullptr || can_save_files_ == nullptr ||
        prepare_open_files_ == nullptr || prepare_save_file_ == nullptr || start_ == nullptr || cancel_ == nullptr ||
        environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android file picker Java methods do not match the platform backend");
    }
  }

  ~AndroidFilePickerTransport() override {
    JniEnvironment attached(virtual_machine_);
    Release(attached.Get());
  }

  [[nodiscard]] bool CanOpenFiles() const noexcept override {
    return Capability(can_open_files_);
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept override {
    return Capability(can_save_files_);
  }

  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr) {
      completion({});
      return {};
    }
    auto control =
        std::make_shared<AndroidPickerOperationControl>(virtual_machine_, cancel_, bridge_, std::move(completion));
    try {
      android::LocalRef<jobjectArray> extensions = MakeStringArray(environment, string_class_, filter.extensions);
      android::LocalRef<jobjectArray> content_types = MakeStringArray(environment, string_class_, filter.content_types);
      auto native_handle = std::make_unique<AndroidPickerControlHandle>(control);
      android::LocalRef<jobject> operation(
          environment,
          environment->CallObjectMethod(
              view_,
              prepare_open_files_,
              static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get())),
              extensions.Get(),
              content_types.Get(),
              multiple ? JNI_TRUE : JNI_FALSE
          )
      );
      if (!operation || environment->ExceptionCheck() || !control->SetOperation(environment, operation.Get())) {
        ClearJavaException(environment);
        control->Fail();
        return {};
      }
      native_handle.release();
      environment->CallVoidMethod(operation.Get(), start_);
      if (environment->ExceptionCheck()) {
        ClearJavaException(environment);
        control->Cancel();
      }
      return [control] { control->Cancel(); };
    } catch (...) {
      ClearJavaException(environment);
      control->Fail();
      return {};
    }
  }

  bool CanOpenDirectories(bool) const noexcept override {
    return CanOpenFiles();
  }

  std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    auto* environment = attached.Get();
    if (!environment) {
      completion({});
      return {};
    }
    auto control =
        std::make_shared<AndroidPickerOperationControl>(virtual_machine_, cancel_, bridge_, std::move(completion));
    auto native_handle = std::make_unique<AndroidPickerControlHandle>(control);
    android::LocalRef<jobject> operation(
        environment,
        environment->CallObjectMethod(view_, prepare_directory_,
                                      static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get())),
                                      writable ? JNI_TRUE : JNI_FALSE));
    if (!operation || environment->ExceptionCheck() || !control->SetOperation(environment, operation.Get())) {
      ClearJavaException(environment);
      control->Fail();
      return {};
    }
    native_handle.release();
    environment->CallVoidMethod(operation.Get(), start_);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      control->Cancel();
    }
    return [control] { control->Cancel(); };
  }

  std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr) {
      completion(false);
      return {};
    }
    auto control = std::make_shared<AndroidPickerOperationControl>(virtual_machine_, cancel_, std::move(completion));
    try {
      android::LocalRef<jstring> source_path = android::Utf8ToJavaString(environment, source.Path());
      const std::string suggested_name = options.suggested_name.empty() ? source.Name() : options.suggested_name;
      android::LocalRef<jstring> java_name = android::Utf8ToJavaString(environment, suggested_name);
      android::LocalRef<jobjectArray> extensions =
          MakeStringArray(environment, string_class_, options.filter.extensions);
      android::LocalRef<jobjectArray> content_types =
          MakeStringArray(environment, string_class_, options.filter.content_types);
      if (!source_path || !java_name) {
        throw std::runtime_error("HuxerUI Android file picker save values could not be allocated");
      }
      auto native_handle = std::make_unique<AndroidPickerControlHandle>(control);
      android::LocalRef<jobject> operation(
          environment,
          environment->CallObjectMethod(
              view_,
              prepare_save_file_,
              static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get())),
              source_path.Get(),
              java_name.Get(),
              extensions.Get(),
              content_types.Get()
          )
      );
      if (!operation || environment->ExceptionCheck() || !control->SetOperation(environment, operation.Get())) {
        ClearJavaException(environment);
        control->Fail();
        return {};
      }
      native_handle.release();
      environment->CallVoidMethod(operation.Get(), start_);
      if (environment->ExceptionCheck()) {
        ClearJavaException(environment);
        control->Cancel();
      }
      return [control] { control->Cancel(); };
    } catch (...) {
      ClearJavaException(environment);
      control->Fail();
      return {};
    }
  }

private:
  bool Capability(jmethodID method) const noexcept {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      return false;
    }
    const bool result = environment->CallBooleanMethod(view_, method) == JNI_TRUE;
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      return false;
    }
    return result;
  }

  void Release(JNIEnv* environment) noexcept {
    if (environment == nullptr) {
      return;
    }
    if (string_class_ != nullptr) {
      environment->DeleteGlobalRef(string_class_);
      string_class_ = nullptr;
    }
    if (operation_class_ != nullptr) {
      environment->DeleteGlobalRef(operation_class_);
      operation_class_ = nullptr;
    }
    if (view_ != nullptr) {
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
    }
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject view_ = nullptr;
  jclass operation_class_ = nullptr;
  jclass string_class_ = nullptr;
  jmethodID prepare_directory_ = nullptr;
  jmethodID can_open_files_ = nullptr;
  jmethodID can_save_files_ = nullptr;
  jmethodID prepare_open_files_ = nullptr;
  jmethodID prepare_save_file_ = nullptr;
  jmethodID start_ = nullptr;
  jmethodID cancel_ = nullptr;
  std::shared_ptr<AndroidFileReferenceBridge> bridge_;
};

} // namespace

FileReference CreateAndroidFileReference(
    JavaVM* virtual_machine, JNIEnv* environment, jobject context, FileReferenceMetadata metadata, std::string_view uri
) {
  auto bridge = std::make_shared<AndroidFileReferenceBridge>(virtual_machine, environment, context);
  auto state = std::make_shared<AndroidFileReferenceState>(bridge, environment, uri, metadata.can_write);
  return MakeFileReference(std::move(metadata), std::move(state));
}

FileDropPreparation CaptureAndroidFileDrop(JNIEnv* environment, jobject operation) {
  auto captured = std::make_shared<AndroidDropCapture>(environment, operation);
  return {[captured](FileDropCompletion completion) { return captured->Start(std::move(completion)); }};
}

std::shared_ptr<FilePickerTransport>
CreateAndroidFilePickerTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view, jobject context) {
  return std::make_shared<AndroidFilePickerTransport>(virtual_machine, environment, view, context);
}

} // namespace huxerui::detail

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIFileReference_nativeComplete(
    JNIEnv* environment, jclass, jlong native_handle, jint result, jint error_code, jbyteArray bytes, jstring message,
    jobjectArray references, jlong transferred, jboolean created) {
  // Java's terminal completion consumes this holder exactly once, even after native cancellation.
  // The control may already be detached; destruction here still releases the callback's ownership.
  using Handle = std::shared_ptr<huxerui::detail::AndroidReferenceOperationControl>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (owner && *owner) {
    (*owner)->Complete(environment, result, error_code, bytes, message, references, transferred, created == JNI_TRUE);
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIFilePicker_nativeComplete(
    JNIEnv* environment, jclass, jlong native_handle, jboolean saved, jobjectArray references) {
  // Selection and export use the same one-shot ownership handoff as reference operations above.
  using Handle = std::shared_ptr<huxerui::detail::AndroidPickerOperationControl>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (owner && *owner) {
    (*owner)->Complete(environment, saved == JNI_TRUE, references);
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIFileDrop_nativeComplete(
    JNIEnv* environment, jclass, jlong native_handle, jobjectArray references, jint error_code) {
  using Handle = std::weak_ptr<huxerui::detail::AndroidDropCapture>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (owner) {
    if (const auto active = owner->lock()) {
      active->Complete(environment, references, error_code);
    }
  }
}
