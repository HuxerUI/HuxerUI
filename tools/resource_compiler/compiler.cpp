#include "compiler.h"
#include "svg_compiler.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "resource_format.h"

namespace huxerui::resource_compiler {

namespace {

namespace resource_format = ::huxerui::detail::resource_format;

using EntryKind = resource_format::EntryKind;

struct Entry {
  EntryKind kind = EntryKind::Raw;
  std::string key;
  std::string package_path{};
  std::string mime_type;
  std::string locale;
  std::string value;
  float scale = 1.0F;
  std::uint32_t pixel_width = 0;
  std::uint32_t pixel_height = 0;
  std::uint64_t content_hash = 0;
  std::uint32_t argument_count = 0;
  std::filesystem::path source_path{};
  float intrinsic_width = 0.0F;
  float intrinsic_height = 0.0F;
  std::vector<std::byte> generated_payload{};
  std::string domain{};
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  std::uint8_t U8() {
    Require(1);
    return std::to_integer<std::uint8_t>(bytes_[offset_++]);
  }

  std::uint32_t U32() {
    Require(4);
    const std::uint32_t value =
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 1])) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 3])) << 24U);
    offset_ += 4;
    return value;
  }

  std::uint64_t U64() {
    const std::uint64_t low = U32();
    const std::uint64_t high = U32();
    return low | (high << 32U);
  }

  float F32() {
    return std::bit_cast<float>(U32());
  }

  std::string String() {
    const std::uint32_t length = U32();
    Require(length);
    const char* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
    offset_ += length;
    return std::string(begin, length);
  }

  [[nodiscard]] bool AtEnd() const noexcept {
    return offset_ == bytes_.size();
  }

private:
  void Require(std::size_t count) const {
    if (count > bytes_.size() - offset_) {
      throw std::runtime_error("resource index is truncated");
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

EntryKind ReadEntryKind(Reader& reader) {
  switch (static_cast<EntryKind>(reader.U8())) {
  case EntryKind::Raw:
    return EntryKind::Raw;
  case EntryKind::Image:
    return EntryKind::Image;
  case EntryKind::String:
    return EntryKind::String;
  default:
    throw std::runtime_error("resource index contains an unknown entry kind");
  }
}

std::string Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to read resource: " + path.string());
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff length = stream.tellg();
  if (length < 0) {
    throw std::runtime_error("unable to determine resource size: " + path.string());
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), length)) {
    throw std::runtime_error("unable to read resource: " + path.string());
  }
  return bytes;
}

std::string ReadText(const std::filesystem::path& path) {
  const std::vector<std::byte> bytes = ReadBytes(path);
  if (bytes.empty()) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void WriteBytesIfChanged(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  if (std::filesystem::exists(path) && ReadBytes(path) == std::vector<std::byte>(bytes.begin(), bytes.end())) {
    return;
  }
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream || (!bytes.empty() && !stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
    throw std::runtime_error("unable to write generated resource: " + path.string());
  }
}

void WriteTextIfChanged(const std::filesystem::path& path, std::string_view value) {
  WriteBytesIfChanged(path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

std::string Utf8PathString(const std::filesystem::path& path) {
  const std::u8string utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::filesystem::path PathFromUtf8(std::string_view value) {
  std::u8string utf8(value.size(), u8'\0');
  if (!value.empty()) {
    std::memcpy(utf8.data(), value.data(), value.size());
  }
  return std::filesystem::path(std::move(utf8));
}

std::string GenericPath(const std::filesystem::path& path) {
  const std::string value = Utf8PathString(path);
  if (value.empty() || path.is_absolute() || path.has_root_path() || value.find(':') != std::string::npos ||
      value.find('\\') != std::string::npos || value.find('\0') != std::string::npos ||
      value.find('"') != std::string::npos ||
      std::ranges::any_of(
          value,
          [](char character) {
            const unsigned char byte = static_cast<unsigned char>(character);
            return byte < 0x20 || byte == 0x7F;
          }
      ) ||
      std::ranges::any_of(path, [](const std::filesystem::path& component) {
        return component.empty() || component == "." || component == "..";
      })) {
    throw std::runtime_error("resource path is not package-relative: " + value);
  }
  return value;
}

std::string MimeType(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(), [](char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  });
  if (extension == ".png") {
    return "image/png";
  }
  if (extension == ".jpg" || extension == ".jpeg") {
    return "image/jpeg";
  }
  if (extension == ".json") {
    return "application/json";
  }
  if (extension == ".txt") {
    return "text/plain";
  }
  return "application/octet-stream";
}

std::uint16_t BigEndian16(const std::byte* bytes) noexcept {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint8_t>(bytes[0]) << 8U) | std::to_integer<std::uint8_t>(bytes[1])
  );
}

std::uint32_t BigEndian32(const std::byte* bytes) noexcept {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]));
}

bool IsPngChunk(std::span<const std::byte> bytes, std::size_t offset, std::string_view type) noexcept {
  return type.size() == 4 && offset + 8 <= bytes.size() &&
         std::equal(
             type.begin(),
             type.end(),
             bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4),
             [](char left, std::byte right) { return static_cast<std::byte>(left) == right; }
         );
}

std::pair<std::uint32_t, std::uint32_t> ImageSize(const std::vector<std::byte>& bytes) {
  constexpr std::byte png_signature[] = {
      std::byte{0x89},
      std::byte{'P'},
      std::byte{'N'},
      std::byte{'G'},
      std::byte{0x0D},
      std::byte{0x0A},
      std::byte{0x1A},
      std::byte{0x0A},
  };
  if (bytes.size() >= std::size(png_signature) &&
      std::equal(std::begin(png_signature), std::end(png_signature), bytes.begin())) {
    if (bytes.size() < 33 || BigEndian32(bytes.data() + 8) != 13 || !IsPngChunk(bytes, 8, "IHDR") ||
        bytes[26] != std::byte{0} || bytes[27] != std::byte{0} || std::to_integer<std::uint8_t>(bytes[28]) > 1) {
      throw std::runtime_error("PNG image resource has an invalid IHDR chunk");
    }
    const std::pair size{BigEndian32(bytes.data() + 16), BigEndian32(bytes.data() + 20)};
    if (size.first == 0 || size.second == 0) {
      throw std::runtime_error("image resource dimensions must be positive");
    }
    bool has_image_data = false;
    std::size_t offset = 8;
    while (offset + 12 <= bytes.size()) {
      const std::uint32_t length = BigEndian32(bytes.data() + offset);
      if (length > bytes.size() - offset - 12) {
        break;
      }
      const std::size_t end = offset + 12 + length;
      if (IsPngChunk(bytes, offset, "IDAT")) {
        has_image_data = true;
      } else if (IsPngChunk(bytes, offset, "IEND")) {
        if (length == 0 && has_image_data && end == bytes.size()) {
          return size;
        }
        break;
      }
      offset = end;
    }
    throw std::runtime_error("PNG image resource must contain complete IDAT and IEND chunks");
  }
  if (bytes.size() >= 4 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xD8}) {
    std::pair<std::uint32_t, std::uint32_t> size{};
    std::size_t offset = 2;
    while (offset + 3 < bytes.size()) {
      while (offset < bytes.size() && bytes[offset] == std::byte{0xFF}) {
        ++offset;
      }
      if (offset >= bytes.size()) {
        break;
      }
      const std::uint8_t marker = std::to_integer<std::uint8_t>(bytes[offset++]);
      if (marker == 0x01 || marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
        continue;
      }
      if (offset + 2 > bytes.size()) {
        break;
      }
      const std::uint16_t segment_size = BigEndian16(bytes.data() + offset);
      if (segment_size < 2 || offset + segment_size > bytes.size()) {
        break;
      }
      const bool frame = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                         (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
      if (frame && segment_size >= 7) {
        size = {
            BigEndian16(bytes.data() + offset + 5),
            BigEndian16(bytes.data() + offset + 3),
        };
        if (size.first == 0 || size.second == 0) {
          throw std::runtime_error("image resource dimensions must be positive");
        }
      }
      if (marker == 0xDA) {
        if (size.first != 0 && size.second != 0 && bytes.size() >= 2 && bytes[bytes.size() - 2] == std::byte{0xFF} &&
            bytes.back() == std::byte{0xD9}) {
          return size;
        }
        break;
      }
      offset += segment_size;
    }
  }
  throw std::runtime_error("image resource must be a supported PNG or JPEG");
}

float ImageScale(std::string& stem) {
  const std::size_t marker = stem.rfind('@');
  if (marker == std::string::npos || !stem.ends_with('x')) {
    return 1.0F;
  }
  const std::string scale_text = stem.substr(marker + 1, stem.size() - marker - 2);
  if (scale_text.empty() || !std::ranges::all_of(scale_text, [](char value) { return value >= '0' && value <= '9'; })) {
    throw std::runtime_error("image scale suffix must use @2x or another positive integer scale");
  }
  const unsigned long scale = std::stoul(scale_text);
  if (scale == 0 || scale > 16) {
    throw std::runtime_error("image scale must be between 1 and 16");
  }
  stem.erase(marker);
  return static_cast<float>(scale);
}

std::string Identifier(std::string_view value) {
  std::string result;
  result.reserve(value.size() + 1);
  for (char character : value) {
    const bool identifier_character = (character >= 'A' && character <= 'Z') ||
                                      (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    if (identifier_character) {
      result.push_back(character);
    } else if (result.empty() || result.back() != '_') {
      result.push_back('_');
    }
  }
  if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) {
    result.insert(result.begin(), '_');
  }
  static const std::set<std::string_view> keywords{
      "alignas",
      "alignof",
      "and",
      "and_eq",
      "asm",
      "atomic_cancel",
      "atomic_commit",
      "atomic_noexcept",
      "auto",
      "bitand",
      "bitor",
      "bool",
      "break",
      "case",
      "catch",
      "char",
      "char8_t",
      "char16_t",
      "char32_t",
      "class",
      "compl",
      "concept",
      "const",
      "consteval",
      "constexpr",
      "constinit",
      "const_cast",
      "continue",
      "co_await",
      "co_return",
      "co_yield",
      "decltype",
      "default",
      "delete",
      "do",
      "double",
      "dynamic_cast",
      "else",
      "enum",
      "explicit",
      "export",
      "extern",
      "false",
      "float",
      "for",
      "friend",
      "goto",
      "if",
      "inline",
      "int",
      "long",
      "mutable",
      "namespace",
      "new",
      "noexcept",
      "not",
      "not_eq",
      "nullptr",
      "operator",
      "or",
      "or_eq",
      "private",
      "protected",
      "public",
      "reflexpr",
      "register",
      "reinterpret_cast",
      "requires",
      "return",
      "short",
      "signed",
      "sizeof",
      "static",
      "static_assert",
      "static_cast",
      "struct",
      "switch",
      "synchronized",
      "template",
      "this",
      "thread_local",
      "throw",
      "true",
      "try",
      "typedef",
      "typeid",
      "typename",
      "union",
      "unsigned",
      "using",
      "virtual",
      "void",
      "volatile",
      "wchar_t",
      "while",
      "xor",
      "xor_eq",
  };
  const bool reserved = result.find("__") != std::string::npos ||
                        (result.size() > 1 && result[0] == '_' && result[1] >= 'A' && result[1] <= 'Z');
  if (reserved || keywords.contains(result)) {
    result.insert(0, "resource_");
  }
  return result;
}

void ValidateNamespace(std::string_view value) {
  const auto ascii_alpha = [](char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
  };
  const auto ascii_alphanumeric = [ascii_alpha](char character) {
    return ascii_alpha(character) || (character >= '0' && character <= '9');
  };
  if (value.empty() || !ascii_alpha(value.front()) ||
      !std::ranges::all_of(
          value.substr(1),
          [ascii_alphanumeric](char character) { return ascii_alphanumeric(character) || character == '_'; }
      ) ||
      value.back() == '_' || Identifier(value) != value) {
    throw std::runtime_error("resource namespace must be a non-reserved C++ identifier");
  }
}

std::string NormalizeLocale(std::string value) {
  std::replace(value.begin(), value.end(), '_', '-');
  std::size_t start = 0;
  std::size_t subtag = 0;
  while (start < value.size()) {
    const std::size_t end = value.find('-', start);
    const std::size_t length = (end == std::string::npos ? value.size() : end) - start;
    if (length == 0 || length > 8 ||
        !std::ranges::all_of(std::string_view(value).substr(start, length), [](char character) {
          return std::isalnum(static_cast<unsigned char>(character));
        })) {
      throw std::runtime_error("string catalog filename must be a valid BCP-47 locale or default.properties");
    }
    const bool script = subtag > 0 && length == 4 &&
                        std::ranges::all_of(std::string_view(value).substr(start, length), [](char character) {
                          return std::isalpha(static_cast<unsigned char>(character));
                        });
    const bool region =
        subtag > 0 &&
        ((length == 2 && std::ranges::all_of(
                             std::string_view(value).substr(start, length),
                             [](char character) { return std::isalpha(static_cast<unsigned char>(character)); }
                         )) ||
         (length == 3 && std::ranges::all_of(std::string_view(value).substr(start, length), [](char character) {
            return std::isdigit(static_cast<unsigned char>(character));
          })));
    for (std::size_t index = start; index < start + length; ++index) {
      value[index] = static_cast<char>(
          region ? std::toupper(static_cast<unsigned char>(value[index]))
                 : std::tolower(static_cast<unsigned char>(value[index]))
      );
    }
    if (script) {
      value[start] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[start])));
    }
    ++subtag;
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return value;
}

std::string Unquote(std::string value) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return value;
  }
  value = value.substr(1, value.size() - 2);
  std::string result;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '\\') {
      result.push_back(value[index]);
      continue;
    }
    if (++index >= value.size()) {
      throw std::runtime_error("localized string ends with an incomplete escape");
    }
    if (value[index] == 'n') {
      result.push_back('\n');
    } else if (value[index] == 't') {
      result.push_back('\t');
    } else if (value[index] == '"' || value[index] == '\\') {
      result.push_back(value[index]);
    } else {
      throw std::runtime_error("localized string contains an unsupported escape");
    }
  }
  return result;
}

std::vector<std::size_t> PlaceholderIndices(std::string_view value) {
  std::vector<std::size_t> result;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if ((value[index] == '{' || value[index] == '}') && index + 1 < value.size() && value[index + 1] == value[index]) {
      ++index;
      continue;
    }
    if (value[index] == '{') {
      const std::size_t end = value.find('}', index + 1);
      if (end == std::string_view::npos || end == index + 1 ||
          !std::ranges::all_of(value.substr(index + 1, end - index - 1), [](char character) {
            return character >= '0' && character <= '9';
          })) {
        throw std::runtime_error("localized string contains an invalid positional placeholder");
      }
      result.push_back(static_cast<std::size_t>(std::stoull(std::string(value.substr(index + 1, end - index - 1)))));
      index = end;
    } else if (value[index] == '}') {
      throw std::runtime_error("localized string contains an unmatched closing brace");
    }
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void AppendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
  AppendU32(bytes, static_cast<std::uint32_t>(value));
  AppendU32(bytes, static_cast<std::uint32_t>(value >> 32U));
}

void AppendString(std::vector<std::byte>& bytes, std::string_view value) {
  AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
  for (char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
}

void CopyPayload(const Entry& entry, const std::filesystem::path& output) {
  const std::filesystem::path destination = output / "package" / PathFromUtf8(entry.package_path);
  if (!entry.generated_payload.empty()) {
    WriteBytesIfChanged(destination, entry.generated_payload);
    return;
  }
  std::filesystem::create_directories(destination.parent_path());
  std::filesystem::copy_file(entry.source_path, destination, std::filesystem::copy_options::overwrite_existing);
}

void ValidateEntries(std::vector<Entry>& entries) {
  std::ranges::sort(entries, {}, [](const Entry& entry) {
    return std::tuple{entry.domain, entry.key, entry.locale, entry.scale, entry.package_path};
  });
  for (std::size_t index = 1; index < entries.size(); ++index) {
    const Entry& previous = entries[index - 1];
    const Entry& current = entries[index];
    if (previous.kind == current.kind && previous.domain == current.domain && previous.key == current.key &&
        previous.locale == current.locale && previous.scale == current.scale) {
      throw std::runtime_error("duplicate resource variant: " + current.key);
    }
  }

  using ResourceFamily = std::pair<std::string, std::string>;
  std::map<ResourceFamily, std::pair<float, float>> image_sizes;
  std::map<ResourceFamily, bool> vector_images;
  std::map<ResourceFamily, std::vector<std::size_t>> default_string_schemas;
  std::map<std::string, std::pair<std::uint64_t, std::string>> payloads;
  for (const Entry& entry : entries) {
    if (!entry.package_path.empty()) {
      const auto [payload, inserted] = payloads.try_emplace(entry.package_path, entry.content_hash, entry.mime_type);
      if (!inserted && payload->second != std::pair{entry.content_hash, entry.mime_type}) {
        throw std::runtime_error("resource variants assign conflicting payloads to: " + entry.package_path);
      }
    }
    const ResourceFamily family{entry.domain, entry.key};
    if (entry.kind == EntryKind::Image) {
      const bool is_vector = entry.mime_type == "application/x-huxerui-vector";
      const auto [format, inserted_format] = vector_images.try_emplace(family, is_vector);
      if (!inserted_format && format->second != is_vector) {
        throw std::runtime_error("raster and vector variants must not share an image resource key: " + entry.key);
      }
      const std::pair logical_size{entry.intrinsic_width, entry.intrinsic_height};
      const auto [found, inserted] = image_sizes.try_emplace(family, logical_size);
      if (!inserted && found->second != logical_size) {
        throw std::runtime_error("image scale variants must have the same intrinsic logical size: " + entry.key);
      }
    } else if (entry.kind == EntryKind::String && entry.locale.empty()) {
      const std::vector<std::size_t> schema = PlaceholderIndices(entry.value);
      for (std::size_t index = 0; index < schema.size(); ++index) {
        if (schema[index] != index) {
          throw std::runtime_error("default localized string arguments must be contiguous from zero: " + entry.key);
        }
      }
      default_string_schemas.emplace(family, schema);
    }
  }
  for (Entry& entry : entries) {
    if (entry.kind != EntryKind::String) {
      continue;
    }
    const auto schema = default_string_schemas.find({entry.domain, entry.key});
    if (schema == default_string_schemas.end()) {
      throw std::runtime_error("localized string requires a default catalog entry: " + entry.key);
    }
    for (std::size_t index : PlaceholderIndices(entry.value)) {
      if (!std::ranges::binary_search(schema->second, index)) {
        throw std::runtime_error("localized string references an undeclared argument: " + entry.key);
      }
    }
    entry.argument_count = static_cast<std::uint32_t>(schema->second.size());
  }

  std::map<std::tuple<std::string, EntryKind, std::string>, std::string> generated_identifiers;
  for (const Entry& entry : entries) {
    const std::string identifier = Identifier(entry.key.substr(entry.key.find('/') + 1));
    const auto [found, inserted] =
        generated_identifiers.try_emplace(std::tuple{entry.domain, entry.kind, identifier}, entry.key);
    if (!inserted && found->second != entry.key) {
      throw std::runtime_error(
          "resource keys generate the same C++ identifier: " + found->second + " and " + entry.key
      );
    }
  }
}

std::vector<Entry> Discover(const CompileOptions& options) {
  std::vector<Entry> entries;
  const std::filesystem::path images = options.root / "images";
  if (std::filesystem::exists(images)) {
    for (const auto& file : std::filesystem::recursive_directory_iterator(images)) {
      if (!file.is_regular_file()) {
        continue;
      }
      std::string extension = file.path().extension().string();
      std::ranges::transform(extension, extension.begin(), [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
      });
      if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".svg") {
        continue;
      }
      std::filesystem::path relative = std::filesystem::relative(file.path(), images);
      std::string stem = Utf8PathString(relative.stem());
      if (extension == ".svg") {
        const std::string unscaled_stem = stem;
        static_cast<void>(ImageScale(stem));
        if (stem != unscaled_stem) {
          throw std::runtime_error("SVG image resources do not support density suffixes: " + file.path().string());
        }
        const CompiledSvg compiled = CompileSvg(file.path());
        std::filesystem::path logical = relative.parent_path() / PathFromUtf8(stem);
        std::filesystem::path packaged = relative;
        packaged.replace_extension(".huxv");
        const std::string package_path = "huxerui/" + options.resource_namespace + "/images/" + GenericPath(packaged);
        entries.push_back({
            EntryKind::Image,
            "images/" + GenericPath(logical),
            package_path,
            "application/x-huxerui-vector",
            {},
            {},
            1.0F,
            0,
            0,
            resource_format::ContentHash(compiled.payload),
            0,
            file.path(),
            compiled.intrinsic_width,
            compiled.intrinsic_height,
            compiled.payload,
            {},
        });
        continue;
      }
      const float scale = ImageScale(stem);
      std::filesystem::path logical = relative.parent_path() / PathFromUtf8(stem);
      const std::string package_path = "huxerui/" + options.resource_namespace + "/images/" + GenericPath(relative);
      const std::vector<std::byte> bytes = ReadBytes(file.path());
      const auto [width, height] = ImageSize(bytes);
      entries.push_back({
          EntryKind::Image,
          "images/" + GenericPath(logical),
          package_path,
          MimeType(file.path()),
          {},
          {},
          scale,
          width,
          height,
          resource_format::ContentHash(bytes),
          0,
          file.path(),
          static_cast<float>(width) / scale,
          static_cast<float>(height) / scale,
          {},
          {},
      });
    }
  }

  const std::filesystem::path raw = options.root / "raw";
  if (std::filesystem::exists(raw)) {
    for (const auto& file : std::filesystem::recursive_directory_iterator(raw)) {
      if (!file.is_regular_file()) {
        continue;
      }
      const std::filesystem::path relative = std::filesystem::relative(file.path(), raw);
      const std::string package_path = "huxerui/" + options.resource_namespace + "/raw/" + GenericPath(relative);
      const std::vector<std::byte> bytes = ReadBytes(file.path());
      entries.push_back({
          EntryKind::Raw,
          "raw/" + GenericPath(relative),
          package_path,
          MimeType(file.path()),
          {},
          {},
          1.0F,
          0,
          0,
          resource_format::ContentHash(bytes),
          0,
          file.path(),
          0.0F,
          0.0F,
          {},
          {},
      });
    }
  }

  const std::filesystem::path strings = options.root / "strings";
  if (std::filesystem::exists(strings)) {
    for (const auto& file : std::filesystem::directory_iterator(strings)) {
      if (!file.is_regular_file()) {
        continue;
      }
      if (file.path().extension() == ".txt") {
        throw std::runtime_error("string catalog must use the .properties extension: " + file.path().string());
      }
      if (file.path().extension() != ".properties") {
        continue;
      }
      const std::string locale =
          file.path().stem() == "default" ? std::string{} : NormalizeLocale(file.path().stem().string());
      std::istringstream stream(ReadText(file.path()));
      std::string line;
      std::size_t line_number = 0;
      while (std::getline(stream, line)) {
        ++line_number;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
          continue;
        }
        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
          throw std::runtime_error(file.path().string() + ':' + std::to_string(line_number) + ": expected key = value");
        }
        const std::string key = Trim(std::string_view(trimmed).substr(0, separator));
        const std::string value = Unquote(Trim(std::string_view(trimmed).substr(separator + 1)));
        if (key.empty()) {
          throw std::runtime_error(file.path().string() + ':' + std::to_string(line_number) + ": key is empty");
        }
        const std::string validated_key = GenericPath(std::filesystem::path("strings") / PathFromUtf8(key));
        if (validated_key != "strings/" + key) {
          throw std::runtime_error("localized string key must be a normalized resource path: " + key);
        }
        static_cast<void>(PlaceholderIndices(value));
        entries.push_back({
            .kind = EntryKind::String,
            .key = "strings/" + key,
            .mime_type = "text/plain",
            .locale = locale,
            .value = value,
        });
      }
    }
  }

  for (Entry& entry : entries) {
    entry.domain = options.resource_namespace;
  }
  ValidateEntries(entries);
  return entries;
}

std::vector<std::byte> EncodeIndex(const std::vector<Entry>& entries) {
  std::vector<std::byte> bytes(resource_format::magic.begin(), resource_format::magic.end());
  AppendU32(bytes, resource_format::current_version);
  AppendU32(bytes, static_cast<std::uint32_t>(entries.size()));
  for (const Entry& entry : entries) {
    bytes.push_back(static_cast<std::byte>(entry.kind));
    AppendString(bytes, entry.domain);
    AppendString(bytes, entry.key);
    AppendString(bytes, entry.package_path);
    AppendString(bytes, entry.mime_type);
    AppendString(bytes, entry.locale);
    AppendString(bytes, entry.value);
    AppendU32(bytes, std::bit_cast<std::uint32_t>(entry.scale));
    AppendU32(bytes, entry.pixel_width);
    AppendU32(bytes, entry.pixel_height);
    AppendU64(bytes, entry.content_hash);
    AppendU32(bytes, entry.argument_count);
    AppendU32(bytes, std::bit_cast<std::uint32_t>(entry.intrinsic_width));
    AppendU32(bytes, std::bit_cast<std::uint32_t>(entry.intrinsic_height));
  }
  return bytes;
}

std::string GenerateHeader(std::string_view resource_namespace, const std::vector<Entry>& entries) {
  std::map<EntryKind, std::map<std::string, std::string>> declarations;
  for (const Entry& entry : entries) {
    if (entry.domain != resource_namespace) {
      continue;
    }
    declarations[entry.kind].try_emplace(entry.key, Identifier(entry.key.substr(entry.key.find('/') + 1)));
  }
  std::ostringstream output;
  output << "#pragma once\n\n#include <huxerui/resource.h>\n\nnamespace " << resource_namespace << " {\n\n";
  const auto write_group =
      [&output, resource_namespace, &declarations](EntryKind kind, std::string_view name, std::string_view type) {
        output << "namespace " << name << " {\n";
        for (const auto& [key, identifier] : declarations[kind]) {
          output << "inline const huxerui::" << type << ' ' << identifier << "{\"" << resource_namespace << "\", \""
                 << key << "\"};\n";
        }
        output << "} // namespace " << name << "\n\n";
      };
  write_group(EntryKind::Image, "images", "ImageResource");
  write_group(EntryKind::Raw, "raw", "RawResource");
  write_group(EntryKind::String, "strings", "StringResource");
  output << "} // namespace " << resource_namespace << "\n";
  return output.str();
}

std::vector<Entry> ReadPackage(const std::filesystem::path& package) {
  const std::filesystem::path index_path = package / "huxerui" / "resources.bin";
  const std::vector<std::byte> bytes = ReadBytes(index_path);
  if (bytes.size() < resource_format::magic.size() ||
      !std::equal(resource_format::magic.begin(), resource_format::magic.end(), bytes.begin())) {
    throw std::runtime_error("resource index has an invalid signature: " + index_path.string());
  }

  Reader reader(std::span<const std::byte>(bytes).subspan(resource_format::magic.size()));
  const std::uint32_t version = reader.U32();
  if (version != resource_format::current_version) {
    throw std::runtime_error("resource index version is unsupported: " + std::to_string(version));
  }
  const std::uint32_t count = reader.U32();
  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Entry entry;
    entry.kind = ReadEntryKind(reader);
    entry.domain = reader.String();
    entry.key = reader.String();
    entry.package_path = reader.String();
    entry.mime_type = reader.String();
    entry.locale = reader.String();
    entry.value = reader.String();
    entry.scale = reader.F32();
    entry.pixel_width = reader.U32();
    entry.pixel_height = reader.U32();
    entry.content_hash = reader.U64();
    entry.argument_count = reader.U32();
    entry.intrinsic_width = reader.F32();
    entry.intrinsic_height = reader.F32();

    ValidateNamespace(entry.domain);
    const std::string normalized_key = GenericPath(PathFromUtf8(entry.key));
    const std::string_view expected_prefix = entry.kind == EntryKind::Image ? "images/"
                                             : entry.kind == EntryKind::Raw ? "raw/"
                                                                            : "strings/";
    if (normalized_key != entry.key || !entry.key.starts_with(expected_prefix) ||
        entry.key.size() == expected_prefix.size()) {
      throw std::runtime_error("resource index contains an invalid key: " + entry.key);
    }
    if (!entry.locale.empty() && NormalizeLocale(entry.locale) != entry.locale) {
      throw std::runtime_error("resource index contains a non-normalized locale: " + entry.locale);
    }
    if (!std::isfinite(entry.scale) || entry.scale <= 0.0F) {
      throw std::runtime_error("resource index contains an invalid image scale");
    }
    if (entry.kind == EntryKind::String) {
      if (!entry.package_path.empty()) {
        throw std::runtime_error("string resource index entry must not contain a package path");
      }
    } else {
      const std::string normalized_path = GenericPath(PathFromUtf8(entry.package_path));
      const std::string expected_path_prefix = "huxerui/" + entry.domain + '/';
      if (normalized_path != entry.package_path || !entry.package_path.starts_with(expected_path_prefix)) {
        throw std::runtime_error("resource index contains an invalid package path: " + entry.package_path);
      }
      entry.source_path = package / PathFromUtf8(entry.package_path);
      if (!std::filesystem::is_regular_file(entry.source_path)) {
        throw std::runtime_error("resource package payload is missing: " + entry.package_path);
      }
      if (resource_format::ContentHash(ReadBytes(entry.source_path)) != entry.content_hash) {
        throw std::runtime_error("resource package payload hash does not match the index: " + entry.package_path);
      }
    }
    if (entry.kind == EntryKind::Raw && !entry.locale.empty()) {
      throw std::runtime_error("raw resource index entry must not contain a locale");
    }
    if (entry.kind == EntryKind::Image &&
        (!std::isfinite(entry.intrinsic_width) || !std::isfinite(entry.intrinsic_height) ||
         entry.intrinsic_width <= 0.0F || entry.intrinsic_height <= 0.0F)) {
      throw std::runtime_error("resource index contains invalid image dimensions");
    }
    entries.push_back(std::move(entry));
  }
  if (!reader.AtEnd()) {
    throw std::runtime_error("resource index contains trailing data: " + index_path.string());
  }
  ValidateEntries(entries);
  return entries;
}

using VariantIdentity = std::tuple<EntryKind, std::string, std::string, std::string, std::uint32_t>;

VariantIdentity Identity(const Entry& entry) {
  const std::string locale = entry.kind == EntryKind::Raw ? std::string{} : entry.locale;
  const std::uint32_t scale = entry.kind == EntryKind::Image ? std::bit_cast<std::uint32_t>(entry.scale) : 0;
  return {entry.kind, entry.domain, entry.key, locale, scale};
}

bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
  const std::filesystem::path normalized_child = std::filesystem::absolute(child).lexically_normal();
  const std::filesystem::path normalized_parent = std::filesystem::absolute(parent).lexically_normal();
  const auto mismatch = std::mismatch(
      normalized_parent.begin(),
      normalized_parent.end(),
      normalized_child.begin(),
      normalized_child.end()
  );
  return mismatch.first == normalized_parent.end();
}

void Publish(
    const std::filesystem::path& output,
    const std::vector<Entry>& entries,
    const std::set<std::string>& resource_namespaces,
    std::string_view header_name = {}
) {
  // These roots are wholly compiler-owned; replacing them prevents deleted resources or namespaces from surviving.
  std::filesystem::remove_all(output / "package");
  std::filesystem::remove_all(output / "include");
  for (const Entry& entry : entries) {
    if (!entry.package_path.empty()) {
      CopyPayload(entry, output);
    }
  }
  const std::filesystem::path index_path = output / "package" / "huxerui" / "resources.bin";
  WriteBytesIfChanged(index_path, EncodeIndex(entries));
  for (const std::string& resource_namespace : resource_namespaces) {
    const std::string output_name =
        header_name.empty() ? Identifier(resource_namespace) + "_resources.h" : std::string(header_name);
    WriteTextIfChanged(output / "include" / output_name, GenerateHeader(resource_namespace, entries));
  }
}

} // namespace

void Compile(const CompileOptions& options) {
  if (!std::filesystem::is_directory(options.root)) {
    throw std::runtime_error("resource root is not a directory: " + options.root.string());
  }
  if (options.output.empty()) {
    throw std::runtime_error("resource output path must not be empty");
  }
  ValidateNamespace(options.resource_namespace);
  if (!options.header_name.empty()) {
    const std::filesystem::path header_name(options.header_name);
    if (header_name != header_name.filename() || header_name.extension() != ".h") {
      throw std::runtime_error("resource header name must be a .h filename without a directory");
    }
  }
  const std::vector<Entry> entries = Discover(options);
  if (entries.empty()) {
    throw std::runtime_error("resource root does not contain any supported resources: " + options.root.string());
  }
  Publish(options.output, entries, {options.resource_namespace}, options.header_name);
}

void Merge(const MergeOptions& options) {
  if (options.inputs.empty()) {
    throw std::runtime_error("resource merge requires at least one input package");
  }
  if (options.output.empty()) {
    throw std::runtime_error("resource merge output path must not be empty");
  }
  const std::filesystem::path output_package = options.output / "package";
  std::set<std::string> resource_namespaces;

  std::map<VariantIdentity, Entry> merged;
  for (const std::filesystem::path& input : options.inputs) {
    if (!std::filesystem::is_directory(input)) {
      throw std::runtime_error("resource merge input is not a package directory: " + input.string());
    }
    if (IsWithin(input, output_package)) {
      throw std::runtime_error("resource merge input must not be inside the output package: " + input.string());
    }
    for (Entry& entry : ReadPackage(input)) {
      resource_namespaces.insert(entry.domain);
      merged.insert_or_assign(Identity(entry), std::move(entry));
    }
  }

  std::vector<Entry> entries;
  entries.reserve(merged.size());
  for (auto& [identity, entry] : merged) {
    static_cast<void>(identity);
    entries.push_back(std::move(entry));
  }
  ValidateEntries(entries);
  Publish(options.output, entries, resource_namespaces);
}

} // namespace huxerui::resource_compiler
