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

@interface HuxerUITestPlatformModuleFactory : NSObject <HUXUIKitPlatformModuleFactory>
@end

@implementation HuxerUITestPlatformModuleFactory

- (id<HUXPlatformModule>)createWithViewController:(UIViewController*)viewController
                                           options:(HUXPlatformPayload*)options
                                            events:(id<HUXPlatformEventEmitter>)events {
  (void)viewController;
  [events emit:@"created" payload:options];
  return [HuxerUITestPlatformModule new];
}

@end

@interface HuxerUITestPlatformView : NSObject <HUXUIKitPlatformView>
@property(nonatomic, readonly) UIView* view;
@end

@implementation HuxerUITestPlatformView

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _view = [UIView new];
  }
  return self;
}

- (void)dispose {
}

@end

@interface HuxerUITestPlatformViewFactory : NSObject <HUXUIKitPlatformViewFactory>
@end

@implementation HuxerUITestPlatformViewFactory

- (id<HUXUIKitPlatformView>)createWithViewController:(UIViewController*)viewController
                                           properties:(HUXPlatformPayload*)properties
                                               events:(id<HUXPlatformEventEmitter>)events {
  (void)viewController;
  [events emit:@"created" payload:properties];
  return [HuxerUITestPlatformView new];
}

@end
