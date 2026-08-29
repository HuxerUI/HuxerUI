#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <huxerui/platform_registry.h>

#if defined(__OBJC__)
@class UIView;
@class UIViewController;
#else
class UIView;
class UIViewController;
#endif

namespace huxerui::ios {

/// Describes the UIKit lifecycle of one registered PlatformView type.
///
/// Properties is the complete controlled value supplied by `PlatformView(name, properties)`. Instance is arbitrary
/// library-owned retained state and commonly stores the UIView plus delegates, targets, and event resources. Controller
/// is optional and connects a strongly typed C++ command facade to the mounted instance.
///
/// `create` and `view` are required. `create` receives the owning UIViewController, and `view` is called once to obtain
/// the stable UIView retained by Instance. When Properties is not void, `update` must apply later property revisions.
/// When Controller is not void, both `connect` and `disconnect` are required. `dispose` is optional and runs after
/// Controller disconnection. Callbacks execute on the UIKit main thread.
///
/// Example:
/// @code
/// ios::PlatformViewFactory<MapProperties, MapInstance, MapController> factory{
///     .create = CreateMap,
///     .view = [](const std::shared_ptr<MapInstance>& instance) { return instance->view; },
///     .update = UpdateMap,
///     .dispose = DisposeMap,
///     .connect = ConnectMapController,
///     .disconnect = DisconnectMapController,
/// };
/// root.RegisterPlatformView<MapProperties, MapController>("Map", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled declarative state, or void when the View has no properties.
/// @tparam Instance Retained C++ or Objective-C++ state whose shared lifetime covers the mounted UIView.
/// @tparam Controller Optional strongly typed command endpoint connected for the mounted View.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;

namespace detail {

/// Type-erased UIKit factory consumed by the iOS PlatformView host.
struct UIKitViewFactory {
  std::function<std::shared_ptr<void>(UIViewController*, const PlatformValue&, PlatformEventEmitter)> create;
  std::function<UIView*(const std::shared_ptr<void>&)> view;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(const std::shared_ptr<void>&)> dispose;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
};

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(ios::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source = std::make_shared<ios::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<UIKitViewFactory>();
  if (source->create) {
    factory->create = [source](UIViewController* owner, const PlatformValue& properties, PlatformEventEmitter events) {
      if constexpr (std::same_as<Properties, void>) {
        return std::static_pointer_cast<void>(source->create(owner, std::move(events)));
      } else {
        return std::static_pointer_cast<void>(source->create(owner, properties.Get<Properties>(), std::move(events)));
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

/// UIKit PlatformView factory without declarative Properties or a Controller.
template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(UIViewController*, PlatformEventEmitter)> create;
  std::function<UIView*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// UIKit PlatformView factory with a Controller and no declarative Properties.
template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(UIViewController*, PlatformEventEmitter)> create;
  std::function<UIView*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;
  std::function<void(Instance&, const Controller&)> connect;
  std::function<void(Instance&, const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// UIKit PlatformView factory with controlled Properties and no Controller.
template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(UIViewController*, const Properties&, PlatformEventEmitter)> create;
  std::function<UIView*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// UIKit PlatformView factory with controlled Properties and a Controller.
template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(UIViewController*, const Properties&, PlatformEventEmitter)> create;
  std::function<UIView*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;
  std::function<void(Instance&, const Controller&)> connect;
  std::function<void(Instance&, const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

} // namespace huxerui::ios
