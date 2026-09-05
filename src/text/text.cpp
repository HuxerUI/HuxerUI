#include <huxerui/text.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

#include <huxerui/root.h>

#include "runtime/mounted_node_internal.h"
#include "internal_access.h"
#include "graphics/paint_internal.h"
#include "text_internal.h"

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

Font::Font(FontFamilyKind family_kind, std::string family_name, float size)
    : family_kind_(family_kind), family_name_(std::move(family_name)), size_(size) {
  RequireFontSize(size);
  if (family_kind_ == FontFamilyKind::Named && family_name_.empty()) {
    throw std::invalid_argument("HuxerUI named font family must not be empty");
  }
}

Font Font::System(float size) {
  return Font{FontFamilyKind::System, {}, size};
}

Font Font::Monospace(float size) {
  return Font{FontFamilyKind::Monospace, {}, size};
}

Font Font::Named(std::string family, float size) {
  return Font{FontFamilyKind::Named, std::move(family), size};
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
  return *service->measurer;
}

// Attribute replacement shares the immutable body, its UTF-16 index, and its comparison hash.
struct AttributedText::Storage {
  std::shared_ptr<const TextBody> body;
  std::vector<TextStyleRange> styles;
  std::vector<TextLinkRange> links;
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
  static const auto empty = std::make_shared<const Storage>(Storage{std::make_shared<const TextBody>(""), {}, {}});
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
  return AttributedText(std::make_shared<const Storage>(Storage{std::move(body), std::move(styles), std::move(links)}));
}

AttributedText AttributedText::WithStyles(std::vector<TextStyleRange> styles) const {
  styles = NormalizeStyles(*storage_->body, std::move(styles));
  if (styles == storage_->styles) {
    return *this;
  }
  return AttributedText(std::make_shared<const Storage>(Storage{storage_->body, std::move(styles), storage_->links}));
}

const std::string& AttributedText::PlainText() const noexcept { return storage_->body->text; }
TextOffset AttributedText::Length() const noexcept { return storage_->body->length; }
std::span<const TextStyleRange> AttributedText::StyleRanges() const noexcept { return storage_->styles; }
std::span<const TextLinkRange> AttributedText::LinkRanges() const noexcept { return storage_->links; }

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
         storage_->styles == other.storage_->styles && storage_->links == other.storage_->links;
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

std::size_t ParagraphCacheCost(const AttributedText& text) noexcept {
  const auto bytes = text.PlainText().size();
  const auto ranges = text.StyleRanges().size() + text.LinkRanges().size();
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
  if (!InternalAccess::SameBody(left, right)) {
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
  std::size_t byte = 0;
  TextOffset offset = 0;
  while (offset < text.Length()) {
    const TextOffset start = offset;
    const auto& overrides = cursor.At(offset);
    const TextOffset end = cursor.Next(offset, text.Length());
    const std::size_t byte_start = byte;
    while (offset < end) {
      Utf8CodePoint point;
      DecodeCodePoint(text.PlainText(), byte, point);
      byte += point.byte_length;
      offset += point.value > 0xFFFFU ? 2 : 1;
    }
    auto style = ResolveTextStyle(base, overrides);
    const Color background = overrides.background.value_or(Color::Transparent());
    if (!runs.empty() && runs.back().style == style && runs.back().background == background) {
      runs.back().range.end = end;
      runs.back().byte_end = byte;
    } else {
      runs.push_back({{start, end}, byte_start, byte, std::move(style), background});
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

std::shared_ptr<TextLayout> GetParagraphLayout(MountedNode& node, PlatformAdapter& platform) {
  const float width = node.ContentBounds().width;
  const auto& style = node.properties.text_style;
  const auto& options = node.properties.text_layout_options;
  const auto& previous = node.paragraph_layout;
  if (previous && previous->width == width && previous->options == options &&
      TextLayoutInputsEqual(previous->text, previous->font, node.text, style.font)) {
    return previous->layout;
  }
  auto next = std::make_shared<ParagraphLayout>();
  next->text = node.text;
  next->font = style.font;
  next->options = options;
  next->width = width;
  next->layout = platform.CreateTextLayout(node.text, style, width, options);
  node.paragraph_layout = std::move(next);
  return node.paragraph_layout->layout;
}

} // namespace detail

} // namespace huxerui
