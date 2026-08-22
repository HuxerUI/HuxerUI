# Application Activation and Lifecycle Design

Status: application activation foundation, observable lifecycle state, and Windows and Android activation mappings implemented; remaining activation mappings staged

This document defines the application-facing boundary for startup activation, subsequent activation, and current application lifecycle state. It covers ownership across the platform application shell, Runtime, composition, files, and navigation without introducing an application session abstraction.

## Goals

- Describe ordinary launch, URL activation, and file activation with platform-neutral typed values.
- Make the startup activation available during the first root composition.
- Deliver subsequent activations in FIFO order on the target Runtime's UI thread.
- Keep application routing, document policy, and window selection application-owned.
- Reuse Root Service, Lifecycle, State, and ordinary Runtime frame scheduling.
- Provide lifecycle state on the same focused application handle without turning it into a general service bag.

## Non-goals

Application activation does not define a route registry, string routes, a process-global navigator, general single-instance policy, multi-window creation, restoration, or platform Intent and URL object types. Browser address-bar history remains owned by `BrowserNavigationStack` rather than becoming a second activation path.

## Public model

The shared activation values are declared in `<huxerui/app.h>`:

```cpp
struct LaunchActivation {
  bool operator==(const LaunchActivation&) const = default;
};

struct UrlActivation {
  std::string url;

  bool operator==(const UrlActivation&) const = default;
};

struct FileActivation {
  std::vector<FileReference> files;
};

using ApplicationActivation = std::variant<
    LaunchActivation,
    UrlActivation,
    FileActivation
>;
```

`LaunchActivation` represents an ordinary launch without an external payload. `UrlActivation` contains a non-empty opaque UTF-8 URL that application code interprets. `FileActivation` contains one or more `FileReference` capability values and never converts platform-granted files into assumed local paths.

The closed variant prevents invalid combinations of unrelated optional fields. Future share or notification inputs require separate reviewed alternatives rather than a generic `PlatformPayload` escape hatch.

## ApplicationHandle

Runtime automatically installs an internal application Root Service. Components access its public lightweight facade through `UseApplication()`:

```cpp
auto application = UseApplication();

UpdateForLifecycle(application.LifecycleState());
application.OnLifecycleChange([](ApplicationLifecycleState state) {
  PersistOrPauseFor(state);
});
HandleActivation(application.StartupActivation());

application.OnActivation([](ApplicationActivation activation) {
  HandleActivation(std::move(activation));
});
```

`ApplicationHandle` deliberately separates four timing contracts:

- `LifecycleState()` is the observable current platform state and may coalesce before recomposition.
- `OnLifecycleChange()` preserves each distinct transition while its declaring component Lifecycle is mounted.
- `StartupActivation()` is immutable for the Runtime lifetime and is available during the first root composition.
- `OnActivation()` receives only activations submitted after that Runtime was created and never replays the startup value.

This distinction lets applications tell cold startup from subsequent activation without adding a flag to every payload. Applications may still route both paths through one policy function when their behavior is identical.

`OnActivation()` is a composition declaration backed by `Lifecycle()`. One Runtime may have one committed activation handler. The handler connection is installed only after a successful frame commit and is removed when its declaration disappears, its owning scope unmounts, a listed dependency changes, or Runtime shuts down.

Captured controlled handles such as `State` and navigation controllers normally remain stable. A callback that captures an ordinary changing value lists it after the handler so the committed connection receives the latest successful value:

```cpp
application.OnActivation(
    [workspace](ApplicationActivation activation) {
      OpenInWorkspace(workspace, std::move(activation));
    },
    workspace
);
```

An empty handler is invalid. A second simultaneously committed handler is also invalid because two independent owners could import the same file or issue conflicting navigation changes.

## Startup activation

The platform application shell resolves the startup input before constructing Runtime:

```text
platform launch input
    -> normalize ApplicationActivation
    -> construct Runtime with startup activation
    -> install the internal application Root Service
    -> compose the application root
    -> read ApplicationHandle::StartupActivation()
    -> commit the first correct frame
```

Runtime defaults to `LaunchActivation` when a platform host does not supply another value. A non-empty URL or file collection is validated before the first composition. The startup value never changes after construction.

## Subsequent activation

A platform host submits a later value through `Runtime::HandleApplicationActivation()` on the owning UI thread:

```text
platform callback
    -> select target Runtime
    -> enqueue activation and request a frame
    -> snapshot the queue length at frame start
    -> invoke the committed application handler in FIFO order
    -> recompose affected State subscribers in that frame
```

The queue retains values while no handler is committed. Connecting a handler requests another frame when queued work exists. The frame processes only the activations present when dispatch starts; a handler that submits another activation leaves it for the next frame, preventing recursive dispatch and starvation. Equal consecutive values are not deduplicated.

Runtime validates, queues, schedules, and delivers activations. It does not parse URLs, open documents, choose a window, inspect NavigationStack, or decide whether an activation is accepted.

## Navigation and files

Application activation is input to application policy rather than a navigation command:

```text
ApplicationActivation
    -> application document or route policy
    -> stable domain identifier
    -> application-owned NavigationPath
    -> NavigationStack
```

An application may replace a root path, push into existing history, update an already-open document, open another window in a future multi-window implementation, or reject the activation. `NavigationPath` remains the only route history source of truth.

`FileReference` may be retained by a document service while importing or establishing a document session. URL-backed, restorable, and equality-comparable routes store stable document identifiers rather than `FileReference` values.

Browser URL changes remain connected directly to the controlled route path through `BrowserNavigationStack`. Web activation is reserved for inputs outside address-bar history, such as a future PWA File Handling launch queue.

## Ownership and future multi-window support

There is no public `ApplicationSession`, session identifier, registry, or target selector. A Runtime already defines the composition and delivery boundary required by the shared implementation.

The platform application shell owns target selection:

```text
platform activation
    -> reuse or create a platform window or embedded target
    -> target Runtime
    -> internal application service
```

Adding multi-window policy later changes the platform shell and window management API, not `ApplicationActivation`, `ApplicationHandle`, or application-owned navigation values.

## Application lifecycle

Application lifecycle state belongs on the same `ApplicationHandle` because it is another platform-owned input to the current application instance:

```cpp
enum class ApplicationLifecycleState {
  Active,
  Inactive,
  Background,
};

ApplicationLifecycleState ApplicationHandle::LifecycleState() const;
void ApplicationHandle::OnLifecycleChange(
    std::function<void(ApplicationLifecycleState)> handler
) const;
```

Lifecycle remains distinct from activation semantics. It exposes a current value that may coalesce and a mounted stream of distinct state transitions, whereas activation is an ordered external-input stream that must not deduplicate. Window focus, minimization, window commands, and title-bar state remain owned by `UseWindow()`.

Reading `LifecycleState()` during composition subscribes only the current scope. `Runtime::UpdateApplicationLifecycleState()` validates platform input, ignores an equal value, stores the latest distinct value, and invalidates subscribed scopes through the existing State dependency mechanism.

`OnLifecycleChange()` uses the same component `Lifecycle()` connection model as `OnActivation()`, with one Runtime-level handler owned by its declaring component Lifecycle rather than a public observer list. While connected, every distinct transition enters a private FIFO before frame delivery. A transition that cannot be presented while the application is backgrounded remains queued and is delivered when frame processing resumes; the coalesced `LifecycleState()` may already contain a later value. Disconnecting the declaring Lifecycle drops its undelivered transitions, because an unmounted component no longer owns side effects. Connecting later begins with future transitions and reads the current value through `LifecycleState()` instead of replaying stale history.

The implemented platform mappings are:

- Windows maps an active restored window to `Active`, deactivation to `Inactive`, and minimization to `Background`.
- Android maps Activity resume to `Active`, pause or foreground transition to `Inactive`, and stop to `Background`; embedded owners explicitly update their `HuxerUIView`.
- iOS maps UIKit active, inactive, and background application callbacks directly.
- macOS maps application activation to `Active` or `Inactive` and application hiding to `Background`.
- Linux maps top-level X11 focus to `Active` or `Inactive` and unmapping to `Background`.
- Web maps a focused visible document to `Active`, a visible unfocused document to `Inactive`, and a hidden document to `Background`.

`Launching`, `Suspended`, and `Terminated` are not planned states. Startup input is represented by `StartupActivation()`, while suspension and termination callbacks cannot be delivered reliably across supported platforms.

## Windows mapping

The Windows application shell parses the process command line before constructing Runtime:

- No payload produces `LaunchActivation`.
- Exactly one argument with a non-drive URL scheme produces `UrlActivation`.
- One or more arguments that all identify existing regular files produce `FileActivation` values backed by Windows `FileReference` capabilities.
- Unknown options, directories, missing files, and mixed inputs remain an ordinary launch rather than being partially interpreted.

For an external URL or file activation, a new process first looks for a window created by the same executable path. If one exists, the process transfers the original UTF-16 arguments through a bounded `WM_COPYDATA` message and exits. The receiving window validates and resolves the payload into fresh URL or `FileReference` values, submits it through `Runtime::HandleApplicationActivation()`, and lets the existing application handler apply policy. The forwarding process restores and activates the target window.

Ordinary launches are never forwarded, so this mechanism does not impose general single-instance behavior. Multiple ordinary instances remain possible, while an external activation targets one existing instance until future multi-window policy provides a more specific selector.

URL protocol and file-association registration remain application or packaging metadata rather than `AppOptions`. `example_application` registers the `huxerui-example` URL protocol under the current Windows user and demonstrates both cold and subsequent browser activation without administrator access.

## Android mapping

`HuxerUIActivity` normalizes its startup `Intent` before attaching `HuxerUIView`, so the Runtime receives the corresponding activation before its first composition. Later `onNewIntent()` values are normalized by the same path and submitted to that View's current Runtime. An Activity launch mode such as `singleTop` is application policy: when Android creates another Activity instance, its Intent becomes that Runtime's startup activation instead of a subsequent activation on an arbitrary existing Runtime.

`ACTION_VIEW` and `ACTION_EDIT` are accepted only when they contain a data URI. `content://` and `file://` values become one-element `FileActivation` values; other schemes become `UrlActivation`. Main-launch, share, malformed, and unsupported Intents remain an ordinary cold launch or are ignored as later input. Share payloads require a separately reviewed activation alternative rather than being projected into files or URLs.

Android document activations keep the provider URI inside `FileReference`. Display name, optional size, MIME type, and write capability are resolved at the platform boundary, while file operations continue through `ContentResolver`. Temporary read and write grants remain governed by the Activity and Android task lifetime; HuxerUI does not persist or widen them. If a sender supplies an unusable capability, later file operations report the existing `FileError` instead of exposing the URI as an application-local path.

The full-screen host installs both timing paths automatically. An embedded owner calls `HuxerUIView.setStartupApplicationIntent()` before attachment and forwards later values through `dispatchApplicationIntent()` on the View's UI thread. Recognized values received before attachment are retained until that View creates its Runtime. This queue exists only at the pre-Runtime platform boundary; once the Runtime exists, the shared application service remains the sole delivery source.

The Android `example_runner` uses `singleTop`. When Gradle selects `example_application`, it enables a dedicated Activity alias that declares the `huxerui-example` scheme plus `content://` and `file://` `ACTION_VIEW` values with any MIME type. This allows cold and subsequent URL activation through a browser or `adb` and makes the example available in another application's system Open with chooser without registering other example runner builds as URL or file handlers.

## Remaining platform mapping stages

Hosts without a completed mapping default to `LaunchActivation` and retain the same Runtime submission boundary. Remaining platform work is staged independently:

- iOS launch or scene URL and document contexts.
- macOS application URL and file-open callbacks.
- Linux command-line file and URL activation.
- Web PWA file handling without duplicating browser History.
- Future OHOS Ability and Want mapping.

Embedded platform views do not consume an enclosing application shell's activation implicitly. Their owner explicitly chooses the target Runtime.

## Implementation ownership

- `<huxerui/app.h>` owns activation values, lifecycle state, `ApplicationHandle`, `UseApplication()`, and the Runtime boundary.
- `src/application.cpp` owns validation, observation, lifecycle-bound connections, FIFO delivery, and handle behavior.
- `src/application_internal.h` is the private contract shared with Runtime.
- `src/runtime.cpp` installs the service and invokes queue delivery before application recomposition.
- Platform application shells own platform input normalization and target selection.

The implementation does not add Runtime subclasses, an Access type, a public service, a callback registry, or a second application state store.

## Invariants

- Startup activation is immutable and visible during the first application composition.
- Subsequent activation never replays the startup value.
- URL and file activations contain non-empty payloads.
- One Runtime has at most one committed activation handler.
- Subsequent activations are delivered in FIFO order on the Runtime UI thread.
- Activations submitted by a handler are deferred to the next frame.
- Platform types never enter the shared activation value.
- Runtime never interprets application URLs, files, routes, or window policy.
- Windows forwards only external URL and file payloads; ordinary launches remain independent.
- Android maps only supported Activity Intents and preserves URI permission boundaries inside `FileReference`.
- Lifecycle updates use one validated current value and invalidate only scopes that observe it.
- A mounted lifecycle handler preserves distinct transitions independently of current-value coalescing.
- NavigationPath remains the only route-history source of truth.
