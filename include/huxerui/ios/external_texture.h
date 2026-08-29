#pragma once

#if defined(__OBJC__)
#import <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CVPixelBuffer.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

NS_SWIFT_NAME(ExternalTexture)
__attribute__((objc_subclassing_restricted))
@interface HUXExternalTexture : NSObject
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

NS_SWIFT_NAME(ExternalTextureSource)
@interface HUXExternalTextureSource : NSObject
- (instancetype)initWithIntrinsicSize:(CGSize)size NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
@property(nonatomic, readonly) HUXExternalTexture* texture;
- (void)publishPixelBuffer:(CVPixelBufferRef)pixelBuffer;
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

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class IosExternalTextureState;
} // namespace detail

namespace ios {

class ExternalTextureSource final {
public:
  explicit ExternalTextureSource(Size intrinsic_size);
  ~ExternalTextureSource();

  ExternalTextureSource(const ExternalTextureSource&) = delete;
  ExternalTextureSource& operator=(const ExternalTextureSource&) = delete;
  ExternalTextureSource(ExternalTextureSource&& other) noexcept;
  ExternalTextureSource& operator=(ExternalTextureSource&& other) noexcept;

  [[nodiscard]] ExternalTexture Texture() const noexcept;
  void Publish(CVPixelBufferRef frame);
  void Finish() noexcept;

private:
  std::shared_ptr<detail::IosExternalTextureState> state_;
};

} // namespace ios

} // namespace huxerui

#if defined(__OBJC__)
#pragma clang diagnostic pop
#endif
#endif
