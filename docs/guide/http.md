# HTTP Client

HuxerUI provides `HttpClient` as a per-window root service backed by each platform's networking stack.
Requests run through `Task`, and continuation resumes on the owning Runtime thread.

## Requests and responses

HTTP bodies use `Bytes` because their content is binary regardless of `Content-Type`.
Headers, URLs, diagnostics, and other textual protocol values use UTF-8 strings.

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

The HTTP layer preserves empty bodies, embedded null bytes, and byte sequences that are not valid UTF-8.
It does not infer an encoding, parse JSON, or decode response text.
Applications that expect text select and validate an encoding from their API contract and `Content-Type`, then explicitly convert `HttpResponse::body`.

## Results and errors

`HttpResult::HasResponse()` distinguishes a received HTTP response from a transport error.
Status codes such as 404 and 500 are responses and remain available through `Response()`.
Timeouts, transport failures, and unsupported adapters produce `HttpError`.

URLs must be absolute HTTP or HTTPS URLs.
GET and HEAD reject nonempty bodies; header names and values reject invalid protocol characters; specified timeouts must be positive.
Invalid request configuration throws `std::invalid_argument` synchronously from `Send()`.

## Ownership and limits

`Send()` takes ownership of its complete `HttpRequest`, including body storage, until the platform operation completes or is canceled.
Responses are buffered completely into owned `Bytes` before the Task resumes.
Streaming, progress, retries, WebSocket, and implicit file transfers are outside this API.

See [HTTP Client Design](../design/http.md) for transport ownership, cancellation, and platform contracts.
