#import <huxerui/macos/external_texture.h>
#import <huxerui/macos/platform_registry.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "macos_external_texture_internal.h"
#include "application/platform_registry_internal.h"

using huxerui::BufferReference;
using huxerui::Bytes;
using huxerui::ExternalTexture;
using huxerui::FileReference;
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
  RaiseInvalidArgument(reason == nil ? @"HuxerUI macOS platform bridge rejected a value" : reason);
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

// Keep the FileReference beside its NSURL: the URL identifies the resource, while the C++ state owns any coordinated
// access that must survive asynchronous native media work.
@interface HUXFileReference () {
@private
  std::optional<FileReference> reference_;
  __strong NSURL* file_url_;
}
- (instancetype)initForHuxerUIWithReference:(FileReference)reference fileURL:(NSURL*)file_url;
- (FileReference)referenceForHuxerUI;
@end

static HUXFileReference* WrapFileReference(FileReference reference) {
  const std::optional<huxerui::File> file = reference.AsFile();
  if (!file) {
    throw std::invalid_argument("HuxerUI macOS FileReference has no native file URL");
  }
  NSURL* url = [NSURL fileURLWithPath:ToNSString(file->Path())
                          isDirectory:reference.Type() == huxerui::FileType::Directory];
  if (url == nil) {
    throw std::runtime_error("HuxerUI macOS file URL could not be created");
  }
  return [[HUXFileReference alloc] initForHuxerUIWithReference:std::move(reference) fileURL:url];
}

static FileReference UnwrapFileReference(HUXFileReference* reference) {
  if (reference == nil) {
    throw std::invalid_argument("HuxerUI macOS platform boundary requires a FileReference value");
  }
  return [reference referenceForHuxerUI];
}

@interface HUXBufferReference () {
@private
  BufferReference reference_;
}
- (instancetype)initForHuxerUIWithReference:(BufferReference)reference;
- (BufferReference)referenceForHuxerUI;
@end

static HUXBufferReference* WrapBufferReference(BufferReference reference) {
  return [[HUXBufferReference alloc] initForHuxerUIWithReference:std::move(reference)];
}

static BufferReference UnwrapBufferReference(HUXBufferReference* reference) {
  if (reference == nil) {
    throw std::invalid_argument("HuxerUI platform boundary requires a BufferReference value");
  }
  return [reference referenceForHuxerUI];
}

// HUXP capability slots index these retained wrapper arrays, so bytes and wrappers must share one object lifetime.
@interface HUXPlatformPayload () {
@private
  Bytes bytes_;
  __strong NSArray<HUXExternalTexture*>* textures_;
  __strong NSArray<HUXFileReference*>* file_references_;
  __strong NSArray<HUXBufferReference*>* buffer_references_;
}
- (instancetype)initForHuxerUIWithEnvelope:(PlatformPayload::Envelope)envelope;
- (PlatformPayload)platformPayloadForHuxerUI;
@end

static PlatformPayload DecodePayload(HUXPlatformPayload* payload) {
  if (payload == nil) {
    throw std::invalid_argument("HuxerUI macOS platform boundary requires a PlatformPayload value");
  }
  return [payload platformPayloadForHuxerUI];
}

static HUXPlatformPayload* EncodePayload(PlatformPayload payload) {
  return [[HUXPlatformPayload alloc] initForHuxerUIWithEnvelope:payload.Encode()];
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
  case PlatformPayloadKind::FileReference:
    return HUXPlatformPayloadKindFileReference;
  case PlatformPayloadKind::BufferReference:
    return HUXPlatformPayloadKindBufferReference;
  }
  throw std::logic_error("HuxerUI PlatformPayload contained an unknown kind");
}

@interface HUXMacPlatformEventEmitter : NSObject <HUXPlatformEventEmitter> {
@private
  std::mutex mutex_;
  PlatformEventEmitter events_;
  bool active_;
}
- (instancetype)initWithEvents:(PlatformEventEmitter)events;
- (void)close;
@end

@implementation HUXMacPlatformEventEmitter

- (instancetype)initWithEvents:(PlatformEventEmitter)events {
  self = [super init];
  if (self != nil) {
    events_ = std::move(events);
    active_ = true;
  }
  return self;
}

- (nullable HUXPlatformPayload*)emit:(NSString*)event payload:(HUXPlatformPayload*)payload {
  try {
    const std::string name = ToCppString(event, "platform event name");
    PlatformPayload value = DecodePayload(payload);
    PlatformEventEmitter emitter;
    {
      std::lock_guard lock(mutex_);
      if (!active_) {
        return nil;
      }
      emitter = events_;
    }
    // Native handlers may synchronously close or reenter this bridge, so invoke a retained copy outside the lock.
    std::optional<PlatformPayload> result = emitter.Emit(name, std::move(value));
    return result.has_value() ? EncodePayload(std::move(*result)) : nil;
  } catch (...) {
    return nil;
  }
}

- (void)close {
  std::lock_guard lock(mutex_);
  active_ = false;
  events_ = {};
}

@end

@interface HUXMacPlatformResult : NSObject <HUXPlatformResult> {
@private
  std::mutex mutex_;
  std::function<void(PlatformResult<PlatformPayload>)> completion_;
}
- (instancetype)initWithCompletion:(std::function<void(PlatformResult<PlatformPayload>)>)completion;
- (std::function<void(PlatformResult<PlatformPayload>)>)takeCompletion;
- (void)close;
@end

@implementation HUXMacPlatformResult

- (instancetype)initWithCompletion:(std::function<void(PlatformResult<PlatformPayload>)>)completion {
  self = [super init];
  if (self != nil) {
    completion_ = std::move(completion);
  }
  return self;
}

- (std::function<void(PlatformResult<PlatformPayload>)>)takeCompletion {
  std::lock_guard lock(mutex_);
  return std::exchange(completion_, {});
}

- (void)complete:(HUXPlatformPayload*)value {
  PlatformPayload payload;
  try {
    payload = DecodePayload(value);
  } catch (...) {
    if (auto completion = [self takeCompletion]) {
      completion(PlatformError{"huxerui/invalid-result",
                               "HuxerUI macOS platform call returned an invalid result payload", {}});
    }
    return;
  }
  std::function<void(PlatformResult<PlatformPayload>)> completion = [self takeCompletion];
  if (completion) {
    completion(std::move(payload));
  }
}

- (void)failWithCode:(NSString*)code
             message:(NSString*)message
             details:(HUXPlatformPayload*)details {
  PlatformError error;
  try {
    error = {ToCppString(code, "platform error code"), ToCppString(message, "platform error message"),
             DecodePayload(details)};
  } catch (...) {
    if (auto completion = [self takeCompletion]) {
      completion(PlatformError{"huxerui/invalid-error", "HuxerUI macOS platform call returned an invalid error", {}});
    }
    return;
  }
  std::function<void(PlatformResult<PlatformPayload>)> completion = [self takeCompletion];
  if (completion) {
    completion(std::move(error));
  }
}

- (void)close {
  std::lock_guard lock(mutex_);
  completion_ = {};
}

@end

class ObjectiveCInstanceState final {
public:
  ObjectiveCInstanceState(id instance, HUXMacPlatformEventEmitter* events)
      : instance_(instance), events_(events) {}

  ~ObjectiveCInstanceState() {
    Dispose();
  }

  std::function<void()> Invoke(std::string method, PlatformPayload arguments,
                               std::function<void(PlatformResult<PlatformPayload>)> completion) {
    if (disposed_) {
      throw std::logic_error("HuxerUI macOS Objective-C platform instance is disposed");
    }
    NSString* method_name = ToNSString(method);
    HUXPlatformPayload* payload = EncodePayload(std::move(arguments));
    HUXMacPlatformResult* result = [[HUXMacPlatformResult alloc] initWithCompletion:std::move(completion)];
    __strong id<HUXPlatformCancellation> cancellation = nil;
    @try {
      cancellation = [instance_ invoke:method_name arguments:payload result:result];
      if (cancellation != nil && ![cancellation respondsToSelector:@selector(cancel)]) {
        throw std::logic_error("HuxerUI macOS Objective-C invocation returned an invalid cancellation object");
      }
    } @catch (NSException* exception) {
      static_cast<void>(exception);
      [result close];
      throw std::logic_error("HuxerUI macOS Objective-C platform invocation raised an exception");
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
  __strong HUXMacPlatformEventEmitter* events_ = nil;
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

@implementation HUXFileReference

- (instancetype)initForHuxerUIWithReference:(FileReference)reference fileURL:(NSURL*)file_url {
  self = [super init];
  if (self != nil) {
    reference_.emplace(std::move(reference));
    file_url_ = file_url;
  }
  return self;
}

- (FileReference)referenceForHuxerUI {
  if (!reference_.has_value() || file_url_ == nil) {
    throw std::invalid_argument("HuxerUI macOS platform boundary received an invalid FileReference value");
  }
  return *reference_;
}

- (NSURL*)fileURL {
  try {
    if (!reference_.has_value() || file_url_ == nil) {
      throw std::invalid_argument("HuxerUI macOS platform boundary received an invalid FileReference value");
    }
    return file_url_;
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

@end

@implementation HUXBufferReference

- (instancetype)initForHuxerUIWithReference:(BufferReference)reference {
  self = [super init];
  if (self != nil) {
    reference_ = std::move(reference);
  }
  return self;
}

- (instancetype)initWithBytes:(const void*)bytes length:(NSUInteger)length owner:(id)owner {
  if (owner == nil || (bytes == nullptr && length != 0)) {
    RaiseInvalidArgument(@"HuxerUI buffer reference requires retained, address-stable storage");
  }
  try {
    struct Owner {
      __strong id object;
    };
    auto retained = std::make_shared<Owner>(owner);
    return [self initForHuxerUIWithReference:BufferReference(
        {static_cast<const std::byte*>(bytes), length}, std::move(retained))];
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (BufferReference)referenceForHuxerUI {
  return reference_;
}

- (void)withUnsafeBytes:(void (^)(const void*, NSUInteger))reader {
  if (reader == nil) {
    RaiseInvalidArgument(@"HuxerUI buffer reader must not be nil");
  }
  const BufferReference retained = reference_;
  const auto bytes = retained.AsBytes();
  reader(bytes.data(), bytes.size());
}

- (HUXBufferReference*)sliceWithOffset:(NSUInteger)offset length:(NSUInteger)length {
  try {
    return WrapBufferReference(reference_.Slice(offset, length));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (BOOL)isEqual:(id)object {
  return [object isKindOfClass:[HUXBufferReference class]] &&
         reference_ == [static_cast<HUXBufferReference*>(object) referenceForHuxerUI];
}

- (NSUInteger)hash {
  const auto bytes = reference_.AsBytes();
  return reinterpret_cast<std::uintptr_t>(bytes.data()) ^ bytes.size();
}

@end

@implementation HUXPlatformPayload

- (instancetype)initForHuxerUIWithEnvelope:(PlatformPayload::Envelope)envelope {
  self = [super init];
  if (self != nil) {
    bytes_ = std::move(envelope.bytes);
    NSMutableArray<HUXExternalTexture*>* textures =
        [NSMutableArray arrayWithCapacity:envelope.external_textures.size()];
    for (std::shared_ptr<ExternalTexture>& texture : envelope.external_textures) {
      [textures addObject:huxerui::macos::detail::WrapExternalTexture(std::move(texture))];
    }
    textures_ = [textures copy];
    NSMutableArray<HUXFileReference*>* file_references =
        [NSMutableArray arrayWithCapacity:envelope.file_references.size()];
    for (FileReference& reference : envelope.file_references) {
      [file_references addObject:WrapFileReference(std::move(reference))];
    }
    file_references_ = [file_references copy];
    NSMutableArray<HUXBufferReference*>* buffers = [NSMutableArray arrayWithCapacity:envelope.buffer_references.size()];
    for (BufferReference& reference : envelope.buffer_references) {
      [buffers addObject:WrapBufferReference(std::move(reference))];
    }
    buffer_references_ = [buffers copy];
  }
  return self;
}

- (PlatformPayload)platformPayloadForHuxerUI {
  if (textures_ == nil || file_references_ == nil || buffer_references_ == nil) {
    throw std::invalid_argument("HuxerUI macOS platform boundary received an invalid PlatformPayload value");
  }
  PlatformPayload::Envelope envelope;
  envelope.bytes = bytes_;
  envelope.external_textures.reserve(textures_.count);
  for (HUXExternalTexture* texture in textures_) {
    envelope.external_textures.push_back(huxerui::macos::detail::UnwrapExternalTexture(texture));
  }
  envelope.file_references.reserve(file_references_.count);
  for (HUXFileReference* reference in file_references_) {
    envelope.file_references.push_back(UnwrapFileReference(reference));
  }
  envelope.buffer_references.reserve(buffer_references_.count);
  for (HUXBufferReference* reference in buffer_references_) {
    envelope.buffer_references.push_back(UnwrapBufferReference(reference));
  }
  return PlatformPayload::Decode(envelope);
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
    return EncodePayload(PlatformPayload(huxerui::macos::detail::UnwrapExternalTexture(texture)));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

+ (instancetype)fileReferenceValue:(HUXFileReference*)reference {
  try {
    return EncodePayload(PlatformPayload(UnwrapFileReference(reference)));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

+ (instancetype)bufferReferenceValue:(HUXBufferReference*)reference {
  try {
    return EncodePayload(PlatformPayload(UnwrapBufferReference(reference)));
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (HUXBufferReference*)bufferReferenceValue {
  try {
    return WrapBufferReference(DecodePayload(self).AsBufferReference());
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
    return huxerui::macos::detail::WrapExternalTexture(DecodePayload(self).AsExternalTexture());
  } catch (const std::exception& exception) {
    RaiseCppException(exception);
  }
}

- (HUXFileReference*)fileReferenceValue {
  try {
    return WrapFileReference(DecodePayload(self).AsFileReference());
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

namespace huxerui::macos::detail {

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
  __strong NSView* view = nil;
  bool channel_connected = false;
};

PlatformChannel CreateObjectiveCPlatformModule(PlatformAdapter& adapter, NSWindow* owner,
                                               id<HUXAppKitPlatformModuleFactory> factory,
                                               PlatformPayload options) {
  if (owner == nil) {
    throw std::logic_error("HuxerUI macOS Objective-C PlatformModule requires an owning NSWindow");
  }
  if (factory == nil || ![factory respondsToSelector:@selector(createWithWindow:options:events:)]) {
    throw std::invalid_argument("HuxerUI macOS Objective-C PlatformModule factory must provide create");
  }
  const huxerui::detail::PlatformChannelEndpoint endpoint = huxerui::detail::MakePlatformChannelEndpoint(adapter);
  HUXMacPlatformEventEmitter* events = [[HUXMacPlatformEventEmitter alloc] initWithEvents:endpoint.Events()];
  __strong id<HUXPlatformModule> instance = nil;
  @try {
    instance = [factory createWithWindow:owner
                                 options:EncodePayload(std::move(options))
                                  events:events];
  } @catch (NSException* exception) {
    static_cast<void>(exception);
    [events close];
    throw std::logic_error("HuxerUI macOS Objective-C PlatformModule factory raised an exception");
  }
  if (instance == nil || ![instance respondsToSelector:@selector(invoke:arguments:result:)] ||
      ![instance respondsToSelector:@selector(dispose)]) {
    [events close];
    throw std::logic_error("HuxerUI macOS Objective-C PlatformModule factory returned an invalid instance");
  }
  auto state = std::make_shared<ObjectiveCInstanceState>(instance, events);
  ConnectInstance(endpoint, state);
  return endpoint.Channel();
}

std::shared_ptr<ObjectiveCPlatformViewInstance>
CreateObjectiveCPlatformView(PlatformAdapter& adapter, NSWindow* owner,
                             id<HUXAppKitPlatformViewFactory> factory, PlatformPayload properties,
                             PlatformEventEmitter events, bool update_required, bool channel_required) {
  if (owner == nil) {
    throw std::logic_error("HuxerUI macOS Objective-C PlatformView requires an owning NSWindow");
  }
  if (factory == nil || ![factory respondsToSelector:@selector(createWithWindow:properties:events:)]) {
    throw std::invalid_argument("HuxerUI macOS Objective-C PlatformView factory must provide create");
  }
  HUXMacPlatformEventEmitter* event_endpoint = [[HUXMacPlatformEventEmitter alloc] initWithEvents:std::move(events)];
  __strong id<HUXAppKitPlatformView> instance = nil;
  @try {
    instance = [factory createWithWindow:owner
                              properties:EncodePayload(std::move(properties))
                                  events:event_endpoint];
  } @catch (NSException* exception) {
    static_cast<void>(exception);
    [event_endpoint close];
    throw std::logic_error("HuxerUI macOS Objective-C PlatformView factory raised an exception");
  }
  if (instance == nil || ![instance respondsToSelector:@selector(view)] ||
      ![instance respondsToSelector:@selector(dispose)] ||
      (update_required && ![instance respondsToSelector:@selector(updateWithProperties:)]) ||
      (channel_required && ![instance respondsToSelector:@selector(invoke:arguments:result:)])) {
    [event_endpoint close];
    throw std::logic_error("HuxerUI macOS Objective-C PlatformView factory returned an invalid instance");
  }
  NSView* view = instance.view;
  if (view == nil) {
    [event_endpoint close];
    throw std::logic_error("HuxerUI macOS Objective-C PlatformView returned a null NSView");
  }
  if (view.superview != nil) {
    [event_endpoint close];
    throw std::logic_error("HuxerUI macOS Objective-C PlatformView returned an attached NSView");
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

NSView* GetObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance) {
  return instance ? instance->view : nil;
}

void UpdateObjectiveCPlatformView(const std::shared_ptr<ObjectiveCPlatformViewInstance>& instance,
                                  PlatformPayload properties) {
  if (!instance || !instance->state || instance->state->Disposed()) {
    throw std::logic_error("HuxerUI macOS Objective-C PlatformView instance is disposed");
  }
  id<HUXAppKitPlatformView> platform_view = instance->state->Instance();
  @try {
    [platform_view updateWithProperties:EncodePayload(std::move(properties))];
    if (platform_view.view != instance->view) {
      throw std::logic_error("HuxerUI macOS Objective-C PlatformView changed its NSView identity");
    }
  } @catch (NSException* exception) {
    static_cast<void>(exception);
    throw std::logic_error("HuxerUI macOS Objective-C PlatformView update raised an exception");
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

} // namespace huxerui::macos::detail
