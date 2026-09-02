# RefreshBox

This document defines controlled refresh ownership, shared overscroll integration, retained presentation, Theme policy, and accessibility behavior.

## Public contract

`RefreshBox` owns one ordinary child declaration and one required controlled `refreshing` value:

```cpp
return RefreshBox(content, refreshing).OnRefresh([=] {
  RequestRefresh();
});
```

`OnRefresh` delegates to `RefreshEvents::Requested` and does not create another callback convention.
The component rejects an empty child before mounting.
It does not own application data, a Task, a controller, pull progress State, or an internal authoritative refresh flag.

An armed direct-touch release and the localized accessibility Refresh action each emit one request.
The next application-provided value decides the result: true holds the presentation at the refresh position, while false returns it to zero.
Programmatic false-to-true changes present refreshing state without emitting a request.

## Mounted structure and identity

The component mounts as one ordinary scroll-capable node containing the supplied View.
Its private layout measures the child once under the normal single-child constraints and reports the constrained result.
It does not create a composition scope, layer, parallel tree, hidden content factory, or second ScrollController.

Compatible updates preserve the child, local State, focus, text editing, scroll offsets, retained extensions, PlatformViews, and ExternalTextures.
Pull and settle frames change descendant presentation only and do not recompose, measure, or lay out the child.

## Scroll ownership and handoff

Runtime's generic nested-scroll transaction visits inner content offsets before selecting one terminal overscroll owner.
RefreshBox enables only leading vertical overscroll and only for `ScrollSource::Drag`.
Ordinary content scrolling therefore remains with the deepest compatible descendant until that content reaches its leading boundary.

During direct dragging, the shared scroll state owns the resisted overscroll offset and existing pointer capture and cancellation rules.
On release or cancellation, the retained behavior clears that offset and seeds its motion controller with the same visual displacement before presentation is resolved.
The content does not jump because shared overscroll and retained translation never remain active at the same time.

The post-release state machine has four retained modes:

- Idle accepts a new direct pull and exposes the semantic Refresh action.
- Awaiting commit has emitted one request and waits for the next controlled value.
- Refreshing holds at the Theme-defined refresh position and animates the indicator.
- Settling returns the presentation to zero before becoming idle.

Release below the trigger, cancellation, a rejected request, completion, disablement, and reduced motion all converge on the same generic cleanup paths.
Wheel and trackpad input never enter this state machine.

## Presentation and invalidation

The root clips its descendants and paints the built-in indicator as foreground content.
The retained behavior writes one generic `NodePresentation::children_transform`; RenderScene, hit testing, semantics, text-input geometry, and PlatformView composition consume the same resolved presentation tree.
The transform is reset before every extension frame and recomputed from retained state, so it cannot leak across incompatible replacement or unmount.

Visual movement invalidates paint and presentation damage only.
The indicator reuses the same progress-circle geometry as `ProgressCircle`, while `RefreshBoxStyle` owns its visual style, container geometry, trigger distance, refresh position, and settle motion.
Material and Flat definitions provide different defaults through ordinary Theme style resolution.
Theme pull resistance and maximum displacement compile into the same `ScrollPhysics` binding used by generic overscroll; a later explicit `ScrollPhysics` modifier keeps normal declaration-order precedence.

## Semantics

Idle enabled content exposes one localized custom Refresh action on the container.
Performing it emits the same typed request as an armed release.
While the controlled value is true, the container reports busy and suppresses another Refresh action; decorative indicator frames do not create separate semantic nodes or announcements.

Descendant semantics remain present and use the same committed presentation translation as drawing and hit testing.
Platform adapters consume the shared semantic frame and do not install native refresh controls or platform-specific refresh state machines.

## Invariants

- Application `refreshing` state is the only authoritative stable refresh value.
- One nested transaction owns direct pull resistance and at most one terminal overscroll displacement.
- Shared overscroll and retained descendant translation are never simultaneously active after handoff.
- A request is emitted once per armed release or accepted semantic action, never for a programmatic state change.
- Pull and animation frames do not write application State or invalidate measurement and layout.
- Runtime and platform adapters contain no `RefreshBox` branch.
