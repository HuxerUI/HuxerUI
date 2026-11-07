#include <huxerui/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui_window_internal.h"
#include "task_internal.h"
#include "application/application_internal.h"
#include "macros_internal.h"
#include "application/system_task_internal.h"

namespace huxerui::detail {

#if !defined(__EMSCRIPTEN__)

namespace {

std::size_t ResolveWorkerCount() noexcept {
  const unsigned int reported = std::thread::hardware_concurrency();
  if (reported == 0) {
    return 2;
  }
  const std::size_t available = reported > 1 ? static_cast<std::size_t>(reported - 1) : 1;
  return available < 4 ? available : 4;
}

class WorkerExecutor final {
public:
  WorkerExecutor() : worker_count_(ResolveWorkerCount()) {
    workers_.reserve(worker_count_);
    for (std::size_t index = 0; index < worker_count_; ++index) {
      workers_.emplace_back([this] { Run(); });
    }
  }

  ~WorkerExecutor() {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
      worker.join();
    }
  }

  WorkerExecutor(const WorkerExecutor&) = delete;
  WorkerExecutor& operator=(const WorkerExecutor&) = delete;

  void Submit(std::function<void()> operation) {
    {
      std::scoped_lock lock(mutex_);
      if (stopping_) {
        throw std::logic_error("HuxerUI worker executor is closed");
      }
      operations_.push_back(std::move(operation));
    }
    condition_.notify_one();
  }

  [[nodiscard]] std::size_t WorkerCount() const noexcept {
    return worker_count_;
  }

  static WorkerExecutor& Instance() {
    static WorkerExecutor executor;
    return executor;
  }

private:
  void Run() {
    for (;;) {
      std::function<void()> operation;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || !operations_.empty(); });
        if (stopping_ && operations_.empty()) {
          return;
        }
        operation = std::move(operations_.front());
        operations_.pop_front();
      }
      operation();
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::function<void()>> operations_;
  std::vector<std::thread> workers_;
  std::size_t worker_count_;
  bool stopping_ = false;
};

} // namespace

#endif

class WorkerSequenceState final {
public:
  std::mutex mutex;
  std::weak_ptr<WorkerOperation> active;
  std::deque<std::weak_ptr<WorkerOperation>> queued;
};

struct TaskWaitState {
  std::weak_ptr<TaskExecution> execution;
  std::coroutine_handle<> coroutine;
  bool active = true;
};

namespace {
thread_local SystemTaskStopState* current_system_task = nullptr;
}

class TaskExecution final : public std::enable_shared_from_this<TaskExecution> {
public:
  TaskExecution(
      std::weak_ptr<TaskScopeState> scope,
      std::uint64_t identity,
      UiThreadDispatcher dispatcher,
      std::thread::id ui_thread, std::shared_ptr<ExecutionContext> context,
      std::coroutine_handle<Task<void>::promise_type> coroutine, std::shared_ptr<SystemTaskStopState> stop
  )
      : scope_(std::move(scope)), identity_(identity), dispatcher_(std::move(dispatcher)),
        ui_thread_(ui_thread), context_(std::move(context)),
        coroutine_(coroutine), stop_(std::move(stop)) {}

  ~TaskExecution() {
    if (coroutine_) {
      coroutine_.destroy();
    }
  }

  void Start();
  void Cancel() noexcept;
  /// Closes this execution during owner retirement on the application thread, deferring destruction while running.
  void CloseOnUi() noexcept;
  void QueueCompletion() noexcept;
  void QueueResume(std::weak_ptr<TaskWaitState> wait) noexcept;
  std::shared_ptr<TaskWaitState> BeginWait(std::coroutine_handle<> coroutine) {
    auto wait = std::make_shared<TaskWaitState>(TaskWaitState{weak_from_this(), coroutine});
    wait_ = wait;
    return wait;
  }
  void CheckStop() const {
    if (stop_ && !stop_->cleaning_up && stop_->source.stop_requested()) {
      throw TaskStopped{};
    }
  }
  void WakeStop() noexcept {
    if (!stop_->cleaning_up && !wait_.expired()) {
      QueueResume(wait_);
    }
  }

private:
  /// Checks the application's owning thread, whether this scope is application-wide or composition-owned.
  /// @return True when immediate coroutine cancellation is permitted without dispatching to another thread.
  [[nodiscard]] bool IsUiThread() const noexcept {
    return std::this_thread::get_id() == ui_thread_;
  }

  void PostNoexcept(std::function<void()> callback) noexcept;
  /// Resumes a suspended coroutine on the owning thread under its original execution context.
  /// @param coroutine Awaiting frame still owned by this execution; ignored when null, complete, or canceled.
  void ResumeOnUi(std::coroutine_handle<> coroutine) noexcept;
  /// Destroys a suspended coroutine under its original context and removes it from the scope; never destroys a running
  /// frame.
  void CancelOnUi() noexcept;
  /// Retires a completed coroutine under its original context; an unhandled Task exception terminates the process.
  void CompleteOnUi() noexcept;
  void DetachFromScope() noexcept;

  std::weak_ptr<TaskScopeState> scope_;
  std::uint64_t identity_;
  UiThreadDispatcher dispatcher_;
  std::thread::id ui_thread_;
  std::shared_ptr<ExecutionContext> context_;
  std::coroutine_handle<Task<void>::promise_type> coroutine_;
  std::shared_ptr<SystemTaskStopState> stop_;
  std::weak_ptr<TaskWaitState> wait_;
  std::atomic<bool> cancellation_requested_ = false;
  bool running_ = false;
};

class TaskScopeState final : public std::enable_shared_from_this<TaskScopeState> {
public:
  TaskScopeState(UiThreadDispatcher dispatcher, std::shared_ptr<ExecutionContext> context)
      : dispatcher_(std::move(dispatcher)),
        ui_thread_(std::this_thread::get_id()), context_(std::move(context)), application_(context_->application) {
    if (!dispatcher_) {
      throw std::logic_error("HuxerUI UseTaskScope() requires a UiThreadDispatcher");
    }
  }

  TaskHandle Launch(std::coroutine_handle<Task<void>::promise_type> coroutine,
                    std::shared_ptr<SystemTaskStopState> stop = {}) {
    if (std::this_thread::get_id() != ui_thread_) {
      if (coroutine) {
        coroutine.destroy();
      }
      throw std::logic_error("HuxerUI TaskScope::Launch() must be called on its UI thread");
    }
    {
      std::unique_lock lock(mutex_);
      const auto application = application_.lock();
      if (closed_ || !application || !application->accepts_tasks) {
        lock.unlock();
        if (coroutine) {
          coroutine.destroy();
        }
        throw std::logic_error("HuxerUI TaskScope is closed");
      }
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
          ui_thread_, context_,
          coroutine, stop
      );
    } catch (...) {
      coroutine.destroy();
      throw;
    }
    coroutine.promise().BindExecution(execution);
    if (stop) {
      stop->execution = execution;
    }
    tasks_.emplace(identity, execution);
    try {
      execution->Start();
    } catch (...) {
      tasks_.erase(identity);
      throw;
    }
    return TaskHandle(execution);
  }

  TaskHandle Launch(Task<void> task, std::shared_ptr<SystemTaskStopState> stop) {
    return Launch(task.Release(), std::move(stop));
  }

  void Post(std::function<void()> callback) {
    std::unique_lock lock(mutex_);
    const auto application = application_.lock();
    if (closed_ || !application || !application->accepts_tasks) {
      return;
    }
    const std::uint64_t identity = next_post_identity_++;
    posts_.emplace(identity, std::move(callback));
    std::weak_ptr<TaskScopeState> weak = weak_from_this();
    try {
      dispatcher_([weak, identity] {
        if (auto scope = weak.lock()) {
          scope->DeliverPost(identity);
        }
      });
    } catch (...) {
      auto discarded = posts_.extract(identity);
      lock.unlock();
      throw;
    }
  }

  void Detach(std::uint64_t identity) noexcept {
    tasks_.erase(identity);
  }

  void Close() noexcept {
    std::unordered_map<std::uint64_t, std::function<void()>> posts;
    {
      std::scoped_lock lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
      posts = std::move(posts_);
    }
    posts.clear();
    auto tasks = std::move(tasks_);
    for (auto& [identity, execution] : tasks) {
      execution->CloseOnUi();
    }
    // Active callbacks and Tasks retain their own context until they finish or reach deferred cancellation.
    context_.reset();
  }

private:
  void DeliverPost(std::uint64_t identity) noexcept {
    std::function<void()> callback;
    {
      std::scoped_lock lock(mutex_);
      const auto application = application_.lock();
      if (closed_ || !application || !application->accepts_tasks) {
        return;
      }
      const auto found = posts_.find(identity);
      if (found == posts_.end()) {
        return;
      }
      callback = std::move(found->second);
      posts_.erase(found);
    }
    ExecutionGuard guard(context_);
    callback();
  }

  UiThreadDispatcher dispatcher_;
  std::thread::id ui_thread_;
  std::shared_ptr<ExecutionContext> context_;
  const std::weak_ptr<ApplicationRuntimeState> application_;
  std::mutex mutex_;
  bool closed_ = false;
  std::uint64_t next_identity_ = 1;
  std::uint64_t next_post_identity_ = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<TaskExecution>> tasks_;
  std::unordered_map<std::uint64_t, std::function<void()>> posts_;
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

  void Suspend(TaskContinuation continuation) {
    Runtime& runtime = *CurrentApplicationRuntime()->owner;
    const auto deadline = runtime.TimerNow() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(duration_seconds_));
    cancel_ = runtime.ScheduleTimerAt(deadline, [continuation] { continuation.Resume(); });
  }

  void TakeResult() const noexcept {}

private:
  double duration_seconds_;
  std::function<void()> cancel_;
};

Task<void> DelayTask(double duration_seconds) {
  co_await TaskAwaiter<DelayAwaiter>(duration_seconds);
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
      execution->ResumeOnUi(execution->coroutine_);
    }
  });
}

void TaskExecution::Cancel() noexcept {
  if (cancellation_requested_.exchange(true)) {
    return;
  }
  if (IsUiThread() && !running_) {
    CancelOnUi();
    return;
  }
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  PostNoexcept([weak] {
    if (auto execution = weak.lock()) {
      execution->CancelOnUi();
    }
  });
}

void TaskExecution::CloseOnUi() noexcept {
  cancellation_requested_ = true;
  CancelOnUi();
}

void TaskExecution::QueueCompletion() noexcept {
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  PostNoexcept([weak] {
    if (auto execution = weak.lock()) {
      execution->CompleteOnUi();
    }
  });
}

void TaskExecution::QueueResume(std::weak_ptr<TaskWaitState> wait) noexcept {
  std::weak_ptr<TaskExecution> weak = shared_from_this();
  PostNoexcept([weak, wait = std::move(wait)] {
    if (auto execution = weak.lock()) {
      if (auto pending = wait.lock(); pending && std::exchange(pending->active, false)) {
        execution->ResumeOnUi(pending->coroutine);
      }
    }
  });
}

void TaskExecution::ResumeOnUi(std::coroutine_handle<> coroutine) noexcept {
  const auto self = shared_from_this();
  const auto application = context_->application.lock();
  const auto grant = stop_ ? stop_->grant.lock() : nullptr;
  if (grant && grant->WasRevoked()) {
    ExecutionGuard guard(context_);
    stop_->source.request_stop();
    CancelOnUi();
    return;
  }
  if (cancellation_requested_ || !application || (!application->accepts_tasks && !stop_)) {
    CancelOnUi();
    return;
  }
  if (!coroutine || coroutine.done()) {
    return;
  }
  ExecutionGuard guard(context_);
  running_ = true;
  auto* previous = std::exchange(current_system_task, stop_.get());
  coroutine.resume();
  current_system_task = previous;
  running_ = false;
  if (cancellation_requested_ && coroutine_) {
    CancelOnUi();
  }
}

void TaskExecution::CancelOnUi() noexcept {
  if (running_ || !coroutine_) {
    return;
  }
  const auto self = shared_from_this();
  ExecutionGuard guard(context_);
  DetachFromScope();
  auto coroutine = std::exchange(coroutine_, {});
  coroutine.destroy();
}

void TaskExecution::CompleteOnUi() noexcept {
  if (!coroutine_ || !coroutine_.done()) {
    return;
  }
  const auto self = shared_from_this();
  ExecutionGuard guard(context_);
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

void EnqueueWorkerOperation(std::function<void()> operation, const char* api_name) {
  if (!operation) {
    throw std::invalid_argument("HuxerUI worker operation must not be empty");
  }
#if defined(__EMSCRIPTEN__)
  throw std::runtime_error(std::string("HuxerUI ") + api_name +
                           " is unavailable because this Web build has no worker execution capability");
#else
  HUXERUI_UNUSED(api_name);
  WorkerExecutor::Instance().Submit(std::move(operation));
#endif
}

void SubmitWorkerSequenceOperation(
    const std::shared_ptr<WorkerSequenceState>& sequence,
    const std::shared_ptr<WorkerOperation>& operation
) {
  bool start = false;
  {
    std::scoped_lock lock(sequence->mutex);
    if (!sequence->active.expired()) {
      sequence->queued.push_back(operation);
    } else {
      sequence->active = operation;
      start = true;
    }
  }
  if (start) {
    operation->Start();
  }
}

void RetireWorkerSequenceOperation(
    const std::shared_ptr<WorkerSequenceState>& sequence,
    const std::shared_ptr<WorkerOperation>& operation
) noexcept {
  std::shared_ptr<WorkerOperation> next;
  {
    std::scoped_lock lock(sequence->mutex);
    if (sequence->active.lock() == operation) {
      sequence->active.reset();
      while (!sequence->queued.empty() && !next) {
        next = sequence->queued.front().lock();
        sequence->queued.pop_front();
      }
      if (next) {
        sequence->active = next;
      }
    } else {
      sequence->queued.erase(
          std::remove_if(sequence->queued.begin(), sequence->queued.end(), [&](const auto& queued) {
            const std::shared_ptr<WorkerOperation> current = queued.lock();
            return !current || current == operation;
          }),
          sequence->queued.end()
      );
    }
  }
  if (next) {
    next->Start();
  }
}

std::size_t WorkerConcurrency() noexcept {
#if defined(__EMSCRIPTEN__)
  return 0;
#else
  return WorkerExecutor::Instance().WorkerCount();
#endif
}

std::shared_ptr<TaskScopeState>
MakeTaskScopeState(UiThreadDispatcher dispatcher, std::shared_ptr<ExecutionContext> context) {
  return std::make_shared<TaskScopeState>(std::move(dispatcher), std::move(context));
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

void TaskContinuation::Resume() const noexcept {
  if (const auto wait = wait_.lock()) {
    if (auto execution = wait->execution.lock()) {
      execution->QueueResume(wait);
    }
  }
}

std::shared_ptr<TaskWaitState> BeginTaskWait(const std::weak_ptr<TaskExecution>& execution,
                                          std::coroutine_handle<> coroutine) {
  if (auto active = execution.lock()) {
    active->CheckStop();
    return active->BeginWait(coroutine);
  }
  throw std::logic_error("HuxerUI asynchronous operation requires a running Task execution");
}

void RetireTaskWait(const std::shared_ptr<TaskWaitState>& wait) noexcept {
  if (wait) {
    wait->active = false;
  }
}

void ResumeTaskWait(const std::shared_ptr<TaskWaitState>& wait) {
  RetireTaskWait(wait);
  if (auto active = wait->execution.lock()) {
    active->CheckStop();
  }
}

void WakeStoppedTask(const SystemTaskStopState& stop) noexcept {
  if (auto execution = stop.execution.lock()) {
    execution->WakeStop();
  }
}

bool IsCurrentSystemTask(const SystemTaskStopState& stop) noexcept {
  return current_system_task == &stop;
}

bool IsSystemTaskCleaningUp() noexcept {
  return current_system_task && current_system_task->cleaning_up;
}

TaskHandle LaunchSystemTask(const std::shared_ptr<TaskScopeState>& scope, Task<void> task,
                            std::shared_ptr<SystemTaskStopState> stop) {
  return scope->Launch(std::move(task), std::move(stop));
}

} // namespace huxerui::detail

namespace huxerui {

WorkerSequence::WorkerSequence() : state_(std::make_shared<detail::WorkerSequenceState>()) {}

void TaskHandle::Cancel() const noexcept {
  if (auto execution = execution_.lock()) {
    execution->Cancel();
  }
}

TaskHandle TaskScope::Launch(Task<void>&& task) const {
  if (detail::IsSystemTaskCleaningUp()) {
    throw std::logic_error("HuxerUI TaskScope::Launch() cannot run during system task cleanup");
  }
  if (detail::Composer::Current() != nullptr) {
    throw std::logic_error("HuxerUI TaskScope::Launch() cannot run during view composition");
  }
  if (!state_) {
    throw std::logic_error("HuxerUI TaskScope is empty");
  }
  return state_->Launch(task.Release());
}

void TaskScope::PostErased(std::function<void()> callback) const {
  if (detail::IsSystemTaskCleaningUp()) {
    throw std::logic_error("HuxerUI TaskScope::Post() cannot run during system task cleanup");
  }
  if (detail::Composer::Current() != nullptr) {
    throw std::logic_error("HuxerUI TaskScope::Post() cannot run during view composition");
  }
  if (!state_) {
    throw std::logic_error("HuxerUI TaskScope is empty");
  }
  state_->Post(std::move(callback));
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
