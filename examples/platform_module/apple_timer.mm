#include "timer.h"

#import <Foundation/Foundation.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <huxerui/app.h>

namespace {

huxerui::PlatformError TimerError(std::string code, std::string message);

struct PendingStart {
  std::uint64_t generation = 0;
  std::function<void(huxerui::PlatformResult<std::uint64_t>)> completion;
};

struct AppleTimerState : huxerui::example::TimerService, std::enable_shared_from_this<AppleTimerState> {
  explicit AppleTimerState(huxerui::PlatformAdapter& adapter_value) : adapter(&adapter_value) {}

  ~AppleTimerState() override {
    pending_start.reset();
    InvalidateTimer();
  }

  void InvalidateTimer() {
    ++generation;
    [timer invalidate];
    timer = nil;
  }

  void StopWithError(huxerui::PlatformError error) {
    std::optional<PendingStart> pending = std::move(pending_start);
    pending_start.reset();
    InvalidateTimer();
    if (pending && pending->completion) {
      Complete(std::move(pending->completion), std::move(error));
    }
  }

  bool Cancel(huxerui::PlatformRequestId cancelled_generation) override {
    if (generation != cancelled_generation) {
      return false;
    }
    pending_start.reset();
    tick_handler = {};
    InvalidateTimer();
    return true;
  }

  void Tick(std::uint64_t timer_generation) {
    if (generation != timer_generation || timer == nil) {
      return;
    }
    ++tick;
    if (pending_start && pending_start->generation == timer_generation) {
      auto completion = std::move(pending_start->completion);
      pending_start.reset();
      Complete(std::move(completion), tick);
    }
    std::function<void(std::uint64_t)> handler = tick_handler;
    adapter->DispatchToUIThread([handler = std::move(handler), next_tick = tick] {
      if (handler) {
        handler(next_tick);
      }
    });
  }

  huxerui::PlatformRequestId Start(std::chrono::milliseconds interval, std::function<void(std::uint64_t)> handler,
                                   std::function<void(huxerui::PlatformResult<std::uint64_t>)> completion) override {
    if (!NSThread.isMainThread) {
      throw std::logic_error("HuxerUI example Apple timer must be used on the main thread");
    }
    if (interval <= std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("HuxerUI example timer interval must be greater than zero");
    }
    if (!handler || !completion) {
      throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
    }

    StopWithError(TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
    tick = 0;
    const std::uint64_t timer_generation = generation;
    tick_handler = std::move(handler);
    pending_start = PendingStart{timer_generation, std::move(completion)};
    const NSTimeInterval seconds = static_cast<NSTimeInterval>(interval.count()) / 1000.0;
    std::weak_ptr<AppleTimerState> weak_state = weak_from_this();
    timer = [NSTimer timerWithTimeInterval:seconds
                                   repeats:YES
                                     block:^(NSTimer*) {
                                       if (const std::shared_ptr<AppleTimerState> state = weak_state.lock()) {
                                         state->Tick(timer_generation);
                                       }
                                     }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    return timer_generation;
  }

  huxerui::PlatformRequestId Stop(std::function<void(huxerui::PlatformResult<std::monostate>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example timer completion must not be empty");
    }
    StopWithError(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
    tick_handler = {};
    const std::uint64_t request = generation;
    Complete(std::move(completion), std::monostate{});
    return request;
  }

  template <class Result, class Value>
  void Complete(std::function<void(huxerui::PlatformResult<Result>)> completion, Value&& value) {
    huxerui::PlatformResult<Result> result(std::forward<Value>(value));
    adapter->DispatchToUIThread(
        [completion = std::move(completion), result = std::move(result)]() mutable { completion(std::move(result)); });
  }

  huxerui::PlatformAdapter* adapter;
  std::function<void(std::uint64_t)> tick_handler;
  __strong NSTimer* timer = nil;
  std::optional<PendingStart> pending_start;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
};

huxerui::PlatformError TimerError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

} // namespace

namespace huxerui::example {

void InstallTimer(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<TimerService>>(timer::type, [](PlatformAdapter& adapter) {
    return std::static_pointer_cast<TimerService>(std::make_shared<AppleTimerState>(adapter));
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<TimerService>>(timer::type));
}

} // namespace huxerui::example
