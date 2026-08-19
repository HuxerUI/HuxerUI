#include <huxerui/web/external_texture.h>

#include <stdexcept>
#include <utility>

#include <emscripten.h>

#include "web_external_texture_internal.h"

namespace huxerui::detail {

namespace {

// clang-format off
EM_JS(emscripten::EM_VAL, CloneWebVideoFrame, (emscripten::EM_VAL handle), {
  if (typeof VideoFrame === "undefined") {
    return Emval.toHandle(null);
  }
  try {
    return Emval.toHandle(VideoFrame.prototype.clone.call(Emval.toValue(handle)));
  } catch (_) {
    return Emval.toHandle(null);
  }
});

EM_JS(void, ReleaseWebVideoFrame, (emscripten::EM_VAL handle), {
  try {
    Emval.toValue(handle).close();
  } catch (_) {
  }
});
// clang-format on

} // namespace

std::shared_ptr<WebExternalTextureState> WebExternalTextureState::Create(Size intrinsic_size) {
  return std::shared_ptr<WebExternalTextureState>(new WebExternalTextureState(intrinsic_size));
}

WebExternalTextureState::~WebExternalTextureState() {
  CloseWebVideoFrame(pending_frame_);
}

void WebExternalTextureState::Publish(const emscripten::val& video_frame) {
  if (finished_) {
    throw std::logic_error("HuxerUI Web external texture source is finished");
  }
  emscripten::val cloned_frame = emscripten::val::take_ownership(CloneWebVideoFrame(video_frame.as_handle()));
  if (cloned_frame.isNull()) {
    throw std::invalid_argument("HuxerUI Web external texture frame must be an open VideoFrame");
  }
  CloseWebVideoFrame(pending_frame_);
  pending_frame_ = std::move(cloned_frame);
  NotifyFrameAvailable();
}

void WebExternalTextureState::Finish() noexcept {
  finished_ = true;
}

emscripten::val WebExternalTextureState::AcquireLatestFrame() noexcept {
  emscripten::val frame = std::move(pending_frame_);
  pending_frame_ = emscripten::val::undefined();
  return frame;
}

void CloseWebVideoFrame(emscripten::val& frame) noexcept {
  if (frame.isNull() || frame.isUndefined()) {
    return;
  }
  ReleaseWebVideoFrame(frame.as_handle());
  frame = emscripten::val::undefined();
}

} // namespace huxerui::detail

namespace huxerui::web {

ExternalTextureSource::ExternalTextureSource(Size intrinsic_size)
    : state_(detail::WebExternalTextureState::Create(intrinsic_size)) {}

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

void ExternalTextureSource::Publish(const emscripten::val& video_frame) {
  if (!state_) {
    throw std::logic_error("HuxerUI Web external texture source is empty");
  }
  state_->Publish(video_frame);
}

void ExternalTextureSource::Finish() noexcept {
  if (state_) {
    state_->Finish();
  }
}

} // namespace huxerui::web
