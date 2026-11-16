#include <huxerui/web/external_texture.h>

#include <memory>
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
EM_JS(emscripten::EM_VAL, ReadWebFramePixels, (emscripten::EM_VAL handle, int width, int height), {
  try {
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    context.drawImage(Emval.toValue(handle), 0, 0, width, height);
    return Emval.toHandle(context.getImageData(0, 0, width, height).data);
  } catch (_) {
    return Emval.toHandle(null);
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

namespace {

Task<std::optional<Bitmap>> ReadVideoFrame(std::shared_ptr<emscripten::val> frame) {
  if (frame->isUndefined()) co_return std::nullopt;
  if (frame->isNull()) throw std::runtime_error("HuxerUI could not retain the Web video frame");
  const auto width = (*frame)["displayWidth"].as<int>();
  const auto height = (*frame)["displayHeight"].as<int>();
  if (width <= 0 || height <= 0) throw std::runtime_error("HuxerUI Web video frame dimensions are invalid");
  auto rgba = emscripten::val::take_ownership(detail::ReadWebFramePixels(frame->as_handle(), width, height));
  if (rgba.isNull()) throw std::runtime_error("HuxerUI could not read the Web video frame");
  Bytes pixels(rgba["length"].as<std::size_t>());
  emscripten::val(emscripten::typed_memory_view(pixels.size(), reinterpret_cast<unsigned char*>(pixels.data())))
      .call<void>("set", rgba);
  co_return Bitmap::FromRgba8(width, height, std::move(pixels));
}

} // namespace

Task<std::optional<Bitmap>> VideoFrameTexture::SnapshotAsync() const {
  auto frame = std::shared_ptr<emscripten::val>(new emscripten::val(AcquireFrame()), [](emscripten::val* value) {
    detail::CloseWebVideoFrame(*value);
    delete value;
  });
  return ReadVideoFrame(std::move(frame));
}

} // namespace huxerui::web
