#pragma once

#include <napi/native_api.h>

#include <memory>

#include <huxerui/app.h>

#include "io/http_internal.h"

namespace huxerui::detail {

class OhosHttpTransport final : public HttpTransport {
public:
  OhosHttpTransport(napi_env env, napi_value host, UiThreadDispatcher dispatch);
  ~OhosHttpTransport() override;

  std::shared_ptr<HttpTransportOperation>
  Start(HttpRequest request, bool require_incremental_response, HttpTransportCallbacks callbacks) override;

  napi_value Receive(napi_callback_info info);
  void Disconnect() noexcept;
  void ReleaseHost() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace huxerui::detail
