#include <catch2/catch_amalgamated.hpp>

#include <string>

#include "linux_text_input_internal.h"

namespace huxerui::test {

TEST_CASE("LinuxTextInputMapsUtf8BytesToUtf16Offsets") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 0) == TextOffset{0});
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 1) == TextOffset{1});
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 4) == TextOffset{2});
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 8) == TextOffset{4});
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16(text, 2).has_value());
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16(text, 9).has_value());
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16(text, -1).has_value());
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16("\xFF", 1).has_value());
}

TEST_CASE("LinuxTextInputMapsUtf16OffsetsToUtf8Bytes") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 0) == 0);
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 1) == 1);
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 2) == 4);
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 4) == 8);
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte(text, 3).has_value());
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte(text, 5).has_value());
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte(text, -1).has_value());
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte("\xFF", 1).has_value());
}

TEST_CASE("LinuxTextInputMapsAdjacentGtkDeletionRanges") {
  REQUIRE(detail::ResolveLinuxDeleteSurrounding(-3, 3) == (detail::LinuxDeleteSurroundingPlan{3, 0}));
  REQUIRE(detail::ResolveLinuxDeleteSurrounding(0, 2) == (detail::LinuxDeleteSurroundingPlan{0, 2}));
  REQUIRE(detail::ResolveLinuxDeleteSurrounding(-2, 5) == (detail::LinuxDeleteSurroundingPlan{2, 3}));
  REQUIRE_FALSE(detail::ResolveLinuxDeleteSurrounding(-3, 2).has_value());
  REQUIRE_FALSE(detail::ResolveLinuxDeleteSurrounding(1, 2).has_value());
  REQUIRE_FALSE(detail::ResolveLinuxDeleteSurrounding(0, -1).has_value());
}

} // namespace huxerui::test
