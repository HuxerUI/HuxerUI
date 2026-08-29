#include <huxerui/macos/external_texture.h>

#import <objc/runtime.h>

#include <memory>
#include <stdexcept>
#include <utility>

#include "macos_external_texture_internal.h"

@interface HUXMacExternalTextureStorage : NSObject {
@public
  huxerui::ExternalTexture texture;
}
@end

@implementation HUXMacExternalTextureStorage
@end

@interface HUXMacExternalTextureSourceStorage : NSObject {
@public
  std::unique_ptr<huxerui::macos::ExternalTextureSource> source;
}
@end

@implementation HUXMacExternalTextureSourceStorage
@end

@interface HUXExternalTexture ()
- (instancetype)initForHuxerUI;
@end

static char external_texture_storage_key;
static char external_texture_source_storage_key;

static HUXMacExternalTextureSourceStorage* SourceStorage(HUXExternalTextureSource* source) {
  HUXMacExternalTextureSourceStorage* storage =
      objc_getAssociatedObject(source, &external_texture_source_storage_key);
  if (storage == nil || !storage->source) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                   reason:@"HuxerUI macOS ExternalTextureSource is unavailable"
                                 userInfo:nil];
  }
  return storage;
}

namespace huxerui::detail {

std::shared_ptr<MacExternalTextureState> MacExternalTextureState::Create(Size intrinsic_size) {
  return std::shared_ptr<MacExternalTextureState>(new MacExternalTextureState(intrinsic_size));
}

MacExternalTextureState::~MacExternalTextureState() {
  CVPixelBufferRef pending_frame = nullptr;
  {
    std::lock_guard lock(frame_mutex_);
    pending_frame = std::exchange(pending_frame_, nullptr);
  }
  if (pending_frame != nullptr) {
    CVPixelBufferRelease(pending_frame);
  }
}

void MacExternalTextureState::Publish(CVPixelBufferRef frame) {
  if (frame == nullptr) {
    throw std::invalid_argument("HuxerUI macOS external texture frame must not be null");
  }
  CVPixelBufferRetain(frame);
  CVPixelBufferRef replaced = nullptr;
  {
    std::lock_guard lock(frame_mutex_);
    if (finished_) {
      CVPixelBufferRelease(frame);
      throw std::logic_error("HuxerUI macOS external texture source is finished");
    }
    replaced = std::exchange(pending_frame_, frame);
  }
  if (replaced != nullptr) {
    CVPixelBufferRelease(replaced);
  }
  NotifyFrameAvailable();
}

void MacExternalTextureState::Finish() noexcept {
  std::lock_guard lock(frame_mutex_);
  finished_ = true;
}

CVPixelBufferRef MacExternalTextureState::AcquireLatestFrame() noexcept {
  std::lock_guard lock(frame_mutex_);
  return std::exchange(pending_frame_, nullptr);
}

} // namespace huxerui::detail

namespace huxerui::macos {

ExternalTextureSource::ExternalTextureSource(Size intrinsic_size)
    : state_(huxerui::detail::MacExternalTextureState::Create(intrinsic_size)) {}

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
    throw std::logic_error("HuxerUI macOS external texture source is empty");
  }
  state_->Publish(frame);
}

void ExternalTextureSource::Finish() noexcept {
  if (state_) {
    state_->Finish();
  }
}

} // namespace huxerui::macos

@implementation HUXExternalTexture

- (instancetype)initForHuxerUI {
  return [super init];
}

@end

namespace huxerui::macos::detail {

HUXExternalTexture* WrapExternalTexture(ExternalTexture texture) {
  if (!texture.HasValue()) {
    throw std::invalid_argument("HuxerUI macOS ExternalTexture must be valid");
  }
  HUXExternalTexture* result = [[HUXExternalTexture alloc] initForHuxerUI];
  HUXMacExternalTextureStorage* storage = [HUXMacExternalTextureStorage new];
  storage->texture = std::move(texture);
  objc_setAssociatedObject(result, &external_texture_storage_key, storage, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return result;
}

ExternalTexture UnwrapExternalTexture(HUXExternalTexture* texture) {
  if (texture == nil) {
    throw std::invalid_argument("HuxerUI macOS ExternalTexture must not be nil");
  }
  HUXMacExternalTextureStorage* storage = objc_getAssociatedObject(texture, &external_texture_storage_key);
  if (storage == nil || !storage->texture.HasValue()) {
    throw std::invalid_argument("HuxerUI macOS platform boundary received an invalid ExternalTexture value");
  }
  return storage->texture;
}

} // namespace huxerui::macos::detail

@implementation HUXExternalTextureSource

- (instancetype)initWithIntrinsicSize:(CGSize)size {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  try {
    auto storage = [HUXMacExternalTextureSourceStorage new];
    storage->source = std::make_unique<huxerui::macos::ExternalTextureSource>(
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
    return huxerui::macos::detail::WrapExternalTexture(SourceStorage(self)->source->Texture());
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
