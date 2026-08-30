#include <huxerui/web/platform_registry.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "platform_registry_internal.h"

namespace huxerui::web::detail {

namespace {

using emscripten::val;

struct WebEventState {
  explicit WebEventState(PlatformEventEmitter value) : events(std::move(value)) {}

  PlatformEventEmitter events;
};

struct WebResultState {
  explicit WebResultState(std::function<void(PlatformResult<PlatformPayload>)> value) : completion(std::move(value)) {}

  std::function<void(PlatformResult<PlatformPayload>)> completion;
};

template <class State> using RetainedState = std::shared_ptr<State>;
template <class State> using RetainedStateHandle = RetainedState<State>*;

val PlatformPayloadClass() {
  const val huxerui = val::module_property("HuxerUI");
  if (huxerui.isUndefined() || huxerui.isNull()) {
    throw std::logic_error("HuxerUI Web PlatformPayload runtime is unavailable");
  }
  const val type = huxerui["PlatformPayload"];
  if (type.isUndefined() || type.isNull()) {
    throw std::logic_error("HuxerUI Web PlatformPayload runtime is unavailable");
  }
  return type;
}

val PlatformBridge() {
  const val bridge = val::module_property("huxerUIWebPlatformBridge");
  if (bridge.isUndefined() || bridge.isNull()) {
    throw std::logic_error("HuxerUI Web platform bridge runtime is unavailable");
  }
  return bridge;
}

bool HasFunction(const val& object, const char* name) {
  return !object.isUndefined() && !object.isNull() && object[name].typeOf().as<std::string>() == "function";
}

void RequireFactory(const val& factory, std::string_view kind) {
  if (!HasFunction(factory, "create")) {
    throw std::invalid_argument("HuxerUI Web JavaScript " + std::string(kind) + " factory must provide create");
  }
}

val PlatformPayloadToJavaScript(const PlatformPayload& payload) {
  std::vector<ExternalTexture> external_textures;
  const Bytes encoded = payload.Encode(external_textures);
  if (!external_textures.empty()) {
    throw std::invalid_argument("HuxerUI Web JavaScript bridge does not support ExternalTexture payloads");
  }
  val bytes = val::global("Uint8Array").new_(encoded.size());
  if (!encoded.empty()) {
    bytes.call<void>(
        "set",
        val(emscripten::typed_memory_view(encoded.size(), reinterpret_cast<const unsigned char*>(encoded.data()))));
  }
  return PlatformPayloadClass().call<val>("decode", bytes);
}

PlatformPayload JavaScriptPlatformPayloadToCpp(const val& payload) {
  if (payload.isUndefined() || payload.isNull() || !payload.instanceof(PlatformPayloadClass())) {
    throw std::invalid_argument("HuxerUI Web platform boundary requires a PlatformPayload value");
  }
  const val source = payload.call<val>("encode");
  const std::size_t size = source["byteLength"].as<std::size_t>();
  Bytes encoded(size);
  if (!encoded.empty()) {
    val(emscripten::typed_memory_view(size, reinterpret_cast<unsigned char*>(encoded.data())))
        .call<void>("set", source);
  }
  return PlatformPayload::Decode(encoded);
}

template <class State> std::uintptr_t RetainForJavaScript(std::shared_ptr<State> state) {
  auto retained = std::make_unique<RetainedState<State>>(std::move(state));
  const std::uintptr_t handle = reinterpret_cast<std::uintptr_t>(retained.get());
  retained.release();
  return handle;
}

template <class State> std::shared_ptr<State> GetRetainedState(std::uintptr_t handle) {
  if (handle == 0) {
    return {};
  }
  return *reinterpret_cast<RetainedStateHandle<State>>(handle);
}

template <class State> std::shared_ptr<State> ReleaseRetainedState(std::uintptr_t handle) {
  if (handle == 0) {
    return {};
  }
  std::unique_ptr<RetainedState<State>> retained(reinterpret_cast<RetainedStateHandle<State>>(handle));
  return std::move(*retained);
}

val NewEventEndpoint(PlatformEventEmitter events) {
  const std::uintptr_t handle = RetainForJavaScript(std::make_shared<WebEventState>(std::move(events)));
  try {
    return PlatformBridge().call<val>("createEvents", handle);
  } catch (...) {
    static_cast<void>(ReleaseRetainedState<WebEventState>(handle));
    throw;
  }
}

val NewResultEndpoint(std::function<void(PlatformResult<PlatformPayload>)> completion) {
  const std::uintptr_t handle = RetainForJavaScript(std::make_shared<WebResultState>(std::move(completion)));
  try {
    return PlatformBridge().call<val>("createResult", handle);
  } catch (...) {
    static_cast<void>(ReleaseRetainedState<WebResultState>(handle));
    throw;
  }
}

void CloseEndpoint(val& endpoint) noexcept {
  if (endpoint.isUndefined() || endpoint.isNull()) {
    return;
  }
  try {
    endpoint.call<void>("close");
  } catch (...) {
  }
  endpoint = val::undefined();
}

val WebPlatformEmit(std::uintptr_t handle, std::string event, const val& payload) noexcept {
  try {
    if (const std::shared_ptr state = GetRetainedState<WebEventState>(handle)) {
      std::optional<PlatformPayload> result =
          state->events.Emit(std::move(event), JavaScriptPlatformPayloadToCpp(payload));
      return result.has_value() ? PlatformPayloadToJavaScript(*result) : val::undefined();
    }
    return val::undefined();
  } catch (...) {
    return val::null();
  }
}

void WebPlatformReleaseEvent(std::uintptr_t handle) noexcept {
  static_cast<void>(ReleaseRetainedState<WebEventState>(handle));
}

bool WebPlatformComplete(std::uintptr_t handle, const val& payload) noexcept {
  PlatformPayload value;
  try {
    value = JavaScriptPlatformPayloadToCpp(payload);
  } catch (...) {
    return false;
  }
  if (const std::shared_ptr state = ReleaseRetainedState<WebResultState>(handle); state && state->completion) {
    try {
      state->completion(std::move(value));
    } catch (...) {
    }
  }
  return true;
}

bool WebPlatformFail(std::uintptr_t handle, std::string code, std::string message, const val& details) noexcept {
  PlatformPayload value;
  try {
    value = JavaScriptPlatformPayloadToCpp(details);
  } catch (...) {
    return false;
  }
  if (const std::shared_ptr state = ReleaseRetainedState<WebResultState>(handle); state && state->completion) {
    try {
      state->completion(PlatformError{std::move(code), std::move(message), std::move(value)});
    } catch (...) {
    }
  }
  return true;
}

void WebPlatformReleaseResult(std::uintptr_t handle) noexcept {
  static_cast<void>(ReleaseRetainedState<WebResultState>(handle));
}

class JavaScriptInvocation final {
public:
  JavaScriptInvocation(val result, val cancellation)
      : result_(std::move(result)), cancellation_(std::move(cancellation)) {}

  ~JavaScriptInvocation() {
    CloseEndpoint(result_);
  }

  void Cancel() noexcept {
    if (!cancellation_.isUndefined() && !cancellation_.isNull()) {
      try {
        cancellation_.call<void>("call", val::undefined());
      } catch (...) {
      }
      cancellation_ = val::undefined();
    }
    CloseEndpoint(result_);
  }

private:
  val result_;
  val cancellation_;
};

class JavaScriptInstanceState final {
public:
  JavaScriptInstanceState(val instance, val emitter) : instance_(std::move(instance)), emitter_(std::move(emitter)) {}

  ~JavaScriptInstanceState() {
    Dispose();
  }

  std::function<void()> Invoke(std::string method, PlatformPayload arguments,
                               std::function<void(PlatformResult<PlatformPayload>)> completion) {
    if (disposed_) {
      throw std::logic_error("HuxerUI Web JavaScript platform instance is disposed");
    }
    val result = NewResultEndpoint(std::move(completion));
    val cancellation = val::undefined();
    try {
      cancellation = instance_.call<val>("invoke", std::move(method), PlatformPayloadToJavaScript(arguments), result);
      if (!cancellation.isUndefined() && !cancellation.isNull() &&
          cancellation.typeOf().as<std::string>() != "function") {
        throw std::logic_error("HuxerUI Web JavaScript platform invocation returned an invalid cancellation value");
      }
    } catch (...) {
      CloseEndpoint(result);
      throw;
    }
    auto invocation = std::make_shared<JavaScriptInvocation>(std::move(result), std::move(cancellation));
    return [invocation] { invocation->Cancel(); };
  }

  void Dispose() noexcept {
    if (disposed_) {
      return;
    }
    disposed_ = true;
    CloseEndpoint(emitter_);
    try {
      if (HasFunction(instance_, "dispose")) {
        instance_.call<void>("dispose");
      }
    } catch (...) {
    }
    instance_ = val::undefined();
  }

  [[nodiscard]] const val& Instance() const noexcept {
    return instance_;
  }

  [[nodiscard]] bool Disposed() const noexcept {
    return disposed_;
  }

private:
  val instance_;
  val emitter_;
  bool disposed_ = false;
};

void ConnectInstance(const huxerui::detail::PlatformChannelEndpoint& endpoint,
                     const std::shared_ptr<JavaScriptInstanceState>& instance) {
  endpoint.Connect({
      .invoke = [instance](std::string method, PlatformPayload arguments,
                           std::function<void(PlatformResult<PlatformPayload>)> completion) {
        return instance->Invoke(std::move(method), std::move(arguments), std::move(completion));
      },
      .dispose = [instance] { instance->Dispose(); },
  });
}

std::shared_ptr<JavaScriptInstanceState>
CreateInstance(const val& factory, PlatformPayload initial_value, PlatformEventEmitter events,
               bool invocation_required, std::string_view kind) {
  RequireFactory(factory, kind);
  val emitter = NewEventEndpoint(std::move(events));
  val instance = val::undefined();
  try {
    instance = factory.call<val>("create", PlatformPayloadToJavaScript(initial_value), emitter);
    if (instance.isUndefined() || instance.isNull()) {
      throw std::logic_error("HuxerUI Web JavaScript " + std::string(kind) + " factory returned an empty instance");
    }
    if (!HasFunction(instance, "dispose")) {
      throw std::logic_error("HuxerUI Web JavaScript " + std::string(kind) + " instance must provide dispose");
    }
    if (invocation_required && !HasFunction(instance, "invoke")) {
      throw std::logic_error("HuxerUI Web JavaScript " + std::string(kind) + " instance must provide invoke");
    }
  } catch (...) {
    CloseEndpoint(emitter);
    try {
      if (!instance.isUndefined() && !instance.isNull() && HasFunction(instance, "dispose")) {
        instance.call<void>("dispose");
      }
    } catch (...) {
    }
    throw;
  }
  return std::make_shared<JavaScriptInstanceState>(std::move(instance), std::move(emitter));
}

} // namespace

class JavaScriptPlatformViewInstance final {
public:
  ~JavaScriptPlatformViewInstance() {
    channel.Close();
    if (state) {
      state->Dispose();
    }
  }

  std::shared_ptr<JavaScriptInstanceState> state;
  PlatformChannel channel;
  val element = val::undefined();
};

PlatformChannel CreateJavaScriptPlatformModule(PlatformAdapter& adapter, const val& factory, PlatformPayload options) {
  const huxerui::detail::PlatformChannelEndpoint endpoint = huxerui::detail::MakePlatformChannelEndpoint(adapter);
  std::shared_ptr state = CreateInstance(factory, std::move(options), endpoint.Events(), true, "PlatformModule");
  ConnectInstance(endpoint, state);
  return endpoint.Channel();
}

std::shared_ptr<JavaScriptPlatformViewInstance>
CreateJavaScriptPlatformView(PlatformAdapter& adapter, const val& factory, PlatformPayload properties,
                             PlatformEventEmitter events, bool update_required, bool channel_required) {
  auto result = std::make_shared<JavaScriptPlatformViewInstance>();
  result->state = CreateInstance(factory, std::move(properties), std::move(events), channel_required, "PlatformView");
  if (update_required && !HasFunction(result->state->Instance(), "update")) {
    throw std::logic_error("HuxerUI Web JavaScript PlatformView instance must provide update");
  }
  result->element = result->state->Instance()["element"];
  if (result->element.isUndefined() || result->element.isNull()) {
    throw std::logic_error("HuxerUI Web JavaScript PlatformView instance must provide element");
  }
  if (channel_required) {
    const huxerui::detail::PlatformChannelEndpoint endpoint = huxerui::detail::MakePlatformChannelEndpoint(adapter);
    ConnectInstance(endpoint, result->state);
    result->channel = endpoint.Channel();
  }
  return result;
}

val GetJavaScriptPlatformView(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance) {
  return instance ? instance->element : val::undefined();
}

void UpdateJavaScriptPlatformView(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance,
                                  PlatformPayload properties) {
  if (!instance || !instance->state || instance->state->Disposed()) {
    throw std::logic_error("HuxerUI Web JavaScript PlatformView instance is disposed");
  }
  try {
    instance->state->Instance().call<void>("update", PlatformPayloadToJavaScript(properties));
  } catch (...) {
    throw std::logic_error("HuxerUI Web JavaScript PlatformView update failed");
  }
}

void DisposeJavaScriptPlatformView(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance) noexcept {
  if (!instance) {
    return;
  }
  instance->channel.Close();
  if (instance->state) {
    instance->state->Dispose();
  }
  instance->element = val::undefined();
}

PlatformChannel GetJavaScriptPlatformViewChannel(const std::shared_ptr<JavaScriptPlatformViewInstance>& instance) {
  return instance ? instance->channel : PlatformChannel{};
}

} // namespace huxerui::web::detail

EMSCRIPTEN_BINDINGS(huxerui_web_platform_registry) {
  emscripten::function("huxeruiWebPlatformEmit", &huxerui::web::detail::WebPlatformEmit);
  emscripten::function("huxeruiWebPlatformReleaseEvent", &huxerui::web::detail::WebPlatformReleaseEvent);
  emscripten::function("huxeruiWebPlatformComplete", &huxerui::web::detail::WebPlatformComplete);
  emscripten::function("huxeruiWebPlatformFail", &huxerui::web::detail::WebPlatformFail);
  emscripten::function("huxeruiWebPlatformReleaseResult", &huxerui::web::detail::WebPlatformReleaseResult);
}
