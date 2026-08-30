# Resources, Files, Network, and Async Work

## Resources

Generated applications use this resource root:

```text
resources/
  images/
    logo.png
    logo@2x.png
    mark.svg
  raw/
    config.json
  strings/
    default.properties
    zh.properties
```

The CLI initially creates empty `images` and `raw` directories and writes `strings/default.properties`. Do not add another namespace directory below `resources`; the generated application CMake registers `RESOURCES resources` with `RESOURCE_NAMESPACE app`. The build generates typed identifiers in `<app_resources.h>` and packages framework and application resources together.

- `StringResource` and `StringVariant` support localized strings and formatted values.
- `ImageResource`, `ImageVariant`, and `ImageAsset` separate declarative identity from resolved image data.
- `RawResource` addresses packaged raw data.
- `ResourceConfiguration` and locale/DPI changes are platform-owned; use public resource APIs rather than constructing package paths.

Use generated identifiers such as `app::images::logo`, `app::raw::config_json`, and `app::strings::welcome`. Raw identifiers include the sanitized filename extension, while raster density variants such as `logo.png` and `logo@2x.png` share one image identifier. SVG files become platform-neutral vector resources. `default.properties` supplies the fallback strings, while locale files such as `zh.properties` provide matching overrides.

Generated libraries use the same directory categories but register their own target-derived resource namespace and generated header. Read the library's generated `CMakeLists.txt` rather than assuming the application namespace.

Pass resource values directly to components and `VisualFill` when their public overloads accept them. Use `UseString(...)`, `UseImage(...)`, `UseVectorImage(...)`, or `UseRawResource(...)` only when application code needs the resolved value itself. These reads are composition-bound and therefore belong in a composable function.

Keep resource identifiers and namespace consistent with generated CMake. Do not open `resources.bin` directly.

## Files and directories

Obtain the runtime-installed services during composition with `UseService<FileSystem>()`, `UseService<FilePicker>()`, and `UseService<HttpClient>()`. These return shared service handles; their public constructors are not application construction APIs. Store the required handle before launching a lifetime-bound task, then capture that handle into the task rather than looking up a composition service after suspension.

`Bytes` from `<huxerui/data.h>` is the canonical owned binary buffer. Use `std::span<const std::byte>` only for borrowed binary input instead of introducing another application byte-container type.

`Uri` from `<huxerui/data.h>` is the immutable absolute RFC 3986 URI value. Use the throwing constructor for trusted caller input and `Uri::Parse()` for external text. Read `Scheme()` and `Path()` directly; `Authority()`, `Query()`, and `Fragment()` are optional so absent and present-empty components stay distinct. Serialization and equality are lexical, raw Unicode IRIs are unsupported, and `Uri` does not own route, query-map, normalization, or HTTP policy.

`File` represents an application-visible path and offers synchronous and asynchronous stat, read, write, append, list, create, delete, copy, and move operations. Byte reads return `FileResult<Bytes>`; synchronous byte writes borrow a span for the call, while asynchronous byte writes take `Bytes` by value so storage remains owned across suspension. Handle `FileResult<T>` and `FileErrorCode`; do not assume every platform permits every path.

Use `File(const Uri&)` and `File::ToUri()` only for supported local `file:` URI conversion. Preserve `FileReference` as the platform-granted capability for Android content URIs, Apple security-scoped URLs, browser handles, and other external files; do not project it into a local path or generic URI.

`FileSystem::Directories()` provides application data, cache, temporary, and optional executable directories. Use those instead of hardcoded OS paths.

Destructive file operations require the user's intended scope. Prefer non-recursive operations unless recursive deletion is explicitly needed and the exact target is validated.

## File picker

`OpenFileAsync` and `OpenFilesAsync` return `FileReference` values rather than unrestricted paths. Use their read/import/replace operations and preserve the capability boundary on sandboxed and Web platforms. `SaveFileAsync` instead receives an application `File` plus `SaveFileOptions` and reports success as `bool`. Check `CanOpenFiles` and `CanSaveFiles` before presenting unavailable actions.

## HTTP

Create requests with `HttpRequest`, `HttpMethod`, and headers during the owning application flow. `HttpRequest::body` and `HttpResponse::body` are owned `Bytes`; HuxerUI does not implicitly encode request text or decode response bytes. Inside the task, call `Send(...)` on the previously captured `HttpClient` handle. Inspect `HttpResult` before accessing response/error. Treat status codes, transport errors, cancellation, body size, encoding, and user-visible retry policy separately.

## Task lifetime

Launch app-side async flows from `UseTaskScope()` so unmount cancels work. Return to owner state only while the scope is alive. Avoid detached platform callbacks that capture UI objects indefinitely.

Call File asynchronous methods and `HttpClient::Send()` directly from the owning Task. File Async already owns its platform-appropriate worker or event completion path, while HTTP keeps its platform asynchronous transport; do not wrap either API in `RunWorker()`.

Use `RunWorker()` only for application-owned synchronous CPU-bound or blocking work. Use `TaskScope::Post()` to hand an external callback back to the UI scope. These APIs do not keep a mobile application running in the background; platform background tasks are a separate capability.

The active SDK's services are installed by the application runtime. If a service is unavailable on a platform, report that public capability limit rather than reaching into a private adapter.
