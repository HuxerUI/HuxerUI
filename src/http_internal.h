#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/http.h>

namespace huxerui::detail {

struct HttpTransportResponse {
  std::string url;
  int status_code = 0;
  std::vector<HttpHeader> headers;
  std::optional<std::uint64_t> body_size;
};

struct HttpTransportCallbacks {
  std::function<void(std::uint64_t)> upload_progress;
  std::function<void(HttpTransportResponse)> response;
  std::function<void(Bytes)> body;
  std::function<void()> complete;
  std::function<void(HttpError)> error;
};

class HttpTransportOperation {
public:
  virtual ~HttpTransportOperation() = default;

  virtual void RequestRead() = 0;
  virtual void Cancel() noexcept = 0;
};

class HttpTransport {
public:
  virtual ~HttpTransport() = default;

  virtual std::shared_ptr<HttpTransportOperation>
  Start(HttpRequest request, bool require_incremental_response, HttpTransportCallbacks callbacks) = 0;
};

} // namespace huxerui::detail
