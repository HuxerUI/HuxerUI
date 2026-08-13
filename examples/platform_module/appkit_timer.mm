#include "timer.h"

#import <AppKit/AppKit.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <huxerui/event.h>

namespace {

constexpr std::string_view timer_type = "example/Timer";

std::uint64_t DecodeTimerTick(const huxerui::PlatformPayload& payload) {
  const std::int64_t tick = payload.AsInteger();
  if (tick < 0) {
    throw std::invalid_argument("HuxerUI example timer tick payload must not be negative");
  }
  return static_cast<std::uint64_t>(tick);
}

struct TimerMethods {
  struct Start {
    using Request = std::chrono::milliseconds;
    using Result = std::uint64_t;
    static constexpr std::string_view Name = "start";

    static huxerui::PlatformPayload Encode(Request interval) {
      return interval.count();
    }

    static Result Decode(const huxerui::PlatformPayload& payload) {
      return DecodeTimerTick(payload);
    }
  };

  struct Stop {
    using Request = std::monostate;
    using Result = std::monostate;
    static constexpr std::string_view Name = "stop";

    static huxerui::PlatformPayload Encode(const Request&) {
      return {};
    }

    static Result Decode(const huxerui::PlatformPayload& payload) {
      if (!payload.IsNull()) {
        throw std::invalid_argument("HuxerUI example timer stop result must be null");
      }
      return {};
    }
  };
};

struct TimerEvents {
  struct Tick : huxerui::Event<std::uint64_t> {
    static constexpr std::string_view Name = "tick";

    static std::uint64_t Decode(const huxerui::PlatformPayload& payload) {
      return DecodeTimerTick(payload);
    }
  };
};

struct PendingStart {
  std::uint64_t generation = 0;
  huxerui::PlatformResultSink result;
};

struct AppKitTimerState {
  explicit AppKitTimerState(huxerui::PlatformEventSink event_sink) : events(std::move(event_sink)) {}

  void InvalidateTimer() {
    ++generation;
    [timer invalidate];
    timer = nil;
  }

  void StopWithError(huxerui::PlatformError error) {
    std::shared_ptr<PendingStart> pending = std::move(pending_start);
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
    events(std::string(TimerEvents::Tick::Name), huxerui::PlatformPayload(tick));
  }

  huxerui::PlatformEventSink events;
  __strong NSTimer* timer = nil;
  std::shared_ptr<PendingStart> pending_start;
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

huxerui::PlatformModuleFactory AppKitTimerFactory() {
  huxerui::PlatformModuleFactory factory;
  factory.create = [](const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events) {
    static_cast<void>(options);
    auto state = std::make_shared<AppKitTimerState>(std::move(events));
    huxerui::PlatformModuleFactory::Instance instance;
    instance.call = [state](std::string method, huxerui::PlatformPayload arguments, huxerui::PlatformResultSink result)
        -> std::function<void()> {
      if (!NSThread.isMainThread) {
        result(TimerError("example/timer-thread", "The native timer must be used from the AppKit main thread"));
        return {};
      }

      if (method == TimerMethods::Start::Name) {
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
        state->pending_start = std::make_shared<PendingStart>(PendingStart{
            timer_generation,
            std::move(result),
        });
        const NSTimeInterval interval = static_cast<NSTimeInterval>(milliseconds) / 1000.0;
        std::weak_ptr<AppKitTimerState> weak_state = state;
        state->timer =
            [NSTimer timerWithTimeInterval:interval
                                   repeats:YES
                                     block:^(NSTimer*) {
                                       if (const std::shared_ptr<AppKitTimerState> strong_state = weak_state.lock()) {
                                         try {
                                           strong_state->Tick(timer_generation);
                                         } catch (...) {
                                         }
                                       }
                                     }];
        [NSRunLoop.mainRunLoop addTimer:state->timer forMode:NSRunLoopCommonModes];
        return [weak_state, timer_generation] {
          if (const std::shared_ptr<AppKitTimerState> strong_state = weak_state.lock()) {
            strong_state->CancelStart(timer_generation);
          }
        };
      }

      if (method == TimerMethods::Stop::Name) {
        state->StopWithError(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
        result(huxerui::PlatformPayload());
        return {};
      }

      result(TimerError("example/unknown-method", "The native timer method is not supported"));
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

TimerService::TimerService(PlatformInstance instance) : instance_(std::move(instance)) {
  instance_.On<TimerEvents::Tick>([this](std::uint64_t tick) {
    if (tick_handler_) {
      tick_handler_(tick);
    }
  });
}

PlatformRequestId TimerService::Start(
    std::chrono::milliseconds interval,
    TickHandler tick_handler,
    std::function<void(PlatformResult<std::uint64_t>)> completion
) {
  if (interval <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("HuxerUI example timer interval must be greater than zero");
  }
  if (!tick_handler || !completion) {
    throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
  }
  tick_handler_ = std::move(tick_handler);
  return instance_.Call<TimerMethods::Start>(interval, std::move(completion));
}

PlatformRequestId TimerService::Stop(std::function<void(PlatformResult<std::monostate>)> completion) {
  if (!completion) {
    throw std::invalid_argument("HuxerUI example timer completion must not be empty");
  }
  tick_handler_ = {};
  return instance_.Call<TimerMethods::Stop>(std::monostate{}, std::move(completion));
}

bool TimerService::Cancel(PlatformRequestId request) {
  return instance_.Cancel(request);
}

std::shared_ptr<TimerService> UseTimer() {
  return UseService<TimerService>();
}

RootHook InstallTimer() {
  return [](RootContext& root) {
    root.Modules().Register(std::string(timer_type), AppKitTimerFactory());
    root.Provide(std::make_shared<TimerService>(root.Modules().Open(std::string(timer_type))));
  };
}

} // namespace huxerui::example
