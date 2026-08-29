#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace huxerui {

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

void NotifyTaskCompleted(const std::weak_ptr<TaskExecution>& execution) noexcept;
void EnqueueWorkerOperation(std::function<void()> operation);
void ResumeTask(const std::weak_ptr<TaskExecution>& execution, std::coroutine_handle<> coroutine) noexcept;

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

template <class T> class [[nodiscard]] Task {
  static_assert(std::is_object_v<T>, "HuxerUI Task value type must be a non-reference object type");
  static_assert(std::same_as<T, std::remove_cv_t<T>>, "HuxerUI Task value type must not be cv-qualified");
  static_assert(std::move_constructible<T>, "HuxerUI Task value type must be move constructible");

public:
  class promise_type final : public detail::TaskPromiseBase {
  public:
    [[nodiscard]] Task get_return_object() noexcept {
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

template <> class [[nodiscard]] Task<void> {
public:
  class promise_type final : public detail::TaskPromiseBase {
  public:
    [[nodiscard]] Task get_return_object() noexcept {
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
};

namespace detail {

template <class Value> struct IsTask : std::false_type {};

template <class Value> struct IsTask<Task<Value>> : std::true_type {};

template <class Function, class... Arguments> class WorkerInvocation final {
public:
  using Result = std::invoke_result_t<Function, Arguments...>;

  WorkerInvocation(Function function, std::tuple<Arguments...> arguments)
      : function_(std::move(function)), arguments_(std::move(arguments)) {}

  Result Invoke() {
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
class WorkerOperationState final : public std::enable_shared_from_this<WorkerOperationState<Operation, Result>> {
public:
  explicit WorkerOperationState(Operation operation) : operation_(std::move(operation)) {}

  void Suspend(std::weak_ptr<TaskExecution> execution, std::coroutine_handle<> continuation) {
    {
      std::scoped_lock lock(mutex_);
      execution_ = std::move(execution);
      continuation_ = continuation;
    }
    const std::shared_ptr<WorkerOperationState> self = this->shared_from_this();
    EnqueueWorkerOperation([self] { self->Run(); });
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
    std::scoped_lock lock(mutex_);
    canceled_ = true;
    execution_.reset();
    continuation_ = {};
    if (!started_) {
      operation_.reset();
    }
  }

private:
  using StoredResult = std::conditional_t<std::is_void_v<Result>, bool, Result>;

  void Run() noexcept {
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || !operation_.has_value()) {
        return;
      }
      started_ = true;
    }

    try {
      if constexpr (std::is_void_v<Result>) {
        operation_->Invoke();
        result_.emplace(true);
      } else {
        result_.emplace(operation_->Invoke());
      }
    } catch (...) {
      exception_ = std::current_exception();
    }

    std::weak_ptr<TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex_);
      operation_.reset();
      started_ = false;
      if (canceled_) {
        return;
      }
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    ResumeTask(execution, continuation);
  }

  std::mutex mutex_;
  std::optional<Operation> operation_;
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::optional<StoredResult> result_;
  std::exception_ptr exception_;
  bool canceled_ = false;
  bool started_ = false;
};

template <class Operation, class Result> class WorkerAwaiter final {
public:
  explicit WorkerAwaiter(Operation operation)
      : state_(std::make_shared<WorkerOperationState<Operation, Result>>(std::move(operation))) {}

  WorkerAwaiter(const WorkerAwaiter&) = delete;
  WorkerAwaiter& operator=(const WorkerAwaiter&) = delete;
  WorkerAwaiter(WorkerAwaiter&&) noexcept = default;
  WorkerAwaiter& operator=(WorkerAwaiter&&) noexcept = default;

  ~WorkerAwaiter() {
    if (state_) {
      state_->Cancel();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> continuation) {
    static_assert(
        std::derived_from<Promise, TaskPromiseBase>,
        "HuxerUI RunWorker() can only be awaited from another HuxerUI Task"
    );
    const std::weak_ptr<TaskExecution> execution = continuation.promise().Execution();
    if (execution.expired()) {
      throw std::logic_error("HuxerUI RunWorker() requires a running Task execution");
    }
    state_->Suspend(execution, continuation);
  }

  Result await_resume() {
    return state_->TakeResult();
  }

private:
  std::shared_ptr<WorkerOperationState<Operation, Result>> state_;
};

template <class Result, class Operation> Task<Result> RunWorkerTask(Operation operation) {
  if constexpr (std::is_void_v<Result>) {
    co_await WorkerAwaiter<Operation, Result>(std::move(operation));
    co_return;
  } else {
    co_return co_await WorkerAwaiter<Operation, Result>(std::move(operation));
  }
}

} // namespace detail

class TaskHandle {
public:
  TaskHandle() noexcept = default;

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

class TaskScope {
public:
  TaskScope() noexcept = default;

  TaskHandle Launch(Task<void>&& task) const;

  template <class Factory>
    requires std::constructible_from<std::decay_t<Factory>, Factory&&> && std::invocable<std::decay_t<Factory>&> &&
             std::same_as<std::invoke_result_t<std::decay_t<Factory>&>, Task<void>>
  TaskHandle Launch(Factory&& factory) const {
    return Launch(detail::InvokeTaskFactory(std::decay_t<Factory>(std::forward<Factory>(factory))));
  }

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
};

TaskScope UseTaskScope();

[[nodiscard]] Task<void> Delay(std::chrono::duration<double> duration);

template <class Function, class... Arguments>
  requires std::constructible_from<std::decay_t<Function>, Function&&> &&
           std::move_constructible<std::decay_t<Function>> &&
           (std::constructible_from<std::decay_t<Arguments>, Arguments&&> && ...) &&
           (std::move_constructible<std::decay_t<Arguments>> && ...) &&
           std::invocable<std::decay_t<Function>, std::decay_t<Arguments>...>
[[nodiscard]] auto RunWorker(Function&& function, Arguments&&... arguments) {
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
