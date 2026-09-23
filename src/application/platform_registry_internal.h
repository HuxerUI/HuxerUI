#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/platform_registry.h>

namespace huxerui::detail {

class PlatformChannelState;
struct ExecutionContext;

[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;

struct PlatformChannelTransport {
  std::function<std::function<void()>(std::string, PlatformPayload,
                                      std::function<void(PlatformResult<PlatformPayload>)>)>
      invoke;
  std::function<void()> dispose;
};

class PlatformChannelEndpoint final {
public:
  PlatformChannelEndpoint() = default;

  [[nodiscard]] PlatformChannel Channel() const;
  [[nodiscard]] PlatformEventEmitter Events() const;
  void Connect(PlatformChannelTransport transport) const;
  void Close() const noexcept;

private:
  explicit PlatformChannelEndpoint(std::shared_ptr<PlatformChannelState> state) : state_(std::move(state)) {}

  std::shared_ptr<PlatformChannelState> state_;

  friend PlatformChannelEndpoint MakePlatformChannelEndpoint(UiThreadDispatcher, std::shared_ptr<ExecutionContext>);
};

/// Creates shared channel state for a Runtime or UiWindow endpoint.
/// @param dispatch_to_ui_thread Queued dispatcher for the original application's thread.
/// @param source Original application or window execution context; never rebound to a replacement host.
/// @return An unconnected endpoint retaining its dispatch context until close or destruction.
PlatformChannelEndpoint MakePlatformChannelEndpoint(UiThreadDispatcher dispatch_to_ui_thread,
                                                    std::shared_ptr<ExecutionContext> source);

} // namespace huxerui::detail
