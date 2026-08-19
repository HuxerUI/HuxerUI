#include "web_http_internal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <emscripten.h>
#include <emscripten/val.h>

#include "http_internal.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

constexpr int web_http_response = 0;
constexpr int web_http_transport_error = 1;
constexpr int web_http_timeout = 2;
constexpr int web_http_canceled = 3;

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
EM_JS(emscripten::EM_VAL, CreateWebHttpOperation, (emscripten::EM_VAL request_handle, double timeout_ms), {
  return Emval.toHandle({
    request: Emval.toValue(request_handle),
    timeoutMs: timeout_ms,
    nativeHandle: 0,
    controller: null,
    timer: 0,
    finish: null,
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
  operation.controller = new AbortController();

  operation.finish = (result) => {
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
    Module._huxerui_web_http_complete(callbackHandle, Emval.toHandle(result));
  };

  try {
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
        operation.finish({kind: 2, message: "HuxerUI HTTP request timed out"});
      };
      operation.timer = setTimeout(timeout, Math.min(operation.timeoutMs, 2147483647));
    }

    fetch(request.url, options)
        .then(async (response) => {
          const headers = [];
          response.headers.forEach((value, name) => headers.push([name, value]));
          const body = new Uint8Array(await response.arrayBuffer());
          operation.finish({
            kind: 0,
            url: response.url,
            statusCode: response.status,
            headers,
            body,
          });
        })
        .catch((error) => {
          if (!operation.finished) {
            const detail = error instanceof Error && error.message ? ": " + error.message : "";
            operation.finish({kind: 1, message: "HuxerUI HTTP request failed" + detail});
          }
        });
  } catch (error) {
    const detail = error instanceof Error && error.message ? ": " + error.message : "";
    operation.finish({kind: 1, message: "HuxerUI HTTP request failed" + detail});
  }
});

EM_JS(void, CancelWebHttpOperation, (emscripten::EM_VAL operation_handle), {
  const operation = Emval.toValue(operation_handle);
  if (!operation || operation.finished) {
    return;
  }
  operation.controller?.abort();
  operation.finish({kind: 3, message: "HuxerUI HTTP request canceled"});
});
// clang-format on

class WebHttpRequest final : public std::enable_shared_from_this<WebHttpRequest> {
public:
  WebHttpRequest(HttpRequest request, HttpTransportCompletion completion)
      : request_(std::move(request)), completion_(std::move(completion)) {}

  void Start() {
    const double timeout_ms = request_.timeout.has_value() ? static_cast<double>(request_.timeout->count()) : -1.0;
    val request = MakeWebRequest(request_);
    request_ = {};
    operation_ = val::take_ownership(CreateWebHttpOperation(request.as_handle(), timeout_ms));

    auto callback = std::make_unique<std::shared_ptr<WebHttpRequest>>(shared_from_this());
    const std::uintptr_t callback_handle = reinterpret_cast<std::uintptr_t>(callback.get());
    static_cast<void>(callback.release());
    StartWebHttpOperation(operation_.as_handle(), callback_handle);
  }

  void Cancel() {
    if (finished_) {
      return;
    }
    completion_ = {};
    CancelWebHttpOperation(operation_.as_handle());
  }

  void Complete(val result) {
    if (finished_) {
      return;
    }
    finished_ = true;
    operation_ = val::undefined();

    HttpTransportCompletion completion = std::move(completion_);
    if (!completion) {
      return;
    }
    try {
      const int kind = result["kind"].as<int>();
      if (kind == web_http_response) {
        completion(HttpResult(MakeResponse(result)));
      } else if (kind == web_http_timeout) {
        completion(HttpResult(HttpError{HttpErrorCode::Timeout, ErrorMessage(result)}));
      } else if (kind == web_http_transport_error) {
        completion(HttpResult(HttpError{HttpErrorCode::Transport, ErrorMessage(result)}));
      } else if (kind == web_http_canceled) {
        return;
      } else {
        completion(HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP response is invalid"}));
      }
    } catch (...) {
      completion(HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP response is invalid"}));
    }
  }

private:
  static std::string ErrorMessage(const val& result) {
    const val message = result["message"];
    if (!message.isUndefined() && !message.isNull()) {
      const std::string value = message.as<std::string>();
      if (!value.empty()) {
        return value;
      }
    }
    return "HuxerUI HTTP request failed";
  }

  static HttpResponse MakeResponse(const val& result) {
    HttpResponse response{
        .url = result["url"].as<std::string>(),
        .status_code = result["statusCode"].as<int>(),
        .headers = {},
        .body = {},
    };

    const val headers = result["headers"];
    const std::size_t header_count = headers["length"].as<std::size_t>();
    response.headers.reserve(header_count);
    for (std::size_t index = 0; index < header_count; ++index) {
      const val entry = headers[index];
      response.headers.push_back({entry[0].as<std::string>(), entry[1].as<std::string>()});
    }

    const val body = result["body"];
    const std::size_t body_size = body["byteLength"].as<std::size_t>();
    response.body.resize(body_size);
    if (body_size != 0) {
      val(emscripten::typed_memory_view(body_size, reinterpret_cast<unsigned char*>(response.body.data())))
          .call<void>("set", body);
    }
    return response;
  }

  HttpRequest request_;
  HttpTransportCompletion completion_;
  val operation_ = val::undefined();
  bool finished_ = false;
};

class WebHttpTransport final : public HttpTransport {
public:
  std::function<void()> Start(HttpRequest request, HttpTransportCompletion completion) override {
    auto operation = std::make_shared<WebHttpRequest>(std::move(request), std::move(completion));
    operation->Start();
    return [operation] { operation->Cancel(); };
  }
};

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_web_http_complete(std::uintptr_t native_handle, emscripten::EM_VAL result_handle) {
  auto callback =
      std::unique_ptr<std::shared_ptr<WebHttpRequest>>(reinterpret_cast<std::shared_ptr<WebHttpRequest>*>(native_handle)
      );
  (*callback)->Complete(val::take_ownership(result_handle));
}

std::shared_ptr<HttpTransport> CreateWebHttpTransport() {
  return std::make_shared<WebHttpTransport>();
}

} // namespace huxerui::detail
