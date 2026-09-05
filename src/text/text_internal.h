#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/text.h>

namespace huxerui {
class PlatformAdapter;
}

namespace huxerui::detail {

struct MountedNode;

struct Utf8CodePoint {
  std::uint32_t value = 0;
  std::size_t byte_length = 0;
};

inline bool DecodeCodePoint(std::string_view text, std::size_t index, Utf8CodePoint& result) noexcept {
  if (index >= text.size()) {
    return false;
  }
  const auto first = static_cast<std::uint8_t>(text[index]);
  if (first <= 0x7FU) {
    result = {first, 1};
    return true;
  }
  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
    value = first & 0x1FU;
    minimum = 0x80U;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
    value = first & 0x0FU;
    minimum = 0x800U;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
    value = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return false;
  }
  if (length > text.size() - index) {
    return false;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto continuation = static_cast<std::uint8_t>(text[index + offset]);
    if ((continuation & 0xC0U) != 0x80U) {
      return false;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
    return false;
  }
  result = {value, length};
  return true;
}

// Keep UTF-16 ranges for platform text APIs and byte ranges for UTF-8 consumers without rescanning each run.
struct ResolvedTextRun {
  TextRange range;
  std::size_t byte_start = 0;
  std::size_t byte_end = 0;
  TextStyle style;
  Color background = Color::Transparent();
};

TextStyle ResolveTextStyle(const TextStyle& base, const TextSpanStyle& overrides);
std::vector<ResolvedTextRun> ResolveTextRuns(const AttributedText& text, const TextStyle& base);
bool TextLayoutInputsEqual(const AttributedText& left, const Font& left_base, const AttributedText& right,
    const Font& right_base);
bool TextPaintInputsEqual(const AttributedText& left, const TextStyle& left_base, const AttributedText& right,
    const TextStyle& right_base);

inline constexpr std::size_t paragraph_cache_budget = 8 * 1024 * 1024;
// Charges platform glyph/line bookkeeping conservatively; oversized paragraphs are never retained by a cache.
std::size_t ParagraphCacheCost(const AttributedText& text) noexcept;

// Geometry is paragraph-local in logical pixels; callers apply vertical placement and presentation transforms.
// Offsets remain UTF-16 positions, and range geometry may contain disjoint visual fragments for one logical range.
class TextLayout {
public:
  virtual ~TextLayout() = default;

  [[nodiscard]] virtual Size Measure() const = 0;
  [[nodiscard]] virtual TextPosition HitTest(Point point) const = 0;
  [[nodiscard]] virtual Rect CaretRect(TextOffset offset, TextAffinity affinity) const = 0;
  [[nodiscard]] virtual std::vector<Rect> RangeRects(TextRange range) const = 0;
  [[nodiscard]] virtual TextOffset PreviousCaretOffset(TextOffset offset) const = 0;
  [[nodiscard]] virtual TextOffset NextCaretOffset(TextOffset offset) const = 0;
};

// These inputs describe shaping geometry, not the foreground colors or link targets of the latest declaration.
struct ParagraphLayout {
  AttributedText text;
  Font font;
  TextLayoutOptions options;
  float width = 0.0F;
  std::shared_ptr<TextLayout> layout;
};

// A mounted paragraph shares its shaping geometry between selection and links, never through a global text cache.
std::shared_ptr<TextLayout> GetParagraphLayout(MountedNode& node, PlatformAdapter& platform);

} // namespace huxerui::detail
