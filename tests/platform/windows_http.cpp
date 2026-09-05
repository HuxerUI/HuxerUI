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

#include "io/http_internal.h"
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
  [[nodiscard]] detail::HttpTransportCallbacks Callbacks() {
    return {
        .response = [this](detail::HttpTransportResponse response) {
          {
            std::scoped_lock lock(mutex_);
            response_.emplace(std::move(response));
          }
          condition_.notify_all();
        },
        .body = [this](Bytes body) {
          {
            std::scoped_lock lock(mutex_);
            response_body_.insert(response_body_.end(), body.begin(), body.end());
            ++body_events_;
          }
          condition_.notify_all();
        },
        .complete = [this] {
          {
            std::scoped_lock lock(mutex_);
            complete_ = true;
          }
          condition_.notify_all();
        },
        .error = [this](HttpError error) {
          {
            std::scoped_lock lock(mutex_);
            error_.emplace(std::move(error));
          }
          condition_.notify_all();
        },
    };
  }

  void WaitForResponse() {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds{5}, [this] { return response_.has_value() || error_; })) {
      throw std::runtime_error("HuxerUI test timed out waiting for WinHTTP response headers");
    }
  }

  void WaitForBodyEvent(std::size_t previous_events) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds{5}, [this, previous_events] {
          return body_events_ != previous_events || complete_ || error_.has_value();
        })) {
      throw std::runtime_error("HuxerUI test timed out waiting for a WinHTTP body event");
    }
  }

  void WaitForTerminal() {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds{5}, [this] { return complete_ || error_.has_value(); })) {
      throw std::runtime_error("HuxerUI test timed out waiting for a WinHTTP terminal event");
    }
  }

  [[nodiscard]] bool WaitForAny(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] {
      return response_.has_value() || body_events_ != 0 || complete_ || error_.has_value();
    });
  }

  [[nodiscard]] std::size_t BodyEvents() const {
    std::scoped_lock lock(mutex_);
    return body_events_;
  }

  [[nodiscard]] bool Complete() const {
    std::scoped_lock lock(mutex_);
    return complete_;
  }

  [[nodiscard]] std::optional<HttpError> Error() const {
    std::scoped_lock lock(mutex_);
    return error_;
  }

  [[nodiscard]] detail::HttpTransportResponse Response() const {
    std::scoped_lock lock(mutex_);
    return *response_;
  }

  [[nodiscard]] Bytes Body() const {
    std::scoped_lock lock(mutex_);
    return response_body_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<detail::HttpTransportResponse> response_;
  Bytes response_body_;
  std::optional<HttpError> error_;
  std::size_t body_events_ = 0;
  bool complete_ = false;
};

void DrainResponse(const std::shared_ptr<detail::HttpTransportOperation>& operation, HttpCompletion& completion) {
  completion.WaitForResponse();
  while (!completion.Complete() && !completion.Error().has_value()) {
    const std::size_t events = completion.BodyEvents();
    operation->RequestRead();
    completion.WaitForBodyEvent(events);
  }
}

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

  const std::shared_ptr<detail::HttpTransportOperation> operation = transport->Start(
      {
          .url = server.Url("/items?source=test"),
          .method = HttpMethod::Post,
          .headers = {{"Content-Type", "application/octet-stream"}, {"X-Trace", "winhttp"}},
          .body = BytesFromString(request_body),
          .timeout = std::chrono::seconds{5},
      },
      true,
      completion.Callbacks()
  );

  const std::string request = server.WaitForRequest();
  completion.WaitForResponse();
  REQUIRE(completion.BodyEvents() == 0);
  DrainResponse(operation, completion);

  REQUIRE(request.starts_with("POST /items?source=test HTTP/1.1\r\n"));
  REQUIRE(request.find("Content-Type: application/octet-stream\r\n") != std::string::npos);
  REQUIRE(request.find("X-Trace: winhttp\r\n") != std::string::npos);
  REQUIRE(request.ends_with(request_body));

  REQUIRE_FALSE(completion.Error().has_value());
  const detail::HttpTransportResponse response = completion.Response();
  REQUIRE(response.status_code == 201);
  REQUIRE(response.url == server.Url("/items?source=test"));
  REQUIRE(completion.Body() == BytesFromString(response_body));
  REQUIRE(response.body_size == response_body.size());
  REQUIRE(std::count(response.headers.begin(), response.headers.end(), HttpHeader{"X-Test", "first"}) == 1);
  REQUIRE(std::count(response.headers.begin(), response.headers.end(), HttpHeader{"X-Test", "second"}) == 1);
}

TEST_CASE("WindowsHttpTransportReturnsOnlyTheFinalRedirectResponse") {
  LoopbackHttpServer final_server(ResponseWithBody("final"), 0);
  const std::string redirect_response = "HTTP/1.1 302 Found\r\nLocation: " + final_server.Url("/final") +
                                        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  LoopbackHttpServer redirect_server(redirect_response, 0);
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;

  const std::shared_ptr<detail::HttpTransportOperation> operation = transport->Start(
      {.url = redirect_server.Url("/redirect"), .timeout = std::chrono::seconds{5}},
      true,
      completion.Callbacks()
  );
  (void)redirect_server.WaitForRequest();
  (void)final_server.WaitForRequest();
  DrainResponse(operation, completion);

  REQUIRE_FALSE(completion.Error().has_value());
  REQUIRE(completion.Response().status_code == 201);
  REQUIRE(completion.Response().url == final_server.Url("/final"));
  REQUIRE(completion.Body() == BytesFromString("final"));
}

TEST_CASE("WindowsHttpTransportUsesPlatformResponseDecompression") {
  const unsigned char compressed_bytes[] = {
      0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xFF, 0x4B, 0xCE, 0xCF,
      0x2D, 0x28, 0x4A, 0x2D, 0x2E, 0x4E, 0x4D, 0x51, 0x00, 0x52, 0x05, 0xF9, 0x79,
      0xC5, 0xA9, 0x00, 0xB1, 0xFF, 0x32, 0x6F, 0x13, 0x00, 0x00, 0x00,
  };
  const std::string compressed(reinterpret_cast<const char*>(compressed_bytes), sizeof(compressed_bytes));
  const std::string response = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " +
                               std::to_string(compressed.size()) + "\r\nConnection: close\r\n\r\n" + compressed;
  LoopbackHttpServer server(response, 0);
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;

  const std::shared_ptr<detail::HttpTransportOperation> operation =
      transport->Start({.url = server.Url("/compressed")}, true, completion.Callbacks());
  (void)server.WaitForRequest();
  DrainResponse(operation, completion);

  REQUIRE_FALSE(completion.Error().has_value());
  REQUIRE(completion.Body() == BytesFromString("compressed response"));
}

TEST_CASE("WindowsHttpTransportAppliesOneDeadlineToTheWholeRequest") {
  LoopbackHttpServer server({}, 0, true);
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;

  const std::shared_ptr<detail::HttpTransportOperation> operation = transport->Start(
      {
          .url = server.Url("/slow"),
          .timeout = std::chrono::milliseconds{250},
      },
      true,
      completion.Callbacks()
  );
  (void)server.WaitForRequest();

  completion.WaitForTerminal();
  REQUIRE(completion.Error()->code == HttpErrorCode::Timeout);
  operation->Cancel();
  server.ReleaseResponse();
}

TEST_CASE("WindowsHttpTransportCancellationSuppressesLateCompletion") {
  LoopbackHttpServer server({}, 0, true);
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;

  const std::shared_ptr<detail::HttpTransportOperation> operation = transport->Start(
      {
          .url = server.Url("/cancel"),
          .timeout = std::nullopt,
      },
      true,
      completion.Callbacks()
  );
  (void)server.WaitForRequest();

  operation->Cancel();
  server.ReleaseResponse();
  REQUIRE_FALSE(completion.WaitForAny(std::chrono::milliseconds{250}));
}

TEST_CASE("WindowsHttpTransportRejectsInvalidUtf8BeforeStartingNativeIO") {
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateWin32HttpTransport();
  HttpCompletion completion;
  std::string invalid_url = "http://example.test/";
  invalid_url.push_back(static_cast<char>(0xFF));

  const std::shared_ptr<detail::HttpTransportOperation> operation =
      transport->Start({.url = std::move(invalid_url)}, true, completion.Callbacks());
  completion.WaitForTerminal();
  operation->Cancel();

  REQUIRE(completion.Error()->code == HttpErrorCode::Transport);
}

} // namespace huxerui::test
