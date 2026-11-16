#include <huxerui/bitmap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include <huxerui/file.h>

#include "bitmap_internal.h"
#include "internal_access.h"

namespace huxerui {

struct Bitmap::Data {
  Bytes pixels;
  std::uint32_t width;
  std::uint32_t height;
  float scale;
  std::uint64_t identity;
};

namespace {

constexpr std::size_t channels = 4;

unsigned Channel(std::byte value) noexcept {
  return std::to_integer<unsigned>(value);
}

void ValidateSampling(ImageSampling sampling) {
  if (sampling != ImageSampling::Nearest && sampling != ImageSampling::Linear) {
    throw std::invalid_argument("HuxerUI image sampling is invalid");
  }
}

void RequireBitmap(const Bitmap& bitmap) {
  if (!bitmap.HasValue()) {
    throw std::logic_error("HuxerUI bitmap has no pixel storage");
  }
}

void ValidateEncodeOptions(const ImageEncodeOptions& options) {
  if (options.format == ImageFormat::Png) return;
  if (options.format != ImageFormat::Jpeg) {
    throw std::invalid_argument("HuxerUI image encoding format is invalid");
  }
  const auto& color = options.jpeg_background;
  const auto valid_channel = [](float channel) { return channel >= 0.0F && channel <= 1.0F; };
  if (options.jpeg_quality < 0 || options.jpeg_quality > 100 || color.alpha != 1.0F ||
      !valid_channel(color.red) || !valid_channel(color.green) || !valid_channel(color.blue)) {
    throw std::invalid_argument("HuxerUI JPEG encoding requires quality from 0 to 100 and an opaque sRGB background");
  }
}

std::byte Quantize(double value) noexcept {
  return static_cast<std::byte>(std::clamp(std::lround(value), 0L, 255L));
}

struct BitmapSampler {
  const std::byte* pixels;
  std::int64_t width;
  std::int64_t height;
  ImageSampling sampling;
  bool extend_edges;

  const std::byte* Pixel(std::int64_t x, std::int64_t y) const {
    if (extend_edges) {
      x = std::clamp(x, std::int64_t{0}, width - 1);
      y = std::clamp(y, std::int64_t{0}, height - 1);
    } else if (x < 0 || y < 0 || x >= width || y >= height) {
      return nullptr;
    }
    return pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)) * channels;
  }

  void Sample(double x, double y, std::byte* destination) const {
    if (extend_edges) {
      x = std::clamp(x, 0.5, static_cast<double>(width) - 0.5);
      y = std::clamp(y, 0.5, static_cast<double>(height) - 0.5);
    } else if (x <= -0.5 || y <= -0.5 || x >= width + 0.5 || y >= height + 0.5) {
      return;
    }
    if (sampling == ImageSampling::Nearest) {
      if (const auto* pixel = Pixel(static_cast<std::int64_t>(std::floor(x)),
                                    static_cast<std::int64_t>(std::floor(y)))) {
        std::copy_n(pixel, channels, destination);
      }
      return;
    }

    const auto x0 = static_cast<std::int64_t>(std::floor(x - 0.5));
    const auto y0 = static_cast<std::int64_t>(std::floor(y - 0.5));
    const double fx = x - 0.5 - x0;
    const double fy = y - 0.5 - y0;
    if (fx == 0.0 && fy == 0.0) {
      if (const auto* pixel = Pixel(x0, y0)) std::copy_n(pixel, channels, destination);
      return;
    }

    // Interpolating straight RGB would mix invisible colors into an antialiased edge.
    double alpha = 0.0;
    std::array<double, 3> color{};
    for (int row = 0; row < 2; ++row) {
      for (int column = 0; column < 2; ++column) {
        const auto* pixel = Pixel(x0 + column, y0 + row);
        if (!pixel) continue;
        const double weight = (column == 0 ? 1.0 - fx : fx) * (row == 0 ? 1.0 - fy : fy);
        const double weighted_alpha = Channel(pixel[3]) * weight;
        alpha += weighted_alpha;
        for (std::size_t channel = 0; channel < color.size(); ++channel) {
          color[channel] += Channel(pixel[channel]) * weighted_alpha;
        }
      }
    }
    destination[3] = Quantize(alpha);
    if (destination[3] != std::byte{0}) {
      for (std::size_t channel = 0; channel < color.size(); ++channel) {
        destination[channel] = Quantize(color[channel] / alpha);
      }
    }
  }
};

struct BitmapTransform {
  double m11;
  double m12;
  double m21;
  double m22;
  double x;
  double y;
};

Bitmap Resample(const Bitmap& bitmap, BitmapTransform inverse, std::uint32_t width, std::uint32_t height,
                ImageSampling sampling, bool extend_edges) {
  Bytes pixels(detail::BitmapByteSize(width, height));
  const BitmapSampler sampler{bitmap.Pixels().data(), bitmap.PixelWidth(), bitmap.PixelHeight(),
                              sampling, extend_edges};
  std::byte* destination = pixels.data();
  for (std::uint32_t y = 0; y < height; ++y) {
    const double row_x = inverse.m21 * (y + 0.5) + inverse.x;
    const double row_y = inverse.m22 * (y + 0.5) + inverse.y;
    for (std::uint32_t x = 0; x < width; ++x) {
      sampler.Sample(inverse.m11 * (x + 0.5) + row_x, inverse.m12 * (x + 0.5) + row_y, destination);
      destination += channels;
    }
  }
  return Bitmap::FromRgba8(width, height, std::move(pixels), bitmap.Scale());
}

Task<bool> SaveBitmap(Bitmap bitmap, File destination, ImageEncodeOptions options) {
  auto bytes = co_await bitmap.EncodeAsync(options);
  co_return co_await destination.WriteBytesAsync(std::move(bytes));
}

} // namespace

Bitmap Bitmap::FromRgba8(std::uint32_t width, std::uint32_t height, Bytes pixels, float scale) {
  if (pixels.size() != detail::BitmapByteSize(width, height)) {
    throw std::invalid_argument("HuxerUI bitmap pixel payload does not match its dimensions");
  }
  if (!std::isfinite(scale) || scale <= 0.0F ||
      !std::isfinite(static_cast<float>(width) / scale) || !std::isfinite(static_cast<float>(height) / scale)) {
    throw std::invalid_argument("HuxerUI bitmap scale and intrinsic dimensions must be finite and positive");
  }
  Bitmap bitmap;
  bitmap.data_ = std::make_shared<const Data>(Data{
      std::move(pixels), width, height, scale, detail::NextImageIdentity(),
  });
  return bitmap;
}

Bitmap Bitmap::CopyPixels(std::uint32_t width, std::uint32_t height, std::span<const std::byte> pixels,
                          std::size_t bytes_per_row, PixelFormat format, float scale) {
  const auto row_bytes = detail::BitmapByteSize(width, height) / height;
  if (bytes_per_row < row_bytes || pixels.size() < row_bytes ||
      (pixels.size() - row_bytes) / bytes_per_row < height - 1) {
    throw std::invalid_argument("HuxerUI bitmap buffer does not cover the requested rows");
  }
  if (format != PixelFormat::Rgba8888 && format != PixelFormat::Bgra8888 && format != PixelFormat::Argb8888) {
    throw std::invalid_argument("HuxerUI bitmap pixel format is invalid");
  }
  return detail::CopyBitmapRows(width, height, pixels.data(), bytes_per_row, format, detail::PixelAlpha::Straight,
                                scale);
}

bool Bitmap::HasValue() const noexcept {
  return static_cast<bool>(data_);
}

std::uint32_t Bitmap::PixelWidth() const noexcept {
  return data_ ? data_->width : 0;
}

std::uint32_t Bitmap::PixelHeight() const noexcept {
  return data_ ? data_->height : 0;
}

float Bitmap::Scale() const noexcept {
  return data_ ? data_->scale : 1.0F;
}

Size Bitmap::IntrinsicSize() const noexcept {
  return {static_cast<float>(PixelWidth()) / Scale(), static_cast<float>(PixelHeight()) / Scale()};
}

std::span<const std::byte> Bitmap::Pixels() const noexcept {
  return data_ ? std::span<const std::byte>{data_->pixels} : std::span<const std::byte>{};
}

Bitmap Bitmap::Crop(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) const {
  RequireBitmap(*this);
  if (width == 0 || height == 0 || x > data_->width || width > data_->width - x ||
      y > data_->height || height > data_->height - y) {
    throw std::invalid_argument("HuxerUI bitmap crop must be nonempty and inside the source");
  }
  if (width == data_->width && height == data_->height) return *this;

  Bytes pixels(detail::BitmapByteSize(width, height));
  const auto source_stride = static_cast<std::size_t>(data_->width) * channels;
  const auto destination_stride = static_cast<std::size_t>(width) * channels;
  const auto* source = data_->pixels.data() + static_cast<std::size_t>(y) * source_stride + x * channels;
  for (std::uint32_t row = 0; row < height; ++row) {
    std::memcpy(pixels.data() + row * destination_stride, source + row * source_stride, destination_stride);
  }
  return FromRgba8(width, height, std::move(pixels), Scale());
}

Bitmap Bitmap::Resize(std::uint32_t width, std::uint32_t height, ImageSampling sampling) const {
  RequireBitmap(*this);
  ValidateSampling(sampling);
  if (width == 0 || height == 0) {
    throw std::invalid_argument("HuxerUI bitmap dimensions must be positive");
  }
  if (width == data_->width && height == data_->height) return *this;

  return Resample(*this, {static_cast<double>(data_->width) / width, 0.0, 0.0,
                            static_cast<double>(data_->height) / height, 0.0, 0.0},
                  width, height, sampling, true);
}

Bitmap Bitmap::Transform(Transform2D transform, std::uint32_t width, std::uint32_t height,
                         ImageSampling sampling) const {
  RequireBitmap(*this);
  ValidateSampling(sampling);
  if (transform.IsIdentity() && width == data_->width && height == data_->height) return *this;
  const auto determinant = static_cast<double>(transform.m11) * transform.m22 -
                           static_cast<double>(transform.m21) * transform.m12;
  if (!std::isfinite(determinant) || determinant == 0.0 ||
      !std::isfinite(transform.translate_x) || !std::isfinite(transform.translate_y)) {
    throw std::invalid_argument("HuxerUI bitmap transform must be finite and invertible");
  }
  BitmapTransform inverse{
      transform.m22 / determinant, -transform.m12 / determinant,
      -transform.m21 / determinant, transform.m11 / determinant, 0.0, 0.0,
  };
  inverse.x = -inverse.m11 * transform.translate_x - inverse.m21 * transform.translate_y;
  inverse.y = -inverse.m12 * transform.translate_x - inverse.m22 * transform.translate_y;
  return Resample(*this, inverse, width, height, sampling, false);
}

Task<Bytes> Bitmap::EncodeAsync(ImageEncodeOptions options) const {
  RequireBitmap(*this);
  ValidateEncodeOptions(options);
  return detail::EncodeBitmap(*this, options);
}

Task<bool> Bitmap::SaveAsync(File destination, ImageEncodeOptions options) const {
  return SaveBitmap(*this, std::move(destination), options);
}

bool Bitmap::operator==(const Bitmap& other) const noexcept {
  return data_ == other.data_;
}

} // namespace huxerui

namespace huxerui::detail {

std::uint64_t NextImageIdentity() noexcept {
  static std::atomic_uint64_t next_identity{1};
  return next_identity.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t InternalAccess::ImageIdentity(const Bitmap& bitmap) noexcept {
  return bitmap.data_ ? bitmap.data_->identity : 0;
}

std::size_t BitmapByteSize(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0 || Bytes{}.max_size() / channels / height < width) {
    throw std::invalid_argument("HuxerUI bitmap dimensions exceed the supported pixel storage");
  }
  return static_cast<std::size_t>(width) * height * channels;
}

static std::byte Unpremultiply(unsigned value, unsigned alpha) {
  return static_cast<std::byte>(alpha == 0 ? 0 : std::min(255U, (value * 255U + alpha / 2U) / alpha));
}

void UnpremultiplyRgba(std::span<std::byte> pixels) {
  for (std::size_t offset = 0; offset < pixels.size(); offset += channels) {
    const unsigned alpha = Channel(pixels[offset + 3]);
    for (std::size_t channel = 0; channel < 3; ++channel) {
      pixels[offset + channel] = Unpremultiply(Channel(pixels[offset + channel]), alpha);
    }
  }
}

Bitmap CopyBitmapRows(std::uint32_t width, std::uint32_t height, const std::byte* pixels,
                      std::size_t stride, Bitmap::PixelFormat format, PixelAlpha alpha_mode, float scale) {
  const auto size = BitmapByteSize(width, height);
  const auto row_bytes = size / height;
  if (!pixels || stride < row_bytes || (height > 1 && (Bytes{}.max_size() - row_bytes) / (height - 1) < stride)) {
    throw std::runtime_error("HuxerUI cannot read the bitmap's platform pixel storage");
  }
  Bytes output(size);
  using PixelFormat = Bitmap::PixelFormat;
  const bool alpha_first = format == PixelFormat::Argb8888;
  const bool blue_first = format == PixelFormat::Bgra8888;
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto* source = pixels + y * stride;
    auto* destination = output.data() + y * row_bytes;
    if (format == PixelFormat::Rgba8888 && alpha_mode == PixelAlpha::Straight) {
      std::memcpy(destination, source, row_bytes);
      continue;
    }
    for (std::uint32_t x = 0; x < width; ++x, source += channels, destination += channels) {
      const unsigned alpha = alpha_mode == PixelAlpha::Opaque ? 255U : Channel(source[alpha_first ? 0 : 3]);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto input = blue_first ? 2 - channel : channel + alpha_first;
        const unsigned value = Channel(source[input]);
        destination[channel] = alpha_mode == PixelAlpha::Premultiplied
            ? Unpremultiply(value, alpha) : static_cast<std::byte>(value);
      }
      destination[3] = static_cast<std::byte>(alpha);
    }
  }
  return Bitmap::FromRgba8(width, height, std::move(output), scale);
}

Bitmap ApplyEncodedOrientation(Bitmap bitmap, std::uint16_t orientation) {
  const auto width = bitmap.PixelWidth();
  const auto height = bitmap.PixelHeight();
  const float x = static_cast<float>(width);
  const float y = static_cast<float>(height);
  constexpr auto nearest = ImageSampling::Nearest;
  switch (orientation) {
  case 2: return bitmap.Transform({-1, 0, 0, 1, x, 0}, width, height, nearest);
  case 3: return bitmap.Transform({-1, 0, 0, -1, x, y}, width, height, nearest);
  case 4: return bitmap.Transform({1, 0, 0, -1, 0, y}, width, height, nearest);
  case 5: return bitmap.Transform({0, 1, 1, 0, 0, 0}, height, width, nearest);
  case 6: return bitmap.Transform({0, 1, -1, 0, y, 0}, height, width, nearest);
  case 7: return bitmap.Transform({0, -1, -1, 0, y, x}, height, width, nearest);
  case 8: return bitmap.Transform({0, -1, 1, 0, 0, x}, height, width, nearest);
  default: return bitmap;
  }
}

Bitmap FlattenBitmap(Bitmap bitmap, Color background) {
  const auto pixels = bitmap.Pixels();
  bool has_transparency = false;
  for (std::size_t alpha = 3; alpha < pixels.size(); alpha += channels) {
    if (pixels[alpha] != std::byte{255}) {
      has_transparency = true;
      break;
    }
  }
  if (!has_transparency) return bitmap;

  Bytes output(pixels.begin(), pixels.end());
  const std::array<double, 3> backdrop{background.red * 255.0, background.green * 255.0, background.blue * 255.0};
  for (std::size_t offset = 0; offset < output.size(); offset += channels) {
    const double alpha = Channel(output[offset + 3]) / 255.0;
    for (std::size_t channel = 0; channel < backdrop.size(); ++channel) {
      const double foreground = Channel(output[offset + channel]) * alpha;
      output[offset + channel] = Quantize(foreground + backdrop[channel] * (1.0 - alpha));
    }
    output[offset + 3] = std::byte{255};
  }
  return Bitmap::FromRgba8(bitmap.PixelWidth(), bitmap.PixelHeight(), std::move(output), bitmap.Scale());
}

} // namespace huxerui::detail
