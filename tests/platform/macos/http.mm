#include <catch2/catch_amalgamated.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "io/http_internal.h"
#include "macos_http_internal.h"

namespace huxerui::test {
namespace {

using namespace std::chrono_literals;

class HttpServer final {
public:
  explicit HttpServer(bool hold_body = false) {
    body.resize(1024 * 1024);
    for (std::size_t i = 0; i < body.size(); ++i) {
      body[i] = static_cast<std::byte>((i / 4) >> ((i % 4) * 8));
    }
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0 || listen(listener_, 1) != 0) {
      if (listener_ >= 0) close(listener_);
      throw std::runtime_error("HuxerUI test HTTP listener could not start");
    }
    url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/body";
    thread_ = std::thread([this, hold_body] { Serve(hold_body); });
  }

  ~HttpServer() {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
      shutdown(listener_, SHUT_RDWR);
      if (client_ >= 0) shutdown(client_, SHUT_RDWR);
    }
    thread_.join();
    close(listener_);
  }

  bool WaitForDisconnect() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 3s, [this] { return disconnected_; });
  }

  Bytes body;
  std::string url;

private:
  static bool Send(int socket, const void* bytes, std::size_t size) {
    const auto* data = static_cast<const char*>(bytes);
    while (size != 0) {
      const ssize_t count = send(socket, data, size, 0);
      if (count <= 0) return false;
      data += count;
      size -= static_cast<std::size_t>(count);
    }
    return true;
  }

  void Serve(bool hold_body) {
    const int client = accept(listener_, nullptr, nullptr);
    if (client < 0) return;
    {
      std::scoped_lock lock(mutex_);
      if (stopping_) {
        close(client);
        return;
      }
      client_ = client;
    }
    const int no_signal = 1;
    const timeval timeout{3, 0};
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
      if (count <= 0) break;
      request.append(buffer, static_cast<std::size_t>(count));
    }
    const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                "\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
    if (Send(client, headers.data(), headers.size()) && hold_body) {
      Send(client, body.data(), 1);
    } else if (!hold_body) {
      for (std::size_t offset = 0; offset < body.size(); offset += 4096) {
        if (!Send(client, body.data() + offset, std::min<std::size_t>(4096, body.size() - offset))) break;
        if (offset % (64 * 1024) == 0) std::this_thread::sleep_for(1ms);
      }
    }
    const bool disconnected = hold_body && recv(client, buffer, sizeof(buffer), 0) == 0;
    {
      std::scoped_lock lock(mutex_);
      disconnected_ = disconnected;
      close(client_);
      client_ = -1;
    }
    condition_.notify_all();
  }

  int listener_ = -1;
  int client_ = -1;
  bool stopping_ = false;
  bool disconnected_ = false;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::thread thread_;
};

struct HttpCapture {
  std::mutex mutex;
  std::condition_variable condition;
  Bytes bytes;
  bool headers = false;
  bool pending = false;
  bool complete = false;
  bool invalid_order = false;
  int terminals = 0;
  std::optional<HttpError> error;

  detail::HttpTransportCallbacks Callbacks(const std::shared_ptr<HttpCapture>& self) {
    return {
        .upload_progress = {},
        .response = [self](detail::HttpTransportResponse) {
          std::scoped_lock lock(self->mutex);
          self->invalid_order |= self->headers;
          self->headers = true;
          self->pending = true;
          self->condition.notify_all();
        },
        .body = [self](Bytes bytes) {
          {
            std::scoped_lock lock(self->mutex);
            self->invalid_order |= !self->headers || self->terminals != 0;
            self->bytes.insert(self->bytes.end(), bytes.begin(), bytes.end());
            self->pending = true;
            self->condition.notify_all();
          }
          std::this_thread::sleep_for(2ms);
        },
        .complete = [self] {
          std::scoped_lock lock(self->mutex);
          self->invalid_order |= !self->headers;
          self->complete = true;
          ++self->terminals;
          self->condition.notify_all();
        },
        .error = [self](HttpError error) {
          std::scoped_lock lock(self->mutex);
          self->error = std::move(error);
          ++self->terminals;
          self->condition.notify_all();
        },
    };
  }
};

}

TEST_CASE("Mac HTTP preserves bytes when the consumer coalesces in-flight callbacks", "[http]") {
  const auto delay = GENERATE(1ms, 12ms, 30ms);
  HttpServer server;
  auto capture = std::make_shared<HttpCapture>();
  auto transport = detail::CreateMacHttpTransport();
  auto operation = transport->Start({.url = server.url, .timeout = 3s}, true, capture->Callbacks(capture));
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  for (;;) {
    std::unique_lock lock(capture->mutex);
    if (!capture->condition.wait_until(lock, deadline, [&] { return capture->pending || capture->terminals != 0; })) {
      break;
    }
    if (capture->terminals != 0) break;
    lock.unlock();
    std::this_thread::sleep_for(delay);
    lock.lock();
    // One demand drains all data already buffered, just as the shared stream does.
    capture->pending = false;
    lock.unlock();
    operation->RequestRead();
  }
  operation->Cancel();
  std::scoped_lock lock(capture->mutex);
  INFO("consumer delay: " << delay.count() << " ms; received " << capture->bytes.size() << " bytes");
  INFO((capture->error.has_value() ? capture->error->message : "no transport error"));
  REQUIRE_FALSE(capture->error.has_value());
  REQUIRE(capture->complete);
  REQUIRE(capture->terminals == 1);
  REQUIRE_FALSE(capture->invalid_order);
  REQUIRE(capture->bytes == server.body);
}

TEST_CASE("Mac HTTP terminates an idle or pending response", "[http]") {
  const bool timeout = GENERATE(false, true);
  const bool read = GENERATE(false, true);
  HttpServer server(true);
  auto capture = std::make_shared<HttpCapture>();
  auto operation = detail::CreateMacHttpTransport()->Start(
      {.url = server.url, .timeout = timeout ? 500ms : 3s}, true, capture->Callbacks(capture));
  {
    std::unique_lock lock(capture->mutex);
    REQUIRE(capture->condition.wait_for(lock, 2s, [&] { return capture->headers; }));
  }
  if (read) operation->RequestRead();
  if (timeout) {
    std::unique_lock lock(capture->mutex);
    REQUIRE(capture->condition.wait_for(lock, 2s, [&] { return capture->terminals != 0; }));
    REQUIRE(capture->error.has_value());
    REQUIRE(capture->error->code == HttpErrorCode::Timeout);
    REQUIRE(capture->terminals == 1);
  } else {
    operation->Cancel();
  }
  operation->Cancel();
  operation->RequestRead();
  REQUIRE(server.WaitForDisconnect());
  std::scoped_lock lock(capture->mutex);
  REQUIRE(capture->bytes.size() <= 1);
  REQUIRE_FALSE(capture->complete);
  REQUIRE(capture->terminals == (timeout ? 1 : 0));
}

}
