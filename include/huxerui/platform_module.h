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
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/event.h>

namespace huxerui {

class PlatformAdapter;

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

using PlatformRequestId = std::uint64_t;

struct PlatformError {
  std::string code;
  std::string message;
  PlatformPayload details;

  bool operator==(const PlatformError&) const = default;
};

template <class Result> using PlatformResult = std::variant<Result, PlatformError>;

using PlatformResultSink = std::function<void(PlatformResult<PlatformPayload>)>;
using UIThreadDispatcher = std::function<void(std::function<void()>)>;

struct PlatformModuleFactory {
  struct Instance {
    std::function<std::function<void()>(std::string, PlatformPayload, PlatformResultSink)> call;
    std::function<void()> dispose;
  };

  std::function<Instance(const PlatformPayload&, PlatformEventSink)> create;
};

namespace detail {

template <class Method>
concept PlatformMethodKey = requires(const typename Method::Request& request, const PlatformPayload& payload) {
  typename Method::Request;
  typename Method::Result;
  requires std::is_object_v<typename Method::Request>;
  requires std::is_object_v<typename Method::Result>;
  requires std::move_constructible<typename Method::Result>;
  requires(!std::same_as<std::remove_cv_t<typename Method::Result>, PlatformError>);
  { Method::Name } -> std::convertible_to<std::string_view>;
  { Method::Encode(request) } -> std::same_as<PlatformPayload>;
  { Method::Decode(payload) } -> std::convertible_to<typename Method::Result>;
};

template <class Function, class Tuple, std::size_t... Indices>
consteval bool IsPlatformTupleInvocable(std::index_sequence<Indices...>) {
  return requires(Function&& function, Tuple&& tuple) {
    std::invoke(
        std::forward<Function>(function), std::get<Indices>(std::forward<Tuple>(tuple))...
    );
  };
}

template <class Function, class Tuple> consteval bool IsPlatformTupleInvocable() {
  using Value = std::remove_cvref_t<Tuple>;
  if constexpr (requires { typename std::tuple_size<Value>::type; }) {
    return IsPlatformTupleInvocable<Function, Tuple>(std::make_index_sequence<std::tuple_size_v<Value>>{});
  }
  return false;
}

template <class Key, class Signature> struct PlatformEventDecoder {
  static constexpr bool compatible = false;
};

template <class Key> struct PlatformEventDecoder<Key, void()> {
  static constexpr bool compatible = requires(const PlatformPayload& payload) { Key::Decode(payload); };

  template <class Handler> static void Dispatch(const PlatformPayload& payload, Handler& handler) {
    Key::Decode(payload);
    std::invoke(handler);
  }
};

template <class Key, class Argument> struct PlatformEventDecoder<Key, void(Argument)> {
  static constexpr bool compatible = requires(const PlatformPayload& payload, std::function<void(Argument)>& handler) {
    std::invoke(handler, Key::Decode(payload));
  };

  template <class Handler> static void Dispatch(const PlatformPayload& payload, Handler& handler) {
    decltype(auto) decoded = Key::Decode(payload);
    std::invoke(handler, std::forward<decltype(decoded)>(decoded));
  }
};

template <class Key, class First, class Second, class... Rest>
struct PlatformEventDecoder<Key, void(First, Second, Rest...)> {
  static consteval bool IsCompatible() {
    if constexpr (requires(const PlatformPayload& payload) { Key::Decode(payload); }) {
      using Handler = std::function<void(First, Second, Rest...)>&;
      using Decoded = decltype(Key::Decode(std::declval<const PlatformPayload&>()));
      return IsPlatformTupleInvocable<Handler, Decoded>();
    }
    return false;
  }

  static constexpr bool compatible = IsCompatible();

  template <class Handler> static void Dispatch(const PlatformPayload& payload, Handler& handler) {
    decltype(auto) decoded = Key::Decode(payload);
    std::apply(
        [&handler](auto&&... values) { std::invoke(handler, std::forward<decltype(values)>(values)...); },
        std::forward<decltype(decoded)>(decoded)
    );
  }
};

template <class Key>
concept PlatformEventKey = EventKey<Key> && requires {
  { Key::Name } -> std::convertible_to<std::string_view>;
  requires PlatformEventDecoder<Key, typename Key::Signature>::compatible;
};

template <PlatformEventKey Key, class Handler>
void DispatchPlatformEvent(const PlatformPayload& payload, Handler& handler) {
  PlatformEventDecoder<Key, typename Key::Signature>::Dispatch(payload, handler);
}

} // namespace detail

class PlatformInstance final {
public:
  PlatformInstance(const PlatformInstance&) = delete;
  PlatformInstance& operator=(const PlatformInstance&) = delete;
  PlatformInstance(PlatformInstance&& other) noexcept;
  PlatformInstance& operator=(PlatformInstance&& other) noexcept;
  ~PlatformInstance();

  template <detail::PlatformMethodKey Method, class Completion>
    requires std::constructible_from<std::function<void(PlatformResult<typename Method::Result>)>, Completion>
  PlatformRequestId Call(const typename Method::Request& request, Completion&& completion) {
    std::function<void(PlatformResult<typename Method::Result>)> typed_completion(std::forward<Completion>(completion));
    return CallRaw(
        std::string(std::string_view(Method::Name)),
        Method::Encode(request),
        [completion = std::move(typed_completion)](PlatformResult<PlatformPayload> result) mutable {
          if (const auto* error = std::get_if<PlatformError>(&result)) {
            std::invoke(completion, PlatformResult<typename Method::Result>{*error});
            return;
          }
          PlatformResult<typename Method::Result> decoded = [&result]() -> PlatformResult<typename Method::Result> {
            try {
              return Method::Decode(std::get<PlatformPayload>(result));
            } catch (...) {
              return PlatformError{
                  "huxerui/invalid-result",
                  "HuxerUI platform module returned an invalid result payload",
                  {},
              };
            }
          }();
          std::invoke(completion, std::move(decoded));
        }
    );
  }

  template <detail::PlatformEventKey Key, class Handler>
    requires std::constructible_from<std::function<typename Key::Signature>, Handler>
  void On(Handler&& handler) {
    std::function<typename Key::Signature> typed_handler(std::forward<Handler>(handler));
    OnRaw(
        std::string(std::string_view(Key::Name)),
        [handler = std::move(typed_handler)](const PlatformPayload& payload) mutable {
          detail::DispatchPlatformEvent<Key>(payload, handler);
        }
    );
  }

  bool Cancel(PlatformRequestId request);
  void Close() noexcept;

private:
  struct State;

  explicit PlatformInstance(std::shared_ptr<State> state) : state_(std::move(state)) {}
  PlatformRequestId CallRaw(
      std::string method, PlatformPayload arguments, std::function<void(PlatformResult<PlatformPayload>)> completion
  );
  void OnRaw(std::string event, std::function<void(const PlatformPayload&)> handler);

  std::shared_ptr<State> state_;

  friend class PlatformModules;
};

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

  [[nodiscard]] PlatformInstance Open(std::string type, PlatformPayload options = {});

  PlatformModules(const PlatformModules&) = delete;
  PlatformModules& operator=(const PlatformModules&) = delete;
  PlatformModules(PlatformModules&&) = delete;
  PlatformModules& operator=(PlatformModules&&) = delete;

private:
  template <class Registration> [[nodiscard]] const Registration* FindCompatible(std::string_view type) const {
    const auto found = registrations_.find(std::string(type));
    return found == registrations_.end() ? nullptr : std::any_cast<Registration>(&found->second);
  }

  PlatformModules(PlatformAdapter& adapter, UIThreadDispatcher dispatch_to_ui_thread)
      : adapter_(&adapter), dispatch_to_ui_thread_(std::move(dispatch_to_ui_thread)) {}

  PlatformAdapter* adapter_;
  std::unordered_map<std::string, std::any> registrations_;
  UIThreadDispatcher dispatch_to_ui_thread_;

  friend class PlatformAdapter;
};

} // namespace huxerui
