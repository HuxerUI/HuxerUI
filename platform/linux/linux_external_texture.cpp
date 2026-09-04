#include "linux_internal.h"

#include <huxerui/linux/external_texture.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "linux_external_texture_internal.h"

namespace huxerui::detail {
namespace {

LinuxPixelFrame CopyFrame(const linux::PixelFrame& frame) {
  if (frame.pixel_width <= 0 || frame.pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions must be positive");
  }
  switch (frame.format) {
  case linux::PixelFormat::Rgba8888:
  case linux::PixelFormat::Bgra8888:
    break;
  default:
    throw std::invalid_argument("HuxerUI Linux external texture pixel format is not supported");
  }

  const std::size_t width = static_cast<std::size_t>(frame.pixel_width);
  const std::size_t height = static_cast<std::size_t>(frame.pixel_height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
  }
  const std::size_t row_bytes = width * 4U;
  if (row_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
  }
  if (frame.bytes_per_row < row_bytes) {
    throw std::invalid_argument("HuxerUI Linux external texture row stride is too small");
  }
  if (height > 1U && frame.bytes_per_row > (std::numeric_limits<std::size_t>::max() - row_bytes) / (height - 1U)) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
  }
  const std::size_t required_bytes = (height - 1U) * frame.bytes_per_row + row_bytes;
  if (frame.pixels.size() < required_bytes) {
    throw std::invalid_argument("HuxerUI Linux external texture pixel buffer is too small");
  }
  if (height > std::numeric_limits<std::size_t>::max() / row_bytes) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions are too large");
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
      const std::uint8_t red = frame.format == linux::PixelFormat::Rgba8888 ? first : third;
      const std::uint8_t blue = frame.format == linux::PixelFormat::Rgba8888 ? third : first;
      const auto premultiply = [alpha](std::uint8_t channel) {
        return static_cast<std::uint8_t>((static_cast<std::uint32_t>(channel) * alpha + 127U) / 255U);
      };
      const std::uint32_t cairo_pixel =
          static_cast<std::uint32_t>(alpha) << 24U | static_cast<std::uint32_t>(premultiply(red)) << 16U |
          static_cast<std::uint32_t>(premultiply(green)) << 8U | static_cast<std::uint32_t>(premultiply(blue));
      std::memcpy(destination_row + x * 4U, &cairo_pixel, sizeof(cairo_pixel));
    }
  }
  return LinuxPixelFrame(frame.pixel_width, frame.pixel_height, std::move(pixels));
}

} // namespace

} // namespace huxerui::detail

namespace huxerui::linux {

struct GdkTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<const detail::LinuxGdkTextureFrame> frame;
  bool finished = false;
};

GdkTexture::GdkTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

GdkTexture::~GdkTexture() {
  Finish();
}

void GdkTexture::Publish(::GdkTexture* frame) {
  if (frame == nullptr) {
    throw std::invalid_argument("HuxerUI Linux external GDK texture frame must not be null");
  }
  auto retained = std::make_shared<const detail::LinuxGdkTextureFrame>(frame);
  std::shared_ptr<const detail::LinuxGdkTextureFrame> retired;
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Linux external GDK texture is finished");
    }
    retired = std::exchange(storage_->frame, std::move(retained));
  }
  NotifyFrameAvailable();
  retired.reset();
}

void GdkTexture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::LinuxGdkTextureFrame> GdkTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

struct PixelTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<const detail::LinuxPixelFrame> frame;
  bool finished = false;
};

PixelTexture::PixelTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

PixelTexture::~PixelTexture() {
  Finish();
}

void PixelTexture::Publish(const PixelFrame& frame) {
  auto copied = std::make_shared<const detail::LinuxPixelFrame>(detail::CopyFrame(frame));
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Linux external texture is finished");
    }
    storage_->frame = std::move(copied);
  }
  NotifyFrameAvailable();
}

void PixelTexture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::LinuxPixelFrame> PixelTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

} // namespace huxerui::linux

namespace huxerui::detail {

std::shared_ptr<const LinuxGdkTextureFrame> GetGdkTextureFrame(const linux::GdkTexture& texture) noexcept {
  return texture.AcquireFrame();
}

std::shared_ptr<const LinuxPixelFrame> GetPixelFrame(const linux::PixelTexture& texture) noexcept {
  return texture.AcquireFrame();
}

} // namespace huxerui::detail
