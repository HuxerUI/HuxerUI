#pragma once

#include <memory>

#include <emscripten/val.h>

#include "external_texture_internal.h"

namespace huxerui::detail {

class WebExternalTextureState final : public ExternalTextureState {
public:
  [[nodiscard]] static std::shared_ptr<WebExternalTextureState> Create(Size intrinsic_size);
  ~WebExternalTextureState() override;

  void Publish(const emscripten::val& video_frame);
  void Finish() noexcept;
  [[nodiscard]] emscripten::val AcquireLatestFrame() noexcept;

private:
  explicit WebExternalTextureState(Size intrinsic_size) : ExternalTextureState(intrinsic_size) {}

  emscripten::val pending_frame_ = emscripten::val::undefined();
  bool finished_ = false;
};

void CloseWebVideoFrame(emscripten::val& frame) noexcept;

} // namespace huxerui::detail
