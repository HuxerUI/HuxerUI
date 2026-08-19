#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <huxerui/task.h>

namespace huxerui {

namespace detail {
class HttpTransport;
} // namespace detail

enum class HttpMethod {
  Get,
  Head,
  Post,
  Put,
  Patch,
  Delete,
  Options,
};

struct HttpHeader {
  std::string name;
  std::string value;

  bool operator==(const HttpHeader&) const = default;
};

struct HttpRequest {
  std::string url;
  HttpMethod method = HttpMethod::Get;
  std::vector<HttpHeader> headers;
  std::string body;
  std::optional<std::chrono::milliseconds> timeout = std::chrono::milliseconds{30000};
};

struct HttpResponse {
  std::string url;
  int status_code = 0;
  std::vector<HttpHeader> headers;
  std::string body;

  bool operator==(const HttpResponse&) const = default;
};

enum class HttpErrorCode {
  Transport,
  Timeout,
  Unsupported,
};

struct HttpError {
  HttpErrorCode code;
  std::string message;

  bool operator==(const HttpError&) const = default;
};

class [[nodiscard]] HttpResult final {
public:
  explicit HttpResult(HttpResponse response);
  explicit HttpResult(HttpError error);

  [[nodiscard]] bool HasResponse() const noexcept;

  [[nodiscard]] HttpResponse& Response() &;
  [[nodiscard]] const HttpResponse& Response() const&;
  [[nodiscard]] HttpResponse&& Response() &&;

  [[nodiscard]] HttpError& Error() &;
  [[nodiscard]] const HttpError& Error() const&;

private:
  std::variant<HttpResponse, HttpError> value_;
};

class HttpClient final {
public:
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;
  HttpClient(HttpClient&&) = delete;
  HttpClient& operator=(HttpClient&&) = delete;

  [[nodiscard]] Task<HttpResult> Send(HttpRequest request) const;

private:
  explicit HttpClient(std::shared_ptr<detail::HttpTransport> transport);

  std::shared_ptr<detail::HttpTransport> transport_;

  friend class Runtime;
};

} // namespace huxerui
