#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <huxerui/semantics.h>

#include "android_accessibility.h"

namespace huxerui::detail {

namespace {

std::uint32_t ReadUint32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  REQUIRE(offset + sizeof(std::uint32_t) <= bytes.size());
  std::uint32_t result = 0;
  for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
    result |= static_cast<std::uint32_t>(bytes[offset + byte]) << (byte * 8U);
  }
  return result;
}

std::uint64_t ReadUint64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  REQUIRE(offset + sizeof(std::uint64_t) <= bytes.size());
  std::uint64_t result = 0;
  for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
    result |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
  }
  return result;
}

} // namespace

TEST_CASE("Android semantic frames use direct checked virtual view ids") {
  SemanticNode root;
  root.children = {27};
  SemanticNode button;
  button.id = 27;
  button.parent = 0;
  button.role = SemanticRole::Button;
  button.label = "保存";
  button.actions = SemanticActionMask(SemanticActionKind::Activate);
  button.bounds = {4.0F, 8.0F, 80.0F, 40.0F};
  SemanticFrame frame{42, 0, {root, button}};

  const std::vector<std::uint8_t> encoded = EncodeAndroidSemanticFrame(frame);
  REQUIRE(ReadUint32(encoded, 0) == android_semantics_magic);
  REQUIRE(ReadUint32(encoded, 4) == android_semantics_version);
  REQUIRE(ReadUint32(encoded, 16) == 0);
  REQUIRE(ReadUint32(encoded, 20) == 2);
  REQUIRE(ReadUint32(encoded, 24) == 0);
  REQUIRE(encoded.at(32) == 0U);
  REQUIRE(ReadUint32(encoded, 33) == 0U);
  REQUIRE(EncodeAndroidSemanticFrame(frame) == encoded);

  const std::string utf8_label = "保存";
  const std::vector<std::uint8_t> expected_label(utf8_label.begin(), utf8_label.end());
  REQUIRE(std::search(encoded.begin(), encoded.end(), expected_label.begin(), expected_label.end()) != encoded.end());
}

TEST_CASE("Android semantic frames encode the ComboBox role") {
  SemanticNode select;
  select.role = SemanticRole::ComboBox;

  const std::vector<std::uint8_t> encoded = EncodeAndroidSemanticFrame({1, 0, {select}});
  REQUIRE(ReadUint32(encoded, 33) == 24U);
}

TEST_CASE("Android semantic frames preserve disabled nodes and their enabled flag") {
  SemanticNode node;
  node.id = 27;
  node.role = SemanticRole::GridCell;
  node.label = "Unavailable date";
  node.enabled = false;

  const std::vector<std::uint8_t> disabled = EncodeAndroidSemanticFrame({1, 27, {node}});
  REQUIRE(ReadUint32(disabled, 20) == 1U);
  REQUIRE(ReadUint32(disabled, 24) == 27U);
  REQUIRE(ReadUint64(disabled, 41) == 0U);
  REQUIRE((disabled.at(49) & 1U) == 0U);

  node.enabled = true;
  const std::vector<std::uint8_t> enabled = EncodeAndroidSemanticFrame({2, 27, {node}});
  REQUIRE(ReadUint32(enabled, 24) == 27U);
  REQUIRE((enabled.at(49) & 1U) != 0U);
}

TEST_CASE("Android semantic frames encode PlatformView anchors without changing the protocol version") {
  SemanticNode root;
  root.platform_view_identity = 0x0102030405060708ULL;

  const std::vector<std::uint8_t> encoded = EncodeAndroidSemanticFrame({7, 0, {root}});
  REQUIRE(ReadUint32(encoded, 4) == 1U);
  REQUIRE(encoded.at(32) == 1U);
  REQUIRE(ReadUint64(encoded, 33) == *root.platform_view_identity);
}

TEST_CASE("Android semantic frame encoding rejects identities outside jint") {
  SemanticNode root;
  root.children = {static_cast<SemanticNodeId>(std::numeric_limits<std::int32_t>::max()) + 1U};
  SemanticNode child;
  child.id = root.children.front();
  child.parent = 0;

  REQUIRE_THROWS_AS(EncodeAndroidSemanticFrame({1, 0, {root, child}}), std::overflow_error);
}

TEST_CASE("Android semantic frame encoding rejects PlatformView identities outside Java long") {
  SemanticNode root;
  root.platform_view_identity = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;

  REQUIRE_THROWS_AS(EncodeAndroidSemanticFrame({1, 0, {root}}), std::overflow_error);
}

} // namespace huxerui::detail
