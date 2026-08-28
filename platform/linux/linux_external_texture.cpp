#include "linux_internal.h"

#include <huxerui/linux/external_texture.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "linux_external_texture_internal.h"

namespace huxerui::detail {
namespace {

LinuxExternalTextureFrame CopyFrame(const linux::ExternalTextureFrame& frame) {
  if (frame.pixel_width <= 0 || frame.pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Linux external texture frame dimensions must be positive");
  }
  switch (frame.format) {
  case linux::ExternalTexturePixelFormat::Rgba8888:
  case linux::ExternalTexturePixelFormat::Bgra8888:
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
      const std::uint8_t red = frame.format == linux::ExternalTexturePixelFormat::Rgba8888 ? first : third;
      const std::uint8_t blue = frame.format == linux::ExternalTexturePixelFormat::Rgba8888 ? third : first;
      const auto premultiply = [alpha](std::uint8_t channel) {
        return static_cast<std::uint8_t>((static_cast<std::uint32_t>(channel) * alpha + 127U) / 255U);
      };
      const std::uint32_t premultiplied_pixel =
          static_cast<std::uint32_t>(alpha) << 24U | static_cast<std::uint32_t>(premultiply(red)) << 16U |
          static_cast<std::uint32_t>(premultiply(green)) << 8U | static_cast<std::uint32_t>(premultiply(blue));
      std::memcpy(destination_row + x * 4U, &premultiplied_pixel, sizeof(premultiplied_pixel));
    }
  }
  return LinuxExternalTextureFrame(frame.pixel_width, frame.pixel_height, std::move(pixels));
}

} // namespace

std::shared_ptr<LinuxExternalTextureState> LinuxExternalTextureState::Create(Size intrinsic_size) {
  return std::shared_ptr<LinuxExternalTextureState>(new LinuxExternalTextureState(intrinsic_size));
}

void LinuxExternalTextureState::Publish(const linux::ExternalTextureFrame& frame) {
  LinuxExternalTextureFrame copied = CopyFrame(frame);
  {
    std::lock_guard lock(frame_mutex_);
    if (finished_) {
      throw std::logic_error("HuxerUI Linux external texture source is finished");
    }
    pending_frame_ = std::move(copied);
  }
  NotifyFrameAvailable();
}

void LinuxExternalTextureState::Finish() noexcept {
  std::lock_guard lock(frame_mutex_);
  finished_ = true;
}

std::optional<LinuxExternalTextureFrame> LinuxExternalTextureState::AcquireLatestFrame() noexcept {
  std::lock_guard lock(frame_mutex_);
  return std::exchange(pending_frame_, std::nullopt);
}

} // namespace huxerui::detail

namespace huxerui::linux {

ExternalTextureSource::ExternalTextureSource(Size intrinsic_size)
    : state_(detail::LinuxExternalTextureState::Create(intrinsic_size)) {}

ExternalTextureSource::~ExternalTextureSource() {
  Finish();
}

ExternalTextureSource::ExternalTextureSource(ExternalTextureSource&& other) noexcept
    : state_(std::move(other.state_)) {}

ExternalTextureSource& ExternalTextureSource::operator=(ExternalTextureSource&& other) noexcept {
  if (this != &other) {
    Finish();
    state_ = std::move(other.state_);
  }
  return *this;
}

ExternalTexture ExternalTextureSource::Texture() const noexcept {
  return state_ ? state_->Texture() : ExternalTexture{};
}

void ExternalTextureSource::Publish(const ExternalTextureFrame& frame) {
  if (!state_) {
    throw std::logic_error("HuxerUI Linux external texture source is empty");
  }
  state_->Publish(frame);
}

void ExternalTextureSource::Finish() noexcept {
  if (state_) {
    state_->Finish();
  }
}

} // namespace huxerui::linux
