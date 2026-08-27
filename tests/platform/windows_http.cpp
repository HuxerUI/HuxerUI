#include <winsock2.h>
#include <ws2tcpip.h>

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "http_internal.h"
#include "win32_http_internal.h"

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

class WinsockRuntime final {
public:
  WinsockRuntime() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("HuxerUI test failed to initialize Winsock");
    }
  }

  ~WinsockRuntime() {
    WSACleanup();
  }

  WinsockRuntime(const WinsockRuntime&) = delete;
  WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

class LoopbackHttpServer final {
public:
  LoopbackHttpServer(std::string response, std::size_t expected_body_size, bool hold_response = false)
      : response_(std::move(response)), expected_body_size_(expected_body_size), hold_response_(hold_response) {
    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET) {
      throw std::runtime_error("HuxerUI test failed to create a loopback socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listen_socket_, 1) == SOCKET_ERROR) {
      closesocket(listen_socket_);
      throw std::runtime_error("HuxerUI test failed to bind a loopback socket");
    }

    int address_size = sizeof(address);
    if (getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &address_size) == SOCKET_ERROR) {
      closesocket(listen_socket_);
      throw std::runtime_error("HuxerUI test failed to query the loopback port");
    }
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { Run(); });
  }

  ~LoopbackHttpServer() {
    ReleaseResponse();
    WakeListener();
    worker_.join();
    closesocket(listen_socket_);
  }

  LoopbackHttpServer(const LoopbackHttpServer&) = delete;
  LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

  [[nodiscard]] std::string Url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

  [[nodiscard]] std::string WaitForRequest() {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds{5}, [this] { return request_ready_; })) {
      throw std::runtime_error("HuxerUI test timed out waiting for a loopback request");
    }
    return request_;
  }

  void ReleaseResponse() {
    {
      std::scoped_lock lock(mutex_);
      response_released_ = true;
    }
    condition_.notify_all();
  }

private:
  void WakeListener() const noexcept {
    const SOCKET wake_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (wake_socket == INVALID_SOCKET) {
      return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port_);
    connect(wake_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    shutdown(wake_socket, SD_BOTH);
    closesocket(wake_socket);
  }

  void Run() {
    const SOCKET client = accept(listen_socket_, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      return;
    }

    std::string request;
    std::size_t expected_size = std::string::npos;
    char buffer[4096];
    while (expected_size == std::string::npos || request.size() < expected_size) {
      const int received = recv(client, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        break;
      }
      request.append(buffer, static_cast<std::size_t>(received));
      const std::size_t header_end = request.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        expected_size = header_end + 4 + expected_body_size_;
      }
    }

    {
      std::scoped_lock lock(mutex_);
      request_ = std::move(request);
      request_ready_ = true;
    }
    condition_.notify_all();

    if (hold_response_) {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, [this] { return response_released_; });
    }

    std::size_t sent = 0;
    while (sent < response_.size()) {
      const int result = send(client, response_.data() + sent, static_cast<int>(response_.size() - sent), 0);
      if (result <= 0) {
        break;
      }
      sent += static_cast<std::size_t>(result);
    }
    shutdown(client, SD_BOTH);
    closesocket(client);
  }

  WinsockRuntime winsock_;
  SOCKET listen_socket_ = INVALID_SOCKET;
  unsigned short port_ = 0;
  std::string response_;
  std::size_t expected_body_size_ = 0;
  bool hold_response_ = false;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::string request_;
  bool request_ready_ = false;
  bool response_released_ = false;
};

class HttpCompletion final {
public:
  void Complete(HttpResult result) {
    {
      std::scoped_lock lock(mutex_);
      result_.emplace(std::move(result));
    }
    condition_.notify_one();
  }

  [[nodiscard]] HttpResult Wait() {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds{5}, [this] { return result_.has_value(); })) {
      throw std::runtime_error("HuxerUI test timed out waiting for a WinHTTP result");
    }
    return std::move(*result_);
  }

  [[nodiscard]] bool WaitFor(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return result_.has_value(); });
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<HttpResult> result_;
};

std::string ResponseWithBody(std::string body) {
  return "HTTP/1.1 201 Created\r\n"
         "Content-Length: " +
         std::to_string(body.size()) +
         "\r\nX-Test: first\r\n"
         "X-Test: second\r\n"
         "Connection: close\r\n\r\n" +
         body;
}

} // namespace

TEST_CASE("WindowsHttpTransportPreservesRequestAndResponseData") {
  const std::string request_body("A\0B", 3);
  const std::string response_body("C\0D\n", 4);
  LoopbackHttpServer server(ResponseWithBody(response_body), request_body.size());
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;

  const std::function<void()> cancel = transport->Start(
      {
          .url = server.Url("/items?source=test"),
          .method = HttpMethod::Post,
          .headers = {{"Content-Type", "application/octet-stream"}, {"X-Trace", "winhttp"}},
          .body = BytesFromString(request_body),
          .timeout = std::chrono::seconds{5},
      },
      [&completion](HttpResult result) { completion.Complete(std::move(result)); }
  );

  const std::string request = server.WaitForRequest();
  HttpResult result = completion.Wait();
  cancel();

  REQUIRE(request.starts_with("POST /items?source=test HTTP/1.1\r\n"));
  REQUIRE(request.find("Content-Type: application/octet-stream\r\n") != std::string::npos);
  REQUIRE(request.find("X-Trace: winhttp\r\n") != std::string::npos);
  REQUIRE(request.ends_with(request_body));

  REQUIRE(result.HasResponse());
  const HttpResponse& response = result.Response();
  REQUIRE(response.status_code == 201);
  REQUIRE(response.url == server.Url("/items?source=test"));
  REQUIRE(response.body == BytesFromString(response_body));
  REQUIRE(std::count(response.headers.begin(), response.headers.end(), HttpHeader{"X-Test", "first"}) == 1);
  REQUIRE(std::count(response.headers.begin(), response.headers.end(), HttpHeader{"X-Test", "second"}) == 1);
}

TEST_CASE("WindowsHttpTransportAppliesOneDeadlineToTheWholeRequest") {
  LoopbackHttpServer server({}, 0, true);
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;

  const std::function<void()> cancel = transport->Start(
      {
          .url = server.Url("/slow"),
          .timeout = std::chrono::milliseconds{250},
      },
      [&completion](HttpResult result) { completion.Complete(std::move(result)); }
  );
  (void)server.WaitForRequest();

  HttpResult result = completion.Wait();
  cancel();
  REQUIRE_FALSE(result.HasResponse());
  REQUIRE(result.Error().code == HttpErrorCode::Timeout);
  server.ReleaseResponse();
}

TEST_CASE("WindowsHttpTransportCancellationSuppressesLateCompletion") {
  LoopbackHttpServer server({}, 0, true);
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;

  const std::function<void()> cancel = transport->Start(
      {
          .url = server.Url("/cancel"),
          .timeout = std::nullopt,
      },
      [&completion](HttpResult result) { completion.Complete(std::move(result)); }
  );
  (void)server.WaitForRequest();

  cancel();
  server.ReleaseResponse();
  REQUIRE_FALSE(completion.WaitFor(std::chrono::milliseconds{250}));
}

TEST_CASE("WindowsHttpTransportRejectsInvalidUtf8BeforeStartingNativeIO") {
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;
  std::string invalid_url = "http://example.test/";
  invalid_url.push_back(static_cast<char>(0xFF));

  const std::function<void()> cancel =
      transport->Start({.url = std::move(invalid_url)}, [&completion](HttpResult result) {
        completion.Complete(std::move(result));
      });
  HttpResult result = completion.Wait();
  cancel();

  REQUIRE_FALSE(result.HasResponse());
  REQUIRE(result.Error().code == HttpErrorCode::Transport);
}

} // namespace huxerui::test
