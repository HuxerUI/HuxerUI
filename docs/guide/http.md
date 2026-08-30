# HTTP Client

HuxerUI provides `HttpClient` as a per-window root service backed by each platform's networking stack.
Requests use `Task`, and continuations and progress callbacks run on the owning Runtime UI thread.

## Buffered requests

`Send()` is the compact API for an ordinary request whose complete response should be retained in memory:

```cpp
Bytes Utf8Bytes(std::string_view text) {
  const auto bytes = std::as_bytes(std::span<const char>(text.data(), text.size()));
  return Bytes(bytes.begin(), bytes.end());
}

Task<HttpResult> CreateItem(const std::shared_ptr<HttpClient>& http) {
  co_return co_await http->Send({
      .url = "https://api.example.com/items",
      .method = HttpMethod::Post,
      .headers = {{"Content-Type", "application/json; charset=utf-8"}},
      .body = Utf8Bytes(R"({"name":"example"})"),
  });
}
```

`HttpResult::HasResponse()` distinguishes a received HTTP response from a transport error.
Status codes such as 404 and 500 are valid responses and remain available through `Response()`.
Timeouts, transport failures, and unsupported adapters produce `HttpError`.

HTTP bodies use `Bytes` because their content is binary regardless of `Content-Type`.
The HTTP layer preserves empty bodies, embedded null bytes, and byte sequences that are not valid UTF-8.
It does not infer an encoding, parse JSON, or decode application response formats.

## Streaming responses

`SendStream()` returns after the final response headers are available and before the response body reaches EOF:

```cpp
Task<HttpResult> DownloadManifest(const std::shared_ptr<HttpClient>& http) {
  HttpStreamResult opened = co_await http->SendStream({
      .url = "https://api.example.com/manifest",
  });
  if (!opened.HasResponse()) {
    co_return HttpResult(std::move(opened.Error()));
  }

  HttpResponseStream stream = std::move(opened).Response();
  HttpResponse response{
      .url = stream.Url(),
      .status_code = stream.StatusCode(),
      .headers = std::vector<HttpHeader>(stream.Headers().begin(), stream.Headers().end()),
  };
  while (true) {
    HttpStreamReadResult read = co_await stream.Read();
    if (read.HasData()) {
      Bytes chunk = std::move(read).Data();
      response.body.insert(response.body.end(), chunk.begin(), chunk.end());
    } else if (read.HasError()) {
      co_return HttpResult(std::move(read.Error()));
    } else {
      co_return HttpResult(std::move(response));
    }
  }
}
```

Only one `Read()` may be pending for a stream.
Each successful read returns between 1 byte and 64 KiB, and `IsComplete()` reports EOF explicitly.
Chunks are arbitrary binary boundaries and do not preserve text, JSON, multipart, or application-record boundaries.
Calling `Read()` after EOF or an error, or accessing a moved-from stream, throws `std::logic_error`.

An error before final headers appears in `HttpStreamResult`.
An error after headers appears in `HttpStreamReadResult`, so status and headers remain available as soon as the platform has received them.
Destroying an unfinished stream cancels it, and canceling a Task suspended in `Read()` cancels the complete HTTP operation.

## Transfer progress

Both request forms accept an optional progress callback:

```cpp
HttpResult result = co_await http->Send(
    {.url = "https://api.example.com/archive"},
    [](HttpProgress progress) {
      if (progress.kind == HttpProgressKind::Download) {
        UpdateDownloadedBytes(progress.transferred_bytes, progress.total_bytes);
      }
    }
);
```

Upload progress counts request bytes accepted by the platform transport, not bytes acknowledged by the server.
An early response such as 401 or 413 may arrive without a final upload callback.
Redirect replays never make the logical uploaded count exceed `HttpRequest::body.size()`.

Download progress counts the exact bytes returned by `Read()` or accumulated into `HttpResponse::body`.
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
Invalid request configuration throws `std::invalid_argument` synchronously from `Send()` or `SendStream()`.

Retries, resumable or streaming uploads, implicit file transfers, WebSocket, certificate pinning, and a framework-owned cookie jar remain outside this API.
See [HTTP Client Design](../design/http.md) for transport ownership and concurrency contracts.
