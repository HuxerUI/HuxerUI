#include "ohos_renderer.h"

#include <native_drawing/drawing_bitmap.h>
#include <native_drawing/drawing_brush.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_filter.h>
#include <native_drawing/drawing_mask_filter.h>
#include <native_drawing/drawing_matrix.h>
#include <native_drawing/drawing_path.h>
#include <native_drawing/drawing_path_effect.h>
#include <native_drawing/drawing_pen.h>
#include <native_drawing/drawing_pixel_map.h>
#include <native_drawing/drawing_point.h>
#include <native_drawing/drawing_rect.h>
#include <native_drawing/drawing_round_rect.h>
#include <native_drawing/drawing_sampling_options.h>
#include <native_drawing/drawing_shader_effect.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/ohos/external_texture.h>

#include "graphics/path_internal.h"
#include "graphics/shadow_internal.h"
#include "internal_access.h"

#include "ohos_bitmap.h"
#include "ohos_text_layout.h"

namespace huxerui::detail {

std::uint32_t OhosDrawingColor(Color color) {
  const auto channel = [](float value) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
  };
  return (channel(color.alpha) << 24U) | (channel(color.red) << 16U) | (channel(color.green) << 8U) |
         channel(color.blue);
}

namespace {

template <auto destroy, class T>
auto Own(T* value) {
  if (!value) {
    throw std::runtime_error("HuxerUI failed to allocate an OHOS drawing resource");
  }
  return std::unique_ptr<T, decltype(destroy)>(value, destroy);
}

auto MakeRect(Rect rect) {
  return Own<OH_Drawing_RectDestroy>(OH_Drawing_RectCreate(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height));
}

auto MakeRoundRect(Rect rect, float radius) {
  const auto bounds = MakeRect(rect);
  return Own<OH_Drawing_RoundRectDestroy>(OH_Drawing_RoundRectCreate(bounds.get(), radius, radius));
}

auto MakeMatrix(const Transform2D& transform) {
  auto matrix = Own<OH_Drawing_MatrixDestroy>(OH_Drawing_MatrixCreate());
  OH_Drawing_MatrixSetMatrix(matrix.get(), transform.m11, transform.m21, transform.translate_x,
                             transform.m12, transform.m22, transform.translate_y, 0.0F, 0.0F, 1.0F);
  return matrix;
}

auto MakePath(const Path& path, PathFillRule rule = PathFillRule::NonZero) {
  auto result = Own<OH_Drawing_PathDestroy>(OH_Drawing_PathCreate());
  auto* target = result.get();
  OH_Drawing_PathSetFillType(target, rule == PathFillRule::EvenOdd ? PATH_FILL_TYPE_EVEN_ODD : PATH_FILL_TYPE_WINDING);
  for (const auto& element : InternalAccess::Elements(path)) {
    const auto& points = element.points;
    switch (element.verb) {
    case PathVerb::MoveTo: OH_Drawing_PathMoveTo(target, points[0].x, points[0].y); break;
    case PathVerb::LineTo: OH_Drawing_PathLineTo(target, points[0].x, points[0].y); break;
    case PathVerb::QuadraticTo:
      OH_Drawing_PathQuadTo(target, points[0].x, points[0].y, points[1].x, points[1].y);
      break;
    case PathVerb::CubicTo:
      OH_Drawing_PathCubicTo(target, points[0].x, points[0].y, points[1].x, points[1].y, points[2].x, points[2].y);
      break;
    case PathVerb::Close: OH_Drawing_PathClose(target); break;
    }
  }
  return result;
}

struct PaintSource {
  std::uint32_t color;
  std::unique_ptr<OH_Drawing_ShaderEffect, decltype(&OH_Drawing_ShaderEffectDestroy)> shader{
      nullptr, &OH_Drawing_ShaderEffectDestroy};
};

PaintSource ResolvePaintSource(const Brush& brush, Rect bounds) {
  return std::visit([bounds](const auto& value) -> PaintSource {
    using Value = std::remove_cvref_t<decltype(value)>;
    if constexpr (std::is_same_v<Value, Color>) {
      return {OhosDrawingColor(value)};
    } else {
      if (bounds.IsEmpty()) {
        return {OhosDrawingColor(value.stops.back().color)};
      }
      std::vector<std::uint32_t> colors;
      std::vector<float> positions;
      colors.reserve(value.stops.size());
      positions.reserve(value.stops.size());
      for (const GradientStop& stop : value.stops) {
        colors.push_back(OhosDrawingColor(stop.color));
        positions.push_back(stop.offset);
      }

      const auto matrix = MakeMatrix(ResolveGradientTransform(bounds, value));
      const auto count = static_cast<std::uint32_t>(colors.size());
      OH_Drawing_ShaderEffect* shader;
      if constexpr (std::is_same_v<Value, LinearGradient>) {
        const OH_Drawing_Point2D start{value.start.x, value.start.y};
        const OH_Drawing_Point2D end{value.end.x, value.end.y};
        shader = OH_Drawing_ShaderEffectCreateLinearGradientWithLocalMatrix(
            &start, &end, colors.data(), positions.data(), count, CLAMP, matrix.get());
      } else {
        const OH_Drawing_Point2D center{value.center.x, value.center.y};
        shader = OH_Drawing_ShaderEffectCreateRadialGradientWithLocalMatrix(
            &center, value.radius.width, colors.data(), positions.data(), count, CLAMP, matrix.get());
      }
      return {OhosDrawingColor(Color::White()), Own<OH_Drawing_ShaderEffectDestroy>(shader)};
    }
  }, brush.Get());
}

class CanvasState {
public:
  explicit CanvasState(OH_Drawing_Canvas* canvas) : canvas_(canvas), depth_(OH_Drawing_CanvasGetSaveCount(canvas)) {
    OH_Drawing_CanvasSave(canvas_);
  }
  ~CanvasState() { OH_Drawing_CanvasRestoreToCount(canvas_, depth_); }

  CanvasState(const CanvasState&) = delete;
  CanvasState& operator=(const CanvasState&) = delete;

private:
  OH_Drawing_Canvas* canvas_;
  std::uint32_t depth_;
};

} // namespace

class OhosRenderer::ScenePainter {
public:
  ScenePainter(OhosRenderer& renderer, OH_Drawing_Canvas* canvas, std::size_t first = 0,
               std::size_t end = std::numeric_limits<std::size_t>::max())
      : renderer_(renderer), canvas_(canvas), first_(first), end_(end),
        brush_(Own<OH_Drawing_BrushDestroy>(OH_Drawing_BrushCreate())),
        pen_(Own<OH_Drawing_PenDestroy>(OH_Drawing_PenCreate())) {
    OH_Drawing_BrushSetAntiAlias(brush_.get(), true);
  }

  ~ScenePainter() {
    OH_Drawing_CanvasDetachBrush(canvas_);
    OH_Drawing_CanvasDetachPen(canvas_);
  }

  void Draw(const RenderNode& node) {
    if (!node.visible || node.opacity <= 0.0F) {
      return;
    }
    const CanvasState state(canvas_);
    OH_Drawing_CanvasTranslate(canvas_, node.offset.x, node.offset.y);
    Transform(node.transform);
    if (node.opacity < 1.0F) {
      auto layer = Own<OH_Drawing_BrushDestroy>(OH_Drawing_BrushCreate());
      OH_Drawing_BrushSetAlpha(layer.get(), static_cast<std::uint8_t>(std::lround(node.opacity * 255.0F)));
      OH_Drawing_CanvasSaveLayer(canvas_, nullptr, layer.get());
    }
    Draw(node.content);
    if (!node.children.empty()) {
      const CanvasState children(canvas_);
      for (const auto& clip : node.child_clips) {
        std::visit([this](const auto& command) { Clip(command); }, clip);
      }
      Transform(node.children_transform);
      for (const auto* child : node.children) {
        Draw(*child);
      }
    }
    Draw(node.foreground);
  }

private:
  void Draw(const PaintSequence& sequence) {
    for (const auto& command : sequence.Commands()) {
      if (std::holds_alternative<PlacePlatformViewCommand>(command)) {
        continue;
      }
      const auto index = next_command_++;
      std::visit([&](const auto& value) {
        using Command = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Command, PushClipCommand> || std::is_same_v<Command, PushPathClipCommand> ||
                      std::is_same_v<Command, PopClipCommand> || std::is_same_v<Command, PushTransformCommand> ||
                      std::is_same_v<Command, PopTransformCommand>) {
          // A slice can begin inside a clip or transform established by an earlier command.
          Draw(value);
        } else if (index < first_ || index >= end_) {
          return;
        } else if constexpr (std::is_same_v<Command, DrawTextCommand>) {
          PaintOhosText(canvas_, value, InternalAccess::TextLayoutForPaint(sequence, command));
        } else {
          Draw(value);
        }
      }, command);
    }
  }

  void Transform(const Transform2D& transform) {
    if (transform.IsIdentity()) {
      return;
    }
    const auto matrix = MakeMatrix(transform);
    OH_Drawing_CanvasConcatMatrix(canvas_, matrix.get());
  }

  void Clip(const PushClipCommand& command) {
    if (command.corner_radius == 0.0F) {
      OH_Drawing_CanvasClipRect(canvas_, MakeRect(command.rect).get(), INTERSECT, true);
    } else {
      const auto clip = MakeRoundRect(command.rect, command.corner_radius);
      OH_Drawing_CanvasClipRoundRect(canvas_, clip.get(), INTERSECT, true);
    }
  }

  void Clip(const PushPathClipCommand& command) {
    OH_Drawing_CanvasClipPath(canvas_, MakePath(command.path, command.fill_rule).get(), INTERSECT, true);
  }

  template <typename Command>
  void PushClip(const Command& command) {
    OH_Drawing_CanvasSave(canvas_);
    Clip(command);
  }

  template <typename DrawShape>
  void Fill(const PaintSource& source, DrawShape draw, OH_Drawing_Filter* filter = nullptr) {
    OH_Drawing_BrushSetColor(brush_.get(), source.color);
    if (source.shader) {
      OH_Drawing_BrushSetShaderEffect(brush_.get(), source.shader.get());
    }
    if (filter) {
      OH_Drawing_BrushSetFilter(brush_.get(), filter);
    }
    OH_Drawing_CanvasAttachBrush(canvas_, brush_.get());
    draw();
    OH_Drawing_CanvasDetachBrush(canvas_);
    if (source.shader || filter) {
      OH_Drawing_BrushReset(brush_.get());
      OH_Drawing_BrushSetAntiAlias(brush_.get(), true);
    }
  }

  template <typename DrawShape>
  void FillShadow(Color color, float sigma, DrawShape draw) {
    const PaintSource source{OhosDrawingColor(color)};
    if (sigma <= 0.0F) {
      Fill(source, draw);
      return;
    }
    const auto blur = Own<OH_Drawing_MaskFilterDestroy>(OH_Drawing_MaskFilterCreateBlur(NORMAL, sigma, true));
    const auto filter = Own<OH_Drawing_FilterDestroy>(OH_Drawing_FilterCreate());
    OH_Drawing_FilterSetMaskFilter(filter.get(), blur.get());
    Fill(source, draw, filter.get());
  }

  template <typename DrawShape>
  void Stroke(const PaintSource& source, const StrokeStyle& style, DrawShape draw) {
    if (style.width <= 0.0F) {
      return;
    }
    OH_Drawing_PenReset(pen_.get());
    OH_Drawing_PenSetAntiAlias(pen_.get(), true);
    OH_Drawing_PenSetColor(pen_.get(), source.color);
    if (source.shader) {
      OH_Drawing_PenSetShaderEffect(pen_.get(), source.shader.get());
    }
    OH_Drawing_PenSetWidth(pen_.get(), style.width);
    OH_Drawing_PenSetCap(pen_.get(), style.cap == StrokeCap::Round ? LINE_ROUND_CAP :
                         style.cap == StrokeCap::Square ? LINE_SQUARE_CAP : LINE_FLAT_CAP);
    OH_Drawing_PenSetJoin(pen_.get(), style.join == StrokeJoin::Round ? LINE_ROUND_JOIN :
                          style.join == StrokeJoin::Bevel ? LINE_BEVEL_JOIN : LINE_MITER_JOIN);
    OH_Drawing_PenSetMiterLimit(pen_.get(), style.miter_limit);
    std::unique_ptr<OH_Drawing_PathEffect, decltype(&OH_Drawing_PathEffectDestroy)> dash(nullptr,
                                                                                       &OH_Drawing_PathEffectDestroy);
    if (!style.dash_pattern.empty()) {
      auto intervals = style.dash_pattern;
      dash = Own<OH_Drawing_PathEffectDestroy>(
          OH_Drawing_CreateDashPathEffect(intervals.data(), static_cast<int>(intervals.size()), style.dash_offset));
      OH_Drawing_PenSetPathEffect(pen_.get(), dash.get());
    }
    OH_Drawing_CanvasAttachPen(canvas_, pen_.get());
    draw();
    OH_Drawing_CanvasDetachPen(canvas_);
  }

  void DrawRoundedRect(Rect rect, float radius) {
    if (radius == 0.0F) {
      OH_Drawing_CanvasDrawRect(canvas_, MakeRect(rect).get());
    } else {
      OH_Drawing_CanvasDrawRoundRect(canvas_, MakeRoundRect(rect, radius).get());
    }
  }

  void Draw(const DrawRectCommand& command) {
    Fill(ResolvePaintSource(command.brush, command.rect), [&] { DrawRoundedRect(command.rect, command.corner_radius); });
  }
  void Draw(const DrawTextRunsCommand& command) { PaintOhosTextRuns(canvas_, command); }

  void Draw(const DrawImageCommand& command) {
    const float scale = RasterImageScale(command.image);
    const auto& src = command.source;
    DrawImage(command.image, {src.x * scale, src.y * scale, src.width * scale, src.height * scale},
              command.destination, command.sampling, command.opacity);
  }

  void DrawImage(const RasterImage& source, Rect source_pixels, Rect destination, ImageSampling sampling_mode,
                 float opacity) {
    const auto* image = renderer_.ImageFor(source);
    if (!image) {
      return;
    }
    const auto src = MakeRect(source_pixels);
    const auto dst = MakeRect(destination);
    const auto filter = sampling_mode == ImageSampling::Nearest ? FILTER_MODE_NEAREST : FILTER_MODE_LINEAR;
    const auto sampling = Own<OH_Drawing_SamplingOptionsDestroy>(
        OH_Drawing_SamplingOptionsCreate(filter, MIPMAP_MODE_NONE));
    const PaintSource paint{OhosDrawingColor(Color::Rgb(255, 255, 255, opacity))};
    Fill(paint, [&] {
      OH_Drawing_CanvasDrawImageRectWithSrc(canvas_, image, src.get(), dst.get(), sampling.get(), STRICT_SRC_RECT_CONSTRAINT);
    });
  }
  void Draw(const DrawCircleCommand& command) {
    auto point = Own<OH_Drawing_PointDestroy>(OH_Drawing_PointCreate(command.center.x, command.center.y));
    Fill({OhosDrawingColor(command.color)}, [&] { OH_Drawing_CanvasDrawCircle(canvas_, point.get(), command.radius); });
  }
  void Draw(const DrawLineCommand& command) {
    Stroke({OhosDrawingColor(command.color)}, command.style, [&] {
      OH_Drawing_CanvasDrawLine(canvas_, command.start.x, command.start.y, command.end.x, command.end.y);
    });
  }
  void Draw(const DrawArcCommand& command) {
    const float radius = command.radius;
    const auto bounds = MakeRect({command.center.x - radius, command.center.y - radius, 2.0F * radius, 2.0F * radius});
    constexpr float degrees = 180.0F / std::numbers::pi_v<float>;
    Stroke({OhosDrawingColor(command.color)}, command.style, [&] {
      OH_Drawing_CanvasDrawArc(canvas_, bounds.get(), command.start_angle * degrees, command.sweep_angle * degrees);
    });
  }
  void Draw(const DrawBorderCommand& command) {
    const auto path = CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width);
    Draw(StrokePathCommand{path, command.color, command.rect, command.style});
  }
  void Draw(const DrawShadowCommand& command) {
    const auto shadow = ResolveShadow(command);
    if (shadow.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    FillShadow(command.color, shadow.standard_deviation, [&] { DrawRoundedRect(shadow.caster, shadow.corner_radius); });
  }
  void Draw(const FillPathCommand& command) {
    const auto path = MakePath(command.path, command.fill_rule);
    Fill(ResolvePaintSource(command.brush, command.brush_bounds), [&] {
      OH_Drawing_CanvasDrawPath(canvas_, path.get());
    });
  }
  void Draw(const StrokePathCommand& command) {
    const auto path = MakePath(command.path);
    Stroke(ResolvePaintSource(command.brush, command.brush_bounds), command.style, [&] {
      OH_Drawing_CanvasDrawPath(canvas_, path.get());
    });
  }
  void Draw(const DrawPathShadowCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    const auto path = MakePath(command.path, command.fill_rule);
    const CanvasState state(canvas_);
    OH_Drawing_CanvasTranslate(canvas_, command.offset.x, command.offset.y);
    FillShadow(command.color, command.blur_radius / 3.0F, [&] { OH_Drawing_CanvasDrawPath(canvas_, path.get()); });
  }
  void Draw(const PushClipCommand& command) { PushClip(command); }
  void Draw(const PushPathClipCommand& command) { PushClip(command); }
  void Draw(const PopClipCommand&) { OH_Drawing_CanvasRestore(canvas_); }
  void Draw(const PushTransformCommand& command) {
    OH_Drawing_CanvasSave(canvas_);
    Transform(command.transform);
  }
  void Draw(const PopTransformCommand&) { OH_Drawing_CanvasRestore(canvas_); }
  void Draw(const DrawExternalTextureCommand& command) {
    if (const auto* texture = dynamic_cast<const ohos::NativeBufferTexture*>(command.texture.get())) {
      if (const auto frame = texture->AcquireFrame()) {
        const auto intrinsic = texture->IntrinsicSize();
        const float scale_x = frame->pixel_size.width / intrinsic.width;
        const float scale_y = frame->pixel_size.height / intrinsic.height;
        const auto& source = command.source;
        const auto src = MakeRect({source.x * scale_x, source.y * scale_y,
                                   source.width * scale_x, source.height * scale_y});
        const auto dst = MakeRect(command.destination);
        const auto filter = command.sampling == ImageSampling::Nearest ? FILTER_MODE_NEAREST : FILTER_MODE_LINEAR;
        const auto sampling = Own<OH_Drawing_SamplingOptionsDestroy>(
            OH_Drawing_SamplingOptionsCreate(filter, MIPMAP_MODE_NONE));
        const auto pixelmap = Own<OH_Drawing_PixelMapDissolve>(
            OH_Drawing_PixelMapGetFromOhPixelMapNative(frame->pixelmap.get()));
        Fill({OhosDrawingColor(Color::Rgb(255, 255, 255, command.opacity))}, [&] {
          OH_Drawing_CanvasDrawPixelMapRect(canvas_, pixelmap.get(), src.get(), dst.get(), sampling.get());
        });
      }
      return;
    }
    const auto* texture = dynamic_cast<const ohos::PixelTexture*>(command.texture.get());
    if (!texture) {
      throw std::logic_error("HuxerUI expected a HarmonyOS external texture");
    }
    const auto frame = texture->AcquireFrame();
    if (!frame) {
      return;
    }
    const auto intrinsic = texture->IntrinsicSize();
    const float scale_x = static_cast<float>(frame->PixelWidth()) / intrinsic.width;
    const float scale_y = static_cast<float>(frame->PixelHeight()) / intrinsic.height;
    const auto& src = command.source;
    DrawImage(*frame, {src.x * scale_x, src.y * scale_y, src.width * scale_x, src.height * scale_y},
              command.destination, command.sampling, command.opacity);
  }
  void Draw(const PlacePlatformViewCommand&) {}

  OhosRenderer& renderer_;
  OH_Drawing_Canvas* canvas_;
  std::size_t first_;
  std::size_t end_;
  std::size_t next_command_ = 0;
  std::unique_ptr<OH_Drawing_Brush, decltype(&OH_Drawing_BrushDestroy)> brush_;
  std::unique_ptr<OH_Drawing_Pen, decltype(&OH_Drawing_PenDestroy)> pen_;
};

OH_Drawing_Image* OhosRenderer::ImageFor(const RasterImage& source) {
  const auto identity = RasterImageIdentity(source);
  const auto cached = std::ranges::find(images_, identity, &CachedImage::identity);
  if (cached != images_.end()) {
    std::rotate(cached, cached + 1, images_.end());
    return images_.back().image.get();
  }

  DrawingImage image(nullptr, &OH_Drawing_ImageDestroy);
  std::size_t byte_size = 0;
  try {
    const Bitmap bitmap = std::visit([](const auto& value) -> Bitmap {
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, Bitmap>) {
        return value;
      } else {
        return DecodeOhosBitmap(value);
      }
    }, source);
    const auto drawing_bitmap = Own<OH_Drawing_BitmapDestroy>(OH_Drawing_BitmapCreate());
    const OH_Drawing_BitmapFormat format{COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_UNPREMUL};
    OH_Drawing_BitmapBuild(drawing_bitmap.get(), bitmap.PixelWidth(), bitmap.PixelHeight(), &format);
    auto* pixels = OH_Drawing_BitmapGetPixels(drawing_bitmap.get());
    if (!pixels) {
      throw std::runtime_error("HuxerUI could not allocate OHOS drawing pixels");
    }
    std::memcpy(pixels, bitmap.Pixels().data(), bitmap.Pixels().size());
    auto decoded = Own<OH_Drawing_ImageDestroy>(OH_Drawing_ImageCreate());
    if (OH_Drawing_ImageBuildFromBitmap(decoded.get(), drawing_bitmap.get())) {
      image = std::move(decoded);
      byte_size = bitmap.Pixels().size();
    }
  } catch (const std::runtime_error&) {
    // Keep a failed decode in the cache so an invalid asset does not interrupt every frame.
  }

  constexpr std::size_t image_cache_capacity = 64;
  constexpr std::size_t image_cache_budget = 64U * 1024U * 1024U;
  while (!images_.empty() && (images_.size() >= image_cache_capacity ||
                            image_bytes_ > image_cache_budget - std::min(byte_size, image_cache_budget))) {
    image_bytes_ -= images_.front().byte_size;
    images_.erase(images_.begin());
  }
  images_.push_back({identity, byte_size, std::move(image)});
  image_bytes_ += byte_size;
  return images_.back().image.get();
}

void OhosRenderer::Paint(OH_Drawing_Canvas* canvas, const RenderScene& scene, float scale) {
  PaintSlice(canvas, scene, scale, 0, std::numeric_limits<std::size_t>::max());
}

void OhosRenderer::PaintSlice(OH_Drawing_Canvas* canvas, const RenderScene& scene, float scale,
                              std::size_t first_command, std::size_t command_count) {
  if (scene.root && command_count != 0) {
    const CanvasState state(canvas);
    OH_Drawing_CanvasScale(canvas, scale, scale);
    ScenePainter(*this, canvas, first_command, first_command + command_count).Draw(*scene.root);
  }
}

} // namespace huxerui::detail
