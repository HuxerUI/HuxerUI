#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <utility>

#include <huxerui/platform_registry.h>

typedef struct _GtkWidget GtkWidget;
typedef struct _GtkWindow GtkWindow;

namespace huxerui {

class WindowHandle;

namespace linux {

/// Borrows the GtkWindow on the GTK application thread. HuxerUI owns window destruction.
/// @param window Handle for the window to query.
/// @return The GtkWindow without acquiring a GObject reference, or nullptr if there is no live GTK attachment.
/// Query again after deferred work.
/// @throws std::logic_error If called off the original live application's thread.
GtkWindow* GetPlatformWindow(const WindowHandle& window);

/// Describes the GTK lifecycle of one registered PlatformView type.
///
/// All callbacks run on the GTK application thread. The GtkWindow passed to `create` is borrowed.
/// `create` and `view` are required. `view` is called once and returns an unparented GtkWidget held by a
/// strong GObject reference in Instance. HuxerUI parents the widget and controls its placement and visibility.
/// A widget with its own GdkSurface, such as GtkWindow, cannot be a PlatformView root.
///
/// When Properties is not void, `update` applies each new controlled property revision. When Controller is not
/// void, both `connect` and `disconnect` are required. Optional `dispose` runs after Controller disconnection
/// and widget removal, while Instance still holds the widget.
///
/// @tparam Properties Controlled declarative value, or void for a View without properties.
/// @tparam Instance Retained C++ state that owns the GtkWidget and its signal connections.
/// @tparam Controller Optional typed command endpoint connected while the View is mounted.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;

namespace detail {

/// Type-erased GTK factory consumed by the Linux PlatformView host.
struct GtkViewFactory {
  std::function<std::shared_ptr<void>(GtkWindow*, const PlatformValue&, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<void>&)> view;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(const std::shared_ptr<void>&)> dispose;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
};

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(linux::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source =
      std::make_shared<linux::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<GtkViewFactory>();
  if (source->create) {
    factory->create = [source](GtkWindow* window, const PlatformValue& properties, PlatformEventEmitter events) {
      if constexpr (std::same_as<Properties, void>) {
        return std::static_pointer_cast<void>(source->create(window, std::move(events)));
      } else {
        return std::static_pointer_cast<void>(source->create(window, properties.Get<Properties>(), std::move(events)));
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

/// GTK PlatformView factory without declarative Properties or a Controller.
template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(GtkWindow*, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// GTK PlatformView factory with a Controller and no declarative Properties.
template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(GtkWindow*, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;
  std::function<void(Instance&, const Controller&)> connect;
  std::function<void(Instance&, const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// GTK PlatformView factory with controlled Properties and no Controller.
template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(GtkWindow*, const Properties&, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// GTK PlatformView factory with controlled Properties and a Controller.
template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(GtkWindow*, const Properties&, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<Instance>&)> view;
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

} // namespace linux

} // namespace huxerui
