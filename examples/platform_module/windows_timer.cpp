#include "timer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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

struct WindowsTimerState : std::enable_shared_from_this<WindowsTimerState> {
  static std::shared_ptr<WindowsTimerState> Create(huxerui::PlatformEventSink events) {
    auto state = std::shared_ptr<WindowsTimerState>(new WindowsTimerState(std::move(events)));
    state->timer = CreateThreadpoolTimer(TimerCallback, state.get(), nullptr);
    if (state->timer == nullptr) {
      throw std::system_error(
          static_cast<int>(GetLastError()),
          std::system_category(),
          "HuxerUI example could not create the Windows timer"
      );
    }
    return state;
  }

  ~WindowsTimerState() {
    Dispose();
  }

  std::function<void()> Start(std::int64_t milliseconds, huxerui::PlatformResultSink result) {
    if (milliseconds <= 0 || milliseconds > static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())) {
      result(TimerError("example/invalid-interval", "The timer interval is outside the Windows timer range"));
      return {};
    }

    std::lock_guard operation_lock(operation_mutex);
    {
      std::lock_guard lock(mutex);
      if (closed) {
        result(TimerError("example/timer-closed", "The native timer is closed"));
        return {};
      }
      if (generation == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result(TimerError("example/timer-exhausted", "The native timer generation space is exhausted"));
        return {};
      }
    }

    DisarmAndWait();

    huxerui::PlatformResultSink replaced_result;
    std::uint64_t timer_generation = 0;
    {
      std::lock_guard lock(mutex);
      if (pending_start) {
        replaced_result = std::move(pending_start->result);
        pending_start.reset();
      }
      timer_generation = ++generation;
      tick = 0;
      armed = true;
      pending_start = PendingStart{timer_generation, std::move(result)};
    }

    FILETIME due_time = RelativeDueTime(milliseconds);
    SetThreadpoolTimer(timer, &due_time, static_cast<DWORD>(milliseconds), 0);
    if (replaced_result) {
      replaced_result(TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
    }

    const std::weak_ptr<WindowsTimerState> weak_state = weak_from_this();
    return [weak_state, timer_generation] {
      if (const std::shared_ptr<WindowsTimerState> state = weak_state.lock()) {
        state->CancelStart(timer_generation);
      }
    };
  }

  void Stop(huxerui::PlatformResultSink result) {
    std::lock_guard operation_lock(operation_mutex);
    {
      std::lock_guard lock(mutex);
      if (closed) {
        result(TimerError("example/timer-closed", "The native timer is closed"));
        return;
      }
    }

    DisarmAndWait();

    huxerui::PlatformResultSink pending_result;
    {
      std::lock_guard lock(mutex);
      armed = false;
      ++generation;
      if (pending_start) {
        pending_result = std::move(pending_start->result);
        pending_start.reset();
      }
    }
    if (pending_result) {
      pending_result(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
    }
    result(huxerui::PlatformPayload());
  }

  void Dispose() noexcept {
    std::lock_guard operation_lock(operation_mutex);
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      closed = true;
      armed = false;
      ++generation;
      pending_start.reset();
      events = {};
    }
    if (timer != nullptr) {
      DisarmAndWait();
      CloseThreadpoolTimer(timer);
      timer = nullptr;
    }
  }

private:
  explicit WindowsTimerState(huxerui::PlatformEventSink event_sink) : events(std::move(event_sink)) {}

  static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept {
    static_cast<WindowsTimerState*>(context)->Tick();
  }

  static FILETIME RelativeDueTime(std::int64_t milliseconds) noexcept {
    const auto relative = -static_cast<LONGLONG>(milliseconds) * 10'000LL;
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<ULONGLONG>(relative);
    return {
        value.LowPart,
        value.HighPart,
    };
  }

  void CancelStart(std::uint64_t timer_generation) {
    std::lock_guard operation_lock(operation_mutex);
    {
      std::lock_guard lock(mutex);
      if (closed || generation != timer_generation) {
        return;
      }
    }

    DisarmAndWait();

    std::lock_guard lock(mutex);
    if (closed || generation != timer_generation) {
      return;
    }
    armed = false;
    ++generation;
    pending_start.reset();
  }

  void DisarmAndWait() noexcept {
    // Control operations use a separate mutex because waiting while holding callback state would deadlock Tick().
    SetThreadpoolTimer(timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
  }

  void Tick() noexcept {
    // Periodic thread-pool callbacks may overlap; serialize delivery so ticks, results, and events stay ordered.
    std::lock_guard callback_lock(callback_mutex);
    huxerui::PlatformResultSink first_result;
    huxerui::PlatformEventSink event_sink;
    std::uint64_t next_tick = 0;
    {
      std::lock_guard lock(mutex);
      if (closed || !armed) {
        return;
      }
      next_tick = ++tick;
      if (pending_start && pending_start->generation == generation) {
        first_result = std::move(pending_start->result);
        pending_start.reset();
      }
      event_sink = events;
    }
    try {
      if (first_result) {
        first_result(huxerui::PlatformPayload(next_tick));
      }
      if (event_sink) {
        event_sink(huxerui::example::timer::tick_event, huxerui::PlatformPayload(next_tick));
      }
    } catch (...) {
    }
  }

  PTP_TIMER timer = nullptr;
  std::mutex operation_mutex;
  std::mutex callback_mutex;
  std::mutex mutex;
  huxerui::PlatformEventSink events;
  std::optional<PendingStart> pending_start;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
  bool armed = false;
  bool closed = false;
};

huxerui::PlatformModuleFactory WindowsTimerFactory() {
  huxerui::PlatformModuleFactory factory;
  factory.create = [](const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events) {
    static_cast<void>(options);
    const std::shared_ptr<WindowsTimerState> state = WindowsTimerState::Create(std::move(events));
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
      result(TimerError("example/unknown-method", "The native timer method is not supported"));
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
  root.Modules().Register(timer::type, WindowsTimerFactory());
  root.Provide(std::make_shared<TimerService>(root.Modules().Open(timer::type)));
}

} // namespace huxerui::example
