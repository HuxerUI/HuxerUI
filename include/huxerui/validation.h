#pragma once

#include <cctype>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace huxerui {

enum class ValidationStatus {
  None,
  Valid,
  Invalid,
  Pending,
};

struct ValidationResult {
  ValidationStatus status = ValidationStatus::None;
  std::string message;

  static ValidationResult None() {
    return {};
  }

  static ValidationResult Valid() {
    return {
        ValidationStatus::Valid,
        {},
    };
  }

  static ValidationResult Invalid(std::string message) {
    return {
        ValidationStatus::Invalid,
        std::move(message),
    };
  }

  static ValidationResult Pending(std::string message = {}) {
    return {
        ValidationStatus::Pending,
        std::move(message),
    };
  }

  [[nodiscard]] bool IsInvalid() const noexcept {
    return status == ValidationStatus::Invalid;
  }

  bool operator==(const ValidationResult&) const = default;
};

class Required {
public:
  explicit Required(std::string message = "This field is required") : message_(std::move(message)) {}

  ValidationResult operator()(std::string_view value) const {
    for (const char character : value) {
      if (std::isspace(static_cast<unsigned char>(character)) == 0) {
        return ValidationResult::Valid();
      }
    }
    return ValidationResult::Invalid(message_);
  }

private:
  std::string message_;
};

class EmailAddress {
public:
  explicit EmailAddress(std::string message = "Enter a valid email address") : message_(std::move(message)) {}

  ValidationResult operator()(std::string_view value) const {
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

private:
  std::string message_;
};

template <class... Rules> ValidationResult Validate(std::string_view value, Rules&&... rules) {
  ValidationResult result = ValidationResult::Valid();
  const auto apply = [&](auto&& rule) {
    if (result.status == ValidationStatus::Valid) {
      result = std::invoke(std::forward<decltype(rule)>(rule), value);
    }
  };
  (apply(std::forward<Rules>(rules)), ...);
  return result;
}

} // namespace huxerui
