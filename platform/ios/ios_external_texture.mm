#include <huxerui/ios/external_texture.h>

#include <stdexcept>
#include <utility>

#include "ios_external_texture_internal.h"

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
    : state_(detail::IosExternalTextureState::Create(intrinsic_size)) {}

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
