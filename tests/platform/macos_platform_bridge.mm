#import <CoreVideo/CoreVideo.h>
#import <objc/runtime.h>

#include <catch2/catch_amalgamated.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/macos/external_texture.h>
#include <huxerui/macos/platform_registry.h>

#include "macos_external_texture_internal.h"
#include "runtime_test_support.h"

@interface HuxerUITestMacCancellation : NSObject <HUXPlatformCancellation>
- (instancetype)initWithBlock:(void (^)(void))block;
@end

@implementation HuxerUITestMacCancellation {
  void (^_block)(void);
}

- (instancetype)initWithBlock:(void (^)(void))block {
  self = [super init];
  if (self != nil) {
    _block = [block copy];
  }
  return self;
}

- (void)cancel {
  void (^block)(void) = _block;
  _block = nil;
  if (block != nil) {
    block();
  }
}

@end

@interface HuxerUITestMacPlatformView : NSObject <HUXAppKitPlatformView>
@property(nonatomic, readonly) NSView* view;
@property(nonatomic, readonly) HUXPlatformPayload* properties;
@property(nonatomic, readonly) NSInteger updateCount;
@property(nonatomic, readonly) NSInteger cancelCount;
@property(nonatomic, readonly) NSInteger disposeCount;
@property(nonatomic, readonly) NSArray<NSString*>* terminationOrder;
- (instancetype)initWithProperties:(HUXPlatformPayload*)properties
                             events:(id<HUXPlatformEventEmitter>)events;
- (HUXPlatformPayload*)requestDecision;
- (void)emitLateEvent;
@end

@implementation HuxerUITestMacPlatformView {
  __strong id<HUXPlatformEventEmitter> _events;
  __strong NSMutableArray<NSString*>* _mutableTerminationOrder;
}

- (instancetype)initWithProperties:(HUXPlatformPayload*)properties
                             events:(id<HUXPlatformEventEmitter>)events {
  self = [super init];
  if (self != nil) {
    _view = [NSView new];
    _properties = properties;
    _events = events;
    _mutableTerminationOrder = [NSMutableArray array];
  }
  return self;
}

- (void)updateWithProperties:(HUXPlatformPayload*)properties {
  _properties = properties;
  ++_updateCount;
}

- (id<HUXPlatformCancellation>)invoke:(NSString*)method
                            arguments:(HUXPlatformPayload*)arguments
                               result:(id<HUXPlatformResult>)result {
  if ([method isEqualToString:@"echo"]) {
    [result complete:arguments];
    [result complete:HUXPlatformPayload.nullValue];
    return nil;
  }
  if ([method isEqualToString:@"pending"]) {
    __weak HuxerUITestMacPlatformView* weak_self = self;
    return [[HuxerUITestMacCancellation alloc] initWithBlock:^{
      HuxerUITestMacPlatformView* strong_self = weak_self;
      if (strong_self != nil) {
        ++strong_self->_cancelCount;
        [strong_self->_mutableTerminationOrder addObject:@"cancel"];
      }
    }];
  }
  [result failWithCode:@"test/unknown-method"
               message:@"Unknown method"
               details:HUXPlatformPayload.nullValue];
  return nil;
}

- (void)dispose {
  ++_disposeCount;
  [_mutableTerminationOrder addObject:@"dispose"];
}

- (NSArray<NSString*>*)terminationOrder {
  return [_mutableTerminationOrder copy];
}

- (HUXPlatformPayload*)requestDecision {
  return [_events emit:@"decision" payload:[HUXPlatformPayload integerValue:7]];
}

- (void)emitLateEvent {
  [_events emit:@"changed" payload:[HUXPlatformPayload stringValue:@"late"]];
}

@end

@interface HuxerUITestMacPlatformViewFactory : NSObject <HUXAppKitPlatformViewFactory>
@property(nonatomic, readonly) NSWindow* owner;
@property(nonatomic, readonly) HuxerUITestMacPlatformView* instance;
@end

@implementation HuxerUITestMacPlatformViewFactory

- (id<HUXAppKitPlatformView>)createWithWindow:(NSWindow*)window
                                   properties:(HUXPlatformPayload*)properties
                                       events:(id<HUXPlatformEventEmitter>)events {
  _owner = window;
  _instance = [[HuxerUITestMacPlatformView alloc] initWithProperties:properties events:events];
  [events emit:@"changed" payload:[HUXPlatformPayload stringValue:@"created"]];
  return _instance;
}

@end

namespace huxerui::test {
namespace {

TEST_CASE("MacObjectiveCPlatformPayloadPreservesEveryValueKind") {
  @autoreleasepool {
    HUXExternalTextureSource* source = [[HUXExternalTextureSource alloc] initWithIntrinsicSize:CGSizeMake(16.0, 9.0)];
    HUXPlatformPayload* payload = [HUXPlatformPayload objectValue:@{
      @"null" : HUXPlatformPayload.nullValue,
      @"boolean" : [HUXPlatformPayload booleanValue:YES],
      @"integer" : [HUXPlatformPayload integerValue:std::numeric_limits<std::int64_t>::min()],
      @"double" : [HUXPlatformPayload doubleValue:-12.5],
      @"string" : [HUXPlatformPayload stringValue:@"value"],
      @"bytes" : [HUXPlatformPayload bytesValue:[NSData dataWithBytes:"\x01\x02" length:2]],
      @"list" : [HUXPlatformPayload listValue:@[
        HUXPlatformPayload.nullValue,
        [HUXPlatformPayload externalTextureValue:source.texture],
      ]],
      @"texture" : [HUXPlatformPayload externalTextureValue:source.texture],
    }];

    REQUIRE(payload.kind == HUXPlatformPayloadKindObject);
    REQUIRE([[payload field:@"null"] kind] == HUXPlatformPayloadKindNull);
    REQUIRE([[payload field:@"boolean"] booleanValue]);
    REQUIRE([[payload field:@"integer"] integerValue] == std::numeric_limits<std::int64_t>::min());
    REQUIRE([[payload field:@"double"] doubleValue] == -12.5);
    REQUIRE([[[payload field:@"string"] stringValue] isEqualToString:@"value"]);
    REQUIRE([[payload field:@"bytes"] bytesValue].length == 2);
    REQUIRE([[[payload field:@"list"] elementAtIndex:1] kind] == HUXPlatformPayloadKindExternalTexture);

    const ExternalTexture texture =
        macos::detail::UnwrapExternalTexture([[payload field:@"texture"] externalTextureValue]);
    REQUIRE(texture.HasValue());
    REQUIRE(texture.IntrinsicSize() == Size{16.0F, 9.0F});
  }
}

TEST_CASE("MacObjectiveCPlatformPayloadRejectsInvalidValuesAndTextures") {
  @autoreleasepool {
    bool invalid_double_rejected = false;
    @try {
      static_cast<void>([HUXPlatformPayload doubleValue:std::numeric_limits<double>::infinity()]);
    } @catch (NSException*) {
      invalid_double_rejected = true;
    }
    REQUIRE(invalid_double_rejected);

    HUXExternalTexture* forged = static_cast<HUXExternalTexture*>(class_createInstance(HUXExternalTexture.class, 0));
    bool forged_texture_rejected = false;
    @try {
      static_cast<void>([HUXPlatformPayload externalTextureValue:forged]);
    } @catch (NSException*) {
      forged_texture_rejected = true;
    }
    REQUIRE(forged_texture_rejected);
  }
}

TEST_CASE("MacObjectiveCExternalTextureSourceUsesTheExistingMailbox") {
  @autoreleasepool {
    HUXExternalTextureSource* source = [[HUXExternalTextureSource alloc] initWithIntrinsicSize:CGSizeMake(2.0, 2.0)];
    CVPixelBufferRef pixel_buffer = nullptr;
    REQUIRE(CVPixelBufferCreate(
                kCFAllocatorDefault,
                2,
                2,
                kCVPixelFormatType_32BGRA,
                nullptr,
                &pixel_buffer
            ) == kCVReturnSuccess);
    REQUIRE(pixel_buffer != nullptr);
    [source publishPixelBuffer:pixel_buffer];
    [source finish];

    bool finished_source_rejected = false;
    @try {
      [source publishPixelBuffer:pixel_buffer];
    } @catch (NSException*) {
      finished_source_rejected = true;
    }
    CVPixelBufferRelease(pixel_buffer);
    REQUIRE(finished_source_rejected);
  }
}

TEST_CASE("MacObjectiveCPlatformViewUsesOneEventAndChannelLifecycle") {
  @autoreleasepool {
    TestPlatform platform;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0.0, 0.0, 320.0, 200.0)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    HuxerUITestMacPlatformViewFactory* factory = [HuxerUITestMacPlatformViewFactory new];
    std::vector<std::pair<std::string, std::string>> events;
    PlatformEventEmitter emitter = detail::MakePlatformEventEmitter(
        {},
        [&events](std::string name, PlatformPayload payload) {
          if (name == "decision") {
            return std::optional{PlatformPayload(payload.AsInteger() == 7)};
          }
          events.emplace_back(std::move(name), payload.AsString());
          return std::optional<PlatformPayload>{};
        }
    );
    const std::shared_ptr<macos::detail::ObjectiveCPlatformViewInstance> instance =
        macos::detail::CreateObjectiveCPlatformView(
            platform, window, factory, PlatformPayload("initial"), std::move(emitter), true, true
        );

    REQUIRE(factory.owner == window);
    REQUIRE(macos::detail::GetObjectiveCPlatformView(instance) == factory.instance.view);
    REQUIRE([factory.instance.properties.stringValue isEqualToString:@"initial"]);
    REQUIRE(events == std::vector<std::pair<std::string, std::string>>{{"changed", "created"}});
    HUXPlatformPayload* decision = [factory.instance requestDecision];
    REQUIRE(decision.kind == HUXPlatformPayloadKindBoolean);
    REQUIRE(decision.booleanValue);

    macos::detail::UpdateObjectiveCPlatformView(instance, PlatformPayload("updated"));
    REQUIRE(factory.instance.updateCount == 1);
    REQUIRE([factory.instance.properties.stringValue isEqualToString:@"updated"]);
    REQUIRE(macos::detail::GetObjectiveCPlatformView(instance) == factory.instance.view);

    PlatformChannel channel = macos::detail::GetObjectiveCPlatformViewChannel(instance);
    bool completed = false;
    std::string result_value;
    static_cast<void>(channel.Invoke(
        "echo",
        PlatformPayload("value"),
        [&](PlatformResult<PlatformPayload> result) {
          completed = true;
          result_value = std::get<PlatformPayload>(std::move(result)).AsString();
        }
    ));
    REQUIRE_FALSE(completed);
    platform.RunPlatformModuleTasks();
    REQUIRE(completed);
    REQUIRE(result_value == "value");

    const PlatformRequestId pending = channel.Invoke("pending", {}, [](PlatformResult<PlatformPayload>) {});
    platform.RunPlatformModuleTasks();
    REQUIRE(channel.Cancel(pending));
    REQUIRE_FALSE(channel.Cancel(pending));
    platform.RunPlatformModuleTasks();
    REQUIRE(factory.instance.cancelCount == 1);

    static_cast<void>(channel.Invoke("pending", {}, [](PlatformResult<PlatformPayload>) {}));
    platform.RunPlatformModuleTasks();
    macos::detail::DisposeObjectiveCPlatformView(instance);
    macos::detail::DisposeObjectiveCPlatformView(instance);
    REQUIRE_FALSE(channel.IsOpen());
    [factory.instance emitLateEvent];
    REQUIRE(events.size() == 1);
    REQUIRE(factory.instance.disposeCount == 0);
    platform.RunPlatformModuleTasks();
    REQUIRE(factory.instance.cancelCount == 2);
    REQUIRE(factory.instance.disposeCount == 1);
    REQUIRE([factory.instance.terminationOrder isEqualToArray:@[ @"cancel", @"cancel", @"dispose" ]]);
  }
}

} // namespace
} // namespace huxerui::test
