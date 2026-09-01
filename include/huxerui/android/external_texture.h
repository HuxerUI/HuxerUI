#pragma once

#include <memory>

#include <huxerui/android/jni.h>
#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class AndroidBitmapFrame;
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

} // namespace android

} // namespace huxerui
