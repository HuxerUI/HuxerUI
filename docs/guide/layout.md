# Layout and Scrolling

## Constraints

Parents give each child minimum and maximum dimensions in DIPs. A finite maximum bounds that axis. Express preferred and allowed dimensions with `Frame`, add inside space with `Padding`, and let the parent resolve the final size. A parent constraint can limit an explicit preferred size.

For a bounded scrolling region, give it finite space through its parent or a `Frame`. A virtual list needs a bounded scroll axis. A virtual grid needs both dimensions bounded.

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

Use `Grow()` on a child to share remaining space in a bounded `Row` or `Column`. A larger factor gets a larger share. Use `Spacer()` to reserve some of that space without visible content. An unbounded main axis keeps natural sizes. `Grow()` on a component applies through transparent composition and Environment boundaries.

### Layout priority

Set `LayoutPriority` on a child of `Row` or `Column` to give it more room when the children compete for space. Higher values keep more of their measured size. An explicit priority, including zero, enables this policy when the main axis has a finite maximum. Children without a priority use zero.

```cpp
return Row {
  Text(title).LineLimit(1).Overflow(TextOverflow::Ellipsis),
  Button("Open").With(LayoutPriority(1.0F)),
}.With(Spacing(8.0F));
```

Use priority for children that should keep their measured size when space is tight. Use `Grow()` to distribute extra space. `Frame` minima still apply, so use a one-line limit, wrapping, or other appropriate overflow handling where the content can shrink.

Custom layouts can read the same metadata with `child.LayoutValueOr<LayoutPriority>(0.0F)`. See [linear allocation](../design/layout.md#linear-allocation) for the sizing rules.

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

Use a pane's `Frame` for its preferred size and bounds. A zero `Grow` keeps it fixed or naturally sized. Positive factors share the space after separators and fixed panes. Supply a bounded main axis to allow resizing.

Dragging, keyboard, and accessibility adjustments deliver one `SplitEvents::Changed` batch for the current pane order. Apply every entry's `Frame` and `Grow` together. A resize can change a pane's sizing mode. Window resizing and programmatic changes leave the declared preferences in application state.

See the [SplitLayout example](../../examples/split_layout/main.cpp) for a controlled three-pane layout and [pane sizing rules](../design/split-and-docking.md#shared-size-rules) for the full allocation contract.

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

### Custom tab bars

Use `DockLayout::TabBar()` to build the bar above each group. `DockTabBarContext::Items()` provides the ordered panel IDs, resolved titles, and close flags. `Selected()` gives the active panel ID. Return `bar.DefaultBar()` to use the built-in control for that group, or `View{}` to hide its bar and give the panel the full height. Copy an ID before capturing it in a callback that outlives the factory call.

```cpp
return DockLayout(items, tree)
    .TabBar([tree](const DockTabBarContext& bar) {
      return Row {
        ForEach(bar.Items(), [&](const DockTabBarItem& item) {
          const std::string id(item.id);
          View label = Chip(item.title, id == bar.Selected())
              .OnClick([tree, id] { tree = tree->Select(id); });
          return bar.BindTabDrop(id, Row {
            bar.BindTabDrag(id, std::move(label)),
            item.closable ? Button("×").OnClick([tree, id] { tree = tree->Remove(id); }) : View{},
          }).Key(id);
        }),
      }.With(Spacing(4.0F));
    })
    .OnChanged([tree](DockTree next) { tree = std::move(next); });
```

`BindTabDrag()` makes the label a drag source, including when its panel is inactive. `BindTabDrop()` uses the left and right halves of the supplied View to insert before or after its panel. In the example, the close button is part of the insertion target and outside the drag source. Only bound zones in a custom bar accept a drop. For example, the right half of the last tab appends to its group. Custom bars own click, closing, keyboard and accessibility behavior, while completed docking moves still use `OnChanged()`.

### Moving panels between windows

Bind each participating `DockLayout` to the same workspace ID and give it a stable, distinct layout ID. The receiving layout handles `DockEvents::TransferRequested`. Use the request's panel and layout IDs to look up both trees and publish one state update. Remove the panel from the source tree and put it in the target tree with `DockTree::Dock()`. An empty `request.target` means the receiving layout is empty, so start it with `DockTree::Tabs()`.

```cpp
return DockLayout(std::move(items), tree)
    .Workspace("editor", layout_id)
    .OnChanged(apply_tree)
    .On<DockEvents::TransferRequested>(move_panel)
    .On<DockEvents::DetachRequested>(open_panel_window);
```

Transfer the panel's declaration along with its placement and rebuild it in the destination. Use shared application state for document contents, drafts, and any selection that should follow the panel. A canceled drag or a rejected request leaves the application state as it was.

`DetachRequested` supplies the panel ID after a confirmed mouse release outside the application. Open the window first, then update the trees if it succeeds. Windows reports the outside release. Windows, macOS, and Linux support mouse transfers between participating windows through application-scoped drag-and-drop. A menu command can open a window on desktop through `OpenWindow()`, or on iOS through `OpenWindowAsync()` in an application task once `CanOpenWindow()` is true.

Build `example_dock_layout` to run the [Harbor Studio example](../../examples/dock_layout/main.cpp). Edit a file, then drag its tab beside another document or onto an editor edge. On Windows, release it outside the application to open a detached window. On desktop, right-click a document tab and choose **Move to new window**. In iOS environments that allow multiple scenes, touch and hold a document tab to open the same menu. On desktop, drag a tab into another participating window to dock it again. The draft follows the document, and an emptied floating window closes. Tool-only groups use the built-in bar. See the [design contract](../design/split-and-docking.md) for sizing and retention details.

## Mounted coordinate spaces

`ViewNode::Bounds()` is the complete node-local layout rectangle in DIPs, including Padding.
`ContentBounds()` is the same rectangle deflated by the resolved Padding and is useful for content drawing and padding-aware hit testing.
`LayoutOffset()` is parent-relative, and `PresentationBounds()` is the node's transformed axis-aligned bound in window DIPs.
Use `LocalToWindow()` and `WindowToLocal()` for point conversion; the inverse returns `std::nullopt` when the resolved presentation transform is not invertible.
`LocalToWindowBounds()` transforms all four rectangle corners and returns their window-space axis-aligned bounds.

The final transform for the current frame is available to `NodeExtension::PrepareGeometry()`.
Earlier extension callbacks may still observe the previously resolved transform, and paint commands remain node-local even when geometry preparation retains a window-space value for a presentation boundary.

To inspect committed layout, open the debug overlay and enable **Layout guides**. It shows resolved padding, gaps and alignment guides in linear layouts, and scroll and clip bounds. Enable `AppOptions::show_debug_overlay` if the overlay is disabled for your build.

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

For edge-to-edge window content, add `SafeAreaPadding` where controls need insets. Use its edge flags to leave some edges for descendants. The modifier adds to ordinary `Padding`. See [Window Insets and System Bars](../design/window-insets.md) for the window-level content modes.

## ScrollView

Use `ScrollView` to make one ordinary content subtree scrollable within a clipped viewport.

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

Use `.FillViewport()` to give short content the viewport's extent along each scroll axis. On an unbounded axis it uses the incoming minimum. Larger content still scrolls normally.

Combine it with `MainAlign(MainAxisAlignment::End)` to place a short transcript at the bottom.

```cpp
return ScrollView(
    Column {
      Transcript(),
    }.With(MainAlign(MainAxisAlignment::End))
).FillViewport();
```

An explicit `ScrollPhysics` modifier configures fling and overscroll behavior for one container.
Without one, the container uses the current platform adapter's default physics.

```cpp
return ScrollView(content).With(ScrollPhysics{
    .fling_enabled = true,
    .overscroll_enabled = true,
});
```

Use `ViewEvents::ScrollInput` when a View needs the wheel or trackpad update before default scrolling, such as a zoomable canvas. Return `true` to consume both components or `false` to let the built-in scroll dispatch handle them. Direct touch dragging uses pointer input. See [Scrolling](../design/scrolling.md) for nested dispatch and motion.

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

Set `refreshing` while your task runs, then clear it to settle the content. A direct touch pull at the leading vertical boundary can request refresh, as can the accessibility Refresh action. A programmatic change to `refreshing` changes presentation without emitting a request. Use `RefreshBoxStyle` to set the indicator and motion for the active Theme. See [RefreshBox design](../design/refresh-box.md) for the overscroll handoff.

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

Use `UseScrollController()` to keep one controller per axis across recomposition. Bind a single-axis view with `.Controller(controller)` and a two-axis view with `.Controller(axis, controller)`. Each bound axis needs a distinct controller.

Observe `Metrics()` in composition to use the clamped offset, viewport extent, content extent, or maximum offset. Use `ScrollTo()` or `ScrollBy()` for offsets and `ScrollToItem()` with a supported virtual layout for item alignment. A disabled container still accepts its controller's requests, which can drive a noninteractive mirrored view.

## VirtualList

Use `VirtualList` for a long sequence. Supply a range or a count and an item factory. The range overload takes a snapshot of the item values. With a count, capture the data the factory needs for its lifetime.

```cpp
return VirtualList(records, [](const Record& record) {
  return RecordRow(record).Key(record.id);
}).EstimatedItemExtent(48.0F).With(Spacing(4.0F));
```

Put a stable key on the factory's returned root when items can move. For item-local hooks, call a composable row function from the factory. Keep deferred factories free of external side effects. If work must continue after an item is evicted, own the task above the list.

The list scrolls vertically by default. Select a horizontal list with `ScrollAxis(Axis::Horizontal)`. Use `ItemExtent()` for items with a known fixed extent, `EstimatedItemExtent()` when the list should refine its sizes, and `CacheExtent()` to tune how much content remains mounted offscreen. See the [virtual list example](../../examples/virtual_list/main.cpp) and the [horizontal list example](../../examples/horizontal_virtual_list/main.cpp).

## VirtualGrid

Use `VirtualGrid` for a vertically scrolling grid in a bounded viewport. Select `GridColumns::Fixed()` for a fixed column count or `GridColumns::Adaptive()` for a minimum column width. Use `RowExtent()` or `EstimatedRowExtent()` for row sizing, and `ItemSpans()` for items that span more than one column. The [virtual grid example](../../examples/virtual_grid/main.cpp) pairs an adaptive grid with variable item heights.

## Custom layout

Derive from `Layout<Derived>` to arrange ordinary children. Measure direct children through `LayoutContext` and place them through `LayoutResult`. Use the returned sizes while computing the layout. The context has already removed the owner's padding from its constraints, so return a content size and place children relative to the content origin.

```cpp
class SimpleRow final : public Layout<SimpleRow> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    LayoutResult result;
    float x = 0.0F;
    float height = 0.0F;
    for (ViewNode& child : node.Children()) {
      const Size size = context.Measure(child, constraints.Loose());
      result.Place(child, {x, 0.0F});
      x += size.width;
      height = std::max(height, size.height);
    }
    return result.SetSize(constraints.Constrain({x, height}));
  }
};
```

Use `LayoutValue<Key>` to pass typed child metadata to a parent policy. Use child references, metadata pointers, and the layout context only for the callback that borrows them.

## Custom virtual layouts

Derive from `VirtualLayout<Derived>` to lay out logical items on demand. Use `context.Viewport()` to choose the indices to request, then obtain each item with `Item()`, measure it, and place it in content coordinates. Return the viewport size with `SetSize()` and the full logical extent with `SetContentSize()`. The single-axis result defaults to vertical. Use `SetAxis()` to select horizontal and implement `ScrollOffsetForItem()` to support item jumps.

For both axes, call the protected `EnableBothAxes()` in the derived constructor. Request only the cells in the two-dimensional viewport and any extra cache range your policy uses. Implement `static std::optional<Rect> ItemBounds(ViewNode&, std::size_t)` to support `ScrollToItem()` without realizing the target. Return content coordinates or an estimate for the unrealized item. Each controller aligns that rectangle along its axis.

Use `SetScrollOffset(float)` for a single-axis correction or `SetScrollOffset(Axis, float)` for a two-axis correction after sizes change. If your policy needs a stable anchor across candidate measurements, derive it from the controllers' committed metrics. The [10,000 × 100 read-only table example](../../examples/virtual_table/main.cpp) demonstrates two-axis realization, item jumps, and size changes.

See [Layout and Virtualization](../design/layout.md) for constraint, placement, and item lifetime contracts.
