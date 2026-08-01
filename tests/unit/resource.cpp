#include <catch2/catch_amalgamated.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <huxerui/resource.h>

#include "image_test_support.h"
#include "resource_internal.h"

namespace {

using namespace huxerui;

static_assert(std::is_convertible_v<ImageResource, ResourceId>);
static_assert(std::is_convertible_v<StringResource, ResourceId>);
static_assert(std::is_convertible_v<RawResource, ResourceId>);
static_assert(!std::is_convertible_v<ImageResource, StringResource>);
static_assert(!std::is_convertible_v<ImageResource, RawResource>);
static_assert(!std::is_convertible_v<StringResource, ImageResource>);
static_assert(!std::is_convertible_v<StringResource, RawResource>);
static_assert(!std::is_convertible_v<RawResource, ImageResource>);
static_assert(!std::is_convertible_v<RawResource, StringResource>);

void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void AppendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
  AppendU32(bytes, static_cast<std::uint32_t>(value));
  AppendU32(bytes, static_cast<std::uint32_t>(value >> 32U));
}

void AppendString(std::vector<std::byte>& bytes, std::string_view value) {
  AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
  for (char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
}

std::vector<std::byte> MakeIndex() {
  std::vector<std::byte> bytes{
      std::byte{'H'},
      std::byte{'U'},
      std::byte{'X'},
      std::byte{'R'},
      std::byte{'E'},
      std::byte{'S'},
      std::byte{0},
      std::byte{0},
  };
  AppendU32(bytes, 1);
  AppendU32(bytes, 1);
  bytes.push_back(std::byte{1});
  AppendString(bytes, "app");
  AppendString(bytes, "raw/config");
  AppendString(bytes, "huxerui/app/raw/config.bin");
  AppendString(bytes, "application/octet-stream");
  AppendString(bytes, "");
  AppendString(bytes, "");
  AppendU32(bytes, std::bit_cast<std::uint32_t>(1.0F));
  AppendU32(bytes, 0);
  AppendU32(bytes, 0);
  AppendU64(bytes, 42);
  AppendU32(bytes, 0);
  return bytes;
}

TEST_CASE("ResourceIdsValidateAndCompareByValue") {
  REQUIRE(ResourceId("app", "images/logo") == ResourceId("app", "images/logo"));
  REQUIRE(ResourceId("app", "images/logo").ToString() == "app:images/logo");
  REQUIRE_THROWS_AS(ResourceId("", "images/logo"), std::invalid_argument);
  REQUIRE_THROWS_AS(ResourceId("app", "../logo"), std::invalid_argument);
  REQUIRE_THROWS_AS(ResourceId("app", "images/./logo"), std::invalid_argument);
  REQUIRE_THROWS_AS(ResourceId("app", "images/logo:dark"), std::invalid_argument);
  REQUIRE_THROWS_AS(ResourceId("app", "images/logo\"dark"), std::invalid_argument);
  REQUIRE_THROWS_AS(ResourceId("app", "images/logo\ndark"), std::invalid_argument);
}

TEST_CASE("LocaleNormalizesCommonLanguageTags") {
  REQUIRE(Locale::FromLanguageTag("ZH_hans_cn").LanguageTag() == "zh-Hans-CN");
  REQUIRE(Locale::Default().LanguageTag() == "en");
  REQUIRE_THROWS_AS(Locale::FromLanguageTag("zh--CN"), std::invalid_argument);
}

TEST_CASE("RawAssetSharesOwnedBytes") {
  RawAsset asset = RawAsset::FromBytes({std::byte{1}, std::byte{2}}, "application/test");
  RawAsset copy = asset;
  REQUIRE(copy == asset);
  REQUIRE(copy.Bytes().size() == 2);
  REQUIRE(copy.MimeType() == "application/test");
}

TEST_CASE("RawAssetDistinguishesMissingStorageFromZeroLengthBytes") {
  REQUIRE_FALSE(RawAsset{}.HasValue());

  const RawAsset empty = RawAsset::FromBytes({});
  REQUIRE(empty.HasValue());
  REQUIRE(empty.Bytes().empty());
}

TEST_CASE("RawAssetExposesExplicitStringViewsAndCopies") {
  RawAsset asset = RawAsset::FromBytes({std::byte{'a'}, std::byte{0}, std::byte{'b'}}, "text/plain");
  const std::string_view view = asset.AsStringView();
  REQUIRE(view.size() == 3);
  REQUIRE(view[0] == 'a');
  REQUIRE(view[1] == '\0');
  REQUIRE(view[2] == 'b');

  const std::string copy = asset.ToString();
  asset = {};
  REQUIRE(copy.size() == 3);
  REQUIRE(copy[1] == '\0');

  REQUIRE(RawAsset{}.AsStringView().empty());
  REQUIRE(RawAsset{}.ToString().empty());
}

TEST_CASE("ImageAssetReadsEncodedMetadataWithoutDecoding") {
  ImageAsset image = ImageAsset::FromEncoded(huxerui::test::MakeTestPng(48, 24), 2.0F);
  REQUIRE(image.HasValue());
  REQUIRE_FALSE(ImageAsset{}.HasValue());
  REQUIRE(image.Format() == ImageFormat::Png);
  REQUIRE(image.PixelWidth() == 48);
  REQUIRE(image.PixelHeight() == 24);
  REQUIRE(image.IntrinsicSize() == Size{24.0F, 12.0F});
  REQUIRE(image.MimeType() == "image/png");
}

TEST_CASE("ImageAssetRejectsTruncatedEncodedImages") {
  std::vector<std::byte> truncated = huxerui::test::MakeTestPng(2, 2);
  truncated.resize(24);
  REQUIRE_THROWS_AS(ImageAsset::FromEncoded(std::move(truncated)), std::invalid_argument);
}

TEST_CASE("ResourceIndexRejectsUnsupportedVersions") {
  std::vector<std::byte> bytes = MakeIndex();
  bytes[8] = std::byte{2};
  REQUIRE_THROWS_WITH(
      huxerui::detail::ParseResourceIndex(RawAsset::FromBytes(std::move(bytes))),
      "HuxerUI resource index version is unsupported: 2"
  );
}

TEST_CASE("ResourceIndexReadsTypedEntries") {
  const auto entries = huxerui::detail::ParseResourceIndex(RawAsset::FromBytes(MakeIndex()));
  REQUIRE(entries.size() == 1);
  REQUIRE(entries.front().kind == huxerui::detail::ResourceEntryKind::Raw);
  REQUIRE(entries.front().id == ResourceId("app", "raw/config"));
  REQUIRE(entries.front().package_path == "huxerui/app/raw/config.bin");
  REQUIRE(entries.front().content_hash == 42);
}

TEST_CASE("ResourcePackagePathsRejectTraversalByComponent") {
  REQUIRE(huxerui::detail::IsValidResourcePackagePath("huxerui/app/raw/config..json"));
  REQUIRE_FALSE(huxerui::detail::IsValidResourcePackagePath("../config.json"));
  REQUIRE_FALSE(huxerui::detail::IsValidResourcePackagePath("huxerui/../config.json"));
  REQUIRE_FALSE(huxerui::detail::IsValidResourcePackagePath("huxerui//config.json"));
  REQUIRE_FALSE(huxerui::detail::IsValidResourcePackagePath("huxerui/app:C/raw/config.json"));
  REQUIRE_FALSE(huxerui::detail::IsValidResourcePackagePath("huxerui/app/raw/config\"dark.json"));
  REQUIRE_FALSE(huxerui::detail::IsValidResourcePackagePath("huxerui/app/raw/config\ndark.json"));
}

} // namespace
