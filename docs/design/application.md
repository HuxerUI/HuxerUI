# Application Activation and Lifecycle Design

This document defines the application-facing boundary for startup activation, subsequent activation, and current application lifecycle state. It covers ownership across the platform application shell, Runtime, composition, files, and navigation without introducing an application session abstraction.

## Goals

- Describe ordinary launch, URL activation, file activation, and local-notification activation with platform-neutral typed values.
- Make the startup activation available during the first root composition.
- Deliver subsequent activations in FIFO order on the target Runtime's application thread.
- Keep application routing, document policy, and window selection application-owned.
- Reuse the application service, Lifecycle, State, and application-thread dispatch.
- Provide lifecycle state on the same focused application handle without turning it into a general service bag.

## Non-goals

Application activation does not define a route registry, string routes, a process-global navigator, general single-instance policy, multi-window creation, restoration, or platform Intent and URL object types. Browser address-bar history remains owned by `BrowserNavigationStack` rather than becoming a second activation path.

## Public model

The shared activation values are declared across `<huxerui/app.h>` and `<huxerui/system.h>`:

```cpp
struct LaunchActivation {
  bool operator==(const LaunchActivation&) const = default;
};

struct UrlActivation {
  Uri url;

  bool operator==(const UrlActivation&) const = default;
};

struct FileActivation {
  std::vector<FileReference> files;
};

struct NotificationActivation {
  std::string identifier;
  PlatformPayload data;

  bool operator==(const NotificationActivation&) const = default;
};

using ApplicationActivation = std::variant<LaunchActivation, UrlActivation, FileActivation, NotificationActivation>;
```

`LaunchActivation` represents an ordinary launch without an external payload. `UrlActivation` contains a validated immutable `Uri` that application code interprets. `FileActivation` contains one or more `FileReference` capability values and never converts platform-granted files into assumed local paths. `NotificationActivation` contains the validated stable application identifier and the submitted resource-free `PlatformPayload` data snapshot of the local notification that received primary interaction. Its data may be stale or externally supplied; application code validates business fields and current authorization independently.

The generic syntax and serialization contract belongs to [URI and Local File URI](uri.md). Runtime does not parse the value again or apply route, network, or normalization policy.

The closed variant prevents invalid combinations of unrelated optional fields. Future share inputs require a separately reviewed alternative rather than a generic `PlatformPayload` escape hatch. [Local Notifications](local-notifications.md) follows that rule with one typed notification activation alternative.

## Application hooks and ApplicationHandle

Runtime installs the application service before running `AppOptions::application_hooks`. The hooks register application-lifetime handlers before queued work or any window initializes:

```cpp
void InstallApplication(ApplicationContext& context) {
  if (const auto& startup = UseApplication().StartupActivation()) {
    HandleActivation(*startup);
  }
  context.OnActivation([](ApplicationActivation activation) {
    HandleActivation(std::move(activation));
  });
  context.OnLifecycleChanged([](ApplicationLifecycleState state) {
    PersistOrPauseFor(state);
  });
}

const Application application{App, {.application_hooks = {InstallApplication}}};
```

Components and application-thread callbacks access the service through `UseApplication()`. A composition can read the immutable startup value to derive its initial UI and observe lifecycle transitions for its own mounted lifetime. Imperative startup routing belongs in one-time application setup as shown above, not in recomposition:

```cpp
auto application = UseApplication();

UpdateForLifecycle(application.LifecycleState());
application.OnLifecycleChanged([](ApplicationLifecycleState state) {
  PersistOrPauseFor(state);
});
const auto& startup = application.StartupActivation();
```

`ApplicationHandle::Clipboard()` returns the same shared `Clipboard` owned by this service on every call, without requiring an active composition after the handle has been obtained. This is a per-Runtime capability, not process-global state or a separate Clipboard Root Service. Application-service disconnection makes captured clipboard instances unavailable before the platform Runtime is destroyed. The implementation lives in `src/application/clipboard.cpp`; platform integration and text-editing commands retain the existing `PlatformClipboard` boundary.

`ApplicationHandle::Directories()` exposes the AppDirectories value stored directly in the application service, without a separate Root Service. Platform Runtime implementations initialize their directory roots, and the application service registers those roots with deletion safeguards before publishing them. Hosts without the capability may omit the value, and Directories() then throws std::logic_error. Retained handles and copied File values remain usable after Runtime destruction because they represent paths rather than live platform access. `CurrentDirectory()` independently queries the process working directory at each call, including outside composition and after Runtime destruction; it is not a cached or per-Runtime directory. Directory preparation and path operations remain in `src/io` and the platform file implementations.

The application API separates four timing contracts:

- `LifecycleState()` is the observable current platform state and may coalesce before recomposition.
- `ApplicationHandle::OnLifecycleChanged()` preserves each distinct transition while its declaring component Lifecycle is mounted. `ApplicationContext::OnLifecycleChanged()` observes transitions until Runtime shutdown.
- `StartupActivation()` is immutable for the Runtime lifetime and is available during the first root composition.
- `ApplicationContext::OnActivation()` receives only activations submitted after that Runtime was created and never replays the startup value.

This distinction lets applications tell cold startup from subsequent activation without adding a flag to every payload. Applications may still route both paths through one policy function when their behavior is identical.

`ApplicationContext::OnActivation()` installs one application-lifetime handler per Runtime. It can update application-owned State or services without depending on a mounted window. An empty handler is invalid, and a second handler is rejected because two independent owners could import the same file or issue conflicting navigation changes. The application hook runs once during Runtime initialization; the handler remains connected until shutdown.

## Startup activation

The platform application shell resolves the startup input before constructing Runtime:

```text
platform launch input
    -> normalize ApplicationActivation
    -> construct Runtime with startup activation
    -> install the application service and run application_hooks
    -> compose the application root
    -> read ApplicationHandle::StartupActivation()
    -> commit the first UI frame
```

Runtime defaults to `LaunchActivation` when a platform host does not supply another value. A URL, file collection, or notification identifier selected as startup input is validated before the first composition. The startup value never changes after construction.

Some native systems expose a launch-causing input only through a delegate callback after application launch.
Such input does not retroactively replace `StartupActivation()`; the platform shell queues it and submits it through the subsequent activation path after Runtime construction.
iOS and macOS User Notifications follow this rule, so a notification interaction is observed through `ApplicationContext::OnActivation()` even when it launched the process.

## Subsequent activation

A platform host submits a later value through `Runtime::HandleApplicationActivation()` on the application thread:

```text
platform callback
    -> select target Runtime
    -> enqueue activation and request application dispatch
    -> snapshot the queue length when dispatch begins
    -> invoke the application-lifetime handler in FIFO order
    -> invalidate affected State subscribers for UI recomposition
```

The queue retains values while no handler is installed. Connecting a handler requests dispatch when queued work exists. Each dispatch processes only the activations present when it starts; a handler that submits another activation leaves it for a later dispatch turn, preventing recursive delivery and starvation. Equal consecutive values are not deduplicated. Delivery does not require a mounted UiWindow.

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

An application may replace a root path, push into existing history, update an already-open document, or reject the activation. `NavigationPath` remains the only route history source of truth.

`FileReference` may be retained by a document service while importing or establishing a document session. URL-backed, restorable, and equality-comparable routes store stable document identifiers rather than `FileReference` values.

Browser URL changes remain connected directly to the controlled route path through `BrowserNavigationStack`.

## Runtime ownership

There is no public `ApplicationSession`, session identifier, registry, or target selector. A Runtime defines the application-service and delivery boundary; each attached UiWindow owns its own composition.

The platform application shell owns target selection:

```text
platform activation
    -> reuse or create a platform window or embedded target
    -> target Runtime
    -> internal application service
```

Multi-window target selection is not part of the current application API.

## Application lifecycle

Application lifecycle state belongs on the same `ApplicationHandle` because it is another platform-owned input to the current application instance:

```cpp
enum class ApplicationLifecycleState {
  Active,
  Inactive,
  Background,
};

ApplicationLifecycleState ApplicationHandle::LifecycleState() const;
void ApplicationContext::OnLifecycleChanged(std::function<void(ApplicationLifecycleState)> handler);
template <class... Dependencies>
void ApplicationHandle::OnLifecycleChanged(
    std::function<void(ApplicationLifecycleState)> handler, Dependencies&&... dependencies
) const;
```

Lifecycle remains distinct from activation semantics. It exposes a current value that may coalesce and an ordered stream of distinct state transitions, whereas activation is an ordered external-input stream that must not deduplicate. Window focus, minimization, window commands, and title-bar state remain owned by `UseWindow()`.

Reading `LifecycleState()` during composition subscribes only the current scope. `Runtime::UpdateApplicationLifecycleState()` validates platform input, ignores an equal value, stores the latest distinct value, and invalidates subscribed scopes through the existing State dependency mechanism.

Both `OnLifecycleChanged()` entry points support multiple observers. The `ApplicationContext` form remains connected until Runtime shutdown; the `ApplicationHandle` form uses `Lifecycle()` and disconnects when its declaring composition Lifecycle unmounts or its dependencies change. Each distinct transition records the observers connected when it occurs and enters a private FIFO for application-thread dispatch, even if no window is mounted or the application is backgrounded. Disconnecting an observer drops its undelivered transitions; connecting later receives only future transitions. Read `LifecycleState()` for the current value, which may already reflect a later transition when callbacks run.

The implemented platform mappings are:

- Windows maps an active restored window to `Active`, deactivation to `Inactive`, and minimization to `Background`.
- Android maps Activity resume to `Active`, pause or foreground transition to `Inactive`, and stop to `Background`; embedded owners explicitly update their `HuxerUIView`.
- iOS maps UIKit active, inactive, and background application callbacks directly.
- macOS maps application activation to `Active` or `Inactive` and application hiding to `Background`.
- Linux maps top-level GTK focus to `Active` or `Inactive` and minimization or unmapping to `Background`.
- Web maps a focused visible document to `Active`, a visible unfocused document to `Inactive`, and a hidden document to `Background`.

`Launching`, `Suspended`, and `Terminated` are not planned states. Startup input is represented by `StartupActivation()`, while suspension and termination callbacks cannot be delivered reliably across supported platforms.

## Windows mapping

The Windows application shell parses the process command line before constructing Runtime:

- No payload produces `LaunchActivation`.
- Exactly one argument containing a valid RFC 3986 URI with a non-drive scheme produces `UrlActivation`.
- One or more arguments that all identify existing regular files produce `FileActivation` values backed by Windows `FileReference` capabilities.
- Unknown options, directories, missing files, and mixed inputs remain an ordinary launch rather than being partially interpreted.

For an external URL or file activation, a new process first looks for a window created by the same executable path. If one exists, the process transfers the original UTF-16 arguments through a bounded `WM_COPYDATA` message and exits. The receiving window validates and resolves the payload into fresh URL or `FileReference` values, submits it through `Runtime::HandleApplicationActivation()`, and lets the existing application handler apply policy. The forwarding process restores and activates the target window.

Ordinary launches are never forwarded, so this mechanism does not impose general single-instance behavior. Multiple ordinary instances remain possible, while an external activation targets one existing instance.

URL protocol registration is an explicit application-entry operation through `windows::RegisterUrlScheme()` and `UnregisterUrlScheme()` in `<huxerui/system.h>`, not `AppOptions` or a Runtime service. The Windows application implementation owns current-user registry writes, executable-path quoting, ownership checks, and Shell association-change notification. It reuses native registry and registration-lock helpers with local notifications, but URL schemes require neither a notification identity nor COM/WinRT. The API is also available in the Windows 7 compatibility backend.

The registration belongs to the current executable path and persists beyond process lifetime. The same executable may register repeatedly; another executable, a non-protocol class, or a machine-wide registration is not replaced. Explicit cleanup verifies ownership before removing a current-user key. This is a cooperative ownership check, not a security boundary against other processes running as the same user. User default-app choices remain untouched. File associations and all-user packaging policy remain application-owned.

`example_application` explicitly registers the `huxerui-example` URL protocol before `RunApplication()` and exposes a separate cleanup argument. It demonstrates both cold and subsequent browser activation without administrator access and without static-initialization side effects or example-owned registry helpers.

## Android mapping

`HuxerUIActivity` normalizes its startup `Intent` before attaching `HuxerUIView`, so the Runtime receives the corresponding activation before its first composition. Later `onNewIntent()` values are normalized by the same path and submitted to that View's current Runtime. An Activity launch mode such as `singleTop` is application policy: when Android creates another Activity instance, its Intent becomes that Runtime's startup activation instead of a subsequent activation on an arbitrary existing Runtime.

`ACTION_VIEW` and `ACTION_EDIT` are accepted only when they contain a data URI. `content://` and `file://` values become one-element `FileActivation` values; other schemes become `UrlActivation`. Main-launch, share, malformed, and unsupported Intents remain an ordinary cold launch or are ignored as later input. Share payloads require a separately reviewed activation alternative rather than being projected into files or URLs.

Android document activations keep the provider URI inside `FileReference`. Display name, optional size, MIME type, and write capability are resolved at the platform boundary, while file operations continue through `ContentResolver`. Temporary read and write grants remain governed by the Activity and Android task lifetime; HuxerUI does not persist or widen them. If a sender supplies an unusable capability, later file operations report the existing `IoError` instead of exposing the URI as an application-local path.

The full-screen host installs both timing paths automatically. An embedded owner calls `HuxerUIView.setStartupApplicationIntent()` before attachment and forwards later values through `dispatchApplicationIntent()` on the View's UI thread. Recognized values received before attachment are retained until that View creates its Runtime. This queue exists only at the pre-Runtime platform boundary; once the Runtime exists, the shared application service remains the sole delivery source.

A HuxerUI local-notification content Intent carries a HuxerUI-namespaced action, a package-scoped identifier URI, and the matching stable application identifier.
The platform boundary accepts it only when all three agree.
An Intent used to create a new Activity becomes that Runtime's `NotificationActivation` startup value, while `onNewIntent()` submits the same alternative to the existing Runtime.
The notification PendingIntent requests `singleTop` behavior when the launch Activity is already at the top; broader task and launch-mode policy remains application-owned.
As with URL activation, this shape validation does not authenticate the caller of an exported Activity; applications use the identifier for routing and apply their normal authorization before any privileged action.

The Android `example_runner` uses `singleTop`. When Gradle selects `example_application`, it enables a dedicated Activity alias that declares the `huxerui-example` scheme plus `content://` and `file://` `ACTION_VIEW` values with any MIME type. This allows cold and subsequent URL activation and makes the example available in another application's system Open with chooser without registering other example runner builds as URL or file handlers. The example attempts to preview the first activated file as UTF-8 and reports the existing invalid-encoding error when its contents are not valid UTF-8.

The separate `example_local_notification` owns notification channel creation, the template provider, small icon, and non-exported alarm receiver through its example-specific Android sources and manifest. It demonstrates default reminders and a download-progress template, including the identifier and data returned through cold or subsequent notification activation.

## macOS mapping

The AppKit shell installs its application delegate before finishing native launch. `application:openURLs:` input received during `finishLaunching` is fully normalized before Runtime construction: the first activation becomes `StartupActivation()`, while any remaining ordered activations enter the Runtime queue after construction. The same callback submits later input directly to the current Runtime. Consecutive file URLs form one `FileActivation`; non-file URLs remain separate `UrlActivation` values in their native order. A batch containing an invalid value, directory, or failed capability conversion is rejected without submitting a partial prefix.

The same shell installs its User Notifications delegate before finishing native launch.
Only a primary interaction with a HuxerUI-created local notification becomes `NotificationActivation`.
If the delegate responds before Runtime construction, the shell retains that value as a subsequent-only entry in the ordered platform activation queue and submits it through `Runtime::HandleApplicationActivation()` after construction instead of consuming it as `StartupActivation()`.
Warm and cold notification interactions therefore share the `ApplicationContext::OnActivation()` path.

File activations reuse the security-scoped macOS `FileReference` implementation. The platform decoder retains capabilities and validated `Uri` values only; it does not inspect routes or copy documents into application storage.

The `example_application` bundle declares the `huxerui-example` custom URL scheme and `public.text` document type in its example-specific Info.plist. General applications own these native declarations through their packaging metadata rather than `AppOptions`.

## iOS mapping

The current UIKit shell is application-delegate based and does not declare scenes. When `UIApplicationLaunchOptionsURLKey` indicates a URL launch, `didFinishLaunchingWithOptions` defers Runtime construction until the corresponding `application:openURL:options:` callback supplies the complete open options. The decoded value then becomes the immutable startup activation before first composition. A later callback submits through the same decoder to the existing Runtime. An open-in-place file URL becomes a one-element `FileActivation` backed by the existing security-scoped iOS `FileReference`; when UIKit requires copying before use, the adapter establishes a private read-only temporary snapshot before the callback returns and retains that snapshot through the same `FileReference` contract. Another URL becomes `UrlActivation`.

UIKit installs its User Notifications delegate during `willFinishLaunchingWithOptions`.
Only a primary interaction with a HuxerUI-created local notification becomes `NotificationActivation`.
User Notifications delivers that response after host launch; if Runtime does not yet exist, the adapter retains the value and submits it through `Runtime::HandleApplicationActivation()` immediately after construction.
The immutable startup value remains `LaunchActivation` or the independently decoded URL/file activation, while both warm and cold notification interactions are observed through `ApplicationContext::OnActivation()`.

The launch-options URL alone does not contain `UIApplicationOpenURLOptionsOpenInPlaceKey`, so it is never used to guess document ownership or suppress the callback that carries that information. Equal URLs opened later remain distinct activations as required by the shared queue.

Temporary activation snapshots do not become application documents. Their directory remains private to the platform reference and is removed after the last shared `FileReference` state is released. Applications still explicitly import a selected document into durable storage through the file API, while `CanWrite()` remains false for a copied activation that cannot write back to its source.

The repository iOS runner declares `huxerui-example` and `public.text` so `example_application` can exercise both paths. Generated and consumer applications own their URL schemes, document types, and associated entitlements in the source-controlled Xcode project. Universal Links, `NSUserActivity`, Associated Domains, scene URL contexts, and multi-window target selection are not part of the current adapter.

Hosts without a completed activation mapping deliver `LaunchActivation` and retain the same Runtime submission boundary.
Embedded platform views do not consume an enclosing application shell's activation implicitly; their owner explicitly chooses the target Runtime.

## Future work

- Map Linux desktop activation and Web PWA launch inputs through the same shared activation values.
- Add iOS Universal Links, scene URL contexts, and multi-window target selection when the application shell adopts scenes.
- Define OHOS activation only through the existing platform normalization boundary.

## Implementation ownership

- `<huxerui/app.h>` owns activation values, lifecycle state, `ApplicationHandle`, `UseApplication()`, and the Runtime boundary.
- `src/application/application.cpp` owns validation, observation, handler connections, FIFO delivery, and handle behavior.
- `src/application/application_internal.h` is the private contract shared with Runtime.
- `src/application/application_runtime.cpp` installs the service and schedules application-thread queue delivery independently of UiWindow composition.
- Platform application shells own platform input normalization and target selection.

The application capability does not add a separate session, public service, callback registry, or second application state store.

## Invariants

- Startup activation is immutable and visible during the first application composition.
- Subsequent activation never replays the startup value.
- URL and file activations contain non-empty payloads.
- One Runtime has at most one application-lifetime activation handler.
- Subsequent activations are delivered in FIFO order on the application thread without requiring a UiWindow.
- Activations submitted by a handler are deferred to a later dispatch turn.
- Platform types never enter the shared activation value.
- Runtime never interprets application URLs, files, routes, or window policy.
- Windows forwards only external URL and file payloads; ordinary launches remain independent.
- Android maps only supported Activity Intents, validates notification identifier identity, and preserves URI permission boundaries inside `FileReference`.
- macOS rejects an incomplete native URL batch before submitting any activation from it.
- iOS uses the complete open-URL options before constructing a URL-launched Runtime and never deduplicates later equal URLs.
- iOS and macOS notification delegate responses always use the subsequent activation queue and never replace immutable startup input.
- Lifecycle updates use one validated current value and invalidate only scopes that observe it.
- A mounted lifecycle handler preserves distinct transitions independently of current-value coalescing.
- NavigationPath remains the only route-history source of truth.
