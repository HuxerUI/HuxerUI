#include <huxerui/platform_registry.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

#include "external_texture_internal.h"

namespace huxerui {

namespace detail {

bool IsValidUtf8(std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<std::uint8_t>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
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
    if (index + length > text.size()) {
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
    index += length;
  }
  return true;
}

} // namespace detail

struct PlatformPayload::Data {
  using Value = std::variant<bool, std::int64_t, double, std::string, Bytes, List, Object, ExternalTexture>;

  explicit Data(Value value) : value(std::move(value)) {}

  Value value;
};

namespace {

constexpr std::size_t max_envelope_bytes = 64U * 1024U * 1024U;
constexpr std::size_t max_scalar_bytes = 16U * 1024U * 1024U;
constexpr std::size_t max_container_entries = 1U * 1024U * 1024U;
constexpr std::size_t max_capability_slots = 1U * 1024U * 1024U;
constexpr std::size_t max_nesting_depth = 64;
constexpr std::uint8_t external_texture_capability = 1;

void ValidatePayload(const PlatformPayload& payload, std::size_t depth) {
  if (depth > max_nesting_depth) {
    throw std::invalid_argument("HuxerUI PlatformPayload exceeds the maximum nesting depth");
  }

  switch (payload.Kind()) {
  case PlatformPayloadKind::Null:
  case PlatformPayloadKind::Boolean:
  case PlatformPayloadKind::Integer:
  case PlatformPayloadKind::Double:
  case PlatformPayloadKind::String:
  case PlatformPayloadKind::Bytes:
  case PlatformPayloadKind::ExternalTexture:
    break;
  case PlatformPayloadKind::List:
    for (const PlatformPayload& child : payload.AsList()) {
      ValidatePayload(child, depth + 1);
    }
    break;
  case PlatformPayloadKind::Object:
    for (const auto& [key, child] : payload.AsObject()) {
      if (!detail::IsValidUtf8(key)) {
        throw std::invalid_argument("HuxerUI PlatformPayload object key must contain valid UTF-8");
      }
      ValidatePayload(child, depth + 1);
    }
    break;
  }
}

void ValidatePayload(const PlatformPayload& payload) {
  ValidatePayload(payload, 0);
}

enum class PayloadTag : std::uint8_t {
  Null,
  Boolean,
  Integer,
  Double,
  String,
  Bytes,
  List,
  Object,
  ExternalTexture,
};

class EnvelopeWriter final {
public:
  Bytes Write(const PlatformPayload& payload, std::vector<ExternalTexture>& external_textures) {
    bytes_ = {
        static_cast<std::byte>('H'),
        static_cast<std::byte>('U'),
        static_cast<std::byte>('X'),
        static_cast<std::byte>('P'),
    };
    WriteUnsigned<std::uint16_t>(1);
    WriteUnsigned<std::uint16_t>(0);
    WriteValue(payload, 0);
    external_textures = std::move(external_textures_);
    return std::move(bytes_);
  }

private:
  template <class Unsigned> void WriteUnsigned(Unsigned value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    EnsureAvailable(sizeof(Unsigned));
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
      bytes_.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
  }

  void WriteTag(PayloadTag tag) {
    WriteUnsigned(static_cast<std::uint8_t>(tag));
  }

  void WriteLength(std::size_t length, std::size_t maximum, const char* description) {
    if (length > maximum || length > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument(std::string("HuxerUI PlatformPayload ") + description + " is too large");
    }
    WriteUnsigned(static_cast<std::uint32_t>(length));
  }

  void WriteBytes(std::span<const std::byte> value) {
    EnsureAvailable(value.size());
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  void WriteString(std::string_view value) {
    WriteLength(value.size(), max_scalar_bytes, "string");
    WriteBytes({reinterpret_cast<const std::byte*>(value.data()), value.size()});
  }

  void WriteValue(const PlatformPayload& payload, std::size_t depth) {
    if (depth > max_nesting_depth) {
      throw std::invalid_argument("HuxerUI PlatformPayload exceeds the maximum envelope nesting depth");
    }
    switch (payload.Kind()) {
    case PlatformPayloadKind::Null:
      WriteTag(PayloadTag::Null);
      return;
    case PlatformPayloadKind::Boolean:
      WriteTag(PayloadTag::Boolean);
      WriteUnsigned<std::uint8_t>(payload.AsBoolean() ? 1 : 0);
      return;
    case PlatformPayloadKind::Integer:
      WriteTag(PayloadTag::Integer);
      WriteUnsigned(std::bit_cast<std::uint64_t>(payload.AsInteger()));
      return;
    case PlatformPayloadKind::Double:
      WriteTag(PayloadTag::Double);
      WriteUnsigned(std::bit_cast<std::uint64_t>(payload.AsDouble()));
      return;
    case PlatformPayloadKind::String:
      WriteTag(PayloadTag::String);
      WriteString(payload.AsString());
      return;
    case PlatformPayloadKind::Bytes:
      WriteTag(PayloadTag::Bytes);
      WriteLength(payload.AsBytes().size(), max_scalar_bytes, "byte value");
      WriteBytes(payload.AsBytes());
      return;
    case PlatformPayloadKind::List: {
      WriteTag(PayloadTag::List);
      const PlatformPayload::List& list = payload.AsList();
      WriteLength(list.size(), max_container_entries, "list");
      for (const PlatformPayload& value : list) {
        WriteValue(value, depth + 1);
      }
      return;
    }
    case PlatformPayloadKind::Object: {
      WriteTag(PayloadTag::Object);
      const PlatformPayload::Object& object = payload.AsObject();
      WriteLength(object.size(), max_container_entries, "object");
      for (const auto& [key, value] : object) {
        WriteString(key);
        WriteValue(value, depth + 1);
      }
      return;
    }
    case PlatformPayloadKind::ExternalTexture: {
      WriteTag(PayloadTag::ExternalTexture);
      WriteUnsigned(external_texture_capability);
      const ExternalTexture& texture = payload.AsExternalTexture();
      const auto found = std::ranges::find(external_textures_, texture);
      const std::size_t slot = found == external_textures_.end()
                                   ? external_textures_.size()
                                   : static_cast<std::size_t>(found - external_textures_.begin());
      if (found == external_textures_.end()) {
        if (slot >= max_capability_slots || slot > std::numeric_limits<std::uint32_t>::max()) {
          throw std::invalid_argument("HuxerUI PlatformPayload contains too many external textures");
        }
        external_textures_.push_back(texture);
      }
      WriteUnsigned(static_cast<std::uint32_t>(slot));
      return;
    }
    }
    throw std::invalid_argument("HuxerUI PlatformPayload has an unknown kind");
  }

  void EnsureAvailable(std::size_t additional) const {
    if (additional > max_envelope_bytes || bytes_.size() > max_envelope_bytes - additional) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope is too large");
    }
  }

  Bytes bytes_;
  std::vector<ExternalTexture> external_textures_;
};

class EnvelopeReader final {
public:
  EnvelopeReader(std::span<const std::byte> bytes, std::span<const ExternalTexture> external_textures)
      : bytes_(bytes), external_textures_(external_textures) {}

  PlatformPayload Read() {
    if (external_textures_.size() > max_capability_slots) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope contains too many external textures");
    }
    std::unordered_set<const detail::ExternalTextureState*> textures;
    textures.reserve(external_textures_.size());
    for (const ExternalTexture& texture : external_textures_) {
      if (!texture.HasValue()) {
        throw std::invalid_argument("HuxerUI PlatformPayload envelope contains an empty external texture");
      }
      if (!textures.insert(detail::ExternalTextureState::From(texture).get()).second) {
        throw std::invalid_argument("HuxerUI PlatformPayload envelope contains a duplicate external texture");
      }
    }
    if (bytes_.size() > max_envelope_bytes || ReadByte() != static_cast<std::byte>('H') ||
        ReadByte() != static_cast<std::byte>('U') || ReadByte() != static_cast<std::byte>('X') ||
        ReadByte() != static_cast<std::byte>('P')) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope has an invalid header");
    }
    if (ReadUnsigned<std::uint16_t>() != 1) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope has an unsupported version");
    }
    if (ReadUnsigned<std::uint16_t>() != 0) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope has unsupported flags");
    }
    PlatformPayload result = ReadValue(0);
    if (offset_ != bytes_.size()) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope contains trailing bytes");
    }
    return result;
  }

private:
  template <class Unsigned> Unsigned ReadUnsigned() {
    static_assert(std::is_unsigned_v<Unsigned>);
    Require(sizeof(Unsigned));
    Unsigned value = 0;
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
      value |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << (index * 8U);
    }
    return value;
  }

  std::byte ReadByte() {
    Require(1);
    return bytes_[offset_++];
  }

  std::size_t ReadLength(std::size_t maximum, const char* description) {
    const std::size_t length = ReadUnsigned<std::uint32_t>();
    if (length > maximum) {
      throw std::invalid_argument(std::string("HuxerUI PlatformPayload envelope ") + description + " is too large");
    }
    return length;
  }

  std::span<const std::byte> ReadBytes(std::size_t length) {
    Require(length);
    const std::span<const std::byte> result = bytes_.subspan(offset_, length);
    offset_ += length;
    return result;
  }

  std::string ReadString() {
    const std::span<const std::byte> bytes = ReadBytes(ReadLength(max_scalar_bytes, "string"));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }

  PlatformPayload ReadValue(std::size_t depth) {
    if (depth > max_nesting_depth) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope exceeds the maximum nesting depth");
    }
    switch (static_cast<PayloadTag>(ReadUnsigned<std::uint8_t>())) {
    case PayloadTag::Null:
      return {};
    case PayloadTag::Boolean: {
      const std::uint8_t value = ReadUnsigned<std::uint8_t>();
      if (value > 1) {
        throw std::invalid_argument("HuxerUI PlatformPayload envelope contains an invalid boolean");
      }
      return PlatformPayload(value != 0);
    }
    case PayloadTag::Integer:
      return PlatformPayload(std::bit_cast<std::int64_t>(ReadUnsigned<std::uint64_t>()));
    case PayloadTag::Double:
      return PlatformPayload(std::bit_cast<double>(ReadUnsigned<std::uint64_t>()));
    case PayloadTag::String:
      return PlatformPayload(ReadString());
    case PayloadTag::Bytes: {
      const std::span<const std::byte> bytes = ReadBytes(ReadLength(max_scalar_bytes, "byte value"));
      return PlatformPayload(Bytes(bytes.begin(), bytes.end()));
    }
    case PayloadTag::List: {
      const std::size_t size = ReadLength(max_container_entries, "list");
      PlatformPayload::List list;
      list.reserve(size);
      for (std::size_t index = 0; index < size; ++index) {
        list.push_back(ReadValue(depth + 1));
      }
      return PlatformPayload(std::move(list));
    }
    case PayloadTag::Object: {
      const std::size_t size = ReadLength(max_container_entries, "object");
      PlatformPayload::Object object;
      for (std::size_t index = 0; index < size; ++index) {
        std::string key = ReadString();
        if (!object.emplace(std::move(key), ReadValue(depth + 1)).second) {
          throw std::invalid_argument("HuxerUI PlatformPayload envelope contains a duplicate object key");
        }
      }
      return PlatformPayload(std::move(object));
    }
    case PayloadTag::ExternalTexture: {
      if (ReadUnsigned<std::uint8_t>() != external_texture_capability) {
        throw std::invalid_argument("HuxerUI PlatformPayload envelope contains an unknown capability kind");
      }
      const std::uint32_t slot = ReadUnsigned<std::uint32_t>();
      if (slot >= external_textures_.size()) {
        throw std::invalid_argument("HuxerUI PlatformPayload envelope references a missing external texture");
      }
      return PlatformPayload(external_textures_[slot]);
    }
    }
    throw std::invalid_argument("HuxerUI PlatformPayload envelope contains an unknown value tag");
  }

  void Require(std::size_t length) const {
    if (length > bytes_.size() || offset_ > bytes_.size() - length) {
      throw std::invalid_argument("HuxerUI PlatformPayload envelope is truncated");
    }
  }

  std::span<const std::byte> bytes_;
  std::span<const ExternalTexture> external_textures_;
  std::size_t offset_ = 0;
};

} // namespace

PlatformPayload::PlatformPayload(bool value) : data_(std::make_shared<Data>(value)) {}

PlatformPayload::PlatformPayload(std::int64_t value) : data_(std::make_shared<Data>(value)) {}

PlatformPayload::PlatformPayload(double value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("HuxerUI PlatformPayload double must be finite");
  }
  data_ = std::make_shared<Data>(value);
}

PlatformPayload::PlatformPayload(std::string value) {
  if (!detail::IsValidUtf8(value)) {
    throw std::invalid_argument("HuxerUI PlatformPayload string must contain valid UTF-8");
  }
  data_ = std::make_shared<Data>(std::move(value));
}

PlatformPayload::PlatformPayload(std::string_view value) : PlatformPayload(std::string(value)) {}

PlatformPayload::PlatformPayload(const char* value) {
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI PlatformPayload string pointer must not be null");
  }
  *this = PlatformPayload(std::string_view(value));
}

PlatformPayload::PlatformPayload(Bytes value) : data_(std::make_shared<Data>(std::move(value))) {}

PlatformPayload::PlatformPayload(List value) : data_(std::make_shared<Data>(std::move(value))) {
  ValidatePayload(*this);
}

PlatformPayload::PlatformPayload(Object value) : data_(std::make_shared<Data>(std::move(value))) {
  ValidatePayload(*this);
}

PlatformPayload::PlatformPayload(ExternalTexture value) {
  if (!value.HasValue()) {
    throw std::invalid_argument("HuxerUI PlatformPayload external texture must not be empty");
  }
  data_ = std::make_shared<Data>(std::move(value));
}

PlatformPayloadKind PlatformPayload::Kind() const noexcept {
  if (!data_) {
    return PlatformPayloadKind::Null;
  }
  return static_cast<PlatformPayloadKind>(data_->value.index() + 1);
}

bool PlatformPayload::AsBoolean() const {
  return std::get<bool>(RequireData().value);
}

std::int64_t PlatformPayload::AsInteger() const {
  return std::get<std::int64_t>(RequireData().value);
}

double PlatformPayload::AsDouble() const {
  return std::get<double>(RequireData().value);
}

std::string_view PlatformPayload::AsString() const {
  return std::get<std::string>(RequireData().value);
}

std::span<const std::byte> PlatformPayload::AsBytes() const {
  return std::get<Bytes>(RequireData().value);
}

const PlatformPayload::List& PlatformPayload::AsList() const {
  return std::get<List>(RequireData().value);
}

const PlatformPayload::Object& PlatformPayload::AsObject() const {
  return std::get<Object>(RequireData().value);
}

const ExternalTexture& PlatformPayload::AsExternalTexture() const {
  return std::get<ExternalTexture>(RequireData().value);
}

const PlatformPayload::Data& PlatformPayload::RequireData() const {
  if (!data_) {
    throw std::bad_variant_access();
  }
  return *data_;
}

bool PlatformPayload::operator==(const PlatformPayload& other) const {
  if (data_ == other.data_) {
    return true;
  }
  return data_ && other.data_ && data_->value == other.data_->value;
}

Bytes PlatformPayload::Encode(std::vector<ExternalTexture>& external_textures) const {
  external_textures.clear();
  return EnvelopeWriter().Write(*this, external_textures);
}

PlatformPayload PlatformPayload::Decode(std::span<const std::byte> bytes,
                                        std::span<const ExternalTexture> external_textures) {
  return EnvelopeReader(bytes, external_textures).Read();
}

} // namespace huxerui
