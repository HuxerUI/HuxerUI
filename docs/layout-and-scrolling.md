# Layout and Scrolling

## Built-in layouts

`Row` and `Column` arrange children along a main axis. `Flow` wraps horizontal content into lines, and `Stack` overlays children.

```cpp
Row {
  Text("Status"),
  Spacer(),
  Button("Save"),
}.With(
    Spacing(8.0F),
    CrossAlign(CrossAxisAlignment::Center)
);
```

The main axis supports `Start`, `Center`, `End`, `SpaceBetween`, `SpaceAround`, and `SpaceEvenly`. Cross-axis alignment supports `Start`, `Center`, `End`, and `Stretch`. `Stack` uses independent horizontal and vertical alignment.

`Spacer()` grows by default. `Grow(factor)` assigns a proportional share of finite remaining main-axis space:

```cpp
Row {
  Sidebar().With(Frame(240.0F, 600.0F)),
  Content().With(Grow()),
};
```

Grow has no expansion effect on an unbounded main axis.

## Constraints and frames

`Frame(width, height)` is the compact fixed-size form. The structured form can specify either preferred axis and independent bounds:

```cpp
Content().With(Frame{
    .min_width = 240.0F,
    .max_width = 640.0F,
    .min_height = 48.0F,
});
```

Frame dimensions include padding. Local bounds are intersected with parent constraints, and preferred dimensions are clamped to the resulting range.

`Flow` uses `Spacing` both between items and between lines. Main alignment is resolved per line, while Grow divides only the remaining width of the line containing that child.

## Responsive composition

Layouts should adapt locally from the `Constraints` they receive. `Flow`, adaptive `VirtualGrid`, Grow, and custom `Layout` policies can change geometry without rebuilding the View tree.

When a viewport range changes application structure rather than only geometry, read the runtime-managed width class:

```cpp
switch (UseViewportClass()) {
case ViewportClass::Compact:
  return CompactContent();
case ViewportClass::Medium:
  return MediumContent();
case ViewportClass::Expanded:
  return ExpandedContent();
}
```

The default Compact-to-Medium and Medium-to-Expanded boundaries are 600 and 840 logical units. Applications can configure them through `AppOptions::viewport_breakpoints`:

```cpp
HUXERUI_APP(
    App,
    {
        .viewport_breakpoints = ViewportBreakpoints{640.0F, 960.0F},
    }
)
```

Changing width within one class only runs measurement and layout. Crossing a boundary updates the root Environment and recomposes the application root and window layers so captured themed presentation sees the same class. HuxerUI intentionally does not publish continuously changing viewport dimensions through Environment; exact dimensions remain layout constraints and platform geometry.

## ScrollView

`ScrollView` mounts its complete content and scrolls vertically by default:

```cpp
ScrollView {
  Column {
    ForEach(items, ItemRow),
  },
};
```

Select horizontal scrolling explicitly:

```cpp
ScrollView {
  Row {
    ForEach(items, ItemCard),
  },
}.ScrollAxis(Axis::Horizontal);
```

Nested containers consume movement from the innermost compatible container outward. Touch dragging crosses a threshold before taking over from a Click target, and unconsumed movement or inertial velocity can continue through a same-axis ancestor.

## ScrollController

Create a stable controller when code needs to observe or change scroll position:

```cpp
auto scroll = UseScrollController();

return ScrollView { Content() }
    .Controller(scroll);
```

`Offset()`, `MaxOffset()`, `ViewportExtent()`, and `ContentExtent()` are observable state reads. Pixel commands work with regular and virtual containers:

```cpp
scroll.ScrollTo(0.0F);
scroll.ScrollBy(80.0F);
```

Virtual layouts can additionally implement item addressing:

```cpp
scroll.ScrollToItem(500, ScrollAlignment::Center);
```

Controllers hold weak connections and remain safe after the bound container unmounts.

## Scroll behavior

Inertial motion is enabled by default. Use `ScrollPhysics` to tune or disable it without replacing the container's scroll implementation:

```cpp
VirtualList(items, ItemView).With(
    ScrollPhysics{
        .deceleration_rate = 7.0F,
        .minimum_fling_velocity = 48.0F,
        .maximum_fling_velocity = 6000.0F,
    }
);
```

Overlay scrollbars opt in through a modifier:

```cpp
VirtualList(items, ItemView).With(
    ScrollBar()
);
```

The scrollbar infers its axis, appears only when content overflows, and does not affect layout. Its thumb supports direct dragging. Fade timing and colors come from `ScrollBarStyle`.

## VirtualList

`VirtualList` mounts only items intersecting the viewport and cache region. Use natural item extents for variable content:

```cpp
VirtualList(items, [](const Item& item) {
  return ItemRow(item).Key(item.id);
}).With(Spacing(8.0F));
```

Use a fixed extent for the faster index-to-offset path:

```cpp
VirtualList(items, ItemRow)
    .ItemExtent(64.0F);
```

`ItemExtent()` means height for a vertical list and width for a horizontal list. `EstimatedItemExtent()` seeds variable-size estimation and `CacheExtent()` controls the extra mounted pixel range.

When an item leaves the cache, its mounted tree is released while its local state slots remain available. Stable keys restore state across eviction and reordering; unkeyed items use their index.

## VirtualGrid

`VirtualGrid` provides fixed or adaptive columns:

```cpp
VirtualGrid(items, [](const Item& item) {
  return ItemCard(item).Key(item.id);
})
    .Columns(GridColumns::Adaptive(160.0F))
    .RowExtent(120.0F)
    .With(Spacing(8.0F));
```

Use `GridColumns::Fixed(count)` for a fixed count. Omit `RowExtent()` for natural row heights. `RowSpacing()` and `ColumnSpacing()` override the common spacing independently.

Item spans are supplied separately because the grid needs its complete row plan before materializing an arbitrary visible item:

```cpp
VirtualGrid(items, ItemCard)
    .Columns(GridColumns::Adaptive(160.0F))
    .ItemSpans(spans);
```

## Custom layouts

Application layouts derive from `Layout<Derived>` and implement a static `Measure()` function. Custom virtual containers derive from `VirtualLayout<Derived>` and request only the logical items needed for the current viewport.

The runtime continues to own reconciliation, keys, state restoration, clipping, hit testing, scrolling, and cleanup. See [Extending HuxerUI](extending-huxerui.md) for the extension protocols.

