#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <huxerui/color.h>
#include <huxerui/data.h>
#include <huxerui/geometry.h>
#include <huxerui/task.h>

namespace huxerui {

class File;

namespace detail {
struct InternalAccess;
}

/// Encoded raster formats supported by image assets and bitmap export.
enum class ImageFormat {
  Png,  ///< Portable Network Graphics payload.
  Jpeg, ///< JPEG payload.
};

/// Sampling policy used when scaling an image.
enum class ImageSampling {
  Nearest, ///< Select nearby source pixels without interpolation.
  Linear,  ///< Interpolate nearby source pixels.
};

/// Selects an encoded file format and its export settings.
struct ImageEncodeOptions {
  /// Encoded format written to the output.
  ImageFormat format = ImageFormat::Png;
  /// JPEG encoder quality in the inclusive range 0 to 100.
  int jpeg_quality = 90;
  /// Opaque sRGB background composited under the bitmap for JPEG output.
  Color jpeg_background = Color::White();
};

/// Immutable sRGB RGBA8 pixels with straight alpha and a logical density.
/// Copies share storage and compare by storage identity. A default bitmap has no pixels.
class Bitmap {
public:
  /// Byte order of a straight-alpha sRGB pixel in an input buffer.
  enum class PixelFormat {
    Rgba8888, ///< Red, green, blue, alpha.
    Bgra8888, ///< Blue, green, red, alpha.
    Argb8888, ///< Alpha, red, green, blue.
  };

  /// Constructs a missing bitmap with no pixel storage.
  Bitmap() = default;

  /// Takes ownership of tightly packed RGBA8 pixels in rows ordered from the top edge.
  /// @param width Positive width in pixels.
  /// @param height Positive height in pixels.
  /// @param pixels Exactly width times height times four bytes, in RGBA channel order.
  /// @param scale Finite, positive pixels per logical unit; the resulting logical dimensions must be finite.
  /// @throws std::invalid_argument If the dimensions, scale, or payload length are invalid.
  static Bitmap FromRgba8(std::uint32_t width, std::uint32_t height, Bytes pixels, float scale = 1.0F);

  /// Copies top-down rows into tightly packed RGBA8 storage.
  /// The format identifies memory bytes in order. A packed 0xAARRGGBB integer occupies BGRA bytes on a
  /// little-endian machine. The input channels must use straight alpha and sRGB.
  /// @param width Positive width in pixels.
  /// @param height Positive height in pixels.
  /// @param pixels Source bytes covering every pixel; padding after the final row is optional.
  /// @param bytes_per_row Distance between consecutive row starts, at least width times four.
  /// @param format Channel order of the source bytes.
  /// @param scale Finite, positive pixels per logical unit; the resulting logical dimensions must be finite.
  /// @throws std::invalid_argument If the dimensions, buffer, stride, format, or scale are invalid.
  static Bitmap CopyPixels(std::uint32_t width, std::uint32_t height, std::span<const std::byte> pixels,
                           std::size_t bytes_per_row, PixelFormat format, float scale = 1.0F);

  /// Reports whether this value has pixel storage.
  bool HasValue() const noexcept;

  /// Returns the physical width, or zero for a missing bitmap.
  std::uint32_t PixelWidth() const noexcept;
  /// Returns the physical height, or zero for a missing bitmap.
  std::uint32_t PixelHeight() const noexcept;
  /// Returns pixels per logical unit, or 1 for a missing bitmap.
  float Scale() const noexcept;
  /// Returns pixel dimensions divided by Scale(), for use in layout.
  Size IntrinsicSize() const noexcept;
  /// Borrows top-down, tightly packed RGBA8 bytes while any copy retains the storage; empty for a missing bitmap.
  std::span<const std::byte> Pixels() const noexcept;

  /// Copies a pixel rectangle starting at (x, y) from the top-left corner of the source.
  /// A full-image crop shares storage with this bitmap. The scale is preserved.
  /// @throws std::invalid_argument If the rectangle is empty or lies outside the source.
  /// @throws std::logic_error If the bitmap has no value.
  Bitmap Crop(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) const;

  /// Resamples to the given pixel dimensions, extending source edge pixels during interpolation.
  /// An unchanged pixel size shares storage with this bitmap. The scale is preserved.
  /// @throws std::invalid_argument If the output dimensions or sampling are invalid.
  /// @throws std::logic_error If the bitmap has no value.
  Bitmap Resize(std::uint32_t width, std::uint32_t height, ImageSampling sampling = ImageSampling::Linear) const;

  /// Maps the source into an output canvas of the given pixel dimensions, starting at (0, 0).
  /// Both origins are at the top left. Source samples outside the pixel grid are transparent.
  /// The transform must be finite and invertible.
  /// An identity transform at the original pixel size shares storage with this bitmap. The scale is preserved.
  /// @throws std::invalid_argument If the transform, output size, or sampling is invalid.
  /// @throws std::logic_error If the bitmap has no value.
  Bitmap Transform(Transform2D source_to_destination, std::uint32_t width, std::uint32_t height,
                   ImageSampling sampling = ImageSampling::Linear) const;

  /// Encodes this bitmap to owned file bytes. The lazy task retains the pixels.
  /// @throws std::invalid_argument At the call site if the format or applicable options are invalid.
  /// @throws std::logic_error At the call site if the bitmap has no value.
  /// @throws std::runtime_error When awaited if the encoder fails.
  Task<Bytes> EncodeAsync(ImageEncodeOptions options = {}) const;

  /// Encodes this bitmap and writes the result using File::WriteBytesAsync().
  /// The lazy task owns its inputs. Encoding errors surface when awaited and leave the destination untouched.
  /// The selected format determines the contents, independently of the destination's extension.
  /// @return A task yielding true if written, or false if the file write fails.
  Task<bool> SaveAsync(File destination, ImageEncodeOptions options) const;

  /// Compares shared storage identity. Two missing bitmaps compare equal.
  bool operator==(const Bitmap& other) const noexcept;

private:
  struct Data;
  std::shared_ptr<const Data> data_;

  friend struct detail::InternalAccess;
};

} // namespace huxerui
