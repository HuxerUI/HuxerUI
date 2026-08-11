#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <utility>

#include <huxerui/resource.h>

namespace huxerui {

enum class ValidationStatus {
  None,
  Valid,
  Invalid,
  Pending,
};

struct ValidationResult {
  ValidationStatus status = ValidationStatus::None;
  StringVariant message;

  static ValidationResult None() {
    return {};
  }

  static ValidationResult Valid() {
    return {
        ValidationStatus::Valid,
        {},
    };
  }

  static ValidationResult Invalid(StringVariant message) {
    return {
        ValidationStatus::Invalid,
        std::move(message),
    };
  }

  static ValidationResult Pending(StringVariant message = {}) {
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
  Required();
  explicit Required(StringVariant message);

  ValidationResult operator()(std::string_view value) const;

private:
  StringVariant message_;
};

class EmailAddress {
public:
  EmailAddress();
  explicit EmailAddress(StringVariant message);

  ValidationResult operator()(std::string_view value) const;

private:
  StringVariant message_;
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
