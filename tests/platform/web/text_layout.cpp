#include <catch2/catch_amalgamated.hpp>
#include <algorithm>
#include <emscripten/val.h>
#include <emscripten.h>
#include <huxerui/paint.h>
#include "web_renderer.h"
#include "text/text_internal.h"
#include "font_test_support.h"

using namespace huxerui;

namespace {

emscripten::val TextTestCanvas() {
  const auto factory = emscripten::val::global("Function").new_(std::string(R"js(
    const context = {
      calls: [],
      draws: [],
      globalAlpha: 1,
      save() {}, restore() {}, setTransform() {}, beginPath() {}, clearRect() {}, rect() {}, clip() {},
      transform() {},
      fillText(text, x, y) { this.draws.push({text, x, y}); },
      measureText(text) {
        this.calls.push(text);
        const advance = this.font && this.font.includes("huxerui_font_") ? 20 : 10;
        return {width: Array.from(text).length * advance, fontBoundingBoxAscent: 8, fontBoundingBoxDescent: 2};
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
    const auto layout = renderer.CreatePlatformTextLayout(text, style, 200.0F, {.wrap = TextWrap::NoWrap});
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

TEST_CASE("WebParagraphDrawingReusesMeasuredLinesAndFillsMissingFragmentPositions") {
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const auto text = AttributedText::FromRanges("abcd", {{{1, 3}, {.foreground = Color::White()}}});
  const TextStyle style{Font::Monospace(10.0F), Color::Black()};
  const TextLayoutOptions options{.align = TextAlign::Center, .wrap = TextWrap::NoWrap};
  const Rect bounds{0.0F, 0.0F, 80.0F, 24.0F};
  const auto metrics = renderer.MeasureText(text, style, bounds.width, options);

  RenderNode root;
  PaintContext paint(root.content, bounds);
  paint.DrawText(bounds, text, style, options);
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.rects = {bounds}}, .revision = 1};
  renderer.DrawSlice(canvas, frame, 0, 1, false, false);
  renderer.DrawSlice(canvas, frame, 0, 1, false, false);

  const auto context = canvas["context"];
  const auto draws = context["draws"];
  REQUIRE(draws["length"].as<unsigned>() == 6);
  REQUIRE(draws[0]["x"].as<float>() == 20.0F);
  REQUIRE(draws[1]["x"].as<float>() == 30.0F);
  REQUIRE(draws[2]["x"].as<float>() == 50.0F);
  REQUIRE(draws[1]["y"].as<float>() == metrics.first_baseline);
  const auto calls = emscripten::vecFromJSArray<std::string>(context["calls"]);
  REQUIRE(std::ranges::count(calls, "abcd") == 1);
  REQUIRE(std::ranges::count(calls, "a") == 1);
  REQUIRE(std::ranges::count(calls, "abc") == 1);
  REQUIRE(renderer.MeasureText(text, style, bounds.width, options) == metrics);
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
      const auto layout = renderer.CreatePlatformTextLayout(text, style, width, options);
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

TEST_CASE("WebParagraphWrapSkipsAnOverwideBreakOpportunity") {
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const auto layout = renderer.CreatePlatformTextLayout(
      AttributedText("ab cdef-gh"), {Font::Monospace(10.0F), Color::Black()}, 75.0F, {});
  const auto lines = layout->LineMetrics();
  REQUIRE(lines.size() == 2);
  CHECK(lines[0].range == TextRange{0, 3});
  CHECK(lines[0].bounds.width == 30.0F);
  CHECK(lines[1].range == TextRange{3, 10});
  CHECK(lines[1].bounds.width == 70.0F);
}

TEST_CASE("WebParagraphReusesWrapMeasurementsForCaretGeometry") {
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const auto layout = renderer.CreatePlatformTextLayout(
      AttributedText("abcdef"), {Font::Monospace(10.0F), Color::Black()}, 35.0F, {});
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

TEST_CASE("WebCustomFontReadinessRebuildsCachedParagraphAndCaretGeometry") {
  EM_ASM({
    Module.fontTestSaved = ({document: globalThis.document, fontFace: globalThis.FontFace});
    globalThis.document = {fonts: new Set()};
    globalThis.FontFace = class {
      load() { return {then(callback) { Module.fontTestComplete = callback; return {catch() {}}; }}; }
    };
  });
  struct Cleanup {
    ~Cleanup() {
      detail::ResetWebFonts();
      EM_ASM({
        globalThis.document = Module.fontTestSaved.document;
        globalThis.FontFace = Module.fontTestSaved.fontFace;
        delete Module.fontTestSaved;
        delete Module.fontTestComplete;
      });
    }
  } cleanup;
  const auto canvas = TextTestCanvas();
  detail::WebRenderer renderer(0, canvas);
  const TextStyle style{Font::FromBytes(huxerui::test::TestFontBytes(), 20.0F), Color::Black()};
  const AttributedText text("AA");
  const auto before = renderer.CreatePlatformTextLayout(text, style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(before->CaretRect(2, TextAffinity::Upstream).x == 20.0F);
  EM_ASM({ Module.fontTestComplete(); });
  renderer.InvalidateTextMetrics();
  const auto after = renderer.CreatePlatformTextLayout(text, style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(renderer.MeasureText(text, style, 200.0F).size.width == 40.0F);
  REQUIRE(after->CaretRect(2, TextAffinity::Upstream).x == 40.0F);
  REQUIRE(after->HitTest({30.0F, 5.0F}).offset == 2);
  REQUIRE(before->CaretRect(2, TextAffinity::Upstream).x == 20.0F);
}

} // namespace
