# Layout and Scrolling

## Constraints

Parents measure children with minimum and maximum width and height constraints.
A layout returns a constrained size and placements for the children it measured.
Application code normally expresses intent with built-in containers and modifiers rather than fixed device dimensions.

## Built-in layouts

- `Row` and `Column` arrange children along one axis.
- `Stack` overlays children.
- `Flow` wraps children across lines.
- `Box` supplies a simple single-child surface.
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
It owns drag, wheel, kinetic motion, nested-scroll arbitration, focus reveal, and semantic scroll actions.

```cpp
return ScrollView {
  Column {
    Content(),
  }.With(Spacing(12.0F)),
};
```

Use `ScrollBar()` as a modifier when a visible indicator is required.
The scrollbar reads committed scroll geometry; it does not own or duplicate the content offset.

## ScrollController

A `ScrollController` provides programmatic scrolling and observable metrics.
Keep the controller stable across recomposition.

It can scroll to or by an offset and ask a supported virtual layout to scroll to an item.
Focus and text-input reveal use the same authoritative scroll position internally rather than a separate controller path.
User input, animation, focus reveal, and controller requests all update one authoritative scroll position.

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
