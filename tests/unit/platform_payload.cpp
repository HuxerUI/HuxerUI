#include <catch2/catch_amalgamated.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/platform_registry.h>

#include "external_texture_test_support.h"

namespace huxerui::test {
namespace {

Bytes WireBytes(std::initializer_list<std::uint8_t> values) {
  Bytes bytes;
  bytes.reserve(values.size());
  for (std::uint8_t value : values) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

TEST_CASE("PlatformPayloadPreservesSupportedKinds") {
  const PlatformPayload payload = PlatformPayload::Object{
      {"boolean", true},
      {"integer", std::int64_t{42}},
      {"double", 2.5},
      {"string", "value"},
      {"bytes", Bytes{std::byte{1}, std::byte{2}}},
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

TEST_CASE("PlatformPayloadRetainsExternalTextureIdentity") {
  const std::shared_ptr<ExternalTexture> texture = MakeTestExternalTexture({320.0F, 180.0F});
  const std::shared_ptr<ExternalTexture> other = MakeTestExternalTexture({320.0F, 180.0F});
  const PlatformPayload payload = PlatformPayload::Object{
      {"preview", texture},
      {"nested", PlatformPayload::List{texture}},
  };

  const PlatformPayload& preview = payload.AsObject().at("preview");
  REQUIRE(preview.Kind() == PlatformPayloadKind::ExternalTexture);
  REQUIRE(preview.AsExternalTexture() == texture);
  REQUIRE(payload.AsObject().at("nested").AsList().front().AsExternalTexture() == texture);
  REQUIRE(PlatformPayload(texture) == PlatformPayload(texture));
  REQUIRE(PlatformPayload(texture) != PlatformPayload(other));
  REQUIRE_THROWS_AS(PlatformPayload(std::shared_ptr<ExternalTexture>{}), std::invalid_argument);
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
  REQUIRE_THROWS_AS(payload.AsExternalTexture(), std::bad_variant_access);
  REQUIRE_THROWS_AS(PlatformPayload{}.AsString(), std::bad_variant_access);
}

TEST_CASE("PlatformPayloadEnvelopeRoundTripsEveryValueKind") {
  const std::shared_ptr<ExternalTexture> texture = MakeTestExternalTexture({320.0F, 180.0F});
  const PlatformPayload payload = PlatformPayload::Object{
      {"boolean", true},
      {"integer", std::numeric_limits<std::int64_t>::min()},
      {"double", -12.5},
      {"string", "value"},
      {"bytes", Bytes{std::byte{1}, std::byte{2}}},
      {"list", PlatformPayload::List{nullptr, texture}},
      {"texture", texture},
  };

  std::vector<std::shared_ptr<ExternalTexture>> external_textures;
  const Bytes encoded = payload.Encode(external_textures);

  REQUIRE(external_textures == std::vector<std::shared_ptr<ExternalTexture>>{texture});
  REQUIRE(PlatformPayload::Decode(encoded, external_textures) == payload);
}

TEST_CASE("PlatformPayloadEnvelopeHasStableWireValues") {
  const std::shared_ptr<ExternalTexture> texture = MakeTestExternalTexture({320.0F, 180.0F});
  const PlatformPayload payload = PlatformPayload::List{
      nullptr,
      true,
      std::int64_t{-2},
      1.5,
      "x",
      Bytes{std::byte{0xAA}},
      PlatformPayload::List{},
      PlatformPayload::Object{},
      texture,
  };
  const Bytes expected = WireBytes({
      0x48, 0x55, 0x58, 0x50, 0x01, 0x00, 0x00, 0x00,
      0x06, 0x09, 0x00, 0x00, 0x00,
      0x00,
      0x01, 0x01,
      0x02, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F,
      0x04, 0x01, 0x00, 0x00, 0x00, 0x78,
      0x05, 0x01, 0x00, 0x00, 0x00, 0xAA,
      0x06, 0x00, 0x00, 0x00, 0x00,
      0x07, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x01, 0x00, 0x00, 0x00, 0x00,
  });
  std::vector<std::shared_ptr<ExternalTexture>> external_textures;

  REQUIRE(payload.Encode(external_textures) == expected);
  REQUIRE(external_textures == std::vector<std::shared_ptr<ExternalTexture>>{texture});
  REQUIRE(PlatformPayload::Decode(expected, external_textures) == payload);
}

TEST_CASE("PlatformPayloadEnvelopeIsCanonical") {
  const PlatformPayload payload = PlatformPayload::Object{{"second", 2}, {"first", 1}};
  std::vector<std::shared_ptr<ExternalTexture>> external_textures;
  const Bytes first = payload.Encode(external_textures);
  const Bytes second = PlatformPayload(PlatformPayload::Object{{"first", 1}, {"second", 2}}).Encode(external_textures);

  REQUIRE(first == second);
}

TEST_CASE("PlatformPayloadEnvelopeRejectsMalformedInput") {
  std::vector<std::shared_ptr<ExternalTexture>> external_textures;
  const Bytes valid = PlatformPayload("value").Encode(external_textures);
  Bytes truncated = valid;
  truncated.pop_back();
  Bytes trailing = valid;
  trailing.push_back(std::byte{0});
  Bytes invalid_header = valid;
  invalid_header[0] = std::byte{0};
  Bytes unsupported_version = valid;
  unsupported_version[4] = std::byte{2};
  Bytes unsupported_flags = valid;
  unsupported_flags[6] = std::byte{1};

  REQUIRE_THROWS_AS(PlatformPayload::Decode({}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(truncated), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(trailing), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(invalid_header), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(unsupported_version), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(unsupported_flags), std::invalid_argument);
}

TEST_CASE("PlatformPayloadEnvelopeRequiresExternalTextureCapabilities") {
  const std::shared_ptr<ExternalTexture> texture = MakeTestExternalTexture({320.0F, 180.0F});
  std::vector<std::shared_ptr<ExternalTexture>> external_textures;
  const Bytes encoded = PlatformPayload(texture).Encode(external_textures);
  const std::vector<std::shared_ptr<ExternalTexture>> duplicate_textures{texture, texture};

  REQUIRE_THROWS_AS(PlatformPayload::Decode(encoded), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(encoded, duplicate_textures), std::invalid_argument);
}

} // namespace
} // namespace huxerui::test
