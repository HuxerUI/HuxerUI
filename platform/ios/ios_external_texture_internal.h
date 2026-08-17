#pragma once

#include <CoreVideo/CVPixelBuffer.h>

#include <memory>
#include <mutex>

#include "external_texture_internal.h"

namespace huxerui::detail {

class IosExternalTextureState final : public ExternalTextureState {
public:
  [[nodiscard]] static std::shared_ptr<IosExternalTextureState> Create(Size intrinsic_size);
  ~IosExternalTextureState() override;

  void Publish(CVPixelBufferRef frame);
  void Finish() noexcept;
  [[nodiscard]] CVPixelBufferRef AcquireLatestFrame() noexcept;

private:
  explicit IosExternalTextureState(Size intrinsic_size) : ExternalTextureState(intrinsic_size) {}

  std::mutex frame_mutex_;
  CVPixelBufferRef pending_frame_ = nullptr;
  bool finished_ = false;
};

} // namespace huxerui::detail
