@import HuxerUIPlatform;

@interface HuxerUITestPlatformModule : NSObject <HUXPlatformModule>
@end

@implementation HuxerUITestPlatformModule

- (id<HUXPlatformCancellation>)invoke:(NSString*)method
                            arguments:(HUXPlatformPayload*)arguments
                               result:(id<HUXPlatformResult>)result {
  [result complete:[HUXPlatformPayload objectValue:@{
                    @"method" : [HUXPlatformPayload stringValue:method],
                    @"arguments" : arguments,
                  }]];
  return nil;
}

- (void)dispose {
}

@end

@interface HuxerUITestPlatformModuleFactory : NSObject <HUXAppKitPlatformModuleFactory>
@end

@implementation HuxerUITestPlatformModuleFactory

- (id<HUXPlatformModule>)createWithWindow:(NSWindow*)window
                                  options:(HUXPlatformPayload*)options
                                   events:(id<HUXPlatformEventEmitter>)events {
  (void)window;
  [events emit:@"created" payload:options];
  return [HuxerUITestPlatformModule new];
}

@end

@interface HuxerUITestPlatformView : NSObject <HUXAppKitPlatformView>
@property(nonatomic, readonly) NSView* view;
@end

@implementation HuxerUITestPlatformView

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _view = [NSView new];
  }
  return self;
}

- (void)dispose {
}

@end

@interface HuxerUITestPlatformViewFactory : NSObject <HUXAppKitPlatformViewFactory>
@end

@implementation HuxerUITestPlatformViewFactory

- (id<HUXAppKitPlatformView>)createWithWindow:(NSWindow*)window
                                   properties:(HUXPlatformPayload*)properties
                                       events:(id<HUXPlatformEventEmitter>)events {
  (void)window;
  [events emit:@"created" payload:properties];
  return [HuxerUITestPlatformView new];
}

@end
