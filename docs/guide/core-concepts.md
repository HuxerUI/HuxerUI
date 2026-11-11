# Core Concepts

## Views and components

A component is an ordinary C++ function returning `View`.
Calling it declares the desired interface; persistent state belongs to the mounted runtime tree rather than the transient `View` value.
`View` is a lightweight copy-on-write value, so application code normally passes and returns it by value without explicit moves.

```cpp
View AccountActions() {
  return Row {
    Button("Cancel"),
    Button("Save"),
  }.With(Spacing(8.0F));
}
```

Containers use braces for child declarations.
Generic behavior is applied with `.With(...)`, typed events with `.On<EventKey>(...)`, parent-child metadata with `.LayoutValue<Key>(...)`, and stable identity with `.Key(...)`.
These fluent APIs are rvalue-qualified, but a newly constructed component is already a temporary.
Use `std::move` only when consuming a named View to call such an API; do not move temporary expressions, every container child, or a local return value.

## Composition and state

The application root owns an implicit composition scope.
A reusable function that directly calls a composition-bound `UseXxx()` function must be marked `[[huxerui::composable]]`.

```cpp
[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count: {}", count),
    Button("Increment").OnClick([count] {
      count += 1;
    }),
  };
}
```

`UseState` returns a copyable `State<T>` handle.
Reads subscribe the current scope; writes update the authoritative value and invalidate subscribed scopes.

UI event callbacks may update State directly for synchronous navigation and other immediate UI changes. Recomposition and any resulting node unmount happen on a later frame; wrapping a plain assignment in `TaskScope::Launch()`, `TaskScope::Post()`, or `Delay(0)` adds an unnecessary dispatcher hop. Native services retain their own reentrancy and teardown contracts.

State identity is determined by the current scope, source location, and occurrence at that location.
Dynamic stateful content therefore needs stable scopes or keys when it can be inserted, removed, or reordered.

When an exception escapes composition of the root, a component, a layer, or a virtual item, the built-in hosts replace the affected window's content with an error page. Other windows keep running. Closing and reopening the affected window starts a fresh composition.
`AppOptions::show_composition_error_details` controls whether the page includes the failed operation and exception message. It defaults to enabled in debug builds and disabled in release builds. Hosts log the diagnostic in both modes.
State slot type mismatches include the declaration's file, line, column, and occurrence within the scope.

## Identity and keys

Unkeyed siblings reconcile by position.
Use a stable semantic key for dynamic siblings:

```cpp
return Column {
  ForEach(records, [](const Record& record) {
    return RecordRow(record).Key(record.id);
  }),
};
```

Keys are unique among one parent's children.
Do not use a current index as the key for reorderable stateful data.

## Controlled values

Application-owned values such as text, selection, checked state, progress, and declarative visibility are controlled.
The component emits a requested change and the owner supplies the next value.

```cpp
[[huxerui::composable]]
View ToggleSetting() {
  auto enabled = UseState(false);

  return Switch(enabled).OnChanged([enabled](bool value) {
    enabled = value;
  });
}
```

Transient hover, pressed, animation, momentum, caret, and IME state are retained by mounted framework behavior.

## Events

Components expose typed events and convenience methods such as `OnClick`, `OnChanged`, and `OnSubmitted`.

Custom event identity is its event key type, while `Event` carries the complete handler signature:

```cpp
struct Submitted : Event<void(std::string)> {};

[[huxerui::composable]]
View SearchButton() {
  auto events = UseEvents();
  return Button("Search").OnClick([events] {
    events.Emit<Submitted>("query");
  });
}
```

Parents attach handlers with `.On<Submitted>(...)`.
An event may return a synchronous decision:

```cpp
enum class NavigationDecision {
  Allow,
  Cancel,
};

struct NavigationRequested : Event<NavigationDecision(const NavigationRequest&)> {};
```

`Emit<NavigationRequested>(request)` returns `std::optional<NavigationDecision>`.
An empty result means that no current handler produced a decision; the emitter chooses its explicit fallback.
Void events continue to return `void`.

## Modifiers and component configuration

Property modifiers apply from left to right without creating wrapper nodes:

```cpp
return Text("Profile").With(
    Padding(12.0F),
    Background(Color::White()),
    CornerRadius(8.0F)
);
```

Required component data belongs in its constructor.
Component-only semantics use typed fluent methods, while reusable node behavior uses modifiers.

## Environment and Theme

Environment propagates typed values through the mounted tree.
`UseEnvironment<Value>()` reads the closest value and subscribes the current scope when used during composition.

```cpp
struct ContentScale {
  float value = 1.0F;

  static ContentScale Default() { return {}; }
  bool operator==(const ContentScale&) const = default;
};

View Content() {
  return ProvideEnvironment(ContentScale{2.0F}, ScaledContent());
}
```

Read the closest value with `UseEnvironment<ContentScale>()`.
Theme is the visual specialization of Environment.
Nested themes override only their subtree, and theme changes invalidate the scopes and retained presentation that consume changed values.

## Lifecycle and tasks

`Lifecycle(...)` defines setup and cleanup tied to a mounted composition scope.
Dependency changes run cleanup before the effect is installed again.

```cpp
[[huxerui::composable]]
View UserSession(UserId user_id) {
  Lifecycle([user_id] {
    StartSubscription(user_id);
    return [user_id] {
      StopSubscription(user_id);
    };
  }, user_id);

  return UserContent();
}
```

`Task<T>` represents asynchronous work.
`UseTaskScope()` owns launched work for the current composition scope.
Its tasks are canceled when that scope is replaced or unmounted, and continuations resume on the Runtime thread.
Keeping an inactive page mounted also keeps its composition scope and tasks alive; pause nonessential polling using application-owned visibility state or refresh on demand.
`RunWorker()` owns a synchronous callable and its arguments, executes them on HuxerUI's bounded native worker pool, and resumes the awaiting Task on that Runtime thread.
Worker code must not access State, composition, Views, or UI-affine platform objects.
`WorkerSequence::Run()` uses the same pool but starts only one operation for that sequence at a time; copied sequence handles share ordering, and each callable receives a `std::stop_token` for cooperative cancellation.
`AwaitCallback<T>(start)` adapts one callback operation to a lazy `Task<T>` without using a worker thread.
It immediately owns the starter, which receives a copyable `CallbackCompletion<T>` and returns a `std::function<void()>` cancellation action (empty if unavailable).
The starter runs once on the owning Task's thread; `Complete(value)` and `Fail(exception_ptr)` may be called from any thread, and only the first submission takes effect.
Use `Complete()` for `void`; failures and startup exceptions propagate at `co_await`.
Canceling the Task requests cancellation of pending work on its owning thread and suppresses late callbacks without delivering a cancellation result.
The cancellation action must not throw, and adapters must complete or fail a still-live Task when their host closes.
See [Callback integration](../design/tasks.md#callback-integration) for ownership, cancellation races, and an adapter example.
`TaskScope::Post()` is the lifecycle-bound handoff for an external thread or callback to enqueue a `void` update on the same Runtime thread; pending updates are discarded when the scope closes.
None of these APIs requests or guarantees platform background execution.

## Application state

`Application` declares the process-level root and options.
Use `AppOptions::application_hooks` to compose application installers and `AppOptions::window_hooks` to compose per-window installers:

```cpp
const Application application{
    App,
    {
        .application_hooks = {InstallApplicationServices, RegisterPlatformFactories},
        .window_hooks = {InstallWindowServices, InstallLayers},
    },
};
```

Each `ApplicationHook` receives a borrowed `ApplicationContext` and runs once per Runtime on the application thread, in declaration order, after built-in application services are ready.
Later hooks can acquire services and open application-level platform modules installed by earlier hooks.
Only after every hook succeeds do services and factories freeze and queued application work become eligible to run; window initialization follows application installation.
Each `WindowHook` instead receives `WindowContext` and runs once for each UiWindow before its first composition, including replacement windows.
Both lists may be empty, but an empty callback entry throws `std::invalid_argument` during initialization.
An application hook failure skips remaining application hooks, releases installed application services, and discards queued application work.
A window hook failure skips remaining window hooks and releases services installed for that window.
Do not retain either context after its callback returns.

The root can observe application lifecycle state and receive cold-start or later activation containing URLs, external files, or stable local-notification identifiers.
Navigation and file handling remain application policy; the platform shell only normalizes and delivers activation data.

`UrlActivation::url` is an immutable validated `Uri` from `<huxerui/data.h>`.
Inspect its typed components directly rather than parsing the serialized string again.
`FileActivation` carries `FileReference` capabilities instead of assuming that every platform-opened document has a local path.
`NotificationActivation` carries the stable identifier of the local notification that received primary interaction; application code maps that identifier to its own route or domain state.

`UseApplication()` returns the application-level handle.
Its permission methods query or request access for application features and libraries. The Task continuation may update `State` directly. Supported permissions include location, notifications, contacts, calendar, media library, Bluetooth, camera, and microphone, with availability depending on the operating system.
The application package supplies permission declarations, privacy descriptions, and entitlements. The application chooses when to request access and how to explain it.

```cpp
tasks.Launch([=]() -> Task<void> {
  camera_status = co_await application.RequestPermissionAsync(Permission::Camera);
});
```

`OpenPermissionSettingsAsync(permission)` opens settings for this application's access. Pass `PermissionSettingsTarget::System` to open a system-wide page, such as Android's location switch. Settings may be unavailable, and opening a page does not change the permission. Query again when the application becomes active. Check `Limited` separately when the feature can work with less access, such as approximate location or selected photos.

`ApplicationHandle::LocalNotifications()` returns the focused local-notification handle.
`LocalNotification::data` supplies a resource-free `PlatformPayload` snapshot to native templates and returns through `NotificationActivation::data`. It defaults to Null, is limited to 64 KiB of HUXP encoding, and rejects nested `ExternalTexture`, `FileReference`, and `BufferReference` values. Treat notification data as potentially stale or externally supplied, not as authorization or protected storage.
It exposes independent capability snapshots, notification authorization returning `PermissionStatus`, immediate presentation, durable one-shot scheduling, and idempotent cancellation through stable application-owned identifiers. Use `Permission::Notifications` to query, request, or open settings for application-level authorization. The notification handle also checks the requirements for delivery, such as Android's configured channel. The permission types and notification API live in `<huxerui/system.h>`. `Granted` and `Provisional` permit submission, with provisional authorization retaining limits such as quiet delivery.
The shared API is available on every host.
Registered Windows applications use the system toast service and scheduler, configured Android hosts use `NotificationManager` and an inexact alarm, and iOS and macOS use User Notifications; Linux and Web currently report unavailable capabilities and operations. Windows requires [shell identity registration](packaging.md#windows-notification-registration), supports default presentation and application-provided XML templates, and routes notification clicks through `OnActivation()` even on cold launch.
Android notification interaction may become `StartupActivation()` or `OnActivation()` according to Activity creation, while the independent iOS and macOS delegates always use `OnActivation()` because their native responses arrive after host launch.
`LocalNotification::presentation` defaults to the system UI.
Use `TemplateNotificationPresentation{.identifier = "application.reminder"}` only when the native application shell declares the same stable identifier, and check `capabilities.can_use_templates` before offering that UI.
Windows maps templates to an application-provided `LocalNotificationTemplateProvider` returning ToastGeneric XML, Android maps them to application-provided `RemoteViews` on API 24 or later, iOS maps them to embedded Notification Content Extension categories, and macOS returns `Unavailable` for template requests.
An unknown template returns `Unavailable` rather than silently using the system layout.

On desktop, its `SystemTray()` sub-handle presents one tray item and `Quit()` requests orderly application termination.
Tray declarations reuse `MenuItem`, `MenuEntry`, and `MenuSection`; their `ImageVariant` icons must resolve to raster `ImageAsset` values.
Register primary tray activation once through `SystemTrayHandle::OnActivate(handler)`, normally in an `ApplicationHook`. This application-lifetime registration requires the original application's thread, rejects empty or duplicate handlers, and survives `Show()`, `Hide()`, and window retirement. It does not capture a window context: capture a specific `WindowHandle` or an application-owned service that selects the target window instead of calling `UseWindow()` inside the handler.
`UseWindow()` supplies independent visibility commands and lifecycle-bound minimize and close request handlers, allowing an application to compose minimize-to-tray behavior while preserving the normal window action when no tray host is available.

See [Application Permissions](../design/permissions.md) for status meanings, native declarations, and platform limitations.
See [Local Notifications](../design/local-notifications.md) for notification authorization, identifiers, ordering, scheduling, and target platform mappings.

## Multiple windows

One Runtime serves all windows on the application's UI thread.
`RunApplication()` opens the initial root with `AppOptions::window`.
From an event handler or application task, query `UseApplication().SupportsMultipleWindows()` and call `OpenWindow(content, options)` to create and present another top-level window:

```cpp
auto application = UseApplication();
auto model = UseService<DocumentModel>();
return Button("New window").OnClick([application, model] {
  application.OpenWindow([model] { return DocumentView(model); }, {
      .title = "Document",
      .initial_size = {800.0F, 600.0F},
  });
}).With(Enabled(application.SupportsMultipleWindows()));
```

The returned `WindowHandle` does not own the window; discarding it leaves the window open.
Its `IsOpen()` method is observable during composition and becomes false when closure is accepted, even before native destruction completes.
Hidden and minimized windows remain open.
Retained handles never target a replacement window; commands after closure are ignored.
`Close()` preserves `OnCloseRequest()` interception; a handler returning true consumes the request.
Minimize and close handlers may issue the other command; recursive requests for the same command are consumed.
If a handler closes the window, the pending native operation is suppressed even when the handler returns false.

Each new window has its own root scope, local `UseState`, Environment, resource configuration, services, layers, focus, and frame scheduling.
Application services and application-owned State are shared, so a State write invalidates subscribed scopes in every live window.
The content factory is retained and evaluated later in that window; capture data with the required lifetime.
Neither `WindowOptions` nor an opener's Environment is inherited: pass content data and options explicitly, and provide window-local values within the new content.
Window hooks see the new native window's resource configuration from the beginning; later scale or locale changes update that window independently.

`OpenWindow()` runs on the original application's thread, outside composition and application/window installation.
Empty content and invalid options throw `std::invalid_argument`; invalid thread, lifetime, or timing throws `std::logic_error`; unsupported native creation throws `std::runtime_error`.
A hook exception propagates after partial window cleanup without affecting other windows or causing application exit.
Successful creation does not guarantee that the first frame has rendered.
macOS, Windows, and Linux support this API; Android, iOS, and Web currently report false while retaining their host-managed UI attachments.

Closing the last framework-owned desktop window requests application shutdown by default.
Set `AppOptions::quit_on_last_window_closed = false` to keep application services, tasks, and activation handlers available without windows; an application or tray callback can then open a new window.
Hidden windows count, while initial windowless startup, failed creation, and retirement of external UI attachments do not trigger this policy.
`ApplicationHandle::Quit()` shuts down the entire application without invoking per-window close interception, retiring windows before application services.

[Multiple-window example](../../examples/multi_window/main.cpp) demonstrates shared and local counters, observable closure, and reopening.

## Runtime model

Each UiWindow owns composition, reconciliation, layout, interaction, scrolling, animation, semantics, and retained scene generation. Its Runtime owns application services, dispatch, and ordinary timers.
Platform adapters own platform lifecycle, frame scheduling, event conversion, text services, supported accessibility bridges, and scene rendering.

```text
component declarations
  -> composition and ViewSpec compilation
  -> reconciliation and MountedNode state
  -> layout, interaction, animation, and semantics
  -> immutable frame commit
  -> platform renderer and, where supported, accessibility bridge
```

See [Architecture Design](../design/architecture.md) for internal ownership and invariants.
