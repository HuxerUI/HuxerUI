#include "ios_presentation_internal.h"

#import <objc/runtime.h>

namespace {
char coordinator_key;

void RequireMainThread() {
  if (!NSThread.isMainThread) {
    [NSException raise:NSInternalInconsistencyException format:@"HuxerUI presentation requires the main thread"];
  }
}

NSError* PresentationError(NSInteger code, NSString* message) {
  return [NSError errorWithDomain:@"HuxerUI.Presentation" code:code
                        userInfo:@{NSLocalizedDescriptionKey : message}];
}
} // namespace

@interface HUXPresentationRequest : NSObject <HUXPlatformCancellation>
@property(nonatomic, weak) HUXPresentationCoordinator* coordinator;
@property(nonatomic, copy) id<HUXPlatformCancellation> (^start)(UIViewController*, void (^)(void));
@property(nonatomic, copy) void (^failure)(NSError*);
@property(nonatomic, strong) id<HUXPlatformCancellation> nativeCancellation;
@property(nonatomic) BOOL cancelled;
@property(nonatomic) BOOL finished;
- (void)fail:(NSError*)error;
- (void)cancelNative;
@end

@interface HUXPresentationCoordinator ()
- (void)cancelRequest:(HUXPresentationRequest*)request;
- (void)finishRequest:(HUXPresentationRequest*)request;
- (void)schedule;
- (void)drain;
@end

@implementation HUXPresentationRequest
- (void)cancel {
  RequireMainThread();
  [self.coordinator cancelRequest:self];
}

- (void)cancelNative {
  id<HUXPlatformCancellation> cancellation = self.nativeCancellation;
  self.nativeCancellation = nil;
  try {
    @try { [cancellation cancel]; }
    @catch (NSException*) {}
  } catch (...) {
    // A failed native cleanup must not release a still-presented controller's slot.
  }
}

- (void)fail:(NSError*)error {
  void (^failure)(NSError*) = self.failure;
  self.failure = nil;
  if (!self.cancelled && failure != nil) {
    try {
      @try { failure(error); }
      @catch (NSException*) {}
    } catch (...) {
      // A consumer exception must not prevent the remaining requests from being retired.
    }
  }
}
@end

@implementation HUXPresentationCoordinator {
  __weak UIViewController* _owner;
  NSMutableArray<HUXPresentationRequest*>* _pending;
  HUXPresentationRequest* _current;
  BOOL _active;
  BOOL _closed;
  BOOL _scheduled;
}

- (instancetype)initWithOwner:(UIViewController*)owner {
  RequireMainThread();
  self = [super init];
  if (self != nil) {
    _owner = owner;
    _pending = [NSMutableArray new];
    objc_setAssociatedObject(owner, &coordinator_key, self, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  return self;
}

- (id<HUXPlatformCancellation>)enqueueWithStart:
    (id<HUXPlatformCancellation> (^)(UIViewController*, void (^)(void)))start
    failure:(void (^)(NSError*))failure {
  RequireMainThread();
  if (start == nil || failure == nil) {
    [NSException raise:NSInvalidArgumentException format:@"HuxerUI presentation requires start and failure"];
  }
  HUXPresentationRequest* request = [HUXPresentationRequest new];
  request.coordinator = self;
  request.start = start;
  request.failure = failure;
  [_pending addObject:request];
  [self schedule];
  return request;
}

- (void)setActive:(BOOL)active {
  RequireMainThread();
  _active = active;
  [self schedule];
}

- (void)schedule {
  if (_scheduled || _pending.count == 0) return;
  _scheduled = YES;
  dispatch_async(dispatch_get_main_queue(), ^{
    self->_scheduled = NO;
    [self drain];
  });
}

- (void)drain {
  if (_closed || _owner == nil) {
    [self close];
    // enqueue remains asynchronous even when called on a retained, retired coordinator.
    NSArray<HUXPresentationRequest*>* pending = [_pending copy];
    [_pending removeAllObjects];
    for (HUXPresentationRequest* request in pending) {
      request.finished = YES;
      request.start = nil;
      [request fail:PresentationError(1, @"HuxerUI presentation window is closed")];
    }
    return;
  }
  if (!_active || _current != nil || _pending.count == 0) return;
  HUXPresentationRequest* request = _pending.firstObject;
  [_pending removeObjectAtIndex:0];
  UIViewController* owner = _owner;
  NSError* error = nil;
  if (owner.viewIfLoaded.window == nil) {
    error = PresentationError(2, @"HuxerUI presentation window is unavailable");
  } else if (owner.presentedViewController != nil || owner.isBeingDismissed || owner.isBeingPresented) {
    error = PresentationError(3, @"HuxerUI presentation window is busy");
  }
  if (error != nil) {
    request.finished = YES;
    request.start = nil;
    [request fail:error];
    [self schedule];
    return;
  }
  _current = request;
  auto start = request.start;
  request.start = nil;
  __weak HUXPresentationCoordinator* coordinator = self;
  __weak HUXPresentationRequest* weak_request = request;
  id<HUXPlatformCancellation> cancellation = nil;
  try {
    @try {
      cancellation = start(owner, ^{
        RequireMainThread();
        [coordinator finishRequest:weak_request];
      });
    } @catch (NSException*) {
      error = PresentationError(4, @"HuxerUI presentation start raised an exception");
    }
  } catch (...) {
    error = PresentationError(4, @"HuxerUI presentation start raised an exception");
  }
  if (request.finished) return;
  if (cancellation == nil || ![cancellation respondsToSelector:@selector(cancel)]) {
    [request fail:error != nil ? error : PresentationError(4, @"HuxerUI active presentation requires native cancellation")];
    // Do not free an installed UI merely because its starter broke the cancellation contract.
    if (owner.presentedViewController == nil) [self finishRequest:request];
    return;
  }
  request.nativeCancellation = cancellation;
  // cancel/close can reenter while start has not yet returned its native cancellation object.
  if (request.cancelled || _closed) [request cancelNative];
}

- (void)finishRequest:(HUXPresentationRequest*)request {
  RequireMainThread();
  if (request == nil || request.finished || _current != request) return;
  request.finished = YES;
  request.nativeCancellation = nil;
  request.failure = nil;
  _current = nil;
  [self schedule];
}

- (void)cancelRequest:(HUXPresentationRequest*)request {
  if (request.finished || request.cancelled) return;
  request.cancelled = YES;
  request.failure = nil;
  if (_current == request) {
    [request cancelNative];
  } else {
    [_pending removeObjectIdenticalTo:request];
    request.start = nil;
    request.finished = YES;
  }
}

- (void)close {
  RequireMainThread();
  if (_closed) return;
  _closed = YES;
  NSArray<HUXPresentationRequest*>* pending = [_pending copy];
  [_pending removeAllObjects];
  HUXPresentationRequest* current = _current;
  NSError* error = PresentationError(1, @"HuxerUI presentation window is closed");
  for (HUXPresentationRequest* request in pending) {
    request.finished = YES;
    request.start = nil;
    [request fail:error];
  }
  [current fail:error];
  [current cancelNative];
}
@end

HUXPresentationCoordinator* HUXGetPresentationCoordinator(UIViewController* owner) {
  RequireMainThread();
  HUXPresentationCoordinator* coordinator = objc_getAssociatedObject(owner, &coordinator_key);
  if (coordinator == nil) {
    [NSException raise:NSInvalidArgumentException format:@"HuxerUI presentation requires its original window controller"];
  }
  return coordinator;
}
