#pragma once

#include <memory>

#include <huxerui/external_texture.h>

namespace huxerui::test {

class ExternalTextureTestTexture final : public ExternalTexture {
public:
  explicit ExternalTextureTestTexture(Size intrinsic_size) : ExternalTexture(intrinsic_size) {}

  void PublishFrame() {
    NotifyFrameAvailable();
  }
};

inline std::shared_ptr<ExternalTexture> MakeTestExternalTexture(Size intrinsic_size) {
  return std::make_shared<ExternalTextureTestTexture>(intrinsic_size);
}

} // namespace huxerui::test
