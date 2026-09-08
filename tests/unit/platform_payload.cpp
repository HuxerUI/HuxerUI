#include <catch2/catch_amalgamated.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/platform_registry.h>

#include "external_texture_test_support.h"
#include "io/file_internal.h"

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

class PayloadFileReferenceState final : public detail::FileReferenceState {
public:
  std::function<void()> ReadBytes(detail::FileReferenceBytesCompletion completion) override {
    completion(IoResult<Bytes>(Bytes{}));
    return {};
  }

  std::function<void()> ImportTo(File, bool, detail::FileReferenceCompletion<std::uint64_t> completion) override {
    completion(IoResult<std::uint64_t>(0));
    return {};
  }

  std::function<void()> ReplaceWith(File, detail::FileReferenceBoolCompletion completion) override {
    completion(false);
    return {};
  }
};

FileReference MakePayloadFileReference(const std::shared_ptr<PayloadFileReferenceState>& state,
                                       std::string name = "sample.mp4") {
  return detail::MakeFileReference(
      {.name = std::move(name), .size = 42, .content_type = "video/mp4", .can_write = true}, state);
}

TEST_CASE("PlatformPayloadTransportsBufferReferencesWithoutInlineBytes") {
  auto storage = std::make_shared<Bytes>(17U * 1024U * 1024U);
  BufferReference reference(*storage, storage);
  const PlatformPayload payload = PlatformPayload::List{reference, reference, reference.Slice(1, 2)};
  auto envelope = payload.Encode();
  REQUIRE(envelope.bytes.size() == 31);
  REQUIRE(envelope.buffer_references.size() == 2);
  REQUIRE(envelope.buffer_references[0] == reference);
  const auto decoded = PlatformPayload::Decode(envelope);
  REQUIRE(decoded == payload);
  REQUIRE(decoded.AsList()[0].AsBufferReference().AsBytes().data() == storage->data());
  (*storage)[0] = std::byte{23};
  REQUIRE(decoded.AsList()[0].AsBufferReference().AsBytes()[0] == std::byte{23});
  REQUIRE(detail::DecodePlatformPayload<BufferReference>(detail::EncodePlatformValue(reference)) == reference);
  STATIC_REQUIRE(detail::PlatformPayloadEncodable<BufferReference>);
  STATIC_REQUIRE(detail::PlatformPayloadDecodable<BufferReference>);
  const PlatformPayload empty = BufferReference{};
  REQUIRE_FALSE(empty.IsNull());
  REQUIRE(empty.Kind() == PlatformPayloadKind::BufferReference);
  REQUIRE(PlatformPayload::Decode(empty.Encode()) == empty);
  REQUIRE_THROWS_AS(empty.AsBytes(), std::bad_variant_access);
}

TEST_CASE("PlatformPayloadRequiresExactBufferCapabilityTables") {
  const PlatformPayload payload = BufferReference{};
  auto envelope = payload.Encode();
  REQUIRE(envelope.bytes == WireBytes({'H', 'U', 'X', 'P', 1, 0, 0, 0, 10, 3, 0, 0, 0, 0}));
  SECTION("Missing table") {
    envelope.buffer_references.clear();
  }
  SECTION("Duplicate slot") {
    envelope.buffer_references.push_back(envelope.buffer_references[0]);
  }
  SECTION("Unused slot") {
    envelope.buffer_references.emplace_back(std::span<const std::byte>{}, std::shared_ptr<const void>{});
  }
  SECTION("Wrong discriminator") {
    envelope.bytes[9] = std::byte{2};
  }
  SECTION("Missing slot") {
    envelope.bytes[10] = std::byte{1};
  }
  REQUIRE_THROWS_AS(PlatformPayload::Decode(envelope), std::invalid_argument);
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

TEST_CASE("PlatformPayloadRetainsFileReferenceCapabilityAndMetadata") {
  auto state = std::make_shared<PayloadFileReferenceState>();
  const FileReference reference = MakePayloadFileReference(state);
  const FileReference alias = MakePayloadFileReference(state, "alias.mp4");
  const FileReference other = MakePayloadFileReference(std::make_shared<PayloadFileReferenceState>());
  const PlatformPayload payload(reference);

  REQUIRE(payload.Kind() == PlatformPayloadKind::FileReference);
  REQUIRE(payload.AsFileReference().Name() == "sample.mp4");
  REQUIRE(payload.AsFileReference().Size() == 42);
  REQUIRE(payload.AsFileReference().ContentType() == "video/mp4");
  REQUIRE(payload.AsFileReference().CanWrite());
  REQUIRE(PlatformPayload(reference) == PlatformPayload(alias));
  REQUIRE(PlatformPayload(reference) != PlatformPayload(other));
  REQUIRE(detail::FileReferenceState::Of(detail::DecodePlatformPayload<FileReference>(payload)) == state);
  REQUIRE(detail::FileReferenceState::Of(detail::EncodePlatformValue(reference).AsFileReference()) == state);

  FileReference moved = reference;
  static_cast<void>(FileReference(std::move(moved)));
  REQUIRE_THROWS_AS(PlatformPayload(std::move(moved)), std::invalid_argument);
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
  REQUIRE_THROWS_AS(payload.AsFileReference(), std::bad_variant_access);
  REQUIRE_THROWS_AS(PlatformPayload{}.AsString(), std::bad_variant_access);
}

TEST_CASE("PlatformPayloadEnvelopeRoundTripsEveryValueKind") {
  const std::shared_ptr<ExternalTexture> texture = MakeTestExternalTexture({320.0F, 180.0F});
  const FileReference reference = MakePayloadFileReference(std::make_shared<PayloadFileReferenceState>());
  const PlatformPayload payload = PlatformPayload::Object{
      {"boolean", true},
      {"integer", std::numeric_limits<std::int64_t>::min()},
      {"double", -12.5},
      {"string", "value"},
      {"bytes", Bytes{std::byte{1}, std::byte{2}}},
      {"list", PlatformPayload::List{nullptr, texture, reference}},
      {"texture", texture},
      {"file", reference},
  };

  const PlatformPayload::Envelope envelope = payload.Encode();

  REQUIRE(envelope.external_textures == std::vector<std::shared_ptr<ExternalTexture>>{texture});
  REQUIRE(envelope.file_references.size() == 1);
  REQUIRE(detail::FileReferenceState::Of(envelope.file_references.front()) ==
          detail::FileReferenceState::Of(reference));
  REQUIRE(PlatformPayload::Decode(envelope) == payload);
}

TEST_CASE("PlatformPayloadEnvelopeHasStableWireValues") {
  const std::shared_ptr<ExternalTexture> texture = MakeTestExternalTexture({320.0F, 180.0F});
  const FileReference reference = MakePayloadFileReference(std::make_shared<PayloadFileReferenceState>());
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
      reference,
  };
  const Bytes expected = WireBytes({
      0x48, 0x55, 0x58, 0x50, 0x01, 0x00, 0x00, 0x00, 0x06, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02,
      0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F,
      0x04, 0x01, 0x00, 0x00, 0x00, 0x78, 0x05, 0x01, 0x00, 0x00, 0x00, 0xAA, 0x06, 0x00, 0x00, 0x00, 0x00,
      0x07, 0x00, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00,
  });
  const PlatformPayload::Envelope envelope = payload.Encode();

  REQUIRE(envelope.bytes == expected);
  REQUIRE(envelope.external_textures == std::vector<std::shared_ptr<ExternalTexture>>{texture});
  REQUIRE(envelope.file_references.size() == 1);
  REQUIRE(PlatformPayload::Decode(envelope) == payload);
}

TEST_CASE("PlatformPayloadEnvelopeIsCanonical") {
  const PlatformPayload payload = PlatformPayload::Object{{"second", 2}, {"first", 1}};
  const Bytes first = payload.Encode().bytes;
  const Bytes second = PlatformPayload(PlatformPayload::Object{{"first", 1}, {"second", 2}}).Encode().bytes;

  REQUIRE(first == second);
}

TEST_CASE("PlatformPayloadEnvelopeRejectsMalformedInput") {
  const Bytes valid = PlatformPayload("value").Encode().bytes;
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

  REQUIRE_THROWS_AS(PlatformPayload::Decode(PlatformPayload::Envelope{}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode({.bytes = truncated}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode({.bytes = trailing}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode({.bytes = invalid_header}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode({.bytes = unsupported_version}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode({.bytes = unsupported_flags}), std::invalid_argument);
}

TEST_CASE("PlatformPayloadEnvelopeRequiresExactCapabilityTables") {
  const std::shared_ptr<ExternalTexture> texture = MakeTestExternalTexture({320.0F, 180.0F});
  const FileReference reference = MakePayloadFileReference(std::make_shared<PayloadFileReferenceState>());
  const PlatformPayload::Envelope texture_envelope = PlatformPayload(texture).Encode();
  const PlatformPayload::Envelope reference_envelope = PlatformPayload(reference).Encode();
  const PlatformPayload::Envelope empty_envelope = PlatformPayload(nullptr).Encode();

  PlatformPayload::Envelope duplicate_textures = texture_envelope;
  duplicate_textures.external_textures.push_back(texture);
  PlatformPayload::Envelope duplicate_references = reference_envelope;
  duplicate_references.file_references.push_back(reference);
  PlatformPayload::Envelope unreferenced_texture = empty_envelope;
  unreferenced_texture.external_textures.push_back(texture);
  PlatformPayload::Envelope unreferenced_reference = empty_envelope;
  unreferenced_reference.file_references.push_back(reference);

  REQUIRE_THROWS_AS(PlatformPayload::Decode({.bytes = texture_envelope.bytes}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(duplicate_textures), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode({.bytes = reference_envelope.bytes}), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(duplicate_references), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(unreferenced_texture), std::invalid_argument);
  REQUIRE_THROWS_AS(PlatformPayload::Decode(unreferenced_reference), std::invalid_argument);
}

} // namespace
} // namespace huxerui::test
