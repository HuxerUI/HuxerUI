#include <catch2/catch_amalgamated.hpp>

#include <objbase.h>

#include <limits>

#include <huxerui/text.h>

#include "win32_renderer.h"
#include "text/text_internal.h"
#include "font_test_support.h"

namespace huxerui::test {

TEST_CASE("Win32UnboundedTextMeasurementIgnoresParagraphAlignment") {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  REQUIRE(SUCCEEDED(com_result));
  detail::Win32Renderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float unbounded = std::numeric_limits<float>::infinity();
  const TextLayoutMetrics leading =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Leading, .wrap = TextWrap::NoWrap});
  const TextLayoutMetrics centered =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Center, .wrap = TextWrap::NoWrap});
  const TextLayoutMetrics trailing =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap});

  REQUIRE(leading.size.width > 0.0F);
  REQUIRE(centered.size == leading.size);
  REQUIRE(trailing.size == leading.size);
  renderer.Discard();
  CoUninitialize();
}

TEST_CASE("Win32AttributedParagraphSharesNativeMeasurementAndSelectionMetrics") {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  REQUIRE(SUCCEEDED(com_result));
  detail::Win32Renderer renderer;
  renderer.Initialize();
  const TextStyle style{Font::System(14.0F), Color::Black()};
  const AttributedText text{
    TextSpan("small "),
    TextSpan("LARGE").Style({.font_size = 36.0F, .font_weight = FontWeight::Bold}),
    TextSpan(" small"),
  };
  const auto metrics = renderer.MeasureText(text, style, 240.0F, {});
  const auto plain = renderer.MeasureText(text.PlainText(), style, 240.0F, {});
  const auto layout = renderer.CreatePlatformTextLayout(text, style, 240.0F, {});
  REQUIRE(metrics.size.height > plain.size.height);
  REQUIRE(layout->Measure().height == metrics.size.height);
  const auto ranges = layout->RangeRects({6, 11});
  REQUIRE_FALSE(ranges.empty());
  REQUIRE(ranges.front().width > 70.0F);
  const auto caret = layout->CaretRect(8, TextAffinity::Downstream);
  REQUIRE(layout->HitTest({caret.x, caret.y + caret.height * 0.5F}).offset == 8);
  const auto wrapped = renderer.MeasureText(text, style, 85.0F, {.wrap = TextWrap::Word});
  REQUIRE(wrapped.line_count > metrics.line_count);
  const auto combining = renderer.CreatePlatformTextLayout(
      AttributedText::FromRanges("a\u0301b", {{{1, 2}, {.foreground = Color::White()}}}), style, 200.0F, {});
  REQUIRE(combining->NextCaretOffset(0) == 2);
  REQUIRE(combining->PreviousCaretOffset(2) == 0);
  const auto styled_combining = renderer.CreatePlatformTextLayout(
      AttributedText::FromRanges("a\u0301b", {{{1, 2}, {.font_size = 24.0F}}}), style, 200.0F, {});
  REQUIRE(styled_combining->NextCaretOffset(0) == 2);
  REQUIRE(styled_combining->PreviousCaretOffset(2) == 0);
  const auto emoji = renderer.CreatePlatformTextLayout(
      AttributedText::FromRanges("\U0001F469\u200D\U0001F4BB!", {{{2, 3}, {.font_size = 24.0F}}}), style, 200.0F, {});
  REQUIRE(emoji->NextCaretOffset(0) == 5);
  REQUIRE(emoji->PreviousCaretOffset(5) == 0);
  renderer.Discard();
  CoUninitialize();
}

TEST_CASE("Win32CustomFontsKeepExactFaceAndLoaderLifetimeAcrossRendererDiscard") {
  REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
  struct ComLifetime { ~ComLifetime() { CoUninitialize(); } } com;
  detail::Win32Renderer renderer;
  renderer.Initialize();
  const auto font = Font::FromFamily(TestFontFamily(), 20.0F);
  const TextStyle regular{font, Color::Black()};
  REQUIRE(renderer.MeasureRun("AA", regular, {}).advance == Catch::Approx(20.0F));
  REQUIRE(renderer.MeasureRun("AA", {font.WithWeight(FontWeight::Bold), Color::Black()}, {}).advance == Catch::Approx(32.0F));
  const auto rich = AttributedText::FromRanges("AA", {{{1, 2}, {.font_weight = FontWeight::Bold}}});
  auto layout = renderer.CreatePlatformTextLayout(rich, regular, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(layout->CaretRect(2, TextAffinity::Upstream).x == Catch::Approx(26.0F));
  const auto system = Font::System(20.0F);
  const auto mixed = AttributedText::FromRanges("AA", {{{1, 2}, {.font = system}}});
  const auto mixed_layout = renderer.CreatePlatformTextLayout(mixed, regular, 200.0F, {.wrap = TextWrap::NoWrap});
  const float system_advance = renderer.MeasureRun("A", {system, Color::Black()}, {}).advance;
  REQUIRE(mixed_layout->CaretRect(2, TextAffinity::Upstream).x == Catch::Approx(10.0F + system_advance));
  renderer.Discard();
  REQUIRE(layout->Measure().width == Catch::Approx(26.0F));
  REQUIRE(layout->HitTest({20.0F, 10.0F}).offset == 2);
}

} // namespace huxerui::test
