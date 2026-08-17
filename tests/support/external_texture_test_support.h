#pragma once

#include <memory>

#include "external_texture_internal.h"

namespace huxerui::test {

class ExternalTextureTestState final : public detail::ExternalTextureState {
public:
  explicit ExternalTextureTestState(Size intrinsic_size) : ExternalTextureState(intrinsic_size) {}

  void PublishFrame() {
    NotifyFrameAvailable();
  }
};

inline ExternalTexture MakeTestExternalTexture(Size intrinsic_size) {
  return std::make_shared<ExternalTextureTestState>(intrinsic_size)->Texture();
}

} // namespace huxerui::test
