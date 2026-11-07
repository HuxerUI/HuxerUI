#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace huxerui {

/// Makes standard duration literals available with `using namespace huxerui`, including `200ms` and `2s`.
using std::chrono_literals::operator""h;
using std::chrono_literals::operator""min;
using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""ns;
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""us;

namespace detail {

class RecomposeScope;
class TaskExecution;
class TaskScopeState;
class WorkerOperation;
class WorkerSequenceState;
struct TaskWaitState;

// A completion can resume only the suspension that issued it.
class TaskContinuation final {
public:
  TaskContinuation() = default;
  explicit TaskContinuation(const std::shared_ptr<TaskWaitState>& wait) noexcept : wait_(wait) {}
  void Resume() const noexcept;

private:
  std::weak_ptr<TaskWaitState> wait_;
};

std::shared_ptr<TaskWaitState> BeginTaskWait(const std::weak_ptr<TaskExecution>& execution,
                                          std::coroutine_handle<> coroutine);
void RetireTaskWait(const std::shared_ptr<TaskWaitState>& wait) noexcept;
void ResumeTaskWait(const std::shared_ptr<TaskWaitState>& wait);

template <class Operation> class TaskAwaiter final {
public:
  template <class... Args> explicit TaskAwaiter(Args&&... args) : operation_(std::forward<Args>(args)...) {}
  ~TaskAwaiter() { RetireTaskWait(wait_); }

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> coroutine) {
    wait_ = BeginTaskWait(coroutine.promise().Execution(), coroutine);
    operation_.Suspend(TaskContinuation(wait_));
  }

  decltype(auto) await_resume() {
    ResumeTaskWait(wait_);
    return operation_.TakeResult();
  }

private:
  Operation operation_;
  std::shared_ptr<TaskWaitState> wait_;
};

void NotifyTaskCompleted(const std::weak_ptr<TaskExecution>& execution) noexcept;
void EnqueueWorkerOperation(std::function<void()> operation, const char* api_name = "RunWorker()");
void SubmitWorkerSequenceOperation(
    const std::shared_ptr<WorkerSequenceState>& sequence,
    const std::shared_ptr<WorkerOperation>& operation
);
void RetireWorkerSequenceOperation(
    const std::shared_ptr<WorkerSequenceState>& sequence,
    const std::shared_ptr<WorkerOperation>& operation
) noexcept;

class TaskPromiseBase {
public:
  [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
    return {};
  }

  class FinalAwaiter {
  public:
    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <class Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coroutine) const noexcept {
      TaskPromiseBase& promise = coroutine.promise();
      if (promise.continuation_) {
        return promise.continuation_;
      }
      NotifyTaskCompleted(promise.execution_);
      return std::noop_coroutine();
    }

    void await_resume() const noexcept {}
  };

  [[nodiscard]] FinalAwaiter final_suspend() const noexcept {
    return {};
  }

  void unhandled_exception() noexcept {
    exception_ = std::current_exception();
  }

  void BindExecution(std::weak_ptr<TaskExecution> execution) noexcept {
    execution_ = std::move(execution);
  }

  [[nodiscard]] const std::weak_ptr<TaskExecution>& Execution() const noexcept {
    return execution_;
  }

  void SetContinuation(std::coroutine_handle<> continuation) noexcept {
    continuation_ = continuation;
  }

  void RethrowException() const {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

private:
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::exception_ptr exception_;
};

} // namespace detail

/// Represents one lazy, move-only asynchronous result owned by HuxerUI structured concurrency.
///
/// Calling a coroutine that returns Task does not run its body immediately. Consume the Task exactly once by moving it
/// into `co_await` from another HuxerUI Task. A Task that is never awaited or launched is destroyed without running.
/// Values must be move-constructible non-cv object types; use `Task<void>` when no value is returned. Exceptions escape
/// from the awaiting `co_await` expression.
///
/// @code
/// Task<int> LoadCount() {
///   HttpResponse response = co_await client.GetAsync("https://example.com/count");
///   co_return ParseCount(response.body);
/// }
///
/// tasks.Launch([=]() -> Task<void> {
///   count = co_await LoadCount();
/// });
/// @endcode
template <class T> class [[nodiscard]] Task {
  static_assert(std::is_object_v<T>, "HuxerUI Task value type must be a non-reference object type");
  static_assert(std::same_as<T, std::remove_cv_t<T>>, "HuxerUI Task value type must not be cv-qualified");
  static_assert(std::move_constructible<T>, "HuxerUI Task value type must be move constructible");

public:
  class promise_type final : public detail::TaskPromiseBase {
  public:
    Task get_return_object() noexcept {
      return Task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    template <class Value>
      requires std::constructible_from<T, Value&&>
    void return_value(Value&& value) {
      value_.emplace(std::forward<Value>(value));
    }

    T TakeValue() {
      if (!value_) {
        throw std::logic_error("HuxerUI Task completed without a value");
      }
      return std::move(*value_);
    }

  private:
    std::optional<T> value_;
  };

  Task() noexcept = default;

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : coroutine_(std::exchange(other.coroutine_, {})) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      Reset();
      coroutine_ = std::exchange(other.coroutine_, {});
    }
    return *this;
  }

  ~Task() {
    Reset();
  }

  class Awaiter {
  public:
    explicit Awaiter(std::coroutine_handle<promise_type> coroutine) noexcept : coroutine_(coroutine) {}

    Awaiter(const Awaiter&) = delete;
    Awaiter& operator=(const Awaiter&) = delete;

    Awaiter(Awaiter&& other) noexcept : coroutine_(std::exchange(other.coroutine_, {})) {}

    ~Awaiter() {
      if (coroutine_) {
        coroutine_.destroy();
      }
    }

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <class ParentPromise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<ParentPromise> parent) noexcept {
      static_assert(
          std::derived_from<ParentPromise, detail::TaskPromiseBase>,
          "HuxerUI Task can only be awaited from another HuxerUI Task"
      );
      coroutine_.promise().BindExecution(parent.promise().Execution());
      coroutine_.promise().SetContinuation(parent);
      return coroutine_;
    }

    T await_resume() {
      coroutine_.promise().RethrowException();
      return coroutine_.promise().TakeValue();
    }

  private:
    std::coroutine_handle<promise_type> coroutine_;
  };

  Awaiter operator co_await() && {
    if (!coroutine_) {
      throw std::logic_error("HuxerUI cannot await an empty Task");
    }
    return Awaiter(std::exchange(coroutine_, {}));
  }

  Awaiter operator co_await() & = delete;

private:
  explicit Task(std::coroutine_handle<promise_type> coroutine) noexcept : coroutine_(coroutine) {}

  void Reset() noexcept {
    if (coroutine_) {
      coroutine_.destroy();
      coroutine_ = {};
    }
  }

  std::coroutine_handle<promise_type> coroutine_;
};

/// Specializes Task for an asynchronous operation with no result value.
///
/// A `Task<void>` may be awaited by another HuxerUI Task or transferred to `TaskScope::Launch()`. It otherwise has the
/// same lazy, move-only, single-consumer lifetime as `Task<T>`.
template <> class [[nodiscard]] Task<void> {
public:
  class promise_type final : public detail::TaskPromiseBase {
  public:
    Task get_return_object() noexcept {
      return Task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    void return_void() const noexcept {}
  };

  Task() noexcept = default;

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : coroutine_(std::exchange(other.coroutine_, {})) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      Reset();
      coroutine_ = std::exchange(other.coroutine_, {});
    }
    return *this;
  }

  ~Task() {
    Reset();
  }

  class Awaiter {
  public:
    explicit Awaiter(std::coroutine_handle<promise_type> coroutine) noexcept : coroutine_(coroutine) {}

    Awaiter(const Awaiter&) = delete;
    Awaiter& operator=(const Awaiter&) = delete;

    Awaiter(Awaiter&& other) noexcept : coroutine_(std::exchange(other.coroutine_, {})) {}

    ~Awaiter() {
      if (coroutine_) {
        coroutine_.destroy();
      }
    }

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <class ParentPromise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<ParentPromise> parent) noexcept {
      static_assert(
          std::derived_from<ParentPromise, detail::TaskPromiseBase>,
          "HuxerUI Task can only be awaited from another HuxerUI Task"
      );
      coroutine_.promise().BindExecution(parent.promise().Execution());
      coroutine_.promise().SetContinuation(parent);
      return coroutine_;
    }

    void await_resume() const {
      coroutine_.promise().RethrowException();
    }

  private:
    std::coroutine_handle<promise_type> coroutine_;
  };

  Awaiter operator co_await() && {
    if (!coroutine_) {
      throw std::logic_error("HuxerUI cannot await an empty Task");
    }
    return Awaiter(std::exchange(coroutine_, {}));
  }

  Awaiter operator co_await() & = delete;

private:
  explicit Task(std::coroutine_handle<promise_type> coroutine) noexcept : coroutine_(coroutine) {}

  std::coroutine_handle<promise_type> Release() noexcept {
    return std::exchange(coroutine_, {});
  }

  void Reset() noexcept {
    if (coroutine_) {
      coroutine_.destroy();
      coroutine_ = {};
    }
  }

  std::coroutine_handle<promise_type> coroutine_;

  friend class TaskScope;
  friend class detail::TaskScopeState;
};

namespace detail {
template <class T> class CallbackOperationState;
template <class T, class Start> class CallbackAwaiter;
} // namespace detail

/// A copyable completion endpoint supplied by AwaitCallback; it does not keep the waiting Task alive.
/// Complete and Fail may be called from any thread. Only the first valid submission takes effect; submissions after
/// cancellation are ignored. Values follow Task's move-constructible, non-cv object requirements.
template <class T> class CallbackCompletion final {
public:
  /// Submits a value. Exceptions while storing it are delivered to the awaiting Task; constructing the argument
  /// before this call retains ordinary C++ exception behavior. Discarded values may be destroyed on the callback
  /// thread.
  void Complete(T value) const;

  /// Submits an exception for rethrow at co_await. A null exception throws std::invalid_argument even after completion
  /// or cancellation.
  void Fail(std::exception_ptr error) const;

private:
  explicit CallbackCompletion(std::weak_ptr<detail::CallbackOperationState<T>> state) : state_(std::move(state)) {}

  std::weak_ptr<detail::CallbackOperationState<T>> state_;

  template <class, class> friend class detail::CallbackAwaiter;
};

/// Completes an AwaitCallback<void> operation without a result value, with the same lifetime and threading rules.
template <> class CallbackCompletion<void> final {
public:
  /// Submits successful completion without a value.
  void Complete() const;

  /// Submits an exception for rethrow at co_await. A null exception throws std::invalid_argument even after completion
  /// or cancellation.
  void Fail(std::exception_ptr error) const;

private:
  explicit CallbackCompletion(std::weak_ptr<detail::CallbackOperationState<void>> state) : state_(std::move(state)) {}

  std::weak_ptr<detail::CallbackOperationState<void>> state_;

  template <class, class> friend class detail::CallbackAwaiter;
};

namespace detail {

template <class T> class CallbackOperationState final {
  using StoredResult = std::conditional_t<std::is_void_v<T>, bool, T>;
  enum class Phase { Pending, Completed, Canceled };

public:
  void Bind(TaskContinuation continuation) {
    // Bind before publishing the completion endpoint; no callback can access these fields yet.
    continuation_ = std::move(continuation);
  }

  void Complete(StoredResult&& value) noexcept {
    Finish([&] { result_.emplace(std::move(value)); });
  }

  void Fail(std::exception_ptr error) noexcept {
    Finish([&] { exception_ = std::move(error); });
  }

  void SetCancellation(std::function<void()> cancellation) noexcept {
    TaskContinuation continuation;
    bool cancel = false;
    {
      std::scoped_lock lock(mutex_);
      starting_ = false;
      if (phase_ == Phase::Pending) {
        cancellation_ = std::move(cancellation);
      } else if (phase_ == Phase::Canceled) {
        cancel = true;
      } else {
        continuation = std::exchange(continuation_, {});
      }
    }
    if (cancel && cancellation) {
      cancellation();
    }
    continuation.Resume();
  }

  void Cancel() noexcept {
    std::function<void()> cancellation;
    bool cancel = false;
    {
      std::scoped_lock lock(mutex_);
      cancel = phase_ == Phase::Pending;
      phase_ = Phase::Canceled;
      continuation_ = {};
      cancellation = std::move(cancellation_);
    }
    // Release captures on the owning thread and outside the lock, including after successful completion.
    if (cancel && cancellation) {
      cancellation();
    }
  }

  T TakeResult() {
    // Resumption synchronizes with publication of the winning outcome; later submissions cannot modify it.
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    if (!result_) {
      throw std::logic_error("HuxerUI callback operation resumed without a result");
    }
    if constexpr (!std::is_void_v<T>) {
      return std::move(*result_);
    }
  }

private:
  template <class Store> void Finish(Store store) noexcept {
    TaskContinuation continuation;
    {
      std::scoped_lock lock(mutex_);
      if (phase_ != Phase::Pending) {
        return;
      }
      try {
        store();
      } catch (...) {
        exception_ = std::current_exception();
      }
      phase_ = Phase::Completed;
      // Even a nested platform event loop must not resume the coroutine while its starter is still executing.
      if (!starting_) {
        continuation = std::exchange(continuation_, {});
      }
    }
    continuation.Resume();
  }

  std::mutex mutex_;
  TaskContinuation continuation_;
  std::function<void()> cancellation_;
  std::optional<StoredResult> result_;
  std::exception_ptr exception_;
  Phase phase_ = Phase::Pending;
  bool starting_ = true;
};

template <class T, class Start> class CallbackAwaiter final {
public:
  explicit CallbackAwaiter(Start& start) : start_(start), state_(std::make_shared<CallbackOperationState<T>>()) {}

  CallbackAwaiter(const CallbackAwaiter&) = delete;
  CallbackAwaiter& operator=(const CallbackAwaiter&) = delete;

  ~CallbackAwaiter() {
    state_->Cancel();
  }

  void Suspend(TaskContinuation continuation) {
    state_->Bind(std::move(continuation));
    std::function<void()> cancellation;
    try {
      cancellation = std::invoke(std::move(start_), CallbackCompletion<T>(state_));
    } catch (...) {
      state_->Fail(std::current_exception());
    }
    state_->SetCancellation(std::move(cancellation));
  }

  T TakeResult() {
    return state_->TakeResult();
  }

private:
  // The enclosing RunCallbackTask frame owns the starter until this awaiter is destroyed.
  Start& start_;
  std::shared_ptr<CallbackOperationState<T>> state_;
};

template <class T, class Start> Task<T> RunCallbackTask(Start start) {
  if constexpr (std::is_void_v<T>) {
    co_await TaskAwaiter<CallbackAwaiter<T, Start>>(start);
  } else {
    co_return co_await TaskAwaiter<CallbackAwaiter<T, Start>>(start);
  }
}

} // namespace detail

template <class T> void CallbackCompletion<T>::Complete(T value) const {
  if (auto state = state_.lock()) {
    state->Complete(std::move(value));
  }
}

template <class T> void CallbackCompletion<T>::Fail(std::exception_ptr error) const {
  if (!error) {
    throw std::invalid_argument("HuxerUI CallbackCompletion::Fail() requires a non-null exception");
  }
  if (auto state = state_.lock()) {
    state->Fail(std::move(error));
  }
}

inline void CallbackCompletion<void>::Complete() const {
  if (auto state = state_.lock()) {
    state->Complete(true);
  }
}

inline void CallbackCompletion<void>::Fail(std::exception_ptr error) const {
  if (!error) {
    throw std::invalid_argument("HuxerUI CallbackCompletion::Fail() requires a non-null exception");
  }
  if (auto state = state_.lock()) {
    state->Fail(std::move(error));
  }
}

/// Adapts one callback operation to a lazy Task, immediately retaining its starter, including move-only captures.
/// The starter receives CallbackCompletion<T> and returns a std::function<void()> cancellation action (empty when
/// unavailable). It runs once on the owning Task's UI thread. Results and exceptions resume through that Task's
/// dispatcher and captured execution context, never inline before the starter returns. The starter must not block.
///
/// Startup exceptions and Fail() propagate at co_await; the first completion or failure wins. Canceling a pending Task
/// retires its continuation and invokes the cancellation action at most once on the owning thread, even if canceled
/// before registration. The action must not throw; an escaping exception terminates. Cancellation after a submission
/// but before queued resumption still suppresses application code without invoking the action. The action is not an
/// unconditional cleanup hook and does not guarantee that native work has stopped when it returns. Late callbacks are
/// ignored, and Task cancellation never delivers a cancellation value or exception.
///
/// Capture owned inputs: retaining a starter does not extend the lifetime of objects captured by reference. A starter
/// that throws after partially starting work must clean up that work itself. Adapters must settle a still-live Task
/// with Complete or Fail when their host closes; native dismissal and operation-specific results remain adapter-owned.
/// Native work that survives Task cancellation must retain its own resources independently of the starter.
template <class T, class Start>
  requires std::constructible_from<std::decay_t<Start>, Start&&> &&
           std::move_constructible<std::decay_t<Start>> &&
           std::invocable<std::decay_t<Start>, CallbackCompletion<T>> &&
           std::convertible_to<
               std::invoke_result_t<std::decay_t<Start>, CallbackCompletion<T>>, std::function<void()>>
Task<T> AwaitCallback(Start&& start) {
  return detail::RunCallbackTask<T>(std::decay_t<Start>(std::forward<Start>(start)));
}

namespace detail {

template <class Value> struct IsTask : std::false_type {};

template <class Value> struct IsTask<Task<Value>> : std::true_type {};

class WorkerOperation {
public:
  virtual ~WorkerOperation() = default;

  virtual void Start() noexcept = 0;
};

template <class Function, class... Arguments> class WorkerInvocation final {
public:
  using Result = std::invoke_result_t<Function, Arguments...>;

  WorkerInvocation(Function function, std::tuple<Arguments...> arguments)
      : function_(std::move(function)), arguments_(std::move(arguments)) {}

  Result Invoke(std::stop_token) {
    return std::apply(
        [this](auto&&... arguments) -> Result {
          return std::invoke(std::move(function_), std::forward<decltype(arguments)>(arguments)...);
        },
        std::move(arguments_)
    );
  }

private:
  Function function_;
  std::tuple<Arguments...> arguments_;
};

template <class Operation, class Result>
class WorkerOperationState final : public WorkerOperation,
                                   public std::enable_shared_from_this<WorkerOperationState<Operation, Result>> {
public:
  WorkerOperationState(Operation operation, std::shared_ptr<WorkerSequenceState> sequence)
      : operation_(std::move(operation)), sequence_(std::move(sequence)) {}

  void Suspend(TaskContinuation continuation) {
    {
      std::scoped_lock lock(mutex_);
      continuation_ = std::move(continuation);
    }
    if (sequence_) {
      SubmitWorkerSequenceOperation(sequence_, this->shared_from_this());
    } else {
      Schedule();
    }
  }

  Result TakeResult() {
    std::scoped_lock lock(mutex_);
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    if (!result_.has_value()) {
      throw std::logic_error("HuxerUI worker operation resumed without a result");
    }
    if constexpr (!std::is_void_v<Result>) {
      return std::move(*result_);
    }
  }

  void Cancel() noexcept {
    bool release_operation = false;
    bool retire_sequence = false;
    bool request_stop = false;
    {
      std::scoped_lock lock(mutex_);
      canceled_ = true;
      continuation_ = {};
      if (phase_ == Phase::Queued || phase_ == Phase::Scheduled) {
        phase_ = Phase::Finished;
        release_operation = true;
        retire_sequence = static_cast<bool>(sequence_);
      } else if (phase_ == Phase::Running && sequence_) {
        request_stop = true;
      }
    }
    if (request_stop) {
      stop_source_.request_stop();
    }
    if (release_operation) {
      operation_.reset();
    }
    if (retire_sequence) {
      RetireWorkerSequenceOperation(sequence_, this->shared_from_this());
    }
  }

  void Start() noexcept override {
    try {
      Schedule();
    } catch (...) {
      CompleteWithException(std::current_exception());
    }
  }

private:
  enum class Phase {
    Queued,
    Scheduled,
    Running,
    Finished,
  };

  using StoredResult = std::conditional_t<std::is_void_v<Result>, bool, Result>;

  void Schedule() {
    std::shared_ptr<WorkerOperationState> self;
    {
      std::scoped_lock lock(mutex_);
      if (phase_ != Phase::Queued) {
        return;
      }
      phase_ = Phase::Scheduled;
      self = this->shared_from_this();
    }
    EnqueueWorkerOperation([self] { self->Run(); }, sequence_ ? "WorkerSequence::Run()" : "RunWorker()");
  }

  void Run() noexcept {
    Operation* operation = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (phase_ != Phase::Scheduled) {
        return;
      }
      phase_ = Phase::Running;
      operation = &*operation_;
    }

    try {
      if constexpr (std::is_void_v<Result>) {
        operation->Invoke(stop_source_.get_token());
        operation_.reset();
        CompleteWithResult(true);
      } else {
        Result result = operation->Invoke(stop_source_.get_token());
        operation_.reset();
        CompleteWithResult(std::move(result));
      }
    } catch (...) {
      operation_.reset();
      CompleteWithException(std::current_exception());
    }
  }

  template <class Value> void CompleteWithResult(Value&& result) noexcept {
    try {
      TaskContinuation continuation;
      {
        std::scoped_lock lock(mutex_);
        if (!canceled_) {
          result_.emplace(std::forward<Value>(result));
          continuation = std::exchange(continuation_, {});
        }
        phase_ = Phase::Finished;
      }
      continuation.Resume();
      RetireSequence();
    } catch (...) {
      CompleteWithException(std::current_exception());
    }
  }

  void CompleteWithException(std::exception_ptr exception) noexcept {
    TaskContinuation continuation;
    {
      std::scoped_lock lock(mutex_);
      if (phase_ == Phase::Finished) {
        return;
      }
      if (!canceled_) {
        exception_ = std::move(exception);
        continuation = std::exchange(continuation_, {});
      }
      phase_ = Phase::Finished;
    }
    operation_.reset();
    continuation.Resume();
    RetireSequence();
  }

  void RetireSequence() noexcept {
    if (sequence_) {
      RetireWorkerSequenceOperation(sequence_, this->shared_from_this());
    }
  }

  std::mutex mutex_;
  std::optional<Operation> operation_;
  std::shared_ptr<WorkerSequenceState> sequence_;
  TaskContinuation continuation_;
  std::optional<StoredResult> result_;
  std::exception_ptr exception_;
  std::stop_source stop_source_;
  Phase phase_ = Phase::Queued;
  bool canceled_ = false;
};

template <class Operation, class Result> class WorkerAwaiter final {
public:
  explicit WorkerAwaiter(Operation operation)
      : state_(std::make_shared<WorkerOperationState<Operation, Result>>(std::move(operation), nullptr)) {}

  WorkerAwaiter(std::shared_ptr<WorkerSequenceState> sequence, Operation operation)
      : state_(std::make_shared<WorkerOperationState<Operation, Result>>(
            std::move(operation), std::move(sequence)
        )) {}

  WorkerAwaiter(const WorkerAwaiter&) = delete;
  WorkerAwaiter& operator=(const WorkerAwaiter&) = delete;
  WorkerAwaiter(WorkerAwaiter&&) noexcept = default;
  WorkerAwaiter& operator=(WorkerAwaiter&&) noexcept = default;

  ~WorkerAwaiter() {
    if (state_) {
      state_->Cancel();
    }
  }

  void Suspend(TaskContinuation continuation) {
    state_->Suspend(std::move(continuation));
  }

  Result TakeResult() {
    return state_->TakeResult();
  }

private:
  std::shared_ptr<WorkerOperationState<Operation, Result>> state_;
};

template <class Result, class Operation> Task<Result> RunWorkerTask(Operation operation) {
  if constexpr (std::is_void_v<Result>) {
    co_await TaskAwaiter<WorkerAwaiter<Operation, Result>>(std::move(operation));
    co_return;
  } else {
    co_return co_await TaskAwaiter<WorkerAwaiter<Operation, Result>>(std::move(operation));
  }
}

template <class Function, class... Arguments> class WorkerSequenceInvocation final {
public:
  using Result = std::invoke_result_t<Function, std::stop_token, Arguments...>;

  WorkerSequenceInvocation(Function function, std::tuple<Arguments...> arguments)
      : function_(std::move(function)), arguments_(std::move(arguments)) {}

  Result Invoke(std::stop_token stop_token) {
    return std::apply(
        [this, stop_token](auto&&... arguments) -> Result {
          return std::invoke(
              std::move(function_), stop_token, std::forward<decltype(arguments)>(arguments)...
          );
        },
        std::move(arguments_)
    );
  }

private:
  Function function_;
  std::tuple<Arguments...> arguments_;
};

template <class Result, class Operation>
Task<Result> RunWorkerSequenceTask(std::shared_ptr<WorkerSequenceState> sequence, Operation operation) {
  if constexpr (std::is_void_v<Result>) {
    co_await TaskAwaiter<WorkerAwaiter<Operation, Result>>(std::move(sequence), std::move(operation));
    co_return;
  } else {
    co_return co_await TaskAwaiter<WorkerAwaiter<Operation, Result>>(std::move(sequence), std::move(operation));
  }
}

} // namespace detail

/// Serializes blocking operations for one application-owned resource on HuxerUI's shared worker pool.
///
/// Copies share one ordering identity. Operations begin in the order their lazy Tasks are awaited, with at most one
/// callable from that sequence running at a time. Independent WorkerSequence instances may run concurrently, and no
/// instance owns or reserves a particular worker thread.
///
/// Every callable receives a leading `std::stop_token`. Canceling its Task skips queued work or requests cooperative
/// stop for active work; a following operation does not begin until the active callable has returned and released its
/// captures. Results and exceptions resume through the awaiting Task's UI thread.
///
/// Keep one WorkerSequence with the resource whose access it protects rather than recreating it per operation.
///
/// @code
/// class LogStore {
/// public:
///   Task<void> Append(std::string entry) {
///     co_await operations_.Run([this](std::stop_token stop_token, std::string value) {
///       if (!stop_token.stop_requested()) {
///         AppendBlocking(value);
///       }
///     }, std::move(entry));
///   }
///
/// private:
///   WorkerSequence operations_;
/// };
/// @endcode
class WorkerSequence {
public:
  /// Creates an independent ordering identity.
  WorkerSequence();

  /// Creates a lazy worker Task that joins this sequence when awaited.
  ///
  /// The callable and arguments are decayed and owned by the returned Task. The callable must accept a leading
  /// `std::stop_token`, may return `void` or a move-constructible object, and must not return another Task. Use
  /// `std::ref()` explicitly when reference semantics are required. Web builds without worker execution report an
  /// unavailable error through the returned Task.
  template <class Function, class... Arguments>
    requires std::constructible_from<std::decay_t<Function>, Function&&> &&
             std::move_constructible<std::decay_t<Function>> &&
             (std::constructible_from<std::decay_t<Arguments>, Arguments&&> && ...) &&
             (std::move_constructible<std::decay_t<Arguments>> && ...) &&
             std::invocable<std::decay_t<Function>, std::stop_token, std::decay_t<Arguments>...>
  auto Run(Function&& function, Arguments&&... arguments) const {
    using Operation = detail::WorkerSequenceInvocation<std::decay_t<Function>, std::decay_t<Arguments>...>;
    using Result = typename Operation::Result;
    static_assert(
        std::is_void_v<Result> || (std::is_object_v<Result> && std::same_as<Result, std::remove_cv_t<Result>>),
        "HuxerUI WorkerSequence::Run() result must be void or a non-cv, non-reference object type"
    );
    static_assert(std::is_void_v<Result> || std::move_constructible<Result>,
                  "HuxerUI WorkerSequence::Run() result must be move constructible");
    static_assert(!detail::IsTask<Result>::value,
                  "HuxerUI WorkerSequence::Run() callable must not return a Task");
    if (!state_) {
      throw std::logic_error("HuxerUI WorkerSequence is empty");
    }
    return detail::RunWorkerSequenceTask<Result>(
        state_,
        Operation(
            std::decay_t<Function>(std::forward<Function>(function)),
            std::tuple<std::decay_t<Arguments>...>(std::forward<Arguments>(arguments)...)
        )
    );
  }

private:
  std::shared_ptr<detail::WorkerSequenceState> state_;
};

/// Controls cancellation of one Task launched in a TaskScope.
///
/// The handle is copyable and does not keep its Task alive. Destroying a handle does not cancel anything; the owning
/// TaskScope remains authoritative for lifetime. An empty handle and a handle whose Task already completed are inert.
class TaskHandle {
public:
  TaskHandle() noexcept = default;

  /// Requests cancellation once and returns immediately.
  ///
  /// Cancellation destroys suspended coroutine state on the owning UI thread and does not resume application code with
  /// a cancellation exception. Underlying blocking work may require its own cooperative stop contract.
  void Cancel() const noexcept;

private:
  explicit TaskHandle(std::weak_ptr<detail::TaskExecution> execution) noexcept : execution_(std::move(execution)) {}

  std::weak_ptr<detail::TaskExecution> execution_;

  friend class detail::TaskScopeState;
};

namespace detail {

template <class Factory> Task<void> InvokeTaskFactory(Factory factory) {
  co_await std::invoke(factory);
}

} // namespace detail

/// Owns asynchronous child Tasks for one composition scope or application lifetime.
///
/// Obtain a composition handle with `UseTaskScope()` or an application handle with `UseApplicationTaskScope()`.
/// Compatible recomposition retains a composition scope; unmount, scope replacement, virtual-item eviction, or
/// UiWindow teardown closes it and cancels every child. The application scope closes at Runtime shutdown.
/// A retained closed handle no longer owns its captured Environment; active callbacks keep it until they return.
/// Launch work only after composition, such as from Lifecycle setup or an event callback.
///
/// Prefer the factory overload for coroutine lambdas so the factory captures remain owned for the child Task lifetime.
/// Ignoring the returned TaskHandle creates a scope-owned fire-and-forget child that is still canceled with the scope.
///
/// @code
/// auto tasks = UseTaskScope();
///
/// Lifecycle([=] {
///   TaskHandle request = tasks.Launch([=]() -> Task<void> {
///     value = co_await service->LoadAsync();
///   });
///   return [request] { request.Cancel(); };
/// });
/// @endcode
class TaskScope {
public:
  TaskScope() noexcept = default;

  /// Transfers one ordinary `Task<void>` into this scope and schedules its first resume on the owning UI thread.
  ///
  /// Use this overload with a Task returned by a named coroutine function. Passing an empty Task, launching after the
  /// scope closes, or launching during composition throws `std::logic_error`.
  TaskHandle Launch(Task<void>&& task) const;

  /// Owns a callable that lazily creates `Task<void>` and launches the resulting child in this scope.
  ///
  /// The factory itself is invoked on the owning UI thread after the current callback or composition has returned.
  template <class Factory>
    requires std::constructible_from<std::decay_t<Factory>, Factory&&> && std::invocable<std::decay_t<Factory>&> &&
             std::same_as<std::invoke_result_t<std::decay_t<Factory>&>, Task<void>>
  TaskHandle Launch(Factory&& factory) const {
    return Launch(detail::InvokeTaskFactory(std::decay_t<Factory>(std::forward<Factory>(factory))));
  }

  /// Posts an owned `void` callback from an external thread to this scope's UI thread.
  ///
  /// Delivery is always queued and is discarded if the scope closes first. Use Post for callbacks that originate
  /// outside a running HuxerUI Task; HuxerUI awaitables such as Delay and RunWorker already restore the UI thread.
  ///
  /// @code
  /// engine.Start([tasks, value](Result result) mutable {
  ///   tasks.Post([value, result = std::move(result)]() mutable {
  ///     value = std::move(result);
  ///   });
  /// });
  /// @endcode
  template <class Callback>
    requires std::constructible_from<std::decay_t<Callback>, Callback&&> &&
             std::invocable<std::decay_t<Callback>&> &&
             std::same_as<std::invoke_result_t<std::decay_t<Callback>&>, void>
  void Post(Callback&& callback) const {
    auto owned = std::make_shared<std::decay_t<Callback>>(std::forward<Callback>(callback));
    PostErased([owned] { std::invoke(*owned); });
  }

private:
  explicit TaskScope(std::shared_ptr<detail::TaskScopeState> state) noexcept : state_(std::move(state)) {}

  void PostErased(std::function<void()> callback) const;

  std::shared_ptr<detail::TaskScopeState> state_;

  friend class detail::RecomposeScope;
  friend TaskScope UseApplicationTaskScope();
};

/// Returns the TaskScope owned by the currently composing RecomposeScope.
///
/// Calls within the same composable scope return the same lifetime without allocating a State slot. Calling outside
/// composition, or without a platform UI-thread dispatcher, throws `std::logic_error`.
TaskScope UseTaskScope();

/// Acquires the original application's shared ordinary Task scope on its application thread.
/// @return A retained scope whose tasks survive UI retirement and close during application shutdown.
/// @throws std::logic_error If no valid application context exists or shutdown has begun.
/// Can be used during application_hooks and outside composition; launch still follows TaskScope's composition
/// restrictions.
/// This scope grants no OS background execution rights. HuxerUI awaitables resume on the application thread, so a
/// completed worker result may update State directly; worker code itself must not read or write State.
/// @code{.cpp}
/// const auto tasks = UseApplicationTaskScope();
/// tasks.Launch([model]() -> Task<void> {
///   const int count = co_await RunWorker(CountItems);
///   model->count = count;
/// });
/// @endcode
TaskScope UseApplicationTaskScope();

/// Suspends for at least the requested duration and resumes on the owning UI thread.
///
/// Delay is lazy, uses the application's monotonic timer independently of UI frames, and accepts standard durations.
/// Zero still resumes asynchronously; negative or non-finite durations throw `std::invalid_argument`.
///
/// @code
/// co_await Delay(300ms);
/// visible = false;
/// @endcode
Task<void> Delay(std::chrono::duration<double> duration);

/// Executes an owned synchronous callable on HuxerUI's bounded process-wide worker pool.
///
/// The returned Task is lazy and submits work only when awaited from a running HuxerUI Task. The callable and arguments
/// are decayed and moved into that Task. A value or exception resumes on the awaiting Task's UI thread, where State may
/// be updated directly. The worker callable must not access State, Views, composition, or UI-affine platform objects.
///
/// Canceling before execution skips the callable. Canceling after it starts suppresses its continuation but cannot
/// interrupt arbitrary C++ code. Use WorkerSequence when an active operation also needs cooperative stop and strict
/// ordering with later operations. Web builds without worker execution report an unavailable error through the Task.
///
/// @code
/// tasks.Launch([=]() -> Task<void> {
///   int count = co_await RunWorker(CountPrimes, 1'000'000);
///   prime_count = count;
/// });
/// @endcode
template <class Function, class... Arguments>
  requires std::constructible_from<std::decay_t<Function>, Function&&> &&
           std::move_constructible<std::decay_t<Function>> &&
           (std::constructible_from<std::decay_t<Arguments>, Arguments&&> && ...) &&
           (std::move_constructible<std::decay_t<Arguments>> && ...) &&
           std::invocable<std::decay_t<Function>, std::decay_t<Arguments>...>
auto RunWorker(Function&& function, Arguments&&... arguments) {
  using Operation = detail::WorkerInvocation<std::decay_t<Function>, std::decay_t<Arguments>...>;
  using Result = typename Operation::Result;
  static_assert(
      std::is_void_v<Result> || (std::is_object_v<Result> && std::same_as<Result, std::remove_cv_t<Result>>),
      "HuxerUI RunWorker() result must be void or a non-cv, non-reference object type"
  );
  static_assert(std::is_void_v<Result> || std::move_constructible<Result>,
                "HuxerUI RunWorker() result must be move constructible");
  static_assert(!detail::IsTask<Result>::value, "HuxerUI RunWorker() callable must not return a Task");
  return detail::RunWorkerTask<Result>(Operation(
      std::decay_t<Function>(std::forward<Function>(function)),
      std::tuple<std::decay_t<Arguments>...>(std::forward<Arguments>(arguments)...)
  ));
}

} // namespace huxerui
