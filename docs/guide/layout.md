# Layout and Scrolling

## Constraints

Parents measure children with minimum and maximum width and height constraints.
A layout returns a constrained size and placements for the children it measured.
Application code normally expresses intent with built-in containers and modifiers rather than fixed device dimensions.

## Built-in layouts

- `Row` and `Column` arrange children along one axis.
- `Stack` overlays children.
- `Flow` wraps children across lines.
- `Spacer` consumes remaining main-axis space in a `Row` or `Column`.
- `IndexedPages` retains pages while measuring and placing the selected page.
- `Pager` retains pages while adding controlled animated and direct paging.
- `RefreshBox` coordinates controlled leading-edge refresh around one content subtree.
- `ScrollView` makes one ordinary subtree scrollable.
- `VirtualList` and `VirtualGrid` materialize only required logical items.

```cpp
return Column {
  Text("Profile", TextRole::Title),
  Row {
    Button("Cancel"),
    Button("Save"),
  }.With(Spacing(8.0F)),
}.With(
    Padding(16.0F),
    Spacing(12.0F),
    CrossAlign(CrossAxisAlignment::Stretch)
);
```

`Grow()` is parent-child layout metadata.
It affects participation in a compatible parent layout and propagates across transparent composition boundaries such as scopes and Environment providers.

## Mounted coordinate spaces

`ViewNode::Bounds()` is the complete node-local layout rectangle in DIPs, including Padding.
`ContentBounds()` is the same rectangle deflated by the resolved Padding and is useful for content drawing and padding-aware hit testing.
`LayoutOffset()` is parent-relative, and `PresentationBounds()` is the node's transformed axis-aligned bound in window DIPs.
Use `LocalToWindow()` and `WindowToLocal()` for point conversion; the inverse returns `std::nullopt` when the resolved presentation transform is not invertible.
`LocalToWindowBounds()` transforms all four rectangle corners and returns their window-space axis-aligned bounds.

The final transform for the current frame is available to `NodeExtension::PrepareGeometry()`.
Earlier extension callbacks may still observe the previously resolved transform, and paint commands remain node-local even when geometry preparation retains a window-space value for a presentation boundary.

## Responsive composition

`UseViewportClass()` exposes the current compact, medium, or expanded width class.
Choose component structure explicitly when the experience changes between those classes.

```cpp
[[huxerui::composable]]
View ResponsiveContent() {
  const ViewportClass viewport = UseViewportClass();
  return viewport == ViewportClass::Compact
      ? CompactContent()
      : ExpandedContent();
}
```

Responsive composition is application policy.
Built-in adaptive components may select their documented presentation from the same metrics, but HuxerUI does not silently rewrite arbitrary layouts.

## Window insets

Window content can use inset or edge-to-edge mode.
The root receives viewport and safe-area data atomically from the platform adapter.
Use the window-level mode and `SafeArea` behavior documented in [Window Insets and System Bars Design](../design/window-insets.md) rather than hard-coded status-bar or navigation-bar padding.

## ScrollView

`ScrollView` measures one content subtree along its scrolling axis and clips presentation to the viewport.
It owns direct drag, wheel and trackpad consumption, kinetic motion, nested-scroll arbitration, overscroll, focus reveal, and semantic scroll actions.

```cpp
return ScrollView {
  Column {
    Content(),
  }.With(Spacing(12.0F)),
};
```

Use `ScrollBar()` as a modifier when a visible indicator is required.
The scrollbar reads committed scroll geometry; it does not own or duplicate the content offset.

An explicit `ScrollPhysics` modifier configures fling and overscroll behavior for one container.
Without one, the container uses the current platform adapter's default physics.

```cpp
return ScrollView(content).With(ScrollPhysics{
    .fling_enabled = true,
    .overscroll_enabled = true,
});
```

Wheel and trackpad input scrolls nested containers independently on each axis and falls back to the host when HuxerUI consumes neither axis.
Direct touch dragging may retain a temporary overscroll displacement after every nested consumer reaches its boundary.
That displacement never changes controller metrics or the authoritative content offset.

Use `ViewEvents::ScrollInput` only when a View needs the raw wheel or trackpad update before default scrolling, such as a zoomable canvas.
Returning true consumes the complete two-dimensional update; returning false leaves it to built-in scrolling.
Touch dragging remains pointer input and does not emit `ScrollInputEvent`.

## RefreshBox

`RefreshBox` wraps one ordinary content View and turns an unconsumed downward touch pull at the leading vertical boundary into a controlled refresh request.
The content retains its mounted identity, scroll offsets, focus, editing state, PlatformViews, and textures while its presentation is displaced.

```cpp
return RefreshBox(
    ScrollView {
      MessageList(messages),
    },
    refreshing
).OnRefresh([=] {
  refreshing = true;
  tasks.Launch([=]() -> Task<void> {
    co_await ReloadMessages();
    refreshing = false;
  });
});
```

The supplied `refreshing` value is authoritative.
An armed release or the localized accessibility Refresh action emits `RefreshEvents::Requested`; setting `refreshing` to true holds the content at the Theme-defined refresh position, while leaving it false settles the pull immediately.
Programmatically changing the value to true presents the same refreshing state without emitting an event.

Only direct touch dragging can initiate the gesture.
Wheel, trackpad, upward, trailing-edge, canceled, disabled, and unarmed input never requests refresh.
The built-in indicator, pull resistance, maximum displacement, trigger distance, refresh position, and settle motion are resolved from `RefreshBoxStyle` in the active Theme.
`RefreshBox` does not own a Task, controller, loading result, or application data.

## IndexedPages and Pager

Use `IndexedPages` when peer pages should retain local state and switch immediately under an external selection control.
Use `Pager` when the same retained pages also need animated controlled changes and direct one-page dragging.

```cpp
return Pager(
           {
               OverviewPage(),
               ActivityPage(),
               SettingsPage(),
           },
           selected_page
)
    .ScrollAxis(Axis::Horizontal)
    .OnChanged([selected_page](std::size_t index) {
      selected_page = index;
    });
```

Pager never owns the selected index.
Set `DragEnabled(false)` to keep programmatic and accessibility paging while disabling direct pointer paging.
`Reverse()` applies an explicit direction inversion; it is not a replacement for natural layout direction.
Pager does not map wheel or trackpad distance to page changes.

## ScrollController

A `ScrollController` provides programmatic scrolling and observable metrics.
Keep the controller stable across recomposition.

It can scroll to or by an offset and ask a supported virtual layout to scroll to an item.
Focus and text-input reveal use the same authoritative scroll position internally rather than a separate controller path.
User input, animation, focus reveal, and controller requests all update one authoritative scroll position.
Observe `Metrics()` for content-offset and extent changes instead of deriving position from raw scroll input.
Nested consumption and overscroll do not add another observable scroll state.

## VirtualList

Use `VirtualList` for long or variable-height sequences.
The item factory receives a logical index and creates the item only when required by measurement or retained state.

```cpp
return VirtualList(
    records.size(),
    [&records](std::size_t index) {
      const Record& record = records[index];
      return RecordRow(record).Key(record.id);
    }
);
```

Each reorderable stateful item needs a stable key.
Virtual measurement can revisit an item while resolving its visible range, so factories must remain declarative and must not perform external side effects.

Variable extents are measured from the realized content.
The layout keeps its scroll range and end anchoring consistent when estimates are replaced by measured extents.

## VirtualGrid

`VirtualGrid` virtualizes an adaptive grid and supports item spans.
Use it when the number of columns follows available width instead of describing every row manually.

The grid, not the item, owns column geometry and placement.
Item declarations remain keyed and declarative like `VirtualList` items.

## Custom layout

Derive from `Layout<Derived>` for a layout over ordinary children.
Measure children only through `LayoutContext`, return a constrained size, and record placements in `LayoutResult`.

Derive from `VirtualLayout<Derived>` for a demand-driven logical item source.
Runtime owns item reconciliation, keys, saved state, clipping, input, semantics, scrolling, and cleanup; the custom layout owns visible-range selection and placement.

See [Extending HuxerUI](extending.md) for the extension contracts.
