#pragma once

#include <memory>

#include <emscripten/val.h>

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class WebRenderer;
} // namespace detail

namespace web {

/// A latest-frame Web ExternalTexture backed by cloned WebCodecs VideoFrame objects.
///
/// Construction, publication, finish, and destruction run on the browser main thread because emscripten::val is
/// thread-affine. The intrinsic size is immutable logical UI geometry rather than VideoFrame display dimensions.
///
/// @code
/// auto texture = std::make_shared<web::VideoFrameTexture>(Size{320.0F, 180.0F});
/// texture->Publish(video_frame);
/// video_frame.call<void>("close");
/// View preview = Image(texture).Fit(ImageFit::Cover);
/// @endcode
class VideoFrameTexture final : public huxerui::ExternalTexture {
public:
  /// Creates an empty VideoFrame mailbox with a finite, strictly positive logical size.
  explicit VideoFrameTexture(Size intrinsic_size) : huxerui::ExternalTexture(intrinsic_size) {}
  /// Closes the retained mailbox clone on the browser main thread.
  ~VideoFrameTexture();

  /// VideoFrameTexture identities cannot be copied; share them through std::shared_ptr.
  VideoFrameTexture(const VideoFrameTexture&) = delete;
  VideoFrameTexture& operator=(const VideoFrameTexture&) = delete;

  /// Clones an open VideoFrame into the latest-frame mailbox and schedules every Runtime displaying this texture.
  ///
  /// Ownership of the caller's frame does not change, so it may be closed immediately after this function returns.
  /// An invalid or closed frame throws std::invalid_argument; publication after Finish() throws std::logic_error.
  void Publish(const emscripten::val& video_frame);
  /// Idempotently stops publication while preserving the last successfully published frame clone.
  void Finish() noexcept;

private:
  [[nodiscard]] emscripten::val AcquireFrame() const noexcept;

  emscripten::val frame_ = emscripten::val::undefined();
  bool finished_ = false;

  friend class huxerui::detail::WebRenderer;
};

} // namespace web

} // namespace huxerui
