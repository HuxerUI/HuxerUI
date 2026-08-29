# Task and Structured Concurrency Design

## Goals

HuxerUI provides a small C++20 coroutine model for asynchronous work owned by a composition scope.

The public model consists of:

- `Task<T>`, a lazy move-only coroutine result.
- `TaskScope`, a copyable handle to a structured concurrency scope.
- `TaskHandle`, a copyable cancellation handle for one launched task.
- `UseTaskScope()`, a composition function that returns the TaskScope owned by the current `RecomposeScope`.
- `Delay()`, a lazy UI-affine minimum-duration suspension.

The model does not add `UseTask()`, `UseAsync()`, a generic `AsyncResult<T>`, Task-aware Lifecycle overloads, or Task-aware event handlers.
State remains synchronous and does not dispatch writes between threads.

## Public API

The public declarations live in `<huxerui/task.h>` and are re-exported from `<huxerui/huxerui.h>`:

```cpp
template <class T>
class [[nodiscard]] Task;

class TaskHandle {
public:
  TaskHandle() = default;

  void Cancel() const noexcept;
};

class TaskScope {
public:
  TaskScope() = default;

  TaskHandle Launch(Task<void>&& task) const;

  template <class Factory>
    requires std::constructible_from<std::decay_t<Factory>, Factory&&> &&
             std::invocable<std::decay_t<Factory>&> &&
             std::same_as<std::invoke_result_t<std::decay_t<Factory>&>, Task<void>>
  TaskHandle Launch(Factory&& factory) const;
};

TaskScope UseTaskScope();

[[nodiscard]] Task<void> Delay(std::chrono::duration<double> duration);
```

`Task<T>` accepts non-cv, non-reference object value types and `void`.
It is move-only and single-consumer: a Task may be consumed by one `co_await` expression or one `TaskScope::Launch()` call.
Destroying a Task that was never consumed destroys its coroutine frame without running it.

TaskScope consumes only `Task<void>` because a launched task has no value consumer.
A `Task<T>` with a value is composed by awaiting it from another Task.
The factory overload retains the callable in a lazy wrapper Task and invokes it on the first queued resume.
This keeps coroutine-lambda captures alive for the complete child Task lifetime and ensures neither the factory nor its coroutine body runs inside the caller.
Coroutine lambdas are therefore passed as factories rather than invoked into a temporary Task before Launch; the direct Task overload is intended for Tasks returned by ordinary coroutine functions.

Launch intentionally is not `[[nodiscard]]`.
Ignoring its returned TaskHandle creates a scope-owned fire-and-forget child that still participates in structured cancellation.
TaskHandle destruction does not cancel the task, while `Cancel()` is idempotent and does not throw.

## Execution and awaiting

Task uses `std::suspend_always` for initial suspension.
TaskScope queues the first resume through the owning platform's `UIThreadDispatcher`, which must enqueue rather than invoke inline.
Code before the first suspension therefore runs on the owning UI thread without re-entering Lifecycle setup or an event callback.

Awaiting one HuxerUI Task from another transfers the child coroutine into the same launched execution.
The child result is moved into the parent, `void` completes normally, and an exception is rethrown from the parent's `co_await` expression.
Nested Tasks do not create independent TaskHandles or TaskScope entries.

HuxerUI awaitables bind their suspended coroutine to the launched execution.
They may perform work on another thread, but completion queues the resume through the execution's UI dispatcher.
A third-party awaitable that resumes its coroutine directly does not gain this guarantee and must provide its own UI-thread handoff.

## Delays

Delay returns Task<void> rather than exposing another public awaiter type.
It accepts standard chrono durations, and `<huxerui/task.h>` exports the standard duration literal operators into the huxerui namespace so applications that use that namespace may write `Delay(3ms)` and `Delay(3s)` directly.
The explicit `std::chrono::duration<double>` form remains available.

The duration is a minimum wait rather than a precise wakeup guarantee.
Negative, non-finite, and NaN durations throw `std::invalid_argument` synchronously when Delay is called.
Zero remains asynchronous and resumes in a later UI scheduling cycle.
Delay uses the PlatformAdapter monotonic clock, so wall-clock and time-zone changes do not affect it.

Runtime owns one deadline-ordered delay queue shared by all TaskScopes in that Runtime.
Registering an earlier deadline requests one platform wakeup through the existing `RequestFrameAt()` contract.
At the beginning of that frame, Runtime removes every due registration before resuming their Task executions on the UI thread; a resumed task that awaits `Delay(0ms)` therefore cannot run that second continuation in the same batch.
Any remaining earliest deadline schedules the next wakeup.
This produces no continuously active frames, platform-specific Task implementation, background thread, or public timer service.

Delay is aligned with the UI frame scheduler and may resume on the next available display frame after its deadline.
It is appropriate for UI feedback, retry backoff, polling, and presentation lifetime, but not for audio, video, sampling, or other high-precision timing.

## Scope ownership

UseTaskScope requires an active composition and does not allocate an ordered state slot.
Every call in the same RecomposeScope returns a handle to the same lazily created TaskScope.
An ordinary helper contributes to its caller's scope, while `[[huxerui::composable]]` creates an independent TaskScope lifetime.
Launch is a side effect and is rejected during view composition; application code starts work from committed Lifecycle setup, event callbacks, or other post-composition owners.

Compatible recomposition, a changed returned root View, and keyed movement retain the TaskScope and its running tasks.
Successfully unmounting the scope, evicting a virtual item, or destroying Runtime closes the TaskScope and cancels all remaining tasks.
Virtual item state caching does not retain TaskScope or running coroutine frames.

TaskScope closure is committed with scope retirement rather than performed during speculative reconciliation.
Runtime drains Lifecycle cleanup first and then closes the retired TaskScope, so explicit resource cleanup remains deterministic before the structured cancellation fallback.
Runtime teardown closes all TaskScopes before releasing Root Services and PlatformModule state.

Copies of TaskScope may outlive their RecomposeScope, but a closed TaskScope cannot launch new work.
Launch on an empty or closed handle throws `std::logic_error`.
UseTaskScope also fails explicitly when a custom PlatformAdapter does not provide a UIThreadDispatcher.

## Cancellation

Cancellation is cooperative and never interrupts currently executing C++ code.
Cancel marks the launched execution immediately and destroys its coroutine tree on the UI thread before another HuxerUI continuation can resume it.
Destroying the root coroutine frame recursively destroys nested Task frames and suspended awaiters.

A HuxerUI awaiter owns its underlying cancellation operation in its suspended state.
Destroying that awaiter cancels or detaches the platform request, and a late completion observes the expired execution and is ignored.
Cancellation is not represented by an exception and does not resume application code.

A suspended Delay owns one removable queue registration.
TaskHandle cancellation, TaskScope closure, and Runtime teardown destroy its awaiter and remove that registration before it can resume application code.

Canceling one TaskHandle does not cancel sibling tasks in the same TaskScope.
Closing TaskScope cancels every active child.

## Lifecycle, events, and State

Lifecycle does not recognize Task types.
Application code explicitly launches a task from setup and cancels the corresponding TaskHandle from cleanup when a dependency change must stop that individual request:

```cpp
[[huxerui::composable]]
View UserName(UserId user_id, std::shared_ptr<UserService> service) {
  auto tasks = UseTaskScope();
  auto name = UseState(std::string{"Loading..."});

  Lifecycle([=] {
    TaskHandle request = tasks.Launch([=]() -> Task<void> {
      name = co_await service->LoadName(user_id);
    });

    return [request] {
      request.Cancel();
    };
  }, user_id);

  return Column {
    Text(name),
    Button("Reload").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        name = co_await service->LoadName(user_id);
      });
    }),
  };
}
```

The explicit TaskHandle is required for a Lifecycle dependency restart because the owning RecomposeScope and its TaskScope remain mounted.
Unmounting the component also closes TaskScope, so cancellation through the returned cleanup and scope retirement is intentionally idempotent.

OnClick and the typed event system keep ordinary `void` handlers.
An event callback may call TaskScope::Launch, but Runtime does not inspect its return type or retain a coroutine through EventBindings.

Task code may update State directly before its first suspension and after a HuxerUI awaitable resumes it because both execute on the owning UI thread.
State does not inspect Task context and does not dispatch writes.
Canceled or scope-retired executions do not resume and therefore cannot perform a later State write.

## Exceptions

Task stores exceptions in its coroutine promise while it has an awaiting parent.
The parent observes the exception from `co_await` and may catch it, update State, return another value, or allow it to propagate farther.

A launched root `Task<void>` has no asynchronous result consumer.
An exception that escapes that root is an uncaught application error and terminates the process after the coroutine frame is released.
The public API does not add an error callback, failure State, Task result query, or AsyncResult wrapper.

Lifecycle setup exceptions continue to propagate from `Runtime::BuildFrame()`.
Lifecycle accepts an ordinary `void` cleanup callable; cleanup runs inside the framework's non-throwing teardown boundary, so an exception escaping cleanup terminates the process.
The framework does not silently discard cleanup failures.

## Platform boundary

PlatformAdapter retains the UIThreadDispatcher already supplied by every supported production adapter and shares copies with PlatformChannel, ExternalTexture, and TaskScope.
No new platform callback, event protocol, or platform-specific Task implementation is introduced.

Windows continues to use its private window message, Apple platforms use the main dispatch queue, Linux uses its event-loop dispatcher, Web uses the browser event loop, and Android uses its owning HuxerUIView dispatcher.
Delay additionally reuses PlatformAdapter's existing monotonic `Now()` and absolute `RequestFrameAt()` contracts, so every platform receives the same scheduling and cancellation model without another timer boundary.

Future HuxerUI asynchronous APIs may return Task values or provide HuxerUI awaitables that resume through the bound execution.
They do not change TaskScope ownership, State semantics, Lifecycle, or EventBindings.

## Validation

Focused tests cover lazy start, direct Task and factory launch, nested values and exceptions, ignored TaskHandles, individual and scope cancellation, late completion, UI-thread restoration, State writes, compatible recomposition, keyed movement, virtual eviction, unmount, Runtime teardown, invalid handles, deterministic deadlines, zero-duration deferral, and Delay cancellation.
The independent `example_task` demonstrates scope-owned launch, cancellable Delay, explicit Lifecycle cancellation, direct State updates, and fire-and-forget event launch through public API.
