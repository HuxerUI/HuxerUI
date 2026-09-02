# Task and Structured Concurrency Design

## Goals

HuxerUI provides a small C++20 coroutine model for asynchronous work owned by a composition scope.

The public model consists of:

- `Task<T>`, a lazy move-only coroutine result.
- `TaskScope`, a copyable handle to a structured concurrency scope.
- `TaskHandle`, a copyable cancellation handle for one launched task.
- `UseTaskScope()`, a composition function that returns the TaskScope owned by the current `RecomposeScope`.
- `Delay()`, a lazy UI-affine minimum-duration suspension.
- `RunWorker()`, a lazy Task that executes owned synchronous work on the shared worker pool.
- `WorkerSequence`, a copyable ordering identity for cooperative, strictly serialized worker operations.
- `TaskScope::Post()`, a lifecycle-bound handoff from an external thread to the scope's UI thread.

The model does not add `UseTask()`, `UseAsync()`, a generic `AsyncResult<T>`, Task-aware Lifecycle overloads, Task-aware event handlers, public executors, or general thread-switching primitives.
State remains synchronous and does not dispatch writes between threads.

## Public API

The public declarations live in `<huxerui/task.h>` and are re-exported from `<huxerui/huxerui.h>`:

```cpp
template <class T>
class [[nodiscard]] Task;

class WorkerSequence {
public:
  WorkerSequence();

  template <class Function, class... Arguments>
    requires std::invocable<std::decay_t<Function>, std::stop_token, std::decay_t<Arguments>...>
  [[nodiscard]] auto Run(Function&& function, Arguments&&... arguments) const;
};

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

  template <class Callback>
    requires std::constructible_from<std::decay_t<Callback>, Callback&&> &&
             std::invocable<std::decay_t<Callback>&> &&
             std::same_as<std::invoke_result_t<std::decay_t<Callback>&>, void>
  void Post(Callback&& callback) const;
};

TaskScope UseTaskScope();

[[nodiscard]] Task<void> Delay(std::chrono::duration<double> duration);

template <class Function, class... Arguments>
  requires std::constructible_from<std::decay_t<Function>, Function&&> &&
           std::move_constructible<std::decay_t<Function>> &&
           (std::constructible_from<std::decay_t<Arguments>, Arguments&&> && ...) &&
           (std::move_constructible<std::decay_t<Arguments>> && ...) &&
           std::invocable<std::decay_t<Function>, std::decay_t<Arguments>...>
[[nodiscard]] auto RunWorker(Function&& function, Arguments&&... arguments);
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

RunWorker decays and owns its move-constructible callable and arguments when the lazy Task is created.
Its synchronous callable may return `void` or a move-constructible non-reference object, but may not return another Task.
Applications use `std::ref()` when reference semantics are intentional.

WorkerSequence owns only a shared ordering identity.
Its `Run()` operation has the same lazy ownership and result rules as RunWorker, but the callable receives a mandatory leading `std::stop_token`.
Copies join the same sequence, while independently constructed WorkerSequence values may execute concurrently.
Destroying every public handle does not invalidate already-created lazy Tasks because each operation retains its sequence identity.

Post accepts an owned `void` callback, does not create a Task, and does not return a TaskHandle.
It belongs to TaskScope because an external thread has no active TaskExecution from which to obtain a UI thread or composition lifetime.

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

## Worker execution

RunWorker is a global function because the awaiting TaskExecution remains the only owner of cancellation and UI-thread restoration.
It returns a lazy Task and submits no work until that Task is awaited from a running HuxerUI Task.
The callable and arguments are invoked once on one process-wide worker executor shared by every Runtime and TaskScope.

The native executor uses a FIFO queue and a bounded number of C++ worker threads.
Its implementation leaves one reported logical processor available where possible, never creates more than four workers, and falls back to two workers when the processor count is unknown.
The queue is not bounded because blocking the UI thread or rejecting otherwise valid Tasks would require another public overload and failure policy.

RunWorker resumes through the awaiting TaskExecution and UIThreadDispatcher.
A value or `void` completion continues on the owning UI thread, while a worker exception is rethrown from the `co_await` expression on that thread.
The callable must not access State, Views, composition facilities, or UI-affine platform objects.
Platform thread initialization such as COM apartments, JNI attachment, or platform-object lifetime scopes remains the responsibility of code that explicitly uses those platform APIs.

Cancellation skips a queued callable and releases its owned inputs without searching or removing the small queue entry.
A callable that already started may finish, but its value or exception is discarded and retired application code is not resumed.
The queued or running operation state holds TaskExecution weakly and never retains Runtime, TaskScope, or mounted UI state.

WorkerSequence uses the same process-wide executor without reserving or pinning a worker thread.
At most one callable belonging to a sequence is active; its completion or terminal cleanup promotes the next queued operation in submission order.
The current completion is posted to its owning UI dispatcher before the next callable starts, but operations awaiting through different UI dispatchers do not gain a cross-dispatcher continuation-order guarantee.

Canceling a queued sequence operation prevents its callable from starting and allows the following operation to advance when it reaches the head.
Canceling the active operation requests its stop token, detaches its Task continuation, and keeps the sequence occupied until the callable returns and releases its captures.
The callable must poll the token or register a prompt, non-blocking `std::stop_callback`; WorkerSequence cannot interrupt arbitrary C++ code.
An exception is delivered through the usual Task continuation, and posting that continuation releases the sequence for the following operation.

WorkerSequence is intended for application-owned blocking resources that permit one operation at a time, such as a database connection, append-only writer, or request/response device protocol.
It is not an executor, a worker-thread affinity mechanism, a general actor, or a replacement for synchronization inside externally invoked callbacks.

Non-Web local File asynchronous operations reuse this worker executor without calling the public RunWorker API or changing File result and persistence semantics.
Ordinary platform-native asynchronous HTTP, file-picker, and external-file transports keep their existing cancellation and scheduling owners.

## Scoped UI posting

TaskScope::Post is the callback boundary for an external engine or thread that must update HuxerUI state:

```cpp
operation = engine->Start([tasks, result](Value value) mutable {
  tasks.Post([result, value = std::move(value)]() mutable {
    result = std::move(value);
  });
});
```

Post may be called from any thread, always enqueues, and never invokes the callback inline.
Callbacks are ordered per TaskScope by the order in which the scope accepts concurrent submissions; calls from one thread therefore preserve program order.
Each pending callback remains owned by the scope until its identified UI-dispatch entry takes it for execution.

Closing the scope clears pending callbacks before they can execute.
A copied TaskScope may receive a late external completion after unmount, but Post observes the closed state and ignores it without touching Runtime or its dispatcher.
Post does not cancel or disconnect the external operation; Lifecycle cleanup remains responsible for that operation's own lifetime.

Post on an empty TaskScope throws `std::logic_error`, while Post on a closed scope is ignored.
Like Launch, Post is a side effect and throws `std::logic_error` during view composition.
An exception escaping the delivered callback has no asynchronous consumer and terminates the process rather than crossing a platform event-loop boundary.

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
Launch and Post are side effects and are rejected during view composition; application code starts work from committed Lifecycle setup, event callbacks, or other post-composition owners.

Compatible recomposition, a changed returned root View, and keyed movement retain the TaskScope and its running tasks.
Successfully unmounting the scope, evicting a virtual item, or destroying Runtime closes the TaskScope and cancels all remaining tasks.
Virtual item state caching does not retain TaskScope or running coroutine frames.

TaskScope closure is committed with scope retirement rather than performed during speculative reconciliation.
Runtime drains Lifecycle cleanup first and then closes the retired TaskScope, so explicit resource cleanup remains deterministic before the structured cancellation fallback.
Runtime teardown closes all TaskScopes before releasing Root Services and PlatformModule state.

Copies of TaskScope may outlive their RecomposeScope, but a closed TaskScope cannot launch new work or deliver new posts.
Launch on an empty or closed handle throws `std::logic_error`.
Post on an empty handle throws `std::logic_error`, while Post on a closed handle is ignored so a late external completion remains harmless.
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

A suspended RunWorker operation owns its callable until it starts or is canceled.
Canceling its Task skips queued work or discards the outcome of work already running.
A suspended WorkerSequence operation additionally leaves its sequence only after queued cancellation or active callable cleanup, so later operations cannot overlap canceled active work.
Closing TaskScope also discards every UI callback that Post submitted but the UI thread has not taken for execution.

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

RunWorker transports an exception back to its awaiting Task before applying the same rule.
A Post callback has no awaiting Task, so an exception escaping it terminates at the non-throwing UI delivery boundary.

Lifecycle setup exceptions continue to propagate from `Runtime::BuildFrame()`.
Lifecycle accepts an ordinary `void` cleanup callable; cleanup runs inside the framework's non-throwing teardown boundary, so an exception escaping cleanup terminates the process.
The framework does not silently discard cleanup failures.

## Platform boundary

PlatformAdapter retains the UIThreadDispatcher already supplied by every supported production adapter and shares copies with PlatformChannel, ExternalTexture, and TaskScope.
No new platform callback, event protocol, or platform-specific Task implementation is introduced.

Windows continues to use its private window message, Apple platforms use the main dispatch queue, Linux uses its event-loop dispatcher, Web uses the browser event loop, and Android uses its owning HuxerUIView dispatcher.
Delay additionally reuses PlatformAdapter's existing monotonic `Now()` and absolute `RequestFrameAt()` contracts, so every platform receives the same scheduling and cancellation model without another timer boundary.

Native desktop and mobile builds use the shared C++ worker executor.
Web does not silently run RunWorker on its main event loop; without a genuine Worker or pthread execution boundary, awaiting RunWorker throws an explicit unavailable-capability `std::runtime_error`.
TaskScope::Post remains available on Web through the existing UI dispatcher.

RunWorker is process-local work and does not request, extend, or guarantee platform background execution.
HuxerUI does not cancel or pause it merely because ApplicationLifecycleState becomes Background.
The operating system may continue, throttle, suspend, or terminate the process; a completed UI continuation waits until the UI dispatcher can run again, while a retired Task remains canceled.
Platform background services, scheduled jobs, foreground services, and background network sessions are separate explicit capabilities.

Future HuxerUI asynchronous APIs may return Task values or provide HuxerUI awaitables that resume through the bound execution.
They do not change TaskScope ownership, State semantics, Lifecycle, or EventBindings.

## Validation

Focused tests cover lazy start, direct Task and factory launch, nested values and exceptions, ignored TaskHandles, individual and scope cancellation, late completion, UI-thread restoration, State writes, compatible recomposition, keyed movement, virtual eviction, unmount, Runtime teardown, invalid handles, deterministic deadlines, zero-duration deferral, Delay cancellation, worker callable forms and result types, worker concurrency bounds, worker cancellation races, Post ordering, move-only callbacks, external-thread delivery, and closed-scope suppression.
Web coverage verifies that RunWorker fails explicitly without worker support and that Post still uses the browser UI dispatcher.
The independent `example_task` demonstrates scope-owned launch, cancellable Delay, worker execution, scoped UI posting, explicit Lifecycle cancellation, and direct UI-thread State updates through public API.
