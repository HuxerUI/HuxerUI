#include <huxerui/http.h>

#include <algorithm>
#include <coroutine>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "http_internal.h"
#include "application/application_internal.h"
#include "runtime/task_internal.h"
#include "stream_internal.h"

namespace huxerui::detail {

namespace {

/// Validates an HTTP operation against the application that originally created its client.
/// @param original Weak binding retained by the client or operation, never rebound after shutdown.
/// @return The original live state on its application thread.
/// @throws std::logic_error If expired/stopped or used from another thread/application context.
std::shared_ptr<ApplicationRuntimeState> RequireHttpApplication(
    const std::weak_ptr<ApplicationRuntimeState>& original) {
  const auto application = original.lock();
  if (!application) throw std::logic_error("HuxerUI HTTP application lifetime has ended");
  ValidateApplicationCall(application->execution);
  if (application->phase == ApplicationRuntimeState::Phase::Stopped) {
    throw std::logic_error("HuxerUI HTTP application lifetime has ended");
  }
  return application;
}

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

std::uint64_t SaturatingAdd(std::uint64_t left, std::size_t right) noexcept {
  const auto right_value = static_cast<std::uint64_t>(right);
  if (right_value > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right_value;
}

struct HttpOpenEvent {
  std::optional<HttpProgress> progress;
  bool response = false;
  std::optional<HttpError> error;
};

} // namespace

class HttpOperationState final : public AsyncInputStreamState, public std::enable_shared_from_this<HttpOperationState> {
public:
  HttpOperationState(std::weak_ptr<ApplicationRuntimeState> application, std::shared_ptr<HttpTransport> transport,
                     HttpRequest request, bool require_incremental_response,
                     std::function<void(HttpProgress)> progress)
      : application_(std::move(application)), transport_(std::move(transport)), request_(std::move(request)),
        require_incremental_response_(require_incremental_response), progress_(std::move(progress)),
        upload_total_(static_cast<std::uint64_t>(request_.body.size())) {}

  ~HttpOperationState() override {
    Cancel();
  }

  Task<IoResult<Bytes>> ReadAsync(std::size_t maximum_bytes) override;

  void SuspendOpen(std::weak_ptr<TaskExecution> execution, std::coroutine_handle<> continuation) {
    bool start = false;
    bool resume = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        return;
      }
      execution_ = std::move(execution);
      continuation_ = continuation;
      start = !started_;
      started_ = true;
      resume = HasOpenEventLocked();
    }

    if (start) {
      StartTransport();
    }
    if (resume) {
      ResumeContinuation();
    }
  }

  HttpOpenEvent TakeOpenEvent() {
    std::scoped_lock lock(mutex_);
    if (pending_upload_progress_.has_value()) {
      const std::uint64_t transferred = std::exchange(pending_upload_progress_, std::nullopt).value();
      reported_upload_progress_ = transferred;
      return HttpOpenEvent{
          .progress = HttpProgress{
              .kind = HttpProgressKind::Upload,
              .transferred_bytes = transferred,
              .total_bytes = upload_total_,
          },
          .response = false,
          .error = std::nullopt,
      };
    }
    if (response_.has_value() && !response_taken_) {
      response_taken_ = true;
      return HttpOpenEvent{.progress = std::nullopt, .response = true, .error = std::nullopt};
    }
    if (error_.has_value() && !response_.has_value()) {
      return HttpOpenEvent{.progress = std::nullopt, .response = false, .error = *error_};
    }
    throw std::logic_error("HuxerUI HTTP request resumed without an event");
  }

  void SuspendRead(std::weak_ptr<TaskExecution> execution, std::coroutine_handle<> continuation) {
    std::shared_ptr<HttpTransportOperation> operation;
    bool resume = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        return;
      }
      execution_ = std::move(execution);
      continuation_ = continuation;
      if (HasReadEventLocked()) {
        resume = true;
      } else if (!read_request_pending_) {
        read_request_pending_ = true;
        operation = operation_;
      }
    }

    if (operation) {
      operation->RequestRead();
    }
    if (resume) {
      ResumeContinuation();
    }
  }

  IoResult<Bytes> TakeReadEvent(std::size_t maximum_bytes) {
    std::scoped_lock lock(mutex_);
    if (body_offset_ < body_.size()) {
      const std::size_t size = std::min(maximum_bytes, body_.size() - body_offset_);
      Bytes data(body_.begin() + static_cast<std::ptrdiff_t>(body_offset_),
                 body_.begin() + static_cast<std::ptrdiff_t>(body_offset_ + size));
      body_offset_ += size;
      if (body_offset_ == body_.size()) {
        body_.clear();
        body_offset_ = 0;
      }
      return IoResult<Bytes>(std::move(data));
    }
    if (error_.has_value()) {
      return IoResult<Bytes>(IoError{
          error_->code == HttpErrorCode::Timeout       ? IoErrorCode::Timeout
          : error_->code == HttpErrorCode::Unsupported ? IoErrorCode::Unsupported
                                                       : IoErrorCode::Io,
          error_->message});
    }
    if (complete_) {
      return IoResult<Bytes>(Bytes{});
    }
    throw std::logic_error("HuxerUI HTTP stream resumed without an event");
  }

  void RequireReadable() {
    static_cast<void>(RequireHttpApplication(application_));
    std::scoped_lock lock(mutex_);
    if (canceled_) {
      throw std::logic_error("HuxerUI HTTP response stream is canceled");
    }
    if (terminal_consumed_) {
      throw std::logic_error("HuxerUI HTTP response stream has already completed");
    }
  }

  void FinishRead(bool terminal) noexcept {
    std::scoped_lock lock(mutex_);
    terminal_consumed_ = terminal;
  }

  [[nodiscard]] const std::string& Url() const {
    return Response().url;
  }

  [[nodiscard]] int StatusCode() const {
    return Response().status_code;
  }

  [[nodiscard]] std::span<const HttpHeader> Headers() const {
    return Response().headers;
  }

  [[nodiscard]] HttpResponseStream MakeResponseStream() {
    return HttpResponseStream(shared_from_this());
  }

  void ReportProgress(HttpProgress progress) {
    if (progress_) {
      progress_(std::move(progress));
    }
  }

  [[nodiscard]] HttpProgress MakeDownloadProgress(std::size_t size) {
    std::scoped_lock lock(mutex_);
    downloaded_bytes_ = SaturatingAdd(downloaded_bytes_, size);
    if (response_->body_size.has_value() && downloaded_bytes_ > *response_->body_size) {
      response_->body_size.reset();
    }
    return HttpProgress{
        .kind = HttpProgressKind::Download,
        .transferred_bytes = downloaded_bytes_,
        .total_bytes = response_->body_size,
    };
  }

  void Cancel() noexcept override {
    std::shared_ptr<HttpTransportOperation> operation;
    bool cancel_operation = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || terminal_consumed_) {
        return;
      }
      canceled_ = true;
      execution_.reset();
      continuation_ = {};
      operation = std::move(operation_);
      cancel_operation = !transport_terminal_;
    }
    MarkFailed();
    if (operation && cancel_operation) {
      operation->Cancel();
    }
  }

private:
  void StartTransport() {
    std::weak_ptr<HttpOperationState> weak = shared_from_this();
    HttpTransportCallbacks callbacks{
        .upload_progress = [weak](std::uint64_t transferred_bytes) {
          if (auto state = weak.lock()) {
            state->OnUploadProgress(transferred_bytes);
          }
        },
        .response = [weak](HttpTransportResponse response) {
          if (auto state = weak.lock()) {
            state->OnResponse(std::move(response));
          }
        },
        .body = [weak](Bytes body) {
          if (auto state = weak.lock()) {
            state->OnBody(std::move(body));
          }
        },
        .complete = [weak] {
          if (auto state = weak.lock()) {
            state->OnComplete();
          }
        },
        .error = [weak](HttpError error) {
          if (auto state = weak.lock()) {
            state->OnError(std::move(error));
          }
        },
    };

    std::shared_ptr<HttpTransportOperation> operation;
    try {
      operation = transport_->Start(std::move(request_), require_incremental_response_, std::move(callbacks));
      if (!operation) {
        OnError(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP transport did not start an operation"});
        return;
      }
    } catch (const std::exception& exception) {
      OnError(HttpError{
          HttpErrorCode::Transport,
          std::string("HuxerUI HTTP transport failed to start: ") + exception.what(),
      });
      return;
    } catch (...) {
      OnError(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP transport failed to start"});
      return;
    }

    bool cancel = false;
    bool request_read = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        cancel = true;
      } else {
        operation_ = operation;
        request_read = read_request_pending_;
      }
    }
    if (cancel) {
      operation->Cancel();
    } else if (request_read) {
      operation->RequestRead();
    }
  }

  void OnUploadProgress(std::uint64_t transferred_bytes) noexcept {
    bool resume = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || response_.has_value() || !progress_) {
        return;
      }
      transferred_bytes = std::min(transferred_bytes, upload_total_);
      const std::uint64_t pending = pending_upload_progress_.value_or(reported_upload_progress_);
      if (transferred_bytes <= pending) {
        return;
      }
      pending_upload_progress_ = transferred_bytes;
      resume = static_cast<bool>(continuation_);
    }
    if (resume) {
      ResumeContinuation();
    }
  }

  void OnResponse(HttpTransportResponse response) noexcept {
    bool resume = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || response_.has_value() || error_.has_value()) {
        return;
      }
      response_.emplace(std::move(response));
      resume = static_cast<bool>(continuation_);
    }
    if (resume) {
      ResumeContinuation();
    }
  }

  void OnBody(Bytes body) noexcept {
    if (body.empty()) {
      OnError(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP transport returned an empty body chunk"});
      return;
    }

    bool resume = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || complete_ || error_.has_value()) {
        return;
      }
      if (!response_.has_value()) {
        SetErrorLocked(HttpError{
            HttpErrorCode::Transport,
            "HuxerUI HTTP transport returned body data before response metadata",
        });
      } else {
        read_request_pending_ = false;
        if (body_offset_ == body_.size()) {
          body_ = std::move(body);
          body_offset_ = 0;
        } else {
          body_.insert(body_.end(), body.begin(), body.end());
        }
      }
      resume = static_cast<bool>(continuation_);
    }
    if (resume) {
      ResumeContinuation();
    }
  }

  void OnComplete() noexcept {
    bool resume = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || complete_ || error_.has_value()) {
        return;
      }
      if (!response_.has_value()) {
        SetErrorLocked(HttpError{
            HttpErrorCode::Transport,
            "HuxerUI HTTP transport completed before response metadata",
        });
      } else {
        read_request_pending_ = false;
        complete_ = true;
      }
      transport_terminal_ = true;
      resume = static_cast<bool>(continuation_);
    }
    if (resume) {
      ResumeContinuation();
    }
  }

  void OnError(HttpError error) noexcept {
    bool resume = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || complete_ || error_.has_value()) {
        return;
      }
      SetErrorLocked(std::move(error));
      transport_terminal_ = true;
      resume = static_cast<bool>(continuation_);
    }
    if (resume) {
      ResumeContinuation();
    }
  }

  void SetErrorLocked(HttpError error) noexcept {
    read_request_pending_ = false;
    error_.emplace(std::move(error));
  }

  void ResumeContinuation() noexcept {
    std::weak_ptr<TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || !continuation_) {
        return;
      }
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    ResumeTask(execution, continuation);
  }

  [[nodiscard]] bool HasOpenEventLocked() const noexcept {
    return pending_upload_progress_.has_value() || (response_.has_value() && !response_taken_) ||
           (error_.has_value() && !response_.has_value());
  }

  [[nodiscard]] bool HasReadEventLocked() const noexcept {
    return body_offset_ < body_.size() || error_.has_value() || complete_;
  }

  [[nodiscard]] const HttpTransportResponse& Response() const {
    if (!response_.has_value()) {
      throw std::logic_error("HuxerUI HTTP response stream has no response metadata");
    }
    return *response_;
  }

  mutable std::mutex mutex_;
  std::weak_ptr<ApplicationRuntimeState> application_;
  std::shared_ptr<HttpTransport> transport_;
  HttpRequest request_;
  bool require_incremental_response_ = false;
  std::function<void(HttpProgress)> progress_;
  const std::uint64_t upload_total_;
  std::shared_ptr<HttpTransportOperation> operation_;
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::optional<HttpTransportResponse> response_;
  std::optional<HttpError> error_;
  Bytes body_;
  std::size_t body_offset_ = 0;
  std::optional<std::uint64_t> pending_upload_progress_;
  std::uint64_t reported_upload_progress_ = 0;
  std::uint64_t downloaded_bytes_ = 0;
  bool started_ = false;
  bool response_taken_ = false;
  bool read_request_pending_ = false;
  bool complete_ = false;
  bool transport_terminal_ = false;
  bool terminal_consumed_ = false;
  bool canceled_ = false;
};

namespace {

class HttpOpenAwaiter final {
public:
  explicit HttpOpenAwaiter(std::shared_ptr<HttpOperationState> state) : state_(std::move(state)) {}

  HttpOpenAwaiter(const HttpOpenAwaiter&) = delete;
  HttpOpenAwaiter& operator=(const HttpOpenAwaiter&) = delete;
  HttpOpenAwaiter(HttpOpenAwaiter&&) noexcept = default;
  HttpOpenAwaiter& operator=(HttpOpenAwaiter&&) noexcept = default;

  ~HttpOpenAwaiter() {
    if (state_) {
      state_->Cancel();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> continuation) {
    state_->SuspendOpen(TaskExecutionFor(continuation), continuation);
  }

  HttpOpenEvent await_resume() {
    return std::exchange(state_, {})->TakeOpenEvent();
  }

private:
  std::shared_ptr<HttpOperationState> state_;
};

class HttpReadAwaiter final {
public:
  HttpReadAwaiter(std::shared_ptr<HttpOperationState> state, std::size_t maximum_bytes)
      : state_(std::move(state)), maximum_bytes_(maximum_bytes) {}

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> continuation) {
    state_->SuspendRead(TaskExecutionFor(continuation), continuation);
  }

  IoResult<Bytes> await_resume() {
    return state_->TakeReadEvent(maximum_bytes_);
  }

private:
  std::shared_ptr<HttpOperationState> state_;
  std::size_t maximum_bytes_ = 0;
};

class HttpReadCancellation final {
public:
  explicit HttpReadCancellation(std::shared_ptr<HttpOperationState> state) : state_(std::move(state)) {}

  HttpReadCancellation(const HttpReadCancellation&) = delete;
  HttpReadCancellation& operator=(const HttpReadCancellation&) = delete;
  HttpReadCancellation(HttpReadCancellation&& other) noexcept : state_(std::exchange(other.state_, {})) {}
  HttpReadCancellation& operator=(HttpReadCancellation&&) = delete;

  ~HttpReadCancellation() {
    if (state_) {
      state_->Cancel();
    }
  }

  [[nodiscard]] const std::shared_ptr<HttpOperationState>& State() const noexcept {
    return state_;
  }

  void Finish(bool terminal) noexcept {
    state_->FinishRead(terminal);
    state_.reset();
  }

private:
  std::shared_ptr<HttpOperationState> state_;
};

Task<HttpResult<HttpResponseStream>> OpenHttpStream(std::weak_ptr<ApplicationRuntimeState> original,
                                                    std::shared_ptr<HttpTransport> transport, HttpRequest request,
                                                    bool require_incremental_response,
                                                    std::function<void(HttpProgress)> progress) {
  const auto application = RequireHttpApplication(original);
  if (!transport) {
    co_return HttpResult<HttpResponseStream>(HttpError{
        HttpErrorCode::Unsupported,
        "HuxerUI HTTP is not supported by the current platform adapter",
    });
  }

  auto state = std::make_shared<HttpOperationState>(original, std::move(transport), std::move(request),
                                                    require_incremental_response, std::move(progress));
  std::erase_if(application->http_operations, [](const auto& operation) { return operation.expired(); });
  application->http_operations.push_back(state);
  while (true) {
    HttpOpenEvent event = co_await HttpOpenAwaiter(state);
    if (event.progress.has_value()) {
      state->ReportProgress(std::move(*event.progress));
      continue;
    }
    if (event.error.has_value()) {
      co_return HttpResult<HttpResponseStream>(std::move(*event.error));
    }
    if (event.response) {
      co_return HttpResult<HttpResponseStream>(state->MakeResponseStream());
    }
  }
}

Task<IoResult<Bytes>> ReadHttpStream(HttpReadCancellation cancellation, std::size_t maximum_bytes) {
  cancellation.State()->RequireReadable();
  auto data = co_await HttpReadAwaiter(cancellation.State(), maximum_bytes);
  if (!data.Succeeded() || data.Value().empty()) {
    cancellation.Finish(true);
    co_return data;
  }
  auto progress = cancellation.State()->MakeDownloadProgress(data.Value().size());
  cancellation.State()->ReportProgress(std::move(progress));
  cancellation.Finish(false);
  co_return data;
}

Task<HttpResult<HttpResponse>> SendHttpRequest(std::weak_ptr<ApplicationRuntimeState> application,
                                               std::shared_ptr<HttpTransport> transport, HttpRequest request,
                                               std::function<void(HttpProgress)> progress) {
  HttpResult<HttpResponseStream> stream_result =
      co_await OpenHttpStream(application, std::move(transport), std::move(request), false, std::move(progress));
  if (!stream_result.Succeeded()) {
    co_return HttpResult<HttpResponse>(std::move(stream_result.Error()));
  }

  HttpResponseStream stream = std::move(stream_result).Value();
  HttpResponse response{
      .url = stream.Url(),
      .status_code = stream.StatusCode(),
      .headers = std::vector<HttpHeader>(stream.Headers().begin(), stream.Headers().end()),
      .body = {},
  };
  while (true) {
    auto result = co_await stream.Body().ReadAsync(64U * 1024U);
    if (!result.Succeeded()) {
      auto error = std::move(result).Error();
      const auto code = error.code == IoErrorCode::Timeout       ? HttpErrorCode::Timeout
                        : error.code == IoErrorCode::Unsupported ? HttpErrorCode::Unsupported
                                                                 : HttpErrorCode::Transport;
      co_return HttpResult<HttpResponse>(HttpError{code, std::move(error.message)});
    }
    const auto& data = result.Value();
    if (data.empty()) {
      co_return HttpResult<HttpResponse>(std::move(response));
    }
    response.body.insert(response.body.end(), data.begin(), data.end());
  }
}

} // namespace

Task<IoResult<Bytes>> HttpOperationState::ReadAsync(std::size_t maximum_bytes) {
  RequireReadable();
  return ReadHttpStream(HttpReadCancellation(shared_from_this()), maximum_bytes);
}

void DisconnectHttpOperations(ApplicationRuntimeState& application) noexcept {
  for (const auto& weak : application.http_operations) {
    if (const auto operation = weak.lock()) operation->Cancel();
  }
  application.http_operations.clear();
}

} // namespace huxerui::detail

namespace huxerui {

HttpResponseStream::HttpResponseStream(std::shared_ptr<detail::HttpOperationState> state)
    : state_(std::move(state)), body_(detail::StreamAccess::MakeAsyncInputStream(state_)) {}

HttpResponseStream::HttpResponseStream(HttpResponseStream&& other) noexcept
    : state_(std::move(other.state_)), body_(std::move(other.body_)) {}

HttpResponseStream& HttpResponseStream::operator=(HttpResponseStream&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
    body_ = std::move(other.body_);
  }
  return *this;
}

HttpResponseStream::~HttpResponseStream() = default;

const std::string& HttpResponseStream::Url() const {
  if (!state_) {
    throw std::logic_error("HuxerUI HTTP response stream has been moved from");
  }
  return state_->Url();
}

int HttpResponseStream::StatusCode() const {
  if (!state_) {
    throw std::logic_error("HuxerUI HTTP response stream has been moved from");
  }
  return state_->StatusCode();
}

std::span<const HttpHeader> HttpResponseStream::Headers() const {
  if (!state_) {
    throw std::logic_error("HuxerUI HTTP response stream has been moved from");
  }
  return state_->Headers();
}

AsyncInputStream& HttpResponseStream::Body() {
  if (!state_) {
    throw std::logic_error("HuxerUI HTTP response stream has been moved from");
  }
  return body_;
}

HttpClient::HttpClient() : HttpClient(detail::CurrentApplicationRuntime()->http_transport) {}

HttpClient::HttpClient(std::shared_ptr<detail::HttpTransport> transport)
    : transport_(std::move(transport)), application_(detail::CurrentApplicationRuntime()) {}

HttpClient::~HttpClient() = default;

Task<HttpResult<HttpResponse>> HttpClient::SendAsync(HttpRequest request, std::function<void(HttpProgress)> progress) const {
  static_cast<void>(detail::RequireHttpApplication(application_));
  detail::ValidateHttpRequest(request);
  return detail::SendHttpRequest(application_, transport_, std::move(request), std::move(progress));
}

Task<HttpResult<HttpResponseStream>>
HttpClient::SendStreamAsync(HttpRequest request, std::function<void(HttpProgress)> progress) const {
  static_cast<void>(detail::RequireHttpApplication(application_));
  detail::ValidateHttpRequest(request);
  return detail::OpenHttpStream(application_, transport_, std::move(request), true, std::move(progress));
}

} // namespace huxerui
