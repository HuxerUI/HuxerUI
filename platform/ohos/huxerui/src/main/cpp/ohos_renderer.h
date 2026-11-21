#pragma once

#include <cstdint>

#include <huxerui/color.h>
#include <huxerui/render_scene.h>

struct OH_Drawing_Canvas;

namespace huxerui::detail {

std::uint32_t OhosDrawingColor(Color color);
void PaintOhosScene(OH_Drawing_Canvas* canvas, const RenderScene& scene, float scale);

} // namespace huxerui::detail
