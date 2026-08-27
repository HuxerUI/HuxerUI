# Navigation and Window

## Navigation surfaces

`NavigationBar` and `NavigationPane` are destination selectors, not page routers. Keep their selected index controlled. `DrawerLayout` combines app content with optional `StartDrawer` and `EndDrawer`; each drawer's open state is controlled through `.Open` and `.OnOpenChanged`.

## Navigation stack

`NavigationStack(root_factory)` provides imperative factory-based navigation. Routed navigation uses:

- copyable, equality-comparable route values;
- `State<NavigationPath<Route>>` as authoritative path state;
- a root factory;
- a resolver from `Route` to `View`;
- `UseNavigation<Route>()` for the nearest compatible stack;
- `UseRootNavigation<Route>()` for the root compatible stack.

The controller supports `Push`, `Replace`, `Pop`, `SetPath`, `CanPop`, and `Depth`. Keep route parameters in the route value instead of side-channel global state. Nested stacks may own local flows while root navigation handles application-level destinations.

On Web, include `<huxerui/web/navigation.h>` and use `BrowserNavigationStack` with a codec whose `Decode(location)` returns an optional path and whose `Encode(path)` returns a same-document location. The codec owns URL policy. One browser navigation stack owns URL synchronization per document.

## Window content

`WindowOptions` configures the title, initial size, content mode, chrome mode, preferred custom title-bar height, and caption labels exposed by the active SDK. `WindowChromeMode::System` leaves chrome to the platform; `WindowChromeMode::Custom` lets application content occupy the title-bar area while retaining HuxerUI's platform-specific window behavior. Use `WindowContentMode::SafeArea` for automatically inset content and `WindowContentMode::EdgeToEdge` only when application content deliberately handles insets.

`SafeAreaPadding` consumes selected safe-area edges without hardcoded platform values. Ancestor consumption affects what descendants see.

`WindowTitleBar` lays out application-defined title-bar content, consumes the platform-resolved caption insets, and marks its non-interactive area as draggable. Interactive descendants such as buttons, tabs, and text fields remain interactive inside it and need no exclusion rectangles. Do not duplicate framework- or platform-managed caption controls. Use `WindowDragRegion` only when another mounted region should drag the window.

`UseWindow()` exposes `Minimize`, `Maximize`, `Restore`, `ToggleMaximize`, and `Close`. It does not expose window dragging, state observation, host window handles, or application-readable caption metrics.

System bar background and foreground brightness come from the resolved `SystemBarsAppearance`, which is both a Theme value and a View modifier. `Light` and `Dark` describe system foreground icons and text, not the background color. Desktop custom chrome and mobile safe areas share a public window boundary but have different platform behavior.

## Activation

Use `UseApplication().StartupActivation()` for the cold-start `ApplicationActivation`. Inspect its `LaunchActivation`, `UrlActivation`, or `FileActivation` alternative with `std::visit` or `std::get_if`. Register `UseApplication().OnActivation(...)` for later activations while the declaring composition lifetime is mounted. Route external URLs or files by updating the authoritative navigation path rather than creating a parallel page stack.
