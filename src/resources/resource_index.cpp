#include "resource_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "resource_format.h"

namespace huxerui::detail {

namespace {

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
      throw std::logic_error("HuxerUI resource index is truncated");
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

ResourceEntryKind ReadEntryKind(Reader& reader) {
  switch (static_cast<resource_format::EntryKind>(reader.U8())) {
  case resource_format::EntryKind::Raw:
    return ResourceEntryKind::Raw;
  case resource_format::EntryKind::Image:
    return ResourceEntryKind::Image;
  case resource_format::EntryKind::String:
    return ResourceEntryKind::String;
  default:
    throw std::logic_error("HuxerUI resource index contains an unknown entry kind");
  }
}

} // namespace

std::vector<ResourceIndexEntry> ParseResourceIndex(RawAsset index) {
  const std::span<const std::byte> bytes = index.Bytes();
  if (bytes.empty()) {
    return {};
  }
  if (bytes.size() < resource_format::magic.size() ||
      !std::equal(resource_format::magic.begin(), resource_format::magic.end(), bytes.begin())) {
    throw std::logic_error("HuxerUI resource index has an invalid signature");
  }

  Reader reader(bytes.subspan(resource_format::magic.size()));
  const std::uint32_t version = reader.U32();
  if (version != resource_format::current_version) {
    throw std::logic_error("HuxerUI resource index version is unsupported: " + std::to_string(version));
  }
  const std::uint32_t count = reader.U32();
  std::vector<ResourceIndexEntry> entries;
  entries.reserve(count);
  for (std::uint32_t index_value = 0; index_value < count; ++index_value) {
    const ResourceEntryKind kind = ReadEntryKind(reader);
    const std::string domain = reader.String();
    const std::string key = reader.String();
    ResourceIndexEntry entry{
        kind,
        ResourceId(domain, key),
        reader.String(),
        reader.String(),
        reader.String(),
        reader.String(),
        reader.F32(),
        reader.U32(),
        reader.U32(),
        reader.U64(),
        reader.U32(),
        {reader.F32(), reader.F32()},
    };
    if (!std::isfinite(entry.scale) || entry.scale <= 0.0F) {
      throw std::logic_error("HuxerUI resource index contains an invalid image scale");
    }
    if (entry.kind == ResourceEntryKind::String) {
      if (!entry.package_path.empty()) {
        throw std::logic_error("HuxerUI string resource index entry must not contain a package path");
      }
    } else if (!IsValidResourcePackagePath(entry.package_path)) {
      throw std::logic_error("HuxerUI resource index contains an invalid package path");
    }
    if (entry.kind == ResourceEntryKind::Image &&
        (!std::isfinite(entry.intrinsic_size.width) || !std::isfinite(entry.intrinsic_size.height) ||
         entry.intrinsic_size.width <= 0.0F || entry.intrinsic_size.height <= 0.0F)) {
      throw std::logic_error("HuxerUI resource index contains invalid image dimensions");
    }
    entries.push_back(std::move(entry));
  }
  if (!reader.AtEnd()) {
    throw std::logic_error("HuxerUI resource index contains trailing data");
  }
  return entries;
}

bool IsValidResourcePackagePath(std::string_view path) noexcept {
  if (path.empty() || path.front() == '/' || path.back() == '/' || path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos || path.find('"') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos || std::ranges::any_of(path, [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7F;
      })) {
    return false;
  }
  for (std::size_t start = 0; start < path.size();) {
    const std::size_t end = path.find('/', start);
    const std::string_view component =
        path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

} // namespace huxerui::detail
