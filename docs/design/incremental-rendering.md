# Incremental Layout and Rendering Design

Status: shared Runtime pipeline implemented; native partial redraw implemented on macOS and Windows

This document defines the implemented architecture for local measurement, layout, paint, and presentation invalidation in HuxerUI.
It intentionally removes the legacy absolute-frame and flat-DisplayList runtime contracts.

## Goals

- Recompute only layout subtrees whose inputs changed.
- Reuse drawing output for unchanged mounted nodes.
- Move, clip, fade, and scroll retained content without recording its drawing commands again.
- Keep component state machines, layout behavior, and rendering data in the shared C++ runtime.
- Keep platform adapters limited to native lifecycle, scheduling, event conversion, text services, and scene rendering.
- Establish one drawing path for built-in components, retained modifiers, and a future `Canvas` component.
- Support conservative damage tracking without requiring components to calculate host-view dirty rectangles.
- Preserve deterministic behavior and make invalidation observable in focused tests.

## Non-goals

- A public application-level API for manually invalidating arbitrary nodes.
- A second component tree or a Flutter-style public `RenderObject` hierarchy.
- Renderer-specific objects in shared Runtime state.
- A general dependency graph in the first implementation.
- GPU layer promotion, occlusion culling, or partial swap-chain submission in the first implementation.
- Encoding future `NativeView` or external-surface behavior before its composition rules are defined.

## Implemented foundation

The current frame path recomposes dirty scopes and reconciles their `ViewSpec` output before visiting the retained layout tree.
Mounted nodes store local bounds, parent-relative offsets, stable RenderNodes, and separate content and foreground paint-dirty state.
Ordinary mounted nodes also retain their input Constraints, measured size, placements, layout revisions, and measure and layout dirty state.
Reconciliation compares built-in layout inputs by value and propagates changed child layout toward the root.
An unchanged subtree reuses its measurement when its Constraints are equal, and an unchanged placement does not revisit its descendants.
Clean PaintSequences are retained across frames; declarative paint input changes, size and focus changes, and explicit NodeExtension invalidation rerecord only the affected sequence.
RenderNodes retain local opacity, and platform renderers composite each node's content, children, and foreground as one group while replaying source-color PaintSequences.
Each PaintSequence has a recording revision, and Runtime compares a lightweight snapshot of the committed scene to derive frame damage without retaining duplicate commands.
`TextField` prepares geometry-dependent scroll state before paint, so paint and native text-input queries only read committed geometry.
`VirtualLayout` retains its committed realization and placements on clean frames.
Viewport, source, constraints, and realized-child layout changes rerun its policy, while stable realized items reuse their cached measurements.

The implementation remains conservative in several areas:

- TextField text and layout-relevant configuration changes invalidate its ancestor layout path; selection-only and composition-marker-only changes invalidate paint without remeasurement.
- Equality-comparable layout values avoid invalidation when their erased values are unchanged; other values invalidate conservatively.
- Ordinary ScrollView offset changes update a retained children transform without rewriting descendant layout offsets.
- Transform-only and opacity-only presentation changes retain PaintSequences.
- Equality-comparable retained modifier values skip unchanged updates when their node inputs are also unchanged; other values update conservatively.
- Geometry-dependent extensions prepare value snapshots after final presentation resolution and invalidate foreground paint only when those snapshots change.
- macOS and Windows invalidate conservative native update bounds derived from DamageRegion.
- Android still calculates shared DamageRegion output, but the View backend invalidates and replays the full native surface because current Android View invalidation ignores dirty rectangles.

## Current pipeline

The implemented data flow is:

```text
State and Environment changes
    -> dirty RecomposeScope
    -> ViewSpec diff
    -> MountedNode reconciliation
    -> local layout geometry
    -> retained RenderScene
    -> platform renderer
```

The mounted tree and render scene have different responsibilities:

| Structure | Responsibility |
|---|---|
| `ViewSpec` | Transient declarative component output |
| `MountedNode` | Reconciliation identity, retained behavior, state coordination, layout inputs, and invalidation |
| Local layout geometry | Measured size and parent-relative placement |
| `RenderNode` | Retained drawing records and presentation properties |
| Platform renderer | Native resource resolution and scene traversal |

There is one `RenderNode` per paintable mounted node.
Non-painting structural nodes may either own an empty render node or be elided when doing so does not change clipping, transforms, opacity, hit testing, or stable scene identity.
The implementation should prefer a predictable one-to-one mapping initially and optimize structural-node elision only after measurement.

## Local coordinate model

Layout geometry is stored relative to the parent:

```cpp
struct LayoutGeometry {
  Size size;
  Point offset;
};
```

`size` is the node's constrained layout size.
`offset` is the node's placement in its parent's local coordinate system.
The node's local bounds are always:

```cpp
Rect{
    .origin = {},
    .size = geometry.size,
};
```

Layout policies measure children and return parent-relative child placements.
They never assign host-view coordinates.
Changing an ancestor's placement therefore does not invalidate descendant measurement or paint records.

Window and screen coordinates are boundary queries.
Hit testing, text-input geometry, accessibility, and native-view integration map a local point or rectangle through the current ancestor transform chain when they need those coordinates.
The runtime may cache resolved transforms for one committed scene revision, but resolved host-view frames are derived data rather than layout ownership.

The public mounted-node geometry API should use unambiguous names:

```cpp
Size LayoutSize() const;
Rect Bounds() const;
Point LayoutOffset() const;
Rect PresentationBounds() const;
```

`Bounds()` is local.
`LayoutOffset()` is parent-relative.
`PresentationBounds()` is the transformed axis-aligned host-view logical bound intended for diagnostics and native-boundary queries.
The ambiguous absolute `Frame()` contract is removed rather than retained as an alias.

## Layout ownership and caching

Each mounted node retains:

- Its most recent input `Constraints`.
- Its measured size.
- Its child placements.
- Revisions for layout-affecting declarative inputs.
- Whether its own layout or a descendant layout is dirty.

A cached layout result is reusable when:

- The node is not layout dirty.
- Its input constraints equal the cached constraints.
- Its layout policy identity is unchanged.
- Its layout-affecting style revision is unchanged.
- Its child structure and relevant child layout values are unchanged.
- No measured child has invalidated the result.

A layout invalidation propagates toward ancestors because a changed child size may change every ancestor that consumes it.
Propagation stops at the root or at a future explicit layout boundary whose size contract is independent of descendant measurement.
The initial implementation does not infer such boundaries automatically.

When an ancestor is visited, unchanged children can still return their cached measurement for identical constraints.
This keeps the propagation rule conservative without forcing complete subtree measurement.

Layout output contains the node size and parent-relative child placements.
Committing a new output compares it with the previous output:

- A size change invalidates the parent's cached layout.
- A child-offset change updates that child's render-node offset and hit-test transform.
- An unchanged child size and offset leave its layout and paint caches intact.
- A removed child contributes its previous paint bounds to damage before its render node is detached.

The layout protocol should continue to expose child measurement through `LayoutContext`.
It should not expose child internals or allow layouts to retain child references across reconciliation.

## Invalidation model

Invalidation is internal runtime state represented as a mask:

```cpp
enum class Invalidation : std::uint8_t {
  None = 0,
  Layout = 1 << 0,
  Paint = 1 << 1,
  Presentation = 1 << 2,
  Interaction = 1 << 3,
};
```

Composition invalidation remains owned by `RecomposeScope`.
It is not another mounted-node bit.
Reconciliation classifies the resulting changes into the narrowest mounted-node invalidations.

| Invalidation | Typical causes | Propagation | Result |
|---|---|---|---|
| Layout | Padding, frame constraints, text metrics, child structure, layout values | Mark the node and ancestors | Recompute affected layout outputs |
| Paint | Color, text content, border, indication state, caret visibility | Mark the owning paint record | Record local drawing commands again |
| Presentation | Offset animation, scale, rotation, opacity, scroll transform | Mark the owning render node | Update retained scene properties |
| Interaction | Enabled state, focusability, pointer policy, hit-test-affecting geometry | Mark the affected route or index | Refresh interaction metadata |

Invalidation dependencies are explicit:

- Layout invalidation implies paint invalidation only when the node's own drawing depends on its size.
- Layout placement changes imply presentation and interaction updates, not descendant paint invalidation.
- Paint invalidation updates paint bounds and therefore may produce damage.
- Presentation invalidation produces damage from the union of old and new transformed paint bounds.
- Interaction invalidation does not imply paint unless the visual state also changed.

The current conservative rule marks the node's own content and foreground records after its size changes, but it does not mark descendant records.

Frame requests outside `Runtime::BuildFrame()` schedule work through the platform's absolute monotonic deadline interface.
A frame request does not mean that layout or paint is dirty.
Invalidation outside frame construction requests a frame so callers cannot leave dirty state unscheduled.

Runtime does not call platform scheduling while `BuildFrame()` is executing.
Paint invalidation raised before recording is consumed by the current frame and does not create redundant follow-up work.
Continuous or delayed work discovered during extension advancement is reported through `FrameResult` and merged into the `FrameCommit::next_frame_deadline` returned with the committed `RenderFrame`.
The platform invalidates and presents native damage before scheduling that deadline.
This keeps frame production, native painting, and the next wake-up as one ordered transaction without reentrant scheduling.

## Reconciliation and declarative diffs

Reconciliation must compare retained inputs by their behavioral impact instead of overwriting every mounted field and treating the whole frame as changed.

The internal style representation is divided by ownership:

```cpp
struct LayoutStyle;
struct PaintStyle;
struct InteractionStyle;
```

This split is internal and does not require separate public modifier syntax.
Public modifiers still apply left to right to `ViewSpec`, while reconciliation compares the resulting groups.

The comparison rules are:

- Node kind, key, and layout-policy incompatibility replace the mounted node.
- Child insertion, removal, or movement invalidates the parent layout.
- A changed `LayoutStyle` invalidates layout.
- A changed `PaintStyle` invalidates the node's paint record.
- A changed `InteractionStyle` invalidates interaction metadata and paint only when its visual resolution changes.
- A changed presentation modifier updates retained extension state and invalidates presentation or paint according to that modifier's implementation.
- Unchanged type-erased values do not invalidate their owner.

`LayoutValue<Key>` and retained modifier values therefore need equality retained through type erasure when their value type is equality comparable.
Values without a usable equality operation use conservative invalidation.
This fallback preserves correctness without requiring every third-party type to provide a hash or revision protocol.

## Retained render scene

The flat frame-owned DisplayList is replaced at the runtime-to-platform boundary by a retained tree.
A conceptual render node is:

```cpp
struct RenderNode {
  std::uint64_t id;
  Point offset;
  Transform2D transform;
  float opacity;
  std::optional<RenderClip> child_clip;
  Transform2D children_transform;
  PaintSequence content;
  std::vector<const RenderNode*> children;
  PaintSequence foreground;
  bool visible;
  std::uint64_t revision;
};
```

This is an implementation model, not a required public layout.
Storage may be embedded in `MountedNode` or owned by `RenderScene` as long as identity is stable and platform adapters never observe dangling nodes.

Traversal order is:

```text
node transform and opacity
    -> content record
    -> child clip
    -> children transform
    -> child render nodes
    -> foreground record
```

The foreground record preserves the current `NodeExtension` foreground-painting behavior.
One foreground record per mounted node is sufficient initially.
Per-extension fragments should be added only if profiling shows that rebuilding several foreground extensions on one node is material.

`RenderNode::visible` is a subtree traversal gate rather than an own-bounds flag.
Runtime derives a node's own visibility from its recorded content and foreground PaintSequence bounds, not from its layout bounds.
It remains true when local paint or an unclipped descendant contributes visible output even if the node's layout bounds are outside the viewport.
An effective child clip can still make that descendant and therefore the clipped subtree invisible.

`PaintSequence` contains immutable platform-neutral `PaintCommand` values in node-local coordinates.
`PaintCommand` remains the vocabulary for rectangles, text, circles, arcs, borders, clips, transforms, shadows, paths, and future primitives.
The retained scene changes command ownership; it does not create a renderer-specific command model.

Platform renderers traverse `RenderScene`, maintain the native transform and clip stacks, and replay only the records referenced by the scene.
They may cache native text, brush, path, or image resources by record and command revision.

## PaintContext and pure paint

All drawing is recorded through `PaintContext`:

```cpp
class PaintContext {
public:
  Rect Bounds() const;
  void DrawRect(Rect rect, Color color, float corner_radius = 0.0F);
  void DrawText(Rect rect, std::string text, Color color, float font_size, TextAlign align);
  void PushClip(Rect rect, float corner_radius = 0.0F);
  void PopClip();
};
```

The exact public surface follows the available `PaintCommand` set.
`PaintContext` owns command balancing validation and records local bounds for damage calculation.
It rejects non-finite geometry, colors, transforms, and negative dimensions, radii, or stroke widths at the recording boundary with `std::invalid_argument`.
Arc start and sweep angles are expressed in radians.
Its transform and clip stacks are reflected in those bounds, so the recorded rectangle conservatively contains the pixels produced by replay.
`PaintContext::Bounds()` supplies the owning node's local layout bounds as a geometry reference; it does not clip drawing to that rectangle.
Dirty sequences are recorded before visibility is resolved, allowing extensions and future Canvas or shadow primitives to paint beyond layout bounds correctly.
Built-in nodes, `NodeExtension::Paint`, and the future `Canvas` component use this same API.

Paint callbacks are pure:

- They may read committed layout geometry and retained visual state.
- They may append commands only to the supplied context.
- They do not mutate scroll offsets, selection, animation state, caches that affect behavior, or frame scheduling.
- They do not query or store host-view coordinates.
- Replaying a cached record has the same visible result as invoking paint again with the same committed inputs.

Geometry preparation that can change state occurs before paint.
For example, `TextField` resolves text layout, caret geometry, selection geometry, and horizontal scroll adjustment after layout and before paint recording.
If preparation changes a visual value, it marks the relevant paint record dirty.
Text-input geometry is then mapped to the native coordinate system from the committed local geometry.

## NodeExtension invalidation

`NodeExtension` remains behavior attached to one mounted node.
It does not become a render-tree or plugin abstraction.

Retained extensions need a restricted way to report changes once paint is cached.
Protected extension operations provide that capability:

```cpp
protected:
  void InvalidatePaint();
```

The runtime binds this operation to the extension's mounted owner for the extension lifetime.
It is unavailable to ordinary application code and does not expose `Runtime`.
Calling it invalidates the owning foreground PaintSequence.
Outside frame construction it also schedules a frame; during frame construction the current recording pass consumes the invalidation.

`FrameResult` remains responsible for continuous-frame and delayed-wake timing.
It does not double as a dirty-state carrier, and paint invalidation does not double as an extension frame scheduler.
Built-in presentation modifiers update retained transform or opacity state while advancing and return the appropriate scheduling result.
An indication whose ripple geometry changed calls `InvalidatePaint()`.

`NodeExtension::Paint` receives local `Bounds()` and `PaintContext`.
It does not receive a flat frame display list or depend on inherited transforms already having been emitted.

## Presentation and scrolling

Presentation properties live on retained render nodes:

- Parent-relative layout offset.
- Animated translation, scale, and rotation.
- Opacity.
- Clip.

Updating these properties does not record content again.
The renderer composes them during scene traversal, and hit testing uses the same transform chain in reverse.

A `ScrollView` owns a viewport RenderNode with a local clip and a children transform derived from the scroll offset.
Changing the offset updates that transform without affecting the viewport's content or foreground.
The retained child layout outputs and PaintSequences remain unchanged.

Scrolling can still require other work:

- A virtual layout may realize or retire items when the visible range changes.
- Scroll bars and overscroll indications may invalidate their own paint or presentation state.
- Text-input visibility handling may adjust a scroll ancestor and therefore its content transform.

These changes remain local to their owners.

## Virtual layout

The public `VirtualLayoutContext` protocol continues to combine policy measurement with item requests.
The runtime commits its result in three stages:

- The virtual policy resolves container size, item metrics, and the requested item range.
- The realization session reconciles that range and retires items no longer requested.
- `LayoutNode` applies the committed parent-relative placements.

Clean virtual nodes reuse the committed policy result, realization, and placements.
Scrolling marks only the virtual viewport input dirty and reruns the policy and realization session without invalidating measurements of stable realized items.
New items enter with layout and paint dirty.
Retired items contribute their previous bounds to damage and preserve saveable state according to the existing virtual-state rules.

Policy-specific realization remains inside the virtual-layout subsystem.
Runtime does not branch on concrete virtual-list or grid types.

## Interaction, text input, and layers

Hit testing traverses mounted and render geometry together:

- Reject outside clips in parent-local coordinates.
- Apply the inverse presentation transform before testing a child.
- Test foreground extensions in reverse declaration order where current interaction semantics require it.
- Preserve the existing enabled, pointer-capture, focus, and scroll-routing rules.

Focus and IME sessions are not paint state.
After the final layout and presentation commit, Runtime refreshes focused text-input geometry only when the client synchronization revision, focused-node layout revision, or node-to-host transform changed.
Text clients report geometry in node-local logical coordinates; Runtime converts it to host-view coordinates before crossing the platform boundary.

LayerHost entries participate in the same layout and render scene as application content.
Independent layer scopes can recompose and invalidate without repainting the application subtree.
Modal hit testing, focus restoration, and captured Environment behavior remain Runtime responsibilities.

## Frame transaction

A frame is one ordered transaction:

```text
apply State invalidations
recompose dirty scopes
reconcile changed ViewSpec subtrees
advance scroll motion
measure dirty layout paths using cached child results
realize dirty virtual ranges while measuring
commit local sizes and placements
refresh enabled, focus, and interaction state
advance retained extensions and prepare geometry-dependent visual state
update RenderNode presentation properties
bring focused text input into view and commit any resulting scroll geometry
refresh the text-input session
record dirty content, foreground, and system-overlay PaintSequences
compute conservative damage
publish FrameCommit with the committed RenderFrame and optional next deadline
invalidate and present the native damage
schedule the returned deadline
```

The platform receives the scene after frame construction completes.
Frame construction mutates retained Runtime state in place, so exceptions are not a transactional recovery boundary and callers must not assume that the previous scene remains published.

The conceptual platform boundary is:

```cpp
struct RenderFrame {
  RenderScene scene;
  DamageRegion damage;
  std::uint64_t revision;
};

struct FrameCommit {
  RenderFrame render_frame;
  std::optional<double> next_frame_deadline;
};
```

`RenderScene` remains valid until the next frame construction or Runtime destruction.
The platform must finish synchronous traversal before returning unless it explicitly retains a versioned immutable snapshot.

## Damage tracking

Damage is derived from retained scene changes:

- A changed paint record damages the union of its old and new transformed paint bounds.
- A presentation change damages the union of the old and new transformed subtree bounds.
- Removing a render node damages its previous transformed bounds.
- Adding a render node damages its new transformed bounds.
- Clip changes damage the conservative union of the affected old and new clipped subtree bounds.

Each PaintCommand contributes conservative visual bounds, including its stroke width where applicable.
Future overflow primitives such as shadows must include their complete visual extent; unknown or renderer-dependent overflow falls back to the host viewport.

The Android View backend currently ignores regional damage at the native invalidation boundary and redraws the full surface.
Other future platform implementations may initially do the same.
The shared runtime must still calculate and test damage correctly so a renderer can adopt partial redraw without changing component behavior.

Runtime retains a lightweight snapshot of node identities, PaintSequence revisions, world transforms, effective clips, child order, and transformed bounds from the last committed frame.
It does not copy PaintCommands.
The first frame and every viewport-size change report full damage.
Static frames report an empty region.
Paint, presentation, clipping, insertion, removal, and stacking-order changes contribute their conservative old and new bounds, and touching or overlapping rectangles are merged.
Non-finite bounds fall back to full viewport damage.

## Implemented public and platform API changes

This design introduced the following coordinated breaking changes:

- Replace ambiguous mounted-node absolute geometry with local geometry queries.
- Replace `NodeExtension::Paint(const MountedNode&, DisplayList&)` with local `PaintContext` recording.
- Replace the flat `Runtime::BuildFrame()` DisplayList result with a committed RenderFrame containing `RenderScene` and `DamageRegion`.
- Change every platform renderer from flat command iteration to retained scene traversal.
- Keep `PaintCommand` as the platform-neutral immutable drawing primitive.

These changes migrated public headers, Runtime, all current renderers, focused tests, examples that implement extensions, and rendering documentation together.
No old-name aliases or parallel rendering entry points are retained.

The common component API remains declarative.
Applications do not receive render-node handles, call invalidation methods, or manage scene lifetimes.

## Required invariants

- Layout geometry is always parent-relative.
- Paint commands are always node-local.
- Paint callbacks are pure.
- Scene identity is stable across compatible reconciliation.
- Scheduling and invalidation are distinct.
- Layout changes do not invalidate descendant paint merely because their window position changed.
- Presentation changes do not invoke layout.
- Platform adapters do not branch on concrete components or modifiers.
- Every renderer handles every `PaintCommand` explicitly.

## Implemented adoption sequence

Implemented stages:

- Make every built-in and extension paint path pure, including `TextField`.
- Store mounted geometry in local coordinates while retaining full-frame layout and flat display-list output.
- Introduce `PaintContext`, `PaintSequence`, `RenderNode`, `RenderScene`, and `RenderFrame`, then migrate all available platform renderers.
- Retain clean content and foreground PaintSequences, publish stable node revisions, and expose protected NodeExtension paint invalidation.
- Calculate DamageRegion from committed scene snapshots, including paint, presentation, clipping, insertion, removal, and child-order changes.
- Retain local opacity in RenderNode and composite each node's content, children, and foreground as one platform group.
- Make ordinary ScrollView scrolling update a retained children transform without rewriting child placement.
- Add layout input caches and conservative ancestor invalidation.
- Add equality-aware retained modifier and layout-value diffs.
- Retain clean virtual policy, realization, and placement state, and reuse stable item measurements while scrolling.
- Consume DamageRegion as native update bounds on macOS and Windows, while Android uses the same committed scene with full View invalidation.

The migration stages defined in this document are implemented.

Canvas, box shadows, page transitions, and embedded native views should build on this foundation rather than introduce competing rendering or invalidation paths.

## Validation

Focused runtime tests must cover:

- Unchanged siblings are not measured or painted after a local declarative change.
- A child size change remeasures every necessary ancestor and no unrelated subtree.
- Moving an ancestor preserves descendant paint records.
- Scrolling a realized non-virtual subtree updates only presentation.
- Virtual scrolling realizes and retires the correct items without remeasuring stable items.
- Paint invalidation rebuilds the correct content or foreground record.
- Ripple, caret, selection, focus ring, and scroll-bar animation schedule and invalidate independently.
- Stable system overlays retain their PaintSequences until geometry or visual state changes.
- Old and new bounds produce conservative damage for paint, movement, clipping, insertion, removal, and child-order changes.
- Hit testing and IME rectangles use the same committed transform chain as rendering.

Instrumentation used by these tests belongs in test support or internal debug counters.
It is not part of the application API.

Every adoption stage runs the full common test suite and all platform builds available on the development host.
Scene-boundary changes require renderer audits for Android, macOS, and Windows even when a platform cannot be built locally.
