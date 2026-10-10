#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <huxerui/platform_registry.h>

typedef struct _GtkWidget GtkWidget;

namespace huxerui::linux {

/// GTK4 factory contract for one native HuxerUI PlatformView.
///
/// The supplied parent is a clipping GtkFixed owned by HuxerUI. `create` adds its root widget to that parent;
/// `view` must return that same child widget. The host controls its allocation, stacking, and lifetime. Creation,
/// update, disposal, focus, and event delivery run on the GTK application thread.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;

namespace detail {

struct LinuxViewFactory {
  std::function<std::shared_ptr<void>(GtkWidget*, const PlatformValue&, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<void>&)> view;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(const std::shared_ptr<void>&)> dispose;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
};

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(linux::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source = std::make_shared<linux::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<LinuxViewFactory>();
  if (source->create) {
    factory->create = [source](GtkWidget* parent, const PlatformValue& properties, PlatformEventEmitter events) {
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

template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(GtkWidget*, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(GtkWidget*, PlatformEventEmitter)> create;
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

template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(GtkWidget*, const Properties&, PlatformEventEmitter)> create;
  std::function<GtkWidget*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow&) && {
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(GtkWidget*, const Properties&, PlatformEventEmitter)> create;
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

} // namespace huxerui::linux
