# Layout and Scrolling

## Constraints

Parents measure children with minimum and maximum width and height constraints.
A layout returns a constrained size and placements for the children it measured.
Application code normally expresses intent with built-in containers and modifiers rather than fixed device dimensions.

## Built-in layouts

- `Row` and `Column` arrange children along one axis.
- `SplitLayout` arranges controlled, resizable panes using their Frame and Grow modifiers.
- `DockLayout` arranges retained, movable panels in controlled tab groups and splits.
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
`Row` and `Column` allocate finite remaining main-axis space by grow factor; an unbounded main axis preserves natural sizes instead.
Growing children avoid speculative measurement when the cross axis does not stretch or its stretched size is already fixed by tight constraints.
Stretching under loose cross-axis constraints still measures intrinsic cross sizes before allocating space.

## Resizable panes

SplitLayout accepts ordinary children and Views groups, ignoring empty Views. It defaults to Horizontal; `.Orientation(Axis::Vertical)` stacks panes vertically. Use stable keys when panes can be inserted, removed, or reordered.

```cpp
auto frames = UseState(std::vector<Frame>{
    {.width = 240.0F, .min_width = 160.0F, .max_width = 480.0F},
    {.min_width = 240.0F},
});
auto grows = UseState(std::vector<Grow>{Grow(0.0F), Grow()});
return SplitLayout {
  Text("Sidebar").With(frames->at(0), grows->at(0)).Key("sidebar"),
  Text("Content").With(frames->at(1), grows->at(1)).Key("content"),
}.OnChanged([=](const std::vector<SplitChange>& changes) {
  std::vector<Frame> next_frames = frames;
  std::vector<Grow> next_grows = grows;
  for (const auto& change : changes) {
    next_frames[change.index] = change.frame;
    next_grows[change.index] = change.grow;
  }
  frames = std::move(next_frames);
  grows = std::move(next_grows);
});
```

Separators reserve 8 DIPs before allocation. A zero Grow factor uses the preferred main-axis Frame dimension or the pane's natural size; positive factors share the remaining finite space while honoring minimum and maximum bounds. A growing pane's preferred dimension is not an additional size base. Remaining space can stay unused when there are no growing panes or all of them reach their maximum. Parent constraints take precedence if the requested sizes cannot fit. Unbounded main-axis measurement preserves natural sizing and disables resizing.

Dragging, focused arrow keys, Home/End, and accessibility adjustment emit one synchronous `SplitEvents::Changed` batch. Each `SplitChange` contains the complete proposed Frame/Grow and the pane's current flattened nonempty index. Apply all entries to controlled state; do not retain index batches across reordering. Rejecting a proposal leaves allocation unchanged. A proposal can coordinate other panes' weights or convert a growing pane to fixed sizing when needed to reproduce the requested geometry; write back both Frame and Grow. Window resizing and programmatic updates never emit Changed or overwrite the declared sizing preferences.

Cancel and Escape stop dragging without undoing accepted updates. Incompatible topology, orientation, enabled state, or geometry/configuration changes cancel the active drag. Panes are clipped to their allocations.

The [SplitLayout example](../../examples/split_layout/main.cpp) demonstrates three controlled panes, optional inspector content, stable editor state, and vertical arrangement on compact windows. Build the `example_split_layout` target to run it.

## Docking workspaces

Declare each panel once with a stable ID, title, and View. `DockTree` describes which IDs appear, how they are grouped, and which panel is selected in each group. Apply the same Frame and Grow sizing rules as SplitLayout to the tree's branches. A group's size includes its tab bar and is independent of the selected item's own Frame.

```cpp
const auto tree = UseState(DockTree::Split(Axis::Horizontal, {
    DockTree::Tabs({"files", "search"}).With(Frame{.width = 240.0F, .min_width = 160.0F}),
    DockTree::Tabs({"editor"}).With(Grow()),
}));
return DockLayout(
    {
        DockItem("files", "Files", FilesPanel()),
        DockItem("search", "Search", SearchPanel()),
        DockItem("editor", "Editor", EditorPanel()).Closable(false),
    },
    tree
).OnChanged([tree](DockTree next) { tree = std::move(next); });
```

Tabs can be dropped before or after another tab, or on a panel edge to create a split. The tab context menu offers the same edits through right click, the Context Menu key, or Shift+F10. Selection, closing, docking, and resizing each propose a complete tree through `DockEvents::Changed`. Write it back to accept the edit.

For application commands, use `Select`, `Remove`, or `Dock` on the current tree. For example, `tree = tree->Dock("search", "editor", DockPlacement::TabAfter)` reopens or moves Search beside Editor. Removing Search from the tree hides it. Keeping its DockItem declaration preserves its mounted content and local State; removing the declaration unmounts it. If the tree is empty, start it again with `DockTree::Tabs`.

The [DockLayout example](../../examples/dock_layout/main.cpp) is a project workspace with a file tree, an editor, an inspector, and an activity log. Move or resize panels, then close and reopen a pinned Inspector from the Panels menu to see its local state retained. Build `example_dock_layout` to run it. See the [design contract](../design/split-and-docking.md) for sizing and retention details.

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

`ScrollView` measures one content subtree and clips presentation to the viewport.
It owns direct drag, wheel and trackpad consumption, kinetic motion, nested-scroll arbitration, overscroll, focus reveal, and semantic scroll actions.

```cpp
return ScrollView {
  Column {
    Content(),
  }.With(Spacing(12.0F)),
};
```

`ScrollView` scrolls vertically by default. Use `ScrollAxis(Axis::Horizontal)` for a horizontal viewport, or `ScrollBothAxes()` to allow diagonal scrolling within one viewport.

```cpp
auto horizontal = UseScrollController();
auto vertical = UseScrollController();

return ScrollView(content)
    .ScrollBothAxes()
    .Controller(Axis::Horizontal, horizontal)
    .Controller(Axis::Vertical, vertical)
    .With(ScrollBar());
```

Use `ScrollBar()` as a modifier to draw an interactive indicator on each axis with a scroll range.
The scrollbar reads committed scroll geometry; it does not own or duplicate the content offset.

Call `.FillViewport()` when short content should occupy the whole scroll viewport.
It raises the content's minimum extent on every enabled axis, using the viewport extent on a bounded axis and the scroll view's incoming minimum on an unbounded one; content that is intrinsically larger keeps its size and still scrolls.
Bottom-anchored feeds combine it with a `MainAlign(MainAxisAlignment::End)` content column so a short transcript sits at the end of the viewport.

```cpp
return ScrollView(
    Column {
      Transcript(),
    }.With(MainAlign(MainAxisAlignment::End))
).FillViewport();
```

Typical uses:

- Bottom-anchored transcripts, logs, and activity feeds whose short content should sit against the composer or footer.
- Empty states and placeholder pages that should center or bottom-align inside the viewport instead of shrinking to the top.
- Form sheets with footer actions that stay at the bottom until the form grows beyond the viewport.
- Full-viewport canvases, backgrounds, and selection surfaces whose painted and hit-tested area must cover the complete viewport.
- Short horizontal rows, tab strips, and carousels that should span the viewport while still scrolling when they overflow.
- Two-axis boards whose pan surface should be at least the viewport in both directions.

The method only changes measurement: on each enabled scroll axis it raises the content minimum to the bounded viewport extent or, when that axis has no bounded extent, to the scroll view's incoming minimum constraint.
It leaves the maximum unbounded and does not affect offset ownership, maximum offset, interaction, or the default shrink-wrapped behavior.
Content that is intrinsically larger keeps its natural size and scrolls as before.
With the usual zero minimum, an unbounded axis sees no difference; a positive parent minimum is forwarded to the content instead of being replaced by zero.

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
`OnChanged(std::size_t)` requests a new controlled index; programmatic selection does not emit Changed.
`OnSettled(std::size_t)` reports the displayed page after a transition or rebound finishes and its final layout is stable, including programmatic transitions and reduced motion.
A short or canceled drag, or a rejected selection proposal, settles back to the original index, so successive Settled events can contain the same index.
Retargeting an unfinished animation reports only the final settled target, not the interrupted target.
Initial mount and stationary recomposition do not emit Settled; the event describes component layout, not acknowledgement that a frame reached the display.
Set `DragEnabled(false)` to keep programmatic and accessibility paging while disabling direct pointer paging.
`Reverse()` applies an explicit direction inversion; it is not a replacement for natural layout direction.
Pager does not map wheel or trackpad distance to page changes.

## ScrollController

A `ScrollController` provides programmatic scrolling and observable metrics for one axis.
Keep the controller stable across recomposition.

Bind a controller to a single-axis view with `.Controller(controller)`. Two-axis views use `.Controller(axis, controller)` for each axis that needs application control. Use distinct controllers when binding both axes.

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
UiWindow owns item reconciliation, keys, saved state, clipping, input, semantics, scrolling, and cleanup; the custom layout owns visible-range selection and placement.

For a two-axis layout, call the protected `EnableBothAxes()` in the derived constructor. Select the rows and columns intersecting `context.Viewport().Bounds()` and any layout-owned overscan. The [read-only virtual table example](../../examples/virtual_table/main.cpp) demonstrates this pattern with a million logical cells.

See [Extending HuxerUI](extending.md) for the extension contracts.
