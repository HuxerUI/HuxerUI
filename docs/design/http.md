# HTTP Client Design

The platform-neutral API, Runtime service, Task integration, deterministic transport tests, and independent Windows, macOS, iOS, Linux, Android, and Web backends are implemented.

## Goals

HuxerUI provides one small HTTP API that composes with Task and retains each platform's networking stack.
The shared layer owns portable request values, response values, validation, error categories, Task resumption, and cancellation races.
Platform adapters own URL loading, TLS, proxy integration, connection reuse, redirects, timeout enforcement, and conversion to owned C++ response values.

HTTP is a built-in Runtime capability rather than a PlatformModule.
It does not introduce a string registry, PlatformPayload encoding, a second asynchronous result model, or an HTTP-specific cancellation handle.

## Public API

The public declarations live in `<huxerui/http.h>` and are re-exported from `<huxerui/huxerui.h>`:

```cpp
enum class HttpMethod {
  Get,
  Head,
  Post,
  Put,
  Patch,
  Delete,
  Options,
};

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpRequest {
  std::string url;
  HttpMethod method = HttpMethod::Get;
  std::vector<HttpHeader> headers;
  std::string body;
  std::optional<std::chrono::milliseconds> timeout =
      std::chrono::milliseconds{30000};
};

struct HttpResponse {
  std::string url;
  int status_code = 0;
  std::vector<HttpHeader> headers;
  std::string body;
};

enum class HttpErrorCode {
  Transport,
  Timeout,
  Unsupported,
};

struct HttpError {
  HttpErrorCode code;
  std::string message;
};

class [[nodiscard]] HttpResult final {
public:
  explicit HttpResult(HttpResponse response);
  explicit HttpResult(HttpError error);

  [[nodiscard]] bool HasResponse() const noexcept;

  [[nodiscard]] HttpResponse& Response() &;
  [[nodiscard]] const HttpResponse& Response() const&;
  [[nodiscard]] HttpResponse&& Response() &&;

  [[nodiscard]] HttpError& Error() &;
  [[nodiscard]] const HttpError& Error() const&;
};

class HttpClient final {
public:
  [[nodiscard]] Task<HttpResult> Send(HttpRequest request) const;
};
```

Request and response bodies are binary-safe byte strings.
HttpClient does not infer an encoding, parse JSON, construct form bodies, or decode application payloads.
Header entries remain a sequence because repeated field names are meaningful, although a platform may combine fields when its response API no longer exposes individual lines.

URLs must be absolute HTTP or HTTPS URLs.
GET and HEAD requests reject non-empty bodies.
Header names use the HTTP token character set, and header names and values reject null, carriage-return, and line-feed characters.
A specified timeout must be positive and covers the request until the complete in-memory response is available.
Omitting it disables the HuxerUI deadline but does not disable failures imposed by the operating system or network stack.

## Usage

Runtime installs one HttpClient Root Service before application RootHooks run.
Components retrieve it through the existing typed service function and launch requests through their existing TaskScope:

```cpp
[[huxerui::composable]]
View RepositoryPage() {
  auto http = UseService<HttpClient>();
  auto tasks = UseTaskScope();
  auto result = UseState(std::string{"Not loaded"});

  return Button("Load").OnClick([=] {
    tasks.Launch([=]() -> Task<void> {
      HttpResult request = co_await http->Send({
          .url = "https://api.example.com/repository",
          .headers = {{"Accept", "application/json"}},
      });
      if (request.HasResponse()) {
        HttpResponse response = std::move(request).Response();
        result = response.body;
      } else {
        result = request.Error().message;
      }
    });
  });
}
```

HTTP status codes, including 4xx and 5xx, produce an HttpResult containing HttpResponse.
Transport failures, timeout, and an unavailable platform transport produce an HttpResult containing HttpError.
HttpResult deliberately exposes `HasResponse()` instead of a Boolean conversion because an HTTP error status is still a valid response.
Invalid methods, URLs, headers, bodies, and timeout values remain caller errors and throw `std::invalid_argument` synchronously from `Send()` before a Task is launched.
The HTTP result model does not add a generic Result, AsyncResult, or second Task error channel.

## Ownership and cancellation

HttpClient is a per-Runtime Root Service and owns a shared private HttpTransport.
The transport may retain a process or adapter-level session so requests reuse transport connection pools.
Every Send call copies that shared transport into the lazy Task before returning, so the coroutine does not retain a raw HttpClient pointer.

The HTTP awaiter binds its continuation to the current Task execution.
A platform completion may arrive on any thread, but it resumes the coroutine through the execution's UIThreadDispatcher.
Code after co_await therefore runs on the TaskScope's owning UI thread and may update State directly.

Destroying the awaiter marks the request canceled, detaches its continuation, and invokes the platform cancellation operation.
TaskHandle cancellation, RecomposeScope retirement, virtual-item eviction, and Runtime teardown all reach that same path through TaskScope ownership.
Cancellation does not throw and does not resume application code.
The private request state accepts at most one result, and a completion arriving after cancellation or timeout is ignored safely.

## Platform boundary

PlatformAdapter has one optional protected capability:

```cpp
virtual std::shared_ptr<detail::HttpTransport> CreateHttpTransport();
```

The base implementation returns no transport so existing custom adapters remain source-compatible.
Runtime still provides HttpClient; awaiting Send on an adapter without a transport returns HttpError with `HttpErrorCode::Unsupported`.

The focused private contract lives in `src/http_internal.h`:

```cpp
class HttpTransport {
public:
  virtual std::function<void()> Start(
      HttpRequest request,
      HttpTransportCompletion completion
  ) = 0;
};
```

Start takes ownership of the complete request and returns an optional cancellation operation.
The completion receives one owned platform-neutral HttpResult and may run on any thread.
Transport callbacks never retain a coroutine handle, Runtime pointer, or awaiter directly.

macOS uses an ephemeral NSURLSession with URLSessionDataTask.
It disables persistent cookies and URL caching, keeps connection reuse inside the session, applies the request deadline, cancels the data task during Task cancellation, and converts the final HTTP response and body into owned C++ values.
iOS has an independent NSURLSession implementation in its platform directory with the same buffering, timeout, cancellation, and ownership contract.

The Android backend uses HttpURLConnection on a bounded worker executor, enforces the complete request deadline, and disconnects a canceled request.
The Web backend uses Fetch on the browser main thread, buffers the response through `arrayBuffer()`, and aborts canceled or timed-out requests through AbortController.
It preserves browser redirect, same-origin credential, CORS, forbidden-header, and response-header visibility rules rather than weakening them in generated glue.
The Windows backend uses one asynchronous WinHTTP session per Runtime and one request handle per Send call.
WinHTTP callbacks advance send, response, and buffered-read phases without occupying a framework worker thread.
Closing the request handle is the single cancellation path, and callback context remains alive until WinHTTP reports `HANDLE_CLOSING`.
A private thread-pool deadline covers the complete operation rather than restarting for each WinHTTP phase.
Windows 10 and later use the operating system automatic proxy configuration, while Windows 7 compatibility builds retain WinHTTP's default proxy mode.
The backend disables persistent cookies, requests transport-managed gzip and deflate decompression when the application has not supplied `Accept-Encoding`, and preserves repeated response headers when WinHTTP exposes them.
The Linux backend uses one libsoup 3 Session on a dedicated GLib network thread, preserving system proxy and trust-store behavior without entering the GTK UI context.
It buffers responses through the asynchronous send-and-read API, enforces the complete request deadline with a GLib timeout source, and cancels requests through GCancellable.
The distribution-provided libsoup 3 development package and its GLib/GIO dependencies are manually installed system dependencies rather than FetchContent inputs.
Adding another platform implements HttpTransport without changing HttpClient or Task.

Platform application projects retain authority over permissions and security policy.
HuxerUI does not add Android Internet permission from CMake, weaken Apple transport security or sandbox entitlements, bypass browser CORS, replace system trust stores, or inject application credentials.

## Deliberate limits

Send buffers the complete request and response bodies in memory and is intended for ordinary API requests.
The HTTP API does not include streaming uploads, streaming downloads, progress callbacks, resumable transfers, background transfer, WebSocket, retry policy, interceptors, certificate pinning, a framework Cookie Jar, or persistent HTTP caching.
Streaming and file-transfer APIs remain separate operations instead of changing the ownership of `HttpResponse::body`.

## Validation

Shared tests use a deterministic fake HttpTransport to verify request preservation, response delivery, HTTP error separation, parameter validation, transport failures, Task cancellation, late completion, unsupported adapters, and UI-thread resumption.
Each platform phase adds its implementation to the corresponding platform build and validates that platform without claiming unexecuted backends.
Windows transport tests use a deterministic loopback server to verify WinHTTP request and response conversion plus the complete-operation deadline.
Linux transport tests use a deterministic loopback server to verify binary bodies, repeated headers, redirects, errors, cancellation, deadlines, races, and shutdown.
`example_http` provides the Windows, macOS, iOS, Linux, Android, and Web end-to-end request demonstration and becomes available on another platform only after that platform transport is implemented.
