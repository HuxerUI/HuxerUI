#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <huxerui/platform_registry.h>

#if !defined(NOMINMAX)
#define NOMINMAX
#define HUXERUI_RESTORE_NOMINMAX
#endif
#include <windows.h>
#if defined(HUXERUI_RESTORE_NOMINMAX)
#undef HUXERUI_RESTORE_NOMINMAX
#undef NOMINMAX
#endif

namespace huxerui::windows {

/// Describes the Win32 lifecycle of one registered PlatformView type.
///
/// Properties is the complete controlled value supplied by `PlatformView(name, properties)`. Instance is arbitrary
/// library-owned retained state and commonly stores the child HWND plus event resources. Controller is optional and
/// connects a strongly typed C++ command facade to the mounted instance.
///
/// `create` and `view` are required. `view` is called once after creation and must return the stable child HWND owned
/// by Instance. When Properties is not void, `update` must apply later property revisions. When Controller is not void,
/// both `connect` and `disconnect` are required. `dispose` is optional and runs after Controller disconnection.
///
/// Example:
/// @code
/// windows::PlatformViewFactory<TextFieldProperties, TextFieldInstance> factory{
///     .create = CreateTextField,
///     .view = [](const std::shared_ptr<TextFieldInstance>& instance) { return instance->window; },
///     .update = UpdateTextField,
///     .dispose = DisposeTextField,
/// };
/// root.RegisterPlatformView<TextFieldProperties>("TextField", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled declarative state, or void when the View has no properties.
/// @tparam Instance Retained C++ state whose shared lifetime covers the mounted child HWND.
/// @tparam Controller Optional strongly typed command endpoint connected for the mounted View.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;

namespace detail {

/// Type-erased Win32 factory consumed by the Windows PlatformView host.
struct Win32ViewFactory {
  std::function<std::shared_ptr<void>(HWND, const PlatformValue&, PlatformEventEmitter)> create;
  std::function<HWND(const std::shared_ptr<void>&)> view;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(const std::shared_ptr<void>&)> dispose;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
};

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(windows::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source =
      std::make_shared<windows::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<Win32ViewFactory>();
  if (source->create) {
    factory->create = [source](HWND parent, const PlatformValue& properties, PlatformEventEmitter events) {
      if constexpr (std::same_as<Properties, void>) {
        return std::static_pointer_cast<void>(source->create(parent, std::move(events)));
      } else {
        return std::static_pointer_cast<void>(source->create(parent, properties.Get<Properties>(), std::move(events)));
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

/// Win32 PlatformView factory without declarative Properties or a Controller.
template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(HWND, PlatformEventEmitter)> create;
  std::function<HWND(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// Win32 PlatformView factory with a Controller and no declarative Properties.
template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(HWND, PlatformEventEmitter)> create;
  std::function<HWND(const std::shared_ptr<Instance>&)> view;
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

/// Win32 PlatformView factory with controlled Properties and no Controller.
template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(HWND, const Properties&, PlatformEventEmitter)> create;
  std::function<HWND(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// Win32 PlatformView factory with controlled Properties and a Controller.
template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(HWND, const Properties&, PlatformEventEmitter)> create;
  std::function<HWND(const std::shared_ptr<Instance>&)> view;
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

} // namespace huxerui::windows
