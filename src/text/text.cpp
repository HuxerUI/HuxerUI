#include <huxerui/text.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>

#include "runtime/mounted_node_internal.h"
#include "internal_access.h"
#include "graphics/paint_internal.h"
#include "text_internal.h"
#include "macros_internal.h"

namespace huxerui {
namespace {

void RequireFontSize(float size) {
  if (!std::isfinite(size) || size <= 0.0F) {
    throw std::invalid_argument("HuxerUI font size must be finite and greater than zero");
  }
}

struct TextBody {
  struct Checkpoint {
    TextOffset offset;
    std::size_t byte;
  };

  explicit TextBody(std::string value) : text(std::move(value)) {
    if (!std::in_range<TextOffset>(text.size())) {
      throw std::invalid_argument("HuxerUI attributed text exceeds the supported length");
    }
    // Sparse scalar-boundary checkpoints bound UTF-16-to-byte scans without storing an index for every code unit.
    checkpoints.push_back({0, 0});
    for (std::size_t byte = 0; byte < text.size();) {
      detail::Utf8CodePoint point;
      if (!detail::DecodeCodePoint(text, byte, point)) {
        throw std::invalid_argument("HuxerUI attributed text must be valid UTF-8");
      }
      length += point.value > 0xFFFFU ? 2 : 1;
      byte += point.byte_length;
      if (length - checkpoints.back().offset >= 64) {
        checkpoints.push_back({length, byte});
      }
    }
    hash = std::hash<std::string_view>{}(text);
  }

  std::size_t ByteOffset(TextOffset offset) const {
    if (offset < 0 || offset > length) {
      throw std::invalid_argument("HuxerUI text range must be within the paragraph");
    }
    if (offset == length) {
      return text.size();
    }
    const auto next =
        std::upper_bound(checkpoints.begin(), checkpoints.end(), offset, [](TextOffset value, const Checkpoint& item) {
          return value < item.offset;
        });
    auto [current, byte] = *std::prev(next);
    while (current < offset) {
      detail::Utf8CodePoint point;
      detail::DecodeCodePoint(text, byte, point);
      current += point.value > 0xFFFFU ? 2 : 1;
      byte += point.byte_length;
    }
    if (current != offset) {
      throw std::invalid_argument("HuxerUI text range must not split a UTF-16 surrogate pair");
    }
    return byte;
  }

  void ValidateRange(TextRange range) const {
    if (!range.IsValid()) {
      throw std::invalid_argument("HuxerUI text range must be ordered and non-negative");
    }
    ByteOffset(range.start);
    ByteOffset(range.end);
  }

  std::string text;
  TextOffset length = 0;
  std::size_t hash = 0;
  std::vector<Checkpoint> checkpoints;
};

TextSpanStyle NormalizeStyle(TextSpanStyle style) {
  if (style.font_size && (!std::isfinite(*style.font_size) || *style.font_size <= 0.0F)) {
    throw std::invalid_argument("HuxerUI text span font size must be finite and greater than zero");
  }
  const auto weight = style.font_weight.value_or(style.font ? style.font->Weight() : FontWeight::Regular);
  if (static_cast<unsigned>(weight) < 100 || static_cast<unsigned>(weight) > 900 ||
      static_cast<unsigned>(weight) % 100 != 0) {
    throw std::invalid_argument("HuxerUI text span font weight must be a known FontWeight");
  }
  const auto slant = style.font_slant.value_or(style.font ? style.font->Slant() : FontSlant::Normal);
  if (slant != FontSlant::Normal && slant != FontSlant::Italic) {
    throw std::invalid_argument("HuxerUI text span font slant must be a known FontSlant");
  }
  if (style.foreground) {
    detail::ValidateColor(*style.foreground, "HuxerUI text span foreground must be finite");
  }
  if (style.background) {
    detail::ValidateColor(*style.background, "HuxerUI text span background must be finite");
  }
  if (style.decoration && static_cast<unsigned>(*style.decoration) > 3) {
    throw std::invalid_argument("HuxerUI text span decoration must contain known flags");
  }
  // Fold partial overrides into a supplied complete font so equivalent requests have one normalized representation.
  if (style.font) {
    style.font = style.font->WithSize(style.font_size.value_or(style.font->Size())).WithWeight(weight).WithSlant(slant);
    style.font_size.reset();
    style.font_weight.reset();
    style.font_slant.reset();
  }
  return style;
}

template <typename Range>
void ValidateRanges(const TextBody& body, const std::vector<Range>& ranges) {
  TextOffset previous_end = 0;
  for (const auto& item : ranges) {
    body.ValidateRange(item.range);
    if (item.range.start < previous_end) {
      throw std::invalid_argument("HuxerUI attributed ranges must be ordered and non-overlapping");
    }
    previous_end = item.range.end;
  }
}

std::vector<TextStyleRange> NormalizeStyles(const TextBody& body, std::vector<TextStyleRange> styles) {
  // Validate before dropping empty or inherited ranges; normalization must not silently accept malformed input.
  ValidateRanges(body, styles);
  std::size_t count = 0;
  for (auto& item : styles) {
    item.style = NormalizeStyle(std::move(item.style));
    if (item.range.IsCollapsed() || item.style == TextSpanStyle{}) {
      continue;
    }
    if (count > 0 && styles[count - 1].range.end == item.range.start && styles[count - 1].style == item.style) {
      styles[count - 1].range.end = item.range.end;
    } else {
      if (&styles[count] != &item) {
        styles[count] = std::move(item);
      }
      ++count;
    }
  }
  styles.resize(count);
  return styles;
}

} // namespace

Font::Font(FontFamily family, float size) : family_(std::move(family)), size_(size) {
  RequireFontSize(size);
}

Font Font::System(float size) {
  return Font{FontFamily::System(), size};
}

Font Font::Monospace(float size) {
  return Font{FontFamily::Monospace(), size};
}

Font Font::Named(std::string family, float size) {
  return Font{FontFamily::Named(std::move(family)), size};
}

Font Font::FromFamily(FontFamily family, float size) {
  return Font{std::move(family), size};
}

Font Font::FromResource(RawResource resource, float size) {
  RequireFontSize(size);
  return FromFamily(FontFamily::FromResources({std::move(resource)}), size);
}

Font Font::FromFile(const File& file, float size) {
  RequireFontSize(size);
  return FromFamily(FontFamily::FromFiles({file}), size);
}

Font Font::FromBytes(Bytes bytes, float size) {
  RequireFontSize(size);
  std::vector<Bytes> files;
  files.push_back(std::move(bytes));
  return FromFamily(FontFamily::FromBytes(std::move(files)), size);
}

Font Font::WithFamily(FontFamily family) const {
  Font result = *this;
  result.family_ = std::move(family);
  return result;
}

Font Font::WithSize(float size) const {
  RequireFontSize(size);
  Font result = *this;
  result.size_ = size;
  return result;
}

Font Font::WithWeight(FontWeight weight) const {
  Font result = *this;
  result.weight_ = weight;
  return result;
}

Font Font::WithSlant(FontSlant slant) const {
  Font result = *this;
  result.slant_ = slant;
  return result;
}

TextMeasurer& UseTextMeasurer() {
  const std::shared_ptr<detail::TextMeasurerService> service = UseService<detail::TextMeasurerService>();
  if (service->measurer == nullptr) {
    throw std::logic_error("HuxerUI text measurer service is disconnected");
  }
#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
  if (service->dependency) detail::ObserveDependency(service->dependency);
#endif
  return *service->measurer;
}

TextLayout TextMeasurer::CreateTextLayout(const AttributedText&, const TextStyle&, float, const TextLayoutOptions&) {
  throw std::logic_error("HuxerUI text measurer has no paragraph layout backend");
}

TextLayout::TextLayout(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

TextLayoutMetrics TextLayout::Metrics() const {
  return data_->metrics;
}

std::span<const TextLineMetrics> TextLayout::Lines() const {
  return data_->lines;
}

std::size_t TextLayout::Data::InlineObjectIndex(TextOffset offset) const {
  const auto objects = text.InlineObjects();
  const auto object = std::ranges::lower_bound(objects, offset, {}, &TextInlineObject::offset);
  return object != objects.end() && object->offset == offset
      ? static_cast<std::size_t>(object - objects.begin()) : objects.size();
}

Rect TextLayout::Data::InlineObjectRect(std::size_t index) const {
  const auto& object = text.InlineObjects()[index];
  const auto& placement = inline_objects[index];
  const float x = placement.leading <= placement.trailing ? placement.leading : placement.leading - object.width;
  return {x, placement.baseline - object.ascent, object.width, object.ascent + object.descent};
}

TextOffset TextLayout::Data::PreviousCaretOffset(TextOffset offset) const {
  const TextOffset previous = backend->PreviousCaretOffset(offset);
  const auto objects = text.InlineObjects();
  const auto object = std::ranges::lower_bound(objects, offset, {}, &TextInlineObject::offset);
  if (object == objects.begin()) {
    return previous;
  }
  return std::max(previous, std::min(offset - 1, std::prev(object)->offset + 1));
}

TextOffset TextLayout::Data::NextCaretOffset(TextOffset offset) const {
  const TextOffset next = backend->NextCaretOffset(offset);
  const auto objects = text.InlineObjects();
  const auto object = std::ranges::lower_bound(objects, offset, {}, &TextInlineObject::offset);
  return object == objects.end() ? next : std::min(next, std::max(offset + 1, object->offset));
}

TextPosition TextLayout::SnapCaret(TextPosition position) const {
  HUXERUI_DISCARD(data_->text.TextInRange({position.offset, position.offset}));
  if (position.offset > 0 && position.offset < data_->text.Length()) {
    const TextOffset previous = data_->PreviousCaretOffset(position.offset);
    const TextOffset next = data_->NextCaretOffset(previous);
    if (next != position.offset) {
      position.offset = position.affinity == TextAffinity::Upstream ? previous : next;
    }
  }
  return position;
}

std::size_t TextLayout::LineIndex(TextPosition position) const {
  position = SnapCaret(position);
  const auto& lines = data_->lines;
  auto line = std::upper_bound(lines.begin(), lines.end(), position.offset,
      [](TextOffset offset, const TextLineMetrics& candidate) { return offset < candidate.range.start; });
  if (line != lines.begin()) {
    --line;
  }
  if (line != lines.begin() && position.affinity == TextAffinity::Upstream &&
      line->range.start == position.offset && std::prev(line)->range.end == position.offset) {
    --line;
  }
  return static_cast<std::size_t>(line - lines.begin());
}

TextPosition TextLayout::HitTest(Point point) const {
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    throw std::invalid_argument("HuxerUI text layout hit-test point must be finite");
  }
  // A wrapped object's trailing edge can belong to the next line in native hit testing.
  const auto objects = data_->text.InlineObjects();
  for (std::size_t index = 0; index < objects.size(); ++index) {
    const Rect box = data_->InlineObjectRect(index);
    if (box.Contains(point)) {
      const auto& placement = data_->inline_objects[index];
      const bool right_half = point.x >= box.x + box.width * 0.5F;
      const bool trailing = right_half == (placement.leading <= placement.trailing);
      return {objects[index].offset + (trailing ? 1 : 0), trailing ? TextAffinity::Upstream : TextAffinity::Downstream};
    }
  }
  if (data_->synthesized_last_line && (data_->lines.size() == 1 || point.y >= data_->lines.back().bounds.y)) {
    return {data_->text.Length(), TextAffinity::Downstream};
  }
  return SnapCaret(data_->backend->HitTest(point));
}

Rect TextLayout::CaretRect(TextPosition position) const {
  position = SnapCaret(position);
  const bool upstream = position.affinity == TextAffinity::Upstream;
  const std::size_t object = data_->InlineObjectIndex(position.offset - (upstream ? 1 : 0));
  if (object < data_->inline_objects.size()) {
    const auto& placement = data_->inline_objects[object];
    const Rect line = data_->lines[LineIndex(position)].bounds;
    return {upstream ? placement.trailing : placement.leading, line.y, 1.0F, line.height};
  }
  if (data_->synthesized_last_line && position.offset == data_->text.Length()) {
    const Rect& line = data_->lines.back().bounds;
    return {line.x, line.y, 1.0F, line.height};
  }
  if (position.affinity == TextAffinity::Upstream) {
    const auto& lines = data_->lines;
    const auto line = std::lower_bound(lines.begin(), lines.end(), position.offset,
        [](const TextLineMetrics& candidate, TextOffset offset) { return candidate.range.start < offset; });
    // A source separator consumes offsets. Only a soft wrap offers two visual lines at one boundary.
    if (line != lines.begin() && line != lines.end() && line->range.start == position.offset &&
        std::prev(line)->range.end < position.offset) {
      position.affinity = TextAffinity::Downstream;
    }
  }
  return data_->backend->CaretRect(position.offset, position.affinity);
}

std::vector<Rect> TextLayout::RangeRects(TextRange range) const {
  HUXERUI_DISCARD(data_->text.TextInRange({range.start, range.start}));
  HUXERUI_DISCARD(data_->text.TextInRange({range.end, range.end}));
  if (range.end < range.start) {
    throw std::invalid_argument("HuxerUI text layout range must be ordered");
  }
  return range.IsCollapsed() ? std::vector<Rect>{} : data_->backend->RangeRects(range);
}

std::optional<Rect> TextLayout::InlineObjectRect(TextOffset offset) const {
  HUXERUI_DISCARD(data_->text.TextInRange({offset, offset}));
  const auto index = data_->InlineObjectIndex(offset);
  return index < data_->inline_objects.size() ? std::optional<Rect>(data_->InlineObjectRect(index)) : std::nullopt;
}

TextOffset TextLayout::PreviousCaretOffset(TextOffset offset) const {
  HUXERUI_DISCARD(data_->text.TextInRange({offset, offset}));
  return data_->PreviousCaretOffset(offset);
}

TextOffset TextLayout::NextCaretOffset(TextOffset offset) const {
  HUXERUI_DISCARD(data_->text.TextInRange({offset, offset}));
  return data_->NextCaretOffset(offset);
}

// Attribute replacement shares the immutable body, its UTF-16 index, and its comparison hash.
struct AttributedText::Storage {
  std::shared_ptr<const TextBody> body;
  std::vector<TextStyleRange> styles;
  std::vector<TextLinkRange> links;
  std::vector<TextInlineObject> inline_objects;
};

TextSpan::TextSpan(std::string text) : text_(std::move(text)) {}

TextSpan TextSpan::Style(TextSpanStyle style) && {
  style_ = NormalizeStyle(std::move(style));
  return std::move(*this);
}

TextSpan TextSpan::Link(Uri target) && {
  target_ = std::move(target);
  return std::move(*this);
}

AttributedText::AttributedText() {
  static const auto empty = std::make_shared<const Storage>(Storage{std::make_shared<const TextBody>(""), {}, {}, {}});
  storage_ = empty;
}

AttributedText::AttributedText(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}

AttributedText::AttributedText(std::string text) : AttributedText(FromRanges(std::move(text), {})) {}

AttributedText::AttributedText(std::initializer_list<TextSpan> spans)
    : AttributedText(std::span<const TextSpan>(spans.begin(), spans.size())) {}

AttributedText::AttributedText(std::span<const TextSpan> spans) {
  std::size_t size = 0;
  for (const auto& span : spans) {
    if (span.text_.size() > std::string{}.max_size() - size) {
      throw std::invalid_argument("HuxerUI attributed text exceeds the supported length");
    }
    size += span.text_.size();
  }
  std::string text;
  text.reserve(size);
  std::vector<TextStyleRange> styles;
  std::vector<TextLinkRange> links;
  TextOffset offset = 0;
  for (const auto& span : spans) {
    const TextOffset start = offset;
    for (std::size_t byte = 0; byte < span.text_.size();) {
      detail::Utf8CodePoint point;
      if (!detail::DecodeCodePoint(span.text_, byte, point)) {
        throw std::invalid_argument("HuxerUI attributed text fragments must be valid UTF-8");
      }
      offset += point.value > 0xFFFFU ? 2 : 1;
      byte += point.byte_length;
    }
    const TextRange range{start, offset};
    if (!span.text_.empty()) {
      text.append(span.text_);
      styles.push_back({range, span.style_});
      if (span.target_) {
        links.push_back({range, *span.target_});
      }
    }
  }
  *this = FromRanges(std::move(text), std::move(styles), std::move(links));
}

AttributedText
AttributedText::FromRanges(std::string text, std::vector<TextStyleRange> styles, std::vector<TextLinkRange> links) {
  if (text.empty() && styles.empty() && links.empty()) {
    return AttributedText{};
  }
  auto body = std::make_shared<const TextBody>(std::move(text));
  styles = NormalizeStyles(*body, std::move(styles));
  ValidateRanges(*body, links);
  // Unlike styles, adjacent equal links remain separate activation and accessibility targets.
  std::erase_if(links, [](const TextLinkRange& link) { return link.range.IsCollapsed(); });
  return AttributedText(std::make_shared<const Storage>(
      Storage{std::move(body), std::move(styles), std::move(links), {}}));
}

AttributedText AttributedText::WithStyles(std::vector<TextStyleRange> styles) const {
  styles = NormalizeStyles(*storage_->body, std::move(styles));
  if (styles == storage_->styles) {
    return *this;
  }
  return AttributedText(std::make_shared<const Storage>(
      Storage{storage_->body, std::move(styles), storage_->links, storage_->inline_objects}));
}

AttributedText AttributedText::WithInlineObjects(std::vector<TextInlineObject> objects) const {
  TextOffset previous = -1;
  for (const auto& object : objects) {
    if (object.offset <= previous || object.offset >= Length()) {
      throw std::invalid_argument("HuxerUI inline object offsets must be in range and strictly increasing");
    }
    const std::size_t byte = storage_->body->ByteOffset(object.offset);
    if (std::string_view(PlainText()).substr(byte, 3) != "\xEF\xBF\xBC") {
      throw std::invalid_argument("HuxerUI inline object offset must name U+FFFC");
    }
    const float height = object.ascent + object.descent;
    if (!std::isfinite(object.width) || object.width <= 0.0F || !std::isfinite(height) || height <= 0.0F ||
        object.ascent < 0.0F || object.descent < 0.0F) {
      throw std::invalid_argument("HuxerUI inline object size must be finite and positive, with nonnegative extents");
    }
    previous = object.offset;
  }
  if (objects == storage_->inline_objects) {
    return *this;
  }
  return AttributedText(std::make_shared<const Storage>(
      Storage{storage_->body, storage_->styles, storage_->links, std::move(objects)}));
}

const std::string& AttributedText::PlainText() const noexcept { return storage_->body->text; }
TextOffset AttributedText::Length() const noexcept { return storage_->body->length; }
std::span<const TextStyleRange> AttributedText::StyleRanges() const noexcept { return storage_->styles; }
std::span<const TextLinkRange> AttributedText::LinkRanges() const noexcept { return storage_->links; }
std::span<const TextInlineObject> AttributedText::InlineObjects() const noexcept { return storage_->inline_objects; }

std::string AttributedText::TextInRange(TextRange range) const {
  storage_->body->ValidateRange(range);
  const auto start = storage_->body->ByteOffset(range.start);
  return storage_->body->text.substr(start, storage_->body->ByteOffset(range.end) - start);
}

bool AttributedText::operator==(const AttributedText& other) const noexcept {
  if (storage_ == other.storage_) {
    return true;
  }
  return detail::InternalAccess::SameBody(*this, other) &&
         storage_->styles == other.storage_->styles && storage_->links == other.storage_->links &&
         storage_->inline_objects == other.storage_->inline_objects;
}

std::size_t detail::InternalAccess::BodyHash(const AttributedText& text) noexcept { return text.storage_->body->hash; }

bool detail::InternalAccess::SameBody(const AttributedText& left, const AttributedText& right) noexcept {
  if (left.storage_->body == right.storage_->body) {
    return true;
  }
  const auto& body = *left.storage_->body;
  const auto& other = *right.storage_->body;
  // The cached hash rejects unequal bodies quickly; collisions must still be resolved by content equality.
  return body.length == other.length && body.hash == other.hash && body.text == other.text;
}

namespace detail {

std::vector<InlineObjectPlacement> TextLayoutBackend::InlineObjectPlacements(
    std::span<const TextInlineObject> objects, std::span<const TextLineMetrics> lines) const {
  std::vector<InlineObjectPlacement> placements;
  placements.reserve(objects.size());
  auto line = lines.begin();
  for (const auto& object : objects) {
    while (line != lines.end() && line->range.end <= object.offset) {
      ++line;
    }
    if (line == lines.end() || line->range.start > object.offset) {
      throw std::logic_error("HuxerUI inline object is missing from the laid out lines");
    }
    placements.push_back({CaretRect(object.offset, TextAffinity::Downstream).x,
                          CaretRect(object.offset + 1, TextAffinity::Upstream).x, line->baseline});
  }
  return placements;
}

std::size_t ParagraphCacheCost(const AttributedText& text) noexcept {
  const auto bytes = text.PlainText().size();
  const auto ranges = text.StyleRanges().size() + text.LinkRanges().size() + text.InlineObjects().size();
  if (bytes > paragraph_cache_budget / 64 || ranges > paragraph_cache_budget / 256) {
    return paragraph_cache_budget + 1;
  }
  std::size_t cost = 256 + bytes * 64 + ranges * 256;
  for (const auto& item : text.StyleRanges()) {
    if (item.style.font) {
      const auto length = item.style.font->FamilyName().size();
      if (length > paragraph_cache_budget || cost > paragraph_cache_budget - length) {
        return paragraph_cache_budget + 1;
      }
      cost += length;
    }
  }
  for (const auto& link : text.LinkRanges()) {
    const auto length = link.target.ToString().size();
    if (length > paragraph_cache_budget || cost > paragraph_cache_budget - length) {
      return paragraph_cache_budget + 1;
    }
    cost += length;
  }
  return cost;
}

namespace {

class StyleCursor {
public:
  explicit StyleCursor(std::span<const TextStyleRange> styles) : styles_(styles) {}

  const TextSpanStyle& At(TextOffset offset) {
    while (index_ < styles_.size() && styles_[index_].range.end <= offset) {
      ++index_;
    }
    return index_ < styles_.size() && styles_[index_].range.start <= offset ? styles_[index_].style : inherited_;
  }

  TextOffset Next(TextOffset offset, TextOffset length) const {
    if (index_ == styles_.size()) {
      return length;
    }
    return styles_[index_].range.start > offset ? styles_[index_].range.start : styles_[index_].range.end;
  }

private:
  std::span<const TextStyleRange> styles_;
  std::size_t index_ = 0;
  TextSpanStyle inherited_;
};

Font ResolveFont(const Font& base, const TextSpanStyle& overrides) {
  Font font = overrides.font.value_or(base);
  if (overrides.font_size) {
    font = font.WithSize(*overrides.font_size);
  }
  if (overrides.font_weight) {
    font = font.WithWeight(*overrides.font_weight);
  }
  if (overrides.font_slant) {
    font = font.WithSlant(*overrides.font_slant);
  }
  return font;
}

template <typename Equal>
bool EqualProjections(const AttributedText& left, const AttributedText& right, Equal equal) {
  if (!InternalAccess::SameBody(left, right) || !std::ranges::equal(left.InlineObjects(), right.InlineObjects())) {
    return false;
  }
  // Compare effective values over the union of range boundaries, not the way either input was fragmented.
  StyleCursor left_cursor(left.StyleRanges());
  StyleCursor right_cursor(right.StyleRanges());
  TextOffset offset = 0;
  while (offset < left.Length()) {
    if (!equal(left_cursor.At(offset), right_cursor.At(offset))) {
      return false;
    }
    offset = std::min(left_cursor.Next(offset, left.Length()), right_cursor.Next(offset, right.Length()));
  }
  return true;
}

} // namespace

TextStyle ResolveTextStyle(const TextStyle& base, const TextSpanStyle& overrides) {
  return {ResolveFont(base.font, overrides), overrides.foreground.value_or(base.foreground),
          overrides.decoration.value_or(base.decoration)};
}

std::vector<ResolvedTextRun> ResolveTextRuns(const AttributedText& text, const TextStyle& base) {
  std::vector<ResolvedTextRun> runs;
  // Advance byte and UTF-16 positions together; link boundaries do not introduce shaping or drawing runs.
  StyleCursor cursor(text.StyleRanges());
  const auto objects = text.InlineObjects();
  auto object = objects.begin();
  std::size_t byte = 0;
  TextOffset offset = 0;
  while (offset < text.Length()) {
    const TextOffset start = offset;
    const auto& overrides = cursor.At(offset);
    const TextOffset next_object = object == objects.end() ? text.Length() : object->offset;
    const bool is_object = offset == next_object;
    const TextOffset end = is_object ? offset + 1 : std::min(cursor.Next(offset, text.Length()), next_object);
    const std::size_t byte_start = byte;
    while (offset < end) {
      Utf8CodePoint point;
      DecodeCodePoint(text.PlainText(), byte, point);
      byte += point.byte_length;
      offset += point.value > 0xFFFFU ? 2 : 1;
    }
    auto style = ResolveTextStyle(base, overrides);
    const Color background = overrides.background.value_or(Color::Transparent());
    if (is_object) {
      runs.push_back({{start, end}, byte_start, byte, std::move(style), background, *object++});
    } else if (!runs.empty() && !runs.back().inline_object &&
               runs.back().style == style && runs.back().background == background) {
      runs.back().range.end = end;
      runs.back().byte_end = byte;
    } else {
      runs.push_back({{start, end}, byte_start, byte, std::move(style), background, {}});
    }
  }
  return runs;
}

bool TextLayoutInputsEqual(const AttributedText& left, const Font& left_base, const AttributedText& right,
    const Font& right_base) {
  if (left_base == right_base && left == right) {
    return true;
  }
  // Empty paragraphs still derive line height and caret metrics from their base font.
  if (left.Length() == 0 && right.Length() == 0) {
    return left_base == right_base;
  }
  // Recoloring and link-target changes must not trigger paragraph reflow or ancestor measurement.
  return EqualProjections(left, right, [&](const TextSpanStyle& a, const TextSpanStyle& b) {
    return ResolveFont(left_base, a) == ResolveFont(right_base, b);
  });
}

bool TextPaintInputsEqual(const AttributedText& left, const TextStyle& left_base, const AttributedText& right,
    const TextStyle& right_base) {
  if (left_base == right_base && left == right) {
    return true;
  }
  return EqualProjections(left, right, [&](const TextSpanStyle& a, const TextSpanStyle& b) {
    return ResolveTextStyle(left_base, a) == ResolveTextStyle(right_base, b) &&
           a.background.value_or(Color::Transparent()) == b.background.value_or(Color::Transparent());
  });
}

std::shared_ptr<TextLayoutBackend> GetParagraphLayout(MountedNode& node, UiWindow& ui_window) {
#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
  const auto revision = InternalAccess::TextMetricsRevision(ui_window);
#endif
  const float width = node.ContentBounds().width;
  const auto& style = node.properties.text_style;
  auto options = node.properties.text_layout_options;
  options.vertical_align = TextVerticalAlign::Top;
  const auto& previous = node.cached_paragraph_layout;
  if (previous && previous->width == width && previous->options == options &&
#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
      previous->revision == revision &&
#endif
      TextLayoutInputsEqual(previous->text, previous->font, node.text, style.font)) {
    return previous->layout;
  }
  auto next = std::make_shared<CachedParagraphLayout>();
#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
  next->revision = revision;
#endif
  next->text = node.text;
  next->font = style.font;
  next->options = options;
  next->width = width;
  next->layout = ui_window.CreatePlatformTextLayout(node.text, style, width, options);
  node.cached_paragraph_layout = std::move(next);
  return node.cached_paragraph_layout->layout;
}

} // namespace detail

} // namespace huxerui
