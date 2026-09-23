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
#include <huxerui/file.h>

namespace huxerui {

class UiWindow;
class Runtime;
class PlatformChannel;
class PlatformEventEmitter;

/// Identifies the exact value kind stored by a PlatformPayload.
///
/// Kinds are preserved by the HUXP binary representation. In particular, Integer and Double remain distinct, Bytes
/// are not interpreted as text, and retained resources remain opaque framework capabilities.
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
  FileReference,
  BufferReference,
};

/// A structurally immutable dynamic value used when data crosses a platform-language boundary.
/// Retained capabilities may refer to mutable external resources; BufferReference is not a byte snapshot.
///
/// Direct C++ PlatformModule and PlatformView implementations receive their concrete C++ types and do not need this
/// class. Java, Swift, Objective-C, JavaScript, and similar bridges use PlatformPayload to exchange null, scalar,
/// collection, byte, ExternalTexture, FileReference, and BufferReference values without JSON coercion.
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
  PlatformPayload(FileReference value);
  PlatformPayload(BufferReference value);

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
  [[nodiscard]] const FileReference& AsFileReference() const;
  /// Borrows a retained memory range without copying its bytes. The stored kind must be BufferReference.
  /// @return The reference, whose storage may change only under producer/consumer synchronization.
  [[nodiscard]] const BufferReference& AsBufferReference() const;

  /// Compares values by kind and contents. Object insertion order does not affect equality, and capabilities compare
  /// as the same retained capability only when they share their backing state.
  bool operator==(const PlatformPayload& other) const;

  /// Holds one encoded HUXP value and its strongly typed retained capability tables.
  ///
  /// Capability indices in bytes address these tables, so all members must cross a platform boundary together.
  struct Envelope {
    Bytes bytes;
    std::vector<std::shared_ptr<ExternalTexture>> external_textures;
    std::vector<FileReference> file_references;
    std::vector<BufferReference> buffer_references;
  };

  /// Encodes this value and its retained capabilities to one transport envelope.
  [[nodiscard]] Envelope Encode() const;
  /// Decodes one complete HUXP envelope and validates its representation and capability slots.
  [[nodiscard]] static PlatformPayload Decode(const Envelope& envelope);

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
  } else if constexpr (std::same_as<Value, FileReference>) {
    return payload.AsFileReference();
  } else if constexpr (std::same_as<Value, BufferReference>) {
    return payload.AsBufferReference();
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
                       std::same_as<Value, std::shared_ptr<ExternalTexture>> ||
                       std::same_as<Value, FileReference> || std::same_as<Value, BufferReference>) {
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
    std::same_as<Value, FileReference> || std::same_as<Value, BufferReference> || requires(const Value& value) {
      { Value::Encode(value) } -> std::same_as<PlatformPayload>;
    };

template <class Value>
concept PlatformPayloadDecodable =
    std::same_as<Value, std::monostate> || std::same_as<Value, bool> || std::integral<Value> ||
    std::floating_point<Value> || std::same_as<Value, std::string> || std::same_as<Value, Bytes> ||
    std::same_as<Value, std::shared_ptr<ExternalTexture>> || std::same_as<Value, FileReference> ||
    std::same_as<Value, BufferReference> || requires(const PlatformPayload& payload) {
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
/// Creates an asynchronous channel endpoint bound to one window's execution lifetime.
/// @param ui_window Live original window, accessed on the application thread.
/// @return An endpoint whose delivery preserves this window context and stops when it retires.
PlatformChannelEndpoint MakePlatformChannelEndpoint(UiWindow& ui_window);
/// Creates an application channel that does not require a window.
/// @param runtime Live original Runtime, accessed on its application thread.
/// @return An endpoint whose delivery uses application context and stops at Runtime shutdown.
PlatformChannelEndpoint MakePlatformChannelEndpoint(Runtime& runtime);
/// Resolves the window associated with the current composition or captured execution context.
/// @return The borrowed original window, or null for application-only context; never chooses a replacement window.
/// @throws std::logic_error If an explicitly captured window or its Runtime is no longer valid.
UiWindow* CurrentUiWindow();

PlatformEventEmitter MakePlatformEventEmitter(
    std::function<std::optional<PlatformValue>(std::type_index, PlatformValue)> emit_direct,
    std::function<std::optional<PlatformPayload>(std::string, PlatformPayload)> emit_payload
);

/// Application-owned factory catalog populated during application_hooks and frozen before ordinary dispatch begins.
/// Module factories declare their lifetime through exactly one Runtime& or UiWindow& parameter. Native View factories
/// are prepared for the mounting window. This catalog is accessed on the application thread, even after freezing.
class PlatformRegistry final {
public:
  explicit PlatformRegistry(Runtime& runtime) : runtime_(&runtime) {}

  /// Registers a module factory without construction options.
  /// @tparam Module Exact movable result type returned by the factory.
  /// @tparam Factory Callable accepting exactly one of Runtime& and UiWindow&.
  /// @param name Nonempty UTF-8 identifier unique across module and View registrations.
  /// @param factory Callable retained by this catalog; invoked when the module is opened.
  template <class Module, class Factory> void RegisterModule(std::string name, Factory factory) {
    RegisterModuleImpl<Module, void>(std::move(name), std::move(factory));
  }

  /// Registers a module factory taking typed construction options.
  /// @tparam Module Exact movable factory result.
  /// @tparam Options Exact options type required when opening the module.
  /// @tparam Factory Callable accepting Runtime& or UiWindow&, followed by const Options&.
  /// @param name Nonempty UTF-8 identifier unique across this catalog.
  /// @param factory Callable retained until application shutdown.
  template <class Module, class Options, class Factory> void RegisterModule(std::string name, Factory factory) {
    RegisterModuleImpl<Module, Options>(std::move(name), std::move(factory));
  }

  /// Retains a platform View factory for preparation against each mounting window.
  /// @tparam Properties Controlled value type, or void; non-void types must be movable and equality comparable.
  /// @tparam Controller Command handle type, or void, with the same value requirements as Properties.
  /// @tparam Factory Copyable native factory exposing Erase(UiWindow&).
  /// @param name Nonempty UTF-8 identifier unique across this catalog.
  /// @param factory Value copied before preparation so its window binding remains local to each mount.
  template <class Properties, class Controller = void, class Factory>
  void RegisterView(std::string name, Factory factory) {
    if constexpr (!std::same_as<Properties, void>) {
      static_assert(std::move_constructible<Properties> && std::equality_comparable<Properties>);
    }
    if constexpr (!std::same_as<Controller, void>) {
      static_assert(std::move_constructible<Controller> && std::equality_comparable<Controller>);
    }
    auto stored = std::make_shared<const Factory>(std::move(factory));
    RegisterViewValue(std::move(name), typeid(Properties), typeid(Controller),
                      [stored](UiWindow& ui) { return Factory(*stored).Erase(ui); });
  }

  /// Opens a new module instance using the registered host kind.
  /// @tparam Module Exact registered result type.
  /// @param name Registered identifier; this overload requires a factory with no options.
  /// @return The factory's value; opening a window factory requires a live current window.
  template <class Module> [[nodiscard]] Module OpenModule(std::string name) {
    return OpenModuleValue(std::move(name), typeid(Module), typeid(void), nullptr).template Take<Module>();
  }

  /// Opens a module with typed options borrowed only for the synchronous factory invocation.
  /// @tparam Module Exact registered result type.
  /// @tparam Options Exact registered options type.
  /// @param name Registered module identifier.
  /// @param options Construction data moved into temporary type-erased storage.
  /// @return The value produced by the matching factory.
  template <class Module, class Options> [[nodiscard]] Module OpenModule(std::string name, Options options) {
    PlatformValue value = PlatformValue::Store(std::move(options));
    return OpenModuleValue(std::move(name), typeid(Module), typeid(Options), &value).template Take<Module>();
  }

  /// Prepares a native View factory against its actual mounting window.
  /// @tparam Factory Expected backend factory representation.
  /// @param ui Live mounting window on the application thread.
  /// @param name Registered View identifier.
  /// @param properties_type Exact declared properties type, including typeid(void).
  /// @param controller_type Exact declared controller type, including typeid(void).
  /// @return A retained prepared factory; mismatched registration types throw std::logic_error.
  template <class Factory>
  [[nodiscard]] std::shared_ptr<const Factory> FindView(UiWindow& ui, std::string_view name, std::type_index properties_type,
                                                        std::type_index controller_type) const {
    PlatformViewFactoryRegistration factory = FindViewValue(ui, name, properties_type, controller_type, typeid(Factory));
    return std::static_pointer_cast<const Factory>(std::move(factory.factory));
  }

  /// Prevents further registration after application_hooks; existing factories remain available until shutdown.
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

  /// Type-erased module recipe whose concrete model chooses the declared Runtime or UiWindow host.
  class ModuleRegistration : public Registration {
  public:
    ModuleRegistration(std::type_index module_type, std::type_index options_type)
        : Registration(Kind::Module, module_type, options_type) {}
    /// Invokes this module recipe in the current application context.
    /// @param runtime Catalog owner used by application factories.
    /// @param options Null for an optionless recipe; otherwise the already type-checked construction value.
    /// @return The exact registered module value in movable erased storage.
    [[nodiscard]] virtual ModuleInstance Open(Runtime& runtime, const PlatformValue* options) = 0;
  };

  /// Selects one host kind from the factory signature without retaining a second binding table.
  /// @tparam Module Exact factory result type.
  /// @tparam Options Registered construction data type, or void.
  /// @tparam Factory Callable accepting exactly one supported host signature.
  template <class Module, class Options, class Factory>
  class ModuleRegistrationModel final : public ModuleRegistration {
  public:
    explicit ModuleRegistrationModel(Factory factory)
        : ModuleRegistration(typeid(Module), typeid(Options)), factory_(std::move(factory)) {}

    [[nodiscard]] ModuleInstance Open(Runtime& runtime, const PlatformValue* options) override {
      constexpr bool application_host = [] {
        if constexpr (std::same_as<Options, void>) {
          return std::invocable<Factory&, Runtime&>;
        } else {
          return std::invocable<Factory&, Runtime&, const Options&>;
        }
      }();
      if constexpr (application_host) {
        return Invoke(runtime, options);
      } else {
        UiWindow* ui = CurrentUiWindow();
        if (!ui) {
          throw std::logic_error("HuxerUI PlatformModule requires a live UI host");
        }
        return Invoke(*ui, options);
      }
    }

  private:
    /// Adapts the already selected host and optional erased options to the factory's exact signature.
    /// @tparam Host Runtime for application factories or UiWindow for window factories.
    /// @param host Original live host, borrowed for this invocation.
    /// @param options Null when Options is void; otherwise storage containing exactly Options.
    /// @return The movable factory result with its registered type preserved.
    template <class Host> ModuleInstance Invoke(Host& host, const PlatformValue* options) {
      if constexpr (std::same_as<Options, void>) {
        static_cast<void>(options);
        static_assert(std::same_as<std::invoke_result_t<Factory&, Host&>, Module>,
                      "HuxerUI PlatformModule factory must return exactly Module");
        return ModuleInstance(std::invoke(factory_, host));
      } else {
        const Options& value = options->Get<Options>();
        static_assert(std::same_as<std::invoke_result_t<Factory&, Host&, const Options&>, Module>,
                      "HuxerUI PlatformModule factory must return exactly Module");
        return ModuleInstance(std::invoke(factory_, host, value));
      }
    }

  private:
    Factory factory_;
  };

  class ViewRegistration final : public Registration {
  public:
    ViewRegistration(std::type_index properties_type, std::type_index controller_type,
                     std::function<PlatformViewFactoryRegistration(UiWindow&)> factory)
        : Registration(Kind::View, properties_type, controller_type), factory(std::move(factory)) {}

    std::function<PlatformViewFactoryRegistration(UiWindow&)> factory;
  };

  template <class Module, class Options, class Factory> void RegisterModuleImpl(std::string name, Factory factory) {
    static_assert(std::move_constructible<Module>);
    if constexpr (std::same_as<Options, void>) {
      static_assert(std::invocable<Factory&, UiWindow&> != std::invocable<Factory&, Runtime&>,
                    "HuxerUI PlatformModule factory must accept exactly one host type");
    } else {
      static_assert(std::invocable<Factory&, UiWindow&, const Options&> !=
                        std::invocable<Factory&, Runtime&, const Options&>,
                    "HuxerUI PlatformModule factory must accept exactly one host type and its Options");
    }
    RegisterValue(std::move(name),
                  std::make_unique<ModuleRegistrationModel<Module, Options, Factory>>(std::move(factory)));
  }

  void RegisterValue(std::string name, std::unique_ptr<Registration> registration);
  void RegisterViewValue(std::string name, std::type_index properties_type, std::type_index controller_type,
                         std::function<PlatformViewFactoryRegistration(UiWindow&)> factory);
  [[nodiscard]] ModuleInstance OpenModuleValue(std::string name, std::type_index module_type,
                                               std::type_index options_type, const PlatformValue* options);
  [[nodiscard]] PlatformViewFactoryRegistration FindViewValue(UiWindow& ui, std::string_view name, std::type_index properties_type,
                                                              std::type_index controller_type,
                                                              std::type_index factory_type) const;

  Runtime* runtime_;
  std::unordered_map<std::string, std::unique_ptr<Registration>> registrations_;
  bool frozen_ = false;
};

/// Obtains the current application's catalog for imperative module creation.
/// @return The application-owned catalog, borrowed for the current application-thread invocation.
/// @throws std::logic_error During composition or without a valid application context.
PlatformRegistry& CurrentPlatformRegistry();

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
/// strongly typed C++ service or Controller. Invocations, results, and events run on the application thread with
/// the instance's original Runtime or UiWindow context. Delivery to a retired host is discarded. Cancel invalidates
/// a request before asking the platform implementation to cancel,
/// Close rejects new work, cancels pending requests, detaches events, and disposes the platform instance.
/// Native cancellation and disposal retain their queued cleanup path after the execution context retires.
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

/// Opens a registered PlatformModule outside composition from an initialized application context.
///
/// Use from WindowHook, committed Lifecycle setup, or ordinary application work after factory installation.
/// The Module type and name must match the factory; UiWindow factories require the caller's live window context.
/// @tparam Module Exact registered factory result type.
/// @param name Registered module identifier whose factory takes no construction options.
/// @return A new module value created by its declared Runtime or UiWindow factory.
/// @throws std::logic_error During composition, without the required live host, or for a name/type mismatch.
///
/// Example:
/// @code
/// Lifecycle([] {
///   auto timer = OpenPlatformModule<std::shared_ptr<TimerService>>("example/Timer");
///   return [timer = std::move(timer)]() mutable { timer.reset(); };
/// });
/// @endcode
template <class Module> Module OpenPlatformModule(std::string name) {
  return detail::CurrentPlatformRegistry().OpenModule<Module>(std::move(name));
}

/// Opens a registered module outside composition using its exact construction-options type.
/// @tparam Module Exact result type installed through ApplicationContext::RegisterPlatformModule.
/// @tparam Options Exact registered options type; borrowed by the factory only during this call.
/// @param name Registered nonempty UTF-8 module identifier.
/// @param options Construction value moved into the invocation's temporary storage.
/// @return A new module value bound to the factory's declared Runtime or current UiWindow host.
/// @throws std::logic_error During composition, without the required live host, or for a name/type mismatch.
template <class Module, class Options> Module OpenPlatformModule(std::string name, Options options) {
  return detail::CurrentPlatformRegistry().OpenModule<Module, Options>(std::move(name), std::move(options));
}

} // namespace huxerui
