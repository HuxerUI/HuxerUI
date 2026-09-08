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

#include "application/platform_registry_internal.h"
#include "web_file_internal.h"

namespace huxerui::web::detail {

namespace {

using emscripten::val;
using PlatformResultCompletion = std::function<void(PlatformResult<PlatformPayload>)>;

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
  PlatformPayload::Envelope envelope = payload.Encode();
  if (!envelope.buffer_references.empty()) {
    throw std::invalid_argument("HuxerUI Web bridge does not support BufferReference");
  }
  if (!envelope.external_textures.empty()) {
    throw std::invalid_argument("HuxerUI Web JavaScript bridge does not support ExternalTexture payloads");
  }
  val bytes = val::global("Uint8Array").new_(envelope.bytes.size());
  if (!envelope.bytes.empty()) {
    const auto* data = reinterpret_cast<const unsigned char*>(envelope.bytes.data());
    bytes.call<void>("set", val(emscripten::typed_memory_view(envelope.bytes.size(), data)));
  }
  const val bridge = PlatformBridge();
  val references = val::array();
  for (const FileReference& reference : envelope.file_references) {
    const huxerui::detail::WebFileReferenceProjection projection = huxerui::detail::ProjectWebFileReference(reference);
    auto handle = std::make_unique<FileReference>(reference);
    val wrapper = bridge.call<val>("createFileReference", reinterpret_cast<std::uintptr_t>(handle.get()),
                                   projection.capability_key, projection.source, projection.is_file);
    handle.release();
    references.call<void>("push", wrapper);
  }
  return bridge.call<val>("decodeEnvelope", bytes, references);
}

PlatformPayload JavaScriptPlatformPayloadToCpp(const val& payload) {
  if (payload.isUndefined() || payload.isNull() || !payload.instanceof(PlatformPayloadClass())) {
    throw std::invalid_argument("HuxerUI Web platform boundary requires a PlatformPayload value");
  }
  const val envelope = PlatformBridge().call<val>("encodeEnvelope", payload);
  const val source = envelope["bytes"];
  const std::size_t size = source["byteLength"].as<std::size_t>();
  PlatformPayload::Envelope decoded;
  decoded.bytes.resize(size);
  if (!decoded.bytes.empty()) {
    val(emscripten::typed_memory_view(size, reinterpret_cast<unsigned char*>(decoded.bytes.data())))
        .call<void>("set", source);
  }
  const val references = envelope["fileReferences"];
  const std::size_t reference_count = references["length"].as<std::size_t>();
  decoded.file_references.reserve(reference_count);
  for (std::size_t index = 0; index < reference_count; ++index) {
    const std::uintptr_t handle = PlatformBridge().call<std::uintptr_t>("retainFileReference", references[index]);
    const std::unique_ptr<FileReference> reference(reinterpret_cast<FileReference*>(handle));
    if (!reference) {
      throw std::invalid_argument("HuxerUI Web FileReference could not be retained");
    }
    decoded.file_references.push_back(*reference);
  }
  return PlatformPayload::Decode(decoded);
}

val NewEventEndpoint(PlatformEventEmitter events) {
  auto retained = std::make_unique<PlatformEventEmitter>(std::move(events));
  const std::uintptr_t handle = reinterpret_cast<std::uintptr_t>(retained.get());
  val endpoint = PlatformBridge().call<val>("createEvents", handle);
  retained.release();
  return endpoint;
}

val NewResultEndpoint(PlatformResultCompletion completion) {
  auto retained = std::make_unique<PlatformResultCompletion>(std::move(completion));
  const std::uintptr_t handle = reinterpret_cast<std::uintptr_t>(retained.get());
  val endpoint = PlatformBridge().call<val>("createResult", handle);
  retained.release();
  return endpoint;
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
    if (handle == 0) {
      return val::undefined();
    }
    // Emit can synchronously reenter JavaScript and close the endpoint, so retain a callable copy first.
    PlatformEventEmitter events = *reinterpret_cast<PlatformEventEmitter*>(handle);
    std::optional<PlatformPayload> result =
        events.Emit(std::move(event), JavaScriptPlatformPayloadToCpp(payload));
    return result.has_value() ? PlatformPayloadToJavaScript(*result) : val::undefined();
  } catch (...) {
    return val::null();
  }
}

void WebPlatformReleaseEvent(std::uintptr_t handle) noexcept {
  delete reinterpret_cast<PlatformEventEmitter*>(handle);
}

bool WebPlatformComplete(std::uintptr_t handle, const val& payload) noexcept {
  PlatformPayload value;
  try {
    value = JavaScriptPlatformPayloadToCpp(payload);
  } catch (...) {
    return false;
  }
  std::unique_ptr<PlatformResultCompletion> completion(reinterpret_cast<PlatformResultCompletion*>(handle));
  if (completion && *completion) {
    try {
      (*completion)(std::move(value));
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
  std::unique_ptr<PlatformResultCompletion> completion(reinterpret_cast<PlatformResultCompletion*>(handle));
  if (completion && *completion) {
    try {
      (*completion)(PlatformError{std::move(code), std::move(message), std::move(value)});
    } catch (...) {
    }
  }
  return true;
}

void WebPlatformReleaseResult(std::uintptr_t handle) noexcept {
  delete reinterpret_cast<PlatformResultCompletion*>(handle);
}

void WebFileReferenceRelease(std::uintptr_t handle) noexcept {
  delete reinterpret_cast<FileReference*>(handle);
}

std::uintptr_t WebFileReferenceRetain(std::uintptr_t handle) {
  if (handle == 0) {
    return 0;
  }
  return reinterpret_cast<std::uintptr_t>(new FileReference(*reinterpret_cast<const FileReference*>(handle)));
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
  emscripten::function("huxeruiWebFileReferenceRelease", &huxerui::web::detail::WebFileReferenceRelease);
  emscripten::function("huxeruiWebFileReferenceRetain", &huxerui::web::detail::WebFileReferenceRetain);
}
