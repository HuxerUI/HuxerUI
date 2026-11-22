#pragma once

#include <arkui/native_node.h>

#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/platform_registry.h>

namespace huxerui::ohos {

/// Describes the ArkUI lifecycle of one registered PlatformView type.
///
/// All callbacks run on the ArkUI application thread. The required `create` callback receives the borrowed
/// node API, an event emitter and, when Properties is not void, the initial controlled value. The required
/// `view` callback is called once per mounted instance and returns the stable, unparented ArkUI node it owns.
/// HuxerUI parents the node, controls its size, position, and visibility, and registers its focus and blur events.
///
/// When Properties is not void, `update` is required and applies each subsequent property revision. When Controller
/// is not void, both `connect` and `disconnect` are required. Optional `dispose` runs after Controller disconnection
/// and node removal, while Instance still owns the node.
///
/// @code
/// ohos::PlatformViewFactory<TextProperties, TextInstance> factory{
///     .create = CreateTextView,
///     .view = [](const std::shared_ptr<TextInstance>& instance) { return instance->view.get(); },
///     .update = UpdateTextView,
/// };
/// context.RegisterPlatformView<TextProperties>("TextView", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled declarative value, or void for a View without properties.
/// @tparam Instance Retained C++ state that owns the ArkUI node and its event registrations.
/// @tparam Controller Optional typed command endpoint connected while the View is mounted.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;

namespace detail {

/// Type-erased ArkUI factory consumed by the HarmonyOS PlatformView host.
struct ArkUiViewFactory {
  std::function<std::shared_ptr<void>(ArkUI_NativeNodeAPI_1&, const PlatformValue&, PlatformEventEmitter)> create;
  std::function<ArkUI_NodeHandle(const std::shared_ptr<void>&)> view;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(const std::shared_ptr<void>&)> dispose;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
  std::function<void(const std::shared_ptr<void>&)> commit;
};

PlatformChannel OpenArkTsModule(Runtime& runtime, const std::string& name, PlatformPayload options);
PlatformChannel OpenArkTsModule(UiWindow& window, const std::string& name, PlatformPayload options);

huxerui::detail::PlatformViewFactoryRegistration PrepareArkTsView(UiWindow& window, const std::string& name,
    std::function<PlatformPayload(const PlatformValue&)> encode,
    std::function<void(const PlatformValue&, PlatformChannel)> connect,
    std::function<void(const PlatformValue&)> disconnect);

template <class Module, class Options, class Host> struct ArkTsModuleFactory {
  /// Name resolved by the originating ability's ArkTS extension object.
  std::string factory_name;
  /// Constructs the library's typed facade from the opened channel on the application thread.
  std::function<Module(PlatformChannel)> create;

  Module operator()(Host& host) requires std::same_as<Options, void> { return Open(host, {}); }

  template <class Value = Options>
    requires (!std::same_as<Options, void> && std::same_as<Value, Options>)
  Module operator()(Host& host, const Value& options) {
    return Open(host, huxerui::detail::EncodePlatformValue(options));
  }

private:
  Module Open(Host& host, PlatformPayload options) {
    if (!create) throw std::invalid_argument("HuxerUI ArkTS module factory requires a C++ facade constructor");
    auto channel = OpenArkTsModule(host, factory_name, std::move(options));
    try {
      return create(channel);
    } catch (...) {
      channel.Close();
      throw;
    }
  }
};

template <class Properties>
std::function<PlatformPayload(const PlatformValue&)> ArkTsViewEncoder() {
  if constexpr (std::same_as<Properties, void>) return {};
  else return [](const PlatformValue& value) { return huxerui::detail::EncodePlatformValue(value.Get<Properties>()); };
}

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(ohos::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source =
      std::make_shared<ohos::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<ArkUiViewFactory>();
  if (source->create) {
    factory->create = [source](ArkUI_NativeNodeAPI_1& node_api, const PlatformValue& properties, PlatformEventEmitter events) {
      if constexpr (std::same_as<Properties, void>) {
        return std::static_pointer_cast<void>(source->create(node_api, std::move(events)));
      } else {
        return std::static_pointer_cast<void>(source->create(node_api, properties.Get<Properties>(), std::move(events)));
      }
    };
  }
  if (source->view) {
    factory->view = [source](const std::shared_ptr<void>& instance) {
      return source->view(std::static_pointer_cast<Instance>(instance));
    };
  }
  if constexpr (!std::same_as<Properties, void>) {
    if (source->update) {
      factory->update = [source](const std::shared_ptr<void>& instance, const PlatformValue& properties) {
        source->update(*std::static_pointer_cast<Instance>(instance), properties.Get<Properties>());
      };
    }
  }
  if (source->dispose) {
    factory->dispose = [source](const std::shared_ptr<void>& instance) {
      source->dispose(*std::static_pointer_cast<Instance>(instance));
    };
  }
  if constexpr (!std::same_as<Controller, void>) {
    if (source->connect) {
      factory->connect = [source](const std::shared_ptr<void>& instance, const PlatformValue& controller) {
        source->connect(*std::static_pointer_cast<Instance>(instance), controller.Get<Controller>());
      };
    }
    if (source->disconnect) {
      factory->disconnect = [source](const std::shared_ptr<void>& instance, const PlatformValue& controller) {
        source->disconnect(*std::static_pointer_cast<Instance>(instance), controller.Get<Controller>());
      };
    }
  }
  return huxerui::detail::MakePlatformViewFactoryRegistration(std::move(factory));
}

} // namespace detail

/// Opens an ArkTS PlatformModule from the originating ability, including when no window is attached.
/// @tparam Module Exact C++ facade constructed from the channel for each opening.
/// @tparam Options Optional construction type encoded through PlatformPayload, or void for Null.
template <class Module, class Options = void>
using ArkTsPlatformModuleFactory = detail::ArkTsModuleFactory<Module, Options, Runtime>;

/// Opens an ArkTS PlatformModule with the original Window and UIContext of the calling attachment.
/// Requests and events are scoped to that attachment. Options and the C++ facade follow ArkTsPlatformModuleFactory.
template <class Module, class Options = void>
using ArkTsWindowPlatformModuleFactory = detail::ArkTsModuleFactory<Module, Options, UiWindow>;

/// Registers an ArkTS factory by the name resolved by the ability's platform extensions.
/// Callbacks run on the ArkUI application thread. Events emitted during creation are delivered after the view commits.
/// @tparam Properties Controlled declarative value encoded with PlatformPayload, or void for a propertyless View.
/// @tparam Controller Typed command endpoint connected to the mounted instance, or void.
template <class Properties, class Controller = void> struct ArkTsPlatformViewFactory {
  /// Name used by the ability's ArkTS view resolver.
  std::string factory_name;
  /// Binds the caller's Controller to the committed View's method channel.
  std::function<void(const Controller&, PlatformChannel)> connect;
  /// Clears the caller's Controller on replacement or removal. HuxerUI closes the retired instance's channel.
  std::function<void(const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& window) && {
    if (!connect || !disconnect) throw std::invalid_argument("HuxerUI ArkTS view controller requires both callbacks");
    return detail::PrepareArkTsView(window, factory_name, detail::ArkTsViewEncoder<Properties>(),
        [callback = std::move(connect)](const PlatformValue& value, PlatformChannel channel) {
          callback(value.Get<Controller>(), std::move(channel));
        }, [callback = std::move(disconnect)](const PlatformValue& value) { callback(value.Get<Controller>()); });
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// Registers an ArkTS view factory without a C++ controller.
template <class Properties> struct ArkTsPlatformViewFactory<Properties, void> {
  /// Name used by the ability's ArkTS view resolver.
  std::string factory_name;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& window) && {
    return detail::PrepareArkTsView(window, factory_name, detail::ArkTsViewEncoder<Properties>(), {}, {});
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// ArkUI PlatformView factory without declarative Properties or a Controller.
template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(ArkUI_NativeNodeAPI_1&, PlatformEventEmitter)> create;
  std::function<ArkUI_NodeHandle(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// ArkUI PlatformView factory with a Controller and no declarative Properties.
template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(ArkUI_NativeNodeAPI_1&, PlatformEventEmitter)> create;
  std::function<ArkUI_NodeHandle(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;
  std::function<void(Instance&, const Controller&)> connect;
  std::function<void(Instance&, const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// ArkUI PlatformView factory with controlled Properties and no Controller.
template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(ArkUI_NativeNodeAPI_1&, const Properties&, PlatformEventEmitter)> create;
  std::function<ArkUI_NodeHandle(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// ArkUI PlatformView factory with controlled Properties and a Controller.
template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(ArkUI_NativeNodeAPI_1&, const Properties&, PlatformEventEmitter)> create;
  std::function<ArkUI_NodeHandle(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;
  std::function<void(Instance&, const Controller&)> connect;
  std::function<void(Instance&, const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

} // namespace huxerui::ohos
