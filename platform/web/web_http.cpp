#include "web_http_internal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <emscripten.h>
#include <emscripten/val.h>

#include "io/http_internal.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

constexpr int web_http_complete = 0;
constexpr int web_http_transport_error = 1;
constexpr int web_http_timeout = 2;
constexpr int web_http_canceled = 3;
constexpr int web_http_unsupported = 4;

const char* HttpMethodName(HttpMethod method) {
  switch (method) {
  case HttpMethod::Get:
    return "GET";
  case HttpMethod::Head:
    return "HEAD";
  case HttpMethod::Post:
    return "POST";
  case HttpMethod::Put:
    return "PUT";
  case HttpMethod::Patch:
    return "PATCH";
  case HttpMethod::Delete:
    return "DELETE";
  case HttpMethod::Options:
    return "OPTIONS";
  }
  return "GET";
}

val MakeWebRequest(const HttpRequest& request) {
  val result = val::object();
  result.set("url", request.url);
  result.set("method", std::string(HttpMethodName(request.method)));

  val headers = val::array();
  for (const HttpHeader& header : request.headers) {
    val entry = val::array();
    entry.call<void>("push", header.name);
    entry.call<void>("push", header.value);
    headers.call<void>("push", entry);
  }
  result.set("headers", headers);

  val body = val::global("Uint8Array").new_(request.body.size());
  if (!request.body.empty()) {
    body.call<void>(
        "set",
        val(emscripten::typed_memory_view(
            request.body.size(),
            reinterpret_cast<const unsigned char*>(request.body.data())
        ))
    );
  }
  result.set("body", body);
  return result;
}

// clang-format off
EM_JS(emscripten::EM_VAL, CreateWebHttpOperation,
      (emscripten::EM_VAL request_handle, double timeout_ms, bool require_incremental_response), {
  return Emval.toHandle({
    request: Emval.toValue(request_handle),
    timeoutMs: timeout_ms,
    requireIncrementalResponse: require_incremental_response,
    nativeHandle: 0,
    controller: null,
    reader: null,
    read: null,
    fallbackBody: null,
    fallbackDelivered: false,
    timer: 0,
    finished: false,
    started: false,
  });
});

EM_JS(void, StartWebHttpOperation, (emscripten::EM_VAL operation_handle, std::uintptr_t native_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.started || operation.finished) {
    return;
  }
  operation.started = true;
  operation.nativeHandle = native_handle;

  operation.finish = (kind, message) => {
    if (operation.finished) {
      return;
    }
    operation.finished = true;
    if (operation.timer) {
      clearTimeout(operation.timer);
      operation.timer = 0;
    }
    const callbackHandle = operation.nativeHandle;
    operation.nativeHandle = 0;
    operation.request = null;
    operation.reader = null;
    operation.read = null;
    operation.fallbackBody = null;
    Module._huxerui_web_http_terminal(callbackHandle, kind, Emval.toHandle(message || ""));
  };

  operation.fail = (error) => {
    if (operation.finished) {
      return;
    }
    const detail = error instanceof Error && error.message ? ": " + error.message : "";
    operation.finish(1, "HuxerUI HTTP request failed" + detail);
  };

  try {
    operation.controller = new AbortController();
    const request = operation.request;
    const headers = new Headers();
    for (const [name, value] of request.headers) {
      headers.append(name, value);
    }
    const options = {
      method: request.method,
      headers,
      signal: operation.controller.signal,
    };
    if (request.body.byteLength !== 0) {
      options.body = request.body;
    }

    if (operation.timeoutMs >= 0) {
      const deadline = performance.now() + operation.timeoutMs;
      const timeout = () => {
        if (operation.finished) {
          return;
        }
        const remaining = deadline - performance.now();
        if (remaining > 0) {
          operation.timer = setTimeout(timeout, Math.min(remaining, 2147483647));
          return;
        }
        operation.controller.abort();
        operation.finish(2, "HuxerUI HTTP request timed out");
      };
      operation.timer = setTimeout(timeout, Math.min(operation.timeoutMs, 2147483647));
    }

    const fetchPromise = fetch(request.url, options);
    if (request.body.byteLength !== 0) {
      Module._huxerui_web_http_upload(operation.nativeHandle, request.body.byteLength);
    }
    fetchPromise
        .then(async (response) => {
          if (operation.finished) {
            return;
          }
          const headers = [];
          response.headers.forEach((value, name) => headers.push([name, value]));
          let bodySize = -1;
          const contentLength = response.headers.get("content-length");
          const contentEncoding = response.headers.get("content-encoding");
          const contentLengthIsDecimal = contentLength &&
              [...contentLength].every((character) => character >= "0" && character <= "9");
          if (!contentEncoding && contentLengthIsDecimal) {
            const parsed = Number(contentLength);
            if (Number.isSafeInteger(parsed)) {
              bodySize = parsed;
            }
          }
          if (request.method === "HEAD" || (response.status >= 100 && response.status < 200) ||
              response.status === 204 || response.status === 304) {
            bodySize = 0;
          }
          const metadata = {
            url: response.url,
            statusCode: response.status,
            headers,
            bodySize,
          };

          if (response.body && typeof response.body.getReader === "function") {
            operation.reader = response.body.getReader();
            operation.read = () => operation.reader.read()
                .then((result) => {
                  if (operation.finished) {
                    return;
                  }
                  if (result.done) {
                    operation.finish(0, "");
                    return;
                  }
                  const body = result.value instanceof Uint8Array ? result.value : new Uint8Array(result.value);
                  if (body.byteLength === 0) {
                    operation.read();
                    return;
                  }
                  Module._huxerui_web_http_body(operation.nativeHandle, Emval.toHandle(body));
                })
                .catch(operation.fail);
            Module._huxerui_web_http_response(operation.nativeHandle, Emval.toHandle(metadata));
            return;
          }
          if (!response.body && bodySize === 0) {
            Module._huxerui_web_http_response(operation.nativeHandle, Emval.toHandle(metadata));
            operation.finish(0, "");
            return;
          }
          if (operation.requireIncrementalResponse) {
            operation.controller.abort();
            operation.finish(4, "HuxerUI Web HTTP streaming requires ReadableStream support");
            return;
          }
          const fallbackBody = new Uint8Array(await response.arrayBuffer());
          if (operation.finished) {
            return;
          }
          operation.fallbackBody = fallbackBody;
          Module._huxerui_web_http_response(operation.nativeHandle, Emval.toHandle(metadata));
        })
        .catch(operation.fail);
  } catch (error) {
    operation.fail(error);
  }
});

EM_JS(void, ReadWebHttpOperation, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  if (operation.read) {
    operation.read();
    return;
  }
  if (operation.fallbackBody && !operation.fallbackDelivered) {
    operation.fallbackDelivered = true;
    if (operation.fallbackBody.byteLength !== 0) {
      Module._huxerui_web_http_body(operation.nativeHandle, Emval.toHandle(operation.fallbackBody));
    } else {
      operation.finish(0, "");
    }
    return;
  }
  operation.finish(0, "");
});

EM_JS(void, CancelWebHttpOperation, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  operation.controller?.abort();
  operation.reader?.cancel().catch(() => {});
  operation.finish(3, "HuxerUI HTTP request canceled");
});

EM_JS(void, FailWebHttpOperation, (emscripten::EM_VAL operation_handle, const char* message), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  operation.controller?.abort();
  operation.reader?.cancel().catch(() => {});
  operation.finish(1, UTF8ToString(message));
});
// clang-format on

class WebHttpRequest final : public HttpTransportOperation, public std::enable_shared_from_this<WebHttpRequest> {
public:
  WebHttpRequest(HttpRequest request, bool require_incremental_response, HttpTransportCallbacks callbacks)
      : request_(std::move(request)), require_incremental_response_(require_incremental_response),
        callbacks_(std::move(callbacks)) {}

  void Start() {
    const double timeout_ms = request_.timeout.has_value() ? static_cast<double>(request_.timeout->count()) : -1.0;
    val request = MakeWebRequest(request_);
    request_ = {};
    operation_ = val::take_ownership(
        CreateWebHttpOperation(request.as_handle(), timeout_ms, require_incremental_response_)
    );

    auto callback = std::make_unique<std::shared_ptr<WebHttpRequest>>(shared_from_this());
    const std::uintptr_t callback_handle = reinterpret_cast<std::uintptr_t>(callback.get());
    static_cast<void>(callback.release());
    StartWebHttpOperation(operation_.as_handle(), callback_handle);
  }

  void RequestRead() override {
    if (!finished_) {
      ReadWebHttpOperation(operation_.as_handle());
    }
  }

  void Cancel() noexcept override {
    if (finished_) {
      return;
    }
    callbacks_ = {};
    CancelWebHttpOperation(operation_.as_handle());
  }

  void Upload(std::uint64_t transferred_bytes) {
    if (!finished_ && callbacks_.upload_progress) {
      callbacks_.upload_progress(transferred_bytes);
    }
  }

  void Response(val result) {
    if (finished_ || !callbacks_.response) {
      return;
    }
    try {
      HttpTransportResponse response{
          .url = result["url"].as<std::string>(),
          .status_code = result["statusCode"].as<int>(),
          .headers = {},
          .body_size = std::nullopt,
      };
      const val headers = result["headers"];
      const std::size_t header_count = headers["length"].as<std::size_t>();
      response.headers.reserve(header_count);
      for (std::size_t index = 0; index < header_count; ++index) {
        const val entry = headers[index];
        response.headers.push_back({entry[0].as<std::string>(), entry[1].as<std::string>()});
      }
      const double body_size = result["bodySize"].as<double>();
      if (body_size >= 0) {
        response.body_size = static_cast<std::uint64_t>(body_size);
      }
      callbacks_.response(std::move(response));
    } catch (...) {
      FailWebHttpOperation(operation_.as_handle(), "HuxerUI HTTP response is invalid");
    }
  }

  void Body(val value) {
    if (finished_ || !callbacks_.body) {
      return;
    }
    try {
      const std::size_t size = value["byteLength"].as<std::size_t>();
      Bytes body(size);
      if (size != 0) {
        val(emscripten::typed_memory_view(size, reinterpret_cast<unsigned char*>(body.data())))
            .call<void>("set", value);
      }
      callbacks_.body(std::move(body));
    } catch (...) {
      FailWebHttpOperation(operation_.as_handle(), "HuxerUI HTTP response body is invalid");
    }
  }

  void Terminal(int kind, std::string message) {
    if (finished_) {
      return;
    }
    finished_ = true;
    operation_ = val::undefined();
    HttpTransportCallbacks callbacks = std::move(callbacks_);
    if (kind == web_http_complete) {
      if (callbacks.complete) {
        callbacks.complete();
      }
    } else if (kind == web_http_timeout) {
      if (callbacks.error) {
        callbacks.error(HttpError{HttpErrorCode::Timeout, std::move(message)});
      }
    } else if (kind == web_http_unsupported) {
      if (callbacks.error) {
        callbacks.error(HttpError{HttpErrorCode::Unsupported, std::move(message)});
      }
    } else if (kind == web_http_transport_error) {
      if (callbacks.error) {
        callbacks.error(HttpError{
            HttpErrorCode::Transport,
            message.empty() ? "HuxerUI HTTP request failed" : std::move(message),
        });
      }
    } else if (kind != web_http_canceled && callbacks.error) {
      callbacks.error(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP response is invalid"});
    }
  }

private:
  HttpRequest request_;
  bool require_incremental_response_ = false;
  HttpTransportCallbacks callbacks_;
  val operation_ = val::undefined();
  bool finished_ = false;
};

class WebHttpTransport final : public HttpTransport {
public:
  std::shared_ptr<HttpTransportOperation>
  Start(HttpRequest request, bool require_incremental_response, HttpTransportCallbacks callbacks) override {
    auto operation = std::make_shared<WebHttpRequest>(
        std::move(request), require_incremental_response, std::move(callbacks)
    );
    operation->Start();
    return operation;
  }
};

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_http_upload(std::uintptr_t native_handle, std::uint64_t transferred_bytes) {
  auto* owner = reinterpret_cast<std::shared_ptr<WebHttpRequest>*>(native_handle);
  if (owner != nullptr && *owner) {
    (*owner)->Upload(transferred_bytes);
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_http_response(std::uintptr_t native_handle, emscripten::EM_VAL result_handle) {
  val result = val::take_ownership(result_handle);
  auto* owner = reinterpret_cast<std::shared_ptr<WebHttpRequest>*>(native_handle);
  if (owner != nullptr && *owner) {
    const std::shared_ptr<WebHttpRequest> request = *owner;
    request->Response(std::move(result));
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_http_body(std::uintptr_t native_handle, emscripten::EM_VAL body_handle) {
  val body = val::take_ownership(body_handle);
  auto* owner = reinterpret_cast<std::shared_ptr<WebHttpRequest>*>(native_handle);
  if (owner != nullptr && *owner) {
    const std::shared_ptr<WebHttpRequest> request = *owner;
    request->Body(std::move(body));
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_http_terminal(std::uintptr_t native_handle, int kind, emscripten::EM_VAL message_handle) {
  val message = val::take_ownership(message_handle);
  auto owner =
      std::unique_ptr<std::shared_ptr<WebHttpRequest>>(reinterpret_cast<std::shared_ptr<WebHttpRequest>*>(native_handle)
      );
  if (owner && *owner) {
    (*owner)->Terminal(kind, message.as<std::string>());
  }
}

std::shared_ptr<HttpTransport> CreateWebHttpTransport() {
  return std::make_shared<WebHttpTransport>();
}

} // namespace huxerui::detail
