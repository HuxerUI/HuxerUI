#include <catch2/catch_amalgamated.hpp>

#include <limits>

#include <huxerui/text.h>

#include "linux_renderer.h"

namespace huxerui::test {

TEST_CASE("LinuxUnboundedTextMeasurementIgnoresParagraphAlignment") {
  detail::LinuxRenderer renderer;
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
}

TEST_CASE("LinuxFontMetricsArePositiveAndFinite") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const FontMetrics metrics = renderer.Metrics(Font::System(14.0F));
  REQUIRE(metrics.ascent > 0.0F);
  REQUIRE(metrics.descent > 0.0F);
  REQUIRE(metrics.leading >= 0.0F);
  REQUIRE(std::isfinite(metrics.ascent));
  REQUIRE(std::isfinite(metrics.descent));
  renderer.Discard();
}

TEST_CASE("LinuxMeasureRunProducesPositiveAdvance") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const TextRunMetrics run = renderer.MeasureRun("Hello", style, {});
  REQUIRE(run.advance > 0.0F);
  REQUIRE(run.visual_bounds.width > 0.0F);
  REQUIRE(run.font_metrics.ascent > 0.0F);
  renderer.Discard();
}

TEST_CASE("LinuxMeasureTextReportsLineCountForNewlines") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const TextLayoutMetrics metrics = renderer.MeasureText("one\ntwo\nthree", style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(metrics.line_count == 3);
  REQUIRE(metrics.size.height > 0.0F);
  REQUIRE(metrics.first_baseline > 0.0F);
  REQUIRE(metrics.last_baseline > metrics.first_baseline);
  renderer.Discard();
}

TEST_CASE("LinuxMeasureTextWrapsAtWordBoundaries") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float width = renderer.MeasureText("Hello World", style, 100000.0F, {.wrap = TextWrap::NoWrap}).size.width;
  REQUIRE(width > 0.0F);
  // A width that fits "Hello" but not "Hello World" must break at the space,
  // producing two lines instead of splitting a word mid-glyph.
  const float narrow = width * 0.6F;
  const TextLayoutMetrics wrapped = renderer.MeasureText("Hello World", style, narrow, {.wrap = TextWrap::Word});
  REQUIRE(wrapped.line_count == 2);
  REQUIRE(wrapped.size.width <= narrow);
  renderer.Discard();
}

TEST_CASE("LinuxMeasureTextKeepsSingleLongWordOnOneLineWhenUnbounded") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const TextLayoutMetrics metrics =
      renderer
          .MeasureText("Supercalifragilistic", style, std::numeric_limits<float>::infinity(), {.wrap = TextWrap::Word});
  REQUIRE(metrics.line_count == 1);
  REQUIRE(metrics.size.width > 0.0F);
  renderer.Discard();
}

} // namespace huxerui::test
