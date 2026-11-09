#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <huxerui/data.h>
#include <huxerui/file.h>

#if defined(__ANDROID__)
#include <jni.h>
#else
struct _JNIEnv;
using JNIEnv = _JNIEnv;
class _jobject;
using jobject = _jobject*;
class _jstring;
using jstring = _jstring*;
class _jbyteArray;
using jbyteArray = _jbyteArray*;
#endif

namespace huxerui {

class PlatformPayload;

namespace android {

namespace detail {

void DeleteLocalReference(JNIEnv* environment, jobject reference) noexcept;

} // namespace detail

template <class Reference> class LocalRef final {
  static_assert(std::is_pointer_v<Reference>);
#if defined(__ANDROID__)
  static_assert(std::is_convertible_v<Reference, jobject>);
#endif

public:
  LocalRef() noexcept = default;
  LocalRef(JNIEnv* environment, Reference reference) noexcept : environment_(environment), reference_(reference) {}

  LocalRef(const LocalRef&) = delete;
  LocalRef& operator=(const LocalRef&) = delete;

  LocalRef(LocalRef&& other) noexcept
      : environment_(std::exchange(other.environment_, nullptr)), reference_(std::exchange(other.reference_, nullptr)) {
  }

  LocalRef& operator=(LocalRef&& other) noexcept {
    if (this != &other) {
      Reset();
      environment_ = std::exchange(other.environment_, nullptr);
      reference_ = std::exchange(other.reference_, nullptr);
    }
    return *this;
  }

  ~LocalRef() {
    Reset();
  }

  Reference Get() const noexcept {
    return reference_;
  }

  explicit operator bool() const noexcept {
    return reference_ != nullptr;
  }

  [[nodiscard]] Reference Release() noexcept {
    return std::exchange(reference_, nullptr);
  }

  void Reset() noexcept {
    if (environment_ != nullptr && reference_ != nullptr) {
      detail::DeleteLocalReference(environment_, reinterpret_cast<jobject>(reference_));
    }
    reference_ = nullptr;
  }

private:
  JNIEnv* environment_ = nullptr;
  Reference reference_ = nullptr;
};

LocalRef<jstring> Utf8ToJavaString(JNIEnv* environment, std::string_view value);
std::string JavaStringToUtf8(JNIEnv* environment, jstring value);
LocalRef<jbyteArray> BytesToJavaByteArray(JNIEnv* environment, std::span<const std::byte> value);
Bytes JavaByteArrayToBytes(JNIEnv* environment, jbyteArray value);
LocalRef<jobject> PlatformPayloadToJava(JNIEnv* environment, const PlatformPayload& payload);
PlatformPayload JavaPlatformPayloadToCpp(JNIEnv* environment, jobject payload);

/// Owns an open Android document descriptor and retains the file reference's provider access.
/// Serialize use of the descriptor, including closing or moving its owner.
class FileDescriptor final {
public:
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&&) noexcept;
  FileDescriptor& operator=(FileDescriptor&&) noexcept;
  ~FileDescriptor();

  /// Returns a borrowed fd, or -1 after close or move. Pass it only to APIs that do not take ownership.
  int Get() const noexcept;

  /// Releases the descriptor and its provider access. Repeated calls succeed without closing another fd.
  /// @return Success or a provider close error. Destruction releases resources without reporting errors.
  IoResult<void> Close();

private:
  struct Impl;
  explicit FileDescriptor(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;

  friend IoResult<FileDescriptor> OpenSeekableFileDescriptor(const FileReference&, FileAccessMode, FileOpenMode);
};

/// Opens the whole file behind an existing Android content URI on the calling thread.
/// @param reference A provider-backed file reference whose original grant authorizes the operation.
/// @param access ReadOnly or ReadWrite. WriteOnly reports Unsupported.
/// @param mode OpenExisting preserves the file. TruncateExisting requires ReadWrite and validates the fd first.
/// @return An owning descriptor or an I/O error. Providers requiring reliable Java close signaling and
/// nonseekable descriptors report Unsupported. Use a worker when the provider may block.
/// @throws std::invalid_argument For invalid or incompatible modes, including CreateNew.
/// @throws std::logic_error If the file reference was moved from.
IoResult<FileDescriptor> OpenSeekableFileDescriptor(const FileReference& reference,
    FileAccessMode access = FileAccessMode::ReadOnly, FileOpenMode mode = FileOpenMode::OpenExisting);

} // namespace android
} // namespace huxerui
