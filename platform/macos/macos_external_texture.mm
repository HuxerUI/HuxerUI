#include <huxerui/macos/external_texture.h>

#import <CoreImage/CoreImage.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "macos_external_texture_internal.h"
#include "appkit_renderer.h"

// The base wrapper owns the shared C++ texture so concrete subclasses and payload-created wrappers have one lifetime.
@interface HUXExternalTexture () {
@private
  std::shared_ptr<huxerui::ExternalTexture> texture_;
}
- (instancetype)initForHuxerUIWithTexture:(std::shared_ptr<huxerui::ExternalTexture>)texture;
- (std::shared_ptr<huxerui::ExternalTexture>)textureForHuxerUI;
@end

namespace huxerui::macos {

namespace {

Bitmap CopyFramePixels(CIImage* image, CIContext* context) {
  if (image == nil || context == nil || CGRectIsEmpty(image.extent) || CGRectIsInfinite(image.extent)) {
    throw std::runtime_error("HuxerUI could not read the texture frame");
  }
  auto cg_image = std::unique_ptr<std::remove_pointer_t<CGImageRef>, decltype(&CGImageRelease)>(
      [context createCGImage:image fromRect:image.extent], CGImageRelease);
  return huxerui::detail::CopyCGImagePixels(cg_image.get());
}

bool IsSupportedMetalPixelFormat(MTLPixelFormat format) noexcept {
  return format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB ||
         format == MTLPixelFormatRGBA8Unorm || format == MTLPixelFormatRGBA8Unorm_sRGB;
}

bool IsValidMetalOrigin(MetalTexture::Origin origin) noexcept {
  return origin == MetalTexture::Origin::TopLeft || origin == MetalTexture::Origin::BottomLeft;
}

bool IsValidMetalAlpha(MetalTexture::Alpha alpha) noexcept {
  return alpha == MetalTexture::Alpha::Opaque || alpha == MetalTexture::Alpha::Premultiplied ||
         alpha == MetalTexture::Alpha::Straight;
}

[[noreturn]] void ThrowMetalCommandError(id<MTLCommandBuffer> command_buffer) {
  const char* description = command_buffer.error.localizedDescription.UTF8String;
  if (description == nullptr) {
    throw std::runtime_error("HuxerUI macOS Metal texture copy failed");
  }
  throw std::runtime_error(std::string("HuxerUI macOS Metal texture copy failed: ") + description);
}

} // namespace

struct MetalTexture::Storage {
  std::mutex publish_mutex;
  mutable std::mutex frame_mutex;
  __strong id<MTLTexture> frame = nil;
  __strong id<MTLTexture> conversion_source = nil;
  __strong id<MTLDevice> device = nil;
  __strong id<MTLCommandQueue> command_queue = nil;
  Origin origin = Origin::TopLeft;
  bool finished = false;
};

PixelBufferTexture::~PixelBufferTexture() {
  CVPixelBufferRef frame = nullptr;
  {
    std::lock_guard lock(frame_mutex_);
    frame = std::exchange(frame_, nullptr);
  }
  if (frame != nullptr) {
    CVPixelBufferRelease(frame);
  }
}

void PixelBufferTexture::Publish(CVPixelBufferRef frame) {
  if (frame == nullptr) {
    throw std::invalid_argument("HuxerUI macOS external texture frame must not be null");
  }
  CVPixelBufferRetain(frame);
  CVPixelBufferRef replaced = nullptr;
  {
    std::lock_guard lock(frame_mutex_);
    if (finished_) {
      CVPixelBufferRelease(frame);
      throw std::logic_error("HuxerUI macOS external texture is finished");
    }
    replaced = std::exchange(frame_, frame);
  }
  if (replaced != nullptr) {
    CVPixelBufferRelease(replaced);
  }
  NotifyFrameAvailable();
}

void PixelBufferTexture::Finish() noexcept {
  std::lock_guard lock(frame_mutex_);
  finished_ = true;
}

CVPixelBufferRef PixelBufferTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(frame_mutex_);
  if (frame_ != nullptr) {
    CVPixelBufferRetain(frame_);
  }
  return frame_;
}

Task<std::optional<Bitmap>> PixelBufferTexture::SnapshotAsync() const {
  std::shared_ptr<std::remove_pointer_t<CVPixelBufferRef>> frame(AcquireFrame(), CVPixelBufferRelease);
  return RunWorker([](const auto& retained) -> std::optional<Bitmap> {
    if (!retained) return std::nullopt;
    @autoreleasepool {
      return CopyFramePixels([CIImage imageWithCVPixelBuffer:retained.get()], [CIContext contextWithOptions:nil]);
    }
  }, std::move(frame));
}

MetalTexture::MetalTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

MetalTexture::~MetalTexture() = default;

void MetalTexture::Publish(Frame frame) {
  if (frame.texture == nil) {
    throw std::invalid_argument("HuxerUI macOS Metal texture must not be nil");
  }
  if (frame.texture.textureType != MTLTextureType2D || frame.texture.sampleCount != 1 || frame.texture.width == 0 ||
      frame.texture.height == 0 || frame.texture.depth != 1 || frame.texture.framebufferOnly ||
      frame.texture.storageMode == MTLStorageModeMemoryless) {
    throw std::invalid_argument("HuxerUI macOS Metal texture must be a copyable non-framebuffer-only 2D texture");
  }
  if (!IsSupportedMetalPixelFormat(frame.texture.pixelFormat)) {
    throw std::invalid_argument("HuxerUI macOS Metal texture pixel format is unsupported");
  }
  if (!IsValidMetalOrigin(frame.origin) || !IsValidMetalAlpha(frame.alpha)) {
    throw std::invalid_argument("HuxerUI macOS Metal texture metadata is invalid");
  }

  @autoreleasepool {
    std::lock_guard publish_lock(storage_->publish_mutex);
    {
      std::lock_guard frame_lock(storage_->frame_mutex);
      if (storage_->finished) {
        throw std::logic_error("HuxerUI macOS Metal texture is finished");
      }
    }
    id<MTLDevice> device = frame.texture.device;
    if (device == nil) {
      throw std::invalid_argument("HuxerUI macOS Metal texture has no device");
    }
    id<MTLCommandQueue> command_queue = storage_->device == device ? storage_->command_queue : nil;
    if (command_queue == nil) {
      command_queue = [device newCommandQueue];
    }
    if (command_queue == nil) {
      throw std::runtime_error("HuxerUI macOS Metal texture command queue creation failed");
    }

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:frame.texture.pixelFormat
                                                           width:frame.texture.width height:frame.texture.height
                                                        mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    const bool convert_alpha = frame.alpha != Alpha::Premultiplied;
    descriptor.usage = MTLTextureUsageShaderRead | (convert_alpha ? MTLTextureUsageShaderWrite : 0);
    id<MTLTexture> snapshot = [device newTextureWithDescriptor:descriptor];
    if (snapshot == nil) {
      throw std::runtime_error("HuxerUI macOS Metal texture snapshot allocation failed");
    }
    id<MTLTexture> conversion_source = nil;
    id<MTLTexture> copy_target = snapshot;
    if (convert_alpha) {
      conversion_source = storage_->conversion_source;
      if (conversion_source.device != device || conversion_source.pixelFormat != frame.texture.pixelFormat ||
          conversion_source.width != frame.texture.width || conversion_source.height != frame.texture.height) {
        descriptor.usage = MTLTextureUsageShaderRead;
        conversion_source = [device newTextureWithDescriptor:descriptor];
      }
      if (conversion_source == nil) {
        throw std::runtime_error("HuxerUI macOS Metal texture conversion source allocation failed");
      }
      copy_target = conversion_source;
    }
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
    if (command_buffer == nil || blit_encoder == nil) {
      throw std::runtime_error("HuxerUI macOS Metal texture copy encoding failed");
    }
    [blit_encoder copyFromTexture:frame.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(frame.texture.width, frame.texture.height, 1) toTexture:copy_target
                 destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit_encoder endEncoding];
    if (convert_alpha) {
      const MPSAlphaType source_alpha =
          frame.alpha == Alpha::Opaque ? MPSAlphaTypeAlphaIsOne : MPSAlphaTypeNonPremultiplied;
      MPSImageConversion* conversion =
          [[MPSImageConversion alloc] initWithDevice:device srcAlpha:source_alpha
                                           destAlpha:MPSAlphaTypePremultiplied backgroundColor:nil conversionInfo:nil];
      if (conversion == nil) {
        throw std::runtime_error("HuxerUI macOS Metal texture alpha conversion creation failed");
      }
      [conversion encodeToCommandBuffer:command_buffer sourceTexture:conversion_source destinationTexture:snapshot];
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      ThrowMetalCommandError(command_buffer);
    }
    if ([CIImage imageWithMTLTexture:snapshot options:nil] == nil) {
      throw std::runtime_error("HuxerUI macOS Metal texture cannot be imported by Core Image");
    }

    {
      std::lock_guard frame_lock(storage_->frame_mutex);
      storage_->frame = snapshot;
      storage_->origin = frame.origin;
    }
    if (convert_alpha) {
      storage_->conversion_source = conversion_source;
    }
    storage_->device = device;
    storage_->command_queue = command_queue;
  }
  NotifyFrameAvailable();
}

void MetalTexture::Finish() noexcept {
  std::lock_guard publish_lock(storage_->publish_mutex);
  std::lock_guard frame_lock(storage_->frame_mutex);
  storage_->finished = true;
}

id<MTLTexture> MetalTexture::AcquireFrame(Origin& origin) const noexcept {
  std::lock_guard frame_lock(storage_->frame_mutex);
  origin = storage_->origin;
  return storage_->frame;
}

Task<std::optional<Bitmap>> MetalTexture::SnapshotAsync() const {
  Origin origin = Origin::TopLeft;
  id<MTLTexture> frame = AcquireFrame(origin);
  return RunWorker([frame, origin]() -> std::optional<Bitmap> {
    if (frame == nil) return std::nullopt;
    @autoreleasepool {
      CIImage* image = [CIImage imageWithMTLTexture:frame options:nil];
      if (origin == Origin::BottomLeft && image != nil) {
        image = [image imageByApplyingTransform:CGAffineTransformMake(1, 0, 0, -1, 0, image.extent.size.height)];
      }
      return CopyFramePixels(image, [CIContext contextWithMTLDevice:frame.device options:nil]);
    }
  });
}

} // namespace huxerui::macos

@implementation HUXExternalTexture

- (instancetype)initForHuxerUIWithTexture:(std::shared_ptr<huxerui::ExternalTexture>)texture {
  self = [super init];
  if (self != nil) {
    texture_ = std::move(texture);
  }
  return self;
}

- (std::shared_ptr<huxerui::ExternalTexture>)textureForHuxerUI {
  if (!texture_) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                   reason:@"HuxerUI macOS ExternalTexture is unavailable"
                                 userInfo:nil];
  }
  return texture_;
}

@end

namespace huxerui::macos::detail {

HUXExternalTexture* WrapExternalTexture(std::shared_ptr<ExternalTexture> texture) {
  if (!texture) {
    throw std::invalid_argument("HuxerUI macOS ExternalTexture must be valid");
  }
  return [[HUXExternalTexture alloc] initForHuxerUIWithTexture:std::move(texture)];
}

std::shared_ptr<ExternalTexture> UnwrapExternalTexture(HUXExternalTexture* texture) {
  if (texture == nil) {
    throw std::invalid_argument("HuxerUI macOS ExternalTexture must not be nil");
  }
  return [texture textureForHuxerUI];
}

} // namespace huxerui::macos::detail

@implementation HUXPixelBufferTexture

- (instancetype)initWithIntrinsicSize:(CGSize)size {
  try {
    auto texture = std::make_shared<huxerui::macos::PixelBufferTexture>(
        huxerui::Size{static_cast<float>(size.width), static_cast<float>(size.height)});
    return [super initForHuxerUIWithTexture:std::move(texture)];
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)publishPixelBuffer:(CVPixelBufferRef)pixelBuffer {
  try {
    std::static_pointer_cast<huxerui::macos::PixelBufferTexture>([self textureForHuxerUI])->Publish(pixelBuffer);
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)finish {
  std::static_pointer_cast<huxerui::macos::PixelBufferTexture>([self textureForHuxerUI])->Finish();
}

@end

@implementation HUXMetalTexture

- (instancetype)initWithIntrinsicSize:(CGSize)size {
  try {
    auto texture = std::make_shared<huxerui::macos::MetalTexture>(
        huxerui::Size{static_cast<float>(size.width), static_cast<float>(size.height)});
    return [super initForHuxerUIWithTexture:std::move(texture)];
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)publishTexture:(id<MTLTexture>)texture origin:(HUXMetalTextureOrigin)origin alpha:(HUXMetalTextureAlpha)alpha {
  try {
    auto metal_texture = std::static_pointer_cast<huxerui::macos::MetalTexture>([self textureForHuxerUI]);
    metal_texture->Publish({texture, static_cast<huxerui::macos::MetalTexture::Origin>(origin),
                            static_cast<huxerui::macos::MetalTexture::Alpha>(alpha)});
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)finish {
  std::static_pointer_cast<huxerui::macos::MetalTexture>([self textureForHuxerUI])->Finish();
}

@end
