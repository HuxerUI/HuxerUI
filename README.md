# HuxerUI

HuxerUI is a cross-platform declarative UI runtime powered by C++. Native backends are available for Android, macOS, and Windows, while core state management, recomposition, node reconciliation, layout, hit testing, and display lists remain in the platform-independent C++ layer.

## Features

- `State<T>` and `UseState()`
- Local component scope generation with `[[huxerui::scope]]`
- Dependency tracking for state reads
- Recomposition scheduling for root and component scopes
- Scope-level local recomposition with coalesced frame requests
- Optional integer, string, and enum keys
- `UseState()` identity based on call site and occurrence
- `Views`, data-driven `ForEach`, virtualized lists, and virtualized grids
- Reconciliation between transient `ViewSpec` objects and persistent `MountedNode` objects
- `Column`, `Row`, `Stack`, `ScrollView`, `VirtualList`, `VirtualGrid`, `Spacer`, `Text`, `Button`, `Checkbox`, `Switch`, and `ProgressCircle`
- Public `Layout<Derived>` extension API shared by built-in and custom layouts
- Public `VirtualLayout<Derived>` extension API for custom virtualized containers
- Typed built-in and component events with `On<Key>()`, `UseEvents()`, and `Emit<Key>()`
- Inherited enabled state, window focus traversal, and platform-independent key events
- Nested Flat and Material Theme providers with semantic Text, Button, Checkbox, Switch, ProgressCircle, Dialog, Toast, and ScrollBar styles
- Per-window layers with Toast and declarative or command-oriented Dialog presentation
- Padding, spacing, frames, foreground and background colors, and corner radii
- Main-axis distribution, cross-axis alignment, stack alignment, and grow factors
- Width-constrained multiline text measurement and rendering
- C++ measurement, layout, and hit testing
- Nested rectangular display-list clipping and paint culling
- Native Android View, AppKit, and Win32 hosts with input forwarding
- One-shot display-synchronized frame scheduling with delayed wakeups
- Android StaticLayout, CoreText, and DirectWrite text measurement and rendering
- Android Canvas, CoreGraphics, and Direct2D drawing

## Example

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::scope]]
View Counter() {
  auto count = UseState(1);

  return Column{
      Text(count),
      Button("+1").OnClick([count] {
        count += 1;
      }),
  }.With(Spacing{16.0F});
}

View App() {
  return Column{
      Counter(),
  }.With(Padding{32.0F});
}

HUXERUI_APP(App, {})
```

`[[huxerui::scope]]` defines the identity boundary of a custom stateful component. Each mounted scope owns an independent `UseState()` state table, so multiple calls to the same component function do not share local state. `UseState()` uses C++20 `std::source_location` to identify call sites within a scope.

`HUXERUI_APP` declares the application entry point. On desktop platforms it
generates `main()` and calls `RunApp()`. On mobile platforms it registers the
application definition for the native lifecycle host. Window options are
written directly after the root factory:

```cpp
HUXERUI_APP(
    App,
    {
        .title = "My App",
        .width = 720.0F,
        .height = 480.0F,
    })
```

Enable build-time scope generation for each target that contains marked components:

```cmake
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE HuxerUI::huxerui)
huxerui_enable_codegen(my_app)
```

The code generator detects `[[huxerui::scope]]` and automatically generates the local state and recomposition boundary before compilation, so the component keeps an ordinary function body. The attribute is only needed on components that require an independent scope. The application root already has an implicit root scope and should remain unmarked. Call `huxerui_enable_codegen()` after adding all sources to the target. Marked function definitions are currently supported in `.cpp`, `.cc`, and `.cxx` files.

Components can expose typed semantic events without adding callbacks to their function parameters:

```cpp
struct SearchBoxEvents {
  struct Submitted
      : Event<SearchBoxEvents, void(std::string)> {};
};

[[huxerui::scope]]
View SearchBox() {
  auto events = UseEvents<SearchBoxEvents>();

  return Button("Submit").OnClick([events] {
    events.Emit<SearchBoxEvents::Submitted>("query");
  });
}

SearchBox().On<SearchBoxEvents::Submitted>(
    [](std::string query) {
      // Handle the submitted query.
    });
```

Event keys carry their callback signature, so incompatible handlers and emitted arguments are rejected at compile time. Each key has at most one handler; a later `.On<Key>()` replaces an earlier one. Events do not bubble, and emitting an event with no handler is a no-op. `EventEmitter` keeps a weak connection to the component scope and becomes disconnected after the component unmounts.

Built-in interactions use the same typed event storage:

```cpp
Button("Save")
    .On<ViewEvents::PointerDown>([](const PointerEvent& event) {
      // Observe the primary pointer.
    })
    .On<ViewEvents::Click>([] {
      Save();
    });
```

`ViewEvents` defines `Click`, pointer events, `FocusChanged`, `KeyDown`, and
`KeyUp`. `OnClick(handler)` is a convenience wrapper for
`On<ViewEvents::Click>(handler)`; both register the same event key, so the
later registration replaces the earlier one. A pointer-down target is retained
by pointer id until Up or Cancel. Move and Up continue to reach that target
outside its bounds, while Click is emitted only when the pointer is released
over the same mounted target. Captures safely expire when their target
unmounts.

Button, Checkbox, and Switch participate in focus traversal automatically.
Custom interactive Views opt in with `Focusable{}`. Tab and Shift+Tab traverse
enabled focusable nodes, while Enter and Space activate any focused View with
`OnClick()` or built-in activation behavior:

```cpp
Column{
    Button("Save")
        .With(Enabled{can_save})
        .OnClick(Save),
    CustomControl()
        .With(Focusable{})
        .On<ViewEvents::FocusChanged>(HandleFocus)
        .On<ViewEvents::KeyDown>(HandleKey),
};
```

Theme focus rings are shown for keyboard focus navigation. Pointer presses can
move focus without drawing a focus ring over the pressed indication.

Checkbox and Switch are controlled components. Their current value comes from
the constructor, and `OnChanged(bool)` asks the owner to update that value:

```cpp
auto checked = UseState(false);

return Row{
    Checkbox(checked).OnChanged([checked](bool value) {
      checked = value;
    }),
    Switch(checked).On<ToggleEvents::Changed>([checked](bool value) {
      checked = value;
    }),
};
```

`OnChanged()` is a convenience wrapper for
`On<ToggleEvents::Changed>()`. Pointer clicks, Enter, and Space all use the
same activation path. Switch movement is retained by its mounted modifier and
uses the current Theme motion duration; reduced-motion themes update it
immediately. `CheckboxStyleKey` and `SwitchStyleKey` allow a nested Theme to
replace their semantic styles.

`ProgressCircle` supports both indeterminate and determinate progress:

```cpp
ProgressCircle();
ProgressCircle(0.65F);
ProgressCircle(progress_state);
```

Determinate values are constrained to the `0` to `1` range. An indeterminate
circle rotates using its mounted modifier, while a determinate circle does not
continuously request frames. Reduced-motion themes keep the indeterminate arc
static. `ProgressCircleStyleKey` controls its intrinsic size, stroke width,
track and indicator colors, arc fraction, and animation duration. `Frame` can
override the intrinsic size.

`Enabled{false}` is inherited by the entire subtree. A disabled control remains
a hit-test barrier, preventing input from falling through to content behind
it, but it does not receive pointer, scroll, focus, or Click interaction.
Modal Dialog layers trap focus in the topmost Dialog and restore the previous
focus when dismissed.

`State<T>` is a lightweight handle to a shared `StateCell<T>` and can be passed to child components by value. A scope subscribes to state changes when it reads that state:

```cpp
[[huxerui::scope]]
View CounterLabel(State<int> count) {
  return Text(count);
}
```

The generated scope factory runs lazily and captures component parameters referenced by the body by value.

When local component state changes, the runtime only recomposes scopes subscribed to that state. It does not recompose the root or unrelated sibling scopes. Multiple state changes are coalesced into the next frame. Measurement and layout currently still traverse the mounted node tree, while painting skips fully invisible subtrees using nested visible rectangles.

Keys are optional. Unkeyed siblings use their position and node type as identity, which is suitable for stable UI structures. Use stable keys when a dynamic list can insert, remove, or reorder nodes that need to retain local state:

```cpp
Column{
    UserCard(first).Key(first.id),
    UserCard(second).Key(second.id),
}
```

Keys support signed integers, unsigned integers, strings, and enums. They only need to be unique among siblings under the same parent. Duplicate sibling keys are rejected before reconciliation.

Within a scope, `UseState()` identity combines the source call site with its occurrence during the current composition. Repeated calls from the same line in a loop therefore do not share a `StateCell`, although each occurrence still has positional identity. When loop items can be inserted, removed, or reordered, wrap each item in a scope with a stable key.

`ForEach` uses a factory to transform a range into a collection of child nodes:

```cpp
Column{
    Text("Users"),
    ForEach(users, [](const User& user) {
      return UserRow(user).Key(user.id);
    }),
}.With(Spacing{8.0F});
```

`ForEach` can accept `State<Range>` directly; the framework handles state reads and dependency registration:

```cpp
ForEach(items, [](const Item& item) {
  return ItemRow(item).Key(item.id);
});
```

Reactive text also requires no manual state read:

```cpp
Text(count);
Text::Format("Taps {}", taps);
```

You can omit `.Key()` when stable data identity is unnecessary. `ForEach` returns a build-time `Views` collection that `Column`, `Row`, and `Stack` flatten in place. List items are therefore actual siblings in the parent container, and spacing and key reconciliation do not pass through an extra fragment layout layer. An empty range produces no nodes.

`ForEach` does not provide scrolling itself, but it can be placed inside a vertical `ScrollView`:

```cpp
ScrollView {
    Column {
        ForEach(items, [](const Item& item) {
          return ItemRow(item).Key(item.id);
        }),
    },
};
```

`ScrollView` measures its content with vertically unbounded constraints. Scroll offsets, layout, clipping, hit testing, and pointer-drag scrolling live in the shared C++ layer. The macOS backend forwards mouse drags, trackpad input, and mouse-wheel input. A regular `ScrollView` mounts all of its content; use `VirtualList` for long lists.

`VirtualList` is vertical by default. Omit the item extent when items should use their natural height:

```cpp
VirtualList(
    items,
    [](const Item& item) {
      return ItemRow(item).Key(item.id);
    })
    .With(Spacing{8.0F});
```

Set an item extent when every item has the same main-axis size and the fixed-size fast path is appropriate:

```cpp
VirtualList(
    items,
    [](const Item& item) {
      return ItemRow(item).Key(item.id);
    })
    .ItemExtent(64.0F);
```

Select the horizontal axis for a horizontally scrolling list:

```cpp
VirtualList(
    items,
    [](const Item& item) {
      return ItemCard(item).Key(item.id);
    })
    .ScrollAxis(Axis::Horizontal)
    .ItemExtent(160.0F)
    .With(Spacing{8.0F});
```

`ItemExtent()` always refers to the scroll-axis size: height for a vertical list and width for a horizontal list. Dynamic lists can provide an initial estimate with `.EstimatedItemExtent()`. `.CacheExtent()` controls the extra pixel range mounted before and after the viewport.

Virtual lists invoke the factory and mount nodes only for items in the viewport and its cache region. Variable-size lists cache measured item extents, refine the estimate for unseen items, and compensate the scroll offset when measurements before the visible anchor change. Fixed-size lists calculate offsets and visible indices directly. Both ranges and `State<Range>` can be passed directly.

Once an item leaves the cache, its view, mounted nodes, and scopes are released, while its `UseState()` slots are retained separately. Stable keys restore those slots after cache eviction and keep state attached to data across reordering. A key represents identity for the lifetime of its virtual list, so removing and later reinserting the same key restores its state; use a new key for a new identity. Unkeyed items use their index as state identity, and state beyond the current range size is discarded.

`VirtualGrid` provides a vertically scrolling grid with fixed or adaptive columns:

```cpp
VirtualGrid(
    items,
    [](const Item& item) {
      return ItemCard(item).Key(item.id);
    })
    .Columns(GridColumns::Adaptive(160.0F))
    .RowExtent(120.0F)
    .With(Spacing{8.0F});
```

Use `GridColumns::Fixed(3)` for a fixed column count. Omit `RowExtent()` to measure each row naturally from its tallest item, and provide `EstimatedRowExtent()` when the default estimate is unsuitable. `RowSpacing()` and `ColumnSpacing()` override the common `Spacing` modifier value independently.

Spans must be available before item views are materialized because the grid needs the complete row plan to resolve an arbitrary scroll position. Pass them separately with `.ItemSpans(spans)`; omitted entries default to one column, and spans larger than the current adaptive column count fill the row:

```cpp
VirtualGrid(items, factory)
    .Columns(GridColumns::Adaptive(160.0F))
    .ItemSpans(spans);
```

Changing the viewport width can change the adaptive column count. The grid keeps the first visible item anchored while rebuilding its row plan. It uses the same virtual item reconciliation, keyed state restoration, clipping, hit testing, and cache lifecycle as `VirtualList`.

Create a stable `ScrollState` when scroll position must be observed or controlled:

```cpp
auto scroll = UseScrollState(40.0F);

return VirtualList(items, factory)
    .ScrollState(scroll);
```

`Offset()`, `MaxOffset()`, `ViewportExtent()`, and `ContentExtent()` are observable reads. A Scope that reads them is recomposed when the metrics change. Pixel commands work with `ScrollView` and every virtual layout:

```cpp
scroll.ScrollTo(0.0F);
scroll.ScrollBy(80.0F);
```

Virtual layouts can additionally support item addressing:

```cpp
scroll.ScrollToItem(500, ScrollAlignment::Center);
```

`VirtualList` and `VirtualGrid` implement item addressing. A custom `VirtualLayout` opts in by defining a static `ScrollOffsetForItem()` method with the signature exposed by their built-in counterparts. Commands return `false` when the state is no longer connected, the item is invalid, or the bound container does not support item addressing. The state uses a weak connection and remains safe after its container is unmounted.

All scroll containers support pointer dragging. Movement below the drag threshold remains eligible for Click. Once the dominant-axis movement crosses the threshold, the nearest scrollable container that can move in that direction takes over and sends `PointerCancel` to the original interaction target. Nested containers pass any unconsumed drag distance to a compatible ancestor at their boundary.

Opt-in overlay scrollbars are shared by `ScrollView`, built-in virtual containers, and custom `VirtualLayout` types:

```cpp
VirtualList(items, factory).With(
    ScrollBar{});

ScrollView{content}.With(
    ScrollBar{ScrollBarStyle{
        .thickness = 8.0F,
        .minimum_thumb_extent = 28.0F,
        .margin = 4.0F,
        .corner_radius = 4.0F,
        .fade_in_duration = 0.12F,
        .fade_out_delay = 0.7F,
        .fade_out_duration = 0.22F,
        .track_color = Color::Transparent(),
        .thumb_color = Color::Rgb(120, 126, 136, 0.8F),
    }});
```

The scrollbar infers its axis, does not affect content layout, and is omitted when the content does not overflow. It fades out after inactivity and reappears while scrolling, hovering, or dragging. Its thumb can be dragged directly and takes pointer priority over content interactions while visible; a fully hidden scrollbar does not intercept content input. Fade timings are configurable through `ScrollBarStyle`. Track-page clicks, inertia, and overscroll effects are not yet implemented.

Layout nodes use braces to express parent-child relationships, while individual controls use constructor calls:

```cpp
Column{
    Text("Title"),
    Row{
        Button("Cancel"),
        Button("Confirm"),
    }.With(Spacing{8.0F}),
}
```

`Row` and `Column` arrange children from the start of the main axis by default and preserve their intrinsic size on the cross axis. Main-axis distribution and cross-axis alignment can be configured explicitly:

```cpp
Row{
    Text("Status"),
    Spacer(),
    Button("Save"),
}.With(
    Spacing{8.0F},
    CrossAlign{CrossAxisAlignment::Center});
```

`Spacer()` has a default grow factor of 1. Other nodes can use `Grow{factor}` to receive a proportional share of the remaining space on the main axis:

```cpp
Row{
    Sidebar().With(Frame{240.0F, 600.0F}),
    Content().With(Grow{}),
};
```

The main axis supports `Start`, `Center`, `End`, `SpaceBetween`, `SpaceAround`, and `SpaceEvenly`. The cross axis supports `Start`, `Center`, `End`, and `Stretch`. `Stack` uses horizontal and vertical alignment:

```cpp
Stack{
    Content(),
}.With(Align{
    HorizontalAlignment::Center,
    VerticalAlignment::Center,
});
```

`Row`, `Column`, and `Stack` use the same public layout protocol as application-defined layouts. A custom layout derives from `Layout<Derived>` and implements a static `Measure` function:

```cpp
class Flow final : public Layout<Flow> {
public:
  using Layout::Layout;

  Flow Gap(float value) && {
    SetSpacing(value);
    return std::move(*this);
  }

  static LayoutResult Measure(
      LayoutContext& context,
      MountedNode& node,
      Constraints constraints) {
    LayoutResult result;
    float x = 0.0F;
    float height = 0.0F;

    for (MountedNode& child : node.Children()) {
      const Size size =
          context.Measure(child, constraints.Loose());
      result.Place(child, {x, 0.0F});
      x += size.width + node.Spacing();
      height = std::max(height, size.height);
    }

    result.SetSize(constraints.Constrain({x, height}));
    return result;
  }
};
```

Custom layouts are used like built-in containers, and generic modifiers preserve their concrete type:

```cpp
Flow{
    Tag("C++"),
    Tag("Runtime"),
}
    .With(Padding{12.0F})
    .Gap(8.0F);
```

`LayoutContext::Measure()` recursively measures a child. `LayoutResult` records content size and relative child placements; the runtime applies padding and the final parent origin. `MountedNode::Cache<T>()` provides per-node layout cache storage. Typed `LayoutValue<Key>()` values let a parent layout read container-specific data from its children without adding every custom property to the global view style. Layout type participates in reconciliation, so different layout classes never reuse the same mounted node accidentally.

Custom virtualized containers derive from `VirtualLayout<Derived>`. Their item source remains demand-driven: `VirtualLayoutContext::Item(index)` requests a logical item, while the runtime creates or reuses its mounted node and restores any saved scope state. The layout measures requested items and returns content-coordinate placements:

```cpp
class VirtualStrip final : public VirtualLayout<VirtualStrip> {
public:
  using VirtualLayout::VirtualLayout;

  static VirtualLayoutResult Measure(
      VirtualLayoutContext& context,
      MountedNode& node,
      Constraints constraints) {
    constexpr float extent = 40.0F;
    const VirtualViewport viewport = context.Viewport();
    const std::size_t first =
        static_cast<std::size_t>(viewport.offset.y / extent);
    const std::size_t last = std::min(
        context.ItemCount(),
        first + static_cast<std::size_t>(viewport.size.height / extent) + 2);

    VirtualLayoutResult result;
    for (std::size_t index = first; index < last; ++index) {
      MountedNode& item = context.Item(index);
      context.Measure(item, constraints.LooseHeight().TightHeight(extent));
      result.Place(item, {0.0F, static_cast<float>(index) * extent});
    }

    const float content_height =
        static_cast<float>(context.ItemCount()) * extent;
    const Size size =
        constraints.Constrain({constraints.max_width, content_height});
    return result.SetAxis(Axis::Vertical)
        .SetSize(size)
        .SetContentSize({size.width, content_height});
  }
};
```

Only items included in the final `VirtualLayoutResult` remain mounted. The runtime owns item reconciliation, duplicate-key validation, state saving, clipping, hit testing, scrolling, and cleanup. `MountedNode::Children()` on a virtual layout contains only its currently mounted items; use `VirtualLayoutContext::ItemCount()` for the logical item count. A virtual layout must receive bounded constraints on its scroll axis. The same protocol also supports non-linear containers: an external grid can cache its own row plan, read typed span metadata, request items by visible row, return two-dimensional placements, and correct its scroll anchor when the column count changes without runtime-specific grid support.

The root node fills the window viewport. Text nodes calculate multiline height from the maximum width supplied by their parent layout, while buttons remain centered on a single line.

`View` uses copy-on-write semantics. Adding a modifier to one copied view does not change the other copy's `ViewSpec`.

Themes are deferred Environment providers. `FlatTheme`, `FlatDarkTheme`,
`MaterialTheme`, and `MaterialDarkTheme` supply complete light and dark theme
boundaries. MaterialTheme maps the current HuxerUI semantic token set and
component styles to a Material 3 baseline, including filled buttons, inverse
surface Toasts, focused and disabled states, and ripple indication. A
`ThemeDefinition` without a `ThemeSpec` inherits the nearest parent theme and
can override individual component styles:

```cpp
template <class Factory>
View AccentTheme(Factory&& content) {
  ThemeDefinition definition;
  definition.Set<ButtonStyleKey>(ButtonStyle{
      .background = Color::Rgb(207, 34, 46),
      .foreground = Color::White(),
      .font_size = 14.0F,
      .padding = EdgeInsets::Symmetric(16.0F, 8.0F),
      .corner_radius = 12.0F,
  });
  return Theme(
      std::move(definition),
      std::forward<Factory>(content));
}

auto App() {
  return HUXERUI_THEME(
      MaterialTheme,
      Column{
          Text("Material", TextRole::Title),
          HUXERUI_THEME(
              AccentTheme,
              Button("Nested override")),
          HUXERUI_THEME(
              MaterialDarkTheme,
              Button("Nested Material dark theme")),
          HUXERUI_THEME(
              FlatTheme,
              Button("Nested Flat boundary")),
      });
}
```

`TextRole::Body`, `TextRole::Label`, and `TextRole::Title` select semantic
typography without hard-coding font sizes at the call site. Text, Button,
Dialog, Toast, ScrollBar, and the default hover and pressed indication derive
their defaults from the nearest `ThemeSpec`. Explicit component StyleKeys can
replace those derived defaults locally. Explicit modifiers such as
`Background`, `Foreground`, and `FontSize` are applied afterward and therefore
take precedence over Theme values.

Built-in theme tokens can be used as a customization starting point:

```cpp
template <class Factory>
View BrandTheme(Factory&& content) {
  ThemeSpec theme = MaterialLightThemeSpec();
  theme.colors.primary = Color::Rgb(130, 80, 210);
  theme.colors.on_primary = Color::White();
  return MaterialTheme(
      std::move(theme),
      std::forward<Factory>(content));
}
```

The `MaterialTheme(ThemeSpec, factory)` overload rebuilds the Material
component StyleKeys from the customized tokens. Use a plain
`ThemeDefinition{theme}` when only the generic semantic defaults are desired.
HuxerUI currently maps the subset of Material tokens consumed by its available
components; it does not claim to implement the complete Material component
catalog.

## Architecture

```text
App()
  ↓
Scope / Composer / UseState / State dependency tracking
  ↓
ViewSpec
  ↓
Reconcile
  ↓
MountedNode
  ↓
Measure / Layout / HitTest
  ↓
DisplayList
  ↓
macOS AppKit / CoreText / CoreGraphics
Windows Win32 / DirectWrite / Direct2D
Android View / StaticLayout / Canvas
```

The platform layer handles windows or host views, frame scheduling, input forwarding, text services, and the canvas. The shared C++ core does not own Android View, AppKit, or Win32 objects.

## Building

macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows with Visual Studio 2022:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Unit tests use the vendored Catch2 sources under `3dparty`, so configuring and
building the test targets does not require downloading dependencies.

The Windows backend targets Windows 10 and later.

Android provides `HuxerUIActivity` for full-screen applications and
`HuxerUIView` for embedding HuxerUI content in an existing Android interface.
`HuxerUIView` loads the shared C++ runtime, while `HuxerUIActivity` loads the
application native library named `huxerui_app`.
`HUXERUI_APP` registers one immutable application definition on mobile
platforms. Every native host view creates its own `Runtime`, so multiple
views can share the same root factory without sharing their state tree,
layout, frame scheduling, or input state.

Mobile platform integrations implement `PlatformHost`, create a `Runtime` from
the registered `AppDefinition`, and forward viewport, frame, and input events:

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

View App() {
  return MaterialTheme([] {
    return Button("Mobile");
  });
}

class MobileHost final : public PlatformHost {
  // Implement frame scheduling, time, and text measurement.
};

MobileHost host;
Runtime runtime{
    {
        .root_factory = App,
        .options = {.title = "HuxerUI"},
    },
    host,
};

runtime.SetViewport({width, height});
const DisplayList& display_list = runtime.BuildFrame();
```

`Runtime` is shared by Android, iOS, and OHOS integrations. Rendering and
native lifecycle ownership remain platform specific.

The Android Gradle project contains the `huxerui` library module and the
`demo` application module. A full-screen application only needs to derive its
launcher activity from `HuxerUIActivity`:

```java
public final class MainActivity extends HuxerUIActivity {}
```

The application module builds its native entry point as `huxerui_app`.
Loading that library performs mobile application registration before the
activity constructs its `HuxerUIView`.

The Android host keeps HuxerUI coordinates density independent, maps
multi-touch, mouse hover, wheel, and keyboard events to the shared input
model, and schedules animation frames through the View frame clock. The
minimum supported Android API level is 23. The complete Gradle and CMake
integration is available in `platform/android`; open that directory in
Android Studio or build it from the command line:

```powershell
cd platform\android
.\gradlew.bat :demo:assembleDebug
```

The demo compiles `App()` from `examples/theme/main.cpp`. Scope code
generation automatically uses the matching host executable under
`tools/prebuilt/<system>/<architecture>`; cross-compiles do not build or run a
target-architecture code generator.

Example applications:

- `huxerui_counter`: component scopes and local state
- `huxerui_layout_gallery`: layout, alignment, grow factors, and multiline text
- `huxerui_dynamic_list`: `ForEach`, stable keys, and per-item local state
- `huxerui_scroll_view`: vertical scrolling, clipping, and local state after scrolling
- `huxerui_virtual_list`: a large virtualized variable-height list
- `huxerui_horizontal_virtual_list`: a virtualized horizontal list with 10,000 fixed-width items
- `huxerui_virtual_grid`: an adaptive virtualized grid with 10,000 items and column spans
- `huxerui_scroll_state`: observable scroll metrics and programmatic item positioning
- `huxerui_toast`: per-window Toast presentation through a root-installed service
- `huxerui_dialog`: declarative modal presentation controlled by local state
- `huxerui_theme`: Material light and dark themes, nested FlatTheme boundaries, semantic text roles, ripple indication, and explicit modifier precedence
- `platform/android/demo`: Android Custom View host displaying the theme example

Run an example:

macOS:

```bash
open build/bin/huxerui_counter.app
open build/bin/huxerui_layout_gallery.app
open build/bin/huxerui_dynamic_list.app
open build/bin/huxerui_scroll_view.app
open build/bin/huxerui_virtual_list.app
open build/bin/huxerui_horizontal_virtual_list.app
open build/bin/huxerui_virtual_grid.app
open build/bin/huxerui_scroll_state.app
open build/bin/huxerui_toast.app
open build/bin/huxerui_dialog.app
open build/bin/huxerui_theme.app
```

Windows:

```powershell
.\build\bin\Debug\huxerui_counter.exe
.\build\bin\Debug\huxerui_theme.exe
```

## CMake Options

| Option | Default | Description |
|---|---:|---|
| `HUXERUI_BUILD_SHARED` | `ON` | Build the shared library |
| `HUXERUI_BUILD_STATIC` | `ON` | Build the static library |
| `HUXERUI_BUILD_TESTS` | `ON` for the top-level project | Build runtime tests |
| `HUXERUI_BUILD_EXAMPLES` | `ON` for the top-level project | Build examples |

## Roadmap

- Local measurement, layout, and paint invalidation
- Composite key paths
- Minimum and maximum frames, layout priority, and intrinsic-size queries
- Clipping, transforms, and opacity
- Event capture/bubbling and explicit user-controlled pointer capture
- IME, editable text input, and text selection
- General saveable-state APIs, inertial scrolling, public animation APIs, and overscroll effects
- Semantics tree and accessibility
- iOS, Linux, and Web backends
