#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace linux {
class PixelTexture;
} // namespace linux

namespace detail {
class LinuxPixelFrame;
std::shared_ptr<const LinuxPixelFrame> GetPixelFrame(const linux::PixelTexture& texture) noexcept;
} // namespace detail

namespace linux {

/// Identifies the byte order of one straight-alpha, untagged sRGB PixelFrame.
enum class PixelFormat {
  /// Four bytes per pixel in red, green, blue, alpha order.
  Rgba8888,
  /// Four bytes per pixel in blue, green, red, alpha order.
  Bgra8888,
};

/// Describes one borrowed Linux software frame supplied to PixelTexture::Publish().
///
/// Pixel bytes use untagged sRGB color with straight alpha. Publish copies the required rows before returning, so the
/// producer may immediately reuse or release the span and any row padding.
struct PixelFrame {
  /// Width of the image in physical pixels. The value must be greater than zero.
  int pixel_width = 0;
  /// Height of the image in physical pixels. The value must be greater than zero.
  int pixel_height = 0;
  /// Byte distance between adjacent rows, including optional padding. It must be at least pixel_width times four.
  std::size_t bytes_per_row = 0;
  /// Byte order used by every pixel in pixels.
  PixelFormat format = PixelFormat::Rgba8888;
  /// Borrowed storage containing at least (pixel_height - 1) * bytes_per_row + pixel_width * 4 bytes.
  std::span<const std::byte> pixels;
};

/// A latest-frame Linux ExternalTexture backed by copied RGBA or BGRA software pixels.
///
/// The intrinsic size is immutable logical UI geometry and need not equal a frame's pixel dimensions. Publish converts
/// straight-alpha input into renderer-owned premultiplied storage and replaces the mailbox atomically.
///
/// @code
/// auto texture = std::make_shared<linux::PixelTexture>(Size{320.0F, 180.0F});
/// texture->Publish({
///     .pixel_width = 640,
///     .pixel_height = 360,
///     .bytes_per_row = 640 * 4,
///     .format = linux::PixelFormat::Rgba8888,
///     .pixels = pixels,
/// });
/// View preview = Image(texture).Fit(ImageFit::Cover);
/// @endcode
class PixelTexture final : public huxerui::ExternalTexture {
public:
  /// Creates an empty software-frame mailbox with a finite, strictly positive logical size.
  explicit PixelTexture(Size intrinsic_size);
  /// Releases the mailbox copy; renderer-owned snapshots retain any frame resources they still use.
  ~PixelTexture();

  /// PixelTexture identities cannot be copied; share them through std::shared_ptr.
  PixelTexture(const PixelTexture&) = delete;
  PixelTexture& operator=(const PixelTexture&) = delete;

  /// Copies frame into the latest-frame mailbox and schedules every Runtime currently displaying this texture.
  ///
  /// Invalid dimensions, stride, format, or storage size throw std::invalid_argument. Publication after Finish()
  /// throws std::logic_error.
  void Publish(const PixelFrame& frame);
  /// Idempotently stops publication while preserving the last successfully published frame.
  void Finish() noexcept;

private:
  struct Storage;

  [[nodiscard]] std::shared_ptr<const huxerui::detail::LinuxPixelFrame> AcquireFrame() const noexcept;

  std::unique_ptr<Storage> storage_;

  friend std::shared_ptr<const huxerui::detail::LinuxPixelFrame>
  huxerui::detail::GetPixelFrame(const PixelTexture& texture) noexcept;
};

} // namespace linux

} // namespace huxerui
