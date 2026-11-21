#include "ohos_text_layout.h"

#include <native_drawing/drawing_brush.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_font_collection.h>
#include <native_drawing/drawing_rect.h>
#include <native_drawing/drawing_register_font.h>
#include <native_drawing/drawing_text_line.h>
#include <native_drawing/drawing_text_typography.h>
#include <unicode/ubidi.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "text/font_internal.h"
#include "text/text_internal.h"

#include "ohos_renderer.h"

namespace huxerui::detail {
namespace {

template <auto destroy, class T>
auto Own(T* value) {
  if (!value) {
    throw std::runtime_error("HuxerUI failed to allocate an OHOS text resource");
  }
  return std::unique_ptr<T, decltype(destroy)>(value, destroy);
}

struct ParagraphFonts {
  std::string Family(const Font& font) {
    if (const auto face = ResolveFontFace(font)) {
      const auto name = "HuxerUI-" + std::to_string(face->identity);
      if (!collection) {
        collection = Own<OH_Drawing_DestroyFontCollection>(OH_Drawing_CreateFontCollection());
      }
      const auto found = std::ranges::find_if(faces, [&](const auto& entry) { return entry.first == face; });
      if (found == faces.end()) {
        auto* bytes = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(face->bytes.data()));
        const bool registered = OH_Drawing_RegisterFontBuffer(collection.get(), name.c_str(), bytes,
                                                               face->bytes.size()) == 0;
        faces.emplace_back(face, registered);
        return registered ? name : std::string{};
      }
      return found->second ? name : std::string{};
    }
    return font.FamilyKind() == FontFamilyKind::Monospace ? "monospace" : std::string(font.FamilyName());
  }

  OH_Drawing_FontCollection* Get() const {
    return collection ? collection.get() : OH_Drawing_GetFontCollectionGlobalInstance();
  }

  std::vector<std::pair<std::shared_ptr<const FontFaceData>, bool>> faces;
  std::unique_ptr<OH_Drawing_FontCollection, decltype(&OH_Drawing_DestroyFontCollection)>
      collection{nullptr, &OH_Drawing_DestroyFontCollection};
};

auto MakeStyle(const TextStyle& style, const TextShapingOptions& shaping, ParagraphFonts& fonts,
               Color background = Color::Transparent()) {
  auto result = Own<OH_Drawing_DestroyTextStyle>(OH_Drawing_CreateTextStyle());
  const Font& font = style.font;
  OH_Drawing_SetTextStyleFontSize(result.get(), font.Size());
  OH_Drawing_SetTextStyleFontWeight(result.get(), static_cast<int>(font.Weight()) / 100 - 1);
  const auto slant = font.Slant() == FontSlant::Italic ? FONT_STYLE_ITALIC : FONT_STYLE_NORMAL;
  OH_Drawing_SetTextStyleFontStyle(result.get(), slant);
  if (const auto family = fonts.Family(font); !family.empty()) {
    const char* name = family.c_str();
    OH_Drawing_SetTextStyleFontFamilies(result.get(), 1, &name);
  }
  if (!shaping.locale.empty()) {
    OH_Drawing_SetTextStyleLocale(result.get(), shaping.locale.c_str());
  }
  const auto color = OhosDrawingColor(style.foreground);
  OH_Drawing_SetTextStyleColor(result.get(), color);
  int decoration = TEXT_DECORATION_NONE;
  if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
    decoration |= TEXT_DECORATION_UNDERLINE;
  }
  if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
    decoration |= TEXT_DECORATION_LINE_THROUGH;
  }
  OH_Drawing_SetTextStyleDecoration(result.get(), decoration);
  OH_Drawing_SetTextStyleDecorationColor(result.get(), color);
  if (background.alpha > 0.0F) {
    auto brush = Own<OH_Drawing_BrushDestroy>(OH_Drawing_BrushCreate());
    OH_Drawing_BrushSetColor(brush.get(), OhosDrawingColor(background));
    OH_Drawing_SetTextStyleBackgroundBrush(result.get(), brush.get());
  }
  return result;
}

using Typography = std::unique_ptr<OH_Drawing_Typography, decltype(&OH_Drawing_DestroyTypography)>;

struct Paragraph {
  ParagraphFonts fonts;
  Typography typography;
  float offset_x = 0.0F;
};

struct TextBoundary {
  TextOffset typography_offset = 0;
  TextOffset source_offset = 0;
  bool follows_break = false;
};

struct ParagraphSource {
  std::u16string text;
  std::vector<TextBoundary> boundaries{{}};
};

std::string NormalizeTextRun(std::string_view source, const ResolvedTextRun& run,
                             ParagraphSource* paragraph) {
  const auto bytes = source.substr(run.byte_start, run.byte_end - run.byte_start);
  if (!paragraph && bytes.find_first_of(std::string_view("\r\0\xE2", 3)) == bytes.npos) {
    return std::string(bytes);
  }

  std::string result;
  result.reserve(bytes.size());
  TextOffset offset = run.range.start;
  for (std::size_t byte = run.byte_start; byte < run.byte_end;) {
    Utf8CodePoint point;
    if (!DecodeCodePoint(source, byte, point)) {
      throw std::logic_error("HuxerUI OHOS typography requires valid UTF-8");
    }
    const TextOffset length = point.value > 0xFFFFU ? 2 : 1;
    offset += length;
    const bool hard_break = point.value == '\r' || point.value == '\n' ||
                            point.value == 0x2028U || point.value == 0x2029U;
    if (point.value == '\n' && byte != 0 && source[byte - 1] == '\r') {
      // Keep the source CRLF together even if its characters have different attributes.
      if (paragraph) {
        paragraph->boundaries.back().source_offset = offset;
      }
    } else {
      if (hard_break) {
        result.push_back('\n');
      } else if (point.value == 0) {
        // API 19 only accepts NUL-terminated strings. Preserve the invisible character's index.
        result.append("\xE2\x81\xA0");
      } else {
        result.append(source.substr(byte, point.byte_length));
      }
      if (paragraph) {
        if (length == 2) {
          const auto scalar = point.value - 0x10000U;
          paragraph->text.push_back(static_cast<char16_t>(0xD800U + (scalar >> 10U)));
          paragraph->text.push_back(static_cast<char16_t>(0xDC00U + (scalar & 0x3FFU)));
        } else {
          paragraph->text.push_back(hard_break ? u'\n' : point.value == 0 ? u'\u2060' : char16_t(point.value));
        }
        paragraph->boundaries.push_back({static_cast<TextOffset>(paragraph->text.size()), offset, hard_break});
      }
    }
    byte += point.byte_length;
  }
  return result;
}

Paragraph Shape(const AttributedText& text, const TextStyle& style, float width, const TextLayoutOptions& options,
                ParagraphSource* paragraph_source = nullptr) {
  auto paragraph = Own<OH_Drawing_DestroyTypographyStyle>(OH_Drawing_CreateTypographyStyle());
  const auto direction = ResolveParagraphDirection(text.PlainText(), options.shaping.direction);
  const bool rtl = direction == TextDirection::RightToLeft;
  const int align = options.align == TextAlign::Center ? TEXT_ALIGN_CENTER :
                    (options.align == TextAlign::Leading) != rtl ? TEXT_ALIGN_LEFT : TEXT_ALIGN_RIGHT;
  OH_Drawing_SetTypographyTextDirection(paragraph.get(), rtl ? TEXT_DIRECTION_RTL : TEXT_DIRECTION_LTR);
  OH_Drawing_SetTypographyTextAlign(paragraph.get(), align);
  ParagraphFonts fonts;
  auto base = MakeStyle(style, options.shaping, fonts);
  OH_Drawing_SetTypographyTextStyle(paragraph.get(), base.get());
  const auto runs = ResolveTextRuns(text, style);
  // The handler captures its font collection when it is created.
  for (const auto& run : runs) {
    if (run.style.font.FamilyKind() == FontFamilyKind::Custom) {
      fonts.Family(run.style.font);
    }
  }
  auto handler = Own<OH_Drawing_DestroyTypographyHandler>(
      OH_Drawing_CreateTypographyHandler(paragraph.get(), fonts.Get()));
  const auto& source = text.PlainText();
  for (const auto& run : runs) {
    if (run.inline_object) {
      const auto& object = *run.inline_object;
      OH_Drawing_PlaceholderSpan span{object.width, object.ascent + object.descent, ALIGNMENT_OFFSET_AT_BASELINE,
                                    TEXT_BASELINE_ALPHABETIC, object.ascent};
      OH_Drawing_TypographyHandlerAddPlaceholder(handler.get(), &span);
      if (paragraph_source) {
        paragraph_source->text.push_back(u'\uFFFC');
        paragraph_source->boundaries.push_back(
            {static_cast<TextOffset>(paragraph_source->text.size()), object.offset + 1, false});
      }
      continue;
    }
    auto run_style = MakeStyle(run.style, options.shaping, fonts, run.background);
    const auto utf8 = NormalizeTextRun(source, run, paragraph_source);
    OH_Drawing_TypographyHandlerPushTextStyle(handler.get(), run_style.get());
    OH_Drawing_TypographyHandlerAddText(handler.get(), utf8.c_str());
    OH_Drawing_TypographyHandlerPopTextStyle(handler.get());
  }
  auto typography = Own<OH_Drawing_DestroyTypography>(OH_Drawing_CreateTypography(handler.get()));
  if (std::isfinite(width) && options.wrap == TextWrap::Word) {
    OH_Drawing_TypographyLayout(typography.get(), std::max(0.0F, width));
    return {std::move(fonts), std::move(typography)};
  }
  OH_Drawing_TypographyLayout(typography.get(), std::numeric_limits<float>::infinity());
  const double intrinsic_width = std::ceil(OH_Drawing_TypographyGetMaxIntrinsicWidth(typography.get()));
  const double layout_width = std::isfinite(width) ? std::max<double>(width, intrinsic_width) : intrinsic_width;
  OH_Drawing_TypographyLayout(typography.get(), layout_width);
  const float overflow = std::isfinite(width) ? static_cast<float>(width - layout_width) : 0.0F;
  const float offset = align == TEXT_ALIGN_RIGHT ? overflow : align == TEXT_ALIGN_CENTER ? overflow * 0.5F : 0.0F;
  return {std::move(fonts), std::move(typography), offset};
}

FontMetrics ReadFontMetrics(Paragraph& paragraph, const TextStyle& style, const TextShapingOptions& shaping) {
  auto native_style = MakeStyle(style, shaping, paragraph.fonts);
  OH_Drawing_Font_Metrics metrics{};
  if (!OH_Drawing_TextStyleGetFontMetrics(paragraph.typography.get(), native_style.get(), &metrics)) {
    throw std::runtime_error("HuxerUI could not read OHOS font metrics");
  }
  return {std::abs(metrics.ascent), std::abs(metrics.descent), std::max(0.0F, metrics.leading),
          std::abs(metrics.underlinePosition), metrics.underlineThickness,
          std::abs(metrics.strikeoutPosition), metrics.strikeoutThickness};
}

TextLayoutMetrics ReadParagraphMetrics(OH_Drawing_Typography* typography) {
  const float longest = static_cast<float>(OH_Drawing_TypographyGetLongestLine(typography));
  const float height = static_cast<float>(OH_Drawing_TypographyGetHeight(typography));
  const float first = static_cast<float>(OH_Drawing_TypographyGetAlphabeticBaseline(typography));
  const auto line_count = OH_Drawing_TypographyGetLineCount(typography);
  OH_Drawing_LineMetrics last{};
  const float last_baseline = line_count > 1 &&
      OH_Drawing_TypographyGetLineMetricsAt(typography, static_cast<int>(line_count - 1), &last)
      ? static_cast<float>(last.y + std::abs(last.ascender)) : first;
  return {{longest, height}, first, last_baseline, line_count};
}

struct Line {
  TextLineMetrics metrics;
  std::size_t first_scalar;
  std::size_t end_scalar;
};

std::vector<Line> ReadLines(const Paragraph& paragraph, const ParagraphSource& source, std::size_t count) {
  std::vector<Line> lines;
  if (source.text.empty()) {
    return lines;
  }
  lines.reserve(count);
  const auto& boundaries = source.boundaries;
  const auto scalar_at = [&](std::size_t offset) {
    const auto found = std::ranges::lower_bound(boundaries, static_cast<TextOffset>(offset), {},
                                                 &TextBoundary::typography_offset);
    return std::min(static_cast<std::size_t>(found - boundaries.begin()), boundaries.size() - 1);
  };
  for (std::size_t i = 0; i < count; ++i) {
    OH_Drawing_LineMetrics line{};
    if (!OH_Drawing_TypographyGetLineMetricsAt(paragraph.typography.get(), static_cast<int>(i), &line)) {
      throw std::runtime_error("HuxerUI could not read OHOS line metrics");
    }
    auto first = scalar_at(line.startIndex);
    if (i + 1 == count && line.width == 0 && boundaries.back().follows_break) {
      // Some API 19 builds include the final break in the synthetic empty line's range.
      first = boundaries.size() - 1;
    }
    lines.push_back({{
        {boundaries[first].source_offset, boundaries[first].source_offset},
        {static_cast<float>(line.x) + paragraph.offset_x, static_cast<float>(line.y),
         static_cast<float>(line.width), static_cast<float>(line.height)},
        static_cast<float>(line.y + std::abs(line.ascender)),
    }, first, first});
  }
  // The native end index can omit trailing spaces on soft wraps. Use the next line's start and trim hard breaks only.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    auto& line = lines[i];
    auto end = i + 1 < lines.size() ? lines[i + 1].first_scalar : boundaries.size() - 1;
    while (end > line.first_scalar && boundaries[end].follows_break) {
      --end;
    }
    line.end_scalar = std::max(end, line.first_scalar);
    line.metrics.range.end = boundaries[line.end_scalar].source_offset;
  }
  return lines;
}

struct GraphemeBox {
  TextRange range;
  float left = 0.0F;
  float width = 0.0F;
  UBiDiLevel level = 0;

  TextPosition Left() const {
    return (level & 1) ? TextPosition{range.end, TextAffinity::Upstream} : TextPosition{range.start};
  }

  TextPosition Right() const {
    return (level & 1) ? TextPosition{range.start} : TextPosition{range.end, TextAffinity::Upstream};
  }
};

struct LineAdvances {
  std::span<float> widths;
  std::size_t count = 0;
  float leading_edge = 0.0F;
};

// Native Drawing invokes the enumeration callback synchronously and does not accept user data.
thread_local LineAdvances pending_advances;

std::vector<float> ReadAdvances(OH_Drawing_TextLine* drawing_line, const Line& line) {
  if (!drawing_line) {
    throw std::runtime_error("HuxerUI could not read OHOS text advances");
  }
  const auto glyphs = static_cast<std::size_t>(OH_Drawing_TextLineGetGlyphCount(drawing_line));
  std::vector<float> advances(glyphs + line.end_scalar - line.first_scalar + 1);
  const auto previous = std::exchange(pending_advances, LineAdvances{advances});
  OH_Drawing_TextLineEnumerateCaretOffsets(drawing_line, [](double offset, std::int32_t, bool leading) noexcept {
    auto& line = pending_advances;
    const float x = static_cast<float>(offset);
    if (leading) {
      line.leading_edge = x;
    } else {
      if (line.count == line.widths.size()) {
        return true;
      }
      line.widths[line.count++] = std::abs(x - line.leading_edge);
    }
    return false;
  });
  advances.resize(pending_advances.count);
  pending_advances = previous;
  return advances;
}

std::vector<GraphemeBox> GraphemeBoxes(const Line& line, std::span<const TextBoundary> boundaries,
                                     std::span<const TextOffset> graphemes, const UBiDi* bidi_line) {
  std::vector<GraphemeBox> boxes;
  auto grapheme = std::upper_bound(graphemes.begin(), graphemes.end(), line.metrics.range.start);
  auto scalar = line.first_scalar;
  const auto first_offset = boundaries[scalar].typography_offset;
  while (scalar < line.end_scalar && grapheme != graphemes.end()) {
    const auto start = boundaries[scalar];
    const auto offset_in_line = static_cast<int>(start.typography_offset - first_offset);
    const TextOffset end = std::min(*grapheme, line.metrics.range.end);
    while (scalar < line.end_scalar && boundaries[scalar].source_offset < end) {
      ++scalar;
    }
    boxes.push_back({{start.source_offset, end}, 0.0F, 0.0F, ubidi_getLevelAt(bidi_line, offset_in_line)});
    ++grapheme;
  }
  return boxes;
}

std::vector<GraphemeBox> PlaceVisualBoxes(std::span<const GraphemeBox> boxes, float x) {
  std::vector<UBiDiLevel> levels;
  levels.reserve(boxes.size());
  for (const auto& box : boxes) {
    levels.push_back(box.level);
  }
  std::vector<int> visual_indices(boxes.size());
  ubidi_reorderVisual(levels.data(), static_cast<int>(levels.size()), visual_indices.data());
  std::vector<GraphemeBox> visual;
  visual.reserve(boxes.size());
  for (int index : visual_indices) {
    GraphemeBox box = boxes[index];
    box.left = x;
    x += box.width;
    visual.push_back(box);
  }
  return visual;
}

class OhosTextLayout final : public TextLayoutBackend {
public:
  OhosTextLayout(const AttributedText& text, const TextStyle& style, float width, const TextLayoutOptions& options)
      : text_(text), style_(style), options_(options), paragraph_(Shape(text, style, width, options, &source_)),
        metrics_(ReadParagraphMetrics(paragraph_.typography.get())),
        lines_(ReadLines(paragraph_, source_, metrics_.line_count)), graphemes_(GraphemeBoundaries(text.PlainText())),
        direction_(ResolveParagraphDirection(text.PlainText(), options.shaping.direction)) {}

  Size Measure() const override { return metrics_.size; }

  std::vector<TextLineMetrics> LineMetrics() const override {
    std::vector<TextLineMetrics> result;
    result.reserve(lines_.size());
    for (const auto& line : lines_) {
      result.push_back(line.metrics);
    }
    return result;
  }

  TextPosition HitTest(Point point) const override {
    if (lines_.empty()) {
      return {};
    }
    const auto after = std::upper_bound(lines_.begin(), lines_.end(), point.y,
        [](float y, const Line& line) { return y < line.metrics.bounds.y; });
    const auto index = after == lines_.begin() ? std::size_t{0} : static_cast<std::size_t>(after - lines_.begin() - 1);
    const auto& boxes = Geometry()[index];
    for (const auto& box : boxes) {
      if (point.x < box.left + box.width * 0.5F) {
        return box.Left();
      }
      if (point.x <= box.left + box.width) {
        return box.Right();
      }
    }
    return boxes.empty() ? TextPosition{lines_[index].metrics.range.start} : boxes.back().Right();
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    if (lines_.empty()) {
      return {};
    }
    auto next = std::upper_bound(lines_.begin(), lines_.end(), offset,
        [](TextOffset value, const Line& line) { return value < line.metrics.range.start; });
    auto line = next == lines_.begin() ? next : std::prev(next);
    if (line != lines_.begin() && affinity == TextAffinity::Upstream && line->metrics.range.start == offset &&
        std::prev(line)->metrics.range.end == offset) {
      --line;
    }
    const auto& bounds = line->metrics.bounds;
    const auto& boxes = Geometry()[static_cast<std::size_t>(line - lines_.begin())];
    std::optional<float> adjacent;
    for (const auto& box : boxes) {
      for (const auto& edge : {std::pair{box.Left(), box.left}, std::pair{box.Right(), box.left + box.width}}) {
        if (edge.first.offset == offset) {
          if (edge.first.affinity == affinity) {
            return {edge.second, bounds.y, 1.0F, bounds.height};
          }
          adjacent = edge.second;
        }
      }
    }
    return {adjacent.value_or(bounds.x), bounds.y, 1.0F, bounds.height};
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    std::vector<Rect> rects;
    if (range.IsCollapsed()) {
      return rects;
    }
    const auto& lines = Geometry();
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const auto& bounds = lines_[i].metrics.bounds;
      bool extending = false;
      for (const auto& box : lines[i]) {
        if (box.width <= 0.0F) {
          continue;
        }
        if (box.range.start < range.end && box.range.end > range.start) {
          if (extending) {
            rects.back().width = box.left + box.width - rects.back().x;
          } else {
            rects.push_back({box.left, bounds.y, box.width, bounds.height});
          }
          extending = true;
        } else {
          extending = false;
        }
      }
    }
    return rects;
  }

  std::vector<InlineObjectPlacement> InlineObjectPlacements(
      std::span<const TextInlineObject> objects, std::span<const TextLineMetrics> lines) const override {
    const std::unique_ptr<OH_Drawing_TextBox, decltype(&OH_Drawing_TypographyDestroyTextBox)> boxes(
        OH_Drawing_TypographyGetRectsForPlaceholders(paragraph_.typography.get()),
        &OH_Drawing_TypographyDestroyTextBox);
    if (!boxes || OH_Drawing_GetSizeOfTextBox(boxes.get()) != objects.size()) {
      return TextLayoutBackend::InlineObjectPlacements(objects, lines);
    }
    std::vector<InlineObjectPlacement> placements;
    placements.reserve(objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index) {
      const int box = static_cast<int>(index);
      const auto left = OH_Drawing_GetLeftFromTextBox(boxes.get(), box) + paragraph_.offset_x;
      const auto right = OH_Drawing_GetRightFromTextBox(boxes.get(), box) + paragraph_.offset_x;
      const auto baseline = OH_Drawing_GetTopFromTextBox(boxes.get(), box) + objects[index].ascent;
      const bool rtl = OH_Drawing_GetTextDirectionFromTextBox(boxes.get(), box) == TEXT_DIRECTION_RTL;
      placements.push_back({rtl ? right : left, rtl ? left : right, baseline});
    }
    return placements;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const auto next = std::lower_bound(graphemes_.begin(), graphemes_.end(), offset);
    return next == graphemes_.begin() ? graphemes_.front() : *std::prev(next);
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const auto next = std::upper_bound(graphemes_.begin(), graphemes_.end(), offset);
    return next == graphemes_.end() ? graphemes_.back() : *next;
  }

  const Paragraph& ShapedParagraph() const noexcept { return paragraph_; }

private:
  const TextInlineObject* ObjectAt(TextOffset offset) const {
    const auto objects = text_.InlineObjects();
    const auto found = std::ranges::lower_bound(objects, offset, {}, &TextInlineObject::offset);
    return found != objects.end() && found->offset == offset ? &*found : nullptr;
  }

  void SetBoxWidths(std::span<GraphemeBox> boxes, std::span<const float> advances, float line_width) const {
    if (boxes.size() == advances.size()) {
      for (std::size_t i = 0; i < boxes.size(); ++i) {
        const auto* object = text_.InlineObjects().empty() ? nullptr : ObjectAt(boxes[i].range.start);
        boxes[i].width = object ? object->width : advances[i];
      }
      return;
    }

    // The API reports glyph advances, so ligatures can span several graphemes without providing their boundaries.
    // Estimate interior stops proportionally and keep the final stop at the width of the shaped line.
    float total = 0.0F;
    float object_width = 0.0F;
    const auto styles = text_.StyleRanges();
    for (auto& box : boxes) {
      if (const auto* object = ObjectAt(box.range.start)) {
        box.width = object->width;
        object_width += box.width;
        continue;
      }
      const auto next = std::upper_bound(styles.begin(), styles.end(), box.range.start,
          [](TextOffset value, const TextStyleRange& candidate) { return value < candidate.range.start; });
      const auto style_range = next == styles.begin() ? styles.end() : std::prev(next);
      const auto style = style_range != styles.end() && box.range.start < style_range->range.end
          ? ResolveTextStyle(style_, style_range->style) : style_;
      const auto isolated = Shape(AttributedText(text_.TextInRange(box.range)), style,
                                  std::numeric_limits<float>::infinity(), options_);
      box.width = static_cast<float>(OH_Drawing_TypographyGetMaxIntrinsicWidth(isolated.typography.get()));
      total += box.width;
    }
    if (total > 0.0F) {
      const float width = std::max(line_width, std::accumulate(advances.begin(), advances.end(), 0.0F));
      const float scale = std::max(0.0F, width - object_width) / total;
      for (auto& box : boxes) {
        if (!ObjectAt(box.range.start)) {
          box.width *= scale;
        }
      }
    }
  }

  const std::vector<std::vector<GraphemeBox>>& Geometry() const {
    if (geometry_) {
      return *geometry_;
    }
    std::vector<std::vector<GraphemeBox>> geometry(lines_.size());
    if (!source_.text.empty() && !lines_.empty()) {
      // API 19 paragraph range queries can return no boxes. Keep the shaped line advances and resolve visual order.
      auto native_lines = Own<OH_Drawing_DestroyTextLines>(
          OH_Drawing_TypographyGetTextLines(paragraph_.typography.get()));
      auto bidi = Own<ubidi_close>(ubidi_open());
      auto bidi_line = Own<ubidi_close>(ubidi_open());
      UErrorCode error = U_ZERO_ERROR;
      const UBiDiLevel base = direction_ == TextDirection::RightToLeft ? 1 : 0;
      ubidi_setPara(bidi.get(), source_.text.data(), static_cast<int>(source_.text.size()), base, nullptr, &error);
      for (std::size_t i = 0; i < lines_.size(); ++i) {
        const auto& line = lines_[i];
        if (line.first_scalar >= line.end_scalar) {
          continue;
        }
        const auto start = static_cast<int>(source_.boundaries[line.first_scalar].typography_offset);
        const auto end = static_cast<int>(source_.boundaries[line.end_scalar].typography_offset);
        ubidi_setLine(bidi.get(), start, end, bidi_line.get(), &error);
        if (U_FAILURE(error)) {
          throw std::runtime_error("HuxerUI could not resolve OHOS text direction for a line");
        }
        auto* drawing_line = OH_Drawing_GetTextLineByIndex(native_lines.get(), i);
        auto advances = ReadAdvances(drawing_line, line);
        const auto& boundaries = source_.boundaries;
        if (line.end_scalar + 1 < boundaries.size() && boundaries[line.end_scalar + 1].follows_break &&
            !advances.empty() && advances.back() == 0.0F) {
          advances.pop_back();
        }
        auto boxes = GraphemeBoxes(line, boundaries, graphemes_, bidi_line.get());
        SetBoxWidths(boxes, advances, line.metrics.bounds.width);
        geometry[i] = PlaceVisualBoxes(boxes, line.metrics.bounds.x);
      }
    }
    return geometry_.emplace(std::move(geometry));
  }

  AttributedText text_;
  TextStyle style_;
  TextLayoutOptions options_;
  ParagraphSource source_;
  Paragraph paragraph_;
  TextLayoutMetrics metrics_;
  std::vector<Line> lines_;
  std::vector<TextOffset> graphemes_;
  TextDirection direction_;
  mutable std::optional<std::vector<std::vector<GraphemeBox>>> geometry_;
};

} // namespace

FontMetrics MeasureOhosFont(const Font& font) {
  const TextStyle style{.font = font};
  auto paragraph = Shape(AttributedText("Hg"), style, std::numeric_limits<float>::infinity(), {});
  return ReadFontMetrics(paragraph, style, {});
}

TextRunMetrics MeasureOhosRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  RequireSingleLineTextRun(text);
  auto paragraph = Shape(AttributedText(std::string(text)), style, std::numeric_limits<float>::infinity(),
                         {.shaping = options, .wrap = TextWrap::NoWrap});
  const auto font_metrics = ReadFontMetrics(paragraph, style, options);
  const float advance = static_cast<float>(OH_Drawing_TypographyGetMaxIntrinsicWidth(paragraph.typography.get()));
  return {advance, {0.0F, -font_metrics.ascent, advance, font_metrics.LineHeight()}, font_metrics};
}

TextLayoutMetrics MeasureOhosText(const AttributedText& text, const TextStyle& style, float width,
                                  const TextLayoutOptions& options) {
  if (width <= 0.0F) {
    return {};
  }
  const auto paragraph = Shape(text, style, width, options);
  auto metrics = ReadParagraphMetrics(paragraph.typography.get());
  metrics.size.width = std::min(metrics.size.width, width);
  return metrics;
}

std::unique_ptr<TextLayoutBackend> CreateOhosTextLayout(const AttributedText& text, const TextStyle& style, float width,
                                                       const TextLayoutOptions& options) {
  return std::make_unique<OhosTextLayout>(text, style, width, options);
}

void PaintOhosText(OH_Drawing_Canvas* canvas, const DrawTextCommand& command, const TextLayoutBackend* prepared) {
  const Rect rect = command.rect;
  const auto* layout = dynamic_cast<const OhosTextLayout*>(prepared);
  const auto shaped = layout ? std::nullopt :
      std::optional(Shape(command.text, command.style, rect.width, command.options));
  const auto& paragraph = layout ? layout->ShapedParagraph() : *shaped;
  auto* typography = paragraph.typography.get();
  const auto clip = Own<OH_Drawing_RectDestroy>(
      OH_Drawing_RectCreate(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height));
  const double free_height = std::max(0.0, rect.height - OH_Drawing_TypographyGetHeight(typography));
  const auto align = command.options.vertical_align;
  const double offset_y = align == TextVerticalAlign::Bottom ? free_height :
                          align == TextVerticalAlign::Center ? free_height * 0.5 : 0.0;
  OH_Drawing_CanvasSave(canvas);
  OH_Drawing_CanvasClipRect(canvas, clip.get(), INTERSECT, false);
  OH_Drawing_TypographyPaint(typography, canvas, rect.x + paragraph.offset_x + command.paragraph_offset.x,
                             rect.y + offset_y + command.paragraph_offset.y);
  OH_Drawing_CanvasRestore(canvas);
}

void PaintOhosTextRuns(OH_Drawing_Canvas* canvas, const DrawTextRunsCommand& command) {
  for (const auto& run : command.runs) {
    const auto paragraph = Shape(AttributedText(run.text), run.style, std::numeric_limits<float>::infinity(),
                                 {.shaping = run.shaping, .wrap = TextWrap::NoWrap});
    auto* typography = paragraph.typography.get();
    const auto baseline = OH_Drawing_TypographyGetAlphabeticBaseline(typography);
    OH_Drawing_TypographyPaint(typography, canvas, run.baseline_origin.x, run.baseline_origin.y - baseline);
  }
}

} // namespace huxerui::detail
