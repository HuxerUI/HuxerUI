#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>

#include <huxerui/platform_adapter.h>
#include <huxerui/platform_registry.h>

namespace huxerui::detail {

class ExternalTextureSurface;
class PlatformChannelState;

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

  friend PlatformChannelEndpoint MakePlatformChannelEndpoint(UIThreadDispatcher,
                                                             std::shared_ptr<ExternalTextureSurface>);
};

PlatformChannelEndpoint MakePlatformChannelEndpoint(UIThreadDispatcher dispatch_to_ui_thread,
                                                    std::shared_ptr<ExternalTextureSurface> texture_surface);

} // namespace huxerui::detail
