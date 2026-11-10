#include <catch2/catch_amalgamated.hpp>

#include <huxerui/text.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>

#include <concepts>
#include <chrono>
#include <filesystem>
#include <stdexcept>

#include "text/text_internal.h"
#include "text/font_internal.h"
#include "font_test_support.h"

namespace huxerui::test {

static_assert(std::equality_comparable<Font>);
static_assert(std::equality_comparable<FontFamily>);
static_assert(std::equality_comparable<TextStyle>);
static_assert(std::equality_comparable<TextLayoutOptions>);
static_assert(std::equality_comparable<AttributedText>);

TEST_CASE("TextLayoutOptionsHaveExplicitAlignmentDefaults") {
  const TextLayoutOptions options;
  REQUIRE(options.align == TextAlign::Leading);
  REQUIRE(options.vertical_align == TextVerticalAlign::Top);
  REQUIRE(options.wrap == TextWrap::Word);
}

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

TEST_CASE("CustomFontFamiliesPreserveValueIdentityAndSelectAvailableFaces") {
  const auto family = TestFontFamily();
  const auto reordered = FontFamily::FromBytes({TestFontBytes(FontWeight::Regular, FontSlant::Italic),
      TestFontBytes(FontWeight::Bold), TestFontBytes()});
  REQUIRE(family == reordered);
  REQUIRE(detail::HashFontFamily(family) == detail::HashFontFamily(reordered));
  REQUIRE(family.Kind() == FontFamilyKind::Custom);
  REQUIRE(family.Name().empty());
  REQUIRE(family != FontFamily::Named("HuxerUI Test"));
  REQUIRE(FontFamily{} == FontFamily::System());
  const auto regular = Font::FromFamily(family, 20.0F);
  REQUIRE(detail::ResolveFontFace(regular)->weight == 400);
  REQUIRE(detail::ResolveFontFace(regular.WithWeight(FontWeight::Medium))->weight == 400);
  REQUIRE(detail::ResolveFontFace(regular.WithWeight(FontWeight::SemiBold))->weight == 700);
  REQUIRE(detail::ResolveFontFace(regular.WithWeight(FontWeight::Black))->weight == 700);
  const auto italic_bold = regular.WithWeight(FontWeight::Bold).WithSlant(FontSlant::Italic);
  REQUIRE(detail::ResolveFontFace(italic_bold)->slant == FontSlant::Italic);
  REQUIRE(detail::ResolveFontFace(italic_bold)->weight == 400);
  REQUIRE(italic_bold.WithFamily(FontFamily::Monospace()) ==
      Font::Monospace(20.0F).WithWeight(FontWeight::Bold).WithSlant(FontSlant::Italic));
  REQUIRE(Font::FromBytes(TestFontBytes(), 20.0F) ==
      Font::FromFamily(FontFamily::FromBytes({TestFontBytes()}), 20.0F));
}

TEST_CASE("CustomFontFamiliesRejectEmptyDuplicateAndMalformedSources") {
  REQUIRE_THROWS_AS(FontFamily::FromBytes({}), std::invalid_argument);
  REQUIRE_THROWS_AS(FontFamily::FromFiles({}), std::invalid_argument);
  REQUIRE_THROWS_AS(FontFamily::FromResources({}), std::invalid_argument);
  REQUIRE_THROWS_AS(FontFamily::FromBytes({TestFontBytes(), TestFontBytes()}), std::invalid_argument);
  REQUIRE_THROWS_AS(Font::FromBytes({}), std::invalid_argument);
  const auto valid = TestFontBytes();
  for (std::size_t size : {0U, 4U, 11U, 100U, 500U}) {
    REQUIRE_THROWS_AS(Font::FromBytes(Bytes(valid.begin(), valid.begin() + size)), std::invalid_argument);
  }
  auto corrupt = valid;
  SECTION("Collection") { std::memcpy(corrupt.data(), "ttcf", 4); }
  SECTION("Compressed font") { std::memcpy(corrupt.data(), "wOFF", 4); }
  SECTION("Table range") { std::fill(corrupt.begin() + 20, corrupt.begin() + 24, std::byte{0xff}); }
  SECTION("Variable font") { std::memcpy(corrupt.data() + 12, "fvar", 4); }
  REQUIRE_THROWS_AS(Font::FromBytes(std::move(corrupt)), std::invalid_argument);
}

TEST_CASE("CustomFontFilesAreSnapshotsIndependentOfPathsAndRuntime") {
  const auto path = std::filesystem::temp_directory_path() /
      ("huxerui-font-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ttf");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
  } cleanup{path};
  const File file(path.generic_u8string());
  REQUIRE(file.WriteBytes(TestFontBytes()));
  const auto snapshot = Font::FromFile(file, 18.0F);
  REQUIRE(snapshot == Font::FromBytes(TestFontBytes(), 18.0F));
  REQUIRE(file.WriteBytes(TestFontBytes(FontWeight::Bold)));
  REQUIRE(snapshot != Font::FromFile(file, 18.0F));
  REQUIRE(std::filesystem::remove(path));
  REQUIRE(detail::ResolveFontFace(snapshot)->bytes == TestFontBytes());
  REQUIRE_THROWS_AS(Font::FromFile(file), std::runtime_error);
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

TEST_CASE("AttributedTextNormalizesFragmentsAndRangesToTheSameValue") {
  const TextSpanStyle emphasis{.font_weight = FontWeight::Bold, .foreground = Color::Rgb(10, 20, 30)};
  const AttributedText fragments{
      TextSpan("Hello "), TextSpan("世").Style(emphasis), TextSpan("界").Style(emphasis), TextSpan(""),
  };
  const auto ranges = AttributedText::FromRanges("Hello 世界", {{{6, 8}, emphasis}});
  REQUIRE(fragments == ranges);
  REQUIRE(fragments.Length() == 8);
  REQUIRE(fragments.StyleRanges().size() == 1);
  REQUIRE(fragments.StyleRanges()[0].range == TextRange{6, 8});
  REQUIRE(fragments.TextInRange({6, 8}) == "世界");
  REQUIRE(AttributedText{} == AttributedText(std::string{}));
  REQUIRE(AttributedText::FromRanges("plain", {{{0, 5}, {}}}) == AttributedText("plain"));
}

TEST_CASE("AttributedTextKeepsLogicalLinksIndependentFromStyles") {
  const Uri target("https://huxerui.org");
  const auto text = AttributedText::FromRanges("read docs",
      {{{0, 4}, {.font_weight = FontWeight::Bold}}, {{5, 9}, {.decoration = TextDecoration::Underline}}},
      {{{0, 9}, target}});
  REQUIRE(text.StyleRanges().size() == 2);
  REQUIRE(text.LinkRanges().size() == 1);
  const auto recolored = text.WithStyles({{{0, 9}, {.foreground = Color::Black()}}});
  REQUIRE(recolored.PlainText().data() == text.PlainText().data());
  REQUIRE(recolored.LinkRanges()[0] == text.LinkRanges()[0]);
  REQUIRE(recolored != text);
  REQUIRE(text.WithStyles({}) == AttributedText::FromRanges("read docs", {}, {{{0, 9}, target}}));

  const AttributedText separate{TextSpan("one").Link(target), TextSpan("two").Link(target)};
  REQUIRE(separate.LinkRanges().size() == 2);
  REQUIRE(separate != AttributedText::FromRanges("onetwo", {}, {{{0, 6}, target}}));
  const auto copied = separate;
  REQUIRE(copied == separate);
  REQUIRE(copied.PlainText().data() == separate.PlainText().data());
}

TEST_CASE("AttributedTextNormalizesCompleteFontBeforePartialOverrides") {
  const auto text = AttributedText::FromRanges("abc",
      {{{0, 3}, {.font = Font::Monospace(12.0F), .font_size = 18.0F, .font_weight = FontWeight::Bold}}});
  REQUIRE(
      text == AttributedText::FromRanges("abc",
          {{{0, 3}, {.font = Font::Monospace(18.0F).WithWeight(FontWeight::Bold)}}})
  );
  REQUIRE_FALSE(text.StyleRanges()[0].style.font_size.has_value());
  REQUIRE_FALSE(text.StyleRanges()[0].style.font_weight.has_value());
}

TEST_CASE("AttributedTextRejectsMalformedUtf8AndInvalidRanges") {
  const TextSpanStyle bold{.font_weight = FontWeight::Bold};
  for (const std::string text : {"\x80", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF0\x9F"}) {
    REQUIRE_THROWS_AS(AttributedText(text), std::invalid_argument);
  }
  REQUIRE_THROWS_AS((AttributedText{TextSpan("\xC2"), TextSpan("\xA0")}), std::invalid_argument);
  const AttributedText text("a😀b");
  REQUIRE(text.Length() == 4);
  REQUIRE(text.TextInRange({1, 3}) == "😀");
  REQUIRE(text.TextInRange({4, 4}).empty());
  for (const TextRange range : {TextRange{-1, 0}, {2, 3}, {1, 2}, {4, 5}, {3, 1}}) {
    REQUIRE_THROWS_AS(text.WithStyles({{range, bold}}), std::invalid_argument);
    REQUIRE_THROWS_AS(text.TextInRange(range), std::invalid_argument);
  }
  REQUIRE_THROWS_AS(text.WithStyles({{{0, 3}, bold}, {{1, 4}, bold}}), std::invalid_argument);
  REQUIRE_THROWS_AS(text.WithStyles({{{3, 4}, bold}, {{0, 1}, bold}}), std::invalid_argument);
  REQUIRE_THROWS_AS(AttributedText::FromRanges("a😀b", {}, {{{1, 2}, Uri("https://huxerui.org")}}),
      std::invalid_argument);
}

TEST_CASE("AttributedTextValidatesAttributesAndPreservesExplicitClearing") {
  for (const float size :
       {0.0F, -1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    REQUIRE_THROWS_AS(AttributedText::FromRanges("a", {{{0, 1}, {.font_size = size}}}), std::invalid_argument);
  }
  REQUIRE_THROWS_AS(AttributedText::FromRanges("a", {{{0, 1}, {.font_weight = static_cast<FontWeight>(950)}}}),
      std::invalid_argument);
  REQUIRE_THROWS_AS(AttributedText::FromRanges("a", {{{0, 1}, {.font_slant = static_cast<FontSlant>(20)}}}),
      std::invalid_argument);
  REQUIRE_THROWS_AS(AttributedText::FromRanges("a", {{{0, 1}, {.decoration = static_cast<TextDecoration>(8)}}}),
      std::invalid_argument);
  const Color invalid{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F};
  REQUIRE_THROWS_AS(AttributedText::FromRanges("a", {{{0, 1}, {.foreground = invalid}}}), std::invalid_argument);
  REQUIRE_THROWS_AS(AttributedText::FromRanges("a", {{{0, 1}, {.background = invalid}}}), std::invalid_argument);
  const auto cleared = AttributedText::FromRanges("a",
      {{{0, 1}, {.background = Color::Transparent(), .decoration = TextDecoration::None}}});
  REQUIRE(cleared.StyleRanges().size() == 1);
  REQUIRE(cleared.StyleRanges()[0].style.decoration == TextDecoration::None);
}

TEST_CASE("AttributedTextIndexesLongUnicodeBodiesAtValidBoundaries") {
  std::string body;
  std::vector<TextStyleRange> styles;
  for (TextOffset index = 0; index < 5000; ++index) {
    body += "中😀a";
    styles.push_back({{index * 4 + 1, index * 4 + 3}, {.font_weight = FontWeight::Bold}});
  }
  const auto text = AttributedText::FromRanges(body, styles);
  REQUIRE(text.Length() == 20000);
  REQUIRE(text.StyleRanges().size() == 5000);
  for (TextOffset index = 0; index < 5000; ++index) {
    REQUIRE(text.TextInRange({index * 4, index * 4 + 4}) == "中😀a");
    REQUIRE_THROWS_AS(text.TextInRange({index * 4 + 2, index * 4 + 3}), std::invalid_argument);
  }
}

TEST_CASE("AttributedTextInvalidationComparesEffectiveStyleProjections") {
  const AttributedText plain("text");
  const TextStyle base{Font::System(16.0F), Color::Black(), TextDecoration::Underline};
  const auto colored = plain.WithStyles({{{0, 4}, {.foreground = Color::White()}}});
  REQUIRE(detail::TextLayoutInputsEqual(plain, base.font, colored, base.font));
  REQUIRE_FALSE(detail::TextPaintInputsEqual(plain, base, colored, base));
  const auto explicit_base = plain.WithStyles({{{0, 2}, {.font = base.font}}, {{2, 4}, {.font_size = 16.0F}}});
  REQUIRE(detail::TextLayoutInputsEqual(plain, base.font, explicit_base, base.font));
  REQUIRE(detail::TextPaintInputsEqual(plain, base, explicit_base, base));
  const auto larger = plain.WithStyles({{{1, 3}, {.font_size = 20.0F}}});
  REQUIRE_FALSE(detail::TextLayoutInputsEqual(plain, base.font, larger, base.font));
  const auto link = AttributedText::FromRanges("text", {}, {{{0, 4}, Uri("https://huxerui.org")}});
  REQUIRE(detail::TextLayoutInputsEqual(plain, base.font, link, base.font));
  REQUIRE(detail::TextPaintInputsEqual(plain, base, link, base));
  REQUIRE_FALSE(detail::TextLayoutInputsEqual(AttributedText{}, base.font, AttributedText{}, Font::System(20.0F)));
}

TEST_CASE("AttributedTextInlineObjectsPreserveIndependentAttributesAndInvalidateGeometry") {
  const TextStyle base{Font::System(16.0F), Color::Black()};
  const auto text = AttributedText::FromRanges("中文👩‍💻\uFFFC继续",
      {{{0, 10}, {.foreground = Color::White()}}}, {{{7, 8}, Uri("https://huxerui.org")}});
  const TextInlineObject fraction{7, 36.0F, 24.0F, 12.0F};
  const auto annotated = text.WithInlineObjects({fraction});
  REQUIRE(annotated.PlainText().data() == text.PlainText().data());
  REQUIRE(annotated.TextInRange({7, 8}) == "\uFFFC");
  REQUIRE(annotated.InlineObjects().size() == 1);
  REQUIRE(annotated.InlineObjects().front() == fraction);
  REQUIRE(std::ranges::equal(annotated.LinkRanges(), text.LinkRanges()));
  REQUIRE(annotated.WithStyles({}).InlineObjects().front() == fraction);
  REQUIRE(annotated.WithInlineObjects({}) == text);
  const auto resized = annotated.WithInlineObjects({{7, 40.0F, 26.0F, 12.0F}});
  REQUIRE_FALSE(detail::TextLayoutInputsEqual(annotated, base.font, resized, base.font));
  REQUIRE_FALSE(detail::TextPaintInputsEqual(annotated, base, resized, base));

  const auto runs = detail::ResolveTextRuns(annotated, base);
  REQUIRE(runs.size() == 3);
  REQUIRE(runs[1].inline_object == fraction);
  REQUIRE(runs[1].byte_start == 17);
  REQUIRE(runs[1].byte_end == 20);
  REQUIRE_THROWS_AS(text.WithInlineObjects({fraction, fraction}), std::invalid_argument);
  REQUIRE_THROWS_AS(text.WithInlineObjects({{3, 36.0F, 24.0F, 12.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(text.WithInlineObjects({{6, 36.0F, 24.0F, 12.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(text.WithInlineObjects({{8, 36.0F, 24.0F, 12.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(text.WithInlineObjects({{7, 0.0F, 24.0F, 12.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(text.WithInlineObjects({{7, 36.0F, -1.0F, 12.0F}}), std::invalid_argument);
}

TEST_CASE("AttributedTextResolvesByteAndUtf16RunsWithoutSplittingLogicalLinks") {
  const TextStyle base{Font::System(16.0F), Color::Black(), TextDecoration::Underline};
  const auto text = AttributedText::FromRanges("a😀中b",
      {{{1, 3}, {.font_size = 20.0F, .decoration = TextDecoration::None}}, {{3, 4}, {.background = Color::White()}}},
      {{{0, 5}, Uri("https://huxerui.org")}});
  const auto runs = detail::ResolveTextRuns(text, base);
  REQUIRE(runs.size() == 4);
  REQUIRE(runs[0].range == TextRange{0, 1});
  REQUIRE(runs[1].range == TextRange{1, 3});
  REQUIRE(runs[1].byte_start == 1);
  REQUIRE(runs[1].byte_end == 5);
  REQUIRE(runs[1].style.font.Size() == 20.0F);
  REQUIRE(runs[1].style.decoration == TextDecoration::None);
  REQUIRE(runs[2].range == TextRange{3, 4});
  REQUIRE(runs[2].byte_end == 8);
  REQUIRE(runs[2].background == Color::White());
  REQUIRE(runs[3].byte_end == text.PlainText().size());
  REQUIRE(text.LinkRanges().size() == 1);
}

TEST_CASE("AttributedParagraphCacheChargesBoundRetainedBodiesAndAttributes") {
  const AttributedText small("label");
  REQUIRE(detail::ParagraphCacheCost(small) > small.PlainText().size());
  REQUIRE(detail::ParagraphCacheCost(small) < detail::paragraph_cache_budget);
  const AttributedText large(std::string(detail::paragraph_cache_budget / 64 + 1, 'x'));
  REQUIRE(detail::ParagraphCacheCost(large) > detail::paragraph_cache_budget);
  const auto linked = AttributedText::FromRanges("label", {}, {{{0, 5}, Uri("https://example.com/guide")}});
  REQUIRE(detail::ParagraphCacheCost(linked) > detail::ParagraphCacheCost(small));
}

} // namespace huxerui::test
