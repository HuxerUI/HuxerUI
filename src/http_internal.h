#pragma once

#include <functional>

#include <huxerui/http.h>

namespace huxerui::detail {

using HttpTransportCompletion = std::function<void(HttpResult)>;

class HttpTransport {
public:
  virtual ~HttpTransport() = default;

  virtual std::function<void()> Start(HttpRequest request, HttpTransportCompletion completion) = 0;
};

} // namespace huxerui::detail
