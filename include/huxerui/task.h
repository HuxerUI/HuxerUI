#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
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

private:
  explicit TaskScope(std::shared_ptr<detail::TaskScopeState> state) noexcept : state_(std::move(state)) {}

  std::shared_ptr<detail::TaskScopeState> state_;

  friend class detail::RecomposeScope;
};

TaskScope UseTaskScope();

[[nodiscard]] Task<void> Delay(std::chrono::duration<double> duration);

} // namespace huxerui
