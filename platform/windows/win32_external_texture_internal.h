#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <huxerui/windows/external_texture.h>

namespace huxerui::detail {

class Win32PixelFrame final {
public:
  Win32PixelFrame() noexcept = default;
  Win32PixelFrame(int pixel_width, int pixel_height, std::vector<std::byte> pixels)
      : pixel_width_(pixel_width), pixel_height_(pixel_height), pixels_(std::move(pixels)) {}

  [[nodiscard]] int PixelWidth() const noexcept {
    return pixel_width_;
  }

  [[nodiscard]] int PixelHeight() const noexcept {
    return pixel_height_;
  }

  [[nodiscard]] std::size_t BytesPerRow() const noexcept {
    return static_cast<std::size_t>(pixel_width_) * 4U;
  }

  [[nodiscard]] std::span<const std::byte> Pixels() const noexcept {
    return pixels_;
  }

private:
  int pixel_width_ = 0;
  int pixel_height_ = 0;
  std::vector<std::byte> pixels_;
};

} // namespace huxerui::detail
