#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

#include <emscripten/val.h>

#include <huxerui/platform_registry.h>

namespace huxerui::web {

/// Describes the Emscripten lifecycle of one registered PlatformView type.
///
/// Properties is the complete controlled value supplied by `PlatformView(name, properties)`. Instance is arbitrary
/// library-owned retained state and commonly stores the JavaScript object, listener handles, and event resources.
/// Controller is optional and connects a strongly typed C++ command facade to the mounted instance.
///
/// `create` and `view` are required. `view` is called once and must return the stable, detached HTMLElement retained by
/// Instance; HuxerUI inserts it into the platform-view layer. When Properties is not void, `update` must apply later
/// property revisions. When Controller is not void, both `connect` and `disconnect` are required. `dispose` is optional
/// and runs after Controller disconnection. Callbacks execute on the browser event-loop thread.
///
/// Example:
/// @code
/// web::PlatformViewFactory<EditorProperties, EditorInstance> factory{
///     .create = CreateEditor,
///     .view = [](const std::shared_ptr<EditorInstance>& instance) { return instance->element; },
///     .update = UpdateEditor,
///     .dispose = DisposeEditor,
/// };
/// root.RegisterPlatformView<EditorProperties>("Editor", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled declarative state, or void when the View has no properties.
/// @tparam Instance Retained C++ or Emscripten state whose shared lifetime covers the mounted HTMLElement.
/// @tparam Controller Optional strongly typed command endpoint connected for the mounted View.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;

/// Adapts a JavaScript PlatformModule factory object to a strongly typed C++ Module facade.
///
/// The JavaScript factory is supplied directly by the library's RootHook; HuxerUI does not maintain a second
/// JavaScript registration table. Options cross the language boundary through PlatformPayload, while the adapter's
/// C++ create callback wraps the created instance's PlatformChannel in the library's exact Module type. The JavaScript
/// factory object provides `create(options, events)`. Its returned instance provides `invoke(method, arguments,
/// result)`, may return a cancellation function from invoke, and provides `dispose()`.
///
/// Example:
/// @code
/// web::JavaScriptPlatformModuleFactory<std::shared_ptr<TimerService>> factory{
///     .factory = emscripten::val::module_property("exampleTimerFactory"),
///     .create = [](PlatformChannel channel) {
///       return std::make_shared<WebTimerService>(std::move(channel));
///     },
/// };
/// root.RegisterPlatformModule<std::shared_ptr<TimerService>>("Timer", std::move(factory));
/// @endcode
///
/// @tparam Module Exact C++ value returned by RootContext::OpenPlatformModule.
/// @tparam Options Optional payload-encodable construction options, or void for Null.
template <class Module, class Options = void> struct JavaScriptPlatformModuleFactory;

/// Adapts a JavaScript PlatformView factory object to the Web PlatformView host.
///
/// Properties cross the language boundary through PlatformPayload. A Controller remains an ordinary strongly typed
/// C++ value and is connected to the JavaScript instance through PlatformChannel only when requested by the View.
/// The factory provides `create(properties, events)`. Its returned instance provides a detached `element`, `dispose()`,
/// `update(properties)` when Properties is not void, and `invoke(method, arguments, result)` when Controller is not
/// void.
///
/// Example:
/// @code
/// web::JavaScriptPlatformViewFactory<EditorProperties> factory{
///     .factory = emscripten::val::module_property("exampleEditorFactory"),
/// };
/// root.RegisterPlatformView<EditorProperties>("Editor", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled payload-encodable state, or void for Null.
/// @tparam Controller Optional strongly typed C++ command facade.
template <class Properties, class Controller = void> struct JavaScriptPlatformViewFactory;

namespace detail {

/// Type-erased Emscripten factory consumed by the Web PlatformView host.
struct WebElementFactory {
  std::function<std::shared_ptr<void>(const PlatformValue&, PlatformEventEmitter)> create;
  std::function<emscripten::val(const std::shared_ptr<void>&)> view;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(const std::shared_ptr<void>&)> dispose;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
};

class JavaScriptPlatformViewInstance;

PlatformChannel CreateJavaScriptPlatformModule(PlatformAdapter& adapter, const emscripten::val& factory,
                                                PlatformPayload options);
std::shared_ptr<JavaScriptPlatformViewInstance>
CreateJavaScriptPlatformView(PlatformAdapter& adapter, const emscripten::val& factory, PlatformPayload properties,
                             PlatformEventEmitter events, bool update_required, bool channel_required);
emscripten::val GetJavaScriptPlatformView(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance);
void UpdateJavaScriptPlatformView(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance,
                                  PlatformPayload properties);
void DisposeJavaScriptPlatformView(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance) noexcept;
PlatformChannel GetJavaScriptPlatformViewChannel(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance);

template <class Properties> PlatformPayload EncodeJavaScriptPlatformViewProperties(const PlatformValue& value) {
  if constexpr (std::same_as<Properties, void>) {
    static_cast<void>(value);
    return {};
  } else {
    static_assert(huxerui::detail::PlatformPayloadEncodable<Properties>);
    return huxerui::detail::EncodePlatformValue(value.Get<Properties>());
  }
}

template <class Properties, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
EraseJavaScriptPlatformViewFactory(web::JavaScriptPlatformViewFactory<Properties, Controller> source_value,
                                   PlatformAdapter& adapter) {
  auto source = std::make_shared<web::JavaScriptPlatformViewFactory<Properties, Controller>>(std::move(source_value));
  PlatformAdapter* platform_adapter = &adapter;
  auto factory = std::make_shared<WebElementFactory>();
  factory->create = [source, platform_adapter](const PlatformValue& properties, PlatformEventEmitter events) {
    return std::static_pointer_cast<void>(CreateJavaScriptPlatformView(
        *platform_adapter, source->factory, EncodeJavaScriptPlatformViewProperties<Properties>(properties),
        std::move(events), !std::same_as<Properties, void>, !std::same_as<Controller, void>));
  };
  factory->view = [](const std::shared_ptr<void>& instance) {
    return GetJavaScriptPlatformView(std::static_pointer_cast<JavaScriptPlatformViewInstance>(instance));
  };
  if constexpr (!std::same_as<Properties, void>) {
    factory->update = [](const std::shared_ptr<void>& instance, const PlatformValue& properties) {
      UpdateJavaScriptPlatformView(std::static_pointer_cast<JavaScriptPlatformViewInstance>(instance),
                                   EncodeJavaScriptPlatformViewProperties<Properties>(properties));
    };
  }
  factory->dispose = [](const std::shared_ptr<void>& instance) {
    DisposeJavaScriptPlatformView(std::static_pointer_cast<JavaScriptPlatformViewInstance>(instance));
  };
  if constexpr (!std::same_as<Controller, void>) {
    factory->connect = [source](const std::shared_ptr<void>& instance, const PlatformValue& controller) {
      if (!source->connect) {
        throw std::logic_error("HuxerUI Web JavaScript PlatformView factory must provide connect");
      }
      source->connect(controller.Get<Controller>(),
                      GetJavaScriptPlatformViewChannel(
                          std::static_pointer_cast<JavaScriptPlatformViewInstance>(instance)));
    };
    factory->disconnect = [source](const std::shared_ptr<void>&, const PlatformValue& controller) {
      if (!source->disconnect) {
        throw std::logic_error("HuxerUI Web JavaScript PlatformView factory must provide disconnect");
      }
      source->disconnect(controller.Get<Controller>());
    };
  }
  return huxerui::detail::MakePlatformViewFactoryRegistration(std::move(factory));
}

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(web::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source = std::make_shared<web::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<WebElementFactory>();
  if (source->create) {
    factory->create = [source](const PlatformValue& properties, PlatformEventEmitter events) {
      if constexpr (std::same_as<Properties, void>) {
        return std::static_pointer_cast<void>(source->create(std::move(events)));
      } else {
        return std::static_pointer_cast<void>(source->create(properties.Get<Properties>(), std::move(events)));
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

/// JavaScript-backed PlatformModule factory without construction Options.
template <class Module> struct JavaScriptPlatformModuleFactory<Module, void> {
  emscripten::val factory = emscripten::val::undefined();
  std::function<Module(PlatformChannel)> create;

  Module operator()(PlatformAdapter& adapter) {
    if (!create) {
      throw std::logic_error("HuxerUI Web JavaScript PlatformModule factory is incomplete");
    }
    PlatformChannel channel = detail::CreateJavaScriptPlatformModule(adapter, factory, {});
    try {
      return create(channel);
    } catch (...) {
      channel.Close();
      throw;
    }
  }
};

/// JavaScript-backed PlatformModule factory with payload-encodable construction Options.
template <class Module, class Options> struct JavaScriptPlatformModuleFactory {
  static_assert(huxerui::detail::PlatformPayloadEncodable<Options>);

  emscripten::val factory = emscripten::val::undefined();
  std::function<Module(PlatformChannel)> create;

  Module operator()(PlatformAdapter& adapter, const Options& options) {
    if (!create) {
      throw std::logic_error("HuxerUI Web JavaScript PlatformModule factory is incomplete");
    }
    PlatformChannel channel =
        detail::CreateJavaScriptPlatformModule(adapter, factory, huxerui::detail::EncodePlatformValue(options));
    try {
      return create(channel);
    } catch (...) {
      channel.Close();
      throw;
    }
  }
};

/// JavaScript-backed PlatformView factory without a Controller.
template <class Properties> struct JavaScriptPlatformViewFactory<Properties, void> {
  emscripten::val factory = emscripten::val::undefined();

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    return detail::EraseJavaScriptPlatformViewFactory(std::move(*this), adapter);
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// JavaScript-backed PlatformView factory with a typed C++ Controller connected through PlatformChannel.
template <class Properties, class Controller> struct JavaScriptPlatformViewFactory {
  emscripten::val factory = emscripten::val::undefined();
  std::function<void(const Controller&, PlatformChannel)> connect;
  std::function<void(const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    return detail::EraseJavaScriptPlatformViewFactory(std::move(*this), adapter);
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// Web PlatformView factory without declarative Properties or a Controller.
template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(PlatformEventEmitter)> create;
  std::function<emscripten::val(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// Web PlatformView factory with a Controller and no declarative Properties.
template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(PlatformEventEmitter)> create;
  std::function<emscripten::val(const std::shared_ptr<Instance>&)> view;
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

/// Web PlatformView factory with controlled Properties and no Controller.
template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(const Properties&, PlatformEventEmitter)> create;
  std::function<emscripten::val(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }

  friend class huxerui::detail::PlatformRegistry;
};

/// Web PlatformView factory with controlled Properties and a Controller.
template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(const Properties&, PlatformEventEmitter)> create;
  std::function<emscripten::val(const std::shared_ptr<Instance>&)> view;
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

} // namespace huxerui::web
