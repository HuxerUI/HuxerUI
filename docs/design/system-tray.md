# System Tray and Window Visibility Design

Status: current implementation contract.

This document defines application-level system tray presentation, window visibility commands, platform minimize and close request handling, and orderly application termination. It keeps these responsibilities separate while allowing applications to compose minimize-to-tray and close-to-tray behavior without platform conditionals.

## Goals

- Expose one application-level system tray item through `ApplicationHandle`.
- Reuse the existing menu declaration model for platform tray menus.
- Let applications show, hide, and activate their current window without exposing platform window objects.
- Let applications independently handle platform minimize and close requests.
- Preserve normal platform behavior when a tray host is unavailable.
- Keep availability observable because Linux tray hosts may appear or disappear at runtime.
- Keep operating-system objects and protocols behind PlatformAdapter transports.

## Non-goals

This design does not add multiple tray items, desktop notifications, badges, startup registration, single-instance policy, background services, or a HuxerUI-rendered popup anchored to a tray icon. It does not change macOS Dock visibility or application activation policy. It does not emulate a tray on mobile or Web, and it does not add Linux XEmbed or deprecated GTK tray support.

System tray presentation is not part of `WindowOptions`. A field such as `minimize_to_tray` would combine application presentation, window request policy, capability fallback, and process termination in one startup flag. Those concerns remain independently composable.

## Ownership

The responsibilities are divided as follows:

| Responsibility | Owner |
| --- | --- |
| Tray icon, tooltip, platform menu, and activation | `SystemTrayHandle` |
| Window visibility, activation, and window requests | `WindowHandle` |
| Orderly whole-application termination | `ApplicationHandle` |
| Platform tray object and event conversion | platform system tray transport |
| Request arbitration and committed handlers | shared window service |

`ApplicationHandle` is the public application-shell facade. It groups activation, lifecycle, termination, and system tray capabilities without becoming a generic service locator. HTTP, files, clipboard, presentation layers, and other independent services keep their existing entry points. Internally, the application service owns the focused system tray service, so application-level capabilities share one lifetime and one public entry point.

One Runtime owns one logical application tray presentation. `SystemTrayHandle::Show()` creates or replaces that presentation rather than allocating another tray item. `ApplicationHandle::SystemTray()` captures a private declaration-owner identity from the current composition scope. Repeated calls through that owner update the same presentation, while a competing active owner is invalid. `Hide()` releases only the calling owner's presentation. This prevents an unmounting stale component from removing a replacement owned elsewhere without adding a public registration type.

## Shared menu declarations

The system tray contract directly reuses the existing `MenuItem`, `MenuEntry`, and `MenuSection` declarations from `<huxerui/presentation.h>`. It does not introduce `TrayMenuItem`, move the existing declarations, or add another action model.

A platform tray menu consumes the existing label, optional icon, enabled state, checked state, nested children, section boundary, and item callback. Platform typography, spacing, highlighting, and menu placement remain platform-owned. An icon tint is a presentation hint when a platform menu cannot reproduce it exactly.

The shared system tray service retains callbacks and assigns generation-scoped platform command identifiers. Platform transports receive resolved menu data and report only command identifiers; they never retain application callbacks. Replacing or hiding a presentation invalidates identifiers from its previous generation.

## Public system tray contract

The contract lives with the application-level entry point in `<huxerui/app.h>`:

```cpp
struct SystemTrayOptions {
  StringVariant tooltip;
  std::vector<MenuEntry> menu;
};

class SystemTrayHandle {
public:
  [[nodiscard]] bool IsAvailable() const;

  void Show(ImageVariant icon, SystemTrayOptions options = {}) const;
  void Hide() const;

  template <class... Dependencies>
  void OnActivate(std::function<void()> handler, Dependencies&&... dependencies) const;
};

class ApplicationHandle {
public:
  [[nodiscard]] SystemTrayHandle SystemTray() const;
};
```

The icon is required and uses the existing `ImageVariant` resource model. It must resolve to `ImageAsset`; `VectorAsset` produces `std::invalid_argument` before platform dispatch because platform tray APIs consume raster imagery. The same rule applies to optional platform menu icons. Tooltip and menu labels use `StringVariant` and resolve against the Environment captured by `ApplicationHandle::SystemTray()`. Recomposition can submit `Show()` again when resource configuration changes. An empty or unresolved required icon is invalid caller input.

`Show()` is idempotent application-level presentation. Its first call creates the desired item and later calls atomically replace icon, tooltip, menu declarations, and callbacks. `Hide()` clears the desired presentation and removes any platform item. Runtime shutdown also removes the platform item, so explicit cleanup is not required for process safety.

When the platform is temporarily unavailable, `Show()` retains the desired presentation without pretending that an item is visible. The service presents it when availability returns. `Hide()` while unavailable clears the retained presentation and prevents later creation.

`IsAvailable()` reports whether the current host can display the desired item. Reading it during composition subscribes the current scope to later changes. It remains callable outside composition as a current-value query, which allows a committed window request handler to choose its fallback at request time.

`OnActivate()` follows the same `Lifecycle()`-bound connection model as `ApplicationHandle::OnActivation()`. One Runtime has at most one committed tray activation handler. It is invoked on the Runtime UI thread for the platform's primary tray activation. Context activation displays the declared platform menu. Empty handlers and competing committed handlers are invalid.

## Window visibility

`WindowHandle` gains three direct visibility commands:

```cpp
class WindowHandle {
public:
  void Show() const;
  void Hide() const;
  void Activate() const;

  // Existing placement and close commands remain.
};
```

`Show()` makes the current window visible without intentionally stealing focus. It preserves the current restored, maximized, or minimized placement where the platform permits.

`Hide()` removes the window from visible desktop presentation without destroying Runtime, mounted composition, navigation, or controlled state. Platform lifecycle mapping may consequently report `ApplicationLifecycleState::Background`.

`Activate()` ensures the window is visible, restores it when minimized, and requests foreground activation. Foreground activation is best effort because desktop platforms may reject focus stealing.

These are direct commands and do not pass through minimize or close request handlers. Existing `Minimize()` and `Close()` remain requests and use the request path described below.

## Window request handling

`WindowHandle` gains Lifecycle-bound handlers:

```cpp
class WindowHandle {
public:
  template <class... Dependencies>
  void OnMinimizeRequest(
      std::function<bool()> handler,
      Dependencies&&... dependencies
  ) const;

  template <class... Dependencies>
  void OnCloseRequest(
      std::function<bool()> handler,
      Dependencies&&... dependencies
  ) const;
};
```

The handler returns `true` when application code handled the request and the platform must suppress its default operation. Returning `false` continues normal minimize or close behavior. Calling `window.Hide()` and returning `true` implements tray behavior without adding minimize or close policy fields to `WindowOptions`.

System chrome, framework-rendered caption controls, and application calls to `WindowHandle::Minimize()` or `WindowHandle::Close()` enter the same shared request path. `Show()`, `Hide()`, and `Activate()` bypass it, preventing recursive request handling.

Each request kind permits at most one committed handler for the current Runtime window. Registration, dependency updates, and disconnection follow `Lifecycle()`. Handlers execute synchronously on the UI thread and must not block. A handler failure is reported through the existing Runtime failure path and suppresses the platform default for that request so an exception cannot accidentally discard application state.

The platform adapter submits a platform-neutral request to the shared window service. The service invokes the committed handler and asks the adapter to perform the platform command only when the handler returns `false`. Runtime contains no concrete component or platform checks.

## Application termination

`ApplicationHandle` gains an explicit application command:

```cpp
class ApplicationHandle {
public:
  void Quit() const;
};
```

`Quit()` requests orderly termination of the current desktop application and bypasses the window close handler. The platform exits through its normal event-loop lifecycle, and Runtime teardown disconnects application services and removes the tray item. It never calls `std::exit()` and is not equivalent to `WindowHandle::Close()`.

Platforms that do not own a terminable desktop application may ignore the request. The shared contract does not encode a closed platform list or expose a desktop platform enum.

## Composed behavior

An application declares tray ownership in a component Lifecycle and combines current capability with independent window request handlers:

```cpp
[[huxerui::composable]]
View AppRoot() {
  const auto window = UseWindow();
  const auto application = UseApplication();
  const auto tray = application.SystemTray();

  tray.OnActivate([window] {
    window.Activate();
  });

  Lifecycle([tray, window, application] {
    tray.Show(
        images::application,
        SystemTrayOptions{
            .tooltip = strings::application_name,
            .menu = {
                MenuItem(strings::show_window, [window] {
                  window.Activate();
                }),
                MenuSection{},
                MenuItem(strings::quit, [application] {
                  application.Quit();
                }),
            },
        }
    );

    return [tray] {
      tray.Hide();
    };
  });

  window.OnMinimizeRequest([tray, window] {
    if (!tray.IsAvailable()) {
      return false;
    }
    window.Hide();
    return true;
  });

  window.OnCloseRequest([tray, window] {
    if (!tray.IsAvailable()) {
      return false;
    }
    window.Hide();
    return true;
  });

  return ApplicationContent();
}
```

The same declaration works on unsupported hosts. `IsAvailable()` remains false and the window requests continue through their normal platform behavior, preventing an unreachable hidden application.

## Runtime and transport ownership

The internal application service owns one focused system tray sub-service. `ApplicationHandle::SystemTray()` therefore does not throw merely because a platform lacks a transport. A missing transport produces an unavailable service without exposing another Root Service entry point.

`PlatformAdapter` creates an implementation-only system tray transport. The transport owns platform handles, object registrations, and menu objects. It receives resolved immutable presentation data and reports availability, primary activation, and generation-scoped menu commands. Each desktop transport delivers callbacks from its platform UI event loop before the shared service touches state or invokes application code.

The application-owned system tray service owns:

- Desired application presentation and captured Environment.
- Declaration-owner identity and exclusive presentation ownership.
- Availability observation.
- Resource resolution and presentation generations.
- Menu callback lookup and stale-command rejection.
- Runtime disconnection.

The transport owns:

- Platform tray item creation, update, and removal.
- Platform menu construction and event conversion.
- Host registration, restart, and reconnection behavior.
- Platform image and string conversion.

Runtime disconnection removes the platform item and detaches callbacks before platform teardown. Transports do not retain their adapter owner, so an application handle that outlives Runtime remains inert rather than retaining a platform window.

The service connects transport events only when application code first queries availability, shows a presentation, or declares an activation handler. Constructing a Runtime that never uses `SystemTray()` therefore does not register a platform tray item or watch a Linux tray host.

## Windows mapping

Windows uses [`Shell_NotifyIconW`](https://learn.microsoft.com/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw) with `NIM_ADD`, `NIM_MODIFY`, and `NIM_DELETE`. Every successful add is followed by `NIM_SETVERSION` with `NOTIFYICON_VERSION_4` so mouse and keyboard activation use the current notification-area contract.

The adapter uses the application window for callbacks, builds a platform popup menu, and returns focus to the notification area after menu dismissal. It registers the `TaskbarCreated` message and recreates the desired tray item after Explorer restarts. Platform icons and menus use RAII and are replaced only after their successor has been prepared successfully.

Win32 `WM_CLOSE` and minimize system commands enter the shared request path before the adapter destroys or minimizes the window. `Hide()` uses ordinary Win32 window hiding, while `Activate()` restores and requests foreground activation without promising that Windows will override focus-stealing policy.

## macOS mapping

macOS uses [`NSStatusBar` and `NSStatusItem`](https://developer.apple.com/documentation/appkit/nsstatusitem). The status item button reports primary activation, while context activation displays an `NSMenu` built from the shared menu declaration. All AppKit work remains on the main thread.

`Hide()` orders the application window out without changing the application's Dock icon or activation policy. `Activate()` makes the application active, brings the window forward, and deminiaturizes it as needed. AppKit close and miniaturize decisions enter the shared request path through the window delegate.

Removing the status item balances the object returned by the system status bar and disconnects its target before Runtime teardown.

## Linux mapping

Linux implements the [StatusNotifierItem](https://specifications.freedesktop.org/status-notifier-item/latest-single/) contract over the session D-Bus through `org.kde.StatusNotifierItem` and registers it with `org.kde.StatusNotifierWatcher`. Its platform menu is exported through the `com.canonical.dbusmenu` contract. The implementation uses existing GIO ownership and main-context dispatch without adding a second desktop integration library.

`IsAvailable()` is true only while a watcher with an active status notifier host is present and registration succeeds. The adapter monitors session-bus ownership, unregisters stale objects, and re-registers the desired presentation when the watcher or host returns. Icon pixmaps follow the protocol's ARGB32 network-byte-order representation.

`Activate` reports primary activation and the host presents the exported menu. GTK close requests enter the shared request path. GDK exposes window-manager minimization only as a state transition, so a handled platform minimize transition is immediately undone after the shared request handler hides the window. Framework-originated minimize commands are guarded against duplicate delivery.

An absent host produces an unavailable service. HuxerUI does not fall back to XEmbed or deprecated GTK status icons, which would create different behavior across X11 and Wayland.

## Other platforms

Web, Android, iOS, embedded hosts, and future platforms without a tray transport retain the same shared service with `IsAvailable() == false`. They do not create a synthetic status item or fail application composition. Window visibility commands continue to follow each host's existing window ownership, while `Quit()` remains a request that a non-terminable host may ignore.

## Validation

Focused shared tests cover retained availability, lazy transport connection, presentation replacement, hidden and stale event rejection, lifecycle cleanup, raster enforcement, application quit forwarding, and window request arbitration.

Platform-focused validation should cover Explorer restart and keyboard activation on Windows, AppKit status-item and menu lifecycle on macOS, and watcher loss, host recovery, D-Bus menu activation, and icon byte order on Linux. Manual validation covers platform menu appearance, accessibility, focus restoration, foreground activation restrictions, and the absence of an unreachable hidden-window state.

## Invariants

- System tray presentation is application-owned and not a `WindowOptions` field.
- One Runtime has one logical tray presentation, one active presentation owner, and at most one activation handler.
- Window minimize and close policies remain independent.
- Unsupported or temporarily unavailable tray hosts preserve platform window behavior.
- `Hide()` never destroys Runtime or mounted application state.
- `Quit()` is the explicit whole-application termination path and bypasses close handling.
- Menu callbacks never become platform transport ownership.
- Stale platform command identifiers never invoke callbacks from a replacement presentation.
- Platform objects, handles, and protocol values never enter public or Runtime state.
- Runtime does not branch on a concrete component or desktop platform.
