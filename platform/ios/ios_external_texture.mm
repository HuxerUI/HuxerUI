#include <huxerui/ios/external_texture.h>

#import <objc/runtime.h>

#include <memory>
#include <stdexcept>
#include <utility>

#include "ios_external_texture_internal.h"

@interface HUXIOSExternalTextureStorage : NSObject {
@public
  huxerui::ExternalTexture texture;
}
@end

@implementation HUXIOSExternalTextureStorage
@end

@interface HUXIOSExternalTextureSourceStorage : NSObject {
@public
  std::unique_ptr<huxerui::ios::ExternalTextureSource> source;
}
@end

@implementation HUXIOSExternalTextureSourceStorage
@end

@interface HUXExternalTexture ()
- (instancetype)initForHuxerUI;
@end

static char external_texture_storage_key;
static char external_texture_source_storage_key;

static HUXIOSExternalTextureSourceStorage* SourceStorage(HUXExternalTextureSource* source) {
  HUXIOSExternalTextureSourceStorage* storage =
      objc_getAssociatedObject(source, &external_texture_source_storage_key);
  if (storage == nil || !storage->source) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                   reason:@"HuxerUI iOS ExternalTextureSource is unavailable"
                                 userInfo:nil];
  }
  return storage;
}

namespace huxerui::detail {

std::shared_ptr<IosExternalTextureState> IosExternalTextureState::Create(Size intrinsic_size) {
  return std::shared_ptr<IosExternalTextureState>(new IosExternalTextureState(intrinsic_size));
}

IosExternalTextureState::~IosExternalTextureState() {
  CVPixelBufferRef pending_frame = nullptr;
  {
    std::lock_guard lock(frame_mutex_);
    pending_frame = std::exchange(pending_frame_, nullptr);
  }
  if (pending_frame != nullptr) {
    CVPixelBufferRelease(pending_frame);
  }
}

void IosExternalTextureState::Publish(CVPixelBufferRef frame) {
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
      replaced = std::exchange(pending_frame_, frame);
    }
  }
  if (rejected) {
    CVPixelBufferRelease(frame);
    throw std::logic_error("HuxerUI iOS external texture source is finished");
  }
  if (replaced != nullptr) {
    CVPixelBufferRelease(replaced);
  }
  NotifyFrameAvailable();
}

void IosExternalTextureState::Finish() noexcept {
  std::lock_guard lock(frame_mutex_);
  finished_ = true;
}

CVPixelBufferRef IosExternalTextureState::AcquireLatestFrame() noexcept {
  std::lock_guard lock(frame_mutex_);
  return std::exchange(pending_frame_, nullptr);
}

} // namespace huxerui::detail

namespace huxerui::ios {

ExternalTextureSource::ExternalTextureSource(Size intrinsic_size)
    : state_(huxerui::detail::IosExternalTextureState::Create(intrinsic_size)) {}

ExternalTextureSource::~ExternalTextureSource() {
  Finish();
}

ExternalTextureSource::ExternalTextureSource(ExternalTextureSource&& other) noexcept
    : state_(std::move(other.state_)) {}

ExternalTextureSource& ExternalTextureSource::operator=(ExternalTextureSource&& other) noexcept {
  if (this != &other) {
    Finish();
    state_ = std::move(other.state_);
  }
  return *this;
}

ExternalTexture ExternalTextureSource::Texture() const noexcept {
  return state_ ? state_->Texture() : ExternalTexture{};
}

void ExternalTextureSource::Publish(CVPixelBufferRef frame) {
  if (!state_) {
    throw std::logic_error("HuxerUI iOS external texture source is empty");
  }
  state_->Publish(frame);
}

void ExternalTextureSource::Finish() noexcept {
  if (state_) {
    state_->Finish();
  }
}

} // namespace huxerui::ios

@implementation HUXExternalTexture

- (instancetype)initForHuxerUI {
  return [super init];
}

@end

namespace huxerui::ios::detail {

HUXExternalTexture* WrapExternalTexture(ExternalTexture texture) {
  if (!texture.HasValue()) {
    throw std::invalid_argument("HuxerUI Apple ExternalTexture must be valid");
  }
  HUXExternalTexture* result = [[HUXExternalTexture alloc] initForHuxerUI];
  HUXIOSExternalTextureStorage* storage = [HUXIOSExternalTextureStorage new];
  storage->texture = std::move(texture);
  objc_setAssociatedObject(result, &external_texture_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return result;
}

ExternalTexture UnwrapExternalTexture(HUXExternalTexture* texture) {
  if (texture == nil) {
    throw std::invalid_argument("HuxerUI Apple ExternalTexture must not be nil");
  }
  HUXIOSExternalTextureStorage* storage = objc_getAssociatedObject(texture, &external_texture_storage_key);
  if (storage == nil || !storage->texture.HasValue()) {
    throw std::invalid_argument("HuxerUI Apple platform boundary received an invalid ExternalTexture value");
  }
  return storage->texture;
}

} // namespace huxerui::ios::detail

@implementation HUXExternalTextureSource

- (instancetype)initWithIntrinsicSize:(CGSize)size {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  try {
    auto storage = [HUXIOSExternalTextureSourceStorage new];
    storage->source = std::make_unique<huxerui::ios::ExternalTextureSource>(
        huxerui::Size{static_cast<float>(size.width), static_cast<float>(size.height)});
    objc_setAssociatedObject(
        self, &external_texture_source_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return self;
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (HUXExternalTexture*)texture {
  try {
    return huxerui::ios::detail::WrapExternalTexture(SourceStorage(self)->source->Texture());
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInternalInconsistencyException reason:reason userInfo:nil];
  }
}

- (void)publishPixelBuffer:(CVPixelBufferRef)pixelBuffer {
  try {
    SourceStorage(self)->source->Publish(pixelBuffer);
  } catch (const std::exception& exception) {
    NSString* reason = [NSString stringWithUTF8String:exception.what()];
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
  }
}

- (void)finish {
  SourceStorage(self)->source->Finish();
}

@end
