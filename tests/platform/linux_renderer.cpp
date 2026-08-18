#include <catch2/catch_amalgamated.hpp>

#include <limits>
#include <string>

#include <huxerui/text.h>

#include "linux_renderer.h"

#include "text_layout_internal.h"

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

TEST_CASE("LinuxTextLayoutHonorsExplicitDirection") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const std::unique_ptr<detail::TextLayout> left_to_right = renderer.CreateTextLayout(
      "abc",
      style,
      200.0F,
      {.shaping = {.direction = TextDirection::LeftToRight}, .wrap = TextWrap::NoWrap}
  );
  const std::unique_ptr<detail::TextLayout> right_to_left = renderer.CreateTextLayout(
      "abc",
      style,
      200.0F,
      {.shaping = {.direction = TextDirection::RightToLeft}, .wrap = TextWrap::NoWrap}
  );
  REQUIRE(left_to_right->CaretRect(0, TextAffinity::Downstream).x == 0.0F);
  REQUIRE(left_to_right->CaretRect(3, TextAffinity::Downstream).x > 0.0F);
  REQUIRE(right_to_left->CaretRect(0, TextAffinity::Downstream).x > 0.0F);
  REQUIRE(right_to_left->CaretRect(3, TextAffinity::Downstream).x == 0.0F);
  renderer.Discard();
}

TEST_CASE("LinuxWordWrappingPreservesUtf8ScalarBoundaries") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float character_width = renderer.MeasureRun("\u4E16", style, {}).advance;
  const TextLayoutMetrics metrics =
      renderer.MeasureText("\u4E16\u754C", style, character_width + 0.1F, {.wrap = TextWrap::Word});
  REQUIRE(metrics.line_count == 2);
  const std::unique_ptr<detail::TextLayout> layout =
      renderer.CreateTextLayout("\u4E16\u754C", style, character_width + 0.1F, {.wrap = TextWrap::Word});
  REQUIRE(layout->NextCaretOffset(0) == 1);
  REQUIRE(layout->NextCaretOffset(1) == 2);
  renderer.Discard();
}

TEST_CASE("LinuxTextMeasurementPreservesTrailingEmptyLine") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const TextLayoutMetrics metrics = renderer.MeasureText("one\n", style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(metrics.line_count == 2);
  REQUIRE(metrics.last_baseline > metrics.first_baseline);
  renderer.Discard();
}

TEST_CASE("LinuxTextDecorationContributesToRunBounds") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle plain{Font::System(14.0F), Color::Black()};
  const TextStyle decorated{
      Font::System(14.0F),
      Color::Black(),
      TextDecoration::Underline | TextDecoration::StrikeThrough,
  };
  const TextRunMetrics plain_metrics = renderer.MeasureRun("Decorated", plain, {});
  const TextRunMetrics decorated_metrics = renderer.MeasureRun("Decorated", decorated, {});
  REQUIRE(decorated_metrics.advance == plain_metrics.advance);
  REQUIRE(decorated_metrics.visual_bounds.y <= plain_metrics.visual_bounds.y);
  REQUIRE(decorated_metrics.visual_bounds.height >= plain_metrics.visual_bounds.height);
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

TEST_CASE("LinuxTextLayoutCaretOffsetsSkipSurrogatePairs") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  // "A" (1 UTF-16 unit), emoji (2 units), "B" (1 unit); offsets are UTF-16 units.
  const std::unique_ptr<detail::TextLayout> layout =
      renderer.CreateTextLayout("A\U0001F600B", style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(layout != nullptr);
  REQUIRE(layout->Measure().width > 0.0F);

  REQUIRE(layout->NextCaretOffset(0) == 1);
  REQUIRE(layout->NextCaretOffset(1) == 3);
  REQUIRE(layout->NextCaretOffset(3) == 4);
  REQUIRE(layout->NextCaretOffset(4) == 4);
  REQUIRE(layout->PreviousCaretOffset(4) == 3);
  REQUIRE(layout->PreviousCaretOffset(3) == 1);
  REQUIRE(layout->PreviousCaretOffset(1) == 0);
  REQUIRE(layout->PreviousCaretOffset(0) == 0);

  const std::vector<Rect> rects = layout->RangeRects({0, 4});
  REQUIRE(rects.size() == 1);
  REQUIRE(rects[0].width >= 0.0F);
  renderer.Discard();
}

TEST_CASE("LinuxTextLayoutCaretOffsetsCrossMultibyteCjk") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  // Each CJK character occupies 3 UTF-8 bytes but 1 UTF-16 unit.
  const std::unique_ptr<detail::TextLayout> layout =
      renderer.CreateTextLayout("A\u4E16B", style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(layout != nullptr);
  REQUIRE(layout->Measure().width > 0.0F);

  REQUIRE(layout->NextCaretOffset(0) == 1);
  REQUIRE(layout->NextCaretOffset(1) == 2);
  REQUIRE(layout->NextCaretOffset(2) == 3);
  REQUIRE(layout->PreviousCaretOffset(3) == 2);
  REQUIRE(layout->PreviousCaretOffset(2) == 1);
  REQUIRE(layout->PreviousCaretOffset(1) == 0);

  // A caret inside the surrogate-adjacent or multibyte region never splits a character.
  const Rect caret = layout->CaretRect(1, TextAffinity::Downstream);
  REQUIRE(caret.width == 1.0F);
  REQUIRE(caret.height > 0.0F);
  renderer.Discard();
}

TEST_CASE("LinuxTextLayoutHitTestStaysWithinUtf16Length") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const std::unique_ptr<detail::TextLayout> layout =
      renderer.CreateTextLayout("hello \U0001F600 world", style, 400.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(layout != nullptr);

  const float width = layout->Measure().width;
  REQUIRE(width > 0.0F);
  // A click far past the end must clamp to the final UTF-16 offset (14 units for
  // "hello \U0001F600 world"), never beyond it.
  const TextPosition position = layout->HitTest({width * 2.0F, 0.0F});
  REQUIRE(position.offset == 14);
  renderer.Discard();
}

TEST_CASE("LinuxRendererFreshInstanceHasNoPresentationState") {
  // A freshly constructed renderer must report no EGL state; the adapter probes
  // this headlessly before creating an X window.
  detail::LinuxRenderer renderer;
  REQUIRE_FALSE(renderer.HasPresentation());
  REQUIRE(renderer.NativeVisualId() == 0);
  renderer.Discard();
}

TEST_CASE("LinuxRendererDiscardOnFreshInstanceIsNoOp") {
  detail::LinuxRenderer renderer;
  renderer.Discard();
  // Repeated teardown without any presentation setup must stay a no-op.
  renderer.Discard();
}

TEST_CASE("LinuxShapeRunCacheReturnsIdenticalResults") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const TextRunMetrics first = renderer.MeasureRun("Hello", style, {});
  const TextRunMetrics second = renderer.MeasureRun("Hello", style, {});
  REQUIRE(second.advance == first.advance);

  const std::unique_ptr<detail::TextLayout> layout =
      renderer.CreateTextLayout("Hello World", style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(layout != nullptr);
  const std::unique_ptr<detail::TextLayout> repeated =
      renderer.CreateTextLayout("Hello World", style, 200.0F, {.wrap = TextWrap::NoWrap});
  REQUIRE(repeated != nullptr);
  REQUIRE(repeated->Measure() == layout->Measure());
  renderer.Discard();
}

TEST_CASE("LinuxShapeRunCacheSurvivesEvictionChurn") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float original_run0 = renderer.MeasureRun("Run0", style, {}).advance;
  const float original_run1 = renderer.MeasureRun("Run1", style, {}).advance;
  const float original_run2 = renderer.MeasureRun("Run2", style, {}).advance;
  for (int index = 0; index < 1050; ++index) {
    const TextRunMetrics run = renderer.MeasureRun("Run" + std::to_string(index), style, {});
    REQUIRE(run.advance > 0.0F);
  }
  // 1050 distinct runs overflow kMaxShapedRuns (1024), evicting the earliest
  // entries; re-shaping the evicted runs must reproduce their exact advances.
  REQUIRE(renderer.MeasureRun("Run0", style, {}).advance == original_run0);
  REQUIRE(renderer.MeasureRun("Run1", style, {}).advance == original_run1);
  REQUIRE(renderer.MeasureRun("Run2", style, {}).advance == original_run2);
  renderer.Discard();
}

TEST_CASE("LinuxParagraphCacheRepeatedWrapsAreStable") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float unbounded = renderer.MeasureText("Hello World", style, 100000.0F, {.wrap = TextWrap::NoWrap}).size.width;
  const float narrow = unbounded * 0.6F;
  const TextLayoutMetrics first = renderer.MeasureText("Hello World", style, narrow, {.wrap = TextWrap::Word});
  const TextLayoutMetrics second = renderer.MeasureText("Hello World", style, narrow, {.wrap = TextWrap::Word});
  const TextLayoutMetrics third = renderer.MeasureText("Hello World", style, narrow, {.wrap = TextWrap::Word});
  REQUIRE(first.line_count == 2);
  REQUIRE(second.size.width == first.size.width);
  REQUIRE(second.line_count == first.line_count);
  REQUIRE(third.size.width == first.size.width);
  REQUIRE(third.line_count == first.line_count);
  renderer.Discard();
}

TEST_CASE("LinuxParagraphCacheSurvivesEviction") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  TextLayoutMetrics original;
  for (int index = 0; index < 300; ++index) {
    const TextLayoutMetrics metrics = renderer.MeasureText(
        "Paragraph number " + std::to_string(index) + " with some words to wrap",
        style,
        200.0F,
        {.wrap = TextWrap::Word}
    );
    REQUIRE(metrics.line_count > 0);
    if (index == 0) {
      original = metrics;
    }
  }
  // 300 paragraphs overflow kMaxParagraphs (256), evicting the earliest
  // entries; re-wrapping the first must reproduce its exact metrics.
  const TextLayoutMetrics repeated =
      renderer.MeasureText("Paragraph number 0 with some words to wrap", style, 200.0F, {.wrap = TextWrap::Word});
  REQUIRE(repeated.size.width == original.size.width);
  REQUIRE(repeated.line_count == original.line_count);
  renderer.Discard();
}

TEST_CASE("LinuxRendererRepeatedLifecycleIsStable") {
  // Three full Initialize/Metrics/Discard cycles exercise FcInit/FcFini
  // refcounting and ~State teardown of the hashed caches without crashing.
  detail::LinuxRenderer first;
  first.Initialize();
  REQUIRE(first.Metrics(Font::System(14.0F)).ascent > 0.0F);
  first.Discard();

  detail::LinuxRenderer second;
  second.Initialize();
  REQUIRE(second.Metrics(Font::System(14.0F)).ascent > 0.0F);
  second.Discard();

  detail::LinuxRenderer third;
  third.Initialize();
  REQUIRE(third.Metrics(Font::System(14.0F)).ascent > 0.0F);
  third.Discard();

  // A fresh renderer torn down without Initialize() must stay a no-op.
  detail::LinuxRenderer fresh;
  fresh.Discard();
}

} // namespace huxerui::test
