#import <huxerui/ios/external_texture.h>
#import <huxerui/ios/platform_registry.h>

#import <objc/runtime.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ios_external_texture_internal.h"
#include "platform_registry_internal.h"

using huxerui::Bytes;
using huxerui::ExternalTexture;
using huxerui::PlatformError;
using huxerui::PlatformEventEmitter;
using huxerui::PlatformPayload;
using huxerui::PlatformPayloadKind;
using huxerui::PlatformResult;

[[noreturn]] static void RaiseInvalidArgument(NSString* reason) {
  @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
}

[[noreturn]] static void RaiseCppException(const std::exception& exception) {
  NSString* reason = [NSString stringWithUTF8String:exception.what()];
  RaiseInvalidArgument(reason == nil ? @"HuxerUI Apple platform bridge rejected a value" : reason);
}

static std::string ToCppString(NSString* value, const char* description) {
  if (value == nil) {
    throw std::invalid_argument(std::string("HuxerUI ") + description + " must not be nil");
  }
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
  if (data == nil) {
    throw std::invalid_argument(std::string("HuxerUI ") + description + " must contain valid Unicode");
  }
  return std::string(static_cast<const char*>(data.bytes), data.length);
}

static NSString* ToNSString(std::string_view value) {
  NSString* result = [[NSString alloc] initWithBytes:value.data()
                                              length:value.size()
                                            encoding:NSUTF8StringEncoding];
  if (result == nil) {
    throw std::logic_error("HuxerUI PlatformPayload contained invalid UTF-8");
  }
  return result;
}

@interface HUXIOSPayloadStorage : NSObject {
@public
  Bytes bytes;
  __strong NSArray<HUXExternalTexture*>* textures;
}
@end

@implementation HUXIOSPayloadStorage
@end

@interface HUXPlatformPayload ()
- (instancetype)initForHuxerUI;
@end

static char payload_storage_key;

static HUXIOSPayloadStorage* PayloadStorage(HUXPlatformPayload* payload) {
  if (payload == nil) {
    throw std::invalid_argument("HuxerUI Apple platform boundary requires a PlatformPayload value");
  }
  HUXIOSPayloadStorage* storage = objc_getAssociatedObject(payload, &payload_storage_key);
  if (storage == nil) {
    throw std::invalid_argument("HuxerUI Apple platform boundary received an invalid PlatformPayload value");
  }
  return storage;
}

static PlatformPayload DecodePayload(HUXPlatformPayload* payload) {
  HUXIOSPayloadStorage* storage = PayloadStorage(payload);
  std::vector<ExternalTexture> textures;
  textures.reserve(storage->textures.count);
  for (HUXExternalTexture* texture in storage->textures) {
    textures.push_back(huxerui::ios::detail::UnwrapExternalTexture(texture));
  }
  return PlatformPayload::Decode(storage->bytes, textures);
}

static HUXPlatformPayload* EncodePayload(PlatformPayload payload) {
  std::vector<ExternalTexture> textures;
  Bytes bytes = payload.Encode(textures);
  NSMutableArray<HUXExternalTexture*>* wrappers = [NSMutableArray arrayWithCapacity:textures.size()];
  for (ExternalTexture& texture : textures) {
    [wrappers addObject:huxerui::ios::detail::WrapExternalTexture(std::move(texture))];
  }
  HUXPlatformPayload* result = [[HUXPlatformPayload alloc] initForHuxerUI];
  HUXIOSPayloadStorage* storage = [HUXIOSPayloadStorage new];
  storage->bytes = std::move(bytes);
  storage->textures = [wrappers copy];
  objc_setAssociatedObject(result, &payload_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return result;
}

static HUXPlatformPayloadKind ToObjectiveCKind(PlatformPayloadKind kind) {
  switch (kind) {
  case PlatformPayloadKind::Null:
    return HUXPlatformPayloadKindNull;
  case PlatformPayloadKind::Boolean:
    return HUXPlatformPayloadKindBoolean;
  case PlatformPayloadKind::Integer:
    return HUXPlatformPayloadKindInteger;
  case PlatformPayloadKind::Double:
    return HUXPlatformPayloadKindDouble;
  case PlatformPayloadKind::String:
    return HUXPlatformPayloadKindString;
  case PlatformPayloadKind::Bytes:
    return HUXPlatformPayloadKindBytes;
  case PlatformPayloadKind::List:
    return HUXPlatformPayloadKindList;
  case PlatformPayloadKind::Object:
    return HUXPlatformPayloadKindObject;
  case PlatformPayloadKind::ExternalTexture:
    return HUXPlatformPayloadKindExternalTexture;
  }
  throw std::logic_error("HuxerUI PlatformPayload contained an unknown kind");
}

struct AppleEventState {
  explicit AppleEventState(PlatformEventEmitter value) : events(std::move(value)) {}

  std::mutex mutex;
  PlatformEventEmitter events;
  bool active = true;
};

@interface HUXIOSPlatformEventEmitter : NSObject <HUXPlatformEventEmitter> {
@public
  std::shared_ptr<AppleEventState> state;
}
- (instancetype)initWithEvents:(PlatformEventEmitter)events;
- (void)close;
@end

@implementation HUXIOSPlatformEventEmitter

- (instancetype)initWithEvents:(PlatformEventEmitter)events {
  self = [super init];
  if (self != nil) {
    state = std::make_shared<AppleEventState>(std::move(events));
  }
  return self;
}

- (nullable HUXPlatformPayload*)emit:(NSString*)event payload:(HUXPlatformPayload*)payload {
  try {
    const std::string name = ToCppString(event, "platform event name");
    PlatformPayload value = DecodePayload(payload);
    PlatformEventEmitter emitter;
    {
      std::lock_guard lock(state->mutex);
      if (!state->active) {
        return nil;
      }
      emitter = state->events;
    }
    std::optional<PlatformPayload> result = emitter.Emit(name, std::move(value));
    return result.has_value() ? EncodePayload(std::move(*result)) : nil;
  } catch (...) {
    return nil;
  }
}

- (void)close {
  if (!state) {
    return;
  }
  std::lock_guard lock(state->mutex);
  state->active = false;
  state->events = {};
}

@end

struct AppleResultState {
  explicit AppleResultState(std::function<void(PlatformResult<PlatformPayload>)> value)
      : completion(std::move(value)) {}

  std::mutex mutex;
  std::function<void(PlatformResult<PlatformPayload>)> completion;
};

static std::function<void(PlatformResult<PlatformPayload>)>
TakeCompletion(const std::shared_ptr<AppleResultState>& state) {
  std::lock_guard lock(state->mutex);
  return std::exchange(state->completion, {});
}

@interface HUXIOSPlatformResult : NSObject <HUXPlatformResult> {
@public
  std::shared_ptr<AppleResultState> state;
}
- (instancetype)initWithCompletion:(std::function<void(PlatformResult<PlatformPayload>)>)completion;
- (void)close;
@end

@implementation HUXIOSPlatformResult

- (instancetype)initWithCompletion:(std::function<void(PlatformResult<PlatformPayload>)>)completion {
  self = [super init];
  if (self != nil) {
    state = std::make_shared<AppleResultState>(std::move(completion));
  }
  return self;
}

- (void)complete:(HUXPlatformPayload*)value {
  PlatformPayload payload;
  try {
    payload = DecodePayload(value);
  } catch (...) {
    if (auto completion = TakeCompletion(state)) {
      completion(PlatformError{
          "huxerui/invalid-result",
          "HuxerUI Apple platform call returned an invalid result payload",
          {},
      });
    }
    return;
  }
  std::function<void(PlatformResult<PlatformPayload>)> completion = TakeCompletion(state);
  if (completion) {
    completion(std::move(payload));
  }
}

- (void)failWithCode:(NSString*)code
             message:(NSString*)message
             details:(HUXPlatformPayload*)details {
  PlatformError error;
  try {
    error = {
        ToCppString(code, "platform error code"),
        ToCppString(message, "platform error message"),
        DecodePayload(details),
    };
  } catch (...) {
    if (auto completion = TakeCompletion(state)) {
      completion(PlatformError{
          "huxerui/invalid-error",
          "HuxerUI Apple platform call returned an invalid error",
          {},
      });
    }
    return;
  }
  std::function<void(PlatformResult<PlatformPayload>)> completion = TakeCompletion(state);
  if (completion) {
    completion(std::move(error));
  }
}

- (void)close {
  if (!state) {
    return;
  }
  std::lock_guard lock(state->mutex);
  state->completion = {};
}

@end

class ObjectiveCInstanceState final {
public:
  ObjectiveCInstanceState(id instance, HUXIOSPlatformEventEmitter* events)
      : instance_(instance), events_(events) {}

  ~ObjectiveCInstanceState() {
    Dispose();
  }

  std::function<void()> Invoke(std::string method, PlatformPayload arguments,
                               std::function<void(PlatformResult<PlatformPayload>)> completion) {
    if (disposed_) {
      throw std::logic_error("HuxerUI iOS Objective-C platform instance is disposed");
    }
    NSString* method_name = ToNSString(method);
    HUXPlatformPayload* payload = EncodePayload(std::move(arguments));
    HUXIOSPlatformResult* result = [[HUXIOSPlatformResult alloc] initWithCompletion:std::move(completion)];
    __strong id<HUXPlatformCancellation> cancellation = nil;
    @try {
      cancellation = [instance_ invoke:method_name arguments:payload result:result];
      if (cancellation != nil && ![cancellation respondsToSelector:@selector(cancel)]) {
        throw std::logic_error("HuxerUI iOS Objective-C invocation returned an invalid cancellation object");
      }
    } @catch (NSException* exception) {
      [result close];
      throw std::logic_error("HuxerUI iOS Objective-C platform invocation raised an exception");
    }
    return [result, cancellation] {
      [result close];
      if (cancellation != nil) {
        @try {
          [cancellation cancel];
        } @catch (NSException* exception) {
          static_cast<void>(exception);
        }
      }
    };
  }

  void CloseEvents() noexcept {
    [events_ close];
  }

  void Dispose() noexcept {
    if (disposed_) {
      return;
    }
    disposed_ = true;
    [events_ close];
    @try {
      [instance_ dispose];
    } @catch (NSException* exception) {
      static_cast<void>(exception);
    }
    instance_ = nil;
    events_ = nil;
  }

  [[nodiscard]] id Instance() const noexcept {
    return instance_;
  }

  [[nodiscard]] bool Disposed() const noexcept {
    return disposed_;
  }

private:
  __strong id instance_ = nil;
  __strong HUXIOSPlatformEventEmitter* events_ = nil;
  bool disposed_ = false;
};

static void ConnectInstance(const huxerui::detail::PlatformChannelEndpoint& endpoint,
                            const std::shared_ptr<ObjectiveCInstanceState>& instance) {
  endpoint.Connect({
      .invoke = [instance](std::string method, PlatformPayload arguments,
                           std::function<void(PlatformResult<PlatformPayload>)> completion) {
        return instance->Invoke(std::move(method), std::move(arguments), std::move(completion));
      },
      .dispose = [instance] { instance->Dispose(); },
  });
}

@implementation HUXPlatformPayload

- (instancetype)initForHuxerUI {
  return [super init];
}

+ (instancetype)nullValue {
  return EncodePayload({});
}

+ (instancetype)booleanValue:(BOOL)value {
  return EncodePayload(PlatformPayload(value == YES));
}

+ (instancetype)integerValue:(int64_t)value {
  return EncodePayload(PlatformPayload(value));
}

+ (instancetype)doubleValue:(double)value {
  try {
    return EncodePayload(PlatformPayload(value));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

+ (instancetype)stringValue:(NSString*)value {
  try {
    return EncodePayload(PlatformPayload(ToCppString(value, "PlatformPayload String value")));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

+ (instancetype)bytesValue:(NSData*)value {
  if (value == nil) {
    RaiseInvalidArgument(@"HuxerUI PlatformPayload Bytes value must not be nil");
  }
  try {
    Bytes bytes(value.length);
    if (!bytes.empty()) {
      std::memcpy(bytes.data(), value.bytes, bytes.size());
    }
    return EncodePayload(PlatformPayload(std::move(bytes)));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

+ (instancetype)listValue:(NSArray<HUXPlatformPayload*>*)value {
  if (value == nil) {
    RaiseInvalidArgument(@"HuxerUI PlatformPayload List value must not be nil");
  }
  try {
    PlatformPayload::List list;
    list.reserve(value.count);
    for (HUXPlatformPayload* item in value) {
      list.push_back(DecodePayload(item));
    }
    return EncodePayload(PlatformPayload(std::move(list)));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

+ (instancetype)objectValue:(NSDictionary<NSString*, HUXPlatformPayload*>*)value {
  if (value == nil) {
    RaiseInvalidArgument(@"HuxerUI PlatformPayload Object value must not be nil");
  }
  try {
    PlatformPayload::Object object;
    for (NSString* key in value) {
      object.emplace(ToCppString(key, "PlatformPayload Object key"), DecodePayload(value[key]));
    }
    return EncodePayload(PlatformPayload(std::move(object)));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

+ (instancetype)externalTextureValue:(HUXExternalTexture*)texture {
  try {
    return EncodePayload(PlatformPayload(huxerui::ios::detail::UnwrapExternalTexture(texture)));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (HUXPlatformPayloadKind)kind {
  try {
    return ToObjectiveCKind(DecodePayload(self).Kind());
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (BOOL)booleanValue {
  try {
    return DecodePayload(self).AsBoolean();
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (int64_t)integerValue {
  try {
    return DecodePayload(self).AsInteger();
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (double)doubleValue {
  try {
    return DecodePayload(self).AsDouble();
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (NSString*)stringValue {
  try {
    return ToNSString(DecodePayload(self).AsString());
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (NSData*)bytesValue {
  try {
    const PlatformPayload payload = DecodePayload(self);
    const std::span bytes = payload.AsBytes();
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (HUXExternalTexture*)externalTextureValue {
  try {
    return huxerui::ios::detail::WrapExternalTexture(DecodePayload(self).AsExternalTexture());
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (HUXPlatformPayload*)field:(NSString*)name {
  try {
    const PlatformPayload payload = DecodePayload(self);
    const auto& object = payload.AsObject();
    const std::string key = ToCppString(name, "PlatformPayload field name");
    const auto found = object.find(key);
    if (found == object.end()) {
      throw std::invalid_argument("HuxerUI PlatformPayload is missing field " + key);
    }
    return EncodePayload(found->second);
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (HUXPlatformPayload*)elementAtIndex:(NSUInteger)index {
  try {
    const PlatformPayload payload = DecodePayload(self);
    const auto& list = payload.AsList();
    if (index >= list.size()) {
      throw std::out_of_range("HuxerUI PlatformPayload element index is outside the List");
    }
    return EncodePayload(list[index]);
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (void)validateFields:(NSSet<NSString*>*)fields {
  if (fields == nil) {
    RaiseInvalidArgument(@"HuxerUI PlatformPayload accepted fields must not be nil");
  }
  try {
    const PlatformPayload payload = DecodePayload(self);
    const auto& object = payload.AsObject();
    for (const auto& [name, value] : object) {
      static_cast<void>(value);
      if (![fields containsObject:ToNSString(name)]) {
        throw std::invalid_argument("HuxerUI PlatformPayload contains unknown field " + name);
      }
    }
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

@end

namespace huxerui::ios::detail {

class ObjectiveCPlatformViewInstance final {
public:
  ~ObjectiveCPlatformViewInstance() {
    if (state) {
      state->CloseEvents();
      if (channel_connected) {
        channel.Close();
      } else {
        state->Dispose();
      }
    }
  }

  std::shared_ptr<ObjectiveCInstanceState> state;
  PlatformChannel channel;
  __strong UIView* view = nil;
  bool channel_connected = false;
};

PlatformChannel CreateObjectiveCPlatformModule(PlatformAdapter& adapter, UIViewController* owner,
                                               id<HUXUIKitPlatformModuleFactory> factory,
                                               PlatformPayload options) {
  if (owner == nil) {
    throw std::logic_error("HuxerUI iOS Objective-C PlatformModule requires an owning UIViewController");
  }
  if (factory == nil || ![factory respondsToSelector:@selector(createWithViewController:options:events:)]) {
    throw std::invalid_argument("HuxerUI iOS Objective-C PlatformModule factory must provide create");
  }
  const huxerui::detail::PlatformChannelEndpoint endpoint = huxerui::detail::MakePlatformChannelEndpoint(adapter);
  HUXIOSPlatformEventEmitter* events = [[HUXIOSPlatformEventEmitter alloc] initWithEvents:endpoint.Events()];
  __strong id<HUXPlatformModule> instance = nil;
  @try {
    instance = [factory createWithViewController:owner
                                         options:EncodePayload(std::move(options))
                                          events:events];
  } @catch (NSException* exception) {
    [events close];
    throw std::logic_error("HuxerUI iOS Objective-C PlatformModule factory raised an exception");
  }
  if (instance == nil || ![instance respondsToSelector:@selector(invoke:arguments:result:)] ||
      ![instance respondsToSelector:@selector(dispose)]) {
    [events close];
    throw std::logic_error("HuxerUI iOS Objective-C PlatformModule factory returned an invalid instance");
  }
  auto state = std::make_shared<ObjectiveCInstanceState>(instance, events);
  ConnectInstance(endpoint, state);
  return endpoint.Channel();
}

std::shared_ptr<ObjectiveCPlatformViewInstance>
CreateObjectiveCPlatformView(PlatformAdapter& adapter, UIViewController* owner,
                             id<HUXUIKitPlatformViewFactory> factory, PlatformPayload properties,
                             PlatformEventEmitter events, bool update_required, bool channel_required) {
  if (owner == nil) {
    throw std::logic_error("HuxerUI iOS Objective-C PlatformView requires an owning UIViewController");
  }
  if (factory == nil || ![factory respondsToSelector:@selector(createWithViewController:properties:events:)]) {
    throw std::invalid_argument("HuxerUI iOS Objective-C PlatformView factory must provide create");
  }
  HUXIOSPlatformEventEmitter* event_endpoint = [[HUXIOSPlatformEventEmitter alloc] initWithEvents:std::move(events)];
  __strong id<HUXUIKitPlatformView> instance = nil;
  @try {
    instance = [factory createWithViewController:owner
                                      properties:EncodePayload(std::move(properties))
                                          events:event_endpoint];
  } @catch (NSException* exception) {
    [event_endpoint close];
    throw std::logic_error("HuxerUI iOS Objective-C PlatformView factory raised an exception");
  }
  if (instance == nil || ![instance respondsToSelector:@selector(view)] ||
      ![instance respondsToSelector:@selector(dispose)] ||
      (update_required && ![instance respondsToSelector:@selector(updateWithProperties:)]) ||
      (channel_required && ![instance respondsToSelector:@selector(invoke:arguments:result:)])) {
    [event_endpoint close];
    throw std::logic_error("HuxerUI iOS Objective-C PlatformView factory returned an invalid instance");
  }
  UIView* view = instance.view;
  if (view == nil) {
    [event_endpoint close];
    throw std::logic_error("HuxerUI iOS Objective-C PlatformView returned a null UIView");
  }
  if (view.superview != nil) {
    [event_endpoint close];
    throw std::logic_error("HuxerUI iOS Objective-C PlatformView returned an attached UIView");
  }

  auto result = std::make_shared<ObjectiveCPlatformViewInstance>();
  result->state = std::make_shared<ObjectiveCInstanceState>(instance, event_endpoint);
  result->view = view;
  if (channel_required) {
    const huxerui::detail::PlatformChannelEndpoint endpoint = huxerui::detail::MakePlatformChannelEndpoint(adapter);
    ConnectInstance(endpoint, result->state);
    result->channel = endpoint.Channel();
    result->channel_connected = true;
  }
  return result;
}

UIView* GetObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance) {
  return instance ? instance->view : nil;
}

void UpdateObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance,
                                  PlatformPayload properties) {
  if (!instance || !instance->state || instance->state->Disposed()) {
    throw std::logic_error("HuxerUI iOS Objective-C PlatformView instance is disposed");
  }
  id<HUXUIKitPlatformView> platform_view = instance->state->Instance();
  @try {
    [platform_view updateWithProperties:EncodePayload(std::move(properties))];
    if (platform_view.view != instance->view) {
      throw std::logic_error("HuxerUI iOS Objective-C PlatformView changed its UIView identity");
    }
  } @catch (NSException* exception) {
    throw std::logic_error("HuxerUI iOS Objective-C PlatformView update raised an exception");
  }
}

void DisposeObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance) noexcept {
  if (!instance) {
    return;
  }
  if (instance->state) {
    instance->state->CloseEvents();
    if (instance->channel_connected) {
      instance->channel.Close();
    } else {
      instance->state->Dispose();
    }
  }
  instance->view = nil;
}

PlatformChannel GetObjectiveCPlatformViewChannel(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance) {
  return instance ? instance->channel : PlatformChannel{};
}

} // namespace huxerui::ios::detail
