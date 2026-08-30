# Animation and Scene Transition Design

This document defines one retained motion model for animated values, synchronized presentation properties, component motion, and whole-scene transitions. It deliberately keeps ordinary View insertion and removal outside the initial scene-transition implementation.

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

NodeExtension::FrameResult OnFrame(MountedNode&, const FrameInfo& frame) override {
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
      scene_transition.Run(CircularRevealSceneTransition{}, [dark] {
        dark = !dark;
      });
    });
```

`RunAt()` accepts a window-local logical point for callers that already own pointer geometry.
`Run()` with a circular reveal uses the final bounds of the retained anchor, while Fade does not require an anchor.

`RunFromCurrentInteraction()` resolves a circular reveal from the Runtime's current synchronous interaction origin:

```cpp
return Button("Dark theme").OnClick([scene_transition, dark] {
  scene_transition.RunFromCurrentInteraction(CircularRevealSceneTransition{}, [dark] {
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

Starting a transition while another one is active freezes the currently committed composite and replaces the prior snapshot. Transitions do not queue or create nested snapshots. Reduced motion runs the mutation without freezing or scheduling animation. A viewport change that invalidates the frozen logical coordinate space ends the transition.

Whole-scene transitions are one-shot. Their public descriptions expose an optional delay but do not expose repetition or reverse playback because either policy would retain obsolete full-scene visual data indefinitely. They reject undamped springs for the same ownership reason: an undamped spring never settles. Repetition and undamped springs remain available to ordinary `MotionController` animation.

### FrozenScene

`FrozenScene` is private Runtime-owned render data. It deep-copies the committed `RenderNode` hierarchy and each `PaintSequence`, assigns independent node identities, and keeps immutable Image and vector resource ownership shared. It contains no mounted nodes, scopes, Environment, event handlers, semantics, text-input clients, or platform objects.

`DrawExternalTextureCommand` retains its producer, so frozen geometry can continue to display the producer's newest pixels. `PlacePlatformViewCommand` is not copied into a frozen scene. A PlatformView remains live in the new tree and does not participate in group opacity or circular clipping. When a live scene contains a PlatformView, both transition kinds therefore degrade to fading the frozen render scene over the unmodified live scene. A PlatformView removed by the mutation disappears immediately because stale platform handles are never retained or simulated.

The existing damage snapshot is renamed `RenderDamageSnapshot`. It remains lightweight metadata for comparing committed render revisions and is not a visual snapshot.

### Scene composition

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
