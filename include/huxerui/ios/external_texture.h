#pragma once

#if defined(__OBJC__)
#import <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CVPixelBuffer.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

/// The Objective-C and Swift base identity for an opaque HuxerUI external texture capability.
///
/// Instances arrive as a concrete HUXPixelBufferTexture, HUXMetalTexture, or through PlatformPayload. The base class
/// cannot be created directly and exposes no renderer-specific resource handle.
NS_SWIFT_NAME(ExternalTexture)
@interface HUXExternalTexture : NSObject
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

/// A latest-frame iOS texture backed by retained CVPixelBuffer objects.
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

typedef NS_ENUM(NSInteger, HUXMetalTextureOrigin) {
  /// Texture row zero is the logical top edge.
  HUXMetalTextureOriginTopLeft,
  /// Texture row zero is the logical bottom edge.
  HUXMetalTextureOriginBottomLeft,
} NS_SWIFT_NAME(MetalTexture.Origin);

typedef NS_ENUM(NSInteger, HUXMetalTextureAlpha) {
  /// Stored alpha is ignored and every texel is treated as fully opaque.
  HUXMetalTextureAlphaOpaque,
  /// RGB channels have already been multiplied by alpha.
  HUXMetalTextureAlphaPremultiplied,
  /// RGB channels are multiplied by alpha while the snapshot is imported.
  HUXMetalTextureAlphaStraight,
} NS_SWIFT_NAME(MetalTexture.Alpha);

/// A latest-frame iOS texture backed by immutable HuxerUI-owned Metal snapshots.
///
/// The producer must finish writing the source texture before publication. Publication completes a GPU copy before
/// returning, so the source may be reused or released immediately afterward.
NS_SWIFT_NAME(MetalTexture)
__attribute__((objc_subclassing_restricted))
@interface HUXMetalTexture : HUXExternalTexture
/// Creates an empty Metal mailbox with a finite, strictly positive logical size.
- (instancetype)initWithIntrinsicSize:(CGSize)size NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
/// Copies texture level zero as the newest frame and schedules every Runtime currently displaying this texture.
- (void)publishTexture:(id<MTLTexture>)texture origin:(HUXMetalTextureOrigin)origin alpha:(HUXMetalTextureAlpha)alpha
    NS_SWIFT_NAME(publish(_:origin:alpha:));
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
class UIKitRenderer;
} // namespace detail

namespace ios {

/// A latest-frame iOS ExternalTexture backed by retained CVPixelBuffer objects.
///
/// Publish retains the buffer rather than copying its planes. The producer may release its own reference after the
/// call, but the buffer contents remain immutable while HuxerUI may render them.
///
/// @code
/// auto texture = std::make_shared<ios::PixelBufferTexture>(Size{320.0F, 180.0F});
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

  friend class huxerui::detail::UIKitRenderer;
};

/// A latest-frame iOS ExternalTexture backed by immutable HuxerUI-owned Metal snapshots.
///
/// The source must be a non-framebuffer-only 2D BGRA8 or RGBA8 texture whose producer work is complete. Publish()
/// synchronously copies level zero on the source device, so the producer may reuse or release its texture afterward.
/// The native Publish() API is available to Objective-C++ translation units where id<MTLTexture> can be expressed.
class MetalTexture final : public huxerui::ExternalTexture {
public:
  enum class Origin {
    /// Texture row zero is the logical top edge.
    TopLeft,
    /// Texture row zero is the logical bottom edge.
    BottomLeft,
  };

  enum class Alpha {
    /// Stored alpha is ignored and every texel is treated as fully opaque.
    Opaque,
    /// RGB channels have already been multiplied by alpha.
    Premultiplied,
    /// RGB channels are multiplied by alpha while the snapshot is imported.
    Straight,
  };

#if defined(__OBJC__)
  struct Frame {
    /// Borrowed for the synchronous Publish() call; explicit non-ownership keeps ARC and non-ARC callers ABI-safe.
    __unsafe_unretained id<MTLTexture> texture = nil;
    Origin origin = Origin::TopLeft;
    Alpha alpha = Alpha::Premultiplied;
  };
#endif

  /// Creates an empty Metal mailbox with a finite, strictly positive logical size.
  explicit MetalTexture(Size intrinsic_size);
  ~MetalTexture();

  /// MetalTexture identities cannot be copied; share them through std::shared_ptr.
  MetalTexture(const MetalTexture&) = delete;
  MetalTexture& operator=(const MetalTexture&) = delete;

#if defined(__OBJC__)
  /// Copies frame.texture level zero and schedules every Runtime currently displaying this texture.
  ///
  /// Invalid textures throw std::invalid_argument, publication failures throw std::runtime_error, and publication
  /// after Finish() throws std::logic_error. A failed publication preserves the previous frame.
  void Publish(Frame frame);
#endif
  /// Idempotently stops publication while preserving the last successfully published frame.
  void Finish() noexcept;

private:
#if defined(__OBJC__)
  [[nodiscard]] id<MTLTexture> AcquireFrame(Origin& origin) const noexcept;
#endif

  struct Storage;
  std::unique_ptr<Storage> storage_;

  friend class huxerui::detail::UIKitRenderer;
};

} // namespace ios

} // namespace huxerui

#if defined(__OBJC__)
#pragma clang diagnostic pop
#endif
#endif
