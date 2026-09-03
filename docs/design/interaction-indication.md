# Interaction and Indication Design

This document defines the implemented ownership and public contract for transient interaction state, indication visuals, resource-backed and gradient fills, and their retained paint ordering.

Runtime owns interaction recognition and ordered interaction edges. Retained indication consumes that state without becoming another input recognizer.
The shared gesture-recognition and competition model is specified separately in [Gesture Recognition and Arbitration Design](gestures.md).

## Goals

- Keep hit testing, gesture arbitration, activation, focus, and cancellation in the shared Runtime.
- Give each mounted node one authoritative transient interaction state.
- Preserve ordered Press, Release, and Cancel events for multi-pointer and ripple behavior.
- Let built-in and third-party retained visuals consume the same interaction contract.
- Let normal backgrounds and interaction layers use colors, gradients, image resources, image assets, and vector assets.
- Keep Theme and modifier values copyable, equality-comparable, and suitable for retained reconciliation.
- Preserve incremental paint boundaries between backgrounds, node content, children, and foreground feedback.

## Non-goals

- Interaction state does not own controlled component values such as checked, selected, text, validation, or progress.
- The design does not introduce an `InteractionSource`, observer registry, callback subscription convention, or indication factory hierarchy.
- Arbitrary Views and function painters are not Theme fill values.
- `ExternalTexture` is not a Theme fill because it has a producer lifetime and independent frame scheduling.
- Gradient focus rings and border modifiers remain deferred even though Canvas and Vector path strokes support gradients.
- InteractionState does not add a generic dragged flag; Drag lifecycle is reported by its typed events and consumers retain any operation-specific state.
- State-driven foreground replacement remains component-specific because a node cannot reliably recolor arbitrary descendant content.
- Unbounded effects remain custom `NodeExtension` behavior; built-in ripple is clipped to its indication geometry.
- Platform high-contrast adaptation remains an Environment and Theme concern rather than another interaction state.

## Interaction facts

`InteractionState` is a coalescing snapshot of the current mounted-node facts:

```cpp
struct InteractionState {
  bool enabled = true;
  bool hovered = false;
  bool focused = false;
  bool focus_visible = false;
  bool pressed = false;

  bool operator==(const InteractionState&) const = default;
};
```

The state intentionally excludes selected, checked, error, loading, and other component-owned values.

`pressed == false` is not one visual state.

An unpressed node may still be hovered, focused, disabled, selected, or checked, so the framework does not define an `unpressed` visual or treat it as the inverse of Press.

Ordered interaction edges accompany the snapshot:

```cpp
struct InteractionEvent {
  enum class Type {
    Press,
    Release,
    Cancel,
  };

  enum class Source {
    Pointer,
    Keyboard,
  };

  Type type = Type::Press;
  Source source = Source::Pointer;
  std::uint64_t press_id = 0;
  std::optional<Point> position;

  bool operator==(const InteractionEvent&) const = default;
};
```

Pointer Press position is node-local.

Keyboard interaction has no synthetic spatial coordinate; a ripple whose Press position is empty starts from the center of the resolved indication geometry.

Each Press receives an identifier unique for the Runtime lifetime, and its Release or Cancel carries the same `press_id`.

The event remains necessary when the snapshot does not change, such as a second simultaneous Press while `pressed` is already true.

## Runtime ownership

MountedNode owns the effective enabled, hover, focus, focus-visible, and aggregate pressed facts.

The existing effective `enabled`, `focused`, and `focus_visible` fields move into the mounted interaction state instead of remaining parallel facts.

The mounted storage remains direct rather than introducing another interaction wrapper:

```cpp
InteractionState interaction;
std::uint32_t active_press_count = 0;
```

`interaction.pressed` is derived from `active_press_count != 0`, while ordered Press sessions and their identifiers remain private Runtime input state.

`local_enabled` remains separate because it is declarative input, while effective enabled state includes the parent chain.

`applies_disabled_appearance` remains separate because it identifies the boundary that applies disabled subtree appearance rather than reporting whether the node accepts input.

MountedNode stores the interaction-resolved surface values directly:

```cpp
std::optional<Border> resolved_border;
CornerRadii resolved_corner_radii;
```

These values are initialized from the node's normal or disabled appearance when declaration or disabled-boundary input changes. `IndicationExtension` then publishes the currently presented border and corner-radii transition during geometry preparation without dirtying an otherwise stable frame.

They remain separate from declarative View properties without introducing a wrapper type for two mounted paint facts.

PointerSession retains one active interaction owner containing its node identity and `press_id` independently from the ordinary pointer-event target.

A pointer target starts the interaction for ordinary Click and pointer-event surfaces.

When no ordinary target exists, an extension gesture that accepts Observe or Capture may establish the interaction on its owning node.

Runtime produces exactly one terminal Release or Cancel for every accepted Press.

Scroll arbitration, native cancellation, replacement of an existing pointer ID, disabling, focus loss for a keyboard press, subtree deactivation, and unmount all terminate the corresponding interaction through the same path.

Click activation remains a separate semantic decision made after Release and only occurs when the release satisfies the target's activation rules.

For an ordinary pointer target, Runtime applies an interaction edge in this order:

```text
arbitrate input target
update the mounted interaction snapshot
deliver OnInteraction with the updated snapshot and ordered event
deliver the specialized pointer or key callback
perform semantic activation when its release rule succeeds
```

This keeps the snapshot authoritative while preserving low-level input delivery and the distinction between Release and Click.

An extension can establish Observe or Capture only by handling its initial pointer event. Runtime therefore publishes that extension-owned Press immediately after recognition, then preserves the same snapshot-before-callback ordering for all later events and emits exactly one terminal event through the shared pointer-session path.

Pointer sessions and the keyboard activation session retain their own active Press identities.

Multiple pointer Press events increment the mounted count, and every terminal event decrements only its matching `press_id`.

Runtime submits each complete next snapshot through one internal `UpdateInteraction` operation rather than mutating mounted fields and separately synchronizing consumers.

The operation stores the authoritative snapshot before notifying retained extensions. It also publishes a supplied ordered event when the aggregate snapshot is unchanged, as required by simultaneous Press sessions.

Disabling or deactivating a subtree cancels matching Press sessions before publishing the inactive effective interaction state, so extensions never observe an enabled transition that leaves a stale active Press behind.

Unmount ends the mounted ownership immediately and discards its transient interaction and indication state; it does not call an extension after that extension's lifetime has ended.

## NodeExtension integration

NodeExtension gains one retained interaction callback:

```cpp
virtual void OnInteraction(MountedNode& node, const InteractionState& state,
                           const std::optional<InteractionEvent>& event);
```

The callback receives the state after the event has been applied.

Runtime calls it for a changed snapshot and for every ordered event, including an event that leaves the aggregate snapshot unchanged.

The callback does not emit a typed View event and does not cause recomposition.

An extension invalidates only its retained paint when the update changes visible output.

Extension-specific hover hit testing remains available for behavior whose geometry differs from the node interaction surface, such as Tooltip and ScrollBar.

Standard indication no longer interprets `OnPointer`, `OnKey`, `OnFocusChanged`, or extension-specific `OnHover` callbacks.

Adding `Indication` to a View without Click, pointer events, or an accepting gesture does not make that View an input target or block a target behind it.

A newly mounted or replaced extension receives its first snapshot only after `RefreshInteractionTree` has resolved inherited enabled state, focus, focus-visible state, and hover for the current tree.

An implementation-only pending-sync flag on the extension entry prevents an early callback with an incomplete default snapshot without adding another observer or binding abstraction.

Compatible extension updates can read `MountedNode::Interaction()` immediately and preserve their retained animation state.

Ordinary node hover follows the deepest interactive hit target and is recomputed from the last pointer position after tree or layout changes.
Public Hover handlers independently receive direct Enter, Move, and Leave notifications along the same resolved branch without becoming interaction targets.

Geometry-specific Hover events remain limited to extensions such as Tooltip and ScrollBar whose active region is not the ordinary node interaction region.

## Normal appearance and transient indication

The normal appearance belongs to the component style or explicit `Background` modifier.

Indication describes transient layers added to that normal appearance.

```text
normal background
    + hover layer
    + press layer or ripple
```

This avoids two owners for the same normal background between a component style and `InteractionScheme`.

An opaque pressed fill can replace the visible normal background by painting after it and fading away to reveal it again.

Component-specific disabled, selected, checked, invalid, and other controlled appearances remain in their owning component styles.

## VisualFill

`VisualFill` is the common immutable value for ordinary backgrounds and interaction layers.

It supports:

- A `Brush` containing `Color`, `LinearGradient`, or `RadialGradient`.
- `ImageResource`.
- `ImageAsset`.
- `VectorAsset`.
- An `ImageFill` carrying fit, alignment, sampling, tint, and opacity.

Direct image and resource constructors use fill geometry defaults, while `ImageFill` expresses non-default placement.

Gradient coordinates use normalized destination bounds so the same value adapts to responsive component sizes.

Gradient stop offsets are finite, lie between zero and one, are nondecreasing, permit equal offsets for hard transitions, and contain at least two stops.

ImageResource remains unresolved in public Theme and modifier values.

Mounted reconciliation resolves it against the effective resource context before the immutable retained modifier is installed. Platform renderers never resolve resources, and unchanged retained sequences do not repeat resource lookup.

Resource-context changes invalidate the affected application and layer Views through the existing resource update path, so recomposition resolves current assets before the next retained recording.

The public value family is:

```cpp
struct GradientStop {
  float offset = 0.0F;
  Color color;
};

struct LinearGradient {
  Point start{0.0F, 0.5F};
  Point end{1.0F, 0.5F};
  std::vector<GradientStop> stops;
  Transform2D transform;
};

struct RadialGradient {
  Point center{0.5F, 0.5F};
  Size radius{0.5F, 0.5F};
  std::vector<GradientStop> stops;
  Transform2D transform;
};

class Brush {
public:
  using Value = std::variant<Color, LinearGradient, RadialGradient>;

  Brush(Color color);
  Brush(LinearGradient gradient);
  Brush(RadialGradient gradient);
};

using ImageVariant = std::variant<ImageResource, ImageAsset, VectorAsset>;

struct ImageFill {
  ImageVariant source;
  ImageFit fit = ImageFit::Fill;
  HorizontalAlignment horizontal_alignment = HorizontalAlignment::Center;
  VerticalAlignment vertical_alignment = VerticalAlignment::Center;
  ImageSampling sampling = ImageSampling::Linear;
  std::optional<Color> tint;
  float opacity = 1.0F;
};

class VisualFill {
public:
  VisualFill(Color color);
  VisualFill(LinearGradient gradient);
  VisualFill(RadialGradient gradient);
  VisualFill(Brush brush);
  VisualFill(ImageResource image);
  VisualFill(ImageAsset image);
  VisualFill(VectorAsset image);
  VisualFill(ImageFill image);
};
```

`VisualFill` stores either one `Brush` or one `ImageFill`. Direct Color and gradient constructors preserve concise declarations, while direct image constructors wrap one `ImageFill`, so every source follows a single owned paint path.

The shared declarations belong in `paint.h` because they are platform-neutral paint values consumed by backgrounds, Images, indication, and Theme rather than indication-specific state.

`ImageFit` also belongs in `paint.h` as the shared source-to-destination geometry policy used by Image and image-backed fills.

## Background

The Background modifier changes from a color-only value to a VisualFill:

```cpp
struct Background {
  VisualFill fill;
};
```

Color remains a direct construction path, so ordinary solid backgrounds do not require a wrapper at call sites.

Foreground remains color-based because text and icon foreground roles are not region fills.

`VisualFill` describes only the interior fill of a region.

Borders remain an independent visual property so a transparent or absent background can be combined with an outline without introducing a special `VisualFill` alternative.

## Border

`Border` is both the generic View modifier and the reusable immutable border value consumed by component styles and indication layers:

```cpp
struct Border {
  static const detail::ModifierDescriptor& Descriptor();

  Color color;
  float width = 1.0F;
};
```

The width is finite and non-negative, and the border follows the node's resolved corner radii.

An absent or transparent `Background` combined with `Border` produces a pure outline.

A zero-width border explicitly suppresses an inherited or normal border.

Borders remain color-based; supporting gradient path strokes does not implicitly widen the Border modifier or Theme contract.

## Indication composition

The color-specific state-overlay and ripple variants converge into one composable state indication:

```cpp
enum class IndicationPlacement {
  BehindContent,
  AboveContent,
};

struct IndicationLayer {
  std::optional<VisualFill> fill;
  std::optional<Border> border;
  std::optional<CornerRadii> corner_radii;
  IndicationPlacement placement = IndicationPlacement::AboveContent;
  AnimationSpec enter = TweenSpec{.duration = 0.08, .easing = Easing::EaseOut};
  AnimationSpec exit = TweenSpec{.duration = 0.16, .easing = Easing::EaseOut};
};

struct RippleEffect {
  Color color;
  IndicationPlacement placement = IndicationPlacement::AboveContent;
  AnimationSpec expansion = TweenSpec{.duration = 0.32, .easing = Easing::Linear};
  AnimationSpec fade_out = TweenSpec{.duration = 0.2, .easing = Easing::Linear};
};

struct IndicationGeometry {
  std::optional<Size> layer_size;
  std::optional<CornerRadii> clip_corner_radii;
};

struct Indication {
  static const detail::ModifierDescriptor& Descriptor();

  IndicationGeometry geometry;
  std::optional<IndicationLayer> focus;
  std::optional<IndicationLayer> hover;
  std::optional<IndicationLayer> press;
  std::optional<RippleEffect> ripple;

  bool operator==(const Indication&) const = default;
};
```

`Indication` is both the immutable visual value stored by Theme and component styles and the retained modifier accepted by `View::With()`.

There is one built-in indication structure rather than a variant of implementation strategies.

An empty explicit value disables built-in state layers and ripple:

```cpp
view.With(Indication{});
```

A component style uses `std::optional<Indication>` when it needs inheritance: an empty optional inherits the Theme value, a populated value overrides it, and a populated empty `Indication{}` disables it deliberately.

Flat themes use focus, hover, and press layers where their visual language requires them.

Material themes use focus and hover state layers with `RippleEffect` for Press.

The default ripple expands and fades linearly so its configured duration describes the perceived propagation directly; custom themes may provide any `AnimationSpec`.

Ripple remains color-based because its standard meaning is a translucent state color expanding from the interaction origin.

Gradient, image, vector, and other masked reveal effects remain custom `NodeExtension` behavior rather than widening every renderer's built-in ripple contract.

Ripple placement is explicit because a selectable container may need feedback above opaque child content, while a Button may keep its label and icon above the ripple.

`AboveContent` is the general selectable-surface default; component themes may choose `BehindContent` where preserving unaffected node content is part of the component treatment.

Custom themes can combine an image or gradient BehindContent layer with an AboveContent ripple without adding another indication kind.

`IndicationLayer::placement` controls its optional fill, while `RippleEffect::placement` controls the ripple independently.

Neither placement changes state-border or corner-radii resolution.

The border is a complete state override resolved in the node's border paint phase, not another outline drawn over the normal border.

The corner radii are a complete state override for the node surface rather than a second clipped shape painted over the normal surface.

The normal border and corner radii come from generic modifiers or the owning component style.

The active state layer is selected in this order:

```text
disabled
pressed
hovered
focus-visible
normal
```

Disabled nodes expose no transient interaction layer and retain only their component-owned disabled appearance.

Press is selected when Press is active and a press layer exists; otherwise the resolver may fall through to hover or focus when those facts and layers exist.

An explicitly present but empty press layer suppresses lower-priority layers while allowing a ripple-only Press treatment.

Once a state layer is selected, each absent border or corner-radii field inherits the normal value rather than a value from a lower-priority interaction layer.

The focus ring remains orthogonal to the selected state layer and is visible only for enabled focus-visible nodes.

Keyboard focus on a `SelectionArea` retains this theme focus ring so selectable static content has a visible focus target.

Only the resolved border is painted, so changing its color or width, making it thinner, or suppressing it with zero width never exposes a second border underneath.

The resolved corner radii apply consistently to the normal background, state fill, border, bounded ripple, focus ring, and descendant clip when `ClipChildren` is present.

Visual fills, indication geometry and layers, ripple configuration, and focus-ring geometry validate finite values, ranges, gradient ordering, non-empty image assets, and animation specifications when a View consumes them. Retained paint keeps its own defensive validation, but invalid declarative or Theme input does not wait until a later frame or interaction state.

All four corner radii interpolate independently and remain finite and non-negative.

The indication geometry controls only the bounds and clip of state fills and ripple.

An empty `layer_size` uses the node bounds; an explicit size is centered in the node without changing layout.

This lets a component keep a larger layout or input target while a Theme supplies a compact state layer, such as the centered state layer of an IconButton, Checkbox, or Switch.

A retained component whose indication region moves or otherwise depends on current geometry may provide `indication_bounds_override`; that dynamic rectangle takes precedence over `layer_size` for the frame.

An empty clip corner radius follows the resolved surface radii, while an explicit value supports compact circular or pill state layers such as navigation indicators.

Clip radii are normalized to the resolved indication bounds before commands are recorded.

Border color, border width, and corner radii animate from their currently presented values to the selected state target.

Moving between normal and a state fill animates its opacity between hidden and visible.

Moving directly between different state fills retains the immutable source and target values and crossfades them, so colors, gradients, images, and vectors never switch before the transition completes or reconstruct their resources per frame.

State changes retarget active animation controllers from their current presented values without snapping back to their previous start values.

Entering a state uses its `enter` animation, returning to normal uses the departing state's `exit` animation, and moving directly between non-normal states uses the target state's `enter` animation.

Press takes visual priority over hover for mutually exclusive state layers, while `RippleEffect` remains an ordered Press visual and may continue after Release or Cancel.

Disabling clears active state and ripple visuals immediately and never retains an active Press fact.

Unmount discards interaction state, animation controllers, and ripple visuals immediately.

Reduced motion resolves these animations through the existing animation policy, normally snapping to the target without changing the interaction contract or final visual state.

## InteractionScheme

InteractionScheme stores the complete default Indication rather than a kind plus detached colors:

```cpp
struct FocusRing {
  Color color;
  float width = 2.0F;
  float offset = 2.0F;
};

struct InteractionScheme {
  Indication indication;
  FocusRing focus_ring;
  float disabled_opacity = 0.42F;
};
```

`FocusRing` is one validated value so its color, width, and offset cannot inherit or change as unrelated configuration facts.

Its offset is the clear gap between the surface boundary and the inner edge of the centered border stroke. Material uses the secondary color with a 3-unit width and 2-unit outward offset.

A zero width suppresses the ring.

This removes `IndicationKind`, `hover_overlay`, `pressed_overlay`, and the standalone ripple color as parallel configuration facts.

A typed component style may still store `std::optional<Indication>` when its surface needs a different state treatment or must disable the inherited treatment.

The normal component background does not move into InteractionScheme.

## Runtime-to-paint frame flow

Interaction does not directly mutate a `PaintSequence`.

Runtime first commits interaction facts to MountedNode, retained extensions consume those facts and advance their visual state, and the paint phase records only the resulting current-frame values.

The relevant frame order is:

```text
compose and reconcile Views
measure and layout dirty nodes
refresh effective interaction state for the mounted tree
advance retained extensions and indication animations
resolve presentation layers and dynamic indication geometry
publish current resolved_border and resolved_corner_radii from retained indication geometry preparation
record dirty content and foreground PaintSequences
compute damage and commit the RenderScene frame
```

`RefreshInteractionTree` occurs after layout because hover and focus-visible delivery require the current mounted tree and final node geometry.

Declaration and disabled-boundary changes establish the normal surface baseline. Geometry preparation publishes a different resolved surface only when the current indication transition changes it.

The extension never writes declarative View properties and Paint never resolves interaction priority or advances animation.

`NodeExtension` exposes the two retained paint positions that the existing RenderNode sequences can represent:

```cpp
enum class PaintInvalidation {
  None,
  Content,
  Foreground,
  Both,
};

virtual PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer& text_measurer);
virtual void PaintBehindContent(const MountedNode& node, PaintContext& context);
virtual void PaintAboveContent(const MountedNode& node, PaintContext& context);
```

The existing foreground-only `Paint` callback becomes `PaintAboveContent`.

`PrepareGeometry` receives the active borrowed text measurer, publishes dynamic geometry such as `indication_bounds_override`, and reports which retained sequence became dirty.

Paint-visible retained state changes call `InvalidatePaint(PaintInvalidation)`, with Foreground as the default for extensions whose existing output remains in the foreground sequence.

This phase value avoids separate invalidation methods and prevents a BehindContent-only animation from rebuilding the foreground sequence.

An extension that changes resolved border or corner radii returns or requests Content, or Both when the same change also affects foreground output such as a focus ring.

No descriptor-level paint metadata, paint observer, or third retained sequence is introduced.

## Paint ordering and invalidation

BehindContent must paint after the normal background but before the resolved border, text, icons, node content, and children.

The retained order is:

```text
shadow
normal background with resolved surface radii
BehindContent indication fill and ripple in indication geometry
resolved border with resolved surface radii
node content
children
AboveContent indication fill and ripple in indication geometry
focus ring with resolved surface radii
```

`RenderNode` keeps its existing content and foreground `PaintSequence` values.

The content sequence records the shadow, normal background, every BehindContent extension phase, resolved border, and node-owned content in that order.

The foreground sequence records every AboveContent extension phase, including indication and ripple, followed by the framework-owned focus ring.

The final focus-ring pass is shared by every focusable node, including a custom `Focusable` View without Click or an Indication extension.
It consumes the Theme ring or a component-specific override and the final indication-bounds override when a compact control supplies one.

`IndicationExtension` owns animation controllers, active ripple instances, fill crossfades, and the currently presented border and corner radii.

MountedNode owns only the authoritative interaction snapshot, active Press count, resolved border and radii, and an optional dynamic indication-bounds override needed by paint and clipping.

PaintContext records immutable commands for the currently presented values; platform renderers only execute those commands.

Changing a BehindContent fill or ripple, resolved interaction border, or resolved surface radii marks the node's content sequence dirty.

Changing an AboveContent fill or ripple marks the foreground sequence dirty.

A layer containing both an AboveContent fill and a border invalidates both sequences.

A transition whose source and target fills occupy different placements invalidates both sequences until the crossfade completes.

Each active ripple retains the placement captured by its Press event, so updating a Theme or explicit Indication affects later ripples without moving a visible ripple between retained sequences.

Both sequences remain independently dirty while active ripples exist in both placements.

Resolved surface-radii changes also update the focus ring and any rounded descendant clip.

When descendant clipping changes, the retained scene revision and damage include both the previous and current transformed clip bounds.

Interaction-driven corner radii do not change measurement, layout bounds, or the node's pointer hit region.

This may rerecord the current node's text, image, or Canvas commands, but it does not remeasure, relayout, or rerecord child nodes.

Interactive controls normally record inexpensive node-owned commands, so adding a third `PaintSequence` to every `RenderNode` is not justified without profiling evidence.

## Brush paint commands

`PaintContext::DrawRect`, `FillPath`, and `StrokePath` accept the same closed `Brush` value and record one command shape each. This prevents geometry and source alternatives from multiplying into parallel command and renderer entry points.

Each gradient carries an identity-by-default affine transform in normalized gradient coordinates.
The destination mapping is applied after that transform, and only the gradient sampling space changes; indication geometry, layout, clipping, hit testing, and damage bounds remain unchanged.

Asymmetric rounded rectangles lower to one `FillPathCommand` with the rectangle as its Brush bounds. `ImageFill` expands into image or vector drawing commands under the required rounded or path clip.

Every supported renderer resolves the Brush alternative inside the geometry command. Opacity and blending remain independent presentation concerns rather than Brush fields.

## Custom indication

`Indication` remains one closed, equality-comparable built-in value.

A third-party visual suppresses the built-in value with an empty explicit `Indication{}` and attaches its own retained modifier whose NodeExtension consumes `OnInteraction`, advances animation in `OnFrame`, and records paint in the appropriate phase.

This reuses the existing extension lifecycle instead of introducing a disabled sentinel, an indication variant or factory hierarchy, or function-valued Theme entries.

## Text selection overlay

The text-selection action toolbar is not currently a normal MountedNode subtree.

It retains an overlay-local InteractionState for each action and feeds the same private indication instance contract.

It does not retain separate visual `SetHovered`, `Press`, and `Release` entry points after the indication refactor.

Moving the toolbar to ordinary mounted Layer content is a separate architectural decision and is not required for this interaction contract.

## Implementation boundary

The implementation introduces the shared interaction owner and ordered events, moves indication instances to that state, consolidates `InteractionScheme` and `FocusRing`, preserves compact indication geometry, and establishes state border and corner-radii resolution with two-phase paint ordering.

The same contract adds generic `Border`, gradients, and resource-backed fills, updates `Background` and indication layers to `VisualFill`, resolves resources before retained recording, and updates every supported renderer.

No old indication names or compatibility aliases are retained unless a compatibility policy is approved separately.

Focused tests cover ordered pointer Press, Release, and Cancel delivery, node-local Press coordinates, state-layer rendering, focus rings, compact indication geometry, and gradient command validation. Existing interaction, presentation, and renderer tests continue to cover shared activation, cancellation, and retained paint behavior.
