#include <huxerui/macos/external_texture.h>

#import <objc/runtime.h>

#include <memory>
#include <stdexcept>
#include <utility>

#include "macos_external_texture_internal.h"

@interface HUXMacExternalTextureStorage : NSObject {
@public
  std::shared_ptr<huxerui::ExternalTexture> texture;
}
@end

@implementation HUXMacExternalTextureStorage
@end

@interface HUXExternalTexture ()
- (instancetype)initForHuxerUI;
@end

static char external_texture_storage_key;

static HUXMacExternalTextureStorage* TextureStorage(HUXExternalTexture* texture) {
  HUXMacExternalTextureStorage* storage = objc_getAssociatedObject(texture, &external_texture_storage_key);
  if (storage == nil || !storage->texture) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                   reason:@"HuxerUI macOS ExternalTexture is unavailable"
                                 userInfo:nil];
  }
  return storage;
}

namespace huxerui::macos {

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

} // namespace huxerui::macos

@implementation HUXExternalTexture

- (instancetype)initForHuxerUI {
  return [super init];
}

@end

namespace huxerui::macos::detail {

HUXExternalTexture* WrapExternalTexture(std::shared_ptr<ExternalTexture> texture) {
  if (!texture) {
    throw std::invalid_argument("HuxerUI macOS ExternalTexture must be valid");
  }
  HUXExternalTexture* result = [[HUXExternalTexture alloc] initForHuxerUI];
  HUXMacExternalTextureStorage* storage = [HUXMacExternalTextureStorage new];
  storage->texture = std::move(texture);
  objc_setAssociatedObject(result, &external_texture_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return result;
}

std::shared_ptr<ExternalTexture> UnwrapExternalTexture(HUXExternalTexture* texture) {
  if (texture == nil) {
    throw std::invalid_argument("HuxerUI macOS ExternalTexture must not be nil");
  }
  return TextureStorage(texture)->texture;
}

} // namespace huxerui::macos::detail

@implementation HUXPixelBufferTexture

- (instancetype)initWithIntrinsicSize:(CGSize)size {
  self = [super initForHuxerUI];
  if (self == nil) {
    return nil;
  }
  try {
    auto storage = [HUXMacExternalTextureStorage new];
    storage->texture = std::make_shared<huxerui::macos::PixelBufferTexture>(
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
    std::static_pointer_cast<huxerui::macos::PixelBufferTexture>(TextureStorage(self)->texture)->Publish(pixelBuffer);
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)finish {
  std::static_pointer_cast<huxerui::macos::PixelBufferTexture>(TextureStorage(self)->texture)->Finish();
}

@end
