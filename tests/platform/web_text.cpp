#include <catch2/catch_amalgamated.hpp>

#include "web_text_internal.h"

using namespace huxerui;

TEST_CASE("WebTextDirectionUsesTheFirstStrongCharacter") {
  REQUIRE(detail::ResolveWebTextDirection("Hello", TextDirection::Auto) == TextDirection::LeftToRight);
  REQUIRE(detail::ResolveWebTextDirection("中文", TextDirection::Auto) == TextDirection::LeftToRight);
  REQUIRE(detail::ResolveWebTextDirection("עברית", TextDirection::Auto) == TextDirection::RightToLeft);
  REQUIRE(detail::ResolveWebTextDirection("العربية", TextDirection::Auto) == TextDirection::RightToLeft);
  REQUIRE(detail::ResolveWebTextDirection("🙂 العربية", TextDirection::Auto) == TextDirection::RightToLeft);
  REQUIRE(detail::ResolveWebTextDirection("123 English العربية", TextDirection::Auto) == TextDirection::LeftToRight);
}

TEST_CASE("WebTextDirectionHonorsAnExplicitDirection") {
  REQUIRE(detail::ResolveWebTextDirection("English", TextDirection::RightToLeft) == TextDirection::RightToLeft);
  REQUIRE(detail::ResolveWebTextDirection("العربية", TextDirection::LeftToRight) == TextDirection::LeftToRight);
}
