#pragma once

#if defined(__OBJC__)
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class HUXExternalTexture;

typedef NS_ENUM(NSInteger, HUXPlatformPayloadKind) {
  HUXPlatformPayloadKindNull,
  HUXPlatformPayloadKindBoolean,
  HUXPlatformPayloadKindInteger,
  HUXPlatformPayloadKindDouble,
  HUXPlatformPayloadKindString,
  HUXPlatformPayloadKindBytes,
  HUXPlatformPayloadKindList,
  HUXPlatformPayloadKindObject,
  HUXPlatformPayloadKindExternalTexture,
} NS_SWIFT_NAME(PlatformPayload.Kind);

NS_SWIFT_NAME(PlatformPayload)
@interface HUXPlatformPayload : NSObject

@property(nonatomic, readonly) HUXPlatformPayloadKind kind;

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;

+ (instancetype)nullValue NS_SWIFT_NAME(null());
+ (instancetype)booleanValue:(BOOL)value NS_SWIFT_NAME(boolean(_:));
+ (instancetype)integerValue:(int64_t)value NS_SWIFT_NAME(integer(_:));
+ (instancetype)doubleValue:(double)value NS_SWIFT_NAME(double(_:));
+ (instancetype)stringValue:(NSString*)value NS_SWIFT_NAME(string(_:));
+ (instancetype)bytesValue:(NSData*)value NS_SWIFT_NAME(bytes(_:));
+ (instancetype)listValue:(NSArray<HUXPlatformPayload*>*)value NS_SWIFT_NAME(list(_:));
+ (instancetype)objectValue:(NSDictionary<NSString*, HUXPlatformPayload*>*)value NS_SWIFT_NAME(object(_:));
+ (instancetype)externalTextureValue:(HUXExternalTexture*)texture NS_SWIFT_NAME(externalTexture(_:));

- (BOOL)booleanValue NS_SWIFT_NAME(boolean());
- (int64_t)integerValue NS_SWIFT_NAME(integer());
- (double)doubleValue NS_SWIFT_NAME(double());
- (NSString*)stringValue NS_SWIFT_NAME(string());
- (NSData*)bytesValue NS_SWIFT_NAME(bytes());
- (HUXExternalTexture*)externalTextureValue NS_SWIFT_NAME(externalTexture());
- (HUXPlatformPayload*)field:(NSString*)name NS_SWIFT_NAME(field(_:));
- (HUXPlatformPayload*)elementAtIndex:(NSUInteger)index NS_SWIFT_NAME(element(at:));
- (void)validateFields:(NSSet<NSString*>*)fields NS_SWIFT_NAME(validate(fields:));

@end

NS_SWIFT_NAME(PlatformEventEmitter)
@protocol HUXPlatformEventEmitter <NSObject>
- (void)emit:(NSString*)event payload:(HUXPlatformPayload*)payload;
@end

NS_SWIFT_NAME(PlatformResult)
@protocol HUXPlatformResult <NSObject>
- (void)complete:(HUXPlatformPayload*)value;
- (void)failWithCode:(NSString*)code
             message:(NSString*)message
             details:(HUXPlatformPayload*)details;
@end

NS_SWIFT_NAME(PlatformCancellation)
@protocol HUXPlatformCancellation <NSObject>
- (void)cancel;
@end

NS_SWIFT_NAME(PlatformModule)
@protocol HUXPlatformModule <NSObject>
- (nullable id<HUXPlatformCancellation>)invoke:(NSString*)method
                                      arguments:(HUXPlatformPayload*)arguments
                                         result:(id<HUXPlatformResult>)result;
- (void)dispose;
@end

NS_SWIFT_NAME(AppKitPlatformModuleFactory)
@protocol HUXAppKitPlatformModuleFactory <NSObject>
- (id<HUXPlatformModule>)createWithWindow:(NSWindow*)window
                                  options:(HUXPlatformPayload*)options
                                   events:(id<HUXPlatformEventEmitter>)events;
@end

NS_SWIFT_NAME(AppKitPlatformView)
@protocol HUXAppKitPlatformView <NSObject>
@property(nonatomic, readonly) NSView* view;
@optional
- (void)updateWithProperties:(HUXPlatformPayload*)properties;
- (nullable id<HUXPlatformCancellation>)invoke:(NSString*)method
                                      arguments:(HUXPlatformPayload*)arguments
                                         result:(id<HUXPlatformResult>)result;
@required
- (void)dispose;
@end

NS_SWIFT_NAME(AppKitPlatformViewFactory)
@protocol HUXAppKitPlatformViewFactory <NSObject>
- (id<HUXAppKitPlatformView>)createWithWindow:(NSWindow*)window
                                   properties:(HUXPlatformPayload*)properties
                                       events:(id<HUXPlatformEventEmitter>)events;
@end

NS_ASSUME_NONNULL_END
#endif

#if defined(__cplusplus)
#if defined(__OBJC__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#endif
#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

#include <huxerui/platform_registry.h>

#if defined(__OBJC__)
@class NSView;
@class NSWindow;
#else
class NSView;
class NSWindow;
#endif

namespace huxerui::macos {

/// Describes the AppKit lifecycle of one registered PlatformView type.
///
/// Properties is the complete controlled value supplied by `PlatformView(name, properties)`. Instance is arbitrary
/// library-owned retained state and commonly stores the NSView plus delegates, observers, and event resources.
/// Controller is optional and connects a strongly typed C++ command facade to the mounted instance.
///
/// `create` and `view` are required. `create` receives the owning NSWindow, and `view` is called once to obtain the
/// stable NSView retained by Instance. When Properties is not void, `update` must apply later property revisions. When
/// Controller is not void, both `connect` and `disconnect` are required. `dispose` is optional and runs after
/// Controller disconnection. Callbacks execute on the AppKit main thread.
///
/// Example:
/// @code
/// macos::PlatformViewFactory<WebProperties, WebInstance> factory{
///     .create = CreateWebView,
///     .view = [](const std::shared_ptr<WebInstance>& instance) { return instance->view; },
///     .update = UpdateWebView,
///     .dispose = DisposeWebView,
/// };
/// root.RegisterPlatformView<WebProperties>("WebView", std::move(factory));
/// @endcode
///
/// @tparam Properties Controlled declarative state, or void when the View has no properties.
/// @tparam Instance Retained C++ or Objective-C++ state whose shared lifetime covers the mounted NSView.
/// @tparam Controller Optional strongly typed command endpoint connected for the mounted View.
template <class Properties, class Instance, class Controller = void> struct PlatformViewFactory;
template <class Module, class Options = void> struct PlatformModuleFactory;

#if defined(__OBJC__)
template <class Module, class Options = void> struct ObjectiveCPlatformModuleFactory;
template <class Properties, class Controller = void> struct ObjectiveCPlatformViewFactory;
#endif

namespace detail {

/// Type-erased AppKit factory consumed by the macOS PlatformView host.
struct AppKitViewFactory {
  std::function<std::shared_ptr<void>(NSWindow*, const PlatformValue&, PlatformEventEmitter)> create;
  std::function<NSView*(const std::shared_ptr<void>&)> view;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> update;
  std::function<void(const std::shared_ptr<void>&)> dispose;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> connect;
  std::function<void(const std::shared_ptr<void>&, const PlatformValue&)> disconnect;
};

NSWindow* GetAppKitWindow(PlatformAdapter& adapter);

#if defined(__OBJC__)
class ObjectiveCPlatformViewInstance;

PlatformChannel CreateObjectiveCPlatformModule(PlatformAdapter& adapter, NSWindow* owner,
                                               id<HUXAppKitPlatformModuleFactory> factory,
                                               PlatformPayload options);
std::shared_ptr<ObjectiveCPlatformViewInstance>
CreateObjectiveCPlatformView(PlatformAdapter& adapter, NSWindow* owner,
                             id<HUXAppKitPlatformViewFactory> factory, PlatformPayload properties,
                             PlatformEventEmitter events, bool update_required, bool channel_required);
NSView* GetObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance);
void UpdateObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance,
                                  PlatformPayload properties);
void DisposeObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance) noexcept;
PlatformChannel GetObjectiveCPlatformViewChannel(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance);

template <class Properties> PlatformPayload EncodeObjectiveCPlatformViewProperties(const PlatformValue& value) {
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
EraseObjectiveCPlatformViewFactory(macos::ObjectiveCPlatformViewFactory<Properties, Controller> source_value,
                                   PlatformAdapter& adapter) {
  auto source =
      std::make_shared<macos::ObjectiveCPlatformViewFactory<Properties, Controller>>(std::move(source_value));
  PlatformAdapter* platform_adapter = &adapter;
  auto factory = std::make_shared<AppKitViewFactory>();
  factory->create = [source, platform_adapter](NSWindow* owner, const PlatformValue& properties,
                                               PlatformEventEmitter events) {
    return std::static_pointer_cast<void>(CreateObjectiveCPlatformView(
        *platform_adapter, owner, source->factory, EncodeObjectiveCPlatformViewProperties<Properties>(properties),
        std::move(events), !std::same_as<Properties, void>, !std::same_as<Controller, void>));
  };
  factory->view = [](const std::shared_ptr<void>& instance) {
    return GetObjectiveCPlatformView(std::static_pointer_cast<ObjectiveCPlatformViewInstance>(instance));
  };
  if constexpr (!std::same_as<Properties, void>) {
    factory->update = [](const std::shared_ptr<void>& instance, const PlatformValue& properties) {
      UpdateObjectiveCPlatformView(std::static_pointer_cast<ObjectiveCPlatformViewInstance>(instance),
                                   EncodeObjectiveCPlatformViewProperties<Properties>(properties));
    };
  }
  factory->dispose = [](const std::shared_ptr<void>& instance) {
    DisposeObjectiveCPlatformView(std::static_pointer_cast<ObjectiveCPlatformViewInstance>(instance));
  };
  if constexpr (!std::same_as<Controller, void>) {
    factory->connect = [source](const std::shared_ptr<void>& instance, const PlatformValue& controller) {
      if (!source->connect) {
        throw std::logic_error("HuxerUI macOS Objective-C PlatformView factory must provide connect");
      }
      source->connect(controller.Get<Controller>(),
                      GetObjectiveCPlatformViewChannel(
                          std::static_pointer_cast<ObjectiveCPlatformViewInstance>(instance)));
    };
    factory->disconnect = [source](const std::shared_ptr<void>&, const PlatformValue& controller) {
      if (!source->disconnect) {
        throw std::logic_error("HuxerUI macOS Objective-C PlatformView factory must provide disconnect");
      }
      source->disconnect(controller.Get<Controller>());
    };
  }
  return huxerui::detail::MakePlatformViewFactoryRegistration(std::move(factory));
}
#endif

template <class Properties, class Instance, class Controller>
huxerui::detail::PlatformViewFactoryRegistration
ErasePlatformViewFactory(macos::PlatformViewFactory<Properties, Instance, Controller> source_value) {
  auto source = std::make_shared<macos::PlatformViewFactory<Properties, Instance, Controller>>(std::move(source_value));
  auto factory = std::make_shared<AppKitViewFactory>();
  if (source->create) {
    factory->create = [source](NSWindow* owner, const PlatformValue& properties, PlatformEventEmitter events) {
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

/// Adapts a direct Objective-C++ PlatformModule factory to the platform-neutral registry callable contract.
template <class Module> struct PlatformModuleFactory<Module, void> {
  std::function<Module(PlatformAdapter&, NSWindow*)> create;

  Module operator()(PlatformAdapter& adapter) {
    if (!create) {
      throw std::logic_error("HuxerUI macOS PlatformModule factory must provide create");
    }
    return create(adapter, detail::GetAppKitWindow(adapter));
  }
};

template <class Module, class Options> struct PlatformModuleFactory {
  std::function<Module(PlatformAdapter&, NSWindow*, const Options&)> create;

  Module operator()(PlatformAdapter& adapter, const Options& options) {
    if (!create) {
      throw std::logic_error("HuxerUI macOS PlatformModule factory must provide create");
    }
    return create(adapter, detail::GetAppKitWindow(adapter), options);
  }
};

#if defined(__OBJC__)
/// Adapts an actual Objective-C or Swift PlatformModule factory object to a strongly typed C++ Module facade.
/// The factory object creates the platform instance; create wraps its framework-owned PlatformChannel in Module.
template <class Module> struct ObjectiveCPlatformModuleFactory<Module, void> {
  __strong id<HUXAppKitPlatformModuleFactory> factory = nil;
  std::function<Module(PlatformChannel)> create;

  Module operator()(PlatformAdapter& adapter) {
    if (factory == nil || !create) {
      throw std::logic_error("HuxerUI macOS Objective-C PlatformModule factory is incomplete");
    }
    PlatformChannel channel =
        detail::CreateObjectiveCPlatformModule(adapter, detail::GetAppKitWindow(adapter), factory, {});
    try {
      return create(channel);
    } catch (...) {
      channel.Close();
      throw;
    }
  }
};

template <class Module, class Options> struct ObjectiveCPlatformModuleFactory {
  static_assert(huxerui::detail::PlatformPayloadEncodable<Options>);

  __strong id<HUXAppKitPlatformModuleFactory> factory = nil;
  std::function<Module(PlatformChannel)> create;

  Module operator()(PlatformAdapter& adapter, const Options& options) {
    if (factory == nil || !create) {
      throw std::logic_error("HuxerUI macOS Objective-C PlatformModule factory is incomplete");
    }
    PlatformChannel channel = detail::CreateObjectiveCPlatformModule(
        adapter, detail::GetAppKitWindow(adapter), factory, huxerui::detail::EncodePlatformValue(options));
    try {
      return create(channel);
    } catch (...) {
      channel.Close();
      throw;
    }
  }
};

template <class Properties> struct ObjectiveCPlatformViewFactory<Properties, void> {
  __strong id<HUXAppKitPlatformViewFactory> factory = nil;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    return detail::EraseObjectiveCPlatformViewFactory(std::move(*this), adapter);
  }
  friend class huxerui::detail::PlatformRegistry;
};

template <class Properties, class Controller> struct ObjectiveCPlatformViewFactory {
  __strong id<HUXAppKitPlatformViewFactory> factory = nil;
  std::function<void(const Controller&, PlatformChannel)> connect;
  std::function<void(const Controller&)> disconnect;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    return detail::EraseObjectiveCPlatformViewFactory(std::move(*this), adapter);
  }
  friend class huxerui::detail::PlatformRegistry;
};
#endif

/// AppKit PlatformView factory without declarative Properties or a Controller.
template <class Instance> struct PlatformViewFactory<void, Instance, void> {
  std::function<std::shared_ptr<Instance>(NSWindow*, PlatformEventEmitter)> create;
  std::function<NSView*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// AppKit PlatformView factory with a Controller and no declarative Properties.
template <class Instance, class Controller> struct PlatformViewFactory<void, Instance, Controller> {
  std::function<std::shared_ptr<Instance>(NSWindow*, PlatformEventEmitter)> create;
  std::function<NSView*(const std::shared_ptr<Instance>&)> view;
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

/// AppKit PlatformView factory with controlled Properties and no Controller.
template <class Properties, class Instance> struct PlatformViewFactory<Properties, Instance, void> {
  std::function<std::shared_ptr<Instance>(NSWindow*, const Properties&, PlatformEventEmitter)> create;
  std::function<NSView*(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;

private:
  huxerui::detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::ErasePlatformViewFactory(std::move(*this));
  }
  friend class huxerui::detail::PlatformRegistry;
};

/// AppKit PlatformView factory with controlled Properties and a Controller.
template <class Properties, class Instance, class Controller> struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(NSWindow*, const Properties&, PlatformEventEmitter)> create;
  std::function<NSView*(const std::shared_ptr<Instance>&)> view;
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

} // namespace huxerui::macos

#if defined(__OBJC__)
#pragma clang diagnostic pop
#endif
#endif
