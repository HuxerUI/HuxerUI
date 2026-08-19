#include <huxerui/task.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include "internal.h"
#include "task_internal.h"

namespace huxerui::detail {

class TaskExecution final : public std::enable_shared_from_this<TaskExecution> {
public:
  TaskExecution(
      std::weak_ptr<TaskScopeState> scope,
      std::uint64_t identity,
      UIThreadDispatcher dispatcher,
      std::thread::id ui_thread,
      std::coroutine_handle<Task<void>::promise_type> coroutine
  )
      : scope_(std::move(scope)), identity_(identity), dispatcher_(std::move(dispatcher)), ui_thread_(ui_thread),
        coroutine_(coroutine) {}

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
  std::thread::id ui_thread_;
  std::coroutine_handle<Task<void>::promise_type> coroutine_;
  std::atomic<bool> cancellation_requested_ = false;
  bool running_ = false;
};

class TaskScopeState final : public std::enable_shared_from_this<TaskScopeState> {
public:
  explicit TaskScopeState(UIThreadDispatcher dispatcher)
      : dispatcher_(std::move(dispatcher)), ui_thread_(std::this_thread::get_id()) {
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
      execution = std::make_shared<TaskExecution>(shared_from_this(), identity, dispatcher_, ui_thread_, coroutine);
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
  std::thread::id ui_thread_;
  bool closed_ = false;
  std::uint64_t next_identity_ = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<TaskExecution>> tasks_;
};

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

std::shared_ptr<TaskScopeState> MakeTaskScopeState(UIThreadDispatcher dispatcher) {
  return std::make_shared<TaskScopeState>(std::move(dispatcher));
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

} // namespace huxerui
