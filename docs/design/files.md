# File and Application Storage Design

`File` operates on a local filesystem path. `FileReference` retains access to one selected external item. This document defines their ownership, execution, and platform boundaries. See [Files and Storage](../guide/files.md) for application usage and [URI and Local File URI](uri.md) for URI syntax and conversion.

## File construction and identity

A `File` captures a normalized absolute path at construction. Relative paths are resolved against the current process directory once. Equality compares the stored paths. Construction and lexical operations validate syntax without inspecting storage. `Child()` validates one name, while `Resolve()` joins and normalizes a relative path. Lexical normalization provides no directory containment or symlink guarantees.

Shared paths use UTF-8. Windows exposes slash-separated drive and UNC paths and converts them to wide native paths at the I/O boundary. POSIX paths retain literal backslashes where permitted. File URI conversion preserves the retained path and depends only on the process's existing filesystem authority.

`Stat()` follows symbolic links and reports the target type. Recursive deletion removes a named link without traversing it and protects filesystem roots and registered application-directory roots. Single-file copy handles ordinary files. Move uses the platform move operation, so cross-device moves can fail.

## Execution and results

Storage opens, reads, enumeration, and detailed metadata use `IoResult<T>`. Valid empty outputs remain distinguishable from errors. Local mutating convenience methods return `bool`. Invalid caller input, such as malformed UTF-8 or an invalid child name, throws synchronously even for a lazy task.

Whole-file reads return owned memory and reject contents too large for the returned value. Text reads validate UTF-8, strip one leading BOM, and preserve line endings. Text writes validate UTF-8 and preserve line endings. Writes take borrowed input for synchronous calls and own the input across asynchronous suspension.

Synchronous methods run on the calling thread. Native asynchronous local operations use the shared file worker and complete on the owning Runtime thread. Web serializes virtual filesystem work through its event loop. Platform reference backends use the same acquisition and I/O logic where both execution modes are available. Cancellation suppresses delivery and requests cooperative stop, while an operation already in a native or browser call can still finish and leave changes.

## Streams

Sequential stream handles are move-only. Synchronous reads borrow a span and return zero at EOF. Asynchronous reads return owned bytes and an empty value at EOF. An asynchronous backend owns its reservation, retains the transport across suspension, and allows one operation at a time. Copy reserves both endpoints and leaves them open. It returns the first error or the byte count on success. A failed copy can leave a written prefix.

Output finalizes through explicit `Close()` or `CloseAsync()`, and a repeated close after success is harmless. I/O and close failures leave a failed stream. Output destruction before successful close releases the transport using its abort path where available. Callback-backed streams use those same rules, including stop-token delivery before the pending callback frame is destroyed and release-time notification for owned callbacks. A callback reader may remain usable at its advanced position if a copy is canceled during a write.

Native asynchronous local descriptor I/O uses a per-stream worker sequence. File and packaged-resource inputs share the worker adapter over a synchronous stream. HTTP and browser transports keep their own asynchronous scheduling. An input and output opened through a reference retain its grant independently.

## Random-access streams

An open returns one owned descriptor and any authorization or coordination it needs for its lifetime. Shared validation checks access and open mode before acquisition, and native code checks type and seekability before truncating the acquired object. Local creation is exclusive. Reads and writes use absolute offsets. Offset and count checks precede transfer, and size queries describe the opened resource.

Read and size failures permit subsequent operations. A backend write failure or failed close ends the session. Close reports finalization without implying rollback or durable storage. Browser-selected references do not offer synchronous random access. Local Web files that need asynchronous persistence reject writable opens before creation or truncation.

## Application directories

Runtime prepares and validates the host's `AppDirectories` before publishing application services and running hooks. The application service retains the directory values, and retained application handles can query them after Runtime retirement. The process working directory remains independent and is queried on each call.

Platform shells derive the application identity from bundle, package, executable, or Web shell metadata. Apple hosts use application support, cache, and temporary directories. Android uses the application Context and creates a temporary child of the cache root. Windows uses the application identity under Local App Data.

Linux derives the identity and executable directory from `/proc/self/exe`. It uses absolute, valid UTF-8 `XDG_DATA_HOME` and `XDG_CACHE_HOME` or the conventional home-directory fallbacks. It uses `XDG_RUNTIME_DIR` only when the effective user owns the private runtime root. Otherwise, it validates owner-only directories under the system temporary directory using the effective UID and executable name.

## External file references

Each `FileReference` retains a shared backend for one item. Names, type, optional size and MIME type, and write capability are metadata recorded at acquisition. Operations recheck actual storage and access. Names missing from provider metadata stay unknown, rather than being derived from opaque identifiers. Directory size and MIME type are absent. Unsupported item kinds report `Unsupported`.

A projected `File` from `AsFile()` holds the stored path alone. It does not extend the grant, coordinate access, or bind operations to the original item's identity. The backend owns temporary copies and native authorization, which live as long as references, streams, and pending operations retain that backend.

A selected directory supplies an access ceiling inherited by its children. `CanWrite()` on a directory describes child creation, while existing child files also have their own provider write flags. A child can outlive the parent value without gaining authority. Native synchronous and asynchronous entry points reuse backend operations. Browser-selected items return `Unsupported` for synchronous I/O before mutation.

`ImportToAsync()` and `ReplaceWithAsync()` use the backend's bounded single-file transfer path. Whole-file reads may use one provider read or coordination scope. Sequential write streams replace the selected contents and require explicit finalization.

### Persistent access records

The live backend manages reference and stream access. `RememberAsync()` explicitly writes an application-private record, and `RestoreAsync()` acquires a new live backend. The Runtime owns the optional store; each lazy operation captures its original application identity before scheduling. Unsupported hosts and capabilities return `Unsupported`. Records are keyed by exact application-supplied UTF-8 strings and replace atomically. Restore is silent. It refreshes metadata, retains the recorded access ceiling, and relies on the current platform grant and provider. A lost read grant fails, while a lost write grant yields a read-only reference. Cancellation suppresses delivery without undoing committed changes.

Android stores the authority URI and effective read/write modes. A tree record uses the original tree grant URI; its live backend uses the provider's root document URI. Restoration intersects the recorded ceiling with `getPersistedUriPermissions()` before consulting the provider. Ownership records hold only the bits the SDK took and the resulting grant timestamp. Cleanup checks both against the current grant and preserves modes required by other keys. A missing or changed grant, an old record without a timestamp, or an interrupted save without confirmed ownership cannot authorize release. Native code sharing the same grant should coordinate changes with the SDK because the system has one grant per application and URI.

iOS uses minimal bookmarks and macOS uses application-scoped security bookmarks, including the read-only bookmark option where applicable. Both reconstruct existing coordinated references and balance scope ownership through those references. A stale bookmark is updated when it resolves and access can be obtained. Temporary copies and children without independent authorization cannot be stored.

Web persists serializable file system handles in IndexedDB under the existing application storage identity. Its transactions are independent of the local filesystem queue. Restore finishes reading the record before querying the browser's current permission or contacting the provider. Upload snapshots have no restorable handle. Windows and Linux leave persistent access unavailable; their ordinary path-backed I/O remains available.

### Platform capability transport

The [PlatformPayload boundary](platform-content.md#platformpayload-boundary) defines wire format and owning wrappers. Native projections reuse the retained capability. The Android URI factory checks existing read access and creates a read-only file capability using the application Context without a window. The iOS URL copy factory creates an independent read-only temporary file, balances any security scope it acquired, and removes partial copies on failure. The last owner releases the completed snapshot.

### Directory capabilities and copying

Listing resolves immediate children with no ordering or file reads. Creating a directory validates a single UTF-8 name before I/O and reuses an existing directory of that name. A different item kind yields a collision. Provider creation verifies the chosen name and cleans up only entries known to have been created by that operation.

Shared traversal owns name validation, conflict handling, progress counters, and error mapping. Backends provide authorized enumeration, creation, identity checks, and finalized single-file transfers. Same or overlapping roots fail before writes. Potentially related roots require a reliable containment check. Traversal rejects detectable traversal-changing links and repeated ancestor identities, and does not infer independence from display names or URI prefixes.

The traversal retains an active directory stack and uses bounded transfer buffers. Each destination operation reuses the preceding child lookup. Backends that scan provider listings by display name can retain a name index per active frame, preserving duplicate-name ambiguity as the destination grows. Native path-backed backends retain a destination anchor and use independent enumeration cursors. No-overwrite uses exclusive creation where available. Provider and browser namespaces can still change from outside the operation.

A successful summary counts finalized files, newly created directories, and actual transferred bytes. The first failure includes the operation stage, side, and escaped source-relative path without exposing native paths or permission-bearing URIs. Successful prefix output remains, and an interrupted file can be partial. Web persistent output finishes its synchronization before reporting success.

## File picker

`FilePicker` is a window root service with one active operation. Queued requests stay with their original window. A canceled task detaches its continuation and asks the host to dismiss native presentation when possible. If the host cannot dismiss it, the transport retains the slot until the platform result or host teardown. Window retirement cancels outstanding operations.

The adapter owns native presentation, advisory filter conversion, selection authority, and save completion. Directory requests specify whether write access is required. The adapter must confer the requested capability or return no selection. This check does not imply write authority over all existing children.

Open With, share, and document URL deliveries carry `FileReference` through `FileActivation`. [Application Activation and Lifecycle](application.md) owns window routing and delivery, and [External File Drop](file-drop.md) owns the host drag lifecycle.

## Platform mapping

### Apple hosts

AppKit uses open and save panels. UIKit uses document pickers and opens selected directories in place. Apple backends retain security-scoped URL access as needed through the path-backed reference backend. Workers use coordinated reads and writes, and a copy can coordinate both locations together. An iOS provider delivery that requires copying before the native callback returns becomes a read-only, reference-owned snapshot. Saving on iOS exports a local copy, with temporary staging when the suggested name differs.

### Android

Documents and trees retain provider URI grants. `DocumentsContract` resolves descendants under the tree authority. For two selected trees, copy requires the same authority, API 29 or later, and working `isChildDocument()` checks in both directions. A copy into application-private local storage can establish independence when the provider has a separate UID. Incomplete provider listings and virtual documents without ordinary byte streams fail explicitly.

The bounded Java executor handles provider acquisition and streaming transfers. Sequential C++ streams own an ordinary detached fd or borrow one from a retained reliable `ParcelFileDescriptor`. Reliable input checks for provider errors at EOF. Explicit output close finalizes the provider operation, and failure or abandonment signals error after worker use ends.

Random access and the [JNI descriptor bridge](../../include/huxerui/android/jni.h) share validation of ordinary, whole-file, seekable descriptors with compatible access flags. The provider opens before any truncation, and a descriptor requiring reliable Java close signaling is rejected. The bridge borrows the fd from the retained Java descriptor and keeps the reference alive. Closing the C++ owner closes through Java.

Native pickers and window-owned Java modules share the attachment's Activity-result queue. Picker data preparation releases the native presentation slot once the result arrives. An embedded host must supply the [Activity result integration](../guide/platforms.md#window-owned-java-modules-and-activity-results). A retained picker cannot redirect requests to a replacement attachment.

### Windows and Linux

The Windows picker uses the window-owned COM dialogs. References retain handles with shared read, write, and delete access. Descendant resolution opens one child relative to its retained parent and rejects traversal-changing reparse points. Reopening by handle preserves object identity and gives enumeration an independent cursor. A rename can fail while a handle is retained. A replacement at the former path does not receive the original grant.

Linux uses the FileChooser portal on a dedicated GLib context. It supplies the realized X11 parent identifier where available. The transport subscribes to the predicted Request path before invoking the portal and switches subscriptions when the portal returns a different path. Cancellation closes the Request without waiting for a Response. Directory mode requires FileChooser version 3 or later. Successful local file URI results use the shared path-backed backend.

### Web

The picker uses the browser File System Access APIs when available. Ordinary open can fall back to a read-only upload snapshot. A directory selection requires a live directory handle, and saving requires a writable handle that can close and report completion. Picker presentation starts within transient user activation. The shared queue waits for an undismissable browser dialog to settle before admitting another presentation.

Each browser input stream retains one resolved `File` snapshot and reads bounded Blob slices. A writable stream retains the browser writer until close commits it or cancellation can abort it. Imports, replacements, saves, and copies share a bounded transfer path. Serializable handles can be remembered independently of local path storage.

## Web application storage

The shell supplies a stable `huxeruiStorageKey`. The Emscripten module mounts and restores one IDBFS tree under `/huxerui/<storage-key>/` before Runtime mounting. Data and cache share the tree and its synchronization domain. Temporary files use MEMFS under `/tmp/huxerui/<storage-key>/`. An initialization failure prevents mounting, and all Runtimes in the module reuse the initialized directories.

Local reads use the restored virtual filesystem. Synchronous mutations of persistent paths are rejected before changing them. This includes writable random-access opens, which report `Unsupported`. Local async file operations run on the browser main thread through a module-owned serial queue. Persistent mutations retain their queue slot through explicit `FS.syncfs(false)` and report success only after synchronization. The same completion applies after a partially failed mutation has already changed storage. Cancellation detaches the application continuation while the queue preserves operation order. Bounded buffers do not limit the size of the memory-resident filesystem.

## Validation

[File tests](../../tests/unit/file.cpp) and the [shared filesystem suite](../../tests/platform/common/file.cpp) cover local paths and I/O. [File-reference tests](../../tests/platform/common/file_reference.cpp) cover provider lifetimes and streams. The [Runtime picker tests](../../tests/runtime/file_picker.cpp) cover presentation ordering and retirement. The [Web storage](../../tests/platform/web/file_storage_test.mjs) and [Web picker](../../tests/platform/web/file_picker.js) suites cover browser-specific behavior.
