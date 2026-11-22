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

/// Retains the completed RGBA8 pixels published by GlTexture or NativeBufferTexture.
/// Pixels are premultiplied, and texture coordinate v = 0 corresponds to the logical top edge.
class GlFrame final {
public:
  GlFrame(const GlFrame&) = delete;
  GlFrame& operator=(const GlFrame&) = delete;

  /// Returns the width in physical pixels.
  int PixelWidth() const noexcept { return pixel_width_; }
  /// Returns the height in physical pixels.
  int PixelHeight() const noexcept { return pixel_height_; }

  /// Imports the frame as a new GL_TEXTURE_2D in the calling thread's current OpenGL ES 3 context.
  /// The context must support importing HarmonyOS buffers as EGLImages. The returned texture shares the frame's
  /// read-only pixels and has independent sampling parameters. The active texture unit's GL_TEXTURE_2D binding
  /// is restored. Retain the frame through GPU sampling and delete the returned texture in a compatible context.
  /// @return A caller-owned texture name for the imported frame.
  /// @throws std::logic_error If no EGL context is current.
  /// @throws std::runtime_error If import is unavailable or fails, including buffers without GPU texture usage.
  std::uint32_t ImportCurrent() const;

private:
  GlFrame(std::shared_ptr<OH_NativeBuffer> buffer, std::shared_ptr<OH_PixelmapNative> pixelmap,
          int pixel_width, int pixel_height);

  std::shared_ptr<OH_NativeBuffer> buffer_;
  std::shared_ptr<OH_PixelmapNative> pixelmap_;
  int pixel_width_;
  int pixel_height_;

  friend class NativeBufferTexture;
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

  /// Retains the latest completed frame, or returns an empty pointer before the first publication.
  /// Acquisition may run concurrently with publication. The frame survives later publications and texture destruction.
  std::shared_ptr<const GlFrame> AcquireFrame() const noexcept;

  /// Captures the latest frame immediately. Reading its pixels runs on a worker when the Task is started.
  /// @return A Task yielding a scale-1 bitmap, or an empty optional before the first publication.
  Task<std::optional<Bitmap>> SnapshotAsync() const override;

private:
  class Storage {
  public:
    bool Publish(OH_NativeBuffer* buffer);
    void RequireOpen() const;
    void Finish() noexcept;
    std::shared_ptr<const GlFrame> AcquireFrame() const noexcept;
    Task<std::optional<Bitmap>> SnapshotAsync() const;

  private:
    mutable std::mutex mutex_;
    std::shared_ptr<const GlFrame> frame_;
    bool finished_ = false;
  };

  Storage storage_;

  friend class GlTexture;
};

/// A live HarmonyOS texture that publishes immutable copies of existing OpenGL ES textures.
///
/// Publication uses the producer's current EGL context and an independent RGBA8 buffer for each frame.
/// The application thread draws completed frames through Native Drawing.
class GlTexture final : public ExternalTexture {
public:
  /// Describes how the source texture's vertical coordinate maps to the logical image.
  enum class Origin {
    /// Texture coordinate zero is the logical top edge.
    TopLeft,
    /// Texture coordinate zero is the logical bottom edge, as in conventional OpenGL content.
    BottomLeft,
  };

  /// Borrows level zero of a premultiplied sRGB RGBA8 GL_TEXTURE_2D for one publication.
  struct Frame {
    /// Texture name in the calling thread's current EGL context.
    std::uint32_t texture_name = 0;
    /// Width of level zero in physical pixels.
    int pixel_width = 0;
    /// Height of level zero in physical pixels.
    int pixel_height = 0;
    /// Orientation of the source pixels.
    Origin origin = Origin::BottomLeft;
  };

  /// Returns whether the current OpenGL ES 3 context and system expose the required buffer import APIs.
  /// A particular frame can still fail allocation or import.
  static bool IsSupportedCurrent() noexcept;

  /// Creates an empty texture with a finite, strictly positive logical size.
  /// @param intrinsic_size Logical size used to measure Image and interpret its source rectangle.
  explicit GlTexture(Size intrinsic_size);

  /// Copies and publishes a completed frame. Waits for the GPU copy on the calling thread.
  /// @param frame Source texture. Synchronize writes made in other contexts before calling.
  /// Calls on the same GlTexture must be serialized. On success the caller may immediately reuse or delete the source.
  /// @return True if the frame was published. Unavailable capabilities and graphics failures leave the previous frame.
  /// @throws std::invalid_argument If the texture name, dimensions, origin, or framebuffer attachment is invalid.
  /// @throws std::logic_error If publication has finished or no EGL context is current.
  bool PublishCurrent(Frame frame);

  /// Stops further publication while retaining the last frame for drawing and readback.
  void Finish() noexcept;

  /// Retains the latest completed frame, or returns an empty pointer before the first publication.
  /// Acquisition may run concurrently with publication. The frame survives later publications and texture destruction.
  std::shared_ptr<const GlFrame> AcquireFrame() const noexcept;

  /// Captures the latest frame immediately. Reading its pixels runs on a worker when the Task is started.
  /// @return A Task yielding a scale-1 bitmap, or an empty optional before the first publication.
  Task<std::optional<Bitmap>> SnapshotAsync() const override;

private:
  NativeBufferTexture::Storage storage_;
};

} // namespace huxerui::ohos
