# Gesture Recognition and Arbitration Design

Status: single-pointer gestures implemented; multi-pointer transform deferred

This document defines the shared gesture-recognition, ownership-resolution, and cancellation model.
Pointer recognition, repeated taps, long presses, and single-pointer drags are implemented; multi-pointer transform and remaining component migrations are staged.
The model extends the interaction foundation without adding general event capture and bubbling or exposing raw pointer capture to applications.

The mounted interaction and indication contract remains defined by [Interaction and Indication Design](interaction-indication.md).
PlatformView composition and initial input ownership remain defined by [Architecture Design](architecture.md#platform-content-integration).

## Goals

- Recognize repeated taps, long presses, single-pointer drags, and multi-pointer transforms in the shared Runtime.
- Give Click, scrolling, public gesture modifiers, and retained pointer extensions one deterministic competition model.
- Keep every physical pointer sequence exclusively owned by HuxerUI or one PlatformView after its Down event.
- Continue delivery to an accepted recognizer outside its node bounds until completion or cancellation.
- Preserve typed event identity, `OnClick()`, raw pointer events, and the existing NodeExtension pointer contract.
- Keep recognition state mounted and incremental without recomposing for every pointer update.
- Let scrolling, Drawer, BottomSheet, ScrollBar, Tooltip, text selection, and third-party retained behavior converge on one pointer path.

## Non-goals

- The design does not add a public single-tap gesture that duplicates Click.
- It does not add DOM-style capture, target, and bubble phases to typed events.
- It does not expose application-facing pointer-capture, release, or transfer handles.
- It does not add `OnTap()`, `OnDrag()`, or other View convenience members.
- It does not define public recognizer priority, simultaneous recognition, failure dependencies, or a gesture-composition DSL.
- It does not define typed payload drag-and-drop, DropTarget behavior, or platform drag sessions.
- It does not infer accessibility semantics from arbitrary gestures.
- It does not add concrete component branches to Runtime.

## One pointer path

The shared Runtime uses one ownership and recognition path:

```text
platform pointer event
        ↓
PlatformView or HuxerUI initial ownership
        ↓
committed HuxerUI hit route
        ↓
PointerSession
        ↓
pointer recognition
        ↓
one owner
        ↓
Click / Drag / LongPress / Scroll / retained pointer extension
```

Raw pointer events are the only side branch:

```text
                    ┌─ raw PointerDown / PointerMove / PointerUp
PointerSession ─────┤
                    └─ ownership resolution → one owner
```

The raw branch reports the physical stream to the deepest ordinary pointer target while recognition resolves ownership.
It cannot stop propagation, accept a gesture, capture a pointer, or create another owner.
When a competing recognizer accepts, the raw target receives one PointerCancel and no later event from that sequence.

Runtime does not retain separate scroll, extension-capture, extension-observer, and gesture-session ownership paths.
Scroll recognition and the existing NodeExtension pointer capability are recognizers alongside built-in and public recognizers.

Hover and `ScrollEvent` remain outside pointer recognition because neither represents an owned Down-to-Up pointer sequence.
Hover performs an ordinary stateless hit test only when no active pointer delivery owns the event.
`ScrollEvent` is a platform-recognized wheel or trackpad delta and continues through the existing nested scroll-consumption path without competing with Drag.

## PointerSession and ownership resolution

Runtime retains one `PointerSession` for each HuxerUI-owned pointer.
The session contains its mounted hit route, optional raw target, ordered recognitions, optional owner, fixed-capacity movement samples, and one quarantine flag.
These are fields of the existing session rather than public types or another pointer registry.

Each recognition returns one private decision for an input update:

```text
Continue
Accept
Reject
```

Ownership resolution is the Runtime algorithm that dispatches the recognition states stored by PointerSession; it is not another retained object or state type.
The algorithm deactivates rejected recognitions and records at most one owner.
It does not need separate Possible, Accepted, Rejected, Completed, and Canceled state objects because those states follow directly from pending recognition, the owner field, and session termination.

Recognitions are collected from the deepest mounted node toward the root.
Retained-modifier recognitions on the same node follow reverse declaration order, matching existing topmost extension dispatch.
The node's built-in Tap or Scroll recognition follows its retained-modifier recognitions.
The first recognition to return Accept in that deterministic order owns the sequence.

Runtime resolves immediate recognizers before publishing raw PointerDown.
If a retained pointer extension returns Capture or consumes Down, it can win without exposing a transient raw Down/PointerCancel pair that did not exist before this design.
Otherwise Runtime installs the pending PointerSession before publishing raw PointerDown, so a handler that dismisses a Layer can safely quarantine that same session without invalidating stack-local ownership state.

Acceptance updates the session before invoking framework or application output:

```text
record owner
    ↓
reject remaining recognitions
    ↓
cancel the raw target and previous pressed interaction
    ↓
publish owner output
```

An application exception therefore cannot leave two owners or unresolved recognition behind.
The owner continues receiving Move, Up, and Cancel outside its original bounds.

Focus and pressed interaction are PointerSession side effects rather than recognizers or owners.
Mouse and pen focus retain their current Down behavior, while touch focus remains pending until Tap succeeds.
Another owner cancels pending touch focus and ends the current pressed interaction with Cancel before publishing its own output.

## Click and successful taps

Click is semantic activation rather than a pointer-only gesture.
A successful pointer tap, keyboard activation, and an accessibility Invoke action continue to produce the same Click event:

```text
pointer tap ─────────┐
keyboard activation ├──▶ ViewEvents::Click
accessibility Invoke ┘
```

Applications continue to use `OnClick()` for ordinary actions:

```cpp
return Button("Save").OnClick(Save);
```

Runtime represents physical tap recognition with one internal tap recognizer.
The recognizer exists for the deepest eligible node that declares Click, MultiTap, or both.
It remains pending through movement and accepts on an Up that satisfies the existing successful-release hit rules.
Movement alone does not reject Click; recognizers that need movement tolerance, such as LongPress and Drag, apply their own thresholds.
If a descendant tap no longer qualifies at release, Runtime continues through the committed route so an eligible ancestor can accept.
Raw PointerUp is emitted before the successful tap output, preserving current event order.

A successful tap has two independent consumers on the same mounted node:

```text
successful tap
    ├─ semantic Click, when declared
    └─ retained MultiTap accumulation, when declared
```

This does not introduce simultaneous owners.
There is one tap owner and multiple outputs from that result.
Long press, drag, transform, scrolling, cancellation, disable, or subtree deactivation rejects the tap recognizer and clears incomplete MultiTap accumulation.
The retained MultiTap modifier contributes to that recognizer through the same private recognizer factory used by other gesture modifiers; it does not add a tap-consumer registry or second recognizer interface.

## Repeated taps

Repeated taps require timing, distance, device, and mounted-identity data that semantic Click intentionally does not expose.
The public modifier is named `MultiTapGesture` and requires a count of at least two:

```cpp
return Image(preview)
    .With(MultiTapGesture{.count = 2})
    .On<MultiTapEvents::Recognized>([](const MultiTapEvent& event) {
      ZoomAt(event.position);
    });
```

Its public values are conceptually:

```cpp
struct MultiTapGesture {
  std::uint32_t count = 2;
  std::optional<std::chrono::duration<double>> maximum_interval;
  std::optional<float> maximum_movement;

  bool operator==(const MultiTapGesture&) const = default;
};

struct MultiTapEvent {
  std::int64_t pointer_id = 0;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  Point position;
  Point window_position;
  std::uint32_t count = 2;
};

struct MultiTapEvents {
  struct Recognized : Event<const MultiTapEvent&> {};
};
```

Runtime computes the count from successful shared tap results rather than trusting platform click counts as a cross-platform contract.
Every contributing tap completes on the same mounted modifier identity, uses a compatible pointer kind, remains within the configured distance, and arrives before the configured deadline.

Declaring Click and MultiTap together does not delay or suppress Click.
Applications that require mutually exclusive single- and double-tap actions must model that delayed policy explicitly until a public gesture-composition use case is established.

## Long press

`LongPressGesture` recognizes one pointer that remains within its movement tolerance until its deadline:

```cpp
return Text("Hold for actions")
    .With(LongPressGesture{})
    .On<LongPressEvents::Started>([](const LongPressEvent& event) {
      ShowContextActions(event.position);
    });
```

Its public values are conceptually:

```cpp
struct LongPressGesture {
  std::optional<std::chrono::duration<double>> minimum_duration;
  std::optional<float> maximum_movement;

  bool operator==(const LongPressGesture&) const = default;
};

struct LongPressEvent {
  std::int64_t pointer_id = 0;
  PointerDeviceKind device_kind = PointerDeviceKind::Touch;
  Point position;
  Point window_position;
};

struct LongPressEvents {
  struct Started : Event<const LongPressEvent&> {};
  struct Ended : Event<const LongPressEvent&> {};
  struct Canceled : Event<const LongPressEvent&> {};
};
```

Movement beyond tolerance before the deadline rejects the recognizer.
Reaching the deadline accepts it and emits Started.
Up emits Ended, while a platform cancellation or another terminal condition that occurs while the modifier remains mounted emits Canceled.
A sequence that ends before recognition emits no lifecycle output.

Runtime schedules the nearest recognition deadline directly and does not poll each frame while a long press is pending.
Tooltip and text-selection long presses may retain component-specific output while sharing this recognizer and ownership model.

## Drag and release velocity

`DragGesture` recognizes a single pointer after movement crosses its threshold:

```cpp
return Text("Drag me")
    .With(
        DragGesture{},
        Offset{offset}
    )
    .On<DragEvents::Changed>([offset](const DragEvent& event) {
      offset = event.translation;
    })
    .On<DragEvents::Ended>([](const DragEvent& event) {
      StartMotion(event.velocity);
    });
```

Its public values are conceptually:

```cpp
struct DragGesture {
  std::optional<Axis> axis;
  std::optional<float> minimum_distance;
  std::optional<std::chrono::duration<double>> minimum_press_duration;

  bool operator==(const DragGesture&) const = default;
};

struct DragEvent {
  std::int64_t pointer_id = 0;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  Point origin;
  Point position;
  Point window_position;
  Point delta;
  Point translation;
  Point velocity;
};

struct DragEvents {
  struct Started : Event<const DragEvent&> {};
  struct Changed : Event<const DragEvent&> {};
  struct Ended : Event<const DragEvent&> {};
  struct Canceled : Event<const DragEvent&> {};
};
```

An empty axis permits free two-dimensional movement.
A Horizontal or Vertical axis recognizes and reports only that axis.
Acceptance emits Started followed by Changed with movement accumulated since Down in the same dispatch.
Later Move events emit Changed, Up emits Ended, and a mounted abnormal termination emits Canceled.

`minimum_press_duration` defaults to zero.
A nonzero value defines long-press-then-drag without adding a general gesture-sequence DSL.
Movement beyond tolerance before the deadline rejects the recognizer; reaching the deadline accepts it and emits a zero-translation Started event.
Later movement follows the ordinary Changed lifecycle.

The design does not add `FlingGesture`.
Drag reports terminal velocity, while the consumer decides whether that velocity starts retained motion.
Scroll containers pass it to `ScrollPhysics`; minimum velocity, maximum velocity, deceleration, and overscroll remain scrolling policy rather than gesture settings.

## Multi-pointer transform

`TransformGesture` combines translation, scale, and rotation for a pointer group:

```cpp
return Canvas(content)
    .With(TransformGesture{})
    .On<TransformEvents::Changed>([transform](const TransformEvent& event) {
      transform = {
          .translation = event.translation,
          .scale = event.scale,
          .rotation = event.rotation,
      };
    });
```

Its public values are conceptually:

```cpp
struct TransformGesture {
  bool translation_enabled = true;
  bool scale_enabled = true;
  bool rotation_enabled = true;
  std::optional<float> minimum_translation;
  std::optional<float> minimum_scale_change;
  std::optional<float> minimum_rotation;

  bool operator==(const TransformGesture&) const = default;
};

struct TransformEvent {
  Point centroid;
  Point window_centroid;
  Point translation;
  Point translation_delta;
  float scale = 1.0F;
  float scale_delta = 1.0F;
  float rotation = 0.0F;
  float rotation_delta = 0.0F;
  std::size_t pointer_count = 0;
};

struct TransformEvents {
  struct Started : Event<const TransformEvent&> {};
  struct Changed : Event<const TransformEvent&> {};
  struct Ended : Event<const TransformEvent&> {};
  struct Canceled : Event<const TransformEvent&> {};
};
```

At least two HuxerUI-owned pointers whose routes contain the same mounted Transform modifier are required.
PointerSessions whose routes contain that stable modifier identity share one private `TransformGestureRecognizer`.
The recognizer accepts when any enabled dimension crosses its threshold and then becomes the owner of every member session.
Scale begins at `1.0F`, rotation uses radians, and translation begins at zero.

Runtime finds a compatible pending or accepted Transform recognizer by scanning the existing active PointerSessions rather than maintaining a group registry.
The sessions and owner fields share that recognizer because it has one real lifetime spanning all member pointer sequences.
Single-pointer recognizers use the same private ownership representation without introducing a second recognizer interface.

Adding or removing a member pointer rebases current geometry while preserving cumulative output so the visible transform does not jump.
Dropping below two pointers ends an accepted transform and does not degrade it into a one-pointer Drag.
A Drag that already won cannot be stolen by a later second pointer.
When Transform accepts, Runtime first installs it as the owner of every member session, rejects the other recognizers in those sessions, cancels their raw targets, and then publishes Started and Changed.

## Coordinates and settings

Gesture events expose a frozen node-local coordinate and the current window coordinate.
Runtime captures the node transform when the recognizer is created and uses it for the complete gesture.
Moving or transforming the target in response to its events therefore does not feed back into later local deltas.

Distances use logical pixels, translation velocities use logical pixels per second, scale is multiplicative, and rotation uses radians.
Transform recognition computes pointer geometry in window space before projecting translation into the frozen local space.

Recognition defaults belong to one Runtime-level value rather than Theme, Environment, or mounted nodes:

```cpp
struct GestureSettings {
  float pointer_slop = 6.0F;
  float multi_tap_slop = 18.0F;
  std::chrono::duration<double> multi_tap_interval{0.3};
  std::chrono::duration<double> long_press_duration{0.5};
};
```

PlatformAdapter supplies available system values and shared fallbacks for unavailable settings.
The macOS adapter maps the system double-click interval; adapters without a stable native equivalent retain the shared fallback.
Individual modifiers override only their optional fields.
Each new recognizer snapshots its effective values, so compatible modifier updates do not reinterpret an active sequence.

GestureSettings is the only new shared configuration type.
There are no Environment values, revisions, registries, per-gesture settings services, or fling thresholds in this contract.

## Scrolling and retained pointer extensions

Each compatible scroll branch contributes one internal recognizer.
The recognizer retains its originating node and walks the session's committed route for ordered nested consumption; Runtime does not retain a parallel `scroll_chain` in PointerSession or duplicate ancestor lists per recognizer.
After axis movement crosses slop, the deepest recognizer that can consume movement accepts.
Existing `ScrollNodeBy`, nested remainder consumption, activity indication, `ScrollPhysics`, and retained momentum continue after acceptance.

A descendant Drag and an ancestor Scroll recognition compete only through recognition order and their thresholds.
A delayed Drag rejects movement before its deadline, allowing scrolling to accept.
No component-specific test is added to Runtime.

The NodeExtension pointer contract participates through one private recognition state.
Capture accepts ownership, Observe remains pending, and CancelTarget accepts while canceling the raw target.
Handled on Down also commits that extension as the owner of the physical sequence, preserving consumed input without allowing a raw target to receive later events.
Capture expresses the same continued-delivery ownership explicitly, while Ignored rejects the extension and creates no owner.
These results do not create extension-capture or observer collections in PointerSession.

Immediate component interactions such as Drawer, BottomSheet, and ScrollBar remain on NodeExtension::OnPointer because that path already participates in ownership resolution, out-of-bounds delivery, and cancellation.
A component uses a focused recognizer only when recognition itself needs pending state such as a deadline or movement tolerance; Tooltip touch long press follows that path.
Controlled values, component events, visuals, and physics stay in their owning modules.

## Text selection

TextSelectionOverlay is the one intentional pre-route arbitration entry because its committed handles and menu are not mounted View nodes.
When its geometry contains Down, it becomes the PointerSession owner directly, so underlying raw targets and mounted recognitions receive none of that physical sequence.
Overlay movement and termination then use the same PointerSession owner path and cancellation rules as recognized owners.

Editor selection contributes private recognizers from the mounted text-selection capability.
Its repeated-tap, touch long-press, and selection-drag state remains specialized because it edits grapheme-aware ranges and coordinates the platform text-input session.
Selection rendering, editing commands, menu actions, controlled TextEditingValue, and overlay geometry remain owned by the existing text-selection subsystem.

## Raw events and PlatformView ownership

Gesture output uses the existing `.On<Key>(handler)` surface.
Component events remain node-local and do not become routed events.

The raw pointer keys remain source compatible:

```cpp
ViewEvents::PointerDown
ViewEvents::PointerMove
ViewEvents::PointerUp
ViewEvents::PointerCancel
```

They have no capture phase, bubble phase, handled result, or public pointer handle.
The deepest ordinary target receives Down and Move while no competing owner exists.
A successful tap sends PointerUp before Click or MultiTap output; another owner sends one PointerCancel.

The committed RenderComposition decides initial ownership before Runtime creates a PointerSession:

```text
PlatformView wins Down
    → the native hierarchy owns the complete sequence
    → Runtime creates no HuxerUI session

HuxerUI wins Down
    → Runtime owns the complete sequence
    → no PlatformView receives it
```

Ownership never transfers across the PlatformView boundary after Down.
Every Transform member pointer must independently belong to HuxerUI.
Platform adapters keep host-level delivery active until physical Up, Cancel, capture loss, or device loss; shared Runtime alone chooses the gesture owner.

## Layers

Layers do not introduce a second input or gesture system.
LayerStack and its entries remain ordinary mounted nodes in the committed root, so reverse child hit testing naturally visits the frontmost visible layer before application content.
Layer content contributes raw targets and recognitions through the same hit route, PointerSession, and ownership resolution as other mounted content.

Layer pointer policies only determine what the committed hit route contains:

- `PassThrough` contributes no pointer target or recognizer.
- `Content` contributes only geometry hit by mounted layer content.
- `Barrier` contributes its full-viewport barrier target behind the layer content.

A PlatformView inside a layer still participates through the committed RenderComposition.
Initial Down arbitration therefore preserves the same exclusive PlatformView-or-HuxerUI ownership contract regardless of whether either target belongs to a layer.

Layer dismissal deactivates input before transition-out rendering completes.
Every active PointerSession whose retained route intersects the dismissed layer subtree is terminated and quarantined until physical Up or Cancel.
If its raw target is still mounted, it receives PointerCancel synchronously; an accepted recognizer whose modifier remains mounted receives Canceled.
The remaining physical sequence cannot fall through to application content or a PlatformView behind the layer.

Because a Barrier handler may dismiss its layer from raw PointerDown, Runtime installs the pending PointerSession before invoking that handler.
Dismissal can then quarantine the installed session without retaining a stack-local session reference across application output.
Captured Environment affects the layer's mounted declarations and event handlers only; it does not create another pointer owner, route, or recognizer lifetime.

## Cancellation, unmount, and quarantine

Up completes the owner and removes the session.
Platform Cancel, capture loss, window deactivation, or device loss first terminates pointer ownership and then removes the session.
Runtime destruction discards sessions before mounted nodes without invoking application callbacks during teardown.

Disable, subtree deactivation, modifier removal, incompatible replacement, or unmount terminates recognition without retaining callbacks from declarations that no longer exist.
If the recognized modifier remains mounted, an accepted recognizer receives Canceled.
If the modifier or node has left the mounted tree, destruction is its lifecycle termination and no application callback is invoked afterward.

A nonterminal physical sequence whose HuxerUI owner disappears becomes quarantined:

```text
clear raw target, interaction, recognitions, and owner
    ↓
retain the PointerSession with no dispatch target
    ↓
ignore remaining Move events
    ↓
remove the session on physical Up or Cancel
```

Quarantine prevents one physical sequence from retargeting a newly mounted HuxerUI node or PlatformView.
It is one bit on PointerSession, not a tombstone registry or another ownership system.

Runtime changes ownership and terminal state before invoking any mounted Canceled handler.
An escaping application exception therefore cannot leave a live owner, duplicate cancellation, or retarget the remaining sequence.
Compatible modifier reconciliation preserves mounted recognizer identity and current event bindings while each active recognizer retains its original recognition configuration.

## Accessibility

Gesture modifiers do not synthesize semantics because physical motion does not identify the application operation.
A Slider continues to expose adjustable semantics, a reorderable collection provides suitable custom actions, and a transformable Canvas provides equivalent controls when the operation is essential.
MultiTap, LongPress, Drag, or Transform must not be the only way to perform an important action without an accessible alternative.

## Performance

Ownership resolution has no work while no pointer sequence is active.
Each active pointer owns one bounded session and a recognition list proportional to its mounted hit route.
Movement sampling uses fixed-capacity storage, and long-press recognition schedules one deadline instead of continuous frames.
Pointer movement changes retained recognizer state directly; only application handlers that update State schedule recomposition.

## Type and implementation ownership

The public surface adds only independently meaningful values:

- `MultiTapGesture`, `LongPressGesture`, `DragGesture`, and later `TransformGesture` are retained modifiers.
- Their event payloads carry different semantic data and remain separate values instead of inheriting from a generic gesture-event base.
- Their grouped event keys follow the existing ViewEvents, SliderEvents, and TabsEvents convention.
- GestureSettings contains only shared recognition defaults.

Ownership resolution, recognizers, tap accumulation, routes, quarantine, and movement samples remain private implementation details.
There is no public recognizer base class, gesture controller, ownership handle, capture token, result wrapper, event phase, event context, or generic gesture payload.
PointerSession stores private shared recognizer ownership because one Transform recognizer can span several sessions; this is the existing session graph's ownership mechanism rather than a public abstraction or group registry.

Implementation ownership is focused:

- `<huxerui/gesture.h>` owns public gesture modifiers, event keys, payloads, and GestureSettings.
- `src/gesture.cpp` owns retained gesture modifier behavior and private recognizer implementations.
- One focused private header owns the recognizer contract used by gesture modifiers, pointer routing, and scrolling.
- `src/runtime_pointer_interaction.cpp` owns PointerSession, raw delivery, ownership resolution, and quarantine.
- Existing scrolling code owns delta consumption and momentum after its recognizer accepts.
- Platform adapters own timestamps, pointer identifiers, host-level sequence delivery, cancellation, and native settings mapping.

Runtime dispatches the private recognizer capability and never checks for LongPress, Drag, Transform, Drawer, BottomSheet, Slider, or another concrete component kind.
The existing generic checks for semantic activation, scrolling capability, and NodeExtension pointer capability create their respective internal recognizers.

## Delivery sequence

Implementation proceeds in independently reviewable stages:

- Implemented: refactor current Click, raw pointer events, scrolling recognition, NodeExtension pointer behavior, and text-selection pointer handling onto PointerSession and shared ownership resolution without adding public gesture API.
- Implemented: add GestureSettings, normalized timing, MultiTapGesture, LongPressGesture, and DragGesture as recognizers on the established path.
- Implemented: migrate Tooltip touch long press to a focused recognizer while keeping immediate Drawer, BottomSheet, and ScrollBar interactions on the NodeExtension pointer adapter.
- Add TransformGesture, pointer groups, and multi-pointer capture-loss coverage on every supported adapter.
- Design typed DragSource, DropTarget, payload transfer, previews, and native drag-and-drop separately.

The pointer-core stage must preserve existing Click, raw event, focus, interaction, scrolling, momentum, and NodeExtension outcomes before new public behavior is added.
Later stages validate deterministic nesting, movement outside bounds, cancellation, quarantine, disable, layer dismissal and pointer policies, multiple pointer IDs, PlatformView exclusivity, and compatible modifier reconciliation.
Platform tests additionally verify that one physical sequence cannot reach both a PlatformView and HuxerUI.
