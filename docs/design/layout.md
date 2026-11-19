# Layout and Virtualization

This document defines the contracts for measurement, placement, parent layout metadata, and virtual item lifetime. See [Layout and Scrolling](../guide/layout.md) for usage and [Scrolling](scrolling.md) for input and offset ownership.

## Constraints and measurement

Each parent supplies minimum and maximum outer dimensions in DIPs. A finite maximum bounds that axis, an infinite maximum leaves it unbounded, and equal limits make it tight. Runtime intersects `Frame` limits with the parent constraints and clamps a preferred dimension into the result. If the limits conflict, the nearest parent limit wins.

Runtime removes resolved `Padding` from the outer constraints before it calls the layout policy. Resolved padding includes the safe-area edges the node consumes. The policy returns a content size and placements relative to the content origin. Runtime adds padding back to the size and child origins. `LayoutContext::Measure()` supplies each child's outer size under its own modifiers and updates `LayoutSize()`. Contexts and child references are borrowed for the measurement call.

The returned `LayoutResult` decides which direct children participate. Measuring a child alone leaves it unplaced. An unplaced child can remain mounted, with its composition state and lifetime intact, while it is excluded from drawing, input, focus traversal, and semantics. Moving a participating child changes its parent-relative offset. Ancestor presentation transforms resolve window geometry after placement.

See [Incremental Layout and Rendering](incremental-rendering.md#layout-ownership-and-caching) for measurement reuse and invalidation.

## Parent layout metadata

`Grow` and `LayoutPriority` use the typed layout-value channel. A Scope exposes its own value when present, then the effective value of its single composed child. An explicit zero therefore overrides a child value. An Environment boundary exposes its child's value. Lookup crosses only those transparent boundaries, and the declarations remain on their original nodes. `Frame` and `Padding` retain node-local ownership. `Spacing` and alignment describe the policy of the layout that declares them.

## Linear allocation

With a bounded main axis, ordinary `Row` and `Column` allocate remaining space to positive `Grow` factors after accounting for the other children and spacing. They measure growing children directly in their allocated main size when the layout does not stretch them, or when stretching has a tight cross extent. Under loose stretching constraints they first resolve the natural cross size. An unbounded main axis uses natural child sizes.

A finite main-axis maximum and at least one explicit `LayoutPriority` select priority allocation. Missing priorities equal zero. The policy starts from measured child sizes. If they fit, `Grow` divides the surplus subject to `Frame` maxima and redistributes space after a child reaches its cap. If they do not fit, the policy reserves each child's minimum up to its measured size and distributes the rest in descending priority order. Children tied in priority share in proportion to their measured size above that reserved minimum. Children remeasure against their assigned extent. Compression can change wrapped text height, so a stretched row can resolve the cross extent again. Overcommitted minima can still overflow the clamped container.

See [Split and Docking Layouts](split-and-docking.md#shared-size-rules) for the allocation policy of resizable panes.

## Virtual item lifecycle

A `VirtualLayout` policy receives a candidate viewport, chooses logical indices, and requests items from `VirtualLayoutContext`. Placed items must have been requested in that session and can appear only once in the result. The policy supplies content coordinates while UiWindow owns identity, the mounted children, clipping, scroll state, interaction, and semantics. Collection semantics describe committed placements without creating additional mounted items.

The factory is deferred and has no active item composition scope of its own. It returns a nonempty View declaration and may return a Scope from a composable function. Runtime reconciles item declarations under the owner's effective Environment. Declaration-time dependencies attach to the declaring scope. When a compatible source is reapplied, the owner clears its deferred declarations and captured dependencies so the factory can see the latest inputs. Built-in measured extents survive this refresh while the geometry inputs to the extent cache stay compatible.

One realization session shares requested items across candidate passes. It matches compatible nodes by root key when present and by logical index otherwise. Duplicate requested keys fail. The final placements determine which items remain mounted. Items can leave the viewport and remain mounted within the policy's cache range. Items omitted from placement unmount, retiring mounted extensions and composition lifetimes.

On eviction the owner can save compatible `UseState` slots by key or index. A later return restores those slots into a newly mounted scope. The mounted node, effects, tasks, focus, and extension instances have their ordinary mount and unmount lifetimes. On source changes out-of-range indexed state is pruned. Keyed state can survive a temporary removal from the source while the same virtual owner stays mounted.

## Virtual geometry and offset corrections

`VirtualList` needs a bounded scroll axis. `VirtualGrid` needs bounded width and height and virtualizes rows vertically. Fixed extents avoid estimation. For variable extents, the policies retain metrics, use estimates for unmeasured items, and update measured extents from realized items. Anchoring uses the logical position and the intra-item or intra-row offset. A grid track change can map the previous logical anchor to a different row.

A single-axis policy reports its axis and any scalar correction in its result. A two-axis policy opts in before measurement and requires bounded dimensions. It returns the viewport size and full content extent. For each axis, UiWindow resolves pending item alignment before a proposed layout correction, clamps against the measured content, and retries within the same session when the viewport changes. Failure to settle within eight passes is a layout invariant error.

Candidate offsets stay separate from the authoritative offset. Placement applies and clamps the correction for the chosen viewport, publishes controller metrics, and subtracts that same offset from the item content origins. This keeps two-axis offsets and realized cells aligned in the committed frame. Clean frames retain realization and placements. A viewport change reruns the virtual policy while stable item measurements can still hit the ordinary layout cache.

### Tree snapshots

TreeView owns one Scope that reads the roots, item-info provider, and synchronous Children snapshots for the expanded structure.
It invokes the row factory for each visible logical item in the expanded tree, including rows offscreen, and reads the returned root View's key without executing a nested Scope.
Keys are unique among logical siblings and are internally qualified by parent identity before rows enter the flat virtual layout.
Unkeyed rows use sibling position; moving a keyed row to another parent creates a new identity.
The snapshot retains those declarations, resolved labels, controlled values, parent relationships, and event dispatch captures.
Scrolling requests retained declarations and executes only realized row Scope bodies; it never repeats business factories or Children traversal until the tree Scope is invalidated.
Local hooks in row content belong in a composable function or explicit Scope, since the raw callbacks do not create a per-item composition lifetime.

The tree layout uses fixed row extents and a bounded vertical viewport.
Structural changes preserve the committed top item's identity and intra-row offset, falling back to its surviving parent or neighbor when it disappears.
An explicit scroll change after that committed geometry takes precedence over anchor restoration in the next layout.
A row containing the focused control remains realized outside the cache window; removing its logical branch releases that control and restores navigation focus to the tree.
Snapshot and semantic-frame work grow with the number of logical rows in the expanded tree. Mounting, nested composition, measurement, and drawing are limited to the viewport, cache range, and focused row.

## Validation

[Layout tests](../../tests/runtime/layout.cpp) cover constraint resolution, metadata through Scope and Environment, priority compression, and measurement reuse. [Virtual layout tests](../../tests/runtime/virtual_layout.cpp) cover windowed realization, state restoration, extent refinement, and same-frame two-axis corrections. [TreeView tests](../../tests/runtime/tree.cpp) cover snapshotting, anchors, and row focus.
