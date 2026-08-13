#include "appkit_renderer.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "path_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"
#include "text_layout_internal.h"

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
      CFAttributedStringCreate(kCFAllocatorDefault, string.Get(), attributes.Get())
  };
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

CTLineRef CreateLine(
    std::string_view text,
    const TextStyle& style,
    const TextShapingOptions& shaping = {},
    CTFontRef supplied_font = nullptr
) {
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

CGMutablePathRef CreatePath(const Path& source) {
  CGMutablePathRef path = CGPathCreateMutable();
  for (const PathElement& element : PathAccess::Elements(source)) {
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

void FillCurrentPath(CGContextRef context, PathFillRule fill_rule) {
  if (fill_rule == PathFillRule::EvenOdd) {
    CGContextEOFillPath(context);
  } else {
    CGContextFillPath(context);
  }
}

} // namespace

struct AppKitRenderer::State {
  struct CachedImage {
    std::uint64_t identity = 0;
    std::size_t byte_size = 0;
    CFRef<CGImageRef> native_image;
  };

  struct FontHash {
    std::size_t operator()(const Font& font) const noexcept {
      return HashFont(font);
    }
  };

  struct CachedFont {
    CFRef<CTFontRef> native_font;
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
    std::string text;
    Font font;
    TextDecoration decoration = TextDecoration::None;
    TextLayoutOptions options;
    Size size;

    bool operator==(const ParagraphKey&) const = default;
  };

  struct ParagraphKeyView {
    std::string_view text;
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
    static std::size_t Hash(
        std::string_view text, const Font& font, TextDecoration decoration, const TextLayoutOptions& options, Size size
    ) noexcept {
      std::size_t result = std::hash<std::string_view>{}(text);
      CombineHash(result, HashFont(font));
      CombineHash(result, std::hash<TextDecoration>{}(decoration));
      CombineHash(result, HashShaping(options.shaping));
      CombineHash(result, std::hash<TextAlign>{}(options.align));
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
      return std::string_view(left.text) == right.text && left.font == right.font &&
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
  };

  CachedFont& FontFor(const Font& font) {
    const auto cached = fonts.find(font);
    if (cached != fonts.end()) {
      return cached->second;
    }

    CFRef<CTFontRef> native_font{CreateFont(font)};
    const FontMetrics metrics{
        static_cast<float>(CTFontGetAscent(native_font.Get())),
        static_cast<float>(CTFontGetDescent(native_font.Get())),
        static_cast<float>(CTFontGetLeading(native_font.Get())),
        static_cast<float>(-CTFontGetUnderlinePosition(native_font.Get())),
        static_cast<float>(CTFontGetUnderlineThickness(native_font.Get())),
        static_cast<float>(CTFontGetXHeight(native_font.Get()) * 0.5),
        static_cast<float>(CTFontGetUnderlineThickness(native_font.Get())),
    };
    constexpr std::size_t maximum_fonts = 64;
    if (fonts.size() >= maximum_fonts) {
      fonts.erase(fonts.begin());
    }
    auto [inserted, was_inserted] = fonts.emplace(font, CachedFont{std::move(native_font), metrics});
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
    resolved_shaping.direction = ResolveTextDirection(text, shaping.direction, cached_font.native_font.Get());
    CFRef<CTLineRef> line{CreateLine(text, style, resolved_shaping, cached_font.native_font.Get())};
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

  CachedParagraph& ParagraphFor(const DrawTextCommand& command) {
    const Size size{command.rect.width, command.rect.height};
    const auto cached = paragraphs.find(
        ParagraphKeyView{command.text, command.style.font, command.style.decoration, command.options, size}
    );
    if (cached != paragraphs.end()) {
      return cached->second;
    }

    CachedFont& cached_font = FontFor(command.style.font);
    std::string layout_text{command.text};
    if (!layout_text.empty() && layout_text.back() == '\n') {
      layout_text.append("\xE2\x80\x8B");
    }
    CFRef<CFAttributedStringRef> attributed{
        CreateAttributedString(layout_text, command.style, command.options, cached_font.native_font.Get())
    };
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

    constexpr std::size_t maximum_paragraphs = 256;
    if (paragraphs.size() >= maximum_paragraphs) {
      paragraphs.erase(paragraphs.begin());
    }
    auto [inserted, was_inserted] = paragraphs.emplace(
        ParagraphKey{command.text, command.style.font, command.style.decoration, command.options, size},
        CachedParagraph{std::move(frame), frame_height, content_height}
    );
    static_cast<void>(was_inserted);
    return inserted->second;
  }

  CGImageRef ImageFor(const ImageAsset& image) {
    const std::uint64_t identity = ResourceAccess::ImageIdentity(image);
    const auto cached =
        std::ranges::find_if(images, [identity](const CachedImage& entry) { return entry.identity == identity; });
    if (cached != images.end()) {
      std::rotate(cached, cached + 1, images.end());
      return images.back().native_image.Get();
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
    CFRef<CGImageRef> native_image{CGImageSourceCreateImageAtIndex(source.Get(), 0, nullptr)};
    if (native_image.Get() == nullptr) {
      return nullptr;
    }
    const std::size_t row_bytes = CGImageGetBytesPerRow(native_image.Get());
    const std::size_t height = CGImageGetHeight(native_image.Get());
    const std::size_t byte_size = height > 0 && row_bytes > std::numeric_limits<std::size_t>::max() / height
                                      ? std::numeric_limits<std::size_t>::max()
                                      : row_bytes * height;
    constexpr std::size_t image_cache_budget = 64U * 1024U * 1024U;
    if (byte_size > image_cache_budget) {
      // Retain one oversized image so repeated frames do not decode it again; the next insertion evicts it.
      images.clear();
      images.push_back({identity, byte_size, std::move(native_image)});
      image_cache_bytes = byte_size;
      return images.back().native_image.Get();
    }
    while (!images.empty() && image_cache_bytes > image_cache_budget - byte_size) {
      image_cache_bytes -= images.front().byte_size;
      images.erase(images.begin());
    }
    images.push_back({identity, byte_size, std::move(native_image)});
    image_cache_bytes += byte_size;
    return images.back().native_image.Get();
  }

  std::unordered_map<Font, CachedFont, FontHash> fonts;
  std::unordered_map<RunKey, CachedRun, RunKeyHash, RunKeyEqual> runs;
  std::unordered_map<ParagraphKey, CachedParagraph, ParagraphKeyHash, ParagraphKeyEqual> paragraphs;
  std::vector<CachedImage> images;
  std::size_t image_cache_bytes = 0;
};

class MacTextLayout final : public TextLayout {
public:
  MacTextLayout(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options) {
    string_ = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    CTFontRef font = CreateFont(style.font);
    line_height_ = static_cast<float>(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
    CFRelease(font);

    const bool constrained = std::isfinite(max_width);
    if (constrained && max_width <= 0.0F) {
      return;
    }

    const bool has_hard_break = text.find('\n') != std::string_view::npos;
    if (!has_hard_break && (!constrained || options.wrap == TextWrap::NoWrap)) {
      line_ = CreateLine(text, style, options.shaping);
      CGFloat ascent = 0.0;
      CGFloat descent = 0.0;
      CGFloat leading = 0.0;
      const double width = CTLineGetTypographicBounds(line_, &ascent, &descent, &leading);
      size_ = {
          std::ceil(static_cast<float>(width)),
          std::ceil(std::max(line_height_, static_cast<float>(ascent + descent + leading))),
      };
      metrics_ = {
          size_,
          static_cast<float>(ascent),
          static_cast<float>(ascent),
          1,
      };
      return;
    }

    std::string layout_text{text};
    if (!layout_text.empty() && layout_text.back() == '\n') {
      layout_text.append("\xE2\x80\x8B");
    }
    CFAttributedStringRef attributed = CreateAttributedString(layout_text, style, options);
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
    CGPathRef path = CGPathCreateWithRect(CGRectMake(0.0, 0.0, width, height), nullptr);
    frame_ = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);
    const float measured_width = std::ceil(static_cast<float>(suggested.width));
    size_ = {soft_wrap ? std::min(max_width, measured_width) : measured_width, height};

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
      line_ = CreateLine(layout_text, style, options.shaping);
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

  const TextLayoutMetrics& MetricsValue() const noexcept {
    return metrics_;
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
  TextLayoutMetrics metrics_;
  float line_height_ = 0.0F;
};

AppKitRenderer::AppKitRenderer() : state_(std::make_unique<State>()) {}

AppKitRenderer::~AppKitRenderer() = default;

FontMetrics AppKitRenderer::Metrics(const Font& font) {
  return state_->FontFor(font).metrics;
}

TextRunMetrics
AppKitRenderer::MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
  }
  return state_->RunFor(text, style, options).metrics;
}

TextLayoutMetrics AppKitRenderer::MeasureText(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  if (std::isfinite(max_width) && max_width <= 0.0F) {
    return {};
  }
  MacTextLayout layout(text, style, max_width, options);
  TextLayoutMetrics metrics = layout.MetricsValue();
  if (std::isfinite(max_width)) {
    metrics.size.width = std::min(metrics.size.width, max_width);
  }
  return metrics;
}

std::unique_ptr<TextLayout> AppKitRenderer::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return std::make_unique<MacTextLayout>(text, style, max_width, options);
}

struct AppKitRenderer::CommandRange {
  std::size_t first = 0;
  std::size_t end = 0;
  std::size_t cursor = 0;
};

void AppKitRenderer::RenderSequence(const PaintSequence& sequence, CGContextRef context, CommandRange* range) {
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

void AppKitRenderer::RenderSceneNode(const RenderNode& node, CGContextRef context, CommandRange* range) {
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
  if (command.rect.width <= 0.0F || command.rect.height <= 0.0F || command.style.foreground.alpha <= 0.0F) {
    return;
  }
  State::CachedParagraph& paragraph = state_->ParagraphFor(command);
  const float vertical_offset =
      command.options.wrap == TextWrap::NoWrap ? (command.rect.height - paragraph.content_height) * 0.5F : 0.0F;

  CGContextSaveGState(context);
  CGContextClipToRect(context, CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height));
  CGContextTranslateCTM(context, command.rect.x, command.rect.y + paragraph.frame_height + vertical_offset);
  CGContextScaleCTM(context, 1.0, -1.0);
  CGContextSetTextMatrix(context, CGAffineTransformIdentity);
  SetFillColor(context, command.style.foreground);
  CTFrameDraw(paragraph.frame.Get(), context);
  CGContextRestoreGState(context);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawTextRunsCommand& command) {
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

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawImageCommand& command) {
  if (command.destination.IsEmpty() || command.source.IsEmpty() || command.opacity <= 0.0F) {
    return;
  }
  CGImageRef image = state_->ImageFor(command.image);
  if (image == nullptr) {
    return;
  }
  const float scale = command.image.Scale();
  const CGRect source_rect = CGRectMake(
      command.source.x * scale,
      static_cast<float>(CGImageGetHeight(image)) - (command.source.y + command.source.height) * scale,
      command.source.width * scale,
      command.source.height * scale
  );
  CGContextSaveGState(context);
  CGContextSetAlpha(context, command.opacity);
  CGContextSetInterpolationQuality(
      context,
      command.sampling == ImageSampling::Nearest ? kCGInterpolationNone : kCGInterpolationHigh
  );
  CGContextClipToRect(
      context,
      CGRectMake(command.destination.x, command.destination.y, command.destination.width, command.destination.height)
  );
  CGContextTranslateCTM(context, command.destination.x, command.destination.y + command.destination.height);
  CGContextScaleCTM(context, 1.0, -1.0);
  const CGFloat horizontal_scale = command.destination.width / source_rect.size.width;
  const CGFloat vertical_scale = command.destination.height / source_rect.size.height;
  CGContextDrawImage(
      context,
      CGRectMake(
          -source_rect.origin.x * horizontal_scale,
          -source_rect.origin.y * vertical_scale,
          static_cast<CGFloat>(CGImageGetWidth(image)) * horizontal_scale,
          static_cast<CGFloat>(CGImageGetHeight(image)) * vertical_scale
      ),
      image
  );
  CGContextRestoreGState(context);
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

  CGColorRelease(shadow_color);
  CGPathRelease(outer_clip);
  CGPathRelease(caster_path);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const FillPathCommand& command) {
  if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
    return;
  }
  CGPathRef path = CreatePath(command.path);
  CGContextSaveGState(context);
  SetFillColor(context, command.color);
  CGContextAddPath(context, path);
  FillCurrentPath(context, command.fill_rule);
  CGContextRestoreGState(context);
  CGPathRelease(path);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const StrokePathCommand& command) {
  if (command.path.IsEmpty() || command.width <= 0.0F || command.color.alpha <= 0.0F) {
    return;
  }
  CGLineCap cap = kCGLineCapButt;
  if (command.cap == StrokeCap::Round) {
    cap = kCGLineCapRound;
  } else if (command.cap == StrokeCap::Square) {
    cap = kCGLineCapSquare;
  }
  CGLineJoin join = kCGLineJoinMiter;
  if (command.join == StrokeJoin::Round) {
    join = kCGLineJoinRound;
  } else if (command.join == StrokeJoin::Bevel) {
    join = kCGLineJoinBevel;
  }

  CGPathRef path = CreatePath(command.path);
  CGContextSaveGState(context);
  SetStrokeColor(context, command.color);
  CGContextSetLineWidth(context, command.width);
  CGContextSetLineCap(context, cap);
  CGContextSetLineJoin(context, join);
  CGContextSetMiterLimit(context, command.miter_limit);
  CGContextAddPath(context, path);
  CGContextStrokePath(context);
  CGContextRestoreGState(context);
  CGPathRelease(path);
}

void AppKitRenderer::RenderCommand(CGContextRef context, const DrawPathShadowCommand& command) {
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
      CGColorCreateGenericRGB(command.color.red, command.color.green, command.color.blue, command.color.alpha);
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

void AppKitRenderer::RenderCommand(CGContextRef context, const PushPathClipCommand& command) {
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

void AppKitRenderer::RenderCommand(CGContextRef context, const PlacePlatformViewCommand& command) {
  static_cast<void>(context);
  static_cast<void>(command);
}

void AppKitRenderer::Draw(CGContextRef context, CGRect dirty_rect, const RenderFrame* frame) {
  CGContextSaveGState(context);
  CGContextClipToRect(context, dirty_rect);
  SetFillColor(context, Color::Rgb(247, 248, 250));
  CGContextFillRect(context, dirty_rect);
  if (frame != nullptr && frame->scene.root != nullptr) {
    RenderSceneNode(*frame->scene.root, context, nullptr);
  }
  CGContextRestoreGState(context);
}

void AppKitRenderer::DrawSlice(
    CGContextRef context,
    CGRect dirty_rect,
    const RenderFrame* frame,
    std::size_t first_command,
    std::size_t command_count,
    bool draw_background
) {
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
