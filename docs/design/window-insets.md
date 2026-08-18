# Window Insets and System Bars Design

Status: implemented for the shared Runtime, Android, and iOS; OHOS remains deferred

This document defines the platform-neutral contract for window metrics, safe-area layout, edge-to-edge content, system-bar backgrounds, and system-bar foreground brightness.
It covers Android, iOS, and the future OHOS adapter while preserving zero-inset desktop behavior and a compatible Web extension point.

## Goals

- Keep ordinary application content safe without requiring every page to declare a SafeArea wrapper.
- Let backgrounds, top app bars, bottom navigation, media, and custom drawing extend into system-bar regions intentionally.
- Give Flat, Material, and custom themes coherent status-bar and navigation-bar defaults.
- Allow a visible page or edge component to override system-bar appearance without imperative native calls.
- Update viewport size and safe-area geometry atomically.
- Apply changing insets during layout without recomposing the application tree.
- Keep IME occlusion, display cutouts, system bars, and system gestures as distinct concepts.
- Preserve one shared contract for full application windows and embedded PlatformViews.

## Non-goals

The initial implementation does not provide:

- A `fullscreen` Boolean that combines layout, system-bar visibility, and immersive input behavior.
- Public Android, iOS, or OHOS inset categories.
- Arbitrary native status-bar text colors on platforms that expose only light and dark foreground modes.
- Dynamic page-by-page changes to the root `WindowContentMode`.
- System-bar visibility control.
- System-gesture exclusion regions.
- Desktop custom title bars or window chrome.
- A public SafeArea container View.

System-bar visibility and desktop window chrome require separate contracts because they do not change the meaning of safe-area layout.

## Terminology

`SafeArea` mode keeps ordinary application content between system obstructions while HuxerUI still owns the complete native drawing surface.

`EdgeToEdge` mode gives application content the complete logical viewport and lets edge-aware components consume the insets for the regions they own.

Neither mode hides the status bar or navigation area.
Hiding native system UI is an immersive-visibility policy and remains separate.

`safe_area` is the resolved four-edge distance required to keep important drawing and interaction away from system bars, display cutouts, and equivalent platform obstructions.
It does not include the software keyboard or system-gesture exclusion policy.

## Public contract

The window contract belongs in `<huxerui/window.h>`.
The umbrella header includes it.

```cpp
enum class WindowContentMode {
  SafeArea,
  EdgeToEdge,
};

enum class SystemBarContentBrightness {
  Automatic,
  Light,
  Dark,
};

struct WindowMetrics {
  Size viewport;
  EdgeInsets safe_area;

  bool operator==(const WindowMetrics&) const = default;
};

struct SystemBarsAppearance {
  Color status_bar_background = Color::White();
  Color navigation_bar_background = Color::White();
  SystemBarContentBrightness status_bar_content = SystemBarContentBrightness::Automatic;
  SystemBarContentBrightness navigation_bar_content = SystemBarContentBrightness::Automatic;

  static SystemBarsAppearance Default();
  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const SystemBarsAppearance&) const = default;
};

struct SafeAreaPadding {
  bool top = true;
  bool right = true;
  bool bottom = true;
  bool left = true;

  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const SafeAreaPadding&) const = default;
};
```

`Light` means light system text and icons.
`Dark` means dark system text and icons.
The names describe the foreground, not the background or the platform color scheme.

`SystemBarsAppearance` is both a Theme value and a property modifier.
A Theme supplies the complete fallback for its subtree, while a modifier declares a more specific appearance for the window edge covered by its View.

`AppOptions` adds one stable window-level policy:

```cpp
struct AppOptions {
  // Existing fields...
  WindowOptions window;
};
```

Platform integration replaces separate viewport updates with an atomic metric update:

```cpp
void Runtime::SetWindowMetrics(WindowMetrics metrics);
```

`SetViewport()` has been removed.
Current platform adapters, tests, and integration documentation use the atomic contract without a compatibility alias.

## Why WindowContentMode is window-level

The content mode establishes the application root's layout coordinate region.
Changing it during a page transition would move both pages between different root constraints while transforms, Popup anchors, text-input geometry, and Layer placement are being resolved.

HuxerUI therefore keeps `WindowContentMode` stable for the lifetime of one Runtime.
An application that contains any immersive page selects `EdgeToEdge` and lets ordinary structural components or `SafeAreaPadding` keep its other pages safe.
Pages may still change `SystemBarsAppearance` without changing root geometry.

This separation keeps navigation transitions deterministic and avoids a window-configuration controller or composition effect solely for geometry switching.

## WindowMetrics semantics

`WindowMetrics::viewport` is the current logical HuxerUI viewport in device-independent units.
The existing native IME-avoidance policy may reduce its height before the metrics reach Runtime.

`WindowMetrics::safe_area` is relative to that current viewport.
When the IME covers the native bottom system area, the effective bottom safe inset is zero rather than being subtracted a second time.

Each platform adapter submits viewport and safe-area changes together on its UI thread.
Runtime validates that the viewport and every inset are finite and non-negative.
Platform adapters normalize transient or unsupported platform values before calling Runtime.

Desktop adapters pass zero safe-area insets.
An embedded host that is already constrained to a native safe area passes zero rather than reporting and consuming the same insets again.
The preferred full-window integration passes the complete host bounds and the real safe-area values.

## Layout-time propagation

Exact window geometry is not a public Environment value.
Publishing continuously changing dimensions through composition would conflict with the existing rule that exact dimensions belong to layout constraints while only coarse `ViewportClass` changes recompose structure.

Runtime seeds each measure traversal with the remaining safe-area insets.
`LayoutContext` exposes the remaining value to custom layouts:

```cpp
EdgeInsets LayoutContext::SafeAreaInsets() const noexcept;
```

`SafeAreaPadding` selects edges from the remaining value, adds them to the View's ordinary Padding, and passes zero for the consumed edges to descendants.
An unselected edge remains available to descendants.

Ordinary Padding and safe-area padding are additive:

```cpp
Column {
  Content(),
}.With(
    Padding(16.0F),
    SafeAreaPadding{},
    Background(theme.colors.surface)
);
```

The View's background covers both the explicit and safe-area padding.
Only its content is inset.

Nested consumers do not apply the same inset twice because children receive the remaining value after their ancestor's consumption.
Measurement caching includes the remaining safe-area value in addition to parent Constraints.
An invalidated ancestor may therefore reuse a clean child only when both its constraints and inherited inset context match.

## SafeArea mode

The Runtime root and LayerStack retain complete viewport bounds.
Only the application child uses the safe content rectangle.

Runtime measures the application child with constraints deflated by `safe_area`, places it at `{safe_area.left, safe_area.top}`, and gives its subtree zero remaining safe-area insets.
The application root's local coordinate system still begins at zero inside that safe rectangle.

System-bar regions are painted by a Runtime-owned backplane beneath application and Layer content.
The backplane uses the resolved `SystemBarsAppearance`, so status and navigation regions remain themed even though ordinary application content does not enter them.

This mode deliberately does not ask Android, iOS, or OHOS to crop the native HuxerUI surface.
Keeping the complete platform surface gives every platform the same rendering and system-bar-style ownership, including Android versions that enforce edge-to-edge system windows.

The default mode is `SafeArea` because a bare application root remains usable without any extra declaration.

## EdgeToEdge mode

Runtime measures and places the application child against the complete viewport.
The full safe-area value enters the application layout traversal.

Edge-aware structural components consume only the edges they own:

- TopAppBar consumes top, left, and right insets for its content while its background covers the complete resulting bounds.
- NavigationBar consumes bottom, left, and right insets for its items while its background reaches the physical bottom edge.
- DrawerLayout extends drawer surfaces to the relevant window edges and keeps drawer content away from top, bottom, and side obstructions.
- A standalone NavigationPane does not assume that it touches a window edge; DrawerLayout owns its inset behavior when it is used as drawer content.

The themed `height` of TopAppBar and NavigationBar continues to describe their content region.
Their measured edge-to-edge height adds the consumed system inset.

An ordinary application shell therefore needs no explicit SafeArea declaration:

```cpp
Column {
  TopAppBar("Settings"),
  Content().With(Grow{}),
  NavigationBar(items, selected),
}
```

Custom full-window drawing keeps its visual backplane edge to edge and protects only the interactive content that needs it:

```cpp
return Stack {
  Canvas(DrawScene),
  Controls().With(SafeAreaPadding{}),
};
```

The modifier can select a subset without adding a separate edge enum:

```cpp
Column {
  Controls(),
}.With(
    SafeAreaPadding{
        .top = false,
        .right = false,
        .left = false,
    }
);
```

## Theme ownership

Flat and Material Theme definitions provide complete `SystemBarsAppearance` values.
Theme defaults are visual policy rather than platform configuration.

The intended defaults are:

| Theme | Status background | Navigation background |
|---|---|---|
| Flat Light | `surface` | `surface` |
| Flat Dark | dark `surface` | dark `surface` |
| Material Light | `surface` | `surface_container` |
| Material Dark | dark `surface` | dark `surface_container` |

Both foreground fields default to `Automatic`.
Theme definitions may replace the complete value through their existing typed override mechanism.

```cpp
auto definition = MaterialThemeDefinition();
definition.Set(SystemBarsAppearance{
  .status_bar_background = theme.colors.primary,
  .navigation_bar_background = theme.colors.surface_container,
});

return Theme(std::move(definition), AppContent);
```

TopAppBar contributes its resolved `TopAppBarStyle::background` to the status-bar region.
NavigationBar contributes its resolved `NavigationBarStyle::background` to the navigation region.
This keeps system-bar surfaces aligned with custom component styles without adding duplicate color fields to those style types.

A full-window custom page may override both regions directly:

```cpp
return VideoPage().With(
    SystemBarsAppearance{
        .status_bar_background = Color::Transparent(),
        .navigation_bar_background = Color::Rgb(0, 0, 0, 0.4F),
        .status_bar_content = SystemBarContentBrightness::Light,
        .navigation_bar_content = SystemBarContentBrightness::Light,
    }
);
```

## Appearance resolution

Runtime resolves each system bar independently after final layout and presentation geometry.
The resolution order is:

- The highest painted appearance declaration adjoining the corresponding physical edge or safe-content boundary.
- The active application Theme's `SystemBarsAppearance`.
- `SystemBarsAppearance::Default()`.

TopAppBar and NavigationBar publish ordinary appearance declarations using their resolved backgrounds.
An application root declaration adjoins both edges and controls both bars.
A TopAppBar declaration controls only the status bar, and a NavigationBar declaration controls only the navigation region.
View modifier order remains significant, so a modifier applied directly to a built-in edge component after its construction replaces that component's declaration.

An ordinary captured presentation Theme does not override the application system bars by itself.
Dialog, Menu, Popup, Toast, and BottomSheet affect system-bar appearance only when their content carries an explicit declaration.
This prevents a transient surface from changing native icons merely because it inherited the same Theme.

Fully transparent or zero-opacity declarations do not win resolution.
Exiting layers remain eligible only while they are visibly painted.
The existing Layer level and sequence order determine which explicit presentation declaration is highest.

`Automatic` selects light or dark foreground from the effective background's relative luminance.
When a translucent background is composited over a known themed backplane, resolution uses the composited color.
Image, video, and arbitrary Canvas content cannot be inferred reliably and should specify `Light` or `Dark` explicitly.

Runtime caches the resolved native foreground pair.
It notifies the platform only when the status or navigation brightness changes.
Background colors are rendered by HuxerUI and are not delegated to deprecated or platform-specific native color APIs.

## Runtime backplane

The Runtime root owns a small paint-only backplane for the status and navigation rectangles.
It uses existing rectangle paint commands and does not introduce a new PaintCommand.

In `SafeArea` mode the backplane remains visible because application content is placed between the bars.
In `EdgeToEdge` mode it provides a deterministic base beneath transparent or partially covered application content, while TopAppBar, NavigationBar, media, or other full-window Views may paint over it.

Changing only a resolved backplane color invalidates the corresponding root paint region.
It does not invalidate application measurement or rebuild PaintSequences unrelated to that region.

## Layer placement

Safe-area behavior belongs to the existing internal Layer placement contract rather than each presentation service implementing its own coordinate adjustment.

The internal policies are:

| Policy | Use |
|---|---|
| Constrain | Dialog, Toast, Menu, Popup, text-selection surfaces, and performance panels |
| ExtendBottom | BottomSheet surfaces whose background reaches the physical bottom while content consumes the bottom inset |
| Ignore | Full-window barriers, Debug corner ribbons, and intentionally full-window System layers |

The policy remains implementation-only.
It does not add a public Layer enum.

Dialog centers within the safe content rectangle.
Toast placement applies its themed viewport padding inside that rectangle.
Menu and Popup retain anchors in final host-view coordinates, perform preferred-side selection against the safe rectangle, and clamp the resulting surface without mutating the anchor.

A modal barrier continues to cover the complete viewport even when its presented surface is constrained.
BottomSheet fills to the physical bottom and applies bottom safe-area padding inside its surface.
Drawer modal scrims cover the complete viewport while the drawer surface owns its content insets.

## Incremental invalidation

`Runtime::SetWindowMetrics()` compares the complete value before scheduling work.

When only `safe_area` changes, Runtime invalidates application-root measurement, Layer placement, affected system-bar backplane paint, and visible text-selection geometry.
It does not recompose the application.

When viewport width crosses a configured breakpoint, Runtime preserves the existing behavior of updating `ViewportClass`, recomposing the application root, and invalidating captured Layer entries.
Insets do not participate in width-class calculation, so small cutout changes cannot make structural layout oscillate around a breakpoint.

When a Theme or edge component changes `SystemBarsAppearance`, normal reconciliation updates only its appearance contribution.
The platform foreground call remains deduplicated.

## IME and text input

IME occlusion is not part of `safe_area`.
Android IME Insets, the iOS keyboard frame, and future OHOS keyboard AvoidArea continue through the current logical viewport-resize path.

The platform computes the effective safe area after applying keyboard occlusion.
When the keyboard covers the bottom system area, the submitted bottom inset is zero.
When it hides, the platform restores the native navigation or home-indicator inset.

BringTextInputIntoView, selection handles, the selection menu, and platform caret geometry use the final logical viewport and host-view coordinate transforms exactly as they do today.
They do not add platform-specific SafeArea branches.

## Platform adapter boundary

PlatformAdapter gains one optional system-bar foreground operation rather than a new PlatformWindow abstraction.
The operation receives already resolved light or dark foreground values and performs no layout work.

The shared Runtime owns:

- `WindowContentMode` behavior.
- Safe-area consumption.
- system-bar backplane colors.
- Theme and component appearance resolution.
- appearance deduplication.
- presentation safe bounds.

Platform adapters own:

- Full platform surface configuration.
- viewport and safe-area collection.
- unit conversion.
- IME occlusion conversion.
- native status and navigation foreground application.
- native lifecycle registration and cleanup.

An embedded PlatformView may have no authority over its containing system window.
In that case the adapter reports geometry but the optional system-bar operation is a no-op unless its native owner installs a window delegate.

## Android mapping

HuxerUIActivity configures an edge-to-edge Android window consistently across the supported API range.
`WindowContentMode::SafeArea` remains a shared Runtime layout policy rather than attempting to disable Android edge-to-edge behavior.

On API 30 and later, HuxerUIView obtains system bars, display cutout, and IME Insets through their typed WindowInsets categories.
On API 23 through 29, it uses the corresponding guarded system-window and display-cutout values.

The resolved safe area takes the maximum obstruction on each edge rather than adding overlapping status-bar and cutout values.
Insets are converted from pixels to logical units before `SetWindowMetrics()`.

Status foreground supports dark and light modes from API 23.
Navigation foreground supports them from API 26.
Unsupported older navigation behavior keeps the platform default.

HuxerUIActivity owns Window appearance application and supplies that capability to HuxerUIView.
HuxerUIView does not cast an arbitrary Context to Activity, preserving embedded use inside application-owned hosts.

HuxerUIActivity keeps the native bar surfaces transparent while the shared backplane paints their resolved colors.
The Activity disables platform contrast scrims where that API exists because the HuxerUI backplane already provides the deterministic surface behind system content.

## iOS mapping

The root HuxerUIView is constrained to all four edges of its UIViewController instead of `safeAreaLayoutGuide`.
This lets the shared Runtime draw system-bar backplanes and perform the same `SafeArea` or `EdgeToEdge` layout policy as Android.

HuxerUIView forwards `safeAreaInsetsDidChange` through its adapter.
The adapter combines the full View bounds, current safeAreaInsets, and keyboard frame into one `WindowMetrics` update.

HuxerUIIOSViewController stores the resolved status foreground, overrides `preferredStatusBarStyle`, and calls `setNeedsStatusBarAppearanceUpdate` only when the value changes.
iOS does not expose a corresponding navigation or Home Indicator foreground API, so the navigation brightness field has no native operation there.

The minimum deployment target remains iOS 13.
All UIKit lifecycle and status-bar work stays on the main thread.

## OHOS mapping

The future OHOS application host uses a full-window ArkUI surface and keeps Window ownership in its ArkTS or UIAbility layer.
The native XComponent does not acquire or retain an application Window directly.

The host obtains and observes:

- `AvoidAreaType.TYPE_SYSTEM`.
- `AvoidAreaType.TYPE_NAVIGATION_INDICATOR`.
- `AvoidAreaType.TYPE_CUTOUT`.

It listens for `avoidAreaChange` so rotation, split-screen changes, foldable posture, and window-mode changes update `WindowMetrics`.
The safe area takes the maximum resolved obstruction on every physical edge.

`AvoidAreaType.TYPE_KEYBOARD` belongs to the IME viewport path and is not merged into `safe_area`.
System-bar foreground maps through `setWindowSystemBarProperties` in the ArkTS Window owner.

Both HuxerUI content modes keep the platform surface full-window for consistency.
The shared Runtime simulates safe content in `SafeArea` mode and owns edge-to-edge layout in `EdgeToEdge` mode.

## Desktop and Web mapping

Windows, macOS, and Linux ordinary client areas submit zero safe-area insets.
The new content mode therefore does not change their current application layout.
Desktop custom title bars, caption buttons, and draggable regions belong to a later Window Chrome design.

The Web adapter can later read CSS `env(safe-area-inset-top)`, `env(safe-area-inset-right)`, `env(safe-area-inset-bottom)`, and `env(safe-area-inset-left)` when the document uses an appropriate viewport-fit policy.
Browser chrome usually does not expose a portable runtime foreground control, so the platform system-bar operation may remain empty.

## Validation

Shared validation is required to cover:

- WindowMetrics validation and equality-aware scheduling.
- SafeArea and EdgeToEdge root measurement and placement.
- additive ordinary and safe-area padding.
- selected-edge behavior and nested consumption.
- measurement-cache invalidation when inherited insets change.
- unchanged recomposition count for inset-only changes.
- ViewportClass behavior across metric changes.
- Theme fallback and explicit appearance precedence.
- TopAppBar and NavigationBar background contribution.
- Automatic foreground contrast and explicit overrides.
- deduplicated PlatformAdapter foreground updates.
- Dialog, Toast, Popup, Menu, BottomSheet, barrier, Drawer, and debug placement.
- IME bottom-inset restoration.

Android validation includes compiling both native ABIs and Java window integration; focused platform tests should continue expanding typed and legacy Insets conversion, display cutouts, navigation modes, IME overlap, foreground API guards, and an embedded HuxerUIView without a Window owner.

iOS source changes require an available macOS build to validate UIKit compilation and behavior.
Until that validation is run, the implementation report must state that iOS remains unverified rather than claiming it passed.

OHOS implementation remains deferred until the repository owns an OHOS adapter, but the adapter must implement this contract without changing the shared API.

## Documentation impact

Implementation updates this document's status and synchronizes:

- Architecture Design for Runtime root and PlatformAdapter ownership.
- Navigation Design for TopAppBar, NavigationBar, NavigationPane, and Drawer behavior.
- Theme, Animation, and Presentation for SystemBarsAppearance and safe Layer placement.
- Platform Support for Android and iOS native behavior.
- Layout and Scrolling for SafeAreaPadding and layout-time inset access.
- README when the public API and supported behavior are available.
