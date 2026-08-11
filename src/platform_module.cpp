#include <huxerui/platform_module.h>

#include <cmath>
#include <stdexcept>
#include <utility>
#include <variant>

namespace huxerui {

struct PlatformPayload::Data {
  using Value = std::variant<bool, std::int64_t, double, std::string, Bytes, List, Object>;

  explicit Data(Value value) : value(std::move(value)) {}

  Value value;
};

namespace {

constexpr std::size_t max_payload_depth = 64;

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

void ValidatePayload(const PlatformPayload& payload, std::size_t depth) {
  if (depth > max_payload_depth) {
    throw std::invalid_argument("HuxerUI PlatformPayload exceeds the maximum nesting depth");
  }

  switch (payload.Kind()) {
  case PlatformPayloadKind::Null:
  case PlatformPayloadKind::Boolean:
  case PlatformPayloadKind::Integer:
  case PlatformPayloadKind::Double:
  case PlatformPayloadKind::String:
  case PlatformPayloadKind::Bytes:
    break;
  case PlatformPayloadKind::List:
    for (const PlatformPayload& child : payload.AsList()) {
      ValidatePayload(child, depth + 1);
    }
    break;
  case PlatformPayloadKind::Object:
    for (const auto& [key, child] : payload.AsObject()) {
      if (!IsValidUtf8(key)) {
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
  if (!IsValidUtf8(value)) {
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

} // namespace huxerui
