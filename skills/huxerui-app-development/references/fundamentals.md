# Fundamentals

## Components and composition

A component is an ordinary function returning `View`. The application root is supplied directly to `Application` and already owns a scope.

Use `[[huxerui::composable]]` on a reusable function that directly calls composition-bound facilities such as `UseState`, `UseEnvironment`, `UseTheme`, `UseEvents`, `UseTaskScope`, `UseNavigation`, or presentation hooks. `UseApplication()` alone also works in ordinary application-thread functions. Mark a reusable component when it needs an independent recomposition boundary so State changes observed inside it recompose that component instead of its caller. Do not annotate every component for performance by default, and do not annotate the app root.

For CMake projects, define annotated functions in `.cpp`, `.cc`, or `.cxx` sources processed by composable code generation, not only in a header. The mcpp rules also transform module units; follow [cpp-modules-and-mcpp.md](cpp-modules-and-mcpp.md) for that build path.

Do not create a View-producing composable solely to wrap a custom `UseXxx()` hook that returns a non-View value; the hook shares its composable caller's active scope.

## View value semantics

`View` is a lightweight, copyable, copy-on-write declaration value. Copying it shares the declaration until a fluent mutation needs a unique copy. Pass Views into component functions and containers by value without adding defensive `std::move` calls.

Fluent APIs such as `.With`, `.On`, `.OnClick`, `.Key`, and component-specific configuration are rvalue-qualified. A freshly constructed component is already a temporary, so chain directly:

```cpp
return Button("Save")
    .OnClick(save)
    .With(Padding(12.0F));
```

When a named local View must be modified through an rvalue-qualified fluent API, consume it once and do not use the moved-from value afterward:

```cpp
View content = BuildContent();
return std::move(content).With(Padding(16.0F));
```

Prefer constructing that chain directly when practical. Do not move temporary Views, move every child into a container, or write `return std::move(view);`; the last form can prevent return-value optimization. Use `std::move` for a View only when the API actually requires consumption of a named value, not as a blanket performance convention.

## State

- `UseState(initial)` returns copyable `State<T>`.
- Pass `State<T>` directly to APIs that accept the handle or a known `T` or `const T&`. For example, literal `Text::Format("Count: {}", count)` accepts `State` directly. Use `state->member` for read-only field and method access.
- When an API deduces its argument type and needs the stored value, initialize a typed value or reference first, such as `const int value = count;` before `std::clamp(value, 0, 100)`. A declaration such as `auto copy = count;` copies the handle and shares its cell.
- Ordinary writes use `state = value`; `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, or `>>=` when supported by `T`; and prefix/postfix increment or decrement. There is no required setter call.
- Use `state.Update([](T& value) { ... })` for custom mutations. To write the value of one State into another, read it into a typed value before assigning it to the destination.
- Reads subscribe the current composition scope; writes invalidate subscribers.
- State identity depends on scope, source location, and occurrence at that location.
- `StateList<T>` provides observable list operations such as `PushBack`, `Insert`, `Set`, `Erase`, `Move`, `PopBack`, and `Clear`.
- Use stable `View::Key(...)` values when dynamic stateful siblings can be inserted, removed, or reordered.
- `ForEach` expands a range into `Views`; item state still needs stable component scope/key identity when order changes.

Application-owned models may hold `State<T>` directly and share it across windows. Reads and writes belong to the original application's thread; worker code returns ordinary data instead. A background or hidden UI need not draw every update: keep the authoritative accumulated result in the model so resumed or newly composed UI reads the latest state. State is a current value, not an event history or persistence mechanism.

```cpp
auto count = UseState(0);

return Column {
  Text::Format("Count: {}", count),
  Button("Increment").OnClick([count] {
    count += 1;
  }),
};
```

## Events

Bind generic typed events with `view.On<Key>(handler)`. Convenience methods such as `OnClick`, `OnChanged`, and `OnSubmitted` map to typed event keys; do not wire a second callback path for the same action.

`UseEvents()` exposes an emitter for app-side component composition where a component needs to emit its own typed event. Prefer existing built-in component events over a parallel owner callback convention.
Declare the complete handler signature with `Event<Result(Arguments...)>`.
Use `Event<void(Arguments...)>` for notifications.
A value-returning event is a synchronous decision owned by its key, and `Emit<Key>(...)` returns `std::optional<Result>` so the caller must choose the fallback for a missing binding.
Do not add `VoidEvent`, result-event aliases, a second registration method, or a parallel callback convention.

For an existing retained modifier, use `NodeExtension::EmitEvent<Key>(...)` to emit through its owning View's current binding; use an explicitly supplied EventEmitter for an outer component's event. See [typed extension output](node-extensions.md#typed-output) for allowed callback phases and lifetime constraints.

### Keyboard routing

Use `ViewEvents::KeyDown` and `KeyUp` for focused-View behavior after built-in component handling, and return `true` only when the application consumed the event.
Use `ViewEvents::KeyIntercept` when a shortcut or parent policy must run before the focused component; the UiWindow visits the active focus-scope route from root to target and stops at the first true result without bubbling.
Unhandled keys continue to window defaults and then the platform.
Prefer `OnClick` for ordinary control activation because it already includes keyboard and accessibility activation.

`KeyEvent::key` distinguishes portable keys, including main-row and numeric-keypad keys, left/right modifiers, punctuation, international keys, and named function keys, independently of layout-resolved `text`.
Release events have empty text and `repeat == false`; `Key::Unknown` remains dispatchable.
Do not add a shortcut registry, key-chord type, callback property, or a second keyboard route when these typed events are sufficient.

## Environment and theme

`Environment` is typed hierarchical propagation. `UseEnvironment<T>()` resolves the closest provided value or `T::Default()`. `ProvideEnvironment(value, content)` provides a value to descendants. `UseTheme()` is the visual specialization for `ThemeSpec`.

Environment and theme reads belong inside a composable boundary. Do not capture a reference returned by `UseEnvironment` beyond the current composition.

`UseViewportClass()` reads the current compact/medium/expanded classification. The application chooses responsive structure; HuxerUI does not silently replace arbitrary page layouts.

## Application and window ownership

One Runtime owns application services, ordinary timers, event dispatch, and application tasks. Each attached UiWindow owns its composition, Environment, interaction, and presentation services. All attached UIs and application State use the same application thread; opening a window does not start another application event loop.

Install shared models and services with `ApplicationContext::Provide()` in `AppOptions::application_hooks`; install per-window services with `WindowContext::Provide()` in `window_hooks`. Application hooks run once in declaration order, then the service table and factory catalog freeze before windows and queued application work. The same service type cannot be installed at both levels.

`UseService<T>()` acquires an installed instance without allocating a composition slot. Frozen application services can be acquired outside composition, including from workers without a window execution context, but their methods keep their own threading rules. Window services require their original live window context. Retained handles do not rebind to a replacement Runtime or window.

## Lifecycle and tasks

`Lifecycle(setup, dependencies...)` runs setup while the declaring composition lifetime is mounted and runs its cleanup on dependency change or unmount. Dependencies can be values, `State`, or `StateList`.

Choose the scope by the intended cancellation lifetime:

- `UseTaskScope()` is composition-bound; unmount, replacement, virtual-item eviction, or window teardown cancels its children.
- `UseApplicationTaskScope()` is available on the application thread, including during application hooks and outside composition. Its children survive window closure and are canceled at Runtime shutdown. It carries application context, not the calling window's Environment or presentation services.

Launch from event handlers, committed `Lifecycle` setup, or application installation/work, never while composing. Prefer `Launch(factory)` for coroutine lambdas so captures remain owned. The returned `TaskHandle` cancels one child; discarding it leaves the task owned by its scope. Neither scope grants operating-system background execution rights; see [system tasks](#system-tasks) for platform admission.

Use `RunWorker()` for owned synchronous CPU-bound or blocking work that must not run on the UI thread. Do not access State, composition, Views, or UI-affine platform objects from its callable. After `co_await RunWorker(...)`, the continuation is already back on the owning UI thread and may update State directly; do not call `TaskScope::Post()` for that continuation. Cancellation discards queued work or the result of work already running.

Use `WorkerSequence::Run()` when one application-owned blocking resource must process operations strictly one at a time. Retain or copy one WorkerSequence for that resource, accept the mandatory leading `std::stop_token`, and return promptly after cancellation; a canceled active callable keeps the sequence occupied until its cleanup finishes. WorkerSequence shares the RunWorker pool and does not provide worker-thread affinity.

Use `TaskScope::Post()` only when an external thread or callback outside a running HuxerUI Task must enqueue an owned `void` update on the application's thread. Choose the application scope when delivery must survive a window closing; a closed scope discards late posts. Web builds without a worker execution capability report worker execution as unavailable.

Prefer concise duration literals such as `200ms` and `2s` when calling `Delay`.

```cpp
auto tasks = UseTaskScope();

Lifecycle([tasks] {
  auto task = tasks.Launch([]() -> Task<void> {
    co_await Delay(200ms);
    co_await Delay(2s);
  });
  return [task] { task.Cancel(); };
});
```

`UseApplication()` returns the current Runtime's `ApplicationHandle` on the application thread without requiring composition. Its optional `StartupActivation()` exposes the immutable cold-start activation; background-only startup may have none. Register the single handler for subsequent activations through `ApplicationContext::OnActivation()` in `application_hooks`; startup is not replayed. Route both forms into application-owned state. See [activation](navigation-and-window.md#activation) for payloads and platform entry points.

`ApplicationHandle::LifecycleState()` reads and subscribes to the current `ApplicationLifecycleState` during composition. Use `ApplicationContext::OnLifecycleChanged()` for application-lifetime observation or `ApplicationHandle::OnLifecycleChanged()` for a mounted composition observer. Keep navigation or file-opening policy in application state.

`ApplicationHandle::CheckPermissionAsync()`, `RequestPermissionAsync()`, and `OpenPermissionSettingsAsync()` expose typed camera and microphone authorization through the owning Task execution.
Await them from a `TaskScope`; the continuation resumes on its UI thread and may update `State` directly.
Native manifests, Apple usage descriptions, entitlements, request rationale, and final feature policy remain application-shell responsibilities.
Treat `PermissionStatus::Unavailable` as a normal platform-capability result rather than inventing a fallback permission path.

`ApplicationHandle::LocalNotifications()` provides local operating-system notifications. For authorization, native templates, scheduling, and activation data, read [local-notifications.md](local-notifications.md).

## System tasks

Install stable declarations together in `AppOptions::system_tasks`, with unique identifiers matching platform configuration. Choose by the work:

| Work | Declaration | Entry |
| --- | --- | --- |
| Maintenance at a system-selected opportunity | `BackgroundTask` | Await `ScheduleAsync()` from a running Task |
| Finite user-started work, such as an export or one Agent turn | `ContinuedTask` | `Start(instance_id, presentation)` |
| An eligible user-started ongoing session | `OngoingTask` | `Start(presentation)` |

Call `Start()` directly from the eligible user action so platform submission occurs before that action returns. Check `GetCapabilities()` and the submission result; an accepted request may remain Pending until the platform grants execution. Keep submission, stopping, and record removal outside composition.

Handlers are application-owned Tasks using Runtime services on the application thread. Update application-owned `State` directly and let UI read it or `GetState()` during composition. Worker code uses copied stop tokens and returns ordinary data to the handler.

Use `RequestStop()` to withdraw or cooperatively stop work. `WaitAsync()` selects the current request when called; later starts do not retarget it, and canceling the observer leaves the work running. When asynchronous release is needed, catch `TaskStopped` in the handler and await one `context.Cleanup(...)` after the catch block, within the remaining execution budget. Persist business inputs and checkpoints needed after process death; State and services belong to one process.

For platform setup, use the selected SDK's documentation. In a source checkout, see the [system task guide](../../../docs/guide/system-tasks.md) and runnable [system task example](../../../examples/system_tasks/README.md).

## Callback integration

Use `AwaitCallback<T>(start)` for one awaited result from an existing callback API; use `TaskScope::Post()` for external notifications that only update UI state, and `RunWorker()` for synchronous blocking work. The bridge immediately owns its starter and runs it once when the lazy Task starts. Capture owned inputs rather than a temporary facade or borrowed native reference.

The starter receives a copyable `CallbackCompletion<T>` and returns a `std::function<void()>` cancellation action, empty when unavailable. Call `Complete(value)` (`Complete()` for void) or `Fail(non_null_exception_ptr)` from any thread; only the first valid submission wins. Startup exceptions and submitted failures propagate at `co_await`, which resumes through the owning Task's thread and captured context. Do not manually post that continuation.

Cancel through the existing TaskHandle or scope. Pending cancellation invokes the action at most once on the owning thread, including when requested before registration; the action must not throw. Late callbacks are ignored, and Task cancellation never resumes business code with a cancellation result. Once a result is submitted, canceling before queued resumption still suppresses the continuation but does not invoke the action. Keep any longer-lived native resource or presentation cleanup with its owner; the action is not unconditional cleanup.

An adapter must complete or fail a still-live Task when its host closes; simply dropping the callback leaves it suspended. Streams, progress subscriptions, buffering, and presentation queues remain domain-owned. For channel-specific closure behavior, see [platform modules](platform-modules.md).

## Reconciliation

`View` is a transient declaration. Retained mutable behavior belongs in state, controllers, or mounted extensions. Unkeyed siblings reconcile by position. Component functions should not retain raw node, child, or platform pointers across reconciliation.
