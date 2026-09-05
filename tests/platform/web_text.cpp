#include <catch2/catch_amalgamated.hpp>

#include "web_text_internal.h"

#ifdef __EMSCRIPTEN__
#include <algorithm>
#include <emscripten/val.h>
#include "web_renderer.h"
#include "text_internal.h"
#endif

using namespace huxerui;

#ifdef __EMSCRIPTEN__
namespace {

emscripten::val TextTestCanvas() {
  const auto factory = emscripten::val::global("Function").new_(std::string(R"js(
    const context = {
      calls: [],
      measureText(text) {
        this.calls.push(text);
        return {width: Array.from(text).length * 10, fontBoundingBoxAscent: 8, fontBoundingBoxDescent: 2};
      }
    };
    return {context, getContext() {return context;}};
  )js"));
  return factory();
}

TEST_CASE("WebParagraphPreservesHardBreakOffsets") {
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const TextStyle style{Font::Monospace(10.0F), Color::Black()};
  for (const std::string separator : {"\n", "\r\n", "\r"}) {
    const AttributedText text("ab" + separator + "cd");
    const auto metrics = renderer.MeasureText(text, style, 200.0F, {.wrap = TextWrap::NoWrap});
    REQUIRE(metrics.line_count == 2);
    const auto layout = renderer.CreateTextLayout(text, style, 200.0F, {.wrap = TextWrap::NoWrap});
    const TextOffset second_start = 2 + static_cast<TextOffset>(separator.size());
    const Rect second = layout->CaretRect(second_start, TextAffinity::Downstream);
    REQUIRE(second.y > 0.0F);
    REQUIRE(layout->HitTest({second.x, second.y + second.height * 0.5F}).offset == second_start);
    REQUIRE(layout->NextCaretOffset(2) == second_start);
    REQUIRE(layout->RangeRects({second_start, text.Length()}).size() == 1);
  }
}

TEST_CASE("WebParagraphMeasurementDoesNotBuildEveryCaret") {
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const AttributedText text(std::string(1024, 'a'));
  const auto metrics = renderer.MeasureText(text, {Font::Monospace(10.0F), Color::Black()},
      20000.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(metrics.line_count == 1);
  REQUIRE(canvas["context"]["calls"]["length"].as<unsigned>() < 8);
}

TEST_CASE("WebParagraphMeasurementConstrainsWidthWithoutChangingNaturalGeometry") {
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const AttributedText text("abcdefghij");
  const TextStyle style{Font::Monospace(10.0F), Color::Black()};
  for (const auto wrap : {TextWrap::NoWrap, TextWrap::Word}) {
    for (const float width : {35.0F, 35.5F, 5.5F, 200.0F}) {
      CAPTURE(wrap, width);
      const TextLayoutOptions options{.wrap = wrap};
      const auto metrics = renderer.MeasureText(text, style, width, options);
      const auto layout = renderer.CreateTextLayout(text, style, width, options);
      REQUIRE(metrics.size.width <= width);
      REQUIRE(metrics.size.height == layout->Measure().height);
      if (wrap == TextWrap::NoWrap) {
        REQUIRE(metrics.size.width == std::min(100.0F, width));
        REQUIRE(metrics.line_count == 1);
        REQUIRE(layout->Measure().width == 100.0F);
        REQUIRE(layout->CaretRect(text.Length(), TextAffinity::Upstream).x == 100.0F);
        const auto fragments = layout->RangeRects({0, text.Length()});
        REQUIRE(fragments.size() == 1);
        REQUIRE(fragments.front().width == 100.0F);
      } else {
        REQUIRE((metrics.line_count > 1) == (width < 100.0F));
      }
    }
    for (const float width : {0.0F, -1.0F}) {
      const auto metrics = renderer.MeasureText(text, style, width, {.wrap = wrap});
      REQUIRE(metrics.size == Size{});
      REQUIRE(metrics.line_count == 0);
    }
  }
}

TEST_CASE("WebParagraphReusesWrapMeasurementsForCaretGeometry") {
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const auto layout = renderer.CreateTextLayout(AttributedText("abcdef"), {Font::Monospace(10.0F), Color::Black()},
      35.0F, {});
  REQUIRE(layout->Measure().height > 12.0F);
  const auto calls = emscripten::vecFromJSArray<std::string>(canvas["context"]["calls"]);
  for (const std::string prefix : {"a", "ab", "abc", "d", "de", "def"}) {
    REQUIRE(std::count(calls.begin(), calls.end(), prefix) == 1);
  }
  for (TextOffset offset = 0; offset <= 6; ++offset) {
    const Rect caret = layout->CaretRect(offset, TextAffinity::Downstream);
    REQUIRE(layout->HitTest({caret.x, caret.y + caret.height * 0.5F}).offset == offset);
  }
}

} // namespace
#endif

TEST_CASE("WebTextRecognizesHardBreakClustersWithoutChangingOffsets") {
  REQUIRE(detail::IsWebHardLineBreak("\n"));
  REQUIRE(detail::IsWebHardLineBreak("\r"));
  REQUIRE(detail::IsWebHardLineBreak("\r\n"));
  REQUIRE_FALSE(detail::IsWebHardLineBreak(""));
  REQUIRE_FALSE(detail::IsWebHardLineBreak(" "));
  REQUIRE_FALSE(detail::IsWebHardLineBreak("\n\n"));
}

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
