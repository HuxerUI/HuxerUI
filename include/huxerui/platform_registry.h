#pragma once

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
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/event.h>
#include <huxerui/external_texture.h>

namespace huxerui {

class PlatformAdapter;
class PlatformChannel;
class PlatformEventEmitter;

/// Identifies the exact value kind stored by a PlatformPayload.
///
/// Kinds are preserved by the HUXP binary representation. In particular, Integer and Double remain distinct, Bytes
/// are not interpreted as text, and ExternalTexture remains an opaque framework capability.
enum class PlatformPayloadKind {
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

/// An immutable dynamic value used only when data crosses a platform-language boundary.
///
/// Direct C++ PlatformModule and PlatformView implementations receive their concrete C++ types and do not need this
/// class. Java, Swift, Objective-C, JavaScript, and similar bridges use PlatformPayload to exchange null, scalar,
/// collection, byte, and ExternalTexture values without JSON coercion.
///
/// Objects require UTF-8 string keys. Integer construction rejects values outside the signed 64-bit range, and the
/// typed accessors throw std::invalid_argument when the stored kind does not match the requested kind.
///
/// Example:
/// @code
/// PlatformPayload request(PlatformPayload::Object{
///     {"enabled", true},
///     {"attempt", 3},
/// });
/// const auto& fields = request.AsObject();
/// const bool enabled = fields.at("enabled").AsBoolean();
/// @endcode
class PlatformPayload {
public:
  /// A payload array whose elements retain their individual kinds.
  using List = std::vector<PlatformPayload>;
  /// A payload object with unique UTF-8 keys and deterministic key ordering.
  using Object = std::map<std::string, PlatformPayload, std::less<>>;

  /// Creates Null. Passing nullptr has the same effect.
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
  PlatformPayload(std::shared_ptr<ExternalTexture> value);

  /// Returns the exact stored kind.
  [[nodiscard]] PlatformPayloadKind Kind() const noexcept;
  /// Returns true when this payload stores Null.
  [[nodiscard]] bool IsNull() const noexcept {
    return !data_;
  }

  /// Returns the stored value or throws std::invalid_argument when the kind does not match.
  [[nodiscard]] bool AsBoolean() const;
  [[nodiscard]] std::int64_t AsInteger() const;
  [[nodiscard]] double AsDouble() const;
  [[nodiscard]] std::string_view AsString() const;
  [[nodiscard]] std::span<const std::byte> AsBytes() const;
  [[nodiscard]] const List& AsList() const;
  [[nodiscard]] const Object& AsObject() const;
  [[nodiscard]] const std::shared_ptr<ExternalTexture>& AsExternalTexture() const;

  /// Compares values by kind and contents. Object insertion order does not affect equality.
  bool operator==(const PlatformPayload& other) const;

  /// Encodes this value to the HUXP binary representation.
  ///
  /// external_textures is replaced with the ExternalTexture capability table referenced by validated slots in the
  /// byte stream. The returned bytes and companion texture list must be delivered together to Decode.
  [[nodiscard]] Bytes Encode(std::vector<std::shared_ptr<ExternalTexture>>& external_textures) const;
  /// Decodes one complete HUXP value and validates the representation and referenced ExternalTexture slots.
  [[nodiscard]] static PlatformPayload Decode(std::span<const std::byte> bytes,
                                              std::span<const std::shared_ptr<ExternalTexture>> external_textures = {});

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

/// Identifies one in-flight PlatformChannel request within that channel.
using PlatformRequestId = std::uint64_t;

/// A structured failure returned by a cross-language PlatformChannel invocation.
///
/// Framework-owned codes use the `huxerui/` prefix. Libraries should use stable namespaced codes of their own.
struct PlatformError {
  std::string code;
  std::string message;
  PlatformPayload details;

  bool operator==(const PlatformError&) const = default;
};

/// The completion value of a typed PlatformChannel invocation.
template <class Result> using PlatformResult = std::variant<Result, PlatformError>;

/// Holds an immutable strongly typed C++ value inside the in-process registry and render path.
///
/// PlatformValue preserves exact C++ type identity and optional value equality after type erasure. It never crosses a
/// platform-language boundary; PlatformPayload owns that contract.
class PlatformValue final {
public:
  PlatformValue() = default;

  template <class T>
    requires std::move_constructible<std::decay_t<T>>
  static PlatformValue Store(T&& value) {
    using Value = std::decay_t<T>;
    PlatformValue result;
    result.type_ = typeid(Value);
    result.value_ = std::make_shared<const Value>(std::forward<T>(value));
    if constexpr (std::equality_comparable<Value>) {
      result.equals_ = [](const void* left, const void* right) {
        return *static_cast<const Value*>(left) == *static_cast<const Value*>(right);
      };
    }
    return result;
  }

  [[nodiscard]] bool HasValue() const noexcept {
    return static_cast<bool>(value_);
  }

  [[nodiscard]] std::type_index Type() const noexcept {
    return type_;
  }

  template <class T> [[nodiscard]] const T& Get() const {
    if (type_ != typeid(T) || !value_) {
      throw std::logic_error("HuxerUI platform value has an incompatible type");
    }
    return *static_cast<const T*>(value_.get());
  }

  [[nodiscard]] bool Equivalent(const PlatformValue& other) const {
    if (value_ == other.value_) {
      return true;
    }
    return type_ == other.type_ && value_ && other.value_ && equals_ != nullptr &&
           equals_(value_.get(), other.value_.get());
  }

  bool operator==(const PlatformValue& other) const {
    return Equivalent(other);
  }

private:
  std::type_index type_{typeid(void)};
  std::shared_ptr<const void> value_;
  bool (*equals_)(const void*, const void*) = nullptr;
};

namespace detail {

struct PlatformViewFactoryRegistration {
  std::type_index type{typeid(void)};
  std::shared_ptr<void> factory;
};

template <class Factory>
PlatformViewFactoryRegistration MakePlatformViewFactoryRegistration(std::shared_ptr<Factory> factory) {
  return {typeid(Factory), std::move(factory)};
}

template <class Signature> struct PlatformEventSignature {
  static constexpr std::size_t argument_count = 2;
};

template <class Result> struct PlatformEventSignature<Result()> {
  static constexpr std::size_t argument_count = 0;
  using ResultType = Result;
};

template <class Result, class Argument> struct PlatformEventSignature<Result(Argument)> {
  static constexpr std::size_t argument_count = 1;
  using ResultType = Result;
  using Value = std::remove_cvref_t<Argument>;
};

template <class Key>
concept PlatformEventKey = EventKey<Key> && requires {
  { Key::Name } -> std::convertible_to<std::string_view>;
  requires PlatformEventSignature<typename Key::Signature>::argument_count <= 1;
  requires std::is_void_v<typename PlatformEventSignature<typename Key::Signature>::ResultType> ||
               std::copy_constructible<typename PlatformEventSignature<typename Key::Signature>::ResultType>;
};

template <class Value> Value DecodePlatformPayload(const PlatformPayload& payload) {
  if constexpr (std::same_as<Value, std::monostate>) {
    if (!payload.IsNull()) {
      throw std::invalid_argument("HuxerUI platform result must be null");
    }
    return {};
  } else if constexpr (std::same_as<Value, bool>) {
    return payload.AsBoolean();
  } else if constexpr (std::integral<Value>) {
    if (!std::in_range<Value>(payload.AsInteger())) {
      throw std::invalid_argument("HuxerUI platform event integer is outside the destination range");
    }
    return static_cast<Value>(payload.AsInteger());
  } else if constexpr (std::floating_point<Value>) {
    return static_cast<Value>(payload.AsDouble());
  } else if constexpr (std::same_as<Value, std::string>) {
    return std::string(payload.AsString());
  } else if constexpr (std::same_as<Value, Bytes>) {
    const std::span<const std::byte> bytes = payload.AsBytes();
    return Bytes(bytes.begin(), bytes.end());
  } else if constexpr (std::same_as<Value, std::shared_ptr<ExternalTexture>>) {
    return payload.AsExternalTexture();
  } else {
    return Value::Decode(payload);
  }
}

template <class Value> PlatformPayload EncodePlatformValue(const Value& value) {
  if constexpr (std::same_as<Value, std::monostate>) {
    static_cast<void>(value);
    return {};
  } else if constexpr (std::same_as<Value, PlatformPayload>) {
    return value;
  } else if constexpr (std::same_as<Value, bool> || std::integral<Value> || std::floating_point<Value> ||
                       std::same_as<Value, std::string> || std::same_as<Value, Bytes> ||
                       std::same_as<Value, std::shared_ptr<ExternalTexture>>) {
    return PlatformPayload(value);
  } else {
    return Value::Encode(value);
  }
}

template <class Value>
concept PlatformPayloadEncodable =
    std::same_as<Value, std::monostate> || std::same_as<Value, PlatformPayload> || std::same_as<Value, bool> ||
    std::integral<Value> || std::floating_point<Value> || std::same_as<Value, std::string> ||
    std::same_as<Value, Bytes> || std::same_as<Value, std::shared_ptr<ExternalTexture>> ||
    requires(const Value& value) {
      { Value::Encode(value) } -> std::same_as<PlatformPayload>;
    };

template <class Value>
concept PlatformPayloadDecodable =
    std::same_as<Value, std::monostate> || std::same_as<Value, bool> || std::integral<Value> ||
    std::floating_point<Value> || std::same_as<Value, std::string> || std::same_as<Value, Bytes> ||
    std::same_as<Value, std::shared_ptr<ExternalTexture>> || requires(const PlatformPayload& payload) {
      { Value::Decode(payload) } -> std::convertible_to<Value>;
    };

template <PlatformEventKey Key> consteval bool CanDispatchPlatformPayloadEvent() {
  using Signature = PlatformEventSignature<typename Key::Signature>;
  constexpr bool argument_supported = [] {
    if constexpr (Signature::argument_count == 0) {
      return true;
    } else {
      return PlatformPayloadDecodable<typename Signature::Value>;
    }
  }();
  constexpr bool result_supported = [] {
    if constexpr (std::is_void_v<typename Signature::ResultType>) {
      return true;
    } else {
      return PlatformPayloadEncodable<typename Signature::ResultType>;
    }
  }();
  return argument_supported && result_supported;
}

template <PlatformEventKey Key> consteval bool IsPlatformPayloadEventKey() {
  using Signature = PlatformEventSignature<typename Key::Signature>;
  return std::is_void_v<typename Signature::ResultType> && CanDispatchPlatformPayloadEvent<Key>();
}

template <class Key>
concept PlatformPayloadEventKey = PlatformEventKey<Key> && IsPlatformPayloadEventKey<Key>();

struct PlatformEventDescriptor {
  std::type_index key{typeid(void)};
  std::string name;
  std::type_index argument_type{typeid(void)};
  std::optional<PlatformValue> (*dispatch_direct)(const PlatformValue&, const EventBindings&) = nullptr;
  std::optional<PlatformPayload> (*dispatch_payload)(const PlatformPayload&, const EventBindings&) = nullptr;

  bool operator==(const PlatformEventDescriptor&) const = default;
};

template <PlatformEventKey Key> PlatformEventDescriptor MakePlatformEventDescriptor() {
  using Signature = PlatformEventSignature<typename Key::Signature>;
  using Result = typename Signature::ResultType;
  if constexpr (Signature::argument_count == 0) {
    std::optional<PlatformPayload> (*dispatch_payload)(const PlatformPayload&, const EventBindings&) = nullptr;
    if constexpr (CanDispatchPlatformPayloadEvent<Key>()) {
      dispatch_payload =
          [](const PlatformPayload& payload, const EventBindings& bindings) -> std::optional<PlatformPayload> {
        if (!payload.IsNull()) {
          throw std::logic_error("HuxerUI fieldless platform event payload must be null");
        }
        if constexpr (std::is_void_v<Result>) {
          static_cast<void>(EmitEvent<Key>(bindings));
          return std::nullopt;
        } else {
          std::optional<Result> result = EmitEvent<Key>(bindings);
          return result.has_value() ? std::optional{EncodePlatformValue(*result)} : std::nullopt;
        }
      };
    }
    return {
        typeid(Key),
        std::string(std::string_view(Key::Name)),
        typeid(void),
        [](const PlatformValue& value, const EventBindings& bindings) -> std::optional<PlatformValue> {
          if (value.HasValue()) {
            throw std::logic_error("HuxerUI fieldless platform event carried a value");
          }
          if constexpr (std::is_void_v<Result>) {
            static_cast<void>(EmitEvent<Key>(bindings));
            return std::nullopt;
          } else {
            std::optional<Result> result = EmitEvent<Key>(bindings);
            return result.has_value() ? std::optional{PlatformValue::Store(std::move(*result))} : std::nullopt;
          }
        },
        dispatch_payload,
    };
  } else {
    using Value = typename Signature::Value;
    std::optional<PlatformPayload> (*dispatch_payload)(const PlatformPayload&, const EventBindings&) = nullptr;
    if constexpr (CanDispatchPlatformPayloadEvent<Key>()) {
      dispatch_payload =
          [](const PlatformPayload& payload, const EventBindings& bindings) -> std::optional<PlatformPayload> {
        Value value = DecodePlatformPayload<Value>(payload);
        if constexpr (std::is_void_v<Result>) {
          static_cast<void>(EmitEvent<Key>(bindings, value));
          return std::nullopt;
        } else {
          std::optional<Result> result = EmitEvent<Key>(bindings, value);
          return result.has_value() ? std::optional{EncodePlatformValue(*result)} : std::nullopt;
        }
      };
    }
    return {
        typeid(Key),
        std::string(std::string_view(Key::Name)),
        typeid(Value),
        [](const PlatformValue& value, const EventBindings& bindings) -> std::optional<PlatformValue> {
          if constexpr (std::is_void_v<Result>) {
            static_cast<void>(EmitEvent<Key>(bindings, value.Get<Value>()));
            return std::nullopt;
          } else {
            std::optional<Result> result = EmitEvent<Key>(bindings, value.Get<Value>());
            return result.has_value() ? std::optional{PlatformValue::Store(std::move(*result))} : std::nullopt;
          }
        },
        dispatch_payload,
    };
  }
}

class PlatformChannelState;
class PlatformChannelEndpoint;
PlatformChannelEndpoint MakePlatformChannelEndpoint(PlatformAdapter& adapter);

PlatformEventEmitter MakePlatformEventEmitter(
    std::function<std::optional<PlatformValue>(std::type_index, PlatformValue)> emit_direct,
    std::function<std::optional<PlatformPayload>(std::string, PlatformPayload)> emit_payload
);

/// Owns all PlatformModule and PlatformView registrations for one surface.
///
/// RootContext is the public registration facade. This internal owner enforces one case-sensitive name space, exact
/// C++ type matching, and registration immutability after root installation completes.
class PlatformRegistry final {
public:
  explicit PlatformRegistry(PlatformAdapter& adapter) : adapter_(&adapter) {}

  template <class Module, class Factory> void RegisterModule(std::string name, Factory factory) {
    RegisterModuleImpl<Module, void>(std::move(name), std::move(factory));
  }

  template <class Module, class Options, class Factory> void RegisterModule(std::string name, Factory factory) {
    RegisterModuleImpl<Module, Options>(std::move(name), std::move(factory));
  }

  template <class Properties, class Controller = void, class Factory>
  void RegisterView(std::string name, Factory factory) {
    if constexpr (!std::same_as<Properties, void>) {
      static_assert(std::move_constructible<Properties> && std::equality_comparable<Properties>);
    }
    if constexpr (!std::same_as<Controller, void>) {
      static_assert(std::move_constructible<Controller> && std::equality_comparable<Controller>);
    }
    PlatformViewFactoryRegistration erased = std::move(factory).Erase(*adapter_);
    RegisterViewValue(std::move(name), typeid(Properties), typeid(Controller), std::move(erased));
  }

  template <class Module> [[nodiscard]] Module OpenModule(std::string name) {
    return OpenModuleValue(std::move(name), typeid(Module), typeid(void), nullptr).template Take<Module>();
  }

  template <class Module, class Options> [[nodiscard]] Module OpenModule(std::string name, Options options) {
    PlatformValue value = PlatformValue::Store(std::move(options));
    return OpenModuleValue(std::move(name), typeid(Module), typeid(Options), &value).template Take<Module>();
  }

  template <class Factory>
  [[nodiscard]] std::shared_ptr<const Factory> FindView(std::string_view name, std::type_index properties_type,
                                                        std::type_index controller_type) const {
    PlatformViewFactoryRegistration factory = FindViewValue(name, properties_type, controller_type, typeid(Factory));
    return std::static_pointer_cast<const Factory>(std::move(factory.factory));
  }

  void Freeze() noexcept {
    frozen_ = true;
  }

  PlatformRegistry(const PlatformRegistry&) = delete;
  PlatformRegistry& operator=(const PlatformRegistry&) = delete;

private:
  enum class Kind { Module, View };

  class ModuleInstance final {
  public:
    template <class T> explicit ModuleInstance(T value) : value_(std::make_unique<Model<T>>(std::move(value))) {}

    template <class T> T Take() {
      if (!value_ || value_->Type() != typeid(T)) {
        throw std::logic_error("HuxerUI platform module factory returned an incompatible type");
      }
      auto* model = static_cast<Model<T>*>(value_.get());
      T result = std::move(model->value);
      value_.reset();
      return result;
    }

  private:
    class Concept {
    public:
      virtual ~Concept() = default;
      [[nodiscard]] virtual std::type_index Type() const noexcept = 0;
    };

    template <class T> class Model final : public Concept {
    public:
      explicit Model(T value) : value(std::move(value)) {}

      [[nodiscard]] std::type_index Type() const noexcept override {
        return typeid(T);
      }

      T value;
    };

    std::unique_ptr<Concept> value_;
  };

  class Registration {
  public:
    Registration(Kind kind, std::type_index primary_type, std::type_index secondary_type)
        : kind(kind), primary_type(primary_type), secondary_type(secondary_type) {}
    virtual ~Registration() = default;

    Kind kind;
    std::type_index primary_type;
    std::type_index secondary_type;
  };

  class ModuleRegistration : public Registration {
  public:
    ModuleRegistration(std::type_index module_type, std::type_index options_type)
        : Registration(Kind::Module, module_type, options_type) {}
    [[nodiscard]] virtual ModuleInstance Open(PlatformAdapter& adapter, const PlatformValue* options) = 0;
  };

  template <class Module, class Options, class Factory>
  class ModuleRegistrationModel final : public ModuleRegistration {
  public:
    explicit ModuleRegistrationModel(Factory factory)
        : ModuleRegistration(typeid(Module), typeid(Options)), factory_(std::move(factory)) {}

    [[nodiscard]] ModuleInstance Open(PlatformAdapter& adapter, const PlatformValue* options) override {
      if constexpr (std::same_as<Options, void>) {
        static_cast<void>(options);
        return ModuleInstance(Module(std::invoke(factory_, adapter)));
      } else {
        const Options& value = options->Get<Options>();
        return ModuleInstance(Module(std::invoke(factory_, adapter, value)));
      }
    }

  private:
    Factory factory_;
  };

  class ViewRegistration final : public Registration {
  public:
    ViewRegistration(std::type_index properties_type, std::type_index controller_type,
                     PlatformViewFactoryRegistration factory)
        : Registration(Kind::View, properties_type, controller_type), factory(std::move(factory)) {}

    PlatformViewFactoryRegistration factory;
  };

  template <class Module, class Options, class Factory> void RegisterModuleImpl(std::string name, Factory factory) {
    static_assert(std::move_constructible<Module>);
    if constexpr (std::same_as<Options, void>) {
      static_assert(std::invocable<Factory&, PlatformAdapter&>,
                    "HuxerUI PlatformModule factory must accept PlatformAdapter&");
    } else {
      static_assert(std::invocable<Factory&, PlatformAdapter&, const Options&>,
                    "HuxerUI PlatformModule factory must accept PlatformAdapter& and its Options");
    }
    RegisterValue(std::move(name),
                  std::make_unique<ModuleRegistrationModel<Module, Options, Factory>>(std::move(factory)));
  }

  void RegisterValue(std::string name, std::unique_ptr<Registration> registration);
  void RegisterViewValue(std::string name, std::type_index properties_type, std::type_index controller_type,
                         PlatformViewFactoryRegistration factory);
  [[nodiscard]] ModuleInstance OpenModuleValue(std::string name, std::type_index module_type,
                                               std::type_index options_type, const PlatformValue* options);
  [[nodiscard]] PlatformViewFactoryRegistration FindViewValue(std::string_view name, std::type_index properties_type,
                                                              std::type_index controller_type,
                                                              std::type_index factory_type) const;

  PlatformAdapter* adapter_;
  std::unordered_map<std::string, std::unique_ptr<Registration>> registrations_;
  bool frozen_ = false;
};

PlatformRegistry* CurrentLifecyclePlatformRegistry() noexcept;
PlatformRegistry* SetLifecyclePlatformRegistry(PlatformRegistry* registry) noexcept;

} // namespace detail

/// Publishes events from a mounted platform implementation to the declaring HuxerUI PlatformView.
///
/// Direct C++ factories should use the typed Emit<Key> overload so event values remain strongly typed. A
/// cross-language bridge may use the named PlatformPayload overload after decoding the platform envelope. PlatformView
/// emission is synchronous on its owning UI thread and ignored before mount, after detach, or from another thread.
/// PlatformChannel-backed Module notifications remain asynchronous and do not return values.
///
/// The event key must declare a stable Name and a complete signature with zero or one argument:
/// @code
/// struct TextChanged : Event<void(std::string)> {
///   static constexpr std::string_view Name = "textChanged";
/// };
///
/// events.Emit<TextChanged>("Updated text");
/// @endcode
class PlatformEventEmitter final {
public:
  /// Creates a disconnected emitter. Emitting through it is a no-op.
  PlatformEventEmitter() = default;

  /// Emits a typed event with no argument.
  template <detail::PlatformEventKey Key>
    requires(detail::PlatformEventSignature<typename Key::Signature>::argument_count == 0)
  auto Emit() const {
    using Result = typename detail::PlatformEventSignature<typename Key::Signature>::ResultType;
    if constexpr (std::is_void_v<Result>) {
      static_cast<void>(EmitValue(typeid(Key), {}));
    } else {
      const std::optional<PlatformValue> result = EmitValue(typeid(Key), {});
      return result.has_value() ? std::optional<Result>{result->Get<Result>()} : std::nullopt;
    }
  }

  /// Emits a typed event whose value exactly matches Key::Signature.
  template <detail::PlatformEventKey Key, class Value>
    requires(detail::PlatformEventSignature<typename Key::Signature>::argument_count == 1 &&
             std::same_as<std::remove_cvref_t<Value>,
                          typename detail::PlatformEventSignature<typename Key::Signature>::Value>)
  auto Emit(Value&& value) const {
    using Result = typename detail::PlatformEventSignature<typename Key::Signature>::ResultType;
    if constexpr (std::is_void_v<Result>) {
      static_cast<void>(EmitValue(typeid(Key), PlatformValue::Store(std::forward<Value>(value))));
    } else {
      const std::optional<PlatformValue> result =
          EmitValue(typeid(Key), PlatformValue::Store(std::forward<Value>(value)));
      return result.has_value() ? std::optional<Result>{result->Get<Result>()} : std::nullopt;
    }
  }

  /// Emits a named cross-language event carrying an already decoded PlatformPayload and returns its optional result.
  std::optional<PlatformPayload> Emit(std::string name, PlatformPayload payload) const;

private:
  PlatformEventEmitter(std::function<std::optional<PlatformValue>(std::type_index, PlatformValue)> emit_direct,
                       std::function<std::optional<PlatformPayload>(std::string, PlatformPayload)> emit_payload)
      : emit_direct_(std::move(emit_direct)), emit_payload_(std::move(emit_payload)) {}

  std::optional<PlatformValue> EmitValue(std::type_index key, PlatformValue value) const;

  std::function<std::optional<PlatformValue>(std::type_index, PlatformValue)> emit_direct_;
  std::function<std::optional<PlatformPayload>(std::string, PlatformPayload)> emit_payload_;

  friend PlatformEventEmitter detail::MakePlatformEventEmitter(
      std::function<std::optional<PlatformValue>(std::type_index, PlatformValue)>,
      std::function<std::optional<PlatformPayload>(std::string, PlatformPayload)>
  );
  friend class detail::PlatformChannelEndpoint;
};

/// A shared asynchronous request and event channel for a cross-language platform instance.
///
/// PlatformChannel is a bridge convenience, not a PlatformModule base class. A library normally wraps it in its own
/// strongly typed C++ service or Controller. Invocations and event delivery are serialized through the owning
/// PlatformAdapter's UI dispatcher. Cancel invalidates a request before asking the platform implementation to cancel,
/// Close rejects new work, cancels pending requests, detaches events, and disposes the platform instance.
///
/// Primitive C++ values are encoded automatically. Structured argument, result, and event types provide static
/// Encode/Decode operations at their type definition:
/// @code
/// channel.Invoke<std::string>(
///     "readTitle",
///     std::monostate{},
///     [](PlatformResult<std::string> result) { /* consume result */ }
/// );
/// channel.On<ProgressChanged>([](double progress) { /* update state */ });
/// @endcode
class PlatformChannel final {
public:
  /// Creates a closed channel.
  PlatformChannel() = default;

  /// Returns true while the underlying platform instance accepts requests and events.
  [[nodiscard]] bool IsOpen() const noexcept;
  /// Invokes a platform method using raw PlatformPayload values.
  ///
  /// The returned request identity may be passed to Cancel. Completion is called at most once on the owning UI thread.
  PlatformRequestId Invoke(std::string method, PlatformPayload arguments,
                           std::function<void(PlatformResult<PlatformPayload>)> completion) const;

  /// Invokes a platform method with typed arguments and result conversion.
  ///
  /// Invalid result payloads complete with the `huxerui/invalid-result` PlatformError instead of escaping an exception
  /// across the platform boundary.
  template <class Result, detail::PlatformPayloadEncodable Arguments, class Completion>
    requires detail::PlatformPayloadDecodable<Result> &&
             std::constructible_from<std::function<void(PlatformResult<Result>)>, Completion>
  PlatformRequestId Invoke(std::string method, const Arguments& arguments, Completion&& completion) const {
    std::function<void(PlatformResult<Result>)> typed_completion(std::forward<Completion>(completion));
    return Invoke(std::move(method), detail::EncodePlatformValue(arguments),
                  [completion = std::move(typed_completion)](PlatformResult<PlatformPayload> result) mutable {
                    if (const auto* error = std::get_if<PlatformError>(&result)) {
                      completion(*error);
                      return;
                    }
                    try {
                      completion(detail::DecodePlatformPayload<Result>(std::get<PlatformPayload>(result)));
                    } catch (...) {
                      completion(PlatformError{
                          "huxerui/invalid-result",
                          "HuxerUI platform call returned an invalid result payload",
                          {},
                      });
                    }
                  });
  }

  /// Invokes a method whose argument and result payload contract uses Null.
  ///
  /// Result should normally be std::monostate. The platform result is validated as Null before completion runs.
  template <class Result, class Completion>
    requires detail::PlatformPayloadDecodable<Result> &&
             std::constructible_from<std::function<void(PlatformResult<Result>)>, Completion>
  PlatformRequestId Invoke(std::string method, Completion&& completion) const {
    return Invoke<Result>(std::move(method), std::monostate{}, std::forward<Completion>(completion));
  }

  /// Subscribes to a named event using its raw PlatformPayload value.
  void On(std::string event, std::function<void(const PlatformPayload&)> handler) const;

  /// Subscribes to a typed event and decodes its payload according to Key::Signature.
  template <detail::PlatformPayloadEventKey Key, class Handler> void On(Handler&& handler) const {
    std::function<typename Key::Signature> typed_handler(std::forward<Handler>(handler));
    On(std::string(std::string_view(Key::Name)),
       [handler = std::move(typed_handler)](const PlatformPayload& payload) mutable {
         using Signature = detail::PlatformEventSignature<typename Key::Signature>;
         if constexpr (Signature::argument_count == 0) {
           if (!payload.IsNull()) {
             throw std::invalid_argument("HuxerUI fieldless platform event payload must be null");
           }
           handler();
         } else {
           handler(detail::DecodePlatformPayload<typename Signature::Value>(payload));
         }
       });
  }

  /// Invalidates a pending request and invokes its platform cancellation operation when one exists.
  ///
  /// Returns false when the request is unknown, already completed, or already cancelled.
  bool Cancel(PlatformRequestId request) const;
  /// Closes the shared channel state. Copies of this PlatformChannel observe the same closure.
  void Close() const noexcept;

private:
  explicit PlatformChannel(std::shared_ptr<detail::PlatformChannelState> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::PlatformChannelState> state_;

  friend class detail::PlatformChannelEndpoint;
};

/// Opens a registered PlatformModule from committed Lifecycle setup.
///
/// This helper is intended for component-owned Lifecycle instances. Root installers may instead use the equivalent
/// RootContext member. The requested Module type and registration name must exactly match the registered factory.
///
/// Example:
/// @code
/// Lifecycle([] {
///   auto timer = OpenPlatformModule<std::shared_ptr<TimerService>>("example/Timer");
///   return [timer = std::move(timer)]() mutable { timer.reset(); };
/// });
/// @endcode
template <class Module> Module OpenPlatformModule(std::string name) {
  detail::PlatformRegistry* registry = detail::CurrentLifecyclePlatformRegistry();
  if (registry == nullptr) {
    throw std::logic_error("HuxerUI OpenPlatformModule must be called from committed Lifecycle setup");
  }
  return registry->template OpenModule<Module>(std::move(name));
}

/// Opens a registered PlatformModule with strongly typed construction options from committed Lifecycle setup.
template <class Module, class Options> Module OpenPlatformModule(std::string name, Options options) {
  detail::PlatformRegistry* registry = detail::CurrentLifecyclePlatformRegistry();
  if (registry == nullptr) {
    throw std::logic_error("HuxerUI OpenPlatformModule must be called from committed Lifecycle setup");
  }
  return registry->template OpenModule<Module, Options>(std::move(name), std::move(options));
}

} // namespace huxerui
