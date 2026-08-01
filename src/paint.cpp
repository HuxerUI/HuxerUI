#include <huxerui/paint.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "geometry_internal.h"
#include "shadow_internal.h"

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

} // namespace

PaintContext::PaintContext(PaintSequence& sequence, Rect bounds) : sequence_(sequence), bounds_(bounds) {
  RequireRect(bounds);
  sequence_.commands_.clear();
  sequence_.bounds_ = {};
}

void PaintContext::DrawRect(Rect rect, Color color, float corner_radius) {
  RequireOpen();
  RequireRect(rect);
  RequireColor(color);
  RequireNonNegative(corner_radius, "HuxerUI paint corner radius must be finite and non-negative");
  sequence_.commands_.emplace_back(DrawRectCommand{rect, color, corner_radius});
  Include(rect);
}

void PaintContext::DrawText(Rect rect, std::string text, TextStyle style, TextLayoutOptions options) {
  RequireOpen();
  RequireRect(rect);
  RequireTextStyle(style);
  sequence_.commands_.emplace_back(DrawTextCommand{rect, std::move(text), std::move(style), std::move(options)});
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

void PaintContext::DrawArc(
    Point center, float radius, float start_angle, float sweep_angle, Color color, float width, StrokeCap cap
) {
  RequireOpen();
  if (!IsFinite(center)) {
    throw std::invalid_argument("HuxerUI paint arc center must be finite");
  }
  RequireNonNegative(radius, "HuxerUI paint arc radius must be finite and non-negative");
  if (!std::isfinite(start_angle) || !std::isfinite(sweep_angle)) {
    throw std::invalid_argument("HuxerUI paint arc angles must be finite radians");
  }
  RequireColor(color);
  RequireNonNegative(width, "HuxerUI paint arc width must be finite and non-negative");
  sequence_.commands_.emplace_back(
      DrawArcCommand{
          center,
          radius,
          start_angle,
          sweep_angle,
          color,
          width,
          cap,
      }
  );
  const float cap_outset =
      cap == StrokeCap::Square ? std::max(0.0F, width) * 0.70710678118F : std::max(0.0F, width) * 0.5F;
  const float extent = std::max(0.0F, radius) + cap_outset;
  Include({
      center.x - extent,
      center.y - extent,
      extent * 2.0F,
      extent * 2.0F,
  });
}

void PaintContext::DrawBorder(Rect rect, Color color, float width, float corner_radius) {
  RequireOpen();
  RequireRect(rect);
  RequireColor(color);
  RequireNonNegative(width, "HuxerUI paint border width must be finite and non-negative");
  RequireNonNegative(corner_radius, "HuxerUI paint corner radius must be finite and non-negative");
  sequence_.commands_.emplace_back(DrawBorderCommand{rect, color, width, corner_radius});
  const float outset = std::max(0.0F, width) * 0.5F;
  Include({
      rect.x - outset,
      rect.y - outset,
      rect.width + outset * 2.0F,
      rect.height + outset * 2.0F,
  });
}

void PaintContext::DrawShadow(
    Rect rect, Color color, Point offset, float blur_radius, float spread, float corner_radius
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
  RequireNonNegative(corner_radius, "HuxerUI paint corner radius must be finite and non-negative");
  const DrawShadowCommand command{rect, color, offset, blur_radius, spread, corner_radius};
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

void PaintContext::StrokePath(Path path, Color color, float width, StrokeCap cap, StrokeJoin join, float miter_limit) {
  RequireOpen();
  RequireColor(color);
  RequireNonNegative(width, "HuxerUI path stroke width must be finite and non-negative");
  if (!std::isfinite(miter_limit) || miter_limit <= 0.0F) {
    throw std::invalid_argument("HuxerUI path stroke miter limit must be finite and greater than zero");
  }
  sequence_.commands_.emplace_back(
      StrokePathCommand{
          std::move(path),
          color,
          width,
          cap,
          join,
          miter_limit,
      }
  );
  const auto& command = std::get<StrokePathCommand>(sequence_.commands_.back());
  if (command.path.IsEmpty() || width <= 0.0F) {
    return;
  }
  const Rect bounds = command.path.Bounds();
  float outset_multiplier = 1.0F;
  if (join == StrokeJoin::Miter) {
    outset_multiplier = std::max(outset_multiplier, miter_limit);
  }
  if (cap == StrokeCap::Square) {
    outset_multiplier = std::max(outset_multiplier, 1.41421356237F);
  }
  const float outset = width * 0.5F * outset_multiplier;
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

void PaintContext::PushClip(Rect rect, float corner_radius) {
  RequireOpen();
  RequireRect(rect);
  RequireNonNegative(corner_radius, "HuxerUI paint clip corner radius must be finite and non-negative");
  sequence_.commands_.emplace_back(PushClipCommand{rect, corner_radius});
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
