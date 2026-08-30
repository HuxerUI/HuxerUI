#include <huxerui/paint.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include <huxerui/vector.h>

#include "geometry_internal.h"
#include "path_internal.h"
#include "shadow_internal.h"
#include "vector_internal.h"

namespace huxerui {
namespace {

bool IsFinite(Point point) noexcept {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

bool IsValid(Rect rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height) &&
         rect.width >= 0.0F && rect.height >= 0.0F;
}

bool IsFinite(Color color) noexcept {
  return std::isfinite(color.red) && std::isfinite(color.green) && std::isfinite(color.blue) &&
         std::isfinite(color.alpha);
}

bool IsFinite(Transform2D transform) noexcept {
  return std::isfinite(transform.m11) && std::isfinite(transform.m12) && std::isfinite(transform.m21) &&
         std::isfinite(transform.m22) && std::isfinite(transform.translate_x) && std::isfinite(transform.translate_y);
}

void RequireRect(Rect rect) {
  if (!IsValid(rect)) {
    throw std::invalid_argument("HuxerUI paint rectangle must be finite with non-negative dimensions");
  }
}

void RequireColor(Color color) {
  if (!IsFinite(color)) {
    throw std::invalid_argument("HuxerUI paint color must be finite");
  }
}

void RequireTextStyle(const TextStyle& style) {
  RequireColor(style.foreground);
  if (!std::isfinite(style.font.Size()) || style.font.Size() <= 0.0F) {
    throw std::invalid_argument("HuxerUI paint font size must be finite and greater than zero");
  }
}

void RequireTextRun(std::string_view text) {
  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
  }
}

void RequireNonNegative(float value, const char* message) {
  if (!std::isfinite(value) || value < 0.0F) {
    throw std::invalid_argument(message);
  }
}

StrokeStyle NormalizeStrokeStyle(StrokeStyle style) {
  RequireNonNegative(style.width, "HuxerUI stroke width must be finite and non-negative");
  switch (style.cap) {
  case StrokeCap::Butt:
  case StrokeCap::Round:
  case StrokeCap::Square:
    break;
  default:
    throw std::invalid_argument("HuxerUI stroke cap is invalid");
  }
  switch (style.join) {
  case StrokeJoin::Miter:
  case StrokeJoin::Round:
  case StrokeJoin::Bevel:
    break;
  default:
    throw std::invalid_argument("HuxerUI stroke join is invalid");
  }
  if (!std::isfinite(style.miter_limit) || style.miter_limit < 1.0F) {
    throw std::invalid_argument("HuxerUI stroke miter limit must be finite and at least one");
  }
  if (!std::isfinite(style.dash_offset)) {
    throw std::invalid_argument("HuxerUI stroke dash offset must be finite");
  }

  double cycle = 0.0;
  for (const float length : style.dash_pattern) {
    if (!std::isfinite(length) || length < 0.0F) {
      throw std::invalid_argument("HuxerUI stroke dash lengths must be finite and non-negative");
    }
    cycle += length;
  }
  if (style.dash_pattern.size() % 2 != 0) {
    const std::size_t original_size = style.dash_pattern.size();
    if (original_size > style.dash_pattern.max_size() - original_size) {
      throw std::invalid_argument("HuxerUI stroke dash pattern is too large");
    }
    style.dash_pattern.reserve(original_size * 2);
    for (std::size_t index = 0; index < original_size; ++index) {
      style.dash_pattern.push_back(style.dash_pattern[index]);
    }
    cycle *= 2.0;
  }
  if (!std::isfinite(cycle) || cycle > std::numeric_limits<float>::max()) {
    throw std::invalid_argument("HuxerUI stroke dash cycle must be finite");
  }
  if (cycle == 0.0) {
    style.dash_pattern.clear();
    style.dash_offset = 0.0F;
    return style;
  }

  style.dash_offset = std::fmod(style.dash_offset, static_cast<float>(cycle));
  if (style.dash_offset < 0.0F) {
    style.dash_offset += static_cast<float>(cycle);
  }
  return style;
}

void RequireCornerRadii(CornerRadii corner_radii) {
  RequireNonNegative(corner_radii.top_left, "HuxerUI paint corner radii must be finite and non-negative");
  RequireNonNegative(corner_radii.top_right, "HuxerUI paint corner radii must be finite and non-negative");
  RequireNonNegative(corner_radii.bottom_right, "HuxerUI paint corner radii must be finite and non-negative");
  RequireNonNegative(corner_radii.bottom_left, "HuxerUI paint corner radii must be finite and non-negative");
}

void RequireGradientStops(const std::vector<GradientStop>& stops) {
  if (stops.size() < 2) {
    throw std::invalid_argument("HuxerUI paint gradient requires at least two stops");
  }
  float previous = -1.0F;
  for (const GradientStop& stop : stops) {
    if (!std::isfinite(stop.offset) || stop.offset < 0.0F || stop.offset > 1.0F || stop.offset < previous) {
      throw std::invalid_argument("HuxerUI paint gradient stops must be ordered within [0, 1]");
    }
    RequireColor(stop.color);
    previous = stop.offset;
  }
}
} // namespace

PaintContext::PaintContext(PaintSequence& sequence, Rect bounds) : sequence_(sequence), bounds_(bounds) {
  RequireRect(bounds);
  sequence_.commands_.clear();
  sequence_.bounds_ = {};
  sequence_.has_external_texture_commands_ = false;
}

void PaintContext::DrawRect(Rect rect, Color color, CornerRadii corner_radii) {
  RequireOpen();
  RequireRect(rect);
  RequireColor(color);
  RequireCornerRadii(corner_radii);
  corner_radii = detail::NormalizeCornerRadii(rect, corner_radii);
  if (corner_radii.IsUniform()) {
    sequence_.commands_.emplace_back(DrawRectCommand{rect, color, corner_radii.top_left});
    Include(rect);
    return;
  }
  FillPath(Path::RoundedRect(rect, corner_radii), color);
}

void PaintContext::DrawLinearGradient(Rect rect, LinearGradient gradient, CornerRadii corner_radii) {
  RequireOpen();
  RequireRect(rect);
  RequireCornerRadii(corner_radii);
  if (!IsFinite(gradient.start) || !IsFinite(gradient.end)) {
    throw std::invalid_argument("HuxerUI linear gradient endpoints must be finite");
  }
  RequireGradientStops(gradient.stops);
  corner_radii = detail::NormalizeCornerRadii(rect, corner_radii);
  if (!corner_radii.IsUniform()) {
    PushClip(rect, corner_radii);
  }
  const float command_corner_radius = corner_radii.IsUniform() ? corner_radii.top_left : 0.0F;
  sequence_.commands_.emplace_back(DrawLinearGradientCommand{rect, std::move(gradient), command_corner_radius});
  Include(rect);
  if (!corner_radii.IsUniform()) {
    PopClip();
  }
}

void PaintContext::DrawRadialGradient(Rect rect, RadialGradient gradient, CornerRadii corner_radii) {
  RequireOpen();
  RequireRect(rect);
  RequireCornerRadii(corner_radii);
  if (!IsFinite(gradient.center) || !std::isfinite(gradient.radius.width) || !std::isfinite(gradient.radius.height) ||
      gradient.radius.width <= 0.0F || gradient.radius.height <= 0.0F) {
    throw std::invalid_argument("HuxerUI radial gradient geometry must be finite with positive radii");
  }
  RequireGradientStops(gradient.stops);
  corner_radii = detail::NormalizeCornerRadii(rect, corner_radii);
  if (!corner_radii.IsUniform()) {
    PushClip(rect, corner_radii);
  }
  const float command_corner_radius = corner_radii.IsUniform() ? corner_radii.top_left : 0.0F;
  sequence_.commands_.emplace_back(DrawRadialGradientCommand{rect, std::move(gradient), command_corner_radius});
  Include(rect);
  if (!corner_radii.IsUniform()) {
    PopClip();
  }
}

void PaintContext::DrawText(
    Rect rect, std::string text, TextStyle style, TextLayoutOptions options, Point paragraph_offset
) {
  RequireOpen();
  RequireRect(rect);
  RequireTextStyle(style);
  if (!IsFinite(paragraph_offset)) {
    throw std::invalid_argument("HuxerUI text paragraph offset must be finite");
  }
  sequence_.commands_.emplace_back(
      DrawTextCommand{rect, std::move(text), std::move(style), std::move(options), paragraph_offset}
  );
  Include(rect);
}

void PaintContext::DrawTextRun(
    Rect bounds, Point baseline_origin, std::string text, TextStyle style, TextShapingOptions shaping
) {
  RequireOpen();
  RequireRect(bounds);
  if (!IsFinite(baseline_origin)) {
    throw std::invalid_argument("HuxerUI text run baseline origin must be finite");
  }
  RequireTextStyle(style);
  RequireTextRun(text);
  if (text.empty() || bounds.IsEmpty()) {
    return;
  }
  TextRun run{bounds, baseline_origin, std::move(text), std::move(style), std::move(shaping)};
  if (!sequence_.commands_.empty()) {
    if (auto* command = std::get_if<DrawTextRunsCommand>(&sequence_.commands_.back())) {
      command->runs.push_back(std::move(run));
      Include(bounds);
      return;
    }
  }
  sequence_.commands_.emplace_back(DrawTextRunsCommand{{std::move(run)}});
  Include(bounds);
}

void PaintContext::DrawTextRuns(std::vector<TextRun> runs) {
  RequireOpen();
  if (runs.empty()) {
    return;
  }
  for (const TextRun& run : runs) {
    RequireRect(run.bounds);
    if (!IsFinite(run.baseline_origin)) {
      throw std::invalid_argument("HuxerUI text run baseline origin must be finite");
    }
    RequireTextStyle(run.style);
    RequireTextRun(run.text);
  }
  std::erase_if(runs, [](const TextRun& run) { return run.text.empty() || run.bounds.IsEmpty(); });
  if (runs.empty()) {
    return;
  }
  for (const TextRun& run : runs) {
    Include(run.bounds);
  }
  if (!sequence_.commands_.empty()) {
    if (auto* command = std::get_if<DrawTextRunsCommand>(&sequence_.commands_.back())) {
      command->runs
          .insert(command->runs.end(), std::make_move_iterator(runs.begin()), std::make_move_iterator(runs.end()));
      return;
    }
  }
  sequence_.commands_.emplace_back(DrawTextRunsCommand{std::move(runs)});
}

void PaintContext::DrawImage(ImageAsset image, Rect destination, ImageSampling sampling, float opacity) {
  const Size intrinsic = image.IntrinsicSize();
  DrawImageRect(std::move(image), {0.0F, 0.0F, intrinsic.width, intrinsic.height}, destination, sampling, opacity);
}

void PaintContext::DrawImageRect(
    ImageAsset image, Rect source, Rect destination, ImageSampling sampling, float opacity
) {
  RequireOpen();
  if (!image.HasValue()) {
    throw std::invalid_argument("HuxerUI paint image must not be empty");
  }
  RequireRect(source);
  RequireRect(destination);
  const Size intrinsic = image.IntrinsicSize();
  if (source.x < 0.0F || source.y < 0.0F || source.x + source.width > intrinsic.width ||
      source.y + source.height > intrinsic.height) {
    throw std::invalid_argument("HuxerUI image source rectangle must be inside the intrinsic image bounds");
  }
  if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F) {
    throw std::invalid_argument("HuxerUI image opacity must be finite and between zero and one");
  }
  if (source.IsEmpty() || destination.IsEmpty() || opacity <= 0.0F) {
    return;
  }
  sequence_.commands_.emplace_back(
      DrawImageCommand{
          std::move(image),
          source,
          destination,
          sampling,
          opacity,
      }
  );
  Include(destination);
}

void PaintContext::DrawImage(
    ExternalTexture texture, Rect destination, ImageSampling sampling, float opacity
) {
  const Size intrinsic = texture.IntrinsicSize();
  DrawImageRect(
      std::move(texture), {0.0F, 0.0F, intrinsic.width, intrinsic.height}, destination, sampling, opacity
  );
}

void PaintContext::DrawImageRect(
    ExternalTexture texture, Rect source, Rect destination, ImageSampling sampling, float opacity
) {
  RequireOpen();
  if (!texture.HasValue()) {
    throw std::invalid_argument("HuxerUI paint external texture must not be empty");
  }
  RequireRect(source);
  RequireRect(destination);
  const Size intrinsic = texture.IntrinsicSize();
  if (source.x < 0.0F || source.y < 0.0F || source.x + source.width > intrinsic.width ||
      source.y + source.height > intrinsic.height) {
    throw std::invalid_argument("HuxerUI external texture source rectangle must be inside the intrinsic bounds");
  }
  if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F) {
    throw std::invalid_argument("HuxerUI external texture opacity must be finite and between zero and one");
  }
  if (source.IsEmpty() || destination.IsEmpty() || opacity <= 0.0F) {
    return;
  }
  sequence_.has_external_texture_commands_ = true;
  sequence_.commands_.emplace_back(
      DrawExternalTextureCommand{
          std::move(texture),
          source,
          destination,
          sampling,
          opacity,
      }
  );
  Include(destination);
}

void PaintContext::DrawImage(VectorAsset image, Rect destination, std::optional<Color> tint, float opacity) {
  const Size intrinsic = image.IntrinsicSize();
  DrawImageRect(std::move(image), {0.0F, 0.0F, intrinsic.width, intrinsic.height}, destination, tint, opacity);
}

void PaintContext::DrawImageRect(
    VectorAsset image, Rect source, Rect destination, std::optional<Color> tint, float opacity
) {
  RequireOpen();
  if (!image.HasValue()) {
    throw std::invalid_argument("HuxerUI paint vector image must not be empty");
  }
  RequireRect(source);
  RequireRect(destination);
  if (tint.has_value()) {
    RequireColor(*tint);
  }
  const Size intrinsic = image.IntrinsicSize();
  if (source.x < 0.0F || source.y < 0.0F || source.x + source.width > intrinsic.width ||
      source.y + source.height > intrinsic.height) {
    throw std::invalid_argument("HuxerUI vector image source rectangle must be inside the intrinsic image bounds");
  }
  if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F) {
    throw std::invalid_argument("HuxerUI vector image opacity must be finite and between zero and one");
  }
  if (source.IsEmpty() || destination.IsEmpty() || opacity <= 0.0F) {
    return;
  }

  const Rect view_box = image.ViewBox();
  const float scale_x = intrinsic.width / view_box.width * destination.width / source.width;
  const float scale_y = intrinsic.height / view_box.height * destination.height / source.height;
  const Transform2D placement{
      scale_x,
      0.0F,
      0.0F,
      scale_y,
      destination.x - source.x * destination.width / source.width - view_box.x * scale_x,
      destination.y - source.y * destination.height / source.height - view_box.y * scale_y,
  };
  const auto resolve_color = [tint, opacity](Color color) {
    if (tint.has_value()) {
      color.red = tint->red;
      color.green = tint->green;
      color.blue = tint->blue;
      color.alpha *= tint->alpha;
    }
    color.alpha *= opacity;
    return color;
  };

  PushClip(destination);
  PushTransform(placement);
  for (const PaintCommand& command : detail::VectorAccess::Sequence(image).Commands()) {
    std::visit(
        [this, &resolve_color](const auto& value) {
          using Command = std::decay_t<decltype(value)>;
          if constexpr (std::same_as<Command, FillPathCommand>) {
            FillPath(value.path, resolve_color(value.color), value.fill_rule);
          } else if constexpr (std::same_as<Command, StrokePathCommand>) {
            StrokePath(value.path, resolve_color(value.color), value.style);
          } else if constexpr (std::same_as<Command, PushPathClipCommand>) {
            PushPathClip(value.path, value.fill_rule);
          } else if constexpr (std::same_as<Command, PopClipCommand>) {
            PopClip();
          } else if constexpr (std::same_as<Command, PushTransformCommand>) {
            PushTransform(value.transform);
          } else if constexpr (std::same_as<Command, PopTransformCommand>) {
            PopTransform();
          } else {
            throw std::logic_error("HuxerUI vector image contains an unsupported paint command");
          }
        },
        command
    );
  }
  PopTransform();
  PopClip();
  Include(destination);
}

void PaintContext::DrawCircle(Point center, float radius, Color color) {
  RequireOpen();
  if (!IsFinite(center)) {
    throw std::invalid_argument("HuxerUI paint circle center must be finite");
  }
  RequireNonNegative(radius, "HuxerUI paint circle radius must be finite and non-negative");
  RequireColor(color);
  sequence_.commands_.emplace_back(DrawCircleCommand{center, radius, color});
  Include({
      center.x - radius,
      center.y - radius,
      radius * 2.0F,
      radius * 2.0F,
  });
}

void PaintContext::DrawLine(Point start, Point end, Color color, StrokeStyle style) {
  RequireOpen();
  if (!IsFinite(start) || !IsFinite(end)) {
    throw std::invalid_argument("HuxerUI paint line endpoints must be finite");
  }
  RequireColor(color);
  style = NormalizeStrokeStyle(std::move(style));
  sequence_.commands_.emplace_back(DrawLineCommand{start, end, color, std::move(style)});
  const auto& command = std::get<DrawLineCommand>(sequence_.commands_.back());
  if (start == end || command.style.width <= 0.0F) {
    return;
  }
  const float outset = command.style.width *
                        (command.style.cap == StrokeCap::Square ? 0.70710678118F : 0.5F);
  Include({
      std::min(start.x, end.x) - outset,
      std::min(start.y, end.y) - outset,
      std::abs(end.x - start.x) + outset * 2.0F,
      std::abs(end.y - start.y) + outset * 2.0F,
  });
}

void PaintContext::DrawArc(Point center, float radius, float start_angle, float sweep_angle, Color color,
                           StrokeStyle style) {
  RequireOpen();
  if (!IsFinite(center)) {
    throw std::invalid_argument("HuxerUI paint arc center must be finite");
  }
  RequireNonNegative(radius, "HuxerUI paint arc radius must be finite and non-negative");
  if (!std::isfinite(start_angle) || !std::isfinite(sweep_angle)) {
    throw std::invalid_argument("HuxerUI paint arc angles must be finite radians");
  }
  RequireColor(color);
  style = NormalizeStrokeStyle(std::move(style));
  sequence_.commands_.emplace_back(
      DrawArcCommand{
          center,
          radius,
          start_angle,
          sweep_angle,
          color,
          std::move(style),
      }
  );
  const auto& command = std::get<DrawArcCommand>(sequence_.commands_.back());
  if (command.style.width <= 0.0F || radius <= 0.0F || sweep_angle == 0.0F) {
    return;
  }
  const float cap_outset =
      command.style.cap == StrokeCap::Square ? command.style.width * 0.70710678118F : command.style.width * 0.5F;
  const float extent = std::max(0.0F, radius) + cap_outset;
  Include({
      center.x - extent,
      center.y - extent,
      extent * 2.0F,
      extent * 2.0F,
  });
}

void PaintContext::DrawBorder(Rect rect, Color color, StrokeStyle style, CornerRadii corner_radii) {
  RequireOpen();
  RequireRect(rect);
  RequireColor(color);
  style = NormalizeStrokeStyle(std::move(style));
  RequireCornerRadii(corner_radii);
  corner_radii = detail::NormalizeCornerRadii(rect, corner_radii);
  if (!corner_radii.IsUniform()) {
    StrokePath(detail::CreateBorderStrokePath(rect, corner_radii, style.width), color, std::move(style));
    return;
  }
  const float width = style.width;
  sequence_.commands_.emplace_back(DrawBorderCommand{rect, color, std::move(style), corner_radii.top_left});
  if (width <= 0.0F || rect.IsEmpty()) {
    return;
  }
  const float outset = width * 0.5F;
  Include({
      rect.x - outset,
      rect.y - outset,
      rect.width + outset * 2.0F,
      rect.height + outset * 2.0F,
  });
}

void PaintContext::DrawShadow(
    Rect rect, Color color, Point offset, float blur_radius, float spread, CornerRadii corner_radii
) {
  RequireOpen();
  RequireRect(rect);
  RequireColor(color);
  if (!IsFinite(offset)) {
    throw std::invalid_argument("HuxerUI paint shadow offset must be finite");
  }
  RequireNonNegative(blur_radius, "HuxerUI paint shadow blur radius must be finite and non-negative");
  if (!std::isfinite(spread)) {
    throw std::invalid_argument("HuxerUI paint shadow spread must be finite");
  }
  RequireCornerRadii(corner_radii);
  if (!corner_radii.IsUniform()) {
    const Rect shadow_rect{
        rect.x - spread,
        rect.y - spread,
        std::max(0.0F, rect.width + spread * 2.0F),
        std::max(0.0F, rect.height + spread * 2.0F),
    };
    corner_radii.top_left = std::max(0.0F, corner_radii.top_left + spread);
    corner_radii.top_right = std::max(0.0F, corner_radii.top_right + spread);
    corner_radii.bottom_right = std::max(0.0F, corner_radii.bottom_right + spread);
    corner_radii.bottom_left = std::max(0.0F, corner_radii.bottom_left + spread);
    DrawPathShadow(Path::RoundedRect(shadow_rect, corner_radii), color, offset, blur_radius);
    return;
  }
  const DrawShadowCommand command{rect, color, offset, blur_radius, spread, corner_radii.top_left};
  sequence_.commands_.emplace_back(command);
  const detail::ResolvedShadow resolved = detail::ResolveShadow(command);
  if (!resolved.IsEmpty()) {
    Include(resolved.bounds);
  }
}

void PaintContext::FillPath(Path path, Color color, PathFillRule fill_rule) {
  RequireOpen();
  RequireColor(color);
  sequence_.commands_.emplace_back(FillPathCommand{std::move(path), color, fill_rule});
  const auto& command = std::get<FillPathCommand>(sequence_.commands_.back());
  if (!command.path.IsEmpty()) {
    Include(command.path.Bounds());
  }
}

void PaintContext::StrokePath(Path path, Color color, StrokeStyle style) {
  RequireOpen();
  RequireColor(color);
  style = NormalizeStrokeStyle(std::move(style));
  sequence_.commands_.emplace_back(
      StrokePathCommand{
          std::move(path),
          color,
          std::move(style),
      }
  );
  const auto& command = std::get<StrokePathCommand>(sequence_.commands_.back());
  if (command.path.IsEmpty() || command.style.width <= 0.0F) {
    return;
  }
  const Rect bounds = command.path.Bounds();
  float outset_multiplier = 1.0F;
  if (command.style.join == StrokeJoin::Miter) {
    outset_multiplier = std::max(outset_multiplier, command.style.miter_limit);
  }
  if (command.style.cap == StrokeCap::Square) {
    outset_multiplier = std::max(outset_multiplier, 1.41421356237F);
  }
  const float outset = command.style.width * 0.5F * outset_multiplier;
  Include({
      bounds.x - outset,
      bounds.y - outset,
      bounds.width + outset * 2.0F,
      bounds.height + outset * 2.0F,
  });
}

void PaintContext::DrawPathShadow(Path path, Color color, Point offset, float blur_radius, PathFillRule fill_rule) {
  RequireOpen();
  RequireColor(color);
  if (!IsFinite(offset)) {
    throw std::invalid_argument("HuxerUI path shadow offset must be finite");
  }
  RequireNonNegative(blur_radius, "HuxerUI path shadow blur radius must be finite and non-negative");
  sequence_.commands_.emplace_back(
      DrawPathShadowCommand{
          std::move(path),
          color,
          offset,
          blur_radius,
          fill_rule,
      }
  );
  const auto& command = std::get<DrawPathShadowCommand>(sequence_.commands_.back());
  if (command.path.IsEmpty()) {
    return;
  }
  const Rect bounds = command.path.Bounds();
  Include({
      bounds.x + offset.x - blur_radius,
      bounds.y + offset.y - blur_radius,
      bounds.width + blur_radius * 2.0F,
      bounds.height + blur_radius * 2.0F,
  });
}

void PaintContext::PushClip(Rect rect, CornerRadii corner_radii) {
  RequireOpen();
  RequireRect(rect);
  RequireCornerRadii(corner_radii);
  corner_radii = detail::NormalizeCornerRadii(rect, corner_radii);
  if (!corner_radii.IsUniform()) {
    PushPathClip(Path::RoundedRect(rect, corner_radii));
    return;
  }
  sequence_.commands_.emplace_back(PushClipCommand{rect, corner_radii.top_left});
  clip_stack_.push_back(clip_);
  command_stack_.push_back(StackEntry::Clip);
  const Rect transformed = detail::TransformBounds(transform_, rect);
  clip_ = clip_.has_value() ? std::optional<Rect>{clip_->Intersection(transformed)} : transformed;
}

void PaintContext::PushPathClip(Path path, PathFillRule fill_rule) {
  RequireOpen();
  const Rect bounds = path.Bounds();
  sequence_.commands_.emplace_back(PushPathClipCommand{std::move(path), fill_rule});
  clip_stack_.push_back(clip_);
  command_stack_.push_back(StackEntry::Clip);
  const Rect transformed = detail::TransformBounds(transform_, bounds);
  clip_ = clip_.has_value() ? std::optional<Rect>{clip_->Intersection(transformed)} : transformed;
}

void PaintContext::PopClip() {
  RequireOpen();
  if (clip_stack_.empty() || command_stack_.empty() || command_stack_.back() != StackEntry::Clip) {
    throw std::logic_error("HuxerUI paint clip stack is unbalanced");
  }
  sequence_.commands_.emplace_back(PopClipCommand{});
  clip_ = clip_stack_.back();
  clip_stack_.pop_back();
  command_stack_.pop_back();
}

void PaintContext::PushTransform(Transform2D transform) {
  RequireOpen();
  if (!IsFinite(transform)) {
    throw std::invalid_argument("HuxerUI paint transform must be finite");
  }
  sequence_.commands_.emplace_back(PushTransformCommand{transform});
  transform_stack_.push_back(transform_);
  command_stack_.push_back(StackEntry::Transform);
  transform_ = detail::ComposeTransform(transform_, transform);
}

void PaintContext::PopTransform() {
  RequireOpen();
  if (transform_stack_.empty() || command_stack_.empty() || command_stack_.back() != StackEntry::Transform) {
    throw std::logic_error("HuxerUI paint transform stack is unbalanced");
  }
  sequence_.commands_.emplace_back(PopTransformCommand{});
  transform_ = transform_stack_.back();
  transform_stack_.pop_back();
  command_stack_.pop_back();
}

void PaintContext::Finish() {
  RequireOpen();
  if (!command_stack_.empty()) {
    throw std::logic_error("HuxerUI paint command stack is unbalanced");
  }
  ++sequence_.revision_;
  finished_ = true;
}

void PaintContext::Include(Rect rect) noexcept {
  rect = detail::TransformBounds(transform_, rect);
  if (clip_.has_value()) {
    rect = rect.Intersection(*clip_);
  }
  if (rect.IsEmpty()) {
    return;
  }
  if (sequence_.bounds_.IsEmpty()) {
    sequence_.bounds_ = rect;
    return;
  }
  const float left = std::min(sequence_.bounds_.x, rect.x);
  const float top = std::min(sequence_.bounds_.y, rect.y);
  const float right = std::max(sequence_.bounds_.x + sequence_.bounds_.width, rect.x + rect.width);
  const float bottom = std::max(sequence_.bounds_.y + sequence_.bounds_.height, rect.y + rect.height);
  sequence_.bounds_ = {
      left,
      top,
      right - left,
      bottom - top,
  };
}

void PaintContext::RequireOpen() const {
  if (finished_) {
    throw std::logic_error("HuxerUI paint context is already finished");
  }
}

} // namespace huxerui
