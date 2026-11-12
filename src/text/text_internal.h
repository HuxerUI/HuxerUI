#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/text.h>

namespace huxerui {
struct DrawTextCommand;
class PaintContext;
class PaintSequence;
class UiWindow;
enum class TextOverflow;
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
  std::optional<TextInlineObject> inline_object;
};

TextStyle ResolveTextStyle(const TextStyle& base, const TextSpanStyle& overrides);
std::vector<ResolvedTextRun> ResolveTextRuns(const AttributedText& text, const TextStyle& base);
std::vector<TextOffset> GraphemeBoundaries(std::string_view text);
bool TextLayoutInputsEqual(const AttributedText& left, const Font& left_base, const AttributedText& right,
    const Font& right_base);
bool TextPaintInputsEqual(const AttributedText& left, const TextStyle& left_base, const AttributedText& right,
    const TextStyle& right_base);

inline constexpr std::size_t paragraph_cache_budget = 8 * 1024 * 1024;
// Charges platform glyph/line bookkeeping conservatively; oversized paragraphs are never retained by a cache.
std::size_t ParagraphCacheCost(const AttributedText& text) noexcept;

struct InlineObjectPlacement {
  float leading = 0.0F;
  float trailing = 0.0F;
  float baseline = 0.0F;
};

// Geometry is paragraph-local in logical pixels; callers apply vertical placement and presentation transforms.
// Offsets remain UTF-16 positions, and range geometry may contain disjoint visual fragments for one logical range.
class TextLayoutBackend {
public:
  virtual ~TextLayoutBackend() = default;

  virtual Size Measure() const = 0;
  virtual std::vector<TextLineMetrics> LineMetrics() const { return {}; }
  virtual TextPosition HitTest(Point point) const = 0;
  virtual Rect CaretRect(TextOffset offset, TextAffinity affinity) const = 0;
  virtual std::vector<Rect> RangeRects(TextRange range) const = 0;
  virtual TextOffset PreviousCaretOffset(TextOffset offset) const = 0;
  virtual TextOffset NextCaretOffset(TextOffset offset) const = 0;
  virtual std::vector<InlineObjectPlacement> InlineObjectPlacements(
      std::span<const TextInlineObject> objects, std::span<const TextLineMetrics> lines) const;
};

struct CachedParagraphLayout {
  struct ElidedText {
    TextOffset prefix_end = 0;
    TextRange suffix{};
    bool ellipsis = false;
    AttributedText text{};
    TextDirection direction = TextDirection::Auto;

    AttributedText Apply(const AttributedText& source) const;
    TextLayoutOptions LayoutOptions(TextLayoutOptions options) const;
    TextOffset DisplaySuffixStart() const noexcept { return prefix_end + (ellipsis ? 1 : 0); }
  };

#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
  std::uint64_t revision = 0;
#endif
  AttributedText source;
  TextStyle style;
  TextLayoutOptions options;
  std::size_t line_limit = 0;
  TextOverflow overflow{};
  float min_width = 0.0F;
  float max_width = 0.0F;
  Size size;
  std::unique_ptr<ElidedText> elision;
  std::shared_ptr<TextLayoutBackend> layout;

  bool HasOverflowPolicy() const noexcept;
  bool RetainsSourceRange(TextRange range) const noexcept;
  Point Origin(Rect content, TextVerticalAlign align) const noexcept;
  TextPosition HitTest(Point point) const;
  Rect CaretRect(TextOffset offset, TextAffinity affinity) const;
  std::vector<Rect> RangeRects(TextRange range) const;
  void Draw(UiWindow& window, PaintContext& paint, Rect content, const AttributedText& current_text,
            const TextStyle& current_style, TextVerticalAlign vertical_align);
  static std::shared_ptr<const TextLayoutBackend> ForPaint(
      const PaintSequence& sequence, const DrawTextCommand& command);

private:
  const AttributedText& DisplayText() const noexcept { return elision ? elision->text : source; }
};

Size MeasureTextParagraph(MountedNode& node, UiWindow& ui_window, const Constraints& constraints);
std::shared_ptr<const CachedParagraphLayout> GetParagraphLayout(MountedNode& node, UiWindow& ui_window);

} // namespace huxerui::detail

namespace huxerui {

struct TextLayout::Data {
  AttributedText text;
  TextStyle style;
  TextLayoutOptions options;
  float width = 0.0F;
  TextLayoutMetrics metrics;
  std::vector<TextLineMetrics> lines;
  std::vector<detail::InlineObjectPlacement> inline_objects;
  std::unique_ptr<detail::TextLayoutBackend> backend;
  bool synthesized_last_line = false;
  std::size_t cache_cost = 0;

  std::size_t InlineObjectIndex(TextOffset offset) const;
  Rect InlineObjectRect(std::size_t index) const;
  TextOffset PreviousCaretOffset(TextOffset offset) const;
  TextOffset NextCaretOffset(TextOffset offset) const;
};

} // namespace huxerui
