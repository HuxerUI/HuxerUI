#include <catch2/catch_amalgamated.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "http_internal.h"
#include "linux_http_internal.h"

namespace huxerui::test {

namespace {

using namespace std::chrono_literals;

Bytes BytesFromString(std::string_view value) {
  Bytes bytes;
  bytes.reserve(value.size());
  for (const char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  return bytes;
}

void CloseSocket(int socket) noexcept {
  if (socket >= 0) {
    close(socket);
  }
}

void SendAll(int socket, std::string_view data) noexcept {
  while (!data.empty()) {
    const ssize_t sent = send(socket, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent <= 0) {
      return;
    }
    data.remove_prefix(static_cast<std::size_t>(sent));
  }
}

void SendResponse(
    int socket, int status, std::string_view phrase, const std::vector<HttpHeader>& headers, std::string_view body
) noexcept {
  std::string response = "HTTP/1.1 " + std::to_string(status) + " " + std::string(phrase) + "\r\n";
  for (const HttpHeader& header : headers) {
    response += header.name + ": " + header.value + "\r\n";
  }
  response += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  response.append(body);
  SendAll(socket, response);
}

std::string LowerAscii(std::string value) {
  for (char& character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

std::size_t RequestContentLength(std::string_view headers) {
  std::size_t line_begin = 0;
  while (line_begin < headers.size()) {
    const std::size_t line_end = headers.find("\r\n", line_begin);
    const std::string_view line = headers.substr(line_begin, line_end - line_begin);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && LowerAscii(std::string(line.substr(0, colon))) == "content-length") {
      std::size_t value_begin = colon + 1;
      while (value_begin < line.size() && line[value_begin] == ' ') {
        ++value_begin;
      }
      return static_cast<std::size_t>(std::stoull(std::string(line.substr(value_begin))));
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_begin = line_end + 2;
  }
  return 0;
}

std::string ReadRequest(int socket) {
  std::string request;
  char buffer[4096];
  std::size_t expected_size = 0;
  while (expected_size == 0 || request.size() < expected_size) {
    const ssize_t received = recv(socket, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      break;
    }
    request.append(buffer, static_cast<std::size_t>(received));
    if (expected_size == 0) {
      const std::size_t header_end = request.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        expected_size = header_end + 4 + RequestContentLength(std::string_view(request).substr(0, header_end));
      }
    }
  }
  return request;
}

class LoopbackHttpServer final {
public:
  using Handler = std::function<void(int, const std::string&)>;

  explicit LoopbackHttpServer(std::vector<Handler> handlers) : handlers_(std::move(handlers)) {
    listen_socket_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_socket_ < 0) {
      throw std::system_error(errno, std::generic_category(), "could not create loopback HTTP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      const int error = errno;
      CloseSocket(std::exchange(listen_socket_, -1));
      throw std::system_error(error, std::generic_category(), "could not bind loopback HTTP socket");
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
        listen(listen_socket_, static_cast<int>(handlers_.size())) != 0) {
      const int error = errno;
      CloseSocket(std::exchange(listen_socket_, -1));
      throw std::system_error(error, std::generic_category(), "could not listen on loopback HTTP socket");
    }
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  LoopbackHttpServer(const LoopbackHttpServer&) = delete;
  LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

  ~LoopbackHttpServer() {
    stopping_ = true;
    if (listen_socket_ >= 0) {
      shutdown(listen_socket_, SHUT_RDWR);
      CloseSocket(std::exchange(listen_socket_, -1));
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] std::string Url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

  [[nodiscard]] bool WaitForRequests(std::size_t count, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this, count] { return requests_.size() >= count || failure_; }) &&
           requests_.size() >= count;
  }

  [[nodiscard]] std::string Request(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    if (failure_) {
      std::rethrow_exception(failure_);
    }
    return requests_.at(index);
  }

private:
  void Run() noexcept {
    try {
      for (const Handler& handler : handlers_) {
        int client = -1;
        do {
          client = accept4(listen_socket_, nullptr, nullptr, SOCK_CLOEXEC);
        } while (client < 0 && errno == EINTR);
        if (client < 0) {
          if (stopping_) {
            return;
          }
          throw std::system_error(errno, std::generic_category(), "could not accept loopback HTTP request");
        }
        const std::string request = ReadRequest(client);
        {
          std::lock_guard lock(mutex_);
          requests_.push_back(request);
        }
        condition_.notify_all();
        handler(client, request);
        CloseSocket(client);
      }
    } catch (...) {
      {
        std::lock_guard lock(mutex_);
        failure_ = std::current_exception();
      }
      condition_.notify_all();
    }
  }

  int listen_socket_ = -1;
  std::uint16_t port_ = 0;
  std::vector<Handler> handlers_;
  std::thread thread_;
  std::atomic<bool> stopping_ = false;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<std::string> requests_;
  std::exception_ptr failure_;
};

struct PendingRequest {
  std::future<HttpResult> result;
  std::shared_ptr<detail::HttpTransportOperation> operation;
};

PendingRequest StartRequest(const std::shared_ptr<detail::HttpTransport>& transport, HttpRequest request) {
  struct State {
    void RequestRead() {
      std::shared_ptr<detail::HttpTransportOperation> current;
      {
        std::scoped_lock lock(mutex);
        if (operation) {
          current = operation;
        } else {
          read_pending = true;
        }
      }
      if (current) {
        current->RequestRead();
      }
    }

    void SetOperation(std::shared_ptr<detail::HttpTransportOperation> value) {
      bool request_read = false;
      {
        std::scoped_lock lock(mutex);
        operation = std::move(value);
        request_read = read_pending;
      }
      if (request_read) {
        operation->RequestRead();
      }
    }

    void Complete(HttpResult result) {
      bool publish = false;
      {
        std::scoped_lock lock(mutex);
        if (!finished) {
          finished = true;
          publish = true;
        }
      }
      if (publish) {
        promise.set_value(std::move(result));
      }
    }

    std::mutex mutex;
    std::promise<HttpResult> promise;
    std::shared_ptr<detail::HttpTransportOperation> operation;
    std::optional<detail::HttpTransportResponse> response;
    Bytes body;
    bool read_pending = false;
    bool finished = false;
  };

  auto state = std::make_shared<State>();
  std::future<HttpResult> future = state->promise.get_future();
  detail::HttpTransportCallbacks callbacks{
      .response = [state](detail::HttpTransportResponse response) {
        {
          std::scoped_lock lock(state->mutex);
          state->response.emplace(std::move(response));
        }
        state->RequestRead();
      },
      .body = [state](Bytes body) {
        {
          std::scoped_lock lock(state->mutex);
          state->body.insert(state->body.end(), body.begin(), body.end());
        }
        state->RequestRead();
      },
      .complete = [state] {
        detail::HttpTransportResponse metadata;
        Bytes body;
        {
          std::scoped_lock lock(state->mutex);
          metadata = std::move(*state->response);
          body = std::move(state->body);
        }
        state->Complete(HttpResult(HttpResponse{
            .url = std::move(metadata.url),
            .status_code = metadata.status_code,
            .headers = std::move(metadata.headers),
            .body = std::move(body),
        }));
      },
      .error = [state](HttpError error) { state->Complete(HttpResult(std::move(error))); },
  };
  auto operation = transport->Start(std::move(request), false, std::move(callbacks));
  state->SetOperation(operation);
  return {std::move(future), std::move(operation)};
}

std::uint16_t ClosedLoopbackPort() {
  const int socket_handle = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket_handle < 0) {
    throw std::system_error(errno, std::generic_category(), "could not create closed-port socket");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    const int error = errno;
    CloseSocket(socket_handle);
    throw std::system_error(error, std::generic_category(), "could not reserve closed loopback port");
  }
  socklen_t address_size = sizeof(address);
  if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
    const int error = errno;
    CloseSocket(socket_handle);
    throw std::system_error(error, std::generic_category(), "could not read closed loopback port");
  }
  CloseSocket(socket_handle);
  return ntohs(address.sin_port);
}

std::string JoinedHeaderValues(const HttpResponse& response, std::string_view requested_name) {
  std::string result;
  for (const HttpHeader& header : response.headers) {
    if (LowerAscii(header.name) == LowerAscii(std::string(requested_name))) {
      result += header.value;
    }
  }
  return result;
}

} // namespace

TEST_CASE("Linux HTTP transport preserves buffered requests and responses") {
  const std::string response_body{'r', 'e', 'p', 'l', 'y', '\0', static_cast<char>(0xFF), 'b', 'y', 't', 'e', 's'};
  LoopbackHttpServer server({[&response_body](int socket, const std::string&) {
    SendResponse(socket, 201, "Created", {{"X-Repeat", "first"}, {"X-Repeat", "second"}}, response_body);
  }});
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateLinuxHttpTransport();
  const std::string request_body("payload\0bytes", 13);
  PendingRequest pending = StartRequest(
      transport,
      {
          .url = server.Url("/items"),
          .method = HttpMethod::Post,
          .headers = {{"Content-Type", "application/octet-stream"}, {"X-Trace", "first"}, {"X-Trace", "second"}},
          .body = BytesFromString(request_body),
          .timeout = 2s,
      }
  );

  REQUIRE(pending.result.wait_for(2s) == std::future_status::ready);
  HttpResult result = pending.result.get();
  REQUIRE(result.HasResponse());
  const HttpResponse& response = result.Response();
  REQUIRE(response.status_code == 201);
  REQUIRE(response.url == server.Url("/items"));
  REQUIRE(response.body == BytesFromString(response_body));
  const std::string repeated_values = JoinedHeaderValues(response, "X-Repeat");
  REQUIRE(repeated_values.find("first") != std::string::npos);
  REQUIRE(repeated_values.find("second") != std::string::npos);

  REQUIRE(server.WaitForRequests(1));
  const std::string wire_request = server.Request(0);
  REQUIRE(wire_request.starts_with("POST /items HTTP/1.1\r\n"));
  REQUIRE(wire_request.find("X-Trace: first") != std::string::npos);
  REQUIRE(wire_request.find("second") != std::string::npos);
  REQUIRE(wire_request.ends_with(request_body));
}

TEST_CASE("Linux HTTP transport follows redirects and returns HTTP error statuses as responses") {
  LoopbackHttpServer server({
      [](int socket, const std::string&) { SendResponse(socket, 302, "Found", {{"Location", "/missing"}}, {}); },
      [](int socket, const std::string&) { SendResponse(socket, 404, "Not Found", {}, "missing"); },
  });
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateLinuxHttpTransport();
  PendingRequest pending = StartRequest(transport, {.url = server.Url("/redirect"), .timeout = 2s});

  REQUIRE(pending.result.wait_for(2s) == std::future_status::ready);
  HttpResult result = pending.result.get();
  REQUIRE(result.HasResponse());
  REQUIRE(result.Response().status_code == 404);
  REQUIRE(result.Response().url == server.Url("/missing"));
  REQUIRE(result.Response().body == BytesFromString("missing"));
  REQUIRE(server.WaitForRequests(2));
  REQUIRE(server.Request(0).starts_with("GET /redirect HTTP/1.1\r\n"));
  REQUIRE(server.Request(1).starts_with("GET /missing HTTP/1.1\r\n"));
}

TEST_CASE("Linux HTTP transport maps connection failures") {
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateLinuxHttpTransport();
  PendingRequest pending = StartRequest(
      transport,
      {
          .url = "http://127.0.0.1:" + std::to_string(ClosedLoopbackPort()) + "/unavailable",
          .timeout = 2s,
      }
  );

  REQUIRE(pending.result.wait_for(2s) == std::future_status::ready);
  HttpResult result = pending.result.get();
  REQUIRE_FALSE(result.HasResponse());
  REQUIRE(result.Error().code == HttpErrorCode::Transport);
}

TEST_CASE("Linux HTTP transport enforces the complete request timeout") {
  LoopbackHttpServer server({[](int socket, const std::string&) {
    std::this_thread::sleep_for(200ms);
    SendResponse(socket, 200, "OK", {}, "late");
  }});
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateLinuxHttpTransport();
  PendingRequest pending = StartRequest(transport, {.url = server.Url("/slow"), .timeout = 20ms});

  REQUIRE(pending.result.wait_for(2s) == std::future_status::ready);
  HttpResult result = pending.result.get();
  REQUIRE_FALSE(result.HasResponse());
  REQUIRE(result.Error().code == HttpErrorCode::Timeout);
}

TEST_CASE("Linux HTTP transport suppresses completion after cancellation") {
  LoopbackHttpServer server({[](int socket, const std::string&) {
    std::this_thread::sleep_for(150ms);
    SendResponse(socket, 200, "OK", {}, "late");
  }});
  const std::shared_ptr<detail::HttpTransport> transport = detail::CreateLinuxHttpTransport();
  std::atomic<int> completions = 0;
  const std::shared_ptr<detail::HttpTransportOperation> operation = transport->Start(
      {.url = server.Url("/cancel"), .timeout = 2s},
      true,
      {
          .complete = [&completions] { ++completions; },
          .error = [&completions](HttpError) { ++completions; },
      }
  );

  REQUIRE(server.WaitForRequests(1));
  operation->Cancel();
  std::this_thread::sleep_for(250ms);
  REQUIRE(completions == 0);
}

TEST_CASE("Linux HTTP transport resolves cancellation and completion races once") {
  for (int iteration = 0; iteration < 20; ++iteration) {
    std::atomic<bool> release_response = false;
    LoopbackHttpServer server({[&release_response](int socket, const std::string&) {
      while (!release_response.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      SendResponse(socket, 200, "OK", {}, "done");
    }});
    std::shared_ptr<detail::HttpTransport> transport = detail::CreateLinuxHttpTransport();
    std::atomic<int> completions = 0;
    const std::shared_ptr<detail::HttpTransportOperation> operation = transport->Start(
        {.url = server.Url("/race"), .timeout = 2s},
        true,
        {
            .complete = [&completions] { ++completions; },
            .error = [&completions](HttpError) { ++completions; },
        }
    );
    REQUIRE(server.WaitForRequests(1));

    std::thread cancel_thread([operation, &release_response] {
      while (!release_response.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      operation->Cancel();
    });
    release_response.store(true, std::memory_order_release);
    cancel_thread.join();
    transport.reset();
    REQUIRE(completions.load() <= 1);
  }
}

TEST_CASE("Destroying Linux HTTP transport cancels active requests and joins its network thread") {
  LoopbackHttpServer server({[](int socket, const std::string&) {
    std::this_thread::sleep_for(250ms);
    SendResponse(socket, 200, "OK", {}, "late");
  }});
  std::shared_ptr<detail::HttpTransport> transport = detail::CreateLinuxHttpTransport();
  std::atomic<int> completions = 0;
  const std::shared_ptr<detail::HttpTransportOperation> operation = transport->Start(
      {.url = server.Url("/shutdown"), .timeout = 2s},
      true,
      {
          .complete = [&completions] { ++completions; },
          .error = [&completions](HttpError) { ++completions; },
      }
  );
  REQUIRE(server.WaitForRequests(1));

  const auto start = std::chrono::steady_clock::now();
  transport.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  REQUIRE(elapsed < 2s);
  REQUIRE(completions == 0);
  static_cast<void>(operation);
}

} // namespace huxerui::test
