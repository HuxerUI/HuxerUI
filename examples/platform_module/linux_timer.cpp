#include "timer.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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

struct LinuxTimerState : std::enable_shared_from_this<LinuxTimerState> {
  static std::shared_ptr<LinuxTimerState> Create(huxerui::PlatformEventSink events) {
    auto state = std::shared_ptr<LinuxTimerState>(new LinuxTimerState(std::move(events)));
    const std::weak_ptr<LinuxTimerState> weak_state = state;
    state->worker = std::thread([weak_state] {
      if (const std::shared_ptr<LinuxTimerState> strong_state = weak_state.lock()) {
        strong_state->Run();
      }
    });
    return state;
  }

  ~LinuxTimerState() {
    Dispose();
  }

  std::function<void()> Start(std::int64_t milliseconds, huxerui::PlatformResultSink result) {
    if (milliseconds <= 0) {
      result(TimerError("example/invalid-interval", "The timer interval must be greater than zero"));
      return {};
    }

    huxerui::PlatformResultSink replaced_result;
    huxerui::PlatformResultSink failed_result;
    std::uint64_t timer_generation = 0;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        result(TimerError("example/timer-closed", "The native timer is closed"));
        return {};
      }
      if (worker_failed) {
        result(TimerError("example/timer-linux", "The Linux timer worker is unavailable"));
        return {};
      }
      if (generation == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result(TimerError("example/timer-exhausted", "The native timer generation space is exhausted"));
        return {};
      }
      if (pending_start) {
        replaced_result = std::move(pending_start->result);
        pending_start.reset();
      }
      timer_generation = ++generation;
      tick = 0;
      pending_start = PendingStart{timer_generation, std::move(result)};
      if (!ArmLocked(milliseconds)) {
        failed_result = std::move(pending_start->result);
        pending_start.reset();
        ++generation;
      }
    }

    if (replaced_result) {
      replaced_result(TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
    }
    if (failed_result) {
      failed_result(TimerError("example/timer-linux", "The Linux timer could not be started"));
      return {};
    }

    const std::weak_ptr<LinuxTimerState> weak_state = weak_from_this();
    return [weak_state, timer_generation] {
      if (const std::shared_ptr<LinuxTimerState> state = weak_state.lock()) {
        state->CancelStart(timer_generation);
      }
    };
  }

  void CancelStart(std::uint64_t timer_generation) {
    std::lock_guard lock(mutex);
    if (closed || generation != timer_generation) {
      return;
    }
    pending_start.reset();
    ++generation;
    DisarmLocked();
  }

  void Stop(huxerui::PlatformResultSink result) {
    huxerui::PlatformResultSink pending_result;
    bool stopped = false;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        result(TimerError("example/timer-closed", "The native timer is closed"));
        return;
      }
      if (pending_start) {
        pending_result = std::move(pending_start->result);
        pending_start.reset();
      }
      ++generation;
      stopped = DisarmLocked();
    }

    if (pending_result) {
      pending_result(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
    }
    if (!stopped) {
      result(TimerError("example/timer-linux", "The Linux timer could not be stopped"));
      return;
    }
    result(huxerui::PlatformPayload());
  }

  void Dispose() noexcept {
    {
      std::lock_guard lock(mutex);
      if (!closed) {
        closed = true;
        ++generation;
        pending_start.reset();
        events = {};
        static_cast<void>(DisarmLocked());
      }
    }

    SignalStop();
    if (worker.joinable()) {
      if (worker.get_id() == std::this_thread::get_id()) {
        worker.detach();
      } else {
        worker.join();
      }
    }
    if (timer_fd >= 0) {
      close(timer_fd);
      timer_fd = -1;
    }
    if (stop_fd >= 0) {
      close(stop_fd);
      stop_fd = -1;
    }
  }

private:
  explicit LinuxTimerState(huxerui::PlatformEventSink event_sink) : events(std::move(event_sink)) {
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) {
      throw std::runtime_error("HuxerUI example could not create the Linux native timer");
    }
    stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_fd < 0) {
      close(timer_fd);
      timer_fd = -1;
      throw std::runtime_error("HuxerUI example could not create the Linux timer stop handle");
    }
  }

  bool ArmLocked(std::int64_t milliseconds) {
    static_cast<void>(DisarmLocked());
    itimerspec timer{};
    timer.it_value.tv_sec = static_cast<time_t>(milliseconds / 1000);
    timer.it_value.tv_nsec = static_cast<long>((milliseconds % 1000) * 1'000'000);
    timer.it_interval = timer.it_value;
    if (timerfd_settime(timer_fd, 0, &timer, nullptr) < 0) {
      armed = false;
      return false;
    }
    armed = true;
    return true;
  }

  bool DisarmLocked() {
    armed = false;
    const itimerspec timer{};
    const bool stopped = timerfd_settime(timer_fd, 0, &timer, nullptr) == 0;
    std::uint64_t expirations = 0;
    while (read(timer_fd, &expirations, sizeof(expirations)) < 0 && errno == EINTR) {
    }
    return stopped;
  }

  void SignalStop() noexcept {
    if (stop_fd < 0) {
      return;
    }
    constexpr std::uint64_t wake = 1;
    while (write(stop_fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {
    }
  }

  void Run() noexcept {
    while (true) {
      pollfd descriptors[2] = {
          {.fd = timer_fd, .events = POLLIN, .revents = 0},
          {.fd = stop_fd, .events = POLLIN, .revents = 0},
      };
      const int poll_result = poll(descriptors, 2, -1);
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        FailWorker();
        return;
      }
      if ((descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        return;
      }
      if ((descriptors[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0 && !ReadTick()) {
        return;
      }
    }
  }

  bool ReadTick() noexcept {
    huxerui::PlatformResultSink first_result;
    huxerui::PlatformResultSink failed_result;
    huxerui::PlatformEventSink event_sink;
    std::uint64_t next_tick = 0;
    bool read_failed = false;
    {
      std::lock_guard lock(mutex);
      std::uint64_t expirations = 0;
      while (read(timer_fd, &expirations, sizeof(expirations)) < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN) {
          return true;
        }
        failed_result = FailWorkerLocked();
        read_failed = true;
        break;
      }
      if (!read_failed) {
        if (closed || !armed || expirations == 0) {
          return true;
        }
        next_tick = ++tick;
        if (pending_start && pending_start->generation == generation) {
          first_result = std::move(pending_start->result);
          pending_start.reset();
        }
        event_sink = events;
      }
    }

    try {
      if (failed_result) {
        failed_result(TimerError("example/timer-linux", "The Linux timer worker failed"));
      }
      if (read_failed) {
        return false;
      }
      if (first_result) {
        first_result(huxerui::PlatformPayload(next_tick));
      }
      if (event_sink) {
        event_sink(huxerui::example::timer::tick_event, huxerui::PlatformPayload(next_tick));
      }
    } catch (...) {
    }
    return true;
  }

  void FailWorker() noexcept {
    huxerui::PlatformResultSink failed_result;
    {
      std::lock_guard lock(mutex);
      failed_result = FailWorkerLocked();
    }
    if (failed_result) {
      try {
        failed_result(TimerError("example/timer-linux", "The Linux timer worker failed"));
      } catch (...) {
      }
    }
  }

  huxerui::PlatformResultSink FailWorkerLocked() noexcept {
    worker_failed = true;
    armed = false;
    ++generation;
    huxerui::PlatformResultSink failed_result;
    if (pending_start) {
      failed_result = std::move(pending_start->result);
      pending_start.reset();
    }
    return failed_result;
  }

  int timer_fd = -1;
  int stop_fd = -1;
  std::thread worker;
  std::mutex mutex;
  huxerui::PlatformEventSink events;
  std::optional<PendingStart> pending_start;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
  bool armed = false;
  bool closed = false;
  bool worker_failed = false;
};

huxerui::PlatformModuleFactory LinuxTimerFactory() {
  huxerui::PlatformModuleFactory factory;
  factory.create = [](const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events) {
    static_cast<void>(options);
    const std::shared_ptr<LinuxTimerState> state = LinuxTimerState::Create(std::move(events));
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
  root.Modules().Register(timer::type, LinuxTimerFactory());
  root.Provide(std::make_shared<TimerService>(root.Modules().Open(timer::type)));
}

} // namespace huxerui::example
