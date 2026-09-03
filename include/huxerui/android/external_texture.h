#pragma once

#include <cstdint>
#include <memory>

#include <huxerui/android/jni.h>
#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class AndroidBitmapFrame;
class AndroidGpuFrame;
class AndroidRenderer;
} // namespace detail

namespace android {

/// A latest-frame ExternalTexture backed by retained android.graphics.Bitmap objects.
///
/// The intrinsic size is logical UI geometry and is independent of each Bitmap's pixel dimensions. Publish validates
/// the Java object, retains it with a JNI global reference, and replaces the mailbox atomically. Pass the same shared
/// object directly to Image or PaintContext.
///
/// @code
/// auto texture = std::make_shared<android::BitmapTexture>(Size{320.0F, 180.0F});
/// texture->Publish(environment, bitmap);
/// View preview = Image(texture).Fit(ImageFit::Cover);
/// @endcode
class BitmapTexture final : public huxerui::ExternalTexture {
public:
  /// Creates an empty Bitmap mailbox with a finite, strictly positive logical size.
  explicit BitmapTexture(Size intrinsic_size);
  /// Releases mailbox references; renderer-owned snapshots retain any Bitmap resources they still use.
  ~BitmapTexture();

  /// BitmapTexture identities cannot be copied; share them through std::shared_ptr.
  BitmapTexture(const BitmapTexture&) = delete;
  BitmapTexture& operator=(const BitmapTexture&) = delete;

  /// Publishes bitmap as the newest frame and schedules every Runtime currently displaying this texture.
  ///
  /// environment must belong to the calling thread, and bitmap must be a non-recycled android.graphics.Bitmap with
  /// positive pixel dimensions. HuxerUI retains the object; its pixels remain immutable and it must not be recycled
  /// after publication. Use a different Bitmap for a later frame.
  ///
  /// Throws std::invalid_argument for invalid arguments, std::logic_error after Finish(), and std::runtime_error when
  /// the required JNI operations fail.
  void Publish(JNIEnv* environment, jobject bitmap);
  /// Stops publication without discarding the last frame.
  ///
  /// Finish is idempotent. Later Publish calls throw std::logic_error, while existing Image and Paint commands continue
  /// displaying the last successfully published Bitmap.
  void Finish() noexcept;

private:
  struct Storage;

  void InitializeJni(JNIEnv* environment);
  [[nodiscard]] std::shared_ptr<const huxerui::detail::AndroidBitmapFrame> AcquireFrame() const noexcept;

  std::unique_ptr<Storage> storage_;

  friend class huxerui::detail::AndroidRenderer;
};

/// A latest-frame Android ExternalTexture synchronously copied from GL_TEXTURE_2D content.
///
/// PublishCurrent must run with the producer EGL context current. HuxerUI synchronously copies the source into its
/// private texture compositor, so the producer may reuse or delete the source texture after PublishCurrent returns.
class GlTexture final : public huxerui::ExternalTexture {
public:
  /// Identifies how a producer texture maps its vertical coordinate to logical image space.
  enum class Origin {
    /// Texture coordinate zero is the logical top edge.
    TopLeft,
    /// Texture coordinate zero is the logical bottom edge, as in conventional OpenGL content.
    BottomLeft,
  };

  /// Identifies the alpha representation stored in a published texture.
  enum class Alpha {
    /// Every texel is fully opaque and its stored alpha is ignored.
    Opaque,
    /// RGB channels have already been multiplied by alpha.
    Premultiplied,
    /// RGB channels are straight and are multiplied by alpha during import.
    Straight,
  };

  /// Describes one GL_TEXTURE_2D publication captured by PublishCurrent().
  struct Frame {
    /// Texture name in the EGL context current on the calling thread.
    std::uint32_t texture_name = 0;
    /// Width of the level-zero image in physical pixels.
    int pixel_width = 0;
    /// Height of the level-zero image in physical pixels.
    int pixel_height = 0;
    /// Optional borrowed Android native-fence fd that becomes readable when producer writes are complete.
    ///
    /// HuxerUI duplicates a nonnegative descriptor before PublishCurrent returns. The caller retains ownership of the
    /// supplied descriptor. Without a fence, PublishCurrent completes outstanding producer GL work synchronously.
    int acquire_fence_fd = -1;
    /// Vertical coordinate convention used by the producer.
    Origin origin = Origin::BottomLeft;
    /// Alpha representation used by the producer.
    Alpha alpha = Alpha::Premultiplied;
  };

  /// Creates an empty GL-frame mailbox with a finite, strictly positive logical size.
  explicit GlTexture(Size intrinsic_size);
  /// Releases the mailbox; renderer-owned snapshots retain canonical GPU resources they still use.
  ~GlTexture();

  /// GlTexture identities cannot be copied; share them through std::shared_ptr.
  GlTexture(const GlTexture&) = delete;
  GlTexture& operator=(const GlTexture&) = delete;

  /// Copies frame from the EGL context current on the calling thread and publishes an immutable internal snapshot.
  ///
  /// Calls to PublishCurrent for the same GlTexture must not overlap.
  /// Invalid dimensions, texture names, contexts, fences, or unsupported EGL capabilities throw an exception.
  /// Publication after Finish() throws std::logic_error.
  void PublishCurrent(Frame frame);
  /// Idempotently stops publication while preserving the last successfully imported frame.
  void Finish() noexcept;

private:
  struct Storage;

  [[nodiscard]] std::shared_ptr<const huxerui::detail::AndroidGpuFrame> AcquireFrame() const noexcept;

  std::unique_ptr<Storage> storage_;

  friend class huxerui::detail::AndroidRenderer;
};

/// A latest-frame Android ExternalTexture backed by a producer-facing android.view.Surface.
///
/// HuxerUI owns the SurfaceTexture/OES consumer and converts each latched producer buffer into one immutable internal
/// GPU snapshot. Camera, MediaCodec, or another Surface producer may retain the returned Surface for the texture's
/// lifetime. Surface buffers use Android's premultiplied-alpha convention; opaque camera and codec streams are
/// unaffected.
class SurfaceStreamTexture final : public huxerui::ExternalTexture {
public:
  /// Creates a stream and its producer Surface.
  ///
  /// environment must belong to the calling thread. Pixel dimensions must be positive and may differ from the
  /// immutable logical intrinsic size. JNI or EGL initialization failures throw std::runtime_error.
  [[nodiscard]] static std::shared_ptr<SurfaceStreamTexture>
  Create(JNIEnv* environment, Size intrinsic_size, int pixel_width, int pixel_height);
  /// Releases the producer Surface and OES consumer; renderer-owned snapshots retain their canonical frames.
  ~SurfaceStreamTexture();

  /// SurfaceStreamTexture identities cannot be copied; share them through std::shared_ptr.
  SurfaceStreamTexture(const SurfaceStreamTexture&) = delete;
  SurfaceStreamTexture& operator=(const SurfaceStreamTexture&) = delete;

  /// Returns a new local reference to the producer android.view.Surface.
  ///
  /// environment must use the same Java VM supplied to Create. The returned LocalRef owns only this local reference;
  /// the stream retains the underlying Surface independently.
  [[nodiscard]] LocalRef<jobject> Surface(JNIEnv* environment) const;
  /// Changes the physical size requested from later producer buffers without changing logical intrinsic size.
  void SetDefaultBufferSize(JNIEnv* environment, int pixel_width, int pixel_height);
  /// Idempotently closes the producer Surface and OES consumer while preserving the last successfully latched frame.
  /// Surface and SetDefaultBufferSize are unavailable after this call.
  void Finish() noexcept;

private:
  struct Storage;

  SurfaceStreamTexture(Size intrinsic_size, std::unique_ptr<Storage> storage);
  void FrameAvailable();
  [[nodiscard]] std::shared_ptr<const huxerui::detail::AndroidGpuFrame> AcquireFrame() const noexcept;

  std::unique_ptr<Storage> storage_;

  friend class huxerui::detail::AndroidRenderer;
};

} // namespace android

} // namespace huxerui
