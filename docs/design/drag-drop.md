# Typed Drag-and-Drop Design

This document defines in-process typed drag-and-drop between mounted HuxerUI nodes.
It extends the shared gesture ownership path without adding public pointer capture, event propagation, a drag registry, or platform transfer formats.

The underlying recognizer and PointerSession rules remain defined by [Gesture Recognition and Arbitration](gestures.md).
Layer ownership remains defined by [Architecture](architecture.md), and scrolling remains defined by [Incremental Layout and Rendering](incremental-rendering.md).

## Goals

- Transfer an immutable application value from one mounted source to one compatible mounted target.
- Preserve exact typed event identity without exposing a type-erased public payload.
- Reuse DragGesture thresholds, delayed recognition, coordinates, velocity, and PointerSession ownership.
- Keep preview presentation, target selection, nested scrolling, cancellation, and exceptions deterministic.
- Retain source and target NodeExtension identity across compatible reconciliation.
- Keep ordinary drag behavior, PlatformView arbitration, layers, and scrolling on their existing ownership paths.

## Non-goals

- The design does not add capture or bubble phases to typed events.
- It does not add a public DragDropSession, controller, context, payload wrapper, candidate list, or target registry.
- It does not serialize application values or expose them to native drag systems.
- It does not define copy, move, or link effects; the application owns the data mutation performed by Dropped.
- It does not infer accessible business operations from an arbitrary payload.
- It does not make PlatformView a drop target in the initial implementation.

## Public model

All public declarations remain in `<huxerui/gesture.h>`.

`DragSource` is a retained modifier with a templated payload constructor:

```cpp
class DragSource {
public:
  template <class T>
  explicit DragSource(T payload, DragGesture gesture = {});

  template <class T>
  DragSource(T payload, std::function<View()> preview, DragGesture gesture = {});
};
```

The payload is stored as its unqualified value type.
An rvalue move-only value is supported because compatible View and modifier copies retain shared ownership of the immutable stored object.
The optional preview factory creates ordinary View content in a non-interactive Layer.

`DropTarget` is a retained modifier created for one exact value type:

```cpp
class DropTarget {
public:
  template <class T>
  static DropTarget Accepts();

  template <class T, class Predicate>
  static DropTarget Accepts(Predicate predicate);
};
```

The predicate is the only synchronous application callback in the target declaration.
It must be quick and free of side effects because Runtime may call it while selecting a target.
Target lifecycle notifications remain ordinary typed events.

The public event values are:

```cpp
struct DropEvent {
  std::int64_t pointer_id = 0;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  Point position;
  Point window_position;
};

struct DragDropResult {
  DragEvent drag;
  bool dropped = false;
};

struct DragSourceEvents {
  struct Started : Event<const DragEvent&> {};
  struct Changed : Event<const DragEvent&> {};
  struct Ended : Event<const DragDropResult&> {};
  struct Canceled : Event<const DragEvent&> {};
};

template <class T> struct DropEvents {
  struct Entered : Event<const T&, const DropEvent&> {};
  struct Moved : Event<const T&, const DropEvent&> {};
  struct Exited : Event<const T&, const DropEvent&> {};
  struct Dropped : Event<const T&, const DropEvent&> {};
};
```

`DropEvents<T>` requires an unqualified non-reference `T`, so one payload type has one event identity.
The payload reference is valid only for the callback invocation; applications retain it by copying the value or by choosing a shared-ownership payload type.

## Usage

```cpp
struct CardTransfer {
  std::uint64_t id = 0;
};

return Row {
  CardView(card)
      .With(DragSource(
          CardTransfer{card.id},
          [card] { return CardPreview(card); }
      )),
  Column {
    Text("Drop here"),
  }
      .With(DropTarget::Accepts<CardTransfer>(
          [column](const CardTransfer& transfer) {
            return column.CanAccept(transfer.id);
          }
      ))
      .On<DropEvents<CardTransfer>::Dropped>(
          [=](const CardTransfer& transfer, const DropEvent&) {
            MoveCard(transfer.id, column.id);
          }
      ),
};
```

Reorder interactions inside scrolling content normally configure `DragGesture::minimum_press_duration`, while direct-manipulation surfaces may use the ordinary movement threshold.
Applications do not combine DragSource and DragGesture on the same node to observe one physical operation.

## Payload ownership

The source declaration stores one exact `std::type_index` and one `std::shared_ptr<const void>`.
No public operation exposes those erased values.

The pointer sequence snapshots the source payload, DragGesture configuration, preview factory, source handle, and source Environment when recognition begins.
Compatible source updates affect the next sequence but do not replace an active transfer.

Targets retain an exact type identity, an optional erased predicate, and a generated dispatch function that restores `const T&` before emitting `DropEvents<T>`.
Changing a compatible target declaration updates its type, predicate, and dispatch behavior without replacing the NodeExtension.
An entered target snapshots that typed dispatch until it exits so a later declaration with a different payload type cannot reinterpret the active payload.

## One ownership path

DragSource contributes a recognizer to the existing ordered PointerSession recognition list.
It shares the private drag tracking implementation used by DragGesture instead of maintaining another threshold, deadline, movement, or velocity state machine.

After acceptance, the same PointerSession remains the sole pointer owner and retains one private DragDropSession:

```text
PointerSession
  recognition owner
  DragDropSession
    frozen source declaration
    current target handle
    preview LayerId
    latest DragEvent
```

DragDropSession coordinates the accepted operation but does not own or transfer the physical pointer.
There is no Runtime-wide active-drag slot, target registry, or second route.
Independent PointerSessions may own independent transfers.

Runtime discovers source and target behavior through private NodeExtension capabilities.
It never branches on DragSourceExtension, DropTargetExtension, a component NodeKind, or the payload type.

## Target selection

The source hit route remains frozen for recognition and source-local DragEvent coordinates.
Drop targets are selected from the current committed visual tree at the latest window position because targets may move, scroll, mount, unmount, or change acceptance during a transfer.

Runtime evaluates the visually topmost HuxerUI Layer before the application tree, excludes the preview Layer, and walks each hit route from the deepest node toward its ancestors.
Retained targets on one node follow reverse declaration order.

A target is eligible only when its node participates in layout, accepts pointer events, remains enabled, has an invertible presentation transform, contains the pointer under existing clipping rules, matches the exact payload type, and accepts the current payload through its predicate.
Rejected targets do not receive events and selection continues toward the ancestor route.
Only one target is active; events do not capture, bubble, or reach several matching nodes.

DropEvent.position is resolved in the target's current local coordinate system.
DropEvent.window_position remains in host-window logical coordinates.

## Event order

Recognition commits ownership and cancels competing recognizers and the raw target before source output:

```text
record owner
cancel competition and raw delivery
create DragDropSession and preview
source Started
target Entered, when eligible
```

Movement updates retained state before invoking application handlers:

```text
update preview placement
source Changed
old target Exited, when selection changed
new target Entered, or current target Moved
```

A successful Up commits terminal state and closes the preview before delivery:

```text
target Dropped
source Ended with dropped = true
```

An Up without an eligible target emits source Ended with dropped set to false.
If an existing target became ineligible, Exited precedes Ended.

Cancel, source invalidation, device loss, or Runtime teardown commits cancellation and closes the preview before emitting current target Exited and source Canceled.
Dropped is the successful terminal counterpart of Entered, so a successful target does not also receive Exited.

## Reconciliation and invalidation

Source and target modifiers retain compatible NodeExtension identity by descriptor and declaration position.
An active source keeps its original declaration snapshot while resolving each event handler from the current mounted source node.
A target uses its latest compatible predicate and current event bindings; an already entered target retains the typed dispatch that admitted it through Exited.

After reconciliation, Runtime validates every active source and current target at the last committed pointer position.
An invalid source cancels the operation.
An invalid target receives Exited when it still exists and is otherwise cleared silently; the source remains active and may select another target.

Source and target declaration updates do not invalidate measurement, layout, paint, or semantics by themselves.
Pointer movement updates retained session and Layer placement directly.
Only application handlers that mutate State request composition through the ordinary state path.

## Preview Layer

The optional preview is explicit; Runtime does not snapshot or clone MountedNode content.
The preview factory receives the source Environment captured at recognition and composes in its own Layer scope.

The preview uses Notification level, PassThrough pointer policy, no focus trap, and hidden semantics.
The existing PassThrough Layer contract excludes its complete subtree from target hit testing without a drag-specific route.
DebugOverlay remains above it at System level.

The initial source-local grab point is transformed into a window-space offset before anchoring preview placement, so transformed sources do not jump the preview origin.
Later movement updates Layer placement without recomposing preview content.
Terminal state, source invalidation, pointer-session quarantine, and Runtime teardown dismiss the preview exactly once.

## Nested auto-scroll

Auto-scroll is active only while a compatible target exists within the relevant scroll route and the pointer enters that scroll viewport's edge band.
This avoids turning an unrelated DragSource over a scrolling Canvas into implicit scrolling.

Each frame derives velocity continuously from edge penetration and first consumes it through the deepest eligible ScrollView or VirtualLayout.
When that container reaches its boundary, selection continues through eligible ancestors on the same current hit route.
It calls the existing scroll-activity notification path, does not create a ScrollController, and does not start a fling.

Scrolling may change the target while the pointer remains stationary, so Runtime repeats target selection after a consumed auto-scroll frame.
Leaving the edge band or reaching every scroll boundary stops frame requests immediately.
Direct-manipulation auto-scroll is not disabled by reduced motion.

## PlatformView and external transfers

Initial PlatformView arbitration remains unchanged.
A sequence owned by PlatformView never creates a HuxerUI DragSource, and an accepted HuxerUI source never transfers that sequence to PlatformView.

PlatformView is not a DropTarget in this phase.
An application that needs a native destination exposes the transfer through a HuxerUI DropTarget or a future platform-transfer contract rather than treating the native view itself as a typed target.

Cross-application files, text, URLs, MIME values, UTTypes, ClipData, and host drag sessions require a separate platform transfer design.
They must not reinterpret an arbitrary in-memory DragSource payload or make serialization implicit.

## Accessibility

An arbitrary payload does not identify the application operation, destination label, or equivalent keyboard behavior.
DragSource and DropTarget therefore do not synthesize semantic actions.

Applications and higher-level reorderable components expose suitable custom semantic actions such as move before, move after, move to group, or add to collection.
Physical drag-and-drop must not be the only way to perform an essential operation.

## Exception safety

Runtime stores every ownership, target, preview, and terminal transition before invoking the corresponding application callback.
An escaping predicate or event exception quarantines the PointerSession, dismisses preview content, prevents duplicate normal terminal events, and then rethrows the original exception.

Quarantine attempts the ordinary single cancellation path, consistent with other accepted gestures; an exception from that cleanup does not replace the original exception.
This preserves strong internal commit while ensuring later physical pointer events cannot resume the failed operation.

## Implementation ownership

- `<huxerui/gesture.h>` owns public source, target, event, result, and payload-construction templates.
- `src/gesture.cpp` owns source and target retained modifiers, erased payload dispatch, and the drag recognition state shared with DragGesture.
- `src/gesture_internal.h` owns private source and target capability contracts and DragDropSession as part of PointerSession.
- `src/runtime_pointer_interaction.cpp` owns target selection, preview coordination, auto-scroll, terminal ordering, and quarantine.
- Existing Layer and scrolling implementations remain generic and receive no DragSource or DropTarget branches.

Focused tests cover exact type matching, predicates, nested target fallback, event order, source payload snapshots, target-local coordinates, preview exclusion and dismissal, compatible target updates, auto-scroll, and cancellation.
