#pragma once

#include <emscripten/val.h>

#include <huxerui/web/external_texture.h>

namespace huxerui::detail {

void CloseWebVideoFrame(emscripten::val& frame) noexcept;

} // namespace huxerui::detail
