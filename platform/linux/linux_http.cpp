#include "linux_internal.h"

#include "linux_http_internal.h"

#include <libsoup/soup.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "http_internal.h"

namespace huxerui::detail {

namespace {

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

class LinuxHttpRequest final : public std::enable_shared_from_this<LinuxHttpRequest> {
public:
  LinuxHttpRequest(
      std::weak_ptr<LinuxHttpTransportState> transport, HttpRequest request, HttpTransportCompletion completion
  )
      : transport_(std::move(transport)), request_(std::move(request)), completion_(std::move(completion)),
        cancellable_(g_cancellable_new()) {}

  ~LinuxHttpRequest() {
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
        FinishWithoutNative(HttpResult(
            HttpError{
                HttpErrorCode::Transport,
                "HuxerUI Linux HTTP URL could not be parsed",
            }
        ));
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

      auto* callback_request = new std::shared_ptr<LinuxHttpRequest>(shared_from_this());
      soup_session_send_and_read_async(
          session,
          message_,
          G_PRIORITY_DEFAULT,
          cancellable_,
          [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::shared_ptr<LinuxHttpRequest>> request(
                static_cast<std::shared_ptr<LinuxHttpRequest>*>(data)
            );
            (*request)->Complete(SOUP_SESSION(source), result);
          },
          callback_request
      );
    } catch (const std::exception& error) {
      FinishWithoutNative(HttpResult(
          HttpError{
              HttpErrorCode::Transport,
              "HuxerUI Linux HTTP request failed: " + std::string(error.what()),
          }
      ));
    } catch (...) {
      FinishWithoutNative(HttpResult(
          HttpError{
              HttpErrorCode::Transport,
              "HuxerUI Linux HTTP request failed",
          }
      ));
    }
  }

  void Cancel() {
    if (!SuppressCompletion()) {
      return;
    }
    if (const std::shared_ptr<LinuxHttpTransportState> transport = transport_.lock()) {
      const std::shared_ptr<LinuxHttpRequest> request = shared_from_this();
      // Keep libsoup request cancellation on the network context that owns its completion and cleanup.
      transport->Post([request] { request->CancelSoupRequest(); });
    }
  }

  void CancelOnNetworkThread() noexcept {
    static_cast<void>(SuppressCompletion());
    CancelSoupRequest();
  }

private:
  [[nodiscard]] bool SuppressCompletion() noexcept {
    std::scoped_lock lock(mutex_);
    if (finished_) {
      return false;
    }
    finished_ = true;
    completion_ = {};
    return true;
  }

  void CancelSoupRequest() noexcept {
    g_cancellable_cancel(cancellable_);
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
    HttpTransportCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion = std::move(completion_);
    }
    if (completion) {
      completion(HttpResult(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"}));
    }
    g_cancellable_cancel(cancellable_);
  }

  void Complete(SoupSession* session, GAsyncResult* result) noexcept {
    GError* error = nullptr;
    GBytes* bytes = soup_session_send_and_read_finish(session, result, &error);
    HttpResult request_result = BuildResult(bytes, error);
    if (bytes != nullptr) {
      g_bytes_unref(bytes);
    }
    if (error != nullptr) {
      g_error_free(error);
    }

    CleanupNative();

    HttpTransportCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_) {
        finished_ = true;
        completion = std::move(completion_);
      }
    }
    if (completion) {
      completion(std::move(request_result));
    }
    RemoveFromTransport();
  }

  HttpResult BuildResult(GBytes* bytes, const GError* error) const noexcept {
    try {
      if (error != nullptr) {
        const HttpErrorCode code = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ? HttpErrorCode::Timeout
                                                                                            : HttpErrorCode::Transport;
        return HttpResult(HttpError{code, ErrorMessage(error)});
      }
      if (bytes == nullptr || message_ == nullptr) {
        return HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI Linux HTTP response is invalid"});
      }

      HttpResponse response{
          .url = {},
          .status_code = static_cast<int>(soup_message_get_status(message_)),
          .headers = {},
          .body = {},
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

      gsize size = 0;
      const auto* data = static_cast<const std::byte*>(g_bytes_get_data(bytes, &size));
      if (data != nullptr && size != 0) {
        response.body.assign(data, data + size);
      }
      return HttpResult(std::move(response));
    } catch (...) {
      return HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI Linux HTTP response could not be converted"});
    }
  }

  void FinishWithoutNative(HttpResult result) noexcept {
    CleanupNative();
    HttpTransportCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_) {
        finished_ = true;
        completion = std::move(completion_);
      }
    }
    if (completion) {
      completion(std::move(result));
    }
    RemoveFromTransport();
  }

  void RemoveFromTransport() noexcept {
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
    g_clear_object(&message_);
  }

  std::weak_ptr<LinuxHttpTransportState> transport_;
  HttpRequest request_;
  mutable std::mutex mutex_;
  HttpTransportCompletion completion_;
  GCancellable* cancellable_ = nullptr;
  SoupMessage* message_ = nullptr;
  GSource* timeout_source_ = nullptr;
  bool finished_ = false;
};

void LinuxHttpTransportState::Queue(std::shared_ptr<LinuxHttpRequest> request) {
  requests_.push_back(request);
  if (stopping_ || request->Finished()) {
    request->CancelOnNetworkThread();
    Remove(request.get());
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
  for (const std::shared_ptr<LinuxHttpRequest>& request : requests_) {
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

  std::function<void()> Start(HttpRequest request, HttpTransportCompletion completion) override {
    auto operation = std::make_shared<LinuxHttpRequest>(state_, std::move(request), std::move(completion));
    LinuxHttpTransportState* state = state_.get();
    state->Post([state, operation] { state->Queue(operation); });
    return [operation] { operation->Cancel(); };
  }

private:
  std::shared_ptr<LinuxHttpTransportState> state_;
};

} // namespace

std::shared_ptr<HttpTransport> CreateLinuxHttpTransport() {
  return std::make_shared<LinuxHttpTransport>();
}

} // namespace huxerui::detail
