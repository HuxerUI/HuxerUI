#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <huxerui/windows/external_texture.h>

#include "external_texture_internal.h"

namespace huxerui::detail {

class Win32ExternalTextureFrame final {
public:
  Win32ExternalTextureFrame() noexcept = default;
  Win32ExternalTextureFrame(int pixel_width, int pixel_height, std::vector<std::byte> pixels)
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

class Win32ExternalTextureState final : public ExternalTextureState {
public:
  [[nodiscard]] static std::shared_ptr<Win32ExternalTextureState> Create(Size intrinsic_size);

  void Publish(const windows::ExternalTextureFrame& frame);
  void Finish() noexcept;
  [[nodiscard]] std::optional<Win32ExternalTextureFrame> AcquireLatestFrame() noexcept;

private:
  explicit Win32ExternalTextureState(Size intrinsic_size) : ExternalTextureState(intrinsic_size) {}

  std::mutex frame_mutex_;
  std::optional<Win32ExternalTextureFrame> pending_frame_;
  bool finished_ = false;
};

} // namespace huxerui::detail
