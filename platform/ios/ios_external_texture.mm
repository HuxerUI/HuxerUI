#include <huxerui/ios/external_texture.h>

#import <CoreImage/CoreImage.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <objc/runtime.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "ios_external_texture_internal.h"

@interface HUXIOSExternalTextureStorage : NSObject {
@public
  std::shared_ptr<huxerui::ExternalTexture> texture;
}
@end

@implementation HUXIOSExternalTextureStorage
@end

@interface HUXExternalTexture ()
- (instancetype)initForHuxerUI;
@end

static char external_texture_storage_key;

static HUXIOSExternalTextureStorage* TextureStorage(HUXExternalTexture* texture) {
  HUXIOSExternalTextureStorage* storage = objc_getAssociatedObject(texture, &external_texture_storage_key);
  if (storage == nil || !storage->texture) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                   reason:@"HuxerUI iOS ExternalTexture is unavailable"
                                 userInfo:nil];
  }
  return storage;
}

namespace huxerui::ios {

namespace {

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
    throw std::runtime_error("HuxerUI iOS Metal texture copy failed");
  }
  throw std::runtime_error(std::string("HuxerUI iOS Metal texture copy failed: ") + description);
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
    throw std::invalid_argument("HuxerUI iOS external texture frame must not be null");
  }
  CVPixelBufferRetain(frame);
  CVPixelBufferRef replaced = nullptr;
  bool rejected = false;
  {
    std::lock_guard lock(frame_mutex_);
    if (finished_) {
      rejected = true;
    } else {
      replaced = std::exchange(frame_, frame);
    }
  }
  if (rejected) {
    CVPixelBufferRelease(frame);
    throw std::logic_error("HuxerUI iOS external texture is finished");
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

MetalTexture::MetalTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

MetalTexture::~MetalTexture() = default;

void MetalTexture::Publish(Frame frame) {
  if (frame.texture == nil) {
    throw std::invalid_argument("HuxerUI iOS Metal texture must not be nil");
  }
  if (frame.texture.textureType != MTLTextureType2D || frame.texture.sampleCount != 1 || frame.texture.width == 0 ||
      frame.texture.height == 0 || frame.texture.depth != 1 || frame.texture.framebufferOnly ||
      frame.texture.storageMode == MTLStorageModeMemoryless) {
    throw std::invalid_argument("HuxerUI iOS Metal texture must be a copyable non-framebuffer-only 2D texture");
  }
  if (!IsSupportedMetalPixelFormat(frame.texture.pixelFormat)) {
    throw std::invalid_argument("HuxerUI iOS Metal texture pixel format is unsupported");
  }
  if (!IsValidMetalOrigin(frame.origin) || !IsValidMetalAlpha(frame.alpha)) {
    throw std::invalid_argument("HuxerUI iOS Metal texture metadata is invalid");
  }

  @autoreleasepool {
    std::lock_guard publish_lock(storage_->publish_mutex);
    {
      std::lock_guard frame_lock(storage_->frame_mutex);
      if (storage_->finished) {
        throw std::logic_error("HuxerUI iOS Metal texture is finished");
      }
    }
    id<MTLDevice> device = frame.texture.device;
    if (device == nil) {
      throw std::invalid_argument("HuxerUI iOS Metal texture has no device");
    }
    id<MTLCommandQueue> command_queue = storage_->device == device ? storage_->command_queue : nil;
    if (command_queue == nil) {
      command_queue = [device newCommandQueue];
    }
    if (command_queue == nil) {
      throw std::runtime_error("HuxerUI iOS Metal texture command queue creation failed");
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
      throw std::runtime_error("HuxerUI iOS Metal texture snapshot allocation failed");
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
        throw std::runtime_error("HuxerUI iOS Metal texture conversion source allocation failed");
      }
      copy_target = conversion_source;
    }
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
    if (command_buffer == nil || blit_encoder == nil) {
      throw std::runtime_error("HuxerUI iOS Metal texture copy encoding failed");
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
        throw std::runtime_error("HuxerUI iOS Metal texture alpha conversion creation failed");
      }
      [conversion encodeToCommandBuffer:command_buffer sourceTexture:conversion_source destinationTexture:snapshot];
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      ThrowMetalCommandError(command_buffer);
    }
    if ([CIImage imageWithMTLTexture:snapshot options:nil] == nil) {
      throw std::runtime_error("HuxerUI iOS Metal texture cannot be imported by Core Image");
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

} // namespace huxerui::ios

@implementation HUXExternalTexture

- (instancetype)initForHuxerUI {
  return [super init];
}

@end

namespace huxerui::ios::detail {

HUXExternalTexture* WrapExternalTexture(std::shared_ptr<ExternalTexture> texture) {
  if (!texture) {
    throw std::invalid_argument("HuxerUI Apple ExternalTexture must be valid");
  }
  HUXExternalTexture* result = [[HUXExternalTexture alloc] initForHuxerUI];
  HUXIOSExternalTextureStorage* storage = [HUXIOSExternalTextureStorage new];
  storage->texture = std::move(texture);
  objc_setAssociatedObject(result, &external_texture_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return result;
}

std::shared_ptr<ExternalTexture> UnwrapExternalTexture(HUXExternalTexture* texture) {
  if (texture == nil) {
    throw std::invalid_argument("HuxerUI Apple ExternalTexture must not be nil");
  }
  return TextureStorage(texture)->texture;
}

} // namespace huxerui::ios::detail

@implementation HUXPixelBufferTexture

- (instancetype)initWithIntrinsicSize:(CGSize)size {
  self = [super initForHuxerUI];
  if (self == nil) {
    return nil;
  }
  try {
    auto storage = [HUXIOSExternalTextureStorage new];
    storage->texture = std::make_shared<huxerui::ios::PixelBufferTexture>(
        huxerui::Size{static_cast<float>(size.width), static_cast<float>(size.height)});
    objc_setAssociatedObject(self, &external_texture_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return self;
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)publishPixelBuffer:(CVPixelBufferRef)pixelBuffer {
  try {
    std::static_pointer_cast<huxerui::ios::PixelBufferTexture>(TextureStorage(self)->texture)->Publish(pixelBuffer);
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)finish {
  std::static_pointer_cast<huxerui::ios::PixelBufferTexture>(TextureStorage(self)->texture)->Finish();
}

@end

@implementation HUXMetalTexture

- (instancetype)initWithIntrinsicSize:(CGSize)size {
  self = [super initForHuxerUI];
  if (self == nil) {
    return nil;
  }
  try {
    auto storage = [HUXIOSExternalTextureStorage new];
    storage->texture = std::make_shared<huxerui::ios::MetalTexture>(
        huxerui::Size{static_cast<float>(size.width), static_cast<float>(size.height)});
    objc_setAssociatedObject(self, &external_texture_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return self;
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)publishTexture:(id<MTLTexture>)texture origin:(HUXMetalTextureOrigin)origin alpha:(HUXMetalTextureAlpha)alpha {
  try {
    auto metal_texture = std::static_pointer_cast<huxerui::ios::MetalTexture>(TextureStorage(self)->texture);
    metal_texture->Publish({texture, static_cast<huxerui::ios::MetalTexture::Origin>(origin),
                            static_cast<huxerui::ios::MetalTexture::Alpha>(alpha)});
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)finish {
  std::static_pointer_cast<huxerui::ios::MetalTexture>(TextureStorage(self)->texture)->Finish();
}

@end
