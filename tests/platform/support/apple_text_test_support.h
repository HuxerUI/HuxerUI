#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/paint.h>
#include <huxerui/render_scene.h>

#include "text/text_internal.h"

namespace huxerui::test {

inline const TextLayoutOptions centered_apple_text_options{
    .shaping = {.locale = "zh-Hans"},
    .align = TextAlign::Center,
    .vertical_align = TextVerticalAlign::Center,
};

template <class Renderer>
std::vector<std::uint8_t> RasterizeAppleText(Renderer& renderer, const AttributedText& text, const TextStyle& style,
    const TextLayoutOptions& options, Rect bounds, Size viewport, int scale) {
  RenderNode root;
  PaintContext paint(root.content, {0, 0, viewport.width, viewport.height});
  paint.DrawText(bounds, text, style, options);
  paint.Finish();
  const RenderFrame frame{.scene = {.root = &root}};
  const auto width = static_cast<std::size_t>(viewport.width * scale);
  const auto height = static_cast<std::size_t>(viewport.height * scale);
  std::vector<std::uint8_t> pixels(width * height * 4);
  CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, colors,
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(colors);
  if (!context) {
    return {};
  }
  CGContextTranslateCTM(context, 0, height);
  CGContextScaleCTM(context, scale, -scale);
  renderer.DrawSlice(context, CGRectMake(0, 0, viewport.width, viewport.height), &frame, 0, 1, false);
  CGContextRelease(context);
  return pixels;
}

template <class Renderer>
std::string_view CheckAppleFallbackTextCentering() {
  const AttributedText text("完成");
  const TextStyle style{Font::System(13.0F).WithWeight(FontWeight::Medium), Color::Black()};
  constexpr Size viewport{76, 36};
  constexpr int scale = 4;
  constexpr int pixel_width = static_cast<int>(viewport.width) * scale;
  constexpr int pixel_height = static_cast<int>(viewport.height) * scale;
  for (const auto wrap : {TextWrap::Word, TextWrap::NoWrap}) {
    Renderer renderer;
    auto options = centered_apple_text_options;
    options.wrap = wrap;
    Rect bounds{0, 0, viewport.width, viewport.height};
    if (wrap == TextWrap::Word) {
      // A Row centers its naturally measured Text child. A Button centers a NoWrap label within its own bounds.
      bounds.height = renderer.MeasureText(text, style, bounds.width, options).size.height;
      bounds.y = (viewport.height - bounds.height) * 0.5F;
      options.vertical_align = TextVerticalAlign::Top;
    }
    const auto pixels = RasterizeAppleText(renderer, text, style, options, bounds, viewport, scale);
    if (pixels.empty()) {
      return "Could not create the bitmap";
    }
    int first_row = pixel_height;
    int last_row = -1;
    for (int row = 0; row < pixel_height; ++row) {
      for (int col = 0; col < pixel_width; ++col) {
        if (pixels[(row * pixel_width + col) * 4 + 3] >= 128) {
          first_row = std::min(first_row, row);
          last_row = row;
          break;
        }
      }
    }
    const float center = (first_row + last_row + 1) / (2.0F * scale);
    if (last_row < 0 || std::abs(center - viewport.height * 0.5F) > 0.5F) {
      return wrap == TextWrap::Word ? "Natural-height Text is off center" : "Fixed-height label is off center";
    }
  }
  return {};
}

template <class Renderer>
std::string_view CheckAppleParagraphReplay() {
  const TextStyle style{Font::System(13.0F).WithWeight(FontWeight::Medium), Color::Black()};
  struct Sample {
    std::string_view text;
    float width;
    TextWrap wrap;
    std::size_t lines;
  };
  for (const Sample& sample : {
           Sample{"完成Ag", 76, TextWrap::NoWrap, 1},
           Sample{"完成Ag", 76, TextWrap::Word, 1},
           Sample{"完成后继续 Ag 编辑", 38, TextWrap::Word, 4},
           Sample{"完成\n继续Ag", 76, TextWrap::NoWrap, 2},
           Sample{"完成\n", 76, TextWrap::Word, 2},
       }) {
    Renderer direct;
    Renderer measured;
    auto options = centered_apple_text_options;
    options.wrap = sample.wrap;
    const AttributedText text{std::string(sample.text)};
    const float measure_width = sample.wrap == TextWrap::Word ? sample.width : std::numeric_limits<float>::infinity();
    const auto metrics = measured.MeasureText(text, style, measure_width, options);
    const auto layout = measured.CreatePlatformTextLayout(text, style, sample.width, options);
    const auto lines = layout->LineMetrics();
    if (metrics.line_count != sample.lines || lines.size() != sample.lines ||
        metrics.size.height != layout->Measure().height || metrics.first_baseline != lines.front().baseline ||
        metrics.last_baseline != lines.back().baseline) {
      return sample.text;
    }
    if (sample.lines == 4) {
      const Rect first = lines.front().bounds;
      const auto caret = layout->CaretRect(0, TextAffinity::Downstream);
      const auto selection = layout->RangeRects({0, 1});
      if (first.height >= lines[2].bounds.height || caret.y != first.y || caret.height != std::ceil(first.height) ||
          selection.empty() || selection.front().y != first.y || selection.front().height != caret.height) {
        return "Fallback line, caret and selection heights disagree";
      }
    }
    if (sample.text.back() == '\n' &&
        (lines.back().range != TextRange{text.Length(), text.Length()} ||
         layout->CaretRect(text.Length(), TextAffinity::Downstream).height <= 0.0F)) {
      return sample.text;
    }
    for (const float height : {metrics.size.height, metrics.size.height + 8.0F, 14.0F}) {
      const Rect bounds{0, 0, sample.width, height};
      const Size viewport{sample.width, height};
      const auto cold = RasterizeAppleText(direct, text, style, options, bounds, viewport, 1);
      const auto warm = RasterizeAppleText(measured, text, style, options, bounds, viewport, 1);
      if (cold.empty() || cold != warm || measured.MeasureText(text, style, measure_width, options) != metrics) {
        return sample.text;
      }
    }
  }
  return {};
}

} // namespace huxerui::test
