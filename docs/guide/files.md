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

## External files

`FileReference` is a capability for a file selected or opened outside application-owned storage.
It may represent a path, security-scoped Apple URL, Android document URI, or browser file handle without exposing that identity to shared code.

External references can be read, imported into application storage, or used for a supported replacement operation.
Their lifetime and persistence follow platform capability rules.

## FilePicker

Use the file picker service to open one or more files or save an application-owned file through the current platform.
Picker cancellation is a normal outcome and remains distinct from transport or permission failure.

Filters are portable hints.
The platform picker may present them according to platform conventions, but application code must still validate selected content.

## Web storage

The Web backend uses browser-managed storage for application directories and browser file capabilities for external selections.
The application identity determines its storage namespace.

Browser persistence is asynchronous and subject to browser policy, quota, and user clearing.
The file API does not pretend browser storage is an unrestricted host filesystem.

## Safety

Validate user-selected content before parsing it.
Keep recursive deletion within an application-owned directory or an explicitly granted capability.
Treat external references as authority to the selected item, not to an arbitrary neighboring path.

See [File and Application Storage Design](../design/files.md) and [URI and Local File URI Design](../design/uri.md) for exact conversion, result, cancellation, and platform contracts.
