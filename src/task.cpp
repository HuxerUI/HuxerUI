#include <huxerui/task.h>

#include <atomic>
#include <cmath>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "internal.h"
#include "task_internal.h"

namespace huxerui::detail {

class TaskDelayScheduler final : public std::enable_shared_from_this<TaskDelayScheduler> {
public:
  explicit TaskDelayScheduler(PlatformAdapter& platform)
      : platform_(&platform), ui_thread_(std::this_thread::get_id()) {}

  std::function<void()> Schedule(double duration_seconds, std::function<void()> callback) {
    if (std::this_thread::get_id() != ui_thread_) {
      throw std::logic_error("HuxerUI Delay() must be awaited on its UI thread");
    }
    const double deadline = platform_->Now() + duration_seconds;
    auto entry = std::make_shared<Entry>();
    entry->callback = std::move(callback);
    const std::shared_ptr<TaskDelayScheduler> scheduler = shared_from_this();
    std::function<void()> cancellation = [scheduler, entry] { scheduler->Cancel(entry); };
    const bool request_wakeup = entries_.empty() || deadline < entries_.begin()->first;
    entry->position = entries_.emplace(deadline, entry);
    if (request_wakeup) {
      try {
        platform_->RequestFrameAt(deadline);
      } catch (...) {
        entries_.erase(*entry->position);
        entry->position.reset();
        throw;
      }
    }

    return cancellation;
  }

  void Advance(double timestamp) {
    if (std::this_thread::get_id() != ui_thread_) {
      throw std::logic_error("HuxerUI task delays must advance on their UI thread");
    }

    std::vector<std::function<void()>> callbacks;
    const auto due_end = entries_.upper_bound(timestamp);
    callbacks.reserve(static_cast<std::size_t>(std::distance(entries_.begin(), due_end)));
    for (auto current = entries_.begin(); current != due_end; ++current) {
      const std::shared_ptr<Entry>& entry = current->second;
      entry->position.reset();
      callbacks.push_back(std::move(entry->callback));
    }
    entries_.erase(entries_.begin(), due_end);

    for (auto& callback : callbacks) {
      callback();
    }
    if (!entries_.empty()) {
      platform_->RequestFrameAt(entries_.begin()->first);
    }
  }

private:
  struct Entry;
  using Entries = std::multimap<double, std::shared_ptr<Entry>>;

  struct Entry {
    std::optional<Entries::iterator> position;
    std::function<void()> callback;
  };

  void Cancel(const std::shared_ptr<Entry>& entry) noexcept {
    if (!entry->position.has_value()) {
      return;
    }
    entries_.erase(*entry->position);
    entry->position.reset();
    entry->callback = {};
  }

  PlatformAdapter* platform_;
  std::thread::id ui_thread_;
  Entries entries_;
};

class TaskExecution final : public std::enable_shared_from_this<TaskExecution> {
public:
  TaskExecution(
      std::weak_ptr<TaskScopeState> scope,
      std::uint64_t identity,
      UIThreadDispatcher dispatcher,
      std::shared_ptr<TaskDelayScheduler> delay_scheduler,
      std::thread::id ui_thread,
      std::coroutine_handle<Task<void>::promise_type> coroutine
  )
      : scope_(std::move(scope)), identity_(identity), dispatcher_(std::move(dispatcher)),
        delay_scheduler_(std::move(delay_scheduler)), ui_thread_(ui_thread), coroutine_(coroutine) {}

  ~TaskExecution() {
    if (coroutine_) {
      coroutine_.destroy();
    }
  }

  void Start();
  void Cancel() noexcept;
  void CloseOnUI() noexcept;
  void QueueCompletion() noexcept;
  void QueueResume(std::coroutine_handle<> coroutine) noexcept;
  std::function<void()> ScheduleDelay(double duration_seconds, std::coroutine_handle<> coroutine);

private:
  [[nodiscard]] bool IsUIThread() const noexcept {
    return std::this_thread::get_id() == ui_thread_;
  }

  void PostNoexcept(std::function<void()> callback) noexcept;
  void ResumeOnUI(std::coroutine_handle<> coroutine) noexcept;
  void CancelOnUI() noexcept;
  void CompleteOnUI() noexcept;
  void DetachFromScope() noexcept;

  std::weak_ptr<TaskScopeState> scope_;
  std::uint64_t identity_;
  UIThreadDispatcher dispatcher_;
  std::shared_ptr<TaskDelayScheduler> delay_scheduler_;
  std::thread::id ui_thread_;
  std::coroutine_handle<Task<void>::promise_type> coroutine_;
  std::atomic<bool> cancellation_requested_ = false;
  bool running_ = false;
};

class TaskScopeState final : public std::enable_shared_from_this<TaskScopeState> {
public:
  TaskScopeState(UIThreadDispatcher dispatcher, std::shared_ptr<TaskDelayScheduler> delay_scheduler)
      : dispatcher_(std::move(dispatcher)), delay_scheduler_(std::move(delay_scheduler)),
        ui_thread_(std::this_thread::get_id()) {
    if (!dispatcher_) {
      throw std::logic_error("HuxerUI UseTaskScope() requires a UIThreadDispatcher");
    }
  }

  TaskHandle Launch(std::coroutine_handle<Task<void>::promise_type> coroutine) {
    if (std::this_thread::get_id() != ui_thread_) {
      if (coroutine) {
        coroutine.destroy();
      }
      throw std::logic_error("HuxerUI TaskScope::Launch() must be called on its UI thread");
    }
    if (closed_) {
      if (coroutine) {
        coroutine.destroy();
      }
      throw std::logic_error("HuxerUI TaskScope is closed");
    }
    if (!coroutine) {
      throw std::logic_error("HuxerUI TaskScope::Launch() requires a valid Task");
    }

    const std::uint64_t identity = next_identity_++;
    std::shared_ptr<TaskExecution> execution;
    try {
      execution = std::make_shared<TaskExecution>(
          shared_from_this(),
          identity,
          dispatcher_,
          delay_scheduler_,
          ui_thread_,
          coroutine
      );
    } catch (...) {
      coroutine.destroy();
      throw;
    }
    coroutine.promise().BindExecution(execution);
    tasks_.emplace(identity, execution);
    try {
      execution->Start();
    } catch (...) {
      tasks_.erase(identity);
      throw;
    }
    return TaskHandle(execution);
  }

  void Detach(std::uint64_t identity) noexcept {
    tasks_.erase(identity);
  }

  void Close() noexcept {
    if (closed_) {
      return;
    }
    closed_ = true;
    auto tasks = std::move(tasks_);
    for (auto& [identity, execution] : tasks) {
      static_cast<void>(identity);
      execution->CloseOnUI();
    }
  }

private:
  UIThreadDispatcher dispatcher_;
  std::shared_ptr<TaskDelayScheduler> delay_scheduler_;
  std::thread::id ui_thread_;
  bool closed_ = false;
  std::uint64_t next_identity_ = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<TaskExecution>> tasks_;
};

class DelayAwaiter final {
public:
  explicit DelayAwaiter(double duration_seconds) noexcept : duration_seconds_(duration_seconds) {}

  DelayAwaiter(const DelayAwaiter&) = delete;
  DelayAwaiter& operator=(const DelayAwaiter&) = delete;
  DelayAwaiter(DelayAwaiter&& other) noexcept
      : duration_seconds_(other.duration_seconds_), cancel_(std::exchange(other.cancel_, {})) {}
  DelayAwaiter& operator=(DelayAwaiter&&) = delete;

  ~DelayAwaiter() {
    if (cancel_) {
      cancel_();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> continuation) {
    const std::shared_ptr<TaskExecution> execution = TaskExecutionFor(continuation).lock();
    if (!execution) {
      throw std::logic_error("HuxerUI Delay() requires a running Task execution");
    }
    cancel_ = execution->ScheduleDelay(duration_seconds_, continuation);
  }

  void await_resume() const noexcept {}

private:
  double duration_seconds_;
  std::function<void()> cancel_;
};

Task<void> DelayTask(double duration_seconds) {
  co_await DelayAwaiter(duration_seconds);
}

void TaskExecution::PostNoexcept(std::function<void()> callback) noexcept {
  try {
    dispatcher_(std::move(callback));
  } catch (...) {
    std::terminate();
  }
}

void TaskExecution::Start() {
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  dispatcher_([weak] {
    if (auto execution = weak.lock()) {
      execution->ResumeOnUI(execution->coroutine_);
    }
  });
}

void TaskExecution::Cancel() noexcept {
  if (cancellation_requested_.exchange(true)) {
    return;
  }
  if (IsUIThread() && !running_) {
    CancelOnUI();
    return;
  }
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  PostNoexcept([weak] {
    if (auto execution = weak.lock()) {
      execution->CancelOnUI();
    }
  });
}

void TaskExecution::CloseOnUI() noexcept {
  cancellation_requested_ = true;
  CancelOnUI();
}

void TaskExecution::QueueCompletion() noexcept {
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  PostNoexcept([weak] {
    if (auto execution = weak.lock()) {
      execution->CompleteOnUI();
    }
  });
}

void TaskExecution::QueueResume(std::coroutine_handle<> coroutine) noexcept {
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  PostNoexcept([weak, coroutine] {
    if (auto execution = weak.lock()) {
      execution->ResumeOnUI(coroutine);
    }
  });
}

std::function<void()> TaskExecution::ScheduleDelay(double duration_seconds, std::coroutine_handle<> coroutine) {
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  return delay_scheduler_->Schedule(duration_seconds, [weak, coroutine] {
    if (auto execution = weak.lock()) {
      execution->ResumeOnUI(coroutine);
    }
  });
}

void TaskExecution::ResumeOnUI(std::coroutine_handle<> coroutine) noexcept {
  const auto self = shared_from_this();
  if (cancellation_requested_) {
    CancelOnUI();
    return;
  }
  if (!coroutine || coroutine.done()) {
    return;
  }
  running_ = true;
  coroutine.resume();
  running_ = false;
  if (cancellation_requested_ && coroutine_) {
    CancelOnUI();
  }
}

void TaskExecution::CancelOnUI() noexcept {
  if (running_ || !coroutine_) {
    return;
  }
  const auto self = shared_from_this();
  DetachFromScope();
  auto coroutine = std::exchange(coroutine_, {});
  coroutine.destroy();
}

void TaskExecution::CompleteOnUI() noexcept {
  if (!coroutine_ || !coroutine_.done()) {
    return;
  }
  const auto self = shared_from_this();
  std::exception_ptr exception;
  try {
    coroutine_.promise().RethrowException();
  } catch (...) {
    exception = std::current_exception();
  }
  DetachFromScope();
  auto coroutine = std::exchange(coroutine_, {});
  coroutine.destroy();
  if (exception) {
    std::terminate();
  }
}

void TaskExecution::DetachFromScope() noexcept {
  if (auto scope = scope_.lock()) {
    scope->Detach(identity_);
  }
  scope_.reset();
}

std::shared_ptr<TaskDelayScheduler> MakeTaskDelayScheduler(PlatformAdapter& platform) {
  return std::make_shared<TaskDelayScheduler>(platform);
}

void AdvanceTaskDelays(const std::shared_ptr<TaskDelayScheduler>& scheduler, double timestamp) {
  scheduler->Advance(timestamp);
}

std::shared_ptr<TaskScopeState>
MakeTaskScopeState(UIThreadDispatcher dispatcher, std::shared_ptr<TaskDelayScheduler> delay_scheduler) {
  return std::make_shared<TaskScopeState>(std::move(dispatcher), std::move(delay_scheduler));
}

void CloseTaskScope(const std::shared_ptr<TaskScopeState>& scope) noexcept {
  if (scope) {
    scope->Close();
  }
}

void NotifyTaskCompleted(const std::weak_ptr<TaskExecution>& execution) noexcept {
  if (auto active = execution.lock()) {
    active->QueueCompletion();
  }
}

void ResumeTask(const std::weak_ptr<TaskExecution>& execution, std::coroutine_handle<> coroutine) noexcept {
  if (auto active = execution.lock()) {
    active->QueueResume(coroutine);
  }
}

} // namespace huxerui::detail

namespace huxerui {

void TaskHandle::Cancel() const noexcept {
  if (auto execution = execution_.lock()) {
    execution->Cancel();
  }
}

TaskHandle TaskScope::Launch(Task<void>&& task) const {
  if (detail::Composer::Current() != nullptr) {
    throw std::logic_error("HuxerUI TaskScope::Launch() cannot run during view composition");
  }
  if (!state_) {
    throw std::logic_error("HuxerUI TaskScope is empty");
  }
  return state_->Launch(task.Release());
}

TaskScope UseTaskScope() {
  detail::Composer* composer = detail::Composer::Current();
  if (composer == nullptr) {
    throw std::logic_error("UseTaskScope() must be called while HuxerUI is composing a view");
  }
  return composer->Tasks();
}

Task<void> Delay(std::chrono::duration<double> duration) {
  if (!std::isfinite(duration.count()) || duration.count() < 0.0) {
    throw std::invalid_argument("HuxerUI Delay duration must be finite and non-negative");
  }
  return detail::DelayTask(duration.count());
}

} // namespace huxerui
