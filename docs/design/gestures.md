# Gesture Recognition and Arbitration Design

This document defines the shared gesture-recognition, ownership-resolution, and cancellation model.
Pointer interception, repeated taps, long presses, single-pointer drags, and multi-pointer transforms share one ownership path.
The model extends the interaction foundation without adding general event capture and bubbling or exposing raw pointer capture to applications.

The mounted interaction and indication contract remains defined by [Interaction and Indication Design](interaction-indication.md).
PlatformView composition and initial input ownership remain defined by [Architecture Design](architecture.md#platform-content-integration).

## Goals

- Recognize repeated taps, long presses, single-pointer drags, and multi-pointer transforms in the shared Runtime.
- Give Click, scrolling, public gesture modifiers, and retained pointer extensions one deterministic competition model.
- Keep every physical pointer sequence exclusively owned by HuxerUI or one PlatformView after its Down event.
- Continue delivery to an accepted recognizer outside its node bounds until completion or cancellation.
- Let application code synchronously intercept a pointer sequence without adding general event routing.
- Preserve typed event identity, `OnClick()`, raw pointer events, and the existing NodeExtension pointer contract.
- Keep recognition state mounted and incremental without recomposing for every pointer update.
- Let scrolling, Drawer, BottomSheet, ScrollBar, Tooltip, text selection, and third-party retained behavior converge on one pointer path.

## Non-goals

- The design does not add a public single-tap gesture that duplicates Click.
- It does not add DOM-style capture, target, and bubble phases to typed events.
- It does not expose application-facing pointer-capture, release, or transfer handles.
- It does not add `OnTap()`, `OnDrag()`, or other View convenience members.
- It does not define public recognizer priority, simultaneous recognition, failure dependencies, or a gesture-composition DSL.
- Typed in-process drag-and-drop is defined separately and does not add another recognition path.
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
one recognition owner
        ↓
PointerIntercept / Click / Drag / LongPress / Transform / Scroll / retained pointer extension
```

Raw pointer target delivery remains a side branch while recognition is pending:

```text
                    ┌─ raw ViewEvents::Pointer (Down / Move / Up / Cancel)
PointerSession ─────┤
                    └─ ownership resolution → one owner
```

The raw branch reports the physical stream to the deepest ordinary pointer target while recognition resolves ownership.
The aggregate `ViewEvents::Pointer` handler cannot stop propagation, accept a gesture, capture a pointer, or create another owner.
`ViewEvents::PointerIntercept` is a distinct typed event key whose result participates as one recognition in the existing PointerSession.
When a competing recognizer accepts, the raw target receives one Cancel update and no later event from that sequence.

Runtime does not retain separate scroll, extension-capture, extension-observer, and gesture-session ownership paths.
Scroll recognition and the existing NodeExtension pointer capability are recognizers alongside built-in and public recognizers.

Hover and `ScrollInputEvent` remain outside pointer recognition because neither represents an owned Down-to-Up pointer sequence.
Hover performs an ordinary stateless hit test only when no active pointer delivery owns the event.
`ScrollInputEvent` is a platform-recognized wheel or trackpad delta and enters the nested scroll transaction without competing with Drag.
Its complete ownership and consumption contract is defined by [Scrolling](scrolling.md).

## Pointer buttons and chords

`PointerButton` is a flag enum containing `Primary`, `Secondary`, `Middle`, `Back`, and `Forward`.
Primary and Secondary describe system button roles rather than physical left and right positions, so platform adapters preserve the user's handedness setting.
Touch contacts and a pen tip use Primary, while an available pen barrel button uses Secondary.

Each `PointerEvent` carries two related facts:

```cpp
PointerButton changed_button = PointerButton::None;
PointerButton pressed_buttons = PointerButton::None;
```

`changed_button` identifies the one button added or removed by Down or Up and is None for Move and Cancel.
`pressed_buttons` is the complete button state after the event, so Down includes its changed button and Up excludes it.
`IsButtonPressed()` tests whether every button in a nonempty flag mask is present.
Button state belongs to one pointer identifier rather than forming a device-global registry.

The first mouse-button Down creates a PointerSession and the final Up whose `pressed_buttons` is None completes it.
Additional Down and Up events follow the committed route of that session.
Raw pointer handlers and PointerIntercept receive the complete chord and can implement custom multi-button interaction.

Built-in activation, gestures, scrolling, text selection, and retained component behavior accept only an unchorded Primary sequence.
A second button cancels their pending or accepted interaction and quarantines standard recognition until every button is released.
An accepted PointerIntercept continues receiving chord updates because it is the application-defined ownership path.
Secondary context-menu recognition is likewise unchorded, while Middle, Back, and Forward have no built-in semantic action.
PlatformView ownership remains native and receives the host's complete button behavior after it wins the initial Down.

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
The algorithm deactivates rejected recognitions and records at most one owner per PointerSession.
It does not need separate Possible, Accepted, Rejected, Completed, and Canceled state objects because those states follow directly from pending recognition, the owner field, and session termination.

Ordinary recognizers belong to one PointerSession.
A multi-pointer recognizer may be referenced by several sessions, while each session still stores its own recognition index as owner.
Runtime resolves every session that references the same recognizer before publishing output, so shared recognition does not require a pointer-group registry, a second router, or another public ownership type.

Recognitions are collected from the deepest mounted node toward the root.
`ViewEvents::PointerIntercept` is first on its mounted node when present.
Retained-modifier recognitions on the same node follow reverse declaration order, matching existing topmost extension dispatch.
The node's built-in Tap or Scroll recognition follows its retained-modifier recognitions.
The first recognition to return Accept in that deterministic order owns the sequence.

Runtime resolves immediate recognizers before publishing raw Down.
If a retained pointer extension returns Capture or consumes Down, it can win without exposing a transient raw Down/Cancel pair that did not exist before this design.
Otherwise Runtime installs the pending PointerSession before publishing raw Down, so a handler that dismisses a Layer can safely quarantine that same session without invalidating stack-local ownership state.

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

## Pointer interception

Application code binds the ordinary typed key when it needs to observe a physical sequence and synchronously decide when to own it:

```cpp
return content.On<ViewEvents::PointerIntercept>([](const PointerEvent& event) {
  return ShouldTakePointer(event);
});
```

The event signature is `Event<bool(const PointerEvent&)>`.
Returning false on Down or Move keeps that recognition pending and allows deeper or later candidates and the raw target to continue.
The first candidate that returns true becomes the sole owner before later candidates see that update.
If the raw target already observed Down, it receives one Cancel update; immediate Down acceptance prevents raw Down entirely.

Once accepted, the accepted View's current PointerIntercept binding receives subsequent Move, Up, and Cancel outside its original bounds, and later return values are ignored.
If another recognition accepts first, a pending interceptor that observed the sequence receives Cancel exactly once.
Removal, disable, incompatible replacement, or unmount never transfers an accepted sequence to another node.
An escaping handler exception follows the existing quarantine and rethrow contract.

Pointer interception advances only while Runtime dispatches a PointerEvent.
A recognition that must accept at a deadline while the pointer is stationary uses the existing retained recognizer path, such as `LongPressGesture` or a delayed `DragGesture`; the Runtime schedules that deadline and resolves ownership before publishing its lifecycle event.
This distinction avoids a public asynchronous capture handle or another pointer-session API.

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
Raw Up is emitted before the successful tap output, preserving current event order.

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

## Context-menu requests

An unchorded Secondary tap can resolve one semantic context-menu request through the existing PointerSession:

```cpp
return content.On<ViewEvents::ContextMenuRequested>([menu](Point position) {
  menu.ShowAt(position, {
    MenuItem("Copy", Copy),
    MenuItem("Delete", Delete),
  });
});
```

The event signature is `Event<void(Point)>`, and its position is window-local logical geometry from the successful Up.
The deepest enabled binding on the committed route is the single candidate.
Binding presence claims the request; a result-controlled parent search would reintroduce event bubbling through a semantic callback.
Movement outside successful-tap rules, a chord, PointerIntercept acceptance, cancellation, disable, or removal rejects the request without transferring it to another node.
Compatible recomposition retains the candidate identity and uses its current binding.

The Context Menu key and Shift+F10 invoke the same event for the nearest enabled binding on the focused route.
Keyboard invocation uses the focused View's presentation-bounds center.
Runtime does not convert touch LongPress into this event because LongPressGesture already owns application-defined touch policy.

`MenuHandle::ShowAt()` remains the only application menu-placement path.
There is no ContextMenu controller, separate menu model, event capture or bubbling phase, or `OnContextMenu()` convenience member.
PlatformViews retain their native context-menu behavior, and Web suppresses a pointer-initiated browser menu only for a request claimed by HuxerUI content.

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
  struct Recognized : Event<void(const MultiTapEvent&)> {};
};
```

Runtime recognizes repeated taps from successful shared tap results using GestureSettings.
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
  struct Started : Event<void(const LongPressEvent&)> {};
  struct Ended : Event<void(const LongPressEvent&)> {};
  struct Canceled : Event<void(const LongPressEvent&)> {};
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
  struct Started : Event<void(const DragEvent&)> {};
  struct Changed : Event<void(const DragEvent&)> {};
  struct Ended : Event<void(const DragEvent&)> {};
  struct Canceled : Event<void(const DragEvent&)> {};
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

`TransformGesture` recognizes pan, scale, and rotation from two or more compatible pointers:

```cpp
return Canvas(content)
    .With(TransformGesture{})
    .On<TransformEvents::Changed>([=](const TransformEvent& event) {
      offset += event.pan;
      scale *= event.scale;
      rotation += event.rotation;
    });
```

Its public values are conceptually:

```cpp
struct TransformGesture {
  bool operator==(const TransformGesture&) const = default;
};

struct TransformEvent {
  PointerDeviceKind device_kind = PointerDeviceKind::Touch;
  std::uint32_t pointer_count = 0;
  Point centroid;
  Point window_centroid;
  Point pan;
  float scale = 1.0F;
  float rotation = 0.0F;
};

struct TransformEvents {
  struct Started : Event<void(const TransformEvent&)> {};
  struct Changed : Event<void(const TransformEvent&)> {};
  struct Ended : Event<void(const TransformEvent&)> {};
  struct Canceled : Event<void(const TransformEvent&)> {};
};
```

The first pointer keeps Transform pending and may still become Click, Drag, Scroll, or another owner.
A second pointer of the same device kind that reaches the same mounted Transform modifier accepts immediately and atomically gives that recognition ownership of both PointerSessions.
An already accepted single-pointer owner cannot be transferred into Transform when another pointer arrives.

Transform values are incremental.
`pan` is the local centroid displacement since the previous update, `scale` is the multiplicative spread change with an identity value of one, and `rotation` is the angular change in radians.
Positive rotation follows HuxerUI's downward Y axis and is clockwise.
Applications retain their authoritative accumulated transform rather than receiving a second framework-owned transform value.

Adding a third or later pointer rebases the calculation and emits an identity Changed event with the new pointer count.
Removing a pointer while at least two remain does the same.
Dropping below two pointers emits Ended and quarantines the remaining physical sequence so it cannot become a new Click, Drag, Scroll, or PlatformView interaction.
Platform Cancel, ownership invalidation, or an exception cancels the complete shared recognition once.

The recognizer calculates its centroid from all active pointers, scale from their root-mean-square distance around that centroid, and rotation from matched centered pointer vectors.
It freezes the owning node coordinate transform when the first pointer starts and rebases geometry whenever the participating pointer set changes, preventing target motion or pointer-count changes from feeding discontinuities back into later deltas.

## Coordinates and settings

Gesture events expose a frozen node-local coordinate and the current window coordinate.
Runtime captures the node transform when the recognizer is created and uses it for the complete gesture.
Moving or transforming the target in response to its events therefore does not feed back into later local deltas.

Distances use logical pixels, translation velocities use logical pixels per second, scale is multiplicative, and rotation uses radians.

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
The macOS adapter maps the system double-click interval; adapters without a stable platform equivalent retain the shared fallback.
Individual modifiers override only their optional fields.
Each new recognizer snapshots its effective values, so compatible modifier updates do not reinterpret an active sequence.

GestureSettings is the only new shared configuration type.
There are no Environment values, revisions, registries, per-gesture settings services, or fling thresholds in this contract.

## Scrolling and retained pointer extensions

Each compatible scroll branch contributes one internal recognizer.
The recognizer retains its originating node and walks the session's committed route through the transaction defined by [Scrolling](scrolling.md); Runtime does not retain a parallel `scroll_chain` in PointerSession or duplicate ancestor lists per recognizer.
After axis movement crosses slop, the deepest recognizer that can consume movement accepts.
Touch may also accept at a boundary when terminal overscroll is enabled.

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

The raw pointer lifecycle remains one notification:

```cpp
return content.On<ViewEvents::Pointer>([](const PointerEvent& event) {
  ObserveRawPointer(event);
});
```

`ViewEvents::Pointer` has the `Event<void(const PointerEvent&)>` signature and reports Down, Move, Up, and Cancel through `PointerEvent::type`.
It has no capture phase, bubble phase, handled result, or public pointer handle.
Applications use the separate `ViewEvents::PointerIntercept` key when they need to compete for ownership.
The deepest ordinary target receives Down and Move while no competing owner exists.
A successful tap sends Up before Click or MultiTap output; another owner sends one Cancel update.

## Hover lifecycle

`ViewEvents::Hover` reports `HoverEventType::Enter`, `Move`, and `Leave` for mouse and pen pointers.
It is a notification with `Event<void(const HoverEvent&)>`, not a gesture recognizer, ownership request, or pointer-session participant.
Touch input does not produce Hover.

Runtime retains one hover-capable pointer identity, device kind, latest window position, and the currently matched public handlers and retained extensions.
This is one projection of the existing pointer hit route rather than a `HoverSession`, registry, or second input path.
An exact duplicate host position does not emit Move.
After final presentation geometry settles, Runtime resolves the same retained position again so recomposition, layout, clipping, transforms, layer dismissal, and unmount can emit Enter or Leave without synthetic movement.

A View with only a Hover handler is eligible for the Hover route but not the ordinary pointer route, so a visual hover overlay does not block Click or raw input behind it.
Disabled Views remain eligible because Hover describes pointer presence rather than activation.
Nested bound Views each receive a direct containment lifecycle with positions converted into their own local coordinate spaces; delivery does not introduce capture, bubbling, handled results, or propagation control.
Enter and Move are delivered from root to deepest bound View, while Leave is delivered from deepest bound View toward the root.

Hover-capable NodeExtensions share the resolved branch and receive the same complete `HoverEvent` through `OnHover()`.
Their existing `HoverHitTest()` and `HoverWhenDisabled()` capabilities continue to own specialized geometry and disabled affordances.
Tooltip uses Move to restart its stationary-hover delay and immediately dismiss a visible hover-owned surface, while focus-owned visibility remains independent.

If the deepest hit node is a `PlatformView`, the native hierarchy owns hover and the HuxerUI route is cleared at that boundary.
Platform exit or cancellation also produces Leave and resets the retained hover state.

## Pointer cursor declarations

`PointerCursor` is an ordinary property modifier rather than an event, gesture recognizer, or `NodeExtension` capability.
Its optional `PointerCursorKind` value is stored in `ViewProperties` and makes that node eligible for cursor resolution without consuming input, changing enabled state, requesting focus, or changing window-drag hit testing.
Cursor resolution reuses the pointer route traversal with cursor declarations as additional terminal candidates; ordinary pointer routing does not include those candidates, so a cursor-only overlay cannot block an interactive sibling behind it.

Runtime scans the committed hit route from deepest node to root and uses the first explicit declaration.
An explicit `PointerCursorKind::Default` stops ancestor fallback, and a disabled View participates in cursor resolution because the declaration describes presentation rather than interaction ownership.
If the deepest hit node is a `PlatformView`, Runtime resolves the HuxerUI surface to `Default` and the native view hierarchy owns the actual cursor.

Mouse and pen movement retain the latest window position.
Runtime resolves the cursor both when input arrives and after final presentation geometry settles in `BuildFrame()`, so recomposition, transforms, layout changes, or a dynamic `State<PointerCursorKind>` update the cursor under a stationary pointer.
Touch input does not change cursor state, and pointer cancellation restores `Default`.
Runtime sends only the resolved kind through `PlatformAdapter::SetPointerCursor()`; platform adapters own native mapping and unsupported hosts may ignore it.

This design does not add a resolver callback, cursor controller, pointer context, or duplicate hit-test implementation.
Custom Canvas content computes a `PointerCursorKind` into ordinary State and declares it with `.With(PointerCursor(kind))`.

The committed RenderComposition decides initial ownership before Runtime creates a PointerSession:

```text
PlatformView wins Down
    → the platform hierarchy owns the complete sequence
    → Runtime creates no HuxerUI session

HuxerUI wins Down
    → Runtime owns the complete sequence
    → no PlatformView receives it
```

Ownership never transfers across the PlatformView boundary after Down.
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
If its raw target is still mounted, it receives a raw Cancel update synchronously; an accepted recognizer whose modifier remains mounted receives Canceled.
The remaining physical sequence cannot fall through to application content or a PlatformView behind the layer.

Because a Barrier handler may dismiss its layer from raw Down, Runtime installs the pending PointerSession before invoking that handler.
Dismissal can then quarantine the installed session without retaining a stack-local session reference across application output.
Captured Environment affects the layer's mounted declarations and event handlers only; it does not create another pointer owner, route, or recognizer lifetime.

## Cancellation, unmount, and quarantine

The final button Up completes the owner and removes the session.
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
MultiTap, LongPress, or Drag must not be the only way to perform an important action without an accessible alternative.

## Performance

Ownership resolution has no work while no pointer sequence is active.
Each active pointer owns one bounded session and a recognition list proportional to its mounted hit route.
Movement sampling uses fixed-capacity storage, and long-press recognition schedules one deadline instead of continuous frames.
Pointer movement changes retained recognizer state directly; only application handlers that update State schedule recomposition.

## Type and implementation ownership

The public surface adds only independently meaningful values:

- `MultiTapGesture`, `LongPressGesture`, `DragGesture`, and `TransformGesture` are retained modifiers.
- Their event payloads carry different semantic data and remain separate values instead of inheriting from a generic gesture-event base.
- Their grouped event keys follow the existing ViewEvents, SliderEvents, and TabsEvents convention.
- GestureSettings contains only shared recognition defaults.
- PointerButton is one flag enum shared by raw input and built-in recognition; there is no PointerButtons wrapper.

Ownership resolution, shared recognizer identity, tap accumulation, routes, quarantine, and movement samples remain private implementation details.
There is no public recognizer base class, gesture controller, ownership handle, capture token, result wrapper, event phase, event context, or generic gesture payload.

Implementation ownership is focused:

- `<huxerui/gesture.h>` owns public gesture modifiers, event keys, payloads, and GestureSettings.
- `src/runtime/gesture.cpp` owns retained gesture modifier behavior and private recognizer implementations.
- One focused private header owns the recognizer contract used by gesture modifiers, pointer routing, and scrolling.
- `src/runtime/runtime_pointer_interaction.cpp` owns PointerSession, raw delivery, ownership resolution, and quarantine.
- Existing scrolling code owns delta consumption and momentum after its recognizer accepts.
- Platform adapters own timestamps, pointer identifiers, semantic button mapping, complete pressed-button masks, host-level sequence delivery, cancellation, and platform settings mapping.

Runtime dispatches the private recognizer capability and never checks for LongPress, Drag, Drawer, BottomSheet, Slider, or another concrete component kind.
The existing generic checks for semantic activation, scrolling capability, and NodeExtension pointer capability create their respective internal recognizers.

Platform tests verify that one physical sequence cannot reach both a PlatformView and HuxerUI.
