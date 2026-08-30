#include "linux_internal.h"

#include "linux_http_internal.h"

#include <libsoup/soup.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "http_internal.h"

namespace huxerui::detail {

namespace {

constexpr gsize read_buffer_size = 64U * 1024U;

const char* HttpMethodName(HttpMethod method) noexcept {
  switch (method) {
  case HttpMethod::Get:
    return SOUP_METHOD_GET;
  case HttpMethod::Head:
    return SOUP_METHOD_HEAD;
  case HttpMethod::Post:
    return SOUP_METHOD_POST;
  case HttpMethod::Put:
    return SOUP_METHOD_PUT;
  case HttpMethod::Patch:
    return "PATCH";
  case HttpMethod::Delete:
    return SOUP_METHOD_DELETE;
  case HttpMethod::Options:
    return SOUP_METHOD_OPTIONS;
  }
  return SOUP_METHOD_GET;
}

std::string ErrorMessage(const GError* error) {
  if (error == nullptr || error->message == nullptr || error->message[0] == '\0') {
    return "HuxerUI Linux HTTP request failed";
  }
  return "HuxerUI Linux HTTP request failed: " + std::string(error->message);
}

bool EqualsAsciiCaseInsensitive(std::string_view first, std::string_view second) noexcept {
  if (first.size() != second.size()) {
    return false;
  }
  for (std::size_t index = 0; index < first.size(); ++index) {
    unsigned char left = first[index];
    unsigned char right = second[index];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<unsigned char>(left + ('a' - 'A'));
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<unsigned char>(right + ('a' - 'A'));
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

std::optional<std::uint64_t> ParseContentLength(const std::vector<HttpHeader>& headers) noexcept {
  std::optional<std::uint64_t> content_length;
  for (const HttpHeader& header : headers) {
    if (EqualsAsciiCaseInsensitive(header.name, "Content-Encoding") ||
        EqualsAsciiCaseInsensitive(header.name, "Transfer-Encoding")) {
      return std::nullopt;
    }
    if (!EqualsAsciiCaseInsensitive(header.name, "Content-Length")) {
      continue;
    }
    std::uint64_t value = 0;
    if (header.value.empty()) {
      return std::nullopt;
    }
    for (const unsigned char character : header.value) {
      if (character < '0' || character > '9') {
        return std::nullopt;
      }
      const std::uint64_t digit = character - '0';
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
        return std::nullopt;
      }
      value = value * 10U + digit;
    }
    if (content_length.has_value() && *content_length != value) {
      return std::nullopt;
    }
    content_length = value;
  }
  return content_length;
}

class LinuxHttpRequest;

class LinuxHttpTransportState final : public std::enable_shared_from_this<LinuxHttpTransportState> {
public:
  static std::shared_ptr<LinuxHttpTransportState> Create() {
    auto state = std::shared_ptr<LinuxHttpTransportState>(new LinuxHttpTransportState());
    state->StartThread();
    return state;
  }

  ~LinuxHttpTransportState() {
    if (loop_ != nullptr) {
      g_main_loop_unref(loop_);
    }
    if (context_ != nullptr) {
      g_main_context_unref(context_);
    }
  }

  void Post(std::function<void()> callback) {
    struct Invocation {
      std::function<void()> callback;
    };
    auto* invocation = new Invocation{std::move(callback)};
    g_main_context_invoke_full(
        context_,
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          auto* value = static_cast<Invocation*>(data);
          try {
            value->callback();
          } catch (...) {
          }
          return G_SOURCE_REMOVE;
        },
        invocation,
        [](gpointer data) { delete static_cast<Invocation*>(data); }
    );
  }

  void Queue(std::shared_ptr<LinuxHttpRequest> request);
  void Remove(LinuxHttpRequest* request) noexcept;

  void Stop() noexcept {
    if (!thread_.joinable()) {
      return;
    }
    if (thread_.get_id() == std::this_thread::get_id()) {
      StopOnNetworkThread();
      thread_.detach();
      return;
    }
    g_main_context_invoke_full(
        context_,
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          static_cast<LinuxHttpTransportState*>(data)->StopOnNetworkThread();
          return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
    thread_.join();
  }

private:
  LinuxHttpTransportState() : context_(g_main_context_new()), loop_(g_main_loop_new(context_, false)) {}

  void StartThread() {
    const std::shared_ptr<LinuxHttpTransportState> self = shared_from_this();
    thread_ = std::thread([self] { self->Run(); });
    std::unique_lock lock(ready_mutex_);
    ready_condition_.wait(lock, [this] { return ready_; });
  }

  void Run() noexcept {
    g_main_context_push_thread_default(context_);
    session_ = soup_session_new();
    soup_session_set_timeout(session_, 0);
    {
      std::lock_guard lock(ready_mutex_);
      ready_ = true;
    }
    ready_condition_.notify_all();
    g_main_loop_run(loop_);
    g_clear_object(&session_);
    g_main_context_pop_thread_default(context_);
  }

  void StopOnNetworkThread() noexcept;

  void MaybeQuit() noexcept {
    if (stopping_ && requests_.empty()) {
      g_main_loop_quit(loop_);
    }
  }

  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  SoupSession* session_ = nullptr;
  std::thread thread_;
  std::mutex ready_mutex_;
  std::condition_variable ready_condition_;
  bool ready_ = false;
  bool stopping_ = false;
  std::vector<std::shared_ptr<LinuxHttpRequest>> requests_;
};

class LinuxHttpRequest final : public HttpTransportOperation, public std::enable_shared_from_this<LinuxHttpRequest> {
public:
  LinuxHttpRequest(
      std::weak_ptr<LinuxHttpTransportState> transport, HttpRequest request, HttpTransportCallbacks callbacks
  )
      : transport_(std::move(transport)), request_(std::move(request)), callbacks_(std::move(callbacks)),
        cancellable_(g_cancellable_new()) {}

  ~LinuxHttpRequest() override {
    g_object_unref(cancellable_);
  }

  [[nodiscard]] bool Finished() const noexcept {
    std::scoped_lock lock(mutex_);
    return finished_;
  }

  void Start(SoupSession* session) noexcept {
    try {
      message_ = soup_message_new(HttpMethodName(request_.method), request_.url.c_str());
      if (message_ == nullptr) {
        FinishWithoutNative(HttpError{HttpErrorCode::Transport, "HuxerUI Linux HTTP URL could not be parsed"});
        return;
      }

      SoupMessageHeaders* request_headers = soup_message_get_request_headers(message_);
      for (const HttpHeader& header : request_.headers) {
        soup_message_headers_append(request_headers, header.name.c_str(), header.value.c_str());
      }
      if (!request_.body.empty()) {
        g_signal_connect(
            message_,
            "restarted",
            G_CALLBACK(+[](SoupMessage* message, gpointer data) {
              static_cast<LinuxHttpRequest*>(data)->SetRequestBody(message);
            }),
            this
        );
        SetRequestBody(message_);
      }

      if (request_.timeout.has_value()) {
        const std::chrono::milliseconds::rep count = request_.timeout->count();
        const auto maximum = static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<guint>::max());
        timeout_source_ = g_timeout_source_new(static_cast<guint>(std::min(count, maximum)));
        auto* timeout_request = new std::shared_ptr<LinuxHttpRequest>(shared_from_this());
        g_source_set_callback(
            timeout_source_,
            [](gpointer data) -> gboolean {
              (*static_cast<std::shared_ptr<LinuxHttpRequest>*>(data))->Timeout();
              return G_SOURCE_REMOVE;
            },
            timeout_request,
            [](gpointer data) { delete static_cast<std::shared_ptr<LinuxHttpRequest>*>(data); }
        );
        g_source_attach(timeout_source_, g_main_context_get_thread_default());
      }

      std::function<void(std::uint64_t)> upload_progress;
      {
        std::scoped_lock lock(mutex_);
        if (!finished_) {
          upload_progress = callbacks_.upload_progress;
        }
      }
      if (!request_.body.empty() && upload_progress) {
        upload_progress(static_cast<std::uint64_t>(request_.body.size()));
      }
      native_pending_ = true;
      auto* callback_request = new std::shared_ptr<LinuxHttpRequest>(shared_from_this());
      soup_session_send_async(
          session,
          message_,
          G_PRIORITY_DEFAULT,
          cancellable_,
          [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::shared_ptr<LinuxHttpRequest>> request(
                static_cast<std::shared_ptr<LinuxHttpRequest>*>(data)
            );
            (*request)->ReceiveHeaders(SOUP_SESSION(source), result);
          },
          callback_request
      );
    } catch (const std::exception& error) {
      FinishWithoutNative(HttpError{
          HttpErrorCode::Transport,
          "HuxerUI Linux HTTP request failed: " + std::string(error.what()),
      });
    } catch (...) {
      FinishWithoutNative(HttpError{HttpErrorCode::Transport, "HuxerUI Linux HTTP request failed"});
    }
  }

  void RequestRead() override {
    if (const std::shared_ptr<LinuxHttpTransportState> transport = transport_.lock()) {
      const std::shared_ptr<LinuxHttpRequest> request = shared_from_this();
      transport->Post([request] { request->ReadOnNetworkThread(); });
    }
  }

  void Cancel() noexcept override {
    if (!SuppressCompletion()) {
      return;
    }
    if (const std::shared_ptr<LinuxHttpTransportState> transport = transport_.lock()) {
      const std::shared_ptr<LinuxHttpRequest> request = shared_from_this();
      transport->Post([request] { request->CancelOnNetworkThread(); });
    }
  }

  void CancelOnNetworkThread() noexcept {
    {
      std::scoped_lock lock(mutex_);
      finished_ = true;
      callbacks_ = {};
    }
    g_cancellable_cancel(cancellable_);
    if (!native_pending_) {
      CleanupNative();
      RemoveFromTransport();
    }
  }

private:
  [[nodiscard]] bool SuppressCompletion() noexcept {
    std::scoped_lock lock(mutex_);
    if (finished_) {
      return false;
    }
    finished_ = true;
    return true;
  }

  void SetRequestBody(SoupMessage* message) noexcept {
    if (g_strcmp0(soup_message_get_method(message), HttpMethodName(request_.method)) != 0) {
      return;
    }
    GBytes* body = g_bytes_new_static(request_.body.data(), request_.body.size());
    soup_message_set_request_body_from_bytes(message, nullptr, body);
    g_bytes_unref(body);
  }

  void Timeout() noexcept {
    HttpTransportCallbacks callbacks;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      callbacks = std::move(callbacks_);
    }
    if (callbacks.error) {
      callbacks.error(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"});
    }
    g_cancellable_cancel(cancellable_);
    if (!native_pending_) {
      CleanupNative();
      RemoveFromTransport();
    }
  }

  void ReceiveHeaders(SoupSession* session, GAsyncResult* result) noexcept {
    GError* error = nullptr;
    GInputStream* stream = soup_session_send_finish(session, result, &error);
    native_pending_ = false;
    if (error != nullptr || stream == nullptr) {
      const HttpErrorCode code = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)
                                     ? HttpErrorCode::Timeout
                                     : HttpErrorCode::Transport;
      const HttpError request_error{code, ErrorMessage(error)};
      if (stream != nullptr) {
        g_object_unref(stream);
      }
      if (error != nullptr) {
        g_error_free(error);
      }
      FinishTerminal(request_error);
      return;
    }
    input_stream_ = stream;

    if (Finished()) {
      CleanupNative();
      RemoveFromTransport();
      return;
    }

    try {
      HttpTransportResponse response{
          .url = {},
          .status_code = static_cast<int>(soup_message_get_status(message_)),
          .headers = {},
          .body_size = std::nullopt,
      };
      if (GUri* uri = soup_message_get_uri(message_)) {
        const std::unique_ptr<char, decltype(&g_free)> text(g_uri_to_string(uri), g_free);
        if (text) {
          response.url = text.get();
        }
      }
      SoupMessageHeadersIter iterator;
      soup_message_headers_iter_init(&iterator, soup_message_get_response_headers(message_));
      const char* name = nullptr;
      const char* value = nullptr;
      while (soup_message_headers_iter_next(&iterator, &name, &value)) {
        response.headers.push_back({name != nullptr ? name : "", value != nullptr ? value : ""});
      }
      if (request_.method == HttpMethod::Head || (response.status_code >= 100 && response.status_code < 200) ||
          response.status_code == 204 || response.status_code == 304) {
        response.body_size = 0;
      } else {
        response.body_size = ParseContentLength(response.headers);
      }
      std::function<void(HttpTransportResponse)> response_callback;
      {
        std::scoped_lock lock(mutex_);
        if (!finished_) {
          response_callback = callbacks_.response;
        }
      }
      if (response_callback) {
        response_callback(std::move(response));
      }
    } catch (...) {
      FinishTerminal(HttpError{HttpErrorCode::Transport, "HuxerUI Linux HTTP response could not be converted"});
    }
  }

  void ReadOnNetworkThread() noexcept {
    if (Finished() || input_stream_ == nullptr || native_pending_) {
      return;
    }
    native_pending_ = true;
    auto* callback_request = new std::shared_ptr<LinuxHttpRequest>(shared_from_this());
    g_input_stream_read_bytes_async(
        input_stream_,
        read_buffer_size,
        G_PRIORITY_DEFAULT,
        cancellable_,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::shared_ptr<LinuxHttpRequest>> request(
              static_cast<std::shared_ptr<LinuxHttpRequest>*>(data)
          );
          (*request)->ReceiveBody(G_INPUT_STREAM(source), result);
        },
        callback_request
    );
  }

  void ReceiveBody(GInputStream* stream, GAsyncResult* result) noexcept {
    GError* error = nullptr;
    GBytes* bytes = g_input_stream_read_bytes_finish(stream, result, &error);
    native_pending_ = false;
    if (Finished()) {
      if (bytes != nullptr) {
        g_bytes_unref(bytes);
      }
      if (error != nullptr) {
        g_error_free(error);
      }
      CleanupNative();
      RemoveFromTransport();
      return;
    }
    if (error != nullptr || bytes == nullptr) {
      const HttpErrorCode code = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)
                                     ? HttpErrorCode::Timeout
                                     : HttpErrorCode::Transport;
      HttpError request_error{code, ErrorMessage(error)};
      if (bytes != nullptr) {
        g_bytes_unref(bytes);
      }
      if (error != nullptr) {
        g_error_free(error);
      }
      FinishTerminal(std::move(request_error));
      return;
    }

    gsize size = 0;
    const auto* data = static_cast<const std::byte*>(g_bytes_get_data(bytes, &size));
    if (size == 0) {
      g_bytes_unref(bytes);
      FinishComplete();
      return;
    }
    Bytes body(data, data + size);
    g_bytes_unref(bytes);
    std::function<void(Bytes)> body_callback;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_) {
        body_callback = callbacks_.body;
      }
    }
    if (body_callback) {
      body_callback(std::move(body));
    }
  }

  void FinishComplete() noexcept {
    HttpTransportCallbacks callbacks;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      callbacks = std::move(callbacks_);
    }
    CleanupNative();
    if (callbacks.complete) {
      callbacks.complete();
    }
    RemoveFromTransport();
  }

  void FinishTerminal(HttpError error) noexcept {
    HttpTransportCallbacks callbacks;
    bool notify = false;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_) {
        finished_ = true;
        callbacks = std::move(callbacks_);
        notify = true;
      }
    }
    CleanupNative();
    if (notify && callbacks.error) {
      callbacks.error(std::move(error));
    }
    RemoveFromTransport();
  }

  void FinishWithoutNative(HttpError error) noexcept {
    native_pending_ = false;
    FinishTerminal(std::move(error));
  }

  void RemoveFromTransport() noexcept {
    if (removed_) {
      return;
    }
    removed_ = true;
    if (const std::shared_ptr<LinuxHttpTransportState> transport = transport_.lock()) {
      transport->Remove(this);
    }
  }

  void CleanupNative() noexcept {
    if (timeout_source_ != nullptr) {
      g_source_destroy(timeout_source_);
      g_source_unref(timeout_source_);
      timeout_source_ = nullptr;
    }
    g_clear_object(&input_stream_);
    g_clear_object(&message_);
  }

  std::weak_ptr<LinuxHttpTransportState> transport_;
  HttpRequest request_;
  mutable std::mutex mutex_;
  HttpTransportCallbacks callbacks_;
  GCancellable* cancellable_ = nullptr;
  SoupMessage* message_ = nullptr;
  GInputStream* input_stream_ = nullptr;
  GSource* timeout_source_ = nullptr;
  bool native_pending_ = false;
  bool removed_ = false;
  bool finished_ = false;
};

void LinuxHttpTransportState::Queue(std::shared_ptr<LinuxHttpRequest> request) {
  requests_.push_back(request);
  if (stopping_ || request->Finished()) {
    request->CancelOnNetworkThread();
    return;
  }
  request->Start(session_);
}

void LinuxHttpTransportState::Remove(LinuxHttpRequest* request) noexcept {
  std::erase_if(requests_, [request](const std::shared_ptr<LinuxHttpRequest>& item) { return item.get() == request; });
  MaybeQuit();
}

void LinuxHttpTransportState::StopOnNetworkThread() noexcept {
  if (stopping_) {
    return;
  }
  stopping_ = true;
  const std::vector requests = requests_;
  for (const std::shared_ptr<LinuxHttpRequest>& request : requests) {
    request->CancelOnNetworkThread();
  }
  MaybeQuit();
}

class LinuxHttpTransport final : public HttpTransport {
public:
  LinuxHttpTransport() : state_(LinuxHttpTransportState::Create()) {}

  ~LinuxHttpTransport() override {
    state_->Stop();
  }

  std::shared_ptr<HttpTransportOperation>
  Start(HttpRequest request, bool, HttpTransportCallbacks callbacks) override {
    auto operation = std::make_shared<LinuxHttpRequest>(state_, std::move(request), std::move(callbacks));
    LinuxHttpTransportState* state = state_.get();
    state->Post([state, operation] { state->Queue(operation); });
    return operation;
  }

private:
  std::shared_ptr<LinuxHttpTransportState> state_;
};

} // namespace

std::shared_ptr<HttpTransport> CreateLinuxHttpTransport() {
  return std::make_shared<LinuxHttpTransport>();
}

} // namespace huxerui::detail
