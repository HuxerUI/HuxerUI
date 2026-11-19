# Files and Storage

Use `File` for paths the application can already access and `FileReference` for items supplied by a picker, file drop, or application activation. Static assets shipped with the app use [Resources and Images](resources.md).

## Application directories

Use `UseApplication().Directories()` to obtain application-owned directories.

- `data_directory` holds durable application state.
- `cache_directory` holds reproducible data.
- `temporary_directory` holds disposable process or session data.

The platform chooses their locations from the application identity and host conventions. When available, `executable_directory` gives the executable location. This can be read-only.

```cpp
auto application = UseApplication();
File settings = application.Directories().data_directory.Child("settings.json");
File working_directory = application.CurrentDirectory();
```

`Directories()` returns the paths captured at initialization. Copy the required `File` values into asynchronous work. A custom host without application directories reports `std::logic_error`. `CurrentDirectory()` queries the process working directory on each call.

## Paths and files

`File` stores an absolute UTF-8 path. A relative input is resolved against the current working directory when the value is constructed. Use `Child()` for one name and `Resolve()` for a multi-segment relative path. These operations manipulate paths lexically. Use `Stat()` or `StatAsync()` when you need the current item type, size, or an error.

Local writes create a missing file or truncate its existing contents before writing. Use `AppendString()` or `AppendBytes()` to add data at the end. Create missing parents explicitly with `CreateDirectories()`. `ListChildren()` returns immediate, unsorted entries, including hidden files. `CopyTo()` copies one ordinary file, and `MoveTo()` can fail across filesystems. Set `overwrite` explicitly to allow replacement. Use recursive deletion only on an intended application-owned subtree.

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

Use `FileReference::AsFile()` to obtain a local path when the reference has one, then `File::ToUri()` if a file URI is needed.
Do not reconstruct a path from a display name, Android content URI, or browser handle; the returned `File` does not take ownership of the reference's access capability.

## Reading, writing, and errors

Use `ReadBytes()` for arbitrary data and `ReadString()` for UTF-8. Text reads remove one leading BOM and preserve line endings. Both read the whole file. Asynchronous writes take ownership of the bytes or string across suspension.

`IoResult<T>` holds either a value or an `IoError`. Check `Succeeded()` before calling `Value()` or `Error()`. Use `std::move(result).Value()` to take a move-only value, such as a stream. An empty successful value can represent an empty file or directory. Convenience mutations such as `WriteString()` and `Delete()` return `bool`.

The asynchronous forms run in a `Task` and resume on the owning Runtime thread. Prefer them for potentially slow storage operations. On Web, local work runs on the browser event loop. Canceling a task suppresses delivery, and a mutation already under way can still finish.

```cpp
Task<IoResult<std::string>> LoadSettings(File data_directory) {
  co_return co_await data_directory.Child("settings.json").ReadStringAsync();
}
```

## Incremental streams

Use `InputStream` and `OutputStream` to process local files without buffering their complete contents. Supply the buffer to `Read()` or `CopyTo()` and call `Close()` to finalize output. This copy helper takes separate source and destination files.

```cpp
bool CopyFileContents(const File& source, const File& destination) {
  IoResult<InputStream> opened_input = source.OpenRead();
  if (!opened_input.Succeeded()) {
    ReportIoError(opened_input.Error());
    return false;
  }
  IoResult<OutputStream> opened_output = destination.OpenWrite();
  if (!opened_output.Succeeded()) {
    ReportIoError(opened_output.Error());
    return false;
  }

  InputStream input = std::move(opened_input).Value();
  OutputStream output = std::move(opened_output).Value();
  Bytes buffer(64 * 1024);
  auto copied = input.CopyTo(output, buffer);
  if (!copied.Succeeded()) {
    ReportIoError(copied.Error());
    return false;
  }
  auto closed = output.Close();
  if (!closed.Succeeded()) {
    ReportIoError(closed.Error());
    return false;
  }
  return true;
}
```

Local output opens truncate existing contents by default. Pass `FileWriteMode::Append` to `OpenWrite()` or `OpenWriteAsync()` to add data at the end.

`AsyncInputStream` and `AsyncOutputStream` provide the same incremental contract for asynchronous sources and destinations.
`ReadAsync(maximum_bytes)` returns at most the caller-selected maximum as owned `Bytes`, and an empty value means EOF.
`CopyToAsync(destination, maximum_read_size)` is an input-stream member and does not close either stream.
Only one operation may be outstanding on each asynchronous stream.
Call `CloseAsync()` after the final write; destroying an unclosed output requests cancellation or aborts it without promising durable output.

Reads, writes, copies, and closes return `IoResult`. Check the result before treating an empty read as EOF. A failed copy may leave a written prefix, and a failed close leaves output unfinalized.

Use `InputStream::FromReader()` or `AsyncInputStream::FromReader()` to read from an application-defined source. The synchronous reader fills a borrowed buffer and returns the byte count. The asynchronous reader takes a stop token and a maximum byte count, then returns a `Task<IoResult<Bytes>>`.

Use `OutputStream::FromWriter()` or `AsyncOutputStream::FromWriter()` to supply a destination. The writer accepts the entire chunk on success. Pass a close callback if the destination needs to finalize output, and an abort callback if it needs cleanup when released without a successful close. Async writes own their chunk, and both async write and close callbacks receive a stop token. All four factories return ordinary streams that work with the existing copy operations.

## Random-access streams

`File::OpenRandomAccess()` opens a local file, and `FileReference::OpenRandomAccess()` acquires a compatible provider descriptor. Both open on the calling thread and return a move-only `RandomAccessStream` with synchronous byte operations. On native platforms, use `RunWorker()` to move a blocking open and its reads or writes off the UI thread.

Both entry points default to `FileAccessMode::ReadOnly` and `FileOpenMode::OpenExisting`. Choose `WriteOnly` or `ReadWrite` to write. `OpenExisting` preserves contents, `CreateNew` creates an empty local file and fails if the name exists, and `TruncateExisting` empties an existing file. Creation and truncation require write access. A `FileReference` accepts the two existing-file modes.

`ReadAt(offset, buffer)` reads into a caller-owned nonempty span and returns the byte count. A successful zero means EOF at that offset, and a subsequent read can start anywhere. `WriteAt(offset, data)` writes the complete span on success. A nonempty write beyond EOF extends the file, with zero bytes in the gap. Offsets and file sizes use 64-bit values. An offset or range outside the backend's representable offset limits reports `TooLarge` before transferring bytes.

`Size()` queries the same opened resource and reports its current length. Zero is a known empty file, a successful `std::nullopt` means unknown length, and an error reports a failed query. `FileReference::Size()` is captured metadata.

Keep operations on one stream serial, including close. The opened stream retains any required reference grant. A read or size error permits another operation. A backend write failure may leave a written prefix and makes the stream unusable. Call `Close()` to check finalization after successful operations. Destruction releases the resource, and closing provides no rollback or power-loss durability guarantee.

Provider support is checked at open. Android document references require a seekable descriptor with a compatible access mode and close contract. Browser selections return `Unsupported`. Local Web paths with asynchronous persistence reject writable random-access opening.

Android C++ callers that need a native fd can include `<huxerui/android/jni.h>` and call `android::OpenSeekableFileDescriptor(reference, access, mode)`. It accepts `ReadOnly` and `ReadWrite` for existing whole-file content URI references. The move-only owner retains the provider access and the Java descriptor. Pass the borrowed `Get()` value to APIs that leave ownership with the caller, then call the owner's `Close()` after the native consumer finishes. Nonseekable descriptors and providers requiring reliable Java close signaling report `Unsupported`.

## External files and directories

Retain a `FileReference` to use the selected item's access. `Name()` can be empty when unknown, while `Size()` and `ContentType()` are optional. `Type()` describes the item at selection time. Operations can still fail if access is revoked or the provider has changed the item.

Use `ReadBytesAsync()` or `ReadStringAsync()` for whole-file reads and `OpenReadAsync()` or `OpenWriteAsync()` for incremental I/O. The write stream replaces the selected file and requires explicit close. Use `ImportToAsync()` to copy into an existing local parent and `ReplaceWithAsync()` to write an application-owned file back through a writable reference.

`ListChildrenAsync()` returns immediate children without sorting. A child retains the original grant after the parent value is released. The asynchronous methods have the broadest Web compatibility. Native providers also support the synchronous forms for compatible files and directories. Browser selections return `Unsupported` from those forms.

### Remembering access across restarts

Call `RememberAsync(key)` on a selected file or directory to keep its access for a future run. The key is a nonempty, case-sensitive UTF-8 string owned by the application, for example `"workspace"`. Every window shares the application's records. A successful save under the same key replaces the previous record. Picking, previewing, and importing an item do not automatically remember it.

```cpp
Task<IoResult<FileReference>> RestoreWorkspace() {
  co_return co_await FileReference::RestoreAsync("workspace");
}
```

Run these tasks on the application thread, including from an application task scope before creating a window. Restore never prompts. It returns a regular `FileReference` with fresh metadata and at most the original granted access. A missing key or target reports `NotFound`, a revoked read grant reports `PermissionDenied`, and loss of write access yields a read-only reference. Let the user reselect a directory from a user action when access is lost, then remember it under the same key. A remembered directory provides access to its children through its usual listing and I/O methods.

`FileReference::ForgetAsync(key)` deletes the record. An absent key counts as success. Already opened references and streams retain their normal lifetime rules; the operating system can deny subsequent access when a grant is released or revoked.

Android provider selections with persistable grants, security-scoped bookmarks on iOS and macOS, and Web file system handles that can be stored in IndexedDB support this workflow. On Web, restore checks the browser's current permission and reports `PermissionDenied` when the browser requires a new grant. Temporary copies and browser uploads report `Unsupported`. Windows and Linux return `Unsupported`; applications using ordinary desktop filesystem access can save local paths instead.

Sandboxed macOS apps need the `com.apple.security.files.bookmarks.app-scope` entitlement to remember user-selected files or directories, in addition to the user-selected file access entitlement for their access mode.

### Passing a reference to a platform library

`PlatformPayload` can carry a `FileReference` through the common Android, Apple, or Web platform-language bridge without converting it to bytes or a local path.
Use this when a media player, document viewer, or similar platform API needs a native resource such as an Android `Uri` or browser `File`, or when retaining an Apple path or `NSURL` alone would lose the `FileReference` access lifetime.

```cpp
PlatformPayload OpenMediaRequest(FileReference source) {
  return PlatformPayload::Object{{"source", std::move(source)}};
}
```

The platform implementation obtains the value with its typed payload accessor:

- Android: `payload.requireFileReference()` returns `HuxerUIFileReference`; call `uri()` and keep the wrapper open while the native API uses the `Uri`.
- iOS and macOS: `payload.fileReference()` returns `FileReference`; its `fileURL` is an `NSURL`, and ARC retains the underlying C++ access capability.
- Web: `payload.requireFileReference()` returns `HuxerUI.FileReference`; `await reference.getFile()` resolves the browser `File`, and `close()` releases the wrapper's retained C++ share.

Android and Web wrappers have idempotent `close()` methods and reject access afterward. The Apple wrapper releases through ARC. Keep the wrapper alive for native use of the URI or URL. The [PlatformPayload boundary](../design/platform-content.md#platformpayload-boundary) defines how the bridge retains access.

### Creating a reference in a platform library

Android libraries can create a read-only capability with `HuxerUIFileReference.fromUri(context, uri)` and return it through `PlatformPayload.fileReference(reference)`.
The URI must be a readable `content://` file, not a directory; the SDK native library must already be loaded, but no window is required.
The synchronous factory queries metadata and checks read access, so run it off the UI thread when a provider may block.
Invalid arguments throw `IllegalArgumentException`, denied access throws `SecurityException`, and other access or construction failures throw `IOException`.
The factory uses existing access only: it neither takes persistent URI permissions nor releases or revokes grants when a wrapper closes.
Applications own persistent-grant policy, and later revocation still causes ordinary I/O failures.
An absent display name remains unknown; URI identifiers are not used as filenames.

iOS libraries can call `+[HUXFileReference copyFromFileURL:error:]`, imported into Swift as `try FileReference.copy(from: url)`, and return the result through the existing payload bridge.
The method synchronously copies a readable ordinary file into SDK-owned temporary storage, supports background queues, and must finish before a provider callback's temporary URL expires.
It returns a read-only capability, or `nil` and an `NSError` on failure, and removes partial temporary contents.
The copy uses bounded memory, preserves available source metadata, and balances only security-scoped access acquired by this call.
The original file remains caller-owned; the returned `fileURL` identifies the independent copy.
Shared references, open streams, and pending operations retain that copy after the creating module is disposed; the last owner triggers temporary-file cleanup.
Retaining `fileURL` or `AsFile()` alone does not retain the copy.

### Direct local-path access

`AsFile()` returns `std::optional<File>` for both files and directories without I/O, copying, or importing their contents.
Windows, macOS, and Linux local selections support it; path-backed iOS selections do as well, while Android document URIs and Web browser file handles return `std::nullopt`.
It returns the stored path, not a fresh existence or permission check, and does not update that path after a rename.

For an IDE, select a writable project directory with `OpenDirectoryAsync(true)`, retain the returned `FileReference` in the project session, and pass its `AsFile()` value to the file tree, indexer, or other path-based library.
The following helper retains its reference parameter until the asynchronous write completes:

```cpp
Task<bool> WriteProjectSettings(FileReference project, std::string json) {
  auto directory = project.AsFile();
  if (!directory) {
    co_return false;
  }
  co_return co_await directory->Child("settings.json").WriteStringAsync(std::move(json));
}
```

`File` remains a path value: it does not retain a security scope or temporary-file owner, enforce the reference's `CanWrite()` restriction, coordinate document access, or preserve native identity after path replacement.
Keep the original reference or a copy alive throughout path-based use when access depends on retained resources, such as macOS security-scoped selections and iOS temporary activation copies.
Actual operations still require system permission; continue using `FileReference` I/O when its grant restrictions or coordination are needed.

On Windows, retained handles can prevent an ancestor from being renamed. Release the reference and any derived references or pending work when you need only ordinary local-path behavior.

## FilePicker

Use `UseService<FilePicker>()` to get the window's picker. `OpenFileAsync()` returns one optional reference and `OpenFilesAsync()` returns a vector. `SaveFileAsync()` presents the platform save dialog and exports a local `File`. Cancellation, unsupported presentation, or other picker failure yields an empty selection or `false`. Use `CanOpenFiles()`, `CanOpenDirectories()`, or `CanSaveFiles()` before offering an action.

A `FilePickerFilter` combines extension and MIME hints. Use suffixes without a leading dot. MIME patterns can be exact or wildcarded, such as `image/*`. Give a nonempty filter a display name and validate the selected contents separately.

`OpenDirectoryAsync()` selects one existing directory. Pass `true` to require write access. The default keeps the reference and its descendants read-only. On a writable directory, `CanWrite()` describes the ability to create children. It does not guarantee the ability to replace every existing item.

Call Web pickers from a user action before asynchronous work consumes transient activation. Directory picking requires a browser with `showDirectoryPicker()` and live directory handles. Select source and destination from separate user actions. For embedded Android hosts, see [Android Activity results](platforms.md#window-owned-java-modules-and-activity-results).

## Receiving dropped files

Use `FileDropTarget` from `<huxerui/file_drop.h>` to receive ordinary files from another application.
Attach it to the receiving view with `.With(...)` and handle typed `FileDropEvents`:

```cpp
return Text("Drop images here")
    .With(FileDropTarget::Accepts({.extensions = {"png", "jpg"}, .content_types = {"image/*"}}))
    .On<FileDropEvents::Dropped>([](const std::vector<FileReference>& files, const FileDropEvent&) {
      OpenImages(files);
    });
```

The example's `OpenImages` is application code: receiving files does not automatically read, import, or write them.
Copy `FileReference` values to retain their access, then use their existing operations or explicitly import them into application storage.
The [Files example](../../examples/files/main.cpp) retains references through either a drop or its equivalent picker button.

`FileDropOptions` accepts filename suffixes without a leading dot, MIME types, and `allows_multiple` (true by default).
Suffixes and MIME types are alternatives; empty lists accept any ordinary file.
Matching ignores ASCII case, supports compound suffixes such as `tar.gz`, and supports `type/*` or `*/*` MIME patterns.
Every file must pass final validation; directories, empty batches, and mixed eligible/ineligible batches are rejected without partial delivery.
Metadata filtering is not content validation: still validate bytes before parsing them.

An optional `Accepts(options, predicate)` predicate receives a `FileDropOffer` with an optional count and possibly incomplete advertised MIME types.
Unknown metadata is not a proven match; hover acceptance is provisional until the actual references are prepared.
Keep predicates fast and free of side effects, file I/O, and permission requests.

Use `Entered` and `Exited` to drive hover feedback; `Moved` reports position updates.
Physical drop ends hover with `Exited` immediately, followed by deferred `Dropped` or `Failed` once preparation and final validation complete.
`Failed` receives an `IoError` and never delivers a partial batch.
Events run on the UI thread, and `Dropped`/`Failed` retain the target-local and window-local DIP coordinates from physical drop.
Several accepted drops may complete independently; compatible recomposition uses the current handlers, while unmounting cancels pending delivery.

A drop supplies read-only references. Keep a picker action available for keyboard, touch, and hosts without external drag support. See [External File Drop](../design/file-drop.md#native-access) for embedded-host integration and native access lifetime.

## Copying directory contents

`CreateDirectory(name)` and `CreateDirectoryAsync(name)` create one child or reuse an existing child directory under a writable directory reference.
Use `CopyFileFromAsync(source, name, overwrite)` to stream one `File` or file `FileReference` into a named child.
Names must be valid UTF-8 single segments, excluding empty names, `.`, `..`, NUL, `/`, and `\\`. Invalid caller names throw immediately, including for asynchronous calls.
Destination-specific restrictions are reported as file errors, not silently corrected names.

`CopyDirectoryContentsToAsync(destination, overwrite = false)` recursively copies a selected source directory into an existing `File` directory or writable directory `FileReference`.
The destination is the copy root: copying `Assets/logo.png` into `Imported` produces `Imported/logo.png`, not `Imported/Assets/logo.png`.
Empty directories and hidden files are included, existing directories merge, and destination-only entries remain untouched.
Existing files fail with `AlreadyExists` unless overwrite is explicitly enabled; file/directory conflicts and ambiguous source names always fail.
The operation never automatically renames or silently skips entries.

The successful `DirectoryCopySummary` counts finalized files, newly created directories below the root, and actual copied bytes.
The first failure returns an `IoError` with the operation stage and escaped relative path, not a partial success summary.
Completed output remains after failure or cancellation; neither a tree transaction nor rollback of an overwritten file is promised.
Cancellation stops subsequent work and attempts to close active I/O without resuming retired application code.

Keep the source and destination disjoint and avoid concurrent changes during copying. The operation rejects identifiable links, cycles, and overlaps. A provider can return `Unsupported` when it cannot verify containment. On Android, copying between two selected trees requires the same provider and Android API 29 or later.

The runnable [directory-copy example](../../examples/directory_copy/main.cpp) demonstrates separate source and destination selection, copying to application storage, overwrite behavior, cancellation, and summaries through the same public API on every platform.

## Web storage

Web application directories use browser-managed storage associated with the application's identity. Reads can use the synchronous API. Use asynchronous methods for mutations in the persistent data and cache directories, including `CloseAsync()` for incremental output. Those operations report success after browser synchronization. Quota and browser storage policies apply.

Temporary files are memory-resident. Local copies also occupy memory even when transferred in bounded chunks. A browser-selected file or directory remains a separate `FileReference`.

See [File and Application Storage Design](../design/files.md) for ownership, cancellation, and backend guarantees.
