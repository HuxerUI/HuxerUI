#pragma once

#if defined(__OBJC__)
#import <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CVPixelBuffer.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// The Objective-C and Swift base identity for an opaque HuxerUI external texture capability.
///
/// Instances arrive as a concrete HUXPixelBufferTexture or through PlatformPayload. The base class cannot be created
/// directly and exposes no renderer-specific resource handle.
NS_SWIFT_NAME(ExternalTexture)
@interface HUXExternalTexture : NSObject
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

/// A latest-frame macOS texture backed by retained CVPixelBuffer objects.
///
/// Swift imports this class as PixelBufferTexture. The intrinsic size is immutable logical UI geometry rather than the
/// pixel dimensions of a published buffer.
///
/// @code
/// let texture = PixelBufferTexture(intrinsicSize: CGSize(width: 320, height: 180))
/// texture.publishPixelBuffer(pixelBuffer)
/// @endcode
NS_SWIFT_NAME(PixelBufferTexture)
__attribute__((objc_subclassing_restricted))
@interface HUXPixelBufferTexture : HUXExternalTexture
/// Creates an empty pixel-buffer mailbox with a finite, strictly positive logical size.
- (instancetype)initWithIntrinsicSize:(CGSize)size NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
/// Retains pixelBuffer as the newest immutable frame and schedules every Runtime currently displaying this texture.
///
/// Pass a different buffer for later mutable content. A null buffer or publication after finish raises an exception.
- (void)publishPixelBuffer:(CVPixelBufferRef)pixelBuffer;
/// Idempotently stops publication while preserving the last successfully published frame.
- (void)finish;
@end

NS_ASSUME_NONNULL_END

#else
#include <CoreVideo/CVPixelBuffer.h>
#endif

#if defined(__cplusplus)
#if defined(__OBJC__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#endif
#include <memory>
#include <mutex>

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class AppKitRenderer;
} // namespace detail

namespace macos {

/// A latest-frame macOS ExternalTexture backed by retained CVPixelBuffer objects.
///
/// Publish retains the buffer rather than copying its planes. The producer may release its own reference after the
/// call, but the buffer contents remain immutable while HuxerUI may render them.
///
/// @code
/// auto texture = std::make_shared<macos::PixelBufferTexture>(Size{320.0F, 180.0F});
/// texture->Publish(pixel_buffer);
/// View preview = Image(texture).Fit(ImageFit::Cover);
/// @endcode
class PixelBufferTexture final : public huxerui::ExternalTexture {
public:
  /// Creates an empty pixel-buffer mailbox with a finite, strictly positive logical size.
  explicit PixelBufferTexture(Size intrinsic_size) : huxerui::ExternalTexture(intrinsic_size) {}
  /// Releases the retained mailbox buffer; renderer-owned snapshots retain their own resources.
  ~PixelBufferTexture();

  /// PixelBufferTexture identities cannot be copied; share them through std::shared_ptr.
  PixelBufferTexture(const PixelBufferTexture&) = delete;
  PixelBufferTexture& operator=(const PixelBufferTexture&) = delete;

  /// Retains frame as the newest immutable image and schedules every Runtime currently displaying this texture.
  ///
  /// A null frame throws std::invalid_argument. Publication after Finish() throws std::logic_error.
  void Publish(CVPixelBufferRef frame);
  /// Idempotently stops publication while preserving the last successfully published frame.
  void Finish() noexcept;

private:
  [[nodiscard]] CVPixelBufferRef AcquireFrame() const noexcept;

  mutable std::mutex frame_mutex_;
  CVPixelBufferRef frame_ = nullptr;
  bool finished_ = false;

  friend class huxerui::detail::AppKitRenderer;
};

} // namespace macos

} // namespace huxerui

#if defined(__OBJC__)
#pragma clang diagnostic pop
#endif
#endif
