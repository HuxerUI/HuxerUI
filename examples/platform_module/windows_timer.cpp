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

struct WindowsTimerState : huxerui::example::TimerService, std::enable_shared_from_this<WindowsTimerState> {
  static std::shared_ptr<WindowsTimerState> Create(huxerui::PlatformAdapter& adapter) {
    auto state = std::shared_ptr<WindowsTimerState>(new WindowsTimerState(adapter));
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

  huxerui::PlatformRequestId Start(std::chrono::milliseconds interval, std::function<void(std::uint64_t)> handler,
                                   std::function<void(huxerui::PlatformResult<std::uint64_t>)> completion) override {
    if (interval <= std::chrono::milliseconds::zero() ||
        interval.count() > static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())) {
      throw std::invalid_argument("HuxerUI example timer interval is outside the Windows timer range");
    }
    if (!handler || !completion) {
      throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
    }
    const std::int64_t milliseconds = interval.count();

    std::lock_guard operation_lock(operation_mutex);
    {
      std::lock_guard lock(mutex);
      if (closed) {
        Complete(std::move(completion), TimerError("example/timer-closed", "The platform timer is closed"));
        return 0;
      }
      if (generation == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        Complete(std::move(completion), TimerError("example/timer-exhausted", "The timer generation is exhausted"));
        return 0;
      }
    }

    DisarmAndWait();

    std::function<void(huxerui::PlatformResult<std::uint64_t>)> replaced_completion;
    std::uint64_t timer_generation = 0;
    {
      std::lock_guard lock(mutex);
      if (pending_start) {
        replaced_completion = std::move(pending_start->completion);
        pending_start.reset();
      }
      timer_generation = ++generation;
      tick = 0;
      armed = true;
      tick_handler = std::move(handler);
      pending_start = PendingStart{timer_generation, std::move(completion)};
    }

    FILETIME due_time = RelativeDueTime(milliseconds);
    SetThreadpoolTimer(timer, &due_time, static_cast<DWORD>(milliseconds), 0);
    if (replaced_completion) {
      Complete(std::move(replaced_completion),
               TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
    }
    return timer_generation;
  }

  huxerui::PlatformRequestId Stop(std::function<void(huxerui::PlatformResult<std::monostate>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example timer completion must not be empty");
    }
    std::lock_guard operation_lock(operation_mutex);
    {
      std::lock_guard lock(mutex);
      if (closed) {
        Complete(std::move(completion), TimerError("example/timer-closed", "The platform timer is closed"));
        return 0;
      }
    }

    DisarmAndWait();

    std::function<void(huxerui::PlatformResult<std::uint64_t>)> pending_completion;
    std::uint64_t request = 0;
    {
      std::lock_guard lock(mutex);
      armed = false;
      request = ++generation;
      tick_handler = {};
      if (pending_start) {
        pending_completion = std::move(pending_start->completion);
        pending_start.reset();
      }
    }
    if (pending_completion) {
      Complete(std::move(pending_completion),
               TimerError("example/timer-stopped", "The timer stopped before its first tick"));
    }
    Complete(std::move(completion), std::monostate{});
    return request;
  }

  bool Cancel(huxerui::PlatformRequestId request) override {
    return CancelStart(request);
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
      tick_handler = {};
    }
    if (timer != nullptr) {
      DisarmAndWait();
      CloseThreadpoolTimer(timer);
      timer = nullptr;
    }
  }

private:
  explicit WindowsTimerState(huxerui::PlatformAdapter& adapter_value) : adapter(&adapter_value) {}

  template <class Result, class Value>
  void Complete(std::function<void(huxerui::PlatformResult<Result>)> completion, Value&& value) {
    huxerui::PlatformResult<Result> result(std::forward<Value>(value));
    adapter->DispatchToUIThread(
        [completion = std::move(completion), result = std::move(result)]() mutable { completion(std::move(result)); });
  }

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

  bool CancelStart(std::uint64_t timer_generation) {
    std::lock_guard operation_lock(operation_mutex);
    {
      std::lock_guard lock(mutex);
      if (closed || generation != timer_generation) {
        return false;
      }
    }

    DisarmAndWait();

    std::lock_guard lock(mutex);
    if (closed || generation != timer_generation) {
      return false;
    }
    armed = false;
    ++generation;
    pending_start.reset();
    tick_handler = {};
    return true;
  }

  void DisarmAndWait() noexcept {
    // Control operations use a separate mutex because waiting while holding callback state would deadlock Tick().
    SetThreadpoolTimer(timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
  }

  void Tick() noexcept {
    // Periodic thread-pool callbacks may overlap; serialize delivery so ticks, results, and events stay ordered.
    std::lock_guard callback_lock(callback_mutex);
    std::function<void(huxerui::PlatformResult<std::uint64_t>)> first_completion;
    std::function<void(std::uint64_t)> handler;
    std::uint64_t next_tick = 0;
    {
      std::lock_guard lock(mutex);
      if (closed || !armed) {
        return;
      }
      next_tick = ++tick;
      if (pending_start && pending_start->generation == generation) {
        first_completion = std::move(pending_start->completion);
        pending_start.reset();
      }
      handler = tick_handler;
    }
    try {
      adapter->DispatchToUIThread(
          [completion = std::move(first_completion), handler = std::move(handler), next_tick]() mutable {
            if (completion) {
              completion(next_tick);
            }
            if (handler) {
              handler(next_tick);
            }
          });
    } catch (...) {
    }
  }

  PTP_TIMER timer = nullptr;
  huxerui::PlatformAdapter* adapter = nullptr;
  std::mutex operation_mutex;
  std::mutex callback_mutex;
  std::mutex mutex;
  std::function<void(std::uint64_t)> tick_handler;
  std::optional<PendingStart> pending_start;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
  bool armed = false;
  bool closed = false;
};

} // namespace

namespace huxerui::example {

void InstallTimer(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<TimerService>>(timer::type, [](PlatformAdapter& adapter) {
    return std::static_pointer_cast<TimerService>(WindowsTimerState::Create(adapter));
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<TimerService>>(timer::type));
}

} // namespace huxerui::example
