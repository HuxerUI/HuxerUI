#include "uikit_renderer.h"
#include "internal_access.h"

#import <UIKit/UIKit.h>
#import <CoreImage/CoreImage.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ios_external_texture_internal.h"
#include "path_internal.h"
#include "paint_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"
#include "text_internal.h"

namespace huxerui::detail {

namespace {

template <typename T> class CFRef {
public:
  explicit CFRef(T value = nullptr) noexcept : value_(value) {}

  ~CFRef() {
    if (value_ != nullptr) {
      CFRelease(value_);
    }
  }

  CFRef(const CFRef&) = delete;
  CFRef& operator=(const CFRef&) = delete;

  CFRef(CFRef&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

  CFRef& operator=(CFRef&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr) {
        CFRelease(value_);
      }
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] T Get() const noexcept {
    return value_;
  }

private:
  T value_ = nullptr;
};

CFRef<CGGradientRef> CreateGradient(const std::vector<GradientStop>& stops) {
  std::vector<CGFloat> components;
  std::vector<CGFloat> locations;
  components.reserve(stops.size() * 4);
  locations.reserve(stops.size());
  for (const GradientStop& stop : stops) {
    components.push_back(stop.color.red);
    components.push_back(stop.color.green);
    components.push_back(stop.color.blue);
    components.push_back(stop.color.alpha);
    locations.push_back(stop.offset);
  }
  CFRef<CGColorSpaceRef> color_space(CGColorSpaceCreateDeviceRGB());
  return CFRef<CGGradientRef>(CGGradientCreateWithColorComponents(
      color_space.Get(), components.data(), locations.data(), stops.size()
  ));
}

void ClipGradientRect(CGContextRef context, Rect rect, float corner_radius) {
  const CGRect native = CGRectMake(rect.x, rect.y, rect.width, rect.height);
  if (corner_radius <= 0.0F) {
    CGContextClipToRect(context, native);
    return;
  }
  CFRef<CGPathRef> path(CGPathCreateWithRoundedRect(native, corner_radius, corner_radius, nullptr));
  CGContextAddPath(context, path.Get());
  CGContextClip(context);
}

template <class Gradient> void ConcatGradientTransform(CGContextRef context, Rect rect, const Gradient& gradient) {
  const Transform2D transform = ResolveGradientTransform(rect, gradient);
  CGContextConcatCTM(
      context, CGAffineTransformMake(transform.m11, transform.m12, transform.m21, transform.m22,
                                     transform.translate_x, transform.translate_y)
  );
}

void DrawGradient(CGContextRef context, Rect bounds, const LinearGradient& value) {
  const CFRef<CGGradientRef> gradient = CreateGradient(value.stops);
  if (gradient.Get() == nullptr) {
    return;
  }
  ConcatGradientTransform(context, bounds, value);
  CGContextDrawLinearGradient(context, gradient.Get(), CGPointMake(value.start.x, value.start.y),
                              CGPointMake(value.end.x, value.end.y),
                              kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
}

void DrawGradient(CGContextRef context, Rect bounds, const RadialGradient& value) {
  const CFRef<CGGradientRef> gradient = CreateGradient(value.stops);
  if (gradient.Get() == nullptr) {
    return;
  }
  const CGPoint center = CGPointMake(value.center.x, value.center.y);
  ConcatGradientTransform(context, bounds, value);
  CGContextDrawRadialGradient(context, gradient.Get(), center, 0.0, center, value.radius.width,
                              kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
}

void CombineHash(std::size_t& seed, std::size_t value) noexcept {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

std::size_t HashFont(const Font& font) noexcept {
  std::size_t result = std::hash<FontFamilyKind>{}(font.FamilyKind());
  CombineHash(result, std::hash<std::string_view>{}(font.FamilyName()));
  CombineHash(result, std::hash<float>{}(font.Size()));
  CombineHash(result, std::hash<FontWeight>{}(font.Weight()));
  CombineHash(result, std::hash<FontSlant>{}(font.Slant()));
  return result;
}

std::size_t HashShaping(const TextShapingOptions& shaping) noexcept {
  std::size_t result = std::hash<TextDirection>{}(shaping.direction);
  CombineHash(result, std::hash<std::string>{}(shaping.locale));
  return result;
}

CFStringRef CreateString(std::string_view text) {
  return CFStringCreateWithBytes(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(text.data()),
      static_cast<CFIndex>(text.size()),
      kCFStringEncodingUTF8,
      false
  );
}

CTFontRef CreateFont(const Font& font) {
  CTFontRef base = nullptr;
  switch (font.FamilyKind()) {
  case FontFamilyKind::System:
    base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, static_cast<CGFloat>(font.Size()), nullptr);
    break;
  case FontFamilyKind::Monospace:
    base = CTFontCreateWithName(CFSTR("Menlo"), static_cast<CGFloat>(font.Size()), nullptr);
    break;
  case FontFamilyKind::Named: {
    CFStringRef family = CreateString(font.FamilyName());
    base = CTFontCreateWithName(family, static_cast<CGFloat>(font.Size()), nullptr);
    CFRelease(family);
    break;
  }
  }

  const double raw_weight = static_cast<double>(font.Weight());
  const double normalized_weight = raw_weight >= 400.0 ? (raw_weight - 400.0) / 500.0 : (raw_weight - 400.0) / 300.0;
  const double slant = font.Slant() == FontSlant::Italic ? 1.0 : 0.0;
  CFNumberRef weight_value = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &normalized_weight);
  CFNumberRef slant_value = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &slant);
  const void* trait_keys[] = {kCTFontWeightTrait, kCTFontSlantTrait};
  const void* trait_values[] = {weight_value, slant_value};
  CFDictionaryRef traits = CFDictionaryCreate(
      kCFAllocatorDefault,
      trait_keys,
      trait_values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  );
  const void* attribute_keys[] = {kCTFontTraitsAttribute};
  const void* attribute_values[] = {traits};
  CFDictionaryRef attributes = CFDictionaryCreate(
      kCFAllocatorDefault,
      attribute_keys,
      attribute_values,
      1,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  );
  CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
  CTFontRef result = CTFontCreateCopyWithAttributes(base, static_cast<CGFloat>(font.Size()), nullptr, descriptor);
  CFRelease(descriptor);
  CFRelease(attributes);
  CFRelease(traits);
  CFRelease(slant_value);
  CFRelease(weight_value);
  CFRelease(base);
  return result;
}

TextDirection ResolveTextDirection(std::string_view text, TextDirection requested, CTFontRef font) {
  if (requested != TextDirection::Auto) {
    return requested;
  }
  if (text.empty()) {
    return TextDirection::LeftToRight;
  }

  CFRef<CFStringRef> string{CreateString(text)};
  const void* keys[] = {kCTFontAttributeName};
  const void* values[] = {font};
  CFRef<CFDictionaryRef> attributes{CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      1,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  )};
  CFRef<CFAttributedStringRef> attributed{
      CFAttributedStringCreate(kCFAllocatorDefault, string.Get(), attributes.Get())};
  CFRef<CTLineRef> line{CTLineCreateWithAttributedString(attributed.Get())};
  CFArrayRef runs = CTLineGetGlyphRuns(line.Get());
  CFCharacterSetRef letters = CFCharacterSetGetPredefined(kCFCharacterSetLetter);
  CFIndex earliest_location = std::numeric_limits<CFIndex>::max();
  TextDirection resolved = TextDirection::LeftToRight;
  for (CFIndex index = 0; index < CFArrayGetCount(runs); ++index) {
    CTRunRef run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, index));
    const CFRange range = CTRunGetStringRange(run);
    CFIndex strong_location = std::numeric_limits<CFIndex>::max();
    TextDirection strong_direction =
        (CTRunGetStatus(run) & kCTRunStatusRightToLeft) != 0 ? TextDirection::RightToLeft : TextDirection::LeftToRight;
    const CFIndex end = range.location + range.length;
    for (CFIndex location = range.location; location < end;) {
      const UniChar first = CFStringGetCharacterAtIndex(string.Get(), location);
      UTF32Char character = first;
      CFIndex length = 1;
      if (CFStringIsSurrogateHighCharacter(first) && location + 1 < end) {
        const UniChar second = CFStringGetCharacterAtIndex(string.Get(), location + 1);
        if (CFStringIsSurrogateLowCharacter(second)) {
          character = CFStringGetLongCharacterForSurrogatePair(first, second);
          length = 2;
        }
      }
      if (character == 0x200E) {
        strong_direction = TextDirection::LeftToRight;
        strong_location = location;
        break;
      } else if (character == 0x061C || character == 0x200F) {
        strong_direction = TextDirection::RightToLeft;
        strong_location = location;
        break;
      } else if (CFCharacterSetIsLongCharacterMember(letters, character)) {
        strong_location = location;
        break;
      }
      location += length;
    }
    if (strong_location < earliest_location) {
      earliest_location = strong_location;
      resolved = strong_direction;
    }
  }
  return resolved;
}

CTTextAlignment ToTextAlignment(TextAlign align, TextDirection direction) {
  switch (align) {
  case TextAlign::Leading:
    return kCTTextAlignmentNatural;
  case TextAlign::Center:
    return kCTTextAlignmentCenter;
  case TextAlign::Trailing:
    return direction == TextDirection::RightToLeft ? kCTTextAlignmentLeft : kCTTextAlignmentRight;
  }
  return kCTTextAlignmentNatural;
}

float TextAlignmentOffset(TextAlign align, TextDirection direction, float available_width, float content_width) {
  if (!std::isfinite(available_width)) {
    return 0.0F;
  }
  const float remaining = available_width - content_width;
  if (align == TextAlign::Center) {
    return remaining * 0.5F;
  }
  if ((align == TextAlign::Leading && direction == TextDirection::RightToLeft) ||
      (align == TextAlign::Trailing && direction != TextDirection::RightToLeft)) {
    return remaining;
  }
  return 0.0F;
}

CFAttributedStringRef CreateAttributedString(
    std::string_view text,
    const TextStyle& style,
    const TextLayoutOptions& options = {},
    CTFontRef supplied_font = nullptr
) {
  CFStringRef string = CreateString(text);
  CTFontRef font = supplied_font == nullptr ? CreateFont(style.font) : static_cast<CTFontRef>(CFRetain(supplied_font));
  const TextDirection direction = ResolveTextDirection(text, options.shaping.direction, font);
  const CTTextAlignment alignment = ToTextAlignment(options.align, direction);
  const CTLineBreakMode line_break =
      options.wrap == TextWrap::Word ? kCTLineBreakByWordWrapping : kCTLineBreakByClipping;
  const CTWritingDirection writing_direction =
      direction == TextDirection::RightToLeft ? kCTWritingDirectionRightToLeft : kCTWritingDirectionLeftToRight;
  CTParagraphStyleSetting paragraph_settings[] = {
      {kCTParagraphStyleSpecifierAlignment, sizeof(alignment), &alignment},
      {kCTParagraphStyleSpecifierLineBreakMode, sizeof(line_break), &line_break},
      {kCTParagraphStyleSpecifierBaseWritingDirection, sizeof(writing_direction), &writing_direction},
  };
  CTParagraphStyleRef paragraph = CTParagraphStyleCreate(paragraph_settings, 3);
  const int underline = HasTextDecoration(style.decoration, TextDecoration::Underline) ? kCTUnderlineStyleSingle : 0;
  const int strike_through =
      HasTextDecoration(style.decoration, TextDecoration::StrikeThrough) ? kCTUnderlineStyleSingle : 0;
  CFNumberRef underline_value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &underline);
  CFNumberRef strike_value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &strike_through);
  std::vector<const void*> keys{
      kCTFontAttributeName,
      kCTForegroundColorFromContextAttributeName,
      kCTParagraphStyleAttributeName,
      kCTUnderlineStyleAttributeName,
      (__bridge const void*)NSStrikethroughStyleAttributeName,
  };
  std::vector<const void*> values{
      font,
      kCFBooleanTrue,
      paragraph,
      underline_value,
      strike_value,
  };
  CFStringRef locale = nullptr;
  if (!options.shaping.locale.empty()) {
    locale = CreateString(options.shaping.locale);
    if (locale != nullptr) {
      keys.push_back(kCTLanguageAttributeName);
      values.push_back(locale);
    }
  }
  CFDictionaryRef attributes = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys.data(),
      values.data(),
      static_cast<CFIndex>(keys.size()),
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  );
  CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
  CFRelease(attributes);
  if (locale != nullptr) {
    CFRelease(locale);
  }
  CFRelease(strike_value);
  CFRelease(underline_value);
  CFRelease(paragraph);
  CFRelease(font);
  CFRelease(string);
  return attributed;
}


CFAttributedStringRef CreateAttributedString(const AttributedText& text, const TextStyle& style,
    const TextLayoutOptions& options, CTFontRef supplied_font = nullptr, bool preserve_final_line = false) {
  CFRef<CFAttributedStringRef> base{CreateAttributedString(text.PlainText(), style, options, supplied_font)};
  CFMutableAttributedStringRef attributed = CFAttributedStringCreateMutableCopy(kCFAllocatorDefault, 0, base.Get());
  // A layout-only sentinel preserves the empty line after a trailing newline; source text and offsets stay unchanged.
  if (preserve_final_line && !text.PlainText().empty() && text.PlainText().back() == '\n') {
    CFStringAppend(CFAttributedStringGetMutableString(attributed), CFSTR("\u200B"));
  }
  // CFString ranges use UTF-16 units; the attributed string retains the CTFont and CGColor objects.
  for (const auto& run : ResolveTextRuns(text, style)) {
    const CFRange range = CFRangeMake(static_cast<CFIndex>(run.range.start), static_cast<CFIndex>(run.range.Length()));
    CFRef<CTFontRef> font{CreateFont(run.style.font)};
    CFAttributedStringSetAttribute(attributed, range, kCTFontAttributeName, font.Get());
    const int underline = HasTextDecoration(run.style.decoration, TextDecoration::Underline)
                              ? kCTUnderlineStyleSingle : 0;
    const int strike = HasTextDecoration(run.style.decoration, TextDecoration::StrikeThrough)
                           ? kCTUnderlineStyleSingle : 0;
    CFRef<CFNumberRef> underline_value{CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &underline)};
    CFRef<CFNumberRef> strike_value{CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &strike)};
    CFAttributedStringSetAttribute(attributed, range, kCTUnderlineStyleAttributeName, underline_value.Get());
    CFAttributedStringSetAttribute(attributed, range, (__bridge CFStringRef)NSStrikethroughStyleAttributeName,
        strike_value.Get());
    if (run.background.alpha > 0.0F) {
      const Color color = run.background;
      CFRef<CGColorRef> background{CGColorCreateSRGB(color.red, color.green, color.blue, color.alpha)};
      CFAttributedStringSetAttribute(attributed, range, CFSTR("HuxerUIBackgroundColor"), background.Get());
    }
  }
  // Keep inherited foreground context-driven for cache reuse; explicit span colors override the CGContext color.
  for (const auto& item : text.StyleRanges()) {
    if (item.style.foreground) {
      const CFRange range =
          CFRangeMake(static_cast<CFIndex>(item.range.start), static_cast<CFIndex>(item.range.Length()));
      const Color color = *item.style.foreground;
      CFRef<CGColorRef> foreground{CGColorCreateSRGB(color.red, color.green, color.blue, color.alpha)};
      CFAttributedStringSetAttribute(attributed, range, kCTForegroundColorFromContextAttributeName, kCFBooleanFalse);
      CFAttributedStringSetAttribute(attributed, range, kCTForegroundColorAttributeName, foreground.Get());
    }
  }
  return attributed;
}

// Use the shaped glyph runs for both passes: backgrounds precede CTFrameDraw, while strikethrough follows it.
// Logical ranges can cross bidi runs, so decoration widths come from visual glyph positions rather than text offsets.
void DrawParagraphDecorations(CGContextRef context, CTFrameRef frame, Color foreground, bool backgrounds) {
  CFArrayRef lines = CTFrameGetLines(frame);
  const CFIndex count = CFArrayGetCount(lines);
  std::vector<CGPoint> origins(static_cast<std::size_t>(count));
  CTFrameGetLineOrigins(frame, CFRangeMake(0, count), origins.data());
  for (CFIndex index = 0; index < count; ++index) {
    CTLineRef line = static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, index));
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex run_index = 0; run_index < CFArrayGetCount(runs); ++run_index) {
      CTRunRef run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, run_index));
      CFDictionaryRef attributes = CTRunGetAttributes(run);
      CGColorRef background = static_cast<CGColorRef>(CFDictionaryGetValue(attributes, CFSTR("HuxerUIBackgroundColor")));
      CFNumberRef strike = static_cast<CFNumberRef>(
          CFDictionaryGetValue(attributes, (__bridge CFStringRef)NSStrikethroughStyleAttributeName));
      int strike_style = 0;
      if (strike != nullptr) {
        CFNumberGetValue(strike, kCFNumberIntType, &strike_style);
      }
      if ((backgrounds && background == nullptr) || (!backgrounds && strike_style == 0)) {
        continue;
      }
      const CFIndex glyph_count = CTRunGetGlyphCount(run);
      if (glyph_count == 0) {
        continue;
      }
      std::vector<CGPoint> positions(static_cast<std::size_t>(glyph_count));
      std::vector<CGSize> advances(static_cast<std::size_t>(glyph_count));
      CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
      CTRunGetAdvances(run, CFRangeMake(0, 0), advances.data());
      CGFloat left = CGFLOAT_MAX;
      CGFloat right = -CGFLOAT_MAX;
      for (CFIndex glyph = 0; glyph < glyph_count; ++glyph) {
        const auto position = static_cast<std::size_t>(glyph);
        left = std::min(left, positions[position].x);
        right = std::max(right, positions[position].x + advances[position].width);
      }
      const CGPoint origin = origins[static_cast<std::size_t>(index)];
      CTFontRef font = static_cast<CTFontRef>(CFDictionaryGetValue(attributes, kCTFontAttributeName));
      CGContextSaveGState(context);
      if (backgrounds) {
        CGFloat ascent = 0.0;
        CGFloat descent = 0.0;
        CGFloat leading = 0.0;
        CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
        CGContextSetFillColorWithColor(context, background);
        CGContextFillRect(context,
            CGRectMake(origin.x + left, origin.y - descent, right - left, ascent + descent + leading));
      } else {
        CGColorRef color = static_cast<CGColorRef>(CFDictionaryGetValue(attributes, kCTForegroundColorAttributeName));
        if (color != nullptr) {
          CGContextSetFillColorWithColor(context, color);
        } else {
          CGContextSetRGBFillColor(context, foreground.red, foreground.green, foreground.blue, foreground.alpha);
        }
        const CGFloat thickness = std::max<CGFloat>(1.0, CTFontGetUnderlineThickness(font));
        CGContextFillRect(context,
            CGRectMake(origin.x + left, origin.y + CTFontGetXHeight(font) * 0.5, right - left, thickness));
      }
      CGContextRestoreGState(context);
    }
  }
}

CTLineRef CreateLine(std::string_view text, const TextStyle& style, const TextShapingOptions& shaping = {},
    CTFontRef supplied_font = nullptr) {
  TextLayoutOptions options;
  options.shaping = shaping;
  options.wrap = TextWrap::NoWrap;
  CFAttributedStringRef attributed = CreateAttributedString(text, style, options, supplied_font);
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

void ApplyStrokeStyle(CGContextRef context, const StrokeStyle& style) {
  CGLineCap cap = kCGLineCapButt;
  if (style.cap == StrokeCap::Round) {
    cap = kCGLineCapRound;
  } else if (style.cap == StrokeCap::Square) {
    cap = kCGLineCapSquare;
  }
  CGLineJoin join = kCGLineJoinMiter;
  if (style.join == StrokeJoin::Round) {
    join = kCGLineJoinRound;
  } else if (style.join == StrokeJoin::Bevel) {
    join = kCGLineJoinBevel;
  }
  std::vector<CGFloat> dashes(style.dash_pattern.begin(), style.dash_pattern.end());
  CGContextSetLineWidth(context, style.width);
  CGContextSetLineCap(context, cap);
  CGContextSetLineJoin(context, join);
  CGContextSetMiterLimit(context, style.miter_limit);
  CGContextSetLineDash(context, style.dash_offset, dashes.empty() ? nullptr : dashes.data(), dashes.size());
}

void DrawCGImage(
    CGContextRef context,
    CGImageRef image,
    Size intrinsic_size,
    Rect source,
    Rect destination,
    ImageSampling sampling,
    float opacity
) {
  if (image == nullptr || intrinsic_size.width <= 0.0F || intrinsic_size.height <= 0.0F || source.IsEmpty() ||
      destination.IsEmpty() || opacity <= 0.0F) {
    return;
  }
  const CGFloat pixel_width = static_cast<CGFloat>(CGImageGetWidth(image));
  const CGFloat pixel_height = static_cast<CGFloat>(CGImageGetHeight(image));
  const CGFloat scale_x = pixel_width / intrinsic_size.width;
  const CGFloat scale_y = pixel_height / intrinsic_size.height;
  const CGRect source_rect = CGRectMake(
      source.x * scale_x,
      pixel_height - (source.y + source.height) * scale_y,
      source.width * scale_x,
      source.height * scale_y
  );
  CGContextSaveGState(context);
  CGContextSetAlpha(context, opacity);
  CGContextSetInterpolationQuality(
      context,
      sampling == ImageSampling::Nearest ? kCGInterpolationNone : kCGInterpolationHigh
  );
  CGContextClipToRect(context, CGRectMake(destination.x, destination.y, destination.width, destination.height));
  CGContextTranslateCTM(context, destination.x, destination.y + destination.height);
  CGContextScaleCTM(context, 1.0, -1.0);
  const CGFloat horizontal_scale = destination.width / source_rect.size.width;
  const CGFloat vertical_scale = destination.height / source_rect.size.height;
  CGContextDrawImage(
      context,
      CGRectMake(
          -source_rect.origin.x * horizontal_scale,
          -source_rect.origin.y * vertical_scale,
          pixel_width * horizontal_scale,
          pixel_height * vertical_scale
      ),
      image
  );
  CGContextRestoreGState(context);
}

CGMutablePathRef CreatePath(const Path& source) {
  CGMutablePathRef path = CGPathCreateMutable();
  for (const PathElement& element : InternalAccess::Elements(source)) {
    switch (element.verb) {
    case PathVerb::MoveTo:
      CGPathMoveToPoint(path, nullptr, element.points[0].x, element.points[0].y);
      break;
    case PathVerb::LineTo:
      CGPathAddLineToPoint(path, nullptr, element.points[0].x, element.points[0].y);
      break;
    case PathVerb::QuadraticTo:
      CGPathAddQuadCurveToPoint(
          path,
          nullptr,
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y
      );
      break;
    case PathVerb::CubicTo:
      CGPathAddCurveToPoint(
          path,
          nullptr,
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y,
          element.points[2].x,
          element.points[2].y
      );
      break;
    case PathVerb::Close:
      CGPathCloseSubpath(path);
      break;
    }
  }
  return path;
}

void ClipStrokePath(CGContextRef context, const Path& path, const StrokeStyle& style) {
  const CFRef<CGPathRef> native_path(CreatePath(path));
  ApplyStrokeStyle(context, style);
  CGContextAddPath(context, native_path.Get());
  CGContextReplacePathWithStrokedPath(context);
  CGContextClip(context);
}

void FillCurrentPath(CGContextRef context, PathFillRule fill_rule) {
  if (fill_rule == PathFillRule::EvenOdd) {
    CGContextEOFillPath(context);
  } else {
    CGContextFillPath(context);
  }
}

void ClipCurrentPath(CGContextRef context, PathFillRule fill_rule) {
  if (fill_rule == PathFillRule::EvenOdd) {
    CGContextEOClip(context);
  } else {
    CGContextClip(context);
  }
}

} // namespace

struct UIKitRenderer::State {
  struct CachedImage {
    std::uint64_t identity = 0;
    std::size_t byte_size = 0;
    CFRef<CGImageRef> cg_image;
  };

  struct CachedExternalTexture {
    std::weak_ptr<ExternalTexture> texture;
    CFRef<CGImageRef> cg_image;
    std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
  };

  struct MetalImageContext {
    __strong id<MTLDevice> device = nil;
    __strong CIContext* context = nil;
  };

  State() : external_texture_context([CIContext contextWithOptions:nil]) {}

  struct FontHash {
    std::size_t operator()(const Font& font) const noexcept {
      return HashFont(font);
    }
  };

  struct CachedFont {
    CFRef<CTFontRef> ct_font;
    FontMetrics metrics;
  };

  struct RunKey {
    std::string text;
    Font font;
    TextDecoration decoration = TextDecoration::None;
    TextShapingOptions shaping;

    bool operator==(const RunKey&) const = default;
  };

  struct RunKeyView {
    std::string_view text;
    const Font& font;
    TextDecoration decoration;
    const TextShapingOptions& shaping;
  };

  struct RunKeyHash {
    using is_transparent = void;

    std::size_t operator()(const RunKey& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.shaping);
    }

    std::size_t operator()(const RunKeyView& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.shaping);
    }

  private:
    static std::size_t Hash(
        std::string_view text, const Font& font, TextDecoration decoration, const TextShapingOptions& shaping
    ) noexcept {
      std::size_t result = std::hash<std::string_view>{}(text);
      CombineHash(result, HashFont(font));
      CombineHash(result, std::hash<TextDecoration>{}(decoration));
      CombineHash(result, HashShaping(shaping));
      return result;
    }
  };

  struct RunKeyEqual {
    using is_transparent = void;

    bool operator()(const RunKey& left, const RunKey& right) const noexcept {
      return left == right;
    }

    bool operator()(const RunKey& left, const RunKeyView& right) const noexcept {
      return std::string_view(left.text) == right.text && left.font == right.font &&
             left.decoration == right.decoration && left.shaping == right.shaping;
    }

    bool operator()(const RunKeyView& left, const RunKey& right) const noexcept {
      return (*this)(right, left);
    }
  };

  struct CachedRun {
    CFRef<CTLineRef> line;
    TextRunMetrics metrics;
    float text_height = 0.0F;
    TextDirection direction = TextDirection::LeftToRight;
  };

  struct ParagraphKey {
    AttributedText text;
    Font font;
    TextDecoration decoration = TextDecoration::None;
    TextLayoutOptions options;
    Size size;

    bool operator==(const ParagraphKey&) const = default;
  };

  struct ParagraphKeyView {
    const AttributedText& text;
    const Font& font;
    TextDecoration decoration;
    const TextLayoutOptions& options;
    Size size;
  };

  struct ParagraphKeyHash {
    using is_transparent = void;

    std::size_t operator()(const ParagraphKey& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.options, key.size);
    }

    std::size_t operator()(const ParagraphKeyView& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.options, key.size);
    }

  private:
    static std::size_t Hash(const AttributedText& text, const Font& font, TextDecoration decoration,
        const TextLayoutOptions& options, Size size) noexcept {
      std::size_t result = InternalAccess::BodyHash(text);
      CombineHash(result, HashFont(font));
      CombineHash(result, std::hash<TextDecoration>{}(decoration));
      CombineHash(result, HashShaping(options.shaping));
      CombineHash(result, std::hash<TextAlign>{}(options.align));
      CombineHash(result, std::hash<TextVerticalAlign>{}(options.vertical_align));
      CombineHash(result, std::hash<TextWrap>{}(options.wrap));
      CombineHash(result, std::hash<float>{}(size.width));
      CombineHash(result, std::hash<float>{}(size.height));
      return result;
    }
  };

  struct ParagraphKeyEqual {
    using is_transparent = void;

    bool operator()(const ParagraphKey& left, const ParagraphKey& right) const noexcept {
      return left == right;
    }

    bool operator()(const ParagraphKey& left, const ParagraphKeyView& right) const noexcept {
      return left.text == right.text && left.font == right.font &&
             left.decoration == right.decoration && left.options == right.options && left.size == right.size;
    }

    bool operator()(const ParagraphKeyView& left, const ParagraphKey& right) const noexcept {
      return (*this)(right, left);
    }
  };

  struct CachedParagraph {
    CFRef<CTFrameRef> frame;
    float frame_height = 0.0F;
    float content_height = 0.0F;
    std::size_t cost = 0;
  };

  CachedFont& FontFor(const Font& font) {
    const auto cached = fonts.find(font);
    if (cached != fonts.end()) {
      return cached->second;
    }

    CFRef<CTFontRef> ct_font{CreateFont(font)};
    const FontMetrics metrics{
        static_cast<float>(CTFontGetAscent(ct_font.Get())),
        static_cast<float>(CTFontGetDescent(ct_font.Get())),
        static_cast<float>(CTFontGetLeading(ct_font.Get())),
        static_cast<float>(-CTFontGetUnderlinePosition(ct_font.Get())),
        static_cast<float>(CTFontGetUnderlineThickness(ct_font.Get())),
        static_cast<float>(CTFontGetXHeight(ct_font.Get()) * 0.5),
        static_cast<float>(CTFontGetUnderlineThickness(ct_font.Get())),
    };
    constexpr std::size_t maximum_fonts = 64;
    if (fonts.size() >= maximum_fonts) {
      fonts.erase(fonts.begin());
    }
    auto [inserted, was_inserted] = fonts.emplace(font, CachedFont{std::move(ct_font), metrics});
    static_cast<void>(was_inserted);
    return inserted->second;
  }

  CachedRun& RunFor(std::string_view text, const TextStyle& style, const TextShapingOptions& shaping) {
    const auto cached = runs.find(RunKeyView{text, style.font, style.decoration, shaping});
    if (cached != runs.end()) {
      return cached->second;
    }

    CachedFont& cached_font = FontFor(style.font);
    TextShapingOptions resolved_shaping = shaping;
    resolved_shaping.direction = ResolveTextDirection(text, shaping.direction, cached_font.ct_font.Get());
    CFRef<CTLineRef> line{CreateLine(text, style, resolved_shaping, cached_font.ct_font.Get())};
    CGFloat ascent = 0.0;
    CGFloat descent = 0.0;
    CGFloat leading = 0.0;
    const float advance = static_cast<float>(CTLineGetTypographicBounds(line.Get(), &ascent, &descent, &leading));
    Rect visual_bounds{0.0F, -static_cast<float>(ascent), advance, static_cast<float>(ascent + descent)};
    const CGRect glyph_bounds = CTLineGetBoundsWithOptions(
        line.Get(),
        static_cast<CTLineBoundsOptions>(kCTLineBoundsUseGlyphPathBounds | kCTLineBoundsIncludeLanguageExtents)
    );
    if (glyph_bounds.size.width > 0.0 && glyph_bounds.size.height > 0.0) {
      const Rect glyph{
          static_cast<float>(glyph_bounds.origin.x),
          static_cast<float>(-CGRectGetMaxY(glyph_bounds)),
          static_cast<float>(glyph_bounds.size.width),
          static_cast<float>(glyph_bounds.size.height),
      };
      const float left = std::min(visual_bounds.x, glyph.x);
      const float top = std::min(visual_bounds.y, glyph.y);
      const float right = std::max(visual_bounds.x + visual_bounds.width, glyph.x + glyph.width);
      const float bottom = std::max(visual_bounds.y + visual_bounds.height, glyph.y + glyph.height);
      visual_bounds = {left, top, right - left, bottom - top};
    }
    const auto include_decoration = [&](float center, float thickness) {
      if (advance <= 0.0F || thickness <= 0.0F) {
        return;
      }
      const Rect decoration{0.0F, center - thickness * 0.5F, advance, thickness};
      const float left = std::min(visual_bounds.x, decoration.x);
      const float top = std::min(visual_bounds.y, decoration.y);
      const float right = std::max(visual_bounds.x + visual_bounds.width, decoration.x + decoration.width);
      const float bottom = std::max(visual_bounds.y + visual_bounds.height, decoration.y + decoration.height);
      visual_bounds = {left, top, right - left, bottom - top};
    };
    if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
      include_decoration(cached_font.metrics.underline_position, cached_font.metrics.underline_thickness);
    }
    if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
      include_decoration(-cached_font.metrics.strike_through_position, cached_font.metrics.strike_through_thickness);
    }

    constexpr std::size_t maximum_runs = 1024;
    if (runs.size() >= maximum_runs) {
      runs.erase(runs.begin());
    }
    auto [inserted, was_inserted] = runs.emplace(
        RunKey{std::string(text), style.font, style.decoration, shaping},
        CachedRun{
            std::move(line),
            {advance, visual_bounds, cached_font.metrics},
            static_cast<float>(ascent + descent + leading),
            resolved_shaping.direction,
        }
    );
    static_cast<void>(was_inserted);
    return inserted->second;
  }

  std::shared_ptr<CachedParagraph> ParagraphFor(const DrawTextCommand& command) {
    const Size size{command.rect.width, command.rect.height};
    const auto cached = paragraphs.find(
        ParagraphKeyView{command.text, command.style.font, command.style.decoration, command.options, size}
    );
    if (cached != paragraphs.end()) {
      return cached->second;
    }

    CachedFont& cached_font = FontFor(command.style.font);
    CFRef<CFAttributedStringRef> attributed{
        CreateAttributedString(command.text, command.style, command.options, cached_font.ct_font.Get(), true)};
    CFRef<CTFramesetterRef> framesetter{CTFramesetterCreateWithAttributedString(attributed.Get())};
    const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
        framesetter.Get(),
        CFRangeMake(0, 0),
        nullptr,
        CGSizeMake(size.width, CGFLOAT_MAX),
        nullptr
    );
    const float content_height =
        std::max(cached_font.metrics.LineHeight(), std::ceil(static_cast<float>(suggested.height)));
    const float frame_height =
        command.options.wrap == TextWrap::NoWrap ? std::max(size.height, content_height) : size.height;
    CFRef<CGPathRef> path{CGPathCreateWithRect(CGRectMake(0.0, 0.0, size.width, frame_height), nullptr)};
    CFRef<CTFrameRef> frame{CTFramesetterCreateFrame(framesetter.Get(), CFRangeMake(0, 0), path.Get(), nullptr)};

    const auto cost = ParagraphCacheCost(command.text);
    // Return an owning handle even when an oversized frame bypasses cache retention.
    auto paragraph = std::make_shared<CachedParagraph>(
        CachedParagraph{std::move(frame), frame_height, content_height, cost});
    if (cost > paragraph_cache_budget) {
      return paragraph;
    }
    constexpr std::size_t maximum_paragraphs = 256;
    while (!paragraphs.empty() &&
           (paragraphs.size() >= maximum_paragraphs || paragraph_bytes + cost > paragraph_cache_budget)) {
      paragraph_bytes -= paragraphs.begin()->second->cost;
      paragraphs.erase(paragraphs.begin());
    }
    paragraph_bytes += cost;
    paragraphs.emplace(ParagraphKey{command.text, command.style.font, command.style.decoration, command.options, size},
        paragraph);
    return paragraph;
  }

  CGImageRef ImageFor(const ImageAsset& image) {
    const std::uint64_t identity = InternalAccess::ImageIdentity(image);
    const auto cached =
        std::ranges::find_if(images, [identity](const CachedImage& entry) { return entry.identity == identity; });
    if (cached != images.end()) {
      std::rotate(cached, cached + 1, images.end());
      return images.back().cg_image.Get();
    }
    const std::span<const std::byte> bytes = image.EncodedBytes();
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<CFIndex>::max())) {
      return nullptr;
    }
    CFRef<CFDataRef> data{CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(bytes.data()),
        static_cast<CFIndex>(bytes.size())
    )};
    if (data.Get() == nullptr) {
      return nullptr;
    }
    CFRef<CGImageSourceRef> source{CGImageSourceCreateWithData(data.Get(), nullptr)};
    if (source.Get() == nullptr) {
      return nullptr;
    }
    CFRef<CGImageRef> cg_image{CGImageSourceCreateImageAtIndex(source.Get(), 0, nullptr)};
    if (cg_image.Get() == nullptr) {
      return nullptr;
    }
    const std::size_t row_bytes = CGImageGetBytesPerRow(cg_image.Get());
    const std::size_t height = CGImageGetHeight(cg_image.Get());
    const std::size_t byte_size = height > 0 && row_bytes > std::numeric_limits<std::size_t>::max() / height
                                      ? std::numeric_limits<std::size_t>::max()
                                      : row_bytes * height;
    constexpr std::size_t image_cache_budget = 64U * 1024U * 1024U;
    if (byte_size > image_cache_budget) {
      // Retain one oversized image so repeated frames do not decode it again; the next insertion evicts it.
      images.clear();
      images.push_back({identity, byte_size, std::move(cg_image)});
      image_cache_bytes = byte_size;
      return images.back().cg_image.Get();
    }
    while (!images.empty() && image_cache_bytes > image_cache_budget - byte_size) {
      image_cache_bytes -= images.front().byte_size;
      images.erase(images.begin());
    }
    images.push_back({identity, byte_size, std::move(cg_image)});
    image_cache_bytes += byte_size;
    return images.back().cg_image.Get();
  }

  CIContext* MetalContextFor(id<MTLDevice> device) {
    const auto found = std::ranges::find_if(metal_image_contexts, [device](const MetalImageContext& entry) {
      return entry.device == device;
    });
    if (found != metal_image_contexts.end()) {
      return found->context;
    }
    CIContext* context = [CIContext contextWithMTLDevice:device options:nil];
    if (context != nil) {
      metal_image_contexts.push_back({device, context});
    }
    return context;
  }

  CGImageRef ImageFor(const std::shared_ptr<ExternalTexture>& texture) {
    const std::shared_ptr<ios::PixelBufferTexture> pixel_buffer_texture =
        std::dynamic_pointer_cast<ios::PixelBufferTexture>(texture);
    const std::shared_ptr<ios::MetalTexture> metal_texture = std::dynamic_pointer_cast<ios::MetalTexture>(texture);
    if (!pixel_buffer_texture && !metal_texture) {
      throw std::logic_error("HuxerUI external texture is incompatible with the iOS renderer");
    }
    auto cached = std::ranges::find_if(external_textures, [&texture](const CachedExternalTexture& entry) {
      return entry.texture.lock() == texture;
    });
    if (cached == external_textures.end()) {
      external_textures.push_back(CachedExternalTexture{texture, CFRef<CGImageRef>{}});
      cached = external_textures.end() - 1;
    }

    const std::uint64_t revision = texture->Revision();
    if (cached->revision == revision) {
      return cached->cg_image.Get();
    }
    cached->revision = revision;

    __strong CIImage* image = nil;
    __strong CIContext* image_context = external_texture_context;
    if (pixel_buffer_texture) {
      CVPixelBufferRef frame = pixel_buffer_texture->AcquireFrame();
      if (frame == nullptr) {
        return cached->cg_image.Get();
      }
      CFRef<CVPixelBufferRef> retained_frame{frame};
      image = [CIImage imageWithCVPixelBuffer:retained_frame.Get()];
    } else {
      ios::MetalTexture::Origin origin = ios::MetalTexture::Origin::TopLeft;
      id<MTLTexture> frame = metal_texture->AcquireFrame(origin);
      if (frame == nil) {
        return cached->cg_image.Get();
      }
      image = [CIImage imageWithMTLTexture:frame options:nil];
      if (image == nil) {
        return cached->cg_image.Get();
      }
      if (origin == ios::MetalTexture::Origin::BottomLeft) {
        image = [image imageByApplyingTransform:CGAffineTransformMake(1.0, 0.0, 0.0, -1.0, 0.0,
                                                                      CGRectGetHeight(image.extent))];
      }
      image_context = MetalContextFor(frame.device);
    }
    if (image == nil || image_context == nil) {
      return cached->cg_image.Get();
    }
    const CGRect extent = image.extent;
    if (CGRectIsEmpty(extent) || CGRectIsInfinite(extent)) {
      return cached->cg_image.Get();
    }
    CFRef<CGImageRef> cg_image{[image_context createCGImage:image fromRect:extent]};
    if (cg_image.Get() != nullptr) {
      cached->cg_image = std::move(cg_image);
    }
    return cached->cg_image.Get();
  }

  void PruneExternalTextures() {
    std::erase_if(external_textures, [](const CachedExternalTexture& entry) {
      const std::shared_ptr<ExternalTexture> texture = entry.texture.lock();
      return !texture || !texture->IsActive();
    });
  }

  std::unordered_map<Font, CachedFont, FontHash> fonts;
  std::unordered_map<RunKey, CachedRun, RunKeyHash, RunKeyEqual> runs;
  std::unordered_map<ParagraphKey, std::shared_ptr<CachedParagraph>, ParagraphKeyHash, ParagraphKeyEqual> paragraphs;
  std::size_t paragraph_bytes = 0;
  std::vector<CachedImage> images;
  std::size_t image_cache_bytes = 0;
  __strong CIContext* external_texture_context = nil;
  std::vector<MetalImageContext> metal_image_contexts;
  std::vector<CachedExternalTexture> external_textures;
};

class UIKitTextLayout final : public TextLayout {
public:
  UIKitTextLayout(const AttributedText& paragraph, const TextStyle& style, float max_width,
      const TextLayoutOptions& options) {
    const auto& text = paragraph.PlainText();
    string_ = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    CTFontRef font = CreateFont(style.font);
    line_height_ = static_cast<float>(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
    const TextDirection direction = ResolveTextDirection(text, options.shaping.direction, font);
    CFRelease(font);

    const bool constrained = std::isfinite(max_width);
    if (constrained && max_width <= 0.0F) {
      return;
    }

    const bool has_hard_break = text.find('\n') != std::string_view::npos;
    if (!has_hard_break && (!constrained || options.wrap == TextWrap::NoWrap)) {
      CFRef<CFAttributedStringRef> attributed{CreateAttributedString(paragraph, style, options)};
      line_ = CTLineCreateWithAttributedString(attributed.Get());
      CGFloat ascent = 0.0;
      CGFloat descent = 0.0;
      CGFloat leading = 0.0;
      const double width = CTLineGetTypographicBounds(line_, &ascent, &descent, &leading);
      line_height_ = std::ceil(std::max(line_height_, static_cast<float>(ascent + descent + leading)));
      size_ = {
          std::ceil(static_cast<float>(width)),
          line_height_,
      };
      horizontal_offset_ = TextAlignmentOffset(options.align, direction, max_width, static_cast<float>(width));
      metrics_ = {
          size_,
          static_cast<float>(ascent),
          static_cast<float>(ascent),
          1,
      };
      return;
    }

    CFAttributedStringRef attributed = CreateAttributedString(paragraph, style, options, nullptr, true);
    CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attributed);
    const bool soft_wrap = constrained && options.wrap == TextWrap::Word;
    const CGFloat suggested_width = soft_wrap ? static_cast<CGFloat>(std::max(1.0F, max_width)) : CGFLOAT_MAX;
    const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
        framesetter,
        CFRangeMake(0, 0),
        nullptr,
        CGSizeMake(suggested_width, CGFLOAT_MAX),
        nullptr
    );
    const float width = soft_wrap ? suggested_width : std::max(1.0F, std::ceil(static_cast<float>(suggested.width)));
    const float height = std::max(line_height_, std::ceil(static_cast<float>(suggested.height)));
    // NoWrap measures at natural width; translate explicit lines to match drawing within the available width.
    horizontal_offset_ = soft_wrap ? 0.0F : TextAlignmentOffset(options.align, direction, max_width, width);
    CGPathRef path = CGPathCreateWithRect(CGRectMake(0.0, 0.0, width, height), nullptr);
    frame_ = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);
    const float measured_width = std::ceil(static_cast<float>(suggested.width));
    size_ = {soft_wrap ? std::min(max_width, measured_width) : measured_width, height};

    CFArrayRef lines = CTFrameGetLines(frame_);
    const CFIndex count = CFArrayGetCount(lines);
    std::vector<CGPoint> origins(static_cast<std::size_t>(count));
    if (count > 0) {
      CTFrameGetLineOrigins(frame_, CFRangeMake(0, count), origins.data());
      for (CGPoint& origin : origins) {
        origin.x += horizontal_offset_;
      }
    }
    // Line references are borrowed from frame_, which stays owned for the lifetime of this geometry layout.
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
      line_ = CTLineCreateWithAttributedString(attributed);
    }
    const float first_baseline = line_records_.empty() ? line_height_ : height - line_records_.front().origin.y;
    const float last_baseline = line_records_.empty() ? first_baseline : height - line_records_.back().origin.y;
    metrics_ = {
        size_,
        first_baseline,
        last_baseline,
        std::max<std::size_t>(1, line_records_.size()),
    };

    CGPathRelease(path);
    CFRelease(framesetter);
    CFRelease(attributed);
  }

  ~UIKitTextLayout() override {
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

  const TextLayoutMetrics& MetricsValue() const noexcept {
    return metrics_;
  }

  TextPosition HitTest(Point point) const override {
    CTLineRef line = line_;
    CGPoint origin{horizontal_offset_, 0.0};
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
    CGPoint origin{horizontal_offset_, 0.0};
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
        const CFIndex glyph_count = CTRunGetGlyphCount(run);
        if (glyph_count == 0) {
          continue;
        }
        std::vector<CGPoint> positions(static_cast<std::size_t>(glyph_count));
        std::vector<CGSize> advances(static_cast<std::size_t>(glyph_count));
        CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
        CTRunGetAdvances(run, CFRangeMake(0, 0), advances.data());
        CGFloat left = positions.front().x;
        CGFloat right = left;
        for (std::size_t glyph = 0; glyph < positions.size(); ++glyph) {
          left = std::min(left, positions[glyph].x);
          right = std::max(right, positions[glyph].x + advances[glyph].width);
        }
        const bool rtl = (CTRunGetStatus(run) & kCTRunStatusRightToLeft) != 0;
        // At bidi boundaries a logical offset has two visual edges; select the one inside this CoreText run.
        auto edge = [&](CFIndex offset) {
          if (offset == run_range.location) {
            return rtl ? right : left;
          }
          if (offset == run_range.location + run_range.length) {
            return rtl ? left : right;
          }
          CGFloat secondary = 0.0;
          const CGFloat primary = CTLineGetOffsetForStringIndex(line, offset, &secondary);
          return std::clamp(primary >= left && primary <= right ? primary : secondary, left, right);
        };
        const CGFloat first = edge(run_start);
        const CGFloat last = edge(run_end);
        rects.push_back({
            static_cast<float>(origin.x + std::min(first, last)),
            top,
            static_cast<float>(std::abs(last - first)),
            std::ceil(height),
        });
      }
    };
    if (line_records_.empty()) {
      append_line(
          line_,
          CFRangeMake(0, static_cast<CFIndex>(string_.length)),
          CGPointMake(horizontal_offset_, 0.0),
          0.0F,
          line_height_
      );
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
  TextLayoutMetrics metrics_;
  float line_height_ = 0.0F;
  float horizontal_offset_ = 0.0F;
};

UIKitRenderer::UIKitRenderer() : state_(std::make_unique<State>()) {}

UIKitRenderer::~UIKitRenderer() = default;

FontMetrics UIKitRenderer::Metrics(const Font& font) {
  return state_->FontFor(font).metrics;
}

TextRunMetrics
UIKitRenderer::MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
  }
  return state_->RunFor(text, style, options).metrics;
}

TextLayoutMetrics UIKitRenderer::MeasureText(std::string_view text, const TextStyle& style, float max_width,
    const TextLayoutOptions& options) {
  return MeasureText(AttributedText(std::string(text)), style, max_width, options);
}

std::unique_ptr<TextLayout> UIKitRenderer::CreateTextLayout(std::string_view text, const TextStyle& style,
    float max_width, const TextLayoutOptions& options) {
  return CreateTextLayout(AttributedText(std::string(text)), style, max_width, options);
}

TextLayoutMetrics UIKitRenderer::MeasureText(const AttributedText& text, const TextStyle& style, float max_width,
    const TextLayoutOptions& options) {
  if (std::isfinite(max_width) && max_width <= 0.0F) {
    return {};
  }
  UIKitTextLayout layout(text, style, max_width, options);
  TextLayoutMetrics metrics = layout.MetricsValue();
  if (std::isfinite(max_width)) {
    metrics.size.width = std::min(metrics.size.width, max_width);
  }
  return metrics;
}

std::unique_ptr<TextLayout> UIKitRenderer::CreateTextLayout(const AttributedText& text, const TextStyle& style,
    float max_width, const TextLayoutOptions& options) {
  return std::make_unique<UIKitTextLayout>(text, style, max_width, options);
}

struct UIKitRenderer::CommandRange {
  std::size_t first = 0;
  std::size_t end = 0;
  std::size_t cursor = 0;
};

void UIKitRenderer::RenderSequence(const PaintSequence& sequence, CGContextRef context, CommandRange* range) {
  for (const PaintCommand& command : sequence.Commands()) {
    if (std::holds_alternative<PlacePlatformViewCommand>(command)) {
      continue;
    }
    const bool selected = range == nullptr || (range->cursor >= range->first && range->cursor < range->end);
    if (range != nullptr) {
      ++range->cursor;
    }
    if (!selected) {
      continue;
    }
    std::visit([this, context](const auto& value) { RenderCommand(context, value); }, command);
  }
}

void UIKitRenderer::RenderSceneNode(const RenderNode& node, CGContextRef context, CommandRange* range) {
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

  RenderSequence(node.content, context, range);
  for (const RenderClip& clip : node.child_clips) {
    std::visit([&](const auto& command) { RenderCommand(context, command); }, clip);
  }
  const bool children_transformed = !node.children_transform.IsIdentity();
  if (children_transformed) {
    RenderCommand(context, PushTransformCommand{node.children_transform});
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      RenderSceneNode(*child, context, range);
    }
  }
  if (children_transformed) {
    RenderCommand(context, PopTransformCommand{});
  }
  for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
    RenderCommand(context, PopClipCommand{});
  }
  RenderSequence(node.foreground, context, range);
  if (translucent) {
    CGContextEndTransparencyLayer(context);
    CGContextRestoreGState(context);
  }
  if (transformed) {
    RenderCommand(context, PopTransformCommand{});
  }
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawRectCommand& command) {
  if (command.rect.IsEmpty()) {
    return;
  }
  std::visit(
      [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Value, Color>) {
          SetFillColor(context, value);
          const CGRect rect = CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height);
          if (command.corner_radius > 0.0F) {
            const CFRef<CGPathRef> path(
                CGPathCreateWithRoundedRect(rect, command.corner_radius, command.corner_radius, nullptr));
            CGContextAddPath(context, path.Get());
            CGContextFillPath(context);
          } else {
            CGContextFillRect(context, rect);
          }
        } else {
          CGContextSaveGState(context);
          ClipGradientRect(context, command.rect, command.corner_radius);
          DrawGradient(context, command.rect, value);
          CGContextRestoreGState(context);
        }
      },
      command.brush.Get()
  );
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawTextCommand& command) {
  if (command.rect.width <= 0.0F || command.rect.height <= 0.0F) {
    return;
  }
  const auto retained = state_->ParagraphFor(command);
  const State::CachedParagraph& paragraph = *retained;
  const float remaining_height = std::max(0.0F, command.rect.height - paragraph.content_height);
  float vertical_offset = 0.0F;
  if (command.options.vertical_align == TextVerticalAlign::Center) {
    vertical_offset = remaining_height * 0.5F;
  } else if (command.options.vertical_align == TextVerticalAlign::Bottom) {
    vertical_offset = remaining_height;
  }

  CGContextSaveGState(context);
  CGContextClipToRect(context, CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height));
  CGContextTranslateCTM(
      context,
      command.rect.x + command.paragraph_offset.x,
      command.rect.y + paragraph.frame_height + vertical_offset + command.paragraph_offset.y
  );
  CGContextScaleCTM(context, 1.0, -1.0);
  CGContextSetTextMatrix(context, CGAffineTransformIdentity);
  SetFillColor(context, command.style.foreground);
  DrawParagraphDecorations(context, paragraph.frame.Get(), command.style.foreground, true);
  CTFrameDraw(paragraph.frame.Get(), context);
  DrawParagraphDecorations(context, paragraph.frame.Get(), command.style.foreground, false);
  CGContextRestoreGState(context);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawTextRunsCommand& command) {
  for (const TextRun& run : command.runs) {
    if (run.text.empty() || run.style.foreground.alpha <= 0.0F) {
      continue;
    }
    const State::CachedRun& cached = state_->RunFor(run.text, run.style, run.shaping);
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, 0.0, run.baseline_origin.y);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    SetFillColor(context, run.style.foreground);
    CGContextSetTextPosition(context, run.baseline_origin.x, 0.0);
    CTLineDraw(cached.line.Get(), context);
    CGContextRestoreGState(context);
  }
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawImageCommand& command) {
  DrawCGImage(
      context,
      state_->ImageFor(command.image),
      command.image.IntrinsicSize(),
      command.source,
      command.destination,
      command.sampling,
      command.opacity
  );
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawExternalTextureCommand& command) {
  DrawCGImage(
      context,
      state_->ImageFor(command.texture),
      command.texture->IntrinsicSize(),
      command.source,
      command.destination,
      command.sampling,
      command.opacity
  );
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawCircleCommand& command) {
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

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawLineCommand& command) {
  if (command.start == command.end || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
    return;
  }
  CGContextSaveGState(context);
  SetStrokeColor(context, command.color);
  ApplyStrokeStyle(context, command.style);
  CGContextMoveToPoint(context, command.start.x, command.start.y);
  CGContextAddLineToPoint(context, command.end.x, command.end.y);
  CGContextStrokePath(context);
  CGContextRestoreGState(context);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawArcCommand& command) {
  if (command.radius <= 0.0F || command.style.width <= 0.0F || command.color.alpha <= 0.0F ||
      !std::isfinite(command.start_angle) || !std::isfinite(command.sweep_angle) || command.sweep_angle == 0.0F) {
    return;
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
  if (std::abs(command.sweep_angle) >= 2.0F * std::numbers::pi_v<float> - 0.0001F) {
    CGPathCloseSubpath(path);
  }
  CGContextSaveGState(context);
  SetStrokeColor(context, command.color);
  ApplyStrokeStyle(context, command.style);
  CGContextAddPath(context, path);
  CGContextStrokePath(context);
  CGContextRestoreGState(context);
  CGPathRelease(path);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawBorderCommand& command) {
  if (command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
    return;
  }
  if (!command.style.dash_pattern.empty() || command.style.join != StrokeJoin::Miter ||
      command.style.miter_limit != 4.0F) {
    RenderCommand(context,
                  StrokePathCommand{
                      CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width),
                      command.color,
                      command.rect,
                      command.style,
                  });
    return;
  }
  const float inset = command.style.width * 0.5F;
  const CGRect rect = CGRectMake(
      command.rect.x + inset,
      command.rect.y + inset,
      std::max(0.0F, command.rect.width - command.style.width),
      std::max(0.0F, command.rect.height - command.style.width)
  );
  const float radius = std::max(0.0F, command.corner_radius - inset);
  CGPathRef path = CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
  CGContextSaveGState(context);
  SetStrokeColor(context, command.color);
  ApplyStrokeStyle(context, command.style);
  CGContextAddPath(context, path);
  CGContextStrokePath(context);
  CGContextRestoreGState(context);
  CGPathRelease(path);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawShadowCommand& command) {
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
      CGColorCreateSRGB(command.color.red, command.color.green, command.color.blue, command.color.alpha);

  CGContextSaveGState(context);
  CGContextAddPath(context, outer_clip);
  CGContextEOClip(context);
  CGContextSetShadowWithColor(context, CGSizeZero, static_cast<CGFloat>(resolved.standard_deviation), shadow_color);
  CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
  CGContextAddPath(context, caster_path);
  CGContextFillPath(context);
  CGContextRestoreGState(context);

  CGColorRelease(shadow_color);
  CGPathRelease(outer_clip);
  CGPathRelease(caster_path);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const FillPathCommand& command) {
  if (command.path.IsEmpty()) {
    return;
  }
  const CFRef<CGPathRef> path(CreatePath(command.path));
  std::visit(
      [&](const auto& value) {
        CGContextSaveGState(context);
        CGContextAddPath(context, path.Get());
        if constexpr (std::same_as<std::decay_t<decltype(value)>, Color>) {
          SetFillColor(context, value);
          FillCurrentPath(context, command.fill_rule);
        } else {
          ClipCurrentPath(context, command.fill_rule);
          DrawGradient(context, command.brush_bounds, value);
        }
        CGContextRestoreGState(context);
      },
      command.brush.Get()
  );
}

void UIKitRenderer::RenderCommand(CGContextRef context, const StrokePathCommand& command) {
  if (command.path.IsEmpty() || command.style.width <= 0.0F) {
    return;
  }
  std::visit(
      [&](const auto& value) {
        CGContextSaveGState(context);
        if constexpr (std::same_as<std::decay_t<decltype(value)>, Color>) {
          const CFRef<CGPathRef> path(CreatePath(command.path));
          SetStrokeColor(context, value);
          ApplyStrokeStyle(context, command.style);
          CGContextAddPath(context, path.Get());
          CGContextStrokePath(context);
        } else {
          ClipStrokePath(context, command.path, command.style);
          DrawGradient(context, command.brush_bounds, value);
        }
        CGContextRestoreGState(context);
      },
      command.brush.Get()
  );
}

void UIKitRenderer::RenderCommand(CGContextRef context, const DrawPathShadowCommand& command) {
  if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
    return;
  }
  CGPathRef path = CreatePath(command.path);
  const CGAffineTransform translation = CGAffineTransformMakeTranslation(command.offset.x, command.offset.y);
  CGPathRef caster_path = CGPathCreateCopyByTransformingPath(path, &translation);
  CGPathRelease(path);

  CGContextSaveGState(context);
  if (command.blur_radius <= 0.0F) {
    SetFillColor(context, command.color);
    CGContextAddPath(context, caster_path);
    FillCurrentPath(context, command.fill_rule);
    CGContextRestoreGState(context);
    CGPathRelease(caster_path);
    return;
  }

  CGColorRef shadow_color =
      CGColorCreateSRGB(command.color.red, command.color.green, command.color.blue, command.color.alpha);
  CGContextBeginTransparencyLayer(context, nullptr);
  CGContextSetShadowWithColor(context, CGSizeZero, static_cast<CGFloat>(command.blur_radius / 3.0F), shadow_color);
  CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
  CGContextAddPath(context, caster_path);
  FillCurrentPath(context, command.fill_rule);
  CGContextSetShadowWithColor(context, CGSizeZero, 0.0, nullptr);
  CGContextSetBlendMode(context, kCGBlendModeClear);
  CGContextAddPath(context, caster_path);
  FillCurrentPath(context, command.fill_rule);
  CGContextEndTransparencyLayer(context);
  CGContextRestoreGState(context);

  CGColorRelease(shadow_color);
  CGPathRelease(caster_path);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const PushClipCommand& command) {
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

void UIKitRenderer::RenderCommand(CGContextRef context, const PushPathClipCommand& command) {
  CGContextSaveGState(context);
  CGPathRef path = CreatePath(command.path);
  CGContextAddPath(context, path);
  if (command.fill_rule == PathFillRule::EvenOdd) {
    CGContextEOClip(context);
  } else {
    CGContextClip(context);
  }
  CGPathRelease(path);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const PopClipCommand& command) {
  static_cast<void>(command);
  CGContextRestoreGState(context);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const PushTransformCommand& command) {
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

void UIKitRenderer::RenderCommand(CGContextRef context, const PopTransformCommand& command) {
  static_cast<void>(command);
  CGContextRestoreGState(context);
}

void UIKitRenderer::RenderCommand(CGContextRef context, const PlacePlatformViewCommand& command) {
  static_cast<void>(context);
  static_cast<void>(command);
}

void UIKitRenderer::DrawSlice(
    CGContextRef context,
    CGRect dirty_rect,
    const RenderFrame* frame,
    std::size_t first_command,
    std::size_t command_count,
    bool draw_background
) {
  state_->PruneExternalTextures();
  CGContextSaveGState(context);
  CGContextClipToRect(context, dirty_rect);
  if (draw_background) {
    SetFillColor(context, Color::Rgb(247, 248, 250));
    CGContextFillRect(context, dirty_rect);
  }
  if (frame != nullptr && frame->scene.root != nullptr && command_count > 0) {
    CommandRange range{first_command, first_command + command_count, 0};
    RenderSceneNode(*frame->scene.root, context, &range);
  }
  CGContextRestoreGState(context);
}

} // namespace huxerui::detail
