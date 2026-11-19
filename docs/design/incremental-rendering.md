# Incremental Layout and Rendering Design

This document defines the implemented architecture for local measurement, layout, paint, and presentation invalidation in HuxerUI.

## Goals

- Recompute only layout subtrees whose inputs changed.
- Reuse drawing output for unchanged mounted nodes.
- Move, clip, fade, and scroll retained content without recording its drawing commands again.
- Keep component state machines, layout behavior, and rendering data in the shared C++ runtime.
- Keep platform adapters limited to platform lifecycle, scheduling, event conversion, text services, and scene rendering.
- Establish one drawing path for built-in components, retained modifiers, and the `Canvas` component.
- Support conservative damage tracking without requiring components to calculate host-view dirty rectangles.
- Preserve deterministic behavior and make invalidation observable in focused tests.

## Non-goals

- A public application-level API for manually invalidating arbitrary nodes.
- A second component tree or a Flutter-style public `RenderObject` hierarchy.
- Renderer-specific objects in shared Runtime state.
- A general dependency graph.
- GPU layer promotion, occlusion culling, or partial swap-chain submission.
- A public PlatformView composition mode, PlatformView frame, or application-managed render slice.

## Implemented foundation

The current frame path recomposes dirty scopes and reconciles their `ViewSpec` output before visiting the retained layout tree.
Mounted nodes store local bounds, parent-relative offsets, stable RenderNodes, and separate content and foreground paint-dirty state.
Ordinary mounted nodes also retain their input Constraints, measured size, placements, layout revisions, and measure and layout dirty state.
Reconciliation compares built-in layout inputs by value and propagates changed child layout toward the root.
An unchanged subtree reuses its measurement when its Constraints are equal, and an unchanged placement does not revisit its descendants.
Clean PaintSequences are retained across frames; declarative paint input changes, size and focus changes, and explicit NodeExtension invalidation rerecord only the affected sequence.
RenderNodes retain local opacity, and platform renderers composite each node's content, children, and foreground as one group while replaying source-color PaintSequences.
Each PaintSequence has a recording revision, and Runtime compares a lightweight snapshot of the committed scene to derive frame damage without retaining duplicate commands.
`TextField` prepares geometry-dependent scroll state before paint, so paint and platform text-input queries only read committed geometry.

The implementation remains conservative in several areas:

- TextField text and layout-relevant configuration changes invalidate its ancestor layout path; selection-only and composition-marker-only changes invalidate paint without remeasurement.
- Equality-comparable layout values avoid invalidation when their erased values are unchanged; other values invalidate conservatively.
- Ordinary ScrollView offset changes update a retained children transform without rewriting descendant layout offsets.
- Transform-only and opacity-only presentation changes retain PaintSequences.
- Equality-comparable retained modifier values skip unchanged updates when their node inputs are also unchanged; other values update conservatively.
- Geometry-dependent extensions prepare value snapshots after final presentation resolution and invalidate foreground paint only when those snapshots change.
- Windows and macOS invalidate conservative platform update bounds derived from DamageRegion.
- Linux and Android still calculate shared DamageRegion output, but their current toolkit surfaces invalidate and replay the full platform surface.

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
| Platform renderer | Platform resource resolution and scene traversal |

## PlatformView composition foundation

The [PlatformView composition contract](platform-content.md#platformview) defines the shared placement order, candidate commit, and supported scene effects. Slice construction reads retained RenderNodes and PaintSequences. It emits at most one more HuxerUI slice than the number of PlatformViews. Compatible slices reuse their retained platform representation.

Insertion, removal, and reordering invalidate changed boundaries and old and new visible bounds. macOS and Web intersect stable slice invalidation with DamageRegion and redraw the complete composition when sibling composition changes. Android invalidates the complete host View. On Windows, visible removed child HWNDs retire after the replacement surface is presented so the previous aperture remains filled during the commit-to-paint interval.

Properties revisions update the platform instance. Event binding changes leave the property revision and clean PaintSequences intact. ExternalTexture remains an ordinary draw command, with the [texture lifetime and damage rules](platform-content.md#externaltexture) applying across visible destinations.

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
Hit testing, text-input geometry, accessibility, and PlatformView integration map a local point or rectangle through the current ancestor transform chain when they need those coordinates.
The runtime may cache resolved transforms for one committed scene revision, but resolved host-view frames are derived data rather than layout ownership.

The [mounted geometry guide](../guide/layout.md#mounted-coordinate-spaces) describes the public coordinate queries. Geometry preparation sees the resolved presentation transform for the current frame, and recorded paint remains node-local.

## Layout ownership and caching

Mounted nodes retain the last measurement inputs, measured size, child placements, and separate measurement and placement dirty state. `MeasureNode()` reuses a result when the measure flag is clean and the incoming constraints, inherited safe-area insets, and title-bar geometry match exactly. Changes to layout policy, layout-relevant properties, or participating children mark the measurement path dirty. An invalidated ancestor can still reuse measurements from clean descendants.

Unchanged placement skips descendant visits. A changed parent-relative offset advances the node's layout revision for presentation and input without rerecording descendant paint. A new local size dirties the affected node's content and foreground paint. Geometry removed from the retained scene contributes its previous painted bounds to damage. [Layout and Virtualization](layout.md) defines the measurement and placement contracts.

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

Frame requests outside `UiWindow::BuildFrame()` schedule work through the platform's absolute monotonic deadline interface.
A frame request does not mean that layout or paint is dirty.
Invalidation outside frame construction requests a frame so callers cannot leave dirty state unscheduled.

Runtime does not call platform scheduling while `BuildFrame()` is executing.
Paint invalidation raised before recording is consumed by the current frame and does not create redundant follow-up work.
Continuous or delayed work discovered during extension advancement is reported through `FrameResult` and merged into the `FrameCommit::next_frame_deadline` returned with the committed `RenderFrame`.
The platform invalidates and presents platform damage before scheduling that deadline.
This keeps frame production, platform painting, and the next wake-up as one ordered transaction without reentrant scheduling.

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
  std::vector<RenderClip> child_clips;
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
Child clips form a retained stack because a rounded container boundary and a ScrollView content viewport can both constrain the same descendants.

Traversal order is:

```text
node transform and opacity
    -> content record
    -> child clip
    -> children transform
    -> child render nodes
    -> foreground record
```

The content and foreground records preserve the two `NodeExtension` paint phases without adding another retained sequence.
One foreground record per mounted node is sufficient initially.
Per-extension fragments should be added only if profiling shows that rebuilding several foreground extensions on one node is material.

`RenderNode::visible` is a subtree traversal gate rather than an own-bounds flag.
Runtime derives a node's own visibility from its recorded content and foreground PaintSequence bounds, not from its layout bounds.
It remains true when local paint or an unclipped descendant contributes visible output even if the node's layout bounds are outside the viewport.
An effective child clip can still make that descendant and therefore the clipped subtree invisible.

`PaintSequence` contains immutable platform-neutral `PaintCommand` values in node-local coordinates.
`PaintCommand` remains the vocabulary for rectangles, text, circles, lines, arcs, Paths, borders, clips, transforms, shadows, external textures, and PlatformView placement boundaries.
The retained scene changes command ownership; it does not create a renderer-specific command model.

Platform renderers traverse `RenderScene`, maintain the platform transform and clip stacks, and replay only the records referenced by the scene.
They may cache platform text, brush, path, or image resources by record and command revision.

## PaintContext and pure paint

All drawing is recorded through `PaintContext`:

```cpp
class PaintContext {
public:
  Rect Bounds() const;
  void DrawRect(Rect rect, Color color, CornerRadii corner_radii = {});
  void DrawText(Rect rect, std::string text, TextStyle style, TextLayoutOptions options = {});
  void DrawTextRun(
      Rect bounds,
      Point baseline_origin,
      std::string text,
      TextStyle style,
      TextShapingOptions shaping = {});
  void PushClip(Rect rect, CornerRadii corner_radii = {});
  void PopClip();
};
```

The exact public surface follows the available `PaintCommand` set.
Paragraph text owns a layout rectangle, while exact text runs own an already measured visual bound and baseline origin.
Renderers may shape a run into platform glyphs but must not replace its supplied geometry with a second text measurement.
`PaintContext` owns command balancing validation and records local bounds for damage calculation.
It rejects non-finite geometry, colors, transforms, and negative dimensions, radii, or stroke widths at the recording boundary with `std::invalid_argument`.
Arc start and sweep angles are expressed in radians.
Its transform and clip stacks are reflected in those bounds, so the recorded rectangle conservatively contains the pixels produced by replay.
`PaintContext::Bounds()` supplies the owning node's local layout bounds as a geometry reference; Canvas instead receives its Padding-deflated content bounds with a `(0, 0)` origin.
Neither form clips drawing to that rectangle.
Dirty sequences are recorded before visibility is resolved, allowing extensions, shadows, and Canvas primitives to paint beyond layout bounds correctly.
Built-in nodes, both `NodeExtension` paint phases, and the `Canvas` component use this same API.

Paint callbacks are pure:

- They may read committed layout geometry and retained visual state.
- They may append commands only to the supplied context.
- They do not mutate scroll offsets, selection, animation state, caches that affect behavior, or frame scheduling.
- They do not query or store host-view coordinates.
- Replaying a cached record has the same visible result as invoking paint again with the same committed inputs.

Geometry preparation that can change state occurs before paint.
`NodeExtension::PrepareGeometry()` receives a borrowed `TextMeasurer` for synchronously preparing labels and other text-dependent geometry. Extensions may retain the returned metrics or immutable layouts on the UI thread.
It may also convert local geometry through the final resolved transform when publishing a value snapshot to a window-owned boundary such as anchored-layer placement.
For example, `TextField` resolves text layout, caret geometry, selection geometry, and horizontal scroll adjustment after layout and before paint recording.
If preparation changes a visual value, it marks the relevant paint record dirty.
Text-input geometry is then mapped to the platform coordinate system from the committed local geometry.

## NodeExtension invalidation

`NodeExtension` remains behavior attached to one mounted node.
It does not become a render-tree or plugin abstraction.

Retained extensions need a restricted way to report changes once paint is cached.
Protected extension operations provide that capability:

```cpp
protected:
  void InvalidatePaint(PaintInvalidation invalidation = PaintInvalidation::Foreground);
```

The runtime binds this operation to the extension's mounted owner for the extension lifetime.
It is unavailable to ordinary application code and does not expose `Runtime`.
Calling it invalidates the owning content PaintSequence, foreground PaintSequence, or both according to the supplied phase.
Outside frame construction it also schedules a frame; during frame construction the current recording pass consumes the invalidation.

`FrameResult` remains responsible for continuous-frame and delayed-wake timing.
It does not double as a dirty-state carrier, and paint invalidation does not double as an extension frame scheduler.
Built-in presentation modifiers update retained transform or opacity state while advancing and return the appropriate scheduling result.
An indication whose retained visual changes calls `InvalidatePaint()` for the phase or phases that contain it.

`NodeExtension::PaintBehindContent` records after the normal background and before the resolved border and node content.
`NodeExtension::PaintAboveContent` records after descendants and before the framework-owned focus ring.
Both callbacks receive local `Bounds()` and `PaintContext`; neither receives a flat frame display list or depends on inherited transforms already having been emitted.

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

Virtual offset changes dirty the viewport input and rerun the policy without dirtying the measurements of stable realized items. An unplaced subtree retains pending viewport invalidation until it participates again. That invalidation stops at the layout-participation boundary. The [virtual layout commit](layout.md#virtual-geometry-and-offset-corrections) determines the placements realized for the frame.

## Interaction, text input, and layers

Hit testing traverses mounted and render geometry together:

- Reject outside clips in parent-local coordinates.
- Apply the inverse presentation transform before testing a child.
- Test foreground extensions in reverse declaration order where current interaction semantics require it.
- Preserve the existing enabled, pointer-capture, focus, and scroll-routing rules.

Focus and IME sessions are not paint state.
After the final layout and presentation commit, Runtime refreshes focused text-input geometry only when the client synchronization revision, focused-node layout revision, or node-to-host transform changed.
Text clients report geometry in node-local logical coordinates; Runtime converts it to host-view coordinates before crossing the platform boundary.

Runtime layer entries participate in the same layout and render scene as application content.
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
resolve and publish the immutable SemanticFrame
record dirty content, foreground, and system-overlay PaintSequences
compute conservative damage
publish FrameCommit with the committed RenderFrame, SemanticFrame, and optional next deadline
invalidate and present the platform damage
schedule the returned deadline
```

The platform receives the scene after frame construction completes.
Frame construction mutates retained Runtime state in place, so exceptions are not a transactional recovery boundary and callers must not assume that the previous scene remains published.

`BuildFrame()` propagates the original exception. The built-in hosts use `BuildFrameOrErrorPage()` to retire the failed window content and commit an independent error page for composition failures, including virtual item composition. Content replacement ends the active text-input session with `ClientRemoved`. Actual window retirement uses `RuntimeDestroyed`. Failures outside the composition boundary and failures building the error page still propagate to the host.

The conceptual platform boundary is:

```cpp
struct RenderFrame {
  RenderScene scene;
  DamageRegion damage;
  std::uint64_t revision;
};

struct FrameCommit {
  RenderFrame render_frame;
  std::shared_ptr<const SemanticFrame> semantic_frame;
  std::optional<double> next_frame_deadline;
};
```

`RenderScene` remains valid until the next frame construction or UiWindow retirement, while the immutable `SemanticFrame` may be retained across later commits.
The platform must finish synchronous traversal before returning unless it explicitly retains a versioned immutable snapshot.

## Damage tracking

Damage is derived from retained scene changes:

- A changed paint record damages the union of its old and new transformed paint bounds.
- A presentation change damages the union of the old and new transformed subtree bounds.
- Removing a render node damages its previous transformed bounds.
- Adding a render node damages its new transformed bounds.
- Clip changes damage the conservative union of the affected old and new clipped subtree bounds.

Each PaintCommand contributes conservative visual bounds, including its stroke width where applicable.
Shadow commands include their resolved caster and complete blur overflow.
Unknown or renderer-dependent overflow falls back to the host viewport.

The Android View backend currently ignores regional damage at the platform invalidation boundary and redraws the full surface.
The Linux GTK backend currently lets GTK coalesce invalidation and snapshots the committed RenderScene into ordered Cairo and GDK texture nodes for each widget draw.
Other future platform implementations may initially redraw the full surface.
The shared runtime must still calculate and test damage correctly so a renderer can adopt partial redraw without changing component behavior.

Runtime retains a lightweight snapshot of node identities, PaintSequence revisions, world transforms, effective clips, child order, and transformed bounds from the last committed frame.
It does not copy PaintCommands.
The first frame and every viewport-size change report full damage.
Static frames report an empty region.
Paint, presentation, clipping, insertion, removal, and stacking-order changes contribute their conservative old and new bounds, and touching or overlapping rectangles are merged.
Non-finite bounds fall back to full viewport damage.

## Public and platform boundaries

Applications write declarations and use borrowed `ViewNode` geometry in layout or extension callbacks. UiWindow owns invalidation, retained RenderNodes, and scene lifetime. `BuildFrame()` returns a committed `RenderFrame` with scene and damage data for platform presentation.

## Required invariants

- Layout geometry is always parent-relative.
- Paint commands are always node-local.
- Paint callbacks are pure.
- Scene identity is stable across compatible reconciliation.
- Scheduling and invalidation are distinct.
- Layout changes do not invalidate descendant paint merely because their window position changed.
- Presentation changes do not invoke layout.
- Platform adapters do not branch on concrete components or modifiers.
- Every platform backend handles every `PaintCommand` explicitly; the shared `RenderComposition` path consumes PlatformView placement before raster replay.

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
