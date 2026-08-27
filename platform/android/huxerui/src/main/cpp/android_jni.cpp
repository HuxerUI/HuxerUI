#include <huxerui/android/jni.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace huxerui::android {

namespace {

std::u16string Utf8ToUtf16(std::string_view value) {
  std::u16string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<std::uint8_t>(value[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (first <= 0x7FU) {
      length = 1;
      code_point = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      code_point = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      code_point = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
    }
    if (index + length > value.size()) {
      throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<std::uint8_t>(value[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
    }
    if (code_point <= 0xFFFFU) {
      result.push_back(static_cast<char16_t>(code_point));
    } else {
      const std::uint32_t surrogate = code_point - 0x10000U;
      result.push_back(static_cast<char16_t>(0xD800U + (surrogate >> 10U)));
      result.push_back(static_cast<char16_t>(0xDC00U + (surrogate & 0x3FFU)));
    }
    index += length;
  }
  return result;
}

void AppendUtf8(std::string& result, std::uint32_t code_point) {
  if (code_point <= 0x7FU) {
    result.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
    result.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

std::string Utf16ToUtf8(std::u16string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    std::uint32_t code_point = value[index];
    if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
      if (index + 1 >= value.size()) {
        throw std::invalid_argument("HuxerUI Android Java string contains an unpaired UTF-16 surrogate");
      }
      const std::uint32_t low = value[++index];
      if (low < 0xDC00U || low > 0xDFFFU) {
        throw std::invalid_argument("HuxerUI Android Java string contains an unpaired UTF-16 surrogate");
      }
      code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
    } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
      throw std::invalid_argument("HuxerUI Android Java string contains an unpaired UTF-16 surrogate");
    }
    AppendUtf8(result, code_point);
  }
  return result;
}

void RequireEnvironment(JNIEnv* environment) {
  if (environment == nullptr) {
    throw std::invalid_argument("HuxerUI Android JNI environment must not be null");
  }
}

} // namespace

namespace detail {

void DeleteLocalReference(JNIEnv* environment, jobject reference) noexcept {
  if (environment != nullptr && reference != nullptr) {
    environment->DeleteLocalRef(reference);
  }
}

} // namespace detail

LocalRef<jstring> Utf8ToJavaString(JNIEnv* environment, std::string_view value) {
  RequireEnvironment(environment);
  const std::u16string utf16 = Utf8ToUtf16(value);
  if (utf16.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android Java string exceeds the JNI length range");
  }
  return LocalRef<jstring>(
      environment,
      environment->NewString(reinterpret_cast<const jchar*>(utf16.data()), static_cast<jsize>(utf16.size()))
  );
}

std::string JavaStringToUtf8(JNIEnv* environment, jstring value) {
  RequireEnvironment(environment);
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI Android Java string must not be null");
  }
  const jsize length = environment->GetStringLength(value);
  std::u16string utf16(static_cast<std::size_t>(length), u'\0');
  if (length > 0) {
    environment->GetStringRegion(value, 0, length, reinterpret_cast<jchar*>(utf16.data()));
  }
  return Utf16ToUtf8(utf16);
}

LocalRef<jbyteArray> BytesToJavaByteArray(JNIEnv* environment, std::span<const std::byte> value) {
  RequireEnvironment(environment);
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android byte array exceeds the JNI length range");
  }
  jbyteArray result = environment->NewByteArray(static_cast<jsize>(value.size()));
  if (result != nullptr && !value.empty()) {
    environment
        ->SetByteArrayRegion(result, 0, static_cast<jsize>(value.size()), reinterpret_cast<const jbyte*>(value.data()));
  }
  return LocalRef<jbyteArray>(environment, result);
}

Bytes JavaByteArrayToBytes(JNIEnv* environment, jbyteArray value) {
  RequireEnvironment(environment);
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI Android Java byte array must not be null");
  }
  const jsize length = environment->GetArrayLength(value);
  Bytes result(static_cast<std::size_t>(length));
  if (length > 0) {
    environment->GetByteArrayRegion(value, 0, length, reinterpret_cast<jbyte*>(result.data()));
  }
  return result;
}

} // namespace huxerui::android
