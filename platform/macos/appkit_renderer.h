#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <memory>
#include <string_view>

#include <huxerui/geometry.h>
#include <huxerui/render_scene.h>

namespace huxerui::detail {

class TextLayout;

class AppKitRenderer final {
public:
  [[nodiscard]] Size MeasureText(std::string_view text, float font_size, float max_width);
  [[nodiscard]] std::unique_ptr<TextLayout> CreateTextLayout(std::string_view text, float font_size, float max_width);
  void Draw(CGContextRef context, CGRect dirty_rect, const RenderFrame* frame);

private:
  void RenderSequence(const PaintSequence& sequence, CGContextRef context);
  void RenderSceneNode(const RenderNode& node, CGContextRef context);
  void RenderCommand(CGContextRef context, const DrawRectCommand& command);
  void RenderCommand(CGContextRef context, const DrawTextCommand& command);
  void RenderCommand(CGContextRef context, const DrawCircleCommand& command);
  void RenderCommand(CGContextRef context, const DrawArcCommand& command);
  void RenderCommand(CGContextRef context, const DrawBorderCommand& command);
  void RenderCommand(CGContextRef context, const DrawShadowCommand& command);
  void RenderCommand(CGContextRef context, const PushClipCommand& command);
  void RenderCommand(CGContextRef context, const PopClipCommand& command);
  void RenderCommand(CGContextRef context, const PushTransformCommand& command);
  void RenderCommand(CGContextRef context, const PopTransformCommand& command);
};

} // namespace huxerui::detail
