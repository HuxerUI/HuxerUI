#include <huxerui/validation.h>

#include <cctype>
#include <utility>

#include "huxerui_builtin_resources.h"

namespace huxerui {

Required::Required() : message_(strings::validation_required) {}

Required::Required(StringVariant message) : message_(std::move(message)) {}

ValidationResult Required::operator()(std::string_view value) const {
  for (const char character : value) {
    if (std::isspace(static_cast<unsigned char>(character)) == 0) {
      return ValidationResult::Valid();
    }
  }
  return ValidationResult::Invalid(message_);
}

EmailAddress::EmailAddress() : message_(strings::validation_email) {}

EmailAddress::EmailAddress(StringVariant message) : message_(std::move(message)) {}

ValidationResult EmailAddress::operator()(std::string_view value) const {
  if (value.empty()) {
    return ValidationResult::Valid();
  }
  const std::size_t separator = value.find('@');
  if (separator == 0 || separator == std::string_view::npos || separator + 1 == value.size() ||
      value.find('@', separator + 1) != std::string_view::npos) {
    return ValidationResult::Invalid(message_);
  }
  for (const char character : value) {
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      return ValidationResult::Invalid(message_);
    }
  }
  return ValidationResult::Valid();
}

} // namespace huxerui
