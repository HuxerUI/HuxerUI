#include "runtime_test_support.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "http_internal.h"

namespace huxerui::test {

namespace {

class ManualHttpTransport final : public detail::HttpTransport {
public:
  std::function<void()> Start(HttpRequest request, detail::HttpTransportCompletion completion) override {
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    detail::HttpTransportCompletion immediate_completion;
    std::optional<HttpResult> immediate_result;
    {
      std::scoped_lock lock(mutex_);
      calls_.push_back({std::move(request), std::move(completion), canceled});
      if (immediate_result_.has_value()) {
        immediate_completion = calls_.back().completion;
        immediate_result = std::move(immediate_result_);
        immediate_result_.reset();
      }
    }
    if (immediate_result.has_value()) {
      immediate_completion(std::move(*immediate_result));
    }
    return [canceled] { *canceled = true; };
  }

  void CompleteImmediately(HttpResult result) {
    std::scoped_lock lock(mutex_);
    immediate_result_ = std::move(result);
  }

  [[nodiscard]] std::size_t CallCount() const {
    std::scoped_lock lock(mutex_);
    return calls_.size();
  }

  [[nodiscard]] HttpRequest Request(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return calls_.at(index).request;
  }

  [[nodiscard]] bool Canceled(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return *calls_.at(index).canceled;
  }

  void Complete(std::size_t index, HttpResult result) {
    detail::HttpTransportCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      completion = calls_.at(index).completion;
    }
    completion(std::move(result));
  }

private:
  struct Call {
    HttpRequest request;
    detail::HttpTransportCompletion completion;
    std::shared_ptr<std::atomic<bool>> canceled;
  };

  mutable std::mutex mutex_;
  std::vector<Call> calls_;
  std::optional<HttpResult> immediate_result_;
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
std::thread::id http_resume_thread;
int http_completions = 0;

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
  http_resume_thread = {};
  http_completions = 0;
}

Task<void> CaptureHttpResult(std::shared_ptr<HttpClient> client, HttpRequest request) {
  HttpResult result = co_await client->Send(std::move(request));
  if (result.HasResponse()) {
    http_response = std::move(result).Response();
  } else {
    http_error = result.Error().code;
  }
  http_resume_thread = std::this_thread::get_id();
}

Task<void> CaptureHttpResponse(std::shared_ptr<HttpClient> client, HttpRequest request) {
  HttpResult result = co_await client->Send(std::move(request));
  if (result.HasResponse()) {
    http_response = std::move(result).Response();
  }
  ++http_completions;
}

Task<void> CaptureHttpError(std::shared_ptr<HttpClient> client, HttpRequest request) {
  HttpResult result = co_await client->Send(std::move(request));
  if (!result.HasResponse()) {
    http_error = result.Error().code;
  }
}

Task<void> CountHttpCompletion(std::shared_ptr<HttpClient> client, HttpRequest request) {
  static_cast<void>(co_await client->Send(std::move(request)));
  ++http_completions;
}

} // namespace

TEST_CASE("HttpResultDistinguishesResponsesFromTransportErrors") {
  HttpResult response_result(HttpResponse{
      .url = "https://example.test/unavailable",
      .status_code = 503,
  });
  REQUIRE(response_result.HasResponse());
  REQUIRE(response_result.Response().status_code == 503);
  REQUIRE_THROWS_AS(response_result.Error(), std::logic_error);

  HttpResult error_result(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"});
  REQUIRE_FALSE(error_result.HasResponse());
  REQUIRE(error_result.Error().code == HttpErrorCode::Timeout);
  REQUIRE_THROWS_AS(error_result.Response(), std::logic_error);
}

TEST_CASE("HttpClientSendsTypedRequestsAndResumesWithResponsesOnTheUIThread") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();

  http_tasks.Launch(CaptureHttpResult(
      http_client,
      {
          .url = "https://example.test/items",
          .method = HttpMethod::Post,
          .headers =
              {
                  {"Accept", "application/json"},
                  {"X-Trace", "first"},
                  {"X-Trace", "second"},
              },
          .body = "{}",
          .timeout = std::chrono::milliseconds{5000},
      }
  ));
  platform.RunPlatformModuleTasks();

  REQUIRE(platform.transport->CallCount() == 1);
  const HttpRequest request = platform.transport->Request(0);
  REQUIRE(request.url == "https://example.test/items");
  REQUIRE(request.method == HttpMethod::Post);
  REQUIRE(
      (request.headers ==
       std::vector<HttpHeader>{
           {"Accept", "application/json"},
           {"X-Trace", "first"},
           {"X-Trace", "second"},
       })
  );
  REQUIRE(request.body == "{}");
  REQUIRE(request.timeout == std::chrono::milliseconds{5000});

  std::thread completion([&platform] {
    platform.transport->Complete(
        0,
        HttpResult(HttpResponse{
            .url = "https://example.test/items/42",
            .status_code = 201,
            .headers = {{"Content-Type", "application/json"}},
            .body = "{\"id\":42}",
        })
    );
  });
  completion.join();

  REQUIRE_FALSE(http_response.has_value());
  platform.RunPlatformModuleTasks();
  REQUIRE(http_response.has_value());
  REQUIRE(http_response->status_code == 201);
  REQUIRE(http_response->body == "{\"id\":42}");
  REQUIRE(http_resume_thread == ui_thread);
}

TEST_CASE("HttpClientHandlesImmediateTransportCompletion") {
  ResetHttpState();
  HttpTestPlatform platform;
  platform.transport->CompleteImmediately(HttpResult(HttpResponse{
      .url = "https://example.test/immediate",
      .status_code = 200,
      .body = "immediate",
  }));
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpResponse(http_client, {.url = "https://example.test/immediate"}));
  platform.RunPlatformModuleTasks();

  REQUIRE(http_completions == 1);
  REQUIRE(http_response.has_value());
  REQUIRE(http_response->body == "immediate");
  REQUIRE_FALSE(platform.transport->Canceled(0));
}

TEST_CASE("HttpClientAcceptsOnlyTheFirstTransportCompletion") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpResponse(http_client, {.url = "https://example.test/duplicate"}));
  platform.RunPlatformModuleTasks();

  platform.transport->Complete(
      0,
      HttpResult(HttpResponse{
          .url = "https://example.test/duplicate",
          .status_code = 200,
          .body = "first",
      })
  );
  platform.transport->Complete(
      0,
      HttpResult(HttpResponse{
          .url = "https://example.test/duplicate",
          .status_code = 200,
          .body = "second",
      })
  );
  platform.RunPlatformModuleTasks();

  REQUIRE(http_completions == 1);
  REQUIRE(http_response.has_value());
  REQUIRE(http_response->body == "first");
}

TEST_CASE("HttpClientMapsTransportFailuresWithoutTreatingHttpStatusesAsErrors") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpError(http_client, {.url = "https://example.test/failure"}));
  platform.RunPlatformModuleTasks();
  platform.transport->Complete(0, HttpResult(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"}));
  platform.RunPlatformModuleTasks();

  REQUIRE(http_error == HttpErrorCode::Timeout);
}

TEST_CASE("HttpClientCancellationStopsThePlatformRequestAndDropsLateCompletion") {
  ResetHttpState();
  HttpTestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  TaskHandle request = http_tasks.Launch(CountHttpCompletion(http_client, {.url = "https://example.test/slow"}));
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(platform.transport->Canceled(0));

  request.Cancel();
  REQUIRE(platform.transport->Canceled(0));
  platform.transport->Complete(
      0,
      HttpResult(HttpResponse{
          .url = "https://example.test/slow",
          .status_code = 200,
          .body = "late",
      })
  );
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
      static_cast<void>(http_client->Send({.url = "https://example.test", .body = "body"})),
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
      static_cast<void>(http_client->Send({
          .url = "https://example.test",
          .headers = {{"X-Test", "first\r\nsecond"}},
      })),
      std::invalid_argument
  );
  REQUIRE(platform.transport->CallCount() == 0);
}

TEST_CASE("HttpClientReportsUnsupportedAdaptersThroughTheTask") {
  ResetHttpState();
  TestPlatform platform;
  Runtime runtime(HttpApp, platform);
  runtime.BuildFrame();

  http_tasks.Launch(CaptureHttpError(http_client, {.url = "https://example.test"}));
  platform.RunPlatformModuleTasks();

  REQUIRE(http_error == HttpErrorCode::Unsupported);
}

} // namespace huxerui::test
