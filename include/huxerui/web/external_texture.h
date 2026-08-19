#pragma once

#include <memory>

#include <emscripten/val.h>

#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class WebExternalTextureState;
} // namespace detail

namespace web {

class ExternalTextureSource final {
public:
  explicit ExternalTextureSource(Size intrinsic_size);
  ~ExternalTextureSource();

  ExternalTextureSource(const ExternalTextureSource&) = delete;
  ExternalTextureSource& operator=(const ExternalTextureSource&) = delete;
  ExternalTextureSource(ExternalTextureSource&& other) noexcept;
  ExternalTextureSource& operator=(ExternalTextureSource&& other) noexcept;

  [[nodiscard]] ExternalTexture Texture() const noexcept;
  void Publish(const emscripten::val& video_frame);
  void Finish() noexcept;

private:
  std::shared_ptr<detail::WebExternalTextureState> state_;
};

} // namespace web

} // namespace huxerui
