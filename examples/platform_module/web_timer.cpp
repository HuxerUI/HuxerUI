#include "timer.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <emscripten/eventloop.h>

namespace {

huxerui::PlatformError TimerError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

struct PendingStart {
  std::uint64_t generation = 0;
  huxerui::PlatformResultSink result;
};

struct WebTimerState : std::enable_shared_from_this<WebTimerState> {
  explicit WebTimerState(huxerui::PlatformEventSink event_sink) : events(std::move(event_sink)) {}

  void InvalidateTimer() noexcept {
    ++generation;
    if (timer != 0) {
      emscripten_clear_interval(timer);
      timer = 0;
    }
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

  std::function<void()> Start(std::int64_t milliseconds, huxerui::PlatformResultSink result) {
    if (milliseconds <= 0 || milliseconds > std::numeric_limits<std::int32_t>::max()) {
      result(TimerError("example/invalid-interval", "The timer interval is outside the browser timer range"));
      return {};
    }

    StopWithError(TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
    tick = 0;
    const std::uint64_t timer_generation = generation;
    pending_start = PendingStart{timer_generation, std::move(result)};
    timer = emscripten_set_interval(TimerCallback, static_cast<double>(milliseconds), this);
    if (timer == 0) {
      StopWithError(TimerError("example/timer-failed", "The browser timer could not be created"));
      return {};
    }

    const std::weak_ptr<WebTimerState> weak_state = weak_from_this();
    return [weak_state, timer_generation] {
      if (const std::shared_ptr<WebTimerState> state = weak_state.lock()) {
        state->CancelStart(timer_generation);
      }
    };
  }

  void Stop(huxerui::PlatformResultSink result) {
    StopWithError(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
    result(huxerui::PlatformPayload());
  }

  void Dispose() noexcept {
    pending_start.reset();
    InvalidateTimer();
    events = {};
  }

private:
  static void TimerCallback(void* context) noexcept {
    try {
      static_cast<WebTimerState*>(context)->Tick();
    } catch (...) {
    }
  }

  void CancelStart(std::uint64_t timer_generation) noexcept {
    if (generation != timer_generation) {
      return;
    }
    pending_start.reset();
    InvalidateTimer();
  }

  void Tick() {
    if (timer == 0) {
      return;
    }
    ++tick;
    if (pending_start && pending_start->generation == generation) {
      huxerui::PlatformResultSink result = std::move(pending_start->result);
      pending_start.reset();
      result(huxerui::PlatformPayload(tick));
    }
    events(huxerui::example::timer::tick_event, huxerui::PlatformPayload(tick));
  }

  huxerui::PlatformEventSink events;
  std::optional<PendingStart> pending_start;
  int timer = 0;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
};

huxerui::PlatformModuleFactory WebTimerFactory() {
  huxerui::PlatformModuleFactory factory;
  factory.create = [](const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events) {
    static_cast<void>(options);
    auto state = std::make_shared<WebTimerState>(std::move(events));
    huxerui::PlatformModuleFactory::Instance instance;
    instance.call = [state](std::string method, huxerui::PlatformPayload arguments, huxerui::PlatformResultSink result)
        -> std::function<void()> {
      if (method == huxerui::example::timer::start_method) {
        std::int64_t milliseconds = 0;
        try {
          milliseconds = arguments.AsInteger();
        } catch (...) {
          result(TimerError("example/invalid-interval", "The timer interval payload is invalid"));
          return {};
        }
        return state->Start(milliseconds, std::move(result));
      }

      if (method == huxerui::example::timer::stop_method) {
        state->Stop(std::move(result));
        return {};
      }

      result(TimerError("example/unknown-method", "The platform timer method is not supported"));
      return {};
    };
    instance.dispose = [state] { state->Dispose(); };
    return instance;
  };
  return factory;
}

} // namespace

namespace huxerui::example {

void InstallTimer(RootContext& root) {
  root.Modules().Register(timer::type, WebTimerFactory());
  root.Provide(std::make_shared<TimerService>(root.Modules().Open(timer::type)));
}

} // namespace huxerui::example
