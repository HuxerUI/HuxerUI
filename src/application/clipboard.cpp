#include <huxerui/clipboard.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "application_internal.h"
#include "text/text_input_internal.h"

namespace huxerui::detail {

IoResult<ClipboardContent> ClipboardFailure(IoErrorCode code) {
  const char* message = "HuxerUI could not read clipboard content";
  switch (code) {
  case IoErrorCode::NotFound: message = "HuxerUI clipboard has no matching content"; break;
  case IoErrorCode::Unsupported: message = "HuxerUI clipboard operation is unsupported"; break;
  case IoErrorCode::PermissionDenied: message = "HuxerUI clipboard access was denied"; break;
  case IoErrorCode::TooLarge: message = "HuxerUI clipboard content exceeds the supported size"; break;
  case IoErrorCode::InvalidEncoding: message = "HuxerUI clipboard content has an invalid encoding"; break;
  default: break;
  }
  return IoResult<ClipboardContent>::Failure({code, message});
}

bool ClipboardImageSizeAllowed(std::size_t width, std::size_t height) noexcept {
  return width != 0 && height != 0 && width <= clipboard_max_image_pixels / height;
}

IoResult<ClipboardContent> ClipboardImage(Bytes bytes) {
  if (bytes.size() > clipboard_max_image_bytes) {
    return ClipboardFailure(IoErrorCode::TooLarge);
  }
  try {
    ImageAsset image = ImageAsset::FromEncoded(std::move(bytes));
    if (!ClipboardImageSizeAllowed(image.PixelWidth(), image.PixelHeight())) {
      return ClipboardFailure(IoErrorCode::TooLarge);
    }
    return IoResult<ClipboardContent>::Success(ClipboardContent{std::move(image)});
  } catch (const std::invalid_argument&) {
    return ClipboardFailure(IoErrorCode::InvalidEncoding);
  }
}

std::optional<std::vector<File>> ClipboardFilePaths(const ClipboardContent& content) {
  if (const auto* files = std::get_if<std::vector<File>>(&content)) {
    return *files;
  }
  std::vector<File> paths;
  for (const FileReference& reference : std::get<std::vector<FileReference>>(content)) {
    auto path = reference.AsFile();
    if (!path) {
      return std::nullopt;
    }
    paths.push_back(std::move(*path));
  }
  return paths;
}

namespace {

constexpr std::string_view whitespace = " \t\r\n\f";
constexpr std::size_t max_nesting = 128;
constexpr std::size_t max_ranges = 65536;

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(whitespace);
  return first == text.npos ? std::string_view{} : text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

char LowerAscii(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

std::string Lower(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), LowerAscii);
  return result;
}

void AppendEscaped(std::string& output, std::string_view text, bool attribute = false) {
  for (const char value : text) {
    switch (value) {
    case '&': output += "&amp;"; break;
    case '<': output += "&lt;"; break;
    case '>': output += "&gt;"; break;
    case '"': output += "&quot;"; break;
    case '\n': output += attribute ? "&#10;" : "<br>"; break;
    case '\r': output += "&#13;"; break;
    case '\t': output += "&#9;"; break;
    case ' ': output += attribute ? " " : "&#32;"; break;
    default: output.push_back(value); break;
    }
  }
}

std::optional<std::uint32_t> Entity(std::string_view value) {
  if (value == "amp") return '&';
  if (value == "lt") return '<';
  if (value == "gt") return '>';
  if (value == "quot") return '"';
  if (value == "apos") return '\'';
  if (value == "nbsp") return 0xA0;
  if (!value.starts_with('#')) return std::nullopt;
  value.remove_prefix(1);
  const bool hex = value.starts_with('x') || value.starts_with('X');
  if (hex) value.remove_prefix(1);
  std::uint32_t code = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), code, hex ? 16 : 10);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || code == 0 ||
      code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
    return std::nullopt;
  }
  return code;
}

void AppendScalar(std::string& text, std::uint32_t code) {
  if (code < 0x80) {
    text.push_back(static_cast<char>(code));
    return;
  }
  const unsigned continuation_count = code < 0x800 ? 1 : code < 0x10000 ? 2 : 3;
  const unsigned prefix = continuation_count == 1 ? 0xC0 : continuation_count == 2 ? 0xE0 : 0xF0;
  text.push_back(static_cast<char>(prefix | (code >> (continuation_count * 6))));
  for (unsigned index = continuation_count; index != 0; --index) {
    text.push_back(static_cast<char>(0x80 | ((code >> ((index - 1) * 6)) & 0x3F)));
  }
}

std::string Decode(std::string_view text, bool collapse_spaces = false) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char value = text[index];
    if (value == '&') {
      const auto entity = text.substr(index + 1, 16);
      const auto end = entity.find(';');
      if (end != entity.npos) {
        if (const auto code = Entity(entity.substr(0, end))) {
          AppendScalar(result, *code);
          index += end + 1;
          continue;
        }
      }
    }
    if (collapse_spaces && whitespace.find(value) != whitespace.npos) {
      if (index == 0 || whitespace.find(text[index - 1]) == whitespace.npos) result.push_back(' ');
    } else {
      result.push_back(value);
    }
  }
  return result;
}

std::optional<Uri> ClipboardLink(std::string_view url) {
  auto target = Uri::Parse(Trim(url));
  if (target) {
    const auto scheme = Lower(target->Scheme());
    if (scheme == "https" || scheme == "http" || scheme == "mailto") return target;
  }
  return std::nullopt;
}

std::optional<Color> CssColor(std::string_view value) {
  if (value.starts_with('#') && (value.size() == 4 || value.size() == 7 || value.size() == 9)) {
    std::uint32_t color = 0;
    const auto parsed = std::from_chars(value.data() + 1, value.data() + value.size(), color, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    if (value.size() == 4) {
      return Color::Rgb(((color >> 8) & 15) * 17, ((color >> 4) & 15) * 17, (color & 15) * 17);
    }
    return Color::Rgba32(value.size() == 7 ? (color << 8) | 255 : color);
  }
  if (value.starts_with("rgb(") && value.ends_with(')')) {
    value = value.substr(4, value.size() - 5);
    int channels[3]{};
    for (int index = 0; index < 3; ++index) {
      const auto comma = value.find(',');
      if ((comma == value.npos) != (index == 2)) return std::nullopt;
      const auto number = Trim(value.substr(0, comma));
      if (number.empty()) return std::nullopt;
      const auto parsed = std::from_chars(number.data(), number.data() + number.size(), channels[index]);
      if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
          channels[index] < 0 || channels[index] > 255) return std::nullopt;
      if (comma != value.npos) value.remove_prefix(comma + 1);
    }
    return Color::Rgb(channels[0], channels[1], channels[2]);
  }
  if (value == "black") return Color::Black();
  if (value == "white") return Color::White();
  if (value == "transparent") return Color::Transparent();
  return std::nullopt;
}

void ApplyCss(TextSpanStyle& style, std::string_view properties) {
  while (!properties.empty()) {
    const auto next = properties.find(';');
    const auto property = properties.substr(0, next);
    properties.remove_prefix(next == properties.npos ? properties.size() : next + 1);
    const auto colon = property.find(':');
    if (colon == property.npos) continue;
    const auto name = Lower(Trim(property.substr(0, colon)));
    const auto value = Lower(Trim(property.substr(colon + 1)));
    if (name == "color" || name == "background-color") {
      if (auto color = CssColor(value)) {
        (name == "color" ? style.foreground : style.background) = *color;
      }
    } else if (name == "font-weight") {
      int weight = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), weight);
      if (value == "bold") style.font_weight = FontWeight::Bold;
      else if (value == "normal") style.font_weight = FontWeight::Regular;
      else if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
               weight >= 100 && weight <= 900 && weight % 100 == 0) {
        style.font_weight = static_cast<FontWeight>(weight);
      }
    } else if (name == "font-style") {
      if (value == "normal") style.font_slant = FontSlant::Normal;
      if (value == "italic" || value == "oblique") style.font_slant = FontSlant::Italic;
    } else if (name == "text-decoration" || name == "text-decoration-line") {
      auto decoration = TextDecoration::None;
      if (value.find("underline") != value.npos) decoration = decoration | TextDecoration::Underline;
      if (value.find("line-through") != value.npos) decoration = decoration | TextDecoration::StrikeThrough;
      style.decoration = decoration;
    } else if (name == "font-size" && (value.ends_with("px") || value.ends_with("pt"))) {
      const auto number = std::string_view(value).substr(0, value.size() - 2);
      if (number.starts_with('+') || number.starts_with("0x")) continue;
      std::istringstream input{std::string(number)};
      input.imbue(std::locale::classic());
      // Float extraction can report underflow for representable subnormal sizes.
      double parsed_size = 0.0;
      if (!(input >> std::noskipws >> parsed_size) || !input.eof() ||
          !(parsed_size > 0.0 && parsed_size <= std::numeric_limits<float>::max())) {
        continue;
      }
      const auto size = static_cast<float>(parsed_size);
      if (size > 0.0F && size <= 1000.0F) {
        style.font_size = value.ends_with("pt") ? size * (4.0F / 3.0F) : size;
      }
    }
  }
}

std::string CssColorValue(Color color) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string value{"#"};
  for (float channel : {color.red, color.green, color.blue, color.alpha}) {
    const unsigned byte = static_cast<unsigned>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
    value.push_back(digits[byte >> 4]);
    value.push_back(digits[byte & 15]);
  }
  if (value.ends_with("ff")) value.resize(7);
  return value;
}

std::string InlineCss(const TextSpanStyle& style) {
  std::string css;
  const auto size = style.font_size.value_or(style.font ? style.font->Size() : 0.0F);
  if (size > 0.0F) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "font-size:" << std::fixed << size << "px;";
    css += output.str();
  }
  if (style.font_weight || style.font) {
    const auto weight = style.font_weight.value_or(style.font ? style.font->Weight() : FontWeight::Regular);
    css += "font-weight:" + std::to_string(static_cast<int>(weight)) + ";";
  }
  if (style.font_slant || style.font) {
    const auto slant = style.font_slant.value_or(style.font ? style.font->Slant() : FontSlant::Normal);
    css += slant == FontSlant::Italic ? "font-style:italic;" : "font-style:normal;";
  }
  if (style.foreground) css += "color:" + CssColorValue(*style.foreground) + ";";
  if (style.background) css += "background-color:" + CssColorValue(*style.background) + ";";
  if (style.decoration) {
    css += "text-decoration:";
    if (*style.decoration == TextDecoration::None) css += "none";
    if (HasTextDecoration(*style.decoration, TextDecoration::Underline)) css += "underline ";
    if (HasTextDecoration(*style.decoration, TextDecoration::StrikeThrough)) css += "line-through";
    css += ';';
  }
  return css;
}

std::size_t TagEnd(std::string_view fragment) {
  char quote = '\0';
  for (std::size_t index = 0; index < fragment.size(); ++index) {
    const char value = fragment[index];
    if (quote != '\0') {
      if (value == quote) quote = '\0';
    } else if (value == '"' || value == '\'') {
      quote = value;
    } else if (value == '>') {
      return index;
    }
  }
  return fragment.npos;
}

struct HtmlContext {
  std::string tag;
  TextSpanStyle style;
  std::optional<Uri> link;
};

void ApplyAttributes(HtmlContext& context, std::string_view attributes) {
  while (!(attributes = Trim(attributes)).empty()) {
    const auto end = attributes.find_first_of(" \t\r\n\f=");
    const auto name = Lower(attributes.substr(0, end));
    attributes.remove_prefix(end == attributes.npos ? attributes.size() : end);
    attributes = Trim(attributes);
    if (!attributes.starts_with('=')) continue;
    attributes = Trim(attributes.substr(1));
    if (attributes.empty()) break;
    const char quote = attributes.front();
    const bool quoted = quote == '\'' || quote == '"';
    if (quoted) attributes.remove_prefix(1);
    const auto value_end = quoted ? attributes.find(quote) : attributes.find_first_of(whitespace);
    const auto value = attributes.substr(0, value_end);
    attributes.remove_prefix(value.size() + (quoted && value_end != attributes.npos ? 1 : 0));
    if (name == "style") ApplyCss(context.style, Decode(value));
    if (name == "href" && context.tag == "a") context.link = ClipboardLink(Decode(value));
    if (name == "color" && context.tag == "font") {
      if (auto color = CssColor(Lower(value))) context.style.foreground = *color;
    }
  }
}

bool IsBlock(std::string_view tag) {
  return tag == "p" || tag == "div" || tag == "li" || tag == "blockquote" || tag == "pre" ||
      tag == "tr" || (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6');
}

class ClipboardFragment {
public:
  void AddText(std::string text) {
    if (text.empty()) return;
    if (pending_break_ && Trim(text).empty()) return;
    if (pending_break_ && !text_.empty() && text_.back() != '\n') {
      text_ += '\n';
      ++offset_;
    }
    pending_break_ = false;
    Append(text);
  }

  void AddTag(std::string_view contents) {
    const bool closing = contents.starts_with('/');
    if (closing) contents.remove_prefix(1);
    const auto end = contents.find_first_of(" \t\r\n\f/");
    const auto tag = Lower(contents.substr(0, end));
    if (closing) {
      const auto found = std::find_if(contexts_.rbegin(), contexts_.rend() - 1,
                                     [&](const HtmlContext& item) { return item.tag == tag; });
      if (found != contexts_.rend() - 1) contexts_.erase(std::prev(found.base()), contexts_.end());
    }
    if (tag == "br") {
      pending_break_ = false;
      Append("\n");
      return;
    }
    if (IsBlock(tag)) pending_break_ = !text_.empty();
    if (closing || tag.empty() || contents.ends_with('/') || tag == "img" || tag == "meta" ||
        tag == "hr" || tag == "link" || tag == "input" || tag == "wbr") return;
    if (contexts_.size() >= max_nesting) throw std::length_error("HuxerUI clipboard HTML nesting is too deep");
    HtmlContext next = contexts_.back();
    next.tag = tag;
    if (tag == "b" || tag == "strong") next.style.font_weight = FontWeight::Bold;
    if (tag == "i" || tag == "em") next.style.font_slant = FontSlant::Italic;
    if (tag == "u") {
      next.style.decoration = next.style.decoration.value_or(TextDecoration::None) | TextDecoration::Underline;
    }
    if (tag == "s" || tag == "strike" || tag == "del") {
      next.style.decoration = next.style.decoration.value_or(TextDecoration::None) | TextDecoration::StrikeThrough;
    }
    if (tag == "a") next.link.reset();
    if (end != contents.npos) ApplyAttributes(next, contents.substr(end));
    contexts_.push_back(std::move(next));
  }

  AttributedText Finish() && {
    return AttributedText::FromRanges(std::move(text_), std::move(styles_), std::move(links_));
  }

private:
  void Append(std::string_view text) {
    const auto length = Utf16Length(text);
    if (!length) throw std::invalid_argument("HuxerUI clipboard HTML contains invalid UTF-8");
    const TextRange range{offset_, offset_ + *length};
    const auto& context = contexts_.back();
    if (context.style != TextSpanStyle{}) {
      if (!styles_.empty() && styles_.back().range.end == range.start && styles_.back().style == context.style) {
        styles_.back().range.end = range.end;
      } else {
        styles_.push_back({range, context.style});
      }
    }
    if (context.link) {
      if (!links_.empty() && links_.back().range.end == range.start && links_.back().target == *context.link) {
        links_.back().range.end = range.end;
      } else {
        links_.push_back({range, *context.link});
      }
    }
    if (styles_.size() + links_.size() > max_ranges) {
      throw std::length_error("HuxerUI clipboard HTML has too many styled ranges");
    }
    text_ += text;
    offset_ = range.end;
  }

  std::vector<HtmlContext> contexts_{HtmlContext{}};
  std::string text_;
  std::vector<TextStyleRange> styles_;
  std::vector<TextLinkRange> links_;
  TextOffset offset_ = 0;
  bool pending_break_ = false;
};

} // namespace

std::string ClipboardHtml(const AttributedText& text) {
  std::string fragment;
  const auto styles = text.StyleRanges();
  const auto links = text.LinkRanges();
  std::size_t style_index = 0;
  std::size_t link_index = 0;
  for (TextOffset start = 0; start < text.Length();) {
    while (style_index < styles.size() && styles[style_index].range.end <= start) ++style_index;
    while (link_index < links.size() && links[link_index].range.end <= start) ++link_index;
    TextOffset end = text.Length();
    std::string css;
    std::optional<Uri> link;
    if (style_index < styles.size()) {
      const auto& style = styles[style_index];
      if (style.range.start <= start) css = InlineCss(style.style);
      end = std::min(end, style.range.start <= start ? style.range.end : style.range.start);
    }
    if (link_index < links.size()) {
      const auto& range = links[link_index];
      if (range.range.start <= start) link = ClipboardLink(range.target.ToString());
      end = std::min(end, range.range.start <= start ? range.range.end : range.range.start);
    }
    if (link) {
      fragment += "<a href=\"";
      AppendEscaped(fragment, link->ToString(), true);
      fragment += "\">";
    }
    if (!css.empty()) fragment += "<span style=\"" + css + "\">";
    AppendEscaped(fragment, text.TextInRange({start, end}));
    if (!css.empty()) fragment += "</span>";
    if (link) fragment += "</a>";
    if (fragment.size() > clipboard_max_text_bytes) return {};
    start = end;
  }
  return fragment;
}

IoResult<ClipboardContent> ReadClipboardHtml(std::string_view fragment) {
  if (fragment.size() > clipboard_max_text_bytes) return ClipboardFailure(IoErrorCode::TooLarge);
  if (fragment.find('\0') != fragment.npos || !Utf16Length(fragment)) {
    return ClipboardFailure(IoErrorCode::InvalidEncoding);
  }
  ClipboardFragment result;
  try {
    while (!fragment.empty()) {
      const auto tag_begin = fragment.find('<');
      result.AddText(Decode(fragment.substr(0, tag_begin), true));
      if (tag_begin == fragment.npos) break;
      fragment.remove_prefix(tag_begin);
      if (fragment.starts_with("<!--")) {
        const auto end = fragment.find("-->", 4);
        if (end == fragment.npos) break;
        fragment.remove_prefix(end + 3);
        continue;
      }
      const auto end = TagEnd(fragment);
      if (end == fragment.npos) return ClipboardFailure(IoErrorCode::InvalidEncoding);
      const auto contents = Trim(fragment.substr(1, end - 1));
      fragment.remove_prefix(end + 1);
      if (contents.empty() || contents.starts_with('!') || contents.starts_with('?')) continue;
      const auto name = Lower(contents.substr(0, contents.find_first_of(" \t\r\n\f")));
      if (name == "script" || name == "style" || name == "head" || name == "template") {
        const std::string closing = "</" + name;
        const auto match = std::search(fragment.begin(), fragment.end(), closing.begin(), closing.end(),
                                       [](char actual, char expected) { return LowerAscii(actual) == expected; });
        if (match == fragment.end()) break;
        fragment.remove_prefix(static_cast<std::size_t>(match - fragment.begin()));
      } else {
        result.AddTag(contents);
      }
    }
    return IoResult<ClipboardContent>::Success(ClipboardContent{std::move(result).Finish()});
  } catch (const std::length_error&) {
    return ClipboardFailure(IoErrorCode::TooLarge);
  } catch (const std::invalid_argument&) {
    return ClipboardFailure(IoErrorCode::InvalidEncoding);
  }
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

template <class T> Task<IoResult<T>> ReadAs(std::weak_ptr<const void> connection,
                                         Task<IoResult<ClipboardContent>> operation) {
  if (connection.expired()) {
    co_return IoResult<T>::Failure({IoErrorCode::Unsupported, "HuxerUI clipboard service is unavailable"});
  }
  auto result = co_await std::move(operation);
  if (connection.expired()) {
    co_return IoResult<T>::Failure({IoErrorCode::Unsupported, "HuxerUI clipboard service is unavailable"});
  }
  if (!result.Succeeded()) {
    co_return IoResult<T>::Failure(std::move(result).Error());
  }
  auto& content = result.Value();
  if (auto* plain = std::get_if<std::string>(&content)) {
    if (plain->size() > detail::clipboard_max_text_bytes) {
      co_return IoResult<T>::Failure({IoErrorCode::TooLarge, "HuxerUI clipboard text exceeds the supported size"});
    }
    if (!detail::Utf16Length(*plain)) {
      co_return IoResult<T>::Failure({IoErrorCode::InvalidEncoding, "HuxerUI clipboard text is not valid UTF-8"});
    }
    if constexpr (std::is_same_v<T, AttributedText>) {
      co_return IoResult<T>::Success(AttributedText::FromRanges(std::move(*plain), {}));
    }
  }
  if (auto* value = std::get_if<T>(&content)) {
    co_return IoResult<T>::Success(std::move(*value));
  }
  co_return IoResult<T>::Failure({IoErrorCode::Unsupported, "HuxerUI clipboard did not return the requested content"});
}

Task<IoResult<ClipboardContent>> UnsupportedRead() {
  return detail::ClipboardResult(detail::ClipboardFailure(IoErrorCode::Unsupported));
}

void ValidateClipboardText(std::string_view text) {
  if (!detail::Utf16Length(text)) {
    throw std::invalid_argument("HuxerUI clipboard text must contain valid UTF-8");
  }
}

bool IsPublishableClipboardContent(const ClipboardContent& content) {
  return std::visit([](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, std::string>) {
      ValidateClipboardText(value);
      return value.size() <= detail::clipboard_max_text_bytes && value.find('\0') == std::string::npos;
    } else if constexpr (std::is_same_v<T, AttributedText>) {
      return value.PlainText().size() <= detail::clipboard_max_text_bytes &&
          value.PlainText().find('\0') == std::string::npos;
    } else if constexpr (std::is_same_v<T, ImageAsset>) {
      if (value.EncodedBytes().empty()) {
        throw std::invalid_argument("HuxerUI clipboard image must contain encoded pixels");
      }
      return value.EncodedBytes().size() <= detail::clipboard_max_image_bytes &&
          detail::ClipboardImageSizeAllowed(value.PixelWidth(), value.PixelHeight());
    } else {
      if (value.empty()) {
        throw std::invalid_argument("HuxerUI clipboard file list must not be empty");
      }
      return value.size() <= detail::clipboard_max_files;
    }
  }, content);
}

} // namespace

bool PlatformClipboard::WriteContent(ClipboardContent content) {
  const auto* text = std::get_if<std::string>(&content);
  return text && WriteText(*text);
}

Task<bool> PlatformClipboard::WriteContentAsync(ClipboardContent content) {
  return detail::ClipboardResult(WriteContent(std::move(content)));
}

Task<IoResult<ClipboardContent>> PlatformClipboard::ReadContentAsync(detail::ClipboardKind) {
  return UnsupportedRead();
}

bool PlatformClipboard::MayHaveContent(detail::ClipboardKind kind) const noexcept {
  return kind == detail::ClipboardKind::Text;
}

Clipboard::Clipboard(PlatformClipboard* platform)
    : connection_(platform ? std::make_shared<Connection>(Connection{platform}) : nullptr) {}

bool Clipboard::IsAvailable() const noexcept {
  return connection_ && connection_->platform->SupportsSynchronousText();
}

bool Clipboard::WriteContent(ClipboardContent content) const {
  return IsPublishableClipboardContent(content) && connection_ &&
         connection_->platform->WriteContent(std::move(content));
}

Task<bool> Clipboard::WriteContentAsync(ClipboardContent content) const {
  if (!IsPublishableClipboardContent(content) || !connection_) {
    return detail::ClipboardResult(false);
  }
  return connection_->platform->WriteContentAsync(std::move(content));
}

std::optional<std::string> Clipboard::ReadText() const {
  if (!IsAvailable()) {
    return std::nullopt;
  }
  std::optional<std::string> text = connection_->platform->ReadText();
  if (text.has_value() && !detail::Utf16Length(*text).has_value()) {
    return std::nullopt;
  }
  return text;
}

bool Clipboard::WriteText(std::string_view text) const {
  ValidateClipboardText(text);
  return IsAvailable() && connection_->platform->WriteText(text);
}

template <class T> Task<IoResult<T>> Clipboard::Read(detail::ClipboardKind kind) const {
  auto operation = connection_ ? connection_->platform->ReadContentAsync(kind) : Task<IoResult<ClipboardContent>>{};
  return ReadAs<T>(connection_, std::move(operation));
}

Task<IoResult<std::string>> Clipboard::ReadTextAsync() const {
  return Read<std::string>(detail::ClipboardKind::Text);
}

Task<IoResult<AttributedText>> Clipboard::ReadRichTextAsync() const {
  return Read<AttributedText>(detail::ClipboardKind::RichText);
}

Task<IoResult<ImageAsset>> Clipboard::ReadImageAsync() const {
  return Read<ImageAsset>(detail::ClipboardKind::Image);
}

Task<IoResult<std::vector<FileReference>>> Clipboard::ReadFilesAsync() const {
  return Read<std::vector<FileReference>>(detail::ClipboardKind::Files);
}

} // namespace huxerui
