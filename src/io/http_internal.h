#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/http.h>

namespace huxerui::detail {

/// Cancels active and idle HTTP operations while the application's native transport is still alive.
/// @param application Original application whose weak operation registrations are disconnected on its owning thread.
void DisconnectHttpOperations(ApplicationRuntimeState& application) noexcept;

struct HttpTransportResponse {
  std::string url;
  int status_code = 0;
  std::vector<HttpHeader> headers;
  std::optional<std::uint64_t> body_size;
};

struct HttpTransportCallbacks {
  std::function<void(std::uint64_t)> upload_progress;
  std::function<void(HttpTransportResponse)> response;
  // Body publications are serialized in byte order after response and before normal completion.
  // RequestRead may reenter or arrive on another thread before a publication returns.
  std::function<void(Bytes)> body;
  std::function<void()> complete;
  std::function<void(HttpError)> error;
};

class HttpTransportOperation {
public:
  virtual ~HttpTransportOperation() = default;

  // Requests more data, not an exact callback count. Native push transports may still deliver
  // in-flight data after pausing. Demand must survive the preceding callback's completion.
  virtual void RequestRead() = 0;
  // Terminal operations cannot be restarted by subsequent demand; cancellation is idempotent.
  virtual void Cancel() noexcept = 0;
};

class HttpTransport {
public:
  virtual ~HttpTransport() = default;

  virtual std::shared_ptr<HttpTransportOperation>
  Start(HttpRequest request, bool require_incremental_response, HttpTransportCallbacks callbacks) = 0;
};

} // namespace huxerui::detail
