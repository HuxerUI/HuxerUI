#include <catch2/catch_amalgamated.hpp>

#include <huxerui/validation.h>

namespace huxerui::test {

TEST_CASE("TestValidationRulesReturnFirstNonValidResult") {
  int calls = 0;
  const ValidationResult result = Validate("", Required("Required"), [&calls](std::string_view) {
    ++calls;
    return ValidationResult::Invalid("Later");
  });

  REQUIRE(result == ValidationResult::Invalid("Required"));
  REQUIRE(calls == 0);
}

TEST_CASE("TestRequiredValidationRejectsEmptyAndWhitespaceContent") {
  REQUIRE(Validate("", Required()).IsInvalid());
  REQUIRE(Validate(" \t\n", Required()).IsInvalid());
  REQUIRE(Validate(" value ", Required()) == ValidationResult::Valid());
  REQUIRE(Validate("你好", Required()) == ValidationResult::Valid());
}

TEST_CASE("TestEmailAddressValidationChecksBasicAddressStructure") {
  REQUIRE(Validate("", EmailAddress()) == ValidationResult::Valid());
  REQUIRE(Validate("person@example.com", EmailAddress()) == ValidationResult::Valid());
  REQUIRE(Validate("person@localhost", EmailAddress()) == ValidationResult::Valid());
  REQUIRE(Validate("person", EmailAddress()).IsInvalid());
  REQUIRE(Validate("@example.com", EmailAddress()).IsInvalid());
  REQUIRE(Validate("person@", EmailAddress()).IsInvalid());
  REQUIRE(Validate("person@@example.com", EmailAddress()).IsInvalid());
  REQUIRE(Validate("person @example.com", EmailAddress()).IsInvalid());
}

TEST_CASE("TestValidationResultRepresentsPendingState") {
  REQUIRE(ValidationResult::None().status == ValidationStatus::None);

  const ValidationResult result = ValidationResult::Pending("Checking");
  REQUIRE(result.status == ValidationStatus::Pending);
  REQUIRE(result == ValidationResult::Pending("Checking"));
  REQUIRE_FALSE(result.IsInvalid());
}

} // namespace huxerui::test
