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
#include "task_internal.h"

namespace huxerui::detail {

constexpr std::size_t max_stream_chunk_size = 64U * 1024U;

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

class HttpOperationState final : public std::enable_shared_from_this<HttpOperationState> {
public:
  HttpOperationState(
      std::shared_ptr<HttpTransport> transport,
      HttpRequest request,
      bool require_incremental_response,
      std::function<void(HttpProgress)> progress
  )
      : transport_(std::move(transport)), request_(std::move(request)),
        require_incremental_response_(require_incremental_response), progress_(std::move(progress)),
        upload_total_(static_cast<std::uint64_t>(request_.body.size())) {}

  ~HttpOperationState() {
    Cancel();
  }

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

  HttpStreamReadResult TakeReadEvent() {
    std::scoped_lock lock(mutex_);
    if (body_offset_ < body_.size()) {
      const std::size_t size = std::min(max_stream_chunk_size, body_.size() - body_offset_);
      Bytes data(body_.begin() + static_cast<std::ptrdiff_t>(body_offset_),
                 body_.begin() + static_cast<std::ptrdiff_t>(body_offset_ + size));
      body_offset_ += size;
      if (body_offset_ == body_.size()) {
        body_.clear();
        body_offset_ = 0;
      }
      return HttpStreamReadResult(std::move(data));
    }
    if (error_.has_value()) {
      return HttpStreamReadResult(*error_);
    }
    if (complete_) {
      return HttpStreamReadResult::Complete();
    }
    throw std::logic_error("HuxerUI HTTP stream resumed without an event");
  }

  void ReserveRead() {
    std::scoped_lock lock(mutex_);
    if (canceled_) {
      throw std::logic_error("HuxerUI HTTP response stream is canceled");
    }
    if (read_reserved_) {
      throw std::logic_error("HuxerUI HTTP response stream already has a pending read");
    }
    if (terminal_consumed_) {
      throw std::logic_error("HuxerUI HTTP response stream has already completed");
    }
    read_reserved_ = true;
  }

  void FinishRead(bool terminal) noexcept {
    std::scoped_lock lock(mutex_);
    read_reserved_ = false;
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

  void Cancel() noexcept {
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
  bool read_reserved_ = false;
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
  explicit HttpReadAwaiter(std::shared_ptr<HttpOperationState> state) : state_(std::move(state)) {}

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> continuation) {
    state_->SuspendRead(TaskExecutionFor(continuation), continuation);
  }

  HttpStreamReadResult await_resume() {
    return state_->TakeReadEvent();
  }

private:
  std::shared_ptr<HttpOperationState> state_;
};

class HttpReadReservation final {
public:
  explicit HttpReadReservation(std::shared_ptr<HttpOperationState> state) : state_(std::move(state)) {}

  HttpReadReservation(const HttpReadReservation&) = delete;
  HttpReadReservation& operator=(const HttpReadReservation&) = delete;
  HttpReadReservation(HttpReadReservation&& other) noexcept : state_(std::exchange(other.state_, {})) {}
  HttpReadReservation& operator=(HttpReadReservation&&) = delete;

  ~HttpReadReservation() {
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

Task<HttpStreamResult> OpenHttpStream(
    std::shared_ptr<HttpTransport> transport,
    HttpRequest request,
    bool require_incremental_response,
    std::function<void(HttpProgress)> progress
) {
  if (!transport) {
    co_return HttpStreamResult(HttpError{
        HttpErrorCode::Unsupported,
        "HuxerUI HTTP is not supported by the current platform adapter",
    });
  }

  auto state = std::make_shared<HttpOperationState>(
      std::move(transport), std::move(request), require_incremental_response, std::move(progress)
  );
  while (true) {
    HttpOpenEvent event = co_await HttpOpenAwaiter(state);
    if (event.progress.has_value()) {
      state->ReportProgress(std::move(*event.progress));
      continue;
    }
    if (event.error.has_value()) {
      co_return HttpStreamResult(std::move(*event.error));
    }
    if (event.response) {
      co_return HttpStreamResult(state->MakeResponseStream());
    }
  }
}

Task<HttpStreamReadResult> ReadHttpStream(HttpReadReservation reservation) {
  HttpStreamReadResult result = co_await HttpReadAwaiter(reservation.State());
  if (result.HasData()) {
    auto progress = reservation.State()->MakeDownloadProgress(result.Data().size());
    reservation.State()->ReportProgress(std::move(progress));
    reservation.Finish(false);
  } else {
    reservation.Finish(true);
  }
  co_return std::move(result);
}

Task<HttpResult> SendHttpRequest(
    std::shared_ptr<HttpTransport> transport,
    HttpRequest request,
    std::function<void(HttpProgress)> progress
) {
  HttpStreamResult stream_result =
      co_await OpenHttpStream(std::move(transport), std::move(request), false, std::move(progress));
  if (!stream_result.HasResponse()) {
    co_return HttpResult(std::move(stream_result.Error()));
  }

  HttpResponseStream stream = std::move(stream_result).Response();
  HttpResponse response{
      .url = stream.Url(),
      .status_code = stream.StatusCode(),
      .headers = std::vector<HttpHeader>(stream.Headers().begin(), stream.Headers().end()),
      .body = {},
  };
  while (true) {
    HttpStreamReadResult read = co_await stream.Read();
    if (read.HasData()) {
      Bytes data = std::move(read).Data();
      response.body.insert(response.body.end(), data.begin(), data.end());
      continue;
    }
    if (read.HasError()) {
      co_return HttpResult(std::move(read.Error()));
    }
    co_return HttpResult(std::move(response));
  }
}

} // namespace

} // namespace huxerui::detail

namespace huxerui {

HttpStreamReadResult::HttpStreamReadResult(Bytes data) : value_(std::move(data)) {
  const std::size_t size = std::get<Bytes>(value_).size();
  if (size == 0 || size > detail::max_stream_chunk_size) {
    throw std::invalid_argument("HuxerUI HTTP stream data must contain between 1 and 65536 bytes");
  }
}

HttpStreamReadResult::HttpStreamReadResult(HttpError error) : value_(std::move(error)) {}

HttpStreamReadResult HttpStreamReadResult::Complete() {
  return HttpStreamReadResult();
}

bool HttpStreamReadResult::HasData() const noexcept {
  return std::holds_alternative<Bytes>(value_);
}

bool HttpStreamReadResult::IsComplete() const noexcept {
  return std::holds_alternative<std::monostate>(value_);
}

bool HttpStreamReadResult::HasError() const noexcept {
  return std::holds_alternative<HttpError>(value_);
}

Bytes& HttpStreamReadResult::Data() & {
  if (auto* data = std::get_if<Bytes>(&value_)) {
    return *data;
  }
  throw std::logic_error("HuxerUI HTTP stream read result does not contain data");
}

const Bytes& HttpStreamReadResult::Data() const& {
  if (const auto* data = std::get_if<Bytes>(&value_)) {
    return *data;
  }
  throw std::logic_error("HuxerUI HTTP stream read result does not contain data");
}

Bytes&& HttpStreamReadResult::Data() && {
  return std::move(static_cast<HttpStreamReadResult&>(*this).Data());
}

HttpError& HttpStreamReadResult::Error() & {
  if (auto* error = std::get_if<HttpError>(&value_)) {
    return *error;
  }
  throw std::logic_error("HuxerUI HTTP stream read result does not contain an error");
}

const HttpError& HttpStreamReadResult::Error() const& {
  if (const auto* error = std::get_if<HttpError>(&value_)) {
    return *error;
  }
  throw std::logic_error("HuxerUI HTTP stream read result does not contain an error");
}

HttpResponseStream::HttpResponseStream(std::shared_ptr<detail::HttpOperationState> state) : state_(std::move(state)) {}

HttpResponseStream::HttpResponseStream(HttpResponseStream&& other) noexcept : state_(std::move(other.state_)) {}

HttpResponseStream& HttpResponseStream::operator=(HttpResponseStream&& other) noexcept {
  if (this != &other) {
    if (state_) {
      state_->Cancel();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

HttpResponseStream::~HttpResponseStream() {
  if (state_) {
    state_->Cancel();
  }
}

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

Task<HttpStreamReadResult> HttpResponseStream::Read() {
  if (!state_) {
    throw std::logic_error("HuxerUI HTTP response stream has been moved from");
  }
  state_->ReserveRead();
  return detail::ReadHttpStream(detail::HttpReadReservation(state_));
}

HttpStreamResult::HttpStreamResult(HttpResponseStream response) : value_(std::move(response)) {}

HttpStreamResult::HttpStreamResult(HttpError error) : value_(std::move(error)) {}

bool HttpStreamResult::HasResponse() const noexcept {
  return std::holds_alternative<HttpResponseStream>(value_);
}

HttpResponseStream& HttpStreamResult::Response() & {
  if (auto* response = std::get_if<HttpResponseStream>(&value_)) {
    return *response;
  }
  throw std::logic_error("HuxerUI HTTP stream result does not contain a response");
}

const HttpResponseStream& HttpStreamResult::Response() const& {
  if (const auto* response = std::get_if<HttpResponseStream>(&value_)) {
    return *response;
  }
  throw std::logic_error("HuxerUI HTTP stream result does not contain a response");
}

HttpResponseStream&& HttpStreamResult::Response() && {
  return std::move(static_cast<HttpStreamResult&>(*this).Response());
}

HttpError& HttpStreamResult::Error() & {
  if (auto* error = std::get_if<HttpError>(&value_)) {
    return *error;
  }
  throw std::logic_error("HuxerUI HTTP stream result does not contain an error");
}

const HttpError& HttpStreamResult::Error() const& {
  if (const auto* error = std::get_if<HttpError>(&value_)) {
    return *error;
  }
  throw std::logic_error("HuxerUI HTTP stream result does not contain an error");
}

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

Task<HttpResult> HttpClient::Send(HttpRequest request, std::function<void(HttpProgress)> progress) const {
  detail::ValidateHttpRequest(request);
  return detail::SendHttpRequest(transport_, std::move(request), std::move(progress));
}

Task<HttpStreamResult>
HttpClient::SendStream(HttpRequest request, std::function<void(HttpProgress)> progress) const {
  detail::ValidateHttpRequest(request);
  return detail::OpenHttpStream(transport_, std::move(request), true, std::move(progress));
}

} // namespace huxerui
