#include <huxerui/http.h>

#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "http_internal.h"
#include "task_internal.h"

namespace huxerui::detail {

namespace {

bool IsHeaderNameCharacter(unsigned char value) noexcept {
  if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')) {
    return true;
  }
  switch (value) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

bool ContainsInvalidLineCharacter(std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos || value.find('\r') != std::string_view::npos ||
         value.find('\n') != std::string_view::npos;
}

bool StartsWithAsciiCaseInsensitive(std::string_view value, std::string_view prefix) noexcept {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    unsigned char character = value[index];
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<unsigned char>(character + ('a' - 'A'));
    }
    if (character != static_cast<unsigned char>(prefix[index])) {
      return false;
    }
  }
  return true;
}

void ValidateHttpMethod(HttpMethod method) {
  switch (method) {
  case HttpMethod::Get:
  case HttpMethod::Head:
  case HttpMethod::Post:
  case HttpMethod::Put:
  case HttpMethod::Patch:
  case HttpMethod::Delete:
  case HttpMethod::Options:
    return;
  }
  throw std::invalid_argument("HuxerUI HTTP method is invalid");
}

void ValidateHttpRequest(const HttpRequest& request) {
  ValidateHttpMethod(request.method);
  if ((!StartsWithAsciiCaseInsensitive(request.url, "http://") &&
       !StartsWithAsciiCaseInsensitive(request.url, "https://")) ||
      ContainsInvalidLineCharacter(request.url)) {
    throw std::invalid_argument("HuxerUI HTTP URL must be an absolute HTTP or HTTPS URL");
  }
  if ((request.method == HttpMethod::Get || request.method == HttpMethod::Head) && !request.body.empty()) {
    throw std::invalid_argument("HuxerUI HTTP GET and HEAD requests must not contain a body");
  }
  if (request.timeout.has_value() && *request.timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("HuxerUI HTTP timeout must be positive when specified");
  }
  for (const HttpHeader& header : request.headers) {
    if (header.name.empty()) {
      throw std::invalid_argument("HuxerUI HTTP header name must not be empty");
    }
    for (unsigned char value : header.name) {
      if (!IsHeaderNameCharacter(value)) {
        throw std::invalid_argument("HuxerUI HTTP header name contains an invalid character");
      }
    }
    if (ContainsInvalidLineCharacter(header.value)) {
      throw std::invalid_argument("HuxerUI HTTP header value contains an invalid character");
    }
  }
}

void InvokeCancellation(std::function<void()> cancellation) noexcept {
  if (!cancellation) {
    return;
  }
  try {
    cancellation();
  } catch (...) {
  }
}

class HttpRequestState final : public std::enable_shared_from_this<HttpRequestState> {
public:
  HttpRequestState(std::shared_ptr<HttpTransport> transport, HttpRequest request)
      : transport_(std::move(transport)), request_(std::move(request)) {}

  void
  Suspend(std::weak_ptr<TaskExecution> execution, std::coroutine_handle<Task<HttpResult>::promise_type> continuation) {
    {
      std::scoped_lock lock(mutex_);
      execution_ = std::move(execution);
      continuation_ = continuation;
    }

    std::weak_ptr<HttpRequestState> weak = shared_from_this();
    std::function<void()> cancellation = transport_->Start(std::move(request_), [weak](HttpResult result) {
      if (auto state = weak.lock()) {
        state->Complete(std::move(result));
      }
    });

    bool cancel_started_request = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        cancel_started_request = true;
      } else if (!result_.has_value()) {
        cancellation_ = std::move(cancellation);
      }
    }
    if (cancel_started_request) {
      InvokeCancellation(std::move(cancellation));
    }
  }

  HttpResult TakeResult() {
    {
      std::scoped_lock lock(mutex_);
      if (!result_.has_value()) {
        throw std::logic_error("HuxerUI HTTP request resumed without a result");
      }
      return std::move(*result_);
    }
  }

  void Cancel() noexcept {
    std::function<void()> cancellation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || result_.has_value()) {
        return;
      }
      canceled_ = true;
      execution_.reset();
      continuation_ = {};
      cancellation = std::move(cancellation_);
    }
    InvokeCancellation(std::move(cancellation));
  }

private:
  void Complete(HttpResult result) noexcept {
    std::weak_ptr<TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || result_.has_value()) {
        return;
      }
      result_.emplace(std::move(result));
      cancellation_ = {};
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    ResumeTask(execution, continuation);
  }

  std::mutex mutex_;
  std::shared_ptr<HttpTransport> transport_;
  HttpRequest request_;
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::function<void()> cancellation_;
  std::optional<HttpResult> result_;
  bool canceled_ = false;
};

class HttpRequestAwaiter final {
public:
  HttpRequestAwaiter(std::shared_ptr<HttpTransport> transport, HttpRequest request)
      : state_(std::make_shared<HttpRequestState>(std::move(transport), std::move(request))) {}

  HttpRequestAwaiter(const HttpRequestAwaiter&) = delete;
  HttpRequestAwaiter& operator=(const HttpRequestAwaiter&) = delete;
  HttpRequestAwaiter(HttpRequestAwaiter&&) noexcept = default;
  HttpRequestAwaiter& operator=(HttpRequestAwaiter&&) noexcept = default;

  ~HttpRequestAwaiter() {
    if (state_) {
      state_->Cancel();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<Task<HttpResult>::promise_type> continuation) {
    state_->Suspend(TaskExecutionFor(continuation), continuation);
  }

  HttpResult await_resume() {
    return state_->TakeResult();
  }

private:
  std::shared_ptr<HttpRequestState> state_;
};

Task<HttpResult> SendHttpRequest(std::shared_ptr<HttpTransport> transport, HttpRequest request) {
  if (!transport) {
    co_return HttpResult(HttpError{
        HttpErrorCode::Unsupported,
        "HuxerUI HTTP is not supported by the current platform adapter",
    });
  }
  co_return co_await HttpRequestAwaiter(std::move(transport), std::move(request));
}

} // namespace

} // namespace huxerui::detail

namespace huxerui {

HttpResult::HttpResult(HttpResponse response) : value_(std::move(response)) {}

HttpResult::HttpResult(HttpError error) : value_(std::move(error)) {}

bool HttpResult::HasResponse() const noexcept {
  return std::holds_alternative<HttpResponse>(value_);
}

HttpResponse& HttpResult::Response() & {
  if (auto* response = std::get_if<HttpResponse>(&value_)) {
    return *response;
  }
  throw std::logic_error("HuxerUI HTTP result does not contain a response");
}

const HttpResponse& HttpResult::Response() const& {
  if (const auto* response = std::get_if<HttpResponse>(&value_)) {
    return *response;
  }
  throw std::logic_error("HuxerUI HTTP result does not contain a response");
}

HttpResponse&& HttpResult::Response() && {
  return std::move(static_cast<HttpResult&>(*this).Response());
}

HttpError& HttpResult::Error() & {
  if (auto* error = std::get_if<HttpError>(&value_)) {
    return *error;
  }
  throw std::logic_error("HuxerUI HTTP result does not contain an error");
}

const HttpError& HttpResult::Error() const& {
  if (const auto* error = std::get_if<HttpError>(&value_)) {
    return *error;
  }
  throw std::logic_error("HuxerUI HTTP result does not contain an error");
}

HttpClient::HttpClient(std::shared_ptr<detail::HttpTransport> transport) : transport_(std::move(transport)) {}

HttpClient::~HttpClient() = default;

Task<HttpResult> HttpClient::Send(HttpRequest request) const {
  detail::ValidateHttpRequest(request);
  return detail::SendHttpRequest(transport_, std::move(request));
}

} // namespace huxerui
