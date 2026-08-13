#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

namespace huxerui::android {

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

  [[nodiscard]] Reference Get() const noexcept {
    return reference_;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
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

[[nodiscard]] LocalRef<jstring> Utf8ToJavaString(JNIEnv* environment, std::string_view value);
[[nodiscard]] std::string JavaStringToUtf8(JNIEnv* environment, jstring value);
[[nodiscard]] LocalRef<jbyteArray> BytesToJavaByteArray(JNIEnv* environment, std::span<const std::byte> value);
[[nodiscard]] std::vector<std::byte> JavaByteArrayToBytes(JNIEnv* environment, jbyteArray value);

} // namespace huxerui::android
