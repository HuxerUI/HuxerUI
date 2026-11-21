#include "ohos_http.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace huxerui::detail {
namespace {

constexpr std::size_t maximum_pending_bytes = 8U * 1024U * 1024U;
std::uint64_t next_request = 0;

// These event values match the ArkTS transport.
enum class Event { Headers, Body, Progress, Complete, Error };

void Check(napi_status status) {
  if (status != napi_ok) {
    throw std::runtime_error("HuxerUI could not communicate with OHOS HTTP");
  }
}

struct HandleScope {
  explicit HandleScope(napi_env env) : env(env) { Check(napi_open_handle_scope(env, &scope)); }
  ~HandleScope() { napi_close_handle_scope(env, scope); }
  HandleScope(const HandleScope&) = delete;
  HandleScope& operator=(const HandleScope&) = delete;
  napi_env env;
  napi_handle_scope scope = nullptr;
};

napi_value String(napi_env env, std::string_view text) {
  napi_value result;
  Check(napi_create_string_utf8(env, text.data(), text.size(), &result));
  return result;
}

std::string ReadString(napi_env env, napi_value value) {
  std::size_t length = 0;
  Check(napi_get_value_string_utf8(env, value, nullptr, 0, &length));
  std::string result(length + 1, '\0');
  Check(napi_get_value_string_utf8(env, value, result.data(), result.size(), &length));
  result.resize(length);
  return result;
}

napi_value Element(napi_env env, napi_value value, std::uint32_t index) {
  napi_value result;
  Check(napi_get_element(env, value, index, &result));
  return result;
}

int Integer(napi_env env, napi_value value) {
  std::int32_t result = 0;
  Check(napi_get_value_int32(env, value, &result));
  return result;
}

std::vector<HttpHeader> ReadHeaders(napi_env env, napi_value value) {
  std::uint32_t length = 0;
  Check(napi_get_array_length(env, value, &length));
  std::vector<HttpHeader> headers;
  headers.reserve(length);
  for (std::uint32_t index = 0; index < length; ++index) {
    const auto field = Element(env, value, index);
    headers.push_back({ReadString(env, Element(env, field, 0)), ReadString(env, Element(env, field, 1))});
  }
  return headers;
}

const char* MethodName(HttpMethod method) {
  switch (method) {
  case HttpMethod::Get: return "GET";
  case HttpMethod::Head: return "HEAD";
  case HttpMethod::Post: return "POST";
  case HttpMethod::Put: return "PUT";
  case HttpMethod::Patch: return "PATCH";
  case HttpMethod::Delete: return "DELETE";
  case HttpMethod::Options: return "OPTIONS";
  }
  throw std::invalid_argument("HuxerUI invalid OHOS HTTP method");
}

class Operation final : public HttpTransportOperation, public std::enable_shared_from_this<Operation> {
public:
  Operation(UiThreadDispatcher dispatch, std::function<void(bool)> retire, HttpTransportCallbacks callbacks,
            bool streaming)
      : dispatch_(std::move(dispatch)), retire_(std::move(retire)), callbacks_(std::move(callbacks)),
        streaming_(streaming) {}

  void RequestRead() override {
    OnApplicationThread([](Operation& operation) {
      operation.requested_ = true;
      operation.Drain();
    });
  }

  void Cancel() noexcept override {
    try {
      OnApplicationThread([](Operation& operation) { operation.Retire(true); });
    } catch (...) {
      // Runtime retirement still disconnects the platform request.
    }
  }

  void Response(HttpTransportResponse response) {
    if (!responded_ && !terminal_) {
      responded_ = true;
      callbacks_.response(std::move(response));
      Drain();
    }
  }

  void Body(std::span<const std::byte> body) {
    if (terminal_ || body.empty()) {
      return;
    }
    if (streaming_ && body.size() > maximum_pending_bytes - pending_bytes_) {
      Fail(HttpErrorCode::Transport, "HuxerUI OHOS HTTP response exceeded the pending body limit");
      return;
    }
    chunks_.emplace_back(body.begin(), body.end());
    pending_bytes_ += body.size();
    Drain();
  }

  void Progress(std::uint64_t count) {
    if (!terminal_) {
      callbacks_.upload_progress(count);
    }
  }

  void Complete() {
    source_complete_ = true;
    if (!responded_) {
      Fail(HttpErrorCode::Transport, "HuxerUI OHOS HTTP completed before the response headers");
      return;
    }
    Drain();
  }

  void Fail(HttpErrorCode code, std::string message) {
    if (!terminal_) {
      Retire(!source_complete_);
      callbacks_.error({code, std::move(message)});
    }
  }

  void Retire(bool abort) {
    if (!std::exchange(terminal_, true)) {
      chunks_.clear();
      std::exchange(retire_, {})(abort && !source_complete_);
    }
  }

private:
  template <class Function> void OnApplicationThread(Function action) {
    auto self = shared_from_this();
    if (application_thread_ == std::this_thread::get_id()) {
      action(*self);
    } else {
      dispatch_([self = std::move(self), action] { action(*self); });
    }
  }

  void Drain() {
    if (draining_ || terminal_ || !responded_) {
      return;
    }
    draining_ = true;
    while (!terminal_ && requested_ && !chunks_.empty()) {
      requested_ = false;
      Bytes bytes = std::move(chunks_.front());
      chunks_.pop_front();
      pending_bytes_ -= bytes.size();
      callbacks_.body(std::move(bytes));
    }
    draining_ = false;
    if (!terminal_ && chunks_.empty() && source_complete_) {
      Retire(false);
      callbacks_.complete();
    }
  }

  const std::thread::id application_thread_ = std::this_thread::get_id();
  UiThreadDispatcher dispatch_;
  std::function<void(bool)> retire_;
  HttpTransportCallbacks callbacks_;
  std::deque<Bytes> chunks_;
  std::size_t pending_bytes_ = 0;
  const bool streaming_;
  bool responded_ = false;
  bool requested_ = false;
  bool draining_ = false;
  bool source_complete_ = false;
  bool terminal_ = false;
};

} // namespace

struct OhosHttpTransport::State {
  napi_env env;
  napi_ref host = nullptr;
  UiThreadDispatcher dispatch;
  std::map<std::string, std::shared_ptr<Operation>, std::less<>> operations;

  void Call(const char* name, std::span<const napi_value> args) const {
    napi_value receiver;
    napi_value method;
    Check(napi_get_reference_value(env, host, &receiver));
    Check(napi_get_named_property(env, receiver, name, &method));
    Check(napi_call_function(env, receiver, method, args.size(), args.data(), nullptr));
  }

  void Retire(const std::string& token, bool abort) noexcept {
    operations.erase(token);
    if (abort && host) {
      try {
        const HandleScope scope(env);
        const napi_value args[]{String(env, token)};
        Call("abort", args);
      } catch (...) {}
    }
  }
};

OhosHttpTransport::OhosHttpTransport(napi_env env, napi_value host, UiThreadDispatcher dispatch)
    : state_(std::make_shared<State>(State{env, nullptr, std::move(dispatch), {}})) {
  Check(napi_create_reference(env, host, 1, &state_->host));
}

OhosHttpTransport::~OhosHttpTransport() {
  Disconnect();
}

std::shared_ptr<HttpTransportOperation> OhosHttpTransport::Start(HttpRequest request, bool streaming,
                                                                HttpTransportCallbacks callbacks) {
  auto& state = *state_;
  if (!state.host) {
    return {};
  }
  const HandleScope scope(state.env);
  const auto token = std::to_string(++next_request);
  auto operation = std::make_shared<Operation>(state.dispatch, [weak = std::weak_ptr(state_), token](bool abort) {
    if (const auto state = weak.lock()) {
      state->Retire(token, abort);
    }
  }, std::move(callbacks), streaming);

  napi_value headers;
  Check(napi_create_array_with_length(state.env, request.headers.size(), &headers));
  for (std::size_t index = 0; index < request.headers.size(); ++index) {
    const auto& header = request.headers[index];
    napi_value pair;
    Check(napi_create_array_with_length(state.env, 2, &pair));
    Check(napi_set_element(state.env, pair, 0, String(state.env, header.name)));
    Check(napi_set_element(state.env, pair, 1, String(state.env, header.value)));
    Check(napi_set_element(state.env, headers, index, pair));
  }
  napi_value body;
  void* storage = nullptr;
  Check(napi_create_arraybuffer(state.env, request.body.size(), &storage, &body));
  if (!request.body.empty()) {
    std::memcpy(storage, request.body.data(), request.body.size());
  }
  napi_value incremental;
  Check(napi_get_boolean(state.env, streaming, &incremental));
  const napi_value args[]{String(state.env, token), String(state.env, request.url),
                         String(state.env, MethodName(request.method)), headers, body, incremental};
  state.operations.emplace(token, operation);
  try {
    state.Call("start", args);
  } catch (...) {
    operation->Retire(true);
    throw;
  }
  return operation;
}

napi_value OhosHttpTransport::Receive(napi_callback_info info) {
  auto& state = *state_;
  std::array<napi_value, 5> args;
  std::size_t count = args.size();
  Check(napi_get_cb_info(state.env, info, &count, args.data(), nullptr, nullptr));
  if (count < 2) {
    return nullptr;
  }
  const auto found = state.operations.find(ReadString(state.env, args[0]));
  if (found == state.operations.end()) {
    return nullptr;
  }
  // A continuation may cancel or remove the request while processing a callback.
  auto operation = found->second;
  switch (static_cast<Event>(Integer(state.env, args[1]))) {
  case Event::Headers:
    if (count == 5) {
      operation->Response({.url = ReadString(state.env, args[2]), .status_code = Integer(state.env, args[3]),
                           .headers = ReadHeaders(state.env, args[4]), .body_size = std::nullopt});
    }
    break;
  case Event::Body:
    if (count == 3) {
      void* bytes = nullptr;
      std::size_t size = 0;
      Check(napi_get_arraybuffer_info(state.env, args[2], &bytes, &size));
      operation->Body({static_cast<const std::byte*>(bytes), size});
    }
    break;
  case Event::Progress:
    if (count == 3) {
      std::int64_t size = 0;
      Check(napi_get_value_int64(state.env, args[2], &size));
      if (size > 0) {
        operation->Progress(static_cast<std::uint64_t>(size));
      }
    }
    break;
  case Event::Complete:
    operation->Complete();
    break;
  case Event::Error:
    if (count == 3) {
      const int code = Integer(state.env, args[2]);
      if (code == -2 || code == -3) {
        operation->Fail(HttpErrorCode::Unsupported, code == -2
            ? "HuxerUI streaming HTTP on HarmonyOS requires API 20"
            : "HuxerUI HarmonyOS HTTP did not provide incremental response headers");
      } else {
        const auto category = code == 1007900028 ? HttpErrorCode::Timeout : HttpErrorCode::Transport;
        operation->Fail(category, "HuxerUI OHOS HTTP failed (" + std::to_string(code) + ")");
      }
    }
    break;
  }
  return nullptr;
}

void OhosHttpTransport::Disconnect() noexcept {
  auto operations = std::exchange(state_->operations, {});
  for (const auto& [token, operation] : operations) {
    operation->Retire(true);
  }
  ReleaseHost();
}

void OhosHttpTransport::ReleaseHost() noexcept {
  if (state_->host) {
    napi_delete_reference(state_->env, std::exchange(state_->host, nullptr));
  }
}

} // namespace huxerui::detail
