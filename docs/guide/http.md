# HTTP Client

HuxerUI provides `HttpClient` as a per-window root service backed by each platform's networking stack.
Requests use `Task`, and continuations and progress callbacks run on the owning Runtime UI thread.

## Buffered requests

`SendAsync()` is the compact API for an ordinary request whose complete response should be retained in memory:

```cpp
Bytes Utf8Bytes(std::string_view text) {
  const auto bytes = std::as_bytes(std::span<const char>(text.data(), text.size()));
  return Bytes(bytes.begin(), bytes.end());
}

Task<HttpResult<HttpResponse>> CreateItem(const std::shared_ptr<HttpClient>& http) {
  co_return co_await http->SendAsync({
      .url = "https://api.example.com/items",
      .method = HttpMethod::Post,
      .headers = {{"Content-Type", "application/json; charset=utf-8"}},
      .body = Utf8Bytes(R"({"name":"example"})"),
  });
}
```

`HttpResult<T>::Succeeded()` distinguishes a received HTTP response from a transport error.
Status codes such as 404 and 500 are valid responses and remain available through `Value()`.
Timeouts, transport failures, and unsupported adapters produce `HttpError`.

HTTP bodies use `Bytes` because their content is binary regardless of `Content-Type`.
The HTTP layer preserves empty bodies, embedded null bytes, and byte sequences that are not valid UTF-8.
It does not infer an encoding, parse JSON, or decode application response formats.

## Streaming responses

`SendStreamAsync()` returns after the final response headers are available and before the response body reaches EOF.
The response body is an `AsyncInputStream`, so the consumer chooses the maximum size of every read or copy buffer:

```cpp
Task<bool> DownloadArchive(const std::shared_ptr<HttpClient>& http, File destination) {
  HttpResult<HttpResponseStream> opened = co_await http->SendStreamAsync({
      .url = "https://api.example.com/archive",
  });
  if (!opened.Succeeded()) {
    ReportHttpError(opened.Error());
    co_return false;
  }

  auto output = co_await destination.OpenWriteAsync();
  if (!output.Succeeded()) {
    ReportIoError(output.Error());
    co_return false;
  }

  HttpResponseStream response = std::move(opened).Value();
  AsyncOutputStream file = std::move(output).Value();
  auto copied = co_await response.Body().CopyToAsync(file, 64 * 1024);
  if (!copied.Succeeded()) {
    ReportIoError(copied.Error());
    co_return false;
  }
  auto closed = co_await file.CloseAsync();
  if (!closed.Succeeded()) {
    ReportIoError(closed.Error());
    co_return false;
  }
  co_return true;
}
```

Only one `ReadAsync()` or `CopyToAsync()` operation may be pending on an input stream, and only one write or close may be pending on an output stream.
`ReadAsync(maximum_bytes)` returns between 1 and `maximum_bytes` owned bytes, with empty `Bytes` representing EOF.
Chunks are arbitrary binary boundaries and do not preserve text, JSON, multipart, or application-record boundaries.
Sequential reads preserve response byte order, including when the next read starts immediately after the preceding one completes.
Reading after EOF remains an empty success; reusing a failed stream, overlapping operations, or accessing a moved-from stream throws `std::logic_error`.

An error before final headers appears in `HttpResult<HttpResponseStream>`.
An error after headers appears as `IoError` in the body operation’s `IoResult`, so status and headers remain available as soon as the platform has received them.
Destroying an unfinished stream cancels it, and canceling a Task suspended in `ReadAsync()` or `CopyToAsync()` cancels the complete HTTP operation.

## Transfer progress

Both request forms accept an optional progress callback:

```cpp
HttpResult<HttpResponse> result = co_await http->SendAsync(
    {.url = "https://api.example.com/archive"},
    [](HttpProgress progress) {
      if (progress.kind == HttpProgressKind::Download) {
        UpdateDownloadedBytes(progress.transferred_bytes, progress.total_bytes);
      }
    });
```

Upload progress counts request bytes accepted by the platform transport, not bytes acknowledged by the server.
An early response such as 401 or 413 may arrive without a final upload callback.
Redirect replays never make the logical uploaded count exceed `HttpRequest::body.size()`.

Download progress counts the exact bytes returned by `ReadAsync()` or accumulated into `HttpResponse::body`.
Those bytes reflect any automatic decoding performed by the current platform transport.
`total_bytes` is present only when the platform exposes a reliable total for that same delivered representation.
If delivered bytes contradict a reported total, later progress omits the total instead of reporting an impossible fraction.
Progress is monotonic, but callback frequency is intentionally unspecified.
Throwing from a progress callback cancels the operation and rethrows from the current Task.

## Redirects, decoding, and lifetime

HuxerUI allows each platform networking stack to perform its normal automatic redirects.
Only the final response URL, status, headers, and body are exposed; intermediate redirect bodies do not contribute download progress.
Platform redirect limits and security rules still apply.

Compression remains platform-managed.
HuxerUI does not add a second gzip pipeline, and a download total is omitted when an encoded content length cannot describe the delivered bytes reliably.

The optional request timeout covers the operation from start through body EOF, including redirects and time spent between stream reads.
`std::nullopt` disables the HuxerUI deadline but cannot disable failures imposed by the operating system, browser, or network stack.

HTTP transports use their native asynchronous facilities or their existing private network executor.
They do not use `RunWorker`.
Ordinary Android and iOS requests do not acquire background-execution time; applications that must continue through suspension use an explicit platform background task or background transfer capability.
macOS requests follow the application's normal process lifetime and system scheduling rather than the mobile suspension contract.

URLs must be absolute HTTP or HTTPS URLs.
GET and HEAD reject nonempty bodies; header names and values reject invalid protocol characters; specified timeouts must be positive.
Invalid request configuration throws `std::invalid_argument` synchronously from `SendAsync()` or `SendStreamAsync()`.

Retries, resumable or streaming uploads, implicit file transfers, WebSocket, certificate pinning, and a framework-owned cookie jar remain outside this API.
See [HTTP Client Design](../design/http.md) for transport ownership and concurrency contracts.
