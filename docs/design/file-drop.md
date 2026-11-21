# External File Drop

External file drop receives ordinary files from the host drag system into one mounted HuxerUI target.
It reuses the file capabilities defined in [Files and Application Storage](files.md), while remaining independent of the PointerSession-owned value transfers in [Typed Drag-and-Drop](drag-drop.md).

## Acceptance rules

The [file-drop guide](../guide/files.md#receiving-dropped-files) owns modifier and event usage. Hover selection rejects only offers known to be ineligible using the advertised metadata. At physical drop, the receiver freezes the options and checks every prepared reference against them. An empty batch, non-file item, disallowed count, or final filter mismatch rejects the entire batch. Unknown final metadata cannot satisfy a configured filter without another matching fact.

Host adapters report position in window DIPs. The receiver converts to target-local DIPs without fabricating a pointer identity.

## Hover and delivery

Entered, Moved, and Exited describe hover only.
Every entered target receives Exited when hover ends, including physical drop, unless its mounted identity has already disappeared.

Physical drop freezes the selected extension identity, target-local and window coordinates, and acceptance options before ending hover and auto-scroll.
The adapter captures native access within the host callback's permitted lifetime, then prepares the file references asynchronously when required.
Successful preparation and whole-batch validation emit Dropped with `std::vector<FileReference>` on the UI thread.
Preparation or final validation failure emits Failed with IoError, without also emitting Dropped.
Neither event reports the success of subsequent application I/O.
Completion is deferred even when native preparation finishes synchronously, keeping callback order independent of the host.

One host surface has at most one hover session but may retain several independently accepted, pending deliveries.
A new hover or a newer completed drop never redirects or cancels an older accepted delivery.
Compatible target recomposition preserves pending delivery and uses current event bindings; acceptance options remain those captured at physical drop.
Unmounting or incompatibly replacing a target cancels its pending deliveries without invoking stale handlers.
Changing hover eligibility after acceptance does not retroactively reject the accepted operation.
UiWindow retirement cancels its pending drop work, and late completions cannot access that UiWindow or resurrect a removed target.

## Shared ownership

FileDropTarget is a retained modifier exposing a private NodeExtension capability.
UiWindow forwards normalized host input to its FileDropReceiver, which owns hover and pending delivery and selects one target using the existing committed pointer hit route, deepest node first and reverse modifier order within a node.
Layer ordering, clipping, pointer participation, enabled state, and presentation transforms keep their existing meaning.
Native PlatformView regions retain native ownership rather than falling through to an ancestor file target.
Coordinate conversion uses Node APIs.

External hover is not a PointerSession: it has no HuxerUI source, gesture recognition, capture, or preview.
Only common edge-scroll and route operations are shared with typed drag-and-drop.
Edge scrolling consumes the deepest eligible scroll container before ancestors, uses the existing DragDrop scroll source, and starts no momentum.
Stationary hover is re-evaluated after layout and scrolling; terminal transitions stop frame requests.
Every transition is committed before invoking application callbacks, and exceptions cannot leave a live failed session or escape an unchecked native callback boundary.

## Native access

Adapters negotiate Copy only when both the source and the target support it; Move and Link are never substituted.
They reuse picker and activation reference factories rather than exposing native paths, URLs, objects, or handles as the portable payload.
References retain the native grant or provider independently of the drag callback wherever the platform permits it.
Temporary provider files are copied into reference-owned temporary storage only when their native lifetime requires it.
Operations still report revoked access, unavailable providers, and externally changed files through existing file results.

Windows receives shell file lists through one OLE IDropTarget per host HWND, with balanced OLE initialization and registration.
AppKit receives file URLs; UIKit receives asynchronous file representations and secures temporary-file ownership before provider callbacks return.
Android API 24 and later can retain content URIs through a shared batch permission lease tied to the granting Activity. `HuxerUIActivity` supplies the hook. Embedded hosts use `HuxerUIView.setFileDropPermissionRequester` to return an `AutoCloseable` lease and clear the hook during teardown.
GTK receives files through its native asynchronous drop controller and finishes each accepted native operation once data reception terminates.
HarmonyOS captures file URI records during the ArkUI drop callback and keeps the drop pending while the application host opens the granted files. The shared receiver delivers the prepared read-only references asynchronously.
Web captures browser file capabilities during the drop event's readable data-store lifetime and does not suppress unrelated native drops.
Unsupported items and unavailable grants fail explicitly rather than producing partial or path-shaped substitute references.

## Validation

[Runtime file-drop tests](../../tests/runtime/file_drop.cpp) cover routing, filtering, deferred delivery, cancellation, and reference lifetime. The [Files example](../../examples/files/main.cpp) provides equivalent picker and drop actions.
