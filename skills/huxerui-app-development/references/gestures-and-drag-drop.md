# Gestures and Typed Drag-and-Drop

Use the recognizers in `<huxerui/gesture.h>` instead of rebuilding gesture state from raw pointer events.
Ordinary activation remains `.OnClick(...)` because Click also covers keyboard and accessibility invocation.

## Typed gesture lifecycle

- `MultiTapGesture` emits `MultiTapEvents::Recognized` after its configured number of successful taps.
- `LongPressGesture` emits `LongPressEvents::Started`, `Ended`, and `Canceled`.
- `DragGesture` emits `DragEvents::Started`, `Changed`, `Ended`, and `Canceled`; configure an optional axis, distance, or press duration.
- `TransformGesture` emits `TransformEvents::Started`, `Changed`, `Ended`, and `Canceled` after two or more compatible pointers participate.

Gesture coordinates use logical pixels.
Node-local coordinates remain based on the transform captured when recognition begins, while window coordinates track the host window.
Handle `Canceled` whenever accepted gesture state or visuals need cleanup.
Do not combine raw pointer handling with a built-in recognizer to observe or own the same physical operation.

## Pointer events

`PointerEvent` reports `type`, `pointer_id`, window-logical `position`, `device_kind`, consecutive `click_count`, `changed_button`, and `pressed_buttons`.
Its `PointerEventType` is `Down`, `Move`, `Up`, or `Cancel`, and `PointerDeviceKind` distinguishes Mouse, Touch, and Pen.
`PointerButton` is a flag enum with `Primary`, `Secondary`, `Middle`, `Back`, and `Forward`; Primary and Secondary follow system roles rather than fixed physical left and right positions.
`changed_button` is the button added by Down or removed by Up, while `pressed_buttons` is the complete post-event mask.
Use `event.IsButtonPressed(mask)` for raw chord logic instead of inferring a button from `click_count`.

`ViewEvents::PointerDown`, `PointerMove`, `PointerUp`, and `PointerCancel` are void notifications for the deepest eligible raw target.
They do not capture, bubble, return a handled result, or acquire pointer ownership.
When another recognizer accepts after raw Down, the raw target receives one PointerCancel and no later event from that sequence.
Use `.OnClick(...)` for semantic activation, a built-in gesture for standard recognition, and `PointerIntercept` only for custom synchronous ownership decisions driven by pointer updates.
Built-in Click, selection, gestures, scrolling, and retained component interaction recognize only an unchorded Primary sequence.
Middle, Back, and Forward remain available to raw pointer handlers and PointerIntercept without implicit semantic behavior.

Bind `ViewEvents::ContextMenuRequested` for context actions:

```cpp
return content.On<ViewEvents::ContextMenuRequested>([menu](Point position) {
  menu.ShowAt(position, {MenuItem("Refresh", Refresh)});
});
```

An unchorded Secondary tap invokes the deepest enabled binding after raw PointerUp and supplies its window-logical release position.
The Context Menu key and Shift+F10 use the nearest enabled binding on the focused route and supply that View's center.
Binding presence claims the request, so do not add a handled result, manually search parents, or rebuild secondary-tap recognition from PointerDown.
PlatformViews retain native context menus, and Web preserves a pointer-initiated browser menu outside HuxerUI content that declares this binding.

## Pointer interception

Bind `ViewEvents::PointerIntercept` when application-level pointer logic must observe a sequence and synchronously decide when to own it.
Its signature is `Event<bool(const PointerEvent&)>`:

```cpp
return content.On<ViewEvents::PointerIntercept>([](const PointerEvent& event) {
  return event.type == PointerEventType::Move && ShouldTakePointer(event.position);
});
```

Returning false keeps the recognition pending; the first deepest-to-root handler that returns true becomes the sole PointerSession owner, cancels an already-started raw target, and receives subsequent Move, Up, and Cancel outside its original bounds.
Once accepted, later return values are ignored.
Compatible recomposition keeps ownership on the same mounted View and subsequent updates use its current PointerIntercept binding.
This is one recognizer in the existing PointerSession, not event capture, bubbling, or a public pointer handle.

Use `PointerIntercept` for decisions driven by incoming pointer updates.
Use `LongPressGesture` or a delayed `DragGesture` when recognition must advance at a deadline while the pointer is stationary; a synchronous event return cannot acquire ownership when no event is being dispatched.
PlatformView ownership is still decided at initial Down and never transfers across the native boundary afterward.
An accepted PointerIntercept may continue to own multi-button chords; adding a button cancels pending or accepted standard recognition.

## Multi-pointer transform

`TransformEvent` reports incremental values rather than an authoritative accumulated transform:

- `pan` is centroid displacement since the previous update;
- `scale` is a multiplicative factor whose identity is `1.0F`;
- `rotation` is an angular delta in radians, positive clockwise;
- `centroid` and `window_centroid` identify the current center;
- `pointer_count` reports current participation.

Retain accumulated transform state in the application: add `event.pan` to the offset, multiply the scale by `event.scale`, and add `event.rotation` to the angle. Bind those updates through `TransformEvents::Changed` on the transformed content.

`Point` supports addition, subtraction, scalar multiplication, and scalar division, including their compound-assignment forms.
Adding or removing a pointer while at least two remain can produce an identity `Changed` event that rebases the calculation; do not treat every `Changed` event as nonzero motion.

## Drag and release motion

`DragEvent` exposes origin, current local and window positions, incremental `delta`, total `translation`, and recent velocity in logical pixels per second.
Use terminal velocity as input to application-owned retained motion; HuxerUI does not expose a separate fling gesture.

A positive `DragGesture::minimum_press_duration` creates press-then-drag behavior suitable for reorder interactions inside scrolling content.
An axis restricts recognition and reported local movement to that axis.
Do not combine `DragGesture` and `DragSource` on one node for the same physical operation.

## Typed in-process drag-and-drop

`DragSource(payload)` stores one immutable application value and uses ordinary `DragGesture` recognition.
An optional preview factory returns normal `View` content presented in a non-interactive layer.
`DropTarget::Accepts<T>()` accepts only the exact unqualified payload type; its optional predicate must be quick and free of side effects.

```cpp
return CardView(card)
    .With(DragSource(CardTransfer{card.id}, [card] { return CardPreview(card); }))
    .On<DragSourceEvents::Ended>([](const DragDropResult& result) {
      ReportDropResult(result.dropped);
    });
```

Targets bind `DropEvents<T>::Entered`, `Moved`, `Exited`, and `Dropped` through `.On<Key>(...)`.
Perform the authoritative application mutation from `Dropped`; the payload reference is valid only during the callback unless copied or represented by a shared-ownership value.
The source receives `DragSourceEvents::Ended` with `DragDropResult::dropped`, or `Canceled` on abnormal termination.

Compatible targets inside scroll content enable edge auto-scroll through the existing scroll hierarchy.
This contract is in-process only: it does not implicitly transfer files, text, URLs, platform drag-session values, or input ownership to a `PlatformView`.
Provide equivalent keyboard or semantic actions when drag-and-drop performs an essential operation.

## Configuration and ownership

Recognition thresholds default to platform-provided `GestureSettings`; prefer per-gesture optional overrides when application behavior genuinely requires them.
Built-in Click, scrolling, public gestures, and retained pointer behavior share one ownership model, so one accepted recognizer cancels competing recognition and raw delivery.
Recognizer lifecycle and competition state remain mounted without recomposition. An application handler that updates `State` intentionally recomposes its subscribers; use that path when the accumulated value is authoritative application state. Use retained mounted behavior only when high-frequency visual state must advance independently of composition.
