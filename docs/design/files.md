# File and Application Storage Design

This document defines the public API, ownership, error, threading, path, picker, external-reference, and platform contracts for files and application storage.
The shared `File`, `FileInfo`, `FileResult<T>`, `FileSystem`, `FileReference`, and `FilePicker` surfaces, shared native worker execution, Runtime service integration, Windows, macOS, Linux, iOS, Android, and Web local implementations, focused local-file example, and fake picker/reference tests are implemented.
The Windows, macOS, Linux, iOS, Android, and Web picker/reference transports and the external-file flows in the focused example are also implemented.

## Goals

- Represent local files and directories with one public `File` value instead of exposing `std::filesystem::path` as the HuxerUI file identity.
- Provide common synchronous operations and explicitly named `Async` counterparts for work that may block.
- Expose stable application data, cache, temporary, executable, and current-working-directory locations without treating packaged resources as ordinary files.
- Preserve arbitrary Unicode paths on Windows without depending on the active ANSI code page.
- Resume asynchronous operations on the owning HuxerUI Task's UI thread and preserve structured cancellation.
- Keep the common API compact while retaining useful errors for reads, directory enumeration, and detailed metadata queries.
- Represent platform-granted external files without pretending that every grant has a stable local path.
- Keep `FileReference` independent of picker presentation so a future application-activation model can carry the same capability without routing through a Root View.
- Keep local paths distinct from platform-granted external files without prematurely defining a virtual filesystem extension protocol.

## Non-goals

The file API does not provide a public `FileSystem` subclassing contract, Zip filesystems, mount tables, general URI dispatch, symbolic-link creation, filesystem watching, general permission management, file locking, memory mapping, random-access handles, streaming I/O, directory pickers, or a document-provider abstraction.

It also does not persist picker grants across process launches or add drag-and-drop, clipboard, recent-file, or share-sheet APIs.
Those capabilities may reuse `FileReference` later without expanding the initial picker contract.

Open With, platform file associations, share intents, application activation, window selection, and multi-window document routing are outside this design.
They require one application-level activation contract rather than a file-specific View event.

Android `content://` values, Apple security-scoped URLs, browser file handles, and other granted external capabilities are not local paths.
They are represented by `FileReference` rather than values constructed as `File`.

Packaged HuxerUI resources remain owned by `PlatformResources` and `AppResources`.
A resource is not required to have a stable operating-system path and is never projected into `File` merely for API uniformity.

## Public model

All public declarations in this design live in `<huxerui/file.h>` and are re-exported from `<huxerui/huxerui.h>`.
`File`, `FileReference`, and `FilePicker` remain one cohesive file-capability surface rather than being split by whether a value originated inside or outside the application sandbox.

The public model consists of:

- `File`, an immutable value identifying a normalized local file or directory path.
- `FileInfo`, detailed metadata returned by `Stat()`.
- `FileError` and `FileResult<T>`, used only where a legitimate empty value must remain distinct from failure.
- `AppDirectories`, immutable application-owned locations represented as `File` values.
- `FileSystem`, the Runtime-installed Root Service that exposes application and process directories.
- `FileReference`, an immutable handle to one platform-granted external file.
- `FilePicker`, the Runtime-installed Root Service for opening and saving user-visible files.

`File` does not have an empty state and does not provide `HasValue()`.
Code uses `std::optional<File>` when absence is meaningful.
A `File` may identify a path that does not currently exist.

## File construction and identity

The initial public construction surface is:

```cpp
class File final {
public:
  explicit File(std::string_view path);
  explicit File(std::u8string_view path);
  explicit File(const Uri& uri);
  File(const File& parent, std::string_view child);

  File(const File&) = default;
  File(File&&) noexcept = default;
  File& operator=(const File&) = default;
  File& operator=(File&&) noexcept = default;

  bool operator==(const File&) const noexcept;
};
```

There is no default constructor.
The string constructors interpret their input as UTF-8 and bind the value to the process's local filesystem.
A relative input is resolved against the process working directory at construction so later working-directory changes cannot retarget an existing value.

The parent-and-child constructor starts from the parent's path and accepts exactly one child name.
It rejects an empty name, `.`, `..`, an absolute path, and a name containing a platform separator.
`Resolve()` is the explicit operation for a multi-segment relative path.

Construction validates path syntax and UTF-8 but does not touch the filesystem, resolve symbolic links, inspect permissions, or require the target to exist.
Invalid caller input throws `std::invalid_argument` synchronously.

File equality compares normalized absolute local paths.

`File(const Uri&)` accepts only supported absolute local `file:` URIs and gives the result no permissions beyond the process's existing filesystem access.
`File::ToUri()` encodes the absolute path already retained by `File`; it never resolves the value again against a later current directory.
The complete syntax, authority, encoding, Windows drive, UNC, and capability boundaries are defined in [URI and Local File URI](uri.md).

## Lexical path operations

Lexical operations are synchronous, do not perform I/O, and do not have `Async` variants:

```cpp
class File final {
public:
  [[nodiscard]] std::string Path() const;
  [[nodiscard]] std::string Name() const;
  [[nodiscard]] std::string Stem() const;
  [[nodiscard]] std::string Extension() const;

  [[nodiscard]] std::optional<std::string> ParentPath() const;
  [[nodiscard]] std::optional<File> Parent() const;

  [[nodiscard]] File Child(std::string_view name) const;
  [[nodiscard]] File Resolve(std::string_view relative_path) const;
  [[nodiscard]] Uri ToUri() const;
};
```

`Path()` returns a normalized absolute UTF-8 path.
Windows output uses `/` as the public separator while preserving drive and UNC roots.
The Windows implementation converts that representation to a platform UTF-16 path before I/O.

`Name()`, `Stem()`, and `Extension()` operate lexically on the final path segment.
`Parent()` and `ParentPath()` return `std::nullopt` for a filesystem root.
`Child()` applies the same single-name validation as the parent-and-child constructor.
`Resolve()` accepts a relative path and performs lexical normalization without resolving symbolic links.
`ToUri()` percent-encodes the retained absolute path using the host-specific local file URI rules.

## Simple status predicates

The common status predicates return `bool` and do not use `FileResult`:

```cpp
[[nodiscard]] bool Exists() const;
[[nodiscard]] bool IsFile() const;
[[nodiscard]] bool IsDirectory() const;
```

These are convenience queries for ordinary local-file code.
They return `false` when the path does not exist or its status cannot be determined.
They do not have `Async` variants because callers that require non-blocking metadata or a diagnostic use `StatAsync()` once and inspect its `FileInfo`.

## Detailed metadata

```cpp
enum class FileType {
  File,
  Directory,
  Other,
};

struct FileInfo {
  FileType type = FileType::Other;
  std::uint64_t size = 0;
  std::optional<std::chrono::system_clock::time_point> modified_at;

  bool operator==(const FileInfo&) const = default;
};

[[nodiscard]] FileResult<FileInfo> Stat() const;
[[nodiscard]] Task<FileResult<FileInfo>> StatAsync() const;
```

`size` is the byte length of an ordinary file and is zero for other types.
`modified_at` is absent when the platform cannot provide a meaningful value.

The initial metadata operation follows symbolic links when determining `FileType`.
Deletion still removes the named link rather than recursively entering its target.

## File results

`FileResult<T>` is a focused expected-like value rather than a general framework Result type:

```cpp
enum class FileErrorCode {
  NotFound,
  PermissionDenied,
  NotDirectory,
  IsDirectory,
  TooLarge,
  InvalidEncoding,
  Unsupported,
  Io,
};

struct FileError {
  FileErrorCode code;
  std::string message;

  bool operator==(const FileError&) const = default;
};

template <class T> class [[nodiscard]] FileResult final {
public:
  explicit FileResult(T value);
  explicit FileResult(FileError error);

  [[nodiscard]] bool Succeeded() const noexcept;

  [[nodiscard]] T& Value() &;
  [[nodiscard]] const T& Value() const&;
  [[nodiscard]] T&& Value() &&;

  [[nodiscard]] FileError& Error() &;
  [[nodiscard]] const FileError& Error() const&;
};
```

The public API has no `FileResult<void>` specialization.
Mutating operations return `bool`; `FileResult<T>` remains limited to metadata, enumeration, and reads where empty output is valid and cannot represent failure.

Calling `Value()` on an error or `Error()` on a value throws `std::logic_error`.
Operational failures never escape these result-returning methods as filesystem exceptions.

## Reading

```cpp
[[nodiscard]] FileResult<Bytes> ReadBytes() const;
[[nodiscard]] Task<FileResult<Bytes>> ReadBytesAsync() const;

[[nodiscard]] FileResult<std::string> ReadString() const;
[[nodiscard]] Task<FileResult<std::string>> ReadStringAsync() const;
```

Read operations load the complete file into memory.
An empty `Bytes` or string value is a successful empty file, which is why reads retain `FileResult<T>`.
The implementation reports a file that cannot fit in the owned result as `TooLarge` before allocating an invalid buffer.

`ReadString()` defines text as UTF-8.
It accepts and removes one leading UTF-8 byte-order mark, rejects invalid UTF-8 with `InvalidEncoding`, and does not transform line endings.
Additional encodings and line-oriented APIs are not part of the public surface.

## Writing and appending

Mutating operations report whether they reached their documented target state:

```cpp
[[nodiscard]] bool WriteBytes(std::span<const std::byte> bytes) const;
[[nodiscard]] Task<bool> WriteBytesAsync(Bytes bytes) const;

[[nodiscard]] bool WriteString(std::string_view value) const;
[[nodiscard]] Task<bool> WriteStringAsync(std::string value) const;

[[nodiscard]] bool AppendBytes(std::span<const std::byte> bytes) const;
[[nodiscard]] Task<bool> AppendBytesAsync(Bytes bytes) const;

[[nodiscard]] bool AppendString(std::string_view value) const;
[[nodiscard]] Task<bool> AppendStringAsync(std::string value) const;
```

Asynchronous writes take ownership of bytes or text so no borrowed buffer survives suspension.
Synchronous writes may borrow their input for the duration of the call.

`WriteBytes()` and `WriteString()` create a missing file and truncate an existing ordinary file.
They require the parent directory to exist and return `false` for a directory target or any operational failure.
`AppendBytes()` and `AppendString()` create a missing file and otherwise append to an ordinary file.

String writes validate UTF-8, write no byte-order mark, and perform no newline conversion.
Invalid string input is caller configuration and throws `std::invalid_argument` before starting either a synchronous or asynchronous operation.

The public API does not add write options, implicit parent creation, or atomic replacement.
Code creates parents explicitly with `CreateDirectories()`.
Atomic write is not supported; it would require a separate explicitly named operation rather than an ambiguous option on every write.

## Directories and deletion

```cpp
[[nodiscard]] FileResult<std::vector<File>> ListChildren() const;
[[nodiscard]] Task<FileResult<std::vector<File>>> ListChildrenAsync() const;

[[nodiscard]] bool CreateDirectory() const;
[[nodiscard]] Task<bool> CreateDirectoryAsync() const;

[[nodiscard]] bool CreateDirectories() const;
[[nodiscard]] Task<bool> CreateDirectoriesAsync() const;

[[nodiscard]] bool Delete() const;
[[nodiscard]] Task<bool> DeleteAsync() const;

[[nodiscard]] bool DeleteRecursively() const;
[[nodiscard]] Task<bool> DeleteRecursivelyAsync() const;
```

`ListChildren()` returns direct children, includes hidden entries, does not recurse, and does not impose a sort order.
An empty vector is a successful empty directory, so enumeration retains `FileResult<T>`.

`CreateDirectory()` requires its parent to exist.
`CreateDirectories()` creates missing ancestors.
Both return `true` when the target already exists as a directory and `false` when it exists as another type or creation fails.

`Delete()` removes an ordinary file, symbolic link, or empty directory.
It returns `true` when the target is already absent because the requested final state is satisfied.
`DeleteRecursively()` removes a directory tree but never follows a symbolic link into another tree.

The implementation rejects recursive deletion of a filesystem root or an application directory root.
The API does not provide a flag that disables this protection.

## Copying and moving

```cpp
[[nodiscard]] bool CopyTo(const File& destination, bool overwrite = false) const;
[[nodiscard]] Task<bool> CopyToAsync(File destination, bool overwrite = false) const;

[[nodiscard]] bool MoveTo(const File& destination, bool overwrite = false) const;
[[nodiscard]] Task<bool> MoveToAsync(File destination, bool overwrite = false) const;
```

The single Boolean argument directly expresses whether an existing destination may be replaced.
The API does not introduce an `ExistingFilePolicy` enum for that binary choice.

The initial `CopyTo()` copies one ordinary file and does not recursively copy directories.
Directory copy is not supported because the current copy operation must not hide an unbounded traversal.

`MoveTo()` uses the local filesystem's platform move operation and returns `false` when it cannot complete that operation.
It does not silently turn a cross-device move into copy followed by deletion.

## Application directories

```cpp
struct AppDirectories {
  std::optional<File> executable_directory;
  File data_directory;
  File cache_directory;
  File temporary_directory;
};

class FileSystem final {
public:
  ~FileSystem();

  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  FileSystem(FileSystem&&) = delete;
  FileSystem& operator=(FileSystem&&) = delete;

  [[nodiscard]] const AppDirectories& Directories() const noexcept;
  [[nodiscard]] File CurrentDirectory() const;
};
```

Runtime automatically installs one `FileSystem` Root Service before application RootHooks run:

```cpp
auto files = UseService<FileSystem>();
File settings{files->Directories().data_directory, "settings.json"};
```

`data_directory` contains durable application-private files that the operating system does not normally evict.
`cache_directory` contains reconstructible data that may be cleared by the operating system or application.
`temporary_directory` contains short-lived files and does not promise persistence across launches.
`executable_directory` is present only when the platform exposes a meaningful local executable location.

`CurrentDirectory()` reports the process working directory and is distinct from `executable_directory`.
HuxerUI does not provide a process-wide working-directory mutation because it would affect other Runtime instances, libraries, and threads.
Application storage must use the semantic application directories rather than depend on a launcher's working directory.

The application directories are created and validated before the service is published.
Platform shells determine their application identity from bundle, package, or executable metadata rather than adding another identifier to the static `Application` declaration.

The public API does not expose a cross-platform Documents directory.
User-visible documents and granted external locations use the separate `FileReference` and `FilePicker` contracts below rather than an unrestricted local path.

## External file references

`FileReference` represents permission to access one external file selected by the user or supplied to the application by the platform:

```cpp
class FileReference final {
public:
  FileReference(const FileReference&) = default;
  FileReference(FileReference&&) noexcept = default;
  FileReference& operator=(const FileReference&) = default;
  FileReference& operator=(FileReference&&) noexcept = default;
  ~FileReference();

  [[nodiscard]] std::string Name() const;
  [[nodiscard]] std::optional<std::uint64_t> Size() const;
  [[nodiscard]] std::optional<std::string> ContentType() const;
  [[nodiscard]] bool CanWrite() const noexcept;

  [[nodiscard]] Task<FileResult<Bytes>> ReadBytesAsync() const;
  [[nodiscard]] Task<FileResult<std::string>> ReadStringAsync() const;
  [[nodiscard]] Task<bool> ImportToAsync(File destination, bool overwrite = false) const;
  [[nodiscard]] Task<bool> ReplaceWithAsync(File source) const;
};
```

There is no default constructor, empty state, `HasValue()`, path accessor, or implicit conversion to `File`.
Code uses `std::optional<FileReference>` when absence is meaningful.
`Name()` is a UTF-8 display filename and not a trustworthy local path segment without the same validation used by `File::Child()`.
`Size()` and `ContentType()` are optional metadata hints obtained without reading the entire file.
`ContentType()` returns a MIME string when the platform can supply or map one and does not expose a platform-specific type identifier.

External references expose asynchronous I/O only because their platform transport may involve a provider process, coordinated access, security-scope activation, or a browser permission check.
`ReadStringAsync()` follows the same UTF-8, byte-order-mark, and line-ending contract as `File::ReadStringAsync()`.
An expired or revoked grant reports `NotFound` or `PermissionDenied` through the existing `FileResult` error model rather than adding a second result type.

`ImportToAsync()` streams the referenced content into a local `File` and requires the destination parent to exist.
It replaces an existing destination only when `overwrite` is `true`.
`ReplaceWithAsync()` writes a local file back through the original platform grant and returns `false` when `CanWrite()` is false or the operation fails.
Neither operation exposes a platform path or silently grants broader access.

Each value retains shared private platform state.
Copying a value retains the grant, and destruction of the last copy releases process-scoped resources such as Apple security-scope access, Android provider state, or a browser handle.
The public API does not serialize references or promise that a grant remains valid after application restart.

## File picker

The picker uses compact cross-platform filters and keeps platform presentation and permission handling in the platform adapter:

```cpp
struct FilePickerFilter {
  std::string name;
  std::vector<std::string> extensions;
  std::vector<std::string> content_types;
};

struct SaveFileOptions {
  std::string suggested_name;
  FilePickerFilter filter;
};

class FilePicker final {
public:
  ~FilePicker();

  FilePicker(const FilePicker&) = delete;
  FilePicker& operator=(const FilePicker&) = delete;
  FilePicker(FilePicker&&) = delete;
  FilePicker& operator=(FilePicker&&) = delete;

  [[nodiscard]] bool CanOpenFiles() const noexcept;
  [[nodiscard]] bool CanSaveFiles() const noexcept;

  [[nodiscard]] Task<std::optional<FileReference>> OpenFileAsync(FilePickerFilter filter = {}) const;
  [[nodiscard]] Task<std::vector<FileReference>> OpenFilesAsync(FilePickerFilter filter = {}) const;
  [[nodiscard]] Task<bool> SaveFileAsync(File source, SaveFileOptions options = {}) const;
};
```

Runtime installs one `FilePicker` Root Service.
Separating `OpenFileAsync()` and `OpenFilesAsync()` keeps result cardinality visible in the type and avoids an `allow_multiple` option whose result would still need another shape.

Extensions omit the leading dot, and content types use exact MIME strings, `type/*`, or `*/*`.
One filter represents the union of every listed extension and content type because only some desktop pickers can expose several user-selectable filter groups.
An empty filter permits all files; a configured filter requires a non-empty display name for platform pickers that show one.
The filter is advisory because platform pickers differ in their filtering support; application code still validates selected content.
Malformed filters or suggested names throw `std::invalid_argument` before platform presentation.

User cancellation is an ordinary outcome, not an error.
Single selection returns `std::nullopt`, multiple selection returns an empty vector, and saving returns `false` when the user cancels.
An unsupported or failed platform operation produces the same empty or false outcome in the current compact API; capability predicates let applications avoid presenting unavailable actions.

`SaveFileAsync()` streams from a local `File` to the destination selected by the platform picker.
The platform interface owns overwrite confirmation, so the common operation does not add another overwrite argument.
Saving bytes or strings directly is outside the public surface; code may write an application temporary file and save that file explicitly.

Only one picker presentation may be active for a Runtime.
Concurrent requests are serialized in call order.
Task cancellation attempts to dismiss the platform picker when supported and otherwise detaches the continuation; Runtime destruction cancels queued requests and releases their retained state.

## Application activation boundary

Open With, share intents, document URL contexts, and equivalent passive file-open requests are application activations rather than file-picker results or View events.
They may arrive before a Runtime exists and may require the application to create a document window, reuse an existing window, select a non-default root, or reject the request.
Delivering them to the currently committed Root View would assign application and window policy to an arbitrary UI tree.

The file API therefore defines no `FileEvents`, root callback, cold-start queue, or Runtime dispatch operation.
`FileReference` is the capability carried by `FileActivation`; activation delivery is defined by the application boundary rather than the file API.

The [Application Activation and Lifecycle Design](application.md) owns the complete route from platform activation to an application-selected Runtime:

```text
Platform application activation
    -> application activation policy
    -> create or select a platform target
    -> construct or notify the target Runtime
```

Platform application metadata remains owned by the shell: Android intent filters, Apple document types and URL declarations, Windows associations, Linux desktop MIME declarations, and Web application handlers are not moved into `AppOptions`, CMake, or the file API.

## Runtime and local ownership

`FileSystem` is a built-in Runtime capability like `HttpClient`, not a PlatformModule and not a user-installed RootHook service.
It owns immutable application-directory values and registers those roots with process-local recursive-deletion safeguards.

Each `File` stores only a normalized absolute UTF-8 local path and remains usable after its originating `FileSystem` handle or component has gone out of scope.
Local operations are private implementation functions rather than a polymorphic provider interface.

The public `FileSystem` is final.
The API does not add filesystem registration, mount names, URL schemes, or a public `ZipFileSystem` subclass merely to reserve that possibility.
An archive or virtual filesystem requires a separate deliberate public design rather than an unused local-file abstraction.

## Synchronous and asynchronous execution

Lexical operations and the three convenience status predicates are synchronous only.
Operations that may transfer data, enumerate directories, or mutate storage provide an explicitly named `Async` counterpart.

Non-Web platform asynchronous work uses the bounded process-wide worker executor shared with RunWorker rather than creating a File-specific pool or one thread per operation.
Web uses the browser event loop and its persistent-storage completion callback instead of creating workers or inheriting an unused provider contract.

An asynchronous call validates caller-owned values before returning its lazy Task.
Once awaited from a launched HuxerUI Task, platform implementations perform filesystem work away from the UI thread, while Web schedules it through the browser event loop.
Every implementation resumes through the owning `TaskExecution` and `UIThreadDispatcher`.
Code after `co_await` may therefore update State directly.

Canceling the owning `TaskHandle`, retiring its TaskScope, or destroying Runtime detaches the continuation.
An operating-system call that cannot be interrupted may finish in the background, but its late result is ignored and never resumes application code.

Synchronous I/O remains available for command-line code, background threads, startup work, tests, and intentionally small local operations.
UI event handlers and Lifecycle-owned work should prefer the asynchronous variants.

## UTF-8 and Windows paths

Every public `std::string` path and filename is UTF-8.
The API never interprets it through the process locale or Windows ANSI code page.

The Windows implementation converts UTF-8 to UTF-16 once at its boundary and uses wide-character Win32 APIs.
It supports drive roots, UNC paths, non-ASCII names, emoji, and long paths without calling `std::filesystem::path::string()` for public conversion.
Internal Windows code may use `std::filesystem::path` constructed from a platform wide path, but that type is not the public HuxerUI file identity.

Windows input accepts `/` and `\` as separators.
Normalized public output uses `/` while preserving the root and filename text exactly except for lexical normalization.
POSIX implementations continue to treat `\` as an ordinary filename character.

Paths preserve case and do not call canonicalization at construction.
Filesystem-specific case folding, symbolic-link resolution, and permission checks occur only when an operation reaches the filesystem.

## Platform mapping

macOS maps durable data to Application Support, cache data to Caches, temporary data to the platform temporary directory, and the executable location to the application executable directory.
Its picker transport presents `NSOpenPanel` and `NSSavePanel`, maps the union filter through UTType where possible, and retains security-scoped URL access inside `FileReference` when required.
Reads, imports, and replacements use coordinated file access off the UI thread, while cancellation dismisses active panels and detaches work that cannot be interrupted safely.

iOS independently maps durable data to Application Support, cache data to Caches, and temporary data to the application temporary directory.
Its executable directory is read-only and is never used as a replacement for packaged HuxerUI resources.
Its picker transport presents `UIDocumentPickerViewController`, maps union filters through `UTType`, and retains security-scoped URLs inside `FileReference`.
Application document activations use the same retained capability for open-in-place URLs. When UIKit marks a delivery as copy-before-use, the application adapter copies it into a private read-only temporary snapshot before returning from the native callback, and the shared `FileReference` state removes that snapshot after its last owner releases it.
External reads, imports, and replacements use coordinated file access off the main thread.
Saving exports a copy of the local source, stages a temporary copy only when the suggested filename differs, and removes that staging directory after completion or cancellation.

Android obtains the durable and cache roots from the application Context, creates an application-owned temporary child under the cache root, and exposes the packaged C++ library directory as a read-only executable location.
These private locations require no broad storage permission.
Its picker transport uses `ACTION_OPEN_DOCUMENT` for single and multiple selection and `ACTION_CREATE_DOCUMENT` for saving without requesting broad storage permission.
Extensions are mapped through `MimeTypeMap` when possible; an unrecognized extension deliberately widens the advisory filter rather than hiding a valid document.
The returned `content://` grant remains private to `FileReference`, while display name, size, MIME type, and provider write support populate its public metadata.
Reads, imports, replacements, and save copies use a bounded Java worker executor, close active streams during cancellation, and atomically replace an existing local import destination within its parent directory.
The initial adapter retains process-scoped URI access only and does not call `takePersistableUriPermission()`.

`HuxerUIActivity` installs the SAF launcher and forwards Activity results automatically.
An embedded `HuxerUIView` does not cast its arbitrary Context to Activity; its owner installs `HuxerUIView.FilePickerLauncher` and forwards matching results through `dispatchFilePickerResult()`.
Without that host capability, `CanOpenFiles()` and `CanSaveFiles()` return `false` while local `FileSystem` access remains available.

Windows uses the application's Local App Data identity for durable data, application-specific cache and temporary children, and the directory containing the process executable.
Its picker transport uses the COM system file dialogs owned by the HuxerUI window for active selection.
Filters map extension values directly and exact MIME types through the Windows registry; wildcard or unknown MIME mappings deliberately widen the system filter rather than excluding valid documents.
Selected filesystem paths remain private to `FileReference`, and metadata reports the filename, size, registered MIME type when available, and basic write capability.
Reads, imports, replacements, and save copies reuse the shared core worker executor while dialog presentation and cancellation stay on the existing UI dispatcher.
The adapter does not request persistent grants, expose platform paths publicly, or add a second Windows-specific file abstraction.

Linux uses the UTF-8 filename resolved from `/proc/self/exe` as its application identity and resolves the executable directory from that same path independently of the process working directory.
Durable data uses `$XDG_DATA_HOME/<executable-name>` or `$HOME/.local/share/<executable-name>`, while cache data uses `$XDG_CACHE_HOME/<executable-name>` or `$HOME/.cache/<executable-name>`.
Relative, empty, or invalid UTF-8 XDG paths are ignored.
Temporary data uses `<XDG_RUNTIME_DIR>/<executable-name>` only when the runtime root is an owner-only directory belonging to the effective user.
Otherwise it uses `<system-temporary-directory>/huxerui-<effective-uid>/<executable-name>` and requires both created children to remain owner-only directories.
An unsafe existing fallback directory fails initialization rather than being reused.
Renaming the executable selects different storage, and executable files with the same name share one per-user application directory.
Its picker transport uses `org.freedesktop.portal.FileChooser` through GDBus on a dedicated GLib main-context thread and reports both capabilities as unavailable when the session bus or portal service cannot be reached.
When GTK uses its X11 backend, the current native window is encoded as `x11:<hex-xid>` for the portal parent; other GDK backends and requests made before realization use an empty parent.
One unpredictable handle token is used per request, the predicted Request path is subscribed before the method call, and a backend-returned legacy path replaces that subscription when necessary.
Task cancellation completes the transport operation immediately and closes the portal Request so Runtime-level picker serialization can advance without waiting for a Response that will not arrive after Close.
Filters map extensions to glob rules and MIME values to MIME rules inside one union filter.
Successful `file://` results remain private Linux `FileReference` state; metadata reflects the selected file, while reads, imports, replacements, and save copies reuse the shared core worker executor.
Saving reports success only after the source file has been copied over the portal-confirmed destination.
This implementation does not add GTK or Qt fallback dialogs, Wayland parent handles, directory selection, or persistent grants.

Web maps application-private storage through the browser filesystem design below and has no executable directory.
Its picker transport uses browser file handles when available and an input-element fallback for opening; unsupported save capabilities remain visible through the capability contract.

## Web application storage

The Web implementation preserves the existing `File`, `FileSystem`, and explicitly named asynchronous operations without adding a browser-specific public file type, provider interface, or mount API.
It uses Emscripten's synchronous virtual filesystem for local path behavior and IDBFS for application-private persistence.
Browser `File` values and File System Access handles remain inside `FileReference` because a user-granted external capability is not an application-private local path.

### Browser picker capabilities

Opening prefers `showOpenFilePicker()` in a secure context so a selected handle can support fresh reads and write-back.
When that API is unavailable, the adapter creates a transient `<input type="file">`, maps the union filter to its `accept` attribute, and retains each selected browser `File` as a read-only `FileReference`.
Both paths support single and multiple selection, metadata, asynchronous reads, and import into application-local storage.

`CanSaveFiles()` is true only when `showSaveFilePicker()` and writable file handles are available.
The adapter does not treat an anchor download as successful picker output because a download cannot report platform cancellation, overwrite choice, or completed replacement through the shared `bool` result.
Saving streams the selected local Emscripten file only after the browser returns a destination handle and reports success after the writable stream closes.

Browser picker presentation requires transient user activation.
Application code starts the picker directly from a click or equivalent event and does not place `Delay()`, HTTP work, or another suspension before `OpenFileAsync()`, `OpenFilesAsync()`, or `SaveFileAsync()`.
A browser dialog cannot generally be dismissed by script, so Task cancellation detaches the HuxerUI continuation while the transport waits for the platform picker to settle before advancing the shared presentation queue.

Handle and browser `File` lifetimes follow the corresponding `FileReference` and remain session scoped.
The Web backend does not persist granted handles in IndexedDB or add browser-specific permission methods to the public API.

### Storage identity and directories

Each Web application supplies one stable storage key through its JavaScript shell:

```js
const module = await createHuxerUIApp({
  huxeruiStorageKey: "com.example.app",
});

const session = module.mountHuxerUI("#huxerui-root");
```

Generated CLI shells derive this value from the project identifier, and repository examples use their configured bundle identifier.
Custom shells provide it explicitly.
The value is host-owned storage identity and does not enter `AppOptions`, the shared `Application` declaration, or C++ PlatformModule payloads.
The implementation rejects a missing or invalid key instead of deriving one from a URL, output filename, or document title whose later change would make existing data appear lost.

The encoded key selects one application-specific IDBFS mount and one temporary subtree:

```text
/huxerui/<storage-key>/
    data/
    cache/

/tmp/huxerui/<storage-key>/
```

`data_directory` maps to the persistent `data` child, `cache_directory` maps to the persistent but reconstructible `cache` child, and `temporary_directory` maps to MEMFS under `/tmp`.
`executable_directory` is `std::nullopt`, and `CurrentDirectory()` continues to report the Emscripten process working directory rather than inventing an executable location.
Data and cache share one IDBFS mount so restoration and persistence have one ordering domain.
Browser quota, storage eviction, private-browsing policy, and user storage controls remain authoritative; HuxerUI does not request durable-storage permission automatically or promise that cache data cannot be evicted.

### Initialization and Runtime ownership

Web storage is initialized once per Emscripten module before any HuxerUI Runtime may mount.
A Web pre-initialization script validates the storage key, mounts IDBFS, restores IndexedDB contents with `FS.syncfs(true)`, creates the application directories, and releases an Emscripten run dependency only after restoration completes.
The module factory therefore does not resolve and `mountHuxerUI()` cannot construct application UI while persistent files are still absent from the virtual filesystem.

Every Runtime in the same module receives a `FileSystem` Root Service using the already initialized directories.
Runtime creation never mounts, restores, or clears IDBFS again, so several host elements share the application storage without introducing per-window databases or races.

If storage initialization fails, Runtime mounting fails with a clear HuxerUI diagnostic.
The implementation does not silently publish a volatile `FileSystem`, because reporting successful durable writes that disappear after reload would violate the application-directory contract.

### Synchronous operation policy

Status queries, metadata, reads, and directory enumeration may execute synchronously against the fully restored virtual filesystem.
Synchronous mutation remains available for paths in MEMFS, including `temporary_directory`, because the documented result requires only the in-memory operation to finish.

A synchronous operation that would mutate the persistent data or cache subtree returns `false` without first changing the virtual filesystem.
This applies to writing, appending, directory creation, deletion, copying into persistent storage, and moving when either the source or destination is persistent.
The restriction is enforced in the shared local-operation path using the persistent root owned by the Web file implementation rather than repeated independently across Web adapter methods.
It preserves the existing `bool` contract: success is never reported before IndexedDB persistence can complete.

### Asynchronous execution and persistence

Web does not enable Emscripten pthreads, create a worker-backed filesystem, or require cross-origin isolation for local file operations.
It replaces the platform filesystem executor with one module-owned serial queue scheduled through the browser event loop.
Virtual filesystem access still runs on the browser main thread and may briefly occupy that thread; documentation must not claim that Web file work executes on a background thread.

An asynchronous persistent mutation performs the virtual filesystem operation and then explicitly calls `FS.syncfs(false)`.
Its Task completes with `true` only after both stages succeed.
Temporary mutations complete after their virtual filesystem operation, while asynchronous reads and queries resume after their queued operation finishes.
The queue retains a persistent mutation until its synchronization callback returns before starting the next operation, preventing overlapping IDBFS snapshots from reordering writes.

IDBFS automatic persistence is not the Task completion mechanism because it does not expose the result of the particular durable operation to that Task.
Explicit synchronization keeps storage failures observable through the existing `bool` or `FileResult<T>` outcome without adding a Web-specific result type.
After a persistent mutation has begun, the implementation attempts synchronization even when a compound virtual operation reports failure because part of that operation may already have changed the mounted tree.
The implementation does not add an in-memory rollback transaction for partially completed filesystem operations.

Canceling the owning Task detaches its continuation but does not discard an operation or synchronization already in progress.
The queue may finish that work to preserve filesystem ordering, while retired application code is never resumed.
This matches the platform rule that an uninterruptible filesystem operation may complete after cancellation.

### Internal boundary

The Web implementation uses narrow internal scheduling, persistent-root classification, synchronization, and `FileSystem` construction functions shared by `src/file.cpp` and `platform/web/web_file.cpp`.
It does not introduce a public or private polymorphic `FileBackend`, a second service registry, a PlatformModule instance, or Web-only methods on `File`.
Emscripten glue owns IDBFS mounting and synchronization, the Web adapter publishes the initialized application directories, and the shared file implementation retains path validation, operation semantics, Task cancellation, and result mapping.

## Integration with existing APIs

`ImageAsset::FromFile(const std::filesystem::path&)` should migrate to `ImageAsset::FromFile(const File&)` with the rest of the approved public breaking change.
Asynchronous image loading may read bytes through `ReadBytesAsync()` and construct the existing encoded `ImageAsset` without adding an image-specific file transport.

Future HTTP upload and download APIs may accept `File` or `FileReference` through explicit overloads.
They do not change `HttpResponse::body` or turn ordinary HTTP requests into implicit file transfers.

Packaged resources continue to use `ResourceId`, `RawAsset`, `ImageAsset`, and `PlatformResources`.
No implementation exposes a temporary resource extraction path merely so a resource can be passed as `File`.

## Validation

Shared lexical tests cover UTF-8, roots, relative construction, parent traversal, extension extraction, child validation, equality, Windows drive and UNC forms, and paths that do not exist.

Shared local-file tests use isolated temporary directories to cover empty and non-empty reads, UTF-8 validation, enumeration, metadata, writing, appending, directory creation, deletion protection, copy and move overwrite behavior, Task cancellation, Runtime teardown, and UI-thread resumption.

Shared fake picker and reference tests cover single and multiple selection, active and queued cancellation, unsupported capabilities, filter validation, serialized presentation, grant retention, import and replacement, and UI-thread resumption.
Platform picker coverage includes revoked access, Runtime teardown, platform dismissal, and grant cleanup where applicable.

Linux picker integration tests use `GTestDBus` with a private bus and fake `org.freedesktop.portal.Desktop` service.
They cover capability probing, optional X11 parenting, filter and suggested-name wire values, predicted and returned Request handles, success and failure responses, URI validation, cancellation with Request.Close, late responses, metadata, reference I/O, and completed save copies without displaying desktop UI.

Each platform verifies its application-directory mapping, Unicode conversion, protected roots, picker mapping, failure mapping, asynchronous execution, and grant cleanup without claiming unavailable behavior.

Web storage validation covers initial restoration before Runtime mounting, persistence across page reloads, temporary-data loss across reloads, isolation between storage keys, serialized persistence, failure mapping, cancellation, Runtime teardown, and reuse of one initialized mount by multiple Runtime instances.
