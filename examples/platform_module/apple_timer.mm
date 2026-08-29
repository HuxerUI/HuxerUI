#include "apple_timer.h"

#import <Foundation/Foundation.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

#include <huxerui/app.h>

namespace {

struct TimerTick : huxerui::Event<std::uint64_t> {
  static constexpr char Name[] = "tick";
};

struct AppleTimerCallbacks {
  std::function<void(std::uint64_t)> tick;
};

class AppleTimerService final : public huxerui::example::TimerService {
public:
  explicit AppleTimerService(huxerui::PlatformChannel channel)
      : channel_(std::move(channel)), callbacks_(std::make_shared<AppleTimerCallbacks>()) {
    channel_.On<TimerTick>([callbacks = callbacks_](std::uint64_t tick) {
      if (callbacks->tick) {
        callbacks->tick(tick);
      }
    });
  }

  ~AppleTimerService() override {
    callbacks_->tick = {};
    channel_.Close();
  }

  huxerui::PlatformRequestId Start(std::chrono::milliseconds interval, std::function<void(std::uint64_t)> handler,
                                   std::function<void(huxerui::PlatformResult<std::uint64_t>)> completion) override {
    if (interval <= std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("HuxerUI example timer interval must be greater than zero");
    }
    if (!handler || !completion) {
      throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
    }
    callbacks_->tick = std::move(handler);
    return channel_.Invoke<std::uint64_t>("start", interval.count(), std::move(completion));
  }

  huxerui::PlatformRequestId Stop(std::function<void(huxerui::PlatformResult<std::monostate>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example timer completion must not be empty");
    }
    callbacks_->tick = {};
    return channel_.Invoke<std::monostate>("stop", std::move(completion));
  }

  bool Cancel(huxerui::PlatformRequestId request) override {
    const bool cancelled = channel_.Cancel(request);
    if (cancelled) {
      callbacks_->tick = {};
    }
    return cancelled;
  }

private:
  huxerui::PlatformChannel channel_;
  std::shared_ptr<AppleTimerCallbacks> callbacks_;
};

} // namespace

@interface HuxerUIExampleTimerCancellation : NSObject <HUXPlatformCancellation>
- (instancetype)initWithBlock:(void (^)(void))block;
@end

@implementation HuxerUIExampleTimerCancellation {
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

@interface HuxerUIExampleTimerModule : NSObject <HUXPlatformModule>
- (instancetype)initWithEvents:(id<HUXPlatformEventEmitter>)events;
@end

@implementation HuxerUIExampleTimerModule {
  __strong id<HUXPlatformEventEmitter> _events;
  __strong id<HUXPlatformResult> _pending_start;
  __strong NSTimer* _timer;
  uint64_t _generation;
  uint64_t _tick;
}

- (instancetype)initWithEvents:(id<HUXPlatformEventEmitter>)events {
  self = [super init];
  if (self != nil) {
    _events = events;
  }
  return self;
}

- (void)invalidateTimer {
  ++_generation;
  [_timer invalidate];
  _timer = nil;
}

- (void)finishPendingStartWithCode:(NSString*)code message:(NSString*)message {
  id<HUXPlatformResult> pending = _pending_start;
  _pending_start = nil;
  if (pending != nil) {
    [pending failWithCode:code message:message details:HUXPlatformPayload.nullValue];
  }
}

- (void)cancelGeneration:(uint64_t)generation {
  if (_generation != generation) {
    return;
  }
  _pending_start = nil;
  [self invalidateTimer];
}

- (id<HUXPlatformCancellation>)startWithArguments:(HUXPlatformPayload*)arguments
                                            result:(id<HUXPlatformResult>)result {
  if (arguments.kind != HUXPlatformPayloadKindInteger || arguments.integerValue <= 0) {
    [result failWithCode:@"example/invalid-interval"
                 message:@"The timer interval must be greater than zero"
                 details:HUXPlatformPayload.nullValue];
    return nil;
  }

  [self finishPendingStartWithCode:@"example/timer-replaced"
                           message:@"The timer was replaced by a newer start call"];
  [self invalidateTimer];
  _tick = 0;
  _pending_start = result;
  const uint64_t generation = _generation;
  const NSTimeInterval seconds = static_cast<NSTimeInterval>(arguments.integerValue) / 1000.0;
  __weak HuxerUIExampleTimerModule* weak_self = self;
  _timer = [NSTimer timerWithTimeInterval:seconds
                                  repeats:YES
                                    block:^(NSTimer*) {
                                      HuxerUIExampleTimerModule* strong_self = weak_self;
                                      if (strong_self == nil || strong_self->_generation != generation) {
                                        return;
                                      }
                                      ++strong_self->_tick;
                                      id<HUXPlatformResult> pending = strong_self->_pending_start;
                                      strong_self->_pending_start = nil;
                                      if (pending != nil) {
                                        [pending complete:[HUXPlatformPayload integerValue:strong_self->_tick]];
                                      }
                                      [strong_self->_events emit:@"tick"
                                                         payload:[HUXPlatformPayload integerValue:strong_self->_tick]];
                                    }];
  [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];

  return [[HuxerUIExampleTimerCancellation alloc] initWithBlock:^{
    [weak_self cancelGeneration:generation];
  }];
}

- (id<HUXPlatformCancellation>)invoke:(NSString*)method
                            arguments:(HUXPlatformPayload*)arguments
                               result:(id<HUXPlatformResult>)result {
  if ([method isEqualToString:@"start"]) {
    return [self startWithArguments:arguments result:result];
  }
  if ([method isEqualToString:@"stop"]) {
    [self finishPendingStartWithCode:@"example/timer-stopped"
                             message:@"The timer stopped before its first tick"];
    [self invalidateTimer];
    [result complete:HUXPlatformPayload.nullValue];
    return nil;
  }
  [result failWithCode:@"example/unknown-method"
               message:@"The timer does not implement the requested method"
               details:HUXPlatformPayload.nullValue];
  return nil;
}

- (void)dispose {
  _pending_start = nil;
  [self invalidateTimer];
  _events = nil;
}

@end

#if TARGET_OS_IOS
@interface HuxerUIExampleTimerFactory : NSObject <HUXUIKitPlatformModuleFactory>
@end

@implementation HuxerUIExampleTimerFactory

- (id<HUXPlatformModule>)createWithViewController:(UIViewController*)view_controller
                                           options:(HUXPlatformPayload*)options
                                            events:(id<HUXPlatformEventEmitter>)events {
  static_cast<void>(view_controller);
  static_cast<void>(options);
  return [[HuxerUIExampleTimerModule alloc] initWithEvents:events];
}

@end
#else
@interface HuxerUIExampleTimerFactory : NSObject <HUXAppKitPlatformModuleFactory>
@end

@implementation HuxerUIExampleTimerFactory

- (id<HUXPlatformModule>)createWithWindow:(NSWindow*)window
                                  options:(HUXPlatformPayload*)options
                                   events:(id<HUXPlatformEventEmitter>)events {
  static_cast<void>(window);
  static_cast<void>(options);
  return [[HuxerUIExampleTimerModule alloc] initWithEvents:events];
}

@end
#endif

namespace huxerui::example {

std::shared_ptr<TimerService> CreateAppleTimerService(PlatformChannel channel) {
  return std::static_pointer_cast<TimerService>(std::make_shared<AppleTimerService>(std::move(channel)));
}

#if TARGET_OS_IOS
id<HUXUIKitPlatformModuleFactory> CreateAppleTimerFactory() {
#else
id<HUXAppKitPlatformModuleFactory> CreateAppleTimerFactory() {
#endif
  return [HuxerUIExampleTimerFactory new];
}

void InstallTimer(RootContext& root) {
#if TARGET_OS_IOS
  ios::ObjectiveCPlatformModuleFactory<std::shared_ptr<TimerService>> factory{
#else
  macos::ObjectiveCPlatformModuleFactory<std::shared_ptr<TimerService>> factory{
#endif
      .factory = CreateAppleTimerFactory(),
      .create = CreateAppleTimerService,
  };
  root.RegisterPlatformModule<std::shared_ptr<TimerService>>(timer::type, std::move(factory));
  root.Provide(root.OpenPlatformModule<std::shared_ptr<TimerService>>(timer::type));
}

} // namespace huxerui::example
