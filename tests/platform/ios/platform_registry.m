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

@interface HuxerUITestPlatformModuleFactory : NSObject <HUXUIKitWindowPlatformModuleFactory>
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

static void HuxerUITestFileReferenceDeclarations(HUXFileReference* reference) {
  NSURL* url = reference.fileURL;
  NSError* error = nil;
  HUXFileReference* copied = [HUXFileReference copyFromFileURL:url error:&error];
  HUXFileReference* round_trip = [HUXPlatformPayload fileReferenceValue:reference].fileReferenceValue;
  (void)copied;
  (void)url;
  (void)round_trip;
}

@interface HuxerUITestApplicationModuleFactory : NSObject <HUXPlatformModuleFactory>
@end
@implementation HuxerUITestApplicationModuleFactory
- (id<HUXPlatformModule>)createWithOptions:(HUXPlatformPayload*)options
                                   events:(id<HUXPlatformEventEmitter>)events {
  [events emit:@"created" payload:options];
  return [HuxerUITestPlatformModule new];
}
@end

static id<HUXPlatformCancellation> HuxerUITestPresentation(UIViewController* owner) {
  return [HUXGetPresentationCoordinator(owner) enqueueWithStart:
      ^id<HUXPlatformCancellation>(UIViewController* presenter, void (^finished)(void)) {
    (void)presenter;
    finished();
    return nil;
  } failure:^(NSError* error) { (void)error; }];
}
