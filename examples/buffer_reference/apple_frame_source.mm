#include "frame_source.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <limits>
#include <utility>

#if TARGET_OS_IOS
#import <huxerui/ios/platform_registry.h>
namespace apple = huxerui::ios;
#else
#import <huxerui/macos/platform_registry.h>
namespace apple = huxerui::macos;
#endif

namespace demo = huxerui::example::buffer_reference;

@interface HuxerUIExampleFrameSource : NSObject <HUXPlatformModule>
@end

@implementation HuxerUIExampleFrameSource {
  NSMutableData* _storage;
  HUXBufferReference* _reference;
  int64_t _sequence;
}

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _storage = [NSMutableData dataWithLength:demo::storage_size];
    _reference = [[HUXBufferReference alloc] initWithBytes:_storage.bytes length:_storage.length owner:_storage];
  }
  return self;
}

- (id<HUXPlatformCancellation>)invoke:(NSString*)method arguments:(HUXPlatformPayload*)arguments
                             result:(id<HUXPlatformResult>)result {
  if (![method isEqualToString:@"next"] || arguments.kind != HUXPlatformPayloadKindNull || _reference == nil ||
      _sequence == std::numeric_limits<int64_t>::max()) {
    [result failWithCode:@"example/invalid-request" message:@"HuxerUI frame source cannot fulfill this request"
                details:HUXPlatformPayload.nullValue];
    return nil;
  }
  // A request arrives only after the C++ reader has finished with the preceding frame.
  demo::Fill({static_cast<std::byte*>(_storage.mutableBytes), _storage.length}, ++_sequence);
  [result complete:[HUXPlatformPayload objectValue:@{
    @"pixels" : [HUXPlatformPayload bufferReferenceValue:_reference],
    @"width" : [HUXPlatformPayload integerValue:demo::frame_width],
    @"height" : [HUXPlatformPayload integerValue:demo::frame_height],
    @"stride" : [HUXPlatformPayload integerValue:demo::row_stride],
    @"sequence" : [HUXPlatformPayload integerValue:_sequence],
  }]];
  return nil;
}

- (void)dispose {
  _reference = nil;
  _storage = nil;
}

@end

#if TARGET_OS_IOS
@interface HuxerUIExampleFrameSourceFactory : NSObject <HUXUIKitPlatformModuleFactory>
#else
@interface HuxerUIExampleFrameSourceFactory : NSObject <HUXAppKitPlatformModuleFactory>
#endif
@end

@implementation HuxerUIExampleFrameSourceFactory

#if TARGET_OS_IOS
- (id<HUXPlatformModule>)createWithViewController:(UIViewController*)controller options:(HUXPlatformPayload*)options
                                         events:(id<HUXPlatformEventEmitter>)events {
#else
- (id<HUXPlatformModule>)createWithWindow:(NSWindow*)window options:(HUXPlatformPayload*)options
                                 events:(id<HUXPlatformEventEmitter>)events {
#endif
  return [HuxerUIExampleFrameSource new];
}

@end

namespace huxerui::example::buffer_reference {

void Install(RootContext& root) {
  apple::ObjectiveCPlatformModuleFactory<std::shared_ptr<FrameSource>> factory{
      .factory = [HuxerUIExampleFrameSourceFactory new],
      .create = CreateBridgeSource,
  };
  root.RegisterPlatformModule<std::shared_ptr<FrameSource>>(module_name, std::move(factory));
  root.Provide(root.OpenPlatformModule<std::shared_ptr<FrameSource>>(module_name));
}

} // namespace huxerui::example::buffer_reference
