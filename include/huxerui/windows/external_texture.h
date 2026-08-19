#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class Win32ExternalTextureState;
} // namespace detail

namespace windows {

enum class ExternalTexturePixelFormat {
  Rgba8888,
  Bgra8888,
};

// Pixel bytes are untagged sRGB with straight alpha. Publish copies the required rows before returning, so the span
// may be reused.
struct ExternalTextureFrame {
  int pixel_width = 0;
  int pixel_height = 0;
  std::size_t bytes_per_row = 0;
  ExternalTexturePixelFormat format = ExternalTexturePixelFormat::Rgba8888;
  std::span<const std::byte> pixels;
};

class ExternalTextureSource final {
public:
  explicit ExternalTextureSource(Size intrinsic_size);
  ~ExternalTextureSource();

  ExternalTextureSource(const ExternalTextureSource&) = delete;
  ExternalTextureSource& operator=(const ExternalTextureSource&) = delete;
  ExternalTextureSource(ExternalTextureSource&& other) noexcept;
  ExternalTextureSource& operator=(ExternalTextureSource&& other) noexcept;

  [[nodiscard]] ExternalTexture Texture() const noexcept;
  void Publish(const ExternalTextureFrame& frame);
  void Finish() noexcept;

private:
  std::shared_ptr<detail::Win32ExternalTextureState> state_;
};

} // namespace windows

} // namespace huxerui
