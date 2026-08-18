#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <huxerui/linux/external_texture.h>

#include "external_texture_internal.h"

namespace huxerui::detail {

class LinuxExternalTextureFrame final {
public:
  LinuxExternalTextureFrame() noexcept = default;
  LinuxExternalTextureFrame(int pixel_width, int pixel_height, std::vector<std::byte> pixels)
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

class LinuxExternalTextureState final : public ExternalTextureState {
public:
  [[nodiscard]] static std::shared_ptr<LinuxExternalTextureState> Create(Size intrinsic_size);

  void Publish(const linux::ExternalTextureFrame& frame);
  void Finish() noexcept;
  [[nodiscard]] std::optional<LinuxExternalTextureFrame> AcquireLatestFrame() noexcept;

private:
  explicit LinuxExternalTextureState(Size intrinsic_size) : ExternalTextureState(intrinsic_size) {}

  std::mutex frame_mutex_;
  std::optional<LinuxExternalTextureFrame> pending_frame_;
  bool finished_ = false;
};

} // namespace huxerui::detail
