# HTTP Client Design

The platform-neutral API, shared operation state, Task integration, and Windows, macOS, iOS, Linux, Android, and Web transports are implemented.

## Ownership

HTTP is a built-in Runtime capability rather than a PlatformModule.
Runtime installs one `HttpClient` root service backed by the adapter's private `HttpTransport`.
The shared layer owns request validation, result types, stream state, progress semantics, buffering for `Send()`, Task resumption, and cancellation races.
Platform transports own URL loading, TLS, proxies, redirects, native decoding, headers, body delivery, and native cancellation.

There is one operation state for both public request forms.
`SendStream()` exposes that state after final headers, while `Send()` opens the same state and repeatedly reads it into `HttpResponse::body`.
There is no buffered transport completion path beside the streaming path and no second registry, callback model, or data channel.

## Public contract

`HttpClient::Send()` returns `Task<HttpResult>` and retains the complete final response body.
`HttpClient::SendStream()` returns `Task<HttpStreamResult>` after final headers and before body EOF.
Both accept `std::function<void(HttpProgress)>`.

`HttpResponseStream` is move-only and exposes immutable final URL, status, and headers plus pull-based `Read()`.
Only one read may be outstanding.
A data result contains 1 byte through 64 KiB, completion is an explicit alternative, and a post-header transport failure is an error alternative.
The shared state splits larger platform deliveries without requesting another native read until the buffered remainder has been consumed.

HTTP status codes, including 4xx and 5xx, remain responses.
Errors before final headers use `HttpStreamResult::Error()`; errors after headers use `HttpStreamReadResult::Error()`.
`Send()` maps either transport phase to `HttpResult::Error()` because its buffered result is not published until EOF.

Request validation happens synchronously before the lazy Task is returned.
URLs must be absolute HTTP or HTTPS URLs, GET and HEAD reject nonempty bodies, header syntax is validated, and a specified timeout must be positive.

## Progress

`HttpProgress::transferred_bytes` is monotonic for each direction.
Upload progress is the logical request body accepted or handed to the platform, not server acknowledgement.
The shared state clamps redirect replays and native over-reporting to the request body size.
An early server response is allowed before the logical upload reaches its total.

Download progress advances immediately before a chunk is returned to `Read()`.
The buffered `Send()` path uses those same reads, so its progress counts the exact bytes appended to `HttpResponse::body`.
The bytes are post-platform-delivery bytes: a backend that performs automatic content decoding reports the decoded bytes, while a backend that exposes encoded bytes reports encoded bytes.

`total_bytes` is optional.
A transport supplies it only when native metadata reliably describes the representation delivered to the shared state.
Content length is therefore omitted for chunked responses and when automatic content decoding makes the encoded content length unreliable.
The shared state clears a native total if delivered bytes exceed it.

Transport callbacks may arrive on any thread.
The shared state coalesces pending upload observations and resumes through the Task execution's `UIThreadDispatcher`.
Application progress callbacks and code after every HTTP `co_await` run on the owning Runtime UI thread.
If a progress callback throws, the current Task rethrows that exception and the operation is canceled.

## Shared operation state

`src/http_internal.h` defines the only platform boundary:

```cpp
struct HttpTransportResponse {
  std::string url;
  int status_code;
  std::vector<HttpHeader> headers;
  std::optional<std::uint64_t> body_size;
};

struct HttpTransportCallbacks {
  std::function<void(std::uint64_t)> upload_progress;
  std::function<void(HttpTransportResponse)> response;
  std::function<void(Bytes)> body;
  std::function<void()> complete;
  std::function<void(HttpError)> error;
};

class HttpTransportOperation {
public:
  virtual void RequestRead() = 0;
  virtual void Cancel() noexcept = 0;
};

class HttpTransport {
public:
  virtual std::shared_ptr<HttpTransportOperation> Start(
      HttpRequest request,
      bool require_incremental_response,
      HttpTransportCallbacks callbacks
  ) = 0;
};
```

The callback order is final-response oriented:

- Zero or more upload observations may precede the final response.
- Exactly one final response or pre-header error is published.
- Body callbacks are demand-driven after `RequestRead()`.
- Exactly one completion or post-header error terminates body delivery.
- Intermediate redirect responses and bodies are not published.

The shared state is mutex-protected but never invokes a platform operation, application callback, or coroutine continuation while holding its lock.
Transport callbacks retain only a weak state reference.
The state keeps the transport and operation alive through EOF, timeout, error, or cancellation.
Body data or completion received before final response metadata terminates the operation with a transport error.
Late and duplicate callbacks are ignored.

Calling `Read()` reserves the single read slot synchronously before returning its lazy Task.
Destroying an unstarted or suspended read Task cancels the complete stream.
Consuming EOF or a read error closes the read contract; subsequent calls throw `std::logic_error`.
Destroying an unfinished `HttpResponseStream` reaches the same idempotent platform cancellation path.

The optional HuxerUI deadline covers request start through stream EOF, including redirects and idle time between reads.
A timeout received after headers is retained until the next `Read()`.
Task cancellation remains distinct from `HttpError`: it detaches the continuation and does not resume application code.

## Platform transports

Windows uses one asynchronous WinHTTP session per Runtime.
It publishes final metadata from `HEADERS_AVAILABLE`, performs one `WinHttpReadData` per requested native read, and keeps callback context alive until `HANDLE_CLOSING`.
Its thread-pool deadline covers the whole stream.
WinHTTP handles redirects and optional gzip or deflate decoding, and download totals are omitted when decoding makes content length unreliable.

Linux uses one libsoup 3 session on its private GLib network thread.
It opens responses with `soup_session_send_async` and performs one `GInputStream` read per demand.
The GLib deadline and `GCancellable` remain active while a stream is idle.
The distribution-provided libsoup and GLib dependencies remain system packages.

Android uses `HttpURLConnection` on a bounded Java executor.
Connection setup and each requested body read occupy workers only while work is active; an idle stream does not reserve a worker thread.
The in-memory request body uses Java's buffered upload path so automatic redirects and authentication are not disabled by fixed-length streaming mode.
JNI publishes headers, body chunks, progress, and one terminal event through the same native operation handle.

Web uses Fetch and `ReadableStreamDefaultReader` when available.
`SendStream()` returns `Unsupported` before headers when the browser cannot expose a readable body stream.
The internal buffered `Send()` path may fall back to `Response.arrayBuffer()` on such a browser without creating another public result path.
`AbortController` and a separate timer remain active through EOF.
Browser CORS, forbidden-header, credential, redirect, and visible-response-header rules are preserved.

iOS and macOS each own a Foundation transport in their platform directory.
Each operation uses an ephemeral `NSURLSession` data delegate, suspends its data task after headers and after each delivered body chunk, and resumes it for one requested read.
A dispatch deadline remains active while the native task is suspended.
The configuration disables persistent cookies and URL caching without weakening App Transport Security or sandbox policy.

No transport uses `RunWorker` as a general networking scheduler.
Windows, iOS, macOS, Web, and libsoup use their native asynchronous facilities; Android retains its bounded Java network executor.
These choices do not extend platform lifecycle or grant background execution.
Android and iOS applications need explicit background-task or background-transfer capabilities when work must continue through application suspension.
On macOS, ordinary requests follow the application's process lifetime and system scheduling rather than the mobile suspension contract.

## Redirects and content decoding

The public API intentionally has no redirect policy object.
Each platform follows redirects permitted by its networking stack and exposes only the final URL, status, headers, and body.
The request timeout includes redirect handling, and upload progress remains a logical-body count even when a platform replays the body.
Platform redirect limits, cross-scheme restrictions, and browser security rules remain authoritative.

Content decoding also remains platform-managed.
The shared layer does not implement gzip, deflate, Brotli, or another parallel decoder.
This preserves the platform's header and byte-delivery contract and prevents a second body pipeline.

## Deliberate limits

Request bodies remain owned in-memory `Bytes`; streaming and resumable uploads are not part of this contract.
`Send()` remains intended for ordinary API responses that fit in memory, while `SendStream()` allows bounded consumption without implying a file destination.
Implicit file transfers, background transfer, retry policy, interceptors, WebSocket, certificate pinning, a framework cookie jar, and persistent HTTP caching remain separate capabilities.

## Validation

Shared Runtime tests verify synchronous validation, binary preservation, headers-first completion, demand reads, 64 KiB splitting, pre- and post-header errors, logical progress, UI-thread callbacks, cancellation, late events, and unsupported adapters.
Windows loopback tests verify WinHTTP request conversion, pull-based body reads, reliable length metadata, deadlines, cancellation, and invalid UTF-8 handling.
Linux loopback tests verify binary bodies, repeated headers, redirects, HTTP statuses, transport failures, deadlines, cancellation races, and transport shutdown.
Android and Web builds validate their language boundary and generated platform artifact; Apple behavior requires macOS or iOS validation because Objective-C++ and Foundation cannot be built on Windows.
