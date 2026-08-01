#include "appkit_renderer.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "shadow_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {

namespace {

CFStringRef CreateString(std::string_view text) {
  return CFStringCreateWithBytes(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(text.data()),
      static_cast<CFIndex>(text.size()),
      kCFStringEncodingUTF8,
      false
  );
}

CTFontRef CreateFont(float font_size) {
  return CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, static_cast<CGFloat>(font_size), nullptr);
}

CFAttributedStringRef CreateAttributedString(std::string_view text, float font_size) {
  CFStringRef string = CreateString(text);
  CTFontRef font = CreateFont(font_size);
  const void* keys[] = {
      kCTFontAttributeName,
      kCTForegroundColorFromContextAttributeName,
  };
  const void* values[] = {
      font,
      kCFBooleanTrue,
  };
  CFDictionaryRef attributes = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  );
  CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
  CFRelease(attributes);
  CFRelease(font);
  CFRelease(string);
  return attributed;
}

CTLineRef CreateLine(std::string_view text, float font_size) {
  CFAttributedStringRef attributed = CreateAttributedString(text, font_size);
  CTLineRef line = CTLineCreateWithAttributedString(attributed);
  CFRelease(attributed);
  return line;
}

void SetFillColor(CGContextRef context, Color color) {
  CGContextSetRGBFillColor(context, color.red, color.green, color.blue, color.alpha);
}

void SetStrokeColor(CGContextRef context, Color color) {
  CGContextSetRGBStrokeColor(context, color.red, color.green, color.blue, color.alpha);
}

} // namespace

class MacTextLayout final : public TextLayout {
public:
  MacTextLayout(std::string_view text, float font_size, float max_width) {
    string_ = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    CTFontRef font = CreateFont(font_size);
    line_height_ = static_cast<float>(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
    CFRelease(font);

    if (!std::isfinite(max_width)) {
      line_ = CreateLine(text, font_size);
      CGFloat ascent = 0.0;
      CGFloat descent = 0.0;
      CGFloat leading = 0.0;
      const double width = CTLineGetTypographicBounds(line_, &ascent, &descent, &leading);
      size_ = {
          std::ceil(static_cast<float>(width)),
          std::ceil(std::max(line_height_, static_cast<float>(ascent + descent + leading))),
      };
      return;
    }

    const float width = std::max(1.0F, max_width);
    std::string layout_text{text};
    if (!layout_text.empty() && layout_text.back() == '\n') {
      layout_text.append("\xE2\x80\x8B");
    }
    CFAttributedStringRef attributed = CreateAttributedString(layout_text, font_size);
    CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attributed);
    const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
        framesetter,
        CFRangeMake(0, 0),
        nullptr,
        CGSizeMake(width, CGFLOAT_MAX),
        nullptr
    );
    const float height = std::max(line_height_, std::ceil(static_cast<float>(suggested.height)));
    CGPathRef path = CGPathCreateWithRect(CGRectMake(0.0, 0.0, width, height), nullptr);
    frame_ = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);
    size_ = {std::min(width, std::ceil(static_cast<float>(suggested.width))), height};

    CFArrayRef lines = CTFrameGetLines(frame_);
    const CFIndex count = CFArrayGetCount(lines);
    std::vector<CGPoint> origins(static_cast<std::size_t>(count));
    if (count > 0) {
      CTFrameGetLineOrigins(frame_, CFRangeMake(0, count), origins.data());
    }
    line_records_.reserve(static_cast<std::size_t>(count));
    for (CFIndex index = 0; index < count; ++index) {
      CTLineRef line = static_cast<CTLineRef>(const_cast<void*>(CFArrayGetValueAtIndex(lines, index)));
      CGFloat ascent = 0.0;
      CGFloat descent = 0.0;
      CGFloat leading = 0.0;
      static_cast<void>(CTLineGetTypographicBounds(line, &ascent, &descent, &leading));
      line_records_.push_back({
          line,
          origins[static_cast<std::size_t>(index)],
          CTLineGetStringRange(line),
          height - static_cast<float>(origins[static_cast<std::size_t>(index)].y) - static_cast<float>(ascent),
          std::max(line_height_, static_cast<float>(ascent + descent + leading)),
      });
    }
    if (line_records_.empty()) {
      line_ = CreateLine(layout_text, font_size);
    }

    CGPathRelease(path);
    CFRelease(framesetter);
    CFRelease(attributed);
  }

  ~MacTextLayout() override {
    if (line_ != nullptr) {
      CFRelease(line_);
    }
    if (frame_ != nullptr) {
      CFRelease(frame_);
    }
  }

  Size Measure() const override {
    return size_;
  }

  TextPosition HitTest(Point point) const override {
    CTLineRef line = line_;
    CGPoint origin{};
    CFRange range = CFRangeMake(0, static_cast<CFIndex>(string_.length));
    std::size_t selected_line = 0;
    if (!line_records_.empty()) {
      const LineRecord* selected = &line_records_.front();
      float distance = std::numeric_limits<float>::infinity();
      for (std::size_t index = 0; index < line_records_.size(); ++index) {
        const LineRecord& candidate = line_records_[index];
        const float bottom = candidate.top + candidate.height;
        const float candidate_distance =
            point.y < candidate.top ? candidate.top - point.y : (point.y > bottom ? point.y - bottom : 0.0F);
        if (candidate_distance < distance) {
          selected = &candidate;
          selected_line = index;
          distance = candidate_distance;
        }
      }
      line = selected->line;
      origin = selected->origin;
      range = selected->range;
    }
    CFIndex index = CTLineGetStringIndexForPosition(line, CGPointMake(point.x - origin.x, 0.0));
    if (index == kCFNotFound) {
      index = point.x <= origin.x ? range.location : range.location + range.length;
    }
    const CFIndex line_end = range.location + range.length;
    if (index == line_end && line_end <= static_cast<CFIndex>(string_.length) && range.length > 0 &&
        [string_ characterAtIndex:static_cast<NSUInteger>(line_end - 1)] == '\n') {
      --index;
    }
    const bool upstream = index == line_end && selected_line + 1 < line_records_.size() &&
                          line_records_[selected_line + 1].range.location == line_end;
    return {
        static_cast<TextOffset>(std::clamp<CFIndex>(index, 0, static_cast<CFIndex>(string_.length))),
        upstream ? TextAffinity::Upstream : TextAffinity::Downstream,
    };
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const CFIndex index = std::clamp<CFIndex>(static_cast<CFIndex>(offset), 0, static_cast<CFIndex>(string_.length));
    CTLineRef line = line_;
    CGPoint origin{};
    float top = 0.0F;
    float height = line_height_;
    if (!line_records_.empty()) {
      const LineRecord* selected = &line_records_.back();
      for (std::size_t line_index = 0; line_index < line_records_.size(); ++line_index) {
        const LineRecord& candidate = line_records_[line_index];
        const CFIndex start = candidate.range.location;
        const CFIndex end = start + candidate.range.length;
        if (index < end ||
            (index == end && (affinity == TextAffinity::Upstream || line_index + 1 == line_records_.size()))) {
          selected = &candidate;
          break;
        }
      }
      line = selected->line;
      origin = selected->origin;
      top = selected->top;
      height = selected->height;
    }
    CGFloat secondary = 0.0;
    const CGFloat primary = CTLineGetOffsetForStringIndex(line, index, &secondary);
    const CGFloat x = affinity == TextAffinity::Upstream && secondary != primary ? secondary : primary;
    return {
        static_cast<float>(origin.x + x),
        top,
        1.0F,
        std::ceil(height),
    };
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const CFIndex start =
        std::clamp<CFIndex>(static_cast<CFIndex>(range.start), 0, static_cast<CFIndex>(string_.length));
    const CFIndex end =
        std::clamp<CFIndex>(static_cast<CFIndex>(range.end), start, static_cast<CFIndex>(string_.length));
    if (start == end) {
      return {};
    }

    std::vector<Rect> rects;
    auto append_line = [&](CTLineRef line, CFRange line_range, CGPoint origin, float top, float height) {
      const CFIndex line_start = std::max(start, line_range.location);
      const CFIndex line_end = std::min(end, line_range.location + line_range.length);
      if (line_start >= line_end) {
        return;
      }
      CFArrayRef runs = CTLineGetGlyphRuns(line);
      const CFIndex count = CFArrayGetCount(runs);
      for (CFIndex index = 0; index < count; ++index) {
        CTRunRef run = static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs, index)));
        const CFRange run_range = CTRunGetStringRange(run);
        const CFIndex run_start = std::max(line_start, run_range.location);
        const CFIndex run_end = std::min(line_end, run_range.location + run_range.length);
        if (run_start >= run_end) {
          continue;
        }
        const CGFloat first = CTLineGetOffsetForStringIndex(line, run_start, nullptr);
        const CGFloat last = CTLineGetOffsetForStringIndex(line, run_end, nullptr);
        rects.push_back({
            static_cast<float>(origin.x + std::min(first, last)),
            top,
            static_cast<float>(std::abs(last - first)),
            std::ceil(height),
        });
      }
    };
    if (line_records_.empty()) {
      append_line(line_, CFRangeMake(0, static_cast<CFIndex>(string_.length)), {}, 0.0F, line_height_);
    } else {
      for (const LineRecord& line : line_records_) {
        append_line(line.line, line.range, line.origin, line.top, line.height);
      }
    }
    return rects;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const NSUInteger length = string_.length;
    const NSUInteger target = static_cast<NSUInteger>(std::clamp<TextOffset>(offset, 0, length));
    if (target == 0) {
      return 0;
    }
    return static_cast<TextOffset>([string_ rangeOfComposedCharacterSequenceAtIndex:target - 1].location);
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const NSUInteger length = string_.length;
    const NSUInteger target = static_cast<NSUInteger>(std::clamp<TextOffset>(offset, 0, length));
    if (target >= length) {
      return static_cast<TextOffset>(length);
    }
    return static_cast<TextOffset>(NSMaxRange([string_ rangeOfComposedCharacterSequenceAtIndex:target]));
  }

private:
  struct LineRecord {
    CTLineRef line = nullptr;
    CGPoint origin;
    CFRange range;
    float top = 0.0F;
    float height = 0.0F;
  };

  __strong NSString* string_ = nil;
  CTLineRef line_ = nullptr;
  CTFrameRef frame_ = nullptr;
  std::vector<LineRecord> line_records_;
  Size size_;
  float line_height_ = 0.0F;
};

Size AppKitRenderer::MeasureText(std::string_view text, float font_size, float max_width) {
  if (std::isfinite(max_width)) {
    if (max_width <= 0.0F) {
      return {};
    }
    CFAttributedStringRef attributed = CreateAttributedString(text, font_size);
    CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attributed);
    const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
        framesetter,
        CFRangeMake(0, 0),
        nullptr,
        CGSizeMake(max_width, CGFLOAT_MAX),
        nullptr
    );
    CFRelease(framesetter);
    CFRelease(attributed);
    return {
        std::ceil(std::min(static_cast<float>(suggested.width), max_width)),
        std::ceil(static_cast<float>(suggested.height)),
    };
  }

  CTLineRef line = CreateLine(text, font_size);
  CGFloat ascent = 0.0;
  CGFloat descent = 0.0;
  CGFloat leading = 0.0;
  const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
  CFRelease(line);

  return {
      std::ceil(static_cast<float>(width)),
      std::ceil(static_cast<float>(ascent + descent + leading)),
  };
}

std::unique_ptr<TextLayout> AppKitRenderer::CreateTextLayout(std::string_view text, float font_size, float max_width) {
  return std::make_unique<MacTextLayout>(text, font_size, max_width);
}

void AppKitRenderer::RenderSequence(const PaintSequence& sequence, CGContextRef context) {
  for (const PaintCommand& command : sequence.Commands()) {
    std::visit([this, context](const auto& value) { RenderCommand(context, value); }, command);
  }
}

void AppKitRenderer::RenderSceneNode(const RenderNode& node, CGContextRef context) {
  const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
  if (!node.visible || opacity <= 0.0F) {
    return;
  }

  Transform2D transform = node.transform;
  transform.translate_x += node.offset.x;
  transform.translate_y += node.offset.y;
  const bool transformed = !transform.IsIdentity();
  if (transformed) {
    RenderCommand(context, PushTransformCommand{transform});
  }

  const bool translucent = opacity < 1.0F;
  if (translucent) {
    CGContextSaveGState(context);
    CGContextSetAlpha(context, opacity);
    CGContextBeginTransparencyLayer(context, nullptr);
  }

  RenderSequence(node.content, context);
  if (node.child_clip.has_value()) {
    RenderCommand(
        context,
        PushClipCommand{
            node.child_clip->rect,
            node.child_clip->corner_radius,
        }
    );
  }
  const bool children_transformed = !node.children_transform.IsIdentity();
  if (children_transformed) {
    RenderCommand(context, PushTransformCommand{node.children_transform});
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      RenderSceneNode(*child, context);
    }
  }
  if (children_transformed) {
    RenderCommand(context, PopTransformCommand{});
  }
  if (node.child_clip.has_value()) {
    RenderCommand(context, PopClipCommand{});
  }
  RenderSequence(node.foreground, context);
  if (translucent) {
    CGContextEndTransparencyLayer(context);
    CGContextRestoreGState(context);
  }
  if (transformed) {
    RenderCommand(context, PopTransformCommand{});
  }
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawRectCommand& command) {
  SetFillColor(context, command.color);
  const CGRect rect = CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height);
  if (command.corner_radius > 0.0F) {
    CGPathRef path = CGPathCreateWithRoundedRect(rect, command.corner_radius, command.corner_radius, nullptr);
    CGContextAddPath(context, path);
    CGContextFillPath(context);
    CGPathRelease(path);
  } else {
    CGContextFillRect(context, rect);
  }
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawTextCommand& command) {
  if (command.rect.width <= 0.0F || command.rect.height <= 0.0F || command.color.alpha <= 0.0F) {
    return;
  }
  if (command.align == TextAlign::Leading) {
    CFAttributedStringRef attributed = CreateAttributedString(command.text, command.font_size);
    CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attributed);
    CGPathRef path = CGPathCreateWithRect(CGRectMake(0.0, 0.0, command.rect.width, command.rect.height), nullptr);
    CTFrameRef frame = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);

    CGContextSaveGState(context);
    CGContextTranslateCTM(context, command.rect.x, command.rect.y + command.rect.height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    SetFillColor(context, command.color);
    CTFrameDraw(frame, context);
    CGContextRestoreGState(context);

    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(framesetter);
    CFRelease(attributed);
    return;
  }

  CTLineRef line = CreateLine(command.text, command.font_size);
  CGFloat ascent = 0.0;
  CGFloat descent = 0.0;
  CGFloat leading = 0.0;
  const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
  const CGFloat text_height = ascent + descent + leading;

  CGFloat x = command.rect.x;
  if (command.align == TextAlign::Center) {
    x += std::max(0.0F, (command.rect.width - static_cast<float>(width)) * 0.5F);
  }

  const CGFloat bottom_padding = std::max(0.0F, (command.rect.height - static_cast<float>(text_height)) * 0.5F);
  const CGFloat baseline_from_bottom = bottom_padding + descent;

  CGContextSaveGState(context);
  CGContextClipToRect(context, CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height));
  CGContextTranslateCTM(context, 0.0, command.rect.y + command.rect.height);
  CGContextScaleCTM(context, 1.0, -1.0);
  CGContextSetTextMatrix(context, CGAffineTransformIdentity);
  SetFillColor(context, command.color);
  CGContextSetTextPosition(context, x, baseline_from_bottom);
  CTLineDraw(line, context);
  CGContextRestoreGState(context);
  CFRelease(line);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawCircleCommand& command) {
  if (command.radius <= 0.0F || command.color.alpha <= 0.0F) {
    return;
  }
  SetFillColor(context, command.color);
  const float diameter = command.radius * 2.0F;
  CGContextFillEllipseInRect(
      context,
      CGRectMake(command.center.x - command.radius, command.center.y - command.radius, diameter, diameter)
  );
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawArcCommand& command) {
  if (command.radius <= 0.0F || command.width <= 0.0F || command.color.alpha <= 0.0F ||
      !std::isfinite(command.start_angle) || !std::isfinite(command.sweep_angle) || command.sweep_angle == 0.0F) {
    return;
  }

  CGLineCap cap = kCGLineCapButt;
  if (command.cap == StrokeCap::Round) {
    cap = kCGLineCapRound;
  } else if (command.cap == StrokeCap::Square) {
    cap = kCGLineCapSquare;
  }

  CGMutablePathRef path = CGPathCreateMutable();
  CGPathAddRelativeArc(
      path,
      nullptr,
      command.center.x,
      command.center.y,
      command.radius,
      command.start_angle,
      command.sweep_angle
  );
  CGContextSaveGState(context);
  SetStrokeColor(context, command.color);
  CGContextSetLineWidth(context, command.width);
  CGContextSetLineCap(context, cap);
  CGContextAddPath(context, path);
  CGContextStrokePath(context);
  CGContextRestoreGState(context);
  CGPathRelease(path);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawBorderCommand& command) {
  if (command.width <= 0.0F || command.color.alpha <= 0.0F) {
    return;
  }
  const float inset = command.width * 0.5F;
  const CGRect rect = CGRectMake(
      command.rect.x + inset,
      command.rect.y + inset,
      std::max(0.0F, command.rect.width - command.width),
      std::max(0.0F, command.rect.height - command.width)
  );
  const float radius = std::max(0.0F, command.corner_radius - inset);
  CGPathRef path = CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
  CGContextSaveGState(context);
  SetStrokeColor(context, command.color);
  CGContextSetLineWidth(context, command.width);
  CGContextAddPath(context, path);
  CGContextStrokePath(context);
  CGContextRestoreGState(context);
  CGPathRelease(path);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawShadowCommand& command) {
  const ResolvedShadow resolved = ResolveShadow(command);
  if (resolved.IsEmpty() || command.color.alpha <= 0.0F) {
    return;
  }
  if (command.blur_radius <= 0.0F) {
    RenderCommand(context, DrawRectCommand{resolved.caster, command.color, resolved.corner_radius});
    return;
  }

  const CGRect caster = CGRectMake(resolved.caster.x, resolved.caster.y, resolved.caster.width, resolved.caster.height);
  CGPathRef caster_path = CGPathCreateWithRoundedRect(caster, resolved.corner_radius, resolved.corner_radius, nullptr);
  CGMutablePathRef outer_clip = CGPathCreateMutable();
  CGPathAddRect(outer_clip, nullptr, CGContextGetClipBoundingBox(context));
  CGPathAddPath(outer_clip, nullptr, caster_path);
  CGColorRef shadow_color =
      CGColorCreateGenericRGB(command.color.red, command.color.green, command.color.blue, command.color.alpha);

  CGContextSaveGState(context);
  CGContextAddPath(context, outer_clip);
  CGContextEOClip(context);
  CGContextSetShadowWithColor(context, CGSizeZero, static_cast<CGFloat>(resolved.standard_deviation), shadow_color);
  CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
  CGContextAddPath(context, caster_path);
  CGContextFillPath(context);
  CGContextRestoreGState(context);

  CGContextSaveGState(context);
  CGContextSetFillColorWithColor(context, shadow_color);
  CGContextAddPath(context, caster_path);
  CGContextFillPath(context);
  CGContextRestoreGState(context);

  CGColorRelease(shadow_color);
  CGPathRelease(outer_clip);
  CGPathRelease(caster_path);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const PushClipCommand& command) {
  CGContextSaveGState(context);
  const CGRect rect = CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height);
  if (command.corner_radius <= 0.0F) {
    CGContextClipToRect(context, rect);
    return;
  }
  const float radius = std::min(command.corner_radius, std::min(command.rect.width, command.rect.height) * 0.5F);
  CGPathRef path = CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
  CGContextAddPath(context, path);
  CGContextClip(context);
  CGPathRelease(path);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const PopClipCommand& command) {
  static_cast<void>(command);
  CGContextRestoreGState(context);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const PushTransformCommand& command) {
  CGContextSaveGState(context);
  CGContextConcatCTM(
      context,
      CGAffineTransformMake(
          command.transform.m11,
          command.transform.m12,
          command.transform.m21,
          command.transform.m22,
          command.transform.translate_x,
          command.transform.translate_y
      )
  );
}

void AppKitRenderer::RenderCommand(CGContextRef context, const PopTransformCommand& command) {
  static_cast<void>(command);
  CGContextRestoreGState(context);
}

void AppKitRenderer::Draw(CGContextRef context, CGRect dirty_rect, const RenderFrame* frame) {
  CGContextSaveGState(context);
  CGContextClipToRect(context, dirty_rect);
  SetFillColor(context, Color::Rgb(247, 248, 250));
  CGContextFillRect(context, dirty_rect);
  if (frame != nullptr && frame->scene.root != nullptr) {
    RenderSceneNode(*frame->scene.root, context);
  }
  CGContextRestoreGState(context);
}

} // namespace huxerui::detail
