#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include <huxerui/external_texture.h>
#include <huxerui/platform_module.h>

namespace huxerui {

class PlatformAdapter;
class PlatformPayload;

namespace detail {

class ExternalTextureSurface final : public std::enable_shared_from_this<ExternalTextureSurface> {
public:
  ExternalTextureSurface(PlatformAdapter& adapter, UIThreadDispatcher dispatch_to_ui_thread)
      : adapter_(&adapter), dispatch_to_ui_thread_(std::move(dispatch_to_ui_thread)) {}

  [[nodiscard]] bool CanRequestFrames();
  void RequestFrame();
  void Close() noexcept;

private:
  PlatformAdapter* adapter_;
  UIThreadDispatcher dispatch_to_ui_thread_;
  std::mutex mutex_;
  bool request_pending_ = false;
  bool closed_ = false;
};

class ExternalTextureState : public std::enable_shared_from_this<ExternalTextureState> {
public:
  virtual ~ExternalTextureState() = default;

  [[nodiscard]] ExternalTexture Texture();
  [[nodiscard]] Size IntrinsicSize() const noexcept {
    return intrinsic_size_;
  }
  [[nodiscard]] std::uint64_t Revision() const noexcept {
    return revision_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool IsActive() const noexcept;

  void Bind(const std::shared_ptr<ExternalTextureSurface>& surface);
  void SetActive(const std::shared_ptr<ExternalTextureSurface>& surface, bool active);

  [[nodiscard]] static const std::shared_ptr<ExternalTextureState>& From(const ExternalTexture& texture) noexcept {
    return texture.state_;
  }

protected:
  explicit ExternalTextureState(Size intrinsic_size);
  void NotifyFrameAvailable();

private:
  Size intrinsic_size_;
  std::atomic<std::uint64_t> revision_ = 0;
  mutable std::mutex mutex_;
  std::weak_ptr<ExternalTextureSurface> surface_;
  bool bound_ = false;
  bool active_ = false;
};

void BindExternalTextures(const PlatformPayload& payload, const std::shared_ptr<ExternalTextureSurface>& surface);

} // namespace detail

} // namespace huxerui
