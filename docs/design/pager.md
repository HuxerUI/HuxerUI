# Pager Design

## Purpose

Pager presents one controlled page from an ordered retained set and supports animated programmatic selection and direct one-page dragging.

It complements rather than replaces the existing page containers:

- IndexedPages retains peer pages and switches immediately without owning input or animation.
- Pager retains peer pages and adds direct paging plus controlled transition presentation.
- NavigationStack owns route history, Push, Pop, Back, and navigation transitions.

Pager is not a navigation history, virtualized collection, restoration store, generic scroll container, or carousel policy surface.

## Public contract

Pager lives in `<huxerui/view.h>` and requires a nonempty page set plus an in-range controlled selected index.

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
    .DragEnabled(true)
    .OnChanged([selected_page](std::size_t index) {
      selected_page = index;
    });
```

The application remains authoritative for selection.
Direct interaction emits at most one adjacent `PagerEvents::Changed` proposal after release.
If the application commits it, Pager settles to the new page; if it does not, Pager returns to the committed page.
Changing the selected index programmatically animates without emitting `Changed`.

`ScrollAxis` selects horizontal or vertical paging.
`DragEnabled(false)` disables Pager's direct pointer gesture without disabling controlled changes or accessibility paging.
`Reverse` is an additional direction inversion after natural layout direction is resolved.
The current layout-direction model resolves horizontal paging as left-to-right; a future inherited layout direction composes with `Reverse` rather than changing its meaning.

Pager intentionally has no uncontrolled constructor, controller, public progress State, page-style object, configurable snap thresholds, wheel mapping, or alternate horizontal and vertical types.

## Mounted ownership

Pager is one ordinary ScrollView-kind mounted node with a private layout descriptor.
It creates no RecomposeScope, page factory, Runtime branch, registry, or parallel page tree.

Every page remains an ordinary reconciled child.
Stable keys, local State, Lifecycle, TaskScope, NodeExtension identity, TextField editing values, PlatformView instances, and ExternalTexture declarations follow normal mounted reconciliation.

One private retained extension on the Pager root owns only transient paging state:

- The last displayed controlled index.
- Direct-drag and release phase.
- The adjacent page participating in the current drag.
- Normalized settle animation progress.
- The current viewport extent and local slot coordinates.

The declaration's selected index remains authoritative and is never replaced by extension state.

## Layout and presentation

The paging axis must be bounded because one page extent equals the viewport extent.
An unbounded paging-axis measurement is a framework usage error.

At rest Pager measures and places only the selected page.
It still publishes enough private content extent for the existing scroll recognizer to determine whether a previous or next page is available.

After direct movement selects a direction, Pager measures and places the committed page and that adjacent page.
Programmatic transitions measure and place only their source and target pages, including non-adjacent index changes without traversing intermediate pages.
Unplaced pages retain mounted state but remain excluded through the generic layout-participation contract.

Slots are local to the current transition rather than absolute page-index coordinates.
When the slot set changes, Pager preserves displacement relative to the source page and remaps it into the new slots.
This prevents accepted drags, rejected drags, reverse paging, and arbitrary programmatic jumps from producing a first-frame position discontinuity.
Viewport changes remap the same normalized displacement or animation progress to the new page extent.

The existing ScrollView child transform, clip, damage, hit testing, PlatformView placement, and RenderScene paths present the transition.
No Pager-specific PaintCommand or renderer behavior exists.

## Input and nested scrolling

Pager participates in the existing typed pointer arbitration and nested scroll transaction on its configured axis.
Orthogonal movement remains available to an ancestor scroll container.
Once Pager wins a same-axis direct sequence, pointer delivery, cancellation, disable, and unmount follow the existing scroll-recognizer ownership path.

The mounted scroll state has a generic allowed-source policy.
Ordinary ScrollView accepts all existing scroll sources.
Pager accepts only direct drag through its scroll offset; wheel, trackpad, scrollbar, controller, focus reveal, drag-and-drop auto-scroll, and generic momentum do not silently become free-distance paging.

Pager consumes release velocity through its retained extension and chooses at most one adjacent target from displacement and velocity.
The settle animation uses the current Theme motion duration and resolves immediately under reduced motion.
Pager does not retain overscroll displacement.

## Semantics

The Pager owner publishes ScrollView semantics and the total page count.
Each page declaration receives component-owned collection-item metadata and selected state while author semantics remain authoritative.
During tentative drag, only the current authoritative page remains interactive and visible to semantics.
After a controlled selection changes, the incoming authoritative page owns interaction and semantics while the outgoing page remains renderable for its transition.
The inactive transition peer uses the same local interaction-disable principle as a covered NavigationStack page and does not acquire a parallel application subtree.

The retained extension owns the Pager Scroll semantic action and converts its axis direction into one adjacent controlled selection proposal.
It does not expose the private slot offset as an application scroll controller.

## Incremental invalidation

Equal selected index and configuration reuse the current measurement, layout, paint, semantics, and extension state.
Starting a drag invalidates layout once when its adjacent page becomes known.
Animation frames change the retained child transform without recomposing page declarations.
Completing a transition rebases the private slots and invalidates layout once so the new selected page becomes the sole participant.

Changing axis, direction, or page count cancels incompatible transient geometry and returns to the current controlled selection.
Disabling direct drag during an active gesture returns to the controlled page without interrupting a programmatic transition.

## Unsupported behavior

Pager does not currently provide lazy page creation, page eviction, wheel paging, keyboard page shortcuts, automatic focus reveal across pages, overscroll effects, infinite wrapping, variable page extents, or public transition progress.
These capabilities require separate product contracts rather than additional flags on the initial component.
