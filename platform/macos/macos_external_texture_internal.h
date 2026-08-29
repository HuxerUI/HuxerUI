#pragma once

#include <CoreVideo/CVPixelBuffer.h>

#include <memory>
#include <mutex>

#include "external_texture_internal.h"

@class HUXExternalTexture;

namespace huxerui::detail {

class MacExternalTextureState final : public ExternalTextureState {
public:
  [[nodiscard]] static std::shared_ptr<MacExternalTextureState> Create(Size intrinsic_size);
  ~MacExternalTextureState() override;

  void Publish(CVPixelBufferRef frame);
  void Finish() noexcept;
  [[nodiscard]] CVPixelBufferRef AcquireLatestFrame() noexcept;

private:
  explicit MacExternalTextureState(Size intrinsic_size) : ExternalTextureState(intrinsic_size) {}

  std::mutex frame_mutex_;
  CVPixelBufferRef pending_frame_ = nullptr;
  bool finished_ = false;
};

} // namespace huxerui::detail

namespace huxerui::macos::detail {

HUXExternalTexture* WrapExternalTexture(ExternalTexture texture);
ExternalTexture UnwrapExternalTexture(HUXExternalTexture* texture);

} // namespace huxerui::macos::detail
