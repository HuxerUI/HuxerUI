#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <huxerui/android/jni.h>
#include <huxerui/platform_registry.h>

namespace huxerui::android {

/// The Android objects available to a direct C++/JNI platform factory.
///
/// Both references are borrowed for the current UI-thread callback. A factory must not retain JNIEnv*. Retain context
/// with an explicit JNI global reference only when the created instance needs it after the callback returns.
struct PlatformEnv {
  JNIEnv* jni = nullptr;
  jobject context = nullptr;
};

/// Borrows JNI facilities from the supplied Android window on its application thread.
/// @param ui_window Live original Android window hosting the factory.
/// @return The current thread's JNIEnv and the window's Android Context; do not retain either borrowed reference.
/// @throws std::logic_error If the backend or calling thread does not match.
/// Use a JNI global reference when a created instance needs the Context after the factory returns.
[[nodiscard]] PlatformEnv GetPlatformEnv(UiWindow& ui_window);

/// Borrows application JNI facilities without requiring an attached HuxerUI window.
/// @param runtime Live original Android Runtime, accessed on its application thread.
/// @return The thread-local JNIEnv and application Context; retain Context only through an explicit global reference.
/// @throws std::logic_error If the backend or calling thread does not match.
/// @code{.cpp}
/// context.RegisterPlatformModule<std::shared_ptr<DeviceService>>("Device", [](Runtime& runtime) {
///   const android::PlatformEnv env = android::GetPlatformEnv(runtime);
///   return CreateDeviceService(env.jni, env.context);
/// });
/// @endcode
[[nodiscard]] PlatformEnv GetPlatformEnv(Runtime& runtime);

/// Describes the direct C++/JNI lifecycle of one registered Android PlatformView type.
///
/// Properties is the complete controlled C++ value supplied by `PlatformView(name, properties)`. Instance is arbitrary
/// library-owned retained state and commonly owns a global reference to the Android View plus listeners and event
/// resources. Controller is optional and connects a strongly typed C++ command facade to the mounted instance.
///
/// `create` and `view` are required. `view` is called once and returns a local reference to the stable Android View
/// retained by Instance. When Properties is not void, `update` must apply later property revisions. When Controller is
/// not void, both `connect` and `disconnect` are required. `dispose` is optional and runs after Controller
/// disconnection. Every callback executes on the Android UI thread and receives the current JNIEnv* where needed.
///
/// Example:
/// @code
/// android::PlatformViewFactory<TextProperties, TextInstance> factory{
///     .create = CreateTextView,
///     .view = [](JNIEnv* env, const std::shared_ptr<TextInstance>& instance) {
///       return env->NewLocalRef(instance->view.Get());
///     },
///     .update = UpdateTextView,
///     .dispose = DisposeTextView,
/// };
/// context.RegisterPlatformView<TextProperties>("TextView", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled declarative state, or void when the View has no properties.
/// @tparam Instance Retained C++/JNI state whose shared lifetime covers the mounted Android View.
/// @tparam Controller Optional strongly typed command endpoint connected for the mounted View.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;

/// Registers an Android PlatformView implemented by a Java HuxerUIPlatformView.Factory class.
///
/// Properties cross JNI through PlatformPayload and therefore must be payload-encodable. HuxerUI constructs the Java
/// factory from its fully qualified class_name, owns the Java instance and View lifecycle, and routes Java events
/// through the framework emitter. The Java Factory class must be public and provide a public no-argument constructor.
/// A typed Controller is connected to the Java instance through a PlatformChannel.
///
/// Example:
/// @code
/// android::JavaPlatformViewFactory<TextProperties> factory{
///     .class_name = "org.example.PlatformTextField",
/// };
/// context.RegisterPlatformView<TextProperties>("TextField", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled C++ state encoded for the Java factory, or void for Null.
/// @tparam Controller Optional strongly typed C++ command endpoint backed by a PlatformChannel.
template <class Properties, class Controller = void> struct JavaPlatformViewFactory;

namespace detail {

class JavaPlatformModuleFactoryState;
class JavaPlatformViewFactoryState;
class JavaPlatformViewInstance;

/// Prepares a Java module factory against the application host without requiring a HuxerUIView.
/// @param runtime Original Android Runtime, accessed on its application thread.
/// @param class_name Fully qualified public Java factory class with a public no-argument constructor.
/// @return Retained factory state bound to the original application's channel endpoint.
std::shared_ptr<JavaPlatformModuleFactoryState> PrepareJavaPlatformModuleFactory(Runtime& runtime,
                                                                                 std::string class_name);
PlatformChannel CreateJavaPlatformModule(const std::shared_ptr<JavaPlatformModuleFactoryState>& factory,
                                         PlatformPayload options);

/// Type-erased Android factory consumed by the Android PlatformView host.
struct AndroidViewFactory {
  std::function<std::shared_ptr<void>(JNIEnv*, jobject, const PlatformValue&, PlatformEventEmitter)> create;
  std::function<jobject(JNIEnv*, const std::shared_ptr<void>&)> view;
  std::function<void(JNIEnv*, const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(JNIEnv*, const std::shared_ptr<void>&)> dispose;
  std::function<void(JNIEnv*, const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(JNIEnv*, const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
};

std::shared_ptr<JavaPlatformViewFactoryState> PrepareJavaPlatformViewFactory(UiWindow& ui_window,
                                                                             std::string class_name);
std::shared_ptr<JavaPlatformViewInstance>
CreateJavaPlatformView(const std::shared_ptr<JavaPlatformViewFactoryState>& factory, PlatformPayload properties,
                       PlatformEventEmitter events, bool channel_required);
jobject GetJavaPlatformView(JNIEnv* environment, const std::shared_ptr<JavaPlatformViewInstance>& instance);
void UpdateJavaPlatformView(JNIEnv* environment, const std::shared_ptr<JavaPlatformViewInstance>& instance,
                            PlatformPayload properties);
void DisposeJavaPlatformView(JNIEnv* environment, const std::shared_ptr<JavaPlatformViewInstance>& instance) noexcept;
PlatformChannel GetJavaPlatformViewChannel(const std::shared_ptr<JavaPlatformViewInstance>& instance);

template <class Properties> PlatformPayload EncodeJavaPlatformViewProperties(const PlatformValue& value) {
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
EraseJavaPlatformViewFactory(android::JavaPlatformViewFactory<Properties, Controller> source_value,
                             UiWindow& ui_window) {
  auto source = std::make_shared<android::JavaPlatformViewFactory<Properties, Controller>>(std::move(source_value));
  auto prepared = PrepareJavaPlatformViewFactory(ui_window, source->class_name);
  auto factory = std::make_shared<AndroidViewFactory>();
  factory->create = [prepared](JNIEnv*, jobject, const PlatformValue& properties, PlatformEventEmitter events) {
    return std::static_pointer_cast<void>(
        CreateJavaPlatformView(prepared, EncodeJavaPlatformViewProperties<Properties>(properties), std::move(events),
                               !std::same_as<Controller, void>));
  };
  factory->view = [](JNIEnv* environment, const std::shared_ptr<void>& instance) {
    return GetJavaPlatformView(environment, std::static_pointer_cast<JavaPlatformViewInstance>(instance));
  };
  if constexpr (!std::same_as<Properties, void>) {
    factory->update = [](JNIEnv* environment, const std::shared_ptr<void>& instance, const PlatformValue& properties) {
      UpdateJavaPlatformView(environment, std::static_pointer_cast<JavaPlatformViewInstance>(instance),
                             EncodeJavaPlatformViewProperties<Properties>(properties));
    };
  }
  factory->dispose = [](JNIEnv* environment, const std::shared_ptr<void>& instance) {
    DisposeJavaPlatformView(environment, std::static_pointer_cast<JavaPlatformViewInstance>(instance));
  };
  if constexpr (!std::same_as<Controller, void>) {
    factory->connect = [source](JNIEnv*, const std::shared_ptr<void>& instance, const PlatformValue& controller) {
      if (!source->connect) {
        throw std::logic_error("HuxerUI Android Java PlatformView factory must provide connect");
      }
      source->connect(controller.Get<Controller>(),
                      GetJavaPlatformViewChannel(std::static_pointer_cast<JavaPlatformViewInstance>(instance)));
    };
    factory->disconnect = [source](JNIEnv*, const std::shared_ptr<void>&, const PlatformValue& controller) {
      if (!source->disconnect) {
        throw std::logic_error("HuxerUI Android Java PlatformView factory must provide disconnect");
      }
      source->disconnect(controller.Get<Controller>());
    };
  }
  return huxerui::detail::MakePlatformViewFactoryRegistration(std::move(factory));
}

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(android::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source =
      std::make_shared<android::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<AndroidViewFactory>();
  if (source->create) {
    factory->create = [source](JNIEnv* environment, jobject context, const PlatformValue& properties,
                               PlatformEventEmitter events) {
      if constexpr (std::same_as<Properties, void>) {
        return std::static_pointer_cast<void>(source->create(environment, context, std::move(events)));
      } else {
        return std::static_pointer_cast<void>(
            source->create(environment, context, properties.Get<Properties>(), std::move(events)));
      }
    };
  }
  if (source->view) {
    factory->view = [source](JNIEnv* environment, const std::shared_ptr<void>& instance) {
      return source->view(environment, std::static_pointer_cast<Instance>(instance));
    };
  }
  if constexpr (!std::same_as<Properties, void>) {
    if (source->update) {
      factory->update = [source](JNIEnv* environment, const std::shared_ptr<void>& instance,
                                 const PlatformValue& properties) {
        source->update(environment, *std::static_pointer_cast<Instance>(instance), properties.Get<Properties>());
      };
    }
  }
  if (source->dispose) {
    factory->dispose = [source](JNIEnv* environment, const std::shared_ptr<void>& instance) {
      source->dispose(environment, *std::static_pointer_cast<Instance>(instance));
    };
  }
  if constexpr (!std::same_as<Controller, void>) {
    if (source->connect) {
      factory->connect = [source](JNIEnv* environment, const std::shared_ptr<void>& instance,
                                  const PlatformValue& controller) {
        source->connect(environment, *std::static_pointer_cast<Instance>(instance), controller.Get<Controller>());
      };
    }
    if (source->disconnect) {
      factory->disconnect = [source](JNIEnv* environment, const std::shared_ptr<void>& instance,
                                     const PlatformValue& controller) {
        source->disconnect(environment, *std::static_pointer_cast<Instance>(instance), controller.Get<Controller>());
      };
    }
  }
  return huxerui::detail::MakePlatformViewFactoryRegistration(std::move(factory));
}

} // namespace detail

/// Adapts a direct C++/JNI PlatformModule factory to the platform-neutral registry callable contract.
///
/// The callback receives the owning UiWindow, the current UI-thread JNIEnv*, the borrowed Android Context, and
/// typed Options when present. It returns the library's exact strongly typed Module facade; no PlatformPayload or
/// PlatformChannel is introduced on this direct path.
///
/// Example:
/// @code
/// android::PlatformModuleFactory<std::shared_ptr<CameraService>, CameraOptions> factory{
///     .create = [](UiWindow& ui_window, JNIEnv* env, jobject context, const CameraOptions& options) {
///       return CreateCameraService(ui_window, env, context, options);
///     },
/// };
/// context.RegisterPlatformModule<std::shared_ptr<CameraService>, CameraOptions>("Camera", std::move(factory));
/// @endcode
///
/// @tparam Module Exact C++ value returned by OpenPlatformModule.
/// @tparam Options Optional strongly typed construction options, or void when none are required.
template <class Module, class Options = void> struct PlatformModuleFactory;

/// Direct C++/JNI PlatformModule factory without construction Options.
template <class Module> struct PlatformModuleFactory<Module, void> {
  std::function<Module(UiWindow&, JNIEnv*, jobject)> create;

  Module operator()(UiWindow& ui_window) {
    if (!create) {
      throw std::logic_error("HuxerUI Android PlatformModule factory must provide create");
    }
    const PlatformEnv env = GetPlatformEnv(ui_window);
    return create(ui_window, env.jni, env.context);
  }
};

/// Direct C++/JNI PlatformModule factory with strongly typed construction Options.
template <class Module, class Options> struct PlatformModuleFactory {
  std::function<Module(UiWindow&, JNIEnv*, jobject, const Options&)> create;

  Module operator()(UiWindow& ui_window, const Options& options) {
    if (!create) {
      throw std::logic_error("HuxerUI Android PlatformModule factory must provide create");
    }
    const PlatformEnv env = GetPlatformEnv(ui_window);
    return create(ui_window, env.jni, env.context, options);
  }
};

/// Registers an Android PlatformModule implemented by a Java HuxerUIPlatformModule.Factory class.
///
/// HuxerUI constructs the Java factory from its fully qualified class_name and passes encoded Options to Java. The Java
/// Factory class must be public and provide a public no-argument constructor. `create` receives the shared
/// PlatformChannel for the created Java instance and returns the library's exact strongly typed C++ Module facade. The
/// facade owns or shares that channel and exposes ordinary domain methods instead of method-name strings to application
/// code. Closing the last owning channel connection disposes the Java instance.
///
/// Example:
/// @code
/// android::JavaPlatformModuleFactory<std::shared_ptr<TimerService>> factory;
/// factory.class_name = "org.example.PlatformTimer";
/// factory.create = [](PlatformChannel channel) {
///   return std::make_shared<AndroidTimerService>(std::move(channel));
/// };
/// context.RegisterPlatformModule<std::shared_ptr<TimerService>>("Timer", std::move(factory));
/// @endcode
///
/// @tparam Module Exact C++ value returned by OpenPlatformModule.
/// @tparam Options Optional payload-encodable construction options, or void for Null.
template <class Module, class Options = void> struct JavaPlatformModuleFactory;

/// Java-backed PlatformModule factory without construction Options.
template <class Module> struct JavaPlatformModuleFactory<Module, void> {
  std::string class_name;
  std::function<Module(PlatformChannel)> create;

  /// Creates an application-bound Java module and wraps its channel in the registered C++ facade.
  /// @param runtime Original application host supplied by the frozen catalog, on its application thread.
  /// @return The exact Module value produced by create; no HuxerUI window is required.
  /// @throws std::logic_error If create is empty or the original native host cannot be used.
  Module operator()(Runtime& runtime) {
    if (!create) {
      throw std::logic_error("HuxerUI Android Java PlatformModule factory is incomplete");
    }
    if (!state_) {
      state_ = detail::PrepareJavaPlatformModuleFactory(runtime, class_name);
    }
    PlatformChannel channel = detail::CreateJavaPlatformModule(state_, {});
    try {
      return create(channel);
    } catch (...) {
      channel.Close();
      throw;
    }
  }

private:
  std::shared_ptr<detail::JavaPlatformModuleFactoryState> state_;
};

/// Java-backed PlatformModule factory with payload-encodable construction Options.
template <class Module, class Options> struct JavaPlatformModuleFactory {
  static_assert(huxerui::detail::PlatformPayloadEncodable<Options>);

  std::string class_name;
  std::function<Module(PlatformChannel)> create;

  /// Creates a Java module with typed options encoded for the native factory invocation.
  /// @param runtime Original application host supplied by the frozen catalog, on its application thread.
  /// @param options Construction data borrowed only during this call and encoded into an owned payload.
  /// @return The exact Module facade wrapping this instance's application-bound PlatformChannel.
  /// A factory object remains associated with the first Runtime for which it was prepared.
  Module operator()(Runtime& runtime, const Options& options) {
    if (!create) {
      throw std::logic_error("HuxerUI Android Java PlatformModule factory is incomplete");
    }
    if (!state_) {
      state_ = detail::PrepareJavaPlatformModuleFactory(runtime, class_name);
    }
    PlatformChannel channel = detail::CreateJavaPlatformModule(state_, huxerui::detail::EncodePlatformValue(options));
    try {
      return create(channel);
    } catch (...) {
      channel.Close();
      throw;
    }
  }

private:
  std::shared_ptr<detail::JavaPlatformModuleFactoryState> state_;
};

/// Java-backed PlatformView factory without a Controller.
template <class Properties> struct JavaPlatformViewFactory<Properties, void> {
  std::string class_name;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& ui_window) && {
    return detail::EraseJavaPlatformViewFactory(std::move(*this), ui_window);
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// Java-backed PlatformView factory with a typed C++ Controller connected through PlatformChannel.
template <class Properties, class Controller> struct JavaPlatformViewFactory {
  std::string class_name;
  std::function<void(const Controller&, PlatformChannel)> connect;
  std::function<void(const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& ui_window) && {
    return detail::EraseJavaPlatformViewFactory(std::move(*this), ui_window);
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// Direct C++/JNI PlatformView factory without declarative Properties or a Controller.
template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(JNIEnv*, jobject, PlatformEventEmitter)> create;
  std::function<jobject(JNIEnv*, const std::shared_ptr<Instance>&)> view;
  std::function<void(JNIEnv*, Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& ui_window) && {
    static_cast<void>(ui_window);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// Direct C++/JNI PlatformView factory with a Controller and no declarative Properties.
template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(JNIEnv*, jobject, PlatformEventEmitter)> create;
  std::function<jobject(JNIEnv*, const std::shared_ptr<Instance>&)> view;
  std::function<void(JNIEnv*, Instance&)> dispose;
  std::function<void(JNIEnv*, Instance&, const Controller&)> connect;
  std::function<void(JNIEnv*, Instance&, const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& ui_window) && {
    static_cast<void>(ui_window);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// Direct C++/JNI PlatformView factory with controlled Properties and no Controller.
template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(JNIEnv*, jobject, const Properties&, PlatformEventEmitter)> create;
  std::function<jobject(JNIEnv*, const std::shared_ptr<Instance>&)> view;
  std::function<void(JNIEnv*, Instance&, const Properties&)> update;
  std::function<void(JNIEnv*, Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& ui_window) && {
    static_cast<void>(ui_window);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// Direct C++/JNI PlatformView factory with controlled Properties and a Controller.
template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(JNIEnv*, jobject, const Properties&, PlatformEventEmitter)> create;
  std::function<jobject(JNIEnv*, const std::shared_ptr<Instance>&)> view;
  std::function<void(JNIEnv*, Instance&, const Properties&)> update;
  std::function<void(JNIEnv*, Instance&)> dispose;
  std::function<void(JNIEnv*, Instance&, const Controller&)> connect;
  std::function<void(JNIEnv*, Instance&, const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(UiWindow& ui_window) && {
    static_cast<void>(ui_window);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

} // namespace huxerui::android
