# Animation and Scene Transition Design

This document defines retained motion for animated values, synchronized presentation properties, component motion, shared elements, and whole-scene transitions. Ordinary View insertion and removal remain outside this contract.

Custom page and scene effects are available through TransitionSpec.
Page configuration is defined in [Navigation](navigation.md#page-transition-customization); navigation and explicit local mutations share the matching and rendering contract below.

## Goals

The animation system has four layers:

```text
TimingCurve and AnimationSpec
    -> MotionController
    -> animated property or synchronized Transition
    -> component motion or scene transition
```

Each layer has one responsibility:

- `TimingCurve` and `AnimationSpec` are immutable motion descriptions.
- `MotionController` owns retained scalar value, target, velocity, time, delay, and repetition state.
- Animated modifiers and `Transition` project retained progress onto presentation properties without recomposition.
- Components and scene transitions choose semantic motion while reusing the same executor.

Animation advances during Runtime frame construction. It never asks a platform renderer to invent timing, interpolate framework state, or recompose a component for every frame.

## Timing and playback

`TimingCurve` accepts the common named curves and an explicit cubic Bezier curve:

```cpp
using TimingCurve = std::variant<Easing, CubicBezierCurve>;
```

The named set contains `Linear`, `EaseIn`, `EaseOut`, and `EaseInOut`. A cubic Bezier curve requires finite control points and x coordinates in the unit interval so progress remains a single-valued function of time.

`AnimationSpec` contains:

```cpp
using AnimationSpec = std::variant<
    SnapSpec,
    TweenSpec,
    SpringSpec,
    KeyframeSpec>;
```

`TweenSpec` combines a non-negative duration and `TimingCurve`. A zero-duration Tween resolves to its target after its optional delay and does not support repeated playback. `KeyframeSpec` contains ordered `ProgressKeyframe` values over one duration. Keyframe fractions are strictly increasing, begin at zero, end at one, and map to normalized progress. Each keyframe owns the curve leading to the next keyframe. This keeps keyframes useful to scalar and projected multi-property motion without introducing an untyped property bag.

`SpringSpec` retains stiffness and damping ratio. Its executor evaluates the damped oscillator analytically rather than accumulating a frame-rate-dependent Euler approximation. Retargeting preserves the current velocity.

Playback is orthogonal to the motion description:

```cpp
struct AnimationPlayback {
  double delay = 0.0;
  std::optional<std::uint32_t> iterations = 1;
  RepeatMode repeat_mode = RepeatMode::Restart;
};
```

An absent iteration count means unbounded repetition. Tween and keyframe motion support restart and reverse repetition. Spring motion supports one iteration only because repeating a physically settled state needs an explicit reset policy; silently inventing that policy would make velocity semantics inconsistent.

## MotionController

`MotionController` is the public retained scalar executor:

```cpp
MotionController motion;
motion.AnimateTo(1.0F, TweenSpec{0.24, Easing::EaseOut});

NodeExtension::FrameResult OnFrame(ViewNode&, const FrameInfo& frame) override {
  const MotionAdvanceResult result = motion.Advance(frame);
  return {result.needs_frame, result.wake_after};
}
```

The controller exposes the current value, requested target, velocity, and running state. `Set()` resolves immediately and establishes a new target. `Seek()` sets a value and velocity for gesture handoff or predictive interaction. `AnimateTo()` retargets from the current state. Completing a reversed iteration may leave the visible value at its starting point without changing the requested target, so an unchanged declarative target does not restart the completed motion. `Advance()` returns both whether the visible value changed and whether continuous or delayed work remains.

`FrameInfo::reduced_motion` is resolved for the current mounted node before its extensions run. A third-party extension therefore receives the same node-local accessibility policy as built-in animation without reaching into Theme internals. Reduced motion resolves an active controller to its target immediately.

`Animated<T>` remains the declarative target form used by built-in presentation modifiers. It carries an `AnimationPlayback` in addition to the target and `AnimationSpec`. Geometry values use independent scalar controllers internally while preserving one declarative target.

## Synchronized presentation transitions

Independent modifiers are appropriate when properties retarget independently. `Transition` is a retained modifier for several presentation properties that must share exactly one progress clock:

```cpp
return content.With(
    Transition{AnimateTo(selected ? 1.0F : 0.0F, TweenSpec{0.2, Easing::EaseOut})}
        .Opacity(0.6F, 1.0F)
        .Offset({-8.0F, 0.0F}, {})
        .Scale(0.96F, 1.0F)
);
```

Synchronized presentation transitions project progress onto opacity, offset, scale, and rotation. They do not animate layout, paint command structure, arbitrary callbacks, or clipping. Transforms use a fixed offset, rotation, and scale order and remain one modifier unit in the surrounding modifier order.

Like the existing presentation modifiers, `Transition` affects content, descendants, foreground drawing, clipping, and hit testing without changing measurement or placement.

## Scene transitions

Scene transitions animate between two committed window composites. They are explicit and do not watch Theme values:

```cpp
auto scene_transition = UseSceneTransition();

return Button("Dark theme")
    .With(scene_transition.Anchor())
    .OnClick([scene_transition, dark] {
      scene_transition.Run(TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36, Easing::EaseInOut}}, [dark] {
        dark = !dark;
      });
    });
```

`RunAt()` accepts a window-local logical point for callers that already own pointer geometry.
`Run()` with a circular reveal uses the final bounds of the retained anchor, while Fade does not require an anchor.

`RunFromCurrentInteraction()` resolves a circular reveal from the Runtime's current synchronous interaction origin:

```cpp
return Button("Dark theme").OnClick([scene_transition, dark] {
  scene_transition.RunFromCurrentInteraction(TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36, Easing::EaseInOut}}, [dark] {
    dark = !dark;
  });
});
```

Pointer delivery establishes its exact window-local position before raw, gesture, Click, Select, and Menu callbacks run.
Nested semantic activation inherits that precise position rather than replacing it with a component center.
Keyboard and accessibility activation establish the activated View's presentation-bounds center when no pointer origin exists.
The origin is dynamic input-dispatch state: nested dispatch restores its outer value, exceptions restore it, and the value is cleared when synchronous delivery ends.

Calling `RunFromCurrentInteraction()` outside that dynamic scope throws `std::logic_error` rather than consulting a global last-pointer value or silently falling back to an anchor.
An asynchronous continuation or another caller that retains geometry uses `RunAt()` explicitly.
Click, Select, and Menu callbacks therefore keep their semantic signatures and do not acquire optional Point arguments.

The service follows this sequence:

```text
freeze the currently committed composite
    -> execute the caller mutation
    -> build and commit the new application tree
    -> render the frozen old scene and live new scene through retained progress
    -> release the frozen scene when motion completes
```

The new tree becomes authoritative immediately for input, focus, text input, text selection, semantics, resources, and window appearance. The old tree is visual data only. This makes scene transition ownership independent from Navigation and avoids retaining obsolete mounted nodes or event handlers.

Starting a transition while another one is active freezes the currently committed composite and replaces the prior snapshot. Transitions do not queue, and only one transition operation remains active; the copied composite can still contain visual structure from the preceding transition. Reduced motion runs the mutation without freezing or scheduling animation. A viewport change that invalidates the frozen logical coordinate space ends the transition.

Whole-scene transitions are one-shot. Their public descriptions expose an optional delay but do not expose repetition or reverse playback because either policy would retain obsolete full-scene visual data indefinitely. They reject undamped springs for the same ownership reason: an undamped spring never settles. Repetition and undamped springs remain available to ordinary `MotionController` animation.

### FrozenScene

`FrozenScene` is private render data retained by Runtime's SceneTransitionService. It deep-copies the committed `RenderNode` hierarchy and each `PaintSequence`, assigns independent node identities, and keeps immutable Image and vector resource ownership shared. It contains no mounted nodes, scopes, Environment, event handlers, semantics, text-input clients, or platform objects.

`DrawExternalTextureCommand` retains its producer, so frozen geometry can continue to display the producer's newest pixels. `PlacePlatformViewCommand` is not copied into a frozen scene. A PlatformView remains live in the new tree and does not participate in group opacity or circular clipping. When a live scene contains a PlatformView, custom and built-in effects therefore degrade to fading the frozen render scene over the unmodified live scene. A PlatformView removed by the mutation disappears immediately because stale platform handles are never retained or simulated.

`RenderDamageSnapshot` remains lightweight metadata for comparing committed render revisions and is not a visual snapshot.

### Scene composition

SceneTransitionService owns the active snapshot, motion controller, and synthetic render wrappers as one optional active transition, without a separate implementation-state allocation. It reads the committed render frame and window through the shared Runtime::State context. Runtime advances the service at the scene-composition boundary and combines its scheduling result with the rest of the frame. Disconnecting the service clears its context pointer, releases active transition data, and rejects subsequent requests from retained handles.

A fade scene transition without a PlatformView uses two synthetic render wrappers: old opacity is `1 - progress`, new opacity is `progress`. A circular reveal draws the old frozen composite normally and places the live composite beneath one circular child clip on an otherwise empty wrapper. It does not require a separate whole-subtree clip primitive or inverse and even-odd clipping.

An active scene transition reports full damage because the visible composite changes across the reveal or cross-fade. Normal incremental damage resumes after completion.

## Component and lifecycle boundaries

Presentation components may continue to choose Theme-owned motion policy while executing it through `MotionController`. Theme changes do not automatically interpolate every style field. A component that needs animated visual policy opts into the relevant retained transition explicitly.

Ordinary View insertion and removal animation is deferred. It requires an explicit `AnimatedVisibility` or `AnimatedContent` ownership contract that can retain outgoing visual state without leaving a logically removed `MountedNode` interactive. Scene snapshots are not used as a hidden substitute for that lifecycle.

Navigation keeps its own page-stack and Back semantics. It may share timing primitives and controllers, but a scene transition does not push pages, retain navigation scopes, or alter route history.

SceneTransition does not depend on Button, Select, Menu, or the event system.
Runtime owns the temporary interaction origin, while the transition service copies the resolved Point into the ordinary one-shot SceneTransitionRequest before executing the mutation.

## Validation

Focused tests cover controller initialization, zero-duration completion, curve and keyframe validation, delayed and repeated playback, reverse completion semantics, active animation replacement, spring retargeting, reduced motion, synchronized presentation projection, anchor lifecycle, invalid scene springs, viewport cancellation, PlatformView fallback ordering, and frozen plus live scene composition with full damage.

## Extensible transitions
The extension preserves the existing motion executor and separates effect evaluation from navigation history, scene retention, and shared-element ownership.

### Public surface and ownership

`AnimationSpec`, `MotionController`, `AnimateTo`, the ordinary animated presentation modifiers, and the single-node `Transition` retain their existing responsibilities.
The new `TransitionSpec` describes one outgoing/incoming visual transition, including its effect value, animation specification, and optional delay.
It does not retain a running controller or application content.

```cpp
auto transition = TransitionSpec{
    LiftTransition{48.0F},
    TweenSpec{0.28, Easing::EaseOut},
};
```

Construction accepts an effect, an `AnimationSpec`, and an optional non-negative delay in seconds.
A default-constructed `TransitionSpec{}` completes immediately.
Copying a description never starts or duplicates a running animation.
Effect values must be copyable and equality-comparable, with equal values describing equal behavior; same-type descriptions compare their effect configuration and timing by value.
The type-erased effect storage remains private to the implementation, without a public `TransitionEffect` wrapper, registration table, or effect base class.

TransitionSpec supports Snap, Tween, convergent Spring, and Keyframe motion through the existing executor.
It does not expose `AnimationPlayback` repetition: navigation pages and obsolete scene visuals must reach an end to their retention.
Non-settling spring configurations and invalid timing are rejected with `std::invalid_argument` before an operation starts.
These restrictions do not change ordinary MotionController playback.

Navigation chooses and executes a page transition while owning history and page state.
SceneTransitionService chooses and executes a scene transition while owning frozen visual data.
Shared elements consume their operation owner's progress and use a bounds-specific calculation rather than receiving an outgoing/incoming page-effect interface. Navigation supplies its page clock; a local scope uses one MotionController for its matched set.

### Effect evaluation

The effect input and result types are:

```cpp
struct TransitionContext {
  float progress;
  Rect bounds;
  std::optional<Point> origin;
};

struct TransitionFragment {
  ClipShape source_clip;
  Transform2D transform;
  float opacity = 1.0F;
};

struct TransitionSample {
  Transform2D transform;
  float opacity = 1.0F;
  std::optional<ClipShape> clip;
  std::vector<TransitionFragment> fragments;
};

enum class TransitionOrder {
  OutgoingAbove,
  IncomingAbove,
};

struct TransitionFrame {
  TransitionSample outgoing;
  TransitionSample incoming;
  TransitionOrder order = TransitionOrder::IncomingAbove;
};
```

`bounds` describes the transition container in its local logical coordinate system: the stack bounds for navigation or the window viewport for a scene.
An interaction or anchor origin is converted into the same coordinates before evaluation.
Source and destination element rectangles are not added to this context; shared-element bounds transforms receive those values through their own smaller contract.

`TransitionSample` is the visual result sampled for one side of a transition at a specified progress, applying to one complete content group including descendants and foreground drawing.
Sampling may follow animation timing, direct gesture progress, or deterministic test input; TransitionFrame combines both samples with their drawing order.
It is an evaluated value, not the controller's running state, and does not own progress, velocity, scheduling, or completion.
Its transform acts on content in the transition container's coordinate system, followed by its clip in that same coordinate system.
The owner's outer clip remains effective, so an effect cannot escape a NavigationStack's clipping boundary.
`ClipShape` describes sampled clip geometry without exposing the rendering representation or requiring clip commands and drawing-stack management in Evaluate; effects never receive RenderNode pointers.
An optional Paint method records decoration through PaintContext and must balance the drawing stack it uses.
Navigation disables page-content interaction for the duration of an operation while keeping container Back and cancellation available; a frozen scene remains non-interactive regardless of its visual transform.
Fragments do not duplicate mounted nodes, event handlers, focus, or semantics and do not require per-fragment input-coordinate instances.

A custom effect is an ordinary immutable value with a const `Evaluate()` method:

```cpp
struct LiftTransition {
  float distance = 32.0F;

  TransitionFrame Evaluate(const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);

    TransitionFrame frame;
    frame.outgoing.opacity = 1.0F - progress;
    frame.incoming.opacity = progress;
    frame.incoming.transform.translate_y = distance * (1.0F - progress);
    return frame;
  }

  bool operator==(const LiftTransition&) const = default;
};
```

Examples using `std::clamp` require `<algorithm>` in addition to the public HuxerUI declarations.
Evaluate must not call composition hooks, mutate State, retain node or Runtime references, or perform business actions.
The framework may evaluate the same input more than once; invocation count and order are not lifecycle notifications.
The owner continues to control frame scheduling, validation, damage, and resource release.

Gesture progress is bounded to the unit interval, while spring or timing-curve output can overshoot.
An effect may preserve finite geometric overshoot or clamp it deliberately, as in the example.
Finite opacity is clamped to the unit interval consistently with ordinary opacity modifiers; non-finite output and invalid clip geometry are rejected before render submission.
Invalid caller configuration or effect output uses `std::invalid_argument`; framework lifecycle and ownership violations use `std::logic_error`, with diagnostics beginning with `HuxerUI`.
Effect failure never implies rollback of an already accepted application-state mutation, and no partially evaluated effect frame may be committed.

At progress zero, the effect must join the outgoing stable presentation; at progress one, it must join the incoming stable presentation.
The owner removes temporary visual treatment when the operation ends.
Crossing progress one during spring overshoot is not itself completion; the executor's settling state owns completion.

### Clip geometry and rendering boundary

`ClipShape` is a public value class for declaring a clipping region, separate from the renderer-facing `RenderClip` representation.
It provides geometry construction, parameter validation, copying, and value equality without exposing its storage variant or paint commands.
The construction surface is:

```cpp
frame.incoming.clip = ClipShape::Rectangle(bounds);
frame.incoming.clip = ClipShape::RoundedRectangle(bounds, 16.0F);
frame.incoming.clip = ClipShape::Circle(center, radius);
frame.incoming.clip = ClipShape::FromPath(path);
frame.incoming.clip = ClipShape::FromPath(path, PathFillRule::EvenOdd);
```

`FromPath` uses `PathFillRule::NonZero` unless an explicit fill rule is supplied and retains the existing Path value semantics.
Factory methods validate finite geometry and non-negative extents and radii at construction; invalid caller input throws `std::invalid_argument` with a diagnostic beginning with `HuxerUI`.
An absent optional clip means no additional clipping, whereas a shape describing an empty region clips all content in the affected group.
Shape coordinates follow the same transition-container coordinate contract as the surrounding presentation.

The shared rendering implementation converts ClipShape into the existing RenderClip representation when constructing render data.
RenderClip can continue to use its existing `std::variant<PushClipCommand, PushPathClipCommand>` representation, and renderers continue to consume that representation rather than interpreting public animation effects.
Rectangle, rounded-rectangle, circle, and path factories reuse existing clipping primitives without introducing another PaintCommand kind.
The public ClipShape API provides no RenderClip conversion method, command accessor, or render-node dependency.

ClipShape is named for reusable geometry rather than transitions; TransitionSample uses it for final clipping and TransitionFragment uses it for source regions.
It does not add new View modifiers, replace unrelated clipping APIs, or introduce a shape registry or another general shape hierarchy.

### Fragment composition and decoration

Each side can supply a flat collection of TransitionFragments; empty preserves the single-source fast path and non-empty replaces it with independently drawn source regions in declaration order.
Source clipping occurs before fragment motion, followed by the side's common transform and final clip; all geometry uses the same container-local coordinate system without rebasing a fragment to its clipped bounds.
Opacity factors multiply, and source regions need not cover the complete source or be disjoint.

FragmentRenderGroup is a private render cache, stored directly by an active Scene operation and allocated on demand by a mounted page wrapper only while it draws fragments.
It constructs ordinary RenderNodes with independent instance identities, preserves those identities across samples, and refreshes copied PaintSequences only when source identities or revisions change.
Sharing one RenderNode pointer under different transforms is prohibited because damage and backend caches associate one presentation with each identity.
The source page remains mounted once; cloning render data must not repeat composition or platform-view lifecycle operations.
Fragment rendering imposes no fixed limits on fragment count, copied nodes, commands, clips, or tree depth, and does not substitute another effect based on those counts.
The cache directly accepts source RenderNodes and fragments; it owns no effect description, fallback presentation, or operation state.
Navigation checks the mounted content of fragmented sides during sampling. If a source contains a PlatformView, the operation uses a fade without decoration through completion or predictive cancellation because native Views cannot be replicated.
This capability decision precedes page presentation, geometry, input, and semantics; rendering does not change the chosen presentation or request a second projection.
PaintAboveContent only records decoration and does not change page presentation or invoke recursive page painting.

An immutable effect may implement `void Paint(PaintContext&, const TransitionContext&) const` alongside Evaluate.
TransitionSpec type-erases this optional method, and both executors use the exact stored sample context rather than advancing or rereading the clock during recording.
The executor supplies and finishes the PaintContext, clips decoration to the container, publishes it above both sides, and releases it with the operation.
Recording must be deterministic, balance pushed clips and transforms, and obey the same no-reentry and no-application-mutation rules as Evaluate.
Scene preflights decoration before mutation when an actual visual transition can start; later failures clear active visuals without claiming to roll back business state.
Scene records decoration into the composite's foreground; Navigation records it once on the container through the existing NodeExtension foreground hook and invalidates that recording as progress changes.
No new PaintCommand, public rendering node, particle controller, or additional scheduling convention is introduced.

### Built-in effects and reversal

The initial built-in effects are `FadeTransition`, `SlideTransition`, `ScaleFadeTransition`, and `CircularRevealTransition`.
They use the same value-based evaluation path as application effects.
The initial effect surface supports two-dimensional affine transforms, group opacity, rectangle/path clips, and outgoing/incoming drawing order.
It does not imply support for blur, arbitrary shaders, three-dimensional perspective, or arbitrary path morphing.

`TransitionSpec::Reversed()` produces the reverse visual description without mutating the original value.
An overload accepts a different AnimationSpec for the reverse operation; otherwise the timing description is preserved.
Reversal evaluates the original effect at `1 - progress`, exchanges outgoing and incoming results including fragments, and reverses their drawing order; decoration receives that same reversed progress and remains above both sides.
It is not repeated or reverse playback of retained obsolete content, and it does not reverse application history by itself.

### Extensible scene requests

SceneTransitionHandle keeps its existing ownership and geometry-source model, with this request surface:

```cpp
void Run(TransitionSpec transition, std::function<void()> mutation) const;

void RunAt(
    Point origin,
    TransitionSpec transition,
    std::function<void()> mutation
) const;

void RunFromCurrentInteraction(
    TransitionSpec transition,
    std::function<void()> mutation
) const;
```

```cpp
auto transition = UseSceneTransition();

return Button("Change theme").OnClick([transition, dark] {
  transition.RunFromCurrentInteraction(
      TransitionSpec{
          CircularRevealTransition{},
          TweenSpec{0.36, Easing::EaseInOut},
      },
      [dark] { dark = !dark; }
  );
});
```

Anchor continues to supply stable mounted geometry, RunAt supplies an explicit logical origin, and RunFromCurrentInteraction captures the existing synchronous input-dispatch origin.
An effect that needs an origin must receive a valid one rather than silently using a global last-pointer position.
Request timing, geometry, and required-origin checks occur before mutation wherever the input is already available; custom output remains subject to validation when evaluated.
The current restriction against using RunFromCurrentInteraction from an asynchronous continuation remains in force.

The service freezes the currently committed composite, executes the synchronous mutation, commits the new tree, and combines frozen outgoing visual data with the live incoming scene until completion.
Input, focus, semantics, and resource ownership transfer through the real new tree rather than the frozen data.
Starting another request replaces the active request from the currently committed composite; it does not queue scene operations.
Starting a scene request recursively from a mutation or effect evaluation is rejected with `std::logic_error`.
A mutation exception is propagated without claiming that earlier writes performed by that mutation were undone.
Viewport-coordinate invalidation ends the visual transition and exposes the real current scene.
Reduced motion still runs the mutation and skips visual retention and animation scheduling.

The initial extension adds no Finish or Cancel method, per-request operation handle, waitable completion result, public outcome enum, or transition-completion event.
Completion, replacement, failure, and destruction still have explicit internal cleanup paths and tests.

Navigation and scene transitions remain independent owners.
If an application explicitly places a navigation mutation inside a scene transition, the two visual effects compose with their independent clocks; SceneTransitionService does not silently rewrite navigation policy.
An application that wants only scene-level motion supplies an immediate page transition for that navigation operation.
Shared elements, when enabled, continue to follow the navigation clock rather than the scene clock.

### Native content and scene retention

Extensible effects must preserve the current distinction between immutable render data, live PlatformViews, and producer-backed external textures.
An unsupported PlatformView combination retains the existing fallback of fading frozen render content above the unmodified live scene; it must not pretend that a native View participates in arbitrary group clipping or transforms.
A PlatformView removed by mutation disappears through its real lifecycle rather than surviving as a stale handle.
Freezing render geometry does not freeze the pixels of a producer-backed ExternalTexture.
Shared-element eligibility and fallback are defined in [Shared retention and fallback](#shared-retention-and-fallback).

A single active operation does not guarantee bounded snapshot memory: copying an in-flight composite can repeatedly preserve visual structure from earlier operations.
FreezeRenderScene copies the committed representation without fixed structural quotas or count-based termination of visual work.
Fragment count and source complexity determine copying and replay costs; repeated interruption can increase the retained representation further.
Effects and application-level request frequency must be assessed on the intended devices. There is no automatic performance-budget policy or guarantee of constant memory across interruptions.
Same-frame requests capture the last committed visual; releasing an active snapshot clears its published reference before that storage is freed.

### Implementation and acceptance

Public effect declarations live in `include/huxerui/animation.h`, ClipShape lives beside Path in `include/huxerui/vector.h`, and page configuration lives in `include/huxerui/navigation.h`.
SharedElement, SharedBounds, SharedTransitionHandle, and SharedTransitionScope live in animation.h so local use does not depend on navigation declarations.
`src/runtime/animation.cpp` owns timing, MotionController, and single-node animation modifiers. `src/runtime/transition.cpp` contains TransitionSpec, built-in effects, and Scene transition execution, with each type retaining its own responsibilities.
ClipShape construction and geometric queries live in `src/graphics/path.cpp`; ClipShape-to-RenderClip conversion lives in shared rendering support. Both reuse InternalAccess only where private shape data is required.
`src/runtime/transition_internal.h` declares the shared transition session, effect-evaluation re-entry guard, timing helpers, and FrozenScene capture contracts used across transition implementations. Snapshot copying remains in render.cpp beside render-tree assembly. TransitionSpec and local shared transitions reuse the same timing validation and immediate-completion checks.
Navigation retains page selection, progress, and cleanup; SceneTransitionService retains scene capture and composition; shared rendering code owns visual subtree copies and damage.
Runtime does not gain concrete component branches, a second animation scheduler, or a public transition registry.
Declarative modifier configuration may live in ViewSpec; mutable running state and captured active configuration belong to the existing subsystem owners or NodeExtensions.
NodePresentation retains intersecting ClipShape values and optional fragment descriptions for descendants in node-local coordinates. Ordinary pointer and window-drag hit testing share the geometric clip check; page content remains disabled during navigation motion, so fragment rendering introduces no alternate input-coordinate tree. RenderClip is produced only when generating render data.
NodePresentation::z_index controls sibling stacking independently from layout and mounted child order. Equal values preserve declaration order; child_paint_order stores the resolved child indices shared by rendering and reverse-order hit traversal. Navigation assigns z_index only to its transition participants; no public ZIndex modifier is provided.

Ordinary View insertion/removal, layout animation, three-dimensional effects, and filters remain outside this extension.

Acceptance requires deterministic tests for effect value equality, default immediate completion, valid and invalid timing, endpoint behavior, reversal of both roles and order, finite overshoot, clipping, reduced motion, mutation exceptions, forbidden scene re-entry, rapid replacement, Runtime destruction, viewport invalidation, and bounded retained resources.
Navigation-specific validation is recorded in [Navigation](navigation.md#transition-validation).
ClipShape tests cover factory validation, copying and value equality, path fill rules, empty versus absent clipping, coordinate-space behavior, and conversion into the existing rectangle/path rendering representation.
Static page content must not recompose, remeasure, or rerecord clean PaintSequences during effect-only frames; custom effect geometry may still require its own per-frame calculation.
Audit every renderer for the reused group-transform, opacity, and clip behavior, and validate each affected platform available locally, including native-content fallback paths.

## Shared transitions

`SharedElement(key)` marks the same visual at two locations; one retained representation moves between their bounds.
`SharedBounds(key)` marks different visuals whose retained source and destination crossfade inside the same moving rectangle.
Both are retained modifiers with immutable configuration in `animation.h` and use `.With(...)`; they do not add a layout value, page option, callback registry, or new PaintCommand.
Shared keys use View::Key's signed integer, unsigned integer, enum, and string forms, with a separate identity domain that never changes reconciliation.
Signed and unsigned keys remain distinct.

### Operation scope

Navigation pairs its outgoing and incoming page descendants for Push, Pop, or Replace, using the page operation's clock and predictive progress.
An explicit local operation pairs the last committed descendants of one stable container with its next prepared descendants:

```cpp
auto transition = UseSharedTransition();
auto expanded = UseState(false);
return Column {
  Button("Toggle").OnClick([transition, expanded] {
    transition.Run(TweenSpec{0.45, Easing::EaseInOut}, [expanded] { expanded = !expanded.Get(); });
  }),
  Stack {
    Image(product.image).With(
        Frame{expanded.Get() ? 200.0F : 96.0F, expanded.Get() ? 200.0F : 96.0F},
        SharedElement(product.id)
    ),
  }.With(Frame{.height = 240.0F}, transition.Scope()),
};
```

The handle is retained by its calling composition scope; Scope() attaches it to at most one mounted View, whose identity must survive the mutation.
Run requires a connected scope on its UI thread, valid one-shot AnimationSpec, and a non-empty synchronous mutation.
Timing validation precedes mutation; endpoint and output validation occur when destination layout becomes available.
Mutations execute once and already-performed writes are never rolled back after an exception.
Recursive local or Scene requests from a mutation or effect evaluation throw std::logic_error.
Snap and reduced motion still perform the mutation and skip capture and playback.

The operation range defines key uniqueness: one key on each navigation page, or one key before and after a local mutation.
Nested local scopes are transparent to matching; independent scopes may reuse keys and animate concurrently.
An active outer operation ends inner shared sessions and owns the resulting visual range; inner Run still executes its mutation without starting shared playback.
When multiple scopes decorate the same mounted node, the later declaration has outer precedence.
Nested NavigationStacks are hard matching boundaries even when an outer local scope encloses them.
The scope owner itself is not a candidate; mark content inside it.

Duplicate keys, different marker kinds for a matched key, and matched ancestor/descendant regions throw std::invalid_argument.
Choose one enclosing SharedBounds or separately marked children.
Matching happens once after layout, before render suppression and page-fragment copying; unsupported or missing counterparts do not join later.
Unmatched local content adopts the new state immediately, with normal layout and lifecycle.
Source nodes replaced by a local mutation may unmount normally; only their paint data survives.
Navigation keeps pages mounted through its existing page lifecycle.

### Bounds evaluation and reversal

Bounds are axis-aligned rectangles in the operation container's logical coordinates, excluding the navigation page effect's transform, opacity, and clip.
The default interpolates those rectangles; `.BoundsTransform(effect)` accepts a copied equality-comparable value with `Rect Evaluate(Rect from, Rect to, float progress) const`.
Evaluation must be pure and preserve the exact supplied endpoints at zero and one.
Output coordinates and dimensions must be finite, with non-negative dimensions.
Progress may overshoot for springs or custom curves; crossfade opacity alone clamps to the unit interval.
The custom bounds effect owns no timing or callback lifecycle and never receives mounted nodes or TransitionContext page data.

Push and Replace prefer the destination's bounds effect and single SharedElement content; Pop prefers the departing page's configuration and content.
If the preferred side has no custom bounds effect, the other side supplies it.
Pop samples swapped endpoint rectangles at `1 - progress`, so the same canonical curve retraces on predictive seeking and cancellation.
Local SharedElement uses the committed source visual; SharedBounds always blends both sides.
Pair draw order follows the destination's paint order for local, Push, and Replace operations and the departing page's order for Pop.

### Shared retention and fallback

The private SharedTransitionSession owns pair metadata, immutable subtree captures, and synthetic render wrappers; Navigation or SharedTransitionState owns operation progress.
transition_internal.h declares the session contract shared with Navigation; local state, visual metadata, and pair definitions remain in shared_transition.cpp.
NodePresentation carries temporary render suppression and a borrowed overlay reference; input exclusion belongs to MountedNode's interaction state and preserves declarative enabled styling.
Runtime calls the internal PrepareSharedTransitions entry after layout and presentation geometry settle, before input/semantics publication and scene assembly.
The shared-transition implementation prepares participating sessions directly and releases hidden sessions without adding a general extension lifecycle interface or cached capability pointer. Local sessions end when no valid pairs remain; input exclusion is resolved once per node from its active local sessions after preparation.
Runtime itself does not inspect marker or component types.

Capture reuses ordinary content and foreground recording, retaining complete paint even when a navigation page is visually displaced or transparent.
Successful pairs suppress the real source/destination drawing before page fragments copy their render sources.
Page content and fragments draw first, then shared visuals, then TransitionSpec.Paint decoration, then window layers.
The overlay stays within its owner and ancestor clips and is not affected by page transforms.
Every synthetic or frozen node has an independent retained-render identity, including simultaneous captures and interrupted composites.
No additional hit, focus, semantic, or text-input representation is created.
Navigation page content remains non-interactive during its operation; a local scope excludes content interaction without applying disabled styling, while external controls remain usable.

| Condition | Behavior |
| --- | --- |
| Missing counterpart, empty or unavailable paint, unrealized virtual item | Skip that pair and retain ordinary drawing |
| PlatformView, ExternalTexture, or nested fragment rendering inside the marked subtree | Skip that pair; a resource reference does not freeze native content or producer pixels |
| Rotation, skew, mirrored mapping, or partial ancestor clipping | Conservatively skip that pair; rectangular ancestor clips must contain the complete region |
| Rounded ancestor clip | Use a conservative inner rectangle; path ancestor clips make descendants ineligible |
| Own node clip | Retain it with the captured content |
| Unrequested target geometry/content change, including keyed child replacement/reordering, or navigation participant replacement | End the affected pair and restore surviving originals |
| Owner coordinate space, viewport, or visibility changes | Release shared visuals; navigation may continue its ordinary page effect |
| Completion, failure, scope removal, or Runtime destruction | Clear published links and release retained paint resources |

Subtrees are captured once per operation, not laid out or rerecorded for each animation sample.
Per-frame work validates participating geometry/content and updates bounds, opacity, and render revisions; its cost depends on the participating tree and captured drawing.
Text scales or crossfades with the snapshot; this does not provide glyph morphing or intermediate line wrapping.

A consecutive local Run freezes each currently committed shared composite as its new source.
Every synchronous mutation executes, while requests before another render commit coalesce their intermediate visuals.
Retargeting does not promise velocity continuity.
Interrupted crossfades may retain earlier composite structure until the latest operation ends; there is no structural quota or constant-memory guarantee.
Applications should keep marked regions and interruption frequency appropriate to their devices.
SceneTransition remains an explicit window-composite operation, does not automatically match shared elements, and can compose independently with local or navigation motion.
Ordinary insertion/removal, automatic layout animation, shaders, and general native snapshot capture remain separate capabilities.

Focused tests verify key/configuration equality, deterministic local movement, replacement and interruption, same-frame mutations, navigation reversal and predictive cancellation, nested ownership, independent scopes, hidden/viewport cleanup, unsupported content, invalid output, and suppression before page-fragment copying.
