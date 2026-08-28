#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/resource.h>
#include <huxerui/text.h>

#include "image_test_support.h"
#include "linux_renderer.h"
#include "linux_text_renderer_internal.h"

#include "text_layout_internal.h"

namespace huxerui::test {
namespace {

constexpr std::uint32_t kDefaultBackgroundPixel = 0xFFF7F8FAU;

std::vector<std::uint32_t>
RenderPixels(detail::LinuxRenderer& renderer, const RenderFrame& frame, int width, int height) {
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
  SDL_Surface* surface =
      SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, pixels.data(), width * sizeof(std::uint32_t));
  REQUIRE(surface != nullptr);
  renderer.Draw(surface, frame);
  SDL_DestroySurface(surface);
  return pixels;
}

std::uint32_t RenderImagePixel(detail::LinuxRenderer& renderer, const ImageAsset& image) {
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 4.0F, 4.0F});
  paint.DrawImage(image, {0.0F, 0.0F, 4.0F, 4.0F}, ImageSampling::Nearest);
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  return RenderPixels(renderer, frame, 4, 4)[2 * 4 + 2];
}

} // namespace

TEST_CASE("LinuxRendererReplaysPaintCommandsIntoCpuBackbuffer") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 8.0F, 8.0F});
  paint.DrawRect({1.0F, 1.0F, 6.0F, 6.0F}, Color::Rgb(255, 0, 0));
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 8, 8);
  REQUIRE(pixels[4 * 8 + 4] == 0xFFFF0000U);
  REQUIRE(pixels[0] == kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererClearsEmptyFramesToThePlatformBackground") {
  detail::LinuxRenderer renderer;
  const RenderFrame frame{.damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 4, 3);
  REQUIRE(std::ranges::all_of(pixels, [](std::uint32_t pixel) { return pixel == kDefaultBackgroundPixel; }));
}

TEST_CASE("LinuxRendererDecodesAndDrawsPngAndJpegImages") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const ImageAsset png = ImageAsset::FromEncoded(MakeTestPng(2, 2));
  const std::vector<std::byte> jpeg_bytes = MakeTestJpeg();
  REQUIRE(jpeg_bytes.size() > 4);
  REQUIRE(jpeg_bytes[0] == std::byte{0xFF});
  REQUIRE(jpeg_bytes[1] == std::byte{0xD8});
  REQUIRE(jpeg_bytes[jpeg_bytes.size() - 2] == std::byte{0xFF});
  REQUIRE(jpeg_bytes.back() == std::byte{0xD9});
  const ImageAsset jpeg = ImageAsset::FromEncoded(jpeg_bytes);
  REQUIRE(RenderImagePixel(renderer, png) == 0xFF000000U);
  const std::uint32_t jpeg_pixel = RenderImagePixel(renderer, jpeg);
  REQUIRE(((jpeg_pixel >> 16U) & 0xFFU) > 0xE0U);
  REQUIRE(((jpeg_pixel >> 8U) & 0xFFU) < 0x20U);
  REQUIRE((jpeg_pixel & 0xFFU) < 0x20U);
  renderer.Discard();
}

TEST_CASE("LinuxRendererClipsParagraphTextToItsCommandRect") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 24.0F});
  paint.DrawText({2.0F, 2.0F, 16.0F, 20.0F}, "MMMM", {Font::System(18.0F), Color::White()});
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 32, 24);

  bool drew_inside = false;
  bool drew_outside = false;
  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < 32; ++x) {
      const bool painted = pixels[static_cast<std::size_t>(y * 32 + x)] != kDefaultBackgroundPixel;
      if (x >= 2 && x < 18 && y >= 2 && y < 22) {
        drew_inside = drew_inside || painted;
      } else {
        drew_outside = drew_outside || painted;
      }
    }
  }
  REQUIRE(drew_inside);
  REQUIRE_FALSE(drew_outside);
  renderer.Discard();
}

TEST_CASE("LinuxRendererArcOnlyDrawsItsRequestedSweep") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 32.0F});
  paint.DrawArc({16.0F, 16.0F}, 8.0F, 0.0F, 1.5707963F, Color::White(), 2.0F);
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 32, 32);
  REQUIRE(pixels[18 * 32 + 23] != kDefaultBackgroundPixel);
  REQUIRE(pixels[16 * 32 + 8] == kDefaultBackgroundPixel);
  REQUIRE(pixels[8 * 32 + 16] == kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererBorderOnlyDrawsTheRequestedBounds") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 32.0F});
  paint.DrawBorder({10.0F, 10.0F, 12.0F, 12.0F}, Color::White(), 2.0F);
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 32, 32);
  REQUIRE(pixels[16 * 32 + 9] == kDefaultBackgroundPixel);
  REQUIRE(pixels[16 * 32 + 10] != kDefaultBackgroundPixel);
  REQUIRE(pixels[16 * 32 + 12] == kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererDrawsNegativeArcSweepCounterclockwise") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 32.0F});
  paint.DrawArc({16.0F, 16.0F}, 8.0F, 0.0F, -1.5707963F, Color::White(), 3.0F);
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 32, 32);
  REQUIRE(pixels[10 * 32 + 22] != kDefaultBackgroundPixel);
  REQUIRE(pixels[22 * 32 + 22] == kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererHonorsPathStrokeCaps") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const auto render_cap = [&renderer](StrokeCap cap) {
    Path path;
    path.MoveTo({10.0F, 16.0F}).LineTo({22.0F, 16.0F});
    RenderNode root;
    PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 32.0F});
    paint.StrokePath(path, Color::White(), 4.0F, cap);
    paint.Finish();
    const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
    return RenderPixels(renderer, frame, 32, 32);
  };

  const std::vector<std::uint32_t> butt = render_cap(StrokeCap::Butt);
  const std::vector<std::uint32_t> round = render_cap(StrokeCap::Round);
  const std::vector<std::uint32_t> square = render_cap(StrokeCap::Square);
  REQUIRE(butt[16 * 32 + 8] == kDefaultBackgroundPixel);
  REQUIRE(round[16 * 32 + 8] != kDefaultBackgroundPixel);
  REQUIRE(square[16 * 32 + 8] != kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererTransformsStrokeGeometryUnderNonUniformScale") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  Path path;
  path.MoveTo({4.0F, 16.0F}).LineTo({12.0F, 16.0F});
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 32.0F, 16.0F});
  paint.PushTransform({.m11 = 2.0F, .m22 = 0.5F});
  paint.StrokePath(path, Color::White(), 4.0F);
  paint.PopTransform();
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 32, 16);
  REQUIRE(pixels[7 * 32 + 16] != kDefaultBackgroundPixel);
  REQUIRE(pixels[5 * 32 + 16] == kDefaultBackgroundPixel);
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

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 40, 40);
  REQUIRE(pixels[20 * 40 + 10] != kDefaultBackgroundPixel);
  REQUIRE(pixels[20 * 40 + 20] == kDefaultBackgroundPixel);
  REQUIRE(pixels[0] == kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererBlurredPathShadowExcludesTheShiftedCasterInterior") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  Path path;
  path.MoveTo({12.0F, 12.0F}).LineTo({28.0F, 12.0F}).LineTo({28.0F, 28.0F}).LineTo({12.0F, 28.0F}).Close();
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 48.0F, 40.0F});
  paint.DrawPathShadow(path, Color::Black(), {4.0F, 0.0F}, 6.0F);
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 48, 40);
  REQUIRE(pixels[20 * 48 + 14] != kDefaultBackgroundPixel);
  REQUIRE(pixels[20 * 48 + 20] == kDefaultBackgroundPixel);
  REQUIRE(pixels[0] == kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererDrawsZeroBlurRectAndPathShadows") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 40.0F, 20.0F});
  paint.DrawShadow({2.0F, 2.0F, 8.0F, 8.0F}, Color::White(), {2.0F, 0.0F}, 0.0F);
  Path path;
  path.MoveTo({20.0F, 2.0F}).LineTo({28.0F, 2.0F}).LineTo({28.0F, 10.0F}).LineTo({20.0F, 10.0F}).Close();
  paint.DrawPathShadow(path, Color::White(), {2.0F, 0.0F}, 0.0F);
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 40, 20);
  REQUIRE(pixels[6 * 40 + 3] == kDefaultBackgroundPixel);
  REQUIRE(pixels[6 * 40 + 5] != kDefaultBackgroundPixel);
  REQUIRE(pixels[6 * 40 + 21] == kDefaultBackgroundPixel);
  REQUIRE(pixels[6 * 40 + 23] != kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererPreservesOneToOneLinearImagePixels") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  constexpr std::array<std::uint8_t, 16> rgba{
      255,
      0,
      0,
      255,
      0,
      255,
      0,
      255,
      0,
      0,
      255,
      128,
      255,
      255,
      255,
      255,
  };
  const ImageAsset image = ImageAsset::FromEncoded(MakeTestPng(2, 2, rgba));
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 2.0F, 2.0F});
  paint.DrawImage(image, {0.0F, 0.0F, 2.0F, 2.0F}, ImageSampling::Linear);
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 2, 2);
  REQUIRE(pixels[0] == 0xFFFF0000U);
  REQUIRE(pixels[1] == 0xFF00FF00U);
  REQUIRE(pixels[2] == 0xFF7B7CFDU);
  REQUIRE(pixels[3] == 0xFFFFFFFFU);
  renderer.Discard();
}

TEST_CASE("LinuxRendererReplaysGradientsClipsAndEvenOddFills") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 20.0F, 8.0F});
  paint.PushClip({0.0F, 0.0F, 8.0F, 4.0F});
  paint.DrawLinearGradient(
      {0.0F, 0.0F, 8.0F, 8.0F},
      {.stops = {{0.0F, Color::Rgb(255, 0, 0)}, {1.0F, Color::Rgb(0, 0, 255)}}}
  );
  paint.PopClip();
  Path donut;
  donut.MoveTo({10.0F, 0.0F})
      .LineTo({18.0F, 0.0F})
      .LineTo({18.0F, 8.0F})
      .LineTo({10.0F, 8.0F})
      .Close()
      .MoveTo({12.0F, 2.0F})
      .LineTo({16.0F, 2.0F})
      .LineTo({16.0F, 6.0F})
      .LineTo({12.0F, 6.0F})
      .Close();
  paint.FillPath(donut, Color::White(), PathFillRule::EvenOdd);
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 20, 8);
  REQUIRE(((pixels[2 * 20 + 1] >> 16U) & 0xFFU) > (pixels[2 * 20 + 1] & 0xFFU));
  REQUIRE((pixels[2 * 20 + 6] & 0xFFU) > ((pixels[2 * 20 + 6] >> 16U) & 0xFFU));
  REQUIRE(pixels[6 * 20 + 4] == kDefaultBackgroundPixel);
  REQUIRE(pixels[1 * 20 + 11] != kDefaultBackgroundPixel);
  REQUIRE(pixels[4 * 20 + 14] == kDefaultBackgroundPixel);
  renderer.Discard();
}

TEST_CASE("LinuxRendererCompositesNodeOpacityOnce") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  RenderNode root;
  PaintContext root_paint(root.content, {0.0F, 0.0F, 4.0F, 4.0F});
  root_paint.DrawRect({0.0F, 0.0F, 4.0F, 4.0F}, Color::Rgb(255, 0, 0));
  root_paint.Finish();
  RenderNode child;
  child.opacity = 0.5F;
  PaintContext child_paint(child.content, {0.0F, 0.0F, 4.0F, 4.0F});
  child_paint.DrawRect({1.0F, 1.0F, 2.0F, 2.0F}, Color::White());
  child_paint.Finish();
  root.children.push_back(&child);
  const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};

  const std::vector<std::uint32_t> pixels = RenderPixels(renderer, frame, 4, 4);
  REQUIRE(pixels[2 * 4 + 2] == 0xFFFF8080U);
  REQUIRE(pixels[0] == 0xFFFF0000U);
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
      "\u05D0\u05D1\u05D2",
      style,
      200.0F,
      {.shaping = {.direction = TextDirection::RightToLeft}, .wrap = TextWrap::NoWrap}
  );
  const Rect left_to_right_start = left_to_right->CaretRect(0, TextAffinity::Downstream);
  const Rect left_to_right_end = left_to_right->CaretRect(3, TextAffinity::Downstream);
  const Rect right_to_left_start = right_to_left->CaretRect(0, TextAffinity::Downstream);
  const Rect right_to_left_end = right_to_left->CaretRect(3, TextAffinity::Downstream);
  REQUIRE(left_to_right->Measure().width > 0.0F);
  REQUIRE(right_to_left->Measure().width > 0.0F);
  REQUIRE(left_to_right_end.x > left_to_right_start.x);
  REQUIRE(right_to_left_start.x > right_to_left_end.x);
  renderer.Discard();
}

TEST_CASE("LinuxTextLayoutUsesDetectedDirectionForAutomaticAlignment") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const std::unique_ptr<detail::TextLayout> leading = renderer.CreateTextLayout(
      "\u05D0\u05D1\u05D2",
      style,
      200.0F,
      {.align = TextAlign::Leading, .wrap = TextWrap::NoWrap}
  );
  const std::unique_ptr<detail::TextLayout> trailing = renderer.CreateTextLayout(
      "\u05D0\u05D1\u05D2",
      style,
      200.0F,
      {.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap}
  );
  REQUIRE(leading->CaretRect(0, TextAffinity::Downstream).x > trailing->CaretRect(0, TextAffinity::Downstream).x);

  const std::unique_ptr<detail::TextLayout> weak_prefix_auto =
      renderer.CreateTextLayout("\u0661 abc", style, 200.0F, {.align = TextAlign::Leading, .wrap = TextWrap::NoWrap});
  const std::unique_ptr<detail::TextLayout> weak_prefix_trailing =
      renderer.CreateTextLayout("\u0661 abc", style, 200.0F, {.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap});
  REQUIRE(
      weak_prefix_auto->CaretRect(0, TextAffinity::Downstream).x <
      weak_prefix_trailing->CaretRect(0, TextAffinity::Downstream).x
  );
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
  const Rect hebrew_start = layout->CaretRect(4, TextAffinity::Downstream);
  const Rect hebrew_middle = layout->CaretRect(6, TextAffinity::Downstream);
  const Rect weak_start = layout->CaretRect(4, TextAffinity::Upstream);
  const Rect strong_end = layout->CaretRect(7, TextAffinity::Downstream);
  const Rect weak_end = layout->CaretRect(7, TextAffinity::Upstream);
  REQUIRE(hebrew_start.x > hebrew_middle.x);
  REQUIRE(hebrew_start.x != weak_start.x);
  REQUIRE(strong_end.x != weak_end.x);
  const TextPosition hit = layout->HitTest({(hebrew_start.x + hebrew_middle.x) * 0.5F, hebrew_start.y});
  REQUIRE(hit.offset >= 4);
  REQUIRE(hit.offset <= 7);
  renderer.Discard();
}

TEST_CASE("LinuxTextMeasurementFallsBackFromAnUnavailableNamedFont") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  constexpr std::string_view text = "\u05D0";
  const TextStyle style{Font::Named("HuxerUI Definitely Missing Font", 14.0F), Color::Black()};
  const TextRunMetrics metrics = renderer.MeasureRun(text, style, {});
  REQUIRE(metrics.advance > 0.0F);
  REQUIRE(metrics.visual_bounds.width > 0.0F);
  REQUIRE(metrics.visual_bounds.height > 0.0F);

  const TextRunMetrics fallback_metrics = renderer.MeasureRun(text, {Font::System(14.0F), Color::Black()}, {});
  REQUIRE(fallback_metrics.advance == metrics.advance);
  REQUIRE(fallback_metrics.visual_bounds == metrics.visual_bounds);
  renderer.Discard();
}

TEST_CASE("LinuxTextMeasurementUsesAGlyphFallbackInsteadOfTheMissingGlyphBox") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::Named("Noto Sans", 20.0F), Color::Black()};
  const TextShapingOptions shaping{.locale = "zh-CN"};
  const TextRunMetrics cjk = renderer.MeasureRun("\u4E16", style, shaping);
  const TextRunMetrics bold_cjk =
      renderer.MeasureRun("\u4E16", {style.font.WithWeight(FontWeight::Bold), Color::Black()}, shaping);
  const TextRunMetrics monospace_cjk = renderer.MeasureRun("\u4E16", {Font::Monospace(20.0F), Color::Black()}, shaping);
  const TextRunMetrics missing = renderer.MeasureRun("\xF4\x8F\xBF\xBF", style, shaping);
  REQUIRE(cjk.advance > 0.0F);
  REQUIRE(cjk.visual_bounds.width > 0.0F);
  REQUIRE(cjk.visual_bounds != missing.visual_bounds);
  REQUIRE(cjk.visual_bounds != bold_cjk.visual_bounds);
  REQUIRE(monospace_cjk.visual_bounds.width > 0.0F);
  renderer.Discard();
}

TEST_CASE("LinuxNamedFontSelectsRequestedWeightAndSlantFaces") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const Font regular = Font::Named("Noto Sans", 24.0F);
  const TextRunMetrics regular_metrics = renderer.MeasureRun("MMMM", {regular, Color::Black()}, {});
  const TextRunMetrics bold_metrics =
      renderer.MeasureRun("MMMM", {regular.WithWeight(FontWeight::Bold), Color::Black()}, {});
  const TextRunMetrics italic_metrics =
      renderer.MeasureRun("MMMM", {regular.WithSlant(FontSlant::Italic), Color::Black()}, {});
  REQUIRE(bold_metrics.visual_bounds != regular_metrics.visual_bounds);
  REQUIRE(italic_metrics.visual_bounds != regular_metrics.visual_bounds);
  renderer.Discard();
}

TEST_CASE("LinuxTextRendererRasterizesAtTheOutputScale") {
  detail::LinuxTextRenderer renderer;
  const TextStyle style{Font::System(14.0F), Color::White()};
  const detail::LinuxRenderedText one_x = renderer.Render("Sharp", style, 200.0F, {.wrap = TextWrap::NoWrap}, 1.0F);
  const detail::LinuxRenderedText two_x = renderer.Render("Sharp", style, 200.0F, {.wrap = TextWrap::NoWrap}, 2.0F);

  REQUIRE(one_x.surface != nullptr);
  REQUIRE(two_x.surface != nullptr);
  REQUIRE(one_x.raster_scale == 1.0F);
  REQUIRE(two_x.raster_scale == 2.0F);
  REQUIRE(two_x.surface->w > one_x.surface->w);
  REQUIRE(two_x.surface->h > one_x.surface->h);
  REQUIRE(two_x.metrics.size.width == Catch::Approx(one_x.metrics.size.width).margin(2.0F));
  REQUIRE(two_x.metrics.size.height == Catch::Approx(one_x.metrics.size.height).margin(2.0F));
}

TEST_CASE("LinuxWrappedCaretAffinitySelectsTheAdjacentVisualLine") {
  detail::LinuxRenderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float first_word_width = renderer.MeasureRun("one ", style, {}).advance;
  const std::unique_ptr<detail::TextLayout> layout =
      renderer.CreateTextLayout("one two", style, first_word_width + 0.1F, {.wrap = TextWrap::Word});
  const Rect upstream = layout->CaretRect(4, TextAffinity::Upstream);
  const Rect downstream = layout->CaretRect(4, TextAffinity::Downstream);
  REQUIRE(layout->Measure().height > upstream.height);
  REQUIRE(upstream.y < downstream.y);
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
  REQUIRE(rects[0].width > 0.0F);
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

TEST_CASE("LinuxRepeatedTextMeasurementsReturnIdenticalResults") {
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

TEST_CASE("LinuxRepeatedParagraphWrappingIsStable") {
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

TEST_CASE("LinuxRendererRepeatedLifecycleIsStable") {
  // Repeated SDL_ttf state creation and teardown must remain independent.
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
