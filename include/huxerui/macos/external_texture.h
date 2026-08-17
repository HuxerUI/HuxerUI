#pragma once

#include <CoreVideo/CVPixelBuffer.h>

#include <memory>

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class MacExternalTextureState;
} // namespace detail

namespace macos {

class ExternalTextureSource final {
public:
  explicit ExternalTextureSource(Size intrinsic_size);
  ~ExternalTextureSource();

  ExternalTextureSource(const ExternalTextureSource&) = delete;
  ExternalTextureSource& operator=(const ExternalTextureSource&) = delete;
  ExternalTextureSource(ExternalTextureSource&& other) noexcept;
  ExternalTextureSource& operator=(ExternalTextureSource&& other) noexcept;

  [[nodiscard]] ExternalTexture Texture() const noexcept;
  void Publish(CVPixelBufferRef frame);
  void Finish() noexcept;

private:
  std::shared_ptr<detail::MacExternalTextureState> state_;
};

} // namespace macos

} // namespace huxerui
