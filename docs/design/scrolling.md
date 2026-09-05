# Scrolling

This document defines scroll input, nested consumption, authoritative offset ownership, momentum, overscroll, observation, and platform integration.

## Ownership

Each mounted scroll container owns one actual offset per supported axis.
`ScrollController::Metrics()` is an observable projection of that offset and geometry, not another writable scroll position.
Momentum owns only its current velocity, while overscroll owns only a temporary presentation displacement outside the clamped content range.
Neither value is copied into `ScrollMetrics`.

Runtime owns scroll recognition and coordination without branching on concrete component types.
`ScrollView`, virtual layouts, and editable text configure the same mounted scroll state and use the same offset mutation path.
Platform adapters normalize host input and report Runtime's consumption result; they do not implement nested scrolling or component behavior.

## Platform scroll input

`ScrollInputEvent` represents one platform-recognized wheel or trackpad update:

```cpp
struct ScrollInputEvent {
  Point position;
  float delta_x;
  float delta_y;
  KeyModifiers modifiers;
};
```

`position` is in window logical coordinates and `delta_x` and `delta_y` are the requested content-offset changes in logical pixels.
Touch dragging remains a pointer gesture and never produces `ScrollInputEvent`.

The deepest enabled View with `ViewEvents::ScrollInput` receives the complete input directly:

```cpp
return canvas.On<ViewEvents::ScrollInput>([](const ScrollInputEvent& event) {
  return ZoomCanvas(event.position, event.delta_y, event.modifiers.control);
});
```

Returning true consumes both axes and suppresses built-in scrolling.
Returning false runs default scroll coordination.
The event does not capture, bubble, or join pointer-sequence arbitration.

`Runtime::HandleScrollInput()` returns the actual two-dimensional delta consumed by HuxerUI.
Platform adapters suppress their host default only when at least one axis was consumed.
An unconsumed wheel update remains available to a native parent, browser page, or platform default.

## Nested scroll transaction

Horizontal and vertical deltas run as independent transactions so a diagonal trackpad update may be split across differently oriented containers.
For each nonzero axis Runtime uses the committed hit route and applies one ordered transaction:

```text
outer pre-scroll extensions
        ↓
inner-to-outer mounted scroll offsets
        ↓
inner post-scroll extensions
        ↓
one terminal direct-touch overscroll owner
```

Only retained extensions attached to participating scroll containers receive nested callbacks.
Pre-scroll visits outer containers before inner containers.
Normal offset consumption visits inner containers before outer containers.
Post-scroll visits inner containers before outer containers.
Modifier declaration order is used for pre-scroll and reversed for post-scroll, preserving ordinary nested wrapper order without another registry.

The extension contract reports actual consumption:

```cpp
float OnPreScroll(MountedNode& node, Axis axis, float available, ScrollSource source);
float OnPostScroll(MountedNode& node, Axis axis, float consumed, float available, ScrollSource source);
```

`available` is the signed delta still available at that point.
`consumed` passed to post-scroll is the amount already consumed earlier in the same transaction.
A returned value must be finite, have the same direction as `available`, and have no greater magnitude.
Runtime treats a violation as a framework invariant failure.

The transaction result is temporary bookkeeping only.
It does not create another offset, connection, controller, dispatcher, or observable state.

## Direct drag and gesture ownership

Direct dragging remains one recognizer in `PointerSession`.
After movement crosses slop on the recognizer's axis, the deepest compatible scroll recognizer owns the pointer sequence.
Its committed route supplies the same pre-scroll, offset, and post-scroll transaction used by indirect input.

Touch may accept at a content boundary when terminal overscroll is enabled for that container.
Mouse and pen dragging require ordinary scroll capacity and do not acquire ownership solely for overscroll.
Pointer cancellation ends the direct activity and settles any temporary overscroll displacement.

## Fling coordination

Fling uses the same scroll-container chain and a paired velocity contract:

```cpp
float OnPreFling(MountedNode& node, Axis axis, float available_velocity);
float OnPostFling(MountedNode& node, Axis axis, float consumed_velocity, float available_velocity);
```

Pre-fling visits outer containers before the selected inner momentum owner.
When that owner finishes or reaches a boundary, its post-fling extensions receive the remaining velocity before the nearest compatible ancestor may continue the momentum.
Consumption follows the same finite, direction, and magnitude invariants as scroll delta consumption.

`ScrollPhysics` is the only scroll motion policy value.
An explicit `ScrollPhysics` modifier overrides the platform adapter's default for that container.
It owns fling thresholds and decay together with overscroll enablement, resistance, extent, and settle rate.
Theme owns scrollbar visuals, not physical behavior.

## Overscroll

The actual content offset remains clamped from zero through the current maximum.
After pre-scroll, every compatible mounted offset, and post-scroll have seen a direct touch delta, at most one terminal container may retain the remainder as overscroll displacement.
The outermost reached container with overscroll enabled for the remainder's direction is the terminal owner.
Leading and trailing edge eligibility are separate mounted capabilities so a retained behavior can accept only the edge it owns without introducing a component branch in Runtime.

Overscroll is presentation-only.
It translates clipped scroll content without changing measurement, virtual ranges, controller metrics, focus reveal, or semantic scroll position.
PlatformViews follow the same translation and clipping as ordinary rendered content.
Release, cancellation, disablement, removal from layout, and unmount settle or clear retained displacement.
Reduced motion clears released overscroll and momentum on the next frame instead of retaining an animation.
Settlement reports `ScrollSource::Overscroll` activity so retained presentation and text-input geometry follow every visual displacement without creating another observable position.

Wheel and trackpad input do not create overscroll.
This prevents an indirect input boundary from accidentally activating retained pull-to-refresh behavior intended for direct manipulation.

A retained behavior may take over the committed overscroll displacement at the terminal `End` or `Cancel` notification.
It clears the shared temporary offset and applies an equivalent descendant presentation transform in the same commit, then owns only its post-gesture animation.
This handoff preserves the single authoritative scroll offset and avoids a one-frame jump without keeping two active overscroll owners.

## Activity and observation

`ScrollActivity` is a transient retained-extension notification:

```cpp
struct ScrollActivity {
  ScrollSource source;
  ScrollPhase phase;
  Axis axis;
  float delta;
  ScrollMetrics metrics;
};
```

It replaces separate offset-changed and direct-gesture notifications.
Scrollbars and editing affordances use it for temporary presentation state, but it is not retained application state.
`ScrollController::Metrics()` remains the single public observable for offset and extent changes.

Application behavior such as loading more content observes controller or virtual-layout metrics.
It does not infer content position from raw wheel input or create a second public changed-event stream.
Visible item ranges and item-count thresholds belong to virtual-layout metrics when that API is introduced.

## Programmatic and semantic scrolling

Controller requests, focus reveal, text-input reveal, scrollbar dragging, drag-and-drop auto-scroll, and accessibility actions all call the same clamped offset mutation path and publish the same metrics.
They target the addressed container directly and do not automatically chain a remainder to ancestors.
Only user scroll transactions and fling continuation perform nested consumption.

`ScrollToItem` retains a pending index and alignment until measured placement agrees with the requested offset. Newly realized variable-height items may change the estimated target, so completion occurs after layout rather than when the initial offset is applied. Direct scrolling, a newer offset request, disablement, or an invalidated target cancels the pending request; correction never becomes a competing scroll position. Requests accepted by an old connection are not replayed after rebinding or remounting.

## Invariants

- A mounted scroll container is the sole owner of its actual offset.
- `ScrollMetrics` is a projection and overscroll displacement is not content position.
- Each axis has one ordered consumption transaction and at most one terminal overscroll owner.
- Touch drag is pointer input; wheel and trackpad updates are `ScrollInputEvent`.
- Direct input handlers decide only whether they consume the complete platform update.
- Nested extensions return actual consumption and never mutate framework-owned offset bookkeeping.
- Platform adapters normalize input and honor Runtime consumption without duplicating scroll policy.
- Runtime remains independent of concrete scroll component types.
