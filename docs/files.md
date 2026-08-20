# Files and Application Storage

HuxerUI represents local paths with `File` and publishes application-owned directories through the Runtime-installed `FileSystem` Root Service.
The current application-directory implementation supports Windows, macOS, Linux, iOS, Android, and Web.
The shared `FileReference` and `FilePicker` contract and its Windows, macOS, iOS, Android, and Web platform adapters are available; the Linux picker adapter remains staged work.

## Application directories

Obtain the service inside a component with `UseService<FileSystem>()`:

```cpp
auto files = UseService<FileSystem>();
const AppDirectories& directories = files->Directories();

File settings(directories.data_directory, "settings.json");
File thumbnail(directories.cache_directory, "thumbnail.png");
File export_file(directories.temporary_directory, "export.bin");
```

`data_directory` stores durable application-private data.
`cache_directory` stores reconstructible data that the operating system may remove.
`temporary_directory` stores short-lived data and does not promise persistence across launches.
`executable_directory` is optional because not every platform exposes a useful local executable location.

`CurrentDirectory()` reports the process working directory and is not an application storage location.
Application data should not depend on the directory from which a launcher happened to start the process.

On Linux, the executable filename identifies the application directory.
Data uses `$XDG_DATA_HOME/<executable-name>` or `$HOME/.local/share/<executable-name>`, and cache uses `$XDG_CACHE_HOME/<executable-name>` or `$HOME/.cache/<executable-name>`.
Temporary data uses a private `$XDG_RUNTIME_DIR/<executable-name>` when available, otherwise HuxerUI creates `<system-temporary-directory>/huxerui-<uid>/<executable-name>` with owner-only access.
Renaming the executable selects different application storage, while independently installed executables with the same filename share these per-user locations.
`executable_directory` is resolved from `/proc/self/exe` rather than the process working directory.

On Windows, the executable filename identifies an application root under the current user's Local App Data directory.
The `data`, `cache`, and `temporary` children remain distinct protected roots, and `executable_directory` is resolved from the running process rather than the working directory.

## Paths

`File` constructors accept UTF-8 and immediately resolve relative input against the current working directory.
The value may identify a path that does not exist yet.

```cpp
File root("/tmp/example");
File report(root, "report.txt");
File nested = root.Resolve("exports/2026/report.txt");

std::string name = nested.Name();
std::optional<File> parent = nested.Parent();
```

`Child()` accepts one name and rejects empty names, `.`, `..`, absolute paths, and platform separators.
`Resolve()` accepts a multi-segment relative path and performs lexical normalization without resolving symbolic links.
`Path()` returns a normalized absolute UTF-8 path.

## Reading and writing

Synchronous methods are useful for startup work, command-line tools, background threads, tests, and intentionally small operations:

```cpp
File settings(files->Directories().data_directory, "settings.json");

if (!settings.WriteString("{\"theme\":\"dark\"}")) {
  // The write did not reach its requested final state.
}

FileResult<std::string> content = settings.ReadString();
if (content.Succeeded()) {
  std::string text = std::move(content).Value();
} else {
  FileError error = content.Error();
}
```

On Web, synchronous reads and temporary-directory mutations use the restored virtual filesystem normally.
Mutations under `data_directory` or `cache_directory` must use their `Async` variants because success is reported only after browser persistence completes.

UI-owned work should use explicitly named asynchronous methods.
Non-Web platforms execute them on a bounded filesystem executor, while Web serializes them through the browser event loop and waits for persistent-storage completion when required.
Every platform resumes the owning HuxerUI Task on its UI thread:

```cpp
[[huxerui::scope]]
View SettingsStatus() {
  auto files = UseService<FileSystem>();
  auto tasks = UseTaskScope();
  auto status = UseState(std::string{"Not saved"});
  File settings(files->Directories().data_directory, "settings.json");

  return Column {
    Text(status),
    Button("Save").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        status = co_await settings.WriteStringAsync("{\"theme\":\"dark\"}") ? "Saved" : "Save failed";
      });
    }),
  };
}
```

Canceling the owning `TaskHandle`, retiring its `TaskScope`, or destroying the Runtime prevents a late filesystem completion from resuming application code.
An underlying storage operation that cannot be interrupted may still finish after cancellation.

## External files and pickers

`FileReference` retains platform-granted access to one external file without exposing a platform path or pretending that the file belongs to the application sandbox.
It is copyable, provides display metadata, and supports asynchronous reads, import into a local `File`, and write-back when `CanWrite()` is true.

Runtime always installs one `FilePicker` Root Service.
Applications inspect its capabilities because platform picker adapters are introduced independently:

```cpp
auto picker = UseService<FilePicker>();
auto tasks = UseTaskScope();

if (picker->CanOpenFiles()) {
  tasks.Launch([picker]() -> Task<void> {
    std::optional<FileReference> selected = co_await picker->OpenFileAsync({
        .name = "Text",
        .extensions = {"txt"},
        .content_types = {"text/plain"},
    });
    if (selected) {
      FileResult<std::string> text = co_await selected->ReadStringAsync();
    }
  });
}
```

Extensions omit the leading dot, and content types use exact MIME strings, `type/*`, or `*/*`.
Every extension and content type in the filter is accepted as part of one union; an empty filter permits all files.
Malformed filters and suggested filenames throw `std::invalid_argument` before opening platform UI.
User cancellation and unavailable platform capability produce `std::nullopt`, an empty vector, or `false` according to the requested operation.
Only one picker is presented per Runtime; concurrent calls wait in request order, and Task cancellation detaches application continuation while asking the platform to dismiss an active picker when possible.

On Android, `HuxerUIActivity` supplies the Storage Access Framework launcher automatically.
An application embedding `HuxerUIView` in its own Activity installs a `HuxerUIView.FilePickerLauncher` and forwards the corresponding Activity result through `dispatchFilePickerResult()`; until it does so, the picker capability predicates return `false`.
Selected `content://` values remain inside `FileReference`, require no broad storage permission, and are not persisted across application launches by this initial API.

On Windows, opening and saving use the system file dialogs owned by the HuxerUI window.
Selected filesystem paths remain private to `FileReference`; reads, imports, replacements, and save copies run on the bounded platform file executor instead of blocking the UI thread.
Extensions and MIME types are translated into advisory system filters, while unsupported MIME mappings widen the filter rather than hiding valid files.

On Web, opening prefers the File System Access API and falls back to a transient `<input type="file">` where that API is unavailable.
Handle-backed references can report write support, while input-backed references remain read-only; both can be read or imported into application storage.
`CanSaveFiles()` is true only when the browser provides `showSaveFilePicker()` and writable file handles because an ordinary download cannot report the shared save result reliably.
Picker calls must begin directly in a click or equivalent user event before another `co_await` consumes the browser's transient user activation.
Web grants remain session scoped and are not persisted to IndexedDB.

## Web storage identity

A Web shell supplies one stable application storage key when creating the Emscripten module:

```js
const module = await createHuxerUIApp({
  huxeruiStorageKey: "com.example.app",
});
```

Generated CLI applications use their project identifier, and repository examples use their bundle identifier.
Changing the key selects another isolated browser database, so custom shells keep it stable across deployments.
Data and cache use IndexedDB-backed IDBFS, temporary files use volatile MEMFS, and `executable_directory` is absent.
Browser quota, eviction, private-browsing policy, and user storage controls remain authoritative.

## Results and mutation outcomes

Reads, metadata, and directory enumeration return `FileResult<T>` because an empty string, empty byte vector, or empty directory is a successful value rather than an error.
Inspect `Succeeded()` before accessing `Value()` or `Error()`.

Mutating methods return `bool` or `Task<bool>`.
They report whether the operation reached its documented final state and intentionally do not add a second result type for operations such as writing, deleting, copying, and moving.

`Exists()`, `IsFile()`, and `IsDirectory()` return `false` both when the target is absent and when its status cannot be determined.
Use `Stat()` or `StatAsync()` when the distinction matters.

## Text and bytes

`ReadBytes()` and `WriteBytes()` preserve bytes exactly.
`ReadString()` requires UTF-8, removes one leading UTF-8 byte-order mark, and does not convert line endings.
String writes validate UTF-8, emit no byte-order mark, and preserve line endings.

The complete-file read methods retain their result in memory and report `FileErrorCode::TooLarge` when the implementation cannot represent the file safely.
Streaming and random-access handles are not part of the current API.

## Directory and deletion safety

`CreateDirectory()` creates one directory and requires its parent to exist.
`CreateDirectories()` also creates missing ancestors.
`Delete()` removes a file, symbolic link, or empty directory and succeeds when the target is already absent.

`DeleteRecursively()` is deliberately explicit.
It does not follow a symbolic link into another tree and refuses to delete filesystem roots or Runtime application-directory roots.

Packaged resources remain separate from local files.
Use generated resource identifiers and `AppResources` for packaged data because a resource is not guaranteed to have a stable operating-system path.

See [File and Application Storage Design](design/files.md) for the complete contract and staged platform plan.
