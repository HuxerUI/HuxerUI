#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

struct ID3D11Texture2D;

namespace huxerui {

namespace windows {
class D3D11Texture;
class PixelTexture;
} // namespace windows

namespace detail {
class Win32D3D11Frame;
class Win32PixelFrame;
std::shared_ptr<const Win32D3D11Frame> GetD3D11Frame(const windows::D3D11Texture& texture) noexcept;
std::shared_ptr<const Win32PixelFrame> GetPixelFrame(const windows::PixelTexture& texture) noexcept;
} // namespace detail

namespace windows {

/// Identifies the byte order of one straight-alpha, untagged sRGB PixelFrame.
enum class PixelFormat {
  /// Four bytes per pixel in red, green, blue, alpha order.
  Rgba8888,
  /// Four bytes per pixel in blue, green, red, alpha order.
  Bgra8888,
};

/// Describes one borrowed Windows software frame supplied to PixelTexture::Publish().
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

/// A latest-frame Windows ExternalTexture backed by copied RGBA or BGRA software pixels.
///
/// The intrinsic size is immutable logical UI geometry and need not equal a frame's pixel dimensions. Publish converts
/// straight-alpha input into renderer-owned premultiplied storage and replaces the mailbox atomically.
///
/// @code
/// auto texture = std::make_shared<windows::PixelTexture>(Size{320.0F, 180.0F});
/// texture->Publish({
///     .pixel_width = 640,
///     .pixel_height = 360,
///     .bytes_per_row = 640 * 4,
///     .format = windows::PixelFormat::Rgba8888,
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

  [[nodiscard]] std::shared_ptr<const huxerui::detail::Win32PixelFrame> AcquireFrame() const noexcept;

  std::unique_ptr<Storage> storage_;

  friend std::shared_ptr<const huxerui::detail::Win32PixelFrame>
  huxerui::detail::GetPixelFrame(const PixelTexture& texture) noexcept;
};

/// A latest-frame Windows ExternalTexture backed by immutable shared D3D11 snapshots.
///
/// Each successful Publish copies the borrowed source into a new HuxerUI-owned shared texture before returning. The
/// producer may therefore reuse or release the source immediately. The source device and its immediate context must be
/// externally serialized, and all writes to the source must have been submitted before publication.
///
/// The source must be a single-sampled, one-mip, one-slice D3D11_USAGE_DEFAULT texture using
/// DXGI_FORMAT_B8G8R8A8_UNORM with no CPU access. Rendering requires the producer and HuxerUI renderer devices to use
/// the same adapter. Pixel row zero is the logical top edge. This texture type is unavailable in
/// HUXERUI_WINDOWS_7_COMPAT builds.
class D3D11Texture final : public huxerui::ExternalTexture {
public:
  /// Describes how the renderer interprets the source alpha channel.
  enum class Alpha {
    /// The source alpha channel is ignored and every pixel is treated as opaque.
    Opaque,
    /// Color channels have already been multiplied by alpha.
    Premultiplied,
  };

  /// Describes one borrowed D3D11 source supplied to Publish().
  struct Frame {
    /// Borrowed source texture copied synchronously during Publish().
    ID3D11Texture2D* texture = nullptr;
    /// Alpha interpretation retained with the immutable snapshot.
    Alpha alpha = Alpha::Premultiplied;
  };

  /// Creates an empty GPU-frame mailbox with a finite, strictly positive logical size.
  explicit D3D11Texture(Size intrinsic_size);
  /// Stops publication and releases the mailbox; renderer-owned snapshots retain resources they still use.
  ~D3D11Texture();

  /// D3D11Texture identities cannot be copied; share them through std::shared_ptr.
  D3D11Texture(const D3D11Texture&) = delete;
  D3D11Texture& operator=(const D3D11Texture&) = delete;

  /// Copies frame into a new immutable shared snapshot and schedules every Runtime displaying this texture.
  ///
  /// Invalid source properties throw std::invalid_argument. Unsupported devices and GPU failures throw
  /// std::runtime_error. Publication after Finish() throws std::logic_error.
  void Publish(Frame frame);
  /// Idempotently stops publication while preserving the last successfully published frame.
  void Finish() noexcept;

private:
  struct Storage;

  [[nodiscard]] std::shared_ptr<const huxerui::detail::Win32D3D11Frame> AcquireFrame() const noexcept;

  std::unique_ptr<Storage> storage_;

  friend std::shared_ptr<const huxerui::detail::Win32D3D11Frame>
  huxerui::detail::GetD3D11Frame(const D3D11Texture& texture) noexcept;
};

} // namespace windows

} // namespace huxerui
