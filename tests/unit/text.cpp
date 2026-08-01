#include <catch2/catch_amalgamated.hpp>

#include <huxerui/text.h>

#include <concepts>
#include <stdexcept>

namespace huxerui::test {

static_assert(std::equality_comparable<Font>);
static_assert(std::equality_comparable<TextStyle>);
static_assert(std::equality_comparable<TextLayoutOptions>);

TEST_CASE("FontFactoriesPreservePlatformNeutralIdentity") {
  const Font system = Font::System(16.0F).WithWeight(FontWeight::SemiBold).WithSlant(FontSlant::Italic);
  REQUIRE(system.FamilyKind() == FontFamilyKind::System);
  REQUIRE(system.FamilyName().empty());
  REQUIRE(system.Size() == 16.0F);
  REQUIRE(system.Weight() == FontWeight::SemiBold);
  REQUIRE(system.Slant() == FontSlant::Italic);

  const Font monospace = Font::Monospace(13.0F);
  REQUIRE(monospace.FamilyKind() == FontFamilyKind::Monospace);

  const Font named = Font::Named("Inter", 15.0F);
  REQUIRE(named.FamilyKind() == FontFamilyKind::Named);
  REQUIRE(named.FamilyName() == "Inter");
}

TEST_CASE("FontRejectsInvalidPublicConfiguration") {
  REQUIRE_THROWS_AS(Font::System(0.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(Font::Monospace(-1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(Font::Named("", 14.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(Font::Named("Inter", 14.0F).WithSize(0.0F), std::invalid_argument);
}

TEST_CASE("TextDecorationFlagsComposeWithoutChangingFontIdentity") {
  const TextDecoration decoration = TextDecoration::Underline | TextDecoration::StrikeThrough;
  REQUIRE(HasTextDecoration(decoration, TextDecoration::Underline));
  REQUIRE(HasTextDecoration(decoration, TextDecoration::StrikeThrough));

  const TextStyle style{
      Font::Named("Inter", 14.0F).WithWeight(FontWeight::Bold),
      Color::Rgb(10, 20, 30),
      decoration,
  };
  REQUIRE(style.font.FamilyName() == "Inter");
  REQUIRE(style.decoration == decoration);
}

} // namespace huxerui::test
