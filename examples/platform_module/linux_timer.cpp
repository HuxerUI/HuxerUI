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

struct LinuxTimerState : huxerui::example::TimerService {
  static std::shared_ptr<LinuxTimerState> Create(huxerui::PlatformAdapter& adapter) {
    auto state = std::shared_ptr<LinuxTimerState>(new LinuxTimerState(adapter));
    LinuxTimerState* state_pointer = state.get();
    state->worker = std::thread([state_pointer] { state_pointer->Run(); });
    return state;
  }

  ~LinuxTimerState() {
    Dispose();
  }

  huxerui::PlatformRequestId Start(std::chrono::milliseconds interval, std::function<void(std::uint64_t)> handler,
                                   std::function<void(huxerui::PlatformResult<std::uint64_t>)> completion) override {
    if (interval <= std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("HuxerUI example timer interval must be greater than zero");
    }
    if (!handler || !completion) {
      throw std::invalid_argument("HuxerUI example timer callbacks must not be empty");
    }
    const std::int64_t milliseconds = interval.count();

    std::function<void(huxerui::PlatformResult<std::uint64_t>)> replaced_completion;
    std::function<void(huxerui::PlatformResult<std::uint64_t>)> failed_completion;
    std::uint64_t timer_generation = 0;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        Complete(std::move(completion), TimerError("example/timer-closed", "The platform timer is closed"));
        return 0;
      }
      if (worker_failed) {
        Complete(std::move(completion), TimerError("example/timer-linux", "The Linux timer worker is unavailable"));
        return 0;
      }
      if (generation == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        Complete(std::move(completion), TimerError("example/timer-exhausted", "The timer generation is exhausted"));
        return 0;
      }
      if (pending_start) {
        replaced_completion = std::move(pending_start->completion);
        pending_start.reset();
      }
      timer_generation = ++generation;
      tick = 0;
      tick_handler = std::move(handler);
      pending_start = PendingStart{timer_generation, std::move(completion)};
      if (!ArmLocked(milliseconds)) {
        failed_completion = std::move(pending_start->completion);
        pending_start.reset();
        ++generation;
      }
    }

    if (replaced_completion) {
      Complete(std::move(replaced_completion), TimerError("example/timer-replaced", "The timer was replaced"));
    }
    if (failed_completion) {
      Complete(std::move(failed_completion), TimerError("example/timer-linux", "The Linux timer could not start"));
      return 0;
    }
    return timer_generation;
  }

  bool Cancel(huxerui::PlatformRequestId timer_generation) override {
    std::lock_guard lock(mutex);
    if (closed || generation != timer_generation) {
      return false;
    }
    pending_start.reset();
    tick_handler = {};
    ++generation;
    DisarmLocked();
    return true;
  }

  huxerui::PlatformRequestId Stop(std::function<void(huxerui::PlatformResult<std::monostate>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example timer completion must not be empty");
    }
    std::function<void(huxerui::PlatformResult<std::uint64_t>)> pending_completion;
    bool stopped = false;
    std::uint64_t request = 0;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        Complete(std::move(completion), TimerError("example/timer-closed", "The platform timer is closed"));
        return 0;
      }
      if (pending_start) {
        pending_completion = std::move(pending_start->completion);
        pending_start.reset();
      }
      request = ++generation;
      tick_handler = {};
      stopped = DisarmLocked();
    }

    if (pending_completion) {
      Complete(std::move(pending_completion), TimerError("example/timer-stopped", "The timer stopped"));
    }
    if (!stopped) {
      Complete(std::move(completion), TimerError("example/timer-linux", "The Linux timer could not stop"));
      return request;
    }
    Complete(std::move(completion), std::monostate{});
    return request;
  }

  void Dispose() noexcept {
    {
      std::lock_guard lock(mutex);
      if (!closed) {
        closed = true;
        ++generation;
        pending_start.reset();
        tick_handler = {};
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
  explicit LinuxTimerState(huxerui::PlatformAdapter& adapter_value) : adapter(&adapter_value) {
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) {
      throw std::runtime_error("HuxerUI example could not create the Linux platform timer");
    }
    stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_fd < 0) {
      close(timer_fd);
      timer_fd = -1;
      throw std::runtime_error("HuxerUI example could not create the Linux timer stop handle");
    }
  }

  template <class Result, class Value>
  void Complete(std::function<void(huxerui::PlatformResult<Result>)> completion, Value&& value) {
    huxerui::PlatformResult<Result> result(std::forward<Value>(value));
    adapter->DispatchToUIThread(
        [completion = std::move(completion), result = std::move(result)]() mutable { completion(std::move(result)); });
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
    std::function<void(huxerui::PlatformResult<std::uint64_t>)> first_completion;
    std::function<void(huxerui::PlatformResult<std::uint64_t>)> failed_completion;
    std::function<void(std::uint64_t)> handler;
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
        failed_completion = FailWorkerLocked();
        read_failed = true;
        break;
      }
      if (!read_failed) {
        if (closed || !armed || expirations == 0) {
          return true;
        }
        next_tick = ++tick;
        if (pending_start && pending_start->generation == generation) {
          first_completion = std::move(pending_start->completion);
          pending_start.reset();
        }
        handler = tick_handler;
      }
    }

    try {
      if (failed_completion) {
        Complete(std::move(failed_completion), TimerError("example/timer-linux", "The Linux timer worker failed"));
      }
      if (read_failed) {
        return false;
      }
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
    return true;
  }

  void FailWorker() noexcept {
    std::function<void(huxerui::PlatformResult<std::uint64_t>)> failed_completion;
    {
      std::lock_guard lock(mutex);
      failed_completion = FailWorkerLocked();
    }
    if (failed_completion) {
      try {
        Complete(std::move(failed_completion), TimerError("example/timer-linux", "The Linux timer worker failed"));
      } catch (...) {
      }
    }
  }

  std::function<void(huxerui::PlatformResult<std::uint64_t>)> FailWorkerLocked() noexcept {
    worker_failed = true;
    armed = false;
    ++generation;
    std::function<void(huxerui::PlatformResult<std::uint64_t>)> failed_completion;
    if (pending_start) {
      failed_completion = std::move(pending_start->completion);
      pending_start.reset();
    }
    return failed_completion;
  }

  int timer_fd = -1;
  int stop_fd = -1;
  huxerui::PlatformAdapter* adapter = nullptr;
  std::thread worker;
  std::mutex mutex;
  std::function<void(std::uint64_t)> tick_handler;
  std::optional<PendingStart> pending_start;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
  bool armed = false;
  bool closed = false;
  bool worker_failed = false;
};

} // namespace

namespace huxerui::example {

void InstallTimer(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<TimerService>>(timer::type, [](PlatformAdapter& adapter) {
    return std::static_pointer_cast<TimerService>(LinuxTimerState::Create(adapter));
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<TimerService>>(timer::type));
}

} // namespace huxerui::example
