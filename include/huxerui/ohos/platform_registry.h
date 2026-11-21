#pragma once

#include <arkui/native_node.h>

#include <concepts>
#include <functional>
#include <memory>
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
};

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
