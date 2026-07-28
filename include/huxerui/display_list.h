#pragma once

#include <string>
#include <utility>
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
};

struct DrawTextCommand {
  Rect rect;
  std::string text;
  Color color;
  float font_size = 15.0F;
  TextAlign align = TextAlign::Leading;
};

struct DrawCircleCommand {
  Point center;
  float radius = 0.0F;
  Color color;
};

struct DrawArcCommand {
  Point center;
  float radius = 0.0F;
  float start_angle = 0.0F;
  float sweep_angle = 0.0F;
  Color color;
  float width = 1.0F;
  StrokeCap cap = StrokeCap::Butt;
};

struct DrawBorderCommand {
  Rect rect;
  Color color;
  float width = 1.0F;
  float corner_radius = 0.0F;
};

struct PushClipCommand {
  Rect rect;
  float corner_radius = 0.0F;
};

struct PopClipCommand {};

using DrawCommand = std::variant<
    DrawRectCommand,
    DrawTextCommand,
    DrawCircleCommand,
    DrawArcCommand,
    DrawBorderCommand,
    PushClipCommand,
    PopClipCommand>;

class DisplayList {
public:
  void Clear() {
    commands_.clear();
  }

  void DrawRect(Rect rect, Color color, float corner_radius = 0.0F) {
    commands_.emplace_back(DrawRectCommand{rect, color, corner_radius});
  }

  void DrawText(
      Rect rect,
      std::string text,
      Color color,
      float font_size,
      TextAlign align = TextAlign::Leading) {
    commands_.emplace_back(DrawTextCommand{rect, std::move(text), color, font_size, align});
  }

  void DrawCircle(Point center, float radius, Color color) {
    commands_.emplace_back(
        DrawCircleCommand{center, radius, color});
  }

  void DrawArc(
      Point center, float radius, float start_angle,
      float sweep_angle, Color color, float width,
      StrokeCap cap = StrokeCap::Butt) {
    commands_.emplace_back(DrawArcCommand{
        center,
        radius,
        start_angle,
        sweep_angle,
        color,
        width,
        cap,
    });
  }

  void DrawBorder(
      Rect rect, Color color, float width,
      float corner_radius = 0.0F) {
    commands_.emplace_back(
        DrawBorderCommand{rect, color, width, corner_radius});
  }

  void PushClip(Rect rect, float corner_radius = 0.0F) {
    commands_.emplace_back(
        PushClipCommand{rect, corner_radius});
  }

  void PopClip() {
    commands_.emplace_back(PopClipCommand{});
  }

  [[nodiscard]] const std::vector<DrawCommand>& Commands() const noexcept {
    return commands_;
  }

private:
  std::vector<DrawCommand> commands_;
};

}  // namespace huxerui
