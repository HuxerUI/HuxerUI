#include <catch2/catch_amalgamated.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <huxerui/platform_module.h>

namespace huxerui::test {
namespace {

TEST_CASE("PlatformPayloadPreservesSupportedKinds") {
  const PlatformPayload payload = PlatformPayload::Object{
      {"boolean", true},
      {"integer", std::int64_t{42}},
      {"double", 2.5},
      {"string", "value"},
      {"bytes", PlatformPayload::Bytes{std::byte{1}, std::byte{2}}},
      {"list", PlatformPayload::List{nullptr, false}},
  };

  REQUIRE(payload.Kind() == PlatformPayloadKind::Object);
  REQUIRE(payload.AsObject().at("boolean").AsBoolean());
  REQUIRE(payload.AsObject().at("integer").AsInteger() == 42);
  REQUIRE(payload.AsObject().at("double").AsDouble() == 2.5);
  REQUIRE(payload.AsObject().at("string").AsString() == "value");
  REQUIRE(payload.AsObject().at("bytes").AsBytes().size() == 2);
  REQUIRE(payload.AsObject().at("list").AsList()[0].IsNull());
}

TEST_CASE("PlatformPayloadObjectEqualityIgnoresInsertionOrder") {
  const PlatformPayload left = PlatformPayload::Object{{"first", 1}, {"second", 2}};
  const PlatformPayload right = PlatformPayload::Object{{"second", 2}, {"first", 1}};

  REQUIRE(left == right);
  REQUIRE(PlatformPayload(-0.0) == PlatformPayload(0.0));
  REQUIRE(PlatformPayload(std::int64_t{1}) != PlatformPayload(1.0));
}

TEST_CASE("PlatformPayloadRejectsInvalidScalars") {
  const std::string invalid_utf8{"\xF0\x28\x8C\x28", 4};

  REQUIRE_THROWS_AS(PlatformPayload(invalid_utf8), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload(PlatformPayload::Object{{invalid_utf8, 1}}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload(std::numeric_limits<double>::infinity()), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload(std::numeric_limits<std::uint64_t>::max()), std::invalid_argument);
}

TEST_CASE("PlatformPayloadRejectsExcessiveNesting") {
  PlatformPayload payload;
  for (std::size_t depth = 0; depth < 64; ++depth) {
    payload = PlatformPayload::List{std::move(payload)};
  }

  REQUIRE_THROWS_AS(PlatformPayload(PlatformPayload::List{std::move(payload)}), std::invalid_argument);
}

TEST_CASE("PlatformPayloadAccessorsRequireTheDeclaredKind") {
  const PlatformPayload payload = true;

  REQUIRE_THROWS_AS(payload.AsInteger(), std::bad_variant_access);
  REQUIRE_THROWS_AS(PlatformPayload{}.AsString(), std::bad_variant_access);
}

} // namespace
} // namespace huxerui::test
