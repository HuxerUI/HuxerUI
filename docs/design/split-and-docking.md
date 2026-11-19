# Split and Docking Layouts

`SplitLayout` resizes ordinary panes. `DockLayout` arranges declared panels under a controlled `DockTree`. Use the [pane and workspace guide](../guide/layout.md#resizable-panes) for construction and event handling. Both build on [Layout and Virtualization](layout.md), [Indexed Pages](indexed-pages.md), and [Typed Drag-and-Drop](drag-drop.md).

## Declaration and ownership

The split flattens ordinary child groups into panes and skips empty Views. Adjacent panes receive one themed separator. The separator reserves up to 8 DIPs, uses the resize cursor for its axis, and offers keyboard and semantic adjustment. Pane contents are clipped to their allocations.

`DockTree` is a value that stores regions, panel IDs, tab selection, and Frame/Grow configuration. A tab group requires nonempty unique IDs and a selection belonging to the group. A split requires at least two nonempty regions. IDs are unique across the tree and all referenced panels must have matching declarations. A declared panel may be absent from the tree. A default tree or empty workspace is valid.

`DockTree::With()` applies only `Frame` and `Grow` from left to right. Only populated Frame fields replace existing values, and the last Grow factor wins. It uses the same dimension and factor validation as View declarations. Tree edits return new values. Application code can remove a panel regardless of whether its declaration exposes a close affordance.

## Shared size rules

| Configuration | Owner and extent |
|---|---|
| Frame/Grow on a SplitLayout child | That pane's allocation in the split |
| Frame/Grow on a DockTree child | That logical region's allocation in its parent split |
| Frame on DockItem content | The content's own measurement constraints, not an implicit group configuration |
| Frame/Grow on the DockTree root | DockLayout's allocation in its external parent; subsequent View::With modifiers retain normal precedence |

A logical region can contain a tab group or another split. Its Frame describes the whole region, including the tab bar for a group. Selecting another tab does not replace the group's allocation policy with the selected content's Frame/Grow. Content constraints remain subject to the parent constraints supplied by the region. SplitLayout reads its immediate pane configuration through existing declaration rules rather than searching arbitrary content descendants for a sizing policy.

SplitLayout and DockLayout share a private allocation algorithm:

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

Keyboard and accessibility adjustments use the same proposal path. Separators expose SemanticRole::Separator with the existing range and adjustment actions.

## Docking edits

Dock can move an existing panel or insert an item that is currently absent from the tree. Its target must identify a panel already in the tree, and the final item references are validated by DockLayout. TabBefore and TabAfter insert on the specified side of the target tab and select the inserted panel. Left, Right, Top, and Bottom split beside the target's whole tab group, preserving the original region's external Frame/Grow and starting the inner regions with equal Grow weights. A moved tab does not carry its former group's allocation policy into the destination.

Select activates an existing panel in its current group. Remove does nothing for an absent panel; removing the selected tab chooses its successor, or its predecessor when no successor exists. Empty groups are removed, single-child splits are collapsed, and the surviving region keeps the collapsed position's external Frame/Grow. The workspace may become empty; create its first group with Tabs. Reopening uses an explicit Dock destination rather than hidden previous-position history.

Drag onto either half of a tab to insert before or after it. The empty part of the built-in tab strip appends to that group. A drop near the edge of a panel splits its group, and a drop in the center inserts after its selected tab. Dragging changes only the preview until a successful drop proposes one complete tree edit. Cancellation leaves the tree unchanged, and topology changes invalidate the transfer. Built-in tab context menus expose the same edits through pointer or keyboard commands.

## Tab bar composition

TabBar installs a factory on the View declaration, with DockTree remaining the authoritative, presentation-independent value. The factory runs during composition for each group. It receives a borrowed DockTabBarContext with ordered panel IDs, resolved titles and close flags, and the selected ID. DefaultBar builds the standard bar and its interactions. An empty View omits the group bar and gives the panel the full height. The layout measures a supplied bar under the group's width and measures the selected panel in the remaining height.

BindTabDrag and BindTabDrop attach the same typed drag source and tab drop marker used by the built-in bar. The former is attached to the application-selected grip or label, while the latter uses the actual layout bounds of the designated tab View. Bound Views retain the current panel ID, session, and topology through their event handlers independently of the borrowed context. The custom bar owns focus, activation, closing and accessibility, and only its explicit bindings act as drop targets. The default strip retains its append target. Pane targets, sibling splits, and keyed panel slots are independent of the chosen bar.

## Transfers between layouts

Workspace assigns a nonempty application-local workspace ID and a nonempty layout ID. A different layout can receive a panel when both assignments share a workspace ID, have distinct layout IDs, and the receiver binds DockEvents::TransferRequested. Opted-in drag sources and targets use the existing application-scoped transport. The payload keeps the live source's session identity and its topology at drag start. Drop acceptance checks the current source and destination, and a destination that already contains the panel ID refuses it.

Only the receiver emits TransferRequested. The request names the source, destination, and panel, and optionally describes a DockPlacement relative to a target panel. An empty target requests insertion as the first group in an empty receiving layout. The receiving layout uses the same insertion and split geometry as a move within one layout. Applications revalidate the request against their current workspace model and publish the two new trees and their associated panel declarations together.

Binding DockEvents::DetachRequested opts into application-scoped drag completion for the source. It emits the panel ID only for a confirmed release outside the application, from a source whose topology is still current. The application creates and closes windows through its existing window handles. A successful drop, canceled drag, release within the application, or unconfirmed release produces no detachment request.

## Panel identity and retention

DockLayout keeps all declared panel content under one stable mounted parent, keyed by DockItem ID. The logical DockTree computes rectangles but is not reproduced as a nested mounted tree of SplitLayouts and page containers. Moving a panel therefore does not reparent its mounted content or require globally unique View keys. Group chrome and separators may change independently of panel content. Panel content keeps its ordinary DockLayout ancestry and its own Environment providers; logical groups do not silently introduce new Environment ancestors.

| Change | Content lifetime |
|---|---|
| Move to another region or tab group; change split relationships | Retain compatible mounted content by item ID |
| Select another tab | Keep the inactive content mounted but unplaced |
| Remove an ID from DockTree while retaining its DockItem | Hide the content while retaining its mounted state |
| Remove the DockItem declaration or replace its content incompatibly | Perform normal unmount or replacement |
| Unmount DockLayout | Unmount all panel content |

Only selected, present panel content is measured and placed for the current workspace. Each visible panel uses a bounded content region with clipping so it cannot draw or receive pointer input over adjacent panels. Unplaced content follows the ordinary [layout-participation contract](layout.md#constraints-and-measurement).

All declared panel content is mounted; this is retention rather than lazy creation or eviction. Compatible content retains local State, scroll offsets, and PlatformView identity, subject to ordinary geometry clamping and platform creation rules. Inactive content can still recompose from State updates, and its TaskScope remains alive. Focus, IME, pointer interaction, and scroll momentum follow the existing IndexedPages cleanup rules; active input sessions and automatic focus restoration are not promised across hiding. Application-owned Layers are not implicitly dismissed when a panel becomes inactive. Retained state does not survive removing the item declaration, moving to another DockLayout or UiWindow, or process recreation.

## Mounted implementation

SplitLayout and DockLayout share the split measurement, clipped pane slot, and separator extension. A layout records the separator geometry; its owner publishes it after presentation transforms resolve. SplitLayout reads its pane declarations directly. DockLayout measures logical regions recursively and places tabs, separators, and selected panel slots in tree order under one mounted parent. Its separator callbacks apply sizing changes to the corresponding branch of the latest committed tree.

DockLayout decorates the existing Tabs labels privately, retaining Tabs layout, selection, scrolling, and keyboard navigation. Typed drag-and-drop owns pointer recognition, application transport, and the preview layer. Drop targets live on the tab strip, tab items, clipped pane slots, and the bounds of an empty receiving layout. Panel content uses its ordinary input contract.
