#include <huxerui/clipboard.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "application_internal.h"
#include "text/text_input_internal.h"

namespace huxerui::detail {

IoError ClipboardError(IoErrorCode code) {
  const char* message = "HuxerUI could not read clipboard content";
  switch (code) {
  case IoErrorCode::NotFound: message = "HuxerUI clipboard has no matching content"; break;
  case IoErrorCode::Unsupported: message = "HuxerUI clipboard operation is unsupported"; break;
  case IoErrorCode::PermissionDenied: message = "HuxerUI clipboard access was denied"; break;
  case IoErrorCode::TooLarge: message = "HuxerUI clipboard content exceeds the supported size"; break;
  case IoErrorCode::InvalidEncoding: message = "HuxerUI clipboard content has an invalid encoding"; break;
  case IoErrorCode::Timeout: message = "HuxerUI clipboard read timed out"; break;
  default: break;
  }
  return {code, message};
}

IoError ClipboardChangedError() {
  return {IoErrorCode::Io, "HuxerUI clipboard item changed during capture"};
}

IoResult<std::string> ClipboardText(std::string text) {
  if (text.size() > clipboard_max_text_bytes) return ClipboardFailure<std::string>(IoErrorCode::TooLarge);
  if (text.find('\0') != text.npos || !Utf16Length(text)) {
    return ClipboardFailure<std::string>(IoErrorCode::InvalidEncoding);
  }
  return IoResult<std::string>::Success(std::move(text));
}

bool ClipboardImageSizeAllowed(std::size_t width, std::size_t height) noexcept {
  return width != 0 && height != 0 && width <= clipboard_max_image_pixels / height;
}

IoResult<ImageAsset> ClipboardImage(Bytes bytes) {
  if (bytes.size() > clipboard_max_image_bytes) {
    return ClipboardFailure<ImageAsset>(IoErrorCode::TooLarge);
  }
  try {
    ImageAsset image = ImageAsset::FromEncoded(std::move(bytes));
    if (!ClipboardImageSizeAllowed(image.PixelWidth(), image.PixelHeight())) {
      return ClipboardFailure<ImageAsset>(IoErrorCode::TooLarge);
    }
    return IoResult<ImageAsset>::Success(std::move(image));
  } catch (const std::invalid_argument&) {
    return ClipboardFailure<ImageAsset>(IoErrorCode::InvalidEncoding);
  }
}

std::optional<std::vector<File>> ClipboardFilePaths(const ClipboardFiles& content) {
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

std::string ClipboardTypeIdentifier(std::string_view mime) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result = "org.huxerui.clipboard.mime.";
  for (const auto value : mime) {
    const auto byte = static_cast<unsigned char>(value);
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

void ClipboardItemRead::SetText(std::optional<std::string> ClipboardItem::* field, IoResult<std::string> result) {
  if (result.Succeeded()) result = ClipboardText(std::move(result).Value());
  if (result.Succeeded()) item_.*field = std::move(result).Value();
  else RecordError(std::move(result).Error());
}

void ClipboardItemRead::SetImage(IoResult<ImageAsset> result) {
  if (result.Succeeded()) item_.image.emplace(std::move(result).Value());
  else RecordError(std::move(result).Error());
}

void ClipboardItemRead::SetCustom(std::string_view mime, IoResult<Bytes> result) {
  if (!result.Succeeded()) {
    RecordError(std::move(result).Error());
    return;
  }
  const auto size = result.Value().size();
  if (size > remaining_custom_bytes_) {
    RecordError(ClipboardError(IoErrorCode::TooLarge));
    return;
  }
  remaining_custom_bytes_ -= size;
  item_.custom_formats.emplace(mime, std::move(result).Value());
}

void ClipboardItemRead::RecordError(IoError error) {
  if (!error_ && error.code != IoErrorCode::NotFound) error_ = std::move(error);
}

IoResult<ClipboardItem> ClipboardItemRead::Finish() && {
  if (item_.text || item_.html || item_.image || !item_.custom_formats.empty()) {
    return IoResult<ClipboardItem>::Success(std::move(item_));
  }
  return IoResult<ClipboardItem>::Failure(error_ ? std::move(*error_) : ClipboardError(IoErrorCode::NotFound));
}

Task<IoResult<ClipboardItem>> FinishClipboardItemRead(ClipboardItemRead item,
                                                      std::optional<Task<IoResult<ImageAsset>>> image) {
  if (image) item.SetImage(co_await std::move(*image));
  co_return std::move(item).Finish();
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

IoResult<AttributedText> ReadClipboardHtml(std::string_view fragment) {
  if (fragment.size() > clipboard_max_text_bytes) return ClipboardFailure<AttributedText>(IoErrorCode::TooLarge);
  if (fragment.find('\0') != fragment.npos || !Utf16Length(fragment)) {
    return ClipboardFailure<AttributedText>(IoErrorCode::InvalidEncoding);
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
      if (end == fragment.npos) return ClipboardFailure<AttributedText>(IoErrorCode::InvalidEncoding);
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
    return IoResult<AttributedText>::Success(std::move(result).Finish());
  } catch (const std::length_error&) {
    return ClipboardFailure<AttributedText>(IoErrorCode::TooLarge);
  } catch (const std::invalid_argument&) {
    return ClipboardFailure<AttributedText>(IoErrorCode::InvalidEncoding);
  }
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

std::string NormalizeClipboardMime(std::string_view mime) {
  constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
  const auto slash = mime.find('/');
  const auto token = [&](std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [&](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             (c != '*' && punctuation.find(c) != punctuation.npos);
    });
  };
  if (mime.size() > 127 || slash == mime.npos || !token(mime.substr(0, slash)) || !token(mime.substr(slash + 1))) {
    throw std::invalid_argument("HuxerUI clipboard custom format must be a parameter-free MIME type");
  }
  auto normalized = detail::Lower(mime);
  constexpr std::array<std::string_view, 9> reserved{
      "text/plain", "text/html", "image/png", "image/jpeg", "image/tiff", "text/uri-list",
      "x-special/gnome-copied-files", "application/x-kde-cutselection", "application/vnd.portal.filetransfer"};
  if (std::ranges::find(reserved, normalized) != reserved.end()) {
    throw std::invalid_argument("HuxerUI clipboard custom format conflicts with a built-in representation");
  }
  return normalized;
}

bool ValidateClipboardText(std::string_view text) {
  if (text.size() > detail::clipboard_max_text_bytes) return false;
  if (text.find('\0') != text.npos || !detail::Utf16Length(text)) {
    throw std::invalid_argument("HuxerUI clipboard text must contain valid UTF-8 without null characters");
  }
  return true;
}

bool PrepareClipboardItem(ClipboardItem& item) {
  if (!item.text && !item.html && !item.image && item.custom_formats.empty()) {
    throw std::invalid_argument("HuxerUI clipboard item must contain a representation");
  }
  if ((item.text && !ValidateClipboardText(*item.text)) || (item.html && !ValidateClipboardText(*item.html))) {
    return false;
  }
  if (item.image) {
    if (item.image->EncodedBytes().empty()) {
      throw std::invalid_argument("HuxerUI clipboard image must contain encoded pixels");
    }
    if (item.image->EncodedBytes().size() > detail::clipboard_max_image_bytes ||
        !detail::ClipboardImageSizeAllowed(item.image->PixelWidth(), item.image->PixelHeight())) return false;
  }
  if (item.custom_formats.size() > detail::clipboard_max_custom_formats) return false;
  decltype(item.custom_formats) normalized;
  auto remaining = detail::clipboard_max_custom_bytes;
  for (auto& [mime, bytes] : item.custom_formats) {
    if (bytes.size() > remaining) return false;
    remaining -= bytes.size();
    if (!normalized.emplace(NormalizeClipboardMime(mime), std::move(bytes)).second) {
      throw std::invalid_argument("HuxerUI clipboard custom formats must be unique");
    }
  }
  item.custom_formats = std::move(normalized);
  return true;
}

void NormalizeClipboardReadOptions(ClipboardReadOptions& options) {
  if ((!options.text && !options.html && !options.image && options.custom_mime_types.empty()) ||
      options.custom_mime_types.size() > detail::clipboard_max_custom_formats) {
    throw std::invalid_argument("HuxerUI clipboard read must request a supported number of representations");
  }
  for (auto current = options.custom_mime_types.begin(); current != options.custom_mime_types.end(); ++current) {
    *current = NormalizeClipboardMime(*current);
    if (std::find(options.custom_mime_types.begin(), current, *current) != current) {
      throw std::invalid_argument("HuxerUI clipboard requested formats must be unique");
    }
  }
}

bool ValidateClipboardFiles(std::size_t size) {
  if (size == 0) throw std::invalid_argument("HuxerUI clipboard file list must not be empty");
  return size <= detail::clipboard_max_files;
}

template <class T>
Task<IoResult<T>> ReadWhileConnected(std::weak_ptr<const void> connection, Task<IoResult<T>> operation) {
  if (connection.expired()) co_return detail::ClipboardFailure<T>(IoErrorCode::Unsupported);
  auto result = co_await std::move(operation);
  if (connection.expired()) co_return detail::ClipboardFailure<T>(IoErrorCode::Unsupported);
  co_return std::move(result);
}

template <class T>
Task<IoResult<T>> ReadItemField(Task<IoResult<ClipboardItem>> operation, std::optional<T> ClipboardItem::* field) {
  auto result = co_await std::move(operation);
  if (!result.Succeeded()) co_return IoResult<T>::Failure(std::move(result).Error());
  auto& value = result.Value().*field;
  if (!value) co_return detail::ClipboardFailure<T>(IoErrorCode::NotFound);
  co_return IoResult<T>::Success(std::move(*value));
}

} // namespace

ClipboardItem ClipboardItem::FromAttributedText(const AttributedText& text) {
  if (!ValidateClipboardText(text.PlainText())) {
    throw std::length_error("HuxerUI clipboard text exceeds the supported size");
  }
  auto html = detail::ClipboardHtml(text);
  if (html.empty() && !text.PlainText().empty()) {
    throw std::length_error("HuxerUI clipboard HTML exceeds the supported size");
  }
  return {.text = text.PlainText(), .html = std::move(html), .image = {}, .custom_formats = {}};
}

IoResult<AttributedText> ClipboardItem::ToAttributedText() const {
  auto result = html ? detail::ReadClipboardHtml(*html)
                     : detail::ClipboardFailure<AttributedText>(IoErrorCode::NotFound);
  if (result.Succeeded() || !text) return result;
  auto plain = detail::ClipboardText(*text);
  if (!plain.Succeeded()) return IoResult<AttributedText>::Failure(std::move(plain).Error());
  return IoResult<AttributedText>::Success(AttributedText::FromRanges(std::move(plain).Value(), {}));
}

bool PlatformClipboard::WriteItem(ClipboardItem item) {
  return item.text && !item.html && !item.image && item.custom_formats.empty() && WriteText(*item.text);
}

Task<bool> PlatformClipboard::WriteItemAsync(ClipboardItem item) {
  return detail::ClipboardResult(WriteItem(std::move(item)));
}

Task<IoResult<ClipboardItem>> PlatformClipboard::ReadItemAsync(ClipboardReadOptions options) {
  if (!SupportsSynchronousText() || !options.text) {
    return detail::ClipboardResult(detail::ClipboardFailure(IoErrorCode::Unsupported));
  }
  detail::ClipboardItemRead item;
  if (auto text = ReadText()) item.SetText(&ClipboardItem::text, IoResult<std::string>::Success(std::move(*text)));
  return detail::ClipboardResult(std::move(item).Finish());
}

bool PlatformClipboard::WriteFiles(detail::ClipboardFiles) {
  return false;
}

Task<IoResult<std::vector<FileReference>>> PlatformClipboard::ReadFilesAsync() {
  return detail::ClipboardResult(detail::ClipboardFailure<std::vector<FileReference>>(IoErrorCode::Unsupported));
}

Clipboard::Clipboard(PlatformClipboard* platform)
    : connection_(platform ? std::make_shared<Connection>(Connection{platform}) : nullptr) {}

bool Clipboard::SupportsSynchronousText() const noexcept {
  return connection_ && connection_->platform->SupportsSynchronousText();
}

bool Clipboard::WriteText(std::string_view text) const {
  return ValidateClipboardText(text) && SupportsSynchronousText() && connection_->platform->WriteText(text);
}

std::optional<std::string> Clipboard::ReadText() const {
  if (!SupportsSynchronousText()) return std::nullopt;
  auto text = connection_->platform->ReadText();
  if (!text) return std::nullopt;
  auto result = detail::ClipboardText(std::move(*text));
  if (!result.Succeeded()) return std::nullopt;
  return std::move(result).Value();
}

Task<IoResult<std::string>> Clipboard::ReadTextAsync() const {
  return ReadItemField(ReadItemAsync({.html = false, .custom_mime_types = {}}), &ClipboardItem::text);
}

bool Clipboard::WriteItem(ClipboardItem item) const {
  return PrepareClipboardItem(item) && connection_ && connection_->platform->WriteItem(std::move(item));
}

Task<bool> Clipboard::WriteItemAsync(ClipboardItem item) const {
  if (!PrepareClipboardItem(item) || !connection_) return detail::ClipboardResult(false);
  return connection_->platform->WriteItemAsync(std::move(item));
}

Task<IoResult<ClipboardItem>> Clipboard::ReadItemAsync(ClipboardReadOptions options) const {
  NormalizeClipboardReadOptions(options);
  auto operation =
      connection_ ? connection_->platform->ReadItemAsync(std::move(options)) : Task<IoResult<ClipboardItem>>{};
  return ReadWhileConnected(connection_, std::move(operation));
}

Task<IoResult<ImageAsset>> Clipboard::ReadImageAsync() const {
  return ReadItemField(ReadItemAsync({.text = false, .html = false, .image = true, .custom_mime_types = {}}),
                       &ClipboardItem::image);
}

bool Clipboard::WriteFiles(std::vector<File> files) const {
  return ValidateClipboardFiles(files.size()) && connection_ && connection_->platform->WriteFiles(std::move(files));
}

bool Clipboard::WriteFiles(std::vector<FileReference> files) const {
  return ValidateClipboardFiles(files.size()) && connection_ && connection_->platform->WriteFiles(std::move(files));
}

Task<IoResult<std::vector<FileReference>>> Clipboard::ReadFilesAsync() const {
  auto operation = connection_ ? connection_->platform->ReadFilesAsync() : Task<IoResult<std::vector<FileReference>>>{};
  return ReadWhileConnected(connection_, std::move(operation));
}

} // namespace huxerui
