#include <huxerui/platform_registry.h>

#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <huxerui/platform_adapter.h>

#include "external_texture_internal.h"
#include "platform_registry_internal.h"

namespace huxerui {

namespace detail {

namespace {

thread_local PlatformRegistry* lifecycle_platform_registry = nullptr;

} // namespace

class PlatformChannelState final : public std::enable_shared_from_this<PlatformChannelState> {
public:
  struct PendingCall {
    std::function<void(PlatformResult<PlatformPayload>)> completion;
    std::function<void()> cancel;
  };

  PlatformChannelState(UIThreadDispatcher ui_thread_dispatcher,
                       std::shared_ptr<ExternalTextureSurface> external_texture_surface)
      : dispatch_to_ui_thread_(std::move(ui_thread_dispatcher)), texture_surface_(std::move(external_texture_surface)) {
    if (!dispatch_to_ui_thread_) {
      throw std::invalid_argument("HuxerUI PlatformChannel UI thread dispatcher must not be empty");
    }
  }

  ~PlatformChannelState() {
    Close();
  }

  void Connect(PlatformChannelTransport transport) {
    if (!transport.invoke) {
      throw std::invalid_argument("HuxerUI PlatformChannel transport must provide invoke");
    }
    std::lock_guard lock(mutex_);
    if (!open_) {
      throw std::logic_error("HuxerUI PlatformChannel is closed");
    }
    if (connected_) {
      throw std::logic_error("HuxerUI PlatformChannel transport is already connected");
    }
    transport_ = std::move(transport);
    connected_ = true;
  }

  [[nodiscard]] bool IsOpen() const noexcept {
    std::lock_guard lock(mutex_);
    return open_;
  }

  PlatformRequestId Invoke(std::string method, PlatformPayload arguments,
                           std::function<void(PlatformResult<PlatformPayload>)> completion) {
    if (method.empty()) {
      throw std::invalid_argument("HuxerUI PlatformChannel method must not be empty");
    }
    if (!IsValidUtf8(method)) {
      throw std::invalid_argument("HuxerUI PlatformChannel method must contain valid UTF-8");
    }
    if (!completion) {
      throw std::invalid_argument("HuxerUI PlatformChannel completion must not be empty");
    }

    UIThreadDispatcher dispatcher;
    PlatformRequestId request = 0;
    {
      std::lock_guard lock(mutex_);
      if (!open_) {
        throw std::logic_error("HuxerUI PlatformChannel is closed");
      }
      if (!connected_) {
        throw std::logic_error("HuxerUI PlatformChannel transport is not connected");
      }
      BindExternalTextures(arguments, texture_surface_);
      if (next_request_ == 0) {
        throw std::logic_error("HuxerUI PlatformChannel request identity space is exhausted");
      }
      request = next_request_++;
      pending_.emplace(request, PendingCall{std::move(completion), {}});
      dispatcher = dispatch_to_ui_thread_;
    }

    const std::weak_ptr weak_state = weak_from_this();
    try {
      dispatcher([weak_state, request, method = std::move(method), arguments = std::move(arguments)]() mutable {
        if (const std::shared_ptr state = weak_state.lock()) {
          state->InvokeTransport(request, std::move(method), std::move(arguments));
        }
      });
    } catch (...) {
      std::lock_guard lock(mutex_);
      pending_.erase(request);
      throw;
    }
    return request;
  }

  void On(std::string event, std::function<void(const PlatformPayload&)> handler) {
    if (event.empty()) {
      throw std::invalid_argument("HuxerUI PlatformChannel event must not be empty");
    }
    if (!IsValidUtf8(event)) {
      throw std::invalid_argument("HuxerUI PlatformChannel event must contain valid UTF-8");
    }
    if (!handler) {
      throw std::invalid_argument("HuxerUI PlatformChannel event handler must not be empty");
    }
    std::lock_guard lock(mutex_);
    if (!open_) {
      throw std::logic_error("HuxerUI PlatformChannel is closed");
    }
    if (!events_.emplace(std::move(event), std::move(handler)).second) {
      throw std::invalid_argument("HuxerUI PlatformChannel event was registered more than once");
    }
  }

  bool Cancel(PlatformRequestId request) {
    if (request == 0) {
      return false;
    }
    std::function<void()> cancel;
    {
      std::lock_guard lock(mutex_);
      const auto found = pending_.find(request);
      if (found == pending_.end()) {
        return false;
      }
      cancel = std::move(found->second.cancel);
      pending_.erase(found);
    }
    if (cancel) {
      UIThreadDispatcher dispatcher = dispatch_to_ui_thread_;
      try {
        dispatcher([cancel = std::move(cancel)] {
          try {
            cancel();
          } catch (...) {
          }
        });
      } catch (...) {
      }
    }
    return true;
  }

  void Close() noexcept {
    std::vector<std::function<void()>> cancellations;
    std::function<void()> dispose;
    UIThreadDispatcher dispatcher;
    {
      std::lock_guard lock(mutex_);
      if (!open_) {
        return;
      }
      open_ = false;
      cancellations.reserve(pending_.size());
      for (auto& [request, call] : pending_) {
        static_cast<void>(request);
        if (call.cancel) {
          cancellations.push_back(std::move(call.cancel));
        }
      }
      pending_.clear();
      events_.clear();
      queued_events_.clear();
      event_delivery_scheduled_ = false;
      dispose = std::move(transport_.dispose);
      transport_.invoke = {};
      connected_ = false;
      dispatcher = dispatch_to_ui_thread_;
    }
    if (cancellations.empty() && !dispose) {
      return;
    }
    try {
      dispatcher([cancellations = std::move(cancellations), dispose = std::move(dispose)] {
        for (const auto& cancel : cancellations) {
          try {
            cancel();
          } catch (...) {
          }
        }
        if (dispose) {
          try {
            dispose();
          } catch (...) {
          }
        }
      });
    } catch (...) {
    }
  }

  void PostEvent(std::string event, PlatformPayload payload) {
    if (event.empty() || !IsValidUtf8(event)) {
      return;
    }
    UIThreadDispatcher dispatcher;
    {
      std::lock_guard lock(mutex_);
      if (!open_) {
        return;
      }
      try {
        BindExternalTextures(payload, texture_surface_);
      } catch (...) {
        return;
      }
      queued_events_.emplace_back(std::move(event), std::move(payload));
      if (event_delivery_scheduled_) {
        return;
      }
      event_delivery_scheduled_ = true;
      dispatcher = dispatch_to_ui_thread_;
    }
    const std::weak_ptr weak_state = weak_from_this();
    try {
      dispatcher([weak_state] {
        if (const std::shared_ptr state = weak_state.lock()) {
          state->DrainEvents();
        }
      });
    } catch (...) {
      std::lock_guard lock(mutex_);
      queued_events_.clear();
      event_delivery_scheduled_ = false;
    }
  }

private:
  void InvokeTransport(PlatformRequestId request, std::string method, PlatformPayload arguments) {
    std::function<std::function<void()>(std::string, PlatformPayload,
                                        std::function<void(PlatformResult<PlatformPayload>)>)>
        invoke;
    {
      std::lock_guard lock(mutex_);
      if (!open_ || !pending_.contains(request)) {
        return;
      }
      invoke = transport_.invoke;
    }

    const std::weak_ptr weak_state = weak_from_this();
    std::function<void()> cancel;
    try {
      cancel = invoke(std::move(method), std::move(arguments),
                      [weak_state, request](PlatformResult<PlatformPayload> result) mutable {
                        if (const std::shared_ptr state = weak_state.lock()) {
                          state->PostResult(request, std::move(result));
                        }
                      });
    } catch (...) {
      PostResult(request, PlatformError{
                              "huxerui/call-failed",
                              "HuxerUI platform call failed before producing a result",
                              {},
                          });
      return;
    }

    std::function<void()> cancel_now;
    {
      std::lock_guard lock(mutex_);
      const auto found = pending_.find(request);
      if (found != pending_.end()) {
        found->second.cancel = std::move(cancel);
      } else {
        cancel_now = std::move(cancel);
      }
    }
    if (cancel_now) {
      try {
        cancel_now();
      } catch (...) {
      }
    }
  }

  void PostResult(PlatformRequestId request, PlatformResult<PlatformPayload> result) {
    UIThreadDispatcher dispatcher;
    {
      std::lock_guard lock(mutex_);
      if (!open_ || !pending_.contains(request)) {
        return;
      }
      try {
        if (const auto* payload = std::get_if<PlatformPayload>(&result)) {
          BindExternalTextures(*payload, texture_surface_);
        } else {
          BindExternalTextures(std::get<PlatformError>(result).details, texture_surface_);
        }
      } catch (...) {
        result = PlatformError{
            "huxerui/invalid-result",
            "HuxerUI platform call returned an external texture from another platform surface",
            {},
        };
      }
      dispatcher = dispatch_to_ui_thread_;
    }
    const std::weak_ptr weak_state = weak_from_this();
    try {
      dispatcher([weak_state, request, result = std::move(result)]() mutable {
        if (const std::shared_ptr state = weak_state.lock()) {
          state->DeliverResult(request, std::move(result));
        }
      });
    } catch (...) {
      std::lock_guard lock(mutex_);
      pending_.erase(request);
    }
  }

  void DeliverResult(PlatformRequestId request, PlatformResult<PlatformPayload> result) {
    std::function<void(PlatformResult<PlatformPayload>)> completion;
    {
      std::lock_guard lock(mutex_);
      if (!open_) {
        return;
      }
      const auto found = pending_.find(request);
      if (found == pending_.end()) {
        return;
      }
      completion = std::move(found->second.completion);
      pending_.erase(found);
    }
    if (auto* error = std::get_if<PlatformError>(&result)) {
      try {
        if (error->code.empty()) {
          throw std::invalid_argument("empty code");
        }
        if (!IsValidUtf8(error->code) || !IsValidUtf8(error->message)) {
          throw std::invalid_argument("HuxerUI PlatformChannel error must contain valid UTF-8");
        }
      } catch (...) {
        result = PlatformError{
            "huxerui/invalid-error",
            "HuxerUI platform call returned an invalid error payload",
            {},
        };
      }
    }
    try {
      completion(std::move(result));
    } catch (...) {
    }
  }

  void DrainEvents() {
    while (true) {
      std::function<void(const PlatformPayload&)> handler;
      PlatformPayload payload;
      {
        std::lock_guard lock(mutex_);
        if (!open_ || queued_events_.empty()) {
          queued_events_.clear();
          event_delivery_scheduled_ = false;
          return;
        }
        auto [event, next_payload] = std::move(queued_events_.front());
        queued_events_.pop_front();
        payload = std::move(next_payload);
        const auto found = events_.find(event);
        if (found != events_.end()) {
          handler = found->second;
        }
      }
      if (handler) {
        try {
          handler(payload);
        } catch (...) {
        }
      }
    }
  }

  UIThreadDispatcher dispatch_to_ui_thread_;
  std::shared_ptr<ExternalTextureSurface> texture_surface_;
  PlatformChannelTransport transport_;
  mutable std::mutex mutex_;
  PlatformRequestId next_request_ = 1;
  bool open_ = true;
  bool connected_ = false;
  bool event_delivery_scheduled_ = false;
  std::unordered_map<PlatformRequestId, PendingCall> pending_;
  std::unordered_map<std::string, std::function<void(const PlatformPayload&)>> events_;
  std::deque<std::pair<std::string, PlatformPayload>> queued_events_;
};

PlatformChannelEndpoint MakePlatformChannelEndpoint(UIThreadDispatcher dispatch_to_ui_thread,
                                                    std::shared_ptr<ExternalTextureSurface> texture_surface) {
  return PlatformChannelEndpoint(
      std::make_shared<PlatformChannelState>(std::move(dispatch_to_ui_thread), std::move(texture_surface)));
}

PlatformChannel PlatformChannelEndpoint::Channel() const {
  return PlatformChannel(state_);
}

PlatformEventEmitter PlatformChannelEndpoint::Events() const {
  const std::weak_ptr weak_state = state_;
  return MakePlatformEventEmitter({}, [weak_state](std::string event, PlatformPayload payload) {
    if (const std::shared_ptr state = weak_state.lock()) {
      state->PostEvent(std::move(event), std::move(payload));
    }
  });
}

void PlatformChannelEndpoint::Connect(PlatformChannelTransport transport) const {
  if (!state_) {
    throw std::logic_error("HuxerUI PlatformChannel endpoint is empty");
  }
  state_->Connect(std::move(transport));
}

void PlatformChannelEndpoint::Close() const noexcept {
  if (state_) {
    state_->Close();
  }
}

void PlatformRegistry::RegisterValue(std::string name, std::unique_ptr<Registration> registration) {
  if (frozen_) {
    throw std::logic_error("HuxerUI platform registry is already frozen");
  }
  if (name.empty()) {
    throw std::invalid_argument("HuxerUI platform registration name must not be empty");
  }
  if (!IsValidUtf8(name)) {
    throw std::invalid_argument("HuxerUI platform registration name must contain valid UTF-8");
  }
  if (!registrations_.emplace(std::move(name), std::move(registration)).second) {
    throw std::logic_error("HuxerUI platform registration name was registered more than once");
  }
}

void PlatformRegistry::RegisterViewValue(std::string name, std::type_index properties_type,
                                         std::type_index controller_type, PlatformViewFactoryRegistration factory) {
  if (!factory.factory || factory.type == typeid(void)) {
    throw std::invalid_argument("HuxerUI PlatformView factory must not be empty");
  }
  RegisterValue(std::move(name),
                std::make_unique<ViewRegistration>(properties_type, controller_type, std::move(factory)));
}

PlatformRegistry::ModuleInstance PlatformRegistry::OpenModuleValue(std::string name, std::type_index module_type,
                                                                   std::type_index options_type,
                                                                   const PlatformValue* options) {
  const auto found = registrations_.find(name);
  if (found == registrations_.end()) {
    throw std::logic_error("HuxerUI PlatformModule is not registered: " + name);
  }
  Registration& registration = *found->second;
  if (registration.kind != Kind::Module) {
    throw std::logic_error("HuxerUI platform registration is not a PlatformModule: " + name);
  }
  if (registration.primary_type != module_type || registration.secondary_type != options_type) {
    throw std::logic_error("HuxerUI PlatformModule registration has incompatible C++ types: " + name);
  }
  return static_cast<ModuleRegistration&>(registration).Open(*adapter_, options);
}

PlatformViewFactoryRegistration PlatformRegistry::FindViewValue(std::string_view name, std::type_index properties_type,
                                                                std::type_index controller_type,
                                                                std::type_index factory_type) const {
  const auto found = registrations_.find(std::string(name));
  if (found == registrations_.end()) {
    throw std::logic_error("HuxerUI PlatformView is not registered: " + std::string(name));
  }
  const Registration& registration = *found->second;
  if (registration.kind != Kind::View) {
    throw std::logic_error("HuxerUI platform registration is not a PlatformView: " + std::string(name));
  }
  if (registration.primary_type != properties_type || registration.secondary_type != controller_type) {
    throw std::logic_error("HuxerUI PlatformView registration has incompatible C++ types: " + std::string(name));
  }
  const auto& view = static_cast<const ViewRegistration&>(registration);
  if (view.factory.type != factory_type) {
    throw std::logic_error("HuxerUI PlatformView is registered for a different platform adapter: " + std::string(name));
  }
  return view.factory;
}

PlatformRegistry* CurrentLifecyclePlatformRegistry() noexcept {
  return lifecycle_platform_registry;
}

PlatformRegistry* SetLifecyclePlatformRegistry(PlatformRegistry* registry) noexcept {
  PlatformRegistry* previous = lifecycle_platform_registry;
  lifecycle_platform_registry = registry;
  return previous;
}

PlatformEventEmitter MakePlatformEventEmitter(std::function<void(std::type_index, PlatformValue)> emit_direct,
                                              std::function<void(std::string, PlatformPayload)> emit_payload) {
  return PlatformEventEmitter(std::move(emit_direct), std::move(emit_payload));
}

} // namespace detail

bool PlatformChannel::IsOpen() const noexcept {
  return state_ && state_->IsOpen();
}

PlatformRequestId PlatformChannel::Invoke(std::string method, PlatformPayload arguments,
                                          std::function<void(PlatformResult<PlatformPayload>)> completion) const {
  if (!state_) {
    throw std::logic_error("HuxerUI PlatformChannel is empty");
  }
  return state_->Invoke(std::move(method), std::move(arguments), std::move(completion));
}

void PlatformChannel::On(std::string event, std::function<void(const PlatformPayload&)> handler) const {
  if (!state_) {
    throw std::logic_error("HuxerUI PlatformChannel is empty");
  }
  state_->On(std::move(event), std::move(handler));
}

bool PlatformChannel::Cancel(PlatformRequestId request) const {
  return state_ && state_->Cancel(request);
}

void PlatformChannel::Close() const noexcept {
  if (state_) {
    state_->Close();
  }
}

void PlatformEventEmitter::EmitValue(std::type_index key, PlatformValue value) const {
  if (emit_direct_) {
    emit_direct_(key, std::move(value));
  }
}

void PlatformEventEmitter::Emit(std::string name, PlatformPayload payload) const {
  if (emit_payload_) {
    emit_payload_(std::move(name), std::move(payload));
  }
}

} // namespace huxerui
