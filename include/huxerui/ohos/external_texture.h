#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

#include <huxerui/bitmap.h>
#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>
#include <huxerui/task.h>

struct OH_NativeBuffer;
struct OH_PixelmapNative;

namespace huxerui::detail {
class OhosRenderer;
}

namespace huxerui::ohos {

/// Byte order for straight-alpha sRGB software frames.
using PixelFormat = Bitmap::PixelFormat;

/// Borrowed rows that PixelTexture copies before Publish() returns.
struct PixelFrame {
  /// Positive width in physical pixels.
  std::uint32_t pixel_width = 0;
  /// Positive height in physical pixels.
  std::uint32_t pixel_height = 0;
  /// Byte distance between row starts, including padding when present.
  std::size_t bytes_per_row = 0;
  /// Channel order for each pixel.
  PixelFormat format = PixelFormat::Rgba8888;
  /// Top-down rows covering all pixels. Padding after the final row is optional.
  std::span<const std::byte> pixels;
};

/// A live HarmonyOS texture that retains the most recently published software frame.
///
/// A producer may publish from any thread. The drawing backend takes an immutable frame reference on the
/// application thread and draws it in scene order with the Image's clipping, transform, and opacity.
class PixelTexture final : public ExternalTexture {
public:
  /// Creates an empty texture with a finite, strictly positive logical size.
  /// @param intrinsic_size Logical size used to measure Image and interpret its source rectangle.
  explicit PixelTexture(Size intrinsic_size);

  /// Copies a frame, replaces the previous frame, and schedules windows displaying this texture.
  /// @param frame Borrowed straight-alpha sRGB rows that may be reused when Publish() returns.
  /// @throws std::invalid_argument If the dimensions, stride, format, or storage length is invalid.
  /// @throws std::logic_error If publication has finished.
  void Publish(const PixelFrame& frame);

  /// Stops further publication while retaining the last frame for drawing and readback.
  void Finish() noexcept;

  /// Captures the latest frame immediately. The Task retains that scale-1 bitmap independently of later publications.
  /// @return A Task yielding the captured bitmap, or an empty optional before the first publication.
  Task<std::optional<Bitmap>> SnapshotAsync() const override;

private:
  std::optional<Bitmap> AcquireFrame() const;

  mutable std::mutex mutex_;
  std::optional<Bitmap> frame_;
  bool finished_ = false;

  friend class huxerui::detail::OhosRenderer;
};

/// A live HarmonyOS texture backed by immutable, premultiplied sRGB RGBA8 buffers.
///
/// Supported from API 22 when the system provides buffer import. A producer may publish from any thread.
/// The drawing backend consumes a system PixelMap that retains the buffer. HarmonyOS controls how the
/// drawing backend uploads its pixels. SnapshotAsync() returns a separate CPU bitmap.
class NativeBufferTexture final : public ExternalTexture {
public:
  /// Returns whether the system provides the buffer import API.
  static bool IsSupported() noexcept;

  /// Creates an empty texture with a finite, strictly positive logical size.
  /// @param intrinsic_size Logical size used to measure Image and interpret its source rectangle.
  explicit NativeBufferTexture(Size intrinsic_size);

  /// Imports a completed buffer and schedules windows displaying this texture.
  /// @param buffer A borrowed RGBA8 buffer with `NATIVEBUFFER_USAGE_CPU_READ` and `NATIVEBUFFER_USAGE_CPU_WRITE`.
  /// Complete producer writes before publishing. The producer may release its reference after this call
  /// and must never write to the published buffer again.
  /// @return True if the frame was published. Returns false if import is unavailable, the buffer lacks the required
  /// usage flags, or the system cannot retain or import the buffer. The previous frame remains available on failure.
  /// @throws std::invalid_argument If the buffer is null or has invalid dimensions or format.
  /// @throws std::logic_error If publication has finished when the imported frame is ready to replace the previous one.
  bool Publish(OH_NativeBuffer* buffer);

  /// Stops further publication while retaining the last frame for drawing and readback.
  void Finish() noexcept;

  /// Captures the latest frame immediately. Reading its pixels runs on a worker when the Task is started.
  /// @return A Task yielding a scale-1 bitmap, or an empty optional before the first publication.
  Task<std::optional<Bitmap>> SnapshotAsync() const override;

private:
  struct Frame {
    std::shared_ptr<OH_NativeBuffer> buffer;
    std::shared_ptr<OH_PixelmapNative> pixelmap;
    Size pixel_size;
  };

  std::shared_ptr<const Frame> AcquireFrame() const;

  mutable std::mutex mutex_;
  std::shared_ptr<const Frame> frame_;
  bool finished_ = false;

  friend class huxerui::detail::OhosRenderer;
};

} // namespace huxerui::ohos
