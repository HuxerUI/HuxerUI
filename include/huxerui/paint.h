#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/geometry.h>

namespace huxerui {

enum class TextAlign {
  Leading,
  Center,
};

enum class StrokeCap {
  Butt,
  Round,
  Square,
};

struct DrawRectCommand {
  Rect rect;
  Color color;
  float corner_radius = 0.0F;

  bool operator==(const DrawRectCommand&) const = default;
};

struct DrawTextCommand {
  Rect rect;
  std::string text;
  Color color;
  float font_size = 15.0F;
  TextAlign align = TextAlign::Leading;

  bool operator==(const DrawTextCommand&) const = default;
};

struct DrawCircleCommand {
  Point center;
  float radius = 0.0F;
  Color color;

  bool operator==(const DrawCircleCommand&) const = default;
};

struct DrawArcCommand {
  Point center;
  float radius = 0.0F;
  float start_angle = 0.0F;
  float sweep_angle = 0.0F;
  Color color;
  float width = 1.0F;
  StrokeCap cap = StrokeCap::Butt;

  bool operator==(const DrawArcCommand&) const = default;
};

struct DrawBorderCommand {
  Rect rect;
  Color color;
  float width = 1.0F;
  float corner_radius = 0.0F;

  bool operator==(const DrawBorderCommand&) const = default;
};

struct DrawShadowCommand {
  Rect rect;
  Color color;
  Point offset;
  float blur_radius = 0.0F;
  float spread = 0.0F;
  float corner_radius = 0.0F;

  bool operator==(const DrawShadowCommand&) const = default;
};

struct PushClipCommand {
  Rect rect;
  float corner_radius = 0.0F;

  bool operator==(const PushClipCommand&) const = default;
};

struct PopClipCommand {
  bool operator==(const PopClipCommand&) const = default;
};

struct PushTransformCommand {
  Transform2D transform;

  bool operator==(const PushTransformCommand&) const = default;
};

struct PopTransformCommand {
  bool operator==(const PopTransformCommand&) const = default;
};

using PaintCommand = std::variant<
    DrawRectCommand,
    DrawTextCommand,
    DrawCircleCommand,
    DrawArcCommand,
    DrawBorderCommand,
    DrawShadowCommand,
    PushClipCommand,
    PopClipCommand,
    PushTransformCommand,
    PopTransformCommand>;

class PaintContext;

class PaintSequence {
public:
  [[nodiscard]] const std::vector<PaintCommand>& Commands() const noexcept {
    return commands_;
  }

  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  [[nodiscard]] std::uint64_t Revision() const noexcept {
    return revision_;
  }

private:
  std::vector<PaintCommand> commands_;
  Rect bounds_;
  std::uint64_t revision_ = 0;

  friend class PaintContext;
};

class PaintContext {
public:
  PaintContext(PaintSequence& sequence, Rect bounds);

  PaintContext(const PaintContext&) = delete;
  PaintContext& operator=(const PaintContext&) = delete;
  PaintContext(PaintContext&&) = delete;
  PaintContext& operator=(PaintContext&&) = delete;

  [[nodiscard]] Rect Bounds() const noexcept {
    return bounds_;
  }

  void DrawRect(Rect rect, Color color, float corner_radius = 0.0F);
  void DrawText(Rect rect, std::string text, Color color, float font_size, TextAlign align = TextAlign::Leading);
  void DrawCircle(Point center, float radius, Color color);
  // Arc angles are expressed in radians.
  void DrawArc(
      Point center,
      float radius,
      float start_angle,
      float sweep_angle,
      Color color,
      float width,
      StrokeCap cap = StrokeCap::Butt
  );
  void DrawBorder(Rect rect, Color color, float width, float corner_radius = 0.0F);
  // blur_radius is the outer falloff extent around the spread shadow shape; spread may contract the caster.
  void DrawShadow(
      Rect rect,
      Color color,
      Point offset,
      float blur_radius,
      float spread = 0.0F,
      float corner_radius = 0.0F
  );
  void PushClip(Rect rect, float corner_radius = 0.0F);
  void PopClip();
  void PushTransform(Transform2D transform);
  void PopTransform();
  void Finish();

private:
  enum class StackEntry {
    Clip,
    Transform,
  };

  void Include(Rect rect) noexcept;
  void RequireOpen() const;

  PaintSequence& sequence_;
  Rect bounds_;
  Transform2D transform_;
  std::optional<Rect> clip_;
  std::vector<Transform2D> transform_stack_;
  std::vector<std::optional<Rect>> clip_stack_;
  std::vector<StackEntry> command_stack_;
  bool finished_ = false;
};

} // namespace huxerui
