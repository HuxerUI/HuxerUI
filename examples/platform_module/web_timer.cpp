#include "timer.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <emscripten/eventloop.h>

#include <huxerui/app.h>

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
  std::function<void(huxerui::PlatformResult<std::uint64_t>)> completion;
};

struct WebTimerState : huxerui::example::TimerService, std::enable_shared_from_this<WebTimerState> {
  explicit WebTimerState(huxerui::PlatformAdapter& adapter_value) : adapter(&adapter_value) {}

  ~WebTimerState() override {
    pending_start.reset();
    InvalidateTimer();
  }

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
    if (pending && pending->completion) {
      Complete(std::move(pending->completion), std::move(error));
    }
  }

  huxerui::PlatformRequestId Start(std::chrono::milliseconds interval, std::function<void(std::uint64_t)> handler,
                                   std::function<void(huxerui::PlatformResult<std::uint64_t>)> completion) override {
    if (interval <= std::chrono::milliseconds::zero() || interval.count() > std::numeric_limits<std::int32_t>::max()) {
      throw std::invalid_argument("HuxerUI example timer interval is outside the browser timer range");
    }
    if (!handler || !completion) {
      throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
    }

    StopWithError(TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
    tick = 0;
    const std::uint64_t timer_generation = generation;
    tick_handler = std::move(handler);
    pending_start = PendingStart{timer_generation, std::move(completion)};
    timer = emscripten_set_interval(TimerCallback, static_cast<double>(interval.count()), this);
    if (timer == 0) {
      StopWithError(TimerError("example/timer-failed", "The browser timer could not be created"));
      return 0;
    }
    return timer_generation;
  }

  huxerui::PlatformRequestId Stop(std::function<void(huxerui::PlatformResult<std::monostate>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example timer completion must not be empty");
    }
    StopWithError(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
    tick_handler = {};
    Complete(std::move(completion), std::monostate{});
    return generation;
  }

private:
  static void TimerCallback(void* context) noexcept {
    try {
      static_cast<WebTimerState*>(context)->Tick();
    } catch (...) {
    }
  }

  bool Cancel(huxerui::PlatformRequestId timer_generation) override {
    if (generation != timer_generation) {
      return false;
    }
    pending_start.reset();
    tick_handler = {};
    InvalidateTimer();
    return true;
  }

  void Tick() {
    if (timer == 0) {
      return;
    }
    ++tick;
    if (pending_start && pending_start->generation == generation) {
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

  template <class Result, class Value>
  void Complete(std::function<void(huxerui::PlatformResult<Result>)> completion, Value&& value) {
    huxerui::PlatformResult<Result> result(std::forward<Value>(value));
    adapter->DispatchToUIThread(
        [completion = std::move(completion), result = std::move(result)]() mutable { completion(std::move(result)); });
  }

  huxerui::PlatformAdapter* adapter;
  std::function<void(std::uint64_t)> tick_handler;
  std::optional<PendingStart> pending_start;
  int timer = 0;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
};

} // namespace

namespace huxerui::example {

void InstallTimer(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<TimerService>>(timer::type, [](PlatformAdapter& adapter) {
    return std::static_pointer_cast<TimerService>(std::make_shared<WebTimerState>(adapter));
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<TimerService>>(timer::type));
}

} // namespace huxerui::example
