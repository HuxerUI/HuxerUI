# File and Application Storage Design

## Status

This document defines the public API, ownership, error, threading, path, picker, external-reference, and platform contracts for files and application storage.
The shared `File`, `FileInfo`, `FileResult<T>`, and `FileSystem` surface, bounded asynchronous executor, Runtime service integration, macOS, iOS, and Android local implementations, and focused example are implemented.
Application-directory mappings for Windows, Linux, and Web, along with `FileReference` and `FilePicker`, remain proposed.

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

The initial implementation does not provide a public `FileSystem` subclassing contract, Zip filesystems, mount tables, URI schemes, symbolic-link creation, filesystem watching, general permission management, file locking, memory mapping, random-access handles, streaming I/O, directory pickers, or a document-provider abstraction.

It also does not persist picker grants across process launches or add drag-and-drop, clipboard, recent-file, or share-sheet APIs.
Those capabilities may reuse `FileReference` later without expanding the initial picker contract.

Open With, native file associations, share intents, application activation, window selection, and multi-window document routing are outside this design.
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
};
```

`Path()` returns a normalized absolute UTF-8 path.
Windows output uses `/` as the public separator while preserving drive and UNC roots.
The Windows implementation converts that representation to a native UTF-16 path before I/O.

`Name()`, `Stem()`, and `Extension()` operate lexically on the final path segment.
`Parent()` and `ParentPath()` return `std::nullopt` for a filesystem root.
`Child()` applies the same single-name validation as the parent-and-child constructor.
`Resolve()` accepts a relative path and performs lexical normalization without resolving symbolic links.

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

The initial API has no `FileResult<void>` specialization.
Mutating operations return `bool`; `FileResult<T>` remains limited to metadata, enumeration, and reads where empty output is valid and cannot represent failure.

Calling `Value()` on an error or `Error()` on a value throws `std::logic_error`.
Operational failures never escape these result-returning methods as filesystem exceptions.

## Reading

```cpp
[[nodiscard]] FileResult<std::vector<std::byte>> ReadBytes() const;
[[nodiscard]] Task<FileResult<std::vector<std::byte>>> ReadBytesAsync() const;

[[nodiscard]] FileResult<std::string> ReadString() const;
[[nodiscard]] Task<FileResult<std::string>> ReadStringAsync() const;
```

The initial operations read the complete file into memory.
An empty vector or string is a successful empty file, which is why reads retain `FileResult<T>`.
The implementation reports a file that cannot fit in the owned result as `TooLarge` before allocating an invalid buffer.

`ReadString()` defines text as UTF-8.
It accepts and removes one leading UTF-8 byte-order mark, rejects invalid UTF-8 with `InvalidEncoding`, and does not transform line endings.
Additional encodings and line-oriented APIs are not part of the initial surface.

## Writing and appending

Mutating operations report whether they reached their documented target state:

```cpp
[[nodiscard]] bool WriteBytes(std::span<const std::byte> bytes) const;
[[nodiscard]] Task<bool> WriteBytesAsync(std::vector<std::byte> bytes) const;

[[nodiscard]] bool WriteString(std::string_view value) const;
[[nodiscard]] Task<bool> WriteStringAsync(std::string value) const;

[[nodiscard]] bool AppendBytes(std::span<const std::byte> bytes) const;
[[nodiscard]] Task<bool> AppendBytesAsync(std::vector<std::byte> bytes) const;

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

The initial API does not add write options, implicit parent creation, or atomic replacement.
Code creates parents explicitly with `CreateDirectories()`.
A future atomic-write operation should be named explicitly instead of adding an ambiguous option to every write.

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
A later directory-copy operation should be named separately so a seemingly small call cannot hide an unbounded traversal.

`MoveTo()` uses the local filesystem's native move operation and returns `false` when it cannot complete that operation.
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
HuxerUI does not provide a process-wide working-directory mutation because it would affect other Runtime instances, modules, and threads.
Application storage must use the semantic application directories rather than depend on a launcher's working directory.

The application directories are created and validated before the service is published.
Platform shells determine their application identity from native bundle, package, or executable metadata rather than adding another identifier to the static `Application` declaration.

The initial API does not expose a cross-platform Documents directory.
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

  [[nodiscard]] Task<FileResult<std::vector<std::byte>>> ReadBytesAsync() const;
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
Neither operation exposes a native path or silently grants broader access.

Each value retains shared private platform state.
Copying a value retains the grant, and destruction of the last copy releases process-scoped resources such as Apple security-scope access, Android provider state, or a browser handle.
The initial API does not serialize references or promise that a grant remains valid after application restart.

## File picker

The picker uses compact cross-platform filters and keeps native presentation and permission handling in the platform adapter:

```cpp
struct FilePickerFilter {
  std::string name;
  std::vector<std::string> extensions;
  std::vector<std::string> content_types;
};

struct FilePickerOptions {
  std::vector<FilePickerFilter> filters;
};

struct SaveFileOptions {
  std::string suggested_name;
  std::vector<FilePickerFilter> filters;
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

  [[nodiscard]] Task<std::optional<FileReference>> OpenFileAsync(FilePickerOptions options = {}) const;
  [[nodiscard]] Task<std::vector<FileReference>> OpenFilesAsync(FilePickerOptions options = {}) const;
  [[nodiscard]] Task<bool> SaveFileAsync(File source, SaveFileOptions options = {}) const;
};
```

Runtime installs one `FilePicker` Root Service.
Separating `OpenFileAsync()` and `OpenFilesAsync()` keeps result cardinality visible in the type and avoids an `allow_multiple` option whose result would still need another shape.

Extensions omit the leading dot and content types use MIME strings.
Filters are advisory because native pickers differ in their filtering support; application code still validates selected content.
Malformed filters or suggested names throw `std::invalid_argument` before native presentation.

User cancellation is an ordinary outcome, not an error.
Single selection returns `std::nullopt`, multiple selection returns an empty vector, and saving returns `false` when the user cancels.
An unsupported or failed platform operation produces the same empty or false outcome in the initial compact API; capability predicates let applications avoid presenting unavailable actions.

`SaveFileAsync()` streams from a local `File` to the destination selected by the native picker.
The native interface owns overwrite confirmation, so the common operation does not add another overwrite argument.
Saving bytes or strings directly is outside the initial surface; code may write an application temporary file and save that file explicitly.

Only one picker presentation may be active for a Runtime.
Concurrent requests are serialized in call order.
Task cancellation attempts to dismiss the native picker when supported and otherwise detaches the continuation; Runtime destruction cancels queued requests and releases their retained state.

## Application activation boundary

Open With, share intents, document URL contexts, and equivalent passive file-open requests are application activations rather than file-picker results or View events.
They may arrive before a Runtime exists and may require the application to create a document window, reuse an existing window, select a non-default root, or reject the request.
Delivering them to the currently committed Root View would assign application and window policy to an arbitrary UI tree.

The file API therefore defines no `FileEvents`, root callback, cold-start queue, or Runtime dispatch operation.
`FileReference` remains suitable as the file capability carried by a future general application-activation value, but this design does not define when or where that value is delivered.

A later application activation and window-session design owns the complete route from native activation to an application-selected Runtime:

```text
Native application activation
    -> application activation policy
    -> create or select a window session
    -> construct the target Runtime with its launch context
```

Native application metadata remains owned by the shell: Android intent filters, Apple document types and URL declarations, Windows associations, Linux desktop MIME declarations, and Web application handlers are not moved into `AppOptions`, CMake, or the file API.

## Runtime and local ownership

`FileSystem` is a built-in Runtime capability like `HttpClient`, not a PlatformModule and not a user-installed RootHook service.
It owns immutable application-directory values and registers those roots with process-local recursive-deletion safeguards.

Each `File` stores only a normalized absolute UTF-8 local path and remains usable after its originating `FileSystem` handle or component has gone out of scope.
Local operations are private implementation functions rather than a polymorphic provider interface.

The initial public `FileSystem` is final.
The initial API does not add filesystem registration, mount names, URL schemes, or a public `ZipFileSystem` subclass merely to reserve that possibility.
An archive or virtual filesystem requires a separate deliberate public design rather than an unused local-file abstraction.

## Synchronous and asynchronous execution

Lexical operations and the three convenience status predicates are synchronous only.
Operations that may transfer data, enumerate directories, or mutate storage provide an explicitly named `Async` counterpart.

Native asynchronous work uses a bounded shared filesystem executor rather than creating one thread per operation.
A future platform with a genuine asynchronous local-storage API may integrate that capability explicitly instead of inheriting an unused provider contract.

An asynchronous call validates caller-owned values before returning its lazy Task.
Once awaited from a launched HuxerUI Task, it performs filesystem work away from the UI thread and resumes through the owning `TaskExecution` and `UIThreadDispatcher`.
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
Internal Windows code may use `std::filesystem::path` constructed from a native wide path, but that type is not the public HuxerUI file identity.

Windows input accepts `/` and `\` as separators.
Normalized public output uses `/` while preserving the root and filename text exactly except for lexical normalization.
POSIX implementations continue to treat `\` as an ordinary filename character.

Paths preserve case and do not call canonicalization at construction.
Filesystem-specific case folding, symbolic-link resolution, and permission checks occur only when an operation reaches the filesystem.

## Platform mapping

macOS maps durable data to Application Support, cache data to Caches, temporary data to the native temporary directory, and the executable location to the application executable directory.
It presents `NSOpenPanel` and `NSSavePanel`, and retains security-scoped URL access inside `FileReference` when required.

iOS independently maps durable data to Application Support, cache data to Caches, and temporary data to the application temporary directory.
Its executable directory is read-only and is never used as a replacement for packaged HuxerUI resources.
It uses the document picker for active selection.

Android obtains the durable and cache roots from the application Context, creates an application-owned temporary child under the cache root, and exposes the native-library directory as a read-only executable location.
These private locations require no broad storage permission.
It uses the Storage Access Framework for active selection.

Windows uses the application's Local App Data identity for durable data, application-specific cache and temporary children, and the directory containing the process executable.
It uses the native file dialogs for active selection.

Linux follows XDG data and cache locations, prefers an application child of `XDG_RUNTIME_DIR` for temporary files with a safe temporary fallback, and resolves the executable directory independently of the process working directory.
It prefers the desktop portal for active selection.

Web requires a separate staged design because durable browser storage is asynchronous and has no executable directory.
The common header and result semantics remain portable, but the implementation must not claim durable success for a synchronous in-memory write before browser persistence completes.
The picker uses browser file handles when available and an input-element fallback for opening; unsupported save capabilities remain visible through the capability contract.

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

Picker and reference tests cover single and multiple selection, cancellation, unsupported capabilities, filter validation, serialized presentation, grant retention, revoked access, import and replacement, Runtime teardown, and UI-thread resumption.

Each platform phase verifies its application-directory mapping, Unicode conversion, protected roots, native picker mapping, native failure mapping, asynchronous execution, and grant cleanup without claiming unavailable implementations.

## Delivery sequence

- Land this design and keep all capabilities marked proposed.
- Add the public values, lexical behavior, focused shared tests, umbrella/header checks, and documentation without claiming unsupported platforms.
- Implement and exercise macOS, iOS, and Android application-directory discovery and local I/O through their native shells.
- Add Windows and Linux directory discovery and Unicode/path integration through their platform adapters.
- Design Web persistence separately against the same public contract before claiming browser support.
- Add `FileReference`, `FilePicker`, and fake-service tests after the local-file contract is stable.
- Implement picker adapters one platform at a time without expanding the first phase into application activation, directory selection, persistent grants, drag-and-drop, or clipboard APIs.
