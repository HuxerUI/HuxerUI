#pragma once

#include <memory>
#include <mutex>
#include <utility>

#include <huxerui/external_texture.h>
#include <huxerui/platform_adapter.h>

namespace huxerui::detail {

class ExternalTextureFrameRequester final : public std::enable_shared_from_this<ExternalTextureFrameRequester> {
public:
  ExternalTextureFrameRequester(PlatformAdapter& adapter, UIThreadDispatcher dispatch_to_ui_thread)
      : adapter_(&adapter), dispatch_to_ui_thread_(std::move(dispatch_to_ui_thread)) {}

  [[nodiscard]] bool CanRequestFrames();
  void RequestFrame();
  void SetActive(const std::shared_ptr<ExternalTexture>& texture, bool active);
  void Close() noexcept;

private:
  PlatformAdapter* adapter_;
  UIThreadDispatcher dispatch_to_ui_thread_;
  std::mutex mutex_;
  bool request_pending_ = false;
  bool closed_ = false;
};

} // namespace huxerui::detail
