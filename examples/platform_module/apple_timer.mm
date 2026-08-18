#include "timer.h"

#import <Foundation/Foundation.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

struct PendingStart {
  std::uint64_t generation = 0;
  huxerui::PlatformResultSink result;
};

struct AppleTimerState {
  explicit AppleTimerState(huxerui::PlatformEventSink event_sink) : events(std::move(event_sink)) {}

  void InvalidateTimer() {
    ++generation;
    [timer invalidate];
    timer = nil;
  }

  void StopWithError(huxerui::PlatformError error) {
    std::optional<PendingStart> pending = std::move(pending_start);
    pending_start.reset();
    InvalidateTimer();
    if (pending && pending->result) {
      huxerui::PlatformResultSink result = std::move(pending->result);
      result(std::move(error));
    }
  }

  void CancelStart(std::uint64_t cancelled_generation) {
    if (generation != cancelled_generation) {
      return;
    }
    pending_start.reset();
    InvalidateTimer();
  }

  void Tick(std::uint64_t timer_generation) {
    if (generation != timer_generation || timer == nil) {
      return;
    }
    ++tick;
    if (pending_start && pending_start->generation == timer_generation) {
      huxerui::PlatformResultSink result = std::move(pending_start->result);
      pending_start.reset();
      result(huxerui::PlatformPayload(tick));
    }
    events(huxerui::example::timer::tick_event, huxerui::PlatformPayload(tick));
  }

  huxerui::PlatformEventSink events;
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

huxerui::PlatformModuleFactory AppleTimerFactory() {
  huxerui::PlatformModuleFactory factory;
  factory.create = [](const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events) {
    static_cast<void>(options);
    auto state = std::make_shared<AppleTimerState>(std::move(events));
    huxerui::PlatformModuleFactory::Instance instance;
    instance.call = [state](std::string method, huxerui::PlatformPayload arguments, huxerui::PlatformResultSink result)
        -> std::function<void()> {
      if (!NSThread.isMainThread) {
        result(TimerError("example/timer-thread", "The platform timer must be used from the main thread"));
        return {};
      }

      if (method == huxerui::example::timer::start_method) {
        std::int64_t milliseconds = 0;
        try {
          milliseconds = arguments.AsInteger();
        } catch (...) {
          result(TimerError("example/invalid-interval", "The timer interval payload is invalid"));
          return {};
        }
        if (milliseconds <= 0) {
          result(TimerError("example/invalid-interval", "The timer interval must be greater than zero"));
          return {};
        }

        state->StopWithError(TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
        state->tick = 0;
        const std::uint64_t timer_generation = state->generation;
        state->pending_start = PendingStart{timer_generation, std::move(result)};
        const NSTimeInterval interval = static_cast<NSTimeInterval>(milliseconds) / 1000.0;
        std::weak_ptr<AppleTimerState> weak_state = state;
        state->timer =
            [NSTimer timerWithTimeInterval:interval
                                   repeats:YES
                                     block:^(NSTimer*) {
                                       if (const std::shared_ptr<AppleTimerState> strong_state = weak_state.lock()) {
                                         try {
                                           strong_state->Tick(timer_generation);
                                         } catch (...) {
                                         }
                                       }
                                     }];
        [NSRunLoop.mainRunLoop addTimer:state->timer forMode:NSRunLoopCommonModes];
        return [weak_state, timer_generation] {
          if (const std::shared_ptr<AppleTimerState> strong_state = weak_state.lock()) {
            strong_state->CancelStart(timer_generation);
          }
        };
      }

      if (method == huxerui::example::timer::stop_method) {
        state->StopWithError(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
        result(huxerui::PlatformPayload());
        return {};
      }

      result(TimerError("example/unknown-method", "The platform timer method is not supported"));
      return {};
    };
    instance.dispose = [state] {
      state->pending_start.reset();
      state->InvalidateTimer();
      state->events = {};
    };
    return instance;
  };
  return factory;
}

} // namespace

namespace huxerui::example {

void InstallTimer(RootContext& root) {
  root.Modules().Register(timer::type, AppleTimerFactory());
  root.Provide(std::make_shared<TimerService>(root.Modules().Open(timer::type)));
}

} // namespace huxerui::example
