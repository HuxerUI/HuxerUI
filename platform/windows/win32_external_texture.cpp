#include <huxerui/windows/external_texture.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "win32_external_texture_internal.h"

namespace huxerui::detail {
namespace {

Win32PixelFrame CopyFrame(const windows::PixelFrame& frame) {
  if (frame.pixel_width <= 0 || frame.pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions must be positive");
  }
  switch (frame.format) {
  case windows::PixelFormat::Rgba8888:
  case windows::PixelFormat::Bgra8888:
    break;
  default:
    throw std::invalid_argument("HuxerUI Windows external texture pixel format is not supported");
  }

  const std::size_t width = static_cast<std::size_t>(frame.pixel_width);
  const std::size_t height = static_cast<std::size_t>(frame.pixel_height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }
  const std::size_t row_bytes = width * 4U;
  if (row_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }
  if (frame.bytes_per_row < row_bytes) {
    throw std::invalid_argument("HuxerUI Windows external texture row stride is too small");
  }
  if (height > 1U && frame.bytes_per_row > (std::numeric_limits<std::size_t>::max() - row_bytes) / (height - 1U)) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }
  const std::size_t required_bytes = (height - 1U) * frame.bytes_per_row + row_bytes;
  if (frame.pixels.size() < required_bytes) {
    throw std::invalid_argument("HuxerUI Windows external texture pixel buffer is too small");
  }
  if (height > std::numeric_limits<std::size_t>::max() / row_bytes) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }

  std::vector<std::byte> pixels(row_bytes * height);
  for (std::size_t y = 0; y < height; ++y) {
    const std::byte* source_row = frame.pixels.data() + y * frame.bytes_per_row;
    std::byte* destination_row = pixels.data() + y * row_bytes;
    for (std::size_t x = 0; x < width; ++x) {
      const std::byte* source = source_row + x * 4U;
      const auto first = static_cast<std::uint8_t>(source[0]);
      const auto green = static_cast<std::uint8_t>(source[1]);
      const auto third = static_cast<std::uint8_t>(source[2]);
      const auto alpha = static_cast<std::uint8_t>(source[3]);
      const std::uint8_t red = frame.format == windows::PixelFormat::Rgba8888 ? first : third;
      const std::uint8_t blue = frame.format == windows::PixelFormat::Rgba8888 ? third : first;
      const auto premultiply = [alpha](std::uint8_t channel) {
        return static_cast<std::uint8_t>((static_cast<std::uint32_t>(channel) * alpha + 127U) / 255U);
      };
      std::byte* destination = destination_row + x * 4U;
      destination[0] = static_cast<std::byte>(premultiply(blue));
      destination[1] = static_cast<std::byte>(premultiply(green));
      destination[2] = static_cast<std::byte>(premultiply(red));
      destination[3] = static_cast<std::byte>(alpha);
    }
  }
  return Win32PixelFrame(frame.pixel_width, frame.pixel_height, std::move(pixels));
}

} // namespace

} // namespace huxerui::detail

namespace huxerui::windows {

struct PixelTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<const detail::Win32PixelFrame> frame;
  bool finished = false;
};

PixelTexture::PixelTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

PixelTexture::~PixelTexture() {
  Finish();
}

void PixelTexture::Publish(const PixelFrame& frame) {
  auto copied = std::make_shared<const detail::Win32PixelFrame>(detail::CopyFrame(frame));
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Windows external texture is finished");
    }
    storage_->frame = std::move(copied);
  }
  NotifyFrameAvailable();
}

void PixelTexture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::Win32PixelFrame> PixelTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

} // namespace huxerui::windows

namespace huxerui::detail {

std::shared_ptr<const Win32PixelFrame> GetPixelFrame(const windows::PixelTexture& texture) noexcept {
  return texture.AcquireFrame();
}

} // namespace huxerui::detail
