#include "ohos_renderer.h"

#include <native_drawing/drawing_brush.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_matrix.h>
#include <native_drawing/drawing_path.h>
#include <native_drawing/drawing_path_effect.h>
#include <native_drawing/drawing_pen.h>
#include <native_drawing/drawing_point.h>
#include <native_drawing/drawing_rect.h>
#include <native_drawing/drawing_round_rect.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <variant>

#include "graphics/path_internal.h"
#include "internal_access.h"

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

Color SolidColor(const Brush& brush) {
  const auto* color = std::get_if<Color>(&brush.Get());
  if (!color) {
    throw std::logic_error("HuxerUI OHOS does not support gradients yet");
  }
  return *color;
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

class ScenePainter {
public:
  explicit ScenePainter(OH_Drawing_Canvas* canvas)
      : canvas_(canvas), brush_(Own<OH_Drawing_BrushDestroy>(OH_Drawing_BrushCreate())),
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
      std::visit([&](const auto& value) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, DrawTextCommand>) {
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
    auto matrix = Own<OH_Drawing_MatrixDestroy>(OH_Drawing_MatrixCreate());
    OH_Drawing_MatrixSetMatrix(matrix.get(), transform.m11, transform.m21, transform.translate_x,
                               transform.m12, transform.m22, transform.translate_y, 0.0F, 0.0F, 1.0F);
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
  void Fill(Color color, DrawShape draw) {
    OH_Drawing_BrushSetColor(brush_.get(), OhosDrawingColor(color));
    OH_Drawing_CanvasAttachBrush(canvas_, brush_.get());
    draw();
    OH_Drawing_CanvasDetachBrush(canvas_);
  }

  template <typename DrawShape>
  void Stroke(Color color, const StrokeStyle& style, DrawShape draw) {
    if (style.width <= 0.0F) {
      return;
    }
    OH_Drawing_PenReset(pen_.get());
    OH_Drawing_PenSetAntiAlias(pen_.get(), true);
    OH_Drawing_PenSetColor(pen_.get(), OhosDrawingColor(color));
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
    Fill(SolidColor(command.brush), [&] { DrawRoundedRect(command.rect, command.corner_radius); });
  }
  void Draw(const DrawTextRunsCommand& command) { PaintOhosTextRuns(canvas_, command); }
  void Draw(const DrawCircleCommand& command) {
    auto point = Own<OH_Drawing_PointDestroy>(OH_Drawing_PointCreate(command.center.x, command.center.y));
    Fill(command.color, [&] { OH_Drawing_CanvasDrawCircle(canvas_, point.get(), command.radius); });
  }
  void Draw(const DrawLineCommand& command) {
    Stroke(command.color, command.style, [&] {
      OH_Drawing_CanvasDrawLine(canvas_, command.start.x, command.start.y, command.end.x, command.end.y);
    });
  }
  void Draw(const DrawArcCommand& command) {
    const float radius = command.radius;
    const auto bounds = MakeRect({command.center.x - radius, command.center.y - radius, 2.0F * radius, 2.0F * radius});
    constexpr float degrees = 180.0F / std::numbers::pi_v<float>;
    Stroke(command.color, command.style, [&] {
      OH_Drawing_CanvasDrawArc(canvas_, bounds.get(), command.start_angle * degrees, command.sweep_angle * degrees);
    });
  }
  void Draw(const DrawBorderCommand& command) {
    const auto path = CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width);
    Draw(StrokePathCommand{path, command.color, command.rect, command.style});
  }
  void Draw(const FillPathCommand& command) {
    const auto path = MakePath(command.path, command.fill_rule);
    Fill(SolidColor(command.brush), [&] { OH_Drawing_CanvasDrawPath(canvas_, path.get()); });
  }
  void Draw(const StrokePathCommand& command) {
    const auto path = MakePath(command.path);
    Stroke(SolidColor(command.brush), command.style, [&] { OH_Drawing_CanvasDrawPath(canvas_, path.get()); });
  }
  void Draw(const PushClipCommand& command) { PushClip(command); }
  void Draw(const PushPathClipCommand& command) { PushClip(command); }
  void Draw(const PopClipCommand&) { OH_Drawing_CanvasRestore(canvas_); }
  void Draw(const PushTransformCommand& command) {
    OH_Drawing_CanvasSave(canvas_);
    Transform(command.transform);
  }
  void Draw(const PopTransformCommand&) { OH_Drawing_CanvasRestore(canvas_); }
  void Draw(const DrawImageCommand&) { throw std::logic_error("HuxerUI OHOS does not support raster images yet"); }
  void Draw(const DrawExternalTextureCommand&) {
    throw std::logic_error("HuxerUI OHOS does not support external textures yet");
  }
  void Draw(const DrawShadowCommand&) { throw std::logic_error("HuxerUI OHOS does not support shadows yet"); }
  void Draw(const DrawPathShadowCommand&) { throw std::logic_error("HuxerUI OHOS does not support path shadows yet"); }
  void Draw(const PlacePlatformViewCommand&) {
    throw std::logic_error("HuxerUI OHOS does not support platform views yet");
  }

  OH_Drawing_Canvas* canvas_;
  std::unique_ptr<OH_Drawing_Brush, decltype(&OH_Drawing_BrushDestroy)> brush_;
  std::unique_ptr<OH_Drawing_Pen, decltype(&OH_Drawing_PenDestroy)> pen_;
};

} // namespace

void PaintOhosScene(OH_Drawing_Canvas* canvas, const RenderScene& scene, float scale) {
  if (scene.root) {
    const CanvasState state(canvas);
    OH_Drawing_CanvasScale(canvas, scale, scale);
    ScenePainter(canvas).Draw(*scene.root);
  }
}

} // namespace huxerui::detail
