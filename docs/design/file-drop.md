# External File Drop

External file drop receives ordinary files from the host drag system into one mounted HuxerUI target.
It reuses the file capabilities defined in [Files and Application Storage](files.md), while remaining independent of the PointerSession-owned value transfers in [Typed Drag-and-Drop](drag-drop.md).

## Public contract

`<huxerui/file_drop.h>` provides `FileDropTarget`, `FileDropOptions`, `FileDropOffer`, `FileDropEvent`, and `FileDropEvents`.
Applications attach `FileDropTarget::Accepts(options)` through `.With(...)` and receive ordinary typed events through `.On<...>()`.
An optional `Accepts(options, predicate)` predicate inspects advertised metadata only; it must be quick and free of side effects.
There is no root service, controller, source modifier, public transfer object, or target registry.

`FileDropOptions` contains `extensions`, `content_types`, and `allows_multiple`, which defaults to true.
Extensions and MIME types form a union; empty lists accept any ordinary file.
The syntax follows FilePickerFilter, but drop options do not contain a picker display label and final drop filtering is mandatory rather than an advisory native UI hint.
Extensions match complete dot-prefixed suffixes with ASCII case folding, including compound suffixes such as `tar.gz`.
MIME matching is ASCII case-insensitive and supports `type/*` and `*/*` without parameters.
Filters do not inspect bytes or establish content safety.

`FileDropOffer` reports an optional file count and the known advertised MIME types.
Missing metadata means unknown, not an empty transfer or a proven match.
Hover selection rejects only offers known to be ineligible; final delivery checks every file against the frozen options.
An empty batch, a directory, an unsupported item, a disallowed count, or a nonmatching file rejects the complete batch without partial delivery.
Unknown final metadata does not satisfy a nonempty filter unless another available fact matches it.

`FileDropEvent` contains target-local `position` and host-local `window_position`, both in DIPs.
It does not fabricate a pointer identifier or device kind for host-owned input.
Received references use existing FileReference operations; retaining a callback argument requires copying its value.
Applications decide whether to read, retain, import, project to a local path, or discard each reference.
Receiving a drop neither imports its contents into application storage nor grants write access.

## Hover and delivery

Entered, Moved, and Exited describe hover only.
Every entered target receives Exited when hover ends, including physical drop, unless its mounted identity has already disappeared.
This intentionally differs from the successful terminal Dropped event of in-process typed drag-and-drop.
Applications drive hover feedback exclusively from Entered and Exited.

Physical drop freezes the selected extension identity, target-local and window coordinates, and acceptance options before ending hover and auto-scroll.
The adapter captures native access within the host callback's permitted lifetime, then prepares the file references asynchronously when required.
Successful preparation and whole-batch validation emit Dropped with `std::vector<FileReference>` on the UI thread.
Preparation or final validation failure emits Failed with FileError, without also emitting Dropped.
Neither event reports the success of subsequent application I/O.
Completion is deferred even when native preparation finishes synchronously, keeping callback order independent of the host.

One host surface has at most one hover session but may retain several independently accepted, pending deliveries.
A new hover or a newer completed drop never redirects or cancels an older accepted delivery.
Compatible target recomposition preserves pending delivery and uses current event bindings; acceptance options remain those captured at physical drop.
Unmounting or incompatibly replacing a target cancels its pending deliveries without invoking stale handlers.
Changing hover eligibility after acceptance does not retroactively reject the accepted operation.
Runtime destruction cancels all pending work, and late completions cannot access Runtime or resurrect a removed target.

## Shared ownership

FileDropTarget is a retained modifier exposing a private NodeExtension capability.
Runtime selects one target using the existing committed pointer hit route, deepest node first and reverse modifier order within a node.
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
This is neither a promise of zero-copy delivery nor a promise that every source remains accessible indefinitely.
Operations still report revoked access, unavailable providers, and externally changed files through existing file results.

Windows receives shell file lists through one OLE IDropTarget per host HWND, with balanced OLE initialization and registration.
AppKit receives file URLs; UIKit receives asynchronous file representations and secures temporary-file ownership before provider callbacks return.
Android receives content URIs with shared batch permission ownership on supported hosts; drag permissions remain bounded by the granting Activity's lifetime.
GTK receives files through its native asynchronous drop controller and finishes each accepted native operation once data reception terminates.
Web captures browser file capabilities during the drop event's readable data-store lifetime and does not suppress unrelated native drops.
Unsupported items and unavailable grants fail explicitly rather than producing partial or path-shaped substitute references.

## Scope and validation

The initial contract accepts single or multiple ordinary files for reading and import.
Directories, recursive drops, outgoing native transfers, Move, and general text, URL, or MIME transfer are not included.
Applications provide an equivalent picker action for keyboard, touch, accessibility, and hosts without external drag support.

Focused tests cover configuration validation, partial offer metadata, final filtering, nested targets, transformed coordinates, clipping, edge scrolling, disabled nodes, compatible updates, unmount, exception cleanup, deferred delivery, concurrent pending drops, preparation cancellation, and reference lifetime.
Existing typed drag-and-drop and scrolling tests remain regression coverage rather than being rewritten to match external hover semantics.

## Implementation boundaries

The public file-drop header owns the receiving modifier and typed events.
`runtime_file_drop.cpp` contains the modifier implementation, private capability, metadata filtering, hover routing, and pending delivery, without a separate internal header, service, or transfer controller.
Tests verify filtering through delivered or failed events rather than exposing private validation functions.
Platform declarations remain in existing file-integration headers; native implementations own only host callbacks, grants, and preparation.
macOS and Web capture file capabilities alongside their existing file-reference implementations, while host event callbacks remain in the adapters.
macOS drop preparation and delivered references share the existing security-scoped access owner without acquiring the same grant twice.
The preparation callback returns best-effort cancellation directly, and Android drop completion does not pass through picker presentation control.
File metadata validation is shared with the existing file module rather than introducing a separate filter module.
