#include <catch2/catch_amalgamated.hpp>

#include <cstdint>
#include <limits>
#include <string>

#include <huxerui/paint.h>
#include <huxerui/text.h>

#include "linux_renderer.h"

#include "text_layout_internal.h"

namespace huxerui::test {

TEST_CASE("LinuxRendererReplaysPaintCommandsIntoCairoSurface") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 8.0F, 8.0F});
  paint.DrawRect({1.0F, 1.0F, 6.0F, 6.0F}, Color::Rgb(255, 0, 0));
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  REQUIRE(pixels[4 * 8 + 4] == 0xFFFF0000U);
  REQUIRE(pixels[0] == 0U);
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererTransformsLinearAndRadialGradientSamplingWithoutTransformingGeometry") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const Transform2D quarter_turn{0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 0.0F};
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 44.0F, 20.0F});
  paint.DrawLinearGradient(
      {0.0F, 0.0F, 20.0F, 20.0F},
      LinearGradient{
          .start = {0.0F, 0.0F},
          .end = {1.0F, 0.0F},
          .stops = {{0.0F, Color::Rgb(255, 0, 0)}, {1.0F, Color::Rgb(0, 0, 255)}},
          .transform = quarter_turn,
      }
  );
  paint.DrawRadialGradient(
      {24.0F, 0.0F, 20.0F, 20.0F},
      RadialGradient{
          .radius = {0.4F, 0.15F},
          .stops = {{0.0F, Color::White()}, {1.0F, Color::Black()}},
          .transform = quarter_turn,
      }
  );
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 44, 20);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  const std::uint32_t linear_top = pixels[2 * 44 + 10];
  const std::uint32_t linear_bottom = pixels[17 * 44 + 10];
  REQUIRE(((linear_top >> 16U) & 0xFFU) > (linear_top & 0xFFU));
  REQUIRE((linear_bottom & 0xFFU) > ((linear_bottom >> 16U) & 0xFFU));
  const std::uint32_t radial_horizontal = pixels[10 * 44 + 39];
  const std::uint32_t radial_vertical = pixels[15 * 44 + 34];
  REQUIRE((radial_vertical & 0xFFU) > (radial_horizontal & 0xFFU));
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererStrokesPathsWithLinearAndRadialGradients") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 48.0F, 24.0F});
  paint.StrokePath(Path::RoundedRect({2.0F, 2.0F, 18.0F, 18.0F}, CornerRadii{2.0F}),
                   LinearGradient{
                       .start = {0.0F, 0.0F},
                       .end = {0.0F, 1.0F},
                       .stops = {{0.0F, Color::Rgb(255, 0, 0)}, {1.0F, Color::Rgb(0, 0, 255)}},
                   },
                   StrokeStyle{.width = 3.0F, .join = StrokeJoin::Round});
  paint.StrokePath(Path{}.MoveTo({34.0F, 12.0F}).LineTo({44.0F, 12.0F}),
                   RadialGradient{
                       .stops = {{0.0F, Color::White()}, {1.0F, Color::Black()}},
                   },
                   {24.0F, 2.0F, 20.0F, 20.0F}, StrokeStyle{.width = 3.0F, .cap = StrokeCap::Round});
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 24);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  const std::uint32_t linear_top = pixels[2 * 48 + 11];
  const std::uint32_t linear_bottom = pixels[19 * 48 + 11];
  REQUIRE(((linear_top >> 16U) & 0xFFU) > (linear_top & 0xFFU));
  REQUIRE((linear_bottom & 0xFFU) > ((linear_bottom >> 16U) & 0xFFU));
  const std::uint32_t radial_center = pixels[12 * 48 + 34];
  const std::uint32_t radial_edge = pixels[12 * 48 + 43];
  REQUIRE((radial_center & 0xFFU) > (radial_edge & 0xFFU));
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererDrawsDirectedDashedLines") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 40.0F, 16.0F});
  paint.DrawLine({4.0F, 8.0F}, {36.0F, 8.0F}, Color::White(),
                 StrokeStyle{.width = 2.0F, .dash_pattern = {4.0F, 4.0F}});
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 40, 16);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  REQUIRE((pixels[8 * 40 + 5] >> 24U) > 0U);
  REQUIRE(pixels[8 * 40 + 10] == 0U);
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererArcDoesNotJoinAnExistingCairoPath") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 32.0F});
  paint.DrawArc({24.0F, 24.0F}, 4.0F, 0.0F, 1.5707963F, Color::White(), StrokeStyle{.width = 2.0F});
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  cairo_move_to(context, 0.0, 0.0);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  REQUIRE(pixels[12 * 32 + 14] == 0U);
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererBorderDoesNotStrokeAnExistingCairoPath") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 32.0F});
  paint.DrawBorder({20.0F, 20.0F, 10.0F, 10.0F}, Color::White(), StrokeStyle{.width = 2.0F});
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  cairo_move_to(context, 2.0, 2.0);
  cairo_line_to(context, 12.0, 12.0);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  REQUIRE(pixels[7 * 32 + 7] == 0U);
  REQUIRE((pixels[20 * 32 + 24] >> 24U) > 0U);
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererDrawsNegativeArcSweepCounterclockwise") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 32.0F});
  paint.DrawArc({16.0F, 16.0F}, 8.0F, 0.0F, -1.5707963F, Color::White(), StrokeStyle{.width = 3.0F});
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  REQUIRE((pixels[10 * 32 + 22] >> 24U) > 0U);
  REQUIRE(pixels[22 * 32 + 22] == 0U);
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererBlurredRectShadowExcludesTheCasterInterior") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 40.0F, 40.0F});
  paint.DrawShadow({12.0F, 12.0F, 16.0F, 16.0F}, Color::Black(), {}, 6.0F);
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 40, 40);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  REQUIRE((pixels[20 * 40 + 10] >> 24U) > 0U);
  REQUIRE(pixels[20 * 40 + 20] == 0U);
  REQUIRE(pixels[0] == 0U);
  cairo_surface_destroy(surface);
  renderer.Discard();
}

TEST_CASE("LinuxRendererBlurredPathShadowExcludesTheShiftedCasterInterior") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  Path path;
  path.MoveTo({12.0F, 12.0F})
      .LineTo({28.0F, 12.0F})
      .LineTo({28.0F, 28.0F})
      .LineTo({12.0F, 28.0F})
      .Close();
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 48.0F, 40.0F});
  paint.DrawPathShadow(path, Color::Black(), {4.0F, 0.0F}, 6.0F);
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 40);
  REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface);
  renderer.Draw(context, frame);
  cairo_destroy(context);
  cairo_surface_flush(surface);

  const auto* pixels = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(surface));
  REQUIRE((pixels[20 * 48 + 14] >> 24U) > 0U);
  REQUIRE(pixels[20 * 48 + 20] == 0U);
  REQUIRE(pixels[0] == 0U);
  cairo_surface_destroy(surface);
  renderer.Discard();
}

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
  const Rect left_to_right_start = left_to_right->CaretRect(0, TextAffinity::Downstream);
  const Rect left_to_right_end = left_to_right->CaretRect(3, TextAffinity::Downstream);
  const Rect right_to_left_start = right_to_left->CaretRect(0, TextAffinity::Downstream);
  const Rect right_to_left_end = right_to_left->CaretRect(3, TextAffinity::Downstream);
  REQUIRE(left_to_right_end.x > left_to_right_start.x);
  REQUIRE(right_to_left_start.x > right_to_left_end.x);
  renderer.Discard();
}

TEST_CASE("LinuxTextLayoutReturnsDisjointRangesForMixedDirectionText") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const std::unique_ptr<detail::TextLayout> layout =
      renderer.CreateTextLayout("abc \u05D0\u05D1\u05D2 def", style, 300.0F, {.wrap = TextWrap::NoWrap});
  const std::vector<Rect> rects = layout->RangeRects({1, 6});

  REQUIRE(rects.size() == 2);
  REQUIRE(rects[0].width > 0.0F);
  REQUIRE(rects[1].width > 0.0F);
  REQUIRE_FALSE(rects[0].Intersects(rects[1]));
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
  // Repeated Pango/Cairo state creation and teardown must remain independent.
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
