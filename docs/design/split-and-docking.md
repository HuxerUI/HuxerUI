# Split and Docking Layouts

Status: SplitLayout is implemented. DockLayout, DockTree, DockItem, and DockEvents remain approved future API and are not available in the current SDK.

This design adds controlled pane resizing and single-window workspace rearrangement while preserving the existing View, modifier, event, and mounted-node contracts. The existing foundations are [Layout and Scrolling](../guide/layout.md), [Indexed Pages](indexed-pages.md), [Typed Drag-and-Drop](drag-drop.md), and [Semantics and Accessibility](semantics.md).

## Scope and ownership

| Public type | Responsibility |
|---|---|
| `SplitLayout` | Arrange ordinary child Views along one axis with draggable separators |
| `DockLayout` | Present and rearrange a controlled workspace |
| `DockTree` | Store split relationships, tab groups, selection, and Frame/Grow configuration |
| `DockItem` | Declare one panel's stable ID, title, content, and close affordance |

SplitLayout supports dynamic pane counts and nesting. DockLayout adds tab ordering, grouping, splitting, panel removal from the layout, and explicit reopening within one UiWindow. An overlapping bottom panel remains ordinary Stack composition; a bottom region that takes space from its neighbors belongs in a vertical split.

Floating native windows, cross-window transfers, automatic hiding at workspace edges, implicit reopening history, and a serialized layout format are outside this design. There is no workspace Runtime service, controller, global panel registry, content factory, or new size-unit type.

## SplitLayout API

The declarations show the public surface; implementation members and forwarding bodies are omitted. All names belong to `huxerui`.

```cpp
struct SplitChange {
  std::size_t index = 0;
  Frame frame{};
  Grow grow{0.0F};
};

struct SplitEvents {
  struct Changed : Event<void(std::vector<SplitChange>)> {};
};

class SplitLayout final : public detail::TypedView<SplitLayout> {
public:
  explicit SplitLayout(std::vector<View> children);
  SplitLayout(std::initializer_list<View> children);

  template <class... Children>
    requires((detail::ViewChild<Children> && ...) &&
             !(sizeof...(Children) == 1 && (std::same_as<std::remove_cvref_t<Children>, SplitLayout> && ...)))
  explicit SplitLayout(Children&&... children);

  SplitLayout Orientation(Axis axis) &&;

  template <class Function>
  SplitLayout OnChanged(Function&& function) &&;
};
```

Orientation defaults to Horizontal. Construction accepts ordinary View children and Views groups using the existing collection rules; empty Views do not create panes. Zero or one pane creates no separator, and N panes create N-1 adjacent separators. Dynamic stateful children use ordinary stable sibling keys. OnChanged delegates to SplitEvents::Changed.

The split fills a bounded main axis and stretches panes across the available cross axis; Frame constraints remain subject to the allocated parent rectangle. Separators reserve 8 DIPs each, shrinking when the parent cannot fit that space. They draw a themed line and expose the appropriate resize cursor, focus, arrow-key adjustment in 10-DIP increments, and Home/End adjustment to the feasible limits. Pane contents are clipped to their allocations.

SplitChange is an output proposal, not another size configuration type. Its index identifies a nonempty pane in the current flattened declaration order, excluding internal separator nodes. Each entry contains the complete proposed Frame/Grow configuration for that pane, and one callback delivers all entries needed for the adjustment. The event is delivered synchronously against that declaration order; applications must not apply a saved index batch to a later reordered child set.

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

## Docking API

This section describes future API.

```cpp
enum class DockPlacement {
  Tab,
  Left,
  Right,
  Top,
  Bottom,
};

class DockTree final {
public:
  Frame frame{};
  Grow grow{0.0F};

  DockTree();

  static DockTree Tabs(
      std::vector<std::string> panels,
      std::string selected = {}
  );
  static DockTree Split(Axis axis, std::vector<DockTree> children);

  template <class... Modifiers>
    requires(
        (std::same_as<std::remove_cvref_t<Modifiers>, Frame> ||
         std::same_as<std::remove_cvref_t<Modifiers>, Grow>) && ...
    )
  DockTree With(Modifiers&&... modifiers) &&;

  bool Empty() const noexcept;
  bool Contains(std::string_view panel) const;

  std::span<const std::string> Panels() const noexcept;
  std::string_view Selected() const noexcept;
  std::optional<Axis> SplitAxis() const noexcept;
  std::span<DockTree> Children() noexcept;
  std::span<const DockTree> Children() const noexcept;

  DockTree Select(std::string_view panel) const;
  DockTree Remove(std::string_view panel) const;
  DockTree Dock(
      std::string panel,
      std::string_view target,
      DockPlacement placement
  ) const;

  bool operator==(const DockTree&) const;
};

class DockItem final {
public:
  DockItem(std::string id, StringVariant title, View content);

  DockItem Closable(bool closable) &&;
};

struct DockEvents {
  struct Changed : Event<void(DockTree)> {};
};

class DockLayout final : public detail::TypedView<DockLayout> {
public:
  DockLayout(std::vector<DockItem> items, DockTree tree);
  DockLayout(std::vector<DockItem> items, const State<DockTree>& tree);

  template <class Function>
  DockLayout OnChanged(Function&& function) &&;
};
```

DockTree is a copyable, equality-comparable value containing no View, native handle, or mutable interaction state. Editing a copy must not change the original tree. Select, Remove, and Dock return new values, and OnChanged delegates to DockEvents::Changed. The public fields and mutable Children span support ordinary State::Update edits without introducing setters or a controller. Spans and string views are borrowed from their tree and must not survive a mutation or replacement that invalidates their storage.

A default tree is empty. Tabs requires at least one nonempty, unique panel ID; an omitted selected value chooses the first panel, and an explicit selection must belong to that group. Split requires at least two nonempty subtrees, with panel IDs unique across the complete tree. Panels and Selected return empty results outside a tab group; Children returns an empty span outside a split, and SplitAxis returns nullopt outside a split.

DockItem IDs are unique within one DockLayout, independent of labels and declaration positions. Every tree reference must resolve to a declared item, while a declared item may be absent from the tree. DockItem requires nonempty content; Closable defaults to true and only controls user-facing close operations, not application edits to the tree. An empty tree and an empty item list are supported.

Factories validate their inputs, and DockLayout validates the complete tree and item set before mounting or accepting a replacement declaration, including values edited through public fields. Invalid caller configuration uses std::invalid_argument with an English HuxerUI diagnostic; internal invariant failures use std::logic_error.

## Declarative construction

DockTree::With is restricted to the existing Frame and Grow types at compile time. It applies values from left to right, updates only populated Frame fields, and uses the last Grow value. It shares the existing Frame/Grow merge and validation logic, including finite nonnegative dimensions and factors and valid minimum/maximum pairs. An empty Frame does not clear earlier fields; direct value editing can reset an optional field explicitly.

This is a layout-data construction operation, not View inheritance or a general modifier pipeline. It installs no ModifierDescriptor, NodeExtension, event binding, or mounted state, and it does not write State automatically. The supported modifier set differs from View::With even though Frame/Grow retain the same meanings.

```cpp
auto tree = UseState(
    DockTree::Split(
        Axis::Horizontal,
        {
            DockTree::Tabs({"files", "search"})
                .With(Frame{.width = 240.0F, .min_width = 160.0F}),
            DockTree::Tabs({"editor"})
                .With(Grow(), Frame{.min_width = 320.0F}),
            DockTree::Tabs({"inspector"})
                .With(Frame{.width = 280.0F, .min_width = 180.0F}),
        }
    )
);

return DockLayout(
    {
        DockItem("files", "Files", FilesPanel()),
        DockItem("search", "Search", SearchPanel()),
        DockItem("editor", "Editor", EditorPanel()).Closable(false),
        DockItem("inspector", "Inspector", InspectorPanel()),
        DockItem("terminal", "Terminal", TerminalPanel()),
    },
    tree
).OnChanged([tree](DockTree next) {
  tree = std::move(next);
});
```

## Shared size rules

| Configuration | Owner and extent |
|---|---|
| Frame/Grow on a SplitLayout child | That pane's allocation in the split |
| Frame/Grow on a DockTree child | That logical region's allocation in its parent split |
| Frame on DockItem content | The content's own measurement constraints, not an implicit group configuration |
| Frame/Grow on the DockTree root | DockLayout's allocation in its external parent; subsequent View::With modifiers retain normal precedence |

A logical region can contain a tab group or another split. Its Frame describes the whole region, including the tab bar for a group. Selecting another tab does not replace the group's allocation policy with the selected content's Frame/Grow. Content constraints remain subject to the parent constraints supplied by the region. SplitLayout reads its immediate pane configuration through existing declaration rules rather than searching arbitrary content descendants for a sizing policy.

SplitLayout uses a private allocation algorithm that DockLayout will reuse:

- Deduct separator space before distributing the main-axis extent.
- A zero Grow factor uses the preferred main-axis Frame dimension, or natural size when that dimension is absent.
- A positive Grow factor receives a weighted share of finite remaining space; a preferred main-axis Frame dimension is not added as a second allocation base.
- Frame minimum and maximum bounds constrain allocation when feasible; items reaching a bound leave the remaining-space redistribution until other eligible items receive their shares.
- With no Grow items, or when every Grow item reaches its maximum, remaining space can stay unused.
- If available space cannot satisfy the requested bounds, parent constraints take precedence and actual extents remain finite and nonnegative.
- An unbounded main axis preserves natural sizing, does not expand Grow items, and disables separator resizing.
- An unbounded cross axis measures panes at their allocated main extents before selecting the shared natural cross extent, preserving wrapped text and aspect-dependent content sizes.

Window resizing and temporary constraint clamping do not overwrite preferred Frame/Grow values or emit Changed. This design does not redefine Row or Column allocation; the split-specific constraint and drag algorithm reuses the existing size meanings without introducing another public size model.

## Controlled resize proposals

A separator adjustment starts from committed visible geometry and changes its two adjacent regions within the feasible range while preserving other regions' actual geometry where feasible. The allocation algorithm produces a proposal that can reproduce the requested geometry when accepted, rather than copying every measured dimension into persistent configuration. Non-growing items primarily update frame.width or frame.height, and growing items primarily update grow.factor. Other same-level configuration may require coordinated conversion, so the entire required change set is delivered together. Minimum/maximum bounds and cross-axis fields remain unchanged.

SplitLayout emits the affected SplitChange entries in one callback; DockLayout applies the same calculation to a proposed DockTree and emits that tree once. These are atomic proposals, not a new transaction mechanism for arbitrary application State writes. Applications provide the next authoritative values through normal State assignment; rejecting a proposal leaves the declared allocation unchanged. Initial measurement, programmatic updates, and window resizing never synthesize user Changed events.

When positive growth cannot reproduce an adjustment, such as collapsing to zero or shrinking a capped Grow pane while leaving surplus space unused, the proposal can set that pane's Grow factor to zero and its preferred main-axis Frame dimension to the requested extent. Applications should write back both fields of every affected entry rather than assume a pane's sizing mode never changes.

Pointer movement uses stable split-container coordinates derived from window coordinates, not accumulated local deltas on a moving separator. Ordinary acceptance of the preceding proposal preserves the drag session. A cross-axis extent or translation change does not invalidate a drag when the mapping from window coordinates to the main axis is unchanged. Child topology or orientation changes, disablement, removal, window retirement, or invalidation of the drag's geometry/configuration baseline cancel the operation before a stale proposal can be delivered. Cancel and Escape stop future updates without rolling back changes already accepted by the application.

Keyboard and accessibility adjustments use the same proposal path. The design adds SemanticRole::Separator and reuses existing range and adjustment actions; each supported accessibility bridge must map the new role. Panel moving and grouping must also have keyboard-accessible commands rather than requiring physical dragging.

## Docking edits

```cpp
tree = tree->Dock("terminal", "editor", DockPlacement::Bottom);
tree = tree->Dock("terminal", "editor", DockPlacement::Tab);
tree = tree->Remove("inspector");
```

Dock can move an existing panel or insert an item that is currently absent from the tree. Its target must identify a panel already in the tree, and the final item references are validated by DockLayout. Tab inserts after the target within its group and selects the inserted panel. Left, Right, Top, and Bottom split beside the target's whole tab group, preserving the original region's external Frame/Grow and starting the inner regions with equal Grow weights, subject to their bounds. A single moved tab does not carry its former group's allocation policy into the destination.

Select activates an existing panel in its current group. Remove does nothing for an absent panel; removing the selected tab chooses its successor, or its predecessor when no successor exists. Empty groups are removed, single-child splits are collapsed, and the surviving region keeps the collapsed position's external Frame/Grow. The workspace may become empty; create its first group with Tabs. Reopening uses an explicit Dock destination rather than hidden previous-position history.

Tab dragging changes only the preview until a successful drop proposes one complete tree edit. Cancellation leaves the tree unchanged, and incompatible topology changes invalidate the drag target. Closing, tab selection, resizing, grouping, and dropping all use DockEvents::Changed, with the application free to reject a proposal.

## Panel identity and retention

DockLayout keeps all declared panel content under one stable mounted parent, keyed by DockItem ID. The logical DockTree computes rectangles but is not reproduced as a nested mounted tree of SplitLayouts and page containers. Moving a panel therefore does not reparent its mounted content or require globally unique View keys. Group chrome and separators may change independently of panel content. Panel content keeps its ordinary DockLayout ancestry and its own Environment providers; logical groups do not silently introduce new Environment ancestors.

| Change | Content lifetime |
|---|---|
| Move to another region or tab group; change split relationships | Retain compatible mounted content by item ID |
| Select another tab | Keep the inactive content mounted but unplaced |
| Remove an ID from DockTree while retaining its DockItem | Hide the content while retaining its mounted state |
| Remove the DockItem declaration or replace its content incompatibly | Perform normal unmount or replacement |
| Unmount DockLayout | Unmount all panel content |

Only selected, present panel content is measured and placed for the current workspace. Each visible panel uses a bounded content region with clipping so it cannot draw or receive pointer input over adjacent panels. The existing LayoutResult participation contract excludes unplaced subtrees from drawing, hit testing, focus traversal, semantics, and active visual frame work. No change to ViewNode::Children, sibling key matching, or generic layout participation is required.

All declared panel content is mounted; this is retention rather than lazy creation or eviction. Compatible content retains local State, scroll offsets, and PlatformView identity, subject to ordinary geometry clamping and platform creation rules. Inactive content can still recompose from State updates, and its TaskScope remains alive. Focus, IME, pointer interaction, and scroll momentum follow the existing IndexedPages cleanup rules; active input sessions and automatic focus restoration are not promised across hiding. Application-owned Layers are not implicitly dismissed when a panel becomes inactive. Retained state does not survive removing the item declaration, moving to another DockLayout or UiWindow, or process recreation.

## Implementation boundaries and validation

SplitLayout lives in `src/components/split_layout.cpp`, using a private layout and NodeExtension behavior. Its scope retains the geometry and event destination; keyed pane slots and private unkeyed separator layouts remain under one mounted parent. Each slot clips its content independently of content presentation transforms, forwarding the declaration key so compatible content retains its mounted identity when panes move. Sizing reads the slot's immediate content configuration and measures through the slot; the slot introduces no alternative Frame/Grow policy. Layout measurement and geometry preparation publish data without emitting resize events. DockLayout will reuse allocation and separator behavior; typed drag-and-drop provides recognition, transfer, preview, and cancellation. Docking supplies HuxerUI-owned drop regions without making a PlatformView itself a DropTarget. Existing Tabs cannot supply per-tab drag sources through its current public API; reuse appropriate private tab behavior and Theme values without adding a general tab factory or a parallel tab-selection engine. The flattened mounted structure must still expose logical group semantics and a coherent focus order through existing semantics facilities.

Before implementation is considered complete, validate:

- Declarative Frame/Grow merging and validation, including compile-time rejection of unsupported DockTree modifiers and independent tree copies.
- Empty, single-pane, dynamic, nested, horizontal, and vertical splits; duplicate or unresolved panel IDs and invalid tree edits.
- Mixed preferred/natural sizes and multiple Grow items, minimum/maximum saturation, insufficient space, and unbounded constraints.
- Replay of an accepted resize proposal reproducing its target geometry, including coordinated weight updates, without rewriting configuration merely because the window resized.
- Rejected proposals, normal controlled writeback, keyboard and semantic adjustment, Escape, pointer Cancel, disablement, topology changes, and retirement.
- Stable mounted identities, local State, scroll offsets, and PlatformViews across grouping, splitting, hiding, reopening, and keyed declaration reordering.
- Absence of inactive paint, input, focus, semantics, and visual frame work; continued Task completion and deterministic cleanup on actual unmount.
- Drop preview isolation, explicit clipping, focus/semantic order, and cancellation over ordinary content and PlatformViews.

Future DockLayout implementation must also update public header checks, umbrella exports, owning SDK guides, affected examples, build/packaging metadata, generated module exports, and available platform accessibility mappings through the existing repository workflows. The user guides describe implemented SplitLayout behavior; the docking contracts above remain future design.
