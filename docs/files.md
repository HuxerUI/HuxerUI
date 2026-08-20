# Files and Application Storage

HuxerUI represents local paths with `File` and publishes application-owned directories through the Runtime-installed `FileSystem` Root Service.
The first implementation supports macOS; the same public API and other platform directory mappings are staged work.

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

UI-owned work should use explicitly named asynchronous methods.
They execute on a bounded filesystem executor and resume the owning HuxerUI Task on its UI thread:

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
An operating-system operation that cannot be interrupted may still finish in the background.

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
