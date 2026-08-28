# Window Chrome Design

This document defines desktop window-chrome ownership, application-defined title-bar content, standard window controls, drag hit testing, and platform fallback behavior.
It complements the mobile-oriented [Window Insets and System Bars Design](window-insets.md) without changing safe-area semantics.

## Goals

- Keep a complete platform-owned title bar when an application wants system window chrome.
- Let an application fully define title-bar content and background without inheriting a partially system-rendered visual surface.
- Preserve platform-appropriate standard window controls in Custom mode.
- Keep system window metadata available to task switching, window management, system menus, and accessibility.
- Reuse mounted layout and pointer geometry for drag and native hit testing.
- Provide convenient per-window commands without exposing platform window objects.
- Keep one ownership model across Windows, macOS, and Linux client-side decorations.

## Non-goals

Custom mode does not provide:

- Runtime switching between System and Custom chrome.
- Application interception of native close requests beyond existing platform behavior.
- Application replacement or removal of the standard macOS traffic lights.
- A second generic window-property or platform-handle abstraction.
- Linux server-side title-bar embedding.
- A Wayland backend.
- Mobile title-bar behavior; mobile system bars remain part of the window-insets contract.

## Ownership model

The system window title, application title-bar content, and standard window controls are separate concerns.

`AppOptions::window.title` remains system window metadata.
The platform uses it for the taskbar, application switcher, window manager, system menu, and accessibility even when no HuxerUI Text displays it.

`WindowTitleBar` is application-defined content.
It may contain a document tab strip, menu, search field, toolbar, status content, a conventional title, or no visible title.

Only two public chrome modes exist:

```cpp
enum class WindowChromeMode {
  System,
  Custom,
};
```

`System` leaves the title-bar surface, standard controls, dragging, and non-client behavior to the platform.
The native client area begins below the system title bar, and `WindowTitleBar` behaves as an ordinary application bar if the application still declares one.

`Custom` transfers title-bar content and background to the application while the framework preserves or supplies standard window controls according to platform convention.
This does not require every platform to render those controls through HuxerUI:

- Windows uses HuxerUI-provided caption controls because native DWM controls cannot be embedded into a fully client-rendered surface with reliable edge alignment.
- macOS keeps the native AppKit traffic lights and reserves their actual bounds for application content.
- Linux client-side decoration support may use HuxerUI controls when the window manager does not provide controls over client content.

The public contract describes responsibility and behavior rather than requiring identical rendering mechanisms.
Applications own their title-bar content; the framework owns the standard window affordances.

There is no Extended or Integrated mode.
A mode that combines application-owned background with platform-owned Windows caption rendering creates ambiguous composition and geometry ownership, and cannot guarantee a visually continuous restored-window edge.

## Public contract

The window contract remains in `<huxerui/window.h>`.

```cpp
enum class WindowChromeMode {
  System,
  Custom,
};

struct WindowTitleBarMetrics {
  float height = 0.0F;
  float left_inset = 0.0F;
  float right_inset = 0.0F;
  bool maximized = false;

  bool operator==(const WindowTitleBarMetrics&) const = default;
};

struct WindowCaptionLabels {
  StringVariant minimize;
  StringVariant toggle_maximize;
  StringVariant close;
};

struct WindowMetrics {
  Size viewport;
  EdgeInsets safe_area;
  std::optional<WindowTitleBarMetrics> title_bar;

  bool operator==(const WindowMetrics&) const = default;
};
```

`AppOptions` groups native-window startup configuration under `WindowOptions`:

```cpp
struct WindowOptions {
  std::string title = "HuxerUI";
  Size initial_size = {520.0F, 360.0F};
  WindowContentMode content_mode = WindowContentMode::SafeArea;
  WindowChromeMode chrome_mode = WindowChromeMode::System;
  float title_bar_height = 40.0F;
  WindowCaptionLabels caption_labels;
};

struct AppOptions {
  WindowOptions window;
  // Application-wide fields...
};
```

`WindowOptions::title_bar_height` is the application's preferred logical height for Custom chrome.
It drives both `WindowTitleBar` layout and framework caption-control geometry, so applications do not configure those heights independently.
Platforms preserve a larger native minimum when their standard controls require one.

`WindowMetrics::title_bar` is the resolved platform geometry available to application layout.
It is present only when application content occupies desktop title-bar space.

`height` is the minimum logical title-bar extent.
`left_inset` and `right_inset` reserve physical regions occupied by framework- or platform-managed standard controls.
They deliberately do not use leading and trailing terminology because the adapter has already resolved system control placement and layout direction.
`maximized` is the platform-resolved placement state used by framework controls to select maximize or restore visuals.

`WindowOptions::caption_labels` makes framework-rendered accessibility labels configurable and resource-aware without
exposing system window objects.
Empty fields resolve `window_minimize`, `window_maximize` or `window_restore` according to platform state, and `window_close` from the built-in `huxerui` resource domain; a non-empty `toggle_maximize` continues to override both maximize and restore states.
The chrome mode, preferred height, and label sources remain stable for one Runtime, while resolved localized labels
refresh with the Runtime resource configuration.

Runtime validates that title-bar values are finite, non-negative, and fit within the submitted viewport.
Platform adapters normalize transient native values before calling `SetWindowMetrics()`.

If a desktop adapter cannot implement Custom chrome correctly, it resolves the request to System and submits no title-bar metrics.
It does not expose a third public mode or approximate system control geometry.
Framework control nodes remain structurally stable but become disabled, semantically hidden, zero-sized, and paint-empty while control geometry is absent.

## WindowTitleBar

`WindowTitleBar` is a horizontal Layout with ordinary child Views:

```cpp
WindowTitleBar {
  MainMenu(),
  EditorTabs().With(Grow(1.0F)),
  SearchField(...),
  IconButton(Icons::settings, ...),
}.With(
    Spacing(6.0F),
    Padding(8.0F),
    Background(theme.colors.surface)
);
```

It does not synthesize an icon, visible title, Back action, or application toolbar action.
Its children retain their own interaction, focus, semantics, and styling.

In Custom mode, framework-managed standard controls occupy the resolved title-bar insets.
On Windows and client-decorated Linux this includes framework-rendered minimize, maximize or restore, and close controls.
On macOS it includes native traffic lights positioned by AppKit.
Applications do not place duplicate standard controls in the normal path.

The layout follows Row sizing and placement with these additions:

- It fills bounded horizontal space.
- Its measured height is at least the resolved title-bar height.
- Children are measured inside the resolved left and right control insets.
- Children are centered vertically in the resolved height.
- Its background covers the complete title-bar bounds, including control insets.
- Ordinary Padding and Spacing remain application-controlled and apply inside the platform reservation.

When `WindowMetrics::title_bar` is absent, the component remains visible as an ordinary horizontal top bar with its natural child height.
This keeps shared application content usable when Custom mode falls back to System or when the same UI runs on mobile or Web.

The component has no dedicated Style type.
Background, height, padding, spacing, opacity, shadow, and child alignment already belong to generic View properties.
Framework-managed controls derive their contrasting glyph color from the composited generic background on the
frontmost intersecting `WindowDragRegion` and its ancestors.
`WindowTitleBar` participates through the marker it already applies rather than through a Runtime component check.
When no intersecting drag region is present, the resolved window backplane is the fallback.
This keeps the control appearance aligned with the surface actually painted beneath it without adding title-bar-only visual properties to View.

## Window commands

`UseWindow()` returns a lightweight handle bound to the current Runtime window, following the established root-service handle pattern used by presentation APIs.
It does not expose `HWND`, `NSWindow`, X11 handles, or another PlatformAdapter.

The initial command surface is:

```cpp
auto window = UseWindow();

window.Minimize();
window.Maximize();
window.Restore();
window.ToggleMaximize();
window.Close();
```

Commands request system window operations rather than mutating shared Runtime state directly.
`Close()` follows the same native close-request path as a standard caption control.

Framework caption controls use this same command boundary and do not call platform implementation helpers directly.
Observable placement, full-screen control, and capability queries are not part of the current window command API.

## Drag regions

The public marker remains:

```cpp
struct WindowDragRegion {
  static const detail::ModifierDescriptor& Descriptor();
};
```

`WindowTitleBar` applies this marker to itself.
Applications may mark another mounted region explicitly.

The platform adapter queries committed geometry through:

```cpp
bool Runtime::IsWindowDragRegion(Point position) const;
```

The query is read-only and uses the mounted tree represented by the currently committed frame.
It follows presentation transforms, child transforms, clipping, scrolling, layer order, and pointer-event policy.

Hit testing visits frontmost descendants before their marked ancestor.
An ordinary pointer target, focusable control, scroll container, or NodeExtension hit wins over a draggable ancestor.
This makes Button, IconButton, TextField, Tabs, selection, and custom pointer content interactive without separate exclusion rectangles.
Disabled controls still reserve their client area.

If no interactive descendant wins, the closest marked region is draggable.
When title-bar metrics are absent, Runtime returns false so a fallback toolbar does not unexpectedly move a system-decorated window.

The marker is a View property rather than a NodeExtension.
It has no retained state, lifecycle, paint output, or event dispatch, and Runtime does not branch on the concrete `WindowTitleBar` type.

## Layout and invalidation

Title-bar metrics are exact window geometry and therefore do not enter Environment.
The internal layout traversal carries them beside constraints and the remaining safe-area value.
`WindowTitleBar` consumes them for application layout, while framework-owned controls use the same committed geometry for placement and hit testing.
Custom Layout implementations that genuinely coordinate with desktop chrome can read the frame-local value through `LayoutContext::TitleBarMetrics()`.

`SetWindowMetrics()` updates viewport, safe area, and title-bar metrics atomically.
A changed title-bar height or inset invalidates layout without recomposing the application tree.
Unchanged metrics schedule no work.

Title-bar metrics are distinct from safe-area insets.
Desktop Custom mode normally reports a zero safe area while still reporting title-bar metrics.
`TopAppBar` remains an application navigation component and never becomes a desktop title bar implicitly.

## Windows mapping

Windows Custom mode makes the complete restored window a client-rendered surface while retaining the native styles required for taskbar behavior, resizing, system commands, and snap integration.
The adapter does not ask DWM to paint caption visuals over the client scene.

HuxerUI supplies the minimize, maximize or restore, and close visuals in the framework window-control layer.
Their geometry is DPI-aware, reaches the visible top and side edges, and is submitted as resolved right title-bar inset.
The resolved control height uses the larger of the DPI-aware system caption-button height and
`AppOptions::window.title_bar_height`.
Control width uses the larger of the DPI-aware system metric and the modern 46-DIP interaction width.
The controls expose button semantics and system window actions through the same Runtime interaction path as other HuxerUI controls.
The adapter submits the current maximized state with the geometry so the middle control changes between maximize and restore visuals without application recomposition.

Native hit testing follows this order:

- Resolve framework caption-control bounds, including `HTMAXBUTTON` behavior required for Windows 11 Snap Layout.
- Resolve the remaining resize edges and corners.
- Convert the remaining screen point to client logical coordinates and query `Runtime::IsWindowDragRegion()`.
- Return `HTCAPTION` for an application drag region and `HTCLIENT` otherwise.

The maximize region still reports `HTMAXBUTTON` so Windows can expose Snap Layout.
Its non-client hover and press messages are bridged into the same Runtime pointer path used by the other framework controls, while HuxerUI remains the single owner of their visual and click state.

The adapter preserves native double-click maximize, `Alt+Space`, the system menu, taskbar commands, DPI transitions, and maximized work-area bounds.
The application does not emulate window movement by accumulating pointer deltas.

Windows 10 and later use this Custom path.
The Windows 7 compatibility build uses the same HuxerUI controls with its existing sequential swap-chain fallback; Windows 11 Snap Layout is naturally unavailable there.

## macOS mapping

macOS Custom mode retains a titled, closable, miniaturizable, and resizable `NSWindow`, enables the full-size content-view style, hides the native title text, and makes the title-bar background transparent.
The standard AppKit traffic lights remain installed, native, and accessible.

The adapter resolves title-bar height from the application preference and AppKit's unobscured content layout, then vertically centers the native traffic-light group without changing its size, horizontal placement, or spacing.
It derives the left content inset from the resulting standard window-button frames converted into HuxerUI view coordinates.
It refreshes those metrics when the window frame, screen, backing scale, or full-screen state changes.
View-size changes synchronously commit the pending Runtime frame before AppKit presents the expanded bounds, while `drawRect:` remains a presentation-only callback.

Drag initiation remains an AppKit window operation selected by `Runtime::IsWindowDragRegion()`.
HuxerUI passes the original mouse-down event to `performWindowDragWithEvent:` and does not move the window by accumulating pointer deltas.
Traffic-light clicks remain native and do not round-trip through Runtime.
Application-invoked `UseWindow()` commands map directly to AppKit minimize, zoom or restore, and close operations; maximize means the macOS zoomed state rather than full screen.

Removing or replacing traffic lights is outside the Custom contract.
It can be considered later as an explicit advanced policy rather than changing the platform default.

System mode retains the ordinary AppKit title bar and submits no title-bar metrics.

## Linux mapping

The Linux backend uses SDL3 and delegates native surface behavior to its active video backend.
Custom mode creates a borderless SDL top-level window while retaining ordinary window-manager ownership, so taskbar presence, snap, and window-manager keybindings keep working.

The framework renders the standard minimize, maximize or restore, and close controls in a `WindowControlsLayout` layer.
Their geometry is submitted as a `WindowTitleBarMetrics.right_inset` of three times 46 DIP, matching the Windows modern interaction width because SDL does not expose a portable native caption-button metric for a custom client area.

Linux metric resolution prefers `AppOptions::window.title_bar_height`, enforces a 32-DIP minimum height, clamps to the viewport, reports zero left inset, caps the right inset at the viewport width, and tracks the maximized state.

Native drag and edge or corner resize use SDL's window hit-test callback.
Hit testing resolves resize edges before `Runtime::IsWindowDragRegion()`, while shared caption controls consume their own pointer input before a drag can begin.

The custom client area uses a fixed 6-DIP resize border and skips resize edges while maximized.

Minimize, maximize, restore, toggle, and close use the corresponding SDL window operations.
SDL window state events drive the caption glyph swap through the shared `maximize_state_changed` path.

System mode is unchanged and submits no title-bar metrics.

These operations remain backend-neutral inside HuxerUI; SDL maps them to the active X11 or Wayland video backend.

## Other platforms

Web, Android, iOS, and future OHOS adapters submit no desktop title-bar metrics.
They ignore the requested desktop chrome mode and preserve `WindowTitleBar` as ordinary application content.
System bars and safe-area behavior continue to follow the separate window-insets contract.

Embedded platform host views also submit no title-bar metrics because their containing system window owns chrome.

## Accessibility and system behavior

Framework-rendered caption controls expose configurable button names, platform actions, and button automation roles.
The maximize control uses a stable toggle action and label while its visual changes with the platform-resolved maximized state.

Platform completion also audits:

- Native resize cursors and edge hit targets.
- Window activation and deactivation visuals.
- Double-click maximize or restore where the platform defines it.
- Keyboard and system-menu window commands.
- High-contrast, reduced-motion, and accessibility settings where applicable.

Activation visuals, high-contrast integration, and exhaustive keyboard and system-menu validation remain follow-up Windows work.

## Platform mapping

The shared API exposes only `WindowChromeMode::System`, `WindowChromeMode::Custom`, `WindowTitleBar`, `WindowDragRegion`, and `UseWindow()`.
The removed Windows Extended experiment has no compatibility alias or retained DWM transparency path.

Windows Custom mode uses the normal opaque renderer, full-client non-client calculation, framework controls, and native resize, drag, system-command, and maximize-button hit testing.
macOS Custom mode uses full-size AppKit content, AppKit traffic lights, converted left-side control geometry, AppKit dragging, and system window commands.
Linux Custom mode uses framework controls in a `WindowControlsLayout` layer, SDL borderless-window policy and commands, SDL hit-tested move and resize operations, and SDL-tracked maximized state.

## Testing

Shared tests cover:

- WindowChromeMode and WindowTitleBarMetrics validation.
- System fallback and resolved Custom layout.
- Left and right standard-control inset placement.
- Ordinary Padding, Spacing, and resolved native height.
- Metric updates without application recomposition.
- Interactive descendants taking precedence over drag ancestors.
- No drag result when title-bar metrics are absent.
- `UseWindow()` commands reaching the native close and placement boundary.
- Caption controls collapsing when resolved metrics disappear.
- Caption glyph invalidation when the composited title-bar background changes.
- Configurable caption-control accessibility labels.

Windows tests isolate caption-button minimum width, narrow-client metric normalization, and maximized work-area bounds.
Manual Windows validation currently covers restored and maximized geometry, resizing, dragging, interactive title-bar controls, and caption hover and click behavior.
High DPI, `Alt+Space`, system-menu behavior, and Windows 11 Snap Layout still require dedicated validation before release.

macOS tests isolate preferred and native title-bar height, traffic-light vertical centering, left-side control reservation, narrow-viewport normalization, and zoomed-state propagation.
Manual macOS validation remains required for system window composition, traffic-light accessibility, dragging, full-screen transitions, and cross-screen behavior before release.
Linux tests isolate metric resolution, preferred-height flooring, viewport clamping, caption-control reservation, and maximized-state propagation.
Manual Linux validation covers SDL decoration removal, drag and edge or corner resize, caption-control interaction, and the maximized glyph swap on the X11 and Wayland SDL video backends.
