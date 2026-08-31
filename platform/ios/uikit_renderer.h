#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include <huxerui/geometry.h>
#include <huxerui/render_scene.h>
#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;

class UIKitRenderer final {
public:
  UIKitRenderer();
  ~UIKitRenderer();

  UIKitRenderer(const UIKitRenderer&) = delete;
  UIKitRenderer& operator=(const UIKitRenderer&) = delete;

  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {});
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {});
  [[nodiscard]] std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  );
  void DrawSlice(
      CGContextRef context,
      CGRect dirty_rect,
      const RenderFrame* frame,
      std::size_t first_command,
      std::size_t command_count,
      bool draw_background
  );

private:
  struct State;
  struct CommandRange;

  void RenderSequence(const PaintSequence& sequence, CGContextRef context, CommandRange* range);
  void RenderSceneNode(const RenderNode& node, CGContextRef context, CommandRange* range);
  void RenderCommand(CGContextRef context, const DrawRectCommand& command);
  void RenderCommand(CGContextRef context, const DrawLinearGradientCommand& command);
  void RenderCommand(CGContextRef context, const DrawRadialGradientCommand& command);
  void RenderCommand(CGContextRef context, const DrawTextCommand& command);
  void RenderCommand(CGContextRef context, const DrawTextRunsCommand& command);
  void RenderCommand(CGContextRef context, const DrawImageCommand& command);
  void RenderCommand(CGContextRef context, const DrawExternalTextureCommand& command);
  void RenderCommand(CGContextRef context, const DrawCircleCommand& command);
  void RenderCommand(CGContextRef context, const DrawLineCommand& command);
  void RenderCommand(CGContextRef context, const DrawArcCommand& command);
  void RenderCommand(CGContextRef context, const DrawBorderCommand& command);
  void RenderCommand(CGContextRef context, const DrawShadowCommand& command);
  void RenderCommand(CGContextRef context, const FillPathCommand& command);
  void RenderCommand(CGContextRef context, const FillLinearGradientPathCommand& command);
  void RenderCommand(CGContextRef context, const FillRadialGradientPathCommand& command);
  void RenderCommand(CGContextRef context, const StrokePathCommand& command);
  void RenderCommand(CGContextRef context, const StrokeLinearGradientPathCommand& command);
  void RenderCommand(CGContextRef context, const StrokeRadialGradientPathCommand& command);
  void RenderCommand(CGContextRef context, const DrawPathShadowCommand& command);
  void RenderCommand(CGContextRef context, const PushClipCommand& command);
  void RenderCommand(CGContextRef context, const PushPathClipCommand& command);
  void RenderCommand(CGContextRef context, const PopClipCommand& command);
  void RenderCommand(CGContextRef context, const PushTransformCommand& command);
  void RenderCommand(CGContextRef context, const PopTransformCommand& command);
  void RenderCommand(CGContextRef context, const PlacePlatformViewCommand& command);

  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
