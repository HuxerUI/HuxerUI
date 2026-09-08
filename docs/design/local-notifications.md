# Local Notifications

> Status: The shared public API, Windows desktop transport, Android transport, iOS/macOS User Notifications transports, and host-template presentation on Android and iOS are implemented. Linux and Web remain unavailable; Windows and macOS support only default presentation.

This document defines the intended application-facing boundary for local operating-system notifications.
It covers authorization, immediate presentation, one-shot scheduling, stable identifiers, cancellation, and application activation without introducing push delivery, a notification registry, or a fallback presentation channel.

## Goals

- Expose local notifications as one typed application-shell capability.
- Keep notification authorization operations independent of camera and microphone requests while sharing status semantics.
- Use stable application-owned identifiers for replacement and cancellation.
- Deliver notification launches through the existing application activation queue.
- Resume asynchronous application work on the owning Runtime's UI thread.
- Report unsupported or incompletely configured capabilities explicitly.
- Keep native declarations, presentation policy, and product policy in the platform application shell.
- Select optional native presentation templates by stable application-owned identifiers without exposing platform UI objects to shared C++.

## Non-goals

The current contract does not provide remote or push notification registration, device tokens, background data delivery, server integration, repeating schedules, calendar or location triggers, exact alarms, badges, custom sounds, attachments, images, progress notifications, text input actions, or a cross-platform notification layout DSL.

The initial contract also does not define custom notification action buttons.
Those actions depend on Apple categories, Android intent construction, Windows activation registration, and background execution policy that do not yet share a reviewed application lifetime.

HuxerUI does not generate Android channels or manifest declarations, Apple categories or entitlements, Linux desktop entries, or Web service workers. On Windows, application code explicitly supplies and registers its notification identity; CMake and the installer do not generate notification metadata.
It does not use `SystemTrayHandle`, Toast layers, application windows, or in-process timers as notification fallbacks.

## Public model

The shared declarations belong in `<huxerui/system.h>` alongside `Permission` and `PermissionStatus`.
`<huxerui/app.h>` includes that header because notification activation is one closed `ApplicationActivation` alternative.

```cpp
struct LocalNotificationCapabilities {
  bool can_show = false;
  bool can_schedule = false;
  bool can_cancel = false;
  bool can_activate = false;
  bool can_use_templates = false;

  bool operator==(const LocalNotificationCapabilities&) const = default;
};

enum class LocalNotificationOperationStatus {
  Accepted,
  Unauthorized,
  Unavailable,
  Failed,
};

struct DefaultNotificationPresentation {};

struct TemplateNotificationPresentation {
  std::string identifier;
};

using LocalNotificationPresentation =
    std::variant<DefaultNotificationPresentation, TemplateNotificationPresentation>;

struct LocalNotification {
  std::string identifier;
  StringVariant title;
  StringVariant body;
  LocalNotificationPresentation presentation;
  PlatformPayload data;
};

struct NotificationActivation {
  std::string identifier;
  PlatformPayload data;

  bool operator==(const NotificationActivation&) const = default;
};

class LocalNotificationHandle final {
public:
  [[nodiscard]] LocalNotificationCapabilities Capabilities() const;
  [[nodiscard]] Task<PermissionStatus> CheckAuthorizationAsync() const;
  [[nodiscard]] Task<PermissionStatus> RequestAuthorizationAsync() const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> ShowAsync(LocalNotification notification) const;
  [[nodiscard]] Task<LocalNotificationOperationStatus>
  ScheduleAsync(LocalNotification notification, std::chrono::system_clock::time_point delivery_time) const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> CancelAsync(std::string_view identifier) const;
};

class ApplicationHandle final {
public:
  [[nodiscard]] LocalNotificationHandle LocalNotifications() const;
};
```

`Capabilities()` reports the currently connected transport's independently supported operations.
It is not an authorization result and does not predict whether the operating system will ultimately display an accepted notification.
Call it when application UI needs the current transport snapshot.

`LocalNotificationOperationStatus::Accepted` means the native notification service accepted the requested state transition.
It does not guarantee delivery time, foreground presentation, sound, persistence across restart, or user visibility because operating-system policy may suppress or delay presentation.
`Unauthorized` means the host supports the operation but the current notification authorization does not permit it.
`Unavailable` covers an unsupported operation, a missing native declaration or identity, and a host that cannot provide the capability.
`Failed` represents a supported and configured platform operation that failed, including invalid native template content or a native service error.

Empty identifiers, embedded null characters, malformed UTF-8, invalid template identifiers, and an unresolved required title and body are caller errors and throw `std::invalid_argument` before platform dispatch.
At least one of the resolved title or body must be non-empty.
Platform failures and authorization decisions are result values rather than exceptions.

## Ownership and lifetime

Local notifications remain part of the focused application-shell facade:

```text
ApplicationHandle
    -> LocalNotificationHandle
    -> LocalNotificationService
    -> LocalNotificationTransport
    -> native notification API
```

One Runtime owns one private `LocalNotificationService` through its existing `ApplicationService`.
`ApplicationHandle::LocalNotifications()` captures a lightweight handle rather than installing another public Root Service or global registry.
The service shares the existing `UIThreadDispatcher` and callback-to-Task machinery with other asynchronous platform capabilities, but it does not depend on `PermissionController` or share mutable authorization state with it.

A captured handle may outlive its Runtime.
After Runtime destruction, new operations complete with `Unavailable`, pending Task continuations remain detached, and late platform callbacks cannot resume retired application code.
Runtime destruction does not cancel an operating-system-owned notification or schedule that was previously accepted.

The operating system may retain an accepted schedule after the creating Runtime and process have exited.
The platform shell, rather than Runtime, owns any native receiver, delegate, activator, or launch entry point needed to reconstruct a later `NotificationActivation`.
The shared service does not keep a process-global list of scheduled notifications.

## Authorization

Notification authorization is not a `Permission` enum value.
Unlike camera and microphone access, notification systems expose provisional and ephemeral authorization, platform settings that can disable all notification channels, hosts with no prompt API, and platforms where presentation is available without a permission request.

`CheckAuthorizationAsync()` never presents system UI.
`RequestAuthorizationAsync()` may present native UI only when the platform and application shell permit a prompt.
Both operations return the shared `PermissionStatus` through the normal Task path; see [Application Permissions](permissions.md) for its definition.

`NotDetermined` and `Provisional` are returned only when the platform explicitly reports those states.
HuxerUI does not create a history store to infer whether a denied-looking platform result has previously been requested.
`Granted` and `Provisional` authorize submission; provisional authorization retains native presentation limits such as quiet delivery.
Native ephemeral authorization maps to `Granted` while effective. A grant reports current access without exposing an expiration time or promising indefinite authorization.

Authorization queries run independently.
Authorization requests, presentation, scheduling, and cancellation share one ordered queue per Runtime, so native prompts do not overlap and concurrently submitted mutations do not overtake one another.
Every completion is posted through the owning UI dispatcher before an awaiting Task resumes.

## Content and localization

`identifier` is a non-empty stable application-owned UTF-8 value.
It is a logical identifier rather than a native integer, tag, request identifier, or D-Bus notification ID.
A platform transport may encode it into a deterministic native identifier when the host imposes a different representation, but application code never observes that encoding.

`title` and `body` use `StringVariant` so declarations can use packaged localized resources.
The service resolves both values against the Environment captured by `ApplicationHandle::LocalNotifications()` when an operation is submitted.
An accepted schedule contains the resolved strings; it does not relocalize automatically after a later locale change or process restart.

`data` carries a resource-free application snapshot using the existing HUXP encoding, as described in [Application data](#application-data).
Applications use the identifier and validated data for routing, while larger or current domain state remains in application-owned storage.

`presentation` defaults to `DefaultNotificationPresentation`.
`TemplateNotificationPresentation::identifier` is a non-empty stable UTF-8 value owned jointly by application C++ and its native shell configuration.
The identifier selects native presentation code; it does not carry arbitrary values, resources, platform objects, or activation data.

## Presentation, replacement, and cancellation

`ShowAsync()` requests immediate operating-system presentation.
The native shell requests ordinary foreground presentation where the platform exposes that choice, while focus modes, notification settings, system policy, and host UI remain authoritative.
HuxerUI does not mirror an accepted operating-system notification as an in-application Toast.

`TemplateNotificationPresentation` asks the native application shell to render a declared template while HuxerUI continues to own identifier mapping, authorization, scheduling, cancellation, and primary activation.
`can_use_templates` reports whether the current host has any supported template mechanism, not whether one particular identifier exists.
A request for an unknown identifier or a platform that cannot render templates returns `Unavailable` without silently falling back to system presentation.
An immediate Android request whose configured provider accepts the identifier but fails to construct native content returns `Failed`.
An accepted schedule may still remain undelivered if the application changes or its provider cannot reconstruct that template when the alarm fires, consistent with the existing best-effort delivery contract.

An accepted `ShowAsync()` replaces both an existing pending schedule and delivered presentation with the same identifier.
An accepted `ScheduleAsync()` replaces an existing pending schedule with the same identifier atomically from the application's perspective.
Scheduling does not remove an already delivered notification with that identifier.

A transport must not report `Accepted` and silently create a duplicate when it knows that the required replacement operation is unsupported.
It returns `Unavailable` instead.
Linux session-local replacement is an explicitly limited platform mapping because the notification server assigns the replaceable native identifier.

`CancelAsync()` is idempotent.
It cancels a pending schedule and removes a currently delivered presentation with the identifier when those operations are supported by the connected host.
An identifier that is already absent produces `Accepted` because the requested terminal state has been reached.
A partially capable platform advertises `can_cancel` only when it can apply the documented cancellation contract to notifications created by that transport's supported lifetime.

## Application data

`LocalNotification::data` defaults to Null and carries an immutable `PlatformPayload` snapshot to native templates and primary `NotificationActivation::data`. It also works with default presentation; business parameters are not a template-only concern. Data fields do not implicitly enable framework features such as media controls, background actions, or progress notifications.

The shared service encodes data before launching the submission Task. Null, Boolean, Integer, finite Double, String, Bytes, List, and Object values retain their HUXP types. Nested `ExternalTexture`, `FileReference`, and `BufferReference` capabilities are rejected with `std::invalid_argument`: retaining a runtime object cannot make it durable across process termination. The complete encoded data value is limited to 64 KiB, independent of platform notification limits; acceptance or presentation is not guaranteed by satisfying that bound.

Immediate and scheduled notifications use the same resource-free encoding. The scheduled request retains the submitted bytes instead of calling the originating Runtime to reconstruct data. Notification interaction returns that same snapshot rather than current application state. Missing native data maps to Null; malformed, oversized, or resource-bearing bytes reject activation rather than silently dropping data. Application-created activations receive the same durable-data validation.

Application code treats activation data as potentially stale and externally supplied. Do not put secrets, authorization credentials, or large media into notification data. Validate domain fields and access independently, and use application storage for current business state.

## Scheduling

The initial scheduling contract is one-shot and best effort.
`delivery_time` is an absolute `std::chrono::system_clock::time_point`.
It is not a local calendar value and does not change when the device time zone changes.

A delivery time that is not later than the call throws `std::invalid_argument`.
If the time passes after validation but before native submission, the transport requests delivery as soon as the platform permits.
An accepted request may be delayed or coalesced by battery, focus, background, or operating-system policy.

HuxerUI does not request Android exact-alarm access and does not describe `ScheduleAsync()` as exact.
A platform reports `can_schedule` only when its native facility can retain the request after the HuxerUI process terminates.
An in-process timer, worker owned by the current Runtime, or browser page timer does not satisfy this capability.

Persistence across process termination is part of `can_schedule`.
Persistence across device restart, application update, uninstall, clock rollback, or platform data clearing is platform-specific and must be documented without a cross-platform guarantee.

## Application activation

Notification interaction reuses the application activation path instead of adding `OnNotificationResponse()` or a notification event registry:

```cpp
using ApplicationActivation = std::variant<
    LaunchActivation,
    UrlActivation,
    FileActivation,
    NotificationActivation
>;
```

When native launch metadata exposes a notification identifier before Runtime construction, the platform shell may validate it and use `NotificationActivation` as the startup value.
When the native notification system reports interaction only after host launch, the shell queues the validated value and submits it through `Runtime::HandleApplicationActivation()` after Runtime construction.
The independent iOS and macOS User Notifications delegates use this second path even when a notification interaction launched the process, so both platforms observe notification interactions through `OnActivation()` without replacing the immutable `StartupActivation()`.
When a notification targets an existing process, the shell selects the target Runtime and submits the same value through the same method.
The existing startup distinction, FIFO queue, frame boundary, one committed handler, UI-thread execution, and application-owned routing policy remain unchanged.

Only the primary notification activation is part of the initial contract.
If custom action buttons are designed later, their identifiers require a separate reviewed typed alternative or an extension to `NotificationActivation` that cannot represent invalid field combinations.
Background actions that do not create or target a Runtime require a separate application lifetime design and must not invoke composition callbacks from a native receiver.

`can_activate` is true only when the platform shell can route primary notification interaction into the application activation path.
Presentation may still be useful when activation is unavailable, so this capability is independent of `can_show`.

## Platform adapter boundary

`PlatformAdapter` exposes one protected `CreateLocalNotificationTransport()` factory.
A missing transport creates an unavailable service so application code retains one API path on every host.

The private transport owns authorization queries and requests, presentation, scheduling, and cancellation.
It receives resolved UTF-8 content and application identifiers and never retains `StringVariant`, Environment, Runtime, View, or composition callbacks. A native application-shell template provider may be retained as immutable host configuration, independently of Runtime lifetime.
Each asynchronous operation returns a cancellation callback when the native API can cancel the operation itself.

Notification activation does not travel back through the transport.
It enters at the existing platform application shell boundary because cold-start target selection and process activation happen before a Runtime or notification service may exist.
Runtime never branches on Android channels, UIKit or AppKit delegates, Windows activators, Linux D-Bus, Web service workers, or another concrete platform mechanism.

## Target platform mapping

The configured Android adapter and the iOS and macOS adapters install native transports.
The Windows application host owns its COM activator and notification transport; Linux and Web use the unavailable transport while preserving the shared API and activation contract.

### Android

The Android transport uses the notification-specific `POST_NOTIFICATIONS` runtime permission on Android 13 and later.
On earlier supported versions it queries whether application notifications are enabled and does not fabricate a `NotDetermined` state.
Android does not expose a reliable public distinction between a fresh ungranted permission and an earlier denial, so an ungranted Android 13 permission reports `Denied` without a framework-owned request-history store.
`RequestAuthorizationAsync()` uses the Activity-owned permission launcher installed on `HuxerUIView`; before Android 13 it returns the current status without presenting system UI.

The native application shell supplies the notification channel and small-icon resource used by the first shared contract.
On Android 8 or later, the shell creates the channel before constructing `HuxerUIView` and exposes its identifier plus the small icon through application metadata.
On Android 13 or later, the shell also declares `POST_NOTIFICATIONS`.
Durable scheduling additionally requires the shell to declare the framework-provided receiver as non-exported:

```xml
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />

<application ...>
    <meta-data
        android:name="org.huxerui.local_notification.channel_id"
        android:value="application.reminders" />
    <meta-data
        android:name="org.huxerui.local_notification.small_icon"
        android:resource="@drawable/ic_notification" />
    <receiver
        android:name="org.huxerui.HuxerUILocalNotificationReceiver"
        android:exported="false" />
</application>
```

HuxerUI does not merge these declarations into its AAR manifest and does not create the channel or select its importance.
A missing small icon, a missing Android 8 channel, or a missing Android 13 permission declaration makes presentation unavailable.
A missing receiver disables scheduling without disabling immediate presentation or activation.
Cancellation remains available without presentation metadata when Android supplies its notification and alarm services.
The transport reports activation only when the package exposes a launch Activity.

Immediate notifications use the complete stable application identifier as the native notification tag, so submitting the same identifier replaces the delivered notification without integer-hash collisions.
One-shot scheduling uses `AlarmManager.setAndAllowWhileIdle()` with a distinct immutable `PendingIntent` identity derived from the complete identifier.
`ShowAsync()` cancels that identifier's pending alarm before presentation, `ScheduleAsync()` replaces the alarm, and `CancelAsync()` removes both the alarm and the tagged delivered notification.
Exact alarm permissions and exact delivery are outside the contract.
The receiver reloads application metadata and authorization when an alarm fires, then posts through the same notification construction path without creating a Runtime.

On API 24 or later, an application may implement `org.huxerui.HuxerUILocalNotificationLayoutProvider` on its `Application` class.
The provider declares supported template identifiers and creates a fresh `HuxerUILocalNotificationLayout` containing independently optional compact, expanded, and heads-up `RemoteViews`; at least one must be supplied. `new HuxerUILocalNotificationLayout(contentView)` supplies only the normal custom layout. Omitting an expanded custom layout does not prohibit system-controlled expansion.
`HuxerUILocalNotificationContent.getData()` returns the decoded `PlatformPayload`. The alarm and activation Intents retain its HUXP bytes, so template rendering and activation can recover the snapshot after process termination without a resource table or a live Runtime.
HuxerUI retains the `Notification.Builder`, channel, small icon, tag, activation `PendingIntent`, and decorated custom-view style; the provider cannot replace those framework-owned semantics.
Android retains the surrounding notification chrome and may constrain or decorate the custom area according to the device version and system UI.
When a scheduled alarm fires after process death, Android recreates the application, the receiver reloads the provider, and the stored template identifier selects fresh views.
API 23 and hosts without the provider report template presentation as unavailable while retaining ordinary system notification support.

A notification tap starts the package launch Activity with a validated HuxerUI notification Intent.
If Android creates a Runtime for that Intent, its identifier becomes `StartupActivation()`; an `onNewIntent()` delivery to an existing `HuxerUIActivity` becomes a later `OnActivation()` value.
Both paths retain the decoded application data with the identifier.
The content Intent requests `singleTop` delivery when the target Activity is already at the top, but application launch mode and task-stack policy remain shell-owned.
The Intent shape is not caller authentication for an exported Activity; application code uses the stable identifier for routing and performs its normal authorization before any privileged action.

Android schedules may survive process termination but are not promised to survive device restart unless the application shell explicitly owns boot restoration policy.

### iOS

The iOS transport lives in the UIKit backend's application implementation and maps User Notifications authorization to `NotDetermined`, `Granted`, `Denied`, or `Provisional`; native ephemeral authorization maps to `Granted`.
It encodes the shared identifier into an iOS-owned native request namespace and stores the original value in tagged notification metadata for activation routing.
Immediate presentation removes a delivered notification and replaces a pending request with the same identifier, while one-shot scheduling uses a non-repeating time-interval trigger.
Cancellation removes both pending and delivered HuxerUI requests with that identifier without targeting unrelated native notifications.
An authorization-system error reports `Unavailable`; submission while authorization is `NotDetermined` or `Denied` reports `Unauthorized`.

The UIKit shell installs its User Notifications delegate before native launch finishes.
HuxerUI-tagged notifications request banner and notification-list presentation while the application is foregrounded without requesting sound.
Only the default primary action is converted to `NotificationActivation`; dismissals, custom actions, and notifications not created by the iOS transport are ignored.
Notification responses enter the Runtime's subsequent activation queue and are observed through `OnActivation()`.

The iOS transport discovers `UNNotificationExtensionCategory` values from embedded Notification Content Extension bundles.
At least one discovered category enables `can_use_templates`; a matching `TemplateNotificationPresentation::identifier` becomes the request content's `categoryIdentifier`, and the extension owns the expanded notification interface.
The ordinary banner and notification-list presentation remains system-controlled.
The transport does not load extension UI, invent categories, or fall back when the requested category is absent.

The iOS transport stores encoded application data under a framework-owned `userInfo` key. A Content Extension includes `<huxerui/ios/platform_registry.h>` and calls `HUXGetLocalNotificationData(notification.request.content)` in Objective-C, or `localNotificationData(notification.request.content)` in Swift, to obtain `HUXPlatformPayload` without depending on that key. The helper returns a Null payload when data is absent and nil for invalid data. It does not require a Runtime. Primary activation decodes the same bytes.

### macOS

The macOS transport lives in the AppKit backend's application implementation and maps User Notifications authorization to `NotDetermined`, `Granted`, `Denied`, or `Provisional`.
Default notifications retain application data in framework-owned `userInfo` metadata and return it with primary activation, independently of custom layout support.
It owns a distinct macOS native request namespace and tagged activation metadata; it does not call through the iOS transport.
Immediate presentation, one-shot scheduling, cancellation, foreground banner/list presentation, and primary-action activation follow the same shared public contract through the AppKit-owned implementation.
The AppKit delegate submits warm and cold notification interactions through `OnActivation()`.
macOS reports `can_use_templates == false` and returns `Unavailable` for template requests.
Custom actions remain application-owned and outside the current contract.

### Windows

The ordinary Win32 host uses inbox `Windows.UI.Notifications` through C++/WinRT, without requiring MSIX or the Windows App SDK. This transport targets the existing Windows 10 version 1607 or later backend.

Application code supplies a stable AppUserModelID, UTF-8 display name, and non-null COM activator CLSID. Notification identity does not depend on `huxerui_add_app`, bundle metadata, generated executable resources, or install location. Development and installed copies use explicitly distinct App IDs and CLSIDs.

The Windows-only `huxerui::windows` entry points declared under `_WIN32` in `<huxerui/system.h>` own the native registration mechanics in `win32_application.cpp`; the application calls `RegisterLocalNotifications(app_id, display_name, activator_clsid)` before `RunApplication()` on every launch. Successful registration publishes an owned App ID and CLSID as process configuration; the display name is used only while registering. The host copies that identity for its COM class factory and transport. Failed registration preserves the previous configuration, and a process cannot register a different identity until explicitly unregistering its current one. Registration validates ownership, writes current-user `LocalServer32`, `CustomActivator`, and display metadata, and manages only the current-user `<app_id>.lnk` shortcut with `System.AppUserModel.ID` and `System.AppUserModel.ToastActivatorCLSID`. A matching shortcut at that path is preserved; other shortcut locations are not searched or modified. The installer creates ordinary shortcuts without notification properties. Registration rejects another executable's path or machine-owned identity. Ordinary host construction never performs these writes. See [Windows notification registration](../guide/packaging.md#windows-notification-registration).

Persistent registration outlives ordinary exit for cold activation. `UnregisterLocalNotifications(app_id, activator_clsid)` is an explicit pre-host or post-host operation that does not require prior in-process registration. It cancels schedules, clears history, removes only the matching current-user registration and framework shortcut path, and clears a matching process configuration. Installer-owned shortcuts and other users are not modified. Current machine-wide installation does not imply automatic cleanup of every user's notification mappings; that lifecycle remains an application/deployment responsibility, not an all-user registry scan in the notification backend. Native mutations are not transactional; registration attempts roll back newly created entries on failure, while cleanup may partially complete and report an error.

The process host registers one COM class factory before constructing the Runtime. It checks the executable identity and matching registered command before exposing a transport. COM callbacks validate the application ID and bounded versioned arguments, retain the decoded activation in a synchronized inbox, and post through the existing UI-thread dispatcher. The inbox retains callbacks received before a window exists and disconnects before the Runtime is destroyed.

A COM-only launch uses `--huxerui-notification-activate` and pumps COM/window messages for at most ten seconds while awaiting real content; failure exits without creating a dummy application window. The received activation is forwarded to an existing executable-specific window through the existing bounded `WM_COPYDATA` path when possible. Otherwise the new Runtime receives it through `OnActivation()`, not `StartupActivation()`. Ordinary launches remain independent; notification activation does not introduce a general single-instance application policy. Private activation arguments are routing data, never an authorization credential.

The full notification identifier and resource-free HUXP data are Base64-encoded in versioned toast launch arguments, with no auxiliary file store. The complete UTF-8 XML payload must fit 5 KiB, including text and encoding overhead; a payload can satisfy the shared 64 KiB data limit and still return `Failed` here. UTF-8, XML, data, and size validation finish before replacement. The native 16-character tag is a stable hash and the group is framework-owned; both scheduled and delivered candidates are checked against the full embedded identifier before any matching notification is removed. A collision or malformed matching record fails rather than deleting another record.

Immediate presentation removes matching pending and delivered notifications before submitting the replacement. Scheduling removes only the matching pending request and uses the operating system scheduler, which survives process exit; cancellation removes both forms. Native removal/submission calls do not form an OS transaction: a native failure after removal does not restore previous content. Accepted delivery remains subject to Windows settings, focus assistance, and scheduling policy.

Authorization maps `NotificationSetting::Enabled` to `Granted`, user/application disablement to `Denied`, group policy to `Restricted`, and manifest/configuration failure to `Unavailable`. `RequestAuthorizationAsync()` queries this setting without opening a permission dialog or Settings.

The optional fourth registration argument is a `windows::LocalNotificationTemplateProvider`, retained in process configuration under the identity mutex and copied into each host's transport. Successful registration replaces it (omission clears it), failed registration preserves it, and matching unregistration releases the process copy. It is not part of native identity or persisted registry data. Registration changes are pre-host operations only; providers capture durable shell-owned values, not Runtime or composition objects. No provider means `can_use_templates == false`; otherwise the flag advertises the mechanism, not individual identifiers.

Template submission invokes the provider synchronously on the host UI thread with the template identifier, resolved title/body, and decoded resource-free data snapshot. `nullopt` returns `Unavailable`; exceptions or invalid content return `Failed`. Default presentation bypasses the callback. The provider returns a complete UTF-8 ToastGeneric document. A bounded parse prohibits DTDs and external entity resolution, requires a single visual/binding layout, and rejects root launch/activationType/protocolActivationTargetApplicationPfn attributes and actions/input/header elements. These exclusions preserve primary-only activation instead of accepting interactions the inbox cannot route. The framework injects the validated launch envelope through the XML DOM and checks the final serialized UTF-8 size before replacing any scheduled or delivered content. Windows owns the rest of its XML schema and version-specific presentation semantics.

Scheduling builds and submits the completed XML now, never at delivery time; no provider callback, live Runtime, or auxiliary data store is needed for delivery or activation after process exit. Application-owned image resources must outlive native delivery. Layout content does not change ShowAsync replacement semantics: native NotificationData/Update progress binding and custom actions remain outside the current contract. The Windows example adds a ToastGeneric download progress provider (progress requires Windows 10 version 1703 or later), while retaining shared download state and activation data. Its detailed-layout option controls extra content, not Windows expansion policy.

A correctly registered identity that has never submitted a native notification can return `ERROR_NOT_FOUND` from the setting query; this maps to `NotDetermined`, not rejection. Both authorization operations remain read-only. The first `ShowAsync()` submits the requested real content directly. For a first `ScheduleAsync()`, after validating the request, the transport submits a silent initialization toast with `SuppressPopup`, a separate framework group, and a fifteen-second expiry, then immediately removes it and rechecks authorization before installing the schedule. Windows otherwise may accept that first schedule and silently discard it at delivery. Initialization never carries application activation data; a failed removal fails the operation and the expiry limits residual notification-center content. No initialization marker is written to application storage or the registry.

The Windows 7 compatibility build reports every local notification capability and authorization operation as unavailable.
The compatibility build excludes WinRT notification APIs and their additional link dependencies.

### Linux

The freedesktop desktop-notification protocol supports immediate presentation, server-assigned identifiers, session-local replacement and closure, and optional interaction signals.
It does not provide durable one-shot scheduling.

A future Linux transport therefore reports `can_schedule` as false.
It may report presentation and cancellation only while a notification server is available and while it retains the server identifiers assigned during the connected service lifetime.
Because notification servers are not required to emit interaction events, `can_activate` is false unless the active host exposes the necessary capability and the platform shell can complete application activation.

HuxerUI does not add X11, Wayland, desktop-environment, daemon, or autostart policy to shared Runtime code to strengthen these guarantees.

### Web

The initial Web backend reports all local notification capabilities as unavailable.
Persistent browser notifications require an application-owned Service Worker, and the current Web application lifecycle does not define Service Worker registration or notification-click activation.
The standard Notifications API does not provide durable scheduled local delivery.

A future Web implementation requires a separate PWA lifecycle design that supplies the Service Worker and typed activation bridge.
It must not implement `ScheduleAsync()` with `setTimeout`, a Runtime Task, or another page-lifetime timer.

## Validation

Shared tests cover:

- Missing transports and independently unavailable capabilities.
- Input validation before platform dispatch.
- Independent authorization queries and ordered authorization-request and notification-mutation behavior.
- UI-thread Task resumption and late completion after cancellation or Runtime destruction.
- Resolved content, stable identifiers, future delivery times, and transport outcomes reaching the shared Task boundary.
- System and template presentation reaching immediate and scheduled transports without losing the stable template identifier.
- Past-time rejection, cancellation forwarding, and accepted operations not being revoked during Runtime disconnect.
- Startup and subsequent `NotificationActivation` validation, FIFO delivery, and reentrant deferral.

Platform-focused tests cover deterministic native mapping and construction boundaries without presenting operating-system UI.
Windows tests cover provider dispatch and default bypass, unknown templates, XML layout and activation preservation, forbidden activation routes and DTDs, malformed content, and final size limits after envelope injection. Native registration, visible delivery, and scheduled persistence still require host integration validation.
Android instrumentation covers complete identifier-based Intent identity, notification activation normalization, and the immutable provider content/layout values without requiring AndroidX or the native HuxerUI library.
macOS platform tests cover system content, template rejection, authorization conversion, foreground presentation, and activation filtering.
iOS Content Extension discovery and category assignment remain iOS build and native integration validation boundaries.
Actual presentation, replacement, cancellation, and persistence remain native integration behavior validated on the corresponding host or device.
Clock-sensitive tests use injected transport time or fixed future instants rather than wall-clock sleeps.

## Remaining platform work

- Add Linux immediate presentation only after its session-local limitations are represented by the capability contract.
- Leave Web unavailable until a PWA and Service Worker activation contract is approved.

## Invariants

- Notification authorization never becomes a generic `Permission` value.
- One public handle owns authorization, presentation, scheduling, and cancellation without a second registry.
- Stable identifiers are application values and native identifier representations do not escape their transports.
- Template identifiers select application-shell presentation only; unknown or unsupported templates never degrade silently to system presentation.
- Accepted scheduling never relies on Runtime, process, or page lifetime.
- Notification interaction uses `ApplicationActivation`; it does not introduce a parallel response callback.
- Native declarations, identity, channels, categories, service workers, and final product policy remain application-owned.
- Unsupported scheduling or activation is explicit and never emulated through System Tray, Toast, or in-process timers.
- Platform callbacks resume Tasks or submit activations only through the owning UI-thread boundary.
- Runtime and shared service code contain no concrete platform branches.
