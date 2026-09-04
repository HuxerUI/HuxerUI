# Files and Storage

HuxerUI provides platform-neutral URI values, application storage, lexical path operations, asynchronous file I/O, external file references, and file pickers.
Application code does not depend on operating-system path, URL, bookmark, or browser handle types.

## Application directories

Use the FileSystem root service to obtain application-owned directories:

- data for durable application state;
- cache for reproducible data;
- temporary for disposable process or session data.

The platform adapter derives the physical location from the application identity and platform conventions.
Do not construct these locations from environment variables in application code.

## Paths and files

`File` represents an application-accessible local path.
Path joining and normalization are lexical; operations that touch storage report platform errors through typed results or exceptions documented by the API.

Keep user-visible paths in UTF-8.
Windows conversion to and from UTF-16 occurs inside the platform file implementation.

## URI values and local file conversion

`Uri` from `<huxerui/data.h>` is an immutable absolute RFC 3986 URI value.
Construct it directly for trusted caller input or use `Uri::Parse()` for external text that may be invalid.
Its `Scheme()` and `Path()` are always present, while `Authority()`, `Query()`, and `Fragment()` use `std::optional<std::string_view>` so absent and present-empty components remain distinct.

```cpp
std::optional<Uri> activation_uri = Uri::Parse("huxerui://documents/42?preview");
if (activation_uri.has_value() && activation_uri->Scheme() == "huxerui") {
  ShowDocument(activation_uri->Path());
}
```

`Uri` retains the validated ASCII serialization without normalization.
Raw Unicode is not accepted as an IRI; encode textual data as UTF-8 bytes followed by URI percent encoding.

Construct `File` from a supported local `file:` URI and use `File::ToUri()` for the inverse conversion.
The conversion is lexical and does not require the target to exist.
POSIX-like hosts accept only local authorities, while Windows additionally maps supported non-empty authorities to UNC paths; `localhost` with a non-drive path identifies a local UNC share.
Query, fragment, user-info, port, encoded separators, and platform-incompatible file forms are rejected.

Do not convert `FileReference` into `File` or `Uri` merely because a platform capability originated from an Android content URI, Apple security-scoped URL, or browser handle.
The capability remains authoritative for access.

## Asynchronous operations

File reads, writes, directory operations, copying, moving, and external imports use `Task` where blocking platform work must leave the Runtime thread.
Continuation resumes on the owning Runtime thread, and cancellation prevents late delivery to an unmounted owner.

```cpp
Task<FileResult<std::string>> LoadSettings(const std::shared_ptr<FileSystem>& files) {
  File file(files->Directories().data_directory, "settings.json");
  co_return co_await file.ReadStringAsync();
}
```

Use `Bytes` from `<huxerui/data.h>` for owned binary data and `std::span<const std::byte>` for borrowed binary input.
Use byte operations for arbitrary payloads and string operations only for UTF-8 content.

```cpp
Task<FileResult<Bytes>> LoadPayload(const std::shared_ptr<FileSystem>& files) {
  File file(files->Directories().data_directory, "payload.bin");
  co_return co_await file.ReadBytesAsync();
}
```

Synchronous `WriteBytes()` and `AppendBytes()` borrow a span only for the call.
Their asynchronous counterparts take `Bytes` by value so the operation owns the storage while suspended.

## External files and directories

`FileReference` is a capability for one file or directory selected or opened outside application-owned storage.
It may represent a path, security-scoped Apple URL, Android document URI, or browser file handle without exposing that identity to shared code.

External references can be read, imported into application storage, or used for a supported replacement operation.
Their lifetime and persistence follow platform capability rules.

`Type()` reports the item kind recorded when the reference was obtained; operations still check current storage state.
Directory references have no `Size()` or `ContentType()` value.
Their `ListChildrenAsync()` returns direct children, including hidden entries, without sorting or reading file contents.
Children retain the selection's grant independently of the parent value, and cannot acquire more permissions than that parent.
The grant restriction is distinct from an item's `CanWrite()` snapshot: a directory that cannot create children may still contain independently writable files, but a read-only selection keeps all derived references read-only.
External renaming or replacement can invalidate later operations; a retained directory grant does not authorize an unrelated directory subsequently occupying its old path.

Directory `ReadBytesAsync()` and `ReadStringAsync()` fail with `IsDirectory`.
Existing `ImportToAsync()` and `ReplaceWithAsync()` remain single-file operations and return `false` for directories.

## FilePicker

Use the file picker service to open one or more files, select one directory, or save an application-owned file through the current platform.
Picker cancellation, denied access, and unsuccessful or unsupported presentation use the existing compact empty or false result; capability queries let applications hide unavailable actions.

Filters are portable hints.
The platform picker may present them according to platform conventions, but application code must still validate selected content.

`OpenDirectoryAsync(bool writable = false)` selects one existing directory without file filters.
The default reference and its children are read-only through HuxerUI even when the operating system grants broader rights.
Passing `true` requests read/write access and never silently returns a read-only selection.
`CanOpenDirectories(writable)` checks host support without opening UI or creating probe files; it does not guarantee approval for a particular location.
`CanWrite()` on the returned directory advertises child creation, not permission to overwrite every existing child.
Directory, file-open, and save requests share the same per-Runtime presentation queue.

## Copying directory contents

Use `CreateDirectoryAsync(name)` on a writable directory reference to create one child or obtain an existing child directory.
Use `CopyFileFromAsync(source, name, overwrite)` to stream one `File` or file `FileReference` into a named child.
Names must be valid UTF-8 single segments, excluding empty names, `.`, `..`, NUL, `/`, and `\\`; invalid caller names throw before returning the lazy Task.
Destination-specific restrictions are reported as file errors, not silently corrected names.

`CopyDirectoryContentsToAsync(destination, overwrite = false)` recursively copies a selected source directory into an existing `File` directory or writable directory `FileReference`.
The destination is the copy root: copying `Assets/logo.png` into `Imported` produces `Imported/logo.png`, not `Imported/Assets/logo.png`.
Empty directories and hidden files are included, existing directories merge, and destination-only entries remain untouched.
Existing files fail with `AlreadyExists` unless overwrite is explicitly enabled; file/directory conflicts and ambiguous source names always fail.
The operation never automatically renames or silently skips entries.

The successful `DirectoryCopySummary` counts finalized files, newly created directories below the root, and actual copied bytes.
The first failure returns a `FileError` with the operation stage and escaped relative path, not a partial success summary.
Completed output remains after failure or cancellation; neither a tree transaction nor rollback of an overwritten file is promised.
Cancellation stops subsequent work and attempts to close active I/O without resuming retired application code.

Copying rejects identifiable links, cycles, and overlapping roots; potentially related roots whose relationship cannot be established safely are unsupported.
The operation is not a snapshot and does not isolate external concurrent creation, renaming, or replacement.
No-overwrite uses exclusive creation where available; browser/provider APIs without that primitive check collisions but cannot guarantee atomic exclusion of external writers.
Applications must avoid concurrently modifying the source and destination during copying when those stronger guarantees are required.

The runnable [directory-copy example](../../examples/directory_copy/main.cpp) demonstrates separate source and destination selection, copying to application storage, overwrite behavior, cancellation, and summaries through the same public API on every platform.

On Android, copying from a separate provider into application-private storage is supported without broad storage access.
Copying between selected trees requires the same provider and reliable containment checks (Android API 29 or later); unknown relationships return `Unsupported` instead of risking recursive self-copy.

## Web storage

The Web backend uses browser-managed storage for application directories and browser file capabilities for external selections.
The application identity determines its storage namespace.

Browser persistence is asynchronous and subject to browser policy, quota, and user clearing.
The file API does not pretend browser storage is an unrestricted host filesystem.

Directory selection requires `showDirectoryPicker()` and the requested access capability; an uploaded directory file list is not a substitute for a live directory grant.
Start source and destination selection from separate user clicks, without earlier asynchronous work consuming transient activation.
Copies use bounded transfer buffers rather than whole-file `ReadBytesAsync()` calls.
This does not bound total memory when the destination is a local `File`: MEMFS/IDBFS retains file contents in memory, and persistent destinations complete only after synchronization succeeds.

## Safety

Validate user-selected content before parsing it.
Keep recursive deletion within an application-owned directory or an explicitly granted capability.
Treat external references as authority to the selected item, not to an arbitrary neighboring path.

See [File and Application Storage Design](../design/files.md) and [URI and Local File URI Design](../design/uri.md) for exact conversion, result, cancellation, and platform contracts.
