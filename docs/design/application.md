# Application Activation and Lifecycle Design

Status: application activation foundation and Windows mapping implemented; lifecycle state and remaining platform mapping staged

This document defines the application-facing boundary for startup activation, subsequent activation, and future application lifecycle state. It covers ownership across the platform application shell, Runtime, composition, files, and navigation without introducing an application session abstraction.

## Goals

- Describe ordinary launch, URL activation, and file activation with platform-neutral typed values.
- Make the startup activation available during the first root composition.
- Deliver subsequent activations in FIFO order on the target Runtime's UI thread.
- Keep application routing, document policy, and window selection application-owned.
- Reuse Root Service, Lifecycle, State, and ordinary Runtime frame scheduling.
- Provide one application-level handle that can later expose lifecycle state without becoming a general service bag.

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

HandleActivation(application.StartupActivation());

application.OnActivation([](ApplicationActivation activation) {
  HandleActivation(std::move(activation));
});
```

`ApplicationHandle` deliberately separates two timing contracts:

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

## Future application lifecycle

Application lifecycle state belongs on the same `ApplicationHandle` because it is another platform-owned input to the current application instance. The planned public surface is:

```cpp
enum class ApplicationLifecycleState {
  Active,
  Inactive,
  Background,
};

ApplicationLifecycleState ApplicationHandle::LifecycleState() const;
```

Lifecycle state remains distinct from activation semantics. It is an observable current value that may coalesce, whereas activation is an ordered event stream that must not deduplicate. Window focus, minimization, window commands, and title-bar state remain owned by `UseWindow()`.

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

## Remaining platform mapping stages

Hosts without a completed mapping default to `LaunchActivation` and retain the same Runtime submission boundary. Remaining platform work is staged independently:

- Android Activity Intent startup and `onNewIntent()` delivery.
- iOS launch or scene URL and document contexts.
- macOS application URL and file-open callbacks.
- Linux command-line file and URL activation.
- Web PWA file handling without duplicating browser History.
- Future OHOS Ability and Want mapping.

Embedded platform views do not consume an enclosing application shell's activation implicitly. Their owner explicitly chooses the target Runtime.

## Implementation ownership

- `<huxerui/app.h>` owns activation values, `ApplicationHandle`, `UseApplication()`, and the Runtime boundary.
- `src/application_activation.cpp` owns validation, lifecycle connection, FIFO delivery, and handle behavior.
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
- NavigationPath remains the only route-history source of truth.
