#pragma once

#include <any>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace huxerui {

enum class PlatformPayloadKind {
  Null,
  Boolean,
  Integer,
  Double,
  String,
  Bytes,
  List,
  Object,
};

class PlatformPayload {
public:
  using Bytes = std::vector<std::byte>;
  using List = std::vector<PlatformPayload>;
  using Object = std::map<std::string, PlatformPayload, std::less<>>;

  PlatformPayload() noexcept = default;
  PlatformPayload(std::nullptr_t) noexcept {}
  PlatformPayload(bool value);
  PlatformPayload(std::int64_t value);

  template <class Integer>
    requires std::integral<Integer> && (!std::same_as<std::remove_cv_t<Integer>, bool>) &&
             (!std::same_as<std::remove_cv_t<Integer>, std::int64_t>)
  PlatformPayload(Integer value) : PlatformPayload(CheckedInteger(value)) {}

  PlatformPayload(double value);
  PlatformPayload(std::string value);
  PlatformPayload(std::string_view value);
  PlatformPayload(const char* value);
  PlatformPayload(Bytes value);
  PlatformPayload(List value);
  PlatformPayload(Object value);

  [[nodiscard]] PlatformPayloadKind Kind() const noexcept;
  [[nodiscard]] bool IsNull() const noexcept {
    return !data_;
  }

  [[nodiscard]] bool AsBoolean() const;
  [[nodiscard]] std::int64_t AsInteger() const;
  [[nodiscard]] double AsDouble() const;
  [[nodiscard]] std::string_view AsString() const;
  [[nodiscard]] std::span<const std::byte> AsBytes() const;
  [[nodiscard]] const List& AsList() const;
  [[nodiscard]] const Object& AsObject() const;

  bool operator==(const PlatformPayload& other) const;

private:
  template <class Integer> static std::int64_t CheckedInteger(Integer value) {
    if (!std::in_range<std::int64_t>(value)) {
      throw std::invalid_argument("HuxerUI PlatformPayload integer is outside the signed 64-bit range");
    }
    return static_cast<std::int64_t>(value);
  }

  struct Data;
  [[nodiscard]] const Data& RequireData() const;

  std::shared_ptr<const Data> data_;
};

using PlatformEventSink = std::function<void(std::string, PlatformPayload)>;

class PlatformModules final {
public:
  template <class Registration> void Register(std::string type, Registration registration) {
    if (type.empty()) {
      throw std::invalid_argument("HuxerUI platform module registration type must not be empty");
    }
    static_cast<void>(PlatformPayload(type));
    if (!registrations_.emplace(std::move(type), std::move(registration)).second) {
      throw std::logic_error("HuxerUI platform module type was registered more than once");
    }
  }

  template <class Registration> [[nodiscard]] const Registration* Find(std::string_view type) const {
    const auto found = registrations_.find(std::string(type));
    if (found == registrations_.end()) {
      return nullptr;
    }
    const Registration* registration = std::any_cast<Registration>(&found->second);
    if (registration == nullptr) {
      throw std::logic_error("HuxerUI platform module registration has an incompatible type");
    }
    return registration;
  }

  PlatformModules(const PlatformModules&) = delete;
  PlatformModules& operator=(const PlatformModules&) = delete;
  PlatformModules(PlatformModules&&) = delete;
  PlatformModules& operator=(PlatformModules&&) = delete;

private:
  PlatformModules() = default;

  std::unordered_map<std::string, std::any> registrations_;

  friend class PlatformAdapter;
};

} // namespace huxerui
