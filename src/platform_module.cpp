#include <huxerui/platform_module.h>

#include <cmath>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace huxerui {

struct PlatformPayload::Data {
  using Value = std::variant<bool, std::int64_t, double, std::string, Bytes, List, Object>;

  explicit Data(Value value) : value(std::move(value)) {}

  Value value;
};

namespace {

constexpr std::size_t max_payload_depth = 64;

bool IsValidUtf8(std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<std::uint8_t>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      value = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      value = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      value = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<std::uint8_t>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      value = (value << 6U) | (continuation & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
      return false;
    }
    index += length;
  }
  return true;
}

void ValidatePayload(const PlatformPayload& payload, std::size_t depth) {
  if (depth > max_payload_depth) {
    throw std::invalid_argument("HuxerUI PlatformPayload exceeds the maximum nesting depth");
  }

  switch (payload.Kind()) {
  case PlatformPayloadKind::Null:
  case PlatformPayloadKind::Boolean:
  case PlatformPayloadKind::Integer:
  case PlatformPayloadKind::Double:
  case PlatformPayloadKind::String:
  case PlatformPayloadKind::Bytes:
    break;
  case PlatformPayloadKind::List:
    for (const PlatformPayload& child : payload.AsList()) {
      ValidatePayload(child, depth + 1);
    }
    break;
  case PlatformPayloadKind::Object:
    for (const auto& [key, child] : payload.AsObject()) {
      if (!IsValidUtf8(key)) {
        throw std::invalid_argument("HuxerUI PlatformPayload object key must contain valid UTF-8");
      }
      ValidatePayload(child, depth + 1);
    }
    break;
  }
}

void ValidatePayload(const PlatformPayload& payload) {
  ValidatePayload(payload, 0);
}

} // namespace

PlatformPayload::PlatformPayload(bool value) : data_(std::make_shared<Data>(value)) {}

PlatformPayload::PlatformPayload(std::int64_t value) : data_(std::make_shared<Data>(value)) {}

PlatformPayload::PlatformPayload(double value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("HuxerUI PlatformPayload double must be finite");
  }
  data_ = std::make_shared<Data>(value);
}

PlatformPayload::PlatformPayload(std::string value) {
  if (!IsValidUtf8(value)) {
    throw std::invalid_argument("HuxerUI PlatformPayload string must contain valid UTF-8");
  }
  data_ = std::make_shared<Data>(std::move(value));
}

PlatformPayload::PlatformPayload(std::string_view value) : PlatformPayload(std::string(value)) {}

PlatformPayload::PlatformPayload(const char* value) {
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI PlatformPayload string pointer must not be null");
  }
  *this = PlatformPayload(std::string_view(value));
}

PlatformPayload::PlatformPayload(Bytes value) : data_(std::make_shared<Data>(std::move(value))) {}

PlatformPayload::PlatformPayload(List value) : data_(std::make_shared<Data>(std::move(value))) {
  ValidatePayload(*this);
}

PlatformPayload::PlatformPayload(Object value) : data_(std::make_shared<Data>(std::move(value))) {
  ValidatePayload(*this);
}

PlatformPayloadKind PlatformPayload::Kind() const noexcept {
  if (!data_) {
    return PlatformPayloadKind::Null;
  }
  return static_cast<PlatformPayloadKind>(data_->value.index() + 1);
}

bool PlatformPayload::AsBoolean() const {
  return std::get<bool>(RequireData().value);
}

std::int64_t PlatformPayload::AsInteger() const {
  return std::get<std::int64_t>(RequireData().value);
}

double PlatformPayload::AsDouble() const {
  return std::get<double>(RequireData().value);
}

std::string_view PlatformPayload::AsString() const {
  return std::get<std::string>(RequireData().value);
}

std::span<const std::byte> PlatformPayload::AsBytes() const {
  return std::get<Bytes>(RequireData().value);
}

const PlatformPayload::List& PlatformPayload::AsList() const {
  return std::get<List>(RequireData().value);
}

const PlatformPayload::Object& PlatformPayload::AsObject() const {
  return std::get<Object>(RequireData().value);
}

const PlatformPayload::Data& PlatformPayload::RequireData() const {
  if (!data_) {
    throw std::bad_variant_access();
  }
  return *data_;
}

bool PlatformPayload::operator==(const PlatformPayload& other) const {
  if (data_ == other.data_) {
    return true;
  }
  return data_ && other.data_ && data_->value == other.data_->value;
}

struct PlatformInstance::State : std::enable_shared_from_this<PlatformInstance::State> {
  struct PendingCall {
    std::function<void(PlatformResult<PlatformPayload>)> completion;
    std::function<void()> cancel;
  };

  explicit State(UIThreadDispatcher ui_thread_dispatcher) : dispatch_to_ui_thread(std::move(ui_thread_dispatcher)) {}

  PlatformEventSink EventSink() {
    const std::weak_ptr<State> weak = weak_from_this();
    return [weak](std::string event, PlatformPayload payload) {
      if (const std::shared_ptr<State> state = weak.lock()) {
        state->PostEvent(std::move(event), std::move(payload));
      }
    };
  }

  PlatformRequestId
  Call(std::string method, PlatformPayload arguments, std::function<void(PlatformResult<PlatformPayload>)> completion) {
    if (method.empty()) {
      throw std::invalid_argument("HuxerUI platform module method must not be empty");
    }
    static_cast<void>(PlatformPayload(method));
    if (!completion) {
      throw std::invalid_argument("HuxerUI platform module completion must not be empty");
    }

    std::function<std::function<void()>(std::string, PlatformPayload, PlatformResultSink)> call;
    PlatformRequestId request = 0;
    {
      std::lock_guard lock(mutex);
      if (!open) {
        throw std::logic_error("HuxerUI platform module instance is closed");
      }
      if (next_request == 0) {
        throw std::logic_error("HuxerUI platform module request identity space is exhausted");
      }
      request = next_request++;
      pending.emplace(request, PendingCall{std::move(completion), {}});
      call = native.call;
    }

    const std::weak_ptr<State> weak = weak_from_this();
    PlatformResultSink result_sink = [weak, request](PlatformResult<PlatformPayload> result) {
      if (const std::shared_ptr<State> state = weak.lock()) {
        state->PostResult(request, std::move(result));
      }
    };

    std::function<void()> cancel;
    try {
      cancel = call(std::move(method), std::move(arguments), std::move(result_sink));
    } catch (...) {
      PostResult(
          request,
          PlatformError{
              "huxerui/call-failed",
              "HuxerUI platform module call failed before producing a result",
              {},
          }
      );
      return request;
    }

    {
      std::lock_guard lock(mutex);
      const auto found = pending.find(request);
      if (found != pending.end()) {
        found->second.cancel = std::move(cancel);
      }
    }
    return request;
  }

  void On(std::string event, std::function<void(const PlatformPayload&)> handler) {
    if (event.empty()) {
      throw std::invalid_argument("HuxerUI platform module event must not be empty");
    }
    static_cast<void>(PlatformPayload(event));
    if (!handler) {
      throw std::invalid_argument("HuxerUI platform module event handler must not be empty");
    }
    std::lock_guard lock(mutex);
    if (!open) {
      throw std::logic_error("HuxerUI platform module instance is closed");
    }
    if (!events.emplace(std::move(event), std::move(handler)).second) {
      throw std::invalid_argument("HuxerUI platform module event was registered more than once");
    }
  }

  bool Cancel(PlatformRequestId request) {
    if (request == 0) {
      return false;
    }
    std::function<void()> cancel;
    {
      std::lock_guard lock(mutex);
      const auto found = pending.find(request);
      if (found == pending.end()) {
        return false;
      }
      cancel = std::move(found->second.cancel);
      pending.erase(found);
    }
    if (cancel) {
      try {
        cancel();
      } catch (...) {
      }
    }
    return true;
  }

  void Close() noexcept {
    std::vector<std::function<void()>> cancellations;
    std::function<void()> dispose;
    {
      std::lock_guard lock(mutex);
      if (!open) {
        return;
      }
      open = false;
      cancellations.reserve(pending.size());
      for (auto& [request, call] : pending) {
        static_cast<void>(request);
        if (call.cancel) {
          cancellations.push_back(std::move(call.cancel));
        }
      }
      pending.clear();
      events.clear();
      queued_events.clear();
      event_delivery_scheduled = false;
      dispose = std::move(native.dispose);
      native.call = {};
    }
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
  }

  void PostResult(PlatformRequestId request, PlatformResult<PlatformPayload> result) {
    UIThreadDispatcher dispatch;
    {
      std::lock_guard lock(mutex);
      if (!open || !pending.contains(request)) {
        return;
      }
      dispatch = dispatch_to_ui_thread;
    }
    const std::weak_ptr<State> weak = weak_from_this();
    try {
      dispatch([weak, request, result = std::move(result)]() mutable {
        if (const std::shared_ptr<State> state = weak.lock()) {
          state->DeliverResult(request, std::move(result));
        }
      });
    } catch (...) {
      std::lock_guard lock(mutex);
      pending.erase(request);
    }
  }

  void PostEvent(std::string event, PlatformPayload payload) {
    if (event.empty() || !IsValidUtf8(event)) {
      return;
    }
    UIThreadDispatcher dispatch;
    {
      std::lock_guard lock(mutex);
      if (!open) {
        return;
      }
      queued_events.emplace_back(std::move(event), std::move(payload));
      if (event_delivery_scheduled) {
        return;
      }
      event_delivery_scheduled = true;
      dispatch = dispatch_to_ui_thread;
    }
    const std::weak_ptr<State> weak = weak_from_this();
    try {
      dispatch([weak] {
        if (const std::shared_ptr<State> state = weak.lock()) {
          state->DrainEvents();
        }
      });
    } catch (...) {
      std::lock_guard lock(mutex);
      queued_events.clear();
      event_delivery_scheduled = false;
    }
  }

  void DeliverResult(PlatformRequestId request, PlatformResult<PlatformPayload> result) {
    std::function<void(PlatformResult<PlatformPayload>)> completion;
    {
      std::lock_guard lock(mutex);
      if (!open) {
        return;
      }
      const auto found = pending.find(request);
      if (found == pending.end()) {
        return;
      }
      completion = std::move(found->second.completion);
      pending.erase(found);
    }
    if (auto* error = std::get_if<PlatformError>(&result)) {
      try {
        if (error->code.empty()) {
          throw std::invalid_argument("empty code");
        }
        static_cast<void>(PlatformPayload(error->code));
        static_cast<void>(PlatformPayload(error->message));
      } catch (...) {
        result = PlatformError{
            "huxerui/invalid-error",
            "HuxerUI platform module returned an invalid error payload",
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
        std::lock_guard lock(mutex);
        if (!open || queued_events.empty()) {
          queued_events.clear();
          event_delivery_scheduled = false;
          return;
        }
        auto [event, next_payload] = std::move(queued_events.front());
        queued_events.pop_front();
        payload = std::move(next_payload);
        const auto found = events.find(event);
        if (found != events.end()) {
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

  UIThreadDispatcher dispatch_to_ui_thread;
  PlatformModuleFactory::Instance native;
  std::mutex mutex;
  PlatformRequestId next_request = 1;
  bool open = true;
  bool event_delivery_scheduled = false;
  std::unordered_map<PlatformRequestId, PendingCall> pending;
  std::unordered_map<std::string, std::function<void(const PlatformPayload&)>> events;
  std::deque<std::pair<std::string, PlatformPayload>> queued_events;
};

PlatformInstance::PlatformInstance(PlatformInstance&& other) noexcept : state_(std::move(other.state_)) {}

PlatformInstance& PlatformInstance::operator=(PlatformInstance&& other) noexcept {
  if (this != &other) {
    Close();
    state_ = std::move(other.state_);
  }
  return *this;
}

PlatformInstance::~PlatformInstance() {
  Close();
}

PlatformRequestId PlatformInstance::CallRaw(
    std::string method, PlatformPayload arguments, std::function<void(PlatformResult<PlatformPayload>)> completion
) {
  if (!state_) {
    throw std::logic_error("HuxerUI platform module instance is closed");
  }
  return state_->Call(std::move(method), std::move(arguments), std::move(completion));
}

void PlatformInstance::OnRaw(std::string event, std::function<void(const PlatformPayload&)> handler) {
  if (!state_) {
    throw std::logic_error("HuxerUI platform module instance is closed");
  }
  state_->On(std::move(event), std::move(handler));
}

bool PlatformInstance::Cancel(PlatformRequestId request) {
  return state_ && state_->Cancel(request);
}

void PlatformInstance::Close() noexcept {
  if (state_) {
    state_->Close();
    state_.reset();
  }
}

PlatformInstance PlatformModules::Open(std::string type, PlatformPayload options) {
  if (type.empty()) {
    throw std::invalid_argument("HuxerUI platform module type must not be empty");
  }
  static_cast<void>(PlatformPayload(type));
  if (!dispatch_to_ui_thread_) {
    throw std::logic_error("HuxerUI UI thread dispatcher is not configured");
  }
  const PlatformModuleFactory* factory = Find<PlatformModuleFactory>(type);
  if (factory == nullptr) {
    throw std::logic_error("HuxerUI platform module type is not registered: " + type);
  }
  if (!factory->create) {
    throw std::logic_error("HuxerUI platform module factory must provide create");
  }

  auto state = std::make_shared<PlatformInstance::State>(dispatch_to_ui_thread_);
  PlatformModuleFactory::Instance native = factory->create(options, state->EventSink());
  if (!native.call) {
    if (native.dispose) {
      try {
        native.dispose();
      } catch (...) {
      }
    }
    throw std::logic_error("HuxerUI platform module factory must provide call");
  }
  state->native = std::move(native);
  return PlatformInstance(std::move(state));
}

} // namespace huxerui
