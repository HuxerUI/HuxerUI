#include "android_application_internal.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/android/jni.h>

#include "android_file_internal.h"
#include "file_internal.h"

namespace huxerui::detail {

namespace {

constexpr jint android_activation_url = 1;
constexpr jint android_activation_file = 2;

std::optional<std::string> OptionalString(JNIEnv* environment, jstring value) {
  if (!value) {
    return std::nullopt;
  }
  return android::JavaStringToUtf8(environment, value);
}

} // namespace

std::optional<ApplicationActivation> DecodeAndroidApplicationActivation(
    JavaVM* virtual_machine,
    JNIEnv* environment,
    jobject context,
    const AndroidApplicationActivationInput& input
) {
  if (environment == nullptr || context == nullptr) {
    throw std::invalid_argument("HuxerUI Android application activation requires JNI and Context");
  }
  if (input.kind == 0) {
    return std::nullopt;
  }
  const std::optional<std::string> decoded_value = OptionalString(environment, input.value);
  if (environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android application activation value could not be read");
  }
  if (!decoded_value.has_value() || decoded_value->empty()) {
    return std::nullopt;
  }
  if (input.kind == android_activation_url) {
    std::optional<Uri> parsed = Uri::Parse(*decoded_value);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    return ApplicationActivation{UrlActivation{std::move(*parsed)}};
  }
  if (input.kind != android_activation_file) {
    return std::nullopt;
  }

  const std::optional<std::string> decoded_name = OptionalString(environment, input.file_name);
  const std::optional<std::string> decoded_content_type = OptionalString(environment, input.content_type);
  if (environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android file activation metadata could not be read");
  }
  if (!decoded_name.has_value() || decoded_name->empty() || input.file_size < -1) {
    return std::nullopt;
  }

  FileReferenceMetadata metadata{
      .name = *decoded_name,
      .size = input.file_size < 0
                  ? std::nullopt
                  : std::optional<std::uint64_t>{static_cast<std::uint64_t>(input.file_size)},
      .content_type = decoded_content_type,
      .can_write = input.writable == JNI_TRUE,
  };
  std::vector<FileReference> files;
  files.push_back(
      CreateAndroidFileReference(virtual_machine, environment, context, std::move(metadata), *decoded_value)
  );
  return ApplicationActivation{FileActivation{std::move(files)}};
}

} // namespace huxerui::detail
