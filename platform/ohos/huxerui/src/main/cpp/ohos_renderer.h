#pragma once

#include <native_drawing/drawing_image.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/render_scene.h>

#include "graphics/paint_internal.h"

struct OH_Drawing_Canvas;

namespace huxerui::detail {

std::uint32_t OhosDrawingColor(Color color);

class OhosRenderer final {
public:
  void Paint(OH_Drawing_Canvas* canvas, const RenderScene& scene, float scale);
  void PaintSlice(OH_Drawing_Canvas* canvas, const RenderScene& scene, float scale,
                  std::size_t first_command, std::size_t command_count);

private:
  using DrawingImage = std::unique_ptr<OH_Drawing_Image, decltype(&OH_Drawing_ImageDestroy)>;

  struct CachedImage {
    std::uint64_t identity;
    std::size_t byte_size;
    DrawingImage image;
  };

  class ScenePainter;

  OH_Drawing_Image* ImageFor(const RasterImage& source);

  std::vector<CachedImage> images_;
  std::size_t image_bytes_ = 0;
};

} // namespace huxerui::detail
