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

void CloseWebVideoFrame(emscripten::val& frame) noexcept {
  if (frame.isNull() || frame.isUndefined()) {
    return;
  }
  ReleaseWebVideoFrame(frame.as_handle());
  frame = emscripten::val::undefined();
}

} // namespace huxerui::detail

namespace huxerui::web {

VideoFrameTexture::~VideoFrameTexture() {
  detail::CloseWebVideoFrame(frame_);
  Finish();
}

void VideoFrameTexture::Publish(const emscripten::val& video_frame) {
  if (finished_) {
    throw std::logic_error("HuxerUI Web external texture is finished");
  }
  emscripten::val cloned_frame = emscripten::val::take_ownership(detail::CloneWebVideoFrame(video_frame.as_handle()));
  if (cloned_frame.isNull()) {
    throw std::invalid_argument("HuxerUI Web external texture frame must be an open VideoFrame");
  }
  detail::CloseWebVideoFrame(frame_);
  frame_ = std::move(cloned_frame);
  NotifyFrameAvailable();
}

void VideoFrameTexture::Finish() noexcept {
  finished_ = true;
}

emscripten::val VideoFrameTexture::AcquireFrame() const noexcept {
  if (frame_.isNull() || frame_.isUndefined()) {
    return emscripten::val::undefined();
  }
  return emscripten::val::take_ownership(detail::CloneWebVideoFrame(frame_.as_handle()));
}

} // namespace huxerui::web
