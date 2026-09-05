#include "runtime_test_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "io/http_internal.h"

namespace huxerui::test {

namespace {

Bytes BytesFromString(std::string_view value) {
  Bytes bytes;
  bytes.reserve(value.size());
  for (const char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  return bytes;
}

class ManualHttpOperation final : public detail::HttpTransportOperation {
public:
  void RequestRead() override {
    ++read_requests;
  }

  void Cancel() noexcept override {
    canceled = true;
  }

  std::atomic<std::size_t> read_requests = 0;
  std::atomic<bool> canceled = false;
};

class ManualHttpTransport final : public detail::HttpTransport {
public:
  std::shared_ptr<detail::HttpTransportOperation>
  Start(HttpRequest request, bool require_incremental_response, detail::HttpTransportCallbacks callbacks) override {
    auto operation = std::make_shared<ManualHttpOperation>();
    std::optional<HttpError> immediate_error;
    std::function<void(HttpError)> error_callback;
    {
      std::scoped_lock lock(mutex_);
      calls_.push_back({std::move(request), require_incremental_response, std::move(callbacks), operation});
      if (immediate_error_.has_value()) {
        immediate_error = std::exchange(immediate_error_, std::nullopt);
        error_callback = calls_.back().callbacks.error;
      }
    }
    if (immediate_error.has_value()) {
      error_callback(std::move(*immediate_error));
    }
    return operation;
  }

  void FailNextStart(HttpError error) {
    std::scoped_lock lock(mutex_);
    immediate_error_ = std::move(error);
  }

  [[nodiscard]] std::size_t CallCount() const {
    std::scoped_lock lock(mutex_);
    return calls_.size();
  }

  [[nodiscard]] HttpRequest Request(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return calls_.at(index).request;
  }

  [[nodiscard]] bool RequiresIncrementalResponse(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return calls_.at(index).require_incremental_response;
  }

  [[nodiscard]] bool Canceled(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return calls_.at(index).operation->canceled;
  }

  [[nodiscard]] std::size_t ReadRequests(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return calls_.at(index).operation->read_requests;
  }

  void Upload(std::size_t index, std::uint64_t transferred_bytes) {
    Callbacks(index).upload_progress(transferred_bytes);
  }

  void Respond(std::size_t index, detail::HttpTransportResponse response) {
    Callbacks(index).response(std::move(response));
  }

  void Body(std::size_t index, Bytes body) {
    Callbacks(index).body(std::move(body));
  }

  void Complete(std::size_t index) {
    Callbacks(index).complete();
  }

  void Error(std::size_t index, HttpError error) {
    Callbacks(index).error(std::move(error));
  }

private:
  struct Call {
    HttpRequest request;
    bool require_incremental_response = false;
    detail::HttpTransportCallbacks callbacks;
    std::shared_ptr<ManualHttpOperation> operation;
  };

  [[nodiscard]] detail::HttpTransportCallbacks Callbacks(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return calls_.at(index).callbacks;
  }

  mutable std::mutex mutex_;
  std::vector<Call> calls_;
  std::optional<HttpError> immediate_error_;
};

class HttpTestPlatform final : public TestPlatform {
public:
  std::shared_ptr<ManualHttpTransport> transport = std::make_shared<ManualHttpTransport>();

protected:
  std::shared_ptr<detail::HttpTransport> CreateHttpTransport() override {
    return transport;
  }
};

std::shared_ptr<HttpClient> http_client;
TaskScope http_tasks;
std::optional<HttpResponse> http_response;
std::optional<HttpErrorCode> http_error;
std::optional<HttpResponseStream> http_stream;
std::optional<HttpStreamReadResult> http_read;
std::vector<HttpProgress> http_progress;
std::vector<std::thread::id> http_progress_threads;
std::thread::id http_resume_thread;
int http_completions = 0;
bool http_progress_exception = false;

View HttpApp() {
  http_client = UseService<HttpClient>();
  http_tasks = UseTaskScope();
  return Text("HTTP");
}

void ResetHttpState() {
  http_client.reset();
  http_tasks = {};
  http_response.reset();
  http_error.reset();
  http_stream.reset();
  http_read.reset();
  http_progress.clear();
  http_progress_threads.clear();
  http_resume_thread = {};
  http_completions = 0;
  http_progress_exception = false;
}

Task<void> CaptureHttpResult(
    std::shared_ptr<HttpClient> client,
    HttpRequest request,
    std::function<void(HttpProgress)> progress = {}
) {
  HttpResult result = co_await client->Send(std::move(request), std::move(progress));
  if (result.HasResponse()) {
    http_response = std::move(result).Response();
  } else {
    http_error = result.Error().code;
  }
  http_resume_thread = std::this_thread::get_id();
  ++http_completions;
}

Task<void> CaptureHttpStream(std::shared_ptr<HttpClient> client, HttpRequest request) {
  HttpStreamResult result = co_await client->SendStream(std::move(request));
  if (result.HasResponse()) {
    http_stream.emplace(std::move(result).Response());
  } else {
    http_error = result.Error().code;
  }
  ++http_completions;
}

Task<void> CaptureHttpRead() {
  http_read = co_await http_stream->Read();
  ++http_completions;
}

Task<void> CaptureHttpProgressException(std::shared_ptr<HttpClient> client) {
  try {
    static_cast<void>(co_await client->Send(
        {.url = "https://example.test/progress-error", .method = HttpMethod::Post, .body = BytesFromString("body")},
        [](HttpProgress) { throw std::runtime_error("progress failed"); }
    ));
  } catch (const std::runtime_error& error) {
    http_progress_exception = std::string_view(error.what()) == "progress failed";
  }
}

} // namespace

TEST_CASE("HttpResultTypesDistinguishTheirAlternatives") {
  HttpResult response_result(HttpResponse{.url = "https://example.test/unavailable", .status_code = 503});
  REQUIRE(response_result.HasResponse());
  REQUIRE(response_result.Response().status_code == 503);
  REQUIRE_THROWS_AS(response_result.Error(), std::logic_error);

  HttpResult error_result(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"});
  REQUIRE_FALSE(error_result.HasResponse());
  REQUIRE(error_result.Error().code == HttpErrorCode::Timeout);
  REQUIRE_THROWS_AS(error_result.Response(), std::logic_error);

  HttpStreamReadResult data(BytesFromString("data"));
  REQUIRE(data.HasData());
  REQUIRE_FALSE(data.IsComplete());
  REQUIRE_THROWS_AS(data.Error(), std::logic_error);
  REQUIRE(HttpStreamReadResult::Complete().IsComplete());
  REQUIRE_THROWS_AS(HttpStreamReadResult(Bytes{}), std::invalid_argument);
  REQUIRE_THROWS_AS(HttpStreamReadResult(Bytes(64U * 1024U + 1U)), std::invalid_argument);
}

TEST_CASE("HttpClientSendsOneTypedOperationAndAggregatesItsPullReads") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();
  const Bytes request_body{std::byte{'{'}, std::byte{0}, std::byte{0xFF}, std::byte{'}'}};

  http_tasks.Launch(CaptureHttpResult(
      http_client,
      {
          .url = "https://example.test/items",
          .method = HttpMethod::Post,
          .headers = {{"Accept", "application/json"}, {"X-Trace", "first"}, {"X-Trace", "second"}},
          .body = request_body,
          .timeout = std::chrono::milliseconds{5000},
      }
  ));
  platform.RunPlatformModuleTasks();

  REQUIRE(platform.transport->CallCount() == 1);
  REQUIRE_FALSE(platform.transport->RequiresIncrementalResponse(0));
  const HttpRequest request = platform.transport->Request(0);
  REQUIRE(request.url == "https://example.test/items");
  REQUIRE(request.method == HttpMethod::Post);
  REQUIRE(request.body == request_body);

  std::thread response([&platform] {
    platform.transport->Respond(
        0,
        {
            .url = "https://example.test/items/42",
            .status_code = 201,
            .headers = {{"Content-Type", "application/json"}},
            .body_size = 4,
        }
    );
  });
  response.join();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->ReadRequests(0) == 1);

  platform.transport->Body(0, Bytes{std::byte{'{'}, std::byte{0}, std::byte{0xFF}, std::byte{'}'}});
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->ReadRequests(0) == 2);
  platform.transport->Complete(0);
  platform.RunPlatformModuleTasks();

  REQUIRE(http_response.has_value());
  REQUIRE(http_response->status_code == 201);
  REQUIRE((http_response->body == Bytes{std::byte{'{'}, std::byte{0}, std::byte{0xFF}, std::byte{'}'}}));
  REQUIRE(http_resume_thread == ui_thread);
  REQUIRE_FALSE(platform.transport->Canceled(0));
}

TEST_CASE("HttpClientHandlesTransportCompletionDuringStart") {
  ResetHttpState();
  HttpTestPlatform platform;
  platform.transport->FailNextStart({HttpErrorCode::Transport, "HuxerUI HTTP request failed immediately"});
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpResult(http_client, {.url = "https://example.test/immediate"}));
  platform.RunPlatformModuleTasks();

  REQUIRE(http_error == HttpErrorCode::Transport);
  REQUIRE(http_completions == 1);
  REQUIRE_FALSE(platform.transport->Canceled(0));
}

TEST_CASE("HttpClientRejectsBodyBeforeResponseMetadata") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/body-before-response"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Body(0, BytesFromString("invalid"));
  platform.RunPlatformModuleTasks();

  REQUIRE(http_error == HttpErrorCode::Transport);
  REQUIRE(http_completions == 1);
  REQUIRE(platform.transport->Canceled(0));

  platform.transport->Respond(0, {.url = "https://example.test/body-before-response", .status_code = 200});
  platform.transport->Complete(0);
  platform.RunPlatformModuleTasks();
  REQUIRE(http_completions == 1);
}

TEST_CASE("HttpClientRejectsCompletionBeforeResponseMetadata") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/complete-before-response"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Complete(0);
  platform.RunPlatformModuleTasks();

  REQUIRE(http_error == HttpErrorCode::Transport);
  REQUIRE(http_completions == 1);
  REQUIRE_FALSE(platform.transport->Canceled(0));

  platform.transport->Respond(0, {.url = "https://example.test/complete-before-response", .status_code = 200});
  platform.RunPlatformModuleTasks();
  REQUIRE(http_completions == 1);
}

TEST_CASE("HttpClientReturnsStreamingHeadersBeforeRequestingBodyData") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/stream"}));
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->RequiresIncrementalResponse(0));

  platform.transport->Respond(
      0,
      {
          .url = "https://example.test/final",
          .status_code = 206,
          .headers = {{"Content-Type", "application/octet-stream"}},
      }
  );
  platform.RunPlatformModuleTasks();

  REQUIRE(http_stream.has_value());
  REQUIRE(http_stream->Url() == "https://example.test/final");
  REQUIRE(http_stream->StatusCode() == 206);
  REQUIRE(platform.transport->ReadRequests(0) == 0);

  http_tasks.Launch(CaptureHttpRead());
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->ReadRequests(0) == 1);
  REQUIRE_FALSE(http_read.has_value());

  platform.transport->Body(0, BytesFromString("first"));
  platform.RunPlatformModuleTasks();
  REQUIRE(http_read.has_value());
  REQUIRE(std::move(*http_read).Data() == BytesFromString("first"));
}

TEST_CASE("HttpResponseStreamSplitsLargeTransportChunksWithoutExtraPlatformReads") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/large"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Respond(0, {.url = "https://example.test/large", .status_code = 200});
  platform.RunPlatformModuleTasks();

  http_tasks.Launch(CaptureHttpRead());
  platform.RunPlatformModuleTasks();
  platform.transport->Body(0, Bytes(64U * 1024U + 17U, std::byte{7}));
  platform.RunPlatformModuleTasks();
  REQUIRE(http_read->Data().size() == 64U * 1024U);
  REQUIRE(platform.transport->ReadRequests(0) == 1);

  http_read.reset();
  http_tasks.Launch(CaptureHttpRead());
  platform.RunPlatformModuleTasks();
  REQUIRE(http_read->Data().size() == 17);
  REQUIRE(platform.transport->ReadRequests(0) == 1);

  http_read.reset();
  http_tasks.Launch(CaptureHttpRead());
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->ReadRequests(0) == 2);
  platform.transport->Complete(0);
  platform.RunPlatformModuleTasks();
  REQUIRE(http_read->IsComplete());
  REQUIRE_THROWS_AS(static_cast<void>(http_stream->Read()), std::logic_error);
}

TEST_CASE("HttpResponseStreamAllowsOneReadAndCancelsAnAbandonedRead") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/abandoned"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Respond(0, {.url = "https://example.test/abandoned", .status_code = 200});
  platform.RunPlatformModuleTasks();

  {
    Task<HttpStreamReadResult> read = http_stream->Read();
    REQUIRE_THROWS_AS(static_cast<void>(http_stream->Read()), std::logic_error);
  }
  REQUIRE(platform.transport->Canceled(0));
}

TEST_CASE("HttpResponseStreamCancelsOnDestructionAndRejectsMovedFromAccess") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/moved"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Respond(0, {.url = "https://example.test/moved", .status_code = 200});
  platform.RunPlatformModuleTasks();

  {
    HttpResponseStream stream = std::move(*http_stream);
    REQUIRE_THROWS_AS(static_cast<void>(http_stream->Url()), std::logic_error);
    REQUIRE_FALSE(platform.transport->Canceled(0));
  }
  REQUIRE(platform.transport->Canceled(0));
}

TEST_CASE("HttpProgressIsMonotonicAndRunsOnTheRuntimeUIThread") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();

  http_tasks.Launch(CaptureHttpResult(
      http_client,
      {.url = "https://example.test/progress", .method = HttpMethod::Post, .body = BytesFromString("body")},
      [](HttpProgress progress) {
        http_progress.push_back(std::move(progress));
        http_progress_threads.push_back(std::this_thread::get_id());
      }
  ));
  platform.RunPlatformModuleTasks();

  std::thread transport([&platform] {
    platform.transport->Upload(0, 2);
    platform.transport->Upload(0, 1);
    platform.transport->Upload(0, 9);
    platform.transport->Respond(
        0,
        {.url = "https://example.test/progress", .status_code = 200, .body_size = 3}
    );
  });
  transport.join();
  platform.RunPlatformModuleTasks();
  platform.transport->Body(0, BytesFromString("abc"));
  platform.RunPlatformModuleTasks();
  platform.transport->Body(0, BytesFromString("d"));
  platform.RunPlatformModuleTasks();
  platform.transport->Complete(0);
  platform.RunPlatformModuleTasks();

  REQUIRE(
      http_progress ==
      std::vector<HttpProgress>{
          {HttpProgressKind::Upload, 4, 4},
          {HttpProgressKind::Download, 3, 3},
          {HttpProgressKind::Download, 4, std::nullopt},
      }
  );
  REQUIRE(std::all_of(http_progress_threads.begin(), http_progress_threads.end(), [ui_thread](std::thread::id thread) {
    return thread == ui_thread;
  }));
}

TEST_CASE("HttpProgressExceptionsCancelAndRethrowFromTheTask") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpProgressException(http_client));
  platform.RunPlatformModuleTasks();
  platform.transport->Upload(0, 1);
  platform.RunPlatformModuleTasks();

  REQUIRE(http_progress_exception);
  REQUIRE(platform.transport->Canceled(0));
}

TEST_CASE("HttpErrorsRemainOnTheSideOfTheResponseBoundaryWhereTheyOccur") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/pre-head"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Error(0, {HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"});
  platform.RunPlatformModuleTasks();
  REQUIRE(http_error == HttpErrorCode::Timeout);

  http_error.reset();
  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test/post-head"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Respond(1, {.url = "https://example.test/post-head", .status_code = 503});
  platform.RunPlatformModuleTasks();
  REQUIRE(http_stream->StatusCode() == 503);

  http_tasks.Launch(CaptureHttpRead());
  platform.RunPlatformModuleTasks();
  platform.transport->Error(1, {HttpErrorCode::Transport, "HuxerUI HTTP stream failed"});
  platform.RunPlatformModuleTasks();
  REQUIRE(http_read->HasError());
  REQUIRE(http_read->Error().code == HttpErrorCode::Transport);
}

TEST_CASE("HttpClientCancellationStopsTheSinglePlatformOperationAndDropsLateEvents") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  TaskHandle request = http_tasks.Launch(CaptureHttpResult(http_client, {.url = "https://example.test/slow"}));
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(platform.transport->Canceled(0));

  request.Cancel();
  REQUIRE(platform.transport->Canceled(0));
  platform.transport->Respond(0, {.url = "https://example.test/slow", .status_code = 200});
  platform.transport->Complete(0);
  platform.RunPlatformModuleTasks();
  REQUIRE(http_completions == 0);
}

TEST_CASE("HttpClientValidatesPortableRequestConfigurationBeforeLaunch") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  REQUIRE_NOTHROW(static_cast<void>(http_client->Send({.url = "HTTPS://example.test"})));
  REQUIRE_THROWS_AS(static_cast<void>(http_client->Send({.url = "file:///tmp/value"})), std::invalid_argument);
  REQUIRE_THROWS_AS(
      static_cast<void>(http_client->Send({.url = "https://example.test", .body = BytesFromString("body")})),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      static_cast<void>(http_client->Send({
          .url = "https://example.test",
          .timeout = std::chrono::milliseconds::zero(),
      })),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      static_cast<void>(http_client->Send({
          .url = "https://example.test",
          .headers = {{"Invalid Header", "value"}},
      })),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      static_cast<void>(http_client->SendStream({
          .url = "https://example.test",
          .headers = {{"X-Test", "first\r\nsecond"}},
      })),
      std::invalid_argument
  );
  REQUIRE(platform.transport->CallCount() == 0);
}

TEST_CASE("HttpClientReportsUnsupportedAdaptersThroughBothTaskShapes") {
  ResetHttpState();
  TestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpResult(http_client, {.url = "https://example.test"}));
  platform.RunPlatformModuleTasks();
  REQUIRE(http_error == HttpErrorCode::Unsupported);

  http_error.reset();
  http_tasks.Launch(CaptureHttpStream(http_client, {.url = "https://example.test"}));
  platform.RunPlatformModuleTasks();
  REQUIRE(http_error == HttpErrorCode::Unsupported);
}

} // namespace huxerui::test
