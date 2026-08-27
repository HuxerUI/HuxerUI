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

#include "file_internal.h"

namespace huxerui::detail {

namespace {

enum class AndroidReferenceResult : jint {
  Bytes,
  True,
  False,
  Error,
  Canceled,
};

enum class AndroidFileError : jint {
  NotFound,
  PermissionDenied,
  TooLarge,
  Io,
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
  }
  return FileErrorCode::Io;
}

class AndroidFileReferenceBridge;

class AndroidReferenceOperationControl final {
public:
  AndroidReferenceOperationControl(JavaVM* virtual_machine, jmethodID cancel, FileReferenceBytesCompletion completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), bytes_completion_(std::move(completion)) {}

  AndroidReferenceOperationControl(JavaVM* virtual_machine, jmethodID cancel, FileReferenceBoolCompletion completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), bool_completion_(std::move(completion)) {}

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

  void Complete(JNIEnv* environment, jint result, jint error_code, jbyteArray bytes, jstring message) noexcept {
    jobject operation = nullptr;
    FileReferenceBytesCompletion bytes_completion;
    FileReferenceBoolCompletion bool_completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      operation = std::exchange(operation_, nullptr);
      bytes_completion = std::move(bytes_completion_);
      bool_completion = std::move(bool_completion_);
    }
    if (operation != nullptr) {
      environment->DeleteGlobalRef(operation);
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
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      bytes_completion = std::move(bytes_completion_);
      bool_completion = std::move(bool_completion_);
    }
    if (bytes_completion) {
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
  bool finished_ = false;
};

using AndroidReferenceControlHandle = std::shared_ptr<AndroidReferenceOperationControl>;

class AndroidFileReferenceBridge final : public std::enable_shared_from_this<AndroidFileReferenceBridge> {
public:
  AndroidFileReferenceBridge(JavaVM* virtual_machine, JNIEnv* environment, jobject context)
      : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr || environment == nullptr || context == nullptr) {
      throw std::invalid_argument("HuxerUI Android file reference requires a Java VM, JNI environment, and Context");
    }
    context_ = environment->NewGlobalRef(context);
    android::LocalRef<jclass> reference_class(environment, environment->FindClass("org/huxerui/HuxerUIFileReference"));
    android::LocalRef<jclass> operation_class(
        environment,
        environment->FindClass("org/huxerui/HuxerUIFileReference$Operation")
    );
    if (context_ == nullptr || !reference_class || !operation_class || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      if (context_ != nullptr) {
        environment->DeleteGlobalRef(context_);
        context_ = nullptr;
      }
      throw std::runtime_error("HuxerUI Android file reference Java implementation is unavailable");
    }
    reference_class_ = static_cast<jclass>(environment->NewGlobalRef(reference_class.Get()));
    operation_class_ = static_cast<jclass>(environment->NewGlobalRef(operation_class.Get()));
    if (reference_class_ == nullptr || operation_class_ == nullptr || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android file reference Java classes could not be retained");
    }
    constructor_ =
        environment->GetMethodID(reference_class_, "<init>", "(Landroid/content/Context;Ljava/lang/String;)V");
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
    if (constructor_ == nullptr || prepare_read_ == nullptr || prepare_import_ == nullptr ||
        prepare_replace_ == nullptr || start_ == nullptr || cancel_ == nullptr || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android file reference Java methods do not match the platform backend");
    }
  }

  ~AndroidFileReferenceBridge() {
    JniEnvironment attached(virtual_machine_);
    Release(attached.Get());
  }

  [[nodiscard]] jobject CreateReference(JNIEnv* environment, std::string_view uri) const {
    android::LocalRef<jstring> java_uri = android::Utf8ToJavaString(environment, uri);
    if (!java_uri) {
      throw std::runtime_error("HuxerUI Android file reference URI could not be allocated");
    }
    android::LocalRef<jobject> reference(
        environment,
        environment->NewObject(reference_class_, constructor_, context_, java_uri.Get())
    );
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
  jclass reference_class_ = nullptr;
  jclass operation_class_ = nullptr;
  jmethodID constructor_ = nullptr;
  jmethodID prepare_read_ = nullptr;
  jmethodID prepare_import_ = nullptr;
  jmethodID prepare_replace_ = nullptr;
  jmethodID start_ = nullptr;
  jmethodID cancel_ = nullptr;
};

class AndroidFileReferenceState final : public FileReferenceState {
public:
  AndroidFileReferenceState(
      std::shared_ptr<AndroidFileReferenceBridge> bridge, JNIEnv* environment, std::string_view uri
  )
      : bridge_(std::move(bridge)), reference_(bridge_->CreateReference(environment, uri)) {}

  ~AndroidFileReferenceState() override {
    JniEnvironment attached(bridge_->VirtualMachine());
    if (JNIEnv* environment = attached.Get(); environment != nullptr && reference_ != nullptr) {
      environment->DeleteGlobalRef(reference_);
    }
  }

  std::function<void()> ReadBytes(FileReferenceBytesCompletion completion) override {
    return Start(bridge_->PrepareRead(), nullptr, false, std::move(completion));
  }

  std::function<void()> ImportTo(File destination, bool overwrite, FileReferenceBoolCompletion completion) override {
    return Start(bridge_->PrepareImport(), &destination, overwrite, std::move(completion));
  }

  std::function<void()> ReplaceWith(File source, FileReferenceBoolCompletion completion) override {
    return Start(bridge_->PrepareReplace(), &source, false, std::move(completion));
  }

private:
  template <class Completion>
  std::function<void()> Start(jmethodID prepare, const File* file, bool overwrite, Completion completion) {
    JniEnvironment attached(bridge_->VirtualMachine());
    JNIEnv* environment = attached.Get();
    if (environment == nullptr) {
      if constexpr (std::is_same_v<Completion, FileReferenceBytesCompletion>) {
        completion(FileResult<Bytes>(FileError{
            FileErrorCode::Io,
            "HuxerUI Android external file operation could not access JNI",
        }));
      } else {
        completion(false);
      }
      return {};
    }

    auto control = std::make_shared<AndroidReferenceOperationControl>(
        bridge_->VirtualMachine(),
        bridge_->Cancel(),
        std::move(completion)
    );
    auto native_handle = std::make_unique<AndroidReferenceControlHandle>(control);
    android::LocalRef<jobject> operation;
    android::LocalRef<jstring> path;
    if (file == nullptr) {
      operation = android::LocalRef<jobject>(
          environment,
          environment->CallObjectMethod(
              reference_,
              prepare,
              static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get()))
          )
      );
    } else {
      path = android::Utf8ToJavaString(environment, file->Path());
      if (path) {
        if (prepare == bridge_->PrepareImport()) {
          operation = android::LocalRef<jobject>(
              environment,
              environment->CallObjectMethod(
                  reference_,
                  prepare,
                  static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get())),
                  path.Get(),
                  overwrite ? JNI_TRUE : JNI_FALSE
              )
          );
        } else {
          operation = android::LocalRef<jobject>(
              environment,
              environment->CallObjectMethod(
                  reference_,
                  prepare,
                  static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get())),
                  path.Get()
              )
          );
        }
      }
    }
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
  }

  std::shared_ptr<AndroidFileReferenceBridge> bridge_;
  jobject reference_ = nullptr;
};

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

  void Complete(
      JNIEnv* environment,
      bool saved,
      jobjectArray uris,
      jobjectArray names,
      jlongArray sizes,
      jobjectArray content_types,
      jbooleanArray writable
  ) noexcept {
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
      if (uris != nullptr || names != nullptr || sizes != nullptr || content_types != nullptr || writable != nullptr) {
        if (uris == nullptr || names == nullptr || sizes == nullptr || content_types == nullptr ||
            writable == nullptr) {
          throw std::runtime_error("HuxerUI Android file picker result arrays are incomplete");
        }
        const jsize count = environment->GetArrayLength(uris);
        if (environment->GetArrayLength(names) != count || environment->GetArrayLength(sizes) != count ||
            environment->GetArrayLength(content_types) != count || environment->GetArrayLength(writable) != count) {
          throw std::runtime_error("HuxerUI Android file picker result arrays have inconsistent lengths");
        }
        std::vector<jlong> size_values(static_cast<std::size_t>(count));
        std::vector<jboolean> writable_values(static_cast<std::size_t>(count));
        if (count != 0) {
          environment->GetLongArrayRegion(sizes, 0, count, size_values.data());
          environment->GetBooleanArrayRegion(writable, 0, count, writable_values.data());
        }
        if (environment->ExceptionCheck()) {
          throw std::runtime_error("HuxerUI Android file picker result arrays could not be read");
        }
        references.reserve(static_cast<std::size_t>(count));
        for (jsize index = 0; index < count; ++index) {
          android::LocalRef<jstring> uri(
              environment,
              static_cast<jstring>(environment->GetObjectArrayElement(uris, index))
          );
          android::LocalRef<jstring> name(
              environment,
              static_cast<jstring>(environment->GetObjectArrayElement(names, index))
          );
          android::LocalRef<jstring> content_type(
              environment,
              static_cast<jstring>(environment->GetObjectArrayElement(content_types, index))
          );
          if (!uri || !name || environment->ExceptionCheck()) {
            throw std::runtime_error("HuxerUI Android file picker result is invalid");
          }
          const std::string uri_value = android::JavaStringToUtf8(environment, uri.Get());
          FileReferenceMetadata metadata{
              .name = android::JavaStringToUtf8(environment, name.Get()),
              .size = size_values[static_cast<std::size_t>(index)] < 0
                          ? std::nullopt
                          : std::optional<std::uint64_t>{static_cast<std::uint64_t>(
                                size_values[static_cast<std::size_t>(index)]
                            )},
              .content_type =
                  content_type ? std::optional<std::string>{android::JavaStringToUtf8(environment, content_type.Get())}
                               : std::nullopt,
              .can_write = writable_values[static_cast<std::size_t>(index)] == JNI_TRUE,
          };
          auto state = std::make_shared<AndroidFileReferenceState>(bridge_, environment, uri_value);
          references.push_back(MakeFileReference(std::move(metadata), std::move(state)));
        }
      }
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
    if (can_open_files_ == nullptr || can_save_files_ == nullptr || prepare_open_files_ == nullptr ||
        prepare_save_file_ == nullptr || start_ == nullptr || cancel_ == nullptr || environment->ExceptionCheck()) {
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
  auto state = std::make_shared<AndroidFileReferenceState>(bridge, environment, uri);
  return MakeFileReference(std::move(metadata), std::move(state));
}

std::shared_ptr<FilePickerTransport>
CreateAndroidFilePickerTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view, jobject context) {
  return std::make_shared<AndroidFilePickerTransport>(virtual_machine, environment, view, context);
}

} // namespace huxerui::detail

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIFileReference_nativeComplete(
    JNIEnv* environment, jclass, jlong native_handle, jint result, jint error_code, jbyteArray bytes, jstring message
) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidReferenceOperationControl>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (owner && *owner) {
    (*owner)->Complete(environment, result, error_code, bytes, message);
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIFilePicker_nativeComplete(
    JNIEnv* environment,
    jclass,
    jlong native_handle,
    jboolean saved,
    jobjectArray uris,
    jobjectArray names,
    jlongArray sizes,
    jobjectArray content_types,
    jbooleanArray writable
) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidPickerOperationControl>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (owner && *owner) {
    (*owner)->Complete(environment, saved == JNI_TRUE, uris, names, sizes, content_types, writable);
  }
}
