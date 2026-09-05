# Indexed Pages Design

## Purpose

IndexedPages presents one page from an ordered set while retaining every page in the mounted application tree.

It fills the gap between controlled destination selection and history navigation:

- Tabs, NavigationBar, and NavigationPane select a peer destination.
- IndexedPages presents the corresponding retained content.
- NavigationStack owns Push, Pop, route history, Back, and page transitions.

IndexedPages is not a navigation history, swipe pager, Layer, state-restoration store, or generic KeepAlive modifier.

## Public API

The public type lives with the built-in layouts in `<huxerui/view.h>`:

```cpp
class IndexedPages final : public Layout<IndexedPages> {
public:
  IndexedPages(std::initializer_list<View> pages, std::size_t selected_index);
  IndexedPages(std::initializer_list<View> pages, const State<std::size_t>& selected_index);

  IndexedPages(std::vector<View> pages, std::size_t selected_index);
  IndexedPages(std::vector<View> pages, const State<std::size_t>& selected_index);

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints);
};
```

The page declarations precede the required controlled selection, matching Tabs, NavigationBar, NavigationPane, and SegmentedButton:

```cpp
return Column {
  Tabs({"Controls", "Layout", "Motion"}, selected_page)
      .OnChanged([selected_page](std::size_t index) {
        selected_page = index;
      }),
  IndexedPages(
      {
          ControlsPage(),
          LayoutPage(),
          MotionPage(),
      },
      selected_page
  ).With(Grow()),
};
```

Construction rejects an empty page set, an empty View entry, and an out-of-range selected index with `std::invalid_argument` and an English HuxerUI diagnostic.

IndexedPages has no default selection, `.Selected(...)`, or `OnChanged` API.

It does not initiate selection; the application keeps the selected index authoritative and selection controls emit requested changes through their existing typed events.

## Identity and ownership

Every declared page enters the ordinary mounted tree.

IndexedPages owns no RecomposeScope, page factory, state-slot cache, controller, registry, or parallel resolved tree.

Page identity follows normal sibling reconciliation:

- A fixed unkeyed page keeps identity at the same declaration position.
- A keyed page keeps identity when it moves among compatible siblings.
- Duplicate sibling keys remain invalid under the existing View reconciliation contract.
- Changing only the selected index does not change any page identity.
- Removing a page, replacing it with an incompatible View, or unmounting IndexedPages performs ordinary subtree unmount.

This retains ordinary non-serializable local State because the page remains mounted; it does not copy or serialize that State.

## Measurement and geometry

IndexedPages measures and places only the selected child.

With tight constraints it occupies the required size.

With loose constraints its size follows the selected page, constrained by its parent.

Inactive pages retain their mounted nodes and cached state but do not contribute an intrinsic maximum size.

Selecting a page whose measurement is dirty measures that page under the current constraints before it becomes visible.

An equal selection and equal constraints reuse the existing layout cache and do not cause measurement, layout, paint, semantics, or extension invalidation.

IndexedPages switches immediately and adds no cross-page animation.

## Layout participation

IndexedPages must not attach a retained modifier to every page or introduce an IndexedPages branch in Runtime.

Instead, Runtime formalizes the general meaning already expressed by `LayoutResult::Place(...)`:

> A layout child participates in the committed UI only when its parent includes that child in the committed placements.

The internal MountedNode records whether it participates in its immediate parent's committed layout.

Effective participation also requires every ancestor to participate.

The root always participates.

Scope, Environment, ScrollView, SelectionArea, and other transparent or single-child nodes propagate participation to their child.

Ordinary Layout nodes derive direct-child participation from their committed `LayoutResult::Placements()`.

VirtualLayout derives it from realized placements.

This is mounted-tree metadata rather than ViewSpec declaration state, so it needs no public modifier, Environment value, revision object, observer, or component-specific callback.

The rule also makes custom Layout behavior coherent: a measured but unplaced child retains mounted state but does not render or receive input from stale geometry.

NavigationStack continues to place its active page or active transition pair.

Covered history entries outside that set remain mounted but become non-participating through the same rule.

## Inactive page behavior

A non-participating page remains mounted but is absent from the committed UI.

Runtime excludes its subtree from:

- Paint recording and visible RenderScene traversal.
- Pointer hit testing, scroll routing, window drag regions, keyboard focus traversal, and Back routing.
- SemanticFrame construction and accessibility actions.
- Text-input geometry and the active platform text-input session.
- NodeExtension frame callbacks, geometry preparation, and semantic contributions.
- System-bar and custom-title-bar appearance candidates.

Pointer, press, hover, keyboard activation, focus, text selection overlay, and IME state that reference a page becoming inactive are cancelled or cleared through the existing interaction cleanup paths.

Focus is not restored automatically when the page becomes active again.

A controlled TextEditingValue remains application state, including its selection value, but an active platform composition session does not survive page deactivation.

Scroll offset remains in the mounted ScrollView.

Scroll momentum stops when its page becomes inactive rather than continuing invisibly or resuming with stale velocity.

PlatformView mounted nodes keep stable identities.

After a PlatformView has participated, its invisible committed placement keeps the platform instance retained while its page is inactive; a PlatformView on an initially inactive page may defer platform creation until first selection.

ExternalTexture usage disappears from the visible scene and releases that Runtime's visibility subscription until the page participates again.

## Lifecycle, tasks, and recomposition

Page selection is not component mount state.

Switching the selected index therefore does not run Lifecycle cleanup or setup and does not close a page TaskScope.

Tasks may finish while their page is inactive, and their State writes may recompose that page so current data is ready when it is selected again.

Inactive visual NodeExtensions do not keep requesting frames.

Their retained instances resume frame participation when the page becomes active.

IndexedPages does not add `OnAppear`, `OnDisappear`, `OnPageActive`, or a second lifecycle stream.

An application that needs domain behavior on selection changes already owns the controlled selected index.

Application-owned Dialog, BottomSheet, Menu, and other Layers are not implicitly dismissed because page selection is not unmount.

Interaction-owned presentation such as hover Tooltip content closes through ordinary pointer and focus cancellation.

## Scrolling composition

Each page that needs an independent scroll position owns its own ScrollView:

```cpp
IndexedPages(
    {
        ScrollView {
          ControlsContent(),
        }.With(ScrollBar()),
        ScrollView {
          LayoutContent(),
        }.With(ScrollBar()),
        ScrollView {
          MotionContent(),
        }.With(ScrollBar()),
    },
    selected_page
).With(Grow())
```

A ScrollView outside IndexedPages has one mounted offset shared by every selected child.

Replacing its content with a shorter page can correctly clamp that shared offset, so an outer shared ScrollView cannot provide per-page restoration.

## Incremental invalidation

Changing the selected index invalidates IndexedPages layout and the participation of the old and new pages.

The old visible bounds are damaged once, and the new selected page is measured, laid out, painted, and published to semantics once.

The switch must not recreate page scopes, state cells, extensions, PlatformViews, or compatible mounted nodes.

An update inside an inactive page may recompose its declaration, but measurement and paint remain deferred until that page participates.

The Runtime traversal cache for NodeExtensions must distinguish structural extension presence from current participation so a newly selected page resumes extensions without scanning inactive descendants on every frame.

Participation changes reuse generic traversal rules rather than concrete layout-type checks.

## Saveable state boundary

IndexedPages provides in-process mounted retention only.

It does not produce a restoration payload and does not restore state after process recreation or a new application session.

Non-serializable UseState values retained by IndexedPages remain explicitly outside process restoration.

## Unsupported capabilities

The public component does not include:

- Swipe gestures or pager physics.
- Lazy page creation or eviction.
- Cross-fade, slide, shared-element, or scene transitions.
- Maximum-page intrinsic measurement.
- Automatic focus restoration.
- Automatic Layer dismissal.
- A public KeepAlive or visibility modifier.
- Saveable-state codecs or platform restoration adapters.

Heavy or unbounded page collections use conditional composition, VirtualLayout, or application navigation instead of IndexedPages.

## Validation

Focused tests cover:

- Constructor validation and selected-page geometry under window constraints.
- Stable local State and mounted identities across selection changes.
- Pointer routing and absence of inactive-page paint and semantics.
- Independent ScrollView offsets.
- Retained PlatformView identity, invisible placement, event isolation, and resumed visibility.

The UI gallery uses one ScrollView per page so selection changes retain Controls state and independent scroll positions.
